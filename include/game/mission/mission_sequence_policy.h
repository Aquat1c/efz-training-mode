#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>

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

// Closing a recorder/menu surface is not itself permission to resume capture.
// Two fresh, physically neutral P1 polls are required so the confirm/back
// button that closed the UI cannot become the first input in the take.
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

// A recorder handoff's physical pause and its input quarantine are separate
// gates. Native character-input polls do not advance under EFZ's official
// Practice pause, so keeping the pause surface until neutral polls arrive
// deadlocks the handoff. Once the protected command/close transaction is done
// and no UI remains visible, let the world poll again while the zero-input
// lease continues to suppress the button that dismissed the menu.
constexpr bool RecorderPhysicalPauseReleaseReady(bool handoffReady,
                                                  bool anyMenuVisible) {
    return handoffReady && !anyMenuVisible;
}

// Pending failed presses are retained for Review, but only a press close to the
// current transition can prove that a table-known follow-up was player-entered.
// Historical unmatched input must not turn later automatic hit phases into
// authored recipe steps.
constexpr bool RecorderAttackBatchIsFresh(int currentFrame, int batchFrame,
                                          int windowTicks) {
    const int age = currentFrame - batchFrame;
    return age >= 0 && age <= windowTicks;
}

// The recorder observes the button edge before EFZ publishes the resulting
// move instance.  Keep that causal bridge bounded, but long enough for the
// game's buffered specials/cancels (the same 30-visual-frame envelope used by
// the runner's delayed-button commit grace).
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

// Extract only notation-strength tokens, not arbitrary letters inside labels.
// This accepts 5A, c.5B, j.C, 214A/B/C and the character-specific 5S spelling.
// IC/BIC/FIC are always performed with C.  A bare descriptive name such as FM
// deliberately stays unknown and falls back to the observed causal edge.
constexpr uint8_t ExpectedAttackMaskFromNotation(const char* notation) {
    if (notation == nullptr) return 0;
    std::size_t length = 0;
    while (notation[length] != '\0') ++length;
    return ExpectedAttackMaskFromNotationRange(notation, 0, length);
}

// Route labels retain their full history (for example FM~A~B~C), but recorder
// causality belongs to the final player-entered step.  Return the newest `~`
// segment containing a strength token; direction-only suffixes such as
// `~5B~4/5/6` therefore correctly fall back to B.
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

// Newest-first selection matters when a failed press and the buffered press
// that actually produced the move coexist in the journal.  A known strength
// skips unrelated fresher edges; unknown notation accepts only a recent edge
// and keeps all older/unmatched presses as diagnostics.
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

// Catalogued automatic and internal phases are aliases of the preceding
// player action.  A coincidental button edge cannot turn one into a new step;
// player-selected continuations carry MoveRole::InputFollowup instead.
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

// Mission capture and demonstration playback are deterministic transactions.
// Persistent Practice helpers (auto jump/airtech, random guard/block, recovery,
// and configured auto-action triggers) must not contribute unrecorded input or
// state changes while either transaction owns the match.  The helpers are
// suppressed rather than disabled, so the user's settings remain untouched and
// resume naturally after the transaction's terminal boundary.
constexpr bool PracticeAutomationSuppressed(bool demoActive,
                                             bool recorderCaptureActive) {
    return demoActive || recorderCaptureActive;
}

enum class DemoBaselineSource {
    None = 0,
    RunnerCheckpoint,
    EmbeddedDump,
};

// An authored embedded state is part of the mission contract, not an optional
// optimization.  If the exact restore backend is unavailable, the mission
// must remain unavailable instead of silently rebuilding a different baseline
// from value-level setup.
constexpr bool EmbeddedSavestateCanLoad(bool hasEmbeddedSavestate,
                                        bool stateDumpAvailable) {
    return !hasEmbeddedSavestate || stateDumpAvailable;
}

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

// The ordinary trial recipe can double as a read-only playback timeline, but
// only for a loaded mission.  PREPARING/RESTORING own their transition UI and
// an unsaved recorder preview has no runner recipe whose cursor can be graded.
// Keep this predicate shared by the engine observer and render routing so they
// cannot drift into a visible-but-frozen (or tracked-but-hidden) state again.
constexpr bool DemoTrialRecipeActive(bool demoPlaying, bool recorderPreview,
                                     bool runnerActive, bool hasSteps) {
    return demoPlaying && !recorderPreview && runnerActive && hasSteps;
}

// When the macro ends on the same monitor sample that satisfies the final
// action, keep the frozen Playing presentation alive long enough to cross at
// least a few ordinary 60 Hz render frames. Without this, zero-tail clips can
// advance the cursor and enter Restoring before EndScene ever draws it.
constexpr int kDemoTerminalRecipeHoldTicks = 12;

