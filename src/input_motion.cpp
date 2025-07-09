#include "../include/input_motion.h"
#include "../include/memory.h"
#include "../include/logger.h"
#include "../include/constants.h"
#include "../include/utilities.h"
#include <vector>
#include <sstream>
#include <cstdlib>
#include <ctime>

// Global variables for motion input system
std::vector<InputFrame> p1InputQueue;
std::vector<InputFrame> p2InputQueue;
int p1QueueIndex = 0;
int p2QueueIndex = 0;
int p1FrameCounter = 0;
int p2FrameCounter = 0;
bool p1QueueActive = false;
bool p2QueueActive = false;

// Button constants for cleaner code
#define BUTTON_A    0x10  // INPUT_A = 0x10
#define BUTTON_B    0x20  // INPUT_B = 0x20
#define BUTTON_C    0x40  // INPUT_C = 0x40

// Enhanced debugging for motion input sequences
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
        case MOTION_236A: return "QCF+A";
        case MOTION_236B: return "QCF+B";
        case MOTION_236C: return "QCF+C";
        case MOTION_623A: return "DP+A";
        case MOTION_623B: return "DP+B";
        case MOTION_623C: return "DP+C";
        case MOTION_214A: return "QCB+A";
        case MOTION_214B: return "QCB+B";
        case MOTION_214C: return "QCB+C";
        case MOTION_41236A: return "HCF+A";
        case MOTION_41236B: return "HCF+B";
        case MOTION_41236C: return "HCF+C";
        case MOTION_63214A: return "HCB+A";
        case MOTION_63214B: return "HCB+B";
        case MOTION_63214C: return "HCB+C";
        default: return "Unknown (" + std::to_string(motionType) + ")";
    }
}

