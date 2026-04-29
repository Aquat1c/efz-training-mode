#include "../../include/input/framestep.h"
#include "../../include/core/logger.h"
#include "../../include/game/efzrevival_addrs.h"
#include "../../include/game/game_state.h"
#include "../../include/game/practice_hotkey_gate.h"
#include "../../include/utils/pause_integration.h"
#include "../../include/core/memory.h"
#include "../../include/utils/network.h"
#include "../../include/utils/utilities.h"
#include "../../include/utils/config.h"
#include "../../include/gui/overlay.h"
#include "../../include/gui/imgui_impl.h"
#include <windows.h>
#include <atomic>
#include <sstream>

// External frame counter (incremented at 192fps in frame_monitor.cpp)
extern std::atomic<int> frameCounter;
// External window active state (from utilities.cpp)
extern std::atomic<bool> g_efzWindowActive;

namespace {
    // State tracking
    std::atomic<bool> s_enabled{false};
    std::atomic<bool> s_paused{false};
    std::atomic<bool> s_stepRequested{false};
    std::atomic<uint32_t> s_stepCounter{0};
    std::atomic<bool> s_inFrameStep{false};
    std::atomic<bool> s_visualPatchesApplied{false};
    std::atomic<bool> s_wePaused{false};
    std::atomic<uintptr_t> s_lastBattleContext{0};
    
    // Frame advance tracking
    std::atomic<int> s_targetFrame{0};        // Target frame to reach
    std::atomic<int> s_stepStartFrame{0};     // Frame when step started
    std::atomic<Framestep::StepMode> s_stepMode{Framestep::StepMode::FullFrame};
    std::atomic<int> s_pendingVanillaSubsteps{0};
    std::atomic<bool> s_vanillaHookStepActive{false};
    std::atomic<bool> s_vanillaStepInBattleUpdate{false};
    std::atomic<uint8_t> s_vanillaStepSpeed{0};
    // Revival: substeps still to request after the currently armed step flag is consumed.
    std::atomic<int> s_pendingRevivalSubsteps{0};
    std::atomic<uint32_t> s_lastRevivalStepCounter{0};
    std::atomic<bool> s_haveRevivalStepCounter{false};
    std::atomic<bool> s_revivalHotkeyGateReady{false};
    std::atomic<DWORD> s_nextHotkeyGateAttemptTick{0};
    std::atomic<bool> s_loggedHotkeyGateUnavailable{false};

    constexpr uintptr_t kBattleGamespeedOffset = 1400;
    constexpr uintptr_t kBattleEnginePauseOffset = 1416;

    enum class Backend {
        Disabled,
        Vanilla,
        Revival102fSubframe
    };

    Backend GetBackend() {
        HMODULE hRev = GetModuleHandleA("EfzRevival.dll");
        if (!hRev) {
            return Backend::Vanilla;
        }

        return IsEfzRevival102fSubframeBuild()
            ? Backend::Revival102fSubframe
            : Backend::Disabled;
    }

    bool IsConfigEnabled() {
        return Config::GetSettings().framestepEnabled;
    }

    bool ShouldSuppressNativeRevivalFramestep() {
        return Config::GetSettings().suppressRevivalFramestep;
    }

    bool IsRuntimeEnabled() {
        return s_enabled.load() && IsConfigEnabled() && GetBackend() != Backend::Disabled;
    }

    const char* BackendName(Backend backend) {
        switch (backend) {
        case Backend::Vanilla: return "vanilla";
        case Backend::Revival102fSubframe: return "Revival 1.02f subframe";
        default: return "disabled";
        }
    }

    bool IsValidFramestepMode() {
        GameMode mode = GetCurrentGameMode();
        Config::Settings cfg = Config::GetSettings();
        return !cfg.restrictToPracticeMode || (mode == GameMode::Practice);
    }

    void RemoveStatusMessage() {
        if (g_FramestepStatusId != -1) {
            DirectDrawHook::RemovePermanentMessage(g_FramestepStatusId);
            g_FramestepStatusId = -1;
        }
    }

