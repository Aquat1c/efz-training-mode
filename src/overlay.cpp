#include <fstream>
#include <iomanip>
#include <chrono>
#include <vector>
#include <thread>
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
    
    // CRITICAL FIX: Only render on specific blit operations that indicate final screen composition
    if (SUCCEEDED(result) && This == primarySurface) {
        // Check if this looks like a final screen blit (no source surface = screen-to-screen blit)
        if (!lpDDSrcSurface || dwFlags & DDBLT_COLORFILL) {
            RenderAllMessages(This);
        }
    }
    
    return result;
}

// The hook for the Flip method
HRESULT WINAPI DirectDrawHook::HookedFlip(IDirectDrawSurface7* This, 
                                        IDirectDrawSurface7* lpDDSurfaceTargetOverride, 
                                        DWORD dwFlags) {
    // Call the original function first
    HRESULT result = originalFlip(This, lpDDSurfaceTargetOverride, dwFlags);
    
    // CRITICAL FIX: Render AFTER the flip, not before
    if (SUCCEEDED(result) && This == primarySurface) {
        RenderAllMessages(This);
    }
    
    return result;
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
    
    // Try more frequently - every 500ms instead of 2 seconds
    if (initializationAttempted && 
        std::chrono::duration_cast<std::chrono::milliseconds>(now - lastAttempt).count() < 500) {
        return false;
    }
    
    initializationAttempted = true;
    lastAttempt = now;
    attemptCount++;
    
    // Increase max attempts to 240 (2 minutes of trying)
    if (attemptCount > 240) {
        LogOut("[OVERLAY] Maximum initialization attempts reached", true);
        return false;
    }
    
    // 1. Check if game window exists
    gameWindow = FindEFZWindow();
    if (!gameWindow) {
        if (attemptCount % 20 == 0) {  // Log every 10 seconds
            LogOut("[OVERLAY] EFZ window not found (attempt " + std::to_string(attemptCount) + ")", true);
        }
        return false;
    }
    
    // 2. Check if the game has loaded DirectDraw
    HMODULE ddrawModule = GetModuleHandleA("ddraw.dll");
    if (!ddrawModule) {
        if (attemptCount % 20 == 0) {
            LogOut("[OVERLAY] ddraw.dll not loaded yet", true);
        }
        return false;
    }
    
    // 3. Check if player data exists (optional - we can show overlay even without game data)
    uintptr_t base = GetEFZBase();
    if (base) {
        uintptr_t hpAddr1 = ResolvePointer(base, EFZ_BASE_OFFSET_P1, HP_OFFSET);
        if (hpAddr1) {
            int hp1 = 0;
            if (SafeReadMemory(hpAddr1, &hp1, sizeof(int))) {
                if (hp1 >= 0 && hp1 <= MAX_HP) {
                    LogOut("[OVERLAY] Game data detected, initializing overlay", true);
                }
            }
        }
    }
    
    // Initialize the overlay
    LogOut("[OVERLAY] Attempting to initialize DirectDraw hook (attempt " + std::to_string(attemptCount) + ")", true);
    
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
    
    // Draw test text
    TextOutA(windowDC, 10, 50, "Simple Overlay Test", 19);
    
    // Clean up
    SelectObject(windowDC, oldFont);
    DeleteObject(font);
    ReleaseDC(gameWindow, windowDC);
    
    LogOut("[OVERLAY] Simple overlay test drawn", true);
    return true;
}

// Add this aggressive fallback method:
bool DirectDrawHook::InitializeBruteForceOverlay() {
    LogOut("[OVERLAY] Attempting brute-force overlay initialization", true);
    
    // Find the game window
    gameWindow = FindEFZWindow();
    if (!gameWindow) {
        LogOut("[OVERLAY] Brute-force: Could not find EFZ window", true);
        return false;
    }
    
    // Create a timer-based overlay that draws directly to the window
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
                // Get window device context and draw directly
                HDC windowDC = GetWindowDC(efzWindow);
                if (windowDC) {
                    SetBkMode(windowDC, TRANSPARENT);
                    
                    // Create a bright, large font
                    HFONT font = CreateFont(
                        24, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                        ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, "Arial"
                    );
                    HFONT oldFont = (HFONT)SelectObject(windowDC, font);
                    
                    // Draw with heavy outline
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
            
            Sleep(100); // Update 10 times per second
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
    LogOut("[OVERLAY] TestHelloWorld called - trying all methods", true);
    
    if (!isHooked) {
        LogOut("[OVERLAY] Testing overlay - not yet hooked, trying all initialization methods", true);
        
        // Try DirectDraw hook first
        if (TryInitializeOverlay()) {
            LogOut("[OVERLAY] DirectDraw hook initialized for testing", true);
        } 
        // Try fallback method
        else if (InitializeFallbackOverlay()) {
            LogOut("[OVERLAY] Fallback overlay initialized for testing", true);
        }
        // Try brute force method
        else if (InitializeBruteForceOverlay()) {
            LogOut("[OVERLAY] Brute-force overlay initialized for testing", true);
        }
        else {
            LogOut("[OVERLAY] All overlay initialization methods failed", true);
            return;
        }
    }
    
    // Add multiple test messages at different positions
    AddMessage("Hello, world! (Test 1)", RGB(255, 255, 0), 5000, 10, 60);
    AddMessage("Hello, world! (Test 2)", RGB(0, 255, 0), 5000, 10, 80);
    AddMessage("Hello, world! (Test 3)", RGB(0, 255, 255), 5000, 320, 100);
    AddMessage("Hello, world! (Test 4)", RGB(255, 0, 255), 5000, 500, 60);
    
    LogOut("[OVERLAY] Multiple test 'Hello, world' messages added at different positions", true);
}