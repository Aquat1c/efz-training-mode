#include "../../include/game/savestate_hook.h"
#include "../../include/game/efzrevival_addrs.h"
#include "../../include/core/logger.h"
#include "../../include/gui/overlay.h"
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

    // Hooked Load State function (VS/Practice mode)
    bool __fastcall HookedLoadState(void* self, void* /*edx*/) {
        bool result = oLoadState ? oLoadState(self) : false;
        s_loadCount.fetch_add(1, std::memory_order_relaxed);
        
        // Display message at same position as Position Loaded (0, 100)
        DirectDrawHook::AddMessage("State Loaded", "savestate", RGB(100, 255, 100), 1500, 0, 100);
        
        return result;
    }

    // Hooked Save State function (VS/Practice mode)
    void __fastcall HookedSaveState(void* self, void* /*edx*/) {
        if (oSaveState) oSaveState(self);
        s_saveCount.fetch_add(1, std::memory_order_relaxed);
        
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
