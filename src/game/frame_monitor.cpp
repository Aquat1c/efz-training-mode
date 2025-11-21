#include "../include/game/frame_monitor.h"
#include "../include/game/auto_airtech.h"
#include "../include/game/auto_jump.h"
#include "../include/game/auto_action.h" // ensure ClearAllAutoActionTriggers declaration
#include "../include/game/frame_analysis.h"
#include "../include/game/frame_advantage.h"
#include "../include/core/constants.h"
#include "../include/utils/utilities.h"
#include "../include/utils/bgm_control.h"
#include "../include/core/memory.h"
#include "../include/core/logger.h"
#include "../include/gui/overlay.h"
#include "../include/game/game_state.h"
#include "../include/game/per_frame_sample.h" // unified sampling context
#include "../include/input/input_buffer.h"
#include "../include/utils/config.h"
#include "../include/input/input_motion.h"
#include "../include/utils/network.h"
#include "../include/utils/pause_integration.h" // PauseIntegration::EnsurePracticePointerCapture/GetPracticeControllerPtr
#include "../include/utils/switch_players.h"    // SwitchPlayers::ResetControlMappingForMenusToP1
#define DISABLE_ATTACK_READER 1
#include "../include/game/attack_reader.h"
#include "../include/game/practice_patch.h"
#include "../include/game/character_settings.h"
#include "../include/game/macro_controller.h"
#include "../include/game/always_rg.h"
#include "../include/game/random_rg.h"
#include "../include/game/random_block.h"
#include "../include/game/practice_offsets.h"   // GAMESTATE_OFF_* and practice controller offsets
#include "../include/input/injection_control.h"  // g_forceBypass/g_injectImmediateOnly/g_pollOverride*
#include "../include/input/input_core.h"         // AI_CONTROL_FLAG_OFFSET
#ifndef CLEAR_ALL_AUTO_ACTION_TRIGGERS_FWD
#define CLEAR_ALL_AUTO_ACTION_TRIGGERS_FWD
void ClearAllAutoActionTriggers();
#endif
#include <deque>
#include <vector>
#include <chrono>
#include <sstream>
#include <algorithm>
#include <thread>
#include <atomic>
#include <iomanip>

// Compile-time gate for verbose Character Select diagnostics (set to 1 locally when needed)
#ifndef ENABLE_CS_DEBUG_LOGS
#define ENABLE_CS_DEBUG_LOGS 0
#endif

// File-scope static variables for state tracking
static uint8_t p1LastFacing = 0;
static uint8_t p2LastFacing = 0;
static bool facingInitialized = false;

// Button state tracking
static uint8_t p1LastButtons[4] = {0, 0, 0, 0}; // A, B, C, D
static uint8_t p2LastButtons[4] = {0, 0, 0, 0}; // A, B, C, D
static bool buttonStateInitialized = false;

MonitorState state = Idle;

static GameMode s_previousGameMode = GameMode::Unknown;
static bool s_wasActive = false;
static GamePhase s_lastPhase = GamePhase::Unknown;  // NEW phase tracker
static bool s_pendingOverlayReinit = false; // Set when we return to a valid mode but chars aren't initialized yet
// NEW: Debounce for CharacterSelect trigger clearing to avoid false positives mid-match
static int s_characterSelectPhaseFrames = 0; // counts consecutive frames seen as CharacterSelect
// Character Select live logger (config-gated)
static uint8_t s_csLastActive = 0xFF;
static uint8_t s_csLastP2Cpu = 0xFF;
static uint8_t s_csLastP1Cpu = 0xFF;
static uint32_t s_csLastP1Ai = 0xFFFFFFFFu;
static uint32_t s_csLastP2Ai = 0xFFFFFFFFu;
static int s_csLogDecim = 0; // throttle
// Character Select input edge logger (active player's A/B/C/D press edges)
static bool s_csBtnInit = false;
static uint8_t s_csPrevBtns[4] = {0,0,0,0}; // A,B,C,D for active player last sample
// Character Select CPU-flag guard (fixes post-Practice return when both flags become 1)
// Snapshot/restore state for Character Select control flags (Practice mode)
struct CSCtrlSnapshot {
    bool     valid{false};
    uint8_t  active{0xFF};   // GAMESTATE_OFF_ACTIVE_PLAYER (0=P1,1=P2)
    uint8_t  p1Cpu{0xFF};    // GAMESTATE_OFF_P1_CPU_FLAG (1=CPU,0=Human)
    uint8_t  p2Cpu{0xFF};    // GAMESTATE_OFF_P2_CPU_FLAG (1=CPU,0=Human)
};
static CSCtrlSnapshot s_csBaseline{};      // captured on first stable CS in Practice
// ---------------- Character Select Handling Strategy ----------------
// We previously attempted to enforce baseline CPU flags and active player DURING the Character Select (CS) phase.
// That caused a regression: both controllers were required to "join" before the engine would proceed, because we
// were overwriting the engine's dynamic writes to +4930 (active player) and CPU flags (+4931 / +4932) mid-CS.
//
// Engine behavior
//   * During CS the engine itself toggles +4930 as users navigate sides.
//   * CPU flags (+4931 P2, +4932 P1) remain authoritative for human/CPU designation but should not be forced
//     while the CS menu logic is actively evaluating inputs.
//
// Fix approach (two parts):
//   1. DEFER CPU flag enforcement until AFTER leaving CS. We snapshot our desired baseline beforehand; during CS
//      we never write +4931/+4932. When CS ends, we apply baseline exactly once and log [CS][APPLY_POST].
//   2. DO NOT force active player (+4930) during CS; instead, we only align the overlay GUI position (GUI_POS) to
//      the engine's current active. This prevents fighting the engine's selection logic while still keeping the GUI
//      visually consistent.
//   3. RESTORE menu mapping to default P1-local if the user performed a side swap in-match. After a short debounce
//      (>=30 CS frames) call SwitchPlayers::ResetControlMappingForMenusToP1(). This affects practice local/remote
//      indices and GUI_POS ONLY (plus disables vanilla routing swap) and intentionally avoids touching CPU flags
//      or active player memory. Log tag: [CS][PRE][UNSWITCH].
//
// Guard variables below govern one-shot actions per CS entry:
static bool s_csDidSnapshotApply = false;   // baseline snapshot applied once per CS entry
static bool s_csDidPersistentTriggerClear = false; // auto-action triggers cleared once per CS entry
static bool s_pendingPostCsCpuApply = false; // deferred CPU apply needed upon CS exit
static bool s_csDidPreUnswitch = false;      // menu mapping reset executed once per CS entry

// Global once-only guards for noisy CS alignment-related logs
static std::atomic<bool> s_logOnce_CsSnapSkip{false};
static std::atomic<bool> s_logOnce_CsRestoreAlign{false};
static std::atomic<bool> s_logOnce_CsRestoreBaselineMatch{false};
static std::atomic<bool> s_logOnce_CsSnapFallback{false};
static std::atomic<bool> s_logOnce_CsApplyPost{false};
static std::atomic<bool> s_practiceHintShown{false}; // prevents repeated Practice overlay hints per session

// Published unified per-frame sample (updated once per 192Hz loop iteration)
PerFrameSample g_lastSample{};

// Global (extern defined elsewhere) suppression flag for auto-action clear logging; declare if missing
extern std::atomic<bool> g_suppressAutoActionClearLogging;

static uintptr_t fm_lastP1Ptr = 0;
static uintptr_t fm_lastP2Ptr = 0;
static uintptr_t fm_lastMoveAddr1 = 0;
static uintptr_t fm_lastMoveAddr2 = 0;
static bool      fm_lastCharsInit = false;
static int       fm_moveReadFailStreak1 = 0;
static int       fm_moveReadFailStreak2 = 0;
static int       fm_overrunWarnCounter = 0;
// phase change logging is centralized; remove per-frame local trackers

static std::string FM_Hex(uintptr_t v) {
    std::ostringstream oss;
    oss << "0x" << std::hex << std::uppercase << v;
    return oss.str();
}

static void MaybeShowPracticeOverlayHintOnce() {
    if (s_practiceHintShown.load(std::memory_order_relaxed)) return;
    if (!g_featuresEnabled.load()) return;
    if (!DirectDrawHook::isHooked) return;

    const auto& cfg = Config::GetSettings();
    if (!cfg.showPracticeEntryHint) return;

    bool expected = false;
    if (!s_practiceHintShown.compare_exchange_strong(expected, true)) {
        return;
    }

    auto pickKeyName = [](int keyCode) -> std::string {
        if (keyCode <= 0) return std::string();
        return Config::GetKeyName(keyCode);
    };
    std::string keyboardKey = pickKeyName(cfg.configMenuKey);
    if (keyboardKey.empty()) {
        keyboardKey = pickKeyName(cfg.toggleImGuiKey);
    }
    if (keyboardKey.empty()) {
        keyboardKey = "3"; // fallback to legacy default
    }

    std::ostringstream msg;
    msg << "Training menu: press " << keyboardKey;
    if (cfg.gpToggleMenuButton >= 0) {
        std::string padName = Config::GetGamepadButtonName(cfg.gpToggleMenuButton);
        if (!padName.empty() && padName != "-1") {
            msg << " (controller: " << padName << ")";
        }
    }
    msg << " to open.";

    DirectDrawHook::AddMessage(msg.str(), "PRACTICE_HINT", RGB(180, 235, 255), 4200, 0, 100);
}

// Framestep debug state (visible via ImGui only)
static std::atomic<int> g_fsdbgSteps{-1};
static std::atomic<bool> g_fsdbgActive{false};

// Lightweight per-iteration pointer cache to reduce repeated SafeReadMemory calls
namespace {
    struct PointerCache {
        uintptr_t base{0};
        uintptr_t gs{0};
        uintptr_t p1{0};
        uintptr_t p2{0};
        uint32_t  gen{0}; // increments once per FrameDataMonitor loop
    };
    static PointerCache s_ptrCache{};
    static std::atomic<uint32_t> s_ptrGen{0};

    static uintptr_t ResolvePlayerBaseBestEffort(int playerIndex, uintptr_t baseHint = 0) {
        uintptr_t ptr = GetPlayerBase(playerIndex);
        if (ptr) return ptr;
        uintptr_t base = baseHint ? baseHint : GetEFZBase();
        if (!base) return 0;
        uintptr_t addr = 0;
        uintptr_t offset = (playerIndex == 1) ? EFZ_BASE_OFFSET_P1 : EFZ_BASE_OFFSET_P2;
        SafeReadMemory(base + offset, &addr, sizeof(addr));
        return addr;
    }

    static uintptr_t ResolvePlayerFieldBestEffort(int playerIndex, uintptr_t fieldOffset, uintptr_t baseHint = 0) {
        uintptr_t ptr = (playerIndex == 1) ? s_ptrCache.p1 : s_ptrCache.p2;
        if (!ptr) ptr = ResolvePlayerBaseBestEffort(playerIndex, baseHint);
        return ptr ? ptr + fieldOffset : 0;
    }

    inline void RefreshPointerCache() {
        // Read all primary pointers once; best-effort only
        s_ptrCache.base = GetEFZBase();
        s_ptrCache.gs = GetGameStatePtr();
        s_ptrCache.p1 = ResolvePlayerBaseBestEffort(1, s_ptrCache.base);
        s_ptrCache.p2 = ResolvePlayerBaseBestEffort(2, s_ptrCache.base);
        s_ptrCache.gen = s_ptrGen.fetch_add(1, std::memory_order_relaxed) + 1;
    }
}

FrameStepDebugInfo GetFrameStepDebugInfo() {
    FrameStepDebugInfo info{};
    info.active = g_fsdbgActive.load(std::memory_order_relaxed);
    info.steps = g_fsdbgSteps.load(std::memory_order_relaxed);
    return info;
}

// --- Character Select diagnostics & reset helpers ---
static void LogCharacterSelectDiagnostics() {
    LogOut("[CS][DIAG] ---- Enter Character Select ----", true);
    uintptr_t base = GetEFZBase();
    if (!base) {
        LogOut("[CS][DIAG] efz base: <null>", true);
        return;
    }
    std::ostringstream os;
    os << "[CS][DIAG] efz base: " << FM_Hex(base);
    LogOut(os.str(), true); os.str(""); os.clear();

    uintptr_t gs = 0;
    if (SafeReadMemory(base + EFZ_BASE_OFFSET_GAME_STATE, &gs, sizeof(gs)) && gs) {
        uint8_t active=0xFF, p2cpu=0xFF, p1cpu=0xFF;
        SafeReadMemory(gs + GAMESTATE_OFF_ACTIVE_PLAYER, &active, sizeof(active));
        SafeReadMemory(gs + GAMESTATE_OFF_P2_CPU_FLAG, &p2cpu, sizeof(p2cpu));
        SafeReadMemory(gs + GAMESTATE_OFF_P1_CPU_FLAG, &p1cpu, sizeof(p1cpu));
        os << "[CS][DIAG] gameState=" << FM_Hex(gs)
           << "  [+4930 active=" << (int)active
           << "] [+4931 P2CPU=" << (int)p2cpu
           << "] [+4932 P1CPU=" << (int)p1cpu << "]";
        LogOut(os.str(), true); os.str(""); os.clear();
    } else {
        LogOut("[CS][DIAG] gameState: <unavailable>", true);
    }

    // Player base pointers and AI flags (if available)
    uintptr_t p1 = ResolvePlayerBaseBestEffort(1, base);
    uintptr_t p2 = ResolvePlayerBaseBestEffort(2, base);
    os << "[CS][DIAG] P1=" << FM_Hex(p1) << "  P2=" << FM_Hex(p2);
    LogOut(os.str(), true); os.str(""); os.clear();
    if (p1) {
        uint32_t a1=0xDEADBEEF; SafeReadMemory(p1 + AI_CONTROL_FLAG_OFFSET, &a1, sizeof(a1));
        os << "[CS][DIAG] P1 AI(+0xA4): " << a1;
        LogOut(os.str(), true); os.str(""); os.clear();
    }
    if (p2) {
        uint32_t a2=0xDEADBEEF; SafeReadMemory(p2 + AI_CONTROL_FLAG_OFFSET, &a2, sizeof(a2));
        os << "[CS][DIAG] P2 AI(+0xA4): " << a2;
        LogOut(os.str(), true); os.str(""); os.clear();
    }

    // Practice controller diagnostics (best-effort)
    PauseIntegration::EnsurePracticePointerCapture();
    void* prac = PauseIntegration::GetPracticeControllerPtr();
    if (prac) {
        uint8_t* pr = reinterpret_cast<uint8_t*>(prac);
        int local=-1, remote=-1; uintptr_t prim=0, sec=0; int initSrc=-1; uint8_t guiPos=0xFF;
        SafeReadMemory((uintptr_t)pr + PRACTICE_OFF_LOCAL_SIDE_IDX, &local, sizeof(local));
        SafeReadMemory((uintptr_t)pr + PRACTICE_OFF_REMOTE_SIDE_IDX, &remote, sizeof(remote));
        SafeReadMemory((uintptr_t)pr + PRACTICE_OFF_SIDE_BUF_PRIMARY, &prim, sizeof(prim));
        SafeReadMemory((uintptr_t)pr + PRACTICE_OFF_SIDE_BUF_SECONDARY, &sec, sizeof(sec));
        SafeReadMemory((uintptr_t)pr + PRACTICE_OFF_INIT_SOURCE_SIDE, &initSrc, sizeof(initSrc));
        SafeReadMemory((uintptr_t)pr + PRACTICE_OFF_GUI_POS, &guiPos, sizeof(guiPos));
        os << "[CS][DIAG] Practice.this=" << FM_Hex((uintptr_t)pr)
           << "  local=" << local << " remote=" << remote
           << "  primary=" << FM_Hex(prim) << " secondary=" << FM_Hex(sec)
           << "  initSrc=" << initSrc << "  GUI_POS(+0x24)=" << (int)guiPos;
        LogOut(os.str(), true); os.str(""); os.clear();
    } else {
        LogOut("[CS][DIAG] Practice controller: <unavailable>", true);
    }

    // Our override/patch states
     os << "[CS][DIAG] overrides: p2Overridden=" << (g_p2ControlOverridden?"1":"0")
         << "  pendingRestore=" << (g_pendingControlRestore.load()?"1":"0");
    LogOut(os.str(), true); os.str(""); os.clear();

    os << "[CS][DIAG] bufferFreezeActive=" << (g_bufferFreezingActive.load()?"1":"0")
       << " activeFreezePlayer=" << g_activeFreezePlayer.load();
    LogOut(os.str(), true); os.str(""); os.clear();

    bool po1 = g_pollOverrideActive[1].load();
    bool po2 = g_pollOverrideActive[2].load();
    os << "[CS][DIAG] pollOverride P1=" << (po1?"1":"0") << " P2=" << (po2?"1":"0");
    LogOut(os.str(), true); os.str(""); os.clear();

    LogOut("[CS][DIAG] ---------------------------------------", true);
}

// Per-frame Character Select logger: samples engine and per-character flags; emits only on change
static void CharacterSelectLiveLoggerTick() {
    if (!Config::GetSettings().enableCharacterSelectLogger) return;
    // Throttle to ~10 Hz to keep logs readable
    if ((++s_csLogDecim % 19) != 0) return; // 192/19 ~= 10 Hz
    uintptr_t base = s_ptrCache.base; if (!base) return;
    uintptr_t gs = s_ptrCache.gs; if (!gs) return;
    uint8_t active=0xFF, p2cpu=0xFF, p1cpu=0xFF;
    SafeReadMemory(gs + GAMESTATE_OFF_ACTIVE_PLAYER, &active, sizeof(active));
    SafeReadMemory(gs + GAMESTATE_OFF_P2_CPU_FLAG, &p2cpu, sizeof(p2cpu));
    SafeReadMemory(gs + GAMESTATE_OFF_P1_CPU_FLAG, &p1cpu, sizeof(p1cpu));
    uintptr_t p1 = s_ptrCache.p1, p2 = s_ptrCache.p2;
    uint32_t a1=0xFFFFFFFFu, a2=0xFFFFFFFFu;
    if (p1) SafeReadMemory(p1 + AI_CONTROL_FLAG_OFFSET, &a1, sizeof(a1));
    if (p2) SafeReadMemory(p2 + AI_CONTROL_FLAG_OFFSET, &a2, sizeof(a2));
    bool changed = (active!=s_csLastActive) || (p2cpu!=s_csLastP2Cpu) || (p1cpu!=s_csLastP1Cpu) || (a1!=s_csLastP1Ai) || (a2!=s_csLastP2Ai);
    if (changed) {
        std::ostringstream os;
        os << "[CS][LIVE] +4930 active=" << (int)active
           << "  +4931 P2CPU=" << (int)p2cpu
           << "  +4932 P1CPU=" << (int)p1cpu
           << "  P1.AI=" << (p1? (int)a1 : -1)
           << "  P2.AI=" << (p2? (int)a2 : -1);
        LogOut(os.str(), true);
        s_csLastActive = active; s_csLastP2Cpu = p2cpu; s_csLastP1Cpu = p1cpu; s_csLastP1Ai = a1; s_csLastP2Ai = a2;
    }
}

// Log down-edges for A/B/C/D buttons for the active player on Character Select
static void CharacterSelectInputEdgeLoggerTick() {
    if (!Config::GetSettings().enableCharacterSelectLogger) return;
    uintptr_t base = s_ptrCache.base; if (!base) return;
    // Determine active player and resolve struct
    uintptr_t gs = s_ptrCache.gs; if (!gs) return;
    uint8_t active = 0; SafeReadMemory(gs + GAMESTATE_OFF_ACTIVE_PLAYER, &active, sizeof(active));
    uintptr_t pStruct = (active == 0) ? s_ptrCache.p1 : s_ptrCache.p2;
    if (!pStruct) return;
    // Sample immediate input registers
    uint8_t a=0,b=0,c=0,d=0;
    SafeReadMemory(pStruct + INPUT_BUTTON_A_OFFSET, &a, sizeof(a));
    SafeReadMemory(pStruct + INPUT_BUTTON_B_OFFSET, &b, sizeof(b));
    SafeReadMemory(pStruct + INPUT_BUTTON_C_OFFSET, &c, sizeof(c));
    SafeReadMemory(pStruct + INPUT_BUTTON_D_OFFSET, &d, sizeof(d));
    uint8_t cur[4] = { a ? 1u : 0u, b ? 1u : 0u, c ? 1u : 0u, d ? 1u : 0u };
    if (!s_csBtnInit) {
        for (int i=0;i<4;++i) s_csPrevBtns[i] = cur[i];
        s_csBtnInit = true;
        return;
    }
    // Detect down edges
    const char* names[4] = {"A","B","C","D"};
    bool any = false;
    std::ostringstream os;
    for (int i=0;i<4;++i) {
        if (s_csPrevBtns[i] == 0 && cur[i] != 0) {
            if (!any) {
                os << "[CS][INPUT] P" << (int)(active+1) << " pressed:";
                any = true;
            }
            os << " " << names[i];
        }
    }
    if (any) {
        LogOut(os.str(), true);
    }
    for (int i=0;i<4;++i) s_csPrevBtns[i] = cur[i];
}

