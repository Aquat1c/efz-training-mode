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
extern std::atomic<bool> g_featuresEnabled;   // NEW: Master switch for all features

// Auto-action settings - replace the single trigger system with individual triggers
extern std::atomic<bool> autoActionEnabled;
extern std::atomic<int> autoActionType;
extern std::atomic<int> autoActionCustomID;
extern std::atomic<int> autoActionPlayer;  // 1=P1, 2=P2, 3=Both

// Individual trigger settings - ADD THESE MISSING DECLARATIONS
extern std::atomic<bool> triggerAfterBlockEnabled;
extern std::atomic<bool> triggerOnWakeupEnabled;
extern std::atomic<bool> triggerAfterHitstunEnabled;
extern std::atomic<bool> triggerAfterAirtechEnabled;

// Delay settings (in visual frames) - ADD THESE MISSING DECLARATIONS
extern std::atomic<int> triggerAfterBlockDelay;
extern std::atomic<int> triggerOnWakeupDelay;
extern std::atomic<int> triggerAfterHitstunDelay;
extern std::atomic<int> triggerAfterAirtechDelay;

// Function declarations
uintptr_t GetEFZBase();
bool IsActionable(short moveID);
bool IsBlockstun(short moveID);
bool IsRecoilGuard(short moveID);
bool IsEFZWindowActive();
HWND FindEFZWindow();
void CreateDebugConsole();
void DestroyDebugConsole(); // NEW: Free console and redirect handles
void SetConsoleVisibility(bool visible); // NEW: Show/Hide console window
void ResetFrameCounter();
void ShowHotkeyInfo();
std::string FormatPosition(double x, double y);
bool IsHitstun(short moveID);
bool IsLaunched(short moveID);
bool IsAirtech(short moveID);
bool IsGroundtech(short moveID);
bool IsFrozen(short moveID);
bool IsSpecialStun(short moveID);

// Explicitly clear all auto-action triggers (and auto-action) persistently.
// Use when returning to Character Select so user can re-enable manually later.
void ClearAllTriggersPersistently();

// ADD THESE MISSING FUNCTION DECLARATIONS
bool IsAttackMove(short moveID);      // From frame_advantage.cpp
bool IsBlockstunState(short moveID);  // From frame_analysis.cpp

short GetUntechValue(uintptr_t base, int player);
void WriteStartupLog(const std::string& message); // Logs messages during the startup phase
std::string GetKeyName(int virtualKey);
void DetectKeyBindings();
bool IsDashState(short moveID); // New: Check if in dash state
bool CanAirtech(short moveID); // Add this missing declaration

// NEW: Add feature management functions
extern std::atomic<bool> g_featuresEnabled;
void EnableFeatures();
void DisableFeatures();

// Add delay support for auto-airtech
extern std::atomic<int> autoAirtechDelay; // 0=instant, 1+=frames to wait

// For features that should inject only into immediate registers (skip buffer writes)
extern std::atomic<bool> g_injectImmediateOnly[3]; // Index 0 unused, 1=P1, 2=P2

// Display data structure
struct DisplayData {
    int hp1, hp2;
    int meter1, meter2;
    double rf1, rf2;
    double x1, y1;
    double x2, y2;
    bool autoAirtech;
    int airtechDirection;
    int airtechDelay;
    bool autoJump;
    int jumpDirection;
    int jumpTarget;
    
    // Add character name fields
    char p1CharName[16];  // Character name with buffer for null termination
    char p2CharName[16];  // Character name 
    
    // Add character ID fields
    int p1CharID;
    int p2CharID;
    
    // Character-specific settings
    // Ikumi
    int p1IkumiBlood;
    int p2IkumiBlood;
    int p1IkumiGenocide;
    int p2IkumiGenocide;
    bool infiniteBloodMode;  // Enables freeze patch for blood
    
    // Misuzu
    int p1MisuzuFeathers;
    int p2MisuzuFeathers;
    bool infiniteFeatherMode; // Add this missing field
    
    // Blue IC/Red IC toggle
    bool p1BlueIC;
    bool p2BlueIC;

    // NEW: Add this flag for P2 control
    bool p2ControlEnabled;
    
    // Keep these for backward compatibility
    bool autoAction;
    int autoActionType;
    int autoActionCustomID;
    int autoActionPlayer;
    
    // Individual trigger settings
    bool triggerAfterBlock;
    bool triggerOnWakeup;
    bool triggerAfterHitstun;
    bool triggerAfterAirtech;
    
    // Delay settings
    int delayAfterBlock;
    int delayOnWakeup;
    int delayAfterHitstun;
    int delayAfterAirtech;
    
    // Individual action settings for each trigger
    int actionAfterBlock;
    int actionOnWakeup;
    int actionAfterHitstun;
    int actionAfterAirtech;
    
    // Custom action IDs for each trigger
    int customAfterBlock;
    int customOnWakeup;
    int customAfterHitstun;
    int customAfterAirtech;

    // Add strength settings for each trigger
    int strengthAfterBlock;
    int strengthOnWakeup;
    int strengthAfterHitstun;
    int strengthAfterAirtech;
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

// Individual action settings for each trigger
extern std::atomic<int> triggerAfterBlockAction;
extern std::atomic<int> triggerOnWakeupAction;
extern std::atomic<int> triggerAfterHitstunAction;
extern std::atomic<int> triggerAfterAirtechAction;

// Custom action IDs for each trigger
extern std::atomic<int> triggerAfterBlockCustomID;
extern std::atomic<int> triggerOnWakeupCustomID;
extern std::atomic<int> triggerAfterHitstunCustomID;
extern std::atomic<int> triggerAfterAirtechCustomID;

// Add a missing constant that utilities.cpp needs
#define DEFAULT_TRIGGER_DELAY 0

// Add these after the other global state variables
extern std::atomic<bool> g_efzWindowActive;
extern std::atomic<bool> g_guiActive;

// NEW: Add these for the debug tab's manual input override feature
extern std::atomic<bool> g_manualInputOverride[3]; // Index 0 unused, 1 for P1, 2 for P2
extern std::atomic<uint8_t> g_manualInputMask[3];
extern std::atomic<bool> g_manualJumpHold[3]; // NEW: For continuous jump on hold

// Add this function declaration
void UpdateWindowActiveState();

// Add these after the other global state variables
extern std::atomic<bool> g_statsDisplayEnabled;
extern int g_statsP1ValuesId;
extern int g_statsP2ValuesId;
extern int g_statsPositionId;
extern int g_statsMoveIdId;

// Window and key monitoring management
void ManageKeyMonitoring();

// Individual strength settings (0=A, 1=B, 2=C) - ADD THESE DECLARATIONS
extern std::atomic<int> triggerAfterBlockStrength;
extern std::atomic<int> triggerOnWakeupStrength;
extern std::atomic<int> triggerAfterHitstunStrength;
extern std::atomic<int> triggerAfterAirtechStrength;

// Attack data structure - NEW
struct AttackData {
    // Offset 0x38: Attack type flags (likely contains high/low data)
    int attackType;  
    
    // Offset 0x3C: Active frames
    short activeFrameStart;
    short activeFrameEnd;
    
    // Other properties...
    int damage;
    int blockstun;
    int hitstun;
};