    void ResetRevivalStepTracking() {
        s_pendingRevivalSubsteps.store(0);
        s_inFrameStep.store(false);
        s_haveRevivalStepCounter.store(false);
    }

    void ResetVanillaStepTracking() {
        s_pendingVanillaSubsteps.store(0);
        s_vanillaHookStepActive.store(false);
        s_vanillaStepInBattleUpdate.store(false);
        s_vanillaStepSpeed.store(0);
        s_inFrameStep.store(false);
    }

    bool EnsureRevivalHotkeyGate() {
        if (s_revivalHotkeyGateReady.load()) {
            return true;
        }
        if (GetBackend() != Backend::Revival102fSubframe) {
            return false;
        }

        const DWORD now = GetTickCount();
        const DWORD nextAttempt = s_nextHotkeyGateAttemptTick.load();
        if (nextAttempt != 0 && static_cast<LONG>(now - nextAttempt) < 0) {
            return false;
        }
        s_nextHotkeyGateAttemptTick.store(now + 1000);

        if (PracticeHotkeyGate::Install()) {
            s_revivalHotkeyGateReady.store(true);
            s_nextHotkeyGateAttemptTick.store(0);
            s_loggedHotkeyGateUnavailable.store(false);
            LogOut("[FRAMESTEP][REVIVAL] Practice hotkey gate ready", true);
            return true;
        }
        return false;
    }

    void LogHotkeyGateUnavailableOnce(const char* keyName) {
        if (s_loggedHotkeyGateUnavailable.exchange(true)) {
            return;
        }

        std::ostringstream oss;
        oss << "[FRAMESTEP][REVIVAL] Native " << keyName
            << " key overlaps our framestep key, but the hotkey gate is unavailable; using our framestep anyway";
        LogOut(oss.str(), true);
    }

    // Visual effect patches (same as pause_integration.cpp)
    struct PatchEntry {
        uintptr_t rva;
        SIZE_T size;
        BYTE nops[5];
        BYTE original[5];
    };

    static PatchEntry s_patches[] = {
        {0x36425F, 5, {0x90,0x90,0x90,0x90,0x90}, {}},
        {0x35E183, 3, {0x90,0x90,0x90}, {}},
        {0x35DFB5, 3, {0x90,0x90,0x90}, {}},
        {0x35DFD0, 3, {0x90,0x90,0x90}, {}},
        {0x35E055, 3, {0x90,0x90,0x90}, {}},
        {0x35E0DA, 3, {0x90,0x90,0x90}, {}},
        {0x36420A, 5, {0x90,0x90,0x90,0x90,0x90}, {}},
        {0x364AEB, 3, {0x90,0x90,0x90}, {}},
        {0x365E59, 5, {0x90,0x90,0x90,0x90,0x90}, {}},
        {0x365E7A, 5, {0x90,0x90,0x90,0x90,0x90}, {}}
    };

    static bool s_originalsSaved = false;

