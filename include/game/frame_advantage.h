#pragma once
#include <windows.h>
#include <string>
#include <atomic>

//#define ENABLE_FRAME_ADV_DEBUG

// Structure to track frame advantage state with subframe precision
struct FrameAdvantageState {
    // Blockstun/Hitstun tracking
    bool p1InBlockstun;
    bool p2InBlockstun;
    bool p1InHitstun;
    bool p2InHitstun;
    bool p1Defending;
    bool p2Defending;
    
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
    
    // Gap tracking (frames between consecutive hits in a string)
    int p1GapFrames;                    // Gap before P1's current attack
    int p2GapFrames;                    // Gap before P2's current attack
    bool p1GapCalculated;
    bool p2GapCalculated;
    
    // Initial moveIDs for analysis
    short p1InitialBlockstunMoveID;
    short p2InitialBlockstunMoveID;

    // Timer to control how long the advantage is displayed
    int displayUntilInternalFrame;
    // Separate timer for gap display
    int gapDisplayUntilInternalFrame;
};

// Global frame advantage state
extern FrameAdvantageState frameAdvState;
// When set to a future internal frame, regular FA overlay updates are suppressed
extern std::atomic<int> g_SkipRegularFAOverlayUntilFrame;

// Function declarations
void ResetFrameAdvantageState();
void MonitorFrameAdvantage(short moveID1, short moveID2, short prevMoveID1, short prevMoveID2);
bool IsFrameAdvantageActive();
FrameAdvantageState GetFrameAdvantageState();

// Helper functions with subframe precision
int GetCurrentInternalFrame();
int GetDisplayDurationInternalFrames();  // Get configured display duration in internal frames
double GetCurrentVisualFrame();      // Returns frame with .33/.66 subframes
std::string FormatFrameAdvantage(int advantageInternal);  // Changed parameter from double to int
bool IsAttackMove(short moveID);
bool IsRecoveryFromAttack(short currentMoveID, short prevMoveID);
// Lightweight query for frame monitor to decide if FA needs ticking even without moveID changes
bool FrameAdvantageTimersActive();