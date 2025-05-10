#include "../include/logger.h"
#include "../include/utilities.h"
#include "../include/memory.h"
#include "../include/constants.h"
#include <iostream>
#include <sstream>
#include <chrono>
#include <iomanip>
#include <windows.h>
#include <atomic>
#include <thread>

std::mutex g_logMutex;
std::atomic<bool> detailedTitleMode(false);

void LogOut(const std::string& msg, bool consoleOutput) {
    // Only output to console if requested
    if (consoleOutput) {
        std::lock_guard<std::mutex> lock(g_logMutex);
        
        // Track message categories for proper spacing
        static std::string lastCategory = "";
        static bool inHotkeyBlock = false;
        std::string currentCategory = "OTHER"; // Default
        
        // Check if we're entering the hotkey information block
        if (msg.find("--- HOTKEY INFORMATION ---") != std::string::npos) {
            inHotkeyBlock = true;
        }
        // Check if we're exiting the hotkey block (at the final divider)
        else if (msg.find("-------------------------") != std::string::npos && inHotkeyBlock) {
            inHotkeyBlock = false;
        }
        
        // Extract the message category from inside first []
        size_t startBracket = msg.find('[');
        size_t endBracket = msg.find(']', startBracket);
        
        if (startBracket != std::string::npos && endBracket != std::string::npos) {
            currentCategory = msg.substr(startBracket + 1, endBracket - startBracket - 1);
            
            // Handle special case - group RG FRAME ADVANTAGE with FRAME ADVANTAGE
            if (currentCategory == "RG FRAME ADVANTAGE") {
                currentCategory = "FRAME ADVANTAGE";
            }
        }
        
        // Don't add spacing for hotkey info or help messages
        bool isHotkeyInfo = inHotkeyBlock || 
                           (msg.find("Key") != std::string::npos && msg.find(":") != std::string::npos) || 
                           msg.find("Press") != std::string::npos ||
                           msg.find("HELP") != std::string::npos;
        
        // Add spacing based on message category, but not for hotkey info
        if (!lastCategory.empty() && !isHotkeyInfo) {
            if (currentCategory != lastCategory && lastCategory != "HELP" && currentCategory != "HELP") {
                // Different categories - add two newlines
                std::cout << std::endl << std::endl;
            } else if (lastCategory != "HELP" && currentCategory != "HELP") {
                // Same category - add one newline
                std::cout << std::endl;
            }
        }
        
        // Output the message
        std::cout << msg << std::endl;
        
        // Only update the last category if it's not a hotkey message
        if (!isHotkeyInfo) {
            lastCategory = currentCategory;
        }
    }
}

void InitializeLogging() {
    // Create a thread to continuously update the console title
    std::thread titleThread([]() {
        UpdateConsoleTitle();
    });
    titleThread.detach();  // Let it run independently

    LogOut("EFZ DLL started", true);
}

short GetCurrentMoveID(int player) {
    uintptr_t base = GetEFZBase();
    if (base == 0) return 0;
    
    uintptr_t baseOffset = (player == 1) ? EFZ_BASE_OFFSET_P1 : EFZ_BASE_OFFSET_P2;
    uintptr_t moveIDAddr = ResolvePointer(base, baseOffset, MOVE_ID_OFFSET);
    
    short moveID = 0;
    if (moveIDAddr) memcpy(&moveID, (void*)moveIDAddr, sizeof(short));
    return moveID;
}

