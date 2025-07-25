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

// Game's input encoding (verified from assembly)
constexpr uint8_t GAME_INPUT_RIGHT = 0x01;  // Bit 0
constexpr uint8_t GAME_INPUT_LEFT  = 0x02;  // Bit 1
constexpr uint8_t GAME_INPUT_DOWN  = 0x04;  // Bit 2
constexpr uint8_t GAME_INPUT_UP    = 0x08;  // Bit 3
constexpr uint8_t GAME_INPUT_A     = 0x10;  // Bit 4
constexpr uint8_t GAME_INPUT_B     = 0x20;  // Bit 5
constexpr uint8_t GAME_INPUT_C     = 0x40;  // Bit 6
constexpr uint8_t GAME_INPUT_D     = 0x80;  // Bit 7


// Global variables for motion input system
std::vector<InputFrame> p1InputQueue;
std::vector<InputFrame> p2InputQueue;
int p1QueueIndex = 0;
int p2QueueIndex = 0;
int p1FrameCounter = 0;
int p2FrameCounter = 0;
bool p1QueueActive = false;
bool p2QueueActive = false;

// Input buffer constants
const uint16_t INPUT_BUFFER_SIZE = 0x180;  // 384 bytes circular buffer
const uintptr_t INPUT_BUFFER_OFFSET = 0x1AB;  // Buffer start offset in player struct
const uintptr_t INPUT_BUFFER_INDEX_OFFSET = 0x260;  // Current buffer index offset


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
const uintptr_t INPUT_BUTTON_A_OFFSET = 0x190;    // 1=pressed, 0=not pressed
const uintptr_t INPUT_BUTTON_B_OFFSET = 0x194;    // 1=pressed, 0=not pressed
const uintptr_t INPUT_BUTTON_C_OFFSET = 0x198;    // 1=pressed, 0=not pressed
const uintptr_t INPUT_BUTTON_D_OFFSET = 0x19C;    // 1=pressed, 0=not pressed

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
            // Queue is finished
            p1QueueActive = false;
            p1InputQueue.clear();
            p1QueueIndex = 0;
            // Write a neutral input to prevent a "stuck" button state
            WritePlayerInputImmediate(1, 0);
        }
    }

    // P2 Queue Logic
    if (p2QueueActive) {
        if (p2QueueIndex < p2InputQueue.size()) {
            p2FrameCounter++;
            if (p2FrameCounter >= p2InputQueue[p2QueueIndex].durationFrames) {
                p2FrameCounter = 0;
                p2QueueIndex++;
            }
        } else {
            // Queue is finished
            p2QueueActive = false;
            p2InputQueue.clear();
            p2QueueIndex = 0;
            // Write a neutral input to prevent a "stuck" button state
            WritePlayerInputImmediate(2, 0);
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

// Add this diagnostic function to verify the input system
void DiagnoseInputSystem(int playerNum) {
    uintptr_t playerPtr = GetPlayerPointer(playerNum);
    if (!playerPtr) {
        LogOut("[INPUT_DIAG] Cannot get player pointer", true);
        return;
    }
    
    LogOut("[INPUT_DIAG] === Player " + std::to_string(playerNum) + " Input Diagnosis ===", true);
    LogOut("[INPUT_DIAG] Player base pointer: 0x" + std::to_string(playerPtr), true);
    
    // Read and display immediate input values
    uint8_t horizontal = 0, vertical = 0;
    uint8_t btnA = 0, btnB = 0, btnC = 0, btnD = 0;
    
    SafeReadMemory(playerPtr + INPUT_HORIZONTAL_OFFSET, &horizontal, 1);
    SafeReadMemory(playerPtr + INPUT_VERTICAL_OFFSET, &vertical, 1);
    SafeReadMemory(playerPtr + INPUT_BUTTON_A_OFFSET, &btnA, 1);
    SafeReadMemory(playerPtr + INPUT_BUTTON_B_OFFSET, &btnB, 1);
    SafeReadMemory(playerPtr + INPUT_BUTTON_C_OFFSET, &btnC, 1);
    SafeReadMemory(playerPtr + INPUT_BUTTON_D_OFFSET, &btnD, 1);
    
    LogOut("[INPUT_DIAG] Immediate inputs:", true);
    LogOut("[INPUT_DIAG]   Horizontal (0x188): " + std::to_string((int)horizontal) + 
           " (" + (horizontal == 1 ? "RIGHT" : horizontal == 255 ? "LEFT" : "NEUTRAL") + ")", true);
    LogOut("[INPUT_DIAG]   Vertical (0x189): " + std::to_string((int)vertical) + 
           " (" + (vertical == 1 ? "DOWN" : vertical == 255 ? "UP" : "NEUTRAL") + ")", true);
    LogOut("[INPUT_DIAG]   Button A (0x190): " + std::to_string((int)btnA), true);
    LogOut("[INPUT_DIAG]   Button B (0x194): " + std::to_string((int)btnB), true);
    LogOut("[INPUT_DIAG]   Button C (0x198): " + std::to_string((int)btnC), true);
    LogOut("[INPUT_DIAG]   Button D (0x19C): " + std::to_string((int)btnD), true);
    
    // Read buffer info
    uint16_t bufferIndex = 0;
    SafeReadMemory(playerPtr + INPUT_BUFFER_INDEX_OFFSET, &bufferIndex, 2);
    LogOut("[INPUT_DIAG] Buffer index (0x260): " + std::to_string(bufferIndex), true);
    
    // Read last few buffer entries
    LogOut("[INPUT_DIAG] Last 5 buffer entries:", true);
    for (int i = 0; i < 5; i++) {
        int idx = (bufferIndex - i + INPUT_BUFFER_SIZE) % INPUT_BUFFER_SIZE;
        uint8_t bufValue = 0;
        SafeReadMemory(playerPtr + INPUT_BUFFER_OFFSET + idx, &bufValue, 1);
        LogOut("[INPUT_DIAG]   [" + std::to_string(idx) + "]: 0x" + 
               std::to_string(bufValue) + " = " + DecodeInputMask(bufValue), true);
    }
    
    LogOut("[INPUT_DIAG] ==================", true);
}

// REVISED: Fully implement QueueMotionInput
bool QueueMotionInput(int playerNum, int motionType, int buttonMask) {
    // New: Use InputFrame with separate direction and button fields
    std::vector<InputFrame> motionSequence;
    const int DIR_FRAMES = 2;
    const int BTN_FRAMES = 3;
    const int NEUTRAL_FRAMES = 2;

    auto addInput = [&](uint8_t dirMask, uint8_t btnMask, int frames) {
        motionSequence.push_back(InputFrame{static_cast<uint8_t>(dirMask | btnMask), frames});
    };

    switch (motionType) {
        case MOTION_NONE:
            if (buttonMask != 0) {
                // If only a direction is pressed (no button)
                if (buttonMask == GAME_INPUT_RIGHT) {
                    addInput(GAME_INPUT_RIGHT, 0, DIR_FRAMES);
                } else if (buttonMask == GAME_INPUT_DOWN) {
                    addInput(GAME_INPUT_DOWN, 0, DIR_FRAMES);
                } else if (buttonMask == GAME_INPUT_LEFT) {
                    addInput(GAME_INPUT_LEFT, 0, DIR_FRAMES);
                } else if (buttonMask == GAME_INPUT_UP) {
                    addInput(GAME_INPUT_UP, 0, DIR_FRAMES);
                } else if (buttonMask == GAME_INPUT_A || buttonMask == GAME_INPUT_B || buttonMask == GAME_INPUT_C || buttonMask == GAME_INPUT_D) {
                    addInput(0, buttonMask, BTN_FRAMES);
                } else if ((buttonMask & (GAME_INPUT_RIGHT | GAME_INPUT_LEFT | GAME_INPUT_UP | GAME_INPUT_DOWN)) && (buttonMask & (GAME_INPUT_A | GAME_INPUT_B | GAME_INPUT_C | GAME_INPUT_D))) {
                    // Direction + button
                    uint8_t dir = buttonMask & (GAME_INPUT_RIGHT | GAME_INPUT_LEFT | GAME_INPUT_UP | GAME_INPUT_DOWN);
                    uint8_t btn = buttonMask & (GAME_INPUT_A | GAME_INPUT_B | GAME_INPUT_C | GAME_INPUT_D);
                    addInput(dir, btn, BTN_FRAMES);
                } else {
                    addInput(0, 0, DIR_FRAMES);
                }
            } else {
                addInput(0, 0, DIR_FRAMES);
            }
            break;
        case ACTION_FORWARD_DASH:
            addInput(GAME_INPUT_RIGHT, 0, DIR_FRAMES);
            addInput(0, 0, NEUTRAL_FRAMES);
            addInput(GAME_INPUT_RIGHT, 0, DIR_FRAMES);
            break;
        case ACTION_BACK_DASH:
            addInput(GAME_INPUT_LEFT, 0, DIR_FRAMES);
            addInput(0, 0, NEUTRAL_FRAMES);
            addInput(GAME_INPUT_LEFT, 0, DIR_FRAMES);
            break;
        case MOTION_236A: case MOTION_236B: case MOTION_236C:
            addInput(GAME_INPUT_DOWN, 0, DIR_FRAMES);
            addInput(GAME_INPUT_DOWN | GAME_INPUT_RIGHT, 0, DIR_FRAMES);
            addInput(GAME_INPUT_RIGHT, buttonMask, BTN_FRAMES);
            break;
        case MOTION_623A: case MOTION_623B: case MOTION_623C:
            addInput(GAME_INPUT_RIGHT, 0, DIR_FRAMES);
            addInput(0, 0, NEUTRAL_FRAMES);
            addInput(GAME_INPUT_DOWN, 0, DIR_FRAMES);
            addInput(GAME_INPUT_DOWN | GAME_INPUT_RIGHT, buttonMask, BTN_FRAMES);
            break;
        case MOTION_214A: case MOTION_214B: case MOTION_214C:
            addInput(GAME_INPUT_DOWN, 0, DIR_FRAMES);
            addInput(GAME_INPUT_DOWN | GAME_INPUT_LEFT, 0, DIR_FRAMES);
            addInput(GAME_INPUT_LEFT, buttonMask, BTN_FRAMES);
            break;
        case MOTION_5A: case MOTION_5B: case MOTION_5C:
            addInput(0, buttonMask, BTN_FRAMES);
            break;
        case MOTION_2A: case MOTION_2B: case MOTION_2C:
            addInput(GAME_INPUT_DOWN, buttonMask, BTN_FRAMES);
            break;
        case MOTION_JA: case MOTION_JB: case MOTION_JC:
            addInput(GAME_INPUT_UP, buttonMask, BTN_FRAMES);
            break;
        default:
            if ((motionType & (GAME_INPUT_UP | GAME_INPUT_DOWN | GAME_INPUT_LEFT | GAME_INPUT_RIGHT)) ||
                (motionType & (GAME_INPUT_A | GAME_INPUT_B | GAME_INPUT_C | GAME_INPUT_D))) {
                uint8_t dir = motionType & (GAME_INPUT_UP | GAME_INPUT_DOWN | GAME_INPUT_LEFT | GAME_INPUT_RIGHT);
                uint8_t btn = buttonMask & (GAME_INPUT_A | GAME_INPUT_B | GAME_INPUT_C | GAME_INPUT_D);
                addInput(dir, btn, BTN_FRAMES);
            } else {
                LogOut("[INPUT_MOTION] QueueMotionInput: Unknown motionType " + std::to_string(motionType), true);
                return false;
            }
            break;
    }

    // Add a final neutral frame
    addInput(0, 0, NEUTRAL_FRAMES);

    if (playerNum == 1) {
        p1InputQueue = motionSequence;
        p1QueueIndex = 0;
        p1FrameCounter = 0;
        p1QueueActive = true;
    } else if (playerNum == 2) {
        p2InputQueue = motionSequence;
        p2QueueIndex = 0;
        p2FrameCounter = 0;
        p2QueueActive = true;
    } else {
        return false;
    }

    LogOut("[INPUT_MOTION] Queued motion " + GetMotionTypeName(motionType) + " for P" + std::to_string(playerNum), detailedLogging.load());
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
    // Example mapping, adjust as needed for your game
    switch (actionType) {
        case 0: return MOTION_5A;
        case 1: return MOTION_5B;
        case 2: return MOTION_5C;
        case 3: return MOTION_2A;
        case 4: return MOTION_2B;
        case 5: return MOTION_2C;
        // Add more as needed
        default: return MOTION_NONE;
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

void DebugCurrentInputState(int playerNum) {
    // ... implementation for debugging ...
}

/*std::string DecodeInputMask(uint8_t inputMask) {/
    std::string result;
    if (inputMask & MOTION_INPUT_UP) result += "U";
    if (inputMask & MOTION_INPUT_DOWN) result += "D";
    if (inputMask & MOTION_INPUT_LEFT) result += "L";
    if (inputMask & MOTION_INPUT_RIGHT) result += "R";
    if (inputMask & MOTION_BUTTON_A) result += "A";
    if (inputMask & MOTION_BUTTON_B) result += "B";
    if (inputMask & MOTION_BUTTON_C) result += "C";
    if (inputMask & MOTION_BUTTON_D) result += "D";
    if (result.empty()) result = "N";
    return result;
}*/