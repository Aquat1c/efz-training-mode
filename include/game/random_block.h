// Per-frame coin flip on autoblock (EfzRevival-style)
#pragma once
#include <atomic>
#include <cstdint>

namespace RandomBlock {
    void SetEnabled(bool enabled);
    bool IsEnabled();
    uint64_t GetMutationGeneration();
    bool SetEnabledIfGeneration(bool enabled, uint64_t expectedGeneration);

    // Per-frame tick; defers OFF while the dummy is guarding
    void Tick(short p1MoveId, short p2MoveId);
}
