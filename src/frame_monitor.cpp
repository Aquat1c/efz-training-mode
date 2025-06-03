#include <fstream>
#include <iomanip>
#include <chrono>
#include "../include/frame_monitor.h"
#include "../include/constants.h"
#include "../include/utilities.h"
#include "../include/memory.h"
#include "../include/logger.h"
#include <deque>
#include <vector>
#include <chrono>
#include <sstream>
#include <algorithm>
#include <thread>
#include <atomic>
#include <iomanip>

MonitorState state = Idle;

// Add this with the other global variable definitions (near the top of the file)
short initialBlockstunMoveID = -1;

// Add these helper functions before FrameDataMonitor function

bool IsHitstun(short moveID) {
    return (moveID >= STAND_HITSTUN_START && moveID <= STAND_HITSTUN_END) || 
           (moveID >= CROUCH_HITSTUN_START && moveID <= CROUCH_HITSTUN_END) ||
           moveID == SWEEP_HITSTUN;
}

bool IsLaunched(short moveID) {
    // Updated to check the full range of launched hitstun states
    return moveID >= LAUNCHED_HITSTUN_START && moveID <= LAUNCHED_HITSTUN_END;
}

bool IsAirtech(short moveID) {
    return moveID == FORWARD_AIRTECH || moveID == BACKWARD_AIRTECH;
}

bool IsGroundtech(short moveID) {
    return moveID == GROUNDTECH_START ||  // 98
           moveID == GROUNDTECH_END ||    // 99  
           moveID == 96;                  // Additional groundtech state
}

bool IsFrozen(short moveID) {
    return moveID >= FROZEN_STATE_START && moveID <= FROZEN_STATE_END;
}

bool IsSpecialStun(short moveID) {
    return moveID == FIRE_STATE || moveID == ELECTRIC_STATE || 
           (moveID >= FROZEN_STATE_START && moveID <= FROZEN_STATE_END);
}

// Add this new function after IsSpecialStun()
bool CanAirtech(short moveID) {
    // Check for standard launched hitstun
    bool isLaunched = moveID >= LAUNCHED_HITSTUN_START && moveID <= LAUNCHED_HITSTUN_END;
    
    // Also allow airtech from fire and electric states
    bool isFireOrElectric = moveID == FIRE_STATE || moveID == ELECTRIC_STATE;
    
    return isLaunched || isFireOrElectric;
}

// Function to get untech value
short GetUntechValue(uintptr_t base, int player) {
    short untechValue = 0;
    uintptr_t baseOffset = (player == 1) ? EFZ_BASE_OFFSET_P1 : EFZ_BASE_OFFSET_P2;
    uintptr_t untechAddr = ResolvePointer(base, baseOffset, UNTECH_OFFSET);
    
    if (untechAddr) {
        memcpy(&untechValue, (void*)untechAddr, sizeof(short));
    }
    
    return untechValue;
}

// Add variables to track airtech state for both players
bool p1InAirHitstun = false;
bool p2InAirHitstun = false;
int p1LastHitstunFrame = -1;
int p2LastHitstunFrame = -1;

