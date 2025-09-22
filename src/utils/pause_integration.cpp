#include "../include/utils/pause_integration.h"
#include "../include/core/logger.h"
#include "../include/game/game_state.h"
#include "../include/utils/utilities.h" // FindEFZWindow
#include "../include/core/constants.h"
#include "../include/core/memory.h"
// RVAs and Practice offsets
#include "../../out/efz_practice_offsets.h"
// MinHook for capturing Practice controller pointer
#include "../3rdparty/minhook/include/MinHook.h"
#include <windows.h>
#include <atomic>
#include <cstring>

namespace {
    std::atomic<bool> s_menuPausedByUs{false};
    std::atomic<bool> s_hookInstalled{false};
    std::atomic<void*> s_practicePtr{nullptr};
    // Gamespeed fallback (0 = freeze, 3 = normal). Resolved from efz.exe+0x39010C -> [ptr] + 0xF7FF8
    std::atomic<uintptr_t> s_gamespeedAddr{0};
    std::atomic<bool> s_speedPausedByUs{false};
    std::atomic<uint8_t> s_prevGamespeed{3};
    
    bool IsEfzRevivalLoaded() {
        return GetModuleHandleA("EfzRevival.dll") != nullptr;
    }

    // Lightweight hook to capture Practice controller pointer (ECX of Practice tick)
    typedef int (__thiscall *tPracticeTick)(void* thisPtr);
    static tPracticeTick oPracticeTick = nullptr;
    // We use __fastcall for __thiscall hooks (ECX=this, EDX=unused)
    static int __fastcall HookedPracticeTick(void* thisPtr, void* /*edx*/) {
        s_practicePtr.store(thisPtr, std::memory_order_relaxed);
        return oPracticeTick ? oPracticeTick(thisPtr) : 0;
    }

    void EnsurePracticePtrHookInstalled() {
        if (s_hookInstalled.load()) return;
        HMODULE hRev = GetModuleHandleA("EfzRevival.dll");
        if (!hRev) return;
        void* target = EFZ_RVA_TO_VA(hRev, EFZREV_RVA_PRACTICE_TICK);
        if (!target) return;
        if (MH_CreateHook(target, &HookedPracticeTick, reinterpret_cast<void**>(&oPracticeTick)) != MH_OK) {
            LogOut("[PAUSE] Failed to create PracticeTick hook (pointer capture)", true);
            return;
        }
        if (MH_EnableHook(target) != MH_OK) {
            LogOut("[PAUSE] Failed to enable PracticeTick hook (pointer capture)", true);
            // Best-effort cleanup
            MH_RemoveHook(target);
            return;
        }
        s_hookInstalled.store(true);
        LogOut("[PAUSE] PracticeTick hook installed (capturing Practice controller pointer)", true);
    }

    // Helpers to read/write the Practice pause flag safely
    bool ReadPracticePauseFlag(bool &outPaused) {
        void* p = s_practicePtr.load();
        if (!p) return false;
        uint8_t v = 0;
        if (!SafeReadMemory(reinterpret_cast<uintptr_t>(p) + PRACTICE_OFF_PAUSE_FLAG, &v, sizeof(v))) return false;
        outPaused = (v != 0);
        return true;
    }
    bool WritePracticePauseFlag(bool paused) {
        void* p = s_practicePtr.load();
        if (!p) return false;
        uint8_t v = paused ? 1u : 0u;
        return SafeWriteMemory(reinterpret_cast<uintptr_t>(p) + PRACTICE_OFF_PAUSE_FLAG, &v, sizeof(v));
    }

