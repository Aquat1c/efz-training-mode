#include "../include/frame_advantage.h"
#include "../include/constants.h"
#include "../include/utilities.h"
#include "../include/memory.h"
#include "../include/logger.h"
#include "../include/frame_analysis.h"
#include "../include/frame_monitor.h"
#include "../include/overlay.h"

// Updated structure initialization with internal frames
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
    0.0, 0.0,                      // p1FrameAdvantage, p2FrameAdvantage (now double)
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
    
    // Add debug message to trace resets
    if (detailedLogging.load()) {
        LogOut("[FRAME_ADV_DEBUG] Frame advantage state reset", false);
    }

    // Remove the frame advantage overlay message
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
        case 1: subframeDecimal = 0.33; break;  // Second subframe (.33) - EXACTLY 0.33
        case 2: subframeDecimal = 0.66; break;  // Third subframe (.66) - EXACTLY 0.66
    }
    
    return visualFrameWhole + subframeDecimal;
}

std::string FormatFrameAdvantage(int advantageInternal) {
    int visualFrames = advantageInternal / 3;
    int subframes = std::abs(advantageInternal % 3);

    std::string sign = (advantageInternal >= 0) ? "+" : "";
    std::string subframeStr = "";
    if (subframes == 1) {
        subframeStr = ".33";
    } else if (subframes == 2) {
        subframeStr = ".66";
    }
    
    if (visualFrames == 0 && advantageInternal < 0) {
        return "-" + std::to_string(visualFrames) + subframeStr;
    }

    return sign + std::to_string(visualFrames) + subframeStr;
}

bool IsAttackMove(short moveID) {
    // More precise attack detection
    return (moveID >= 200 && moveID <= 350) ||           
           (moveID >= 400 && moveID <= 500);
}

