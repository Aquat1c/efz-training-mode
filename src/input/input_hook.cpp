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
#include "../include/utils/minhook_utils.h"
#include "../include/game/auto_action.h"
#include "../include/input/injection_control.h"
#include <windows.h>
#include <vector>
#include <atomic>
#include <array>
#include <cstring>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <mutex>
#include "../include/input/immediate_input.h"

// Local helper to format a single byte as two-digit hex (uppercase)
static std::string FormatHexByte(uint8_t value) {
    std::ostringstream oss;
    oss << std::uppercase << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(value);
    return oss.str();
}
// Static variable to track the previous frame's button mask for edge detection.
static uint8_t g_lastInjectedMask[3] = {0, 0, 0}; // Index 0 unused, 1 for P1, 2 for P2
// Track whether we were bypassing original (buffered injection) last frame per player.
static bool g_wasBypassBuffered[3] = { false, false, false };

// Global control to force authoritative buffered injection (macro playback, etc.)
std::atomic<bool> g_forceBypass[3] = { false, false, false };

// Poll override: when active, our poll hook returns this mask instead of device state (index 0 unused).
std::atomic<bool> g_pollOverrideActive[3] = { false, false, false };
std::atomic<uint8_t> g_pollOverrideMask[3] = { 0, 0, 0 };
static std::atomic<uint32_t> g_pollOverrideHitCount[3] = { 0, 0, 0 };

void ResetInputPollOverrideHitCount(int playerNum) {
    if (playerNum != 1 && playerNum != 2) return;
    g_pollOverrideHitCount[playerNum].store(0, std::memory_order_release);
}

uint32_t GetInputPollOverrideHitCount(int playerNum) {
    if (playerNum != 1 && playerNum != 2) return 0;
    return g_pollOverrideHitCount[playerNum].load(std::memory_order_acquire);
}

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

namespace {
std::atomic<bool> s_inputHooksCreated{false};
std::atomic<bool> s_inputHooksEnabled{false};
std::atomic<int> s_processInputPlayerContext{0};
std::atomic<bool> s_loggedPollContextReroute[3] = { false, false, false };
std::atomic<bool> s_loggedPollOverrideHumanForce[3] = { false, false, false };
uintptr_t s_processTargetAddr = 0;
uintptr_t s_pollTargetAddr = 0;

bool ResolveInputHookTargets(uintptr_t& targetAddr, uintptr_t& pollAddr) {
    uintptr_t base = GetEFZBase();
    if (!base) {
        LogOut("[INPUT_HOOK] Failed to get game base address. Hook not installed.", true);
        return false;
    }

    targetAddr = base + PROCESS_INPUTS_FUNC_OFFSET;
    pollAddr = base + POLL_INPUT_STATE_FUNC_OFFSET;
    return true;
}

} // namespace

// Engine-only input ownership fallback. Revival's Practice SwitchPlayers hotkey swaps
// these same adjacent 16-byte live control maps in EFZ's input-manager object. Keeping
// the swap at this layer preserves P1's physical controls for the P2 character without
// changing character player indices or hijacking unrelated poll call sites.
static std::atomic<bool> g_loggedRoutingStateOnce{false};
namespace {

constexpr uintptr_t kInputManagerObjectRva = 0x003B0968u;
constexpr uintptr_t kP1BindingOffset = 448u;
constexpr size_t kBindingCount = 8u;

using BindingBlock = std::array<uint16_t, kBindingCount>;

struct BindingPair {
    BindingBlock p1{};
    BindingBlock p2{};
};
static_assert(sizeof(BindingPair) == 32, "EFZ control-map pair must remain exactly 32 bytes");

struct BindingSwapState {
    bool active{false};
    BindingPair baseline{};
};

std::mutex g_bindingSwapMutex;
BindingSwapState g_bindingSwapState;

bool BindingPairsEqual(const BindingPair& lhs, const BindingPair& rhs) {
    return std::memcmp(&lhs, &rhs, sizeof(BindingPair)) == 0;
}

BindingPair MakeSwappedBindingPair(const BindingPair& source) {
    BindingPair result{};
    result.p1 = source.p2;
    result.p2 = source.p1;
    return result;
}

uintptr_t GetBindingPairAddress() {
    const uintptr_t base = GetEFZBase();
    return base ? base + kInputManagerObjectRva + kP1BindingOffset : 0;
}

bool ReadBindingPair(BindingPair& outPair) {
    const uintptr_t address = GetBindingPairAddress();
    return address != 0 && SafeReadMemory(address, &outPair, sizeof(outPair));
}

bool WriteAndVerifyBindingPair(const BindingPair& pair) {
    const uintptr_t address = GetBindingPairAddress();
    if (!address || !SafeWriteMemory(address, &pair, sizeof(pair))) {
        return false;
    }

    BindingPair verify{};
    return SafeReadMemory(address, &verify, sizeof(verify))
        && BindingPairsEqual(verify, pair);
}

std::string FormatBindingBlock(const BindingBlock& block) {
    std::ostringstream oss;
    oss << std::hex << std::uppercase << std::setfill('0');
    for (size_t i = 0; i < block.size(); ++i) {
        if (i != 0) oss << ' ';
        oss << std::setw(4) << static_cast<unsigned int>(block[i]);
    }
    return oss.str();
}

struct ScopedProcessInputPlayerContext {
    explicit ScopedProcessInputPlayerContext(int playerNum)
        : previous(s_processInputPlayerContext.load(std::memory_order_relaxed)) {
        s_processInputPlayerContext.store(playerNum, std::memory_order_relaxed);
    }

