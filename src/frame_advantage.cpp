#include "../include/frame_advantage.h"
#include "../include/constants.h"
#include "../include/utilities.h"
#include "../include/memory.h"
#include "../include/logger.h"
#include "../include/frame_analysis.h"
#include "../include/frame_monitor.h"

// Updated structure initialization with internal frames
FrameAdvantageState frameAdvState = {
    false, false, false, false,    // p1InBlockstun, p2InBlockstun, p1InHitstun, p2InHitstun
    false, false,                  // p1Attacking, p2Attacking
    -1, -1,                        // p1AttackStartInternalFrame, p2AttackStartInternalFrame
    0, 0,                          // p1AttackMoveID, p2AttackMoveID
    -1, -1,                        // p1BlockstunStartInternalFrame, p2BlockstunStartInternalFrame
    -1, -1,                        // p1HitstunStartInternalFrame, p2HitstunStartInternalFrame
    -1, -1,                        // p1ActionableInternalFrame, p2ActionableInternalFrame
    -1, -1,                        // p1DefenderFreeInternalFrame, p2DefenderFreeInternalFrame
    0.0, 0.0,                      // p1FrameAdvantage, p2FrameAdvantage (now double)
    false, false,                  // p1AdvantageCalculated, p2AdvantageCalculated
    0, 0                           // p1InitialBlockstunMoveID, p2InitialBlockstunMoveID
};

void ResetFrameAdvantageState() {
    frameAdvState.p1InBlockstun = false;
    frameAdvState.p2InBlockstun = false;
    frameAdvState.p1InHitstun = false;
    frameAdvState.p2InHitstun = false;
    frameAdvState.p1Attacking = false;
    frameAdvState.p2Attacking = false;
    frameAdvState.p1AttackStartInternalFrame = -1;
    frameAdvState.p2AttackStartInternalFrame = -1;
    frameAdvState.p1AttackMoveID = 0;
    frameAdvState.p2AttackMoveID = 0;
    frameAdvState.p1BlockstunStartInternalFrame = -1;
    frameAdvState.p2BlockstunStartInternalFrame = -1;
    frameAdvState.p1HitstunStartInternalFrame = -1;
    frameAdvState.p2HitstunStartInternalFrame = -1;
    frameAdvState.p1ActionableInternalFrame = -1;
    frameAdvState.p2ActionableInternalFrame = -1;
    frameAdvState.p1DefenderFreeInternalFrame = -1;
    frameAdvState.p2DefenderFreeInternalFrame = -1;
    frameAdvState.p1FrameAdvantage = 0.0;
    frameAdvState.p2FrameAdvantage = 0.0;
    frameAdvState.p1AdvantageCalculated = false;
    frameAdvState.p2AdvantageCalculated = false;
    frameAdvState.p1InitialBlockstunMoveID = 0;
    frameAdvState.p2InitialBlockstunMoveID = 0;
}

int GetCurrentInternalFrame() {
    return frameCounter.load();
}

double GetCurrentVisualFrame() {
    int internalFrame = frameCounter.load();
    
    // Convert internal frame to visual frame with subframe precision
    // Each visual frame = 3 internal frames
    // Subframe 0 = .00, subframe 1 = .33, subframe 2 = .66
    int visualFrameWhole = internalFrame / 3;
    int subframe = internalFrame % 3;
    
    double subframeDecimal = 0.0;
    switch (subframe) {
        case 0: subframeDecimal = 0.0; break;   // First subframe (.00)
        case 1: subframeDecimal = 0.33; break;  // Second subframe (.33) - EXACTLY 0.33
        case 2: subframeDecimal = 0.66; break;  // Third subframe (.66) - EXACTLY 0.66
    }
    
    return visualFrameWhole + subframeDecimal;
}

