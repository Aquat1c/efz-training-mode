#include <fstream>
#include <iomanip>
#include <chrono>
#include <vector>
#include <thread>
#include <atomic> // Add this for std::atomic
#include "../include/gui/overlay.h"
#include "../include/core/logger.h"
#include "../include/utils/utilities.h"
#include "../include/core/globals.h"
#include "../include/core/memory.h"   
#include "../include/core/constants.h" 
#include "../3rdparty/detours/include/detours.h"
#include <algorithm>
#include "../include/gui/imgui_impl.h"
#include <d3d9.h>
#include <mutex>
#include <deque>
#include "../include/gui/imgui_gui.h"
#include "../3rdparty/minhook/include/MinHook.h"
// ADD these includes for the new rendering loop
#include "../include/gui/imgui_impl.h"

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
    static bool imguiInitialized = false;
    static int initFailCount = 0;
    const int MAX_INIT_ATTEMPTS = 5;
    
    // Critical: Check if shutdown is in progress
    if (g_isShuttingDown.load()) {
        return oEndScene(pDevice);
    }
    
    // Only try to initialize if not already done
    if (!imguiInitialized && initFailCount < MAX_INIT_ATTEMPTS) {
        // Validate D3D device before initialization
        if (pDevice) {
            D3DDEVICE_CREATION_PARAMETERS params;
            if (SUCCEEDED(pDevice->GetCreationParameters(&params))) {
                if (ImGuiImpl::Initialize(pDevice)) {
                    imguiInitialized = true;
                    LogOut("[D3D9] ImGui initialized successfully in EndScene", true);
                } else {
                    initFailCount++;
                    LogOut("[D3D9] ImGui initialization failed (attempt " + 
                           std::to_string(initFailCount) + "/" + 
                           std::to_string(MAX_INIT_ATTEMPTS) + ")", true);
                }
            }
        }
    }
    
    // Render ImGui if initialized and visible
    if (imguiInitialized && ImGuiImpl::IsVisible()) {
        // Extra safety: Validate ImGui context and style
        if (ImGui::GetCurrentContext() != nullptr) {
            ImGuiStyle& style = ImGui::GetStyle();
            if (style.WindowMinSize.x < 1.0f || style.WindowMinSize.y < 1.0f) {
                style.WindowMinSize.x = 100.0f;
                style.WindowMinSize.y = 100.0f;
            }
            ImGuiImpl::RenderFrame();
        }
    }

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
    
    // Get screen width to avoid going off-screen
    // Using 640 as default game window width, and allowing 20px margin
    const int screenWidth = 640;
    const int MAX_TEXT_WIDTH = screenWidth - x - 20;
    
    // Check if this is a trigger overlay by position (right-aligned text)
    bool isTriggerOverlay = (x >= 510 && y >= 140 && y <= 200);
    
    // For trigger overlay, adjust X position instead of truncating
    int adjustedX = x;
    std::string displayText = text;
    
    // Measure text size
    SIZE textSize;
    GetTextExtentPoint32A(hdc, text.c_str(), text.length(), &textSize);
    
    if (isTriggerOverlay && textSize.cx > MAX_TEXT_WIDTH) {
        // Calculate how far left we need to move the text
        // to ensure the last character is at the right edge
        adjustedX = screenWidth - 20 - textSize.cx;
    } else if (!isTriggerOverlay && textSize.cx > MAX_TEXT_WIDTH) {
        // For regular overlay, truncate text from the right
        displayText = FitTextToWidth(text, MAX_TEXT_WIDTH, hdc);
    }
    
    // Draw drop shadow first (for better visibility)
    SetTextColor(hdc, RGB(0, 0, 0));
    TextOutA(hdc, adjustedX + 2, y + 2, displayText.c_str(), displayText.length());
    
    // Draw the main text
    SetTextColor(hdc, color);
    TextOutA(hdc, adjustedX, y, displayText.c_str(), displayText.length());
    
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
        
        // Get screen width to avoid going off-screen (640 is standard game width)
        const int screenWidth = 640;
        const int MAX_TEXT_WIDTH = screenWidth - x - 20;
        
        // Check if this is a trigger overlay by position (right-aligned text)
        bool isTriggerOverlay = (x >= 510 && y >= 140 && y <= 200);
        
        // For trigger overlay, adjust X position instead of truncating
        int adjustedX = x;
        std::string displayText = text;
        
        // Measure text size
        SIZE textSize;
        GetTextExtentPoint32A(hdc, text.c_str(), text.length(), &textSize);
        
        if (isTriggerOverlay && textSize.cx > MAX_TEXT_WIDTH) {
            // Calculate how far left we need to move the text
            // to ensure the last character is at the right edge
            adjustedX = screenWidth - 20 - textSize.cx;
        } else if (!isTriggerOverlay && textSize.cx > MAX_TEXT_WIDTH) {
            // For regular overlay, truncate text from the right
            displayText = FitTextToWidth(text, MAX_TEXT_WIDTH, hdc);
        }
        
        // Draw black outline for better visibility (thinner outline)
        SetTextColor(hdc, RGB(0, 0, 0));
        TextOutA(hdc, adjustedX + 1, y, displayText.c_str(), displayText.length());
        TextOutA(hdc, adjustedX - 1, y, displayText.c_str(), displayText.length());
        TextOutA(hdc, adjustedX, y + 1, displayText.c_str(), displayText.length());
        TextOutA(hdc, adjustedX, y - 1, displayText.c_str(), displayText.length());
        
        // Draw main text
        SetTextColor(hdc, color);
        TextOutA(hdc, adjustedX, y, displayText.c_str(), displayText.length());
        
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
    if (!drawList)
        return;

    std::lock_guard<std::mutex> lock(messagesMutex);

    // Helper lambda to render a message with a background
    auto renderMessage = [&](const OverlayMessage& msg) {
        // Check if this is a trigger overlay by position
        bool isTriggerOverlay = (msg.xPos >= 510 && msg.yPos >= 140 && msg.yPos <= 200);
        
        // Calculate starting position
        ImVec2 textPos(msg.xPos, msg.yPos);
        ImVec2 textSize = ImGui::CalcTextSize(msg.text.c_str());
        
        if (isTriggerOverlay) {
            // For trigger overlays, adjust X position so text ends at screen edge
            const float screenWidth = 640.0f;  // Standard EFZ window width
            const float margin = 20.0f;        // Margin from screen edge
            const float targetX = screenWidth - margin;  // Where we want text to end
            
            // If text would go off-screen, adjust position
            if (textPos.x + textSize.x > targetX) {
                textPos.x = targetX - textSize.x;
            }
            
            // Draw background with corrected position and width
            drawList->AddRectFilled(
                ImVec2(textPos.x - 4, textPos.y - 2),
                ImVec2(textPos.x + textSize.x + 4, textPos.y + textSize.y + 2),
                IM_COL32(0, 0, 0, 180)
            );
        } else {
            // For normal messages, just draw background directly
            drawList->AddRectFilled(
                ImVec2(textPos.x - 4, textPos.y - 2),
                ImVec2(textPos.x + textSize.x + 4, textPos.y + textSize.y + 2),
                IM_COL32(0, 0, 0, 180)
            );
        }
        
        // Extract color components
        int r = (msg.color & 0xFF);
        int g = ((msg.color >> 8) & 0xFF);
        int b = ((msg.color >> 16) & 0xFF);
        
        // Draw text
        drawList->AddText(ImVec2((float)textPos.x, (float)textPos.y), IM_COL32(r, g, b, 255), msg.text.c_str());
    };

    // Render permanent messages
    for (const auto& msg : permanentMessages) {
        renderMessage(msg);
    }

    // Render temporary messages
    auto now = std::chrono::steady_clock::now();
    for (const auto& msg : messages) {
        if (msg.expireTime > now) {
            renderMessage(msg);
        }
    }
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
    LogOut("[OVERLAY] Shutting down D3D9 hooks.", true);
    // Disable both hooks
    HMODULE hD3D9 = GetModuleHandleA("d3d9.dll");
    if (hD3D9) {
        void* pCreateFn = GetProcAddress(hD3D9, "Direct3DCreate9");
        if (pCreateFn) {
            MH_DisableHook(pCreateFn);
            MH_RemoveHook(pCreateFn);
        }
    }
    if (oEndScene) {
        // To get the target address for EndScene, we need to re-resolve it briefly
        // This is a bit ugly but necessary without storing the vTable address globally.
        // A better long-term solution would be to store the vTable address.
        // For now, this will work.
        IDirect3D9* d3d9 = Direct3DCreate9(D3D_SDK_VERSION);
        if (d3d9) {
            D3DPRESENT_PARAMETERS d3dpp = {};
            d3dpp.Windowed = TRUE;
            d3dpp.SwapEffect = D3DSWAPEFFECT_DISCARD;
            d3dpp.hDeviceWindow = FindEFZWindow();
            IDirect3DDevice9* pDummyDevice = nullptr;
            if (SUCCEEDED(d3d9->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, d3dpp.hDeviceWindow, D3DCREATE_SOFTWARE_VERTEXPROCESSING, &d3dpp, &pDummyDevice))) {
                void** vTable = *reinterpret_cast<void***>(pDummyDevice);
                MH_DisableHook(vTable[42]);
                MH_RemoveHook(vTable[42]);
                pDummyDevice->Release();
            }
            d3d9->Release();
        }
    }
    
    // REMOVED: MH_Uninitialize() is now called globally in dllmain.cpp
}

