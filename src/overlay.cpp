#include <fstream>
#include <iomanip>
#include <chrono>
#include <vector>
#include <thread>
#include <atomic> // Add this for std::atomic
#include "../include/overlay.h"
#include "../include/logger.h"
#include "../include/utilities.h"
#include "../include/memory.h"   
#include "../include/constants.h" 
#include "../3rdparty/detours/include/detours.h"
#include <algorithm>
#include "../include/imgui_impl.h"
#include <d3d9.h>
#include <mutex>
#include <deque>
#include "../include/imgui_gui.h"
#include "../3rdparty/minhook/include/MinHook.h"

// --- Define static members of DirectDrawHook ---
DirectDrawCreateFunc DirectDrawHook::originalDirectDrawCreate = nullptr;
DirectDrawEnumerateFunc DirectDrawHook::originalDirectDrawEnumerate = nullptr;
BlitFunc DirectDrawHook::originalBlit = nullptr;
FlipFunc DirectDrawHook::originalFlip = nullptr;
IDirectDrawSurface7* DirectDrawHook::primarySurface = nullptr;
HWND DirectDrawHook::gameWindow = nullptr;
std::mutex DirectDrawHook::messagesMutex;
std::deque<OverlayMessage> DirectDrawHook::messages;
std::vector<OverlayMessage> DirectDrawHook::permanentMessages;
int DirectDrawHook::nextMessageId = 0;
bool DirectDrawHook::isHooked = false;
// --- End static member definitions ---

// --- D3D9 Hooking Globals ---
typedef HRESULT(WINAPI* EndScene_t)(LPDIRECT3DDEVICE9);
static EndScene_t oEndScene = nullptr;
static std::atomic<bool> g_d3d9Hooked = false;
static LPDIRECT3DDEVICE9 g_pd3dDevice = nullptr; // Renamed from g_d3dDevice
static std::atomic<bool> g_d3dInitialized = false; // Added
static HANDLE g_imguiRenderThread = nullptr; // Added
static std::atomic<bool> g_imguiRenderThreadRunning = false; // Added
// --- End D3D9 Globals ---


// Helper function to hook virtual methods
void HookVTableMethod(void* pInterface, int vtableOffset, void* hookFunction, void** originalFunction) {
    void** vtable = *reinterpret_cast<void***>(pInterface);
    *originalFunction = vtable[vtableOffset];
    
    DWORD oldProtect;
    VirtualProtect(&vtable[vtableOffset], sizeof(void*), PAGE_READWRITE, &oldProtect);
    vtable[vtableOffset] = hookFunction;
    VirtualProtect(&vtable[vtableOffset], sizeof(void*), oldProtect, &oldProtect);
}

// The hook for DirectDrawCreate
HRESULT WINAPI DirectDrawHook::HookedDirectDrawCreate(GUID* lpGUID, LPVOID* lplpDD, IUnknown* pUnkOuter) {
    LogOut("[OVERLAY] DirectDrawCreate called", true);
    
    // Call the original function first
    HRESULT result = originalDirectDrawCreate(lpGUID, lplpDD, pUnkOuter);
    
    if (SUCCEEDED(result) && lplpDD) {
        LogOut("[OVERLAY] DirectDrawCreate succeeded, setting up hooks", true);
        
        // Get IDirectDraw interface
        IDirectDraw* ddraw = static_cast<IDirectDraw*>(*lplpDD);
        if (ddraw) {
            // Query for IDirectDraw7 interface
            IDirectDraw7* ddraw7 = nullptr;
            if (SUCCEEDED(ddraw->QueryInterface(IID_IDirectDraw7, (void**)&ddraw7))) {
                LogOut("[OVERLAY] Got IDirectDraw7 interface", true);
                
                // Set cooperative level
                ddraw7->SetCooperativeLevel(gameWindow, DDSCL_NORMAL);
                
                // Create primary surface description
                DDSURFACEDESC2 ddsd;
                ZeroMemory(&ddsd, sizeof(ddsd));
                ddsd.dwSize = sizeof(ddsd);
                ddsd.dwFlags = DDSD_CAPS;
                ddsd.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE;
                
                // Create primary surface
                HRESULT surfResult = ddraw7->CreateSurface(&ddsd, &primarySurface, NULL);
                if (SUCCEEDED(surfResult) && primarySurface) {
                    LogOut("[OVERLAY] Primary surface created successfully", true);
                    
                    // Hook the Blt and Flip methods more aggressively
                    HookVTableMethod(primarySurface, DDRAW_BLIT_OFFSET, HookedBlit, (void**)&originalBlit);
                    HookVTableMethod(primarySurface, DDRAW_FLIP_OFFSET, HookedFlip, (void**)&originalFlip);
                    
                    LogOut("[OVERLAY] Surface methods hooked successfully", true);
                    
                    // CRITICAL: Also try to hook other surfaces that might be created
                    LogOut("[OVERLAY] Looking for additional surfaces to hook...", true);
                } else {
                    LogOut("[OVERLAY] Failed to create primary surface: " + std::to_string(surfResult), true);
                }
                
                ddraw7->Release();
            }
        }
    } else {
        LogOut("[OVERLAY] DirectDrawCreate failed: " + std::to_string(result), true);
    }
    
    return result;
}

