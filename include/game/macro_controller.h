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
void Stop();           // Force stop (record/replay), restore state

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

} // namespace MacroController
