#include "../include/game/random_block.h"
#include "../include/game/practice_patch.h" // SetPracticeAutoBlockEnabled/GetPracticeAutoBlockEnabled, GetDummyAutoBlockMode, DAB_*
#include "../include/core/logger.h"
#include "../include/game/game_state.h"
#include "../include/core/constants.h"
#include "../include/utils/utilities.h" // ::IsActionable
#include <cstdlib>
#include <atomic>

namespace RandomBlock {
    static std::atomic<bool> g_enabled{false};
    static std::atomic<int>  g_lastApplied{-1}; // -1 unknown, 0 off, 1 on
    // Pending OFF deferral when guard/inactionable
    static std::atomic<bool> g_pendingOff{false};

    void SetEnabled(bool enabled) {
        g_enabled.store(enabled);
        g_pendingOff.store(false);
        g_lastApplied.store(-1);
        LogOut(std::string("[RANDOM_BLOCK] ") + (enabled ? "ENABLED" : "DISABLED"), true);
    }

    bool IsEnabled() { return g_enabled.load(); }

    // Helper: conservative guard/actionability classification for P2 using move IDs
    static inline bool IsP2BlockingOrBlockstun(short moveId) {
        return (moveId >= 150 && moveId <= 156); // same range used in practice_patch
    }

    void Tick(short /*p1MoveId*/, short p2MoveId) {
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
        // Special-case: FirstHitThenOff should HOLD ON deterministically until the first block occurs
        // (i.e., while wantWindow==true for this mode). Do not randomize in this phase.
        int dabMode = GetDummyAutoBlockMode();
        if (dabMode == DAB_FirstHitThenOff && wantWindow) {
            bool curOn = false; if (!GetPracticeAutoBlockEnabled(curOn)) return;
            if (!curOn || g_lastApplied.load() != 1) {
                SetExternalAutoBlockController(true);
                if (SetPracticeAutoBlockEnabled(true, "RandomBlock: force ON (FirstHitThenOff window)")) {
                    g_lastApplied.store(1);
                }
                SetExternalAutoBlockController(false);
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
            if (wantVal != g_lastApplied.load()) {
                // Announce external control so MonitorDummyAutoBlock won't write this frame
                SetExternalAutoBlockController(true);
                if (SetPracticeAutoBlockEnabled(wantOnFinal, wantOnFinal ? "RandomBlock: hold ON (deferring OFF)" : "RandomBlock: OFF (mode window closed)")) {
                    g_lastApplied.store(wantVal);
                }
                SetExternalAutoBlockController(false);
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
        if (wantVal != g_lastApplied.load()) {
            // Announce external control so MonitorDummyAutoBlock won't write this frame
            SetExternalAutoBlockController(true);
            if (SetPracticeAutoBlockEnabled(wantOn, wantOn ? "RandomBlock: coin ON" : "RandomBlock: coin OFF")) {
                g_lastApplied.store(wantVal);
            }
            SetExternalAutoBlockController(false);
        }
    }
}
