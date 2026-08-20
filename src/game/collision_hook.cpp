#include "../include/game/collision_hook.h"
#include "../include/core/logger.h"
#include "../include/core/memory.h"
#include "../include/utils/utilities.h"
#include "../include/input/input_core.h"
#include "../3rdparty/minhook/include/MinHook.h"
#include "../include/game/practice_patch.h"
#include "../include/game/game_state.h"
#include "../include/core/globals.h"
#include <windows.h>
#include <atomic>
#include <array>
#include <cstring>
#include <string>
#include <sstream>
#include <iomanip>
#include "../include/core/constants.h"
#include "../include/gui/overlay.h"
#include "../include/utils/minhook_utils.h"

// Offset of handlePlayerToPlayerCollision relative to module base
static constexpr uintptr_t HANDLE_P2P_COLLISION_OFFSET = 0x367F60;
// Offset of processProjectileCollision (entity attack -> player) relative to
// the 0x400000 image base.  Retail VA: 0x7697D0.
static constexpr uintptr_t HANDLE_ENTITY_TO_PLAYER_COLLISION_OFFSET = 0x3697D0;

// Original function pointer typedef and storage
using tHandleP2PCollision = void(__thiscall*)(void* gameSystem, int attackerPtr, int defenderPtr, int attackerFrameData, const void* defenderFrameData);
static tHandleP2PCollision oHandleP2PCollision = nullptr;
using tHandleEntityToPlayerCollision = void(__thiscall*)(
    void* gameSystem, int ownerPtr, int defenderPtr, int entitySlot,
    const void* defenderFrameData);
static tHandleEntityToPlayerCollision oHandleEntityToPlayerCollision = nullptr;

// Caches for last seen pointers and discovered offsets per player
static std::atomic<uintptr_t> g_lastAttackDataP1{0};
static std::atomic<uintptr_t> g_lastAttackDataP2{0};
static std::atomic<int> g_attackDataOffsetP1{-1};
static std::atomic<int> g_attackDataOffsetP2{-1};
static std::atomic<bool> s_collisionHookCreated{false};
static std::atomic<bool> s_collisionHookEnabled{false};
static uintptr_t s_collisionHookTargetAddr = 0;
static std::atomic<bool> s_entityCollisionHookCreated{false};
static std::atomic<bool> s_entityCollisionHookEnabled{false};
static uintptr_t s_entityCollisionHookTargetAddr = 0;

static Mission::Contact::Journal<128> s_contactJournal;

