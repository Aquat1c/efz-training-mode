#include "../include/utilities.h"
#include "../include/constants.h"
#include "../include/logger.h"
#include "../include/memory.h"
#include "../include/input_handler.h"
#include "../include/di_keycodes.h"
#include "../include/frame_analysis.h"    // ADD THIS - for IsBlockstunState
#include "../include/frame_advantage.h"
#include "../include/config.h"
#include "../include/imgui_impl.h"
#include "../include/imgui_gui.h"
#include "../include/overlay.h"
#include "../include/input_handler.h"
#include "../include/auto_airtech.h"
#include "../include/auto_action.h"
#include "../include/frame_monitor.h"
#include <sstream>
#include <iomanip>
#include <iostream>  // Add this include for std::cout and std::cerr
#include <algorithm>  // For std::transform
#include <cwctype>    // For wide character functions
#include <locale>     // For std::locale
#include <fstream>
#include <iomanip>
#include <chrono>
#include <vector>
#include "../include/character_settings.h"

std::atomic<bool> g_efzWindowActive(false);
std::atomic<bool> g_guiActive(false);

// NEW: Define the manual input override atomics
std::atomic<bool> g_manualInputOverride[3] = {false, false, false};
std::atomic<uint8_t> g_manualInputMask[3] = {0, 0, 0};
std::atomic<bool> g_manualJumpHold[3] = {false, false, false}; // NEW: Definition for jump hold

// NEW: Add feature management functions
void EnableFeatures() {
    if (g_featuresEnabled.load())
        return;
        
    LogOut("[SYSTEM] Game in valid mode. Enabling patches and overlays.", true);

    // Apply patches if the feature is enabled
    if (autoAirtechEnabled.load()) {
        ApplyAirtechPatches();
    }

    g_featuresEnabled.store(true);
    
    // Only reinitialize overlays if characters are initialized and we're in a valid game mode
    if (DirectDrawHook::isHooked && AreCharactersInitialized()) {
        GameMode currentMode = GetCurrentGameMode();
        if (IsValidGameMode(currentMode)) {
            ReinitializeOverlays();
            if (g_statsDisplayEnabled.load()) {
                UpdateStatsDisplay();
            }
        } else {
            LogOut("[SYSTEM] Not initializing overlays - invalid game mode: " + 
                   GetGameModeName(currentMode), true);
        }
    }
    
    // Key monitoring will be handled separately by ManageKeyMonitoring()
}

void DisableFeatures() {
    if (!g_featuresEnabled.load())
        return;
    
    LogOut("[SYSTEM] Game left valid mode. Disabling patches and overlays.", true);

    // Stop key monitoring when leaving valid game mode
    if (keyMonitorRunning.load()) {
        LogOut("[SYSTEM] Stopping key monitoring due to invalid game mode.", true);
        keyMonitorRunning.store(false);
    }

    // Remove any active patches
    RemoveAirtechPatches();
    CharacterSettings::RemoveCharacterPatches(); // Remove character-specific patches

    // Clear ALL visual overlays
    DirectDrawHook::ClearAllMessages();
    
    // Reset stats display IDs since they've been cleared
    g_statsP1ValuesId = -1;
    g_statsP2ValuesId = -1;
    g_statsPositionId = -1;
    g_statsMoveIdId = -1;
    
    // Close the menu if it's open
    if (ImGuiImpl::IsVisible()) {
        ImGuiImpl::ToggleVisibility();
    }

    // Reset all core logic states
    ResetFrameAdvantageState();
    ResetActionFlags();
    p1DelayState = {false, 0, TRIGGER_NONE, 0};
    p2DelayState = {false, 0, TRIGGER_NONE, 0};

    g_featuresEnabled.store(false);
    
    // Key monitoring will be handled separately by ManageKeyMonitoring()
}


// Global flag to track if we're still in startup mode
std::atomic<bool> inStartupPhase(true);
std::string startupLogPath;

