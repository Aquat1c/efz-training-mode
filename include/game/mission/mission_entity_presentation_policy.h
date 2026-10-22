#pragma once

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>

// Presentation-only policy: may hide or fold rows, but must never alter what
// the runner grades.
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

// A contact may attach silently to its producer token only when its producer/
// contact/deadline gates all describe that one user-facing action; traps,
// cross-action summons, interleaved hits, and survivors keep their own token.
struct ExactProducerHit {
    int action = -1;
    // Legacy timing inference is not enough evidence to remove a strict
    // entity obligation from the recipe.
    bool exactSource = false;
    bool generated = false;
    bool resolvedContact = false;
    bool actionEligible = false;
    // A tagged entity-only command is exact without a player move ID.
    bool commandSource = false;
    // Persistent setplay/traps/summons need the stronger producer proof below:
    // a child allocated during a later normal can still belong to an earlier setter.
    bool ordinaryProjectile = false;
    bool producerMoveMatches = false;
    // A stale source pair from an earlier part must never absorb a later
    // segment's entity obligation.
    bool sourceSegmentMatches = true;
    // A combo-end marker on the source step is a recovery boundary; keep that
    // contact visible as the next setup part.
    bool sourceComboEndBeforeContact = false;
};

// An ordinary fireball whose entity contact belongs to the same authored
// action is already represented by that action cell; anything cross-action,
// interleaved, long-lived, or combo-ending gets its own row.
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
    // A barrier from any later action proves the entity activated as an
    // independently timed object.
    if (timing.afterStep > candidate.action ||
        (timing.afterStepContact > 0 &&
         timing.afterStep != candidate.action)) {
        return false;
    }
    // Anything later means the entity survived across an intervening authored
    // action and must remain visible on its own.
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
    // The exact gates above proved this ordinal is another contact of the same
    // producer action, not an intervening user action.
    if (producerTiming.afterStep == candidate.action) {
        producerTiming.afterStepContact = -1;
    }
    producerTiming.comboEndAfter = false;
    return !IsStructurallyStandalone(producerTiming);
}

// Without this guard, notation generated for another overload of the same
// entity pattern could make a mismatched label look generated and foldable.
constexpr bool PersistedSemanticMatchesSource(bool hasPersistedSemantic,
                                              bool exactSource,
                                              int persistedProducerMove,
                                              int sourceMove) {
    return hasPersistedSemantic &&
        (!exactSource || persistedProducerMove == sourceMove);
}

// Explicitly unresolved new recordings return -1 even for an ordinary
// InlineProjectile.
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

// Unknown committed contacts stay visible as raw #pattern evidence; known
// traps/summons/setplay may opt into an unconditional row.
constexpr bool ShouldDisplay(bool mapped, bool alwaysStandalone,
                             bool failed, const Timing& timing) {
    (void)failed;
    // Failure changes tint, never layout.
    return !mapped || alwaysStandalone || IsStructurallyStandalone(timing);
}

// `comboEndAfter` alone does not make an exact immediate projectile a second
// user action; only CanInlineExactProducerHit's exact proof may hide a row.
constexpr bool ShouldDisplay(bool mapped, bool alwaysStandalone,
                             bool failed, const Timing& timing,
                             bool inlineExactProducerHit) {
    (void)mapped;
    (void)alwaysStandalone;
    (void)failed;
    (void)timing;
    // Timing is not causality; the exact producer proof is the only authority
    // allowed to remove a raw obligation from the visible recipe.
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
// between them; within one deadline, same-slot morph phases or children from
// one cast may share a visible token without weakening the grading schedule.
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

// Presentation must use the authored minimum too: showing the recorded
// maximum would tell the player every possible projectile has to connect.
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

// One compact hit count: prefer whichever safeguard carries the larger target.
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
    // xN (not n/N) keeps the recipe from shifting sideways mid-combo.
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