namespace {
bool ResolveCollisionHookTarget(uintptr_t& targetAddr) {
    uintptr_t base = GetEFZBase();
    if (!base) {
        LogOut("[COLLISION_HOOK] Failed to get game base address.", true);
        return false;
    }

    targetAddr = base + HANDLE_P2P_COLLISION_OFFSET;
    return true;
}

bool ResolveEntityCollisionHookTarget(uintptr_t& targetAddr) {
    uintptr_t base = GetEFZBase();
    if (!base) {
        LogOut("[COLLISION_HOOK] Failed to get game base address for entity contact.", true);
        return false;
    }

    targetAddr = base + HANDLE_ENTITY_TO_PLAYER_COLLISION_OFFSET;
    return true;
}

bool ValidateCollisionHookTarget(uintptr_t targetAddr) {
    // Eternal Fighter Zero 1.02e retail prologue at 0x767F60:
    //   push ebp; mov ebp,esp; sub esp,0x3B4
    static const unsigned char expected[] = {
        0x55, 0x8B, 0xEC, 0x81, 0xEC, 0xB4, 0x03, 0x00, 0x00
    };
    unsigned char actual[sizeof(expected)] = {};
    if (!SafeReadMemory(targetAddr, actual, sizeof(actual))) return false;
    for (size_t i = 0; i < sizeof(expected); ++i) {
        if (actual[i] != expected[i]) return false;
    }
    return true;
}

bool ValidateEntityCollisionHookTarget(uintptr_t targetAddr) {
    // Eternal Fighter Zero 1.02e retail prologue at 0x7697D0:
    //   push ebp; mov ebp,esp; sub esp,0x23C; mov [ebp-0x1BC],ecx
    // The function ends in `ret 0x10`: four stack arguments after ECX.
    static const unsigned char expected[] = {
        0x55, 0x8B, 0xEC, 0x81, 0xEC, 0x3C, 0x02, 0x00, 0x00,
        0x89, 0x8D, 0x44, 0xFE, 0xFF, 0xFF
    };
    unsigned char actual[sizeof(expected)] = {};
    if (!SafeReadMemory(targetAddr, actual, sizeof(actual))) return false;
    for (size_t i = 0; i < sizeof(expected); ++i) {
        if (actual[i] != expected[i]) return false;
    }
    return true;
}

struct ContactState {
    short move = 0;
    short frame = 0;
    short attackTimer = 0;
    int hitState = 0;
    short combo = 0;              // player+0x174 is a 16-bit counter
    int hp = 0;
};

bool ReadContactStateChecked(uintptr_t player, ContactState& s) {
    if (!player) return false;

    // Keep the resolver hook cheap without broad player-object reads.  These
    // are three established, narrow spans: move/frame, HP, and the adjacent
    // hit-state/attack-timer/combo fields.  Reading the padding inside the
    // 0x168..0x175 resolver span is preferable to six independent
    // IsBadReadPtr probes on every pre/post sample.
    constexpr std::size_t kMoveSpanBytes =
        (CURRENT_FRAME_INDEX_OFFSET + sizeof(short)) - MOVE_ID_OFFSET;
    constexpr std::size_t kResolverSpanBytes =
        (PLAYER_COMBO_COUNTER_OFFSET + sizeof(short)) - PLAYER_HIT_STATE_OFFSET;
    std::array<uint8_t, kMoveSpanBytes> moveSpan{};
    std::array<uint8_t, kResolverSpanBytes> resolverSpan{};

    const bool moveOk = SafeReadMemory(
        player + MOVE_ID_OFFSET, moveSpan.data(), moveSpan.size());
    const bool hpOk = SafeReadMemory(player + HP_OFFSET, &s.hp, sizeof(s.hp));
    const bool resolverOk = SafeReadMemory(
        player + PLAYER_HIT_STATE_OFFSET,
        resolverSpan.data(), resolverSpan.size());

    if (moveOk) {
        std::memcpy(&s.move, moveSpan.data(), sizeof(s.move));
        std::memcpy(&s.frame,
                    moveSpan.data() + CURRENT_FRAME_INDEX_OFFSET - MOVE_ID_OFFSET,
                    sizeof(s.frame));
    }
    if (resolverOk) {
        std::memcpy(&s.hitState, resolverSpan.data(), sizeof(s.hitState));
        std::memcpy(&s.attackTimer,
                    resolverSpan.data() + PLAYER_ATTACK_TIMER_OFFSET -
                        PLAYER_HIT_STATE_OFFSET,
                    sizeof(s.attackTimer));
        std::memcpy(&s.combo,
                    resolverSpan.data() + PLAYER_COMBO_COUNTER_OFFSET -
                        PLAYER_HIT_STATE_OFFSET,
                    sizeof(s.combo));
    }
    return moveOk && hpOk && resolverOk;
}

ContactState ReadContactState(uintptr_t player) {
    ContactState s;
    (void)ReadContactStateChecked(player, s);
    return s;
}

struct EntityContactState {
    uint16_t pattern = 0;
    uint16_t frame = 0;
    int rawState = 0;
    int16_t collisionLife = 0;
};

constexpr uintptr_t kEntityEntryBase = 0x4D0;
constexpr uintptr_t kEntityEntryStride = 0x98;
constexpr uintptr_t kEntityRawStateOffset = 0x80;
constexpr uintptr_t kEntityCollisionLifeOffset = 0x84;

bool ReadEntityContactState(uintptr_t owner, int slot, EntityContactState& s) {
    if (!owner || slot < 0 || slot >= 64) return false;
    const uintptr_t entity = owner + kEntityEntryBase +
        kEntityEntryStride * static_cast<uintptr_t>(slot);

    std::array<uint8_t, 4> animationSpan{};
    constexpr std::size_t kCollisionSpanBytes =
        (kEntityCollisionLifeOffset + sizeof(int16_t)) - kEntityRawStateOffset;
    std::array<uint8_t, kCollisionSpanBytes> collisionSpan{};
    const bool animationOk = SafeReadMemory(
        entity, animationSpan.data(), animationSpan.size());
    const bool collisionOk = SafeReadMemory(
        entity + kEntityRawStateOffset,
        collisionSpan.data(), collisionSpan.size());
    if (animationOk) {
        std::memcpy(&s.pattern, animationSpan.data(), sizeof(s.pattern));
        std::memcpy(&s.frame, animationSpan.data() + sizeof(s.pattern),
                    sizeof(s.frame));
    }
    if (collisionOk) {
        std::memcpy(&s.rawState, collisionSpan.data(), sizeof(s.rawState));
        std::memcpy(&s.collisionLife,
                    collisionSpan.data() + kEntityCollisionLifeOffset -
                        kEntityRawStateOffset,
                    sizeof(s.collisionLife));
    }
    return animationOk && collisionOk;
}

struct PlayerPointerSnapshot {
    uintptr_t p1 = 0;
    uintptr_t p2 = 0;
};

bool ReadPlayerPointers(PlayerPointerSnapshot& out) {
    static_assert(EFZ_BASE_OFFSET_P2 == EFZ_BASE_OFFSET_P1 + sizeof(uint32_t),
                  "EFZ player pointers must remain adjacent 32-bit slots");
    const uintptr_t base = GetEFZBase();
    if (!base) return false;
    uint32_t raw[2] = {};
    if (!SafeReadMemory(base + EFZ_BASE_OFFSET_P1, raw, sizeof(raw))) {
        return false;
    }
    out.p1 = static_cast<uintptr_t>(raw[0]);
    out.p2 = static_cast<uintptr_t>(raw[1]);
    return true;
}

int PlayerNumberForPointer(uintptr_t player,
                           const PlayerPointerSnapshot& players) {
    if (!player) return 0;
    if (player == players.p1) return 1;
    if (player == players.p2) return 2;
    return 0;
}

struct CollisionBranchFlags {
    uint16_t attackFlags = 0;
    uint16_t defenderAttackFlags = 0;
    uint32_t defenderFrameFlags = 0;
};

bool ReadCollisionBranchFlags(const void* attackerFrameData,
                              const void* defenderFrameData,
                              CollisionBranchFlags& out) {
    if (!attackerFrameData || !defenderFrameData) return false;
    constexpr uintptr_t kAttackFlagsOffset = 170;
    constexpr uintptr_t kFrameFlagsOffset = 176;
    constexpr std::size_t kDefenderSpanBytes =
        kFrameFlagsOffset + sizeof(uint32_t) - kAttackFlagsOffset;
    std::array<uint8_t, kDefenderSpanBytes> defenderSpan{};
    const bool attackerOk = SafeReadMemory(
        reinterpret_cast<uintptr_t>(attackerFrameData) + kAttackFlagsOffset,
        &out.attackFlags, sizeof(out.attackFlags));
    const bool defenderOk = SafeReadMemory(
        reinterpret_cast<uintptr_t>(defenderFrameData) + kAttackFlagsOffset,
        defenderSpan.data(), defenderSpan.size());
    if (defenderOk) {
        std::memcpy(&out.defenderAttackFlags, defenderSpan.data(),
                    sizeof(out.defenderAttackFlags));
        std::memcpy(&out.defenderFrameFlags,
                    defenderSpan.data() + kFrameFlagsOffset - kAttackFlagsOffset,
                    sizeof(out.defenderFrameFlags));
    }
    return attackerOk && defenderOk;
}

bool ExactThrowBranch(const CollisionBranchFlags& flags) {
    if (flags.attackFlags & 0x100) return false;
    return ((flags.defenderFrameFlags & 0x1000) && (flags.attackFlags & 1)) ||
           ((flags.defenderFrameFlags & 0x0800) && (flags.attackFlags & 2));
}

bool ExactGuardPointBranch(const CollisionBranchFlags& flags,
                           bool defenderAirborne) {
    if (!(flags.defenderFrameFlags & 0x200)) return false;
    if ((flags.defenderAttackFlags & flags.attackFlags & 1) != 0) return true;
    if ((flags.defenderAttackFlags & flags.attackFlags & 2) != 0) return true;
    return defenderAirborne &&
           (flags.defenderAttackFlags & flags.attackFlags & 4) != 0;
}

} // namespace

