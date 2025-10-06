#include "../../include/utils/pause_integration.h"
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
    // === State tracking ===
    std::atomic<bool> s_menuVisible{false};
    std::atomic<void*> s_practicePtr{nullptr};
    std::atomic<bool> s_practiceHooksInstalled{false};

    // Official pause ownership: did WE invoke the real toggle (sub_10075720) when opening the menu?
    std::atomic<bool> s_weUsedOfficialToggle{false};
    // Fallback ownership (only used if official path fails): did we manually assert pause flag & patch freeze?
    std::atomic<bool> s_weForcedFlagPause{false};
    std::atomic<bool> s_weAppliedPatchFreeze{false};

    // Gamespeed emergency fallback (should rarely trigger – only if neither official nor flag+patch succeeds)
    std::atomic<void*> s_battleContext{nullptr};
    std::atomic<bool> s_weFrozeGamespeed{false};
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
        // Capture pre-step counter if menu visible (so we can neutralize any increment)
        uint32_t before = 0;
        bool wantNeutralize = false;
        if (s_menuVisible.load(std::memory_order_relaxed)) {
            SafeReadMemory(reinterpret_cast<uintptr_t>(thisPtr)+PRACTICE_OFF_STEP_COUNTER, &before, sizeof(before));
            wantNeutralize = true;
        }
        int ret = oPracticeTick ? oPracticeTick(thisPtr) : 0;
        if (wantNeutralize) {
            uint32_t after = before;
            if (SafeReadMemory(reinterpret_cast<uintptr_t>(thisPtr)+PRACTICE_OFF_STEP_COUNTER, &after, sizeof(after))) {
                if (after == before + 1) {
                    // Roll back the step advance
                    SafeWriteMemory(reinterpret_cast<uintptr_t>(thisPtr)+PRACTICE_OFF_STEP_COUNTER, &before, sizeof(before));
                }
            }
        }
        return ret;
    }

    // Also capture pointer via Pause toggle entry (sub_10075720) when the user presses Space/P
    typedef int (__thiscall *tTogglePause)(void* thisPtr);
    static tTogglePause oTogglePause = nullptr;
    // Internal bypass lets us invoke the official toggle even while menu visible
    static std::atomic<bool> s_internalPauseBypass{false};
    static int __fastcall HookedTogglePause(void* thisPtr, void* /*edx*/) {
        if (thisPtr) s_practicePtr.store(thisPtr, std::memory_order_relaxed);
        if (s_menuVisible.load(std::memory_order_relaxed) && !s_internalPauseBypass.load(std::memory_order_relaxed)) {
            // Suppress user-initiated pause/unpause while menu open
            return 0;
        }
        return oTogglePause ? oTogglePause(thisPtr) : 0;
    }

    void EnsurePracticePtrHookInstalled() {
        // Attempt direct resolution first (fast path, no hooks needed once valid)
        if (!s_practicePtr.load()) {
            void* resolved = nullptr;
            if (TryResolvePracticePtrFromModeArray(resolved)) {
                s_practicePtr.store(resolved, std::memory_order_relaxed);
                LogOut("[PAUSE] Practice ptr resolved via mode array", true);
            }
        }
        if (s_practiceHooksInstalled.load()) return;
        HMODULE hRev = GetModuleHandleA("EfzRevival.dll");
        if (!hRev) return; // wait for injection
        void* tickTarget  = EFZ_RVA_TO_VA(hRev, EFZREV_RVA_PRACTICE_TICK);
        void* pauseTarget = EFZ_RVA_TO_VA(hRev, EFZREV_RVA_TOGGLE_PAUSE);
        bool anyHook = false;
        if (tickTarget && MH_CreateHook(tickTarget, &HookedPracticeTick, reinterpret_cast<void**>(&oPracticeTick)) == MH_OK && MH_EnableHook(tickTarget) == MH_OK) {
            anyHook = true; LogOut("[PAUSE] PracticeTick hook active", true);
        }
        if (pauseTarget && MH_CreateHook(pauseTarget, &HookedTogglePause, reinterpret_cast<void**>(&oTogglePause)) == MH_OK && MH_EnableHook(pauseTarget) == MH_OK) {
            anyHook = true; LogOut("[PAUSE] TogglePause hook active", true);
        }
        if (anyHook) s_practiceHooksInstalled.store(true);
    }

    // Practice pause flag helpers (flag semantics: 1 = paused, 0 = running)
    bool ReadPracticePauseFlag(bool &outPaused) {
        void* p = s_practicePtr.load(); if (!p) return false;
        uint8_t v = 0; if (!SafeReadMemory(reinterpret_cast<uintptr_t>(p) + PRACTICE_OFF_PAUSE_FLAG, &v, sizeof(v))) return false;
        outPaused = (v != 0); return true;
    }
    bool WritePracticePauseFlag(bool paused) {
        void* p = s_practicePtr.load(); if (!p) return false;
        uint8_t v = paused ? 1u : 0u; return SafeWriteMemory(reinterpret_cast<uintptr_t>(p) + PRACTICE_OFF_PAUSE_FLAG, &v, sizeof(v));
    }

    // Hook battle screen render to capture battleContext pointer.
    typedef BOOL (__thiscall *tRenderBattleScreen)(void* battleContext);
    static tRenderBattleScreen oRenderBattleScreen = nullptr;
    static BOOL __fastcall HookedRenderBattleScreen(void* battleContext, void* /*edx*/) {
        if (battleContext) s_battleContext.store(battleContext, std::memory_order_relaxed);
        return oRenderBattleScreen ? oRenderBattleScreen(battleContext) : FALSE;
    }

    void EnsureBattleContextHook() {
        static std::atomic<bool> installed{false};
        if (installed.load()) return;
        HMODULE hRev = GetModuleHandleA("EfzRevival.dll");
        if (!hRev) return; // wait until injected
        void* target = EFZ_RVA_TO_VA(hRev, EFZ_RVA_RENDER_BATTLE_SCREEN);
        if (!target) return;
        if (MH_CreateHook(target, &HookedRenderBattleScreen, reinterpret_cast<void**>(&oRenderBattleScreen)) != MH_OK) {
            LogOut("[PAUSE] Failed to create RenderBattleScreen hook (battleContext capture)", true);
            return;
        }
        if (MH_EnableHook(target) != MH_OK) {
            LogOut("[PAUSE] Failed to enable RenderBattleScreen hook (battleContext capture)", true);
            MH_RemoveHook(target);
            return;
        }
        installed.store(true);
        LogOut("[PAUSE] RenderBattleScreen hook installed (capturing battleContext)", true);
    }

    // Read/write game speed from battleContext + 0x1400
    bool ReadGamespeed(uint8_t &out) {
        void* bc = s_battleContext.load();
        if (!bc) return false;
        return SafeReadMemory(reinterpret_cast<uintptr_t>(bc) + 0x1400, &out, sizeof(out));
    }
    bool WriteGamespeed(uint8_t v) {
        void* bc = s_battleContext.load();
        if (!bc) return false;
        uintptr_t addr = reinterpret_cast<uintptr_t>(bc) + 0x1400;
        bool ok = SafeWriteMemory(addr, &v, sizeof(v));
        std::ostringstream oss; oss << "[PAUSE] WriteGamespeed battleContext+0x1400 at 0x" << std::hex << addr << ": " << std::dec << (int)v << (ok ? " (OK)" : " (FAIL)");
        LogOut(oss.str(), true);
        return ok;
    }

    // === Patch toggler (sub_1006B2A0) wrapper ===
    typedef int (__cdecl *tPatchToggle)(void* patchCtx, char enable);
    tPatchToggle GetPatchToggleFn() {
        HMODULE hRev = GetModuleHandleA("EfzRevival.dll"); if (!hRev) return nullptr;
        return reinterpret_cast<tPatchToggle>(EFZ_RVA_TO_VA(hRev, EFZREV_RVA_PATCH_TOGGLER));
    }
    void* GetPatchCtxPtr() {
        HMODULE hRev = GetModuleHandleA("EfzRevival.dll"); if (!hRev) return nullptr;
        return EFZ_RVA_TO_VA(hRev, EFZREV_RVA_PATCH_CTX);
    }
    bool ApplyPatchFreeze(bool freeze) {
        auto fn = GetPatchToggleFn(); void* ctx = GetPatchCtxPtr(); if (!fn || !ctx) return false;
        // Engine semantics: enable=0 => apply NOP patches (freeze); enable=1 => restore (run)
        char enable = freeze ? 0 : 1;
        __try { fn(ctx, enable); } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
        return true;
    }
    
    // Official pause toggle (sub_10075720)
    typedef int (__thiscall *tOfficialToggle)(void* thisPtr);
    tOfficialToggle GetOfficialToggleFn() {
        HMODULE hRev = GetModuleHandleA("EfzRevival.dll"); if (!hRev) return nullptr;
        return reinterpret_cast<tOfficialToggle>(EFZ_RVA_TO_VA(hRev, EFZREV_RVA_TOGGLE_PAUSE));
    }
    bool InvokeOfficialToggle() {
        void* p = s_practicePtr.load(); if (!p) return false; auto fn = GetOfficialToggleFn(); if (!fn) return false;
        // Temporarily bypass suppression so HookedTogglePause calls through
        s_internalPauseBypass.store(true, std::memory_order_relaxed);
        bool ok = false;
        __try { fn(p); ok = true; } __except(EXCEPTION_EXECUTE_HANDLER) { ok = false; }
        s_internalPauseBypass.store(false, std::memory_order_relaxed);
        return ok;
    }
}

