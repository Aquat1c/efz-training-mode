#include <fstream>
#include <iomanip>
#include <chrono>
#include <vector>
#include <windows.h>
#include <thread>
#include "../include/utilities.h"
#include "../include/logger.h"
#include "../include/frame_monitor.h"
#include "../include/network.h"
#include "../include/di_keycodes.h"
#include "../include/input_handler.h"

// Forward declarations for functions in other files
void MonitorKeys();
void FrameDataMonitor();
void UpdateConsoleTitle();
void MonitorOnlineStatus();
void WriteStartupLog(const std::string& message);
extern std::atomic<bool> inStartupPhase;

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
    
    // Set initialization flag and stop startup logging
    g_initialized = true;
    inStartupPhase = false;
}

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
    return TRUE;
}