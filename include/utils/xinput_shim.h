#pragma once
#include <windows.h>
#include <Xinput.h>

namespace XInputShim {
    // Initialize by loading an available XInput DLL (tries 1_4, 9_1_0, 1_3)
    bool Init();
    bool IsLoaded();
    const char* LoadedDllName();

    // Safe wrappers that work even if XInput is unavailable.
    DWORD GetState(DWORD dwUserIndex, XINPUT_STATE* pState);
    DWORD GetCapabilities(DWORD dwUserIndex, DWORD dwFlags, XINPUT_CAPABILITIES* pCaps);
    void  Enable(BOOL enable);

    // Per-frame cached snapshot (reduces redundant GetState calls across subsystems).
    // Call RefreshSnapshotOncePerFrame() exactly once early in the frame (e.g. at start of HookedEndScene)
    // then other code can query IsPadConnectedCached / GetCachedState without further syscalls.
    void RefreshSnapshotOncePerFrame();
    bool IsPadConnectedCached(int index); // fast check using cached mask
    unsigned GetConnectedMaskCached();    // bit i set if pad i connected in last refresh
    const XINPUT_STATE* GetCachedState(int index); // returns pointer or nullptr if disconnected
}
