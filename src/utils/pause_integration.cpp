#include "../include/utils/pause_integration.h"
#include "../include/core/logger.h"
#include "../include/game/game_state.h"
#include "../include/utils/utilities.h" // FindEFZWindow
#include "../include/core/constants.h"
#include "../include/core/memory.h"
// RVAs and Practice offsets (local header)
#include "../include/game/practice_offsets.h"
// MinHook for capturing Practice controller pointer
#include "../3rdparty/minhook/include/MinHook.h"
#include <windows.h>
#include <atomic>
#include <cstring>
#include <sstream>

namespace {
    std::atomic<bool> s_menuPausedByUs{false};
    std::atomic<bool> s_menuVisible{false};
    std::atomic<bool> s_hookInstalled{false};
    std::atomic<void*> s_practicePtr{nullptr};
    // Gamespeed fallback (0 = freeze, 3 = normal). Resolved from efz.exe+0x39010C -> [ptr] + 0xF7FF8
    std::atomic<uintptr_t> s_gamespeedAddr{0};
    std::atomic<bool> s_speedPausedByUs{false};
    std::atomic<uint8_t> s_prevGamespeed{3};
    
    bool IsEfzRevivalLoaded() {
        return GetModuleHandleA("EfzRevival.dll") != nullptr;
    }

    // Try to resolve Practice controller pointer directly from EfzRevival's mode array
    // EFZ_GameMode_GetStructByIndex(idx) = *(int*)(EfzRevivalBase + 0x790110 + 4*idx)
    // Practice index observed as 3 in decompile; guard for nulls and mode mismatch.
    bool TryResolvePracticePtrFromModeArray(void*& outPtr) {
        HMODULE hRev = GetModuleHandleA("EfzRevival.dll");
        if (!hRev) return false;
        auto base = reinterpret_cast<uintptr_t>(hRev);
        uintptr_t slotAddr = base + EFZREV_RVA_GAME_MODE_PTR_ARRAY + 4 * 3; // index 3
        uint32_t slotVal = 0;
        if (!SafeReadMemory(slotAddr, &slotVal, sizeof(slotVal))) return false;
        if (slotVal == 0) return false;
        outPtr = reinterpret_cast<void*>(static_cast<uintptr_t>(slotVal));
        return true;
    }

    // Lightweight hook to capture Practice controller pointer (ECX of Practice tick)
    typedef int (__thiscall *tPracticeTick)(void* thisPtr);
    static tPracticeTick oPracticeTick = nullptr;
    // We use __fastcall for __thiscall hooks (ECX=this, EDX=unused)
    static int __fastcall HookedPracticeTick(void* thisPtr, void* /*edx*/) {
        s_practicePtr.store(thisPtr, std::memory_order_relaxed);
        return oPracticeTick ? oPracticeTick(thisPtr) : 0;
    }

    // Also capture pointer via Pause toggle entry (sub_10075720) when the user presses Space/P
    typedef int (__thiscall *tTogglePause)(void* thisPtr);
    static tTogglePause oTogglePause = nullptr;
    static int __fastcall HookedTogglePause(void* thisPtr, void* /*edx*/) {
        if (thisPtr) s_practicePtr.store(thisPtr, std::memory_order_relaxed);
        return oTogglePause ? oTogglePause(thisPtr) : 0;
    }

