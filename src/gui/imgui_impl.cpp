#include "../include/utils/utilities.h"

#include "../include/gui/imgui_impl.h"
#include "../include/utils/xinput_shim.h"
#include "../include/core/logger.h"
#include "../include/gui/imgui_gui.h"
#include "../include/game/practice_hotkey_gate.h"
namespace PracticeOverlayGate { void SetMenuVisible(bool); }
#include "../include/gui/overlay.h" 
#include <stdexcept>
#include <Xinput.h>
#include <algorithm>
#include "../include/utils/pause_integration.h"
// Read UI scale from config to build crisp fonts at the right size
#include "../include/utils/config.h"
// Math helpers
#include <cmath>

// XInput linked dynamically via XInputShim for Wine compatibility

// Global reference to shutdown flag - MOVED OUTSIDE namespace
extern std::atomic<bool> g_isShuttingDown;

// Global state
static bool g_imguiInitialized = false;
// Made non-static to satisfy legacy external references during LTCG; accessor functions should be preferred.
bool g_imguiVisible = false;
static IDirect3DDevice9* g_d3dDevice = nullptr;
static WNDPROC g_originalWndProc = nullptr;

// Virtual cursor/gamepad state
static bool g_useVirtualCursor = false;
static ImVec2 g_virtualCursorPos = ImVec2(200.0f, 200.0f);
static ImVec2 g_overlayCenter = ImVec2(0.0f, 0.0f);
static bool g_requestOverlayFocus = false;
static bool g_lastLeftDown = false;
static bool g_lastRightDown = false;

// Track applied font scale to rebuild atlas only when needed
static float g_lastFontScaleApplied = 0.0f; // 0 = uninitialized
static int   g_lastFontModeApplied  = -1;   // -1 = uninitialized

