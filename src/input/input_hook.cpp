#include "../include/input/input_hook.h"
#include "../include/input/input_motion.h"
#include "../include/core/memory.h"
#include "../include/core/logger.h"
#include "../include/core/constants.h"
#include "../include/utils/utilities.h"

#include "../include/input/input_core.h"

#include "../3rdparty/minhook/include/MinHook.h"
#include "../include/game/practice_patch.h"
#include "../include/input/input_buffer.h" // for g_bufferFreezingActive
#include "../include/input/immediate_input.h"
#include "../include/game/auto_action.h"
#include "../include/input/injection_control.h"
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

// Global control to force authoritative buffered injection (macro playback, etc.)
std::atomic<bool> g_forceBypass[3] = { false, false, false };

// Poll override: when active, our poll hook returns this mask instead of device state (index 0 unused).
std::atomic<bool> g_pollOverrideActive[3] = { false, false, false };
std::atomic<uint8_t> g_pollOverrideMask[3] = { 0, 0, 0 };

// Arming state for motion-token neutralization and optional staged cleanup
static std::atomic<bool> s_armedNeutralize[3] = { false, false, false };
static std::atomic<bool> s_doFullCleanup[3] = { false, false, false };
static std::atomic<uint16_t> s_lastHead[3] = { 0xFFFF, 0xFFFF, 0xFFFF };
static std::atomic<int> s_headStable[3] = { 0, 0, 0 };

void InputHook_ArmTokenNeutralize(int playerNum, bool alsoDoFullCleanup) {
    if (playerNum != 1 && playerNum != 2) return;
    s_armedNeutralize[playerNum].store(true, std::memory_order_relaxed);
    s_doFullCleanup[playerNum].store(alsoDoFullCleanup, std::memory_order_relaxed);
    s_lastHead[playerNum].store(0xFFFF, std::memory_order_relaxed);
    s_headStable[playerNum].store(0, std::memory_order_relaxed);
    if (detailedLogging.load()) {
        LogOut(std::string("[INPUT_HOOK] Armed token neutralize for P") + std::to_string(playerNum) +
               (alsoDoFullCleanup?" + full cleanup":""), true);
    }
}

// Helper invoked at the end of processing to perform late neutralization/cleanup
static void MaybePerformTailCleanup(int playerNum) {
    if (playerNum != 1 && playerNum != 2) return;
    // Skip during buffer-freeze for this player (let freeze system manage state)
    if (g_bufferFreezingActive.load()) {
        int owner = g_activeFreezePlayer.load();
        if (owner == playerNum || owner == 0) return;
    }
    // One-shot token neutralization
    if (s_armedNeutralize[playerNum].load(std::memory_order_relaxed)) {
        (void)NeutralizeMotionToken(playerNum);
        s_armedNeutralize[playerNum].store(false, std::memory_order_relaxed);
    }
    // Staged cleanup once buffer head is stable for >=2 frames
    if (s_doFullCleanup[playerNum].load(std::memory_order_relaxed)) {
        uintptr_t pPtr = GetPlayerPointer(playerNum);
        if (!pPtr) return;
        uint16_t head = 0;
        if (!SafeReadMemory(pPtr + INPUT_BUFFER_INDEX_OFFSET, &head, sizeof(head))) return;
        uint16_t last = s_lastHead[playerNum].load(std::memory_order_relaxed);
        if (last == 0xFFFF || head != last) {
            s_lastHead[playerNum].store(head, std::memory_order_relaxed);
            s_headStable[playerNum].store(0, std::memory_order_relaxed);
            return;
        }
        int stable = s_headStable[playerNum].fetch_add(1, std::memory_order_relaxed) + 1;
        if (stable >= 2) {
            (void)FullCleanupAfterToggle(playerNum);
            s_doFullCleanup[playerNum].store(false, std::memory_order_relaxed);
            s_lastHead[playerNum].store(0xFFFF, std::memory_order_relaxed);
            s_headStable[playerNum].store(0, std::memory_order_relaxed);
        }
    }
}

