#include "../include/input_motion.h"
#include "../include/memory.h"
#include "../include/logger.h"
#include "../include/constants.h"
#include "../include/utilities.h"
#include "../include/auto_action_helpers.h"
#include <vector>
#include <sstream>
#include <chrono>
#include <iomanip>
#include <algorithm>
#include <atomic>
#include <mutex>
#include "../include/practice_patch.h"
#include <thread>
#include <chrono>

#ifdef max
#undef max // Undefine any existing max macro
#endif

// Define the missing MOTION_INPUT_BUTTON constant
#define MOTION_INPUT_BUTTON (MOTION_BUTTON_A | MOTION_BUTTON_B | MOTION_BUTTON_C | MOTION_BUTTON_D)

// Global variables for motion input system
std::vector<InputFrame> p1InputQueue;
std::vector<InputFrame> p2InputQueue;
int p1QueueIndex = 0;
int p2QueueIndex = 0;
int p1FrameCounter = 0;
int p2FrameCounter = 0;
bool p1QueueActive = false;
bool p2QueueActive = false;

// Add these globals at the top of the file with other globals
std::atomic<bool> g_bufferFreezingActive(false);
std::atomic<bool> g_indexFreezingActive(false);
std::thread g_bufferFreezeThread;
std::vector<uint8_t> g_frozenBufferValues;
uint16_t g_frozenBufferStartIndex = 0;
uint16_t g_frozenBufferLength = 0;
uint16_t g_frozenIndexValue = 0;
// Input buffer constants
const uint16_t INPUT_BUFFER_SIZE = 0x180;  // 384 bytes circular buffer
const uintptr_t INPUT_BUFFER_OFFSET = 0x1AB;  // Buffer start offset in player struct
const uintptr_t INPUT_BUFFER_INDEX_OFFSET = 0x260;  // Current buffer index offset

// REVISED: Only use buffer injection for all motion inputs
constexpr uintptr_t AI_CONTROL_FLAG_OFFSET = 164; // Confirmed from your codebase

// Direction combinations for diagonals
const uint8_t GAME_INPUT_DOWNRIGHT = GAME_INPUT_DOWN | GAME_INPUT_RIGHT;
const uint8_t GAME_INPUT_DOWNLEFT = GAME_INPUT_DOWN | GAME_INPUT_LEFT;
const uint8_t GAME_INPUT_UPRIGHT = GAME_INPUT_UP | GAME_INPUT_RIGHT;
const uint8_t GAME_INPUT_UPLEFT = GAME_INPUT_UP | GAME_INPUT_LEFT;

// Button constants for cleaner code
#define BUTTON_A    GAME_INPUT_A
#define BUTTON_B    GAME_INPUT_B
#define BUTTON_C    GAME_INPUT_C
#define BUTTON_D    GAME_INPUT_D


// Thread-safe input state tracking
std::atomic<bool> g_forceHumanControlActive(false);
static std::atomic<uint8_t> p1CurrentInput(0);
static std::atomic<uint8_t> p2CurrentInput(0);
static std::atomic<bool> p1InputLocked(false);
static std::atomic<bool> p2InputLocked(false);

// Frame timing constants
const int FRAMES_PER_INPUT = 3;
const int BUTTON_HOLD_FRAMES = 4;
const int NEUTRAL_FRAMES = 2;

// Add the TestInput structure definition after the includes, before any function that uses it
struct TestInput {
    uint8_t mask;
    const char* name;
    int holdFrames;
};

// Input register offsets (from player base pointer)
const uintptr_t INPUT_HORIZONTAL_OFFSET = 0x188;  // 1=right, 255=left, 0=neutral
const uintptr_t INPUT_VERTICAL_OFFSET = 0x189;    // 1=down, 255=up, 0=neutral
const uintptr_t INPUT_BUTTON_A_OFFSET = 0x18A; // 394
const uintptr_t INPUT_BUTTON_B_OFFSET = 0x18B; // 395
const uintptr_t INPUT_BUTTON_C_OFFSET = 0x18C; // 396
const uintptr_t INPUT_BUTTON_D_OFFSET = 0x18D; // 397

// Returns the button mask for a given motion type (used for input queueing)
uint8_t DetermineButtonFromMotionType(int motionType) {
    switch (motionType) {
        case MOTION_5A: case MOTION_2A: case MOTION_JA: case MOTION_236A: case MOTION_623A: case MOTION_214A:
            return GAME_INPUT_A;
        case MOTION_5B: case MOTION_2B: case MOTION_JB: case MOTION_236B: case MOTION_623B: case MOTION_214B:
            return GAME_INPUT_B;
        case MOTION_5C: case MOTION_2C: case MOTION_JC: case MOTION_236C: case MOTION_623C: case MOTION_214C:
            return GAME_INPUT_C;
        // Add D button motions if needed
        default:
            return 0;
    }
}

// Helper function to explicitly cast integers to uint8_t to avoid narrowing conversion warnings
inline uint8_t u8(int value) {
    return static_cast<uint8_t>(value);
}

// Helper function to get motion type name
std::string GetMotionTypeName(int motionType) {
    switch (motionType) {
        case MOTION_5A: return "5A";
        case MOTION_5B: return "5B";
        case MOTION_5C: return "5C";
        case MOTION_2A: return "2A";
        case MOTION_2B: return "2B";
        case MOTION_2C: return "2C";
        case MOTION_JA: return "j.A";
        case MOTION_JB: return "j.B";
        case MOTION_JC: return "j.C";
        case MOTION_236A: return "236A";
        case MOTION_236B: return "236B";
        case MOTION_236C: return "236C";
        case MOTION_623A: return "623A";
        case MOTION_623B: return "623B";
        case MOTION_623C: return "623C";
        case MOTION_214A: return "214A";
        case MOTION_214B: return "214B";
        case MOTION_214C: return "214C";
        case MOTION_421A: return "421A";
        case MOTION_421B: return "421B";
        case MOTION_421C: return "421C";
        case MOTION_41236A: return "41236A";
        case MOTION_41236B: return "41236B";
        case MOTION_41236C: return "41236C";
        case MOTION_63214A: return "63214A";
        case MOTION_63214B: return "63214B";
        case MOTION_63214C: return "63214C";
        case ACTION_FORWARD_DASH: return "Forward Dash";
        case ACTION_BACK_DASH: return "Back Dash";
        default: return "Unknown";
    }
}

// Decode input mask to readable string
std::string DecodeInputMask(uint8_t inputMask) {
    if (inputMask == 0)
        return "N";
    std::string result;
    
    // Directions
    if (inputMask & GAME_INPUT_UP) {
        if (inputMask & GAME_INPUT_LEFT) result += "UL";
        else if (inputMask & GAME_INPUT_RIGHT) result += "UR";
        else result += "U";
    } else if (inputMask & GAME_INPUT_DOWN) {
        if (inputMask & GAME_INPUT_LEFT) result += "DL";
        else if (inputMask & GAME_INPUT_RIGHT) result += "DR";
        else result += "D";
    } else if (inputMask & GAME_INPUT_LEFT) {
        result += "L";
    } else if (inputMask & GAME_INPUT_RIGHT) {
        result += "R";
    }
    
    // Buttons
    if (inputMask & GAME_INPUT_A) result += "A";
    if (inputMask & GAME_INPUT_B) result += "B";
    if (inputMask & GAME_INPUT_C) result += "C";
    if (inputMask & GAME_INPUT_D) result += "D";
    
    return result;
}

// Helper function to get player base pointer
uintptr_t GetPlayerPointer(int playerNum) {
    uintptr_t base = GetEFZBase();
    if (!base) return 0;
    
    uintptr_t playerPtrOffset = (playerNum == 1) ? EFZ_BASE_OFFSET_P1 : EFZ_BASE_OFFSET_P2;
    uintptr_t playerPtr = 0;
    
    if (!SafeReadMemory(base + playerPtrOffset, &playerPtr, sizeof(uintptr_t))) {
        return 0;
    }
    
    return playerPtr;
}
// Logging helper for button presses
void LogButtonPress(const char* buttonName, uintptr_t address, uint8_t value, const char* result) {
    std::ostringstream oss;
    oss << "[DEBUG_INPUT] Pressed " << buttonName << " | Addr: 0x" << std::hex << address << " | Value: 0x" << std::hex << (int)value << " | Result: " << result;
    LogOut(oss.str(), true);
}

// Simulate spamming attack button for N frames
void SpamAttackButton(uintptr_t playerBase, uint8_t button, int frames, const char* buttonName) {
    uintptr_t buttonAddr = playerBase + 0x190; // Default: A
    if (button == BUTTON_B) buttonAddr = playerBase + 0x194;
    else if (button == BUTTON_C) buttonAddr = playerBase + 0x198;
    else if (button == BUTTON_D) buttonAddr = playerBase + 0x19C;
    for (int i = 0; i < frames; ++i) {
        uint8_t press = button;
        uint8_t release = 0x00;
        SafeWriteMemory(buttonAddr, &press, 1);
        LogButtonPress(buttonName, buttonAddr, button, "Write OK");
        SafeWriteMemory(buttonAddr, &release, 1);
        LogButtonPress(buttonName, buttonAddr, 0x00, "Release");
    }
}
// Write to the circular buffer (for move history/detection)
bool WritePlayerInputToBuffer(int playerNum, uint8_t inputMask) {
    uintptr_t playerPtr = GetPlayerPointer(playerNum);
    if (!playerPtr) {
        LogOut("[INPUT_BUFFER] Failed to get player " + std::to_string(playerNum) + " pointer", true);
        return false;
    }

    // The game's input buffer is a circular array where it stores the history of inputs.
    // This is used for detecting special moves like quarter-circles.
    // We need to read the current index where the game expects the next input.
    uint16_t currentIndex = 0;
    if (!SafeReadMemory(playerPtr + INPUT_BUFFER_INDEX_OFFSET, &currentIndex, sizeof(uint16_t))) {
        LogOut("[INPUT_BUFFER] Failed to read buffer index for P" + std::to_string(playerNum), true);
        return false;
    }

    // The index wraps around the buffer size. The game engine handles the incrementing
    // and wrapping of this index. We just need to write our input to the current slot.
    uintptr_t writeAddr = playerPtr + INPUT_BUFFER_OFFSET + currentIndex;

    // Write the input mask to the calculated address in the buffer.
    if (!SafeWriteMemory(writeAddr, &inputMask, sizeof(uint8_t))) {
        LogOut("[INPUT_BUFFER] Failed to write to P" + std::to_string(playerNum) + " input buffer", true);
        return false;
    }

    // Log for debugging if detailed logging is enabled
    if (detailedLogging.load()) {
        LogOut("[INPUT_BUFFER] Wrote " + DecodeInputMask(inputMask) + " to P" + std::to_string(playerNum) + " buffer at index " + std::to_string(currentIndex), true);
    }

    return true;
}

