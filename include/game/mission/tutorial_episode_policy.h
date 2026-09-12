#pragma once

// Pure vocabulary/policy shared by tutorial content preflight and the live
// dummy driver.  Keeping these checks out of the motion implementation prevents
// a lesson from becoming launchable merely because its action string is nonempty.

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include "core/constants.h"
#include "mission_sequence_policy.h"

namespace Mission::TutorialEpisodePolicy {

inline bool IsInjectableAction(const std::string& action) {
    static const char* const kActions[] = {
        "jump",
        "5A", "5B", "5C", "2A", "2B", "2C", "6A", "6B", "6C",
        "66A", "66B", "66C", "662A", "662B", "662C", "5A>5B>2C",
        "j.A", "j.B", "j.C",
        "236A", "236B", "236C", "623A", "623B", "623C",
        "214A", "214B", "214C",
        // Down-down and half-circle specials ride the same buffer-freeze lane
        // as 236/623/214: DetermineButtonFromMotionType and the freeze/queue
        // pattern tables already cover MOTION_22A..C / MOTION_41236A..C.
        "22A", "22B", "22C", "41236A", "41236B", "41236C",
    };
    for (const char* known : kActions) if (action == known) return true;
    return false;
}

// These actions need a dedicated live driver rather than one ordinary motion
// token. A dash normal waits until the game's forward-dash state is visible
// before pressing its normal; the contact chain waits for each hit's freeze to
// finish before injecting the next cancel, so an RG freeze cannot consume the
// follow-up input.
inline bool IsDashNormalAction(const std::string& action) {
    return action == "66A" || action == "66B" || action == "66C" ||
           action == "662A" || action == "662B" || action == "662C";
}

// An approach needs proof that the forward dash really happened, but the
// session sampler is allowed to first observe any native forward-dash phase.
// 163 is startup, 164/178 are recovery variants, and Kaori uses 250. Backdash
// states are deliberately excluded.
inline bool IsForwardDashApproachState(short moveId) {
    return moveId == 163 || moveId == 164 || moveId == 178 || moveId == 250;
}

inline bool IsContactChainAction(const std::string& action) {
    return action == "5A>5B>2C";
}

// Multi-action episodes use the ordinary injection lane for every action
// except a first-step dash normal, which has its own dash-state driver. Keep
// this policy shared by schema validation, capability preflight, and runtime
// readiness so an accepted lesson cannot become a silent no-op in battle.
inline bool IsSupportedScriptAction(const std::string& action,
                                    std::size_t stepIndex) {
    if (action.empty() || !IsInjectableAction(action) || action == "jump" ||
        IsContactChainAction(action)) {
        return false;
    }
    return !IsDashNormalAction(action) || stepIndex == 0;
}

// Script acknowledgement is a move INSTANCE edge, not merely neutral->attack.
// This accepts real attack->attack cancels and same-ID chains (5A->5A), while
// retaining the shared animation-head guard against ordinary looping frames.
inline bool IsScriptAttackInstanceEdge(short currentMove, short currentFrame,
                                       short previousMove, short previousFrame) {
    return currentMove >= 200 &&
           ::Mission::SequencePolicy::IsMoveInstanceEdge(
               currentMove, currentFrame, previousMove, previousFrame);
}

inline bool ScriptCancelDelaySatisfied(int elapsedTicks,
                                       int destinationWaitTicks,
                                       bool freezeActive) {
    return !freezeActive && elapsedTicks >= destinationWaitTicks;
}

inline bool CanInjectContactChainFollowup(bool contactFreezeSeen,
                                          bool freezeActive) {
    return contactFreezeSeen && !freezeActive;
}

// Auto-action's onRG path does not press a normal on the RG entry edge. It
// waits out the defender's stand/crouch/air RG freeze first, then exposes the
// response input at the same validated boundary. Tutorial rg_answer episodes
// share this policy so their two-tick button press cannot expire in hitstop.
inline int RecoilGuardAnswerDelayTicks(short recoilGuardMove) {
    switch (recoilGuardMove) {
        case RG_STAND_ID:  return RG_STAND_FREEZE_DURATION;
        case RG_CROUCH_ID: return RG_CROUCH_FREEZE_DURATION;
        case RG_AIR_ID:    return RG_AIR_FREEZE_DURATION;
        default:           return -1;
    }
}

inline bool RecoilGuardAnswerDelaySatisfied(short recoilGuardMove,
                                            int elapsedTicks) {
    const int required = RecoilGuardAnswerDelayTicks(recoilGuardMove);
    return required >= 0 && elapsedTicks >= required;
}

inline bool IsSupportedTrigger(const std::string& trigger) {
    return trigger == "afterBlock" || trigger == "onRG" ||
           trigger == "afterHitstun" || trigger == "afterAirtech" ||
           trigger == "onWakeup";
}

inline bool IsSupportedRepeatMode(const std::string& mode) {
    return mode == "loop" || mode == "once" || mode == "afterFailure";
}

inline bool IsSupportedApproach(const std::string& approach) {
    return approach == "dash" || approach == "none";
}

// An opener launched from a standing start (dash normal, buffered special)
// reaches the learner with no visual warning, which makes reaction drills
// (RG, guard-point, armor) unreadable. The only supported telegraph is a
// neutral hop in place: press UP, wait for the dummy to land and settle,
// then run the authored opener. The hop IS the reaction cue, so it is only
// meaningful on taskArmed macro episodes with an authored opener.
inline bool IsSupportedTelegraph(const std::string& telegraph) {
    return telegraph.empty() || telegraph == "jump";
}

// A first-frame wakeup action is produced by AutoAction's native pre-buffer
// path.  Tutorial must configure and observe that producer, but must not lease
// P2 control or any of the motion/immediate/freeze writers it needs internally.
inline bool UsesNativeWakeProducer(const std::string& kind,
                                   const std::string& start,
                                   const std::string& trigger) {
    return kind == "macro" && start == "trigger" && trigger == "onWakeup";
}

inline bool NeedsTutorialWriterLeases(const std::string& kind,
                                      const std::string& start,
                                      const std::string& trigger) {
    return !UsesNativeWakeProducer(kind, start, trigger);
}

// The authored tutorial dummy has exclusive control of its reactions. Practice
// auto-actions remain configured for later use, but cannot run during a lesson;
// the scoped native wake producer is the sole exception and always targets P2.
inline bool SuppressUserAutoActions(bool tutorialActive,
                                    bool tutorialWakeLeaseActive) {
    return tutorialActive && !tutorialWakeLeaseActive;
}

inline int ResolveRuntimeAutoActionTarget(bool tutorialActive,
                                          bool tutorialWakeLeaseActive,
                                          int ordinaryTarget) {
    if (tutorialWakeLeaseActive) return 2;
    if (tutorialActive) return 0;
    return ordinaryTarget;
}

inline bool ExpectedAttackMatches(const std::vector<int>& expectedMoveIds,
                                  short moveId) {
    if (moveId < 200) return false;
    if (expectedMoveIds.empty()) return true;
    for (int expected : expectedMoveIds) {
        if (expected == static_cast<int>(moveId)) return true;
    }
    return false;
}

// Native On-Wakeup auto-action mapping. A wake reversal must be pre-buffered
// during the wake animation to come out on the FIRST actionable frame - only
// the shipped auto-action wake prearm machinery does that; a trigger-edge
// injection is always frames late. Maps an authored episode action onto the
// auto-action (action, strength) pair; actions outside that vocabulary
// (dash normals, contact chains, "jump") do not map.
inline bool WakeAutoActionForAction(const std::string& action,
                                    int& outAction, int& outStrength) {
    if (action.size() < 2) return false;
    const char btn = action.back();
    const int idx = btn == 'A' ? 0 : btn == 'B' ? 1 : btn == 'C' ? 2 : -1;
    if (idx < 0) return false;
    const std::string head = action.substr(0, action.size() - 1);
    outStrength = idx;
    if (head == "5")     { outAction = ACTION_5A + idx;  return true; }
    if (head == "2")     { outAction = ACTION_2A + idx;  return true; }
    if (head == "6")     { outAction = ACTION_6A + idx;  return true; }
    if (head == "4")     { outAction = ACTION_4A + idx;  return true; }
    if (head == "j.")    { outAction = ACTION_JA + idx;  return true; }
    if (head == "236")   { outAction = ACTION_QCF;       return true; }
    if (head == "623")   { outAction = ACTION_DP;        return true; }
    if (head == "214")   { outAction = ACTION_QCB;       return true; }
    if (head == "22")    { outAction = ACTION_22;        return true; }
    if (head == "41236") { outAction = ACTION_SUPER1;    return true; }
    return false;
}

// EFZ's Practice stance selector uses 0 for standing and 2 for crouching.
// Fixed-guard tutorial episodes select this native setting; they never need a
// synthetic P2 direction to hold the authored guard.
inline int FixedGuardPracticeMode(const std::string& clip) {
    return clip == "crouch" ? 2 : 0;
}

// A cue is a timed warning, not a decorative copy of the task label. Ordinary
// and player-reactive episodes show it when the attempt/cycle is armed. Most
// dummy-triggered episodes show it on their edge; native wakeup is the one
// exception because its reversal has already been pre-buffered by that point.
inline bool HasTimedCue(const std::string& text, int leadTicks) {
    return !text.empty() && leadTicks > 0;
}

inline bool CueStartsOnArm(const std::string& start,
                           const std::string& trigger = std::string()) {
    // A wakeup reversal is already pre-buffered when the trigger edge occurs;
    // warning at that edge is too late. Show its cue as soon as the task arms.
    return start != "trigger" || trigger == "onWakeup";
}

inline bool CueStartsOnTrigger(const std::string& start, bool triggerEdge) {
    return start == "trigger" && triggerEdge;
}

// Build one or more shuffled variant cycles. Every complete cycle contains
// each variant exactly once; callers bind the resulting indices to stable
// episode IDs so retries do not reroll an already revealed exercise.
inline std::vector<std::size_t> BuildBalancedVariantAssignment(
        uint32_t& seed, std::size_t memberCount, std::size_t variantCount) {
    std::vector<std::size_t> out;
    if (memberCount == 0 || variantCount == 0) return out;
    out.reserve(memberCount);
    std::vector<std::size_t> cycle(variantCount);
    for (std::size_t i = 0; i < variantCount; ++i) cycle[i] = i;
    if (seed == 0) seed = 1;
    while (out.size() < memberCount) {
        for (std::size_t i = variantCount; i > 1; --i) {
            seed = seed * 1664525u + 1013904223u;
            const std::size_t pick = (seed >> 8) % i;
            std::swap(cycle[i - 1], cycle[pick]);
        }
        for (std::size_t index : cycle) {
            if (out.size() == memberCount) break;
            out.push_back(index);
        }
    }
    return out;
}

inline bool ParseP1MovePredicate(const std::string& predicate, int& moveIdOut) {
    static const std::string kPrefix = "p1move:";
    if (predicate.rfind(kPrefix, 0) != 0 || predicate.size() == kPrefix.size()) {
        return false;
    }
    for (std::size_t i = kPrefix.size(); i < predicate.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(predicate[i]))) return false;
    }
    const long parsed = std::strtol(predicate.c_str() + kPrefix.size(), nullptr, 10);
    if (parsed < 0 || parsed > 65535) return false;
    moveIdOut = static_cast<int>(parsed);
    return true;
}

} // namespace Mission::TutorialEpisodePolicy
