#include "../../include/game/savestate_hook.h"
#include "../../include/game/efzrevival_addrs.h"
#include "../../include/game/auto_action.h"
#include "../../include/game/character_settings.h"
#include "../../include/game/combo_overlay.h"
#include "../../include/game/macro_controller.h"
#include "../../include/game/practice_offsets.h"
#include "../../include/utils/switch_players.h"
#include "../../include/core/logger.h"
#include "../../include/core/memory.h"
#include "../../include/core/constants.h"
#include "../../include/gui/overlay.h"
#include "../../include/utils/utilities.h"
#include "../../3rdparty/minhook/include/MinHook.h"
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <sstream>

namespace {
    std::atomic<bool> s_installed{false};
    std::atomic<unsigned int> s_saveCount{0};
    std::atomic<unsigned int> s_loadCount{0};

    uintptr_t s_loadStateAddr = 0;
    uintptr_t s_saveStateAddr = 0;

    using LoadStateFn = bool (__thiscall*)(void* self);
    LoadStateFn oLoadState = nullptr;

    using SaveStateFn = void (__thiscall*)(void* self);
    SaveStateFn oSaveState = nullptr;

    struct SavedModState {
        bool valid = false;
        bool p2ControlWasOverridden = false;
        uint32_t originalP2ControlFlag = 1;
        uint8_t p1CpuFlag = 0;
        uint8_t p2CpuFlag = 1;
        int localSide = 0;
        MacroController::State macroState = MacroController::State::Idle;
        int macroSlot = 1;
    };

    SavedModState s_savedModState{};

    void CanonicalizeSavedControlState(SavedModState& state) {
        if (state.localSide != 0 && state.localSide != 1) {
            state.localSide = 0;
        }
        state.p1CpuFlag = static_cast<uint8_t>((state.localSide == 1) ? 1u : 0u);
        state.p2CpuFlag = static_cast<uint8_t>((state.localSide == 1) ? 0u : 1u);
    }

    void CaptureModState() {
        s_savedModState.valid = true;
        s_savedModState.p2ControlWasOverridden = g_p2ControlOverridden;
        s_savedModState.originalP2ControlFlag = g_originalP2ControlFlag;

        s_savedModState.localSide = SwitchPlayers::GetLocalSide();
        if (s_savedModState.localSide < 0) {
            s_savedModState.localSide = 0;
        }

        uintptr_t base = GetEFZBase();
        if (base) {
            uintptr_t gameStatePtr = 0;
            if (SafeReadMemory(base + EFZ_BASE_OFFSET_GAME_STATE, &gameStatePtr, sizeof(gameStatePtr)) && gameStatePtr) {
                SafeReadMemory(gameStatePtr + GAMESTATE_OFF_P1_CPU_FLAG, &s_savedModState.p1CpuFlag, sizeof(uint8_t));
                SafeReadMemory(gameStatePtr + GAMESTATE_OFF_P2_CPU_FLAG, &s_savedModState.p2CpuFlag, sizeof(uint8_t));
            }
        }
        CanonicalizeSavedControlState(s_savedModState);

        s_savedModState.macroState = MacroController::GetState();
        s_savedModState.macroSlot = MacroController::GetCurrentSlot();

        std::ostringstream oss;
        oss << "[SAVESTATE][REVIVAL] Captured mod state: p2Override=" << (s_savedModState.p2ControlWasOverridden ? "true" : "false")
            << " origP2Flag=" << s_savedModState.originalP2ControlFlag
            << " localSide=" << s_savedModState.localSide
            << " p1Cpu=" << static_cast<int>(s_savedModState.p1CpuFlag)
            << " p2Cpu=" << static_cast<int>(s_savedModState.p2CpuFlag)
            << " macroState=" << static_cast<int>(s_savedModState.macroState)
            << " macroSlot=" << s_savedModState.macroSlot;
        LogOut(oss.str(), true);
    }

