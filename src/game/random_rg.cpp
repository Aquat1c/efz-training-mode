#include "../include/game/random_rg.h"
#include "../include/core/memory.h"
#include "../include/core/constants.h"
#include "../include/core/logger.h"
#include "../include/game/game_state.h"
#include "../include/utils/utilities.h" // GetEFZBase
#include "../include/utils/switch_players.h"
#include <cstdlib> // rand
#include <atomic>

namespace RandomRG {
    static std::atomic<bool> g_enabled{false};
    // Track last byte written to avoid noisy logs
    static uint8_t g_lastArmValue = 0xFF; // 0xFF = uninitialized

    void SetEnabled(bool enabled) {
        g_enabled.store(enabled);
        g_lastArmValue = 0xFF; // reset edge tracker
        LogOut(std::string("[RANDOM_RG] ") + (enabled ? "ENABLED" : "DISABLED"), true);
    }

    bool IsEnabled() { return g_enabled.load(); }

    void Tick(short /*p1MoveId*/, short /*p2MoveId*/) {
        if (!g_enabled.load()) return;
        if (GetCurrentGameMode() != GameMode::Practice) return;
        if (GetCurrentGamePhase() != GamePhase::Match) return;

        // Arm the practice DUMMY side, not a fixed P2. When the player has
        // swapped controls onto the P2 side the dummy is P1, so resolve it
        // through SwitchPlayers exactly as ALWAYS RECOIL GUARD does - otherwise
        // the two toggles point at different characters after a swap.
        const uintptr_t dummy = GetPlayerBase(SwitchPlayers::GetRemotePlayerIndex());
        if (!dummy) return;

        // EfzRevival parity: coin flip each frame
        bool heads = (rand() & 1) != 0;
    uint8_t arm = heads ? 0x3C : 0x00;
    // IMPORTANT: RG arm byte is at +334 (decimal), not 0x334.
    SafeWriteMemory(dummy + 334, &arm, sizeof(arm));

        // Basic logging only on state change to keep output readable
        if (arm != g_lastArmValue) {
            g_lastArmValue = arm;
            LogOut(std::string("[RANDOM_RG] ") + (arm ? "armed" : "disarmed"), false);
        }
    }
}
