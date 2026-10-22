#pragma once
#include <windows.h>
#include <cstdint>

uint8_t GetPlayerInputs(int playerNum);

bool SafeReadMemory(uintptr_t address, void* buffer, size_t size);
bool SafeWriteMemory(uintptr_t address, const void* data, size_t size);

uintptr_t ResolvePointer(uintptr_t base, uintptr_t baseOffset, uintptr_t offset);
void WriteGameMemory(uintptr_t address, const void* data, size_t size);
// Verifies the writes and reports failure to the caller.
bool TrySetPlayerPosition(uintptr_t base, uintptr_t playerOffset, double x, double y,
                          bool updateMoveID = true);
void SetPlayerPosition(uintptr_t base, uintptr_t playerOffset, double x, double y, bool updateMoveID = true);
void UpdatePlayerValues(uintptr_t base, uintptr_t baseOffsetP1, uintptr_t baseOffsetP2);
void UpdatePlayerValuesExceptRF(uintptr_t base, uintptr_t baseOffsetP1, uintptr_t baseOffsetP2);
void ApplyRFValues(double p1RF, double p2RF);
bool PatchMemory(uintptr_t address, const char* bytes, size_t length);
bool NopMemory(uintptr_t address, size_t length);
bool SetRFValuesDirect(double p1RF, double p2RF);
bool SetICColorDirect(bool p1BlueIC, bool p2BlueIC);
// True when RF freeze is active AND color lock is enabled for that player.
bool IsRFFreezeColorManaging(int player);

enum class EngineRegenMode {
	Unknown = 0,
	Normal,
	F5_FullOrPreset,   // A==1000/2000 or B==3332
	F4_FineTuneActive  // B==9999 and A stepping not at defaults
};

// Values are 16-bit words copied from battleContext each tick; true if both
// reads succeeded.
bool ReadEngineRegenParams(uint16_t& outParamA, uint16_t& outParamB);

EngineRegenMode InferEngineRegenMode(uint16_t paramA, uint16_t paramB);

// True when engine regen is likely to override manual HP/Meter/RF writes.
bool IsEngineRegenLikelyActive();
// Stateful inference (history + cooldown) to avoid false F4 after F5 cycles;
// true if params were read.
bool GetEngineRegenStatus(EngineRegenMode& outMode, uint16_t& outParamA, uint16_t& outParamB);
// Debug: scans a window for Param A/B if offsets drift; true if a plausible
// pair is found.
bool DebugScanRegenParamWindow(uintptr_t playerBase, uint32_t& outAOffset, uint16_t& outAVal, uint32_t& outBOffset, uint16_t& outBVal);
bool DeriveRfFromParamA(uint16_t paramA, float& rfValue, bool& isBlueIC);

bool WriteEngineRegenParams(uint16_t paramA, uint16_t paramB);
// A=1000 or 2000, B=3332.
bool ForceEngineF5Preset(uint16_t presetA);
// Sets HP to 9999 for both players.
bool ForceEngineF5Full();
// A in 0..2000 step 5; B=9999.
bool ForceEngineF4Value(uint16_t targetA);

void SavePlayerPositions(uintptr_t base);
void LoadPlayerPositions(uintptr_t base);

void StartRFFreeze(double p1Value, double p2Value);
void StartRFFreezeOne(int player, double value);
void StopRFFreeze();
void StopRFFreezePlayer(int player);
// When enabled, RF freeze writes apply only while the player is in neutral.
void SetRFFreezeNeutralOnly(bool enabled);
// Single-tick RF freeze maintenance.
void UpdateRFFreezeTick();

bool SetICColorPlayer(int player, bool blueIC);

// While RF freeze is active, also lock IC color (blueIC: false=red, true=blue).
void SetRFFreezeColorDesired(int player, bool enabled, bool blueIC);

enum class RFFreezeOrigin {
	None = 0,
	ManualUI = 1,
	ContinuousRecovery = 2,
	Other = 3
};
void StartRFFreezeOneFromUI(int player, double value);
void StartRFFreezeOneFromCR(int player, double value);
bool GetRFFreezeStatus(int player, bool& isActive, double& value, bool& colorManaged, bool& colorBlue);
RFFreezeOrigin GetRFFreezeOrigin(int player);

#define INPUT_UP     0x01
#define INPUT_DOWN   0x02
#define INPUT_LEFT   0x04
#define INPUT_RIGHT  0x08
#define INPUT_A      0x10  // Light attack
#define INPUT_B      0x20  // Medium attack
#define INPUT_C      0x40  // Heavy attack
#define INPUT_D      0x80  // Special

#define P1_INPUT_OFFSET 0xB8

bool GetPlayerFacingDirection(int playerNum);  // true = facing right
