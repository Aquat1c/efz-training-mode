#include "../include/input/input_hook.h"
#include "../include/input/input_motion.h"
#include "../include/core/memory.h"
#include "../include/core/logger.h"
#include "../include/core/constants.h"
#include "../include/utils/utilities.h"

#include "../include/input/input_core.h"

#include "../3rdparty/minhook/include/MinHook.h"
#include "../include/game/practice_patch.h"
#include "../include/game/game_state.h"
#include "../include/input/input_buffer.h" // for g_bufferFreezingActive
#include "../include/input/immediate_input.h"
#include "../include/utils/minhook_utils.h"
#include "../include/game/auto_action.h"
#include "../include/game/macro_controller.h"
#include "../include/input/injection_control.h"
#include <windows.h>
#include <vector>
#include <atomic>
#include <array>
#include <cstring>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <mutex>
#include "../include/input/immediate_input.h"

#ifndef EFZ_ENABLE_AUTO_ACTION_WAKE_TRACE
#define EFZ_ENABLE_AUTO_ACTION_WAKE_TRACE 0
#endif

static InputHookPolicy::TutorialP2InputSource TutorialP2Source();

// Local helper to format a single byte as two-digit hex (uppercase)
static std::string FormatHexByte(uint8_t value) {
    std::ostringstream oss;
    oss << std::uppercase << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(value);
    return oss.str();
}

// Static variable to track the previous frame's button mask for edge detection.
static uint8_t g_lastInjectedMask[3] = {0, 0, 0}; // Index 0 unused, 1 for P1, 2 for P2
// Track whether we were bypassing original (buffered injection) last frame per player.
static bool g_wasBypassBuffered[3] = { false, false, false };

// Global control to force authoritative buffered injection (macro playback, etc.)
std::atomic<bool> g_forceBypass[3] = { false, false, false };

// Poll override: when active, our poll hook returns this mask instead of device state (index 0 unused).
std::atomic<bool> g_pollOverrideActive[3] = { false, false, false };
std::atomic<uint8_t> g_pollOverrideMask[3] = { 0, 0, 0 };
static std::atomic<uint32_t> g_pollOverrideHitCount[3] = { 0, 0, 0 };
static std::atomic<uint8_t> g_lastPolledMask[3] = { 0, 0, 0 };
static std::atomic<uint8_t> g_pendingPollAttackEdges[3] = { 0, 0, 0 };
static std::atomic<uint32_t> g_inputPollSerial[3] = { 0, 0, 0 };
static std::atomic<bool> g_observePhysicalPoll[3] = { false, false, false };
static std::atomic<uint8_t> g_lastObservedPhysicalPoll[3] = { 0xFF, 0xFF, 0xFF };

#if EFZ_ENABLE_AUTO_ACTION_WAKE_TRACE
static const char* TraceBool(bool value) {
    return value ? "1" : "0";
}

static const char* TraceSubframeSuffix(int internalFrame) {
    static const char* kSuffixes[3] = { "00", "33", "66" };
    int subframe = internalFrame % 3;
    if (subframe < 0) subframe += 3;
    return kSuffixes[subframe];
}

static const char* TraceMoveClass(short moveID) {
    switch (moveID) {
        case IDLE_MOVE_ID: return "idle";
        case WALK_FWD_ID: return "walk-f";
        case WALK_BACK_ID: return "walk-b";
        case CROUCH_ID: return "crouch";
        case CROUCH_TO_STAND_ID: return "crouch-stand";
        case GROUNDTECH_PRE: return "gtech-pre";
        case GROUNDTECH_RECOVERY: return "gtech-96";
        case GROUNDTECH_START: return "gtech-start";
        case GROUNDTECH_END: return "gtech-end";
        case STRAIGHT_JUMP_ID: return "jump-n";
        case FORWARD_JUMP_ID: return "jump-f";
        case BACKWARD_JUMP_ID: return "jump-b";
        case FALLING_ID: return "fall";
        case FORWARD_DASH_START_ID: return "dash-f-start";
        case FORWARD_DASH_RECOVERY_ID: return "dash-f-rec";
        case FORWARD_DASH_RECOVERY_SENTINEL_ID: return "dash-f-sentinel";
        case BACKWARD_DASH_START_ID: return "dash-b-start";
        case BACKWARD_DASH_RECOVERY_ID: return "dash-b-rec";
        case KAORI_FORWARD_DASH_START_ID: return "kaori-dash-f";
        case STAND_GUARD_ID: return "stand-guard";
        case CROUCH_GUARD_ID: return "crouch-guard";
        case AIR_GUARD_ID: return "air-guard";
        case RG_STAND_ID: return "rg-stand";
        case RG_CROUCH_ID: return "rg-crouch";
        case RG_AIR_ID: return "rg-air";
        default: break;
    }
    if (IsHitstun(moveID)) return "hitstun";
    if (IsLaunched(moveID)) return "launch";
    if (IsAirtech(moveID)) return "airtech";
    if (IsThrown(moveID)) return "throw";
    if (IsFrozen(moveID)) return "frozen";
    if (IsAttackMove(moveID)) return "attack";
    if (IsBlockstun(moveID)) return "blockstun";
    if (IsActionable(moveID)) return "actionable";
    return "other";
}

static short TraceReadMoveIDFromPtr(uintptr_t playerPtr) {
    short moveID = -1;
    if (playerPtr) {
        (void)SafeReadMemory(playerPtr + MOVE_ID_OFFSET, &moveID, sizeof(moveID));
    }
    return moveID;
}

static bool TraceInputHookWakeWindowActive(short p1MoveID, short p2MoveID) {
    static int s_traceWindow[3] = { 0, 0, 0 };
    static int s_lastUpdatedFrame = -1;
    const int now = frameCounter.load();

    if (s_lastUpdatedFrame != now) {
        s_lastUpdatedFrame = now;
        if (IsGroundtech(p1MoveID)) {
            s_traceWindow[1] = 90;
        } else if (s_traceWindow[1] > 0) {
            --s_traceWindow[1];
        }
        if (IsGroundtech(p2MoveID)) {
            s_traceWindow[2] = 90;
        } else if (s_traceWindow[2] > 0) {
            --s_traceWindow[2];
        }
    }

    return s_traceWindow[1] > 0 || s_traceWindow[2] > 0;
}

static void TraceAppendQueueState(std::ostringstream& oss, int playerNum) {
    const bool queueActive = (playerNum == 1) ? p1QueueActive : p2QueueActive;
    const int queueIndex = (playerNum == 1) ? p1QueueIndex : p2QueueIndex;
    const int frameCounterForQueue = (playerNum == 1) ? p1FrameCounter : p2FrameCounter;
    const int motionType = (playerNum == 1) ? p1CurrentMotionType : p2CurrentMotionType;
    const size_t queueSize = (playerNum == 1) ? p1InputQueue.size() : p2InputQueue.size();
    oss << " p" << playerNum << "Queue=" << TraceBool(queueActive)
        << ":" << queueIndex << "/" << queueSize
        << " qFrame=" << frameCounterForQueue
        << " qMotion=" << motionType;
}

static void TraceAppendInputState(std::ostringstream& oss, int playerNum) {
    oss << " p" << playerNum
        << " desired=" << static_cast<int>(ImmediateInput::GetCurrentDesired(playerNum))
        << " remTicks=" << ImmediateInput::GetRemainingTicks(playerNum)
        << " manual=" << TraceBool(g_manualInputOverride[playerNum].load(std::memory_order_relaxed))
        << " manualMask=" << static_cast<int>(g_manualInputMask[playerNum].load(std::memory_order_relaxed))
        << " immediateOnly=" << TraceBool(g_injectImmediateOnly[playerNum].load(std::memory_order_relaxed))
        << " forceBypass=" << TraceBool(g_forceBypass[playerNum].load(std::memory_order_relaxed))
        << " poll=" << TraceBool(g_pollOverrideActive[playerNum].load(std::memory_order_relaxed))
        << " pollMask=" << static_cast<int>(g_pollOverrideMask[playerNum].load(std::memory_order_relaxed))
        << " lastInjected=" << static_cast<int>(g_lastInjectedMask[playerNum]);
    TraceAppendQueueState(oss, playerNum);
}

static void TraceInputHookWakePhase(const char* phase, int playerNum, uintptr_t characterPtr, short entryMoveID, const char* exitPath) {
    const short p1MoveID = static_cast<short>(GetPlayerMoveID(1));
    const short p2MoveID = static_cast<short>(GetPlayerMoveID(2));
    if (!TraceInputHookWakeWindowActive(p1MoveID, p2MoveID)) return;

    const short charMoveID = TraceReadMoveIDFromPtr(characterPtr);
    const int internalFrame = frameCounter.load();
    std::ostringstream oss;
    oss << "[AA_WAKE_TRACE][INPUT_HOOK]"
        << " phase=" << (phase ? phase : "unknown")
        << " IF=" << internalFrame
        << " VF=" << (internalFrame / 3) << "." << TraceSubframeSuffix(internalFrame)
        << " hookP=" << playerNum
        << " entryMove=" << entryMoveID << "(" << TraceMoveClass(entryMoveID) << ")"
        << " charMove=" << charMoveID << "(" << TraceMoveClass(charMoveID) << ")"
        << " p1Move=" << p1MoveID << "(" << TraceMoveClass(p1MoveID) << ")"
        << " p2Move=" << p2MoveID << "(" << TraceMoveClass(p2MoveID) << ")"
        << " exitPath=" << (exitPath ? exitPath : "")
        << " freeze=" << TraceBool(g_bufferFreezingActive.load(std::memory_order_relaxed))
        << " freezeOwner=" << g_activeFreezePlayer.load(std::memory_order_relaxed)
        << " pendingRestore=" << TraceBool(g_pendingControlRestore.load(std::memory_order_relaxed))
        << " p2Override=" << TraceBool(g_p2ControlOverridden);
    TraceAppendInputState(oss, 1);
    TraceAppendInputState(oss, 2);
    LogOut(oss.str(), false);
}

struct InputHookWakeTraceScope {
    InputHookWakeTraceScope(int player, uintptr_t ptr)
        : playerNum(player), characterPtr(ptr), entryMoveID(TraceReadMoveIDFromPtr(ptr)) {
        TraceInputHookWakePhase("entry", playerNum, characterPtr, entryMoveID, exitLabel);
    }

    ~InputHookWakeTraceScope() {
        TraceInputHookWakePhase("exit", playerNum, characterPtr, entryMoveID, exitLabel);
    }

    void Mark(const char* phase) {
        TraceInputHookWakePhase(phase, playerNum, characterPtr, entryMoveID, exitLabel);
    }

    void SetExit(const char* label) {
        exitLabel = label;
    }

    int playerNum;
    uintptr_t characterPtr;
    short entryMoveID;
    const char* exitLabel = "exit";
};
#endif

void ResetInputPollOverrideHitCount(int playerNum) {
    if (playerNum != 1 && playerNum != 2) return;
    g_pollOverrideHitCount[playerNum].store(0, std::memory_order_release);
}

uint32_t GetInputPollOverrideHitCount(int playerNum) {
    if (playerNum != 1 && playerNum != 2) return 0;
    return g_pollOverrideHitCount[playerNum].load(std::memory_order_acquire);
}

uint8_t ConsumeInputPollAttackEdges(int playerNum) {
    if (playerNum != 1 && playerNum != 2) return 0;
    return g_pendingPollAttackEdges[playerNum].exchange(0, std::memory_order_acq_rel);
}

uint32_t GetInputPollSerial(int playerNum) {
    if (playerNum != 1 && playerNum != 2) return 0;
    return g_inputPollSerial[playerNum].load(std::memory_order_acquire);
}

void SetPollOverridePhysicalObservation(int playerNum, bool enabled) {
    if (playerNum != 1 && playerNum != 2) return;
    if (enabled) g_lastObservedPhysicalPoll[playerNum].store(0xFF, std::memory_order_release);
    g_observePhysicalPoll[playerNum].store(enabled, std::memory_order_release);
}

bool IsPollOverridePhysicalObservationEnabled(int playerNum) {
    if (playerNum != 1 && playerNum != 2) return false;
    return g_observePhysicalPoll[playerNum].load(std::memory_order_acquire);
}

uint8_t GetLastObservedPhysicalPollMask(int playerNum) {
    if (playerNum != 1 && playerNum != 2) return 0xFF;
    return g_lastObservedPhysicalPoll[playerNum].load(std::memory_order_acquire);
}

// Arming state for motion-token neutralization and optional staged cleanup
static std::atomic<bool> s_armedNeutralize[3] = { false, false, false };
static std::atomic<bool> s_doFullCleanup[3] = { false, false, false };
static std::atomic<uint16_t> s_lastHead[3] = { 0xFFFF, 0xFFFF, 0xFFFF };
static std::atomic<int> s_headStable[3] = { 0, 0, 0 };

void InputHook_ArmTokenNeutralize(int playerNum, bool alsoDoFullCleanup) {
    if (playerNum != 1 && playerNum != 2) return;
    s_armedNeutralize[playerNum].store(true, std::memory_order_relaxed);
    s_doFullCleanup[playerNum].store(alsoDoFullCleanup, std::memory_order_relaxed);
    s_lastHead[playerNum].store(0xFFFF, std::memory_order_relaxed);
    s_headStable[playerNum].store(0, std::memory_order_relaxed);
    if (detailedLogging.load()) {
        LogOut(std::string("[INPUT_HOOK] Armed token neutralize for P") + std::to_string(playerNum) +
               (alsoDoFullCleanup?" + full cleanup":""), true);
    }
}

// Helper invoked at the end of processing to perform late neutralization/cleanup
static void MaybePerformTailCleanup(int playerNum) {
    if (playerNum != 1 && playerNum != 2) return;
    // Skip during buffer-freeze for this player (let freeze system manage state)
    if (g_bufferFreezingActive.load()) {
        int owner = g_activeFreezePlayer.load();
        if (owner == playerNum || owner == 0) return;
    }
    // One-shot token neutralization
    if (s_armedNeutralize[playerNum].load(std::memory_order_relaxed)) {
        (void)NeutralizeMotionToken(playerNum);
        s_armedNeutralize[playerNum].store(false, std::memory_order_relaxed);
    }
    // Staged cleanup once buffer head is stable for >=2 frames
    if (s_doFullCleanup[playerNum].load(std::memory_order_relaxed)) {
        uintptr_t pPtr = GetPlayerPointer(playerNum);
        if (!pPtr) return;
        uint16_t head = 0;
        if (!SafeReadMemory(pPtr + INPUT_BUFFER_INDEX_OFFSET, &head, sizeof(head))) return;
        uint16_t last = s_lastHead[playerNum].load(std::memory_order_relaxed);
        if (last == 0xFFFF || head != last) {
            s_lastHead[playerNum].store(head, std::memory_order_relaxed);
            s_headStable[playerNum].store(0, std::memory_order_relaxed);
            return;
        }
        int stable = s_headStable[playerNum].fetch_add(1, std::memory_order_relaxed) + 1;
        if (stable >= 2) {
            (void)FullCleanupAfterToggle(playerNum);
            s_doFullCleanup[playerNum].store(false, std::memory_order_relaxed);
            s_lastHead[playerNum].store(0xFFFF, std::memory_order_relaxed);
            s_headStable[playerNum].store(0, std::memory_order_relaxed);
        }
    }
}

