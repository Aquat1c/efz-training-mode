#pragma once
#include <atomic>
#include <string>
#include "../include/core/constants.h"
#include "../include/game/game_state.h"

// Lightweight per-frame snapshot for consumers; produced by FrameDataMonitor
struct FrameSnapshot {
    unsigned long long tickMs;  // GetTickCount64 at publish
    GamePhase phase;
    GameMode mode;
    short p1Move;
    short p2Move;
    short prevP1Move;
    short prevP2Move;
    bool p2BlockEdge;   // transition into block/guard state this frame
    bool p2HitstunEdge; // transition into hitstun this frame
    // Positions
    double p1X;
    double p2X;
    double p1Y;
    double p2Y;
    // Vital stats
    int p1Hp;
    int p2Hp;
    int p1Meter;
    int p2Meter;
    double p1RF;
    double p2RF;
    // Character identity
    int p1CharId;
    int p2CharId;
};

// Read-mostly accessor; returns false if snapshot is missing or stale beyond maxAgeMs
bool TryGetLatestSnapshot(FrameSnapshot &out, unsigned int maxAgeMs);


// Update the monitor state enum
enum MonitorState {
    Idle,
    Monitoring,
    RGMonitoring,
    SuperflashMonitoring
};

extern MonitorState state;

// Auto-airtech related declarations
extern bool p1InAirHitstun;
extern bool p2InAirHitstun;
extern int p1LastHitstunFrame;
extern int p2LastHitstunFrame;

// Forward declare the struct from auto_action.h instead of redefining it
struct TriggerDelayState;
extern TriggerDelayState p1DelayState;
extern TriggerDelayState p2DelayState;

// Add function declarations for the missing functions
void ApplyAirtechPatches();
void RemoveAirtechPatches();
void ApplyAirtech(uintptr_t moveIDAddr, int playerNum, int frameNum);
void MonitorAutoAirtech(short moveID1, short moveID2);
void MonitorAutoJump();

void FrameDataMonitor();

// Blockstun and attack level detection
extern short initialBlockstunMoveID;
bool IsBlockstunState(short moveID);
int GetAttackLevel(short blockstunMoveID);
bool IsDashState(short moveID);

// Auto-action related functions
extern short GetActionMoveID(int actionType, int triggerType, int playerNum);
void ApplyAutoAction(int playerNum, uintptr_t moveIDAddr, short currentMoveID, short prevMoveID);
extern bool p1ActionApplied;
extern bool p2ActionApplied;

// Update function declarations
void ProcessTriggerDelays();

// Add these function declarations:
void MonitorAutoActions();
void ResetActionFlags();
void ClearDelayStatesIfNonActionable();
void UpdateTriggerOverlay();
bool CheckAndHandleInvalidGameState(GameMode currentMode); // Changed signature
void ReinitializeOverlays();

// Add with other function declarations
bool AreCharactersInitialized();
bool IsValidGameMode(GameMode mode);
void UpdateStatsDisplay();