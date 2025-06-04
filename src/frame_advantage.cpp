#include "../include/frame_advantage.h"
#include "../include/constants.h"
#include "../include/utilities.h"
#include "../include/memory.h"
#include "../include/logger.h"
#include "../include/frame_analysis.h"
#include "../include/frame_monitor.h"

// Use the proper FrameAdvantageState structure from the header
FrameAdvantageState frameAdvState = {
    false, false, false, false,    // p1InBlockstun, p2InBlockstun, p1InHitstun, p2InHitstun
    false, false,                  // p1Attacking, p2Attacking
    -1, -1,                        // p1AttackStartVisualFrame, p2AttackStartVisualFrame
    0, 0,                          // p1AttackMoveID, p2AttackMoveID
    -1, -1,                        // p1BlockstunStartVisualFrame, p2BlockstunStartVisualFrame
    -1, -1,                        // p1HitstunStartVisualFrame, p2HitstunStartVisualFrame
    -1, -1,                        // p1ActionableVisualFrame, p2ActionableVisualFrame
    -1, -1,                        // p1DefenderFreeVisualFrame, p2DefenderFreeVisualFrame
    0, 0,                          // p1FrameAdvantage, p2FrameAdvantage
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
    frameAdvState.p1AttackStartVisualFrame = -1;
    frameAdvState.p2AttackStartVisualFrame = -1;
    frameAdvState.p1AttackMoveID = 0;
    frameAdvState.p2AttackMoveID = 0;
    frameAdvState.p1BlockstunStartVisualFrame = -1;
    frameAdvState.p2BlockstunStartVisualFrame = -1;
    frameAdvState.p1HitstunStartVisualFrame = -1;
    frameAdvState.p2HitstunStartVisualFrame = -1;
    frameAdvState.p1ActionableVisualFrame = -1;
    frameAdvState.p2ActionableVisualFrame = -1;
    frameAdvState.p1DefenderFreeVisualFrame = -1;
    frameAdvState.p2DefenderFreeVisualFrame = -1;
    frameAdvState.p1FrameAdvantage = 0;
    frameAdvState.p2FrameAdvantage = 0;
    frameAdvState.p1AdvantageCalculated = false;
    frameAdvState.p2AdvantageCalculated = false;
    frameAdvState.p1InitialBlockstunMoveID = 0;
    frameAdvState.p2InitialBlockstunMoveID = 0;
}

int GetCurrentVisualFrame() {
    // Convert our internal frames to visual frames
    return frameCounter.load() / 3;
}

bool IsAttackMove(short moveID) {
    // More precise attack detection
    return (moveID >= 200 && moveID <= 350) ||           
           (moveID >= 400 && moveID <= 500);
}

