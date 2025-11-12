#include "../include/utils/switch_players.h"
#include "../include/core/logger.h"
#include "../include/core/memory.h"
#include "../include/game/game_state.h"
#include "../include/utils/pause_integration.h"
#include "../include/game/practice_offsets.h"
#include "../include/game/efzrevival_addrs.h"
#include "../include/input/input_motion.h" // SetAIControlFlag
#include "../include/input/input_hook.h" // SetVanillaSwapInputRouting
#include "../include/input/input_core.h" // cleanup helpers
#include "../include/utils/utilities.h" // GetEFZBase
#include "../include/utils/network.h" // GetEfzRevivalVersion
#include "../include/utils/debug_log.h"
#include <windows.h>
#include <atomic>
#include <sstream>
#include <iomanip>

namespace {
    // Simple presence check for EfzRevival
    static inline bool IsRevivalLoaded() {
        return GetModuleHandleA("EfzRevival.dll") != nullptr;
    }
    // Track whether sides were swapped during the current match
    static std::atomic<bool> s_sidesAreSwapped{false};
    
    // Character Select–scoped log suppression for missing Practice controller during menu resets
    static std::atomic<bool> s_csActive{false};
    static std::atomic<bool> s_loggedNoPracticeThisCS{false};
    // Global once-only logging guard for missing Practice controller during menu mapping reset
    static std::atomic<bool> s_loggedNoPracticeEver{false};
    static inline void UpdateCsCycleState() {
        const bool cs = IsInCharacterSelectScreen();
        if (cs) {
            if (!s_csActive.exchange(true, std::memory_order_relaxed)) {
                s_loggedNoPracticeThisCS.store(false, std::memory_order_relaxed);
            }
        } else {
            s_csActive.store(false, std::memory_order_relaxed);
        }
    }
    // Dump a region of memory as hex for detailed analysis
    static void DumpMemoryRegion(const char* label, uintptr_t address, size_t size) {
        if (!address || size == 0 || size > 256) return; // Safety limit
        
        uint8_t buffer[256];
        if (!SafeReadMemory(address, buffer, size)) {
            DebugLog::Write(std::string(label) + " - Failed to read memory");
            return;
        }
        
        std::ostringstream oss;
        oss << label << " @0x" << std::hex << std::uppercase << address << " [" << std::dec << size << " bytes]:";
        for (size_t i = 0; i < size; ++i) {
            if (i % 16 == 0) oss << "\n  ";
            oss << std::hex << std::setw(2) << std::setfill('0') << (int)buffer[i] << " ";
        }
        DebugLog::Write(oss.str());
    }
    
    // Dump critical Practice controller fields with memory dumps
    static void DumpPracticeStateDetailed(uint8_t* practice, const char* tag) {
        if (!practice) { 
            DebugLog::Write(std::string("[DUMP] ") + (tag ? tag : "state") + " - practice pointer is NULL");
            return; 
        }
        
        EfzRevivalVersion ver = GetEfzRevivalVersion();
        const char* verName = EfzRevivalVersionName(ver);
        
        DebugLog::Write("========================================");
        DebugLog::Write(std::string("PRACTICE CONTROLLER DUMP: ") + (tag ? tag : "state"));
        DebugLog::Write("========================================");
        
        std::ostringstream ossBasic;
        ossBasic << "Version: " << (verName ? verName : "unknown")
                 << " | Practice base: 0x" << std::hex << std::uppercase << (uintptr_t)practice;
        DebugLog::Write(ossBasic.str());
        
        // Read all critical fields
        int local=-1, remote=-1, initSrc=-1;
        uint8_t guiPos=0xFF;
        uintptr_t primary=0, secondary=0;
        
        SafeReadMemory((uintptr_t)practice + EFZ_Practice_LocalSideOffset(), &local, sizeof(local));
        SafeReadMemory((uintptr_t)practice + EFZ_Practice_RemoteSideOffset(), &remote, sizeof(remote));
        SafeReadMemory((uintptr_t)practice + PRACTICE_OFF_GUI_POS, &guiPos, sizeof(guiPos));
        SafeReadMemory((uintptr_t)practice + EFZ_Practice_InitSourceSideOffset(), &initSrc, sizeof(initSrc));
        SafeReadMemory((uintptr_t)practice + PRACTICE_OFF_SIDE_BUF_PRIMARY, &primary, sizeof(primary));
        SafeReadMemory((uintptr_t)practice + PRACTICE_OFF_SIDE_BUF_SECONDARY, &secondary, sizeof(secondary));
        
        DebugLog::Write("--- Core Fields ---");
        DebugLog::LogRead("local side", (uintptr_t)practice + EFZ_Practice_LocalSideOffset(), local);
        DebugLog::LogRead("remote side", (uintptr_t)practice + EFZ_Practice_RemoteSideOffset(), remote);
        DebugLog::LogRead("GUI position", (uintptr_t)practice + PRACTICE_OFF_GUI_POS, guiPos);
        DebugLog::LogRead("init source", (uintptr_t)practice + EFZ_Practice_InitSourceSideOffset(), initSrc);
        DebugLog::LogRead("primary buffer ptr", (uintptr_t)practice + PRACTICE_OFF_SIDE_BUF_PRIMARY, primary);
        DebugLog::LogRead("secondary buffer ptr", (uintptr_t)practice + PRACTICE_OFF_SIDE_BUF_SECONDARY, secondary);
        
        // Calculate expected buffer addresses based on version
        uintptr_t baseLocal, baseRemote;
        if (ver == EfzRevivalVersion::Revival102e) {
            baseLocal = (uintptr_t)practice + PRACTICE_OFF_BUF_LOCAL_BASE;
            baseRemote = (uintptr_t)practice + PRACTICE_OFF_BUF_REMOTE_BASE;
        } else {
            baseLocal = (uintptr_t)practice + 0x314;  // h/i: 788 decimal
            baseRemote = (uintptr_t)practice + 0x320; // h/i: 800 decimal
        }
        
        std::ostringstream ossExpected;
        ossExpected << "Expected buffers - Local: 0x" << std::hex << std::uppercase << baseLocal
                    << " | Remote: 0x" << baseRemote;
        DebugLog::Write(ossExpected.str());
        
        // Dump memory around buffer pointer storage
        DebugLog::Write("--- Memory Dumps ---");
        DumpMemoryRegion("Buffer pointers region [+0x330 to +0x350]", 
                        (uintptr_t)practice + 0x330, 32);
        
        // Dump the actual buffer contents if pointers are valid
        if (primary != 0 && primary > 0x10000) {
            DumpMemoryRegion("Primary buffer content", primary, 64);
        } else {
            DebugLog::Write("Primary buffer pointer invalid or NULL");
        }
        
        if (secondary != 0 && secondary > 0x10000) {
            DumpMemoryRegion("Secondary buffer content", secondary, 64);
        } else {
            DebugLog::Write("Secondary buffer pointer invalid or NULL");
        }
        
        // Dump local and remote buffer areas
        DumpMemoryRegion("Local buffer area", baseLocal, 64);
        DumpMemoryRegion("Remote buffer area", baseRemote, 64);
        
        // Dump the area around local/remote side values
        DumpMemoryRegion("Side values region [+0x680 to +0x690]",
                        (uintptr_t)practice + 0x680, 16);
        
        DebugLog::Write("========================================");
    }
    
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
        // Gate scanning strictly: Practice mode only, offline only, and never during Character Select
        if (IsInCharacterSelectScreen()) return nullptr;
        if (GetCurrentGameMode() != GameMode::Practice) return nullptr;
        if (DetectOnlineMatch() || isOnlineMatch.load(std::memory_order_relaxed)) return nullptr;
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
        
