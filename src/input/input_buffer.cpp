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
#include "../include/input/input_freeze.h"
#include <vector>
#include <sstream>
#include <iomanip>
#include <algorithm>

// Define the buffer constants here
// IMPORTANT: The actual input buffer is 180 bytes long. Using 0x180 (384)
// would overwrite into other fields (including the index at 0x260), causing anomalies.
const uint16_t INPUT_BUFFER_SIZE = 180;    // 180 bytes circular buffer (0xB4)
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
std::atomic<int> g_activeFreezePlayer{0};

// Define buffer functions
void FreezeBufferValuesThread(int playerNum) {
    LogOut("[INPUT_BUFFER] Starting buffer freeze thread for P" + std::to_string(playerNum) +
           " startIdx=" + std::to_string(g_frozenBufferStartIndex) +
           " len=" + std::to_string(g_frozenBufferLength) +
           " idxLock=" + (g_indexFreezingActive.load()?std::to_string(g_frozenIndexValue):std::string("off")), true);
    g_activeFreezePlayer.store(playerNum);
    
    uintptr_t initialPlayerPtr = GetPlayerPointer(playerNum);
    if (!initialPlayerPtr) {
        LogOut("[INPUT_BUFFER] Invalid player pointer at thread start, aborting", true);
        g_bufferFreezingActive = false;
        return;
    }
    
    // One-time sanity check: ensure buffer does not overlap index
    static std::atomic<bool> s_layoutChecked{false};
    if (!s_layoutChecked.load()) {
        uintptr_t testPlayer = initialPlayerPtr;
        if (testPlayer) {
            uintptr_t bufStart = testPlayer + INPUT_BUFFER_OFFSET;
            uintptr_t bufEnd   = bufStart + INPUT_BUFFER_SIZE - 1;
            uintptr_t idxAddr  = testPlayer + INPUT_BUFFER_INDEX_OFFSET;
            std::stringstream ss;
            ss << "[INPUT_BUFFER] Layout: start=0x" << std::hex << bufStart
               << " end=0x" << bufEnd << " index=0x" << idxAddr
               << std::dec;
            LogOut(ss.str(), true);
            if (bufEnd >= idxAddr && idxAddr >= bufStart) {
                LogOut("[INPUT_BUFFER][WARN] Buffer region overlaps index! Adjust sizes/offsets.", true);
            }
        }
        s_layoutChecked.store(true);
    }

    // Get the move ID address for monitoring move execution
    uintptr_t base = GetEFZBase();
    uintptr_t moveIDAddr = ResolvePointer(base, 
        (playerNum == 1) ? EFZ_BASE_OFFSET_P1 : EFZ_BASE_OFFSET_P2, 
        MOVE_ID_OFFSET);
    
    short lastMoveID = -1;
    bool moveIDChanged = false;
    int consecutiveMoveTicks = 0;
    const int freezeLimit = 300; // Hard limit on freeze frames
    
    // Get initial move ID for comparison
    SafeReadMemory(moveIDAddr, &lastMoveID, sizeof(short));
    
    // AGGRESSIVE PHASE: Write very frequently at the start
    const int aggressivePhaseFrames = 15;
    for (int i = 0; i < aggressivePhaseFrames && g_bufferFreezingActive; i++) {
        if (g_onlineModeActive.load()) { g_bufferFreezingActive = false; break; }
        // Check player pointer validity
        uintptr_t playerPtr = GetPlayerPointer(playerNum);
        if (!playerPtr || playerPtr != initialPlayerPtr) {
            LogOut("[INPUT_BUFFER] Player pointer changed during aggressive phase, aborting", true);
            g_bufferFreezingActive = false;
            break;
        }
        
        // Write buffer values very frequently (every iteration)
        for (uint16_t j = 0; j < g_frozenBufferLength; j++) {
            uint16_t idx = (g_frozenBufferStartIndex + j) % INPUT_BUFFER_SIZE;
            SafeWriteMemory(playerPtr + INPUT_BUFFER_OFFSET + idx, 
                          &g_frozenBufferValues[j], sizeof(uint8_t));
        }
        
        // Ensure index stays frozen
        if (g_indexFreezingActive) {
            SafeWriteMemory(playerPtr + INPUT_BUFFER_INDEX_OFFSET, 
                          &g_frozenIndexValue, sizeof(uint16_t));
        }
        
        // Check for move ID changes
        short currentMoveID = 0;
        if (SafeReadMemory(moveIDAddr, &currentMoveID, sizeof(short))) {
            if (currentMoveID != lastMoveID) {
                LogOut("[BUFFER_FREEZE] MoveID changed: " + std::to_string(lastMoveID) + 
                      " → " + std::to_string(currentMoveID), true);
                lastMoveID = currentMoveID;
                moveIDChanged = true;
            }
        }
        
        // Very short sleep for aggressive phase
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    
    // NORMAL PHASE: Continue with standard frequency
    int freezeCount = aggressivePhaseFrames;
    while (g_bufferFreezingActive && freezeCount < freezeLimit && !g_isShuttingDown.load()) {
        if (g_onlineModeActive.load()) { LogOut("[INPUT_BUFFER] Online mode active, stopping buffer freeze", true); break; }
        // Check game state and player pointer validity
        GamePhase currentPhase = GetCurrentGamePhase();
        if (currentPhase != GamePhase::Match) {
            LogOut("[INPUT_BUFFER] Game phase changed, stopping buffer freeze", true);
            break;
        }
        
        uintptr_t currentPlayerPtr = GetPlayerPointer(playerNum);
        if (!currentPlayerPtr || currentPlayerPtr != initialPlayerPtr) {
            LogOut("[INPUT_BUFFER] Player pointer changed, stopping buffer freeze", true);
            break;
        }
        
        // Rewrite buffer values every 3rd frame
        if (freezeCount % 3 == 0) {
            for (uint16_t i = 0; i < g_frozenBufferLength; i++) {
                uint16_t idx = (g_frozenBufferStartIndex + i) % INPUT_BUFFER_SIZE;
                SafeWriteMemory(currentPlayerPtr + INPUT_BUFFER_OFFSET + idx, 
                               &g_frozenBufferValues[i], sizeof(uint8_t));
            }
        }
        
        // Always keep index frozen
        if (g_indexFreezingActive) {
            SafeWriteMemory(currentPlayerPtr + INPUT_BUFFER_INDEX_OFFSET, 
                          &g_frozenIndexValue, sizeof(uint16_t));
        }
        
        // Check for move ID changes
        short currentMoveID = 0;
        if (SafeReadMemory(moveIDAddr, &currentMoveID, sizeof(short))) {
            if (currentMoveID != lastMoveID) {
                LogOut("[BUFFER_FREEZE] MoveID changed: " + std::to_string(lastMoveID) + 
                      " → " + std::to_string(currentMoveID), true);
                lastMoveID = currentMoveID;
                moveIDChanged = true;
            }
            
            // Detect successful special move execution (non-zero moveID for multiple ticks)
            if (currentMoveID != 0 && moveIDChanged) {
                consecutiveMoveTicks++;
                if (consecutiveMoveTicks >= 3) {
                    LogOut("[BUFFER_FREEZE] Motion recognized! Move ID: " + 
                          std::to_string(currentMoveID), true);
                    break; // Exit early on successful execution
                }
            } else {
                consecutiveMoveTicks = 0;
            }
        }
        
        freezeCount++;
        // Standard sleep time
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }
    
    // Cleanup phase - DO NOT forcibly wipe entire buffer here; just neutralize trailing inputs
    LogOut("[BUFFER_FREEZE] Buffer freeze thread ended (counter=" + std::to_string(freezeCount) + ")", true);
    if (g_frozenBufferLength > 0) {
        uintptr_t playerPtr = GetPlayerPointer(playerNum);
        if (playerPtr) {
            // Overwrite pattern region with neutral to avoid ghost follow‑ups
            uint8_t zero = 0x00;
            for (uint16_t i = 0; i < g_frozenBufferLength; ++i) {
                uint16_t idx = (g_frozenBufferStartIndex + i) % INPUT_BUFFER_SIZE;
                SafeWriteMemory(playerPtr + INPUT_BUFFER_OFFSET + idx, &zero, sizeof(uint8_t));
            }
            // Push a few neutral frames at index tail
            uint16_t curIdx = 0;
            SafeReadMemory(playerPtr + INPUT_BUFFER_INDEX_OFFSET, &curIdx, sizeof(uint16_t));
            for (int n=0; n<4; ++n) {
                uint16_t w = (curIdx + INPUT_BUFFER_SIZE - n) % INPUT_BUFFER_SIZE;
                uint8_t z = 0x00;
                SafeWriteMemory(playerPtr + INPUT_BUFFER_OFFSET + w, &z, sizeof(uint8_t));
            }
        }
    }
    
    g_bufferFreezingActive = false;
    g_indexFreezingActive = false;
    g_activeFreezePlayer.store(0);
    
    LogOut("[BUFFER_FREEZE] End session P" + std::to_string(playerNum) + " (thread ended)", true);
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
    for (size_t i = 0; i < static_cast<size_t>(length); i++) {
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
    for (size_t i = 0; i < static_cast<size_t>(length); i++) {
        ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(g_frozenBufferValues[i]);
        if (i + 1 < static_cast<size_t>(length)) ss << " ";
    }
    LogOut(ss.str(), true);
    
    // Start freezing
    g_bufferFreezingActive = true;
    g_activeFreezePlayer.store(playerNum);
    g_bufferFreezeThread = std::thread(FreezeBufferValuesThread, playerNum);
    g_bufferFreezeThread.detach();  // Detach to prevent termination
    
    LogOut("[INPUT_BUFFER] Buffer freezing activated for P" + std::to_string(playerNum) + 
           " starting at index " + std::to_string(startIndex) +
           " with length " + std::to_string(length) +
           ", owner=P" + std::to_string(g_activeFreezePlayer.load()), true);
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
        int owner = g_activeFreezePlayer.exchange(0);
        if (owner != 0) {
            LogOut("[INPUT_BUFFER] StopBufferFreezing() called (owner=P" + std::to_string(owner) + ")", true);
        } else {
            LogOut("[INPUT_BUFFER] StopBufferFreezing() called (no active owner)", true);
        }
        
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
                            int w = static_cast<int>(currentIndex) - i;
                            // wrap into [0, INPUT_BUFFER_SIZE)
                            w %= static_cast<int>(INPUT_BUFFER_SIZE);
                            if (w < 0) w += static_cast<int>(INPUT_BUFFER_SIZE);
                            uint16_t writeIndex = static_cast<uint16_t>(w);
                            SafeWriteMemory(playerPtr + INPUT_BUFFER_OFFSET + writeIndex, &neutral, sizeof(uint8_t));
                        }
                    }
                }
            }
        }
        
        LogOut("[INPUT_BUFFER] Buffer freezing stopped and buffer cleared", true);
    }
}