void MonitorFrameAdvantage(short moveID1, short moveID2, short prevMoveID1, short prevMoveID2) {
    int currentVisualFrame = GetCurrentVisualFrame();
    
    // Reset if both players are idle to prevent stale data
    if (IsActionable(moveID1) && IsActionable(moveID2) && 
        !frameAdvState.p1Attacking && !frameAdvState.p2Attacking) {
        ResetFrameAdvantageState();
        return;
    }
    
    // STEP 1: Detect attack start
    if (IsAttackMove(moveID1) && !IsAttackMove(prevMoveID1) && !frameAdvState.p1Attacking) {
        frameAdvState.p1Attacking = true;
        frameAdvState.p1AttackStartVisualFrame = currentVisualFrame;
        frameAdvState.p1AttackMoveID = moveID1;
        frameAdvState.p1ActionableVisualFrame = -1;
        frameAdvState.p2DefenderFreeVisualFrame = -1;
        frameAdvState.p1AdvantageCalculated = false;
        
        LogOut("[FRAME ADVANTAGE] P1 attack started at frame " + std::to_string(currentVisualFrame) + 
               " (moveID: " + std::to_string(moveID1) + ")", true);
    }
    
    if (IsAttackMove(moveID2) && !IsAttackMove(prevMoveID2) && !frameAdvState.p2Attacking) {
        frameAdvState.p2Attacking = true;
        frameAdvState.p2AttackStartVisualFrame = currentVisualFrame;
        frameAdvState.p2AttackMoveID = moveID2;
        frameAdvState.p2ActionableVisualFrame = -1;
        frameAdvState.p1DefenderFreeVisualFrame = -1;
        frameAdvState.p2AdvantageCalculated = false;
        
        LogOut("[FRAME ADVANTAGE] P2 attack started at frame " + std::to_string(currentVisualFrame) + 
               " (moveID: " + std::to_string(moveID2) + ")", true);
    }
    
    // STEP 2: Detect when attacker becomes actionable
    if (frameAdvState.p1Attacking && frameAdvState.p1ActionableVisualFrame == -1) {
        if (IsActionable(moveID1) && !IsAttackMove(moveID1)) {
            frameAdvState.p1ActionableVisualFrame = currentVisualFrame;
            LogOut("[FRAME ADVANTAGE] P1 became actionable at frame " + std::to_string(currentVisualFrame), true);
        }
    }
    
    if (frameAdvState.p2Attacking && frameAdvState.p2ActionableVisualFrame == -1) {
        if (IsActionable(moveID2) && !IsAttackMove(moveID2)) {
            frameAdvState.p2ActionableVisualFrame = currentVisualFrame;
            LogOut("[FRAME ADVANTAGE] P2 became actionable at frame " + std::to_string(currentVisualFrame), true);
        }
    }
    
    // STEP 3: Detect when defender becomes free
    if (frameAdvState.p1Attacking && frameAdvState.p2DefenderFreeVisualFrame == -1) {
        if ((IsBlockstun(prevMoveID2) || IsHitstun(prevMoveID2)) && 
            IsActionable(moveID2) && !IsBlockstun(moveID2) && !IsHitstun(moveID2)) {
            frameAdvState.p2DefenderFreeVisualFrame = currentVisualFrame;
            LogOut("[FRAME ADVANTAGE] P2 (defender) became free at frame " + std::to_string(currentVisualFrame), true);
        }
    }
    
    if (frameAdvState.p2Attacking && frameAdvState.p1DefenderFreeVisualFrame == -1) {
        if ((IsBlockstun(prevMoveID1) || IsHitstun(prevMoveID1)) && 
            IsActionable(moveID1) && !IsBlockstun(moveID1) && !IsHitstun(moveID1)) {
            frameAdvState.p1DefenderFreeVisualFrame = currentVisualFrame;
            LogOut("[FRAME ADVANTAGE] P1 (defender) became free at frame " + std::to_string(currentVisualFrame), true);
        }
    }
    
    // STEP 4: Calculate frame advantage when we have both pieces
    if (frameAdvState.p1Attacking && !frameAdvState.p1AdvantageCalculated &&
        frameAdvState.p1ActionableVisualFrame != -1 && frameAdvState.p2DefenderFreeVisualFrame != -1) {
        
        int advantage = frameAdvState.p2DefenderFreeVisualFrame - frameAdvState.p1ActionableVisualFrame;
        frameAdvState.p1FrameAdvantage = advantage;
        frameAdvState.p1AdvantageCalculated = true;
        
        LogOut("[FRAME ADVANTAGE] P1 has " + std::to_string(advantage) + " frame advantage" + 
               " (P1 actionable: " + std::to_string(frameAdvState.p1ActionableVisualFrame) + 
               ", P2 free: " + std::to_string(frameAdvState.p2DefenderFreeVisualFrame) + ")", true);
        
        frameAdvState.p1Attacking = false;
    }
    
    if (frameAdvState.p2Attacking && !frameAdvState.p2AdvantageCalculated &&
        frameAdvState.p2ActionableVisualFrame != -1 && frameAdvState.p1DefenderFreeVisualFrame != -1) {
        
        int advantage = frameAdvState.p1DefenderFreeVisualFrame - frameAdvState.p2ActionableVisualFrame;
        frameAdvState.p2FrameAdvantage = advantage;
        frameAdvState.p2AdvantageCalculated = true;
        
        LogOut("[FRAME ADVANTAGE] P2 has " + std::to_string(advantage) + " frame advantage" + 
               " (P2 actionable: " + std::to_string(frameAdvState.p2ActionableVisualFrame) + 
               ", P1 free: " + std::to_string(frameAdvState.p1DefenderFreeVisualFrame) + ")", true);
        
        frameAdvState.p2Attacking = false;
    }
    
    // STEP 5: Auto-reset if tracking goes stale
    static int idleFrameCount = 0;
    if (IsActionable(moveID1) && IsActionable(moveID2) && 
        (frameAdvState.p1Attacking || frameAdvState.p2Attacking)) {
        idleFrameCount++;
        if (idleFrameCount > 120) { // 2 seconds at 64 FPS
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