#pragma once
#include <windows.h>

// Function to read player inputs
uint8_t GetPlayerInputs(int playerNum);

// Add these declarations near the top with your other function declarations
bool SafeReadMemory(uintptr_t address, void* buffer, size_t size);
bool SafeWriteMemory(uintptr_t address, const void* data, size_t size);

// Memory manipulation functions
uintptr_t ResolvePointer(uintptr_t base, uintptr_t baseOffset, uintptr_t offset);
void WriteGameMemory(uintptr_t address, const void* data, size_t size);
void SetPlayerPosition(uintptr_t base, uintptr_t playerOffset, double x, double y, bool updateMoveID = true);
void UpdatePlayerValues(uintptr_t base, uintptr_t baseOffsetP1, uintptr_t baseOffsetP2);
void UpdatePlayerValuesExceptRF(uintptr_t base, uintptr_t baseOffsetP1, uintptr_t baseOffsetP2);
void ApplyRFValues(double p1RF, double p2RF);
bool PatchMemory(uintptr_t address, const char* bytes, size_t length);
bool NopMemory(uintptr_t address, size_t length);
bool SetRFValuesDirect(double p1RF, double p2RF);
bool SetICColorDirect(bool p1BlueIC, bool p2BlueIC);

// Add these for position save/load
void SavePlayerPositions(uintptr_t base);
void LoadPlayerPositions(uintptr_t base);

// Add these declarations with the other function declarations
void InitRFFreezeThread();
void StartRFFreeze(double p1Value, double p2Value);
void StopRFFreeze();

// Input bitmask constants
#define INPUT_UP     0x01
#define INPUT_DOWN   0x02
#define INPUT_LEFT   0x04
#define INPUT_RIGHT  0x08
#define INPUT_A      0x10  // Light attack
#define INPUT_B      0x20  // Medium attack
#define INPUT_C      0x40  // Heavy attack
#define INPUT_D      0x80  // Special

// Memory offset for player inputs (you'll need to update these with correct values)
#define P1_INPUT_OFFSET 0xB8  // Input bitmask offset for P1
//#define P2_INPUT_OFFSET 0x5678  // Unused for now.

// Add this function declaration near the other function declarations
bool GetPlayerFacingDirection(int playerNum);  // Returns true if facing right, false if facing left