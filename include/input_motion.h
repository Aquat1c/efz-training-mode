#pragma once
#include <windows.h>
#include <vector>
#include <cstdint>
#include <string>
#include <atomic>

// Motion input constants
#define MOTION_NONE       0
#define MOTION_5A         1   // Standing A
#define MOTION_5B         2   // Standing B
#define MOTION_5C         3   // Standing C
#define MOTION_2A         4   // Crouching A
#define MOTION_2B         5   // Crouching B
#define MOTION_2C         6   // Crouching C
#define MOTION_JA         31  // Jumping A
#define MOTION_JB         32  // Jumping B
#define MOTION_JC         33  // Jumping C
#define MOTION_236A       11  // QCF + A
#define MOTION_236B       12  // QCF + B
#define MOTION_236C       13  // QCF + C
#define MOTION_623A       14  // DP + A
#define MOTION_623B       15  // DP + B
#define MOTION_623C       16  // DP + C
#define MOTION_214A       17  // QCB + A
#define MOTION_214B       18  // QCB + B
#define MOTION_214C       19  // QCB + C
#define MOTION_421A       20  // Half Circle Back Down + A
#define MOTION_421B       21  // Half Circle Back Down + B
#define MOTION_421C       22  // Half Circle Back Down + C
#define MOTION_41236A     23  // Half Circle Forward + A
#define MOTION_41236B     24  // Half Circle Forward + B
#define MOTION_41236C     25  // Half Circle Forward + C
#define MOTION_63214A     26  // Half Circle Back + A
#define MOTION_63214B     27  // Half Circle Back + B
#define MOTION_63214C     28  // Half Circle Back + C

// New constants for dashes
#define ACTION_FORWARD_DASH 101
#define ACTION_BACK_DASH    102

// Input direction/button constants
#define MOTION_INPUT_RIGHT  0x01
#define MOTION_INPUT_LEFT   0x02
#define MOTION_INPUT_DOWN   0x04
#define MOTION_INPUT_UP     0x08
#define MOTION_BUTTON_A     0x10
#define MOTION_BUTTON_B     0x20
#define MOTION_BUTTON_C     0x40
#define MOTION_BUTTON_D     0x80

// Structure to represent a single frame of input
struct InputFrame {
    uint8_t inputMask;
    int durationFrames;
    
    InputFrame(uint8_t mask, int duration) : inputMask(mask), durationFrames(duration) {}
};

// Global variables for the input queue system
extern std::vector<InputFrame> p1InputQueue;
extern std::vector<InputFrame> p2InputQueue;
extern int p1QueueIndex;
extern int p2QueueIndex;
extern int p1FrameCounter;
extern int p2FrameCounter;
extern bool p1QueueActive;
extern bool p2QueueActive;

// --- Core API ---

// Queues a sequence of inputs to perform a motion (e.g., a special move).
bool QueueMotionInput(int playerNum, int motionType, int buttonMask);

// Processes the active input queues each frame. Called by the frame monitor.
void ProcessInputQueues();
uintptr_t GetPlayerPointer(int playerNum);
// Writes a single frame of input. Used by the input hook and for simple actions.
bool WritePlayerInput(int playerNum, uint8_t inputMask);
bool WritePlayerInputImmediate(int playerNum, uint8_t inputMask);
bool WritePlayerInputToBuffer(int playerNum, uint8_t inputMask);

// --- Manual Override API (for debug tab / direct control) ---

// Forces a player's inputs to a specific state until released.
bool HoldWalkForward(int playerNum);
bool HoldWalkBackward(int playerNum);
bool HoldCrouch(int playerNum);
bool HoldUp(int playerNum);
bool HoldBackCrouch(int playerNum);
bool HoldButtonA(int playerNum);
bool HoldButtonB(int playerNum);
bool HoldButtonC(int playerNum);
bool HoldButtonD(int playerNum);
bool ReleaseInputs(int playerNum);
void DiagnoseInputSystem(int playerNum);
int ConvertActionToMotion(int actionType, int triggerType);
uint8_t DetermineButtonFromMotionType(int motionType);
std::string DecodeInputMask(uint8_t inputMask);