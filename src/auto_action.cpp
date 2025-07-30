#include "../include/auto_action.h"
#include "../include/constants.h"
#include "../include/utilities.h"
#include "../include/memory.h"
#include "../include/logger.h"
#include "../include/input_motion.h"
#include "../include/auto_action_helpers.h"

// Define the motion input constants if they're not already defined
#ifndef MOTION_INPUT_UP
#define MOTION_INPUT_UP INPUT_UP
#endif

#ifndef MOTION_INPUT_LEFT
#define MOTION_INPUT_LEFT INPUT_LEFT
#endif

#ifndef ACTION_BACK_DASH
#define ACTION_BACK_DASH ACTION_BACKDASH
#endif

// Forward declaration for function used within this file
void ApplyAutoAction(int playerNum, uintptr_t moveIDAddr, short currentMoveID, short prevMoveID);

// Initialize delay states
TriggerDelayState p1DelayState = {false, 0, TRIGGER_NONE, 0};
TriggerDelayState p2DelayState = {false, 0, TRIGGER_NONE, 0};
bool p1ActionApplied = false;
bool p2ActionApplied = false;

// Definitions for trigger tracking globals
std::atomic<int> g_lastActiveTriggerType(TRIGGER_NONE);
std::atomic<int> g_lastActiveTriggerFrame(0);

static bool p1TriggerActive = false;
static bool p2TriggerActive = false;
static int p1TriggerCooldown = 0;
static int p2TriggerCooldown = 0;
static const int TRIGGER_COOLDOWN_FRAMES = 20; // About 1/3 second cooldown

bool IsCharacterGrounded(int playerNum) {
    uintptr_t base = GetEFZBase();
    if (!base) return true; // Default to true if can't check
    
    uintptr_t playerOffset = (playerNum == 1) ? EFZ_BASE_OFFSET_P1 : EFZ_BASE_OFFSET_P2;
    uintptr_t yAddr = ResolvePointer(base, playerOffset, YPOS_OFFSET);
    uintptr_t yVelAddr = ResolvePointer(base, playerOffset, YVEL_OFFSET);
    
    if (!yAddr || !yVelAddr) return true; // Default to true if can't check
    
    double yPos = 0.0, yVel = 0.0;
    SafeReadMemory(yAddr, &yPos, sizeof(double));
    SafeReadMemory(yVelAddr, &yVel, sizeof(double));
    
    // Character is considered grounded when very close to y=0 and not moving vertically
    return (yPos <= 0.1 && fabs(yVel) < 0.1);
}

