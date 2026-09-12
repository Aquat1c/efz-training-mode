// Random Block module (EfzRevival-style per-frame coin flip on autoblock)
#pragma once
#include <atomic>
#include <cstdint>

namespace RandomBlock {
    // Enable/disable the feature
    void SetEnabled(bool enabled);
    bool IsEnabled();
    uint64_t GetMutationGeneration();
    bool SetEnabledIfGeneration(bool enabled, uint64_t expectedGeneration);

    // Per-frame tick; pass current move IDs so we can safely defer OFF while guarding
    void Tick(short p1MoveId, short p2MoveId);
}
