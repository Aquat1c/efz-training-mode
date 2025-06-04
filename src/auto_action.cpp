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
        switch (triggerType) {
            case TRIGGER_AFTER_BLOCK:
                return triggerAfterBlockCustomID.load();
            case TRIGGER_ON_WAKEUP:
                return triggerOnWakeupCustomID.load();
            case TRIGGER_AFTER_HITSTUN:
                return triggerAfterHitstunCustomID.load();
            case TRIGGER_AFTER_AIRTECH:
                return triggerAfterAirtechCustomID.load();
            default:
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
    // P2 delay processing
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
            
            uintptr_t base = GetEFZBase();
            if (base) {
                uintptr_t moveIDAddr = ResolvePointer(base, EFZ_BASE_OFFSET_P2, MOVE_ID_OFFSET);
                if (moveIDAddr) {
                    short actionMoveID = p2DelayState.pendingMoveID;
                    
                    // Check if player is still actionable before applying
                    short currentMoveID = 0;
                    SafeReadMemory(moveIDAddr, &currentMoveID, sizeof(short));
                    
                    if (IsActionable(currentMoveID)) {
                        if (SafeWriteMemory(moveIDAddr, &actionMoveID, sizeof(short))) {
                            LogOut("[AUTO-ACTION] SUCCESS: Applied action moveID " + 
                                   std::to_string(actionMoveID) + " to P2 (was in state " + 
                                   std::to_string(currentMoveID) + ")", true);
                        } else {
                            LogOut("[AUTO-ACTION] FAILED: Could not write action to P2", true);
                        }
                    } else {
                        LogOut("[AUTO-ACTION] SKIPPED: P2 no longer actionable (moveID " + 
                               std::to_string(currentMoveID) + ")", true);
                    }
                } else {
                    LogOut("[AUTO-ACTION] FAILED: Could not resolve P2 moveID address", true);
                }
            } else {
                LogOut("[AUTO-ACTION] FAILED: Could not get game base address", true);
            }
            
            p2DelayState.isDelaying = false;
            p2DelayState.triggerType = TRIGGER_NONE;
            p2DelayState.pendingMoveID = 0;
            p2ActionApplied = true;
        }
    }
    
    // P1 delay processing (same logic)
    if (p1DelayState.isDelaying) {
        p1DelayState.delayFramesRemaining--;
        
        if (p1DelayState.delayFramesRemaining % 64 == 0 && p1DelayState.delayFramesRemaining > 0) {
            int visualFramesRemaining = p1DelayState.delayFramesRemaining / 3;
            LogOut("[AUTO-ACTION] P1 delay countdown: " + 
                   std::to_string(visualFramesRemaining) + " visual frames remaining (" +
                   std::to_string(p1DelayState.delayFramesRemaining) + " internal frames)", true);
        }
        
        if (p1DelayState.delayFramesRemaining <= 0) {
            LogOut("[AUTO-ACTION] P1 delay EXPIRED, applying action moveID " + 
                   std::to_string(p1DelayState.pendingMoveID), true);
            
            uintptr_t base = GetEFZBase();
            if (base) {
                uintptr_t moveIDAddr = ResolvePointer(base, EFZ_BASE_OFFSET_P1, MOVE_ID_OFFSET);
                if (moveIDAddr) {
                    short actionMoveID = p1DelayState.pendingMoveID;
                    
                    // Check if player is still actionable before applying
                    short currentMoveID = 0;
                    SafeReadMemory(moveIDAddr, &currentMoveID, sizeof(short));
                    
                    if (IsActionable(currentMoveID)) {
                        if (SafeWriteMemory(moveIDAddr, &actionMoveID, sizeof(short))) {
                            LogOut("[AUTO-ACTION] SUCCESS: Applied action moveID " + 
                                   std::to_string(actionMoveID) + " to P1 (was in state " + 
                                   std::to_string(currentMoveID) + ")", true);
                        } else {
                            LogOut("[AUTO-ACTION] FAILED: Could not write action to P1", true);
                        }
                    } else {
                        LogOut("[AUTO-ACTION] SKIPPED: P1 no longer actionable (moveID " + 
                               std::to_string(currentMoveID) + ")", true);
                    }
                } else {
                    LogOut("[AUTO-ACTION] FAILED: Could not resolve P1 moveID address", true);
                }
            } else {
                LogOut("[AUTO-ACTION] FAILED: Could not get game base address", true);
            }
            
            // When applying the action, add this protection:
            if (p1DelayState.delayFramesRemaining <= 0) {
                // Apply the action
                uintptr_t moveIDAddr1 = ResolvePointer(base, EFZ_BASE_OFFSET_P1, MOVE_ID_OFFSET);
                if (moveIDAddr1) {
                    LogOut("[AUTO-ACTION] P1 applying moveID " + std::to_string(p1DelayState.pendingMoveID), true);
                    
                    if (SafeWriteMemory(moveIDAddr1, &p1DelayState.pendingMoveID, sizeof(short))) {
                        // Verify the write immediately
                        short verifyMoveID = 0;
                        SafeReadMemory(moveIDAddr1, &verifyMoveID, sizeof(short));
                        
                        if (verifyMoveID == p1DelayState.pendingMoveID) {
                            LogOut("[AUTO-ACTION] P1 action applied successfully (verified: " + std::to_string(verifyMoveID) + ")", true);
                            p1ActionApplied = true;
                        } else {
                            LogOut("[AUTO-ACTION] P1 action verification failed! Expected " + 
                                   std::to_string(p1DelayState.pendingMoveID) + " but got " + std::to_string(verifyMoveID), true);
                        }
                    } else {
                        LogOut("[AUTO-ACTION] P1 memory write failed", true);
                    }
                }
                
                // Clear delay state
                p1DelayState.isDelaying = false;
                p1DelayState.triggerType = TRIGGER_NONE;
                p1DelayState.pendingMoveID = 0;
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
            LogOut("[AUTO-ACTION] Failed to get base address for immediate application", true);
            return;
        }
        
        uintptr_t moveIDAddr;
        if (playerNum == 1) {
            moveIDAddr = ResolvePointer(base, EFZ_BASE_OFFSET_P1, MOVE_ID_OFFSET);
        } else {
            moveIDAddr = ResolvePointer(base, EFZ_BASE_OFFSET_P2, MOVE_ID_OFFSET);
        }
        
        if (moveIDAddr) {
            LogOut("[AUTO-ACTION] Applying immediate action (0 delay): P" + std::to_string(playerNum) + 
                   " moveID " + std::to_string(moveID), true);
            
            if (SafeWriteMemory(moveIDAddr, &moveID, sizeof(short))) {
                // Verify the write
                short verifyMoveID = 0;
                SafeReadMemory(moveIDAddr, &verifyMoveID, sizeof(short));
                
                if (verifyMoveID == moveID) {
                    LogOut("[AUTO-ACTION] SUCCESS: P" + std::to_string(playerNum) + 
                           " immediate action applied (verified moveID: " + std::to_string(verifyMoveID) + ")", true);
                    
                    if (playerNum == 1) p1ActionApplied = true;
                    else if (playerNum == 2) p2ActionApplied = true;
                } else {
                    LogOut("[AUTO-ACTION] VERIFICATION FAILED: Expected " + std::to_string(moveID) + 
                           " but got " + std::to_string(verifyMoveID), true);
                }
            } else {
                LogOut("[AUTO-ACTION] MEMORY WRITE FAILED for immediate action", true);
            }
        } else {
            LogOut("[AUTO-ACTION] Failed to resolve moveID address for immediate application", true);
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
            // Check for the specific transition from airtech (157/158) to actionable state
            if (IsAirtech(prevMoveID1) && !IsAirtech(moveID1) && IsActionable(moveID1)) {
                shouldTrigger = true;
                triggerType = TRIGGER_AFTER_AIRTECH;
                delay = triggerAfterAirtechDelay.load();
                actionMoveID = GetActionMoveID(triggerAfterAirtechAction.load(), TRIGGER_AFTER_AIRTECH);
                LogOut("[AUTO-ACTION] P1 after airtech trigger activated", true);
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
            if (IsAirtech(prevMoveID2) && !IsAirtech(moveID2) && IsActionable(moveID2)) {
                shouldTrigger = true;
                triggerType = TRIGGER_AFTER_AIRTECH;
                delay = triggerAfterAirtechDelay.load();
                actionMoveID = GetActionMoveID(triggerAfterAirtechAction.load(), TRIGGER_AFTER_AIRTECH);
                
                LogOut("[AUTO-ACTION] P2 after airtech trigger activated", true);
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