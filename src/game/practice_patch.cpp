#include <windows.h>
#include <string>
#include <algorithm>
#include <sstream>
#include <iomanip>
#include "../include/core/memory.h"
#include "../include/core/logger.h"
#include "../include/game/game_state.h"

#include "../include/core/constants.h"
#include "../include/utils/utilities.h"

#include "../include/game/practice_patch.h"
#include "../include/gui/overlay.h"
#include "../include/input/input_core.h"  // Include this to get AI_CONTROL_FLAG_OFFSET
#include "../include/input/input_motion.h" // for direction/stance constants

// Define constants for offsets
const uintptr_t P2_CPU_FLAG_OFFSET = 4931;
const uintptr_t PRACTICE_BLOCK_MODE_OFFSET = 4934;   // 0..2
const uintptr_t PRACTICE_AUTO_BLOCK_OFFSET = 4936;   // dword, 0/1

// Forward declarations for helper functions
std::string FormatHexAddress(uintptr_t address);
void DumpPracticeModeState();
void LogPlayerInputsInPracticeMode();
static bool WriteP2BlockStance(uint8_t stance);

// Patch to enable Player 2 controls in Practice mode
bool EnablePlayer2InPracticeMode() {
    LogOut("[PRACTICE_PATCH] Attempting to enable Player 2 controls in Practice mode...", true);

    // Get base address of the game
    uintptr_t efzBase = GetEFZBase();
    if (!efzBase) {
        LogOut("[PRACTICE_PATCH] Failed to get EFZ base address", true);
        return false;
    }
    LogOut("[PRACTICE_PATCH] EFZ base address: 0x" + std::to_string(efzBase), true);

    // Game state pointer
    uintptr_t gameStatePtr = 0;
    
    // Get the game state pointer using our existing offset
    if (!SafeReadMemory(efzBase + EFZ_BASE_OFFSET_GAME_STATE, &gameStatePtr, sizeof(uintptr_t))) {
        LogOut("[PRACTICE_PATCH] Failed to read game state pointer", true);
        return false;
    }
    LogOut("[PRACTICE_PATCH] Game state pointer: 0x" + std::to_string(gameStatePtr), true);

    // Check if we're in practice mode
    uint8_t gameMode = 0;
    if (!SafeReadMemory(gameStatePtr + GAME_MODE_OFFSET, &gameMode, sizeof(uint8_t))) {
        LogOut("[PRACTICE_PATCH] Failed to read game mode", true);
        return false;
    }
    LogOut("[PRACTICE_PATCH] Current game mode value: " + std::to_string(gameMode), true);

    if (gameMode != 1) { // 1 = Practice mode
        LogOut("[PRACTICE_PATCH] Not in practice mode, not applying patch", true);
        return false;
    }
    LogOut("[PRACTICE_PATCH] Practice mode detected, proceeding with patch", true);

    // Now we need to modify the CPU control flag for Player 2
    // Read the current CPU control flag value before patching
    uint8_t currentControlFlag = 0;
    if (SafeReadMemory(gameStatePtr + P2_CPU_FLAG_OFFSET, &currentControlFlag, sizeof(uint8_t))) {
        LogOut("[PRACTICE_PATCH] Current P2 CPU control flag value: " + std::to_string(currentControlFlag), true);
    }
    
    // Set Player 2 to be human controlled (0 = human, 1 = CPU)
    uint8_t humanControlled = 0;
    if (!SafeWriteMemory(gameStatePtr + P2_CPU_FLAG_OFFSET, &humanControlled, sizeof(uint8_t))) {
        LogOut("[PRACTICE_PATCH] Failed to patch Player 2 CPU flag", true);
        return false;
    }
    LogOut("[PRACTICE_PATCH] Successfully patched P2 CPU control flag to 0 (human controlled)", true);

    // Additionally, we need to find and patch the AI flags for the actual Player 2 character
    // This is set in character initialization based on the above flag
    uintptr_t p2CharPtr = 0;
    if (!SafeReadMemory(efzBase + EFZ_BASE_OFFSET_P2, &p2CharPtr, sizeof(uintptr_t))) {
        LogOut("[PRACTICE_PATCH] Failed to read Player 2 character pointer", true);
        return false;
    }
    LogOut("[PRACTICE_PATCH] Player 2 character pointer: 0x" + std::to_string(p2CharPtr), true);

    // Handle the character AI control flag
    bool aiPatchSuccess = false;
    
    if (p2CharPtr) {
        // Read current AI control flag value
        uint32_t currentAIFlag = 0;
        if (SafeReadMemory(p2CharPtr + AI_CONTROL_FLAG_OFFSET, &currentAIFlag, sizeof(uint32_t))) {
            LogOut("[PRACTICE_PATCH] Current P2 AI control flag value: " + std::to_string(currentAIFlag), true);
        }
        
        // Apply patch multiple times to ensure it sticks
        uint32_t aiControlFlag = 0; // 0 = human controlled
        
        // First attempt
        if (SafeWriteMemory(p2CharPtr + AI_CONTROL_FLAG_OFFSET, &aiControlFlag, sizeof(uint32_t))) {
            LogOut("[PRACTICE_PATCH] First attempt: Successfully patched P2 AI control flag to 0", true);
            aiPatchSuccess = true;
        } else {
            LogOut("[PRACTICE_PATCH] First attempt: Failed to patch Player 2 AI control flag", true);
        }
        
        // Add a small delay and try again to make sure it sticks
        Sleep(10);
        
        // Second attempt
        if (SafeWriteMemory(p2CharPtr + AI_CONTROL_FLAG_OFFSET, &aiControlFlag, sizeof(uint32_t))) {
            LogOut("[PRACTICE_PATCH] Second attempt: Successfully patched P2 AI control flag to 0", true);
            aiPatchSuccess = true;
        } else {
            LogOut("[PRACTICE_PATCH] Second attempt: Failed to patch Player 2 AI control flag", true);
        }
    }
    else {
        LogOut("[PRACTICE_PATCH] Player 2 character not initialized yet, skipping AI flag patch", true);
    }

    LogOut("[PRACTICE_PATCH] " + std::string(aiPatchSuccess ? "Successfully" : "Partially") + 
           " enabled Player 2 controls in Practice mode", true);
    
    // Add additional debug verification
    LogOut("[PRACTICE_PATCH] Verifying patch application with final memory read...", true);
    
    // Verify CPU flag was actually changed
    uint8_t verifyControlFlag = 0xFF; // Initialize to invalid value
    if (SafeReadMemory(gameStatePtr + P2_CPU_FLAG_OFFSET, &verifyControlFlag, sizeof(uint8_t))) {
        if (verifyControlFlag == 0) {
            LogOut("[PRACTICE_PATCH] Verification SUCCESS: P2 CPU flag is properly set to 0", true);
        } else {
            LogOut("[PRACTICE_PATCH] Verification FAILED: P2 CPU flag is " + std::to_string(verifyControlFlag) + 
                  " (expected 0)", true);
        }
    }
    
    // Verify AI flag was actually changed (if character pointer exists)
    if (p2CharPtr) {
        uint32_t verifyAIFlag = 0xFFFFFFFF; // Initialize to invalid value
        if (SafeReadMemory(p2CharPtr + AI_CONTROL_FLAG_OFFSET, &verifyAIFlag, sizeof(uint32_t))) {
            if (verifyAIFlag == 0) {
                LogOut("[PRACTICE_PATCH] Verification SUCCESS: P2 AI flag is properly set to 0", true);
            } else {
                LogOut("[PRACTICE_PATCH] Verification FAILED: P2 AI flag is " + std::to_string(verifyAIFlag) + 
                      " (expected 0)", true);
                
                // If verification failed, try one more time with a different approach
                LogOut("[PRACTICE_PATCH] Attempting additional repair of AI flag...", true);
                
                // Try again with additional protection options
                DWORD oldProtect;
                if (VirtualProtect((LPVOID)(p2CharPtr + AI_CONTROL_FLAG_OFFSET), sizeof(uint32_t), PAGE_READWRITE, &oldProtect)) {
                    uint32_t zero = 0;
                    memcpy((void*)(p2CharPtr + AI_CONTROL_FLAG_OFFSET), &zero, sizeof(uint32_t));
                    VirtualProtect((LPVOID)(p2CharPtr + AI_CONTROL_FLAG_OFFSET), sizeof(uint32_t), oldProtect, &oldProtect);
                    
                    // Verify one final time
                    if (SafeReadMemory(p2CharPtr + AI_CONTROL_FLAG_OFFSET, &verifyAIFlag, sizeof(uint32_t)) && 
                        verifyAIFlag == 0) {
                        LogOut("[PRACTICE_PATCH] Additional repair SUCCESS: P2 AI flag is now 0", true);
                    } else {
                        LogOut("[PRACTICE_PATCH] Additional repair FAILED: P2 AI flag still not 0", true);
                    }
                } else {
                    LogOut("[PRACTICE_PATCH] Failed to change memory protection for additional repair", true);
                }
            }
        }
    }
    
    // Dump the state after patching to verify changes
    DumpPracticeModeState();
    
    return true;
}

