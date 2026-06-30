#include "../include/gui/imgui_impl.h"
#include "../include/utils/xinput_shim.h"
#include "../include/core/logger.h"
#include "../include/gui/imgui_gui.h"
#include "../include/gui/custom_menu/renderer.h"
#include "../include/game/practice_hotkey_gate.h"
namespace PracticeOverlayGate { void SetMenuVisible(bool); }
#include "../include/gui/overlay.h" 
#include "../include/utils/utilities.h"
#include "../include/core/memory.h"
#include "../include/utils/switch_players.h"
#include "../include/utils/xp_compat.h"
#include <stdexcept>
#include <Xinput.h>
#include <algorithm>
#include "../include/utils/pause_integration.h"
// Read UI scale from config to build crisp fonts at the right size
#include "../include/utils/config.h"
// Math helpers
#include <cmath>
// Throttle timers
#include <chrono>
#include <thread>
// Hotkey cooldown
#include "../include/input/input_handler.h"

// XInput linked dynamically via XInputShim for Wine compatibility

// Global reference to shutdown flag - MOVED OUTSIDE namespace
extern std::atomic<bool> g_isShuttingDown;

// Thread-safe initialization guard
static std::atomic<bool> s_imguiInitInProgress{false};

// Global state
static bool g_imguiInitialized = false;
// Made non-static to satisfy legacy external references during LTCG; accessor functions should be preferred.
bool g_imguiVisible = false;
namespace CharacterSettings {
    std::atomic<bool> g_guiVisible{false};
}
static IDirect3DDevice9* g_d3dDevice = nullptr;
static WNDPROC g_originalWndProc = nullptr;
static HWND g_imguiHostWindow = nullptr;
static bool g_externalFallbackHost = false;
static std::atomic<bool> g_externalFallbackThreadRunning{false};
static std::atomic<bool> g_externalFallbackReady{false};
static std::atomic<bool> g_externalFallbackInitFailed{false};
static std::atomic<bool> g_externalFallbackExit{false};
static HWND g_externalFallbackWindow = nullptr;
static const char* kFallbackWindowClassName = "EFZ_TM_IMGUI_FALLBACK";

// Virtual cursor/gamepad state
static bool g_useVirtualCursor = false;
static ImVec2 g_virtualCursorPos = ImVec2(200.0f, 200.0f);
static ImVec2 g_overlayCenter = ImVec2(0.0f, 0.0f);
static bool g_requestOverlayFocus = false;
static bool g_lastLeftDown = false;
static bool g_lastRightDown = false;
// Flag to request virtual cursor activation state reset on next update (e.g. menu just opened in fullscreen)
static bool g_resetVirtualCursorOnOpen = false;

// Track applied font scale to rebuild atlas only when needed
static float g_lastFontScaleApplied = 0.0f; // 0 = uninitialized
static int   g_lastFontModeApplied  = -1;   // -1 = uninitialized

static HWND GetActiveHostWindow() {
    if (g_imguiHostWindow && IsWindow(g_imguiHostWindow)) {
        return g_imguiHostWindow;
    }
    return FindEFZWindow();
}

static inline float ClampF(float v, float lo, float hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static bool IsVkDownForImGuiNav(int vk) {
    return vk > 0 && (GetAsyncKeyState(vk) & 0x8000) != 0;
}

static uint8_t ReadMenuGameplayInputs() {
    int localSide = SwitchPlayers::GetLocalSide();
    int localPlayer = 1;
    if (localSide == 1) {
        localPlayer = 2;
    }

    uint8_t inputs = GetPlayerInputs(localPlayer);
    if (inputs == 0 && localPlayer != 1) {
        // Menu routing is frequently reset to P1-local; fall back there if the local-side read is empty.
        inputs = GetPlayerInputs(1);
    }
    return inputs;
}

struct CachedWindowMetrics {
    HWND hwnd = nullptr;
    DWORD tick = 0;
    bool fullscreen = false;
    float clientW = 0.0f;
    float clientH = 0.0f;
};

static void GetCachedWindowMetrics(HWND hwnd, bool& fullscreen, float& clientW, float& clientH) {
    static CachedWindowMetrics s_cache{};
    if (!hwnd) {
        fullscreen = false;
        clientW = 0.0f;
        clientH = 0.0f;
        s_cache = {};
        return;
    }

    const DWORD now = GetTickCount();
    if (hwnd == s_cache.hwnd && s_cache.tick != 0 && (now - s_cache.tick) < 125) {
        fullscreen = s_cache.fullscreen;
        clientW = s_cache.clientW;
        clientH = s_cache.clientH;
        return;
    }

    RECT clientRect{};
    float cachedClientW = 0.0f;
    float cachedClientH = 0.0f;
    if (GetClientRect(hwnd, &clientRect)) {
        cachedClientW = static_cast<float>(clientRect.right - clientRect.left);
        cachedClientH = static_cast<float>(clientRect.bottom - clientRect.top);
    }

    WINDOWPLACEMENT wp{ sizeof(WINDOWPLACEMENT) };
    RECT wndRect{};
    HMONITOR mon = nullptr;
    MONITORINFO mi{ sizeof(MONITORINFO) };
    bool cachedFullscreen = false;
    if (GetWindowPlacement(hwnd, &wp)
        && GetWindowRect(hwnd, &wndRect)
        && (mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY)) != nullptr
        && GetMonitorInfo(mon, &mi)) {
        cachedFullscreen = EqualRect(&wndRect, &mi.rcMonitor) || EqualRect(&wndRect, &mi.rcWork);
    }

    s_cache.hwnd = hwnd;
    s_cache.tick = now;
    s_cache.fullscreen = cachedFullscreen;
    s_cache.clientW = cachedClientW;
    s_cache.clientH = cachedClientH;

    fullscreen = cachedFullscreen;
    clientW = cachedClientW;
    clientH = cachedClientH;
}