// Apply airtech for a specific player
void ApplyAirtech(uintptr_t moveIDAddr, int playerNum, int frameNum) {
    if (!moveIDAddr) return;
    
    // Check player's Y position before applying airtech
    double yPos = playerNum == 1 ? displayData.y1 : displayData.y2;
    double xPos = playerNum == 1 ? displayData.x1 : displayData.x2;
    
    // At Y=-1.0 characters can airtech (confirmed by frame-by-frame testing)
    const double MIN_AIRTECH_HEIGHT = -1.1;
    
    // Only apply airtech if character is high enough
    if (yPos <= MIN_AIRTECH_HEIGHT) {
        uintptr_t base = GetEFZBase();
        if (!base) return;
        
        // 1. Apply the airtech moveID
        short airtechMoveID = (autoAirtechDirection == 0) ? FORWARD_AIRTECH : BACKWARD_AIRTECH;
        WriteGameMemory(moveIDAddr, &airtechMoveID, sizeof(short));
        
        // 2. Calculate physics values based on airtech direction
        double xVelocity, yVelocity;
        
        // Values based on your test data - only keep velocities
        if (autoAirtechDirection == 0) { // Forward
            xVelocity = 1.0;
            yVelocity = -2.21;
        } else { // Backward
            xVelocity = -1.0;
            yVelocity = -2.21;
        }
        
        // 3. Apply physics values
        uintptr_t baseOffset = (playerNum == 1) ? EFZ_BASE_OFFSET_P1 : EFZ_BASE_OFFSET_P2;
        
        // Write X velocity
        uintptr_t xVelAddr = ResolvePointer(base, baseOffset, 0x30);
        if (xVelAddr) {
            WriteGameMemory(xVelAddr, &xVelocity, sizeof(double));
        }
        
        // Write Y velocity
        uintptr_t yVelAddr = ResolvePointer(base, baseOffset, 0x38);
        if (yVelAddr) {
            WriteGameMemory(yVelAddr, &yVelocity, sizeof(double));
        }
        
        // Only log if detailed logging is enabled
        if (detailedLogging) {
            LogOut("[AUTO-AIRTECH] Applied " + 
                   std::string(autoAirtechDirection == 0 ? "forward" : "backward") + 
                   " airtech for P" + std::to_string(playerNum) + 
                   " at frame " + std::to_string(frameNum) + 
                   " (Y position: " + std::to_string(yPos) + 
                   ", X velocity: " + std::to_string(xVelocity) + 
                   ", Y velocity: " + std::to_string(yVelocity) + ")", true);
        } else {
            // Simplified message for normal mode
            LogOut("[SYSTEM] Applied auto-airtech for P" + std::to_string(playerNum), true);
        }
    } else {
        LogOut("[AUTO-AIRTECH] Skipped airtech for P" + std::to_string(playerNum) + 
               " - too close to ground (Y position: " + std::to_string(yPos) + 
               ", minimum: " + std::to_string(MIN_AIRTECH_HEIGHT) + ")", true);
    }
}

// Monitor auto-airtech state
void MonitorAutoAirtech(short moveID1, short moveID2) {
    static bool prevEnabled = false;
    static int prevDirection = -1;
    
    // Check if settings have changed
    bool directionChanged = prevDirection != autoAirtechDirection.load();
    bool enabledChanged = prevEnabled != autoAirtechEnabled.load();
    
    if (enabledChanged || directionChanged) {
        if (autoAirtechEnabled) {
            // Enable auto-airtech with current direction
            ApplyAirtechPatches();
        } else {
            // Disable auto-airtech
            RemoveAirtechPatches();
        }
        
        // Fix: Load the atomic values into regular bool/int variables
        prevEnabled = autoAirtechEnabled.load();
        prevDirection = autoAirtechDirection.load();
    }
    
    // Log when players enter airtech-eligible states
    static bool p1WasAirtechable = false;
    static bool p2WasAirtechable = false;
    
    bool p1Airtechable = CanAirtech(moveID1);
    bool p2Airtechable = CanAirtech(moveID2);
    
    // P1 entered a state where airtech is possible
    if (p1Airtechable && !p1WasAirtechable) {
        std::string stateType = "air hitstun";
        if (moveID1 == FIRE_STATE) stateType = "fire state";
        else if (moveID1 == ELECTRIC_STATE) stateType = "electric state";
        
        // Only log if detailed logging is enabled
        if (detailedLogging) {
            LogOut("[AUTO-AIRTECH] P1 entered " + stateType + " at frame " + 
                   std::to_string(frameCounter) + 
                   (autoAirtechEnabled ? " (auto-airtech active)" : ""), 
                   true); // Changed from "detailedLogging" to "true" for when detailed is enabled
        }
    }
    
    // P2 entered a state where airtech is possible
    if (p2Airtechable && !p2WasAirtechable) {
        std::string stateType = "air hitstun";
        if (moveID2 == FIRE_STATE) stateType = "fire state";
        else if (moveID2 == ELECTRIC_STATE) stateType = "electric state";
        
        LogOut("[AUTO-AIRTECH] P2 entered " + stateType + " at frame " + 
               std::to_string(frameCounter) + 
               (autoAirtechEnabled ? " (auto-airtech active)" : ""), 
               detailedLogging);
    }
    
    p1WasAirtechable = p1Airtechable;
    p2WasAirtechable = p2Airtechable;
}

// Original bytes for patches
char originalEnableBytes[2] = {0x74, 0x71}; // Default values from constants
char originalForwardBytes[2] = {0x75, 0x24};
char originalBackwardBytes[2] = {0x75, 0x21};
bool patchesApplied = false;