// Function pointer for the original game function we are hooking.
typedef int(__thiscall* tProcessCharacterInput)(int characterPtr);
tProcessCharacterInput oProcessCharacterInput = nullptr;

// Universal player state update.  Its vtable+16 call is the exact normal
// consumer, before later per-character updates and collision resolution can
// replace the move with hit/block/throw state.
typedef int(__thiscall* tUpdateEntityState)(int characterPtr);
tUpdateEntityState oUpdateEntityState = nullptr;

// The address of the function to hook, relative to efz.exe's base address.
// This is the entry point for a character's input processing for the frame.
// CORRECTED ADDRESS: 0x411BE0 -> relative offset 0x11BE0
const uintptr_t PROCESS_INPUTS_FUNC_OFFSET = 0x11BE0;
const uintptr_t UPDATE_ENTITY_STATE_FUNC_OFFSET = 0x0F230;

// Hook: pollPlayerInputState(inputManager, playerIndex) → returns 8-bit unified mask
// RVA from efz.exe base: 0x0040CD00 → offset 0x0CD00
typedef int(__thiscall* tPollPlayerInputState)(int inputManagerPtr, unsigned int playerIndex);
static tPollPlayerInputState oPollPlayerInputState = nullptr;
static const uintptr_t POLL_INPUT_STATE_FUNC_OFFSET = 0x0CD00;

namespace {
std::atomic<bool> s_inputHooksCreated{false};
std::atomic<bool> s_inputHooksEnabled{false};
std::atomic<int> s_processInputPlayerContext{0};
std::atomic<bool> s_loggedPollContextReroute[3] = { false, false, false };
std::atomic<bool> s_loggedPollOverrideHumanForce[3] = { false, false, false };
uintptr_t s_processTargetAddr = 0;
uintptr_t s_pollTargetAddr = 0;
uintptr_t s_stateUpdateTargetAddr = 0;

enum class NormalPulseTransport : uint8_t {
    Unselected,
    NativePoll,
    AiPostWrite,
};

struct NormalPulseRuntimeSlot {
    std::recursive_mutex mutex;
    NormalInputPolicy::PulseState state;
    NormalPulseTransport transport{NormalPulseTransport::Unselected};
    uintptr_t gameState{0};
    uintptr_t character{0};
    bool deliveredAnyPhase{false};
    // Keep the raw-register lane reserved through updateEntityState's later
    // character consumer. The consumer hook releases it exactly afterward; the
    // next matching input pass is a fallback if that barrier was skipped.
    bool rawReleaseGuard{false};
    // Input generation and the character's command/normal consumer are two
    // separate stages of EFZ's battle tick for both human and AI control.  A
    // successful poll/post-write only proves that the pulse reached the hand-
    // off registers. Confirm Press after updateEntityState's vtable+16 consumer
    // has had a chance to start the requested action.
    bool pressAwaitingConfirmation{false};
    short pressBeforeMove{-1};
    short pressBeforeFrame{-1};
    uint8_t pressAttempts{0};
    bool retryNeutralPending{false};
    bool abortAfterRetryNeutral{false};
};

NormalPulseRuntimeSlot s_normalPulse[3];
std::atomic<bool> s_normalRawRegisterOwner[3] = { false, false, false };

struct NormalProcessRoute {
    bool active{false};
    bool observed{false};
    bool nativePollObserved{false};
    int player{0};
    uint8_t mask{0};
    bool retryNeutral{false};
    bool abortAfterWrite{false};
    NormalPulseTransport transport{NormalPulseTransport::Unselected};
    NormalInputPolicy::Snapshot snapshot{};
};

// HookedPoll runs synchronously inside the matching original
// ProcessCharacterInput call, so thread-local context is an exact consumption
// acknowledgement and cannot be confused with another player or generation.
thread_local NormalProcessRoute s_nativeNormalPollRoute;

constexpr uintptr_t kNativeHeldButtonOffsets[4] = {400u, 404u, 408u, 412u};
constexpr uintptr_t kReplayIoModeOffset = 82563u;

bool ResolveInputHookTargets(uintptr_t& targetAddr,
                             uintptr_t& pollAddr,
                             uintptr_t& stateUpdateAddr) {
    uintptr_t base = GetEFZBase();
    if (!base) {
        LogOut("[INPUT_HOOK] Failed to get game base address. Hook not installed.", true);
        return false;
    }

    targetAddr = base + PROCESS_INPUTS_FUNC_OFFSET;
    pollAddr = base + POLL_INPUT_STATE_FUNC_OFFSET;
    stateUpdateAddr = base + UPDATE_ENTITY_STATE_FUNC_OFFSET;
    return true;
}

bool ValidateStateUpdateTarget(uintptr_t target) {
    // Retail/Memorial 1.20 updateEntityState @ 0x0040F230.  This critical hook
    // is optional only in the sense that normal injection is refused if the
    // executable does not match; we never detour an unvalidated body.
    constexpr std::array<uint8_t, 12> expected = {
        0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x0C,
        0x89, 0x4D, 0xFC, 0x8B, 0x45, 0xFC,
    };
    std::array<uint8_t, expected.size()> actual{};
    if (!target ||
        !SafeReadMemory(target, actual.data(), actual.size()) ||
        actual != expected) {
        LogOut("[INPUT_HOOK] updateEntityState signature mismatch at " +
               FormatHexAddress(target) +
               "; authoritative normal delivery disabled", true);
        return false;
    }
    return true;
}

} // namespace

// Engine-only input ownership fallback. Revival's Practice SwitchPlayers hotkey swaps
// these same adjacent 16-byte live control maps in EFZ's input-manager object. Keeping
// the swap at this layer preserves P1's physical controls for the P2 character without
// changing character player indices or hijacking unrelated poll call sites.
static std::atomic<bool> g_loggedRoutingStateOnce{false};
namespace {

constexpr uintptr_t kInputManagerObjectRva = 0x003B0968u;
constexpr uintptr_t kP1BindingOffset = 448u;
constexpr size_t kBindingCount = 8u;

using BindingBlock = std::array<uint16_t, kBindingCount>;

struct BindingPair {
    BindingBlock p1{};
    BindingBlock p2{};
};
static_assert(sizeof(BindingPair) == 32, "EFZ control-map pair must remain exactly 32 bytes");

struct BindingSwapState {
    bool active{false};
    BindingPair baseline{};
};

std::mutex g_bindingSwapMutex;
BindingSwapState g_bindingSwapState;

bool BindingPairsEqual(const BindingPair& lhs, const BindingPair& rhs) {
    return std::memcmp(&lhs, &rhs, sizeof(BindingPair)) == 0;
}

BindingPair MakeSwappedBindingPair(const BindingPair& source) {
    BindingPair result{};
    result.p1 = source.p2;
    result.p2 = source.p1;
    return result;
}

uintptr_t GetBindingPairAddress() {
    const uintptr_t base = GetEFZBase();
    return base ? base + kInputManagerObjectRva + kP1BindingOffset : 0;
}

bool ReadBindingPair(BindingPair& outPair) {
    const uintptr_t address = GetBindingPairAddress();
    return address != 0 && SafeReadMemory(address, &outPair, sizeof(outPair));
}

bool WriteAndVerifyBindingPair(const BindingPair& pair) {
    const uintptr_t address = GetBindingPairAddress();
    if (!address || !SafeWriteMemory(address, &pair, sizeof(pair))) {
        return false;
    }

    BindingPair verify{};
    return SafeReadMemory(address, &verify, sizeof(verify))
        && BindingPairsEqual(verify, pair);
}

std::string FormatBindingBlock(const BindingBlock& block) {
    std::ostringstream oss;
    oss << std::hex << std::uppercase << std::setfill('0');
    for (size_t i = 0; i < block.size(); ++i) {
        if (i != 0) oss << ' ';
        oss << std::setw(4) << static_cast<unsigned int>(block[i]);
    }
    return oss.str();
}

struct ScopedProcessInputPlayerContext {
    explicit ScopedProcessInputPlayerContext(int playerNum)
        : previous(s_processInputPlayerContext.load(std::memory_order_relaxed)) {
        s_processInputPlayerContext.store(playerNum, std::memory_order_relaxed);
    }

    ~ScopedProcessInputPlayerContext() {
        s_processInputPlayerContext.store(previous, std::memory_order_relaxed);
    }

    int previous;
};

int CallOriginalProcessCharacterInputWithContext(int characterPtr, int playerNum) {
    ScopedProcessInputPlayerContext context(playerNum);
    return oProcessCharacterInput ? oProcessCharacterInput(characterPtr) : 0;
}

void EnsureHumanControlForActivePollOverride(int characterPtr, int playerNum) {
    if ((playerNum != 1 && playerNum != 2) || characterPtr == 0) {
        return;
    }
    if (!g_pollOverrideActive[playerNum].load(std::memory_order_relaxed)) {
        return;
    }

    uint32_t aiFlag = 0;
    if (!SafeReadMemory(static_cast<uintptr_t>(characterPtr) + AI_CONTROL_FLAG_OFFSET,
                        &aiFlag,
                        sizeof(aiFlag))) {
        return;
    }
    if (aiFlag == 0) {
        return;
    }

    const uint32_t humanControlFlag = 0;
    const bool okWrite = SafeWriteMemory(static_cast<uintptr_t>(characterPtr) + AI_CONTROL_FLAG_OFFSET,
                                         &humanControlFlag,
                                         sizeof(humanControlFlag));
    uint32_t after = aiFlag;
    SafeReadMemory(static_cast<uintptr_t>(characterPtr) + AI_CONTROL_FLAG_OFFSET,
                   &after,
                   sizeof(after));

    if (!s_loggedPollOverrideHumanForce[playerNum].exchange(true, std::memory_order_relaxed)
        || !okWrite
        || after != 0) {
        std::ostringstream oss;
        oss << "[INPUT_HOOK][POLL_OVERRIDE] Forced P" << playerNum
            << " AI flag human for engine poll path @0x" << std::hex
            << (static_cast<uintptr_t>(characterPtr) + AI_CONTROL_FLAG_OFFSET)
            << std::dec
            << " before=" << aiFlag
            << " after=" << after
            << " okWrite=" << (okWrite ? "1" : "0");
        LogOut(oss.str(), true);
    }
}

} // namespace