short GetActionMoveID(int actionType, int triggerType, int playerNum) {
    // First check if it's a custom action
    if (actionType == ACTION_CUSTOM) {
        short customID = 0;
        // Move custom moveID selection logs to detailed mode
        switch (triggerType) {
            case TRIGGER_AFTER_AIRTECH:
                customID = triggerAfterAirtechCustomID.load();
                LogOut("[AUTO-ACTION] Using After Airtech custom moveID: " + std::to_string(customID), detailedLogging.load());
                return customID;
            case TRIGGER_AFTER_BLOCK:
                customID = triggerAfterBlockCustomID.load();
                LogOut("[AUTO-ACTION] Using After Block custom moveID: " + std::to_string(customID), detailedLogging.load());
                return customID;
            case TRIGGER_ON_WAKEUP:
                customID = triggerOnWakeupCustomID.load();
                LogOut("[AUTO-ACTION] Using On Wakeup custom moveID: " + std::to_string(customID), detailedLogging.load());
                return customID;
            case TRIGGER_AFTER_HITSTUN:
                customID = triggerAfterHitstunCustomID.load();
                LogOut("[AUTO-ACTION] Using After Hitstun custom moveID: " + std::to_string(customID), detailedLogging.load());
                return customID;
            default:
                LogOut("[AUTO-ACTION] Using default custom moveID: 200", detailedLogging.load());
                return 200; // Default to 5A if no trigger specified
        }
    }
    
    // Check character ground/air state for context-specific moves
    bool isGrounded = IsCharacterGrounded(playerNum);
    // Move ground state check logs to detailed mode
    LogOut("[AUTO-ACTION] Player " + std::to_string(playerNum) + 
           " ground state: " + (isGrounded ? "grounded" : "airborne") + 
           ", action type: " + std::to_string(actionType), detailedLogging.load());
    
    // Air-specific moves - only process these if character is in the air
    if (!isGrounded) {
        switch (actionType) {
            case ACTION_JA:
                return BASE_ATTACK_JA;  // Air A
            case ACTION_JB:
                return BASE_ATTACK_JB;  // Air B
            case ACTION_JC:
                return BASE_ATTACK_JC;  // Air C
            default:
                // If in air and trying to do a ground move, use a safe fallback
                return STRAIGHT_JUMP_ID;
        }
    }
    else { // Character is on the ground
        // Air move attempted on ground - check if it's a jump-related action
        if (actionType == ACTION_JA || actionType == ACTION_JB || actionType == ACTION_JC) {
            LogOut("[AUTO-ACTION] Cannot perform air move on ground, using JUMP instead", true);
            return STRAIGHT_JUMP_ID; // If air move selected on ground, do a jump instead
        }
        
        // Ground moves - only available when grounded
        switch (actionType) {
            case ACTION_5A:
                return BASE_ATTACK_5A;  // 200
            case ACTION_5B:
                return BASE_ATTACK_5B;  // 201
            case ACTION_5C:
                return BASE_ATTACK_5C;  // 203
            case ACTION_2A:
                return BASE_ATTACK_2A;  // 204
            case ACTION_2B:
                return BASE_ATTACK_2B;  // 205
            case ACTION_2C:
                return BASE_ATTACK_2C;  // 206
            case ACTION_JUMP:
                return STRAIGHT_JUMP_ID; // 4
            case ACTION_BACKDASH:
                return BACKWARD_DASH_START_ID; // 165
            case ACTION_BLOCK:
                return STAND_GUARD_ID; // 151
            default:
                return BASE_ATTACK_5A; // Default to 5A for unknown actions
        }
    }
}

void ProcessTriggerDelays() {
    uintptr_t base = GetEFZBase();
    if (!base) return;

    // P1 delay processing
    if (p1DelayState.isDelaying) {
        p1DelayState.delayFramesRemaining--;
        
        if (p1DelayState.delayFramesRemaining % 64 == 0 && p1DelayState.delayFramesRemaining > 0) {
            LogOut("[AUTO-ACTION] P1 delay countdown: " + std::to_string(p1DelayState.delayFramesRemaining/3) + 
                   " visual frames remaining", detailedLogging.load());
        }
        
        if (p1DelayState.delayFramesRemaining <= 0) {
            LogOut("[AUTO-ACTION] P1 delay expired, applying action", true);
            
            // Get moveIDAddr just to pass to ApplyAutoAction - it won't be used to write to anymore
            uintptr_t moveIDAddr = ResolvePointer(base, EFZ_BASE_OFFSET_P1, MOVE_ID_OFFSET);
            short currentMoveID = 0;
            
            if (moveIDAddr) {
                SafeReadMemory(moveIDAddr, &currentMoveID, sizeof(short)); // Read just for logging
                
                // Apply action via input system
                ApplyAutoAction(1, moveIDAddr, 0, 0); // moveID parameters are ignored now
                
                LogOut("[AUTO-ACTION] P1 action applied via input system", true);
                
                p1DelayState.isDelaying = false;
                p1DelayState.triggerType = TRIGGER_NONE;
                p1DelayState.pendingMoveID = 0;
            } else {
                LogOut("[AUTO-ACTION] Failed to apply P1 action - invalid moveID address", true);
                p1DelayState.isDelaying = false;
            }
        }
    }
    
    // P2 delay processing
    if (p2DelayState.isDelaying) {
        p2DelayState.delayFramesRemaining--;
        
        if (p2DelayState.delayFramesRemaining % 64 == 0 && p2DelayState.delayFramesRemaining > 0) {
            LogOut("[AUTO-ACTION] P2 delay countdown: " + std::to_string(p2DelayState.delayFramesRemaining/3) + 
                   " visual frames remaining", detailedLogging.load());
        }
        
        if (p2DelayState.delayFramesRemaining <= 0) {
            LogOut("[AUTO-ACTION] P2 delay expired, applying action", true);
            
            // Get moveIDAddr just to pass to ApplyAutoAction - it won't be used to write to anymore
            uintptr_t moveIDAddr = ResolvePointer(base, EFZ_BASE_OFFSET_P2, MOVE_ID_OFFSET);
            short currentMoveID = 0;
            
            if (moveIDAddr) {
                SafeReadMemory(moveIDAddr, &currentMoveID, sizeof(short)); // Read just for logging
                
                // Apply action via input system
                ApplyAutoAction(2, moveIDAddr, 0, 0); // moveID parameters are ignored now
                
                LogOut("[AUTO-ACTION] P2 action applied via input system", true);
                
                p2DelayState.isDelaying = false;
                p2DelayState.triggerType = TRIGGER_NONE;
                p2DelayState.pendingMoveID = 0;
            } else {
                LogOut("[AUTO-ACTION] Failed to apply P2 action - invalid moveID address", true);
                p2DelayState.isDelaying = false;
            }
        }
    }
}

