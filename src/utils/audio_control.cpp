#include "../include/utils/audio_control.h"

#include "../include/utils/config.h"
#include "../include/utils/extended_config_bridge.h"
#include "../include/core/constants.h"
#include "../include/core/logger.h"
#include "../include/core/memory.h"
#include "../include/utils/debug_log.h"
#include "../include/utils/utilities.h"
#include "../3rdparty/minhook/include/MinHook.h"

#include <algorithm>
#include <atomic>
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
constexpr uintptr_t kDirectSoundObjectOffset = 0xC;
constexpr uintptr_t kBgmBufferOffset = 0xF26;
constexpr uintptr_t kBgmTargetVolumeOffset = 0xF28;
constexpr uintptr_t kBgmCurrentVolumeOffset = 0xF2C;
constexpr uintptr_t kBgmAdjustRateOffset = 0xF30;
constexpr uintptr_t kCommonSeBufferTableOffset = 3748;
constexpr uintptr_t kSoundManagerBufferTableOffset = 1216;
constexpr uintptr_t kCharacterSoundManagerOffset = 136;
constexpr uintptr_t kCharacterSeBufferTableOffset = 612;

constexpr int kMinDirectSoundVolume = -10000;
constexpr int kMaxDirectSoundVolume = 0;
constexpr int kDefaultSeDirectSoundVolume = -500;
constexpr int kCommonSeCount = 65;
constexpr int kCharacterSeCount = 50;
constexpr int kSoundManagerBufferCount = 150;
constexpr uint16_t kInvalidBufferIndex = 150;

constexpr uintptr_t kDirectSoundSetCooperativeLevelVtableOffset = 24;
constexpr uintptr_t kDsBufferPlayVtableOffset = 48;
constexpr uintptr_t kDsBufferSetCurrentPositionVtableOffset = 52;
constexpr uintptr_t kDsBufferSetVolumeVtableOffset = 60;
constexpr uintptr_t kDsBufferStopVtableOffset = 72;

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
using DirectSoundBufferSetVolumeFn = int(__stdcall*)(void* soundBufferPtr, int volumeLevel);

PlayBackgroundMusicFn g_originalPlayBackgroundMusic = nullptr;
PlaySoundBufferFn g_originalPlaySoundBuffer = nullptr;
SetSoundVolumeFn g_originalSetSoundVolume = nullptr;
FadeWithSoundAdjustmentFn g_originalFadeWithSoundAdjustment = nullptr;
LoadSoundEffectsFn g_originalLoadSoundEffects = nullptr;
LoadCharacterSoundsFn g_originalLoadCharacterSounds = nullptr;
bool g_hooksInstalled = false;
bool g_revivalAudioChainInstalled = false;
uintptr_t g_revivalPlaySoundBufferTarget = 0;
uintptr_t g_revivalSetSoundVolumeTarget = 0;
std::atomic<bool> g_volumeApplicationReady{false};
std::atomic<bool> g_runtimeAudioControlSuppressed{false};
std::atomic<bool> g_runtimeAudioSuppressLogged{false};
std::atomic<bool> g_revivalAudioChainDeferredLogged{false};
std::atomic<bool> g_audioHookIncompleteLogged{false};

void ApplyBgmVolumeToGameSystem(uintptr_t gameSystemPtr);
void ApplyCommonSeVolumeToGameSystem(uintptr_t gameSystemPtr);
void ApplyCharacterSeVolume(uintptr_t characterPtr);
void ApplyCharacterSeVolumesNow();
void ApplyVolumeBeforePlayback(void* soundManagerPtr, unsigned short bufferIndex);
int GetConfiguredBgmFadeBaseVolume(int baseDirectSoundVolume);
int AdjustVolumeForBuffer(void* soundManagerPtr, unsigned short bufferIndex, int volumeLevel);
void __stdcall PreRevivalPlaySoundBuffer(void* soundManagerPtr, unsigned short bufferIndex, int loopFlag);
int __stdcall AdjustRevivalSetSoundVolume(void* soundManagerPtr, unsigned short bufferIndex, int volumeLevel);
bool IsExecutableAddress(uintptr_t address);
bool IsCurrentBgmBuffer(void* soundManagerPtr, unsigned short bufferIndex, uintptr_t& outGameSystemPtr);
void __fastcall HookedPlayBackgroundMusic(uintptr_t gameSystemPtr, void*, unsigned short trackNumber);
int __fastcall HookedPlaySoundBuffer(void* soundManagerPtr, void*, unsigned short bufferIndex, int loopFlag);
int __fastcall HookedSetSoundVolume(void* soundManagerPtr, void*, unsigned short bufferIndex, int volumeLevel);
int __fastcall HookedFadeWithSoundAdjustment(void* screenEffectPtr,
                                             void*,
                                             int paletteId,
                                             unsigned char fadeDirection,
                                             int baseVolume,
                                             int volumeAdjustment);