// Macro playback becomes Idle once its final input has been consumed and its
// command activation guard retires. The resulting attack/projectile can still
// be in startup. Keep the world running under exclusive neutral for a bounded
// settlement window so delayed final contacts can finish the authored recipe.
constexpr int kDemoRecipeSettleTimeoutTicks = 768; // four seconds at 192 Hz

enum class DemoRecipeEndAction {
    ContinuePlayback = 0,
    SettleFinalContacts,
    HoldTerminalFrame,
    FailThenHold,
};

// Input playback ending and trial grading ending are separate events. A move
// can be activated while its direct/projectile contact is still pending.
constexpr DemoRecipeEndAction DecideDemoRecipeEnd(
    bool macroIdle, bool runnerTerminal, int settleTicksRemaining) {
    if (!macroIdle) return DemoRecipeEndAction::ContinuePlayback;
    if (runnerTerminal) return DemoRecipeEndAction::HoldTerminalFrame;
    return settleTicksRemaining == 1
        ? DemoRecipeEndAction::FailThenHold
        : DemoRecipeEndAction::SettleFinalContacts;
}

// Demo recipe grading is presentation-only.  A divergent authored clip may
// latch its failed step for the bar, but it must never count as a player try or
// arm the runner's checkpoint auto-retry while exclusive playback owns P1.
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

// Loading a Revival checkpoint restores an active-round image, but EFZ can
// re-publish its saved round-event fields on the first live update after the
// load returns.  That one-frame handoff is not a new intro/KO.  Give the demo
// a short, bounded startup window in which such a field reassertion is
// normalized; once the window closes, the same signal is a genuine round
// transition and the demonstration must rewind immediately.
constexpr int kDemoRoundRestoreSettleTicks = 12;
constexpr DemoRoundEventAction DecideDemoRoundEvent(
    bool roundEventActive, int restoreSettleTicksRemaining,
    bool bothFightersAlive) {
    if (!roundEventActive) return DemoRoundEventAction::Continue;
    return restoreSettleTicksRemaining > 0 && bothFightersAlive
        ? DemoRoundEventAction::NormalizeRestoreTransient
        : DemoRoundEventAction::RestoreBaseline;
}

// A Snapshot is sampled before exact mission/demo restore transactions run in
// Engine::Tick. Any restore therefore invalidates that sample, even when a
// demo also reaches Idle before the bottom-of-tick runner gate. Keep the
// explicit terminal latch as a second defence for handoff-only paths.
constexpr bool SnapshotCanResumeAfterRestore(bool restoredWorldThisTick,
                                             bool demoActive,
                                             bool terminalHandoffThisTick) {
    return !restoredWorldThisTick && !demoActive &&
           !terminalHandoffThisTick;
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

// A same-ID frame rewind is only a credible new action when EFZ's own input
// poll also consumed the button that produces that move. Air-normal/airdash
// animation tails can wrap 3->2 with no new action at all; treating that visual
// loop as a second j.A made otherwise correct recordings fail. A move-ID
// change remains authoritative and needs no input corroboration.
constexpr bool IsCausallyCredibleMoveInstanceEdge(
    short currentMove, short currentFrame,
    short previousMove, short previousFrame,
    bool matchingAttackEdge) {
    if (currentMove != previousMove) return true;
    return matchingAttackEdge &&
           IsMoveInstanceEdge(currentMove, currentFrame,
                              previousMove, previousFrame);
}

// Recorder-created direct multi-hits may make fewer contacts at different
// spacing. They are allowed to finish only after at least one exact hit and a
// structural continuation proof; merely touching once while the move remains
// active is not enough to skip the rest of its authored timing.
constexpr bool FlexibleMultiHitCanFinalize(bool allowPartialHits,
                                           int observedHits,
                                           bool moveEnded,
                                           bool expectedNextActionStarted,
                                           bool comboBoundaryObserved) {
    return allowPartialHits && observedHits > 0 &&
           (moveEnded || expectedNextActionStarted || comboBoundaryObserved);
}

// Strict trial playback grades action *instances*, not merely move-ID changes.
// In particular, pressing a repeated normal again rewinds the move frame while
// leaving the ID unchanged.  Once the current recipe step is already armed,
// that is another authored action and must not be silently absorbed by the
// first instance.
//
// Two transitions deliberately remain legal:
//  - recorder-authored automatic phases are flattened into one Step::moveIds
//    set, so an ID-changing transition whose source and destination both match
//    that step is still the same action;
//  - a delayed-hit step may overlap the next authored action.  Repeated-ID
//    recipes use this path too when the prior contact and next cancel arrive in
//    the same monitor sample.
// A same-ID animation that simply keeps advancing is not an instance edge and
// therefore never reaches the strict decision at all (ordinary multi-hit
// phases remain untouched).
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

// Resolver transactions can prove a recovery boundary that the sampled combo
// counter never exposes. Usually the first Part-2 contact sees comboBefore==0;
// if recovery and the next collision share the native transaction, EFZ can
// instead expose the counter rolling directly from N to a smaller positive M.
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