void StartTriggerDelay(int playerNum, int triggerType, short moveID, int delayFrames) {
    // Check if the player is already in a trigger cooldown period
    if (playerNum == 1 && p1TriggerActive) {
        return;
    }
    if (playerNum == 2 && p2TriggerActive) {
        return;
    }

    // Set the last active trigger type and frame for overlay feedback
    g_lastActiveTriggerType.store(triggerType);
    g_lastActiveTriggerFrame.store(frameCounter.load());

    LogOut("[AUTO-ACTION] StartTriggerDelay called: Player=" + std::to_string(playerNum) + 
           ", triggerType=" + std::to_string(triggerType) + 
           ", delay=" + std::to_string(delayFrames), true);
    
    // Set cooldown to prevent rapid re-triggering
    if (playerNum == 1) {
        p1TriggerActive = true;
        p1TriggerCooldown = TRIGGER_COOLDOWN_FRAMES;
    } else {
        p2TriggerActive = true;
        p2TriggerCooldown = TRIGGER_COOLDOWN_FRAMES;
    }
    
    // If delay is 0, apply immediately
    if (delayFrames == 0) {
        // Get the player's move ID address - just to pass to ApplyAutoAction
        // but the function won't actually use it to write moveIDs anymore
        uintptr_t moveIDAddr = (playerNum == 1) ? 
            ResolvePointer(GetEFZBase(), EFZ_BASE_OFFSET_P1, MOVE_ID_OFFSET) :
            ResolvePointer(GetEFZBase(), EFZ_BASE_OFFSET_P2, MOVE_ID_OFFSET);
        
        if (moveIDAddr) {
            // Call ApplyAutoAction which now uses input-based execution
            ApplyAutoAction(playerNum, moveIDAddr, 0, 0); // moveID parameters are ignored now
        }
    }
    // For delayed actions, use the existing delay system but don't store moveID
    else {
        int internalFrames = delayFrames * 3;
        
        if (playerNum == 1) {
            p1DelayState.isDelaying = true;
            p1DelayState.delayFramesRemaining = internalFrames;
            p1DelayState.triggerType = triggerType;
            p1DelayState.pendingMoveID = 0; // Not used anymore
        } else {
            p2DelayState.isDelaying = true;
            p2DelayState.delayFramesRemaining = internalFrames;
            p2DelayState.triggerType = triggerType;
            p2DelayState.pendingMoveID = 0; // Not used anymore
        }
    }
}

// Add this function to process cooldowns
void ProcessTriggerCooldowns() {
    if (p1TriggerCooldown > 0) {
        p1TriggerCooldown--;
        if (p1TriggerCooldown <= 0) {
            p1TriggerActive = false;
        }
    }
    
    if (p2TriggerCooldown > 0) {
        p2TriggerCooldown--;
        if (p2TriggerCooldown <= 0) {
            p2TriggerActive = false;
        }
    }
}

