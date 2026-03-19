#include "../include/utils/xinput_shim.h"
#include "../include/core/logger.h"
#include "../include/core/globals.h"
#include "../include/utils/utilities.h"

#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {
    HMODULE g_xinput = nullptr;
    const char* g_name = nullptr;
    typedef DWORD (WINAPI *PFN_XInputGetState)(DWORD, XINPUT_STATE*);
    typedef DWORD (WINAPI *PFN_XInputGetCapabilities)(DWORD, DWORD, XINPUT_CAPABILITIES*);
    typedef void  (WINAPI *PFN_XInputEnable)(BOOL);
    PFN_XInputGetState pGetState = nullptr;
    PFN_XInputGetCapabilities pGetCaps = nullptr;
    PFN_XInputEnable pEnable = nullptr;

    struct GenericPad {
        LPDIRECTINPUTDEVICE8 device = nullptr;
        std::string name;
        DIJOYSTATE2 rawState{};
        XINPUT_STATE syntheticState{};
        DWORD packetCounter = 0;
        bool connected = false;
    };

    LPDIRECTINPUT8 g_directInput = nullptr;
    std::vector<GenericPad> g_genericPads;
    bool g_directInputInitAttempted = false;
    DWORD g_lastRefreshTick = 0;
    DWORD g_lastGenericEnumTick = 0;
    unsigned g_cachedMask = 0;
    unsigned g_cachedNativeMask = 0;
    unsigned g_cachedGenericMask = 0;
    XINPUT_STATE g_cachedStates[4] = {};
    bool g_cachedGenericSlots[4] = { false, false, false, false };
    std::string g_cachedSlotNames[4];
    std::string g_lastSlotSummary;
    std::string g_lastGenericInventorySummary;
    std::mutex g_snapshotMutex;
    std::atomic<bool> g_watcherStarted{ false };
    std::atomic<bool> g_hasInitialSnapshot{ false };

    HMODULE TryLoad(const char* dll) {
        HMODULE h = LoadLibraryA(dll);
        if (!h) return nullptr;
        auto gs = (PFN_XInputGetState)GetProcAddress(h, "XInputGetState");
        auto gc = (PFN_XInputGetCapabilities)GetProcAddress(h, "XInputGetCapabilities");
        auto en = (PFN_XInputEnable)GetProcAddress(h, "XInputEnable");
        if (!gs || !gc) {
            FreeLibrary(h);
            return nullptr;
        }
        pGetState = gs;
        pGetCaps = gc;
        pEnable = en;
        g_name = dll;
        return h;
    }

    HWND ResolveInputCooperativeWindow() {
        HWND hwnd = FindEFZWindow();
        if (!hwnd) hwnd = GetConsoleWindow();
        if (!hwnd) hwnd = GetDesktopWindow();
        return hwnd;
    }

    std::string NarrowDeviceName(const TCHAR* value) {
        if (!value || !*value) return "DirectInput Controller";
#if defined(UNICODE)
        int needed = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
        if (needed <= 1) return "DirectInput Controller";
        std::string out(static_cast<size_t>(needed), '\0');
        WideCharToMultiByte(CP_UTF8, 0, value, -1, &out[0], needed, nullptr, nullptr);
        if (!out.empty() && out.back() == '\0') out.pop_back();
        return out;
#else
        return std::string(value);
#endif
    }

    void ReleaseGenericPadsLocked() {
        for (GenericPad& pad : g_genericPads) {
            if (pad.device) {
                pad.device->Unacquire();
                pad.device->Release();
                pad.device = nullptr;
            }
        }
        g_genericPads.clear();
    }

    void ConfigureAxisRange(IDirectInputDevice8* device, DWORD offset) {
        DIPROPRANGE range{};
        range.diph.dwSize = sizeof(DIPROPRANGE);
        range.diph.dwHeaderSize = sizeof(DIPROPHEADER);
        range.diph.dwObj = offset;
        range.diph.dwHow = DIPH_BYOFFSET;
        range.lMin = -32768;
        range.lMax = 32767;
        device->SetProperty(DIPROP_RANGE, &range.diph);
    }

    BOOL CALLBACK EnumGenericPadsCallback(const DIDEVICEINSTANCE* instance, VOID* context) {
        auto* pads = static_cast<std::vector<GenericPad>*>(context);
        if (!pads || !g_directInput) return DIENUM_STOP;
        if (pads->size() >= 4) return DIENUM_STOP;

        LPDIRECTINPUTDEVICE8 device = nullptr;
        HRESULT hr = g_directInput->CreateDevice(instance->guidInstance, &device, nullptr);
        if (FAILED(hr) || !device) {
            return DIENUM_CONTINUE;
        }

        hr = device->SetDataFormat(&c_dfDIJoystick2);
        if (FAILED(hr)) {
            device->Release();
            return DIENUM_CONTINUE;
        }

        hr = device->SetCooperativeLevel(ResolveInputCooperativeWindow(), DISCL_NONEXCLUSIVE | DISCL_BACKGROUND);
        if (FAILED(hr)) {
            device->Release();
            return DIENUM_CONTINUE;
        }

        ConfigureAxisRange(device, DIJOFS_X);
        ConfigureAxisRange(device, DIJOFS_Y);
        ConfigureAxisRange(device, DIJOFS_Z);
        ConfigureAxisRange(device, DIJOFS_RX);
        ConfigureAxisRange(device, DIJOFS_RY);
        ConfigureAxisRange(device, DIJOFS_RZ);

        device->Acquire();

        GenericPad pad;
        pad.device = device;
        pad.name = NarrowDeviceName(instance->tszProductName);
        pads->push_back(pad);
        return DIENUM_CONTINUE;
    }

    bool EnsureDirectInputInitializedLocked() {
        if (g_directInput) return true;
        if (g_directInputInitAttempted) return false;
        g_directInputInitAttempted = true;

        HRESULT hr = DirectInput8Create(GetModuleHandle(nullptr), DIRECTINPUT_VERSION,
            IID_IDirectInput8, reinterpret_cast<void**>(&g_directInput), nullptr);
        if (FAILED(hr) || !g_directInput) {
            LogOut("[GAMEPAD] DirectInput fallback initialization failed", true);
            return false;
        }

        LogOut("[GAMEPAD] DirectInput fallback initialized", true);
        return true;
    }

    void EnumerateGenericPadsLocked(bool forceLog) {
        if (!EnsureDirectInputInitializedLocked()) {
            return;
        }

        ReleaseGenericPadsLocked();
        g_lastGenericEnumTick = GetTickCount();

        std::vector<GenericPad> pads;
        g_directInput->EnumDevices(DI8DEVCLASS_GAMECTRL, EnumGenericPadsCallback, &pads, DIEDFL_ATTACHEDONLY);
        g_genericPads.swap(pads);

        if (g_genericPads.empty()) {
            if (g_lastGenericInventorySummary != "none") {
                g_lastGenericInventorySummary = "none";
            }
            if (forceLog) {
                LogOut("[GAMEPAD] No DirectInput fallback controllers detected", true);
            }
            return;
        }

        std::ostringstream oss;
        oss << g_genericPads.size();
        for (size_t i = 0; i < g_genericPads.size(); ++i) {
            oss << "|" << g_genericPads[i].name;
        }
        std::string inventory = oss.str();
        if (!forceLog && inventory == g_lastGenericInventorySummary) {
            return;
        }
        g_lastGenericInventorySummary = inventory;

        LogOut("[GAMEPAD] DirectInput fallback controllers detected: " + std::to_string(g_genericPads.size()), true);
        for (size_t i = 0; i < g_genericPads.size(); ++i) {
            LogOut("[GAMEPAD] DirectInput pad " + std::to_string(i) + ": " + g_genericPads[i].name, true);
        }
    }

    bool PollGenericPadLocked(GenericPad& pad) {
        if (!pad.device) {
            pad.connected = false;
            return false;
        }

        HRESULT hr = pad.device->Poll();
        if (FAILED(hr)) {
            hr = pad.device->Acquire();
            if (FAILED(hr)) {
                pad.connected = false;
                return false;
            }
            hr = pad.device->Poll();
            if (FAILED(hr)) {
                pad.connected = false;
                return false;
            }
        }

        DIJOYSTATE2 state{};
        hr = pad.device->GetDeviceState(sizeof(DIJOYSTATE2), &state);
        if (FAILED(hr)) {
            pad.connected = false;
            return false;
        }

        pad.rawState = state;
        pad.connected = true;
        return true;
    }

    static bool ButtonDown(const DIJOYSTATE2& state, int index) {
        return index >= 0 && index < 128 && (state.rgbButtons[index] & 0x80) != 0;
    }

    static void MapPovToDpad(const DIJOYSTATE2& state, WORD& buttons) {
        DWORD pov = state.rgdwPOV[0];
        if (pov == 0xFFFFFFFF) return;
        if (pov >= 31500 || pov <= 4500) buttons |= XINPUT_GAMEPAD_DPAD_UP;
        if (pov >= 4500 && pov <= 13500) buttons |= XINPUT_GAMEPAD_DPAD_RIGHT;
        if (pov >= 13500 && pov <= 22500) buttons |= XINPUT_GAMEPAD_DPAD_DOWN;
        if (pov >= 22500 && pov <= 31500) buttons |= XINPUT_GAMEPAD_DPAD_LEFT;
    }

    static SHORT ClampAxisToShort(LONG value) {
        if (value > 32767) value = 32767;
        if (value < -32768) value = -32768;
        return static_cast<SHORT>(value);
    }

    static BYTE TriggerFromAxisPositive(LONG value) {
        if (value <= 0) return 0;
        if (value >= 32767) return 255;
        return static_cast<BYTE>((value * 255) / 32767);
    }

    XINPUT_STATE BuildSyntheticState(GenericPad& pad) {
        XINPUT_STATE state{};
        WORD buttons = 0;

        // Conventional PlayStation/DirectInput mapping used by DualShock/DualSense style devices.
        if (ButtonDown(pad.rawState, 1)) buttons |= XINPUT_GAMEPAD_A; // Cross
        if (ButtonDown(pad.rawState, 2)) buttons |= XINPUT_GAMEPAD_B; // Circle
        if (ButtonDown(pad.rawState, 0)) buttons |= XINPUT_GAMEPAD_X; // Square
        if (ButtonDown(pad.rawState, 3)) buttons |= XINPUT_GAMEPAD_Y; // Triangle
        if (ButtonDown(pad.rawState, 4)) buttons |= XINPUT_GAMEPAD_LEFT_SHOULDER;
        if (ButtonDown(pad.rawState, 5)) buttons |= XINPUT_GAMEPAD_RIGHT_SHOULDER;
        if (ButtonDown(pad.rawState, 8)) buttons |= XINPUT_GAMEPAD_BACK;
        if (ButtonDown(pad.rawState, 9)) buttons |= XINPUT_GAMEPAD_START;
        if (ButtonDown(pad.rawState, 10)) buttons |= XINPUT_GAMEPAD_LEFT_THUMB;
        if (ButtonDown(pad.rawState, 11)) buttons |= XINPUT_GAMEPAD_RIGHT_THUMB;
        MapPovToDpad(pad.rawState, buttons);

        state.Gamepad.wButtons = buttons;
        state.Gamepad.bLeftTrigger = ButtonDown(pad.rawState, 6) ? 255 : 0;
        state.Gamepad.bRightTrigger = ButtonDown(pad.rawState, 7) ? 255 : 0;
        if (state.Gamepad.bLeftTrigger == 0 && state.Gamepad.bRightTrigger == 0) {
            // Some DirectInput pads expose analog triggers on Z/Rz instead of digital buttons.
            state.Gamepad.bLeftTrigger = TriggerFromAxisPositive(-pad.rawState.lZ);
            state.Gamepad.bRightTrigger = TriggerFromAxisPositive(pad.rawState.lZ);
            if (state.Gamepad.bRightTrigger == 0) {
                state.Gamepad.bRightTrigger = TriggerFromAxisPositive(pad.rawState.lRz);
            }
        }
        state.Gamepad.sThumbLX = ClampAxisToShort(pad.rawState.lX);
        state.Gamepad.sThumbLY = ClampAxisToShort(-pad.rawState.lY);
        state.Gamepad.sThumbRX = ClampAxisToShort(pad.rawState.lRx);
        state.Gamepad.sThumbRY = ClampAxisToShort(-pad.rawState.lRy);

        if (memcmp(&state.Gamepad, &pad.syntheticState.Gamepad, sizeof(XINPUT_GAMEPAD)) != 0) {
            ++pad.packetCounter;
        }
        state.dwPacketNumber = pad.packetCounter;
        pad.syntheticState = state;
        return state;
    }

    DWORD GetNativeState(DWORD idx, XINPUT_STATE* st) {
        if (!g_xinput && !XInputShim::Init()) return ERROR_DEVICE_NOT_CONNECTED;
        if (!pGetState) return ERROR_DEVICE_NOT_CONNECTED;
        return pGetState(idx, st);
    }

    DWORD GetNativeCapabilities(DWORD idx, DWORD flags, XINPUT_CAPABILITIES* caps) {
        if (!g_xinput && !XInputShim::Init()) return ERROR_DEVICE_NOT_CONNECTED;
        if (!pGetCaps) return ERROR_DEVICE_NOT_CONNECTED;
        return pGetCaps(idx, flags, caps);
    }

    void LogSlotSummaryLocked();

    void UpdateSnapshotLocked() {
        DWORD now = GetTickCount();

        // Avoid re-enumerating DirectInput devices on a fixed 5s cadence while a stable pad
        // is already connected. Device enumeration is comparatively heavy and can cause the
        // exact kind of rare hitch the user is seeing. We only enumerate:
        //  - on startup
        //  - periodically when no generic pads are currently known
        const bool firstGenericEnum = (g_lastGenericEnumTick == 0);
        const bool noKnownGenericPads = g_genericPads.empty();
        if (firstGenericEnum || (noKnownGenericPads && (now - g_lastGenericEnumTick >= 5000))) {
            EnumerateGenericPadsLocked(firstGenericEnum);
        }

        std::array<XINPUT_STATE, 4> nativeStates{};
        unsigned nativeMask = 0;
        for (DWORD i = 0; i < 4; ++i) {
            if (GetNativeState(i, &nativeStates[i]) == ERROR_SUCCESS) {
                nativeMask |= (1u << i);
            } else {
                ZeroMemory(&nativeStates[i], sizeof(XINPUT_STATE));
            }
        }

        auto collectGenericStates = [&]() {
            std::vector<std::pair<XINPUT_STATE, std::string>> out;
            out.reserve(g_genericPads.size());
            for (GenericPad& pad : g_genericPads) {
                if (!PollGenericPadLocked(pad)) {
                    continue;
                }
                out.emplace_back(BuildSyntheticState(pad), pad.name);
            }
            return out;
        };

        std::vector<std::pair<XINPUT_STATE, std::string>> genericStates = collectGenericStates();

        // Recovery path: if we used to know about generic pads but none of them are pollable now,
        // perform a one-shot re-enumeration after a short cooldown. This preserves hot-plug
        // recovery without paying the cost continuously during stable play.
        if (!g_genericPads.empty() && genericStates.empty() && (now - g_lastGenericEnumTick >= 1000)) {
            EnumerateGenericPadsLocked(false);
            genericStates = collectGenericStates();
        }

        g_cachedMask = 0;
        g_cachedNativeMask = nativeMask;
        g_cachedGenericMask = 0;
        for (int i = 0; i < 4; ++i) {
            g_cachedStates[i] = {};
            g_cachedGenericSlots[i] = false;
            g_cachedSlotNames[i].clear();
        }

        for (int i = 0; i < 4; ++i) {
            if (((nativeMask >> i) & 1u) == 0) continue;
            g_cachedStates[i] = nativeStates[i];
            g_cachedMask |= (1u << i);
        }

        int nextGenericSlot = 0;
        for (const auto& entry : genericStates) {
            while (nextGenericSlot < 4 && ((g_cachedMask >> nextGenericSlot) & 1u) != 0) {
                ++nextGenericSlot;
            }
            if (nextGenericSlot >= 4) break;

            g_cachedStates[nextGenericSlot] = entry.first;
            g_cachedGenericSlots[nextGenericSlot] = true;
            g_cachedSlotNames[nextGenericSlot] = entry.second;
            g_cachedMask |= (1u << nextGenericSlot);
            g_cachedGenericMask |= (1u << nextGenericSlot);
            ++nextGenericSlot;
        }

        g_lastRefreshTick = now;
        g_hasInitialSnapshot.store(true, std::memory_order_release);
        LogSlotSummaryLocked();
    }

    void ControllerWatcherThread() {
        LogOut("[GAMEPAD] Background controller watcher started", true);
        while (!g_isShuttingDown.load(std::memory_order_acquire)) {
            DWORD sleepMs = 16;
            {
                std::lock_guard<std::mutex> lock(g_snapshotMutex);
                UpdateSnapshotLocked();
                if (g_cachedMask == 0 && g_genericPads.empty()) {
                    sleepMs = 250;
                } else {
                    // 60 Hz snapshot refresh is enough for menu navigation and hotkeys.
                    // The previous 8 ms native polling bought little responsiveness but
                    // increased wakeups on the background thread.
                    sleepMs = 16;
                }
            }
            if (!g_efzWindowActive.load(std::memory_order_relaxed)
                && !g_guiActive.load(std::memory_order_relaxed)) {
                sleepMs = (std::max)(sleepMs, static_cast<DWORD>(48));
            }
            Sleep(sleepMs);
        }
    }

    void EnsureWatcherStarted() {
        bool expected = false;
        if (g_watcherStarted.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            std::thread(ControllerWatcherThread).detach();
        }
    }

    void LogSlotSummaryLocked() {
        std::ostringstream oss;
        oss << "[GAMEPAD] Snapshot changed: combined=0x" << std::hex << std::uppercase << g_cachedMask
            << " xinput=0x" << g_cachedNativeMask
            << " generic=0x" << g_cachedGenericMask;
        for (int i = 0; i < 4; ++i) {
            if (((g_cachedMask >> i) & 1u) == 0) continue;
            oss << " slot" << std::dec << i << "="
                << (g_cachedGenericSlots[i] ? "Generic:" : "XInput:");
            if (!g_cachedSlotNames[i].empty()) {
                oss << g_cachedSlotNames[i];
            } else {
                oss << (g_cachedGenericSlots[i] ? "DirectInput Controller" : "Pad");
            }
        }
        std::string summary = oss.str();
        if (summary != g_lastSlotSummary) {
            g_lastSlotSummary = summary;
            LogOut(summary, true);
        }
    }
}

