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

std::mutex g_logMutex;
std::atomic<bool> detailedTitleMode(false);

void LogOut(const std::string& msg, bool consoleOutput) {
    // Only output to console if requested
    if (consoleOutput) {
        std::lock_guard<std::mutex> lock(g_logMutex);
        
        // Track message categories for proper spacing
        static std::string lastCategory = "";
        std::string currentCategory = "OTHER"; // Default
        
        // Extract the message category from inside first []
        size_t startBracket = msg.find('[');
        size_t endBracket = msg.find(']', startBracket);
        
        // Handle special case - group RG FRAME ADVANTAGE with FRAME ADVANTAGE
        /*if (startBracket != std::string::npos && endBracket != std::string::npos) {
            currentCategory = msg.substr(startBracket + 1, endBracket - startBracket - 1);
            
            if (currentCategory == "RG FRAME ADVANTAGE") {
                currentCategory = "FRAME ADVANTAGE";
            }
        }*/
        
        // Add spacing based on message category
        if (!lastCategory.empty()) {
            if (currentCategory != lastCategory) {
                // Different categories - add two newlines
                std::cout << std::endl << std::endl;
            } else {
                // Same category - add one newline
                std::cout << std::endl;
            }
        }
        
        // Output the message and update category tracking
        std::cout << msg << std::endl;
        lastCategory = currentCategory;
    }
}

void InitializeLogging() {
    // Nothing to initialize now that file logging is removed
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
        
        if (detailedTitleMode) {
            // Detailed mode - show frame counters and move IDs
            sprintf_s(title, 
                "EFZ DLL | Frame: %d (Visual: %.1f) | P1 Move: %d | P2 Move: %d | Detailed: %s",
                frameCounter.load(),
                frameCounter.load() / SUBFRAMES_PER_VISUAL_FRAME,
                GetCurrentMoveID(1),
                GetCurrentMoveID(2),
                detailedLogging.load() ? "ON" : "OFF");
        } else {
            // Normal mode - show player stats
            sprintf_s(title,
                "EFZ DLL | P1 HP:%d Meter:%d RF:%.1f X:%.1f Y:%.1f | P2 HP:%d Meter:%d RF:%.1f X:%.1f Y:%.1f",
                displayData.hp1, displayData.meter1, displayData.rf1, displayData.x1, displayData.y1,
                displayData.hp2, displayData.meter2, displayData.rf2, displayData.x2, displayData.y2);
        }
        
        SetConsoleTitleA(title);
        Sleep(100);
    }
}