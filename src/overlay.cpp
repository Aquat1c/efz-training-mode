#include <fstream>
#include <iomanip>
#include <chrono>
#include <vector>
#include "../include/overlay.h"
#include "../include/logger.h"
#include "../include/utilities.h"
#include "../include/memory.h"    // Add this for SafeReadMemory
#include "../include/constants.h" // Add this for memory constants
#include "../3rdparty/detours/include/detours.h"
#include <algorithm>

// Initialize static members
DirectDrawCreateFunc DirectDrawHook::originalDirectDrawCreate = nullptr;
DirectDrawEnumerateFunc DirectDrawHook::originalDirectDrawEnumerate = nullptr;
bool DirectDrawHook::isHooked = false;
IDirectDrawSurface7* DirectDrawHook::primarySurface = nullptr;
HWND DirectDrawHook::gameWindow = NULL;  // Change from HWWND to HWND
std::mutex DirectDrawHook::messagesMutex;
std::deque<OverlayMessage> DirectDrawHook::messages;
std::vector<OverlayMessage> DirectDrawHook::permanentMessages;
int DirectDrawHook::nextMessageId = 1;

// Global message IDs
int g_AirtechStatusId = -1;
int g_JumpStatusId = -1;

// Function prototypes for vtable hooks
typedef HRESULT(WINAPI* Blit_t)(IDirectDrawSurface7*, LPRECT, IDirectDrawSurface7*, LPRECT, DWORD, LPDDBLTFX);
typedef HRESULT(WINAPI* Flip_t)(IDirectDrawSurface7*, IDirectDrawSurface7*, DWORD);

// Original function pointers
Blit_t originalBlit = nullptr;
Flip_t originalFlip = nullptr;

// VTable offsets
constexpr int DDRAW_BLIT_OFFSET = 5;  // Offset in the vtable for Blt
constexpr int DDRAW_FLIP_OFFSET = 11; // Offset in the vtable for Flip

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
    // Call the original function first
    HRESULT result = originalDirectDrawCreate(lpGUID, lplpDD, pUnkOuter);
    
    if (SUCCEEDED(result) && lplpDD) {
        LogOut("[OVERLAY] DirectDrawCreate hooked successfully", true);
        
        // Get the DirectDraw interface
        IDirectDraw* ddraw = static_cast<IDirectDraw*>(*lplpDD);
        
        // Create a cooperative level to allow us to set the display mode
        ddraw->SetCooperativeLevel(GetDesktopWindow(), DDSCL_NORMAL);
        
        // Query for IDirectDraw7 interface
        IDirectDraw7* ddraw7 = nullptr;
        if (SUCCEEDED(ddraw->QueryInterface(IID_IDirectDraw7, (void**)&ddraw7))) {
            LogOut("[OVERLAY] Got DirectDraw7 interface", true);
            
            // Create the primary surface
            DDSURFACEDESC2 ddsd = {0};
            ddsd.dwSize = sizeof(ddsd);
            ddsd.dwFlags = DDSD_CAPS;
            ddsd.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE;
            
            IDirectDrawSurface7* primary = nullptr;
            if (SUCCEEDED(ddraw7->CreateSurface(&ddsd, &primary, nullptr))) {
                LogOut("[OVERLAY] Created primary surface", true);
                primarySurface = primary;
                
                // Now hook the Blt and Flip methods
                HookVTableMethod(primary, DDRAW_BLIT_OFFSET, (void*)HookedBlit, (void**)&originalBlit);
                HookVTableMethod(primary, DDRAW_FLIP_OFFSET, (void*)HookedFlip, (void**)&originalFlip);
                
                LogOut("[OVERLAY] Hooked Blit and Flip methods", true);
            }
            
            ddraw7->Release();
        }
    }
    
    return result;
}

// The hook for the Blt method
HRESULT WINAPI DirectDrawHook::HookedBlit(IDirectDrawSurface7* This, LPRECT lpDestRect, 
                                        IDirectDrawSurface7* lpDDSrcSurface, LPRECT lpSrcRect, 
                                        DWORD dwFlags, LPDDBLTFX lpDDBltFx) {
    // Call the original function first
    HRESULT result = originalBlit(This, lpDestRect, lpDDSrcSurface, lpSrcRect, dwFlags, lpDDBltFx);
    
    // If this is the primary surface and we're blitting to the screen, render our messages
    if (SUCCEEDED(result) && This == primarySurface && !lpDDSrcSurface) {
        RenderAllMessages(This);
    }
    
    return result;
}

// The hook for the Flip method
HRESULT WINAPI DirectDrawHook::HookedFlip(IDirectDrawSurface7* This, 
                                        IDirectDrawSurface7* lpDDSurfaceTargetOverride, 
                                        DWORD dwFlags) {
    // Render our messages before flipping if this is the primary surface
    if (This == primarySurface) {
        RenderAllMessages(This);
    }
    
    // Call the original function
    HRESULT result = originalFlip(This, lpDDSurfaceTargetOverride, dwFlags);
    
    return result;
}

