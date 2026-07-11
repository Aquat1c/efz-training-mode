#pragma once

#include <algorithm>

namespace Mission::SequencePolicy {

// EFZ can cancel a move into another instance with the same move ID. The move
// frame counter (+0xA) resets at that boundary, so ID change OR frame rewind is
// the shared recorder/runner definition of a new action instance.
constexpr bool IsMoveInstanceEdge(short currentMove, short currentFrame,
                                  short previousMove, short previousFrame) {
    return currentMove != previousMove || currentFrame < previousFrame;
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
    static constexpr int kCommitGraceTicks = 6;

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