// Identify which player owns this frame-data by scanning both player bases for a matching field.
static void IdentifyPlayerByFrameData(uintptr_t frameDataPtr,
                                      const PlayerPointerSnapshot& players,
                                      int& outPlayerNum, int& outOffset) {
    outPlayerNum = 0; outOffset = -1;
    if (!frameDataPtr) return;
    // scan first 0x600 bytes at 4-byte alignment
    auto scan = [&](uintptr_t playerBase) -> int {
        if (!playerBase) return -1;
    for (int off = 0; off <= 0x1200 - 4; off += 4) {
            uintptr_t candidate = 0;
            if (!SafeReadMemory(playerBase + off, &candidate, sizeof(candidate))) continue;
            if (candidate == frameDataPtr) return off;
        }
        return -1;
    };
    int off1 = scan(players.p1);
    if (off1 >= 0) { outPlayerNum = 1; outOffset = off1; return; }
    int off2 = scan(players.p2);
    if (off2 >= 0) { outPlayerNum = 2; outOffset = off2; return; }
}

// We use __fastcall wrapper to intercept __thiscall
static void __fastcall HookedHandleP2PCollision(void* gameSystem, void* /*edx*/, int attackerPtr, int defenderPtr, int attackerFrameData, const void* defenderFrameData) {
    if (!s_collisionHookEnabled.load(std::memory_order_acquire)
        || g_onlineModeActive.load(std::memory_order_relaxed)) {
        oHandleP2PCollision(gameSystem, attackerPtr, defenderPtr, attackerFrameData, defenderFrameData);
        return;
    }

    const uintptr_t attacker = static_cast<uintptr_t>(attackerPtr);
    const uintptr_t defender = static_cast<uintptr_t>(defenderPtr);
    PlayerPointerSnapshot players;
    (void)ReadPlayerPointers(players);
    const int attackerPlayer = PlayerNumberForPointer(attacker, players);
    const int defenderPlayer = PlayerNumberForPointer(defender, players);
    const ContactState beforeAttacker = ReadContactState(attacker);
    const ContactState beforeDefender = ReadContactState(defender);
    double defenderY = 0.0;
    if (defender) {
        (void)SafeReadMemory(defender + YPOS_OFFSET, &defenderY,
                             sizeof(defenderY));
    }
    CollisionBranchFlags branchFlags;
    const bool branchFlagsValid = ReadCollisionBranchFlags(
        reinterpret_cast<const void*>(attackerFrameData), defenderFrameData,
        branchFlags);
    const bool throwBranch = branchFlagsValid && ExactThrowBranch(branchFlags);
    const bool guardPointBranch = branchFlagsValid &&
        ExactGuardPointBranch(branchFlags, defenderY < 0.0);

    // Cache last seen frame-data pointer unconditionally; AttackReader will resolve nested attack-data.
    if (attackerPtr && attackerFrameData) {
        uintptr_t frameData = (uintptr_t)attackerFrameData;
        // Sanity range check, skip caching if not a plausible pointer, but DO NOT early-return
        if (frameData >= 0x00400000 && frameData <= 0x0FFFFFFF) {
            int playerNum = attackerPlayer;
            int fdOff = playerNum == 1 ? g_attackDataOffsetP1.load()
                      : playerNum == 2 ? g_attackDataOffsetP2.load() : -1;
            // The function receives the owning player pointer directly. Only
            // scan while discovering the structural frame-data offset, rather
            // than scanning two 0x1200-byte objects on every collision call.
            if (fdOff < 0) {
                IdentifyPlayerByFrameData(
                    frameData, players, playerNum, fdOff);
            }
            if (playerNum == 1) {
                uintptr_t prev = g_lastAttackDataP1.exchange(frameData);
                if (frameData && frameData != prev &&
                    detailedLogging.load(std::memory_order_relaxed)) {
                    LogOut(std::string("[COLLISION_HOOK] P1 frameData=") + FormatHexAddress(frameData), true);
                }
                if (fdOff >= 0 && g_attackDataOffsetP1.load() < 0) {
                    g_attackDataOffsetP1.store(fdOff);
                    LogOut("[COLLISION_HOOK] Discovered frameData offset P1: " + std::to_string(fdOff), true);
                }
            } else if (playerNum == 2) {
                uintptr_t prev = g_lastAttackDataP2.exchange(frameData);
                if (frameData && frameData != prev &&
                    detailedLogging.load(std::memory_order_relaxed)) {
                    LogOut(std::string("[COLLISION_HOOK] P2 frameData=") + FormatHexAddress(frameData), true);
                }
                if (fdOff >= 0 && g_attackDataOffsetP2.load() < 0) {
                    g_attackDataOffsetP2.store(fdOff);
                    LogOut("[COLLISION_HOOK] Discovered frameData offset P2: " + std::to_string(fdOff), true);
                }
            }
        }
    }

    oHandleP2PCollision(gameSystem, attackerPtr, defenderPtr, attackerFrameData, defenderFrameData);

    if (attackerPlayer == 0 || defenderPlayer == 0) return;
    const ContactState afterAttacker = ReadContactState(attacker);
    const ContactState afterDefender = ReadContactState(defender);
    const bool timerConsumed = afterAttacker.attackTimer < beforeAttacker.attackTimer;
    const bool stateResolved = afterAttacker.hitState != beforeAttacker.hitState;
    const bool comboIncreased = afterAttacker.combo > beforeAttacker.combo;
    const bool comboRestarted = Mission::Contact::CorroboratesComboRestart(
        beforeAttacker.combo, afterAttacker.combo,
        beforeDefender.hp, afterDefender.hp,
        IsHitstun(afterDefender.move) || IsLaunched(afterDefender.move),
        afterAttacker.hitState);
    Mission::Contact::DirectEvidence evidence;
    evidence.resolved = timerConsumed || (stateResolved &&
        (comboIncreased || afterDefender.hp < beforeDefender.hp ||
         afterDefender.move != beforeDefender.move));
    evidence.comboIncreased = comboIncreased;
    evidence.comboRestarted = comboRestarted;
    evidence.defenderRecoilGuard = IsRecoilGuard(afterDefender.move);
    evidence.defenderBlocked = IsBlockstunState(afterDefender.move) &&
                               !evidence.defenderRecoilGuard;
    evidence.exactThrowBranch = throwBranch && afterAttacker.hitState == 6;
    evidence.exactGuardPointBranch = guardPointBranch && afterAttacker.hitState == 2 &&
                                     !evidence.defenderBlocked &&
                                     !evidence.defenderRecoilGuard;
    evidence.rawAttackerState = afterAttacker.hitState;
    const Mission::Contact::Result result = Mission::Contact::ClassifyDirect(evidence);
    if (result == Mission::Contact::Result::None) return;

    Mission::Contact::Event event;
    event.batchId = GetCurrentBattleUpdateBatch();
    event.attacker = static_cast<uint8_t>(attackerPlayer);
    event.defender = static_cast<uint8_t>(defenderPlayer);
    event.source = Mission::Contact::Source::DirectPlayer;
    event.result = result;
    event.attackerMove = beforeAttacker.move;
    event.attackerFrame = beforeAttacker.frame;
    event.defenderMoveBefore = beforeDefender.move;
    event.defenderMove = afterDefender.move;
    event.timerBefore = beforeAttacker.attackTimer;
    event.timerAfter = afterAttacker.attackTimer;
    event.rawStateBefore = beforeAttacker.hitState;
    event.rawStateAfter = afterAttacker.hitState;
    event.comboBefore = beforeAttacker.combo;
    event.comboAfter = afterAttacker.combo;
    event.defenderHpBefore = beforeDefender.hp;
    event.defenderHpAfter = afterDefender.hp;
    s_contactJournal.Publish(event);
}

