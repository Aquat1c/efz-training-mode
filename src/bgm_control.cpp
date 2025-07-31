#include "../include/bgm_control.h"
#include "../include/logger.h"
#include "../include/memory.h"
#include "../include/constants.h"

// Offsets
constexpr uintptr_t BGM_SLOT_OFFSET   = 0xF26;
constexpr uintptr_t SOUND_MANAGER_PTR = 0x8;

// Typedefs for game's functions
typedef int   (__thiscall* StopBGMFunc)(uintptr_t gameSystemPtr);
typedef void  (__thiscall* PlayBGMFunc)(uintptr_t gameSystemPtr, unsigned short trackNumber);

// Helper to get efz.exe base (implement this if not present)
extern uintptr_t GetEFZBase();

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