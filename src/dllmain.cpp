#include <fstream>
#include <iomanip>
#include <chrono>
#include <vector>
#include <windows.h>
#include <thread>
#include <timeapi.h>
#include "../include/core/memory.h"
#include "../include/utils/utilities.h"
#include "../include/input/input_buffer.h"
#include "../include/core/logger.h"
#include "../include/game/frame_monitor.h"
#include "../include/utils/network.h"
#include "../include/core/di_keycodes.h"
#include "../include/input/input_handler.h"
#include "../include/gui/overlay.h"
#include "../include/gui/imgui_impl.h"
#include "../include/gui/imgui_gui.h"
#include "../include/utils/config.h"
#include "../include/game/practice_patch.h"
#include "../include/game/final_memory_patch.h"
#include "../include/game/auto_action.h"  // ADD THIS INCLUDE
#include "../include/input/input_hook.h" // Add this include
#include "../3rdparty/minhook/include/MinHook.h" // Add this include
#include "../include/utils/bgm_control.h"
#include "../include/game/game_state.h"
#include "../include/core/globals.h"  // Add this include
#include "../include/game/collision_hook.h"
#include "../include/game/practice_hotkey_gate.h"
#include "../include/game/practice_offsets.h"
#include "../include/utils/debug_log.h"
#include "../include/game/efzrevival_addrs.h"
#include "../include/input/framestep.h"
// forward declaration for overlay gate
namespace PracticeOverlayGate { void EnsureInstalled(); void SetMenuVisible(bool); }
#pragma comment(lib, "winmm.lib")

// Forward declarations for functions in other files
void MonitorKeys();
void FrameDataMonitor();
void UpdateConsoleTitle();
void WriteStartupLog(const std::string& message);
extern std::atomic<bool> inStartupPhase;

// Add this declaration before it's used
void InitializeConfig();

// Define the global flags (remove 'static' if present)
extern std::atomic<bool> g_isShuttingDown;  // Reference the one defined in globals.cpp
std::atomic<bool> g_initialized(false);
std::atomic<bool> g_featuresEnabled(false);  // If this exists elsewhere, move it here