// Create a function that writes to a log file without requiring the console
void WriteStartupLog(const std::string& message) {
    if (!inStartupPhase) return; // Skip if we're past startup
    
    try {
        // Open log file in append mode
        if (startupLogPath.empty()) {
            char path[MAX_PATH] = {0};
            GetModuleFileNameA(NULL, path, MAX_PATH);
            std::string exePath(path);
            startupLogPath = exePath.substr(0, exePath.find_last_of("\\/")) + "\\efz_startup.log";
        }
        
        // Open file and append message with timestamp
        std::ofstream logFile(startupLogPath, std::ios::app);
        if (logFile.is_open()) {
            // Get current time
            auto now = std::chrono::system_clock::now();
            auto time = std::chrono::system_clock::to_time_t(now);
            
            // Convert to calendar time
            tm timeInfo;
            localtime_s(&timeInfo, &time);
            
            // Format timestamp: [HH:MM:SS.mmm]
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                now.time_since_epoch()) % 1000;
            
            char timeStr[20];
            std::strftime(timeStr, sizeof(timeStr), "%H:%M:%S", &timeInfo);
            
            // Write timestamped message
            logFile << "[" << timeStr << "." << std::setfill('0') << std::setw(3) << ms.count() 
                   << "] " << message << std::endl;
                   
            logFile.close();
        }
    }
    catch (...) {
        // Failsafe - we can't log the error anywhere reliable
    }
}

std::atomic<bool> menuOpen(false);
std::atomic<int> frameCounter(0);
std::atomic<bool> detailedLogging(false);
std::atomic<bool> autoAirtechEnabled(false);
std::atomic<int> autoAirtechDirection(0);  // 0=forward, 1=backward
std::atomic<bool> autoJumpEnabled(false);     // This was missing!
std::atomic<int> jumpDirection(0);            // 0=straight, 1=forward, 2=backward
std::atomic<bool> p1Jumping(false);
std::atomic<bool> p2Jumping(false);
std::atomic<int> jumpTarget(3);
std::atomic<bool> g_featuresEnabled(false); // Default to disabled
DisplayData displayData = {
    9999, 9999,         // hp1, hp2
    3000, 3000,         // meter1, meter2
    1000.0, 1000.0,     // rf1, rf2
    240.0, 0.0,         // x1, y1
    400.0, 0.0,         // x2, y2
    false,              // autoAirtech
    0,                  // airtechDirection
    0,                  // airtechDelay
    false,              // autoJump
    0,                  // jumpDirection
    3,                  // jumpTarget
    "",                 // p1CharName
    "",                 // p2CharName
    0,                  // p1CharID
    0,                  // p2CharID
    0, 0, 0, 0,         // Ikumi settings
    false,              // infiniteBloodMode
    0, 0,               // Misuzu settings
    false,              // infiniteFeatherMode
    false, false,       // Blue IC toggles
    false,              // NEW: p2ControlEnabled
    false,              // autoAction
    ACTION_5A,          // autoActionType
    200,                // autoActionCustomID
    0,                  // autoActionPlayer
    // ... rest of initialization
};

// Initialize key bindings with default values
KeyBindings detectedBindings = {
    INPUT_DEVICE_KEYBOARD, // inputDevice (default to keyboard)
    0,                     // gamepadIndex
    "Keyboard",            // deviceName
    VK_UP,                 // upKey (default to arrow keys)
    VK_DOWN,               // downKey
    VK_LEFT,               // leftKey
    VK_RIGHT,              // rightKey
    'Z',                   // aButton (common defaults)
    'X',                   // bButton
    'C',                   // cButton
    'A',                   // dButton
    false,                 // directionsDetected
    false                  // attacksDetected
};

// Add with other global variables
std::atomic<bool> g_statsDisplayEnabled(false);
int g_statsP1ValuesId = -1;
int g_statsP2ValuesId = -1;
int g_statsPositionId = -1;
int g_statsMoveIdId = -1;