void MonitorAutoActions() {
    if (!autoActionEnabled.load()) {
        return;
    }
        ProcessTriggerCooldowns();
    
    uintptr_t base = GetEFZBase();
    if (!base) return;
    
    // Get current moveIDs
    short moveID1 = 0, moveID2 = 0;
    uintptr_t moveIDAddr1 = ResolvePointer(base, EFZ_BASE_OFFSET_P1, MOVE_ID_OFFSET);
    uintptr_t moveIDAddr2 = ResolvePointer(base, EFZ_BASE_OFFSET_P2, MOVE_ID_OFFSET);
    
    if (moveIDAddr1) SafeReadMemory(moveIDAddr1, &moveID1, sizeof(short));
    if (moveIDAddr2) SafeReadMemory(moveIDAddr2, &moveID2, sizeof(short));
    
    static short prevMoveID1 = 0, prevMoveID2 = 0;
    
    int targetPlayer = autoActionPlayer.load();
    
    // P1 triggers
    if ((targetPlayer == 1 || targetPlayer == 3) && !p1DelayState.isDelaying && !p1ActionApplied) {
        bool shouldTrigger = false;
        int triggerType = TRIGGER_NONE;
        int delay = 0;
        short actionMoveID = 0;        
        
        if (!shouldTrigger && triggerAfterAirtechEnabled.load()) {
            // Check if player was in airtech last frame
            bool wasInAirtech = (prevMoveID1 == FORWARD_AIRTECH || prevMoveID1 == BACKWARD_AIRTECH);
            
            // Check if player is now in the first actionable frame after airtech
            bool isNowActionable = (moveID1 == FALLING_ID);
            
            // P1 After-Airtech trigger condition
            if (wasInAirtech && isNowActionable) {
                LogOut("[AUTO-ACTION] P1 After Airtech trigger activated (from moveID " + 
                       std::to_string(prevMoveID1) + " to " + std::to_string(moveID1) + ")", true);
                shouldTrigger = true;
                triggerType = TRIGGER_AFTER_AIRTECH;
                delay = triggerAfterAirtechDelay.load();
                
                // Get the appropriate action moveID for After Airtech trigger
                int actionType = triggerAfterAirtechAction.load();
                actionMoveID = GetActionMoveID(actionType, TRIGGER_AFTER_AIRTECH, 1);
            }
        }


        // After Block trigger
        if (!shouldTrigger && triggerAfterBlockEnabled.load()) {
            bool wasInBlockstun = IsBlockstun(prevMoveID1);
            bool nowNotInBlockstun = !IsBlockstun(moveID1);
            bool wasNotActionable = !IsActionable(prevMoveID1);
            bool isNowActionable = IsActionable(moveID1);
            bool justBecameActionable = wasNotActionable && isNowActionable;

            if (wasInBlockstun && nowNotInBlockstun && justBecameActionable) {
                shouldTrigger = true;
                triggerType = TRIGGER_AFTER_BLOCK;
                delay = triggerAfterBlockDelay.load();
                actionMoveID = GetActionMoveID(triggerAfterBlockAction.load(), TRIGGER_AFTER_BLOCK, 1);
                LogOut("[AUTO-ACTION] P1 After Block trigger activated (just became actionable)", true);
            }
        }
        
        // After Hitstun trigger
        if (!shouldTrigger && triggerAfterHitstunEnabled.load()) {
            bool wasInHitstun = IsHitstun(prevMoveID1);
            bool nowNotInHitstun = !IsHitstun(moveID1);
            bool wasNotActionable = !IsActionable(prevMoveID1);
            bool isNowActionable = IsActionable(moveID1);
            bool justBecameActionable = wasNotActionable && isNowActionable;

            if (wasInHitstun && nowNotInHitstun && justBecameActionable) {
                shouldTrigger = true;
                triggerType = TRIGGER_AFTER_HITSTUN;
                delay = triggerAfterHitstunDelay.load();
                actionMoveID = GetActionMoveID(triggerAfterHitstunAction.load(), TRIGGER_AFTER_HITSTUN, 1);
                LogOut("[AUTO-ACTION] P1 After Hitstun trigger activated (just became actionable)", true);
            }
        }
        
        // On Wakeup trigger
        if (!shouldTrigger && triggerOnWakeupEnabled.load()) {
            if (IsGroundtech(prevMoveID1) && IsActionable(moveID1)) {
                shouldTrigger = true;
                triggerType = TRIGGER_ON_WAKEUP;
                delay = triggerOnWakeupDelay.load();
                actionMoveID = GetActionMoveID(triggerOnWakeupAction.load(), TRIGGER_ON_WAKEUP, 1);
                
                LogOut("[AUTO-ACTION] P1 On Wakeup trigger activated", true);
            }
        }
        
        // Apply the trigger if any condition was met
        if (shouldTrigger) {
            StartTriggerDelay(1, triggerType, actionMoveID, delay);
        }
    }
    
    // P2 triggers - apply same fix
    if ((targetPlayer == 2 || targetPlayer == 3) && !p2DelayState.isDelaying && !p2ActionApplied) {
        bool shouldTrigger = false;
        int triggerType = TRIGGER_NONE;
        int delay = 0;
        short actionMoveID = 0;
        
         // CRITICAL FIX: Complete implementation of After Airtech trigger for P2
        if (!shouldTrigger && triggerAfterAirtechEnabled.load()) {
            // Check for transition from airtech to actionable state
            bool wasAirtech = IsAirtech(prevMoveID2);
            bool isNowActionable = IsActionable(moveID2);
            
            if (wasAirtech && isNowActionable) {
                LogOut("[AUTO-ACTION] P2 After Airtech trigger activated", true);
                shouldTrigger = true;
                triggerType = TRIGGER_AFTER_AIRTECH;
                delay = triggerAfterAirtechDelay.load() * 3; // Convert to internal frames
                actionMoveID = GetActionMoveID(triggerAfterAirtechAction.load(), TRIGGER_AFTER_AIRTECH, 2);
            }
        }
        // After Block trigger
        if (!shouldTrigger && triggerAfterBlockEnabled.load()) {
            if (IsBlockstun(prevMoveID2) && IsActionable(moveID2)) {
                shouldTrigger = true;
                triggerType = TRIGGER_AFTER_BLOCK;
                delay = triggerAfterBlockDelay.load();
                actionMoveID = GetActionMoveID(triggerAfterBlockAction.load(), TRIGGER_AFTER_BLOCK, 2);
                
                LogOut("[AUTO-ACTION] P2 After Block trigger activated", true);
            }
        }
        
        // After Hitstun trigger
        if (!shouldTrigger && triggerAfterHitstunEnabled.load()) {
            if (IsHitstun(prevMoveID2) && !IsHitstun(moveID2) && !IsAirtech(moveID2)) {
                if (IsActionable(moveID2)) {
                    shouldTrigger = true;
                    triggerType = TRIGGER_AFTER_HITSTUN;
                    delay = triggerAfterHitstunDelay.load();
                    actionMoveID = GetActionMoveID(triggerAfterHitstunAction.load(), TRIGGER_AFTER_HITSTUN, 2);
                    LogOut("[AUTO-ACTION] P2 after hitstun trigger activated", true);
                }
            }
        }
        
        // On Wakeup trigger
        if (!shouldTrigger && triggerOnWakeupEnabled.load()) {
            if (IsGroundtech(prevMoveID2) && IsActionable(moveID2)) {
                shouldTrigger = true;
                triggerType = TRIGGER_ON_WAKEUP;
                delay = triggerOnWakeupDelay.load();
                actionMoveID = GetActionMoveID(triggerOnWakeupAction.load(), TRIGGER_ON_WAKEUP, 2);
                
                LogOut("[AUTO-ACTION] P2 On Wakeup trigger activated", true);
            }
        }
        
        if (shouldTrigger) {
            StartTriggerDelay(2, triggerType, actionMoveID, delay);
        }
    }
    
    // Action flag reset logic (existing code)
    static int p1ActionAppliedFrame = -1;
    static int p2ActionAppliedFrame = -1;
    
    if (p1ActionApplied && p1ActionAppliedFrame == -1) {
        p1ActionAppliedFrame = frameCounter.load();
    }
    if (p2ActionApplied && p2ActionAppliedFrame == -1) {
        p2ActionAppliedFrame = frameCounter.load();
    }
    
    int currentFrame = frameCounter.load();
    
    if (p1ActionApplied && p1ActionAppliedFrame != -1) {
        if (currentFrame - p1ActionAppliedFrame >= 30) {
            p1ActionApplied = false;
            p1ActionAppliedFrame = -1;
        }
    }
    
    if (p2ActionApplied && p2ActionAppliedFrame != -1) {
        if (currentFrame - p2ActionAppliedFrame >= 30) {
            p2ActionApplied = false;
            p2ActionAppliedFrame = -1;
        }
    }
    
    prevMoveID1 = moveID1;
    prevMoveID2 = moveID2;
}