static void ResetControlOnCharacterSelect() {
    // Clear any residual input overrides and freezes
    if (g_bufferFreezingActive.load()) {
        StopBufferFreezing();
    }
    // Restore P2 control if our override is active
    if (g_p2ControlOverridden) {
        RestoreP2ControlState();
    }
    g_pendingControlRestore.store(false);

    // Clear poll override/injection flags for both players
    for (int i = 1; i <= 2; ++i) {
        g_pollOverrideActive[i].store(false);
        g_pollOverrideMask[i].store(0);
        g_forceBypass[i].store(false);
        g_injectImmediateOnly[i].store(false);
        g_manualInputOverride[i].store(false);
    }
    // At Character Select: diagnostics only; do NOT mutate game/practice control state here.
    uintptr_t base = GetEFZBase();
    uintptr_t gs = 0;
    if (base && SafeReadMemory(base + EFZ_BASE_OFFSET_GAME_STATE, &gs, sizeof(gs)) && gs) {
        uint8_t activeNow = 0xFF;
        SafeReadMemory(gs + GAMESTATE_OFF_ACTIVE_PLAYER, &activeNow, sizeof(activeNow));
        if (detailedLogging.load()) {
            std::ostringstream os; os << "[CS][RESET][DIAG] +4930(active)=" << (int)activeNow;
            LogOut(os.str(), true);
        }
        // Diagnostic only: if both CPU flags match (both 0 or both 1), note it for later analysis.
        // We intentionally avoid writing here due to side-effects on Cancel/join behavior.
        if (detailedLogging.load()) {
            uint8_t p1Cpu = 0xFF, p2Cpu = 0xFF;
            SafeReadMemory(gs + GAMESTATE_OFF_P1_CPU_FLAG, &p1Cpu, sizeof(p1Cpu));
            SafeReadMemory(gs + GAMESTATE_OFF_P2_CPU_FLAG, &p2Cpu, sizeof(p2Cpu));
            if (p1Cpu == p2Cpu) {
                std::ostringstream os; os << "[CS][RESET][WARN] CPU flags equal at entry (P1CPU="
                                          << (int)p1Cpu << ", P2CPU=" << (int)p2Cpu << ")";
                LogOut(os.str(), true);
            }
        }
    }
}

// --- Frame snapshot store (double-buffer, lock-free) ---
namespace {
    struct SnapBuf { FrameSnapshot snap; std::atomic<uint32_t> seq{0}; };
    static SnapBuf g_snapA, g_snapB;
    static std::atomic<SnapBuf*> g_writeBuf{&g_snapA};
    static std::atomic<SnapBuf*> g_readBuf{&g_snapB};
}

static void PublishSnapshot(const FrameSnapshot &s) {
    SnapBuf* wb = g_writeBuf.load(std::memory_order_relaxed);
    uint32_t startSeq = wb->seq.load(std::memory_order_relaxed);
    if ((startSeq & 1u) == 0u) startSeq++; // make it odd (writing)
    wb->seq.store(startSeq, std::memory_order_release);
    wb->snap = s; // POD copy
    wb->seq.store(startSeq+1, std::memory_order_release); // even = stable
    // swap read/write buffers for next time
    SnapBuf* oldRead = g_readBuf.exchange(wb, std::memory_order_acq_rel);
    g_writeBuf.store(oldRead, std::memory_order_release);
}

bool TryGetLatestSnapshot(FrameSnapshot &out, unsigned int maxAgeMs) {
    SnapBuf* rb = g_readBuf.load(std::memory_order_acquire);
    uint32_t s1 = rb->seq.load(std::memory_order_acquire);
    if (s1 & 1u) return false; // being written
    FrameSnapshot tmp = rb->snap; // copy
    uint32_t s2 = rb->seq.load(std::memory_order_acquire);
    if (s1 != s2 || (s2 & 1u)) return false; // changed mid-read
    unsigned long long now = GetTickCount64();
    if (tmp.tickMs == 0 || now - tmp.tickMs > maxAgeMs) return false;
    out = tmp;
    return true;
}

bool IsValidGameMode(GameMode mode) {
    const Config::Settings& cfg = Config::GetSettings();
    return !cfg.restrictToPracticeMode || (mode == GameMode::Practice);
}

bool ShouldFeaturesBeActive() {
    // Check if we have a valid game state
    uintptr_t base = GetEFZBase();
    const Config::Settings& cfg = Config::GetSettings();
    GameMode currentMode = GetCurrentGameMode();

    bool isMatchRunning = false;
    if (base) {
        uintptr_t p1StructAddr = ResolvePlayerBaseBestEffort(1, base);
        isMatchRunning = (p1StructAddr != 0);
    }

    if (!isMatchRunning) {
        return false;
    }

    // Features are active based on game mode, not window focus
    // Window focus only affects key monitoring (handled separately in input_handler.cpp)
    return !cfg.restrictToPracticeMode || (currentMode == GameMode::Practice);
}

// Function to update the trigger status overlay with per-line coloring
void UpdateTriggerOverlay() {
    auto remove_all_triggers = []() {
        if (g_TriggerAfterBlockId != -1) { DirectDrawHook::RemovePermanentMessage(g_TriggerAfterBlockId); g_TriggerAfterBlockId = -1; }
        if (g_TriggerOnWakeupId != -1) { DirectDrawHook::RemovePermanentMessage(g_TriggerOnWakeupId); g_TriggerOnWakeupId = -1; }
        if (g_TriggerAfterHitstunId != -1) { DirectDrawHook::RemovePermanentMessage(g_TriggerAfterHitstunId); g_TriggerAfterHitstunId = -1; }
        if (g_TriggerAfterAirtechId != -1) { DirectDrawHook::RemovePermanentMessage(g_TriggerAfterAirtechId); g_TriggerAfterAirtechId = -1; }
        if (g_TriggerOnRGId != -1) { DirectDrawHook::RemovePermanentMessage(g_TriggerOnRGId); g_TriggerOnRGId = -1; }
    };

    if (!autoActionEnabled.load()) {
        remove_all_triggers();
        return;
    }

    int yPos = 140;
    const int yIncrement = 15;
    int targetPlayer = autoActionPlayer.load();

    auto getActionName = [](int actionType, int customId, int strength) -> std::string {
        std::string strengthLetter = "";
        
        // Determine strength letter (A, B, C)
        if (actionType == ACTION_QCF ||
            actionType == ACTION_DP ||
            actionType == ACTION_QCB ||
            actionType == ACTION_421 ||
            actionType == ACTION_SUPER1 ||
            actionType == ACTION_SUPER2 ||
            actionType == ACTION_236236 ||
            actionType == ACTION_214214 ||
            actionType == ACTION_641236 ||
            actionType == ACTION_463214 ||
            actionType == ACTION_412 ||
            actionType == ACTION_22 ||
            actionType == ACTION_4123641236 ||
            actionType == ACTION_6321463214) {
            
            // Convert strength number to letter
            switch(strength) {
                case 0: strengthLetter = "A"; break;
                case 1: strengthLetter = "B"; break;
                case 2: strengthLetter = "C"; break;
                default: strengthLetter = "A"; break;
            }
        }

        switch (actionType) {
            case ACTION_5A: return "5A";
            case ACTION_5B: return "5B";
            case ACTION_5C: return "5C";
            case ACTION_2A: return "2A";
            case ACTION_2B: return "2B";
            case ACTION_2C: return "2C";
            case ACTION_JA: return "j.A";
            case ACTION_JB: return "j.B";
            case ACTION_JC: return "j.C";
            case ACTION_6A: return "6A";
            case ACTION_6B: return "6B";
            case ACTION_6C: return "6C";
            case ACTION_4A: return "4A";
            case ACTION_4B: return "4B";
            case ACTION_4C: return "4C";
            case ACTION_QCF: return "236" + strengthLetter; // QCF + strength
            case ACTION_DP: return "623" + strengthLetter;  // DP + strength
            case ACTION_QCB: return "214" + strengthLetter; // QCB + strength
            case ACTION_421: return "421" + strengthLetter; // Half-circle down + strength
            case ACTION_SUPER1: return "41236" + strengthLetter; // HCF + strength
            case ACTION_SUPER2: return "214236" + strengthLetter; // Hybrid replaces removed 63214
            case ACTION_236236: return "236236" + strengthLetter; // Double QCF + strength
            case ACTION_214214: return "214214" + strengthLetter; // Double QCB + strength
            case ACTION_641236: return "641236" + strengthLetter; // Pretzel variant
            case ACTION_463214: return "463214" + strengthLetter; // Reverse roll
            case ACTION_412: return "412" + strengthLetter;       // 4,1,2 partial roll
            case ACTION_22: return "22" + strengthLetter;         // Down-Down
            case ACTION_4123641236: return "4123641236" + strengthLetter; // Double 41236
            case ACTION_6321463214: return "6321463214" + strengthLetter;
            case ACTION_JUMP: return "Jump";
            case ACTION_BACKDASH: return "Backdash";
            case ACTION_FORWARD_DASH: return "Forward Dash";
            case ACTION_BLOCK: return "Block";
            case ACTION_FINAL_MEMORY: return "Final Memory";
            default: return "Unknown (" + std::to_string(actionType) + ")";
        }
    };

    // Get last active trigger for highlighting
    int lastActiveTrigger = g_lastActiveTriggerType.load();
    int lastActiveFrame = g_lastActiveTriggerFrame.load();
    int currentFrame = frameCounter.load();
    const int activeHighlightDuration = 30; // How long to show active highlight

    auto update_line = [&](int& msgId, bool isEnabled, const std::string& label, int action, 
                          int customId, int delay, int strength, int triggerType) {
        if (isEnabled) {
            // Helper: build a compact token for a single TriggerOption
            auto tokenFor = [&](int act, int str, int macro) -> std::string {
                if (macro > 0) {
                    return std::string("Macro ") + std::to_string(macro);
                }
                auto letter = [&](int s)->char { return (s<=0)?'a':(s==1?'b':'c'); };
                switch (act) {
                    case ACTION_5A: return "5a"; case ACTION_5B: return "5b"; case ACTION_5C: return "5c";
                    case ACTION_2A: return "2a"; case ACTION_2B: return "2b"; case ACTION_2C: return "2c";
                    case ACTION_JA: return "ja"; case ACTION_JB: return "jb"; case ACTION_JC: return "jc";
                    case ACTION_6A: return "6a"; case ACTION_6B: return "6b"; case ACTION_6C: return "6c";
                    case ACTION_4A: return "4a"; case ACTION_4B: return "4b"; case ACTION_4C: return "4c";
                    case ACTION_QCF: return std::string("236") + letter(str);
                    case ACTION_DP:  return std::string("623") + letter(str);
                    case ACTION_QCB: return std::string("214") + letter(str);
                    case ACTION_421: return std::string("421") + letter(str);
                    case ACTION_SUPER1: return std::string("41236") + letter(str);
                    case ACTION_SUPER2: return std::string("214236") + letter(str);
                    case ACTION_236236: return std::string("236236") + letter(str);
                    case ACTION_214214: return std::string("214214") + letter(str);
                    case ACTION_641236: return std::string("641236") + letter(str);
                    case ACTION_463214: return std::string("463214") + letter(str);
                    case ACTION_412: return std::string("412") + letter(str);
                    case ACTION_22:  return std::string("22") + letter(str);
                    case ACTION_4123641236: return std::string("4123641236") + letter(str);
                    case ACTION_6321463214: return std::string("6321463214") + letter(str);
                    case ACTION_JUMP: {
                        // strength: 0=neutral jump, 1=forward jump, 2=backward jump
                        int s = (str < 0 ? 0 : (str > 2 ? 2 : str));
                        switch (s) {
                            case 1: return "Forward Jump";
                            case 2: return "Backward Jump";
                            default: return "Jump";
                        }
                    }
                    case ACTION_BACKDASH: return "44";
                    case ACTION_FORWARD_DASH: return "66";
                    case ACTION_BLOCK: return "[4]";
                    case ACTION_FINAL_MEMORY: return "fm";
                    default: return "?";
                }
            };

            // Determine macro slot for main row display and build slash-separated summary including main row
            int macroSlot = 0;
            switch (triggerType) {
                case TRIGGER_AFTER_BLOCK: macroSlot = triggerAfterBlockMacroSlot.load(); break;
                case TRIGGER_ON_WAKEUP: macroSlot = triggerOnWakeupMacroSlot.load(); break;
                case TRIGGER_AFTER_HITSTUN: macroSlot = triggerAfterHitstunMacroSlot.load(); break;
                case TRIGGER_AFTER_AIRTECH: macroSlot = triggerAfterAirtechMacroSlot.load(); break;
                case TRIGGER_ON_RG: macroSlot = triggerOnRGMacroSlot.load(); break;
                default: break;
            }

            std::string combined;
            const TriggerOption* opts = nullptr; int optCount = 0;
            switch (triggerType) {
                case TRIGGER_AFTER_BLOCK: opts = g_afterBlockOptions; optCount = g_afterBlockOptionCount; break;
                case TRIGGER_ON_WAKEUP: opts = g_onWakeupOptions; optCount = g_onWakeupOptionCount; break;
                case TRIGGER_AFTER_HITSTUN: opts = g_afterHitstunOptions; optCount = g_afterHitstunOptionCount; break;
                case TRIGGER_AFTER_AIRTECH: opts = g_afterAirtechOptions; optCount = g_afterAirtechOptionCount; break;
                case TRIGGER_ON_RG: opts = g_onRGOptions; optCount = g_onRGOptionCount; break;
                default: break;
            }
            // Always start with the main trigger row token
            combined = tokenFor(action, strength, macroSlot);
            // Then append any enabled sub-rows in order
            if (opts && optCount > 0) {
                for (int i = 0; i < optCount; ++i) {
                    if (!opts[i].enabled) continue;
                    combined += "/";
                    combined += tokenFor(opts[i].action, opts[i].strength, opts[i].macroSlot);
                }
            }

            // If no row summary was produced, use legacy single-action display (with optional macro and delay)
            std::string text;
            if (!combined.empty()) {
                text = label + combined;
            } else {
                std::string actionName = (macroSlot > 0) ? (std::string("Macro Slot #") + std::to_string(macroSlot))
                                                         : getActionName(action, customId, strength);
                text = label + actionName;
                if (delay > 0) { text += " +" + std::to_string(delay); }
            }

            bool isActive = false;
            if (g_lastActiveTriggerType.load() == triggerType) {
                // Highlight for 96 internal frames (~0.5 seconds) after activation
                if (frameCounter.load() - g_lastActiveTriggerFrame.load() < 96) {
                    isActive = true;
                }
            }

            COLORREF color = isActive ? RGB(50, 255, 50) : RGB(255, 215, 0);

            // Position to the right side with proper margins
            // 640 is standard game width, leave 10px margin
            const int triggerX = 540; // Fixed position at the right side of the screen

            if (msgId == -1) {
                msgId = DirectDrawHook::AddPermanentMessage(text, color, triggerX, yPos);
            } else {
                DirectDrawHook::UpdatePermanentMessage(msgId, text, color);
            }
            yPos += yIncrement;
        } else {
            if (msgId != -1) {
                DirectDrawHook::RemovePermanentMessage(msgId);
                msgId = -1;
            }
        }
    };

    update_line(g_TriggerAfterBlockId, triggerAfterBlockEnabled.load(), "After Block: ", 
                triggerAfterBlockAction.load(), triggerAfterBlockCustomID.load(), 
                triggerAfterBlockDelay.load(), triggerAfterBlockStrength.load(), TRIGGER_AFTER_BLOCK);
    
    update_line(g_TriggerOnWakeupId, triggerOnWakeupEnabled.load(), "On Wakeup: ", 
                triggerOnWakeupAction.load(), triggerOnWakeupCustomID.load(), 
                triggerOnWakeupDelay.load(), triggerOnWakeupStrength.load(), TRIGGER_ON_WAKEUP);
    
    update_line(g_TriggerAfterHitstunId, triggerAfterHitstunEnabled.load(), "After Hitstun: ", 
                triggerAfterHitstunAction.load(), triggerAfterHitstunCustomID.load(), 
                triggerAfterHitstunDelay.load(), triggerAfterHitstunStrength.load(), TRIGGER_AFTER_HITSTUN);
    
    update_line(g_TriggerAfterAirtechId, triggerAfterAirtechEnabled.load(), "After Airtech: ", 
                triggerAfterAirtechAction.load(), triggerAfterAirtechCustomID.load(), 
                triggerAfterAirtechDelay.load(), triggerAfterAirtechStrength.load(), TRIGGER_AFTER_AIRTECH);

    // New: On RG trigger line
    update_line(g_TriggerOnRGId, triggerOnRGEnabled.load(), "On RG: ",
                triggerOnRGAction.load(), triggerOnRGCustomID.load(),
                triggerOnRGDelay.load(), triggerOnRGStrength.load(), TRIGGER_ON_RG);

    // (Removed) AI control flag overlay: now shown only in the Stats/ImGui panel to declutter on-screen HUD.
}

