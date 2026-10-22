#pragma once
// Revival savestate buffer dump/restore: captures EfzRevival's practice
// savestate (the 500KB linear snapshot buffer behind the manual save hotkey)
// into a portable base64 blob and restores it into any later session through
// Revival's own restore machinery.
// Restore pipeline (characters must already match - hotswap first):
// TriggerSave (records current session addresses + buffer len) -> verify
// lengths/chars (mismatch = abort) -> pointer reconciliation (pointer-bearing
// payload ranges are overwritten with fresh session bytes; stale heap pointers
// from another session would crash) -> overwrite buffer + TriggerLoad -> verify
// both entity rings, repair once or fail.
// Full RE & per-version layout: shared_documentation/REVIVAL_PRACTICE_SAVELOAD_RE.md
// and MISSION_SAVESTATE_AND_RECORDING_DESIGN.md.
// Both directions use Revival's single manual save slot; after a successful
// Restore the slot holds the mission start state, so retry loads keep working.
#include <string>

namespace Mission::StateDump {

// Hooks installed + supported Revival version (e/f/g/h/i/j).
bool Available();

// Dump the current match state. On success outB64 holds the encoded blob.
bool Capture(std::string& outB64, std::string& outErr);

// Restore a blob into the running match (Match phase, hotswap/setup idle,
// characters matching the dump - the caller sequences that).
bool Restore(const std::string& b64, std::string& outErr);

} // namespace Mission::StateDump
