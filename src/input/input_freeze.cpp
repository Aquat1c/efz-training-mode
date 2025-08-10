#include "../include/input/input_freeze.h"
#include "../include/core/globals.h"
#include "../include/input/input_core.h"
#include "../include/core/memory.h"  
#include "../include/core/logger.h"
#include "../include/core/constants.h"
#include "../include/input/input_buffer.h"
#include "../include/utils/utilities.h"
#include "../include/game/game_state.h"
#include "../include/input/motion_system.h"
#include "../include/input/input_motion.h"  // Add this include for motion functions
#include "../include/game/frame_monitor.h" // Add this include for shared constants
// These functions are implemented in input_buffer.cpp
extern void FreezeBufferValuesThread(int playerNum);
extern bool CaptureAndFreezeBuffer(int playerNum, uint16_t startIndex, uint16_t length);
extern bool FreezeBufferIndex(int playerNum, uint16_t indexValue);
extern void StopBufferFreezing(void);
// Helper to freeze the perfect Dragon Punch motion
bool FreezePerfectDragonPunch(int playerNum) {
    // Stop any existing freeze thread
    StopBufferFreezing();
    
    LogOut("[BUFFER_FREEZE] Starting Dragon Punch buffer freeze for P" + std::to_string(playerNum), true);
    
    // Use the exact values from your successful DP execution
    std::vector<uint8_t> dpMotion = {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x02, 0x02, 0x02, 0x04, 
        0x04, 0x04, 0x26, 0x06, 0x06, 0x06, 0x00, 0x00
    };
    
    // Detailed logging of input sequence
    std::stringstream ss;
    ss << "[BUFFER_FREEZE] DP motion buffer values: ";
    for (size_t i = 0; i < dpMotion.size(); i++) {
        if (dpMotion[i] != 0) {
            ss << std::hex << std::setw(2) << std::setfill('0') 
               << static_cast<int>(dpMotion[i]) << "(" << DecodeInputMask(dpMotion[i]) << ") ";
        } else if (i > 0 && dpMotion[i-1] != 0) {
            ss << "00 ";  // Only show zeros that follow non-zero values
        }
    }
    LogOut(ss.str(), true);
    
    // Setup the frozen buffer values
    g_frozenBufferValues = dpMotion;
    g_frozenBufferLength = static_cast<uint16_t>(dpMotion.size());
    g_frozenBufferStartIndex = 128; // Place in middle of buffer for visibility
    
    // Set index to point at position 148 (128+20)
    g_frozenIndexValue = (g_frozenBufferStartIndex + 20) % INPUT_BUFFER_SIZE;
    g_indexFreezingActive = true;
    
    // Start freezing thread
    g_bufferFreezingActive = true;
    g_bufferFreezeThread = std::thread(FreezeBufferValuesThread, playerNum);
    g_bufferFreezeThread.detach();  // Detach to prevent termination
    
    LogOut("[BUFFER_FREEZE] Perfect Dragon Punch motion freezing activated at index " + 
           std::to_string(g_frozenIndexValue), true);
    return true;
}