void ResetActionFlags() {
    p1ActionApplied = false;
    p2ActionApplied = false;
}

void ClearDelayStatesIfNonActionable() {
    // Only run this if there are actually delays active
    if (!p1DelayState.isDelaying && !p2DelayState.isDelaying) {
        return;
    }
    
    uintptr_t base = GetEFZBase();
    if (!base) return;
    
    // Use cached addresses
    static uintptr_t cachedMoveIDAddr1 = 0;
    static uintptr_t cachedMoveIDAddr2 = 0;
    static int cacheRefreshCounter = 0;
    
    if (cacheRefreshCounter++ >= 64) {
        cachedMoveIDAddr1 = ResolvePointer(base, EFZ_BASE_OFFSET_P1, MOVE_ID_OFFSET);
        cachedMoveIDAddr2 = ResolvePointer(base, EFZ_BASE_OFFSET_P2, MOVE_ID_OFFSET);
        cacheRefreshCounter = 0;
    }
    
    short moveID1 = 0, moveID2 = 0;
    if (cachedMoveIDAddr1) SafeReadMemory(cachedMoveIDAddr1, &moveID1, sizeof(short));
    if (cachedMoveIDAddr2) SafeReadMemory(cachedMoveIDAddr2, &moveID2, sizeof(short));
    
    // CRITICAL FIX: Don't clear delays if the player is in the middle of executing the action we just applied
    // Only clear if they're in clearly bad states (hitstun, blockstun, etc.)
    bool p1InBadState = IsBlockstun(moveID1) || IsHitstun(moveID1) || IsFrozen(moveID1);
    bool p2InBadState = IsBlockstun(moveID2) || IsHitstun(moveID2) || IsFrozen(moveID2);
    
    if (p1DelayState.isDelaying && p1InBadState) {
        p1DelayState.isDelaying = false;
        p1DelayState.triggerType = TRIGGER_NONE;
        p1DelayState.pendingMoveID = 0;
        LogOut("[AUTO-ACTION] Cleared P1 delay - in bad state (moveID " + std::to_string(moveID1) + ")", true);
    }
    
    if (p2DelayState.isDelaying && p2InBadState) {
        p2DelayState.isDelaying = false;
        p2DelayState.triggerType = TRIGGER_NONE;
        p2DelayState.pendingMoveID = 0;
        LogOut("[AUTO-ACTION] Cleared P2 delay - in bad state (moveID " + std::to_string(moveID2) + ")", true);
    }
}