static bool IsFullscreen(HWND hwnd) {
    bool fullscreen = false;
    float clientW = 0.0f;
    float clientH = 0.0f;
    GetCachedWindowMetrics(hwnd, fullscreen, clientW, clientH);
    return fullscreen;
}

static void ReleaseGamepadNavInputs(ImGuiIO& io) {
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
    io.AddKeyEvent(ImGuiKey_Enter, false);
    io.AddKeyEvent(ImGuiKey_Escape, false);
}

// Poll XInput and update a software mouse cursor
static void UpdateVirtualCursor(ImGuiIO& io) {
    const auto& cfg = Config::GetSettings();
    HWND hwnd = GetActiveHostWindow();
    float clientW = io.DisplaySize.x;
    float clientH = io.DisplaySize.y;
    bool fullscreen = false;
    if (hwnd) {
        GetCachedWindowMetrics(hwnd, fullscreen, clientW, clientH);
    }
    
    // Track physical mouse movement to enable cursor only when mouse is actually used
    static ImVec2 s_lastMousePos = ImVec2(-1.0f, -1.0f);
    static bool s_mouseHasMoved = false;
    
    // Check for real mouse movement (only in fullscreen)
    if (fullscreen && hwnd) {
        POINT pt;
        if (GetCursorPos(&pt) && ScreenToClient(hwnd, &pt)) {
            ImVec2 currentPos((float)pt.x, (float)pt.y);
            if (s_lastMousePos.x >= 0.0f && s_lastMousePos.y >= 0.0f) {
                float dx = currentPos.x - s_lastMousePos.x;
                float dy = currentPos.y - s_lastMousePos.y;
                float distSq = dx * dx + dy * dy;
                if (distSq > 4.0f) { // 2px threshold
                    s_mouseHasMoved = true;
                }
            }
            s_lastMousePos = currentPos;
        }
    }
    
    // Only allow virtual cursor in fullscreen when mouse has actually moved
    const bool allowCursor = cfg.enableVirtualCursor && g_imguiVisible && hwnd && fullscreen && s_mouseHasMoved;
    bool wasActive = g_useVirtualCursor;
    g_useVirtualCursor = allowCursor;
    
    // Reset mouse moved flag when menu closes
    if (!g_imguiVisible) {
        s_mouseHasMoved = false;
    }

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
    static bool s_releasedGamepadWhileHidden = false;
    if (!g_imguiVisible) {
        if (!s_releasedGamepadWhileHidden) {
            ReleaseGamepadNavInputs(io);
            s_releasedGamepadWhileHidden = true;
        }
        g_useVirtualCursor = false;
        io.MouseDrawCursor = false;
        return;
    }
    s_releasedGamepadWhileHidden = false;

    // The custom menu (Config::useCustomMenu) owns its own keyboard/dpad input
    // model and reads io.MousePos only for *real* mouse hover. The gamepad-driven
    // virtual cursor below is a legacy-ImGui-menu feature; while the custom menu
    // is active it would feed analog-stick motion (including resting stick drift)
    // into io.MousePos every frame, which the custom menu interprets as the mouse
    // sweeping across rows - the "menu navigates by itself in fullscreen when a
    // controller is plugged" bug. Skip it so io.MousePos reflects only the real
    // OS cursor fed by PreNewFrameInputs(); dpad/keyboard nav is unaffected.
    if (cfg.useCustomMenu) {
        ReleaseGamepadNavInputs(io);
        g_useVirtualCursor = false;
        return;
    }

    // Determine current client rect for clamping and centering
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

    // Ensure XInput snapshot is fresh for this frame
    XInputShim::RefreshSnapshotOncePerFrame();
    // Poll from cached snapshot to avoid redundant syscalls
    auto pollController = [](int index, XINPUT_STATE& out) -> bool {
        const XINPUT_STATE* s = XInputShim::GetCachedState(index);
        if (!s) { ZeroMemory(&out, sizeof(out)); return false; }
        out = *s; return true;
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
    const float navAnalogThreshold = ClampF(cfg.guiNavAnalogThreshold, 0.05f, 0.95f);

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
            if (XInputShim::IsGenericFallbackSlot(i)) {
                // Generic DirectInput pads sometimes surface their only usable navigation axes
                // on what looks like the right stick after translation. Prefer the stronger pair.
                float rxNorm = axisToAnalog(gp.sThumbRX, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE);
                float ryNorm = axisToAnalog(gp.sThumbRY, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE);
                const float leftStrength = (std::max)(std::fabs(lxNorm), std::fabs(lyNorm));
                const float rightStrength = (std::max)(std::fabs(rxNorm), std::fabs(ryNorm));
                if (rightStrength > leftStrength) {
                    lxNorm = rxNorm;
                    lyNorm = ryNorm;
                }
            }
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

    // Also mirror the same gameplay-facing inputs the match already recognizes.
    // This keeps ImGui navigation compatible with non-XInput devices that still work in-game.
    {
        const uint8_t menuInputs = ReadMenuGameplayInputs();
        kDpadL |= (menuInputs & INPUT_LEFT)  != 0;
        kDpadR |= (menuInputs & INPUT_RIGHT) != 0;
        kDpadU |= (menuInputs & INPUT_UP)    != 0;
        kDpadD |= (menuInputs & INPUT_DOWN)  != 0;
        kFaceDown  |= (menuInputs & INPUT_A) != 0;
        kFaceRight |= (menuInputs & INPUT_B) != 0;
        kFaceLeft  |= (menuInputs & INPUT_C) != 0;
        kFaceUp    |= (menuInputs & INPUT_D) != 0;
    }

    // Supplement the gameplay bitmask with any discovered keyboard bindings so paused-menu
    // navigation still works even if the game-side input byte stalls for a frame.
    if (detectedBindings.directionsDetected) {
        kDpadL |= IsVkDownForImGuiNav(detectedBindings.leftKey);
        kDpadR |= IsVkDownForImGuiNav(detectedBindings.rightKey);
        kDpadU |= IsVkDownForImGuiNav(detectedBindings.upKey);
        kDpadD |= IsVkDownForImGuiNav(detectedBindings.downKey);
    }
    if (detectedBindings.attacksDetected) {
        kFaceDown  |= IsVkDownForImGuiNav(detectedBindings.aButton);
        kFaceRight |= IsVkDownForImGuiNav(detectedBindings.bButton);
        kFaceLeft  |= IsVkDownForImGuiNav(detectedBindings.cButton);
        kFaceUp    |= IsVkDownForImGuiNav(detectedBindings.dButton);
    }

    // Merge all directional sources into a single raw signal per direction,
    // then gate it through a "just-pressed → hold → repeat-with-acceleration"
    // state machine.  This prevents double-taps from multiple input sources
    // (XInput dpad, analog stick, gameplay byte, keyboard) all independently
    // triggering ImGui navigation, and gives a human-friendly feel:
    //   • tap  → exactly one nav step
    //   • hold < 1 s  → still just one step
    //   • hold ≥ 1 s  → start repeating, accelerating over time
    {
        // Combine every source into one bool per direction (dpad + stick + gameplay + keyboard)
        bool rawDir[4] = {
            kDpadL || (aLLeft  >= navAnalogThreshold),
            kDpadR || (aLRight >= navAnalogThreshold),
            kDpadU || (aLUp    >= navAnalogThreshold),
            kDpadD || (aLDown  >= navAnalogThreshold),
        };

        // Per-direction state persisted across frames
        static bool  s_prevRaw[4]        = {false, false, false, false};
        static float s_holdTime[4]       = {0, 0, 0, 0};
        static float s_nextRepeatAt[4]   = {0, 0, 0, 0};

        const float holdThreshold = 1.0f; // seconds before repeat kicks in

        bool gated[4];
        for (int i = 0; i < 4; i++) {
            if (rawDir[i]) {
                if (!s_prevRaw[i]) {
                    // Rising edge – emit one nav step ("just pressed")
                    gated[i] = true;
                    s_holdTime[i] = 0.f;
                    s_nextRepeatAt[i] = holdThreshold;
                } else {
                    s_holdTime[i] += io.DeltaTime;
                    if (s_holdTime[i] >= s_nextRepeatAt[i]) {
                        // Emit a repeat pulse
                        gated[i] = true;
                        // Acceleration: rate increases with hold duration
                        float elapsed = s_holdTime[i] - holdThreshold;
                        float interval;
                        if (elapsed < 0.5f)       interval = 0.18f;  // ~6/s
                        else if (elapsed < 1.5f)   interval = 0.09f;  // ~11/s
                        else                        interval = 0.045f; // ~22/s
                        s_nextRepeatAt[i] = s_holdTime[i] + interval;
                    } else {
                        gated[i] = false; // suppress during dead zone
                    }
                }
            } else {
                gated[i] = false;
                s_holdTime[i] = 0.f;
                s_nextRepeatAt[i] = 0.f;
            }
            s_prevRaw[i] = rawDir[i];
        }

        // Write gated values back; ALL directional nav is driven solely
        // through these gated dpad bools.  LStick analog is zeroed to prevent
        // ImGui's NavUpdate from reading AnalogValue and double-triggering nav.
        kDpadL = gated[0]; kDpadR = gated[1];
        kDpadU = gated[2]; kDpadD = gated[3];
        aLLeft = 0.f; aLRight = 0.f; aLUp = 0.f; aLDown = 0.f;
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
        // LStick: fully zeroed - nav is handled exclusively by gated dpad above.
        // ImGui reads AnalogValue directly in NavUpdate regardless of the bool,
        // so passing non-zero analog would double-trigger every nav step.
        io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickLeft,  false, 0.f);
        io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickRight, false, 0.f);
        io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickUp,    false, 0.f);
        io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickDown,  false, 0.f);
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

    // Keyboard fallback (TEMP DISABLED for arrow keys due to assertions):
    // We only mirror Accept/Cancel to Enter/Escape. Real arrow key input will come from Win32 backend.
    // If needed later, re-enable arrow mapping after root cause is identified.
    io.AddKeyEvent(ImGuiKey_Enter,  kFaceDown);
    io.AddKeyEvent(ImGuiKey_Escape, kFaceRight || kBack);

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

        // UI tab/sub-tab controller bindings (edge-detected)
        {
            const auto& cfg2 = Config::GetSettings();
            auto isBindingDown = [&](int mask)->bool {
                if (mask < 0) return false;
                if (mask == 0x10000) return selGp.bLeftTrigger > 30;  // LT threshold
                if (mask == 0x20000) return selGp.bRightTrigger > 30; // RT threshold
                return (selGp.wButtons & (WORD)mask) != 0;
            };
            static bool s_lastTopPrev=false, s_lastTopNext=false, s_lastSubPrev=false, s_lastSubNext=false;
            bool nowTopPrev = isBindingDown(cfg2.gpUiTopTabPrev);
            bool nowTopNext = isBindingDown(cfg2.gpUiTopTabNext);
            bool nowSubPrev = isBindingDown(cfg2.gpUiSubTabPrev);
            bool nowSubNext = isBindingDown(cfg2.gpUiSubTabNext);
            bool pressTopPrev = nowTopPrev && !s_lastTopPrev;
            bool pressTopNext = nowTopNext && !s_lastTopNext;
            bool pressSubPrev = nowSubPrev && !s_lastSubPrev;
            bool pressSubNext = nowSubNext && !s_lastSubNext;
            if (g_imguiVisible) {
                if (pressTopPrev) ImGuiGui::RequestTopTabCycle(-1);
                if (pressTopNext) ImGuiGui::RequestTopTabCycle(+1);
                if (pressSubPrev) ImGuiGui::RequestActiveSubTabCycle(-1);
                if (pressSubNext) ImGuiGui::RequestActiveSubTabCycle(+1);
            }
            s_lastTopPrev = nowTopPrev;
            s_lastTopNext = nowTopNext;
            s_lastSubPrev = nowSubPrev;
            s_lastSubNext = nowSubNext;
        }

        // Virtual cursor is now mouse-only - gamepad controls are disabled
        // Analog stick and dpad input still used for ImGui navigation, but not for cursor movement

        float dt = io.DeltaTime > 0.f ? io.DeltaTime : (1.f/60.f);

        // No gamepad cursor movement - virtual cursor only responds to physical mouse
        // The cursor position is updated from OS mouse movement when enabled

        // Right-stick -> mouse wheel (scroll) for GUI navigation
        if (Config::GetSettings().guiScrollRightStickEnable) {
            auto applyDeadzone = [](SHORT v, SHORT dz) -> float {
                int iv = (int)v;
                if (iv > dz) iv -= dz; else if (iv < -dz) iv += dz; else iv = 0;
                float n = (float)iv / (32767.0f - dz);
                if (n > 1.f) n = 1.f; if (n < -1.f) n = -1.f;
                return n;
            };
            
            bool genericNavBorrowedRightStick = false;
            if (XInputShim::IsGenericFallbackSlot(selPad)) {
                float lxNorm = applyDeadzone(selGp.sThumbLX, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE);
                float lyNorm = applyDeadzone(selGp.sThumbLY, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE);
                float rxNorm = applyDeadzone(selGp.sThumbRX, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE);
                float ryNorm = applyDeadzone(selGp.sThumbRY, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE);
                const float leftStrength = (std::max)(std::fabs(lxNorm), std::fabs(lyNorm));
                const float rightStrength = (std::max)(std::fabs(rxNorm), std::fabs(ryNorm));
                genericNavBorrowedRightStick = rightStrength > leftStrength;
            }

            float rx = genericNavBorrowedRightStick ? 0.0f : applyDeadzone(selGp.sThumbRX, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE);
            float ry = genericNavBorrowedRightStick ? 0.0f : applyDeadzone(selGp.sThumbRY, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE);
            // Small deadzone to avoid noise
            const float dz = 0.15f;
            if (fabsf(rx) < dz) rx = 0.0f;
            if (fabsf(ry) < dz) ry = 0.0f;
            if (rx != 0.0f || ry != 0.0f) {
                float scale = Config::GetSettings().guiScrollRightStickScale;
                // Negative ry scrolls down (ImGui expects positive up), invert to match UI expectations
                float wheelX = rx * scale * dt;      // horizontal wheel
                float wheelY = -ry * scale * dt;     // vertical wheel
                // Use event API to report wheel motion (both axes)
                io.AddMouseWheelEvent(wheelX, wheelY);
            }
        }
    } else {
        // release L3 edge tracker if nothing connected
        s_lastLThumbDown = false;
    }

    // (per-second pads mask diagnostics removed)

    // Update virtual cursor position from OS mouse when enabled
    if (g_useVirtualCursor && hwnd) {
        POINT pt;
        if (GetCursorPos(&pt) && ScreenToClient(hwnd, &pt)) {
            g_virtualCursorPos.x = (float)pt.x;
            g_virtualCursorPos.y = (float)pt.y;
            
            // Clamp within client region size
            g_virtualCursorPos.x = ClampF(g_virtualCursorPos.x, 0.0f, clientW - 1.0f);
            g_virtualCursorPos.y = ClampF(g_virtualCursorPos.y, 0.0f, clientH - 1.0f);
        }
    }

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
    if (g_onlineModeActive.load(std::memory_order_relaxed)) {
        return CallWindowProc(g_originalWndProc, hWnd, msg, wParam, lParam);
    }

    // When our UI is visible, block directional keyboard events from reaching
    // ImGui's Win32 backend.  Arrow keys (and their game-bound equivalents)
    // generate WM_KEYDOWN messages that ImGui_ImplWin32_WndProcHandler converts
    // into ImGuiKey_UpArrow / DownArrow / LeftArrow / RightArrow nav triggers,
    // which fire independently of our gated gamepad dpad - causing double-navigation.
    // We handle ALL directional nav ourselves in PreNewFrameInputs, so suppress
    // these here to prevent a second input path.
    if (g_imguiVisible && (msg == WM_KEYDOWN || msg == WM_KEYUP)) {
        if (wParam == VK_UP || wParam == VK_DOWN || wParam == VK_LEFT || wParam == VK_RIGHT) {
            // Still pass to game so non-ImGui systems aren't starved
            return CallWindowProc(g_originalWndProc, hWnd, msg, wParam, lParam);
        }
    }

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

