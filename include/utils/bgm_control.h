#pragma once
#include <windows.h>
#include <string>

// BGM control API for EFZ Training Mode
// Set BGM volume instantly (DirectSound scale: 0 = max, -10000 = mute)
bool SetBGMVolume(uintptr_t gameStatePtr, int volume);

// Mute BGM instantly
bool MuteBGM(uintptr_t gameStatePtr);

// Unmute BGM (set to full volume)
bool UnmuteBGM(uintptr_t gameStatePtr);

// Stop currently playing BGM (calls game's stop logic if possible)
bool StopBGM(uintptr_t gameStatePtr);

// Play a BGM track by index (calls game's play logic if possible)
bool PlayBGM(uintptr_t gameStatePtr, unsigned short trackNumber);

// Get current BGM slot/index
int GetBGMSlot(uintptr_t gameStatePtr);

// Get current BGM volume (DirectSound scale)
int GetBGMVolume(uintptr_t gameStatePtr);

// Log current BGM state (slot, volume, etc)
void LogBGMState(uintptr_t gameStatePtr);

// Legacy suppression/toggle interfaces removed; stubs exist only in implementation for ABI.
void SetBGMSuppressed(bool suppress); // no-op
bool IsBGMSuppressed();               // always false
bool InstallBGMHook(uintptr_t efzBase);

// Get/set last BGM track for toggling/resume
unsigned short GetLastBgmTrack();
void SetLastBgmTrack(unsigned short track);
bool SetBGMVolumeViaGame(uintptr_t gameSystemPtr, int volumeLevel);
void StartBGMSuppressionPoller(); // no-op
void StopBGMSuppressionPoller();  // no-op