        // Also log to debug file
        if (okReadBefore) {
            DebugLog::LogWrite(label, addr, before, valueToWrite);
        }
        if (okReadAfter && okWrite) {
            DebugLog::LogRead(std::string(label) + " [verified]", addr, after);
        }
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
        
        // Also log to debug file
        if (okReadBefore) {
            DebugLog::LogWrite(label, addr, before, valueToWrite);
        }
        if (okReadAfter && okWrite) {
            DebugLog::LogRead(std::string(label) + " [verified]", addr, after);
        }
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
        // Plain C-style function to avoid C++ object unwinding issues with SEH
        static bool SehSafeToggleThisImpl(void* fn, void* c, int val) {
            if (!fn || !c) return false;
            typedef int(__thiscall* ToggleFn)(void*, char);
            ToggleFn toggleFn = (ToggleFn)fn;
            __try { 
                toggleFn(c, (char)val); 
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER) { 
                return false;
            }
        }
        
        static bool SehSafeToggleThis(int(__thiscall* fn)(void*, char), void* c, char val) {
            if (!fn || !c) return false;
            // Log before/after SEH block
            char logBuf[128];
            sprintf_s(logBuf, "Calling PatchToggler with param=%d", (int)val);
            DebugLog::Write(logBuf);
            
            bool success = SehSafeToggleThisImpl((void*)fn, c, (int)val);
            
            DebugLog::Write(success ? "PatchToggler returned successfully" : "PatchToggler threw exception!");
            return success;
        }
        void freeze() {
            if (!ctx || active) return;
            DebugLog::Write("--- FREEZE BEGIN ---");
            std::ostringstream oss;
            oss << "Freezing game - ctx=0x" << std::hex << std::uppercase << (uintptr_t)ctx 
                << " toggleThis=0x" << (uintptr_t)toggleThis;
            DebugLog::Write(oss.str());
            bool ok = false;
            if (toggleThis) ok = SehSafeToggleThis(toggleThis, ctx, (char)0);
            active = ok;
            DebugLog::Write(ok ? "Freeze ON - SUCCESS" : "Freeze ON - FAILED");
            LogOut(ok ? "[SWITCH] Freeze ON" : "[SWITCH] Freeze ON threw exception", true);
        }
        void unfreeze() {
            if (!ctx || !active) return;
            DebugLog::Write("--- UNFREEZE BEGIN ---");
            int unfreezeParam = EFZ_PatchToggleUnfreezeParam(); // 1 for e, 3 for h/i
            std::ostringstream oss;
            oss << "Unfreezing game - ctx=0x" << std::hex << std::uppercase << (uintptr_t)ctx 
                << " param=" << std::dec << unfreezeParam;
            DebugLog::Write(oss.str());
            bool ok = false;
            if (toggleThis) ok = SehSafeToggleThis(toggleThis, ctx, (char)unfreezeParam);
            DebugLog::Write(ok ? "Freeze OFF - SUCCESS" : "Freeze OFF - FAILED");
            LogOut(ok ? "[SWITCH] Freeze OFF" : "[SWITCH] Freeze OFF threw exception", true);
            active = false;
        }
        ~EFZFreezeGuard() { 
            if (active) {
                DebugLog::Write("--- FREEZE GUARD DESTRUCTOR (auto-unfreeze) ---");
                unfreeze(); 
            }
        }
    };

    // Helper to reset per-side mapping like sub_1006D640((char **)(this + 8 * (*[this+0x680] + 104)))
    // We don't call into that function directly; instead emulate minimum safe state:
    // In practice, flipping LOCAL/REMOTE and swapping the two side buffer pointers suffices for input routing.
    // If further fixes are needed, we can introduce a lightweight refresh by touching the shared input vector.
    void PostSwitchRefresh(uint8_t* practice, int explicitLocal = -1) {
        DebugLog::Write("--- POST SWITCH REFRESH BEGIN ---");
        
        // Attempt to use the official helpers first (works across e/h/i):
        // sub_1006D640 (e), sub_1006DEC0 (h), sub_1006E190 (i)
        // and call CleanupPair when local == 1 (P2) to mirror init path.
        do {
            if (!practice) {
                DebugLog::Write("PostSwitchRefresh: practice is NULL");
                break;
            }
            // Resolve local side (use explicit if provided, otherwise read from memory)
            int local = -1;
            if (explicitLocal >= 0 && explicitLocal <= 1) {
                local = explicitLocal;
                DebugLog::Write("PostSwitchRefresh: using explicit local=" + std::to_string(local));
            } else {
                (void)SafeReadMemory((uintptr_t)practice + PRACTICE_OFF_LOCAL_SIDE_IDX, &local, sizeof(local));
                DebugLog::LogRead("PostSwitchRefresh: local side", (uintptr_t)practice + PRACTICE_OFF_LOCAL_SIDE_IDX, local);
            }
            if (local != 0 && local != 1) {
                DebugLog::Write("PostSwitchRefresh: invalid local side value");
                break;
            }

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
        if (explicitLocal >= 0 && explicitLocal <= 1) {
            local = explicitLocal;
        } else if (!SafeReadMemory((uintptr_t)practice + EFZ_Practice_LocalSideOffset(), &local, sizeof(local))) {
            return;
        }
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
    // Minimal vanilla path: no EfzRevival. Update engine flags only and align AI roles.
    static bool ApplyEngineOnlySet(int desiredLocal) {
        if (desiredLocal != 0 && desiredLocal != 1) return false;
        // Only during Practice match to avoid side effects in other modes
        if (GetCurrentGameMode() != GameMode::Practice || !IsMatchPhase()) return false;

        uintptr_t efzBase = GetEFZBase();
        if (!efzBase) return false;
        uintptr_t gameStatePtr = 0;
        if (!SafeReadMemory(efzBase + EFZ_BASE_OFFSET_GAME_STATE, &gameStatePtr, sizeof(gameStatePtr)) || !gameStatePtr)
            return false;

        uint8_t activePlayer = static_cast<uint8_t>(desiredLocal); // 0=P1,1=P2
        uint8_t p2CpuFlag    = (desiredLocal == 1) ? 0u : 1u;       // human when local
        uint8_t p1CpuFlag    = (uint8_t)(1u - p2CpuFlag);

        bool okA = SafeWriteMemory(gameStatePtr + GAMESTATE_OFF_ACTIVE_PLAYER, &activePlayer, sizeof(activePlayer));
        bool ok1 = SafeWriteMemory(gameStatePtr + GAMESTATE_OFF_P1_CPU_FLAG, &p1CpuFlag, sizeof(p1CpuFlag));
        bool ok2 = SafeWriteMemory(gameStatePtr + GAMESTATE_OFF_P2_CPU_FLAG, &p2CpuFlag, sizeof(p2CpuFlag));

        // Align our AI control hooks with the new local side (local = Human, remote = AI)
        if (desiredLocal == 1) {
            SetAIControlFlag(1, /*human=*/false); // P1 AI
            SetAIControlFlag(2, /*human=*/true);  // P2 Human
            LogOut("[SWITCH][VANILLA] Control roles: P1=AI, P2=Human", true);
        } else {
            SetAIControlFlag(1, /*human=*/true);  // P1 Human
            SetAIControlFlag(2, /*human=*/false); // P2 AI
            LogOut("[SWITCH][VANILLA] Control roles: P1=Human, P2=AI", true);
        }

        // Critical: swap control routing in vanilla so that when P2 is local, P2 uses P1's controls.
        // enable=true when desiredLocal==1 (P2 local), disable when desiredLocal==0 (P1 local)
        SetVanillaSwapInputRouting(desiredLocal == 1);

        std::ostringstream oss;
        oss << "[SWITCH][VANILLA] Engine-only swap -> active=" << (int)activePlayer
            << " P1CPU=" << (int)p1CpuFlag << " P2CPU=" << (int)p2CpuFlag
            << " gameState=0x" << std::hex << gameStatePtr;
        LogOut(oss.str(), true);

        // Arm late neutralization on the side becoming AI to prevent residual motions
        int aiPlayer = (desiredLocal == 1) ? 1 : 2; // if P2 local -> P1 AI, else P2 AI
    // Immediately neutralize the motion token for the side becoming AI to kill any in-flight motions
    (void)NeutralizeMotionToken(aiPlayer);
        InputHook_ArmTokenNeutralize(aiPlayer, /*alsoDoFullCleanup=*/true);

        return okA && ok1 && ok2;
    }
    static bool ApplySet(uint8_t* practice, int desiredLocal) {
        if (!practice) return false;
        if (desiredLocal != 0 && desiredLocal != 1) return false;

        // Start debug log session with full context
        DebugLog::Write("========================================");
        DebugLog::Write("SWITCH PLAYER OPERATION START");
        DebugLog::Write("========================================");
        
        EfzRevivalVersion ver = GetEfzRevivalVersion();
        const char* verName = EfzRevivalVersionName(ver);
        std::ostringstream ossHeader;
        ossHeader << "Version: " << (verName ? verName : "unknown") 
                  << " | Practice base: 0x" << std::hex << std::uppercase << (uintptr_t)practice
                  << " | Desired local: " << std::dec << desiredLocal;
        DebugLog::Write(ossHeader.str());
        
        // Log EfzRevival.dll base for reference
        HMODULE hEfz = GetModuleHandleA("EfzRevival.dll");
        if (hEfz) {
            std::ostringstream ossEfz;
            ossEfz << "EfzRevival.dll base: 0x" << std::hex << std::uppercase << (uintptr_t)hEfz;
            DebugLog::Write(ossEfz.str());
        }
        
        // Read current
    int curLocal = 0; SafeReadMemory((uintptr_t)practice + EFZ_Practice_LocalSideOffset(), &curLocal, sizeof(curLocal));
        DebugLog::LogRead("practice.localSide[INITIAL]", (uintptr_t)practice + EFZ_Practice_LocalSideOffset(), curLocal);
        
        // Dump DETAILED state before any changes
        DumpPracticeStateDetailed(practice, "BEFORE SWITCH");
        DumpPracticeState(practice, "before");
        
        if (curLocal == desiredLocal) {
            LogOut("[SWITCH] Local side already set; no changes", true);
            DebugLog::Write("No changes needed - already at desired local side");
            DebugLog::Write("========================================");
            return true;
        }
        // Only freeze around E-path edits; H/I path will emulate engine hotkey without extra bracketing.
        EFZFreezeGuard guard; // used for 1.02e only
        bool useFreeze = (ver == EfzRevivalVersion::Revival102e);
        if (useFreeze) {
            if (!PauseIntegration::IsPausedOrFrozen()) {
                guard.freeze();
            } else {
                LogOut("[SWITCH] Detected paused/frozen state; skipping additional Freeze ON/OFF", true);
            }
        }

        // Log practice base for CE watch setup
        {
            std::ostringstream oss; oss << "[SWITCH] practice.base=0x" << std::hex << (uintptr_t)practice;
            LogOut(oss.str(), true);
        }

        // For 1.02e we update Practice fields directly; for h/i we do not touch these.
        int newRemote = (desiredLocal == 0) ? 1 : 0;
        if (ver == EfzRevivalVersion::Revival102e) {
            LogRW<int>("practice.localSide[+0x680]", (uintptr_t)practice + PRACTICE_OFF_LOCAL_SIDE_IDX, desiredLocal);
            LogRW<int>("practice.remoteSide[+0x684]", (uintptr_t)practice + PRACTICE_OFF_REMOTE_SIDE_IDX, newRemote);
        }

        // VERSION SPECIFIC BEHAVIOR - CRITICAL DIFFERENCE:
        //
        // E-version: Manual live swap by writing buffer pointers
        //   - Writes buffer pointers at +0x338/+0x33C to game exe addresses
        //   - Immediately swaps input routing without reinitialization
        //
        // H/I-version: Official switch mechanism using flag at +36
        //   - Setting *(Practice+36)=1 triggers full reinitialization on next tick
        //   - Engine handles ALL state updates (buffers, pointers, etc.)
        //   - From decompilation (h): if (*(_DWORD *)(this + 36) == 1) { sub_1006D320(...); *(_DWORD *)(this + 36) = 0; }
        //
        {
            EfzRevivalVersion ver = GetEfzRevivalVersion();
            
            if (ver == EfzRevivalVersion::Revival102e) {
                // E-version: Manual buffer pointer swap
                uintptr_t efzBase = GetEFZBase();
                uintptr_t bufP1 = efzBase + 0x390104;  // P1 buffer in game exe
                uintptr_t bufP2 = efzBase + 0x390108;  // P2 buffer in game exe
                
                std::ostringstream ossBuffers;
                ossBuffers << "[SWITCH] Game executable buffers - P1: 0x" << std::hex << std::uppercase << bufP1
                           << " | P2: 0x" << bufP2 << " (efz.exe base: 0x" << efzBase << ")";
                LogOut(ossBuffers.str(), true);
                DebugLog::Write(ossBuffers.str());
                
                uintptr_t primary = (desiredLocal == 0) ? bufP1 : bufP2;
                uintptr_t secondary = (desiredLocal == 0) ? bufP2 : bufP1;
                
                LogRWPtr("practice.sideBuf.primary[+0x338]", (uintptr_t)practice + PRACTICE_OFF_SIDE_BUF_PRIMARY, primary);
                LogRWPtr("practice.sideBuf.secondary[+0x33C]", (uintptr_t)practice + PRACTICE_OFF_SIDE_BUF_SECONDARY, secondary);
                
                // Mirror init by updating INIT_SOURCE too (so next reinit stays consistent)
                LogRW<int>("practice.initSource[+0x944]", (uintptr_t)practice + PRACTICE_OFF_INIT_SOURCE_SIDE, desiredLocal);
            } else {
                // H/I-version: Make the other side local and swap mapping once, matching engine init semantics.
                // Steps:
                // 1) Update Practice local/remote to desired/newRemote
                // 2) Clear the engine switch flag (+36) to prevent a second swap in the tail
                // 3) Call CleanupPair(ctx) exactly once (engine swap helper)
                // 4) Update engine-facing flags (active player, CPU) and GUI pos so UI/control roles match
                // 5) Return (no further map reset/refresh here)

                // Clear switch-needed flag to prevent tail double-swap; we'll perform a single immediate swap here.
                LogRW<int>("practice.switchFlag[+36]", (uintptr_t)practice + 36, 0);

                // First commit new local/remote indices to reflect the chosen local side
                uintptr_t offLocal  = (uintptr_t)practice + EFZ_Practice_LocalSideOffset();
                uintptr_t offRemote = (uintptr_t)practice + EFZ_Practice_RemoteSideOffset();
                LogRW<int>("practice.localSide[h/i]", offLocal, desiredLocal);
                LogRW<int>("practice.remoteSide[h/i]", offRemote, newRemote);

                // Then perform a single swap via CleanupPair on patch ctx so the new local gets the previous local's controls
                HMODULE hMod = GetModuleHandleA("EfzRevival.dll");
                uintptr_t ctxRva = EFZ_RVA_PatchCtx();
                uintptr_t cleanRva = EFZ_RVA_CleanupPair(); // sub_1006D320(h) / sub_1006D5F0(i)
                if (hMod && ctxRva && cleanRva) {
                    auto fnCleanup = reinterpret_cast<int(__thiscall*)(void*)>(reinterpret_cast<uintptr_t>(hMod) + cleanRva);
                    void* patchCtx = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(hMod) + ctxRva);
                    int rc = 0;
                    bool ok = SehSafe_CleanupPair(fnCleanup, patchCtx, &rc);
                    std::ostringstream oss; oss << "[SWITCH][H/I] CleanupPair(ctx) -> "
                        << (ok?"rc=":"EXC rc=") << rc << " ctx=0x" << std::hex << (uintptr_t)patchCtx;
                    LogOut(oss.str(), true);
                } else {
                    LogOut("[SWITCH][H/I] CleanupPair or ctx not available; skipped", true);
                }

                // Update engine-facing flags and GUI to reflect the new local side
                uintptr_t efzBase = GetEFZBase();
                if (efzBase) {
                    uintptr_t gameStatePtr = 0;
                    if (SafeReadMemory(efzBase + EFZ_BASE_OFFSET_GAME_STATE, &gameStatePtr, sizeof(gameStatePtr)) && gameStatePtr) {
                        uint8_t activePlayer = (uint8_t)desiredLocal;        // 0=P1, 1=P2
                        uint8_t p2CpuFlag    = (desiredLocal == 1) ? 0u : 1u; // P2 human when local==1
                        uint8_t p1CpuFlag    = (uint8_t)(1u - p2CpuFlag);
                        LogRW<uint8_t>("engine.activePlayer[+4930]", gameStatePtr + GAMESTATE_OFF_ACTIVE_PLAYER, activePlayer);
                        LogRW<uint8_t>("engine.P2_CPU_FLAG[+4931]", gameStatePtr + GAMESTATE_OFF_P2_CPU_FLAG, p2CpuFlag);
                        LogRW<uint8_t>("engine.P1_CPU_FLAG[+4932]", gameStatePtr + GAMESTATE_OFF_P1_CPU_FLAG, p1CpuFlag);
                        uint8_t guiPos = (desiredLocal == 0) ? 1u : 0u; // 1 when P1 local, 0 when P2 local
                        LogRW<uint8_t>("practice.GUI_POS[+0x24]", (uintptr_t)practice + PRACTICE_OFF_GUI_POS, guiPos);
                    } else {
                        LogOut("[SWITCH][H/I] Game state pointer not available; engine flags not updated", true);
                    }
                }

                // Align our AI control flags with the new local side (local = Human, remote = AI)
                if (desiredLocal == 1) {
                    SetAIControlFlag(1, /*human=*/false); // P1 AI
                    SetAIControlFlag(2, /*human=*/true);  // P2 Human
                    LogOut("[SWITCH][H/I] Control roles: P1=AI, P2=Human", true);
                } else {
                    SetAIControlFlag(1, /*human=*/true);  // P1 Human
                    SetAIControlFlag(2, /*human=*/false); // P2 AI
                    LogOut("[SWITCH][H/I] Control roles: P1=Human, P2=AI", true);
                }

                // Arm late neutralization for the side becoming AI
                {
                    int aiPlayer = (desiredLocal == 1) ? 1 : 2;
                    (void)NeutralizeMotionToken(aiPlayer);
                    InputHook_ArmTokenNeutralize(aiPlayer, /*alsoDoFullCleanup=*/true);
                }

                DebugLog::Write("[SWITCH][H/I] Single-swap path finished; returning");
                DebugLog::Write("========================================");
                return true;
            }
        }

        // Ensure control roles match the new local side (E only). H/I path returns earlier above.
        // desiredLocal == 0 -> P1 Human, P2 AI
        // desiredLocal == 1 -> P2 Human, P1 AI
        if (ver == EfzRevivalVersion::Revival102e) {
            bool p1HumanBefore = IsAIControlFlagHuman(1);
            bool p2HumanBefore = IsAIControlFlagHuman(2);
            if (desiredLocal == 1) {
                SetAIControlFlag(1, /*human=*/false);
                SetAIControlFlag(2, /*human=*/true);
                LogOut("[SWITCH] Control roles: P1=AI, P2=Human", true);
            } else {
                SetAIControlFlag(1, /*human=*/true);
                SetAIControlFlag(2, /*human=*/false);
                LogOut("[SWITCH] Control roles: P1=Human, P2=AI", true);
            }
            bool p1HumanAfter = IsAIControlFlagHuman(1);
            bool p2HumanAfter = IsAIControlFlagHuman(2);
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
        if (ver == EfzRevivalVersion::Revival102e) {
            uintptr_t efzBase = GetEFZBase();
            if (efzBase) {
                uintptr_t gameStatePtr = 0;
                if (SafeReadMemory(efzBase + EFZ_BASE_OFFSET_GAME_STATE, &gameStatePtr, sizeof(gameStatePtr)) && gameStatePtr) {
                    uint8_t p2CpuFlag = (desiredLocal == 1) ? 0u : 1u;
                    uint8_t p1CpuFlag = (uint8_t)(1u - p2CpuFlag);
                    uint8_t activePlayer = (uint8_t)desiredLocal;
                    std::ostringstream oss; oss << "[SWITCH][ENGINE] gameState=0x" << std::hex << gameStatePtr
                        << " write active=" << std::dec << (int)activePlayer
                        << " P2CPU=" << (int)p2CpuFlag << " P1CPU=" << (int)p1CpuFlag;
                    LogOut(oss.str(), true);
                    LogRW<uint8_t>("engine.activePlayer[+4930]", gameStatePtr + GAMESTATE_OFF_ACTIVE_PLAYER, activePlayer);
                    LogRW<uint8_t>("engine.P2_CPU_FLAG[+4931]", gameStatePtr + GAMESTATE_OFF_P2_CPU_FLAG, p2CpuFlag);
                    LogRW<uint8_t>("engine.P1_CPU_FLAG[+4932]", gameStatePtr + GAMESTATE_OFF_P1_CPU_FLAG, p1CpuFlag);
                    uint8_t guiPos = (desiredLocal == 0) ? 1u : 0u;
                    LogRW<uint8_t>("practice.GUI_POS[+0x24]", (uintptr_t)practice + PRACTICE_OFF_GUI_POS, guiPos);
                } else {
                    LogOut("[SWITCH] Game state pointer not available; engine flags not updated", true);
                }
            }
        }

        // Now that engine/practice flags reflect the intended human side, reset per-side mapping and cleanup input pair
        // so that the rebind logic observes the correct target (fixes third-press misassignment).
    // 1.02e only; H/I returned earlier.
    PostSwitchRefresh(practice);

        // Arm late neutralization for the side becoming AI on E-version as well
        {
            int aiPlayer = (desiredLocal == 1) ? 1 : 2;
            (void)NeutralizeMotionToken(aiPlayer);
            InputHook_ArmTokenNeutralize(aiPlayer, /*alsoDoFullCleanup=*/true);
        }

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
        
        // Final verification logging with DETAILED dump
        DebugLog::Write("========================================");
        DebugLog::Write("FINAL STATE VERIFICATION");
        DebugLog::Write("========================================");
        DebugLog::LogRead("practice.localSide[FINAL]", (uintptr_t)practice + EFZ_Practice_LocalSideOffset(), checkLocal);
        DebugLog::LogRead("practice.remoteSide[FINAL]", (uintptr_t)practice + EFZ_Practice_RemoteSideOffset(), checkRemote);
        DebugLog::LogRead("practice.GUI_POS[FINAL]", (uintptr_t)practice + PRACTICE_OFF_GUI_POS, guiPosR);
        
        std::ostringstream ossFinal;
        ossFinal << "AI Control: P1=" << (p1Human ? "Human" : "AI") 
                 << ", P2=" << (p2Human ? "Human" : "AI");
        DebugLog::Write(ossFinal.str());
        
        // Dump detailed state after changes
        DumpPracticeStateDetailed(practice, "AFTER SWITCH");
        
        DebugLog::Write("========================================");
        DebugLog::Write("SWITCH PLAYER OPERATION COMPLETE - SUCCESS");
        DebugLog::Write("========================================");
        DebugLog::Flush();
        
        return true;
    }

    bool ToggleLocalSide() {
        if (GetCurrentGameMode() != GameMode::Practice) return false;
        // Only switch during active match to avoid confusing selection/menus
        if (!IsMatchPhase()) {
            LogOut("[SWITCH] Ignored toggle outside of match phase", true);
            return false;
        }
        // Vanilla path: if EfzRevival is not loaded, operate on engine flags only
        if (!IsRevivalLoaded()) {
            uintptr_t efzBase = GetEFZBase();
            if (!efzBase) return false;
            uintptr_t gs = 0; if (!SafeReadMemory(efzBase + EFZ_BASE_OFFSET_GAME_STATE, &gs, sizeof(gs)) || !gs) return false;
            uint8_t curActive = 0; SafeReadMemory(gs + GAMESTATE_OFF_ACTIVE_PLAYER, &curActive, sizeof(curActive));
            int desired = (curActive == 0) ? 1 : 0;
            bool success = ApplyEngineOnlySet(desired);
            if (success) {
                s_sidesAreSwapped.store(true, std::memory_order_relaxed);
                LogOut("[SWITCH] Toggle succeeded - sides now swapped (flag set)", true);
            }
            return success;
        }
        PauseIntegration::EnsurePracticePointerCapture();
        void* p = PauseIntegration::GetPracticeControllerPtr();
        if (!p) {
            // Fallback: try direct resolution from game-mode array
            uint8_t* fb = ResolvePracticePtrFallback();
            if (!fb) {
                LogOut("[SWITCH] Practice controller not available", true);
                // As a last resort, perform an engine-only swap even though Revival is present but practice is missing
                uintptr_t efzBase = GetEFZBase();
                if (!efzBase) return false; uintptr_t gs=0; 
                if (!SafeReadMemory(efzBase + EFZ_BASE_OFFSET_GAME_STATE, &gs, sizeof(gs)) || !gs) return false;
                uint8_t curActive=0; SafeReadMemory(gs + GAMESTATE_OFF_ACTIVE_PLAYER, &curActive, sizeof(curActive));
                int desired = (curActive == 0) ? 1 : 0;
                bool success = ApplyEngineOnlySet(desired);
                if (success) {
                    s_sidesAreSwapped.store(true, std::memory_order_relaxed);
                    LogOut("[SWITCH] Toggle succeeded (fallback) - sides now swapped (flag set)", true);
                }
                return success;
            }
            LogOut("[SWITCH] Practice pointer resolved via fallback (GameModePtrArray)", true);
            p = fb;
        }
        uint8_t* practice = reinterpret_cast<uint8_t*>(p);
    int curLocal = 0; if (!SafeReadMemory((uintptr_t)practice + EFZ_Practice_LocalSideOffset(), &curLocal, sizeof(curLocal))) return false;
        int desired = (curLocal == 0) ? 1 : 0;
        bool success = ApplySet(practice, desired);
        if (success) {
            s_sidesAreSwapped.store(true, std::memory_order_relaxed);
            LogOut("[SWITCH] Toggle succeeded - sides now swapped (flag set)", true);
        }
        return success;
    }

    bool SetLocalSide(int sideIdx) {
        if (GetCurrentGameMode() != GameMode::Practice) return false;
        if (!IsMatchPhase()) {
            LogOut("[SWITCH] Ignored set outside of match phase", true);
            return false;
        }
        if (!IsRevivalLoaded()) {
            return ApplyEngineOnlySet(sideIdx);
        }
        PauseIntegration::EnsurePracticePointerCapture();
        void* p = PauseIntegration::GetPracticeControllerPtr();
        if (!p) {
            uint8_t* fb = ResolvePracticePtrFallback();
            if (!fb) {
                LogOut("[SWITCH] Practice controller not available", true);
                // Fallback to engine-only path
                return ApplyEngineOnlySet(sideIdx);
            }
            LogOut("[SWITCH] Practice pointer resolved via fallback (GameModePtrArray)", true);
            p = fb;
        }
        return ApplySet(reinterpret_cast<uint8_t*>(p), sideIdx);
    }

    bool ResetControlMappingForMenusToP1() {
        // Only operate in Practice mode
        if (GetCurrentGameMode() != GameMode::Practice) return false;
        
        // Prevent repeated logs per Character Select instance
        UpdateCsCycleState();
        // Vanilla: ensure routing swap is disabled
        SetVanillaSwapInputRouting(false);

        // If EfzRevival is not loaded, nothing else to do for menus; return success
        HMODULE h = GetModuleHandleA("EfzRevival.dll");
        if (!h) {
            LogOut("[SWITCH][MENU] Vanilla routing reset for menus (P1 controls -> P1)", true);
            return true;
        }

        // With Revival: set Practice local=0 (P1), remote=1, align GUI_POS, and refresh mapping block.
        PauseIntegration::EnsurePracticePointerCapture();
        uint8_t* practice = reinterpret_cast<uint8_t*>(PauseIntegration::GetPracticeControllerPtr());
        if (!practice) {
            practice = ResolvePracticePtrFallback();
        }
        if (!practice) {
            // Log only once per CS instance
            bool firstThisCS = !s_loggedNoPracticeThisCS.exchange(true, std::memory_order_relaxed);
            if (firstThisCS) {
                if (!s_loggedNoPracticeEver.exchange(true, std::memory_order_relaxed)) {
                    LogOut("[SWITCH][MENU] Practice controller not available; only vanilla routing reset applied", true);
                }
            }
            return false;
        }

        // Check if already at default state (local=0, remote=1, GUI_POS=1)
        int currentLocal = -1, currentRemote = -1;
        uint8_t currentGuiPos = 0xFF;
        SafeReadMemory((uintptr_t)practice + EFZ_Practice_LocalSideOffset(), &currentLocal, sizeof(currentLocal));
        SafeReadMemory((uintptr_t)practice + EFZ_Practice_RemoteSideOffset(), &currentRemote, sizeof(currentRemote));
        SafeReadMemory((uintptr_t)practice + PRACTICE_OFF_GUI_POS, &currentGuiPos, sizeof(currentGuiPos));
        
        bool practiceStateIsDefault = (currentLocal == 0 && currentRemote == 1 && currentGuiPos == 1);
        
        // Also check engine CPU flags to see if they match the default Practice mapping
        bool engineFlagsNeedReset = false;
        uintptr_t efzBase = GetEFZBase();
        if (efzBase) {
            uintptr_t gameStatePtr = 0;
            if (SafeReadMemory(efzBase + EFZ_BASE_OFFSET_GAME_STATE, &gameStatePtr, sizeof(gameStatePtr)) && gameStatePtr) {
                uint8_t p1Cpu = 0xFF, p2Cpu = 0xFF;
                SafeReadMemory(gameStatePtr + GAMESTATE_OFF_P1_CPU_FLAG, &p1Cpu, sizeof(p1Cpu));
                SafeReadMemory(gameStatePtr + GAMESTATE_OFF_P2_CPU_FLAG, &p2Cpu, sizeof(p2Cpu));
                // Default Practice mapping: P1 human (0), P2 CPU (1)
                engineFlagsNeedReset = (p1Cpu != 0 || p2Cpu != 1);
            }
        }
        
        // Check if sides were actually swapped during match using tracking flag
        bool sidesWereSwapped = s_sidesAreSwapped.load(std::memory_order_relaxed);
        
        if (practiceStateIsDefault && !engineFlagsNeedReset && !sidesWereSwapped) {
            // Already at default and no swap occurred; no need to refresh
            // Clear flag just in case (shouldn't be set, but defensive programming)
            s_sidesAreSwapped.store(false, std::memory_order_relaxed);
            static std::atomic<bool> s_loggedAlreadyDefault{false};
            if (!s_loggedAlreadyDefault.exchange(true, std::memory_order_relaxed)) {
                LogOut("[SWITCH][MENU] Already at default P1 local mapping; skipping refresh", true);
            }
            return true;
        }

        // If sides were swapped during match, need full PostSwitchRefresh to restore input routing
        if (sidesWereSwapped) {
            // Sides were swapped during match - need full PostSwitchRefresh to restore
            LogOut("[SWITCH][MENU] Sides were swapped during match; performing full reset via PostSwitchRefresh", true);
            
            // Write local/remote indices first
            int local = 0, remote = 1;
            (void)SafeWriteMemory((uintptr_t)practice + EFZ_Practice_LocalSideOffset(), &local, sizeof(local));
            (void)SafeWriteMemory((uintptr_t)practice + EFZ_Practice_RemoteSideOffset(), &remote, sizeof(remote));
            uint8_t guiPos = 1u; 
            (void)SafeWriteMemory((uintptr_t)practice + PRACTICE_OFF_GUI_POS, &guiPos, sizeof(guiPos));
            
            // Call PostSwitchRefresh to physically restore input routing
            PostSwitchRefresh(practice, /*explicitLocal=*/0);
            
            // Also restore engine CPU flags to default Practice state (P1=human, P2=CPU)
            if (efzBase) {
                uintptr_t gameStatePtr = 0;
                if (SafeReadMemory(efzBase + EFZ_BASE_OFFSET_GAME_STATE, &gameStatePtr, sizeof(gameStatePtr)) && gameStatePtr) {
                    uint8_t p1Human = 0, p2Cpu = 1;
                    SafeWriteMemory(gameStatePtr + GAMESTATE_OFF_P1_CPU_FLAG, &p1Human, sizeof(p1Human));
                    SafeWriteMemory(gameStatePtr + GAMESTATE_OFF_P2_CPU_FLAG, &p2Cpu, sizeof(p2Cpu));
                    LogOut("[SWITCH][MENU] Restored CPU flags after swap: P1=0(human), P2=1(CPU)", true);
                }
            }
            
            // Clear the swap flag since we've restored default state
            s_sidesAreSwapped.store(false, std::memory_order_relaxed);
            
            LogOut("[SWITCH][MENU] Reset mapping for menus: local=P1, remote=P2, GUI_POS=1 (with input swap, flag cleared)", true);
            return true;
        }

        // Practice state is default but engine flags need fixing - just write the flags
        if (engineFlagsNeedReset && efzBase) {
            uintptr_t gameStatePtr = 0;
            if (SafeReadMemory(efzBase + EFZ_BASE_OFFSET_GAME_STATE, &gameStatePtr, sizeof(gameStatePtr)) && gameStatePtr) {
                uint8_t p1Human = 0, p2Cpu = 1;
                SafeWriteMemory(gameStatePtr + GAMESTATE_OFF_P1_CPU_FLAG, &p1Human, sizeof(p1Human));
                SafeWriteMemory(gameStatePtr + GAMESTATE_OFF_P2_CPU_FLAG, &p2Cpu, sizeof(p2Cpu));
                LogOut("[SWITCH][MENU] Fixed engine CPU flags without input swap", true);
            }
        }

        return true;
    }

    void ClearSwapFlag() {
        s_sidesAreSwapped.store(false, std::memory_order_relaxed);
    }
}