// Function to scan for potential AI control flags in P2's character structure
void ScanForPotentialAIFlags() {
    LogOut("[PRACTICE_PATCH] Scanning for potential AI control flags...", true);
    
    uintptr_t efzBase = GetEFZBase();
    if (!efzBase) {
        LogOut("[PRACTICE_PATCH] Failed to get EFZ base address", true);
        return;
    }
    
    uintptr_t p2CharPtr = 0;
    if (!SafeReadMemory(efzBase + EFZ_BASE_OFFSET_P2, &p2CharPtr, sizeof(uintptr_t)) || !p2CharPtr) {
        LogOut("[PRACTICE_PATCH] Failed to read Player 2 character pointer", true);
        return;
    }
    
    LogOut("[PRACTICE_PATCH] P2 character pointer: " + FormatHexAddress(p2CharPtr), true);
    
    // Scan the first few hundred bytes of the character structure for values of 1
    // which might indicate control flags
    const int SCAN_SIZE = 512; // Scan first 512 bytes
    
    LogOut("[PRACTICE_PATCH] Scanning " + std::to_string(SCAN_SIZE) + " bytes for potential control flags...", true);
    
    for (int offset = 0; offset < SCAN_SIZE; offset += 4) {
        uint32_t value = 0;
        if (SafeReadMemory(p2CharPtr + offset, &value, sizeof(uint32_t))) {
            if (value == 1) { // Potential flag
                std::stringstream ss;
                ss << "[PRACTICE_PATCH] Potential control flag found at offset " 
                   << offset << " (0x" << std::hex << std::uppercase 
                   << std::setw(4) << std::setfill('0') << offset << ") = " << std::dec 
                   << value;
                LogOut(ss.str(), true);
                
                // Try patching it to see what happens
                uint32_t zero = 0;
                if (SafeWriteMemory(p2CharPtr + offset, &zero, sizeof(uint32_t))) {
                    LogOut("[PRACTICE_PATCH] Successfully patched potential control flag at offset " + 
                           std::to_string(offset) + " to 0", true);
                } else {
                    LogOut("[PRACTICE_PATCH] Failed to patch potential control flag at offset " + 
                           std::to_string(offset), true);
                }
            }
        }
    }
    
    LogOut("[PRACTICE_PATCH] Completed scanning for potential AI control flags", true);
}
// Function that continuously monitors and applies the patch
// This needs to be called from a background thread
void MonitorAndPatchPracticeMode() {
    LogOut("[PRACTICE_PATCH] Starting practice mode monitor thread", true);
    
    // Keep track of consecutive failures for error reporting
    int consecutiveFailures = 0;
    GameMode lastMode = GameMode::Unknown;
    int cycleCount = 0;
    
    // Track P2 control state changes
    bool lastP2CpuControlled = true;
    bool lastP2AIControlled = true;
    
    // Add input monitoring to the main loop
    while (true) {
        GameMode currentMode = GetCurrentGameMode();
        cycleCount++;
        
        // Log mode changes
        if (currentMode != lastMode) {
            LogOut("[PRACTICE_PATCH] Game mode changed: " + GetGameModeName(lastMode) + 
                   " -> " + GetGameModeName(currentMode), true);
            lastMode = currentMode;
            
            // Reset tracking on mode change
            lastP2CpuControlled = true;
            lastP2AIControlled = true;
            
            // Dump full state whenever game mode changes
            DumpPracticeModeState();
        }
        
        if (currentMode == GameMode::Practice) {
            // Check current P2 CPU flag and AI flag before patching
            uintptr_t efzBase = GetEFZBase();
            uintptr_t gameStatePtr = 0;
            uint8_t p2CpuFlag = 1; // Default to CPU controlled

            if (efzBase && 
                SafeReadMemory(efzBase + EFZ_BASE_OFFSET_GAME_STATE, &gameStatePtr, sizeof(uintptr_t)) &&
                SafeReadMemory(gameStatePtr + P2_CPU_FLAG_OFFSET, &p2CpuFlag, sizeof(uint8_t))) {
                
                bool p2CpuControlled = (p2CpuFlag != 0);
                
                // Log when P2 control state changes
                if (p2CpuControlled != lastP2CpuControlled) {
                    LogOut("[PRACTICE_PATCH] P2 CPU control state changed: " + 
                           std::string(lastP2CpuControlled ? "CPU" : "Human") + " -> " + 
                           std::string(p2CpuControlled ? "CPU" : "Human"), true);
                    lastP2CpuControlled = p2CpuControlled;
                }
                
                // Always check the AI flag status, even if CPU flag is already human
                uintptr_t p2CharPtr = 0;
                uint32_t p2AIFlag = 1; // Default to AI controlled
                bool p2CharacterInitialized = false;
                
                if (SafeReadMemory(efzBase + EFZ_BASE_OFFSET_P2, &p2CharPtr, sizeof(uintptr_t)) && p2CharPtr) {
                    p2CharacterInitialized = true;
                    SafeReadMemory(p2CharPtr + AI_CONTROL_FLAG_OFFSET, &p2AIFlag, sizeof(uint32_t));
                    
                    bool p2AIControlled = (p2AIFlag != 0);
                    
                    // Log when P2 AI control state changes
                    if (p2AIControlled != lastP2AIControlled && p2CharacterInitialized) {
                        LogOut("[PRACTICE_PATCH] P2 AI control state changed: " + 
                               std::string(lastP2AIControlled ? "AI" : "Human") + " -> " + 
                               std::string(p2AIControlled ? "AI" : "Human"), true);
                        
                        // If AI control was human but got switched back to AI, log this as a significant event
                        if (!lastP2AIControlled && p2AIControlled) {
                            LogOut("[PRACTICE_PATCH] IMPORTANT: AI control flag was reset to 1 by the game!", true);
                            LogOut("[PRACTICE_PATCH] This suggests the flag is actively maintained by game code", true);
                            
                            // Add timestamp to help track when this happens
                            auto now = std::chrono::system_clock::now();
                            auto now_time_t = std::chrono::system_clock::to_time_t(now);
                            std::stringstream ss;
                            ss << "[PRACTICE_PATCH] Time of AI flag reset: " << std::ctime(&now_time_t);
                            LogOut(ss.str(), true);
                            
                            // Dump full state when this happens
                            DumpPracticeModeState();
                        }
                        
                        lastP2AIControlled = p2AIControlled;
                    }
                    
                    // Always force the AI flag to 0 in Practice mode if it's not already
                    if (p2AIControlled && p2CharacterInitialized) {
                        LogOut("[PRACTICE_PATCH] P2 is AI controlled, applying AI flag patch...", detailedLogging.load());
                        uint32_t humanAIFlag = 0;
                        if (SafeWriteMemory(p2CharPtr + AI_CONTROL_FLAG_OFFSET, &humanAIFlag, sizeof(uint32_t))) {
                            LogOut("[PRACTICE_PATCH] Successfully patched P2 AI flag to 0 (human controlled)", detailedLogging.load());
                        } else {
                            LogOut("[PRACTICE_PATCH] Failed to patch P2 AI flag", true);
                        }
                    }
                }
                
                // Apply full patch if P2 is still CPU controlled at the game state level
                if (p2CpuControlled) {
                    LogOut("[PRACTICE_PATCH] P2 is CPU controlled, applying full patch...", true);
                    bool success = EnablePlayer2InPracticeMode();
                    
                    if (!success) {
                        consecutiveFailures++;
                        if (consecutiveFailures >= 5) {
                            LogOut("[PRACTICE_PATCH] WARNING: Failed to apply patch 5 times in a row", true);
                            DumpPracticeModeState(); // Dump state after repeated failures
                            consecutiveFailures = 0; // Reset counter to prevent spam
                        }
                    } else {
                        consecutiveFailures = 0;
                    }
                } else {
                    LogOut("[PRACTICE_PATCH] P2 is already human controlled at game state level", detailedLogging.load());
                }
            }
            
            // Periodically dump state in practice mode (every 30 cycles = ~15 seconds)
            if (cycleCount % 30 == 0) {
                LogOut("[PRACTICE_PATCH] Performing periodic state check", detailedLogging.load());
                DumpPracticeModeState();
                
                // Scan for potential AI flags every 5 minutes (600 cycles at 0.5s per cycle)
                if (cycleCount % 600 == 0) {
                    ScanForPotentialAIFlags();
                }
            }
            
            // Log player inputs every 10 cycles if in practice mode
            if (cycleCount % 10 == 0) {
                LogPlayerInputsInPracticeMode();
            }
        }
        
        // Sleep to avoid high CPU usage - check more frequently to catch any changes
        Sleep(500); // Check every 0.5 seconds instead of 1 second
    }
}

