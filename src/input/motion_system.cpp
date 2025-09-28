#include "../include/input/motion_system.h"
#include "../include/input/input_core.h"
#include "../include/core/memory.h"
#include "../include/core/logger.h"
#include "../include/core/constants.h"
#include "../include/game/auto_action_helpers.h"
#include "../include/input/motion_constants.h"  // Add this include
#include <vector>
#include <sstream>
#include <algorithm>

// Global variables for motion input system
std::vector<InputFrame> p1InputQueue;
std::vector<InputFrame> p2InputQueue;
int p1QueueIndex = 0;
int p2QueueIndex = 0;
int p1FrameCounter = 0;
int p2FrameCounter = 0;
bool p1QueueActive = false;
bool p2QueueActive = false;

// Returns the button mask for a given motion type (used for input queueing)
uint8_t DetermineButtonFromMotionType(int motionType) {
    switch (motionType) {
        case MOTION_5A: case MOTION_2A: case MOTION_JA: 
        case MOTION_236A: case MOTION_623A: case MOTION_214A:
        case MOTION_421A: case MOTION_41236A:
    case MOTION_236236A: case MOTION_214214A: case MOTION_641236A:
        case MOTION_412A: case MOTION_22A: case MOTION_214236A:
        case MOTION_463214A: case MOTION_4123641236A: case MOTION_6321463214A:
            return GAME_INPUT_A;
            
        case MOTION_5B: case MOTION_2B: case MOTION_JB: 
        case MOTION_236B: case MOTION_623B: case MOTION_214B:
        case MOTION_421B: case MOTION_41236B:
    case MOTION_236236B: case MOTION_214214B: case MOTION_641236B:
        case MOTION_412B: case MOTION_22B: case MOTION_214236B:
        case MOTION_463214B: case MOTION_4123641236B: case MOTION_6321463214B:
            return GAME_INPUT_B;
            
        case MOTION_5C: case MOTION_2C: case MOTION_JC: 
        case MOTION_236C: case MOTION_623C: case MOTION_214C:
        case MOTION_421C: case MOTION_41236C:
    case MOTION_236236C: case MOTION_214214C: case MOTION_641236C:
        case MOTION_412C: case MOTION_22C: case MOTION_214236C:
        case MOTION_463214C: case MOTION_4123641236C: case MOTION_6321463214C:
            return GAME_INPUT_C;
            
        default:
            return GAME_INPUT_A;  // Default to A button
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
        case MOTION_236236A: return "236236A";
        case MOTION_236236B: return "236236B";
        case MOTION_236236C: return "236236C";
        case MOTION_214214A: return "214214A";
        case MOTION_214214B: return "214214B";
        case MOTION_214214C: return "214214C";
        case MOTION_641236A: return "641236A";
        case MOTION_641236B: return "641236B";
        case MOTION_641236C: return "641236C";
        case MOTION_412A: return "412A";
        case MOTION_412B: return "412B";
        case MOTION_412C: return "412C";
        case MOTION_22A: return "22A";
        case MOTION_22B: return "22B";
        case MOTION_22C: return "22C";
        case MOTION_214236A: return "214236A";
        case MOTION_214236B: return "214236B";
        case MOTION_214236C: return "214236C";
        case MOTION_463214A: return "463214A";
        case MOTION_463214B: return "463214B";
        case MOTION_463214C: return "463214C";
        case MOTION_4123641236A: return "4123641236A";
        case MOTION_4123641236B: return "4123641236B";
        case MOTION_4123641236C: return "4123641236C";
    case MOTION_6321463214A: return "6321463214A";
    case MOTION_6321463214B: return "6321463214B";
    case MOTION_6321463214C: return "6321463214C";
    case MOTION_FORWARD_DASH: return "Forward Dash";
    case MOTION_BACK_DASH: return "Back Dash";
        default: return "Unknown";
    }
}

// Helper function to explicitly cast integers to uint8_t to avoid narrowing conversion warnings
inline uint8_t u8(int value) {
    return static_cast<uint8_t>(value);
}

