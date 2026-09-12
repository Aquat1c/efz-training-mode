#pragma once

#include <atomic>
#include <cstdint>
struct EfzTmEntryV1;

// Centralized immediate-register input writer.
// Runs at visual framerate (64 fps), writing only the immediate registers
// (no buffer writes). Provides clean edges by inserting a neutral frame when
// changing or re-asserting a non-zero mask.

namespace ImmediateInput {

// Start/stop worker (idempotent)
void Start();
void Stop();
// Join the old worker and clear private generation/edge state without looking
// up players or writing neutral. The owner restores input before this boundary.
void RetireWorker();
bool BindPracticeWorld(const EfzTmEntryV1& world);
bool RetireWorker(const EfzTmEntryV1& heldWorld);
// Close commands and wake the worker; restoration/join belongs to RetireWorker.
void CancelWork();
bool IsRunning();

// Continuous set: hold mask until Clear() is called.
bool Set(int playerNum, uint8_t mask);

// Timed press: hold mask for N ticks (visual frames). ticks<=0 is ignored.
// Returns false without caching the press when another input owner currently
// owns the raw lane. Callers may retry the same authored action later.
bool PressFor(int playerNum, uint8_t mask, int ticks);

// Release and write neutral immediately; cancels any active press/hold.
void Clear(int playerNum);

// For diagnostics
uint8_t GetCurrentDesired(int playerNum);
int GetRemainingTicks(int playerNum);

// Retire only a completed worker latch. This performs no game-memory write and
// fails if a timed or continuous ImmediateInput source is still active.
bool RetireCompletedBookkeeping(int playerNum);

// Tutorial-only exclusive lease. Regular Set/PressFor/Clear calls are ignored
// for this player while the token is held, so a jump cue cannot overwrite (or
// later clear) another producer's timed hold.
bool AcquireTutorialLease(int playerNum, uint64_t& tokenOut);
bool PressForTutorial(int playerNum, uint64_t token, uint8_t mask, int ticks);
void ReleaseTutorialLease(int playerNum, uint64_t token);
bool TutorialLeaseActive(int playerNum);

} // namespace ImmediateInput
