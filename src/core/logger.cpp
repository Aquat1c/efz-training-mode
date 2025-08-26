#include <fstream>
#include <iomanip>
#include <chrono>
#include "../include/core/logger.h"
#include "../include/utils/utilities.h"

#include "../include/core/memory.h"
#include "../include/core/constants.h"
#include <iostream>
#include <sstream> // Required for std::ostringstream
#include <vector>
#include <chrono>
#include <iomanip>
#include <windows.h>
#include <atomic>
#include <thread>
#include "../include/game/game_state.h"
 // Add this include
#include "../include/input/input_motion.h"

std::mutex g_logMutex;
std::atomic<bool> detailedTitleMode(false);
std::atomic<bool> detailedDebugOutput(false);

// Buffer logs until console is ready so enabling console later shows early logs
static std::vector<std::string> g_pendingConsoleLogs;
std::atomic<bool> g_consoleReady{false};

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
        // Buffer until console window exists
        if (!g_consoleReady.load() || GetConsoleWindow() == nullptr) {
            g_pendingConsoleLogs.emplace_back(msg);
            return;
        }
        
        // Skip spacing logic for empty lines - this fixes most spacing issues
        if (msg.empty()) {
            std::cout << std::endl;
            return;
        }
        
        // Track message categories for proper spacing
        static std::string lastCategory = "";
        static bool wasEmptyLine = false;
        std::string currentCategory = "OTHER"; // Default
        
        // Extract the message category from inside first []
        size_t startBracket = msg.find('[');
        size_t endBracket = msg.find(']', startBracket);
        
        if (startBracket != std::string::npos && endBracket != std::string::npos) {
            currentCategory = msg.substr(startBracket + 1, endBracket - startBracket - 1);
        }
        
        // Skip certain debug messages unless detailed debug is enabled
        bool isDetailedDebugMsg = 
            currentCategory == "WINDOW" || 
            currentCategory == "OVERLAY" || 
            currentCategory == "IMGUI" || 
            currentCategory == "IMGUI_MONITOR" ||
            currentCategory == "CONFIG" ||
            currentCategory == "KEYBINDS";
            
        // Don't show detailed debug messages unless enabled
        if (isDetailedDebugMsg && !detailedLogging.load()) {
            return;
        }
        
        // Don't add extra spacing if the last line was empty or this is a help message
        bool isHelpMessage = (msg.find("Key") != std::string::npos && msg.find(":") != std::string::npos) || 
                            msg.find("NOTE:") != std::string::npos ||
                            msg.find("---") != std::string::npos;
        
        // Add spacing based on category change, but not for help messages
        if (!wasEmptyLine && !isHelpMessage && !lastCategory.empty() && currentCategory != lastCategory) {
            std::cout << std::endl;
        }
        
        // Output the message
        std::cout << msg << std::endl;
        
        // Update tracking variables
        wasEmptyLine = msg.empty();
        if (!msg.empty() && !isHelpMessage) {
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
    LogOut("Debug Hotkeys:", true);
    LogOut("Numpad 8 = Enhanced Dragon Punch Buffer Freeze with diagnostic info", true);
    LogOut("Numpad 9 = Original Dragon Punch Buffer Freeze", true);
    LogOut("Numpad 5 = Stop Buffer Freezing", true);
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
    std::string lastTitle;
    int sleepMs = 100; // fast when changing
    const int minSleepMs = 100;
    const int maxSleepMs = 250; // slower when idle/no match
    int stableIters = 0;
    
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
                titleCacheCounter = 0;
                cachedAddresses[0] = ResolvePointer(base, EFZ_BASE_OFFSET_P1, HP_OFFSET);
                cachedAddresses[1] = ResolvePointer(base, EFZ_BASE_OFFSET_P1, METER_OFFSET);
                cachedAddresses[2] = ResolvePointer(base, EFZ_BASE_OFFSET_P1, RF_OFFSET);
                cachedAddresses[3] = ResolvePointer(base, EFZ_BASE_OFFSET_P1, XPOS_OFFSET);
                cachedAddresses[4] = ResolvePointer(base, EFZ_BASE_OFFSET_P1, YPOS_OFFSET);
                cachedAddresses[5] = ResolvePointer(base, EFZ_BASE_OFFSET_P1, CHARACTER_NAME_OFFSET);
                cachedAddresses[6] = ResolvePointer(base, EFZ_BASE_OFFSET_P2, HP_OFFSET);
                cachedAddresses[7] = ResolvePointer(base, EFZ_BASE_OFFSET_P2, METER_OFFSET);
                cachedAddresses[8] = ResolvePointer(base, EFZ_BASE_OFFSET_P2, RF_OFFSET);
                cachedAddresses[9] = ResolvePointer(base, EFZ_BASE_OFFSET_P2, XPOS_OFFSET);
                cachedAddresses[10] = ResolvePointer(base, EFZ_BASE_OFFSET_P2, YPOS_OFFSET);
                cachedAddresses[11] = ResolvePointer(base, EFZ_BASE_OFFSET_P2, CHARACTER_NAME_OFFSET);
            }
            
            // Read values from cached addresses
            if (cachedAddresses[0]) SafeReadMemory(cachedAddresses[0], &displayData.hp1, sizeof(int));
            if (cachedAddresses[1]) SafeReadMemory(cachedAddresses[1], &displayData.meter1, sizeof(int));
            if (cachedAddresses[2]) SafeReadMemory(cachedAddresses[2], &displayData.rf1, sizeof(double));
            if (cachedAddresses[3]) SafeReadMemory(cachedAddresses[3], &displayData.x1, sizeof(double));
            if (cachedAddresses[4]) SafeReadMemory(cachedAddresses[4], &displayData.y1, sizeof(double));
            if (cachedAddresses[5]) SafeReadMemory(cachedAddresses[5], &displayData.p1CharName, sizeof(displayData.p1CharName) - 1);
            if (cachedAddresses[6]) SafeReadMemory(cachedAddresses[6], &displayData.hp2, sizeof(int));
            if (cachedAddresses[7]) SafeReadMemory(cachedAddresses[7], &displayData.meter2, sizeof(int));
            if (cachedAddresses[8]) SafeReadMemory(cachedAddresses[8], &displayData.rf2, sizeof(double));
            if (cachedAddresses[9]) SafeReadMemory(cachedAddresses[9], &displayData.x2, sizeof(double));
            if (cachedAddresses[10]) SafeReadMemory(cachedAddresses[10], &displayData.y2, sizeof(double));
            if (cachedAddresses[11]) SafeReadMemory(cachedAddresses[11], &displayData.p2CharName, sizeof(displayData.p2CharName) - 1);
        }
        
        // Check if we can access game data or if all values are default/zero
        bool gameActive = base != 0;
        bool allZeros = displayData.hp1 == 0 && displayData.hp2 == 0 && 
                       displayData.meter1 == 0 && displayData.meter2 == 0 &&
                       displayData.x1 == 0 && displayData.y1 == 0;
        
        if (!gameActive || allZeros) {
            sprintf_s(title, sizeof(title), "EFZ Training Mode - Waiting for match...");
        } 
        else {
            sprintf_s(title, sizeof(title),
                    "P1 (%s): %d HP, %d Meter, %.1f RF | P2 (%s): %d HP, %d Meter, %.1f RF | Frame: %d",
                    displayData.p1CharName, displayData.hp1, displayData.meter1, displayData.rf1,
                    displayData.p2CharName, displayData.hp2, displayData.meter2, displayData.rf2,
                    frameCounter.load() / 3);
        }
    
        // Append game mode information to the title, regardless of game state
        uint8_t rawValue;
        GameMode currentMode = GetCurrentGameMode(&rawValue); // Get both enum and raw value
        std::string modeName = GetGameModeName(currentMode);
        
        char modeBuffer[100];
        sprintf_s(modeBuffer, sizeof(modeBuffer), " | Mode: %s (%d)", modeName.c_str(), rawValue);
        strcat_s(title, sizeof(title), modeBuffer);
        
        // Only update title if it changed
        if (lastTitle != title) {
            SetConsoleTitleA(title);
            lastTitle = title;
            sleepMs = minSleepMs;
            stableIters = 0;
        } else {
            // Back off when no changes
            stableIters++;
            if (stableIters > 2) sleepMs = maxSleepMs;
        }
        
        Sleep(sleepMs);
    }
}