void FrameDataMonitor() {
    using clock = std::chrono::high_resolution_clock;
    
    if (Config::GetSettings().enableFpsDiagnostics || detailedLogging.load()) {
        LogOut("[FRAME MONITOR] Starting frame monitoring at 192fps for maximum precision", true);
    }
    
    // Use high (but not time-critical) priority to avoid starving DWM/GPU queues
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    
    short prevMoveID1 = -1, prevMoveID2 = -1;
    // Update frame time to match 192fps instead of 60fps
    // 1,000,000,000 / 192 = 5,208,333 nanoseconds per frame
    const auto targetFrameTime = std::chrono::nanoseconds(5208333);
    static uintptr_t cachedMoveIDAddr1 = 0;
    static uintptr_t cachedMoveIDAddr2 = 0;
    static int addressCacheCounter = 0;
    auto lastLogTime = clock::now();
    int framesSinceLastLog = 0;
    extern std::atomic<bool> g_isShuttingDown;

    
    // Cache values that don't change frequently:
    static bool detailedLogCached = false;
    static int logCounter = 0;

    static short lastLoggedMoveID1 = -1;
    static short lastLoggedMoveID2 = -1;
    static std::atomic<int> moveLogCooldown{0};
    
    // High precision timer resolution (enable only during Match to reduce system-wide timer pressure)
    bool highResActive = false;
    // Timing diagnostics (per 960 internal frames)
    long long driftAccum = 0; // sum of (actual - target) ns
    long long absDriftAccum = 0;
    long long maxLate = 0;
    long long maxEarly = 0; // negative max
    int driftSamples = 0;
    int oversleepCount = 0; // frames that exceeded 2x target

    // New: fine grained section timing (optional on demand)
    bool sectionTiming = false; // toggle via debugger if needed
    long long sec_mem = 0, sec_logic = 0, sec_features = 0; // accumulators
    int sec_samples = 0;

    // Improved scheduling target time (accumulative to avoid drift)
    auto startTime = clock::now();
    auto expectedNext = startTime + targetFrameTime; // next frame boundary

    while (!g_isShuttingDown) {
        // If online mode is active, perform one-time cleanup then exit the thread
        if (g_onlineModeActive.load()) {
            StopBufferFreezing();
            ResetActionFlags();
            p1DelayState = {false, 0, TRIGGER_NONE, 0, -1, -1, 0, -1};
            p2DelayState = {false, 0, TRIGGER_NONE, 0, -1, -1, 0, -1};
            break; // exit thread to allow safe self-unload
        }
        auto frameStart = clock::now();
        // Catch-up logic: if we are *very* late (> 10 frames), jump ahead to avoid cascading backlog
        if (frameStart - expectedNext > targetFrameTime * 10) {
            expectedNext = frameStart + targetFrameTime;
        }
        
    // Refresh core pointer cache once per loop iteration
    RefreshPointerCache();
    // Check current game phase (single authoritative call per loop)
    GamePhase currentPhase = GetCurrentGamePhase();

    // Lightweight, integrated online detection (replaces separate network thread)
    {
        static int netCheckCounter = 0;              // frames since last check
        static int consecutiveOnline = 0;            // consecutive positive detections
        static bool stopNetChecks = false;           // stop after timeout / confirmation
        static auto gameStartTime = std::chrono::steady_clock::now();
        if (!stopNetChecks) {
            // 2.5s cadence at 192 Hz ~ 480 frames
            if (++netCheckCounter >= 480) {
                netCheckCounter = 0;
                OnlineState st = ReadEfzRevivalOnlineState();
                if (st != OnlineState::Unknown) {
                    if (st == OnlineState::Netplay || st == OnlineState::Spectating || st == OnlineState::Tournament) {
                        ++consecutiveOnline;
                        if (consecutiveOnline >= 2) {
                            // Immediately enter online mode; do not alter console visibility here
                            isOnlineMatch = true;
                            EnterOnlineMode();
                            // After EnterOnlineMode, loop will hit g_onlineModeActive guard and break
                            stopNetChecks = true;
                        }
                    } else {
                        consecutiveOnline = 0;
                    }
                }

                // Stop checking after 10s if nothing detected
                auto elapsedSecs = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now() - gameStartTime).count();
                if (elapsedSecs > 10 && !isOnlineMatch.load()) {
                    stopNetChecks = true;
                }
            }
        }
    }
        
    // CRITICAL FIX: Stop buffer freezing IMMEDIATELY if not in match
        if (currentPhase != GamePhase::Match) {
            // Check if buffer freezing is active and stop it
            if (g_bufferFreezingActive.load()) {
                LogOut("[FRAME MONITOR] Phase left Match, emergency stopping buffer freeze", true);
                StopBufferFreezing();
            }
            
            // Also clear any pending delays and restore control
            if (p1DelayState.isDelaying || p2DelayState.isDelaying) {
                p1DelayState.isDelaying = false;
                p2DelayState.isDelaying = false;
                p1DelayState.triggerType = TRIGGER_NONE;
                p2DelayState.triggerType = TRIGGER_NONE;
                LogOut("[FRAME MONITOR] Cleared delay states due to phase change", true);
            }
            
            // Restore P2 control if it was overridden
            if (g_p2ControlOverridden) {
                RestoreP2ControlState();
            }
        }

        // Toggle high-resolution timers only when needed
        bool needHighRes = (currentPhase == GamePhase::Match);
        if (needHighRes && !highResActive) {
            timeBeginPeriod(1);
            highResActive = true;
        } else if (!needHighRes && highResActive) {
            timeEndPeriod(1);
            highResActive = false;
        }
        
        // Track phase changes in one place and log once
        static GamePhase lastPhase = GamePhase::Unknown;
        if (currentPhase != lastPhase) {
            // Log a single concise message on change
            LogPhaseIfChanged();

            // On ANY phase change away from Match, ensure cleanup
            if (lastPhase == GamePhase::Match && currentPhase != GamePhase::Match) {
                // Force stop everything
                StopBufferFreezing();
                // Stop any RF freeze maintenance started by CR
                StopRFFreeze();
                ResetActionFlags();

                // Clear all auto-action states
                p1DelayState = {false, 0, TRIGGER_NONE, 0, -1, -1, 0, -1};
                p2DelayState = {false, 0, TRIGGER_NONE, 0, -1, -1, 0, -1};
                p1ActionApplied = false;
                p2ActionApplied = false;

                // Restore P2 control
                if (g_p2ControlOverridden) {
                    RestoreP2ControlState();
                    g_p2ControlOverridden = false;
                }
            }

            // Entering MATCH phase -> reinit transient state
            if (currentPhase == GamePhase::Match && lastPhase != GamePhase::Match) {
                prevMoveID1 = -1;
                prevMoveID2 = -1;

                // Ensure default control flags (P1=Player, P2=AI) at match start in Practice mode
                GameMode modeAtMatch = GetCurrentGameMode();
                if (modeAtMatch == GameMode::Practice) {
                    EnsureDefaultControlFlagsOnMatchStart();
                    // Reset Dummy Auto-Block per-round state machine
                    ResetDummyAutoBlockState();
                    // Clear swap tracking flag at match start (new round = fresh state)
                    SwitchPlayers::ClearSwapFlag();
                    MaybeShowPracticeOverlayHintOnce();
                }
            }

            // If we just arrived at Character Select: (suppress legacy diagnostics by default) + safe reset
            if (currentPhase == GamePhase::CharacterSelect && lastPhase != GamePhase::CharacterSelect) {
#if ENABLE_CS_DEBUG_LOGS
                LogCharacterSelectDiagnostics();
#endif
                // Only perform CS control resets in Practice mode
                if (GetCurrentGameMode() == GameMode::Practice) {
                    // DON'T clear swap flag here - ResetControlMappingForMenusToP1 needs it to know if restoration is needed
                    // Flag will be cleared AFTER restoration happens (inside ResetControlMappingForMenusToP1)
                    ResetControlOnCharacterSelect();
                }
                // Reset live logger state on entry so first sample prints (only used when debug logs enabled)
                s_csLastActive = 0xFF; s_csLastP2Cpu = 0xFF; s_csLastP1Cpu = 0xFF;
                s_csLastP1Ai = 0xFFFFFFFFu; s_csLastP2Ai = 0xFFFFFFFFu; s_csLogDecim = 0;
                // Reset input-edge logger state
                s_csBtnInit = false; for (int i=0;i<4;++i) s_csPrevBtns[i] = 0;
                // Reset per-entry snapshot apply guard
                s_csDidSnapshotApply = false;
                // NEW: mark that we have not yet performed our one-shot persistent trigger clear this CS entry
                s_csDidPersistentTriggerClear = false;
            }

            lastPhase = currentPhase;
        }

        // Character Select handling: run per-frame, not only on phase-change edge
        if (currentPhase == GamePhase::CharacterSelect) {
#if ENABLE_CS_DEBUG_LOGS
            CharacterSelectLiveLoggerTick();
            CharacterSelectInputEdgeLoggerTick();
#endif
            const bool inPractice = (GetCurrentGameMode() == GameMode::Practice);
            if (!inPractice) {
                // Outside Practice mode: do not touch or log Character Select behavior
                s_characterSelectPhaseFrames = 0;
            } else {
                s_characterSelectPhaseFrames++;
                if (s_characterSelectPhaseFrames == 1 && (detailedLogging.load() || !g_reducedLogging.load())) {
                    LogOut("[FRAME MONITOR] Detected CharacterSelect phase - starting debounce window", true);
                    s_csDidPreUnswitch = false; // reset guard each CS entry
                }
                // After short debounce, ensure menu mapping is reset so P1 controls P1 on CS (Practice only)
                if (!s_csDidPreUnswitch && s_characterSelectPhaseFrames >= 30) {
                    if (SwitchPlayers::ResetControlMappingForMenusToP1()) {
                        s_csDidPreUnswitch = true;
                        if (detailedLogging.load()) LogOut("[CS][PRE][UNSWITCH] Menu mapping reset to P1 local", true);
                    }
                }
            }
            // Snapshot/restore: after a short debounce, capture CS baseline ONCE (first stable CS in session) and restore every CS entry.
            // Rationale: We do NOT want in-match swaps to redefine the CS baseline. CS should always return to the original mapping.
            if (s_characterSelectPhaseFrames >= 30 && GetCurrentGameMode() == GameMode::Practice) {
                uintptr_t base = GetEFZBase(); uintptr_t gs = 0;
                if (base && SafeReadMemory(base + EFZ_BASE_OFFSET_GAME_STATE, &gs, sizeof(gs)) && gs) {
                    uint8_t active=0xFF, p1cpu=0xFF, p2cpu=0xFF;
                    SafeReadMemory(gs + GAMESTATE_OFF_ACTIVE_PLAYER, &active, sizeof(active));
                    SafeReadMemory(gs + GAMESTATE_OFF_P1_CPU_FLAG, &p1cpu, sizeof(p1cpu));
                    SafeReadMemory(gs + GAMESTATE_OFF_P2_CPU_FLAG, &p2cpu, sizeof(p2cpu));
                    bool flagsAreSane = (p1cpu != p2cpu) && (active == 0u || active == 1u);
                    // Cross-check with Practice controller GUI_POS; note: GUI_POS 1 = P1, 0 = P2, while active 0 = P1, 1 = P2.
                    // So consistency means: (active == 0 && guiPos == 1) OR (active == 1 && guiPos == 0).
                    bool guiConsistent = true; // default to true if we cannot read it
                    uint8_t guiPos = 0xFF;
                    PauseIntegration::EnsurePracticePointerCapture();
                    if (void* prac = PauseIntegration::GetPracticeControllerPtr()) {
                        SafeReadMemory((uintptr_t)prac + PRACTICE_OFF_GUI_POS, &guiPos, sizeof(guiPos));
                        if (guiPos == 0u || guiPos == 1u) {
                            guiConsistent = ((active == 0u && guiPos == 1u) || (active == 1u && guiPos == 0u));
                        }
                    }
                    // Additional gate: only capture baseline when we observe the expected default Practice mapping (P1 human, P2 CPU)
                    bool defaultPracticeMap = (p1cpu == 0u && p2cpu == 1u);
                    if (!s_csBaseline.valid && flagsAreSane && guiConsistent && defaultPracticeMap) {
                        // Capture only once per session; later CS entries will restore to this snapshot
                        s_csBaseline.valid = true;
                        s_csBaseline.active = active;
                        s_csBaseline.p1Cpu = p1cpu;
                        s_csBaseline.p2Cpu = p2cpu;
                        if (detailedLogging.load()) {
                            std::ostringstream os; os << "[CS][SNAP] Captured baseline: active=" << (int)active
                                                      << " P1CPU=" << (int)p1cpu << " P2CPU=" << (int)p2cpu;
                            LogOut(os.str(), true);
                        }
                    } else if (!s_csBaseline.valid && flagsAreSane && guiConsistent && !defaultPracticeMap) {
                        // We saw a sane but swapped/non-default mapping during first stable CS; log and defer capture.
                        if (detailedLogging.load()) {
                            if (!s_logOnce_CsSnapSkip.exchange(true, std::memory_order_relaxed)) {
                                std::ostringstream os; os << "[CS][SNAP][SKIP] Sane but non-default mapping observed (active=" << (int)active
                                                          << " P1CPU=" << (int)p1cpu << " P2CPU=" << (int)p2cpu
                                                          << ") - deferring baseline capture until default (P1 human/P2 CPU) appears.";
                                LogOut(os.str(), true);
                            }
                        }
                    }
                    // Apply baseline (if valid) to prevent swapped or duplicated controls on CS.
                    if (s_csBaseline.valid) {
                        if (active != s_csBaseline.active || p1cpu != s_csBaseline.p1Cpu || p2cpu != s_csBaseline.p2Cpu) {
                            uint8_t wantActive = s_csBaseline.active;
                            uint8_t wantP1 = s_csBaseline.p1Cpu;
                            uint8_t wantP2 = s_csBaseline.p2Cpu;
                            // During CS: do not force engine's active owner; only align GUI_POS.
                            bool okA = true; // we are not writing active here intentionally
                            // If Practice controller exists and GUI_POS disagrees with desired active, align it
                            bool okGui = true;
                            if (void* prac = PauseIntegration::GetPracticeControllerPtr()) {
                                uint8_t curGui = 0xFF;
                                SafeReadMemory((uintptr_t)prac + PRACTICE_OFF_GUI_POS, &curGui, sizeof(curGui));
                                // Align GUI to the engine-reported active player (not baseline) to keep UI consistent
                                if ((curGui == 0u || curGui == 1u) && curGui != active) {
                                    uint8_t newGui = active; okGui = SafeWriteMemory((uintptr_t)prac + PRACTICE_OFF_GUI_POS, &newGui, sizeof(newGui));
                                }
                            }
                            // Mark for post-CS CPU flag application if needed
                            if (p1cpu != wantP1 || p2cpu != wantP2) {
                                s_pendingPostCsCpuApply = true;
                            }
                            if (detailedLogging.load()) {
                                if (!s_logOnce_CsRestoreAlign.exchange(true, std::memory_order_relaxed)) {
                                    std::ostringstream os; os << "[CS][RESTORE] Aligned GUI to engine-active; deferred CPU: engineActive=" << (int)active
                                                              << " P1CPU->" << (int)wantP1 << " P2CPU->" << (int)wantP2
                                                              << " okA=" << (okA?"1":"0")
                                                              << " okGUI=" << (okGui?"1":"0")
                                                              << " [CS][DEFER_CPU]";
                                    LogOut(os.str(), true);
                                }
                            }
                        } else if (!s_csDidSnapshotApply && detailedLogging.load()) {
                            // Log a one-time notice per application lifetime that baseline is already in effect
                            if (!s_logOnce_CsRestoreBaselineMatch.exchange(true, std::memory_order_relaxed)) {
                                std::ostringstream os; os << "[CS][RESTORE] Baseline already matches; no enforcement needed";
                                LogOut(os.str(), true);
                            }
                        }
                        // Mark that we have applied/verified once this CS entry
                        s_csDidSnapshotApply = true;
                    } else if (!s_csBaseline.valid && s_characterSelectPhaseFrames == 120) {
                        // Fallback: after extended debounce (120 frames) baseline still not captured; synthesize default mapping.
                        // Use GUI_POS (if available) to infer active. GUI_POS 1=P1 -> active=0; GUI_POS 0=P2 -> active=1. Default CPU flags: P1 human (0), P2 CPU (1).
                        uint8_t synthActive = 0u; // default assume P1
                        if (guiPos == 0u) synthActive = 1u; // if GUI_POS shows P2 side
                        s_csBaseline.valid = true;
                        s_csBaseline.active = synthActive;
                        s_csBaseline.p1Cpu = 0u;
                        s_csBaseline.p2Cpu = 1u;
                        if (detailedLogging.load()) {
                            if (!s_logOnce_CsSnapFallback.exchange(true, std::memory_order_relaxed)) {
                                std::ostringstream os; os << "[CS][SNAP][FALLBACK] Synthesized baseline after timeout: active=" << (int)synthActive
                                                          << " P1CPU=0 P2CPU=1";
                                LogOut(os.str(), true);
                            }
                        }
                    }
                }
            }
            if (s_characterSelectPhaseFrames >= 120 && GetCurrentGameMode() == GameMode::Practice) {
                bool charsInit = AreCharactersInitialized();
                if (!charsInit && !s_csDidPersistentTriggerClear) {
                    if (!g_reducedLogging.load() || detailedLogging.load()) {
                        LogOut("[FRAME MONITOR] CharacterSelect phase stable (>=120 frames) and characters not initialized -> clearing triggers", true);
                    }
                    // Suppress nested auto-action clear logging for this one-shot CS clear
                    g_suppressAutoActionClearLogging.store(true);
                    ClearAllTriggersPersistently();
                    g_suppressAutoActionClearLogging.store(false);
                    s_csDidPersistentTriggerClear = true; // one-shot per CS entry
                } else if (charsInit) {
#if ENABLE_CS_DEBUG_LOGS
                    if (detailedLogging.load() && !g_reducedLogging.load()) {
                        LogOut("[FRAME MONITOR] CharacterSelect phase stable but characters still initialized; skipping trigger clear (possible false phase)", true);
                    }
#endif
                }
            }
        } else if (s_characterSelectPhaseFrames > 0) {
            // Leaving Character Select: apply any deferred CPU flag baseline once
            s_characterSelectPhaseFrames = 0;
            if (GetCurrentGameMode() == GameMode::Practice && s_pendingPostCsCpuApply && s_csBaseline.valid) {
                uintptr_t gs = 0; uintptr_t base = GetEFZBase();
                if (base && SafeReadMemory(base + EFZ_BASE_OFFSET_GAME_STATE, &gs, sizeof(gs)) && gs) {
                    uint8_t wantP1 = s_csBaseline.p1Cpu;
                    uint8_t wantP2 = s_csBaseline.p2Cpu;
                    bool ok1 = SafeWriteMemory(gs + GAMESTATE_OFF_P1_CPU_FLAG, &wantP1, sizeof(wantP1));
                    bool ok2 = SafeWriteMemory(gs + GAMESTATE_OFF_P2_CPU_FLAG, &wantP2, sizeof(wantP2));
                    if (detailedLogging.load()) {
                        if (!s_logOnce_CsApplyPost.exchange(true, std::memory_order_relaxed)) {
                            std::ostringstream os; os << "[CS][APPLY_POST] Applied deferred CPU flags: P1CPU=" << (int)wantP1
                                                      << " P2CPU=" << (int)wantP2
                                                      << " ok1=" << (ok1?"1":"0")
                                                      << " ok2=" << (ok2?"1":"0");
                            LogOut(os.str(), true);
                        }
                    }
                }
                s_pendingPostCsCpuApply = false;
            }
        }
        
    // SINGLE authoritative frame increment
    int currentFrame = frameCounter.fetch_add(1) + 1;

        // Process any active input queues
        ProcessInputQueues();

        UpdateWindowActiveState();
        // Throttle stats overlay further to ~15-16 Hz to reduce churn and CPU
        {
            static int statsDecim = -1; // prime to fire quickly after enable
            if (statsDecim < 0) statsDecim = 11; // first iteration hits 0 modulo 12
            if ((statsDecim++ % 12) == 0) {
                UpdateStatsDisplay();
            }
        }

        bool shouldBeActive = ShouldFeaturesBeActive();
        if (shouldBeActive && !g_featuresEnabled.load()) {
            EnableFeatures();
        } else if (!shouldBeActive && g_featuresEnabled.load()) {
            DisableFeatures();
        }

        ManageKeyMonitoring();

        // Track initialization & mode
        bool isInitialized = AreCharactersInitialized();
        GameMode currentMode = GetCurrentGameMode();
        bool isValidGameMode = !Config::GetSettings().restrictToPracticeMode || (currentMode == GameMode::Practice);

        // Lightweight global Practice framestep tracker (no on-screen overlay)
        // - Tracks step count only while FA timing is actively waiting during pause in Practice.
        // - Exposes state via GetFrameStepDebugInfo() for ImGui to render in the debug menu.
        {
            static bool s_prevPaused = false;
            static bool s_haveLast = false;
            static uint32_t s_lastStep = 0;
            static int s_stepsSincePause = 0;
            static bool s_prevFAInProgress = false; // FA waiting window last frame

            const bool inPractice = (currentMode == GameMode::Practice);
            bool paused = PauseIntegration::IsPracticePaused();

            // Reset counters across mode changes away from Practice
            if (!inPractice) {
                s_prevPaused = false; s_haveLast = false; s_stepsSincePause = 0; s_prevFAInProgress = false;
            } else {
                if (paused) {
                    // Determine whether FA tracking is actively waiting for timings
                    bool faInProgress = false;
                    {
                        FrameAdvantageState fas = GetFrameAdvantageState();
                        bool faActive = fas.p1Attacking || fas.p2Attacking || fas.p1Defending || fas.p2Defending;
                        bool waitingAtk = (fas.p1Attacking && fas.p1ActionableInternalFrame == -1) ||
                                          (fas.p2Attacking && fas.p2ActionableInternalFrame == -1);
                        bool waitingDef = (fas.p1Defending && fas.p1DefenderFreeInternalFrame == -1) ||
                                          (fas.p2Defending && fas.p2DefenderFreeInternalFrame == -1);
                        faInProgress = faActive && (waitingAtk || waitingDef);
                    }

                    if (!s_prevPaused) {
                        s_haveLast = false; // seed will be re-captured on first FA step read
                    }

                    // On FA start (rising edge), reset the step counter and seed last counter
                    if (faInProgress && !s_prevFAInProgress) {
                        s_stepsSincePause = 0;
                        s_haveLast = false;
                    }

                    uint32_t cur = 0;
                    if (faInProgress) {
                        if (PauseIntegration::ReadStepCounter(cur)) {
                            if (!s_haveLast) {
                                s_lastStep = cur; s_haveLast = true;
                            } else if (cur != s_lastStep) {
                                uint32_t delta = cur - s_lastStep;
                                if (delta > 1000u) delta = 1u; // sanity clamp
                                s_stepsSincePause += (int)delta;
                                s_lastStep = cur;
                            }
                        }
                    } else {
                        // FA not in progress while paused: drop seed; next FA will reseed
                        s_haveLast = false;
                    }
                } else {
                    // Not paused: reset per-session trackers
                    s_haveLast = false;
                    s_stepsSincePause = 0;
                }
                s_prevPaused = paused;
                // Track FA in-progress state for edge detection
                {
                    FrameAdvantageState fas = GetFrameAdvantageState();
                    bool faActive = fas.p1Attacking || fas.p2Attacking || fas.p1Defending || fas.p2Defending;
                    bool waitingAtk = (fas.p1Attacking && fas.p1ActionableInternalFrame == -1) ||
                                      (fas.p2Attacking && fas.p2ActionableInternalFrame == -1);
                    bool waitingDef = (fas.p1Defending && fas.p1DefenderFreeInternalFrame == -1) ||
                                      (fas.p2Defending && fas.p2DefenderFreeInternalFrame == -1);
                    s_prevFAInProgress = faActive && (waitingAtk || waitingDef);
                }
            }

            // Publish debug snapshot for ImGui (global atomics)
            bool faActiveNow = s_prevFAInProgress && inPractice && paused;
            g_fsdbgActive.store(faActiveNow, std::memory_order_relaxed);
            g_fsdbgSteps.store(faActiveNow ? s_stepsSincePause : -1, std::memory_order_relaxed);
        }

        // Track game mode transitions
        if (g_featuresEnabled.load() && currentMode != s_previousGameMode) {
            // IMPORTANT: Add this section to handle transitions FROM valid modes
            if (!isValidGameMode && IsValidGameMode(s_previousGameMode)) {
                LogOut("[FRAME MONITOR] Detected exit from valid game mode, cleaning up resources", true);
                
                // Clean up BGM resources
                uintptr_t base = GetEFZBase();
                if (base) {
                    uintptr_t gameStatePtr = 0;
                    if (SafeReadMemory(base + EFZ_BASE_OFFSET_GAME_STATE, &gameStatePtr, sizeof(uintptr_t)) && gameStatePtr) {
                        SetBGMSuppressed(false); // Ensure BGM isn't suppressed
                    }
                }
                
                // Stop any buffer freezing
                StopBufferFreezing();
                
                // Reset action flags and restore P2 control state
                ResetActionFlags();
                
                // Clear delay states
                p1DelayState.isDelaying = false;
                p1DelayState.triggerType = TRIGGER_NONE;
                p2DelayState.isDelaying = false;
                p2DelayState.triggerType = TRIGGER_NONE;

                // Hard clear of all auto-action trigger internals (cooldowns, last active, etc.)
                ClearAllAutoActionTriggers();

                // No overlay reinit here to avoid re-adding messages while features are disabled.
                s_pendingOverlayReinit = false; // cancel any pending reinit once we leave valid mode
            }
            
            // Existing code for transitions TO valid modes
            if (isValidGameMode && isInitialized && 
                (s_previousGameMode != GameMode::Unknown && !IsValidGameMode(s_previousGameMode))) {
                LogOut("[FRAME MONITOR] Detected return to valid game mode with initialized characters, reinitializing overlays", true);
                ReinitializeOverlays();
                s_pendingOverlayReinit = false;
            } else if (isValidGameMode && !isInitialized &&
                       (s_previousGameMode != GameMode::Unknown && !IsValidGameMode(s_previousGameMode))) {
                // We returned to a valid mode but characters aren't initialized yet; defer reinit
                LogOut("[FRAME MONITOR] Returned to valid game mode; waiting for character initialization to reinit overlays", true);
                s_pendingOverlayReinit = true;
            }
            
            s_previousGameMode = currentMode;
        }

        // Update initialization tracking
        //wasInitialized = isInitialized;
        
        // Only run the main monitoring logic if features are enabled
        if (g_featuresEnabled.load()) {
            // Throttle trigger overlay to ~12-13 Hz (every 16 internal frames ~83ms)
            static int trigDecim = -1; // prime to show overlay quickly after enable
            if (trigDecim < 0) trigDecim = 15;
            if ((trigDecim++ % 16) == 0) {
                UpdateTriggerOverlay();
            }
                        uintptr_t base = s_ptrCache.base;
            if (!base) {
                goto FRAME_MONITOR_FRAME_END;
            }

            // Pointer change logging
                        uintptr_t p1Ptr = s_ptrCache.p1, p2Ptr = s_ptrCache.p2;
            if ((p1Ptr != fm_lastP1Ptr || p2Ptr != fm_lastP2Ptr) && (p1Ptr || p2Ptr)) {
          LogOut("[FRAME_MONITOR][PTR] P1 " + FM_Hex(p1Ptr) + " (was " + FM_Hex(fm_lastP1Ptr) + ")  P2 " +
              FM_Hex(p2Ptr) + " (was " + FM_Hex(fm_lastP2Ptr) + ")", detailedLogging.load());
                fm_lastP1Ptr = p1Ptr;
                fm_lastP2Ptr = p2Ptr;
            }

            // Initialization transition logging
            if (isInitialized != fm_lastCharsInit) {
                bool wasInitialized = fm_lastCharsInit;
          LogOut(std::string("[FRAME MONITOR][INIT] CharactersInitialized: ") +
              (fm_lastCharsInit ? "true" : "false") + " -> " +
              (isInitialized ? "true" : "false") +
              " frame=" + std::to_string(currentFrame), detailedLogging.load());
                fm_lastCharsInit = isInitialized;

                // If we were waiting for init in a valid mode, reinitialize overlays now
                if (!wasInitialized && isInitialized && s_pendingOverlayReinit && isValidGameMode && g_featuresEnabled.load()) {
                    LogOut("[FRAME MONITOR] Characters initialized in valid mode; reinitializing overlays now", true);
                    ReinitializeOverlays();
                    s_pendingOverlayReinit = false;
                }
            }

            // Phase gating: use currentPhase from above

            // Lightweight ticking (cooldowns) always runs
            auto lightweightTick = []() {
                ProcessTriggerCooldowns(); // make sure cooldowns advance outside Match
            };

            bool skipHeavy = false;

            // Outside actual gameplay -> only do lightweight logic
            if (currentPhase != GamePhase::Match) {
                lightweightTick();
                prevMoveID1 = 0;
                prevMoveID2 = 0;
                skipHeavy = true;
            }
            // =========================================================================

            if (skipHeavy) {
                // Still allow precise frame pacing / rest of loop tail
                goto FRAME_MONITOR_FRAME_END;
            }

            // (EXISTING HEAVY LOGIC BELOW: address refresh, moveID reads, processing)
            // First: tick practice macro controller before processing inputs/motions
            MacroController::Tick();
            // Refresh addresses periodically, and also on first use if not yet cached
            if (addressCacheCounter++ >= 192 || !cachedMoveIDAddr1 || !cachedMoveIDAddr2) {
                cachedMoveIDAddr1 = ResolvePointer(base, EFZ_BASE_OFFSET_P1, MOVE_ID_OFFSET);
                cachedMoveIDAddr2 = ResolvePointer(base, EFZ_BASE_OFFSET_P2, MOVE_ID_OFFSET);
                addressCacheCounter = 0;
                
                // Log timing performance every second (config-gated)
                auto currentTime = clock::now();
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(currentTime - lastLogTime);
                if (elapsed.count() >= 1000) {
                    double actualFPS = framesSinceLastLog / (elapsed.count() / 1000.0);
                    // Only log if enabled in config and either detailed logging is on or fps deviates
                    if (Config::GetSettings().enableFpsDiagnostics && (detailedLogging.load() || fabs(actualFPS - 192.0) > 5.0)) {
                        LogOut("[FRAME MONITOR] Actual FPS: " + std::to_string(actualFPS) + 
                               " (target: 192.0)", detailedLogging.load());
                    }
                    lastLogTime = currentTime;
                    framesSinceLastLog = 0;
                }
            }
            
            // Read move IDs using cached addresses with error checking
            short moveID1 = 0, moveID2 = 0;
            if (cachedMoveIDAddr1 && !SafeReadMemory(cachedMoveIDAddr1, &moveID1, sizeof(short))) {
                // Re-cache on read failure
                cachedMoveIDAddr1 = ResolvePointer(base, EFZ_BASE_OFFSET_P1, MOVE_ID_OFFSET);
            }
            if (cachedMoveIDAddr2 && !SafeReadMemory(cachedMoveIDAddr2, &moveID2, sizeof(short))) {
                cachedMoveIDAddr2 = ResolvePointer(base, EFZ_BASE_OFFSET_P2, MOVE_ID_OFFSET);
            }

            // Populate unified per-frame sample (read-only use for now)
            // Neutral whitelist duplicated previously across Continuous Recovery & DummyAutoBlock.
            auto isNeutralMoveId = [](short m) {
                return (m == 0 || m == 1 || m == 2 || m == 3 || m == 4 ||
                        m == 7 || m == 8 || m == 9 || m == 13);
            };
            g_lastSample.frame = (uint32_t)frameCounter.load();
            g_lastSample.tickMs = GetTickCount64();
            g_lastSample.phase = currentPhase;
            g_lastSample.mode = GetCurrentGameMode();
            g_lastSample.charsInitialized = isInitialized;
            g_lastSample.moveID1 = moveID1;
            g_lastSample.moveID2 = moveID2;
            g_lastSample.prevMoveID1 = prevMoveID1;
            g_lastSample.prevMoveID2 = prevMoveID2;
            g_lastSample.actionable1 = IsActionable(moveID1);
            g_lastSample.actionable2 = IsActionable(moveID2);
            g_lastSample.neutral1 = isNeutralMoveId(moveID1);
            g_lastSample.neutral2 = isNeutralMoveId(moveID2);
            g_lastSample.basePtr = base;
            g_lastSample.gameStatePtr = s_ptrCache.gs;
            g_lastSample.p1Ptr = s_ptrCache.p1;
            g_lastSample.p2Ptr = s_ptrCache.p2;
            g_lastSample.online = g_onlineModeActive.load();
            // Expose function symbol (lambda can't have external linkage) via inline in anonymous namespace
            // We'll define GetCurrentPerFrameSample after the loop.

            // --- Recoil Guard (RG) analysis: detect edges and compute freeze/advantage ---
            struct RGAnalysis {
                bool active = false;   // any RG event currently being tracked for this defender
                int defender = 0;      // 1 or 2
                short rgMove = 0;      // 168/169/170
                // Freeze/stun durations in visual frames (as per wiki)
                double defFreezeF = 0.0;
                double atkFreezeF = 0.0;
                double rgStunF   = 0.0; // universal 20F (except Sayuri nuance)
                double netAdvF   = 0.0; // attacker recovers earlier by this many visual frames
                // Frame advantage presentation
                double fa1F      = 0.0; // immediate FA from freeze timings (visual frames)
                double fa2ThF    = 0.0; // theoretical FA2 = FA1 + RG stun (visual frames)
                double fa2F      = 0.0; // measured FA until both actionable again (visual frames)
                bool   fa2Ready  = false;
                // Tracking for cRG window
                int attacker = 0;          // 1 or 2
                short attackerMoveAtEvent = -1;
                bool cRGOpen = false;
                int openedAtFrame = 0;     // internal frameCounter when opened
                // Actionability timestamps (internal frames @192Hz)
                int atkActionableAt = -1;
                int defActionableAt = -1;
                bool fa2Announced = false;
            };
            static RGAnalysis s_rgP1; // last RG where P1 was the defender
            static RGAnalysis s_rgP2; // last RG where P2 was the defender

            // Transient Counter RG assist:
            // When the human (P1) RGs the dummy, briefly enable dummy autoblock and arm RG for P2
            // so it can counter-RG without requiring Always RG to be enabled globally.
            static bool s_crgAssistActive = false;
            static bool s_crgSavedAutoBlock = false;
            static bool s_crgSavedAutoBlockValid = false;

         auto computeRGInfo = [](short rgMove, double &defF, double &atkF, double &stunF, double &advF, int defenderCharId) {
                // Values sourced from wiki: EFZ Strategy/Game Mechanics pages
                if (rgMove == RG_STAND_ID) {
                    defF = RG_STAND_FREEZE_DEFENDER;
                    atkF = RG_STAND_FREEZE_ATTACKER;
                } else if (rgMove == RG_CROUCH_ID) {
                    defF = RG_CROUCH_FREEZE_DEFENDER;
                    atkF = RG_CROUCH_FREEZE_ATTACKER;
                } else { // RG_AIR_ID
                    defF = RG_AIR_FREEZE_DEFENDER;
                    atkF = RG_AIR_FREEZE_ATTACKER;
                }
                // Base universal RG stun value
                stunF = RG_STUN_DURATION;
                // Character specific quirk: Sayuri defender experiences ~21.66F (65 internal frames @192Hz)
                // Convert 65 internal frames -> visual frames: 65 * 60/192 = 65 * 0.3125 = 20.3125? (But provided value 21.66)
                // Using provided authoritative value 21.66F (approx 65 internal * 60/180?); keep as explicit constant.
                if (defenderCharId == CHAR_ID_SAYURI) {
                    stunF = 21.66; // documented anomaly
                }
                advF = defF - atkF; // positive means attacker earlier by advF
            };

            auto emitRGMessage = [&](const RGAnalysis &rg) {
                const char* kind = (rg.rgMove == RG_STAND_ID) ? "Stand" : ((rg.rgMove == RG_CROUCH_ID) ? "Crouch" : "Air");
                std::ostringstream os; os.setf(std::ios::fixed); os << std::setprecision(2);
            os << "RG: P" << rg.defender << " " << kind
             << "  defFreeze=" << rg.defFreezeF << "F"
             << "  atkFreeze=" << rg.atkFreezeF << "F"
         << "  netAdv(att)=" << rg.netAdvF << "F"
             << "  RGstun=" << rg.rgStunF << "F"
         << "  FA1(endFreeze)=" << rg.fa1F << "F"
         << "  FA2(th)=" << rg.fa2ThF << "F"
             << "  cRG window: until attacker recovers/cancels";
                LogOut(std::string("[RG][FM] ") + os.str(), true);
                // One-shot overlay toast (gated by debug flag)
                if (g_ShowRGDebugToasts.load()) {
                    DirectDrawHook::AddMessage(os.str(), "RG", RGB(120, 200, 255), 1500, 0, 140);
                }
            };

            // RG analysis now uses unified per-frame sample (reduces repeated IsActionable calls)
            const PerFrameSample &rgSample = g_lastSample;
            auto closeCRGIfOver = [&](RGAnalysis &rg) {
                if (!rg.active || !rg.cRGOpen) return;
                // Determine current attacker move/actionable state
                short atkMoveNow = (rg.attacker == 1) ? rgSample.moveID1 : rgSample.moveID2;
                bool atkActionable = (rg.attacker == 1) ? rgSample.actionable1 : rgSample.actionable2;
                // Close when attacker becomes actionable or cancels out of their move
                if (atkActionable || atkMoveNow != rg.attackerMoveAtEvent) {
                    rg.cRGOpen = false;
                    std::ostringstream os; os.setf(std::ios::fixed); os << std::setprecision(2);
                    os << "RG: cRG window closed for P" << rg.defender << " (attacker now actionable/cancelled)";
                    LogOut(std::string("[RG][FM] ") + os.str(), detailedLogging.load());
                    // No overlay toast on close to reduce noise
                }
            };

            // Update actionability times and announce FA2 once both are known
            auto updateRGFA = [&](RGAnalysis &rg) {
                if (!rg.active) return;
                const double kIntToVis = 60.0 / 192.0; // convert internal 192 Hz frames to visual frames

                // Track attacker actionable timestamp
                if (rg.atkActionableAt < 0) {
                    short atkMoveNow = (rg.attacker == 1) ? rgSample.moveID1 : rgSample.moveID2;
                    bool atkActionable = (rg.attacker == 1) ? rgSample.actionable1 : rgSample.actionable2;
                    if (atkActionable) {
                        rg.atkActionableAt = frameCounter.load();
                        // Debug signal for verification
                        {
                            std::ostringstream os; os.setf(std::ios::fixed); os << std::setprecision(2);
                            os << "RG: Attacker actionable (P" << rg.attacker << ")";
                            LogOut(std::string("[RG][FM] ") + os.str(), true);
                            if (g_ShowRGDebugToasts.load()) {
                                DirectDrawHook::AddMessage(os.str(), "RG", RGB(160, 255, 160), 1200, 0, 156);
                            }
                        }
                    }
                }
                // Track defender actionable timestamp
                if (rg.defActionableAt < 0) {
                    short defMoveNow = (rg.defender == 1) ? rgSample.moveID1 : rgSample.moveID2;
                    bool defActionable = (rg.defender == 1) ? rgSample.actionable1 : rgSample.actionable2;
                    if (defActionable) {
                        rg.defActionableAt = frameCounter.load();
                        // Debug signal for verification
                        {
                            std::ostringstream os; os.setf(std::ios::fixed); os << std::setprecision(2);
                            os << "RG: Defender actionable (P" << rg.defender << ")";
                            LogOut(std::string("[RG][FM] ") + os.str(), true);
                            if (g_ShowRGDebugToasts.load()) {
                                DirectDrawHook::AddMessage(os.str(), "RG", RGB(255, 240, 160), 1200, 0, 156);
                            }
                        }
                    }
                }

                // When both are known, compute FA2 (only once)
                if (!rg.fa2Announced && rg.atkActionableAt >= 0 && rg.defActionableAt >= 0) {
                    int deltaInt = rg.defActionableAt - rg.atkActionableAt; // positive => attacker earlier
                    rg.fa2F = deltaInt * kIntToVis;
                    rg.fa2Ready = true;
                    rg.fa2Announced = true;

                    std::ostringstream os; os.setf(std::ios::fixed); os << std::setprecision(2);
                          os << "RG: P" << rg.defender
                              << "  FA1(endFreeze)=" << rg.fa1F << "F"
                              << "  FA2(meas)=" << rg.fa2F << "F";
                    LogOut(std::string("[RG][FM] ") + os.str(), true);
                    if (g_ShowRGDebugToasts.load()) {
                        DirectDrawHook::AddMessage(os.str(), "RG", RGB(120, 200, 255), 1500, 0, 156);
                    }

                    // Update standard overlay with FA1/FA2(meas) [endFreeze]
                    auto toIntFrames = [](double visF) {
                        return (visF >= 0.0) ? (int)(visF * 3.0 + 0.5) : (int)(visF * 3.0 - 0.5);
                    };
                    int fa1Int = toIntFrames(rg.fa1F);
                    int fa2Int = toIntFrames(rg.fa2F);
                    std::string fa1Text = FormatFrameAdvantage(fa1Int);
                    std::string fa2Text = FormatFrameAdvantage(fa2Int);
                    // Show numeric end-of-freeze duration (visual frames) with subframe precision [.00/.33/.66]
                    double endFreezeVis = (rg.defFreezeF >= rg.atkFreezeF) ? rg.defFreezeF : rg.atkFreezeF;
                    auto fmtUnsignedVis = [](double visF) {
                        int internal = (visF >= 0.0) ? (int)(visF * 3.0 + 0.5) : (int)(visF * 3.0 - 0.5);
                        int whole = internal / 3;
                        int sub = std::abs(internal % 3);
                        const char* frac = (sub == 1) ? ".33" : (sub == 2) ? ".66" : ".00";
                        return std::to_string(whole) + std::string(frac);
                    };
                    std::string endFreezeStr = fmtUnsignedVis(endFreezeVis);
                    // New format: "[FA1]/FA2" with separate colors per value
                    std::string leftText = "[" + fa1Text + "]";
                    std::string rightText = "/" + fa2Text; // leading slash stays with right segment
                    COLORREF leftColor = (fa1Int >= 0) ? RGB(0, 255, 0) : RGB(255, 0, 0);
                    COLORREF rightColor = (fa2Int >= 0) ? RGB(0, 255, 0) : RGB(255, 0, 0);
                    // Clear any previous regular FA display window to ensure RG takes precedence
                    frameAdvState.displayUntilInternalFrame = -1;
                    // Suppress regular FA overlay updates for the same duration as display
                    // Use pause-aware frame counter to match timer check in MonitorFrameAdvantage
                    int nowInt = GetCurrentInternalFrame();
                    g_SkipRegularFAOverlayUntilFrame.store(nowInt + GetDisplayDurationInternalFrames());
                    // Render as two messages placed side-by-side. Keep baseline Y and compute X for right.
                    // We’ll approximate widths by measuring when menu is hidden; otherwise keep coarse spacing.
                    const int baseX = 305;
                    const int baseY = 430;
                    if (g_showFrameAdvantageOverlay.load()) {
                        // Update left segment
                        if (g_FrameAdvantageId != -1) {
                            DirectDrawHook::UpdatePermanentMessage(g_FrameAdvantageId, leftText, leftColor);
                        } else {
                            g_FrameAdvantageId = DirectDrawHook::AddPermanentMessage(leftText, leftColor, baseX, baseY);
                        }
                        // Update right segment: place a few characters to the right; conservative offset of 60px
                        // Since our overlay draws a background box sized to text, small spacing avoids overlap.
                        int rightX = baseX + 60;
                        if (g_FrameAdvantage2Id != -1) {
                            DirectDrawHook::UpdatePermanentMessage(g_FrameAdvantage2Id, rightText, rightColor);
                        } else {
                            g_FrameAdvantage2Id = DirectDrawHook::AddPermanentMessage(rightText, rightColor, rightX, baseY);
                        }
                        // Set display timer for RG messages (configurable via .ini, default 8 seconds)
                        frameAdvState.displayUntilInternalFrame = nowInt + GetDisplayDurationInternalFrames();
                        #if defined(ENABLE_FRAME_ADV_DEBUG)
                        LogOut("[RG_DEBUG] Set RG timer: nowInt=" + std::to_string(nowInt) + 
                               " expiry=" + std::to_string(frameAdvState.displayUntilInternalFrame), true);
                        #endif
                    } else {
                        // If hidden, ensure any existing FA messages are cleared
                        if (g_FrameAdvantageId != -1) { DirectDrawHook::RemovePermanentMessage(g_FrameAdvantageId); g_FrameAdvantageId = -1; }
                        if (g_FrameAdvantage2Id != -1) { DirectDrawHook::RemovePermanentMessage(g_FrameAdvantage2Id); g_FrameAdvantage2Id = -1; }
                    }

                    // After FA2 is known, we can stop tracking this RG instance
                    // But don't clear the display messages immediately - let them show for the full duration
                    // Set a timer so regular FA can take over after the suppression window expires
                    rg.active = false;
                    // The messages will be cleared naturally when:
                    // 1. g_SkipRegularFAOverlayUntilFrame expires and regular FA takes over
                    // 2. User toggles the overlay off
                    // 3. A new RG occurs
                }
            };

            auto onRGEdge = [&](int defender, short rgMove) {
                RGAnalysis &slot = (defender == 1) ? s_rgP1 : s_rgP2;
                slot = RGAnalysis{}; // reset
                slot.active = true;
                slot.defender = defender;
                slot.rgMove = rgMove;
                // Determine defender character ID (live displayData may still have last snapshot IDs)
                int defCharId = (defender == 1) ? displayData.p1CharID : displayData.p2CharID;
                computeRGInfo(rgMove, slot.defFreezeF, slot.atkFreezeF, slot.rgStunF, slot.netAdvF, defCharId);
                slot.fa1F = slot.netAdvF; // FA1 = freeze-based net advantage
                slot.fa2ThF = slot.fa1F + slot.rgStunF; // FA2 theoretical = FA1 + RG stun
                // Determine attacker side and their move at event
                slot.attacker = (defender == 1) ? 2 : 1;
                slot.attackerMoveAtEvent = (slot.attacker == 1) ? moveID1 : moveID2;
                slot.cRGOpen = true;
                slot.openedAtFrame = frameCounter.load();
                emitRGMessage(slot);

                // If P1 (human) just RG'd and Counter RG is enabled, prepare P2 to counter-RG
                if (defender == 1) {
                    if (g_counterRGEnabled.load() && !AlwaysRG::IsEnabled() && GetCurrentGameMode() == GameMode::Practice) {
                        bool curAB = false;
                        if (GetPracticeAutoBlockEnabled(curAB)) {
                            s_crgSavedAutoBlock = curAB;
                            s_crgSavedAutoBlockValid = true;
                        } else {
                            s_crgSavedAutoBlockValid = false;
                        }
                        // Ensure autoblock is ON during the counter-RG window
                        if (!curAB) {
                            SetPracticeAutoBlockEnabled(true);
                        }
                        s_crgAssistActive = true;
                        if (detailedLogging.load()) {
                            LogOut("[CRG][ASSIST] Activated: enabling dummy autoblock and arming RG during window", true);
                        }
                    }
                }
            };

            // Detect RG state edges for both players
            if (IsRecoilGuard(moveID1) && !IsRecoilGuard(prevMoveID1)) {
                onRGEdge(1, moveID1);
            }
            if (IsRecoilGuard(moveID2) && !IsRecoilGuard(prevMoveID2)) {
                onRGEdge(2, moveID2);
            }

            // Maintain cRG windows succinctly
            closeCRGIfOver(s_rgP1);
            closeCRGIfOver(s_rgP2);

            // Update and announce FA2 when both sides become actionable again
            updateRGFA(s_rgP1);
            updateRGFA(s_rgP2);

            // Counter RG assist maintenance: arm P2 RG while the P1 RG window is open
                if (s_crgAssistActive) {
                bool windowOpen = (s_rgP1.active && s_rgP1.cRGOpen);
                bool p2RgEdge = (IsRecoilGuard(moveID2) && !IsRecoilGuard(prevMoveID2));
                bool stopAssist = !windowOpen || p2RgEdge || (GetCurrentGamePhase() != GamePhase::Match) || (GetCurrentGameMode() != GameMode::Practice);
                if (stopAssist) {
                    // Restore previous autoblock setting if we changed it
                    if (s_crgSavedAutoBlockValid && !s_crgSavedAutoBlock) {
                        SetPracticeAutoBlockEnabled(false);
                    }
                    s_crgAssistActive = false;
                    s_crgSavedAutoBlockValid = false;
                    if (detailedLogging.load()) {
                        LogOut("[CRG][ASSIST] Deactivated: restoring autoblock state", true);
                    }
                } else {
                    // Arm RG for P2 by writing 0x3C to [P2 + 334]
                    uintptr_t p2Ptr = ResolvePlayerBaseBestEffort(2);
                    if (p2Ptr) {
                        uint8_t arm = 0x3C;
                        SafeWriteMemory(p2Ptr + 334, &arm, sizeof(arm));
                    }
                }
            }

    // DEBUG: sampler for block-related values; trigger only on attacks (move/state >= 200 and not idle)
    // Disabled (kept for potential future diagnostics)
            {
        constexpr bool kEnableBlockDbg = false;
                if (kEnableBlockDbg && detailedLogging.load() && currentPhase == GamePhase::Match) {
                    bool p1Trig = (moveID1 >= 200 && moveID1 != IDLE_MOVE_ID);
                    bool p2Trig = (moveID2 >= 200 && moveID2 != IDLE_MOVE_ID);
            // Only sample when at least one player is in an attack/state >= 200
            bool shouldSample = p1Trig || p2Trig;
                    if (shouldSample) {
                        uintptr_t p1PtrDbg = ResolvePlayerBaseBestEffort(1, base);
                        uintptr_t p2PtrDbg = ResolvePlayerBaseBestEffort(2, base);
                        // Determine attacker for this log line
                        int atkId = p1Trig ? 1 : (p2Trig ? 2 : ((moveID1 >= 200) ? 1 : ((moveID2 >= 200) ? 2 : 0)));
                        bool atkHasReq = false;
                        auto samplePlayer = [&](int pid, uintptr_t pBase, bool showAddr, bool* hasReqOut) {
                            if (!pBase) return std::string("P") + std::to_string(pid) + ": <null>";
                            // Read direction/stance (raw inputs)
                            int8_t dir = 0; uint8_t stance = 0;
                            SafeReadMemory(pBase + BLOCK_DIRECTION_OFFSET, &dir, sizeof(dir));
                            SafeReadMemory(pBase + BLOCK_STANCE_OFFSET, &stance, sizeof(stance));
                            // Compute frameBlock pointer
                            uint16_t state = 0, frame = 0; uintptr_t animTab = 0, framesPtr = 0, frameBlock = 0;
                            SafeReadMemory(pBase + MOVE_ID_OFFSET, &state, sizeof(state)); // move/state at +0x8
                            SafeReadMemory(pBase + CURRENT_FRAME_INDEX_OFFSET, &frame, sizeof(frame));
                            SafeReadMemory(pBase + ANIM_TABLE_OFFSET, &animTab, sizeof(animTab));
                            if (animTab) {
                                uintptr_t entryAddr = animTab + (static_cast<uintptr_t>(state) * ANIM_ENTRY_STRIDE) + ANIM_ENTRY_FRAMES_PTR_OFFSET;
                                SafeReadMemory(entryAddr, &framesPtr, sizeof(framesPtr));
                                if (framesPtr) {
                                    frameBlock = framesPtr + (static_cast<uintptr_t>(frame) * FRAME_BLOCK_STRIDE);
                                }
                            }
                            uint16_t atk=0, hit=0, grd=0;
                            if (frameBlock) {
                                SafeReadMemory(frameBlock + FRAME_ATTACK_PROPS_OFFSET, &atk, sizeof(atk));
                                SafeReadMemory(frameBlock + FRAME_HIT_PROPS_OFFSET, &hit, sizeof(hit));
                                SafeReadMemory(frameBlock + FRAME_GUARD_PROPS_OFFSET, &grd, sizeof(grd));
                            }
                            bool isAttacker = (pid == atkId);
                            // Decode only for attacker to avoid confusion
                            const char* level = "N/A";
                            bool blockable = false;
                            // Hidden probes kept for future validation; set to true locally when needed
                            constexpr bool kShowProbeBits = false;
                            bool blockable10 = false, blockable01 = false, blockable2000 = false;
                            if (isAttacker) {
                                bool isHigh = (atk & 0x1) != 0;
                                bool isLow  = (atk & 0x2) != 0;
                // Treat guardable frames with no HIGH/LOW bits as ANY
                if (isHigh && isLow) level = "ANY";
                else if (isHigh) level = "HIGH";
                else if (isLow) level = "LOW";
                else if (grd != 0) level = "ANY";
                else level = "NONE";
                if (hasReqOut) { *hasReqOut = (isHigh || isLow || (grd != 0)); }
                                // Prefer GuardProps presence as practical blockable indicator (nonzero => guardable window)
                                blockable = (grd != 0);
                                // Keep probes available for quick toggling if needed
                                blockable10   = (hit & 0x10)   != 0;   // bit4 probe
                                blockable01   = (hit & 0x01)   != 0;   // bit0 probe (expected)
                                blockable2000 = (hit & 0x2000) != 0;   // bit13 probe
                            }
                            std::ostringstream os; os << (isAttacker ? "ATK " : "DEF ") << "P" << pid
                                << " dir=" << (int)dir
                                << " stance=" << (int)stance
                                << " move=" << state
                                << " frame=" << frame
                                << " atk=0x" << std::hex << std::uppercase << atk
                                << " hit=0x" << hit
                                << " grd=0x" << grd
                                << std::dec;
                            if (isAttacker) {
                                          os << " req=" << level
                                              << " blk=" << (blockable ? "BLOCKABLE" : "UNBLOCKABLE") << "[GRD]";
                                if (kShowProbeBits) {
                                    os << " (p10=" << (blockable10 ? "Y" : "N")
                                       << ", p01=" << (blockable01 ? "Y" : "N")
                                       << ", p2000=" << (blockable2000 ? "Y" : "N")
                                       << ")";
                                }
                            }
                            if (showAddr && frameBlock) {
                                os << " fb=" << FM_Hex(frameBlock);
                            }
                            return os.str();
                        };
                        bool showAddr = (p1Trig || p2Trig);
                        std::string l1 = samplePlayer(1, p1PtrDbg, showAddr, &atkHasReq);
                        std::string l2 = samplePlayer(2, p2PtrDbg, showAddr, &atkHasReq);
                        std::string prefix = "[BLOCKDBG]";
            if ((p1Trig || p2Trig) && !atkHasReq) {
                            // Attacker has no guard requirement yet; skip logs for this move frame
                            (void)0;
                        } else {
                            if (p1Trig || p2Trig) {
                            prefix += " [TRIG";
                                if (p1Trig) prefix += " P1";
                                if (p2Trig) prefix += " P2";
                            prefix += "]";
                            }
                            LogOut(prefix + std::string(" ") + l1 + " | " + l2, true);
                        }
                    }
                }
            }
            
            // CRITICAL: Increment frame counter IMMEDIATELY for precise tracking
            framesSinceLastLog++;
            
            // Process features in order of priority - NO THROTTLING
            bool moveIDsChanged = (moveID1 != prevMoveID1) || (moveID2 != prevMoveID2);
            bool criticalFeaturesActive = autoJumpEnabled.load() || autoActionEnabled.load() || autoAirtechEnabled.load();

            // Process frame advantage only when move IDs change or timers/overlays require ticking
            {
                bool faNeedsTick = FrameAdvantageTimersActive();
                if (moveIDsChanged || faNeedsTick) {
                    // Use context overload (currently thin wrapper) to begin migration
                    MonitorFrameAdvantage(GetCurrentPerFrameSample());
                }
            }
            
            // Run dummy auto-block using unified sample (still every frame for precision)
            MonitorDummyAutoBlock(GetCurrentPerFrameSample());

            // Practice-only: Defense helpers
            // Always RG takes effect when enabled; Random RG mimics Revival's per-frame coin flip.
            AlwaysRG::Tick(moveID1, moveID2);
            RandomRG::Tick(moveID1, moveID2);
            // Random Block: per-frame coin flip for the autoblock flag with safe OFF deferral
            RandomBlock::Tick(moveID1, moveID2);

            if (moveIDsChanged || criticalFeaturesActive) {
                // STEP 1: Process auto-actions FIRST (highest priority)
                // When tick-integrated mode is active, auto-actions are driven directly from the
                // engine's per-tick input hook; skip here to avoid double-processing.
                if (!g_tickIntegratedAutoActions.load()) {
                    ProcessTriggerDelays();      // Handle pending delays
                    // Pass cached move IDs to avoid extra reads and enable lighter math inside
                    MonitorAutoActions(moveID1, moveID2, prevMoveID1, prevMoveID2);
                }
                
                // STEP 2: Auto-jump
                // Always call to allow internal cleanup when toggled off; function self-checks enable state
                MonitorAutoJump();
                
                // STEP 3: Auto-airtech (every frame for precision, no throttling)
                MonitorAutoAirtech(moveID1, moveID2);  
                ClearDelayStatesIfNonActionable();     
            }

            // Continuous Recovery: restore values based on unified sample neutral flags + optional both-neutral delay
            {
                // Track RF freezes we started due to Continuous Recovery (per-side)
                static bool s_crRFFreezeP1 = false;
                static bool s_crRFFreezeP2 = false;
                // Both-neutral timing gate
                static bool s_prevBothNeutral = false;
                static unsigned long long s_bothNeutralStartMs = 0ULL;
                // Use unified sample values (populated earlier this frame)
                const PerFrameSample &sample = g_lastSample; // local alias
                auto resolveTargets = [](bool isP1) {
                    struct T { int hp; int meter; double rf; bool bic; bool hpOn; bool meterOn; bool rfOn; bool wantRedIC; } t; t={0,0,0.0,false,false,false,false,false};
                    // HP
                    int hpm = isP1 ? g_contRecHpModeP1.load() : g_contRecHpModeP2.load();
                    if (hpm > 0) {
                        t.hpOn = true;
                        if (hpm == 1) t.hp = MAX_HP;
                        else if (hpm == 2) t.hp = 3332; // FM preset
                        else if (hpm == 3) t.hp = CLAMP((isP1? g_contRecHpCustomP1.load() : g_contRecHpCustomP2.load()), 0, MAX_HP);
                    }
                    // Meter
                    int mm = isP1 ? g_contRecMeterModeP1.load() : g_contRecMeterModeP2.load();
                    if (mm > 0) {
                        t.meterOn = true;
                        if (mm == 1) t.meter = 0;
                        else if (mm == 2) t.meter = 1000;
                        else if (mm == 3) t.meter = 2000;
                        else if (mm == 4) t.meter = 3000;
                        else if (mm == 5) t.meter = CLAMP((isP1? g_contRecMeterCustomP1.load() : g_contRecMeterCustomP2.load()), 0, MAX_METER);
                    }
                    // RF
                    int rm = isP1 ? g_contRecRfModeP1.load() : g_contRecRfModeP2.load();
                    if (rm > 0) {
                        t.rfOn = true;
                        if (rm == 1) t.rf = 0.0;
                        else if (rm == 2) t.rf = 1000.0;
                        else if (rm == 3) t.rf = 500.0;
                        else if (rm == 4) t.rf = 999.0;
                        else if (rm == 5) t.rf = (double)CLAMP((int)(isP1? g_contRecRfCustomP1.load() : g_contRecRfCustomP2.load()), 0, (int)MAX_RF);
                        // Red != BIC: only honor BIC flag when using Custom RF mode
                        if (rm == 5) {
                            t.bic = isP1 ? g_contRecRfForceBlueICP1.load() : g_contRecRfForceBlueICP2.load();
                        } else {
                            t.bic = false;
                        }
                        // If using Red presets (500 or 999), ensure IC is not Blue for that side
                        t.wantRedIC = (rm == 3 || rm == 4);
                    }
                    return t;
                };
                
                if (moveIDsChanged) {
                    // Engine regen gating: if engine-managed regen (F4/F5) is active, do not perform CR writes
                    uint16_t engineParamA=0, engineParamB=0; EngineRegenMode regenMode = EngineRegenMode::Unknown;
                    bool gotParams = GetEngineRegenStatus(regenMode, engineParamA, engineParamB);
                    bool engineRegenActive = gotParams && (regenMode == EngineRegenMode::F5_FullOrPreset || regenMode == EngineRegenMode::F4_FineTuneActive);

                    // Neutral states from sample (already whitelisted)
                    bool p1NeutralNow = sample.neutral1;
                    bool p2NeutralNow = sample.neutral2;

                    // Optional both-neutral + delay gating
                    const Config::Settings& cfg = Config::GetSettings();
                    bool requireBoth = cfg.crRequireBothNeutral;
                    int delayMs = (cfg.crBothNeutralDelayMs < 0 ? 0 : cfg.crBothNeutralDelayMs);
                    bool bothNeutral = p1NeutralNow && p2NeutralNow;
                    unsigned long long nowMs = GetTickCount64();
                    if (requireBoth) {
                        if (bothNeutral) {
                            if (!s_prevBothNeutral) {
                                s_bothNeutralStartMs = nowMs;
                            }
                        } else {
                            s_bothNeutralStartMs = 0ULL;
                        }
                        s_prevBothNeutral = bothNeutral;
                    } else {
                        // When not requiring both, reset tracker to avoid stale timestamps
                        s_prevBothNeutral = false; s_bothNeutralStartMs = 0ULL;
                    }

                    // Define applier with captured gating context
                    auto applyForPlayerIfEligible = [&](int p){
                        bool enabled = (p==1)? g_contRecEnabledP1.load() : g_contRecEnabledP2.load();
                        if (!enabled) return;
                        uintptr_t baseNow = s_ptrCache.base ? s_ptrCache.base : GetEFZBase();
                        if (!baseNow) return;
                        uintptr_t pBase = ResolvePlayerBaseBestEffort(p, baseNow);
                        if (!pBase) return;
                        auto getPlayerBase = [&](int idx) -> uintptr_t {
                            if (idx == p) return pBase;
                            return ResolvePlayerBaseBestEffort(idx, baseNow);
                        };
                        auto tg = resolveTargets(p==1);
                        bool wroteHpOrMeter = false;
                        bool appliedRf = false;
                        if (tg.hpOn) {
                            uintptr_t hpA = pBase + HP_OFFSET;
                            int cur=0; SafeReadMemory(hpA, &cur, sizeof(cur));
                            int tgt = CLAMP(tg.hp, 0, MAX_HP);
                            if (cur != tgt) {
                                SafeWriteMemory(pBase + HP_OFFSET, &tgt, sizeof(tgt));
                                SafeWriteMemory(pBase + HP_BAR_OFFSET, &tgt, sizeof(tgt));
                                wroteHpOrMeter = true;
                            }
                        }
                        if (tg.meterOn) {
                            uintptr_t mA = pBase + METER_OFFSET;
                            int curFull=0; SafeReadMemory(mA, &curFull, sizeof(curFull));
                            WORD cur = (WORD)(curFull & 0xFFFF);
                            WORD tgt = (WORD)CLAMP(tg.meter, 0, MAX_METER);
                            if (cur != tgt) { SafeWriteMemory(mA, &tgt, sizeof(tgt)); wroteHpOrMeter = true; }
                        }
                        if (tg.rfOn) {
                            // Use robust setter for both players to avoid desync; read other side first
                            double p1rf=0.0, p2rf=0.0;
                            uintptr_t p1B = getPlayerBase(1);
                            uintptr_t p2B = getPlayerBase(2);
                            if (p1B) SafeReadMemory(p1B + RF_OFFSET, &p1rf, sizeof(p1rf));
                            if (p2B) SafeReadMemory(p2B + RF_OFFSET, &p2rf, sizeof(p2rf));
                            if (p==1) p1rf = tg.rf; else p2rf = tg.rf;
                            (void)SetRFValuesDirect(p1rf, p2rf); // best-effort write; freeze handles persistence
                            appliedRf = true;
                            if (Config::GetSettings().freezeRFAfterContRec) {
                                StartRFFreezeOneFromCR(p, tg.rf);
                                if (p==1) s_crRFFreezeP1 = true; else s_crRFFreezeP2 = true;
                            }
                        }
                        if (tg.bic) {
                            // Set IC color to blue for restored side only; preserve the other side
                            uintptr_t p1B = getPlayerBase(1);
                            uintptr_t p2B = getPlayerBase(2);
                            int ic1=1, ic2=1;
                            if (p1B) SafeReadMemory(p1B + IC_COLOR_OFFSET, &ic1, sizeof(ic1));
                            if (p2B) SafeReadMemory(p2B + IC_COLOR_OFFSET, &ic2, sizeof(ic2));
                            bool p1Blue = (p==1)? true : (ic1 != 0);
                            bool p2Blue = (p==2)? true : (ic2 != 0);
                            SetICColorDirect(p1Blue, p2Blue);
                        } else if (tg.wantRedIC) {
                            // Red RF preset chosen: ensure this side is not Blue IC (flip to Red if needed)
                            uintptr_t p1B = getPlayerBase(1);
                            uintptr_t p2B = getPlayerBase(2);
                            int ic1=0, ic2=0;
                            if (p1B) SafeReadMemory(p1B + IC_COLOR_OFFSET, &ic1, sizeof(ic1));
                            if (p2B) SafeReadMemory(p2B + IC_COLOR_OFFSET, &ic2, sizeof(ic2));
                            bool p1Blue = (ic1 != 0);
                            bool p2Blue = (ic2 != 0);
                            if ((p==1 && p1Blue) || (p==2 && p2Blue)) {
                                // Force this side to Red (false), keep the other side as-is
                                bool newP1Blue = (p==1) ? false : p1Blue;
                                bool newP2Blue = (p==2) ? false : p2Blue;
                                SetICColorDirect(newP1Blue, newP2Blue);
                            }
                        }
                        // Log recovery reason and state snapshot
                        {
                            // Build a concise reason based on gating config captured from outer scope
                            std::string reason;
                            if (requireBoth) {
                                unsigned long long elapsed = (s_bothNeutralStartMs == 0ULL) ? 0ULL : (nowMs - s_bothNeutralStartMs);
                                reason = std::string("both-neutral ") + (bothNeutral?"true":"false") + ", delay=" + std::to_string(delayMs) + "ms, elapsed=" + std::to_string((unsigned long long)elapsed) + "ms";
                            } else {
                                int prevM = (p==1? prevMoveID1 : prevMoveID2);
                                int curM  = (p==1? moveID1 : moveID2);
                                reason = std::string("transition ") + std::to_string(prevM) + "->" + std::to_string(curM);
                            }
                            // Targets summary (+ move IDs to debug misclassification)
                            const char* hpOnStr = tg.hpOn ? "on" : "off";
                            char hpVal[24] = {0};
                            if (tg.hpOn) { snprintf(hpVal, sizeof(hpVal), "=%d", CLAMP(tg.hp, 0, MAX_HP)); }
                            const char* meterOnStr = tg.meterOn ? "on" : "off";
                            char meterVal[24] = {0};
                            if (tg.meterOn) { snprintf(meterVal, sizeof(meterVal), "=%d", CLAMP(tg.meter, 0, MAX_METER)); }
                            const char* rfOnStr = tg.rfOn ? "on" : "off";
                            double rfVal = tg.rfOn ? tg.rf : 0.0;
                            const char* bicStr = tg.bic ? "true" : "false";
                            const char* redIcStr = tg.wantRedIC ? "true" : "false";
                            char buf[256];
                            snprintf(buf, sizeof(buf), "[FM][ContRec] P%d recovered; reason=%s; p1Neutral=%s p2Neutral=%s engineRegen=%s moves{p1:%d p2:%d} targets{hp:%s%s meter:%s%s rf:%s%.1f BIC:%s RedIC:%s}",
                                     (p==1?1:2),
                                     reason.c_str(),
                                     (p1NeutralNow?"true":"false"), (p2NeutralNow?"true":"false"),
                                     (engineRegenActive?"true":"false"),
                                     (int)moveID1, (int)moveID2,
                                     hpOnStr, hpVal,
                                     meterOnStr, meterVal,
                                     rfOnStr, rfVal,
                                     bicStr, redIcStr);
                            #if defined(ENABLE_FRAME_ADV_DEBUG)
                            LogOut(buf, true);
                            #endif
                            #if defined(ENABLE_FRAME_ADV_DEBUG)
                            if (detailedLogging.load()) {
                                // Secondary detail: wrote flags
                                char buf2[192];
                                snprintf(buf2, sizeof(buf2), "[FM][ContRec] P%d wroteHpOrMeter=%s appliedRF=%s freezeRF=%s",
                                         (p==1?1:2), wroteHpOrMeter?"true":"false", appliedRf?"true":"false",
                                         ((p==1? s_crRFFreezeP1 : s_crRFFreezeP2)?"true":"false"));
                                LogOut(buf2, true);
                            }
                            #endif
                        }
                    };

                    // Determine per-player eligibility
                    bool p1Eligible = false, p2Eligible = false;
                    // Transition detection (legacy behavior path): previously not actionable -> now neutral
                    bool transitionedToNeutralP1 = (!IsActionable(prevMoveID1)) && sample.neutral1;
                    bool transitionedToNeutralP2 = (!IsActionable(prevMoveID2)) && sample.neutral2;
                    if (!engineRegenActive) {
                        if (requireBoth) {
                            // Both must be neutral for at least delayMs
                            if (bothNeutral) {
                                unsigned long long start = s_bothNeutralStartMs;
                                if (start != 0ULL && (nowMs - start) >= (unsigned long long)delayMs) {
                                    p1Eligible = g_contRecEnabledP1.load();
                                    p2Eligible = g_contRecEnabledP2.load();
                                }
                            }
                        } else {
                            // Legacy behavior: on transition to allowed neutral per-side (now via sample flags)
                            if (transitionedToNeutralP1) p1Eligible = g_contRecEnabledP1.load();
                            if (transitionedToNeutralP2) p2Eligible = g_contRecEnabledP2.load();
                        }
                    }

                    #if defined(ENABLE_FRAME_ADV_DEBUG)
                    if (engineRegenActive) {
                        // Helpful note when engine regen blocks CR on a transition or both-neutral window
                        if ((requireBoth && bothNeutral) || (!requireBoth && (transitionedToNeutralP1 || transitionedToNeutralP2))) {
                            LogOut("[FM][ContRec] Skipped due to engine regen active (F4/F5)", true);
                        }
                    }
                    #endif

                    if (p1Eligible) applyForPlayerIfEligible(1);
                    if (p2Eligible) applyForPlayerIfEligible(2);

                    // Stop per-side RF freeze only if we started it via Continuous Recovery and it's no longer active
                    bool p1RFActive = g_contRecEnabledP1.load() && (g_contRecRfModeP1.load() > 0);
                    bool p2RFActive = g_contRecEnabledP2.load() && (g_contRecRfModeP2.load() > 0);
                    if (s_crRFFreezeP1 && !p1RFActive) { StopRFFreezePlayer(1); s_crRFFreezeP1 = false; }
                    if (s_crRFFreezeP2 && !p2RFActive) { StopRFFreezePlayer(2); s_crRFFreezeP2 = false; }
                }

                // Auto-fix HP anomalies in neutral: if enabled, and a side is neutral with HP<=0, set to 9999.
                if (Config::GetSettings().autoFixHPOnNeutral) {
                    uintptr_t baseNow = s_ptrCache.base;
                    uintptr_t p1B = ResolvePlayerBaseBestEffort(1, baseNow);
                    uintptr_t p2B = ResolvePlayerBaseBestEffort(2, baseNow);
                    if (p1B && sample.neutral1) {
                        int hp = 1; SafeReadMemory(p1B + HP_OFFSET, &hp, sizeof(hp));
                        if (hp <= 0) { int full = MAX_HP; SafeWriteMemory(p1B + HP_OFFSET, &full, sizeof(full)); SafeWriteMemory(p1B + HP_BAR_OFFSET, &full, sizeof(full)); }
                    }
                    if (p2B && sample.neutral2) {
                        int hp = 1; SafeReadMemory(p2B + HP_OFFSET, &hp, sizeof(hp));
                        if (hp <= 0) { int full = MAX_HP; SafeWriteMemory(p2B + HP_OFFSET, &full, sizeof(full)); SafeWriteMemory(p2B + HP_BAR_OFFSET, &full, sizeof(full)); }
                    }
                }
            }
            
            // (Removed duplicate late call to MonitorDummyAutoBlock; it now runs once early each frame.)

            // Publish a snapshot for other consumers at the end of logic section
            {
                // Resolve and cache addresses periodically to minimize ResolvePointer overhead
                static uintptr_t s_p1YAddr = 0, s_p2YAddr = 0;
                static uintptr_t s_p1XAddr = 0, s_p2XAddr = 0;
                static uintptr_t s_p1HpAddr = 0, s_p2HpAddr = 0;
                static uintptr_t s_p1MeterAddr = 0, s_p2MeterAddr = 0;
                static uintptr_t s_p1RfAddr = 0, s_p2RfAddr = 0;
                static uintptr_t s_p1CharNameAddr = 0, s_p2CharNameAddr = 0; // used to derive IDs if needed
                static uintptr_t s_p1CharIdAddr = 0, s_p2CharIdAddr = 0; // if ID offset exists in struct (fallback to name->id map)
                static int s_cacheCounter = 0;
                // Clean Hit helper state (HP-based, one-shot)
                static int s_prevHpP1 = -1, s_prevHpP2 = -1;
                static int s_cleanHitSuppress = 0; // small cooldown in frames to avoid dupes
                if (++s_cacheCounter >= 192 || !s_p1YAddr || !s_p2YAddr || !s_p1XAddr || !s_p2XAddr ||
                    !s_p1HpAddr || !s_p2HpAddr || !s_p1MeterAddr || !s_p2MeterAddr || !s_p1RfAddr || !s_p2RfAddr ||
                    !s_p1CharNameAddr || !s_p2CharNameAddr) {
                    s_p1YAddr = ResolvePlayerFieldBestEffort(1, YPOS_OFFSET, base);
                    s_p2YAddr = ResolvePlayerFieldBestEffort(2, YPOS_OFFSET, base);
                    s_p1XAddr = ResolvePlayerFieldBestEffort(1, XPOS_OFFSET, base);
                    s_p2XAddr = ResolvePlayerFieldBestEffort(2, XPOS_OFFSET, base);
                    s_p1HpAddr = ResolvePlayerFieldBestEffort(1, HP_OFFSET, base);
                    s_p2HpAddr = ResolvePlayerFieldBestEffort(2, HP_OFFSET, base);
                    s_p1MeterAddr = ResolvePlayerFieldBestEffort(1, METER_OFFSET, base);
                    s_p2MeterAddr = ResolvePlayerFieldBestEffort(2, METER_OFFSET, base);
                    s_p1RfAddr = ResolvePlayerFieldBestEffort(1, RF_OFFSET, base);
                    s_p2RfAddr = ResolvePlayerFieldBestEffort(2, RF_OFFSET, base);
                    s_p1CharNameAddr = ResolvePlayerFieldBestEffort(1, CHARACTER_NAME_OFFSET, base);
                    s_p2CharNameAddr = ResolvePlayerFieldBestEffort(2, CHARACTER_NAME_OFFSET, base);
                    s_cacheCounter = 0;
                }

                // Read values with best-effort safety
                double p1Y=0.0, p2Y=0.0; if (s_p1YAddr) SafeReadMemory(s_p1YAddr, &p1Y, sizeof(p1Y)); if (s_p2YAddr) SafeReadMemory(s_p2YAddr, &p2Y, sizeof(p2Y));
                double p1X=0.0, p2X=0.0; if (s_p1XAddr) SafeReadMemory(s_p1XAddr, &p1X, sizeof(p1X)); if (s_p2XAddr) SafeReadMemory(s_p2XAddr, &p2X, sizeof(p2X));
                int p1Hp=0, p2Hp=0; if (s_p1HpAddr) SafeReadMemory(s_p1HpAddr, &p1Hp, sizeof(p1Hp)); if (s_p2HpAddr) SafeReadMemory(s_p2HpAddr, &p2Hp, sizeof(p2Hp));
                int p1Meter=0, p2Meter=0; if (s_p1MeterAddr) { unsigned short w=0; SafeReadMemory(s_p1MeterAddr, &w, sizeof(w)); p1Meter=(int)w; } if (s_p2MeterAddr) { unsigned short w=0; SafeReadMemory(s_p2MeterAddr, &w, sizeof(w)); p2Meter=(int)w; }
                double p1Rf=0.0, p2Rf=0.0; if (s_p1RfAddr) SafeReadMemory(s_p1RfAddr, &p1Rf, sizeof(p1Rf)); if (s_p2RfAddr) SafeReadMemory(s_p2RfAddr, &p2Rf, sizeof(p2Rf));

                FrameSnapshot snap{};
                snap.tickMs = GetTickCount64();
                snap.phase = currentPhase;
                snap.mode = currentMode;
                snap.p1Move = moveID1;
                snap.p2Move = moveID2;
                snap.prevP1Move = prevMoveID1;
                snap.prevP2Move = prevMoveID2;
                // Use cached actionable from unified sample for current moveID2
                snap.p2BlockEdge = (IsBlockstun(prevMoveID2) && !IsBlockstun(moveID2) && g_lastSample.actionable2);
                snap.p2HitstunEdge = (!IsHitstun(prevMoveID2) && IsHitstun(moveID2));
                snap.p1X = p1X; snap.p2X = p2X;
                snap.p1Y = p1Y; snap.p2Y = p2Y;
                snap.p1Hp = p1Hp; snap.p2Hp = p2Hp;
                snap.p1Meter = p1Meter; snap.p2Meter = p2Meter;
                snap.p1RF = p1Rf; snap.p2RF = p2Rf;
                // Character IDs: derive from name if direct ID offset is unavailable
                int pid1 = -1, pid2 = -1;
                if (s_p1CharNameAddr) {
                    char name1[16] = {0}; SafeReadMemory(s_p1CharNameAddr, &name1, sizeof(name1)-1);
                    pid1 = CharacterSettings::GetCharacterID(std::string(name1));
                }
                if (s_p2CharNameAddr) {
                    char name2[16] = {0}; SafeReadMemory(s_p2CharNameAddr, &name2, sizeof(name2)-1);
                    pid2 = CharacterSettings::GetCharacterID(std::string(name2));
                }
                snap.p1CharId = pid1; snap.p2CharId = pid2;

                // One-shot Akiko Clean Hit helper (frame-monitor based): trigger on HP drop of defender
                if (currentPhase == GamePhase::Match && DirectDrawHook::isHooked) {
                    if (s_prevHpP1 < 0 || s_prevHpP2 < 0) { s_prevHpP1 = p1Hp; s_prevHpP2 = p2Hp; }
                    bool p1Dropped = (p1Hp < s_prevHpP1);
                    bool p2Dropped = (p2Hp < s_prevHpP2);
                    if (s_cleanHitSuppress > 0) { s_cleanHitSuppress--; }

                    int atkPlayer = 0, defPlayer = 0;
                    if (p2Dropped && !p1Dropped) { atkPlayer = 1; defPlayer = 2; }
                    else if (p1Dropped && !p2Dropped) { atkPlayer = 2; defPlayer = 1; }

                    if (atkPlayer != 0 && s_cleanHitSuppress == 0) {
                        // Gating: attacker must be Akiko, and user enabled per-player flag
                        bool akikoAtk = (atkPlayer == 1) ? (displayData.p1CharID == CHAR_ID_AKIKO)
                                                         : (displayData.p2CharID == CHAR_ID_AKIKO);
                        bool userEnabled = (atkPlayer == 1) ? displayData.p1AkikoShowCleanHit
                                                            : displayData.p2AkikoShowCleanHit;
                        if (akikoAtk && userEnabled) {
                            // Move gating: only last hit of 623 (259 for A/B, 254 for C)
                            short atkMove = (atkPlayer == 1) ? moveID1 : moveID2;
                            bool isLastAB = (atkMove == AKIKO_MOVE_623_LAST_AB);
                            bool isLastC  = (atkMove == AKIKO_MOVE_623_LAST_C);
                            if (isLastAB || isLastC) {
                                // Compute dY using current positions
                                double atkY = (atkPlayer == 1) ? p1Y : p2Y;
                                double defY = (defPlayer == 1) ? p1Y : p2Y;
                                double diff = atkY - defY;

                                std::stringstream ss; ss.setf(std::ios::fixed); ss << std::setprecision(2);
                                ss << "Akiko 623 last hit dY=" << diff << "  ";
                                if (isLastC) {
                                    if (diff > 47.0 && diff < 53.0) {
                                        ss << "FULL CLEAN HIT!";
                                    } else if (diff > 40.0 && diff < 60.0) {
                                        ss << "PARTIAL CLEAN HIT!";
                                        if (diff >= 47.0) ss << " (Enemy's too high for FULL by " << (diff - 53.0) << ")";
                                        else ss << " (Enemy's too low for FULL by " << (47.0 - diff) << ")";
                                    } else {
                                        if (diff >= 40.0) ss << "Enemy's too high for PARTIAL by " << (diff - 60.0);
                                        else ss << "Enemy's too low for PARTIAL by " << (40.0 - diff);
                                    }
                                } else {
                                    if (diff > 32.0 && diff < 48.0) {
                                        ss << "CLEAN HIT!";
                                    } else if (diff >= 48.0) {
                                        ss << "Enemy's too high by " << (diff - 48.0);
                                    } else {
                                        ss << "Enemy's too low by " << (32.0 - diff);
                                    }
                                }

                                LogOut(std::string("[CLEANHIT][FM] atk=P") + std::to_string(atkPlayer) +
                                       " def=P" + std::to_string(defPlayer) +
                                       " move=" + std::to_string((int)atkMove) +
                                       " diffY=" + std::to_string(diff) +
                                       " -> " + ss.str(), true);
                                DirectDrawHook::AddMessage(ss.str(), "SYSTEM", RGB(255, 255, 0), 1500, 0, 100);
                                s_cleanHitSuppress = 8; // ~40ms at 192fps
                            }
                        }
                    }

                    // Update previous HPs after detection pass
                    s_prevHpP1 = p1Hp; s_prevHpP2 = p2Hp;
                }

                PublishSnapshot(snap);

                // Enforce character-specific settings on a modest cadence (~16 Hz)
                static int charEnfDecim = 0;
                if ((++charEnfDecim % 12) == 0) {
                    // Keep IDs fresh for enforcement decisions
                    if (snap.p1CharId >= 0) displayData.p1CharID = snap.p1CharId;
                    if (snap.p2CharId >= 0) displayData.p2CharID = snap.p2CharId;
                    CharacterSettings::TickCharacterEnforcements(base, displayData);
                }

                // Read character-specific values infrequently (~every 2 seconds)
                {
                    using clock = std::chrono::steady_clock;
                    static clock::time_point lastCharRead = clock::time_point{};
                    auto now = clock::now();
                    if (lastCharRead.time_since_epoch().count() == 0 || (now - lastCharRead) >= std::chrono::seconds(2)) {
                        // Sync IDs for ReadCharacterValues
                        if (snap.p1CharId >= 0) displayData.p1CharID = snap.p1CharId;
                        if (snap.p2CharId >= 0) displayData.p2CharID = snap.p2CharId;
                        CharacterSettings::ReadCharacterValues(base, displayData);
                        lastCharRead = now;
                    }
                }
            }

            // Now update previous moveIDs for next frame
            prevMoveID1 = moveID1;
            prevMoveID2 = moveID2;

            // AttackReader disabled to reduce CPU usage
            if (!DISABLE_ATTACK_READER && moveID1 != lastLoggedMoveID1 && IsAttackMove(moveID1)) {
            // Don't log too frequently - enforce a cooldown
            if (moveLogCooldown.load() <= 0) {
                AttackReader::LogMoveData(1, moveID1);
                lastLoggedMoveID1 = moveID1;
                moveLogCooldown.store(30); // Don't log another move for 30 frames (about 0.5s)
            }
        }
        
    // AttackReader disabled to reduce CPU usage
    if (!DISABLE_ATTACK_READER && moveID2 != lastLoggedMoveID2 && IsAttackMove(moveID2)) {
            // Don't log too frequently - enforce a cooldown
            if (moveLogCooldown.load() <= 0) {
                AttackReader::LogMoveData(2, moveID2);
                lastLoggedMoveID2 = moveID2;
                moveLogCooldown.store(30); // Don't log another move for 30 frames (about 0.5s)
            }
        }
    }

    // Process move change logging

        // Decrement cooldown counter
        if (moveLogCooldown.load() > 0) {
            moveLogCooldown.store(moveLogCooldown.load() - 1);
        }
        
FRAME_MONITOR_FRAME_END:
        // New paced sleep using accumulated schedule (expectedNext)
        auto beforeSleep = clock::now();
        while (beforeSleep < expectedNext) {
            auto remaining = expectedNext - beforeSleep;
            if (remaining > std::chrono::microseconds(100)) {
                // Sleep all but ~100us; rely on timeBeginPeriod(1) for ~1ms granularity when active
                auto sleepChunk = remaining - std::chrono::microseconds(100);
                std::this_thread::sleep_for(sleepChunk);
            } else {
                // Only do tight spin precision in Match; otherwise yield-friendly sleep
                if (currentPhase == GamePhase::Match) {
                    int spinIters = 0;
                    while (clock::now() < expectedNext) {
                        _mm_pause();
                        if ((++spinIters & 0xFF) == 0) {
                            // Yield occasionally to avoid starving other threads
                            SwitchToThread();
                        }
                    }
                } else {
                    // Outside matches, just sleep the small remainder to avoid CPU churn
                    std::this_thread::sleep_for(std::chrono::microseconds(100));
                }
                break;
            }
            beforeSleep = clock::now();
        }

        // Maintain RF freeze inline only during Match. Outside Match, avoid repeated stop spam.
        if (currentPhase == GamePhase::Match) {
            // ~32 Hz maintenance
            static int rfDecim = 0; if ((rfDecim++ % 6) == 0) { UpdateRFFreezeTick(); }
        } else {
            // Outside Match: do not maintain or force-stop; CR will handle start/stop explicitly
        }

        auto frameEnd = clock::now();
        long long actualNs = std::chrono::duration_cast<std::chrono::nanoseconds>(frameEnd - frameStart).count();
        long long targetNs = targetFrameTime.count();
        long long drift = actualNs - targetNs; // positive = late this frame
        driftAccum += drift;
        absDriftAccum += (drift < 0 ? -drift : drift);
        if (drift > maxLate) maxLate = drift;
        if (drift < maxEarly) maxEarly = drift;
        if (actualNs > targetNs * 2) oversleepCount++;
        driftSamples++;

        // Advance schedule (even if we were late) to avoid accumulating latency
        expectedNext += targetFrameTime;
        // If we are more than 4 frames late, realign to now to prevent runaway catch-up loop
        if (frameEnd - expectedNext > targetFrameTime * 4) {
            expectedNext = frameEnd + targetFrameTime;
        }

        if (driftSamples >= 960) {
            double avgDriftUs = (double)driftAccum / driftSamples / 1000.0;
            double avgAbsDriftUs = (double)absDriftAccum / driftSamples / 1000.0;
            if (Config::GetSettings().enableFpsDiagnostics) LogOut("[FRAME_MONITOR][TIMING] samples=" + std::to_string(driftSamples) +
             " avgDrift(us)=" + std::to_string(avgDriftUs) +
             " avgAbs(us)=" + std::to_string(avgAbsDriftUs) +
             " maxLate(ms)=" + std::to_string(maxLate/1e6) +
             " maxEarly(ms)=" + std::to_string(maxEarly/1e6) +
             " oversleep>2x=" + std::to_string(oversleepCount), detailedLogging.load());
            if (Config::GetSettings().enableFpsDiagnostics && sectionTiming && sec_samples > 0) {
          LogOut("[FRAME MONITOR][SECTIONS] samples=" + std::to_string(sec_samples) +
              " memAvg(us)=" + std::to_string((sec_mem / 1000.0)/sec_samples) +
              " logicAvg(us)=" + std::to_string((sec_logic / 1000.0)/sec_samples) +
              " featAvg(us)=" + std::to_string((sec_features / 1000.0)/sec_samples), detailedLogging.load());
                sec_mem = sec_logic = sec_features = 0;
                sec_samples = 0;
            }
            driftAccum = 0;
            absDriftAccum = 0;
            maxLate = 0;
            maxEarly = 0;
            driftSamples = 0;
            oversleepCount = 0;
        }
    }
    
    if (Config::GetSettings().enableFpsDiagnostics || detailedLogging.load()) {
        LogOut("[FRAME MONITOR] Shutting down frame monitor thread", true);
    }
    if (highResActive) {
        timeEndPeriod(1);
    }
}