namespace {

const char* NormalPulsePhaseName(NormalInputPolicy::Phase phase) {
    switch (phase) {
        case NormalInputPolicy::Phase::Idle:           return "idle";
        case NormalInputPolicy::Phase::AwaitingStart:  return "await";
        case NormalInputPolicy::Phase::PreNeutral:     return "pre-neutral";
        case NormalInputPolicy::Phase::Press:          return "press";
        case NormalInputPolicy::Phase::ReleaseNeutral: return "release-neutral";
    }
    return "unknown";
}

uintptr_t ReadLiveGameState() {
    const uintptr_t base = GetEFZBase();
    uintptr_t gameState = 0;
    if (!base || !SafeReadMemory(base + EFZ_BASE_OFFSET_GAME_STATE,
                                 &gameState, sizeof(gameState))) {
        return 0;
    }
    return gameState;
}

const char* NormalPulseTransportName(NormalPulseTransport transport) {
    switch (transport) {
        case NormalPulseTransport::Unselected: return "unselected";
        case NormalPulseTransport::NativePoll: return "native-poll";
        case NormalPulseTransport::AiPostWrite:return "ai-postwrite";
    }
    return "unknown";
}

int NormalButtonIndex(const NormalInputPolicy::Intent& intent) {
    const uint8_t buttons[4] = {
        NormalInputPolicy::kInputA,
        NormalInputPolicy::kInputB,
        NormalInputPolicy::kInputC,
        NormalInputPolicy::kInputD,
    };
    for (int index = 0; index < 4; ++index) {
        if (intent.button == buttons[index]) return index;
    }
    return -1;
}

bool ReadCharacterResourceName(uintptr_t character, char (&name)[16]) {
    std::memset(name, 0, sizeof(name));
    return character != 0 &&
        SafeReadMemory(character + CHARACTER_NAME_OFFSET,
                       name, sizeof(name) - 1);
}

bool NameEquals(const char* name, const char* expected) {
    return name && expected && std::strcmp(name, expected) == 0;
}

bool CharacterSupportsDIntent(uintptr_t character,
                              const NormalInputPolicy::Intent& intent) {
    if (intent.button != NormalInputPolicy::kInputD) return true;

    char name[16] = {};
    if (!ReadCharacterResourceName(character, name)) return false;
    if (intent.airborne) {
        // These consumers have a raw airborne S/D branch. UNKNOWN's resource
        // name is case-sensitive: regular UNKNOWN is "mizukaB" while the boss
        // variant is "mizuka".
        return NameEquals(name, "mio") || NameEquals(name, "ayu") ||
               NameEquals(name, "mizuka") || NameEquals(name, "mizukaB");
    }

    if (NameEquals(name, "mai") &&
        intent.direction == NormalInputPolicy::RelativeDirection::Down) {
        return false; // Mai has ground S, but no 2S branch.
    }
    return NameEquals(name, "shiori") || NameEquals(name, "nayuki") ||
           NameEquals(name, "mizuka") || NameEquals(name, "mizukaB") ||
           NameEquals(name, "mio") || NameEquals(name, "minagi") ||
           NameEquals(name, "mai") || NameEquals(name, "kano");
}

bool DidConsumerStartRequestedNormal(uintptr_t character,
                                     const NormalInputPolicy::Intent& intent,
                                     short beforeMove,
                                     short beforeFrame,
                                     short afterMove,
                                     short afterFrame) {
    const bool actionInstanceChanged = afterMove != beforeMove ||
        (afterFrame >= 0 && beforeFrame >= 0 && afterFrame < beforeFrame);
    if (!actionInstanceChanged) return false;

    // Every retail A/B/C consumer enters an action state >= 200.  Directional
    // movement happens first and remains below 200, so this cannot mistake 6X
    // or 4X walking for a consumed button.
    if (intent.button != NormalInputPolicy::kInputD) {
        return afterMove >= 200;
    }

    char name[16] = {};
    if (!ReadCharacterResourceName(character, name)) return false;
    if (NameEquals(name, "shiori")) return afterMove == 309;
    if (NameEquals(name, "nayuki")) return afterMove == 259 || afterMove == 280;
    if (NameEquals(name, "mizuka") || NameEquals(name, "mizukaB")) {
        return afterMove == 179;
    }
    if (NameEquals(name, "mio")) return afterMove == 270 || afterMove == 17;
    if (NameEquals(name, "minagi")) {
        return afterMove >= 250 && afterMove <= 252;
    }
    if (NameEquals(name, "mai")) return afterMove == 260 || afterMove == 269;
    if (NameEquals(name, "kano")) return afterMove == 250;
    if (NameEquals(name, "ayu")) return afterMove == 40;
    return false;
}

enum class NormalRegisterWriteResult : uint8_t {
    Written,
    ClearedAfterFailure,
    Failed,
};

std::array<uint8_t, 6> EncodeImmediateRegisterBlock(uint8_t mask) {
    const bool up = (mask & GAME_INPUT_UP) != 0;
    const bool down = (mask & GAME_INPUT_DOWN) != 0;
    const bool left = (mask & GAME_INPUT_LEFT) != 0;
    const bool right = (mask & GAME_INPUT_RIGHT) != 0;
    return {
        static_cast<uint8_t>(right ? 1 : (left ? 0xFF : 0)),
        static_cast<uint8_t>(down ? 1 : (up ? 0xFF : 0)),
        static_cast<uint8_t>((mask & GAME_INPUT_A) != 0),
        static_cast<uint8_t>((mask & GAME_INPUT_B) != 0),
        static_cast<uint8_t>((mask & GAME_INPUT_C) != 0),
        static_cast<uint8_t>((mask & GAME_INPUT_D) != 0),
    };
}

bool ClearOwnedNormalRegisters(uintptr_t character) {
    if (!character) return false;
    const uint16_t noMotion = 0x0063;
    const std::array<uint8_t, 2> noCommands = {0, 0};
    const std::array<uint8_t, 6> neutral = {0, 0, 0, 0, 0, 0};
    bool ok = true;
    // Evaluate every write: cleanup is best-effort even if an earlier region
    // fails. These are the complete lanes read by the later normal consumer.
    ok = SafeWriteMemory(character + MOTION_TOKEN_OFFSET,
                         &noMotion, sizeof(noMotion)) && ok;
    ok = SafeWriteMemory(character + COMMAND_BUFFER_OFFSET,
                         noCommands.data(), noCommands.size()) && ok;
    ok = SafeWriteMemory(character + INPUT_HORIZONTAL_OFFSET,
                         neutral.data(), neutral.size()) && ok;
    return ok;
}

NormalRegisterWriteResult WriteOwnedNormalRegisters(uintptr_t character,
                                                     uint8_t mask) {
    if (!character) return NormalRegisterWriteResult::Failed;
    const uint16_t noMotion = 0x0063;
    const std::array<uint8_t, 2> noCommands = {0, 0};
    const auto raw = EncodeImmediateRegisterBlock(mask);

    // Commit each contiguous engine region as a unit and place the raw pulse
    // last. If any region fails, neutralize every owned lane before the later
    // character consumer rather than leaving a prefix-committed mixed input.
    const bool written =
        SafeWriteMemory(character + MOTION_TOKEN_OFFSET,
                        &noMotion, sizeof(noMotion)) &&
        SafeWriteMemory(character + COMMAND_BUFFER_OFFSET,
                        noCommands.data(), noCommands.size()) &&
        SafeWriteMemory(character + INPUT_HORIZONTAL_OFFSET,
                        raw.data(), raw.size());
    if (written) return NormalRegisterWriteResult::Written;
    return ClearOwnedNormalRegisters(character)
        ? NormalRegisterWriteResult::ClearedAfterFailure
        : NormalRegisterWriteResult::Failed;
}

bool ClearNativeNormalHeldLatch(uintptr_t character,
                                const NormalInputPolicy::Intent& intent) {
    const int buttonIndex = NormalButtonIndex(intent);
    if (!character || buttonIndex < 0) return false;
    const uint32_t zero = 0;
    return SafeWriteMemory(character + kNativeHeldButtonOffsets[buttonIndex],
                           &zero, sizeof(zero));
}

void ClearDeliveredNormalState(int playerNum,
                               NormalPulseTransport transport,
                               uintptr_t expectedGameState,
                               uintptr_t expectedCharacter,
                               const NormalInputPolicy::Intent& intent) {
    // Never touch a replacement fighter, or battle memory after online mode
    // has taken ownership. Awaiting requests are cancelled without writes.
    if (g_onlineModeActive.load(std::memory_order_relaxed) ||
        ReadLiveGameState() != expectedGameState ||
        GetPlayerPointer(playerNum) != expectedCharacter) {
        return;
    }

    if (transport != NormalPulseTransport::AiPostWrite &&
        transport != NormalPulseTransport::NativePoll) {
        return;
    }

    // Cancellation owns the complete consumer hand-off, not just the selected
    // button byte. Clear direction, all four pulses, both command latches, and
    // the WORD motion token so no partially written input survives this tick.
    (void)ClearOwnedNormalRegisters(expectedCharacter);
    if (transport == NormalPulseTransport::NativePoll) {
        // Native polling also updates one 32-bit held latch. Only that latch was
        // synthetic; preserve unrelated physical held-button bookkeeping.
        (void)ClearNativeNormalHeldLatch(expectedCharacter, intent);
    }
}

} // namespace

NormalInputPolicy::SubmitResult QueueAutoActionNormalPulse(
    int playerNum,
    const NormalInputPolicy::Intent& intent,
    NormalInputPolicy::Timing timing,
    uint64_t* generationOut) {
    if (generationOut) *generationOut = 0;
    if (playerNum < 1 || playerNum > 2 || !intent ||
        !s_inputHooksEnabled.load(std::memory_order_acquire) ||
        g_onlineModeActive.load(std::memory_order_relaxed) || !IsMatchPhase()) {
        return NormalInputPolicy::SubmitResult::Invalid;
    }

    if (MacroController::IsExclusivePlayback() &&
        MacroController::GetPlaybackPlayer() == playerNum) {
        return NormalInputPolicy::SubmitResult::Busy;
    }

    const uintptr_t gameState = ReadLiveGameState();
    const uintptr_t character = GetPlayerPointer(playerNum);
    if (!gameState || !character) {
        return NormalInputPolicy::SubmitResult::Invalid;
    }
    if (!CharacterSupportsDIntent(character, intent)) {
        LogOut("[AUTO-NORMAL] Rejected unsupported character-specific S/D intent for P" +
               std::to_string(playerNum), detailedLogging.load());
        return NormalInputPolicy::SubmitResult::Invalid;
    }

    uint64_t generation = 0;
    NormalInputPolicy::SubmitResult result;
    {
        NormalPulseRuntimeSlot& slot = s_normalPulse[playerNum];
        std::lock_guard<std::recursive_mutex> lock(slot.mutex);
        if (!s_inputHooksEnabled.load(std::memory_order_acquire) ||
            g_onlineModeActive.load(std::memory_order_relaxed)) {
            return NormalInputPolicy::SubmitResult::Invalid;
        }
        if (slot.state.Active() &&
            (slot.gameState != gameState || slot.character != character)) {
            slot.state.Reset();
            slot.transport = NormalPulseTransport::Unselected;
            slot.gameState = 0;
            slot.character = 0;
            slot.deliveredAnyPhase = false;
            slot.rawReleaseGuard = false;
            slot.pressAwaitingConfirmation = false;
            slot.pressBeforeMove = -1;
            slot.pressBeforeFrame = -1;
            slot.pressAttempts = 0;
            slot.retryNeutralPending = false;
            slot.abortAfterRetryNeutral = false;
            s_normalRawRegisterOwner[playerNum].store(
                false, std::memory_order_release);
        }

        const bool alreadyActive = slot.state.Active();
        result = slot.state.Submit(intent, timing, &generation);
        if (result == NormalInputPolicy::SubmitResult::Accepted &&
            !alreadyActive) {
            slot.transport = NormalPulseTransport::Unselected;
            slot.gameState = gameState;
            slot.character = character;
            slot.deliveredAnyPhase = false;
        }
    }
    if (generationOut) *generationOut = generation;

    if (detailedLogging.load() || result != NormalInputPolicy::SubmitResult::Accepted) {
        LogOut("[AUTO-NORMAL] Queue P" + std::to_string(playerNum) +
               " gen=" + std::to_string(generation) +
               " button=0x" + FormatHexByte(intent.button) +
               " timing=" + (timing == NormalInputPolicy::Timing::Immediate
                                   ? "immediate" : "actionable") +
               " result=" +
               (result == NormalInputPolicy::SubmitResult::Accepted ? "accepted" :
                result == NormalInputPolicy::SubmitResult::Busy ? "busy" : "invalid"),
               true);
    }
    return result;
}

NormalInputPolicy::SubmitResult QueueAutoActionNormalPulse(
    int playerNum,
    int motionType,
    NormalInputPolicy::Timing timing,
    uint64_t* generationOut) {
    return QueueAutoActionNormalPulse(playerNum,
        NormalInputPolicy::IntentFromMotion(motionType), timing, generationOut);
}

bool IsAutoActionNormalPulseActive(int playerNum) {
    if (playerNum < 1 || playerNum > 2) return false;
    std::lock_guard<std::recursive_mutex> lock(s_normalPulse[playerNum].mutex);
    return s_normalPulse[playerNum].state.Active();
}

bool IsAutoActionNormalPulseOwningImmediateRegisters(int playerNum) {
    return playerNum >= 1 && playerNum <= 2 &&
        s_normalRawRegisterOwner[playerNum].load(std::memory_order_acquire);
}

bool TryAcquireImmediateInputWriteLease(int playerNum) {
    if (playerNum < 1 || playerNum > 2) return false;
    NormalPulseRuntimeSlot& slot = s_normalPulse[playerNum];
    if (!slot.mutex.try_lock()) return false;
    if (s_normalRawRegisterOwner[playerNum].load(
            std::memory_order_acquire)) {
        slot.mutex.unlock();
        return false;
    }
    return true;
}

void ReleaseImmediateInputWriteLease(int playerNum) {
    if (playerNum < 1 || playerNum > 2) return;
    s_normalPulse[playerNum].mutex.unlock();
}

void CancelAutoActionNormalPulse(int playerNum) {
    if (playerNum < 1 || playerNum > 2) return;

    NormalPulseRuntimeSlot& slot = s_normalPulse[playerNum];
    std::lock_guard<std::recursive_mutex> lock(slot.mutex);
    const NormalInputPolicy::Snapshot snapshot = slot.state.Current();
    const bool ownsRawLane = s_normalRawRegisterOwner[playerNum].load(
        std::memory_order_acquire);
    const bool hasSelectedTransport = snapshot &&
        slot.transport != NormalPulseTransport::Unselected;

    // Cleanup must happen while the slot mutex and raw-register ownership are
    // still held.  Releasing either first lets the detached ImmediateInput
    // worker acquire the lane and then get overwritten by this cancellation.
    if (hasSelectedTransport && (slot.deliveredAnyPhase || ownsRawLane)) {
        ClearDeliveredNormalState(playerNum, slot.transport, slot.gameState,
                                  slot.character, snapshot.intent);
    }

    // A cancellation can arrive after processCharacterInput returns but before
    // this tick's updateEntityState consumer. Keep competing raw writers out
    // until that consumer barrier if any owned phase was already delivered.
    const bool deferRawRelease = slot.rawReleaseGuard ||
        (hasSelectedTransport && ownsRawLane);

    slot.state.Reset();
    slot.transport = NormalPulseTransport::Unselected;
    if (!deferRawRelease) {
        slot.gameState = 0;
        slot.character = 0;
    }
    slot.deliveredAnyPhase = false;
    slot.rawReleaseGuard = deferRawRelease;
    slot.pressAwaitingConfirmation = false;
    slot.pressBeforeMove = -1;
    slot.pressBeforeFrame = -1;
    slot.pressAttempts = 0;
    slot.retryNeutralPending = false;
    slot.abortAfterRetryNeutral = false;
    if (!deferRawRelease) {
        s_normalRawRegisterOwner[playerNum].store(
            false, std::memory_order_release);
    }
}

void CancelAllAutoActionNormalPulses() {
    CancelAutoActionNormalPulse(1);
    CancelAutoActionNormalPulse(2);
}

