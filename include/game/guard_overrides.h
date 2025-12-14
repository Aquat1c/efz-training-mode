#pragma once
#include <cstdint>

namespace GuardOverrides {
    // Returns true if the given moveId for the character should be treated as a grounded overhead (i.e., block high)
    // charId values are the CHAR_ID_* constants from core/constants.h
    // attackerBase: player base pointer for the attacker (needed for state-dependent checks like Mishio's element)
    bool IsGroundedOverhead(int charId, int moveId, uintptr_t attackerBase);
    
    // Returns true if the given moveId for the character should be treated as low even when airborne
    // (e.g., Makoto's 236236A/B/C which hit low from the air)
    bool IsAirborneLow(int charId, int moveId);
}