// Update ProcessInputQueues to only manage state, not write inputs.
void ProcessInputQueues() {
    // P1 Queue Logic
    if (p1QueueActive) {
        if (p1QueueIndex < p1InputQueue.size()) {
            p1FrameCounter++;
            if (p1FrameCounter >= p1InputQueue[p1QueueIndex].durationFrames) {
                p1FrameCounter = 0;
                p1QueueIndex++;
            }
        } else {
            p1QueueActive = false;
            p1InputQueue.clear();
            p1QueueIndex = 0;
            WritePlayerInputImmediate(1, 0);
        }
    }

    // P2 Queue Logic
    if (p2QueueActive) {
        if (p2QueueIndex < p2InputQueue.size()) {
            // Only write to the buffer for motion simulation
            WritePlayerInputToBuffer(2, p2InputQueue[p2QueueIndex].inputMask);
            p2FrameCounter++;
            if (p2FrameCounter >= p2InputQueue[p2QueueIndex].durationFrames) {
                p2FrameCounter = 0;
                p2QueueIndex++;
            }
        } else {
            p2QueueActive = false;
            p2InputQueue.clear();
            p2QueueIndex = 0;
            // Optionally write a neutral input to the buffer
            WritePlayerInputToBuffer(2, 0);
        }
    }
}

// Main function to write player input - this is now only used by the hook
// and for simple, one-off inputs like auto-jump.
bool WritePlayerInput(int playerNum, uint8_t inputMask) {
    // Write to circular buffer for move detection
    bool bufferSuccess = WritePlayerInputToBuffer(playerNum, inputMask);
    
    // Write to immediate registers for current frame input
    bool immediateSuccess = WritePlayerInputImmediate(playerNum, inputMask);
    
    // Store current input state
    if (playerNum == 1) {
        p1CurrentInput.store(inputMask);
    } else {
        p2CurrentInput.store(inputMask);
    }
    
    return bufferSuccess && immediateSuccess;
}

// Add or update the DiagnoseInputSystem function with buffer inspection
void DiagnoseInputSystem(int playerNum) {
    LogOut("[INPUT_DIAG] Starting input system diagnosis for P" + std::to_string(playerNum), true);
    
    uintptr_t playerPtr = GetPlayerPointer(playerNum);
    if (!playerPtr) {
        LogOut("[INPUT_DIAG] Failed to get player pointer", true);
        return;
    }
    
    // Read immediate input registers
    uint8_t horizontal = 0, vertical = 0, btnA = 0, btnB = 0, btnC = 0, btnD = 0;
    SafeReadMemory(playerPtr + INPUT_HORIZONTAL_OFFSET, &horizontal, 1);
    SafeReadMemory(playerPtr + INPUT_VERTICAL_OFFSET, &vertical, 1);
    SafeReadMemory(playerPtr + INPUT_BUTTON_A_OFFSET, &btnA, 1);
    SafeReadMemory(playerPtr + INPUT_BUTTON_B_OFFSET, &btnB, 1);
    SafeReadMemory(playerPtr + INPUT_BUTTON_C_OFFSET, &btnC, 1);
    SafeReadMemory(playerPtr + INPUT_BUTTON_D_OFFSET, &btnD, 1);
    
    // Log the results
    LogOut("[INPUT_DIAG] Immediate input registers:", true);
    LogOut("[INPUT_DIAG]   Horizontal (0x188): " + std::to_string((int)horizontal) + 
           (horizontal == 0 ? " (neutral)" : (horizontal == 1 ? " (right)" : " (left)")), true);
    LogOut("[INPUT_DIAG]   Vertical (0x189): " + std::to_string((int)vertical) + 
           (vertical == 0 ? " (neutral)" : (vertical == 1 ? " (down)" : " (up)")), true);
    LogOut("[INPUT_DIAG]   Button A (0x18A): " + std::to_string((int)btnA), true);
    LogOut("[INPUT_DIAG]   Button B (0x18B): " + std::to_string((int)btnB), true);
    LogOut("[INPUT_DIAG]   Button C (0x18C): " + std::to_string((int)btnC), true);
    LogOut("[INPUT_DIAG]   Button D (0x18D): " + std::to_string((int)btnD), true);
    
    // Read buffer info
    uint16_t bufferIndex = 0;
    SafeReadMemory(playerPtr + INPUT_BUFFER_INDEX_OFFSET, &bufferIndex, sizeof(uint16_t));
    LogOut("[INPUT_DIAG] Buffer index (0x260): " + std::to_string(bufferIndex), true);
    
    // Test writing a sequence of inputs to the buffer
    LogOut("[INPUT_DIAG] Writing a test sequence to the buffer...", true);
    
    std::vector<InputFrame> testSequence = {
        {MOTION_INPUT_RIGHT, 4},                              // Right
        {MOTION_INPUT_RIGHT | MOTION_INPUT_DOWN, 4},          // Down-Right
        {MOTION_INPUT_DOWN, 4},                               // Down
        {MOTION_INPUT_RIGHT | MOTION_BUTTON_A, 10}            // Right+A
    };
    
    // Dump buffer before test sequence
    LogOut("[INPUT_DIAG] Buffer state BEFORE test sequence:", true);
    DumpInputBuffer(playerNum);
    
    // Write test sequence
    bool result = WriteSequentialInputs(playerNum, testSequence);
    LogOut("[INPUT_DIAG] WriteSequentialInputs returned: " + std::string(result ? "SUCCESS" : "FAILURE"), true);
    
    // Dump buffer after test sequence
    LogOut("[INPUT_DIAG] Buffer state AFTER test sequence:", true);
    DumpInputBuffer(playerNum);
    
    LogOut("[INPUT_DIAG] Input system diagnosis complete for P" + std::to_string(playerNum), true);
}


void ForceHumanControl(int playerNum) {
    uintptr_t playerPtr = GetPlayerPointer(playerNum);
    if (!playerPtr) return;
    uint32_t aiFlag = 0;
    SafeWriteMemory(playerPtr + AI_CONTROL_FLAG_OFFSET, &aiFlag, sizeof(uint32_t));
}