// Function to queue a motion input for automatic execution
bool QueueMotionInput(int playerNum, int motionType, int buttonMask) {
    // Convert decimal button values to actual bit masks if needed
    if (buttonMask == BUTTON_A || buttonMask == 16) buttonMask = 0x10;
    if (buttonMask == BUTTON_B || buttonMask == 32) buttonMask = 0x20; 
    if (buttonMask == BUTTON_C || buttonMask == 64) buttonMask = 0x40;

    LogOut("[INPUT_MOTION] QueueMotionInput called: Player=" + std::to_string(playerNum) + 
           ", motion=" + GetMotionTypeName(motionType) + 
           ", buttonMask=0x" + std::to_string(buttonMask), true);
    
    // Abort if a queue is already active for this player
    if ((playerNum == 1 && p1QueueActive) || (playerNum == 2 && p2QueueActive)) {
        LogOut("[INPUT_MOTION] Cannot queue new motion - player " + 
               std::to_string(playerNum) + " already has an active queue", 
               detailedLogging.load());
        return false;
    }
    
    // Skip if motionType is invalid
    if (motionType <= MOTION_NONE) {
        LogOut("[INPUT_MOTION] Invalid motion type: " + std::to_string(motionType), true);
        return false;
    }
    
    // Convert button mask to a readable string for logging
    std::string buttonStr;
    if (buttonMask & BUTTON_A) buttonStr += "A";
    if (buttonMask & BUTTON_B) buttonStr += "B";
    if (buttonMask & BUTTON_C) buttonStr += "C";
    if (buttonStr.empty()) buttonStr = "None";
    
    LogOut("[INPUT_MOTION] Queueing motion " + std::to_string(motionType) + 
           " with button(s) " + buttonStr + " for P" + std::to_string(playerNum), true);
    
    std::vector<InputFrame> sequence;
    
    // Create appropriate sequence based on motion type
    switch (motionType) {
        case MOTION_5A:
        case MOTION_5B:
        case MOTION_5C:
            // Simple button press with neutral direction (5)
            sequence.clear();
            sequence.push_back(InputFrame(buttonMask, 4));           // Press button for 4 frames
            sequence.push_back(InputFrame(0, 2));                    // Release all inputs
            break;
            
        case MOTION_2A:
        case MOTION_2B:
        case MOTION_2C:
            // Crouching button press (2 + button)
            sequence.clear();
            sequence.push_back(InputFrame(INPUT_DOWN, 2));                   // Hold down for 2 frames
            sequence.push_back(InputFrame(INPUT_DOWN | buttonMask, 4));      // Press button while holding down
            sequence.push_back(InputFrame(0, 2));                            // Release all inputs
            break;
            
        case MOTION_236A:
        case MOTION_236B:
        case MOTION_236C:
            // QCF (↓↘→) + button
            sequence.clear();
            sequence.push_back(InputFrame(INPUT_DOWN, 3));                           // Down
            sequence.push_back(InputFrame(INPUT_DOWN | INPUT_RIGHT, 3));             // Down-Right
            sequence.push_back(InputFrame(INPUT_RIGHT, 3));                          // Right
            sequence.push_back(InputFrame(INPUT_RIGHT | buttonMask, 4));             // Right + Button
            sequence.push_back(InputFrame(0, 2));                                    // Release
            break;
            
        case MOTION_623A:
        case MOTION_623B:
        case MOTION_623C:
            // DP (→↓↘) + button
            sequence.clear();
            sequence.push_back(InputFrame(INPUT_RIGHT, 3));                          // Right
            sequence.push_back(InputFrame(INPUT_DOWN, 3));                           // Down
            sequence.push_back(InputFrame(INPUT_DOWN | INPUT_RIGHT, 3));             // Down-Right
            sequence.push_back(InputFrame(INPUT_DOWN | INPUT_RIGHT | buttonMask, 4));// Down-Right + Button
            sequence.push_back(InputFrame(0, 2));                                    // Release
            break;
            
        case MOTION_214A:
        case MOTION_214B:
        case MOTION_214C:
            // QCB (↓↙←) + button
            sequence.clear();
            sequence.push_back(InputFrame(INPUT_DOWN, 3));                           // Down
            sequence.push_back(InputFrame(INPUT_DOWN | INPUT_LEFT, 3));              // Down-Left
            sequence.push_back(InputFrame(INPUT_LEFT, 3));                           // Left
            sequence.push_back(InputFrame(INPUT_LEFT | buttonMask, 4));              // Left + Button
            sequence.push_back(InputFrame(0, 2));                                    // Release
            break;
            
        case MOTION_421A:
        case MOTION_421B:
        case MOTION_421C:
            // RDP (←↓↙) + button
            sequence.clear();
            sequence.push_back(InputFrame(INPUT_LEFT, 3));                           // Left
            sequence.push_back(InputFrame(INPUT_DOWN, 3));                           // Down
            sequence.push_back(InputFrame(INPUT_DOWN | INPUT_LEFT, 3));              // Down-Left
            sequence.push_back(InputFrame(INPUT_DOWN | INPUT_LEFT | buttonMask, 4)); // Down-Left + Button
            sequence.push_back(InputFrame(0, 2));                                    // Release
            break;
            
        case MOTION_41236A:
        case MOTION_41236B:
        case MOTION_41236C:
            // HCF (↙↓↘→) + button
            sequence.clear();
            sequence.push_back(InputFrame(INPUT_LEFT, 2));                           // Left
            sequence.push_back(InputFrame(INPUT_DOWN | INPUT_LEFT, 2));              // Down-Left
            sequence.push_back(InputFrame(INPUT_DOWN, 2));                           // Down
            sequence.push_back(InputFrame(INPUT_DOWN | INPUT_RIGHT, 2));             // Down-Right
            sequence.push_back(InputFrame(INPUT_RIGHT, 2));                          // Right
            sequence.push_back(InputFrame(INPUT_RIGHT | buttonMask, 4));             // Right + Button
            sequence.push_back(InputFrame(0, 2));                                    // Release
            break;
            
        case MOTION_63214A:
        case MOTION_63214B:
        case MOTION_63214C:
            // HCB (→↘↓↙←) + button
            sequence.clear();
            sequence.push_back(InputFrame(INPUT_RIGHT, 2));                          // Right
            sequence.push_back(InputFrame(INPUT_DOWN | INPUT_RIGHT, 2));             // Down-Right
            sequence.push_back(InputFrame(INPUT_DOWN, 2));                           // Down
            sequence.push_back(InputFrame(INPUT_DOWN | INPUT_LEFT, 2));              // Down-Left
            sequence.push_back(InputFrame(INPUT_LEFT, 2));                           // Left
            sequence.push_back(InputFrame(INPUT_LEFT | buttonMask, 4));              // Left + Button
            sequence.push_back(InputFrame(0, 2));                                    // Release
            break;
            
        case MOTION_JA:
        case MOTION_JB:
        case MOTION_JC:
            // Jumping attack
            sequence.clear();
            sequence.push_back(InputFrame(buttonMask, 4));                           // Press button for 4 frames
            sequence.push_back(InputFrame(0, 2));                                    // Release all inputs
            break;
            
        default:
            // Unknown motion type, use default 5A
            LogOut("[INPUT_MOTION] Unknown motion type " + std::to_string(motionType) + 
                   ", using default 5A motion", true);
            sequence.clear();
            sequence.push_back(InputFrame(buttonMask, 4));
            sequence.push_back(InputFrame(0, 2));
            break;
    }
    
    // Log the sequence details
    std::stringstream sequenceStr;
    sequenceStr << "[INPUT_MOTION] Created sequence with " << sequence.size() << " frames: ";
    for (size_t i = 0; i < sequence.size(); i++) {
        sequenceStr << "0x" << std::hex << (int)sequence[i].inputMask 
                    << "(" << std::dec << sequence[i].durationFrames << "f)";
        if (i < sequence.size() - 1) sequenceStr << " → ";
    }
    LogOut(sequenceStr.str(), detailedLogging.load());
    
    // Assign the sequence to the appropriate player
    if (playerNum == 1) {
        p1InputQueue = sequence;
        p1QueueIndex = 0;
        p1FrameCounter = 0;
        p1QueueActive = true;
        LogOut("[INPUT_MOTION] Motion sequence activated for P1", true);
    } else {
        p2InputQueue = sequence;
        p2QueueIndex = 0;
        p2FrameCounter = 0;
        p2QueueActive = true;
        LogOut("[INPUT_MOTION] Motion sequence activated for P2", true);
    }
    
    return true;
}

