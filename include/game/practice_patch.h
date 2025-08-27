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
bool SetPracticeAutoBlockEnabled(bool enabled);
bool GetPracticeAutoBlockEnabled(bool &enabledOut);

// Block Mode (maps to game state +4934): 0=None, 1=First, 2=All
bool SetPracticeBlockMode(int mode /*0..2*/);
bool GetPracticeBlockMode(int &modeOut);
bool CyclePracticeBlockMode();