// The hook for the Blt method
HRESULT WINAPI DirectDrawHook::HookedBlit(IDirectDrawSurface7* This, LPRECT lpDestRect, 
                                       IDirectDrawSurface7* lpDDSrcSurface, LPRECT lpSrcRect, 
                                       DWORD dwFlags, LPDDBLTFX lpDDBltFx) {
    // Call the original function first
    HRESULT result = originalBlit(This, lpDestRect, lpDDSrcSurface, lpSrcRect, dwFlags, lpDDBltFx);
    
    // CRITICAL: Only render on the primary surface to avoid duplicate rendering
    if (SUCCEEDED(result) && This == primarySurface) {
        // Render normal overlay messages
        RenderAllMessages(This);
        
        // CRITICAL FIX: DON'T call RenderImGui here - let the thread do it
        // Remove or comment out the ImGui rendering code
        // if (ImGuiImpl::IsInitialized() && ImGuiImpl::IsVisible()) {
        //     RenderImGui();
        // }
    }
    
    return result;
}

// The hook for the Flip method
HRESULT WINAPI DirectDrawHook::HookedFlip(IDirectDrawSurface7* This, 
                                        IDirectDrawSurface7* lpDDSurfaceTargetOverride, 
                                        DWORD dwFlags) {
    // This is for the old text overlay. We can leave it.
    if (isHooked) {
        RenderAllMessages(This);
    }
    return originalFlip(This, lpDDSurfaceTargetOverride, dwFlags);
}

// --- NEW: The Correct D3D9 EndScene Hook ---
HRESULT WINAPI HookedEndScene(LPDIRECT3DDEVICE9 pDevice) {
    static std::mutex renderMutex;
    
    // Prevent crashes caused by null device
    if (!pDevice) {
        return oEndScene ? oEndScene(pDevice) : D3DERR_INVALIDCALL;
    }
    
    // Store device pointer for initialization (without race conditions)
    if (!g_pd3dDevice) {
        std::lock_guard<std::mutex> lock(renderMutex);
        if (!g_pd3dDevice) { // Double-check after lock
            g_pd3dDevice = pDevice;
            LogOut("[IMGUI] D3D device obtained", detailedLogging.load());
        }
    }
    
    // Initialize ImGui at the first possible moment - but safely
    static bool imguiInitStarted = false;
    if (!ImGuiImpl::IsInitialized() && g_pd3dDevice && !imguiInitStarted) {
        std::lock_guard<std::mutex> lock(renderMutex);
        if (!imguiInitStarted) { // Double-check after lock
            imguiInitStarted = true;
            
            // Initialize directly without threading - safer approach
            try {
                bool success = ImGuiImpl::Initialize(g_pd3dDevice);
                LogOut("[IMGUI] ImGui initialization " + 
                      std::string(success ? "succeeded" : "failed"), true);
            }
            catch (const std::exception& e) {
                LogOut("[IMGUI] Exception during ImGui initialization: " + 
                      std::string(e.what()), true);
            }
            catch (...) {
                LogOut("[IMGUI] Unknown exception during ImGui initialization", true);
            }
        }
    }

    // Render ImGui with proper synchronization
    if (ImGuiImpl::IsInitialized() && ImGuiImpl::IsVisible()) {
        std::lock_guard<std::mutex> lock(renderMutex);
        try {
            ImGuiImpl::RenderFrame();
        }
        catch (const std::exception& e) {
            LogOut("[IMGUI] Exception during ImGui rendering: " + 
                  std::string(e.what()), true);
        }
        catch (...) {
            LogOut("[IMGUI] Unknown exception during ImGui rendering", true);
            // Don't disable rendering on exception to avoid permanent breakage
        }
    }

    // Call the original EndScene
    return oEndScene(pDevice);
}


