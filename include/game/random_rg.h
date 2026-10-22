#pragma once
#include <atomic>

// Mimics EfzRevival's "Random RG" training option: each frame during a
// Practice Match, flip a coin (rand() & 1) and write 0x3C (armed) or 0
// (disarmed) to the RG arm byte at [P2 + 0x334].
namespace RandomRG {
    void SetEnabled(bool enabled);
    bool IsEnabled();
    void Tick(short p1MoveId, short p2MoveId);
}
