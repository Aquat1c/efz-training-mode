#include "../include/game/game_state.h"

#include "../include/core/memory.h"
#include "../include/core/constants.h"
#include "../include/utils/utilities.h"
#include "frame_monitor.h"
#include "../include/core/logger.h"
#include <sstream>
#include <iomanip>

static std::atomic<GamePhase> g_phaseCache{ GamePhase::Unknown };
static std::atomic<int> g_phaseStableFrames{0};
static std::atomic<uint8_t> g_lastRawScreenState{ 255 };

// REVISED: Now takes an optional out parameter.
GameMode GetCurrentGameMode(uint8_t* rawValueOut) {
    // Static variables to track the previous raw value and guarantee the first read is logged.
    static uint8_t prevRawValue = 99; // Initialize to a value that cannot be the actual first value.

    GameMode currentMode = GameMode::Unknown;
    uint8_t rawValue = 255; // Default to unknown

    uintptr_t efzBase = GetEFZBase();
    uintptr_t gameModeAddr = 0;
    uintptr_t gameStructPtr = 0;

    if (efzBase) {
        // --- MANUAL POINTER RESOLUTION ---
        // Step 1: Get the address of the base pointer.
        uintptr_t basePtrAddr = efzBase + EFZ_BASE_OFFSET_GAME_STATE;
        
        // Step 2: Read the value of the base pointer.
        if (SafeReadMemory(basePtrAddr, &gameStructPtr, sizeof(uintptr_t))) {
            // Step 3: Add the final offset to get the game mode address.
            gameModeAddr = gameStructPtr + GAME_MODE_OFFSET;
            
            // Step 4: Read the final byte value.
            SafeReadMemory(gameModeAddr, &rawValue, sizeof(uint8_t));
        }
    }
    
    // Always convert the raw value to its corresponding enum.
    switch (rawValue) {
        case 0: currentMode = GameMode::Arcade; break;
        case 1: currentMode = GameMode::Practice; break;
        case 3: currentMode = GameMode::VsCpu; break;
        case 4: currentMode = GameMode::VsHuman; break;
        case 5: currentMode = GameMode::Replay; break;
        case 6: currentMode = GameMode::AutoReplay; break;
        default: currentMode = GameMode::Unknown; break;
    }

    // If the raw byte value has changed, log it to the console.
    if (rawValue != prevRawValue) {
        // --- DETAILED DEBUG LOGGING FOR POINTER RESOLUTION ---
        std::ostringstream oss;
        oss << std::hex << std::uppercase;
        LogOut("", true); // Add a blank line for readability
        LogOut("[GAME_STATE_DBG] --- Game Mode Pointer Trace ---", true);
        oss << "[GAME_STATE_DBG] efz.exe Base Address: 0x" << efzBase;
        LogOut(oss.str(), true); oss.str(""); oss.clear();
        
        oss << "[GAME_STATE_DBG] Reading pointer at [efz.exe + 0x" << EFZ_BASE_OFFSET_GAME_STATE << "] = 0x" << (efzBase + EFZ_BASE_OFFSET_GAME_STATE);
        LogOut(oss.str(), true); oss.str(""); oss.clear();
        
        oss << "[GAME_STATE_DBG]   -> Read value (as 4-byte pointer): 0x" << gameStructPtr;
        LogOut(oss.str(), true); oss.str(""); oss.clear();

        oss << "[GAME_STATE_DBG] Reading byte at [0x" << gameStructPtr << " + 0x" << GAME_MODE_OFFSET << "] = 0x" << gameModeAddr;
        LogOut(oss.str(), true); oss.str(""); oss.clear();

        oss << "[GAME_STATE_DBG]   -> Read value (as 1-byte): " << std::dec << (int)rawValue;
        LogOut(oss.str(), true); oss.str(""); oss.clear();
        LogOut("[GAME_STATE_DBG] ---------------------------------", true);
        // --- END DEBUG LOGGING ---

        std::string modeName = GetGameModeName(currentMode);
        LogOut("[GAME STATE] Game mode changed to: " + modeName + " (Value: " + std::to_string(rawValue) + ")", true);
        prevRawValue = rawValue;
    }

    // Pass the raw value back to the caller if requested.
    if (rawValueOut) {
        *rawValueOut = rawValue;
    }

    return currentMode;
}

