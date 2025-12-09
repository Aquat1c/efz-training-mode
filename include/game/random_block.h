// Random Block module (EfzRevival-style per-frame coin flip on autoblock)
#pragma once
#include <atomic>

namespace RandomBlock {
    // Enable/disable the feature
    void SetEnabled(bool enabled);
    bool IsEnabled();

    // Per-frame tick; pass current move IDs so we can safely defer OFF while guarding
    void Tick(short p1MoveId, short p2MoveId);
}
