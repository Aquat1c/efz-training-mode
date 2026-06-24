#include "../../include/game/practice_hotkey_gate.h"
#include "../../include/game/practice_offsets.h"
#include "../../include/game/efzrevival_addrs.h" // version-aware RVAs
#include "../../include/core/logger.h"
#include "../../include/core/constants.h"
#include "../../include/core/memory.h"
#include "../../include/input/framestep.h"
#include "../../include/utils/pause_integration.h"
#include "../../3rdparty/minhook/include/MinHook.h"
#include <windows.h>
#include <atomic>
#include <mutex>
#include <sstream>
#include <string>

static std::atomic<bool> s_menuVisibleForGate{false};
static bool Gate_IsMenuVisible() { return s_menuVisibleForGate.load(std::memory_order_relaxed); }

namespace {
    static std::string ToHex(uint32_t v){ std::ostringstream oss; oss<<std::hex<<v; return oss.str(); }
    // Target is a method (original __thiscall). It compares incoming key (a2) against configured hotkeys.
    // Prototype: char/bool return, takes (this, int a2). We detour as __fastcall and forward correctly.
    using HotkeyEvalFn = char (__thiscall*)(void* self, int a2);
    HotkeyEvalFn oHotkeyEval = nullptr;
    std::atomic<bool> s_installed{false};
    std::atomic<uint64_t> s_suppressedFrames{0};
    uintptr_t s_evalAddr = 0;

    // Forward declaration of scanner (fallback). Returns 0 if not found.
    uintptr_t ScanForHotkeyEvaluator();

    char __fastcall HookedHotkeyEval(void* self, void* /*edx*/, int a2) {
        PauseIntegration::NotePracticeControllerCandidate(self, "PracticeDispatcher");
        if (Gate_IsMenuVisible()) {
            // Suppress all practice hotkey side-effects this frame
            s_suppressedFrames.fetch_add(1, std::memory_order_relaxed);
            return 0; // early exit, indicate not handled
        }
        // Custom savestate backend removed — Practice save/load hotkeys fall
        // through to EfzRevival's native handler below.
        if (Framestep::ShouldSuppressRevivalHotkey(self, a2)) {
            s_suppressedFrames.fetch_add(1, std::memory_order_relaxed);
            return 0;
        }
        return oHotkeyEval ? oHotkeyEval(self, a2) : 0;
    }

    uintptr_t ResolveHotkeyEvaluatorRva() {
        HMODULE mod = GetModuleHandleA("EfzRevival.dll");
        if (!mod) return 0;
        // Version-aware fast path: use dispatcher RVA per build
        uintptr_t rva = EFZ_RVA_PracticeDispatcher();
        if (rva) {
            uintptr_t candidate = reinterpret_cast<uintptr_t>(mod) + rva;
            uint8_t firstBytes[5] = {0};
            if (SafeReadMemory(candidate, firstBytes, sizeof(firstBytes))) {
                // Accept if readable; additional signature checks can be added if needed
                return candidate;
            }
        }
        // If version-aware lookup declines, do not guess a legacy RVA. The old
        // fast path can overlap unrelated code in split 1.02f builds.
        return ScanForHotkeyEvaluator();
    }

    uintptr_t ScanForHotkeyEvaluator() {
        // TODO: Implement signature scan if RVA drifts. For now just return 0 to fail gracefully.
        return 0;
    }
}

namespace PracticeHotkeyGate {
    bool Install() {
        if (s_installed.load()) return true;
        HMODULE mod = GetModuleHandleA("EfzRevival.dll");
        if (!mod) {
            LogOut("[HOTKEY] EfzRevival not yet loaded; cannot install gate", true);
            return false;
        }
        s_evalAddr = ResolveHotkeyEvaluatorRva();
        if (!s_evalAddr) {
            LogOut("[HOTKEY] Failed to resolve Practice hotkey evaluator; gate inactive", true);
            return false;
        }
        if (MH_CreateHook(reinterpret_cast<LPVOID>(s_evalAddr), reinterpret_cast<LPVOID>(&HookedHotkeyEval), reinterpret_cast<void**>(&oHotkeyEval)) != MH_OK) {
            LogOut("[HOTKEY] CreateHook failed for evaluator", true);
            return false;
        }
        if (MH_EnableHook(reinterpret_cast<LPVOID>(s_evalAddr)) != MH_OK) {
            LogOut("[HOTKEY] EnableHook failed for evaluator", true);
            MH_RemoveHook(reinterpret_cast<LPVOID>(s_evalAddr));
            return false;
        }
        s_installed.store(true);
        {
            std::ostringstream oss; oss << "[HOTKEY] Practice hotkey gate installed at RVA=0x" 
                << std::hex << static_cast<uint32_t>(s_evalAddr - reinterpret_cast<uintptr_t>(mod));
            LogOut(oss.str(), true);
        }
        return true;
    }

    void Uninstall() {
        if (!s_installed.load()) return;
        if (s_evalAddr) {
            MH_DisableHook(reinterpret_cast<LPVOID>(s_evalAddr));
            MH_RemoveHook(reinterpret_cast<LPVOID>(s_evalAddr));
        }
        s_installed.store(false);
    }

    uint64_t GetSuppressedFrameCount() { return s_suppressedFrames.load(); }
    void NotifyMenuVisibility(bool visible) { s_menuVisibleForGate.store(visible, std::memory_order_relaxed); }
}