bool QueueMotionInput(int playerNum, int motionType, int buttonMask) {
    // Check if this is a special motion type that would benefit from buffer freezing
    bool isSpecialMotion = (
        motionType == MOTION_236A || motionType == MOTION_236B || motionType == MOTION_236C ||  // QCF
        motionType == MOTION_623A || motionType == MOTION_623B || motionType == MOTION_623C ||  // DP
        motionType == MOTION_214A || motionType == MOTION_214B || motionType == MOTION_214C ||  // QCB
        motionType == MOTION_41236A || motionType == MOTION_41236B || motionType == MOTION_41236C || // HCF
        motionType == MOTION_63214A || motionType == MOTION_63214B || motionType == MOTION_63214C || // HCB
        motionType == MOTION_421A || motionType == MOTION_421B || motionType == MOTION_421C     // 421
    );
    
    // For special motions, use the buffer freezing technique for more consistent execution
    if (isSpecialMotion) {
        return FreezeBufferForMotion(playerNum, motionType, buttonMask);
    }
    
    // For simple moves and dashes, continue with the traditional queue approach
    std::vector<uint8_t> motionSequence;
    const int DIR_FRAMES = 2;
    const int BTN_FRAMES = 3;
    const int NEUTRAL_FRAMES = 2;

    // Get player's facing direction
    bool facingRight = GetPlayerFacingDirection(playerNum);
    
    // Log facing direction for debugging
    LogOut("[INPUT_MOTION] Player " + std::to_string(playerNum) + 
           " facing " + (facingRight ? "right" : "left"), detailedLogging.load());

    // Helper to convert directional inputs based on facing direction
    auto getDirectionMask = [facingRight](uint8_t dirMask) -> uint8_t {
        if (facingRight) {
            // No change needed when facing right (default orientation)
            return dirMask;
        } else {
            // Flip horizontal directions when facing left
            uint8_t result = dirMask & ~(GAME_INPUT_LEFT | GAME_INPUT_RIGHT); // Clear left/right bits
            
            // Swap left and right
            if (dirMask & GAME_INPUT_LEFT) result |= GAME_INPUT_RIGHT;
            if (dirMask & GAME_INPUT_RIGHT) result |= GAME_INPUT_LEFT;
            
            return result;
        }
    };

    auto addInput = [&](uint8_t dirMask, uint8_t btnMask, int frames) {
        // Apply direction conversion before adding to sequence
        uint8_t adjustedDirMask = getDirectionMask(dirMask);
        for (int i = 0; i < frames; ++i)
            motionSequence.push_back(static_cast<uint8_t>(adjustedDirMask | btnMask));
    };

    // Build the input sequence for each motion type
    switch (motionType) {
        case MOTION_236A: case MOTION_236B: case MOTION_236C:
            // QCF: Down, Down-Forward, Forward + Button
            addInput(GAME_INPUT_DOWN, 0, DIR_FRAMES);
            addInput(GAME_INPUT_DOWN | GAME_INPUT_RIGHT, 0, DIR_FRAMES);
            addInput(GAME_INPUT_RIGHT, buttonMask, BTN_FRAMES);
            break;
            
        case MOTION_623A: case MOTION_623B: case MOTION_623C:
            // DP: Forward, Down, Down-Forward + Button
            addInput(GAME_INPUT_RIGHT, 0, DIR_FRAMES);
            addInput(GAME_INPUT_DOWN, 0, DIR_FRAMES);
            addInput(GAME_INPUT_DOWN | GAME_INPUT_RIGHT, buttonMask, BTN_FRAMES);
            break;
            
        case MOTION_214A: case MOTION_214B: case MOTION_214C:
            // QCB: Down, Down-Back, Back + Button
            addInput(GAME_INPUT_DOWN, 0, DIR_FRAMES);
            addInput(GAME_INPUT_DOWN | GAME_INPUT_LEFT, 0, DIR_FRAMES);
            addInput(GAME_INPUT_LEFT, buttonMask, BTN_FRAMES);
            break;
            
        case MOTION_41236A: case MOTION_41236B: case MOTION_41236C:
            // HCF: Back, Down-Back, Down, Down-Forward, Forward + Button
            addInput(GAME_INPUT_LEFT, 0, DIR_FRAMES);
            addInput(GAME_INPUT_DOWN | GAME_INPUT_LEFT, 0, DIR_FRAMES);
            addInput(GAME_INPUT_DOWN, 0, DIR_FRAMES);
            addInput(GAME_INPUT_DOWN | GAME_INPUT_RIGHT, 0, DIR_FRAMES);
            addInput(GAME_INPUT_RIGHT, buttonMask, BTN_FRAMES);
            break;
            
        case MOTION_63214A: case MOTION_63214B: case MOTION_63214C:
            // HCB: Forward, Down-Forward, Down, Down-Back, Back + Button
            addInput(GAME_INPUT_RIGHT, 0, DIR_FRAMES);
            addInput(GAME_INPUT_DOWN | GAME_INPUT_RIGHT, 0, DIR_FRAMES);
            addInput(GAME_INPUT_DOWN, 0, DIR_FRAMES);
            addInput(GAME_INPUT_DOWN | GAME_INPUT_LEFT, 0, DIR_FRAMES);
            addInput(GAME_INPUT_LEFT, buttonMask, BTN_FRAMES);
            break;
            
        case MOTION_421A: case MOTION_421B: case MOTION_421C:
            // Down, Down-Back, Back + Button
            addInput(GAME_INPUT_DOWN, 0, DIR_FRAMES);
            addInput(GAME_INPUT_DOWN | GAME_INPUT_LEFT, 0, DIR_FRAMES);
            addInput(GAME_INPUT_LEFT, buttonMask, BTN_FRAMES);
            break;
            
        case ACTION_FORWARD_DASH:
            // Forward, Neutral, Forward
            addInput(GAME_INPUT_RIGHT, 0, DIR_FRAMES);
            addInput(0, 0, NEUTRAL_FRAMES);
            addInput(GAME_INPUT_RIGHT, 0, DIR_FRAMES);
            break;
            
        case ACTION_BACK_DASH:
            // Back, Neutral, Back
            addInput(GAME_INPUT_LEFT, 0, DIR_FRAMES);
            addInput(0, 0, NEUTRAL_FRAMES);
            addInput(GAME_INPUT_LEFT, 0, DIR_FRAMES);
            break;
            
        default:
            // For normals, jump attacks, or simple directions/buttons
            if (motionType == MOTION_5A || motionType == MOTION_5B || motionType == MOTION_5C ||
                motionType == MOTION_2A || motionType == MOTION_2B || motionType == MOTION_2C ||
                motionType == MOTION_JA || motionType == MOTION_JB || motionType == MOTION_JC) {
                
                std::vector<InputFrame>& queue = (playerNum == 1) ? p1InputQueue : p2InputQueue;
                queue.clear();
                
                uint8_t dirMask = 0;
                if (motionType == MOTION_2A || motionType == MOTION_2B || motionType == MOTION_2C)
                    dirMask = GAME_INPUT_DOWN;
                else if (motionType == MOTION_JA || motionType == MOTION_JB || motionType == MOTION_JC)
                    dirMask = GAME_INPUT_UP;
                    
                // Apply facing direction to directional inputs
                dirMask = getDirectionMask(dirMask);
                
                // Add the inputs to the queue
                for (int i = 0; i < DIR_FRAMES; ++i) {
                    queue.push_back(InputFrame(dirMask, 1));
                }
                for (int i = 0; i < BTN_FRAMES; ++i) {
                    queue.push_back(InputFrame(dirMask | buttonMask, 1));
                }
                
                if (playerNum == 1) {
                    p1QueueIndex = 0; p1FrameCounter = 0; p1QueueActive = true;
                } else {
                    p2QueueIndex = 0; p2FrameCounter = 0; p2QueueActive = true;
                }
                
                LogOut("[INPUT_MOTION] Queued normal/jump " + GetMotionTypeName(motionType) + 
                       " for P" + std::to_string(playerNum) + 
                       " (facing " + (facingRight ? "right" : "left") + ")", 
                       detailedLogging.load());
                return true;
            }
            
            // Unknown or unsupported motion
            LogOut("[INPUT_MOTION] QueueMotionInput: Unknown or unsupported motionType " + 
                   std::to_string(motionType), true);
            return false;
    }

    // Add a final neutral frame for all motion inputs
    addInput(GAME_INPUT_NEUTRAL, 0, 1);

    // Create and start the input queue
    std::vector<InputFrame>& queue = (playerNum == 1) ? p1InputQueue : p2InputQueue;
    queue.clear();
    for (uint8_t input : motionSequence) {
        queue.push_back(InputFrame(input, 1));
    }
    
    if (playerNum == 1) {
        p1QueueIndex = 0;
        p1FrameCounter = 0;
        p1QueueActive = true;
    } else {
        p2QueueIndex = 0;
        p2FrameCounter = 0;
        p2QueueActive = true;
    }
    
    LogOut("[INPUT_MOTION] Queued " + GetMotionTypeName(motionType) + " for P" + 
           std::to_string(playerNum) + " (" + std::to_string(motionSequence.size()) + 
           " frames, facing " + (facingRight ? "right" : "left") + ")", 
           detailedLogging.load());
    
    return true;
}

// FIX: This function was calling an undefined function. It should call the new queue system.
bool ExecuteSimpleMoveViaInputs(int playerNum, int moveType) {
    uint8_t buttonMask = DetermineButtonFromMotionType(moveType);
    if (buttonMask == 0) {
        LogOut("[INPUT_MOTION] Could not determine button for moveType " + std::to_string(moveType), true);
        return false;
    }
    
    // This now correctly calls the fully implemented QueueMotionInput function.
    return QueueMotionInput(playerNum, moveType, buttonMask);
}

bool ExecuteSimpleMove(int playerNum, int moveType) {
    // For now, all moves will be executed via the input queue for consistency.
    return ExecuteSimpleMoveViaInputs(playerNum, moveType);
}


int ConvertActionToMotion(int actionType, int triggerType) {
    // 0 = A, 1 = B, 2 = C
    int strength = GetSpecialMoveStrength(actionType, triggerType);

    switch (actionType) {
        // Normals
        case ACTION_5A:    return MOTION_5A;
        case ACTION_5B:    return MOTION_5B;
        case ACTION_5C:    return MOTION_5C;
        case ACTION_2A:    return MOTION_2A;
        case ACTION_2B:    return MOTION_2B;
        case ACTION_2C:    return MOTION_2C;
        case ACTION_JA:    return MOTION_JA;
        case ACTION_JB:    return MOTION_JB;
        case ACTION_JC:    return MOTION_JC;

        // QCF (236)
        case ACTION_QCF:
            if (strength == 1) return MOTION_236B;
            if (strength == 2) return MOTION_236C;
            return MOTION_236A;

        // DP (623)
        case ACTION_DP:
            if (strength == 1) return MOTION_623B;
            if (strength == 2) return MOTION_623C;
            return MOTION_623A;

        // QCB (214)
        case ACTION_QCB:
            if (strength == 1) return MOTION_214B;
            if (strength == 2) return MOTION_214C;
            return MOTION_214A;

        // 421 (HCB Down)
        case ACTION_421:
            if (strength == 1) return MOTION_421B;
            if (strength == 2) return MOTION_421C;
            return MOTION_421A;

        // HCF (41236)
        case ACTION_SUPER1:
            if (strength == 1) return MOTION_41236B;
            if (strength == 2) return MOTION_41236C;
            return MOTION_41236A;

        // HCB (63214)
        case ACTION_SUPER2:
            if (strength == 1) return MOTION_63214B;
            if (strength == 2) return MOTION_63214C;
            return MOTION_63214A;

        // Dashes, jump, block
        case ACTION_JUMP:      return MOTION_JA; // Or a dedicated jump motion if you have one
        case ACTION_BACKDASH:  return ACTION_BACK_DASH;
        case ACTION_FORWARD_DASH: return ACTION_FORWARD_DASH;
        case ACTION_BLOCK:     return MOTION_NONE; // Or implement block input

        default:
            return MOTION_NONE;
    }
}
bool WritePlayerInputImmediate(int playerNum, uint8_t inputMask) {
    uintptr_t playerPtr = GetPlayerPointer(playerNum);
    if (!playerPtr) {
        return false;
    }
    bool success = true;
    uint8_t horizontal = RAW_INPUT_NEUTRAL;
    uint8_t vertical = RAW_INPUT_NEUTRAL;
    uint8_t btnA = 0, btnB = 0, btnC = 0, btnD = 0;

    // Decode horizontal direction
    if (inputMask & GAME_INPUT_RIGHT) horizontal = RAW_INPUT_RIGHT;
    else if (inputMask & GAME_INPUT_LEFT) horizontal = RAW_INPUT_LEFT;

    // Decode vertical direction
    if (inputMask & GAME_INPUT_UP) vertical = RAW_INPUT_UP;
    else if (inputMask & GAME_INPUT_DOWN) vertical = RAW_INPUT_DOWN;

    // Decode buttons
    if (inputMask & GAME_INPUT_A) btnA = 1;
    if (inputMask & GAME_INPUT_B) btnB = 1;
    if (inputMask & GAME_INPUT_C) btnC = 1;
    if (inputMask & GAME_INPUT_D) btnD = 1;

    // Write the decoded values
    if (!SafeWriteMemory(playerPtr + INPUT_HORIZONTAL_OFFSET, &horizontal, 1)) {
        LogOut("[INPUT_IMMEDIATE] Failed to write horizontal input", true);
        success = false;
    }
    if (!SafeWriteMemory(playerPtr + INPUT_VERTICAL_OFFSET, &vertical, 1)) {
        LogOut("[INPUT_IMMEDIATE] Failed to write vertical input", true);
        success = false;
    }
    if (!SafeWriteMemory(playerPtr + INPUT_BUTTON_A_OFFSET, &btnA, 1)) {
        LogOut("[INPUT_IMMEDIATE] Failed to write button A", true);
        success = false;
    }
    if (!SafeWriteMemory(playerPtr + INPUT_BUTTON_B_OFFSET, &btnB, 1)) {
        LogOut("[INPUT_IMMEDIATE] Failed to write button B", true);
        success = false;
    }
    if (!SafeWriteMemory(playerPtr + INPUT_BUTTON_C_OFFSET, &btnC, 1)) {
        LogOut("[INPUT_IMMEDIATE] Failed to write button C", true);
        success = false;
    }
    if (!SafeWriteMemory(playerPtr + INPUT_BUTTON_D_OFFSET, &btnD, 1)) {
        LogOut("[INPUT_IMMEDIATE] Failed to write button D", true);
        success = false;
    }
    if (success && detailedLogging.load()) {
        LogOut("[INPUT_IMMEDIATE] P" + std::to_string(playerNum) +
               " wrote: H=" + std::to_string((int)horizontal) +
               " V=" + std::to_string((int)vertical) +
               " A=" + std::to_string((int)btnA) +
               " B=" + std::to_string((int)btnB) +
               " C=" + std::to_string((int)btnC) +
               " D=" + std::to_string((int)btnD) +
               " (" + DecodeInputMask(inputMask) + ")", true);
    }
    return success;
}
bool HoldUp(int playerNum) {
    if (playerNum < 1 || playerNum > 2) return false;
    g_manualInputMask[playerNum].store(MOTION_INPUT_UP);
    g_manualInputOverride[playerNum].store(true);
    return true;
}