// Function pointer for the original game function we are hooking.
typedef int(__thiscall* tProcessCharacterInput)(int characterPtr);
tProcessCharacterInput oProcessCharacterInput = nullptr;

// The address of the function to hook, relative to efz.exe's base address.
// This is the entry point for a character's input processing for the frame.
// CORRECTED ADDRESS: 0x411BE0 -> relative offset 0x11BE0
const uintptr_t PROCESS_INPUTS_FUNC_OFFSET = 0x11BE0;

// Hook: pollPlayerInputState(inputManager, playerIndex) → returns 8-bit unified mask
// RVA from efz.exe base: 0x0040CD00 → offset 0x0CD00
typedef int(__thiscall* tPollPlayerInputState)(int inputManagerPtr, unsigned int playerIndex);
static tPollPlayerInputState oPollPlayerInputState = nullptr;
static const uintptr_t POLL_INPUT_STATE_FUNC_OFFSET = 0x0CD00;

// Vanilla-only input routing swap flag
static std::atomic<bool> g_swapVanillaRouting{false};
static inline bool RevivalLoaded() { return GetModuleHandleA("EfzRevival.dll") != nullptr; }
void SetVanillaSwapInputRouting(bool enable) {
    g_swapVanillaRouting.store(enable, std::memory_order_relaxed);
    std::ostringstream oss; oss << "[INPUT_HOOK] Vanilla routing swap " << (enable?"ENABLED":"DISABLED");
    LogOut(oss.str(), true);
}

