#include "../include/game/always_rg.h"
#include "../include/core/memory.h"
#include "../include/core/constants.h"
#include "../include/core/logger.h"
#include "../include/game/game_state.h"
#include "../include/utils/utilities.h"
#include "../include/utils/switch_players.h"
#include <mutex>

namespace AlwaysRG {
	static std::atomic<bool> g_enabled{false};
	static std::mutex g_writeMutex;
	static std::atomic<uint64_t> g_generation{0};

	static void LogState(bool enabled) {
		LogOut(std::string("[ALWAYS_RG] ") + (enabled ? "ENABLED" : "DISABLED"), true);
	}

	void SetEnabled(bool enabled) {
		std::lock_guard<std::mutex> lk(g_writeMutex);
		g_enabled.store(enabled);
		g_generation.fetch_add(1, std::memory_order_release);
		LogState(enabled);
	}

	bool IsEnabled() { return g_enabled.load(); }

	uint64_t GetMutationGeneration() {
		return g_generation.load(std::memory_order_acquire);
	}

	bool SetEnabledIfGeneration(bool enabled, uint64_t expectedGeneration) {
		std::lock_guard<std::mutex> lk(g_writeMutex);
		if (g_generation.load(std::memory_order_relaxed) != expectedGeneration) return false;
		g_enabled.store(enabled);
		g_generation.store(expectedGeneration + 1, std::memory_order_release);
		LogState(enabled);
		return true;
	}

	// Engine field: byte at [playerBase + 334] is the RG arm timer. Writing ~0x3C keeps it armed.
	// We arm the practice DUMMY side - normally P2, but when the player has
	// swapped controls onto the P2 side the dummy is P1, so resolve the side
	// through SwitchPlayers like the other practice systems do.
	void Tick(short /*p1MoveId*/, short /*p2MoveId*/) {
		if (!g_enabled.load()) return;
		if (GetCurrentGameMode() != GameMode::Practice) return;
		if (GetCurrentGamePhase() != GamePhase::Match) return;

		const uintptr_t dummy = GetPlayerBase(SwitchPlayers::GetRemotePlayerIndex());
		if (!dummy) return;

		// Write 0x3C to the RG arm byte (+334). Keep it byte-sized to match the engine's usage.
		uint8_t arm = 0x3C;
		SafeWriteMemory(dummy + 334, &arm, sizeof(arm));
		// Optional: extremely low-frequency debug spam gate is omitted here to keep it quiet.
	}
}