    ~ScopedProcessInputPlayerContext() {
        s_processInputPlayerContext.store(previous, std::memory_order_relaxed);
    }

    int previous;
};

int CallOriginalProcessCharacterInputWithContext(int characterPtr, int playerNum) {
    ScopedProcessInputPlayerContext context(playerNum);
    return oProcessCharacterInput ? oProcessCharacterInput(characterPtr) : 0;
}

void EnsureHumanControlForActivePollOverride(int characterPtr, int playerNum) {
    if ((playerNum != 1 && playerNum != 2) || characterPtr == 0) {
        return;
    }
    if (!g_pollOverrideActive[playerNum].load(std::memory_order_relaxed)) {
        return;
    }

    uint32_t aiFlag = 0;
    if (!SafeReadMemory(static_cast<uintptr_t>(characterPtr) + AI_CONTROL_FLAG_OFFSET,
                        &aiFlag,
                        sizeof(aiFlag))) {
        return;
    }
    if (aiFlag == 0) {
        return;
    }

    const uint32_t humanControlFlag = 0;
    const bool okWrite = SafeWriteMemory(static_cast<uintptr_t>(characterPtr) + AI_CONTROL_FLAG_OFFSET,
                                         &humanControlFlag,
                                         sizeof(humanControlFlag));
    uint32_t after = aiFlag;
    SafeReadMemory(static_cast<uintptr_t>(characterPtr) + AI_CONTROL_FLAG_OFFSET,
                   &after,
                   sizeof(after));

    if (!s_loggedPollOverrideHumanForce[playerNum].exchange(true, std::memory_order_relaxed)
        || !okWrite
        || after != 0) {
        std::ostringstream oss;
        oss << "[INPUT_HOOK][POLL_OVERRIDE] Forced P" << playerNum
            << " AI flag human for engine poll path @0x" << std::hex
            << (static_cast<uintptr_t>(characterPtr) + AI_CONTROL_FLAG_OFFSET)
            << std::dec
            << " before=" << aiFlag
            << " after=" << after
            << " okWrite=" << (okWrite ? "1" : "0");
        LogOut(oss.str(), true);
    }
}

} // namespace

bool SetVanillaSwapInputRouting(bool enable) {
    std::lock_guard<std::mutex> lock(g_bindingSwapMutex);
    const bool previous = g_bindingSwapState.active;

    if (previous == enable) {
        if (!g_loggedRoutingStateOnce.exchange(true, std::memory_order_relaxed)) {
            LogOut(std::string("[INPUT_HOOK] Engine control-map swap already ")
                + (enable ? "ENABLED" : "DISABLED"), true);
        }
        return true;
    }

    BindingPair current{};
    if (!ReadBindingPair(current)) {
        LogOut("[INPUT_HOOK][ERROR] Cannot read EFZ P1/P2 live control maps", true);
        return false;
    }

    if (enable) {
        const BindingPair swapped = MakeSwappedBindingPair(current);
        if (!WriteAndVerifyBindingPair(swapped)) {
            LogOut("[INPUT_HOOK][ERROR] Failed to write/verify EFZ P1/P2 live control-map swap", true);
            return false;
        }

        g_bindingSwapState.baseline = current;
        g_bindingSwapState.active = true;
        g_loggedRoutingStateOnce.store(false, std::memory_order_relaxed);

        std::ostringstream oss;
        oss << "[INPUT_HOOK] Verified engine live control-map swap at 0x" << std::hex
            << std::uppercase << GetBindingPairAddress()
            << " | P1<-oldP2 [" << FormatBindingBlock(swapped.p1)
            << "] P2<-oldP1 [" << FormatBindingBlock(swapped.p2) << ']';
        LogOut(oss.str(), true);
        return true;
    }

    // Restore the exact baseline captured when the current swapped pair was applied.
    // If EFZ already restored it, leave memory untouched and only clear our state.
    const bool alreadyRestored = BindingPairsEqual(current, g_bindingSwapState.baseline);
    if (!alreadyRestored && !WriteAndVerifyBindingPair(g_bindingSwapState.baseline)) {
        LogOut("[INPUT_HOOK][ERROR] Failed to restore/verify EFZ P1/P2 live control maps", true);
        return false;
    }

    g_bindingSwapState = BindingSwapState{};
    g_loggedRoutingStateOnce.store(false, std::memory_order_relaxed);
    LogOut("[INPUT_HOOK] Restored verified default P1/P2 live control maps", true);
    return true;
}

