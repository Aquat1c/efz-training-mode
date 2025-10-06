#include "../include/game/always_rg.h"
#include "../include/core/memory.h"
#include "../include/core/constants.h"
#include "../include/core/logger.h"
#include "../include/game/game_state.h"
#include "../include/utils/utilities.h"

namespace AlwaysRG {
	static std::atomic<bool> g_enabled{false};

	void SetEnabled(bool enabled) {
		g_enabled.store(enabled);
		LogOut(std::string("[ALWAYS_RG] ") + (enabled ? "ENABLED" : "DISABLED"), true);
	}

	bool IsEnabled() { return g_enabled.load(); }

	// Engine field: byte at [playerBase + 334] is the RG arm timer. Writing ~0x3C keeps it armed.
	// We only arm P2 (the practice dummy) and only during Practice matches to avoid side effects.
	void Tick(short /*p1MoveId*/, short /*p2MoveId*/) {
		if (!g_enabled.load()) return;
		if (GetCurrentGameMode() != GameMode::Practice) return;
		if (GetCurrentGamePhase() != GamePhase::Match) return;

		uintptr_t base = GetEFZBase();
		if (!base) return;
		uintptr_t p2 = 0;
		if (!SafeReadMemory(base + EFZ_BASE_OFFSET_P2, &p2, sizeof(p2)) || !p2) return;

		// Write 0x3C to the RG arm byte (+334). Keep it byte-sized to match the engine's usage.
		uint8_t arm = 0x3C;
		SafeWriteMemory(p2 + 334, &arm, sizeof(arm));
		// Optional: extremely low-frequency debug spam gate is omitted here to keep it quiet.
	}
}
