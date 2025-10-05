#include <windows.h>
#include <hidsdi.h>
#include <xinput.h>
#include <string>
#include <vector>
#include <regex>
#include <setupapi.h>
#include <devguid.h>
#include <cfgmgr32.h>
#pragma comment(lib, "xinput9_1_0.lib")
#pragma comment(lib, "hid.lib")
#pragma comment(lib, "setupapi.lib")

// Helper: trim whitespace
static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    size_t b = s.find_last_not_of(" \t\r\n");
    if (a == std::string::npos) return std::string();
    return s.substr(a, b - a + 1);
}

// Try to read ProductString from a HID handle
static std::string GetHidProductString(HANDLE h) {
    wchar_t wbuf[256];
    if (HidD_GetProductString(h, wbuf, sizeof(wbuf))) {
        int len = WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, nullptr, 0, nullptr, nullptr);
        std::string out(len > 0 ? len - 1 : 0, '\0');
        if (len > 0) WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, out.data(), len, nullptr, nullptr);
        return trim(out);
    }
    return std::string();
}

// Best-effort: map RI device to XInput slot by instance path heuristic containing "IG_" index
static int GuessXInputIndexFromPath(const std::string& path) {
    // Look for "IG_\d" near the end; not guaranteed but common for XInput wrapper devices
    std::regex igRe("IG_([0-9]+)", std::regex_constants::icase);
    std::smatch m;
    if (std::regex_search(path, m, igRe)) {
        try { return std::stoi(m[1].str()); } catch (...) {}
    }
    return -1;
}

std::string GetControllerNameForIndex(int userIndex) {
    // Simple cache to avoid frequent enumeration cost when UI is open
    struct Cache {
        DWORD tick = 0;
        std::string names[4];
        bool valid[4] = {false,false,false,false};
    };
    static Cache cache;
    auto now = GetTickCount();
    auto getCached = [&](int idx) -> std::string {
        if (idx < 0 || idx > 3) return std::string();
        if (cache.valid[idx] && (now - cache.tick) < 2000) {
            return cache.names[idx];
        }
        return std::string();
    };
    auto setCached = [&](int idx, const std::string& s) {
        if (idx < 0 || idx > 3) return;
        cache.names[idx] = s;
        cache.valid[idx] = true;
        cache.tick = now;
    };

    if (userIndex < 0 || userIndex > 3) return std::string("All (Any)");

    // If not connected, report clearly
    XINPUT_STATE st{};
    if (XInputGetState(userIndex, &st) != ERROR_SUCCESS) {
        return std::string("(Disconnected) Pad ") + std::to_string(userIndex);
    }

    // Try cache
    if (auto cached = getCached(userIndex); !cached.empty()) return cached;

    // Prefer a stable name based on XInput capabilities (avoids Raw Input mislabeling headsets/duplicates)
    XINPUT_CAPABILITIES caps{};
    if (XInputGetCapabilities(userIndex, XINPUT_FLAG_GAMEPAD, &caps) == ERROR_SUCCESS) {
        std::string base;
        switch (caps.SubType) {
            case XINPUT_DEVSUBTYPE_GAMEPAD:       base = "XInput Gamepad"; break;
            case XINPUT_DEVSUBTYPE_WHEEL:         base = "XInput Racing Wheel"; break;
            case XINPUT_DEVSUBTYPE_ARCADE_STICK:  base = "XInput Arcade Stick"; break;
            case XINPUT_DEVSUBTYPE_FLIGHT_STICK:  base = "XInput Flight Stick"; break;
            case XINPUT_DEVSUBTYPE_DANCE_PAD:     base = "XInput Dance Pad"; break;
            case XINPUT_DEVSUBTYPE_GUITAR:        base = "XInput Guitar"; break;
            case XINPUT_DEVSUBTYPE_GUITAR_ALTERNATE: base = "XInput Guitar (Alt)"; break;
            case XINPUT_DEVSUBTYPE_DRUM_KIT:      base = "XInput Drum Kit"; break;
            case XINPUT_DEVSUBTYPE_GUITAR_BASS:   base = "XInput Bass Guitar"; break;
            default:                               base = "XInput Controller"; break;
        }
        // Include pad index for clarity
        std::string name = base + std::string(" (Pad ") + std::to_string(userIndex) + ")";
        setCached(userIndex, name);
        return name;
    }

    // Fallback: enumerate raw input devices of type gamepad/joystick to try a product string
    UINT num = 0;
    if (GetRawInputDeviceList(nullptr, &num, sizeof(RAWINPUTDEVICELIST)) != 0 || num == 0) {
        return std::string("Controller ") + std::to_string(userIndex);
    }
    std::vector<RAWINPUTDEVICELIST> list(num);
    if (GetRawInputDeviceList(list.data(), &num, sizeof(RAWINPUTDEVICELIST)) == (UINT)-1) {
        return std::string("Controller ") + std::to_string(userIndex);
    }

    for (UINT i = 0; i < num; ++i) {
        if (list[i].dwType != RIM_TYPEHID) continue;
        // Filter to HID Gamepad/Joystick only: Usage Page 0x01 (Generic Desktop), Usage 0x04 (Joystick) or 0x05 (Game Pad)
        RID_DEVICE_INFO ridi{}; ridi.cbSize = sizeof(RID_DEVICE_INFO); UINT isz = sizeof(RID_DEVICE_INFO);
        if (GetRawInputDeviceInfo(list[i].hDevice, RIDI_DEVICEINFO, &ridi, &isz) == (UINT)-1) continue;
        if (ridi.dwType != RIM_TYPEHID) continue;
        USHORT usagePage = ridi.hid.usUsagePage;
        USHORT usage = ridi.hid.usUsage;
        if (usagePage != 0x01 || !(usage == 0x04 || usage == 0x05)) continue; // skip headsets, keyboards, mice, consumer devices
        UINT size = 0;
        if (GetRawInputDeviceInfoA(list[i].hDevice, RIDI_DEVICENAME, nullptr, &size) != 0 || size == 0) continue;
        std::string path(size, '\0');
        if (GetRawInputDeviceInfoA(list[i].hDevice, RIDI_DEVICENAME, path.data(), &size) == (UINT)-1) continue;
        path.resize(path.find('\0') != std::string::npos ? path.find('\0') : path.size());

        // Try to relate this HID to the target XInput user index
        int guessed = GuessXInputIndexFromPath(path);
        if (guessed != -1 && guessed != userIndex) continue; // different slot

    // Open for HID product string
        HANDLE h = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            // Try read-only
            h = CreateFileA(path.c_str(), GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        }
        if (h != INVALID_HANDLE_VALUE) {
            std::string prod = GetHidProductString(h);
            CloseHandle(h);
            if (!prod.empty()) { setCached(userIndex, prod); return prod; }
        }
    }

    // Fallback to a generic but nicer label
    {
        std::string fb = std::string("XInput Controller ") + std::to_string(userIndex);
        setCached(userIndex, fb);
        return fb;
    }
}
