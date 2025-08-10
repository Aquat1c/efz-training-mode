#include "../include/input/input_hook.h"
#include "../include/input/input_motion.h"
#include "../include/core/memory.h"
#include "../include/core/logger.h"
#include "../include/core/constants.h"
#include "../include/utils/utilities.h"

#include "../3rdparty/minhook/include/MinHook.h"
#include "../include/game/practice_patch.h"
#include <windows.h>
#include <vector>
#include <atomic>
// Add this static variable to track the previous frame's button mask for edge detection.
static uint8_t g_lastInjectedMask[3] = {0, 0, 0}; // Index 0 unused, 1 for P1, 2 for P2

// Function pointer for the original game function we are hooking.
typedef int(__thiscall* tProcessCharacterInput)(int characterPtr);
tProcessCharacterInput oProcessCharacterInput = nullptr;

// The address of the function to hook, relative to efz.exe's base address.
// This is the entry point for a character's input processing for the frame.
// CORRECTED ADDRESS: 0x411BE0 -> relative offset 0x11BE0
const uintptr_t PROCESS_INPUTS_FUNC_OFFSET = 0x11BE0;

// Our custom function that will be called instead of the original.
// We use __fastcall for __thiscall hooks from MinHook.
int __fastcall HookedProcessCharacterInput(int characterPtr, int edx) {
    // Add state validation
    static int lastGameState = -1;
    uintptr_t gameStateAddr = GetEFZBase() + 0x1A0000; // Example offset - adjust based on your findings
    int currentGameState = 0;
    SafeReadMemory(gameStateAddr, &currentGameState, sizeof(int));
    
    if (currentGameState != lastGameState) {
        LogOut("[HOOK] Game state changed: " + std::to_string(lastGameState) + 
               " -> " + std::to_string(currentGameState), true);
        lastGameState = currentGameState;
    }
    
    // Determine if this is P1 or P2
    uintptr_t p1Ptr = GetPlayerPointer(1);
    uintptr_t p2Ptr = GetPlayerPointer(2);
    
    int playerNum = 0;
    if (characterPtr != 0 && characterPtr == p1Ptr) playerNum = 1;
    else if (characterPtr != 0 && characterPtr == p2Ptr) playerNum = 2;
    
    // CRITICAL: Ensure we're not processing during wrong game states
    // Game states where we should NOT inject inputs:
    // - Menu screens
    // - Loading screens  
    // - Victory screens
    // Add your game state checks here
    
    bool shouldInject = false;
    if (playerNum > 0) {
        if (g_manualInputOverride[playerNum].load()) {
            shouldInject = true;
        } else if (playerNum == 1 && p1QueueActive) {
            shouldInject = true;
        } else if (playerNum == 2 && p2QueueActive) {
            shouldInject = true;
        }
    }

    if (shouldInject) {
        uint8_t currentMask = 0;
        if (g_manualInputOverride[playerNum].load()) {
            currentMask = g_manualInputMask[playerNum].load();
        } else {
            std::vector<InputFrame>& queue = (playerNum == 1) ? p1InputQueue : p2InputQueue;
            int& queueIndex = (playerNum == 1) ? p1QueueIndex : p2QueueIndex;
            if (queueIndex >= 0 && (size_t)queueIndex < queue.size()) {
                currentMask = queue[queueIndex].inputMask;
            }
        }
        
        // Write to both immediate input and buffer
        WritePlayerInputImmediate(playerNum, currentMask);
        
        // CRITICAL: Only write to buffer if we're in active gameplay
        if (currentGameState == 2 || currentGameState == 3) { // Adjust these values
            WritePlayerInputToBuffer(playerNum, currentMask);
        }
        
        g_lastInjectedMask[playerNum] = currentMask;
    }
    
    // Reset the last injected mask for this player
    g_lastInjectedMask[playerNum] = 0;
    
    return oProcessCharacterInput(characterPtr);
}

void InstallInputHook() {
    uintptr_t base = GetEFZBase();
    if (!base) {
        LogOut("[INPUT_HOOK] Failed to get game base address. Hook not installed.", true);
        return;
    }

    // The actual address in memory is the game's base + the relative offset.
    uintptr_t targetAddr = base + PROCESS_INPUTS_FUNC_OFFSET;

    // REMOVED: MH_Initialize() is now called globally in dllmain.cpp

    // Create the hook.
    if (MH_CreateHook((LPVOID)targetAddr, &HookedProcessCharacterInput, (LPVOID*)&oProcessCharacterInput) != MH_OK) {
        LogOut("[INPUT_HOOK] Failed to create hook at address " + FormatHexAddress(targetAddr), true);
        return;
    }

    // Enable the hook.
    if (MH_EnableHook((LPVOID)targetAddr) != MH_OK) {
        LogOut("[INPUT_HOOK] Failed to enable hook.", true);
        return;
    }

    LogOut("[INPUT_HOOK] Successfully hooked game's input processing function at " + FormatHexAddress(targetAddr), true);
}

void RemoveInputHook() {
    uintptr_t base = GetEFZBase();
    if (base) {
        uintptr_t targetAddr = base + PROCESS_INPUTS_FUNC_OFFSET;
        MH_DisableHook((LPVOID)targetAddr);
        MH_RemoveHook((LPVOID)targetAddr); // Also explicitly remove the hook
    }
    // REMOVED: MH_Uninitialize() is now called globally in dllmain.cpp
    LogOut("[INPUT_HOOK] Input hook removed.", true);
}