namespace {

bool NativeNormalInputGateOpen() {
    if (!IsMatchPhase()) return false;
    const uintptr_t gameState = ReadLiveGameState();
    if (!gameState) return false;

    uint8_t roundEvent = 0;
    uint32_t roundGate = 0;
    uint8_t replayIo = 0;
    return SafeReadMemory(gameState + SYS_FLAG_4944,
                          &roundEvent, sizeof(roundEvent)) &&
           SafeReadMemory(gameState + SYS_FLAG_4948,
                          &roundGate, sizeof(roundGate)) &&
           SafeReadMemory(gameState + kReplayIoModeOffset,
                          &replayIo, sizeof(replayIo)) &&
           roundEvent == 0 && roundGate == 0 && replayIo == 0;
}

bool RequestedNativeNormalButtonHeld(
    uintptr_t character,
    const NormalInputPolicy::Intent& intent) {
    const int index = NormalButtonIndex(intent);
    if (index < 0) return true;
    uint32_t held = 0;
    if (!SafeReadMemory(character + kNativeHeldButtonOffsets[index],
                        &held, sizeof(held))) {
        // A conservative neutral pass is safer than silently losing a repeated
        // same-button edge when the latch cannot be inspected.
        return true;
    }
    return held != 0;
}

bool CurrentPatAllowsAirNormal(uintptr_t character,
                               const NormalInputPolicy::Intent& intent,
                               short moveID) {
    int destinationMove = -1;
    switch (intent.button) {
        case NormalInputPolicy::kInputA: destinationMove = 207; break;
        case NormalInputPolicy::kInputB: destinationMove = 208; break;
        case NormalInputPolicy::kInputC: destinationMove = 209; break;
        default: return false;
    }

    // EFZ decides air-normal availability from the live PAT row, not from a
    // move-ID allowlist.  This is what opens j.X part-way through jump,
    // double-jump, hover, and air-dash animations.  207/208/209 are the
    // cast-wide A/B/C tier anchors used by this rank check; a character may
    // enter a stance-specific move with the same tier. Air-S/D behavior is
    // character-specific and has no fabricated universal anchor.
    constexpr uintptr_t kAnimEntryRankOffset = 2u;
    uint16_t frameIndex = 0;
    uintptr_t animTable = 0;
    uintptr_t frameTable = 0;
    uint16_t frameFlags = 0;
    int16_t currentRank = 0;
    int16_t destinationRank = 0;
    int32_t contactState = 0;
    if (moveID < 0 || moveID > 1023 ||
        !SafeReadMemory(character + CURRENT_FRAME_INDEX_OFFSET,
                        &frameIndex, sizeof(frameIndex)) ||
        frameIndex > 4095 ||
        !SafeReadMemory(character + ANIM_TABLE_OFFSET,
                        &animTable, sizeof(animTable)) ||
        !animTable ||
        !SafeReadMemory(animTable + ANIM_ENTRY_STRIDE *
                            static_cast<uintptr_t>(moveID) +
                            ANIM_ENTRY_FRAMES_PTR_OFFSET,
                        &frameTable, sizeof(frameTable)) ||
        !frameTable ||
        !SafeReadMemory(frameTable + FRAME_BLOCK_STRIDE *
                            static_cast<uintptr_t>(frameIndex) +
                            FRAME_HIT_PROPS_OFFSET,
                        &frameFlags, sizeof(frameFlags)) ||
        !SafeReadMemory(animTable + ANIM_ENTRY_STRIDE *
                            static_cast<uintptr_t>(moveID) +
                            kAnimEntryRankOffset,
                        &currentRank, sizeof(currentRank)) ||
        !SafeReadMemory(animTable + ANIM_ENTRY_STRIDE *
                            static_cast<uintptr_t>(destinationMove),
                        &destinationRank, sizeof(destinationRank)) ||
        !SafeReadMemory(character + PLAYER_HIT_STATE_OFFSET,
                        &contactState, sizeof(contactState))) {
        return false;
    }

    return (frameFlags & 0x0008u) != 0 &&
           currentRank <= destinationRank &&
           (moveID < 200 || (contactState >= 2 && contactState != 6));
}

bool NormalPostureReady(uintptr_t character,
                        const NormalInputPolicy::Snapshot& snapshot) {
    short moveID = -1;
    if (!SafeReadMemory(character + MOVE_ID_OFFSET,
                        &moveID, sizeof(moveID))) {
        return false;
    }
    double y = 0.0;
    if (!SafeReadMemory(character + YPOS_OFFSET, &y, sizeof(y))) {
        return false;
    }
    const bool physicallyAirborne = y < 0.0;

    // Exact-tick dash follow-ups intentionally bypass ground actionability, but
    // must never turn into an air normal after an unexpected launch. Air input
    // always uses the native PAT eligibility check below.
    if (snapshot.timing == NormalInputPolicy::Timing::Immediate &&
        !snapshot.intent.airborne) {
        return !physicallyAirborne;
    }

    const bool landing = moveID == LANDING_ID || moveID == LANDING_1_ID ||
                         moveID == LANDING_2_ID || moveID == LANDING_3_ID;
    const bool groundReady = !physicallyAirborne && IsActionable(moveID) &&
                             moveID != FALLING_ID && !landing;
    const bool universalAirButton =
        snapshot.intent.button != NormalInputPolicy::kInputD;
    const bool airReady = physicallyAirborne &&
        (universalAirButton
             ? CurrentPatAllowsAirNormal(character, snapshot.intent, moveID)
             // There is no cast-wide j.S state. Preserve the previous bounded
             // best-effort behavior from a freely actionable fall; exact
             // consumer confirmation will reject unsupported characters.
             : moveID == FALLING_ID);

    // Posture stays strict so a delayed ground trigger cannot silently become
    // j.X after a launch, nor an air trigger become 5X after landing.
    return NormalInputPolicy::PostureEligible(
        snapshot.intent, groundReady, airReady);
}

bool NormalConsumerGateOpen(int playerNum, uintptr_t character) {
    const int opponentNum = playerNum == 1 ? 2 : 1;
    const uintptr_t opponent = GetPlayerPointer(opponentNum);
    if (!character || !opponent) return false;

    short targetStateTimer = -1;
    short targetSuperFreeze = -1;
    short opponentSuperFreeze = -1;
    return SafeReadMemory(character + BLOCKSTUN_OFFSET,
                          &targetStateTimer, sizeof(targetStateTimer)) &&
           SafeReadMemory(character + SUPERFLASH_FREEZE_OFFSET,
                          &targetSuperFreeze, sizeof(targetSuperFreeze)) &&
           SafeReadMemory(opponent + SUPERFLASH_FREEZE_OFFSET,
                          &opponentSuperFreeze, sizeof(opponentSuperFreeze)) &&
           targetStateTimer == 0 && targetSuperFreeze == 0 &&
           opponentSuperFreeze == 0;
}

bool PredictFacingRightForConsumer(int playerNum, uintptr_t character) {
    const uintptr_t opponent = GetPlayerPointer(playerNum == 1 ? 2 : 1);
    double x = 0.0;
    double opponentX = 0.0;
    if (character && opponent &&
        SafeReadMemory(character + XPOS_OFFSET, &x, sizeof(x)) &&
        SafeReadMemory(opponent + XPOS_OFFSET, &opponentX, sizeof(opponentX))) {
        if (x < opponentX) return true;
        if (x > opponentX) return false;
    }
    // Equal positions preserve the engine's current +0x50 direction.
    return GetPlayerFacingDirection(playerNum);
}

void AbortNormalPulseLocked(NormalPulseRuntimeSlot& slot,
                            int playerNum,
                            const char* reason,
                            bool deferRawRelease = false,
                            bool preserveAcceptedSuccessor = false) {
    const bool keepRawLease = deferRawRelease &&
        s_normalRawRegisterOwner[playerNum].load(std::memory_order_acquire);
    bool successorPromoted = false;
    if (preserveAcceptedSuccessor) {
        successorPromoted = slot.state.RetireCurrent();
    } else {
        slot.state.Reset();
    }
    slot.transport = NormalPulseTransport::Unselected;
    if (!keepRawLease && !successorPromoted) {
        slot.gameState = 0;
        slot.character = 0;
    }
    slot.deliveredAnyPhase = false;
    slot.rawReleaseGuard = keepRawLease;
    slot.pressAwaitingConfirmation = false;
    slot.pressBeforeMove = -1;
    slot.pressBeforeFrame = -1;
    slot.pressAttempts = 0;
    slot.retryNeutralPending = false;
    slot.abortAfterRetryNeutral = false;
    if (!keepRawLease) {
        s_normalRawRegisterOwner[playerNum].store(false,
                                                   std::memory_order_release);
    }
    LogOut("[AUTO-NORMAL] Aborted P" + std::to_string(playerNum) +
           " pulse: " + reason +
           (successorPromoted ? "; accepted successor promoted" : ""), true);
}

bool AbortExpectedImmediateNormal(int playerNum,
                                  const NormalInputPolicy::Snapshot& expected,
                                  uintptr_t expectedGameState,
                                  uintptr_t expectedCharacter,
                                  const char* reason) {
    if (playerNum < 1 || playerNum > 2) return false;
    NormalPulseRuntimeSlot& slot = s_normalPulse[playerNum];
    std::lock_guard<std::recursive_mutex> lock(slot.mutex);
    const auto current = slot.state.Current();
    if (!current || !expected || current.generation != expected.generation ||
        current.timing != NormalInputPolicy::Timing::Immediate ||
        slot.gameState != expectedGameState ||
        slot.character != expectedCharacter) {
        return false;
    }
    ClearDeliveredNormalState(playerNum, slot.transport, slot.gameState,
                              slot.character, current.intent);
    const bool deferRawRelease =
        s_normalRawRegisterOwner[playerNum].load(std::memory_order_acquire);
    AbortNormalPulseLocked(slot, playerNum, reason, deferRawRelease);
    return true;
}

void ReleaseRawGuardAfterConsumer(int playerNum) {
    if (playerNum < 1 || playerNum > 2) return;
    NormalPulseRuntimeSlot& slot = s_normalPulse[playerNum];
    std::lock_guard<std::recursive_mutex> lock(slot.mutex);
    if (!slot.rawReleaseGuard) return;
    slot.rawReleaseGuard = false;
    s_normalRawRegisterOwner[playerNum].store(false,
                                               std::memory_order_release);
    if (!slot.state.Active()) {
        slot.gameState = 0;
        slot.character = 0;
    }
}

void RelinquishPreparedNormalForConflict(
    int playerNum,
    uint64_t expectedGeneration = 0,
    const char* immediateReason = "exact-tick normal conflicted with another input owner",
    bool clearOwnedDelivery = true) {
    if (playerNum < 1 || playerNum > 2) return;
    NormalPulseRuntimeSlot& slot = s_normalPulse[playerNum];
    std::lock_guard<std::recursive_mutex> lock(slot.mutex);
    const auto current = slot.state.Current();
    if (!current ||
        (expectedGeneration != 0 && current.generation != expectedGeneration)) {
        return;
    }
    // Once an Immediate press has been rejected, the next operation is cleanup,
    // not a fresh deferred attack. A regular actionable pulse returns to
    // AwaitingStart so the higher-priority owner is not deadlocked behind our
    // raw-register lease and the normal can retry from a clean edge later.
    if (slot.retryNeutralPending) {
        if (clearOwnedDelivery) {
            ClearDeliveredNormalState(playerNum, slot.transport, slot.gameState,
                                      slot.character, current.intent);
        }
        if (current.timing == NormalInputPolicy::Timing::Immediate) {
            AbortNormalPulseLocked(
                slot, playerNum,
                "exact-tick retry-neutral conflicted with another input owner",
                true);
        } else if (slot.state.RestartAwaiting()) {
            slot.transport = NormalPulseTransport::Unselected;
            slot.deliveredAnyPhase = false;
            slot.rawReleaseGuard = false;
            slot.pressAwaitingConfirmation = false;
            slot.pressBeforeMove = -1;
            slot.pressBeforeFrame = -1;
            slot.pressAttempts = 0;
            slot.retryNeutralPending = false;
            slot.abortAfterRetryNeutral = false;
            s_normalRawRegisterOwner[playerNum].store(
                false, std::memory_order_release);
        }
        return;
    }
    // Immediate is an exact dash tick, not a deferred request. Once another
    // owner wins that fighter pass, clear any delivered raw edge and retire it;
    // restarting Press as AwaitingStart would create a late 66X/662X normal.
    if (current.timing == NormalInputPolicy::Timing::Immediate) {
        if (clearOwnedDelivery) {
            ClearDeliveredNormalState(playerNum, slot.transport, slot.gameState,
                                      slot.character, current.intent);
        }
        const bool deferRawRelease =
            s_normalRawRegisterOwner[playerNum].load(std::memory_order_acquire);
        AbortNormalPulseLocked(slot, playerNum, immediateReason,
                               deferRawRelease);
        return;
    }
    if (current.phase == NormalInputPolicy::Phase::ReleaseNeutral) {
        // Press has already crossed the previous tick's consumer. Remove its
        // residual raw state now and yield; keeping the lease here would stop a
        // tutorial ImmediateInput owner from ever reaching neutral itself.
        if (clearOwnedDelivery) {
            ClearDeliveredNormalState(playerNum, slot.transport, slot.gameState,
                                      slot.character, current.intent);
        }
        AbortNormalPulseLocked(slot, playerNum,
                               "normal release yielded to another input owner",
                               false, true);
        return;
    }
    if ((current.phase != NormalInputPolicy::Phase::Press &&
         current.phase != NormalInputPolicy::Phase::PreNeutral) ||
        !slot.state.RestartAwaiting()) {
        return;
    }
    slot.transport = NormalPulseTransport::Unselected;
    slot.deliveredAnyPhase = false;
    slot.pressAwaitingConfirmation = false;
    slot.pressBeforeMove = -1;
    slot.pressBeforeFrame = -1;
    slot.pressAttempts = 0;
    slot.retryNeutralPending = false;
    slot.abortAfterRetryNeutral = false;
    s_normalRawRegisterOwner[playerNum].store(false,
                                               std::memory_order_release);
}

bool PreparedNormalHasHigherPriorityOwner(int playerNum) {
    if (g_pollOverrideActive[playerNum].load(std::memory_order_relaxed) ||
        (MacroController::IsExclusivePlayback() &&
         MacroController::GetPlaybackPlayer() == playerNum)) {
        return true;
    }
    return playerNum == 2 && TutorialP2ControlLeaseActive() &&
        !InputHookPolicy::TutorialP2AllowsNormalPulse(TutorialP2Source());
}

// Controller mode can change between pulse phases (and, for tutorial leases,
// between preflight and the original producer call). Keep the route paired with
// the producer EFZ will actually execute. A prepared press moving from AI to
// native polling is restarted so native held-edge bookkeeping can be sampled
// correctly; an Immediate dash normal is retired instead of firing late.
bool ReconcileNormalTransport(int playerNum,
                              uintptr_t character,
                              const NormalInputPolicy::Snapshot& expected,
                              NormalPulseTransport& transport) {
    if (!expected || transport == NormalPulseTransport::Unselected) return false;

    uint32_t aiFlag = 0xFFFFFFFFu;
    if (!SafeReadMemory(character + AI_CONTROL_FLAG_OFFSET,
                        &aiFlag, sizeof(aiFlag))) {
        (void)AbortExpectedImmediateNormal(
            playerNum, expected, ReadLiveGameState(), character,
            "exact-tick normal could not revalidate controller mode");
        return false;
    }
    const NormalPulseTransport desired = aiFlag == 0
        ? NormalPulseTransport::NativePoll
        : NormalPulseTransport::AiPostWrite;
    if (desired == transport) return true;

    NormalPulseRuntimeSlot& slot = s_normalPulse[playerNum];
    std::lock_guard<std::recursive_mutex> lock(slot.mutex);
    const auto current = slot.state.Current();
    if (!current || current.generation != expected.generation ||
        current.phase != expected.phase || slot.transport != transport) {
        return false;
    }

    if (transport == NormalPulseTransport::AiPostWrite &&
        desired == NormalPulseTransport::NativePoll &&
        !slot.retryNeutralPending &&
        (current.phase == NormalInputPolicy::Phase::Press ||
         current.phase == NormalInputPolicy::Phase::PreNeutral)) {
        // The AI branch has no selected-button held latch. Native polling does,
        // so re-enter AwaitingStart and decide whether it needs a pre-neutral
        // from the live latch rather than inventing an edge mid-transaction.
        RelinquishPreparedNormalForConflict(
            playerNum, current.generation,
            "exact-tick normal changed from AI to native control");
        return false;
    }

    if (transport == NormalPulseTransport::NativePoll &&
        desired == NormalPulseTransport::AiPostWrite &&
        !ClearNativeNormalHeldLatch(character, current.intent)) {
        LogOut("[AUTO-NORMAL] Could not clear native held latch during AI hand-off P" +
               std::to_string(playerNum) + " gen=" +
               std::to_string(current.generation), true);
    }
    slot.transport = desired;
    transport = desired;
    LogOut("[AUTO-NORMAL] P" + std::to_string(playerNum) +
           " controller changed; pulse continues via " +
           NormalPulseTransportName(desired), detailedLogging.load());
    return true;
}

bool TryPrepareNormalRoute(int playerNum,
                           uintptr_t character,
                           NormalProcessRoute& outRoute) {
    outRoute = {};
    if (playerNum < 1 || playerNum > 2 || !character ||
        g_onlineModeActive.load(std::memory_order_relaxed)) {
        return false;
    }

    NormalPulseRuntimeSlot& slot = s_normalPulse[playerNum];
    NormalInputPolicy::Snapshot snapshot;
    NormalPulseTransport transport = NormalPulseTransport::Unselected;
    uintptr_t expectedGameState = 0;
    uintptr_t expectedCharacter = 0;
    bool retryNeutral = false;
    bool abortAfterRetryNeutral = false;
    {
        std::lock_guard<std::recursive_mutex> lock(slot.mutex);
        // This is the first matching fighter pass after a raw release was
        // posted. The previous battle-tick consumer has now run, so the 64-Hz
        // writer may safely resume unless a promoted successor claims it below.
        if (slot.rawReleaseGuard) {
            slot.rawReleaseGuard = false;
            s_normalRawRegisterOwner[playerNum].store(
                false, std::memory_order_release);
            if (!slot.state.Active()) {
                slot.gameState = 0;
                slot.character = 0;
            }
        }
        snapshot = slot.state.Current();
        transport = slot.transport;
        expectedGameState = slot.gameState;
        expectedCharacter = slot.character;
        retryNeutral = slot.retryNeutralPending;
        abortAfterRetryNeutral = slot.abortAfterRetryNeutral;
    }
    // The normal case is deliberately one mutex read and no game-memory work.
    if (!snapshot) return false;

    const uintptr_t gameState = ReadLiveGameState();
    if (!gameState || gameState != expectedGameState ||
        character != expectedCharacter || character != GetPlayerPointer(playerNum)) {
        std::lock_guard<std::recursive_mutex> lock(slot.mutex);
        const auto current = slot.state.Current();
        if (current && current.generation == snapshot.generation) {
            AbortNormalPulseLocked(slot, playerNum,
                                   "world/character identity changed");
        }
        return false;
    }

    const bool nativeGateOpen = NativeNormalInputGateOpen();
    const bool consumerGateOpen = NormalConsumerGateOpen(playerNum, character);

    if (!nativeGateOpen && !retryNeutral) {
        (void)AbortExpectedImmediateNormal(
            playerNum, snapshot, expectedGameState, expectedCharacter,
            "exact-tick normal reached a closed round/input gate");
        return false;
    }

    // Existing poll/tutorial owners are authoritative. A regular normal waits
    // without expiring; an exact-tick dash normal is retired rather than moved
    // to a later fighter pass.
    if (PreparedNormalHasHigherPriorityOwner(playerNum)) {
        RelinquishPreparedNormalForConflict(
            playerNum, snapshot.generation,
            "exact-tick normal conflicted with an input owner");
        return false;
    }
    if (snapshot.phase != NormalInputPolicy::Phase::AwaitingStart &&
        !ReconcileNormalTransport(
            playerNum, character, snapshot, transport)) {
        return false;
    }

    if (retryNeutral) {
        outRoute.active = true;
        outRoute.player = playerNum;
        outRoute.mask = 0;
        outRoute.retryNeutral = true;
        outRoute.abortAfterWrite = abortAfterRetryNeutral;
        outRoute.transport = transport;
        outRoute.snapshot = snapshot;
        return true;
    }

    // updateEntityState returns before the character command/normal consumer
    // during contact/RG/super freeze. Posting in that window lets the next
    // producer pass clear it without the move ever seeing it, so press and
    // pre-neutral phases wait for a real consumer tick.
    const bool releasePhase =
        snapshot.phase == NormalInputPolicy::Phase::ReleaseNeutral;
    const bool startWindowOpen = releasePhase ||
        (consumerGateOpen &&
         NormalPostureReady(character, snapshot));
    if (!startWindowOpen) {
        if (AbortExpectedImmediateNormal(
                playerNum, snapshot, expectedGameState, expectedCharacter,
                "exact-tick normal had no consumer/action window")) {
            return false;
        }
        // A native pre-neutral can be followed by a launch/landing/freeze before
        // its press. Relinquish the raw lane and wait from AwaitingStart again;
        // otherwise ImmediateInput and adaptive stance could be starved forever.
        if ((snapshot.phase == NormalInputPolicy::Phase::Press ||
             snapshot.phase == NormalInputPolicy::Phase::PreNeutral) &&
            transport != NormalPulseTransport::Unselected) {
            std::lock_guard<std::recursive_mutex> lock(slot.mutex);
            const auto current = slot.state.Current();
            if (current && current.generation == snapshot.generation &&
                (current.phase == NormalInputPolicy::Phase::Press ||
                 current.phase == NormalInputPolicy::Phase::PreNeutral) &&
                slot.state.RestartAwaiting()) {
                slot.transport = NormalPulseTransport::Unselected;
                slot.deliveredAnyPhase = false;
                slot.pressAwaitingConfirmation = false;
                slot.pressBeforeMove = -1;
                slot.pressBeforeFrame = -1;
                slot.pressAttempts = 0;
                slot.retryNeutralPending = false;
                slot.abortAfterRetryNeutral = false;
                s_normalRawRegisterOwner[playerNum].store(
                    false, std::memory_order_release);
            }
        }
        return false;
    }

    if (snapshot.phase == NormalInputPolicy::Phase::AwaitingStart) {
        uint32_t aiFlag = 0xFFFFFFFFu;
        if (!SafeReadMemory(character + AI_CONTROL_FLAG_OFFSET,
                            &aiFlag, sizeof(aiFlag))) {
            (void)AbortExpectedImmediateNormal(
                playerNum, snapshot, expectedGameState, expectedCharacter,
                "exact-tick normal could not read controller mode");
            return false;
        }
        transport = aiFlag == 0
            ? NormalPulseTransport::NativePoll
            : NormalPulseTransport::AiPostWrite;
        // The AI branch owns pulse fields, not held-button latches, and clears
        // +394..397 at the beginning of every pass. It never needs a synthetic
        // pre-neutral. Human/native control uses the real +400..412 latches.
        const bool held = transport == NormalPulseTransport::NativePoll &&
            RequestedNativeNormalButtonHeld(character, snapshot.intent);

        // An exact dash-normal cannot spend this pass manufacturing a neutral
        // edge and then press on a later pass. If the requested native button
        // is already held, fail closed and leave that physical hold untouched.
        if (held && snapshot.timing == NormalInputPolicy::Timing::Immediate) {
            (void)AbortExpectedImmediateNormal(
                playerNum, snapshot, expectedGameState, expectedCharacter,
                "exact-tick normal required a pre-neutral edge");
            return false;
        }

        std::lock_guard<std::recursive_mutex> lock(slot.mutex);
        const NormalInputPolicy::Snapshot current = slot.state.Current();
        if (!current || current.generation != snapshot.generation ||
            current.phase != NormalInputPolicy::Phase::AwaitingStart ||
            !slot.state.Prepare(held)) {
            return false;
        }
        slot.transport = transport;
        slot.deliveredAnyPhase = false;
        slot.pressAwaitingConfirmation = false;
        slot.pressBeforeMove = -1;
        slot.pressBeforeFrame = -1;
        slot.pressAttempts = 0;
        slot.retryNeutralPending = false;
        slot.abortAfterRetryNeutral = false;
        // Both routes ultimately populate +392..397. Keep every competing
        // immediate/stance writer out until EFZ's later character consumer and
        // the following authoritative neutral have both passed.
        s_normalRawRegisterOwner[playerNum].store(
            true, std::memory_order_release);
        snapshot = slot.state.Current();
    }

    if (!snapshot || snapshot.phase == NormalInputPolicy::Phase::AwaitingStart ||
        transport == NormalPulseTransport::Unselected) {
        return false;
    }

    outRoute.active = true;
    outRoute.player = playerNum;
    outRoute.transport = transport;
    outRoute.snapshot = snapshot;
    outRoute.mask = snapshot.phase == NormalInputPolicy::Phase::Press
        ? NormalInputPolicy::ResolveMask(
              snapshot.intent,
              PredictFacingRightForConsumer(playerNum, character))
        : 0;
    return true;
}

void AcknowledgeNormalRoute(const NormalProcessRoute& route,
                            short beforeMove,
                            short afterMove) {
    if (!route.active || !route.observed || route.player < 1 || route.player > 2) {
        return;
    }

    NormalPulseRuntimeSlot& slot = s_normalPulse[route.player];
    bool releasePosted = false;
    bool acknowledged = false;
    NormalInputPolicy::Phase nextPhase = NormalInputPolicy::Phase::Idle;
    {
        std::lock_guard<std::recursive_mutex> lock(slot.mutex);
        const NormalInputPolicy::Snapshot current = slot.state.Current();
        if (current && current.generation == route.snapshot.generation &&
            current.phase == route.snapshot.phase &&
            slot.transport == route.transport) {
            releasePosted = current.phase ==
                NormalInputPolicy::Phase::ReleaseNeutral;
            acknowledged = slot.state.Acknowledge(route.snapshot);
            nextPhase = slot.state.Current().phase;
            if (acknowledged) {
                slot.deliveredAnyPhase = true;
            }
            if (acknowledged && releasePosted) {
                const bool successorPromoted = slot.state.Active();
                slot.transport = NormalPulseTransport::Unselected;
                slot.deliveredAnyPhase = false;
                slot.rawReleaseGuard = true;
                // Deliberately leave raw-register ownership asserted until the
                // updateEntityState hook returns after this battle tick's later
                // consumer (with next matching input pass as fallback).
                (void)successorPromoted;
            }
        }
    }

    if (!acknowledged) {
        LogOut("[AUTO-NORMAL] Ignored stale pass acknowledgement P" +
               std::to_string(route.player) + " gen=" +
               std::to_string(route.snapshot.generation), true);
        return;
    }

    LogOut("[AUTO-NORMAL] Consumed P" + std::to_string(route.player) +
           " gen=" + std::to_string(route.snapshot.generation) +
           " transport=" + NormalPulseTransportName(route.transport) +
           " phase=" + NormalPulsePhaseName(route.snapshot.phase) +
           " mask=0x" + FormatHexByte(route.mask) +
           " move=" + std::to_string(beforeMove) + "->" +
           std::to_string(afterMove) +
           " next=" + NormalPulsePhaseName(nextPhase),
           detailedLogging.load());

}

NormalRegisterWriteResult WriteAuthoritativeNormalMask(int playerNum,
                                                       uintptr_t character,
                                                       uint8_t mask) {
    if (!character || GetPlayerPointer(playerNum) != character) {
        return NormalRegisterWriteResult::Failed;
    }

    // The producer may have emitted both a higher-priority special token and a
    // different normal pulse. This is common for AI, but a native poll can also
    // recognize a command already present in its input history. The later
    // character consumer checks those commands before normals and A/B/C/D in
    // order, so a raw button write alone is not authoritative. Scrub the
    // producer's command lanes, then write all four pulse bytes (zeroing the
    // other buttons) plus our exact direction.
    return WriteOwnedNormalRegisters(character, mask);
}

bool NeutralizeNativeNormalHistorySample(uintptr_t character,
                                         uint16_t headBefore,
                                         uint8_t expectedSample) {
    if (!character || headBefore >= INPUT_BUFFER_SIZE) return false;
    uint16_t headAfter = 0;
    uint8_t sample = 0;
    const uint16_t expectedAfter = static_cast<uint16_t>(
        (headBefore + 1u) % INPUT_BUFFER_SIZE);
    if (!SafeReadMemory(character + INPUT_BUFFER_INDEX_OFFSET,
                        &headAfter, sizeof(headAfter)) ||
        headAfter != expectedAfter ||
        !SafeReadMemory(character + INPUT_BUFFER_OFFSET + headBefore,
                        &sample, sizeof(sample)) ||
        sample != expectedSample) {
        return false;
    }

    // processCharacterInput has already converted this edge into held/pulse
    // state and run its current-tick recognizer. Removing only our just-written
    // history byte prevents the synthetic button from being recognized again
    // on following ticks; physical history before it remains untouched.
    const uint8_t neutral = 0;
    return SafeWriteMemory(character + INPUT_BUFFER_OFFSET + headBefore,
                           &neutral, sizeof(neutral));
}

int RunNormalRoute(int characterPtr,
                   NormalProcessRoute route) {
    NormalPulseRuntimeSlot& slot = s_normalPulse[route.player];
    std::unique_lock<std::recursive_mutex> processLock(slot.mutex);
    const NormalInputPolicy::Snapshot current = slot.state.Current();
    if (!current || current.generation != route.snapshot.generation ||
        current.phase != route.snapshot.phase ||
        slot.transport != route.transport) {
        processLock.unlock();
        return CallOriginalProcessCharacterInputWithContext(
            characterPtr, route.player);
    }

    // Preflight and producer execution are separate synchronization points.
    // Recheck both controller mode and higher-priority owners while the normal
    // slot is locked so no tutorial/macro press can be overwritten by a stale
    // AI post-write route.
    if (PreparedNormalHasHigherPriorityOwner(route.player)) {
        RelinquishPreparedNormalForConflict(
            route.player, route.snapshot.generation,
            "exact-tick normal lost its producer pass to an input owner");
        processLock.unlock();
        EnsureHumanControlForActivePollOverride(characterPtr, route.player);
        return CallOriginalProcessCharacterInputWithContext(
            characterPtr, route.player);
    }
    if (!ReconcileNormalTransport(
            route.player, static_cast<uintptr_t>(characterPtr),
            route.snapshot, route.transport)) {
        processLock.unlock();
        return CallOriginalProcessCharacterInputWithContext(
            characterPtr, route.player);
    }

    short beforeMove = -1;
    short beforeFrame = -1;
    (void)SafeReadMemory(static_cast<uintptr_t>(characterPtr) + MOVE_ID_OFFSET,
                         &beforeMove, sizeof(beforeMove));
    (void)SafeReadMemory(static_cast<uintptr_t>(characterPtr) +
                             CURRENT_FRAME_INDEX_OFFSET,
                         &beforeFrame, sizeof(beforeFrame));

    int ret = 0;
    if (route.transport == NormalPulseTransport::NativePoll) {
        uint16_t historyHeadBefore = 0;
        uint32_t selectedHeldBefore = 0;
        const int selectedButtonIndex = NormalButtonIndex(route.snapshot.intent);
        const bool capturedNativeState = selectedButtonIndex >= 0 &&
            SafeReadMemory(
            static_cast<uintptr_t>(characterPtr) + INPUT_BUFFER_INDEX_OFFSET,
            &historyHeadBefore, sizeof(historyHeadBefore)) &&
            SafeReadMemory(
                static_cast<uintptr_t>(characterPtr) +
                    kNativeHeldButtonOffsets[selectedButtonIndex],
                &selectedHeldBefore, sizeof(selectedHeldBefore));
        s_nativeNormalPollRoute = route;
        ret = CallOriginalProcessCharacterInputWithContext(
            characterPtr, route.player);
        route.nativePollObserved = s_nativeNormalPollRoute.observed;
        s_nativeNormalPollRoute = {};
        // Preserve the native edge/held bookkeeping created by HookedPoll, but
        // make its hand-off to the later character consumer authoritative. A
        // buffered command recognized by the original processor must not turn
        // a requested normal into a special.
        if (route.nativePollObserved) {
            // EFZ stores directions plus button rising edges, not the held poll
            // mask itself. Derive the exact byte from the selected DWORD latch
            // captured before original processing.
            const uint8_t expectedHistorySample =
                NormalInputPolicy::NativeHistorySample(
                    route.mask, route.snapshot.intent.button,
                    selectedHeldBefore != 0);
            const bool historyNeutralized = capturedNativeState &&
                NeutralizeNativeNormalHistorySample(
                    static_cast<uintptr_t>(characterPtr), historyHeadBefore,
                    expectedHistorySample);
            const NormalRegisterWriteResult writeResult = historyNeutralized
                ? WriteAuthoritativeNormalMask(
                      route.player, static_cast<uintptr_t>(characterPtr), route.mask)
                : (ClearOwnedNormalRegisters(
                       static_cast<uintptr_t>(characterPtr))
                       ? NormalRegisterWriteResult::ClearedAfterFailure
                       : NormalRegisterWriteResult::Failed);
            route.observed =
                writeResult == NormalRegisterWriteResult::Written ||
                (route.mask == 0 &&
                 writeResult == NormalRegisterWriteResult::ClearedAfterFailure);
            if (!route.observed) {
                // Our poll really ran, so its selected held latch is ours to
                // remove. Do not do this when a late macro/poll owner won.
                (void)ClearNativeNormalHeldLatch(
                    static_cast<uintptr_t>(characterPtr), route.snapshot.intent);
                if (!historyNeutralized) {
                    LogOut("[AUTO-NORMAL] Native history hand-off could not be neutralized P" +
                           std::to_string(route.player) + " gen=" +
                           std::to_string(route.snapshot.generation), true);
                }
            }
        }
    } else {
        // Preserve EFZ's AI branch and all of its bookkeeping. The later
        // character consumer reads +394..397 after this input pass, so post the
        // hook-owned pulse only after AI input generation has finished.
        ret = CallOriginalProcessCharacterInputWithContext(
            characterPtr, route.player);
        uint32_t aiFlagAfterProducer = 0;
        const bool controllerStillAi =
            SafeReadMemory(static_cast<uintptr_t>(characterPtr) +
                               AI_CONTROL_FLAG_OFFSET,
                           &aiFlagAfterProducer, sizeof(aiFlagAfterProducer)) &&
            aiFlagAfterProducer != 0;
        if (PreparedNormalHasHigherPriorityOwner(route.player) ||
            !controllerStillAi) {
            // No normal-owned phase was posted after this producer. A macro,
            // tutorial acquire, or controller flip may already own the raw
            // lane, so retire/restart without clearing those bytes.
            RelinquishPreparedNormalForConflict(
                route.player, route.snapshot.generation,
                "exact-tick normal lost AI ownership during its producer pass",
                false);
            return ret;
        } else {
            const NormalRegisterWriteResult writeResult =
                WriteAuthoritativeNormalMask(
                    route.player, static_cast<uintptr_t>(characterPtr), route.mask);
            route.observed =
                writeResult == NormalRegisterWriteResult::Written ||
                (route.mask == 0 &&
                 writeResult == NormalRegisterWriteResult::ClearedAfterFailure);
        }
    }

    short afterMove = beforeMove;
    (void)SafeReadMemory(static_cast<uintptr_t>(characterPtr) + MOVE_ID_OFFSET,
                         &afterMove, sizeof(afterMove));
    const auto clearThisRouteDelivery = [&]() {
        // A native cleanup may clear held/pulse state only if our HookedPoll
        // actually won. A macro/poll owner can appear after the preflight; its
        // same-button edge must remain untouched.
        if (route.transport == NormalPulseTransport::AiPostWrite ||
            route.nativePollObserved) {
            ClearDeliveredNormalState(
                route.player, route.transport, slot.gameState, slot.character,
                route.snapshot.intent);
        }
    };
    if (route.retryNeutral) {
        if (route.observed) {
            slot.retryNeutralPending = false;
            slot.abortAfterRetryNeutral = false;
        }
        if (route.observed && route.abortAfterWrite) {
            const char* reason =
                route.snapshot.timing == NormalInputPolicy::Timing::Immediate
                    ? "exact-tick normal was rejected by the character consumer"
                    : "normal action did not start after 3 consumer attempts";
            AbortNormalPulseLocked(
                slot, route.player, reason, true,
                route.snapshot.timing == NormalInputPolicy::Timing::WhenActionable);
        } else if (!route.observed && route.abortAfterWrite) {
            clearThisRouteDelivery();
            AbortNormalPulseLocked(
                slot, route.player,
                "final retry-neutral could not be delivered", true,
                route.snapshot.timing == NormalInputPolicy::Timing::WhenActionable);
        } else if (!route.observed) {
            LogOut("[AUTO-NORMAL] Retry-neutral delivery failed P" +
                   std::to_string(route.player) + " gen=" +
                   std::to_string(route.snapshot.generation), true);
        }
    } else if (route.observed &&
               route.snapshot.phase == NormalInputPolicy::Phase::Press) {
        // Do not call this "consumed" yet: updateEntityState runs later in this
        // battle tick and confirms a real attack-instance transition.
        slot.pressAwaitingConfirmation = true;
        slot.pressBeforeMove = beforeMove;
        slot.pressBeforeFrame = beforeFrame;
        if (slot.pressAttempts < 0xFF) ++slot.pressAttempts;
        slot.deliveredAnyPhase = true;
        LogOut("[AUTO-NORMAL] Posted P" + std::to_string(route.player) +
               " gen=" + std::to_string(route.snapshot.generation) +
               " transport=" + NormalPulseTransportName(route.transport) +
               " phase=press mask=0x" + FormatHexByte(route.mask) +
               " attempt=" + std::to_string(slot.pressAttempts) +
               " awaiting-consumer",
               detailedLogging.load());
    } else if (route.observed) {
        AcknowledgeNormalRoute(route, beforeMove, afterMove);
    } else if (route.snapshot.timing == NormalInputPolicy::Timing::Immediate) {
        // No matching producer hand-off occurred on the only valid dash tick.
        // Best-effort removal prevents a partially completed native poll from
        // becoming an untracked late press; the request itself must not retry.
        clearThisRouteDelivery();
        AbortNormalPulseLocked(
            slot, route.player,
            "exact-tick normal was not delivered on its matching fighter pass",
            true);
    } else {
        LogOut("[AUTO-NORMAL] Input pass did not consume/write P" +
               std::to_string(route.player) + " gen=" +
               std::to_string(route.snapshot.generation) +
               " transport=" + NormalPulseTransportName(route.transport) +
               " phase=" + NormalPulsePhaseName(route.snapshot.phase),
               true);
    }

    g_lastInjectedMask[route.player] = route.mask;
    g_wasBypassBuffered[route.player] = false;
    if (route.snapshot.phase == NormalInputPolicy::Phase::ReleaseNeutral) {
        MaybePerformTailCleanup(route.player);
    }
    return ret;
}

int __fastcall HookedUpdateEntityState(int characterPtr, int /*edx*/) {
    if (!oUpdateEntityState) return characterPtr;

    int playerNum = 0;
    if (characterPtr != 0 &&
        static_cast<uintptr_t>(characterPtr) == GetPlayerPointer(1)) {
        playerNum = 1;
    } else if (characterPtr != 0 &&
               static_cast<uintptr_t>(characterPtr) == GetPlayerPointer(2)) {
        playerNum = 2;
    }

    NormalInputPolicy::Snapshot posted;
    NormalPulseTransport transport = NormalPulseTransport::Unselected;
    short beforeMove = -1;
    short beforeFrame = -1;
    // Cancellation writes the same command/raw lanes consumed by vtable+16.
    // Hold this recursive slot lock across the exact consumer call so an
    // asynchronous reset cannot expose a torn command/button combination.
    std::unique_lock<std::recursive_mutex> consumerLock;
    if (playerNum != 0 &&
        s_inputHooksEnabled.load(std::memory_order_acquire)) {
        NormalPulseRuntimeSlot& slot = s_normalPulse[playerNum];
        consumerLock = std::unique_lock<std::recursive_mutex>(slot.mutex);
        const auto current = slot.state.Current();
        if (current && current.phase == NormalInputPolicy::Phase::Press &&
            slot.pressAwaitingConfirmation &&
            slot.character == static_cast<uintptr_t>(characterPtr)) {
            posted = current;
            transport = slot.transport;
            beforeMove = slot.pressBeforeMove;
            beforeFrame = slot.pressBeforeFrame;
        }
    }
    if (posted) {
        (void)SafeReadMemory(static_cast<uintptr_t>(characterPtr) + MOVE_ID_OFFSET,
                             &beforeMove, sizeof(beforeMove));
        (void)SafeReadMemory(static_cast<uintptr_t>(characterPtr) +
                                 CURRENT_FRAME_INDEX_OFFSET,
                             &beforeFrame, sizeof(beforeFrame));
    }

    const int result = oUpdateEntityState(characterPtr);
    // Release/abort cleanup was posted after this fighter's producer and has
    // now crossed the exact later consumer barrier. Competing 64-Hz writers may
    // resume without racing the just-consumed raw registers.
    if (playerNum != 0) {
        ReleaseRawGuardAfterConsumer(playerNum);
    }
    if (!posted || playerNum == 0) return result;

    short afterMove = -1;
    short afterFrame = -1;
    (void)SafeReadMemory(static_cast<uintptr_t>(characterPtr) + MOVE_ID_OFFSET,
                         &afterMove, sizeof(afterMove));
    (void)SafeReadMemory(static_cast<uintptr_t>(characterPtr) +
                             CURRENT_FRAME_INDEX_OFFSET,
                         &afterFrame, sizeof(afterFrame));

    bool started = false;
    bool retry = false;
    bool finalRejection = false;
    {
        NormalPulseRuntimeSlot& slot = s_normalPulse[playerNum];
        const auto current = slot.state.Current();
        if (!current || current.generation != posted.generation ||
            current.phase != NormalInputPolicy::Phase::Press ||
            slot.transport != transport ||
            !slot.pressAwaitingConfirmation ||
            slot.character != static_cast<uintptr_t>(characterPtr)) {
            return result;
        }

        started = DidConsumerStartRequestedNormal(
            static_cast<uintptr_t>(characterPtr), current.intent,
            beforeMove, beforeFrame, afterMove, afterFrame);
        slot.pressAwaitingConfirmation = false;
        slot.pressBeforeMove = -1;
        slot.pressBeforeFrame = -1;
        if (started && slot.state.Acknowledge(current)) {
            slot.pressAttempts = 0;
            slot.retryNeutralPending = false;
            slot.abortAfterRetryNeutral = false;
        } else {
            retry = true;
            finalRejection = current.timing == NormalInputPolicy::Timing::Immediate ||
                             slot.pressAttempts >= 3;
            slot.retryNeutralPending = true;
            slot.abortAfterRetryNeutral = finalRejection;
        }
    }

    LogOut("[AUTO-NORMAL] Consumer " +
           std::string(started ? "confirmed" :
               (finalRejection ? "rejected-final" : "not-started")) +
           " P" + std::to_string(playerNum) +
           " gen=" + std::to_string(posted.generation) +
           " transport=" + NormalPulseTransportName(transport) +
           " move=" + std::to_string(beforeMove) + ":" +
           std::to_string(beforeFrame) + "->" +
           std::to_string(afterMove) + ":" + std::to_string(afterFrame) +
           (retry ? " retry-neutral" : " release-neutral"),
           detailedLogging.load() || finalRejection);
    return result;
}

} // namespace

