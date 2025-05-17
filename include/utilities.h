#pragma once
#include <windows.h>
#include <string>
#include <atomic>
#include <chrono>

// Global state variables
extern std::atomic<bool> menuOpen;
extern std::atomic<int> frameCounter;
extern std::atomic<bool> detailedLogging;
extern std::atomic<bool> autoAirtechEnabled;  // New: Controls auto-airtech feature
extern std::atomic<int> autoAirtechDirection; // New: 0=forward, 1=backward
extern std::atomic<bool> autoJumpEnabled;     // Controls auto-jump feature
extern std::atomic<int> jumpDirection;        // 0=straight, 1=forward, 2=backward
extern std::atomic<bool> p1Jumping;           // Tracks if P1 is currently in jump state
extern std::atomic<bool> p2Jumping;           // Tracks if P2 is currently in jump state
extern std::atomic<int> jumpTarget;           // 1=P1, 2=P2, 3=Both
extern std::atomic<bool> inStartupPhase;      // Tracks if the application is in the startup phase

// Function declarations
uintptr_t GetEFZBase();
bool IsActionable(short moveID);
bool IsBlockstun(short moveID);
bool IsRecoilGuard(short moveID);
bool IsEFZWindowActive();
void CreateDebugConsole();
void ResetFrameCounter();
void ShowHotkeyInfo();
std::string FormatPosition(double x, double y);
bool IsHitstun(short moveID);
bool IsLaunched(short moveID);
bool IsAirtech(short moveID);
bool IsGroundtech(short moveID);
bool IsFrozen(short moveID);
bool IsSpecialStun(short moveID);
short GetUntechValue(uintptr_t base, int player);
void WriteStartupLog(const std::string& message); // Logs messages during the startup phase
std::string GetKeyName(int virtualKey);
void DetectKeyBindings();
bool IsDashState(short moveID); // New: Check if in dash state

// Display data structure
struct DisplayData {
    int hp1, hp2;
    int meter1, meter2;
    double rf1, rf2;
    double x1, y1;
    double x2, y2;
    bool autoAirtech;
    int airtechDirection;  // 0=forward, 1=backward
    bool autoJump;
    int jumpDirection;     // 0=straight, 1=forward, 2=backward
    int jumpTarget;        // 1=P1, 2=P2, 3=Both
};

extern DisplayData displayData;

// Structure to hold detected key bindings
struct KeyBindings {
    // Input device type
    int inputDevice;      // 0=keyboard, 1=gamepad
    int gamepadIndex;     // Which gamepad (for multiple controllers)
    std::string deviceName; // Name of the detected input device
    
    // P1 direction keys
    int upKey;
    int downKey;
    int leftKey;
    int rightKey;
    
    // P1 attack buttons
    int aButton;  // Light attack
    int bButton;  // Medium attack
    int cButton;  // Heavy attack
    int dButton;  // Special
    
    // Flags to track if bindings have been detected
    bool directionsDetected;
    bool attacksDetected;
};

extern KeyBindings detectedBindings;