// Function to process queued inputs each frame
void ProcessInputQueues() {
    // Process P1 queue
    if (p1QueueActive && !p1InputQueue.empty()) {
        if (p1QueueIndex < p1InputQueue.size()) {
            // Write current input to memory
            bool result = WritePlayerInput(1, p1InputQueue[p1QueueIndex].inputMask);
            
            // Log the input write - only if detailed logging is enabled
            if (detailedLogging.load()) {
                LogOut("[INPUT_MOTION] P1 Input: 0x" + 
                       std::to_string(p1InputQueue[p1QueueIndex].inputMask) + 
                       " (frame " + std::to_string(p1FrameCounter + 1) + "/" + 
                       std::to_string(p1InputQueue[p1QueueIndex].durationFrames) + 
                       "), success=" + std::to_string(result), false);
            }
            
            // Update frame counter
            p1FrameCounter++;
            
            // Check if we need to advance to next input
            if (p1FrameCounter >= p1InputQueue[p1QueueIndex].durationFrames) {
                p1QueueIndex++;
                p1FrameCounter = 0;
                
                // Log transition to next input
                if (p1QueueIndex < p1InputQueue.size() && detailedLogging.load()) {
                    LogOut("[INPUT_MOTION] P1 Advancing to next input: 0x" + 
                           std::to_string(p1InputQueue[p1QueueIndex].inputMask), false);
                }
            }
        } else {
            // Queue is finished
            p1QueueActive = false;
            WritePlayerInput(1, 0); // Clear inputs
            LogOut("[INPUT_MOTION] P1 Motion sequence complete", detailedLogging.load());
        }
    }
    
    // Process P2 queue
    if (p2QueueActive && !p2InputQueue.empty()) {
        if (p2QueueIndex < p2InputQueue.size()) {
            // Write current input to memory
            bool result = WritePlayerInput(2, p2InputQueue[p2QueueIndex].inputMask);
            
            // Log the input write - only if detailed logging is enabled
            if (detailedLogging.load()) {
                LogOut("[INPUT_MOTION] P2 Input: 0x" + 
                       std::to_string(p2InputQueue[p2QueueIndex].inputMask) + 
                       " (frame " + std::to_string(p2FrameCounter + 1) + "/" + 
                       std::to_string(p2InputQueue[p2QueueIndex].durationFrames) + 
                       "), success=" + std::to_string(result), false);
            }
            
            // Update frame counter
            p2FrameCounter++;
            
            // Check if we need to advance to next input
            if (p2FrameCounter >= p2InputQueue[p2QueueIndex].durationFrames) {
                p2QueueIndex++;
                p2FrameCounter = 0;
                
                // Log transition to next input
                if (p2QueueIndex < p2InputQueue.size() && detailedLogging.load()) {
                    LogOut("[INPUT_MOTION] P2 Advancing to next input: 0x" + 
                           std::to_string(p2InputQueue[p2QueueIndex].inputMask), false);
                }
            }
        } else {
            // Queue is finished
            p2QueueActive = false;
            WritePlayerInput(2, 0); // Clear inputs
            LogOut("[INPUT_MOTION] P2 Motion sequence complete", detailedLogging.load());
        }
    }
}

// Helper function to convert action type to motion type
int ConvertActionToMotion(int actionType, int triggerType) {
    // Add detailed logging to diagnose the issue
    LogOut("[INPUT_MOTION] ConvertActionToMotion called with actionType=" + 
           std::to_string(actionType) + ", triggerType=" + 
           std::to_string(triggerType), true);
    
    int motionType = MOTION_5A; // Default to 5A
    
    switch (actionType) {
        // Normal attacks - these map directly
        case ACTION_5A: 
            LogOut("[INPUT_MOTION] Converting ACTION_5A to MOTION_5A", true);
            return MOTION_5A;
        case ACTION_5B: 
            LogOut("[INPUT_MOTION] Converting ACTION_5B to MOTION_5B", true);
            return MOTION_5B;
        case ACTION_5C: 
            LogOut("[INPUT_MOTION] Converting ACTION_5C to MOTION_5C", true);
            return MOTION_5C;
        case ACTION_2A: 
            LogOut("[INPUT_MOTION] Converting ACTION_2A to MOTION_2A", true);
            return MOTION_2A;
        case ACTION_2B: 
            LogOut("[INPUT_MOTION] Converting ACTION_2B to MOTION_2B", true);
            return MOTION_2B;
        case ACTION_2C: 
            LogOut("[INPUT_MOTION] Converting ACTION_2C to MOTION_2C", true);
            return MOTION_2C;
        case ACTION_JA: 
            LogOut("[INPUT_MOTION] Converting ACTION_JA to MOTION_JA", true);
            return MOTION_JA;
        case ACTION_JB: 
            LogOut("[INPUT_MOTION] Converting ACTION_JB to MOTION_JB", true);
            return MOTION_JB;
        case ACTION_JC: 
            LogOut("[INPUT_MOTION] Converting ACTION_JC to MOTION_JC", true);
            return MOTION_JC;
        
        // Special moves - these need proper mapping
        case ACTION_QCF: 
            LogOut("[INPUT_MOTION] Converting ACTION_QCF to QCF motion", true);
            // Select the appropriate strength based on the trigger type
            if (triggerType == TRIGGER_AFTER_BLOCK) {
                LogOut("[INPUT_MOTION] After block trigger - using QCF+C", true);
                return MOTION_236C;  // Use C for reversals (max invuln)
            } else if (triggerType == TRIGGER_AFTER_HITSTUN) {
                LogOut("[INPUT_MOTION] After hitstun trigger - using QCF+B", true);
                return MOTION_236B;  // Use B for combos
            }
            LogOut("[INPUT_MOTION] Default trigger - using QCF+A", true);
            return MOTION_236A;  // Default to A version
            
        case ACTION_DP:
            LogOut("[INPUT_MOTION] Converting ACTION_DP to DP motion", true);
            // DP is commonly used as a reversal, so use the C version for more invulnerability
            if (triggerType == TRIGGER_AFTER_BLOCK || triggerType == TRIGGER_ON_WAKEUP) {
                LogOut("[INPUT_MOTION] Reversal situation - using DP+C", true);
                return MOTION_623C;  // Max invincibility for wake-up/reversal
            } else if (triggerType == TRIGGER_AFTER_HITSTUN) {
                LogOut("[INPUT_MOTION] After hitstun trigger - using DP+B", true);
                return MOTION_623B;  // Use B for combos
            }
            LogOut("[INPUT_MOTION] Default trigger - using DP+A", true);
            return MOTION_623A;  // Default to A version
            
        case ACTION_QCB:
            LogOut("[INPUT_MOTION] Converting ACTION_QCB to QCB motion", true);
            if (triggerType == TRIGGER_AFTER_BLOCK || triggerType == TRIGGER_ON_WAKEUP) {
                LogOut("[INPUT_MOTION] Reversal situation - using QCB+C", true);
                return MOTION_214C;  // Use C version for reversals
            } else if (triggerType == TRIGGER_AFTER_HITSTUN) {
                LogOut("[INPUT_MOTION] After hitstun trigger - using QCB+B", true);
                return MOTION_214B;  // Use B version after hitstun
            }
            LogOut("[INPUT_MOTION] Default trigger - using QCB+A", true);
            return MOTION_214A;  // Default to A version
            
        case ACTION_SUPER1:
            LogOut("[INPUT_MOTION] Converting ACTION_SUPER1 to HCF+C motion", true);
            return MOTION_41236C;  // Always use C version for supers (more damage)
            
        case ACTION_SUPER2:
            LogOut("[INPUT_MOTION] Converting ACTION_SUPER2 to HCB+C motion", true);
            return MOTION_63214C;  // Always use C version for supers (more damage)
            
        // Other actions
        case ACTION_CUSTOM:
            LogOut("[INPUT_MOTION] ACTION_CUSTOM (custom move ID) defaults to 5A", true);
            return MOTION_5A;  // Custom moves still use direct moveID setting
            
        default:
            LogOut("[INPUT_MOTION] Unknown action type " + std::to_string(actionType) + 
                   ", defaulting to MOTION_5A", true);
            return MOTION_5A;  // Default to basic attack
    }
}

