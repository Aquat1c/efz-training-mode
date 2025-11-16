#pragma once
#include <string>

// Helper function to format memory addresses as hex strings
std::string FormatHexAddress(uintptr_t address);

// Function to enable Player 2 controls in Practice mode
bool EnablePlayer2InPracticeMode();

// Function to disable Player 2 controls in Practice mode
bool DisablePlayer2InPracticeMode();

// Thread function to continuously monitor and patch practice mode
void MonitorAndPatchPracticeMode();

// Debug function to dump the state of practice mode
void DumpPracticeModeState();

// Function to monitor and log player inputs in practice mode
void LogPlayerInputsInPracticeMode();

// Function to scan for potential AI control flags
void ScanForPotentialAIFlags();

// Function to reset P2 character completely
void ResetP2Character();

// Ensure default control flags when entering a match in Practice:
// - P1 player-controlled, P2 AI-controlled
void EnsureDefaultControlFlagsOnMatchStart();

// Practice dummy controls (expose F6/F7 equivalents via UI)
// Auto-Block toggle (maps to game state +4936)
// Optional 'reason' is logged whenever the flag actually changes (for diagnostics)
bool SetPracticeAutoBlockEnabled(bool enabled, const char* reason = nullptr);
bool GetPracticeAutoBlockEnabled(bool &enabledOut);

// Sync internal Dummy Auto-Block mode to the current game flag (+4936).
// If clearOverride is true, stop overriding the flag from our custom modes
// so F7 (or in-game toggles) take immediate effect without deferral.
bool SyncAutoBlockModeFromGameFlag(bool clearOverride);

// Block Mode (maps to game state +4934): 0=None, 1=First, 2=All
bool SetPracticeBlockMode(int mode /*0..2*/);
bool GetPracticeBlockMode(int &modeOut);
bool CyclePracticeBlockMode();

// Extended Dummy Auto-Block modes (superset of F7):
// 0=None, 1=All (F7), 2=First Hit (disable after first block), 3=After First Hit (enable after first hit), 4=Adaptive
enum DummyAutoBlockMode : int {
	DAB_None = 0,
	DAB_All = 1,
	DAB_FirstHitThenOff = 2,
	DAB_EnableAfterFirstHit = 3,
	DAB_Adaptive = 4
};

void SetDummyAutoBlockMode(int mode);
int  GetDummyAutoBlockMode();
void ResetDummyAutoBlockState();
// Called every frame (Match only) with current and previous move IDs
void MonitorDummyAutoBlock(short p1MoveID, short p2MoveID, short prevP1MoveID, short prevP2MoveID);
// Context overload (PerFrameSample) used after unified sampling introduced
struct PerFrameSample; // fwd decl
void MonitorDummyAutoBlock(const PerFrameSample& sample);

// New: Adaptive stance can be used with any mode
void SetAdaptiveStanceEnabled(bool enabled);
bool GetAdaptiveStanceEnabled();

// Current desired autoblock window computed by MonitorDummyAutoBlock each frame.
// When true, the mode logic wants autoblock ON (e.g., All mode, or post-hit window in AfterFirstHit).
// Useful for features like Random Block to randomize ON only during these allowed windows.
bool GetCurrentDesiredAutoBlockOn(bool &onOut);

// Hint that an external controller (e.g., Random Block) will perform the +4936 write this frame,
// so MonitorDummyAutoBlock should skip its own write.
void SetExternalAutoBlockController(bool enabled);
