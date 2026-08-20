#include "../include/utils/audio_control.h"

#include "../include/utils/bgm_control.h"
#include "../include/utils/config.h"
#include "../include/utils/extended_config_bridge.h"
#include "../include/core/constants.h"
#include "../include/core/logger.h"
#include "../include/core/memory.h"
#include "../include/utils/debug_log.h"
#include "../include/utils/utilities.h"
#include "../3rdparty/minhook/include/MinHook.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <iterator>
#include <limits>
#include <mutex>
#include <sstream>
#include <string>
#include <tlhelp32.h>

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

constexpr std::array<uint8_t, 6> kVanillaPlaySoundBufferPrologue = {
    0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x08,
};
constexpr std::array<uint8_t, 7> kVanillaSetSoundVolumePrologue = {
    0x55, 0x8B, 0xEC, 0x51, 0x89, 0x4D, 0xFC,
};

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
#if defined(_M_IX86)
// Revival's patch trampoline executes:
//   pushad; pushfd; call callback; popfd; popad
// without pushing conventional callback arguments.  Consequently the callback
// sees the saved machine state plus the intercepted function's return address
// and arguments as twelve cdecl DWORD slots.  This layout is shared by the
// 1.02e-i decompilations and the 1.02j refactor; the callback implementation
// itself may consume either slots 11/12 or the original-ESP slot (slot 5).
using RevivalAudioCallbackFn = int(__cdecl*)(
    uintptr_t, uintptr_t, uintptr_t, uintptr_t,
    uintptr_t, uintptr_t, uintptr_t, uintptr_t,
    uintptr_t, uintptr_t, uintptr_t, uintptr_t);
#endif

PlayBackgroundMusicFn g_originalPlayBackgroundMusic = nullptr;
PlaySoundBufferFn g_originalPlaySoundBuffer = nullptr;
SetSoundVolumeFn g_originalSetSoundVolume = nullptr;
FadeWithSoundAdjustmentFn g_originalFadeWithSoundAdjustment = nullptr;
LoadSoundEffectsFn g_originalLoadSoundEffects = nullptr;
LoadCharacterSoundsFn g_originalLoadCharacterSounds = nullptr;
#if defined(_M_IX86)
RevivalAudioCallbackFn g_originalRevivalPlaySoundCallback = nullptr;
RevivalAudioCallbackFn g_originalRevivalSetVolumeCallback = nullptr;
#endif
HMODULE g_pinnedRevivalModule = nullptr;
uintptr_t g_revivalPlayCallbackTarget = 0;
uintptr_t g_revivalSetVolumeCallbackTarget = 0;

enum class HookGroupState {
    NotInstalled,
    Deferred,
    Installed,
    Suppressed,
    Failed,
};

enum class ContestedHookOwner {
    Unresolved,
    Vanilla,
    Revival,
    External,
};

HookGroupState g_commonHookState = HookGroupState::NotInstalled;
HookGroupState g_contestedHookState = HookGroupState::NotInstalled;
ContestedHookOwner g_contestedHookOwner = ContestedHookOwner::Unresolved;
std::mutex g_audioHookInstallMutex;
std::atomic<bool> g_volumeApplicationReady{false};
std::atomic<bool> g_runtimeAudioControlSuppressed{false};
std::atomic<bool> g_runtimeAudioSuppressLogged{false};
std::atomic<bool> g_revivalAudioChainDeferredLogged{false};
std::atomic<bool> g_revivalAudioPairIncompleteLogged{false};
std::atomic<bool> g_audioHookIncompleteLogged{false};
std::atomic<bool> g_audioHookConflictLogged{false};
std::atomic<bool> g_revivalCallbackFrameInvalidLogged{false};
std::atomic<bool> g_audioHostDeferredLogged{false};
bool g_vanillaHostCandidateActive = false;
DWORD g_vanillaHostCandidateSince = 0;
std::string g_vanillaHostCandidateParent;

constexpr DWORD kVanillaHostStabilityMs = 2000;

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
int __cdecl HookedRevivalPlaySoundCallback(
    uintptr_t, uintptr_t, uintptr_t, uintptr_t,
    uintptr_t, uintptr_t, uintptr_t, uintptr_t,
    uintptr_t, uintptr_t, uintptr_t, uintptr_t);
int __cdecl HookedRevivalSetVolumeCallback(
    uintptr_t, uintptr_t, uintptr_t, uintptr_t,
    uintptr_t, uintptr_t, uintptr_t, uintptr_t,
    uintptr_t, uintptr_t, uintptr_t, uintptr_t);
#endif