// Apply patches to enable auto-airtech
void ApplyAirtechPatches() {
    if (patchesApplied) return;
    
    uintptr_t base = GetEFZBase();
    if (!base) return;
    
    // Store original bytes before patching
    memcpy(originalEnableBytes, (void*)(base + AIRTECH_ENABLE_ADDR), sizeof(originalEnableBytes));
    memcpy(originalForwardBytes, (void*)(base + AIRTECH_FORWARD_ADDR), sizeof(originalForwardBytes));
    memcpy(originalBackwardBytes, (void*)(base + AIRTECH_BACKWARD_ADDR), sizeof(originalBackwardBytes));
    
    // Apply patches
    bool mainPatchSuccess = NopMemory(base + AIRTECH_ENABLE_ADDR, 2);
    
    // Apply direction-specific patch based on settings
    if (autoAirtechDirection == 0) {
        // Forward airtech
        NopMemory(base + AIRTECH_FORWARD_ADDR, 2);
    } else {
        // Backward airtech
        NopMemory(base + AIRTECH_BACKWARD_ADDR, 2);
    }
    
    if (mainPatchSuccess) {
        patchesApplied = true;
        LogOut("[AUTO-AIRTECH] Patches applied - automatic " + 
               std::string(autoAirtechDirection == 0 ? "forward" : "backward") + 
               " airtech enabled", true);
    } else {
        LogOut("[AUTO-AIRTECH] Failed to apply patches", true);
    }
}

// Remove patches and restore original bytes
void RemoveAirtechPatches() {
    if (!patchesApplied) return;
    
    uintptr_t base = GetEFZBase();
    if (!base) return;
    
    // Restore original bytes
    PatchMemory(base + AIRTECH_ENABLE_ADDR, originalEnableBytes, 2);
    PatchMemory(base + AIRTECH_FORWARD_ADDR, originalForwardBytes, 2);
    PatchMemory(base + AIRTECH_BACKWARD_ADDR, originalBackwardBytes, 2);
    
    patchesApplied = false;
    LogOut("[AUTO-AIRTECH] Patches removed - automatic airtech disabled", true);
}

// Variables to track jump state for each player
enum JumpState {
    Grounded,
    Rising,
    Falling,
    Landing,
    NeutralFrame
};

JumpState p1JumpState = Grounded;
JumpState p2JumpState = Grounded;
int p1StateFrames = 0; // Track how many frames in current state
int p2StateFrames = 0;

// Apply jump moveID to a player
void ApplyJump(uintptr_t moveIDAddr, int playerNum, int jumpType) {
    if (!moveIDAddr) return;
    
    short jumpMoveID;
    
    // Determine which jump type to use
    switch (jumpType) {
        case 0: // Straight
            jumpMoveID = STRAIGHT_JUMP_ID;
            break;
        case 1: // Forward
            jumpMoveID = FORWARD_JUMP_ID;
            break;
        case 2: // Backward
            jumpMoveID = BACKWARD_JUMP_ID;
            break;
        default:
            jumpMoveID = STRAIGHT_JUMP_ID;
    }
    
    // Write the jump moveID to memory
    WriteGameMemory(moveIDAddr, &jumpMoveID, sizeof(short));
    
    LogOut("[AUTO-JUMP] Applied " + 
           std::string(jumpType == 0 ? "straight" : (jumpType == 1 ? "forward" : "backward")) + 
           " jump for P" + std::to_string(playerNum), detailedLogging);
}

// First add this helper function to properly check if a player is in a state where auto-jump shouldn't be applied
bool IsNonJumpableState(short moveID) {
    // For debug logging
    if (detailedLogging && moveID > 0) {
        LogOut("[AUTO-JUMP] Checking moveID: " + std::to_string(moveID), false);
    }

    // APPROACH: Only allow jumping from specific known jumpable states
    // This is safer than trying to identify all non-jumpable states
    
    // These are the only states we know for sure allow jumping
    bool isJumpableState = 
        moveID == IDLE_MOVE_ID ||         // Standing idle
        moveID == WALK_FWD_ID ||          // Walking forward
        moveID == WALK_BACK_ID ||         // Walking backward
        moveID == CROUCH_ID ||            // Crouching
        moveID == CROUCH_TO_STAND_ID ||   // Rising from crouch
        moveID == 163;                    // Forward dash (as mentioned)
    
    // If it's in our list of jumpable states, then it's not non-jumpable
    return !isJumpableState;
}