// NEW: Text fitting utility to prevent text from going off-screen
std::string DirectDrawHook::FitTextToWidth(const std::string& text, int maxWidth, HDC hdc) {
    if (text.empty() || maxWidth <= 0) return text;
    
    // Create a temporary DC if none provided
    HDC tempDC = hdc;
    bool needToReleaseDC = false;
    
    if (!tempDC) {
        tempDC = CreateCompatibleDC(NULL);
        needToReleaseDC = true;
    }
    
    // Create and select font to measure text accurately
    HFONT font = CreateFont(
        20, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_OUTLINE_PRECIS, CLIP_DEFAULT_PRECIS,
        ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, "Arial"
    );
    HFONT oldFont = (HFONT)SelectObject(tempDC, font);
    
    // Measure text
    SIZE textSize;
    GetTextExtentPoint32A(tempDC, text.c_str(), text.length(), &textSize);
    
    std::string result = text;
    
    // If text is too long, truncate and add ellipsis
    if (textSize.cx > maxWidth) {
        // Start with whole string and binary search to find fitting length
        int left = 0;
        int right = text.length();
        std::string ellipsis = "...";
        SIZE ellipsisSize;
        GetTextExtentPoint32A(tempDC, ellipsis.c_str(), ellipsis.length(), &ellipsisSize);
        
        while (left < right) {
            int mid = (left + right + 1) / 2;
            std::string testStr = text.substr(0, mid);
            SIZE testSize;
            GetTextExtentPoint32A(tempDC, testStr.c_str(), testStr.length(), &testSize);
            
            if (testSize.cx + ellipsisSize.cx <= maxWidth) {
                left = mid;
            } else {
                right = mid - 1;
            }
        }
        
        // Ensure we don't cut in the middle of a word if possible
        int cutPoint = left;
        if (cutPoint > 10) {  // Only if we have enough text to work with
            while (cutPoint > 0 && text[cutPoint] != ' ' && text[cutPoint] != ',') {
                cutPoint--;
            }
            if (text[cutPoint] == ' ' || text[cutPoint] == ',') {
                cutPoint++; // Move past the space/comma
            } else {
                cutPoint = left; // No good word break found, revert to original
            }
        }
        
        result = text.substr(0, cutPoint) + ellipsis;
    }
    
    // Clean up
    SelectObject(tempDC, oldFont);
    DeleteObject(font);
    
    if (needToReleaseDC) {
        DeleteDC(tempDC);
    }
    
    return result;
}

