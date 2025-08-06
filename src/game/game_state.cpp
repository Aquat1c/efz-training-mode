#include "../include/game/game_state.h"

#include "../include/core/memory.h"
#include "../include/core/constants.h"
#include "../include/utils/utilities.h"

#include "../include/core/logger.h"
#include <sstream>
#include <iomanip>

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