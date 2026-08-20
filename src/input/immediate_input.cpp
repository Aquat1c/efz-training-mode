#include "../include/input/immediate_input.h"
#include "../include/input/input_core.h"
#include "../include/input/input_hook.h"
#include "../include/input/auto_action_motion_transaction.h"
#include "../include/input/injection_control.h"
#include "../include/input/input_buffer.h"
#include "../include/input/input_freeze.h"
#include "../include/input/motion_system.h"
#include "../include/input/scoped_input_reservation.h"
#include "../include/core/logger.h"
#include "../include/core/memory.h"
#include "../include/game/auto_action.h"
#include "../include/game/macro_controller.h"
#include "../include/utils/utilities.h" // for g_onlineModeActive
#include <thread>
#include <chrono>
#include <mutex>

namespace ImmediateInput {

namespace {

class ImmediateWriteLease {
public:
    explicit ImmediateWriteLease(int playerNum)
        : playerNum_(playerNum), acquired_(
              TryAcquireImmediateInputWriteLease(playerNum)) {}
    ~ImmediateWriteLease() {
        if (acquired_) ReleaseImmediateInputWriteLease(playerNum_);
    }
    explicit operator bool() const { return acquired_; }

private:
    int playerNum_{0};
    bool acquired_{false};
};

} // namespace

static std::thread s_thread;
static std::atomic<bool> s_running{false};
static std::atomic<bool> s_stop{false};
static std::atomic<uint64_t> s_workerGeneration{0};
static std::mutex s_lifecycleMutex;
static constexpr auto kVisualFrameDuration = std::chrono::nanoseconds(15625000); // exact 1/64 s

struct Slot {
    std::atomic<uint8_t> desired{0};
    std::atomic<int>     ticks{0};      // remaining visual ticks for timed press
    std::atomic<uint8_t> lastWritten{0};
    std::atomic<bool>    needNeutralEdge{false};
};

static Slot s_slot[3]; // 1=P1, 2=P2
static std::atomic<uint64_t> s_tutorialToken[3]{};
static std::atomic<uint64_t> s_nextTutorialToken{1};

// Caller holds g_p2ControlMutex.  ImmediateInput is a raw-register owner; it
// must not arm a delayed desired value behind another producer and let the
// detached worker deliver it after that producer releases the lane.
static bool OtherInputOwnerActive(int playerNum) {
    return (playerNum == 1 && IsP1StartupNeutralGateActive()) ||
           IsScopedInputReserved(playerNum) ||
           IsAutoActionMotionTransactionActive(playerNum) ||
           IsAutoActionNormalPulseActive(playerNum) ||
           g_manualInputOverride[playerNum].load(std::memory_order_acquire) ||
           g_pollOverrideActive[playerNum].load(std::memory_order_acquire) ||
           GetMotionQueueSnapshot(playerNum).active ||
           TutorialMotionQueueLeaseActive(playerNum) ||
           TutorialBufferFreezeLeaseActive() ||
           (g_bufferFreezingActive.load(std::memory_order_acquire) &&
            (g_activeFreezePlayer.load(std::memory_order_acquire) == 0 ||
             g_activeFreezePlayer.load(std::memory_order_acquire) == playerNum)) ||
           (MacroController::IsExclusivePlayback() &&
            MacroController::GetPlaybackPlayer() == playerNum);
}

static uint64_t NextWorkerGeneration() {
    uint64_t generation =
        s_workerGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;
    if (generation == 0) {
        generation =
            s_workerGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;
    }
    return generation;
}

static bool OwnsWorkerGeneration(uint64_t generation) {
    return generation != 0 &&
        s_workerGeneration.load(std::memory_order_acquire) == generation;
}

static void Worker(uint64_t generation) {
    // CRITICAL: Never run during online mode
    if (g_onlineModeActive.load(std::memory_order_acquire) ||
        !OwnsWorkerGeneration(generation)) {
        if (OwnsWorkerGeneration(generation)) {
            s_running.store(false, std::memory_order_release);
        }
        return;
    }

    using clock = std::chrono::steady_clock;
    auto next = clock::now();
    if (detailedLogging.load()) {
        LogOut("[IMMEDIATE_INPUT] Worker started with exact 64 Hz cadence (" +
               std::to_string(kVisualFrameDuration.count()) + " ns per visual frame)", true);
    }

    while (!s_stop.load(std::memory_order_acquire) &&
           OwnsWorkerGeneration(generation)) {
        // Exit if online mode is detected
        if (g_onlineModeActive.load(std::memory_order_acquire)) {
            break;
        }

        auto now = clock::now();
        if (now < next) {
            std::this_thread::sleep_for(next - now);
        }
        next += kVisualFrameDuration;

        // Netplay may have been published while this detached worker slept.
        // Do not let the remainder of the old offline iteration write once the
        // online owner is live.
        if (g_onlineModeActive.load(std::memory_order_acquire) ||
            s_stop.load(std::memory_order_acquire) ||
            !OwnsWorkerGeneration(generation)) {
            break;
        }

        for (int p = 1; p <= 2; ++p) {
            // A hook-owned AI normal must survive from its post-AI write to
            // EFZ's later character consumer. Pause this wall-clock writer for
            // the complete raw pulse/release transaction instead of racing it.
            ImmediateWriteLease writeLease(p);
            if (!writeLease) continue;
            if (g_onlineModeActive.load(std::memory_order_acquire) ||
                s_stop.load(std::memory_order_acquire) ||
                !OwnsWorkerGeneration(generation)) {
                break;
            }

            // Use acquire to ensure we see desired before checking ticks
            uint8_t curDesired = s_slot[p].desired.load(std::memory_order_acquire);
            int t = s_slot[p].ticks.load(std::memory_order_relaxed);
            uint8_t last = s_slot[p].lastWritten.load(std::memory_order_relaxed);
            bool needNeutral = s_slot[p].needNeutralEdge.exchange(false);

            // Timed press handling
            if (t > 0) {
                // Ensure the desired is asserted; if it changed between ticks, ensure edge
                if (curDesired == 0) {
                    // no-op: nothing to press
                    s_slot[p].ticks.store(0, std::memory_order_relaxed);
                    continue;
                }
                // If we just wrote a non-zero last time, keep holding
                // On transition or periodic re-press, create an edge by forcing a neutral first
                if (last != 0 && last == curDesired) {
                    // keep holding (reassert to ensure game sees it)
                    WritePlayerInputImmediate(p, curDesired);
                    
                    // Debug: Log wake jump writes
                    if ((curDesired & GAME_INPUT_UP) != 0) {
                        uint16_t moveID = 0;
                        uintptr_t playerPtr = GetPlayerPointer(p);
                        if (playerPtr) {
                            SafeReadMemory(playerPtr + MOVE_ID_OFFSET, &moveID, sizeof(uint16_t));
                        }
                        LogOut("[IMMEDIATE_INPUT] Holding UP for P" + std::to_string(p) + 
                               " mask=" + std::to_string(curDesired) + 
                               " ticksLeft=" + std::to_string(t-1) +
                               " moveID=" + std::to_string(moveID), true);
                    }
                } else {
                    // ensure a neutral edge when transitioning to a new non-zero
                    if (last != 0) {
                        WritePlayerInputImmediate(p, 0);
                    }
                    WritePlayerInputImmediate(p, curDesired);
                    
                    // Debug: Log initial wake jump write
                    if ((curDesired & GAME_INPUT_UP) != 0) {
                        uint16_t moveID = 0;
                        uintptr_t playerPtr = GetPlayerPointer(p);
                        if (playerPtr) {
                            SafeReadMemory(playerPtr + MOVE_ID_OFFSET, &moveID, sizeof(uint16_t));
                        }
                        LogOut("[IMMEDIATE_INPUT] Initial UP write for P" + std::to_string(p) + 
                               " mask=" + std::to_string(curDesired) + 
                               " ticksLeft=" + std::to_string(t-1) +
                               " moveID=" + std::to_string(moveID), true);
                    }
                }
                s_slot[p].lastWritten.store(curDesired, std::memory_order_relaxed);
                s_slot[p].ticks.store(t - 1, std::memory_order_relaxed);
                if (t - 1 <= 0) {
                    // auto-release to neutral on completion
                    WritePlayerInputImmediate(p, 0);
                    s_slot[p].lastWritten.store(0, std::memory_order_relaxed);
                    s_slot[p].desired.store(0, std::memory_order_relaxed);
                }
                continue;
            }

            // Continuous hold handling
            if (needNeutral && curDesired != 0) {
                // Force a neutral edge before reasserting non-zero mask
                WritePlayerInputImmediate(p, 0);
                s_slot[p].lastWritten.store(0, std::memory_order_relaxed);
                // Next loop will assert the non-zero
            }

            if (curDesired != 0) {
                // Maintain hold; ensure the mask is reasserted periodically since the game may clear per frame
                if (last != 0 && last != curDesired) {
                    WritePlayerInputImmediate(p, 0);
                }
                WritePlayerInputImmediate(p, curDesired);
                s_slot[p].lastWritten.store(curDesired, std::memory_order_relaxed);
            } else {
                if (last != 0) {
                    WritePlayerInputImmediate(p, 0);
                    s_slot[p].lastWritten.store(0, std::memory_order_relaxed);
                }
            }
        }
    }

    // On an ordinary offline stop, release to neutral. At the online ownership
    // boundary only retire private bookkeeping: a delayed neutral write is
    // still an unauthorized input write in netplay.
    // Stop/restart invalidates the generation before synchronous cleanup. An
    // obsolete detached worker must not clear a newer worker's desired state
    // or issue a late neutral write.
    if (!OwnsWorkerGeneration(generation)) return;

    const bool onlineExit = g_onlineModeActive.load(std::memory_order_acquire);
    for (int p = 1; p <= 2; ++p) {
        if (!onlineExit) {
            ImmediateWriteLease writeLease(p);
            if (writeLease) WritePlayerInputImmediate(p, 0);
        }
        s_slot[p].lastWritten.store(0, std::memory_order_relaxed);
        s_slot[p].desired.store(0, std::memory_order_relaxed);
        s_slot[p].ticks.store(0, std::memory_order_relaxed);
        s_slot[p].needNeutralEdge.store(false, std::memory_order_relaxed);
    }
    if (OwnsWorkerGeneration(generation)) {
        s_running.store(false, std::memory_order_release);
    }
}

void Start() {
    // Match netplay publication's lock order.  The recheck under the shared
    // barrier prevents a detached writer from starting in the hand-off gap.
    std::lock_guard<std::recursive_mutex> controlLock(g_p2ControlMutex);
    std::lock_guard<std::mutex> lifecycleLock(s_lifecycleMutex);
    if (g_onlineModeActive.load(std::memory_order_acquire) ||
        s_running.load(std::memory_order_acquire)) return;

    const uint64_t generation = NextWorkerGeneration();
    s_stop.store(false, std::memory_order_release);
    for (int p = 1; p <= 2; ++p) {
        s_slot[p].desired.store(0);
        s_slot[p].ticks.store(0);
        s_slot[p].lastWritten.store(0);
        s_slot[p].needNeutralEdge.store(false);
    }
    s_running.store(true, std::memory_order_release);
    s_thread = std::thread([generation]{ Worker(generation); });
    s_thread.detach();
}

void Stop() {
    // Wait out any in-progress raw write, then invalidate its generation. The
    // detached worker can exit later, but it can no longer touch registers or
    // erase state belonging to a subsequent Start().
    std::lock_guard<std::recursive_mutex> controlLock(g_p2ControlMutex);
    std::lock_guard<std::mutex> lifecycleLock(s_lifecycleMutex);
    const bool wasRunning = s_running.exchange(false, std::memory_order_acq_rel);
    s_stop.store(true, std::memory_order_release);
    (void)NextWorkerGeneration();

    for (int p = 1; p <= 2; ++p) {
        s_slot[p].desired.store(0, std::memory_order_relaxed);
        s_slot[p].ticks.store(0, std::memory_order_relaxed);
        s_slot[p].needNeutralEdge.store(false, std::memory_order_relaxed);
        if (!g_onlineModeActive.load(std::memory_order_acquire)) {
            ImmediateWriteLease writeLease(p);
            if (writeLease) WritePlayerInputImmediate(p, 0);
        }
        s_slot[p].lastWritten.store(0, std::memory_order_relaxed);
    }
    if (wasRunning && detailedLogging.load()) {
        LogOut("[IMMEDIATE_INPUT] Worker stopped and generation retired", true);
    }
}

bool IsRunning() { return s_running.load(); }

static void SetImpl(int playerNum, uint8_t mask) {
    if (playerNum < 1 || playerNum > 2) return;
    // If mask is non-zero and equals the last written, request a neutral edge once
    uint8_t last = s_slot[playerNum].lastWritten.load(std::memory_order_relaxed);
    if (mask != 0 && last == mask) {
        s_slot[playerNum].needNeutralEdge.store(true, std::memory_order_relaxed);
    }
    s_slot[playerNum].ticks.store(0, std::memory_order_relaxed); // cancel timed press
    s_slot[playerNum].desired.store(mask, std::memory_order_relaxed);
}

bool Set(int playerNum, uint8_t mask) {
    if (playerNum < 1 || playerNum > 2 ||
        s_tutorialToken[playerNum].load(std::memory_order_acquire) != 0) return false;
    std::unique_lock<std::recursive_mutex> p2ControlLock(g_p2ControlMutex);
    if (s_tutorialToken[playerNum].load(std::memory_order_acquire) != 0 ||
        g_onlineModeActive.load(std::memory_order_acquire) ||
        OtherInputOwnerActive(playerNum)) return false;
    // A non-zero continuous source must win admission now; do not cache it
    // behind a normal/motion owner and let it appear later out of context.
    if (mask != 0) {
        ImmediateWriteLease writeLease(playerNum);
        if (!writeLease) return false;
        SetImpl(playerNum, mask);
        return true;
    }
    SetImpl(playerNum, mask);
    return true;
}

static bool PressForImpl(int playerNum, uint8_t mask, int ticks) {
    if (playerNum < 1 || playerNum > 2 || ticks <= 0) return false;
    const uint8_t last = s_slot[playerNum].lastWritten.load(std::memory_order_relaxed);
    ImmediateWriteLease writeLease(playerNum);
    if (!writeLease) return false;
    if (mask != 0) {
        // Prime the immediate register now so delayed actions do not wait for the next 64 Hz worker tick.
        const bool neutralOk = last == 0 || WritePlayerInputImmediate(playerNum, 0);
        const bool pressOk = WritePlayerInputImmediate(playerNum, mask);
        if (!neutralOk || !pressOk) {
            // Do not retain a rejected one-shot for the worker to deliver at a
            // later, unrelated actionable window.
            (void)WritePlayerInputImmediate(playerNum, 0);
            s_slot[playerNum].lastWritten.store(0, std::memory_order_relaxed);
            return false;
        }
        s_slot[playerNum].lastWritten.store(mask, std::memory_order_relaxed);
        if (detailedLogging.load()) {
            LogOut("[IMMEDIATE_INPUT] Primed timed press for P" + std::to_string(playerNum) +
                   " mask=" + std::to_string(mask) +
                   " ticks=" + std::to_string(ticks), true);
        }
    }
    // Set desired first with release semantics, then arm the remaining worker ticks.
    s_slot[playerNum].desired.store(mask, std::memory_order_release);
    s_slot[playerNum].ticks.store((ticks > 1) ? (ticks - 1) : 1, std::memory_order_relaxed);
    return true;
}

bool PressFor(int playerNum, uint8_t mask, int ticks) {
    if (playerNum < 1 || playerNum > 2 ||
        s_tutorialToken[playerNum].load(std::memory_order_acquire) != 0 ||
        IsScopedInputReserved(playerNum)) return false;
    std::unique_lock<std::recursive_mutex> p2ControlLock(g_p2ControlMutex);
    if (s_tutorialToken[playerNum].load(std::memory_order_acquire) != 0 ||
        g_onlineModeActive.load(std::memory_order_acquire) ||
        OtherInputOwnerActive(playerNum)) return false;
    return PressForImpl(playerNum, mask, ticks);
}

static void ClearImpl(int playerNum) {
    if (playerNum < 1 || playerNum > 2) return;
    s_slot[playerNum].desired.store(0, std::memory_order_relaxed);
    s_slot[playerNum].ticks.store(0, std::memory_order_relaxed);
    // Immediate neutral write will occur on the next tick; proactively clear
    // now unless an AI normal owns the registers through its consumer window.
    ImmediateWriteLease writeLease(playerNum);
    if (writeLease) WritePlayerInputImmediate(playerNum, 0);
    s_slot[playerNum].lastWritten.store(0, std::memory_order_relaxed);
}

void Clear(int playerNum) {
    if (playerNum < 1 || playerNum > 2 ||
        s_tutorialToken[playerNum].load(std::memory_order_acquire) != 0) return;
    std::unique_lock<std::recursive_mutex> controlLock(g_p2ControlMutex);
    if (s_tutorialToken[playerNum].load(std::memory_order_acquire) != 0) return;
    ClearImpl(playerNum);
}

uint8_t GetCurrentDesired(int playerNum) {
    if (playerNum < 1 || playerNum > 2) return 0;
    return s_slot[playerNum].desired.load(std::memory_order_relaxed);
}

int GetRemainingTicks(int playerNum) {
    if (playerNum < 1 || playerNum > 2) return 0;
    return s_slot[playerNum].ticks.load(std::memory_order_relaxed);
}

bool RetireCompletedBookkeeping(int playerNum) {
    if (playerNum < 1 || playerNum > 2) return false;
    if (s_slot[playerNum].desired.load(std::memory_order_acquire) != 0 ||
        s_slot[playerNum].ticks.load(std::memory_order_acquire) != 0) {
        return false;
    }
    s_slot[playerNum].lastWritten.store(0, std::memory_order_release);
    s_slot[playerNum].needNeutralEdge.store(false,
                                             std::memory_order_release);
    return true;
}

bool AcquireTutorialLease(int playerNum, uint64_t& tokenOut) {
    tokenOut = 0;
    if (playerNum < 1 || playerNum > 2) return false;
    std::unique_lock<std::recursive_mutex> p2ControlLock(g_p2ControlMutex);
    if (s_tutorialToken[playerNum].load(std::memory_order_acquire) != 0 ||
        g_onlineModeActive.load(std::memory_order_acquire) ||
        OtherInputOwnerActive(playerNum)) return false;
    if (s_slot[playerNum].desired.load(std::memory_order_acquire) != 0 ||
        s_slot[playerNum].ticks.load(std::memory_order_acquire) != 0) return false;
    uint64_t expected = 0;
    uint64_t token = s_nextTutorialToken.fetch_add(1, std::memory_order_relaxed);
    if (token == 0) token = s_nextTutorialToken.fetch_add(1, std::memory_order_relaxed);
    if (!s_tutorialToken[playerNum].compare_exchange_strong(
            expected, token, std::memory_order_acq_rel)) return false;
    tokenOut = token;
    return true;
}

bool PressForTutorial(int playerNum, uint64_t token, uint8_t mask, int ticks) {
    if (playerNum < 1 || playerNum > 2 || token == 0 || ticks <= 0 ||
        s_tutorialToken[playerNum].load(std::memory_order_acquire) != token)
        return false;
    std::unique_lock<std::recursive_mutex> p2ControlLock(g_p2ControlMutex);
    if (s_tutorialToken[playerNum].load(std::memory_order_acquire) != token ||
        g_onlineModeActive.load(std::memory_order_acquire) ||
        OtherInputOwnerActive(playerNum)) return false;
    return PressForImpl(playerNum, mask, ticks);
}

void ReleaseTutorialLease(int playerNum, uint64_t token) {
    if (playerNum < 1 || playerNum > 2 || token == 0 ||
        s_tutorialToken[playerNum].load(std::memory_order_acquire) != token)
        return;
    std::unique_lock<std::recursive_mutex> controlLock(g_p2ControlMutex);
    if (s_tutorialToken[playerNum].load(std::memory_order_acquire) != token) return;
    s_slot[playerNum].desired.store(0, std::memory_order_relaxed);
    s_slot[playerNum].ticks.store(0, std::memory_order_relaxed);
    s_slot[playerNum].needNeutralEdge.store(false, std::memory_order_relaxed);
    ImmediateWriteLease writeLease(playerNum);
    if (writeLease) WritePlayerInputImmediate(playerNum, 0);
    s_slot[playerNum].lastWritten.store(0, std::memory_order_relaxed);
    uint64_t expected = token;
    (void)s_tutorialToken[playerNum].compare_exchange_strong(
        expected, 0, std::memory_order_acq_rel);
}

bool TutorialLeaseActive(int playerNum) {
    return playerNum >= 1 && playerNum <= 2 &&
        s_tutorialToken[playerNum].load(std::memory_order_acquire) != 0;
}

} // namespace ImmediateInput