void __fastcall HookedLoadSoundEffects(uintptr_t gameSystemPtr, void*);
void __fastcall HookedLoadCharacterSounds(uintptr_t characterPtr, void*);
#if defined(_M_IX86)
void HookedRevivalPlaySoundBufferBridge();
void HookedRevivalSetSoundVolumeBridge();
#endif

void TraceAudio(const std::string& message) {
    DebugLog::Write(message);
}

bool IsEfzRevivalModuleLoaded() {
    return GetModuleHandleA("EfzRevival.dll") != nullptr;
}

bool EntryPointAlreadyJmpPatched(uintptr_t address) {
    uint8_t opcode = 0;
    return SafeReadMemory(address, &opcode, sizeof(opcode)) && opcode == 0xE9;
}

bool ReadRelativeJumpTarget(uintptr_t address, uintptr_t& outTarget) {
    outTarget = 0;

    uint8_t opcode = 0;
    int32_t rel32 = 0;
    if (!SafeReadMemory(address, &opcode, sizeof(opcode)) || opcode != 0xE9) {
        return false;
    }
    if (!SafeReadMemory(address + 1, &rel32, sizeof(rel32))) {
        return false;
    }

    outTarget = address + 5 + static_cast<intptr_t>(rel32);
    return outTarget != 0 && IsExecutableAddress(outTarget);
}

bool EfzAudioEntryPointsAlreadyOwned(uintptr_t efzBase) {
    if (!efzBase) {
        return false;
    }

    return EntryPointAlreadyJmpPatched(efzBase + kPlaySoundBufferRva)
        || EntryPointAlreadyJmpPatched(efzBase + kSetSoundVolumeRva);
}

void LogRuntimeAudioSuppressedOnce(const char* reason) {
    if (g_runtimeAudioSuppressLogged.exchange(true, std::memory_order_acq_rel)) {
        return;
    }

    std::string message = "[AUDIO] runtime EFZ audio detours disabled";
    if (reason && *reason) {
        message += " (";
        message += reason;
        message += ")";
    }
    message += "; leaving audio hook ownership to Revival/external patch";
    LogOut(message, true);
}

bool RuntimeAudioControlSuppressed(const char* reason = nullptr) {
    if (g_runtimeAudioControlSuppressed.load(std::memory_order_acquire)) {
        LogRuntimeAudioSuppressedOnce(reason);
        return true;
    }
    return false;
}

bool RuntimeVolumeApplicationReady() {
    return g_volumeApplicationReady.load(std::memory_order_acquire);
}

bool IsReadableProtection(DWORD protection) {
    if (protection & (PAGE_GUARD | PAGE_NOACCESS)) {
        return false;
    }

    switch (protection & 0xFF) {
        case PAGE_READONLY:
        case PAGE_READWRITE:
        case PAGE_WRITECOPY:
        case PAGE_EXECUTE_READ:
        case PAGE_EXECUTE_READWRITE:
        case PAGE_EXECUTE_WRITECOPY:
            return true;
        default:
            return false;
    }
}

bool IsExecutableProtection(DWORD protection) {
    if (protection & (PAGE_GUARD | PAGE_NOACCESS)) {
        return false;
    }

    switch (protection & 0xFF) {
        case PAGE_EXECUTE:
        case PAGE_EXECUTE_READ:
        case PAGE_EXECUTE_READWRITE:
        case PAGE_EXECUTE_WRITECOPY:
            return true;
        default:
            return false;
    }
}

