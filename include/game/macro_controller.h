#pragma once

#include <cstdint>
#include <string>

// Lightweight Practice-owned macro controller (NOW mode first cut).
// - Three-stage record: arm PreRecord (grants P2 control), explicitly start,
//   then stop and keep the clip.
// - Replay: per-player engine poll override driven by recorded input frames.
// - Pause/frame-step safe: progression halts when game speed is frozen (gamespeed==0).

namespace MacroController {

enum class State : uint8_t {
    Idle = 0,
    PreRecord,
    Recording,
    Replaying
};

// Ordinary Practice macros retain their existing admission behavior: replay
// first publishes neutral and waits for the target input-buffer index to move
// before loading the first serialized tick.  Exact-state mission demos use the
// aligned mode after restoring/freeze-owning their world; it publishes tick 0
// before the next native input poll so no neutral gameplay poll can slip in.
enum class PlaybackStartMode : uint8_t {
    Ordinary = 0,
    PrimeBeforeNextPoll,
};

namespace PlaybackStartPolicy {
    struct Decision {
        bool valid;
        bool prime;
        int tickToPrepare;
        int cursorAfterAdmission;
    };

    // Shared by production admission and the policy test.  Priming consumes
    // exactly one serialized tick at admission; ordinary replay leaves the
    // cursor untouched until its historical buffer-advance handshake fires.
    constexpr Decision Decide(PlaybackStartMode mode, int startTick,
                              int streamTicks) {
        const bool valid = startTick >= 0 && startTick < streamTicks;
        const bool prime = valid &&
            mode == PlaybackStartMode::PrimeBeforeNextPoll;
        return {valid, prime, startTick, startTick + (prime ? 1 : 0)};
    }

    // A prepared 64 Hz tick may contain several raw EFZ input-buffer writes.
    // Admission is not itself a gameplay subframe: distribute only the first
    // third of tick zero before the first poll and retain the remainder for the
    // following acknowledged subframes.  In particular, never collapse every
    // tick-zero write merely because the mission transition currently freezes
    // the world.
    constexpr int WritesForSubframe(int writesLeft, int subframesLeft) {
        return writesLeft > 0 && subframesLeft > 0
            ? (writesLeft + subframesLeft - 1) / subframesLeft
            : 0;
    }

    // Playback can expose one distinct raw state per 192 Hz subframe. Larger
    // groups cannot be reproduced without collapsing intermediate motions, so
    // reject them instead of pretending they are lossless.
    constexpr bool SupportedRawWriteCount(int writes) {
        return writes >= 0 && writes <= 3;
    }

    constexpr bool CanPublishNextPrimedSlice(bool awaitingFirstPoll,
                                              bool pollAdvanced,
                                              bool bufferAdvanced,
                                              bool frameStepAdvanced) {
        return !awaitingFirstPoll &&
               (pollAdvanced || bufferAdvanced || frameStepAdvanced);
    }

    // Priming occupies one native subframe before gameplay resumes. The two
    // remaining cadence slots still belong to tick zero even when its raw
    // queue is already empty; otherwise tick one can be admitted halfway
    // through a 3-subframe group and its motion samples get compressed.
    constexpr bool PrimedTickPending(int subframesRemaining,
                                     int queuedWrites,
                                     int writesLeft) {
        return subframesRemaining > 0 || queuedWrites > 0 || writesLeft > 0;
    }

    constexpr bool PrimedSliceCanLatchEnd(
        int subframesRemainingBeforePublish) {
        return subframesRemainingBeforePublish <= 1;
    }

    // The frame-monitor's 192 Hz passes are not guaranteed to interleave with
    // EFZ's three native polls.  The serialized macro mask is therefore the
    // authoritative state for the complete 64 Hz logical tick.  Raw samples
    // are still drained for buffer-cursor/snapshot accounting, but none may
    // replace this scalar publication: even a non-neutral refinement would
    // make {A, B, neutral} resolve differently depending on thread scheduling.
    constexpr uint8_t PublishMonitorSlice(uint8_t logicalMask,
                                          uint8_t rawMask,
                                          uint8_t directionMask,
                                          uint8_t buttonMask) {
        (void)rawMask;
        (void)directionMask;
        (void)buttonMask;
        return logicalMask;
    }

    constexpr bool StreamEndCanLatch(int cursor, int streamTicks,
                                     int queuedWrites, int writesLeft) {
        return cursor >= streamTicks && queuedWrites == 0 && writesLeft == 0;
    }
}

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
bool DidAdvanceRecordingTick(); // true for the current frame-monitor pass

// Mission authoring can temporarily put a live P1 recording behind a nested
// pause/settings surface. While suspended, neither the macro timeline nor the
// input-buffer cursor advances. Releasing the suspension rebases the cursor to
// the current frozen buffer head so menu input can never become clip input.
void SetRecordingCaptureSuspended(bool suspended);
bool IsRecordingCaptureSuspended();

// Hotkeys
void ToggleRecord();   // Idle -> PreRecord -> Recording -> Idle(stop)
void Play();           // Start replay current slot if present
void PlayFromTick(int startTick); // Start replay from a specific tick offset
bool PlayForPlayer(int playerNum, int startTick = 0,
                   bool exclusiveInput = false);
void Stop();           // Force stop (record/replay), restore state
void UnswapThenStop(); // Restore default mapping (unswap+CPU) first, then stop

// Player-selectable recording/replay used by mission demonstrations. Regular
// Practice macros keep using P2 through ToggleRecord()/Play(); mission capture
// uses P1 without swapping local control.
bool BeginPlayerRecording(int playerNum, bool switchLocalControl = false);
bool StartPlayerRecording();
bool FinishPlayerRecording();
// Finish the active recording and serialize the exact slot sealed by that
// operation.  The slot is copied while the recorder mutex still owns it, so a
// later macro action cannot redirect the result through the mutable "last
// recording" selector.  Honors RequestRecordingStopAtBoundary().
bool FinishPlayerRecordingAndSerialize(std::string& serializedOut,
                                       bool includeBuffers);
void RequestRecordingStopAtBoundary();
std::string SerializeLastPlayerRecording(bool includeBuffers);

// Parse and play a temporary macro without replacing any of the eight user
// slots. `exclusiveInput` marks the replay as owning gameplay input; callers
// should gate their frontend so only an explicit cancel command is accepted.
bool PlaySerializedForPlayer(const std::string& text, int playerNum,
                             bool exclusiveInput, std::string& errorOut,
                             PlaybackStartMode startMode =
                                 PlaybackStartMode::Ordinary);
bool ValidateSerialized(const std::string& text, std::string& errorOut);
bool IsExclusivePlayback();
int  GetPlaybackPlayer();
void ReleaseExclusivePlaybackHold();

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
