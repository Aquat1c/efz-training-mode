#pragma once
#include <windows.h>
#include <Xinput.h>
#include <atomic>
#include <cstddef>
#include <string>

namespace XInputShim {
    constexpr int kControllerSlotCount = 4;
    constexpr std::size_t kControllerSlotNameCapacity = 96;

    // One coherent publication from the background controller watcher: copy this small
    // object once and inspect masks/states/slot kinds/names without observing two different polls.
    struct Snapshot {
        unsigned connectedMask = 0;
        unsigned nativeMask = 0;
        unsigned genericMask = 0;
        DWORD generation = 0;
        XINPUT_STATE states[kControllerSlotCount] = {};
        XINPUT_CAPABILITIES capabilities[kControllerSlotCount] = {};
        bool capabilitiesValid[kControllerSlotCount] = {};
        bool genericSlots[kControllerSlotCount] = {};
        char slotNames[kControllerSlotCount][kControllerSlotNameCapacity] = {};

        bool IsConnected(int index) const {
            return index >= 0 && index < kControllerSlotCount &&
                   ((connectedMask >> index) & 1u) != 0;
        }
        bool CopyState(int index, XINPUT_STATE& out) const {
            if (!IsConnected(index)) {
                ZeroMemory(&out, sizeof(out));
                return false;
            }
            out = states[index];
            return true;
        }
    };

    // Runtime-only debug toggle for verbose generic-controller input translation logs.
    extern std::atomic<bool> g_LogGenericPadInputDebug;

    // Initialize by loading an available XInput DLL (tries 1_4, 9_1_0, 1_3)
    bool Init();
    // Published by the Practice Battle owner; getters cannot enable polling.
    void SetPollingActive(bool active);
    bool IsPollingActive();
    bool IsLoaded();
    const char* LoadedDllName();

    // Safe wrappers that work even if XInput is unavailable.
    DWORD GetState(DWORD dwUserIndex, XINPUT_STATE* pState);
    DWORD GetCapabilities(DWORD dwUserIndex, DWORD dwFlags, XINPUT_CAPABILITIES* pCaps);
    void  Enable(BOOL enable);

    // The watcher performs all XInput/DirectInput hardware work away from the
    // render thread, then publishes under a short copy-only mutex.
    void RefreshSnapshotOncePerFrame();
    void CopySnapshot(Snapshot& out);
    bool CopyCachedState(int index, XINPUT_STATE& out);
    bool IsPadConnectedCached(int index);
    unsigned GetConnectedMaskCached();    // bit i set if pad i connected in last refresh
    unsigned GetNativeConnectedMaskCached();   // physical XInput slots
    unsigned GetGenericConnectedMaskCached();  // DirectInput fallback slots
    bool IsGenericFallbackSlot(int index);
    std::string GetSlotDisplayName(int index);

    // Friendly controller display name pre-computed off the game thread; reads are
    // non-blocking (try_lock with stale fallback), safe for per-frame UI. Returns
    // true if the published name was used; false if a fallback string was written.
    bool GetPublishedControllerName(int index, char* buf, size_t bufLen);
}