namespace PauseIntegration {
    void EnsurePracticePointerCapture() { EnsurePracticePtrHookInstalled(); }
    void* GetPracticeControllerPtr() { return s_practicePtr.load(); }

    bool IsPracticePaused() { bool p=false; if (ReadPracticePauseFlag(p)) return p; return false; }
    bool __cdecl IsGameSpeedFrozen() { uint8_t v=3; if (ReadGamespeed(v)) return v==0; return false; }
    bool IsPausedOrFrozen() { return IsPracticePaused() || IsGameSpeedFrozen(); }

    // No periodic maintenance required for official path; fallback paths are one-shot.
    void MaintainFreezeWhileMenuVisible() {
        if (!s_menuVisible.load()) return;
        if (s_weFrozeGamespeed.load()) {
            uint8_t cur=3; if (ReadGamespeed(cur) && cur!=0) WriteGamespeed(0);
        }
        if (s_weForcedFlagPause.load()) {
            bool p=false; if (ReadPracticePauseFlag(p) && !p) WritePracticePauseFlag(true);
        }
    }

    void OnMenuVisibilityChanged(bool visible) {
        s_menuVisible.store(visible);
        EnsurePracticePtrHookInstalled();
        GameMode mode = GetCurrentGameMode();
        const bool inPractice = (mode == GameMode::Practice);
        const bool revivalLoaded = (GetModuleHandleA("EfzRevival.dll") != nullptr);
        std::ostringstream log; log << "[PAUSE] Menu=" << (visible?"open":"close") << " practice=" << (inPractice?1:0);
        LogOut(log.str(), true);

        if (visible) {
            // CLEAR previous ownership (defensive) – we re-detect each open.
            s_weUsedOfficialToggle.store(false);
            s_weForcedFlagPause.store(false);
            s_weAppliedPatchFreeze.store(false);
            s_weFrozeGamespeed.store(false);

            if (revivalLoaded && inPractice && s_practicePtr.load()) {
                bool alreadyPaused=false; ReadPracticePauseFlag(alreadyPaused);
                if (!alreadyPaused) {
                    // Try official toggle
                    if (InvokeOfficialToggle()) {
                        bool nowPaused=false; if (ReadPracticePauseFlag(nowPaused) && nowPaused) {
                            s_weUsedOfficialToggle.store(true);
                            LogOut("[PAUSE] Official toggle succeeded", true);
                            return;
                        }
                        LogOut("[PAUSE] Official toggle returned but state not paused; attempting fallback", true);
                    } else {
                        LogOut("[PAUSE] Official toggle call failed; attempting fallback", true);
                    }
                } else {
                    LogOut("[PAUSE] Already paused before menu; will not auto-unpause", true);
                    return; // respect external pause
                }
            }
            // Fallback 1: direct practice flag + patch freeze (mirrors paused state) if we have ptr & patch toggler
            bool appliedAny=false;
            if (revivalLoaded && s_practicePtr.load()) {
                bool p=false; ReadPracticePauseFlag(p);
                if (!p && WritePracticePauseFlag(true)) { s_weForcedFlagPause.store(true); appliedAny=true; LogOut("[PAUSE] Fallback: pause flag set", true); }
                if (ApplyPatchFreeze(true)) { s_weAppliedPatchFreeze.store(true); appliedAny=true; LogOut("[PAUSE] Fallback: patch freeze applied", true); }
            }
            // Fallback 2: brute gamespeed (last resort)
            if (!appliedAny) {
                uint8_t cur=3; if (ReadGamespeed(cur) && cur!=0) { s_prevGamespeed.store(cur); if (WriteGamespeed(0)) { s_weFrozeGamespeed.store(true); LogOut("[PAUSE] Emergency fallback: gamespeed frozen", true); } }
            }
        } else { // closing menu
            // Unwind in reverse priority order of what we actually used
            if (s_weUsedOfficialToggle.load()) {
                bool stillPaused=false; ReadPracticePauseFlag(stillPaused);
                if (stillPaused) {
                    if (InvokeOfficialToggle()) LogOut("[PAUSE] Official unpause", true);
                    else LogOut("[PAUSE] Failed to invoke official unpause", true);
                }
                s_weUsedOfficialToggle.store(false);
            }
            if (s_weAppliedPatchFreeze.load()) {
                ApplyPatchFreeze(false); // ignore result
                s_weAppliedPatchFreeze.store(false);
            }
            if (s_weForcedFlagPause.load()) {
                bool p=false; if (ReadPracticePauseFlag(p) && p) WritePracticePauseFlag(false);
                s_weForcedFlagPause.store(false);
            }
            if (s_weFrozeGamespeed.load()) {
                uint8_t cur=0; if (ReadGamespeed(cur) && cur==0) WriteGamespeed(s_prevGamespeed.load());
                s_weFrozeGamespeed.store(false);
            }
        }
    }

    bool ReadStepCounter(uint32_t &outCounter) {
        void* p = s_practicePtr.load(); if (!p) return false;
        return SafeReadMemory(reinterpret_cast<uintptr_t>(p) + PRACTICE_OFF_STEP_COUNTER, &outCounter, sizeof(outCounter));
    }

    bool ConsumeStepAdvance() {
        static uint32_t s_lastStep = 0;
        uint32_t cur = 0;
        if (!IsPracticePaused()) return false;
        if (!ReadStepCounter(cur)) return false;
        if (cur != s_lastStep) {
            s_lastStep = cur;
            return true;
        }
        return false;
    }
}
