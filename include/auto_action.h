#pragma once
#include <windows.h>
#include <atomic> // Add this include for std::atomic
#include "../include/constants.h"

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

// NEW: Globals to track the most recently activated trigger for overlay feedback
extern std::atomic<int> g_lastActiveTriggerType;
extern std::atomic<int> g_lastActiveTriggerFrame;

// Function declarations
short GetActionMoveID(int actionType);
void ProcessTriggerDelays();
void StartTriggerDelay(int playerNum, int triggerType, short moveID, int delayFrames);
void MonitorAutoActions();
void ResetActionFlags();
void ClearDelayStatesIfNonActionable();