// 0x7697D0 is __thiscall(gameSystem, owner, defender, slot,
// defenderFrameData).  Both retail call sites pass a live slot from one
// player's 64-entry ring and the opposing player's copied 200-byte frame
// record.  Capture only bounded POD state around the exact resolver; no
// logging, allocation, notation lookup, or ring scan belongs in this hook.
static void __fastcall HookedHandleEntityToPlayerCollision(
    void* gameSystem, void* /*edx*/, int ownerPtr, int defenderPtr,
    int entitySlot, const void* defenderFrameData) {
    if (!s_entityCollisionHookEnabled.load(std::memory_order_acquire) ||
        g_onlineModeActive.load(std::memory_order_relaxed)) {
        oHandleEntityToPlayerCollision(
            gameSystem, ownerPtr, defenderPtr, entitySlot, defenderFrameData);
        return;
    }

    const uintptr_t owner = static_cast<uintptr_t>(ownerPtr);
    const uintptr_t defender = static_cast<uintptr_t>(defenderPtr);
    PlayerPointerSnapshot players;
    (void)ReadPlayerPointers(players);
    const int ownerPlayer = PlayerNumberForPointer(owner, players);
    const int defenderPlayer = PlayerNumberForPointer(defender, players);

    EntityContactState beforeEntity;
    ContactState beforeOwner;
    ContactState beforeDefender;
    const bool beforeValid = ownerPlayer != 0 && defenderPlayer != 0 &&
        ownerPlayer != defenderPlayer &&
        ReadEntityContactState(owner, entitySlot, beforeEntity) &&
        ReadContactStateChecked(owner, beforeOwner) &&
        ReadContactStateChecked(defender, beforeDefender);

    oHandleEntityToPlayerCollision(
        gameSystem, ownerPtr, defenderPtr, entitySlot, defenderFrameData);

    if (!beforeValid) return;

    EntityContactState afterEntity;
    ContactState afterOwner;
    ContactState afterDefender;
    if (!ReadEntityContactState(owner, entitySlot, afterEntity) ||
        !ReadContactStateChecked(owner, afterOwner) ||
        !ReadContactStateChecked(defender, afterDefender)) {
        return;
    }

    // Entry +0x80 must be clear for 0x7697D0 to test collision.  Once inside
    // this exact call, any of these mutations is resolver-local proof that a
    // contact branch committed.  They do not, by themselves, identify which
    // branch: Guard Point/armor/guard-break overlap the raw values.
    const bool entityStateChanged =
        afterEntity.rawState != beforeEntity.rawState;
    const bool lifeConsumed =
        afterEntity.collisionLife < beforeEntity.collisionLife;
    const bool comboChanged = afterOwner.combo != beforeOwner.combo;
    const bool hpChanged = afterDefender.hp != beforeDefender.hp;
    const bool defenderReactionChanged =
        afterDefender.move != beforeDefender.move;
    const bool playerLatchChanged =
        afterOwner.hitState != beforeOwner.hitState ||
        afterDefender.hitState != beforeDefender.hitState;

    Mission::Contact::EntityEvidence evidence;
    evidence.resolved = beforeEntity.rawState == 0 &&
        (entityStateChanged || lifeConsumed || comboChanged || hpChanged ||
         defenderReactionChanged || playerLatchChanged);
    evidence.comboIncreased = afterOwner.combo > beforeOwner.combo;
    evidence.comboRestarted = Mission::Contact::CorroboratesComboRestart(
        beforeOwner.combo, afterOwner.combo,
        beforeDefender.hp, afterDefender.hp,
        IsHitstun(afterDefender.move) || IsLaunched(afterDefender.move),
        afterEntity.rawState);
    evidence.defenderRecoilGuard = IsRecoilGuard(afterDefender.move);
    evidence.defenderBlocked = IsBlockstunState(afterDefender.move) &&
                               !evidence.defenderRecoilGuard;
    evidence.rawEntityState = afterEntity.rawState;
    const Mission::Contact::Result result =
        Mission::Contact::ClassifyEntity(evidence);
    if (result == Mission::Contact::Result::None) return;

    Mission::Contact::Event event;
    event.batchId = GetCurrentBattleUpdateBatch();
    event.attacker = static_cast<uint8_t>(ownerPlayer);
    event.defender = static_cast<uint8_t>(defenderPlayer);
    event.source = Mission::Contact::Source::Entity;
    event.result = result;
    // Entity contact predicates use the entity pattern namespace.  Keep the
    // duplicated explicit field so diagnostics do not need source-dependent
    // interpretation of attackerMove.
    event.attackerMove = static_cast<short>(beforeEntity.pattern);
    event.attackerFrame = static_cast<short>(beforeEntity.frame);
    event.defenderMoveBefore = beforeDefender.move;
    event.defenderMove = afterDefender.move;
    event.timerBefore = beforeEntity.collisionLife;
    event.timerAfter = afterEntity.collisionLife;
    event.rawStateBefore = beforeEntity.rawState;
    event.rawStateAfter = afterEntity.rawState;
    event.comboBefore = beforeOwner.combo;
    event.comboAfter = afterOwner.combo;
    event.defenderHpBefore = beforeDefender.hp;
    event.defenderHpAfter = afterDefender.hp;
    event.entitySlot = entitySlot;
    event.entityPattern = beforeEntity.pattern;
    s_contactJournal.Publish(event);
}

