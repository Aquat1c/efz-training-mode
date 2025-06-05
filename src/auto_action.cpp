#include "../include/auto_action.h"
#include "../include/constants.h"
#include "../include/utilities.h"
#include "../include/memory.h"
#include "../include/logger.h"

// Initialize delay states
TriggerDelayState p1DelayState = {false, 0, TRIGGER_NONE, 0};
TriggerDelayState p2DelayState = {false, 0, TRIGGER_NONE, 0};
bool p1ActionApplied = false;
bool p2ActionApplied = false;

short GetActionMoveID(int actionType, int triggerType = TRIGGER_NONE) {
    // First check if it's a custom action
    if (actionType == ACTION_CUSTOM) {
        short customID = 0;
        switch (triggerType) {
            case TRIGGER_AFTER_BLOCK:
                customID = triggerAfterBlockCustomID.load();
                LogOut("[AUTO-ACTION] Using After Block custom moveID: " + std::to_string(customID), true);
                return customID;
            case TRIGGER_ON_WAKEUP:
                customID = triggerOnWakeupCustomID.load();
                LogOut("[AUTO-ACTION] Using On Wakeup custom moveID: " + std::to_string(customID), true);
                return customID;
            case TRIGGER_AFTER_HITSTUN:
                customID = triggerAfterHitstunCustomID.load();
                LogOut("[AUTO-ACTION] Using After Hitstun custom moveID: " + std::to_string(customID), true);
                return customID;
            case TRIGGER_AFTER_AIRTECH:
                customID = triggerAfterAirtechCustomID.load();
                LogOut("[AUTO-ACTION] Using After Airtech custom moveID: " + std::to_string(customID), true);
                return customID;
            default:
                LogOut("[AUTO-ACTION] Warning: Custom action with unknown trigger type", true);
                return BASE_ATTACK_5A;
        }
    }
    
    // Standard action types
    switch (actionType) {
        case ACTION_5A:
            return BASE_ATTACK_5A;
        case ACTION_5B:
            return BASE_ATTACK_5B;
        case ACTION_5C:
            return BASE_ATTACK_5C;
        case ACTION_2A:
            return BASE_ATTACK_2A;
        case ACTION_2B:
            return BASE_ATTACK_2B;
        case ACTION_2C:
            return BASE_ATTACK_2C;
        case ACTION_JUMP:
            return STRAIGHT_JUMP_ID;
        case ACTION_BACKDASH:
            return BACKWARD_DASH_START_ID;
        case ACTION_BLOCK:
            return STAND_GUARD_ID;
        // NEW: Add air attack options
        case ACTION_JA:
            return BASE_ATTACK_JA;
        case ACTION_JB:
            return BASE_ATTACK_JB;
        case ACTION_JC:
            return BASE_ATTACK_JC;
        default:
            return BASE_ATTACK_5A;
    }
}