static inline float ClampF(float v, float lo, float hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

// Helper: check if window is in fullscreen (covering its monitor)
static bool IsFullscreen(HWND hwnd) {
    if (!hwnd) return false;
    WINDOWPLACEMENT wp{ sizeof(WINDOWPLACEMENT) };
    if (!GetWindowPlacement(hwnd, &wp)) return false;
    if (wp.showCmd != SW_SHOWMAXIMIZED && wp.showCmd != SW_SHOWNORMAL && wp.showCmd != SW_SHOW) {
        // Still allow borderless popup fullscreen
    }

    RECT wndRect{};
    if (!GetWindowRect(hwnd, &wndRect)) return false;
    HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{ sizeof(MONITORINFO) };
    if (!GetMonitorInfo(mon, &mi)) return false;
    // Consider fullscreen if window rect matches monitor work area or monitor area
    return EqualRect(&wndRect, &mi.rcMonitor) || EqualRect(&wndRect, &mi.rcWork);
}

// Poll XInput and update a software mouse cursor
static void UpdateVirtualCursor(ImGuiIO& io) {
    const auto& cfg = Config::GetSettings();
    HWND hwnd = FindEFZWindow();
    bool fullscreen = hwnd && IsFullscreen(hwnd);
    const bool allowCursor = cfg.enableVirtualCursor && g_imguiVisible && hwnd && (fullscreen || cfg.virtualCursorAllowWindowed);
    bool wasActive = g_useVirtualCursor;
    g_useVirtualCursor = allowCursor;

    // Track focus transitions to allow recenter on refocus
    static bool s_lastWindowFocused = false;
    bool focusedNow = (hwnd && GetForegroundWindow() == hwnd);
    bool regainedFocus = (focusedNow && !s_lastWindowFocused);
    s_lastWindowFocused = focusedNow;
    // Track edge states for additional user-triggered recenters
    static bool s_lastMiddleDown = false;       // mouse middle button
    static bool s_ignoreMiddleUntilUp = false;  // latch to ignore held MMB after recenter
    static int  s_unstickFrames = 0;            // frames to force-release mouse buttons after recenter
    static bool s_lastLThumbDown = false;       // gamepad left stick click

    // We'll always attempt to provide ImGui gamepad nav inputs when the menu is visible,
    // regardless of whether the virtual cursor feature is enabled.

    // Determine current client rect for clamping and centering
    float clientW = io.DisplaySize.x;
    float clientH = io.DisplaySize.y;
    if (hwnd) {
        RECT rc{}; if (GetClientRect(hwnd, &rc)) { clientW = (float)(rc.right - rc.left); clientH = (float)(rc.bottom - rc.top); }
    }

    if (g_useVirtualCursor && (!wasActive || regainedFocus)) {
        // Center cursor on first activation OR when window regains focus
        ImVec2 target = (g_overlayCenter.x > 0.f && g_overlayCenter.y > 0.f)
            ? g_overlayCenter
            : ImVec2(clientW * 0.5f, clientH * 0.5f);
        g_virtualCursorPos = target;
    }

    // Middle mouse (hardware) recenter when ImGui visible & window focused
    if (focusedNow && g_imguiVisible) {
        SHORT mm = GetAsyncKeyState(VK_MBUTTON);
        bool middleDown = (mm & 0x8000) != 0;
        bool middlePressed = middleDown && !s_lastMiddleDown;
        if (middlePressed) {
            ImVec2 target = (g_overlayCenter.x > 0.f && g_overlayCenter.y > 0.f)
                ? g_overlayCenter
                : ImVec2(clientW * 0.5f, clientH * 0.5f);
            g_virtualCursorPos = target;
            // Bring EFZ to foreground to ensure subsequent input goes to the game
            if (hwnd) {
                // Try to ensure EFZ is foreground before moving the cursor
                ShowWindow(hwnd, SW_RESTORE);
                BringWindowToTop(hwnd);
                SetForegroundWindow(hwnd);
                SetActiveWindow(hwnd);
                SetFocus(hwnd);
            }
            // Also move OS cursor so both cursors converge to the same point
            POINT pt{ (LONG)target.x, (LONG)target.y };
            ClientToScreen(hwnd, &pt);
            SetCursorPos(pt.x, pt.y);
            // Important: release any OS mouse capture and ensure ImGui doesn't think middle button is held
            // This prevents the virtual cursor from feeling "stuck" immediately after recentering.
            ReleaseCapture();
            // Force-release all mouse buttons for a short period to clear any drag/pan states.
            io.AddMouseButtonEvent(0, false); // left up
            io.AddMouseButtonEvent(1, false); // right up
            io.AddMouseButtonEvent(2, false); // middle up
            s_ignoreMiddleUntilUp = true;
            s_unstickFrames = 2; // force-release for a couple frames
            // Re-emit current virtual cursor position so ImGui updates this frame
            io.AddMousePosEvent(g_virtualCursorPos.x, g_virtualCursorPos.y);
            // Ask GUI to focus the overlay window so keyboard/gamepad nav starts at center
            g_requestOverlayFocus = true;
        }
        // While the physical middle button remains down after recenter, keep it logically up for ImGui
        if (s_ignoreMiddleUntilUp) {
            if (middleDown) {
                io.AddMouseButtonEvent(2, false);
            } else {
                s_ignoreMiddleUntilUp = false;
            }
        }
        // Briefly force-release left/right to break any drag captures that might freeze the cursor
        if (s_unstickFrames > 0) {
            io.AddMouseButtonEvent(0, false);
            io.AddMouseButtonEvent(1, false);
            io.AddMouseButtonEvent(2, false);
            // Re-emit position to ensure the software cursor is updated during the debounce
            io.AddMousePosEvent(g_virtualCursorPos.x, g_virtualCursorPos.y);
            --s_unstickFrames;
        }
        s_lastMiddleDown = middleDown;
    }

    // Poll all controllers and aggregate for ImGui nav; keep selected/last-active for virtual cursor
    auto pollController = [](int index, XINPUT_STATE& out) -> bool {
        ZeroMemory(&out, sizeof(out));
    DWORD r = XInputShim::GetState(index, &out);
        return (r == ERROR_SUCCESS);
    };

    static int s_lastActivePad = 0; // remember last pad that produced any input
    XINPUT_STATE states[4]{};
    bool connected[4] = {false,false,false,false};
    unsigned connectedMask = 0;
    for (int i = 0; i < 4; ++i) {
        connected[i] = pollController(i, states[i]);
        if (connected[i]) connectedMask |= (1u << i);
    }

    // (diagnostic logs removed)

    // Update last active pad based on any activity this frame when using All mode
    const int cfgIndex = cfg.controllerIndex;
    if (cfgIndex < 0 || cfgIndex > 3) {
        const int dzL = XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE;
        for (int i = 0; i < 4; ++i) if (connected[i]) {
            const auto& gp = states[i].Gamepad;
            bool anyBtn = gp.wButtons != 0;
            bool trig = gp.bLeftTrigger > 30 || gp.bRightTrigger > 30;
            bool lx = (gp.sThumbLX > dzL || gp.sThumbLX < -dzL);
            bool ly = (gp.sThumbLY > dzL || gp.sThumbLY < -dzL);
            if (anyBtn || trig || lx || ly) { s_lastActivePad = i; break; }
        }
    } else if (cfgIndex >= 0 && cfgIndex <= 3 && connected[cfgIndex]) {
        s_lastActivePad = cfgIndex;
    }

    const bool anyConnected = connectedMask != 0;
    // Ensure ImGui sees a gamepad backend while our menu is visible to enable nav
    if (g_imguiVisible) io.BackendFlags |= ImGuiBackendFlags_HasGamepad; else {
        if (anyConnected) io.BackendFlags |= ImGuiBackendFlags_HasGamepad; else io.BackendFlags &= ~ImGuiBackendFlags_HasGamepad;
    }
    // (no-pad warning removed)

    // Helper for analog normalization
    auto axisToAnalog = [](SHORT v, SHORT dz) -> float {
        int iv = (int)v;
        if (iv > dz) iv -= dz; else if (iv < -dz) iv += dz; else iv = 0;
        float n = (float)iv / (32767.0f - dz);
        if (n > 1.f) n = 1.f; if (n < -1.f) n = -1.f;
        return n;
    };

    // Aggregate ImGui navigation input from ALL connected controllers
    bool kFaceDown=false, kFaceRight=false, kFaceLeft=false, kFaceUp=false;
    bool kL1=false, kR1=false, kBack=false, kStart=false, kL3=false, kR3=false;
    bool kDpadL=false, kDpadR=false, kDpadU=false, kDpadD=false;
    float aLLeft=0.f, aLRight=0.f, aLUp=0.f, aLDown=0.f;
    float aL2=0.f, aR2=0.f;
    if (anyConnected) {
        for (int i = 0; i < 4; ++i) if (connected[i]) {
            const auto& gp = states[i].Gamepad;
            // Buttons OR
            kFaceDown |= (gp.wButtons & XINPUT_GAMEPAD_A) != 0;
            kFaceRight |= (gp.wButtons & XINPUT_GAMEPAD_B) != 0;
            kFaceLeft |= (gp.wButtons & XINPUT_GAMEPAD_X) != 0;
            kFaceUp |= (gp.wButtons & XINPUT_GAMEPAD_Y) != 0;
            kL1 |= (gp.wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER) != 0;
            kR1 |= (gp.wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) != 0;
            kBack |= (gp.wButtons & XINPUT_GAMEPAD_BACK) != 0;
            kStart |= (gp.wButtons & XINPUT_GAMEPAD_START) != 0;
            kL3 |= (gp.wButtons & XINPUT_GAMEPAD_LEFT_THUMB) != 0;
            kR3 |= (gp.wButtons & XINPUT_GAMEPAD_RIGHT_THUMB) != 0;
            kDpadL |= (gp.wButtons & XINPUT_GAMEPAD_DPAD_LEFT) != 0;
            kDpadR |= (gp.wButtons & XINPUT_GAMEPAD_DPAD_RIGHT) != 0;
            kDpadU |= (gp.wButtons & XINPUT_GAMEPAD_DPAD_UP) != 0;
            kDpadD |= (gp.wButtons & XINPUT_GAMEPAD_DPAD_DOWN) != 0;

            // Left stick per-direction max (avoid std::max due to Windows min/max macros)
            float lxNorm = axisToAnalog(gp.sThumbLX, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE);
            float lyNorm = axisToAnalog(gp.sThumbLY, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE);
            {
                float cLeft  = (lxNorm < 0.f) ? -lxNorm : 0.f;
                float cRight = (lxNorm > 0.f) ?  lxNorm : 0.f;
                float cUp    = (lyNorm > 0.f) ?  lyNorm : 0.f;
                float cDown  = (lyNorm < 0.f) ? -lyNorm : 0.f;
                aLLeft  = (aLLeft  > cLeft)  ? aLLeft  : cLeft;
                aLRight = (aLRight > cRight) ? aLRight : cRight;
                aLUp    = (aLUp    > cUp)    ? aLUp    : cUp;
                aLDown  = (aLDown  > cDown)  ? aLDown  : cDown;
            }

            // Triggers max
            {
                float cL2 = gp.bLeftTrigger / 255.0f;
                float cR2 = gp.bRightTrigger / 255.0f;
                aL2 = (aL2 > cL2) ? aL2 : cL2;
                aR2 = (aR2 > cR2) ? aR2 : cR2;
            }
        }
    }

    // Feed ImGui only once with aggregated values (pre-NewFrame)
    if (anyConnected) {
        io.AddKeyEvent(ImGuiKey_GamepadFaceDown,  kFaceDown);
        io.AddKeyEvent(ImGuiKey_GamepadFaceRight, kFaceRight);
        io.AddKeyEvent(ImGuiKey_GamepadFaceLeft,  kFaceLeft);
        io.AddKeyEvent(ImGuiKey_GamepadFaceUp,    kFaceUp);
        io.AddKeyEvent(ImGuiKey_GamepadL1,        kL1);
        io.AddKeyEvent(ImGuiKey_GamepadR1,        kR1);
        io.AddKeyEvent(ImGuiKey_GamepadBack,      kBack);
        io.AddKeyEvent(ImGuiKey_GamepadStart,     kStart);
        io.AddKeyEvent(ImGuiKey_GamepadL3,        kL3);
        io.AddKeyEvent(ImGuiKey_GamepadR3,        kR3);
        io.AddKeyEvent(ImGuiKey_GamepadDpadLeft,  kDpadL);
        io.AddKeyEvent(ImGuiKey_GamepadDpadRight, kDpadR);
        io.AddKeyEvent(ImGuiKey_GamepadDpadUp,    kDpadU);
        io.AddKeyEvent(ImGuiKey_GamepadDpadDown,  kDpadD);
        io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickLeft,  aLLeft  > 0.f, aLLeft);
        io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickRight, aLRight > 0.f, aLRight);
        io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickUp,    aLUp    > 0.f, aLUp);
        io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickDown,  aLDown  > 0.f, aLDown);
        io.AddKeyAnalogEvent(ImGuiKey_GamepadL2,          aL2 > 0.05f, aL2);
        io.AddKeyAnalogEvent(ImGuiKey_GamepadR2,          aR2 > 0.05f, aR2);
    } else {
        // Ensure keys are released if no controllers to avoid sticky inputs
        io.AddKeyEvent(ImGuiKey_GamepadFaceDown,  false);
        io.AddKeyEvent(ImGuiKey_GamepadFaceRight, false);
        io.AddKeyEvent(ImGuiKey_GamepadFaceLeft,  false);
        io.AddKeyEvent(ImGuiKey_GamepadFaceUp,    false);
        io.AddKeyEvent(ImGuiKey_GamepadL1,        false);
        io.AddKeyEvent(ImGuiKey_GamepadR1,        false);
        io.AddKeyEvent(ImGuiKey_GamepadBack,      false);
        io.AddKeyEvent(ImGuiKey_GamepadStart,     false);
        io.AddKeyEvent(ImGuiKey_GamepadL3,        false);
        io.AddKeyEvent(ImGuiKey_GamepadR3,        false);
        io.AddKeyEvent(ImGuiKey_GamepadDpadLeft,  false);
        io.AddKeyEvent(ImGuiKey_GamepadDpadRight, false);
        io.AddKeyEvent(ImGuiKey_GamepadDpadUp,    false);
        io.AddKeyEvent(ImGuiKey_GamepadDpadDown,  false);
        io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickLeft,  false, 0.f);
        io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickRight, false, 0.f);
        io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickUp,    false, 0.f);
        io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickDown,  false, 0.f);
        io.AddKeyAnalogEvent(ImGuiKey_GamepadL2,          false, 0.f);
        io.AddKeyAnalogEvent(ImGuiKey_GamepadR2,          false, 0.f);
    }

    // Keyboard fallback: mirror aggregated nav into arrow/enter/escape keys
    // This helps when ImGui ignores gamepad events due to backend quirks.
    {
        bool navLeft  = kDpadL || (aLLeft  > 0.45f);
        bool navRight = kDpadR || (aLRight > 0.45f);
        bool navUp    = kDpadU || (aLUp    > 0.45f);
        bool navDown  = kDpadD || (aLDown  > 0.45f);
        io.AddKeyEvent(ImGuiKey_LeftArrow,  navLeft);
        io.AddKeyEvent(ImGuiKey_RightArrow, navRight);
        io.AddKeyEvent(ImGuiKey_UpArrow,    navUp);
        io.AddKeyEvent(ImGuiKey_DownArrow,  navDown);
        // Accept/Back
        io.AddKeyEvent(ImGuiKey_Enter,  kFaceDown);
        io.AddKeyEvent(ImGuiKey_Escape, kFaceRight || kBack);
    }

    // (burst debug logging removed)

    // Edge detection for left stick click (L3) on the selected/active pad to recenter virtual cursor
    int selPad = -1;
    if (cfgIndex >= 0 && cfgIndex <= 3) selPad = cfgIndex; else selPad = s_lastActivePad;
    if (selPad < 0 || selPad > 3 || !connected[selPad]) {
        // fallback to first connected
        for (int i = 0; i < 4; ++i) if (connected[i]) { selPad = i; break; }
    }

    if (selPad >= 0 && selPad <= 3 && connected[selPad]) {
        const auto& selGp = states[selPad].Gamepad;
        bool lThumbDown = (selGp.wButtons & XINPUT_GAMEPAD_LEFT_THUMB) != 0;
        bool lThumbPressed = lThumbDown && !s_lastLThumbDown && focusedNow && g_imguiVisible;
        if (lThumbPressed) {
            ImVec2 target = (g_overlayCenter.x > 0.f && g_overlayCenter.y > 0.f)
                ? g_overlayCenter
                : ImVec2(clientW * 0.5f, clientH * 0.5f);
            g_virtualCursorPos = target;
            // Do not yank OS mouse on L3; only snap virtual cursor. Still request overlay focus.
            g_requestOverlayFocus = true;
        }
        s_lastLThumbDown = lThumbDown;

        // Analog stick movement for virtual cursor only from selected pad
        auto applyDeadzone = [](SHORT v, SHORT dz) -> float {
            int iv = (int)v;
            if (iv > dz) iv -= dz; else if (iv < -dz) iv += dz; else iv = 0;
            float n = (float)iv / (32767.0f - dz);
            if (n > 1.f) n = 1.f; if (n < -1.f) n = -1.f;
            return n;
        };

        float nx = applyDeadzone(selGp.sThumbLX, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE);
        float ny = applyDeadzone(selGp.sThumbLY, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE);

        // Apply acceleration/response curve (exponent >1 softens near center for precision)
        if (cfg.virtualCursorAccelPower != 1.0f) {
            auto curve = [&](float v) {
                float sign = (v >= 0.f) ? 1.f : -1.f;
                float mag = fabsf(v);
                mag = powf(mag, cfg.virtualCursorAccelPower);
                return sign * mag;
            };
            nx = curve(nx);
            ny = curve(ny);
        }

        // DPAD provides discrete nudge
        float dpadX = 0.f, dpadY = 0.f;
        if (selGp.wButtons & XINPUT_GAMEPAD_DPAD_RIGHT) dpadX += 1.f;
        if (selGp.wButtons & XINPUT_GAMEPAD_DPAD_LEFT)  dpadX -= 1.f;
        if (selGp.wButtons & XINPUT_GAMEPAD_DPAD_UP)    dpadY -= 1.f;
        if (selGp.wButtons & XINPUT_GAMEPAD_DPAD_DOWN)  dpadY += 1.f;

        float dt = io.DeltaTime > 0.f ? io.DeltaTime : (1.f/60.f);
        const bool fast = (selGp.wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER) != 0;
        float baseSpeed = fast ? cfg.virtualCursorFastSpeed : cfg.virtualCursorBaseSpeed;  // pixels per second
        float dpadSpeed = cfg.virtualCursorDpadSpeed;
        if (baseSpeed < 50.f) baseSpeed = 50.f; if (baseSpeed > 8000.f) baseSpeed = 8000.f;
        if (dpadSpeed < 10.f) dpadSpeed = 10.f; if (dpadSpeed > 4000.f) dpadSpeed = 4000.f;

        if (g_useVirtualCursor) {
            g_virtualCursorPos.x += (nx * baseSpeed + dpadX * dpadSpeed) * dt;
            g_virtualCursorPos.y += (-ny * baseSpeed + dpadY * dpadSpeed) * dt; // note: Y is inverted

            // Clamp within client region size
            g_virtualCursorPos.x = ClampF(g_virtualCursorPos.x, 0.0f, clientW - 1.0f);
            g_virtualCursorPos.y = ClampF(g_virtualCursorPos.y, 0.0f, clientH - 1.0f);

            // Buttons -> mouse clicks from selected pad
            bool leftDown = (selGp.wButtons & XINPUT_GAMEPAD_A) != 0;
            bool rightDown = (selGp.wButtons & XINPUT_GAMEPAD_B) != 0;
            io.AddMouseButtonEvent(0, leftDown);
            io.AddMouseButtonEvent(1, rightDown);
            g_lastLeftDown = leftDown;
            g_lastRightDown = rightDown;
        }
    } else {
        // release L3 edge tracker if nothing connected
        s_lastLThumbDown = false;
    }

    // (per-second pads mask diagnostics removed)

    // Draw ImGui software cursor only when enabled
    if (g_useVirtualCursor) {
        io.MouseDrawCursor = true;
        // Feed position via event API
        io.AddMousePosEvent(g_virtualCursorPos.x, g_virtualCursorPos.y);
    } else {
        io.MouseDrawCursor = false;
    }
}