void InstallCollisionHook() {
    uintptr_t targetAddr = 0;
    if (!ResolveCollisionHookTarget(targetAddr)) {
        return;
    }
    s_collisionHookTargetAddr = targetAddr;

    if (!s_collisionHookCreated.load(std::memory_order_acquire)) {
        // Validate the PRISTINE retail prologue ONLY before the first install —
        // i.e. before MinHook overwrites 0x767F60 with its own E9 trampoline JMP.
        // InstallCollisionHook is re-entered on every online->offline transition
        // (ExitNetplaySuspend), and the soft SetCollisionHookActive(false) used on
        // suspend leaves the trampoline in place (it only flips an atomic flag,
        // never MH_DisableHook). So on re-install the bytes ARE our own E9 stub;
        // re-validating here would fail at byte 0 and permanently kill the
        // re-enable. Same class of bug as the frontend signature gate — see
        // reference_revival_prehook_battle_signature.
        if (!ValidateCollisionHookTarget(targetAddr)) {
            LogOut("[COLLISION_HOOK] Refusing strict contact hook: 0x767F60 prologue mismatch", true);
            return;
        }
        if (!MinHookUtils::CreateAndEnableHook(reinterpret_cast<LPVOID>(targetAddr),
                                               reinterpret_cast<void*>(&HookedHandleP2PCollision),
                                               reinterpret_cast<void**>(&oHandleP2PCollision),
                                               "[COLLISION_HOOK]",
                                               "handleP2PCollision")) {
            return;
        }
        s_collisionHookCreated.store(true, std::memory_order_release);
        LogOut("[COLLISION_HOOK] Installed at " + FormatHexAddress(targetAddr), true);
    }

    uintptr_t entityTargetAddr = 0;
    if (ResolveEntityCollisionHookTarget(entityTargetAddr)) {
        s_entityCollisionHookTargetAddr = entityTargetAddr;
        // Same first-install-only validation caveat as the direct hook above:
        // after creation the prologue is our trampoline, so re-validating on a
        // re-install would spuriously log a mismatch. Only check before creating.
        if (!s_entityCollisionHookCreated.load(std::memory_order_acquire)) {
            if (!ValidateEntityCollisionHookTarget(entityTargetAddr)) {
                LogOut("[COLLISION_HOOK] Entity contact unavailable: 0x7697D0 prologue mismatch", true);
            } else if (MinHookUtils::CreateAndEnableHook(
                    reinterpret_cast<LPVOID>(entityTargetAddr),
                    reinterpret_cast<void*>(&HookedHandleEntityToPlayerCollision),
                    reinterpret_cast<void**>(&oHandleEntityToPlayerCollision),
                    "[COLLISION_HOOK]", "handleEntityToPlayerCollision")) {
                s_entityCollisionHookCreated.store(true, std::memory_order_release);
                LogOut("[COLLISION_HOOK] Entity contact installed at " +
                       FormatHexAddress(entityTargetAddr), true);
            }
        }
    }

    if (g_onlineModeActive.load(std::memory_order_relaxed)) {
        SetCollisionHookActive(false);
        return;
    }

    SetCollisionHookActive(true);
}

