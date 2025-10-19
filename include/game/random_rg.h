// Random RG arming module (EfzRevival parity)
#pragma once
#include <atomic>

// Minimal API to mimic EfzRevival's "Random RG" training option.
// While enabled, on each frame during a Practice Match, we flip a coin (rand() & 1)
// and write 0x3C (armed) or 0 (disarmed) to the RG arm byte at [P2 + 0x334].
// This matches the original behavior closely without additional sliders or probabilities.
namespace RandomRG {
    void SetEnabled(bool enabled);
    bool IsEnabled();
    void Tick(short p1MoveId, short p2MoveId);
}