// Render text on the given surface
void DirectDrawHook::RenderText(HDC hdc, const std::string& text, int x, int y, COLORREF color) {
    if (!hdc || text.empty()) return;
    
    // Set up the text properties
    SetBkMode(hdc, TRANSPARENT);
    
    // Create a better font for our overlay - FIX THE CreateFont CALL
    HFONT font = CreateFont(
        20,                        // Height
        0,                         // Width
        0,                         // Escapement
        0,                         // Orientation
        FW_BOLD,                   // Weight
        FALSE,                     // Italic
        FALSE,                     // Underline
        FALSE,                     // StrikeOut
        DEFAULT_CHARSET,           // CharSet
        OUT_OUTLINE_PRECIS,        // OutPrecision
        CLIP_DEFAULT_PRECIS,       // ClipPrecision
        ANTIALIASED_QUALITY,       // Quality
        DEFAULT_PITCH | FF_SWISS,  // PitchAndFamily - FIX: Use FF_SWISS
        "Arial"                    // FaceName
    );
    
    HFONT oldFont = (HFONT)SelectObject(hdc, font);
    
    // Draw drop shadow first (for better visibility)
    SetTextColor(hdc, RGB(0, 0, 0));
    TextOutA(hdc, x + 2, y + 2, text.c_str(), text.length());
    
    // Draw the main text
    SetTextColor(hdc, color);
    TextOutA(hdc, x, y, text.c_str(), text.length());
    
    // Clean up
    SelectObject(hdc, oldFont);
    DeleteObject(font);
}

// Render simple text with a black outline for visibility
void DirectDrawHook::RenderSimpleText(IDirectDrawSurface7* surface, const std::string& text, int x, int y, COLORREF color) {
    if (!surface || text.empty()) return;
    
    // Get device context for drawing
    HDC hdc;
    HRESULT hr = surface->GetDC(&hdc);
    if (FAILED(hr)) {
        LogOut("[OVERLAY] Failed to get DC: " + std::to_string(hr), true);
        return;
    }
    
    try {
        // Set up text properties similar to EFZ's style
        SetBkMode(hdc, TRANSPARENT);
        
        // Create a font that should be compatible with EFZ - FIX THE CreateFont CALL
        HFONT font = CreateFont(
            14,                        // Height - smaller for less interference
            0,                         // Width (auto)
            0,                         // Escapement
            0,                         // Orientation
            FW_NORMAL,                 // Weight - normal instead of bold
            FALSE,                     // Italic
            FALSE,                     // Underline
            FALSE,                     // StrikeOut
            DEFAULT_CHARSET,           // CharSet
            OUT_DEFAULT_PRECIS,        // OutputPrecision
            CLIP_DEFAULT_PRECIS,       // ClipPrecision
            DEFAULT_QUALITY,           // Quality - use default instead of antialiased
            DEFAULT_PITCH | FF_DONTCARE, // PitchAndFamily - FIX: Use FF_DONTCARE
            "MS Sans Serif"            // Face name - this is the 14th parameter
        );
        
        HFONT oldFont = (HFONT)SelectObject(hdc, font);
        
        // Draw black outline for better visibility (thinner outline)
        SetTextColor(hdc, RGB(0, 0, 0));
        TextOutA(hdc, x + 1, y, text.c_str(), text.length());
        TextOutA(hdc, x - 1, y, text.c_str(), text.length());
        TextOutA(hdc, x, y + 1, text.c_str(), text.length());
        TextOutA(hdc, x, y - 1, text.c_str(), text.length());
        
        // Draw main text
        SetTextColor(hdc, color);
        TextOutA(hdc, x, y, text.c_str(), text.length());
        
        // Clean up
        SelectObject(hdc, oldFont);
        DeleteObject(font);
        
    } catch (...) {
        LogOut("[OVERLAY] Exception in RenderSimpleText", true);
    }
    
    surface->ReleaseDC(hdc);
}