// Now modify the MonitorAutoJump function to use this helper
void MonitorAutoJump() {
    static bool prevJumpEnabled = false;
    static int prevJumpDirection = -1;
    static int prevJumpTarget = -1;
    
    bool currentJumpEnabled = autoJumpEnabled.load();
    int currentJumpDirection = jumpDirection.load();
    int currentJumpTarget = jumpTarget.load();
    
    // Check if settings have changed
    bool jumpEnabledChanged = prevJumpEnabled != currentJumpEnabled;
    bool jumpDirectionChanged = prevJumpDirection != currentJumpDirection;
    bool jumpTargetChanged = prevJumpTarget != currentJumpTarget;
    
    if (jumpEnabledChanged || jumpDirectionChanged || jumpTargetChanged) {
        if (currentJumpEnabled) {
            std::string directionStr;
            switch (currentJumpDirection) {
                case 0: directionStr = "Straight"; break;
                case 1: directionStr = "Forward"; break;
                case 2: directionStr = "Backward"; break;
                default: directionStr = "Unknown"; break;
            }
            
            std::string targetStr;
            switch (currentJumpTarget) {
                case 1: targetStr = "P1"; break;
                case 2: targetStr = "P2"; break;
                case 3: targetStr = "Both"; break;
                default: targetStr = "Unknown"; break;
            }
            
            LogOut("[AUTO-JUMP] Enabled: " + targetStr + " " + directionStr, true);
        } else {
            LogOut("[AUTO-JUMP] Disabled", true);
        }
        
        prevJumpEnabled = currentJumpEnabled;
        prevJumpDirection = currentJumpDirection;
        prevJumpTarget = currentJumpTarget;
    }
}

// Add these missing function implementations after the existing helper functions:

bool IsBlockstunState(short moveID) {
    // Check if the moveID corresponds to any blockstun state
    return moveID == STANDING_BLOCK_LVL1 || 
           moveID == STANDING_BLOCK_LVL2 || 
           moveID == STANDING_BLOCK_LVL3 ||
           moveID == CROUCHING_BLOCK_LVL1 || 
           moveID == CROUCHING_BLOCK_LVL2_A ||
           moveID == CROUCHING_BLOCK_LVL2_B || 
           moveID == AIR_GUARD_ID ||
           (moveID >= 157 && moveID <= 165); // Extended blockstun range
}

int GetAttackLevel(short blockstunMoveID) {
    // Determine attack level based on the blockstun moveID
    switch (blockstunMoveID) {
        case STANDING_BLOCK_LVL1:
        case CROUCHING_BLOCK_LVL1:
            return 1; // Level 1 attack
            
        case STANDING_BLOCK_LVL2:
        case CROUCHING_BLOCK_LVL2_A:
        case CROUCHING_BLOCK_LVL2_B:
            return 2; // Level 2 attack
            
        case STANDING_BLOCK_LVL3:
            return 3; // Level 3 attack
            
        case AIR_GUARD_ID:
            return 0; // Air block (special case)
            
        default:
            // For extended blockstun ranges, try to infer level
            if (blockstunMoveID >= 157 && blockstunMoveID <= 160) {
                return 1; // Assume level 1 for lower range
            } else if (blockstunMoveID >= 161 && blockstunMoveID <= 165) {
                return 2; // Assume level 2 for higher range
            }
            return 1; // Default to level 1
    }
}

std::string GetBlockStateType(short blockstunMoveID) {
    // Return a descriptive string for the block state
    switch (blockstunMoveID) {
        case STANDING_BLOCK_LVL1:
            return "Standing Block (Level 1)";
        case STANDING_BLOCK_LVL2:
            return "Standing Block (Level 2)";
        case STANDING_BLOCK_LVL3:
            return "Standing Block (Level 3)";
        case CROUCHING_BLOCK_LVL1:
            return "Crouching Block (Level 1)";
        case CROUCHING_BLOCK_LVL2_A:
            return "Crouching Block (Level 2A)";
        case CROUCHING_BLOCK_LVL2_B:
            return "Crouching Block (Level 2B)";
        case AIR_GUARD_ID:
            return "Air Block";
        default:
            // For other blockstun states, provide generic description
            if (blockstunMoveID >= 157 && blockstunMoveID <= 165) {
                return "Extended Blockstun (Level " + std::to_string(GetAttackLevel(blockstunMoveID)) + ")";
            }
            return "Unknown Blockstun State";
    }
}

