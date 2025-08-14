#include "../include/game/auto_action.h"
#include "../include/core/constants.h"
#include "../include/utils/utilities.h"
#include "../include/game/game_state.h"
#include "../include/core/memory.h"
#include "../include/core/logger.h"
#include "../include/input/input_core.h"        
#include "../include/input/motion_system.h"     
#include "../include/game/auto_action_helpers.h"
#include "../include/input/motion_constants.h"  
#include "../include/input/input_motion.h"      // Add this include
#include "../include/input/input_freeze.h"     // Add this include near the top with the other includes
#include "../include/game/attack_reader.h"
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
static constexpr int TRIGGER_COOLDOWN_FRAMES = 60; // was larger
bool g_p2ControlOverridden = false;
uint32_t g_originalP2ControlFlag = 1; // Default to AI control

// Add at the top with other global variables (around line 40)
std::atomic<bool> g_pendingControlRestore(false);
std::atomic<int> g_controlRestoreTimeout(0);
std::atomic<short> g_lastP2MoveID(-1);
const int CONTROL_RESTORE_TIMEOUT = 180; // 180 internal frames = 1 second


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
            
            uintptr_t moveIDAddr = ResolvePointer(base, EFZ_BASE_OFFSET_P2, MOVE_ID_OFFSET);
            short currentMoveID = 0;
            
            if (moveIDAddr) {
                SafeReadMemory(moveIDAddr, &currentMoveID, sizeof(short));
                
                // Apply action via input system
                ApplyAutoAction(2, moveIDAddr, 0, 0);
                
                LogOut("[AUTO-ACTION] P2 action applied via input system", true);
                
                p2DelayState.isDelaying = false;
                p2DelayState.triggerType = TRIGGER_NONE;
                p2DelayState.pendingMoveID = 0;
                p2ActionApplied = true;
                
                // Restore P2 control state if we changed it
                RestoreP2ControlState();
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
     // IMPORTANT: If targeting P2, ensure human control is enabled
    if (playerNum == 2) {
        // Add debug logs to track control state changes
        LogOut("[AUTO-ACTION] Enabling human control for P2 auto-action", true);
        EnableP2ControlForAutoAction();
    }
    // Set trigger cooldown to prevent rapid re-triggering
    if (playerNum == 1) {
        p1TriggerActive = true;
        p1TriggerCooldown = TRIGGER_COOLDOWN_FRAMES;
        p1DelayState.triggerType = triggerType;
    } else {
        p2TriggerActive = true;
        p2TriggerCooldown = TRIGGER_COOLDOWN_FRAMES;
        p2DelayState.triggerType = triggerType;
    }
    
    // If delay is 0, apply immediately
    if (delayFrames == 0) {
        // Get the player's move ID address - just to pass to ApplyAutoAction
        uintptr_t moveIDAddr = (playerNum == 1) ? 
            ResolvePointer(GetEFZBase(), EFZ_BASE_OFFSET_P1, MOVE_ID_OFFSET) :
            ResolvePointer(GetEFZBase(), EFZ_BASE_OFFSET_P2, MOVE_ID_OFFSET);
        
        if (moveIDAddr) {
            // Call ApplyAutoAction which now uses input-based execution
            ApplyAutoAction(playerNum, moveIDAddr, 0, 0);
        }
    }
    // For delayed actions, use the existing delay system
    else {
        int internalFrames = delayFrames * 3;
        
        if (playerNum == 1) {
            p1DelayState.isDelaying = true;
            p1DelayState.delayFramesRemaining = internalFrames;
            // triggerType already set above
            p1DelayState.pendingMoveID = 0;
        } else {
            p2DelayState.isDelaying = true;
            p2DelayState.delayFramesRemaining = internalFrames;
            // triggerType already set above
            p2DelayState.pendingMoveID = 0;
        }
    }
}

