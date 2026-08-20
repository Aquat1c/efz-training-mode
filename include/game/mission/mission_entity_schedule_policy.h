#pragma once

#include <cstdint>

namespace Mission::EntitySchedulePolicy {

enum class LifecycleKind : uint8_t {
    None = 0,
    Baseline,
    Spawn,
    Morph,
    Despawn,
};

struct Progress {
    int contacts = 0;
    int comboHits = 0;
};

constexpr bool RequiresV2Ordering(int contactAfterAction,
                                  int afterStepContact,
                                  int dueBeforeStepContact) {
    return contactAfterAction >= 0 || afterStepContact >= 0 ||
           dueBeforeStepContact >= 0;
}

constexpr bool RuntimeMarkerSupportsOrdering(int markerVersion,
                                             bool requiresV2Ordering) {
    return markerVersion == 2 || markerVersion == 3 || markerVersion == 4 ||
           markerVersion == 5 ||
           (markerVersion == 1 && !requiresV2Ordering);
}

constexpr bool RuntimeMarkerSupportsLineage(int markerVersion,
                                            bool hasLineageFields) {
    return markerVersion == 3 || markerVersion == 4 || markerVersion == 5 ||
           ((markerVersion == 1 || markerVersion == 2) &&
            !hasLineageFields);
}

constexpr bool IsContactProducerKind(LifecycleKind kind) {
    return kind == LifecycleKind::Baseline || kind == LifecycleKind::Spawn ||
           kind == LifecycleKind::Morph;
}

constexpr bool ValidProducerDescriptor(LifecycleKind kind,
                                       int producerPattern,
                                       int producerPriorPattern) {
    if (!IsContactProducerKind(kind) || producerPattern <= 0) return false;
    if (kind == LifecycleKind::Morph) {
        return producerPriorPattern > 0 &&
               producerPriorPattern != producerPattern;
    }
    return producerPriorPattern < 0;
}

// v3 contact identity. Pattern alone is insufficient when two notes, bullets,
// or summon bodies of the same family coexist. The exact restored ring makes
// the recorded slot/generation deterministic for a take; the linked lifecycle
// descriptor proves that the expected producer, rather than a concurrent
// lookalike, owns the resolver contact.
constexpr bool StrictLineageMatches(
    int requiredSlot, uint32_t requiredGeneration,
    LifecycleKind requiredKind, int requiredPattern,
    int requiredPriorPattern, int requiredProducerAction,
    int contactSlot, int contactPattern,
    int observedSlot, uint32_t observedGeneration,
    LifecycleKind observedKind, int observedPattern,
    int observedPriorPattern, int observedProducerAction) {
    return requiredSlot >= 0 && requiredGeneration > 0 &&
           contactSlot == requiredSlot && contactPattern == requiredPattern &&
           observedSlot == requiredSlot &&
           observedGeneration == requiredGeneration &&
           observedKind == requiredKind &&
           observedPattern == requiredPattern &&
           observedPriorPattern == requiredPriorPattern &&
           observedProducerAction == requiredProducerAction;
}

constexpr bool Satisfied(const Progress& progress,
                         int requiredContacts,
                         int requiredComboHits) {
    return progress.contacts >= requiredContacts &&
           progress.comboHits >= requiredComboHits;
}

constexpr bool Overshot(const Progress& progress,
                        int requiredContacts,
                        int requiredComboHits) {
    return progress.contacts > requiredContacts ||
           progress.comboHits > requiredComboHits;
}

// A fanout episode records every sibling that happened to connect in the
// authoring take, but only its explicit minimum is compulsory at playback.
// Exact member identity and the cast/due window are its bounds; the aggregate
// observed in one take is diagnostic only because a sibling which whiffed in
// that take may validly connect during playback.
constexpr bool FanoutSatisfied(const Progress& progress,
                               int minimumContacts,
                               int minimumComboHits) {
    return minimumContacts > 0 && minimumComboHits >= 0 &&
           Satisfied(progress, minimumContacts, minimumComboHits);
}

constexpr bool FanoutAggregateAllowed(const Progress& progress,
                                      int /*contactsObservedInTake*/,
                                      int /*comboHitsObservedInTake*/) {
    return progress.contacts >= 0 && progress.comboHits >= 0;
}

constexpr bool FanoutMemberContactMatches(bool resultMatches,
                                          bool exactMemberLineageMatches) {
    return resultMatches && exactMemberLineageMatches;
}

constexpr bool OptionalFanoutContactCanBeConsumed(
    bool gatesOpen,
    bool dueWindowOpen,
    bool resultMatches,
    bool exactMemberLineageMatches) {
    return gatesOpen && dueWindowOpen &&
           FanoutMemberContactMatches(resultMatches,
                                      exactMemberLineageMatches);
}

constexpr int OrderedHitContribution(bool hitResult,
                                     int comboBefore,
                                     int comboAfter) {
    if (!hitResult) return 0;
    const int delta = comboAfter - comboBefore;
    return delta > 0 ? delta : 1;
}

constexpr bool GatesOpen(int startedThroughAction,
                         int satisfiedThroughDirectBarrier,
                         int currentSegment,
                         int opensAfterAction,
                         int afterDirectStep,
                         int requiredSegment) {
    return startedThroughAction >= opensAfterAction &&
           satisfiedThroughDirectBarrier >= afterDirectStep &&
           currentSegment == requiredSegment;
}

// Generated schedules may bind an entity run between resolver contacts of a
// single direct action. A positive ordinal is one-based and opens immediately
// after that committed contact. -1 is the legacy contract: wait for the whole
// direct step to satisfy.
constexpr bool AfterDirectBarrierSatisfied(int satisfiedThroughStep,
                                            int activeStep,
                                            int activeStepContacts,
                                            int afterStep,
                                            int afterStepContact) {
    if (afterStep < 0) return true;
    if (afterStepContact < 1) return satisfiedThroughStep >= afterStep;
    return satisfiedThroughStep > afterStep ||
           (activeStep == afterStep &&
            activeStepContacts >= afterStepContact);
}

constexpr bool OrderedGatesOpen(int startedThroughAction,
                                int satisfiedThroughDirectBarrier,
                                int activeDirectStep,
                                int activeDirectContacts,
                                int currentSegment,
                                int opensAfterAction,
                                int contactAfterAction,
                                int afterDirectStep,
                                int afterDirectContact,
                                int requiredSegment) {
    return startedThroughAction >= opensAfterAction &&
           startedThroughAction >= contactAfterAction &&
           AfterDirectBarrierSatisfied(
               satisfiedThroughDirectBarrier, activeDirectStep,
               activeDirectContacts, afterDirectStep, afterDirectContact) &&
           currentSegment == requiredSegment;
}

constexpr bool DirectBarrierSatisfied(bool requiresConnect,
                                      int requiredHits,
                                      bool typedResultMatches,
                                      bool connectSeen,
                                      int hitsSeen) {
    if (!typedResultMatches) return false;
    return requiresConnect ? connectSeen : hitsSeen >= requiredHits;
}

constexpr bool EntityPrecedesDirect(uint32_t lastEntitySequence,
                                    uint32_t firstDirectSequence) {
    return firstDirectSequence == 0 ||
           lastEntitySequence < firstDirectSequence;
}

// Entity damage is resolver-local just like direct-step damage.  A small
// tolerance keeps ordinary engine rounding from invalidating a take while a
// materially different projectile variant/proration still fails at the exact
// obligation that diverged.  Zero means the authoring trace had no damage
// contract (blocks/RG/guard points) and therefore never grades damage.
constexpr int kEntityDamageTolerance = 500;
constexpr bool DamageDiverged(int recordedDamage, int observedDamage) {
    if (recordedDamage <= 0) return false;
    const int difference = observedDamage - recordedDamage;
    return difference > kEntityDamageTolerance ||
           difference < -kEntityDamageTolerance;
}

// Compile-time segment ownership is the sum of already-visible step-owned
// boundaries and earlier entity-owned boundaries.  The monotonic floor keeps
// coarse sampled action anchors from moving a later resolver event backwards.
constexpr int CompiledSegment(int stepOwnedSegment,
                              int entityBoundariesSeen,
                              int monotonicFloor) {
    const int owned = stepOwnedSegment + entityBoundariesSeen;
    return owned > monotonicFloor ? owned : monotonicFloor;
}

// During compilation slot/generation are exact authoring lineage. v1/v2 keep
// them diagnostic; v3 additionally replays this identity against the exact
// restored ring. Never collapse two recorded producers or two contact-time
// action gates into one count obligation: a later setter must receive its own
// due barrier and cannot repair a missed earlier run.
constexpr bool SameRecordedProducerLineage(int previousSlot,
                                           uint32_t previousGeneration,
                                           int previousProducerGate,
                                           int eventSlot,
                                           uint32_t eventGeneration,
                                           int eventProducerGate) {
    return previousSlot == eventSlot &&
           previousGeneration == eventGeneration &&
           previousProducerGate == eventProducerGate;
}

constexpr bool CanJoinContactRun(bool previousEndsCombo,
                                 int previousSegment,
                                 int eventSegment,
                                 int previousSlot,
                                 uint32_t previousGeneration,
                                 int eventSlot,
                                 uint32_t eventGeneration,
                                 int previousActionGate,
                                 int eventActionGate,
                                 int previousAfterStep,
                                 int previousAfterContact,
                                 int eventAfterStep,
                                 int eventAfterContact) {
    return !previousEndsCombo && previousSegment == eventSegment &&
           previousSlot == eventSlot &&
           previousGeneration == eventGeneration &&
           previousActionGate == eventActionGate &&
           previousAfterStep == eventAfterStep &&
           previousAfterContact == eventAfterContact;
}

// Recorder ordering is a pair: the exact closed Battle update which exposed
// the action and its monotonically assigned action order. observedAfterAction
// is the last action known to precede the resolver transaction. Unknown batch
// IDs fall back to that exact recorder order for old/synthetic producers.
constexpr bool RecordedActionFollowsContact(uint32_t actionBatch,
                                            uint32_t actionOrder,
                                            uint32_t contactBatch,
                                            uint32_t observedAfterAction) {
    if (actionOrder <= observedAfterAction) return false;
    if (actionBatch == 0 || contactBatch == 0) return true;
    const int32_t batchDelta =
        static_cast<int32_t>(actionBatch - contactBatch);
    // Equal Battle batches have no portable intra-batch ordering. Compiling
    // equality as an action-start due barrier is impossible at runtime because
    // action starts are checked before that batch's contact journal is consumed.
    // A following direct-contact sequence remains the safe ordering fallback.
    return batchDelta > 0;
}

constexpr bool RecordedActionCanPrecedeContact(uint32_t actionBatch,
                                               uint32_t contactBatch) {
    if (actionBatch == 0 || contactBatch == 0) return true;
    return static_cast<int32_t>(actionBatch - contactBatch) <= 0;
}

struct DueBarrier {
    int step = -1;
    int contact = -1; // -1 legacy completion, 0 action start, >0 contact ordinal
};

// The earlier ordered barrier wins. An action start precedes every direct
// contact owned by that same action; a direct contact from an already-running
// earlier action remains the stronger barrier.
constexpr DueBarrier SelectDueBarrier(int followingActionStep,
                                      int followingDirectStep,
                                      int followingDirectContact) {
    return followingActionStep >= 0 &&
           (followingDirectStep < 0 ||
            followingActionStep <= followingDirectStep)
        ? DueBarrier{followingActionStep, 0}
        : followingDirectStep >= 0
            ? DueBarrier{followingDirectStep, followingDirectContact}
            : DueBarrier{};
}

constexpr bool DueDirectContactReached(int step,
                                       int contactOrdinal,
                                       int dueStep,
                                       int dueContact) {
    return dueStep == step && dueContact > 0 &&
           contactOrdinal >= dueContact;
}

// Once a flexible episode reaches its minimum, later exact siblings from the
// same cast remain consumable until its authored due barrier. This prevents a
// valid second/third bullet from being misreported as an extra projectile just
// because the first bullet already satisfied the minimum.
constexpr bool OptionalFanoutWindowOpen(int startedThroughAction,
                                        int satisfiedThroughDirectStep,
                                        int activeDirectStep,
                                        int activeDirectContacts,
                                        int dueStep,
                                        int dueContact) {
    if (dueStep < 0) return true;
    if (dueContact == 0) return startedThroughAction < dueStep;
    if (dueContact > 0) {
        return activeDirectStep < dueStep ||
               (activeDirectStep == dueStep &&
                activeDirectContacts < dueContact);
    }
    return satisfiedThroughDirectStep < dueStep;
}

// A closed resolver batch may straddle two authored direct actions. Events
// before the preserved hand-off sequence belong to the armed step; the hand-off
// event itself and every later transaction belong to the authored successor.
// A zero sequence means no ordered transition was proven for this batch.
constexpr int DirectStepForOrderedEvent(int initialStep,
                                        int nextStep,
                                        uint32_t nextStepSequence,
                                        uint32_t eventSequence) {
    return nextStep >= 0 && nextStepSequence != 0 &&
            eventSequence >= nextStepSequence
        ? nextStep
        : initialStep;
}

// Within one step the ordered due points are action start (0), contact 1..N,
// then legacy whole-step completion (-1). This is used only for validation;
// runtime keeps the omitted legacy default unchanged.
constexpr int DueContactSortRank(int contact) {
    return contact < 0 ? 0x3fffffff : contact;
}

constexpr bool DueBarrierDoesNotRegress(int previousStep,
                                        int previousContact,
                                        int currentStep,
                                        int currentContact) {
    return currentStep > previousStep ||
           (currentStep == previousStep &&
            DueContactSortRank(currentContact) >=
                DueContactSortRank(previousContact));
}

constexpr bool BarrierWindowValid(int afterStep,
                                  int afterContact,
                                  int dueStep,
                                  int dueContact) {
    if (dueStep < 0 || afterStep < 0) return true;
    if (dueStep > afterStep) return true;
    if (dueStep < afterStep) return false;
    if (afterContact < 1) return false;
    return dueContact < 0 || dueContact > afterContact;
}

// Land/Connect author exactly one direct contact. Hits authors the explicit
// hit count. A positive schedule ordinal beyond that contract can never be
// reached (or, worse, can escape enforcement when its step advances).
constexpr int AuthoredDirectContactCount(bool hitsRequirement,
                                         int hitsRequired) {
    return hitsRequirement && hitsRequired > 0 ? hitsRequired : 1;
}

constexpr bool ContactOrdinalWithinAuthoredRequirement(
    int ordinal, int authoredContactCount) {
    // -1 is the v1 whole-step sentinel. Zero denotes action start and is not a
    // contact ordinal; callers validate it separately where it is meaningful.
    return ordinal < 0 ||
           (ordinal > 0 && ordinal <= authoredContactCount);
}

// A required combo boundary may be visible only in the sampled state while
// resolver records from both sides are already present. Advance locally at the
// first ordered committed contact whose combo baseline belongs to the new part.
constexpr bool OrderedContactBeginsNewCombo(int comboBefore,
                                            int comboAfter) {
    return comboBefore == 0 ||
           (comboBefore > 0 && comboAfter > 0 && comboAfter < comboBefore);
}

// The sampled combo counter can miss a complete recovery/restart between two
// monitor passes (for example, both samples read 1).  A closed resolver batch
// retains the exact before/after counter for each committed contact, so an
// already-authored boundary can still be consumed without guessing from the
// sampled state.
constexpr bool ExactOrderedContactConsumesAwaitedBoundary(
    bool awaitingComboEnd,
    bool committedContact,
    int comboBefore,
    int comboAfter) {
    return awaitingComboEnd && committedContact &&
           OrderedContactBeginsNewCombo(comboBefore, comboAfter);
}

constexpr int SegmentForOrderedContact(int currentSegment,
                                       bool boundaryAdvancePending,
                                       bool committedContact,
                                       int comboBefore,
                                       int comboAfter) {
    // A block or RG is still the first committed contact of the authored next
    // part. Requiring damage here strands its entity requirements in Part 1.
    return boundaryAdvancePending && committedContact &&
            OrderedContactBeginsNewCombo(comboBefore, comboAfter)
        ? currentSegment + 1
        : currentSegment;
}

// v3 schedules bind their producer generations to the restored world.  A
// value-only progress reset necessarily destroys that identity baseline.  v1
// and v2 keep their historical logical-reset behavior.
constexpr bool RequiresRestoreBoundReset(int markerVersion,
                                         bool hasEntitySchedule) {
    return markerVersion >= 3 && hasEntitySchedule;
}

} // namespace Mission::EntitySchedulePolicy
