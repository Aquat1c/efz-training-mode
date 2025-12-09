#pragma once
#include <windows.h>
#include <cstdint>

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
// Query if RF freeze is currently enforcing IC color for a player (returns true if freeze active AND color lock enabled)
bool IsRFFreezeColorManaging(int player);

// Engine regen (F4/F5) parameter accessors and inference
enum class EngineRegenMode {
	Unknown = 0,
	Normal,
	F5_FullOrPreset,   // Any F5-driven preset/cycle observed (A==1000/2000 or B==3332)
	F4_FineTuneActive  // Heuristic: B==9999 and A stepping not at defaults
};

// Read per-player copies of engine params (copied from battleContext each tick)
// Returns true if both reads succeeded; values are 16-bit words.
bool ReadEngineRegenParams(uint16_t& outParamA, uint16_t& outParamB);

// Infer current engine-managed regen mode from param A/B
EngineRegenMode InferEngineRegenMode(uint16_t paramA, uint16_t paramB);

// Convenience: true when engine regen likely to override manual HP/Meter/RF writes
bool IsEngineRegenLikelyActive();
// Stateful inference with history and cooldown to avoid false F4 after F5 cycles
// Returns true if params were read; fills outMode and the latest A/B.
bool GetEngineRegenStatus(EngineRegenMode& outMode, uint16_t& outParamA, uint16_t& outParamB);
// Debug deep scan: attempt to locate Param A/B dynamically within a window if offsets drift.
// Returns true if a plausible pair is found; outputs offsets & values.
bool DebugScanRegenParamWindow(uintptr_t playerBase, uint32_t& outAOffset, uint16_t& outAVal, uint32_t& outBOffset, uint16_t& outBVal);
// Derive RF and gauge color from Param A (engine regen parameter)
bool DeriveRfFromParamA(uint16_t paramA, float& rfValue, bool& isBlueIC);

// Write engine regen params (per-player copies). Returns true on success.
bool WriteEngineRegenParams(uint16_t paramA, uint16_t paramB);
// Force F5 Automatic Regeneration preset (A=1000 or 2000, B=3332)
bool ForceEngineF5Preset(uint16_t presetA);
// Force F5 "Full values" effect: set HP to 9999 for both players and mark params coherently
bool ForceEngineF5Full();
// Force F4 RF Recovery value (A in 0..2000 step 5 recommended, B=9999)
bool ForceEngineF4Value(uint16_t targetA);

// Add these for position save/load
void SavePlayerPositions(uintptr_t base);
void LoadPlayerPositions(uintptr_t base);

// Add these declarations with the other function declarations
void InitRFFreezeThread();
void StartRFFreeze(double p1Value, double p2Value);
// New: freeze RF for only one player (1 or 2)
void StartRFFreezeOne(int player, double value);
void StopRFFreeze();
// New: stop RF freeze for one player (1 or 2)
void StopRFFreezePlayer(int player);
void StopRFFreezeThread();
// Configure whether RF freeze writes only apply when player is in neutral
void SetRFFreezeNeutralOnly(bool enabled);
// New: single-tick RF freeze maintenance (when folding thread into main loop)
void UpdateRFFreezeTick();

// New: write IC color for only one player without touching the other
bool SetICColorPlayer(int player, bool blueIC);

// New: configure whether to also lock IC color while RF Freeze is active for a player
// enabled=false disables color enforcement for that player. If enabled=true, blueIC selects Red(false)/Blue(true)
void SetRFFreezeColorDesired(int player, bool enabled, bool blueIC);

// RF Freeze provenance and status helpers
enum class RFFreezeOrigin {
	None = 0,
	ManualUI = 1,
	ContinuousRecovery = 2,
	Other = 3
};
// Start RF freeze for one player with origin attribution
void StartRFFreezeOneFromUI(int player, double value);
void StartRFFreezeOneFromCR(int player, double value);
// Query RF freeze status for a player
bool GetRFFreezeStatus(int player, bool& isActive, double& value, bool& colorManaged, bool& colorBlue);
// Query RF freeze origin for a player
RFFreezeOrigin GetRFFreezeOrigin(int player);

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