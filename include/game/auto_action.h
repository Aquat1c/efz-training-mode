#pragma once
#include <windows.h>
#include <atomic>
#include <string>  // Add this for std::string
#include "../include/core/constants.h"

// Delay tracking structure
struct TriggerDelayState {
    bool isDelaying;
    int delayFramesRemaining;
    int triggerType;
    short pendingMoveID;
};

// Global variables
extern TriggerDelayState p1DelayState;
extern TriggerDelayState p2DelayState;
extern bool p1ActionApplied;
extern bool p2ActionApplied;

// Globals to track the most recently activated trigger for overlay feedback
extern std::atomic<int> g_lastActiveTriggerType;
extern std::atomic<int> g_lastActiveTriggerFrame;

// Function declarations
short GetActionMoveID(int actionType, int triggerType = TRIGGER_NONE, int playerNum = 2);
void ProcessTriggerDelays();
void StartTriggerDelay(int playerNum, int triggerType, short moveID, int delayFrames);
void MonitorAutoActions();
// Optimized overload: avoid per-frame memory reads by passing current/prev move IDs
void MonitorAutoActions(short moveID1, short moveID2, short prevMoveID1, short prevMoveID2);
void ResetActionFlags();
void ClearDelayStatesIfNonActionable();

// Add this function declaration for the special move logic
void ApplyAutoAction(int playerNum, uintptr_t moveIDAddr, short currentMoveID, short prevMoveID);

// Helper functions for motion selection
int GetSpecialMoveStrength(int actionType, int triggerType);
std::string GetTriggerName(int triggerType);

// Variables to track P2 control state for auto-actions
extern bool g_p2ControlOverridden;
extern uint32_t g_originalP2ControlFlag;

void RestoreP2ControlState();
void EnableP2ControlForAutoAction();
void ProcessAutoControlRestore();
void ProcessTriggerCooldowns();

// Existing forward declarations...
extern std::atomic<bool> autoActionEnabled;
extern std::atomic<int>  autoActionPlayer;

// ADD these externs for control-restore globals defined in auto_action.cpp
extern std::atomic<bool>  g_pendingControlRestore;
extern std::atomic<int>   g_controlRestoreTimeout;
extern std::atomic<short> g_lastP2MoveID;

// Control restore / cleanup helpers
void ProcessAutoControlRestore();
void ClearAllAutoActionTriggers();

// Tick-integrated execution
// When enabled, auto-actions are evaluated once per internal engine tick from the input hook
// (right before the engine consumes inputs for that tick). The frame monitor will skip
// running its copy to avoid double-processing.
extern std::atomic<bool> g_tickIntegratedAutoActions;

// Lightweight tick entry called from the input hook (once per sub-tick, before P1 processing).
// It executes only the auto-action path using provided move IDs to avoid extra memory reads.
void AutoActionsTick_Inline(short moveID1, short moveID2);