// Helper function to convert button type to button mask
uint8_t ConvertButtonToMask(int buttonType) {
    switch (buttonType) {
        case 0: return BUTTON_A;
        case 1: return BUTTON_B;
        case 2: return BUTTON_C;
        default: return BUTTON_A;
    }
}

// Function to write inputs directly to memory
bool WritePlayerInput(int playerNum, uint8_t inputMask) {
    uintptr_t base = GetEFZBase();
    if (!base) return false;

    // Get player's base address
    uintptr_t playerOffset = (playerNum == 1) ? EFZ_BASE_OFFSET_P1 : EFZ_BASE_OFFSET_P2;
    uintptr_t playerBase = 0;
    if (!SafeReadMemory(base + playerOffset, &playerBase, sizeof(uintptr_t)) || !playerBase) {
        LogOut("[INPUT_MOTION] Failed to get player base address", true);
        return false;
    }

    // Get the current input buffer index (2 bytes, uint16_t)
    uint16_t currentIndex = 0;
    if (!SafeReadMemory(playerBase + P1_INPUT_BUFFER_INDEX_OFFSET, &currentIndex, sizeof(uint16_t))) {
        LogOut("[INPUT_MOTION] Failed to read input buffer index (2 bytes)", true);
        return false;
    }

    // Calculate the address to write the input
    // The buffer is at playerBase + P1_INPUT_BUFFER_OFFSET
    // The current position is at index currentIndex % 0x180
    int writeIndex = currentIndex % 0x180;
    uintptr_t inputAddr = playerBase + P1_INPUT_BUFFER_OFFSET + writeIndex;
    
    // For debugging, read the current value at this address
    uint8_t currentValue = 0;
    SafeReadMemory(inputAddr, &currentValue, sizeof(uint8_t));
    
    LogOut("[INPUT_MOTION] Writing input mask 0x" + std::to_string((int)inputMask) + 
           " to buffer address 0x" + std::to_string(inputAddr) + 
           " (index: " + std::to_string((int)currentIndex) + 
           ", writeIndex: " + std::to_string(writeIndex) + 
           ", current value: 0x" + std::to_string((int)currentValue) + ")", true);
    
    // Write the input to memory
    if (!SafeWriteMemory(inputAddr, &inputMask, sizeof(uint8_t))) {
        LogOut("[INPUT_MOTION] Failed to write input to memory", true);
        return false;
    }

    // Advance the buffer index and write it back (simulate new input frame)
    uint16_t newIndex = (currentIndex + 1) % 0x180;
    if (!SafeWriteMemory(playerBase + P1_INPUT_BUFFER_INDEX_OFFSET, &newIndex, sizeof(uint16_t))) {
        LogOut("[INPUT_MOTION] Failed to write new input buffer index", true);
        return false;
    }
    LogOut("[INPUT_MOTION] Advanced buffer index to " + std::to_string((int)newIndex), true);
    return true;
}

