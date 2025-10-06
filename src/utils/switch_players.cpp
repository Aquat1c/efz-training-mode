#include "../include/utils/switch_players.h"
#include "../include/core/logger.h"
#include "../include/core/memory.h"
#include "../include/game/game_state.h"
#include "../include/utils/pause_integration.h"
#include "../include/game/practice_offsets.h"
#include "../include/input/input_motion.h" // SetAIControlFlag
#include "../include/utils/utilities.h" // GetEFZBase
#include <windows.h>
#include <sstream>
#include <iomanip>

namespace {
    static bool s_disableMapReset = false; // Auto-latch off after first exception
    // SEH-safe wrappers (no C++ objects in scope) to call into EfzRevival without crashing the game
    static bool SehSafe_MapReset(bool(__thiscall* fn)(char**), char** mapPtr, bool* outOk) {
        if (!fn || !outOk) return false;
        __try {
            *outOk = fn(mapPtr);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    static bool SehSafe_CleanupPair(int(__thiscall* fn)(void*), void* ctx, int* outRc) {
        if (!fn || !ctx || !outRc) return false;
        __try {
            *outRc = fn(ctx);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }
    // Small helpers for consistent logging of memory writes
    template <typename T>
    static void LogRW(const char* label, uintptr_t addr, const T& valueToWrite) {
        T before{}; T after{};
        bool okReadBefore = SafeReadMemory(addr, &before, sizeof(T));
        bool okWrite = SafeWriteMemory(addr, &valueToWrite, sizeof(T));
        bool okReadAfter = SafeReadMemory(addr, &after, sizeof(T));
        std::ostringstream oss;
        oss << "[SWITCH][RW] " << label
            << " @0x" << std::hex << std::uppercase << addr
            << std::dec
            << " before=" << (okReadBefore ? std::to_string((uint64_t)before) : std::string("?"))
            << " write=" << std::to_string((uint64_t)valueToWrite)
            << " after=" << (okReadAfter ? std::to_string((uint64_t)after) : std::string("?"))
            << " okWrite=" << (okWrite ? "1" : "0");
        LogOut(oss.str(), true);
    }

    // Overload for pointers/addresses to log in hex
    static void LogRWPtr(const char* label, uintptr_t addr, uintptr_t valueToWrite) {
        uintptr_t before=0, after=0;
        bool okReadBefore = SafeReadMemory(addr, &before, sizeof(before));
        bool okWrite = SafeWriteMemory(addr, &valueToWrite, sizeof(valueToWrite));
        bool okReadAfter = SafeReadMemory(addr, &after, sizeof(after));
        std::ostringstream oss;
        oss << "[SWITCH][RW] " << label
            << " @0x" << std::hex << std::uppercase << addr
            << " before=0x" << (okReadBefore ? before : 0)
            << " write=0x" << valueToWrite
            << " after=0x" << (okReadAfter ? after : 0)
            << std::dec
            << " okWrite=" << (okWrite ? "1" : "0");
        LogOut(oss.str(), true);
    }

    // Freeze guard: uses EfzRevival's internal patch toggler to temporarily freeze gameplay
    // while we rewire side pointers and flags. This mirrors the engine's own safety during init.
    struct EFZFreezeGuard {
        void* ctx{nullptr};
        int (__thiscall *toggle)(void*, int){nullptr};
        bool active{false};
        EFZFreezeGuard() {
            HMODULE h = GetModuleHandleA("EfzRevival.dll");
            if (!h) return;
            ctx = reinterpret_cast<void*>((uintptr_t)h + EFZREV_RVA_PATCH_CTX);
            toggle = reinterpret_cast<int(__thiscall*)(void*, int)>((uintptr_t)h + EFZREV_RVA_PATCH_TOGGLER);
        }
        static bool SehSafeToggle(int(__thiscall* fn)(void*, int), void* ctx, int val) {
            if (!fn || !ctx) return false;
            __try { fn(ctx, val); return true; }
            __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        }
        void freeze() {
            if (ctx && toggle && !active) {
                bool ok = SehSafeToggle(toggle, ctx, 0);
                active = ok;
                LogOut(ok ? "[SWITCH] Freeze ON" : "[SWITCH] Freeze ON threw exception", true);
            }
        }
        void unfreeze() {
            if (ctx && toggle && active) {
                bool ok = SehSafeToggle(toggle, ctx, 1);
                LogOut(ok ? "[SWITCH] Freeze OFF" : "[SWITCH] Freeze OFF threw exception", true);
                active = false;
            }
        }
        ~EFZFreezeGuard() { unfreeze(); }
    };

    // Helper to reset per-side mapping like sub_1006D640((char **)(this + 8 * (*[this+0x680] + 104)))
    // We don't call into that function directly; instead emulate minimum safe state:
    // In practice, flipping LOCAL/REMOTE and swapping the two side buffer pointers suffices for input routing.
    // If further fixes are needed, we can introduce a lightweight refresh by touching the shared input vector.
    void PostSwitchRefresh(uint8_t* practice) {
        if (!practice) return;
        // Mirror init: sub_1006D640((char **)(this + 8 * (*[this+0x680] + 104)))
        int local = 0;
        if (!SafeReadMemory((uintptr_t)practice + PRACTICE_OFF_LOCAL_SIDE_IDX, &local, sizeof(local))) return;
        uintptr_t efzrevBase = (uintptr_t)GetModuleHandleA("EfzRevival.dll");
        if (!efzrevBase) return;
        auto mapReset = (bool(__thiscall*)(char**))(efzrevBase + EFZREV_RVA_MAP_RESET);
        // Compute (this + 8 * (local + 104)) as char**
        char** mapPtr = (char**)((uintptr_t)practice + (uintptr_t)(8 * (local + 104)));
        if (!s_disableMapReset && mapReset) {
            bool ok = false; bool sehOk = SehSafe_MapReset(mapReset, mapPtr, &ok);
            std::ostringstream oss; oss << "[SWITCH] sub_1006D640 reset(local=" << local << ") -> "
                << (sehOk ? (ok?"OK":"FAIL") : "EXCEPTION");
            LogOut(oss.str(), true);
            if (!sehOk) { s_disableMapReset = true; LogOut("[SWITCH] Map reset disabled after exception", true); }
        } else if (s_disableMapReset) {
            LogOut("[SWITCH] sub_1006D640 skipped (disabled)", true);
        } else {
            LogOut("[SWITCH] sub_1006D640 symbol missing; skipped", true);
        }

        // Call EFZ_Obj_SubStruct448_CleanupPair for either direction to ensure device/pair rebind
        // Always rebind pair so the active pairing follows the current local side
        {
            auto cleanupPair = (int(__thiscall*)(void*))(efzrevBase + EFZREV_RVA_CLEANUP_PAIR);
            void* patchCtx = (void*)(efzrevBase + EFZREV_RVA_PATCH_CTX);
            if (cleanupPair && patchCtx) {
                int rc = 0; bool sehOk = SehSafe_CleanupPair(cleanupPair, patchCtx, &rc);
                std::ostringstream oss; oss << "[SWITCH] CleanupPair(local=" << local << ") -> " << (sehOk?"rc=":"EXCEPTION rc=") << rc;
                LogOut(oss.str(), true);
            } else {
                LogOut("[SWITCH] CleanupPair symbol or ctx missing; skipped", true);
            }
        }

        // Refresh mapping block into Practice (+4..+0x24) from EfzRevival patch ctx (best-effort)
        {
            auto refreshMap = (int(__thiscall*)(void*))(efzrevBase + EFZREV_RVA_REFRESH_MAPPING_BLOCK);
            if (refreshMap) {
                int rc2 = 0; bool sehOk2 = SehSafe_CleanupPair(refreshMap, practice, &rc2); // reuse wrapper for __thiscall(void*)
                std::ostringstream oss; oss << "[SWITCH] RefreshMappingBlock() -> " << (sehOk2?"rc=":"EXCEPTION rc=") << rc2;
                LogOut(oss.str(), true);
            } else {
                LogOut("[SWITCH] RefreshMappingBlock symbol missing; skipped", true);
            }
        }
    }
}

namespace SwitchPlayers {
    static bool ApplySet(uint8_t* practice, int desiredLocal) {
        if (!practice) return false;
        if (desiredLocal != 0 && desiredLocal != 1) return false;

        // Read current
        int curLocal = 0; SafeReadMemory((uintptr_t)practice + PRACTICE_OFF_LOCAL_SIDE_IDX, &curLocal, sizeof(curLocal));
        if (curLocal == desiredLocal) {
            LogOut("[SWITCH] Local side already set; no changes", true);
            return true;
        }
        // If the game is already paused/frozen (either via Practice pause flag or gamespeed freeze),
        // skip toggling the EfzRevival freeze to avoid redundant ON/OFF logs and keep the current pause state.
        EFZFreezeGuard guard; // used only if needed
        if (!PauseIntegration::IsPausedOrFrozen()) {
            guard.freeze();
        } else {
            LogOut("[SWITCH] Detected paused/frozen state; skipping additional Freeze ON/OFF", true);
        }

        // Log practice base for CE watch setup
        {
            std::ostringstream oss; oss << "[SWITCH] practice.base=0x" << std::hex << (uintptr_t)practice;
            LogOut(oss.str(), true);
        }

        int newRemote = (desiredLocal == 0) ? 1 : 0;
        // Write LOCAL and REMOTE with full before/after logs
        LogRW<int>("practice.localSide[+0x680]", (uintptr_t)practice + PRACTICE_OFF_LOCAL_SIDE_IDX, desiredLocal);
        LogRW<int>("practice.remoteSide[+0x684]", (uintptr_t)practice + PRACTICE_OFF_REMOTE_SIDE_IDX, newRemote);

        // Wire side buffer pointers exactly as init does, based on desiredLocal
        uintptr_t baseLocal = (uintptr_t)practice + PRACTICE_OFF_BUF_LOCAL_BASE;   // +0x788
        uintptr_t baseRemote = (uintptr_t)practice + PRACTICE_OFF_BUF_REMOTE_BASE; // +0x800
        uintptr_t primary = (desiredLocal == 0) ? baseLocal : baseRemote;
        uintptr_t secondary = (desiredLocal == 0) ? baseRemote : baseLocal;
        LogRWPtr("practice.sideBuf.primary[+0x824]", (uintptr_t)practice + PRACTICE_OFF_SIDE_BUF_PRIMARY, primary);
        LogRWPtr("practice.sideBuf.secondary[+0x828]", (uintptr_t)practice + PRACTICE_OFF_SIDE_BUF_SECONDARY, secondary);

        // Optional: mirror init by updating INIT_SOURCE too (so next reinit stays consistent)
        LogRW<int>("practice.initSource[+0x944]", (uintptr_t)practice + PRACTICE_OFF_INIT_SOURCE_SIDE, desiredLocal);

    // Ensure control roles match the new local side immediately to avoid double-toggle
        // desiredLocal == 0 -> P1 Human, P2 AI
        // desiredLocal == 1 -> P2 Human, P1 AI
        bool p1HumanBefore = IsAIControlFlagHuman(1);
        bool p2HumanBefore = IsAIControlFlagHuman(2);
        if (desiredLocal == 1) {
            // Local is P2
            SetAIControlFlag(1, /*human=*/false); // P1 AI
            SetAIControlFlag(2, /*human=*/true);  // P2 Human
            LogOut("[SWITCH] Control roles: P1=AI, P2=Human", true);
        } else {
            // Local is P1
            SetAIControlFlag(1, /*human=*/true);  // P1 Human
            SetAIControlFlag(2, /*human=*/false); // P2 AI
            LogOut("[SWITCH] Control roles: P1=Human, P2=AI", true);
        }
        bool p1HumanAfter = IsAIControlFlagHuman(1);
        bool p2HumanAfter = IsAIControlFlagHuman(2);
        {
            std::ostringstream oss;
            oss << "[SWITCH][AI] P1 before=" << (p1HumanBefore?"Human":"AI")
                << " after=" << (p1HumanAfter?"Human":"AI")
                << " | P2 before=" << (p2HumanBefore?"Human":"AI")
                << " after=" << (p2HumanAfter?"Human":"AI");
            LogOut(oss.str(), true);
        }

        // Also update the Practice game-state CPU flags and active player index to match the chosen local side.
        // In Practice:
        //  - gameState + GAMESTATE_OFF_P2_CPU_FLAG (byte) controls P2 CPU (1=CPU, 0=Human)
        //  - gameState + GAMESTATE_OFF_P1_CPU_FLAG (byte) controls P1 CPU (1=CPU, 0=Human)
        //  - gameState + GAMESTATE_OFF_ACTIVE_PLAYER (byte) is 0 for P1, 1 for P2
        // Keep these in sync so the engine doesn't revert our AI/Human assignment next tick.
        uintptr_t efzBase = GetEFZBase();
        if (efzBase) {
            uintptr_t gameStatePtr = 0;
            if (SafeReadMemory(efzBase + EFZ_BASE_OFFSET_GAME_STATE, &gameStatePtr, sizeof(gameStatePtr)) && gameStatePtr) {
                uint8_t p2CpuFlag = (desiredLocal == 1) ? 0u : 1u; // local P2 -> human (0), local P1 -> CPU (1)
                uint8_t p1CpuFlag = (uint8_t)(1u - p2CpuFlag);    // opposite of P2
                uint8_t activePlayer = (uint8_t)desiredLocal;      // 0=P1, 1=P2
                LogRW<uint8_t>("engine.activePlayer[+4930]", gameStatePtr + GAMESTATE_OFF_ACTIVE_PLAYER, activePlayer);
                LogRW<uint8_t>("engine.P2_CPU_FLAG[+4931]", gameStatePtr + GAMESTATE_OFF_P2_CPU_FLAG, p2CpuFlag);
                LogRW<uint8_t>("engine.P1_CPU_FLAG[+4932]", gameStatePtr + GAMESTATE_OFF_P1_CPU_FLAG, p1CpuFlag);
                // Update GUI/buffer display position: *(practice+0x24) = 1 when P1 local, 0 when P2 local
                // CE observed as "Current GUI position"; EfzRevival writes here too (sete -> mov [esi+24], eax)
                uint8_t guiPos = (desiredLocal == 0) ? 1u : 0u;
                LogRW<uint8_t>("practice.GUI_POS[+0x24]", (uintptr_t)practice + PRACTICE_OFF_GUI_POS, guiPos);
            }
            else {
                LogOut("[SWITCH] Game state pointer not available; engine flags not updated", true);
            }
        }

        // Now that engine/practice flags reflect the intended human side, reset per-side mapping and cleanup input pair
        // so that the rebind logic observes the correct target (fixes third-press misassignment).
        PostSwitchRefresh(practice);

        // Diagnostics: verify final values
        int checkLocal = -1, checkRemote = -1;
        SafeReadMemory((uintptr_t)practice + PRACTICE_OFF_LOCAL_SIDE_IDX, &checkLocal, sizeof(checkLocal));
        SafeReadMemory((uintptr_t)practice + PRACTICE_OFF_REMOTE_SIDE_IDX, &checkRemote, sizeof(checkRemote));
        LogOut("[SWITCH] Swapped local/remote input sides; local=" + std::to_string(checkLocal) + ", remote=" + std::to_string(checkRemote), true);
        uintptr_t chkPrim=0, chkSec=0;
        SafeReadMemory((uintptr_t)practice + PRACTICE_OFF_SIDE_BUF_PRIMARY, &chkPrim, sizeof(chkPrim));
        SafeReadMemory((uintptr_t)practice + PRACTICE_OFF_SIDE_BUF_SECONDARY, &chkSec, sizeof(chkSec));
        {
            std::ostringstream oss; 
            oss << "[SWITCH] Side buffers: primary=0x" << std::hex << chkPrim 
                << ", secondary=0x" << chkSec
                << ", expected primary=0x" << std::hex << ((desiredLocal==0)? ((uintptr_t)practice + PRACTICE_OFF_BUF_LOCAL_BASE) : ((uintptr_t)practice + PRACTICE_OFF_BUF_REMOTE_BASE));
            LogOut(oss.str(), true);
        }
        // Verify AI flags readback (single diagnostic snapshot)
        bool p1Human = IsAIControlFlagHuman(1);
        bool p2Human = IsAIControlFlagHuman(2);
        LogOut(std::string("[SWITCH] AI flags after toggle: P1=") + (p1Human?"Human":"AI") + ", P2=" + (p2Human?"Human":"AI"), true);
    // Verify GUI/buffer display position at +0x24
    uint8_t guiPosR = 0; SafeReadMemory((uintptr_t)practice + PRACTICE_OFF_GUI_POS, &guiPosR, sizeof(guiPosR));
    LogOut(std::string("[SWITCH] Practice +0x24 GUI pos (1=P1,0=P2): ") + (guiPosR?"1":"0"), true);

        // Inspect shared input vector slots that may select which handle is considered active
        uintptr_t slot0 = 0, slot1 = 0;
        SafeReadMemory((uintptr_t)practice + PRACTICE_OFF_SHARED_INPUT_VEC, &slot0, sizeof(slot0));
        SafeReadMemory((uintptr_t)practice + PRACTICE_OFF_SHARED_INPUT_VEC + sizeof(uintptr_t), &slot1, sizeof(slot1));
        {
            std::ostringstream oss; oss << "[SWITCH] Shared input vec [+0x1240]: slot0=0x" << std::hex << slot0
                                        << ", slot1=0x" << slot1;
            LogOut(oss.str(), true);
        }

        // Done; unfreeze on scope exit
        return true;
    }

    bool ToggleLocalSide() {
        if (GetCurrentGameMode() != GameMode::Practice) return false;
        // Only switch during active match to avoid confusing selection/menus
        if (!IsMatchPhase()) {
            LogOut("[SWITCH] Ignored toggle outside of match phase", true);
            return false;
        }
        PauseIntegration::EnsurePracticePointerCapture();
        void* p = PauseIntegration::GetPracticeControllerPtr();
        if (!p) { LogOut("[SWITCH] Practice controller not available", true); return false; }
        uint8_t* practice = reinterpret_cast<uint8_t*>(p);
        int curLocal = 0; if (!SafeReadMemory((uintptr_t)practice + PRACTICE_OFF_LOCAL_SIDE_IDX, &curLocal, sizeof(curLocal))) return false;
        int desired = (curLocal == 0) ? 1 : 0;
        return ApplySet(practice, desired);
    }

    bool SetLocalSide(int sideIdx) {
        if (GetCurrentGameMode() != GameMode::Practice) return false;
        if (!IsMatchPhase()) {
            LogOut("[SWITCH] Ignored set outside of match phase", true);
            return false;
        }
        PauseIntegration::EnsurePracticePointerCapture();
        void* p = PauseIntegration::GetPracticeControllerPtr();
        if (!p) { LogOut("[SWITCH] Practice controller not available", true); return false; }
        return ApplySet(reinterpret_cast<uint8_t*>(p), sideIdx);
    }
}