bool SetVanillaSwapInputRouting(bool enable) {
    std::lock_guard<std::mutex> lock(g_bindingSwapMutex);
    const bool previous = g_bindingSwapState.active;

    if (previous == enable) {
        if (!g_loggedRoutingStateOnce.exchange(true, std::memory_order_relaxed)) {
            LogOut(std::string("[INPUT_HOOK] Engine control-map swap already ")
                + (enable ? "ENABLED" : "DISABLED"), true);
        }
        return true;
    }

    BindingPair current{};
    if (!ReadBindingPair(current)) {
        LogOut("[INPUT_HOOK][ERROR] Cannot read EFZ P1/P2 live control maps", true);
        return false;
    }

    if (enable) {
        const BindingPair swapped = MakeSwappedBindingPair(current);
        if (!WriteAndVerifyBindingPair(swapped)) {
            LogOut("[INPUT_HOOK][ERROR] Failed to write/verify EFZ P1/P2 live control-map swap", true);
            return false;
        }

        g_bindingSwapState.baseline = current;
        g_bindingSwapState.active = true;
        g_loggedRoutingStateOnce.store(false, std::memory_order_relaxed);

        std::ostringstream oss;
        oss << "[INPUT_HOOK] Verified engine live control-map swap at 0x" << std::hex
            << std::uppercase << GetBindingPairAddress()
            << " | P1<-oldP2 [" << FormatBindingBlock(swapped.p1)
            << "] P2<-oldP1 [" << FormatBindingBlock(swapped.p2) << ']';
        LogOut(oss.str(), true);
        return true;
    }

    // Restore the exact baseline captured when the current swapped pair was applied.
    // If EFZ already restored it, leave memory untouched and only clear our state.
    const bool alreadyRestored = BindingPairsEqual(current, g_bindingSwapState.baseline);
    if (!alreadyRestored && !WriteAndVerifyBindingPair(g_bindingSwapState.baseline)) {
        LogOut("[INPUT_HOOK][ERROR] Failed to restore/verify EFZ P1/P2 live control maps", true);
        return false;
    }

    g_bindingSwapState = BindingSwapState{};
    g_loggedRoutingStateOnce.store(false, std::memory_order_relaxed);
    LogOut("[INPUT_HOOK] Restored verified default P1/P2 live control maps", true);
    return true;
}

