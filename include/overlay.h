#pragma once
#include <windows.h>
#include <ddraw.h>
#include <string>
#include <vector>
#include <deque>
#include <chrono>
#include <mutex>

// Function pointer types for DirectDraw
typedef HRESULT(WINAPI* DirectDrawCreateFunc)(GUID*, LPVOID*, IUnknown*);
typedef HRESULT(WINAPI* DirectDrawEnumerateFunc)(LPDDENUMCALLBACKA, LPVOID);

// Structure to represent a text message in the overlay
struct OverlayMessage {
    std::string text;
    COLORREF color;
    std::chrono::steady_clock::time_point expireTime;
    int xPos;
    int yPos;
    bool isPermanent;  // If true, stays until explicitly removed
    int id;            // Unique ID for permanent messages
};

class DirectDrawHook {
private:
    // Original DirectDraw functions
    static DirectDrawCreateFunc originalDirectDrawCreate;
    static DirectDrawEnumerateFunc originalDirectDrawEnumerate;
    
    // Hook state
    static IDirectDrawSurface7* primarySurface;
    static HWND gameWindow;
    static std::mutex messagesMutex;
    static std::deque<OverlayMessage> messages;
    static std::vector<OverlayMessage> permanentMessages;
    static int nextMessageId;
    
    // Hook functions
    static HRESULT WINAPI HookedDirectDrawCreate(GUID* lpGUID, LPVOID* lplpDD, IUnknown* pUnkOuter);
    static HRESULT WINAPI HookedBlit(IDirectDrawSurface7* This, LPRECT lpDestRect, 
                                    IDirectDrawSurface7* lpDDSrcSurface, LPRECT lpSrcRect, 
                                    DWORD dwFlags, LPDDBLTFX lpDDBltFx);
    static HRESULT WINAPI HookedFlip(IDirectDrawSurface7* This, 
                                    IDirectDrawSurface7* lpDDSurfaceTargetOverride, 
                                    DWORD dwFlags);
    
    // Rendering helper functions
    static void RenderText(HDC hdc, const std::string& text, int x, int y, COLORREF color);
    static void RenderSimpleText(IDirectDrawSurface7* surface, const std::string& text, int x, int y, COLORREF color);
    static void RenderAllMessages(IDirectDrawSurface7* surface);

public:
    // Initialize the hook
    static bool Initialize();
    
    // Try to initialize when the game is ready
    static bool TryInitializeOverlay();
    
    // Make isHooked visible for status checks
    static bool isHooked;
    
    // Add a temporary message
    static void AddMessage(const std::string& text, COLORREF color = RGB(255, 255, 0), 
                          int durationMs = 3000, int x = 10, int y = 10);
    
    // Add or update a permanent message
    static int AddPermanentMessage(const std::string& text, COLORREF color = RGB(255, 255, 0), 
                                 int x = 10, int y = 10);
    
    // Update an existing permanent message
    static void UpdatePermanentMessage(int id, const std::string& newText, COLORREF newColor = RGB(255, 255, 0));
    
    // Remove a permanent message
    static void RemovePermanentMessage(int id);
    
    // Remove all messages
    static void ClearAllMessages();

    // Add this method - it's missing but called in dllmain.cpp
    static void Shutdown();

    // Fallback method for when DirectDraw hook fails
    static bool InitializeFallbackOverlay();

    // Add this after the existing public methods
    static void TestOverlay();  // For testing if overlay works

    // Add this test method
    static void TestHelloWorld();

    // Add this simple overlay test method
    static bool InitializeSimpleOverlay();

    // Add this brute-force overlay method
    static bool InitializeBruteForceOverlay();
};

// Global status message IDs
extern int g_AirtechStatusId;
extern int g_JumpStatusId;