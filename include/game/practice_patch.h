#pragma once
#include <cstdint>
#include <string>

std::string FormatHexAddress(uintptr_t address);

bool EnablePlayer2InPracticeMode();

bool DisablePlayer2InPracticeMode();

void MonitorAndPatchPracticeMode();

void DumpPracticeModeState();

void LogPlayerInputsInPracticeMode();

void ScanForPotentialAIFlags();

void ResetP2Character();

// P1 player-controlled, P2 AI-controlled on Practice match entry.
void EnsureDefaultControlFlagsOnMatchStart();

// Practice dummy controls (F6/F7 equivalents via UI)
// Auto-Block toggle (game state +4936)
// 'reason' is logged whenever the flag actually changes.
bool SetPracticeAutoBlockEnabled(bool enabled, const char* reason = nullptr);
bool GetPracticeAutoBlockEnabled(bool &enabledOut);
uint64_t GetPracticeAutoBlockWriteGeneration();
bool RestorePracticeAutoBlockIfUnchanged(bool enabled,
                                         uint64_t expectedGeneration,
                                         bool expectedCurrent,
                                         const char* reason = nullptr);

// Syncs the internal Dummy Auto-Block mode to the current game flag (+4936);
// clearOverride=true stops overriding the flag from our custom modes so F7
// (or in-game toggles) take immediate effect.
bool SyncAutoBlockModeFromGameFlag(bool clearOverride);

// Block Mode (game state +4934): 0=None, 1=First, 2=All
bool SetPracticeBlockMode(int mode /*0..2*/);
bool GetPracticeBlockMode(int &modeOut);
bool CyclePracticeBlockMode();

// Extended Dummy Auto-Block modes (superset of F7):
// 0=None, 1=All (F7), 2=First Hit, 3=After First Hit, 4=Adaptive
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
struct PerFrameSample; // fwd decl
void MonitorDummyAutoBlock(const PerFrameSample& sample);

void SetAdaptiveStanceEnabled(bool enabled);
bool GetAdaptiveStanceEnabled();

// The desired autoblock window computed each frame: true when the mode logic
// wants autoblock ON (All mode, or the post-hit window in AfterFirstHit).
// Random Block randomizes only within these windows.
bool GetCurrentDesiredAutoBlockOn(bool &onOut);

// An external controller (e.g. Random Block) performs the +4936 write this
// frame, so MonitorDummyAutoBlock skips its own write and native-F7 sync;
// held for Random Block's complete enabled lifetime.
void SetExternalAutoBlockController(bool enabled);

// Long-lived writer exclusion for deterministic tutorial block episodes;
// composes with the RandomBlock controller ownership.
bool AcquireTutorialAutoBlockController(uint64_t& tokenOut);
void ReleaseTutorialAutoBlockController(uint64_t token);
