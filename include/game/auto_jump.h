#pragma once
#include <windows.h>

// Forward declare the auto-action structures and variables
struct TriggerDelayState;
extern TriggerDelayState p1DelayState;
extern TriggerDelayState p2DelayState;
extern bool p1ActionApplied;
extern bool p2ActionApplied;

// Function declarations
void ApplyJump(uintptr_t moveIDAddr, int playerNum, int jumpType);
void MonitorAutoJump();
bool IsAutoActionActiveForPlayer(int playerNum); 
void AutoJumpReleaseForPlayer(int playerNum);     // Release any auto-jump-held input for a player