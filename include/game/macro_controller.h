#pragma once

#include <cstdint>
#include <string>
struct EfzTmEntryV1;

// Practice-owned macro controller: three-stage record (PreRecord arms P2
// control, explicit start, stop-and-keep), replay via per-player input poll
// override, progression halts while game speed is frozen (gamespeed==0).
namespace MacroController {

enum class State : uint8_t {
    Idle = 0,
    PreRecord,
    Recording,
    Replaying
};

// Ordinary replay publishes neutral and waits for the target input-buffer
// index to move before loading the first serialized tick. Exact-state mission
// demos use the aligned mode after restoring/freeze-owning their world: it
// publishes tick 0 before the next native input poll so no neutral gameplay
// poll can slip in.
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

    // Shared by production admission and the policy test. Priming consumes
    // exactly one serialized tick at admission; ordinary replay leaves the
    // cursor untouched until its buffer-advance handshake fires.
    constexpr Decision Decide(PlaybackStartMode mode, int startTick,
                              int streamTicks) {
        const bool valid = startTick >= 0 && startTick < streamTicks;
        const bool prime = valid &&
            mode == PlaybackStartMode::PrimeBeforeNextPoll;
        return {valid, prime, startTick, startTick + (prime ? 1 : 0)};
    }

    // A prepared 64 Hz tick may contain several raw EFZ input-buffer writes.
    // Admission is not itself a gameplay subframe: distribute only the first
    // third of tick zero before the first poll and retain the remainder for
    // the following acknowledged subframes. Never collapse every tick-zero
    // write merely because the mission transition currently freezes the world.
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

    // Priming occupies one native subframe before gameplay resumes; the two
    // remaining cadence slots still belong to tick zero even when its raw
    // queue is already empty, or tick one could be admitted halfway through a
    // 3-subframe group and its motion samples compressed.
    constexpr bool PrimedTickPending(int subframesRemaining,
                                     int queuedWrites,
                                     int writesLeft) {
        return subframesRemaining > 0 || queuedWrites > 0 || writesLeft > 0;
    }

    constexpr bool PrimedSliceCanLatchEnd(
        int subframesRemainingBeforePublish) {
        return subframesRemainingBeforePublish <= 1;
    }

    // The serialized macro mask is authoritative for the complete 64 Hz tick;
    // the frame monitor's 192 Hz passes are not guaranteed to interleave with
    // EFZ's three native polls. Raw samples are still drained for accounting,
    // but none may replace this publication: even a non-neutral refinement
    // would make {A, B, neutral} resolve differently depending on scheduling.
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

struct SlotStats {
    int spanCount;
    int totalTicks;
    int bufEntries;
    int bufTicks;
    int bufIndexTicks; // per-tick buffer index samples captured
    uint16_t bufStartIdx;
    uint16_t bufEndIdx;
    bool hasData;
};

// Call once per internal frame from the Frame Monitor (Match phase only)
void Tick();
bool DidAdvanceRecordingTick(); // true for the current frame-monitor pass

// While suspended (live P1 recording behind a nested pause/settings surface),
// neither the macro timeline nor the input-buffer cursor advances; release
// rebases the cursor to the current frozen buffer head so menu input can never
// become clip input.
void SetRecordingCaptureSuspended(bool suspended);
bool IsRecordingCaptureSuspended();

void ToggleRecord();   // Idle -> PreRecord -> Recording -> Idle(stop)
void Play();
void PlayFromTick(int startTick);
bool PlayForPlayer(int playerNum, int startTick = 0,
                   bool exclusiveInput = false);
void Stop();           // force stop (record/replay), restore state
void UnswapThenStop(); // restore default mapping (unswap+CPU) first, then stop
// Private timeline only; input ownership must already be stopped and drained.
// Preserves recorded slots, streams and the selected slot/player settings.
void ResetTimeline();
bool BindPracticeWorld(const EfzTmEntryV1& world);
void CancelPracticeWork();
uint32_t RetirePracticeWorld(const EfzTmEntryV1& heldWorld);

// Player-selectable recording/replay for mission demonstrations; regular
// Practice macros keep using P2 through ToggleRecord()/Play().
bool BeginPlayerRecording(int playerNum, bool switchLocalControl = false);
bool StartPlayerRecording();
bool FinishPlayerRecording();
// Finishes the active recording and serializes the exact slot sealed by that
// operation, copied under the recorder mutex so a later macro action cannot
// redirect the result through the mutable "last recording" selector.
// Honors RequestRecordingStopAtBoundary().
bool FinishPlayerRecordingAndSerialize(std::string& serializedOut,
                                       bool includeBuffers);
void RequestRecordingStopAtBoundary();
std::string SerializeLastPlayerRecording(bool includeBuffers);

// Plays a temporary macro without replacing any of the eight user slots.
// exclusiveInput marks the replay as owning gameplay input; callers should
// gate their frontend so only an explicit cancel command is accepted.
bool PlaySerializedForPlayer(const std::string& text, int playerNum,
                             bool exclusiveInput, std::string& errorOut,
                             PlaybackStartMode startMode =
                                 PlaybackStartMode::Ordinary);
bool ValidateSerialized(const std::string& text, std::string& errorOut);
bool IsExclusivePlayback();
int  GetPlaybackPlayer();
void ReleaseExclusivePlaybackHold();

int  GetCurrentSlot();           // 1-based slot index
void SetCurrentSlot(int slot);   // clamps to valid range
void NextSlot();
void PrevSlot();
int  GetSlotCount();
bool IsSlotEmpty(int slot);
inline bool IsCurrentSlotEmpty() { return IsSlotEmpty(GetCurrentSlot()); }

State GetState();
std::string GetStatusLine();

SlotStats GetSlotStats(int slot);

// Effective tick count (last non-neutral tick + 1); excludes trailing neutral
// inputs.
int GetEffectiveTicks(int slot);

// 0-based tick of the first attack button press (A/B/C/D), or -1 if none.
int GetFirstButtonTick(int slot);

// Full input mask (direction + buttons) at the first attack button tick, or 0
// if none.
uint8_t GetFirstAttackInput(int slot);

// Text serialization for macros (human-editable)
// Format header: "EFZMACRO 1" then a space-separated sequence of tokens.
// Token syntax (per 64 Hz tick):
//   - Direction+buttons, e.g., 5, 6A, 2AB, N, 4C (digit is numpad: 2=D, 4=L, 5=N, 6=R, 8=U, diagonals 1/3/7/9)
//   - Optional repeat suffix: xN (e.g., 5Ax50)
//   - Optional buffer group: {k: v1 v2 ...} where k is number of raw buffer writes this tick and v* are either
//     direction+buttons tokens or hex bytes (0xNN). If omitted, playback defaults to one write equal to the tick mask.
// IncludeBuffers controls whether Serialize emits explicit buffer groups.
std::string SerializeSlot(int slot, bool includeBuffers);

// Parse a serialized macro and replace the given slot. On success returns true
// and clears errorOut; on failure returns false, fills errorOut, and leaves
// the slot contents unchanged.
bool DeserializeSlot(int slot, const std::string& text, std::string& errorOut);

} // namespace MacroController
