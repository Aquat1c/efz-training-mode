#include "game/mission/tutorial_session.h"
#include "game/mission/tutorial_episode_policy.h"
#include "game/mission/tutorial_layout_policy.h"
#include "game/mission/tutorial_text_policy.h"
#include "game/mission/tutorial_state_policy.h"
#include "input/input_hook.h"

#include <cstdlib>
#include <iostream>

namespace {

void Check(bool condition, const char* message) {
    if (condition) return;
    std::cerr << "FAILED: " << message << '\n';
    std::exit(1);
}

} // namespace

int main() {
    using namespace Mission::TutorialSession;
    namespace StatePolicy = Mission::TutorialStatePolicy;
    namespace EpisodePolicy = Mission::TutorialEpisodePolicy;
    namespace LayoutPolicy = Mission::TutorialLayoutPolicy;
    namespace TextPolicy = Mission::TutorialTextPolicy;
    namespace HookPolicy = InputHookPolicy;

    Check(HookPolicy::TutorialP2Route(
              HookPolicy::TutorialP2InputSource::Neutral) ==
              HookPolicy::ProcessRoute::NativePoll &&
          HookPolicy::TutorialP2Route(
              HookPolicy::TutorialP2InputSource::MotionQueue) ==
              HookPolicy::ProcessRoute::NativePoll &&
          HookPolicy::TutorialP2Route(
              HookPolicy::TutorialP2InputSource::ImmediatePress) ==
              HookPolicy::ProcessRoute::NativePoll &&
          HookPolicy::TutorialP2Route(
              HookPolicy::TutorialP2InputSource::BufferFreeze) ==
              HookPolicy::ProcessRoute::NativePoll,
          "tutorial P2 queue, immediate, and frozen inputs all retain EFZ's native processor");
    Check(HookPolicy::TutorialP2AllowsNormalPulse(
              HookPolicy::TutorialP2InputSource::Neutral) &&
          !HookPolicy::TutorialP2AllowsNormalPulse(
              HookPolicy::TutorialP2InputSource::MotionQueue) &&
          !HookPolicy::TutorialP2AllowsNormalPulse(
              HookPolicy::TutorialP2InputSource::ImmediatePress) &&
          !HookPolicy::TutorialP2AllowsNormalPulse(
              HookPolicy::TutorialP2InputSource::BufferFreeze),
          "tutorial normals wait for exclusive scripted input owners to become neutral");

    Check(!SessionMenuAllowed(Phase::Preparing) &&
          !SessionMenuAllowed(Phase::DemoSuspended) &&
          SessionMenuAllowed(Phase::Intro) &&
          SessionMenuAllowed(Phase::TaskActive) &&
          SessionMenuAllowed(Phase::Error),
          "session menu cannot interrupt startup or the demo restore handoff");
    Check(PersistedAttemptDelta(-1) == 0 &&
          PersistedAttemptDelta(0) == 0 &&
          PersistedAttemptDelta(1) == 1 &&
          PersistedAttemptDelta(3) == 3,
          "progress persists real misses/restarts and preserves a flawless zero");

    constexpr LayoutPolicy::Typography smallUi =
        LayoutPolicy::TypographyFor(0.70f);
    constexpr LayoutPolicy::Typography normalUi =
        LayoutPolicy::TypographyFor(1.00f);
    constexpr LayoutPolicy::Typography largeUi =
        LayoutPolicy::TypographyFor(1.50f);
    Check(smallUi.prosePx >= 12.0f && smallUi.taskPx >= 11.0f &&
          smallUi.metaPx >= 9.0f && normalUi.prosePx == 13.0f &&
          largeUi.prosePx <= 16.0f && largeUi.pageTitlePx <= 20.0f,
          "tutorial typography stays readable and bounded at every supported UI scale");
    Check(LayoutPolicy::WithinCanvas(LayoutPolicy::ActiveRequirements()) &&
          LayoutPolicy::WithinCanvas(LayoutPolicy::BelowStatsRequirements()) &&
          LayoutPolicy::WithinCanvas(LayoutPolicy::ModalActions()) &&
          LayoutPolicy::WithinCanvas(LayoutPolicy::PageBanner()) &&
          LayoutPolicy::WithinCanvas(LayoutPolicy::PageCard()) &&
          LayoutPolicy::WithinCanvas(LayoutPolicy::PageActions()) &&
          LayoutPolicy::SafeBandsDoNotOverlap() &&
          LayoutPolicy::ActiveRequirements().x <= 16.0f &&
          LayoutPolicy::ActiveRequirements().y >= 96.0f &&
          LayoutPolicy::ActiveRequirements().x +
              LayoutPolicy::ActiveRequirements().w <= 400.0f &&
          LayoutPolicy::PageBanner().y >= 92.0f &&
          LayoutPolicy::ModalActions().y + LayoutPolicy::ModalActions().h <= 448.0f,
          "live requirements use the upper-left Trial band and clear native meters");
    constexpr LayoutPolicy::Rect upperRequirements =
        LayoutPolicy::RequirementBounds(false);
    constexpr LayoutPolicy::Rect belowStatsRequirements =
        LayoutPolicy::RequirementBounds(true);
    Check(upperRequirements.x == 8.0f && upperRequirements.y == 108.0f &&
          upperRequirements.w == 372.0f && upperRequirements.h == 252.0f &&
          belowStatsRequirements.x == 8.0f && belowStatsRequirements.y == 206.0f &&
          belowStatsRequirements.w == 372.0f && belowStatsRequirements.h == 206.0f &&
          belowStatsRequirements.y + belowStatsRequirements.h <= 412.0f,
          "authored requirement placement selects one stable stats-safe anchor");
    Check(LayoutPolicy::MaxVisibleRequirements() == 8 &&
          LayoutPolicy::RequirementWindowStart(12, 8, 0) == 0 &&
          LayoutPolicy::RequirementWindowStart(12, 8, 6) == 2 &&
          LayoutPolicy::RequirementWindowStart(12, 8, 11) == 4 &&
          LayoutPolicy::RequirementWindowStart(3, 8, 2) == 0,
          "oversized authored checklists keep the current requirement in view");
    Check(TextPolicy::NotationToRich("normal block") == "normal block" &&
          TextPolicy::NotationToRich("~5A") == "~ {input:5A}" &&
          TextPolicy::NotationToRich("delayed 6C / 4C throw") ==
              "delayed {input:6C}  /  {input:4C throw}" &&
          TextPolicy::NotationToRich("22C / BIC") ==
              "{input:22C}  /  {term:BIC}" &&
          TextPolicy::NotationToRich("j.B on wakeup") ==
              "{input:j.B on wakeup}",
          "tutorial notation separates inputs, annotations, delay marks, and IC terms");
    Check(TextPolicy::NormalizeUiText("it\xE2\x80\x99s") == "it's",
          "tutorial prose normalizes punctuation outside the compact font range");
    Check(TextPolicy::HumanizeIdentifier("notation.setup_marker") ==
              "SETUP MARKER",
          "unlabeled authored tasks never expose internal identifiers");

    Check(EpisodePolicy::IsInjectableAction("66A") &&
          EpisodePolicy::IsInjectableAction("66C") &&
          EpisodePolicy::IsInjectableAction("662A") &&
          EpisodePolicy::IsInjectableAction("5A>5B>2C") &&
          EpisodePolicy::IsDashNormalAction("66A") &&
          EpisodePolicy::IsDashNormalAction("66C") &&
          EpisodePolicy::IsContactChainAction("5A>5B>2C"),
          "RG drills expose their dedicated dash-normal and contact-chain scripts");
    Check(EpisodePolicy::IsForwardDashApproachState(163) &&
          EpisodePolicy::IsForwardDashApproachState(164) &&
          EpisodePolicy::IsForwardDashApproachState(178) &&
          EpisodePolicy::IsForwardDashApproachState(250) &&
          !EpisodePolicy::IsForwardDashApproachState(165) &&
          !EpisodePolicy::IsForwardDashApproachState(166) &&
          !EpisodePolicy::IsForwardDashApproachState(1),
          "scripted approaches accept every forward-dash phase but never walking or backdash");
    Check(EpisodePolicy::FixedGuardPracticeMode("stand") == 0 &&
          EpisodePolicy::FixedGuardPracticeMode("crouch") == 2,
          "fixed guard episodes select the native Practice stand/crouch mode");
    Check(EpisodePolicy::IsSupportedScriptAction("66B", 0) &&
          !EpisodePolicy::IsSupportedScriptAction("66B", 1) &&
          EpisodePolicy::IsSupportedScriptAction("623C", 1) &&
          !EpisodePolicy::IsSupportedScriptAction("jump", 0) &&
          !EpisodePolicy::IsSupportedScriptAction("5A>5B>2C", 0) &&
          !EpisodePolicy::IsSupportedScriptAction("63214C", 0),
          "schema, preflight, and runtime share one multi-action script vocabulary");
    Check(EpisodePolicy::UsesNativeWakeProducer(
              "macro", "trigger", "onWakeup") &&
          !EpisodePolicy::NeedsTutorialWriterLeases(
              "macro", "trigger", "onWakeup") &&
          !EpisodePolicy::UsesNativeWakeProducer(
              "macro", "taskArmed", "onWakeup") &&
          EpisodePolicy::NeedsTutorialWriterLeases(
              "macro", "taskArmed", "onWakeup"),
          "native wake pre-buffering never competes with tutorial input writers");
    Check(EpisodePolicy::SuppressUserAutoActions(true, false) &&
          !EpisodePolicy::SuppressUserAutoActions(true, true) &&
          !EpisodePolicy::SuppressUserAutoActions(false, false) &&
          EpisodePolicy::ResolveRuntimeAutoActionTarget(true, false, 1) == 0 &&
          EpisodePolicy::ResolveRuntimeAutoActionTarget(true, true, 1) == 2 &&
          EpisodePolicy::ResolveRuntimeAutoActionTarget(false, false, 1) == 1,
          "tutorials suppress user auto-actions while native wake remains a P2-only exception");
    Check(EpisodePolicy::ExpectedAttackMatches({252}, 252) &&
          !EpisodePolicy::ExpectedAttackMatches({252}, 203) &&
          EpisodePolicy::ExpectedAttackMatches({}, 264) &&
          !EpisodePolicy::ExpectedAttackMatches({}, 3),
          "episode acknowledgement accepts only its authored attack states");
    int wakeAction = -1;
    int wakeStrength = -1;
    Check(EpisodePolicy::WakeAutoActionForAction(
              "214C", wakeAction, wakeStrength) &&
          wakeAction == ACTION_QCB && wakeStrength == 2 &&
          !EpisodePolicy::WakeAutoActionForAction(
              "63214C", wakeAction, wakeStrength),
          "the authored Misaki wakeup 214C maps to native QCB strength C only");
    Check(EpisodePolicy::IsScriptAttackInstanceEdge(263, 0, 231, 8) &&
          EpisodePolicy::IsScriptAttackInstanceEdge(200, 1, 200, 8) &&
          !EpisodePolicy::IsScriptAttackInstanceEdge(200, 9, 200, 8) &&
          !EpisodePolicy::IsScriptAttackInstanceEdge(200, 3, 200, 10) &&
          !EpisodePolicy::IsScriptAttackInstanceEdge(3, 0, 200, 8),
          "script acknowledgement accepts attack cancels and same-ID re-entry only at a new instance");
    Check(EpisodePolicy::ScriptCancelDelaySatisfied(8, 8, false) &&
          !EpisodePolicy::ScriptCancelDelaySatisfied(7, 8, false) &&
          !EpisodePolicy::ScriptCancelDelaySatisfied(8, 8, true),
          "a destination cancel delay advances only after its unfrozen authored wait");
    Check(!EpisodePolicy::CanInjectContactChainFollowup(true, true) &&
          !EpisodePolicy::CanInjectContactChainFollowup(false, false) &&
          EpisodePolicy::CanInjectContactChainFollowup(true, false),
          "a scripted RG follow-up waits until the observed contact freeze ends");
    Check(EpisodePolicy::RecoilGuardAnswerDelayTicks(RG_STAND_ID) ==
              RG_STAND_FREEZE_DURATION &&
          EpisodePolicy::RecoilGuardAnswerDelayTicks(RG_CROUCH_ID) ==
              RG_CROUCH_FREEZE_DURATION &&
          EpisodePolicy::RecoilGuardAnswerDelayTicks(RG_AIR_ID) ==
              RG_AIR_FREEZE_DURATION &&
          EpisodePolicy::RecoilGuardAnswerDelayTicks(0) < 0 &&
          !EpisodePolicy::RecoilGuardAnswerDelaySatisfied(RG_STAND_ID, 0) &&
          !EpisodePolicy::RecoilGuardAnswerDelaySatisfied(
              RG_STAND_ID, RG_STAND_FREEZE_DURATION - 1) &&
          EpisodePolicy::RecoilGuardAnswerDelaySatisfied(
              RG_STAND_ID, RG_STAND_FREEZE_DURATION),
          "an rg_answer never injects on the RG edge and becomes ready at the auto-action delay");
    Check(EpisodePolicy::HasTimedCue("GET READY", 45) &&
          !EpisodePolicy::HasTimedCue("", 45) &&
          !EpisodePolicy::HasTimedCue("GET READY", 0) &&
          EpisodePolicy::CueStartsOnArm("taskArmed") &&
          EpisodePolicy::CueStartsOnArm("playerState") &&
          !EpisodePolicy::CueStartsOnArm("trigger") &&
          EpisodePolicy::CueStartsOnArm("trigger", "onWakeup") &&
          !EpisodePolicy::CueStartsOnTrigger("trigger", false) &&
          EpisodePolicy::CueStartsOnTrigger("trigger", true),
          "episode cues start on the authored arm or trigger boundary");
    uint32_t variantSeed = 0x12345u;
    const auto balancedPair =
        EpisodePolicy::BuildBalancedVariantAssignment(variantSeed, 2, 2);
    Check(balancedPair.size() == 2 && balancedPair[0] != balancedPair[1] &&
          balancedPair[0] < 2 && balancedPair[1] < 2,
          "a two-exercise variant group receives both hidden outcomes exactly once");
    const auto balancedCycles =
        EpisodePolicy::BuildBalancedVariantAssignment(variantSeed, 6, 3);
    Check(balancedCycles.size() == 6 &&
          balancedCycles[0] != balancedCycles[1] &&
          balancedCycles[0] != balancedCycles[2] &&
          balancedCycles[1] != balancedCycles[2] &&
          balancedCycles[3] != balancedCycles[4] &&
          balancedCycles[3] != balancedCycles[5] &&
          balancedCycles[4] != balancedCycles[5],
          "each complete grouped-variant cycle is a without-replacement permutation");
    Check(ShouldCarryCommitEdge(true, true, true) &&
          !ShouldCarryCommitEdge(false, true, true) &&
          !ShouldCarryCommitEdge(true, true, false),
          "a cancel button edge crosses into the next commit only on its frozen boundary");
    Check(ShouldRejectUnprovenCommit(true, true, false) &&
          !ShouldRejectUnprovenCommit(true, true, true) &&
          !ShouldRejectUnprovenCommit(false, true, false),
          "a one-use system action with the wrong button becomes a formal failure");
    Check(!ShouldResumeReviewAfterRelease(false, true, false) &&
          !ShouldResumeReviewAfterRelease(true, false, false) &&
          !ShouldResumeReviewAfterRelease(true, true, true) &&
          ShouldResumeReviewAfterRelease(true, true, false),
          "review remains frozen until its closing physical UI input is released");
    Check(FlickerICProofSatisfied(true, true, true, true, true, false, true) &&
          !FlickerICProofSatisfied(true, true, true, true, false, false, true) &&
          !FlickerICProofSatisfied(true, true, true, true, true, true, true) &&
          !FlickerICProofSatisfied(true, true, false, true, true, false, true) &&
          !FlickerICProofSatisfied(true, true, true, false, true, false, true) &&
          !FlickerICProofSatisfied(true, true, true, true, true, false, false),
          "FIC proof needs the exact cancel, C edge, safe spacing, live projectile, and no contact");
    Check(IsNewForbiddenAbsenceAttack(true, 200) &&
          IsNewForbiddenAbsenceAttack(true, 220) &&
          !IsNewForbiddenAbsenceAttack(false, 200) &&
          !IsNewForbiddenAbsenceAttack(true, 199),
          "absence grading rejects only a newly started attack, not inherited recovery");
    Check(IsDummyAttackStart(96, 258) &&
          !IsDummyAttackStart(258, 260) &&
          !IsDummyAttackEnd(false, 258, 0) &&
          IsDummyAttackEnd(true, 258, 0),
          "reversal bait cannot finish until a real dummy attack starts and ends");
    Check(IsCarryContinuity("continue") &&
          !IsCarryContinuity("sameCombo") &&
          !IsCarryContinuity("independent") &&
          ContinuationRetryHead(
              {"independent", "continue", "continue", "sameCombo"}, 2) == 0 &&
          ContinuationRetryHead(
              {"independent", "continue", "continue", "sameCombo"}, 3) == 3 &&
          ContinuationRetryHead(
              {"sameCombo", "sameCombo"}, 1) == 1,
          "only explicit continue tasks rewind through a live-state carry chain");

    Check(StatePolicy::IsCoherentField("rf") &&
          !StatePolicy::IsCoherentField("untech"),
          "resource fields remain owned by coherent_state");
    Check(StatePolicy::IsJuggleField("untech") &&
          StatePolicy::IsJuggleField("launched") &&
          StatePolicy::IsJuggleField("airtech") &&
          !StatePolicy::IsJuggleField("recoverable"),
          "juggle_state exposes only proven counters and move classifiers");
    Check(StatePolicy::Compare(24.0, "gt", 0.0) &&
          StatePolicy::Compare(0.0, "eq", 0.0) &&
          !StatePolicy::Compare(1.0, "unknown", 1.0),
          "goal-state comparison is exact and rejects unknown operators");
    Check(StatePolicy::IsUntechActive(1.0) &&
          !StatePolicy::IsUntechActive(0.0) &&
          StatePolicy::IsUntechEmpty(0.0) &&
          StatePolicy::IsUntechEmpty(-1.0),
          "juggle observation opens on positive untech and closes only when it empties");
    Check(StatePolicy::IsWakeExit(true, false) &&
          !StatePolicy::IsWakeExit(true, true) &&
          !StatePolicy::IsWakeExit(false, false),
          "wakeup timing opens on every groundtech exit, including a direct meaty hit");
    Check(StatePolicy::IsWithinWakeWindow(0, 5) &&
          StatePolicy::IsWithinWakeWindow(5, 5) &&
          !StatePolicy::IsWithinWakeWindow(6, 5) &&
          !StatePolicy::IsWithinWakeWindow(-1, 5),
          "a five-tick meaty window rejects two full actionable visual frames");
    Check(StatePolicy::BranchPhaseForOutcomeEdge(true, false) == 1 &&
          StatePolicy::BranchPhaseForOutcomeEdge(false, true) == 2 &&
          StatePolicy::BranchPhaseForOutcomeEdge(false, false) == 3,
          "hit-confirm starter consumes same-snapshot hit/block edges instead of losing them");

    DemoResumeTarget target = DecideDemoResumeTarget(
        Phase::Feedback, true, true, 2, -1);
    Check(target.phase == Phase::Complete && target.task == -1,
          "a demo after final-task success resumes at lesson completion");

    target = DecideDemoResumeTarget(Phase::Feedback, true, false, 1, 2);
    Check(target.phase == Phase::TaskActive && target.task == 2,
          "a demo after mid-lesson success resumes at the first pending task");

    target = DecideDemoResumeTarget(Phase::Feedback, false, false, 1, 0);
    Check(target.phase == Phase::TaskActive && target.task == 1,
          "a demo after failed feedback retries the same task");

    target = DecideDemoResumeTarget(Phase::TaskActive, true, false, 2, 1);
    Check(target.phase == Phase::TaskActive && target.task == 1,
          "an active lesson resumes at its first pending task");

    target = DecideDemoResumeTarget(Phase::Review, true, true, 2, -1);
    Check(target.phase == Phase::Review && target.task == -1,
          "a demonstration opened while reviewing returns to the review page");

    target = DecideDemoResumeTarget(Phase::ConfirmExit, true, false, 0, 0);
    Check(target.phase == Phase::ConfirmExit && target.task == -1,
          "a demonstration preserves the page exit confirmation");

    std::cout << "tutorial_session_policy_tests passed\n";
    return 0;
}
