#include "../include/utils/audio_control.h"

#include "../include/utils/config.h"
#include "../include/core/constants.h"
#include "../include/core/logger.h"
#include "../include/core/memory.h"
#include "../include/utils/debug_log.h"
#include "../include/utils/utilities.h"
#include "../3rdparty/minhook/include/MinHook.h"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <string>

extern uintptr_t GetEFZBase();

namespace AudioControl {

namespace {

constexpr uintptr_t kPlayBackgroundMusicRva = 0x68B0;
// efz.c: playSoundBuffer is at 0x40DE80; 0x40DF50 is releaseSoundBufferAndMemory.
constexpr uintptr_t kPlaySoundBufferRva = 0xDE80;
constexpr uintptr_t kSetSoundVolumeRva = 0xE5D0;
constexpr uintptr_t kFadeWithSoundAdjustmentRva = 0x359C90;
constexpr uintptr_t kLoadSoundEffectsRva = 0x6780;
constexpr uintptr_t kLoadCharacterSoundsRva = 0x11AE0;

constexpr uintptr_t kSoundManagerOffset = 0x8;
constexpr uintptr_t kBgmBufferOffset = 0xF26;
constexpr uintptr_t kBgmTargetVolumeOffset = 0xF28;
constexpr uintptr_t kBgmCurrentVolumeOffset = 0xF2C;
constexpr uintptr_t kBgmAdjustRateOffset = 0xF30;
constexpr uintptr_t kCommonSeBufferTableOffset = 3748;
constexpr uintptr_t kCharacterSoundManagerOffset = 136;
constexpr uintptr_t kCharacterSeBufferTableOffset = 612;

constexpr int kMinDirectSoundVolume = -10000;
constexpr int kMaxDirectSoundVolume = 0;
constexpr int kDefaultSeDirectSoundVolume = -500;
constexpr int kCommonSeCount = 65;
constexpr int kCharacterSeCount = 50;
constexpr uint16_t kInvalidBufferIndex = 150;

using PlayBackgroundMusicFn = void(__thiscall*)(uintptr_t gameSystemPtr, unsigned short trackNumber);
using PlaySoundBufferFn = int(__thiscall*)(void* soundManagerPtr, unsigned short bufferIndex, int loopFlag);
using FadeWithSoundAdjustmentFn = int(__thiscall*)(void* screenEffectPtr,
                                                   int paletteId,
                                                   unsigned char fadeDirection,
                                                   int baseVolume,
                                                   int volumeAdjustment);
using LoadSoundEffectsFn = void(__thiscall*)(uintptr_t gameSystemPtr);
using LoadCharacterSoundsFn = void(__thiscall*)(uintptr_t characterPtr);
using SetSoundVolumeFn = int(__thiscall*)(void* soundManagerPtr, unsigned short bufferIndex, int volumeLevel);

PlayBackgroundMusicFn g_originalPlayBackgroundMusic = nullptr;
PlaySoundBufferFn g_originalPlaySoundBuffer = nullptr;
SetSoundVolumeFn g_originalSetSoundVolume = nullptr;
FadeWithSoundAdjustmentFn g_originalFadeWithSoundAdjustment = nullptr;
LoadSoundEffectsFn g_originalLoadSoundEffects = nullptr;
LoadCharacterSoundsFn g_originalLoadCharacterSounds = nullptr;
bool g_hooksInstalled = false;

void ApplyBgmVolumeToGameSystem(uintptr_t gameSystemPtr);
void ApplyCommonSeVolumeToGameSystem(uintptr_t gameSystemPtr);
void ApplyCharacterSeVolume(uintptr_t characterPtr);
void ApplyCharacterSeVolumesNow();
void ApplyVolumeBeforePlayback(void* soundManagerPtr, unsigned short bufferIndex);
int GetConfiguredBgmFadeBaseVolume(int baseDirectSoundVolume);

void TraceAudio(const std::string& message) {
    DebugLog::Write(message);
}

void TraceAudioBuffer(const char* label,
                      void* soundManagerPtr,
                      unsigned short bufferIndex,
                      int directSoundVolume,
                      uintptr_t gameSystemPtr = 0,
                      uintptr_t characterPtr = 0) {
    std::ostringstream oss;
    oss << "[AUDIO][TRACE] " << label
        << " soundManager=0x" << std::hex << reinterpret_cast<uintptr_t>(soundManagerPtr)
        << " buffer=" << std::dec << bufferIndex
        << " dsVolume=" << directSoundVolume;
    if (gameSystemPtr) {
        oss << " gameSystem=0x" << std::hex << gameSystemPtr;
    }
    if (characterPtr) {
        oss << " character=0x" << std::hex << characterPtr;
    }
    TraceAudio(oss.str());
}

bool SehSetSoundVolume(SetSoundVolumeFn fn, void* soundManagerPtr, unsigned short bufferIndex, int volumeLevel, int* outResult) {
    if (!fn || !outResult) {
        return false;
    }
    __try {
        *outResult = fn(soundManagerPtr, bufferIndex, volumeLevel);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool SehCallPlayBackgroundMusic(PlayBackgroundMusicFn fn, uintptr_t gameSystemPtr, unsigned short trackNumber) {
    if (!fn) {
        return false;
    }
    __try {
        fn(gameSystemPtr, trackNumber);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

int SehCallPlaySoundBuffer(PlaySoundBufferFn fn,
                           void* soundManagerPtr,
                           unsigned short bufferIndex,
                           int loopFlag,
                           bool* outOk) {
    if (outOk) {
        *outOk = false;
    }
    if (!fn) {
        return static_cast<int>(bufferIndex);
    }
    __try {
        const int result = fn(soundManagerPtr, bufferIndex, loopFlag);
        if (outOk) {
            *outOk = true;
        }
        return result;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return static_cast<int>(bufferIndex);
    }
}

int SehCallFadeWithSoundAdjustment(FadeWithSoundAdjustmentFn fn,
                                   void* screenEffectPtr,
                                   int paletteId,
                                   unsigned char fadeDirection,
                                   int baseVolume,
                                   int volumeAdjustment,
                                   bool* outOk) {
    if (outOk) {
        *outOk = false;
    }
    if (!fn) {
        return 0;
    }
    __try {
        const int result = fn(screenEffectPtr, paletteId, fadeDirection, baseVolume, volumeAdjustment);
        if (outOk) {
            *outOk = true;
        }
        return result;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

bool SehCallLoadSoundEffects(LoadSoundEffectsFn fn, uintptr_t gameSystemPtr) {
    if (!fn) {
        return false;
    }
    __try {
        fn(gameSystemPtr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool SehCallLoadCharacterSounds(LoadCharacterSoundsFn fn, uintptr_t characterPtr) {
    if (!fn) {
        return false;
    }
    __try {
        fn(characterPtr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool SehApplyBgmVolumeToGameSystem(uintptr_t gameSystemPtr) {
    __try {
        ApplyBgmVolumeToGameSystem(gameSystemPtr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool SehApplyCommonSeVolumeToGameSystem(uintptr_t gameSystemPtr) {
    __try {
        ApplyCommonSeVolumeToGameSystem(gameSystemPtr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool SehApplyCharacterSeVolume(uintptr_t characterPtr) {
    __try {
        ApplyCharacterSeVolume(characterPtr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool SehApplyCharacterSeVolumesNow() {
    __try {
        ApplyCharacterSeVolumesNow();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool SehApplyVolumeBeforePlayback(void* soundManagerPtr, unsigned short bufferIndex) {
    __try {
        ApplyVolumeBeforePlayback(soundManagerPtr, bufferIndex);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

int ClampPercent(int value) {
    return std::clamp(value, 0, 100);
}

int PercentToDirectSoundVolume(int percent, int baseDirectSoundVolume) {
    if (percent <= 0) {
        return kMinDirectSoundVolume;
    }

    const double baseAmplitude = std::pow(10.0, static_cast<double>(baseDirectSoundVolume) / 2000.0);
    const double scaledAmplitude = baseAmplitude * (static_cast<double>(percent) / 100.0);
    if (scaledAmplitude <= 0.0) {
        return kMinDirectSoundVolume;
    }

    const double directSoundVolume = 2000.0 * std::log10(scaledAmplitude);
    return std::clamp(static_cast<int>(std::lround(directSoundVolume)),
                      kMinDirectSoundVolume,
                      kMaxDirectSoundVolume);
}

int GetConfiguredBgmFadeBaseVolume(int baseDirectSoundVolume) {
    return PercentToDirectSoundVolume(GetConfiguredBgmVolumePercent(), baseDirectSoundVolume);
}

SetSoundVolumeFn ResolveSetSoundVolume() {
    if (g_originalSetSoundVolume) {
        return g_originalSetSoundVolume;
    }

    const uintptr_t efzBase = GetEFZBase();
    if (!efzBase) {
        return nullptr;
    }
    return reinterpret_cast<SetSoundVolumeFn>(efzBase + kSetSoundVolumeRva);
}

bool ReadPointer(uintptr_t address, void*& outPtr) {
    outPtr = nullptr;
    return SafeReadMemory(address, &outPtr, sizeof(outPtr)) && outPtr != nullptr;
}

bool SetBufferVolume(void* soundManagerPtr, unsigned short bufferIndex, int directSoundVolume) {
    if (!soundManagerPtr || bufferIndex == kInvalidBufferIndex) {
        return false;
    }

    SetSoundVolumeFn setSoundVolume = ResolveSetSoundVolume();
    if (!setSoundVolume) {
        return false;
    }

    int result = -1;
    if (!SehSetSoundVolume(setSoundVolume, soundManagerPtr, bufferIndex, directSoundVolume, &result)) {
        LogOut("[AUDIO][SEH] Exception while calling setSoundVolume", true);
        TraceAudioBuffer("setSoundVolume exception", soundManagerPtr, bufferIndex, directSoundVolume);
        return false;
    }
    return result == 0;
}

void ApplyBgmVolumeToGameSystem(uintptr_t gameSystemPtr) {
    if (!gameSystemPtr) {
        TraceAudio("[AUDIO][TRACE] ApplyBgmVolumeToGameSystem skipped because gameSystemPtr was null");
        return;
    }

    void* soundManagerPtr = nullptr;
    if (!ReadPointer(gameSystemPtr + kSoundManagerOffset, soundManagerPtr)) {
        TraceAudio("[AUDIO][TRACE] ApplyBgmVolumeToGameSystem could not resolve sound manager");
        return;
    }

    uint16_t bufferIndex = kInvalidBufferIndex;
    if (!SafeReadMemory(gameSystemPtr + kBgmBufferOffset, &bufferIndex, sizeof(bufferIndex)) || bufferIndex == kInvalidBufferIndex) {
        TraceAudio("[AUDIO][TRACE] ApplyBgmVolumeToGameSystem found no active BGM buffer");
        return;
    }

    const int directSoundVolume = GetConfiguredBgmDirectSoundVolume();
    if (!SetBufferVolume(soundManagerPtr, bufferIndex, directSoundVolume)) {
        TraceAudioBuffer("ApplyBgmVolumeToGameSystem failed", soundManagerPtr, bufferIndex, directSoundVolume, gameSystemPtr);
        return;
    }

    const uint16_t zeroAdjustRate = 0;
    SafeWriteMemory(gameSystemPtr + kBgmTargetVolumeOffset, &directSoundVolume, sizeof(directSoundVolume));
    SafeWriteMemory(gameSystemPtr + kBgmCurrentVolumeOffset, &directSoundVolume, sizeof(directSoundVolume));
    SafeWriteMemory(gameSystemPtr + kBgmAdjustRateOffset, &zeroAdjustRate, sizeof(zeroAdjustRate));
    TraceAudioBuffer("ApplyBgmVolumeToGameSystem applied", soundManagerPtr, bufferIndex, directSoundVolume, gameSystemPtr);
}

void ApplyCommonSeVolumeToGameSystem(uintptr_t gameSystemPtr) {
    if (!gameSystemPtr) {
        TraceAudio("[AUDIO][TRACE] ApplyCommonSeVolumeToGameSystem skipped because gameSystemPtr was null");
        return;
    }

    void* soundManagerPtr = nullptr;
    if (!ReadPointer(gameSystemPtr + kSoundManagerOffset, soundManagerPtr)) {
        TraceAudio("[AUDIO][TRACE] ApplyCommonSeVolumeToGameSystem could not resolve sound manager");
        return;
    }

    const int directSoundVolume = GetConfiguredSeDirectSoundVolume();
    int appliedCount = 0;
    for (int soundIndex = 0; soundIndex < kCommonSeCount; ++soundIndex) {
        uint16_t bufferIndex = kInvalidBufferIndex;
        if (!SafeReadMemory(gameSystemPtr + kCommonSeBufferTableOffset + sizeof(uint16_t) * soundIndex,
                            &bufferIndex,
                            sizeof(bufferIndex))) {
            continue;
        }
        if (SetBufferVolume(soundManagerPtr, bufferIndex, directSoundVolume)) {
            ++appliedCount;
        }
    }
    std::ostringstream oss;
    oss << "[AUDIO][TRACE] ApplyCommonSeVolumeToGameSystem applied=" << appliedCount
        << " dsVolume=" << directSoundVolume
        << " gameSystem=0x" << std::hex << gameSystemPtr;
    TraceAudio(oss.str());
}

void ApplyCharacterSeVolume(uintptr_t characterPtr) {
    if (!characterPtr) {
        TraceAudio("[AUDIO][TRACE] ApplyCharacterSeVolume skipped because characterPtr was null");
        return;
    }

    void* soundManagerPtr = nullptr;
    if (!ReadPointer(characterPtr + kCharacterSoundManagerOffset, soundManagerPtr)) {
        TraceAudio("[AUDIO][TRACE] ApplyCharacterSeVolume could not resolve character sound manager");
        return;
    }

    const int directSoundVolume = GetConfiguredSeDirectSoundVolume();
    int appliedCount = 0;
    for (int soundIndex = 0; soundIndex < kCharacterSeCount; ++soundIndex) {
        uint16_t bufferIndex = kInvalidBufferIndex;
        if (!SafeReadMemory(characterPtr + kCharacterSeBufferTableOffset + sizeof(uint16_t) * soundIndex,
                            &bufferIndex,
                            sizeof(bufferIndex))) {
            continue;
        }
        if (SetBufferVolume(soundManagerPtr, bufferIndex, directSoundVolume)) {
            ++appliedCount;
        }
    }
    std::ostringstream oss;
    oss << "[AUDIO][TRACE] ApplyCharacterSeVolume applied=" << appliedCount
        << " dsVolume=" << directSoundVolume
        << " character=0x" << std::hex << characterPtr;
    TraceAudio(oss.str());
}

void ApplyCharacterSeVolumesNow() {
    const uintptr_t efzBase = GetEFZBase();
    if (!efzBase) {
        return;
    }

    uintptr_t characterPtr = 0;
    if (SafeReadMemory(efzBase + EFZ_BASE_OFFSET_P1, &characterPtr, sizeof(characterPtr)) && characterPtr) {
        ApplyCharacterSeVolume(characterPtr);
    }

    characterPtr = 0;
    if (SafeReadMemory(efzBase + EFZ_BASE_OFFSET_P2, &characterPtr, sizeof(characterPtr)) && characterPtr) {
        ApplyCharacterSeVolume(characterPtr);
    }
}

bool ResolveCurrentGameSystem(uintptr_t& outGameSystemPtr) {
    outGameSystemPtr = GetGameStatePtr();
    if (!outGameSystemPtr) {
        TraceAudio("[AUDIO][TRACE] ResolveCurrentGameSystem returned null");
    }
    return outGameSystemPtr != 0;
}

bool IsCurrentBgmBuffer(void* soundManagerPtr, unsigned short bufferIndex, uintptr_t& outGameSystemPtr) {
    outGameSystemPtr = 0;
    if (!soundManagerPtr || bufferIndex == kInvalidBufferIndex) {
        return false;
    }

    if (!ResolveCurrentGameSystem(outGameSystemPtr)) {
        return false;
    }

    void* currentSoundManagerPtr = nullptr;
    if (!ReadPointer(outGameSystemPtr + kSoundManagerOffset, currentSoundManagerPtr)) {
        return false;
    }
    if (currentSoundManagerPtr != soundManagerPtr) {
        return false;
    }

    uint16_t currentBgmBuffer = kInvalidBufferIndex;
    if (!SafeReadMemory(outGameSystemPtr + kBgmBufferOffset, &currentBgmBuffer, sizeof(currentBgmBuffer))) {
        return false;
    }

    return currentBgmBuffer == bufferIndex;
}

void ApplyVolumeBeforePlayback(void* soundManagerPtr, unsigned short bufferIndex) {
    uintptr_t gameSystemPtr = 0;
    if (IsCurrentBgmBuffer(soundManagerPtr, bufferIndex, gameSystemPtr)) {
        const int directSoundVolume = GetConfiguredBgmDirectSoundVolume();
        if (SetBufferVolume(soundManagerPtr, bufferIndex, directSoundVolume)) {
            const uint16_t zeroAdjustRate = 0;
            SafeWriteMemory(gameSystemPtr + kBgmTargetVolumeOffset, &directSoundVolume, sizeof(directSoundVolume));
            SafeWriteMemory(gameSystemPtr + kBgmCurrentVolumeOffset, &directSoundVolume, sizeof(directSoundVolume));
            SafeWriteMemory(gameSystemPtr + kBgmAdjustRateOffset, &zeroAdjustRate, sizeof(zeroAdjustRate));
            TraceAudioBuffer("ApplyVolumeBeforePlayback classified BGM", soundManagerPtr, bufferIndex, directSoundVolume, gameSystemPtr);
        }
        return;
    }
}

template <typename T>
bool InstallHook(const char* name, void* target, void* detour, T* original) {
    MH_STATUS status = MH_CreateHook(target, detour, reinterpret_cast<void**>(original));
    if (status != MH_OK && status != MH_ERROR_ALREADY_CREATED) {
        LogOut(std::string("[AUDIO] failed to create ") + name + " hook status=" + std::to_string(static_cast<int>(status)), true);
        return false;
    }

    status = MH_EnableHook(target);
    if (status != MH_OK && status != MH_ERROR_ENABLED) {
        LogOut(std::string("[AUDIO] failed to enable ") + name + " hook status=" + std::to_string(static_cast<int>(status)), true);
        return false;
    }

    return true;
}

void __fastcall HookedPlayBackgroundMusic(uintptr_t gameSystemPtr, void*, unsigned short trackNumber) {
    {
        std::ostringstream oss;
        oss << "[AUDIO][TRACE] HookedPlayBackgroundMusic track=" << trackNumber
            << " gameSystem=0x" << std::hex << gameSystemPtr;
        TraceAudio(oss.str());
    }

    if (g_originalPlayBackgroundMusic && !SehCallPlayBackgroundMusic(g_originalPlayBackgroundMusic, gameSystemPtr, trackNumber)) {
        LogOut("[AUDIO][SEH] Exception in original playBackgroundMusic call", true);
        return;
    }
    if (!SehApplyBgmVolumeToGameSystem(gameSystemPtr)) {
        LogOut("[AUDIO][SEH] Exception while applying BGM volume after playBackgroundMusic", true);
    }
}

int __fastcall HookedPlaySoundBuffer(void* soundManagerPtr, void*, unsigned short bufferIndex, int loopFlag) {
    {
        std::ostringstream oss;
        oss << "[AUDIO][TRACE] HookedPlaySoundBuffer buffer=" << bufferIndex
            << " loop=" << loopFlag
            << " soundManager=0x" << std::hex << reinterpret_cast<uintptr_t>(soundManagerPtr);
        TraceAudio(oss.str());
    }

    if (!SehApplyVolumeBeforePlayback(soundManagerPtr, bufferIndex)) {
        LogOut("[AUDIO][SEH] Exception while applying pre-playback volume", true);
    }

    bool originalOk = false;
    const int result = SehCallPlaySoundBuffer(g_originalPlaySoundBuffer, soundManagerPtr, bufferIndex, loopFlag, &originalOk);
    if (!originalOk) {
        LogOut("[AUDIO][SEH] Exception in original playSoundBuffer call", true);
    }
    return result;
}

int __fastcall HookedSetSoundVolume(void* soundManagerPtr, void*, unsigned short bufferIndex, int volumeLevel) {
    int adjustedVolumeLevel = volumeLevel;
    uintptr_t gameSystemPtr = 0;
    if (IsCurrentBgmBuffer(soundManagerPtr, bufferIndex, gameSystemPtr)) {
        adjustedVolumeLevel = PercentToDirectSoundVolume(GetConfiguredBgmVolumePercent(), volumeLevel);
    }

    SetSoundVolumeFn setSoundVolume = ResolveSetSoundVolume();
    int result = -1;
    if (!SehSetSoundVolume(setSoundVolume, soundManagerPtr, bufferIndex, adjustedVolumeLevel, &result)) {
        LogOut("[AUDIO][SEH] Exception in original setSoundVolume call", true);
        return -1;
    }
    return result;
}

int __fastcall HookedFadeWithSoundAdjustment(void* screenEffectPtr,
                                            void*,
                                            int paletteId,
                                            unsigned char fadeDirection,
                                            int baseVolume,
                                            int volumeAdjustment) {
    {
        std::ostringstream oss;
        oss << "[AUDIO][TRACE] HookedFadeWithSoundAdjustment dir=" << static_cast<int>(fadeDirection)
            << " palette=0x" << std::hex << paletteId
            << " baseVolume=" << std::dec << baseVolume
            << " volumeAdjustment=" << volumeAdjustment
            << " bgmPercent=" << GetConfiguredBgmVolumePercent()
            << " screenEffect=0x" << std::hex << reinterpret_cast<uintptr_t>(screenEffectPtr);
        TraceAudio(oss.str());
    }

    bool originalOk = false;
    const int result = SehCallFadeWithSoundAdjustment(g_originalFadeWithSoundAdjustment,
                                                      screenEffectPtr,
                                                      paletteId,
                                                      fadeDirection,
                                                      baseVolume,
                                                      volumeAdjustment,
                                                      &originalOk);
    if (!originalOk) {
        LogOut("[AUDIO][SEH] Exception in original fadeWithSoundAdjustment call", true);
    }
    return result;
}

void __fastcall HookedLoadSoundEffects(uintptr_t gameSystemPtr, void*) {
    {
        std::ostringstream oss;
        oss << "[AUDIO][TRACE] HookedLoadSoundEffects gameSystem=0x" << std::hex << gameSystemPtr;
        TraceAudio(oss.str());
    }
    if (g_originalLoadSoundEffects && !SehCallLoadSoundEffects(g_originalLoadSoundEffects, gameSystemPtr)) {
        LogOut("[AUDIO][SEH] Exception in original loadSoundEffects call", true);
        return;
    }
    if (!SehApplyCommonSeVolumeToGameSystem(gameSystemPtr)) {
        LogOut("[AUDIO][SEH] Exception while applying common SE volume", true);
    }
}

void __fastcall HookedLoadCharacterSounds(uintptr_t characterPtr, void*) {
    {
        std::ostringstream oss;
        oss << "[AUDIO][TRACE] HookedLoadCharacterSounds character=0x" << std::hex << characterPtr;
        TraceAudio(oss.str());
    }
    if (g_originalLoadCharacterSounds && !SehCallLoadCharacterSounds(g_originalLoadCharacterSounds, characterPtr)) {
        LogOut("[AUDIO][SEH] Exception in original loadCharacterSounds call", true);
        return;
    }
    if (!SehApplyCharacterSeVolume(characterPtr)) {
        LogOut("[AUDIO][SEH] Exception while applying character SE volume", true);
    }
}

} // namespace

bool InstallHooks(uintptr_t efzBase) {
    if (g_hooksInstalled) {
        TraceAudio("[AUDIO][TRACE] InstallHooks skipped because hooks are already installed");
        return true;
    }
    if (!efzBase) {
        TraceAudio("[AUDIO][TRACE] InstallHooks failed because efzBase was null");
        return false;
    }

    bool ok = true;
    ok = InstallHook("playBackgroundMusic",
                     reinterpret_cast<void*>(efzBase + kPlayBackgroundMusicRva),
                     reinterpret_cast<void*>(&HookedPlayBackgroundMusic),
                     &g_originalPlayBackgroundMusic) && ok;
    ok = InstallHook("playSoundBuffer",
                     reinterpret_cast<void*>(efzBase + kPlaySoundBufferRva),
                     reinterpret_cast<void*>(&HookedPlaySoundBuffer),
                     &g_originalPlaySoundBuffer) && ok;
    ok = InstallHook("setSoundVolume",
                     reinterpret_cast<void*>(efzBase + kSetSoundVolumeRva),
                     reinterpret_cast<void*>(&HookedSetSoundVolume),
                     &g_originalSetSoundVolume) && ok;
    ok = InstallHook("fadeWithSoundAdjustment",
                     reinterpret_cast<void*>(efzBase + kFadeWithSoundAdjustmentRva),
                     reinterpret_cast<void*>(&HookedFadeWithSoundAdjustment),
                     &g_originalFadeWithSoundAdjustment) && ok;
    ok = InstallHook("loadSoundEffects",
                     reinterpret_cast<void*>(efzBase + kLoadSoundEffectsRva),
                     reinterpret_cast<void*>(&HookedLoadSoundEffects),
                     &g_originalLoadSoundEffects) && ok;
    ok = InstallHook("loadCharacterSounds",
                     reinterpret_cast<void*>(efzBase + kLoadCharacterSoundsRva),
                     reinterpret_cast<void*>(&HookedLoadCharacterSounds),
                     &g_originalLoadCharacterSounds) && ok;

    if (ok) {
        g_hooksInstalled = true;
        LogOut("[AUDIO] runtime audio hooks installed", true);
    } else {
        LogOut("[AUDIO] runtime audio hook install incomplete", true);
    }
    return ok;
}

bool PlayBackgroundMusic(uintptr_t gameSystemPtr, unsigned short trackNumber) {
    if (!gameSystemPtr) {
        TraceAudio("[AUDIO][TRACE] PlayBackgroundMusic skipped because gameSystemPtr was null");
        return false;
    }

    PlayBackgroundMusicFn playBackgroundMusic = g_originalPlayBackgroundMusic;
    if (!playBackgroundMusic) {
        const uintptr_t efzBase = GetEFZBase();
        if (!efzBase) {
            LogOut("[AUDIO] playBackgroundMusic unavailable because EFZ base was not resolved", true);
            return false;
        }
        playBackgroundMusic = reinterpret_cast<PlayBackgroundMusicFn>(efzBase + kPlayBackgroundMusicRva);
    }

    {
        std::ostringstream oss;
        oss << "[AUDIO][TRACE] PlayBackgroundMusic request track=" << trackNumber
            << " gameSystem=0x" << std::hex << gameSystemPtr;
        TraceAudio(oss.str());
    }

    if (!SehCallPlayBackgroundMusic(playBackgroundMusic, gameSystemPtr, trackNumber)) {
        LogOut("[AUDIO][SEH] Exception in requested playBackgroundMusic call", true);
        return false;
    }
    if (!SehApplyBgmVolumeToGameSystem(gameSystemPtr)) {
        LogOut("[AUDIO][SEH] Exception while applying BGM volume after requested playBackgroundMusic", true);
    }
    return true;
}

void ApplyConfiguredVolumesNow() {
    TraceAudio("[AUDIO][TRACE] ApplyConfiguredVolumesNow begin");
    const uintptr_t gameSystemPtr = GetGameStatePtr();
    if (gameSystemPtr) {
        if (!SehApplyBgmVolumeToGameSystem(gameSystemPtr)) {
            LogOut("[AUDIO][SEH] Exception while applying configured BGM volume", true);
        }
        if (!SehApplyCommonSeVolumeToGameSystem(gameSystemPtr)) {
            LogOut("[AUDIO][SEH] Exception while applying configured common SE volume", true);
        }
    } else {
        TraceAudio("[AUDIO][TRACE] ApplyConfiguredVolumesNow could not resolve current game system");
    }
    if (!SehApplyCharacterSeVolumesNow()) {
        LogOut("[AUDIO][SEH] Exception while applying configured character SE volumes", true);
    }
    TraceAudio("[AUDIO][TRACE] ApplyConfiguredVolumesNow end");
}

int GetConfiguredBgmVolumePercent() {
    return ClampPercent(Config::GetSettings().bgmVolumePercent);
}

int GetConfiguredSeVolumePercent() {
    return ClampPercent(Config::GetSettings().seVolumePercent);
}

int GetConfiguredBgmDirectSoundVolume() {
    return PercentToDirectSoundVolume(GetConfiguredBgmVolumePercent(), 0);
}

int GetConfiguredSeDirectSoundVolume() {
    return PercentToDirectSoundVolume(GetConfiguredSeVolumePercent(), kDefaultSeDirectSoundVolume);
}

} // namespace AudioControl