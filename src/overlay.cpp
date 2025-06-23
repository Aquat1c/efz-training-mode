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
// ADD these includes for the new rendering loop
#include "../include/imgui_impl.h"

// Global status message IDs
int g_AirtechStatusId = -1;
int g_JumpStatusId = -1;
int g_FrameAdvantageId = -1;

// NEW: Individual trigger message IDs
int g_TriggerAfterBlockId = -1;
int g_TriggerOnWakeupId = -1;
int g_TriggerAfterHitstunId = -1;
int g_TriggerAfterAirtechId = -1;

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
// --- End D3D9 Globals ---

// --- FIX: Add missing implementations for obsolete DirectDraw hooks ---
HRESULT WINAPI DirectDrawHook::HookedDirectDrawCreate(GUID* lpGUID, LPVOID* lplpDD, IUnknown* pUnkOuter) {
    // This function is obsolete. It is stubbed to resolve linker errors.
    // The new overlay method uses D3D9 hooking.
    if (originalDirectDrawCreate) {
        return originalDirectDrawCreate(lpGUID, lplpDD, pUnkOuter);
    }
    return E_FAIL;
}

HRESULT WINAPI DirectDrawHook::HookedBlit(IDirectDrawSurface7* This, LPRECT lpDestRect, IDirectDrawSurface7* lpDDSrcSurface, LPRECT lpSrcRect, DWORD dwFlags, LPDDBLTFX lpDDBltFx) {
    // This function is obsolete. It is stubbed to resolve linker errors.
    if (originalBlit) {
        return originalBlit(This, lpDestRect, lpDDSrcSurface, lpSrcRect, dwFlags, lpDDBltFx);
    }
    return E_FAIL;
}

HRESULT WINAPI DirectDrawHook::HookedFlip(IDirectDrawSurface7* This, IDirectDrawSurface7* lpDDSurfaceTargetOverride, DWORD dwFlags) {
    // This function is obsolete. It is stubbed to resolve linker errors.
    if (originalFlip) {
        return originalFlip(This, lpDDSurfaceTargetOverride, dwFlags);
    }
    return E_FAIL;
}
// --- End of fix ---

// --- REVISED AND CORRECTED D3D9 EndScene Hook ---
HRESULT WINAPI HookedEndScene(LPDIRECT3DDEVICE9 pDevice) {
    if (!pDevice) {
        return oEndScene(pDevice);
    }

    static bool imguiInit = false;
    if (!imguiInit) {
        if (ImGuiImpl::Initialize(pDevice)) {
            imguiInit = true;
            LogOut("[OVERLAY] ImGui initialized from EndScene hook.", true);
        } else {
            LogOut("[OVERLAY] ImGui failed to initialize from EndScene hook.", true);
            return oEndScene(pDevice); // Don't proceed if init fails
        }
    }

    // Start a new ImGui frame
    ImGui_ImplDX9_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    // Render our custom text overlays using the background draw list
    DirectDrawHook::RenderD3D9Overlays(pDevice);

    // Render the main ImGui configuration window if it's visible
    if (ImGuiImpl::IsVisible()) {
        ImGuiGui::RenderGui();
    }

    // End the frame and render all accumulated draw data
    ImGui::EndFrame();
    ImGui::Render();
    ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());

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
    // This function is for the old DirectDraw hook and is no longer the primary rendering path.
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

// NEW: Implement the D3D9 overlay renderer
void DirectDrawHook::RenderD3D9Overlays(LPDIRECT3DDEVICE9 pDevice) {
    auto drawList = ImGui::GetBackgroundDrawList();
    if (!drawList) return;

    std::lock_guard<std::mutex> lock(messagesMutex);

    // Helper lambda to render a message with a background
    auto renderMessage = [&](const OverlayMessage& msg) {
        ImVec2 textPos((float)msg.xPos, (float)msg.yPos);
        ImVec2 textSize = ImGui::CalcTextSize(msg.text.c_str());

        // Add a small padding for the background
        ImVec2 bgMin(textPos.x - 4, textPos.y - 2);
        ImVec2 bgMax(textPos.x + textSize.x + 4, textPos.y + textSize.y + 2);

        // Draw the semi-transparent background
        drawList->AddRectFilled(bgMin, bgMax, IM_COL32(0, 0, 0, 128));

        // Draw the text (FIX: Convert COLORREF to ImU32 for correct color and alpha)
        ImU32 textColor = IM_COL32(GetRValue(msg.color), GetGValue(msg.color), GetBValue(msg.color), 255);
        drawList->AddText(textPos, textColor, msg.text.c_str());
    };

    // Render permanent messages
    for (const auto& msg : permanentMessages) {
        renderMessage(msg);
    }

    // Render temporary messages
    auto now = std::chrono::steady_clock::now();
    messages.erase(std::remove_if(messages.begin(), messages.end(),
        [&](const OverlayMessage& msg) {
            if (now > msg.expireTime) {
                return true;
            }
            renderMessage(msg);
            return false;
        }), messages.end());
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
    {
        std::lock_guard<std::mutex> lock(messagesMutex);
        messages.clear();
        permanentMessages.clear();
    }
    ShutdownD3D9();
}