// Auto-action settings - replace single trigger with individual triggers
std::atomic<bool> autoActionEnabled(false);
std::atomic<int> autoActionType(ACTION_5A);
std::atomic<int> autoActionCustomID(200); // Default to 5A
std::atomic<int> autoActionPlayer(2);     // Default to P2 (training dummy)

// Individual trigger settings
std::atomic<bool> triggerAfterBlockEnabled(false);
std::atomic<bool> triggerOnWakeupEnabled(false);
std::atomic<bool> triggerAfterHitstunEnabled(false);
std::atomic<bool> triggerAfterAirtechEnabled(false);

// Delay settings (in visual frames)
std::atomic<int> triggerAfterBlockDelay(DEFAULT_TRIGGER_DELAY);
std::atomic<int> triggerOnWakeupDelay(DEFAULT_TRIGGER_DELAY);
std::atomic<int> triggerAfterHitstunDelay(DEFAULT_TRIGGER_DELAY);
std::atomic<int> triggerAfterAirtechDelay(DEFAULT_TRIGGER_DELAY);

// Auto-airtech delay support
std::atomic<int> autoAirtechDelay(0); // Default to instant activation

// Individual action settings for each trigger
std::atomic<int> triggerAfterBlockAction(ACTION_5A);
std::atomic<int> triggerOnWakeupAction(ACTION_5A);
std::atomic<int> triggerAfterHitstunAction(ACTION_5A);
std::atomic<int> triggerAfterAirtechAction(ACTION_5A);

// Custom action IDs for each trigger
std::atomic<int> triggerAfterBlockCustomID(BASE_ATTACK_5A);
std::atomic<int> triggerOnWakeupCustomID(BASE_ATTACK_5A);
std::atomic<int> triggerAfterHitstunCustomID(BASE_ATTACK_5A);
std::atomic<int> triggerAfterAirtechCustomID(BASE_ATTACK_JA);  // Default to jumping A for airtech

void EnsureLocaleConsistency() {
    static bool localeSet = false;
    if (!localeSet) {
        std::locale::global(std::locale("C"));
        localeSet = true;
    }
}

std::string FormatPosition(double x, double y) {
    std::locale::global(std::locale("C")); 
    // This ensures consistent decimal point format
    std::stringstream ss;
    ss << std::fixed << std::setprecision(2) << "X=" << x << " Y=" << y;
    return ss.str();
}

uintptr_t GetEFZBase() {
    std::locale::global(std::locale("C")); 
    // This ensures consistent decimal point format
    return (uintptr_t)GetModuleHandleA(NULL);
}

// Add these helper functions to better detect state changes
bool IsActionable(short moveID) {
    // Character is actionable in these neutral states
    if (moveID == IDLE_MOVE_ID || 
        moveID == WALK_FWD_ID || 
        moveID == WALK_BACK_ID || 
        moveID == CROUCH_ID ||
        moveID == CROUCH_TO_STAND_ID ||
        moveID == LANDING_ID) {
        return true;
    }
    
    // NOT actionable during these states
    if (IsAttackMove(moveID) || 
        IsBlockstunState(moveID) || 
        IsHitstun(moveID) || 
        IsLaunched(moveID) ||
        IsAirtech(moveID) || 
        IsGroundtech(moveID) ||
        IsFrozen(moveID) ||
        moveID == STAND_GUARD_ID || 
        moveID == CROUCH_GUARD_ID || 
        moveID == AIR_GUARD_ID) {
        return false;
    }
    
    return true; // Default to actionable for unknown states
}

bool IsBlockstun(short moveID) {
    std::locale::global(std::locale("C")); 
    
    // Directly check for core blockstun IDs
    if (moveID == STAND_GUARD_ID || 
        moveID == CROUCH_GUARD_ID || 
        moveID == CROUCH_GUARD_STUN1 ||
        moveID == CROUCH_GUARD_STUN2 || 
        moveID == AIR_GUARD_ID) {
        return true;
    }
    
    // Check the range that includes standing blockstun states
    if (moveID == 150 || moveID == 152 || 
        (moveID >= 140 && moveID <= 149) ||
        (moveID >= 153 && moveID <= 165)) {
        return true;
    }
    
    return false;
}

