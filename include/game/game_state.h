#pragma once
#include <cstdint>
#include <string>

// Enum to represent the different game modes
enum class GameMode : uint8_t {
    Arcade = 0,
    Practice = 1,
    VsCpu = 3,
    VsHuman = 4,
    Replay = 5,
    AutoReplay = 6,
    Unknown = 255 // Default for unhandled values
};

// Function to get the current game mode
// REVISED: Now takes an optional out parameter for the raw value.
GameMode GetCurrentGameMode(uint8_t* rawValueOut = nullptr);

// Function to get the name of a game mode
std::string GetGameModeName(GameMode mode);
void DebugDumpScreenState();
bool IsInGameplayState();
bool IsInCharacterSelectScreen();

enum class FrontendExitTarget : uint8_t {
    CharacterSelect = 1,
    Loading = 2,
    Title = 0,
};

// Installs lightweight front-end safety hooks:
// - suppresses EFZ's DirectInput ESC/F-key battle hotkeys while our menu is open
// - redirects Practice ESC to the training menu instead of EFZ's character-select exit
// - lets requested battle cleanup return to Title or Loading instead of Character Select
bool EnsureFrontendControlHooksInstalled();

using BattleUpdateCallback = void (*)(void* battleContext);
void SetBattleUpdateCallbacks(BattleUpdateCallback beforeUpdate, BattleUpdateCallback afterUpdate);

// Monotonic IDs around the exact game-thread Battle update. Collision-hook
// events carry the active ID; consumers only read through the completed ID so
// a monitor tick cannot observe half of one game update.
uint32_t GetCurrentBattleUpdateBatch();
uint32_t GetCompletedBattleUpdateBatch();

bool CanRequestFrontendExit(FrontendExitTarget target);
bool RequestFrontendExit(FrontendExitTarget target);

// Enum to represent the different game phases
enum class GamePhase : uint8_t {
    Unknown = 0,
    Menu,          // Title / config / replay select
    CharacterSelect,
    Loading,
    Match,         // Active fight (intros / round / result still with spawned chars)
};

// Phase query
GamePhase GetCurrentGamePhase();

// Fast inline helper
inline bool IsMatchPhase() { return GetCurrentGamePhase() == GamePhase::Match; }
void LogPhaseIfChanged();