bool IsReadableRange(uintptr_t address, size_t size) {
    if (!address || size == 0) {
        return false;
    }

    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(reinterpret_cast<LPCVOID>(address), &mbi, sizeof(mbi)) == 0) {
        return false;
    }
    if (mbi.State != MEM_COMMIT || !IsReadableProtection(mbi.Protect)) {
        return false;
    }

    const uintptr_t regionBase = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
    const uintptr_t regionEnd = regionBase + static_cast<uintptr_t>(mbi.RegionSize);
    const uintptr_t end = address + size;
    return end >= address && address >= regionBase && end <= regionEnd;
}

bool IsExecutableAddress(uintptr_t address) {
    if (!address) {
        return false;
    }

    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(reinterpret_cast<LPCVOID>(address), &mbi, sizeof(mbi)) == 0) {
        return false;
    }
    return mbi.State == MEM_COMMIT && IsExecutableProtection(mbi.Protect);
}

template <typename T>
bool ReadChecked(uintptr_t address, T& out) {
    out = T{};
    return IsReadableRange(address, sizeof(T))
        && SafeReadMemory(address, &out, sizeof(T));
}

bool IsComObjectReady(uintptr_t objectPtr, const uintptr_t* requiredVtableOffsets, size_t requiredCount) {
    if (!objectPtr || !requiredVtableOffsets || requiredCount == 0) {
        return false;
    }

    uintptr_t vtable = 0;
    if (!ReadChecked(objectPtr, vtable) || !vtable) {
        return false;
    }

    for (size_t i = 0; i < requiredCount; ++i) {
        uintptr_t method = 0;
        if (!ReadChecked(vtable + requiredVtableOffsets[i], method) || !IsExecutableAddress(method)) {
            return false;
        }
    }

    return true;
}

bool IsSoundManagerDirectSoundReady(uintptr_t soundManagerPtr) {
    if (!soundManagerPtr) {
        return false;
    }

    uintptr_t directSoundObject = 0;
    if (!ReadChecked(soundManagerPtr + kDirectSoundObjectOffset, directSoundObject) || !directSoundObject) {
        return false;
    }

    const uintptr_t requiredDirectSoundMethods[] = {
        kDirectSoundSetCooperativeLevelVtableOffset,
    };
    return IsComObjectReady(directSoundObject,
                            requiredDirectSoundMethods,
                            sizeof(requiredDirectSoundMethods) / sizeof(requiredDirectSoundMethods[0]));
}

bool IsSoundBufferReadyForOps(uintptr_t soundManagerPtr,
                              unsigned short bufferIndex,
                              bool requirePlayback,
                              bool requireVolume) {
    if (!soundManagerPtr || bufferIndex >= kSoundManagerBufferCount) {
        return false;
    }

    uintptr_t soundBuffer = 0;
    if (!ReadChecked(soundManagerPtr + kSoundManagerBufferTableOffset + sizeof(uintptr_t) * bufferIndex, soundBuffer)
        || !soundBuffer) {
        return false;
    }

    uintptr_t requiredMethods[4] = {};
    size_t requiredCount = 0;
    if (requirePlayback) {
        requiredMethods[requiredCount++] = kDsBufferPlayVtableOffset;
        requiredMethods[requiredCount++] = kDsBufferSetCurrentPositionVtableOffset;
        requiredMethods[requiredCount++] = kDsBufferStopVtableOffset;
    }
    if (requireVolume) {
        requiredMethods[requiredCount++] = kDsBufferSetVolumeVtableOffset;
    }

    return requiredCount > 0 && IsComObjectReady(soundBuffer, requiredMethods, requiredCount);
}

bool CommonSoundEffectReady(uintptr_t gameSystemPtr, unsigned short soundIndex) {
    if (!gameSystemPtr || soundIndex >= kCommonSeCount) {
        return false;
    }

    uintptr_t soundManagerPtr = 0;
    if (!ReadChecked(gameSystemPtr + kSoundManagerOffset, soundManagerPtr)
        || !IsSoundManagerDirectSoundReady(soundManagerPtr)) {
        return false;
    }

    uint16_t bufferIndex = kInvalidBufferIndex;
    if (!ReadChecked(gameSystemPtr + kCommonSeBufferTableOffset + sizeof(uint16_t) * soundIndex, bufferIndex)) {
        return false;
    }

    return IsSoundBufferReadyForOps(soundManagerPtr, bufferIndex, true, true);
}

