#include "../include/utils/bgm_control.h"
#include "../include/core/logger.h"
#include "../include/core/memory.h"
#include "../include/core/constants.h"
#include "../include/gui/overlay.h"
#include "../include/core/globals.h"  // Add this include
#include "../3rdparty/minhook/include/MinHook.h"
#include <atomic>
#include <thread>
#include <chrono>

// Offsets
constexpr uintptr_t BGM_SLOT_OFFSET   = 0xF26;
constexpr uintptr_t SOUND_MANAGER_PTR = 0x8;

// Typedefs for game's functions
typedef int   (__thiscall* StopBGMFunc)(uintptr_t gameSystemPtr);
typedef void  (__thiscall* PlayBGMFunc)(uintptr_t gameSystemPtr, unsigned short trackNumber);
static PlayBGMFunc oPlayBGM = nullptr;

// Helper to get efz.exe base (implement this if not present)
extern uintptr_t GetEFZBase();

static std::atomic<bool> g_bgmSuppressed{false};
static unsigned short g_lastBgmTrack = 0;

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
    LogOut("[BGM] Called stopBackgroundMusic.", true);
    return true;
}

bool PlayBGM(uintptr_t gameSystemPtr, unsigned short trackNumber) {
    if (!gameSystemPtr) return false;
    uintptr_t efzBase = GetEFZBase();
    if (!efzBase) {
        LogOut("[BGM] Could not get EFZ base address!", true);
        return false;
    }
    PlayBGMFunc playBGM = (PlayBGMFunc)(efzBase + 0x68B0); // 0x4068B0 RVA
    LogOut("[BGM] Calling game's playBackgroundMusic with track " + std::to_string(trackNumber), true);
    playBGM(gameSystemPtr, trackNumber);
    LogOut("[BGM] Called playBackgroundMusic.", true);
    return true;
}

int GetBGMSlot(uintptr_t gameStatePtr) {
    uint16_t slot = 0;
    SafeReadMemory(gameStatePtr + 0xF26, &slot, sizeof(uint16_t));
    return static_cast<int>(slot);
}

int GetBGMVolume(uintptr_t gameStatePtr) {
    int vol = 0;
    SafeReadMemory(gameStatePtr + 0xF2C, &vol, sizeof(int));
    return vol;
}

bool ToggleBGM(uintptr_t gameSystemPtr) {
    if (!gameSystemPtr) return false;
    if (!IsBGMSuppressed()) {
        // Save current slot before stopping
        g_lastBgmTrack = static_cast<unsigned short>(GetBGMSlot(gameSystemPtr));
        StopBGM(gameSystemPtr);
        LogOut("[BGM] Toggled OFF (stopped BGM)", true);
        DirectDrawHook::AddMessage("BGM: OFF", "SYSTEM", RGB(255, 100, 100), 1500, 0, 100);
    } else {
        // Resume BGM if we have a valid track
        if (g_lastBgmTrack != 150 && g_lastBgmTrack != 0) {
            PlayBGM(gameSystemPtr, g_lastBgmTrack);
            LogOut("[BGM] Toggled ON (resumed BGM, track " + std::to_string(g_lastBgmTrack) + ")", true);
            DirectDrawHook::AddMessage("BGM: ON", "SYSTEM", RGB(100, 255, 100), 1500, 0, 100);
        } else {
            LogOut("[BGM] Toggled ON but no previous track to resume.", true);
            DirectDrawHook::AddMessage("BGM: ON (no track)", "SYSTEM", RGB(255, 255, 100), 1500, 0, 100);
        }
    }
    return true;
}

// Our hook function
void __fastcall HookedPlayBGM(uintptr_t gameSystemPtr, void*, unsigned short trackNumber) {
    SetLastBgmTrack(trackNumber);
    
    // Add safety check
    if (!gameSystemPtr) {
        LogOut("[BGM] Invalid gameSystemPtr in HookedPlayBGM, bypassing hook", true);
        if (oPlayBGM) oPlayBGM(gameSystemPtr, trackNumber);
        return;
    }
    
    if (g_bgmSuppressed.load()) {
        LogOut("[BGM] playBackgroundMusic suppressed by toggle.", true);
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

void SetBGMSuppressed(bool suppress) {
    g_bgmSuppressed.store(suppress);
    if (suppress) {
        LogOut("[BGM] Global BGM suppression ENABLED", true);
        DirectDrawHook::AddMessage("BGM: OFF (global)", "SYSTEM", RGB(255, 100, 100), 1500, 0, 100);
    } else {
        LogOut("[BGM] Global BGM suppression DISABLED", true);
        DirectDrawHook::AddMessage("BGM: ON (global)", "SYSTEM", RGB(100, 255, 100), 1500, 0, 100);
    }
}
bool IsBGMSuppressed() { return g_bgmSuppressed.load(); }

unsigned short GetLastBgmTrack() {
    return g_lastBgmTrack;
}

void SetLastBgmTrack(unsigned short track) {
    g_lastBgmTrack = track;
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

// --- BGM Suppression Poller Thread ---
static std::atomic<bool> g_bgmPollerRunning{false};
static std::thread g_bgmPollerThread;

void BGMSuppressionPoller() {
    LogOut("[BGM] BGM suppression poller thread started", true);
    
    while (g_bgmPollerRunning.load() && !g_isShuttingDown.load()) {  // Check shutdown flag
        // If suppression isn't enabled, sleep longer and skip work
        if (!g_bgmSuppressed.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            continue;
        }

        uintptr_t efzBase = GetEFZBase();
        uintptr_t gameStatePtr = 0;
        if (efzBase && SafeReadMemory(efzBase + EFZ_BASE_OFFSET_GAME_STATE, &gameStatePtr, sizeof(uintptr_t)) && gameStatePtr) {
            StopBGM(gameStatePtr);
        }

        // Check shutdown more frequently and back off a bit between enforcement attempts
        for (int i = 0; i < 10 && g_bgmPollerRunning.load() && !g_isShuttingDown.load(); i++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    
    LogOut("[BGM] BGM suppression poller thread ending", true);
}

void StartBGMSuppressionPoller() {
    if (g_bgmPollerRunning.load()) return;
    g_bgmPollerRunning.store(true);
    g_bgmPollerThread = std::thread(BGMSuppressionPoller);
    g_bgmPollerThread.detach();
}

void StopBGMSuppressionPoller() {
    g_bgmPollerRunning.store(false);
    // No join needed since it's detached
}