    void RestoreModState() {
        if (!s_savedModState.valid) {
            LogOut("[SAVESTATE][REVIVAL] No valid mod state to restore", true);
            return;
        }

        CanonicalizeSavedControlState(s_savedModState);
        g_p2ControlOverridden = s_savedModState.p2ControlWasOverridden;
        g_originalP2ControlFlag = s_savedModState.originalP2ControlFlag;

        int currentSide = SwitchPlayers::GetLocalSide();
        if (s_savedModState.localSide >= 0) {
            LogOut("[SAVESTATE][REVIVAL] Restoring local side from " + std::to_string(currentSide) + " to " + std::to_string(s_savedModState.localSide), true);
            if (!SwitchPlayers::ReapplyLocalSide(s_savedModState.localSide)
                && !SwitchPlayers::RestoreEngineControlState(s_savedModState.localSide,
                                                             s_savedModState.p1CpuFlag,
                                                             s_savedModState.p2CpuFlag)) {
                if (s_savedModState.localSide == 1) {
                    SwitchPlayers::MarkSwapped();
                } else {
                    SwitchPlayers::ClearSwapFlag();
                }
            }
        }

        uintptr_t base = GetEFZBase();
        if (base) {
            uintptr_t gameStatePtr = 0;
            if (SafeReadMemory(base + EFZ_BASE_OFFSET_GAME_STATE, &gameStatePtr, sizeof(gameStatePtr)) && gameStatePtr) {
                const uint8_t activePlayer = static_cast<uint8_t>(s_savedModState.localSide);
                SafeWriteMemory(gameStatePtr + GAMESTATE_OFF_ACTIVE_PLAYER, &activePlayer, sizeof(activePlayer));
                SafeWriteMemory(gameStatePtr + GAMESTATE_OFF_P1_CPU_FLAG, &s_savedModState.p1CpuFlag, sizeof(uint8_t));
                SafeWriteMemory(gameStatePtr + GAMESTATE_OFF_P2_CPU_FLAG, &s_savedModState.p2CpuFlag, sizeof(uint8_t));
            }
        }

        std::ostringstream oss;
        oss << "[SAVESTATE][REVIVAL] Restored mod state: p2Override=" << (g_p2ControlOverridden ? "true" : "false")
            << " origP2Flag=" << g_originalP2ControlFlag
            << " localSide=" << s_savedModState.localSide
            << " p1Cpu=" << static_cast<int>(s_savedModState.p1CpuFlag)
            << " p2Cpu=" << static_cast<int>(s_savedModState.p2CpuFlag);
        LogOut(oss.str(), true);
    }

    bool __fastcall HookedLoadState(void* self, void* /*edx*/) {
        if (g_onlineModeActive.load(std::memory_order_relaxed)) {
            return oLoadState ? oLoadState(self) : false;
        }

        LogOut("[SAVESTATE][REVIVAL] === LOAD STATE BEGIN ===", true);
        CancelAutoActionsAndMacros();

        bool result = oLoadState ? oLoadState(self) : false;
        s_loadCount.fetch_add(1, std::memory_order_relaxed);

        RestoreModState();
        ComboOverlay::ResetState("revival savestate load");
        if (uintptr_t base = GetEFZBase()) {
            CharacterSettings::TickCharacterEnforcements(base, displayData);
        }

        LogOut("[SAVESTATE][REVIVAL] === LOAD STATE END (result=" + std::string(result ? "true" : "false") + ") ===", true);
        DirectDrawHook::AddMessage("Revival State Loaded", "savestate", RGB(100, 255, 100), 1500, 0, 100);
        return result;
    }