// Replace the entire ApplyAutoAction function with this implementation
void ApplyAutoAction(int playerNum, uintptr_t moveIDAddr, short currentMoveID, short prevMoveID) {
    // Determine which action to perform based on trigger type
    int actionType = 0;
    int triggerType = 0;
    
    if (playerNum == 1) {
        triggerType = p1DelayState.triggerType;
    } else {
        triggerType = p2DelayState.triggerType;
    }

    switch (triggerType) {
        case TRIGGER_AFTER_BLOCK:   actionType = triggerAfterBlockAction.load();   break;
        case TRIGGER_ON_WAKEUP:     actionType = triggerOnWakeupAction.load();     break;
        case TRIGGER_AFTER_HITSTUN: actionType = triggerAfterHitstunAction.load(); break;
        case TRIGGER_AFTER_AIRTECH: actionType = triggerAfterAirtechAction.load(); break;
        default: return; // Should not happen
    }

    // Convert the generic action type to a specific motion for the input system
    int motionType = ConvertActionToMotion(actionType, triggerType);
    if (motionType == MOTION_NONE) {
        LogOut("[AUTO-ACTION] No valid motion found for actionType: " + std::to_string(actionType), true);
        return;
    }

    uint8_t buttonMask = DetermineButtonFromMotionType(motionType);

    // FIX: Use the correct, modern input queue system.
    // This replaces the call to the undefined ExecuteSimpleMoveViaInputs function.
    QueueMotionInput(playerNum, motionType, buttonMask);

    // Mark the action as applied for this player
    if (playerNum == 1) {
        p1ActionApplied = true;
    } else {
        p2ActionApplied = true;
    }
}