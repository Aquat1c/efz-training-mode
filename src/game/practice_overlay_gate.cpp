#include "../../include/game/practice_offsets.h"
#include "../../include/core/logger.h"
#include "../../include/game/practice_hotkey_gate.h" // for menu visibility notification if needed
#include "../../3rdparty/minhook/include/MinHook.h"
#include <windows.h>
#include <atomic>

namespace {
    std::atomic<bool> g_overlayHooksInstalled{false};
    std::atomic<bool> g_menuVisible{false};

    // Provided by existing gate
    extern void PracticeHotkeyGate::NotifyMenuVisibility(bool visible);

    bool MenuVisible() { return g_menuVisible.load(std::memory_order_relaxed); }

    template<typename T> bool MakeHook(void* target, void* detour, T** original) {
        if (!target) return false;
        if (MH_CreateHook(target, detour, reinterpret_cast<void**>(original)) != MH_OK) return false;
        if (MH_EnableHook(target) != MH_OK) { MH_RemoveHook(target); return false; }
        return true;
    }

    using ToggleFn = void(__thiscall*)(void* self);
    static ToggleFn oToggleHurt = nullptr;
    static ToggleFn oToggleHit  = nullptr;
    static ToggleFn oToggleDisp = nullptr; // We intentionally do NOT hook pause here; pause gating handled in pause_integration.

    void __fastcall HookedToggleHurt(void* self, void*) { if (MenuVisible()) return; if (oToggleHurt) oToggleHurt(self); }
    void __fastcall HookedToggleHit (void* self, void*) { if (MenuVisible()) return; if (oToggleHit)  oToggleHit(self); }
    void __fastcall HookedToggleDisp(void* self, void*) { if (MenuVisible()) return; if (oToggleDisp) oToggleDisp(self); }

    void InstallOverlayHooksInternal() {
        if (g_overlayHooksInstalled.load()) return;
        HMODULE hRev = GetModuleHandleA("EfzRevival.dll");
        if (!hRev) return;
        int installed = 0;
        if (MakeHook(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(hRev)+EFZREV_RVA_TOGGLE_HURTBOXES), &HookedToggleHurt, &oToggleHurt)) { ++installed; }
        if (MakeHook(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(hRev)+EFZREV_RVA_TOGGLE_HITBOXES), &HookedToggleHit, &oToggleHit)) { ++installed; }
        if (MakeHook(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(hRev)+EFZREV_RVA_TOGGLE_DISPLAY), &HookedToggleDisp, &oToggleDisp)) { ++installed; }
        // Pause toggle NOT hooked here; suppression handled via pause_integration's hook with internal bypass.
        if (installed) {
            g_overlayHooksInstalled.store(true);
            LogOut("[HOTKEY] Overlay/Pause toggle hooks installed (" + std::to_string(installed) + ")", true);
        }
    }
}

namespace PracticeOverlayGate {
    void SetMenuVisible(bool v) { g_menuVisible.store(v, std::memory_order_relaxed); }
    void EnsureInstalled() { InstallOverlayHooksInternal(); }
}
