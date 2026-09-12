#pragma once
#include <windows.h>
#include <d3d9.h>
#include "../3rdparty/imgui/imgui.h"
#include "../3rdparty/imgui/backends/imgui_impl_dx9.h"
#include "../3rdparty/imgui/backends/imgui_impl_win32.h"

// Forward declare message handler from imgui_impl_win32.cpp
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// ImGui implementation for EFZ
namespace ImGuiImpl {
    // Initialize ImGui with DX9
    bool Initialize(IDirect3DDevice9* device);
    
    // Shutdown ImGui
    void Shutdown();
    
    // NEW: Consolidated render function
    void RenderFrame();
    
    // Feed pre-NewFrame inputs (XInput aggregation + virtual cursor) and emit debug logs.
    // Call this after backend NewFrame calls and before ImGui::NewFrame() in the active render loop.
    void PreNewFrameInputs();
    
    // (Diagnostics removed) Placeholder to avoid dangling references if any.
    inline void PostNewFrameDiagnostics() {}
    
    // Check if ImGui is initialized
    bool IsInitialized();
    
    // Toggle ImGui visibility
    void ToggleVisibility();
    // Unconditional close. Unlike ToggleVisibility() this is not netplay-gated,
    // so teardown paths cannot leave menuOpen/g_imguiVisible desynced. Use this
    // wherever the menu must go away rather than be toggled.
    void ForceHide();

    // Open the standalone fallback ImGui window when in-game rendering is unavailable.
    bool ShowFallbackWindow();

    // True while the separate fallback ImGui host window owns the current menu.
    bool IsExternalFallbackHost();
    
    // Check if ImGui is visible
    bool IsVisible();

    // Handle WndProc messages
    LRESULT WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

    // Provide the current overlay window center (in ImGui screen coordinates)
    // so recenter actions (middle-click/L3) can snap to the UI instead of the raw client center.
    void SetOverlayCenter(const ImVec2& center);

    // Request focusing the main overlay window next frame (consumed in RenderGui).
    bool ConsumeOverlayFocusRequest();
}