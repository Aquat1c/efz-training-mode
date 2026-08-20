#pragma once

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>

// Presentation-only policy for the strict entity-contact side lane.
//
// The mission runner deliberately keeps exact pattern/producer lineage (either
// as an ordinary requirement or as the members of a curated fanout episode).
// This layer may hide redundant ordinary projectile rows and fold related
// phases into one readable token, but it must never alter what the runner
// grades.
namespace Mission::EntityPresentationPolicy {

struct Timing {
    int opensAfterAction = -1;
    int contactAfterAction = -1;
    int afterStep = -1;
    int afterStepContact = -1;
    int dueBeforeStep = -1;
    int dueBeforeStepContact = -1;
    bool comboEndAfter = false;
};

// A contact may attach silently to its producer token only when the recipe
// proves they are one user-facing action. Keep the policy independent
// of Mission/renderer types so the conservative boundary is unit-testable.
//
// This is deliberately narrower than "not structurally standalone": the
// entity must be an exact generated contact requirement owned by an eligible
// action, and its producer/contact/deadline gates must all describe that one
// action. A trap set earlier, a summon attacking during a later normal, an
// interleaved hit, or a contact that survives across another action therefore
// keeps its own visible token even when both tokens happen to use identical
// notation.
struct ExactProducerHit {
    int action = -1;
    // New recordings must persist one exact (action, move) pair.  Legacy
    // timing inference can keep old files readable, but it is not enough
    // evidence to remove a strict entity obligation from the recipe.
    bool exactSource = false;
    bool generated = false;
    bool resolvedContact = false;
    bool actionEligible = false;
    // A tagged entity-only command is exact without a player move ID. Its
    // restored slot/generation transition and causal S edge are validated by
    // the runner; the contact may therefore attach to that command cell.
    bool commandSource = false;
    // Ordinary projectiles are causally owned by their sampled lifecycle
    // action. Persistent setplay/traps/summons need the stronger semantic
    // producer proof below because a child allocated during a later normal can
    // still belong to an earlier setter (Nagamori's notes are the canonical
    // example).
    bool ordinaryProjectile = false;
    bool producerMoveMatches = false;
    // Presentation callers independently derive the source action's combo
    // segment from the authored recipe.  A stale source pair from an earlier
    // part must never absorb a later segment's entity obligation.
    bool sourceSegmentMatches = true;
    // Even when the numeric segment has not advanced yet, a combo-end marker
    // on the source step is a recovery boundary between the move and the
    // entity contact.  Keep that contact visible as the next setup part.
    bool sourceComboEndBeforeContact = false;
};

// An ordinary fireball whose entity contact belongs to the same authored
// action is already represented by that action cell.  A baseline entity,
// cross-action activation, interleaved resolver contact, long-lived effect, or
// combo-ending entity is independently meaningful and gets its own row.
constexpr bool IsStructurallyStandalone(const Timing& timing) {
    if (timing.comboEndAfter) return true;
    if (timing.opensAfterAction < 0) return true;
    if (timing.contactAfterAction > timing.opensAfterAction) return true;
    if (timing.afterStep > timing.opensAfterAction) return true;
    if (timing.afterStepContact > 0) return true;
    return timing.dueBeforeStep > timing.opensAfterAction + 1;
}

constexpr bool CanInlineExactProducerHit(const ExactProducerHit& candidate,
                                         const Timing& timing) {
    if (candidate.action < 0 || !candidate.exactSource ||
        !candidate.generated ||
        !candidate.resolvedContact || !candidate.actionEligible ||
        !candidate.sourceSegmentMatches ||
        candidate.sourceComboEndBeforeContact ||
        (!candidate.commandSource && !candidate.ordinaryProjectile &&
         !candidate.producerMoveMatches)) {
        return false;
    }
    if (timing.opensAfterAction != candidate.action ||
        timing.contactAfterAction != candidate.action) {
        return false;
    }
    // A direct hit owned by the same move is still part of that move's attack
    // episode. A barrier from any later action proves the entity activated as
    // an independently timed object.
    if (timing.afterStep > candidate.action ||
        (timing.afterStepContact > 0 &&
         timing.afterStep != candidate.action)) {
        return false;
    }
    // The deadline may be another contact of this same move, the next action,
    // or mission end. Anything later means the entity survived across an
    // intervening authored action and must remain visible on its own.
    if (timing.dueBeforeStep >= 0 &&
        timing.dueBeforeStep != candidate.action &&
        timing.dueBeforeStep != candidate.action + 1) {
        return false;
    }
    if (timing.dueBeforeStep == candidate.action &&
        timing.dueBeforeStepContact <= 0) {
        return false;
    }
    if (timing.dueBeforeStep == candidate.action + 1 &&
        timing.dueBeforeStepContact > 0) {
        return false;
    }
    Timing producerTiming = timing;
    // IsStructurallyStandalone is intentionally conservative for arbitrary
    // contact ordinals. At this point, however, the exact gates above have
    // proved that this ordinal is another contact of the same producer action
    // (for example a projectile between direct hits of one multihit move), so
    // it is not an intervening user action.
    if (producerTiming.afterStep == candidate.action) {
        producerTiming.afterStepContact = -1;
    }
    producerTiming.comboEndAfter = false;
    return !IsStructurallyStandalone(producerTiming);
}

// A persisted generated label is only evidence for the exact producer that
// created it.  Without this guard, notation generated for another overload of
// the same entity pattern could turn an authored/mismatched label back into a
// generated fallback and make the strict row eligible for folding.
constexpr bool PersistedSemanticMatchesSource(bool hasPersistedSemantic,
                                              bool exactSource,
                                              int persistedProducerMove,
                                              int sourceMove) {
    return hasPersistedSemantic &&
        (!exactSource || persistedProducerMove == sourceMove);
}

// Select the cell which may present an entity obligation.  Exact new
// provenance wins; only an actually legacy file may use the old lifecycle
// inference.  Explicitly unresolved new recordings therefore return -1 even
// for an ordinary InlineProjectile.
constexpr int SelectOwnerAction(bool exactSource, bool legacySource,
                                int semanticAction, int lifecycleAction,
                                int totalActions) {
    if (exactSource && semanticAction >= 0 &&
        semanticAction < totalActions) {
        return semanticAction;
    }
    if (legacySource && lifecycleAction >= 0 &&
        lifecycleAction < totalActions) {
        return lifecycleAction;
    }
    return -1;
}

// Unknown committed contacts remain visible as raw #pattern evidence.  Known
// traps/summons/setplay can opt into an unconditional row; ordinary mapped
// projectiles are shown only when their recorded timing makes them standalone.
constexpr bool ShouldDisplay(bool mapped, bool alwaysStandalone,
                             bool failed, const Timing& timing) {
    (void)failed;
    // Failure changes tint, never layout.  An immediate mapped projectile
    // remains attached to its cast cell even when that obligation fails.
    return !mapped || alwaysStandalone || IsStructurallyStandalone(timing);
}

// `comboEndAfter` is structural metadata, but it does not make an otherwise
// exact immediate projectile a second user action. Keep the general
// ShouldDisplay policy conservative for delayed, interleaved, or unresolved
// contacts and override it only after CanInlineExactProducerHit proved the
// exact generated producer relationship. The renderer still reads the hidden
// requirement's boundary to place the following [SETUP] divider.
constexpr bool ShouldDisplay(bool mapped, bool alwaysStandalone,
                             bool failed, const Timing& timing,
                             bool inlineExactProducerHit) {
    (void)mapped;
    (void)alwaysStandalone;
    (void)failed;
    (void)timing;
    // Timing is not causality. The exact producer proof above is the only
    // authority allowed to remove a raw obligation from the visible recipe.
    // Otherwise a mapped child allocated during another action could vanish
    // merely because its sampled barriers happen to look immediate.
    return !inlineExactProducerHit;
}

struct Identity {
    const char* family = nullptr;
    int owner = 0;
    int target = 0;
    int segment = 0;
    int slot = -1;
    int generation = 0;
    int producerAction = -1;
    int contactAfterAction = -1;
    int afterStep = -1;
    int afterStepContact = -1;
    int dueStep = -1;
    int dueContact = -1;
};

constexpr bool SameInstance(const Identity& left, const Identity& right) {
    return left.slot >= 0 && right.slot >= 0 &&
           left.generation > 0 && right.generation > 0 &&
           left.slot == right.slot && left.generation == right.generation;
}

constexpr bool SameProducerEpisode(const Identity& left,
                                   const Identity& right) {
    return left.producerAction >= 0 &&
           left.producerAction == right.producerAction;
}

// Different deadlines stay distinct because a direct player hit can occur
// between them.  Within one authored deadline, same-slot morph phases or
// separately allocated children from the same cast can share one visible
// token without weakening the exact grading schedule.
inline bool CanFold(const Identity& left, const Identity& right,
                    const char* leftResult, const char* rightResult) {
    if (!left.family || !right.family ||
        std::strcmp(left.family, right.family) != 0) {
        return false;
    }
    if (!leftResult || !rightResult ||
        std::strcmp(leftResult, rightResult) != 0) {
        return false;
    }
    return left.owner == right.owner && left.target == right.target &&
           left.segment == right.segment &&
           left.contactAfterAction == right.contactAfterAction &&
           left.afterStep == right.afterStep &&
           left.afterStepContact == right.afterStepContact &&
           left.dueStep == right.dueStep &&
           left.dueContact == right.dueContact &&
           (SameInstance(left, right) || SameProducerEpisode(left, right));
}

constexpr bool AllSatisfied(int satisfiedMembers, int memberCount) {
    return memberCount > 0 && satisfiedMembers == memberCount;
}

// A fanout cast records the complete randomized child ring for lineage and
// diagnostics, while grading only the authored minimum.  Presentation must
// therefore use the minimum too: showing the recorded maximum (for example
// Shiori's fifteen #435 children) falsely tells the player that every possible
// projectile has to connect.
struct RequirementCounts {
    int recordedContacts = 0;
    int recordedComboHits = 0;
    int minimumContacts = 0;
    int minimumComboHits = 0;
    bool fanout = false;
};

constexpr int RequiredContacts(const RequirementCounts& counts) {
    return counts.fanout ? counts.minimumContacts : counts.recordedContacts;
}

constexpr int RequiredComboHits(const RequirementCounts& counts) {
    return counts.fanout ? counts.minimumComboHits : counts.recordedComboHits;
}

constexpr bool RequirementSatisfied(const RequirementCounts& counts,
                                    int contactsSeen, int comboHitsSeen) {
    return contactsSeen >= RequiredContacts(counts) &&
           comboHitsSeen >= RequiredComboHits(counts);
}

// Contacts and combo hits are separate grading safeguards, but exposing both
// counters produces implementation-facing text such as
// "CONTACTS 0/4  COMBO HITS 0/4".  The recipe needs only one compact hit count.
// Prefer whichever safeguard carries the larger target.  Completion and
// failure are communicated by the token tint rather than another text field.
constexpr int CompactRequiredCount(int contactsRequired,
                                   int comboHitsRequired) {
    return contactsRequired > comboHitsRequired
        ? contactsRequired : comboHitsRequired;
}

inline std::string CompactCountSuffix(int contactsRequired,
                                      int comboHitsRequired) {
    const int required = CompactRequiredCount(contactsRequired,
                                              comboHitsRequired);
    if (required <= 1) return {};
    // Status tint carries live progress.  Keeping the text as xN avoids the
    // recipe shifting sideways when 0/4 becomes 1/4 during a combo.
    return " x" + std::to_string(required);
}

inline std::string CompactCountSuffix(const RequirementCounts& counts) {
    return CompactCountSuffix(RequiredContacts(counts),
                              RequiredComboHits(counts));
}

inline int DisplayAnchor(int contactAfterAction, int dueBeforeStep,
                         int afterStep, int totalActions) {
    if (totalActions < 0) return 0;
    if (dueBeforeStep >= 0 && dueBeforeStep < totalActions) {
        return dueBeforeStep;
    }
    if (contactAfterAction >= 0) {
        return (std::min)(contactAfterAction + 1, totalActions);
    }
    if (afterStep >= 0) {
        return (std::min)(afterStep + 1, totalActions);
    }
    return totalActions;
}

inline std::string FriendlyOutcomeLabel(const std::string& label,
                                        const std::string& outcome) {
    const std::string technical = " (" + outcome + ")";
    if (label.size() < technical.size() ||
        label.compare(label.size() - technical.size(), technical.size(),
                      technical) != 0) {
        return label;
    }
    std::string friendly = outcome;
    for (char& ch : friendly) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    std::string result = label;
    result.replace(result.size() - technical.size(), technical.size(),
                   " (" + friendly + ")");
    return result;
}

} // namespace Mission::EntityPresentationPolicy