int GetExpectedFrameAdvantage(int attackLevel, bool isAirBlock, bool isHit) {
    // Calculate expected frame advantage based on attack level and hit/block
    if (isAirBlock) {
        return FRAME_ADV_AIR_BLOCK;
    }
    
    if (isHit) {
        // Hit advantages
        switch (attackLevel) {
            case 1: return FRAME_ADV_LVL1_HIT;
            case 2: return FRAME_ADV_LVL2_HIT;
            case 3: return FRAME_ADV_LVL3_HIT;
            default: return FRAME_ADV_LVL1_HIT;
        }
    } else {
        // Block advantages
        switch (attackLevel) {
            case 1: return FRAME_ADV_LVL1_BLOCK;
            case 2: return FRAME_ADV_LVL2_BLOCK;
            case 3: return FRAME_ADV_LVL3_BLOCK;
            default: return FRAME_ADV_LVL1_BLOCK;
        }
    }
}

// Add these variables at the top of the file with other globals

// Track if we have performed an auto-action this sequence
bool p1ActionApplied = false;
bool p2ActionApplied = false;

// Helper function to get attack moveID based on action type
short GetActionMoveID(int actionType) {
    switch (actionType) {
        case ACTION_5A: return BASE_ATTACK_5A;        // 200
        case ACTION_5B: return BASE_ATTACK_5B;        // 201
        case ACTION_5C: return BASE_ATTACK_5C;        // 203
        case ACTION_2A: return BASE_ATTACK_2A;        // 204
        case ACTION_2B: return BASE_ATTACK_2B;        // 205
        case ACTION_2C: return BASE_ATTACK_2C;        // 206
        case ACTION_JUMP: return STRAIGHT_JUMP_ID;    // 4
        case ACTION_BACKDASH: return 164;             // Correct backdash moveID
        case ACTION_CUSTOM: return (short)autoActionCustomID.load();
        default: 
            LogOut("[AUTO-ACTION] Warning: Unknown action type " + std::to_string(actionType), true);
            return 0;
    }
}