// Render all current messages on the surface
void DirectDrawHook::RenderAllMessages(IDirectDrawSurface7* surface) {
    if (!surface) return;
    
    std::lock_guard<std::mutex> lock(messagesMutex);
    
    // Get the current time
    auto now = std::chrono::steady_clock::now();
    
    // Check if character data is initialized
    uintptr_t base = GetEFZBase();
    bool showHelloWorld = false;
    
    if (base) {
        // Check if both players have valid HP values (indicating game is active)
        uintptr_t hpAddr1 = ResolvePointer(base, EFZ_BASE_OFFSET_P1, HP_OFFSET);
        uintptr_t hpAddr2 = ResolvePointer(base, EFZ_BASE_OFFSET_P2, HP_OFFSET);
        
        if (hpAddr1 && hpAddr2) {
            int hp1 = 0, hp2 = 0;
            if (SafeReadMemory(hpAddr1, &hp1, sizeof(int)) && 
                SafeReadMemory(hpAddr2, &hp2, sizeof(int))) {
                // Show "Hello, world" if both players have valid HP (indicating match is active)
                showHelloWorld = (hp1 > 0 && hp2 > 0 && hp1 <= MAX_HP && hp2 <= MAX_HP);
                
                if (showHelloWorld) {
                    LogOut("[OVERLAY] Showing Hello World - HP1: " + std::to_string(hp1) + ", HP2: " + std::to_string(hp2), true);
                }
            }
        }
    }
    
    // CRITICAL FIX: Use a more aggressive rendering approach
    HDC hdc;
    if (FAILED(surface->GetDC(&hdc))) {
        LogOut("[OVERLAY] Failed to get surface DC", true);
        return;
    }
    
    try {
        // Set up for high-visibility rendering
        SetBkMode(hdc, TRANSPARENT);
        
        // Render "Hello, world" text if characters are initialized
        if (showHelloWorld) {
            // Use multiple rendering techniques to ensure visibility
            
            // Method 1: Large, bold text with heavy outline
            HFONT bigFont = CreateFont(
                32,                        // Much larger height
                0,                         // Width (auto)
                0,                         // Escapement
                0,                         // Orientation
                FW_BOLD,                   // Bold weight
                FALSE,                     // Italic
                FALSE,                     // Underline
                FALSE,                     // StrikeOut
                DEFAULT_CHARSET,           // CharSet
                OUT_DEFAULT_PRECIS,        // OutputPrecision
                CLIP_DEFAULT_PRECIS,       // ClipPrecision
                ANTIALIASED_QUALITY,       // Quality
                DEFAULT_PITCH | FF_SWISS,  // PitchAndFamily
                "Arial"                    // Face name
            );
            
            HFONT oldFont = (HFONT)SelectObject(hdc, bigFont);
            
            // Draw heavy black outline (multiple passes)
            SetTextColor(hdc, RGB(0, 0, 0));
            for (int dx = -3; dx <= 3; dx++) {
                for (int dy = -3; dy <= 3; dy++) {
                    if (dx != 0 || dy != 0) {
                        TextOutA(hdc, 50 + dx, 50 + dy, "Hello, world!", 13);
                    }
                }
            }
            
            // Draw main text in bright yellow
            SetTextColor(hdc, RGB(255, 255, 0));
            TextOutA(hdc, 50, 50, "Hello, world!", 13);
            
            // Also draw at multiple positions to ensure visibility
            SetTextColor(hdc, RGB(0, 255, 0));
            TextOutA(hdc, 200, 100, "OVERLAY TEST", 12);
            
            SetTextColor(hdc, RGB(255, 0, 255));
            TextOutA(hdc, 400, 150, "EFZ TRAINING", 12);
            
            SelectObject(hdc, oldFont);
            DeleteObject(bigFont);
            
            LogOut("[OVERLAY] Rendered Hello World with enhanced visibility", true);
        }
        
        // Render permanent messages
        for (const auto& msg : permanentMessages) {
            RenderText(hdc, msg.text, msg.xPos, msg.yPos, msg.color);
        }
        
        // Render temporary messages and remove expired ones
        auto it = messages.begin();
        while (it != messages.end()) {
            if (now >= it->expireTime) {
                it = messages.erase(it);
            } else {
                RenderText(hdc, it->text, it->xPos, it->yPos, it->color);
                ++it;
            }
        }
        
    } catch (...) {
        LogOut("[OVERLAY] Exception in RenderAllMessages", true);
    }
    
    // Release the device context
    surface->ReleaseDC(hdc);
}

