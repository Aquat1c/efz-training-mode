#include <windows.h>
#include <string>
#include <sstream>
#include <iomanip>
#include "../include/memory.h"
#include "../include/logger.h"
#include "../include/game_state.h"
#include "../include/constants.h"
#include "../include/utilities.h"

// Define constants for offsets
const uintptr_t P2_CPU_FLAG_OFFSET = 4931;
const uintptr_t AI_CONTROL_FLAG_OFFSET = 164;

// Forward declarations for helper functions
std::string FormatHexAddress(uintptr_t address);
void DumpPracticeModeState();
void LogPlayerInputsInPracticeMode();

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

// Function to monitor and log player inputs in practice mode
// This function should be called periodically when in practice mode
void LogPlayerInputsInPracticeMode() {
    uintptr_t efzBase = GetEFZBase();
    if (!efzBase) {
        return; // Silently fail - this will be called frequently
    }
    
    GameMode currentMode = GetCurrentGameMode();
    if (currentMode != GameMode::Practice) {
        return; // Only log inputs in practice mode
    }
    
    // Get character pointers
    uintptr_t p1CharPtr = 0;
    uintptr_t p2CharPtr = 0;
    
    if (!SafeReadMemory(efzBase + EFZ_BASE_OFFSET_P1, &p1CharPtr, sizeof(uintptr_t)) ||
        !SafeReadMemory(efzBase + EFZ_BASE_OFFSET_P2, &p2CharPtr, sizeof(uintptr_t))) {
        return; // Can't read character pointers
    }
    
    // Get AI control flags
    uint32_t p1AIFlag = 0;
    uint32_t p2AIFlag = 0;
    
    bool p1AIFlagValid = p1CharPtr && SafeReadMemory(p1CharPtr + AI_CONTROL_FLAG_OFFSET, &p1AIFlag, sizeof(uint32_t));
    bool p2AIFlagValid = p2CharPtr && SafeReadMemory(p2CharPtr + AI_CONTROL_FLAG_OFFSET, &p2AIFlag, sizeof(uint32_t));
    
    // Get player inputs
    uint8_t p1Input = GetPlayerInputs(1);
    uint8_t p2Input = GetPlayerInputs(2);
    
    // Only log if inputs are detected or every 20 calls (for periodic updates)
    static int callCount = 0;
    callCount++;
    
    if (p1Input || p2Input || callCount >= 20) {
        std::stringstream ss;
        ss << "[PRACTICE_PATCH] P1 " << (p1AIFlagValid ? (p1AIFlag ? "(AI) " : "(Human) ") : "(Unknown) ");
        
        // Decode P1 inputs
        if (p1Input & INPUT_UP) ss << "UP ";
        if (p1Input & INPUT_DOWN) ss << "DOWN ";
        if (p1Input & INPUT_LEFT) ss << "LEFT ";
        if (p1Input & INPUT_RIGHT) ss << "RIGHT ";
        if (p1Input & INPUT_A) ss << "A ";
        if (p1Input & INPUT_B) ss << "B ";
        if (p1Input & INPUT_C) ss << "C ";
        if (p1Input & INPUT_D) ss << "D ";
        
        if (!p1Input) ss << "NONE ";
        
        ss << "| P2 " << (p2AIFlagValid ? (p2AIFlag ? "(AI) " : "(Human) ") : "(Unknown) ");
        
        // Decode P2 inputs
        if (p2Input & INPUT_UP) ss << "UP ";
        if (p2Input & INPUT_DOWN) ss << "DOWN ";
        if (p2Input & INPUT_LEFT) ss << "LEFT ";
        if (p2Input & INPUT_RIGHT) ss << "RIGHT ";
        if (p2Input & INPUT_A) ss << "A ";
        if (p2Input & INPUT_B) ss << "B ";
        if (p2Input & INPUT_C) ss << "C ";
        if (p2Input & INPUT_D) ss << "D ";
        
        if (!p2Input) ss << "NONE ";
        
        LogOut(ss.str(), true);
        
        // Reset counter after logging
        if (callCount >= 20) {
            callCount = 0;
        }
    }
}


// NEW: Function to reset P2 character completely
void ResetP2Character() {
    LogOut("[PRACTICE_PATCH] Attempting to reset P2 character...", true);
    
    uintptr_t efzBase = GetEFZBase();
    if (!efzBase) {
        LogOut("[PRACTICE_PATCH] Failed to get EFZ base address", true);
        return;
    }
    
    uintptr_t gameStatePtr = 0;
    if (!SafeReadMemory(efzBase + EFZ_BASE_OFFSET_GAME_STATE, &gameStatePtr, sizeof(uintptr_t))) {
        LogOut("[PRACTICE_PATCH] Failed to read game state pointer", true);
        return;
    }
    
    // Set Player 2 to be human controlled (0 = human, 1 = CPU)
    uint8_t humanControlled = 0;
    if (!SafeWriteMemory(gameStatePtr + P2_CPU_FLAG_OFFSET, &humanControlled, sizeof(uint8_t))) {
        LogOut("[PRACTICE_PATCH] Failed to patch Player 2 CPU flag during reset", true);
        return;
    }
    
    LogOut("[PRACTICE_PATCH] Reset P2 character completed", true);
}