// Delayed initialization function
void DelayedInitialization(HMODULE hModule) {
    try {
        // Short delay to ensure the game has started properly
        Sleep(1500);

        WriteStartupLog("Starting delayed initialization");

        // Initialize logging system (starts title updater thread)
        WriteStartupLog("Initializing logging system...");
        InitializeLogging();
        WriteStartupLog("Logging system initialized");

        // Initialize configuration system first so we can gate file logging
        InitializeConfig();

    // Gate file debug logging behind dedicated config flag (separate from console verbosity)
    DebugLog::g_EnableDebugLog = Config::GetSettings().enableDebugFileLog;
        if (DebugLog::g_EnableDebugLog) {
            WriteStartupLog("Initializing debug log file (enabled by config)...");
        } else {
            WriteStartupLog("Debug log file disabled by config");
        }
        DebugLog::Initialize();
        WriteStartupLog("Debug log initialization step complete");

        // Create/hide console according to setting
        if (Config::GetSettings().enableConsole) {
            WriteStartupLog("Creating debug console as per settings...");
            CreateDebugConsole();
            if (HWND consoleWnd = GetConsoleWindow()) {
                ShowWindow(consoleWnd, SW_SHOW);
            }
        } else {
            // Ensure any inherited console is hidden; logs will be buffered
            if (HWND consoleWnd = GetConsoleWindow()) {
                ShowWindow(consoleWnd, SW_HIDE);
            }
            SetConsoleReady(false);
        }

        // Early gate: if online at startup, do NOT initialize hooks/threads/overlays
        // Leave the console (per settings) and exit initialization immediately.
        bool onlineAtStart = false;
        try {
            onlineAtStart = DetectOnlineMatch();
        } catch (...) {
            onlineAtStart = false; // be conservative; if unknown, continue
        }
        if (onlineAtStart) {
            LogOut("[SYSTEM] Online mode detected at startup; skipping hooks, threads, and overlays.", true);
            LogOut("[SYSTEM] Console state left as configured; no initialization will proceed while online.", true);
            inStartupPhase = false;
            return; // do not install hooks or start background workers
        }

        // Initialize MinHook once for the entire application.
        if (MH_Initialize() != MH_OK) {
            LogOut("[SYSTEM] MinHook initialization failed. Hooks will not be installed.", true);
            inStartupPhase = false; // ensure we don't stay stuck in startup state
            return; // Early exit if MinHook fails
        }
        LogOut("[SYSTEM] MinHook initialized successfully.", true);
        
        // Initialize framestep system (vanilla only)
        Framestep::Initialize();

        // Attempt to install Practice hotkey gate (will succeed only after EfzRevival.dll present)
        try {
            if (PracticeHotkeyGate::Install()) {
                LogOut("[HOTKEY] Practice hotkey gate active (menu suppression)", true);
            } else {
                LogOut("[HOTKEY] Practice hotkey gate not installed yet (EfzRevival may not be loaded)", true);
            }
            // Also install overlay toggle hooks (will silently do nothing if module not loaded yet)
            PracticeOverlayGate::EnsureInstalled();
        } catch (...) {
            LogOut("[HOTKEY] Exception while installing practice hotkey gate", true);
        }

        // Install hooks (with guards)
        try {
            InstallInputHook();
        } catch (...) {
            LogOut("[SYSTEM] Exception while installing input hook.", true);
        }
        try {
            InstallCollisionHook();
        } catch (...) {
            LogOut("[SYSTEM] Exception while installing collision hook.", true);
        }
        try {
            StartBGMSuppressionPoller();
        } catch (...) {
            LogOut("[SYSTEM] Exception while starting BGM suppression poller.", true);
        }

    // Final Memory HP bypass is now manual via Debug tab to avoid unintended changes.

        LogOut("[SYSTEM] EFZ Training Mode - Delayed initialization starting", true);
        LogOut("[SYSTEM] Console initialized with code page: " + std::to_string(GetConsoleOutputCP()), true);
        LogOut("[SYSTEM] Current locale: C", true);

    // Start essential threads.
    LogOut("[SYSTEM] Starting background threads...", true);
    // Note: UpdateConsoleTitle thread is already started by InitializeLogging(); don't start a duplicate here.
    std::thread(FrameDataMonitor).detach();
        LogOut("[SYSTEM] Essential background threads started.", true);

        // Use standard Windows input APIs instead of DirectInput
        WriteStartupLog("Using standard Windows input APIs instead of DirectInput");
        g_directInputAvailable = false;  // Ensure DirectInput is marked as unavailable

        WriteStartupLog("Reading key.ini file...");
        ReadKeyMappingsFromIni();
        WriteStartupLog("Key mappings read");

        LogOut("EFZ Training Mode initialized successfully", true);
        WriteStartupLog("Delayed initialization complete");

    // Initialize D3D9 hook for overlays once at startup (on a separate thread)
    std::thread([]{
            Sleep(2000); // Give the game a moment to be fully ready
            try {
        if (g_onlineModeActive.load()) return; // don't init if online already
                if (DirectDrawHook::InitializeD3D9()) {
                    LogOut("[SYSTEM] D3D9 Overlay system initialized.", true);
                } else {
                    LogOut("[SYSTEM] Failed to initialize D3D9 Overlay system.", true);
                    static bool s_warnedNoD3D9 = false;
                    if (!s_warnedNoD3D9) {
                        s_warnedNoD3D9 = true;
                        // Show a one-time guidance message to help users (esp. on Linux/Wine)
                        const char* msg =
                            "EFZ Training Mode: D3D9 overlay not detected.\n\n"
                            "This disables on-screen overlays (frame advantage, triggers, etc.).\n\n"
                            "If you're running under Linux/Wine: open winecfg and add an override for ddraw.dll\n"
                            "(set it to native, then builtin), or set WINEDLLOVERRIDES=ddraw=n,b before launching the game.\n\n"
                            "If you're on Windows: ensure d3d9.dll is available and not blocked by overlays from other apps.";
                        MessageBoxA(FindEFZWindow(), msg, "EFZ Training Mode", MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);
                    }
                }
            } catch (...) {
                LogOut("[SYSTEM] Exception during D3D9 overlay initialization.", true);
            }
        }).detach();

        // Set initialization flag and stop startup logging
        g_initialized = true;
        inStartupPhase = false;

    // RF freeze now maintained inline by FrameDataMonitor; no background thread needed

    // ImGui status monitoring thread removed; window/key state managed by existing update paths

    // Screen state monitoring thread removed to reduce overhead; phase changes are logged from FrameDataMonitor only
    } catch (...) {
        // Ensure we don't crash the game due to an unhandled exception during startup
        LogOut("[SYSTEM] Exception during DelayedInitialization (top-level catch).", true);
        inStartupPhase = false;
    }
}

// Implementation of the function
void InitializeConfig() {
    LogOut("[SYSTEM] Initializing configuration system...", true);
    if (Config::Initialize()) {
        LogOut("[SYSTEM] Configuration loaded successfully", true);
        
        // Apply settings
        detailedLogging = Config::GetSettings().detailedLogging;
    // Keep file debug logging in sync with config flag
    DebugLog::g_EnableDebugLog = Config::GetSettings().enableDebugFileLog;
    // Console visibility will be handled post-init in DelayedInitialization
    }
    else {
        LogOut("[SYSTEM] Failed to initialize configuration, using defaults", true);
    }
}

// In the DllMain function, keep the existing code as is
BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    switch (ul_reason_for_call) {
    case DLL_PROCESS_ATTACH:
    // Remember our own module for safe self-unload later
    g_hSelfModule = hModule;
        DisableThreadLibraryCalls(hModule);
        std::thread(DelayedInitialization, hModule).detach();
        break;
    case DLL_PROCESS_DETACH:
        // Signal shutdown to all threads
        g_isShuttingDown = true;
        g_featuresEnabled = false;
        
        // Shutdown debug log
        DebugLog::Shutdown();
        
        // CRITICAL: Stop buffer freezing FIRST
        StopBufferFreezing();
        
        // Then restore P2 control
        if (g_p2ControlOverridden) {
            RestoreP2ControlState();
        }
        
        // Clean up hooks safely
        try {
            RemoveInputHook();
            RemoveCollisionHook();
            StopBGMSuppressionPoller();
            // Stop any active overlay rendering
            if (g_guiActive.load()) {
                g_guiActive = false;
            }
        } catch (...) {
            // Suppress exceptions during shutdown
        }
        
        // Give threads a moment to clean up
        Sleep(100);
        
        // Uninitialize MinHook
        MH_Uninitialize();
        break;
    }
    return TRUE;
}