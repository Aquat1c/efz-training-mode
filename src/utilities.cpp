#include "../include/utilities.h"
#include "../include/constants.h"
#include "../include/logger.h"
#include "../include/memory.h"
#include "../include/input_handler.h"
#include "../include/di_keycodes.h"
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
std::atomic<bool> autoJumpEnabled(false);
std::atomic<int> jumpDirection(0);      // 0=straight, 1=forward, 2=backward
std::atomic<bool> p1Jumping(false);
std::atomic<bool> p2Jumping(false);
std::atomic<int> jumpTarget(3);         // Default to both players
DisplayData displayData;

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

// Add with other globals

// Auto-action settings
std::atomic<bool> autoActionEnabled(false);
std::atomic<int> autoActionTrigger(TRIGGER_AFTER_BLOCK);
std::atomic<int> autoActionType(ACTION_5A);
std::atomic<int> autoActionCustomID(200); // Default to 5A
std::atomic<int> autoActionPlayer(2);     // Default to P2 (training dummy)

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
    std::locale::global(std::locale("C")); 
    bool actionable = moveID == IDLE_MOVE_ID ||
        moveID == WALK_FWD_ID ||
        moveID == WALK_BACK_ID ||
        moveID == CROUCH_ID ||
        moveID == LANDING_ID ||
        moveID == CROUCH_TO_STAND_ID;
    
    // Debug logging for actionable state detection
    if (detailedLogging.load() && actionable) {
        LogOut("[STATE] MoveID " + std::to_string(moveID) + " is actionable", true);
    }
    
    return actionable;
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
    std::locale::global(std::locale("C")); 
    // This ensures consistent decimal point format
    frameCounter.store(0);
    LogOut("[DLL] Frame counter reset to 0", true);
}

void ShowHotkeyInfo() {
    std::locale::global(std::locale("C"));
    
    // Clear the console before showing the help
    system("cls");
    
    // Get key/button names based on the detected input device
    std::string leftKey = "←", rightKey = "→", upKey = "↑", downKey = "↓", aKey = "A";
    
    // Only use detected values if they've been found
    if (detectedBindings.directionsDetected) {
        leftKey = GetKeyName(detectedBindings.leftKey);
        rightKey = GetKeyName(detectedBindings.rightKey);
        upKey = GetKeyName(detectedBindings.upKey);
        downKey = GetKeyName(detectedBindings.downKey);
        
        if (detectedBindings.attacksDetected) {
            aKey = GetKeyName(detectedBindings.aButton);
        }
    }
    
    LogOut("\n--- EFZ TRAINING MODE CONTROLS ---", true);
    
    // Show the detected input device if available
    if (!detectedBindings.deviceName.empty()) {
        LogOut("Detected input device: " + detectedBindings.deviceName, true);
    }
    
    LogOut("\nKey Combinations:", true);
    LogOut("1: Teleport players to recorded/default position", true);
    LogOut("  + 1+" + leftKey + " (P1's Left): Teleport both players to left side", true);
    LogOut("  + 1+" + rightKey + " (P1's Right): Teleport both players to right side", true);
    // FIX: Correct these two lines to match the actual functionality
    LogOut("  + 1+" + upKey + " (P1's Up): Swap P1 and P2 positions", true);
    LogOut("  + 1+" + downKey + " (P1's Down): Place players close together at center", true);
    LogOut("  + 1+" + downKey + "+" + aKey + " (P1's Down + Light Attack): Place players at round start positions", true);
    LogOut("2: Record current player positions", true);
    LogOut("3: Open config menu", true);
    LogOut("4: Toggle title display mode", true);
    LogOut("5: Reset frame counter", true);
    LogOut("6: Show this help and clear console", true);
    LogOut("T: Test overlay system (Hello, world)", true);
    LogOut("", true);
    
    // Show message about detected keys
    if (!detectedBindings.directionsDetected || !detectedBindings.attacksDetected) {
        LogOut("NOTE: Some input bindings haven't been detected yet.", true);
        LogOut("      Play the game for a while to improve detection.", true);
    } else {
        LogOut("NOTE: Input bindings have been detected for your controls.", true);
        
        if (detectedBindings.inputDevice == INPUT_DEVICE_GAMEPAD) {
            LogOut("      Using " + detectedBindings.deviceName + " (Gamepad " + 
                   std::to_string(detectedBindings.gamepadIndex + 1) + ")", true);
        } else {
            LogOut("      Using keyboard controls", true);
        }
    }
    
    LogOut("-------------------------", true);
}

// Add this function

std::string GetKeyName(int virtualKey) {
    unsigned int scanCode = MapVirtualKey(virtualKey, MAPVK_VK_TO_VSC);
    
    // Handle extended keys
    switch (virtualKey) {
        case VK_LEFT: case VK_UP: case VK_RIGHT: case VK_DOWN:
        case VK_PRIOR: case VK_NEXT: case VK_END: case VK_HOME:
        case VK_INSERT: case VK_DELETE:
        case VK_DIVIDE:
            scanCode |= 0x100;
            break;
    }

    // Special cases for keys that need different names
    switch (virtualKey) {
        case VK_LEFT:     return "←";
        case VK_UP:       return "↑";
        case VK_RIGHT:    return "→";
        case VK_DOWN:     return "↓";
        case VK_RETURN:   return "Enter";
        case VK_ESCAPE:   return "Esc";
        case VK_SPACE:    return "Space";
        case VK_LSHIFT:   return "L-Shift";
        case VK_RSHIFT:   return "R-Shift";
        case VK_LCONTROL: return "L-Ctrl";
        case VK_RCONTROL: return "R-Ctrl";
        case VK_LMENU:    return "L-Alt";
        case VK_RMENU:    return "R-Alt";
    }
    
    // Get key name from system
    char keyName[64];
    if (GetKeyNameTextA(scanCode << 16, keyName, sizeof(keyName)) > 0) {
        return keyName;
    }
    
    // If all else fails, return the virtual key code
    if (virtualKey >= 'A' && virtualKey <= 'Z') {
        char c = static_cast<char>(virtualKey);
        return std::string(1, c);
    }
    
    return "Key(" + std::to_string(virtualKey) + ")";
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

