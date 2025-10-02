#include "../../include/game/practice_hotkey_gate.h"
#include "../../include/game/practice_offsets.h"
#include "../../include/core/logger.h"
#include "../../include/core/constants.h"
#include "../../include/core/memory.h"
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
    using HotkeyEvalFn = void (__thiscall*)(void* self);
    HotkeyEvalFn oHotkeyEval = nullptr;
    std::atomic<bool> s_installed{false};
    std::atomic<uint64_t> s_suppressedFrames{0};
    uintptr_t s_evalAddr = 0;

    // Forward declaration of scanner (fallback). Returns 0 if not found.
    uintptr_t ScanForHotkeyEvaluator();

    void __fastcall HookedHotkeyEval(void* self, void* /*edx*/) {
        if (Gate_IsMenuVisible()) {
            // Suppress all practice hotkey side-effects this frame
            s_suppressedFrames.fetch_add(1, std::memory_order_relaxed);
            return; // early exit
        }
        if (oHotkeyEval) oHotkeyEval(self);
    }

    uintptr_t ResolveHotkeyEvaluatorRva() {
        HMODULE mod = GetModuleHandleA("EfzRevival.dll");
        if (!mod) return 0;
        // Fast path: use known RVA constant
        uintptr_t candidate = reinterpret_cast<uintptr_t>(mod) + static_cast<uintptr_t>(EFZREV_RVA_PRACTICE_HOTKEY_EVAL);
        // Basic sanity: attempt to read first bytes safely
        uint8_t firstBytes[5] = {0};
        if (SafeReadMemory(candidate, firstBytes, sizeof(firstBytes))) {
            // Heuristic: function should start with typical prologue 55 8B EC or push/ mov patterns.
            if (firstBytes[0] == 0x55 || firstBytes[0] == 0x8B || firstBytes[0] == 0x53) {
                return candidate;
            }
        }
        // Fallback: pattern scan (not fully implemented; stub for future upgrade)
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