void TraceAudio(const std::string& message) {
    DebugLog::Write(message);
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

// Revival owns short-lived executable wrappers on its heap.  VirtualQuery +
// memcpy (and SafeReadMemory, which uses that same pattern) still has a TOCTOU
// window if Revival frees a wrapper between the probe and the copy.  A
// ReadProcessMemory snapshot fails cleanly when that happens instead of taking
// an access violation in this module.
bool ReadProcessSnapshot(uintptr_t address, void* destination, size_t size) {
    if (!address || !destination || size == 0) {
        return false;
    }

    SIZE_T bytesRead = 0;
    return ReadProcessMemory(GetCurrentProcess(),
                             reinterpret_cast<LPCVOID>(address),
                             destination,
                             size,
                             &bytesRead) != FALSE
        && bytesRead == size;
}

template <size_t N>
bool ReadBytes(uintptr_t address, std::array<uint8_t, N>& out) {
    out.fill(0);
    return ReadProcessSnapshot(address, out.data(), out.size());
}

template <size_t N>
bool BytesMatch(uintptr_t address, const std::array<uint8_t, N>& expected) {
    std::array<uint8_t, N> actual{};
    return ReadBytes(address, actual) && actual == expected;
}

std::string FormatBytes(uintptr_t address, size_t count) {
    constexpr size_t kMaxDiagnosticBytes = 16;
    count = (std::min)(count, kMaxDiagnosticBytes);
    std::array<uint8_t, kMaxDiagnosticBytes> bytes{};
    if (!ReadProcessSnapshot(address, bytes.data(), count)) {
        return "<unreadable>";
    }

    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (size_t i = 0; i < count; ++i) {
        if (i != 0) {
            oss << ' ';
        }
        oss << std::setw(2) << static_cast<unsigned int>(bytes[i]);
    }
    return oss.str();
}

bool AddRelative32(uintptr_t instructionEnd, int32_t displacement, uintptr_t& outTarget) {
    outTarget = 0;
    const int64_t target = static_cast<int64_t>(instructionEnd)
        + static_cast<int64_t>(displacement);
    if (target <= 0
        || static_cast<uint64_t>(target) > static_cast<uint64_t>((std::numeric_limits<uintptr_t>::max)())) {
        return false;
    }
    outTarget = static_cast<uintptr_t>(target);
    return true;
}

bool DecodeRelativeBranchTarget(const uint8_t* snapshot,
                                size_t snapshotSize,
                                size_t instructionOffset,
                                uintptr_t instructionAddress,
                                uint8_t expectedOpcode,
                                uintptr_t& outTarget) {
    outTarget = 0;
    if (!snapshot || instructionOffset > snapshotSize || snapshotSize - instructionOffset < 5
        || snapshot[instructionOffset] != expectedOpcode) {
        return false;
    }

    int32_t displacement = 0;
    std::memcpy(&displacement, snapshot + instructionOffset + 1, sizeof(displacement));
    return AddRelative32(instructionAddress + 5, displacement, outTarget);
}

bool GetModuleImageRange(HMODULE module, uintptr_t& outBase, uintptr_t& outEnd) {
    outBase = reinterpret_cast<uintptr_t>(module);
    outEnd = 0;
    if (!outBase) {
        return false;
    }

    IMAGE_DOS_HEADER dos{};
    if (!ReadProcessSnapshot(outBase, &dos, sizeof(dos))
        || dos.e_magic != IMAGE_DOS_SIGNATURE
        || dos.e_lfanew <= 0
        || dos.e_lfanew > 0x100000) {
        return false;
    }

    IMAGE_NT_HEADERS32 nt{};
    const uintptr_t ntAddress = outBase + static_cast<uintptr_t>(dos.e_lfanew);
    if (ntAddress < outBase
        || !ReadProcessSnapshot(ntAddress, &nt, sizeof(nt))
        || nt.Signature != IMAGE_NT_SIGNATURE
        || nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC
        || nt.OptionalHeader.SizeOfImage == 0) {
        return false;
    }

    outEnd = outBase + static_cast<uintptr_t>(nt.OptionalHeader.SizeOfImage);
    return outEnd > outBase;
}

bool AddressBelongsToModule(uintptr_t address, HMODULE module) {
    uintptr_t moduleBase = 0;
    uintptr_t moduleEnd = 0;
    return GetModuleImageRange(module, moduleBase, moduleEnd)
        && address >= moduleBase
        && address < moduleEnd
        && IsExecutableAddress(address);
}

bool AcquireRevivalModulePin(HMODULE expectedModule,
                             uintptr_t playCallback,
                             uintptr_t setVolumeCallback,
                             HMODULE& outPinnedModule) {
    outPinnedModule = nullptr;
    if (!expectedModule || !playCallback || !setVolumeCallback) {
        return false;
    }

    HMODULE pinnedModule = nullptr;
    if (!GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
            reinterpret_cast<LPCSTR>(playCallback),
            &pinnedModule)) {
        LogOut("[AUDIO][HOOK] failed to retain EfzRevival.dll for validated audio callbacks"
               " (GetModuleHandleExA error=" + std::to_string(GetLastError()) + ")", true);
        return false;
    }

    // FROM_ADDRESS takes a loader reference before returning.  Verify that the
    // address still resolves to the exact module snapshot we validated and
    // that both callback targets remain executable within that image.
    if (pinnedModule != expectedModule
        || GetModuleHandleA("EfzRevival.dll") != pinnedModule
        || !AddressBelongsToModule(playCallback, pinnedModule)
        || !AddressBelongsToModule(setVolumeCallback, pinnedModule)) {
        LogOut("[AUDIO][HOOK] EfzRevival callback module changed before it could be retained; deferring", true);
        FreeLibrary(pinnedModule);
        return false;
    }

    outPinnedModule = pinnedModule;
    return true;
}

bool QueryParentProcessImageName(std::string& outName) {
    outName.clear();

    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return false;
    }

    PROCESSENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    DWORD parentProcessId = 0;
    if (Process32First(snapshot, &entry)) {
        do {
            if (entry.th32ProcessID == GetCurrentProcessId()) {
                parentProcessId = entry.th32ParentProcessID;
                break;
            }
        } while (Process32Next(snapshot, &entry));
    }

    if (parentProcessId != 0) {
        entry = PROCESSENTRY32{};
        entry.dwSize = sizeof(entry);
        if (Process32First(snapshot, &entry)) {
            do {
                if (entry.th32ProcessID == parentProcessId) {
                    outName = entry.szExeFile;
                    break;
                }
            } while (Process32Next(snapshot, &entry));
        }
    }

    CloseHandle(snapshot);
    return !outName.empty();
}

bool WindowTitleIndicatesRevival() {
    const HWND window = FindEFZWindow();
    if (!window) {
        return false;
    }

    std::array<char, 256> title{};
    if (GetWindowTextA(window, title.data(), static_cast<int>(title.size())) <= 0) {
        return false;
    }

    std::string normalized(title.data());
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char value) {
        return static_cast<char>(std::tolower(value));
    });
    return normalized.find("revival") != std::string::npos;
}

void ResetVanillaHostCandidate() {
    g_vanillaHostCandidateActive = false;
    g_vanillaHostCandidateSince = 0;
    g_vanillaHostCandidateParent.clear();
}

