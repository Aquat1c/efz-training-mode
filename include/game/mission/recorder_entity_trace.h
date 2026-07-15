#pragma once

#include <cstdint>

namespace Mission::RecorderEntityTrace {

// Pure policy used by the authoring-only ring sampler. A baseline observation
// describes what was already alive at GO; it is deliberately not a spawn.
enum class SampledSlotTransition : uint8_t {
    None = 0,
    Baseline,
    Spawn,
    Morph,
    Despawn,
};

constexpr SampledSlotTransition ClassifySampledSlotTransition(
    bool priorEstablished, bool priorAlive, uint16_t priorPattern,
    bool currentAlive, uint16_t currentPattern) {
    if (!priorEstablished) {
        return currentAlive ? SampledSlotTransition::Baseline
                            : SampledSlotTransition::None;
    }
    if (!priorAlive && currentAlive) return SampledSlotTransition::Spawn;
    if (priorAlive && !currentAlive) return SampledSlotTransition::Despawn;
    if (priorAlive && currentAlive && priorPattern != currentPattern) {
        return SampledSlotTransition::Morph;
    }
    return SampledSlotTransition::None;
}

static_assert(ClassifySampledSlotTransition(false, false, 0, true, 400) ==
                  SampledSlotTransition::Baseline,
              "an entity already alive at GO is baseline, not spawn");
static_assert(ClassifySampledSlotTransition(true, false, 0, true, 400) ==
                  SampledSlotTransition::Spawn,
              "dead-to-alive slot transition is a sampled spawn");
static_assert(ClassifySampledSlotTransition(true, true, 400, true, 405) ==
                  SampledSlotTransition::Morph,
              "same live slot with a new pattern is a sampled morph");
static_assert(ClassifySampledSlotTransition(true, true, 405, false, 0) ==
                  SampledSlotTransition::Despawn,
              "alive-to-dead slot transition is a sampled despawn");

} // namespace Mission::RecorderEntityTrace