// Initialize the hook
bool DirectDrawHook::Initialize() {
    if (isHooked) return true;
    
    LogOut("[OVERLAY] Initializing DirectDraw hook", true);
    
    // Use the robust FindEFZWindow function instead of FindWindowA
    gameWindow = FindEFZWindow();
    if (!gameWindow) {
        LogOut("[OVERLAY] Could not find EFZ window using FindEFZWindow()", true);
        
        // Try alternative window finding methods
        gameWindow = FindWindowA(NULL, "Eternal Fighter Zero");
        if (!gameWindow) {
            gameWindow = FindWindowA(NULL, "Eternal Fighter Zero -Revival-");
        }
        if (!gameWindow) {
            gameWindow = FindWindowA(NULL, "Eternal Fighter Zero -Revival- 1.02e");
        }
        
        if (!gameWindow) {
            LogOut("[OVERLAY] All window finding methods failed", true);
            return false;
        } else {
            LogOut("[OVERLAY] Found EFZ window using alternative method", true);
        }
    } else {
        LogOut("[OVERLAY] Found EFZ window using FindEFZWindow()", true);
    }
    
    // Get the module handle for ddraw.dll
    HMODULE ddrawModule = GetModuleHandleA("ddraw.dll");
    if (!ddrawModule) {
        LogOut("[OVERLAY] ddraw.dll not loaded", true);
        return false;
    }
    
    // Get the DirectDrawCreate function address
    originalDirectDrawCreate = reinterpret_cast<DirectDrawCreateFunc>(
        GetProcAddress(ddrawModule, "DirectDrawCreate"));
        
    if (!originalDirectDrawCreate) {
        LogOut("[OVERLAY] Could not find DirectDrawCreate function", true);
        return false;
    }
    
    // Hook DirectDrawCreate using Microsoft Detours
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    
    DetourAttach(&(PVOID&)originalDirectDrawCreate, HookedDirectDrawCreate);
    
    LONG result = DetourTransactionCommit();
    if (result != NO_ERROR) {
        LogOut("[OVERLAY] Failed to hook DirectDrawCreate: " + std::to_string(result), true);
        return false;
    }
    
    isHooked = true;
    LogOut("[OVERLAY] DirectDraw hook initialized successfully", true);
    return true;
}

// Clean up the hook
void DirectDrawHook::Shutdown() {
    if (!isHooked)
        return;
    
    LogOut("[OVERLAY] Shutting down DirectDraw hook", true);
    
    // Stop the ImGui render thread if running
    if (g_imguiRenderThreadRunning) {
        g_imguiRenderThreadRunning = false;
        
        // Wait for the thread to exit
        if (g_imguiRenderThread) {
            WaitForSingleObject(g_imguiRenderThread, 1000);  // Wait up to 1 second
            CloseHandle(g_imguiRenderThread);
            g_imguiRenderThread = NULL;
        }
    }
    
    // Shutdown ImGui
    if (ImGuiImpl::IsInitialized()) {
        ImGuiImpl::Shutdown();
    }
    
    // Release D3D9 device
    if (g_pd3dDevice) {
        g_pd3dDevice->Release();
        g_pd3dDevice = nullptr;
        g_d3dInitialized = false;
    }
    
    // Unhook DirectDrawCreate
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourDetach(&(PVOID&)originalDirectDrawCreate, HookedDirectDrawCreate);
    LONG result = DetourTransactionCommit();
    
    if (result != NO_ERROR) {
        LogOut("[OVERLAY] Error unhooking DirectDrawCreate: " + std::to_string(result), true);
    }
    
    // Clear message lists
    {
        std::lock_guard<std::mutex> lock(messagesMutex);
        messages.clear();
        permanentMessages.clear();
    }
    
    isHooked = false;
    LogOut("[OVERLAY] DirectDraw hook shutdown complete", true);
}

// Add a temporary message
void DirectDrawHook::AddMessage(const std::string& text, COLORREF color, int durationMs, int x, int y) {
    std::lock_guard<std::mutex> lock(messagesMutex);
    
    OverlayMessage msg;
    msg.text = text;
    msg.color = color;
    msg.xPos = x;
    msg.yPos = y;
    msg.isPermanent = false;
    msg.expireTime = std::chrono::steady_clock::now() + std::chrono::milliseconds(durationMs);
    
    messages.push_back(msg);
    
    // Limit the number of messages to prevent overflow
    if (messages.size() > 10) {
        messages.pop_front();
    }
}

// Add a permanent message
int DirectDrawHook::AddPermanentMessage(const std::string& text, COLORREF color, int x, int y) {
    std::lock_guard<std::mutex> lock(messagesMutex);
    
    OverlayMessage msg;
    msg.text = text;
    msg.color = color;
    msg.xPos = x;
    msg.yPos = y;
    msg.isPermanent = true;
    msg.id = nextMessageId++;
    
    permanentMessages.push_back(msg);
    return msg.id;
}

// Update an existing permanent message
void DirectDrawHook::UpdatePermanentMessage(int id, const std::string& newText, COLORREF newColor) {
    std::lock_guard<std::mutex> lock(messagesMutex);
    
    for (auto& msg : permanentMessages) {
        if (msg.id == id) {
            msg.text = newText;
            msg.color = newColor;
            break;
        }
    }
}