bool HoldBackCrouch(int playerNum) {
    if (playerNum < 1 || playerNum > 2) return false;
    g_manualInputMask[playerNum].store(MOTION_INPUT_DOWN | MOTION_INPUT_LEFT);
    g_manualInputOverride[playerNum].store(true);
    return true;
}

// The HoldButton functions are being removed.


// Spam A button for debug menu: alternate press/release for 6 frames
bool HoldButtonA(int playerNum) {
    if (playerNum < 1 || playerNum > 2) return false;
    std::vector<InputFrame> spamSeq = {
        InputFrame(MOTION_BUTTON_A, 1),
        InputFrame(0, 1),
        InputFrame(MOTION_BUTTON_A, 1),
        InputFrame(0, 1),
        InputFrame(MOTION_BUTTON_A, 1),
        InputFrame(0, 1)
    };
    LogOut("[DEBUG] HoldButtonA called for P" + std::to_string(playerNum), true);
    for (size_t i = 0; i < spamSeq.size(); ++i) {
        LogOut("[DEBUG] Frame " + std::to_string(i) + ": mask=" + DecodeInputMask(spamSeq[i].inputMask) + ", duration=" + std::to_string(spamSeq[i].durationFrames), true);
    }
    if (playerNum == 1) {
        p1InputQueue = spamSeq;
        p1QueueIndex = 0;
        p1FrameCounter = 0;
        p1QueueActive = true;
        LogOut("[DEBUG] P1 input queue set for HoldButtonA", true);
    } else {
        p2InputQueue = spamSeq;
        p2QueueIndex = 0;
        p2FrameCounter = 0;
        p2QueueActive = true;
        LogOut("[DEBUG] P2 input queue set for HoldButtonA", true);
    }
    return true;
}


// Spam B button for debug menu
bool HoldButtonB(int playerNum) {
    if (playerNum < 1 || playerNum > 2) return false;
    std::vector<InputFrame> spamSeq = {
        InputFrame(MOTION_BUTTON_B, 1),
        InputFrame(0, 1),
        InputFrame(MOTION_BUTTON_B, 1),
        InputFrame(0, 1),
        InputFrame(MOTION_BUTTON_B, 1),
        InputFrame(0, 1)
    };
    LogOut("[DEBUG] HoldButtonB called for P" + std::to_string(playerNum), true);
    for (size_t i = 0; i < spamSeq.size(); ++i) {
        LogOut("[DEBUG] Frame " + std::to_string(i) + ": mask=" + DecodeInputMask(spamSeq[i].inputMask) + ", duration=" + std::to_string(spamSeq[i].durationFrames), true);
    }
    if (playerNum == 1) {
        p1InputQueue = spamSeq;
        p1QueueIndex = 0;
        p1FrameCounter = 0;
        p1QueueActive = true;
        LogOut("[DEBUG] P1 input queue set for HoldButtonB", true);
    } else {
        p2InputQueue = spamSeq;
        p2QueueIndex = 0;
        p2FrameCounter = 0;
        p2QueueActive = true;
        LogOut("[DEBUG] P2 input queue set for HoldButtonB", true);
    }
    return true;
}


// Spam C button for debug menu
bool HoldButtonC(int playerNum) {
    if (playerNum < 1 || playerNum > 2) return false;
    std::vector<InputFrame> spamSeq = {
        InputFrame(MOTION_BUTTON_C, 1),
        InputFrame(0, 1),
        InputFrame(MOTION_BUTTON_C, 1),
        InputFrame(0, 1),
        InputFrame(MOTION_BUTTON_C, 1),
        InputFrame(0, 1)
    };
    LogOut("[DEBUG] HoldButtonC called for P" + std::to_string(playerNum), true);
    for (size_t i = 0; i < spamSeq.size(); ++i) {
        LogOut("[DEBUG] Frame " + std::to_string(i) + ": mask=" + DecodeInputMask(spamSeq[i].inputMask) + ", duration=" + std::to_string(spamSeq[i].durationFrames), true);
    }
    if (playerNum == 1) {
        p1InputQueue = spamSeq;
        p1QueueIndex = 0;
        p1FrameCounter = 0;
        p1QueueActive = true;
        LogOut("[DEBUG] P1 input queue set for HoldButtonC", true);
    } else {
        p2InputQueue = spamSeq;
        p2QueueIndex = 0;
        p2FrameCounter = 0;
        p2QueueActive = true;
        LogOut("[DEBUG] P2 input queue set for HoldButtonC", true);
    }
    return true;
}


// Spam D button for debug menu
bool HoldButtonD(int playerNum) {
    if (playerNum < 1 || playerNum > 2) return false;
    std::vector<InputFrame> spamSeq = {
        InputFrame(MOTION_BUTTON_D, 1),
        InputFrame(0, 1),
        InputFrame(MOTION_BUTTON_D, 1),
        InputFrame(0, 1),
        InputFrame(MOTION_BUTTON_D, 1),
        InputFrame(0, 1)
    };
    LogOut("[DEBUG] HoldButtonD called for P" + std::to_string(playerNum), true);
    for (size_t i = 0; i < spamSeq.size(); ++i) {
        LogOut("[DEBUG] Frame " + std::to_string(i) + ": mask=" + DecodeInputMask(spamSeq[i].inputMask) + ", duration=" + std::to_string(spamSeq[i].durationFrames), true);
    }
    if (playerNum == 1) {
        p1InputQueue = spamSeq;
        p1QueueIndex = 0;
        p1FrameCounter = 0;
        p1QueueActive = true;
        LogOut("[DEBUG] P1 input queue set for HoldButtonD", true);
    } else {
        p2InputQueue = spamSeq;
        p2QueueIndex = 0;
        p2FrameCounter = 0;
        p2QueueActive = true;
        LogOut("[DEBUG] P2 input queue set for HoldButtonD", true);
    }
    return true;
}

bool ReleaseInputs(int playerNum) {
    if (playerNum < 1 || playerNum > 2) return false;
    g_manualInputOverride[playerNum].store(false);
    g_manualInputMask[playerNum].store(0);
    return true;
}


void ForceHumanControlThread(int playerNum) {
    while (g_forceHumanControlActive) {
        SetAIControlFlag(playerNum, true);
        std::this_thread::sleep_for(std::chrono::microseconds(100)); // Very short sleep
    }
}

void LogNextBufferValue(int playerNum) {
    uintptr_t playerPtr = GetPlayerPointer(playerNum);
    if (!playerPtr) return;
    uint16_t currentIndex = 0;
    if (!SafeReadMemory(playerPtr + INPUT_BUFFER_INDEX_OFFSET, &currentIndex, sizeof(uint16_t)))
        return;
    uint16_t nextIndex = (currentIndex + 1) % INPUT_BUFFER_SIZE;
    uint8_t nextValue = 0;
    SafeReadMemory(playerPtr + INPUT_BUFFER_OFFSET + nextIndex, &nextValue, 1);
    LogOut("[DEBUG] Next buffer index (" + std::to_string(nextIndex) + ") value: 0x" +
           std::to_string(nextValue) + " = " + DecodeInputMask(nextValue), true);
}

// Write a motion sequence directly into the buffer, starting at current index + 1
bool InjectMotionToBuffer(int playerNum, const std::vector<uint8_t>& motionSequence, int offset) {
    uintptr_t playerPtr = GetPlayerPointer(playerNum);
    if (!playerPtr) return false;

    uint16_t currentIndex = 0;
    if (!SafeReadMemory(playerPtr + INPUT_BUFFER_INDEX_OFFSET, &currentIndex, sizeof(uint16_t)))
        return false;

    // Force human control flag again before EACH write
    SetAIControlFlag(playerNum, true);
    
    // Inject input sequence
    for (size_t i = 0; i < motionSequence.size(); ++i) {
        // Set AI flag to human AGAIN for each input - belt and suspenders approach
        if (i % 2 == 0) SetAIControlFlag(playerNum, true);
        
        uint16_t writeIndex = (currentIndex + 1 + i + offset) % INPUT_BUFFER_SIZE;
        uintptr_t writeAddr = playerPtr + INPUT_BUFFER_OFFSET + writeIndex;
        uint8_t input = motionSequence[i];
        if (!SafeWriteMemory(writeAddr, &input, sizeof(uint8_t)))
            return false;

        LogOut("[DEBUG] InjectMotionToBuffer: Wrote mask=" + DecodeInputMask(input) +
               " to index=" + std::to_string(writeIndex), detailedLogging.load());
    }
    // Set flag one final time after all writes
    SetAIControlFlag(playerNum, true);
    
    return true;
}

void SetAIControlFlag(int playerNum, bool human) {
    uintptr_t playerPtr = GetPlayerPointer(playerNum);
    if (!playerPtr) return;
    uint32_t aiFlag = human ? 0 : 1;
    SafeWriteMemory(playerPtr + AI_CONTROL_FLAG_OFFSET, &aiFlag, sizeof(uint32_t));
}

