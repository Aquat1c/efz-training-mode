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

// Cache for detected EfzRevival version
static std::atomic<int> s_cachedRevivalVer{0}; // 0 = Unknown (EfzRevivalVersion::Unknown)

// Helper: narrow/wide title fetcher (best-effort)
static std::string GetEFZWindowTitleA() {
    HWND hwnd = FindEFZWindow();
    if (!hwnd) return std::string();
    char titleA[256] = {0};
    if (GetWindowTextA(hwnd, titleA, (int)sizeof(titleA) - 1) > 0) {
        return std::string(titleA);
    }
    return std::string();
}

EfzRevivalVersion GetEfzRevivalVersion() {
    int cached = s_cachedRevivalVer.load(std::memory_order_acquire);
    if (cached != 0) {
        return static_cast<EfzRevivalVersion>(cached);
    }
    // Parse the current EFZ window title
    std::string t = GetEFZWindowTitleA();
    if (t.empty()) {
        s_cachedRevivalVer.store((int)EfzRevivalVersion::Unknown, std::memory_order_release);
        return EfzRevivalVersion::Unknown;
    }
    // Normalize case for robust matching
    std::string lower = t;
    for (auto &c : lower) c = (char)tolower((unsigned char)c);

    EfzRevivalVersion v = EfzRevivalVersion::Vanilla;
    if (lower.find("-revival-") != std::string::npos) {
        // Has Revival marker; check for known tags
        if (lower.find("1.02e") != std::string::npos) v = EfzRevivalVersion::Revival102e;
        else if (lower.find("1.02h") != std::string::npos) v = EfzRevivalVersion::Revival102h;
        else if (lower.find("1.02i") != std::string::npos) v = EfzRevivalVersion::Revival102i;
        else v = EfzRevivalVersion::Other;
    } else {
        // No Revival marker -> vanilla or unknown
        v = EfzRevivalVersion::Vanilla;
    }
    s_cachedRevivalVer.store((int)v, std::memory_order_release);
    return v;
}

const char* EfzRevivalVersionName(EfzRevivalVersion v) {
    switch (v) {
        case EfzRevivalVersion::Unknown: return "Unknown";
        case EfzRevivalVersion::Vanilla: return "Vanilla";
        case EfzRevivalVersion::Revival102e: return "Revival 1.02e";
        case EfzRevivalVersion::Revival102h: return "Revival 1.02h";
        case EfzRevivalVersion::Revival102i: return "Revival 1.02i";
        case EfzRevivalVersion::Other: return "Revival (Other)";
        default: return "(invalid)";
    }
}

bool IsEfzRevivalVersionSupported(EfzRevivalVersion v /*=detected*/) {
    EfzRevivalVersion vv = (v == (EfzRevivalVersion)0) ? GetEfzRevivalVersion() : v;
    // Stub policy: Support Vanilla and 1.02e. For any other Revival builds (e.g., 1.02h), mark unsupported.
    return (vv == EfzRevivalVersion::Vanilla) || (vv == EfzRevivalVersion::Revival102e);
}

// Try to read the ONLINE state flag exposed by EfzRevival.dll.
// Known RVAs (module-relative):
//  - Revival 1.02e: 0x00A05D0
//  - Revival 1.02h: 0x00A05F0
OnlineState ReadEfzRevivalOnlineState() {
    // Select RVA strictly by known versions to avoid false positives on unknown builds.
    EfzRevivalVersion vv = GetEfzRevivalVersion();
    uintptr_t rva = 0;
    switch (vv) {
        case EfzRevivalVersion::Revival102e: rva = 0x00A05D0; break;
        case EfzRevivalVersion::Revival102h:
            rva = 0x00A05F0; break; // 1.02h
        case EfzRevivalVersion::Revival102i:
            rva = 0x00A15FC; break; // 1.02i (confirmed in docs)
        default:
            return OnlineState::Unknown; // Vanilla/Other/Unknown: don't probe
    }
    HMODULE hEfzRev = GetModuleHandleA("EfzRevival.dll");
    if (!hEfzRev) return OnlineState::Unknown;

    // Address is module base + rva, 4-byte integer
    uintptr_t base = reinterpret_cast<uintptr_t>(hEfzRev);
    volatile int* pFlag = reinterpret_cast<volatile int*>(base + rva);
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