    // Resolve and access the global gamespeed byte using the pointer chain provided.
    // Address chain: efz.exe + 0x39010C -> [basePtr] + 0xF7FF8 yields a BYTE: 0 = freeze, 3 = normal.
    bool ResolveGamespeedAddress(uintptr_t &outAddr) {
        uintptr_t cached = s_gamespeedAddr.load();
        if (cached) { outAddr = cached; return true; }
        HMODULE hEfz = GetModuleHandleA("efz.exe");
        if (!hEfz) return false;
        uintptr_t base = reinterpret_cast<uintptr_t>(hEfz);
        uintptr_t ptrLoc = base + 0x39010C;
        uint32_t basePtr = 0;
        if (!SafeReadMemory(ptrLoc, &basePtr, sizeof(basePtr))) return false;
        if (!basePtr) return false;
        uintptr_t speedAddr = static_cast<uintptr_t>(basePtr) + 0xF7FF8;
        // Optionally, read once to validate address is readable
        uint8_t tmp = 0;
        if (!SafeReadMemory(speedAddr, &tmp, sizeof(tmp))) return false;
        s_gamespeedAddr.store(speedAddr);
        outAddr = speedAddr;
        LogOut("[PAUSE] Resolved gamespeed address via CE chain", true);
        return true;
    }
    bool ReadGamespeed(uint8_t &out) {
        uintptr_t addr = 0; if (!ResolveGamespeedAddress(addr)) return false;
        return SafeReadMemory(addr, &out, sizeof(out));
    }
    bool WriteGamespeed(uint8_t v) {
        uintptr_t addr = 0; if (!ResolveGamespeedAddress(addr)) return false;
        return SafeWriteMemory(addr, &v, sizeof(v));
    }
}

namespace PauseIntegration {
    void OnMenuVisibilityChanged(bool visible) {
        // Only apply in Practice mode; avoid affecting netplay/replay/menus.
        if (GetCurrentGameMode() != GameMode::Practice) return;
        // Practice pause flag control requires EfzRevival + captured pointer; gamespeed fallback does not.
        const bool revivalLoaded = IsEfzRevivalLoaded();
        if (revivalLoaded) {
            EnsurePracticePtrHookInstalled();
        }

        if (visible) {
            bool frozeSomething = false;
            // 1) Prefer Practice pause flag if available
            if (revivalLoaded && s_practicePtr.load()) {
                bool alreadyPaused = false; (void)ReadPracticePauseFlag(alreadyPaused);
                if (alreadyPaused) {
                    LogOut("[PAUSE] Menu opened: Practice already paused; leaving flag unchanged", true);
                    s_menuPausedByUs.store(false);
                } else if (WritePracticePauseFlag(true)) {
                    LogOut("[PAUSE] Menu opened: set Practice pause flag = 1", true);
                    s_menuPausedByUs.store(true);
                    frozeSomething = true;
                } else {
                    LogOut("[PAUSE] Menu opened: failed to write Practice pause flag", true);
                }
            }
            // 2) Also apply gamespeed freeze if not already frozen (covers cases where Practice flag isn't honored)
            uint8_t curSpeed = 0;
            if (ReadGamespeed(curSpeed)) {
                if (curSpeed != 0) {
                    s_prevGamespeed.store(curSpeed);
                    if (WriteGamespeed(0)) {
                        LogOut("[PAUSE] Menu opened: gamespeed set to 0 (freeze)", true);
                        s_speedPausedByUs.store(true);
                        frozeSomething = true;
                    } else {
                        LogOut("[PAUSE] Menu opened: failed to write gamespeed", true);
                    }
                } else {
                    LogOut("[PAUSE] Menu opened: gamespeed already 0; leaving as-is", true);
                    s_speedPausedByUs.store(false);
                }
            }
            if (!frozeSomething) {
                LogOut("[PAUSE] Menu opened: no freeze applied (state already paused)", true);
            }
        } else {
            // Closing menu: unfreeze only if we froze it.
            if (s_menuPausedByUs.load()) {
                bool isPaused = false; (void)ReadPracticePauseFlag(isPaused);
                if (isPaused) {
                    if (WritePracticePauseFlag(false)) {
                        LogOut("[PAUSE] Menu closed: cleared Practice pause flag (resume)", true);
                    } else {
                        LogOut("[PAUSE] Menu closed: failed to clear Practice pause flag", true);
                    }
                } else {
                    LogOut("[PAUSE] Menu closed: Practice already unpaused; no action", true);
                }
                s_menuPausedByUs.store(false);
            }
            if (s_speedPausedByUs.load()) {
                uint8_t cur = 0; if (ReadGamespeed(cur)) {
                    // Only restore if the value is still what we set (0)
                    if (cur == 0) {
                        const uint8_t prev = s_prevGamespeed.load();
                        if (WriteGamespeed(prev)) {
                            LogOut("[PAUSE] Menu closed: restored gamespeed", true);
                        } else {
                            LogOut("[PAUSE] Menu closed: failed to restore gamespeed", true);
                        }
                    } else {
                        LogOut("[PAUSE] Menu closed: gamespeed changed externally; not restoring", true);
                    }
                }
                s_speedPausedByUs.store(false);
            }
        }
    }
}
