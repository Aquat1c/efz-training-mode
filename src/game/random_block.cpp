#include "../include/game/random_block.h"
#include "../include/game/practice_patch.h" // SetPracticeAutoBlockEnabled/GetPracticeAutoBlockEnabled, GetDummyAutoBlockMode, DAB_*
#include "../include/core/logger.h"
#include "../include/game/game_state.h"
#include "../include/core/constants.h"
#include "../include/utils/utilities.h" // ::IsActionable
#include <cstdlib>
#include <atomic>
#include <mutex>

namespace RandomBlock {
    static std::atomic<bool> g_enabled{false};
    static std::atomic<int>  g_lastApplied{-1}; // -1 unknown, 0 off, 1 on
    // Pending OFF deferral when guard/inactionable
    static std::atomic<bool> g_pendingOff{false};
    // The physical Practice auto-block flag is randomized while this feature
    // owns it, but the authored dummy mode remains the source of truth.  Keep
    // the most recent desired window so disabling Random Block restores the
    // configured behavior instead of leaking the final coin-flip value.
    static std::atomic<bool> g_restoreOn{false};
    static std::atomic<bool> g_restoreKnown{false};
    static std::mutex g_writeMutex;
    static std::atomic<uint64_t> g_generation{0};

    static bool BaselineForMode(int mode) {
        return mode == DAB_All || mode == DAB_FirstHitThenOff;
    }

    static void ApplyEnabled(bool enabled) {
        if (enabled) {
            // Ownership must cover the complete randomization lifetime.  The
            // low-frequency native-F7 watcher otherwise mistakes a temporary
            // coin-flip OFF for a user configuration change.
            SetExternalAutoBlockController(true);
            g_restoreKnown.store(false, std::memory_order_relaxed);
        }
        g_enabled.store(enabled);
        g_pendingOff.store(false);
        g_lastApplied.store(-1);
        if (!enabled) {
            const bool restoreOn = g_restoreKnown.load(std::memory_order_relaxed)
                ? g_restoreOn.load(std::memory_order_relaxed)
                : BaselineForMode(GetDummyAutoBlockMode());
            (void)SetPracticeAutoBlockEnabled(
                restoreOn, restoreOn
                    ? "RandomBlock: restore configured block window ON"
                    : "RandomBlock: restore configured block window OFF");
            g_restoreKnown.store(false, std::memory_order_relaxed);
            // Release only after the authored value is back in +4936, so the
            // watcher can never observe the final random coin as user intent.
            SetExternalAutoBlockController(false);
        }
        LogOut(std::string("[RANDOM_BLOCK] ") + (enabled ? "ENABLED" : "DISABLED"), true);
    }

    void SetEnabled(bool enabled) {
        std::lock_guard<std::mutex> lk(g_writeMutex);
        ApplyEnabled(enabled);
        g_generation.fetch_add(1, std::memory_order_release);
    }

    bool IsEnabled() { return g_enabled.load(); }

    uint64_t GetMutationGeneration() {
        return g_generation.load(std::memory_order_acquire);
    }

    bool SetEnabledIfGeneration(bool enabled, uint64_t expectedGeneration) {
        std::lock_guard<std::mutex> lk(g_writeMutex);
        if (g_generation.load(std::memory_order_relaxed) != expectedGeneration) return false;
        ApplyEnabled(enabled);
        g_generation.store(expectedGeneration + 1, std::memory_order_release);
        return true;
    }

    // Helper: conservative guard/actionability classification for P2 using move IDs
    static inline bool IsP2BlockingOrBlockstun(short moveId) {
        return (moveId >= 150 && moveId <= 156); // same range used in practice_patch
    }