// Update ProcessInputQueues to only manage state, not write inputs
void ProcessInputQueues() {
    // Process P1 queue
    if (p1QueueActive) {
        if (p1QueueIndex >= 0 && (size_t)p1QueueIndex < p1InputQueue.size()) {
            if (++p1FrameCounter >= p1InputQueue[p1QueueIndex].durationFrames) {
                p1FrameCounter = 0;
                p1QueueIndex++;
                
                if (p1QueueIndex >= (int)p1InputQueue.size()) {
                    p1QueueActive = false;
                    p1QueueIndex = 0;
                    LogOut("[INPUT_QUEUE] P1 queue completed", true);
                }
            }
        } else {
            p1QueueActive = false;
            LogOut("[INPUT_QUEUE] P1 queue deactivated (invalid index)", true);
        }
    }
    
    // Process P2 queue
    if (p2QueueActive) {
        if (p2QueueIndex >= 0 && (size_t)p2QueueIndex < p2InputQueue.size()) {
            if (++p2FrameCounter >= p2InputQueue[p2QueueIndex].durationFrames) {
                p2FrameCounter = 0;
                p2QueueIndex++;
                
                if (p2QueueIndex >= (int)p2InputQueue.size()) {
                    p2QueueActive = false;
                    p2QueueIndex = 0;
                    LogOut("[INPUT_QUEUE] P2 queue completed", true);
                }
            }
        } else {
            p2QueueActive = false;
            LogOut("[INPUT_QUEUE] P2 queue deactivated (invalid index)", true);
        }
    }
}