bool IsKnownStandaloneParent(const std::string& imageName) {
    // These are direct interactive/development launchers.  An unfamiliar
    // parent may itself be an injector, so it is intentionally not promoted to
    // vanilla merely because its name is different from EfzRevival.exe.
    constexpr const char* kStandaloneParents[] = {
        "explorer.exe",
        "cmd.exe",
        "powershell.exe",
        "pwsh.exe",
        "devenv.exe",
        "vsdebugconsole.exe",
    };
    for (const char* candidate : kStandaloneParents) {
        if (_stricmp(imageName.c_str(), candidate) == 0) {
            return true;
        }
    }
    return false;
}

// A momentary GetModuleHandle(nullptr) result is not a host verdict: the
// standard Revival launcher creates efz.exe suspended and injects the DLL
// later, and efz_netplay_mod may manage that load as well.  Only claim the
// vanilla entrypoints after a stable, positively standalone launch signature.
// Known Revival-capable hosts remain deferred indefinitely until the actual DLL
// appears, leaving the contested EFZ bytes untouched.
bool CanFinalizeVanillaAudioOwnership(std::string& reason) {
    if (GetModuleHandleA("efz_netplay_mod.dll")
        || GetModuleHandleA("efz_netplay_mod")
        || GetModuleHandleA("netbridge.dll")) {
        ResetVanillaHostCandidate();
        reason = "a Revival-capable netplay host module is loaded";
        return false;
    }

    if (WindowTitleIndicatesRevival()) {
        ResetVanillaHostCandidate();
        reason = "the EFZ window identifies itself as a Revival host";
        return false;
    }

    std::string parentImage;
    if (!QueryParentProcessImageName(parentImage)) {
        ResetVanillaHostCandidate();
        reason = "the parent process could not be identified, so standalone ownership is unproven";
        return false;
    }
    if (_stricmp(parentImage.c_str(), "EfzRevival.exe") == 0) {
        ResetVanillaHostCandidate();
        reason = "efz.exe was launched by EfzRevival.exe";
        return false;
    }
    if (!IsKnownStandaloneParent(parentImage)) {
        ResetVanillaHostCandidate();
        reason = "the parent process is not a recognized standalone launcher (parent="
            + parentImage + ")";
        return false;
    }

    const DWORD now = GetTickCount();
    if (!g_vanillaHostCandidateActive
        || _stricmp(g_vanillaHostCandidateParent.c_str(), parentImage.c_str()) != 0) {
        g_vanillaHostCandidateActive = true;
        g_vanillaHostCandidateSince = now;
        g_vanillaHostCandidateParent = parentImage;
        reason = "standalone launch candidate detected (parent=" + parentImage
            + "); waiting for host state to remain stable";
        return false;
    }

    const DWORD stableFor = now - g_vanillaHostCandidateSince;
    if (stableFor < kVanillaHostStabilityMs) {
        reason = "standalone launch candidate is still stabilizing (parent=" + parentImage
            + ", stableMs=" + std::to_string(stableFor) + ")";
        return false;
    }

    reason = "standalone launch positively stabilized (parent=" + parentImage
        + ", stableMs=" + std::to_string(stableFor) + ")";
    return true;
}

enum class RevivalSiteValidation {
    Ready,
    NotPatched,
    Transient,
    Invalid,
};

struct RevivalSiteInfo {
    uintptr_t entry = 0;
    uintptr_t trampoline = 0;
    uintptr_t callback = 0;
    std::string reason;
};