    // Apply or restore visual effect patches
    bool ApplyVisualPatches(bool freeze) {
        HMODULE hEfz = GetModuleHandleA("efz.exe");
        if (!hEfz) return false;
        uintptr_t base = reinterpret_cast<uintptr_t>(hEfz);

        // Save originals on first freeze
        if (freeze && !s_originalsSaved) {
            for (auto& patch : s_patches) {
                uintptr_t addr = base + patch.rva;
                if (!SafeReadMemory(addr, patch.original, patch.size)) {
                    LogOut("[FRAMESTEP] Failed to save original bytes", true);
                    return false;
                }
            }
            s_originalsSaved = true;
        }

        // Apply or restore
        int successCount = 0;
        for (auto& patch : s_patches) {
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

        return successCount == _countof(s_patches);
    }

    bool ReadBattleGamespeed(uintptr_t battleContext, uint8_t& outValue) {
        if (!battleContext) return false;
        return SafeReadMemory(battleContext + kBattleGamespeedOffset, &outValue, sizeof(outValue));
    }

    bool WriteBattleGamespeed(uintptr_t battleContext, uint8_t value) {
        if (!battleContext) return false;
        return SafeWriteMemory(battleContext + kBattleGamespeedOffset, &value, sizeof(value));
    }

    // Get gamespeed address (from captured battle context or efz.exe GameMode array)
    uintptr_t GetGamespeedAddress() {
        uintptr_t lastBattle = s_lastBattleContext.load();
        uint8_t probe = 0xFF;
        if (ReadBattleGamespeed(lastBattle, probe) && probe <= 3) {
            return lastBattle + kBattleGamespeedOffset;
        }

        uintptr_t efzBase = GetEFZBase();
        if (!efzBase) return 0;

        constexpr uintptr_t RVA_GameModeArray = 0x00390110;
        
        // Try slot 3 (battle context) + 0x0578
        uintptr_t slot3 = efzBase + RVA_GameModeArray + 4 * 3;
        uintptr_t basePtr = 0;
        if (!SafeReadMemory(slot3, &basePtr, sizeof(basePtr)) || !basePtr) {
            return 0;
        }

        uintptr_t addr = basePtr + 0x0578;
        probe = 0xFF;
        if (SafeReadMemory(addr, &probe, sizeof(probe)) && probe <= 3) {
            return addr;
        }

        return 0;
    }

    // Set gamespeed (0 = paused, 3 = normal)
    bool SetGamespeed(uint8_t value) {
        uintptr_t addr = GetGamespeedAddress();
        if (!addr) return false;

        bool ok = SafeWriteMemory(addr, &value, sizeof(value));
        std::ostringstream oss;
        oss << "[FRAMESTEP] SetGamespeed(" << (int)value << ") at 0x" << std::hex << addr 
            << (ok ? " OK" : " FAIL");
        LogOut(oss.str(), true);
        return ok;
    }

    void ClearActiveFramestepState(Backend backend, const char* reason) {
        const bool hadState = s_stepRequested.load()
            || s_paused.load()
            || s_inFrameStep.load()
            || s_visualPatchesApplied.load()
            || s_wePaused.load()
            || s_pendingRevivalSubsteps.load() > 0
            || g_FramestepStatusId != -1;

        s_stepRequested.store(false);

        if (backend == Backend::Revival102fSubframe) {
            if (s_wePaused.load() && PauseIntegration::IsPracticePaused()) {
                PauseIntegration::SetPracticePausedForFramestep(false);
            }
            s_paused.store(false);
            s_wePaused.store(false);
            ResetRevivalStepTracking();
        } else {
            if (s_paused.load() || s_inFrameStep.load() || s_visualPatchesApplied.load()) {
                if (s_visualPatchesApplied.load()) {
                    ApplyVisualPatches(false);
                    s_visualPatchesApplied.store(false);
                }
                SetGamespeed(3);
            }
            s_paused.store(false);
            ResetVanillaStepTracking();
        }

        RemoveStatusMessage();
        if (hadState && detailedLogging.load() && reason) {
            std::ostringstream oss;
            oss << "[FRAMESTEP] Cleared active state: " << reason;
            LogOut(oss.str(), true);
        }
    }

    // Advance one frame (or multiple subframes based on step mode)
    void AdvanceOneFrame() {
        int subframes = static_cast<int>(s_stepMode.load());
        int currentFrame = frameCounter.load();
        s_stepStartFrame.store(currentFrame);
        s_targetFrame.store(currentFrame + subframes);

        // Temporarily unpause to allow frames to advance
        if (s_visualPatchesApplied.load()) {
            ApplyVisualPatches(false);
            s_visualPatchesApplied.store(false);
        }
        uint8_t speed = static_cast<uint8_t>(subframes);
        if (speed > 3) speed = 3;
        if (speed == 0) speed = 1;
        SetGamespeed(speed);

        std::stringstream oss;
        oss << "[FRAMESTEP] Advancing " << subframes << " subframe(s) from frame " 
            << currentFrame << " to " << s_targetFrame.load();
        LogOut(oss.str(), true);
    }

    // Check if frame advance is complete
    bool IsFrameAdvanceComplete() {
        if (!s_inFrameStep.load()) return true;
        
        int current = frameCounter.load();
        int target = s_targetFrame.load();
        
        if (current >= target) {
            // Frame advance complete - re-pause
            SetGamespeed(0);
            if (ApplyVisualPatches(true)) {
                s_visualPatchesApplied.store(true);
            }

            std::stringstream oss;
            oss << "[FRAMESTEP] Frame advance complete at frame " << current 
                << " (advanced " << (current - s_stepStartFrame.load()) << " subframes)";
            LogOut(oss.str(), true);

            s_inFrameStep.store(false);
            return true;
        }
        
        return false;
    }

    bool BeginVanillaHookStep() {
        const int subframes = Framestep::GetSubframesPerStep();
        if (subframes <= 0) {
            return false;
        }

        if (s_inFrameStep.exchange(true)) {
            return false;
        }

        s_stepRequested.store(false);
        s_stepStartFrame.store(frameCounter.load());
        s_pendingVanillaSubsteps.store(subframes);
        s_vanillaHookStepActive.store(true);
        s_vanillaStepInBattleUpdate.store(false);
        s_vanillaStepSpeed.store(0);

        std::ostringstream oss;
        oss << "[FRAMESTEP][VANILLA] Queued " << subframes
            << " subframe(s) for next battle update";
        LogOut(oss.str(), true);
        return true;
    }

    void OnBattleUpdateBefore(void* battleContext) {
        const uintptr_t battle = reinterpret_cast<uintptr_t>(battleContext);
        s_lastBattleContext.store(battle);

        if (!s_vanillaHookStepActive.load()
            || !s_inFrameStep.load()
            || !s_paused.load()
            || GetBackend() != Backend::Vanilla
            || GetCurrentGameMode() != GameMode::Practice
            || IsNetplaySuspendActive()) {
            return;
        }

        int pending = s_pendingVanillaSubsteps.load();
        if (pending <= 0) {
            return;
        }

        uint8_t speed = static_cast<uint8_t>(pending);
        if (speed > 3) speed = 3;
        if (speed == 0) speed = 1;

        if (s_visualPatchesApplied.load()) {
            ApplyVisualPatches(false);
            s_visualPatchesApplied.store(false);
        }

        uint32_t unpaused = 0;
        SafeWriteMemory(battle + kBattleEnginePauseOffset, &unpaused, sizeof(unpaused));

        if (WriteBattleGamespeed(battle, speed)) {
            s_vanillaStepSpeed.store(speed);
            s_vanillaStepInBattleUpdate.store(true);
        }
    }

    void OnBattleUpdateAfter(void* battleContext) {
        const uintptr_t battle = reinterpret_cast<uintptr_t>(battleContext);
        s_lastBattleContext.store(battle);

        if (!s_vanillaStepInBattleUpdate.exchange(false)) {
            return;
        }

        const uint8_t speed = s_vanillaStepSpeed.exchange(0);
        WriteBattleGamespeed(battle, 0);

        if (s_paused.load()) {
            if (ApplyVisualPatches(true)) {
                s_visualPatchesApplied.store(true);
            }
        }

        s_pendingVanillaSubsteps.store(0);
        s_vanillaHookStepActive.store(false);
        s_inFrameStep.store(false);
        s_stepCounter.fetch_add(1);

        std::ostringstream oss;
        oss << "[FRAMESTEP][VANILLA] Battle update consumed "
            << static_cast<int>(speed) << " subframe(s) (step "
            << s_stepCounter.load() << ")";
        LogOut(oss.str(), true);
    }

    bool ReadRevivalPracticeHotkey(uintptr_t offset, int& outKey) {
        outKey = 0;
        if (!offset || GetBackend() != Backend::Revival102fSubframe) {
            return false;
        }

        PauseIntegration::EnsurePracticePointerCapture();
        void* practice = PauseIntegration::ResolvePracticeControllerPtrNow(false, true, "framestep hotkey");
        if (!practice) {
            practice = PauseIntegration::GetPracticeControllerPtr();
        }
        if (!practice) {
            return false;
        }

        uint32_t key = 0;
        if (!SafeReadMemory(reinterpret_cast<uintptr_t>(practice) + offset, &key, sizeof(key))) {
            return false;
        }
        outKey = static_cast<int>(key);
        return key != 0;
    }

    bool ConfiguredKeyMatchesRevivalNative(bool stepKey) {
        const Config::Settings cfg = Config::GetSettings();
        const int configured = stepKey
            ? (cfg.framestepStepKey > 0 ? cfg.framestepStepKey : 'P')
            : (cfg.framestepPauseKey > 0 ? cfg.framestepPauseKey : VK_SPACE);

        int native = 0;
        const uintptr_t offset = stepKey
            ? EFZ_Practice_StepHotkeyOffset()
            : EFZ_Practice_PauseHotkeyOffset();
        if (ReadRevivalPracticeHotkey(offset, native)) {
            return configured == native;
        }

        // Before the Practice pointer is captured, prefer avoiding a double-toggle
        // on the stock Revival bindings. Custom bindings become active once the
        // pointer is available and we can compare against the game's own fields.
        const int defaultKey = stepKey ? 'P' : VK_SPACE;
        return configured == defaultKey;
    }

    void HandleRevivalStepDelta(uint32_t delta) {
        if (delta == 0) {
            return;
        }

        int observed = static_cast<int>(delta);
        if (observed < 0 || observed > 16) {
            observed = 1;
        }

        if (s_inFrameStep.load()) {
            int remaining = s_pendingRevivalSubsteps.load();
            if (observed > 1) {
                remaining -= (observed - 1);
                if (remaining < 0) {
                    remaining = 0;
                }
            }

            if (remaining > 0) {
                s_pendingRevivalSubsteps.store(remaining - 1);
                if (!PauseIntegration::RequestPracticeSubframeStep()) {
                    s_pendingRevivalSubsteps.store(0);
                    s_inFrameStep.store(false);
                }
            } else {
                s_pendingRevivalSubsteps.store(0);
                s_inFrameStep.store(false);
            }
            return;
        }

        // Native Revival steps can still happen if suppression is disabled.
        s_stepCounter.fetch_add(static_cast<uint32_t>(observed));
    }

    void OnPracticeStepAdvanced(uint32_t beforeCounter, uint32_t afterCounter) {
        if (!IsRuntimeEnabled() || GetBackend() != Backend::Revival102fSubframe) {
            return;
        }

        if (afterCounter < beforeCounter) {
            ResetRevivalStepTracking();
            s_haveRevivalStepCounter.store(true);
            s_lastRevivalStepCounter.store(afterCounter);
            return;
        }

        const uint32_t delta = afterCounter - beforeCounter;
        s_haveRevivalStepCounter.store(true);
        s_lastRevivalStepCounter.store(afterCounter);
        HandleRevivalStepDelta(delta);
    }

    bool QueueRevivalSubsteps(int subframes) {
        if (subframes <= 0) {
            return false;
        }

        // Native Revival has a single step-request bit. Matching that behavior
        // avoids coalescing fast repeat presses into a later 3-subframe burst.
        if (s_inFrameStep.exchange(true)) {
            return false;
        }

        uint32_t currentCounter = 0;
        if (PauseIntegration::ReadStepCounter(currentCounter)) {
            s_lastRevivalStepCounter.store(currentCounter);
            s_haveRevivalStepCounter.store(true);
        }

        s_stepCounter.fetch_add(1);
        s_pendingRevivalSubsteps.store(subframes - 1);
        if (PauseIntegration::RequestPracticeSubframeStep()) {
            s_inFrameStep.store(true);
            return true;
        }

        s_pendingRevivalSubsteps.store(0);
        if (s_stepCounter.load() > 0) {
            s_stepCounter.fetch_sub(1);
        }
        s_inFrameStep.store(false);
        return false;
    }

    void SyncRevivalFramestep() {
        PauseIntegration::EnsurePracticePointerCapture();
        const bool paused = PauseIntegration::IsPracticePaused();
        s_paused.store(paused);

        if (!paused) {
            ResetRevivalStepTracking();
            s_wePaused.store(false);
            return;
        }

        uint32_t currentCounter = 0;
        if (!PauseIntegration::ReadStepCounter(currentCounter)) {
            return;
        }

        if (!s_haveRevivalStepCounter.exchange(true)) {
            s_lastRevivalStepCounter.store(currentCounter);
            return;
        }

        const uint32_t lastCounter = s_lastRevivalStepCounter.load();
        if (currentCounter == lastCounter) {
            return;
        }
        if (currentCounter < lastCounter) {
            s_lastRevivalStepCounter.store(currentCounter);
            ResetRevivalStepTracking();
            s_haveRevivalStepCounter.store(true);
            return;
        }
        s_lastRevivalStepCounter.store(currentCounter);

        uint32_t delta = currentCounter - lastCounter;
        if (delta == 0) {
            delta = 1;
        }

        HandleRevivalStepDelta(delta);
    }
}

namespace Framestep {
    void Initialize() {
        s_enabled.store(true);
        SetBattleUpdateCallbacks(&OnBattleUpdateBefore, &OnBattleUpdateAfter);
        EnsureFrontendControlHooksInstalled();
        PauseIntegration::SetPracticeStepAdvanceCallback(&OnPracticeStepAdvanced);
        Backend backend = GetBackend();
        std::ostringstream oss;
        oss << "[FRAMESTEP] Initialized; backend=" << BackendName(backend);
        if (backend == Backend::Disabled && GetModuleHandleA("EfzRevival.dll")) {
            oss << " (unsupported Revival framestep)";
        }
        LogOut(oss.str(), true);
    }

