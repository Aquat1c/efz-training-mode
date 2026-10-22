#pragma once
#include <atomic>
#include <cstdint>

// Tick() writes 0x3C to [P2+334] during Practice Match phase, keeping RG armed
// without altering normal guard rules.
namespace AlwaysRG {
	void SetEnabled(bool enabled);
	bool IsEnabled();
	uint64_t GetMutationGeneration();
	bool SetEnabledIfGeneration(bool enabled, uint64_t expectedGeneration);
	void Tick(short p1MoveId, short p2MoveId);
}
