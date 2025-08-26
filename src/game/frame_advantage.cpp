#include "../include/game/frame_advantage.h"
#include "../include/core/constants.h"
#include "../include/utils/utilities.h"

#include "../include/core/memory.h"
#include "../include/core/logger.h"
#include "../include/game/frame_analysis.h"
#include "../include/game/frame_monitor.h"
#include "../include/gui/overlay.h"

// Global state for frame advantage tracking
FrameAdvantageState frameAdvState = {
    false, false, false, false,    // p1InBlockstun, p2InBlockstun, p1InHitstun, p2InHitstun
    false, false,                  // p1Defending, p2Defending
    false, false,                  // p1Attacking, p2Attacking
    -1, -1,                        // p1AttackStartInternalFrame, p2AttackStartInternalFrame
    0, 0,                          // p1AttackMoveID, p2AttackMoveID
    -1, -1,                        // p1BlockstunStartInternalFrame, p2BlockstunStartInternalFrame
    -1, -1,                        // p1HitstunStartInternalFrame, p2HitstunStartInternalFrame
    -1, -1,                        // p1ActionableInternalFrame, p2ActionableInternalFrame
    -1, -1,                        // p1DefenderFreeInternalFrame, p2DefenderFreeInternalFrame
    0.0, 0.0,                      // p1FrameAdvantage, p2FrameAdvantage
    false, false,                  // p1AdvantageCalculated, p2AdvantageCalculated
    0, 0,                          // p1InitialBlockstunMoveID, p2InitialBlockstunMoveID
    -1                             // displayUntilInternalFrame
};

void ResetFrameAdvantageState() {
    frameAdvState.p1InBlockstun = false;
    frameAdvState.p2InBlockstun = false;
    frameAdvState.p1InHitstun = false;
    frameAdvState.p2InHitstun = false;
    frameAdvState.p1Defending = false;
    frameAdvState.p2Defending = false;
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
    frameAdvState.displayUntilInternalFrame = -1;
    
    if (detailedLogging.load()) {
    #if defined(ENABLE_FRAME_ADV_DEBUG)
    LogOut("[FRAME_ADV_DEBUG] Frame advantage state reset", false);
    #endif
    }

    // Clear any existing display
    if (g_FrameAdvantageId != -1) {
        DirectDrawHook::RemovePermanentMessage(g_FrameAdvantageId);
        g_FrameAdvantageId = -1;
    }
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
        case 1: subframeDecimal = 0.33; break;  // Second subframe (.33)
        case 2: subframeDecimal = 0.66; break;  // Third subframe (.66)
    }
    
    return visualFrameWhole + subframeDecimal;
}

// Fix the FormatFrameAdvantage function to handle signs properly
std::string FormatFrameAdvantage(int advantageInternal) {
    // Convert internal frames to visual frames with subframe precision
    int visualFrames = advantageInternal / 3;
    int subframes = std::abs(advantageInternal % 3);

    // Only add the sign for positive values - this prevents double + signs later
    std::string sign = (advantageInternal >= 0) ? "+" : "";
    std::string subframeStr = "";
    
    if (subframes == 1) {
        subframeStr = ".33";
    } else if (subframes == 2) {
        subframeStr = ".66";
    }
    
    // Special case for negative values where the whole part is zero
    if (visualFrames == 0 && advantageInternal < 0) {
        return "-0" + subframeStr;
    }

    return sign + std::to_string(visualFrames) + subframeStr;
}

bool IsAttackMove(short moveID) {
    // Attack moves are typically in specific ID ranges
    return (moveID >= 200 && moveID <= 350) ||           
           (moveID >= 400 && moveID <= 500);
}

