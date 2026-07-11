#include "game/character_hotswap_transition.h"

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
    using namespace CharacterHotswap::Transition;

    Check(Decide(WaitState::AwaitingLoading, ObservedPhase::Match, true, false) == Decision::Wait,
          "old Match cannot complete a match-origin direct reload");
    Check(Decide(WaitState::AwaitingLoading, ObservedPhase::Loading, false, true) ==
              Decision::LoadingObserved,
          "observed Loading advances the transaction");
    Check(Decide(WaitState::AwaitingLoading, ObservedPhase::Match, false, true) ==
              Decision::Complete,
          "Loading-hook receipt permits a skipped monitor phase");
    Check(Decide(WaitState::AwaitingMatch, ObservedPhase::Loading, false, true) == Decision::Wait,
          "Loading cannot complete before the destination Match");
    Check(Decide(WaitState::AwaitingMatch, ObservedPhase::Match, false, true) == Decision::Complete,
          "new Match completes only after Loading was observed");
    Check(Decide(WaitState::AwaitingLoading, ObservedPhase::CharacterSelect, true, false) ==
              Decision::DirectFallback,
          "direct bootstrap failure retains selector fallback");
    Check(ReceiptGenerationMatches(7, 7),
          "prepared-session receipt is accepted in its destination generation");
    Check(!ReceiptGenerationMatches(7, 8),
          "prepared-session receipt cannot authorize a later session");
    Check(!ReceiptGenerationMatches(0, 0),
          "generation zero is never a valid receipt");

    std::cout << "character_hotswap_transition_tests passed\n";
    return 0;
}