// Helper function to format memory addresses as hex strings
std::string FormatHexAddress(uintptr_t address) {
    std::stringstream ss;
    ss << "0x" << std::hex << std::uppercase << address;
    return ss.str();
}

// Debug function to dump the state of practice mode
void DumpPracticeModeState() {
    LogOut("[PRACTICE_PATCH] --------- Practice Mode Debug Dump ---------", true);
    
    uintptr_t efzBase = GetEFZBase();
    if (!efzBase) {
        LogOut("[PRACTICE_PATCH] Cannot dump state: Failed to get EFZ base address", true);
        return;
    }
    
    LogOut("[PRACTICE_PATCH] EFZ base address: " + FormatHexAddress(efzBase), true);
    
    // Get the game state pointer
    uintptr_t gameStatePtr = 0;
    if (!SafeReadMemory(efzBase + EFZ_BASE_OFFSET_GAME_STATE, &gameStatePtr, sizeof(uintptr_t))) {
        LogOut("[PRACTICE_PATCH] Cannot dump state: Failed to read game state pointer", true);
        return;
    }
    
    LogOut("[PRACTICE_PATCH] Game state pointer: " + FormatHexAddress(gameStatePtr), true);
    
    // Read current game mode
    uint8_t gameMode = 0;
    if (SafeReadMemory(gameStatePtr + GAME_MODE_OFFSET, &gameMode, sizeof(uint8_t))) {
        LogOut("[PRACTICE_PATCH] Current game mode value: " + std::to_string(gameMode), true);
    } else {
        LogOut("[PRACTICE_PATCH] Failed to read game mode", true);
    }
    
    // Read P2 CPU control flag
    uint8_t p2CpuFlag = 0;
    if (SafeReadMemory(gameStatePtr + P2_CPU_FLAG_OFFSET, &p2CpuFlag, sizeof(uint8_t))) {
        LogOut("[PRACTICE_PATCH] P2 CPU control flag: " + std::to_string(p2CpuFlag) + 
                " (" + (p2CpuFlag ? "CPU controlled" : "Human controlled") + ")", true);
    } else {
        LogOut("[PRACTICE_PATCH] Failed to read P2 CPU control flag", true);
    }
    
    // Read P2 character pointer and AI flag
    uintptr_t p2CharPtr = 0;
    if (SafeReadMemory(efzBase + EFZ_BASE_OFFSET_P2, &p2CharPtr, sizeof(uintptr_t))) {
        LogOut("[PRACTICE_PATCH] P2 character pointer: " + FormatHexAddress(p2CharPtr), true);
        
        if (p2CharPtr) {
            uint32_t aiFlag = 0;
            if (SafeReadMemory(p2CharPtr + AI_CONTROL_FLAG_OFFSET, &aiFlag, sizeof(uint32_t))) {
                LogOut("[PRACTICE_PATCH] P2 AI control flag: " + std::to_string(aiFlag) + 
                        " (" + (aiFlag ? "AI controlled" : "Player controlled") + ")", true);
            } else {
                LogOut("[PRACTICE_PATCH] Failed to read P2 AI control flag", true);
            }
        }
    } else {
        LogOut("[PRACTICE_PATCH] Failed to read P2 character pointer", true);
    }
    
    LogOut("[PRACTICE_PATCH] --------- End of Debug Dump ---------", true);
}

