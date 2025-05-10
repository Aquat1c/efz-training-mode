#include <windows.h>
#include <thread>
#include "../include/utilities.h"
#include "../include/logger.h"
#include "../include/frame_monitor.h"

// Forward declarations for functions in other files
void MonitorKeys();
void FrameDataMonitor();
void UpdateConsoleTitle();

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);

        // Create debug console
        CreateDebugConsole();

        // Initialize logging system
        InitializeLogging();

        // Start threads
        std::thread(MonitorKeys).detach();
        std::thread(UpdateConsoleTitle).detach();
        std::thread(FrameDataMonitor).detach();

        LogOut("EFZ Training Mode initialized", true);
    }
    return TRUE;
}