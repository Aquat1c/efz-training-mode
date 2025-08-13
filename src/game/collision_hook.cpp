#include "../include/game/collision_hook.h"
#include "../include/core/logger.h"
#include "../include/core/memory.h"
#include "../include/utils/utilities.h"
#include "../include/input/input_core.h"
#include "../3rdparty/minhook/include/MinHook.h"
#include "../include/game/practice_patch.h"
#include <windows.h>
#include <atomic>
#include <array>
#include <string>

// EFZ address of handlePlayerToPlayerCollision relative to module base (from decomp notes)
// Absolute: 0x00767F60, module base is 0x00400000, so relative offset is 0x00367F60
static constexpr uintptr_t HANDLE_P2P_COLLISION_OFFSET = 0x367F60;

// Original function pointer typedef and storage
using tHandleP2PCollision = int(__thiscall*)(void* gameSystem, int attackerPtr, int defenderPtr, int attackerFrameData, const void* defenderFrameData);
static tHandleP2PCollision oHandleP2PCollision = nullptr;

// Caches for last seen pointers and discovered offsets per player
static std::atomic<uintptr_t> g_lastAttackDataP1{0};
static std::atomic<uintptr_t> g_lastAttackDataP2{0};
static std::atomic<int> g_attackDataOffsetP1{-1};
static std::atomic<int> g_attackDataOffsetP2{-1};

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
    // Cache last seen frame-data pointer unconditionally; AttackReader will resolve nested attack-data.
    if (attackerPtr && attackerFrameData) {
        uintptr_t frameData = (uintptr_t)attackerFrameData;
        // Sanity range check, skip if not a plausible pointer
        if (frameData < 0x00400000 || frameData > 0x0FFFFFFF) {
            return oHandleP2PCollision(gameSystem, attackerPtr, defenderPtr, attackerFrameData, defenderFrameData);
        }
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

    // Optional: tiny sanity log when detailed logging enabled
    // uint8_t flags = 0; SafeReadMemory((uintptr_t)attackerFrameData + 0xAA, &flags, 1);
    // LogOut("[COLLISION_HOOK] attacker=" + FormatHexAddress(attackerPtr) + " frameData=" + FormatHexAddress((uintptr_t)attackerFrameData) + " flags=" + std::to_string(flags), detailedLogging.load());

    return oHandleP2PCollision(gameSystem, attackerPtr, defenderPtr, attackerFrameData, defenderFrameData);
}

void InstallCollisionHook() {
    uintptr_t base = GetEFZBase();
    if (!base) {
        LogOut("[COLLISION_HOOK] Failed to get game base address.", true);
        return;
    }
    uintptr_t targetAddr = base + HANDLE_P2P_COLLISION_OFFSET;

    if (MH_CreateHook((LPVOID)targetAddr, &HookedHandleP2PCollision, (LPVOID*)&oHandleP2PCollision) != MH_OK) {
        LogOut("[COLLISION_HOOK] Failed to create hook at address " + FormatHexAddress(targetAddr), true);
        return;
    }
    if (MH_EnableHook((LPVOID)targetAddr) != MH_OK) {
        LogOut("[COLLISION_HOOK] Failed to enable collision hook.", true);
        return;
    }
    LogOut("[COLLISION_HOOK] Installed at " + FormatHexAddress(targetAddr), true);
}

void RemoveCollisionHook() {
    uintptr_t base = GetEFZBase();
    if (!base) return;
    uintptr_t targetAddr = base + HANDLE_P2P_COLLISION_OFFSET;
    MH_DisableHook((LPVOID)targetAddr);
    MH_RemoveHook((LPVOID)targetAddr);
}

uintptr_t GetCachedAttackDataForPlayer(int playerNum) {
    return (playerNum == 1) ? g_lastAttackDataP1.load() : g_lastAttackDataP2.load();
}

int GetAttackDataOffsetForPlayer(int playerNum) {
    return (playerNum == 1) ? g_attackDataOffsetP1.load() : g_attackDataOffsetP2.load();
}