bool IsAIControlFlagHuman(int playerNum) {
    uintptr_t playerPtr = GetPlayerPointer(playerNum);
    if (!playerPtr) return false;
    uint32_t aiFlag = 1;
    SafeReadMemory(playerPtr + AI_CONTROL_FLAG_OFFSET, &aiFlag, sizeof(uint32_t));
    return aiFlag == 0;
}

// Restore AI control if P2 control is not enabled in ImGui/config
void RestoreAIControlIfNeeded(int playerNum) {
    if (playerNum != 2) return;
    extern DisplayData displayData;
    if (!displayData.p2ControlEnabled) {
        SetAIControlFlag(2, false);
        LogOut("[INPUT_MOTION] Restored P2 AI flag to AI after input injection", detailedLogging.load());
    }
}

// Add this implementation to visualize the input buffer state
void DumpInputBuffer(int playerNum) {
    uintptr_t playerPtr = GetPlayerPointer(playerNum);
    if (!playerPtr) {
        LogOut("[INPUT_DEBUG] Failed to get player pointer for P" + std::to_string(playerNum), true);
        return;
    }
    
    // Read current buffer index
    uint16_t currentIndex = 0;
    if (!SafeReadMemory(playerPtr + INPUT_BUFFER_INDEX_OFFSET, &currentIndex, sizeof(uint16_t))) {
        LogOut("[INPUT_DEBUG] Failed to read current buffer index for P" + std::to_string(playerNum), true);
        return;
    }

    // Read the entire buffer
    uint8_t buffer[INPUT_BUFFER_SIZE];
    if (!SafeReadMemory(playerPtr + INPUT_BUFFER_OFFSET, buffer, INPUT_BUFFER_SIZE)) {
        LogOut("[INPUT_DEBUG] Failed to read input buffer for P" + std::to_string(playerNum), true);
        return;
    }

    // Create header for log output
    std::stringstream header;
    header << "[INPUT_DEBUG] P" << playerNum << " Buffer Dump (Current Index: " << currentIndex << ")";
    LogOut(header.str(), true);
    
    // Output buffer contents in blocks with positions
    const int BLOCK_SIZE = 16; // Display 16 entries per line
    for (int i = 0; i < INPUT_BUFFER_SIZE; i += BLOCK_SIZE) {
        std::stringstream line;
        line << "[" << std::setw(3) << i << "] ";
        
        for (int j = 0; j < BLOCK_SIZE && (i + j) < INPUT_BUFFER_SIZE; j++) {
            // Highlight current read position
            if ((i + j) == currentIndex) {
                line << "[" << std::setw(2) << std::setfill('0') << std::hex 
                     << static_cast<int>(buffer[i + j]) << "]" << std::dec << std::setfill(' ');
            } else {
                line << " " << std::setw(2) << std::setfill('0') << std::hex 
                     << static_cast<int>(buffer[i + j]) << " " << std::dec << std::setfill(' ');
            }
            
            // Add space every 4 entries for readability
            if ((j + 1) % 4 == 0 && j < BLOCK_SIZE - 1) {
                line << "  ";
            }
        }
        
        LogOut(line.str(), true);
    }
}

// Returns a visual representation of inputs around the current position
std::string GetInputBufferVisualization(int playerNum, int window) {
    uintptr_t playerPtr = GetPlayerPointer(playerNum);
    if (!playerPtr) {
        return "Failed to get player pointer";
    }
    
    // Read current buffer index
    uint16_t currentIndex = 0;
    if (!SafeReadMemory(playerPtr + INPUT_BUFFER_INDEX_OFFSET, &currentIndex, sizeof(uint16_t))) {
        return "Failed to read buffer index";
    }
    
    // Calculate start and end positions for the window
    int halfWindow = window / 2;
    int startPos = (currentIndex - halfWindow + INPUT_BUFFER_SIZE) % INPUT_BUFFER_SIZE;
    
    // Read the relevant part of the buffer
    uint8_t windowBuffer[32]; // Max window size supported is 32
    for (int i = 0; i < window; i++) {
        int pos = (startPos + i) % INPUT_BUFFER_SIZE;
        if (!SafeReadMemory(playerPtr + INPUT_BUFFER_OFFSET + pos, &windowBuffer[i], sizeof(uint8_t))) {
            return "Failed to read buffer at position " + std::to_string(pos);
        }
    }
    
    // Create the visualization
    std::stringstream visual;
    visual << "P" << playerNum << " Buffer [idx:" << currentIndex << "] ";
    
    // Position markers
    visual << "\nPos: ";
    for (int i = 0; i < window; i++) {
        int pos = (startPos + i) % INPUT_BUFFER_SIZE;
        visual << std::setw(3) << pos;
    }
    
    // Input values in hex
    visual << "\nHex: ";
    for (int i = 0; i < window; i++) {
        int pos = (startPos + i) % INPUT_BUFFER_SIZE;
        if (pos == currentIndex) {
            visual << "[" << std::setw(1) << std::hex << static_cast<int>(windowBuffer[i]) << "]";
        } else {
            visual << " " << std::setw(1) << std::hex << static_cast<int>(windowBuffer[i]) << " ";
        }
    }
    visual << std::dec;
    
    // Input representations
    visual << "\nDir: ";
    for (int i = 0; i < window; i++) {
        int pos = (startPos + i) % INPUT_BUFFER_SIZE;
        std::string inputChar = "."; // Default is neutral
        
        uint8_t input = windowBuffer[i];
        if (input & MOTION_INPUT_UP) {
            if (input & MOTION_INPUT_RIGHT) inputChar = "9";
            else if (input & MOTION_INPUT_LEFT) inputChar = "7";
            else inputChar = "8";
        } else if (input & MOTION_INPUT_DOWN) {
            if (input & MOTION_INPUT_RIGHT) inputChar = "3";
            else if (input & MOTION_INPUT_LEFT) inputChar = "1";
            else inputChar = "2";
        } else {
            if (input & MOTION_INPUT_RIGHT) inputChar = "6";
            else if (input & MOTION_INPUT_LEFT) inputChar = "4";
            else inputChar = "5";
        }
        
        // Add button indicators
        if (input & MOTION_INPUT_BUTTON) {
            inputChar += "+";
        }
        
        if (pos == currentIndex) {
            visual << "[" << inputChar << "]";
        } else {
            visual << " " << inputChar << " ";
        }
    }
    
    return visual.str();
}

// Revised function to write a sequence of inputs with better logging and neutral handling
bool WriteSequentialInputs(int playerNum, const std::vector<InputFrame>& frames) {
    uintptr_t playerPtr = GetPlayerPointer(playerNum);
    if (!playerPtr) return false;
    
    // Get current buffer position
    uint16_t currentIndex = 0;
    if (!SafeReadMemory(playerPtr + INPUT_BUFFER_INDEX_OFFSET, &currentIndex, sizeof(uint16_t)))
        return false;
    
    // Write closer to current position (only 4-6 frames ahead instead of 10+)
    int writeIndex = (currentIndex + 5) % INPUT_BUFFER_SIZE;
    int originalWriteIndex = writeIndex;
    
    LogOut("[INPUT] Writing sequence of " + std::to_string(frames.size()) + " inputs starting at index " + 
           std::to_string(writeIndex) + " (current game index: " + std::to_string(currentIndex) + ")", 
           detailedLogging.load());
    
    // Add a neutral frame at the beginning to clear any existing inputs
    uint8_t neutralMask = MOTION_INPUT_NEUTRAL;
    SafeWriteMemory(playerPtr + INPUT_BUFFER_OFFSET + writeIndex, &neutralMask, sizeof(uint8_t));
    writeIndex = (writeIndex + 1) % INPUT_BUFFER_SIZE;
    
    // For DP motion, write each input to 2-3 consecutive positions to ensure recognition
    for (const auto& frame : frames) {
        // Write each input at least 2 times
        int repeatCount = 2;
        
        for (int i = 0; i < repeatCount; i++) {
            if (!SafeWriteMemory(playerPtr + INPUT_BUFFER_OFFSET + writeIndex, 
                               &frame.inputMask, sizeof(uint8_t))) {
                LogOut("[INPUT] Failed to write to input buffer at index " + std::to_string(writeIndex), true);
                return false;
            }
            
            LogOut("[INPUT] Wrote " + DecodeInputMask(frame.inputMask) + 
                   " at index " + std::to_string(writeIndex), true);
            
            // Move to next position in circular buffer
            writeIndex = (writeIndex + 1) % INPUT_BUFFER_SIZE;
        }
        
        // After writing the input, immediately verify it was written
        uint8_t verifyValue = 0;
        int verifyIndex = (writeIndex - 1 + INPUT_BUFFER_SIZE) % INPUT_BUFFER_SIZE;
        SafeReadMemory(playerPtr + INPUT_BUFFER_OFFSET + verifyIndex, &verifyValue, sizeof(uint8_t));
        if (verifyValue != frame.inputMask) {
            LogOut("[INPUT] WARNING: Verification failed! Expected " + 
                   DecodeInputMask(frame.inputMask) + " but read " + 
                   DecodeInputMask(verifyValue), true);
        }
    }
    
    return true;
}

