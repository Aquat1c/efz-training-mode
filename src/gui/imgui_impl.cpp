#include "../include/utils/utilities.h"

#include "../include/gui/imgui_impl.h"
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

#pragma comment(lib, "xinput9_1_0.lib")

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
    if (!cfg.enableVirtualCursor) {
        g_useVirtualCursor = false;
        io.MouseDrawCursor = false;
        return;
    }
    HWND hwnd = FindEFZWindow();
    bool fullscreen = hwnd && IsFullscreen(hwnd);
    const bool want = g_imguiVisible && hwnd && (fullscreen || cfg.virtualCursorAllowWindowed);
    bool wasActive = g_useVirtualCursor;
    g_useVirtualCursor = want;

    // Track focus transitions to allow recenter on refocus
    static bool s_lastWindowFocused = false;
    bool focusedNow = (hwnd && GetForegroundWindow() == hwnd);
    bool regainedFocus = (focusedNow && !s_lastWindowFocused);
    s_lastWindowFocused = focusedNow;
    // Track edge states for additional user-triggered recenters
    static bool s_lastMiddleDown = false;       // mouse middle button
    static bool s_lastLThumbDown = false;       // gamepad left stick click

    if (!g_useVirtualCursor) {
        io.MouseDrawCursor = false;
        return;
    }

    // Draw ImGui software cursor
    io.MouseDrawCursor = true;

    // Determine current client rect for clamping and centering
    float clientW = io.DisplaySize.x;
    float clientH = io.DisplaySize.y;
    if (hwnd) {
        RECT rc{}; if (GetClientRect(hwnd, &rc)) { clientW = (float)(rc.right - rc.left); clientH = (float)(rc.bottom - rc.top); }
    }

    if (!wasActive || regainedFocus) {
        // Center cursor on first activation OR when window regains focus
        g_virtualCursorPos = ImVec2(clientW * 0.5f, clientH * 0.5f);
    }

    // Middle mouse (hardware) recenter when ImGui visible & window focused
    if (focusedNow && g_imguiVisible) {
        SHORT mm = GetAsyncKeyState(VK_MBUTTON);
        bool middleDown = (mm & 0x8000) != 0;
        bool middlePressed = middleDown && !s_lastMiddleDown;
        if (middlePressed) {
            g_virtualCursorPos = ImVec2(clientW * 0.5f, clientH * 0.5f);
            // If we are NOT drawing software cursor (possible future toggle), also move OS cursor
            if (!g_useVirtualCursor) {
                POINT pt{ (LONG)(clientW * 0.5f), (LONG)(clientH * 0.5f) };
                ClientToScreen(hwnd, &pt);
                SetCursorPos(pt.x, pt.y);
            }
        }
        s_lastMiddleDown = middleDown;
    }

    XINPUT_STATE state{};
    DWORD xr = XInputGetState(0, &state);
    if (xr == ERROR_SUCCESS) {
        // Edge detection for left stick click (L3) to recenter
        bool lThumbDown = (state.Gamepad.wButtons & XINPUT_GAMEPAD_LEFT_THUMB) != 0;
        bool lThumbPressed = lThumbDown && !s_lastLThumbDown && focusedNow && g_imguiVisible;
        if (lThumbPressed) {
            g_virtualCursorPos = ImVec2(clientW * 0.5f, clientH * 0.5f);
        }
        s_lastLThumbDown = lThumbDown;
        // ...continue with normal analog handling
        // Analog stick movement
        const SHORT lx = state.Gamepad.sThumbLX;
        const SHORT ly = state.Gamepad.sThumbLY;
        auto applyDeadzone = [](SHORT v, SHORT dz) -> float {
            int iv = (int)v;
            if (iv > dz) iv -= dz; else if (iv < -dz) iv += dz; else iv = 0;
            float n = (float)iv / (32767.0f - dz);
            if (n > 1.f) n = 1.f; if (n < -1.f) n = -1.f;
            return n;
        };

        float nx = applyDeadzone(lx, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE);
        float ny = applyDeadzone(ly, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE);

        // Apply acceleration/response curve (exponent >1 softens near center for precision)
        if (cfg.virtualCursorAccelPower != 1.0f) {
            auto curve = [&](float v) {
                float sign = (v >= 0.f) ? 1.f : -1.f;
                float mag = fabsf(v);
                mag = powf(mag, cfg.virtualCursorAccelPower); // stable for mag in [0,1]
                return sign * mag;
            };
            nx = curve(nx);
            ny = curve(ny);
        }

        // DPAD provides discrete nudge
        float dpadX = 0.f, dpadY = 0.f;
        if (state.Gamepad.wButtons & XINPUT_GAMEPAD_DPAD_RIGHT) dpadX += 1.f;
        if (state.Gamepad.wButtons & XINPUT_GAMEPAD_DPAD_LEFT)  dpadX -= 1.f;
        if (state.Gamepad.wButtons & XINPUT_GAMEPAD_DPAD_UP)    dpadY -= 1.f;
        if (state.Gamepad.wButtons & XINPUT_GAMEPAD_DPAD_DOWN)  dpadY += 1.f;

        float dt = io.DeltaTime > 0.f ? io.DeltaTime : (1.f/60.f);
        // Speed scaling
    const bool fast = (state.Gamepad.wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER) != 0;
    float baseSpeed = fast ? cfg.virtualCursorFastSpeed : cfg.virtualCursorBaseSpeed;  // pixels per second
    float dpadSpeed = cfg.virtualCursorDpadSpeed;
    if (baseSpeed < 50.f) baseSpeed = 50.f; if (baseSpeed > 8000.f) baseSpeed = 8000.f;
    if (dpadSpeed < 10.f) dpadSpeed = 10.f; if (dpadSpeed > 4000.f) dpadSpeed = 4000.f;

        g_virtualCursorPos.x += (nx * baseSpeed + dpadX * dpadSpeed) * dt;
        g_virtualCursorPos.y += (-ny * baseSpeed + dpadY * dpadSpeed) * dt; // note: Y is inverted

        // Clamp within client region size
        g_virtualCursorPos.x = ClampF(g_virtualCursorPos.x, 0.0f, clientW - 1.0f);
        g_virtualCursorPos.y = ClampF(g_virtualCursorPos.y, 0.0f, clientH - 1.0f);

        // Buttons -> mouse clicks
        bool leftDown = (state.Gamepad.wButtons & XINPUT_GAMEPAD_A) != 0;
        bool rightDown = (state.Gamepad.wButtons & XINPUT_GAMEPAD_B) != 0;

        io.MouseDown[0] = leftDown;
        io.MouseDown[1] = rightDown;
        g_lastLeftDown = leftDown;
        g_lastRightDown = rightDown;
    }

    // Feed position
    io.MousePos = g_virtualCursorPos;
}

// Custom WndProc to handle ImGui input
LRESULT CALLBACK ImGuiWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    // Only process input for ImGui when it's visible
    if (g_imguiVisible && ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;
    
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
            // Refresh local data now so UI reflects current state on first frame
            ImGuiGui::RefreshLocalData();
        } else {
            LogOut("[IMGUI] ImGui interface closed", true);
            
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
        if (g_imguiVisible && ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
            return true;
        
        return 0;
    }
    
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
            // Start new ImGui frame
            ImGui_ImplDX9_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();
            // Skip rendering if minimized to avoid style asserts (DisplaySize == 0)
            ImGuiIO& io = ImGui::GetIO();
            if (io.DisplaySize.x <= 0.0f || io.DisplaySize.y <= 0.0f) {
                ImGui::EndFrame();
                return;
            }
            // Maintain pause while menu is visible (guards against stray unfreeze)
            PauseIntegration::MaintainFreezeWhileMenuVisible();
            
            // Update virtual cursor from XInput when in fullscreen
            UpdateVirtualCursor(io);

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
}