// Add this variable near the top of the file with other globals
std::atomic<bool> g_monitorInputs(true);
uint8_t lastP1Input = 0;
uint8_t lastP2Input = 0;

// Enhanced function to log player inputs with filtering for non-neutral inputs
void LogPlayerInputsInPracticeMode() {
    uintptr_t base = GetEFZBase();
    if (!base) return;
    
    // Get the current inputs for both players
    uint8_t p1Input = GetPlayerInputs(1);
    uint8_t p2Input = GetPlayerInputs(2);
    
    // Only log when inputs change from previous state or are non-neutral
    if (p1Input != lastP1Input && p1Input != 0) {
        LogOut("[INPUT] P1: 0x" + std::to_string(p1Input) + " (" + DecodeInputMask(p1Input) + ")", true);
    }
    
    if (p2Input != lastP2Input && p2Input != 0) {
        LogOut("[INPUT] P2: 0x" + std::to_string(p2Input) + " (" + DecodeInputMask(p2Input) + ")", true);
    }
    
    // Update last known inputs
    lastP1Input = p1Input;
    lastP2Input = p2Input;
}

// Add/update this function to properly decode player inputs
std::string GetDirectionName(uint8_t inputBits) {
    if (inputBits == 0) return "NONE";
    
    std::string result;
    
    if (inputBits & INPUT_UP)    result += "UP ";
    if (inputBits & INPUT_DOWN)  result += "DOWN ";
    if (inputBits & INPUT_LEFT)  result += "LEFT ";
    if (inputBits & INPUT_RIGHT) result += "RIGHT ";
    if (inputBits & INPUT_A)     result += "A ";
    if (inputBits & INPUT_B)     result += "B ";
    if (inputBits & INPUT_C)     result += "C ";
    if (inputBits & INPUT_D)     result += "D ";
    
    // Remove trailing space if any
    if (!result.empty()) {
        result.pop_back();
    }
    
    return result;
}