    void __fastcall HookedSaveState(void* self, void* /*edx*/) {
        if (g_onlineModeActive.load(std::memory_order_relaxed)) {
            if (oSaveState) {
                oSaveState(self);
            }
            return;
        }

        LogOut("[SAVESTATE][REVIVAL] === SAVE STATE BEGIN ===", true);
        CaptureModState();

        if (oSaveState) {
            oSaveState(self);
        }
        s_saveCount.fetch_add(1, std::memory_order_relaxed);

        LogOut("[SAVESTATE][REVIVAL] === SAVE STATE END ===", true);
        DirectDrawHook::AddMessage("Revival State Saved", "savestate", RGB(255, 255, 100), 1500, 0, 100);
    }
}

namespace SavestateHook {
    bool Install() {
        if (s_installed.load()) return true;

        HMODULE mod = GetModuleHandleA("EfzRevival.dll");
        if (!mod) {
            LogOut("[SAVESTATE][REVIVAL] EfzRevival.dll not loaded; cannot install hooks", true);
            return false;
        }

        uintptr_t loadRva = EFZ_RVA_LoadState();
        uintptr_t saveRva = EFZ_RVA_SaveState();

        if (!loadRva || !saveRva) {
            LogOut("[SAVESTATE][REVIVAL] Unsupported EfzRevival version; hooks not available", true);
            return false;
        }

        uintptr_t base = reinterpret_cast<uintptr_t>(mod);
        s_loadStateAddr = base + loadRva;
        s_saveStateAddr = base + saveRva;

        bool loadHookOk = false;
        bool saveHookOk = false;

        if (MH_CreateHook(reinterpret_cast<LPVOID>(s_loadStateAddr),
                          reinterpret_cast<LPVOID>(&HookedLoadState),
                          reinterpret_cast<void**>(&oLoadState)) == MH_OK) {
            if (MH_EnableHook(reinterpret_cast<LPVOID>(s_loadStateAddr)) == MH_OK) {
                loadHookOk = true;
            } else {
                MH_RemoveHook(reinterpret_cast<LPVOID>(s_loadStateAddr));
            }
        }

        if (MH_CreateHook(reinterpret_cast<LPVOID>(s_saveStateAddr),
                          reinterpret_cast<LPVOID>(&HookedSaveState),
                          reinterpret_cast<void**>(&oSaveState)) == MH_OK) {
            if (MH_EnableHook(reinterpret_cast<LPVOID>(s_saveStateAddr)) == MH_OK) {
                saveHookOk = true;
            } else {
                MH_RemoveHook(reinterpret_cast<LPVOID>(s_saveStateAddr));
            }
        }

        if (!loadHookOk && !saveHookOk) {
            LogOut("[SAVESTATE][REVIVAL] Failed to install any hooks", true);
            return false;
        }

        s_installed.store(true);
        std::ostringstream oss;
        oss << "[SAVESTATE][REVIVAL] Hooks installed - Load: " << (loadHookOk ? "OK" : "FAILED")
            << " (RVA=0x" << std::hex << loadRva << ")"
            << ", Save: " << (saveHookOk ? "OK" : "FAILED")
            << " (RVA=0x" << std::hex << saveRva << ")";
        LogOut(oss.str(), true);
        return true;
    }

    void Uninstall() {
        if (!s_installed.load()) return;

        if (s_loadStateAddr) {
            MH_DisableHook(reinterpret_cast<LPVOID>(s_loadStateAddr));
            MH_RemoveHook(reinterpret_cast<LPVOID>(s_loadStateAddr));
            s_loadStateAddr = 0;
        }

        if (s_saveStateAddr) {
            MH_DisableHook(reinterpret_cast<LPVOID>(s_saveStateAddr));
            MH_RemoveHook(reinterpret_cast<LPVOID>(s_saveStateAddr));
            s_saveStateAddr = 0;
        }

        s_installed.store(false);
        LogOut("[SAVESTATE][REVIVAL] Hooks uninstalled", true);
    }

    bool IsInstalled() {
        return s_installed.load();
    }

    unsigned int GetSaveCount() {
        return s_saveCount.load();
    }

    unsigned int GetLoadCount() {
        return s_loadCount.load();
    }
}
