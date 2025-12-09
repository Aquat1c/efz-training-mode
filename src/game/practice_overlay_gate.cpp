#include "../../include/game/practice_offsets.h"
#include "../../include/game/efzrevival_addrs.h" // future: version-aware toggles
#include "../../include/core/logger.h"
#include "../../include/game/practice_hotkey_gate.h" // for menu visibility notification if needed
#include "../../include/utils/network.h" // IsEfzRevivalVersionSupported
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

    void __stdcall HookedToggleHurt(void* self) { if (MenuVisible()) return; if (oToggleHurt) oToggleHurt(self); }
    void __stdcall HookedToggleHit (void* self) { if (MenuVisible()) return; if (oToggleHit)  oToggleHit(self); }
    void __stdcall HookedToggleDisp(void* self) { if (MenuVisible()) return; if (oToggleDisp) oToggleDisp(self); }

    void InstallOverlayHooksInternal() {
        if (g_overlayHooksInstalled.load()) return;
        HMODULE hRev = GetModuleHandleA("EfzRevival.dll");
        if (!hRev) return;
        
        // Log version detection for debugging
        EfzRevivalVersion ver = GetEfzRevivalVersion();
        bool supported = IsEfzRevivalVersionSupported();
        char buf[256];
        snprintf(buf, sizeof(buf), "[HOTKEY] Overlay gate: version=%s supported=%d", 
                 EfzRevivalVersionName(ver), supported);
        LogOut(buf, true);
        
        // DISABLED: Hitbox/hurtbox/frame display hooks cause crashes on all Revival versions
        LogOut("[HOTKEY] Overlay hooks disabled (hitbox/hurtbox/frame display hooks cause crashes)", true);
        return;
        
        // Only install hooks for supported Revival versions - unsupported versions have wrong RVAs
        if (!supported) {
            LogOut("[HOTKEY] Overlay hooks skipped for unsupported Revival version", true);
            return;
        }
        
        // Get version-aware RVAs
        uintptr_t hurtRva = EFZ_RVA_ToggleHurtboxDisplay();
        uintptr_t hitRva = EFZ_RVA_ToggleHitboxDisplay();
        uintptr_t dispRva = EFZ_RVA_ToggleFrameDisplay();
        
        // Check if any RVAs are missing (0 means not found for this version)
        if (!hurtRva || !hitRva || !dispRva) {
            snprintf(buf, sizeof(buf), "[HOTKEY] Overlay hooks skipped (missing RVAs: hurt=%#x hit=%#x disp=%#x)", 
                     hurtRva, hitRva, dispRva);
            LogOut(buf, true);
            return;
        }
        
        int installed = 0;
        if (MakeHook(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(hRev)+hurtRva), &HookedToggleHurt, &oToggleHurt)) { ++installed; }
        if (MakeHook(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(hRev)+hitRva), &HookedToggleHit, &oToggleHit)) { ++installed; }
        if (MakeHook(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(hRev)+dispRva), &HookedToggleDisp, &oToggleDisp)) { ++installed; }
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
