#include "../../include/game/practice_hotkey_gate.h"
#include "../../include/game/practice_offsets.h"
#include "../../include/game/efzrevival_addrs.h" // version-aware RVAs
#include "../../include/game/collision_display.h"
#include "../../include/game/savestate_hook.h"
#include "../../include/game/mission/mission_engine.h"
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
    // J's MinGW dispatcher returns a pointer-sized value while legacy builds
    // only consume AL. Preserve the full EAX value; legacy callers still see
    // the same low byte.
    using HotkeyEvalFn = uintptr_t (__fastcall*)(void* self, void* edxValue, int a2);
    HotkeyEvalFn oHotkeyEval = nullptr;
    std::atomic<bool> s_installed{false};
    std::atomic<uint64_t> s_suppressedFrames{0};
    uintptr_t s_evalAddr = 0;

    // Forward declaration of scanner (fallback). Returns 0 if not found.
    uintptr_t ScanForHotkeyEvaluator();

    uintptr_t __fastcall HookedHotkeyEval(void* self, void* edxValue, int a2) {
        PauseIntegration::NotePracticeControllerCandidate(self, "PracticeDispatcher");
        if (Gate_IsMenuVisible() || Mission::Engine::Demo::IsActive() ||
            Mission::Engine::Recorder::OwnsCaptureHotkeys()) {
            // Suppress all practice hotkey side-effects this frame
            s_suppressedFrames.fetch_add(1, std::memory_order_relaxed);
            return 0; // early exit, indicate not handled
        }
        // Custom savestate backend removed - Practice save/load hotkeys fall
        // through to EfzRevival's native handler below.
        if (Framestep::ShouldSuppressRevivalHotkey(self, a2)) {
            s_suppressedFrames.fetch_add(1, std::memory_order_relaxed);
            return 0;
        }
        if (CollisionDisplay::ShouldSuppressRevivalHotkey(self, a2)) {
            s_suppressedFrames.fetch_add(1, std::memory_order_relaxed);
            return 0;
        }
        const uint8_t inlineActions = SavestateHook::BeginInlinePracticeHotkey(self, a2);
        // Preserve EDX as well as ECX/stack. Legacy __thiscall dispatchers
        // ignore it; J's MinGW body carries it through one auxiliary branch.
        const uintptr_t result = oHotkeyEval ? oHotkeyEval(self, edxValue, a2) : 0;
        SavestateHook::EndInlinePracticeHotkey(inlineActions);
        return result;
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
