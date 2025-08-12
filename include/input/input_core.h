#pragma once
#include <windows.h>
#include <cstdint>
#include <string>

// Core input constants
constexpr uint8_t GAME_INPUT_NEUTRAL = 0x00;
constexpr uint8_t GAME_INPUT_RIGHT = 0x01;
constexpr uint8_t GAME_INPUT_LEFT  = 0x02;
constexpr uint8_t GAME_INPUT_DOWN  = 0x04;
constexpr uint8_t GAME_INPUT_UP    = 0x08;
constexpr uint8_t GAME_INPUT_A     = 0x10;
constexpr uint8_t GAME_INPUT_B     = 0x20;
constexpr uint8_t GAME_INPUT_C     = 0x40;
constexpr uint8_t GAME_INPUT_D     = 0x80;

constexpr uintptr_t AI_CONTROL_FLAG_OFFSET = 164; // Confirmed from your codebase

// Direction combinations for diagonals
const uint8_t GAME_INPUT_DOWNRIGHT = GAME_INPUT_DOWN | GAME_INPUT_RIGHT;
const uint8_t GAME_INPUT_DOWNLEFT = GAME_INPUT_DOWN | GAME_INPUT_LEFT;
const uint8_t GAME_INPUT_UPRIGHT = GAME_INPUT_UP | GAME_INPUT_RIGHT;
const uint8_t GAME_INPUT_UPLEFT = GAME_INPUT_UP | GAME_INPUT_LEFT;
const uintptr_t INPUT_HORIZONTAL_OFFSET = 0x188;  // 1=right, 255=left, 0=neutral
const uintptr_t INPUT_VERTICAL_OFFSET = 0x189;    // 1=down, 255=up, 0=neutral
const uintptr_t INPUT_BUTTON_A_OFFSET = 0x18A; // 394
const uintptr_t INPUT_BUTTON_B_OFFSET = 0x18B; // 395
const uintptr_t INPUT_BUTTON_C_OFFSET = 0x18C; // 396
const uintptr_t INPUT_BUTTON_D_OFFSET = 0x18D; // 397
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