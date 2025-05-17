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
    return moveID == GROUNDTECH_START || moveID == GROUNDTECH_END;
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
void MonitorAutoJump(
    uintptr_t base,
    uintptr_t moveIDAddr1, uintptr_t moveIDAddr2,
    short moveID1, short moveID2
) {
    // Skip if auto-jump is disabled
    if (!autoJumpEnabled) {
        p1JumpState = Grounded;
        p2JumpState = Grounded;
        p1StateFrames = 0;
        p2StateFrames = 0;
        return;
    }
    
    // Check if player is in a non-jumpable state
    bool p1NonJumpable = IsNonJumpableState(moveID1);
    bool p2NonJumpable = IsNonJumpableState(moveID2);
    
    // Enhanced logging for debugging
    if (detailedLogging) {
        if (p1NonJumpable || moveID1 > 0) {
            LogOut("[AUTO-JUMP] P1 MoveID: " + std::to_string(moveID1) + 
                   " (Non-jumpable: " + (p1NonJumpable ? "Yes" : "No") + ")", false);
        }
        
        if (p2NonJumpable || moveID2 > 0) {
            LogOut("[AUTO-JUMP] P2 MoveID: " + std::to_string(moveID2) + 
                   " (Non-jumpable: " + (p2NonJumpable ? "Yes" : "No") + ")", false);
        }
    }
    
    // Player 1 jump monitoring if enabled AND in an actionable state
    if ((jumpTarget == 1 || jumpTarget == 3) && !p1NonJumpable && moveIDAddr1) {
        switch (p1JumpState) {
            case Grounded:
                // Only apply jump if in an actionable state
                if (IsActionable(moveID1)) {
                    ApplyJump(moveIDAddr1, 1, jumpDirection);
                    p1JumpState = Rising;
                    p1StateFrames = 0;
                }
                break;
                
            case Rising:
                // Check if we've started falling
                if (moveID1 == FALLING_ID) {
                    p1JumpState = Falling;
                    p1StateFrames = 0;
                } else {
                    p1StateFrames++;
                    // Force the proper jumping moveID
                    short jumpMoveID;
                    switch (jumpDirection) {
                        case 0: jumpMoveID = STRAIGHT_JUMP_ID; break;
                        case 1: jumpMoveID = FORWARD_JUMP_ID; break;
                        case 2: jumpMoveID = BACKWARD_JUMP_ID; break;
                        default: jumpMoveID = STRAIGHT_JUMP_ID;
                    }
                    WriteGameMemory(moveIDAddr1, &jumpMoveID, sizeof(short));
                }
                break;
                
            case Falling:
                // Wait for landing
                if (moveID1 == LANDING_ID) {
                    p1JumpState = Landing;
                    p1StateFrames = 0;
                } else {
                    p1StateFrames++;
                    // Ensure we're still in falling state
                    if (moveID1 != FALLING_ID) {
                        short fallingID = FALLING_ID;
                        WriteGameMemory(moveIDAddr1, &fallingID, sizeof(short));
                    }
                }
                break;
                
            case Landing:
                // Allow landing animation to play for at least 2 frames
                p1StateFrames++;
                if (p1StateFrames >= 2) {
                    p1JumpState = NeutralFrame;
                    p1StateFrames = 0;
                    // Force neutral state
                    short idleID = IDLE_MOVE_ID;
                    WriteGameMemory(moveIDAddr1, &idleID, sizeof(short));
                }
                break;
                
            case NeutralFrame:
                // Wait one frame in neutral state
                p1StateFrames++;
                if (p1StateFrames >= 1) {
                    // Restart jump cycle
                    p1JumpState = Grounded;
                    p1StateFrames = 0;
                }
                break;
        }
    }
    
    // Player 2 jump monitoring if enabled AND in an actionable state
    if ((jumpTarget == 2 || jumpTarget == 3) && !p2NonJumpable && moveIDAddr2) {
        switch (p2JumpState) {
            case Grounded:
                // Only apply jump if in an actionable state
                if (IsActionable(moveID2)) {
                    ApplyJump(moveIDAddr2, 2, jumpDirection);
                    p2JumpState = Rising;
                    p2StateFrames = 0;
                }
                break;
                
            case Rising:
                // Check if we've started falling
                if (moveID2 == FALLING_ID) {
                    p2JumpState = Falling;
                    p2StateFrames = 0;
                } else {
                    p2StateFrames++;
                    // Force the proper jumping moveID
                    short jumpMoveID;
                    switch (jumpDirection) {
                        case 0: jumpMoveID = STRAIGHT_JUMP_ID; break;
                        case 1: jumpMoveID = FORWARD_JUMP_ID; break;
                        case 2: jumpMoveID = BACKWARD_JUMP_ID; break;
                        default: jumpMoveID = STRAIGHT_JUMP_ID;
                    }
                    WriteGameMemory(moveIDAddr2, &jumpMoveID, sizeof(short));
                }
                break;
                
            case Falling:
                // Wait for landing
                if (moveID2 == LANDING_ID) {
                    p2JumpState = Landing;
                    p2StateFrames = 0;
                } else {
                    p2StateFrames++;
                    // Ensure we're still in falling state
                    if (moveID2 != FALLING_ID) {
                        short fallingID = FALLING_ID;
                        WriteGameMemory(moveIDAddr2, &fallingID, sizeof(short));
                    }
                }
                break;
                
            case Landing:
                // Allow landing animation to play for at least 2 frames
                p2StateFrames++;
                if (p2StateFrames >= 2) {
                    p2JumpState = NeutralFrame;
                    p2StateFrames = 0;
                    // Force neutral state
                    short idleID = IDLE_MOVE_ID;
                    WriteGameMemory(moveIDAddr2, &idleID, sizeof(short));
                }
                break;
                
            case NeutralFrame:
                // Wait one frame in neutral state
                p2StateFrames++;
                if (p2StateFrames >= 1) {
                    p2JumpState = Grounded;
                    p2StateFrames = 0;
                }
                break;
        }
    }
}

// First, improve the IsBlockstunState function to catch more standing blockstun states
bool IsBlockstunState(short moveID) {
    // Directly check for core blockstun IDs
    if (moveID == STAND_GUARD_ID || 
        moveID == CROUCH_GUARD_ID || 
        moveID == CROUCH_GUARD_STUN1 ||
        moveID == CROUCH_GUARD_STUN2 || 
        moveID == AIR_GUARD_ID) {
        return true;
    }
    
    // Check the range that includes standing blockstun states
    // Ensure we explicitly include 150 and 152 for clarity
    if (moveID == 150 || moveID == 152 || 
        (moveID >= 140 && moveID <= 149) ||  // First blockstun range
        (moveID >= 153 && moveID <= 165)) {  // Second blockstun range (excluding 150-152 which we check explicitly)
        return true;
    }
    
    return false;
}