// NEW: Text fitting utility to fit text within a width by truncating from the left side
std::string DirectDrawHook::FitTextToWidthFromLeft(const std::string& text, int maxWidth, HDC hdc) {
    if (text.empty() || maxWidth <= 0) return text;
    
    // Create a temporary DC if none provided
    HDC tempDC = hdc;
    bool needToReleaseDC = false;
    
    if (!tempDC) {
        tempDC = CreateCompatibleDC(NULL);
        needToReleaseDC = true;
    }
    
    // Create and select font to measure text accurately
    HFONT font = CreateFont(
        20, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_OUTLINE_PRECIS, CLIP_DEFAULT_PRECIS,
        ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, "Arial"
    );
    HFONT oldFont = (HFONT)SelectObject(tempDC, font);
    
    // Measure text
    SIZE textSize;
    GetTextExtentPoint32A(tempDC, text.c_str(), text.length(), &textSize);
    
    std::string result = text;
    
    // For right-aligned text, we don't truncate but calculate the adjusted X position
    // so that the last character is always at the right edge (maxWidth)
    if (textSize.cx > maxWidth) {
        // This function doesn't actually modify the text, it just returns the original
        // The caller will need to adjust the X position when drawing
        
        // We return the original text, as we'll adjust position instead
        result = text;
    }
    
    // Clean up
    SelectObject(tempDC, oldFont);
    DeleteObject(font);
    
    if (needToReleaseDC) {
        DeleteDC(tempDC);
    }
    
    return result;
}

