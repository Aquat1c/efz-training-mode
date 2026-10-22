#pragma once
// Tutorial session runtime (TUTORIAL_MODE_DESIGN.md §3/§6):
// BROWSER -> INTRO PAGES -> RELEASE-TO-NEUTRAL -> TASK cue/active ->
// feedback/checkpoint -> COMPLETE.
// The session owns pages, the task controller, completion + the atomic
// progress write, and the lesson-context pause-menu commands. The mission
// Runner stays loader/baseline owner; for tutorialSchema lessons its trial
// validation (TickRun) is bypassed and this session consumes the engine snapshot.
#include <string>
#include <vector>
#include <cstdint>

struct ImDrawList;

namespace Mission { struct Mission; }
namespace Mission::Engine { struct Snapshot; }

namespace Mission::TutorialSession {

enum class Phase : int {
    Idle = 0,
    Preparing,    // runner setup/baseline is not ready; no visible controls
    Intro,        // reading pages (frozen; no attempt can start)
    ConfirmExit,  // page-1 cancel: RETURN TO LESSONS? (frozen)
    NeutralGate,  // release-to-neutral before arming a task
    TaskActive,   // combat task armed; gameplay input live
    Choice,       // choice task (frozen, controller-navigable)
    Feedback,     // success/failure line before checkpoint restore
    Review,       // re-reading pages mid-lesson (frozen; returns to task)
    DemoSuspended,// demonstration owns input/presentation; returns via neutral
    Complete,     // LESSON COMPLETE menu (frozen)
    Error,        // checkpoint/setup failure; frozen until restart/return
};

// DemoSuspended keeps menu commands blocked even after Demo::IsActive()
// becomes false: the restore result must be consumed and freeze/neutral
// acquired before a session command may mutate the phase.
constexpr bool SessionMenuAllowed(Phase phase) {
    return phase != Phase::Preparing && phase != Phase::DemoSuspended;
}

// Progress stores misses/manual restarts, not a one-based run number. A
// flawless clear therefore persists zero attempts.
constexpr int PersistedAttemptDelta(int attemptsThisRun) {
    return attemptsThisRun > 0 ? attemptsThisRun : 0;
}

// Passing directions through here would let the Up/Back that closed the
// previous surface become a jump on resume: keep every gameplay bit
// suppressed until a physically neutral poll.
constexpr uint8_t NeutralGatePublishedMask(uint8_t /*physicalPoll*/) {
    return 0;
}

constexpr bool NeutralGatePollIsReleased(bool sampleValid,
                                         uint8_t physicalPoll) {
    return sampleValid && physicalPoll == 0;
}

constexpr int NeutralGateRequiredFreshPolls() { return 2; }

constexpr uint32_t NeutralGateObservedSerialBaseline(bool sampleValid,
                                                     uint32_t observedSerial) {
    return sampleValid ? observedSerial : 0;
}

constexpr int NeutralGateNeutralPollCount(bool sampleValid, bool freshSample,
                                          uint8_t physicalPoll,
                                          int previousCount) {
    if (!sampleValid || !freshSample) return previousCount;
    return physicalPoll == 0 ? previousCount + 1 : 0;
}

// A successful task is already done before Feedback begins; blindly re-arming
// the old task would reopen completed work instead of entering COMPLETE.
struct DemoResumeTarget {
    Phase phase = Phase::TaskActive;
    int task = -1;
};

constexpr DemoResumeTarget DecideDemoResumeTarget(
    Phase sourcePhase, bool feedbackSuccess, bool completionMet,
    int currentTask, int firstPendingTask) {
    if (sourcePhase == Phase::Intro || sourcePhase == Phase::Review ||
        sourcePhase == Phase::ConfirmExit) {
        return {sourcePhase, -1};
    }
    if (sourcePhase == Phase::Feedback && !feedbackSuccess) {
        return {Phase::TaskActive, currentTask};
    }
    if (sourcePhase == Phase::Complete || completionMet || firstPendingTask < 0) {
        return {Phase::Complete, -1};
    }
    return {Phase::TaskActive, firstPendingTask};
}

// A connected action can complete with its cancel input polled on the same
// frozen sample; once the destination move appears, a missing authored button
// proof is a formal failure (one-use resources such as Blue IC).
constexpr bool ShouldCarryCommitEdge(bool frozenSample,
                                     bool nextRequiresCommit,
                                     bool nextInputMatches) {
    return frozenSample && nextRequiresCommit && nextInputMatches;
}

constexpr bool ShouldRejectUnprovenCommit(bool destinationEdge,
                                          bool requiresCommit,
                                          bool committedInput) {
    return destinationEdge && requiresCommit && !committedInput;
}

// Leaving Review must not age or mutate the frozen combat situation: keep the
// freeze until the closing physical UI input is released (a held EFZ A/B
// could become an attack on the first live poll).
constexpr bool ShouldResumeReviewAfterRelease(bool resumePending,
                                              bool inputActive,
                                              bool anyUiInputHeld) {
    return resumePending && inputActive && !anyUiInputHeld;
}

// A curated FIC task needs more than IC move 167: IC entered directly from the
// authored source on the consumed C edge, source at the proven safe spacing,
// projectile still alive, and no earlier source/projectile contact.
constexpr bool FlickerICProofSatisfied(bool destinationEdge,
                                       bool sourceMatches,
                                       bool committedInput,
                                       bool sourceDistanceSafe,
                                       bool projectileAlive,
                                       bool contactSeen,
                                       bool defenderUntouched) {
    return destinationEdge && sourceMatches && committedInput &&
           sourceDistanceSafe && projectileAlive && !contactSeen &&
           defenderUntouched;
}

// `continue` is the explicit cross-task carry contract (a task built on the
// previous task's live setup cannot be retried from the lesson-start savestate
// alone). `sameCombo` is NOT a carry marker; standalone drills own checkpoints.
inline bool IsCarryContinuity(const std::string& continuity) {
    return continuity == "continue";
}

inline int ContinuationRetryHead(const std::vector<std::string>& continuities,
                                 int currentTask) {
    if (currentTask < 0 || currentTask >= static_cast<int>(continuities.size())) {
        return currentTask;
    }
    while (currentTask > 0 && IsCarryContinuity(continuities[currentTask])) {
        --currentTask;
    }
    return currentTask;
}

// action_absence grades a newly committed attack, not the lingering animation
// that created a carried state before the observation task armed.
constexpr bool IsNewForbiddenAbsenceAttack(bool moveInstanceEdge,
                                           int moveId) {
    return moveInstanceEdge && moveId >= 200;
}

// Reversal bait must see the dummy's real attack start and recover. A fixed
// timeout could otherwise pass when the wakeup trigger or injection never ran.
constexpr bool IsDummyAttackStart(short previousMove, short currentMove) {
    return previousMove < 200 && currentMove >= 200;
}

constexpr bool IsDummyAttackEnd(bool attackSeen, short previousMove,
                                short currentMove) {
    return attackSeen && previousMove >= 200 && currentMove < 200;
}

// Runner::Load hands over here for tutorialSchema lessons; Runner::Unload ends.
bool Begin(const ::Mission::Mission& lesson);
void End(const char* reason);
void NotifyStartupFailure(const std::string& message);
// Savestate preambles cancel process-wide input producers; reconcile tutorial
// leases after that (only an unfinished TaskActive task re-arms its owners).
void NotifyStateLoaded();
bool IsActive();
Phase GetPhase();
// True only after startup has a proven successor owner: EFZ physically paused
// for a frozen surface, or NeutralGate's attributed zero poll live.
bool HasStartupInputOwnership();

// Monitor-thread tick (called from Engine::Tick in place of TickRun).
void Tick(const ::Mission::Engine::Snapshot& s);

// Lesson-menu commands (§3.8). Return false when not applicable now; they
// repeat the phase gates rather than trusting the menu frontend.
bool CommandRetryTask();
bool CommandRestartLesson();
bool CommandReviewLesson();
bool CommandPlayDemo(std::string& message);
bool CommandNextLesson(std::string& message);
bool LessonCleared();
std::string NextLessonName();   // display name for "NEXT LESSON - <name>" ("" = none)
std::string ProgressLabel();    // phase-aware PAGE/TASK/COMPLETE label for pause UI
bool CanRetryCurrentTask();

// Browser-time registry so Next Lesson can resolve stable ids to files. A
// browser rescan stages registrations and publishes atomically; publication is
// skipped while a session is active so a Next Lesson lookup cannot be cleared.
void BeginLessonRegistryRefresh();
void RegisterLessonPath(const std::string& packId, const std::string& lessonId,
                        const std::string& path,
                        const std::string& displayName,
                        const std::string& nextLessonId,
                        bool available = true);
void FinishLessonRegistryRefresh();

// render thread
bool WantsDraw();
void Draw(void* device, ImDrawList* dl);
// Called only after the EndScene hook is detached; releases the Win32-TLS
// snapshot cache without adding a PE TLS directory on XP.
void ReleaseRenderThreadState();

} // namespace Mission::TutorialSession
