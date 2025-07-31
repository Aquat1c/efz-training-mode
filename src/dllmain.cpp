#include <fstream>
#include <iomanip>
#include <chrono>
#include <vector>
#include <windows.h>
#include <thread>
#include "../include/memory.h"
#include "../include/utilities.h"
#include "../include/logger.h"
#include "../include/frame_monitor.h"
#include "../include/network.h"
#include "../include/di_keycodes.h"
#include "../include/input_handler.h"
#include "../include/overlay.h"
#include "../include/imgui_impl.h"
#include "../include/imgui_gui.h"
#include "../include/config.h"
#include "../include/practice_patch.h"
#include "../include/input_hook.h" // Add this include
#include "../3rdparty/minhook/include/MinHook.h" // Add this include
#include "../include/bgm_control.h"

// Forward declarations for functions in other files
void MonitorKeys();
void FrameDataMonitor();
void UpdateConsoleTitle();
void MonitorOnlineStatus();
void WriteStartupLog(const std::string& message);
extern std::atomic<bool> inStartupPhase;

// Add this declaration before it's used
void InitializeConfig();

// Add this flag to track initialization state
std::atomic<bool> g_initialized(false);

// Add this near the top with other globals
std::atomic<bool> g_isShuttingDown(false);

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
    std::thread(GlobalF1MonitorThread).detach();
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
    if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
        // Begin minimal startup logging
        WriteStartupLog("DLL_PROCESS_ATTACH");
        WriteStartupLog("Module handle: " + std::to_string((uintptr_t)hModule));
        
        // Disable thread notifications
        DisableThreadLibraryCalls(hModule);
        
        // Set up minimal locale settings
        try {
            std::locale::global(std::locale("C"));
            WriteStartupLog("Set locale to C");
        } catch (...) {
            WriteStartupLog("Failed to set locale");
        }
        
        // Launch a delayed initialization thread
        WriteStartupLog("Starting delayed initialization thread");
        std::thread(DelayedInitialization, hModule).detach();
        WriteStartupLog("Delayed initialization thread started");
        
        // Return immediately to let the game continue loading
        WriteStartupLog("DLL_PROCESS_ATTACH complete, returning control to game");
        return TRUE;
    }
    else if (ul_reason_for_call == DLL_PROCESS_DETACH) {
        // Set shutdown flag to stop threads
        g_isShuttingDown.store(true);
        
        // Add a small delay to allow threads to notice the flag
        Sleep(100);
        
        // Remove the input hook first
        RemoveInputHook();
        globalF1ThreadRunning = false;
        // First clean up ImGui to prevent rendering during shutdown
        ImGuiImpl::Shutdown();
        StopBGMSuppressionPoller();
        // Then clean up the D3D9 hook
        DirectDrawHook::ShutdownD3D9();
        
        // Finally clean up the overlay
        DirectDrawHook::Shutdown();

        // NEW: Uninitialize MinHook once at the very end.
        MH_Uninitialize();
        LogOut("[SYSTEM] MinHook uninitialized.", true);
        
        LogOut("[SYSTEM] DLL detaching, cleanup complete", true);
    }
    return TRUE;
}