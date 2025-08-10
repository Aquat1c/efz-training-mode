#include "../include/input/input_buffer.h"
#include "../include/input/input_core.h"
#include "../include/core/constants.h"
#include "../include/core/memory.h"
#include "../include/core/logger.h"
#include "../include/utils/utilities.h"
#include "../include/input/motion_constants.h"
#include "../include/core/globals.h"
#include "../include/input/shared_constants.h" // Add this include for shared constants
#include "../include/input/input_debug.h"// Add this include for debug functions
#include "../include/input/motion_system.h"
#include "../include/input/input_motion.h"  // Add this include
#include "../include/game/game_state.h"  // Add this include
#include <vector>
#include <sstream>
#include <iomanip>
#include <algorithm>

// Define the buffer constants here
const uint16_t INPUT_BUFFER_SIZE = 0x180;  // 384 bytes circular buffer
const uintptr_t INPUT_BUFFER_OFFSET = 0x1AB;  // Buffer start offset in player struct
const uintptr_t INPUT_BUFFER_INDEX_OFFSET = 0x260;  // Current buffer index offset

// Define the freeze buffer variables here
std::atomic<bool> g_bufferFreezingActive(false);
std::atomic<bool> g_indexFreezingActive(false);
std::thread g_bufferFreezeThread;
std::vector<uint8_t> g_frozenBufferValues;
uint16_t g_frozenBufferStartIndex = 0;
uint16_t g_frozenBufferLength = 0;
uint16_t g_frozenIndexValue = 0;

// Define buffer functions
void FreezeBufferValuesThread(int playerNum) {
    LogOut("[INPUT_BUFFER] Starting buffer freeze thread for P" + std::to_string(playerNum), true);
    
    // Store initial valid pointer for safety check
    uintptr_t initialPlayerPtr = GetPlayerPointer(playerNum);
    if (!initialPlayerPtr) {
        LogOut("[INPUT_BUFFER] Invalid player pointer at thread start, aborting", true);
        g_bufferFreezingActive = false;
        return;
    }
    
    // Get the move ID address for this player
    uintptr_t base = GetEFZBase();
    uintptr_t moveIDAddr = ResolvePointer(base, 
        (playerNum == 1) ? EFZ_BASE_OFFSET_P1 : EFZ_BASE_OFFSET_P2, 
        MOVE_ID_OFFSET);
    
    int freezeCount = 0;
    int lastMoveID = -1;
    uint16_t lastIndex = 0xFFFF;
    int stabilityCounter = 0;
    std::vector<uint8_t> actualBufferValues(g_frozenBufferLength);
    
    while (g_bufferFreezingActive && !g_isShuttingDown.load()) {
        // CRITICAL FIX: Check if we're still in a valid game state
        GamePhase currentPhase = GetCurrentGamePhase();
        if (currentPhase != GamePhase::Match) {
            LogOut("[INPUT_BUFFER] Game phase changed to " + std::to_string((int)currentPhase) + 
                   ", stopping buffer freeze", true);
            break;
        }
        
        // CRITICAL FIX: Validate player pointer is still valid
        uintptr_t currentPlayerPtr = GetPlayerPointer(playerNum);
        if (!currentPlayerPtr || currentPlayerPtr != initialPlayerPtr) {
            LogOut("[INPUT_BUFFER] Player pointer invalidated, stopping buffer freeze", true);
            break;
        }
        
        // Validate buffer addresses before access
        if (!SafeReadMemory(currentPlayerPtr + INPUT_BUFFER_INDEX_OFFSET, &lastIndex, sizeof(uint16_t))) {
            LogOut("[INPUT_BUFFER] Failed to read buffer index, stopping freeze", true);
            break;
        }
        
        // Write frozen values
        for (size_t i = 0; i < g_frozenBufferLength; i++) {
            uint16_t bufferPos = (g_frozenBufferStartIndex + i) % INPUT_BUFFER_SIZE;
            uintptr_t addr = currentPlayerPtr + INPUT_BUFFER_OFFSET + bufferPos;
            
            if (!SafeWriteMemory(addr, &g_frozenBufferValues[i], sizeof(uint8_t))) {
                LogOut("[INPUT_BUFFER] Failed to write to buffer, stopping freeze", true);
                g_bufferFreezingActive = false;
                break;
            }
        }
        
        // Freeze index if requested
        if (g_indexFreezingActive) {
            if (!SafeWriteMemory(currentPlayerPtr + INPUT_BUFFER_INDEX_OFFSET, 
                                &g_frozenIndexValue, sizeof(uint16_t))) {
                LogOut("[INPUT_BUFFER] Failed to freeze index, stopping", true);
                break;
            }
        }
        
        freezeCount++;
        
        // Log periodically
        if (freezeCount % 60 == 0) {
            LogOut("[INPUT_BUFFER] Freeze active for " + std::to_string(freezeCount) + " frames", false);
        }
        
        Sleep(5); // ~192 FPS
    }
    
    LogOut("[INPUT_BUFFER] Buffer freeze thread terminated safely", true);
    g_bufferFreezingActive = false;
    g_indexFreezingActive = false;
}

