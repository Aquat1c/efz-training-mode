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
#include "../include/core/memory.h"
#include "../include/game/game_state.h"
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
    // Supported builds: Vanilla EFZ and EfzRevival 1.02e, 1.02h, 1.02i
    switch (vv) {
        case EfzRevivalVersion::Vanilla:
        case EfzRevivalVersion::Revival102e:
        case EfzRevivalVersion::Revival102h:
        case EfzRevivalVersion::Revival102i:
            return true;
        default:
            return false;
    }
}

// Try to read the ONLINE state exposed by EfzRevival.dll.
// Preferred path (CE-confirmed):
//   ctxPtr = *(void**)(EfzRevival.dll + 0x26A4)
//   state  = *(int*)(ctxPtr + 0x370)   // 1.02e/1.02h
//   state  = *(int*)(ctxPtr + 0x37C)   // 1.02i
// Fallback path: legacy fixed RVAs (module-relative) returning a 32-bit int.
OnlineState ReadEfzRevivalOnlineState() {
    auto mapState = [](int v) -> OnlineState {
        switch (v) {
            case 0: return OnlineState::Netplay;
            case 1: return OnlineState::Spectating;
            case 2: return OnlineState::Offline;
            case 3: return OnlineState::Tournament;
            default: return OnlineState::Unknown;
        }
    };

    EfzRevivalVersion vv = GetEfzRevivalVersion();
    if (vv == EfzRevivalVersion::Unknown || vv == EfzRevivalVersion::Vanilla || vv == EfzRevivalVersion::Other)
        return OnlineState::Unknown;

    HMODULE hEfzRev = GetModuleHandleA("EfzRevival.dll");
    if (!hEfzRev) return OnlineState::Unknown;
    uintptr_t base = reinterpret_cast<uintptr_t>(hEfzRev);

    // Pointer-based path first (more stable across sub-versions)
    {
        uintptr_t ctx = 0; 
        uintptr_t ctxPtrAddr = base + 0x26A4; // module global: pointer to net/rollback context
        if (SafeReadMemory(ctxPtrAddr, &ctx, sizeof(ctx)) && ctx) {
            const size_t candidates[2] = { 0x370, 0x37C }; // e/h then i; probe both to be safe
            for (size_t i = 0; i < 2; ++i) {
                int raw = 0; 
                if (!SafeReadMemory(ctx + candidates[i], &raw, sizeof(raw))) continue;
                // Primary attempt: exact enum 0..3
                OnlineState st = mapState(raw);
                if (st != OnlineState::Unknown) return st;
                // Secondary attempt: some builds store in a byte or low bits
                st = mapState(raw & 0xFF);
                if (st != OnlineState::Unknown) return st;
                st = mapState(raw & 0x03);
                if (st != OnlineState::Unknown) return st;
            }
        }
    }

    // Legacy fixed-RVA ints (keep as fallback)
    uintptr_t rva = 0;
    switch (vv) {
        case EfzRevivalVersion::Revival102e: rva = 0x00A05D0; break;
        case EfzRevivalVersion::Revival102h: rva = 0x00A05F0; break;
        case EfzRevivalVersion::Revival102i: rva = 0x00A15FC; break;
        default: return OnlineState::Unknown;
    }
    int raw = 0; 
    if (!SafeReadMemory(base + rva, &raw, sizeof(raw))) {
        return OnlineState::Unknown;
    }
    OnlineState st = mapState(raw);
    if (st != OnlineState::Unknown) return st;
    st = mapState(raw & 0xFF);
    if (st != OnlineState::Unknown) return st;
    st = mapState(raw & 0x03);
    if (st != OnlineState::Unknown) return st;
    return OnlineState::Unknown;
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
    EfzRevivalVersion v = GetEfzRevivalVersion();
    
    // For unsupported/unknown versions: try multiple known RVAs
    // The state variable address drifts between versions, so probe all candidates
    if (v == EfzRevivalVersion::Other || v == EfzRevivalVersion::Unknown) {
        HMODULE hEfzRev = GetModuleHandleA("EfzRevival.dll");
        if (!hEfzRev) {
            // EfzRevival.dll not loaded yet - definitely not online
            return false;
        }
        
        uintptr_t base = reinterpret_cast<uintptr_t>(hEfzRev);
        // Known RVAs from supported versions (most recent first)
        const uintptr_t candidateRVAs[] = {
            0xA15FC,  // 1.02i
            0xA05F0,  // 1.02h
            0xA05D0   // 1.02e
        };
        
        bool foundValidState = false;
        for (uintptr_t rva : candidateRVAs) {
            int state = -1;
            if (SafeReadMemory(base + rva, &state, sizeof(state))) {
                char debugBuf[128];
                snprintf(debugBuf, sizeof(debugBuf), "[NETWORK_DBG] RVA 0x%X read value: %d (0x%08X)", (unsigned int)rva, state, (unsigned int)state);
                LogOut(debugBuf);
                
                // Valid state values: 0=Netplay, 1=Spectating, 2=Offline, 3=Tournament
                // Check if we got a valid state value
                if (state >= 0 && state <= 3) {
                    foundValidState = true;
                    
                    // IMPORTANT: During early startup, reading 0 might just be uninitialized memory
                    // To avoid false positives, we need additional validation
                    // Check if this looks like a real initialized state by verifying surrounding memory
                    // isn't all zeros (which would indicate uninitialized .data section)
                    if (state == 0 || state == 1 || state == 3) {
                        // Reading what looks like an "online" state - verify it's real
                        // Read a few bytes before and after to see if memory looks initialized
                        int verify1 = 0, verify2 = 0;
                        bool mem1 = SafeReadMemory(base + rva - 4, &verify1, sizeof(verify1));
                        bool mem2 = SafeReadMemory(base + rva + 4, &verify2, sizeof(verify2));
                        
                        // If we can't read surrounding memory, or it's all zeros, this is likely
                        // uninitialized memory during early startup - assume offline
                        if (!mem1 || !mem2 || (verify1 == 0 && verify2 == 0 && state == 0)) {
                            char buf[256];
                            snprintf(buf, sizeof(buf), "[NETWORK_DBG] State %d at RVA 0x%X looks uninitialized (surrounding: 0x%08X, 0x%08X) - ignoring",
                                state, (unsigned int)rva, (unsigned int)verify1, (unsigned int)verify2);
                            LogOut(buf);
                            continue; // Try next RVA
                        }
                    }
                    
                    // Found valid state; 2 = Offline/Practice, others = online
                    if (state != 2) {
                        char buf[128];
                        snprintf(buf, sizeof(buf), "[NETWORK] Unsupported version online state detected at RVA 0x%X: %d", (unsigned int)rva, state);
                        LogOut(buf);
                        return true;
                    }
                    // State is 2 (Offline), found the correct address
                    char buf[128];
                    snprintf(buf, sizeof(buf), "[NETWORK] Offline state confirmed at RVA 0x%X: %d", (unsigned int)rva, state);
                    LogOut(buf);
                    return false;
                }
            }
        }
        
        // If we read valid-looking states but they're all invalid, or all reads failed,
        // conservatively assume offline to avoid blocking features during startup
        if (!foundValidState) {
            LogOut("[NETWORK_DBG] No valid state found at any known RVA - assuming offline (likely early startup)");
        }
        return false;
    }
    
    // For supported versions: use EfzRevival.dll flag for detection (no TCP/UDP fallback)
    OnlineState st = ReadEfzRevivalOnlineState();
    if (st != OnlineState::Unknown) {
        // Treat Tournament as online-safe (disable features) conservatively
        return (st == OnlineState::Netplay || st == OnlineState::Spectating || st == OnlineState::Tournament);
    }
    return false;
}

// Standalone MonitorOnlineStatus thread removed; online detection integrated into FrameDataMonitor