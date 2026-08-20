#pragma once

#include <cstdint>

namespace AudioControl {

// Audio hook installation is split because EfzRevival installs its own hooks
// over EFZ's playSoundBuffer/setSoundVolume entrypoints during delayed startup.
// The common hooks do not overlap Revival and may be installed immediately;
// the contested pair must wait for positive final-host resolution. HostResolved
// may remain Deferred and is safe to poll if Revival can be injected later.
enum class HookInstallPhase {
    CommonOnly,
    HostResolved,
};

enum class HookInstallResult {
    Deferred,
    Ready,
    Suppressed,
    Failed,
};

HookInstallResult InstallHooks(uintptr_t efzBase, HookInstallPhase phase);
const char* HookInstallResultName(HookInstallResult result);
// Removes the Revival callback detours. A true pre-detach shutdown may also
// release the retained module reference; callers running under DllMain's
// loader lock must pass false and intentionally retain it until process exit.
void ShutdownHooks(bool releaseRevivalModuleReference);
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
