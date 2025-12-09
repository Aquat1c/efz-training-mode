#pragma once
#include <windows.h>
#include <string>

// Global variables
extern short initialBlockstunMoveID;

// Function declarations
bool IsHitstun(short moveID);
bool IsLaunched(short moveID);
bool IsAirtech(short moveID);
bool IsGroundtech(short moveID);
bool IsFrozen(short moveID);
bool IsSpecialStun(short moveID);
bool IsThrown(short moveID);
bool IsBlockstunState(short moveID);    // Make sure this is declared
int GetAttackLevel(short blockstunMoveID);
std::string GetBlockStateType(short blockstunMoveID);
int GetExpectedFrameAdvantage(int attackLevel, bool isAirBlock, bool isHit = false);
short GetUntechValue(uintptr_t base, int player);

// Read blockstun/guard-freeze counter (short) at BLOCKSTUN_OFFSET
short GetBlockstunValue(uintptr_t base, int player);