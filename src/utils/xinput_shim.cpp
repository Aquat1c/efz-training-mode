#include "../include/utils/xinput_shim.h"
#include "../include/core/logger.h"

namespace {
    HMODULE g_xinput = nullptr;
    const char* g_name = nullptr;
    typedef DWORD (WINAPI *PFN_XInputGetState)(DWORD, XINPUT_STATE*);
    typedef DWORD (WINAPI *PFN_XInputGetCapabilities)(DWORD, DWORD, XINPUT_CAPABILITIES*);
    typedef void  (WINAPI *PFN_XInputEnable)(BOOL);
    PFN_XInputGetState pGetState = nullptr;
    PFN_XInputGetCapabilities pGetCaps = nullptr;
    PFN_XInputEnable pEnable = nullptr;

    HMODULE TryLoad(const char* dll) {
        HMODULE h = LoadLibraryA(dll);
        if (!h) return nullptr;
        auto gs = (PFN_XInputGetState)GetProcAddress(h, "XInputGetState");
        auto gc = (PFN_XInputGetCapabilities)GetProcAddress(h, "XInputGetCapabilities");
        auto en = (PFN_XInputEnable)GetProcAddress(h, "XInputEnable");
        if (!gs || !gc) { FreeLibrary(h); return nullptr; }
        pGetState = gs; pGetCaps = gc; pEnable = en; g_name = dll; return h;
    }
}

namespace XInputShim {
    bool Init() {
        if (g_xinput) return true;
        // Try common variants in order of modernity/availability
        const char* cands[] = { "xinput1_4.dll", "xinput9_1_0.dll", "xinput1_3.dll" };
        for (const char* c : cands) {
            HMODULE h = TryLoad(c);
            if (h) { g_xinput = h; LogOut(std::string("[XINPUT] Loaded ") + c, true); return true; }
        }
        LogOut("[XINPUT] No XInput DLL found; gamepad features will be limited.", true);
        return false;
    }

    bool IsLoaded() { return g_xinput != nullptr; }
    const char* LoadedDllName() { return g_name ? g_name : "(none)"; }

    DWORD GetState(DWORD idx, XINPUT_STATE* st) {
        if (!g_xinput && !Init()) return ERROR_DEVICE_NOT_CONNECTED;
        if (!pGetState) return ERROR_DEVICE_NOT_CONNECTED;
        return pGetState(idx, st);
    }
    DWORD GetCapabilities(DWORD idx, DWORD flags, XINPUT_CAPABILITIES* caps) {
        if (!g_xinput && !Init()) return ERROR_DEVICE_NOT_CONNECTED;
        if (!pGetCaps) return ERROR_DEVICE_NOT_CONNECTED;
        return pGetCaps(idx, flags, caps);
    }
    void Enable(BOOL en) {
        if (!g_xinput && !Init()) return;
        if (pEnable) pEnable(en);
    }

    // --- Cached snapshot implementation ---
    namespace {
        XINPUT_STATE g_cachedStates[4] = {};
        unsigned     g_cachedMask = 0; // bit i => connected
        DWORD        g_lastRefreshTick = 0; // GetTickCount of last refresh
    }

    void RefreshSnapshotOncePerFrame() {
        // Coalesce calls that land in the same ms to avoid redundant syscalls within one frame
        DWORD now = GetTickCount();
        if (now == g_lastRefreshTick) return;
        g_lastRefreshTick = now;

        unsigned mask = 0;
        for (DWORD i = 0; i < 4; ++i) {
            XINPUT_STATE st{};
            if (GetState(i, &st) == ERROR_SUCCESS) {
                g_cachedStates[i] = st;
                mask |= (1u << i);
            } else {
                ZeroMemory(&g_cachedStates[i], sizeof(XINPUT_STATE));
            }
        }
        g_cachedMask = mask;
    }

    bool IsPadConnectedCached(int index) {
        if (index < 0 || index > 3) return false;
        return ((g_cachedMask >> index) & 1u) != 0;
    }

    unsigned GetConnectedMaskCached() { return g_cachedMask; }

    const XINPUT_STATE* GetCachedState(int index) {
        if (!IsPadConnectedCached(index)) return nullptr;
        return &g_cachedStates[index];
    }
}