void ProcessTriggerDelays() {
    uintptr_t base = GetEFZBase();
    if (!base) return;

    // P1 delay processing
    if (p1DelayState.isDelaying) {
        p1DelayState.delayFramesRemaining--;
        
        if (p1DelayState.delayFramesRemaining % 64 == 0 && p1DelayState.delayFramesRemaining > 0) {
            int visualFramesRemaining = p1DelayState.delayFramesRemaining / 3;
            LogOut("[AUTO-ACTION] P1 delay countdown: " + 
                   std::to_string(visualFramesRemaining) + " visual frames remaining", true);
        }
        
        if (p1DelayState.delayFramesRemaining <= 0) {
            LogOut("[AUTO-ACTION] P1 delay expired, applying action moveID " + 
                   std::to_string(p1DelayState.pendingMoveID), true);
            
            uintptr_t moveIDAddr = ResolvePointer(base, EFZ_BASE_OFFSET_P1, MOVE_ID_OFFSET);
            
            // CRITICAL FIX: Handle airtech trigger properly
            bool isAirTechTrigger = (p1DelayState.triggerType == TRIGGER_AFTER_AIRTECH);
            
            if (moveIDAddr) {
                if (isAirTechTrigger) {
                    // Get Y position and velocity
                    uintptr_t yPosAddr = ResolvePointer(base, EFZ_BASE_OFFSET_P1, YPOS_OFFSET);
                    uintptr_t yVelAddr = ResolvePointer(base, EFZ_BASE_OFFSET_P1, YVEL_OFFSET);
                    
                    if (yPosAddr && yVelAddr) {
                        double yPos = 0.0;
                        double yVel = 0.0;
                        
                        SafeReadMemory(yPosAddr, &yPos, sizeof(double));
                        SafeReadMemory(yVelAddr, &yVel, sizeof(double));
                        
                        // Apply the moveID
                        SafeWriteMemory(moveIDAddr, &p1DelayState.pendingMoveID, sizeof(short));
                        
                        // If not a jump move and in air, ensure we preserve air state
                        if (p1DelayState.pendingMoveID != STRAIGHT_JUMP_ID && 
                            p1DelayState.pendingMoveID != FORWARD_JUMP_ID && 
                            p1DelayState.pendingMoveID != BACKWARD_JUMP_ID && 
                            yPos > 0.0) {
                            
                            if (p1DelayState.pendingMoveID != FALLING_ID && 
                               (p1DelayState.pendingMoveID < BASE_ATTACK_JA || 
                                p1DelayState.pendingMoveID > BASE_ATTACK_JC)) {
                                
                                // Apply the attack but ensure we go back to falling state after
                                LogOut("[AUTO-ACTION] P1: Preserving air state after moveID " + 
                                      std::to_string(p1DelayState.pendingMoveID), true);
                                
                                // Ensure downward velocity for proper falling
                                double fallVelocity = yVel <= -1.0 ? yVel : -1.0;
                                SafeWriteMemory(yVelAddr, &fallVelocity, sizeof(double));
                                
                                // Set a timer to restore falling state
                                std::thread([=]() {
                                    Sleep(50); // Short delay for move to execute
                                    short fallingID = FALLING_ID;
                                    SafeWriteMemory(moveIDAddr, &fallingID, sizeof(short));
                                }).detach();
                            }
                        }
                    } else {
                        // Fallback to just applying the moveID
                        SafeWriteMemory(moveIDAddr, &p1DelayState.pendingMoveID, sizeof(short));
                    }
                } else {
                    // Standard trigger, apply moveID directly
                    SafeWriteMemory(moveIDAddr, &p1DelayState.pendingMoveID, sizeof(short));
                }
                
                LogOut("[AUTO-ACTION] P1 action applied: moveID " + 
                       std::to_string(p1DelayState.pendingMoveID), true);
                
                p1DelayState.isDelaying = false;
                p1DelayState.triggerType = TRIGGER_NONE;
                p1DelayState.pendingMoveID = 0;
                p1ActionApplied = true;
            } else {
                LogOut("[AUTO-ACTION] Failed to apply P1 action - invalid moveID address", true);
                p1DelayState.isDelaying = false;
            }
        }
    }
    
    // P2 delay processing (mirror of P1 code)
    if (p2DelayState.isDelaying) {
        p2DelayState.delayFramesRemaining--;
        
        // Log countdown every 64 internal frames (about every 1/3 second)
        if (p2DelayState.delayFramesRemaining % 64 == 0 && p2DelayState.delayFramesRemaining > 0) {
            int visualFramesRemaining = p2DelayState.delayFramesRemaining / 3;
            LogOut("[AUTO-ACTION] P2 delay countdown: " + 
                   std::to_string(visualFramesRemaining) + " visual frames remaining (" +
                   std::to_string(p2DelayState.delayFramesRemaining) + " internal frames)", true);
        }
        
        if (p2DelayState.delayFramesRemaining <= 0) {
            LogOut("[AUTO-ACTION] P2 delay EXPIRED, applying action moveID " + 
                   std::to_string(p2DelayState.pendingMoveID), true);
            
            uintptr_t moveIDAddr = ResolvePointer(base, EFZ_BASE_OFFSET_P2, MOVE_ID_OFFSET);
            
            // CRITICAL FIX: Handle airtech trigger properly
            bool isAirTechTrigger = (p2DelayState.triggerType == TRIGGER_AFTER_AIRTECH);
            
            if (moveIDAddr) {
                if (isAirTechTrigger) {
                    // Get Y position and velocity
                    uintptr_t yPosAddr = ResolvePointer(base, EFZ_BASE_OFFSET_P2, YPOS_OFFSET);
                    uintptr_t yVelAddr = ResolvePointer(base, EFZ_BASE_OFFSET_P2, YVEL_OFFSET);
                    
                    if (yPosAddr && yVelAddr) {
                        double yPos = 0.0;
                        double yVel = 0.0;
                        
                        SafeReadMemory(yPosAddr, &yPos, sizeof(double));
                        SafeReadMemory(yVelAddr, &yVel, sizeof(double));
                        
                        // Apply the moveID
                        SafeWriteMemory(moveIDAddr, &p2DelayState.pendingMoveID, sizeof(short));
                        
                        // If not a jump move and in air, ensure we preserve air state
                        if (p2DelayState.pendingMoveID != STRAIGHT_JUMP_ID && 
                            p2DelayState.pendingMoveID != FORWARD_JUMP_ID && 
                            p2DelayState.pendingMoveID != BACKWARD_JUMP_ID && 
                            yPos > 0.0) {
                            
                            if (p2DelayState.pendingMoveID != FALLING_ID && 
                               (p2DelayState.pendingMoveID < BASE_ATTACK_JA || 
                                p2DelayState.pendingMoveID > BASE_ATTACK_JC)) {
                                
                                // Apply the attack but ensure we go back to falling state after
                                LogOut("[AUTO-ACTION] P2: Preserving air state after moveID " + 
                                      std::to_string(p2DelayState.pendingMoveID), true);
                                
                                // Ensure downward velocity for proper falling
                                double fallVelocity = yVel <= -1.0 ? yVel : -1.0;
                                SafeWriteMemory(yVelAddr, &fallVelocity, sizeof(double));
                                
                                // Set a timer to restore falling state
                                std::thread([=]() {
                                    Sleep(50); // Short delay for move to execute
                                    short fallingID = FALLING_ID;
                                    SafeWriteMemory(moveIDAddr, &fallingID, sizeof(short));
                                }).detach();
                            }
                        }
                    } else {
                        // Fallback to just applying the moveID
                        SafeWriteMemory(moveIDAddr, &p2DelayState.pendingMoveID, sizeof(short));
                    }
                } else {
                    // Standard trigger, apply moveID directly
                    SafeWriteMemory(moveIDAddr, &p2DelayState.pendingMoveID, sizeof(short));
                }
                
                LogOut("[AUTO-ACTION] P2 action applied: moveID " + 
                       std::to_string(p2DelayState.pendingMoveID), true);
                
                p2DelayState.isDelaying = false;
                p2DelayState.triggerType = TRIGGER_NONE;
                p2DelayState.pendingMoveID = 0;
                p2ActionApplied = true;
            } else {
                LogOut("[AUTO-ACTION] Failed to apply P2 action - invalid moveID address", true);
                p2DelayState.isDelaying = false;
            }
        }
    }
}

