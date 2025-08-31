#define WIN32_LEAN_AND_MEAN
#include <fstream>
#include <iomanip>
#include <chrono>
#include <windows.h>
#include <thread>
#include <vector>
#include "../include/utils/network.h"
#include "../include/core/logger.h"
#include "../include/utils/utilities.h"
// For global shutdown flag
#include "../include/core/globals.h"
extern std::atomic<bool> g_isShuttingDown;



std::atomic<bool> isOnlineMatch(false);

// Try to read the ONLINE state flag exposed by EfzRevival.dll at +0xA05D0
OnlineState ReadEfzRevivalOnlineState() {
    HMODULE hEfzRev = GetModuleHandleA("EfzRevival.dll");
    if (!hEfzRev) return OnlineState::Unknown;

    // Address is module base + 0xA05D0, 4-byte integer
    uintptr_t base = reinterpret_cast<uintptr_t>(hEfzRev);
    volatile int* pFlag = reinterpret_cast<volatile int*>(base + 0xA05D0);
    __try {
        int v = *pFlag;
        if (v == 0) return OnlineState::Netplay;
        if (v == 1) return OnlineState::Spectating;
        if (v == 2) return OnlineState::Offline;
        if (v == 3) return OnlineState::Tournament;
        return OnlineState::Unknown;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return OnlineState::Unknown;
    }
}

// Helper: human-readable name for OnlineState
const char* OnlineStateName(OnlineState st) {
    switch (st) {
        case OnlineState::Netplay: return "Netplay";
        case OnlineState::Spectating: return "Spectating";
        case OnlineState::Offline: return "Offline";
        case OnlineState::Tournament: return "Tournament";
        default: return "Unknown";
    }
}

// Check if the process has any network connections - improved for local network detection
bool DetectOnlineMatch() {
    // Only use EfzRevival.dll flag for detection (no TCP/UDP fallback)
    static OnlineState s_lastLogged = OnlineState::Unknown;
    OnlineState st = ReadEfzRevivalOnlineState();
    if (st != OnlineState::Unknown) {
        if (st != s_lastLogged) {
            LogOut(std::string("[NETWORK] EfzRevival state detected: ") + OnlineStateName(st), true);
            s_lastLogged = st;
        }
        // Treat Tournament as online-safe (disable features) conservatively
        return (st == OnlineState::Netplay || st == OnlineState::Spectating || st == OnlineState::Tournament);
    }
    return false;
}

// Monitor for online status and manage console visibility
void MonitorOnlineStatus() {
    bool prevOnlineStatus = false;
    int checkCounter = 0;
    int consecutiveOnlineChecks = 0;
    bool stopChecking = false;
    auto gameStartTime = std::chrono::steady_clock::now();
    
    while (true) {
        // Exit cleanly on shutdown
        if (g_isShuttingDown.load()) {
            break;
        }
        // If we've already entered online mode globally, park this thread
        if (g_onlineModeActive.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            continue;
        }
        // Check if we should stop checking network status
        if (stopChecking) {
            // Just sleep and continue the loop without doing checks
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            continue;
        }

        // Check if game has been running for 10 seconds
        auto currentTime = std::chrono::steady_clock::now();
        auto elapsedSecs = std::chrono::duration_cast<std::chrono::seconds>(
            currentTime - gameStartTime).count();
        
        if (elapsedSecs > 10 && !isOnlineMatch.load()) {
            // Stop checking after 10 seconds if not online
            LogOut("[NETWORK] Stopped network monitoring (timeout)", false);
            stopChecking = true;
            continue;
        }
        
        // Check more frequently - every 2.5 seconds instead of 5
        if (++checkCounter >= 50) {
            checkCounter = 0;
            
            bool currentOnlineStatus = DetectOnlineMatch();
            
            if (currentOnlineStatus) {
                ++consecutiveOnlineChecks;
                
                // Reduce required checks from 3 to 2 for testing
                if (consecutiveOnlineChecks >= 2) {
                    // If available, log the exact EfzRevival state (0=netplay,1=spectating,2=offline,3=tournament)
                    OnlineState st = ReadEfzRevivalOnlineState();
                    if (st != OnlineState::Unknown) {
                        LogOut(std::string("[NETWORK] EfzRevival state: ") + OnlineStateName(st), true);
                    }
                    LogOut("[NETWORK] Online match confirmed", true);
                    
                    // Reduced countdown from 5 to 3 seconds
                    for (int i = 3; i > 0; i--) {
                        LogOut("[NETWORK] Console will be hidden in " + std::to_string(i) + " seconds...", true);
                        std::this_thread::sleep_for(std::chrono::seconds(1));
                    }
                    
                    // Hide console window
                    HWND consoleWnd = GetConsoleWindow();
                    if (consoleWnd != NULL) {
                        ShowWindow(consoleWnd, SW_HIDE);
                    }
                    
                    isOnlineMatch = true;
                    prevOnlineStatus = true;

                    // Enter online-safe mode (terminate mod threads/features)
                    EnterOnlineMode();
                    
                    // Add this to stop checking after confirming online
                    stopChecking = true;
                }
            }
        }
        
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}