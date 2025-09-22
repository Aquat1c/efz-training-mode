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

namespace {
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
        __try {
            if (mapReset) {
                mapReset(mapPtr);
            }
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            // Best-effort; ignore if callsite not safe in this context
        }

        // If new local is P2 (1), EfzRevival calls EFZ_Obj_SubStruct448_CleanupPair(&dword_100A0760)
        if (local == 1) {
            auto cleanupPair = (int(__thiscall*)(void*))(efzrevBase + EFZREV_RVA_CLEANUP_PAIR);
            void* patchCtx = (void*)(efzrevBase + EFZREV_RVA_PATCH_CTX);
            __try {
                if (cleanupPair && patchCtx) {
                    cleanupPair(patchCtx);
                }
            } __except(EXCEPTION_EXECUTE_HANDLER) {
                // ignore
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

        int newRemote = (desiredLocal == 0) ? 1 : 0;
    // Write LOCAL and REMOTE
        SafeWriteMemory((uintptr_t)practice + PRACTICE_OFF_LOCAL_SIDE_IDX, &desiredLocal, sizeof(desiredLocal));
        SafeWriteMemory((uintptr_t)practice + PRACTICE_OFF_REMOTE_SIDE_IDX, &newRemote, sizeof(newRemote));

    // Wire side buffer pointers exactly as init does, based on desiredLocal
    uintptr_t baseLocal = (uintptr_t)practice + PRACTICE_OFF_BUF_LOCAL_BASE;   // +0x788
    uintptr_t baseRemote = (uintptr_t)practice + PRACTICE_OFF_BUF_REMOTE_BASE; // +0x800
    uintptr_t primary = (desiredLocal == 0) ? baseLocal : baseRemote;
    uintptr_t secondary = (desiredLocal == 0) ? baseRemote : baseLocal;
    SafeWriteMemory((uintptr_t)practice + PRACTICE_OFF_SIDE_BUF_PRIMARY, &primary, sizeof(primary));
    SafeWriteMemory((uintptr_t)practice + PRACTICE_OFF_SIDE_BUF_SECONDARY, &secondary, sizeof(secondary));

        // Optional: mirror init by updating INIT_SOURCE too (so next reinit stays consistent)
        SafeWriteMemory((uintptr_t)practice + PRACTICE_OFF_INIT_SOURCE_SIDE, &desiredLocal, sizeof(desiredLocal));

        PostSwitchRefresh(practice);

        // Ensure control roles match the new local side immediately to avoid double-toggle
        // desiredLocal == 0 -> P1 Human, P2 AI
        // desiredLocal == 1 -> P2 Human, P1 AI
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

        // Also update the Practice game-state CPU flag for P2 to match the chosen local side.
        // In Practice: gameState + 4931 (byte) controls P2 CPU (1=CPU, 0=Human)
        // Keep this in sync so the engine doesn't revert our AI/Human assignment next tick.
        uintptr_t efzBase = GetEFZBase();
        if (efzBase) {
            uintptr_t gameStatePtr = 0;
            if (SafeReadMemory(efzBase + EFZ_BASE_OFFSET_GAME_STATE, &gameStatePtr, sizeof(gameStatePtr)) && gameStatePtr) {
                uint8_t p2CpuFlag = (desiredLocal == 1) ? 0u : 1u; // local P2 -> human (0), local P1 -> CPU (1)
                SafeWriteMemory(gameStatePtr + 4931, &p2CpuFlag, sizeof(p2CpuFlag));
                // Mirror EfzRevival gating: *(practice + 36) = (p2CpuFlag == 0)
                uint32_t p2HumanGate = (p2CpuFlag == 0) ? 1u : 0u;
                SafeWriteMemory((uintptr_t)practice + PRACTICE_OFF_P2_HUMAN_GATE, &p2HumanGate, sizeof(p2HumanGate));
            }
        }

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
    // Verify AI flags readback
        bool p1Human = IsAIControlFlagHuman(1);
        bool p2Human = IsAIControlFlagHuman(2);
        LogOut(std::string("[SWITCH] AI flags after toggle: P1=") + (p1Human?"Human":"AI") + ", P2=" + (p2Human?"Human":"AI"), true);
    // Verify Practice human gate at +0x24
    uint32_t gate = 0; SafeReadMemory((uintptr_t)practice + PRACTICE_OFF_P2_HUMAN_GATE, &gate, sizeof(gate));
    LogOut(std::string("[SWITCH] Practice +0x24 gate (P2 human): ") + (gate?"1":"0"), true);

        // Stabilization: engine logic may flip roles or CPU flag in the next few ticks.
        // Reassert AI/Human and P2 CPU flag briefly to avoid needing extra presses.
        for (int i = 0; i < 12; ++i) {
            if (desiredLocal == 1) {
                SetAIControlFlag(1, /*human=*/false);
                SetAIControlFlag(2, /*human=*/true);
            } else {
                SetAIControlFlag(1, /*human=*/true);
                SetAIControlFlag(2, /*human=*/false);
            }
            if (efzBase) {
                uintptr_t gameStatePtr = 0;
                if (SafeReadMemory(efzBase + EFZ_BASE_OFFSET_GAME_STATE, &gameStatePtr, sizeof(gameStatePtr)) && gameStatePtr) {
                    uint8_t p2CpuFlag = (desiredLocal == 1) ? 0u : 1u;
                    SafeWriteMemory(gameStatePtr + 4931, &p2CpuFlag, sizeof(p2CpuFlag));
                    // Keep Practice human gate in lockstep
                    uint32_t p2HumanGate = (p2CpuFlag == 0) ? 1u : 0u;
                    SafeWriteMemory((uintptr_t)practice + PRACTICE_OFF_P2_HUMAN_GATE, &p2HumanGate, sizeof(p2HumanGate));
                    uint8_t rb=0; if (SafeReadMemory(gameStatePtr + 4931, &rb, sizeof(rb))) {
                        LogOut(std::string("[SWITCH] Stabilize: Practice P2 CPU flag now ") + (rb?"CPU(1)":"Human(0)"), true);
                    }
                }
            }
            Sleep(16); // ~ one frame
        }
        return true;
    }

    bool ToggleLocalSide() {
        if (GetCurrentGameMode() != GameMode::Practice) return false;
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
        PauseIntegration::EnsurePracticePointerCapture();
        void* p = PauseIntegration::GetPracticeControllerPtr();
        if (!p) { LogOut("[SWITCH] Practice controller not available", true); return false; }
        return ApplySet(reinterpret_cast<uint8_t*>(p), sideIdx);
    }
}