bool IsRecoilGuard(short moveID) {
    std::locale::global(std::locale("C")); 
    // This ensures consistent decimal point format
    return moveID == RG_STAND_ID || moveID == RG_CROUCH_ID || moveID == RG_AIR_ID;
}

bool IsEFZWindowActive() {
    std::locale::global(std::locale("C")); 
    HWND fg = GetForegroundWindow();
    if (!fg)
        return false;
    
    // Try with Unicode API first
    WCHAR wideTitle[256] = { 0 };
    GetWindowTextW(fg, wideTitle, sizeof(wideTitle)/sizeof(WCHAR) - 1);
    
    // Case-insensitive comparison for wide strings
    if (_wcsicmp(wideTitle, L"ETERNAL FIGHTER ZERO") == 0 ||
        wcsstr(_wcslwr(wideTitle), L"efz.exe") != NULL ||
        wcsstr(_wcslwr(wideTitle), L"eternal fighter zero") != NULL ||
        wcsstr(_wcslwr(wideTitle), L"revival") != NULL) {
        return true;
    }
    
    // Fallback to ANSI for compatibility
    char title[256] = { 0 };
    GetWindowTextA(fg, title, sizeof(title) - 1);
    std::string t(title);
    std::transform(t.begin(), t.end(), t.begin(), ::toupper);
    
    return t.find("ETERNAL FIGHTER ZERO") != std::string::npos ||
           t.find("EFZ.EXE") != std::string::npos ||
           t.find("ETERNAL FIGHTER ZERO -REVIVAL-") != std::string::npos;
}