std::string FormatFrameAdvantage(double advantage) {
    // Format frame advantage with subframe precision
    if (advantage >= 0) {
        if (advantage == (int)advantage) {
            return "+" + std::to_string((int)advantage);
        } else {
            // Handle the exact subframe values
            double fractionalPart = advantage - (int)advantage;
            if (abs(fractionalPart - 0.33) < 0.001) {
                return "+" + std::to_string((int)advantage) + ".33";
            } else if (abs(fractionalPart - 0.66) < 0.001) {
                return "+" + std::to_string((int)advantage) + ".66";
            } else {
                char buffer[32];
                sprintf_s(buffer, "+%.2f", advantage);
                return std::string(buffer);
            }
        }
    } else {
        if (advantage == (int)advantage) {
            return std::to_string((int)advantage);
        } else {
            // Handle negative subframes correctly
            double fractionalPart = advantage - (int)advantage;
            if (abs(fractionalPart - 0.33) < 0.001) {
                return std::to_string((int)advantage) + ".33";
            } else if (abs(fractionalPart - 0.66) < 0.001) {
                return std::to_string((int)advantage) + ".66";
            } else {
                char buffer[32];
                sprintf_s(buffer, "%.2f", advantage);
                return std::string(buffer);
            }
        }
    }
}

bool IsAttackMove(short moveID) {
    // More precise attack detection
    return (moveID >= 200 && moveID <= 350) ||           
           (moveID >= 400 && moveID <= 500);
}