// Custom WndProc to handle ImGui input
LRESULT CALLBACK ImGuiWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    // Always feed events to ImGui so backend state stays coherent even when UI is hidden
    ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam);

    // Only intercept inputs when our UI is visible AND ImGui wants to capture them
    if (g_imguiVisible) {
        ImGuiIO& io = ImGui::GetIO();
        const bool isMouseMsg = (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) || msg == WM_MOUSEWHEEL || msg == WM_MOUSEHWHEEL;
        const bool isKeyMsg = (msg == WM_KEYDOWN || msg == WM_KEYUP || msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP || msg == WM_CHAR);
        if ((isMouseMsg && io.WantCaptureMouse) || (isKeyMsg && io.WantCaptureKeyboard)) {
            return 1; // swallow event for the game when UI is active and wants it
        }
    }

    return CallWindowProc(g_originalWndProc, hWnd, msg, wParam, lParam);
}

namespace ImGuiImpl {
    // Get DPI scale factor for the window
    static float GetDpiScale() {
        HWND hwnd = FindEFZWindow();
        if (!hwnd) {
            LogOut("[IMGUI] GetDpiScale: No EFZ window found, returning 1.0", true);
            return 1.0f;
        }
        
        // Try to get DPI for the window (Windows 10+)
        typedef UINT(WINAPI* GetDpiForWindowFunc)(HWND);
        HMODULE user32 = GetModuleHandleA("user32.dll");
        if (user32) {
            GetDpiForWindowFunc pGetDpiForWindow = (GetDpiForWindowFunc)GetProcAddress(user32, "GetDpiForWindow");
            if (pGetDpiForWindow) {
                UINT dpi = pGetDpiForWindow(hwnd);
                float scale = (float)dpi / 96.0f;
                LogOut((std::string("[IMGUI] DPI detected: ") + std::to_string(dpi) + " (scale=" + std::to_string(scale) + ")").c_str(), true);
                return scale; // 96 DPI is 100% scaling
            }
        }
        
        // Fallback: Get DPI from DC
        HDC hdc = GetDC(hwnd);
        if (hdc) {
            int dpiX = GetDeviceCaps(hdc, LOGPIXELSX);
            ReleaseDC(hwnd, hdc);
            float scale = (float)dpiX / 96.0f;
            LogOut((std::string("[IMGUI] DPI detected (fallback): ") + std::to_string(dpiX) + " (scale=" + std::to_string(scale) + ")").c_str(), true);
            return scale;
        }
        
        LogOut("[IMGUI] GetDpiScale: Could not detect DPI, returning 1.0", true);
        return 1.0f;
    }

