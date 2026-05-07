#include "../include/game/collision_hook.h"
#include "../include/core/logger.h"
#include "../include/core/memory.h"
#include "../include/utils/utilities.h"
#include "../include/input/input_core.h"
#include "../3rdparty/minhook/include/MinHook.h"
#include "../include/game/practice_patch.h"
#include "../include/core/globals.h"
#include <windows.h>
#include <atomic>
#include <array>
#include <string>
#include <sstream>
#include <iomanip>
#include "../include/core/constants.h"
#include "../include/gui/overlay.h"
#include "../include/utils/minhook_utils.h"

// Offset of handlePlayerToPlayerCollision relative to module base
static constexpr uintptr_t HANDLE_P2P_COLLISION_OFFSET = 0x367F60;

// Original function pointer typedef and storage
using tHandleP2PCollision = int(__thiscall*)(void* gameSystem, int attackerPtr, int defenderPtr, int attackerFrameData, const void* defenderFrameData);
static tHandleP2PCollision oHandleP2PCollision = nullptr;

// Caches for last seen pointers and discovered offsets per player
static std::atomic<uintptr_t> g_lastAttackDataP1{0};
static std::atomic<uintptr_t> g_lastAttackDataP2{0};
static std::atomic<int> g_attackDataOffsetP1{-1};
static std::atomic<int> g_attackDataOffsetP2{-1};
static std::atomic<bool> s_collisionHookCreated{false};
static std::atomic<bool> s_collisionHookEnabled{false};
static uintptr_t s_collisionHookTargetAddr = 0;

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

} // namespace

// Identify which player owns this frame-data by scanning both player bases for a matching field.
static void IdentifyPlayerByFrameData(uintptr_t frameDataPtr, int& outPlayerNum, int& outOffset) {
    outPlayerNum = 0; outOffset = -1;
    if (!frameDataPtr) return;
    uintptr_t p1 = GetPlayerPointer(1);
    uintptr_t p2 = GetPlayerPointer(2);
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
    int off1 = scan(p1);
    if (off1 >= 0) { outPlayerNum = 1; outOffset = off1; return; }
    int off2 = scan(p2);
    if (off2 >= 0) { outPlayerNum = 2; outOffset = off2; return; }
}

// We use __fastcall wrapper to intercept __thiscall
static int __fastcall HookedHandleP2PCollision(void* gameSystem, void* /*edx*/, int attackerPtr, int defenderPtr, int attackerFrameData, const void* defenderFrameData) {
    if (!s_collisionHookEnabled.load(std::memory_order_acquire)
        || g_onlineModeActive.load(std::memory_order_relaxed)) {
        return oHandleP2PCollision(gameSystem, attackerPtr, defenderPtr, attackerFrameData, defenderFrameData);
    }

    // Cache last seen frame-data pointer unconditionally; AttackReader will resolve nested attack-data.
    if (attackerPtr && attackerFrameData) {
        uintptr_t frameData = (uintptr_t)attackerFrameData;
        // Sanity range check, skip caching if not a plausible pointer, but DO NOT early-return
        if (frameData >= 0x00400000 && frameData <= 0x0FFFFFFF) {
            int playerNum = 0; int fdOff = -1;
            IdentifyPlayerByFrameData(frameData, playerNum, fdOff);
            if (playerNum == 0) {
                // fallback by comparing attackerPtr directly
                uintptr_t p1b = GetPlayerPointer(1);
                uintptr_t p2b = GetPlayerPointer(2);
                if ((uintptr_t)attackerPtr == p1b) playerNum = 1; else if ((uintptr_t)attackerPtr == p2b) playerNum = 2;
            }
            if (playerNum == 1) {
                uintptr_t prev = g_lastAttackDataP1.exchange(frameData);
                if (frameData && frameData != prev) {
                    LogOut(std::string("[COLLISION_HOOK] P1 frameData=") + FormatHexAddress(frameData), true);
                }
                if (fdOff >= 0 && g_attackDataOffsetP1.load() < 0) {
                    g_attackDataOffsetP1.store(fdOff);
                    LogOut("[COLLISION_HOOK] Discovered frameData offset P1: " + std::to_string(fdOff), true);
                }
            } else if (playerNum == 2) {
                uintptr_t prev = g_lastAttackDataP2.exchange(frameData);
                if (frameData && frameData != prev) {
                    LogOut(std::string("[COLLISION_HOOK] P2 frameData=") + FormatHexAddress(frameData), true);
                }
                if (fdOff >= 0 && g_attackDataOffsetP2.load() < 0) {
                    g_attackDataOffsetP2.store(fdOff);
                    LogOut("[COLLISION_HOOK] Discovered frameData offset P2: " + std::to_string(fdOff), true);
                }
            }
        }
    }

    // Read pre-call defender HP to detect damage that happens during the call
    uintptr_t p1b = GetPlayerPointer(1);
    uintptr_t p2b = GetPlayerPointer(2);
    int preHpP1 = 0, preHpP2 = 0;
    if (p1b) { SafeReadMemory(p1b + HP_OFFSET, &preHpP1, sizeof(preHpP1)); }
    if (p2b) { SafeReadMemory(p2b + HP_OFFSET, &preHpP2, sizeof(preHpP2)); }

    int ret = oHandleP2PCollision(gameSystem, attackerPtr, defenderPtr, attackerFrameData, defenderFrameData);

        // Clean Hit rendering handled in frame_monitor via HP-drop detection; no-op here to avoid duplication

    return ret;
}

void InstallCollisionHook() {
    uintptr_t targetAddr = 0;
    if (!ResolveCollisionHookTarget(targetAddr)) {
        return;
    }
    s_collisionHookTargetAddr = targetAddr;

    if (!s_collisionHookCreated.load(std::memory_order_acquire)) {
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

    const bool currentlyEnabled = s_collisionHookEnabled.load(std::memory_order_acquire);
    if (currentlyEnabled == active) {
        return;
    }

    s_collisionHookEnabled.store(active, std::memory_order_release);
    LogOut(std::string("[COLLISION_HOOK] Collision hook ") + (active ? "enabled" : "disabled"), true);
}

void RemoveCollisionHook() {
    if (!s_collisionHookTargetAddr) return;
    (void)MinHookUtils::DisableHook((LPVOID)s_collisionHookTargetAddr, "[COLLISION_HOOK]", "handleP2PCollision");
    (void)MinHookUtils::RemoveHook((LPVOID)s_collisionHookTargetAddr, "[COLLISION_HOOK]", "handleP2PCollision");
    s_collisionHookEnabled.store(false, std::memory_order_release);
    s_collisionHookCreated.store(false, std::memory_order_release);
    s_collisionHookTargetAddr = 0;
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
}