// Our poll hook. Use __fastcall to match __thiscall trampoline signature.
static int __fastcall HookedPollPlayerInputState(int inputManagerPtr, int /*edx*/, unsigned int playerIndex)
{
    if (!s_inputHooksEnabled.load(std::memory_order_acquire)
        || g_onlineModeActive.load(std::memory_order_relaxed)) {
        return oPollPlayerInputState ? oPollPlayerInputState(inputManagerPtr, playerIndex) : 0;
    }

    // Engine uses 0 for P1 and 1 for P2; our globals use 1=P1, 2=P2.
    const unsigned int idxFromPollArg = (playerIndex <= 1) ? (playerIndex + 1) : 0;

    // Prefer the character currently being processed over the raw poll argument.
    // Side-switching paths can make EFZ poll P1's physical binding while it is
    // processing the P2 character; macro playback still needs to feed the P2
    // character in that case.
    const int processContext = s_processInputPlayerContext.load(std::memory_order_relaxed);
    const unsigned int idxFromProcessContext =
        (processContext == 1 || processContext == 2) ? static_cast<unsigned int>(processContext) : 0;

    if (idxFromProcessContext <= 2 && idxFromProcessContext != 0
        && g_pollOverrideActive[idxFromProcessContext].load(std::memory_order_relaxed)) {
        if (idxFromPollArg != 0 && idxFromPollArg != idxFromProcessContext
            && !s_loggedPollContextReroute[idxFromProcessContext].exchange(true, std::memory_order_relaxed)) {
            std::ostringstream oss;
            oss << "[INPUT_HOOK][POLL_OVERRIDE] Context-routed poll override: process=P"
                << idxFromProcessContext
                << " rawPollIndex=" << playerIndex
                << " rawPollPlayer=P" << idxFromPollArg;
            LogOut(oss.str(), true);
        }
        g_pollOverrideHitCount[idxFromProcessContext].fetch_add(1, std::memory_order_relaxed);
        return static_cast<int>(g_pollOverrideMask[idxFromProcessContext].load(std::memory_order_relaxed));
    }

    if (idxFromPollArg <= 2 && idxFromPollArg != 0
        && g_pollOverrideActive[idxFromPollArg].load(std::memory_order_relaxed)) {
        g_pollOverrideHitCount[idxFromPollArg].fetch_add(1, std::memory_order_relaxed);
        return static_cast<int>(g_pollOverrideMask[idxFromPollArg].load(std::memory_order_relaxed));
    }
    return oPollPlayerInputState ? oPollPlayerInputState(inputManagerPtr, playerIndex) : 0;
}

// Our custom function that will be called instead of the original.
// We use __fastcall for __thiscall hooks from MinHook.
int __fastcall HookedProcessCharacterInput(int characterPtr, int edx) {
    if (!s_inputHooksEnabled.load(std::memory_order_acquire)
        || g_onlineModeActive.load(std::memory_order_relaxed)) {
        return oProcessCharacterInput(characterPtr);
    }

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
            return CallOriginalProcessCharacterInputWithContext(characterPtr, playerNum);
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
                // Special handling for split injection (Immediate=0, Buffer=Macro):
                // If a poll override is active, prefer it for the buffer write.
                // This allows macro playback to suppress immediate inputs (via currentMask=0)
                // while still populating the buffer history (via pollOverride=Macro).
                uint8_t bufferMask = currentMask;
                if (g_pollOverrideActive[playerNum].load(std::memory_order_relaxed)) {
                    bufferMask = g_pollOverrideMask[playerNum].load(std::memory_order_relaxed);
                }
                WritePlayerInputToBuffer(playerNum, bufferMask);
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
            EnsureHumanControlForActivePollOverride(characterPtr, playerNum);
            int ret = CallOriginalProcessCharacterInputWithContext(characterPtr, playerNum);
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
            EnsureHumanControlForActivePollOverride(characterPtr, playerNum);
            int ret = CallOriginalProcessCharacterInputWithContext(characterPtr, playerNum);
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
        EnsureHumanControlForActivePollOverride(characterPtr, playerNum);
        int ret = CallOriginalProcessCharacterInputWithContext(characterPtr, playerNum);
        MaybePerformTailCleanup(playerNum);
        return ret;
    }
}