void MonitorFrameAdvantage(short moveID1, short moveID2, short prevMoveID1, short prevMoveID2) {
    int currentInternalFrame = GetCurrentInternalFrame();
    double currentVisualFrame = GetCurrentVisualFrame();
    
    // Reset if both players are neutral to prevent stale data
    if (IsActionable(moveID1) && IsActionable(moveID2) && 
        !frameAdvState.p1Attacking && !frameAdvState.p2Attacking) {
        ResetFrameAdvantageState();
        return;
    }
    
    // STEP 1: Detect when an attack CONNECTS (causes blockstun/hitstun)
    // P1 attacking P2 - Look for the EXACT transition into blockstun/hitstun
    if (!frameAdvState.p1Attacking) {
        // CRITICAL FIX: More comprehensive connection detection
        bool p2EnteredBlockstun = !IsBlockstunState(prevMoveID2) && IsBlockstunState(moveID2);
        bool p2EnteredHitstun = !IsHitstun(prevMoveID2) && IsHitstun(moveID2);
        bool p1WasAttacking = IsAttackMove(prevMoveID1) || IsAttackMove(moveID1);
        
        if ((p2EnteredBlockstun || p2EnteredHitstun) && p1WasAttacking) {
            frameAdvState.p1Attacking = true;
            frameAdvState.p1AttackStartInternalFrame = currentInternalFrame;
            frameAdvState.p1AttackMoveID = IsAttackMove(moveID1) ? moveID1 : prevMoveID1;
            frameAdvState.p2InBlockstun = IsBlockstunState(moveID2);
            frameAdvState.p2InHitstun = IsHitstun(moveID2);
            frameAdvState.p2BlockstunStartInternalFrame = currentInternalFrame;
            frameAdvState.p2InitialBlockstunMoveID = moveID2;
            frameAdvState.p1ActionableInternalFrame = -1;
            frameAdvState.p2DefenderFreeInternalFrame = -1;
            frameAdvState.p1AdvantageCalculated = false;
            
            LogOut("[FRAME ADVANTAGE] P1 attack connected at internal frame " + 
                   std::to_string(currentInternalFrame) + " (visual: " + std::to_string(currentVisualFrame) + ")" +
                   " - P2 in " + (IsBlockstunState(moveID2) ? "blockstun" : "hitstun") + 
                   " (moveID: " + std::to_string(moveID2) + ")", true);
        }
    }
    
    // P2 attacking P1 - Same logic
    if (!frameAdvState.p2Attacking) {
        bool p1EnteredBlockstun = !IsBlockstunState(prevMoveID1) && IsBlockstunState(moveID1);
        bool p1EnteredHitstun = !IsHitstun(prevMoveID1) && IsHitstun(moveID1);
        bool p2WasAttacking = IsAttackMove(prevMoveID2) || IsAttackMove(moveID2);
        
        if ((p1EnteredBlockstun || p1EnteredHitstun) && p2WasAttacking) {
            frameAdvState.p2Attacking = true;
            frameAdvState.p2AttackStartInternalFrame = currentInternalFrame;
            frameAdvState.p2AttackMoveID = IsAttackMove(moveID2) ? moveID2 : prevMoveID2;
            frameAdvState.p1InBlockstun = IsBlockstunState(moveID1);
            frameAdvState.p1InHitstun = IsHitstun(moveID1);
            frameAdvState.p1BlockstunStartInternalFrame = currentInternalFrame;
            frameAdvState.p1InitialBlockstunMoveID = moveID1;
            frameAdvState.p2ActionableInternalFrame = -1;
            frameAdvState.p1DefenderFreeInternalFrame = -1;
            frameAdvState.p2AdvantageCalculated = false;
            
            LogOut("[FRAME ADVANTAGE] P2 attack connected at internal frame " + 
                   std::to_string(currentInternalFrame) + " (visual: " + std::to_string(currentVisualFrame) + ")" +
                   " - P1 in " + (IsBlockstunState(moveID1) ? "blockstun" : "hitstun") + 
                   " (moveID: " + std::to_string(moveID1) + ")", true);
        }
    }
    
    // STEP 2: Detect when attacker exits recovery - IMPROVED DETECTION
    if (frameAdvState.p1Attacking && frameAdvState.p1ActionableInternalFrame == -1) {
        // CRITICAL FIX: Multiple detection methods for better reliability
        bool wasInAttack = IsAttackMove(prevMoveID1);
        bool nowActionable = IsActionable(moveID1);
        bool wasNotActionable = !IsActionable(prevMoveID1);
        
        // Method 1: Direct transition from attack to actionable
        bool exitedAttackToActionable = wasInAttack && nowActionable;
        
        // Method 2: General non-actionable to actionable transition
        bool becameActionable = wasNotActionable && nowActionable;
        
        // Method 3: Check if we're in a neutral state after being in attack
        bool inNeutralAfterAttack = (wasInAttack || IsAttackMove(frameAdvState.p1AttackMoveID)) && 
                                   (moveID1 == IDLE_MOVE_ID || moveID1 == WALK_FWD_ID || 
                                    moveID1 == WALK_BACK_ID || moveID1 == CROUCH_ID ||
                                    moveID1 == LANDING_ID);
        
        if (exitedAttackToActionable || becameActionable || inNeutralAfterAttack) {
            frameAdvState.p1ActionableInternalFrame = currentInternalFrame;
            LogOut("[FRAME ADVANTAGE] P1 (attacker) became actionable at internal frame " + 
                   std::to_string(currentInternalFrame) + " (visual: " + std::to_string(currentVisualFrame) + ")" +
                   " | moveID: " + std::to_string(prevMoveID1) + " -> " + std::to_string(moveID1), true);
        }
    }
    
    if (frameAdvState.p2Attacking && frameAdvState.p2ActionableInternalFrame == -1) {
        bool wasInAttack = IsAttackMove(prevMoveID2);
        bool nowActionable = IsActionable(moveID2);
        bool wasNotActionable = !IsActionable(prevMoveID2);
        
        bool exitedAttackToActionable = wasInAttack && nowActionable;
        bool becameActionable = wasNotActionable && nowActionable;
        bool inNeutralAfterAttack = (wasInAttack || IsAttackMove(frameAdvState.p2AttackMoveID)) && 
                                   (moveID2 == IDLE_MOVE_ID || moveID2 == WALK_FWD_ID || 
                                    moveID2 == WALK_BACK_ID || moveID2 == CROUCH_ID ||
                                    moveID2 == LANDING_ID);
        
        if (exitedAttackToActionable || becameActionable || inNeutralAfterAttack) {
            frameAdvState.p2ActionableInternalFrame = currentInternalFrame;
            LogOut("[FRAME ADVANTAGE] P2 (attacker) became actionable at internal frame " + 
                   std::to_string(currentInternalFrame) + " (visual: " + std::to_string(currentVisualFrame) + ")" +
                   " | moveID: " + std::to_string(prevMoveID2) + " -> " + std::to_string(moveID2), true);
        }
    }
    
    // STEP 3: Detect when defender exits blockstun/hitstun - IMPROVED DETECTION
    if (frameAdvState.p1Attacking && frameAdvState.p2DefenderFreeInternalFrame == -1) {
        bool wasInBlockstun = IsBlockstunState(prevMoveID2);
        bool wasInHitstun = IsHitstun(prevMoveID2);
        bool nowActionable = IsActionable(moveID2);
        
        // CRITICAL FIX: Detect exact transition out of stun states
        bool exitedBlockstun = wasInBlockstun && !IsBlockstunState(moveID2);
        bool exitedHitstun = wasInHitstun && !IsHitstun(moveID2);
        bool exitedToActionable = (exitedBlockstun || exitedHitstun) && nowActionable;
        
        // Also check for direct transition to neutral states
        bool exitedToNeutral = (wasInBlockstun || wasInHitstun) && 
                              (moveID2 == IDLE_MOVE_ID || moveID2 == WALK_FWD_ID || 
                               moveID2 == WALK_BACK_ID || moveID2 == CROUCH_ID ||
                               moveID2 == LANDING_ID);
        
        if (exitedToActionable || exitedToNeutral) {
            frameAdvState.p2DefenderFreeInternalFrame = currentInternalFrame;
            LogOut("[FRAME ADVANTAGE] P2 (defender) exited stun at internal frame " + 
                   std::to_string(currentInternalFrame) + " (visual: " + std::to_string(currentVisualFrame) + ")" +
                   " | moveID: " + std::to_string(prevMoveID2) + " -> " + std::to_string(moveID2), true);
        }
    }
    
    if (frameAdvState.p2Attacking && frameAdvState.p1DefenderFreeInternalFrame == -1) {
        bool wasInBlockstun = IsBlockstunState(prevMoveID1);
        bool wasInHitstun = IsHitstun(prevMoveID1);
        bool nowActionable = IsActionable(moveID1);
        
        bool exitedBlockstun = wasInBlockstun && !IsBlockstunState(moveID1);
        bool exitedHitstun = wasInHitstun && !IsHitstun(moveID1);
        bool exitedToActionable = (exitedBlockstun || exitedHitstun) && nowActionable;
        
        bool exitedToNeutral = (wasInBlockstun || wasInHitstun) && 
                              (moveID1 == IDLE_MOVE_ID || moveID1 == WALK_FWD_ID || 
                               moveID1 == WALK_BACK_ID || moveID1 == CROUCH_ID ||
                               moveID1 == LANDING_ID);
        
        if (exitedToActionable || exitedToNeutral) {
            frameAdvState.p1DefenderFreeInternalFrame = currentInternalFrame;
            LogOut("[FRAME ADVANTAGE] P1 (defender) exited stun at internal frame " + 
                   std::to_string(currentInternalFrame) + " (visual: " + std::to_string(currentVisualFrame) + ")" +
                   " | moveID: " + std::to_string(prevMoveID1) + " -> " + std::to_string(moveID1), true);
        }
    }
    
    // STEP 4: Calculate frame advantage with PRECISE subframe calculation
    if (frameAdvState.p1Attacking && !frameAdvState.p1AdvantageCalculated &&
        frameAdvState.p1ActionableInternalFrame != -1 && frameAdvState.p2DefenderFreeInternalFrame != -1) {
        
        // CRITICAL: Calculate advantage in internal frames first
        int internalFrameAdvantage = frameAdvState.p2DefenderFreeInternalFrame - frameAdvState.p1ActionableInternalFrame;
        
        // Convert to visual frames with exact subframe precision
        double visualFrameAdvantage;
        if (internalFrameAdvantage == 0) {
            visualFrameAdvantage = 0.0;  // Exactly 0 frames
        } else {
            int wholeFrames = internalFrameAdvantage / 3;
            int subframeRemainder = internalFrameAdvantage % 3;
            
            if (internalFrameAdvantage < 0) {
                int absInternalFrames = abs(internalFrameAdvantage);
                wholeFrames = -(absInternalFrames / 3);
                subframeRemainder = absInternalFrames % 3;
                if (subframeRemainder > 0) {
                    wholeFrames -= 1;
                    subframeRemainder = 3 - subframeRemainder;
                }
            }
            
            switch (subframeRemainder) {
                case 0:
                    visualFrameAdvantage = (double)wholeFrames;
                    break;
                case 1:
                    visualFrameAdvantage = (double)wholeFrames + 0.33;
                    break;
                case 2:
                    visualFrameAdvantage = (double)wholeFrames + 0.66;
                    break;
                default:
                    visualFrameAdvantage = (double)wholeFrames;
                    break;
            }
        }
        
        frameAdvState.p1FrameAdvantage = visualFrameAdvantage;
        frameAdvState.p1AdvantageCalculated = true;
        
        // Get attack info for context
        int attackLevel = GetAttackLevel(frameAdvState.p2InitialBlockstunMoveID);
        bool wasBlocked = frameAdvState.p2InBlockstun;
        std::string advantageStr = FormatFrameAdvantage(visualFrameAdvantage);
        
        LogOut("[FRAME ADVANTAGE] P1 " + advantageStr + " frames" + 
               " (Level " + std::to_string(attackLevel) + " " + (wasBlocked ? "blocked" : "hit") + ")" +
               " | Internal advantage: " + std::to_string(internalFrameAdvantage) +
               " | Attacker free: " + std::to_string(frameAdvState.p1ActionableInternalFrame) + 
               " | Defender free: " + std::to_string(frameAdvState.p2DefenderFreeInternalFrame), true);
        
        frameAdvState.p1Attacking = false;
    }
    
    if (frameAdvState.p2Attacking && !frameAdvState.p2AdvantageCalculated &&
        frameAdvState.p2ActionableInternalFrame != -1 && frameAdvState.p1DefenderFreeInternalFrame != -1) {
        
        int internalFrameAdvantage = frameAdvState.p1DefenderFreeInternalFrame - frameAdvState.p2ActionableInternalFrame;
        
        double visualFrameAdvantage;
        if (internalFrameAdvantage == 0) {
            visualFrameAdvantage = 0.0;
        } else {
            int wholeFrames = internalFrameAdvantage / 3;
            int subframeRemainder = internalFrameAdvantage % 3;
            
            if (internalFrameAdvantage < 0) {
                int absInternalFrames = abs(internalFrameAdvantage);
                wholeFrames = -(absInternalFrames / 3);
                subframeRemainder = absInternalFrames % 3;
                if (subframeRemainder > 0) {
                    wholeFrames -= 1;
                    subframeRemainder = 3 - subframeRemainder;
                }
            }
            
            switch (subframeRemainder) {
                case 0:
                    visualFrameAdvantage = (double)wholeFrames;
                    break;
                case 1:
                    visualFrameAdvantage = (double)wholeFrames + 0.33;
                    break;
                case 2:
                    visualFrameAdvantage = (double)wholeFrames + 0.66;
                    break;
                default:
                    visualFrameAdvantage = (double)wholeFrames;
                    break;
            }
        }
        
        frameAdvState.p2FrameAdvantage = visualFrameAdvantage;
        frameAdvState.p2AdvantageCalculated = true;
        
        int attackLevel = GetAttackLevel(frameAdvState.p1InitialBlockstunMoveID);
        bool wasBlocked = frameAdvState.p1InBlockstun;
        std::string advantageStr = FormatFrameAdvantage(visualFrameAdvantage);
        
        LogOut("[FRAME ADVANTAGE] P2 " + advantageStr + " frames" + 
               " (Level " + std::to_string(attackLevel) + " " + (wasBlocked ? "blocked" : "hit") + ")" +
               " | Internal advantage: " + std::to_string(internalFrameAdvantage) +
               " | Attacker free: " + std::to_string(frameAdvState.p2ActionableInternalFrame) + 
               " | Defender free: " + std::to_string(frameAdvState.p1DefenderFreeInternalFrame), true);
        
        frameAdvState.p2Attacking = false;
    }
    
    // STEP 5: Auto-reset if tracking goes stale
    static int idleFrameCount = 0;
    if (IsActionable(moveID1) && IsActionable(moveID2) && 
        (frameAdvState.p1Attacking || frameAdvState.p2Attacking)) {
        idleFrameCount++;
        if (idleFrameCount > 360) { // 2 seconds at 192 internal FPS
            LogOut("[FRAME ADVANTAGE] Resetting stale tracking", true);
            ResetFrameAdvantageState();
            idleFrameCount = 0;
        }
    } else {
        idleFrameCount = 0;
    }
}

bool IsFrameAdvantageActive() {
    return frameAdvState.p1Attacking || frameAdvState.p2Attacking;
}

FrameAdvantageState GetFrameAdvantageState() {
    return frameAdvState;
}