namespace XInputShim {
    bool Init() {
        EnsureWatcherStarted();
        if (g_xinput) return true;
        const char* cands[] = { "xinput1_4.dll", "xinput9_1_0.dll", "xinput1_3.dll" };
        for (const char* c : cands) {
            HMODULE h = TryLoad(c);
            if (h) {
                g_xinput = h;
                LogOut(std::string("[XINPUT] Loaded ") + c, true);
                return true;
            }
        }
        LogOut("[XINPUT] No XInput DLL found; gamepad features will use fallback paths only.", true);
        return false;
    }

    bool IsLoaded() { return g_xinput != nullptr; }
    const char* LoadedDllName() { return g_name ? g_name : "(none)"; }

    DWORD GetState(DWORD idx, XINPUT_STATE* st) {
        if (!st || idx > 3) return ERROR_DEVICE_NOT_CONNECTED;
        EnsureWatcherStarted();
        if (!g_hasInitialSnapshot.load(std::memory_order_acquire)) {
            std::lock_guard<std::mutex> lock(g_snapshotMutex);
            if (!g_hasInitialSnapshot.load(std::memory_order_relaxed)) {
                UpdateSnapshotLocked();
            }
        }
        RefreshSnapshotOncePerFrame();
        std::lock_guard<std::mutex> lock(g_snapshotMutex);
        if (((g_cachedMask >> idx) & 1u) == 0) {
            return ERROR_DEVICE_NOT_CONNECTED;
        }
        *st = g_cachedStates[idx];
        return ERROR_SUCCESS;
    }