void LogCurrentInputs() {
    uintptr_t base = GetEFZBase();
    if (!base) return;

    // Helper to get the most recent input from the buffer
    auto getRecentInput = [base](int playerNum) -> uint8_t {
        uintptr_t playerOffset = (playerNum == 1) ? EFZ_BASE_OFFSET_P1 : EFZ_BASE_OFFSET_P2;
        uintptr_t playerBase = 0;
        if (!SafeReadMemory(base + playerOffset, &playerBase, sizeof(uintptr_t)) || !playerBase)
            return 0;
        // Use P1 offsets for both players if P2 offsets are not defined
        uintptr_t bufferOffset = P1_INPUT_BUFFER_OFFSET;
        uintptr_t indexOffset  = P1_INPUT_BUFFER_INDEX_OFFSET;
        uint16_t currentIndex = 0;
        if (!SafeReadMemory(playerBase + indexOffset, &currentIndex, sizeof(uint16_t)))
            return 0;
        int readIndex = (currentIndex - 1 + 0x180) % 0x180;
        uint8_t inputValue = 0;
        SafeReadMemory(playerBase + bufferOffset + readIndex, &inputValue, sizeof(uint8_t));
        return inputValue;
    };

    // Read most recent P1 and P2 input from buffer
    uint8_t p1Input = getRecentInput(1);
    uint8_t p2Input = getRecentInput(2);

    // Format inputs as direction + buttons
    auto formatInput = [](uint8_t input) -> std::string {
        std::string result = "";
        
        // Direction
        if (input & INPUT_UP) {
            if (input & INPUT_LEFT) result += "↖";
            else if (input & INPUT_RIGHT) result += "↗";
            else result += "↑";
        }
        else if (input & INPUT_DOWN) {
            if (input & INPUT_LEFT) result += "↙";
            else if (input & INPUT_RIGHT) result += "↘";
            else result += "↓";
        }
        else if (input & INPUT_LEFT) result += "←";
        else if (input & INPUT_RIGHT) result += "→";
        else result += "5"; // Numpad notation for neutral
        
        // Buttons
        if (input & INPUT_A) result += "A";
        if (input & INPUT_B) result += "B";
        if (input & INPUT_C) result += "C";
        if (input & INPUT_D) result += "D";
        
        if (result == "5") result = "Neutral";
        
        return result;
    };
    
    LogOut("[INPUT_STATUS] P1: " + formatInput(p1Input) + " (0x" + 
           std::to_string(p1Input) + "), P2: " + formatInput(p2Input) + 
           " (0x" + std::to_string(p2Input) + ")", true);
}

// Add to ProcessInputQueues or use as part of your debug tab
void ApplyDirectInput(int playerNum, uint8_t inputMask, int holdFrames) {
    // Reset any active queues for this player
    if (playerNum == 1) {
        p1InputQueue.clear();
        p1QueueActive = false;
        p1QueueIndex = 0;
        p1FrameCounter = 0;
    } else {
        p2InputQueue.clear();
        p2QueueActive = false;
        p2QueueIndex = 0;
        p2FrameCounter = 0;
    }
    
    LogOut("[INPUT_DIRECT] Applying direct input 0x" + std::to_string(inputMask) + 
           " to P" + std::to_string(playerNum) + " for " + std::to_string(holdFrames) + 
           " frames", true);
    
    // Create a simple sequence with the requested input
    std::vector<InputFrame>& queue = (playerNum == 1) ? p1InputQueue : p2InputQueue;
    queue.clear();
    
    // Add the input to hold
    queue.push_back(InputFrame(inputMask, holdFrames));
    
    // Add a release frame
    queue.push_back(InputFrame(0, 2));
    
    // Activate the queue
    if (playerNum == 1) {
        p1QueueActive = true;
    } else {
        p2QueueActive = true;
    }
    
    LogOut("[INPUT_DIRECT] Direct input sequence queued", true);
}

// Generate random inputs for testing
void GenerateRandomInputs(int playerNum, int durationFrames) {
    // Seed random generator
    srand((unsigned int)time(NULL));
    
    LogOut("[INPUT_TEST] Starting random input generation for P" + std::to_string(playerNum) + 
           " for " + std::to_string(durationFrames) + " frames", true);
    
    // Cache the player base and input addresses
    uintptr_t base = GetEFZBase();
    if (!base) return;
    
    uintptr_t playerOffset = (playerNum == 1) ? EFZ_BASE_OFFSET_P1 : EFZ_BASE_OFFSET_P2;
    uintptr_t playerBase = 0;
    SafeReadMemory(base + playerOffset, &playerBase, sizeof(uintptr_t));
    
    LogOut("[INPUT_TEST] Player " + std::to_string(playerNum) + 
           " base address: 0x" + std::to_string(playerBase), true);
    
    for (int i = 0; i < durationFrames; i++) {
        // Generate random input mask
        uint8_t randomMask = 0;
        
        // 25% chance for each directional input
        if (rand() % 4 == 0) randomMask |= INPUT_UP;
        if (rand() % 4 == 0) randomMask |= INPUT_DOWN;
        if (rand() % 4 == 0) randomMask |= INPUT_LEFT;
        if (rand() % 4 == 0) randomMask |= INPUT_RIGHT;
        
        // 20% chance for each button
        if (rand() % 5 == 0) randomMask |= INPUT_A;
        if (rand() % 5 == 0) randomMask |= INPUT_B;
        if (rand() % 5 == 0) randomMask |= INPUT_C;
        if (rand() % 5 == 0) randomMask |= INPUT_D;
        
        // Apply the input
        WritePlayerInput(playerNum, randomMask);
        
        // Hold for 1-3 frames
        int holdFrames = (rand() % 3) + 1;
        Sleep(holdFrames * 16); // ~16ms per frame
        
        // Sometimes insert neutral
        if (rand() % 3 == 0) {
            WritePlayerInput(playerNum, 0);
            Sleep(16);
        }
    }
    
    LogOut("[INPUT_TEST] Completed random input generation", true);
}

