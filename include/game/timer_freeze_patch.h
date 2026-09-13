// Character timer freezes implemented as owned CODE patches, not memory writes.
//
// Three engine countdowns can be held in place for drills:
//   * Akiko's 214214 curse timer (her own +0x3148, seeded 1000 ticks) - the
//     opponent stays cursed at whatever level landed;
//   * Akiko's 641236 time-slow odometer (her own +0x3154/58/5C/60, counting UP
//     by 4-level per tick) - the slow never winds down;
//   * Mizuka Nagamori's Final Memory timer (her own +0x3150, seeded 1200) -
//     the opponent stays frozen and the FM never ends.
//
// Each freeze no-ops exactly ONE instruction of the engine's own tick (the
// decrement, or the odometer add) through the practice PatchLedger under its
// own owner key. Nothing is written into the fighter structs, every other
// side effect of the tick (curse aura, freeze pulses, meter pin, HUD gauges)
// keeps running from the engine's unmodified code, and restoring the bytes
// simply lets the countdown resume from where it stopped. The sites live
// inside character-specific handlers, so an installed patch is inert unless
// that character is actually in the match.
//
// Lifecycle mirrors final_memory_patch: the request is a local training
// preference; the bytes are installed only while features are enabled, the
// mode is Practice, the phase is Match and no netplay session/suspend is
// live, and they are force-restored on DisableFeatures, EnterNetplaySuspend,
// startup and DLL detach. Leaving gameplay therefore always hands the timers
// back to the engine, which re-initialises them at the next round/select.
#pragma once

namespace TimerFreeze {

enum Timer {
    AkikoCurse        = 0,
    AkikoTimeslow     = 1,
    MizukaFinalMemory = 2,
    kTimerCount       = 3
};

// player is 1 or 2 and names the side whose menu row was toggled. The patch
// is code-level, so the live request for a timer is P1 OR P2; the per-side
// bookkeeping exists only so each menu can clear its own toggle.
// Synchronises the live patch when the aggregate request changes.
void SetRequested(Timer timer, int player, bool enabled);

// Aggregate (either side) request for a timer.
bool IsRequested(Timer timer);

// Whether this timer's owner currently holds installed bytes or an
// unresolved restoration obligation.
bool IsInstalled(Timer timer);

// Reconcile all three patches with their requests and the runtime gates.
// Returns the number of sites changed (installed or restored).
int SyncForCurrentMode(const char* reason = nullptr);

// Restore every owned site regardless of requests (suspend, disable,
// shutdown). Requests are preserved for the next Sync.
int ForceRestore(const char* reason = nullptr);

} // namespace TimerFreeze