// Function to disable Player 2 controls in Practice mode
bool DisablePlayer2InPracticeMode() {
    LogOut("[PRACTICE_PATCH] Attempting to disable Player 2 controls in Practice mode...", true);

    uintptr_t efzBase = GetEFZBase();
    if (!efzBase) {
        LogOut("[PRACTICE_PATCH] Failed to get EFZ base address", true);
        return false;
    }

    uintptr_t gameStatePtr = 0;
    if (!SafeReadMemory(efzBase + EFZ_BASE_OFFSET_GAME_STATE, &gameStatePtr, sizeof(uintptr_t))) {
        LogOut("[PRACTICE_PATCH] Failed to read game state pointer", true);
        return false;
    }

    // Set Player 2 to be CPU controlled (1 = CPU, 0 = human)
    uint8_t cpuControlled = 1;
    if (!SafeWriteMemory(gameStatePtr + P2_CPU_FLAG_OFFSET, &cpuControlled, sizeof(uint8_t))) {
        LogOut("[PRACTICE_PATCH] Failed to restore Player 2 CPU flag", true);
        return false;
    }
    LogOut("[PRACTICE_PATCH] Restored P2 CPU control flag to 1 (CPU controlled)", true);

    // Restore the AI control flag for P2's character
    uintptr_t p2CharPtr = 0;
    if (!SafeReadMemory(efzBase + EFZ_BASE_OFFSET_P2, &p2CharPtr, sizeof(uintptr_t))) {
        LogOut("[PRACTICE_PATCH] Failed to read Player 2 character pointer", true);
        return false;
    }
    if (p2CharPtr) {
        uint32_t aiControlFlag = 1; // 1 = AI controlled
        if (!SafeWriteMemory(p2CharPtr + AI_CONTROL_FLAG_OFFSET, &aiControlFlag, sizeof(uint32_t))) {
            LogOut("[PRACTICE_PATCH] Failed to restore Player 2 AI control flag", true);
            return false;
        }
        LogOut("[PRACTICE_PATCH] Restored P2 AI control flag to 1 (AI controlled)", true);
    } else {
        LogOut("[PRACTICE_PATCH] Player 2 character not initialized yet, skipping AI flag restore", true);
    }

    DumpPracticeModeState();
    return true;
}

// Ensure P1 is player-controlled and P2 is AI-controlled at match start
void EnsureDefaultControlFlagsOnMatchStart() {
    uintptr_t efzBase = GetEFZBase();
    if (!efzBase) {
        LogOut("[PRACTICE_PATCH] EnsureDefaultControlFlagsOnMatchStart: EFZ base missing", true);
        return;
    }

    // 1) Set P2 to CPU at game state level
    uintptr_t gameStatePtr = 0;
    if (SafeReadMemory(efzBase + EFZ_BASE_OFFSET_GAME_STATE, &gameStatePtr, sizeof(uintptr_t)) && gameStatePtr) {
        uint8_t p2Cpu = 1; // 1 = CPU, 0 = human
        if (SafeWriteMemory(gameStatePtr + P2_CPU_FLAG_OFFSET, &p2Cpu, sizeof(uint8_t))) {
            LogOut("[PRACTICE_PATCH] MatchStart: P2 CPU flag set to 1 (CPU)", true);
        } else {
            LogOut("[PRACTICE_PATCH] MatchStart: Failed to set P2 CPU flag", true);
        }
    } else {
        LogOut("[PRACTICE_PATCH] MatchStart: Failed to read game state ptr", true);
    }

    // 2) Adjust per-character AI flags (affects in-match control)
    uintptr_t p1CharPtr = 0, p2CharPtr = 0;
    SafeReadMemory(efzBase + EFZ_BASE_OFFSET_P1, &p1CharPtr, sizeof(uintptr_t));
    SafeReadMemory(efzBase + EFZ_BASE_OFFSET_P2, &p2CharPtr, sizeof(uintptr_t));

    if (p1CharPtr) {
        uint32_t p1Ai = 0; // 0 = player-controlled
        if (SafeWriteMemory(p1CharPtr + AI_CONTROL_FLAG_OFFSET, &p1Ai, sizeof(uint32_t))) {
            LogOut("[PRACTICE_PATCH] MatchStart: P1 AI flag set to 0 (Player)", true);
        } else {
            LogOut("[PRACTICE_PATCH] MatchStart: Failed to set P1 AI flag", true);
        }
    } else {
        LogOut("[PRACTICE_PATCH] MatchStart: P1 character pointer invalid", true);
    }

    if (p2CharPtr) {
        uint32_t p2Ai = 1; // 1 = AI-controlled
        if (SafeWriteMemory(p2CharPtr + AI_CONTROL_FLAG_OFFSET, &p2Ai, sizeof(uint32_t))) {
            LogOut("[PRACTICE_PATCH] MatchStart: P2 AI flag set to 1 (AI)", true);
        } else {
            LogOut("[PRACTICE_PATCH] MatchStart: Failed to set P2 AI flag", true);
        }
    } else {
        LogOut("[PRACTICE_PATCH] MatchStart: P2 character pointer invalid", true);
    }
}

// --- Practice dummy F6/F7 equivalents ---

static bool GetGameStatePtr(uintptr_t &gameStateOut) {
    uintptr_t base = GetEFZBase();
    if (!base) return false;
    return SafeReadMemory(base + EFZ_BASE_OFFSET_GAME_STATE, &gameStateOut, sizeof(uintptr_t)) && gameStateOut != 0;
}

bool GetPracticeAutoBlockEnabled(bool &enabledOut) {
    enabledOut = false;
    if (GetCurrentGameMode() != GameMode::Practice) return false;
    uintptr_t gs = 0; if (!GetGameStatePtr(gs)) return false;
    uint32_t val = 0; if (!SafeReadMemory(gs + PRACTICE_AUTO_BLOCK_OFFSET, &val, sizeof(val))) return false;
    enabledOut = (val != 0);
    return true;
}