// Our poll hook. Use __fastcall to match __thiscall trampoline signature.
static int __fastcall HookedPollPlayerInputState(int inputManagerPtr, int /*edx*/, unsigned int playerIndex)
{
    // Engine uses 0 for P1 and 1 for P2; our globals use 1=P1, 2=P2.
    unsigned int idx = (playerIndex <= 1) ? (playerIndex + 1) : 0;
    if (idx <= 2) {
        if (g_pollOverrideActive[idx].load(std::memory_order_relaxed)) {
            return static_cast<int>(g_pollOverrideMask[idx].load(std::memory_order_relaxed));
        }
    }
    // Vanilla-only: swap control routing by flipping the polled index
    if (!RevivalLoaded() && g_swapVanillaRouting.load(std::memory_order_relaxed)) {
        unsigned int swapped = (playerIndex == 0) ? 1u : (playerIndex == 1 ? 0u : playerIndex);
        return oPollPlayerInputState ? oPollPlayerInputState(inputManagerPtr, swapped) : 0;
    }
    return oPollPlayerInputState ? oPollPlayerInputState(inputManagerPtr, playerIndex) : 0;
}

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

    // Tick-integrated auto-actions: run once per internal sub-tick before P1 processing
    if (g_tickIntegratedAutoActions.load() && playerNum == 1) {
        short move1 = 0, move2 = 0;
        // Read move IDs directly from character structs (word at +MOVE_ID_OFFSET)
        (void)SafeReadMemory((uintptr_t)characterPtr + MOVE_ID_OFFSET, &move1, sizeof(move1));
        uintptr_t otherPtr = (characterPtr == (int)p1Ptr) ? p2Ptr : p1Ptr;
        if (otherPtr) {
            (void)SafeReadMemory(otherPtr + MOVE_ID_OFFSET, &move2, sizeof(move2));
        } else {
            move2 = -1;
        }
        AutoActionsTick_Inline(move1, move2);
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
            // Intentionally skip tail cleanup during freeze
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
        // If forced bypass is enabled, always perform authoritative buffered injection.
        if (g_forceBypass[playerNum].load() || !g_injectImmediateOnly[playerNum].load()) {
            // Authoritative buffered injection: bypass original.
            WritePlayerInputImmediate(playerNum, currentMask);
            
            // CRITICAL FIX: Skip buffer writes for dash motions - they're written all at once when queued.
            // Frame-by-frame writes are too slow and get contaminated by neutral inputs from the game.
            int currentMotion = (playerNum == 1) ? p1CurrentMotionType : p2CurrentMotionType;
            bool isDashMotion = (currentMotion == MOTION_FORWARD_DASH || currentMotion == MOTION_BACK_DASH);
            if (!isDashMotion) {
                WritePlayerInputToBuffer(playerNum, currentMask);
            }
            
            g_lastInjectedMask[playerNum] = currentMask;
            g_wasBypassBuffered[playerNum] = true;
            MaybePerformTailCleanup(playerNum);
            return 0;
        } else {
            // For immediate-only, write before AND after calling the game's processor.
            // Some parts of the original function can overwrite immediate registers mid-frame.
            // Pre-write ensures the game reads our state; post-write stabilizes the final state for this tick.
            WritePlayerInputImmediate(playerNum, currentMask);
            int ret = oProcessCharacterInput(characterPtr);
            WritePlayerInputImmediate(playerNum, currentMask);
            g_lastInjectedMask[playerNum] = currentMask;
            g_wasBypassBuffered[playerNum] = false;
            MaybePerformTailCleanup(playerNum);
            return ret;
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
            MaybePerformTailCleanup(playerNum);
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
    {
        int ret = oProcessCharacterInput(characterPtr);
        MaybePerformTailCleanup(playerNum);
        return ret;
    }
}

void InstallInputHook() {
    uintptr_t base = GetEFZBase();
    if (!base) {
        LogOut("[INPUT_HOOK] Failed to get game base address. Hook not installed.", true);
        return;
    }

    // The actual address in memory is the game's base + the relative offset.
    uintptr_t targetAddr = base + PROCESS_INPUTS_FUNC_OFFSET;
    uintptr_t pollAddr   = base + POLL_INPUT_STATE_FUNC_OFFSET;

    // REMOVED: MH_Initialize() is now called globally in dllmain.cpp

    // Create the hooks.
    if (MH_CreateHook((LPVOID)targetAddr, &HookedProcessCharacterInput, (LPVOID*)&oProcessCharacterInput) != MH_OK) {
        LogOut("[INPUT_HOOK] Failed to create hook at address " + FormatHexAddress(targetAddr), true);
        return;
    }

    if (MH_CreateHook((LPVOID)pollAddr, &HookedPollPlayerInputState, (LPVOID*)&oPollPlayerInputState) != MH_OK) {
        LogOut("[INPUT_HOOK] Failed to create poll hook at address " + FormatHexAddress(pollAddr), true);
        // Clean up previously created hook to avoid partial state
        MH_RemoveHook((LPVOID)targetAddr);
        return;
    }

    // Enable the hooks.
    if (MH_EnableHook((LPVOID)targetAddr) != MH_OK) {
        LogOut("[INPUT_HOOK] Failed to enable hook.", true);
        MH_RemoveHook((LPVOID)targetAddr);
        MH_RemoveHook((LPVOID)pollAddr);
        return;
    }

    if (MH_EnableHook((LPVOID)pollAddr) != MH_OK) {
        LogOut("[INPUT_HOOK] Failed to enable poll hook.", true);
        MH_DisableHook((LPVOID)targetAddr);
        MH_RemoveHook((LPVOID)targetAddr);
        MH_RemoveHook((LPVOID)pollAddr);
        return;
    }

    LogOut("[INPUT_HOOK] Hooked processCharacterInput at " + FormatHexAddress(targetAddr), true);
    LogOut("[INPUT_HOOK] Hooked pollPlayerInputState at " + FormatHexAddress(pollAddr), true);
}

void RemoveInputHook() {
    uintptr_t base = GetEFZBase();
    if (base) {
        uintptr_t targetAddr = base + PROCESS_INPUTS_FUNC_OFFSET;
        uintptr_t pollAddr   = base + POLL_INPUT_STATE_FUNC_OFFSET;
        MH_DisableHook((LPVOID)targetAddr);
        MH_RemoveHook((LPVOID)targetAddr); // Also explicitly remove the hook
        MH_DisableHook((LPVOID)pollAddr);
        MH_RemoveHook((LPVOID)pollAddr);
    }
    // REMOVED: MH_Uninitialize() is now called globally in dllmain.cpp
    LogOut("[INPUT_HOOK] Input hook removed.", true);
}