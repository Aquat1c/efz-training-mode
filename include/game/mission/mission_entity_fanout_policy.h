#pragma once

#include <cctype>
#include <string>

// Curated policy for attacks which allocate several interchangeable attacking
// children from one cast.  This is intentionally not a generic
// "same-pattern means optional" rule: persistent traps, notes, summons,
// deterministic multi-hit phases, and successive casts must retain their
// exact producer obligations.
namespace Mission::EntityFanoutPolicy {

inline bool EqualsAsciiCaseInsensitive(const std::string& left,
                                       const char* right) {
    if (!right) return false;
    std::size_t index = 0;
    for (; index < left.size() && right[index] != '\0'; ++index) {
        const unsigned char leftChar =
            static_cast<unsigned char>(left[index]);
        const unsigned char rightChar =
            static_cast<unsigned char>(right[index]);
        if (std::tolower(leftChar) != std::tolower(rightChar)) return false;
    }
    return index == left.size() && right[index] == '\0';
}

constexpr bool IsShiori2141236ProducerMove(int moveId) {
    return moveId >= 312 && moveId <= 317;
}

constexpr bool IsShiori2141236AttackChild(int pattern) {
    return pattern == 435;
}

// Intentionally absent: Rumi/Nanase 214C (move 261, controller #429,
// attack-capable children #405-#408).  That move has four distinct controller
// emissions, and randomness is local to each volley.  The current persisted
// member identity (child slot/generation + cast action) cannot distinguish
// those volleys, so treating the whole cast as one minimum-one episode would
// erase three cadence obligations.
//
// Before that family can be admitted, both contact and whiffed-lifecycle
// members need an exact parent-emission identity:
//   * #429 controller slot and generation; and
//   * a controller-relative emission ordinal, incremented at the native child
//     allocation site (not inferred from contact time or monitor batch).
// Normalization must key on cast + controller instance + emission ordinal and
// build one episode per volley.  Only #405-#408 may be hit members;
// #409-#411 remain non-attacking outcomes/helpers.  Runtime must reproduce the
// same four ordered emission ordinals and apply any minimum within each volley,
// never across the complete cast.

inline bool IsFlexibleFanout(const std::string& resourceName,
                             int attackChildPattern,
                             int producerMoveId) {
    return EqualsAsciiCaseInsensitive(resourceName, "shiori") &&
           IsShiori2141236AttackChild(attackChildPattern) &&
           IsShiori2141236ProducerMove(producerMoveId);
}

} // namespace Mission::EntityFanoutPolicy
