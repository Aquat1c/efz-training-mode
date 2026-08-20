// Always RG arming module
#pragma once
#include <atomic>
#include <cstdint>

// Minimal API for arming Recoil Guard (RG) continuously for the practice dummy.
// Usage:
//  - Call AlwaysRG::SetEnabled(true) to enable.
//  - Call AlwaysRG::Tick(p1MoveId, p2MoveId) each frame from the frame monitor.
// The Tick will write 0x3C to [P2+334] while in Practice Match phase, keeping RG armed
// without altering normal guard rules.
namespace AlwaysRG {
	void SetEnabled(bool enabled);
	bool IsEnabled();
	uint64_t GetMutationGeneration();
	bool SetEnabledIfGeneration(bool enabled, uint64_t expectedGeneration);
	void Tick(short p1MoveId, short p2MoveId);
}
