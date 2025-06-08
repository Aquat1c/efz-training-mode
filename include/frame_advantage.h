#pragma once
#include <windows.h>
#include <string>

// Structure to track frame advantage state with subframe precision
struct FrameAdvantageState {
    // Blockstun/Hitstun tracking
    bool p1InBlockstun;
    bool p2InBlockstun;
    bool p1InHitstun;
    bool p2InHitstun;
    
    // Attack tracking
    bool p1Attacking;
    bool p2Attacking;
    int p1AttackStartInternalFrame;     // Changed to internal frames for precision
    int p2AttackStartInternalFrame;
    short p1AttackMoveID;
    short p2AttackMoveID;
    
    // Frame tracking (in internal frames at 192 FPS for precision)
    int p1BlockstunStartInternalFrame;
    int p2BlockstunStartInternalFrame;
    int p1HitstunStartInternalFrame;
    int p2HitstunStartInternalFrame;
    
    // Recovery tracking (in internal frames)
    int p1ActionableInternalFrame;
    int p2ActionableInternalFrame;
    int p1DefenderFreeInternalFrame;    // When P1 exits blockstun/hitstun
    int p2DefenderFreeInternalFrame;    // When P2 exits blockstun/hitstun
    
    // Frame advantage results (stored as subframes for precision)
    double p1FrameAdvantage;            // Changed to double for subframe precision
    double p2FrameAdvantage;
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

// Helper functions with subframe precision
int GetCurrentInternalFrame();
double GetCurrentVisualFrame();      // Returns frame with .33/.66 subframes
std::string FormatFrameAdvantage(double advantage);  // Format with subframes
bool IsAttackMove(short moveID);
bool IsRecoveryFromAttack(short currentMoveID, short prevMoveID);