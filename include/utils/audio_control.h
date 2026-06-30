#pragma once

#include <cstdint>

namespace AudioControl {

bool InstallHooks(uintptr_t efzBase);
bool PlayBackgroundMusic(uintptr_t gameSystemPtr, unsigned short trackNumber);
void ApplyConfiguredVolumesNow();
void SetVolumeApplicationReady(bool ready);
bool IsVolumeApplicationReady();
bool IsGameSoundSystemReady(uintptr_t gameSystemPtr = 0);
bool IsCommonSoundEffectReady(uintptr_t gameSystemPtr, unsigned short soundIndex);
bool EnableVolumeApplicationIfSoundReady(uintptr_t gameSystemPtr = 0, const char* reason = nullptr);

int GetConfiguredBgmVolumePercent();
int GetConfiguredSeVolumePercent();
int GetConfiguredBgmDirectSoundVolume();
int GetConfiguredSeDirectSoundVolume();

} // namespace AudioControl