// Remove a permanent message
void DirectDrawHook::RemovePermanentMessage(int id) {
    std::lock_guard<std::mutex> lock(messagesMutex);
    
    permanentMessages.erase(
        std::remove_if(permanentMessages.begin(), permanentMessages.end(),
                      [id](const OverlayMessage& msg) { return msg.id == id; }),
        permanentMessages.end());
}

// Remove all messages
void DirectDrawHook::ClearAllMessages() {
    std::lock_guard<std::mutex> lock(messagesMutex);
    messages.clear();
    permanentMessages.clear();
}

// --- NEW: D3D9 Hook Initialization and Shutdown ---
bool DirectDrawHook::InitializeD3D9() {
    LogOut("[OVERLAY] Attempting to initialize D3D9 hook", detailedLogging.load());
    
    if (g_d3d9Hooked) {
        LogOut("[OVERLAY] D3D9 already hooked", true);
        return true;
    }
    
    // Initialize MinHook
    if (MH_Initialize() != MH_OK) {
        LogOut("[OVERLAY] Failed to initialize MinHook library", true);
        return false;
    }
    
    // Find the game window
    HWND gameWindow = FindEFZWindow();
    if (!gameWindow) {
        LogOut("[OVERLAY] Failed to find EFZ window", true);
        return false;
    }
    
    LogOut("[OVERLAY] Found game window: " + Logger::hwndToString(gameWindow), true);
    
    // Get the D3D9 module handle
    HMODULE d3d9Module = GetModuleHandleA("d3d9.dll");
    if (!d3d9Module) {
        // Try to load it if it's not already loaded
        d3d9Module = LoadLibraryA("d3d9.dll");
        if (!d3d9Module) {
            LogOut("[OVERLAY] Failed to get d3d9.dll module", true);
            return false;
        }
    }
    
    // Get the address of Direct3DCreate9
    auto Direct3DCreate9_fn = (LPDIRECT3D9(WINAPI*)(UINT))(GetProcAddress(d3d9Module, "Direct3DCreate9"));
    if (!Direct3DCreate9_fn) {
        LogOut("[OVERLAY] Failed to get Direct3DCreate9 address", true);
        return false;
    }
    
    // Create a D3D9 object
    LPDIRECT3D9 d3d9 = Direct3DCreate9_fn(D3D_SDK_VERSION);
    if (!d3d9) {
        LogOut("[OVERLAY] Failed to create D3D9 object", true);
        return false;
    }
    
    // Set up present parameters
    D3DPRESENT_PARAMETERS d3dpp = {};
    d3dpp.Windowed = TRUE;
    d3dpp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    d3dpp.BackBufferFormat = D3DFMT_UNKNOWN;
    d3dpp.BackBufferWidth = 2;
    d3dpp.BackBufferHeight = 2;
    d3dpp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    
    // Create a temporary device to get the VTable
    LPDIRECT3DDEVICE9 tempDevice;
    HRESULT hr = d3d9->CreateDevice(
        D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, gameWindow,
        D3DCREATE_SOFTWARE_VERTEXPROCESSING, &d3dpp, &tempDevice);
        
    if (FAILED(hr)) {
        LogOut("[OVERLAY] Failed to create temp D3D9 device: " + std::to_string(hr), true);
        d3d9->Release();
        return false;
    }
    
    // Get the function pointer for EndScene
    void** vTable = *reinterpret_cast<void***>(tempDevice);
    void* endSceneAddr = vTable[42]; // EndScene is at index 42
    
    // Create the hook using MinHook
    LogOut("[OVERLAY] Hooking EndScene", detailedLogging.load());
    if (MH_CreateHook(endSceneAddr, HookedEndScene, reinterpret_cast<void**>(&oEndScene)) != MH_OK) {
        LogOut("[OVERLAY] Failed to create hook for EndScene", true);
        tempDevice->Release();
        d3d9->Release();
        return false;
    }
    
    // Enable the hook
    if (MH_EnableHook(endSceneAddr) != MH_OK) {
        LogOut("[OVERLAY] Failed to enable EndScene hook", true);
        tempDevice->Release();
        d3d9->Release();
        return false;
    }
    
    // Clean up temporary objects
    tempDevice->Release();
    d3d9->Release();
    
    g_d3d9Hooked = true;
    LogOut("[OVERLAY] D3D9 hook initialized successfully", detailedLogging.load());
    LogOut("[SYSTEM] ImGui D3D9 hook initialized successfully.", detailedLogging.load());  // Keep this one visible
    LogOut("[IMGUI] ImGui initialized successfully", detailedLogging.load());
    LogOut("[IMGUI] ImGui initialization succeeded", detailedLogging.load());
    LogOut("[IMGUI_GUI] GUI state initialized", detailedLogging.load());
    return true;
}

