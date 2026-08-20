#pragma once

#include "game/mission/mission_data.h"
#include "game/mission/mission_entity_schedule_policy.h"

#include <cstdint>
#include <string>

namespace Mission::EntityLifecyclePolicy {

using LifecycleKind = EntitySchedulePolicy::LifecycleKind;

// Runtime-facing view of one sampled entity producer transition.  Unlike a
// contact, a lifecycle objective is proved by this transition itself.  Slot
// and generation keep two concurrent (or successively reused) ring entries
// with the same numeric pattern from satisfying one another's objective.
struct ProducerObservation {
    int slot = -1;
    uint32_t generation = 0;
    LifecycleKind kind = LifecycleKind::None;
    int pattern = -1;
    int priorPattern = -1;
    int actionGate = -1;
};

struct ObjectiveState {
    bool satisfied = false;
};

// Ring lineage proves that an attack-capable phase existed.  Calling that
// phase a whiff additionally requires uninterrupted resolver evidence: if the
// entity-contact hook or journal was unavailable, an absent contact record is
// not proof that no contact occurred.
constexpr bool ExactWhiffEvidenceAvailable(
    bool hasExactSavestate,
    bool entityContactHookObserved,
    bool entityContactHookComplete,
    bool contactEpochContinuous,
    bool contactJournalComplete,
    bool entityTraceComplete,
    bool entityProbeComplete,
    bool entityAllocationUnambiguous) {
    return hasExactSavestate && entityContactHookObserved &&
           entityContactHookComplete && contactEpochContinuous &&
           contactJournalComplete && entityTraceComplete &&
           entityProbeComplete && entityAllocationUnambiguous;
}

inline LifecycleKind KindFromName(const std::string& value) {
    if (value == "baseline") return LifecycleKind::Baseline;
    if (value == "spawn") return LifecycleKind::Spawn;
    if (value == "morph") return LifecycleKind::Morph;
    if (value == "despawn") return LifecycleKind::Despawn;
    return LifecycleKind::None;
}

// A lifecycle objective is deliberately stricter than a pattern lookup.  Its
// complete authoring identity must be usable before any observation may match.
constexpr bool ValidIdentity(int slot, int generation, LifecycleKind kind,
                             int pattern, int priorPattern,
                             int actionGate) {
    if (slot < 0 || generation <= 0 || kind == LifecycleKind::None ||
        actionGate < -1) {
        return false;
    }
    switch (kind) {
    case LifecycleKind::Baseline:
        return actionGate == -1 && pattern > 0 && priorPattern < 0;
    case LifecycleKind::Spawn:
        return pattern > 0 && priorPattern < 0;
    case LifecycleKind::Morph:
        return pattern > 0 && priorPattern > 0 &&
               priorPattern != pattern;
    case LifecycleKind::Despawn:
        return pattern < 0 && priorPattern > 0;
    case LifecycleKind::None:
        break;
    }
    return false;
}

constexpr bool ExactIdentityMatches(
    int requiredSlot, int requiredGeneration, LifecycleKind requiredKind,
    int requiredPattern, int requiredPriorPattern, int requiredActionGate,
    const ProducerObservation& observed) {
    return ValidIdentity(requiredSlot, requiredGeneration, requiredKind,
                         requiredPattern, requiredPriorPattern,
                         requiredActionGate) &&
           observed.slot == requiredSlot &&
           observed.generation ==
               static_cast<uint32_t>(requiredGeneration) &&
           observed.kind == requiredKind &&
           observed.pattern == requiredPattern &&
           observed.priorPattern == requiredPriorPattern &&
           observed.actionGate == requiredActionGate;
}

inline bool ExactIdentityMatches(
    const EntityLifecycleRequirement& requirement,
    const ProducerObservation& observed) {
    return ExactIdentityMatches(
        requirement.slot, requirement.generation,
        KindFromName(requirement.lifecycle), requirement.pattern,
        requirement.priorPattern, requirement.opensAfterAction, observed);
}

// Ring sampling and combo-boundary consumption happen in the same monitor
// snapshot. When the recorder assigned an exact producer to the new combo
// segment, retain that authored segment even if the runner has not consumed
// the visible boundary until later in the tick. A non-matching transition
// always keeps the actually sampled segment.
inline int SegmentForExactObjective(
    int sampledSegment,
    const EntityLifecycleRequirement& requirement,
    const ProducerObservation& observed) {
    return ExactIdentityMatches(requirement, observed)
        ? requirement.segment : sampledSegment;
}

// Two rows with the same strict producer identity would both be satisfied by
// one sampled transition in one combo part. Keep presentation and deadline
// fields out of the identity: changing a label or allowance cannot turn one
// producer into a second objective. Segment remains part of the identity
// because a persistent summon may repeat the same morph in a later combo part.
inline bool SameObjectiveIdentity(
    const EntityLifecycleRequirement& left,
    const EntityLifecycleRequirement& right) {
    return left.owner == right.owner && left.slot == right.slot &&
           left.generation == right.generation &&
           left.lifecycle == right.lifecycle &&
           left.pattern == right.pattern &&
           left.priorPattern == right.priorPattern &&
           left.opensAfterAction == right.opensAfterAction &&
           left.segment == right.segment;
}

inline bool HasPriorDuplicateObjective(
    const std::vector<EntityLifecycleRequirement>& requirements,
    std::size_t index) {
    if (index >= requirements.size()) return false;
    for (std::size_t prior = 0; prior < index; ++prior) {
        if (SameObjectiveIdentity(requirements[prior], requirements[index])) {
            return true;
        }
    }
    return false;
}

// A persistent summon can repeat the same exact morph in later combo parts.
// Prefer the still-pending objective in the sampled segment. If that one was
// already satisfied (or does not exist), a transition sampled on the tick
// before the visible boundary is consumed may belong to the immediately next
// authored segment. Never bank one observation across two or more boundaries.
inline int SegmentForPendingExactObjective(
    int sampledSegment,
    const std::vector<EntityLifecycleRequirement>& requirements,
    const std::vector<ObjectiveState>& states,
    const ProducerObservation& observed) {
    int nextSegment = sampledSegment;
    for (std::size_t i = 0; i < requirements.size(); ++i) {
        if (i < states.size() && states[i].satisfied) continue;
        const EntityLifecycleRequirement& requirement = requirements[i];
        if (!ExactIdentityMatches(requirement, observed)) continue;
        if (requirement.segment == sampledSegment) return sampledSegment;
        if (requirement.segment == sampledSegment + 1) {
            nextSegment = requirement.segment;
        }
    }
    return nextSegment;
}

// Returns true only for the transition which newly satisfies this objective.
// The latest producer remains visible in the runtime lineage across monitor
// ticks, so returning false after satisfaction prevents that persistent sample
// from being consumed repeatedly.
constexpr bool TrySatisfyOnce(
    ObjectiveState& state,
    int requiredSlot, int requiredGeneration, LifecycleKind requiredKind,
    int requiredPattern, int requiredPriorPattern, int requiredActionGate,
    const ProducerObservation& observed) {
    if (state.satisfied ||
        !ExactIdentityMatches(
            requiredSlot, requiredGeneration, requiredKind,
            requiredPattern, requiredPriorPattern, requiredActionGate,
            observed)) {
        return false;
    }
    state.satisfied = true;
    return true;
}

inline bool TrySatisfyOnce(
    ObjectiveState& state,
    const EntityLifecycleRequirement& requirement,
    const ProducerObservation& observed) {
    if (state.satisfied || !ExactIdentityMatches(requirement, observed)) {
        return false;
    }
    state.satisfied = true;
    return true;
}

} // namespace Mission::EntityLifecyclePolicy
