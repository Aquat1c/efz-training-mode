#include "../include/input/input_motion.h"
#include "../include/core/memory.h"
#include "../include/core/logger.h"
#include "../include/core/constants.h"
#include "../include/utils/utilities.h"

#include "../include/game/auto_action_helpers.h"
#include "../include/input/input_buffer.h"  
#include "../include/input/input_debug.h"  // Add this include
#include "../include/input/input_core.h"    // Add this include
#include "../include/input/shared_constants.h" // Add this include
#include <vector>
#include <sstream>
#include <chrono>
#include <iomanip>
#include <algorithm>
#include <atomic>
#include <mutex>
#include "../include/game/practice_patch.h"
#include <thread>
#include <chrono>

#ifdef max
#undef max // Undefine any existing max macro
#endif

// Define the missing MOTION_INPUT_BUTTON constant
#define MOTION_INPUT_BUTTON (MOTION_BUTTON_A | MOTION_BUTTON_B | MOTION_BUTTON_C | MOTION_BUTTON_D)

// Thread-safe input state tracking
std::atomic<bool> g_forceHumanControlActive(false);
extern std::atomic<bool> g_manualInputOverride[3];
extern std::atomic<uint8_t> g_manualInputMask[3];

// REMOVE ALL duplicate definitions that are defined in other files
// - Remove GetPlayerPointer (should be in input_core.cpp)
// - Remove DecodeInputMask (should be in input_core.cpp)
// - Remove WritePlayerInput functions (should be in input_core.cpp)
// - Remove queue variables and functions (should be in motion_system.cpp)
// - Remove buffer constants (should be defined in input_buffer.cpp)
// - Remove debug functions (should be in input_debug.cpp)

// This function is implemented in memory.cpp
// bool GetPlayerFacingDirection(int playerNum);

void SetAIControlFlag(int playerNum, bool human) {
    uintptr_t base = GetEFZBase();
    if (!base) return;
    
    uintptr_t playerOffset = (playerNum == 1) ? EFZ_BASE_OFFSET_P1 : EFZ_BASE_OFFSET_P2;
    uintptr_t playerPtr = 0;
    if (!SafeReadMemory(base + playerOffset, &playerPtr, sizeof(uintptr_t)) || !playerPtr) {
        return;
    }
    
    // Write the AI control flag (0 = human, 1 = AI)
    uint32_t controlFlag = human ? 0 : 1;
    SafeWriteMemory(playerPtr + AI_CONTROL_FLAG_OFFSET, &controlFlag, sizeof(uint32_t));
}

bool IsAIControlFlagHuman(int playerNum) {
    uintptr_t base = GetEFZBase();
    if (!base) return false;
    
    uintptr_t playerOffset = (playerNum == 1) ? EFZ_BASE_OFFSET_P1 : EFZ_BASE_OFFSET_P2;
    uintptr_t playerPtr = 0;
    if (!SafeReadMemory(base + playerOffset, &playerPtr, sizeof(uintptr_t)) || !playerPtr) {
        return false;
    }
    
    uint32_t controlFlag = 1; // Default to AI control
    if (!SafeReadMemory(playerPtr + AI_CONTROL_FLAG_OFFSET, &controlFlag, sizeof(uint32_t))) {
        return false;
    }
    
    // 0 = human control, 1 = AI control
    return controlFlag == 0;
}

void RestoreAIControlIfNeeded(int playerNum) {
    // Only restore if force human control is not active
    if (!g_forceHumanControlActive.load()) {
        // Set flag to AI control
        SetAIControlFlag(playerNum, false);
    }
}

void ForceHumanControl(int playerNum) {
    // Mark that we're forcing human control
    g_forceHumanControlActive.store(true);
    
    // Start a thread to continuously set the flag to human
    std::thread controlThread(ForceHumanControlThread, playerNum);
    controlThread.detach();
}

void ForceHumanControlThread(int playerNum) {
    LogOut("[INPUT_MOTION] Starting human control force thread for P" + std::to_string(playerNum), true);
    
    while (g_forceHumanControlActive.load()) {
        // Set to human control
        SetAIControlFlag(playerNum, true);
        
        // Sleep briefly to avoid hammering CPU
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
    
    LogOut("[INPUT_MOTION] Human control force thread terminated", true);
}

bool HoldUp(int playerNum) {
    // Up = 0x08
    return WritePlayerInput(playerNum, MOTION_INPUT_UP);
}

bool HoldBackCrouch(int playerNum) {
    // Get player's facing direction to determine which way is "back"
    bool facingRight = GetPlayerFacingDirection(playerNum);
    
    // If facing right, back is left (0x02). If facing left, back is right (0x01)
    uint8_t backInput = facingRight ? MOTION_INPUT_LEFT : MOTION_INPUT_RIGHT;
    
    // Combine with down input (0x04)
    return WritePlayerInput(playerNum, backInput | MOTION_INPUT_DOWN);
}

bool ReleaseInputs(int playerNum) {
    // Write neutral (0) to release all inputs
    return WritePlayerInput(playerNum, MOTION_INPUT_NEUTRAL);
}
