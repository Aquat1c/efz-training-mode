#pragma once
#include <windows.h>
#include <string>
#include <atomic>
#include <chrono>

// Global state variables
extern std::atomic<bool> menuOpen;
extern std::atomic<int> frameCounter;
extern std::atomic<bool> detailedLogging;

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

// Display data structure
struct DisplayData {
    WORD hp1 = 0, hp2 = 0, meter1 = 0, meter2 = 0;
    double rf1 = 0, rf2 = 0;
    double x1 = 0, y1 = 0, x2 = 0, y2 = 0;
};

extern DisplayData displayData;