static int RecordInputPollResult(unsigned int player, int result) {
    if (player == 1 || player == 2) {
        constexpr uint8_t kAttackButtons = 0xF0; // A/B/C/D
        const uint8_t mask = static_cast<uint8_t>(result);
        const uint8_t previous =
            g_lastPolledMask[player].exchange(mask, std::memory_order_acq_rel);
        const uint8_t rising = static_cast<uint8_t>(
            mask & static_cast<uint8_t>(~previous) & kAttackButtons);
        if (rising != 0) {
            g_pendingPollAttackEdges[player].fetch_or(rising, std::memory_order_release);
        }
        g_inputPollSerial[player].fetch_add(1, std::memory_order_release);
    }
    return result;
}

// A tutorial dummy lease owns P2's complete input stream, but it must still
// run through EFZ's native character-input processor.  That processor does
// more than poll the controller: it advances the history head, recognizes
// dash commands, updates button edges, and dispatches the character update.
// The old tutorial path wrote the immediate/history fields and returned from
// our process hook, which left a valid 66 pattern in memory without ever
// allowing EFZ to turn it into move 163.
static uint8_t TutorialP2PollMask() {
    if (p2QueueActive && p2QueueIndex >= 0 &&
        static_cast<size_t>(p2QueueIndex) < p2InputQueue.size()) {
        return p2InputQueue[p2QueueIndex].inputMask;
    }
    const uint8_t immediate = ImmediateInput::GetCurrentDesired(2);
    if (immediate != 0) {
        // An authored press may become active after normal preflight. It keeps
        // priority here as well; the native normal observes that it lost this
        // poll, then relinquishes/retries without extending the timed press.
        return immediate;
    }
    if (s_nativeNormalPollRoute.active &&
        s_nativeNormalPollRoute.player == 2) {
        s_nativeNormalPollRoute.observed = true;
        return s_nativeNormalPollRoute.mask;
    }
    // Zero is authoritative between motions: physical/CPU input cannot leak
    // into a scripted episode. Immediate presses (jump/air normal) share the
    // same native poll lane rather than bypassing the game processor.
    return 0;
}

