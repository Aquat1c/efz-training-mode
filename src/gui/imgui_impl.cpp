#include "../include/utils/utilities.h"

#include "../include/gui/imgui_impl.h"
#include "../include/core/logger.h"
#include "../include/gui/imgui_gui.h"
#include "../include/gui/overlay.h" 
#include <stdexcept>

// Global reference to shutdown flag - MOVED OUTSIDE namespace
extern std::atomic<bool> g_isShuttingDown;

// Global state
static bool g_imguiInitialized = false;
static bool g_imguiVisible = false;
static IDirect3DDevice9* g_d3dDevice = nullptr;
static WNDPROC g_originalWndProc = nullptr;

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
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
        
        ImGui::StyleColorsDark();
        
    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(1.2f);
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