// Scan memory for possible input-related bytes
void ScanForInputBytes(int playerNum) {
    uintptr_t base = GetEFZBase();
    if (!base) {
        LogOut("[INPUT_SCAN] Failed to get game base address", true);
        return;
    }
    
    uintptr_t playerOffset = (playerNum == 1) ? EFZ_BASE_OFFSET_P1 : EFZ_BASE_OFFSET_P2;
    uintptr_t playerBase = 0;
    if (!SafeReadMemory(base + playerOffset, &playerBase, sizeof(uintptr_t)) || !playerBase) {
        LogOut("[INPUT_SCAN] Failed to get player base address", true);
        return;
    }
    
    LogOut("[INPUT_SCAN] Player " + std::to_string(playerNum) + 
           " base address: 0x" + std::to_string(playerBase), true);
    
    // Scan more offsets than just 0xB8
    const int SCAN_RANGES[][2] = {
        {0xB0, 0xC0},   // Around our current offset
        {0x50, 0x70},   // Common input-related offsets
        {0x100, 0x120}, // Another typical range
        {0x200, 0x220}  // Further out
    };
    
    for (int i = 0; i < sizeof(SCAN_RANGES)/sizeof(SCAN_RANGES[0]); i++) {
        int startOffset = SCAN_RANGES[i][0];
        int endOffset = SCAN_RANGES[i][1];
        
        LogOut("[INPUT_SCAN] Scanning offsets 0x" + std::to_string(startOffset) + 
               " to 0x" + std::to_string(endOffset), true);
        
        for (int offset = startOffset; offset <= endOffset; offset++) {
            uint8_t value = 0;
            if (SafeReadMemory(playerBase + offset, &value, sizeof(uint8_t))) {
                LogOut("[INPUT_SCAN] Offset 0x" + std::to_string(offset) + 
                       " = 0x" + std::to_string((int)value), true);
            }
        }
    }
}

// Try writing to multiple possible input offsets
void TestMultipleInputOffsets(int playerNum, uint8_t inputMask) {
    uintptr_t base = GetEFZBase();
    if (!base) return;
    
    uintptr_t playerOffset = (playerNum == 1) ? EFZ_BASE_OFFSET_P1 : EFZ_BASE_OFFSET_P2;
    uintptr_t playerBase = 0;
    SafeReadMemory(base + playerOffset, &playerBase, sizeof(uintptr_t));
    
    if (!playerBase) {
        LogOut("[INPUT_TEST] Invalid player base address", true);
        return;
    }
    
    // Try writing to multiple offsets
    const int TEST_OFFSETS[] = {0xB8, 0x60, 0x64, 0x68, 0x6C, 0x70, 0x100, 0x104, 0x108, 0x10C};
    
    LogOut("[INPUT_TEST] Testing input mask 0x" + std::to_string(inputMask) + 
           " on multiple offsets for P" + std::to_string(playerNum), true);
    
    for (int i = 0; i < sizeof(TEST_OFFSETS)/sizeof(TEST_OFFSETS[0]); i++) {
        int offset = TEST_OFFSETS[i];
        
        uint8_t oldValue = 0;
        SafeReadMemory(playerBase + offset, &oldValue, sizeof(uint8_t));
        
        LogOut("[INPUT_TEST] Writing to offset 0x" + std::to_string(offset) + 
               ", old value: 0x" + std::to_string((int)oldValue), true);
        
        if (SafeWriteMemory(playerBase + offset, &inputMask, sizeof(uint8_t))) {
            // Hold for a moment
            Sleep(100);
            
            uint8_t newValue = 0;
            SafeReadMemory(playerBase + offset, &newValue, sizeof(uint8_t));
            
            LogOut("[INPUT_TEST] Read back value: 0x" + std::to_string((int)newValue), true);
            
            // Restore old value
            SafeWriteMemory(playerBase + offset, &oldValue, sizeof(uint8_t));
        } else {
            LogOut("[INPUT_TEST] Failed to write to offset 0x" + std::to_string(offset), true);
        }
    }
}

