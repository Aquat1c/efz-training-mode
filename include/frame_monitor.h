#pragma once
#include <atomic>
#include <string>  // Add this include
#include "../include/constants.h"

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
short GetActionMoveID(int actionType, int triggerType = TRIGGER_NONE, int playerNum = 2);
void ApplyAutoAction(int playerNum, uintptr_t moveIDAddr, short currentMoveID, short prevMoveID);
extern bool p1ActionApplied;
extern bool p2ActionApplied;

// Update function declarations
void ProcessTriggerDelays();

// Add these function declarations:
void MonitorAutoActions();
void ResetActionFlags();
void ClearDelayStatesIfNonActionable();