// Process trigger cooldowns to prevent rapid re-triggering
void ProcessTriggerCooldowns() {
    // P1 cooldown processing
    if (p1TriggerActive && p1TriggerCooldown > 0) {
        p1TriggerCooldown--;
        if (p1TriggerCooldown <= 0) {
            p1TriggerActive = false;
            LogOut("[AUTO-ACTION] P1 trigger cooldown expired, new triggers allowed", 
                   detailedLogging.load());
        }
    }
    
    // P2 cooldown processing
    if (p2TriggerActive && p2TriggerCooldown > 0) {
        p2TriggerCooldown--;
        if (p2TriggerCooldown <= 0) {
            p2TriggerActive = false;
            LogOut("[AUTO-ACTION] P2 trigger cooldown expired, new triggers allowed", 
                   detailedLogging.load());
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
        if (!shouldTrigger && triggerAfterBlockEnabled.load() && !p2TriggerActive) {
            if (IsBlockstun(prevMoveID2) && IsActionable(moveID2)) {
                // Add a check to ensure we're not still in a trigger cooldown
                if (p2TriggerCooldown <= 0) {
                    shouldTrigger = true;
                    triggerType = TRIGGER_AFTER_BLOCK;
                    delay = triggerAfterBlockDelay.load();
                    actionMoveID = GetActionMoveID(triggerAfterBlockAction.load(), TRIGGER_AFTER_BLOCK, 2);
                    
                    LogOut("[AUTO-ACTION] P2 After Block trigger activated", true);
                } else {
                    LogOut("[AUTO-ACTION] P2 After Block trigger condition met but cooldown active: " + 
                           std::to_string(p2TriggerCooldown), detailedLogging.load());
                }
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

    // Process auto control restore at the end of every monitor cycle
    ProcessAutoControlRestore();
}

void ResetActionFlags() {
    p1ActionApplied = false;
    p2ActionApplied = false;

    // If we're resetting action flags, also restore P2 control if needed
    RestoreP2ControlState();
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

// Replace the ApplyAutoAction function with this implementation:
void ApplyAutoAction(int playerNum, uintptr_t moveIDAddr, short currentMoveID, short prevMoveID) {
    // Get the trigger type from the player's delay state
    int triggerType = (playerNum == 1) ? p1DelayState.triggerType : p2DelayState.triggerType;

    // Get the appropriate action for this trigger
    int actionType = 0;
    switch (triggerType) {
        case TRIGGER_AFTER_BLOCK:
            actionType = triggerAfterBlockAction.load();
            break;
        case TRIGGER_ON_WAKEUP:
            actionType = triggerOnWakeupAction.load();
            break;
        case TRIGGER_AFTER_HITSTUN:
            actionType = triggerAfterHitstunAction.load();
            break;
        case TRIGGER_AFTER_AIRTECH:
            actionType = triggerAfterAirtechAction.load();
            break;
        default:
            actionType = ACTION_5A; // Default to 5A
            break;
    }

    // Convert action to motion type and determine button mask
    int motionType = ConvertTriggerActionToMotion(actionType, triggerType);
    int buttonMask = 0;
    
    // Determine button mask based on action type
    if (actionType >= ACTION_5A && actionType <= ACTION_2C) {
        // Normal attacks
        int button = (actionType - ACTION_5A) % 3;  // 0=A, 1=B, 2=C
        buttonMask = (1 << (4 + button));  // A=16, B=32, C=64
    } else if (actionType >= ACTION_JA && actionType <= ACTION_JC) {
        // Jump attacks
        int button = (actionType - ACTION_JA) % 3;  // 0=A, 1=B, 2=C
        buttonMask = (1 << (4 + button));  // A=16, B=32, C=64
    } else if (actionType >= ACTION_QCF && actionType <= ACTION_CUSTOM) {
        // Special moves - get strength from helper function
        int strength = GetSpecialMoveStrength(actionType, triggerType);
        buttonMask = (1 << (4 + strength));  // 0=A(16), 1=B(32), 2=C(64)
    }

    LogOut("[AUTO-ACTION] Converting action type " + std::to_string(actionType) + 
           " for trigger type " + std::to_string(triggerType), true);
    LogOut("[AUTO-ACTION] Converted to motion type: " + std::to_string(motionType) + 
           " with button mask: " + std::to_string(buttonMask), true);

    // If buttonMask is zero, use default
    if (buttonMask == 0) {
        LogOut("[AUTO-ACTION] Using default button mask (A) for motion", true);
        buttonMask = 0x10; // Default to A button (16)
    }
    
    bool success = false;
    bool isRegularMove = (motionType >= MOTION_5A && motionType <= MOTION_JC);
    bool isSpecialMove = (motionType >= MOTION_236A);
    
    if (isRegularMove) {
        // For regular moves, use manual input override (this works correctly)
        LogOut("[AUTO-ACTION] Applying regular move " + GetMotionTypeName(motionType) + 
               " via manual input override", true);
               
        // Get the input mask for this move type
        uint8_t inputMask = 0x00; // Start with neutral
        
        // Handle different move types
        if (motionType >= MOTION_5A && motionType <= MOTION_5C) {
            inputMask = buttonMask; // Just the button press
        } 
        else if (motionType >= MOTION_2A && motionType <= MOTION_2C) {
            inputMask = GAME_INPUT_DOWN | buttonMask; // Down + button
        } 
        else if (motionType >= MOTION_JA && motionType <= MOTION_JC) {
            inputMask = buttonMask; // Just button in air
        }
        
        // Set up manual override to hold this input for several frames
        g_manualInputMask[playerNum].store(inputMask);
        g_manualInputOverride[playerNum].store(true);
        
        // Create a thread to release the override after a short time
        std::thread([playerNum]() {
            // Hold the input for ~6 frames (30ms) which is enough for most moves to register
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            
            // Clear the override
            g_manualInputOverride[playerNum].store(false);
            g_manualInputMask[playerNum].store(0);
            
            LogOut("[AUTO-ACTION] Released manual input override for P" + 
                   std::to_string(playerNum), true);
        }).detach();
        
        success = true;
    }
    else if (isSpecialMove) {
        // For special moves, use buffer freezing (this works better for motions)
        LogOut("[AUTO-ACTION] Applying special move " + GetMotionTypeName(motionType) + 
               " via buffer freeze", true);
        
        // Use the FreezeBufferForMotion function from input_freeze.cpp
        success = FreezeBufferForMotion(playerNum, motionType, buttonMask);
    }
    
    if (success) {
        if (isRegularMove) {
            LogOut("[AUTO-ACTION] Set up P2 control restoration for normal attack", true);
            g_lastP2MoveID.store(currentMoveID);
            g_pendingControlRestore.store(true);
            g_controlRestoreTimeout.store(60); // Shorter timeout for normal attacks
        } else {
            LogOut("[AUTO-ACTION] Set up P2 control restoration monitoring: initial moveID=" + 
                   std::to_string(currentMoveID), true);
            g_lastP2MoveID.store(currentMoveID);
            g_pendingControlRestore.store(true);
            g_controlRestoreTimeout.store(180); // Longer timeout for special moves
        }
    } else {
        LogOut("[AUTO-ACTION] Failed to apply move " + GetMotionTypeName(motionType), true);
        RestoreP2ControlState();
    }

    // Log attack data for debugging
    short moveID = GetActionMoveID(actionType, triggerType, playerNum);
    AttackReader::LogMoveData(playerNum, moveID);
}

// Enable P2 human control for auto-action and save original state
void EnableP2ControlForAutoAction() {
    uintptr_t base = GetEFZBase();
    if (!base) {
        LogOut("[AUTO-ACTION] Failed to get EFZ base address", true);
        return;
    }
    
    uintptr_t p2CharPtr = 0;
    if (!SafeReadMemory(base + EFZ_BASE_OFFSET_P2, &p2CharPtr, sizeof(uintptr_t)) || !p2CharPtr) {
        LogOut("[AUTO-ACTION] Failed to get P2 pointer for control override", true);
        return;
    }
    
    // IMPORTANT: Always verify the ACTUAL control flag value
    uint32_t currentAIFlag = 1;
    if (!SafeReadMemory(p2CharPtr + AI_CONTROL_FLAG_OFFSET, &currentAIFlag, sizeof(uint32_t))) {
        LogOut("[AUTO-ACTION] Failed to read P2 control state, aborting override", true);
        return;
    }
    
    // Only save the original flag the first time we override it
    if (!g_p2ControlOverridden) {
        g_originalP2ControlFlag = currentAIFlag;
        LogOut("[AUTO-ACTION] Saving original P2 AI control flag: " + std::to_string(g_originalP2ControlFlag), true);
    } else if (currentAIFlag != 0) {
        // If flag was reset by the game, log it
        LogOut("[AUTO-ACTION] P2 control flag was reset to " + std::to_string(currentAIFlag) + ", setting back to human control", true);
    }
    
    // Always set to human control (0) regardless of our tracking variable
    uint32_t humanControlFlag = 0;
    if (SafeWriteMemory(p2CharPtr + AI_CONTROL_FLAG_OFFSET, &humanControlFlag, sizeof(uint32_t))) {
        // Double-check that it was actually written
        uint32_t verifyFlag = 1;
        if (SafeReadMemory(p2CharPtr + AI_CONTROL_FLAG_OFFSET, &verifyFlag, sizeof(uint32_t)) && verifyFlag == 0) {
            g_p2ControlOverridden = true;
            LogOut("[AUTO-ACTION] P2 control successfully set to human (0) for auto-action", true);
        } else {
            LogOut("[AUTO-ACTION] P2 control write failed verification, flag still = " + 
                  std::to_string(verifyFlag), true);
        }
    } else {
        LogOut("[AUTO-ACTION] Failed to write human control flag to P2", true);
    }
}

// Restore P2 to original control state
void RestoreP2ControlState() {
    if (g_p2ControlOverridden) {
        // Make sure to stop any buffer freezing when restoring control
        StopBufferFreezing();
        
        // IMPORTANT: Force a longer cooldown period to prevent immediate re-triggering
        p2TriggerActive = true;
        p2TriggerCooldown = TRIGGER_COOLDOWN_FRAMES; // now ~0.42s instead of previous large value
        LogOut("[AUTO-ACTION] Enforcing extended trigger cooldown after control restore", true);
        
        uintptr_t base = GetEFZBase();
        if (!base) {
            LogOut("[AUTO-ACTION] Failed to get EFZ base for control restore, marking as restored anyway", true);
            g_p2ControlOverridden = false; // Reset flag anyway to avoid getting stuck
            return;
        }
        
        uintptr_t p2CharPtr = 0;
        if (!SafeReadMemory(base + EFZ_BASE_OFFSET_P2, &p2CharPtr, sizeof(uintptr_t)) || !p2CharPtr) {
            LogOut("[AUTO-ACTION] Failed to get P2 pointer for control restore, marking as restored anyway", true);
            g_p2ControlOverridden = false; // Reset flag anyway to avoid getting stuck
            return;
        }
        
        // Restore original control state
        LogOut("[AUTO-ACTION] Restoring P2 control to original state: " + std::to_string(g_originalP2ControlFlag), true);
        if (SafeWriteMemory(p2CharPtr + AI_CONTROL_FLAG_OFFSET, &g_originalP2ControlFlag, sizeof(uint32_t))) {
            LogOut("[AUTO-ACTION] P2 control restored successfully", true);
        } else {
            LogOut("[AUTO-ACTION] Failed to write P2 control state for restore", true);
        }
        
        // Reset flag regardless of write success to avoid getting stuck
        g_p2ControlOverridden = false;
    }
}

// Add this function to auto_action.h
void ProcessAutoControlRestore() {
    if (!IsMatchPhase()) {
        if (g_pendingControlRestore.load()) {
            LogOut("[AUTO-ACTION] Phase left MATCH during restore; forcing cleanup", true);
            RestoreP2ControlState();
            g_pendingControlRestore.store(false);
        }
        return;
    }
    if (g_pendingControlRestore.load()) {
        // Get current P2 moveID
        uintptr_t base = GetEFZBase();
        if (!base) return;
        
        uintptr_t moveIDAddr2 = ResolvePointer(base, EFZ_BASE_OFFSET_P2, MOVE_ID_OFFSET);
        short moveID2 = 0;
        
        if (moveIDAddr2) {
            SafeReadMemory(moveIDAddr2, &moveID2, sizeof(short));
        }
        
        // Decrement timeout counter (if we're using one)
        int timeout = g_controlRestoreTimeout.fetch_sub(1);
        
        // Less frequent logging
        if (timeout % 60 == 0) {
            LogOut("[AUTO-ACTION] Monitoring move execution: MoveID=" + 
                   std::to_string(moveID2) + ", LastMoveID=" + 
                   std::to_string(g_lastP2MoveID.load()) + 
                   ", Timeout=" + std::to_string(timeout), true);
        }
        
        // Only consider a move change after we've seen a non-zero moveID first
        // This prevents premature restoration due to staying in neutral (moveID=0)
        static bool sawNonZeroMoveID = false;
        if (moveID2 > 0) {
            sawNonZeroMoveID = true;
        }
        
        bool moveChanged = (moveID2 != g_lastP2MoveID.load() && moveID2 == 0 && sawNonZeroMoveID);
        bool timeoutExpired = (timeout <= 0);
        
        if (moveChanged || timeoutExpired) {
            LogOut("[AUTO-ACTION] Auto-restoring P2 control state after move execution", true);
            LogOut("[AUTO-ACTION] Reason: " + 
                   std::string(moveChanged ? "Move completed" : "Timeout expired") +
                   ", MoveID: " + std::to_string(moveID2), true);
            
            RestoreP2ControlState();
            g_pendingControlRestore.store(false);
            g_lastP2MoveID.store(-1);
            sawNonZeroMoveID = false;
            
            // Add a cooldown to prevent re-triggering immediately
            p2TriggerCooldown = TRIGGER_COOLDOWN_FRAMES; // now ~0.42s instead of previous large value
        } else {
            // Update last moveID for tracking
            if (moveID2 != 0) {
                g_lastP2MoveID.store(moveID2);
            }
        }
    }
}

bool AutoGuard(int playerNum, int opponentPtr) {
    uintptr_t playerPtr = GetPlayerPointer(playerNum);
    if (!playerPtr || !opponentPtr)
        return false;
        
    // Get opponent's current move ID
    short moveID = 0;
    if (!SafeReadMemory(opponentPtr + 0x8, &moveID, sizeof(short)) || moveID <= 0)
        return false;
        
    // Log what move we're trying to block
    LogOut("[AUTO_GUARD] Attempting to block move ID: " + std::to_string(moveID), true);
    
    // Get attack height
    AttackHeight height = AttackReader::GetAttackHeight(opponentPtr, moveID);
    
    // Check if in air
    double yPos = 0.0;
    SafeReadMemory(playerPtr + 40, &yPos, sizeof(double));
    bool inAir = (yPos < 0.0);
    
    // Determine block stance
    bool playerFacingRight = GetPlayerFacingDirection(playerNum);
    uint8_t blockInput = 0;
    
    LogOut("[AUTO_GUARD] Attack height: " + std::to_string(height) + 
           ", Player in air: " + (inAir ? "yes" : "no"), true);
    
    switch (height) {
        case ATTACK_HEIGHT_LOW:
            // Must crouch block
            blockInput = GAME_INPUT_DOWN | (playerFacingRight ? GAME_INPUT_LEFT : GAME_INPUT_RIGHT);
            LogOut("[AUTO_GUARD] Using crouch block for low attack", true);
            break;
            
        case ATTACK_HEIGHT_HIGH:
            if (inAir) {
                // Air block
                blockInput = playerFacingRight ? GAME_INPUT_LEFT : GAME_INPUT_RIGHT;
                LogOut("[AUTO_GUARD] Using air block for high attack", true);
            } else {
                // Stand block
                blockInput = playerFacingRight ? GAME_INPUT_LEFT : GAME_INPUT_RIGHT;
                LogOut("[AUTO_GUARD] Using stand block for high attack", true);
            }
            break;
            
        case ATTACK_HEIGHT_MID:
            if (inAir) {
                // Air block
                blockInput = playerFacingRight ? GAME_INPUT_LEFT : GAME_INPUT_RIGHT;
                LogOut("[AUTO_GUARD] Using air block for mid attack", true);
            } else {
                // Either stand or crouch block works, prefer crouch
                blockInput = GAME_INPUT_DOWN | (playerFacingRight ? GAME_INPUT_LEFT : GAME_INPUT_RIGHT);
                LogOut("[AUTO_GUARD] Using crouch block for mid attack", true);
            }
            break;
            
        case ATTACK_HEIGHT_THROW:
            LogOut("[AUTO_GUARD] Cannot block unblockable attack", true);
            return false;
            
        default:
            LogOut("[AUTO_GUARD] Unknown attack height", true);
            return false;
    }
    
    // Apply block input
    return WritePlayerInput(playerPtr, blockInput);
}