void FlushPendingConsoleLogs() {
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (g_consoleReady.load() && GetConsoleWindow() != nullptr) {
        for (const auto& line : g_pendingConsoleLogs) {
            // Reuse normal path but bypass re-buffering by writing directly
            if (line.empty()) {
                std::cout << std::endl;
                continue;
            }
            // Filter by detailedLogging like normal LogOut does
            size_t startBracket = line.find('[');
            size_t endBracket = line.find(']', startBracket);
            std::string currentCategory = "OTHER";
            if (startBracket != std::string::npos && endBracket != std::string::npos) {
                currentCategory = line.substr(startBracket + 1, endBracket - startBracket - 1);
            }
            bool isDetailedDebugMsg =
                currentCategory == "WINDOW" ||
                currentCategory == "OVERLAY" ||
                currentCategory == "IMGUI" ||
                currentCategory == "IMGUI_MONITOR" ||
                currentCategory == "CONFIG" ||
                currentCategory == "KEYBINDS";
            if (isDetailedDebugMsg && !detailedLogging.load()) {
                continue;
            }
            std::cout << line << std::endl;
        }
        g_pendingConsoleLogs.clear();
    }
}

void SetConsoleReady(bool ready) {
    g_consoleReady = ready;
    if (ready) {
        FlushPendingConsoleLogs();
    }
}