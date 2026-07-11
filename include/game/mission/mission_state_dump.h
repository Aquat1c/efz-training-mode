#pragma once
//
// Mission::StateDump - Revival savestate buffer dump/restore.
//
// Captures EfzRevival's practice savestate (the 500KB linear snapshot buffer
// behind the manual save hotkey) into a portable base64 blob, and restores
// such a blob into ANY later session - including after the game was restarted
// - by swapping the buffer contents under Revival's own restore machinery:
//
//   Capture:  TriggerSave -> read descriptor buffer [base..write) -> encode.
//   Restore:  (characters must already match - hotswap first)
//             TriggerSave            - fresh restore records with CURRENT
//                                      session addresses + current buffer len
//             verify lengths/chars   - mismatch = abort (caller falls back to
//                                      value-level Mission::Setup)
//             pointer reconciliation - pointer-bearing ranges inside the file
//                                      payload are overwritten with the fresh
//                                      session's bytes (stale heap pointers
//                                      from another session would crash)
//             overwrite buffer + TriggerLoad -> Revival memmoves the payload
//             into live memory through its own record walk.
//
// Full RE + per-version layout: shared_documentation/REVIVAL_PRACTICE_SAVELOAD_RE.md
// and MISSION_SAVESTATE_AND_RECORDING_DESIGN.md (descriptor matrix, region
// order, reconciliation ranges).
//
// The mission engine stores the blob in Mission::savestate ("savestate" JSON
// field), captured at record start and restored after the mission's hotswap
// settles. NOTE: both directions use Revival's single manual save slot - the
// mission flows already own it (auto-retry baseline); after a successful
// Restore the slot holds the mission start state, so retry loads keep working.
//
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