// Look for potential input struct patterns
void AnalyzeInputStructure(int playerNum) {
    uintptr_t base = GetEFZBase();
    if (!base) return;
    
    uintptr_t playerOffset = (playerNum == 1) ? EFZ_BASE_OFFSET_P1 : EFZ_BASE_OFFSET_P2;
    uintptr_t playerBase = 0;
    SafeReadMemory(base + playerOffset, &playerBase, sizeof(uintptr_t));
    
    if (!playerBase) {
        LogOut("[INPUT_ANALYZE] Invalid player base address", true);
        return;
    }
    
    LogOut("[INPUT_ANALYZE] Analyzing input structure for P" + std::to_string(playerNum) +
           " (base: 0x" + std::to_string(playerBase) + ")", true);
    
    // From efz.exe.c we know the game has an input manager
    // Search for potential input manager pointers
    for (int offset = 0; offset < 0x200; offset += 4) {
        uintptr_t potentialPtr = 0;
        if (SafeReadMemory(playerBase + offset, &potentialPtr, sizeof(uintptr_t)) && 
            potentialPtr >= 0x10000 && potentialPtr < 0x20000000) { // Reasonable pointer range
            
            LogOut("[INPUT_ANALYZE] Potential pointer at offset 0x" + std::to_string(offset) + 
                   ": 0x" + std::to_string(potentialPtr), true);
            
            // Check if this might point to an input structure
            uint8_t inputBytes[16] = {0};
            if (SafeReadMemory(potentialPtr, inputBytes, sizeof(inputBytes))) {
                std::string bytesStr = "";
                for (int i = 0; i < 16; i++) {
                    bytesStr += std::to_string(inputBytes[i]) + " ";
                }
                LogOut("[INPUT_ANALYZE] Bytes at 0x" + std::to_string(potentialPtr) + ": " + bytesStr, true);
            }
        }
    }
}

// Test a promising input address
void TestPromisingSingleInputAddress(int playerNum, uint8_t inputMask, int holdFrames) {
    // Get base player address
    uintptr_t base = GetEFZBase();
    if (!base) return;
    
    uintptr_t playerOffset = (playerNum == 1) ? EFZ_BASE_OFFSET_P1 : EFZ_BASE_OFFSET_P2;
    uintptr_t playerBase = 0;
    SafeReadMemory(base + playerOffset, &playerBase, sizeof(uintptr_t));
    
    if (!playerBase) {
        LogOut("[INPUT_TEST] Invalid player base address", true);
        return;
    }
    
    LogOut("[INPUT_TEST] Testing sustained input at offset 0x96", true);
    
    // Save original value
    uint8_t originalValue = 0;
    SafeReadMemory(playerBase + 0x96, &originalValue, sizeof(uint8_t));
    
    // Write new value and hold it for specified frames
    LogOut("[INPUT_TEST] Writing input mask 0x" + std::to_string(inputMask) + 
           " to offset 0x96 and holding for " + std::to_string(holdFrames) + " frames", true);
    
    // Write repeatedly to ensure value sticks
    for (int i = 0; i < holdFrames; i++) {
        SafeWriteMemory(playerBase + 0x96, &inputMask, sizeof(uint8_t));
        Sleep(16); // ~60fps = ~16ms per frame
    }
    
    // Restore original value
    SafeWriteMemory(playerBase + 0x96, &originalValue, sizeof(uint8_t));
    LogOut("[INPUT_TEST] Restored original value", true);
}

// Monitor input addresses to see which ones change
void MonitorInputAddresses(int playerNum, int durationFrames) {
    uintptr_t base = GetEFZBase();
    if (!base) return;
    
    uintptr_t playerOffset = (playerNum == 1) ? EFZ_BASE_OFFSET_P1 : EFZ_BASE_OFFSET_P2;
    uintptr_t playerBase = 0;
    SafeReadMemory(base + playerOffset, &playerBase, sizeof(uintptr_t));
    
    if (!playerBase) {
        LogOut("[INPUT_MONITOR] Invalid player base address", true);
        return;
    }
    
    // Arrays to store previous values for comparison
    uint8_t prevValues[10] = {0};
    
    // Addresses to monitor - these are the most promising ones from our scan
    const int monitored_offsets[] = {0x96, 0xB8, 0x100, 0x264, 0x268, 0x272, 0x280, 0x284};
    const int num_offsets = sizeof(monitored_offsets) / sizeof(int);
    
    LogOut("[INPUT_MONITOR] Starting input address monitoring for " + 
           std::to_string(durationFrames) + " frames", true);
    LogOut("[INPUT_MONITOR] Press some inputs while this runs!", true);
    
    for (int frame = 0; frame < durationFrames; frame++) {
        for (int i = 0; i < num_offsets; i++) {
            uint8_t currentValue = 0;
            SafeReadMemory(playerBase + monitored_offsets[i], &currentValue, sizeof(uint8_t));
            
            // Check if value changed
            if (currentValue != prevValues[i]) {
                LogOut("[INPUT_MONITOR] Frame " + std::to_string(frame) + 
                       ": Offset 0x" + std::to_string(monitored_offsets[i]) + 
                       " changed from 0x" + std::to_string(prevValues[i]) + 
                       " to 0x" + std::to_string(currentValue), true);
                
                prevValues[i] = currentValue;
            }
        }
        
        Sleep(16); // ~60fps = ~16ms per frame
    }
    
    LogOut("[INPUT_MONITOR] Monitoring complete", true);
}

// Add this function to monitor the input buffer

