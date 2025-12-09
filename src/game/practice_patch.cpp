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
#include "../include/utils/network.h"

#include "../include/game/practice_patch.h"
#include "../include/game/per_frame_sample.h"
#include "../include/gui/overlay.h"
#include "../include/input/input_core.h"  // Include this to get AI_CONTROL_FLAG_OFFSET
#include "../include/input/input_motion.h" // for direction/stance constants
#include "../include/game/guard_overrides.h" // character/move grounded overheads
#include "../include/game/character_settings.h" // character ID from name
#include "../include/game/auto_action.h" // g_p2ControlOverridden
// EfzRevival practice controller offsets and pause integration (for GUI_POS fix at match start)
#include "../include/game/practice_offsets.h"
#include "../include/utils/pause_integration.h"
#include "../include/utils/config.h"
// For blockstun counter accessor used to gate autoblock disable
#include "../include/game/frame_analysis.h"

// Define constants for offsets
// P2 is always on the RIGHT side spatially
const uintptr_t P2_CPU_FLAG_OFFSET = 4932; // RIGHT side shutter (P2's spatial position)
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
    uintptr_t gameStatePtr = GetGameStatePtr();
    if (!gameStatePtr) {
        LogOut("[PRACTICE_PATCH] Failed to resolve game state pointer", true);
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
    uintptr_t p2CharPtr = GetPlayerBase(2);
    if (!p2CharPtr) {
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
    
    uintptr_t p2CharPtr = GetPlayerBase(2);
    if (!p2CharPtr) {
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
            uintptr_t gameStatePtr = GetGameStatePtr();
            uint8_t p2CpuFlag = 1; // Default to CPU controlled

            if (gameStatePtr &&
                SafeReadMemory(gameStatePtr + P2_CPU_FLAG_OFFSET, &p2CpuFlag, sizeof(uint8_t))) {
                
                bool p2CpuControlled = (p2CpuFlag != 0);
                
                // Log when P2 control state changes
                if (p2CpuControlled != lastP2CpuControlled) {
                    LogOut("[PRACTICE_PATCH] P2 CPU control state changed: " + 
                           std::string(lastP2CpuControlled ? "CPU" : "Human") + " -> " + 
                           std::string(p2CpuControlled ? "CPU" : "Human"), true);
                    lastP2CpuControlled = p2CpuControlled;
                }
                
                // Keep P2 character AI flag aligned with the game state's CPU flag (when not temporarily overridden).
                // If CPU flag says human (0), ensure AI flag = 0. If CPU flag says CPU (1), ensure AI flag = 1.
                uintptr_t p2CharPtr = GetPlayerBase(2);
                uint32_t p2AIFlag = 1; // Default to AI controlled
                bool p2CharacterInitialized = (p2CharPtr != 0);

                if (p2CharacterInitialized) {
                    SafeReadMemory(p2CharPtr + AI_CONTROL_FLAG_OFFSET, &p2AIFlag, sizeof(uint32_t));

                    bool p2AIControlled = (p2AIFlag != 0);

                    if (p2AIControlled != lastP2AIControlled && p2CharacterInitialized) {
                        LogOut("[PRACTICE_PATCH] P2 AI control state changed: " +
                               std::string(lastP2AIControlled ? "AI" : "Human") + " -> " +
                               std::string(p2AIControlled ? "AI" : "Human"), true);
                        lastP2AIControlled = p2AIControlled;
                    }

                    // Desired AI flag follows the game state's CPU flag
                    uint32_t desiredAIFlag = p2CpuControlled ? 1u : 0u;
                    if (p2AIFlag != desiredAIFlag) {
                        // Do not perform writes unless we are firmly in Match phase to avoid affecting menus/CS
                        if (GetCurrentGamePhase() != GamePhase::Match) {
                            // Just observe; avoid writing during Character Select or other non-match phases
                            lastP2AIControlled = (p2AIFlag != 0);
                        } else
                        
                        if (g_p2ControlOverridden) {
                            // During auto-action/macro control override, do not fight temporary human control.
                            if (detailedLogging.load()) {
                                LogOut("[PRACTICE_PATCH] Skip AI sync (override active)", true);
                            }
                        } else {
                            // Audit before/after for attribution
                            uint32_t before = 0xFFFFFFFF, after = 0xFFFFFFFF;
                            SafeReadMemory(p2CharPtr + AI_CONTROL_FLAG_OFFSET, &before, sizeof(before));
                            bool okWrite = SafeWriteMemory(p2CharPtr + AI_CONTROL_FLAG_OFFSET, &desiredAIFlag, sizeof(uint32_t));
                            SafeReadMemory(p2CharPtr + AI_CONTROL_FLAG_OFFSET, &after, sizeof(after));
                            std::ostringstream oss; oss << "[AUDIT][AI] PracticeSync P2 @0x" << std::hex << (p2CharPtr + AI_CONTROL_FLAG_OFFSET)
                                                        << " " << std::dec << before << "->" << after
                                                        << " (want " << desiredAIFlag << ")" << (okWrite?"":" (fail)");
                            LogOut(oss.str(), true);
                        }
                    }
                }

                // Do not auto-toggle the P2 CPU flag here. That is controlled by UI actions and SwitchPlayers.
                // This avoids fighting with side switching logic and eliminates the uncontrollable toggles.
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
    uintptr_t gameStatePtr = GetGameStatePtr();
    if (!gameStatePtr) {
        LogOut("[PRACTICE_PATCH] Cannot dump state: Failed to resolve game state pointer", true);
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
    uintptr_t p2CharPtr = GetPlayerBase(2);
    if (p2CharPtr) {
        LogOut("[PRACTICE_PATCH] P2 character pointer: " + FormatHexAddress(p2CharPtr), true);
        
        uint32_t aiFlag = 0;
        if (SafeReadMemory(p2CharPtr + AI_CONTROL_FLAG_OFFSET, &aiFlag, sizeof(uint32_t))) {
            LogOut("[PRACTICE_PATCH] P2 AI control flag: " + std::to_string(aiFlag) + 
                    " (" + (aiFlag ? "AI controlled" : "Player controlled") + ")", true);
        } else {
            LogOut("[PRACTICE_PATCH] Failed to read P2 AI control flag", true);
        }
    } else {
        LogOut("[PRACTICE_PATCH] Failed to read Player 2 character pointer", true);
    }
    
    LogOut("[PRACTICE_PATCH] --------- End of Debug Dump ---------", true);
}

std::atomic<bool> g_monitorInputs(true);
uint8_t lastP1Input = 0;
uint8_t lastP2Input = 0;

// Enhanced function to log player inputs with filtering for non-neutral inputs
void LogPlayerInputsInPracticeMode() {
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

    uintptr_t gameStatePtr = GetGameStatePtr();
    if (!gameStatePtr) {
        LogOut("[PRACTICE_PATCH] Failed to resolve game state pointer", true);
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
    uintptr_t p2CharPtr = GetPlayerBase(2);
    if (p2CharPtr) {
        uint32_t aiControlFlag = 1; // 1 = AI controlled
        if (!SafeWriteMemory(p2CharPtr + AI_CONTROL_FLAG_OFFSET, &aiControlFlag, sizeof(uint32_t))) {
            LogOut("[PRACTICE_PATCH] Failed to restore Player 2 AI control flag", true);
            return false;
        }
        LogOut("[PRACTICE_PATCH] Restored P2 AI control flag to 1 (AI controlled)", true);
    } else {
        LogOut("[PRACTICE_PATCH] Player 2 character not initialized yet, skipping AI flag restore", true);
        return false;
    }

    DumpPracticeModeState();
    return true;
}

// Ensure P1 is player-controlled and P2 is AI-controlled at match start
void EnsureDefaultControlFlagsOnMatchStart() {
    // Only modify control flags in offline Practice mode
    if (GetCurrentGameMode() != GameMode::Practice) return;
    if (DetectOnlineMatch()) return;
    
    uintptr_t gameStatePtr = GetGameStatePtr();
    if (gameStatePtr) {
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
    uintptr_t p1CharPtr = GetPlayerBase(1);
    uintptr_t p2CharPtr = GetPlayerBase(2);

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

    // 3) Align EfzRevival Practice GUI position to P1 (same as SwitchPlayers logic)
    //    GUI_POS(+0x24) expects 1 when P1 is local/human and 0 when P2 is local.
    //    Our default enforcement makes P1 human at match start, so set GUI_POS = 1.
    PauseIntegration::EnsurePracticePointerCapture();
    void* practice = PauseIntegration::GetPracticeControllerPtr();
    if (practice) {
        uint8_t guiPos = 1u; // P1
        bool okWrite = SafeWriteMemory((uintptr_t)practice + PRACTICE_OFF_GUI_POS, &guiPos, sizeof(guiPos));
        uint8_t verify = 0xFF; SafeReadMemory((uintptr_t)practice + PRACTICE_OFF_GUI_POS, &verify, sizeof(verify));
        std::ostringstream oss; oss << "[PRACTICE_PATCH] MatchStart: GUI_POS(+0x24) set to " << (int)verify << (okWrite?"":" (fail)");
        LogOut(oss.str(), true);
    } else {
        LogOut("[PRACTICE_PATCH] MatchStart: Practice controller unavailable, GUI_POS not updated", true);
    }
}

// --- Practice dummy F6/F7 equivalents ---

bool GetPracticeAutoBlockEnabled(bool &enabledOut) {
    enabledOut = false;
    if (GetCurrentGameMode() != GameMode::Practice) return false;
    uintptr_t gs = GetGameStatePtr(); if (!gs) return false;
    uint32_t val = 0; if (!SafeReadMemory(gs + PRACTICE_AUTO_BLOCK_OFFSET, &val, sizeof(val))) return false;
    enabledOut = (val != 0);
    return true;
}

bool SetPracticeAutoBlockEnabled(bool enabled, const char* reason) {
    if (GetCurrentGameMode() != GameMode::Practice) return false;
    if (DetectOnlineMatch()) return false;
    uintptr_t gs = GetGameStatePtr(); if (!gs) return false;
    uint32_t val = enabled ? 1u : 0u;
    // Read current to avoid redundant writes/logs
    uint32_t cur = 0;
    SafeReadMemory(gs + PRACTICE_AUTO_BLOCK_OFFSET, &cur, sizeof(cur));
    if (cur == val) return true; // no change
    bool ok = SafeWriteMemory(gs + PRACTICE_AUTO_BLOCK_OFFSET, &val, sizeof(val));
    if (ok) {
        std::ostringstream oss;
        oss << "Auto-Block " << (enabled ? "ON" : "OFF")
            << " (was " << (cur ? "ON" : "OFF") << ")";
        if (reason && *reason) {
            oss << " [reason: " << reason << "]";
        }
        LogOut(std::string("[PRACTICE_PATCH] ") + oss.str(), true);
    }
    return ok;
}

// (moved below static variables and helper definitions)

bool GetPracticeBlockMode(int &modeOut) {
    modeOut = 0;
    if (GetCurrentGameMode() != GameMode::Practice) return false;
    uintptr_t gs = GetGameStatePtr(); if (!gs) return false;
    uint8_t v = 0; if (!SafeReadMemory(gs + PRACTICE_BLOCK_MODE_OFFSET, &v, sizeof(v))) return false;
    modeOut = (int)v; if (modeOut < 0 || modeOut > 2) modeOut = 0;
    return true;
}

bool SetPracticeBlockMode(int mode) {
    if (GetCurrentGameMode() != GameMode::Practice) return false;
    if (DetectOnlineMatch()) return false;
    if (mode < 0) mode = 0; if (mode > 2) mode = 2;
    uintptr_t gs = GetGameStatePtr(); if (!gs) return false;
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
// Expose the current desired autoblock state evaluated each frame by MonitorDummyAutoBlock
static std::atomic<bool> g_desiredAbOn{false};
// When true, MonitorDummyAutoBlock will not write to +4936 (Random Block or other controller will own writes)
static std::atomic<bool> g_externalAbController{false};

// Helper: human-readable name for Dummy Auto-Block modes
static const char* GetDabModeName(int mode) {
    switch (mode) {
        case DAB_None:                 return "None";
        case DAB_All:                  return "All";
        case DAB_FirstHitThenOff:      return "FirstHitThenOff";
        case DAB_EnableAfterFirstHit:  return "EnableAfterFirstHit";
        case DAB_Adaptive:             return "Adaptive (deprecated)";
        default:                       return "Unknown";
    }
}

// Lightweight sampler for attacker frame flags (AttackProps/HitProps) to drive adaptive stance
// Returns true if sampled successfully; outputs:
//   level: 0=None, 1=High, 2=Low, 3=Any
//   blockable: true if HitProps bit0 is set
static bool SampleAttackerFrameFlags(int attacker /*1=P1, 2=P2*/, int &level, bool &blockable,
                                    uint16_t* outAtk=nullptr, uint16_t* outHit=nullptr, uint16_t* outGrd=nullptr,
                                    uint16_t* outState=nullptr, uint16_t* outFrame=nullptr,
                                    int* outNextLevel=nullptr, bool* outNextBlockable=nullptr,
                                    int* outNext2Level=nullptr, bool* outNext2Blockable=nullptr) {
    // Per-attacker caches to minimize ResolvePointer/reads
    static uintptr_t s_lastPBase[3]      = {0, 0, 0};
    static uintptr_t s_lastAnimTab[3]    = {0, 0, 0};
    static uint16_t  s_lastState[3]      = {0xFFFF, 0xFFFF, 0xFFFF};
    static uintptr_t s_lastFramesPtr[3]  = {0, 0, 0};

    uintptr_t pBase = 0;
    if (attacker == 1 || attacker == 2) {
        pBase = GetPlayerBase(attacker);
    }
    if (!pBase) return false;
    // Resolve anim frames table and frame block with caching
    uint16_t state = 0, frame = 0; uintptr_t animTab = 0, framesPtr = 0, frameBlock = 0;
    // Prefer unified per-frame sample for current moveID/state when attacker is player 1 or 2
    const PerFrameSample &sample = GetCurrentPerFrameSample();
    if (attacker == 1) {
        state = static_cast<uint16_t>(sample.moveID1);
    } else if (attacker == 2) {
        state = static_cast<uint16_t>(sample.moveID2);
    } else {
        if (!SafeReadMemory(pBase + MOVE_ID_OFFSET, &state, sizeof(state))) return false;
    }
    if (!SafeReadMemory(pBase + CURRENT_FRAME_INDEX_OFFSET, &frame, sizeof(frame))) return false;
    if (!SafeReadMemory(pBase + ANIM_TABLE_OFFSET, &animTab, sizeof(animTab))) return false;
    if (!animTab) return false;

    // Invalidate cache if base/anim changed
    if (s_lastPBase[attacker] != pBase || s_lastAnimTab[attacker] != animTab) {
        s_lastPBase[attacker] = pBase;
        s_lastAnimTab[attacker] = animTab;
        s_lastState[attacker] = 0xFFFF;
        s_lastFramesPtr[attacker] = 0;
    }
    if (s_lastState[attacker] != state || s_lastFramesPtr[attacker] == 0) {
        uintptr_t entryAddr = animTab + (static_cast<uintptr_t>(state) * ANIM_ENTRY_STRIDE) + ANIM_ENTRY_FRAMES_PTR_OFFSET;
        if (!SafeReadMemory(entryAddr, &framesPtr, sizeof(framesPtr)) || !framesPtr) return false;
        s_lastState[attacker] = state;
        s_lastFramesPtr[attacker] = framesPtr;
    } else {
        framesPtr = s_lastFramesPtr[attacker];
    }
    frameBlock = framesPtr + (static_cast<uintptr_t>(frame) * FRAME_BLOCK_STRIDE);
    uint16_t atk=0, hit=0, grd=0; if (!SafeReadMemory(frameBlock + FRAME_ATTACK_PROPS_OFFSET, &atk, sizeof(atk))) return false;
    SafeReadMemory(frameBlock + FRAME_HIT_PROPS_OFFSET, &hit, sizeof(hit));
    SafeReadMemory(frameBlock + FRAME_GUARD_PROPS_OFFSET, &grd, sizeof(grd));
    if (outAtk) *outAtk = atk; if (outHit) *outHit = hit; if (outGrd) *outGrd = grd;
    if (outState) *outState = state; if (outFrame) *outFrame = frame;
    // Decode
    bool isHigh = (atk & 0x1) != 0;
    bool isLow  = (atk & 0x2) != 0;
    level = isHigh && isLow ? 3 : (isHigh ? 1 : (isLow ? 2 : 0));
    // Prefer GuardProps presence for practical blockable window
    blockable = (grd != 0);
    // If there's a guardable window but no explicit HIGH/LOW bit, treat it as ANY to preserve stance
    if (level == 0 && blockable) {
        level = 3; // ANY
    }

    // Optional: 1-frame lookahead to detect imminent HIGH/LOW to preempt stance switch
    if (outNextLevel || outNextBlockable || outNext2Level || outNext2Blockable) {
        int nextLevel = -1; bool nextBlk = false;
        uintptr_t nextFrameBlock = framesPtr + (static_cast<uintptr_t>(frame + 1) * FRAME_BLOCK_STRIDE);
        uint16_t nAtk=0, nHit=0, nGrd=0;
        if (SafeReadMemory(nextFrameBlock + FRAME_ATTACK_PROPS_OFFSET, &nAtk, sizeof(nAtk))) {
            SafeReadMemory(nextFrameBlock + FRAME_HIT_PROPS_OFFSET, &nHit, sizeof(nHit));
            SafeReadMemory(nextFrameBlock + FRAME_GUARD_PROPS_OFFSET, &nGrd, sizeof(nGrd));
            bool nHigh = (nAtk & 0x1) != 0;
            bool nLow  = (nAtk & 0x2) != 0;
            nextLevel = nHigh && nLow ? 3 : (nHigh ? 1 : (nLow ? 2 : 0));
            nextBlk = (nGrd != 0);
            if (nextLevel == 0 && nextBlk) nextLevel = 3;
        }
        int next2Level = -1; bool next2Blk = false;
        uintptr_t next2FrameBlock = framesPtr + (static_cast<uintptr_t>(frame + 2) * FRAME_BLOCK_STRIDE);
        uint16_t n2Atk=0, n2Hit=0, n2Grd=0;
        if (SafeReadMemory(next2FrameBlock + FRAME_ATTACK_PROPS_OFFSET, &n2Atk, sizeof(n2Atk))) {
            SafeReadMemory(next2FrameBlock + FRAME_HIT_PROPS_OFFSET, &n2Hit, sizeof(n2Hit));
            SafeReadMemory(next2FrameBlock + FRAME_GUARD_PROPS_OFFSET, &n2Grd, sizeof(n2Grd));
            bool n2High = (n2Atk & 0x1) != 0;
            bool n2Low  = (n2Atk & 0x2) != 0;
            next2Level = n2High && n2Low ? 3 : (n2High ? 1 : (n2Low ? 2 : 0));
            next2Blk = (n2Grd != 0);
            if (next2Level == 0 && next2Blk) next2Level = 3;
        }
        if (outNextLevel) *outNextLevel = nextLevel;
        if (outNextBlockable) *outNextBlockable = nextBlk;
        if (outNext2Level) *outNext2Level = next2Level;
        if (outNext2Blockable) *outNext2Blockable = next2Blk;
    }
    return true;
}

// Internal: set mode without marking override or forcing baseline writes
static void SetDummyAutoBlockModeFromSync(int mode) {
    if (mode < 0) mode = 0; if (mode > 4) mode = 4;
    // Do not auto-enable adaptive monitoring from deprecated mode; map to All only
    if (mode == DAB_Adaptive) { mode = DAB_All; }
    int prev = g_dummyAutoBlockMode.load();
    g_dummyAutoBlockMode.store(mode);
    if (prev != mode) {
        std::ostringstream oss; oss << "Sync: mode " << GetDabModeName(prev) << " -> " << GetDabModeName(mode);
        LogOut(std::string("[DUMMY_AB] ") + oss.str(), true);
    }
    ResetDummyAutoBlockState();
}

// Tunables (ms)
static constexpr unsigned long long AB_DELAY_FIRST_HIT_THEN_OFF_MS = 2000ULL; // 2 seconds
static constexpr unsigned long long AB_DELAY_AFTER_FIRST_HIT_MS    = 1000ULL; // 1 second

void SetDummyAutoBlockMode(int mode) {
    if (mode < 0) mode = 0; if (mode > 4) mode = 4;
    // Migrate deprecated Adaptive mode to All; checkbox now controls monitoring
    if (mode == DAB_Adaptive) { mode = DAB_All; }
    int prev = g_dummyAutoBlockMode.load();
    g_dummyAutoBlockMode.store(mode);
    if (prev != mode) {
        std::ostringstream oss; oss << "Set: mode " << GetDabModeName(prev) << " -> " << GetDabModeName(mode);
        LogOut(std::string("[DUMMY_AB] ") + oss.str(), true);
    }
    ResetDummyAutoBlockState();
    // Only take over the game's autoblock flag for the two custom modes that programmatically toggle it.
    // For None/All, leave override disabled so in-game F7 (and our F7 mirror) remain authoritative.
    bool wantOverride = (mode == DAB_FirstHitThenOff) || (mode == DAB_EnableAfterFirstHit);
    g_abOverrideActive.store(wantOverride);
    // Immediate base config for +4936 auto-block and stance
    switch (mode) {
        case DAB_All:
            SetPracticeAutoBlockEnabled(true, "SetDummyAutoBlockMode: DAB_All baseline");
            break;
        case DAB_None:
            // Turn off native autoblock
            SetPracticeAutoBlockEnabled(false, "SetDummyAutoBlockMode: DAB_None baseline");
            break;
        case DAB_FirstHitThenOff:
            // Start enabled so we can catch the first block
            SetPracticeAutoBlockEnabled(true, "SetDummyAutoBlockMode: DAB_FirstHitThenOff baseline");
            break;
        case DAB_EnableAfterFirstHit:
            // Start disabled until we detect first hit
            SetPracticeAutoBlockEnabled(false, "SetDummyAutoBlockMode: DAB_EnableAfterFirstHit baseline");
            break;
        case DAB_Adaptive:
            // Deprecated as a standalone mode; treat as All with adaptive checkbox controlling stance
            SetPracticeAutoBlockEnabled(true, "SetDummyAutoBlockMode: DAB_Adaptive->All baseline");
            break;
    }
}

int GetDummyAutoBlockMode() { return g_dummyAutoBlockMode.load(); }

bool SyncAutoBlockModeFromGameFlag(bool clearOverride) {
    if (GetCurrentGameMode() != GameMode::Practice) return false;
    uintptr_t gs = GetGameStatePtr(); if (!gs) return false;
    uint32_t f = 0; if (!SafeReadMemory(gs + PRACTICE_AUTO_BLOCK_OFFSET, &f, sizeof(f))) return false;
    // Update our mode to mirror the flag (None/All only), without engaging override
    SetDummyAutoBlockModeFromSync((f != 0) ? DAB_All : DAB_None);
    if (clearOverride) {
        g_abOverrideActive.store(false);
    }
    return true;
}

void ResetDummyAutoBlockState() {
    g_firstEventSeen.store(false);
    // No counter history needed when using moveID edge detection
    g_abWindowActive.store(false);
    g_abWindowDeadlineMs.store(0);
    g_desiredAbOn.store(false);
}

void SetAdaptiveStanceEnabled(bool enabled) {
    g_adaptiveStance.store(enabled);
    if (enabled) {
        // Force the next MonitorDummyAutoBlock pass to evaluate immediately
        g_adaptiveForceTick.store(true);
    }
}
bool GetAdaptiveStanceEnabled() { return g_adaptiveStance.load(); }

bool GetCurrentDesiredAutoBlockOn(bool &onOut) {
    if (GetCurrentGameMode() != GameMode::Practice) { onOut = false; return false; }
    onOut = g_desiredAbOn.load();
    return true;
}

void SetExternalAutoBlockController(bool enabled) {
    g_externalAbController.store(enabled);
}

// Helper: read stance/direction fields for P2, and Y positions for attacker(P1)
static bool ReadP2BlockFields(uint8_t &dirOut, uint8_t &stanceOut) {
    uintptr_t p2 = GetPlayerBase(2); if (!p2) return false;
    uint8_t dir=0, stance=0;
    SafeReadMemory(p2 + 392, &dir, sizeof(dir));
    SafeReadMemory(p2 + 393, &stance, sizeof(stance));
    dirOut = dir; stanceOut = stance;
    return true;
}

static bool WriteP2BlockStance(uint8_t stance) {
    uintptr_t p2 = GetPlayerBase(2); if (!p2) return false;
    return SafeWriteMemory(p2 + 393, &stance, sizeof(stance));
}

// Assist: lightly nudge P2 direction to match stance for faster recognition by engine
// For crouch: set DOWN bit, clear UP; for stand: clear DOWN (leave other bits intact)
// Assist guard by holding BACK (and optionally DOWN) relative to P2 facing
// guardLevel: 0=None, 1=High, 2=Low, 3=Any
static void AssistP2DirectionForStance(uint8_t desiredStance, bool wantGuard, int guardLevel) {
    uintptr_t p2 = GetPlayerBase(2); if (!p2) return;
    uint8_t dir = 0; SafeReadMemory(p2 + 392, &dir, sizeof(dir));
    // Stance vertical assist
    if (desiredStance == 1) {
        // crouch
        dir &= ~INPUT_UP;           // clear UP
        dir |= INPUT_DOWN;          // set DOWN
    } else {
        // stand
        dir &= ~INPUT_DOWN;         // clear DOWN
    }

    // Guard horizontal assist: press BACK relative to facing if a guard window is active/imminent
    if (wantGuard) {
        bool facingRight = GetPlayerFacingDirection(2); // true = facing right
        uint8_t backBit = facingRight ? INPUT_LEFT : INPUT_RIGHT;
        uint8_t fwdBit  = facingRight ? INPUT_RIGHT : INPUT_LEFT;
        // Clear forward, set back
        dir &= ~fwdBit;
        dir |= backBit;
        // For low guard (or if crouch desired), ensure DOWN is set (down-back)
        if (guardLevel == 2 || desiredStance == 1) {
            dir |= INPUT_DOWN;
            dir &= ~INPUT_UP;
        }
    }
    SafeWriteMemory(p2 + 392, &dir, sizeof(dir));
}

static bool ReadPositions(double &p1Y, double &p2Y) {
    static uintptr_t s_p1YAddr = 0;
    static uintptr_t s_p2YAddr = 0;
    static int s_cacheCounter = 0;

    uintptr_t base = GetEFZBase();
    if (!base) { s_p1YAddr = s_p2YAddr = 0; return false; }

    // Refresh cached addresses occasionally or if missing
    if (!s_p1YAddr || !s_p2YAddr || (++s_cacheCounter >= 192)) {
        s_p1YAddr = ResolvePointer(base, EFZ_BASE_OFFSET_P1, YPOS_OFFSET);
        s_p2YAddr = ResolvePointer(base, EFZ_BASE_OFFSET_P2, YPOS_OFFSET);
        s_cacheCounter = 0;
    }

    if (!s_p1YAddr || !s_p2YAddr) return false;

    bool ok1 = SafeReadMemory(s_p1YAddr, &p1Y, sizeof(p1Y));
    bool ok2 = SafeReadMemory(s_p2YAddr, &p2Y, sizeof(p2Y));
    if (!ok1 || !ok2) {
        // Invalidate cache on failure so we’ll resolve next call
        if (!ok1) s_p1YAddr = 0;
        if (!ok2) s_p2YAddr = 0;
        return false;
    }
    return true;
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
    // Only operate in offline Practice mode
    if (GetCurrentGameMode() != GameMode::Practice) return;
    if (DetectOnlineMatch()) return;
    
    // Clear override when we return to Character Select (follow game's flag until user changes)
    static GamePhase s_lastPhase = GamePhase::Unknown;
    GamePhase phaseNow = GetCurrentGamePhase();
    if (phaseNow != s_lastPhase) {
        if (phaseNow == GamePhase::CharacterSelect) {
            g_abOverrideActive.store(false);
            // Sync displayed mode to current game flag
            uintptr_t gs = GetGameStatePtr(); if (gs) {
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
    static bool s_pendingAbOff = false; // defer turning OFF until blockstun ends/actionable

    // Shared event detectors
    const bool justBlocked = DidP2JustBlockThisFrame(prevP2MoveID, p2MoveID);
    const bool hitNow = (!IsP2InHitstun(prevP2MoveID) && IsP2InHitstun(p2MoveID));
    auto isAllowedNeutral = [](short m){
        // Allowed MoveIDs: 0,1,2,3,4,7,8,9,13 (same as Continuous Recovery)
        return (m == 0 || m == 1 || m == 2 || m == 3 || m == 4 || m == 7 || m == 8 || m == 9 || m == 13);
    };
    const bool neutralNow = isAllowedNeutral(p2MoveID);
    const bool transitionedToNeutral = (!isAllowedNeutral(prevP2MoveID) && neutralNow);
    int neutralTimeoutMs = Config::GetSettings().autoBlockNeutralTimeoutMs;
    if (neutralTimeoutMs < 0) neutralTimeoutMs = 0; // clamp
    const unsigned long long curMs = GetTickCount64();

    // Reset per-mode state when mode changes
    static int s_lastMode = -999;
    static bool s_waitForHitAfterBlock = false; // DAB_FirstHitThenOff
    static bool s_waitForBlockAfterHit = false; // DAB_EnableAfterFirstHit
    static unsigned long long s_neutralStartMs = 0ULL; // continuous neutral timer while waiting
    if (mode != s_lastMode) {
        s_waitForHitAfterBlock = false;
        s_waitForBlockAfterHit = false;
        s_neutralStartMs = 0ULL;
        g_abWindowActive.store(false);
        // Force immediate reevaluation log with explicit names
        {
            std::ostringstream oss; oss << "Mode changed: " << GetDabModeName(s_lastMode) << " -> "
                                        << GetDabModeName(mode) << "; resetting autoblock state machine";
            log_ab(oss.str());
        }
        s_lastMode = mode;
    }

    // First-hit toggles (event-driven)
    if (mode == DAB_FirstHitThenOff) {
        // Start ON; on first block, turn OFF and wait until we detect a hit OR a safe neutral edge, then turn ON again.
        if (justBlocked) {
            abOn = false;
            s_waitForHitAfterBlock = true;
            s_neutralStartMs = 0ULL;
            log_ab("FirstHitThenOff: blocked -> autoblock OFF (waiting for hit or neutral)");
        } else if (s_waitForHitAfterBlock) {
            // Keep OFF while waiting; re-enable when we observe a hit OR a neutral reset
            abOn = false;
            if (hitNow) {
                abOn = true;
                s_waitForHitAfterBlock = false;
                s_neutralStartMs = 0ULL;
                log_ab("FirstHitThenOff: got hit -> autoblock ON");
            } else {
                if (neutralTimeoutMs == 0) {
                    if (transitionedToNeutral) {
                        abOn = true;
                        s_waitForHitAfterBlock = false;
                        s_neutralStartMs = 0ULL;
                        log_ab("FirstHitThenOff: neutral edge -> autoblock ON");
                    }
                } else if (neutralNow) {
                    if (s_neutralStartMs == 0ULL) s_neutralStartMs = curMs;
                    if (curMs - s_neutralStartMs >= (unsigned long long)neutralTimeoutMs) {
                        abOn = true;
                        s_waitForHitAfterBlock = false;
                        s_neutralStartMs = 0ULL;
                        log_ab("FirstHitThenOff: neutral timeout -> autoblock ON");
                    }
                } else {
                    s_neutralStartMs = 0ULL; // lost neutral, reset timer
                }
            }
        } else {
            // Normal armed state
            abOn = true;
        }
    }
    else if (mode == DAB_EnableAfterFirstHit) {
        // Start OFF; on first hit, turn ON as soon as possible, and keep ON until a block occurs (or neutral timeout), then turn OFF.
        if (hitNow && !s_waitForBlockAfterHit) {
            abOn = true;
            s_waitForBlockAfterHit = true;
            s_neutralStartMs = 0ULL;
            log_ab("AfterFirstHit: hit detected -> autoblock ON (waiting for block)");
        }
        if (s_waitForBlockAfterHit) {
            abOn = true;
            if (justBlocked) {
                abOn = false;
                s_waitForBlockAfterHit = false;
                s_neutralStartMs = 0ULL;
                log_ab("AfterFirstHit: blocked -> autoblock OFF");
            } else {
                if (neutralTimeoutMs == 0) {
                    if (transitionedToNeutral) {
                        abOn = false;
                        s_waitForBlockAfterHit = false;
                        s_neutralStartMs = 0ULL;
                        log_ab("AfterFirstHit: neutral edge -> autoblock OFF");
                    }
                } else if (neutralNow) {
                    if (s_neutralStartMs == 0ULL) s_neutralStartMs = curMs;
                    if (curMs - s_neutralStartMs >= (unsigned long long)neutralTimeoutMs) {
                        // Safety: if the sequence ends without a block, time out cleanly on neutral for configured duration
                        abOn = false;
                        s_waitForBlockAfterHit = false;
                        s_neutralStartMs = 0ULL;
                        log_ab("AfterFirstHit: neutral timeout -> autoblock OFF");
                    }
                } else {
                    s_neutralStartMs = 0ULL; // lost neutral, reset timer
                }
            }
        } else if (!hitNow) {
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

    // Gate turning OFF until guard ends only for custom modes that programmatically toggle OFF
    if (mode == DAB_FirstHitThenOff || mode == DAB_EnableAfterFirstHit) {
        // Read P2 blockstun counter (decrements by 3 per visual frame) and guard state
        short p2Blockstun = 0;
        {
            uintptr_t base = GetEFZBase();
            if (base) {
                p2Blockstun = GetBlockstunValue(base, 2);
            }
        }
        const bool inGuardNow = IsP2BlockingOrBlockstun(p2MoveID) || (p2Blockstun > 0);
        // Use unified sample actionable flag for current P2 move when available
        const PerFrameSample &dabSample = GetCurrentPerFrameSample();
        const bool actionableNow = (dabSample.moveID2 == p2MoveID ? dabSample.actionable2 : IsActionable(p2MoveID));
        const bool leftGuardNow = (IsP2BlockingOrBlockstun(prevP2MoveID) && !IsP2BlockingOrBlockstun(p2MoveID));

        // If we want to turn OFF while guarding, defer until safe
        if (!abOn) {
            if (inGuardNow || !actionableNow) {
                // Defer the OFF toggle; keep autoblock ON until safe
                s_pendingAbOff = true;
                abOn = true;
            }
        }
        // If previously pending OFF, check if conditions cleared
        if (s_pendingAbOff) {
            // Try to disable on guard-end edge if actionable (even if counter still > 0)
            if ((leftGuardNow && actionableNow) || (!inGuardNow && actionableNow)) {
                // Safe to finally disable
                abOn = false;
                s_pendingAbOff = false;
                if (detailedLogging.load()) {
                    LogOut("[DUMMY_AB] OFF on guard end edge (moveID)", true);
                }
            } else {
                // Continue holding ON while waiting
                abOn = true;
            }
        }
    } else {
        // All/None should not auto-disable; clear any stale pending flag
        s_pendingAbOff = false;
    }

    // Publish desired AB state for other systems (e.g., Random Block)
    g_desiredAbOn.store(abOn);

    // Apply desired autoblock state only if user override is active or custom modes require it,
    // and no external controller is currently managing writes to +4936
    bool overrideEffective = g_abOverrideActive.load() || (mode == DAB_FirstHitThenOff) || (mode == DAB_EnableAfterFirstHit);
    if (overrideEffective && !g_externalAbController.load() && abOn != s_lastAbOn) {
        const char* why = nullptr;
        if (mode == DAB_FirstHitThenOff) {
            why = abOn ? "FirstHitThenOff: re-enable (hit/neutral)" : "FirstHitThenOff: disable (block)";
        } else if (mode == DAB_EnableAfterFirstHit) {
            why = abOn ? "EnableAfterFirstHit: enable (hit)" : "EnableAfterFirstHit: disable (block/neutral)";
        } else {
            why = "OverrideEffective write";
        }
        SetPracticeAutoBlockEnabled(abOn, why);
        s_lastAbOn = abOn;
    }

    // Watch the actual autoblock flag (+4936) at low frequency (1 Hz) and display overlay on any change
    static int s_lastAbFlag = -1; // -1 = unknown, otherwise 0/1
    static unsigned long long s_lastAbFlagCheckMs = 0;
    unsigned long long nowMs = GetTickCount64();
    if (nowMs - s_lastAbFlagCheckMs >= 1000ULL) {
        uintptr_t gsWatch = GetGameStatePtr();
        if (gsWatch) {
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
        uintptr_t gs = GetGameStatePtr();
        if (gs) {
            uint32_t f=0; SafeReadMemory(gs + PRACTICE_AUTO_BLOCK_OFFSET, &f, sizeof(f));
            gameFlagOn = (f!=0);
        }
    }
    bool activeAb = overrideEffective ? abOn : gameFlagOn;
    static bool s_prevActiveAb = false;
    if (g_adaptiveStance.load() && activeAb) {
        // Gate adaptive stance on P2 actually being AI controlled. If we've forced P2 to human (0) for auto-actions,
        // we should skip adaptive stance adjustments to avoid log spam and unintended stance overwrites.
        uintptr_t baseAI = GetPlayerBase(2);
        if (!baseAI) return; // can't evaluate
        uint32_t aiFlag = 1; SafeReadMemory(baseAI + AI_CONTROL_FLAG_OFFSET, &aiFlag, sizeof(aiFlag));
        if (aiFlag == 0) {
            // Suppressed; optional throttled debug
            static int s_aiGateDbg = 0; if (detailedLogging.load() && (s_aiGateDbg++ & 0x3F) == 0) {
                LogOut("[ADAPTIVE] Skipping stance logic (P2 under player control)", true);
            }
            return;
        }
        static unsigned long long s_lastAdaptiveMs = 0;
        unsigned long long now = GetTickCount64();
        const unsigned long long ADAPTIVE_INTERVAL_MS = 0ULL; // every frame
        bool due = (now - s_lastAdaptiveMs >= ADAPTIVE_INTERVAL_MS) || g_adaptiveForceTick.load();
        if (due) {
            // Stats/diagnostics sampling (not used for stance): keep per-frame guard decoding for UI
            int dummyLevel=-1; bool dummyBlk=false; int dummyNL=-1; bool dummyNB=false; int dummyN2L=-1; bool dummyN2B=false;
            uint16_t atkFlags=0, hitFlags=0, grdFlags=0, st=0, fr=0;
            SampleAttackerFrameFlags(1, dummyLevel, dummyBlk, &atkFlags, &hitFlags, &grdFlags, &st, &fr, &dummyNL, &dummyNB, &dummyN2L, &dummyN2B);

            // Determine base stance: ground=crouch, air=stand
            double p1Y=0.0, p2Y=0.0; bool haveCached = TryGetCachedYPositions(p1Y, p2Y, 200);
            if (!haveCached && ReadPositions(p1Y, p2Y)) {
                UpdatePositionCache(0.0, p1Y, 0.0, p2Y);
                haveCached = true;
            }
            bool attackerAir = haveCached ? (p1Y < 0.0) : false;

            // Character-specific overrides: certain grounded moves are overheads => stand
            static int s_p1CharID = -1;
            static unsigned long long s_lastCharRefresh = 0;
            if (now - s_lastCharRefresh > 500ULL || s_p1CharID < 0) {
                uintptr_t base = GetEFZBase();
                char nameBuf[32] = {0};
                SafeReadMemory(ResolvePointer(base, EFZ_BASE_OFFSET_P1, CHARACTER_NAME_OFFSET), nameBuf, sizeof(nameBuf)-1);
                s_p1CharID = CharacterSettings::GetCharacterID(std::string(nameBuf));
                s_lastCharRefresh = now;
            }

            uint8_t desiredStance = attackerAir ? 0 : 1; // 0=stand,1=crouch
            if (!attackerAir && s_p1CharID >= 0) {
                uintptr_t p1Base = GetPlayerBase(1);
                if (GuardOverrides::IsGroundedOverhead(s_p1CharID, static_cast<int>(st), p1Base)) {
                    desiredStance = 0;
                }
            }

            // Write if changed
            uint8_t curDir=0, curStance=0;
            if (ReadP2BlockFields(curDir, curStance)) {
                if (curStance != desiredStance) {
                    // Preserve P2 guard context (blockstun + blocking MoveID) so stance/mode tweaks don't cut guard
                    uintptr_t p2 = GetPlayerBase(2);
                    short prevBlk=0; short prevMove=0;
                    bool prevWasBlocking=false;
                    if (p2) {
                        SafeReadMemory(p2 + BLOCKSTUN_OFFSET, &prevBlk, sizeof(prevBlk));
                        SafeReadMemory(p2 + MOVE_ID_OFFSET, &prevMove, sizeof(prevMove));
                        prevWasBlocking = IsP2BlockingOrBlockstun(prevMove);
                    }
                    WriteP2BlockStance(desiredStance);
                    SetPracticeBlockMode(desiredStance == 0 ? 0 : 2);
                    if (p2) {
                        if (prevBlk > 0) {
                            SafeWriteMemory(p2 + BLOCKSTUN_OFFSET, &prevBlk, sizeof(prevBlk));
                        }
                        if (prevWasBlocking) {
                            short curMove=0; SafeReadMemory(p2 + MOVE_ID_OFFSET, &curMove, sizeof(curMove));
                            if (!IsP2BlockingOrBlockstun(curMove)) {
                                SafeWriteMemory(p2 + MOVE_ID_OFFSET, &prevMove, sizeof(prevMove));
                            }
                        }
                    }
                    if (detailedLogging.load()) {
                        std::ostringstream os;
                        os << "[ADAPTIVE] stance change: P2 "
                           << (curStance==1?"crouch":"stand") << " -> "
                           << (desiredStance==1?"crouch":"stand")
                           << " | move=" << st
                           << " frame=" << fr
                           << " atk=0x" << std::hex << std::uppercase << atkFlags
                           << " hit=0x" << hitFlags
                           << " grd=0x" << grdFlags
                           << std::dec
                           << " t=" << GetTickCount64();
                        LogOut(os.str(), true);
                    }
                }
            } else {
                // Best-effort preservation when fields can't be read
                uintptr_t p2 = GetPlayerBase(2);
                short prevBlk=0; short prevMove=0; bool prevWasBlocking=false;
                if (p2) {
                    SafeReadMemory(p2 + BLOCKSTUN_OFFSET, &prevBlk, sizeof(prevBlk));
                    SafeReadMemory(p2 + MOVE_ID_OFFSET, &prevMove, sizeof(prevMove));
                    prevWasBlocking = IsP2BlockingOrBlockstun(prevMove);
                }
                WriteP2BlockStance(desiredStance);
                SetPracticeBlockMode(desiredStance == 0 ? 0 : 2);
                if (p2) {
                    if (prevBlk > 0) {
                        SafeWriteMemory(p2 + BLOCKSTUN_OFFSET, &prevBlk, sizeof(prevBlk));
                    }
                    if (prevWasBlocking) {
                        short curMove=0; SafeReadMemory(p2 + MOVE_ID_OFFSET, &curMove, sizeof(curMove));
                        if (!IsP2BlockingOrBlockstun(curMove)) {
                            SafeWriteMemory(p2 + MOVE_ID_OFFSET, &prevMove, sizeof(prevMove));
                        }
                    }
                }
            }

            s_lastAdaptiveMs = now;
            g_adaptiveForceTick.store(false);
        }
    }
}

// Overload using unified per-frame sample (delegates; preserves existing timing/ordering)
void MonitorDummyAutoBlock(const PerFrameSample& sample) {
    MonitorDummyAutoBlock(sample.moveID1, sample.moveID2, sample.prevMoveID1, sample.prevMoveID2);
}