bool SetPracticeAutoBlockEnabled(bool enabled) {
    if (GetCurrentGameMode() != GameMode::Practice) return false;
    uintptr_t gs = 0; if (!GetGameStatePtr(gs)) return false;
    uint32_t val = enabled ? 1u : 0u;
    // Read current to avoid redundant writes/logs
    uint32_t cur = 0;
    SafeReadMemory(gs + PRACTICE_AUTO_BLOCK_OFFSET, &cur, sizeof(cur));
    if (cur == val) return true; // no change
    bool ok = SafeWriteMemory(gs + PRACTICE_AUTO_BLOCK_OFFSET, &val, sizeof(val));
    if (ok) {
    LogOut(std::string("[PRACTICE_PATCH] Auto-Block ") + (enabled ? "ON" : "OFF"), detailedLogging.load());
    }
    return ok;
}

bool GetPracticeBlockMode(int &modeOut) {
    modeOut = 0;
    if (GetCurrentGameMode() != GameMode::Practice) return false;
    uintptr_t gs = 0; if (!GetGameStatePtr(gs)) return false;
    uint8_t v = 0; if (!SafeReadMemory(gs + PRACTICE_BLOCK_MODE_OFFSET, &v, sizeof(v))) return false;
    modeOut = (int)v; if (modeOut < 0 || modeOut > 2) modeOut = 0;
    return true;
}

bool SetPracticeBlockMode(int mode) {
    if (GetCurrentGameMode() != GameMode::Practice) return false;
    if (mode < 0) mode = 0; if (mode > 2) mode = 2;
    uintptr_t gs = 0; if (!GetGameStatePtr(gs)) return false;
    // Avoid redundant writes
    uint8_t current = 0; SafeReadMemory(gs + PRACTICE_BLOCK_MODE_OFFSET, &current, sizeof(current));
    uint8_t v = (uint8_t)mode;
    if (current == v) return true;
    bool ok = SafeWriteMemory(gs + PRACTICE_BLOCK_MODE_OFFSET, &v, sizeof(v));
    if (ok) {
        // Map to stance semantics for 0/2; keep First for 1
        if (v == 0) {
            LogOut("[PRACTICE_PATCH] Block Stance -> Stand", detailedLogging.load());
            // Also immediately set stance byte to stand
            WriteP2BlockStance(0);
        } else if (v == 2) {
            LogOut("[PRACTICE_PATCH] Block Stance -> Crouch", detailedLogging.load());
            // Also immediately set stance byte to crouch
            WriteP2BlockStance(1);
        } else {
            LogOut("[PRACTICE_PATCH] Block Mode -> First", detailedLogging.load());
        }
    }
    return ok;
}

bool CyclePracticeBlockMode() {
    int m = 0; if (!GetPracticeBlockMode(m)) return false;
    m = (m + 1) % 3; return SetPracticeBlockMode(m);
}

// ---- Extended Dummy Auto-Block modes implementation ----
static std::atomic<int> g_dummyAutoBlockMode{0};
static std::atomic<bool> g_firstEventSeen{false};
// Note: Legacy detection using guardstun/hitstun counters proved unreliable across states; rely on moveIDs instead.
static std::atomic<bool> g_abWindowActive{false};
static std::atomic<unsigned long long> g_abWindowDeadlineMs{0};
static std::atomic<bool> g_adaptiveStance{false};
static std::atomic<bool> g_adaptiveForceTick{false}; // force immediate evaluation on enable
static std::atomic<bool> g_abOverrideActive{false}; // when false, follow the game's autoblock flag

// Internal: set mode without marking override or forcing baseline writes
static void SetDummyAutoBlockModeFromSync(int mode) {
    if (mode < 0) mode = 0; if (mode > 4) mode = 4;
    // Do not auto-enable adaptive monitoring from deprecated mode; map to All only
    if (mode == DAB_Adaptive) { mode = DAB_All; }
    g_dummyAutoBlockMode.store(mode);
    ResetDummyAutoBlockState();
}

// Tunables (ms)
static constexpr unsigned long long AB_DELAY_FIRST_HIT_THEN_OFF_MS = 2000ULL; // 2 seconds
static constexpr unsigned long long AB_DELAY_AFTER_FIRST_HIT_MS    = 1000ULL; // 1 second

void SetDummyAutoBlockMode(int mode) {
    if (mode < 0) mode = 0; if (mode > 4) mode = 4;
    // Migrate deprecated Adaptive mode to All; checkbox now controls monitoring
    if (mode == DAB_Adaptive) { mode = DAB_All; }
    g_dummyAutoBlockMode.store(mode);
    ResetDummyAutoBlockState();
    g_abOverrideActive.store(true);
    // Immediate base config for +4936 auto-block and stance
    switch (mode) {
        case DAB_All:
            SetPracticeAutoBlockEnabled(true);
            break;
        case DAB_None:
            // Turn off native autoblock
            SetPracticeAutoBlockEnabled(false);
            break;
        case DAB_FirstHitThenOff:
            // Start enabled so we can catch the first block
            SetPracticeAutoBlockEnabled(true);
            break;
        case DAB_EnableAfterFirstHit:
            // Start disabled until we detect first hit
            SetPracticeAutoBlockEnabled(false);
            break;
        case DAB_Adaptive:
            // Deprecated as a standalone mode; treat as All with adaptive checkbox controlling stance
            SetPracticeAutoBlockEnabled(true);
            break;
    }
}

int GetDummyAutoBlockMode() { return g_dummyAutoBlockMode.load(); }

void ResetDummyAutoBlockState() {
    g_firstEventSeen.store(false);
    // No counter history needed when using moveID edge detection
    g_abWindowActive.store(false);
    g_abWindowDeadlineMs.store(0);
}

void SetAdaptiveStanceEnabled(bool enabled) {
    g_adaptiveStance.store(enabled);
    if (enabled) {
        // Force the next MonitorDummyAutoBlock pass to evaluate immediately
        g_adaptiveForceTick.store(true);
    }
}
bool GetAdaptiveStanceEnabled() { return g_adaptiveStance.load(); }