static InputHookPolicy::TutorialP2InputSource TutorialP2Source() {
    if (g_bufferFreezingActive.load(std::memory_order_relaxed) &&
        (g_activeFreezePlayer.load(std::memory_order_relaxed) == 0 ||
         g_activeFreezePlayer.load(std::memory_order_relaxed) == 2)) {
        return InputHookPolicy::TutorialP2InputSource::BufferFreeze;
    }
    if (p2QueueActive) {
        return InputHookPolicy::TutorialP2InputSource::MotionQueue;
    }
    return ImmediateInput::GetCurrentDesired(2) != 0
        ? InputHookPolicy::TutorialP2InputSource::ImmediatePress
        : InputHookPolicy::TutorialP2InputSource::Neutral;
}

static void ObservePhysicalPollIfRequested(unsigned int logicalPlayer,
                                           int inputManagerPtr,
                                           unsigned int playerIndex) {
    if (logicalPlayer < 1 || logicalPlayer > 2 ||
        !g_observePhysicalPoll[logicalPlayer].load(std::memory_order_acquire)) {
        return;
    }
    const int raw = oPollPlayerInputState
        ? oPollPlayerInputState(inputManagerPtr, playerIndex) : 0;
    g_lastObservedPhysicalPoll[logicalPlayer].store(
        static_cast<uint8_t>(raw), std::memory_order_release);
}

// Our poll hook. Use __fastcall to match __thiscall trampoline signature.
static int __fastcall HookedPollPlayerInputState(int inputManagerPtr, int /*edx*/, unsigned int playerIndex)
{
    if (!s_inputHooksEnabled.load(std::memory_order_acquire)
        || g_onlineModeActive.load(std::memory_order_relaxed)) {
        return oPollPlayerInputState ? oPollPlayerInputState(inputManagerPtr, playerIndex) : 0;
    }

    // Engine uses 0 for P1 and 1 for P2; our globals use 1=P1, 2=P2.
    const unsigned int idxFromPollArg = (playerIndex <= 1) ? (playerIndex + 1) : 0;

    // Prefer the character currently being processed over the raw poll argument.
    // Side-switching paths can make EFZ poll P1's physical binding while it is
    // processing the P2 character; macro playback still needs to feed the P2
    // character in that case.
    const int processContext = s_processInputPlayerContext.load(std::memory_order_relaxed);
    const unsigned int idxFromProcessContext =
        (processContext == 1 || processContext == 2) ? static_cast<unsigned int>(processContext) : 0;

    // The process context is authoritative when side routing makes EFZ poll a
    // different physical binding.  Count this as an override hit so episode
    // diagnostics can prove that the native processor actually consumed the
    // injected input rather than merely advancing the tutorial queue.
    if (idxFromProcessContext == 2 && TutorialP2ControlLeaseActive()) {
        g_pollOverrideHitCount[2].fetch_add(1, std::memory_order_relaxed);
        return RecordInputPollResult(2, static_cast<int>(TutorialP2PollMask()));
    }

    if (idxFromProcessContext <= 2 && idxFromProcessContext != 0
        && g_pollOverrideActive[idxFromProcessContext].load(std::memory_order_relaxed)) {
        if (idxFromPollArg != 0 && idxFromPollArg != idxFromProcessContext
            && !s_loggedPollContextReroute[idxFromProcessContext].exchange(true, std::memory_order_relaxed)) {
            std::ostringstream oss;
            oss << "[INPUT_HOOK][POLL_OVERRIDE] Context-routed poll override: process=P"
                << idxFromProcessContext
                << " rawPollIndex=" << playerIndex
                << " rawPollPlayer=P" << idxFromPollArg;
            LogOut(oss.str(), true);
        }
        g_pollOverrideHitCount[idxFromProcessContext].fetch_add(1, std::memory_order_relaxed);
        ObservePhysicalPollIfRequested(idxFromProcessContext, inputManagerPtr, playerIndex);
        return RecordInputPollResult(
            idxFromProcessContext,
            static_cast<int>(g_pollOverrideMask[idxFromProcessContext].load(std::memory_order_relaxed)));
    }

    if (idxFromPollArg <= 2 && idxFromPollArg != 0
        && g_pollOverrideActive[idxFromPollArg].load(std::memory_order_relaxed)) {
        g_pollOverrideHitCount[idxFromPollArg].fetch_add(1, std::memory_order_relaxed);
        ObservePhysicalPollIfRequested(idxFromPollArg, inputManagerPtr, playerIndex);
        return RecordInputPollResult(
            idxFromPollArg,
            static_cast<int>(g_pollOverrideMask[idxFromPollArg].load(std::memory_order_relaxed)));
    }

    if (idxFromProcessContext != 0 && s_nativeNormalPollRoute.active &&
        s_nativeNormalPollRoute.player == static_cast<int>(idxFromProcessContext)) {
        s_nativeNormalPollRoute.observed = true;
        return RecordInputPollResult(
            idxFromProcessContext,
            static_cast<int>(s_nativeNormalPollRoute.mask));
    }

    const int result = oPollPlayerInputState
        ? oPollPlayerInputState(inputManagerPtr, playerIndex) : 0;
    const unsigned int logicalPlayer = idxFromProcessContext != 0
        ? idxFromProcessContext : idxFromPollArg;
    return RecordInputPollResult(logicalPlayer, result);
}

