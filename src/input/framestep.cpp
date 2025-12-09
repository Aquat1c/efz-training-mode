#include "../../include/input/framestep.h"
#include "../../include/core/logger.h"
#include "../../include/game/game_state.h"
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
    
    // Frame advance tracking
    std::atomic<int> s_targetFrame{0};        // Target frame to reach
    std::atomic<int> s_stepStartFrame{0};     // Frame when step started
    std::atomic<Framestep::StepMode> s_stepMode{Framestep::StepMode::FullFrame};

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

    // Get gamespeed address (from efz.exe GameMode array)
    uintptr_t GetGamespeedAddress() {
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
        uint8_t probe = 0xFF;
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
        SetGamespeed(3);

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
}

namespace Framestep {
    void Initialize() {
        // Only enable for vanilla EFZ (no Revival)
        HMODULE hRev = GetModuleHandleA("EfzRevival.dll");
        s_enabled.store(hRev == nullptr);

        if (s_enabled.load()) {
            LogOut("[FRAMESTEP] Initialized for vanilla EFZ - framestep enabled", true);
        } else {
            LogOut("[FRAMESTEP] EfzRevival.dll detected - framestep disabled", true);
        }
    }

    void Update() {
        if (!s_enabled.load()) return;

        // Check if game window is active
        if (!g_efzWindowActive.load()) {
            return;
        }

        // Check if ImGui menu is open (keys should be gated)
        if (ImGuiImpl::IsVisible()) {
            return;
        }

        // Only work in Practice mode (or any mode if restrictToPracticeMode is off)
        GameMode mode = GetCurrentGameMode();
        Config::Settings cfg = Config::GetSettings();
        bool isValidMode = !cfg.restrictToPracticeMode || (mode == GameMode::Practice);
        
        if (!isValidMode) {
            // Reset state if we leave valid mode
            if (s_paused.load()) {
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
        if (DetectOnlineMatch() || isOnlineMatch.load()) {
            return;
        }

        // Check if frame advance is in progress
        if (s_inFrameStep.load()) {
            IsFrameAdvanceComplete();
        }

        // Process frame step if requested
        if (s_stepRequested.load() && s_paused.load() && !s_inFrameStep.load()) {
            s_inFrameStep.store(true);
            s_stepCounter++;
            
            std::ostringstream oss;
            oss << "[FRAMESTEP] Frame step requested (step " << s_stepCounter.load() << ")";
            LogOut(oss.str(), true);

            AdvanceOneFrame();
            s_stepRequested.store(false);
        }
    }

    bool IsPaused() {
        return s_enabled.load() && s_paused.load();
    }

    unsigned int GetStepCounter() {
        return s_stepCounter.load();
    }

    void ResetStepCounter() {
        s_stepCounter.store(0);
        LogOut("[FRAMESTEP] Step counter reset", true);
    }

    void TogglePause() {
        if (!s_enabled.load()) return;

        GameMode mode = GetCurrentGameMode();
        if (mode != GameMode::Practice) return;

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
        if (!s_enabled.load()) return;

        GameMode mode = GetCurrentGameMode();
        if (mode != GameMode::Practice) return;

        // Ensure we're paused
        if (!s_paused.load()) {
            TogglePause();
        }

        // Request step
        s_stepRequested.store(true);
        LogOut("[FRAMESTEP] Frame step requested", true);
    }

    bool IsEnabled() {
        return s_enabled.load();
    }

    void UpdateOverlayStatus() {
        if (!s_enabled.load()) {
            // Remove message if framestep is disabled
            if (g_FramestepStatusId != -1) {
                DirectDrawHook::RemovePermanentMessage(g_FramestepStatusId);
                g_FramestepStatusId = -1;
            }
            return;
        }

        GameMode mode = GetCurrentGameMode();
        if (mode != GameMode::Practice) {
            // Remove message if not in Practice mode
            if (g_FramestepStatusId != -1) {
                DirectDrawHook::RemovePermanentMessage(g_FramestepStatusId);
                g_FramestepStatusId = -1;
            }
            return;
        }

        // Only show message when paused
        if (!s_paused.load()) {
            // Remove message when not paused
            if (g_FramestepStatusId != -1) {
                DirectDrawHook::RemovePermanentMessage(g_FramestepStatusId);
                g_FramestepStatusId = -1;
            }
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
        return static_cast<int>(s_stepMode.load());
    }
}