void StartTriggerDelay(int playerNum, int triggerType, short moveID, int delayFrames) {
    LogOut("[AUTO-ACTION] StartTriggerDelay called: Player=" + std::to_string(playerNum) + 
           ", delay=" + std::to_string(delayFrames) + ", moveID=" + std::to_string(moveID), true);
    
    // CRITICAL FIX: If delay is 0, apply immediately
    if (delayFrames == 0) {
        uintptr_t base = GetEFZBase();
        if (!base) {
            LogOut("[AUTO-ACTION] Failed to get base address", true);
            return;
        }
        
        // Determine whether we need to preserve air state
        bool isAirTechTrigger = (triggerType == TRIGGER_AFTER_AIRTECH);
        
        uintptr_t moveIDAddr;
        uintptr_t playerOffset;
        
        if (playerNum == 1) {
            playerOffset = EFZ_BASE_OFFSET_P1;
            moveIDAddr = ResolvePointer(base, EFZ_BASE_OFFSET_P1, MOVE_ID_OFFSET);
        } else {
            playerOffset = EFZ_BASE_OFFSET_P2;
            moveIDAddr = ResolvePointer(base, EFZ_BASE_OFFSET_P2, MOVE_ID_OFFSET);
        }
        
        if (moveIDAddr) {
            // CRITICAL FIX: For airtech triggers, preserve velocity and air state
            if (isAirTechTrigger) {
                // Get Y position and velocity
                uintptr_t yPosAddr = ResolvePointer(base, playerOffset, YPOS_OFFSET);
                uintptr_t yVelAddr = ResolvePointer(base, playerOffset, YVEL_OFFSET);
                
                if (yPosAddr && yVelAddr) {
                    double yPos = 0.0;
                    double yVel = 0.0;
                    
                    SafeReadMemory(yPosAddr, &yPos, sizeof(double));
                    SafeReadMemory(yVelAddr, &yVel, sizeof(double));
                    
                    // Set the requested moveID
                    SafeWriteMemory(moveIDAddr, &moveID, sizeof(short));
                    
                    // If not a jump and in air, ensure we stay in air with proper velocity
                    if (moveID != STRAIGHT_JUMP_ID && 
                        moveID != FORWARD_JUMP_ID && 
                        moveID != BACKWARD_JUMP_ID && 
                        yPos > 0.0) {
                        
                        // If move isn't already a jump or air attack, preserve the falling state
                        if (moveID != FALLING_ID && (moveID < BASE_ATTACK_JA || moveID > BASE_ATTACK_JC)) {
                            // Apply the attack but ensure we go back to falling state afterward
                            LogOut("[AUTO-ACTION] Preserving air state after moveID " + 
                                   std::to_string(moveID) + " application", true);
                            
                            // Ensure downward velocity for proper falling
                            double fallVelocity = yVel <= -1.0 ? yVel : -1.0;
                            SafeWriteMemory(yVelAddr, &fallVelocity, sizeof(double));
                            
                            // Set a timer to restore falling state
                            std::thread([=]() {
                                Sleep(50); // Short delay for move to execute
                                short fallingID = FALLING_ID;
                                SafeWriteMemory(moveIDAddr, &fallingID, sizeof(short));
                            }).detach();
                        }
                    }
                } else {
                    LogOut("[AUTO-ACTION] Failed to resolve position addresses", true);
                    SafeWriteMemory(moveIDAddr, &moveID, sizeof(short));
                }
            } else {
                // Non-airtech trigger, just apply moveID directly
                SafeWriteMemory(moveIDAddr, &moveID, sizeof(short));
            }
            
            LogOut("[AUTO-ACTION] Immediate action applied: Player " + std::to_string(playerNum) + 
                   " moveID " + std::to_string(moveID), true);
        } else {
            LogOut("[AUTO-ACTION] Failed to resolve moveID address", true);
        }
        
        return;
    }
    
    // For delayed actions, use the existing delay system
    int internalFrames = delayFrames * 3;
    
    if (playerNum == 1) {
        p1DelayState.isDelaying = true;
        p1DelayState.delayFramesRemaining = internalFrames;
        p1DelayState.triggerType = triggerType;
        p1DelayState.pendingMoveID = moveID;
        
        LogOut("[AUTO-ACTION] P1 delay started: " + std::to_string(delayFrames) + 
               " visual frames = " + std::to_string(internalFrames) + " internal frames", true);
    } else if (playerNum == 2) {
        p2DelayState.isDelaying = true;
        p2DelayState.delayFramesRemaining = internalFrames;
        p2DelayState.triggerType = triggerType;
        p2DelayState.pendingMoveID = moveID;
        
        LogOut("[AUTO-ACTION] P2 delay started: " + std::to_string(delayFrames) + 
               " visual frames = " + std::to_string(internalFrames) + " internal frames", true);
    }
}