// Enhanced version with diagnostic dump and adjusted index placement
bool FreezePerfectDragonPunchEnhanced(int playerNum) {
    // Stop any existing freeze thread
    StopBufferFreezing();
    
    LogOut("[BUFFER_DEBUG] Starting enhanced DP buffer freeze for P" + std::to_string(playerNum), true);
    
    // Get player pointer
    uintptr_t playerPtr = GetPlayerPointer(playerNum);
    if (!playerPtr) {
        LogOut("[BUFFER_DEBUG] Failed to get player pointer", true);
        return false;
    }
    
    // Read current buffer index for reference
    uint16_t currentIndex = 0;
    if (!SafeReadMemory(playerPtr + INPUT_BUFFER_INDEX_OFFSET, &currentIndex, sizeof(uint16_t))) {
        LogOut("[BUFFER_DEBUG] Failed to read current buffer index", true);
        return false;
    }
    
    // First dump the current buffer contents for diagnostic purposes
    DumpInputBuffer(playerNum);
    
    // Use the exact DP motion sequence from the console dump
    std::vector<uint8_t> dpMotion = {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x02, 0x02, 0x02, 0x04, 
        0x04, 0x04, 0x26, 0x06, 0x06, 0x00, 0x00, 0x00
    };
    
    // Calculate index based on current buffer position
    // We want to place the motion sequence just before the current read position
    g_frozenBufferStartIndex = (currentIndex + INPUT_BUFFER_SIZE - dpMotion.size()) % INPUT_BUFFER_SIZE;
    g_frozenBufferValues = dpMotion;
    g_frozenBufferLength = static_cast<uint16_t>(dpMotion.size());
    
    // Set the index to point at the start of the important part of the sequence
    g_frozenIndexValue = (g_frozenBufferStartIndex + 20) % INPUT_BUFFER_SIZE;
    g_indexFreezingActive = true;
    
    LogOut("[BUFFER_DEBUG] Current buffer index: " + std::to_string(currentIndex), true);
    LogOut("[BUFFER_DEBUG] Setting buffer start at: " + std::to_string(g_frozenBufferStartIndex), true);
    LogOut("[BUFFER_DEBUG] Setting frozen index to: " + std::to_string(g_frozenIndexValue), true);
    
    // Detailed logging of input sequence
    std::stringstream ss;
    ss << "[BUFFER_DEBUG] DP motion sequence: ";
    for (size_t i = 0; i < dpMotion.size(); i++) {
        ss << std::hex << std::setw(2) << std::setfill('0') 
           << static_cast<int>(dpMotion[i]) << "(" << DecodeInputMask(dpMotion[i]) << ") ";
    }
    LogOut(ss.str(), true);
    
    // Start freezing thread
    g_bufferFreezingActive = true;
    g_bufferFreezeThread = std::thread([playerNum]() {
        LogOut("[BUFFER_DEBUG] Enhanced freeze thread starting - won't terminate until Numpad 5 pressed", true);
        FreezeBufferValuesThread(playerNum);
    });
    g_bufferFreezeThread.detach();  // Detach to prevent termination
    
    LogOut("[BUFFER_DEBUG] Enhanced DP buffer freeze activated for P" + 
           std::to_string(playerNum), true);
    return true;
}