    // Rebuild font atlas for crisp text at a given UI scale. Safe to call multiple times.
    static void UpdateFontAtlasForScale(float uiScale)
    {
        if (!ImGui::GetCurrentContext()) return;
        ImGuiIO& io = ImGui::GetIO();

        // Check desired font mode from config
        const int fontMode = Config::GetSettings().uiFontMode; // 0=Default, 1=Segoe UI

        // Get DPI scale factor
        float dpiScale = GetDpiScale();

        // Clamp and round to nearest hundredth to avoid thrashing on tiny changes
        float s = uiScale * dpiScale; // Combine UI scale with DPI scale
        if (s < 0.70f) s = 0.70f; else if (s > 2.50f) s = 2.50f;
        float sRounded = floorf(s * 100.0f + 0.5f) / 100.0f;
        const bool scaleChanged = !(fabsf(sRounded - g_lastFontScaleApplied) < 0.01f);
        const bool fontChanged  = (fontMode != g_lastFontModeApplied);
        if (!scaleChanged && !fontChanged) return;

    // Rebuild fonts at scaled pixel size for sharp rendering
    // Start from ImGui's default base of ~13 px
    const float basePx = 13.0f;
    const float targetPx = roundf(basePx * sRounded);

        // Invalidate DX9 objects before changing fonts
        ImGui_ImplDX9_InvalidateDeviceObjects();

        io.Fonts->Clear();
        // Keep atlas flags consistent for DX9
    io.Fonts->Flags |= ImFontAtlasFlags_NoPowerOfTwoHeight;
    io.Fonts->TexGlyphPadding = 1;

    ImFontConfig cfg;
        cfg.OversampleH = 3; // Higher oversampling for smoother edges at high DPI
        cfg.OversampleV = 3;
        cfg.PixelSnapH = false; // Allow subpixel positioning for smoother text
        cfg.SizePixels = targetPx; // set explicit pixel size for crisp text

        // Choose font per config: 0=ImGui default, 1=Segoe UI (if available)
        ImFont* defaultFont = nullptr;
        if (fontMode == 1) {
            const char* segoePath = "C:\\Windows\\Fonts\\segoeui.ttf";
            DWORD fa = GetFileAttributesA(segoePath);
            if (fa != INVALID_FILE_ATTRIBUTES && !(fa & FILE_ATTRIBUTE_DIRECTORY)) {
                defaultFont = io.Fonts->AddFontFromFileTTF(segoePath, targetPx, &cfg);
            }
        }
        if (!defaultFont) {
            // Default path: ImGui built-in font
            defaultFont = io.Fonts->AddFontDefault(&cfg);
        }
    io.FontDefault = defaultFont;

        // Recreate device objects with the new atlas
        if (!ImGui_ImplDX9_CreateDeviceObjects()) {
            // If recreation fails, keep previous state and avoid updating the applied marker
            LogOut("[IMGUI] Warning: Failed to recreate DX9 device objects after font rebuild.", true);
            return;
        }

    g_lastFontScaleApplied = sRounded;
    g_lastFontModeApplied  = fontMode;
    LogOut((std::string("[IMGUI] Rebuilt font atlas: scale=") + std::to_string(sRounded) +
        ", px=" + std::to_string((int)targetPx) + ", font=" + (fontMode==0?"Default":"Segoe UI")).c_str(), true);
    }
    bool Initialize(IDirect3DDevice9* device) {
        if (g_imguiInitialized)
            return true;

        LogOut("[IMGUI] Initializing ImGui", true);
        
        if (!device) {
            LogOut("[IMGUI] Error: No valid D3D device provided", true);
            return false;
        }
        
        // Enable DPI awareness for crisp rendering
        ImGui_ImplWin32_EnableDpiAwareness();
        LogOut("[IMGUI] Enabled DPI awareness", true);
        
        g_d3dDevice = device;

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO(); (void)io;
        
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    // Re-enable gamepad navigation for ImGui
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;

    // Make font atlas slightly cheaper to render (DX9)
    io.Fonts->Flags |= ImFontAtlasFlags_NoPowerOfTwoHeight; // avoid unnecessary atlas stretch
    io.Fonts->TexGlyphPadding = 1;
        
        ImGui::StyleColorsDark();
        
    ImGuiStyle& style = ImGui::GetStyle();
    // Avoid baseline upscaling here; per-window UI scale is applied in imgui_gui.cpp
    // Keeping default metrics ensures more integer-aligned sizes and reduces blur
    style.Alpha = 1.0f; // prefer opaque rendering for cheaper blending
    // Safety: ensure valid minimum window size to satisfy ImGui asserts
    if (style.WindowMinSize.x < 1.0f) style.WindowMinSize.x = 16.0f;
    if (style.WindowMinSize.y < 1.0f) style.WindowMinSize.y = 16.0f;
        
        HWND gameWindow = FindEFZWindow();
        if (!gameWindow) {
            LogOut("[IMGUI] Error: Couldn't find EFZ window", true);
            return false;
        }
        
        if (!ImGui_ImplWin32_Init(gameWindow)) {
            LogOut("[IMGUI] Error: ImGui_ImplWin32_Init failed", true);
            return false;
        }
        
        if (!ImGui_ImplDX9_Init(device)) {
            LogOut("[IMGUI] Error: ImGui_ImplDX9_Init failed", true);
            ImGui_ImplWin32_Shutdown();
            return false;
        }

    // Build crisp font atlas at the configured UI scale right after backend init
        {
            float initScale = Config::GetSettings().uiScale;
            UpdateFontAtlasForScale(initScale);
        }
        
    g_originalWndProc = (WNDPROC)SetWindowLongPtr(gameWindow, GWLP_WNDPROC, (LONG_PTR)ImGuiWndProc);
        if (!g_originalWndProc) {
            LogOut("[IMGUI] Error: Failed to hook window procedure", true);
            ImGui_ImplDX9_Shutdown();
            ImGui_ImplWin32_Shutdown();
            return false;
        }
        
    // Let ImGui know we can provide inputs. Disable HasSetMousePos so ImGui doesn't warp OS cursor,
    // we manage OS cursor explicitly (e.g., on middle-click).
    io.BackendFlags |= ImGuiBackendFlags_HasMouseCursors;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    io.BackendFlags &= ~ImGuiBackendFlags_HasSetMousePos;

    ImGuiGui::Initialize();
        
        g_imguiInitialized = true;
        LogOut("[IMGUI] ImGui initialized successfully", true);
        
        return true;
    }
    
