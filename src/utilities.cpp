#include "../include/utilities.h"
#include "../include/constants.h"
#include "../include/logger.h"
#include <sstream>
#include <iomanip>
#include <iostream>  // Add this include for std::cout and std::cerr

std::atomic<bool> menuOpen(false);
std::atomic<int> frameCounter(0);
std::atomic<bool> detailedLogging(false);
DisplayData displayData;

std::string FormatPosition(double x, double y) {
    std::stringstream ss;
    ss << std::fixed << std::setprecision(2) << "X=" << x << " Y=" << y;
    return ss.str();
}

uintptr_t GetEFZBase() {
    return (uintptr_t)GetModuleHandleA(NULL);
}

bool IsActionable(short moveID) {
    return moveID == IDLE_MOVE_ID ||
        moveID == WALK_FWD_ID ||
        moveID == WALK_BACK_ID ||
        moveID == CROUCH_ID ||
        moveID == LANDING_ID ||
        moveID == CROUCH_TO_STAND_ID;
}

bool IsBlockstun(short moveID) {
    return moveID == STAND_GUARD_ID ||
        moveID == CROUCH_GUARD_ID ||
        moveID == CROUCH_GUARD_STUN1 ||
        moveID == CROUCH_GUARD_STUN2 ||
        moveID == AIR_GUARD_ID;
}

bool IsRecoilGuard(short moveID) {
    return moveID == RG_STAND_ID || moveID == RG_CROUCH_ID || moveID == RG_AIR_ID;
}

bool IsEFZWindowActive() {
    HWND fg = GetForegroundWindow();
    if (!fg)
        return false;
    
    char title[256] = { 0 };
    GetWindowTextA(fg, title, sizeof(title) - 1);
    std::string t(title);
    for (auto& c : t)
        c = toupper(c);
    
    return t.find("ETERNAL FIGHTER ZERO") != std::string::npos ||
        t.find("EFZ.EXE") != std::string::npos ||
        t.find("ETERNAL FIGHTER ZERO -REVIVAL-") != std::string::npos;
}

void CreateDebugConsole() {
    AllocConsole();
    FILE* fp;
    freopen_s(&fp, "CONOUT$", "w", stdout);
    freopen_s(&fp, "CONOUT$", "w", stderr);
    std::cout.clear();
    std::cerr.clear();
    SetConsoleTitleA("EFZ DLL Debug Console");
}

void ResetFrameCounter() {
    frameCounter.store(0);
    LogOut("[DLL] Frame counter reset to 0", true);
}

void ShowHotkeyInfo() {
    static bool shown = false;
    
    // Clear the console before showing the help
    system("cls");
    
    LogOut("\n--- HOTKEY INFORMATION ---", true);
    LogOut("1: Teleport players to recorded/default position", true);
    LogOut("  + Left Arrow: Teleport both players to left side", true);
    LogOut("  + Right Arrow: Teleport both players to right side", true);
    LogOut("  + Up Arrow: Swap P1 and P2 positions", true);
    LogOut("  + Down Arrow: Place players at round start positions", true);
    LogOut("2: Record current player positions", true);
    LogOut("3: Open config menu", true);
    LogOut("4: Toggle title display mode", true);
    LogOut("5: Reset frame counter", true);
    LogOut("6: Show this help and clear console", true);
    LogOut("-------------------------", true);
    shown = true;
}