// Add a temporary message
void DirectDrawHook::AddMessage(const std::string& text, const std::string& category, COLORREF color, int durationMs, int x, int y) {
    std::lock_guard<std::mutex> lock(messagesMutex);

    // Remove existing temporary messages of the same category
    if (!category.empty()) {
        messages.erase(std::remove_if(messages.begin(), messages.end(),
            [&](const OverlayMessage& msg) {
                return !msg.isPermanent && msg.category == category;
            }), messages.end());
    }

    auto expireTime = std::chrono::steady_clock::now() + std::chrono::milliseconds(durationMs);
    messages.push_back({text, color, expireTime, x, y, false, -1, category});
}

// Add a permanent message
int DirectDrawHook::AddPermanentMessage(const std::string& text, COLORREF color, int x, int y) {
    std::lock_guard<std::mutex> lock(messagesMutex);
    
    // FIX: Declare newId and increment the static counter
    int newId = nextMessageId++;
    
    // FIX: Add the missing 'category' member to the initializer list.
    // Permanent messages don't need a category, so we use an empty string.
    permanentMessages.push_back({text, color, {}, x, y, true, newId, ""});
    
    return newId;
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
    permanentMessages.erase(std::remove_if(permanentMessages.begin(), permanentMessages.end(),
        [id](const OverlayMessage& msg) { return msg.id == id; }), permanentMessages.end());
}

// NEW: Remove messages by category
void DirectDrawHook::RemoveMessagesByCategory(const std::string& category) {
    std::lock_guard<std::mutex> lock(messagesMutex);
    if (category.empty()) return;

    // Remove temporary messages of the specified category
    messages.erase(std::remove_if(messages.begin(), messages.end(),
        [&](const OverlayMessage& msg) {
            return !msg.isPermanent && msg.category == category;
        }), messages.end());
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
    
    if (isHooked) { // FIX: Use the class's static member variable
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
    
    isHooked = true;
    LogOut("[OVERLAY] D3D9 hook initialized successfully", detailedLogging.load());
    LogOut("[SYSTEM] ImGui D3D9 hook initialized successfully.", detailedLogging.load());  // Keep this one visible
    LogOut("[IMGUI] ImGui initialized successfully", detailedLogging.load());
    LogOut("[IMGUI] ImGui initialization succeeded", detailedLogging.load());
    LogOut("[IMGUI_GUI] GUI state initialized", detailedLogging.load());
    LogOut("[OVERLAY] D3D9 EndScene hook installed successfully.", true);
    return true;
}

void DirectDrawHook::ShutdownD3D9() {
    LogOut("[OVERLAY] Shutting down D3D9 hook...", true);
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
}

void DirectDrawHook::SetupWindowProcedures() {
    // This function can be implemented later if needed for more complex input handling.
    LogOut("[OVERLAY] SetupWindowProcedures called (currently a stub).", detailedLogging.load());
}

// REMOVE the following obsolete function implementations
/*
// Fallback implementation using a transparent window
bool DirectDrawHook::InitializeFallbackOverlay() {
    LogOut("[OVERLAY] InitializeFallbackOverlay is obsolete and has been disabled.", true);
    return false;
}

// Add this simple fallback overlay method:
bool DirectDrawHook::InitializeSimpleOverlay() {
    LogOut("[OVERLAY] InitializeSimpleOverlay is obsolete and has been disabled.", true);
    return false;
}

bool DirectDrawHook::InitializeBruteForceOverlay() {
    LogOut("[OVERLAY] InitializeBruteForceOverlay is obsolete and has been disabled.", true);
    return false;
}

// Add this test function
void DirectDrawHook::TestOverlay() {
    LogOut("[OVERLAY] TestOverlay is obsolete and has been disabled.", true);
}

// Update the TestHelloWorld function:
void DirectDrawHook::TestHelloWorld() {
    LogOut("[OVERLAY] TestHelloWorld is obsolete and has been disabled.", true);
}
*/

