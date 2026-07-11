#include "game/mission/mission_sequence_policy.h"
#include "game/mission/mission_engine.h"

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
    using namespace Mission::SequencePolicy;

    using Recorder = Mission::Engine::Recorder::Phase;
    using Advance = Mission::Engine::Recorder::AdvanceEffect;
    Check(Mission::Engine::Recorder::DecideAdvance(Recorder::PreRecord) ==
              Advance::BeginCountIn,
          "Macro Record starts the authoring countdown");
    Check(Mission::Engine::Recorder::DecideAdvance(Recorder::CountIn) ==
              Advance::CancelCountIn,
          "Macro Record cancels an unstarted countdown safely");
    Check(Mission::Engine::Recorder::DecideAdvance(Recorder::Recording) ==
              Advance::StopToReview,
          "Macro Record stops active capture into Review");
    Check(Mission::Engine::Recorder::DecideAdvance(Recorder::Review) ==
              Advance::KeepReview,
          "Macro Record cannot erase a Review take");

    Check(IsMoveInstanceEdge(200, 0, 0, 0),
          "changing onto a move starts an action instance");
    Check(IsMoveInstanceEdge(200, 0, 200, 7),
          "same-ID frame rewind preserves a chained repeated normal");
    Check(!IsMoveInstanceEdge(200, 8, 200, 7),
          "ordinary animation advance is not a new action instance");

    Check(DecideComboEnd(false, false) == ComboEndDecision::None,
          "no recovery edge must not change the run");
    Check(DecideComboEnd(true, true) == ComboEndDecision::ConsumeRequired,
          "authored recovery edge must be consumed");
    Check(DecideComboEnd(true, false) == ComboEndDecision::UnexpectedDrop,
          "legacy one-piece combo must still drop on recovery");

    Check(DetectSampledComboBoundary(true, false, 3, 0) ==
              SampledComboBoundary::VisibleRecovery,
          "a visible N->0 recovery is a sampled boundary");
    Check(DetectSampledComboBoundary(true, true, 3, 1) ==
              SampledComboBoundary::CounterRollover,
          "a frame-perfect N->1 wake-up hit is retained as a sampled boundary");
    Check(DetectSampledComboBoundary(true, true, 2, 3) ==
              SampledComboBoundary::None,
          "ordinary increasing combo counts are not boundaries");

    Check(DecideStepSatisfaction(false, true, false) ==
              StepSatisfactionDecision::Wait,
          "a pre-started meaty may stay armed while recovery is pending");
    Check(DecideStepSatisfaction(true, true, true) ==
              StepSatisfactionDecision::Advance,
          "move-only okizeme setup may advance before recovery");
    Check(DecideStepSatisfaction(true, true, false) ==
              StepSatisfactionDecision::PrematureContactDrop,
          "contact before the required recovery proves a continuous combo");
    Check(DecideStepSatisfaction(true, false, false) ==
              StepSatisfactionDecision::Advance,
          "the pre-started meaty may land after recovery is consumed");
    Check(RecordedActionBaselineAfterBoundary(3, 3, 2, false) == 0,
          "a pre-started unlanded Part-2 action rebases from N to zero");
    Check(RecordedActionBaselineAfterBoundary(3, 2, 2, true) == 3,
          "the landed Part-1 boundary action keeps its baseline");
    Check(RunnerArmBaseline(1, true) == 0,
          "same-sample wake-up action+hit arms against the new segment baseline");
    Check(RunnerArmBaseline(3, false) == 3,
          "ordinary action arms against the current combo count");

    DelayedActionWindow gap;
    Check(!gap.Tick(false, false, false, 3), "gap tick 1 remains open");
    Check(!gap.Tick(false, false, false, 3), "gap tick 2 remains open");
    Check(!gap.Tick(false, false, true, 3),
          "attack input on the last legal tick latches commit grace");
    Check(!gap.Tick(false, true, false, 3) && gap.elapsed == 0,
          "matching action on the next sample wins and resets the window");
    Check(!gap.Tick(false, false, false, 1), "fresh one-tick window stays open");
    Check(gap.Tick(false, false, false, 1),
          "an expired gap with no committed input drops");

    Check(RecordedGapAllowance(0) == 0, "zero gap stays default");
    Check(RecordedGapAllowance(200) == 460,
          "supplied Misuzu recording preserves its 200->460 allowance");
    Check(RecordedGapAllowance(300) == 660,
          "delays longer than the former 576 cap remain recognizable");
    Check(RecordedGapAllowance(4000) == 5760, "malformed delay is safety-capped");

    SegmentScore score;
    score.Observe(1, 300);
    score.Observe(3, 1200);
    score.FinalizeSegment();
    score.Observe(0, 1200); // stale post-recovery damage must not leak into Part 2
    score.Observe(2, 800);
    Check(score.TotalHits() == 5, "hits accumulate across explicit combo parts");
    Check(score.TotalDamage() == 2000, "damage accumulates across explicit combo parts");
    score.Reset();
    Check(score.TotalHits() == 0 && score.TotalDamage() == 0,
          "retry/reset clears every segment accumulator");

    std::cout << "mission_sequence_policy_tests passed\n";
    return 0;
}
