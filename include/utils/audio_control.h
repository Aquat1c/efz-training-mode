#pragma once

#include <cstdint>

namespace AudioControl {

bool InstallHooks(uintptr_t efzBase);
void ApplyConfiguredVolumesNow();

int GetConfiguredBgmVolumePercent();
int GetConfiguredSeVolumePercent();
int GetConfiguredBgmDirectSoundVolume();
int GetConfiguredSeDirectSoundVolume();

} // namespace AudioControl