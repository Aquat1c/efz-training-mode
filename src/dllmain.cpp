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
#include "../include/game/auto_action.h"  // ADD THIS INCLUDE
#include "../include/input/input_hook.h" // Add this include
#include "../3rdparty/minhook/include/MinHook.h" // Add this include
#include "../include/utils/bgm_control.h"
#include "../include/game/game_state.h"
#include "../include/core/globals.h"  // Add this include
#pragma comment(lib, "winmm.lib")

// Forward declarations for functions in other files
void MonitorKeys();
void FrameDataMonitor();
void UpdateConsoleTitle();
void MonitorOnlineStatus();
void WriteStartupLog(const std::string& message);
extern std::atomic<bool> inStartupPhase;

// Add this declaration before it's used
void InitializeConfig();

// Define the global flags (remove 'static' if present)
std::atomic<bool> g_isShuttingDown(false);
std::atomic<bool> g_initialized(false);
std::atomic<bool> g_featuresEnabled(false);  // If this exists elsewhere, move it here

// Delayed initialization function
void DelayedInitialization(HMODULE hModule) {
    // Remove the 5-second delay
    // Sleep(5000);  <-- Replace this with a much shorter delay
    Sleep(1500); // Just a small delay to ensure the game has started properly
    
    WriteStartupLog("Starting delayed initialization");
    
    // Create debug console with explicit visibility
    WriteStartupLog("Creating debug console...");
    CreateDebugConsole();
    WriteStartupLog("CreateDebugConsole returned");
    
    // Explicitly make console visible
    HWND consoleWnd = GetConsoleWindow();
    if (consoleWnd) {
        ShowWindow(consoleWnd, SW_SHOW);
    }
    
    // Initialize logging system
    WriteStartupLog("Initializing logging system...");
    InitializeLogging();
    WriteStartupLog("Logging system initialized");
    
    // NEW: Initialize MinHook once for the entire application.
    if (MH_Initialize() != MH_OK) {
        LogOut("[SYSTEM] MinHook initialization failed. Hooks will not be installed.", true);
        return; // Early exit if MinHook fails
    }
    LogOut("[SYSTEM] MinHook initialized successfully.", true);

    // Install the input hook after the game is stable
    InstallInputHook();
    StartBGMSuppressionPoller();

    LogOut("[SYSTEM] EFZ Training Mode - Delayed initialization starting", true);
    LogOut("[SYSTEM] Console initialized with code page: " + std::to_string(GetConsoleOutputCP()), true);
    LogOut("[SYSTEM] Current locale: C", true);
    
    // Initialize configuration system - ADD THIS LINE
    InitializeConfig();
    
    // Start ESSENTIAL threads. Hotkey monitor is now managed by Enable/DisableFeatures.
    LogOut("[SYSTEM] Starting background threads...", true);
    std::thread(UpdateConsoleTitle).detach();
    std::thread(FrameDataMonitor).detach();
    std::thread(MonitorOnlineStatus).detach();
    //std::thread(GlobalF1MonitorThread).detach();
    // REMOVED: The practice mode patch is no longer a persistent thread.
    // std::thread(MonitorAndPatchPracticeMode).detach(); 
    LogOut("[SYSTEM] Essential background threads started.", true);
    
    // Use standard Windows input APIs instead of DirectInput
    WriteStartupLog("Using standard Windows input APIs instead of DirectInput");
    g_directInputAvailable = false;  // Ensure DirectInput is marked as unavailable
    
    WriteStartupLog("Reading key.ini file...");
    ReadKeyMappingsFromIni();
    WriteStartupLog("Key mappings read");
    
    LogOut("EFZ Training Mode initialized successfully", true);
    WriteStartupLog("Delayed initialization complete");
    
    // REMOVED: Do not show help screen automatically on startup
    // The user can open it with the configured help key.
    
    // RE-ADD: Initialize D3D9 hook for overlays once at startup
    std::thread([]{
        Sleep(2000); // Give the game a moment to be fully ready
        if (DirectDrawHook::InitializeD3D9()) {
            LogOut("[SYSTEM] D3D9 Overlay system initialized.", true);
        } else {
            LogOut("[SYSTEM] Failed to initialize D3D9 Overlay system.", true);
        }
    }).detach();
    
    // Set initialization flag and stop startup logging
    g_initialized = true;
    inStartupPhase = false;

    // Initialize RF freeze thread
    InitRFFreezeThread();
    
    // Add ImGui monitoring thread
    std::thread([]{
        // Wait a bit for everything to initialize
        Sleep(5000);
        
        // Log ImGui rendering status every few seconds
        while (true) {
            if (ImGuiImpl::IsInitialized()) {
                // Change this line to respect the detailed logging flag
                LogOut("[IMGUI_MONITOR] Status: Initialized=" + 
                      std::to_string(ImGuiImpl::IsInitialized()) + 
                      ", Visible=" + std::to_string(ImGuiImpl::IsVisible()), 
                      detailedLogging.load()); // Only show if detailed logging is enabled
            }
            
            // Check every 5 seconds
            Sleep(5000);
        }
    }).detach();
    
    // Add screen state monitoring thread
    std::thread([]{
        GameMode lastGameMode = GameMode::Unknown;
        bool lastCharSelectState = false;
        
        LogOut("[SYSTEM] Starting screen state monitoring thread", true);
        
        // Keep monitoring while the DLL is loaded
        while (!g_isShuttingDown.load()) {
            GameMode currentMode = GetCurrentGameMode();
            bool isCharSelect = IsInCharacterSelectScreen();
            
            // When any game mode changes or character select state changes
            if (currentMode != lastGameMode || isCharSelect != lastCharSelectState) {
                // Log the transition
                LogOut("[SCREEN_MONITOR] Screen state changed - Mode: " + 
                      GetGameModeName(lastGameMode) + " → " + GetGameModeName(currentMode) + 
                      ", CharSelect: " + (lastCharSelectState ? "Yes" : "No") + " → " + 
                      (isCharSelect ? "Yes" : "No"), true);
                
                // Call the debug dump function
                DebugDumpScreenState();
                
                // Update tracking variables
                lastGameMode = currentMode;
                lastCharSelectState = isCharSelect;
                
                // Also check if we're exiting from gameplay to character select
                if (IsInGameplayState() && isCharSelect) {
                    LogOut("[SCREEN_MONITOR] Detected transition from gameplay to character select", true);
                    
                    // Ensure proper cleanup
                    StopBufferFreezing();
                    SetBGMSuppressed(false);
                    
                    // Additional logs to help diagnose buffer freeze issues
                    LogOut("[BUFFER_STATE] g_bufferFreezingActive = " + 
                          std::to_string(g_bufferFreezingActive), true);
                    LogOut("[BUFFER_STATE] g_indexFreezingActive = " + 
                          std::to_string(g_indexFreezingActive), true);
                }
            }
            
            GamePhase phase = GetCurrentGamePhase();
            static GamePhase lastPhase = GamePhase::Unknown;
            if (phase != lastPhase) {
                LogOut("[SCREEN_MONITOR] Phase change: " + std::to_string((int)lastPhase) + " -> " + std::to_string((int)phase), true);
                DebugDumpScreenState();

                if (lastPhase == GamePhase::Match && phase != GamePhase::Match) {
                    StopBufferFreezing();
                    ResetActionFlags();
                    g_bufferFreezingActive = false;
                    g_indexFreezingActive = false;
                }
                lastPhase = phase;
            }
            
            // Sleep to avoid high CPU usage (check every 100ms)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        
        LogOut("[SCREEN_MONITOR] Screen state monitoring thread stopped", true);
    }).detach();
}

// Implementation of the function
void InitializeConfig() {
    LogOut("[SYSTEM] Initializing configuration system...", true);
    if (Config::Initialize()) {
        LogOut("[SYSTEM] Configuration loaded successfully", true);
        
        // Apply settings
        detailedLogging = Config::GetSettings().detailedLogging;
    }
    else {
        LogOut("[SYSTEM] Failed to initialize configuration, using defaults", true);
    }
}

// In the DllMain function, keep the existing code as is
BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    switch (ul_reason_for_call) {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hModule);
        std::thread(DelayedInitialization, hModule).detach();
        break;
    case DLL_PROCESS_DETACH:
        // Signal shutdown to all threads
        g_isShuttingDown = true;
        g_featuresEnabled = false;
        
        // CRITICAL: Stop buffer freezing FIRST
        StopBufferFreezing();
        
        // Then restore P2 control
        if (g_p2ControlOverridden) {
            RestoreP2ControlState();
        }
        
        // Clean up hooks safely
        try {
            RemoveInputHook();
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