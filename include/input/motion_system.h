#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include <cstdint>
#include "input_core.h"

// Input frame structure
struct InputFrame {
    uint8_t inputMask;
    int durationFrames;
    
    InputFrame(uint8_t mask, int duration) : inputMask(mask), durationFrames(duration) {}
};

// Motion queueing functions
bool QueueMotionInput(int playerNum, int motionType, int buttonMask = 0);
uint8_t DetermineButtonFromMotionType(int motionType);
std::string GetMotionTypeName(int motionType);
void ProcessInputQueues();
int ConvertActionToMotion(int actionType, int triggerType);
inline uint8_t u8(int value);
// Motion input globals
extern std::vector<InputFrame> p1InputQueue;
extern std::vector<InputFrame> p2InputQueue;
extern int p1QueueIndex;
extern int p2QueueIndex;
extern int p1FrameCounter;
extern int p2FrameCounter;
extern bool p1QueueActive;
extern bool p2QueueActive;