    void Shutdown() {
        if (!g_imguiInitialized)
            return;
        
        LogOut("[IMGUI] Shutting down ImGui", true);
        
        // Restore original window procedure
        HWND gameWindow = FindEFZWindow();
        if (gameWindow && g_originalWndProc) {
            SetWindowLongPtr(gameWindow, GWLP_WNDPROC, (LONG_PTR)g_originalWndProc);
        }
        
        ImGui_ImplDX9_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        
        g_imguiInitialized = false;
        g_imguiVisible = false;
        g_d3dDevice = nullptr;
    }
    
    
    bool IsInitialized() {
        return g_imguiInitialized;
    }
    
    void ToggleVisibility() {
        g_imguiVisible = !g_imguiVisible;
        
        if (g_imguiVisible) {
            LogOut("[IMGUI] ImGui interface opened - will render continuously until closed", true);
            // Log nav flags on open
            if (ImGui::GetCurrentContext()) {
                ImGuiIO& io = ImGui::GetIO();
                // Also take an immediate XInput snapshot
                unsigned mask = 0; XINPUT_STATE s; ZeroMemory(&s, sizeof(s));
                for (int i = 0; i < 4; ++i) { if (XInputShim::GetState(i, &s) == ERROR_SUCCESS) mask |= (1u << i); }
                char buf[256];
                // Force-enable nav flags on open for reliability
                io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
                io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
                _snprintf_s(buf, sizeof(buf), _TRUNCATE, "[IMGUI] Open: NavEnableGamepad=%d BackendHasGamepad=%d XInputMask=0x%X (forced)",
                    (io.ConfigFlags & ImGuiConfigFlags_NavEnableGamepad) ? 1 : 0,
                    (io.BackendFlags & ImGuiBackendFlags_HasGamepad) ? 1 : 0,
                    mask);
                LogOut(buf, true);
                LogOut("[IMGUI] Keyboard fallback for nav is active (Arrow/Enter/Escape)", true);
                // (burst debug window removed)
            }
            // Refresh local data now so UI reflects current state on first frame
            ImGuiGui::RefreshLocalData();
        } else {
            LogOut("[IMGUI] ImGui interface closed", true);
            if (ImGui::GetCurrentContext()) {
                ImGuiIO& io = ImGui::GetIO();
                char buf[256];
                _snprintf_s(buf, sizeof(buf), _TRUNCATE, "[IMGUI] Close: NavEnableGamepad=%d BackendHasGamepad=%d",
                    (io.ConfigFlags & ImGuiConfigFlags_NavEnableGamepad) ? 1 : 0,
                    (io.BackendFlags & ImGuiBackendFlags_HasGamepad) ? 1 : 0);
                LogOut(buf, true);
            }
            
            // BUGFIX: Reset the global menuOpen flag when ImGui is closed
            // Use global namespace resolution operator (::) to access the global variable
            ::menuOpen.store(false);
        }

        // Practice Pause integration: mirror EfzRevival pause when menu visible
    PauseIntegration::OnMenuVisibilityChanged(g_imguiVisible);
    PracticeHotkeyGate::NotifyMenuVisibility(g_imguiVisible);
    PracticeOverlayGate::SetMenuVisible(g_imguiVisible);
        
        // Ensure the visibility state persists by setting it in a global
        static bool stateLogged = false;
        if (!stateLogged) {
            LogOut("[IMGUI] Visibility state persistence confirmed", true);
            stateLogged = true;
        }
    }
    