// Add this helper function to determine attack level from blockstun MoveID
int GetAttackLevel(short blockstunMoveID) {
    // Standing blockstun states
    if (blockstunMoveID == 150) return 1;
    if (blockstunMoveID == STAND_GUARD_ID) return 2; // 151
    if (blockstunMoveID == 152) return 3;
    
    // Crouching blockstun states
    if (blockstunMoveID == 153) return 1;
    if (blockstunMoveID == 154 || blockstunMoveID == 155) return 2;
    
    // Air block
    if (blockstunMoveID == AIR_GUARD_ID) return 0; // Special case for air block
    
    // Default for unknown blockstun types
    return 0;
}

// Helper to determine if it's a standing or crouching block
std::string GetBlockStateType(short blockstunMoveID) {
    if (blockstunMoveID >= 150 && blockstunMoveID <= 152) return "Standing";
    if (blockstunMoveID >= 153 && blockstunMoveID <= 155) return "Crouching";
    if (blockstunMoveID == AIR_GUARD_ID) return "Air";
    return "Unknown";
}

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
        base = GetEFZBase();
        if (base) {
            // Add this code to improve detection of blockstun exit
            // Get player move IDs
            uintptr_t moveIDAddr1 = ResolvePointer(base, EFZ_BASE_OFFSET_P1, MOVE_ID_OFFSET);
            uintptr_t moveIDAddr2 = ResolvePointer(base, EFZ_BASE_OFFSET_P2, MOVE_ID_OFFSET);
            
            short moveID1 = 0, moveID2 = 0;
            if (moveIDAddr1) memcpy(&moveID1, (void*)moveIDAddr1, sizeof(short));
            if (moveIDAddr2) memcpy(&moveID2, (void*)moveIDAddr2, sizeof(short));
            
            // Check for blockstun -> actionable transitions
            bool p1BlockstunNow = IsBlockstunState(moveID1);
            bool p2BlockstunNow = IsBlockstunState(moveID2);
            
            // P1 exiting blockstun
            if (p1WasInBlockstun && !p1BlockstunNow) {
                // Store the frame advantage calculation even without checking IsActionable
                p1BlockstunEndFrame = frameCounter;
                
                // P1 was defender, P2 was attacker
                if (p2BlockstunEndFrame < 0 || (frameCounter - p2BlockstunEndFrame) < 20) {
                    int advantage = p1BlockstunEndFrame - p2BlockstunEndFrame;
                    
                    // Only show if we have valid data and enough time since last display
                    if (p2BlockstunEndFrame > 0 && (frameCounter - lastFrameAdvDisplay) >= FRAME_ADV_COOLDOWN) {
                        LogOut("[FRAME ADVANTAGE] P2 +" + std::to_string(advantage) + 
                              " (P2 vs P1)", true);
                        lastFrameAdvDisplay = frameCounter;
                    }
                }
            }
            
            // P2 exiting blockstun
            if (p2WasInBlockstun && !p2BlockstunNow) {
                // Store the frame advantage calculation even without checking IsActionable
                p2BlockstunEndFrame = frameCounter;
                
                // P2 was defender, P1 was attacker
                if (p1BlockstunEndFrame < 0 || (frameCounter - p1BlockstunEndFrame) < 20) {
                    int advantage = p2BlockstunEndFrame - p1BlockstunEndFrame;
                    
                    // Only show if we have valid data and enough time since last display
                    if (p1BlockstunEndFrame > 0 && (frameCounter - lastFrameAdvDisplay) >= FRAME_ADV_COOLDOWN) {
                        LogOut("[FRAME ADVANTAGE] P1 +" + std::to_string(advantage) + 
                              " (P1 vs P2)", true);
                        lastFrameAdvDisplay = frameCounter;
                    }
                }
            }
            
            // Update blockstun tracking
            p1WasInBlockstun = p1BlockstunNow;
            p2WasInBlockstun = p2BlockstunNow;
            
            // Monitor for new blockstun (reset tracking)
            if (p1BlockstunNow && !p1WasInBlockstun) {
                p1BlockstunEndFrame = -1;
            }
            if (p2BlockstunNow && !p2WasInBlockstun) {
                p2BlockstunEndFrame = -1;
            }
            
            // Modified blockstun state detection - Use this in the main monitoring loop:
          if (IsBlockstunState(moveID1) && !p1WasInBlockstun) {
                initialBlockstunMoveID = moveID1;
                int attackLevel = GetAttackLevel(moveID1);
                std::string blockType = GetBlockStateType(moveID1);
                
                LogOut("[BLOCKSTUN] P1 entered " + blockType + " blockstun (Level " + 
                    std::to_string(attackLevel) + ") at frame " + 
                    std::to_string(frameCounter), detailedLogging);
            }

            // Skip frame advantage calculation for dash states
            if (IsDashState(moveID1) || IsDashState(moveID2)) {
                LogOut("[INFO] Dash state detected - skipping frame advantage calculation", detailedLogging);
                state = Idle;
                continue;
}
            // When blockstun ends, capture the EXACT frame:
            if (!IsBlockstunState(moveID1) && p1WasInBlockstun) {
                // The player becomes actionable on THIS frame (not the next one)
                defenderActionableFrame = frameCounter;
                
                // Log exact frame for debugging
                LogOut("[BLOCKSTUN] P" + std::to_string(defender) + 
                       " exited blockstun at frame " + std::to_string(frameCounter) + 
                       " (MoveID: " + std::to_string(moveID1) + ")", detailedLogging);
            }
            
            // Add this line to call our modified MonitorAutoAirtech function
            MonitorAutoAirtech(moveID1, moveID2);
            
            // Add this line to call our auto-jump monitoring function
            MonitorAutoJump(base, moveIDAddr1, moveIDAddr2, moveID1, moveID2);

            // Get untech values
            prevP1UntechValue = p1UntechValue;
            prevP2UntechValue = p2UntechValue;
            
            p1UntechValue = GetUntechValue(base, 1);
            p2UntechValue = GetUntechValue(base, 2);
            
            // Log untech values when they change
            if (p1UntechValue != prevP1UntechValue && detailedLogging) {
                LogOut("[UNTECH] P1 untech value changed: " + std::to_string(prevP1UntechValue) + 
                      " -> " + std::to_string(p1UntechValue), true);
                
                // Check if player entered hitstun with this change
                if (p1UntechValue > 0 && prevP1UntechValue == 0 && hitstunPlayer != 1) {
                    // This could indicate the start of a combo
                    if (!inCombo || hitstunPlayer != 1) {
                        inCombo = true;
                        hitstunPlayer = 1;
                        comboHits = 1;
                        lastHitFrame = frameCounter;
                        LogOut("[COMBO] P1 entered combo state with " + std::to_string(p1UntechValue) + 
                              " untech frames", detailedTitleMode.load());
                    }
                }
                // Check if this is a new hit in an existing combo
                else if (p1UntechValue > 0 && prevP1UntechValue > 0 && hitstunPlayer == 1 && inCombo) {
                    comboHits++;
                    lastHitFrame = frameCounter;
                    LogOut("[COMBO] P1 hit " + std::to_string(comboHits) + 
                          " with " + std::to_string(p1UntechValue) + " untech frames", 
                          detailedTitleMode.load());
                }
                // Check if combo ended
                else if (p1UntechValue == 0 && prevP1UntechValue > 0 && hitstunPlayer == 1) {
                    inCombo = false;
                    LogOut("[COMBO] P1 combo ended after " + std::to_string(comboHits) + 
                          " hits", true);
                    hitstunPlayer = -1;
                }
            }
            
            if (p2UntechValue != prevP2UntechValue && detailedLogging) {
                LogOut("[UNTECH] P2 untech value changed: " + std::to_string(prevP2UntechValue) + 
                      " -> " + std::to_string(p2UntechValue), true);
                
                // Check if player entered hitstun with this change
                if (p2UntechValue > 0 && prevP2UntechValue == 0 && hitstunPlayer != 2) {
                    // This could indicate the start of a combo
                    if (!inCombo || hitstunPlayer != 2) {
                        inCombo = true;
                        hitstunPlayer = 2;
                        comboHits = 1;
                        lastHitFrame = frameCounter;
                        LogOut("[COMBO] P2 entered combo state with " + std::to_string(p2UntechValue) + 
                              " untech frames", detailedTitleMode.load());
                    }
                }
                // Check if this is a new hit in an existing combo
                else if (p2UntechValue > 0 && prevP2UntechValue > 0 && hitstunPlayer == 2 && inCombo) {
                    comboHits++;
                    lastHitFrame = frameCounter;
                    LogOut("[COMBO] P2 hit " + std::to_string(comboHits) + 
                          " with " + std::to_string(p2UntechValue) + " untech frames", 
                          detailedTitleMode.load());
                }
                // Check if combo ended
                else if (p2UntechValue == 0 && prevP2UntechValue > 0 && hitstunPlayer == 2) {
                    inCombo = false;
                    LogOut("[COMBO] P2 combo ended after " + std::to_string(comboHits) + 
                          " hits", true);
                    hitstunPlayer = -1;
                }
            }

            // Add to history for better detection
            moveHistory1.push_front(moveID1);
            moveHistory2.push_front(moveID2);
            if (moveHistory1.size() > HISTORY_SIZE) moveHistory1.pop_back();
            if (moveHistory2.size() > HISTORY_SIZE) moveHistory2.pop_back();

            // Log MoveID changes for debugging
            if (moveID1 != prevMoveID1 && moveIDAddr1 && detailedLogging) {
                std::ostringstream moveMsg;
                moveMsg << "[MOVE] P1 moveID changed at frame " << frameCounter 
                       << ": " << prevMoveID1 << " -> " << moveID1;
                
                // Add more detail if we're in monitoring
                if (state == Monitoring || state == RGMonitoring || state == SuperflashMonitoring) {
                    if (state == SuperflashMonitoring) {
                        moveMsg << " [Superflash]";
                    } else {
                        if (state == RGMonitoring) {
                            moveMsg << " [RG Monitoring]";
                        } else {
                            moveMsg << " [Block Monitoring]";
                        }
                    }
                    
                    if (defender == 1) {
                        moveMsg << " [Defender]";
                    } else if (attacker == 1) {
                        moveMsg << " [Attacker]";
                    } else if (superflashInitiator == 1) {
                        moveMsg << " [Initiator]";
                    }
                }
                
                // Add hitstat/tech state information when relevant
                if (IsHitstun(moveID1)) {
                    moveMsg << " [Hitstun]";
                    if (IsLaunched(moveID1)) {
                        moveMsg << " [Launched]";
                    }
                } else if (IsAirtech(moveID1)) {
                    moveMsg << " [Airtech]";
                } else if (IsGroundtech(moveID1)) {
                    moveMsg << " [Groundtech]";
                } else if (IsSpecialStun(moveID1)) {
                    if (moveID1 == FIRE_STATE) moveMsg << " [Fire]";
                    else if (moveID1 == ELECTRIC_STATE) moveMsg << " [Electric]";
                    else if (IsFrozen(moveID1)) moveMsg << " [Frozen]";
                }
                
                LogOut(moveMsg.str(), true);
            }
            
            if (moveID2 != prevMoveID2 && moveIDAddr2 && detailedLogging) {
                std::ostringstream moveMsg;
                moveMsg << "[MOVE] P2 moveID changed at frame " << frameCounter 
                       << ": " << prevMoveID2 << " -> " << moveID2;
                
                // Add more detail if we're in monitoring
                if (state == Monitoring || state == RGMonitoring || state == SuperflashMonitoring) {
                    if (state == SuperflashMonitoring) {
                        moveMsg << " [Superflash]";
                    } else {
                        if (state == RGMonitoring) {
                            moveMsg << " [RG Monitoring]";
                        } else {
                            moveMsg << " [Block Monitoring]";
                        }
                    }
                    
                    if (defender == 2) {
                        moveMsg << " [Defender]";
                    } else if (attacker == 2) {
                        moveMsg << " [Attacker]";
                    } else if (superflashInitiator == 2) {
                        moveMsg << " [Initiator]";
                    }
                }
                
                // Add hitstun/tech state information when relevant
                if (IsHitstun(moveID2)) {
                    moveMsg << " [Hitstun]";
                    if (IsLaunched(moveID2)) {
                        moveMsg << " [Launched]";
                    }
                } else if (IsAirtech(moveID2)) {
                    moveMsg << " [Airtech]";
                } else if (IsGroundtech(moveID2)) {
                    moveMsg << " [Groundtech]";
                } else if (IsSpecialStun(moveID2)) {
                    if (moveID2 == FIRE_STATE) moveMsg << " [Fire]";
                    else if (moveID2 == ELECTRIC_STATE) moveMsg << " [Electric]";
                    else if (IsFrozen(moveID2)) moveMsg << " [Frozen]";
                }
                
                LogOut(moveMsg.str(), true);
            }

            auto now = clock::now();
            double ms = std::chrono::duration<double, std::milli>(now - lastFrame).count();
            lastFrame = now;

            // Enhanced blockstun detection
            auto isInBlockstun = [&blockstunMoveIDs](short moveID) {
                return std::find(blockstunMoveIDs.begin(), blockstunMoveIDs.end(), moveID) != blockstunMoveIDs.end();
            };

            // Function to check if a moveID is a Recoil Guard state
            auto isRecoilGuard = [&rgMoveIDs](short moveID) {
                return std::find(rgMoveIDs.begin(), rgMoveIDs.end(), moveID) != rgMoveIDs.end();
            };

            // Only log debug info if menu is not open (to avoid cluttering logs)
            if (!menuOpen) {
                // Log full debug info only if detailed logging is enabled
                std::ostringstream oss;
                oss << "[DEBUG] Frame: " << frameCounter
                    << " | " << ms << " ms"
                    << " | P1 HP:" << displayData.hp1
                    << " Meter:" << displayData.meter1
                    << " RF:" << displayData.rf1
                    << " X:" << displayData.x1
                    << " Y:" << displayData.y1
                    << " MoveID:" << moveID1
                    << " Untech:" << p1UntechValue
                    << " | P2 HP:" << displayData.hp2
                    << " Meter:" << displayData.meter2
                    << " RF:" << displayData.rf2
                    << " X:" << displayData.x2
                    << " Y:" << displayData.y2
                    << " MoveID:" << moveID2
                    << " Untech:" << p2UntechValue;

                // Convert internal frame count to visible frames for display
                int visibleFramesSinceStart = 0;
                if ((state == Monitoring || state == RGMonitoring) && defenderBlockstunStart != -1) {
                    visibleFramesSinceStart = static_cast<int>((frameCounter - defenderBlockstunStart) / SUBFRAMES_PER_VISUAL_FRAME);
                    oss << " | Monitoring: Defender=" << defender
                        << " BlockstunStart=" << defenderBlockstunStart
                        << " DAF=" << defenderActionableFrame
                        << " AAF=" << attackerActionableFrame
                        << " NoChange=" << consecutiveNoChangeFrames
                        << " VisibleFrames=" << visibleFramesSinceStart;

                    // If in RG monitoring, add RG-specific info
                    if (state == RGMonitoring) {
                        if (rgType == RG_STAND_ID) {
                            oss << " | RG: Type=Stand";
                        } else if (rgType == RG_CROUCH_ID) {
                            oss << " | RG: Type=Crouch";
                        } else {
                            oss << " | RG: Type=Air";
                        }
                        oss << " StartFrame=" << rgStartFrame;
                    }
                }
                // Add superflash info if monitoring superflash
                else if (state == SuperflashMonitoring && superflashStartFrame != -1) {
                    int elapsedFrames = frameCounter - superflashStartFrame;
                    visibleFramesSinceStart = static_cast<int>(elapsedFrames / SUBFRAMES_PER_VISUAL_FRAME);
                    oss << " | Superflash: Initiator=P" << superflashInitiator
                        << " Type=" << (superflashType == 1 ? "IC" : "Super")
                        << " StartFrame=" << superflashStartFrame
                        << " ElapsedFrames=" << elapsedFrames
                        << " VisibleFrames=" << visibleFramesSinceStart;
                }
                
                // Add combo info if in a combo
                if (inCombo) {
                    oss << " | Combo: Player=" << hitstunPlayer
                        << " Hits=" << comboHits
                        << " LastHit=" << lastHitFrame;
                }
                
                // Add tech info if in tech recovery
                if (techRecoveryEndFrame > 0) {
                    oss << " | Tech: Player=" << techPlayer
                        << " Type=" << (techType == FORWARD_AIRTECH ? "Forward" : 
                                       (techType == BACKWARD_AIRTECH ? "Backward" : 
                                       (techType == GROUNDTECH_START || techType == GROUNDTECH_END ? "Ground" : "Unknown")))
                        << " StartFrame=" << techStartFrame
                        << " RecoveryEnd=" << techRecoveryEndFrame;
                }

                LogOut(oss.str(), detailedLogging);

                // Check for frame rate issues - we expect ~5.2ms per frame at 192Hz
                if (ms > 7.0 && (state == Monitoring || state == RGMonitoring || state == SuperflashMonitoring)) {
                    LogOut("[WARNING] Possible frame skip detected: " + std::to_string(ms) + "ms (expected ~5.2ms)", detailedLogging);
                }
            }

            switch (state) {
            case Idle:
                // Check for Hitstun states
                if (IsHitstun(moveID1) && !IsHitstun(prevMoveID1)) {
                    LogOut("[HITSTUN] P1 entered hitstun at frame " + std::to_string(frameCounter) +
                          " (MoveID: " + std::to_string(moveID1) + ")", true);
                    
                    if (IsLaunched(moveID1)) {
                        LogOut("[HITSTUN] P1 was launched", true);
                    }
                    
                    hitstunStartFrame = frameCounter;
                }
                else if (IsHitstun(moveID2) && !IsHitstun(prevMoveID2)) {
                    LogOut("[HITSTUN] P2 entered hitstun at frame " + std::to_string(frameCounter) +
                          " (MoveID: " + std::to_string(moveID2) + ")", true);
                    
                    if (IsLaunched(moveID2)) {
                        LogOut("[HITSTUN] P2 was launched", true);
                    }
                    
                    hitstunStartFrame = frameCounter;
                }
                
                // Air tech detection
                if (IsAirtech(moveID1) && !IsAirtech(prevMoveID1)) {
                    LogOut("[TECH] P1 performed air tech at frame " + std::to_string(frameCounter) +
                          " (MoveID: " + std::to_string(moveID1) + ")", detailedLogging);
                    
                    techStartFrame = frameCounter;
                    techType = moveID1;
                    techPlayer = 1;
                    techRecoveryEndFrame = frameCounter + AIRTECH_VULNERABLE_FRAMES * 3;
                    
                    LogOut("[TECH] P1 is vulnerable for " + std::to_string(AIRTECH_VULNERABLE_FRAMES) + 
                          " frames after air tech", detailedLogging);
                }
                else if (IsAirtech(moveID2) && !IsAirtech(prevMoveID2)) {
                    LogOut("[TECH] P2 performed air tech at frame " + std::to_string(frameCounter) +
                          " (MoveID: " + std::to_string(moveID2) + ")", detailedLogging);
                    
                    techStartFrame = frameCounter;
                    techType = moveID2;
                    techPlayer = 2;
                    techRecoveryEndFrame = frameCounter + AIRTECH_VULNERABLE_FRAMES * 3;
                    
                    LogOut("[TECH] P2 is vulnerable for " + std::to_string(AIRTECH_VULNERABLE_FRAMES) + 
                          " frames after air tech", detailedLogging);
                }
                
                // Ground tech detection
                if (moveID1 == GROUNDTECH_START && prevMoveID1 != GROUNDTECH_START) {
                    LogOut("[TECH] P1 started ground tech at frame " + std::to_string(frameCounter), detailedLogging);
                    techStartFrame = frameCounter;
                    techType = GROUNDTECH_START;
                    techPlayer = 1;
                }
                else if (moveID1 == GROUNDTECH_END && prevMoveID1 == GROUNDTECH_START) {
                    LogOut("[TECH] P1 ground tech recovery at frame " + std::to_string(frameCounter), detailedLogging);
                }
                
                if (moveID2 == GROUNDTECH_START && prevMoveID2 != GROUNDTECH_START) {
                    LogOut("[TECH] P2 started ground tech at frame " + std::to_string(frameCounter), detailedLogging);
                    techStartFrame = frameCounter;
                    techType = GROUNDTECH_START;
                    techPlayer = 2;
                }
                else if (moveID2 == GROUNDTECH_END && prevMoveID2 == GROUNDTECH_START) {
                    LogOut("[TECH] P2 ground tech recovery at frame " + std::to_string(frameCounter), detailedLogging);
                }
                
                // Special stun detection
                if (IsSpecialStun(moveID1) && !IsSpecialStun(prevMoveID1)) {
                    std::string stunType = "";
                    if (moveID1 == FIRE_STATE) stunType = "fire";
                    else if (moveID1 == ELECTRIC_STATE) stunType = "electric";
                    else if (moveID1 >= FROZEN_STATE_START && moveID1 <= FROZEN_STATE_END) stunType = "frozen";
                    
                    LogOut("[STUN] P1 entered " + stunType + " state at frame " + 
                          std::to_string(frameCounter) + " (MoveID: " + std::to_string(moveID1) + ")", detailedLogging);
                    
                    if (moveID1 == FROZEN_STATE_START || moveID1 == FROZEN_STATE_START + 1) {
                        LogOut("[STUN] P1 will be directly actionable after frozen state", true);
                    } else if (moveID1 == FROZEN_STATE_END - 1 || moveID1 == FROZEN_STATE_END) {
                        LogOut("[STUN] P1 will fall down after frozen state", true);
                    }
                }
                
                if (IsSpecialStun(moveID2) && !IsSpecialStun(prevMoveID2)) {
                    std::string stunType = "";
                    if (moveID2 == FIRE_STATE) stunType = "fire";
                    else if (moveID2 == ELECTRIC_STATE) stunType = "electric";
                    else if (moveID2 >= FROZEN_STATE_START && moveID2 <= FROZEN_STATE_END) stunType = "frozen";
                    
                    LogOut("[STUN] P2 entered " + stunType + " state at frame " + 
                          std::to_string(frameCounter) + " (MoveID: " + std::to_string(moveID2) + ")", detailedLogging);
                    
                    if (moveID2 == FROZEN_STATE_START || moveID2 == FROZEN_STATE_START + 1) {
                        LogOut("[STUN] P2 will be directly actionable after frozen state", true);
                    } else if (moveID2 == FROZEN_STATE_END - 1 || moveID2 == FROZEN_STATE_END) {
                        LogOut("[STUN] P2 will fall down after frozen state", true);
                    }
                }
                
                // Tech recovery end detection
                if (techRecoveryEndFrame != -1 && frameCounter == techRecoveryEndFrame) {
                    LogOut("[TECH] P" + std::to_string(techPlayer) + " tech vulnerability period ended at frame " + 
                          std::to_string(frameCounter), detailedLogging);
                    techRecoveryEndFrame = -1;
                    techPlayer = -1;
                }
                
                // Check for gap between blockstun if we've just ended monitoring
                if (lastBlockstunEndFrame != -1) {
                    if (isInBlockstun(moveID1) && !isInBlockstun(prevMoveID1) ||
                        isRecoilGuard(moveID1) && !isRecoilGuard(prevMoveID1)) {
                        int gap = frameCounter - lastBlockstunEndFrame;
                        int visibleFramesWhole = gap / 3;
                        int remainder = gap % 3;

                        std::ostringstream gapMsg;
                        gapMsg << "[FRAME GAP] Gap between defensive states: ";

                        // Format with whole number + decimal
                        gapMsg << visibleFramesWhole;
                        if (remainder == 1) {
                            gapMsg << ".33";
                        }
                        else if (remainder == 2) {
                            gapMsg << ".66";
                        }

                        gapMsg << " visual frames (" << gap << " internal frames)";
                        LogOut(gapMsg.str(), true);
                        lastBlockstunEndFrame = -1;  // Reset after reporting gap
                    }
                    else if (isInBlockstun(moveID2) && !isInBlockstun(prevMoveID2) ||
                        isRecoilGuard(moveID2) && !isRecoilGuard(prevMoveID2)) {
                        int gap = frameCounter - lastBlockstunEndFrame;
                        int visibleFramesWhole = gap / 3;
                        int remainder = gap % 3;

                        std::ostringstream gapMsg;
                        gapMsg << "[FRAME GAP] Gap between defensive states: ";

                        // Format with whole number + decimal
                        gapMsg << visibleFramesWhole;
                        if (remainder == 1) {
                            gapMsg << ".33";
                        }
                        else if (remainder == 2) {
                            gapMsg << ".66";
                        }

                        gapMsg << " visual frames (" << gap << " internal frames)";
                        LogOut(gapMsg.str(), true);
                        lastBlockstunEndFrame = -1;  // Reset after reporting gap
                    }
                    else if (frameCounter - lastBlockstunEndFrame > 90) {  // ~30 visible frames with no new blockstun
                        lastBlockstunEndFrame = -1;  // Reset gap tracking if it's been too long
                    }
                }

                // Check for Superflash from IC activation
                if ((moveID1 == GROUND_IC_ID || moveID1 == AIR_IC_ID) && 
                    prevMoveID1 != GROUND_IC_ID && prevMoveID1 != AIR_IC_ID) {
                    // P1 activated IC with superflash
                    superflashStartFrame = frameCounter;
                    superflashDuration = IC_FLASH_DURATION;
                    superflashType = 1;  // IC
                    superflashInitiator = 1;
                    superflashLogged = false;
                    state = SuperflashMonitoring;
                    LogOut("[SUPERFLASH] P1 activated IC with superflash at frame " + 
                           std::to_string(frameCounter) + 
                           (moveID1 == GROUND_IC_ID ? " (Ground IC)" : " (Air IC)"), true);
                }
                else if ((moveID2 == GROUND_IC_ID || moveID2 == AIR_IC_ID) && 
                         prevMoveID2 != GROUND_IC_ID && prevMoveID2 != AIR_IC_ID) {
                    // P2 activated IC with superflash
                    superflashStartFrame = frameCounter;
                    superflashDuration = IC_FLASH_DURATION;
                    superflashType = 1;  // IC
                    superflashInitiator = 2;
                    superflashLogged = false;
                    state = SuperflashMonitoring;
                    LogOut("[SUPERFLASH] P2 activated IC with superflash at frame " + 
                           std::to_string(frameCounter) + 
                           (moveID2 == GROUND_IC_ID ? " (Ground IC)" : " (Air IC)"), true);
                }
                // Check for RG (special case)
                else if (IsRecoilGuard(moveID1) && !IsRecoilGuard(prevMoveID1)) {
                    // P1 entered Recoil Guard
                    defender = 1; attacker = 2;
                    rgStartFrame = frameCounter;
                    rgType = moveID1;
                    defenderBlockstunStart = frameCounter;
                    defenderActionableFrame = -1;
                    attackerActionableFrame = -1;
                    consecutiveNoChangeFrames = 0;
                    
                    // Set RG freeze duration based on type
                    if (rgType == RG_STAND_ID) {
                        RG_FREEZE_DURATION = RG_STAND_FREEZE_DURATION;
                    } else if (rgType == RG_CROUCH_ID) {
                        RG_FREEZE_DURATION = RG_CROUCH_FREEZE_DURATION;
                    } else if (rgType == RG_AIR_ID) {
                        RG_FREEZE_DURATION = RG_AIR_FREEZE_DURATION;
                    }
                    
                    state = RGMonitoring;
                    LogOut("[MONITOR] P1 activated Recoil Guard at frame " + std::to_string(frameCounter) +
                        " (Type: " + std::string(rgType == RG_STAND_ID ? "Stand" : (rgType == RG_CROUCH_ID ? "Crouch" : "Air")) + ")", 
                        detailedTitleMode.load());
                }
                else if (IsRecoilGuard(moveID2) && !IsRecoilGuard(prevMoveID2)) {
                    // P2 entered Recoil Guard
                    defender = 2; attacker = 1;
                    rgStartFrame = frameCounter;
                    rgType = moveID2;
                    defenderBlockstunStart = frameCounter;
                    defenderActionableFrame = -1;
                    attackerActionableFrame = -1;
                    consecutiveNoChangeFrames = 0;
                    state = RGMonitoring;
                    LogOut("[MONITOR] P2 activated Recoil Guard at frame " + std::to_string(frameCounter) +
                        " (Type: " + std::string(rgType == RG_STAND_ID ? "Stand" : (rgType == RG_CROUCH_ID ? "Crouch" : "Air")) + ")", 
                        detailedTitleMode.load());
                }
                // Check for blockstun
                else if (IsBlockstun(moveID1) && !IsBlockstun(prevMoveID1)) {
                    // P1 entered blockstun
                    defender = 1; attacker = 2;
                    defenderBlockstunStart = frameCounter;
                    defenderActionableFrame = -1;
                    attackerActionableFrame = -1;
                    consecutiveNoChangeFrames = 0;
                    state = Monitoring;
                    LogOut("[MONITOR] P1 entered blockstun at frame " + std::to_string(frameCounter) +
                        " (MoveID: " + std::to_string(moveID1) + ")", detailedTitleMode.load());
                }
                else if (IsBlockstun(moveID2) && !IsBlockstun(prevMoveID2)) {
                    // P2 entered blockstun
                    defender = 2; attacker = 1;
                    defenderBlockstunStart = frameCounter;
                    defenderActionableFrame = -1;
                    attackerActionableFrame = -1;
                    consecutiveNoChangeFrames = 0;
                    state = Monitoring;
                    LogOut("[MONITOR] P2 entered blockstun at frame " + std::to_string(frameCounter) +
                        " (MoveID: " + std::to_string(moveID2) + ")", detailedTitleMode.load());
                }
                break;
                
            case SuperflashMonitoring:
                {
                    // Track the duration of the superflash
                    int elapsedFrames = frameCounter - superflashStartFrame;
                    
                    // Log progress at intervals if detailed logging is enabled
                    if (detailedLogging && !menuOpen && elapsedFrames % 15 == 0) {
                        double visualFrames = elapsedFrames / 3.0;
                        LogOut("[SUPERFLASH] Duration: " + 
                               std::to_string(visualFrames) + " visual frames", true);
                    }
                    
                    // Check if superflash has ended
                    if (elapsedFrames >= superflashDuration) {
                        // Format the duration for display (convert to visual frames)
                        std::ostringstream flashMsg;
                        flashMsg << "[SUPERFLASH ENDED] Total duration: " 
                                 << std::fixed << std::setprecision(2)
                                 << (superflashDuration / 3.0) << " visual frames";
                        
                        if (superflashType == 1) {
                            flashMsg << " (IC)";
                        } else if (superflashType == 2) {
                            flashMsg << " (Super)";
                        }
                        
                        flashMsg << " initiated by P" << superflashInitiator;
                        
                        // Output to console
                        LogOut(flashMsg.str(), true);
                        
                        // Return to idle state
                        state = Idle;
                    }
                    
                    // Check for move transitions during superflash
                    if (superflashInitiator == 1) {
                        if (moveID1 != prevMoveID1 && moveIDAddr1) {
                            LogOut("[SUPERFLASH] P1 moveID changed during superflash: " + 
                                   std::to_string(prevMoveID1) + " -> " + std::to_string(moveID1), 
                                   detailedLogging && !menuOpen);
                        }
                    } else if (superflashInitiator == 2) {
                        if (moveID2 != prevMoveID2 && moveIDAddr2) {
                            LogOut("[SUPERFLASH] P2 moveID changed during superflash: " + 
                                   std::to_string(prevMoveID2) + " -> " + std::to_string(moveID2), 
                                   detailedLogging && !menuOpen);
                        }
                    }
                }
                break;

            case Monitoring:
            case RGMonitoring:
                {
                    bool stateChanged = false;

                    // Check for Superflash interruptions
                    if ((moveID1 == GROUND_IC_ID || moveID1 == AIR_IC_ID) && 
                        prevMoveID1 != GROUND_IC_ID && prevMoveID1 != AIR_IC_ID) {
                        LogOut("[MONITOR] Monitoring interrupted by P1 IC superflash", true);
                        
                        // Switch to superflash monitoring
                        superflashStartFrame = frameCounter;
                        superflashDuration = IC_FLASH_DURATION;
                        superflashType = 1;  // IC
                        superflashInitiator = 1;
                        superflashLogged = false;
                        state = SuperflashMonitoring;
                        break; // Skip the rest of the monitoring code
                    }
                    else if ((moveID2 == GROUND_IC_ID || moveID2 == AIR_IC_ID) && 
                             prevMoveID2 != GROUND_IC_ID && prevMoveID2 != AIR_IC_ID) {
                        LogOut("[MONITOR] Monitoring interrupted by P2 IC superflash", true);
                        
                        // Switch to superflash monitoring
                        superflashStartFrame = frameCounter;
                        superflashDuration = IC_FLASH_DURATION;
                        superflashType = 1;  // IC
                        superflashInitiator = 2;
                        superflashLogged = false;
                        state = SuperflashMonitoring;
                        break; // Skip the rest of the monitoring code
                    }

                    if (defender == 1 && defenderActionableFrame == -1) {
                        // Check for exact frame when leaving blockstun/RG and entering actionable state
                        if ((IsBlockstun(prevMoveID1) || IsRecoilGuard(prevMoveID1)) && 
                            !IsBlockstun(moveID1) && !IsRecoilGuard(moveID1) && IsActionable(moveID1)) {
                            defenderActionableFrame = frameCounter;
                            stateChanged = true;
                            LogOut("[MONITOR] P1 (defender) became actionable at frame " +
                                std::to_string(frameCounter) + " (MoveID: " + std::to_string(moveID1) + ")", 
                                detailedTitleMode.load());
                        }
                        // Backup check for if they're already in an actionable state
                        else if (!IsBlockstun(moveID1) && !IsRecoilGuard(moveID1) && IsActionable(moveID1)) {
                            defenderActionableFrame = frameCounter;
                            stateChanged = true;
                            LogOut("[MONITOR] P1 (defender) detected in actionable state at frame " +
                                std::to_string(frameCounter) + " (MoveID: " + std::to_string(moveID1) + ")", 
                                detailedTitleMode.load());
                        }
                    }
                    else if (defender == 2 && defenderActionableFrame == -1) {
                        // Same logic for P2
                        if ((IsBlockstun(prevMoveID2) || IsRecoilGuard(prevMoveID2)) && 
                            !IsBlockstun(moveID2) && !IsRecoilGuard(moveID2) && IsActionable(moveID2)) {
                            defenderActionableFrame = frameCounter;
                            stateChanged = true;
                            LogOut("[MONITOR] P2 (defender) became actionable at frame " +
                                std::to_string(frameCounter) + " (MoveID: " + std::to_string(moveID2) + ")", 
                                detailedTitleMode.load());
                        }
                        // Backup check for if they're already in an actionable state
                        else if (!IsBlockstun(moveID2) && !IsRecoilGuard(moveID2) && IsActionable(moveID2)) {
                            defenderActionableFrame = frameCounter;
                            stateChanged = true;
                            LogOut("[MONITOR] P2 (defender) detected in actionable state at frame " +
                                std::to_string(frameCounter) + " (MoveID: " + std::to_string(moveID2) + ")", 
                                detailedTitleMode.load());
                        }
                    }

                    // Check if attacker has become actionable
                    if (attacker == 1 && attackerActionableFrame == -1) {
                        if (IsActionable(moveID1)) {
                            attackerActionableFrame = frameCounter;
                            stateChanged = true;
                            LogOut("[MONITOR] P1 (attacker) became actionable at frame " +
                                std::to_string(frameCounter) + " (MoveID: " + std::to_string(moveID1) + ")", 
                                detailedTitleMode.load());
                        }
                    }
                    else if (attacker == 2 && attackerActionableFrame == -1) {
                        if (IsActionable(moveID2)) {
                            attackerActionableFrame = frameCounter;
                            stateChanged = true;
                            LogOut("[MONITOR] P2 (attacker) became actionable at frame " +
                                std::to_string(frameCounter) + " (MoveID: " + std::to_string(moveID2) + ")", 
                                detailedTitleMode.load());
                        }
                    }

                    // Handle special case - if attacker is already in an actionable state at blockstun start
                    if (attackerActionableFrame == -1) {
                        if ((attacker == 1 && IsActionable(moveID1)) || (attacker == 2 && IsActionable(moveID2))) {
                            attackerActionableFrame = defenderBlockstunStart;
                            stateChanged = true;
                            LogOut("[MONITOR] Attacker was already actionable at blockstun start", 
                                detailedTitleMode.load());
                        }
                    }

                    if (defenderActionableFrame != -1 && attackerActionableFrame != -1) {
                        // Create single source of truth for RG state
                        bool isRG = (state == RGMonitoring);
                        
                        // Calculate frame advantage from P1's perspective
                        // Positive means P1 can act before P2 (has advantage)
                        // Negative means P2 can act before P1 (P1 has disadvantage)
                        int frameAdv;
                        if (defender == 1) {
                            // P1 was blocking, P2 was attacking
                            // To get P1's advantage: P2's recovery - P1's recovery
                            frameAdv = attackerActionableFrame - defenderActionableFrame;
                        } else {
                            // P2 was blocking, P1 was attacking
                            // To get P1's advantage: P2's recovery - P1's recovery
                            frameAdv = defenderActionableFrame - attackerActionableFrame;
                        }
                        
                        // For RG, handle the specific mechanics of RG freeze and stun
                        int fullStunAdvantage = frameAdv;
                        
                        if (isRG) {
                            // Calculate full stun disadvantage (if defender doesn't cancel with an attack)
                            // RG_STUN_DURATION is typically 20F (60 internal frames)
                            if (defender == 1) {
                                fullStunAdvantage = frameAdv - (RG_STUN_DURATION * 3); // P1 is defender
                            } else {
                                fullStunAdvantage = frameAdv + (RG_STUN_DURATION * 3); // P2 is defender
                            }
                        }
                        
                        // Calculate visual frames with precise decimal representation
                        int visibleFramesWhole = abs(frameAdv) / 3;
                        int remainder = abs(frameAdv) % 3;
                        
                        // Format decimal part consistently using .33 and .66
                        std::string framePrecision = "";
                        if (remainder == 1) {
                            framePrecision = ".33";
                        } else if (remainder == 2) {
                            framePrecision = ".66";
                        }
                        
                        // Format the advantage string with proper sign
                        std::string frameAdvDisplay = (frameAdv > 0 ? "+" : "-") + 
                            std::to_string(visibleFramesWhole) + framePrecision;
                        
                        // Calculate visual frame values for full stun advantage (only for RG)
                        std::string fullStunDisplayAdvantage = "";
                        if (isRG) {
                            int fullStunVisualAdvantage = abs(fullStunAdvantage) / 3;
                            std::string fullStunDecimalPart = "";
                            
                            int fullStunRemainder = abs(fullStunAdvantage) % 3;
                            if (fullStunRemainder == 1) {
                                fullStunDecimalPart = ".33";
                            } else if (fullStunRemainder == 2) {
                                fullStunDecimalPart = ".66";
                            }
                            
                            fullStunDisplayAdvantage = (fullStunAdvantage > 0 ? "+" : "-") +
                                std::to_string(fullStunVisualAdvantage) + fullStunDecimalPart;
                        }
                        
                        // Create the advantage message - ONLY ONCE
                        std::ostringstream advMsg;
                        
                        if (isRG) {
                            advMsg << "[RG FRAME ADVANTAGE] ";
                        } else {
                            advMsg << "[FRAME ADVANTAGE] ";
                        }
                        
                        // Always display from P1's perspective
                        advMsg << "P1 is " << frameAdvDisplay << " on block";
                        
                        // Add RG-specific info with improved information about RG mechanics
                        if (isRG) {
                            advMsg << " [RG Type: "
                                << (rgType == RG_STAND_ID ? "Stand" :
                                    (rgType == RG_CROUCH_ID ? "Crouch" : "Air")) << "]";
                            
                            // Add info about RG stun cancelability
                            advMsg << " [Can attack immediately after RG freeze]";
                            
                            // Add detailed information about immediate attack window
                            if (defender == 1 && frameAdv > 0) {
                                int recoveryWindow = frameAdv;
                                advMsg << " [" << std::fixed << std::setprecision(2) << (recoveryWindow / 3.0) 
                                    << "F window for immediate attack]";
                            }
                            else if (defender == 2 && frameAdv < 0) {
                                int recoveryWindow = -frameAdv;
                                advMsg << " [" << std::fixed << std::setprecision(2) << (recoveryWindow / 3.0) 
                                    << "F window for immediate attack]";
                            }
                            
                            // Add information about full stun advantage
                            advMsg << " [If not canceled: P1 is " << fullStunDisplayAdvantage << " on block]";
                            
                            // Add air-specific information if applicable
                            if (rgType == RG_AIR_ID) {
                                advMsg << " [Can RG again in air]";
                            }
                        }
                        
                        // Output to console (always show frame advantage regardless of detailed mode)
                        LogOut(advMsg.str(), true);
                        
                        // Record the frame when this blockstun ended for gap tracking
                        lastBlockstunEndFrame = frameCounter;
                        
                        state = Idle;
                        stateChanged = true;
                    }

                    // If both players seem to be in idle states for a while, something might be wrong
                    if (((defender == 1 && IsActionable(moveID1)) || (defender == 2 && IsActionable(moveID2))) &&
                        consecutiveNoChangeFrames > 500 * 3) {  // 500 visual frames (much more lenient timeout)
                        forceTimeout = true;
                        LogOut("[DEBUG] Force timeout due to early actionable state for " +
                            std::to_string(consecutiveNoChangeFrames / SUBFRAMES_PER_VISUAL_FRAME) +
                            " visible frames", detailedLogging && !menuOpen);
                    }

                    // For RG, handle special timeouts based on RG stun duration
                    if (state == RGMonitoring) {
                        // If we've been in RG monitoring longer than expected RG freeze + stun
                        int rgFreezeTime = 0;
                        if (rgType == RG_STAND_ID) {
                            rgFreezeTime = RG_STAND_FREEZE_DURATION;
                        }
                        else if (rgType == RG_CROUCH_ID) {
                            rgFreezeTime = RG_CROUCH_FREEZE_DURATION;
                        }
                        else if (rgType == RG_AIR_ID) {
                            rgFreezeTime = RG_AIR_FREEZE_DURATION;
                        }



                        // Special handling for RG: try to detect actionable state even if not explicitly represented
                       
                        // This is particularly important for longer moves
                        if (defenderActionableFrame == -1) {
                            short defenderMoveID = (defender == 1) ? moveID1 : moveID2;
                            
                            // Important: In EFZ, the defender can attack immediately after RG freeze
                            // Calculate when the defender can first input an attack after RG freeze
                            if (frameCounter > rgStartFrame + rgFreezeTime && defenderActionableFrame == -1) {
                                // The defender is actionable for attacks after the freeze duration
                                int attackActionableFrame = rgStartFrame + rgFreezeTime;
                                
                                // Only set this if we haven't already detected an actionable state
                                if (defenderActionableFrame == -1) {
                                    defenderActionableFrame = attackActionableFrame;
                                    stateChanged = true;
                                    LogOut("[MONITOR] Defender can input attacks after RG freeze at frame " + 
                                        std::to_string(defenderActionableFrame) + " (RG stun can be canceled with attacks)", 
                                        
                                        detailedTitleMode.load());
                                    
                                    // Also log the RG freeze time and RG type for troubleshooting
                                    LogOut("[DEBUG] RG freeze details: Type=" + std::string(rgType == RG_STAND_ID ? "Stand" : 
                                        (rgType == RG_CROUCH_ID ? "Crouch" : "Air")) + 
                                        ", Freeze duration=" + std::to_string(rgFreezeTime) + 
                                        " internal frames", detailedLogging);
                                }
                            }
                        }
                    }

                    // Only apply the force timeout conditions, not the maximum time limit
                    if (forceTimeout) {
                        LogOut("[MONITOR] Monitoring force-terminated due to possible stuck state after " +
                            std::to_string(consecutiveNoChangeFrames / SUBFRAMES_PER_VISUAL_FRAME) +
                            " visible frames with no change", detailedTitleMode.load());
                        state = Idle;
                    }
                }
                break;
            }

            frameCounter++;
        }
        Sleep(1000 / 192);  // Run at game's internal frame rate of 192fps
    }
}