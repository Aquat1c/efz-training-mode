#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>

namespace Mission::SequencePolicy {

// Count-in uses the frame monitor's 192 Hz clock; config is in ms so 0 = off,
// and the ceiling conversion never starts a take early.
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

// Two fresh neutral P1 polls are required before capture resumes so the
// button that closed the UI cannot become the take's first input.
constexpr int RecorderMenuNeutralPollCount(bool anyMenuVisible,
                                           bool freshPoll,
                                           unsigned physicalMask,
                                           int currentCount) {
    if (anyMenuVisible || !freshPoll) return anyMenuVisible ? 0 : currentCount;
    return physicalMask == 0 ? (std::min)(2, currentCount + 1) : 0;
}

constexpr bool RecorderMenuReleaseReady(bool anyMenuVisible,
                                        int neutralPollCount) {
    return !anyMenuVisible && neutralPollCount >= 2;
}

// EFZ's native Practice pause does not advance character-input polls, so
// holding the pause until neutral polls arrive would deadlock the handoff.
// Release it once the close transaction is done while the zero-input lease
// still suppresses the dismissing button.
constexpr bool RecorderPhysicalPauseReleaseReady(bool handoffReady,
                                                  bool anyMenuVisible) {
    return handoffReady && !anyMenuVisible;
}

// Only a press close to the current transition proves a table-known follow-up
// was player-entered; historical unmatched input must not turn automatic hit
// phases into authored steps.
constexpr bool RecorderAttackBatchIsFresh(int currentFrame, int batchFrame,
                                          int windowTicks) {
    const int age = currentFrame - batchFrame;
    return age >= 0 && age <= windowTicks;
}

// Bounded button-edge-to-move-instance bridge; 90 ticks is the same
// 30-visual-frame envelope the game's buffered specials/cancels need.
constexpr int kRecorderInputCausalityWindowTicks = 90;
constexpr uint8_t kAttackButtonA = 0x10;
constexpr uint8_t kAttackButtonB = 0x20;
constexpr uint8_t kAttackButtonC = 0x40;
constexpr uint8_t kAttackButtonD = 0x80;
constexpr uint8_t kAttackButtonMask =
    kAttackButtonA | kAttackButtonB | kAttackButtonC | kAttackButtonD;

constexpr bool ValidExpectedAttackMask(int mask) {
    return mask >= 0 && mask <= 255 &&
           (mask & ~static_cast<int>(kAttackButtonMask)) == 0;
}

constexpr char UpperAscii(char value) {
    return value >= 'a' && value <= 'z'
        ? static_cast<char>(value - ('a' - 'A'))
        : value;
}

constexpr bool IsNotationButtonBoundary(char value) {
    return value == '\0' || value == ' ' || value == '\t' ||
           value == '/' || value == ')' || value == '(' || value == '~' ||
           value == '-' || value == '+';
}

constexpr uint8_t AttackButtonBit(char value) {
    return UpperAscii(value) == 'A' ? kAttackButtonA
         : UpperAscii(value) == 'B' ? kAttackButtonB
         : UpperAscii(value) == 'C' ? kAttackButtonC
         : UpperAscii(value) == 'D' ? kAttackButtonD
         : 0;
}

constexpr uint8_t ExpectedAttackMaskFromNotationRange(
    const char* notation, std::size_t begin, std::size_t end) {
    uint8_t result = 0;
    for (std::size_t index = begin; index < end; ++index) {
        const char current = UpperAscii(notation[index]);
        const char previous = index == begin ? '\0' : notation[index - 1];
        const char next = index + 1 < end ? notation[index + 1] : '\0';
        const bool icTokenStart = index == begin ||
            IsNotationButtonBoundary(previous) || previous == '.' ||
            ((UpperAscii(previous) == 'B' || UpperAscii(previous) == 'F') &&
             (index == begin + 1 ||
              IsNotationButtonBoundary(notation[index - 2])));
        if (current == 'I' && UpperAscii(next) == 'C' && icTokenStart &&
            IsNotationButtonBoundary(
                index + 2 < end ? notation[index + 2] : '\0')) {
            result = static_cast<uint8_t>(result | kAttackButtonC);
            continue;
        }

        const bool strengthPrefix = index == begin ||
            (previous >= '0' && previous <= '9') || previous == '.' ||
            previous == '/' || previous == '~' || previous == ' ';
        if (!strengthPrefix || !IsNotationButtonBoundary(next)) continue;

        const uint8_t button = AttackButtonBit(current);
        if (button != 0) {
            result = static_cast<uint8_t>(result | button);
        } else if (current == 'S') {
            // EFZ's S notation is the physical D/special button.
            result = static_cast<uint8_t>(result | kAttackButtonD);
        }
    }
    return result;
}

// Extract only notation-strength tokens (5A, c.5B, j.C, 214A/B/C, 5S), not
// arbitrary letters in labels.  IC/BIC/FIC are performed with C; a bare
// descriptive name stays unknown and falls back to the observed causal edge.
constexpr uint8_t ExpectedAttackMaskFromNotation(const char* notation) {
    if (notation == nullptr) return 0;
    std::size_t length = 0;
    while (notation[length] != '\0') ++length;
    return ExpectedAttackMaskFromNotationRange(notation, 0, length);
}

// Route labels retain their full history (FM~A~B~C); causality belongs to the
// newest `~` segment containing a strength token (`~5B~4/5/6` falls back to B).
constexpr uint8_t ExpectedAttackMaskFromFinalNotationStep(
    const char* notation) {
    if (notation == nullptr) return 0;
    std::size_t segmentStart = 0;
    uint8_t latest = 0;
    for (std::size_t index = 0;; ++index) {
        if (notation[index] != '~' && notation[index] != '\0') continue;
        const uint8_t segment = ExpectedAttackMaskFromNotationRange(
            notation, segmentStart, index);
        if (segment != 0) latest = segment;
        if (notation[index] == '\0') return latest;
        segmentStart = index + 1;
    }
}

constexpr uint8_t ExpectedAttackMaskFromMoveId(int moveId) {
    switch (moveId) {
        case 167: // ground IC
        case 171: // air IC
        case 203: case 206: case 209:
        case 232: case 235:
            return kAttackButtonC;
        case 200: case 204: case 207:
        case 230: case 233:
            return kAttackButtonA;
        case 201: case 202: case 205: case 208:
        case 231: case 234:
            return kAttackButtonB;
        default:
            return 0;
    }
}

constexpr uint8_t ExpectedAttackMaskForAction(const char* notation,
                                              int moveId) {
    const uint8_t fromNotation = ExpectedAttackMaskFromNotation(notation);
    return fromNotation != 0 ? fromNotation
                             : ExpectedAttackMaskFromMoveId(moveId);
}

constexpr bool AttackEdgeMatchesExpected(uint8_t observedMask,
                                         uint8_t expectedMask) {
    const uint8_t observedAttack =
        static_cast<uint8_t>(observedMask & kAttackButtonMask);
    return observedAttack != 0 &&
           (expectedMask == 0 || (observedAttack & expectedMask) != 0);
}

constexpr uint8_t BoundCausalAttackMask(uint8_t observedMask,
                                        uint8_t derivedExpectedMask) {
    const uint8_t observedAttack =
        static_cast<uint8_t>(observedMask & kAttackButtonMask);
    return derivedExpectedMask != 0
        ? static_cast<uint8_t>(observedAttack & derivedExpectedMask)
        : observedAttack;
}

struct RecorderAttackEdgeCandidate {
    uint32_t serial = 0;
    uint8_t mask = 0;
    int frame = 0;
};

// Newest-first: a known strength skips unrelated fresher edges; unknown
// notation accepts only a recent edge, keeping older presses as diagnostics.
constexpr int SelectNewestEligibleAttackEdge(
    const RecorderAttackEdgeCandidate* candidates, std::size_t count,
    uint32_t currentPollSerial, int currentFrame, int windowTicks,
    uint8_t expectedMask) {
    if (candidates == nullptr) return -1;
    for (std::size_t index = count; index-- > 0;) {
        const RecorderAttackEdgeCandidate& candidate = candidates[index];
        if (candidate.serial == 0 ||
            static_cast<int32_t>(currentPollSerial - candidate.serial) < 0 ||
            !RecorderAttackBatchIsFresh(currentFrame, candidate.frame,
                                        windowTicks) ||
            !AttackEdgeMatchesExpected(candidate.mask, expectedMask)) {
            continue;
        }
        return static_cast<int>(index);
    }
    return -1;
}

// Catalogued automatic phases alias the preceding player action; player-selected
// continuations carry MoveRole::InputFollowup instead.
constexpr bool ShouldFoldKnownAutomaticPhase(bool knownAutomaticPhase) {
    return knownAutomaticPhase;
}

// Format-1 files predating expectedAttackMask preserve their former any-attack
// deadline grace. New recordings arm grace only for the current step's button.
constexpr bool DeadlineAttackEdgeMatches(uint8_t observedMask,
                                         uint8_t expectedMask) {
    return AttackEdgeMatchesExpected(observedMask, expectedMask);
}

inline bool IsLegacyUnmatchedInputDiagnostic(const std::string& reason) {
    static const std::string prefix =
        " attack input batch(es) did not become recorded moves:";
    static const std::string suffix =
        "; retake or remove those presses";
    std::size_t countEnd = 0;
    while (countEnd < reason.size() &&
           reason[countEnd] >= '0' && reason[countEnd] <= '9') {
        ++countEnd;
    }
    return countEnd > 0 &&
           reason.compare(countEnd, prefix.size(), prefix) == 0 &&
           reason.size() >= suffix.size() &&
           reason.compare(reason.size() - suffix.size(), suffix.size(),
                          suffix) == 0;
}

constexpr bool RecorderStopWaitExpired(bool macroTickAdvanced, bool paused,
                                       int waitedTicks, int maxWaitTicks) {
    return !macroTickAdvanced && !paused && waitedTicks > maxWaitTicks;
}

// Persistent Practice helpers are suppressed (not disabled) during capture/demo
// transactions so they cannot contribute unrecorded input; user settings resume
// after the terminal boundary.
constexpr bool PracticeAutomationSuppressed(bool demoActive,
                                             bool recorderCaptureActive) {
    return demoActive || recorderCaptureActive;
}

enum class DemoBaselineSource {
    None = 0,
    RunnerCheckpoint,
    EmbeddedDump,
};

// An authored embedded state is part of the mission contract: without the exact
// restore backend the mission stays unavailable rather than rebuilding a
// different baseline from value-level setup.
constexpr bool EmbeddedSavestateCanLoad(bool hasEmbeddedSavestate,
                                        bool stateDumpAvailable) {
    return !hasEmbeddedSavestate || stateDumpAvailable;
}

// Loaded mission playback uses the verified same-session retry checkpoint;
// recorder previews use their embedded frame-zero dump.
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

// Read-only playback timeline for a loaded mission only (not an unsaved
// preview); shared by the engine observer and render routing.
constexpr bool DemoTrialRecipeActive(bool demoPlaying, bool recorderPreview,
                                     bool runnerActive, bool hasSteps) {
    return demoPlaying && !recorderPreview && runnerActive && hasSteps;
}

// Hold the frozen Playing presentation a few 60 Hz render frames so zero-tail
// clips draw before Restoring.
constexpr int kDemoTerminalRecipeHoldTicks = 12;

// After macro Idle the final attack/projectile can still be in startup; keep
// the world running under exclusive neutral for a bounded settlement window.
constexpr int kDemoRecipeSettleTimeoutTicks = 768; // four seconds at 192 Hz

enum class DemoRecipeEndAction {
    ContinuePlayback = 0,
    SettleFinalContacts,
    HoldTerminalFrame,
    FailThenHold,
};

// Input playback ending and trial grading ending are separate events.
constexpr DemoRecipeEndAction DecideDemoRecipeEnd(
    bool macroIdle, bool runnerTerminal, int settleTicksRemaining) {
    if (!macroIdle) return DemoRecipeEndAction::ContinuePlayback;
    if (runnerTerminal) return DemoRecipeEndAction::HoldTerminalFrame;
    return settleTicksRemaining == 1
        ? DemoRecipeEndAction::FailThenHold
        : DemoRecipeEndAction::SettleFinalContacts;
}

// Demo grading is presentation-only: it may latch the failed step but must
// never count as a player try or arm checkpoint auto-retry.
constexpr int RunnerDropAttemptDelta(bool demoPresentation) {
    return demoPresentation ? 0 : 1;
}

constexpr int RunnerDropRetryDelay(bool demoPresentation,
                                   int ordinaryRetryDelay) {
    return demoPresentation ? 0 : ordinaryRetryDelay;
}

enum class DemoRoundEventAction {
    Continue = 0,
    NormalizeRestoreTransient,
    RestoreBaseline,
};

// EFZ can re-publish saved round-event fields on the first live update after a
// Revival checkpoint load; that one-frame handoff is not a new intro/KO.  A
// short bounded window normalizes it, after which the same signal is a genuine
// round transition and the demo must rewind.
constexpr int kDemoRoundRestoreSettleTicks = 12;
constexpr DemoRoundEventAction DecideDemoRoundEvent(
    bool roundEventActive, int restoreSettleTicksRemaining,
    bool bothFightersAlive) {
    if (!roundEventActive) return DemoRoundEventAction::Continue;
    return restoreSettleTicksRemaining > 0 && bothFightersAlive
        ? DemoRoundEventAction::NormalizeRestoreTransient
        : DemoRoundEventAction::RestoreBaseline;
}

// A Snapshot sampled before restore transactions is invalidated by any
// restore; the explicit terminal latch is a second defence for handoff-only paths.
constexpr bool SnapshotCanResumeAfterRestore(bool restoredWorldThisTick,
                                             bool demoActive,
                                             bool terminalHandoffThisTick) {
    return !restoredWorldThisTick && !demoActive &&
           !terminalHandoffThisTick;
}

// EFZ can cancel a move into another instance with the same move ID; the move
// frame counter (+0xA) resets to the animation head at that boundary. Looping
// animations (air IC float, air dashes, whiffed-air falling tails) also rewind
// their frame every loop pass, so a same-ID rewind counts only at the head.
constexpr short kMoveRestartFrameMax = 2;
constexpr bool IsMoveInstanceEdge(short currentMove, short currentFrame,
                                  short previousMove, short previousFrame) {
    return currentMove != previousMove ||
           (currentFrame < previousFrame && currentFrame <= kMoveRestartFrameMax);
}

// A same-ID rewind is a new action only when EFZ's own input poll consumed the
// button that produces that move (animation tails can wrap 3->2 with no new
// action); a move-ID change is authoritative without corroboration.
constexpr bool IsCausallyCredibleMoveInstanceEdge(
    short currentMove, short currentFrame,
    short previousMove, short previousFrame,
    bool matchingAttackEdge) {
    if (currentMove != previousMove) return true;
    return matchingAttackEdge &&
           IsMoveInstanceEdge(currentMove, currentFrame,
                              previousMove, previousFrame);
}

// Recorder-created direct multi-hits may finish only after at least one exact
// hit plus a structural continuation proof.
constexpr bool FlexibleMultiHitCanFinalize(bool allowPartialHits,
                                           int observedHits,
                                           bool moveEnded,
                                           bool expectedNextActionStarted,
                                           bool comboBoundaryObserved) {
    return allowPartialHits && observedHits > 0 &&
           (moveEnded || expectedNextActionStarted || comboBoundaryObserved);
}

// Strict playback grades action instances, not move-ID changes: a repeated
// normal rewinds the frame while keeping the ID.  Two transitions stay legal:
// recorder-authored automatic phases flattened into one Step::moveIds set, and
// a delayed-hit step overlapping the next authored action.
enum class StrictActionInstanceDecision {
    Ignore,
    AcceptExpected,
    AcceptAutomaticPhase,
    AcceptNextOverlap,
    SkipOptional,
    RejectUnexpected,
};

constexpr StrictActionInstanceDecision DecideStrictActionInstance(
    bool instanceEdge,
    bool comboStepMove,
    bool currentStepArmed,
    bool matchesCurrentStep,
    bool previousMoveMatchesCurrentStep,
    bool matchesPreviousStep,
    bool previousMoveMatchesPreviousStep,
    bool moveIdChanged,
    bool matchesNextOverlap,
    bool matchesOptionalAhead) {
    if (!instanceEdge || !comboStepMove) {
        return StrictActionInstanceDecision::Ignore;
    }

    const bool currentAutomaticPhase = currentStepArmed && moveIdChanged &&
        matchesCurrentStep && previousMoveMatchesCurrentStep;
    const bool previousAutomaticPhase = moveIdChanged &&
        matchesPreviousStep && previousMoveMatchesPreviousStep;
    if (currentAutomaticPhase || previousAutomaticPhase) {
        return StrictActionInstanceDecision::AcceptAutomaticPhase;
    }
    if (currentStepArmed && matchesNextOverlap) {
        return StrictActionInstanceDecision::AcceptNextOverlap;
    }
    if (matchesCurrentStep && !currentStepArmed) {
        return StrictActionInstanceDecision::AcceptExpected;
    }
    if (matchesOptionalAhead) {
        return StrictActionInstanceDecision::SkipOptional;
    }
    return StrictActionInstanceDecision::RejectUnexpected;
}

// A cancel destination is creditable only on a fresh instance and, when the
// author names sources, when the immediately previous move is one of them.
constexpr bool DirectTransitionSatisfied(bool destinationInstanceEdge,
                                         bool sourceRequired,
                                         bool previousSourceMatches) {
    return destinationInstanceEdge &&
           (!sourceRequired || previousSourceMatches);
}

// In a true blockstring the defender need not leave blockstun between
// contacts; a fresh reaction is a move change or a same-ID restart at the head.
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

// Akiko 41236A/B/C is one authored input with two consecutive move IDs:
// startup 263/265/267 then automatic hit phase 264/266/268 (one recipe step).
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

// Rumi's command throw always enters move 251 without another input; the later
// 236A/B/C choice (262/252/306) is a real input and is not folded in.
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

// Format 1 observes snapshots, not the game-thread transaction: most
// boundaries expose a zero-combo sample, a frame-perfect wake-up hit can
// instead reset N directly to a smaller positive count.
constexpr SampledComboBoundary DetectSampledComboBoundary(
    bool wasAlive, bool isAlive, int previousCombo, int currentCombo) {
    if (wasAlive && !isAlive) return SampledComboBoundary::VisibleRecovery;
    if (wasAlive && isAlive && previousCombo > 0 && currentCombo > 0 &&
        currentCombo < previousCombo) {
        return SampledComboBoundary::CounterRollover;
    }
    return SampledComboBoundary::None;
}

// If recovery and the next collision share one native transaction, EFZ rolls
// the counter directly from N to a smaller positive M instead of exposing 0.
constexpr bool ContactBeginsNewComboBaseline(int comboBefore,
                                             int comboAfter) {
    return comboBefore == 0 ||
           (comboBefore > 0 && comboAfter > 0 && comboAfter < comboBefore);
}

constexpr bool ExactContactBeginsNewCombo(bool anyPriorLanded,
                                          bool committedHit,
                                          int comboBefore,
                                          int comboAfter,
                                          unsigned int sequence,
                                          unsigned int priorLandedSequence) {
    return anyPriorLanded && committedHit &&
           ContactBeginsNewComboBaseline(comboBefore, comboAfter) &&
           sequence != 0 && sequence > priorLandedSequence;
}

// A hit/contact requirement cannot be satisfied before the required recovery
// edge is consumed; otherwise the take is still one continuous combo.
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
// transition it produces; never satisfies a step, only postpones the drop.
struct DelayedActionWindow {
    // A buffered cancel legitimately holds the press until the cancel point
    // opens (edge can precede the instance edge by 3-20 visual frames), so the
    // grace is 90 ticks (~30 frames).  It arms only while the commit step is
    // CURRENT and the pressed button matches its inputMask.
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

        // Remember an attack sampled at the deadline even if frozen; its
        // transition may appear on the next sample.
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

// Hit-variant mechanics keep the same move ID but change damage by thousands;
// ordinary proration variance stays within a few hundred.  Recorded delta 0
// means "no damage requirement".
constexpr int kStepDamageTolerance = 500;
constexpr bool StepDamageDiverged(int recordedDelta, int observedDelta) {
    if (recordedDelta <= 0) return false;
    const int diff = observedDelta - recordedDelta;
    return diff > kStepDamageTolerance || diff < -kStepDamageTolerance;
}

// 192-Hz internal-tick unit; allowance = twice the observed gap plus one fail
// window, ceiling 5760 (30 s).
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
        // Zero combo never owns score for a new segment.
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
