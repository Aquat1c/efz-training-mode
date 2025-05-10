#include "../include/utilities.h"
#include "../include/gui.h"
#include "../include/logger.h"
#include "../include/memory.h"
#include "../include/constants.h"
#include "../include/network.h" // Add at top with other includes
#include <windows.h>

void MonitorKeys() {
    double recordedX1 = 0.0, recordedY1 = 0.0;
    double recordedX2 = 0.0, recordedY2 = 0.0;
    bool hasRecorded = false;
    
    // Constants for teleport positions
    const double leftX = 43.6548, rightX = 595.425, teleportY = 0.0;
    const double p1StartX = 240.0, p2StartX = 400.0, startY = 0.0;

    ShowHotkeyInfo();
    
    while (true) {
        // *** Add this check at the beginning of the loop ***
        if (isOnlineMatch.load()) {
            // Skip all keybind processing during online matches
            Sleep(100); // Reduced polling when online
            continue;
        }
        
        // Only process hotkeys when EFZ window is active
        if (IsEFZWindowActive()) {
            uintptr_t base = GetEFZBase();
            
            if (base) {
                // Position recording with '2'
                if (GetAsyncKeyState('2') & 1) {
                    uintptr_t xAddr1 = ResolvePointer(base, EFZ_BASE_OFFSET_P1, XPOS_OFFSET);
                    uintptr_t yAddr1 = ResolvePointer(base, EFZ_BASE_OFFSET_P1, YPOS_OFFSET);
                    uintptr_t xAddr2 = ResolvePointer(base, EFZ_BASE_OFFSET_P2, XPOS_OFFSET);
                    uintptr_t yAddr2 = ResolvePointer(base, EFZ_BASE_OFFSET_P2, YPOS_OFFSET);
                    
                    if (xAddr1 && yAddr1 && xAddr2 && yAddr2) {
                        memcpy(&recordedX1, (void*)xAddr1, sizeof(double));
                        memcpy(&recordedY1, (void*)yAddr1, sizeof(double));
                        memcpy(&recordedX2, (void*)xAddr2, sizeof(double));
                        memcpy(&recordedY2, (void*)yAddr2, sizeof(double));
                        hasRecorded = true;
                        LogOut("[DLL] P1 Position recorded: " + FormatPosition(recordedX1, recordedY1), true);
                        LogOut("[DLL] P2 Position recorded: " + FormatPosition(recordedX2, recordedY2), true);
                    }
                }
                
                // Position recall/teleport with '1'
                if (GetAsyncKeyState('1') & 1) {
                    if (GetAsyncKeyState(VK_LEFT) & 0x8000) {
                        // Teleport both to left
                        SetPlayerPosition(base, EFZ_BASE_OFFSET_P1, leftX, teleportY);
                        SetPlayerPosition(base, EFZ_BASE_OFFSET_P2, leftX, teleportY);
                        LogOut("[DLL] Both players teleported to LEFT: " + FormatPosition(leftX, teleportY), true);
                    }
                    else if (GetAsyncKeyState(VK_RIGHT) & 0x8000) {
                        // Teleport both to right
                        SetPlayerPosition(base, EFZ_BASE_OFFSET_P1, rightX, teleportY);
                        SetPlayerPosition(base, EFZ_BASE_OFFSET_P2, rightX, teleportY);
                        LogOut("[DLL] Both players teleported to RIGHT: " + FormatPosition(rightX, teleportY), true);
                    }
                    else if (GetAsyncKeyState(VK_UP) & 0x8000) {
                        // Swap P1 and P2 positions
                        double tempX1, tempY1, tempX2, tempY2;
                        uintptr_t xAddr1 = ResolvePointer(base, EFZ_BASE_OFFSET_P1, XPOS_OFFSET);
                        uintptr_t yAddr1 = ResolvePointer(base, EFZ_BASE_OFFSET_P1, YPOS_OFFSET);
                        uintptr_t xAddr2 = ResolvePointer(base, EFZ_BASE_OFFSET_P2, XPOS_OFFSET);
                        uintptr_t yAddr2 = ResolvePointer(base, EFZ_BASE_OFFSET_P2, YPOS_OFFSET);
                        
                        if (xAddr1 && yAddr1 && xAddr2 && yAddr2) {
                            // Read current positions
                            memcpy(&tempX1, (void*)xAddr1, sizeof(double));
                            memcpy(&tempY1, (void*)yAddr1, sizeof(double));
                            memcpy(&tempX2, (void*)xAddr2, sizeof(double));
                            memcpy(&tempY2, (void*)yAddr2, sizeof(double));
                            
                            // Swap positions
                            SetPlayerPosition(base, EFZ_BASE_OFFSET_P1, tempX2, tempY2);
                            SetPlayerPosition(base, EFZ_BASE_OFFSET_P2, tempX1, tempY1);
                            
                            LogOut("[DLL] Swapped player positions: P1=" + FormatPosition(tempX2, tempY2) +
                                ", P2=" + FormatPosition(tempX1, tempY1), true);
                        }
                    }
                    else if (GetAsyncKeyState(VK_DOWN) & 0x8000) {
                        // Set round start positions
                        SetPlayerPosition(base, EFZ_BASE_OFFSET_P1, p1StartX, startY);
                        SetPlayerPosition(base, EFZ_BASE_OFFSET_P2, p2StartX, startY);
                        LogOut("[DLL] Set round start positions: P1=" + FormatPosition(p1StartX, startY) +
                            ", P2=" + FormatPosition(p2StartX, startY), true);
                    }
                    else {
                        // Default teleport to recorded positions
                        double x1 = hasRecorded ? recordedX1 : p1StartX;
                        double y1 = hasRecorded ? recordedY1 : startY;
                        double x2 = hasRecorded ? recordedX2 : p2StartX;
                        double y2 = hasRecorded ? recordedY2 : startY;
                        SetPlayerPosition(base, EFZ_BASE_OFFSET_P1, x1, y1);
                        SetPlayerPosition(base, EFZ_BASE_OFFSET_P2, x2, y2);
                        LogOut("[DLL] P1 moved to " + FormatPosition(x1, y1), true);
                        LogOut("[DLL] P2 moved to " + FormatPosition(x2, y2), true);
                    }
                    Sleep(200); // Prevent key repeat
                }
                
                // Open settings menu with '3'
                if (GetAsyncKeyState('3') & 1) {
                    if (!menuOpen) {
                        OpenMenu();
                    }
                    Sleep(200);
                }
                
                // Toggle title display mode with '4'
                if (GetAsyncKeyState('4') & 1) {
                    detailedTitleMode = !detailedTitleMode;
                    LogOut("[DLL] Title display mode " + std::string(detailedTitleMode ? "detailed" : "standard"), true);
                    Sleep(200);
                }
                
                // Reset frame counter with '5'
                if (GetAsyncKeyState('5') & 1) {
                    ResetFrameCounter();
                    Sleep(200);
                }
                
                // Show help message with '6'
                if (GetAsyncKeyState('6') & 1) {
                    ShowHotkeyInfo();
                    Sleep(200);
                }
            }
            
            // Legacy hotkeys (kept for backward compatibility)
            if (GetAsyncKeyState(VK_F9) & 0x8000) {
                if (!menuOpen) {
                    OpenMenu();
                }
                Sleep(200);
            }
            
            if (GetAsyncKeyState(VK_F10) & 0x8000) {
                ResetFrameCounter();
                Sleep(200);
            }
            
            if (GetAsyncKeyState(VK_F11) & 0x8000) {
                detailedLogging = !detailedLogging;
                LogOut("[INPUT] Detailed logging " + std::string(detailedLogging ? "enabled" : "disabled"), true);
                Sleep(200);
            }
        }
        
        // Sleep to reduce CPU usage
        Sleep(50);
    }
}