// Add this function to apply auto actions when appropriate
void ApplyAutoAction(int playerNum, uintptr_t moveIDAddr, short currentMoveID, short prevMoveID) {
    if (!autoActionEnabled.load()) return;
    if (moveIDAddr == 0) return;
    
    // Check if we should apply to this player
    int targetPlayer = autoActionPlayer.load();
    if (targetPlayer != 3 && targetPlayer != playerNum) return;
    
    // Reference the appropriate flags
    bool& actionApplied = (playerNum == 1) ? p1ActionApplied : p2ActionApplied;
    
    int trigger = autoActionTrigger.load();
    bool shouldApply = false;
    
    // Detect trigger conditions based on moveID transitions
    switch (trigger) {
        case TRIGGER_AFTER_BLOCK:
            // Detect transition from blockstun to actionable
            if (IsBlockstun(prevMoveID) && IsActionable(currentMoveID)) {
                shouldApply = true;
                LogOut("[AUTO-ACTION] P" + std::to_string(playerNum) + " recovered from blockstun (MoveID: " + 
                       std::to_string(prevMoveID) + " -> " + std::to_string(currentMoveID) + ")", true);
            }
            break;
            
        case TRIGGER_ON_WAKEUP:
            // Detect recovery from knockdown states - FIXED to include moveID 96
            if ((prevMoveID == GROUNDTECH_START ||  // 98
                 prevMoveID == GROUNDTECH_END ||    // 99
                 prevMoveID == 96 ||                // Missing groundtech state
                 prevMoveID == 90 || prevMoveID == 91 || // Generic knockdown states
                 prevMoveID == 92 || prevMoveID == 93) && 
                IsActionable(currentMoveID)) {
                shouldApply = true;
                LogOut("[AUTO-ACTION] P" + std::to_string(playerNum) + " woke up (MoveID: " + 
                       std::to_string(prevMoveID) + " -> " + std::to_string(currentMoveID) + ")", true);
            }
            break;
            
        case TRIGGER_AFTER_HITSTUN:
            // Detect recovery from hitstun
            if (IsHitstun(prevMoveID) && IsActionable(currentMoveID)) {
                shouldApply = true;
                LogOut("[AUTO-ACTION] P" + std::to_string(playerNum) + " recovered from hitstun (MoveID: " + 
                       std::to_string(prevMoveID) + " -> " + std::to_string(currentMoveID) + ")", true);
            }
            break;
    }
    
    // Reset flag if we're no longer in actionable state
    if (!IsActionable(currentMoveID)) {
        actionApplied = false;
    }
    
    // Apply the action if triggered and not already applied
    if (shouldApply && !actionApplied) {
        short actionMoveID = GetActionMoveID(autoActionType.load());
        
        if (actionMoveID != 0) {
            // Write the moveID
            WriteGameMemory(moveIDAddr, &actionMoveID, sizeof(short));
            
            // Log the action with better formatting
            std::string actionName;
            switch (autoActionType.load()) {
                case ACTION_5A: actionName = "5A (Light)"; break;
                case ACTION_5B: actionName = "5B (Medium)"; break;
                case ACTION_5C: actionName = "5C (Heavy)"; break;
                case ACTION_2A: actionName = "2A (Crouching Light)"; break;
                case ACTION_2B: actionName = "2B (Crouching Medium)"; break;
                case ACTION_2C: actionName = "2C (Crouching Heavy)"; break;
                case ACTION_JUMP: actionName = "Jump"; break;
                case ACTION_BACKDASH: actionName = "Backdash"; break;
                case ACTION_CUSTOM: actionName = "Custom MoveID " + std::to_string(autoActionCustomID.load()); break;
                default: actionName = "Unknown Action"; break;
            }
            
            LogOut("[AUTO-ACTION] Applied " + actionName + " for P" + std::to_string(playerNum) + 
                   " (MoveID: " + std::to_string(actionMoveID) + ")", true);
            actionApplied = true;
        } else {
            LogOut("[AUTO-ACTION] Warning: Invalid action MoveID for P" + std::to_string(playerNum), true);
        }
    }
}

// MonitorAutoActions function
void MonitorAutoActions() {
    static short prevMoveID1 = 0, prevMoveID2 = 0;
    static bool initialized = false;
    
    if (!initialized) {
        LogOut("[AUTO-ACTION] Monitoring system initialized", true);
        initialized = true;
    }
    
    if (!autoActionEnabled.load()) {
        // Reset previous moveIDs when disabled to avoid false triggers when re-enabled
        prevMoveID1 = 0;
        prevMoveID2 = 0;
        return;
    }
    
    uintptr_t base = GetEFZBase();
    if (!base) return;
    
    // Get current moveIDs and addresses
    uintptr_t moveIDAddr1 = ResolvePointer(base, EFZ_BASE_OFFSET_P1, MOVE_ID_OFFSET);
    uintptr_t moveIDAddr2 = ResolvePointer(base, EFZ_BASE_OFFSET_P2, MOVE_ID_OFFSET);
    
    short currentMoveID1 = 0, currentMoveID2 = 0;
    if (moveIDAddr1) memcpy(&currentMoveID1, (void*)moveIDAddr1, sizeof(short));
    if (moveIDAddr2) memcpy(&currentMoveID2, (void*)moveIDAddr2, sizeof(short));
    
    // Apply auto-actions for both players
    if (moveIDAddr1) {
        ApplyAutoAction(1, moveIDAddr1, currentMoveID1, prevMoveID1);
    }
    if (moveIDAddr2) {
        ApplyAutoAction(2, moveIDAddr2, currentMoveID2, prevMoveID2);
    }
    
    // Update previous moveIDs for next frame
    prevMoveID1 = currentMoveID1;
    prevMoveID2 = currentMoveID2;
}

