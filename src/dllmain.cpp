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
    
    LogOut("[SYSTEM] EFZ Training Mode - Delayed initialization starting", true);
    LogOut("[SYSTEM] Console initialized with code page: " + std::to_string(GetConsoleOutputCP()), true);
    LogOut("[SYSTEM] Current locale: C", true);
    
    // Initialize configuration system - ADD THIS LINE
    InitializeConfig();
    
    // Start threads with small delays between them to avoid race conditions
    WriteStartupLog("Starting MonitorKeys thread...");
    std::thread(MonitorKeys).detach();
    Sleep(100);
    WriteStartupLog("MonitorKeys thread started");
    
    WriteStartupLog("Starting UpdateConsoleTitle thread...");
    std::thread(UpdateConsoleTitle).detach();
    Sleep(100);
    WriteStartupLog("UpdateConsoleTitle thread started");
    
    WriteStartupLog("Starting FrameDataMonitor thread...");
    std::thread(FrameDataMonitor).detach();
    Sleep(100);
    WriteStartupLog("FrameDataMonitor thread started");
    
    WriteStartupLog("Starting MonitorOnlineStatus thread...");
    std::thread(MonitorOnlineStatus).detach();
    WriteStartupLog("MonitorOnlineStatus thread started");
    
    // Use standard Windows input APIs instead of DirectInput
    WriteStartupLog("Using standard Windows input APIs instead of DirectInput");
    g_directInputAvailable = false;  // Ensure DirectInput is marked as unavailable
    
    WriteStartupLog("Reading key.ini file...");
    ReadKeyMappingsFromIni();
    WriteStartupLog("Key mappings read");
    
    LogOut("EFZ Training Mode initialized successfully", true);
    WriteStartupLog("Delayed initialization complete");
    
    // Show help screen automatically
    WriteStartupLog("Showing help screen automatically");
    ShowHotkeyInfo();
    WriteStartupLog("Help screen shown");
    
    // NEW: Replace the old overlay logic with the new D3D9 hook initialization.
    std::thread([]{
        // Wait for the game window and D3D to be ready.
        Sleep(5000);
        
        if (DirectDrawHook::InitializeD3D9()) {
            LogOut("[SYSTEM] ImGui D3D9 hook initialized successfully.", true);
        } else {
            LogOut("[SYSTEM] FATAL: Could not initialize ImGui D3D9 hook.", true);
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
                LogOut("[IMGUI_MONITOR] Status: Initialized=" + 
                      std::to_string(ImGuiImpl::IsInitialized()) + 
                      ", Visible=" + std::to_string(ImGuiImpl::IsVisible()), true);
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
        // Clean up the D3D9 hook and ImGui
        DirectDrawHook::ShutdownD3D9();
        // Clean up the overlay
        DirectDrawHook::Shutdown();
    }
    return TRUE;
}