    DWORD GetCapabilities(DWORD idx, DWORD flags, XINPUT_CAPABILITIES* caps) {
        if (!caps || idx > 3) return ERROR_DEVICE_NOT_CONNECTED;
        EnsureWatcherStarted();

        if (GetNativeCapabilities(idx, flags, caps) == ERROR_SUCCESS) {
            return ERROR_SUCCESS;
        }

        if (!g_hasInitialSnapshot.load(std::memory_order_acquire)) {
            std::lock_guard<std::mutex> initLock(g_snapshotMutex);
            if (!g_hasInitialSnapshot.load(std::memory_order_relaxed)) {
                UpdateSnapshotLocked();
            }
        }
        RefreshSnapshotOncePerFrame();
        std::lock_guard<std::mutex> lock(g_snapshotMutex);
        if (!g_cachedGenericSlots[idx]) {
            return ERROR_DEVICE_NOT_CONNECTED;
        }

        ZeroMemory(caps, sizeof(*caps));
        caps->Type = XINPUT_DEVTYPE_GAMEPAD;
        caps->SubType = XINPUT_DEVSUBTYPE_GAMEPAD;
        caps->Flags = 0;
        caps->Gamepad.wButtons = g_cachedStates[idx].Gamepad.wButtons;
        caps->Gamepad.bLeftTrigger = 0xFF;
        caps->Gamepad.bRightTrigger = 0xFF;
        caps->Gamepad.sThumbLX = 0xFF;
        caps->Gamepad.sThumbLY = 0xFF;
        caps->Gamepad.sThumbRX = 0xFF;
        caps->Gamepad.sThumbRY = 0xFF;
        return ERROR_SUCCESS;
    }

    void Enable(BOOL en) {
        if (!g_xinput && !Init()) return;
        if (pEnable) pEnable(en);
    }

    void RefreshSnapshotOncePerFrame() {
        EnsureWatcherStarted();
    }

    bool IsPadConnectedCached(int index) {
        if (index < 0 || index > 3) return false;
        return ((g_cachedMask >> index) & 1u) != 0;
    }

    unsigned GetConnectedMaskCached() { return g_cachedMask; }
    unsigned GetNativeConnectedMaskCached() { return g_cachedNativeMask; }
    unsigned GetGenericConnectedMaskCached() { return g_cachedGenericMask; }

    const XINPUT_STATE* GetCachedState(int index) {
        if (!IsPadConnectedCached(index)) return nullptr;
        return &g_cachedStates[index];
    }

    bool IsGenericFallbackSlot(int index) {
        if (index < 0 || index > 3) return false;
        return g_cachedGenericSlots[index];
    }

    std::string GetSlotDisplayName(int index) {
        if (index < 0 || index > 3) return std::string();
        return g_cachedSlotNames[index];
    }
}
