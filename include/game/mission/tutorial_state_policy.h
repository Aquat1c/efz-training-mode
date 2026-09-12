#pragma once

// Pure policy for tutorial goal-state predicates. Keeping field ownership and
// comparisons here lets the JSON validator, capability preflight, runtime, and
// unit tests agree without duplicating strings or silently broadening a
// capability.

#include <string>

namespace Mission::TutorialStatePolicy {

// `distance` is |P1.x - P2.x| from the XPOS doubles (+0x20) both overlays
// already sample; `player` is ignored for it. `ic` is the IC color latch at
// +0x120 (0 = red, nonzero = blue - the same int SetICColorPlayer writes),
// exposed as 0/1 so "eq 0"/"eq 1" express "while in Red/Blue IC state".
inline bool IsCoherentField(const std::string& field) {
    return field == "sp" || field == "rf" || field == "guard" ||
           field == "hp" || field == "distance" || field == "ic";
}

// `juggle_state` deliberately exposes only values already established by the
// runtime map/classifiers. `untech` is the signed 16-bit counter at +0x124;
// `launched`, `airtech`, and `downed` are 0/1 classifications of the current
// move state (`downed` = the groundtech/knockdown family auto-action's wake
// logic already trusts). A zero untech counter is NOT advertised as "can
// recover": EFZ has additional recovery gates, and inferring them here would
// make the contract dishonest.
inline bool IsJuggleField(const std::string& field) {
    return field == "untech" || field == "launched" || field == "airtech" ||
           field == "downed";
}

inline bool IsKnownField(const std::string& field) {
    return IsCoherentField(field) || IsJuggleField(field);
}

inline bool IsKnownOperator(const std::string& op) {
    return op == "ge" || op == "le" || op == "gt" || op == "lt" ||
           op == "eq";
}

inline bool Compare(double observed, const std::string& op, double target) {
    if (op == "ge") return observed >= target;
    if (op == "le") return observed <= target;
    if (op == "gt") return observed > target;
    if (op == "lt") return observed < target;
    if (op == "eq") return observed == target;
    return false;
}

// Exact counter gates for the Juggle Gauge observation window. Opening on a
// positive sample prevents a standing/root-baseline dummy from satisfying the
// task; closing on <=0 binds success to the actual gauge exhaustion rather than
// an authored wall-clock estimate.
inline bool IsUntechActive(double untech) { return untech > 0.0; }
inline bool IsUntechEmpty(double untech) { return untech <= 0.0; }

// Wake timing begins when the defender LEAVES the groundtech family, not only
// when an actionable neutral state is sampled. A true meaty can transition
// directly from wakeup to hitstun and therefore has no intervening actionable
// sample. `elapsedTicks` is the tutorial monitor's non-frozen 192 Hz clock.
inline bool IsWakeExit(bool wasGroundtech, bool isGroundtech) {
    return wasGroundtech && !isGroundtech;
}

inline bool IsWithinWakeWindow(int elapsedTicks, int maxTicks) {
    return maxTicks > 0 && elapsedTicks >= 0 && elapsedTicks <= maxTicks;
}

// branch_on_outcome starts watching on the learner's starter move edge. The
// defender can enter hit/block reaction on that SAME monitor sample, so the
// initial branch phase must consume the edge immediately instead of always
// parking in an outcome-pending phase. Runtime phases: 1=hit route, 2=block
// stop, 3=starter committed and outcome still pending.
inline int BranchPhaseForOutcomeEdge(bool hitEdge, bool blockEdge) {
    return hitEdge ? 1 : blockEdge ? 2 : 3;
}

} // namespace Mission::TutorialStatePolicy