bool GameSoundSystemReady(uintptr_t gameSystemPtr) {
    if (!gameSystemPtr) {
        gameSystemPtr = GetGameStatePtr();
    }
    if (!gameSystemPtr) {
        return false;
    }

    uintptr_t soundManagerPtr = 0;
    if (!ReadChecked(gameSystemPtr + kSoundManagerOffset, soundManagerPtr)
        || !IsSoundManagerDirectSoundReady(soundManagerPtr)) {
        return false;
    }

    return true;
}

bool TryEnableVolumeApplication(uintptr_t gameSystemPtr, const char* reason) {
    if (RuntimeAudioControlSuppressed(reason ? reason : "volume application requested")) {
        return false;
    }
    if (RuntimeVolumeApplicationReady()) {
        return true;
    }
    if (!GameSoundSystemReady(gameSystemPtr)) {
        return false;
    }

    const bool previous = g_volumeApplicationReady.exchange(true, std::memory_order_acq_rel);
    if (!previous) {
        if (reason && *reason) {
            LogOut(std::string("[AUDIO] runtime volume application enabled after ") + reason, true);
        } else {
            LogOut("[AUDIO] runtime volume application enabled", true);
        }
    }
    return true;
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
    if (RuntimeAudioControlSuppressed("setSoundVolume resolve requested")) {
        return nullptr;
    }
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
    uintptr_t ptr = 0;
    if (!ReadChecked(address, ptr) || !ptr) {
        return false;
    }
    outPtr = reinterpret_cast<void*>(ptr);
    return true;
}

bool ReadSoundBufferPointer(void* soundManagerPtr, unsigned short bufferIndex, void*& outBufferPtr) {
    outBufferPtr = nullptr;
    if (!soundManagerPtr || bufferIndex >= kSoundManagerBufferCount) {
        return false;
    }

    uintptr_t bufferPtr = 0;
    const uintptr_t tableEntry = reinterpret_cast<uintptr_t>(soundManagerPtr)
        + kSoundManagerBufferTableOffset
        + sizeof(uintptr_t) * bufferIndex;
    if (!ReadChecked(tableEntry, bufferPtr) || !bufferPtr) {
        return false;
    }

    outBufferPtr = reinterpret_cast<void*>(bufferPtr);
    return true;
}

