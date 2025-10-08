#include "../../include/utils/pause_integration.h"
#include "../include/core/logger.h"
#include "../include/game/game_state.h"
#include "../include/utils/utilities.h" // FindEFZWindow
#include "../include/core/constants.h"
#include "../include/core/memory.h"
// RVAs and Practice offsets (local header)
#include "../include/game/practice_offsets.h"
#include "../include/game/efzrevival_addrs.h"
#include "../include/utils/network.h" // GetEfzRevivalVersion, EfzRevivalVersion
// MinHook for capturing Practice controller pointer
#include "../3rdparty/minhook/include/MinHook.h"
#include <windows.h>
#include <atomic>
#include <cstring>
#include <sstream>

namespace {
    static inline bool IsHVersion() {
        EfzRevivalVersion v = GetEfzRevivalVersion();
        return v == EfzRevivalVersion::Revival102h || v == EfzRevivalVersion::Revival102i;
    }
    // SEH-safe wrapper for calling EfzRevival's GetModeStruct in 1.02i
    typedef void* (__stdcall *tGetModeStruct)(int idx);
    static void* Seh_GetModeStruct(tGetModeStruct fn, int idx) {
        void* candPtr = nullptr;
        __try { candPtr = fn ? fn(idx) : nullptr; }
        __except (EXCEPTION_EXECUTE_HANDLER) { candPtr = nullptr; }
        return candPtr;
    }
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
    static bool ValidatePracticeCandidate(uintptr_t cand) {
        if (!cand) return false;
        int side = -1; uint8_t pauseFlag = 0xFF; uintptr_t primary = 0;
        SafeReadMemory(cand + PRACTICE_OFF_LOCAL_SIDE_IDX, &side, sizeof(side));
        // Use version-aware offset for pause flag
        uintptr_t pauseOff = EFZ_Practice_PauseFlagOffset();
        SafeReadMemory(cand + pauseOff, &pauseFlag, sizeof(pauseFlag));
        SafeReadMemory(cand + PRACTICE_OFF_SIDE_BUF_PRIMARY, &primary, sizeof(primary));
        bool sideOk = (side == 0 || side == 1);
        bool pauseOk = (pauseFlag == 0 || pauseFlag == 1);
        bool bufOk = (primary == (cand + PRACTICE_OFF_BUF_LOCAL_BASE)) || (primary == (cand + PRACTICE_OFF_BUF_REMOTE_BASE));
        return sideOk && pauseOk && bufOk;
    }

    bool TryResolvePracticePtrFromModeArray(void*& outPtr) {
        HMODULE hRev = GetModuleHandleA("EfzRevival.dll");
        if (!hRev) return false;
        auto base = reinterpret_cast<uintptr_t>(hRev);

        // For 1.02i, prefer calling sub_1006C040(idx) to fetch the mode struct
        EfzRevivalVersion ver = GetEfzRevivalVersion();
        if (ver == EfzRevivalVersion::Revival102i) {
            tGetModeStruct getMode = reinterpret_cast<tGetModeStruct>(base + 0x006C040);
            // Try current, then likely practice slots
            uint8_t rawMode = 255; GetCurrentGameMode(&rawMode);
            int tryIdx[3] = { (int)rawMode, 1, 3 };
            for (int t = 0; t < 3; ++t) {
                int idx = tryIdx[t]; if (idx < 0 || idx > 15) continue;
                void* candPtr = Seh_GetModeStruct(getMode, idx);
                uintptr_t cand = reinterpret_cast<uintptr_t>(candPtr);
                if (!cand) continue;
                if (!ValidatePracticeCandidate(cand)) continue;
                outPtr = reinterpret_cast<void*>(cand);
                std::ostringstream oss; oss << "[PAUSE] GetModeStruct idx=" << std::dec << idx << " resolved practice=0x" << std::hex << cand;
                LogOut(oss.str(), true);
                return true;
            }
            return false;
        }

        // Otherwise, try the global mode array RVA fast-path
        uintptr_t gm = EFZ_RVA_GameModePtrArray();
        if (!gm) return false;
        // Try current game mode index first
        uint8_t rawMode = 255; GetCurrentGameMode(&rawMode);
        int tryIdx[3] = { (int)rawMode, 1, 3 }; // prefer current mode, then common Practice(1), then legacy(3)
        for (int t = 0; t < 3; ++t) {
            int idx = tryIdx[t]; if (idx < 0 || idx > 15) continue;
            uintptr_t slotAddr = base + gm + 4 * idx;
            uintptr_t cand = 0;
            if (!SafeReadMemory(slotAddr, &cand, sizeof(cand)) || !cand) continue;
            if (!ValidatePracticeCandidate(cand)) continue;
            outPtr = reinterpret_cast<void*>(cand);
            std::ostringstream oss; oss << "[PAUSE] ModeArray idx=" << std::dec << idx << " resolved practice=0x" << std::hex << cand;
            LogOut(oss.str(), true);
            return true;
        }
        return false;
    }

