#pragma once
#include <windows.h>
#include <vector>
#include <cstdint>
#include <string>  // Add this for std::string

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
#define MOTION_421A       20  // Reverse DP + A
#define MOTION_421B       21  // Reverse DP + B
#define MOTION_421C       22  // Reverse DP + C
#define MOTION_41236A     23  // Half Circle Forward + A
#define MOTION_41236B     24  // Half Circle Forward + B
#define MOTION_41236C     25  // Half Circle Forward + C
#define MOTION_63214A     26  // Half Circle Back + A
#define MOTION_63214B     27  // Half Circle Back + B
#define MOTION_63214C     28  // Half Circle Back + C

/*// Update these constants near the top of the file
#define INPUT_RIGHT  0x01
#define INPUT_LEFT   0x02
#define INPUT_DOWN   0x04
#define INPUT_UP     0x08
#define INPUT_A      0x10  // Light attack
#define INPUT_B      0x20  // Medium attack
#define INPUT_C      0x40  // Heavy attack
#define INPUT_D      0x80  // Special*/

// Use bit mask constants directly for buttons
#define BUTTON_A    0x10  // INPUT_A = 0x10 (16 in decimal)
#define BUTTON_B    0x20  // INPUT_B = 0x20 (32 in decimal)
#define BUTTON_C    0x40  // INPUT_C = 0x40 (64 in decimal)
#define BUTTON_D    0x80  // INPUT_D = 0x80 (128 in decimal)

// Structure to represent a single frame of input
struct InputFrame {
    uint8_t inputMask;
    int durationFrames;
    
    InputFrame(uint8_t mask, int duration) : inputMask(mask), durationFrames(duration) {}
};

// Global variables to store motion sequence data
extern std::vector<InputFrame> p1InputQueue;
extern std::vector<InputFrame> p2InputQueue;
extern int p1QueueIndex;
extern int p2QueueIndex;
extern int p1FrameCounter;
extern int p2FrameCounter;
extern bool p1QueueActive;
extern bool p2QueueActive;

// Function to queue a motion input for automatic execution
bool QueueMotionInput(int playerNum, int motionType, int buttonMask);

// Function to process queued inputs each frame
void ProcessInputQueues();

// Function to write inputs directly to memory
bool WritePlayerInput(int playerNum, uint8_t inputMask);

// Helper function to convert an action type to motion type
int ConvertActionToMotion(int actionType, int triggerType = 0);

// Helper function to convert button type to button mask
uint8_t ConvertButtonToMask(int buttonType);

// Debug helper functions
std::string GetMotionTypeName(int motionType);
std::string DecodeInputMask(uint8_t inputMask);
void LogCurrentInputs();

// Direct input control for testing
void ApplyDirectInput(int playerNum, uint8_t inputMask, int holdFrames);

// Add these function declarations
void GenerateRandomInputs(int playerNum, int durationFrames);
void ScanForInputBytes(int playerNum);
void TestMultipleInputOffsets(int playerNum, uint8_t inputMask);
void AnalyzeInputStructure(int playerNum);
void TestPromisingSingleInputAddress(int playerNum, uint8_t inputMask, int holdFrames);
void MonitorInputAddresses(int playerNum, int durationFrames);

// Add this function declaration

// Function to monitor the input buffer
void MonitorInputBuffer(int playerNum, int frameCount);
void TestInputSequence(int playerNum);