// Accessor returns last published per-frame sample
const PerFrameSample& GetCurrentPerFrameSample() {
    // Before first frame monitor tick, return zero-initialized sample
    static PerFrameSample initial{};
    if (frameCounter.load() == 0) return initial;
    return g_lastSample;
}

void ReinitializeOverlays() {
    // Clear existing trigger messages
    if (g_TriggerAfterBlockId != -1) {
        DirectDrawHook::RemovePermanentMessage(g_TriggerAfterBlockId);
        g_TriggerAfterBlockId = -1;
    }
    if (g_TriggerOnWakeupId != -1) {
        DirectDrawHook::RemovePermanentMessage(g_TriggerOnWakeupId);
        g_TriggerOnWakeupId = -1;
    }
    if (g_TriggerAfterHitstunId != -1) {
        DirectDrawHook::RemovePermanentMessage(g_TriggerAfterHitstunId);
        g_TriggerAfterHitstunId = -1;
    }
    if (g_TriggerAfterAirtechId != -1) {
        DirectDrawHook::RemovePermanentMessage(g_TriggerAfterAirtechId);
        g_TriggerAfterAirtechId = -1;
    }
    if (g_TriggerOnRGId != -1) {
        DirectDrawHook::RemovePermanentMessage(g_TriggerOnRGId);
        g_TriggerOnRGId = -1;
    }
    
    // Reset frame advantage display
    if (g_FrameAdvantageId != -1) {
        DirectDrawHook::RemovePermanentMessage(g_FrameAdvantageId);
        g_FrameAdvantageId = -1;
    }
    if (g_FrameAdvantage2Id != -1) {
        DirectDrawHook::RemovePermanentMessage(g_FrameAdvantage2Id);
        g_FrameAdvantage2Id = -1;
    }
    if (g_FrameGapId != -1) {
        DirectDrawHook::RemovePermanentMessage(g_FrameGapId);
        g_FrameGapId = -1;
    }
    
    // Reset auto-tech and auto-jump status displays
    if (g_AirtechStatusId != -1) {
        DirectDrawHook::RemovePermanentMessage(g_AirtechStatusId);
        g_AirtechStatusId = -1;
    }
    
    if (g_JumpStatusId != -1) {
        DirectDrawHook::RemovePermanentMessage(g_JumpStatusId);
        g_JumpStatusId = -1;
    }
    
    // Also reset stats display IDs so they'll be recreated if enabled
    if (g_statsDisplayEnabled.load()) {
        if (g_statsP1ValuesId != -1) {
            DirectDrawHook::RemovePermanentMessage(g_statsP1ValuesId);
            DirectDrawHook::RemovePermanentMessage(g_statsP2ValuesId);
            DirectDrawHook::RemovePermanentMessage(g_statsPositionId);
            DirectDrawHook::RemovePermanentMessage(g_statsMoveIdId);
            if (g_statsBlockstunId != -1) { DirectDrawHook::RemovePermanentMessage(g_statsBlockstunId); g_statsBlockstunId = -1; }
            if (g_statsUntechId != -1) { DirectDrawHook::RemovePermanentMessage(g_statsUntechId); g_statsUntechId = -1; }
            if (g_statsNayukiId != -1) { DirectDrawHook::RemovePermanentMessage(g_statsNayukiId); g_statsNayukiId = -1; }
            if (g_statsMisuzuId != -1) { DirectDrawHook::RemovePermanentMessage(g_statsMisuzuId); g_statsMisuzuId = -1; }
            if (g_statsMishioId != -1) { DirectDrawHook::RemovePermanentMessage(g_statsMishioId); g_statsMishioId = -1; }
            if (g_statsRumiId != -1) { DirectDrawHook::RemovePermanentMessage(g_statsRumiId); g_statsRumiId = -1; }
            if (g_statsIkumiId != -1) { DirectDrawHook::RemovePermanentMessage(g_statsIkumiId); g_statsIkumiId = -1; }
            if (g_statsMaiId != -1) { DirectDrawHook::RemovePermanentMessage(g_statsMaiId); g_statsMaiId = -1; }
            if (g_statsMinagiId != -1) { DirectDrawHook::RemovePermanentMessage(g_statsMinagiId); g_statsMinagiId = -1; }
            g_statsP1ValuesId = -1;
            g_statsP2ValuesId = -1;
            g_statsPositionId = -1;
            g_statsMoveIdId = -1;
            if (g_statsBlockstunId != -1) { DirectDrawHook::RemovePermanentMessage(g_statsBlockstunId); g_statsBlockstunId = -1; }
            if (g_statsUntechId != -1) { DirectDrawHook::RemovePermanentMessage(g_statsUntechId); g_statsUntechId = -1; }
        }
    }
    
    // Force an immediate update of the trigger overlay
    UpdateTriggerOverlay();
    
    LogOut("[FRAME MONITOR] Reinitialized overlay displays", true);
}