// FrameDataMonitor function
void FrameDataMonitor() {
    uintptr_t base = GetEFZBase();
    state = Idle;
    
    // Initialize global variables
    initialBlockstunMoveID = -1;

    // Rest of your existing initialization
    int RG_FREEZE_DURATION = 0;  // Keep this as it's set dynamically based on RG type

    // Add these lines to fix the undeclared identifier errors
    bool forceTimeout = false;
    bool stateChanged = false;  // Add this line to fix the stateChanged errors

    // Block/RG monitoring variables
    int defender = -1, attacker = -1;
    int defenderBlockstunStart = -1;
    int defenderActionableFrame = -1, attackerActionableFrame = -1;
    int consecutiveNoChangeFrames = 0;
    int lastBlockstunEndFrame = -1;
    int rgStartFrame = -1;
    short rgType = 0;
    short prevMoveID1 = 0, prevMoveID2 = 0;
    short moveID1 = 0, moveID2 = 0;
    
    // Variables for superflash tracking
    int superflashStartFrame = -1;
    int superflashDuration = 0;
    short superflashType = 0;  // 1 = IC, 2 = Super
    int superflashInitiator = -1;  // Player who initiated superflash
    bool superflashLogged = false; // To prevent duplicate logging

    // Variables for hitstun and tech monitoring
    short p1UntechValue = 0, p2UntechValue = 0;
    short prevP1UntechValue = 0, prevP2UntechValue = 0;
    int hitstunStartFrame = -1;
    int hitstunPlayer = -1;  // Which player is in hitstun
    bool inCombo = false;
    int comboHits = 0;
    int lastHitFrame = -1;
    int techStartFrame = -1;
    int techRecoveryEndFrame = -1;
    int techPlayer = -1; // Which player is teching
    short techType = 0;
    
    using clock = std::chrono::high_resolution_clock;
    auto lastFrame = clock::now();

    // Track move history to improve detection reliability
    std::deque<short> moveHistory1;
    std::deque<short> moveHistory2;
    const size_t HISTORY_SIZE = 5;

    // Add more blockstun move IDs if needed
    const std::vector<short> blockstunMoveIDs = {
        STAND_GUARD_ID, CROUCH_GUARD_ID, CROUCH_GUARD_STUN1,
        CROUCH_GUARD_STUN2, AIR_GUARD_ID,
        157, 158, 159, 160
    };

    // Define Recoil Guard move IDs
    const std::vector<short> rgMoveIDs = {
        RG_STAND_ID, RG_CROUCH_ID, RG_AIR_ID
    };

    // Add these variables near the top of the function
    int frameAdv = 0;
    int lastFrameAdvDisplay = 0;
    const int FRAME_ADV_COOLDOWN = 30; // Don't show more than once per 30 frames
    
    // Making detection more sensitive by tracking possible blockstun transitions
    bool p1WasInBlockstun = false;
    bool p2WasInBlockstun = false;
    int p1BlockstunEndFrame = -1;
    int p2BlockstunEndFrame = -1;
    
    // Add at the beginning of the FrameDataMonitor function
    bool p1WasInGroundtech = false;
    bool p2WasInGroundtech = false;

    while (true) {
        auto frameStart = clock::now();
        
        if (IsEFZWindowActive()) {
            uintptr_t base = GetEFZBase();
            if (base) {
                // Get current moveIDs
                uintptr_t moveIDAddr1 = ResolvePointer(base, EFZ_BASE_OFFSET_P1, MOVE_ID_OFFSET);
                uintptr_t moveIDAddr2 = ResolvePointer(base, EFZ_BASE_OFFSET_P2, MOVE_ID_OFFSET);
                
                short currentMoveID1 = 0, currentMoveID2 = 0;
                if (moveIDAddr1) memcpy(&currentMoveID1, (void*)moveIDAddr1, sizeof(short));
                if (moveIDAddr2) memcpy(&currentMoveID2, (void*)moveIDAddr2, sizeof(short));
                
                // Monitor auto-actions (NEW - this was missing!)
                MonitorAutoActions();
                
                // Monitor existing features
                MonitorAutoAirtech(currentMoveID1, currentMoveID2);
                
                // Monitor auto-jump
                MonitorAutoJump();
                
                // Update previous moveIDs
                prevMoveID1 = currentMoveID1;
                prevMoveID2 = currentMoveID2;
                
                frameCounter++;
            }
        }
        
        // Sleep to maintain ~192Hz monitoring rate (same as game's internal rate)
        auto frameEnd = clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(frameEnd - frameStart);
        auto targetTime = std::chrono::microseconds(5208); // ~192Hz
        
        if (elapsed < targetTime) {
            std::this_thread::sleep_for(targetTime - elapsed);
        }
    }
}