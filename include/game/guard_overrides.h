#pragma once
#include <cstdint>

namespace GuardOverrides {
    // True when the grounded move must be blocked high. charId is a CHAR_ID_*;
    // attackerBase is the attacker's player base (needed for state-dependent
    // checks such as Mishio's element).
    bool IsGroundedOverhead(int charId, int moveId, uintptr_t attackerBase);
    
    // True when the airborne move still hits low (e.g. Makoto's 236236A/B/C).
    bool IsAirborneLow(int charId, int moveId);
}