LRESULT CALLBACK FallbackWindowProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_CLOSE) {
        if (g_imguiVisible) {
            ImGuiImpl::ToggleVisibility();
        }
        return 0;
    }

    if (g_imguiInitialized && g_externalFallbackHost) {
        const LRESULT handled = ImGuiImpl::WndProc(hWnd, msg, wParam, lParam);
        if (handled != 0) {
            return handled;
        }
    }

    return DefWindowProc(hWnd, msg, wParam, lParam);
}

namespace ImGuiImpl {
    // Get DPI scale factor for the window
    static float GetDpiScale() {
        HWND hwnd = GetActiveHostWindow();
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

    // Rebuild font atlas for crisp text at a given UI scale. Throttled to avoid per-frame rebuilds.
    static void UpdateFontAtlasForScale(float uiScale)
    {
        if (!ImGui::GetCurrentContext()) return;
        ImGuiIO& io = ImGui::GetIO();

        // Throttle: only allow rebuild at most every 1000ms even if tiny scale oscillations occur.
        static auto s_lastAttempt = std::chrono::steady_clock::time_point{};
        auto now = std::chrono::steady_clock::now();
        bool timeOk = (s_lastAttempt.time_since_epoch().count() == 0) || (now - s_lastAttempt >= std::chrono::milliseconds(1000));

        const int fontMode = Config::GetSettings().uiFontMode; // 0=Default, 1=Segoe UI
        float dpiScale = GetDpiScale();
        float s = uiScale * dpiScale; // Combine UI + DPI
        if (s < 0.70f) s = 0.70f; else if (s > 2.50f) s = 2.50f;
        float sRounded = floorf(s * 100.0f + 0.5f) / 100.0f; // nearest hundredth

        const bool scaleChanged = !(fabsf(sRounded - g_lastFontScaleApplied) < 0.01f);
        const bool fontChanged  = (fontMode != g_lastFontModeApplied);
        if (!scaleChanged && !fontChanged) return; // nothing meaningful changed
        if (!timeOk && scaleChanged && !fontChanged) return; // oscillating minor scale; wait

        s_lastAttempt = now;

        const float basePx = 13.0f;
        const float targetPx = roundf(basePx * sRounded);

        ImGui_ImplDX9_InvalidateDeviceObjects();
        io.Fonts->Clear();
        io.Fonts->Flags |= ImFontAtlasFlags_NoPowerOfTwoHeight;
        io.Fonts->TexGlyphPadding = 1;

        ImFontConfig cfg;
        cfg.OversampleH = 3;
        cfg.OversampleV = 3;
        cfg.PixelSnapH = false;
        cfg.SizePixels = targetPx;

        ImFont* defaultFont = nullptr;
        if (fontMode == 1) {
            const char* segoePath = "C:\\Windows\\Fonts\\segoeui.ttf";
            DWORD fa = GetFileAttributesA(segoePath);
            if (fa != INVALID_FILE_ATTRIBUTES && !(fa & FILE_ATTRIBUTE_DIRECTORY)) {
                defaultFont = io.Fonts->AddFontFromFileTTF(segoePath, targetPx, &cfg);
            }
        }
        if (!defaultFont) {
            defaultFont = io.Fonts->AddFontDefault(&cfg);
        }
        io.FontDefault = defaultFont;

        if (!ImGui_ImplDX9_CreateDeviceObjects()) {
            LogOut("[IMGUI] Warning: Failed to recreate DX9 device objects after font rebuild.", true);
            return;
        }

        g_lastFontScaleApplied = sRounded;
        g_lastFontModeApplied  = fontMode;
        LogOut((std::string("[IMGUI] Rebuilt font atlas: scale=") + std::to_string(sRounded) +
            ", px=" + std::to_string((int)targetPx) + ", font=" + (fontMode==0?"Default":"Segoe UI")).c_str(), true);
    }
    namespace {
        bool InitializeForHost(IDirect3DDevice9* device, HWND hostWindow, bool hookWndProc, bool externalFallback) {
            if (g_imguiInitialized) {
                return g_externalFallbackHost == externalFallback;
            }

            bool expected = false;
            if (!s_imguiInitInProgress.compare_exchange_strong(expected, true)) {
                Sleep(50);
                return g_imguiInitialized && g_externalFallbackHost == externalFallback;
            }

            if (g_imguiInitialized) {
                s_imguiInitInProgress.store(false);
                return g_externalFallbackHost == externalFallback;
            }

            if (!device) {
                LogOut("[IMGUI] Error: No valid D3D device provided", true);
                s_imguiInitInProgress.store(false);
                return false;
            }

            if (!hostWindow) {
                LogOut("[IMGUI] Error: No valid host window for ImGui", true);
                s_imguiInitInProgress.store(false);
                return false;
            }

            LogOut(std::string("[IMGUI] Initializing ImGui for ") + (externalFallback ? "fallback window" : "game window"), true);

            bool createdContext = false;
            g_d3dDevice = device;
            g_imguiHostWindow = hostWindow;
            g_externalFallbackHost = externalFallback;
            g_originalWndProc = nullptr;

            ImGui_ImplWin32_EnableDpiAwareness();
            LogOut("[IMGUI] Enabled DPI awareness", true);

            IMGUI_CHECKVERSION();
            ImGui::CreateContext();
            createdContext = true;
            ImGuiIO& io = ImGui::GetIO(); (void)io;

            io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
            io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;

            {
                const auto& cfg = Config::GetSettings();
                io.KeyRepeatDelay = ClampF(cfg.guiNavRepeatDelay, 0.05f, 1.0f);
                io.KeyRepeatRate  = ClampF(cfg.guiNavRepeatRate,  0.01f, 0.50f);
            }

            io.Fonts->Flags |= ImFontAtlasFlags_NoPowerOfTwoHeight;
            io.Fonts->TexGlyphPadding = 1;

            ImGui::StyleColorsDark();

            ImGuiStyle& style = ImGui::GetStyle();
            style.Alpha = 1.0f;
            if (style.WindowMinSize.x < 1.0f) style.WindowMinSize.x = 16.0f;
            if (style.WindowMinSize.y < 1.0f) style.WindowMinSize.y = 16.0f;

            if (!ImGui_ImplWin32_Init(hostWindow)) {
                LogOut("[IMGUI] Error: ImGui_ImplWin32_Init failed", true);
                ImGui::DestroyContext();
                g_d3dDevice = nullptr;
                g_imguiHostWindow = nullptr;
                g_externalFallbackHost = false;
                s_imguiInitInProgress.store(false);
                return false;
            }

            if (!ImGui_ImplDX9_Init(device)) {
                LogOut("[IMGUI] Error: ImGui_ImplDX9_Init failed", true);
                ImGui_ImplWin32_Shutdown();
                ImGui::DestroyContext();
                g_d3dDevice = nullptr;
                g_imguiHostWindow = nullptr;
                g_externalFallbackHost = false;
                s_imguiInitInProgress.store(false);
                return false;
            }

            {
                float initScale = Config::GetSettings().uiScale;
                UpdateFontAtlasForScale(initScale);
            }

            if (hookWndProc) {
                SetLastError(0);
                g_originalWndProc = (WNDPROC)SetWindowLongPtr(hostWindow, GWLP_WNDPROC, (LONG_PTR)ImGuiWndProc);
                DWORD wndProcErr = GetLastError();
                if (!g_originalWndProc && wndProcErr != 0) {
                    LogOut("[IMGUI] Error: Failed to hook window procedure (err=" + std::to_string(wndProcErr) + ")", true);
                    ImGui_ImplDX9_Shutdown();
                    ImGui_ImplWin32_Shutdown();
                    if (createdContext) {
                        ImGui::DestroyContext();
                    }
                    g_d3dDevice = nullptr;
                    g_imguiHostWindow = nullptr;
                    g_externalFallbackHost = false;
                    s_imguiInitInProgress.store(false);
                    return false;
                }
            }

            io.BackendFlags |= ImGuiBackendFlags_HasMouseCursors;
            io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
            io.BackendFlags &= ~ImGuiBackendFlags_HasSetMousePos;

            ImGuiGui::Initialize();

            g_imguiInitialized = true;
            s_imguiInitInProgress.store(false);
            LogOut(std::string("[IMGUI] ImGui initialized successfully for ") + (externalFallback ? "fallback window" : "game window"), true);
            return true;
        }

        void SetVisibilityInternal(bool visible) {
            g_imguiVisible = visible;
            ::menuOpen.store(visible);
            CharacterSettings::g_guiVisible.store(g_imguiVisible, std::memory_order_relaxed);

            if (g_externalFallbackHost && g_externalFallbackWindow && IsWindow(g_externalFallbackWindow)) {
                if (visible) {
                    ShowWindow(g_externalFallbackWindow, SW_SHOWNORMAL);
                    SetForegroundWindow(g_externalFallbackWindow);
                    SetFocus(g_externalFallbackWindow);
                } else {
                    ShowWindow(g_externalFallbackWindow, SW_HIDE);
                }
            }

            if (g_imguiVisible) {
                LogOut("[IMGUI] ImGui interface opened - will render continuously until closed", true);
                g_resetVirtualCursorOnOpen = true;
                ImGuiGui::RequestInitialNavFocus();
                if (ImGui::GetCurrentContext()) {
                    ImGuiIO& io = ImGui::GetIO();
                    XInputShim::RefreshSnapshotOncePerFrame();
                    unsigned mask = XInputShim::GetConnectedMaskCached();
                    unsigned nativeMask = XInputShim::GetNativeConnectedMaskCached();
                    unsigned genericMask = XInputShim::GetGenericConnectedMaskCached();
                    char buf[256];
                    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
                    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
                    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "[IMGUI] Open: NavEnableGamepad=%d BackendHasGamepad=%d GamepadMask=0x%X (xinput=0x%X generic=0x%X, forced)",
                        (io.ConfigFlags & ImGuiConfigFlags_NavEnableGamepad) ? 1 : 0,
                        (io.BackendFlags & ImGuiBackendFlags_HasGamepad) ? 1 : 0,
                        mask, nativeMask, genericMask);
                    LogOut(buf, true);
                    LogOut("[IMGUI] Keyboard fallback for nav is active (Arrow/Enter/Escape)", true);
                }
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
                StartHotkeyCooldown();
            }

            PauseIntegration::OnMenuVisibilityChanged(g_imguiVisible);
            PracticeHotkeyGate::NotifyMenuVisibility(g_imguiVisible);
            PracticeOverlayGate::SetMenuVisible(g_imguiVisible);
            CharacterSettings::g_guiVisible.store(g_imguiVisible, std::memory_order_relaxed);

            static bool stateLogged = false;
            if (!stateLogged) {
                LogOut("[IMGUI] Visibility state persistence confirmed", true);
                stateLogged = true;
            }
        }

        void RunFallbackWindowThread() {
            g_externalFallbackThreadRunning.store(true, std::memory_order_release);
            g_externalFallbackReady.store(false, std::memory_order_release);
            g_externalFallbackInitFailed.store(false, std::memory_order_release);

            const HINSTANCE instance = GetModuleHandleA(nullptr);
            WNDCLASSA wc = {};
            wc.lpfnWndProc = FallbackWindowProc;
            wc.hInstance = instance;
            wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
            wc.lpszClassName = kFallbackWindowClassName;
            ATOM atom = RegisterClassA(&wc);
            if (atom == 0) {
                DWORD le = GetLastError();
                if (le != ERROR_CLASS_ALREADY_EXISTS) {
                    LogOut("[IMGUI] Failed to register fallback window class", true);
                    g_externalFallbackInitFailed.store(true, std::memory_order_release);
                    g_externalFallbackThreadRunning.store(false, std::memory_order_release);
                    return;
                }
            }

            const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
            RECT rect{0, 0, 960, 720};
            AdjustWindowRect(&rect, style, FALSE);
            HWND hwnd = CreateWindowExA(
                0,
                kFallbackWindowClassName,
                "EFZ Training Mode",
                style,
                CW_USEDEFAULT,
                CW_USEDEFAULT,
                rect.right - rect.left,
                rect.bottom - rect.top,
                nullptr,
                nullptr,
                instance,
                nullptr);
            if (!hwnd) {
                LogOut("[IMGUI] Failed to create fallback window", true);
                g_externalFallbackInitFailed.store(true, std::memory_order_release);
                g_externalFallbackThreadRunning.store(false, std::memory_order_release);
                UnregisterClassA(kFallbackWindowClassName, instance);
                return;
            }
            g_externalFallbackWindow = hwnd;

            LPDIRECT3D9 d3d9 = Direct3DCreate9(D3D_SDK_VERSION);
            if (!d3d9) {
                LogOut("[IMGUI] Failed to create D3D9 object for fallback window", true);
                DestroyWindow(hwnd);
                g_externalFallbackWindow = nullptr;
                g_externalFallbackInitFailed.store(true, std::memory_order_release);
                g_externalFallbackThreadRunning.store(false, std::memory_order_release);
                UnregisterClassA(kFallbackWindowClassName, instance);
                return;
            }

            D3DPRESENT_PARAMETERS d3dpp = {};
            d3dpp.Windowed = TRUE;
            d3dpp.SwapEffect = D3DSWAPEFFECT_DISCARD;
            d3dpp.BackBufferFormat = D3DFMT_UNKNOWN;
            d3dpp.PresentationInterval = D3DPRESENT_INTERVAL_ONE;
            d3dpp.hDeviceWindow = hwnd;

            IDirect3DDevice9* device = nullptr;
            HRESULT hr = d3d9->CreateDevice(
                D3DADAPTER_DEFAULT,
                D3DDEVTYPE_HAL,
                hwnd,
                D3DCREATE_SOFTWARE_VERTEXPROCESSING,
                &d3dpp,
                &device);
            if (FAILED(hr) || !device) {
                LogOut("[IMGUI] Failed to create D3D9 device for fallback window", true);
                d3d9->Release();
                DestroyWindow(hwnd);
                g_externalFallbackWindow = nullptr;
                g_externalFallbackInitFailed.store(true, std::memory_order_release);
                g_externalFallbackThreadRunning.store(false, std::memory_order_release);
                UnregisterClassA(kFallbackWindowClassName, instance);
                return;
            }

            if (!InitializeForHost(device, hwnd, false, true)) {
                LogOut("[IMGUI] Failed to initialize ImGui for fallback window", true);
                device->Release();
                d3d9->Release();
                DestroyWindow(hwnd);
                g_externalFallbackWindow = nullptr;
                g_externalFallbackInitFailed.store(true, std::memory_order_release);
                g_externalFallbackThreadRunning.store(false, std::memory_order_release);
                UnregisterClassA(kFallbackWindowClassName, instance);
                return;
            }

            ShowWindow(hwnd, SW_HIDE);
            UpdateWindow(hwnd);
            g_externalFallbackReady.store(true, std::memory_order_release);

            while (!g_externalFallbackExit.load(std::memory_order_acquire) && !g_isShuttingDown.load(std::memory_order_relaxed)) {
                MSG msg;
                while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
                    if (msg.message == WM_QUIT) {
                        g_externalFallbackExit.store(true, std::memory_order_release);
                        break;
                    }
                    TranslateMessage(&msg);
                    DispatchMessage(&msg);
                }

                if (g_externalFallbackExit.load(std::memory_order_acquire) || g_isShuttingDown.load(std::memory_order_relaxed)) {
                    break;
                }

                if (!g_imguiVisible) {
                    Sleep(16);
                    continue;
                }

                HRESULT coop = device->TestCooperativeLevel();
                if (coop == D3DERR_DEVICELOST) {
                    Sleep(16);
                    continue;
                }
                if (coop == D3DERR_DEVICENOTRESET) {
                    ImGui_ImplDX9_InvalidateDeviceObjects();
                    if (SUCCEEDED(device->Reset(&d3dpp))) {
                        ImGui_ImplDX9_CreateDeviceObjects();
                    } else {
                        Sleep(16);
                    }
                    continue;
                }

                device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(12, 12, 16), 1.0f, 0);
                if (SUCCEEDED(device->BeginScene())) {
                    RenderFrame();
                    device->EndScene();
                }
                device->Present(nullptr, nullptr, nullptr, nullptr);
            }

