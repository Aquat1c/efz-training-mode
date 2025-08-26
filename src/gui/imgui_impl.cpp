#include "../include/utils/utilities.h"

#include "../include/gui/imgui_impl.h"
#include "../include/core/logger.h"
#include "../include/gui/imgui_gui.h"
#include "../include/gui/overlay.h" 
#include <stdexcept>
#include <Xinput.h>
#include <algorithm>

#pragma comment(lib, "xinput9_1_0.lib")

// Global reference to shutdown flag - MOVED OUTSIDE namespace
extern std::atomic<bool> g_isShuttingDown;

// Global state
static bool g_imguiInitialized = false;
static bool g_imguiVisible = false;
static IDirect3DDevice9* g_d3dDevice = nullptr;
static WNDPROC g_originalWndProc = nullptr;

// Virtual cursor/gamepad state
static bool g_useVirtualCursor = false;
static ImVec2 g_virtualCursorPos = ImVec2(200.0f, 200.0f);
static bool g_lastLeftDown = false;
static bool g_lastRightDown = false;

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
    HWND hwnd = FindEFZWindow();
    const bool want = g_imguiVisible && hwnd && IsFullscreen(hwnd);
    bool wasActive = g_useVirtualCursor;
    g_useVirtualCursor = want;

    if (!g_useVirtualCursor) {
        io.MouseDrawCursor = false;
        return;
    }

    // Draw ImGui software cursor
    io.MouseDrawCursor = true;

    if (!wasActive) {
        // Center cursor on first activation
        g_virtualCursorPos = ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f);
    }

    XINPUT_STATE state{};
    DWORD xr = XInputGetState(0, &state);
    if (xr == ERROR_SUCCESS) {
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

        const float nx = applyDeadzone(lx, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE);
        const float ny = applyDeadzone(ly, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE);

        // DPAD provides discrete nudge
        float dpadX = 0.f, dpadY = 0.f;
        if (state.Gamepad.wButtons & XINPUT_GAMEPAD_DPAD_RIGHT) dpadX += 1.f;
        if (state.Gamepad.wButtons & XINPUT_GAMEPAD_DPAD_LEFT)  dpadX -= 1.f;
        if (state.Gamepad.wButtons & XINPUT_GAMEPAD_DPAD_UP)    dpadY -= 1.f;
        if (state.Gamepad.wButtons & XINPUT_GAMEPAD_DPAD_DOWN)  dpadY += 1.f;

        float dt = io.DeltaTime > 0.f ? io.DeltaTime : (1.f/60.f);
        // Speed scaling
        const bool fast = (state.Gamepad.wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER) != 0;
        const float baseSpeed = fast ? 1800.f : 900.f;  // pixels per second
        const float dpadSpeed = 700.f;

        g_virtualCursorPos.x += (nx * baseSpeed + dpadX * dpadSpeed) * dt;
        g_virtualCursorPos.y += (-ny * baseSpeed + dpadY * dpadSpeed) * dt; // note: Y is inverted

        // Clamp within display
    g_virtualCursorPos.x = ClampF(g_virtualCursorPos.x, 0.0f, io.DisplaySize.x - 1.0f);
    g_virtualCursorPos.y = ClampF(g_virtualCursorPos.y, 0.0f, io.DisplaySize.y - 1.0f);

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
    bool Initialize(IDirect3DDevice9* device) {
        if (g_imguiInitialized)
            return true;

        LogOut("[IMGUI] Initializing ImGui", true);
        
        if (!device) {
            LogOut("[IMGUI] Error: No valid D3D device provided", true);
            return false;
        }
        
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
    style.ScaleAllSizes(1.2f);
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
            
            // Update our local copy of the display data by reading from memory
            ImGuiGui::RefreshLocalData();
        } else {
            LogOut("[IMGUI] ImGui interface closed", true);
            
            // BUGFIX: Reset the global menuOpen flag when ImGui is closed
            // Use global namespace resolution operator (::) to access the global variable
            ::menuOpen.store(false);
        }
        
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
            
            // Update virtual cursor from XInput when in fullscreen
            UpdateVirtualCursor(io);

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