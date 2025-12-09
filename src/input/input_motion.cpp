#include "../include/input/input_motion.h"
#include "../include/core/memory.h"
#include "../include/core/logger.h"
#include "../include/core/constants.h"
#include "../include/utils/utilities.h"

#include "../include/game/auto_action_helpers.h"
#include "../include/input/input_buffer.h"  
#include "../include/input/input_debug.h"  
#include "../include/input/input_core.h"    
#include "../include/input/shared_constants.h" 
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
// For global shutdown flag
#include "../include/core/globals.h"

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
    
    // Write the AI control flag (0 = human, 1 = AI) with audit logging
    uint32_t desired = human ? 0u : 1u;
    uint32_t before = 0xFFFFFFFFu, after = 0xFFFFFFFFu;
    SafeReadMemory(playerPtr + AI_CONTROL_FLAG_OFFSET, &before, sizeof(uint32_t));
    bool okWrite = SafeWriteMemory(playerPtr + AI_CONTROL_FLAG_OFFSET, &desired, sizeof(uint32_t));
    SafeReadMemory(playerPtr + AI_CONTROL_FLAG_OFFSET, &after, sizeof(uint32_t));
    if (before != after || !okWrite) {
        std::ostringstream oss;
        oss << "[AUDIT][AI] SetAIControlFlag P" << playerNum
            << " @0x" << std::hex << (playerPtr + AI_CONTROL_FLAG_OFFSET)
            << std::dec
            << " before=" << before
            << " write=" << desired
            << " after=" << after
            << " okWrite=" << (okWrite?"1":"0");
        LogOut(oss.str(), true);
    }
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
    int sleepMs = 16;
    int stableIters = 0;
    while (g_forceHumanControlActive.load() && !g_isShuttingDown.load() && !g_onlineModeActive.load()) {
        // Read current flag to avoid unnecessary writes
        bool alreadyHuman = IsAIControlFlagHuman(playerNum);
        if (!alreadyHuman) {
            SetAIControlFlag(playerNum, true);
            sleepMs = 16;
            stableIters = 0;
        } else {
            // Back off progressively when stable
            stableIters++;
            if (stableIters > 15) sleepMs = 32;   // ~31 Hz
            if (stableIters > 60) sleepMs = 64;   // ~16 Hz
            if (stableIters > 180) sleepMs = 128; // ~8 Hz
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));
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
