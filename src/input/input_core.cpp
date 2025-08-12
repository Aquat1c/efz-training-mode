#include "../include/input/input_core.h"
#include "../include/core/memory.h"
#include "../include/core/logger.h"
#include "../include/core/constants.h"
#include "../include/utils/utilities.h"
    // For GetEFZBase()
#include "../include/input/input_buffer.h" // For INPUT_BUFFER_* constants
#include <sstream>

// Decode input mask to readable string
std::string DecodeInputMask(uint8_t inputMask) {
    if (inputMask == 0)
        return "N";
    std::string result;
    
    // Directions
    if (inputMask & GAME_INPUT_UP) {
        if (inputMask & GAME_INPUT_LEFT) result += "UL";
        else if (inputMask & GAME_INPUT_RIGHT) result += "UR";
        else result += "U";
    } else if (inputMask & GAME_INPUT_DOWN) {
        if (inputMask & GAME_INPUT_LEFT) result += "DL";
        else if (inputMask & GAME_INPUT_RIGHT) result += "DR";
        else result += "D";
    } else if (inputMask & GAME_INPUT_LEFT) {
        result += "L";
    } else if (inputMask & GAME_INPUT_RIGHT) {
        result += "R";
    }
    
    // Buttons
    if (inputMask & GAME_INPUT_A) result += "A";
    if (inputMask & GAME_INPUT_B) result += "B";
    if (inputMask & GAME_INPUT_C) result += "C";
    if (inputMask & GAME_INPUT_D) result += "D";
    
    return result;
}

// Helper function to get player base pointer
uintptr_t GetPlayerPointer(int playerNum) {
    uintptr_t base = GetEFZBase();
    if (!base) return 0;
    
    uintptr_t playerPtrOffset = (playerNum == 1) ? EFZ_BASE_OFFSET_P1 : EFZ_BASE_OFFSET_P2;
    uintptr_t playerPtr = 0;
    
    if (!SafeReadMemory(base + playerPtrOffset, &playerPtr, sizeof(uintptr_t))) {
        return 0;
    }
    
    return playerPtr;
}

// Write to the player's immediate input registers
bool WritePlayerInputImmediate(int playerNum, uint8_t inputMask) {
    uintptr_t playerPtr = GetPlayerPointer(playerNum);
    if (!playerPtr) return false;
    
    // Extract direction and button components
    bool up = (inputMask & GAME_INPUT_UP) != 0;
    bool down = (inputMask & GAME_INPUT_DOWN) != 0;
    bool left = (inputMask & GAME_INPUT_LEFT) != 0;
    bool right = (inputMask & GAME_INPUT_RIGHT) != 0;
    
    // Convert to game's internal format
    uint8_t horzValue = 0;
    uint8_t vertValue = 0;
    
    // Horizontal: 0=neutral, 1=right, 255=left
    if (right) horzValue = 1;
    else if (left) horzValue = 255;
    
    // Vertical: 0=neutral, 1=down, 255=up
    if (down) vertValue = 1;
    else if (up) vertValue = 255;
    
    // Write direction values
    if (!SafeWriteMemory(playerPtr + INPUT_HORIZONTAL_OFFSET, &horzValue, sizeof(uint8_t)))
        return false;
    if (!SafeWriteMemory(playerPtr + INPUT_VERTICAL_OFFSET, &vertValue, sizeof(uint8_t)))
        return false;
        
    // Write button values (1=pressed, 0=released)
    uint8_t buttonA = (inputMask & GAME_INPUT_A) ? 1 : 0;
    uint8_t buttonB = (inputMask & GAME_INPUT_B) ? 1 : 0;
    uint8_t buttonC = (inputMask & GAME_INPUT_C) ? 1 : 0;
    uint8_t buttonD = (inputMask & GAME_INPUT_D) ? 1 : 0;
    
    if (!SafeWriteMemory(playerPtr + INPUT_BUTTON_A_OFFSET, &buttonA, sizeof(uint8_t)))
        return false;
    if (!SafeWriteMemory(playerPtr + INPUT_BUTTON_B_OFFSET, &buttonB, sizeof(uint8_t)))
        return false;
    if (!SafeWriteMemory(playerPtr + INPUT_BUTTON_C_OFFSET, &buttonC, sizeof(uint8_t)))
        return false;
    if (!SafeWriteMemory(playerPtr + INPUT_BUTTON_D_OFFSET, &buttonD, sizeof(uint8_t)))
        return false;
    
    return true;
}

// Write to the circular buffer (for move history/detection)
bool WritePlayerInputToBuffer(int playerNum, uint8_t inputMask) {
    uintptr_t playerPtr = GetPlayerPointer(playerNum);
    if (!playerPtr) return false;
    
    // Get current buffer index
    uint16_t currentIndex = 0;
    if (!SafeReadMemory(playerPtr + INPUT_BUFFER_INDEX_OFFSET, &currentIndex, sizeof(uint16_t)))
        return false;
    
    // Write to current index in the buffer
    uint16_t bufferIndex = currentIndex % INPUT_BUFFER_SIZE;
    if (!SafeWriteMemory(playerPtr + INPUT_BUFFER_OFFSET + bufferIndex, &inputMask, sizeof(uint8_t)))
        return false;
    
    // Increment buffer index
    currentIndex = (currentIndex + 1) % INPUT_BUFFER_SIZE;
    if (!SafeWriteMemory(playerPtr + INPUT_BUFFER_INDEX_OFFSET, &currentIndex, sizeof(uint16_t)))
        return false;
    
    return true;
}

// Main function to write player input
bool WritePlayerInput(int playerNum, uint8_t inputMask) {
    // First write to immediate input registers
    if (!WritePlayerInputImmediate(playerNum, inputMask))
        return false;
    
    // Then write to input buffer for move detection
    if (!WritePlayerInputToBuffer(playerNum, inputMask))
        return false;
    
    return true;
}