// Helper: read stance/direction fields for P2, and Y positions for attacker(P1)
static bool ReadP2BlockFields(uint8_t &dirOut, uint8_t &stanceOut) {
    uintptr_t base = GetEFZBase(); if (!base) return false;
    uintptr_t p2 = 0; if (!SafeReadMemory(base + EFZ_BASE_OFFSET_P2, &p2, sizeof(p2)) || !p2) return false;
    uint8_t dir=0, stance=0;
    SafeReadMemory(p2 + 392, &dir, sizeof(dir));
    SafeReadMemory(p2 + 393, &stance, sizeof(stance));
    dirOut = dir; stanceOut = stance;
    return true;
}

static bool WriteP2BlockStance(uint8_t stance) {
    uintptr_t base = GetEFZBase(); if (!base) return false;
    uintptr_t p2 = 0; if (!SafeReadMemory(base + EFZ_BASE_OFFSET_P2, &p2, sizeof(p2)) || !p2) return false;
    return SafeWriteMemory(p2 + 393, &stance, sizeof(stance));
}

static bool ReadPositions(double &p1Y, double &p2Y) {
    uintptr_t base = GetEFZBase(); if (!base) return false;
    uintptr_t y1 = ResolvePointer(base, EFZ_BASE_OFFSET_P1, YPOS_OFFSET);
    uintptr_t y2 = ResolvePointer(base, EFZ_BASE_OFFSET_P2, YPOS_OFFSET);
    return y1 && y2 && SafeReadMemory(y1, &p1Y, sizeof(p1Y)) && SafeReadMemory(y2, &p2Y, sizeof(p2Y));
}

// Helper: classify common states by moveID
static inline bool IsP2BlockingOrBlockstun(short moveId) {
    // Inclusive range covers standing/crouching/air guard and early guardstun states
    return (moveId >= 150 && moveId <= 156);
}
static inline bool IsP2InHitstun(short moveId) {
    // Use constants.h ranges for hitstun (standing/crouch/launch/sweep)
    if ((moveId >= STAND_HITSTUN_START && moveId <= STAND_HITSTUN_END) ||
        (moveId >= CROUCH_HITSTUN_START && moveId <= CROUCH_HITSTUN_END) ||
        (moveId >= LAUNCHED_HITSTUN_START && moveId <= LAUNCHED_HITSTUN_END) ||
        (moveId == SWEEP_HITSTUN)) {
        return true;
    }
    return false;
}
// Helper: detect transition into a blocking/guard state this frame from moveIDs
static inline bool DidP2JustBlockThisFrame(short prevMoveId, short currMoveId) {
    return (!IsP2BlockingOrBlockstun(prevMoveId) && IsP2BlockingOrBlockstun(currMoveId));
}