void MonitorAutoActions() {
    if (!autoActionEnabled.load()) {
        return;
    }
    
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
        
        // After Block trigger
        if (triggerAfterBlockEnabled.load()) {
            if (IsBlockstun(prevMoveID1) && IsActionable(moveID1)) {
                shouldTrigger = true;
                triggerType = TRIGGER_AFTER_BLOCK;
                delay = triggerAfterBlockDelay.load();
                actionMoveID = GetActionMoveID(triggerAfterBlockAction.load(), TRIGGER_AFTER_BLOCK);
                
                LogOut("[AUTO-ACTION] P1 After Block trigger activated", true);
            }
        }
        
        // After Hitstun trigger
        if (!shouldTrigger && triggerAfterHitstunEnabled.load()) {
            if (IsHitstun(prevMoveID1) && !IsHitstun(moveID1) && !IsAirtech(moveID1)) {
                // CRITICAL FIX: Only trigger if not going into airtech
                if (IsActionable(moveID1)) {
                    shouldTrigger = true;
                    triggerType = TRIGGER_AFTER_HITSTUN;
                    delay = triggerAfterHitstunDelay.load();
                    actionMoveID = GetActionMoveID(triggerAfterHitstunAction.load(), TRIGGER_AFTER_HITSTUN);
                    LogOut("[AUTO-ACTION] P1 after hitstun trigger activated", true);
                }
            }
        }
        
        // On Wakeup trigger
        if (!shouldTrigger && triggerOnWakeupEnabled.load()) {
            if (IsGroundtech(prevMoveID1) && IsActionable(moveID1)) {
                shouldTrigger = true;
                triggerType = TRIGGER_ON_WAKEUP;
                delay = triggerOnWakeupDelay.load();
                actionMoveID = GetActionMoveID(triggerOnWakeupAction.load(), TRIGGER_ON_WAKEUP);
                
                LogOut("[AUTO-ACTION] P1 On Wakeup trigger activated", true);
            }
        }
        
        // CRITICAL FIX: Complete implementation of After Airtech trigger
        if (!shouldTrigger && triggerAfterAirtechEnabled.load()) {
            // Check if player was in airtech last frame
            bool wasInAirtech = (prevMoveID1 == FORWARD_AIRTECH || prevMoveID1 == BACKWARD_AIRTECH);
            
            // Check if player is now in first actionable frame after airtech
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
                actionMoveID = GetActionMoveID(actionType, TRIGGER_AFTER_AIRTECH);
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
        
        // After Block trigger
        if (triggerAfterBlockEnabled.load()) {
            if (IsBlockstun(prevMoveID2) && IsActionable(moveID2)) {
                shouldTrigger = true;
                triggerType = TRIGGER_AFTER_BLOCK;
                delay = triggerAfterBlockDelay.load();
                actionMoveID = GetActionMoveID(triggerAfterBlockAction.load(), TRIGGER_AFTER_BLOCK);
                
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
                    actionMoveID = GetActionMoveID(triggerAfterHitstunAction.load(), TRIGGER_AFTER_HITSTUN);
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
                actionMoveID = GetActionMoveID(triggerOnWakeupAction.load(), TRIGGER_ON_WAKEUP);
                
                LogOut("[AUTO-ACTION] P2 On Wakeup trigger activated", true);
            }
        }
        
        // CRITICAL FIX: Complete implementation of After Airtech trigger for P2
        if (!shouldTrigger && triggerAfterAirtechEnabled.load()) {
            // Check if player was in airtech last frame
            bool wasInAirtech = (prevMoveID2 == FORWARD_AIRTECH || prevMoveID2 == BACKWARD_AIRTECH);
            
            // Check if player is now in first actionable frame after airtech
            bool isNowActionable = (moveID2 == FALLING_ID);
            
            // P2 After-Airtech trigger condition
            if (wasInAirtech && isNowActionable) {
                LogOut("[AUTO-ACTION] P2 After Airtech trigger activated (from moveID " + 
                       std::to_string(prevMoveID2) + " to " + std::to_string(moveID2) + ")", true);
                shouldTrigger = true;
                triggerType = TRIGGER_AFTER_AIRTECH;
                delay = triggerAfterAirtechDelay.load();
                
                // Get the appropriate action moveID for After Airtech trigger
                int actionType = triggerAfterAirtechAction.load();
                actionMoveID = GetActionMoveID(actionType, TRIGGER_AFTER_AIRTECH);
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