bool SetBufferVolume(void* soundManagerPtr, unsigned short bufferIndex, int directSoundVolume) {
    if (RuntimeAudioControlSuppressed("buffer volume write requested")) {
        return false;
    }
    if (!soundManagerPtr || bufferIndex >= kSoundManagerBufferCount) {
        return false;
    }
    if (!IsSoundBufferReadyForOps(reinterpret_cast<uintptr_t>(soundManagerPtr), bufferIndex, false, true)) {
        return false;
    }

    void* soundBufferPtr = nullptr;
    if (!ReadSoundBufferPointer(soundManagerPtr, bufferIndex, soundBufferPtr)) {
        return false;
    }

    uintptr_t vtable = 0;
    uintptr_t setVolumeMethod = 0;
    if (!ReadChecked(reinterpret_cast<uintptr_t>(soundBufferPtr), vtable)
        || !ReadChecked(vtable + kDsBufferSetVolumeVtableOffset, setVolumeMethod)
        || !IsExecutableAddress(setVolumeMethod)) {
        return false;
    }

    auto setVolume = reinterpret_cast<DirectSoundBufferSetVolumeFn>(setVolumeMethod);
    int result = -1;
    __try {
        result = setVolume(soundBufferPtr, directSoundVolume);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return result == 0;
}

int AdjustVolumeForBuffer(void* soundManagerPtr, unsigned short bufferIndex, int volumeLevel) {
    if (!soundManagerPtr || bufferIndex >= kSoundManagerBufferCount) {
        return volumeLevel;
    }
    if (!IsSoundBufferReadyForOps(reinterpret_cast<uintptr_t>(soundManagerPtr), bufferIndex, false, true)) {
        return volumeLevel;
    }

    uintptr_t gameSystemPtr = 0;
    if (IsCurrentBgmBuffer(soundManagerPtr, bufferIndex, gameSystemPtr)) {
        const int configuredAbsoluteVolume = GetConfiguredBgmDirectSoundVolume();
        const bool alreadySharedAbsolute =
            ExtendedConfigBridge::IsSharedAudioActive()
            && std::abs(volumeLevel - configuredAbsoluteVolume) <= 2;
        if (!alreadySharedAbsolute) {
            return PercentToDirectSoundVolume(GetConfiguredBgmVolumePercent(), volumeLevel);
        }
    } else {
        const int configuredAbsoluteVolume = GetConfiguredSeDirectSoundVolume();
        const bool alreadySharedAbsolute =
            ExtendedConfigBridge::IsSharedAudioActive()
            && std::abs(volumeLevel - configuredAbsoluteVolume) <= 2;
        if (!alreadySharedAbsolute) {
            return PercentToDirectSoundVolume(GetConfiguredSeVolumePercent(), volumeLevel);
        }
    }

    return volumeLevel;
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
    return outGameSystemPtr != 0;
}

bool IsCurrentBgmBuffer(void* soundManagerPtr, unsigned short bufferIndex, uintptr_t& outGameSystemPtr) {
    outGameSystemPtr = 0;
    if (!soundManagerPtr || bufferIndex >= kSoundManagerBufferCount) {
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
    if (!soundManagerPtr || bufferIndex >= kSoundManagerBufferCount) {
        return;
    }

    uintptr_t gameSystemPtr = 0;
    if (IsCurrentBgmBuffer(soundManagerPtr, bufferIndex, gameSystemPtr)) {
        const int directSoundVolume = GetConfiguredBgmDirectSoundVolume();
        if (SetBufferVolume(soundManagerPtr, bufferIndex, directSoundVolume)) {
            const uint16_t zeroAdjustRate = 0;
            SafeWriteMemory(gameSystemPtr + kBgmTargetVolumeOffset, &directSoundVolume, sizeof(directSoundVolume));
            SafeWriteMemory(gameSystemPtr + kBgmCurrentVolumeOffset, &directSoundVolume, sizeof(directSoundVolume));
            SafeWriteMemory(gameSystemPtr + kBgmAdjustRateOffset, &zeroAdjustRate, sizeof(zeroAdjustRate));
        }
        return;
    }

    const int directSoundVolume = GetConfiguredSeDirectSoundVolume();
    SetBufferVolume(soundManagerPtr, bufferIndex, directSoundVolume);
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

bool InstallStandardAudioHooks(uintptr_t efzBase) {
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
    return ok;
}

bool InstallRevivalAudioChainHooks(uintptr_t efzBase) {
#if defined(_M_IX86)
    if (g_revivalAudioChainInstalled) {
        return true;
    }

    uintptr_t playTarget = 0;
    uintptr_t setVolumeTarget = 0;
    if (!ReadRelativeJumpTarget(efzBase + kPlaySoundBufferRva, playTarget)
        || !ReadRelativeJumpTarget(efzBase + kSetSoundVolumeRva, setVolumeTarget)) {
        if (!g_revivalAudioChainDeferredLogged.exchange(true, std::memory_order_acq_rel)) {
            LogOut("[AUDIO] EfzRevival audio JMPs are not ready yet; runtime audio chain deferred", true);
        }
        return false;
    }

    g_revivalPlaySoundBufferTarget = playTarget;
    g_revivalSetSoundVolumeTarget = setVolumeTarget;

    bool ok = true;
    ok = InstallHook("revival playSoundBuffer chain",
                     reinterpret_cast<void*>(efzBase + kPlaySoundBufferRva),
                     reinterpret_cast<void*>(&HookedRevivalPlaySoundBufferBridge),
                     &g_originalPlaySoundBuffer) && ok;
    ok = InstallHook("revival setSoundVolume chain",
                     reinterpret_cast<void*>(efzBase + kSetSoundVolumeRva),
                     reinterpret_cast<void*>(&HookedRevivalSetSoundVolumeBridge),
                     &g_originalSetSoundVolume) && ok;
    ok = InstallHook("playBackgroundMusic",
                     reinterpret_cast<void*>(efzBase + kPlayBackgroundMusicRva),
                     reinterpret_cast<void*>(&HookedPlayBackgroundMusic),
                     &g_originalPlayBackgroundMusic) && ok;
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
        g_revivalAudioChainInstalled = true;
        LogOut("[AUDIO] runtime audio hooks chained after EfzRevival audio trampolines", true);
    }
    return ok;
#else
    (void)efzBase;
    LogOut("[AUDIO] EfzRevival audio chain requires x86 naked bridge support", true);
    return false;
#endif
}

void __fastcall HookedPlayBackgroundMusic(uintptr_t gameSystemPtr, void*, unsigned short trackNumber) {
    if (g_originalPlayBackgroundMusic) {
        SehCallPlayBackgroundMusic(g_originalPlayBackgroundMusic, gameSystemPtr, trackNumber);
    }
}

int __fastcall HookedPlaySoundBuffer(void* soundManagerPtr, void*, unsigned short bufferIndex, int loopFlag) {
    if (RuntimeAudioControlSuppressed("playSoundBuffer hook invoked after external audio owner appeared")) {
        bool originalOk = false;
        return SehCallPlaySoundBuffer(g_originalPlaySoundBuffer, soundManagerPtr, bufferIndex, loopFlag, &originalOk);
    }

    if (!IsSoundBufferReadyForOps(reinterpret_cast<uintptr_t>(soundManagerPtr), bufferIndex, true, false)) {
        return static_cast<int>(bufferIndex);
    }

    SehApplyVolumeBeforePlayback(soundManagerPtr, bufferIndex);

    bool originalOk = false;
    const int result = SehCallPlaySoundBuffer(g_originalPlaySoundBuffer, soundManagerPtr, bufferIndex, loopFlag, &originalOk);
    return result;
}

int __fastcall HookedSetSoundVolume(void* soundManagerPtr, void*, unsigned short bufferIndex, int volumeLevel) {
    if (RuntimeAudioControlSuppressed("setSoundVolume hook invoked after external audio owner appeared")) {
        return static_cast<int>(bufferIndex);
    }

    if (!IsSoundBufferReadyForOps(reinterpret_cast<uintptr_t>(soundManagerPtr), bufferIndex, false, true)) {
        return static_cast<int>(bufferIndex);
    }

    const int adjustedVolumeLevel = AdjustVolumeForBuffer(soundManagerPtr, bufferIndex, volumeLevel);
    return SetBufferVolume(soundManagerPtr, bufferIndex, adjustedVolumeLevel)
        ? 0
        : static_cast<int>(bufferIndex);
}

int __fastcall HookedFadeWithSoundAdjustment(void* screenEffectPtr,
                                            void*,
                                            int paletteId,
                                            unsigned char fadeDirection,
                                            int baseVolume,
                                            int volumeAdjustment) {
    bool originalOk = false;
    const int result = SehCallFadeWithSoundAdjustment(g_originalFadeWithSoundAdjustment,
                                                      screenEffectPtr,
                                                      paletteId,
                                                      fadeDirection,
                                                      baseVolume,
                                                      volumeAdjustment,
                                                      &originalOk);
    return result;
}

void __fastcall HookedLoadSoundEffects(uintptr_t gameSystemPtr, void*) {
    if (g_originalLoadSoundEffects && !SehCallLoadSoundEffects(g_originalLoadSoundEffects, gameSystemPtr)) {
        return;
    }
}

void __fastcall HookedLoadCharacterSounds(uintptr_t characterPtr, void*) {
    if (g_originalLoadCharacterSounds && !SehCallLoadCharacterSounds(g_originalLoadCharacterSounds, characterPtr)) {
        return;
    }
}

void __stdcall PreRevivalPlaySoundBuffer(void* soundManagerPtr, unsigned short bufferIndex, int loopFlag) {
    (void)loopFlag;
    __try {
        ApplyVolumeBeforePlayback(soundManagerPtr, bufferIndex);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

int __stdcall AdjustRevivalSetSoundVolume(void* soundManagerPtr, unsigned short bufferIndex, int volumeLevel) {
    __try {
        return AdjustVolumeForBuffer(soundManagerPtr, bufferIndex, volumeLevel);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return volumeLevel;
    }
}

#if defined(_M_IX86)
__declspec(naked) void HookedRevivalPlaySoundBufferBridge() {
    __asm {
        pushfd
        pushad
        mov eax, dword ptr [esp + 24]      // original ECX: sound manager
        movzx ecx, word ptr [esp + 40]     // original stack arg 1: buffer index
        mov edx, dword ptr [esp + 44]      // original stack arg 2: loop flag
        push edx
        push ecx
        push eax
        call PreRevivalPlaySoundBuffer
        popad
        popfd
        jmp dword ptr [g_revivalPlaySoundBufferTarget]
    }
}

__declspec(naked) void HookedRevivalSetSoundVolumeBridge() {
    __asm {
        pushfd
        pushad
        mov eax, dword ptr [esp + 24]      // original ECX: sound manager
        movzx ecx, word ptr [esp + 40]     // original stack arg 1: buffer index
        mov edx, dword ptr [esp + 44]      // original stack arg 2: volume
        push edx
        push ecx
        push eax
        call AdjustRevivalSetSoundVolume
        mov dword ptr [esp + 44], eax      // rewrite original volume argument for Revival
        popad
        popfd
        jmp dword ptr [g_revivalSetSoundVolumeTarget]
    }
}
#endif

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

    bool ok = false;
    if (IsEfzRevivalModuleLoaded()) {
        ok = InstallRevivalAudioChainHooks(efzBase);
    } else if (EfzAudioEntryPointsAlreadyOwned(efzBase)) {
        g_runtimeAudioControlSuppressed.store(true, std::memory_order_release);
        g_volumeApplicationReady.store(false, std::memory_order_release);
        LogRuntimeAudioSuppressedOnce("EFZ audio entrypoints already JMP-patched by an unknown owner");
        return false;
    } else {
        ok = InstallStandardAudioHooks(efzBase);
    }

    if (ok) {
        g_hooksInstalled = true;
        if (!g_revivalAudioChainInstalled) {
            LogOut("[AUDIO] runtime audio hooks installed", true);
        }
    } else {
        if (!g_audioHookIncompleteLogged.exchange(true, std::memory_order_acq_rel)) {
            LogOut("[AUDIO] runtime audio hook install incomplete", true);
        }
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
    if (RuntimeAudioControlSuppressed("playBackgroundMusic volume follow-up")) {
        return true;
    }
    if (!RuntimeVolumeApplicationReady()) {
        TryEnableVolumeApplication(gameSystemPtr, "requested playBackgroundMusic");
    }
    if (!RuntimeVolumeApplicationReady()) {
        return true;
    }
    if (!SehApplyBgmVolumeToGameSystem(gameSystemPtr)) {
        LogOut("[AUDIO][SEH] Exception while applying BGM volume after requested playBackgroundMusic", true);
    }
    return true;
}

void ApplyConfiguredVolumesNow() {
    if (RuntimeAudioControlSuppressed("ApplyConfiguredVolumesNow")) {
        return;
    }
    if (!RuntimeVolumeApplicationReady()) {
        TryEnableVolumeApplication(0, "ApplyConfiguredVolumesNow");
    }
    if (!RuntimeVolumeApplicationReady()) {
        TraceAudio("[AUDIO][TRACE] ApplyConfiguredVolumesNow skipped because runtime volume application is not ready");
        return;
    }

    TraceAudio("[AUDIO][TRACE] ApplyConfiguredVolumesNow begin");
    ExtendedConfigBridge::ImportAudioSettingsIfAvailable(false);
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

void SetVolumeApplicationReady(bool ready) {
    if (ready && RuntimeAudioControlSuppressed("SetVolumeApplicationReady")) {
        return;
    }
    if (ready && !GameSoundSystemReady(0)) {
        TraceAudio("[AUDIO][TRACE] SetVolumeApplicationReady(true) deferred because EFZ sound system is not ready");
        return;
    }

    const bool previous = g_volumeApplicationReady.exchange(ready, std::memory_order_acq_rel);
    if (previous != ready) {
        LogOut(std::string("[AUDIO] runtime volume application ")
               + (ready ? "enabled" : "disabled"), true);
    }
}

bool IsVolumeApplicationReady() {
    return RuntimeVolumeApplicationReady();
}

bool IsGameSoundSystemReady(uintptr_t gameSystemPtr) {
    return GameSoundSystemReady(gameSystemPtr);
}

bool IsCommonSoundEffectReady(uintptr_t gameSystemPtr, unsigned short soundIndex) {
    return CommonSoundEffectReady(gameSystemPtr, soundIndex);
}

bool EnableVolumeApplicationIfSoundReady(uintptr_t gameSystemPtr, const char* reason) {
    return TryEnableVolumeApplication(gameSystemPtr, reason);
}

} // namespace AudioControl