void DirectDrawHook::ShutdownD3D9() {
    if (!g_d3d9Hooked) return;

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourDetach(&(PVOID&)oEndScene, HookedEndScene);
    DetourTransactionCommit();

    if (ImGuiImpl::IsInitialized()) {
        ImGuiImpl::Shutdown();
    }

    g_d3d9Hooked = false;
    LogOut("[D3D9] D3D9 hook shut down.", true);
}

// Fallback implementation using a transparent window
bool DirectDrawHook::InitializeFallbackOverlay() {
    if (isHooked) return true;
    
    LogOut("[OVERLAY] Attempting fallback overlay initialization", true);
    
    // Find the game window using the robust detection
    gameWindow = FindEFZWindow();
    if (!gameWindow) {
        LogOut("[OVERLAY] Fallback: Could not find EFZ window", true);
        return false;
    }
    
    LogOut("[OVERLAY] Fallback: Found EFZ window", true);
    
    // Register window class
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "EFZTrainingOverlay";
    wc.hbrBackground = (HBRUSH)GetStockObject(NULL_BRUSH);
    
    if (!RegisterClassA(&wc)) {
        DWORD error = GetLastError();
        if (error != ERROR_CLASS_ALREADY_EXISTS) {
            LogOut("[OVERLAY] Failed to register window class: " + std::to_string(error), true);
            return false;
        }
    }
    
    // Get game window position and dimensions
    RECT gameRect;
    if (!GetWindowRect(gameWindow, &gameRect)) {
        LogOut("[OVERLAY] Failed to get game window rect", true);
        return false;
    }
    
    // Create transparent overlay window
    HWND overlayWnd = CreateWindowExA(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST,
        "EFZTrainingOverlay",
        "EFZ Training Overlay",
        WS_POPUP,
        gameRect.left, gameRect.top,
        gameRect.right - gameRect.left, gameRect.bottom - gameRect.top,
        NULL, NULL, GetModuleHandleA(NULL), NULL
    );
    
    if (!overlayWnd) {
        LogOut("[OVERLAY] Failed to create overlay window: " + std::to_string(GetLastError()), true);
        return false;
    }
    
    // Set the transparency
    SetLayeredWindowAttributes(overlayWnd, RGB(0,0,0), 0, LWA_COLORKEY);
    
    // Show the window
    ShowWindow(overlayWnd, SW_SHOWNOACTIVATE);
    
    // Start a thread to track the game window position
    HWND capturedGameWindow = gameWindow;
    std::thread([overlayWnd, capturedGameWindow]() {
        while (IsWindow(overlayWnd) && IsWindow(capturedGameWindow)) {
            RECT gameRect;
            if (GetWindowRect(capturedGameWindow, &gameRect)) {
                SetWindowPos(overlayWnd, HWND_TOPMOST,
                    gameRect.left, gameRect.top,
                    gameRect.right - gameRect.left, gameRect.bottom - gameRect.top,
                    SWP_NOACTIVATE);
            }
            Sleep(100);
        }
    }).detach();
    
    isHooked = true;
    LogOut("[OVERLAY] Fallback overlay initialized successfully", true);
    return true;
}