    void Update() {
        if (!s_enabled.load()) {
            RemoveStatusMessage();
            return;
        }

        Backend backend = GetBackend();

        if (!IsConfigEnabled()) {
            ClearActiveFramestepState(backend, "disabled by config");
            return;
        }

        if (backend == Backend::Disabled) {
            ClearActiveFramestepState(backend, "backend unavailable");
            return;
        }

        if (backend == Backend::Revival102fSubframe && ShouldSuppressNativeRevivalFramestep()) {
            EnsureRevivalHotkeyGate();
        }

        // Check if game window is active
        if (!g_efzWindowActive.load()) {
            return;
        }

        // Check if ImGui menu is open (keys should be gated)
        if (ImGuiImpl::IsVisible()) {
            return;
        }

        if (!IsValidFramestepMode()) {
            // Reset state if we leave valid mode
            if (backend == Backend::Revival102fSubframe) {
                if (s_wePaused.load()) {
                    PauseIntegration::SetPracticePausedForFramestep(false);
                    s_wePaused.store(false);
                }
                s_paused.store(false);
                ResetRevivalStepTracking();
            } else if (s_paused.load()) {
                s_paused.store(false);
                if (s_visualPatchesApplied.load()) {
                    ApplyVisualPatches(false);
                    s_visualPatchesApplied.store(false);
                }
                SetGamespeed(3);
            }
            return;
        }

        // Don't interfere with online matches
        if (IsNetplaySuspendActive()) {
            return;
        }

        if (backend == Backend::Revival102fSubframe) {
            if (ShouldSuppressNativeRevivalFramestep()) {
                EnsureRevivalHotkeyGate();
            }
            SyncRevivalFramestep();
            return;
        }

        // Check if frame advance is in progress
        if (s_inFrameStep.load()) {
            if (!s_vanillaHookStepActive.load()) {
                IsFrameAdvanceComplete();
            }
            return;
        }

        // Process frame step if requested
        if (s_stepRequested.load() && s_paused.load()) {
            if (EnsureFrontendControlHooksInstalled() && BeginVanillaHookStep()) {
                return;
            }

            s_inFrameStep.store(true);
            s_stepCounter++;

            std::ostringstream oss;
            oss << "[FRAMESTEP] Frame step requested (fallback step " << s_stepCounter.load() << ")";
            LogOut(oss.str(), true);

            AdvanceOneFrame();
            s_stepRequested.store(false);
        }
    }