// Queues a motion input for the specified player
bool QueueMotionInput(int playerNum, int motionType, int buttonMask) {
    if (playerNum < 1 || playerNum > 2) return false;
    
    // Get the player's facing direction
    bool facingRight = GetPlayerFacingDirection(playerNum);
    LogOut("[INPUT_MOTION] Player " + std::to_string(playerNum) + " is facing " + 
           (facingRight ? "right" : "left"), true);
    
    // Clear any existing input queue for this player
    std::vector<InputFrame>& queue = (playerNum == 1) ? p1InputQueue : p2InputQueue;
    queue.clear();

    // If caller passed 0, infer button from motion type (prevents accidental no-button sequences)
    if (buttonMask == 0) {
        buttonMask = DetermineButtonFromMotionType(motionType);
    }
    
    // Frame timing constants
    const int DIR_FRAMES = 3;     // Duration for directional inputs
    const int BTN_FRAMES = 6;     // Duration for button press
    const int NEUTRAL_FRAMES = 2; // Duration for neutral inputs between directions
    
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
            queue.push_back({static_cast<uint8_t>(adjustedDirMask | btnMask), 1});
    };

    // Build the input sequence based on motion type
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
            
        // (63214*) single half-circle back removed per updated requirements

        case MOTION_236236A: case MOTION_236236B: case MOTION_236236C:
            // Double QCF: (Down, Down-Forward, Forward) x2 + Button on final Forward
            addInput(GAME_INPUT_DOWN, 0, DIR_FRAMES);
            addInput(GAME_INPUT_DOWN | GAME_INPUT_RIGHT, 0, DIR_FRAMES);
            addInput(GAME_INPUT_RIGHT, 0, DIR_FRAMES);
            addInput(GAME_INPUT_DOWN, 0, DIR_FRAMES);
            addInput(GAME_INPUT_DOWN | GAME_INPUT_RIGHT, 0, DIR_FRAMES);
            addInput(GAME_INPUT_RIGHT, buttonMask, BTN_FRAMES);
            break;

        case MOTION_214214A: case MOTION_214214B: case MOTION_214214C:
            // Double QCB: (Down, Down-Back, Back) x2 + Button on final Back
            addInput(GAME_INPUT_DOWN, 0, DIR_FRAMES);
            addInput(GAME_INPUT_DOWN | GAME_INPUT_LEFT, 0, DIR_FRAMES);
            addInput(GAME_INPUT_LEFT, 0, DIR_FRAMES);
            addInput(GAME_INPUT_DOWN, 0, DIR_FRAMES);
            addInput(GAME_INPUT_DOWN | GAME_INPUT_LEFT, 0, DIR_FRAMES);
            addInput(GAME_INPUT_LEFT, buttonMask, BTN_FRAMES);
            break;

        case MOTION_641236A: case MOTION_641236B: case MOTION_641236C:
            // 641236 (pretzel-like) : Forward, Back, Down-Back, Down, Down-Forward, Forward + Button
            addInput(GAME_INPUT_RIGHT, 0, DIR_FRAMES);          // 6
            addInput(GAME_INPUT_LEFT, 0, DIR_FRAMES);           // 4
            addInput(GAME_INPUT_DOWN | GAME_INPUT_LEFT, 0, DIR_FRAMES);  // 1
            addInput(GAME_INPUT_DOWN, 0, DIR_FRAMES);           // 2
            addInput(GAME_INPUT_DOWN | GAME_INPUT_RIGHT, 0, DIR_FRAMES); // 3
            addInput(GAME_INPUT_RIGHT, buttonMask, BTN_FRAMES); // 6 + button
            break;

        case MOTION_412A: case MOTION_412B: case MOTION_412C:
            // 412: Back, Down-Back, Down + Button
            addInput(GAME_INPUT_LEFT, 0, DIR_FRAMES);                // 4
            addInput(GAME_INPUT_DOWN | GAME_INPUT_LEFT, 0, DIR_FRAMES); // 1
            addInput(GAME_INPUT_DOWN, buttonMask, BTN_FRAMES);       // 2 + button
            break;

        case MOTION_22A: case MOTION_22B: case MOTION_22C:
            // 22: Down, (neutral optional), Down + Button
            addInput(GAME_INPUT_DOWN, 0, DIR_FRAMES);                // 2
            addInput(0, 0, NEUTRAL_FRAMES);                         // small neutral (can tweak)
            addInput(GAME_INPUT_DOWN, buttonMask, BTN_FRAMES);       // 2 + button
            break;

        case MOTION_214236A: case MOTION_214236B: case MOTION_214236C:
            // 214236: QCB then QCF (Down, Down-Back, Back, Down, Down-Forward, Forward + Button)
            addInput(GAME_INPUT_DOWN, 0, DIR_FRAMES);                // 2
            addInput(GAME_INPUT_DOWN | GAME_INPUT_LEFT, 0, DIR_FRAMES); // 1
            addInput(GAME_INPUT_LEFT, 0, DIR_FRAMES);                // 4
            addInput(GAME_INPUT_DOWN, 0, DIR_FRAMES);                // 2
            addInput(GAME_INPUT_DOWN | GAME_INPUT_RIGHT, 0, DIR_FRAMES); // 3
            addInput(GAME_INPUT_RIGHT, buttonMask, BTN_FRAMES);      // 6 + button
            break;

        case MOTION_463214A: case MOTION_463214B: case MOTION_463214C:
            // 463214: Left, Right, Down-Right, Down, Down-Left, Left + Button
            addInput(GAME_INPUT_LEFT, 0, DIR_FRAMES);                // 4
            addInput(GAME_INPUT_RIGHT, 0, DIR_FRAMES);               // 6
            addInput(GAME_INPUT_DOWN | GAME_INPUT_RIGHT, 0, DIR_FRAMES); // 3
            addInput(GAME_INPUT_DOWN, 0, DIR_FRAMES);                // 2
            addInput(GAME_INPUT_DOWN | GAME_INPUT_LEFT, 0, DIR_FRAMES);  // 1
            addInput(GAME_INPUT_LEFT, buttonMask, BTN_FRAMES);       // 4 + button
            break;

        case MOTION_4123641236A: case MOTION_4123641236B: case MOTION_4123641236C:
            // 4123641236: (41236) x2 without button until final Forward
            // First 41236
            addInput(GAME_INPUT_LEFT, 0, DIR_FRAMES);                // 4
            addInput(GAME_INPUT_DOWN | GAME_INPUT_LEFT, 0, DIR_FRAMES); // 1
            addInput(GAME_INPUT_DOWN, 0, DIR_FRAMES);                // 2
            addInput(GAME_INPUT_DOWN | GAME_INPUT_RIGHT, 0, DIR_FRAMES); // 3
            addInput(GAME_INPUT_RIGHT, 0, DIR_FRAMES);               // 6
            // Second 41236 with button at end
            addInput(GAME_INPUT_LEFT, 0, DIR_FRAMES);                // 4
            addInput(GAME_INPUT_DOWN | GAME_INPUT_LEFT, 0, DIR_FRAMES); // 1
            addInput(GAME_INPUT_DOWN, 0, DIR_FRAMES);                // 2
            addInput(GAME_INPUT_DOWN | GAME_INPUT_RIGHT, 0, DIR_FRAMES); // 3
            addInput(GAME_INPUT_RIGHT, buttonMask, BTN_FRAMES);      // 6 + button
            break;

        case MOTION_6321463214A: case MOTION_6321463214B: case MOTION_6321463214C:
            // 6321463214: 6,3,2,1,4,6,3,2,1,4 + Button
            addInput(GAME_INPUT_RIGHT, 0, DIR_FRAMES);               // 6
            addInput(GAME_INPUT_DOWN | GAME_INPUT_RIGHT, 0, DIR_FRAMES); // 3
            addInput(GAME_INPUT_DOWN, 0, DIR_FRAMES);                // 2
            addInput(GAME_INPUT_DOWN | GAME_INPUT_LEFT, 0, DIR_FRAMES); // 1
            addInput(GAME_INPUT_LEFT, 0, DIR_FRAMES);                // 4
            addInput(GAME_INPUT_RIGHT, 0, DIR_FRAMES);               // 6
            addInput(GAME_INPUT_DOWN | GAME_INPUT_RIGHT, 0, DIR_FRAMES); // 3
            addInput(GAME_INPUT_DOWN, 0, DIR_FRAMES);                // 2
            addInput(GAME_INPUT_DOWN | GAME_INPUT_LEFT, 0, DIR_FRAMES); // 1
            addInput(GAME_INPUT_LEFT, buttonMask, BTN_FRAMES);       // 4 + button
            break;
            
        case MOTION_421A: case MOTION_421B: case MOTION_421C:
            // Down, Down-Back, Back + Button
            addInput(GAME_INPUT_DOWN, 0, DIR_FRAMES);
            addInput(GAME_INPUT_DOWN | GAME_INPUT_LEFT, 0, DIR_FRAMES);
            addInput(GAME_INPUT_LEFT, buttonMask, BTN_FRAMES);
            break;
            
        case MOTION_FORWARD_DASH:
            // Forward, Neutral, Forward
            addInput(GAME_INPUT_RIGHT, 0, DIR_FRAMES);
            addInput(0, 0, NEUTRAL_FRAMES);
            addInput(GAME_INPUT_RIGHT, 0, DIR_FRAMES);
            break;
            
        case MOTION_BACK_DASH:
            // Back, Neutral, Back
            addInput(GAME_INPUT_LEFT, 0, DIR_FRAMES);
            addInput(0, 0, NEUTRAL_FRAMES);
            addInput(GAME_INPUT_LEFT, 0, DIR_FRAMES);
            break;
            
        case MOTION_5A: case MOTION_5B: case MOTION_5C:
            // Standing normals: just press the button
            addInput(0, buttonMask, BTN_FRAMES);
            break;
            
        case MOTION_2A: case MOTION_2B: case MOTION_2C:
            // Crouching normals: hold down + button
            addInput(GAME_INPUT_DOWN, buttonMask, BTN_FRAMES);
            break;
            
        case MOTION_JA: case MOTION_JB: case MOTION_JC:
            // Jumping normals: hold up + button
            addInput(GAME_INPUT_UP, buttonMask, BTN_FRAMES);
            break;
            
        default:
            LogOut("[INPUT_MOTION] WARNING: Unknown motion type " + std::to_string(motionType), true);
            return false;
    }

    // Activate the queue for this player
    if (playerNum == 1) {
        p1QueueActive = true;
        p1QueueIndex = 0;
        p1FrameCounter = 0;
    } else {
        p2QueueActive = true;
        p2QueueIndex = 0;
        p2FrameCounter = 0;
    }

    LogOut("[INPUT_MOTION] Queued motion " + GetMotionTypeName(motionType) + 
           " for P" + std::to_string(playerNum) + " with " + std::to_string(queue.size()) + 
           " inputs", true);
    
    return true;
}

// Wrapper for auto action system
int ConvertActionToMotion(int actionType, int triggerType) {
    return ConvertTriggerActionToMotion(actionType, triggerType);
}