// Function to freeze the current buffer section continuously
void FreezeBufferValuesThread(int playerNum) {
    LogOut("[INPUT_BUFFER] Starting buffer freeze thread for P" + std::to_string(playerNum), true);
    uintptr_t playerPtr = GetPlayerPointer(playerNum);
    
    if (!playerPtr) {
        LogOut("[INPUT_BUFFER] Failed to get player pointer for freezing", true);
        return;
    }
    
    // Get the move ID address for this player
    uintptr_t base = GetEFZBase();
    uintptr_t moveIDAddr = ResolvePointer(base, 
        (playerNum == 1) ? EFZ_BASE_OFFSET_P1 : EFZ_BASE_OFFSET_P2, 
        MOVE_ID_OFFSET);
    
    int freezeCount = 0;
    int lastMoveID = -1;
    uint16_t lastIndex = 0xFFFF;
    int stabilityCounter = 0;
    std::vector<uint8_t> actualBufferValues(g_frozenBufferLength);
    
    while (g_bufferFreezingActive) {
        freezeCount++;
        
        // First ensure human control flag is set (twice per loop for reliability)
        SetAIControlFlag(playerNum, true);
        
        // Read current buffer index and move ID
        uint16_t currentIndex = 0;
        short currentMoveID = 0;
        
        bool indexRead = SafeReadMemory(playerPtr + INPUT_BUFFER_INDEX_OFFSET, &currentIndex, sizeof(uint16_t));
        bool moveIDRead = moveIDAddr && SafeReadMemory(moveIDAddr, &currentMoveID, sizeof(short));
        
        // Check if values match what we're trying to set
        bool indexMatch = (currentIndex == g_frozenIndexValue);
        
        // Read what's actually in buffer to verify our writes are taking effect
        bool bufferMatch = true;
        if (freezeCount % 30 == 0 || !indexMatch) {
            for (size_t i = 0; i < g_frozenBufferLength; i++) {
                uint16_t readIdx = (g_frozenBufferStartIndex + i) % INPUT_BUFFER_SIZE;
                uint8_t value = 0;
                SafeReadMemory(playerPtr + INPUT_BUFFER_OFFSET + readIdx, &value, sizeof(uint8_t));
                actualBufferValues[i] = value;
                if (i >= 20 && i < 30 && value != g_frozenBufferValues[i]) {
                    bufferMatch = false;
                }
            }
        }
        
        // Track stability - how long have we maintained correct state
        if (indexMatch && bufferMatch) {
            stabilityCounter++;
        } else {
            stabilityCounter = 0;
        }
        
        // Log on events or periodically
        bool shouldLog = (freezeCount % 60 == 0) || 
                         (moveIDRead && currentMoveID != lastMoveID) || 
                         (!indexMatch && freezeCount % 10 == 0);
        
        if (shouldLog) {
            LogOut(std::string("[BUFFER_STATE] Iter=") + std::to_string(freezeCount) + 
                   " Index=" + std::to_string(currentIndex) + 
                   " Target=" + std::to_string(g_frozenIndexValue) + 
                   " MoveID=" + std::to_string(currentMoveID) +
                   " Stability=" + std::to_string(stabilityCounter) +
                   (bufferMatch ? " [Buffer OK]" : " [Buffer Mismatch]"), true);
            
            if (currentMoveID != lastMoveID) {
                // When move changes, dump detailed buffer state
                std::stringstream ss;
                ss << "[BUFFER_CHANGE] MoveID changed " << lastMoveID << " → " << currentMoveID << "\n";
                ss << "Buffer content: ";
                for (int i = 20; i < 30 && i < actualBufferValues.size(); i++) {
                    ss << std::hex << std::setw(2) << std::setfill('0') 
                       << static_cast<int>(actualBufferValues[i]) << "(" << DecodeInputMask(actualBufferValues[i]) << ") ";
                }
                LogOut(ss.str(), true);
            }
        }
        
        // Write frozen values with precise timing pattern
        // Freeze index first
        if (g_indexFreezingActive && playerPtr) {
            SafeWriteMemory(playerPtr + INPUT_BUFFER_INDEX_OFFSET, &g_frozenIndexValue, sizeof(uint16_t));
            // Small delay to let index write take effect
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
        
        // Then write buffer values
        if (playerPtr && g_frozenBufferLength > 0) {
            SetAIControlFlag(playerNum, true); // Ensure control before buffer write
            
            for (size_t i = 0; i < g_frozenBufferLength; i++) {
                uint16_t writeIndex = (g_frozenBufferStartIndex + i) % INPUT_BUFFER_SIZE;
                uintptr_t writeAddr = playerPtr + INPUT_BUFFER_OFFSET + writeIndex;
                SafeWriteMemory(writeAddr, &g_frozenBufferValues[i], sizeof(uint8_t));
            }
        }
        
        lastMoveID = currentMoveID;
        lastIndex = currentIndex;
        
        // Dynamic sleep time - use shorter interval when index doesn't match
        std::this_thread::sleep_for(std::chrono::microseconds(indexMatch ? 500 : 100));
    }
    
    LogOut("[INPUT_BUFFER] Buffer freeze thread terminated", true);
}

// Capture current buffer section and begin freezing it
bool CaptureAndFreezeBuffer(int playerNum, uint16_t startIndex, uint16_t length) {
    // Stop any existing freeze thread
    StopBufferFreezing();
    
    uintptr_t playerPtr = GetPlayerPointer(playerNum);
    if (!playerPtr) {
        LogOut("[INPUT_BUFFER] Failed to get player pointer for capture", true);
        return false;
    }
    
    // Validate parameters
    if (length > INPUT_BUFFER_SIZE) {
        length = INPUT_BUFFER_SIZE;
    }
    
    // Read current buffer index for reference
    uint16_t currentIndex = 0;
    if (!SafeReadMemory(playerPtr + INPUT_BUFFER_INDEX_OFFSET, &currentIndex, sizeof(uint16_t))) {
        LogOut("[INPUT_BUFFER] Failed to read current buffer index", true);
        return false;
    }
    
    // Read buffer values
    g_frozenBufferValues.resize(length);
    g_frozenBufferStartIndex = startIndex;
    g_frozenBufferLength = length;
    
    bool readSuccess = true;
    for (size_t i = 0; i < length; i++) {
        uint16_t readIndex = (startIndex + i) % INPUT_BUFFER_SIZE;
        if (!SafeReadMemory(playerPtr + INPUT_BUFFER_OFFSET + readIndex, &g_frozenBufferValues[i], sizeof(uint8_t))) {
            LogOut("[INPUT_BUFFER] Failed to read buffer at index " + std::to_string(readIndex), true);
            readSuccess = false;
        }
    }
    
    if (!readSuccess) {
        LogOut("[INPUT_BUFFER] Some buffer values couldn't be read", true);
        return false;
    }
    
    // Output the captured values
    std::stringstream ss;
    ss << "[INPUT_BUFFER] Captured values: ";
    for (size_t i = 0; i < length; i++) {
        ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(g_frozenBufferValues[i]) << " ";
    }
    LogOut(ss.str(), true);
    
    // Start freezing
    g_bufferFreezingActive = true;
    g_bufferFreezeThread = std::thread(FreezeBufferValuesThread, playerNum);
    
    LogOut("[INPUT_BUFFER] Buffer freezing activated for P" + std::to_string(playerNum) + 
           " starting at index " + std::to_string(startIndex) +
           " with length " + std::to_string(length), true);
    return true;
}

// Option to also freeze buffer index
bool FreezeBufferIndex(int playerNum, uint16_t indexValue) {
    uintptr_t playerPtr = GetPlayerPointer(playerNum);
    if (!playerPtr) {
        LogOut("[INPUT_BUFFER] Failed to get player pointer for index freezing", true);
        return false;
    }
    
    g_frozenIndexValue = indexValue;
    g_indexFreezingActive = true;
    
    LogOut("[INPUT_BUFFER] Buffer index freezing activated, locked to " + std::to_string(indexValue), true);
    return true;
}

// Function to stop buffer freezing
void StopBufferFreezing() {
    if (g_bufferFreezingActive) {
        g_bufferFreezingActive = false;
        g_indexFreezingActive = false;
        
        if (g_bufferFreezeThread.joinable()) {
            g_bufferFreezeThread.join();
        }
        
        LogOut("[INPUT_BUFFER] Buffer freezing stopped", true);
    }
}

// Helper to freeze the perfect Dragon Punch motion
bool FreezePerfectDragonPunch(int playerNum) {
    // Stop any existing freeze thread
    StopBufferFreezing();
    
    LogOut("[BUFFER_FREEZE] Starting Dragon Punch buffer freeze for P" + std::to_string(playerNum), true);
    
    // Use the exact values from your successful DP execution
    std::vector<uint8_t> dpMotion = {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x02, 0x02, 0x02, 0x04, 
        0x04, 0x04, 0x26, 0x06, 0x06, 0x06, 0x00, 0x00
    };
    
    // Detailed logging of input sequence
    std::stringstream ss;
    ss << "[BUFFER_FREEZE] DP motion buffer values: ";
    for (size_t i = 0; i < dpMotion.size(); i++) {
        if (dpMotion[i] != 0) {
            ss << std::hex << std::setw(2) << std::setfill('0') 
               << static_cast<int>(dpMotion[i]) << "(" << DecodeInputMask(dpMotion[i]) << ") ";
        } else if (i > 0 && dpMotion[i-1] != 0) {
            ss << "00 ";  // Only show zeros that follow non-zero values
        }
    }
    LogOut(ss.str(), true);
    
    // Setup the frozen buffer values
    g_frozenBufferValues = dpMotion;
    g_frozenBufferLength = dpMotion.size();
    g_frozenBufferStartIndex = 128; // Place in middle of buffer for visibility
    
    // Set index to point at position 148 (128+20)
    g_frozenIndexValue = (g_frozenBufferStartIndex + 20) % INPUT_BUFFER_SIZE;
    g_indexFreezingActive = true;
    
    // Start freezing thread
    g_bufferFreezingActive = true;
    g_bufferFreezeThread = std::thread(FreezeBufferValuesThread, playerNum);
    g_bufferFreezeThread.detach();  // Detach to prevent termination
    
    LogOut("[BUFFER_FREEZE] Perfect Dragon Punch motion freezing activated at index " + 
           std::to_string(g_frozenIndexValue), true);
    return true;
}

// Enhanced version with diagnostic dump and adjusted index placement
bool FreezePerfectDragonPunchEnhanced(int playerNum) {
    // Stop any existing freeze thread
    StopBufferFreezing();
    
    LogOut("[BUFFER_DEBUG] Starting enhanced DP buffer freeze for P" + std::to_string(playerNum), true);
    
    // Get player pointer
    uintptr_t playerPtr = GetPlayerPointer(playerNum);
    if (!playerPtr) {
        LogOut("[BUFFER_DEBUG] Failed to get player pointer", true);
        return false;
    }
    
    // Read current buffer index for reference
    uint16_t currentIndex = 0;
    if (!SafeReadMemory(playerPtr + INPUT_BUFFER_INDEX_OFFSET, &currentIndex, sizeof(uint16_t))) {
        LogOut("[BUFFER_DEBUG] Failed to read current buffer index", true);
        return false;
    }
    
    // First dump the current buffer contents for diagnostic purposes
    DumpInputBuffer(playerNum);
    
    // Use the exact DP motion sequence from the console dump
    std::vector<uint8_t> dpMotion = {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x02, 0x02, 0x02, 0x04, 
        0x04, 0x04, 0x26, 0x06, 0x06, 0x00, 0x00, 0x00
    };
    
    // Calculate index based on current buffer position
    // We want to place the motion sequence just before the current read position
    g_frozenBufferStartIndex = (currentIndex + INPUT_BUFFER_SIZE - dpMotion.size()) % INPUT_BUFFER_SIZE;
    g_frozenBufferValues = dpMotion;
    g_frozenBufferLength = dpMotion.size();
    
    // Set the index to point at the start of the important part of the sequence
    g_frozenIndexValue = (g_frozenBufferStartIndex + 20) % INPUT_BUFFER_SIZE;
    g_indexFreezingActive = true;
    
    LogOut("[BUFFER_DEBUG] Current buffer index: " + std::to_string(currentIndex), true);
    LogOut("[BUFFER_DEBUG] Setting buffer start at: " + std::to_string(g_frozenBufferStartIndex), true);
    LogOut("[BUFFER_DEBUG] Setting frozen index to: " + std::to_string(g_frozenIndexValue), true);
    
    // Detailed logging of input sequence
    std::stringstream ss;
    ss << "[BUFFER_DEBUG] DP motion sequence: ";
    for (size_t i = 0; i < dpMotion.size(); i++) {
        ss << std::hex << std::setw(2) << std::setfill('0') 
           << static_cast<int>(dpMotion[i]) << "(" << DecodeInputMask(dpMotion[i]) << ") ";
    }
    LogOut(ss.str(), true);
    
    // Start freezing thread
    g_bufferFreezingActive = true;
    g_bufferFreezeThread = std::thread([playerNum]() {
        LogOut("[BUFFER_DEBUG] Enhanced freeze thread starting - won't terminate until Numpad 5 pressed", true);
        FreezeBufferValuesThread(playerNum);
    });
    g_bufferFreezeThread.detach();  // Detach to prevent termination
    
    LogOut("[BUFFER_DEBUG] Enhanced DP buffer freeze activated for P" + 
           std::to_string(playerNum), true);
    return true;
}

bool ComboFreezeDP(int playerNum) {
    // Stop any existing freeze thread
    StopBufferFreezing();
    
    LogOut("[BUFFER_COMBO] Starting CheatEngine-style DP freeze for P" + std::to_string(playerNum), true);
    
    uintptr_t playerPtr = GetPlayerPointer(playerNum);
    if (!playerPtr) {
        LogOut("[BUFFER_COMBO] Failed to get player pointer", true);
        return false;
    }

    // Read initial buffer state
    uint16_t currentIndex = 0;
    SafeReadMemory(playerPtr + INPUT_BUFFER_INDEX_OFFSET, &currentIndex, sizeof(uint16_t));
    LogOut("[BUFFER_COMBO] Initial buffer index: " + std::to_string(currentIndex), true);
    
    // This is the exact CheatEngine pattern that works
    std::vector<uint8_t> dpMotion = {
        0x02, 0x02, 0x02,  // LEFT x3
        0x04, 0x04, 0x04,  // DOWN x3
        0x26,              // DOWN+LEFT+BUTTON
        0x06, 0x06, 0x06, 0x06, // DOWN+LEFT x4
        0x26, 0x26, 0x26, 0x26  // DOWN+LEFT+BUTTON x4
    };
    
    // Target index where we want to maintain the pattern
    g_frozenIndexValue = 149;
    g_frozenBufferValues = dpMotion;
    g_frozenBufferLength = dpMotion.size();
    
    // Enable buffer and index manipulation
    g_indexFreezingActive = true;
    g_bufferFreezingActive = true;
    
    // Start buffer freeze thread
    g_bufferFreezeThread = std::thread([playerNum, dpMotion]() {
        LogOut("[BUFFER_COMBO] Starting DP pattern buffer freeze thread", true);
        uintptr_t playerPtr = GetPlayerPointer(playerNum);
        if (!playerPtr) return;
        
        int counter = 0;
        uint16_t lastIndex = 0;
        short lastMoveID = -1;
        
        // Main freeze loop
        while (g_bufferFreezingActive) {
            // Read current index and moveID for monitoring
            uint16_t currentIndex = 0;
            short moveID = 0;
            SafeReadMemory(playerPtr + INPUT_BUFFER_INDEX_OFFSET, &currentIndex, sizeof(uint16_t));
            
            // Optional: Read moveID for logging
            uintptr_t moveIDAddr = ResolvePointer(GetEFZBase(), 
                (playerNum == 1) ? EFZ_BASE_OFFSET_P1 : EFZ_BASE_OFFSET_P2, 
                MOVE_ID_OFFSET);
            if (moveIDAddr) {
                SafeReadMemory(moveIDAddr, &moveID, sizeof(short));
                if (moveID != lastMoveID && moveID != 0) {
                    LogOut("[BUFFER_COMBO] MoveID changed: " + std::to_string(lastMoveID) + 
                          " → " + std::to_string(moveID), true);
                    lastMoveID = moveID;
                }
            }
            
            // Allow index to float in 149-152 range, only reset if it's outside
            if (currentIndex < 147 || currentIndex > 152) {
                SafeWriteMemory(playerPtr + INPUT_BUFFER_INDEX_OFFSET, &g_frozenIndexValue, sizeof(uint16_t));
                currentIndex = g_frozenIndexValue;
            }
            
            // Always write the pattern at multiple positions relative to the current index
            // This ensures it's found no matter which exact index the game checks
            for (int offset = -2; offset <= 2; offset++) {
                int basePos = (currentIndex - dpMotion.size() / 2 + offset) % INPUT_BUFFER_SIZE;
                if (basePos < 0) basePos += INPUT_BUFFER_SIZE;
                
                // Write the pattern
                for (size_t i = 0; i < dpMotion.size(); i++) {
                    uint16_t writeIndex = (basePos + i) % INPUT_BUFFER_SIZE;
                    SafeWriteMemory(playerPtr + INPUT_BUFFER_OFFSET + writeIndex, 
                                  &dpMotion[i], sizeof(uint8_t));
                }
            }
            
            // Also make sure we write the exact pattern at fixed locations that are known to work
            const int knownGoodStart = 144;
            for (size_t i = 0; i < dpMotion.size(); i++) {
                SafeWriteMemory(playerPtr + INPUT_BUFFER_OFFSET + knownGoodStart + i, 
                              &dpMotion[i], sizeof(uint8_t));
            }
            
            // Log periodically or on index change
            if (currentIndex != lastIndex || counter % 100 == 0) {
                LogOut("[BUFFER_COMBO] Maintaining buffer pattern at index: " + 
                      std::to_string(currentIndex), true);
                lastIndex = currentIndex;
            }
            
            counter++;
            
            // Use short sleep for responsiveness
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });
    g_bufferFreezeThread.detach();
    
    LogOut("[BUFFER_COMBO] DP buffer pattern freeze activated", true);
    return true;
}

bool FreezeBufferForMotion(int playerNum, int motionType, int buttonMask) {
    // Stop any existing freeze thread
    StopBufferFreezing();
    
    LogOut("[BUFFER_FREEZE] Starting buffer freeze for motion " + GetMotionTypeName(motionType) + " (P" + std::to_string(playerNum) + ")", true);
    
    uintptr_t playerPtr = GetPlayerPointer(playerNum);
    if (!playerPtr) {
        LogOut("[BUFFER_FREEZE] Failed to get player pointer", true);
        return false;
    }

    // Read initial buffer state
    uint16_t currentIndex = 0;
    SafeReadMemory(playerPtr + INPUT_BUFFER_INDEX_OFFSET, &currentIndex, sizeof(uint16_t));
    LogOut("[BUFFER_FREEZE] Initial buffer index: " + std::to_string(currentIndex), true);
    
    // Get player's facing direction
    bool facingRight = GetPlayerFacingDirection(playerNum);
    LogOut("[BUFFER_FREEZE] Player " + std::to_string(playerNum) + " facing " + (facingRight ? "right" : "left"), true);
    
    // Generate buffer pattern based on motion type
    std::vector<uint8_t> motionPattern;
    
    // Helper to adjust directions based on facing
    auto adjustDirection = [facingRight](uint8_t dirMask) -> uint8_t {
        if (facingRight) {
            return dirMask;
        } else {
            // Flip horizontal directions when facing left
            uint8_t result = dirMask & ~(GAME_INPUT_LEFT | GAME_INPUT_RIGHT); // Clear left/right bits
            
            // Swap left and right
            if (dirMask & GAME_INPUT_LEFT) result |= GAME_INPUT_RIGHT;
            if (dirMask & GAME_INPUT_RIGHT) result |= GAME_INPUT_LEFT;
            
            return result;
        }
    };
    
    // Populate the buffer pattern based on motion type
    switch (motionType) {
        case MOTION_236A: case MOTION_236B: case MOTION_236C: {
            // QCF: Down, Down-Forward, Forward + Button
            uint8_t down = adjustDirection(GAME_INPUT_DOWN);
            uint8_t downFwd = adjustDirection(GAME_INPUT_DOWN | GAME_INPUT_RIGHT);
            uint8_t fwd = adjustDirection(GAME_INPUT_RIGHT);
            uint8_t btn = buttonMask;
            
            // Pattern: Neutral → Down → Down-Forward → Forward+Button
            motionPattern = {
                u8(0x00), u8(0x00), u8(0x00), // Neutral padding
                u8(down), u8(down), u8(down), // Down x3
                u8(downFwd), u8(downFwd), u8(downFwd), // Down-Forward x3
                u8(fwd | btn), u8(fwd | btn), u8(fwd | btn) // Forward+Button x3
            };
            break;
        }
        
        case MOTION_623A: case MOTION_623B: case MOTION_623C: {
            // DP: Forward, Down, Down-Forward + Button
            uint8_t fwd = adjustDirection(GAME_INPUT_RIGHT);
            uint8_t down = adjustDirection(GAME_INPUT_DOWN);
            uint8_t downFwd = adjustDirection(GAME_INPUT_DOWN | GAME_INPUT_RIGHT);
            uint8_t btn = buttonMask;
            
            // Use the exact pattern that works from CheatEngine
            motionPattern = {
                u8(0x00), u8(0x00), u8(0x00), // Neutral padding
                u8(fwd), u8(fwd), u8(fwd), // Forward x3
                u8(down), u8(down), u8(down), // Down x3
                u8(downFwd | btn), // Down-Forward+Button x1
                u8(downFwd), u8(downFwd), u8(downFwd), u8(downFwd), // Down-Forward x4
                u8(downFwd | btn), u8(downFwd | btn), u8(downFwd | btn), u8(downFwd | btn) // Down-Forward+Button x4
            };
            break;
        }
        
        case MOTION_214A: case MOTION_214B: case MOTION_214C: {
            // QCB: Down, Down-Back, Back + Button
            uint8_t down = adjustDirection(GAME_INPUT_DOWN);
            uint8_t downBack = adjustDirection(GAME_INPUT_DOWN | GAME_INPUT_LEFT);
            uint8_t back = adjustDirection(GAME_INPUT_LEFT);
            uint8_t btn = buttonMask;
            
            // Pattern: Neutral → Down → Down-Back → Back+Button
            motionPattern = {
                u8(0x00), u8(0x00), u8(0x00), // Neutral padding
                u8(down), u8(down), u8(down), // Down x3
                u8(downBack), u8(downBack), // Down-Back x2
                u8(back | btn), u8(back | btn), u8(back | btn), u8(back | btn) // Back+Button x4
            };
            break;
        }
        
        case MOTION_41236A: case MOTION_41236B: case MOTION_41236C: {
            // HCF: Back, Down-Back, Down, Down-Forward, Forward + Button
            uint8_t back = adjustDirection(GAME_INPUT_LEFT);
            uint8_t downBack = adjustDirection(GAME_INPUT_DOWN | GAME_INPUT_LEFT);
            uint8_t down = adjustDirection(GAME_INPUT_DOWN);
            uint8_t downFwd = adjustDirection(GAME_INPUT_DOWN | GAME_INPUT_RIGHT);
            uint8_t fwd = adjustDirection(GAME_INPUT_RIGHT);
            uint8_t btn = buttonMask;
            
            // Pattern for HCF
            motionPattern = {
                u8(back), u8(back), // Back x2
                u8(downBack), u8(downBack), // Down-Back x2
                u8(down), u8(down), // Down x2
                u8(downFwd), u8(downFwd), // Down-Forward x2
                u8(fwd | btn), u8(fwd | btn), u8(fwd | btn), u8(fwd | btn) // Forward+Button x4
            };
            break;
        }
        
        case MOTION_63214A: case MOTION_63214B: case MOTION_63214C: {
            // HCB: Forward, Down-Forward, Down, Down-Back, Back + Button
            uint8_t fwd = adjustDirection(GAME_INPUT_RIGHT);
            uint8_t downFwd = adjustDirection(GAME_INPUT_DOWN | GAME_INPUT_RIGHT);
            uint8_t down = adjustDirection(GAME_INPUT_DOWN);
            uint8_t downBack = adjustDirection(GAME_INPUT_DOWN | GAME_INPUT_LEFT);
            uint8_t back = adjustDirection(GAME_INPUT_LEFT);
            uint8_t btn = buttonMask;
            
            // Pattern for HCB
            motionPattern = {
                u8(fwd), u8(fwd), // Forward x2
                u8(downFwd), u8(downFwd), // Down-Forward x2
                u8(down), u8(down), // Down x2
                u8(downBack), u8(downBack), // Down-Back x2
                u8(back | btn), u8(back | btn), u8(back | btn), u8(back | btn) // Back+Button x4
            };
            break;
        }
        
        case MOTION_421A: case MOTION_421B: case MOTION_421C: {
            // Half Circle Back Down: Down, Down-Back, Back + Button
            uint8_t down = adjustDirection(GAME_INPUT_DOWN);
            uint8_t downBack = adjustDirection(GAME_INPUT_DOWN | GAME_INPUT_LEFT);
            uint8_t back = adjustDirection(GAME_INPUT_LEFT);
            uint8_t btn = buttonMask;
            
            // Pattern for 421
            motionPattern = {
                u8(0x00), u8(0x00), u8(0x00), // Neutral padding
                u8(down), u8(down), u8(down), // Down x3
                u8(downBack), u8(downBack), // Down-Back x2
                u8(back | btn), u8(back | btn), u8(back | btn), u8(back | btn) // Back+Button x4
            };
            break;
        }
        
        case ACTION_FORWARD_DASH: {
            // Forward, Neutral, Forward
            uint8_t fwd = adjustDirection(GAME_INPUT_RIGHT);
            
            motionPattern = {
                u8(0x00), u8(0x00), u8(0x00), // Neutral padding
                u8(fwd), u8(fwd), // Forward x2
                u8(0x00), u8(0x00), // Neutral x2
                u8(fwd), u8(fwd), u8(fwd), u8(fwd) // Forward x4
            };
            break;
        }
        
        case ACTION_BACK_DASH: {
            // Back, Neutral, Back
            uint8_t back = adjustDirection(GAME_INPUT_LEFT);
            
            motionPattern = {
                u8(0x00), u8(0x00), u8(0x00), // Neutral padding
                u8(back), u8(back), // Back x2
                u8(0x00), u8(0x00), // Neutral x2
                u8(back), u8(back), u8(back), u8(back) // Back x4
            };
            break;
        }
        
        default:
            // For unknown motion types, default to a simple forward+button pattern
            uint8_t fwd = adjustDirection(GAME_INPUT_RIGHT);
            motionPattern = {
                u8(0x00), u8(0x00), u8(0x00), u8(0x00),
                u8(fwd | buttonMask), u8(fwd | buttonMask), u8(fwd | buttonMask), u8(fwd | buttonMask)
            };
            LogOut("[BUFFER_FREEZE] Warning: Unknown motion type " + std::to_string(motionType) + 
                  ", using default pattern", true);
    }
    
    // Diagnostic logging of the motion pattern
    std::stringstream ss;
    ss << "[BUFFER_FREEZE] Pattern values for " << GetMotionTypeName(motionType) << ": ";
    for (size_t i = 0; i < motionPattern.size(); i++) {
        ss << std::hex << std::setw(2) << std::setfill('0') 
           << static_cast<int>(motionPattern[i]) << "(" 
           << DecodeInputMask(motionPattern[i]) << ") ";
    }
    LogOut(ss.str(), true);
    
    // Target index where we want to maintain the pattern
    g_frozenIndexValue = 149; // This is the optimal index from testing
    g_frozenBufferValues = motionPattern;
    g_frozenBufferLength = motionPattern.size();
    
    // Enable buffer and index manipulation
    g_indexFreezingActive = true;
    g_bufferFreezingActive = true;
    
    // Start buffer freeze thread
    g_bufferFreezeThread = std::thread([playerNum, motionPattern, motionType]() {
        LogOut("[BUFFER_FREEZE] Starting pattern buffer freeze thread for " + 
              GetMotionTypeName(motionType), true);
        
        uintptr_t playerPtr = GetPlayerPointer(playerNum);
        if (!playerPtr) return;
        
        int counter = 0;
        uint16_t lastIndex = 0;
        short lastMoveID = -1;
        
        // Main freeze loop
        while (g_bufferFreezingActive) {
            // Read current index and moveID for monitoring
            uint16_t currentIndex = 0;
            short moveID = 0;
            SafeReadMemory(playerPtr + INPUT_BUFFER_INDEX_OFFSET, &currentIndex, sizeof(uint16_t));
            
            // Optional: Read moveID for logging
            uintptr_t moveIDAddr = ResolvePointer(GetEFZBase(), 
                (playerNum == 1) ? EFZ_BASE_OFFSET_P1 : EFZ_BASE_OFFSET_P2, 
                MOVE_ID_OFFSET);
                
            if (moveIDAddr) {
                SafeReadMemory(moveIDAddr, &moveID, sizeof(short));
                if (moveID != lastMoveID && moveID != 0) {
                    LogOut("[BUFFER_FREEZE] MoveID changed: " + std::to_string(lastMoveID) + 
                          " → " + std::to_string(moveID), true);
                    
                    // If the move is recognized (special move ID range), stop the freeze thread
                    if (moveID >= 250 && moveID <= 300) {
                        LogOut("[BUFFER_FREEZE] Motion recognized! Detected move with ID: " + 
                              std::to_string(moveID), true);
                        g_bufferFreezingActive = false;
                        break;
                    }
                    
                    lastMoveID = moveID;
                }
            }
            
            // Allow index to float in optimal range, only reset if it's outside
            if (currentIndex < 147 || currentIndex > 152) {
                SafeWriteMemory(playerPtr + INPUT_BUFFER_INDEX_OFFSET, &g_frozenIndexValue, sizeof(uint16_t));
                currentIndex = g_frozenIndexValue;
            }
            
            // Always write the pattern at multiple positions relative to the current index
            // This ensures it's found no matter which exact index the game checks
            for (int offset = -2; offset <= 2; offset++) {
                int basePos = (currentIndex - motionPattern.size() / 2 + offset) % INPUT_BUFFER_SIZE;
                if (basePos < 0) basePos += INPUT_BUFFER_SIZE;
                
                // Write the pattern
                for (size_t i = 0; i < motionPattern.size(); i++) {
                    uint16_t writeIndex = (basePos + i) % INPUT_BUFFER_SIZE;
                    SafeWriteMemory(playerPtr + INPUT_BUFFER_OFFSET + writeIndex, 
                                   &motionPattern[i], sizeof(uint8_t));
                }
            }
            
            // Also make sure we write the exact pattern at known good positions
            const int knownGoodStart = 144;
            for (size_t i = 0; i < motionPattern.size(); i++) {
                SafeWriteMemory(playerPtr + INPUT_BUFFER_OFFSET + knownGoodStart + i, 
                               &motionPattern[i], sizeof(uint8_t));
            }
            
            // Log periodically or on index change
            if (currentIndex != lastIndex || counter % 100 == 0) {
                LogOut("[BUFFER_FREEZE] Maintaining " + GetMotionTypeName(motionType) + 
                       " pattern at index: " + std::to_string(currentIndex), 
                       detailedLogging.load());
                lastIndex = currentIndex;
            }
            
            counter++;
            
            // Use short sleep for responsiveness
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        
        LogOut("[BUFFER_FREEZE] Buffer freeze thread for " + GetMotionTypeName(motionType) + 
               " ended", true);
    });
    g_bufferFreezeThread.detach();
    
    LogOut("[BUFFER_FREEZE] " + GetMotionTypeName(motionType) + 
           " buffer pattern freeze activated", true);
    return true;
}