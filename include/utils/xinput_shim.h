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
}
