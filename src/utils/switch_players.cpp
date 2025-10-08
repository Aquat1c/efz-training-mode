#include "../include/utils/switch_players.h"
#include "../include/core/logger.h"
#include "../include/core/memory.h"
#include "../include/game/game_state.h"
#include "../include/utils/pause_integration.h"
#include "../include/game/practice_offsets.h"
#include "../include/game/efzrevival_addrs.h"
#include "../include/input/input_motion.h" // SetAIControlFlag
#include "../include/utils/utilities.h" // GetEFZBase
#include "../include/utils/network.h" // GetEfzRevivalVersion
#include <windows.h>
#include <sstream>
#include <iomanip>

namespace {
    // Dump a comprehensive snapshot of the Practice controller to help debug switches
    static void DumpPracticeState(uint8_t* practice, const char* tag) {
        if (!practice) { LogOut("[SWITCH][DUMP] practice=null", true); return; }
        EfzRevivalVersion ver = GetEfzRevivalVersion();
        const char* verName = EfzRevivalVersionName(ver);
        uintptr_t pauseOff = EFZ_Practice_PauseFlagOffset();
        uintptr_t stepOff  = EFZ_Practice_StepFlagOffset();
        uintptr_t stepCnt  = EFZ_Practice_StepCounterOffset();
        int local=-1, remote=-1, initSrc=-1;
        uint8_t pause=0xFF, guiPos=0xFF, stepFlag=0xFF; uint32_t stepCounter=0xDEADCAFE;
        uintptr_t primary=0, secondary=0;
        uintptr_t slot0=0, slot1=0;
    SafeReadMemory((uintptr_t)practice + EFZ_Practice_LocalSideOffset(), &local, sizeof(local));
    SafeReadMemory((uintptr_t)practice + EFZ_Practice_RemoteSideOffset(), &remote, sizeof(remote));
        if (pauseOff) SafeReadMemory((uintptr_t)practice + pauseOff, &pause, sizeof(pause));
        if (stepOff)  SafeReadMemory((uintptr_t)practice + stepOff, &stepFlag, sizeof(stepFlag));
        if (stepCnt)  SafeReadMemory((uintptr_t)practice + stepCnt, &stepCounter, sizeof(stepCounter));
        SafeReadMemory((uintptr_t)practice + PRACTICE_OFF_GUI_POS, &guiPos, sizeof(guiPos));
    SafeReadMemory((uintptr_t)practice + EFZ_Practice_InitSourceSideOffset(), &initSrc, sizeof(initSrc));
        SafeReadMemory((uintptr_t)practice + PRACTICE_OFF_SIDE_BUF_PRIMARY, &primary, sizeof(primary));
        SafeReadMemory((uintptr_t)practice + PRACTICE_OFF_SIDE_BUF_SECONDARY, &secondary, sizeof(secondary));
        SafeReadMemory((uintptr_t)practice + PRACTICE_OFF_SHARED_INPUT_VEC, &slot0, sizeof(slot0));
        SafeReadMemory((uintptr_t)practice + PRACTICE_OFF_SHARED_INPUT_VEC + sizeof(uintptr_t), &slot1, sizeof(slot1));

        uintptr_t expectedLocalBase  = (uintptr_t)practice + PRACTICE_OFF_BUF_LOCAL_BASE;
        uintptr_t expectedRemoteBase = (uintptr_t)practice + PRACTICE_OFF_BUF_REMOTE_BASE;

        std::ostringstream oss;
        oss << "[SWITCH][DUMP] " << (tag?tag:"state")
            << " ver=" << (verName?verName:"?")
            << " practice=0x" << std::hex << (uintptr_t)practice
            << "\n    off.local=0x" << std::hex << PRACTICE_OFF_LOCAL_SIDE_IDX
            << " off.remote=0x" << PRACTICE_OFF_REMOTE_SIDE_IDX
            << " off.primary=0x" << PRACTICE_OFF_SIDE_BUF_PRIMARY
            << " off.secondary=0x" << PRACTICE_OFF_SIDE_BUF_SECONDARY
            << " off.baseLocal=0x" << PRACTICE_OFF_BUF_LOCAL_BASE
            << " off.baseRemote=0x" << PRACTICE_OFF_BUF_REMOTE_BASE
            << " off.initSrc=0x" << PRACTICE_OFF_INIT_SOURCE_SIDE
            << " off.gui=0x" << PRACTICE_OFF_GUI_POS
            << " off.pause=0x" << pauseOff
            << " off.step=0x" << stepOff
            << " off.stepCnt=0x" << stepCnt
            << std::dec
            << "\n    local=" << local << " remote=" << remote << " initSrc=" << initSrc
            << " pause=" << (pause==0xFF? -1 : (int)pause)
            << " stepFlag=" << (stepFlag==0xFF? -1 : (int)stepFlag)
            << " stepCnt=" << stepCounter
            << " gui=" << (guiPos==0xFF? -1 : (int)guiPos)
            << "\n    primary=0x" << std::hex << primary
            << " secondary=0x" << secondary
            << " expectedPrimary=0x" << ((local==0)? expectedLocalBase : expectedRemoteBase)
            << " expectedSecondary=0x" << ((local==0)? expectedRemoteBase : expectedLocalBase)
            << "\n    sharedVec[0]=0x" << slot0 << " [1]=0x" << slot1;
        LogOut(oss.str(), true);

        // Also log key EfzRevival call addresses for this session
        HMODULE h = GetModuleHandleA("EfzRevival.dll"); uintptr_t base = (uintptr_t)h;
        uintptr_t rvaTog = EFZ_RVA_PatchToggler();
        uintptr_t rvaCtx = EFZ_RVA_PatchCtx();
        uintptr_t rvaMap = EFZ_RVA_MapReset();
        uintptr_t rvaClean = EFZ_RVA_CleanupPair();
        uintptr_t rvaRef = EFZ_RVA_RefreshMappingBlock();
        uintptr_t rvaP2C = EFZ_RVA_RefreshMappingBlock_PracToCtx();
        std::ostringstream ab;
        ab << "[SWITCH][DUMP] RVAs: tog=0x" << std::hex << rvaTog
           << " ctx=0x" << rvaCtx << " map=0x" << rvaMap
           << " clean=0x" << rvaClean << " ref=0x" << rvaRef << " p2c=0x" << rvaP2C
           << " | VAs: base=0x" << base
           << " togVA=0x" << (base + rvaTog)
           << " ctxVA=0x" << (base + rvaCtx)
           << " mapVA=0x" << (base + rvaMap)
           << " cleanVA=0x" << (base + rvaClean)
           << " refVA=0x" << (base + rvaRef)
           << " p2cVA=0x" << (base + rvaP2C);
        LogOut(ab.str(), true);
    }
    static bool s_disableMapReset = false; // Auto-latch off after first exception
    // SEH-safe wrapper for EfzRevival getMode function (no C++ objects in scope)
    static void* SehSafe_GetMode(void* fnRaw, int idx, bool* outOk) {
        auto fn = reinterpret_cast<void*(__stdcall*)(int)>(fnRaw);
        if (outOk) *outOk = false;
        if (!fn) return nullptr;
        void* ret = nullptr;
        __try {
            ret = fn(idx);
            if (outOk) *outOk = true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            ret = nullptr;
        }
        return ret;
    }
    // Resolve Practice pointer directly from EfzRevival's game-mode array as a fallback
    // when hooks haven't captured ECX yet. Index 3 corresponds to Practice.
    static uint8_t* ResolvePracticePtrFallback() {
        HMODULE h = GetModuleHandleA("EfzRevival.dll");
        if (!h) return nullptr;
        uintptr_t base = reinterpret_cast<uintptr_t>(h);
        // Prefer 1.02i helper if available: sub_1006C040(idx)
        if (GetEfzRevivalVersion() == EfzRevivalVersion::Revival102i) {
            void* getModeRaw = reinterpret_cast<void*>(base + 0x006C040);
            // Try current mode then common candidates
            uint8_t rawMode = 255; GetCurrentGameMode(&rawMode);
            int tryIdx[3] = { (int)rawMode, 1, 3 };
            for (int t=0;t<3;++t) {
                int idx = tryIdx[t]; if (idx < 0 || idx > 15) continue;
                bool okCall = false;
                void* cand = SehSafe_GetMode(getModeRaw, idx, &okCall);
                if (!okCall || !cand) continue;
                uintptr_t p = reinterpret_cast<uintptr_t>(cand);
                int local=-1, remote=-1;
                if (!SafeReadMemory(p + PRACTICE_OFF_LOCAL_SIDE_IDX, &local, sizeof(local))) continue;
                if (!SafeReadMemory(p + PRACTICE_OFF_REMOTE_SIDE_IDX, &remote, sizeof(remote))) continue;
                if (!((local==0||local==1) && (remote==0||remote==1))) continue;
                return reinterpret_cast<uint8_t*>(p);
            }
            LogOut("[SWITCH] GetModeStruct calls failed; falling back to array scan", true);
        }
        // Fallback: mode array scan (0..15)
        uintptr_t arrRva = EFZ_RVA_GameModePtrArray();
        if (!arrRva) return nullptr;
        for (int idx=0; idx<16; ++idx) {
            uintptr_t slotAddr = base + arrRva + 4*idx;
            uintptr_t p = 0; if (!SafeReadMemory(slotAddr, &p, sizeof(p)) || !p) continue;
            int local=-1, remote=-1;
            if (!SafeReadMemory(p + PRACTICE_OFF_LOCAL_SIDE_IDX, &local, sizeof(local))) continue;
            if (!SafeReadMemory(p + PRACTICE_OFF_REMOTE_SIDE_IDX, &remote, sizeof(remote))) continue;
            if (!((local==0||local==1) && (remote==0||remote==1))) continue;
            std::ostringstream oss; oss << "[SWITCH] Fallback resolved practice slot=" << idx << " VA=0x" << std::hex << p;
            LogOut(oss.str(), true);
            return reinterpret_cast<uint8_t*>(p);
        }
        return nullptr;
    }
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
        // All versions use __thiscall (ctx, char)
        int (__thiscall *toggleThis)(void*, char){nullptr};
        bool active{false};
        EFZFreezeGuard() {
            HMODULE h = GetModuleHandleA("EfzRevival.dll");
            if (!h) return;
            uintptr_t ctxRva = EFZ_RVA_PatchCtx();
            uintptr_t togRva = EFZ_RVA_PatchToggler();
            if (!ctxRva || !togRva) return;
            ctx = reinterpret_cast<void*>((uintptr_t)h + ctxRva);
            // Single path: __thiscall for e/h/i
            toggleThis = reinterpret_cast<int(__thiscall*)(void*, char)>((uintptr_t)h + togRva);
            {
                std::ostringstream oss; oss << "[SWITCH] FreezeGuard init ctx=0x" << std::hex << (uintptr_t)ctx
                    << " toggle=0x" << (uintptr_t)(void*)toggleThis
                    << " conv=thiscall";
                LogOut(oss.str(), true);
            }
        }
        static bool SehSafeToggleThis(int(__thiscall* fn)(void*, char), void* c, char val) {
            if (!fn || !c) return false;
            __try { fn(c, val); return true; }
            __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        }
        void freeze() {
            if (!ctx || active) return;
            bool ok = false;
            if (toggleThis) ok = SehSafeToggleThis(toggleThis, ctx, (char)0);
            active = ok;
            LogOut(ok ? "[SWITCH] Freeze ON" : "[SWITCH] Freeze ON threw exception", true);
        }
        void unfreeze() {
            if (!ctx || !active) return;
            bool ok = false;
            int unfreezeParam = EFZ_PatchToggleUnfreezeParam(); // 1 for e, 3 for h/i
            if (toggleThis) ok = SehSafeToggleThis(toggleThis, ctx, (char)unfreezeParam);
            LogOut(ok ? "[SWITCH] Freeze OFF" : "[SWITCH] Freeze OFF threw exception", true);
            active = false;
        }
        ~EFZFreezeGuard() { unfreeze(); }
    };

    // Helper to reset per-side mapping like sub_1006D640((char **)(this + 8 * (*[this+0x680] + 104)))
    // We don't call into that function directly; instead emulate minimum safe state:
    // In practice, flipping LOCAL/REMOTE and swapping the two side buffer pointers suffices for input routing.
    // If further fixes are needed, we can introduce a lightweight refresh by touching the shared input vector.
    void PostSwitchRefresh(uint8_t* practice) {
        // Attempt to use the official helpers first (works across e/h/i):
        // sub_1006D640 (e), sub_1006DEC0 (h), sub_1006E190 (i)
        // and call CleanupPair when local == 1 (P2) to mirror init path.
        do {
            if (!practice) break;
            // Resolve local side
            int local = -1;
            (void)SafeReadMemory((uintptr_t)practice + PRACTICE_OFF_LOCAL_SIDE_IDX, &local, sizeof(local));
            if (local != 0 && local != 1) break;

            // Compute per-side map pointer: (char**)(this + 8 * (local + 104))
            char** mapPtr = reinterpret_cast<char**>((uintptr_t)practice + (8 * (local + 104)));

            // Resolve MapReset RVA and call if available
            uintptr_t rvaMapReset = EFZ_RVA_MapReset();
            HMODULE hMod = GetModuleHandleA("EfzRevival.dll");
            if (!hMod || !rvaMapReset) break;
            auto fnMapReset = reinterpret_cast<bool(__thiscall*)(char**)>(reinterpret_cast<uintptr_t>(hMod) + rvaMapReset);
            bool ok = false;
            // Compute per-version MapReset pointer address: (char**)(this + 8 * (local + bias))
            int bias = EFZ_Practice_MapResetIndexBias();
            mapPtr = reinterpret_cast<char**>((uintptr_t)practice + (8 * (local + bias)));
            if (!SehSafe_MapReset(fnMapReset, mapPtr, &ok) || !ok) break; // fall back if it failed

            // If local == P2, call cleanup/refresh on patch context
            if (local == 1) {
                uintptr_t rvaCleanup = EFZ_RVA_CleanupPair();
                uintptr_t rvaCtx     = EFZ_RVA_PatchCtx();
                if (rvaCleanup && rvaCtx) {
                    auto fnCleanup = reinterpret_cast<int(__thiscall*)(void*)>(reinterpret_cast<uintptr_t>(hMod) + rvaCleanup);
                    void* patchCtx = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(hMod) + rvaCtx);
                    int rc = 0;
                    SehSafe_CleanupPair(fnCleanup, patchCtx, &rc);
                }
            }

            // If we got here, official path succeeded; no further emulation needed.
            return;
        } while (false);

        // Fallback: legacy emulation used previously (swap buffers, touch shared vector) if official path isn't available
        // Mirror init: sub_1006D640((char **)(this + 8 * (*[this+0x680] + 104)))
        int local = 0;
    if (!SafeReadMemory((uintptr_t)practice + EFZ_Practice_LocalSideOffset(), &local, sizeof(local))) return;
        uintptr_t efzrevBase = (uintptr_t)GetModuleHandleA("EfzRevival.dll");
        if (!efzrevBase) return;
        EfzRevivalVersion ver = GetEfzRevivalVersion();
        bool isE = (ver == EfzRevivalVersion::Revival102e);
        uintptr_t mapRva = isE ? EFZ_RVA_MapReset() : 0; // MapReset is e-only safe; skip on h/i
        {
            std::ostringstream oss; oss << "[SWITCH] PostSwitchRefresh local=" << local
                << " base=0x" << std::hex << efzrevBase
                << " mapRVA=0x" << mapRva << " cleanupRVA=0x" << EFZ_RVA_CleanupPair()
                << " refreshRVA=0x" << EFZ_RVA_RefreshMappingBlock();
            LogOut(oss.str(), true);
        }
        auto mapReset = mapRva ? (bool(__thiscall*)(char**))(efzrevBase + mapRva) : nullptr;
        // Compute (this + 8 * (local + 104)) as char**
        {
            int bias = EFZ_Practice_MapResetIndexBias();
            char** mapPtr = (char**)((uintptr_t)practice + (uintptr_t)(8 * (local + bias)));
            {
                std::ostringstream oss; oss << "[SWITCH] MapReset args: practice=0x" << std::hex << (uintptr_t)practice
                    << " local=" << std::dec << local << " mapPtr=practice+8*(" << (local+bias) << ") => 0x" << std::hex << (uintptr_t)mapPtr;
                LogOut(oss.str(), true);
            }
            if (!isE) {
                LogOut("[SWITCH] sub_1006D640 skipped on this version (h/i); using CleanupPair+Refresh instead", true);
            } else if (!s_disableMapReset && mapReset) {
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
        }

        // Always call EFZ_Obj_SubStruct448_CleanupPair so the active input pair tracks the current local side.
        // Previously this was gated to local==1, which could leave controls mapped to P2 when switching back to P1.
        {
            uintptr_t cleanRva = EFZ_RVA_CleanupPair();
            uintptr_t ctxRva = EFZ_RVA_PatchCtx();
            auto cleanupPair = cleanRva ? (int(__thiscall*)(void*))(efzrevBase + cleanRva) : nullptr;
            void* patchCtx = ctxRva ? (void*)(efzrevBase + ctxRva) : nullptr;
            if (cleanupPair && patchCtx) {
                std::ostringstream pre; pre << "[SWITCH] Rebinding input pair via CleanupPair (local=" << local << ") ctx=0x" << std::hex << (efzrevBase + ctxRva);
                LogOut(pre.str(), true);
                int rc = 0; bool sehOk = SehSafe_CleanupPair(cleanupPair, patchCtx, &rc);
                std::ostringstream oss; oss << "[SWITCH] CleanupPair(local=" << local << ") -> " << (sehOk?"rc=":"EXCEPTION rc=") << rc;
                LogOut(oss.str(), true);
            } else {
                LogOut("[SWITCH] CleanupPair symbol or ctx missing; skipped", true);
            }
        }

        // Refresh mapping block into Practice (+4..+0x24). Use version-aware receiver:
        //  - 1.02e: call with Practice as 'this' (legacy signature)
        //  - 1.02h/i: call with patch ctx as 'this' (ctx→Prac variant per doc)
        {
            uintptr_t refreshRva = EFZ_RVA_RefreshMappingBlock();
            auto refreshMap = refreshRva ? (int(__thiscall*)(void*))(efzrevBase + refreshRva) : nullptr;
            uintptr_t refreshPracToCtxRva = EFZ_RVA_RefreshMappingBlock_PracToCtx();
            auto refreshPracToCtx = refreshPracToCtxRva ? (int(__thiscall*)(void*))(efzrevBase + refreshPracToCtxRva) : nullptr;
            // Resolve patch ctx to use as receiver
            void* patchCtx = nullptr;
            uintptr_t ctxRva = EFZ_RVA_PatchCtx();
            if (ctxRva) patchCtx = (void*)(efzrevBase + ctxRva);
            EfzRevivalVersion ver = GetEfzRevivalVersion();
            if (!refreshMap) {
                LogOut("[SWITCH] RefreshMappingBlock symbol missing; skipped", true);
            } else {
                int rc2 = 0; bool sehOk2 = false;
                if (ver == EfzRevivalVersion::Revival102e) {
                    // 1.02e: call with Practice as receiver
                    sehOk2 = SehSafe_CleanupPair(refreshMap, practice, &rc2);
                    std::ostringstream oss; oss << "[SWITCH] RefreshMappingBlock(Practice) -> " << (sehOk2?"rc=":"EXCEPTION rc=") << rc2
                        << " practice=0x" << std::hex << (uintptr_t)practice;
                    LogOut(oss.str(), true);
                } else {
                    // 1.02h/i: synchronize both directions: Practice->ctx then ctx->Practice
                    if (!patchCtx) {
                        LogOut("[SWITCH] RefreshMappingBlock ctx missing; skipped", true);
                    } else {
                        if (refreshPracToCtx) {
                            int rcA = 0; bool okA = SehSafe_CleanupPair(refreshPracToCtx, practice, &rcA);
                            std::ostringstream osa; osa << "[SWITCH] RefreshMappingBlock(Prac→ctx) -> " << (okA?"rc=":"EXCEPTION rc=") << rcA
                                << " practice=0x" << std::hex << (uintptr_t)practice;
                            LogOut(osa.str(), true);
                        } else {
                            LogOut("[SWITCH] RefreshMappingBlock_PracToCtx symbol missing; skipped", true);
                        }
                        int rcB = 0; bool okB = SehSafe_CleanupPair(refreshMap, patchCtx, &rcB);
                        std::ostringstream osb; osb << "[SWITCH] RefreshMappingBlock(ctx→Prac) -> " << (okB?"rc=":"EXCEPTION rc=") << rcB
                            << " ctx=0x" << std::hex << (uintptr_t)patchCtx << " practice=0x" << (uintptr_t)practice;
                        LogOut(osb.str(), true);
                    }
                }
            }
        }
    }
}

