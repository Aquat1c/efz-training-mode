#pragma once

#include <cstdint>

namespace AudioControl {

bool InstallHooks(uintptr_t efzBase);
bool PlayBackgroundMusic(uintptr_t gameSystemPtr, unsigned short trackNumber);
void ApplyConfiguredVolumesNow();

int GetConfiguredBgmVolumePercent();
int GetConfiguredSeVolumePercent();
int GetConfiguredBgmDirectSoundVolume();
int GetConfiguredSeDirectSoundVolume();

} // namespace AudioControl