    void EnsurePracticePtrHookInstalled() {
        // First, attempt static resolution to avoid depending on Pause or Tick timing
        void* resolved = nullptr;
        if (!s_practicePtr.load() && TryResolvePracticePtrFromModeArray(resolved)) {
            s_practicePtr.store(resolved, std::memory_order_relaxed);
            LogOut("[PAUSE] Resolved Practice pointer from mode array (no hooks)", true);
        }
        if (s_hookInstalled.load()) return;
        HMODULE hRev = GetModuleHandleA("EfzRevival.dll");
        if (!hRev) return;
        void* tickTarget = EFZ_RVA_TO_VA(hRev, EFZREV_RVA_PRACTICE_TICK);
        if (!tickTarget) return;
        if (MH_CreateHook(tickTarget, &HookedPracticeTick, reinterpret_cast<void**>(&oPracticeTick)) != MH_OK) {
            LogOut("[PAUSE] Failed to create PracticeTick hook (pointer capture)", true);
            return;
        }
        if (MH_EnableHook(tickTarget) != MH_OK) {
            LogOut("[PAUSE] Failed to enable PracticeTick hook (pointer capture)", true);
            // Best-effort cleanup
            MH_RemoveHook(tickTarget);
            return;
        }
        // Install secondary capture at Pause toggle (sub_10075720) to catch ECX when user toggles pause via hotkey
        void* pauseTarget = EFZ_RVA_TO_VA(hRev, EFZREV_RVA_TOGGLE_PAUSE);
        if (pauseTarget) {
            if (MH_CreateHook(pauseTarget, &HookedTogglePause, reinterpret_cast<void**>(&oTogglePause)) != MH_OK) {
                LogOut("[PAUSE] Failed to create TogglePause hook (pointer capture)", true);
            } else if (MH_EnableHook(pauseTarget) != MH_OK) {
                LogOut("[PAUSE] Failed to enable TogglePause hook (pointer capture)", true);
                MH_RemoveHook(pauseTarget);
            } else {
                LogOut("[PAUSE] TogglePause hook installed (secondary capture)", true);
            }
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
        // Use the host executable base (NULL) for reliability
        HMODULE hEfz = GetModuleHandleA(NULL);
        if (!hEfz) return false;
        uintptr_t base = reinterpret_cast<uintptr_t>(hEfz);
        uintptr_t ptrLoc = base + 0x39010C;
        uint32_t basePtr = 0;
        if (!SafeReadMemory(ptrLoc, &basePtr, sizeof(basePtr)) || !basePtr) return false;

        // Two observed candidates differ by 0x18: +0xF7FF8 (older) vs +0xF7FE0 (earlier).
        uintptr_t addr1 = static_cast<uintptr_t>(basePtr) + 0xF7FF8; // our previous
        uintptr_t addr2 = static_cast<uintptr_t>(basePtr) + 0xF7FE0; // reported correct
        uint8_t v1 = 0, v2 = 0; bool ok1 = SafeReadMemory(addr1, &v1, sizeof(v1)); bool ok2 = SafeReadMemory(addr2, &v2, sizeof(v2));

        // Choose the address whose value looks like a valid speed (0..3). Prefer addr2 if both are plausible.
        auto isPlausible = [](uint8_t v){ return v <= 3; };
        uintptr_t chosen = 0; uint8_t chosenVal = 0;
        if (ok1 && ok2) {
            if (isPlausible(v2)) { chosen = addr2; chosenVal = v2; }
            else if (isPlausible(v1)) { chosen = addr1; chosenVal = v1; }
            else { chosen = addr2; chosenVal = v2; }
        } else if (ok2) {
            chosen = addr2; chosenVal = v2;
        } else if (ok1) {
            chosen = addr1; chosenVal = v1;
        } else {
            return false;
        }

        s_gamespeedAddr.store(chosen);
        outAddr = chosen;
        {
            std::ostringstream oss; oss << "[PAUSE] Resolved gamespeed candidates: base=0x" << std::hex << base
                << ", ptrLoc=0x" << (base + 0x39010C) << ", basePtr=0x" << static_cast<uintptr_t>(basePtr)
                << ", addr1(+0xF7FF8)=0x" << addr1 << " val=" << std::dec << (ok1 ? (int)v1 : -1)
                << ", addr2(+0xF7FE0)=0x" << std::hex << addr2 << " val=" << std::dec << (ok2 ? (int)v2 : -1)
                << ", chosen=0x" << std::hex << chosen << " (val=" << std::dec << (int)chosenVal << ")";
            LogOut(oss.str(), true);
        }
        return true;
    }
    bool ReadGamespeed(uint8_t &out) {
        uintptr_t addr = 0; if (!ResolveGamespeedAddress(addr)) return false;
        return SafeReadMemory(addr, &out, sizeof(out));
    }
    bool WriteGamespeed(uint8_t v) {
        uintptr_t addr = 0; if (!ResolveGamespeedAddress(addr)) return false;
        bool ok = SafeWriteMemory(addr, &v, sizeof(v));
        std::ostringstream oss; oss << "[PAUSE] WriteGamespeed at 0x" << std::hex << addr << ": " << std::dec << (int)v << (ok ? " (OK)" : " (FAIL)");
        LogOut(oss.str(), true);
        return ok;
    }
}

namespace PauseIntegration {
    void EnsurePracticePointerCapture() { EnsurePracticePtrHookInstalled(); }
    void* GetPracticeControllerPtr() { return s_practicePtr.load(); }
    void MaintainFreezeWhileMenuVisible() {
        // If the menu isn't visible, nothing to maintain.
        if (!s_menuVisible.load()) return;

        // 1) Maintain Practice pause flag while visible (if we are in Practice and have the pointer)
        if (GetCurrentGameMode() == GameMode::Practice) {
            void* p = s_practicePtr.load();
            if (p) {
                bool isPaused = false;
                if (ReadPracticePauseFlag(isPaused) && !isPaused) {
                    // Re-assert pause if something cleared it while our menu is open
                    if (WritePracticePauseFlag(true)) {
                        LogOut("[PAUSE] Maintain: re-setting Practice pause flag during menu", true);
                        // Track that pause came from us so we clear on close
                        s_menuPausedByUs.store(true);
                    }
                }
            }
        }

        // 2) Maintain gamespeed freeze while in gameplay states
        if (s_speedPausedByUs.load() && IsInGameplayState()) {
            uint8_t cur = 3;
            if (ReadGamespeed(cur) && cur != 0) {
                // Someone unfroze time; re-freeze to maintain menu pause behavior
                if (WriteGamespeed(0)) {
                    LogOut("[PAUSE] Maintain: re-applying gamespeed freeze during menu", true);
                }
            }
        }
    }
    void OnMenuVisibilityChanged(bool visible) {
        // Prefer Practice-mode pause flag; fall back to gamespeed freeze during active gameplay
        GameMode mode = GetCurrentGameMode();
        const bool inGameplay = IsInGameplayState();
        LogOut(std::string("[PAUSE] Menu toggle: visible=") + (visible ? "1" : "0") + 
               ", mode=" + GetGameModeName(mode) + 
               ", gameplay=" + (inGameplay ? "1" : "0"), true);

        s_menuVisible.store(visible);

        // Practice pause flag control requires EfzRevival + captured pointer; gamespeed fallback does not.
        const bool revivalLoaded = IsEfzRevivalLoaded();
        if (revivalLoaded) {
            EnsurePracticePtrHookInstalled();
        }

        if (visible) {
            bool frozeSomething = false;

            // 1) If we're in Practice, prefer the Practice pause flag
            if (mode == GameMode::Practice) {
                if (revivalLoaded && s_practicePtr.load()) {
                    bool alreadyPaused = false; (void)ReadPracticePauseFlag(alreadyPaused);
                    if (alreadyPaused) {
                        LogOut("[PAUSE] Menu opened: Practice already paused; leaving flag unchanged", true);
                        s_menuPausedByUs.store(false);
                    } else if (WritePracticePauseFlag(true)) {
                        {
                            void* p = s_practicePtr.load();
                            std::ostringstream oss; oss << "[PAUSE] Menu opened: set Practice pause flag = 1 at 0x" << std::hex << (reinterpret_cast<uintptr_t>(p) + PRACTICE_OFF_PAUSE_FLAG);
                            LogOut(oss.str(), true);
                        }
                        s_menuPausedByUs.store(true);
                        frozeSomething = true;
                    } else {
                        LogOut("[PAUSE] Menu opened: failed to write Practice pause flag", true);
                    }
                } else {
                    LogOut("[PAUSE] Menu opened: Practice pointer not available; will rely on gamespeed freeze", true);
                }
            }

            // 2) Apply gamespeed freeze in any active match (covers practice fallback and non-practice gameplay)
            if (inGameplay) {
                uint8_t curSpeed = 0;
                if (ReadGamespeed(curSpeed)) {
                    if (curSpeed != 0) {
                        s_prevGamespeed.store(curSpeed);
                        uintptr_t addr=0; ResolveGamespeedAddress(addr);
                        {
                            std::ostringstream oss; oss << "[PAUSE] Menu opened: freeze gamespeed at 0x" << std::hex << addr << ", cur=" << std::dec << (int)curSpeed;
                            LogOut(oss.str(), true);
                        }
                        if (WriteGamespeed(0)) {
                            s_speedPausedByUs.store(true);
                            frozeSomething = true;
                        } else {
                            LogOut("[PAUSE] Menu opened: failed to write gamespeed", true);
                        }
                    } else {
                        LogOut("[PAUSE] Menu opened: gamespeed already 0; leaving as-is", true);
                        s_speedPausedByUs.store(false);
                    }
                } else {
                    LogOut("[PAUSE] Menu opened: could not resolve gamespeed address", true);
                }
            } else {
                LogOut("[PAUSE] Menu opened: not in gameplay; skipping gamespeed freeze", true);
            }

            if (!frozeSomething) {
                LogOut("[PAUSE] Menu opened: no freeze applied (state already paused or not applicable)", true);
            }
        } else {
            // Closing menu: unfreeze only if we froze it.
            if (s_menuPausedByUs.load()) {
                bool isPaused = false; (void)ReadPracticePauseFlag(isPaused);
                if (isPaused) {
                    if (WritePracticePauseFlag(false)) {
                        void* p = s_practicePtr.load();
                        std::ostringstream oss; oss << "[PAUSE] Menu closed: cleared Practice pause flag at 0x" << std::hex << (reinterpret_cast<uintptr_t>(p) + PRACTICE_OFF_PAUSE_FLAG);
                        LogOut(oss.str(), true);
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
                        uintptr_t addr=0; ResolveGamespeedAddress(addr);
                        {
                            std::ostringstream oss; oss << "[PAUSE] Menu closed: restore gamespeed at 0x" << std::hex << addr << ", prev=" << std::dec << (int)prev;
                            LogOut(oss.str(), true);
                        }
                        if (WriteGamespeed(prev)) {
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