    bool IsPaused() {
        if (!IsRuntimeEnabled()) return false;
        if (GetBackend() == Backend::Revival102fSubframe) {
            if (ShouldSuppressNativeRevivalFramestep()) {
                EnsureRevivalHotkeyGate();
            }
            return PauseIntegration::IsPracticePaused();
        }
        return s_paused.load();
    }

    unsigned int GetStepCounter() {
        return s_stepCounter.load();
    }

    void ResetStepCounter() {
        s_stepCounter.store(0);
        if (GetBackend() == Backend::Revival102fSubframe) {
            ResetRevivalStepTracking();
        } else {
            ResetVanillaStepTracking();
        }
        LogOut("[FRAMESTEP] Step counter reset", true);
    }

    void TogglePause() {
        if (!IsRuntimeEnabled()) return;

        GameMode mode = GetCurrentGameMode();
        if (mode != GameMode::Practice) return;

        Backend backend = GetBackend();
        if (backend == Backend::Revival102fSubframe) {
            if (ShouldSuppressNativeRevivalFramestep()
                && ConfiguredKeyMatchesRevivalNative(false)
                && !EnsureRevivalHotkeyGate()) {
                LogHotkeyGateUnavailableOnce("pause");
            }

            const bool newState = !PauseIntegration::IsPracticePaused();
            if (PauseIntegration::SetPracticePausedForFramestep(newState)) {
                s_paused.store(newState);
                s_wePaused.store(newState);
                ResetRevivalStepTracking();
                if (newState) {
                    ResetStepCounter();
                    LogOut("[FRAMESTEP][REVIVAL] Paused", true);
                } else {
                    LogOut("[FRAMESTEP][REVIVAL] Unpaused", true);
                }
            }
            return;
        }

        bool newState = !s_paused.load();
        s_paused.store(newState);

        if (newState) {
            // Pausing
            SetGamespeed(0);
            if (ApplyVisualPatches(true)) {
                s_visualPatchesApplied.store(true);
            }
            ResetStepCounter();
            LogOut("[FRAMESTEP] Paused", true);
        } else {
            // Unpausing
            ResetVanillaStepTracking();
            if (s_visualPatchesApplied.load()) {
                ApplyVisualPatches(false);
                s_visualPatchesApplied.store(false);
            }
            SetGamespeed(3);
            LogOut("[FRAMESTEP] Unpaused", true);
        }

        // Clear any pending step request
        s_stepRequested.store(false);
    }