    // Heuristic scan: iterate a small range of mode slots to find a struct that looks like Practice by invariants.
    bool TryResolvePracticePtrByScan(void*& outPtr) {
        HMODULE hRev = GetModuleHandleA("EfzRevival.dll");
        if (!hRev) return false;
        auto base = reinterpret_cast<uintptr_t>(hRev);
        uintptr_t gm = EFZ_RVA_GameModePtrArray();
        if (!gm) return false;
        for (int idx = 0; idx < 16; ++idx) {
            uintptr_t slotAddr = base + gm + 4 * idx;
            uintptr_t cand = 0;
            if (!SafeReadMemory(slotAddr, &cand, sizeof(cand)) || !cand) continue;
            if (ValidatePracticeCandidate(cand)) {
                outPtr = reinterpret_cast<void*>(cand);
                std::ostringstream oss; oss << "[PAUSE] Heuristic scan resolved practice=0x" << std::hex << cand << " (slot=" << std::dec << idx << ")";
                LogOut(oss.str(), true);
                return true;
            }
        }
        LogOut("[PAUSE] Heuristic scan failed to resolve Practice controller", true);
        return false;
    }

    // Lightweight hooks to capture Practice controller pointer (ECX of Practice tick)
    // 1.02e: sub_10074F70(int this) -> we use (void*) and no extra arg
    typedef int (__thiscall *tPracticeTickE)(void* thisPtr);
    static tPracticeTickE oPracticeTickE = nullptr;
    static int __fastcall HookedPracticeTickE(void* thisPtr, void* /*edx*/) {
        s_practicePtr.store(thisPtr, std::memory_order_relaxed);
        {
            std::ostringstream oss; oss << "[PAUSE] HookedPracticeTickE ECX=0x" << std::hex << (uintptr_t)thisPtr;
            LogOut(oss.str(), false);
        }
        // Capture pre-step counter if menu visible (so we can neutralize any increment)
        uint32_t before = 0; bool wantNeutralize = false;
        if (s_menuVisible.load(std::memory_order_relaxed)) {
            uintptr_t stepCounterOff = EFZ_Practice_StepCounterOffset();
            SafeReadMemory(reinterpret_cast<uintptr_t>(thisPtr)+stepCounterOff, &before, sizeof(before));
            wantNeutralize = true;
        }
        int ret = oPracticeTickE ? oPracticeTickE(thisPtr) : 0;
        if (wantNeutralize) {
            uint32_t after = before;
            uintptr_t stepCounterOff = EFZ_Practice_StepCounterOffset();
            if (SafeReadMemory(reinterpret_cast<uintptr_t>(thisPtr)+stepCounterOff, &after, sizeof(after))) {
                if (after == before + 1) {
                    SafeWriteMemory(reinterpret_cast<uintptr_t>(thisPtr)+stepCounterOff, &before, sizeof(before));
                }
            }
        }
        return ret;
    }

