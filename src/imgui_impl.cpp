#include "../include/imgui_impl.h"
#include "../include/utilities.h"
#include "../include/logger.h"
#include "../include/imgui_gui.h"
#include "../include/overlay.h"  
#include <stdexcept>

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

        // Create ImGui context
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO(); (void)io;
        
        // Enable keyboard controls, gamepad controls
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
        
        // Setup Dear ImGui style
        ImGui::StyleColorsDark();
        
        // Scale up the style for better readability
        ImGuiStyle& style = ImGui::GetStyle();
        style.ScaleAllSizes(1.2f);
        
        // Find game window
        HWND gameWindow = FindEFZWindow();
        if (!gameWindow) {
            LogOut("[IMGUI] Error: Couldn't find EFZ window", true);
            return false;
        }
        
        // Setup Platform/Renderer backends
        if (!ImGui_ImplWin32_Init(gameWindow)) {
            LogOut("[IMGUI] Error: ImGui_ImplWin32_Init failed", true);
            return false;
        }
        
        if (!ImGui_ImplDX9_Init(device)) {
            LogOut("[IMGUI] Error: ImGui_ImplDX9_Init failed", true);
            ImGui_ImplWin32_Shutdown();
            return false;
        }
        
        // Hook window procedure to receive input events
        g_originalWndProc = (WNDPROC)SetWindowLongPtr(gameWindow, GWLP_WNDPROC, (LONG_PTR)ImGuiWndProc);
        if (!g_originalWndProc) {
            LogOut("[IMGUI] Error: Failed to hook window procedure", true);
            ImGui_ImplDX9_Shutdown();
            ImGui_ImplWin32_Shutdown();
            return false;
        }
        
        // Initialize ImGui GUI
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
        
        // Shutdown ImGui backends
        ImGui_ImplDX9_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        
        g_imguiInitialized = false;
        g_imguiVisible = false;
        g_d3dDevice = nullptr;
    }
    
    void RenderFrame() {
        if (!g_imguiInitialized || !g_imguiVisible || !g_d3dDevice)
            return;

        // 1. Start a new ImGui frame
        ImGui_ImplDX9_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        // 2. Build the GUI
        ImGuiGui::RenderGui();

        // 3. End the frame. This is a necessary step before rendering.
        ImGui::EndFrame();

        // 4. Render the ImGui draw data
        ImGui::Render();
        ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
    }

    // The functions below are now obsolete and can be removed.
    /*
    void NewFrame() {
        if (!g_imguiInitialized || !g_imguiVisible)
            return;
        
        ImGui_ImplDX9_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
    }
    
    void EndFrame() {
        if (!g_imguiInitialized || !g_imguiVisible)
            return;
        
        ImGui::EndFrame();
    }
    
    void Render() {
        if (!g_imguiInitialized || !g_imguiVisible || !g_d3dDevice)
            return;
        
        // Render the ImGui interface
        ImGuiGui::RenderGui();
        
        // Render ImGui
        ImGui::Render();
        ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
    }
    */
    
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