    void RequestFrameStep() {
        if (!IsRuntimeEnabled()) return;

        GameMode mode = GetCurrentGameMode();
        if (mode != GameMode::Practice) return;

        Backend backend = GetBackend();
        if (backend == Backend::Revival102fSubframe) {
            if (ShouldSuppressNativeRevivalFramestep()
                && ConfiguredKeyMatchesRevivalNative(true)
                && !EnsureRevivalHotkeyGate()) {
                LogHotkeyGateUnavailableOnce("step");
            }

            if (!PauseIntegration::IsPracticePaused()) {
                if (!PauseIntegration::SetPracticePausedForFramestep(true)) {
                    return;
                }
                s_paused.store(true);
                s_wePaused.store(true);
            }

            const int subframes = GetSubframesPerStep();
            if (QueueRevivalSubsteps(subframes)) {
                std::ostringstream oss;
                oss << "[FRAMESTEP][REVIVAL] Queued " << subframes << " subframe(s)";
                LogOut(oss.str(), true);
            }
            return;
        }

        // Ensure we're paused
        if (!s_paused.load()) {
            TogglePause();
        }

        // Request step
        s_stepRequested.store(true);
        LogOut("[FRAMESTEP] Frame step requested", true);
    }

    bool IsEnabled() {
        return IsRuntimeEnabled();
    }