// Capture current buffer section and begin freezing it
bool CaptureAndFreezeBuffer(int playerNum, uint16_t startIndex, uint16_t length) {
    // Stop any existing freeze thread
    StopBufferFreezing();
    
    uintptr_t playerPtr = GetPlayerPointer(playerNum);
    if (!playerPtr) {
        LogOut("[INPUT_BUFFER] Cannot get player pointer", true);
        return false;
    }
    
    // Validate parameters
    if (length > INPUT_BUFFER_SIZE) {
        LogOut("[INPUT_BUFFER] Length too large, capping at " + std::to_string(INPUT_BUFFER_SIZE), true);
        length = INPUT_BUFFER_SIZE;
    }
    
    // Read current buffer index for reference
    uint16_t currentIndex = 0;
    if (!SafeReadMemory(playerPtr + INPUT_BUFFER_INDEX_OFFSET, &currentIndex, sizeof(uint16_t))) {
        LogOut("[INPUT_BUFFER] Failed to read buffer index", true);
        return false;
    }
    
    // Read buffer values
    g_frozenBufferValues.resize(length);
    g_frozenBufferStartIndex = startIndex;
    g_frozenBufferLength = length;
    
    bool readSuccess = true;
    for (size_t i = 0; i < length; i++) {
        uint16_t readIdx = (startIndex + i) % INPUT_BUFFER_SIZE;
        if (!SafeReadMemory(playerPtr + INPUT_BUFFER_OFFSET + readIdx, &g_frozenBufferValues[i], sizeof(uint8_t))) {
            readSuccess = false;
            LogOut("[INPUT_BUFFER] Failed to read buffer value at " + std::to_string(readIdx), true);
        }
    }
    
    if (!readSuccess) {
        LogOut("[INPUT_BUFFER] Failed to read some buffer values, freezing may be inconsistent", true);
    }
    
    // Output the captured values
    std::stringstream ss;
    ss << "[INPUT_BUFFER] Captured values: ";
    for (size_t i = 0; i < length; i++) {
        ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(g_frozenBufferValues[i]);
        if (i < length - 1) ss << " ";
    }
    LogOut(ss.str(), true);
    
    // Start freezing
    g_bufferFreezingActive = true;
    g_bufferFreezeThread = std::thread(FreezeBufferValuesThread, playerNum);
    g_bufferFreezeThread.detach();  // Detach to prevent termination
    
    LogOut("[INPUT_BUFFER] Buffer freezing activated for P" + std::to_string(playerNum) + 
           " starting at index " + std::to_string(startIndex) +
           " with length " + std::to_string(length), true);
    return true;
}

// Option to also freeze buffer index
bool FreezeBufferIndex(int playerNum, uint16_t indexValue) {
    uintptr_t playerPtr = GetPlayerPointer(playerNum);
    if (!playerPtr) {
        LogOut("[INPUT_BUFFER] Cannot get player pointer", true);
        return false;
    }
    
    g_frozenIndexValue = indexValue;
    g_indexFreezingActive = true;
    
    LogOut("[INPUT_BUFFER] Buffer index freezing activated, locked to " + std::to_string(indexValue), true);
    return true;
}

// Function to stop buffer freezing
void StopBufferFreezing() {
    if (g_bufferFreezingActive) {
        g_bufferFreezingActive = false;
        g_indexFreezingActive = false;
        
        // Wait for thread to terminate (small timeout)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        
        // IMPORTANT: Write neutral inputs to the last few buffer entries
        // to prevent lingering input patterns from triggering moves
        uintptr_t base = GetEFZBase();
        if (base) {
            for (int player = 1; player <= 2; player++) {
                uintptr_t playerPtr = GetPlayerPointer(player);
                if (playerPtr) {
                    // Read current buffer index
                    uint16_t currentIndex = 0;
                    if (SafeReadMemory(playerPtr + INPUT_BUFFER_INDEX_OFFSET, &currentIndex, sizeof(uint16_t))) {
                        // Write neutral (0x00) to the last 8 buffer entries
                        uint8_t neutral = 0x00;
                        for (int i = 0; i < 8; i++) {
                            uint16_t writeIndex = (currentIndex - i) % INPUT_BUFFER_SIZE;
                            if (writeIndex < 0) writeIndex += INPUT_BUFFER_SIZE;
                            SafeWriteMemory(playerPtr + INPUT_BUFFER_OFFSET + writeIndex, &neutral, sizeof(uint8_t));
                        }
                    }
                }
            }
        }
        
        LogOut("[INPUT_BUFFER] Buffer freezing stopped and buffer cleared", true);
    }
}
