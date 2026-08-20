#pragma once
//
// Tutorial session runtime (TUTORIAL_MODE_DESIGN.md §3/§6, P1-P2 spine):
//   TUTORIAL BROWSER → (launch) → INTRO PAGES → RELEASE-TO-NEUTRAL →
//   TASK cue/active → feedback/checkpoint → COMPLETE → NEXT | AGAIN | RETURN
//
// The session owns pages, the task controller (combat adapter + choice
// tasks), completion + the atomic progress write, and the lesson-context
// commands used by the pause menu. The mission Runner remains the loader and
// baseline owner: for tutorialSchema lessons its trial validation (TickRun)
// is bypassed and this session consumes the engine snapshot instead.
//
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

// Preparing owns an unfinished startup transaction. DemoSuspended owns the
// stronger terminal-restore handoff even after Demo::IsActive() becomes false:
// Tutorial must consume the restore result and acquire freeze/neutral before a
// session command is allowed to mutate its phase.
constexpr bool SessionMenuAllowed(Phase phase) {
    return phase != Phase::Preparing && phase != Phase::DemoSuspended;
}

// Progress stores misses/manual restarts, not a one-based run number. A
// flawless clear therefore persists zero attempts.
constexpr int PersistedAttemptDelta(int attemptsThisRun) {
    return attemptsThisRun > 0 ? attemptsThisRun : 0;
}

// The transition from a reading/task-complete surface into live play is a
// release gate, not a motion pre-buffer.  Passing directions through here lets
// the Up/Back input that closed or navigated the previous surface become a
// jump as soon as EFZ resumes.  Keep every gameplay bit suppressed and require
// a physically neutral poll before the task can arm.
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

// Pure policy used when a demonstration hands control back to the lesson.
// A successful task is already marked done before Feedback begins, so resuming
// by blindly arming the old task would reopen completed work (and would reopen
// the final task instead of entering LESSON COMPLETE).
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

// Commit grading must survive the boundary where a connected action completes
// and its cancel input is polled on that same frozen sample. Conversely, once
// the destination move appears, missing the authored button proof is a formal
// failure (important for one-use resources such as Blue IC).
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

// Review is presentation-only: leaving it must neither age nor mutate the
// frozen combat situation.  Keep the page freeze until the physical UI input
// that closed it is released; otherwise a held EFZ A/B button can become an
// attack on the first live gameplay poll.
constexpr bool ShouldResumeReviewAfterRelease(bool resumePending,
                                              bool inputActive,
                                              bool anyUiInputHeld) {
    return resumePending && inputActive && !anyUiInputHeld;
}

// A curated FIC task is stronger than merely seeing IC move 167. The game
// must enter IC directly from the authored source on the consumed C edge, the
// source must have begun at the proven safe spacing, its projectile must still
// exist, and no source/projectile contact may have occurred first.
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

// A task whose live setup was produced by the preceding task cannot be retried
// from the lesson-start savestate in isolation. `continue` is the explicit
// cross-task carry contract. `sameCombo` is deliberately NOT a carry marker: it
// keeps the ordered actions inside one task combo, but two adjacent standalone
// combo drills still own independent checkpoints.
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
// Savestate preambles cancel process-wide input producers. Reconcile tutorial
// leases after that cancellation; only an unfinished TaskActive task re-arms
// its episode owners. Frozen/feedback/completed phases remain in place.
void NotifyStateLoaded();
bool IsActive();
Phase GetPhase();
// True only after startup has a proven successor owner: either EFZ is
// physically paused for a frozen tutorial surface, or NeutralGate's attributed
// zero poll is live. Mission::Engine keeps its dedicated P1 gate until then.
bool HasStartupInputOwnership();

// Monitor-thread tick (called from Engine::Tick in place of TickRun).
void Tick(const ::Mission::Engine::Snapshot& s);

// Lesson-menu commands (§3.8). Return false when not applicable now. The
// command implementations repeat the phase gates rather than trusting only
// the menu frontend.
bool CommandRetryTask();
bool CommandRestartLesson();
bool CommandReviewLesson();
bool CommandPlayDemo(std::string& message);
bool CommandNextLesson(std::string& message);
bool LessonCleared();
std::string NextLessonName();   // display name for "NEXT LESSON - <name>" ("" = none)
std::string ProgressLabel();    // phase-aware PAGE/TASK/COMPLETE label for pause UI
bool CanRetryCurrentTask();

// Browser-time registry so Next Lesson can resolve stable ids to files. A full
// browser rescan stages registrations and publishes them atomically, removing
// files that disappeared since the prior scan. Publication is skipped while a
// lesson session is active so its Next Lesson lookup cannot be cleared midway.
void BeginLessonRegistryRefresh();
void RegisterLessonPath(const std::string& packId, const std::string& lessonId,
                        const std::string& path,
                        const std::string& displayName,
                        const std::string& nextLessonId,
                        bool available = true);
void FinishLessonRegistryRefresh();

// ---- render thread ----
bool WantsDraw();
void Draw(void* device, ImDrawList* dl);
// Called only after the EndScene hook is detached. Releases the explicit
// Win32-TLS snapshot cache without introducing a PE TLS directory on XP.
void ReleaseRenderThreadState();

} // namespace Mission::TutorialSession