std::string GetGameModeName(GameMode mode) {
    switch (mode) {
        case GameMode::Arcade:      return "Arcade";
        case GameMode::Practice:    return "Practice";
        case GameMode::VsCpu:       return "VS CPU";
        case GameMode::VsHuman:     return "VS Human";
        case GameMode::Replay:      return "Replay";
        case GameMode::AutoReplay:  return "Auto-Replay";
        default:                    return "Unknown";
    }
}

// Replace screen/phase with direct screen-state byte
static uint8_t ReadRawScreenState() {
    uintptr_t base = GetEFZBase();
    uint8_t v = 255;
    if (!base) return v;
    // Direct byte read at efz.exe+0x390148
    SafeReadMemory(base + EFZ_BASE_OFFSET_SCREEN_STATE, &v, sizeof(v));
    return v;
}

// Character Select quick check using the raw byte
bool IsInCharacterSelectScreen() {
    uint8_t st = ReadRawScreenState();
    return st == 1; // 1 = Character Select
}

// Gameplay state: rely on raw byte 3 (in-game) conservatively
bool IsInGameplayState() {
    uint8_t st = ReadRawScreenState();
    return st == 3; // 3 = In-game
}

// Enhanced debug dump: log first 0x40 bytes (byte granularity)
void DebugDumpScreenState() {
    uintptr_t base = GetEFZBase();
    if (!base) {
        LogOut("[SCREEN_STATE] Base missing", true);
        return;
    }
    uintptr_t gameStatePtr = 0;
    if (!SafeReadMemory(base + EFZ_BASE_OFFSET_GAME_STATE, &gameStatePtr, sizeof(uintptr_t)) || !gameStatePtr) {
        LogOut("[SCREEN_STATE] Game state ptr invalid", true);
        return;
    }

    LogOut("[SCREEN_STATE] ---- Game State Raw (first 0x40 bytes) ----", true);
    uint8_t buffer[0x40]{};
    SafeReadMemory(gameStatePtr, buffer, sizeof(buffer));
    std::string line;
    for (int i = 0; i < 0x40; i++) {
        char b[8];
        sprintf_s(b, "%02X ", buffer[i]);
        line += b;
        if ((i & 0x0F) == 0x0F) {
            LogOut("[SCREEN_STATE] " + line, true);
            line.clear();
        }
    }
    if (!line.empty())
        LogOut("[SCREEN_STATE] " + line, true);

    // Summaries
    uint8_t rawScr = ReadRawScreenState();
    LogOut("[SCREEN_STATE] Mode=" + GetGameModeName(GetCurrentGameMode()) +
           " ScreenByte=" + std::to_string(rawScr) +
           " CharSelect=" + std::to_string(IsInCharacterSelectScreen()), true);
    LogOut("[SCREEN_STATE] ------------------------------------------", true);
}



GamePhase GetCurrentGamePhase() {
    // Map efz.exe+0x390148 directly to our phases with minimal debounce
    uint8_t st = ReadRawScreenState();
    g_lastRawScreenState.store(st);
    GamePhase derived = GamePhase::Unknown;

    switch (st) {
        case 0: // Title
        case 6: // Settings
        case 8: // Replay selection
        case 5: // Win screen (disable features; not relevant for Practice)
            derived = GamePhase::Menu;
            break;
        case 1: // Character Select
            derived = GamePhase::CharacterSelect;
            break;
        case 2: // Loading
            derived = GamePhase::Loading;
            break;
        case 3: // In-game
            derived = GamePhase::Match;
            break;
        default:
            derived = GamePhase::Unknown;
            break;
    }

    // Debounce: require 3 consecutive frames before committing
    static GamePhase lastRaw = GamePhase::Unknown;
    if (derived == lastRaw) {
        int s = g_phaseStableFrames.fetch_add(1) + 1;
        if (s >= 3) g_phaseCache.store(derived);
    } else {
        g_phaseStableFrames.store(0);
        lastRaw = derived;
    }

    return g_phaseCache.load();
}

// OPTIONAL: expose a small debug hook
void LogPhaseIfChanged() {
    static GamePhase prev = GamePhase::Unknown;
    GamePhase now = g_phaseCache.load();
    if (now != prev) {
        LogOut("[GAME_PHASE] " + std::to_string((int)prev) + " -> " + std::to_string((int)now), true);
        prev = now;
    }
}