void CreateDebugConsole() {
    // Start diagnostic logging
    WriteStartupLog("CreateDebugConsole() started");
    WriteStartupLog("Current code page: " + std::to_string(GetConsoleOutputCP()));
    
    // Ensure C locale for consistency
    std::locale::global(std::locale("C"));
    WriteStartupLog("Locale set to C");
    
    // Create console and ensure success
    WriteStartupLog("Calling AllocConsole()...");
    if (!AllocConsole()) {
        DWORD lastError = GetLastError();
        WriteStartupLog("AllocConsole() failed with error code: " + std::to_string(lastError));
        
        // If AllocConsole fails, try attaching to parent console first
        WriteStartupLog("Attempting AttachConsole(ATTACH_PARENT_PROCESS)...");
        if (!AttachConsole(ATTACH_PARENT_PROCESS)) {
            lastError = GetLastError();
            WriteStartupLog("AttachConsole() failed with error code: " + std::to_string(lastError));
            MessageBoxA(NULL, "Failed to create debug console", "Error", MB_OK | MB_ICONERROR);
            return;
        }
        WriteStartupLog("AttachConsole() succeeded");
    } else {
        WriteStartupLog("AllocConsole() succeeded");
    }
    
    // Redirect stdout/stderr with error checking
    FILE* fp = nullptr;
    WriteStartupLog("Redirecting stdout to CONOUT$...");
    if (freopen_s(&fp, "CONOUT$", "w", stdout) != 0) {
        DWORD lastError = GetLastError();
        WriteStartupLog("stdout redirection failed with error code: " + std::to_string(lastError));
        MessageBoxA(NULL, "Failed to redirect stdout", "Error", MB_OK | MB_ICONERROR);
    } else {
        WriteStartupLog("stdout redirection succeeded");
    }
    
    WriteStartupLog("Redirecting stderr to CONOUT$...");
    if (freopen_s(&fp, "CONOUT$", "w", stderr) != 0) {
        DWORD lastError = GetLastError();
        WriteStartupLog("stderr redirection failed with error code: " + std::to_string(lastError));
        MessageBoxA(NULL, "Failed to redirect stderr", "Error", MB_OK | MB_ICONERROR);
    } else {
        WriteStartupLog("stderr redirection succeeded");
    }
    
    // Clear stream state
    std::cout.clear();
    std::cerr.clear();
    WriteStartupLog("Cleared stream state");
    
    // Set console title with Unicode
    WriteStartupLog("Setting console title...");
    bool titleSet = SetConsoleTitleW(L"EFZ Training Mode") != 0;
    WriteStartupLog("SetConsoleTitleW returned: " + std::to_string(titleSet));
    
    // Set console code page to UTF-8 for proper character display
    WriteStartupLog("Setting console code page to UTF-8...");
    bool cpSet = SetConsoleOutputCP(CP_UTF8) != 0;
    WriteStartupLog("SetConsoleOutputCP returned: " + std::to_string(cpSet));
    
    bool cpInSet = SetConsoleCP(CP_UTF8) != 0;
    WriteStartupLog("SetConsoleCP returned: " + std::to_string(cpInSet));
    
    // Get console handle
    WriteStartupLog("Getting console handle...");
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut == INVALID_HANDLE_VALUE) {
        DWORD lastError = GetLastError();
        WriteStartupLog("GetStdHandle failed with error: " + std::to_string(lastError));
    } else {
        WriteStartupLog("GetStdHandle succeeded");
        
        // Set console mode to enable virtual terminal processing (for ANSI colors)
        WriteStartupLog("Setting console mode...");
        DWORD dwMode = 0;
        if (!GetConsoleMode(hOut, &dwMode)) {
            DWORD lastError = GetLastError();
            WriteStartupLog("GetConsoleMode failed with error: " + std::to_string(lastError));
        } else {
            WriteStartupLog("Current console mode: " + std::to_string(dwMode));
            if (!SetConsoleMode(hOut, dwMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING | ENABLE_PROCESSED_OUTPUT)) {
                DWORD lastError = GetLastError();
                WriteStartupLog("SetConsoleMode failed with error: " + std::to_string(lastError));
            } else {
                WriteStartupLog("SetConsoleMode succeeded");
            }
        }
        
        // Set console buffer size for more history
        WriteStartupLog("Setting console buffer size...");
        CONSOLE_SCREEN_BUFFER_INFO csbi;
        if (!GetConsoleScreenBufferInfo(hOut, &csbi)) {
            DWORD lastError = GetLastError();
            WriteStartupLog("GetConsoleScreenBufferInfo failed with error: " + std::to_string(lastError));
        } else {
            COORD size = { csbi.dwSize.X, 2000 }; // Increase buffer height
            if (!SetConsoleScreenBufferSize(hOut, size)) {
                DWORD lastError = GetLastError();
                WriteStartupLog("SetConsoleScreenBufferSize failed with error: " + std::to_string(lastError));
            } else {
                WriteStartupLog("SetConsoleScreenBufferSize succeeded");
            }
        }
    }
    
    // Ensure console window is visible
    WriteStartupLog("Getting console window handle...");
    HWND consoleWindow = GetConsoleWindow();
    if (consoleWindow == NULL) {
        DWORD lastError = GetLastError();
        WriteStartupLog("GetConsoleWindow returned NULL, error: " + std::to_string(lastError));
    } else {
        WriteStartupLog("GetConsoleWindow succeeded, showing window...");
        ShowWindow(consoleWindow, SW_SHOW);
        WriteStartupLog("ShowWindow called");
    }
    
    // Test that console is working by writing directly to it
    WriteStartupLog("Testing console output...");
    try {
        std::cout << "Console initialization complete!" << std::endl;
        WriteStartupLog("Console test output successful");
    } catch (const std::exception& e) {
        WriteStartupLog("Console test output failed: " + std::string(e.what()));
    } catch (...) {
        WriteStartupLog("Console test output failed with unknown exception");
    }
    
    WriteStartupLog("CreateDebugConsole() completed");
}

void ResetFrameCounter() {
    frameCounter = 0;
    startFrameCount = 0;
    LogOut("[SYSTEM] Frame counter reset", true);
}