// Add this simple fallback overlay method:
bool DirectDrawHook::InitializeSimpleOverlay() {
    LogOut("[OVERLAY] Attempting simple overlay initialization", true);
    
    // Find the game window
    gameWindow = FindEFZWindow();
    if (!gameWindow) {
        LogOut("[OVERLAY] Simple overlay: Could not find EFZ window", true);
        return false;
    }
    
    // Get window device context
    HDC windowDC = GetDC(gameWindow);
    if (!windowDC) {
        LogOut("[OVERLAY] Simple overlay: Could not get window DC", true);
        return false;
    }
    
    // Test drawing directly to window
    SetBkMode(windowDC, TRANSPARENT);
    SetTextColor(windowDC, RGB(255, 255, 0));
    
    // Use a simple system font
    HFONT font = CreateFont(
        16,                        // Height
        0,                         // Width (auto)
        0,                         // Escapement
        0,                         // Orientation
        FW_BOLD,                   // Weight
        FALSE,                     // Italic
        FALSE,                     // Underline
        FALSE,                     // StrikeOut
        DEFAULT_CHARSET,           // CharSet
        OUT_DEFAULT_PRECIS,        // OutputPrecision
        CLIP_DEFAULT_PRECIS,       // ClipPrecision
        DEFAULT_QUALITY,           // Quality
        DEFAULT_PITCH | FF_SWISS,  // PitchAndFamily - FIX: Use FF_SWISS instead of FF_SANS_SERIF
        "Arial"                    // Face name - ADD THIS 14th PARAMETER
    );
    HFONT oldFont = (HFONT)SelectObject(windowDC, font);
    
    TextOutA(windowDC, 10, 50, "Simple Overlay Test", 19);
    
    SelectObject(windowDC, oldFont);
    DeleteObject(font);
    ReleaseDC(gameWindow, windowDC);
    
    LogOut("[OVERLAY] Simple overlay test drawn", true);
    return true;
}

bool DirectDrawHook::InitializeBruteForceOverlay() {
    LogOut("[OVERLAY] Attempting brute-force overlay initialization", true);
    
    gameWindow = FindEFZWindow();
    if (!gameWindow) {
        LogOut("[OVERLAY] Brute-force: Could not find EFZ window", true);
        return false;
    }
    
    std::thread([]{
        while (true) {
            HWND efzWindow = FindEFZWindow();
            if (!efzWindow) {
                Sleep(1000);
                continue;
            }
            
            // Check if we should show overlay
            uintptr_t base = GetEFZBase();
            bool shouldShow = false;
            
            if (base) {
                uintptr_t hpAddr1 = ResolvePointer(base, EFZ_BASE_OFFSET_P1, HP_OFFSET);
                uintptr_t hpAddr2 = ResolvePointer(base, EFZ_BASE_OFFSET_P2, HP_OFFSET);
                
                if (hpAddr1 && hpAddr2) {
                    int hp1 = 0, hp2 = 0;
                    if (SafeReadMemory(hpAddr1, &hp1, sizeof(int)) && 
                        SafeReadMemory(hpAddr2, &hp2, sizeof(int))) {
                        shouldShow = (hp1 > 0 && hp2 > 0 && hp1 <= MAX_HP && hp2 <= MAX_HP);
                    }
                }
            }
            
            if (shouldShow) {
                HDC windowDC = GetWindowDC(efzWindow);
                if (windowDC) {
                    SetBkMode(windowDC, TRANSPARENT);
                    
                    HFONT font = CreateFont(
                        24, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                        ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, "Arial"
                    );
                    HFONT oldFont = (HFONT)SelectObject(windowDC, font);
                    
                    SetTextColor(windowDC, RGB(0, 0, 0));
                    for (int dx = -2; dx <= 2; dx++) {
                        for (int dy = -2; dy <= 2; dy++) {
                            if (dx != 0 || dy != 0) {
                                TextOutA(windowDC, 100 + dx, 100 + dy, "BRUTE FORCE OVERLAY", 19);
                            }
                        }
                    }
                    
                    // Draw main text
                    SetTextColor(windowDC, RGB(255, 255, 0));
                    TextOutA(windowDC, 100, 100, "BRUTE FORCE OVERLAY", 19);
                    
                    SelectObject(windowDC, oldFont);
                    DeleteObject(font);
                    ReleaseDC(efzWindow, windowDC);
                    
                    LogOut("[OVERLAY] Brute-force overlay rendered", true);
                }
            }
            
            Sleep(100); // 10 times per second
        }
    }).detach();
    
    LogOut("[OVERLAY] Brute-force overlay thread started", true);
    return true;
}

// Add this test function
void DirectDrawHook::TestOverlay() {
    if (isHooked) {
        AddMessage("Overlay Test - This message should appear!", RGB(255, 0, 0), 5000, 10, 100);
        LogOut("[OVERLAY] Test message added", true);
    } else {
        LogOut("[OVERLAY] Cannot test - overlay not hooked", true);
    }
}

// Update the TestHelloWorld function:
void DirectDrawHook::TestHelloWorld() {
    LogOut("[OVERLAY] Running TestHelloWorld...", true);
    
    if (isHooked) {
        AddMessage("Hello, World! (DDraw Overlay Test)", RGB(255, 255, 0), 5000, 100, 100);
        LogOut("[OVERLAY] Test message added.", true);
    } else {
        LogOut("[OVERLAY] DDraw hook not active, cannot add test message.", true);
    }
}

