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
    // Hook state
    std::atomic<bool> s_installed{false};
    std::atomic<unsigned int> s_saveCount{0};
    std::atomic<unsigned int> s_loadCount{0};

    // Target addresses
    uintptr_t s_loadStateAddr = 0;
    uintptr_t s_saveStateAddr = 0;

    // Original function pointers
    // VS/Practice mode Load State: bool __thiscall sub_10075910(int *this)
    using LoadStateFn = bool (__thiscall*)(void* self);
    LoadStateFn oLoadState = nullptr;

    // VS/Practice mode Save State: void __thiscall sub_10075980(int this)
    using SaveStateFn = void (__thiscall*)(void* self);
    SaveStateFn oSaveState = nullptr;

    // =========================================================================
    // Captured mod state at savestate time
    // =========================================================================
    struct SavedModState {
        bool valid = false;
        
        // Control state
        bool p2ControlWasOverridden = false;
        uint32_t originalP2ControlFlag = 1;
        
        // CPU flags at game state level
        uint8_t p1CpuFlag = 0;
        uint8_t p2CpuFlag = 1;
        
        // Local side (which player is being controlled: 0=P1, 1=P2)
        int localSide = 0;
        
        // Macro state
        MacroController::State macroState = MacroController::State::Idle;
        int macroSlot = 1;
    };
    
    SavedModState s_savedModState{};

    // =========================================================================
    // Capture current mod state (called on save)
    // =========================================================================
    void CaptureModState() {
        s_savedModState.valid = true;
        
        // Capture P2 control override state
        s_savedModState.p2ControlWasOverridden = g_p2ControlOverridden;
        s_savedModState.originalP2ControlFlag = g_originalP2ControlFlag;
        
        // Capture local side (which player is being controlled)
        s_savedModState.localSide = SwitchPlayers::GetLocalSide();
        if (s_savedModState.localSide < 0) s_savedModState.localSide = 0; // default to P1 if unable to read
        
        // Capture CPU flags from game state
        uintptr_t base = GetEFZBase();
        if (base) {
            uintptr_t gameStatePtr = 0;
            if (SafeReadMemory(base + EFZ_BASE_OFFSET_GAME_STATE, &gameStatePtr, sizeof(gameStatePtr)) && gameStatePtr) {
                SafeReadMemory(gameStatePtr + GAMESTATE_OFF_P1_CPU_FLAG, &s_savedModState.p1CpuFlag, sizeof(uint8_t));
                SafeReadMemory(gameStatePtr + GAMESTATE_OFF_P2_CPU_FLAG, &s_savedModState.p2CpuFlag, sizeof(uint8_t));
            }
        }
        
        // Capture macro state
        s_savedModState.macroState = MacroController::GetState();
        s_savedModState.macroSlot = MacroController::GetCurrentSlot();
        
        std::ostringstream oss;
        oss << "[SAVESTATE] Captured mod state: p2Override=" << (s_savedModState.p2ControlWasOverridden ? "true" : "false")
            << " origP2Flag=" << s_savedModState.originalP2ControlFlag
            << " localSide=" << s_savedModState.localSide
            << " p1Cpu=" << (int)s_savedModState.p1CpuFlag
            << " p2Cpu=" << (int)s_savedModState.p2CpuFlag
            << " macroState=" << (int)s_savedModState.macroState
            << " macroSlot=" << s_savedModState.macroSlot;
        LogOut(oss.str(), true);
    }

    // =========================================================================
    // Restore captured mod state (called after load)
    // =========================================================================
    void RestoreModState() {
        if (!s_savedModState.valid) {
            LogOut("[SAVESTATE] No valid mod state to restore", true);
            return;
        }
        
        // Restore P2 control tracking state
        // Note: The actual game memory will be restored by the savestate itself,
        // but we need to sync our tracking variables
        g_p2ControlOverridden = s_savedModState.p2ControlWasOverridden;
        g_originalP2ControlFlag = s_savedModState.originalP2ControlFlag;
        
        // Restore local side (which player is being controlled)
        int currentSide = SwitchPlayers::GetLocalSide();
        if (currentSide != s_savedModState.localSide && s_savedModState.localSide >= 0) {
            LogOut("[SAVESTATE] Restoring local side from " + std::to_string(currentSide) + " to " + std::to_string(s_savedModState.localSide), true);
            SwitchPlayers::SetLocalSide(s_savedModState.localSide);
        }
        
        // Restore CPU flags to what they were when saved
        // This handles cases where an auto-action modified them before loading
        uintptr_t base = GetEFZBase();
        if (base) {
            uintptr_t gameStatePtr = 0;
            if (SafeReadMemory(base + EFZ_BASE_OFFSET_GAME_STATE, &gameStatePtr, sizeof(gameStatePtr)) && gameStatePtr) {
                SafeWriteMemory(gameStatePtr + GAMESTATE_OFF_P1_CPU_FLAG, &s_savedModState.p1CpuFlag, sizeof(uint8_t));
                SafeWriteMemory(gameStatePtr + GAMESTATE_OFF_P2_CPU_FLAG, &s_savedModState.p2CpuFlag, sizeof(uint8_t));
            }
        }
        
        std::ostringstream oss;
        oss << "[SAVESTATE] Restored mod state: p2Override=" << (g_p2ControlOverridden ? "true" : "false")
            << " origP2Flag=" << g_originalP2ControlFlag
            << " localSide=" << s_savedModState.localSide
            << " p1Cpu=" << (int)s_savedModState.p1CpuFlag
            << " p2Cpu=" << (int)s_savedModState.p2CpuFlag;
        LogOut(oss.str(), true);
    }

    // Hooked Load State function (VS/Practice mode)
    bool __fastcall HookedLoadState(void* self, void* /*edx*/) {
        if (g_onlineModeActive.load(std::memory_order_relaxed)) {
            return oLoadState ? oLoadState(self) : false;
        }

        LogOut("[SAVESTATE] === LOAD STATE BEGIN ===", true);
        
        // Cancel any active auto-actions/macros BEFORE loading state
        // This ensures clean state when the savestate is restored
        CancelAutoActionsAndMacros();
        
        bool result = oLoadState ? oLoadState(self) : false;
        s_loadCount.fetch_add(1, std::memory_order_relaxed);
        
        // Restore our captured mod state after the game state is loaded
        RestoreModState();
        ComboOverlay::ResetState("savestate load");
        if (uintptr_t base = GetEFZBase()) {
            CharacterSettings::TickCharacterEnforcements(base, displayData);
        }
        
        LogOut("[SAVESTATE] === LOAD STATE END (result=" + std::string(result ? "true" : "false") + ") ===", true);
        
        // Display message at same position as Position Loaded (0, 100)
        DirectDrawHook::AddMessage("State Loaded", "savestate", RGB(100, 255, 100), 1500, 0, 100);
        
        return result;
    }

    // Hooked Save State function (VS/Practice mode)
    void __fastcall HookedSaveState(void* self, void* /*edx*/) {
        if (g_onlineModeActive.load(std::memory_order_relaxed)) {
            if (oSaveState) oSaveState(self);
            return;
        }

        LogOut("[SAVESTATE] === SAVE STATE BEGIN ===", true);
        
        // Capture our mod state before the game saves its state
        CaptureModState();
        
        if (oSaveState) oSaveState(self);
        s_saveCount.fetch_add(1, std::memory_order_relaxed);
        
        LogOut("[SAVESTATE] === SAVE STATE END ===", true);
        
        // Display message at same position as Position Saved (0, 100)
        DirectDrawHook::AddMessage("State Saved", "savestate", RGB(255, 255, 100), 1500, 0, 100);
    }
}