    bool IsVisible() {
        return g_imguiVisible;
    }
    
    LRESULT WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        // Always provide events to ImGui backend
        ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam);

        if (g_imguiVisible) {
            ImGuiIO& io = ImGui::GetIO();
            const bool isMouseMsg = (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) || msg == WM_MOUSEWHEEL || msg == WM_MOUSEHWHEEL;
            const bool isKeyMsg = (msg == WM_KEYDOWN || msg == WM_KEYUP || msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP || msg == WM_CHAR);
            if ((isMouseMsg && io.WantCaptureMouse) || (isKeyMsg && io.WantCaptureKeyboard)) {
                return 1; // handled by ImGui
            }
        }
        return 0;
    }
    
    // Expose pre-/post- hooks for the active render loop (overlay EndScene)
    void PreNewFrameInputs() {
        if (!g_imguiInitialized || g_isShuttingDown.load()) return;
        if (!ImGui::GetCurrentContext()) return;

        // Align ImGui IO to the game's fixed 640x480 render target and remap mouse to RT space
        // This fixes mouse misalignment when the window client area is larger than 640x480.
        ImGuiIO& io = ImGui::GetIO();

        // Base game backbuffer size
        const float baseW = 640.0f;
        const float baseH = 480.0f;

        // Always force ImGui to render against the backbuffer size (not the window size)
        io.DisplaySize = ImVec2(baseW, baseH);
        io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);

        // Remap OS mouse to backbuffer coordinates (letterbox/pillarbox aware)
        HWND hwnd = FindEFZWindow();
        if (hwnd) {
            RECT rc{};
            if (GetClientRect(hwnd, &rc)) {
                const float cw = (float)(rc.right - rc.left);
                const float ch = (float)(rc.bottom - rc.top);
                if (cw > 0.0f && ch > 0.0f) {
                    // Scale to preserve 4:3 inside client area
                    const float sx = cw / baseW;
                    const float sy = ch / baseH;
                    const float scale = (sx < sy) ? sx : sy;
                    const float gw = baseW * scale;
                    const float gh = baseH * scale;
                    const float ox = (cw - gw) * 0.5f;
                    const float oy = (ch - gh) * 0.5f;

                    // Query current cursor position in client coordinates
                    POINT pt{};
                    if (GetCursorPos(&pt)) {
                        ScreenToClient(hwnd, &pt);
                        float mx = (float)pt.x;
                        float my = (float)pt.y;
                        // Map into the game viewport then into 640x480 space
                        if (scale > 0.0f) {
                            mx = (mx - ox) / scale;
                            my = (my - oy) / scale;
                        }
                        // Clamp to RT bounds
                        if (mx < 0.0f) mx = 0.0f; else if (mx > baseW - 1.0f) mx = baseW - 1.0f;
                        if (my < 0.0f) my = 0.0f; else if (my > baseH - 1.0f) my = baseH - 1.0f;

                        // Feed corrected mouse position to ImGui (after backend NewFrame)
                        io.AddMousePosEvent(mx, my);
                    }
                }
            }
        }

        // Note: Virtual cursor/gamepad aggregation happens later in UpdateVirtualCursor during
        // the alternate RenderFrame path, and for the EndScene path we only need OS mouse remap here.
    }

    // (PostNewFrameDiagnostics removed)

    void RenderFrame() {
        // Safety check
        if (!g_imguiInitialized || !g_d3dDevice || g_isShuttingDown.load()) {
            return;
        }
        
        // Only render if visible
        if (!g_imguiVisible) {
            return;
        }
        
        try {
            // Prepare backend new-frame data first
            ImGui_ImplDX9_NewFrame();
            ImGui_ImplWin32_NewFrame();
            // Feed our gamepad/virtual cursor inputs AFTER backend NewFrame so our events
            // override OS mouse position provided by the backend, then before ImGui::NewFrame
            {
                ImGuiIO& io = ImGui::GetIO();
                UpdateVirtualCursor(io);
            }
            ImGui::NewFrame();
            // Post-NewFrame diagnostics: verify ImGui processed our gamepad events (throttled)
            PostNewFrameDiagnostics();
            // Skip rendering if minimized to avoid style asserts (DisplaySize == 0)
            ImGuiIO& io = ImGui::GetIO();
            if (io.DisplaySize.x <= 0.0f || io.DisplaySize.y <= 0.0f) {
                ImGui::EndFrame();
                return;
            }
            // Maintain pause while menu is visible (guards against stray unfreeze)
            PauseIntegration::MaintainFreezeWhileMenuVisible();


            // Keep font atlas in sync with current UI scale for crisp text
            UpdateFontAtlasForScale(Config::GetSettings().uiScale);

            // Render the GUI
            ImGuiGui::RenderGui();
            
            // End frame and render
            ImGui::EndFrame();
            ImGui::Render();
            if (io.DisplaySize.x > 0.0f && io.DisplaySize.y > 0.0f) {
                ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
            }
        } catch (...) {
            // Silently catch any exceptions during rendering to prevent crashes
        }
    }

    void SetOverlayCenter(const ImVec2& center) {
        g_overlayCenter = center;
    }

    bool ConsumeOverlayFocusRequest() {
        bool v = g_requestOverlayFocus;
        g_requestOverlayFocus = false;
        return v;
    }
}