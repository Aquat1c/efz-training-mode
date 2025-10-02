#pragma once
#include <windows.h>
#include <cstdint>
#include <string>
// Use the single source of truth for raw offsets
#include "../core/constants.h"

// Core input constants (UNIFIED INPUT MASK)
// These bitflags match the game's buffer encoding, but we use them as a single
// logical mask throughout the codebase. For immediate-register injection, these
// flags are translated by WritePlayerInputImmediate() into the proper per-axis
// and per-button register values. Only motion queues/freezes should write these
// masks into the circular buffer directly.
constexpr uint8_t GAME_INPUT_NEUTRAL = 0x00;
constexpr uint8_t GAME_INPUT_RIGHT = 0x01;
constexpr uint8_t GAME_INPUT_LEFT  = 0x02;
constexpr uint8_t GAME_INPUT_DOWN  = 0x04;
constexpr uint8_t GAME_INPUT_UP    = 0x08;
constexpr uint8_t GAME_INPUT_A     = 0x10;
constexpr uint8_t GAME_INPUT_B     = 0x20;
constexpr uint8_t GAME_INPUT_C     = 0x40;
constexpr uint8_t GAME_INPUT_D     = 0x80;

constexpr uintptr_t AI_CONTROL_FLAG_OFFSET = 0xA4; // Hex offset; confirmed from your codebase.

// Direction combinations for diagonals
const uint8_t GAME_INPUT_DOWNRIGHT = GAME_INPUT_DOWN | GAME_INPUT_RIGHT;
const uint8_t GAME_INPUT_DOWNLEFT = GAME_INPUT_DOWN | GAME_INPUT_LEFT;
const uint8_t GAME_INPUT_UPRIGHT = GAME_INPUT_UP | GAME_INPUT_RIGHT;
const uint8_t GAME_INPUT_UPLEFT = GAME_INPUT_UP | GAME_INPUT_LEFT;

// ===================== IMPORTANT INPUT INJECTION POLICY =====================
// Always inject gameplay inputs into the IMMEDIATE REGISTERS below for:
//   - Normals (5A/2B/etc.)
//   - Jumps (U/UF/UB)
//   - Simple dashes if not using motion queue
// Do NOT write these actions into the circular input buffer; doing so can cause
// misalignment and missed presses. Use buffer writes only for motion sequences
// driven by the queue (e.g., 236/214/etc.) or buffer-freeze utilities.
// Implementation notes:
//   - The input hook respects g_injectImmediateOnly[player] to skip buffer
//     writes while still applying immediate values. Set it to true for
//     normals/jumps and false when explicitly queuing motions.
//   - Write order is immediate first, optional buffer second. Keep buffer off
//     unless you are performing a motion via the queue.
// ============================================================================
// Immediate input registers (raw)
// Note: Horizontal/Vertical are tightly packed and buttons are contiguous bytes.
// Alias to the canonical definitions in constants.h to prevent drift.
constexpr uintptr_t INPUT_HORIZONTAL_OFFSET = HORIZONTAL_INPUT_OFFSET;  // 0x188
constexpr uintptr_t INPUT_VERTICAL_OFFSET   = VERTICAL_INPUT_OFFSET;    // 0x189
constexpr uintptr_t INPUT_BUTTON_A_OFFSET   = BUTTON_A_OFFSET;          // 0x18A
constexpr uintptr_t INPUT_BUTTON_B_OFFSET   = BUTTON_B_OFFSET;          // 0x18B
constexpr uintptr_t INPUT_BUTTON_C_OFFSET   = BUTTON_C_OFFSET;          // 0x18C
constexpr uintptr_t INPUT_BUTTON_D_OFFSET   = BUTTON_D_OFFSET;          // 0x18D
// Button constants for cleaner code
#define BUTTON_A    GAME_INPUT_A
#define BUTTON_B    GAME_INPUT_B
#define BUTTON_C    GAME_INPUT_C
#define BUTTON_D    GAME_INPUT_D

// Core input functions
uintptr_t GetPlayerPointer(int playerNum);
std::string DecodeInputMask(uint8_t inputMask);
bool WritePlayerInput(int playerNum, uint8_t inputMask);
bool WritePlayerInputImmediate(int playerNum, uint8_t inputMask);
bool WritePlayerInputToBuffer(int playerNum, uint8_t inputMask);
// Utility: forcibly reset the player's circular input buffer index to 0.
// Used before queuing certain motion sequences (e.g., forward dash + chained normal)
// to create a deterministic starting window for the engine's dash recognition.
// Returns true on success.
bool ResetPlayerInputBufferIndex(int playerNum);
// Clear entire circular input buffer (set every byte to 0) and optionally reset index.
// Used when returning control to AI so stale patterns cannot accidentally trigger motions.
bool ClearPlayerInputBuffer(int playerNum, bool resetIndex);
// Dump buffer contents for debugging (shows last 15 positions and recognition window)
void DumpInputBuffer(int playerNum, const std::string& context);
// Read the current MoveID value (offset 610 = 0x262)
uint16_t GetPlayerMoveID(int playerNum);