bool AreCharactersInitialized() {
    uintptr_t base = GetEFZBase();
    if (!base) {
        return false;
    }

    // Check if both P1 and P2 character pointers exist
    uintptr_t p1StructAddr = 0;
    uintptr_t p2StructAddr = 0;
    SafeReadMemory(base + EFZ_BASE_OFFSET_P1, &p1StructAddr, sizeof(uintptr_t));
    SafeReadMemory(base + EFZ_BASE_OFFSET_P2, &p2StructAddr, sizeof(uintptr_t));

    // Check if both pointers are valid and at least one has a non-zero HP value
    if (p1StructAddr && p2StructAddr) {
        int hp1 = 0, hp2 = 0;
        uintptr_t hp1Addr = p1StructAddr + HP_OFFSET;
        uintptr_t hp2Addr = p2StructAddr + HP_OFFSET;

        SafeReadMemory(hp1Addr, &hp1, sizeof(int));
        SafeReadMemory(hp2Addr, &hp2, sizeof(int));

        // Consider initialized if both pointers exist and at least one has HP
        return (hp1 > 0 || hp2 > 0);
    }
    
    return false;
}

void UpdateStatsDisplay() {
    // Always require overlay hook; allow Clean Hit helper to run even if stats are disabled
    if (!DirectDrawHook::isHooked) {
        // Clear any existing messages when overlay is unavailable
        if (g_statsP1ValuesId != -1) {
            DirectDrawHook::RemovePermanentMessage(g_statsP1ValuesId);
            DirectDrawHook::RemovePermanentMessage(g_statsP2ValuesId);
            DirectDrawHook::RemovePermanentMessage(g_statsPositionId);
            DirectDrawHook::RemovePermanentMessage(g_statsMoveIdId);
            g_statsP1ValuesId = -1;
            g_statsP2ValuesId = -1;
            g_statsPositionId = -1;
            g_statsMoveIdId = -1;
            if (g_statsBlockstunId != -1) { DirectDrawHook::RemovePermanentMessage(g_statsBlockstunId); g_statsBlockstunId = -1; }
            if (g_statsUntechId != -1) { DirectDrawHook::RemovePermanentMessage(g_statsUntechId); g_statsUntechId = -1; }
        }
        if (g_statsCleanHitId != -1) {
            DirectDrawHook::RemovePermanentMessage(g_statsCleanHitId);
            g_statsCleanHitId = -1;
        }
        if (g_statsNayukiId != -1) {
            DirectDrawHook::RemovePermanentMessage(g_statsNayukiId);
            g_statsNayukiId = -1;
        }
        if (g_statsMisuzuId != -1) {
            DirectDrawHook::RemovePermanentMessage(g_statsMisuzuId);
            g_statsMisuzuId = -1;
        }
        if (g_statsMishioId != -1) {
            DirectDrawHook::RemovePermanentMessage(g_statsMishioId);
            g_statsMishioId = -1;
        }
        if (g_statsRumiId != -1) {
            DirectDrawHook::RemovePermanentMessage(g_statsRumiId);
            g_statsRumiId = -1;
        }
        if (g_statsIkumiId != -1) {
            DirectDrawHook::RemovePermanentMessage(g_statsIkumiId);
            g_statsIkumiId = -1;
        }
        if (g_statsMaiId != -1) {
            DirectDrawHook::RemovePermanentMessage(g_statsMaiId);
            g_statsMaiId = -1;
        }
        if (g_statsMinagiId != -1) {
            DirectDrawHook::RemovePermanentMessage(g_statsMinagiId);
            g_statsMinagiId = -1;
        }
        if (g_statsAIFlagsId != -1) {
            DirectDrawHook::RemovePermanentMessage(g_statsAIFlagsId);
            g_statsAIFlagsId = -1;
        }
        if (g_statsBlockstunId != -1) { DirectDrawHook::RemovePermanentMessage(g_statsBlockstunId); g_statsBlockstunId = -1; }
        if (g_statsUntechId != -1) { DirectDrawHook::RemovePermanentMessage(g_statsUntechId); g_statsUntechId = -1; }
        return;
    }

    // Stats can be toggled off; keep Clean Hit helper independent of this
    bool statsOn = g_statsDisplayEnabled.load();
    if (!statsOn) {
        // Clear stats lines if they exist while stats are disabled
        if (g_statsP1ValuesId != -1) {
            DirectDrawHook::RemovePermanentMessage(g_statsP1ValuesId);
            DirectDrawHook::RemovePermanentMessage(g_statsP2ValuesId);
            DirectDrawHook::RemovePermanentMessage(g_statsPositionId);
            DirectDrawHook::RemovePermanentMessage(g_statsMoveIdId);
            g_statsP1ValuesId = -1;
            g_statsP2ValuesId = -1;
            g_statsPositionId = -1;
            g_statsMoveIdId = -1;
            if (g_statsBlockstunId != -1) { DirectDrawHook::RemovePermanentMessage(g_statsBlockstunId); g_statsBlockstunId = -1; }
            if (g_statsUntechId != -1) { DirectDrawHook::RemovePermanentMessage(g_statsUntechId); g_statsUntechId = -1; }
        }
        // Also clear any character-specific stat lines (Nayuki/Misuzu)
        if (g_statsNayukiId != -1) {
            DirectDrawHook::RemovePermanentMessage(g_statsNayukiId);
            g_statsNayukiId = -1;
        }
        if (g_statsMisuzuId != -1) {
            DirectDrawHook::RemovePermanentMessage(g_statsMisuzuId);
            g_statsMisuzuId = -1;
        }
        if (g_statsMishioId != -1) {
            DirectDrawHook::RemovePermanentMessage(g_statsMishioId);
            g_statsMishioId = -1;
        }
        if (g_statsRumiId != -1) {
            DirectDrawHook::RemovePermanentMessage(g_statsRumiId);
            g_statsRumiId = -1;
        }
        if (g_statsIkumiId != -1) {
            DirectDrawHook::RemovePermanentMessage(g_statsIkumiId);
            g_statsIkumiId = -1;
        }
        if (g_statsMaiId != -1) {
            DirectDrawHook::RemovePermanentMessage(g_statsMaiId);
            g_statsMaiId = -1;
        }
        if (g_statsMinagiId != -1) {
            DirectDrawHook::RemovePermanentMessage(g_statsMinagiId);
            g_statsMinagiId = -1;
        }
        if (g_statsAIFlagsId != -1) {
            DirectDrawHook::RemovePermanentMessage(g_statsAIFlagsId);
            g_statsAIFlagsId = -1;
        }
        if (g_statsBlockstunId != -1) { DirectDrawHook::RemovePermanentMessage(g_statsBlockstunId); g_statsBlockstunId = -1; }
        if (g_statsUntechId != -1) { DirectDrawHook::RemovePermanentMessage(g_statsUntechId); g_statsUntechId = -1; }
    }

    // Check for valid game state - return early if not in a valid state
    if (!AreCharactersInitialized()) {
        // Clear existing messages in invalid game state
        if (g_statsP1ValuesId != -1) {
            DirectDrawHook::RemovePermanentMessage(g_statsP1ValuesId);
            DirectDrawHook::RemovePermanentMessage(g_statsP2ValuesId);
            DirectDrawHook::RemovePermanentMessage(g_statsPositionId);
            DirectDrawHook::RemovePermanentMessage(g_statsMoveIdId);
            g_statsP1ValuesId = -1;
            g_statsP2ValuesId = -1;
            g_statsPositionId = -1;
            g_statsMoveIdId = -1;
        }
        if (g_statsCleanHitId != -1) {
            DirectDrawHook::RemovePermanentMessage(g_statsCleanHitId);
            g_statsCleanHitId = -1;
        }
        if (g_statsNayukiId != -1) {
            DirectDrawHook::RemovePermanentMessage(g_statsNayukiId);
            g_statsNayukiId = -1;
        }
        if (g_statsMisuzuId != -1) {
            DirectDrawHook::RemovePermanentMessage(g_statsMisuzuId);
            g_statsMisuzuId = -1;
        }
        if (g_statsMishioId != -1) {
            DirectDrawHook::RemovePermanentMessage(g_statsMishioId);
            g_statsMishioId = -1;
        }
        if (g_statsRumiId != -1) {
            DirectDrawHook::RemovePermanentMessage(g_statsRumiId);
            g_statsRumiId = -1;
        }
        if (g_statsIkumiId != -1) {
            DirectDrawHook::RemovePermanentMessage(g_statsIkumiId);
            g_statsIkumiId = -1;
        }
        if (g_statsMaiId != -1) {
            DirectDrawHook::RemovePermanentMessage(g_statsMaiId);
            g_statsMaiId = -1;
        }
        if (g_statsMinagiId != -1) {
            DirectDrawHook::RemovePermanentMessage(g_statsMinagiId);
            g_statsMinagiId = -1;
        }
        if (g_statsAIFlagsId != -1) {
            DirectDrawHook::RemovePermanentMessage(g_statsAIFlagsId);
            g_statsAIFlagsId = -1;
        }
        if (g_statsBlockstunId != -1) { DirectDrawHook::RemovePermanentMessage(g_statsBlockstunId); g_statsBlockstunId = -1; }
        if (g_statsUntechId != -1) { DirectDrawHook::RemovePermanentMessage(g_statsUntechId); g_statsUntechId = -1; }
        return;
    }

    // Prefer live snapshot; fallback to minimal direct reads only when stale
    FrameSnapshot snap{};
    bool haveSnap = TryGetLatestSnapshot(snap, 300);
    uintptr_t base = GetEFZBase();
    if (!base && !haveSnap) return;

    int p1Hp = 0, p2Hp = 0, p1Meter = 0, p2Meter = 0; double p1Rf = 0.0, p2Rf = 0.0;
    double p1X = 0.0, p1Y = 0.0, p2X = 0.0, p2Y = 0.0; short p1MoveId = 0, p2MoveId = 0;

    // Throttle heavy stats formatting & reads when overlay active.
    // We still maintain position cache every frame (using snapshot if present) for other systems.
    static int s_lastStatsUpdateFrame = -9999;
    static std::string s_lastP1Values, s_lastP2Values, s_lastPositions, s_lastMoveIds;

    bool wantStatsUpdate = true; // default when overlay off or we have no snapshot fallback
    if (statsOn) {
        int fNow = frameCounter.load();
        // Update at most every 4 internal frames (192Hz -> 48Hz) unless snapshot missing
        if (fNow - s_lastStatsUpdateFrame < 4 && haveSnap) {
            // Early position cache update from snapshot then skip expensive formatting
            UpdatePositionCache(snap.p1X, snap.p1Y, snap.p2X, snap.p2Y);
            return; // skip rest until next throttle boundary
        }
        s_lastStatsUpdateFrame = fNow;
    }

    if (haveSnap) {
        p1Hp = snap.p1Hp; p2Hp = snap.p2Hp;
        p1Meter = snap.p1Meter; p2Meter = snap.p2Meter;
        p1Rf = snap.p1RF; p2Rf = snap.p2RF;
        p1X = snap.p1X; p2X = snap.p2X;
        p1Y = snap.p1Y; p2Y = snap.p2Y;
        p1MoveId = snap.p1Move; p2MoveId = snap.p2Move;
    } else {
        static uintptr_t p1HpAddr = 0, p1MeterAddr = 0, p1RfAddr = 0;
        static uintptr_t p2HpAddr = 0, p2MeterAddr = 0, p2RfAddr = 0;
        static uintptr_t p1XAddr = 0, p1YAddr = 0, p2XAddr = 0, p2YAddr = 0;
        static uintptr_t p1MoveIdAddr = 0, p2MoveIdAddr = 0;
        static int cacheCounter = 0;
        if (cacheCounter++ >= 60 || !p1HpAddr) {
            p1HpAddr = ResolvePlayerFieldBestEffort(1, HP_OFFSET, base);
            p1MeterAddr = ResolvePlayerFieldBestEffort(1, METER_OFFSET, base);
            p1RfAddr = ResolvePlayerFieldBestEffort(1, RF_OFFSET, base);
            p1XAddr = ResolvePlayerFieldBestEffort(1, XPOS_OFFSET, base);
            p1YAddr = ResolvePlayerFieldBestEffort(1, YPOS_OFFSET, base);
            p1MoveIdAddr = ResolvePlayerFieldBestEffort(1, MOVE_ID_OFFSET, base);
            p2HpAddr = ResolvePlayerFieldBestEffort(2, HP_OFFSET, base);
            p2MeterAddr = ResolvePlayerFieldBestEffort(2, METER_OFFSET, base);
            p2RfAddr = ResolvePlayerFieldBestEffort(2, RF_OFFSET, base);
            p2XAddr = ResolvePlayerFieldBestEffort(2, XPOS_OFFSET, base);
            p2YAddr = ResolvePlayerFieldBestEffort(2, YPOS_OFFSET, base);
            p2MoveIdAddr = ResolvePlayerFieldBestEffort(2, MOVE_ID_OFFSET, base);
            cacheCounter = 0;
        }
        if (p1HpAddr) SafeReadMemory(p1HpAddr, &p1Hp, sizeof(int));
    if (p1MeterAddr) { unsigned short w=0; SafeReadMemory(p1MeterAddr, &w, sizeof(w)); p1Meter=(int)w; }
        if (p1RfAddr) SafeReadMemory(p1RfAddr, &p1Rf, sizeof(double));
        if (p2HpAddr) SafeReadMemory(p2HpAddr, &p2Hp, sizeof(int));
    if (p2MeterAddr) { unsigned short w=0; SafeReadMemory(p2MeterAddr, &w, sizeof(w)); p2Meter=(int)w; }
        if (p2RfAddr) SafeReadMemory(p2RfAddr, &p2Rf, sizeof(double));
        if (p1XAddr) SafeReadMemory(p1XAddr, &p1X, sizeof(double));
        if (p1YAddr) SafeReadMemory(p1YAddr, &p1Y, sizeof(double));
        if (p2XAddr) SafeReadMemory(p2XAddr, &p2X, sizeof(double));
        if (p2YAddr) SafeReadMemory(p2YAddr, &p2Y, sizeof(double));
        if (p1MoveIdAddr) SafeReadMemory(p1MoveIdAddr, &p1MoveId, sizeof(short));
        if (p2MoveIdAddr) SafeReadMemory(p2MoveIdAddr, &p2MoveId, sizeof(short));
    }

    // Feed positions cache for other systems
    UpdatePositionCache(p1X, p1Y, p2X, p2Y);

    // Format the strings
    std::stringstream p1Values, p2Values, positions, moveIds;
    
    // Player 1 values (HP, Meter, RF)
    p1Values << "P1:  HP: " << p1Hp << "  Meter: " << p1Meter << "  RF: " << std::fixed << std::setprecision(1) << p1Rf;
    
    // Player 2 values (HP, Meter, RF)
    p2Values << "P2:  HP: " << p2Hp << "  Meter: " << p2Meter << "  RF: " << std::fixed << std::setprecision(1) << p2Rf;
    
    // Player positions
    positions << "Position:  P1 [X: " << std::fixed << std::setprecision(2) << p1X 
              << ", Y: " << std::fixed << std::setprecision(2) << p1Y 
              << "]  P2 [X: " << std::fixed << std::setprecision(2) << p2X 
              << ", Y: " << std::fixed << std::setprecision(2) << p2Y << "]";
    
    // Move IDs with P1 guard requirement (HIGH/LOW/ANY) appended when available
    std::string p1ReqStr;
    if (statsOn) {
        // Sample P1's current frame flags to decode guard requirement
        if (base) {
            uintptr_t p1BasePtr = ResolvePlayerBaseBestEffort(1, base);
            if (p1BasePtr) {
                uint16_t st = 0, fr = 0; uintptr_t animTab = 0;
                if (SafeReadMemory(p1BasePtr + MOVE_ID_OFFSET, &st, sizeof(st)) &&
                    SafeReadMemory(p1BasePtr + CURRENT_FRAME_INDEX_OFFSET, &fr, sizeof(fr)) &&
                    SafeReadMemory(p1BasePtr + ANIM_TABLE_OFFSET, &animTab, sizeof(animTab)) && animTab) {
                    uintptr_t framesPtr = 0;
                    uintptr_t entryAddr = animTab + (static_cast<uintptr_t>(st) * ANIM_ENTRY_STRIDE) + ANIM_ENTRY_FRAMES_PTR_OFFSET;
                    if (SafeReadMemory(entryAddr, &framesPtr, sizeof(framesPtr)) && framesPtr) {
                        uintptr_t frameBlock = framesPtr + (static_cast<uintptr_t>(fr) * FRAME_BLOCK_STRIDE);
                        uint16_t atk=0, grd=0, hit=0;
                        if (SafeReadMemory(frameBlock + FRAME_ATTACK_PROPS_OFFSET, &atk, sizeof(atk))) {
                            // GuardProps preferred for blockable window; use AttackProps to decide HIGH/LOW
                            SafeReadMemory(frameBlock + FRAME_GUARD_PROPS_OFFSET, &grd, sizeof(grd));
                            SafeReadMemory(frameBlock + FRAME_HIT_PROPS_OFFSET, &hit, sizeof(hit));
                            bool isHigh = (atk & 0x1) != 0;
                            bool isLow  = (atk & 0x2) != 0;
                            if (isHigh && isLow) p1ReqStr = "ANY";
                            else if (isHigh)     p1ReqStr = "HIGH";
                            else if (isLow)      p1ReqStr = "LOW";
                            else if (grd != 0)   p1ReqStr = "ANY"; // guardable with no explicit high/low
                            // else: leave empty when not guardable
                        }
                    }
                }
            }
        }
        if (g_statsBlockstunId != -1) { DirectDrawHook::RemovePermanentMessage(g_statsBlockstunId); g_statsBlockstunId = -1; }
        if (g_statsUntechId != -1) { DirectDrawHook::RemovePermanentMessage(g_statsUntechId); g_statsUntechId = -1; }
    }
    moveIds << "MoveID:  P1: " << p1MoveId;
    if (!p1ReqStr.empty()) moveIds << " [" << p1ReqStr << "]";
    moveIds << "  P2: " << p2MoveId;

    // Set or update the display - MOVED DOWN from original position
    const int startX = 20;
    int startY = 100;
    const int lineHeight = 15;

    auto upsert = [&](int &id, const std::string &text) {
        if (id == -1) {
            id = DirectDrawHook::AddPermanentMessage(text, RGB(255,255,255), startX, startY);
        } else {
            DirectDrawHook::UpdatePermanentMessage(id, text, RGB(255,255,255));
        }
        startY += lineHeight;
    };

    if (statsOn) {
        upsert(g_statsP1ValuesId, p1Values.str());
        upsert(g_statsP2ValuesId, p2Values.str());
        upsert(g_statsPositionId, positions.str());

        // New: Separate Blockstun and Untech counters (raw internal values)
        auto readStunCounters = [&]() {
            uintptr_t baseNow = s_ptrCache.base ? s_ptrCache.base : GetEFZBase();
            if (!baseNow) return std::tuple<int,int,int,int>(-1,-1,-1,-1);
            uintptr_t p1 = ResolvePlayerBaseBestEffort(1, baseNow);
            uintptr_t p2 = ResolvePlayerBaseBestEffort(2, baseNow);
            short p1Blk=0, p2Blk=0, p1Hit=0, p2Hit=0;
            if (p1) { SafeReadMemory(p1 + BLOCKSTUN_OFFSET, &p1Blk, sizeof(p1Blk)); SafeReadMemory(p1 + UNTECH_OFFSET, &p1Hit, sizeof(p1Hit)); }
            if (p2) { SafeReadMemory(p2 + BLOCKSTUN_OFFSET, &p2Blk, sizeof(p2Blk)); SafeReadMemory(p2 + UNTECH_OFFSET, &p2Hit, sizeof(p2Hit)); }
            // Return raw counters as integers; clamp negatives to zero for display neatness
            auto clamp0 = [](int v){ return v < 0 ? 0 : v; };
            return std::tuple<int,int,int,int>(clamp0((int)p1Blk), clamp0((int)p1Hit), clamp0((int)p2Blk), clamp0((int)p2Hit));
        };
        {
            int p1BlkLF=0, p1HitLF=0, p2BlkLF=0, p2HitLF=0;
            std::tie(p1BlkLF, p1HitLF, p2BlkLF, p2HitLF) = readStunCounters();
            std::stringstream blkLine, hitLine;
            blkLine << "Blockstun:  P1 " << p1BlkLF << "  P2 " << p2BlkLF;
            hitLine << "Untech:    P1 " << p1HitLF << "  P2 " << p2HitLF;
            upsert(g_statsBlockstunId, blkLine.str());
            upsert(g_statsUntechId, hitLine.str());
        }

        // TEMP diagnostic: probe adjacent offset (+0x14C) to validate blockstun pointer choice.
        // Remove after confirming correct address in live testing.
        {
            uintptr_t baseNow = GetEFZBase();
            uintptr_t p1=ResolvePlayerBaseBestEffort(1, baseNow);
            uintptr_t p2=ResolvePlayerBaseBestEffort(2, baseNow);
            short p1Cand=0, p2Cand=0;
            if (p1) { SafeReadMemory(p1 + BLOCKSTUN_OFFSET + 2, &p1Cand, sizeof(p1Cand)); }
            if (p2) { SafeReadMemory(p2 + BLOCKSTUN_OFFSET + 2, &p2Cand, sizeof(p2Cand)); }
            std::stringstream diag;
            diag << "Blk?(+0x14C): P1 " << (int)(p1Cand < 0 ? 0 : p1Cand) << "  P2 " << (int)(p2Cand < 0 ? 0 : p2Cand);
            upsert(g_statsAIFlagsId, diag.str()); // reuse AI flags line position temporarily below character lines
        }

        // Character-specific: Nayuki (Awake) snowbunnies timer line
        // Show when either side is Nayuki(Awake); include Infinite indicator per side
        bool showNayuki = (displayData.p1CharID == CHAR_ID_NAYUKIB) || (displayData.p2CharID == CHAR_ID_NAYUKIB);
        if (showNayuki) {
            std::stringstream nayukiLine;
            nayukiLine << "Snowbunnies: ";
            if (displayData.p1CharID == CHAR_ID_NAYUKIB) {
                nayukiLine << "P1 " << displayData.p1NayukiSnowbunnies;
                if (displayData.p1NayukiInfiniteSnow) nayukiLine << " (Inf)";
            } else {
                nayukiLine << "P1 -";
            }
            nayukiLine << "  ";
            if (displayData.p2CharID == CHAR_ID_NAYUKIB) {
                nayukiLine << "P2 " << displayData.p2NayukiSnowbunnies;
                if (displayData.p2NayukiInfiniteSnow) nayukiLine << " (Inf)";
            } else {
                nayukiLine << "P2 -";
            }
            upsert(g_statsNayukiId, nayukiLine.str());
        } else {
            if (g_statsNayukiId != -1) { DirectDrawHook::RemovePermanentMessage(g_statsNayukiId); g_statsNayukiId = -1; }
        }

        // Character-specific: Misuzu Poison timer/level line
        bool showMisuzu = (displayData.p1CharID == CHAR_ID_MISUZU) || (displayData.p2CharID == CHAR_ID_MISUZU);
        if (showMisuzu) {
            std::stringstream misuzuLine;
            misuzuLine << "Misuzu Poison: ";
            if (displayData.p1CharID == CHAR_ID_MISUZU) {
                misuzuLine << "P1 " << displayData.p1MisuzuPoisonTimer;
                if (displayData.p1MisuzuInfinitePoison) misuzuLine << " (Inf)";
                misuzuLine << " [Lvl " << displayData.p1MisuzuPoisonLevel << "]";
            } else {
                misuzuLine << "P1 -";
            }
            misuzuLine << "  ";
            if (displayData.p2CharID == CHAR_ID_MISUZU) {
                misuzuLine << "P2 " << displayData.p2MisuzuPoisonTimer;
                if (displayData.p2MisuzuInfinitePoison) misuzuLine << " (Inf)";
                misuzuLine << " [Lvl " << displayData.p2MisuzuPoisonLevel << "]";
            } else {
                misuzuLine << "P2 -";
            }
            upsert(g_statsMisuzuId, misuzuLine.str());
        } else {
            if (g_statsMisuzuId != -1) { DirectDrawHook::RemovePermanentMessage(g_statsMisuzuId); g_statsMisuzuId = -1; }
        }

        // Character-specific: Mishio element and awakened timer
        bool showMishio = (displayData.p1CharID == CHAR_ID_MISHIO) || (displayData.p2CharID == CHAR_ID_MISHIO);
        if (showMishio) {
            auto elemName = [](int e){
                switch (e) {
                    case MISHIO_ELEM_NONE: return "None";
                    case MISHIO_ELEM_FIRE: return "Fire";
                    case MISHIO_ELEM_LIGHTNING: return "Lightning";
                    case MISHIO_ELEM_AWAKENED: return "Awakened";
                    default: return "?";
                }
            };
            std::stringstream mishioLine;
            mishioLine << "Mishio: ";
            if (displayData.p1CharID == CHAR_ID_MISHIO) {
                mishioLine << "P1 " << elemName(displayData.p1MishioElement) << "  Aw: " << displayData.p1MishioAwakenedTimer;
                if (displayData.infiniteMishioElement || displayData.infiniteMishioAwakened) mishioLine << " (Inf)";
            } else {
                mishioLine << "P1 -";
            }
            mishioLine << "  ";
            if (displayData.p2CharID == CHAR_ID_MISHIO) {
                mishioLine << "P2 " << elemName(displayData.p2MishioElement) << "  Aw: " << displayData.p2MishioAwakenedTimer;
                if (displayData.infiniteMishioElement || displayData.infiniteMishioAwakened) mishioLine << " (Inf)";
            } else {
                mishioLine << "P2 -";
            }
            upsert(g_statsMishioId, mishioLine.str());
        } else {
            if (g_statsMishioId != -1) { DirectDrawHook::RemovePermanentMessage(g_statsMishioId); g_statsMishioId = -1; }
        }

        // Character-specific: Rumi (Nanase) – Barehand, Shinai, Kimchi
        bool showRumi = (displayData.p1CharID == CHAR_ID_NANASE) || (displayData.p2CharID == CHAR_ID_NANASE);
        if (showRumi) {
            std::stringstream rumiLine;
            rumiLine << "Rumi: ";
            if (displayData.p1CharID == CHAR_ID_NANASE) {
                rumiLine << "P1 " << (displayData.p1RumiBarehanded ? "Bare" : "Shinai");
                rumiLine << "  Kimchi: " << (displayData.p1RumiKimchiActive ? "On" : "Off")
                         << " (" << displayData.p1RumiKimchiTimer << ")";
                if (displayData.p1RumiInfiniteKimchi || displayData.p1RumiInfiniteShinai) rumiLine << " (Inf)";
            } else {
                rumiLine << "P1 -";
            }
            rumiLine << "  ";
            if (displayData.p2CharID == CHAR_ID_NANASE) {
                rumiLine << "P2 " << (displayData.p2RumiBarehanded ? "Bare" : "Shinai");
                rumiLine << "  Kimchi: " << (displayData.p2RumiKimchiActive ? "On" : "Off")
                         << " (" << displayData.p2RumiKimchiTimer << ")";
                if (displayData.p2RumiInfiniteKimchi || displayData.p2RumiInfiniteShinai) rumiLine << " (Inf)";
            } else {
                rumiLine << "P2 -";
            }
            upsert(g_statsRumiId, rumiLine.str());
        } else {
            if (g_statsRumiId != -1) { DirectDrawHook::RemovePermanentMessage(g_statsRumiId); g_statsRumiId = -1; }
        }

        // Character-specific: Ikumi – Blood, Genocide, and Level Gauge (0..99)
        bool showIkumi = (displayData.p1CharID == CHAR_ID_IKUMI) || (displayData.p2CharID == CHAR_ID_IKUMI);
        if (showIkumi) {
            std::stringstream ikumiLine;
            ikumiLine << "Ikumi: ";
            if (displayData.p1CharID == CHAR_ID_IKUMI) {
                ikumiLine << "P1 Blood Value " << displayData.p1IkumiLevelGauge
                          << "  Lvl " << displayData.p1IkumiBlood
                          << "  Genocide Timer " << displayData.p1IkumiGenocide;
                if (displayData.infiniteBloodMode) ikumiLine << " (Inf)";
            } else {
                ikumiLine << "P1 -";
            }
            ikumiLine << "  ";
            if (displayData.p2CharID == CHAR_ID_IKUMI) {
                ikumiLine << "P2 Blood Value " << displayData.p2IkumiLevelGauge
                          << "  Lvl " << displayData.p2IkumiBlood
                          << "  Genocide Timer " << displayData.p2IkumiGenocide;
                if (displayData.infiniteBloodMode) ikumiLine << " (Inf)";
            } else {
                ikumiLine << "P2 -";
            }
            upsert(g_statsIkumiId, ikumiLine.str());
        } else {
            if (g_statsIkumiId != -1) { DirectDrawHook::RemovePermanentMessage(g_statsIkumiId); g_statsIkumiId = -1; }
        }

        // Character-specific: Mai – Status-derived timer display with Infinite markers
        bool showMai = (displayData.p1CharID == CHAR_ID_MAI) || (displayData.p2CharID == CHAR_ID_MAI);
        if (showMai) {
            auto statusName = [](int st){
                switch (st) {
                    case 1: return "Ghost";
                    case 2: return "Unsummon";
                    case 3: return "Charge";
                    case 4: return "Awakening";
                    default: return "Inactive";
                }
            };
            std::stringstream maiLine;
            maiLine << "Mai: ";
            if (displayData.p1CharID == CHAR_ID_MAI) {
                maiLine << "P1 " << statusName(displayData.p1MaiStatus) << " ";
                if (displayData.p1MaiStatus == 1) {
                    maiLine << displayData.p1MaiGhostTime;
                    if (displayData.p1MaiInfiniteGhost) maiLine << " (Inf)";
                } else if (displayData.p1MaiStatus == 3) {
                    maiLine << displayData.p1MaiGhostCharge;
                    if (displayData.p1MaiInfiniteCharge) maiLine << " (Inf)";
                    if (displayData.p1MaiNoChargeCD) maiLine << " [NoCD]";
                } else if (displayData.p1MaiStatus == 4) {
                    maiLine << displayData.p1MaiAwakeningTime;
                    if (displayData.p1MaiInfiniteAwakening) maiLine << " (Inf)";
                } else {
                    maiLine << "-";
                }
            } else {
                maiLine << "P1 -";
            }
            maiLine << "  ";
            if (displayData.p2CharID == CHAR_ID_MAI) {
                maiLine << "P2 " << statusName(displayData.p2MaiStatus) << " ";
                if (displayData.p2MaiStatus == 1) {
                    maiLine << displayData.p2MaiGhostTime;
                    if (displayData.p2MaiInfiniteGhost) maiLine << " (Inf)";
                } else if (displayData.p2MaiStatus == 3) {
                    maiLine << displayData.p2MaiGhostCharge;
                    if (displayData.p2MaiInfiniteCharge) maiLine << " (Inf)";
                    if (displayData.p2MaiNoChargeCD) maiLine << " [NoCD]";
                } else if (displayData.p2MaiStatus == 4) {
                    maiLine << displayData.p2MaiAwakeningTime;
                    if (displayData.p2MaiInfiniteAwakening) maiLine << " (Inf)";
                } else {
                    maiLine << "-";
                }
            } else {
                maiLine << "P2 -";
            }
            upsert(g_statsMaiId, maiLine.str());
        } else {
            if (g_statsMaiId != -1) { DirectDrawHook::RemovePermanentMessage(g_statsMaiId); g_statsMaiId = -1; }
        }

        // Character-specific: Minagi – Puppet (Michiru) coordinates and current entity id (sticky)
        bool showMinagi = (displayData.p1CharID == CHAR_ID_MINAGI) || (displayData.p2CharID == CHAR_ID_MINAGI);
        if (showMinagi) {
            auto scanPuppet = [&](int playerIndex, uintptr_t playerBase, double &outX, double &outY, int &outId, double &lastX, double &lastY) {
                // Default to prior sticky values
                outX = lastX;
                outY = lastY;
                outId = -1;
                if (!playerBase) return;
                for (int i = 0; i < MINAGI_PUPPET_SLOT_MAX_SCAN; ++i) {
                    uintptr_t slotBase = playerBase + MINAGI_PUPPET_SLOTS_BASE + static_cast<uintptr_t>(i) * MINAGI_PUPPET_SLOT_STRIDE;
                    uint16_t id = 0;
                    if (!SafeReadMemory(slotBase + MINAGI_PUPPET_SLOT_ID_OFFSET, &id, sizeof(id))) continue;
                    // Track any non-zero entity id in this slot range; prefer Michiru when present (both 400=unreadied, 401=readied)
                    if (id != 0) {
                        outId = id;
                        double x = 0.0, y = 0.0;
                        SafeReadMemory(slotBase + MINAGI_PUPPET_SLOT_X_OFFSET, &x, sizeof(x));
                        SafeReadMemory(slotBase + MINAGI_PUPPET_SLOT_Y_OFFSET, &y, sizeof(y));
                        // Update sticky coords only when a valid entity is present
                        lastX = x; lastY = y; outX = x; outY = y;
                        // Also capture frame/subframe for monitoring
                        uint16_t fr = 0, sub = 0;
                        SafeReadMemory(slotBase + MINAGI_PUPPET_SLOT_FRAME_OFFSET, &fr, sizeof(fr));
                        SafeReadMemory(slotBase + MINAGI_PUPPET_SLOT_SUBFRAME_OFFSET, &sub, sizeof(sub));
                        if (playerIndex == 1) { displayData.p1MichiruFrame = (int)fr; displayData.p1MichiruSubframe = (int)sub; }
                        else { displayData.p2MichiruFrame = (int)fr; displayData.p2MichiruSubframe = (int)sub; }
                        if (id == MINAGI_PUPPET_ENTITY_ID || id == 401) break; // Found Michiru (400 unreadied or 401 readied); stop early
                    }
                }
            };
            uintptr_t p1Base = ResolvePlayerBaseBestEffort(1, base);
            uintptr_t p2Base = ResolvePlayerBaseBestEffort(2, base);
            if (displayData.p1CharID == CHAR_ID_MINAGI) {
                scanPuppet(1, p1Base, displayData.p1MinagiPuppetX, displayData.p1MinagiPuppetY, displayData.p1MichiruCurrentId, displayData.p1MichiruLastX, displayData.p1MichiruLastY);
            } else { displayData.p1MichiruCurrentId = -1; }
            if (displayData.p2CharID == CHAR_ID_MINAGI) {
                scanPuppet(2, p2Base, displayData.p2MinagiPuppetX, displayData.p2MinagiPuppetY, displayData.p2MichiruCurrentId, displayData.p2MichiruLastX, displayData.p2MichiruLastY);
            } else { displayData.p2MichiruCurrentId = -1; }

            std::stringstream minagiLine; minagiLine.setf(std::ios::fixed); minagiLine << std::setprecision(2);
            minagiLine << "Michiru: ";
            auto fmtState = [](int fr, int sub){
                if (fr < 0) return std::string("");
                if (fr <= 1) return std::string(" [Ready]");
                std::stringstream s; s << " [Anim " << fr << ":" << sub << "]"; return s.str();
            };
            // Indicate when conversion gating is active for debugging visibility
            auto convActive = [&](short mv){ return displayData.minagiConvertNewProjectiles && mv >= 400 && mv <= 469; };
            if (displayData.p1CharID == CHAR_ID_MINAGI && !std::isnan(displayData.p1MichiruLastX)) {
                minagiLine << "P1 [" << displayData.p1MichiruLastX << ", " << displayData.p1MichiruLastY << "]";
                if (displayData.p1MichiruCurrentId > 0) minagiLine << " (ID " << displayData.p1MichiruCurrentId << ")";
                minagiLine << fmtState(displayData.p1MichiruFrame, displayData.p1MichiruSubframe);
                if (convActive(p1MoveId)) minagiLine << " {Conv}";
            } else {
                minagiLine << "P1 -";
            }
            minagiLine << "  ";
            if (displayData.p2CharID == CHAR_ID_MINAGI && !std::isnan(displayData.p2MichiruLastX)) {
                minagiLine << "P2 [" << displayData.p2MichiruLastX << ", " << displayData.p2MichiruLastY << "]";
                if (displayData.p2MichiruCurrentId > 0) minagiLine << " (ID " << displayData.p2MichiruCurrentId << ")";
                minagiLine << fmtState(displayData.p2MichiruFrame, displayData.p2MichiruSubframe);
                if (convActive(p2MoveId)) minagiLine << " {Conv}";
            } else {
                minagiLine << "P2 -";
            }
            upsert(g_statsMinagiId, minagiLine.str());
        } else {
            if (g_statsMinagiId != -1) { DirectDrawHook::RemovePermanentMessage(g_statsMinagiId); g_statsMinagiId = -1; }
        }

        // AI Control Flags line (moved from trigger overlay). Only show when stats are on and match phase.
        if (statsOn) {
            GamePhase phase = GetCurrentGamePhase();
            if (phase == GamePhase::Match) {
                uintptr_t p1BasePtr = 0, p2BasePtr = 0; uint32_t p1AI=0, p2AI=0; bool haveP1=false, haveP2=false;
                p1BasePtr = ResolvePlayerBaseBestEffort(1, base);
                p2BasePtr = ResolvePlayerBaseBestEffort(2, base);
                if (p1BasePtr) {
                    haveP1 = SafeReadMemory(p1BasePtr + AI_CONTROL_FLAG_OFFSET, &p1AI, sizeof(p1AI));
                }
                if (p2BasePtr) {
                    haveP2 = SafeReadMemory(p2BasePtr + AI_CONTROL_FLAG_OFFSET, &p2AI, sizeof(p2AI));
                }
                std::stringstream aiLine;
                aiLine << "AI: P1=" << (haveP1 ? (p1AI?"1":"0") : "-") << " P2=" << (haveP2 ? (p2AI?"1":"0") : "-");
                // Indicate when P2 control is temporarily overridden by automation
                if (g_p2ControlOverridden) aiLine << " [Override]";
                upsert(g_statsAIFlagsId, aiLine.str());
            } else {
                if (g_statsAIFlagsId != -1) { DirectDrawHook::RemovePermanentMessage(g_statsAIFlagsId); g_statsAIFlagsId = -1; }
            }
        }
    else if (g_statsAIFlagsId != -1) { DirectDrawHook::RemovePermanentMessage(g_statsAIFlagsId); g_statsAIFlagsId = -1; }
    if (!statsOn && g_statsBlockstunId != -1) { DirectDrawHook::RemovePermanentMessage(g_statsBlockstunId); g_statsBlockstunId = -1; }
    if (!statsOn && g_statsUntechId != -1) { DirectDrawHook::RemovePermanentMessage(g_statsUntechId); g_statsUntechId = -1; }
    }

    if (statsOn) {
        upsert(g_statsMoveIdId, moveIds.str());
    }

    return;
}