void SetCollisionHookActive(bool active) {
    if (!s_collisionHookCreated.load(std::memory_order_acquire)) {
        if (active) {
            InstallCollisionHook();
        }
        return;
    }

    const bool entityActive = active &&
        s_entityCollisionHookCreated.load(std::memory_order_acquire);
    const bool directChanged =
        s_collisionHookEnabled.exchange(active, std::memory_order_acq_rel) != active;
    const bool entityChanged =
        s_entityCollisionHookEnabled.exchange(entityActive, std::memory_order_acq_rel) !=
        entityActive;
    if (!directChanged && !entityChanged) {
        return;
    }

    LogOut(std::string("[COLLISION_HOOK] Collision hooks ") +
           (active ? "enabled" : "disabled"), true);
}

void RemoveCollisionHook() {
    if (s_entityCollisionHookTargetAddr &&
        s_entityCollisionHookCreated.load(std::memory_order_acquire)) {
        (void)MinHookUtils::DisableHook(
            (LPVOID)s_entityCollisionHookTargetAddr, "[COLLISION_HOOK]",
            "handleEntityToPlayerCollision");
        (void)MinHookUtils::RemoveHook(
            (LPVOID)s_entityCollisionHookTargetAddr, "[COLLISION_HOOK]",
            "handleEntityToPlayerCollision");
    }
    s_entityCollisionHookEnabled.store(false, std::memory_order_release);
    s_entityCollisionHookCreated.store(false, std::memory_order_release);
    s_entityCollisionHookTargetAddr = 0;
    oHandleEntityToPlayerCollision = nullptr;

    if (s_collisionHookTargetAddr &&
        s_collisionHookCreated.load(std::memory_order_acquire)) {
        (void)MinHookUtils::DisableHook((LPVOID)s_collisionHookTargetAddr,
                                       "[COLLISION_HOOK]", "handleP2PCollision");
        (void)MinHookUtils::RemoveHook((LPVOID)s_collisionHookTargetAddr,
                                      "[COLLISION_HOOK]", "handleP2PCollision");
    }
    s_collisionHookEnabled.store(false, std::memory_order_release);
    s_collisionHookCreated.store(false, std::memory_order_release);
    s_collisionHookTargetAddr = 0;
    oHandleP2PCollision = nullptr;
}