bool ComboFreezeDP(int playerNum) {
    // Stop any existing freeze thread
    StopBufferFreezing();
    
    LogOut("[BUFFER_COMBO] Starting CheatEngine-style DP freeze for P" + std::to_string(playerNum), true);
    
    uintptr_t playerPtr = GetPlayerPointer(playerNum);
    if (!playerPtr) {
        LogOut("[BUFFER_COMBO] Failed to get player pointer", true);
        return false;
    }

    // Read initial buffer state
    uint16_t currentIndex = 0;
    SafeReadMemory(playerPtr + INPUT_BUFFER_INDEX_OFFSET, &currentIndex, sizeof(uint16_t));
    LogOut("[BUFFER_COMBO] Initial buffer index: " + std::to_string(currentIndex), true);
    
    // This is the exact CheatEngine pattern that works
    std::vector<uint8_t> dpMotion = {
        0x02, 0x02, 0x02,  // LEFT x3
        0x04, 0x04, 0x04,  // DOWN x3
        0x26,              // DOWN+LEFT+BUTTON
        0x06, 0x06, 0x06, 0x06, // DOWN+LEFT x4
        0x26, 0x26, 0x26, 0x26  // DOWN+LEFT+BUTTON x4
    };
    
    // Target index where we want to maintain the pattern
    g_frozenIndexValue = 149;
    g_frozenBufferValues = dpMotion;
    g_frozenBufferLength = static_cast<uint16_t>(dpMotion.size());
    
    // Enable buffer and index manipulation
    g_indexFreezingActive = true;
    g_bufferFreezingActive = true;
    
    // Start buffer freeze thread
    g_bufferFreezeThread = std::thread([playerNum, dpMotion]() {
        LogOut("[BUFFER_COMBO] Starting DP pattern buffer freeze thread", true);
        uintptr_t playerPtr = GetPlayerPointer(playerNum);
        if (!playerPtr) return;
        
        int counter = 0;
        uint16_t lastIndex = 0;
        short lastMoveID = -1;
        
        // Main freeze loop
        while (g_bufferFreezingActive) {
            // Read current index and moveID for monitoring
            uint16_t currentIndex = 0;
            short moveID = 0;
            SafeReadMemory(playerPtr + INPUT_BUFFER_INDEX_OFFSET, &currentIndex, sizeof(uint16_t));
            
            // Optional: Read moveID for logging
            uintptr_t moveIDAddr = ResolvePointer(GetEFZBase(), 
                (playerNum == 1) ? EFZ_BASE_OFFSET_P1 : EFZ_BASE_OFFSET_P2, 
                MOVE_ID_OFFSET);
            if (moveIDAddr) {
                SafeReadMemory(moveIDAddr, &moveID, sizeof(short));
                if (moveID != lastMoveID && moveID != 0) {
                    LogOut("[BUFFER_COMBO] MoveID changed: " + std::to_string(lastMoveID) + 
                          " → " + std::to_string(moveID), true);
                    lastMoveID = moveID;
                }
            }
            
            // Allow index to float in 149-152 range, only reset if it's outside
            if (currentIndex < 147 || currentIndex > 152) {
                SafeWriteMemory(playerPtr + INPUT_BUFFER_INDEX_OFFSET, &g_frozenIndexValue, sizeof(uint16_t));
                currentIndex = g_frozenIndexValue;
            }
            
            // Always write the pattern at multiple positions relative to the current index
            // This ensures it's found no matter which exact index the game checks
            for (int offset = -2; offset <= 2; offset++) {
                int basePos = (currentIndex - dpMotion.size() / 2 + offset) % INPUT_BUFFER_SIZE;
                if (basePos < 0) basePos += INPUT_BUFFER_SIZE;
                
                // Write the pattern
                for (size_t i = 0; i < dpMotion.size(); i++) {
                    uint16_t writeIndex = (basePos + i) % INPUT_BUFFER_SIZE;
                    SafeWriteMemory(playerPtr + INPUT_BUFFER_OFFSET + writeIndex, 
                                  &dpMotion[i], sizeof(uint8_t));
                }
            }
            
            // Also make sure we write the exact pattern at fixed locations that are known to work
            const int knownGoodStart = 144;
            for (size_t i = 0; i < dpMotion.size(); i++) {
                SafeWriteMemory(playerPtr + INPUT_BUFFER_OFFSET + knownGoodStart + i, 
                              &dpMotion[i], sizeof(uint8_t));
            }
            
            // Log periodically or on index change
            if (currentIndex != lastIndex || counter % 100 == 0) {
                LogOut("[BUFFER_COMBO] Maintaining buffer pattern at index: " + 
                      std::to_string(currentIndex), true);
                lastIndex = currentIndex;
            }
            
            counter++;
            
            // Use short sleep for responsiveness
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });
    g_bufferFreezeThread.detach();
    
    LogOut("[BUFFER_COMBO] DP buffer pattern freeze activated", true);
    return true;
}

bool FreezeBufferForMotion(int playerNum, int motionType, int buttonMask, int optimalIndex) {
    // Stop any existing freeze thread
    StopBufferFreezing();
    
    // Add a short delay to ensure previous thread is truly stopped
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    
    LogOut("[BUFFER_FREEZE] Starting buffer freeze for motion " + GetMotionTypeName(motionType) + " (P" + std::to_string(playerNum) + ")", true);
    
    // OLD (caused error):
    // BeginBufferFreezeSession(playerNum, GetMotionTypeName(motionType));
    // FIX:
    {
        std::string motionLabel = GetMotionTypeName(motionType);
        BeginBufferFreezeSession(playerNum, motionLabel.c_str());
    }
    
    uintptr_t playerPtr = GetPlayerPointer(playerNum);
    if (!playerPtr) {
        LogOut("[BUFFER_FREEZE] Failed to get player pointer", true);
        return false;
    }

    // Read initial buffer state
    uint16_t currentIndex = 0;
    SafeReadMemory(playerPtr + INPUT_BUFFER_INDEX_OFFSET, &currentIndex, sizeof(uint16_t));
    LogOut("[BUFFER_FREEZE] Initial buffer index: " + std::to_string(currentIndex), true);
    
    // Get player's facing direction
    bool facingRight = GetPlayerFacingDirection(playerNum);
    LogOut("[BUFFER_FREEZE] Player " + std::to_string(playerNum) + " facing " + (facingRight ? "right" : "left"), true);
    
    // Generate buffer pattern based on motion type
    std::vector<uint8_t> motionPattern;
    
    // Helper to adjust directions based on facing
    auto adjustDirection = [facingRight](uint8_t dirMask) -> uint8_t {
        if (facingRight) {
            return dirMask;
        } else {
            // Flip horizontal directions when facing left
            uint8_t result = dirMask & ~(GAME_INPUT_LEFT | GAME_INPUT_RIGHT); // Clear left/right bits
            
            // Swap left and right
            if (dirMask & GAME_INPUT_LEFT) result |= GAME_INPUT_RIGHT;
            if (dirMask & GAME_INPUT_RIGHT) result |= GAME_INPUT_LEFT;
            
            return result;
        }
    };
    
    // Populate the buffer pattern based on motion type
    switch (motionType) {
        case MOTION_236A: case MOTION_236B: case MOTION_236C: {
            // QCF: Down, Down-Forward, Forward + Button
            uint8_t down = adjustDirection(GAME_INPUT_DOWN);
            uint8_t downFwd = adjustDirection(GAME_INPUT_DOWN | GAME_INPUT_RIGHT);
            uint8_t fwd = adjustDirection(GAME_INPUT_RIGHT);
            uint8_t btn = buttonMask;
            
            // Pattern: Neutral → Down → Down-Forward → Forward+Button
            motionPattern = {
                u8(0x00), u8(0x00), u8(0x00), // Neutral padding
                u8(down), u8(down), u8(down), // Down x3
                u8(downFwd), u8(downFwd), u8(downFwd), // Down-Forward x3
                u8(fwd | btn), u8(fwd | btn), u8(fwd | btn) // Forward+Button x3
            };
            break;
        }
        
        case MOTION_623A: case MOTION_623B: case MOTION_623C: {
            // DP: Forward, Down, Down-Forward + Button
            uint8_t fwd = adjustDirection(GAME_INPUT_RIGHT);
            uint8_t down = adjustDirection(GAME_INPUT_DOWN);
            uint8_t downFwd = adjustDirection(GAME_INPUT_DOWN | GAME_INPUT_RIGHT);
            uint8_t btn = buttonMask;
            
            // Use the exact pattern that works from CheatEngine
            motionPattern = {
                u8(0x00), u8(0x00), u8(0x00), // Neutral padding
                u8(fwd), u8(fwd), u8(fwd), // Forward x3
                u8(down), u8(down), u8(down), // Down x3
                u8(downFwd | btn), // Down-Forward+Button x1
                u8(downFwd), u8(downFwd), u8(downFwd), u8(downFwd), // Down-Forward x4
                u8(downFwd | btn), u8(downFwd | btn), u8(downFwd | btn), u8(downFwd | btn) // Down-Forward+Button x4
            };
            break;
        }
        
        case MOTION_214A: case MOTION_214B: case MOTION_214C: {
            // QCB: Down, Down-Back, Back + Button
            uint8_t down = adjustDirection(GAME_INPUT_DOWN);
            uint8_t downBack = adjustDirection(GAME_INPUT_DOWN | GAME_INPUT_LEFT);
            uint8_t back = adjustDirection(GAME_INPUT_LEFT);
            uint8_t btn = buttonMask;
            
            // Pattern: Neutral → Down → Down-Back → Back+Button
            motionPattern = {
                u8(0x00), u8(0x00), u8(0x00), // Neutral padding
                u8(down), u8(down), u8(down), // Down x3
                u8(downBack), u8(downBack), // Down-Back x2
                u8(back | btn), u8(back | btn), u8(back | btn), u8(back | btn) // Back+Button x4
            };
            break;
        }
        
        case MOTION_41236A: case MOTION_41236B: case MOTION_41236C: {
            // HCF: Back, Down-Back, Down, Down-Forward, Forward + Button
            uint8_t back = adjustDirection(GAME_INPUT_LEFT);
            uint8_t downBack = adjustDirection(GAME_INPUT_DOWN | GAME_INPUT_LEFT);
            uint8_t down = adjustDirection(GAME_INPUT_DOWN);
            uint8_t downFwd = adjustDirection(GAME_INPUT_DOWN | GAME_INPUT_RIGHT);
            uint8_t fwd = adjustDirection(GAME_INPUT_RIGHT);
            uint8_t btn = buttonMask;
            
            // Pattern for HCF
            motionPattern = {
                u8(back), u8(back), // Back x2
                u8(downBack), u8(downBack), // Down-Back x2
                u8(down), u8(down), // Down x2
                u8(downFwd), u8(downFwd), // Down-Forward x2
                u8(fwd | btn), u8(fwd | btn), u8(fwd | btn), u8(fwd | btn) // Forward+Button x4
            };
            break;
        }
        
        case MOTION_63214A: case MOTION_63214B: case MOTION_63214C: {
            // HCB: Forward, Down-Forward, Down, Down-Back, Back + Button
            uint8_t fwd = adjustDirection(GAME_INPUT_RIGHT);
            uint8_t downFwd = adjustDirection(GAME_INPUT_DOWN | GAME_INPUT_RIGHT);
            uint8_t down = adjustDirection(GAME_INPUT_DOWN);
            uint8_t downBack = adjustDirection(GAME_INPUT_DOWN | GAME_INPUT_LEFT);
            uint8_t back = adjustDirection(GAME_INPUT_LEFT);
            uint8_t btn = buttonMask;
            
            // Pattern for HCB
            motionPattern = {
                u8(fwd), u8(fwd), // Forward x2
                u8(downFwd), u8(downFwd), // Down-Forward x2
                u8(down), u8(down), // Down x2
                u8(downBack), u8(downBack), // Down-Back x2
                u8(back | btn), u8(back | btn), u8(back | btn), u8(back | btn) // Back+Button x4
            };
            break;
        }
        
        case MOTION_421A: case MOTION_421B: case MOTION_421C: {
            // Half Circle Back Down: Down, Down-Back, Back + Button
            uint8_t down = adjustDirection(GAME_INPUT_DOWN);
            uint8_t downBack = adjustDirection(GAME_INPUT_DOWN | GAME_INPUT_LEFT);
            uint8_t back = adjustDirection(GAME_INPUT_LEFT);
            uint8_t btn = buttonMask;
            
            // Pattern for 421
            motionPattern = {
                u8(0x00), u8(0x00), u8(0x00), // Neutral padding
                u8(down), u8(down), u8(down), // Down x3
                u8(downBack), u8(downBack), // Down-Back x2
                u8(back | btn), u8(back | btn), u8(back | btn), u8(back | btn) // Back+Button x4
            };
            break;
        }
        
        case ACTION_FORWARD_DASH: {
            // Forward, Neutral, Forward
            uint8_t fwd = adjustDirection(GAME_INPUT_RIGHT);
            
            motionPattern = {
                u8(0x00), u8(0x00), u8(0x00), // Neutral padding
                u8(fwd), u8(fwd), // Forward x2
                u8(0x00), u8(0x00), // Neutral x2
                u8(fwd), u8(fwd), u8(fwd), u8(fwd) // Forward x4
            };
            break;
        }
        
        case ACTION_BACK_DASH: {
            // Back, Neutral, Back
            uint8_t back = adjustDirection(GAME_INPUT_LEFT);
            
            motionPattern = {
                u8(0x00), u8(0x00), u8(0x00), // Neutral padding
                u8(back), u8(back), // Back x2
                u8(0x00), u8(0x00), // Neutral x2
                u8(back), u8(back), u8(back), u8(back) // Back x4
            };
            break;
        }
        
        default:
            // For unknown motion types, default to a simple forward+button pattern
            uint8_t fwd = adjustDirection(GAME_INPUT_RIGHT);
            motionPattern = {
                u8(0x00), u8(0x00), u8(0x00), u8(0x00),
                u8(fwd | buttonMask), u8(fwd | buttonMask), u8(fwd | buttonMask), u8(fwd | buttonMask)
            };
            LogOut("[BUFFER_FREEZE] Warning: Unknown motion type " + std::to_string(motionType) + 
                  ", using default pattern", true);
    }
    
    // Diagnostic logging of the motion pattern
    std::stringstream ss;
    ss << "[BUFFER_FREEZE] Pattern values for " << GetMotionTypeName(motionType) << ": ";
    for (size_t i = 0; i < motionPattern.size(); i++) {
        ss << std::hex << std::setw(2) << std::setfill('0') 
           << static_cast<int>(motionPattern[i]) << "(" 
           << DecodeInputMask(motionPattern[i]) << ") ";
    }
    LogOut(ss.str(), true);
    
    // Target index where we want to maintain the pattern
    g_frozenIndexValue = optimalIndex;
    g_frozenBufferValues = motionPattern;
    g_frozenBufferLength = motionPattern.size();
    
    // Enable buffer and index manipulation
    g_indexFreezingActive = true;
    g_bufferFreezingActive = true;
    
    // Launch the freeze thread
    g_bufferFreezeThread = std::thread([playerNum, motionType, motionPattern, optimalIndex]() {
        LogOut("[BUFFER_FREEZE] Starting pattern buffer freeze thread for " + 
              GetMotionTypeName(motionType), true);
        
        uintptr_t playerPtr = GetPlayerPointer(playerNum);
        if (!playerPtr) {
            LogOut("[BUFFER_FREEZE] Failed to get player pointer, stopping freeze", true);
            g_bufferFreezingActive = false;
            return;
        }
        
        // Store initial player pointer for validation
        uintptr_t initialPlayerPtr = playerPtr;
        
        int counter = 0;
        uint16_t lastIndex = 0;
        short lastMoveID = -1;
        short moveID = 0;
        bool moveDetected = false;
        
        // Add a timeout to ensure the thread doesn't run forever
        int timeoutCounter = 0;
        const int MAX_TIMEOUT = 300; // 5 seconds (60 frames per second)
        
        // Main freeze loop with multiple exit conditions
        while (g_bufferFreezingActive && timeoutCounter++ < MAX_TIMEOUT && !g_isShuttingDown.load()) {
            // CRITICAL: Check game phase FIRST
            GamePhase phase = GetCurrentGamePhase();
            if (phase != GamePhase::Match) {
                LogOut("[BUFFER_FREEZE] Game no longer in Match phase, aborting", true);
                break;
            }
            
            // Validate player pointer hasn't changed
            uintptr_t currentPlayerPtr = GetPlayerPointer(playerNum);
            if (!currentPlayerPtr || currentPlayerPtr != initialPlayerPtr) {
                LogOut("[BUFFER_FREEZE] Player pointer changed/invalidated, aborting", true);
                break;
            }
            
            // Safety check game mode
            if (!IsValidGameMode(GetCurrentGameMode())) {
                LogOut("[BUFFER_FREEZE] Invalid game mode, aborting", true);
                break;
            }
            
            // Read current index
            uint16_t currentIndex = 0;
            if (!SafeReadMemory(playerPtr + INPUT_BUFFER_INDEX_OFFSET, &currentIndex, sizeof(uint16_t))) {
                LogOut("[BUFFER_FREEZE] Failed to read buffer index, aborting", true);
                break;
            }
            
            // Write the pattern
            for (size_t i = 0; i < motionPattern.size(); i++) {
                uint16_t writePos = (optimalIndex + i) % INPUT_BUFFER_SIZE;
                uintptr_t addr = playerPtr + INPUT_BUFFER_OFFSET + writePos;
                
                if (!SafeWriteMemory(addr, &motionPattern[i], sizeof(uint8_t))) {
                    LogOut("[BUFFER_FREEZE] Failed to write to buffer, aborting", true);
                    g_bufferFreezingActive = false;
                    break;
                }
            }
            
            // Check for move execution
            uintptr_t moveIDAddr = ResolvePointer(GetEFZBase(), 
                (playerNum == 1) ? EFZ_BASE_OFFSET_P1 : EFZ_BASE_OFFSET_P2, 
                MOVE_ID_OFFSET);
                
            if (moveIDAddr && SafeReadMemory(moveIDAddr, &moveID, sizeof(short))) {
                if (moveID != lastMoveID) {
                    LogOut("[BUFFER_FREEZE] MoveID changed: " + std::to_string(lastMoveID) + 
                          " → " + std::to_string(moveID), true);
                    
                    // If move executed successfully, we can stop
                    if (moveID > 0 && counter > 30) {
                        LogOut("[BUFFER_FREEZE] Motion recognized! Move ID: " + 
                              std::to_string(moveID), true);
                        break;
                    }
                    
                    lastMoveID = moveID;
                }
            }
            
            counter++;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        
        // Cleanup
        LogOut("[BUFFER_FREEZE] Buffer freeze thread for " + GetMotionTypeName(motionType) + 
               " ended (counter=" + std::to_string(counter) + ")", true);
        
        g_bufferFreezingActive = false;
        EndBufferFreezeSession(playerNum, "thread ended");
    });
    
    // Keep thread joinable instead of detaching it
    if (g_bufferFreezeThread.joinable()) {
        g_bufferFreezeThread.detach();
    }
    
    return true;
}

namespace {
    struct FreezeSessionState {
        std::atomic<bool> active{false};
        std::atomic<bool> threadRunning{false};
        uint16_t originalIndex{0};
        bool originalIndexValid{false};
    };
    FreezeSessionState g_freezeSession[3]; // 1,2 (ignore 0)
}

// Clear (zero) entire buffer + index safely
void ClearPlayerInputBuffer(int playerNum) {
    uintptr_t playerPtr = GetPlayerPointer(playerNum);
    if (!playerPtr) return;
    uint8_t zero = 0x00;
    for (uint16_t i = 0; i < INPUT_BUFFER_SIZE; ++i) {
        SafeWriteMemory(playerPtr + INPUT_BUFFER_OFFSET + i, &zero, sizeof(uint8_t));
    }
    uint16_t idxZero = 0;
    SafeWriteMemory(playerPtr + INPUT_BUFFER_INDEX_OFFSET, &idxZero, sizeof(uint16_t));
    LogOut(std::string("[BUFFER_FREEZE] Cleared full buffer & index for P") + std::to_string(playerNum), true);
}

void BeginBufferFreezeSession(int playerNum, std::string_view label) {
    StopBufferFreezing();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    auto &s = g_freezeSession[playerNum];
    s.active.store(true);
    s.threadRunning.store(false);
    s.originalIndexValid = false;
    uintptr_t playerPtr = GetPlayerPointer(playerNum);
    if (playerPtr) {
        uint16_t curIdx = 0;
        if (SafeReadMemory(playerPtr + INPUT_BUFFER_INDEX_OFFSET, &curIdx, sizeof(uint16_t))) {
            s.originalIndex = curIdx;
            s.originalIndexValid = true;
        }
    }
    LogOut(std::string("[BUFFER_FREEZE] Begin session (") + (label.empty() ? "" : std::string(label)) +
           ") P" + std::to_string(playerNum), true);
}

void EndBufferFreezeSession(int playerNum, const char* reason, bool clearGlobals) {
    auto &s = g_freezeSession[playerNum];
    if (!s.active.load()) return;

    // Signal stop
    g_bufferFreezingActive = false;
    g_indexFreezingActive  = false;

    // Wait briefly if a thread may still be winding down
    for (int i=0; i<20 && s.threadRunning.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    ClearPlayerInputBuffer(playerNum);

    s.active.store(false);
    s.threadRunning.store(false);

    if (clearGlobals) {
        g_frozenBufferLength = 0;
        g_frozenBufferValues.clear();
    }

    LogOut(std::string("[BUFFER_FREEZE] End session P") + std::to_string(playerNum) +
           " (" + (reason?reason:"no reason") + ")", true);
}