namespace SwitchPlayers {
    static bool ApplySet(uint8_t* practice, int desiredLocal) {
        if (!practice) return false;
        if (desiredLocal != 0 && desiredLocal != 1) return false;

        // Read current
    int curLocal = 0; SafeReadMemory((uintptr_t)practice + EFZ_Practice_LocalSideOffset(), &curLocal, sizeof(curLocal));
        DumpPracticeState(practice, "before");
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

        // Wire side buffer pointers exactly as init does on 1.02e. On 1.02h/i, the layout differs
        // so we avoid direct pointer writes and rely on engine CleanupPair + RefreshMappingBlock.
        {
            EfzRevivalVersion ver = GetEfzRevivalVersion();
            if (ver == EfzRevivalVersion::Revival102e) {
                uintptr_t baseLocal = (uintptr_t)practice + PRACTICE_OFF_BUF_LOCAL_BASE;   // +0x796
                uintptr_t baseRemote = (uintptr_t)practice + PRACTICE_OFF_BUF_REMOTE_BASE; // +0x808
                uintptr_t primary = (desiredLocal == 0) ? baseLocal : baseRemote;
                uintptr_t secondary = (desiredLocal == 0) ? baseRemote : baseLocal;
                LogRWPtr("practice.sideBuf.primary[+0x832]", (uintptr_t)practice + PRACTICE_OFF_SIDE_BUF_PRIMARY, primary);
                LogRWPtr("practice.sideBuf.secondary[+0x836]", (uintptr_t)practice + PRACTICE_OFF_SIDE_BUF_SECONDARY, secondary);
                // Optional: mirror init by updating INIT_SOURCE too (so next reinit stays consistent)
                LogRW<int>("practice.initSource[+0x944]", (uintptr_t)practice + PRACTICE_OFF_INIT_SOURCE_SIDE, desiredLocal);
            } else {
                LogOut("[SWITCH] Skipping direct side-buffer/initSource writes on 1.02h/i; using engine refresh", true);
            }
        }

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
                {
                    std::ostringstream oss; oss << "[SWITCH][ENGINE] gameState=0x" << std::hex << gameStatePtr
                        << " write active=" << std::dec << (int)activePlayer
                        << " P2CPU=" << (int)p2CpuFlag << " P1CPU=" << (int)p1CpuFlag;
                    LogOut(oss.str(), true);
                }
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
        {
            EfzRevivalVersion ver = GetEfzRevivalVersion();
            if (ver == EfzRevivalVersion::Revival102e) {
                uintptr_t chkPrim=0, chkSec=0;
                SafeReadMemory((uintptr_t)practice + PRACTICE_OFF_SIDE_BUF_PRIMARY, &chkPrim, sizeof(chkPrim));
                SafeReadMemory((uintptr_t)practice + PRACTICE_OFF_SIDE_BUF_SECONDARY, &chkSec, sizeof(chkSec));
                std::ostringstream oss; 
                oss << "[SWITCH] Side buffers: primary=0x" << std::hex << chkPrim 
                    << ", secondary=0x" << chkSec
                    << ", expected primary=0x" << std::hex << ((desiredLocal==0)? ((uintptr_t)practice + PRACTICE_OFF_BUF_LOCAL_BASE) : ((uintptr_t)practice + PRACTICE_OFF_BUF_REMOTE_BASE));
                LogOut(oss.str(), true);
            } else {
                LogOut("[SWITCH] Side buffer verification skipped on 1.02h/i (layout differs)", true);
            }
        }
        // Verify AI flags readback (single diagnostic snapshot)
        bool p1Human = IsAIControlFlagHuman(1);
        bool p2Human = IsAIControlFlagHuman(2);
        LogOut(std::string("[SWITCH] AI flags after toggle: P1=") + (p1Human?"Human":"AI") + ", P2=" + (p2Human?"Human":"AI"), true);
    // Verify GUI/buffer display position at +0x24
    uint8_t guiPosR = 0; SafeReadMemory((uintptr_t)practice + PRACTICE_OFF_GUI_POS, &guiPosR, sizeof(guiPosR));
    LogOut(std::string("[SWITCH] Practice +0x24 GUI pos (1=P1,0=P2): ") + (guiPosR?"1":"0"), true);

        // Inspect shared input vector slots that may select which handle is considered active
        {
            EfzRevivalVersion ver = GetEfzRevivalVersion();
            if (ver == EfzRevivalVersion::Revival102e) {
                uintptr_t slot0 = 0, slot1 = 0;
                SafeReadMemory((uintptr_t)practice + PRACTICE_OFF_SHARED_INPUT_VEC, &slot0, sizeof(slot0));
                SafeReadMemory((uintptr_t)practice + PRACTICE_OFF_SHARED_INPUT_VEC + sizeof(uintptr_t), &slot1, sizeof(slot1));
                std::ostringstream oss; oss << "[SWITCH] Shared input vec [+0x1240]: slot0=0x" << std::hex << slot0
                                            << ", slot1=0x" << slot1;
                LogOut(oss.str(), true);
            } else {
                LogOut("[SWITCH] Shared input vec dump skipped on 1.02h/i (offset likely differs)", true);
            }
        }

        // Done; unfreeze on scope exit
        DumpPracticeState(practice, "after");
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
        if (!p) {
            // Fallback: try direct resolution from game-mode array
            uint8_t* fb = ResolvePracticePtrFallback();
            if (!fb) {
                LogOut("[SWITCH] Practice controller not available", true);
                return false;
            }
            LogOut("[SWITCH] Practice pointer resolved via fallback (GameModePtrArray)", true);
            p = fb;
        }
        uint8_t* practice = reinterpret_cast<uint8_t*>(p);
    int curLocal = 0; if (!SafeReadMemory((uintptr_t)practice + EFZ_Practice_LocalSideOffset(), &curLocal, sizeof(curLocal))) return false;
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
        if (!p) {
            uint8_t* fb = ResolvePracticePtrFallback();
            if (!fb) { LogOut("[SWITCH] Practice controller not available", true); return false; }
            LogOut("[SWITCH] Practice pointer resolved via fallback (GameModePtrArray)", true);
            p = fb;
        }
        return ApplySet(reinterpret_cast<uint8_t*>(p), sideIdx);
    }
}
