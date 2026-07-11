#pragma once

#include <cstdint>

// Pure transition policy for the asynchronous Practice reload transaction.
// Keeping this independent from game memory makes the critical old-Match vs
// new-Match distinction regression-testable.
namespace CharacterHotswap::Transition {

enum class WaitState : uint8_t { AwaitingLoading, AwaitingMatch };
enum class ObservedPhase : uint8_t { Other, CharacterSelect, Loading, Match };
enum class Decision : uint8_t { Wait, DirectFallback, LoadingObserved, Complete };

constexpr Decision Decide(WaitState state,
                          ObservedPhase phase,
                          bool directBootstrapPending,
                          bool loadingHandoffComplete) {
    if (state == WaitState::AwaitingLoading) {
        if (directBootstrapPending && phase == ObservedPhase::CharacterSelect) {
            return Decision::DirectFallback;
        }
        if (phase == ObservedPhase::Loading) {
            return Decision::LoadingObserved;
        }
        if (phase == ObservedPhase::Match && loadingHandoffComplete) {
            // The monitor may miss a very short Loading phase. The game-thread
            // Loading hook receipt still proves this is the destination Match.
            return Decision::Complete;
        }
        // In particular, Match here is the origin session still finishing its
        // native cleanup. It can never complete this transaction.
        return Decision::Wait;
    }
    return phase == ObservedPhase::Match ? Decision::Complete : Decision::Wait;
}

// A completed-load receipt describes one concrete destination session.  The
// tuple alone is insufficient: the same characters/stage can appear again
// after leaving that Match.  Bind acceptance to the lifecycle generation that
// was current when the reload completed.
constexpr bool ReceiptGenerationMatches(uint32_t receiptGeneration,
                                        uint32_t currentGeneration) {
    return receiptGeneration != 0 && receiptGeneration == currentGeneration;
}

} // namespace CharacterHotswap::Transition