void UpdateConsoleTitle() {
    while (true) {
        // Check if the console window still exists
        if (GetConsoleWindow() == nullptr) {
            // Console window was closed, exit thread
            break;
        }

        char title[256];
        uintptr_t base = GetEFZBase();
        
        // Refresh display data from memory before updating title
        if (base != 0) {
            // Read P1 data
            uintptr_t hpAddr1 = ResolvePointer(base, EFZ_BASE_OFFSET_P1, HP_OFFSET);
            uintptr_t meterAddr1 = ResolvePointer(base, EFZ_BASE_OFFSET_P1, METER_OFFSET);
            uintptr_t rfAddr1 = ResolvePointer(base, EFZ_BASE_OFFSET_P1, RF_OFFSET);
            uintptr_t xAddr1 = ResolvePointer(base, EFZ_BASE_OFFSET_P1, XPOS_OFFSET);
            uintptr_t yAddr1 = ResolvePointer(base, EFZ_BASE_OFFSET_P1, YPOS_OFFSET);
            
            // Read P2 data
            uintptr_t hpAddr2 = ResolvePointer(base, EFZ_BASE_OFFSET_P2, HP_OFFSET);
            uintptr_t meterAddr2 = ResolvePointer(base, EFZ_BASE_OFFSET_P2, METER_OFFSET);
            uintptr_t rfAddr2 = ResolvePointer(base, EFZ_BASE_OFFSET_P2, RF_OFFSET);
            uintptr_t xAddr2 = ResolvePointer(base, EFZ_BASE_OFFSET_P2, XPOS_OFFSET);
            uintptr_t yAddr2 = ResolvePointer(base, EFZ_BASE_OFFSET_P2, YPOS_OFFSET);
            
            // Update the display data structure
            if (hpAddr1) memcpy(&displayData.hp1, (void*)hpAddr1, sizeof(WORD));
            if (meterAddr1) memcpy(&displayData.meter1, (void*)meterAddr1, sizeof(WORD));
            if (rfAddr1) {
                // Read RF value as a double with error checking
                double rf = 0.0;
                memcpy(&rf, (void*)rfAddr1, sizeof(double));
                
                // Validate RF value (should be between 0 and 1000)
                if (rf >= 0.0 && rf <= MAX_RF) {
                    displayData.rf1 = rf;
                } else {
                    // If value is out of valid range, set to a reasonable default
                    displayData.rf1 = 0.0;
                }
            }
            if (xAddr1) memcpy(&displayData.x1, (void*)xAddr1, sizeof(double));
            if (yAddr1) memcpy(&displayData.y1, (void*)yAddr1, sizeof(double));
            
            if (hpAddr2) memcpy(&displayData.hp2, (void*)hpAddr2, sizeof(WORD));
            if (meterAddr2) memcpy(&displayData.meter2, (void*)meterAddr2, sizeof(WORD));
            if (rfAddr2) {
                double rf = 0.0;
                memcpy(&rf, (void*)rfAddr2, sizeof(double));
                
                if (rf >= 0.0 && rf <= MAX_RF) {
                    displayData.rf2 = rf;
                } else {
                    displayData.rf2 = 0.0;
                }
            }
            if (xAddr2) memcpy(&displayData.x2, (void*)xAddr2, sizeof(double));
            if (yAddr2) memcpy(&displayData.y2, (void*)yAddr2, sizeof(double));
        }
        
        // Check if we can access game data or if all values are default/zero
        bool gameActive = base != 0;
        bool allZeros = displayData.hp1 == 0 && displayData.hp2 == 0 && 
                       displayData.meter1 == 0 && displayData.meter2 == 0 &&
                       displayData.x1 == 0 && displayData.y1 == 0;
        
        if (!gameActive || allZeros) {
            // Game not fully loaded or initialized yet
            sprintf_s(title, 
                "EFZ Training Mode | Waiting for match to start... | Press 3 to open config menu | Press 6 for help");
        } 
        else if (detailedTitleMode) {
            // Detailed mode - show frame counters and move IDs
            sprintf_s(title, 
                "EFZ DLL | Frame: %d (Visual: %.1f) | P1 Move: %d | P2 Move: %d | Detailed: %s",
                frameCounter.load(),
                frameCounter.load() / SUBFRAMES_PER_VISUAL_FRAME,
                GetCurrentMoveID(1),
                GetCurrentMoveID(2),
                detailedLogging.load() ? "ON" : "OFF");
        } 
        else {
            // Normal mode - show player stats
            sprintf_s(title,
                "EFZ DLL | P1 HP:%d Meter:%d RF:%.1f X:%.1f Y:%.1f | P2 HP:%d Meter:%d RF:%.1f X:%.1f Y:%.1f",
                displayData.hp1, displayData.meter1, displayData.rf1, displayData.x1, displayData.y1,
                displayData.hp2, displayData.meter2, displayData.rf2, displayData.x2, displayData.y2);
        }
        
        SetConsoleTitleA(title);
        
        // Update at 64Hz to match EFZ's update frequency
        Sleep(15);  // ~64 updates per second
    }
}