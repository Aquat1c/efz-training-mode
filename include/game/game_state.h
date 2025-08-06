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