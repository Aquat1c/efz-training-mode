#include "../include/utils/bgm_control.h"
#include "../include/core/logger.h"
#include "../include/core/memory.h"
#include "../include/core/constants.h"
#include "../include/gui/overlay.h"
#include "../include/core/globals.h"  
#include "../include/utils/audio_control.h"
#include "../3rdparty/minhook/include/MinHook.h"
#include <thread>
#include <chrono>
#include <atomic>

// Offsets
constexpr uintptr_t BGM_SLOT_OFFSET   = 0xF26;
constexpr uintptr_t SOUND_MANAGER_PTR = 0x8;

// Typedefs for game's functions
typedef int   (__thiscall* StopBGMFunc)(uintptr_t gameSystemPtr);
typedef void  (__thiscall* PlayBGMFunc)(uintptr_t gameSystemPtr, unsigned short trackNumber);
static PlayBGMFunc oPlayBGM = nullptr;

// Helper to get efz.exe base (implement this if not present)
extern uintptr_t GetEFZBase();

static std::atomic<unsigned int> g_lastBgmTrack{0xFFFFu};

bool StopBGM(uintptr_t gameSystemPtr) {
    if (!gameSystemPtr) return false;
    uintptr_t efzBase = GetEFZBase();
    if (!efzBase) {
        LogOut("[BGM] Could not get EFZ base address!", true);
        return false;
    }
    StopBGMFunc stopBGM = (StopBGMFunc)(efzBase + 0x6A10); // 0x406A10 RVA
    LogOut("[BGM] Calling game's stopBackgroundMusic...", true);
    stopBGM(gameSystemPtr);
    // Track 150 is EFZ's established "OFF" selection. Preserve silence as a
    // reproducible logical presentation state instead of leaving a stale track.
    SetLastBgmTrack(150);
    LogOut("[BGM] Called stopBackgroundMusic.", true);
    return true;
}

bool PlayBGM(uintptr_t gameSystemPtr, unsigned short trackNumber) {
    if (!gameSystemPtr) return false;
    LogOut("[BGM] Calling game's playBackgroundMusic with track " + std::to_string(trackNumber), true);
    if (!AudioControl::PlayBackgroundMusic(gameSystemPtr, trackNumber)) {
        LogOut("[BGM] playBackgroundMusic request failed for track " + std::to_string(trackNumber), true);
        return false;
    }
    SetLastBgmTrack(trackNumber);
    LogOut("[BGM] playBackgroundMusic request succeeded.", true);
    return true;
}

int GetBGMBufferIndex(uintptr_t gameStatePtr) {
    uint16_t slot = 0;
    SafeReadMemory(gameStatePtr + 0xF26, &slot, sizeof(uint16_t));
    return static_cast<int>(slot);
}

int GetBGMSlot(uintptr_t gameStatePtr) {
    return GetBGMBufferIndex(gameStatePtr);
}

int GetBGMVolume(uintptr_t gameStatePtr) {
    int vol = 0;
    SafeReadMemory(gameStatePtr + 0xF2C, &vol, sizeof(int));
    return vol;
}

// ToggleBGM removed (unused legacy API).

// Our hook function
void __fastcall HookedPlayBGM(uintptr_t gameSystemPtr, void*, unsigned short trackNumber) {
    SetLastBgmTrack(trackNumber);
    
    // Add safety check
    if (!gameSystemPtr) {
        LogOut("[BGM] Invalid gameSystemPtr in HookedPlayBGM, bypassing hook", true);
        if (oPlayBGM) oPlayBGM(gameSystemPtr, trackNumber);
        return;
    }
    
    // Call original
    if (oPlayBGM) {
        oPlayBGM(gameSystemPtr, trackNumber);
    } else {
        LogOut("[BGM] Original playBackgroundMusic function pointer is null!", true);
    }
}

bool InstallBGMHook(uintptr_t efzBase) {
    if (!efzBase) return false;
    void* target = (void*)(efzBase + 0x68B0); // 0x4068B0 RVA for playBackgroundMusic
    if (MH_CreateHook(target, &HookedPlayBGM, reinterpret_cast<void**>(&oPlayBGM)) != MH_OK) {
        LogOut("[BGM] Failed to create playBackgroundMusic hook", true);
        return false;
    }
    if (MH_EnableHook(target) != MH_OK) {
        LogOut("[BGM] Failed to enable playBackgroundMusic hook", true);
        return false;
    }
    LogOut("[BGM] playBackgroundMusic hook installed", true);
    return true;
}

// Suppression interface removed; stubs retained for ABI compatibility.
void SetBGMSuppressed(bool /*suppress*/) {}
bool IsBGMSuppressed() { return false; }

unsigned short GetLastBgmTrack() {
    return static_cast<unsigned short>(g_lastBgmTrack.load(std::memory_order_acquire));
}

void SetLastBgmTrack(unsigned short track) {
    g_lastBgmTrack.store(track, std::memory_order_release);
}

// Set BGM volume using the game's internal setSoundVolume function
bool SetBGMVolumeViaGame(uintptr_t gameSystemPtr, int volumeLevel) {
    if (!gameSystemPtr) return false;
    uintptr_t efzBase = GetEFZBase();
    if (!efzBase) return false;

    // Get sound manager and buffer index
    void* soundManagerPtr = *(void**)(gameSystemPtr + 0x8);
    unsigned short bufferIndex = *(unsigned short*)(gameSystemPtr + 0xF26);
    if (!soundManagerPtr || bufferIndex == 150) return false;

    typedef int (__thiscall* SetSoundVolumeFunc)(void* soundManagerPtr, unsigned short bufferIndex, int volumeLevel);
    SetSoundVolumeFunc setSoundVolume = (SetSoundVolumeFunc)(efzBase + 0xE5D0); // 0x40E5D0 RVA

    int result = setSoundVolume(soundManagerPtr, bufferIndex, volumeLevel);
    LogOut("[BGM] Set volume via game function: buffer=" + std::to_string(bufferIndex) + ", vol=" + std::to_string(volumeLevel) + ", result=" + std::to_string(result), true);
    return result == 0;
}

// Poller APIs removed; keep empty functions for binary compatibility.
void StartBGMSuppressionPoller() {}
void StopBGMSuppressionPoller() {}
