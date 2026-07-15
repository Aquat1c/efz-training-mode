#pragma once

#include <algorithm>

namespace Mission::SequencePolicy {

// Mission authoring count-in uses the frame monitor's 192 Hz logical clock.
// Configuration is stored in milliseconds so 0 can mean "off" without a
// separate flag. Ceiling conversion never starts a take earlier than the
// duration selected by the author.
constexpr int kMissionMonitorTicksPerSecond = 192;
constexpr int RecorderCountInTicksFromMs(int milliseconds) {
    return milliseconds <= 0
        ? 0
        : (milliseconds * kMissionMonitorTicksPerSecond + 999) / 1000;
}
constexpr int RecorderCountInRemainingMs(int remainingTicks) {
    return remainingTicks <= 0
        ? 0
        : (remainingTicks * 1000 + kMissionMonitorTicksPerSecond - 1) /
              kMissionMonitorTicksPerSecond;
}

enum class DemoBaselineSource {
    None = 0,
    RunnerCheckpoint,
    EmbeddedDump,
};

// Loaded mission playback should use the already-verified same-session retry
// checkpoint. Recorder previews have no such ownership and use their embedded
// frame-zero dump instead.
constexpr DemoBaselineSource DecideDemoBaselineSource(
    bool recorderPreview, bool runnerCheckpointReady, bool embeddedDumpReady) {
    if (!recorderPreview && runnerCheckpointReady) {
        return DemoBaselineSource::RunnerCheckpoint;
    }
    if (embeddedDumpReady) {
        return DemoBaselineSource::EmbeddedDump;
    }
    return DemoBaselineSource::None;
}

// EFZ can cancel a move into another instance with the same move ID. The move
// frame counter (+0xA) resets to the animation head at that boundary. A bare
// "frame went backwards" test is NOT sufficient: looping animations (air IC
// float, air dashes, a whiffed air normal's falling tail) rewind their frame
// on every loop pass, and each wrap was being recorded as a new action
// (2026-07-11: one air IC captured as five j.IC steps at the ~18-tick loop
// period). A same-ID rewind therefore only counts when it lands back at the
// animation head.
constexpr short kMoveRestartFrameMax = 2;
constexpr bool IsMoveInstanceEdge(short currentMove, short currentFrame,
                                  short previousMove, short previousFrame) {
    return currentMove != previousMove ||
           (currentFrame < previousFrame && currentFrame <= kMoveRestartFrameMax);
}

// A cancel/transition destination is creditable only on a fresh destination
// instance and, when the author names a source, when the immediately previous
// sampled move is one of those sources.  This deliberately rejects the weaker
// "A occurred at some point before B" interpretation.
constexpr bool DirectTransitionSatisfied(bool destinationInstanceEdge,
                                         bool sourceRequired,
                                         bool previousSourceMatches) {
    return destinationInstanceEdge &&
           (!sourceRequired || previousSourceMatches);
}

// Consecutive blocked attacks in a true blockstring do not necessarily leave
// the defender's blockstun move between contacts.  A fresh reaction is either
// a change to another blockstun move or a same-ID animation restart at its
// head.  Merely observing the defender anywhere inside an old blockstun
// animation is not evidence for the currently armed strike.
constexpr bool FreshBlockReactionSatisfied(bool currentBlockstun,
                                           bool currentRecoilGuard,
                                           short currentMove,
                                           short currentFrame,
                                           short previousMove,
                                           short previousFrame) {
    return currentBlockstun && !currentRecoilGuard &&
           IsMoveInstanceEdge(currentMove, currentFrame,
                              previousMove, previousFrame);
}

// Akiko's 41236A/B/C vacuum is one authored input with two consecutive move
// IDs: suction/startup (263/265/267), then the automatic damaging phase
// (264/266/268). This pair must remain one recipe step; otherwise the recorder
// invents a second input and strict playback rejects the automatic transition.
constexpr short AkikoVacuumHitPhase(short startupMove) {
    return startupMove == 263 ? 264
         : startupMove == 265 ? 266
         : startupMove == 267 ? 268
         : 0;
}

constexpr bool IsAkikoVacuumHitTransition(short startupMove, short currentMove) {
    const short hitPhase = AkikoVacuumHitPhase(startupMove);
    return hitPhase != 0 && currentMove == hitPhase;
}

// Rumi's three 41236 strengths have different starter IDs, but a successful
// command throw always enters move 251 without another player input. The later
// 236A/B/C choice (262/252/306) is a real input and deliberately is not folded
// into this transition.
constexpr bool IsRumiCommandThrowSuccessTransition(short startupMove,
                                                   short currentMove) {
    return currentMove == 251 &&
           (startupMove == 250 || startupMove == 263 || startupMove == 264);
}

enum class ComboEndDecision {
    None,
    ConsumeRequired,
    UnexpectedDrop,
};

enum class SampledComboBoundary {
    None,
    VisibleRecovery,
    CounterRollover,
};

// Format 1 observes snapshots rather than the authoritative game-thread combo
// transaction. Most boundaries expose a zero-combo sample; a frame-perfect
// wake-up hit can instead reset N directly to a smaller positive count.
constexpr SampledComboBoundary DetectSampledComboBoundary(
    bool wasAlive, bool isAlive, int previousCombo, int currentCombo) {
    if (wasAlive && !isAlive) return SampledComboBoundary::VisibleRecovery;
    if (wasAlive && isAlive && previousCombo > 0 && currentCombo > 0 &&
        currentCombo < previousCombo) {
        return SampledComboBoundary::CounterRollover;
    }
    return SampledComboBoundary::None;
}

// A setup/meaty action is allowed to begin while the defender is still in the
// authored Part-1 recovery.  Performing a move-only setup is progress, but a
// hit/contact requirement cannot be fulfilled until the required recovery edge
// has been consumed; doing so would prove the take was still one continuous
// combo instead of the authored reset.
enum class StepSatisfactionDecision {
    Wait,
    Advance,
    PrematureContactDrop,
};

constexpr StepSatisfactionDecision DecideStepSatisfaction(
    bool satisfied, bool awaitingComboEnd, bool moveOnly) {
    if (!satisfied) return StepSatisfactionDecision::Wait;
    if (awaitingComboEnd && !moveOnly) {
        return StepSatisfactionDecision::PrematureContactDrop;
    }
    return StepSatisfactionDecision::Advance;
}

constexpr int RecordedActionBaselineAfterBoundary(
    int priorBaseline, int activeStep, int boundaryStep, bool activeStepLanded) {
    return activeStep > boundaryStep && !activeStepLanded ? 0 : priorBaseline;
}

constexpr int RunnerArmBaseline(int currentCombo, bool boundaryConsumedThisTick) {
    return boundaryConsumedThisTick ? 0 : currentCombo;
}

// Bounded bridge between a poll-consumed attack-button edge and the move-ID
// transition it produces. It never satisfies a step: it only postpones the
// neutral-gap drop long enough for normal strict action matching to run.
struct DelayedActionWindow {
    // 2026-07-14 live trace (Mai OTG, 22C canceling the throw): a buffered
    // cancel legitimately holds the button press until the move's cancel
    // point opens - the C edge preceded the 167 instance edge by 3-20 visual
    // frames, so a 6-tick (2-frame) grace expired and a perfect direct
    // 221->167 cancel was rejected as "a different button". The bond stays
    // honest at 90 ticks (~30 frames): the grace only arms while the commit
    // step is CURRENT and the pressed button matches its inputMask, and a
    // Red/Blue IC (167/171) cannot arise from any other button anyway.
    static constexpr int kCommitGraceTicks = 90;