    bool ShouldSuppressRevivalHotkey(void* practiceController, int key) {
        if (!practiceController || key == 0) return false;
        if (!IsRuntimeEnabled() || GetBackend() != Backend::Revival102fSubframe) return false;
        if (!ShouldSuppressNativeRevivalFramestep()) return false;
        if (GetCurrentGameMode() != GameMode::Practice || IsNetplaySuspendActive()) return false;

        uint32_t pauseKey = 0;
        uint32_t stepKey = 0;
        const uintptr_t base = reinterpret_cast<uintptr_t>(practiceController);
        const uintptr_t pauseOff = EFZ_Practice_PauseHotkeyOffset();
        const uintptr_t stepOff = EFZ_Practice_StepHotkeyOffset();
        if (pauseOff) {
            (void)SafeReadMemory(base + pauseOff, &pauseKey, sizeof(pauseKey));
        }
        if (stepOff) {
            (void)SafeReadMemory(base + stepOff, &stepKey, sizeof(stepKey));
        }

        const bool suppress = (pauseKey != 0 && key == static_cast<int>(pauseKey))
            || (stepKey != 0 && key == static_cast<int>(stepKey));
        if (suppress && detailedLogging.load()) {
            LogOut("[FRAMESTEP][REVIVAL] Suppressed native Revival pause/step hotkey", true);
        }
        return suppress;
    }

