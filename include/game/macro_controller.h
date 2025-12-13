#pragma once

#include <cstdint>
#include <string>

// Lightweight Practice-owned macro controller (NOW mode first cut).
// - Two-press record: first press enters PreRecord (cycles slot, grants P2 control),
//   second press starts recording; third press stops.
// - Replay: immediate input (ImmediateInput::Set/Clear) driven by recorded run-lengths.
// - Pause/frame-step safe: progression halts when game speed is frozen (gamespeed==0).

namespace MacroController {

enum class State : uint8_t {
    Idle = 0,
    PreRecord,
    Recording,
    Replaying
};

// Lightweight per-slot statistics for debugging/validation
struct SlotStats {
    int spanCount;
    int totalTicks;
    int bufEntries;
    int bufTicks;
    int bufIndexTicks; // number of per-tick buffer index samples captured
    uint16_t bufStartIdx;
    uint16_t bufEndIdx;
    bool hasData;
};

// Call once per internal frame from the Frame Monitor (Match phase only)
void Tick();

// Hotkeys
void ToggleRecord();   // Idle -> PreRecord -> Recording -> Idle(stop)
void Play();           // Start replay current slot if present
void PlayFromTick(int startTick); // Start replay from a specific tick offset
void Stop();           // Force stop (record/replay), restore state
void UnswapThenStop(); // Restore default mapping (unswap+CPU) first, then stop

// Slot helpers
int  GetCurrentSlot();           // 1-based slot index
void SetCurrentSlot(int slot);   // clamps to valid range
void NextSlot();                 // advance slot (wraps)
void PrevSlot();                 // optional: go back (wraps)
int  GetSlotCount();
bool IsSlotEmpty(int slot);
inline bool IsCurrentSlotEmpty() { return IsSlotEmpty(GetCurrentSlot()); }

// Status for overlays/diagnostics
State GetState();
std::string GetStatusLine();

// Debug helpers
SlotStats GetSlotStats(int slot);

// Returns the effective tick count (last non-neutral tick + 1) for timing calculations.
// This excludes trailing neutral inputs from the total.
int GetEffectiveTicks(int slot);

// Returns the tick index (0-based) of the first attack button press (A/B/C/D).
// For wake timing, this is what needs to land during the buffer window.
// Returns -1 if no button found.
int GetFirstButtonTick(int slot);

// Returns the full input mask (direction + buttons) at the first attack button tick.
// This is what should be injected during wakeup buffer window.
// Returns 0 if no button found.
uint8_t GetFirstAttackInput(int slot);

// Text serialization for macros (human-editable)
// Format header: "EFZMACRO 1" then a space-separated sequence of tokens.
// Token syntax (per 64 Hz tick):
//   - Direction+buttons, e.g., 5, 6A, 2AB, N, 4C (digit is numpad: 2=D, 4=L, 5=N, 6=R, 8=U, diagonals 1/3/7/9)
//   - Optional repeat suffix: xN (e.g., 5Ax50)
//   - Optional buffer group: {k: v1 v2 ...} where k is number of raw buffer writes this tick and v* are either
//     direction+buttons tokens or hex bytes (0xNN). If omitted, playback defaults to one write equal to the tick mask.
// IncludeBuffers controls whether Serialize emits explicit buffer groups (recommended when preserving recorder fidelity).
std::string SerializeSlot(int slot, bool includeBuffers);

// Parse a serialized macro and replace the given slot. On success returns true and clears errorOut.
// On failure returns false and puts a message into errorOut; slot contents are left unchanged.
bool DeserializeSlot(int slot, const std::string& text, std::string& errorOut);

} // namespace MacroController