            if (g_imguiInitialized && g_externalFallbackHost) {
                Shutdown();
            }

            device->Release();
            d3d9->Release();
            DestroyWindow(hwnd);
            g_externalFallbackWindow = nullptr;
            g_externalFallbackReady.store(false, std::memory_order_release);
            g_externalFallbackInitFailed.store(false, std::memory_order_release);
            g_externalFallbackExit.store(false, std::memory_order_release);
            g_externalFallbackThreadRunning.store(false, std::memory_order_release);
            UnregisterClassA(kFallbackWindowClassName, instance);
        }
    }

    bool Initialize(IDirect3DDevice9* device) {
        if (g_externalFallbackHost) {
            return false;
        }
        return InitializeForHost(device, FindEFZWindow(), true, false);
    }
    
    void Shutdown() {
        if (!g_imguiInitialized)
            return;
        
        LogOut("[IMGUI] Shutting down ImGui", true);
        
        // Restore original window procedure
        HWND hostWindow = GetActiveHostWindow();
        if (hostWindow && g_originalWndProc) {
            WNDPROC currentWndProc = reinterpret_cast<WNDPROC>(GetWindowLongPtr(hostWindow, GWLP_WNDPROC));
            if (currentWndProc == ImGuiWndProc) {
                SetWindowLongPtr(hostWindow, GWLP_WNDPROC, (LONG_PTR)g_originalWndProc);
            } else {
                LogOut("[IMGUI] Skipping WndProc restore because another hook owns the window proc", true);
            }
        }
        
        ImGui_ImplDX9_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        
        g_imguiInitialized = false;
        g_imguiVisible = false;
        g_d3dDevice = nullptr;
        g_originalWndProc = nullptr;
        g_imguiHostWindow = nullptr;
        g_externalFallbackHost = false;
    }
    
    
    bool IsInitialized() {
        return g_imguiInitialized;
    }

    bool ShowFallbackWindow() {
        if (g_onlineModeActive.load(std::memory_order_relaxed)) {
            LogOut("[IMGUI] Ignoring fallback window request while netplay suspend is active", true);
            return false;
        }

        if (g_externalFallbackReady.load(std::memory_order_acquire) && g_externalFallbackHost) {
            if (!g_imguiVisible) {
                SetVisibilityInternal(true);
            } else if (g_externalFallbackWindow && IsWindow(g_externalFallbackWindow)) {
                ShowWindow(g_externalFallbackWindow, SW_SHOWNORMAL);
                SetForegroundWindow(g_externalFallbackWindow);
            }
            return true;
        }

        if (!g_externalFallbackThreadRunning.load(std::memory_order_acquire)) {
            g_externalFallbackExit.store(false, std::memory_order_release);
            std::thread(RunFallbackWindowThread).detach();
        }

        const DWORD start = GetTickCount();
        while (!g_externalFallbackReady.load(std::memory_order_acquire)
            && !g_externalFallbackInitFailed.load(std::memory_order_acquire)
            && (GetTickCount() - start) < 3000) {
            Sleep(10);
        }

        if (!g_externalFallbackReady.load(std::memory_order_acquire)) {
            LogOut("[IMGUI] Fallback window did not become ready", true);
            return false;
        }

        SetVisibilityInternal(true);
        return true;
    }

    bool IsExternalFallbackHost() {
        return g_externalFallbackHost;
    }
    
    void ToggleVisibility() {
        if (g_onlineModeActive.load(std::memory_order_relaxed)) {
            LogOut("[IMGUI] Ignoring visibility toggle while netplay suspend is active", true);
            return;
        }

        SetVisibilityInternal(!g_imguiVisible);
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

        // Nav repeat is fully owned by our per-direction gate (pulse emitter).
        // Set ImGui's built-in repeat to effectively-never so it doesn't fire
        // on top of our gated pulses.  Non-nav keys (e.g. text input) still
        // get a sane rate.
        io.KeyRepeatDelay = 9999.f;
        io.KeyRepeatRate  = 9999.f;

        // Always force ImGui to render against the backbuffer size (not the window size)
        io.DisplaySize = ImVec2(baseW, baseH);
        io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);

        // Remap OS mouse to backbuffer coordinates (letterbox/pillarbox aware, DPI-robust)
        HWND hwnd = GetActiveHostWindow();
        if (hwnd) {
            RECT rcClient{};
            if (GetClientRect(hwnd, &rcClient)) {
                // Convert client rect to screen space to obtain physical pixel size under DPI scaling
                POINT tl{rcClient.left, rcClient.top};
                POINT br{rcClient.right, rcClient.bottom};
                ClientToScreen(hwnd, &tl);
                ClientToScreen(hwnd, &br);
                const float cw_px = float(br.x - tl.x);
                const float ch_px = float(br.y - tl.y);
                if (cw_px > 0.0f && ch_px > 0.0f) {
                    // Compute scaling from 640x480 to current client; allow anisotropic fill if aspect deviates
                    const float sx = cw_px / baseW;
                    const float sy = ch_px / baseH;
                    const float aspectClient = cw_px / ch_px;
                    const float aspectBase = baseW / baseH; // 4:3
                    const float aspectDelta = fabsf(aspectClient - aspectBase) / aspectBase;
                    const bool useUniformLetterbox = (aspectDelta < 0.02f); // within ~2% of 4:3 -> assume letterbox/pillarbox
                    const float scale = (sx < sy) ? sx : sy;
                    const float gw = baseW * scale;
                    const float gh = baseH * scale;
                    const float ox = (cw_px - gw) * 0.5f;
                    const float oy = (ch_px - gh) * 0.5f;

                    // Current cursor in screen space (physical pixels)
                    POINT pt{};
                    if (GetCursorPos(&pt)) {
                        // Convert to client-relative physical pixels
                        const float cx = float(pt.x - tl.x);
                        const float cy = float(pt.y - tl.y);
                        float mx, my;
                        if (useUniformLetterbox) {
                            // Uniform scale with centered inner 4:3 region
                            mx = (cx - ox) / (scale > 0.0f ? scale : 1.0f);
                            my = (cy - oy) / (scale > 0.0f ? scale : 1.0f);
                        } else {
                            // Content is stretched to fill client non-uniformly (windowed mode). Map axis independently.
                            mx = cx / (sx > 0.0f ? sx : 1.0f);
                            my = cy / (sy > 0.0f ? sy : 1.0f);
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

        // Feed controller navigation and optional virtual-cursor input for the active frame.
        // The EndScene overlay path relies on this helper, so gamepad events must be queued here.
        UpdateVirtualCursor(io);
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
            const bool useCustomMenu = g_externalFallbackHost || Config::GetSettings().useCustomMenu;

            if (useCustomMenu) {
                CustomMenu::PrepareFrame();
            }

            // Prepare backend new-frame data first
            ImGui_ImplDX9_NewFrame();
            ImGui_ImplWin32_NewFrame();
            // Feed our corrected mouse/gamepad inputs after backend NewFrame, before ImGui::NewFrame.
            PreNewFrameInputs();
            ImGui::NewFrame();
            // (PostNewFrameDiagnostics removed to reduce per-frame overhead)
            // Skip rendering if minimized to avoid style asserts (DisplaySize == 0)
            ImGuiIO& io = ImGui::GetIO();
            if (io.DisplaySize.x <= 0.0f || io.DisplaySize.y <= 0.0f) {
                ImGui::EndFrame();
                return;
            }
            // Maintain pause while menu is visible (guards against stray unfreeze)
            PauseIntegration::MaintainFreezeWhileMenuVisible();

            if (useCustomMenu) {
                CustomMenu::Render();
            } else {
                // Keep font atlas in sync with current UI scale for crisp text
                UpdateFontAtlasForScale(Config::GetSettings().uiScale);
                ImGuiGui::RenderGui();
            }
            
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
