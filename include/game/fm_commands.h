#pragma once
#include <vector>
#include <cstdint>
#include "../core/constants.h"

// Entry describing a character Final Memory (FM) input sequence.
// pattern: unified GAME_INPUT_* byte masks in chronological order.
// Some FMs accept variant final button (e.g., Mio short range last input 236C or 623C): we encode
// the base pattern up to the divergence; injection code may append the chosen tail.
struct FinalMemoryCommand {
    int characterId;              // CHAR_ID_*
    const char* name;             // Short label
    std::vector<uint8_t> pattern; // Encoded input pattern (already includes buttons)
    bool (*gate)(int playerNum);  // Optional gating predicate (nullptr = always allowed)
    const char* gateDesc;         // Human-readable gating description (for logs/UI)
    int indexAdvance{0};          // Extra neutral frames to advance frozen index beyond last pattern element (0 = legacy behavior)
};

// Builds (or returns cached) FM pattern list (excluding characters with none like Doppel).
const std::vector<FinalMemoryCommand>& GetFinalMemoryCommands();

// Attempt to execute a character's Final Memory by freezing the buffer with its pattern.
// Returns true if pattern dispatched (character supported and pattern built).
bool ExecuteFinalMemory(int playerNum, int characterId);

// Utility to build a raw pattern from human friendly tokens.
// Grammar extensions:
//   Direction digits 1-9 optionally followed by button letters (A,B,C,S or D).
//   Optional *COUNT suffix to repeat the expanded mask COUNT times (e.g., "2*12", "2A*6").
//   Neutral may be expressed as '5' or 'N'. Example neutral gap: "5*9" for 9 frames neutral.
//   Without *COUNT:
//      - Pure direction produces DEFAULT_DIR_FRAMES repeats.
//      - Direction+button produces DEFAULT_BTN_FRAMES repeats (button held).
//      - Pure button (A/B/C/S/D) produces DEFAULT_BTN_FRAMES repeats.
//   Buttons inside a combined direction token are not split; they are held together.
//   Mirroring (facing) is applied after pattern construction.
std::vector<uint8_t> BuildPattern(const std::vector<const char*>& tokens, bool facingRight);