// Render text on the given surface
void DirectDrawHook::RenderText(HDC hdc, const std::string& text, int x, int y, COLORREF color) {
    if (!hdc || text.empty()) return;
    
    // Set up the text properties
    SetBkMode(hdc, TRANSPARENT);
    
    // Create a better font for our overlay
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
        DEFAULT_PITCH | FF_SWISS,  // PitchAndFamily
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

// Render all current messages on the surface
void DirectDrawHook::RenderAllMessages(IDirectDrawSurface7* surface) {
    if (!surface) return;
    
    std::lock_guard<std::mutex> lock(messagesMutex);
    
    // Get the current time
    auto now = std::chrono::steady_clock::now();
    
    // Get device context for drawing
    HDC hdc;
    if (FAILED(surface->GetDC(&hdc))) {
        return;
    }
    
    try {
        // Remove expired temporary messages
        messages.erase(
            std::remove_if(messages.begin(), messages.end(),
                [now](const OverlayMessage& msg) {
                    return !msg.isPermanent && now > msg.expireTime;
                }),
            messages.end()
        );
        
        // Render permanent messages first
        for (const auto& msg : permanentMessages) {
            RenderText(hdc, msg.text, msg.xPos, msg.yPos, msg.color);
        }
        
        // Render temporary messages
        for (const auto& msg : messages) {
            RenderText(hdc, msg.text, msg.xPos, msg.yPos, msg.color);
        }
        
        // Force the drawing to complete
        GdiFlush();
        
    } catch (...) {
        // Ensure we always release the DC
    }
    
    // Always release the device context
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
    if (!isHooked) return;
    
    LogOut("[OVERLAY] Shutting down DirectDraw hook", true);
    
    // Unhook DirectDrawCreate
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourDetach(&(PVOID&)originalDirectDrawCreate, HookedDirectDrawCreate);
    LONG result = DetourTransactionCommit();
    
    if (result != NO_ERROR) {
        LogOut("[OVERLAY] Failed to unhook DirectDrawCreate: " + std::to_string(result), true);
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

// Add this function before the Initialize() function:

bool DirectDrawHook::TryInitializeOverlay() {
    static bool initializationAttempted = false;
    static std::chrono::steady_clock::time_point lastAttempt;
    static int attemptCount = 0;
    
    // Don't attempt if already hooked
    if (isHooked) return true;
    
    auto now = std::chrono::steady_clock::now();
    
    // Only try once every 2 seconds to avoid excessive polling
    if (initializationAttempted && 
        std::chrono::duration_cast<std::chrono::seconds>(now - lastAttempt).count() < 2) {
        return false;
    }
    
    initializationAttempted = true;
    lastAttempt = now;
    attemptCount++;
    
    // Increase max attempts from 30 to 120 (4 minutes of trying)
    if (attemptCount > 120) {
        LogOut("[OVERLAY] Maximum initialization attempts reached", true);
        return false;
    }
    
    // 1. Check if game window exists using the robust detection
    gameWindow = FindEFZWindow();
    if (!gameWindow) {
        if (attemptCount % 10 == 0) {
            LogOut("[OVERLAY] EFZ window not found (attempt " + std::to_string(attemptCount) + ")", true);
        }
        return false;
    }
    
    // Log that we found the window
    if (attemptCount % 10 == 0) {
        LogOut("[OVERLAY] Found EFZ window, checking game state (attempt " + std::to_string(attemptCount) + ")", true);
    }
    
    // 2. Check if player data is initialized
    uintptr_t base = GetEFZBase();
    if (!base) {
        if (attemptCount % 10 == 0) {
            LogOut("[OVERLAY] Game base address not found", true);
        }
        return false;
    }
    
    // Relaxed criteria - only check P1's data since it's more reliable
    uintptr_t hpAddr1 = ResolvePointer(base, EFZ_BASE_OFFSET_P1, HP_OFFSET);
    
    if (!hpAddr1) {
        if (attemptCount % 10 == 0) {
            LogOut("[OVERLAY] P1 HP address not resolved", true);
        }
        return false;
    }
    
    // 3. Verify HP value is readable
    int hp1 = 0;
    if (!SafeReadMemory(hpAddr1, &hp1, sizeof(int))) {
        if (attemptCount % 10 == 0) {
            LogOut("[OVERLAY] Cannot read P1 HP value", true);
        }
        return false;
    }
    
    // HP should be a reasonable value (between 0 and MAX_HP)
    if (hp1 < 0 || hp1 > MAX_HP) {
        if (attemptCount % 10 == 0) {
            LogOut("[OVERLAY] P1 HP value out of range: " + std::to_string(hp1), true);
        }
        return false;
    }
    
    // All checks passed, initialize the overlay
    LogOut("[OVERLAY] Game ready, initializing overlay", true);
    
    // Use a try-catch block to handle potential DirectDraw initialization issues
    try {
        return Initialize();
    } catch (const std::exception& e) {
        LogOut("[OVERLAY] Exception during initialization: " + std::string(e.what()), true);
        return false;
    } catch (...) {
        LogOut("[OVERLAY] Unknown exception during initialization", true);
        return false;
    }
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

// Add this test function
void DirectDrawHook::TestOverlay() {
    if (isHooked) {
        AddMessage("Overlay Test - This message should appear!", RGB(255, 0, 0), 5000, 10, 100);
        LogOut("[OVERLAY] Test message added", true);
    } else {
        LogOut("[OVERLAY] Cannot test - overlay not hooked", true);
    }
}