    int elapsed = 0;
    int commitGrace = 0;

    void Reset() {
        elapsed = 0;
        commitGrace = 0;
    }

    bool Tick(bool frozen, bool actionArmed, bool attackPressedEdge, int limit) {
        if (actionArmed) {
            Reset();
            return false;
        }
        if (limit <= 0) return false;

        // Remember an attack sampled at/just before the deadline even if this
        // tick is frozen and its action transition appears on the next sample.
        if (attackPressedEdge && elapsed >= (std::max)(0, limit - 1)) {
            commitGrace = kCommitGraceTicks;
        }
        if (frozen) return false;

        ++elapsed;
        if (attackPressedEdge && elapsed >= (std::max)(0, limit - 1)) {
            commitGrace = kCommitGraceTicks;
        }
        if (elapsed <= limit) return false;
        if (commitGrace > 0) {
            --commitGrace;
            return false;
        }
        return true;
    }
};

constexpr ComboEndDecision DecideComboEnd(bool endedEdge, bool required) {
    if (!endedEdge) return ComboEndDecision::None;
    return required ? ComboEndDecision::ConsumeRequired
                    : ComboEndDecision::UnexpectedDrop;
}

// Per-step damage validation. Hit-variant mechanics (Akiko rekka crits,
// Mizuka clean hits on j.C, ...) keep the same move ID but change the step's
// damage by thousands, silently derailing the rest of the combo. Ordinary
// variance (proration rounding after small route drift) stays within a few
// hundred. A recorded delta of 0 means "no damage requirement".
constexpr int kStepDamageTolerance = 500;
constexpr bool StepDamageDiverged(int recordedDelta, int observedDelta) {
    if (recordedDelta <= 0) return false;
    const int diff = observedDelta - recordedDelta;
    return diff > kStepDamageTolerance || diff < -kStepDamageTolerance;
}

// Legacy recorder/runner timing uses EFZ's nominal 192-Hz internal-tick unit.
// Retain twice the observed gap plus one normal fail window, with a 30-second
// malformed-take safety ceiling.
constexpr int RecordedGapAllowance(int observedTicks) {
    if (observedTicks <= 0) return 0;
    const long long generous = static_cast<long long>(observedTicks) * 2 + 60;
    return static_cast<int>(generous > 5760 ? 5760 : generous);
}

struct SegmentScore {
    int finalizedHits = 0;
    int finalizedDamage = 0;
    int segmentMaxHits = 0;
    int segmentMaxDamage = 0;

    void Reset() {
        finalizedHits = 0;
        finalizedDamage = 0;
        segmentMaxHits = 0;
        segmentMaxDamage = 0;
    }

    void Observe(int comboHits, int comboDamage) {
        // Damage can remain readable briefly after recovery; zero combo never
        // owns score for a new segment.
        if (comboHits <= 0) return;
        segmentMaxHits = (std::max)(segmentMaxHits, comboHits);
        segmentMaxDamage = (std::max)(segmentMaxDamage, comboDamage);
    }

    void FinalizeSegment() {
        finalizedHits += segmentMaxHits;
        finalizedDamage += segmentMaxDamage;
        segmentMaxHits = 0;
        segmentMaxDamage = 0;
    }

    int TotalHits() const { return finalizedHits + segmentMaxHits; }
    int TotalDamage() const { return finalizedDamage + segmentMaxDamage; }
};

} // namespace Mission::SequencePolicy
