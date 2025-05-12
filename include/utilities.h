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

// Display data structure
struct DisplayData {
    WORD hp1 = 0, hp2 = 0, meter1 = 0, meter2 = 0;
    double rf1 = 0, rf2 = 0;
    double x1 = 0, y1 = 0, x2 = 0, y2 = 0;
    bool autoAirtech = false;    // New: Added to DisplayData
    int airtechDirection = 0;    // New: Added to DisplayData 
};

extern DisplayData displayData;