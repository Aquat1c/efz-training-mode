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
    // Helper to check for 1.02h specifically (only h uses the two-arg PracticeTick signature)
    static inline bool IsHOnly() {
        return GetEfzRevivalVersion() == EfzRevivalVersion::Revival102h;
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
    // Global once-only guard for verbose candidate validation log
    std::atomic<bool> s_loggedValidateOnce{false};
    // Character Select scoped suppression for noisy scan failures
    std::atomic<bool> s_csActive{false};
    std::atomic<bool> s_scanSuppressedThisCS{false};
    // Global once-only logging guard for heuristic scan failure
    std::atomic<bool> s_loggedScanFailOnce{false};

    // Official pause ownership: did WE invoke the real toggle (sub_10075720) when opening the menu?
    std::atomic<bool> s_weUsedOfficialToggle{false};
    // Fallback ownership (only used if official path fails): did we manually assert pause flag & patch freeze?
    std::atomic<bool> s_weForcedFlagPause{false};
    std::atomic<bool> s_weAppliedPatchFreeze{false};

    // Gamespeed emergency fallback (should rarely trigger – only if neither official nor flag+patch succeeds)
    std::atomic<void*> s_battleContext{nullptr};
    std::atomic<bool> s_weFrozeGamespeed{false};
    std::atomic<uint8_t> s_prevGamespeed{3};
    // Direct gamespeed byte address resolved from efz.exe GameMode array (CE-confirmed)
    std::atomic<uintptr_t> s_gamespeedAddr{0};
    // Vanilla EFZ pause ownership (engine pause via battleContext+0x1416)
    std::atomic<bool> s_weVanillaEnginePause{false};
    // Visual effect patches ownership (for vanilla/unsupported versions)
    std::atomic<bool> s_weAppliedVisualPatches{false};
    
    bool IsEfzRevivalLoaded() {
        // For unsupported versions, treat as vanilla (return false)
        return GetModuleHandleA("EfzRevival.dll") != nullptr && IsEfzRevivalVersionSupported();
    }

    // Validate a candidate Practice controller pointer by checking key invariants
    static bool ValidatePracticeCandidate(uintptr_t cand) {
        if (!cand) return false;
        EfzRevivalVersion ver = GetEfzRevivalVersion();
        {
            // Print this verbose diagnostic only once globally to avoid spam
            if (!s_loggedValidateOnce.exchange(true, std::memory_order_relaxed)) {
                std::ostringstream oss; oss << "[PAUSE] Validate candidate=0x" << std::hex << cand
                    << " ver=" << (EfzRevivalVersionName(ver) ? EfzRevivalVersionName(ver) : "?")
                    << " localOff=0x" << PRACTICE_OFF_LOCAL_SIDE_IDX
                    << " remoteOff=0x" << PRACTICE_OFF_REMOTE_SIDE_IDX
                    << " pauseOff=0x" << EFZ_Practice_PauseFlagOffset();
                LogOut(oss.str(), detailedLogging.load());
            }
        }
        int local = -1;
        uint8_t pauseFlag = 0xFF;
        (void)SafeReadMemory(cand + PRACTICE_OFF_LOCAL_SIDE_IDX, &local, sizeof(local));
        // Use version-aware offset for pause flag
        uintptr_t pauseOff = EFZ_Practice_PauseFlagOffset();
        if (pauseOff) (void)SafeReadMemory(cand + pauseOff, &pauseFlag, sizeof(pauseFlag));
        bool sideOk = (local == 0 || local == 1);
        bool pauseOk = (pauseOff == 0 || pauseFlag == 0 || pauseFlag == 1);

        // On 1.02e we additionally validate that the primary side buffer points to one of the known
        // base blocks embedded in the Practice struct. These offsets do not hold across h/i, so skip there.
        if (ver == EfzRevivalVersion::Revival102e) {
            uintptr_t primary = 0;
            (void)SafeReadMemory(cand + PRACTICE_OFF_SIDE_BUF_PRIMARY, &primary, sizeof(primary));
            bool bufOk = (primary == (cand + PRACTICE_OFF_BUF_LOCAL_BASE)) || (primary == (cand + PRACTICE_OFF_BUF_REMOTE_BASE));
            return sideOk && pauseOk && bufOk;
        } else {
            // For 1.02h/1.02i, rely on the basic invariants and optionally cross-check GUI_POS when present (0 or 1)
            uint8_t guiPos = 0xFF;
            (void)SafeReadMemory(cand + PRACTICE_OFF_GUI_POS, &guiPos, sizeof(guiPos));
            bool guiOk = (guiPos == 0 || guiPos == 1);
            return sideOk && (pauseOk || guiOk);
        }
    }

    bool TryResolvePracticePtrFromModeArray(void*& outPtr) {
        // Note: EfzRevival's EFZ_GameMode_GetStructByIndex returns engine mode objects
        // (e.g., battle context at idx=3), not the Practice controller. We therefore
        // do NOT use the game mode array to find Practice. Keep only the direct static
        // pointer fast-path; otherwise rely on lightweight hooks to capture ECX.
        HMODULE hRev = GetModuleHandleA("EfzRevival.dll");
        if (!hRev) return false;
        auto base = reinterpret_cast<uintptr_t>(hRev);

        // For all versions (e/h/i), prefer the direct static pointer (CheatEngine found all three)
        EfzRevivalVersion ver = GetEfzRevivalVersion();
        uintptr_t ptrRva = EFZ_RVA_PracticeControllerPtr();
        if (ptrRva) {
            uintptr_t ptrAddr = base + ptrRva;
            uintptr_t cand = 0;
            if (SafeReadMemory(ptrAddr, &cand, sizeof(cand)) && cand) {
                if (ValidatePracticeCandidate(cand)) {
                    outPtr = reinterpret_cast<void*>(cand);
                    std::ostringstream oss; oss << "[PAUSE] Direct ptr @+0x" << std::hex << ptrRva << " resolved practice=0x" << cand;
                    LogOut(oss.str(), detailedLogging.load());
                    return true;
                }
            }
        }
        return false;
    }

    // Heuristic scan: iterate a small range of mode slots to find a struct that looks like Practice by invariants.
    bool TryResolvePracticePtrByScan(void*& outPtr) {
        // Gate scanning strictly: Practice mode only, offline only, and never during Character Select
        if (IsInCharacterSelectScreen()) {
            return false;
        }
        GameMode mode = GetCurrentGameMode();
        if (mode != GameMode::Practice) {
            return false;
        }
        if (DetectOnlineMatch() || isOnlineMatch.load(std::memory_order_relaxed)) {
            return false;
        }
        // If we've already failed once during the current Character Select, skip further scans/logs
        if (IsInCharacterSelectScreen() && s_scanSuppressedThisCS.load(std::memory_order_relaxed)) {
            return false;
        }
        HMODULE hRev = GetModuleHandleA("EfzRevival.dll");
        if (!hRev) return false;
        auto base = reinterpret_cast<uintptr_t>(hRev);
        uintptr_t gmArrayRva = EFZ_RVA_GameModePtrArray();
        if (!gmArrayRva) return false;
        for (int idx = 0; idx < 16; ++idx) {
            uintptr_t slotAddr = base + gmArrayRva + 4 * idx;
            uintptr_t cand = 0;
            if (!SafeReadMemory(slotAddr, &cand, sizeof(cand)) || !cand) continue;
            if (ValidatePracticeCandidate(cand)) {
                outPtr = reinterpret_cast<void*>(cand);
                std::ostringstream oss; oss << "[PAUSE] Heuristic scan resolved practice=0x" << std::hex << cand << " (slot=" << std::dec << idx << ")";
                LogOut(oss.str(), detailedLogging.load());
                return true;
            }
        }
        // Log failure only once per Character Select instance, and only once globally overall
        bool firstThisCS = !s_scanSuppressedThisCS.exchange(true, std::memory_order_relaxed);
        if (firstThisCS) {
            if (!s_loggedScanFailOnce.exchange(true, std::memory_order_relaxed)) {
                LogOut("[PAUSE] Heuristic scan failed to resolve Practice controller", detailedLogging.load());
            }
        }
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
            LogOut(oss.str(), detailedLogging.load());
        }
        if (s_menuVisible.load(std::memory_order_relaxed) && !s_internalPauseBypass.load(std::memory_order_relaxed)) {
            // Suppress user-initiated pause/unpause while menu open
            return 0;
        }
        return oTogglePause ? oTogglePause(thisPtr) : 0;
    }

    static inline void UpdateCsCycleState() {
        const bool cs = IsInCharacterSelectScreen();
        if (cs) {
            // New CS entry: reset suppression so we try once per CS
            if (!s_csActive.exchange(true, std::memory_order_relaxed)) {
                s_scanSuppressedThisCS.store(false, std::memory_order_relaxed);
            }
        } else {
            // Leaving CS
            s_csActive.store(false, std::memory_order_relaxed);
        }
    }

    void EnsurePracticePtrHookInstalled() {
        // Gate to Practice mode only (offline is checked in scanning functions)
        GameMode mode = GetCurrentGameMode();
        if (mode != GameMode::Practice) return;
        
        // Don't install hooks for unsupported Revival versions
        if (!IsEfzRevivalLoaded()) return;
        
        // Maintain CS cycle bookkeeping to bound scan/log spam to once per CS
        UpdateCsCycleState();
        // Attempt direct resolution first (fast path, no hooks needed once valid)
        if (!s_practicePtr.load()) {
            void* resolved = nullptr;
            if (TryResolvePracticePtrFromModeArray(resolved)) {
                s_practicePtr.store(resolved, std::memory_order_relaxed);
                LogOut("[PAUSE] Practice ptr resolved via mode array", true);
                // Once resolved, clear any prior CS suppression
                s_scanSuppressedThisCS.store(false, std::memory_order_relaxed);
            } else if (TryResolvePracticePtrByScan(resolved)) {
                s_practicePtr.store(resolved, std::memory_order_relaxed);
                LogOut("[PAUSE] Practice ptr resolved via heuristic scan", true);
                s_scanSuppressedThisCS.store(false, std::memory_order_relaxed);
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
            LogOut(oss.str(), detailedLogging.load());
        }
        bool anyHook = false;
        // Select the correct PracticeTick hook based on version/signature
        if (tickTarget) {
            EfzRevivalVersion ver = GetEfzRevivalVersion();
            if (IsEfzRevivalLoaded() && ver == EfzRevivalVersion::Revival102h) {
                if (MH_CreateHook(tickTarget, &HookedPracticeTickH, reinterpret_cast<void**>(&oPracticeTickH)) == MH_OK && MH_EnableHook(tickTarget) == MH_OK) {
                    anyHook = true; LogOut("[PAUSE] PracticeTick hook active (1.02h)", detailedLogging.load());
                }
            } else {
                if (MH_CreateHook(tickTarget, &HookedPracticeTickE, reinterpret_cast<void**>(&oPracticeTickE)) == MH_OK && MH_EnableHook(tickTarget) == MH_OK) {
                    anyHook = true; LogOut("[PAUSE] PracticeTick hook active (1.02e/1.02i)", detailedLogging.load());
                }
            }
        }
        if (pauseTarget && MH_CreateHook(pauseTarget, &HookedTogglePause, reinterpret_cast<void**>(&oTogglePause)) == MH_OK && MH_EnableHook(pauseTarget) == MH_OK) {
            anyHook = true; LogOut("[PAUSE] TogglePause hook active", detailedLogging.load());
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

    // Hook battle screen render to capture battleContext pointer (legacy path).
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

    // Revival uses EFZ_GameMode_GetStructByIndex(3) to fetch the battle context. Mirror that here
    // to avoid fragile render hooks in vanilla.
    static bool RefreshBattleContextFromGameModeArray() {
        uintptr_t efzBase = GetEFZBase(); if (!efzBase) return false;
        constexpr uintptr_t RVA_GameModeArray = 0x00790110; // efz.exe absolute RVA
        void* bc = nullptr;
        uintptr_t slot3 = efzBase + RVA_GameModeArray + 4 * 3; // index 3 = battle context
        if (!SafeReadMemory(slot3, &bc, sizeof(bc)) || !bc) return false;
        s_battleContext.store(bc, std::memory_order_relaxed);
        return true;
    }

    // Resolve the gamespeed byte address from efz.exe GameMode array.
    // Cheat table pointers confirm three equivalent paths ending at the same address:
    //   *(efz+0x390114) + 0x0F20 -> byte
    //   *(efz+0x390118) + 0x09B8 -> byte
    //   *(efz+0x39011C) + 0x0578 -> byte (slot 3 = battle)
    static bool ResolveGamespeedAddr() {
        if (s_gamespeedAddr.load()) return true;
        uintptr_t efzBase = GetEFZBase(); if (!efzBase) return false;
        constexpr uintptr_t RVA_GameModeArray = 0x00390110; // same as 0x00790110 RVA; we add base
        struct Path { int idx; uintptr_t off; } paths[] = {
            { 1, 0x0F20 },
            { 2, 0x09B8 },
            { 3, 0x0578 },
        };
        for (const auto &p : paths) {
            uintptr_t slot = efzBase + RVA_GameModeArray + 4u * (uintptr_t)p.idx;
            uintptr_t basePtr = 0;
            if (!SafeReadMemory(slot, &basePtr, sizeof(basePtr)) || !basePtr) continue;
            uintptr_t cand = basePtr + p.off;
            uint8_t probe = 0xFF;
            if (SafeReadMemory(cand, &probe, sizeof(probe))) {
                // Valid speeds observed in engine: 0 (frozen), 3 (normal)
                if (probe <= 3) {
                    s_gamespeedAddr.store(cand, std::memory_order_relaxed);
                    std::ostringstream oss; oss << "[PAUSE][ADDR] Gamespeed addr=0x" << std::hex << cand
                        << " via slot=" << std::dec << p.idx << " off=0x" << std::hex << p.off
                        << " val=" << std::dec << (int)probe;
                    LogOut(oss.str(), true);
                    return true;
                }
            }
        }
        LogOut("[PAUSE][ADDR] Failed to resolve gamespeed address from GameMode array", true);
        return false;
    }

    void EnsureBattleContextHook() {
        static std::atomic<bool> installed{false};
        static std::atomic<bool> loggedFail{false};
        if (installed.load()) return;
        // RenderBattleScreen lives in efz.exe, not EfzRevival.dll
        uintptr_t efzBase = GetEFZBase();
        if (!efzBase) return;
        uintptr_t rva = EFZ_RVA_RenderBattleScreen();
        void* target = rva ? reinterpret_cast<void*>(efzBase + rva) : nullptr;
        if (!target) return;
        auto rcCreate = MH_CreateHook(target, &HookedRenderBattleScreen, reinterpret_cast<void**>(&oRenderBattleScreen));
        if (rcCreate != MH_OK && rcCreate != MH_ERROR_ALREADY_CREATED) {
            if (!loggedFail.exchange(true)) {
                std::ostringstream oss; oss << "[PAUSE] Failed to create RenderBattleScreen hook at VA=0x" << std::hex << (uintptr_t)target;
                LogOut(oss.str(), true);
            }
            return;
        }
        auto rcEnable = MH_EnableHook(target);
        if (rcEnable != MH_OK && rcEnable != MH_ERROR_ENABLED) {
            if (!loggedFail.exchange(true)) {
                std::ostringstream oss; oss << "[PAUSE] Failed to enable RenderBattleScreen hook at VA=0x" << std::hex << (uintptr_t)target;
                LogOut(oss.str(), true);
            }
            // Don't remove the hook if it was already created; just bail
            return;
        }
    installed.store(true);
    LogOut("[PAUSE] RenderBattleScreen hook installed (capturing battleContext)", detailedLogging.load());
    }

    // Read/write engine pause flag from battleContext + 0x1416 (int, toggled by in-engine input)
    bool ReadEnginePauseFlag(bool &outPaused) {
        void* bc = s_battleContext.load(); if (!bc) return false;
        uint32_t v = 0; if (!SafeReadMemory(reinterpret_cast<uintptr_t>(bc) + 0x1416, &v, sizeof(v))) return false;
        outPaused = (v != 0); return true;
    }
    bool WriteEnginePauseFlag(bool paused) {
        void* bc = s_battleContext.load(); if (!bc) return false;
        uint32_t v = paused ? 1u : 0u;
        uintptr_t addr = reinterpret_cast<uintptr_t>(bc) + 0x1416;
        bool ok = SafeWriteMemory(addr, &v, sizeof(v));
        std::ostringstream oss; oss << "[PAUSE][VANILLA] WriteEnginePause battleContext+0x1416 at 0x" << std::hex << addr
            << ": " << std::dec << (paused?1:0) << (ok?" (OK)":" (FAIL)");
        LogOut(oss.str(), true);
        return ok;
    }

    // Read/write game speed via CE-confirmed slot address (preferred), fallback to legacy bc+0x1400 if unresolved
    bool ReadGamespeed(uint8_t &out) {
        // Try direct resolved address first
        uintptr_t addr = s_gamespeedAddr.load();
        if (addr || ResolveGamespeedAddr()) {
            addr = s_gamespeedAddr.load();
            if (addr && SafeReadMemory(addr, &out, sizeof(out))) return true;
        }
        // Legacy: battleContext + 0x1400 (kept as best-effort fallback)
        void* bc = s_battleContext.load(); if (!bc) return false;
        return SafeReadMemory(reinterpret_cast<uintptr_t>(bc) + 0x1400, &out, sizeof(out));
    }
    bool WriteGamespeed(uint8_t v) {
        // Try direct resolved address first
        uintptr_t addr = s_gamespeedAddr.load();
        if (!addr && !ResolveGamespeedAddr()) {
            // Legacy fallback: battleContext + 0x1400
            void* bc = s_battleContext.load(); if (!bc) return false;
            addr = reinterpret_cast<uintptr_t>(bc) + 0x1400;
            bool ok = SafeWriteMemory(addr, &v, sizeof(v));
            std::ostringstream oss; oss << "[PAUSE] WriteGamespeed legacy bc+0x1400 at 0x" << std::hex << addr
                << ": " << std::dec << (int)v << (ok ? " (OK)" : " (FAIL)");
            LogOut(oss.str(), true);
            return ok;
        }
        addr = s_gamespeedAddr.load();
        bool ok = addr ? SafeWriteMemory(addr, &v, sizeof(v)) : false;
        std::ostringstream oss; oss << "[PAUSE] WriteGamespeed slot-addr 0x" << std::hex << addr
            << ": " << std::dec << (int)v << (ok ? " (OK)" : " (FAIL)");
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

    // Apply visual effect freeze patches (NOPs out animation/effect update CALLs in efz.exe)
    // This mirrors Revival's sub_1006B2A0 behavior for unsupported versions
    bool ApplyVisualEffectPatches(bool freeze) {
        HMODULE hEfz = GetModuleHandleA("efz.exe");
        if (!hEfz) return false;
        uintptr_t base = reinterpret_cast<uintptr_t>(hEfz);
        
        // Patch addresses from Revival sub_1006B2A0 (these are RVAs from efz.exe base 0x400000)
        // When freezing, write NOPs; when unfreezing, restore original bytes
        struct PatchEntry {
            uintptr_t rva;
            SIZE_T size;
            BYTE nops[5];
            BYTE original[5];
        };
        
        static PatchEntry patches[] = {
            {0x36425F, 5, {0x90,0x90,0x90,0x90,0x90}, {}}, // Character rendering update
            {0x35E183, 3, {0x90,0x90,0x90}, {}},             // Animation frame update
            {0x35DFB5, 3, {0x90,0x90,0x90}, {}},             // Effect processing
            {0x35DFD0, 3, {0x90,0x90,0x90}, {}},             // Effect processing
            {0x35E055, 3, {0x90,0x90,0x90}, {}},             // Animation update
            {0x35E0DA, 3, {0x90,0x90,0x90}, {}},             // Animation update
            {0x36420A, 5, {0x90,0x90,0x90,0x90,0x90}, {}},   // Rendering update
            {0x364AEB, 3, {0x90,0x90,0x90}, {}},             // Effect update
            {0x365E59, 5, {0x90,0x90,0x90,0x90,0x90}, {}},   // Animation update
            {0x365E7A, 5, {0x90,0x90,0x90,0x90,0x90}, {}}    // Animation update
        };
        
        static bool originalsSaved = false;
        
        // On first freeze, save original bytes
        if (freeze && !originalsSaved) {
            for (auto& patch : patches) {
                uintptr_t addr = base + patch.rva;
                if (!SafeReadMemory(addr, patch.original, patch.size)) {
                    LogOut("[PAUSE][VANILLA] Failed to read original bytes for visual effect patch", true);
                    return false;
                }
            }
            originalsSaved = true;
        }
        
        // Apply or restore patches
        int successCount = 0;
        for (auto& patch : patches) {
            uintptr_t addr = base + patch.rva;
            const BYTE* data = freeze ? patch.nops : patch.original;
            
            DWORD oldProtect;
            if (!VirtualProtect((void*)addr, patch.size, PAGE_EXECUTE_READWRITE, &oldProtect)) {
                continue;
            }
            
            memcpy((void*)addr, data, patch.size);
            FlushInstructionCache(GetCurrentProcess(), (void*)addr, patch.size);
            
            DWORD dummy;
            VirtualProtect((void*)addr, patch.size, oldProtect, &dummy);
            
            successCount++;
        }
        
        if (successCount == _countof(patches)) {
            LogOut(freeze ? "[PAUSE][VANILLA] Visual effect patches applied (10 CALLs NOPed)" 
                          : "[PAUSE][VANILLA] Visual effect patches restored", true);
            return true;
        } else {
            std::ostringstream oss;
            oss << "[PAUSE][VANILLA] Partial visual effect patch: " << successCount << "/" << _countof(patches);
            LogOut(oss.str(), true);
            return false;
        }
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
    void EnsurePracticePointerCapture() {
        EnsurePracticePtrHookInstalled();
        // Prefer direct mode-array resolution for battleContext (Revival-compatible). Fallback to render hook.
        if (!RefreshBattleContextFromGameModeArray()) {
            EnsureBattleContextHook();
        }
    }
    void* GetPracticeControllerPtr() { return s_practicePtr.load(); }

    bool IsPracticePaused() { bool p=false; if (ReadPracticePauseFlag(p)) return p; return false; }
    bool __cdecl IsGameSpeedFrozen() { uint8_t v=3; if (ReadGamespeed(v)) return v==0; return false; }
    bool IsPausedOrFrozen() { return IsPracticePaused() || IsGameSpeedFrozen(); }

    // Persistent pause enforcement: keep game frozen regardless of external unpause attempts
    // ONLY for unsupported Revival versions - vanilla and supported versions handle pause correctly
    void MaintainFreezeWhileMenuVisible() {
        if (!s_menuVisible.load()) return;
        
        // Check if we're dealing with an unsupported Revival version
        HMODULE hRev = GetModuleHandleA("EfzRevival.dll");
        const bool unsupportedRevival = (hRev != nullptr) && !IsEfzRevivalVersionSupported();
        
        // Only enforce persistent pause for unsupported Revival versions
        if (unsupportedRevival) {
            // Always enforce gamespeed freeze while menu is visible
            uint8_t cur=3;
            if (ReadGamespeed(cur) && cur!=0) {
                WriteGamespeed(0);
            }
            
            // Always re-apply visual effect patches if we own them
            if (s_weAppliedVisualPatches.load()) {
                ApplyVisualEffectPatches(true);
            }
        }
        
        // Keep vanilla engine pause asserted if we set it (vanilla only)
        if (s_weVanillaEnginePause.load()) {
            bool paused=false; if (ReadEnginePauseFlag(paused) && !paused) { WriteEnginePauseFlag(true); }
        }
        
        // Keep practice pause flag set if we own it (fallback paths)
        if (s_weForcedFlagPause.load()) {
            bool p=false; if (ReadPracticePauseFlag(p) && !p) WritePracticePauseFlag(true);
        }
    }

    void OnMenuVisibilityChanged(bool visible) {
        s_menuVisible.store(visible);
        EnsurePracticePtrHookInstalled();
        GameMode mode = GetCurrentGameMode();
        const bool inPractice = (mode == GameMode::Practice);
        // For unsupported Revival versions, treat as vanilla
        const bool revivalLoaded = (GetModuleHandleA("EfzRevival.dll") != nullptr) && IsEfzRevivalVersionSupported();
        std::ostringstream log; log << "[PAUSE] Menu=" << (visible?"open":"close") << " practice=" << (inPractice?1:0)
            << " prx=0x" << std::hex << (uintptr_t)s_practicePtr.load();
        LogOut(log.str(), true);

        if (visible) {
            // CLEAR previous ownership (defensive) – we re-detect each open.
            s_weUsedOfficialToggle.store(false);
            s_weForcedFlagPause.store(false);
            s_weAppliedPatchFreeze.store(false);
            s_weFrozeGamespeed.store(false);
            s_weVanillaEnginePause.store(false);

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
            // Vanilla path: if EfzRevival is NOT loaded, mirror Revival by freezing gamespeed AND visual effects
            // Revival's pause (sub_1006B2A0) does TWO things:
            //   1. Sets battleContext+0x1400 (gamespeed) to 0
            //   2. NOPs out 10 CALL instructions to animation/effect update functions in efz.exe
            // For unsupported Revival versions, we replicate BOTH to fully freeze gameplay and visuals.
            if (!revivalLoaded) {
                // Try to resolve battleContext via mode array like Revival
                if (!RefreshBattleContextFromGameModeArray()) {
                    EnsureBattleContextHook();
                }
                
                bool appliedAny = false;
                
                // Step 1: Freeze gamespeed
                uint8_t cur=3;
                if (ReadGamespeed(cur)) {
                    // Only freeze if gamespeed is at default value (3) to avoid overwriting user-set or already-frozen values
                    if (cur == 3 && WriteGamespeed(0)) {
                        s_prevGamespeed.store(cur);
                        s_weFrozeGamespeed.store(true);
                        appliedAny = true;
                    } else if (cur != 3 && cur != 0) {
                        LogOut("[PAUSE][VANILLA] Gamespeed already modified (not default), skipping freeze", true);
                        return; // don't touch non-default gamespeed
                    }
                }
                
                // Step 2: Apply visual effect patches (NOP out animation/effect CALLs)
                if (ApplyVisualEffectPatches(true)) {
                    s_weAppliedVisualPatches.store(true);
                    appliedAny = true;
                }
                
                if (appliedAny) {
                    LogOut("[PAUSE][VANILLA] Full pause applied: gamespeed frozen + visual effects frozen", true);
                    return;
                } else {
                    LogOut("[PAUSE][VANILLA] Failed to apply full pause", true);
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
            if (s_weAppliedVisualPatches.load()) {
                ApplyVisualEffectPatches(false); // Restore original bytes
                s_weAppliedVisualPatches.store(false);
            }
            if (s_weVanillaEnginePause.load()) {
                bool paused=false; if (ReadEnginePauseFlag(paused) && paused) WriteEnginePauseFlag(false);
                s_weVanillaEnginePause.store(false);
                LogOut("[PAUSE][VANILLA] Engine pause cleared on menu close", true);
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