template <size_t N>
RevivalSiteValidation ValidateRevivalAudioSite(const char* label,
                                               uintptr_t entry,
                                               HMODULE revivalModule,
                                               const std::array<uint8_t, N>& pristinePrologue,
                                               RevivalSiteInfo& outInfo) {
    outInfo = {};
    outInfo.entry = entry;

    std::array<uint8_t, N> entryBefore{};
    if (!ReadBytes(entry, entryBefore)) {
        outInfo.reason = "EFZ entrypoint snapshot is unreadable";
        return RevivalSiteValidation::Transient;
    }

    if (entryBefore == pristinePrologue) {
        outInfo.reason = "Revival has not patched the pristine EFZ entrypoint yet";
        return RevivalSiteValidation::NotPatched;
    }

    if (entryBefore[0] != 0xE9) {
        outInfo.reason = "EFZ entrypoint is neither pristine nor a relative JMP";
        return RevivalSiteValidation::Invalid;
    }
    if (!DecodeRelativeBranchTarget(entryBefore.data(), entryBefore.size(), 0, entry, 0xE9, outInfo.trampoline)
        || !IsExecutableAddress(outInfo.trampoline)) {
        outInfo.reason = "EFZ entrypoint JMP target is invalid or non-executable";
        return RevivalSiteValidation::Invalid;
    }

    constexpr size_t kPrefixSize = 9;
    constexpr size_t kRelativeJumpSize = 5;
    constexpr size_t kWrapperSize = kPrefixSize + N + kRelativeJumpSize;
    std::array<uint8_t, kWrapperSize> wrapperBefore{};
    if (!ReadBytes(outInfo.trampoline, wrapperBefore)) {
        outInfo.reason = "Revival trampoline snapshot disappeared while being read";
        return RevivalSiteValidation::Transient;
    }
    if (wrapperBefore[0] != 0x60
        || wrapperBefore[1] != 0x9C
        || wrapperBefore[2] != 0xE8
        || wrapperBefore[7] != 0x9D
        || wrapperBefore[8] != 0x61) {
        outInfo.reason = "entrypoint JMP does not target a Revival 60 9C E8 ... 9D 61 trampoline";
        return RevivalSiteValidation::Invalid;
    }

    if (!DecodeRelativeBranchTarget(wrapperBefore.data(), wrapperBefore.size(), 2,
                                    outInfo.trampoline + 2, 0xE8, outInfo.callback)
        || !IsExecutableAddress(outInfo.callback)
        || !AddressBelongsToModule(outInfo.callback, revivalModule)) {
        outInfo.reason = "Revival trampoline callback is not executable code inside EfzRevival.dll";
        return RevivalSiteValidation::Invalid;
    }

    if (!std::equal(pristinePrologue.begin(), pristinePrologue.end(), wrapperBefore.begin() + kPrefixSize)) {
        outInfo.reason = "Revival trampoline did not preserve the pristine EFZ prologue";
        return RevivalSiteValidation::Invalid;
    }

    const size_t tailOffset = kPrefixSize + N;
    uintptr_t tailTarget = 0;
    if (!DecodeRelativeBranchTarget(wrapperBefore.data(), wrapperBefore.size(), tailOffset,
                                    outInfo.trampoline + tailOffset, 0xE9, tailTarget)) {
        outInfo.reason = "Revival trampoline has no relative JMP after its copied EFZ prologue";
        return RevivalSiteValidation::Invalid;
    }
    if (tailTarget != entry + N) {
        outInfo.reason = "Revival trampoline tail does not return immediately after the copied EFZ prologue";
        return RevivalSiteValidation::Invalid;
    }

    // Revalidate both source and destination.  A valid-looking first snapshot
    // is not enough because Revival may have restored/free'd/rebuilt either
    // wrapper while this function was decoding it.
    std::array<uint8_t, N> entryAfter{};
    std::array<uint8_t, kWrapperSize> wrapperAfter{};
    if (!ReadBytes(entry, entryAfter)
        || !ReadBytes(outInfo.trampoline, wrapperAfter)
        || entryAfter != entryBefore
        || wrapperAfter != wrapperBefore) {
        outInfo.reason = "Revival audio wrapper changed during snapshot validation";
        return RevivalSiteValidation::Transient;
    }

    std::ostringstream reason;
    reason << label << " entry=0x" << std::hex << entry
           << " trampoline=0x" << outInfo.trampoline
           << " callback=0x" << outInfo.callback
           << " copied=" << FormatBytes(outInfo.trampoline + 9, pristinePrologue.size());
    outInfo.reason = reason.str();
    return RevivalSiteValidation::Ready;
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

struct PendingAudioHook {
    const char* name = nullptr;
    void* target = nullptr;
    void* detour = nullptr;
    void** original = nullptr;
};

template <typename T>
void** OriginalStorage(T* storage) {
    return reinterpret_cast<void**>(storage);
}

bool RemoveAudioHookAndClearOriginal(PendingAudioHook& hook, const char* operation) {
    if (!hook.target) {
        return hook.original == nullptr || *hook.original == nullptr;
    }

    const MH_STATUS disableStatus = MH_DisableHook(hook.target);
    if (disableStatus != MH_OK
        && disableStatus != MH_ERROR_DISABLED
        && disableStatus != MH_ERROR_NOT_CREATED) {
        std::ostringstream oss;
        oss << "[AUDIO][HOOK] " << (operation ? operation : "cleanup")
            << " disable failed at " << (hook.name ? hook.name : "<unnamed>")
            << " target=0x" << std::hex << reinterpret_cast<uintptr_t>(hook.target)
            << " status=" << std::dec << static_cast<int>(disableStatus)
            << "; attempting removal while retaining the original pointer";
        LogOut(oss.str(), true);
    }

    const MH_STATUS removeStatus = MH_RemoveHook(hook.target);
    if (removeStatus != MH_OK && removeStatus != MH_ERROR_NOT_CREATED) {
        std::ostringstream oss;
        oss << "[AUDIO][HOOK] " << (operation ? operation : "cleanup")
            << " remove failed at " << (hook.name ? hook.name : "<unnamed>")
            << " target=0x" << std::hex << reinterpret_cast<uintptr_t>(hook.target)
            << " status=" << std::dec << static_cast<int>(removeStatus)
            << "; original trampoline pointer retained";
        LogOut(oss.str(), true);
        return false;
    }

    // MH_RemoveHook owns and frees the trampoline.  Clear our callable copy
    // only after MinHook confirms that the hook was removed (or was already
    // absent).  If removal fails, the detour may still execute and must retain
    // its forwarding pointer.
    if (hook.original) {
        *hook.original = nullptr;
    }
    return true;
}

bool RollBackHookGroup(PendingAudioHook* hooks, size_t createdCount) {
    for (size_t i = 0; i < createdCount; ++i) {
        MH_QueueDisableHook(hooks[i].target);
    }
    MH_ApplyQueued();

    bool complete = true;
    for (size_t i = 0; i < createdCount; ++i) {
        complete = RemoveAudioHookAndClearOriginal(hooks[i], "transaction rollback")
            && complete;
    }
    return complete;
}

bool InstallHookGroupTransactional(const char* groupName,
                                   PendingAudioHook* hooks,
                                   size_t hookCount) {
    if (!hooks || hookCount == 0) {
        return false;
    }

    size_t createdCount = 0;
    for (; createdCount < hookCount; ++createdCount) {
        PendingAudioHook& hook = hooks[createdCount];
        const MH_STATUS status = MH_CreateHook(hook.target, hook.detour, hook.original);
        if (status != MH_OK) {
            std::ostringstream oss;
            oss << "[AUDIO][HOOK] " << groupName << " create failed at "
                << (hook.name ? hook.name : "<unnamed>")
                << " target=0x" << std::hex << reinterpret_cast<uintptr_t>(hook.target)
                << " status=" << std::dec << static_cast<int>(status);
            LogOut(oss.str(), true);
            RollBackHookGroup(hooks, createdCount);
            return false;
        }
    }

    for (size_t i = 0; i < hookCount; ++i) {
        const MH_STATUS status = MH_QueueEnableHook(hooks[i].target);
        if (status != MH_OK) {
            std::ostringstream oss;
            oss << "[AUDIO][HOOK] " << groupName << " queue-enable failed at "
                << (hooks[i].name ? hooks[i].name : "<unnamed>")
                << " status=" << static_cast<int>(status);
            LogOut(oss.str(), true);
            RollBackHookGroup(hooks, createdCount);
            return false;
        }
    }

    const MH_STATUS applyStatus = MH_ApplyQueued();
    if (applyStatus != MH_OK) {
        LogOut(std::string("[AUDIO][HOOK] ") + groupName
               + " apply failed status=" + std::to_string(static_cast<int>(applyStatus)), true);
        RollBackHookGroup(hooks, createdCount);
        return false;
    }

    LogOut(std::string("[AUDIO][HOOK] ") + groupName
           + " installed transactionally (" + std::to_string(hookCount) + " hooks)", true);
    return true;
}

bool InstallCommonAudioHooks(uintptr_t efzBase) {
    PendingAudioHook hooks[] = {
        {"playBackgroundMusic",
         reinterpret_cast<void*>(efzBase + kPlayBackgroundMusicRva),
         reinterpret_cast<void*>(&HookedPlayBackgroundMusic),
         OriginalStorage(&g_originalPlayBackgroundMusic)},
        {"fadeWithSoundAdjustment",
         reinterpret_cast<void*>(efzBase + kFadeWithSoundAdjustmentRva),
         reinterpret_cast<void*>(&HookedFadeWithSoundAdjustment),
         OriginalStorage(&g_originalFadeWithSoundAdjustment)},
        {"loadSoundEffects",
         reinterpret_cast<void*>(efzBase + kLoadSoundEffectsRva),
         reinterpret_cast<void*>(&HookedLoadSoundEffects),
         OriginalStorage(&g_originalLoadSoundEffects)},
        {"loadCharacterSounds",
         reinterpret_cast<void*>(efzBase + kLoadCharacterSoundsRva),
         reinterpret_cast<void*>(&HookedLoadCharacterSounds),
         OriginalStorage(&g_originalLoadCharacterSounds)},
    };
    return InstallHookGroupTransactional("common audio hook group", hooks, std::size(hooks));
}

bool InstallVanillaContestedAudioHooks(uintptr_t efzBase) {
    PendingAudioHook hooks[] = {
        {"playSoundBuffer",
         reinterpret_cast<void*>(efzBase + kPlaySoundBufferRva),
         reinterpret_cast<void*>(&HookedPlaySoundBuffer),
         OriginalStorage(&g_originalPlaySoundBuffer)},
        {"setSoundVolume",
         reinterpret_cast<void*>(efzBase + kSetSoundVolumeRva),
         reinterpret_cast<void*>(&HookedSetSoundVolume),
         OriginalStorage(&g_originalSetSoundVolume)},
    };
    return InstallHookGroupTransactional("vanilla contested audio hook group", hooks, std::size(hooks));
}

bool InstallRevivalAudioCallbackHooks(HMODULE revivalModule,
                                      const RevivalSiteInfo& playInfo,
                                      const RevivalSiteInfo& setVolumeInfo) {
#if defined(_M_IX86)
    if (!playInfo.callback
        || !setVolumeInfo.callback
        || playInfo.callback == setVolumeInfo.callback) {
        LogOut("[AUDIO][HOOK] Revival audio callback targets are missing or alias each other", true);
        return false;
    }
    if (g_pinnedRevivalModule) {
        LogOut("[AUDIO][HOOK] Revival callback installation encountered an existing module pin", true);
        return false;
    }

    HMODULE pinnedModule = nullptr;
    if (!AcquireRevivalModulePin(revivalModule,
                                 playInfo.callback,
                                 setVolumeInfo.callback,
                                 pinnedModule)) {
        return false;
    }

    PendingAudioHook hooks[] = {
        {"Revival playSoundBuffer callback",
         reinterpret_cast<void*>(playInfo.callback),
         reinterpret_cast<void*>(&HookedRevivalPlaySoundCallback),
         OriginalStorage(&g_originalRevivalPlaySoundCallback)},
        {"Revival setSoundVolume callback",
         reinterpret_cast<void*>(setVolumeInfo.callback),
         reinterpret_cast<void*>(&HookedRevivalSetVolumeCallback),
         OriginalStorage(&g_originalRevivalSetVolumeCallback)},
    };

    const bool ok = InstallHookGroupTransactional(
        "stable Revival audio callback group",
        hooks,
        std::size(hooks));
    const bool rollbackLeftHookState = g_originalRevivalPlaySoundCallback != nullptr
        || g_originalRevivalSetVolumeCallback != nullptr;
    if (ok || rollbackLeftHookState) {
        // Keep the image containing both callback targets loaded for as long as
        // one of our MinHook detours/trampolines can reference it.  A failed
        // transaction whose rollback could not remove every hook is retained
        // too; dropping the reference in that state would turn a recoverable
        // hook failure into an executable-address use-after-free.
        g_pinnedRevivalModule = pinnedModule;
        g_revivalPlayCallbackTarget = playInfo.callback;
        g_revivalSetVolumeCallbackTarget = setVolumeInfo.callback;
    } else if (!FreeLibrary(pinnedModule)) {
        g_pinnedRevivalModule = pinnedModule;
        LogOut("[AUDIO][HOOK] failed to release the unused EfzRevival module reference"
               " after callback-hook rollback; retaining it for shutdown", true);
    }

    if (ok) {
        std::ostringstream oss;
        oss << "[AUDIO] runtime audio hooks attached to stable EfzRevival callbacks"
            << " play=0x" << std::hex << playInfo.callback
            << " setVolume=0x" << setVolumeInfo.callback
            << "; EFZ entrypoints and Revival-owned trampolines remain untouched";
        LogOut(oss.str(), true);
    }
    if (!ok && rollbackLeftHookState) {
        LogOut("[AUDIO][HOOK] Revival callback transaction rollback was incomplete;"
               " callback module pin and original trampoline pointer retained", true);
    }
    return ok;
#else
    (void)revivalModule;
    (void)playInfo;
    (void)setVolumeInfo;
    LogOut("[AUDIO] EfzRevival audio callback hooks require the x86 callback ABI", true);
    return false;
#endif
}

void __fastcall HookedPlayBackgroundMusic(uintptr_t gameSystemPtr, void*, unsigned short trackNumber) {
    // GameSystem+0xF26 is only the allocated sound-buffer index. Capture the
    // logical track argument at the game entry point so mission metadata never
    // confuses those two ID spaces.
    SetLastBgmTrack(trackNumber);
    if (!g_originalPlayBackgroundMusic
        || !SehCallPlayBackgroundMusic(g_originalPlayBackgroundMusic, gameSystemPtr, trackNumber)) {
        return;
    }

    // These absolute DirectSound writes do not require ownership of Revival's
    // contested EFZ entrypoints.  Keep volume behavior intact while ownership
    // is deliberately deferred for a late-loading Revival host.
    if (!RuntimeAudioControlSuppressed("BGM load volume follow-up")
        && TryEnableVolumeApplication(gameSystemPtr, "BGM load")
        && !SehApplyBgmVolumeToGameSystem(gameSystemPtr)) {
        TraceAudio("[AUDIO][SEH] BGM volume follow-up failed after playBackgroundMusic");
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

    if (!RuntimeAudioControlSuppressed("common SE load volume follow-up")
        && TryEnableVolumeApplication(gameSystemPtr, "common SE load")
        && !SehApplyCommonSeVolumeToGameSystem(gameSystemPtr)) {
        TraceAudio("[AUDIO][SEH] common SE volume follow-up failed after loadSoundEffects");
    }
}

void __fastcall HookedLoadCharacterSounds(uintptr_t characterPtr, void*) {
    if (g_originalLoadCharacterSounds && !SehCallLoadCharacterSounds(g_originalLoadCharacterSounds, characterPtr)) {
        return;
    }

    if (!RuntimeAudioControlSuppressed("character SE load volume follow-up")
        && TryEnableVolumeApplication(0, "character SE load")
        && !SehApplyCharacterSeVolume(characterPtr)) {
        TraceAudio("[AUDIO][SEH] character SE volume follow-up failed after loadCharacterSounds");
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
bool ValidateRevivalCallbackFrame(uintptr_t originalEsp,
                                  uintptr_t savedReturnAddress,
                                  uintptr_t bufferSlot,
                                  uintptr_t valueSlot) {
    // The dynamically validated Revival wrapper guarantees the pushad/pushfd
    // layout.  Cross-check the saved original ESP before using it so a future
    // callback caller with an ordinary cdecl frame cannot make us write through
    // an unrelated fifth argument.
    if (!IsReadableRange(originalEsp, 3 * sizeof(uintptr_t))) {
        return false;
    }

    uintptr_t observedReturnAddress = 0;
    uint16_t observedBufferIndex = 0;
    int observedValue = 0;
    return SafeReadMemory(originalEsp, &observedReturnAddress, sizeof(observedReturnAddress))
        && SafeReadMemory(originalEsp + sizeof(uintptr_t),
                          &observedBufferIndex,
                          sizeof(observedBufferIndex))
        && SafeReadMemory(originalEsp + 2 * sizeof(uintptr_t),
                          &observedValue,
                          sizeof(observedValue))
        && observedReturnAddress == savedReturnAddress
        && observedBufferIndex == static_cast<uint16_t>(bufferSlot)
        && observedValue == static_cast<int>(valueSlot);
}

void LogInvalidRevivalCallbackFrameOnce() {
    if (!g_revivalCallbackFrameInvalidLogged.exchange(true, std::memory_order_acq_rel)) {
        LogOut("[AUDIO][HOOK] Revival audio callback arrived without the validated wrapper frame; forwarding unchanged", true);
    }
}

int __cdecl HookedRevivalPlaySoundCallback(uintptr_t savedFlags,
                                           uintptr_t savedEdi,
                                           uintptr_t savedEsi,
                                           uintptr_t savedEbp,
                                           uintptr_t originalEsp,
                                           uintptr_t savedEbx,
                                           uintptr_t savedEdx,
                                           uintptr_t originalEcx,
                                           uintptr_t savedEax,
                                           uintptr_t originalReturnAddress,
                                           uintptr_t bufferSlot,
                                           uintptr_t loopFlagSlot) {
    if (ValidateRevivalCallbackFrame(originalEsp,
                                     originalReturnAddress,
                                     bufferSlot,
                                     loopFlagSlot)) {
        PreRevivalPlaySoundBuffer(reinterpret_cast<void*>(originalEcx),
                                  static_cast<unsigned short>(bufferSlot),
                                  static_cast<int>(loopFlagSlot));
    } else {
        LogInvalidRevivalCallbackFrameOnce();
    }

    if (!g_originalRevivalPlaySoundCallback) {
        return 0;
    }
    return g_originalRevivalPlaySoundCallback(savedFlags,
                                              savedEdi,
                                              savedEsi,
                                              savedEbp,
                                              originalEsp,
                                              savedEbx,
                                              savedEdx,
                                              originalEcx,
                                              savedEax,
                                              originalReturnAddress,
                                              bufferSlot,
                                              loopFlagSlot);
}

int __cdecl HookedRevivalSetVolumeCallback(uintptr_t savedFlags,
                                           uintptr_t savedEdi,
                                           uintptr_t savedEsi,
                                           uintptr_t savedEbp,
                                           uintptr_t originalEsp,
                                           uintptr_t savedEbx,
                                           uintptr_t savedEdx,
                                           uintptr_t originalEcx,
                                           uintptr_t savedEax,
                                           uintptr_t originalReturnAddress,
                                           uintptr_t bufferSlot,
                                           uintptr_t volumeSlot) {
    uintptr_t forwardedVolumeSlot = volumeSlot;
    if (ValidateRevivalCallbackFrame(originalEsp,
                                     originalReturnAddress,
                                     bufferSlot,
                                     volumeSlot)) {
        const int adjustedVolume = AdjustRevivalSetSoundVolume(
            reinterpret_cast<void*>(originalEcx),
            static_cast<unsigned short>(bufferSlot),
            static_cast<int>(volumeSlot));

        // The Revival callback is advisory; the copied EFZ prologue executes
        // after it returns and consumes the original stack argument.  Update
        // that live argument as well as the value forwarded to Revival so both
        // paths observe the same configured volume.
        __try {
            *reinterpret_cast<volatile int*>(originalEsp + 2 * sizeof(uintptr_t)) = adjustedVolume;
            forwardedVolumeSlot = static_cast<uintptr_t>(adjustedVolume);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            forwardedVolumeSlot = volumeSlot;
        }
    } else {
        LogInvalidRevivalCallbackFrameOnce();
    }

    if (!g_originalRevivalSetVolumeCallback) {
        return 0;
    }
    return g_originalRevivalSetVolumeCallback(savedFlags,
                                              savedEdi,
                                              savedEsi,
                                              savedEbp,
                                              originalEsp,
                                              savedEbx,
                                              savedEdx,
                                              originalEcx,
                                              savedEax,
                                              originalReturnAddress,
                                              bufferSlot,
                                              forwardedVolumeSlot);
}
#endif

} // namespace

const char* HookInstallResultName(HookInstallResult result) {
    switch (result) {
        case HookInstallResult::Deferred: return "Deferred";
        case HookInstallResult::Ready: return "Ready";
        case HookInstallResult::Suppressed: return "Suppressed";
        case HookInstallResult::Failed: return "Failed";
        default: return "Unknown";
    }
}

void ShutdownHooks(bool releaseRevivalModuleReference) {
    std::lock_guard<std::mutex> lock(g_audioHookInstallMutex);

#if defined(_M_IX86)
    PendingAudioHook playHook{
        "Revival playSoundBuffer callback",
        reinterpret_cast<void*>(g_revivalPlayCallbackTarget),
        nullptr,
        OriginalStorage(&g_originalRevivalPlaySoundCallback),
    };
    PendingAudioHook setVolumeHook{
        "Revival setSoundVolume callback",
        reinterpret_cast<void*>(g_revivalSetVolumeCallbackTarget),
        nullptr,
        OriginalStorage(&g_originalRevivalSetVolumeCallback),
    };

    const bool playRemoved = g_revivalPlayCallbackTarget
        ? RemoveAudioHookAndClearOriginal(playHook, "shutdown")
        : g_originalRevivalPlaySoundCallback == nullptr;
    const bool setVolumeRemoved = g_revivalSetVolumeCallbackTarget
        ? RemoveAudioHookAndClearOriginal(setVolumeHook, "shutdown")
        : g_originalRevivalSetVolumeCallback == nullptr;

    if (playRemoved) {
        g_revivalPlayCallbackTarget = 0;
    }
    if (setVolumeRemoved) {
        g_revivalSetVolumeCallbackTarget = 0;
    }

    if (!playRemoved || !setVolumeRemoved) {
        LogOut("[AUDIO][HOOK] shutdown could not remove every Revival callback hook;"
               " retaining the callback module reference and original trampoline pointer", true);
        return;
    }
#endif

    if (g_pinnedRevivalModule && !releaseRevivalModuleReference) {
        // FreeLibrary is forbidden while DllMain owns the loader lock.  The
        // callback detours are gone, so leaking this single reference until
        // process exit is safe and avoids a recursive loader-lock transition.
        LogOut("[AUDIO][HOOK] Revival callback hooks removed; retained module reference"
               " left pinned until process exit (loader-lock shutdown)", true);
    } else if (g_pinnedRevivalModule) {
        if (!FreeLibrary(g_pinnedRevivalModule)) {
            LogOut("[AUDIO][HOOK] shutdown removed Revival callback hooks but failed to"
                   " release the retained module reference", true);
            return;
        }
        g_pinnedRevivalModule = nullptr;
        LogOut("[AUDIO][HOOK] Revival callback hooks removed and retained module reference released", true);
    }

    if (g_contestedHookOwner == ContestedHookOwner::Revival) {
        g_contestedHookOwner = ContestedHookOwner::Unresolved;
        g_contestedHookState = HookGroupState::NotInstalled;
    }
}

HookInstallResult InstallHooks(uintptr_t efzBase, HookInstallPhase phase) {
    std::lock_guard<std::mutex> lock(g_audioHookInstallMutex);

    if (!efzBase) {
        TraceAudio("[AUDIO][TRACE] InstallHooks failed because efzBase was null");
        return HookInstallResult::Failed;
    }

    if (g_commonHookState == HookGroupState::NotInstalled) {
        if (!InstallCommonAudioHooks(efzBase)) {
            g_commonHookState = HookGroupState::Failed;
            if (!g_audioHookIncompleteLogged.exchange(true, std::memory_order_acq_rel)) {
                LogOut("[AUDIO] common audio hook install failed transactionally", true);
            }
            return HookInstallResult::Failed;
        }
        g_commonHookState = HookGroupState::Installed;
        LogOut("[AUDIO] non-conflicting runtime audio hooks installed", true);
    } else if (g_commonHookState == HookGroupState::Failed) {
        return HookInstallResult::Failed;
    }

    if (g_contestedHookState == HookGroupState::Installed) {
        return HookInstallResult::Ready;
    }
    if (g_contestedHookState == HookGroupState::Suppressed) {
        return HookInstallResult::Suppressed;
    }
    if (g_contestedHookState == HookGroupState::Failed) {
        return HookInstallResult::Failed;
    }

    if (phase == HookInstallPhase::CommonOnly) {
        g_contestedHookState = HookGroupState::Deferred;
        if (!g_revivalAudioChainDeferredLogged.exchange(true, std::memory_order_acq_rel)) {
            LogOut("[AUDIO][HOOK] playSoundBuffer/setSoundVolume ownership deferred until Revival resolution", true);
        }
        return HookInstallResult::Deferred;
    }

    const uintptr_t playEntry = efzBase + kPlaySoundBufferRva;
    const uintptr_t setVolumeEntry = efzBase + kSetSoundVolumeRva;
    HMODULE revivalModule = GetModuleHandleA("EfzRevival.dll");

    if (revivalModule) {
        ResetVanillaHostCandidate();
        RevivalSiteInfo playInfo{};
        RevivalSiteInfo setVolumeInfo{};
        const RevivalSiteValidation playValidation = ValidateRevivalAudioSite(
            "playSoundBuffer",
            playEntry,
            revivalModule,
            kVanillaPlaySoundBufferPrologue,
            playInfo);
        const RevivalSiteValidation setVolumeValidation = ValidateRevivalAudioSite(
            "setSoundVolume",
            setVolumeEntry,
            revivalModule,
            kVanillaSetSoundVolumePrologue,
            setVolumeInfo);

        // Revival restores and rebuilds these EFZ wrappers from its frame hook.
        // A read can therefore observe pristine bytes or a partially rewritten
        // JMP even on a supported build.  Treat every non-ready snapshot as
        // transient and retry; never claim or suppress an entrypoint based on a
        // torn observation.  Once both wrappers validate, only their stable DLL
        // callback targets are hooked below.
        if (playValidation != RevivalSiteValidation::Ready
            || setVolumeValidation != RevivalSiteValidation::Ready) {
            g_contestedHookState = HookGroupState::Deferred;
            if (!g_revivalAudioPairIncompleteLogged.exchange(true, std::memory_order_acq_rel)) {
                std::ostringstream oss;
                oss << "[AUDIO][HOOK] Revival audio wrapper snapshot not ready; deferring"
                    << " play={" << playInfo.reason << ", bytes=" << FormatBytes(playEntry, 12) << "}"
                    << " setVolume={" << setVolumeInfo.reason << ", bytes=" << FormatBytes(setVolumeEntry, 12) << "}";
                LogOut(oss.str(), true);
            }
            return HookInstallResult::Deferred;
        }
        if (GetModuleHandleA("EfzRevival.dll") != revivalModule) {
            g_contestedHookState = HookGroupState::Deferred;
            return HookInstallResult::Deferred;
        }

        LogOut(std::string("[AUDIO][HOOK] validated Revival ") + playInfo.reason, true);
        LogOut(std::string("[AUDIO][HOOK] validated Revival ") + setVolumeInfo.reason, true);
        if (!InstallRevivalAudioCallbackHooks(revivalModule, playInfo, setVolumeInfo)) {
            g_contestedHookState = HookGroupState::Failed;
            if (!g_audioHookIncompleteLogged.exchange(true, std::memory_order_acq_rel)) {
                LogOut("[AUDIO] Revival audio callback hook install failed transactionally", true);
            }
            return HookInstallResult::Failed;
        }

        g_contestedHookOwner = ContestedHookOwner::Revival;
        g_contestedHookState = HookGroupState::Installed;
        return HookInstallResult::Ready;
    }

    std::string hostResolutionReason;
    if (!CanFinalizeVanillaAudioOwnership(hostResolutionReason)) {
        g_contestedHookState = HookGroupState::Deferred;
        if (!g_audioHostDeferredLogged.exchange(true, std::memory_order_acq_rel)) {
            LogOut(std::string("[AUDIO][HOOK] contested vanilla entrypoints remain unowned: ")
                   + hostResolutionReason, true);
        }
        return HookInstallResult::Deferred;
    }

    LogOut(std::string("[AUDIO][HOOK] vanilla host ownership finalized: ")
           + hostResolutionReason, true);

    const bool playIsPristine = BytesMatch(playEntry, kVanillaPlaySoundBufferPrologue);
    const bool setVolumeIsPristine = BytesMatch(setVolumeEntry, kVanillaSetSoundVolumePrologue);
    if (!playIsPristine || !setVolumeIsPristine) {
        g_contestedHookState = HookGroupState::Suppressed;
        g_contestedHookOwner = ContestedHookOwner::External;
        g_runtimeAudioControlSuppressed.store(true, std::memory_order_release);
        g_volumeApplicationReady.store(false, std::memory_order_release);
        if (!g_audioHookConflictLogged.exchange(true, std::memory_order_acq_rel)) {
            std::ostringstream oss;
            oss << "[AUDIO][HOOK] rejected vanilla audio ownership: exact prologue mismatch"
                << " playSoundBuffer=\"" << FormatBytes(playEntry, kVanillaPlaySoundBufferPrologue.size()) << "\""
                << " setSoundVolume=\"" << FormatBytes(setVolumeEntry, kVanillaSetSoundVolumePrologue.size()) << "\"";
            LogOut(oss.str(), true);
        }
        LogRuntimeAudioSuppressedOnce("vanilla EFZ audio prologue validation failed or an external owner is present");
        return HookInstallResult::Suppressed;
    }

    // Close the ordinary load/probe window before MinHook writes either site.
    // Supported late-Revival launchers were already excluded above; this
    // second check also catches a module/capability that appeared while the
    // pristine snapshots were being validated.
    std::string hostRevalidationReason;
    if (GetModuleHandleA("EfzRevival.dll")
        || !CanFinalizeVanillaAudioOwnership(hostRevalidationReason)) {
        g_contestedHookState = HookGroupState::Deferred;
        return HookInstallResult::Deferred;
    }
    if (!BytesMatch(playEntry, kVanillaPlaySoundBufferPrologue)
        || !BytesMatch(setVolumeEntry, kVanillaSetSoundVolumePrologue)) {
        g_contestedHookState = HookGroupState::Deferred;
        return HookInstallResult::Deferred;
    }

    LogOut(std::string("[AUDIO][HOOK] validated vanilla playSoundBuffer prologue: ")
           + FormatBytes(playEntry, kVanillaPlaySoundBufferPrologue.size()), true);
    LogOut(std::string("[AUDIO][HOOK] validated vanilla setSoundVolume prologue: ")
           + FormatBytes(setVolumeEntry, kVanillaSetSoundVolumePrologue.size()), true);
    if (!InstallVanillaContestedAudioHooks(efzBase)) {
        g_contestedHookState = HookGroupState::Failed;
        if (!g_audioHookIncompleteLogged.exchange(true, std::memory_order_acq_rel)) {
            LogOut("[AUDIO] vanilla contested audio hook install failed transactionally", true);
        }
        return HookInstallResult::Failed;
    }

    g_contestedHookOwner = ContestedHookOwner::Vanilla;
    g_contestedHookState = HookGroupState::Installed;
    LogOut("[AUDIO] runtime audio hooks installed for validated vanilla EFZ", true);
    return HookInstallResult::Ready;
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
    // Calls through g_originalPlayBackgroundMusic use the trampoline and do not
    // re-enter HookedPlayBackgroundMusic.
    SetLastBgmTrack(trackNumber);
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