    // 1.02h: sub_10074F40(int this, int a2) -> we must accept the extra int argument
    typedef char (__thiscall *tPracticeTickH)(void* thisPtr, int a2);
    static tPracticeTickH oPracticeTickH = nullptr;
    static char __fastcall HookedPracticeTickH(void* thisPtr, void* /*edx*/, int a2) {
        s_practicePtr.store(thisPtr, std::memory_order_relaxed);
        {
            std::ostringstream oss; oss << "[PAUSE] HookedPracticeTickH ECX=0x" << std::hex << (uintptr_t)thisPtr << " a2=" << std::dec << a2;
            LogOut(oss.str(), false);
        }
        uint32_t before = 0; bool wantNeutralize = false;
        if (s_menuVisible.load(std::memory_order_relaxed)) {
            uintptr_t stepCounterOff = EFZ_Practice_StepCounterOffset();
            SafeReadMemory(reinterpret_cast<uintptr_t>(thisPtr)+stepCounterOff, &before, sizeof(before));
            wantNeutralize = true;
        }
        char ret = oPracticeTickH ? oPracticeTickH(thisPtr, a2) : 0;
        if (wantNeutralize) {
            uint32_t after = before;
            uintptr_t stepCounterOff = EFZ_Practice_StepCounterOffset();
            if (SafeReadMemory(reinterpret_cast<uintptr_t>(thisPtr)+stepCounterOff, &after, sizeof(after))) {
                if (after == before + 1) {
                    SafeWriteMemory(reinterpret_cast<uintptr_t>(thisPtr)+stepCounterOff, &before, sizeof(before));
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
        {
            std::ostringstream oss; oss << "[PAUSE] HookedTogglePause ECX=0x" << std::hex << (uintptr_t)thisPtr
                << " menuVisible=" << (s_menuVisible.load()?1:0)
                << " bypass=" << (s_internalPauseBypass.load()?1:0);
            LogOut(oss.str(), true);
        }
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
            } else if (TryResolvePracticePtrByScan(resolved)) {
                s_practicePtr.store(resolved, std::memory_order_relaxed);
                LogOut("[PAUSE] Practice ptr resolved via heuristic scan", true);
            }
        }
        if (s_practiceHooksInstalled.load()) return;
        HMODULE hRev = GetModuleHandleA("EfzRevival.dll");
        if (!hRev) return; // wait for injection
        uintptr_t rvaTick = EFZ_RVA_PracticeTick();
        uintptr_t rvaPause = EFZ_RVA_TogglePause();
        void* tickTarget  = rvaTick ? EFZ_RVA_TO_VA(hRev, rvaTick) : nullptr;
        void* pauseTarget = rvaPause ? EFZ_RVA_TO_VA(hRev, rvaPause) : nullptr;
        {
            std::ostringstream oss; oss << "[PAUSE] Installing hooks: PracticeTick=0x" << std::hex << (uintptr_t)tickTarget
                << " TogglePause=0x" << (uintptr_t)pauseTarget;
            LogOut(oss.str(), true);
        }
        bool anyHook = false;
        // Select the correct PracticeTick hook based on version/signature
        if (tickTarget) {
            if (IsEfzRevivalLoaded() && IsHVersion()) {
                if (MH_CreateHook(tickTarget, &HookedPracticeTickH, reinterpret_cast<void**>(&oPracticeTickH)) == MH_OK && MH_EnableHook(tickTarget) == MH_OK) {
                    anyHook = true; LogOut("[PAUSE] PracticeTick hook active (1.02h/1.02i)", true);
                }
            } else {
                if (MH_CreateHook(tickTarget, &HookedPracticeTickE, reinterpret_cast<void**>(&oPracticeTickE)) == MH_OK && MH_EnableHook(tickTarget) == MH_OK) {
                    anyHook = true; LogOut("[PAUSE] PracticeTick hook active (1.02e)", true);
                }
            }
        }
        if (pauseTarget && MH_CreateHook(pauseTarget, &HookedTogglePause, reinterpret_cast<void**>(&oTogglePause)) == MH_OK && MH_EnableHook(pauseTarget) == MH_OK) {
            anyHook = true; LogOut("[PAUSE] TogglePause hook active", true);
        }
        if (anyHook) s_practiceHooksInstalled.store(true);
    }

    // Practice pause flag helpers (flag semantics: 1 = paused, 0 = running)
    bool ReadPracticePauseFlag(bool &outPaused) {
        void* p = s_practicePtr.load(); if (!p) return false;
        uintptr_t pauseOff = EFZ_Practice_PauseFlagOffset();
        uint8_t v = 0; if (!SafeReadMemory(reinterpret_cast<uintptr_t>(p) + pauseOff, &v, sizeof(v))) return false;
        outPaused = (v != 0); return true;
    }
    bool WritePracticePauseFlag(bool paused) {
        void* p = s_practicePtr.load(); if (!p) return false;
        uintptr_t pauseOff = EFZ_Practice_PauseFlagOffset();
        uint8_t v = paused ? 1u : 0u; return SafeWriteMemory(reinterpret_cast<uintptr_t>(p) + pauseOff, &v, sizeof(v));
    }

    // Reset the Practice step counter to 0, mirroring the official toggle behavior
    // 1.02e: +0xB0, 1.02h/i: +0x176
    bool ResetPracticeStepCounterToZero() {
        void* p = s_practicePtr.load(); if (!p) return false;
        uint32_t zero = 0;
        uintptr_t stepCounterOff = EFZ_Practice_StepCounterOffset();
        bool ok = SafeWriteMemory(reinterpret_cast<uintptr_t>(p) + stepCounterOff, &zero, sizeof(zero));
        if (ok) {
            std::ostringstream oss; oss << "[PAUSE] Reset step counter to 0 at +0x" << std::hex << stepCounterOff 
                << " for Practice=0x" << (uintptr_t)p;
            LogOut(oss.str(), true);
        } else {
            LogOut("[PAUSE] Failed to reset step counter (Practice ptr missing or write failed)", true);
        }
        return ok;
    }

    // Hook battle screen render to capture battleContext pointer.
    typedef BOOL (__thiscall *tRenderBattleScreen)(void* battleContext);
    static tRenderBattleScreen oRenderBattleScreen = nullptr;
    static BOOL __fastcall HookedRenderBattleScreen(void* battleContext, void* /*edx*/) {
        if (battleContext) s_battleContext.store(battleContext, std::memory_order_relaxed);
        {
            std::ostringstream oss; oss << "[PAUSE] RenderBattleScreen bc=0x" << std::hex << (uintptr_t)battleContext;
            LogOut(oss.str(), false);
        }
        return oRenderBattleScreen ? oRenderBattleScreen(battleContext) : FALSE;
    }

    void EnsureBattleContextHook() {
        static std::atomic<bool> installed{false};
        if (installed.load()) return;
        HMODULE hRev = GetModuleHandleA("EfzRevival.dll");
        if (!hRev) return; // wait until injected
    uintptr_t rva = EFZ_RVA_RenderBattleScreen();
    void* target = rva ? EFZ_RVA_TO_VA(hRev, rva) : nullptr;
        if (!target) return;
        if (MH_CreateHook(target, &HookedRenderBattleScreen, reinterpret_cast<void**>(&oRenderBattleScreen)) != MH_OK) {
            {
                std::ostringstream oss; oss << "[PAUSE] Failed to create RenderBattleScreen hook at VA=0x" << std::hex << (uintptr_t)target;
                LogOut(oss.str(), true);
            }
            return;
        }
        if (MH_EnableHook(target) != MH_OK) {
            {
                std::ostringstream oss; oss << "[PAUSE] Failed to enable RenderBattleScreen hook at VA=0x" << std::hex << (uintptr_t)target;
                LogOut(oss.str(), true);
            }
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

    // === Patch toggler wrapper ===
    // All supported versions (1.02e/h/i) use __thiscall: int func(void* this, char enable)
    // Unfreeze parameter varies by version (1 for 1.02e, 3 for 1.02h/i). See EFZ_PatchToggleUnfreezeParam().
    typedef int (__thiscall *tPatchToggle)(void* patchCtx, char enable);
    // Official pause toggle (sub_10075720)
    typedef int (__thiscall *tOfficialToggle)(void* thisPtr);

    // SEH helpers must avoid C++ objects with destructors in scope
    static bool SehCall_PatchToggleThis(void* ctx, tPatchToggle fn, char enable) {
        __try { fn(ctx, enable); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    // No stdcall variant required anymore (1.02i is also __thiscall)
    static bool SehCall_OfficialToggle(void* thisPtr, tOfficialToggle fn) {
        __try { fn(thisPtr); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    tPatchToggle GetPatchToggleFn() {
        HMODULE hRev = GetModuleHandleA("EfzRevival.dll"); if (!hRev) return nullptr;
        uintptr_t rva = EFZ_RVA_PatchToggler();
        if (!rva) return nullptr;
        return reinterpret_cast<tPatchToggle>(EFZ_RVA_TO_VA(hRev, rva));
    }
    // No stdcall getter required
    void* GetPatchCtxPtr() {
        HMODULE hRev = GetModuleHandleA("EfzRevival.dll"); if (!hRev) return nullptr;
        uintptr_t rva = EFZ_RVA_PatchCtx();
        if (!rva) return nullptr;
        return EFZ_RVA_TO_VA(hRev, rva);
    }
    bool ApplyPatchFreeze(bool freeze) {
        void* ctx = GetPatchCtxPtr(); if (!ctx) return false;
        int enableParam = freeze ? 0 : EFZ_PatchToggleUnfreezeParam();
        {
            std::ostringstream oss; oss << "[PAUSE] PatchToggle freeze=" << (freeze?1:0) << " param=" << enableParam
                << " ctx=0x" << std::hex << (uintptr_t)ctx;
            LogOut(oss.str(), true);
        }
        auto fn = GetPatchToggleFn(); if (!fn) return false;
        return SehCall_PatchToggleThis(ctx, fn, (char)enableParam);
    }
    tOfficialToggle GetOfficialToggleFn() {
        HMODULE hRev = GetModuleHandleA("EfzRevival.dll"); if (!hRev) return nullptr;
    uintptr_t rva = EFZ_RVA_TogglePause();
    if (!rva) return nullptr;
    return reinterpret_cast<tOfficialToggle>(EFZ_RVA_TO_VA(hRev, rva));
    }
    bool InvokeOfficialToggle() {
        void* p = s_practicePtr.load(); if (!p) return false; auto fn = GetOfficialToggleFn(); if (!fn) return false;
        // Temporarily bypass suppression so HookedTogglePause calls through
        s_internalPauseBypass.store(true, std::memory_order_relaxed);
        bool ok = false;
        ok = SehCall_OfficialToggle(p, fn);
        LogOut(std::string("[PAUSE] InvokeOfficialToggle -> ") + (ok?"OK":"EXCEPTION"), true);
        s_internalPauseBypass.store(false, std::memory_order_relaxed);
        return ok;
    }
}

namespace PauseIntegration {
    void EnsurePracticePointerCapture() { EnsurePracticePtrHookInstalled(); EnsureBattleContextHook(); }
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
        std::ostringstream log; log << "[PAUSE] Menu=" << (visible?"open":"close") << " practice=" << (inPractice?1:0)
            << " prx=0x" << std::hex << (uintptr_t)s_practicePtr.load();
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
            // Fallback 1: apply patch freeze unconditionally if EfzRevival is present, and set practice pause flag if pointer is known
            bool appliedAny=false;
            if (revivalLoaded) {
                // Try patch freeze first; it doesn't require the practice pointer
                if (ApplyPatchFreeze(true)) { s_weAppliedPatchFreeze.store(true); appliedAny=true; LogOut("[PAUSE] Fallback: patch freeze applied", true); }
                // If we already have the practice pointer, mirror engine state by setting its pause flag
                if (s_practicePtr.load()) {
                    bool p=false; ReadPracticePauseFlag(p);
                    if (!p && WritePracticePauseFlag(true)) { s_weForcedFlagPause.store(true); appliedAny=true; LogOut("[PAUSE] Fallback: pause flag set", true); }
                    // Mirror official behavior: reset step counter when toggling pause via fallback
                    ResetPracticeStepCounterToZero();
                }
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
                // Mirror official behavior on unfreeze: reset step counter to 0 as well
                ResetPracticeStepCounterToZero();
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
        uintptr_t stepCounterOff = EFZ_Practice_StepCounterOffset();
        return SafeReadMemory(reinterpret_cast<uintptr_t>(p) + stepCounterOff, &outCounter, sizeof(outCounter));
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
