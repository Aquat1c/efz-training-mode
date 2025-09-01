#include "../include/input/input_hook.h"
#include "../include/input/input_motion.h"
#include "../include/core/memory.h"
#include "../include/core/logger.h"
#include "../include/core/constants.h"
#include "../include/utils/utilities.h"

#include "../3rdparty/minhook/include/MinHook.h"
#include "../include/game/practice_patch.h"
#include "../include/input/input_buffer.h" // for g_bufferFreezingActive
#include "../include/input/immediate_input.h"
#include <windows.h>
#include <vector>
#include <atomic>
#include <sstream>
#include <iomanip>
#include <chrono>
#include "../include/input/immediate_input.h"

// Local helper to format a single byte as two-digit hex (uppercase)
static std::string FormatHexByte(uint8_t value) {
    std::ostringstream oss;
    oss << std::uppercase << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(value);
    return oss.str();
}
// Add this static variable to track the previous frame's button mask for edge detection.
static uint8_t g_lastInjectedMask[3] = {0, 0, 0}; // Index 0 unused, 1 for P1, 2 for P2
// Track whether we were bypassing original (buffered injection) last frame per player.
static bool g_wasBypassBuffered[3] = { false, false, false };

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
    // Determine if this is P1 or P2 by comparing the character object pointer.
    
    // --- CORRECTED POINTER LOGIC ---
    // Use the reliable GetPlayerPointer utility instead of reading from the base offsets directly.
    uintptr_t p1Ptr = GetPlayerPointer(1);
    uintptr_t p2Ptr = GetPlayerPointer(2);
    // --- END CORRECTION ---

    int playerNum = 0;
    if (characterPtr != 0 && characterPtr == p1Ptr) playerNum = 1;
    else if (characterPtr != 0 && characterPtr == p2Ptr) playerNum = 2;

    // Safety: if we cannot identify the player, defer to the original immediately
    if (playerNum == 0) {
        return oProcessCharacterInput(characterPtr);
    }

    // Do not inject while a buffer-freeze is active for THIS player; let the game's
    // original logic read the frozen buffer/index without interference.
    if (g_bufferFreezingActive.load()) {
        int freezeOwner = g_activeFreezePlayer.load();
        if (freezeOwner == playerNum || freezeOwner == 0) {
            if (freezeOwner == playerNum) {
                // Throttle this diagnostic to avoid spamming every frame while freeze is active
                static std::chrono::steady_clock::time_point s_lastSkipLogAt[3] = { {}, {}, {} };
                auto now = std::chrono::steady_clock::now();
                bool timeOk = (s_lastSkipLogAt[playerNum].time_since_epoch().count() == 0) ||
                              ((now - s_lastSkipLogAt[playerNum]) >= std::chrono::milliseconds(250));
                if (detailedLogging.load() && timeOk) {
                    LogOut(std::string("[INPUT_HOOK] Skipping injection for P") + std::to_string(playerNum) +
                           " due to active buffer-freeze (owner=P" + std::to_string(freezeOwner) + ")", true);
                    s_lastSkipLogAt[playerNum] = now;
                }
            }
            g_lastInjectedMask[playerNum] = 0;
            return oProcessCharacterInput(characterPtr);
        }
    }

    // REVERTED LOGIC: The queue system now handles all injection states.
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
        if (g_lastInjectedMask[playerNum] != currentMask) {
            static std::chrono::steady_clock::time_point lastLogAt[3] = { {}, {}, {} };
            auto now = std::chrono::steady_clock::now();
            bool timeOk = (lastLogAt[playerNum].time_since_epoch().count() == 0) || ((now - lastLogAt[playerNum]) >= std::chrono::seconds(2));
            if (detailedLogging.load() && timeOk) {
                LogOut(std::string("[INPUT_HOOK] Injecting for P") + std::to_string(playerNum) +
                       " mask=0x" + FormatHexByte(currentMask) +
                       (g_injectImmediateOnly[playerNum].load() ? " (immediate-only)" : " (buffered)"), true);
                lastLogAt[playerNum] = now;
            }
        }

        // Split behavior based on injection mode:
        //  - immediate-only: call original first so the game updates its own state, then override immediate regs.
        //  - buffered (queue/manual hold): bypass original to avoid buffer double-advance and write both immediate+buffer.
        if (g_injectImmediateOnly[playerNum].load()) {
            // For immediate-only, write before calling the game's processor so it sees our input this frame.
            WritePlayerInputImmediate(playerNum, currentMask);
            int ret = oProcessCharacterInput(characterPtr);
            g_lastInjectedMask[playerNum] = currentMask;
            g_wasBypassBuffered[playerNum] = false;
            return ret;
        } else {
            // Authoritative buffered injection: bypass original.
            WritePlayerInputImmediate(playerNum, currentMask);
            WritePlayerInputToBuffer(playerNum, currentMask);
            g_lastInjectedMask[playerNum] = currentMask;
            g_wasBypassBuffered[playerNum] = true;
            return 0;
        }
    } 
    
    // If no manual/queue injection is active, enforce the centralized immediate writer's desired mask
    // so that short presses/holds from ImmediateInput are not lost if the game overwrites registers.
    // We write before calling the game's processor (so it can see inputs this frame) and once more
    // after (to guard against late overwrites inside the function).
    {
        static uint8_t s_lastDesired[3] = {0, 0, 0};
        uint8_t desired = ImmediateInput::GetCurrentDesired(playerNum);
        bool haveDesired = (desired != 0) || (s_lastDesired[playerNum] != 0);
        if (haveDesired) {
            // Pre-write desired state
            WritePlayerInputImmediate(playerNum, desired);
            int ret = oProcessCharacterInput(characterPtr);
            // Post-write to ensure final state for this tick
            WritePlayerInputImmediate(playerNum, desired);
            s_lastDesired[playerNum] = desired;
            g_lastInjectedMask[playerNum] = desired;
            g_wasBypassBuffered[playerNum] = false;
            return ret;
        }
    }
    
    // --- NORMAL MODE ---
    // No input is queued for this player.
    // If we were bypassing last frame (buffered injection), force a one-time neutral clear to avoid latched immediate state.
    if (g_wasBypassBuffered[playerNum]) {
        WritePlayerInputImmediate(playerNum, 0);
        g_wasBypassBuffered[playerNum] = false;
    }
    // If the immediate input service has a desired mask, apply it just before original processing
    // so the game sees the current immediate inputs this frame (no buffer writes here).
    uint8_t desired = ImmediateInput::GetCurrentDesired(playerNum);
    if (desired != 0) {
        WritePlayerInputImmediate(playerNum, desired);
        g_lastInjectedMask[playerNum] = desired;
    } else {
        // Reset the last injected mask for this player to ensure a clean state on the next injection.
        g_lastInjectedMask[playerNum] = 0;
    }
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