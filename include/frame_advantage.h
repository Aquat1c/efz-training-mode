#pragma once
#include <windows.h>

// Structure to track frame advantage state
struct FrameAdvantageState {
    // Blockstun/Hitstun tracking
    bool p1InBlockstun;
    bool p2InBlockstun;
    bool p1InHitstun;
    bool p2InHitstun;
    
    // Attack tracking - NEW: Track who attacked whom
    bool p1Attacking;
    bool p2Attacking;
    int p1AttackStartVisualFrame;
    int p2AttackStartVisualFrame;
    short p1AttackMoveID;
    short p2AttackMoveID;
    
    // Frame tracking (in visual frames at 64 FPS)
    int p1BlockstunStartVisualFrame;
    int p2BlockstunStartVisualFrame;
    int p1HitstunStartVisualFrame;
    int p2HitstunStartVisualFrame;
    
    // Recovery tracking - NEW: Track when attacker becomes actionable
    int p1ActionableVisualFrame;
    int p2ActionableVisualFrame;
    int p1DefenderFreeVisualFrame;  // When P1 exits blockstun/hitstun
    int p2DefenderFreeVisualFrame;  // When P2 exits blockstun/hitstun
    
    // Frame advantage results
    int p1FrameAdvantage;  // Positive = advantage, negative = disadvantage
    int p2FrameAdvantage;
    bool p1AdvantageCalculated;
    bool p2AdvantageCalculated;
    
    // Initial moveIDs for analysis
    short p1InitialBlockstunMoveID;
    short p2InitialBlockstunMoveID;
};

// Global frame advantage state
extern FrameAdvantageState frameAdvState;

// Function declarations
void ResetFrameAdvantageState();
void MonitorFrameAdvantage(short moveID1, short moveID2, short prevMoveID1, short prevMoveID2);
bool IsFrameAdvantageActive();
FrameAdvantageState GetFrameAdvantageState();

// Helper functions
int GetCurrentVisualFrame();
bool IsAttackMove(short moveID);
bool IsRecoveryFromAttack(short currentMoveID, short prevMoveID);