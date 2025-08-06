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
void ResetActionFlags();
void ClearDelayStatesIfNonActionable();

// Add this function declaration for the special move logic
void ApplyAutoAction(int playerNum, uintptr_t moveIDAddr, short currentMoveID, short prevMoveID);

// Helper functions for motion selection
int GetSpecialMoveStrength(int actionType, int triggerType);
std::string GetTriggerName(int triggerType);