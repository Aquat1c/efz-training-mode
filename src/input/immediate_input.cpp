#include "../include/input/immediate_input.h"
#include "../include/input/input_core.h"
#include "../include/core/logger.h"
#include <thread>
#include <chrono>

namespace ImmediateInput {

static std::thread s_thread;
static std::atomic<bool> s_running{false};
static std::atomic<bool> s_stop{false};

struct Slot {
    std::atomic<uint8_t> desired{0};
    std::atomic<int>     ticks{0};      // remaining visual ticks for timed press
    std::atomic<uint8_t> lastWritten{0};
    std::atomic<bool>    needNeutralEdge{false};
};

static Slot s_slot[3]; // 1=P1, 2=P2

static void Worker() {
    using clock = std::chrono::steady_clock;
    const auto frameDur = std::chrono::milliseconds(1000 / 64); // ~15.625ms
    auto next = clock::now();

    while (!s_stop.load(std::memory_order_relaxed)) {
        auto now = clock::now();
        if (now < next) {
            std::this_thread::sleep_for(next - now);
        }
        next += frameDur;

        for (int p = 1; p <= 2; ++p) {
            uint8_t curDesired = s_slot[p].desired.load(std::memory_order_relaxed);
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
                    // keep holding
                    WritePlayerInputImmediate(p, curDesired);
                } else {
                    // ensure a neutral edge when transitioning to a new non-zero
                    if (last != 0) {
                        WritePlayerInputImmediate(p, 0);
                    }
                    WritePlayerInputImmediate(p, curDesired);
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
                // Maintain hold; if last was different, insert edge
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

    // On exit, ensure neutral
    for (int p = 1; p <= 2; ++p) {
        WritePlayerInputImmediate(p, 0);
        s_slot[p].lastWritten.store(0, std::memory_order_relaxed);
        s_slot[p].desired.store(0, std::memory_order_relaxed);
        s_slot[p].ticks.store(0, std::memory_order_relaxed);
        s_slot[p].needNeutralEdge.store(false, std::memory_order_relaxed);
    }
}

void Start() {
    bool expected = false;
    if (!s_running.compare_exchange_strong(expected, true)) return;
    s_stop.store(false);
    for (int p = 1; p <= 2; ++p) {
        s_slot[p].desired.store(0);
        s_slot[p].ticks.store(0);
        s_slot[p].lastWritten.store(0);
        s_slot[p].needNeutralEdge.store(false);
    }
    s_thread = std::thread([]{ Worker(); });
    s_thread.detach();
}

void Stop() {
    if (!s_running.exchange(false)) return;
    s_stop.store(true);
}

bool IsRunning() { return s_running.load(); }

void Set(int playerNum, uint8_t mask) {
    if (playerNum < 1 || playerNum > 2) return;
    // If mask is non-zero and equals the last written, request a neutral edge once
    uint8_t last = s_slot[playerNum].lastWritten.load(std::memory_order_relaxed);
    if (mask != 0 && last == mask) {
        s_slot[playerNum].needNeutralEdge.store(true, std::memory_order_relaxed);
    }
    s_slot[playerNum].ticks.store(0, std::memory_order_relaxed); // cancel timed press
    s_slot[playerNum].desired.store(mask, std::memory_order_relaxed);
}

void PressFor(int playerNum, uint8_t mask, int ticks) {
    if (playerNum < 1 || playerNum > 2 || ticks <= 0) return;
    s_slot[playerNum].desired.store(mask, std::memory_order_relaxed);
    s_slot[playerNum].ticks.store(ticks, std::memory_order_relaxed);
}

void Clear(int playerNum) {
    if (playerNum < 1 || playerNum > 2) return;
    s_slot[playerNum].desired.store(0, std::memory_order_relaxed);
    s_slot[playerNum].ticks.store(0, std::memory_order_relaxed);
    // Immediate neutral write will occur on the next tick; proactively clear now
    WritePlayerInputImmediate(playerNum, 0);
    s_slot[playerNum].lastWritten.store(0, std::memory_order_relaxed);
}

uint8_t GetCurrentDesired(int playerNum) {
    if (playerNum < 1 || playerNum > 2) return 0;
    return s_slot[playerNum].desired.load(std::memory_order_relaxed);
}

} // namespace ImmediateInput