void MonitorInputBuffer(int playerNum, int frameCount) {
    uintptr_t base = GetEFZBase();
    if (!base) return;

    // Get player's base address
    uintptr_t playerOffset = (playerNum == 1) ? EFZ_BASE_OFFSET_P1 : EFZ_BASE_OFFSET_P2;
    uintptr_t playerBase = 0;
    if (!SafeReadMemory(base + playerOffset, &playerBase, sizeof(uintptr_t)) || !playerBase) {
        LogOut("[INPUT_BUFFER] Failed to get player base address", true);
        return;
    }

    LogOut("[INPUT_BUFFER] Starting input buffer monitoring for Player " + 
           std::to_string(playerNum) + " for " + std::to_string(frameCount) + " frames", true);
    
    for (int frame = 0; frame < frameCount; frame++) {
        // Get the current input buffer index
        uint8_t currentIndex = 0;
        if (!SafeReadMemory(playerBase + P1_INPUT_BUFFER_INDEX_OFFSET, &currentIndex, sizeof(uint8_t))) {
            LogOut("[INPUT_BUFFER] Failed to read input buffer index", true);
            continue;
        }
        
        // Read the current input value
        uint8_t inputValue = 0;
        SafeReadMemory(playerBase + P1_INPUT_BUFFER_OFFSET + currentIndex, &inputValue, sizeof(uint8_t));
        
        // Decode the input
        std::string inputDesc = "";
        if (inputValue & INPUT_UP) inputDesc += "↑";
        if (inputValue & INPUT_DOWN) inputDesc += "↓";
        if (inputValue & INPUT_LEFT) inputDesc += "←";
        if (inputValue & INPUT_RIGHT) inputDesc += "→";
        if (inputValue & INPUT_A) inputDesc += "A";
        if (inputValue & INPUT_B) inputDesc += "B";
        if (inputValue & INPUT_C) inputDesc += "C";
        if (inputValue & INPUT_D) inputDesc += "D";
        if (inputDesc.empty()) inputDesc = "neutral";
        
        LogOut("[INPUT_BUFFER] Frame " + std::to_string(frame) + 
               ": Index=" + std::to_string((int)currentIndex) + 
               ", Input=0x" + std::to_string((int)inputValue) + 
               " (" + inputDesc + ")", true);
        
        Sleep(16); // ~60fps
    }
    
    LogOut("[INPUT_BUFFER] Input buffer monitoring complete", true);
}

void TestInputSequence(int playerNum) {
    LogOut("[INPUT_TEST] Starting input sequence test for P" + std::to_string(playerNum), true);
    
    // Test each directional input
    uint8_t inputs[] = {
        INPUT_UP,                   // Up
        INPUT_DOWN,                 // Down
        INPUT_LEFT,                 // Left
        INPUT_RIGHT,                // Right
        INPUT_UP | INPUT_LEFT,      // Up-Left
        INPUT_UP | INPUT_RIGHT,     // Up-Right
        INPUT_DOWN | INPUT_LEFT,    // Down-Left
        INPUT_DOWN | INPUT_RIGHT,   // Down-Right
        INPUT_A,                    // A button
        INPUT_B,                    // B button
        INPUT_C,                    // C button
        INPUT_D,                    // D button
        INPUT_A | INPUT_DOWN,       // Down+A (2A)
        INPUT_B | INPUT_DOWN,       // Down+B (2B)
        0x00                        // Neutral
    };
    
    // Apply each input for 10 frames
    for (int i = 0; i < sizeof(inputs)/sizeof(uint8_t); i++) {
        uint8_t input = inputs[i];
        
        std::string inputDesc = DecodeInputMask(input);
        LogOut("[INPUT_TEST] Testing input: " + inputDesc, true);
        
        // Apply for 10 frames
        for (int frame = 0; frame < 10; frame++) {
            WritePlayerInput(playerNum, input);
            Sleep(16); // ~60fps
        }
    }
    
    LogOut("[INPUT_TEST] Input sequence test complete", true);
}

std::string DecodeInputMask(uint8_t inputMask) {
    std::string result;
    
    // Handle neutral case first
    if (inputMask == 0) {
        return "neutral";
    }
    
    // Correct directional mappings based on your findings
    if (inputMask & 0x1) result += "→"; // Right (was incorrectly mapped)
    if (inputMask & 0x2) result += "←"; // Left (was incorrectly mapped)
    if (inputMask & 0x4) result += "↓"; // Down (was incorrectly mapped)
    if (inputMask & 0x8) result += "↑"; // Up (was incorrectly mapped)
    
    // Handle attack buttons
    if (inputMask & BUTTON_A) result += "A";
    if (inputMask & BUTTON_B) result += "B";
    if (inputMask & BUTTON_C) result += "C";
    if (inputMask & BUTTON_D) result += "D";
    
    return result;
}

// Improved test input logic: write a test input to the buffer using the new buffer/index logic
void TestInputBufferWrite(int playerNum, uint8_t inputMask) {
    uint8_t buffer[0x180] = {0};
    int currentIndex = 0;
    if (!ReadPlayerInputBuffer(playerNum, buffer, 0x180, currentIndex)) {
        LogOut("[TEST_INPUT] Failed to read input buffer for P" + std::to_string(playerNum), true);
        return;
    }
    // Write the input at the current index (wrap if needed)
    int writeIndex = currentIndex % 0x180;
    buffer[writeIndex] = inputMask;
    LogOut("[TEST_INPUT] Writing input 0x" + std::to_string(inputMask) + " to buffer index " + std::to_string(writeIndex) + " for P" + std::to_string(playerNum), true);
    // Actually write to game memory
    uintptr_t base = GetEFZBase();
    uintptr_t playerPtr = 0;
    uintptr_t baseOffset = (playerNum == 1) ? EFZ_BASE_OFFSET_P1 : EFZ_BASE_OFFSET_P2;
    if (!SafeReadMemory(base + baseOffset, &playerPtr, sizeof(uintptr_t)) || !playerPtr)
        return;
    SafeWriteMemory(playerPtr + 0x1AB + writeIndex, &inputMask, sizeof(uint8_t));
}