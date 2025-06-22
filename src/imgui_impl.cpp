#include "../include/utilities.h"
#include "../include/imgui_impl.h"
#include "../include/logger.h"
#include "../include/imgui_gui.h"
#include "../include/overlay.h" 
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
    
    void RenderFrame() {
        // First check shutdown flag, then the other conditions
        if (g_isShuttingDown || !g_imguiInitialized || !g_imguiVisible || !g_d3dDevice)
            return;

        // Add more null checks
        HWND gameWindow = FindEFZWindow();
        if (!gameWindow || !IsWindow(gameWindow))
            return;
        
        ImGui_ImplDX9_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        ImGuiIO& io = ImGui::GetIO();
        if (gameWindow) {
            POINT mousePos;
            GetCursorPos(&mousePos);
            ScreenToClient(gameWindow, &mousePos);

            RECT rect;
            GetClientRect(gameWindow, &rect);
            float scaleX = (float)(rect.right - rect.left) / 640.0f;
            float scaleY = (float)(rect.bottom - rect.top) / 480.0f;

            io.MousePos.x = mousePos.x / scaleX;
            io.MousePos.y = mousePos.y / scaleY;
            
            static int frameCounter = 0;
            if (frameCounter++ % 300 == 0) { 
                LogOut("[IMGUI] Mouse position: " + std::to_string(io.MousePos.x) + 
                       ", " + std::to_string(io.MousePos.y) + 
                       " (scale: " + std::to_string(scaleX) + ", " + std::to_string(scaleY) + ")", true);
            }
        }

        ImGuiGui::RenderGui();
        ImGui::EndFrame();

        // 4. Render the ImGui draw data
        ImGui::Render();
        ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
    }
    
    bool IsInitialized() {
        return g_imguiInitialized;
    }
    
    void ToggleVisibility() {
        g_imguiVisible = !g_imguiVisible;
        
        if (g_imguiVisible) {
            LogOut("[IMGUI] ImGui interface opened - will render continuously until closed", true);
            
            // Update our local copy of the display data
            ImGuiGui::guiState.localData = displayData;
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
}