namespace SavestateHook {
    bool Install() {
        if (s_installed.load()) return true;

        // Check if EfzRevival.dll is loaded
        HMODULE mod = GetModuleHandleA("EfzRevival.dll");
        if (!mod) {
            LogOut("[SAVESTATE] EfzRevival.dll not loaded; cannot install hooks", true);
            return false;
        }

        // Get RVAs for save/load state functions
        uintptr_t loadRva = EFZ_RVA_LoadState();
        uintptr_t saveRva = EFZ_RVA_SaveState();

        if (!loadRva || !saveRva) {
            LogOut("[SAVESTATE] Unsupported EfzRevival version; savestate hooks not available", true);
            return false;
        }

        // Calculate absolute addresses
        uintptr_t base = reinterpret_cast<uintptr_t>(mod);
        s_loadStateAddr = base + loadRva;
        s_saveStateAddr = base + saveRva;

        // Create hooks
        bool loadHookOk = false;
        bool saveHookOk = false;

        // Hook Load State
        if (MH_CreateHook(reinterpret_cast<LPVOID>(s_loadStateAddr),
                         reinterpret_cast<LPVOID>(&HookedLoadState),
                         reinterpret_cast<void**>(&oLoadState)) == MH_OK) {
            if (MH_EnableHook(reinterpret_cast<LPVOID>(s_loadStateAddr)) == MH_OK) {
                loadHookOk = true;
            } else {
                MH_RemoveHook(reinterpret_cast<LPVOID>(s_loadStateAddr));
            }
        }

        // Hook Save State
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
            LogOut("[SAVESTATE] Failed to install any savestate hooks", true);
            return false;
        }

        s_installed.store(true);

        // Log success
        std::ostringstream oss;
        oss << "[SAVESTATE] Hooks installed - Load: " << (loadHookOk ? "OK" : "FAILED")
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
        LogOut("[SAVESTATE] Hooks uninstalled", true);
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