    void Tick(short /*p1MoveId*/, short p2MoveId) {
        std::lock_guard<std::mutex> lk(g_writeMutex);
        if (!g_enabled.load()) return;
        if (GetCurrentGameMode() != GameMode::Practice) return;
        if (GetCurrentGamePhase() != GamePhase::Match) return;

        // Determine whether the dummy mode currently wants autoblock ON.
        // We'll randomize only during ON windows per the active mode.
        bool wantWindow = false;
        if (!GetCurrentDesiredAutoBlockOn(wantWindow)) {
            // Fall back to basic All/None based on current flag if desired state unavailable
            bool curOn = false; GetPracticeAutoBlockEnabled(curOn);
            wantWindow = curOn;
        }
        g_restoreOn.store(wantWindow, std::memory_order_relaxed);
        g_restoreKnown.store(true, std::memory_order_relaxed);
        // Special-case: FirstHitThenOff should HOLD ON deterministically until the first block occurs
        // (i.e., while wantWindow==true for this mode). Do not randomize in this phase.
        int dabMode = GetDummyAutoBlockMode();
        if (dabMode == DAB_FirstHitThenOff && wantWindow) {
            bool curOn = false; if (!GetPracticeAutoBlockEnabled(curOn)) return;
            if (!curOn || g_lastApplied.load() != 1) {
                if (SetPracticeAutoBlockEnabled(true, "RandomBlock: force ON (FirstHitThenOff window)")) {
                    g_lastApplied.store(1);
                }
            }
            // Ensure no pending OFF while in the hold-ON window
            g_pendingOff.store(false);
            return;
        }

        if (!wantWindow) {
            // If the mode does not want AB now, ensure OFF (with safety deferral) and skip randomizing
            bool curOn = false; if (!GetPracticeAutoBlockEnabled(curOn)) return;
            bool wantOnFinal = false;
            if (IsP2BlockingOrBlockstun(p2MoveId) || !::IsActionable(p2MoveId)) {
                g_pendingOff.store(true);
                wantOnFinal = true; // hold ON until safe
            }
            if (g_pendingOff.load()) {
                if (!IsP2BlockingOrBlockstun(p2MoveId) && ::IsActionable(p2MoveId)) {
                    wantOnFinal = false; g_pendingOff.store(false);
                } else {
                    wantOnFinal = true;
                }
            }
            int wantVal = wantOnFinal ? 1 : 0;
            if (curOn != wantOnFinal || wantVal != g_lastApplied.load()) {
                if (SetPracticeAutoBlockEnabled(wantOnFinal, wantOnFinal ? "RandomBlock: hold ON (deferring OFF)" : "RandomBlock: OFF (mode window closed)")) {
                    g_lastApplied.store(wantVal);
                }
            }
            return;
        }

        // EfzRevival-style: coin flip each frame decides whether autoblock should be ON this frame
        bool wantOn = (rand() & 1) != 0;

        // Read current flag to avoid redundant writes
        bool curOn = false; if (!GetPracticeAutoBlockEnabled(curOn)) return;

        // Defer turning OFF while guarding or inactionable to avoid cutting guard/creating odd transitions
        if (!wantOn) {
            if (IsP2BlockingOrBlockstun(p2MoveId) || !::IsActionable(p2MoveId)) {
                g_pendingOff.store(true);
                wantOn = true; // keep ON until safe
            }
        }

        // If we previously deferred OFF, attempt to apply when safe now
        if (g_pendingOff.load()) {
            if (!IsP2BlockingOrBlockstun(p2MoveId) && ::IsActionable(p2MoveId)) {
                // Safe to turn OFF this frame
                wantOn = false;
                g_pendingOff.store(false);
            } else {
                wantOn = true; // continue holding ON
            }
        }

        int wantVal = wantOn ? 1 : 0;
        if (curOn != wantOn || wantVal != g_lastApplied.load()) {
            if (SetPracticeAutoBlockEnabled(wantOn, wantOn ? "RandomBlock: coin ON" : "RandomBlock: coin OFF")) {
                g_lastApplied.store(wantVal);
            }
        }
    }
}