void InstallInputHook() {
    uintptr_t targetAddr = 0;
    uintptr_t pollAddr = 0;
    if (!ResolveInputHookTargets(targetAddr, pollAddr)) {
        return;
    }
    s_processTargetAddr = targetAddr;
    s_pollTargetAddr = pollAddr;

    if (!s_inputHooksCreated.load(std::memory_order_acquire)) {
        if (!MinHookUtils::CreateHook(reinterpret_cast<LPVOID>(targetAddr),
                                      reinterpret_cast<void*>(&HookedProcessCharacterInput),
                                      reinterpret_cast<void**>(&oProcessCharacterInput),
                                      "[INPUT_HOOK]",
                                      "processCharacterInput")) {
            return;
        }

        if (!MinHookUtils::CreateHook(reinterpret_cast<LPVOID>(pollAddr),
                                      reinterpret_cast<void*>(&HookedPollPlayerInputState),
                                      reinterpret_cast<void**>(&oPollPlayerInputState),
                                      "[INPUT_HOOK]",
                                      "pollPlayerInputState")) {
            (void)MinHookUtils::RemoveHook(reinterpret_cast<LPVOID>(targetAddr), "[INPUT_HOOK]", "processCharacterInput");
            return;
        }

        if (!MinHookUtils::EnableHook(reinterpret_cast<LPVOID>(targetAddr), "[INPUT_HOOK]", "processCharacterInput")
            || !MinHookUtils::EnableHook(reinterpret_cast<LPVOID>(pollAddr), "[INPUT_HOOK]", "pollPlayerInputState")) {
            return;
        }

        s_inputHooksCreated.store(true, std::memory_order_release);
        LogOut("[INPUT_HOOK] Hooked processCharacterInput at " + FormatHexAddress(targetAddr), true);
        LogOut("[INPUT_HOOK] Hooked pollPlayerInputState at " + FormatHexAddress(pollAddr), true);
    }

    if (g_onlineModeActive.load(std::memory_order_relaxed)) {
        SetInputHookActive(false);
        return;
    }

    SetInputHookActive(true);
}

void SetInputHookActive(bool active) {
    if (!s_inputHooksCreated.load(std::memory_order_acquire)) {
        if (active) {
            InstallInputHook();
        }
        return;
    }

    const bool currentlyEnabled = s_inputHooksEnabled.load(std::memory_order_acquire);
    if (currentlyEnabled == active) {
        return;
    }

    s_inputHooksEnabled.store(active, std::memory_order_release);
    LogOut(std::string("[INPUT_HOOK] Input hooks ") + (active ? "enabled" : "disabled"), true);
}

void RemoveInputHook() {
    (void)SetVanillaSwapInputRouting(false);
    if (s_processTargetAddr) {
        (void)MinHookUtils::DisableHook((LPVOID)s_processTargetAddr, "[INPUT_HOOK]", "processCharacterInput");
        (void)MinHookUtils::RemoveHook((LPVOID)s_processTargetAddr, "[INPUT_HOOK]", "processCharacterInput");
    }
    if (s_pollTargetAddr) {
        (void)MinHookUtils::DisableHook((LPVOID)s_pollTargetAddr, "[INPUT_HOOK]", "pollPlayerInputState");
        (void)MinHookUtils::RemoveHook((LPVOID)s_pollTargetAddr, "[INPUT_HOOK]", "pollPlayerInputState");
    }
    s_inputHooksEnabled.store(false, std::memory_order_release);
    s_inputHooksCreated.store(false, std::memory_order_release);
    s_processTargetAddr = 0;
    s_pollTargetAddr = 0;
    LogOut("[INPUT_HOOK] Input hook removed.", true);
}