// REVISED: This function now opens the ImGui menu to the Help tab.
void ShowHotkeyInfo() {
    // If ImGui is enabled, open it to the help tab
    if (Config::GetSettings().useImGui) {
        if (!ImGuiImpl::IsVisible()) {
            ImGuiImpl::ToggleVisibility();
        }
        ImGuiGui::guiState.requestedTab = 2; // Request the Help tab
        LogOut("[GUI] Opening ImGui to Help tab", true);
    } else {
        // Fallback for legacy dialog
        LogOut("[GUI] ImGui not enabled, showing legacy hotkey dialog", true);
        MessageBoxA(NULL, "Hotkeys:\n\nMove: Arrow Keys\nAttack: A, S, D\nJump: W\nSpecial: Q, E\nPause: P\nToggle Debug: F1\nShow Frame Data: F2\nShow Hitboxes: F3\nShow HUD: F4\nShow Console: F5", "Hotkey Info", MB_OK | MB_ICONINFORMATION);
    }
}

std::string GetKeyName(int virtualKey) {
    // Handle special cases for clarity
    switch (virtualKey) {
        case VK_LEFT: return "Left Arrow";
        case VK_RIGHT: return "Right Arrow";
        case VK_UP: return "Up Arrow";
        case VK_DOWN: return "Down Arrow";
        case VK_RETURN: return "Enter";
        case VK_ESCAPE: return "Escape";
        case VK_SPACE: return "Space";
        case VK_LSHIFT: return "Left Shift";
        case VK_RSHIFT: return "Right Shift";
        case VK_LCONTROL: return "Left Ctrl";
        case VK_RCONTROL: return "Right Ctrl";
        case VK_LMENU: return "Left Alt";
        case VK_RMENU: return "Right Alt";
        case VK_TAB: return "Tab";
        case VK_CAPITAL: return "Caps Lock";
        case VK_BACK: return "Backspace";
        case VK_INSERT: return "Insert";
        case VK_DELETE: return "Delete";
        case VK_HOME: return "Home";
        case VK_END: return "End";
        case VK_PRIOR: return "Page Up";
        case VK_NEXT: return "Page Down";
    }

    // For F-keys and numbers/letters
    if (virtualKey >= VK_F1 && virtualKey <= VK_F24) {
        return "F" + std::to_string(virtualKey - VK_F1 + 1);
    }
    if (virtualKey >= '0' && virtualKey <= '9') {
        return std::string(1, (char)virtualKey);
    }
    if (virtualKey >= 'A' && virtualKey <= 'Z') {
        return std::string(1, (char)virtualKey);
    }

    // Fallback for other keys using system function
    char keyName[256];
    UINT scanCode = MapVirtualKey(virtualKey, MAPVK_VK_TO_VSC);
    if (GetKeyNameTextA(scanCode << 16, keyName, sizeof(keyName)) > 0) {
        return std::string(keyName);
    }

    return "Unknown Key";
}

bool IsDashState(short moveID) {
    std::locale::global(std::locale("C")); 
    // This ensures consistent decimal point format
    return moveID == FORWARD_DASH_START_ID || 
           moveID == FORWARD_DASH_RECOVERY_ID ||
           moveID == BACKWARD_DASH_START_ID || 
           moveID == BACKWARD_DASH_RECOVERY_ID;
}