void MonitorFrameAdvantage(short moveID1, short moveID2, short prevMoveID1, short prevMoveID2) {
    int currentInternalFrame = GetCurrentInternalFrame();
    double currentVisualFrame = GetCurrentVisualFrame();
    
    static int p1_last_defender_free_frame = -1;
    static int p2_last_defender_free_frame = -1;
    static int p1_hit_connect_cooldown = 0;
    static int p2_hit_connect_cooldown = 0;

    if (p1_hit_connect_cooldown > 0) p1_hit_connect_cooldown--;
    if (p2_hit_connect_cooldown > 0) p2_hit_connect_cooldown--;

    // Check if the display timer has expired
    if (frameAdvState.displayUntilInternalFrame != -1 && currentInternalFrame >= frameAdvState.displayUntilInternalFrame) {
        ResetFrameAdvantageState();
    }
    
    // Add debug logging to track if the function is being called
    static int debugCounter = 0;
    if (detailedLogging.load() && ++debugCounter % 60 == 0) {
        LogOut("[FRAME_ADV_DEBUG] Monitoring active: p1Attacking=" + 
               std::to_string(frameAdvState.p1Attacking) + 
               ", p2Attacking=" + std::to_string(frameAdvState.p2Attacking), false);
    }
    
    // IMPROVED RESET: Reset if both players are in neutral for several frames 
    static int neutralFrameCount = 0;
    if (IsActionable(moveID1) && IsActionable(moveID2) && 
        !IsAttackMove(moveID1) && !IsAttackMove(moveID2)) {
        neutralFrameCount++;
        if (neutralFrameCount > 10) { // Reset after a short period of neutral
             if (frameAdvState.p1Attacking || frameAdvState.p2Attacking) {
                ResetFrameAdvantageState();
             }
             p1_last_defender_free_frame = -1;
             p2_last_defender_free_frame = -1;
        }
    } else {
        neutralFrameCount = 0;
    }
    
    // STEP 1: Detect if an attack connects (P1 attacking P2)
    if (IsBlockstunState(moveID2) && p1_hit_connect_cooldown == 0) {
        int gap = -1;
        if (p2_last_defender_free_frame != -1) {
            gap = currentInternalFrame - p2_last_defender_free_frame;
        }

        ResetFrameAdvantageState();

        if (gap != -1 && (gap / 3) < 20) {
            std::string gapText = "Gap: " + std::to_string(gap / 3) + "f";
            if (g_FrameAdvantageId != -1) DirectDrawHook::RemovePermanentMessage(g_FrameAdvantageId);
            g_FrameAdvantageId = DirectDrawHook::AddPermanentMessage(gapText, RGB(255, 255, 0), 305, 460);
            frameAdvState.displayUntilInternalFrame = currentInternalFrame + 128;
        } else {
            frameAdvState.p1Attacking = true;
            frameAdvState.p2Defending = true;
            frameAdvState.p1AttackStartInternalFrame = currentInternalFrame;
            frameAdvState.p1AttackMoveID = IsAttackMove(moveID1) ? moveID1 : prevMoveID1;
            frameAdvState.p2InBlockstun = IsBlockstunState(moveID2);
            frameAdvState.p2InHitstun = IsHitstun(moveID2);
            frameAdvState.p2BlockstunStartInternalFrame = currentInternalFrame;
            frameAdvState.p2InitialBlockstunMoveID = moveID2;
            LogOut("[FRAME ADVANTAGE] P1 attack connected at internal frame " + std::to_string(currentInternalFrame) + " (visual: " + std::to_string(currentVisualFrame) + ") - P2 in blockstun (moveID: " + std::to_string(moveID2) + ")", detailedLogging.load());
        }
        p2_last_defender_free_frame = -1;
        p1_hit_connect_cooldown = 15; // Cooldown for ~5 visual frames
    }
    
    // STEP 2: Detect if an attack connects (P2 attacking P1)
    if (IsBlockstunState(moveID1) && p2_hit_connect_cooldown == 0) {
        int gap = -1;
        if (p1_last_defender_free_frame != -1) {
            gap = currentInternalFrame - p1_last_defender_free_frame;
        }
        
        ResetFrameAdvantageState();

        if (gap != -1 && (gap / 3) < 20) {
            std::string gapText = "Gap: " + std::to_string(gap / 3) + "f";
            if (g_FrameAdvantageId != -1) DirectDrawHook::RemovePermanentMessage(g_FrameAdvantageId);
            g_FrameAdvantageId = DirectDrawHook::AddPermanentMessage(gapText, RGB(255, 255, 0), 305, 460);
            frameAdvState.displayUntilInternalFrame = currentInternalFrame + 128;
        } else {
            frameAdvState.p2Attacking = true;
            frameAdvState.p1Defending = true;
            frameAdvState.p2AttackStartInternalFrame = currentInternalFrame;
            frameAdvState.p2AttackMoveID = IsAttackMove(moveID2) ? moveID2 : prevMoveID2;
            frameAdvState.p1InBlockstun = IsBlockstunState(moveID1);
            frameAdvState.p1InHitstun = IsHitstun(moveID1);
            frameAdvState.p1BlockstunStartInternalFrame = currentInternalFrame;
            frameAdvState.p1InitialBlockstunMoveID = moveID1;
            LogOut("[FRAME ADVANTAGE] P2 attack connected at internal frame " + std::to_string(currentInternalFrame) + " (visual: " + std::to_string(currentVisualFrame) + ") - P1 in blockstun (moveID: " + std::to_string(moveID1) + ")", detailedLogging.load());
        }
        p1_last_defender_free_frame = -1;
        p2_hit_connect_cooldown = 15; // Cooldown for ~5 visual frames
    }
    
    // STEP 2: Detect when attacker exits recovery - SIMPLIFIED
    if (frameAdvState.p1Attacking && frameAdvState.p1ActionableInternalFrame == -1) {
        if (!IsActionable(prevMoveID1) && IsActionable(moveID1)) {
            frameAdvState.p1ActionableInternalFrame = currentInternalFrame;
            double actionableVisualFrame = currentVisualFrame;
            
            // Only show attacker actionable logs in detailed mode
            LogOut("[FRAME ADVANTAGE] P1 (attacker) became actionable at internal frame " + 
                   std::to_string(currentInternalFrame) + " (visual: " + std::to_string(actionableVisualFrame) + 
                   ") | moveID: " + std::to_string(prevMoveID1) + " -> " + std::to_string(moveID1), detailedLogging.load());
        }
    }
    
    if (frameAdvState.p2Attacking && frameAdvState.p2ActionableInternalFrame == -1) {
        if (!IsActionable(prevMoveID2) && IsActionable(moveID2)) {
            frameAdvState.p2ActionableInternalFrame = currentInternalFrame;
            double actionableVisualFrame = currentVisualFrame;
            
            LogOut("[FRAME ADVANTAGE] P2 (attacker) became actionable at internal frame " + 
                   std::to_string(currentInternalFrame) + " (visual: " + std::to_string(actionableVisualFrame) + 
                   ") | moveID: " + std::to_string(prevMoveID2) + " -> " + std::to_string(moveID2), detailedLogging.load());
        }
    }
    
    // STEP 3: Detect when defender exits blockstun/hitstun - SIMPLIFIED
    if (frameAdvState.p2Defending && frameAdvState.p2DefenderFreeInternalFrame == -1) {
        bool wasInStun = IsBlockstunState(prevMoveID2) || IsHitstun(prevMoveID2);
        if (wasInStun && IsActionable(moveID2)) {
            frameAdvState.p2DefenderFreeInternalFrame = currentInternalFrame;
            p2_last_defender_free_frame = currentInternalFrame;
            double defenderFreeVisualFrame = currentVisualFrame;
            
            LogOut("[FRAME ADVANTAGE] P2 (defender) exited stun at internal frame " + 
                   std::to_string(currentInternalFrame) + " (visual: " + std::to_string(defenderFreeVisualFrame) + 
                   ") | moveID: " + std::to_string(prevMoveID2) + " -> " + std::to_string(moveID2), detailedLogging.load());
        }
    }
    
    if (frameAdvState.p1Defending && frameAdvState.p1DefenderFreeInternalFrame == -1) {
        bool wasInStun = IsBlockstunState(prevMoveID1) || IsHitstun(prevMoveID1);
        if (wasInStun && IsActionable(moveID1)) {
            frameAdvState.p1DefenderFreeInternalFrame = currentInternalFrame;
            p1_last_defender_free_frame = currentInternalFrame;
            double defenderFreeVisualFrame = currentVisualFrame;
            
            LogOut("[FRAME ADVANTAGE] P1 (defender) exited stun at internal frame " + 
                   std::to_string(currentInternalFrame) + " (visual: " + std::to_string(defenderFreeVisualFrame) + 
                   ") | moveID: " + std::to_string(prevMoveID1) + " -> " + std::to_string(moveID1), detailedLogging.load());
        }
    }
    
    // STEP 4: Calculate frame advantage with PRECISE subframe calculation
    if (frameAdvState.p1Attacking && !frameAdvState.p1AdvantageCalculated &&
        frameAdvState.p1ActionableInternalFrame != -1 && frameAdvState.p2DefenderFreeInternalFrame != -1) {
        // Calculate advantage in internal frames, then convert to visual frames
        int advantageInternal = frameAdvState.p2DefenderFreeInternalFrame - frameAdvState.p1ActionableInternalFrame;
        frameAdvState.p1FrameAdvantage = static_cast<double>(advantageInternal) / SUBFRAMES_PER_VISUAL_FRAME;
        frameAdvState.p1AdvantageCalculated = true;
        frameAdvState.displayUntilInternalFrame = currentInternalFrame + 192; // Display for ~1 second

        std::string advantageText = FormatFrameAdvantage(advantageInternal);
        
        COLORREF advantageColor = (advantageInternal >= 0) ? RGB(100, 255, 100) : RGB(255, 100, 100);
        
        // Remove the old message before adding a new one to ensure it's replaced at the correct position
        if (g_FrameAdvantageId != -1) {
            DirectDrawHook::RemovePermanentMessage(g_FrameAdvantageId);
        }
        g_FrameAdvantageId = DirectDrawHook::AddPermanentMessage(advantageText, advantageColor, 305, 430);
    }
    
    if (frameAdvState.p2Attacking && !frameAdvState.p2AdvantageCalculated &&
        frameAdvState.p2ActionableInternalFrame != -1 && frameAdvState.p1DefenderFreeInternalFrame != -1) {
        int advantageInternal = frameAdvState.p1DefenderFreeInternalFrame - frameAdvState.p2ActionableInternalFrame;
        frameAdvState.p2FrameAdvantage = static_cast<double>(advantageInternal) / SUBFRAMES_PER_VISUAL_FRAME;
        frameAdvState.p2AdvantageCalculated = true;
        frameAdvState.displayUntilInternalFrame = currentInternalFrame + 192; // Display for ~1 second

        std::string advantageText = FormatFrameAdvantage(advantageInternal);

        COLORREF advantageColor = (advantageInternal >= 0) ? RGB(100, 255, 100) : RGB(255, 100, 100);

        // Remove the old message before adding a new one to ensure it's replaced at the correct position
        if (g_FrameAdvantageId != -1) {
            DirectDrawHook::RemovePermanentMessage(g_FrameAdvantageId);
        }
        g_FrameAdvantageId = DirectDrawHook::AddPermanentMessage(advantageText, advantageColor, 305, 430);
    }
    
    // STEP 5: Add improved timeout detection
    static int staleFrameCounter = 0;
    if (frameAdvState.p1Attacking || frameAdvState.p2Attacking) {
        if (++staleFrameCounter > 180) { // 3 seconds with no resolution
            LogOut("[FRAME_ADV_DEBUG] Resetting stale frame advantage tracking", detailedLogging.load());
            ResetFrameAdvantageState();
            staleFrameCounter = 0;
        }
    } else {
        staleFrameCounter = 0;
    }
}

bool IsFrameAdvantageActive() {
    return frameAdvState.p1Attacking || frameAdvState.p2Attacking;
}

FrameAdvantageState GetFrameAdvantageState() {
    return frameAdvState;
}