    void UpdateOverlayStatus() {
        if (!IsRuntimeEnabled()) {
            RemoveStatusMessage();
            return;
        }

        GameMode mode = GetCurrentGameMode();
        if (mode != GameMode::Practice) {
            RemoveStatusMessage();
            return;
        }

        const bool paused = IsPaused();
        s_paused.store(paused);

        // Only show message when paused
        if (!paused) {
            RemoveStatusMessage();
            return;
        }

        // Build status message (only when paused)
        std::string text = "Paused [Space=Resume P=Step]";
        COLORREF color = RGB(255, 100, 100); // Red for paused
        
        // Add step counter and mode info if we've stepped
        if (s_stepCounter.load() > 0) {
            text += " (" + std::to_string(s_stepCounter.load());
            
            // Show fractional frame count for subframe mode
            if (s_stepMode.load() == StepMode::Subframe) {
                float visualFrames = s_stepCounter.load() / 3.0f;
                char buf[32];
                snprintf(buf, sizeof(buf), " = %.2ff", visualFrames);
                text += buf;
            }
            text += ")";
        }

        // Position on right side, same Y as teleport messages (100)
        const int x = 540;
        const int y = 100;

        if (g_FramestepStatusId == -1) {
            g_FramestepStatusId = DirectDrawHook::AddPermanentMessage(text, color, x, y);
        } else {
            DirectDrawHook::RemovePermanentMessage(g_FramestepStatusId);
            g_FramestepStatusId = DirectDrawHook::AddPermanentMessage(text, color, x, y);
        }
    }

    StepMode GetStepMode() {
        return s_stepMode.load();
    }

    void SetStepMode(StepMode mode) {
        s_stepMode.store(mode);
        std::string modeStr = (mode == StepMode::Subframe) ? "Subframe (1 step = 192fps frame)" : "Full Frame (1 step = 64fps frame)";
        LogOut("[FRAMESTEP] Step mode changed to: " + modeStr, true);
    }

    int GetSubframesPerStep() {
        return s_stepMode.load() == StepMode::Subframe ? 1 : 3;
    }
}