// Add this function after the IsEFZWindowActive() function
HWND FindEFZWindow() {
    HWND foundWindow = NULL;
    static bool debugLogged = false;
    
    // Log debug info only once
    if (!debugLogged) {
        LogOut("[WINDOW] Searching for EFZ window...", true);
        debugLogged = true;
    }
    
    // Enumerate all windows to find EFZ
    EnumWindows([](HWND hwnd, LPARAM lParam) -> BOOL {
        HWND* result = reinterpret_cast<HWND*>(lParam);
        
        // Skip invisible windows
        if (!IsWindowVisible(hwnd)) {
            return TRUE; // Continue enumeration
        }
        
        // Get window title for debugging
        char title[256] = { 0 };
        GetWindowTextA(hwnd, title, sizeof(title) - 1);
        
        // Log visible windows (only log non-empty titles)
        if (strlen(title) > 0) {
            LogOut("[WINDOW] Found window: '" + std::string(title) + "'", true);
        }
        
        // Try with Unicode API first
        WCHAR wideTitle[256] = { 0 };
        GetWindowTextW(hwnd, wideTitle, sizeof(wideTitle)/sizeof(WCHAR) - 1);
        
        // Make a copy for case-insensitive comparison
        WCHAR wideTitleLower[256];
        wcscpy_s(wideTitleLower, wideTitle);
        _wcslwr_s(wideTitleLower);
        
        // Case-insensitive comparison for wide strings
        if (_wcsicmp(wideTitle, L"ETERNAL FIGHTER ZERO") == 0 ||
            wcsstr(wideTitleLower, L"efz.exe") != NULL ||
            wcsstr(wideTitleLower, L"eternal fighter zero") != NULL ||
            wcsstr(wideTitleLower, L"revival") != NULL) {
            
            LogOut("[WINDOW] Found EFZ window via Unicode: '" + std::string(title) + "'", true);
            *result = hwnd;
            return FALSE; // Stop enumeration
        }
        
        // Fallback to ANSI for compatibility
        std::string t(title);
        std::transform(t.begin(), t.end(), t.begin(), ::toupper);
        
        if (t.find("ETERNAL FIGHTER ZERO") != std::string::npos ||
            t.find("EFZ.EXE") != std::string::npos ||
            t.find("REVIVAL") != std::string::npos) {
            
            LogOut("[WINDOW] Found EFZ window via ANSI: '" + std::string(title) + "'", true);
            *result = hwnd;
            return FALSE; // Stop enumeration
        }
        
        return TRUE; // Continue enumeration
    }, reinterpret_cast<LPARAM>(&foundWindow));
    
    if (!foundWindow) {
        LogOut("[WINDOW] EFZ window not found", true);
    }
    
    return foundWindow;
}

void UpdateWindowActiveState() {
    HWND activeWindow = GetForegroundWindow();
    HWND efzWindow = FindEFZWindow();
    
    // Update EFZ window active state
    g_efzWindowActive.store(activeWindow == efzWindow);
    
    // Check if our GUI is active
    g_guiActive.store(menuOpen.load() || ImGuiImpl::IsVisible());
    
    // Log state changes only (not every update)
    static bool prevEfzActive = false;
    static bool prevGuiActive = false;
    
    if (prevEfzActive != g_efzWindowActive.load() || prevGuiActive != g_guiActive.load()) {
        LogOut("[WINDOW] EFZ window active: " + std::to_string(g_efzWindowActive.load()) + 
               ", GUI active: " + std::to_string(g_guiActive.load()), 
               detailedLogging.load());
        
        prevEfzActive = g_efzWindowActive.load();
        prevGuiActive = g_guiActive.load();
    }
}

// Separate function to manage key monitoring based on window focus
void ManageKeyMonitoring() {
    bool currentWindowActive = g_efzWindowActive.load();
    bool currentFeaturesEnabled = g_featuresEnabled.load();
    
    // Check if we should start key monitoring
    bool shouldMonitorKeys = currentWindowActive && currentFeaturesEnabled;
    bool isCurrentlyMonitoring = keyMonitorRunning.load();
    
    // Start key monitoring if we should be monitoring but aren't
    if (shouldMonitorKeys && !isCurrentlyMonitoring) {
        LogOut("[SYSTEM] Starting key monitoring - Window active: " + 
               std::to_string(currentWindowActive) + ", Features enabled: " + 
               std::to_string(currentFeaturesEnabled), true);
        RestartKeyMonitoring();
    }
    // Stop key monitoring if we shouldn't be monitoring but are
    else if (!shouldMonitorKeys && isCurrentlyMonitoring) {
        std::string reason = !currentFeaturesEnabled ? "features disabled" : "window inactive";
        LogOut("[SYSTEM] Stopping key monitoring - " + reason, true);
        keyMonitorRunning.store(false);
    }
}
