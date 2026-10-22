#pragma once

#include <cstddef>
#include <cstdint>

namespace Mission::RecorderEntityTrace {

// A baseline observation was already alive at GO; it is deliberately not a spawn.
enum class SampledSlotTransition : uint8_t {
    None = 0,
    Baseline,
    Spawn,
    Morph,
    Despawn,
};

// Recorder and strict-v3 playback share one bounded lifecycle budget: a take
// that fits the authoring trace must stay replayable.
constexpr std::size_t kTraceEventCapacity = 4096;

// If the allocation cursor advanced but no dead->live edge survived to the
// endpoint sample, an entity was allocated and retired between observations;
// slot/generation identity is unknowable, so exact authoring fails closed.
constexpr bool AllocationAdvancedWithoutObservedSpawn(
    bool priorCursorEstablished,
    uint16_t priorAllocationCursor,
    uint16_t currentAllocationCursor,
    std::size_t observedSpawnCount) {
    return priorCursorEstablished &&
           priorAllocationCursor != currentAllocationCursor &&
           observedSpawnCount == 0;
}

// A contact grades only the nearest producer selected by
// FindRecordedEntityProducer; sharing slot+generation alone is not enough.
constexpr bool ContactGradesLifecycle(std::size_t lifecycleTraceIndex,
                                      std::size_t linkedProducerTraceIndex) {
    return lifecycleTraceIndex == linkedProducerTraceIndex;
}

// Suppression of post-contact recovery phases is evidence-gated: the catalog
// must identify the phase as recovery, the preceding same-instance event must
// be a same-family committed contact, and the trace must show it return inert
// or despawn; anything else stays review-required.
constexpr bool CanSuppressCompletedPostContactRecovery(
    bool catalogSaysPostContactRecovery,
    bool immediatelyPrecededBySameFamilyCommittedContact,
    bool laterReturnedInertOrDespawned) {
    return catalogSaysPostContactRecovery &&
           immediatelyPrecededBySameFamilyCommittedContact &&
           laterReturnedInertOrDespawned;
}

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
static_assert(AllocationAdvancedWithoutObservedSpawn(true, 4, 5, 0),
              "an invisible allocation must invalidate exact lineage");
static_assert(!AllocationAdvancedWithoutObservedSpawn(true, 4, 5, 1),
              "a visible spawn explains ordinary cursor movement");
static_assert(ContactGradesLifecycle(7, 7) &&
                  !ContactGradesLifecycle(6, 7),
              "a later producer phase cannot grade an earlier lifecycle");
static_assert(CanSuppressCompletedPostContactRecovery(true, true, true) &&
                  !CanSuppressCompletedPostContactRecovery(true, true, false) &&
                  !CanSuppressCompletedPostContactRecovery(false, true, true),
              "post-contact suppression must retain all three proofs");

} // namespace Mission::RecorderEntityTrace