// Our custom function that will be called instead of the original.
// We use __fastcall for __thiscall hooks from MinHook.
int __fastcall HookedProcessCharacterInput(int characterPtr, int edx) {
    if (!s_inputHooksEnabled.load(std::memory_order_acquire)
        || g_onlineModeActive.load(std::memory_order_relaxed)) {
        return oProcessCharacterInput(characterPtr);
    }

    // Determine if this is P1 or P2 by comparing the character object pointer.
    
    // --- CORRECTED POINTER LOGIC ---
    // Use the reliable GetPlayerPointer utility instead of reading from the base offsets directly.
    uintptr_t p1Ptr = GetPlayerPointer(1);
    uintptr_t p2Ptr = GetPlayerPointer(2);
    // --- END CORRECTION ---

    int playerNum = 0;
    if (characterPtr != 0 && characterPtr == p1Ptr) playerNum = 1;
    else if (characterPtr != 0 && characterPtr == p2Ptr) playerNum = 2;

    // Safety: if we cannot identify the player, defer to the original immediately
    if (playerNum == 0) {
        return oProcessCharacterInput(characterPtr);
    }

#if EFZ_ENABLE_AUTO_ACTION_WAKE_TRACE
    InputHookWakeTraceScope wakeTrace(playerNum, static_cast<uintptr_t>(characterPtr));
#endif

    // Tick-integrated auto-actions: run once per internal sub-tick before P1 processing
    if (g_tickIntegratedAutoActions.load() && playerNum == 1) {
        short move1 = 0, move2 = 0;
        // Read move IDs directly from character structs (word at +MOVE_ID_OFFSET)
        (void)SafeReadMemory((uintptr_t)characterPtr + MOVE_ID_OFFSET, &move1, sizeof(move1));
        uintptr_t otherPtr = (characterPtr == (int)p1Ptr) ? p2Ptr : p1Ptr;
        if (otherPtr) {
            (void)SafeReadMemory(otherPtr + MOVE_ID_OFFSET, &move2, sizeof(move2));
        } else {
            move2 = -1;
        }
        AutoActionsTick_Inline(move1, move2);
#if EFZ_ENABLE_AUTO_ACTION_WAKE_TRACE
        wakeTrace.Mark("after-auto-tick");
#endif
    }

    // Do not inject while a buffer-freeze is active for THIS player; let the game's
    // original logic read the frozen buffer/index without interference.
    if (g_bufferFreezingActive.load()) {
        int freezeOwner = g_activeFreezePlayer.load();
        if (freezeOwner == playerNum || freezeOwner == 0) {
            RelinquishPreparedNormalForConflict(
                playerNum, 0,
                "exact-tick normal conflicted with buffer freeze");
            if (freezeOwner == playerNum) {
                // Throttle this diagnostic to avoid spamming every frame while freeze is active
                static std::chrono::steady_clock::time_point s_lastSkipLogAt[3] = { {}, {}, {} };
                auto now = std::chrono::steady_clock::now();
                bool timeOk = (s_lastSkipLogAt[playerNum].time_since_epoch().count() == 0) ||
                              ((now - s_lastSkipLogAt[playerNum]) >= std::chrono::milliseconds(250));
                if (detailedLogging.load() && timeOk) {
                    LogOut(std::string("[INPUT_HOOK] Skipping injection for P") + std::to_string(playerNum) +
                           " due to active buffer-freeze (owner=P" + std::to_string(freezeOwner) + ")", true);
                    s_lastSkipLogAt[playerNum] = now;
                }
            }
            g_lastInjectedMask[playerNum] = 0;
            // Intentionally skip tail cleanup during freeze
#if EFZ_ENABLE_AUTO_ACTION_WAKE_TRACE
            wakeTrace.Mark("before-original:freeze-skip");
            wakeTrace.SetExit("exit:freeze-skip");
#endif
            return CallOriginalProcessCharacterInputWithContext(characterPtr, playerNum);
        }
    }

    NormalProcessRoute normalRoute;
    const bool normalRouteReady = TryPrepareNormalRoute(
        playerNum, static_cast<uintptr_t>(characterPtr), normalRoute);

    // A tutorial P2 lease must call the original processor. HookedPoll above
    // supplies its authoritative queue/immediate mask, while the original
    // function performs native command recognition and character updating.
    // In particular, bypassing this call leaves a valid F,N,F buffer pattern
    // inert and makes scripted approaches time out without entering move 163.
    const bool tutorialOwnsP2 = playerNum == 2 && TutorialP2ControlLeaseActive();
    if (tutorialOwnsP2 &&
        InputHookPolicy::TutorialP2Route(TutorialP2Source()) ==
            InputHookPolicy::ProcessRoute::NativePoll) {
        if (normalRouteReady) {
#if EFZ_ENABLE_AUTO_ACTION_WAKE_TRACE
            wakeTrace.Mark("before-original:tutorial-normal-pulse");
            wakeTrace.SetExit("exit:tutorial-normal-pulse");
#endif
            return RunNormalRoute(characterPtr, normalRoute);
        }
        const uint8_t currentMask = TutorialP2PollMask();
        g_lastInjectedMask[2] = currentMask;
        g_wasBypassBuffered[2] = false;
#if EFZ_ENABLE_AUTO_ACTION_WAKE_TRACE
        wakeTrace.Mark("before-original:tutorial-poll");
#endif
        const int ret = CallOriginalProcessCharacterInputWithContext(characterPtr, 2);
        MaybePerformTailCleanup(2);
#if EFZ_ENABLE_AUTO_ACTION_WAKE_TRACE
        wakeTrace.SetExit("exit:tutorial-poll");
#endif
        return ret;
    }

    // Trigger normals deliberately precede the legacy manual/motion-queue
    // bypass. AI targets retain AI control and receive a post-AI pulse; human
    // targets receive an exact native poll. Both own the following neutral
    // release. Macro/poll overrides and freezes remain authoritative.
    if (normalRouteReady) {
#if EFZ_ENABLE_AUTO_ACTION_WAKE_TRACE
        wakeTrace.Mark("before-original:normal-pulse");
        wakeTrace.SetExit("exit:normal-pulse");
#endif
        return RunNormalRoute(characterPtr, normalRoute);
    }

    // REVERTED LOGIC: The queue system now handles all other injection states.
    bool shouldInject = false;
    const bool exclusiveMacroOwnsPlayer = playerNum > 0 &&
        MacroController::IsExclusivePlayback() &&
        MacroController::GetPlaybackPlayer() == playerNum;
    if (playerNum > 0 && !exclusiveMacroOwnsPlayer) {
        if (g_manualInputOverride[playerNum].load()) {
            shouldInject = true;
        } else if (playerNum == 1 && p1QueueActive) {
            shouldInject = true;
        } else if (playerNum == 2 && p2QueueActive) {
            shouldInject = true;
        }
    }

    if (shouldInject) {
        uint8_t currentMask = 0;
        if (g_manualInputOverride[playerNum].load()) {
            currentMask = g_manualInputMask[playerNum].load();
        } else {
            std::vector<InputFrame>& queue = (playerNum == 1) ? p1InputQueue : p2InputQueue;
            int& queueIndex = (playerNum == 1) ? p1QueueIndex : p2QueueIndex;
            if (queueIndex >= 0 && (size_t)queueIndex < queue.size()) {
                currentMask = queue[queueIndex].inputMask;
            }
        }
        if (g_lastInjectedMask[playerNum] != currentMask) {
            static std::chrono::steady_clock::time_point lastLogAt[3] = { {}, {}, {} };
            auto now = std::chrono::steady_clock::now();
            bool timeOk = (lastLogAt[playerNum].time_since_epoch().count() == 0) || ((now - lastLogAt[playerNum]) >= std::chrono::seconds(2));
            if (detailedLogging.load() && timeOk) {
                LogOut(std::string("[INPUT_HOOK] Injecting for P") + std::to_string(playerNum) +
                       " mask=0x" + FormatHexByte(currentMask) +
                       (g_injectImmediateOnly[playerNum].load() ? " (immediate-only)" : " (buffered)"), true);
                lastLogAt[playerNum] = now;
            }
        }

        // Split behavior based on injection mode:
        //  - immediate-only: call original first so the game updates its own state, then override immediate regs.
        //  - buffered (queue/manual hold): bypass original to avoid buffer double-advance and write both immediate+buffer.
        // If forced bypass is enabled, always perform authoritative buffered injection.
        if (g_forceBypass[playerNum].load() || !g_injectImmediateOnly[playerNum].load()) {
            // Authoritative buffered injection: bypass original.
            WritePlayerInputImmediate(playerNum, currentMask);
            
            // CRITICAL FIX: Skip buffer writes for dash motions - they're written all at once when queued.
            // Frame-by-frame writes are too slow and get contaminated by neutral inputs from the game.
            int currentMotion = (playerNum == 1) ? p1CurrentMotionType : p2CurrentMotionType;
            bool isDashMotion = (currentMotion == MOTION_FORWARD_DASH || currentMotion == MOTION_BACK_DASH);
            if (!isDashMotion) {
                // Special handling for split injection (Immediate=0, Buffer=Macro):
                // If a poll override is active, prefer it for the buffer write.
                // This allows macro playback to suppress immediate inputs (via currentMask=0)
                // while still populating the buffer history (via pollOverride=Macro).
                uint8_t bufferMask = currentMask;
                if (g_pollOverrideActive[playerNum].load(std::memory_order_relaxed)) {
                    bufferMask = g_pollOverrideMask[playerNum].load(std::memory_order_relaxed);
                }
                WritePlayerInputToBuffer(playerNum, bufferMask);
            }
            
            g_lastInjectedMask[playerNum] = currentMask;
            g_wasBypassBuffered[playerNum] = true;
            MaybePerformTailCleanup(playerNum);
#if EFZ_ENABLE_AUTO_ACTION_WAKE_TRACE
            wakeTrace.SetExit("exit:bypass-buffered");
#endif
            return 0;
        } else {
            // For immediate-only, write before AND after calling the game's processor.
            // Some parts of the original function can overwrite immediate registers mid-frame.
            // Pre-write ensures the game reads our state; post-write stabilizes the final state for this tick.
            WritePlayerInputImmediate(playerNum, currentMask);
            EnsureHumanControlForActivePollOverride(characterPtr, playerNum);
#if EFZ_ENABLE_AUTO_ACTION_WAKE_TRACE
            wakeTrace.Mark("before-original:immediate-only");
#endif
            int ret = CallOriginalProcessCharacterInputWithContext(characterPtr, playerNum);
            WritePlayerInputImmediate(playerNum, currentMask);
            g_lastInjectedMask[playerNum] = currentMask;
            g_wasBypassBuffered[playerNum] = false;
            MaybePerformTailCleanup(playerNum);
#if EFZ_ENABLE_AUTO_ACTION_WAKE_TRACE
            wakeTrace.SetExit("exit:immediate-only");
#endif
            return ret;
        }
    } 
    
    // If no manual/queue injection is active, enforce the centralized immediate writer's desired mask
    // so that short presses/holds from ImmediateInput are not lost if the game overwrites registers.
    // We write before calling the game's processor (so it can see inputs this frame) and once more
    // after (to guard against late overwrites inside the function).
    {
        static uint8_t s_lastDesired[3] = {0, 0, 0};
        uint8_t desired = ImmediateInput::GetCurrentDesired(playerNum);
        bool haveDesired = (desired != 0) || (s_lastDesired[playerNum] != 0);
        if (haveDesired) {
            // Pre-write desired state
            WritePlayerInputImmediate(playerNum, desired);
            EnsureHumanControlForActivePollOverride(characterPtr, playerNum);
#if EFZ_ENABLE_AUTO_ACTION_WAKE_TRACE
            wakeTrace.Mark("before-original:immediate-desired");
#endif
            int ret = CallOriginalProcessCharacterInputWithContext(characterPtr, playerNum);
            // Post-write to ensure final state for this tick
            WritePlayerInputImmediate(playerNum, desired);
            s_lastDesired[playerNum] = desired;
            g_lastInjectedMask[playerNum] = desired;
            g_wasBypassBuffered[playerNum] = false;
            MaybePerformTailCleanup(playerNum);
#if EFZ_ENABLE_AUTO_ACTION_WAKE_TRACE
            wakeTrace.SetExit("exit:immediate-desired");
#endif
            return ret;
        }
    }
    
    // --- NORMAL MODE ---
    // No input is queued for this player.
    // If we were bypassing last frame (buffered injection), force a one-time neutral clear to avoid latched immediate state.
    if (g_wasBypassBuffered[playerNum]) {
        WritePlayerInputImmediate(playerNum, 0);
        g_wasBypassBuffered[playerNum] = false;
    }
    // If the immediate input service has a desired mask, apply it just before original processing
    // so the game sees the current immediate inputs this frame (no buffer writes here).
    uint8_t desired = ImmediateInput::GetCurrentDesired(playerNum);
    if (desired != 0) {
        WritePlayerInputImmediate(playerNum, desired);
        g_lastInjectedMask[playerNum] = desired;
    } else {
        // Reset the last injected mask for this player to ensure a clean state on the next injection.
        g_lastInjectedMask[playerNum] = 0;
    }
    {
        EnsureHumanControlForActivePollOverride(characterPtr, playerNum);
#if EFZ_ENABLE_AUTO_ACTION_WAKE_TRACE
        wakeTrace.Mark("before-original:normal");
#endif
        int ret = CallOriginalProcessCharacterInputWithContext(characterPtr, playerNum);
        MaybePerformTailCleanup(playerNum);
#if EFZ_ENABLE_AUTO_ACTION_WAKE_TRACE
        wakeTrace.SetExit("exit:normal");
#endif
        return ret;
    }
}

void InstallInputHook() {
    uintptr_t targetAddr = 0;
    uintptr_t pollAddr = 0;
    uintptr_t stateUpdateAddr = 0;
    if (!ResolveInputHookTargets(targetAddr, pollAddr, stateUpdateAddr)) {
        return;
    }
    s_processTargetAddr = targetAddr;
    s_pollTargetAddr = pollAddr;
    s_stateUpdateTargetAddr = stateUpdateAddr;

    if (!s_inputHooksCreated.load(std::memory_order_acquire)) {
        if (!ValidateStateUpdateTarget(stateUpdateAddr)) {
            return;
        }
        if (!MinHookUtils::CreateHook(reinterpret_cast<LPVOID>(targetAddr),
                                      reinterpret_cast<void*>(&HookedProcessCharacterInput),
                                      reinterpret_cast<void**>(&oProcessCharacterInput),
                                      "[INPUT_HOOK]",
                                      "processCharacterInput")) {
            return;
        }

        if (!MinHookUtils::CreateHook(reinterpret_cast<LPVOID>(pollAddr),
                                      reinterpret_cast<void*>(&HookedPollPlayerInputState),
                                      reinterpret_cast<void**>(&oPollPlayerInputState),
                                      "[INPUT_HOOK]",
                                      "pollPlayerInputState")) {
            (void)MinHookUtils::RemoveHook(reinterpret_cast<LPVOID>(targetAddr), "[INPUT_HOOK]", "processCharacterInput");
            return;
        }

        if (!MinHookUtils::CreateHook(reinterpret_cast<LPVOID>(stateUpdateAddr),
                                      reinterpret_cast<void*>(&HookedUpdateEntityState),
                                      reinterpret_cast<void**>(&oUpdateEntityState),
                                      "[INPUT_HOOK]",
                                      "updateEntityState")) {
            (void)MinHookUtils::RemoveHook(reinterpret_cast<LPVOID>(pollAddr), "[INPUT_HOOK]", "pollPlayerInputState");
            (void)MinHookUtils::RemoveHook(reinterpret_cast<LPVOID>(targetAddr), "[INPUT_HOOK]", "processCharacterInput");
            return;
        }

        if (!MinHookUtils::EnableHook(reinterpret_cast<LPVOID>(targetAddr), "[INPUT_HOOK]", "processCharacterInput")
            || !MinHookUtils::EnableHook(reinterpret_cast<LPVOID>(pollAddr), "[INPUT_HOOK]", "pollPlayerInputState")
            || !MinHookUtils::EnableHook(reinterpret_cast<LPVOID>(stateUpdateAddr), "[INPUT_HOOK]", "updateEntityState")) {
            (void)MinHookUtils::DisableHook(reinterpret_cast<LPVOID>(stateUpdateAddr), "[INPUT_HOOK]", "updateEntityState");
            (void)MinHookUtils::DisableHook(reinterpret_cast<LPVOID>(pollAddr), "[INPUT_HOOK]", "pollPlayerInputState");
            (void)MinHookUtils::DisableHook(reinterpret_cast<LPVOID>(targetAddr), "[INPUT_HOOK]", "processCharacterInput");
            (void)MinHookUtils::RemoveHook(reinterpret_cast<LPVOID>(stateUpdateAddr), "[INPUT_HOOK]", "updateEntityState");
            (void)MinHookUtils::RemoveHook(reinterpret_cast<LPVOID>(pollAddr), "[INPUT_HOOK]", "pollPlayerInputState");
            (void)MinHookUtils::RemoveHook(reinterpret_cast<LPVOID>(targetAddr), "[INPUT_HOOK]", "processCharacterInput");
            return;
        }

        s_inputHooksCreated.store(true, std::memory_order_release);
        LogOut("[INPUT_HOOK] Hooked processCharacterInput at " + FormatHexAddress(targetAddr), true);
        LogOut("[INPUT_HOOK] Hooked pollPlayerInputState at " + FormatHexAddress(pollAddr), true);
        LogOut("[INPUT_HOOK] Hooked updateEntityState consumer at " + FormatHexAddress(stateUpdateAddr), true);
    }

    if (g_onlineModeActive.load(std::memory_order_relaxed)) {
        SetInputHookActive(false);
        return;
    }

    SetInputHookActive(true);
}

void SetInputHookActive(bool active) {
    if (!s_inputHooksCreated.load(std::memory_order_acquire)) {
        if (active) {
            InstallInputHook();
        }
        return;
    }

    const bool currentlyEnabled = s_inputHooksEnabled.load(std::memory_order_acquire);
    if (currentlyEnabled == active) {
        return;
    }

    if (!active) {
        // Close submission first, then drain anything already accepted.
        s_inputHooksEnabled.store(false, std::memory_order_release);
        CancelAllAutoActionNormalPulses();
    } else {
        s_inputHooksEnabled.store(true, std::memory_order_release);
    }
    LogOut(std::string("[INPUT_HOOK] Input hooks ") + (active ? "enabled" : "disabled"), true);
}

void RemoveInputHook() {
    s_inputHooksEnabled.store(false, std::memory_order_release);
    CancelAllAutoActionNormalPulses();
    (void)SetVanillaSwapInputRouting(false);
    if (s_processTargetAddr) {
        (void)MinHookUtils::DisableHook((LPVOID)s_processTargetAddr, "[INPUT_HOOK]", "processCharacterInput");
        (void)MinHookUtils::RemoveHook((LPVOID)s_processTargetAddr, "[INPUT_HOOK]", "processCharacterInput");
    }
    if (s_pollTargetAddr) {
        (void)MinHookUtils::DisableHook((LPVOID)s_pollTargetAddr, "[INPUT_HOOK]", "pollPlayerInputState");
        (void)MinHookUtils::RemoveHook((LPVOID)s_pollTargetAddr, "[INPUT_HOOK]", "pollPlayerInputState");
    }
    if (s_stateUpdateTargetAddr) {
        (void)MinHookUtils::DisableHook((LPVOID)s_stateUpdateTargetAddr, "[INPUT_HOOK]", "updateEntityState");
        (void)MinHookUtils::RemoveHook((LPVOID)s_stateUpdateTargetAddr, "[INPUT_HOOK]", "updateEntityState");
    }
    s_inputHooksEnabled.store(false, std::memory_order_release);
    s_inputHooksCreated.store(false, std::memory_order_release);
    s_processTargetAddr = 0;
    s_pollTargetAddr = 0;
    s_stateUpdateTargetAddr = 0;
    // No consumer detour remains to retire a deferred raw guard. Destruction is
    // the final shutdown barrier, so force-release the writer leases now.
    for (int playerNum = 1; playerNum <= 2; ++playerNum) {
        NormalPulseRuntimeSlot& slot = s_normalPulse[playerNum];
        std::lock_guard<std::recursive_mutex> lock(slot.mutex);
        slot.state.Reset();
        slot.transport = NormalPulseTransport::Unselected;
        slot.gameState = 0;
        slot.character = 0;
        slot.deliveredAnyPhase = false;
        slot.rawReleaseGuard = false;
        slot.pressAwaitingConfirmation = false;
        slot.pressBeforeMove = -1;
        slot.pressBeforeFrame = -1;
        slot.pressAttempts = 0;
        slot.retryNeutralPending = false;
        slot.abortAfterRetryNeutral = false;
        s_normalRawRegisterOwner[playerNum].store(
            false, std::memory_order_release);
    }
    LogOut("[INPUT_HOOK] Input hook removed.", true);
}
