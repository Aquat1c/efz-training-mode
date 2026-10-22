#pragma once

// Pure policy for tutorial goal-state predicates, shared by the JSON
// validator, capability preflight, runtime, and unit tests.

#include <string>

namespace Mission::TutorialStatePolicy {

// `distance` is |P1.x - P2.x| from the XPOS doubles (+0x20); `player` is
// ignored for it. `ic` is the IC color latch at +0x120 (0 = red, nonzero =
// blue), exposed as 0/1.
inline bool IsCoherentField(const std::string& field) {
    return field == "sp" || field == "rf" || field == "guard" ||
           field == "hp" || field == "distance" || field == "ic";
}

// `untech` is the signed 16-bit counter at +0x124; `launched`/`airtech`/
// `downed` are 0/1 classifications of the current move state. A zero untech
// counter is NOT "can recover": EFZ has additional recovery gates.
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

// Open on a positive sample (a standing dummy must not satisfy the task);
// close on <=0 (actual gauge exhaustion, not a wall-clock estimate).
inline bool IsUntechActive(double untech) { return untech > 0.0; }
inline bool IsUntechEmpty(double untech) { return untech <= 0.0; }

// Wake timing begins when the defender LEAVES the groundtech family; a true
// meaty transitions directly from wakeup to hitstun with no actionable sample.
// `elapsedTicks` is the non-frozen 192 Hz clock.
inline bool IsWakeExit(bool wasGroundtech, bool isGroundtech) {
    return wasGroundtech && !isGroundtech;
}

inline bool IsWithinWakeWindow(int elapsedTicks, int maxTicks) {
    return maxTicks > 0 && elapsedTicks >= 0 && elapsedTicks <= maxTicks;
}

// The defender can enter hit/block reaction on the same sample as the
// learner's starter edge, so the initial phase consumes it immediately.
// Phases: 1=hit route, 2=block stop, 3=starter committed, outcome pending.
inline int BranchPhaseForOutcomeEdge(bool hitEdge, bool blockEdge) {
    return hitEdge ? 1 : blockEdge ? 2 : 3;
}

} // namespace Mission::TutorialStatePolicy