// Core per-frame monitor; call from frame monitor after move IDs are read
void MonitorDummyAutoBlock(short p1MoveID, short p2MoveID, short prevP1MoveID, short prevP2MoveID) {
    if (GetCurrentGameMode() != GameMode::Practice) return;
    // Clear override when we return to Character Select (follow game's flag until user changes)
    static GamePhase s_lastPhase = GamePhase::Unknown;
    GamePhase phaseNow = GetCurrentGamePhase();
    if (phaseNow != s_lastPhase) {
        if (phaseNow == GamePhase::CharacterSelect) {
            g_abOverrideActive.store(false);
            // Sync displayed mode to current game flag
            uintptr_t gs = 0; if (GetGameStatePtr(gs)) {
                uint32_t f=0; if (SafeReadMemory(gs + PRACTICE_AUTO_BLOCK_OFFSET, &f, sizeof(f))) {
                    SetDummyAutoBlockModeFromSync((f!=0) ? DAB_All : DAB_None);
                }
            }
        }
        s_lastPhase = phaseNow;
    }
    int mode = g_dummyAutoBlockMode.load();
    // Transition log throttle (5s)
    static unsigned long long s_lastAbLog = 0;
    auto log_ab = [&](const std::string& msg){
        unsigned long long now = GetTickCount64();
        if (now - s_lastAbLog >= 5000ULL) {
            LogOut("[DUMMY_AB] " + msg, true);
            s_lastAbLog = now;
        }
    };
    // Compute desired autoblock state (single write at end when overridden)
    static bool s_lastAbOn = false; // what we last wrote when overriding
    bool abOn = false;

    // First-hit toggles
    if (mode == DAB_FirstHitThenOff) {
        // Enable initially; after the first successful block, keep enabled for a grace period (2s), then restore (disable)
        // Behavior (repeatable): start ON; on each block, immediately turn OFF, then after 2s turn ON again.
        bool justBlocked = DidP2JustBlockThisFrame(prevP2MoveID, p2MoveID);
        if (justBlocked) {
            // Immediately disable and start cooldown
            abOn = false;
            g_abWindowActive.store(true);
            g_abWindowDeadlineMs.store(GetTickCount64() + AB_DELAY_FIRST_HIT_THEN_OFF_MS);
            log_ab("FirstHitThenOff: blocked -> autoblock OFF, re-enable in 2s");
        } else if (g_abWindowActive.load()) {
            // During cooldown keep disabled until deadline
            abOn = false;
            if (GetTickCount64() >= g_abWindowDeadlineMs.load()) {
                g_abWindowActive.store(false);
                abOn = true;
                log_ab("FirstHitThenOff: cooldown ended -> autoblock ON");
            }
        } else {
            // Normal state: enabled, waiting for next block to start cooldown
            abOn = true;
        }
    }
    else if (mode == DAB_EnableAfterFirstHit) {
        // Initially disabled; when we detect first hit (hitstun rising), enable for 1s to block the next hit, then restore
        // Behavior (repeatable): idle OFF; when a hit occurs, enable for 1s or until a block occurs, then OFF again
        bool hitNow = (!IsP2InHitstun(prevP2MoveID) && IsP2InHitstun(p2MoveID));
        if (hitNow && !g_abWindowActive.load()) {
            abOn = true;
            g_abWindowActive.store(true);
            g_abWindowDeadlineMs.store(GetTickCount64() + AB_DELAY_AFTER_FIRST_HIT_MS);
            log_ab("AfterFirstHit: hit detected -> autoblock ON for 1s");
        }
        if (g_abWindowActive.load()) {
            // Keep enabled during window and close on block/timeout
            abOn = true;
            if (DidP2JustBlockThisFrame(prevP2MoveID, p2MoveID) || GetTickCount64() >= g_abWindowDeadlineMs.load()) {
                abOn = false;
                g_abWindowActive.store(false);
                log_ab("AfterFirstHit: window ended -> autoblock OFF");
            }
        } else {
            abOn = false;
        }
    }

    else if (mode == DAB_All) {
        abOn = true;
    }
    else if (mode == DAB_None) {
        abOn = false;
    }
    else {
        // Fallback safety
        abOn = false;
    }

    // Apply desired autoblock state only if user override is active or custom modes require it
    bool overrideEffective = g_abOverrideActive.load() || (mode == DAB_FirstHitThenOff) || (mode == DAB_EnableAfterFirstHit);
    if (overrideEffective && abOn != s_lastAbOn) {
        SetPracticeAutoBlockEnabled(abOn);
        s_lastAbOn = abOn;
    }

    // Watch the actual autoblock flag (+4936) at low frequency (1 Hz) and display overlay on any change
    static int s_lastAbFlag = -1; // -1 = unknown, otherwise 0/1
    static unsigned long long s_lastAbFlagCheckMs = 0;
    unsigned long long nowMs = GetTickCount64();
    if (nowMs - s_lastAbFlagCheckMs >= 1000ULL) {
        uintptr_t gsWatch = 0;
        if (GetGameStatePtr(gsWatch)) {
            uint32_t flagVal = 0;
            if (SafeReadMemory(gsWatch + PRACTICE_AUTO_BLOCK_OFFSET, &flagVal, sizeof(flagVal))) {
                int curFlag = (flagVal != 0) ? 1 : 0;
                if (s_lastAbFlag == -1) {
                    // First sample on entering Practice: initialize and sync display to actual flag (no announcement)
                    s_lastAbFlag = curFlag;
                    int curMode = g_dummyAutoBlockMode.load();
                    if (curMode == DAB_All && curFlag == 0) {
                        SetDummyAutoBlockModeFromSync(DAB_None);
                        LogOut("[DUMMY_AB] Sync(init): Game autoblock OFF -> mode set to None", true);
                    } else if (curMode == DAB_None && curFlag == 1) {
                        SetDummyAutoBlockModeFromSync(DAB_All);
                        LogOut("[DUMMY_AB] Sync(init): Game autoblock ON -> mode set to All", true);
                    }
                } else if (curFlag != s_lastAbFlag) {
                    // Emit overlay once per actual memory change
                    DirectDrawHook::AddMessage(curFlag ? "Block: ON" : "Block: OFF",
                                               "SYSTEM",
                                               curFlag ? RGB(100, 255, 100) : RGB(255, 255, 100),
                                               1500,
                                               0,
                                               100);
                    // Keep UI combobox in sync with actual game flag, but only for the simple None/All modes
                    int curMode = g_dummyAutoBlockMode.load();
                    if (curMode == DAB_All && curFlag == 0) {
                        SetDummyAutoBlockModeFromSync(DAB_None);
                        LogOut("[DUMMY_AB] Sync: Game cleared autoblock -> mode set to None", true);
                    } else if (curMode == DAB_None && curFlag == 1) {
                        SetDummyAutoBlockModeFromSync(DAB_All);
                        LogOut("[DUMMY_AB] Sync: Game enabled autoblock -> mode set to All", true);
                    }
                    s_lastAbFlag = curFlag;
                }
            }
        }
        s_lastAbFlagCheckMs = nowMs;
    }

    // Apply adaptive stance only when autoblock is ON, with throttling and dedupe
    // Determine whether autoblock is currently active for adaptive stance gate
    bool gameFlagOn = false; {
        uintptr_t gs=0; if (GetGameStatePtr(gs)) { uint32_t f=0; SafeReadMemory(gs + PRACTICE_AUTO_BLOCK_OFFSET, &f, sizeof(f)); gameFlagOn = (f!=0); }
    }
    bool activeAb = overrideEffective ? abOn : gameFlagOn;
    if (g_adaptiveStance.load() && activeAb) {
        static unsigned long long s_lastAdaptiveMs = 0;
        static bool s_lastAttackerAir = false;

        unsigned long long now = GetTickCount64();
        const unsigned long long ADAPTIVE_INTERVAL_MS = 16ULL; // ~60 Hz when enabled
        bool due = (now - s_lastAdaptiveMs >= ADAPTIVE_INTERVAL_MS) || g_adaptiveForceTick.load();
        if (due) {
            double p1Y = 0.0, p2Y = 0.0;
            if (ReadPositions(p1Y, p2Y)) {
                bool attackerAir = (p1Y < 0.0);
                bool changed = g_adaptiveForceTick.load() || (attackerAir != s_lastAttackerAir);
                if (changed) {
                    uint8_t desiredStance = attackerAir ? 0 /*stand*/ : 1 /*crouch*/;
                    uint8_t curDir=0, curStance=0;
                    if (ReadP2BlockFields(curDir, curStance)) {
                        if (curStance != desiredStance) {
                            WriteP2BlockStance(desiredStance);
                        }
                    } else {
                        WriteP2BlockStance(desiredStance);
                    }
                    SetPracticeBlockMode(attackerAir ? 0 : 2);
                    s_lastAttackerAir = attackerAir;
                }
            }
            s_lastAdaptiveMs = now;
            g_adaptiveForceTick.store(false);
        }
    }
}