void MonitorFrameAdvantage(short moveID1, short moveID2, short prevMoveID1, short prevMoveID2) {
    int currentInternalFrame = GetCurrentInternalFrame();
    
    // For gap detection
    static int p1_last_defender_free_frame = -1;
    static int p2_last_defender_free_frame = -1;
    
    // Cooldowns for hit detection - REDUCED to improve string detection
    static int p1_hit_connect_cooldown = 0;
    static int p2_hit_connect_cooldown = 0;
    
    // Reduce cooldowns
    if (p1_hit_connect_cooldown > 0) p1_hit_connect_cooldown--;
    if (p2_hit_connect_cooldown > 0) p2_hit_connect_cooldown--;
    
    // Log the current state for debugging
    static int debugCounter = 0;
    if (detailedLogging.load() && ++debugCounter % 60 == 0) {
     #if defined(ENABLE_FRAME_ADV_DEBUG)
     LogOut("[FRAME_ADV_DEBUG] State: p1Block=" + std::to_string(frameAdvState.p1InBlockstun) + 
               " p2Block=" + std::to_string(frameAdvState.p2InBlockstun) + 
               " p1Def=" + std::to_string(frameAdvState.p1Defending) + 
               " p2Def=" + std::to_string(frameAdvState.p2Defending) + 
               " p1Atk=" + std::to_string(frameAdvState.p1Attacking) + 
         " p2Atk=" + std::to_string(frameAdvState.p2Attacking), 
         false);
     #endif
    }
    
    // Check if the display timer has expired
    if (frameAdvState.displayUntilInternalFrame != -1 && currentInternalFrame >= frameAdvState.displayUntilInternalFrame) {
        // Only clear the display, not the entire state
        if (g_FrameAdvantageId != -1) {
            DirectDrawHook::RemovePermanentMessage(g_FrameAdvantageId);
            g_FrameAdvantageId = -1;
        }
        frameAdvState.displayUntilInternalFrame = -1;
    }
    
    // Update player states for proper tracking
    bool p1Actionable = IsActionable(moveID1);
    bool p2Actionable = IsActionable(moveID2);
    
    // Reset if both players have been in neutral for several frames
    static int neutralFrameCount = 0;
    if (p1Actionable && p2Actionable && !IsAttackMove(moveID1) && !IsAttackMove(moveID2)) {
        neutralFrameCount++;
        if (neutralFrameCount > 30) { // 0.5 seconds
            ResetFrameAdvantageState();
            neutralFrameCount = 0;
            // But keep these for gap detection
            p1_last_defender_free_frame = -1;
            p2_last_defender_free_frame = -1;
        }
    } else {
        neutralFrameCount = 0;
    }
    
    // Detect when defender exits blockstun/hitstun (This is for gap detection)
    bool p1_exiting_stun = (IsBlockstunState(prevMoveID1) || IsHitstun(prevMoveID1)) && 
                           !(IsBlockstunState(moveID1) || IsHitstun(moveID1));
    
    bool p2_exiting_stun = (IsBlockstunState(prevMoveID2) || IsHitstun(prevMoveID2)) && 
                           !(IsBlockstunState(moveID2) || IsHitstun(moveID2));
    
    if (p1_exiting_stun) {
        p1_last_defender_free_frame = currentInternalFrame;
         #if defined(ENABLE_FRAME_ADV_DEBUG)
         LogOut("[FRAME_ADV_DEBUG] P1 exited stun at frame " + std::to_string(currentInternalFrame), 
             detailedLogging.load());
         #endif
    }
    
    if (p2_exiting_stun) {
        p2_last_defender_free_frame = currentInternalFrame;
         #if defined(ENABLE_FRAME_ADV_DEBUG)
         LogOut("[FRAME_ADV_DEBUG] P2 exited stun at frame " + std::to_string(currentInternalFrame), 
             detailedLogging.load());
         #endif
    }
    
    // STEP 1: Detect if an attack connects (P1 attacking P2)
    bool p2_entering_blockstun = IsBlockstunState(moveID2) && !IsBlockstunState(prevMoveID2);
    bool p2_entering_hitstun = IsHitstun(moveID2) && !IsHitstun(prevMoveID2);
    
    if ((p2_entering_blockstun || p2_entering_hitstun) && p1_hit_connect_cooldown == 0) {
        // Check for gap between moves in a string
        if (p2_last_defender_free_frame != -1) {
            int gapFrames = currentInternalFrame - p2_last_defender_free_frame;
            
            // Only consider gaps that are reasonably small
            if (gapFrames > 0 && gapFrames <= 60) {
                // Format gap WITHOUT a plus sign
                int visualGapFrames = gapFrames / 3;
                int subframes = gapFrames % 3;
                
                std::string gapText = "Gap: " + std::to_string(visualGapFrames);
                
                // Add subframe precision if needed
                if (subframes == 1) {
                    gapText += ".33";
                } else if (subframes == 2) {
                    gapText += ".66";
                }
                
                if (g_FrameAdvantageId != -1) {
                    DirectDrawHook::UpdatePermanentMessage(g_FrameAdvantageId, gapText, RGB(255, 255, 0));
                } else {
                    g_FrameAdvantageId = DirectDrawHook::AddPermanentMessage(gapText, RGB(255, 255, 0), 305, 430);
                }
                
                // Display for ~1/3 second (60 internal frames)
                frameAdvState.displayUntilInternalFrame = currentInternalFrame + 60;
                
                LogOut("[FRAME_ADV] Gap detected: " + gapText, true);
            }
        }
        
        // CRUCIAL CHANGE: Don't reset the entire state for strings of attacks
        // Instead, just update the attack/defend state for this new hit
        
        // Clear any previous attack tracking but keep the rest of the state
        if (frameAdvState.p1Attacking || frameAdvState.p2Attacking) {
            // Just clear attack state variables
            frameAdvState.p1Attacking = false;
            frameAdvState.p2Attacking = false;
            frameAdvState.p1AttackStartInternalFrame = -1;
            frameAdvState.p2AttackStartInternalFrame = -1;
            frameAdvState.p1ActionableInternalFrame = -1;
            frameAdvState.p2ActionableInternalFrame = -1;
            frameAdvState.p1AdvantageCalculated = false;
            frameAdvState.p2AdvantageCalculated = false;
        }
        
        // Always clear previous calculation flags on a brand-new hit to avoid alternating suppression
        frameAdvState.p1AdvantageCalculated = false;
        frameAdvState.p2AdvantageCalculated = false;
        
        // Set up new attack state
        frameAdvState.p1Attacking = true;
        frameAdvState.p2Defending = true;
        frameAdvState.p1Defending = false; // Opponent is attacking, so P1 is not defending in this exchange
        frameAdvState.p1AttackStartInternalFrame = currentInternalFrame;
        frameAdvState.p2DefenderFreeInternalFrame = -1; // Reset only this defender variable
        
        if (p2_entering_blockstun) {
            frameAdvState.p2InBlockstun = true;
            frameAdvState.p2InHitstun = false;
            frameAdvState.p2BlockstunStartInternalFrame = currentInternalFrame;
            frameAdvState.p2InitialBlockstunMoveID = moveID2;
        } else {
            frameAdvState.p2InBlockstun = false;
            frameAdvState.p2InHitstun = true;
            frameAdvState.p2HitstunStartInternalFrame = currentInternalFrame;
        }
        
         #if defined(ENABLE_FRAME_ADV_DEBUG)
         LogOut("[FRAME_ADV_DEBUG] P1->P2 hit connected at frame " + std::to_string(currentInternalFrame), 
             detailedLogging.load());
         #endif
        
        // Reset this for accurate gap detection in the next sequence
        p2_last_defender_free_frame = -1;
        
        // Apply a minimal cooldown (just 3 internal frames = 1 visual frame)
        p1_hit_connect_cooldown = 3;
    }
    
    // STEP 2: Detect if an attack connects (P2 attacking P1) - mirror of P1 logic
    bool p1_entering_blockstun = IsBlockstunState(moveID1) && !IsBlockstunState(prevMoveID1);
    bool p1_entering_hitstun = IsHitstun(moveID1) && !IsHitstun(prevMoveID1);
    
    if ((p1_entering_blockstun || p1_entering_hitstun) && p2_hit_connect_cooldown == 0) {
        // Check for gap between moves in a string
        if (p1_last_defender_free_frame != -1) {
            int gapFrames = currentInternalFrame - p1_last_defender_free_frame;
            
            // Only consider gaps that are reasonably small
            if (gapFrames > 0 && gapFrames <= 60) {
                // Format gap WITHOUT a plus sign
                int visualGapFrames = gapFrames / 3;
                int subframes = gapFrames % 3;
                
                std::string gapText = "Gap: " + std::to_string(visualGapFrames);
                
                // Add subframe precision if needed
                if (subframes == 1) {
                    gapText += ".33";
                } else if (subframes == 2) {
                    gapText += ".66";
                }
                
                if (g_FrameAdvantageId != -1) {
                    DirectDrawHook::UpdatePermanentMessage(g_FrameAdvantageId, gapText, RGB(255, 255, 0));
                } else {
                    g_FrameAdvantageId = DirectDrawHook::AddPermanentMessage(gapText, RGB(255, 255, 0), 305, 430);
                }
                
                // Display for ~1/3 second (60 internal frames)
                frameAdvState.displayUntilInternalFrame = currentInternalFrame + 60;
                
                LogOut("[FRAME_ADV] Gap detected: " + gapText, true);
            }
        }
        
        // CRUCIAL CHANGE: Don't reset the entire state for strings of attacks
        // Instead, just update the attack/defend state for this new hit
        
        // Clear any previous attack tracking but keep the rest of the state
        if (frameAdvState.p1Attacking || frameAdvState.p2Attacking) {
            // Just clear attack state variables
            frameAdvState.p1Attacking = false;
            frameAdvState.p2Attacking = false;
            frameAdvState.p1AttackStartInternalFrame = -1;
            frameAdvState.p2AttackStartInternalFrame = -1;
            frameAdvState.p1ActionableInternalFrame = -1;
            frameAdvState.p2ActionableInternalFrame = -1;
            frameAdvState.p1AdvantageCalculated = false;
            frameAdvState.p2AdvantageCalculated = false;
        }
        
        // Always clear previous calculation flags on a brand-new hit to avoid alternating suppression
        frameAdvState.p1AdvantageCalculated = false;
        frameAdvState.p2AdvantageCalculated = false;
        
        // Set up new attack state
        frameAdvState.p2Attacking = true;
        frameAdvState.p1Defending = true;
        frameAdvState.p2Defending = false; // Opponent is attacking, so P2 is not defending in this exchange
        frameAdvState.p2AttackStartInternalFrame = currentInternalFrame;
        frameAdvState.p1DefenderFreeInternalFrame = -1; // Reset only this defender variable
        
        if (p1_entering_blockstun) {
            frameAdvState.p1InBlockstun = true;
            frameAdvState.p1InHitstun = false;
            frameAdvState.p1BlockstunStartInternalFrame = currentInternalFrame;
            frameAdvState.p1InitialBlockstunMoveID = moveID1;
        } else {
            frameAdvState.p1InBlockstun = false;
            frameAdvState.p1InHitstun = true;
            frameAdvState.p1HitstunStartInternalFrame = currentInternalFrame;
        }
        
         #if defined(ENABLE_FRAME_ADV_DEBUG)
         LogOut("[FRAME_ADV_DEBUG] P2->P1 hit connected at frame " + std::to_string(currentInternalFrame), 
             detailedLogging.load());
         #endif
        
        // Reset this for accurate gap detection in the next sequence
        p1_last_defender_free_frame = -1;
        
        // Apply a minimal cooldown (just 3 internal frames = 1 visual frame)
        p2_hit_connect_cooldown = 3;
    }
    
    // STEP 3: Detect when attacker exits recovery
    if (frameAdvState.p1Attacking && frameAdvState.p1ActionableInternalFrame == -1) {
        if (!IsActionable(prevMoveID1) && IsActionable(moveID1)) {
            frameAdvState.p1ActionableInternalFrame = currentInternalFrame;
         #if defined(ENABLE_FRAME_ADV_DEBUG)
         LogOut("[FRAME_ADV_DEBUG] P1 attacker recovery ended at frame " + 
             std::to_string(currentInternalFrame), detailedLogging.load());
         #endif
        }
    }
    
    if (frameAdvState.p2Attacking && frameAdvState.p2ActionableInternalFrame == -1) {
        if (!IsActionable(prevMoveID2) && IsActionable(moveID2)) {
            frameAdvState.p2ActionableInternalFrame = currentInternalFrame;
         #if defined(ENABLE_FRAME_ADV_DEBUG)
         LogOut("[FRAME_ADV_DEBUG] P2 attacker recovery ended at frame " + 
             std::to_string(currentInternalFrame), detailedLogging.load());
         #endif
        }
    }
    
    // STEP 4: Detect when defender exits blockstun/hitstun
    if (frameAdvState.p2Defending && frameAdvState.p2DefenderFreeInternalFrame == -1) {
        bool wasInStun = IsBlockstunState(prevMoveID2) || IsHitstun(prevMoveID2);
        bool nowNotInStun = !IsBlockstunState(moveID2) && !IsHitstun(moveID2);
        
        if (wasInStun && nowNotInStun) {
            frameAdvState.p2DefenderFreeInternalFrame = currentInternalFrame;
         #if defined(ENABLE_FRAME_ADV_DEBUG)
         LogOut("[FRAME_ADV_DEBUG] P2 defender freed at frame " + 
             std::to_string(currentInternalFrame), detailedLogging.load());
         #endif
        }
    }
    
    if (frameAdvState.p1Defending && frameAdvState.p1DefenderFreeInternalFrame == -1) {
        bool wasInStun = IsBlockstunState(prevMoveID1) || IsHitstun(prevMoveID1);
        bool nowNotInStun = !IsBlockstunState(moveID1) && !IsHitstun(moveID1);
        
        if (wasInStun && nowNotInStun) {
            frameAdvState.p1DefenderFreeInternalFrame = currentInternalFrame;
         #if defined(ENABLE_FRAME_ADV_DEBUG)
         LogOut("[FRAME_ADV_DEBUG] P1 defender freed at frame " + 
             std::to_string(currentInternalFrame), detailedLogging.load());
         #endif
        }
    }
    
    // STEP 5: Calculate frame advantage when all necessary data is available
    if (frameAdvState.p1Attacking && !frameAdvState.p1AdvantageCalculated &&
        frameAdvState.p1ActionableInternalFrame != -1 && frameAdvState.p2DefenderFreeInternalFrame != -1) {
        
        // Calculate frame advantage (defender free - attacker actionable)
        // Positive: Attacker has advantage
        // Negative: Defender has advantage
        int frameAdvantage = frameAdvState.p2DefenderFreeInternalFrame - frameAdvState.p1ActionableInternalFrame;
        frameAdvState.p1FrameAdvantage = frameAdvantage;
        frameAdvState.p1AdvantageCalculated = true;
        
        // Format the advantage display with proper sign and subframe precision
        // FormatFrameAdvantage already includes the "+" for positive values
        std::string frameAdvText = FormatFrameAdvantage(frameAdvantage);
        
        // Display the calculated advantage
        if (g_FrameAdvantageId != -1) {
            DirectDrawHook::UpdatePermanentMessage(g_FrameAdvantageId, frameAdvText, 
                frameAdvantage >= 0 ? RGB(0, 255, 0) : RGB(255, 0, 0));
        } else {
            g_FrameAdvantageId = DirectDrawHook::AddPermanentMessage(frameAdvText, 
                frameAdvantage >= 0 ? RGB(0, 255, 0) : RGB(255, 0, 0), 305, 430);
        }
        
        // Set display duration (show for approximately 1 second)
        frameAdvState.displayUntilInternalFrame = currentInternalFrame + 192;
        
        LogOut("[FRAME_ADV] P1->P2 Frame Advantage: " + frameAdvText, true);
        
        // Reset attack state for the next sequence while preserving defender state
        frameAdvState.p1Attacking = false;
        frameAdvState.p1AttackStartInternalFrame = -1;
        frameAdvState.p1ActionableInternalFrame = -1;
    }
    
    if (frameAdvState.p2Attacking && !frameAdvState.p2AdvantageCalculated &&
        frameAdvState.p2ActionableInternalFrame != -1 && frameAdvState.p1DefenderFreeInternalFrame != -1) {
        
        // Calculate advantage (defender free - attacker actionable)
        int frameAdvantage = frameAdvState.p1DefenderFreeInternalFrame - frameAdvState.p2ActionableInternalFrame;
        frameAdvState.p2FrameAdvantage = frameAdvantage;
        frameAdvState.p2AdvantageCalculated = true;
        
        // Format the advantage display with proper sign and subframe precision
        // FormatFrameAdvantage already includes the "+" for positive values
        std::string frameAdvText = FormatFrameAdvantage(frameAdvantage);
        
        // Display the calculated advantage
        if (g_FrameAdvantageId != -1) {
            DirectDrawHook::UpdatePermanentMessage(g_FrameAdvantageId, frameAdvText, 
                frameAdvantage >= 0 ? RGB(0, 255, 0) : RGB(255, 0, 0));
        } else {
            g_FrameAdvantageId = DirectDrawHook::AddPermanentMessage(frameAdvText, 
                frameAdvantage >= 0 ? RGB(0, 255, 0) : RGB(255, 0, 0), 305, 430);
        }
        
        // Set display duration (show for approximately 1 second)
        frameAdvState.displayUntilInternalFrame = currentInternalFrame + 192;
        
        LogOut("[FRAME_ADV] P2->P1 Frame Advantage: " + frameAdvText, true);
        
        // Reset attack state for the next sequence while preserving defender state
        frameAdvState.p2Attacking = false;
        frameAdvState.p2AttackStartInternalFrame = -1;
        frameAdvState.p2ActionableInternalFrame = -1;
    }
    
    // STEP 6: Add timeout detection for stale states
    static int staleFrameCounter = 0;
    bool hasActiveTracking = (frameAdvState.p1Attacking && !frameAdvState.p1AdvantageCalculated) || 
                             (frameAdvState.p2Attacking && !frameAdvState.p2AdvantageCalculated);
                             
    if (hasActiveTracking) {
        staleFrameCounter++;
        // If we've been tracking a state for more than 3 seconds without resolving it,
        // something probably went wrong - reset to avoid getting stuck
        if (staleFrameCounter > 576) {  // ~3 seconds
            LogOut("[FRAME_ADV] Stale state detected, resetting", true);
            ResetFrameAdvantageState();
            staleFrameCounter = 0;
            // But keep these for gap detection
            p1_last_defender_free_frame = -1;
            p2_last_defender_free_frame = -1;
        }
    } else {
        staleFrameCounter = 0;
    }
}

bool IsFrameAdvantageActive() {
    return frameAdvState.p1Attacking || frameAdvState.p2Attacking ||
           frameAdvState.p1Defending || frameAdvState.p2Defending;
}

FrameAdvantageState GetFrameAdvantageState() {
    return frameAdvState;
}