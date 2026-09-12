#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include <cstdint>
#include <cstddef>
#include "input_core.h"

// Input frame structure
struct InputFrame {
    uint8_t inputMask;
    int durationFrames;
    
    InputFrame(uint8_t mask, int duration) : inputMask(mask), durationFrames(duration) {}
};

// Motion queueing functions
bool QueueMotionInput(int playerNum, int motionType, int buttonMask = 0);

// Exclusive queue lease used by tutorial dummy episodes.  It prevents an
// auto-action from replacing the dummy's in-flight queue and lets teardown
// clear only the queue owned by the matching token.
bool AcquireTutorialMotionQueue(int playerNum, uint64_t& tokenOut);
bool TutorialMotionQueueLeaseActive(int playerNum);
bool QueueTutorialMotionInput(int playerNum, uint64_t token, int motionType,
                              int buttonMask = 0);
void ReleaseTutorialMotionQueue(int playerNum, uint64_t token);
uint8_t DetermineButtonFromMotionType(int motionType);
std::string GetMotionTypeName(int motionType);
void ProcessInputQueues();
int ConvertActionToMotion(int actionType, int triggerType);
// Coherent read-only view of the legacy frame queue.  Callers must not sample
// the exported vector/index globals independently: the frame monitor and input
// hook can otherwise observe different generations of the queue.
struct MotionQueueSnapshot {
    bool active{false};
    int index{0};
    int frameCounter{0};
    int motionType{0};
    std::size_t size{0};
    bool hasCurrentMask{false};
    uint8_t currentMask{0};
};
MotionQueueSnapshot GetMotionQueueSnapshot(int playerNum);
// includeTutorialOwned=false preserves a live tutorial lease.  The netplay
// ownership boundary passes true after synchronizing with that lease's P2
// controller barrier, retiring only local queue bookkeeping.
bool ClearMotionInputQueue(int playerNum, bool includeTutorialOwned = false);
// Motion input globals
extern std::vector<InputFrame> p1InputQueue;
extern std::vector<InputFrame> p2InputQueue;
extern int p1QueueIndex;
extern int p2QueueIndex;
extern int p1FrameCounter;
extern int p2FrameCounter;
extern bool p1QueueActive;
extern bool p2QueueActive;

// Track the last motion type queued for each player (for diagnostics / conditional logic)
extern int p1CurrentMotionType;
extern int p2CurrentMotionType;
