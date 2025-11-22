#include <fstream>
#include <iomanip>
#include <chrono>
#include "../include/core/logger.h"
#include "../include/core/version.h"
#include "../include/utils/utilities.h"
#include "../include/utils/network.h" // For EfzRevival version detection

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
// For global shutdown flag
#include "../include/core/globals.h"
 // Add this include
#include "../include/input/input_motion.h"
// Snapshot access for light reads
#include "../include/game/frame_monitor.h"
#include "../include/game/character_settings.h"
#include "../include/utils/debug_log.h"

std::mutex g_logMutex;
std::atomic<bool> detailedTitleMode(false);
std::atomic<bool> detailedDebugOutput(false);
std::atomic<bool> g_reducedLogging(true);

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
    // After online hard-stop or during shutdown, suppress all logging entirely
    if (g_onlineModeActive.load() || g_isShuttingDown.load()) {
        return;
    }
    
    // Always write to debug log if it's a switch-related message
    if (msg.find("[SWITCH]") != std::string::npos || 
        msg.find("[FREEZE]") != std::string::npos ||
        msg.find("[AI]") != std::string::npos ||
        msg.find("[ENGINE]") != std::string::npos) {
        DebugLog::Write(msg);
    }
    
    // Only output to console if requested
    if (consoleOutput) {
        // Quick pre-filter (no lock or timestamp) to drop verbose categories when detailedLogging is off
        std::string currentCategory = "OTHER";
        {
            size_t startBracket = msg.find('[');
            size_t endBracket = msg.find(']', startBracket);
            if (startBracket != std::string::npos && endBracket != std::string::npos) {
                currentCategory = msg.substr(startBracket + 1, endBracket - startBracket - 1);
            }
        }
        bool isDetailedDebugMsg =
            currentCategory == "WINDOW" ||
            currentCategory == "OVERLAY" ||
            currentCategory == "IMGUI" ||
            currentCategory == "IMGUI_MONITOR" ||
            currentCategory == "CONFIG" ||
            currentCategory == "KEYBINDS" ||
            // High-frequency categories gated unless detailedLogging is on
            currentCategory == "INPUT_BUFFER" ||
            currentCategory == "BUFFER_FREEZE" ||
            currentCategory == "BUFFER_DEBUG" ||
            currentCategory == "BUFFER_COMBO" ||
            currentCategory == "BUFFER_DUMP" ||
            currentCategory == "AUTO-ACTION" ||
            currentCategory == "TRIGGER_DIAG" ||
            currentCategory == "DELAY" ||
            currentCategory == "COOLDOWN" ||
            currentCategory == "DASH" ||
            currentCategory == "DASH_DEBUG" ||
            currentCategory == "AUTO_GUARD" ||
            currentCategory == "CRG" ||
            currentCategory == "RG";
        if (isDetailedDebugMsg && !detailedLogging.load()) {
            return;
        }

        std::lock_guard<std::mutex> lock(g_logMutex);

        auto buildPrefix = []() -> std::string {
            auto now = std::chrono::system_clock::now();
            auto timeT = std::chrono::system_clock::to_time_t(now);
            tm timeInfo{};
            localtime_s(&timeInfo, &timeT);
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
            char ts[32];
            std::strftime(ts, sizeof(ts), "%H:%M:%S", &timeInfo);
            std::ostringstream tsoss; tsoss << ts << "." << std::setw(3) << std::setfill('0') << ms.count() << " ";
            return tsoss.str();
        };

        // Buffer until console window exists (store formatted with timestamp)
        if (!g_consoleReady.load() || GetConsoleWindow() == nullptr) {
            std::string formatted = msg.empty() ? std::string() : (buildPrefix() + msg);
            g_pendingConsoleLogs.emplace_back(formatted);
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

        // Don't add extra spacing if the last line was empty or this is a help message
        bool isHelpMessage = (msg.find("Key") != std::string::npos && msg.find(":") != std::string::npos) ||
                             msg.find("NOTE:") != std::string::npos ||
                             msg.find("---") != std::string::npos;

        // Add spacing based on category change, but not for help messages
        if (!wasEmptyLine && !isHelpMessage && !lastCategory.empty() && currentCategory != lastCategory) {
            std::cout << std::endl;
        }

        // Reduced logging duplicate suppression & lightweight category throttling
        if (g_reducedLogging.load()) {
            // Maintain a tiny ring of last few messages to collapse duplicates within a window
            struct DupEntry { std::string text; int count; std::chrono::steady_clock::time_point first; };
            static std::vector<DupEntry> recent; // intentionally small
            static const size_t kMaxDupEntries = 16;
            static const auto kWindow = std::chrono::seconds(3); // collapse duplicates over 3s
            auto nowSteady = std::chrono::steady_clock::now();
            // Expire old entries
            recent.erase(std::remove_if(recent.begin(), recent.end(), [&](const DupEntry &e){return (nowSteady - e.first) > kWindow;}), recent.end());
            // Key off raw msg (without timestamp)
            bool suppressed = false;
            for (auto &e : recent) {
                if (e.text == msg) {
                    e.count++;
                    suppressed = true;
                    break;
                }
            }
            if (!suppressed) {
                if (recent.size() >= kMaxDupEntries) recent.erase(recent.begin());
                recent.push_back({msg,1,nowSteady});
            }
            // Periodically flush accumulated counts (once per second)
            static auto lastFlush = nowSteady;
            if (nowSteady - lastFlush >= std::chrono::seconds(1)) {
                for (auto &e : recent) {
                    std::string p = buildPrefix();
                    if (e.count > 1) {
                        std::cout << p << e.text << " (x" << e.count << ")" << std::endl;
                    } else if (e.count == 1) {
                        std::cout << p << e.text << std::endl;
                    }
                }
                recent.clear();
                lastFlush = nowSteady;
            }
            if (suppressed) {
                return; // defer actual printing to periodic flush
            }
        }

        // Output the message immediately (non-reduced or first occurrence)
        {
            std::string formatted = buildPrefix() + msg;
            std::cout << formatted << std::endl;
        }

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

    LogOut("==============================================", true);
    LogOut(std::string("  EFZ Training Mode v") + EFZ_TRAINING_MODE_VERSION, true);
    LogOut(std::string("  Build: ") + EFZ_TRAINING_MODE_BUILD_DATE + " " + EFZ_TRAINING_MODE_BUILD_TIME, true);
    LogOut("==============================================", true);
    
    // Log detected EfzRevival version early
    EfzRevivalVersion detectedVer = GetEfzRevivalVersion();
    std::string verMsg = "[VERSION] Detected: ";
    verMsg += EfzRevivalVersionName(detectedVer);
    verMsg += IsEfzRevivalVersionSupported(detectedVer) ? " (supported)" : " (UNSUPPORTED)";
    LogOut(verMsg, true);
    
    // Developer motion-debug hotkey banner removed
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
        // Exit if shutting down
        if (g_isShuttingDown.load()) break;
        // Exit immediately when entering online/hard-stopped mode to silence all activity
        if (g_onlineModeActive.load()) {
            // Proactively destroy console so no further output appears
            DestroyDebugConsole();
            break;
        }

        // If the console window isn't present or visible, back off and try later
        HWND hWnd = GetConsoleWindow();
        if (hWnd == nullptr || !IsWindow(hWnd) || !IsWindowVisible(hWnd)) {
            Sleep(500);
            continue;
        }

        char title[512];
        uintptr_t base = GetEFZBase();
        
        // Keep the fast update rate as requested - every 250ms
        if (base != 0) {
            // Prefer snapshot for fast reads
            FrameSnapshot snap{};
            bool haveSnap = TryGetLatestSnapshot(snap, 500);

            if (haveSnap) {
                displayData.hp1 = snap.p1Hp; displayData.hp2 = snap.p2Hp;
                displayData.meter1 = snap.p1Meter; displayData.meter2 = snap.p2Meter;
                displayData.rf1 = snap.p1RF; displayData.rf2 = snap.p2RF;
                displayData.x1 = snap.p1X; displayData.y1 = snap.p1Y;
                displayData.x2 = snap.p2X; displayData.y2 = snap.p2Y;
                // Fill char names via ID mapping when available
                if (snap.p1CharId >= 0) {
                    auto n1 = CharacterSettings::GetCharacterName(snap.p1CharId);
                    strncpy_s(displayData.p1CharName, n1.c_str(), sizeof(displayData.p1CharName)-1);
                }
                if (snap.p2CharId >= 0) {
                    auto n2 = CharacterSettings::GetCharacterName(snap.p2CharId);
                    strncpy_s(displayData.p2CharName, n2.c_str(), sizeof(displayData.p2CharName)-1);
                }
            } else {
                // Minimal fallback: refresh addresses occasionally and read values (including names)
                static uintptr_t cachedAddresses[12] = {0};
                static int titleCacheCounter = 0;
                // Refresh cached addresses less frequently to reduce pointer resolution overhead
                if (titleCacheCounter++ >= 60) {
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
                if (cachedAddresses[0]) SafeReadMemory(cachedAddresses[0], &displayData.hp1, sizeof(int));
                if (cachedAddresses[1]) { unsigned short w=0; SafeReadMemory(cachedAddresses[1], &w, sizeof(w)); displayData.meter1 = (int)w; }
                if (cachedAddresses[2]) SafeReadMemory(cachedAddresses[2], &displayData.rf1, sizeof(double));
                if (cachedAddresses[3]) SafeReadMemory(cachedAddresses[3], &displayData.x1, sizeof(double));
                if (cachedAddresses[4]) SafeReadMemory(cachedAddresses[4], &displayData.y1, sizeof(double));
                if (cachedAddresses[5]) SafeReadMemory(cachedAddresses[5], &displayData.p1CharName, sizeof(displayData.p1CharName) - 1);
                if (cachedAddresses[6]) SafeReadMemory(cachedAddresses[6], &displayData.hp2, sizeof(int));
                if (cachedAddresses[7]) { unsigned short w=0; SafeReadMemory(cachedAddresses[7], &w, sizeof(w)); displayData.meter2 = (int)w; }
                if (cachedAddresses[8]) SafeReadMemory(cachedAddresses[8], &displayData.rf2, sizeof(double));
                if (cachedAddresses[9]) SafeReadMemory(cachedAddresses[9], &displayData.x2, sizeof(double));
                if (cachedAddresses[10]) SafeReadMemory(cachedAddresses[10], &displayData.y2, sizeof(double));
                if (cachedAddresses[11]) SafeReadMemory(cachedAddresses[11], &displayData.p2CharName, sizeof(displayData.p2CharName) - 1);
            }

            // Feed the shared positions cache
            UpdatePositionCache(displayData.x1, displayData.y1, displayData.x2, displayData.y2);
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
            // Already stored with timestamp prefix above; write directly
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

void SetReducedLogging(bool reduced) {
    g_reducedLogging.store(reduced);
}