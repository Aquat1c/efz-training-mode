#include <fstream>
#include <iomanip>
#include <chrono>
#include "../include/logger.h"
#include "../include/utilities.h"
#include "../include/memory.h"
#include "../include/constants.h"
#include <iostream>
#include <sstream> // Required for std::ostringstream
#include <chrono>
#include <iomanip>
#include <windows.h>
#include <atomic>
#include <thread>

std::mutex g_logMutex;
std::atomic<bool> detailedTitleMode(false);
std::atomic<bool> detailedDebugOutput(false);

// NEW: Definition for Logger::hwndToString
namespace Logger {
    std::string hwndToString(HWND hwnd) {
        if (hwnd == NULL) {
            return "NULL";
        }
        std::ostringstream oss;
        oss << hwnd;
        return oss.str();
    }
}

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
        
        // Skip certain debug messages unless detailed debug is enabled
        bool isDetailedDebugMsg = 
            currentCategory == "POSITION" || 
            currentCategory == "HITSTUN" || 
            currentCategory == "STATE";
            
        // Always show frame advantage regardless of detailed debug setting
        if (isDetailedDebugMsg && !detailedDebugOutput && 
            currentCategory != "FRAME ADVANTAGE") { // Special case for frame advantage
            // Skip this message - detailed debug not enabled
            return;
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
    // Keep this thread at normal priority since you want it to keep up with the game
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);
    
    while (true) {
        // Check if the console window still exists
        if (GetConsoleWindow() == nullptr) {
            break;
        }

        char title[512];
        uintptr_t base = GetEFZBase();
        
        // Keep the fast update rate as requested - every 250ms
        if (base != 0) {
            // Cache memory addresses to avoid repeated ResolvePointer calls
            static uintptr_t cachedAddresses[12] = {0};
            static int titleCacheCounter = 0;
            
            // Refresh cached addresses every 20 iterations (5 seconds)
            if (titleCacheCounter++ >= 20) {
                cachedAddresses[0] = ResolvePointer(base, EFZ_BASE_OFFSET_P1, HP_OFFSET);
                cachedAddresses[1] = ResolvePointer(base, EFZ_BASE_OFFSET_P1, METER_OFFSET);
                cachedAddresses[2] = ResolvePointer(base, EFZ_BASE_OFFSET_P1, RF_OFFSET);
                cachedAddresses[3] = ResolvePointer(base, EFZ_BASE_OFFSET_P1, XPOS_OFFSET);
                cachedAddresses[4] = ResolvePointer(base, EFZ_BASE_OFFSET_P1, YPOS_OFFSET);
                cachedAddresses[5] = ResolvePointer(base, EFZ_BASE_OFFSET_P2, HP_OFFSET);
                cachedAddresses[6] = ResolvePointer(base, EFZ_BASE_OFFSET_P2, METER_OFFSET);
                cachedAddresses[7] = ResolvePointer(base, EFZ_BASE_OFFSET_P2, RF_OFFSET);
                cachedAddresses[8] = ResolvePointer(base, EFZ_BASE_OFFSET_P2, XPOS_OFFSET);
                cachedAddresses[9] = ResolvePointer(base, EFZ_BASE_OFFSET_P2, YPOS_OFFSET);
                // Add character name addresses to cached addresses
                cachedAddresses[10] = ResolvePointer(base, EFZ_BASE_OFFSET_P1, CHARACTER_NAME_OFFSET);
                cachedAddresses[11] = ResolvePointer(base, EFZ_BASE_OFFSET_P2, CHARACTER_NAME_OFFSET);
                titleCacheCounter = 0;
            }
            
            // Read values using cached addresses (existing code)
            if (cachedAddresses[0]) memcpy(&displayData.hp1, (void*)cachedAddresses[0], sizeof(WORD));
            if (cachedAddresses[1]) memcpy(&displayData.meter1, (void*)cachedAddresses[1], sizeof(WORD));
            if (cachedAddresses[2]) {
                double rf = 0.0;
                memcpy(&rf, (void*)cachedAddresses[2], sizeof(double));
                if (rf >= 0.0 && rf <= MAX_RF) {
                    displayData.rf1 = rf;
                }
            }
            if (cachedAddresses[3]) memcpy(&displayData.x1, (void*)cachedAddresses[3], sizeof(double));
            if (cachedAddresses[4]) memcpy(&displayData.y1, (void*)cachedAddresses[4], sizeof(double));
            
            if (cachedAddresses[5]) memcpy(&displayData.hp2, (void*)cachedAddresses[5], sizeof(WORD));
            if (cachedAddresses[6]) memcpy(&displayData.meter2, (void*)cachedAddresses[6], sizeof(WORD));
            if (cachedAddresses[7]) {
                double rf = 0.0;
                memcpy(&rf, (void*)cachedAddresses[7], sizeof(double));
                if (rf >= 0.0 && rf <= MAX_RF) {
                    displayData.rf2 = rf;
                }
            }
            if (cachedAddresses[8]) memcpy(&displayData.x2, (void*)cachedAddresses[8], sizeof(double));
            if (cachedAddresses[9]) memcpy(&displayData.y2, (void*)cachedAddresses[9], sizeof(double));
            if (cachedAddresses[10]) {
                SafeReadMemory(cachedAddresses[10], displayData.p1CharName, sizeof(displayData.p1CharName) - 1);
                displayData.p1CharName[sizeof(displayData.p1CharName) - 1] = '\0'; // Ensure null termination
            }
            if (cachedAddresses[11]) {
                SafeReadMemory(cachedAddresses[11], displayData.p2CharName, sizeof(displayData.p2CharName) - 1);
                displayData.p2CharName[sizeof(displayData.p2CharName) - 1] = '\0'; // Ensure null termination
            }
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
            // Detailed mode - show frame counters correctly
            int currentVisualFrame = frameCounter.load();
            double secondsElapsed = currentVisualFrame / 64.0; // 64 visual FPS
            
            sprintf_s(title, 
                "EFZ DLL | Visual Frame: %d (%.2fs @ 64FPS) | P1 Move: %d | P2 Move: %d | Detailed: %s",
                currentVisualFrame,
                secondsElapsed,
                GetCurrentMoveID(1),
                GetCurrentMoveID(2),
                detailedLogging.load() ? "ON" : "OFF");
            
            // Add auto-action status if enabled
            if (autoActionEnabled.load()) {
                std::string triggerTypes = "";
                if (triggerAfterBlockEnabled.load()) triggerTypes += "Block ";
                if (triggerOnWakeupEnabled.load()) triggerTypes += "Wakeup ";
                if (triggerAfterHitstunEnabled.load()) triggerTypes += "Hitstun ";
                if (triggerAfterAirtechEnabled.load()) triggerTypes += "Airtech ";
                
                if (triggerTypes.empty()) {
                    triggerTypes = "None";
                } else {
                    // Remove trailing space
                    triggerTypes = triggerTypes.substr(0, triggerTypes.length() - 1);
                }
                
                std::string actionName;
                switch (autoActionType.load()) {
                    case ACTION_5A: actionName = "5A"; break;
                    case ACTION_5B: actionName = "5B"; break;
                    case ACTION_5C: actionName = "5C"; break;
                    case ACTION_2A: actionName = "2A"; break;
                    case ACTION_2B: actionName = "2B"; break;
                    case ACTION_2C: actionName = "2C"; break;
                    case ACTION_JUMP: actionName = "Jump"; break;
                    case ACTION_BACKDASH: actionName = "Backdash"; break;
                    case ACTION_BLOCK: actionName = "Block"; break;
                    case ACTION_CUSTOM: actionName = "Custom(" + std::to_string(autoActionCustomID.load()) + ")"; break;
                    default: actionName = "Unknown"; break;
                }
                
                // Append auto-action info to title
                strcat_s(title, " | Auto: ");
                strcat_s(title, actionName.c_str());
                strcat_s(title, " (");
                strcat_s(title, triggerTypes.c_str());
                strcat_s(title, ")");
            }
        } 
        else {
            // Normal mode - show player stats
            sprintf_s(title,
                "EFZ Training Mode | P1: %s (HP:%d) vs P2: %s (HP:%d) | Press 3 to open menu",
                displayData.p1CharName[0] ? displayData.p1CharName : "Unknown",
                displayData.hp1,
                displayData.p2CharName[0] ? displayData.p2CharName : "Unknown", 
                displayData.hp2);
        }
        
        SetConsoleTitleA(title);
        
        // Keep the fast 250ms update as requested
        Sleep(250);
    }
}