uintptr_t GetCachedAttackDataForPlayer(int playerNum) {
    return (playerNum == 1) ? g_lastAttackDataP1.load() : g_lastAttackDataP2.load();
}

int GetAttackDataOffsetForPlayer(int playerNum) {
    return (playerNum == 1) ? g_attackDataOffsetP1.load() : g_attackDataOffsetP2.load();
}

void ResetCollisionHookSessionCaches(const char* reason) {
    const uintptr_t lastP1 = g_lastAttackDataP1.exchange(0);
    const uintptr_t lastP2 = g_lastAttackDataP2.exchange(0);

    if (lastP1 || lastP2 || detailedLogging.load()) {
        std::ostringstream oss;
        oss << "[COLLISION_HOOK] Reset session caches"
            << " reason=" << (reason ? reason : "unspecified")
            << " P1=" << FormatHexAddress(lastP1)
            << " P2=" << FormatHexAddress(lastP2)
            << " offsets=" << g_attackDataOffsetP1.load() << "/" << g_attackDataOffsetP2.load();
        LogOut(oss.str(), true);
    }
    ResetContactEventJournal(reason);
}

bool IsDirectContactHookReady() {
    return s_collisionHookCreated.load(std::memory_order_acquire) &&
           s_collisionHookEnabled.load(std::memory_order_acquire) &&
           !g_onlineModeActive.load(std::memory_order_relaxed);
}

bool IsEntityContactHookReady() {
    return s_entityCollisionHookCreated.load(std::memory_order_acquire) &&
           s_entityCollisionHookEnabled.load(std::memory_order_acquire) &&
           !g_onlineModeActive.load(std::memory_order_relaxed);
}

uint32_t GetContactEventEpoch() { return s_contactJournal.Epoch(); }
uint32_t GetContactEventWatermark() { return s_contactJournal.Watermark(); }

void ResetContactEventJournal(const char* reason) {
    const uint32_t epoch = s_contactJournal.ResetEpoch();
    if (detailedLogging.load()) {
        LogOut("[CONTACT] journal epoch=" + std::to_string(epoch) +
               " reason=" + (reason ? reason : "unspecified"), true);
    }
}

Mission::Contact::ReadResult ReadCommittedContactEvents(
    uint32_t afterSequence, uint32_t expectedEpoch, uint32_t closedBatch,
    Mission::Contact::Event* out, size_t outCapacity) {
    return s_contactJournal.ReadAfter(afterSequence, expectedEpoch, closedBatch,
                                      out, outCapacity);
}
