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
    OnlineState st = ReadEfzRevivalOnlineState();
    if (st != OnlineState::Unknown) {
        // Treat Tournament as online-safe (disable features) conservatively
        return (st == OnlineState::Netplay || st == OnlineState::Spectating || st == OnlineState::Tournament);
    }
    return false;
}

// Standalone MonitorOnlineStatus thread removed; online detection integrated into FrameDataMonitor