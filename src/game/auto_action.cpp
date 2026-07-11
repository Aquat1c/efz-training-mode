#include "../include/game/auto_action.h"
#include "../include/core/constants.h"
#include "../include/utils/utilities.h"
#include "../include/utils/network.h"
#include "../include/game/game_state.h"
#include "../include/core/memory.h"
#include "../include/core/logger.h"
#include "../include/input/input_core.h"        
#include "../include/input/motion_system.h"     
#include "../include/game/auto_action_helpers.h"
#include "../include/input/motion_constants.h"  
#include "../include/input/input_motion.h"      
#include "../include/input/input_freeze.h"
#include "../include/input/input_buffer.h"     // For g_activeFreezePlayer owner check
#include "../include/game/attack_reader.h"
#include "../include/input/immediate_input.h"
#include "../include/game/fm_commands.h" // Final Memory execution
#include "../include/game/macro_controller.h" // Integrate macros with triggers
#include "../include/game/mission/mission_engine.h"
#include "../include/game/character_settings.h" // For authoritative character ID mapping
#include "../include/game/frame_analysis.h" // For IsThrown/IsHitstun/IsLaunched helpers
#include "../include/game/per_frame_sample.h" // Unified per-frame sample accessor
#include "../include/game/validation_metrics.h" // Validation metrics instrumentation
#include "../include/game/validation_metrics.h" // Validation metrics instrumentation
#include "../include/gui/overlay.h" // For DirectDrawHook::AddMessage
#include "../include/utils/switch_players.h"

// Safety forward declarations (in case of include-order differences in some build phases)
bool IsThrown(short moveID);
// Forward declarations for row selection helpers used in StartTriggerDelay
static inline bool HasEnabledRows(int triggerType);
static inline bool PickRandomRow(int triggerType, TriggerOption &out);
static inline bool PickRandomPoolOption(int triggerType, int playerNum, TriggerOption &out);
#include <cmath>
#include <algorithm>
#include <vector>
#include <sstream>

#ifndef EFZ_ENABLE_AUTO_ACTION_WAKE_TRACE
#define EFZ_ENABLE_AUTO_ACTION_WAKE_TRACE 0
#endif

// Define the motion input constants if they're not already defined
#ifndef MOTION_INPUT_UP
#define MOTION_INPUT_UP INPUT_UP
#endif

#ifndef MOTION_INPUT_LEFT
#define MOTION_INPUT_LEFT INPUT_LEFT
#endif

#ifndef ACTION_BACK_DASH
#define ACTION_BACK_DASH ACTION_BACKDASH
#endif

// Forward declaration for function used within this file
void ApplyAutoAction(int playerNum, uintptr_t moveIDAddr, short currentMoveID, short prevMoveID);
static int ResolveButtonMaskForAction(int actionType, int triggerType, int strengthOverride = -1);
static bool ExecuteWakeSpecialNow(int playerNum, int actionType, short currentMoveID, int strengthOverride = -1);
static bool SupportsImmediateWakeHold(int actionType);
static bool BuildImmediateWakeHoldMask(int playerNum, int actionType, uint8_t &outMask);
static bool IssueWakeImmediateHold(int playerNum, int actionType);
static constexpr int kInternalTicksPerVisualFrame = 3;

static inline int VisualFramesToInternalTicks(int visualFrames) {
    return (visualFrames <= 0) ? 0 : (visualFrames * kInternalTicksPerVisualFrame);
}

static inline int InternalTicksToVisualFramesCeil(int internalTicks) {
    return (internalTicks <= 0) ? 0 : ((internalTicks + kInternalTicksPerVisualFrame - 1) / kInternalTicksPerVisualFrame);
}

// Wake jump tracking for debugging
static int s_p1WakeJumpTrackFrame = -1;
static int s_p2WakeJumpTrackFrame = -1;

// Helper function: Check if moveID represents an action that could be buffered after wake
// (attacks, walks, dashes, jumps, etc.)
static inline bool IsBufferableAction(short moveID) {
    // Attacks (normals, specials, supers)
    if (IsAttackMove(moveID)) return true;
    
    // Walks
    if (moveID == WALK_FWD_ID || moveID == WALK_BACK_ID) return true;
    
    // Dashes
    if (moveID == FORWARD_DASH_START_ID || moveID == FORWARD_DASH_RECOVERY_ID ||
        moveID == FORWARD_DASH_RECOVERY_SENTINEL_ID ||
        moveID == BACKWARD_DASH_START_ID || moveID == BACKWARD_DASH_RECOVERY_ID ||
        moveID == KAORI_FORWARD_DASH_START_ID) {
        return true;
    }
    
    // Jumps
    if (moveID == STRAIGHT_JUMP_ID || moveID == FORWARD_JUMP_ID || moveID == BACKWARD_JUMP_ID) {
        return true;
    }
    
    // Crouch (can be buffered after wake)
    if (moveID == CROUCH_ID) return true;
    
    return false;
}

// Wakeup timing table: rising frames(moveid 96) (visual @ 64Hz) for each character
// Multiply by 3 to get logical frames (192Hz ticks)
//Taken out straight from https://mizuumi.wiki/w/Eternal_Fighter_Zero/Advanced_Mechanics#Wakeup_Time
struct WakeupTiming {
    int charID;
    int risingFramesVisual;  // Visual frames at 64Hz
};

static const WakeupTiming s_wakeupTimings[] = {
    { CHAR_ID_MIZUKA,    13 }, // Runtime "nagamori" resolves to this ID
    { CHAR_ID_NAGAMORI,  13 }, 
    { CHAR_ID_MAKOTO,    16 },
    { CHAR_ID_MINAGI,    16 },
    { CHAR_ID_EXNANASE,  16 }, 
    { CHAR_ID_NANASE,    16 }, 
    { CHAR_ID_AKANE,     17 },
    { CHAR_ID_NAYUKI,    17 }, 
    { CHAR_ID_MISUZU,    19 },
    { CHAR_ID_MIZUKAB,   19 },
    { CHAR_ID_IKUMI,     20 },
    { CHAR_ID_KAORI,     20 },
    { CHAR_ID_SAYURI,    20 },
    { CHAR_ID_SHIORI,    20 },
    { CHAR_ID_AYU,       21 },
    { CHAR_ID_MISAKI,    21 },
    { CHAR_ID_MISHIO,    21 },
    { CHAR_ID_MAI,       23 },  
    { CHAR_ID_AKIKO,     26 },
    { CHAR_ID_MIO,       26 },  
    { CHAR_ID_NAYUKIB,   27 },  
    { CHAR_ID_KANO,      29 },
    { CHAR_ID_KANNA,     31 },
    { CHAR_ID_MAYU,      39 }, 
};

// Get rising frame count for character (returns visual frames at 64Hz)
static int GetWakeupRisingFrames(int charID) {
    for (const auto& timing : s_wakeupTimings) {
        if (timing.charID == charID) {
            return timing.risingFramesVisual;  // Visual frames at 64Hz
        }
    }
    // Default fallback: assume mid-range wakeup (20 visual frames)
    return 20;
}

// Get rising frame count for character (returns logical ticks at 192Hz)
static int GetWakeupRisingTicks(int charID) {
    for (const auto& timing : s_wakeupTimings) {
        if (timing.charID == charID) {
            return VisualFramesToInternalTicks(timing.risingFramesVisual);
        }
    }
    // Default fallback: assume mid-range wakeup (20 visual frames × 3 = 60 ticks)
    return VisualFramesToInternalTicks(20);
}

// Initialize delay states
TriggerDelayState p1DelayState = {false, 0, TRIGGER_NONE, 0, -1, -1, 0, -1, -1};
TriggerDelayState p2DelayState = {false, 0, TRIGGER_NONE, 0, -1, -1, 0, -1, -1};
bool p1ActionApplied = false;
bool p2ActionApplied = false;

// Definitions for trigger tracking globals
std::atomic<int> g_lastActiveTriggerType(TRIGGER_NONE);
std::atomic<int> g_lastActiveTriggerFrame(0);

// Globals to track trigger cancelled state (flashes red on cancel)
std::atomic<bool> g_triggersCancelledActive(false);
std::atomic<int> g_triggersCancelledFrame(0);

struct AutoActionLogScope {
    AutoActionLogScope(const char* phaseLabel, int playerNum, int triggerType)
        : player(playerNum), trigger(triggerType), phase(phaseLabel ? phaseLabel : "AutoAction") {
        LogOut("=================", true);
        LogOut("[AUTO-ACTION] " + phase + " begin (player=" + std::to_string(player) +
               ", trigger=" + std::to_string(trigger) + ")", true);
    }
    ~AutoActionLogScope() {
        LogOut("[AUTO-ACTION] " + phase + " end (player=" + std::to_string(player) +
               ", trigger=" + std::to_string(trigger) + ")", true);
        LogOut("=================", true);
    }
    int player;
    int trigger;
    std::string phase;
};

struct TriggerFlowTracker {
    bool active = false;
    int player = 0;
    int startFrame = 0;
    short startMoveID = -1;
    short lastMoveID = -1;
    const char* reason = "Unknown";
};

static TriggerFlowTracker g_flowTrackers[3];

static inline TriggerFlowTracker& GetFlowTracker(int playerNum) {
    return g_flowTrackers[(playerNum == 2) ? 2 : 1];
}

static const char* TriggerTypeLabel(int triggerType) {
    switch (triggerType) {
        case TRIGGER_AFTER_BLOCK: return "AfterBlock";
        case TRIGGER_ON_WAKEUP: return "OnWakeup";
        case TRIGGER_AFTER_HITSTUN: return "AfterHitstun";
        case TRIGGER_AFTER_AIRTECH: return "AfterAirtech";
        case TRIGGER_ON_RG: return "OnRG";
        default: return "Unknown";
    }
}

static void BeginFlowSequence(int playerNum, const char* reason, short prevMoveID, short currMoveID) {
    TriggerFlowTracker& tracker = GetFlowTracker(playerNum);
    if (tracker.active) return;
    tracker.active = true;
    tracker.player = playerNum;
    tracker.startFrame = frameCounter.load();
    tracker.startMoveID = currMoveID;
    tracker.lastMoveID = currMoveID;
    tracker.reason = reason ? reason : "Unknown";
    (void)prevMoveID;
}

static void LogFlowSequenceEvent(int playerNum, const char* label, short prevMoveID, short currMoveID) {
    TriggerFlowTracker& tracker = GetFlowTracker(playerNum);
    if (!tracker.active) return;
    tracker.lastMoveID = currMoveID;
    (void)label;
    (void)prevMoveID;
}

static void EndFlowSequence(int playerNum, const char* reason, short currMoveID) {
    TriggerFlowTracker& tracker = GetFlowTracker(playerNum);
    if (!tracker.active) return;
    (void)reason;
    (void)currMoveID;
    tracker.active = false;
    tracker.reason = "Unknown";
    tracker.startFrame = 0;
    tracker.startMoveID = -1;
    tracker.lastMoveID = -1;
}

static void MaybeBeginFlowSequence(int playerNum, short prevMoveID, short currMoveID) {
    TriggerFlowTracker& tracker = GetFlowTracker(playerNum);
    if (tracker.active) return;
    bool enterGround = !IsGroundtech(prevMoveID) && IsGroundtech(currMoveID);
    bool enterLaunch = !IsLaunched(prevMoveID) && IsLaunched(currMoveID);
    bool enterAirtech = !IsAirtech(prevMoveID) && IsAirtech(currMoveID);
    const char* reason = nullptr;
    if (enterGround) reason = "EnteredGroundtech";
    else if (enterLaunch) reason = "Launched";
    else if (enterAirtech) reason = "Airtech";
    if (reason) {
        BeginFlowSequence(playerNum, reason, prevMoveID, currMoveID);
    }
}

static void MaybeAutoCloseFlowSequence(int playerNum, short prevMoveID, short currMoveID, bool wakePrearmed, bool restorePending) {
    TriggerFlowTracker& tracker = GetFlowTracker(playerNum);
    if (!tracker.active) return;
    bool stillInRecovery = IsGroundtech(currMoveID) || IsAirtech(currMoveID) || IsLaunched(currMoveID);
    if (!stillInRecovery && !restorePending && !wakePrearmed) {
        EndFlowSequence(playerNum, "SequenceAutoClosed", currMoveID);
    } else if (tracker.lastMoveID != currMoveID) {
        tracker.lastMoveID = currMoveID;
    }
    (void)prevMoveID;
}

static bool p1TriggerActive = false;
static bool p2TriggerActive = false;
// Log throttles to avoid spamming the console during tight loops
// (Removed per-trigger throttles for P2 After Block in refactor)
static int p1TriggerCooldown = 0;
static int p2TriggerCooldown = 0;
// Further reduce cooldown to re-arm triggers faster (was 6)
static constexpr int TRIGGER_COOLDOWN_FRAMES = 6; // modest base cooldown; simplified logic allows reliable expiry
static constexpr int RESTORE_GRACE_PERIOD = 0; // grace disabled: neutralization now prevents immediate re-trigger
static int g_restoreGraceCounter = 0; // counts down from RESTORE_GRACE_PERIOD after control restore
bool g_p2ControlOverridden = false;
uint32_t g_originalP2ControlFlag = 1; // Default to AI control

// ---------------------------------------------------------------------------
// Forward dash follow-up deferral (moved out of ApplyAutoAction so we can fire
// on the exact frame the dash window is satisfied, even if no new trigger fires)
// mode: 0 = post-dash (inject first frame AFTER queue inactive)
//       1 = dash-normal timing (inject first frame queue becomes active)
// stage (internal): for post-dash, stage advances to 1 once we have seen the
//                   queue active at least one frame; then we wait for inactive.
//                   for dash-normal, stage remains 0 until we fire while active.
struct DashFollowDeferred {
    // We only need: which follow-up (1..6), which player, and whether we've latched dash start.
    std::atomic<int> pendingSel{0};
    std::atomic<int> player{0};
    std::atomic<int> dashStartLatched{0}; // 0 = not yet, 1 = latched & fired, -1 = latched but failed to fire (safety)
    std::atomic<int> isBack{0};           // 0 = forward dash, 1 = backdash
};
static DashFollowDeferred g_dashDeferred; // single slot (only one forward dash follow-up expected at a time)

// ---------------------------------------------------------------------------
// Character ID caching for auto-action logic
// Cached once per MATCH phase; invalidated when leaving match / clearing triggers
// Sentinel value -999 = not yet cached; -1 = cached but unknown
// ---------------------------------------------------------------------------
static int g_cachedP1CharID = -999;
static int g_cachedP2CharID = -999;
static bool g_loggedCharInvalidation = false;
static uint32_t g_characterCacheGeneration = 0;

static void InvalidateCachedCharacterIDs(const char* reason) {
    const uint32_t generation = GetRuntimeLifecycleGeneration();
    if ((g_cachedP1CharID != -999 || g_cachedP2CharID != -999) && !g_loggedCharInvalidation) {
        LogOut(std::string("[AUTO-ACTION][CHAR] Invalidating cached character IDs gen=")
            + std::to_string(generation) + " (" + reason + ")", detailedLogging.load());
        g_loggedCharInvalidation = true;
    }
    g_cachedP1CharID = -999;
    g_cachedP2CharID = -999;
    g_characterCacheGeneration = generation;
}

void InvalidateAutoActionCharacterCaches(const char* reason) {
    InvalidateCachedCharacterIDs(reason);
}

static void EnsureCachedCharacterIDs(uintptr_t base) {
    const uint32_t lifecycleGeneration = GetRuntimeLifecycleGeneration();
    if (g_characterCacheGeneration != lifecycleGeneration) {
        const bool hadCachedValues = (g_cachedP1CharID != -999 || g_cachedP2CharID != -999);
        if (hadCachedValues || detailedLogging.load()) {
            LogOut(std::string("[AUTO-ACTION][CHAR] Lifecycle generation changed ")
                + std::to_string(g_characterCacheGeneration) + "->" + std::to_string(lifecycleGeneration),
                true);
        }
        InvalidateCachedCharacterIDs("lifecycle generation change");
    }

    if (g_cachedP1CharID == -999) {
        char nameBuf1[16] = {0};
        uintptr_t nameAddr1 = ResolvePointer(base, EFZ_BASE_OFFSET_P1, CHARACTER_NAME_OFFSET);
        if (nameAddr1) SafeReadMemory(nameAddr1, &nameBuf1, sizeof(nameBuf1)-1);
        g_cachedP1CharID = CharacterSettings::GetCharacterID(std::string(nameBuf1));
        if (g_cachedP1CharID < 0) g_cachedP1CharID = -1;
        g_characterCacheGeneration = lifecycleGeneration;
        LogOut(std::string("[AUTO-ACTION][CHAR] Cached P1 gen=") + std::to_string(g_characterCacheGeneration)
            + " char name='" + nameBuf1 + "' id=" + std::to_string(g_cachedP1CharID), detailedLogging.load());
        g_loggedCharInvalidation = false;
    }
    if (g_cachedP2CharID == -999) {
        char nameBuf2[16] = {0};
        uintptr_t nameAddr2 = ResolvePointer(base, EFZ_BASE_OFFSET_P2, CHARACTER_NAME_OFFSET);
        if (nameAddr2) SafeReadMemory(nameAddr2, &nameBuf2, sizeof(nameBuf2)-1);
        g_cachedP2CharID = CharacterSettings::GetCharacterID(std::string(nameBuf2));
        if (g_cachedP2CharID < 0) g_cachedP2CharID = -1;
        g_characterCacheGeneration = lifecycleGeneration;
        LogOut(std::string("[AUTO-ACTION][CHAR] Cached P2 gen=") + std::to_string(g_characterCacheGeneration)
            + " char name='" + nameBuf2 + "' id=" + std::to_string(g_cachedP2CharID), detailedLogging.load());
        g_loggedCharInvalidation = false;
    }
}

static void ScheduleForwardDashFollowup(int playerNum, int sel, bool /*dashModeIgnored*/) {
    if (g_dashDeferred.pendingSel.load() > 0) {
        LogOut(std::string("[AUTO-ACTION] Forward dash follow-up schedule ignored; pending sel=") + std::to_string(g_dashDeferred.pendingSel.load()), true);
        return;
    }
    g_dashDeferred.pendingSel.store(sel);
    g_dashDeferred.player.store(playerNum);
    g_dashDeferred.dashStartLatched.store(0);
    g_dashDeferred.isBack.store(0);
    LogOut(std::string("[AUTO-ACTION] Forward dash follow-up armed (sel=") + std::to_string(sel) + ")", true);
    // Note: Do NOT arm control-restore here. The actual dash action path will
    // set up override/restore timing when the dash is queued successfully.
}

// Mirror of forward follow-up: schedule for backdash
static void ScheduleBackDashFollowup(int playerNum, int sel) {
    if (g_dashDeferred.pendingSel.load() > 0) {
        LogOut(std::string("[AUTO-ACTION] Backdash follow-up schedule ignored; pending sel=") + std::to_string(g_dashDeferred.pendingSel.load()), true);
        return;
    }
    g_dashDeferred.pendingSel.store(sel);
    g_dashDeferred.player.store(playerNum);
    g_dashDeferred.dashStartLatched.store(0);
    g_dashDeferred.isBack.store(1);
    LogOut(std::string("[AUTO-ACTION] Backdash follow-up armed (sel=") + std::to_string(sel) + ")", true);
    // Note: Do NOT arm control-restore here. The actual backdash action path will
    // set up override/restore timing when the dash is queued successfully.
}

// Refactored: use authoritative moveID transitions (163->164) instead of queue heuristics.
// current/prev move IDs for both players are passed each frame for precise detection.
static void ProcessDeferredDashFollowups(short curP1, short prevP1, short curP2, short prevP2) {
    int sel = g_dashDeferred.pendingSel.load();
    if (sel <= 0) return;
    int targetP = g_dashDeferred.player.load();
    short prev = (targetP == 1) ? prevP1 : prevP2;
    short curr = (targetP == 1) ? curP1  : curP2;
    bool back = (g_dashDeferred.isBack.load() != 0);
    
    // Debug: Log MoveID transitions while waiting for dash
    static short lastLoggedP1 = -999;
    static short lastLoggedP2 = -999;
        if (targetP == 1 && curr != lastLoggedP1) {
        bool targIsKaori = (g_cachedP1CharID == CHAR_ID_KAORI);
        int waitId = back ? BACKWARD_DASH_START_ID : (targIsKaori ? KAORI_FORWARD_DASH_START_ID : FORWARD_DASH_START_ID);
            if (detailedLogging.load()) {
                LogOut("[DASH_DEBUG] P1 MoveID: " + std::to_string(prev) + " -> " + std::to_string(curr) + 
                       std::string(" (waiting for ") + std::to_string(waitId) + ")", true);
            }
        lastLoggedP1 = curr;
            if ((back && curr == BACKWARD_DASH_START_ID) || (!back && (curr == FORWARD_DASH_START_ID || (targIsKaori && curr == KAORI_FORWARD_DASH_START_ID)))) {
                if (detailedLogging.load()) {
                    DumpInputBuffer(1, "DASH_START_DETECTED");
                }
        }
    }
        if (targetP == 2 && curr != lastLoggedP2) {
        bool targIsKaori = (g_cachedP2CharID == CHAR_ID_KAORI);
        int waitId = back ? BACKWARD_DASH_START_ID : (targIsKaori ? KAORI_FORWARD_DASH_START_ID : FORWARD_DASH_START_ID);
            if (detailedLogging.load()) {
                LogOut("[DASH_DEBUG] P2 MoveID: " + std::to_string(prev) + " -> " + std::to_string(curr) + 
                       std::string(" (waiting for ") + std::to_string(waitId) + ")", true);
            }
        lastLoggedP2 = curr;
            if ((back && curr == BACKWARD_DASH_START_ID) || (!back && (curr == FORWARD_DASH_START_ID || (targIsKaori && curr == KAORI_FORWARD_DASH_START_ID)))) {
                if (detailedLogging.load()) {
                    DumpInputBuffer(2, "DASH_START_DETECTED");
                }
        }
    }
    
    // Fire exactly on first frame we see the dash start ID (forward or back).
    bool targIsKaori = (targetP == 1 ? (g_cachedP1CharID == CHAR_ID_KAORI) : (g_cachedP2CharID == CHAR_ID_KAORI));
    bool forwardDashStart = (!back && (curr == FORWARD_DASH_START_ID || (targIsKaori && curr == KAORI_FORWARD_DASH_START_ID)));
    bool backDashStart = (back && curr == BACKWARD_DASH_START_ID);
    if ((forwardDashStart && prev != curr) || (backDashStart && prev != BACKWARD_DASH_START_ID)) {
        bool facingRight = GetPlayerFacingDirection(targetP);
        uint8_t forwardDir = facingRight ? GAME_INPUT_RIGHT : GAME_INPUT_LEFT;
        uint8_t backDir    = facingRight ? GAME_INPUT_LEFT  : GAME_INPUT_RIGHT;
        uint8_t mask = 0;
        switch (sel) {
            case 1: mask = (back ? backDir : forwardDir) | GAME_INPUT_A; break; // dash A/B/C
            case 2: mask = (back ? backDir : forwardDir) | GAME_INPUT_B; break;
            case 3: mask = (back ? backDir : forwardDir) | GAME_INPUT_C; break;
            case 4: mask = ((back ? backDir : forwardDir) | GAME_INPUT_DOWN) | GAME_INPUT_A; break; // dash 2A/2B/2C
            case 5: mask = ((back ? backDir : forwardDir) | GAME_INPUT_DOWN) | GAME_INPUT_B; break;
            case 6: mask = ((back ? backDir : forwardDir) | GAME_INPUT_DOWN) | GAME_INPUT_C; break;
            default: break;
        }
        if (mask) {
            ImmediateInput::PressFor(targetP, mask, 2);
            LogOut(std::string("[AUTO-ACTION] Dash-normal injected sel=") + std::to_string(sel) + " frame=0 of dash", true);
            
            // CRITICAL: Clear the dash pattern from buffer immediately after injecting the normal.
            // Otherwise the dash pattern (L,L,L,N,N,L,L,L) remains in buffer and re-triggers after attack ends.
            ClearPlayerInputBuffer(targetP, true);
            WritePlayerInputImmediate(targetP, 0x00);
            LogOut("[AUTO-ACTION] Cleared dash pattern from buffer after dash normal injection", true);
            
            g_dashDeferred.pendingSel.store(0);
            g_dashDeferred.dashStartLatched.store(1);
            if (targetP == 2) { if (p2TriggerCooldown < 6) p2TriggerCooldown = 6; }
            else { if (p1TriggerCooldown < 6) p1TriggerCooldown = 6; }
        } else {
            // Safety: if something invalid, clear so we don't spin forever.
            g_dashDeferred.pendingSel.store(0);
            g_dashDeferred.dashStartLatched.store(-1);
            LogOut("[AUTO-ACTION] Dash follow-up selection invalid, clearing", true);
        }
    }
}

// Add at the top with other global variables (around line 40)
std::atomic<bool> g_pendingControlRestore(false);
std::atomic<short> g_lastP2MoveID(-1);
// Counter-RG fast restore: when active, restore AI control as soon as the special actually starts
std::atomic<bool> g_crgFastRestore(false);
// Forward dash tracking to prevent premature restore before MoveID 163 appears
static std::atomic<bool> g_recentDashQueued(false);
static std::atomic<int> g_recentDashQueuedFrame(0);
// Grace period: prevent fallback restore from triggering immediately after freeze starts
static std::atomic<DWORD> g_pendingRestoreTimestamp(0);

// When true, the next macro-driven P2 control restore should preserve
// the input buffer and motion token (used for wake pre-buffered macros).
std::atomic<bool> g_macroWakePreserveBuffer(false);

// Set when a wake-prebuffered macro's playback (including finish-guard)
// has fully completed. Auto-action uses this to schedule monitoring for
// successful buffered move execution, then cleans up the motion token
// after the buffered move executes (similar to regular macros).
std::atomic<bool> g_wakeMacroPlaybackCompleted(false);
static int s_p2WakeMacroTokenNeutralizeCountdown = 0;
static short s_p2LastWakeMoveID = -1; // Track moveID transitions for cleanup
static bool s_p2WakeMoveIDChanged = false; // unused but kept for compatibility

// Pre-arm flags for On Wakeup: buffer inputs during GROUNDTECH_RECOVERY to fire on wake
static bool s_p1WakePrearmed = false; // wake pre-arm without early triggerActive
static bool s_p2WakePrearmed = false; // same
static int  s_p1WakePrearmExpiry = 0;
static int  s_p2WakePrearmExpiry = 0;
// Wake pre-arm action metadata (so we can execute normals/dashes at the actionable frame)
static int  s_p1WakePrearmActionType = -1;
static int  s_p2WakePrearmActionType = -1;
static int  s_p1WakePrearmStrength  = -1; // row/button strength chosen for wake pre-arm
static int  s_p2WakePrearmStrength  = -1;
static bool s_p1WakePrearmIsSpecial = false; // specials rely on buffer freeze; normals/dashes injected later
static bool s_p2WakePrearmIsSpecial = false;
static bool s_p1WakePrearmIsMacro = false; // macro wake has its own timing logic, not treated as special
static bool s_p2WakePrearmIsMacro = false;
static bool s_p1WakeActionableWarning = false;
static bool s_p2WakeActionableWarning = false;
// Wake special early buffer: count logical frames (192Hz) in moveID 96
// Buffer on last rising frame before actionable (character-specific timing)
static int s_p1WakeMoveID96FrameCount = 0;
static int s_p2WakeMoveID96FrameCount = 0;
static bool s_p1WakeBufferFrozen = false;
static bool s_p2WakeBufferFrozen = false;
static bool s_p1WakeHoldPrimed = false;
static bool s_p2WakeHoldPrimed = false;
static bool s_p1WakeHoldIssued = false;
static bool s_p2WakeHoldIssued = false;
// Macro-specific wake tracking: late pre-buffer window for wakeup macros
static int  s_p1WakeMacro96FrameCount = 0;
static bool s_p1WakeMacroQueued       = false;
static int  s_p1WakeMacroTargetFrame  = -1;
static int  s_p1WakeMacroSlot         = -1;
static int  s_p1WakeMacroStartTick    = 0;   // Tick offset to skip to for wake macros
static int  s_p2WakeMacro96FrameCount = 0;
static bool s_p2WakeMacroQueued       = false;
// Pre-picked option for this wake sequence (row selection done early in moveID 96)
static TriggerOption s_p1WakePrePickedOption = {};
static TriggerOption s_p2WakePrePickedOption = {};
static int  s_p2WakeMacroTargetFrame  = -1;
static int  s_p2WakeMacroSlot         = -1;
static int  s_p2WakeMacroStartFrame   = -1;
static int  s_p2WakeMacroStartTick    = 0;   // Tick offset to skip to for wake macros
static bool s_p1WakeOptionPicked = false;
static bool s_p2WakeOptionPicked = false;

#if EFZ_ENABLE_AUTO_ACTION_WAKE_TRACE
static const char* TraceBool(bool value) {
    return value ? "1" : "0";
}

static const char* TraceSubframeSuffix(int internalFrame) {
    static const char* kSuffixes[3] = { "00", "33", "66" };
    int subframe = internalFrame % 3;
    if (subframe < 0) subframe += 3;
    return kSuffixes[subframe];
}

static const char* TraceMoveClass(short moveID) {
    switch (moveID) {
        case IDLE_MOVE_ID: return "idle";
        case WALK_FWD_ID: return "walk-f";
        case WALK_BACK_ID: return "walk-b";
        case CROUCH_ID: return "crouch";
        case CROUCH_TO_STAND_ID: return "crouch-stand";
        case GROUNDTECH_PRE: return "gtech-pre";
        case GROUNDTECH_RECOVERY: return "gtech-96";
        case GROUNDTECH_START: return "gtech-start";
        case GROUNDTECH_END: return "gtech-end";
        case STRAIGHT_JUMP_ID: return "jump-n";
        case FORWARD_JUMP_ID: return "jump-f";
        case BACKWARD_JUMP_ID: return "jump-b";
        case FALLING_ID: return "fall";
        case FORWARD_DASH_START_ID: return "dash-f-start";
        case FORWARD_DASH_RECOVERY_ID: return "dash-f-rec";
        case FORWARD_DASH_RECOVERY_SENTINEL_ID: return "dash-f-sentinel";
        case BACKWARD_DASH_START_ID: return "dash-b-start";
        case BACKWARD_DASH_RECOVERY_ID: return "dash-b-rec";
        case KAORI_FORWARD_DASH_START_ID: return "kaori-dash-f";
        case STAND_GUARD_ID: return "stand-guard";
        case CROUCH_GUARD_ID: return "crouch-guard";
        case AIR_GUARD_ID: return "air-guard";
        case RG_STAND_ID: return "rg-stand";
        case RG_CROUCH_ID: return "rg-crouch";
        case RG_AIR_ID: return "rg-air";
        default: break;
    }
    if (IsHitstun(moveID)) return "hitstun";
    if (IsLaunched(moveID)) return "launch";
    if (IsAirtech(moveID)) return "airtech";
    if (IsThrown(moveID)) return "throw";
    if (IsFrozen(moveID)) return "frozen";
    if (IsAttackMove(moveID)) return "attack";
    if (IsBlockstun(moveID)) return "blockstun";
    if (IsActionable(moveID)) return "actionable";
    return "other";
}

static bool TraceWakeRelevantForPlayer(int playerNum, short prevMoveID, short moveID) {
    if (IsGroundtech(prevMoveID) || IsGroundtech(moveID)) return true;
    if (playerNum == 1) {
        return s_p1WakePrearmed || s_p1WakeMacroQueued || p1DelayState.triggerType == TRIGGER_ON_WAKEUP ||
               s_p1WakeMoveID96FrameCount > 0 || s_p1WakeMacro96FrameCount > 0;
    }
    return s_p2WakePrearmed || s_p2WakeMacroQueued || p2DelayState.triggerType == TRIGGER_ON_WAKEUP ||
           s_p2WakeMoveID96FrameCount > 0 || s_p2WakeMacro96FrameCount > 0 ||
           s_p2WakeMacroTokenNeutralizeCountdown > 0;
}

static bool TraceWakeWindowActive(short moveID1, short moveID2, short prevMoveID1, short prevMoveID2) {
    static int s_traceWindow[3] = { 0, 0, 0 };
    static int s_lastUpdatedFrame = -1;
    const int now = frameCounter.load();

    if (s_lastUpdatedFrame != now) {
        s_lastUpdatedFrame = now;
        const bool p1Relevant = TraceWakeRelevantForPlayer(1, prevMoveID1, moveID1);
        const bool p2Relevant = TraceWakeRelevantForPlayer(2, prevMoveID2, moveID2);
        if (p1Relevant) {
            s_traceWindow[1] = 90;
        } else if (s_traceWindow[1] > 0) {
            --s_traceWindow[1];
        }
        if (p2Relevant) {
            s_traceWindow[2] = 90;
        } else if (s_traceWindow[2] > 0) {
            --s_traceWindow[2];
        }
    }

    return s_traceWindow[1] > 0 || s_traceWindow[2] > 0;
}

static void TraceAppendBufferSnapshot(std::ostringstream& oss, int playerNum) {
    uintptr_t playerPtr = GetPlayerPointer(playerNum);
    if (!playerPtr) {
        oss << " p" << playerNum << "Buf=null";
        return;
    }

    uint16_t head = 0;
    uint8_t last = 0;
    bool haveHead = SafeReadMemory(playerPtr + INPUT_BUFFER_INDEX_OFFSET, &head, sizeof(head));
    if (haveHead) {
        const uint16_t lastIndex = (head > 0) ? static_cast<uint16_t>(head - 1) : 0;
        (void)SafeReadMemory(playerPtr + INPUT_BUFFER_OFFSET + lastIndex, &last, sizeof(last));
        oss << " p" << playerNum << "BufHead=" << head
            << " p" << playerNum << "BufLast=" << static_cast<int>(last);
    } else {
        oss << " p" << playerNum << "BufHead=read-fail";
    }
}

static void TraceAppendPlayerWakeState(std::ostringstream& oss, int playerNum, short prevMoveID, short moveID) {
    const bool actionable = IsActionable(moveID);
    const bool groundtech = IsGroundtech(moveID);
    oss << " p" << playerNum << "=" << prevMoveID << "->" << moveID
        << "(" << TraceMoveClass(prevMoveID) << "->" << TraceMoveClass(moveID) << ")"
        << " act=" << TraceBool(actionable)
        << " gtech=" << TraceBool(groundtech);

    if (playerNum == 1) {
        oss << " pre=" << TraceBool(s_p1WakePrearmed)
            << " spec=" << TraceBool(s_p1WakePrearmIsSpecial)
            << " macro=" << TraceBool(s_p1WakePrearmIsMacro)
            << " action=" << s_p1WakePrearmActionType
            << " str=" << s_p1WakePrearmStrength
            << " c96=" << s_p1WakeMoveID96FrameCount
            << " m96=" << s_p1WakeMacro96FrameCount
            << " mQ=" << TraceBool(s_p1WakeMacroQueued)
            << " mTarget=" << s_p1WakeMacroTargetFrame
            << " frozen=" << TraceBool(s_p1WakeBufferFrozen)
            << " hold=" << TraceBool(s_p1WakeHoldPrimed) << "/" << TraceBool(s_p1WakeHoldIssued)
            << " dly=" << TraceBool(p1DelayState.isDelaying) << ":" << p1DelayState.delayFramesRemaining
            << " dTrig=" << TriggerTypeLabel(p1DelayState.triggerType)
            << " trig=" << TraceBool(p1TriggerActive) << ":" << p1TriggerCooldown;
    } else {
        oss << " pre=" << TraceBool(s_p2WakePrearmed)
            << " spec=" << TraceBool(s_p2WakePrearmIsSpecial)
            << " macro=" << TraceBool(s_p2WakePrearmIsMacro)
            << " action=" << s_p2WakePrearmActionType
            << " str=" << s_p2WakePrearmStrength
            << " c96=" << s_p2WakeMoveID96FrameCount
            << " m96=" << s_p2WakeMacro96FrameCount
            << " mQ=" << TraceBool(s_p2WakeMacroQueued)
            << " mTarget=" << s_p2WakeMacroTargetFrame
            << " mSlot=" << s_p2WakeMacroSlot
            << " opt=" << TraceBool(s_p2WakeOptionPicked)
            << " frozen=" << TraceBool(s_p2WakeBufferFrozen)
            << " hold=" << TraceBool(s_p2WakeHoldPrimed) << "/" << TraceBool(s_p2WakeHoldIssued)
            << " tokenCd=" << s_p2WakeMacroTokenNeutralizeCountdown
            << " dly=" << TraceBool(p2DelayState.isDelaying) << ":" << p2DelayState.delayFramesRemaining
            << " dTrig=" << TriggerTypeLabel(p2DelayState.triggerType)
            << " trig=" << TraceBool(p2TriggerActive) << ":" << p2TriggerCooldown
            << " restore=" << TraceBool(g_pendingControlRestore.load())
            << " p2Override=" << TraceBool(g_p2ControlOverridden);
    }
    TraceAppendBufferSnapshot(oss, playerNum);
}

static void TraceAutoActionWakePhase(const char* phase, short moveID1, short moveID2, short prevMoveID1, short prevMoveID2) {
    if (!TraceWakeWindowActive(moveID1, moveID2, prevMoveID1, prevMoveID2)) return;

    const int internalFrame = frameCounter.load();
    std::ostringstream oss;
    oss << "[AA_WAKE_TRACE] phase=" << (phase ? phase : "unknown")
        << " IF=" << internalFrame
        << " VF=" << (internalFrame / 3) << "." << TraceSubframeSuffix(internalFrame)
        << " macroState=" << static_cast<int>(MacroController::GetState())
        << " freeze=" << TraceBool(g_bufferFreezingActive.load())
        << " freezeOwner=" << g_activeFreezePlayer.load()
        << " wakeBuf=" << TraceBool(g_wakeBufferingEnabled.load())
        << " random=" << TraceBool(triggerRandomizeEnabled.load());
    TraceAppendPlayerWakeState(oss, 1, prevMoveID1, moveID1);
    TraceAppendPlayerWakeState(oss, 2, prevMoveID2, moveID2);
    LogOut(oss.str(), false);
}

static void TraceAutoActionWakeEvent(const char* eventLabel, int playerNum, short prevMoveID, short moveID, const std::string& extra = std::string()) {
    if (!TraceWakeRelevantForPlayer(playerNum, prevMoveID, moveID)) return;

    const int internalFrame = frameCounter.load();
    std::ostringstream oss;
    oss << "[AA_WAKE_TRACE] event=" << (eventLabel ? eventLabel : "unknown")
        << " IF=" << internalFrame
        << " VF=" << (internalFrame / 3) << "." << TraceSubframeSuffix(internalFrame)
        << " p=" << playerNum;
    TraceAppendPlayerWakeState(oss, playerNum, prevMoveID, moveID);
    if (!extra.empty()) {
        oss << " " << extra;
    }
    LogOut(oss.str(), false);
}
#else
static inline void TraceAutoActionWakePhase(const char*, short, short, short, short) {}
static inline void TraceAutoActionWakeEvent(const char*, int, short, short, const std::string& = std::string()) {}
#endif

// Lightweight stun/wakeup timing trackers for diagnostics
struct StunTimers {
    // Start frames for current stuns (-1 if not in that state)
    int blockStart = -1;
    int hitStart   = -1;
    int wakeMarker = -1;   // Last seen GROUNDTECH_RECOVERY frame before wake
    // Last measured durations (frames)
    int lastBlockDuration = 0;
    int lastHitDuration   = 0;
    int lastWakeDelay     = 0; // Frames from recovery marker (96) to actionable
    // Since-end counters (frames since leaving respective states)
    int sinceBlockEnd = -1;
    int sinceHitEnd   = -1;
    int sinceWake     = -1;    // Frames since actionable after wake
};

static StunTimers s_p1Timers;
static StunTimers s_p2Timers;

static inline bool AutoActionWorkPending();

// (Deprecated) Pending window for On RG: retained for state reset only
static bool s_p1RGPending = false;
static int  s_p1RGExpiry = 0;
static bool s_p2RGPending = false;
static int  s_p2RGExpiry = 0;

// Enable tick-integrated auto-actions by default; the frame monitor will defer when this is on.
std::atomic<bool> g_tickIntegratedAutoActions{ true };

// Lightweight per-tick entry from input hook: evaluates auto-actions once per internal tick
void AutoActionsTick_Inline(short moveID1, short moveID2) {
    // Online mode hard stop (never operate in netplay)
    if (g_onlineModeActive.load()) return;
    if (Mission::Engine::Demo::IsActive()) return;
    if (!AutoActionWorkPending()) return;

    // Static prevs to compute edges without extra memory traffic
    static short prevMoveID1 = -1;
    static short prevMoveID2 = -1;

    TraceAutoActionWakePhase("tick:begin", moveID1, moveID2, prevMoveID1, prevMoveID2);
    
    // Check if jump state reached after wake hold
    if (s_p1WakeJumpTrackFrame >= 0) {
        int elapsed = frameCounter.load() - s_p1WakeJumpTrackFrame;
        if (elapsed <= 10) {  // Track for 10 frames
            if (moveID1 == 4 || moveID1 == 5 || moveID1 == 6) {
                LogOut("[AUTO-ACTION] P1 wake jump SUCCESS! moveID=" + std::to_string(moveID1) + 
                       " elapsed=" + std::to_string(elapsed) + "F", true);
                s_p1WakeJumpTrackFrame = -1;
            }
        } else {
            LogOut("[AUTO-ACTION] P1 wake jump FAILED - no jump state reached within 10F (moveID=" + 
                   std::to_string(moveID1) + ")", true);
            s_p1WakeJumpTrackFrame = -1;
        }
    }
    if (s_p2WakeJumpTrackFrame >= 0) {
        int elapsed = frameCounter.load() - s_p2WakeJumpTrackFrame;
        if (elapsed <= 10) {  // Track for 10 frames
            if (moveID2 == 4 || moveID2 == 5 || moveID2 == 6) {
                LogOut("[AUTO-ACTION] P2 wake jump SUCCESS! moveID=" + std::to_string(moveID2) + 
                       " elapsed=" + std::to_string(elapsed) + "F", true);
                s_p2WakeJumpTrackFrame = -1;
            }
        } else {
            LogOut("[AUTO-ACTION] P2 wake jump FAILED - no jump state reached within 10F (moveID=" + 
                   std::to_string(moveID2) + ")", true);
            s_p2WakeJumpTrackFrame = -1;
        }
    }

    // Process any armed delays first, then evaluate triggers against current moves
    ProcessTriggerDelays(moveID1, moveID2, prevMoveID1, prevMoveID2);
    TraceAutoActionWakePhase("tick:after-delays", moveID1, moveID2, prevMoveID1, prevMoveID2);
    MonitorAutoActions(moveID1, moveID2, prevMoveID1, prevMoveID2);
    TraceAutoActionWakePhase("tick:after-monitor", moveID1, moveID2, prevMoveID1, prevMoveID2);
    ClearDelayStatesIfNonActionable(moveID1, moveID2, prevMoveID1, prevMoveID2, "tick");
    TraceAutoActionWakePhase("tick:after-clear", moveID1, moveID2, prevMoveID1, prevMoveID2);

    // Update prevs for next tick
    // if (detailedLogging.load() && (IsGroundtech(prevMoveID2) || IsGroundtech(moveID2))) {
    //     LogOut("[AUTO-ACTION][PREV-UPDATE] P2 prev " + std::to_string(prevMoveID2) + "->" + std::to_string(moveID2) +
    //            " (wasGroundtech=" + std::to_string(IsGroundtech(prevMoveID2)) + 
    //            " isGroundtech=" + std::to_string(IsGroundtech(moveID2)) + ")", true);
    // }
    TraceAutoActionWakePhase("tick:prev-update", moveID1, moveID2, moveID1, moveID2);
    prevMoveID1 = moveID1;
    prevMoveID2 = moveID2;
}

// New: Pre-arm specials/FM at RG entry when user delay is 0
static bool s_p1RGPrearmed = false;
static bool s_p2RGPrearmed = false;
static bool s_p1RGPrearmIsSpecial = false; // true when we early-froze a special/FM
static bool s_p2RGPrearmIsSpecial = false;
static int  s_p1RGPrearmActionType = -1;   // store action to help skip duplicate inject
static int  s_p2RGPrearmActionType = -1;
static int  s_p1RGPrearmExpiry = 0;        // safety expiry (internal frames)
static int  s_p2RGPrearmExpiry = 0;

static inline void UpdateStunTimersForPlayer(StunTimers& t, short prevMoveID, short currMoveID) {
    int now = frameCounter.load();

    // Blockstun entry/exit
    bool wasBlk = IsBlockstun(prevMoveID);
    bool isBlk  = IsBlockstun(currMoveID);
    if (!wasBlk && isBlk) {
        t.blockStart = now;
        t.sinceBlockEnd = -1;
    } else if (wasBlk && !isBlk) {
        if (t.blockStart >= 0) t.lastBlockDuration = now - t.blockStart;
        t.blockStart = -1;
        t.sinceBlockEnd = 0;
    } else if (!isBlk && t.sinceBlockEnd >= 0) {
        t.sinceBlockEnd++;
    }

    // Hitstun entry/exit
    bool wasHit = IsHitstun(prevMoveID);
    bool isHit  = IsHitstun(currMoveID);
    if (!wasHit && isHit) {
        t.hitStart = now;
        t.sinceHitEnd = -1;
    } else if (wasHit && !isHit) {
        if (t.hitStart >= 0) t.lastHitDuration = now - t.hitStart;
        t.hitStart = -1;
        t.sinceHitEnd = 0;
    } else if (!isHit && t.sinceHitEnd >= 0) {
        t.sinceHitEnd++;
    }

    // Wakeup: remember last recovery marker (96), and when we become actionable from groundtech, compute delay
    bool wasGtech = IsGroundtech(prevMoveID);
    bool isGtech  = IsGroundtech(currMoveID);
    bool actionableNow = IsActionable(currMoveID);
    if (currMoveID == GROUNDTECH_RECOVERY) {
        t.wakeMarker = now;
        t.sinceWake = -1;
    }
    if (wasGtech && actionableNow && !isGtech) {
        // Became actionable after groundtech
        if (t.wakeMarker >= 0) t.lastWakeDelay = now - t.wakeMarker;
        t.sinceWake = 0;
        t.wakeMarker = -1;
    } else if (t.sinceWake >= 0 && actionableNow) {
        t.sinceWake++;
    }
}
// Lightweight check to skip all auto-action work when nothing can or should run
static inline bool AutoActionWorkPending() {
    // If macro playback is active, suppress P2 auto-action work to avoid conflicts.
    if (MacroController::GetState() == MacroController::State::Replaying) {
        return false;
    }
    // If any triggers are enabled, we may need to evaluate
    bool triggersEnabled = HasAnyAutoActionTriggerEnabled();
    // If a delay is active, wakeup is pre-armed, cooldowns are running, or restore is pending, keep running
    bool delaysActive = p1DelayState.isDelaying || p2DelayState.isDelaying;
    bool cooldownsActive = p1TriggerActive || p2TriggerActive || (p1TriggerCooldown > 0) || (p2TriggerCooldown > 0);
    bool wakePrearmed = s_p1WakePrearmed || s_p2WakePrearmed;
    bool restorePending = g_pendingControlRestore.load();
    return triggersEnabled || delaysActive || cooldownsActive || wakePrearmed || restorePending;
}


bool IsCharacterGrounded(int playerNum) {
    uintptr_t base = GetEFZBase();
    if (!base) return true; // Default to true if can't check
    
    uintptr_t playerOffset = (playerNum == 1) ? EFZ_BASE_OFFSET_P1 : EFZ_BASE_OFFSET_P2;
    uintptr_t yAddr = ResolvePointer(base, playerOffset, YPOS_OFFSET);
    uintptr_t yVelAddr = ResolvePointer(base, playerOffset, YVEL_OFFSET);
    
    if (!yAddr || !yVelAddr) return true; // Default to true if can't check
    
    double yPos = 0.0, yVel = 0.0;
    SafeReadMemory(yAddr, &yPos, sizeof(double));
    SafeReadMemory(yVelAddr, &yVel, sizeof(double));
    
    // Character is considered grounded when very close to y=0 and not moving vertically
    return (yPos <= 0.1 && fabs(yVel) < 0.1);
}

short GetActionMoveID(int actionType, int triggerType, int playerNum) {
    // Custom action deprecated; proceed directly to contextual resolution
    
    // Check character ground/air state for context-specific moves
    bool isGrounded = IsCharacterGrounded(playerNum);
    // Move ground state check logs to detailed mode
    LogOut("[AUTO-ACTION] Player " + std::to_string(playerNum) + 
           " ground state: " + (isGrounded ? "grounded" : "airborne") + 
           ", action type: " + std::to_string(actionType), detailedLogging.load());
    
    // Air-specific moves - only process these if character is in the air
    if (!isGrounded) {
        switch (actionType) {
            case ACTION_JA:
                return BASE_ATTACK_JA;  // Air A
            case ACTION_JB:
                return BASE_ATTACK_JB;  // Air B
            case ACTION_JC:
                return BASE_ATTACK_JC;  // Air C
            case ACTION_JD:
                return BASE_ATTACK_JD;  // Air D
            default:
                // If in air and trying to do a ground move, use a safe fallback
                return STRAIGHT_JUMP_ID;
        }
    }
    else { // Character is on the ground
        // Air move attempted on ground - check if it's a jump-related action
        if (actionType == ACTION_JA || actionType == ACTION_JB || actionType == ACTION_JC || actionType == ACTION_JD) {
            LogOut("[AUTO-ACTION] Cannot perform air move on ground, using JUMP instead", true);
            return STRAIGHT_JUMP_ID; // If air move selected on ground, do a jump instead
        }
        
        // Ground moves - only available when grounded
        switch (actionType) {
            case ACTION_5A:
                return BASE_ATTACK_5A;  // 200
            case ACTION_5B:
                return BASE_ATTACK_5B;  // 201
            case ACTION_5C:
                return BASE_ATTACK_5C;  // 203
            case ACTION_5D:
                return BASE_ATTACK_5D;  // D button standing
            case ACTION_2A:
                return BASE_ATTACK_2A;  // 204
            case ACTION_2B:
                return BASE_ATTACK_2B;  // 205
            case ACTION_2C:
                return BASE_ATTACK_2C;  // 206
            case ACTION_2D:
                return BASE_ATTACK_2D;  // D button crouching
            case ACTION_JUMP:
                return STRAIGHT_JUMP_ID; // 4
            case ACTION_BACKDASH:
                return BACKWARD_DASH_START_ID; // 165
            case ACTION_BLOCK:
                return STAND_GUARD_ID; // 151
            default:
                return BASE_ATTACK_5A; // Default to 5A for unknown actions
        }
    }
}

static void ResetDelayState(TriggerDelayState& state) {
    state.isDelaying = false;
    state.delayFramesRemaining = 0;
    state.triggerType = TRIGGER_NONE;
    state.pendingMoveID = 0;
    state.chosenAction = -1;
    state.chosenStrength = -1;
    state.chosenMacroSlot = 0;
    state.chosenCustomId = -1;
    state.chosenDelay = -1;
}

static bool IsDashLikeState(short moveID) {
    return moveID == FORWARD_DASH_START_ID ||
           moveID == FORWARD_DASH_RECOVERY_ID ||
           moveID == FORWARD_DASH_RECOVERY_SENTINEL_ID ||
           moveID == BACKWARD_DASH_START_ID ||
           moveID == BACKWARD_DASH_RECOVERY_ID ||
           moveID == KAORI_FORWARD_DASH_START_ID;
}

static bool IsJumpLikeState(short moveID) {
    return moveID == STRAIGHT_JUMP_ID ||
           moveID == FORWARD_JUMP_ID ||
           moveID == BACKWARD_JUMP_ID ||
           moveID == FALLING_ID;
}

static inline int TriggerStrengthForType(int triggerType) {
    switch (triggerType) {
        case TRIGGER_AFTER_BLOCK:   return triggerAfterBlockStrength.load();
        case TRIGGER_ON_WAKEUP:     return triggerOnWakeupStrength.load();
        case TRIGGER_AFTER_HITSTUN: return triggerAfterHitstunStrength.load();
        case TRIGGER_AFTER_AIRTECH: return triggerAfterAirtechStrength.load();
        case TRIGGER_ON_RG:         return triggerOnRGStrength.load();
        default:                    return 0;
    }
}

static inline int TriggerDelayForType(int triggerType) {
    switch (triggerType) {
        case TRIGGER_AFTER_BLOCK:   return triggerAfterBlockDelay.load();
        case TRIGGER_ON_WAKEUP:     return triggerOnWakeupDelay.load();
        case TRIGGER_AFTER_HITSTUN: return triggerAfterHitstunDelay.load();
        case TRIGGER_AFTER_AIRTECH: return triggerAfterAirtechDelay.load();
        case TRIGGER_ON_RG:         return triggerOnRGDelay.load();
        default:                    return 0;
    }
}

static inline int TriggerPoolDelayForIndex(int triggerType, int idx) {
    if (idx < 0 || idx >= MAX_ACTION_POOL_OPTIONS) {
        return TriggerDelayForType(triggerType);
    }

    const int* delays = nullptr;
    switch (triggerType) {
        case TRIGGER_AFTER_BLOCK:   delays = g_afterBlockActionPoolDelays; break;
        case TRIGGER_ON_WAKEUP:     delays = g_onWakeupActionPoolDelays; break;
        case TRIGGER_AFTER_HITSTUN: delays = g_afterHitstunActionPoolDelays; break;
        case TRIGGER_AFTER_AIRTECH: delays = g_afterAirtechActionPoolDelays; break;
        case TRIGGER_ON_RG:         delays = g_onRGActionPoolDelays; break;
        default:                    break;
    }
    if (!delays) {
        return TriggerDelayForType(triggerType);
    }

    const int explicitDelay = delays[idx];
    if (explicitDelay < 0) {
        return TriggerDelayForType(triggerType);
    }
    return CLAMP(explicitDelay, 0, 60);
}

static inline int TriggerCustomIdForType(int triggerType) {
    switch (triggerType) {
        case TRIGGER_AFTER_BLOCK:   return triggerAfterBlockCustomID.load();
        case TRIGGER_ON_WAKEUP:     return triggerOnWakeupCustomID.load();
        case TRIGGER_AFTER_HITSTUN: return triggerAfterHitstunCustomID.load();
        case TRIGGER_AFTER_AIRTECH: return triggerAfterAirtechCustomID.load();
        case TRIGGER_ON_RG:         return triggerOnRGCustomID.load();
        default:                    return BASE_ATTACK_5A;
    }
}

static inline bool TriggerPoolConfigForType(int triggerType, uint32_t& legacyMask,
                                            uint64_t& maskLo, uint64_t& maskHi,
                                            bool& usePool) {
    switch (triggerType) {
        case TRIGGER_AFTER_BLOCK:
            legacyMask = triggerAfterBlockActionPoolMask.load();
            maskLo = triggerAfterBlockActionPoolMaskLo.load();
            maskHi = triggerAfterBlockActionPoolMaskHi.load();
            usePool = triggerAfterBlockUsePool.load();
            break;
        case TRIGGER_ON_WAKEUP:
            legacyMask = triggerOnWakeupActionPoolMask.load();
            maskLo = triggerOnWakeupActionPoolMaskLo.load();
            maskHi = triggerOnWakeupActionPoolMaskHi.load();
            usePool = triggerOnWakeupUsePool.load();
            break;
        case TRIGGER_AFTER_HITSTUN:
            legacyMask = triggerAfterHitstunActionPoolMask.load();
            maskLo = triggerAfterHitstunActionPoolMaskLo.load();
            maskHi = triggerAfterHitstunActionPoolMaskHi.load();
            usePool = triggerAfterHitstunUsePool.load();
            break;
        case TRIGGER_AFTER_AIRTECH:
            legacyMask = triggerAfterAirtechActionPoolMask.load();
            maskLo = triggerAfterAirtechActionPoolMaskLo.load();
            maskHi = triggerAfterAirtechActionPoolMaskHi.load();
            usePool = triggerAfterAirtechUsePool.load();
            break;
        case TRIGGER_ON_RG:
            legacyMask = triggerOnRGActionPoolMask.load();
            maskLo = triggerOnRGActionPoolMaskLo.load();
            maskHi = triggerOnRGActionPoolMaskHi.load();
            usePool = triggerOnRGUsePool.load();
            break;
        default:
            legacyMask = 0;
            maskLo = 0;
            maskHi = 0;
            usePool = false;
            return false;
    }

    if ((maskLo | maskHi) == 0 && legacyMask != 0) {
        const int strength = CLAMP(TriggerStrengthForType(triggerType), 0, 3);
        auto setBit = [&](int index) {
            if (index < 0 || index >= 128) return;
            if (index < 64) maskLo |= (1ull << index);
            else maskHi |= (1ull << (index - 64));
        };
        auto legacyToConcrete = [&](int motionIdx) -> int {
            switch (motionIdx) {
                case 0:  return 0 + strength;   // 5A-D
                case 1:  return 4 + strength;   // 2A-D
                case 2:  return 8 + strength;   // jA-D
                case 3:  return 20 + strength;  // 236A-D
                case 4:  return 24 + strength;  // 623A-D
                case 5:  return 28 + strength;  // 214A-D
                case 6:  return 32 + strength;  // 421A-D
                case 7:  return 44 + strength;  // 41236A-D
                case 8:  return 48 + strength;  // 214236A-D
                case 9:  return 52 + strength;  // 236236A-D
                case 10: return 56 + strength;  // 214214A-D
                case 11: return 60 + strength;  // 641236A-D
                case 12: return 64 + strength;  // 463214A-D
                case 13: return 36 + strength;  // 412A-D
                case 14: return 40 + strength;  // 22A-D
                case 15: return 68 + strength;  // 4123641236A-D
                case 16: return 72 + strength;  // 6321463214A-D
                case 17: return 77 + CLAMP(TriggerStrengthForType(triggerType), 0, 2);
                case 18: return 80;
                case 19: return 81;
                case 20: return 82;
                case 21: return 76;
                case 22: return 12 + strength;  // 6A-D
                case 23: return 16 + strength;  // 4A-D
                default: return -1;
            }
        };
        for (int bit = 0; bit < 24; ++bit) {
            if ((legacyMask & (1u << bit)) == 0) continue;
            setBit(legacyToConcrete(bit));
        }
    }

    return true;
}

static inline TriggerOption MapConcretePoolIndexToOption(int idx, int triggerType) {
    int action = ACTION_5A;
    int strength = 0;
    if (idx >= 0 && idx <= 19) {
        action = idx;
        strength = idx % 4;
    } else if (idx >= 20 && idx <= 43) {
        const int local = idx - 20;
        strength = local % 4;
        switch (local / 4) {
            case 0: action = ACTION_QCF; break;
            case 1: action = ACTION_DP; break;
            case 2: action = ACTION_QCB; break;
            case 3: action = ACTION_421; break;
            case 4: action = ACTION_412; break;
            case 5: action = ACTION_22; break;
            default: action = ACTION_QCF; break;
        }
    } else if (idx >= 44 && idx <= 75) {
        const int local = idx - 44;
        strength = local % 4;
        switch (local / 4) {
            case 0: action = ACTION_SUPER1; break;
            case 1: action = ACTION_SUPER2; break;
            case 2: action = ACTION_236236; break;
            case 3: action = ACTION_214214; break;
            case 4: action = ACTION_641236; break;
            case 5: action = ACTION_463214; break;
            case 6: action = ACTION_4123641236; break;
            case 7: action = ACTION_6321463214; break;
            default: action = ACTION_SUPER1; break;
        }
    } else {
        switch (idx) {
            case 76: action = ACTION_FINAL_MEMORY; strength = 0; break;
            case 77: action = ACTION_JUMP; strength = 0; break;
            case 78: action = ACTION_JUMP; strength = 1; break;
            case 79: action = ACTION_JUMP; strength = 2; break;
            case 80: action = ACTION_BACKDASH; strength = 0; break;
            case 81: action = ACTION_FORWARD_DASH; strength = 0; break;
            case 82: action = ACTION_BLOCK; strength = 0; break;
            default: action = ACTION_5A; strength = 0; break;
        }
    }
    return TriggerOption{
        true,
        action,
        strength,
        TriggerPoolDelayForIndex(triggerType, idx),
        TriggerCustomIdForType(triggerType),
        0
    };
}

static inline int ResolvePickedDelayForTrigger(int triggerType, int currentDelayFrames, int pickedUserDelay) {
    pickedUserDelay = CLAMP(pickedUserDelay, 0, 60);
    switch (triggerType) {
        case TRIGGER_ON_WAKEUP:
            return (pickedUserDelay <= 1) ? 0 : (pickedUserDelay - 1);
        case TRIGGER_ON_RG: {
            const int baseUserDelay = triggerOnRGDelay.load();
            const int resolved = currentDelayFrames - baseUserDelay + pickedUserDelay;
            return resolved < 0 ? 0 : resolved;
        }
        default:
            return pickedUserDelay;
    }
}

static inline bool PickRandomPoolOption(int triggerType, int playerNum, TriggerOption& out) {
    uint32_t legacyMask = 0;
    uint64_t maskLo = 0;
    uint64_t maskHi = 0;
    bool usePool = false;
    if (!TriggerPoolConfigForType(triggerType, legacyMask, maskLo, maskHi, usePool) ||
        !usePool || ((maskLo | maskHi) == 0)) {
        return false;
    }

    int candidates[128];
    int count = 0;
    for (int bit = 0; bit < 83; ++bit) {
        const bool set = bit < 64
            ? (((maskLo >> bit) & 1ull) != 0)
            : (((maskHi >> (bit - 64)) & 1ull) != 0);
        if (!set) continue;
        candidates[count++] = bit;
    }
    if (count <= 0) return false;

    int seed = frameCounter.load() + playerNum * 31 + triggerType * 17;
    if (seed < 0) seed = -seed;
    const int poolIdx = candidates[seed % count];
    out = MapConcretePoolIndexToOption(poolIdx, triggerType);
    return true;
}

static bool ShouldCancelDelayForState(const TriggerDelayState& state, short prevMoveID, short moveID, std::string& reason) {
    if (!state.isDelaying) {
        return false;
    }

    if (state.triggerType == TRIGGER_AFTER_BLOCK) {
        if (!IsBlockstun(prevMoveID) && IsBlockstun(moveID)) {
            reason = "new_blockstun";
            return true;
        }
        if (IsHitstun(moveID)) {
            reason = "hitstun";
            return true;
        }
        if (IsThrown(moveID)) {
            reason = "thrown";
            return true;
        }
        if (IsLaunched(moveID)) {
            reason = "launched";
            return true;
        }
        if (IsAirtech(moveID)) {
            reason = "airtech";
            return true;
        }
        if (IsGroundtech(moveID)) {
            reason = "groundtech";
            return true;
        }
        if (IsFrozen(moveID)) {
            reason = "frozen";
            return true;
        }
        if (IsJumpLikeState(moveID)) {
            reason = "jump";
            return true;
        }
        if (IsDashLikeState(moveID)) {
            reason = "dash";
            return true;
        }
        if (IsAttackMove(moveID)) {
            reason = "attack";
            return true;
        }
        if (IsRecoilGuard(moveID)) {
            reason = "recoil_guard";
            return true;
        }
        return false;
    }

    if (IsBlockstun(moveID)) {
        reason = "blockstun";
        return true;
    }
    if (IsHitstun(moveID)) {
        reason = "hitstun";
        return true;
    }
    if (IsFrozen(moveID)) {
        reason = "frozen";
        return true;
    }
    if (IsThrown(moveID)) {
        reason = "thrown";
        return true;
    }
    if (IsLaunched(moveID)) {
        reason = "launched";
        return true;
    }
    if (IsAirtech(moveID)) {
        reason = "airtech";
        return true;
    }
    if (IsGroundtech(moveID)) {
        reason = "groundtech";
        return true;
    }

    return false;
}

void ProcessTriggerDelays(short moveID1, short moveID2, short prevMoveID1, short prevMoveID2) {
    uintptr_t base = GetEFZBase();
    if (!base) return;

    // P1 delay processing
    if (p1DelayState.isDelaying) {
        const int remainingBefore = p1DelayState.delayFramesRemaining;
        const int visualBefore = InternalTicksToVisualFramesCeil(remainingBefore);
        LogOut("[DELAY] P1 delaying: internalRemaining=" + std::to_string(remainingBefore) +
               ", visualRemaining=" + std::to_string(visualBefore) +
               ", triggerType=" + std::to_string(p1DelayState.triggerType), detailedLogging.load());
        p1DelayState.delayFramesRemaining--;
        const int remainingAfter = p1DelayState.delayFramesRemaining;
        const int visualAfter = InternalTicksToVisualFramesCeil(remainingAfter);

        if (remainingAfter > 0 && visualAfter != visualBefore && detailedLogging.load()) {
            LogOut("[AUTO-ACTION] P1 delay countdown: visualRemaining=" + std::to_string(visualAfter) +
                   " internalRemaining=" + std::to_string(remainingAfter),
                   true);
        }
        
    if (remainingAfter <= 0) {
            LogOut("[AUTO-ACTION] P1 delay expired, applying action", true);
            TraceAutoActionWakeEvent("p1-delay-expired", 1, prevMoveID1, moveID1);
            // Use unified per-frame sample; avoid redundant MOVE_ID resolution
            ApplyAutoAction(1, 0, moveID1, prevMoveID1); // address ignored
            LogOut("[AUTO-ACTION] P1 action applied via input system", true);
            if (ValidationMetricsEnabled()) { GetValidationMetrics().p1ActionsApplied++; }
            ResetDelayState(p1DelayState);
        }
    }
    
    // P2 delay processing
    if (p2DelayState.isDelaying) {
        const int remainingBefore = p2DelayState.delayFramesRemaining;
        const int visualBefore = InternalTicksToVisualFramesCeil(remainingBefore);
        LogOut("[DELAY] P2 delaying: internalRemaining=" + std::to_string(remainingBefore) +
               ", visualRemaining=" + std::to_string(visualBefore) +
               ", triggerType=" + std::to_string(p2DelayState.triggerType), detailedLogging.load());
        p2DelayState.delayFramesRemaining--;
        const int remainingAfter = p2DelayState.delayFramesRemaining;
        const int visualAfter = InternalTicksToVisualFramesCeil(remainingAfter);

        if (remainingAfter > 0 && visualAfter != visualBefore && detailedLogging.load()) {
            LogOut("[AUTO-ACTION] P2 delay countdown: visualRemaining=" + std::to_string(visualAfter) +
                   " internalRemaining=" + std::to_string(remainingAfter),
                   true);
        }
        
    if (remainingAfter <= 0) {
            LogOut("[AUTO-ACTION] P2 delay expired, applying action", true);
            TraceAutoActionWakeEvent("p2-delay-expired", 2, prevMoveID2, moveID2);

            // If a macro is selected for this trigger and has data, prefer playing it
            int sel = 0;
            switch (p2DelayState.triggerType) {
                case TRIGGER_AFTER_BLOCK: sel = triggerAfterBlockMacroSlot.load(); break;
                case TRIGGER_ON_WAKEUP: sel = triggerOnWakeupMacroSlot.load(); break;
                case TRIGGER_AFTER_HITSTUN: sel = triggerAfterHitstunMacroSlot.load(); break;
                case TRIGGER_AFTER_AIRTECH: sel = triggerAfterAirtechMacroSlot.load(); break;
                case TRIGGER_ON_RG: sel = triggerOnRGMacroSlot.load(); break;
                default: sel = 0; break;
            }
            // Prefer row-specific macro slot if set
            if (p2DelayState.chosenMacroSlot > 0) sel = p2DelayState.chosenMacroSlot;
            if (sel > 0 && MacroController::GetState() != MacroController::State::Recording) {
                sel = CLAMP(sel, 1, MacroController::GetSlotCount());
                if (!MacroController::IsSlotEmpty(sel)) {
                    MacroController::SetCurrentSlot(sel);
                    if (MacroController::GetState() != MacroController::State::Replaying) {
                        LogOut("[AUTO-ACTION][MACRO] Starting macro playback (slot=" + std::to_string(sel) + ") for P2 on trigger expiry", true);
                        MacroController::Play();
                        // Clear the delay state and exit
                        ResetDelayState(p2DelayState);
                        p2TriggerActive = false;
                        p2TriggerCooldown = 0;
                        if (ValidationMetricsEnabled()) { GetValidationMetrics().p2ActionsApplied++; }
                        return;
                    }
                }
            }

            // Fallback: apply the configured single action via input system using unified sample
            if (MacroController::GetState() != MacroController::State::Replaying) {
                ApplyAutoAction(2, 0, moveID2, prevMoveID2);
            } else {
                LogOut("[AUTO-ACTION][MACRO] P2 macro active; skipping ApplyAutoAction", detailedLogging.load());
                if (ValidationMetricsEnabled()) { GetValidationMetrics().p2SuppressedByMacro++; }
            }
            LogOut("[AUTO-ACTION] P2 action applied via input system", true);
            if (ValidationMetricsEnabled() && MacroController::GetState() != MacroController::State::Replaying) { GetValidationMetrics().p2ActionsApplied++; }
            ResetDelayState(p2DelayState);
            p2ActionApplied = true;
            if (detailedLogging.load()) {
                LogOut(std::string("[AUTO-ACTION] P2 delayed action applied; pendingRestore=") +
                       (g_pendingControlRestore.load()?"true":"false"), true);
            }
        }
    }
}

void StartTriggerDelay(int playerNum, int triggerType, short moveID, int delayFrames) {
    // Do not queue P2 trigger delays while a macro is playing; they would fight the macro.
    if (playerNum == 2 && MacroController::GetState() == MacroController::State::Replaying) {
        LogOut("[AUTO-ACTION][MACRO] Suppressing P2 StartTriggerDelay during macro playback", detailedLogging.load());
        return;
    }
    // Check if the player is already in a trigger cooldown period
    if (playerNum == 1 && p1TriggerActive) {
    LogOut("[AUTO-ACTION] P1 StartTriggerDelay ignored - trigger already active/cooldown=" + std::to_string(p1TriggerCooldown), detailedLogging.load());
        return;
    }
    if (playerNum == 2 && p2TriggerActive) {
    LogOut("[AUTO-ACTION] P2 StartTriggerDelay ignored - trigger already active/cooldown=" + std::to_string(p2TriggerCooldown), detailedLogging.load());
        return;
    }

    // Set the last active trigger type and frame for overlay feedback
    g_lastActiveTriggerType.store(triggerType);
    g_lastActiveTriggerFrame.store(frameCounter.load());

    AutoActionLogScope triggerScope("StartTriggerDelay", playerNum, triggerType);
    (void)triggerScope;

    LogOut("[AUTO-ACTION] StartTriggerDelay called: Player=" + std::to_string(playerNum) + 
           ", triggerType=" + std::to_string(triggerType) + 
           ", delay=" + std::to_string(delayFrames) +
           ", p1ActApplied=" + std::to_string(p1ActionApplied) +
           ", p2ActApplied=" + std::to_string(p2ActionApplied) +
           ", p1TrigActive=" + std::to_string(p1TriggerActive) +
           ", p2TrigActive=" + std::to_string(p2TriggerActive) +
           ", p1Cooldown=" + std::to_string(p1TriggerCooldown) +
           ", p2Cooldown=" + std::to_string(p2TriggerCooldown), detailedLogging.load());
    if (triggerType == TRIGGER_ON_WAKEUP) {
        const short stateMoveID = static_cast<short>(GetPlayerMoveID(playerNum));
        TraceAutoActionWakeEvent("start-trigger-delay:wakeup", playerNum, stateMoveID, stateMoveID,
                                 "pendingActionMoveID=" + std::to_string(moveID) +
                                 " delayF=" + std::to_string(delayFrames));
    }
    if (ValidationMetricsEnabled()) {
        ValidationMetrics &vm = GetValidationMetrics();
        if (playerNum == 1) vm.p1TriggerStarts++; else if (playerNum == 2) vm.p2TriggerStarts++;
    }
    // NOTE: We no longer unconditionally override P2 control here. Override is now done
    // just-in-time only for specials/supers inside ApplyAutoAction (and wake pre-arm
    // when freezing a special). Normals/jumps/dashes use immediate inputs and do not
    // require AI flag changes.
    // Set trigger cooldown to prevent rapid re-triggering
    if (playerNum == 1) {
        p1TriggerActive = true;
        p1TriggerCooldown = TRIGGER_COOLDOWN_FRAMES;
        p1DelayState.triggerType = triggerType;
    } else {
        p2TriggerActive = true;
        p2TriggerCooldown = TRIGGER_COOLDOWN_FRAMES;
        p2DelayState.triggerType = triggerType;
    }

    TriggerDelayState& dstate = (playerNum == 1) ? p1DelayState : p2DelayState;
    dstate.chosenAction = -1;
    dstate.chosenStrength = -1;
    dstate.chosenMacroSlot = 0;
    dstate.chosenCustomId = -1;
    dstate.chosenDelay = -1;
    
    // Random Pool is picked at trigger-arm time so wakeup pre-buffering and
    // delayed actions both consume the same concrete action. P2 wakeup may
    // have already rolled an option while entering groundtech; reuse that
    // result so the buffered action cannot diverge from the armed action.
    const bool reuseP1WakePick = (playerNum == 1 &&
                                  triggerType == TRIGGER_ON_WAKEUP &&
                                  s_p1WakeOptionPicked);
    const bool reuseP2WakePick = (playerNum == 2 &&
                                  triggerType == TRIGGER_ON_WAKEUP &&
                                  s_p2WakeOptionPicked);
    TriggerOption poolPick{};
    if (reuseP1WakePick || reuseP2WakePick) {
        const TriggerOption& wakePick = reuseP1WakePick ? s_p1WakePrePickedOption : s_p2WakePrePickedOption;
        dstate.chosenAction = wakePick.action;
        dstate.chosenStrength = wakePick.strength;
        dstate.chosenMacroSlot = wakePick.macroSlot;
        dstate.chosenCustomId = wakePick.customId;
        dstate.chosenDelay = wakePick.delay;
        LogOut("[AUTO-ACTION] Wake pre-picked option reused: action=" +
               std::to_string(wakePick.action) +
               " strength=" + std::to_string(wakePick.strength) +
               " macroSlot=" + std::to_string(wakePick.macroSlot) +
               " delayF=" + std::to_string(delayFrames), true);
    } else if (PickRandomPoolOption(triggerType, playerNum, poolPick)) {
        dstate.chosenAction = poolPick.action;
        dstate.chosenStrength = poolPick.strength;
        dstate.chosenMacroSlot = poolPick.macroSlot;
        dstate.chosenCustomId = poolPick.customId;
        dstate.chosenDelay = poolPick.delay;
        delayFrames = ResolvePickedDelayForTrigger(triggerType, delayFrames, poolPick.delay);
        LogOut("[AUTO-ACTION] Pool-selected option applied: action=" + std::to_string(poolPick.action) +
               " strength=" + std::to_string(poolPick.strength) +
               " userDelayF=" + std::to_string(poolPick.delay) +
               " resolvedDelayF=" + std::to_string(delayFrames), true);
    } else if (HasEnabledRows(triggerType)) {
        // If there are per-trigger option rows configured, pick one now and
        // override both the chosen action parameters and the delay as needed.
        TriggerOption picked{};
        if (PickRandomRow(triggerType, picked)) {
            // Stash chosen params on the appropriate delay state so ApplyAutoAction can consume them
            dstate.chosenAction    = picked.action;
            dstate.chosenStrength  = picked.strength;
            dstate.chosenMacroSlot = picked.macroSlot;
            dstate.chosenCustomId  = picked.customId;
            dstate.chosenDelay     = picked.delay;
            delayFrames = ResolvePickedDelayForTrigger(triggerType, delayFrames, picked.delay);
            LogOut("[AUTO-ACTION] Row-selected option applied: action=" + std::to_string(picked.action) +
                   " strength=" + std::to_string(picked.strength) +
                   " macroSlot=" + std::to_string(picked.macroSlot) +
                   " userDelayF=" + std::to_string(picked.delay) +
                   " resolvedDelayF=" + std::to_string(delayFrames), true);
        }
    }

    // If delay is 0, apply immediately
    if (delayFrames == 0) {
        // Immediate apply path; ApplyAutoAction uses input system, so addr is informational only
        uintptr_t base = GetEFZBase();
        uintptr_t moveIDAddr = 0;
        if (base) {
            moveIDAddr = (playerNum == 1)
                ? ResolvePointer(base, EFZ_BASE_OFFSET_P1, MOVE_ID_OFFSET)
                : ResolvePointer(base, EFZ_BASE_OFFSET_P2, MOVE_ID_OFFSET);
        }
        // For P2: if a macro slot is selected for this trigger, play it instead
        if (playerNum == 2) {
            int sel = 0;
            switch (triggerType) {
                case TRIGGER_AFTER_BLOCK: sel = triggerAfterBlockMacroSlot.load(); break;
                case TRIGGER_ON_WAKEUP: sel = triggerOnWakeupMacroSlot.load(); break;
                case TRIGGER_AFTER_HITSTUN: sel = triggerAfterHitstunMacroSlot.load(); break;
                case TRIGGER_AFTER_AIRTECH: sel = triggerAfterAirtechMacroSlot.load(); break;
                case TRIGGER_ON_RG: sel = triggerOnRGMacroSlot.load(); break;
                default: sel = 0; break;
            }
            // Prefer a row-specific macro slot when present
            if (playerNum == 2) {
                if ((triggerType == TRIGGER_AFTER_BLOCK && p2DelayState.chosenMacroSlot > 0) ||
                    (triggerType == TRIGGER_ON_WAKEUP && p2DelayState.chosenMacroSlot > 0) ||
                    (triggerType == TRIGGER_AFTER_HITSTUN && p2DelayState.chosenMacroSlot > 0) ||
                    (triggerType == TRIGGER_AFTER_AIRTECH && p2DelayState.chosenMacroSlot > 0) ||
                    (triggerType == TRIGGER_ON_RG && p2DelayState.chosenMacroSlot > 0)) {
                    sel = p2DelayState.chosenMacroSlot;
                }
            }
            if (sel > 0 && MacroController::GetState() != MacroController::State::Recording) {
                sel = CLAMP(sel, 1, MacroController::GetSlotCount());
                if (!MacroController::IsSlotEmpty(sel)) {
                    MacroController::SetCurrentSlot(sel);
                    if (MacroController::GetState() != MacroController::State::Replaying) {
                        LogOut("[AUTO-ACTION][MACRO] Starting macro playback (slot=" + std::to_string(sel) + ") for P2 (immediate)", true);
                        MacroController::Play();
                        return;
                    }
                }
            }
        }
        ApplyAutoAction(playerNum, moveIDAddr, 0, 0);
        LogOut(std::string("[AUTO-ACTION] Immediate apply done for P") + std::to_string(playerNum), detailedLogging.load());
    }
    // For delayed actions, use the existing delay system
    else {
        int internalFrames = VisualFramesToInternalTicks(delayFrames);
        
        if (playerNum == 1) {
            p1DelayState.isDelaying = true;
            p1DelayState.delayFramesRemaining = internalFrames;
            // triggerType already set above
            p1DelayState.pendingMoveID = 0;
            LogOut("[DELAY] Armed P1 delay: visualFrames=" + std::to_string(delayFrames) +
                   ", internalFrames=" + std::to_string(internalFrames) +
                   ", triggerType=" + std::to_string(triggerType) +
                   ", expiresAtInternal=" + std::to_string(frameCounter.load() + internalFrames),
                   true);
        } else {
            p2DelayState.isDelaying = true;
            p2DelayState.delayFramesRemaining = internalFrames;
            // triggerType already set above
            p2DelayState.pendingMoveID = 0;
            LogOut("[DELAY] Armed P2 delay: visualFrames=" + std::to_string(delayFrames) +
                   ", internalFrames=" + std::to_string(internalFrames) +
                   ", triggerType=" + std::to_string(triggerType) +
                   ", expiresAtInternal=" + std::to_string(frameCounter.load() + internalFrames),
                   true);
        }
    }
}

void ProcessTriggerDelays() {
    const PerFrameSample &sample = GetCurrentPerFrameSample();
    ProcessTriggerDelays(sample.moveID1, sample.moveID2, sample.prevMoveID1, sample.prevMoveID2);
}

void ProcessTriggerCooldowns() {
    // Avoid building strings unless diagnostic logging is enabled, and throttle to ~5s
    if (detailedLogging.load()) {
        static int s_nextCooldownDiagFrame = 0; // 5s throttle at 192 fps => 960 frames
        int now = frameCounter.load();
        if (now >= s_nextCooldownDiagFrame) {
            // LogOut("[COOLDOWN] p1Active=" + std::to_string(p1TriggerActive) +
            //        " p1CD=" + std::to_string(p1TriggerCooldown) +
            //        " | p2Active=" + std::to_string(p2TriggerActive) +
            //        " p2CD=" + std::to_string(p2TriggerCooldown), true);
            s_nextCooldownDiagFrame = now + 960;
        }
    }
    // Simplified cooldown progression (no pinning) to avoid deadlocks
    if (p1TriggerActive && p1TriggerCooldown > 0) {
        if (!p1DelayState.isDelaying) {
            p1TriggerCooldown--;
            if (p1TriggerCooldown <= 0) {
                p1TriggerActive = false;
                LogOut("[AUTO-ACTION] P1 trigger cooldown expired, new triggers allowed", detailedLogging.load());
            }
        }
    }
    if (p2TriggerActive && p2TriggerCooldown > 0) {
        if (!p2DelayState.isDelaying) {
            p2TriggerCooldown--;
            if (p2TriggerCooldown <= 0) {
                p2TriggerActive = false;
                LogOut("[AUTO-ACTION] P2 trigger cooldown expired, new triggers allowed", detailedLogging.load());
            }
        }
    }
}

// Core implementation that uses caller-provided move IDs for better cache locality
static void MonitorAutoActionsImpl(short moveID1, short moveID2, short prevMoveID1, short prevMoveID2) {
    static bool s_p2ForcedNeutral = false;
    // Only operate in offline Practice mode
    if (GetCurrentGameMode() != GameMode::Practice) return;
    if (IsNetplaySuspendActive()) return;
    
    // Fast-path out when there's definitively no work to do this frame
    if (!AutoActionWorkPending()) {
        return;
    }
    // Quiescent fast-path: if nothing is pending and both move IDs are unchanged, skip all work
    // This keeps idle CPU low when triggers are enabled but no transitions occur.
    // EXCEPTION: Never skip while P2 is in groundtech - we need to count ticks for wake macro timing!
    bool p2InGroundtech = IsGroundtech(moveID2);
    if (!p1DelayState.isDelaying && !p2DelayState.isDelaying &&
        !g_pendingControlRestore.load() && g_dashDeferred.pendingSel.load() == 0 &&
        !s_p1WakePrearmed && !s_p2WakePrearmed && !s_p1RGPrearmed && !s_p2RGPrearmed &&
        p1TriggerCooldown <= 0 && p2TriggerCooldown <= 0 &&
        s_p2WakeMacroTokenNeutralizeCountdown <= 0 &&
        !p2InGroundtech && // Don't skip during groundtech - need to count wake timing ticks
        moveID1 == prevMoveID1 && moveID2 == prevMoveID2) {
        return;
    }
    ProcessTriggerCooldowns();
    // (Deferred dash follow-ups handled after we have moveID context below)

    MaybeBeginFlowSequence(1, prevMoveID1, moveID1);
    MaybeBeginFlowSequence(2, prevMoveID2, moveID2);

    // Update timing trackers once per tick for both players
    UpdateStunTimersForPlayer(s_p1Timers, prevMoveID1, moveID1);
    UpdateStunTimersForPlayer(s_p2Timers, prevMoveID2, moveID2);

    // Note: Do NOT restore on RG exit; we must keep buffer-freeze active until the special actually begins.
    
    int targetPlayer = ResolveAutoActionTargetPlayer();
    // Throttle trigger diagnostics to ~5s intervals
    static int s_nextTrigDiagFrame = 0; // shared across P1/P2 logs
    auto canLogTrigDiag = [&]() -> bool {
        int now = frameCounter.load();
        if (now >= s_nextTrigDiagFrame) {
            s_nextTrigDiagFrame = now + 960; // 960 internal frames ~= 5 seconds @192fps
            return true;
        }
        return false;
    };
    
    // P1 triggers
    if ((targetPlayer == 1 || targetPlayer == 3) && !p1DelayState.isDelaying && !p1ActionApplied) {
        bool shouldTrigger = false;
        int triggerType = TRIGGER_NONE;
        int delay = 0;
        short actionMoveID = 0;        
        // Extra diagnostics for P1 trigger evaluation
    if (detailedLogging.load() && canLogTrigDiag()) {
            // LogOut("[TRIGGER_DIAG] P1 eval: prev=" + std::to_string(prevMoveID1) + 
            //            ", curr=" + std::to_string(moveID1) +
            //            ", trigActive=" + std::to_string(p1TriggerActive) +
            //            ", cooldown=" + std::to_string(p1TriggerCooldown) +
            //        ", actionable(prev/curr)=" + std::to_string(IsActionable(prevMoveID1)) + "/" + std::to_string(IsActionable(moveID1)), true);
        }
        
    if (!shouldTrigger && triggerAfterAirtechEnabled.load()) {
            // Check if player was in airtech last frame
            bool wasInAirtech = IsAirtech(prevMoveID1);
            // Post-airtech actionable: allow either general actionable states or explicit FALLING
            bool postAirtechNow = (!IsAirtech(moveID1)) && (IsActionable(moveID1) || moveID1 == FALLING_ID);
            if (detailedLogging.load() && canLogTrigDiag()) {
                // LogOut("[TRIGGER_DIAG] P1 AfterAirtech check: wasAirtech=" + std::to_string(wasInAirtech) +
                //        ", postAirtechNow=" + std::to_string(postAirtechNow) +
                //        ", targetPlayer=" + std::to_string(targetPlayer), true);
            }

            // P1 After-Airtech trigger condition
            if (wasInAirtech && postAirtechNow) {
                if (triggerRandomizeEnabled.load()) { if ((rand() & 1) == 0) { if (detailedLogging.load() && canLogTrigDiag()) LogOut("[AUTO-ACTION] P1 After Airtech skipped by random gate", true); goto p1_after_airtech_done; } }
                LogOut("[AUTO-ACTION] P1 After Airtech trigger activated (from moveID " + 
                       std::to_string(prevMoveID1) + " to " + std::to_string(moveID1) + ")", detailedLogging.load());
                shouldTrigger = true;
                triggerType = TRIGGER_AFTER_AIRTECH;
                delay = triggerAfterAirtechDelay.load();

                // Get the appropriate action moveID for After Airtech trigger
                int actionType = triggerAfterAirtechAction.load();
                actionMoveID = GetActionMoveID(actionType, TRIGGER_AFTER_AIRTECH, 1);
            }
        p1_after_airtech_done: ;
        }


        // After Block trigger
    if (!shouldTrigger && triggerAfterBlockEnabled.load()) {
            bool wasInBlockstun = IsBlockstun(prevMoveID1);
            bool nowNotInBlockstun = !IsBlockstun(moveID1);
            bool wasNotActionable = !IsActionable(prevMoveID1);
            bool isNowActionable = IsActionable(moveID1);
            bool justBecameActionable = wasNotActionable && isNowActionable;

            if (wasInBlockstun && nowNotInBlockstun && justBecameActionable) {
                if (triggerRandomizeEnabled.load()) { if ((rand() & 1) == 0) { if (detailedLogging.load() && canLogTrigDiag()) LogOut("[AUTO-ACTION] P1 After Block skipped by random gate", true); goto p1_after_block_done; } }
                shouldTrigger = true;
                triggerType = TRIGGER_AFTER_BLOCK;
                delay = triggerAfterBlockDelay.load();
                actionMoveID = GetActionMoveID(triggerAfterBlockAction.load(), TRIGGER_AFTER_BLOCK, 1);
                LogOut("[AUTO-ACTION] P1 After Block trigger activated (just became actionable)", true);
                if (detailedLogging.load()) {
                    LogOut("[AUTO-ACTION] P1 After Block trigger activated (just became actionable)", true);
                    LogOut("[TRIGGER_TIMING] P1 blockstun dur=" + std::to_string(s_p1Timers.lastBlockDuration) +
                           ", sinceEnd=" + std::to_string(s_p1Timers.sinceBlockEnd), true);
                }
            }
        p1_after_block_done: ;
        }

        // On RG trigger: arm at RG entry and schedule for RG stun end (stand/crouch/air specific)
    if (!shouldTrigger && triggerOnRGEnabled.load()) {
            bool enteredRG = (!IsRecoilGuard(prevMoveID1) && IsRecoilGuard(moveID1));
            if (enteredRG) {
        if (triggerRandomizeEnabled.load()) { if ((rand() & 1) == 0) { if (detailedLogging.load() && canLogTrigDiag()) LogOut("[AUTO-ACTION] P1 On RG skipped by random gate", true); goto p1_onrg_done; } }
                int baseDelayF = 20; // fallback in visual frames
                if (moveID1 == RG_STAND_ID) baseDelayF = RG_STAND_FREEZE_DEFENDER; // 20F
                else if (moveID1 == RG_CROUCH_ID) baseDelayF = RG_CROUCH_FREEZE_DEFENDER; // 22F
                else if (moveID1 == RG_AIR_ID) baseDelayF = RG_AIR_FREEZE_DEFENDER; // 22F
                int userDelayF = triggerOnRGDelay.load();
                int totalDelayF = baseDelayF + userDelayF;
                if (totalDelayF < 0) totalDelayF = 0; // do not allow negative
                LogOut("[TRIGGER_DIAG] P1 entered RG (" + std::to_string(moveID1) + ") baseDelay=" + std::to_string(baseDelayF) +
                       "F + user=" + std::to_string(userDelayF) + "F => total=" + std::to_string(totalDelayF) + "F", true);
                // Schedule execution exactly at RG stun end (converted to internal frames in StartTriggerDelay)
                StartTriggerDelay(1, TRIGGER_ON_RG, 0, totalDelayF);
                const int selectedUserDelayF = (p1DelayState.chosenDelay >= 0)
                    ? p1DelayState.chosenDelay
                    : userDelayF;

                // If user requested 0F delay and action is a special/FM, pre-arm now so motion exists on the first actionable frame
                if (selectedUserDelayF == 0 && !s_p1RGPrearmed) {
                    int at = (p1DelayState.chosenAction >= 0) ? p1DelayState.chosenAction : triggerOnRGAction.load();
                    int selectedStrength = (p1DelayState.chosenStrength >= 0)
                        ? p1DelayState.chosenStrength
                        : GetSpecialMoveStrength(at, TRIGGER_ON_RG);
                    int motion = ConvertTriggerActionToMotion(at, TRIGGER_ON_RG, selectedStrength);
                    int buttonMask = 0;
                    if ((at >= ACTION_5A && at <= ACTION_2D) || (at >= ACTION_6A && at <= ACTION_4D)) {
                        int button;
                        if (at >= ACTION_5A && at <= ACTION_2D) {
                            button = (at - ACTION_5A) % 4;
                        } else { // directional normals contiguous groups 6A..6D then 4A..4D
                            int offset = at - ACTION_6A;
                            button = offset % 4; // 0=A,1=B,2=C,3=D
                        }
                        buttonMask = (1 << (4 + button));
                    } else if (at >= ACTION_JA && at <= ACTION_JD) {
                        int button = (at - ACTION_JA) % 4;
                        buttonMask = (1 << (4 + button));
                    } else if (at >= ACTION_QCF && at <= ACTION_6321463214) {
                        buttonMask = (1 << (4 + CLAMP(selectedStrength, 0, 3)));
                    }
                    bool preOk = false;
                    bool isSpec = (motion >= MOTION_236A) || (at == ACTION_FINAL_MEMORY);
                    if (isSpec) {
                        if (at == ACTION_FINAL_MEMORY) {
                            int charId = displayData.p1CharID;
                            preOk = ExecuteFinalMemory(1, charId);
                            s_p1RGPrearmIsSpecial = true;
                        } else {
                            // Ensure buffer writes allowed
                            g_injectImmediateOnly[1].store(false);
                            preOk = FreezeBufferForMotion(1, motion, buttonMask);
                            s_p1RGPrearmIsSpecial = true;
                        }
                    }
                    if (preOk) {
                        s_p1RGPrearmed = true;
                        s_p1RGPrearmActionType = at;
                        s_p1RGPrearmExpiry = frameCounter.load() + (baseDelayF * 3) + 90; // small guard window after actionable
                        LogOut("[AUTO-ACTION][RG] P1 pre-armed at RG entry: action=" + std::to_string(at) +
                               " motion=" + std::to_string(motion) + " btnMask=" + std::to_string(buttonMask), true);
                    } else if (isSpec) {
                        LogOut("[AUTO-ACTION][RG] P1 pre-arm failed; will attempt on delay expiry", true);
                    }
                }
            }
        p1_onrg_done: ;
        }
        
        // After Hitstun trigger
    if (!shouldTrigger && triggerAfterHitstunEnabled.load()) {
            bool wasInHitstun = IsHitstun(prevMoveID1);
            bool nowNotInHitstun = !IsHitstun(moveID1);
            bool wasNotActionable = !IsActionable(prevMoveID1);
            bool isNowActionable = IsActionable(moveID1);
            bool justBecameActionable = wasNotActionable && isNowActionable;

            if (wasInHitstun && nowNotInHitstun && justBecameActionable) {
                if (triggerRandomizeEnabled.load()) { if ((rand() & 1) == 0) { if (detailedLogging.load() && canLogTrigDiag()) LogOut("[AUTO-ACTION] P1 After Hitstun skipped by random gate", true); goto p1_after_hitstun_done; } }
                shouldTrigger = true;
                triggerType = TRIGGER_AFTER_HITSTUN;
                delay = triggerAfterHitstunDelay.load();
                actionMoveID = GetActionMoveID(triggerAfterHitstunAction.load(), TRIGGER_AFTER_HITSTUN, 1);
                LogOut("[AUTO-ACTION] P1 After Hitstun trigger activated (just became actionable)", true);
                if (detailedLogging.load()) {
                    LogOut("[AUTO-ACTION] P1 After Hitstun trigger activated (just became actionable)", true);
                    LogOut("[TRIGGER_TIMING] P1 hitstun dur=" + std::to_string(s_p1Timers.lastHitDuration) +
                           ", sinceEnd=" + std::to_string(s_p1Timers.sinceHitEnd), true);
                }
            }
        p1_after_hitstun_done: ;
        }
        
        // On Wakeup pre-arm: record metadata when delay == 0 and action is a special/FM.
        // IMPORTANT: If a macro slot is configured for On Wakeup, prefer the generic
        // trigger->delay->macro path even at 0F so macros can be used for 0F wakeup
        // reversals. In that case we skip pre-arm entirely and let StartTriggerDelay
        // handle immediate macro playback on the first actionable wake frame.
    if (triggerOnWakeupEnabled.load() && !s_p1WakePrearmed) {
            if (moveID1 == GROUNDTECH_RECOVERY) {
                if (triggerRandomizeEnabled.load()) { if ((rand() & 1) == 0) { if (detailedLogging.load() && canLogTrigDiag()) LogOut("[AUTO-ACTION] P1 Wake prearm skipped by random gate", true); goto p1_wake_prearm_done; } }
                bool enteringGroundtechThisFrame = !IsGroundtech(prevMoveID1) && IsGroundtech(moveID1);
                if (enteringGroundtechThisFrame) {
                    s_p1WakeOptionPicked = false;
                    s_p1WakePrePickedOption = {};
                    if (PickRandomPoolOption(TRIGGER_ON_WAKEUP, 1, s_p1WakePrePickedOption)) {
                        s_p1WakeOptionPicked = true;
                        TraceAutoActionWakeEvent("p1-wake-pool-picked", 1, prevMoveID1, moveID1,
                                                 "delay=" + std::to_string(s_p1WakePrePickedOption.delay) +
                                                 " action=" + std::to_string(s_p1WakePrePickedOption.action) +
                                                 " strength=" + std::to_string(s_p1WakePrePickedOption.strength));
                    } else if (HasEnabledRows(TRIGGER_ON_WAKEUP)) {
                        if (PickRandomRow(TRIGGER_ON_WAKEUP, s_p1WakePrePickedOption)) {
                            s_p1WakeOptionPicked = true;
                            TraceAutoActionWakeEvent("p1-wake-option-picked", 1, prevMoveID1, moveID1,
                                                     "macroSlot=" + std::to_string(s_p1WakePrePickedOption.macroSlot) +
                                                     " delay=" + std::to_string(s_p1WakePrePickedOption.delay) +
                                                     " action=" + std::to_string(s_p1WakePrePickedOption.action) +
                                                     " strength=" + std::to_string(s_p1WakePrePickedOption.strength));
                        }
                    }
                }

                int userDelayF = s_p1WakeOptionPicked ? s_p1WakePrePickedOption.delay : triggerOnWakeupDelay.load();
                if (s_p1WakeOptionPicked) {
                    userDelayF = (userDelayF <= 1) ? 0 : (userDelayF - 1);
                }
                int actionType = s_p1WakeOptionPicked ? s_p1WakePrePickedOption.action : triggerOnWakeupAction.load();
                int chosenStrength = s_p1WakeOptionPicked ? s_p1WakePrePickedOption.strength : triggerOnWakeupStrength.load();
                chosenStrength = CLAMP(chosenStrength, 0, 3);
                int motionType = ConvertTriggerActionToMotion(actionType, TRIGGER_ON_WAKEUP, chosenStrength);
                // If a macro slot is selected for On Wakeup, avoid pre-arm so that
                // StartTriggerDelay can fire a macro (including 0F) instead. Allow
                // pre-arm to proceed if the configured slot is empty to prevent
                // silent no-ops.
                int macroSlot = s_p1WakeOptionPicked ? s_p1WakePrePickedOption.macroSlot : triggerOnWakeupMacroSlot.load();
                int macroSel = CLAMP(macroSlot, 1, MacroController::GetSlotCount());
                bool macroHasData = (macroSlot > 0) && MacroController::GetState() != MacroController::State::Recording && !MacroController::IsSlotEmpty(macroSel);
                bool isDash = (motionType == MOTION_FORWARD_DASH || motionType == MOTION_BACK_DASH);
                bool isSpecial = (actionType == ACTION_FINAL_MEMORY) || (motionType >= MOTION_236A);
                bool holdEligible = (userDelayF == 0) && !isSpecial && SupportsImmediateWakeHold(actionType);
                // Macros are now a supported pre-arm type (treated like specials for timing)
                bool isMacro = macroHasData && (userDelayF == 0);
                if (userDelayF == 0 && (isSpecial || holdEligible || isMacro)) {
                    s_p1WakePrearmed = true;
                    s_p1WakePrearmIsSpecial = isSpecial;
                    s_p1WakePrearmActionType = actionType;
                    s_p1WakePrearmStrength = chosenStrength;
                    s_p1WakePrearmExpiry = frameCounter.load() + 120;
                    s_p1WakeMoveID96FrameCount = 0;
                    s_p1WakeBufferFrozen = false;
                    s_p1WakeHoldPrimed = holdEligible;
                    s_p1WakeHoldIssued = false;
                    // Initialize macro state
                    s_p1WakeMacro96FrameCount = 0;
                    s_p1WakeMacroQueued = false;
                    s_p1WakeMacroTargetFrame = -1;
                    s_p1WakeMacroSlot = -1;
                    s_p1WakeMacroStartTick = 0;
                    const char* tag = isSpecial ? "special" : (isMacro ? "macro" : "hold");
                    LogOut(std::string("[AUTO-ACTION] P1 wake ") + tag + " metadata stored", true);
                    TraceAutoActionWakeEvent("p1-prearm-stored", 1, prevMoveID1, moveID1,
                                             "kind=" + std::string(tag) +
                                             " action=" + std::to_string(actionType) +
                                             " strength=" + std::to_string(s_p1WakePrearmStrength) +
                                             " delayF=" + std::to_string(userDelayF));
                    LogFlowSequenceEvent(1, "wake metadata stored", prevMoveID1, moveID1);
                } else if (detailedLogging.load()) {
                    LogOut("[AUTO-ACTION] P1 wake pre-arm skipped (delay>0 or unsupported action)", true);
                }
            p1_wake_prearm_done: ;
            }
        }

        // On Wakeup trigger (fallback if not pre-armed or for normals/jumps)
        bool p1BecameActionableFromGroundtech = IsGroundtech(prevMoveID1) && IsActionable(moveID1);
        if (s_p1WakePrearmed) {
            // Count logical frames (192Hz) in moveID 96, buffer on last rising frame
            if (moveID1 == GROUNDTECH_RECOVERY) {
                s_p1WakeMoveID96FrameCount++;
                s_p1WakeMacro96FrameCount++;  // Also increment macro frame counter (64Hz equivalent)
                // Get character-specific rising frame count
                int charID = displayData.p1CharID;
                int risingTicks = GetWakeupRisingTicks(charID);
                int bufferFrame = risingTicks - 1;  // Buffer on last frame before actionable
                // Allow 3-frame window for safety (buffer-3 to buffer)
                if (s_p1WakePrearmIsSpecial && !s_p1WakeBufferFrozen && 
                    s_p1WakeMoveID96FrameCount >= (bufferFrame - 2) && s_p1WakeMoveID96FrameCount <= bufferFrame) {
                    int at = s_p1WakePrearmActionType;
                    bool p1Facing = GetPlayerFacingDirection(1);
                    bool ok = ExecuteWakeSpecialNow(1, at, moveID1, s_p1WakePrearmStrength);
                    LogOut("[AUTO-ACTION] P1 wake special early buffer (frame " + std::to_string(s_p1WakeMoveID96FrameCount) + "/" + std::to_string(risingTicks) + ", charID=" + std::to_string(charID) + ", facing=" + (p1Facing?"right":"left") + ") " + std::string(ok?"ok":"fail"), true);
                    TraceAutoActionWakeEvent("p1-early-special-freeze", 1, prevMoveID1, moveID1,
                                             "count96=" + std::to_string(s_p1WakeMoveID96FrameCount) +
                                             " risingTicks=" + std::to_string(risingTicks) +
                                             " bufferFrame=" + std::to_string(bufferFrame) +
                                             " charID=" + std::to_string(charID) +
                                             " action=" + std::to_string(at) +
                                             " ok=" + std::to_string(ok));
                    LogFlowSequenceEvent(1, "wake early buffer", prevMoveID1, moveID1);
                    s_p1WakeBufferFrozen = ok;
                }
                
                // Macro pre-buffering for P1: calculate optimal playback start frame
                int macroSlot = s_p1WakeOptionPicked ? s_p1WakePrePickedOption.macroSlot : triggerOnWakeupMacroSlot.load();
                int userDelayF = s_p1WakeOptionPicked ? s_p1WakePrePickedOption.delay : triggerOnWakeupDelay.load();
                if (s_p1WakeOptionPicked) {
                    userDelayF = (userDelayF <= 1) ? 0 : (userDelayF - 1);
                }
                if (macroSlot > 0 && userDelayF == 0 && !s_p1WakeMacroQueued) {
                    int risingFrames = GetWakeupRisingFrames(charID); // Visual frames at 64Hz (13-39)
                    
                    // Find the FIRST attack button in the macro
                    int sel = CLAMP(macroSlot, 1, MacroController::GetSlotCount());
                    int firstButtonTick = MacroController::GetFirstButtonTick(sel);
                    
                    if (firstButtonTick < 0) {
                        // No attack button in macro, nothing to buffer
                        s_p1WakeMacroTargetFrame = -1;
                        s_p1WakeMacroSlot = -1;
                        s_p1WakeMacroStartTick = 0;
                    } else {
                        // Convert ticks to frames (ceil division to be safe)
                        int framesToButton = (firstButtonTick + 2) / 3;
                        
                    // We want the attack button pressed before actionable (inside buffer window)
                    // playStartFrame = when to start the macro during state 96
                    // kBufferMargin accounts for the buffer window + timing margin
                    // Tested with regular Mizuka and Kano as well as Mayu- this value seems to be fumbleproof so macros won't fail buffering
                    // For Mizuka, 623c(3 hits) 22c was used as a test, for Kano - 214236c 22c, for Mayu - 623c delay 22c -  all of these seems to be properly buffering on wakeup and having f1 invlunerability
                        constexpr int kBufferMargin = 12;
                        int playStartFrame = risingFrames - framesToButton - kBufferMargin;
                        if (playStartFrame < 1) playStartFrame = 1;
                        
                        // NOTE: Counter increments at 192Hz (ticks), so convert target to ticks
                        s_p1WakeMacroStartTick = 0; // Play from beginning
                        s_p1WakeMacroTargetFrame = playStartFrame * 3; // Convert visual frames to ticks
                        s_p1WakeMacroSlot = macroSlot;
                        
                        if (detailedLogging.load() && s_p1WakeMacro96FrameCount == 1) {
                            LogOut("[AUTO-ACTION][MACRO-TIMING-INIT] P1 charID=" + std::to_string(charID) +
                                   " risingFrames=" + std::to_string(risingFrames) +
                                   " firstBtnTick=" + std::to_string(firstButtonTick) +
                                   " framesToBtn=" + std::to_string(framesToButton) +
                                   " playStartFrame=" + std::to_string(playStartFrame), true);
                        }
                        
                        // Start macro playback at calculated frame
                        if (s_p1WakeMacro96FrameCount >= s_p1WakeMacroTargetFrame) {
                            if (MacroController::GetState() != MacroController::State::Recording) {
                                if (!MacroController::IsSlotEmpty(sel)) {
                                    MacroController::SetCurrentSlot(sel);
                                    LogOut("[AUTO-ACTION][MACRO] P1 Pre-buffering wake macro (slot=" + std::to_string(sel) +
                                           ") at frame " + std::to_string(s_p1WakeMacro96FrameCount) +
                                           " (target=" + std::to_string(playStartFrame) + ")", true);
                                    TraceAutoActionWakeEvent("p1-wake-macro-play", 1, prevMoveID1, moveID1,
                                                             "slot=" + std::to_string(sel) +
                                                             " count96=" + std::to_string(s_p1WakeMacro96FrameCount) +
                                                             " targetTick=" + std::to_string(s_p1WakeMacroTargetFrame));
                                    MacroController::Play();
                                    s_p1WakeMacroQueued = true;
                                    s_p1WakeMacroTargetFrame = -1;
                                    s_p1WakeMacroSlot = -1;
                                    s_p1WakeBufferFrozen = true; // Mark as buffered to skip fallback path
                                }
                            }
                        }
                    }
                }
            }
            // Check for 96→0 transition (must be outside the moveID==96 block!)
            if (!s_p1WakePrearmIsSpecial && s_p1WakeHoldPrimed && !s_p1WakeHoldIssued) {
                const bool leavingGroundtechThisFrame = (prevMoveID1 == GROUNDTECH_RECOVERY) && (moveID1 != GROUNDTECH_RECOVERY);
                if (leavingGroundtechThisFrame) {
                    if (IssueWakeImmediateHold(1, s_p1WakePrearmActionType)) {
                        s_p1WakeHoldIssued = true;
                        TraceAutoActionWakeEvent("p1-wake-hold-issued", 1, prevMoveID1, moveID1,
                                                 "action=" + std::to_string(s_p1WakePrearmActionType));
                    }
                }
            }
            bool actionableButLostGround = !IsGroundtech(prevMoveID1) && IsActionable(moveID1) && !IsGroundtech(moveID1);
            if (actionableButLostGround && !s_p1WakeActionableWarning) {
                LogOut("[AUTO-ACTION][FLOW] P1 wake actionable while prev state not groundtech (prev=" + std::to_string(prevMoveID1) +
                       ", curr=" + std::to_string(moveID1) + ")", true);
                LogFlowSequenceEvent(1, "wake actionable lost-ground", prevMoveID1, moveID1);
                s_p1WakeActionableWarning = true;
            } else if (!actionableButLostGround) {
                s_p1WakeActionableWarning = false;
            }
        } else {
            s_p1WakeActionableWarning = false;
        }

        if (!shouldTrigger && triggerOnWakeupEnabled.load()) {
            // If special was already frozen during moveID 96, just clear state on actionable
            if (s_p1WakePrearmed && s_p1WakeBufferFrozen && p1BecameActionableFromGroundtech) {
                LogFlowSequenceEvent(1, "wake exec (buffered early)", prevMoveID1, moveID1);
                p1TriggerActive = false;
                p1TriggerCooldown = 0;
                s_p1WakePrearmed = false;
                s_p1WakePrearmIsSpecial = false;
                s_p1WakePrearmActionType = -1;
                s_p1WakePrearmStrength = -1;
                s_p1WakeMoveID96FrameCount = 0;
                s_p1WakeBufferFrozen = false;
                s_p1WakeHoldPrimed = false;
                s_p1WakeHoldIssued = false;
                s_p1WakeMacro96FrameCount = 0;
                s_p1WakeMacroQueued = false;
                s_p1WakeMacroTargetFrame = -1;
                s_p1WakeMacroSlot = -1;
                s_p1WakeOptionPicked = false;
                s_p1WakePrePickedOption = {};
            } else if (s_p1WakePrearmed && !s_p1WakePrearmIsSpecial && s_p1WakeHoldIssued && p1BecameActionableFromGroundtech) {
                LogOut("[AUTO-ACTION] P1 wake hold exec (inputs maintained through wake)", true);
                LogFlowSequenceEvent(1, "wake hold exec", prevMoveID1, moveID1);
                s_p1WakePrearmed = false;
                s_p1WakePrearmActionType = -1;
                s_p1WakePrearmStrength = -1;
                s_p1WakeMoveID96FrameCount = 0;
                s_p1WakeBufferFrozen = false;
                s_p1WakeHoldPrimed = false;
                s_p1WakeHoldIssued = false;
                s_p1WakeMacro96FrameCount = 0;
                s_p1WakeMacroQueued = false;
                s_p1WakeMacroTargetFrame = -1;
                s_p1WakeMacroSlot = -1;
                s_p1WakeOptionPicked = false;
                s_p1WakePrePickedOption = {};
            } else if (s_p1WakePrearmed && !s_p1WakeBufferFrozen && p1BecameActionableFromGroundtech) {
                AutoActionLogScope wakeScope("WakeImmediateExec", 1, TRIGGER_ON_WAKEUP);
                LogOut("[AUTO-ACTION] P1 wake exec (fallback): action=" + std::to_string(s_p1WakePrearmActionType) +
                       " special=" + std::to_string(s_p1WakePrearmIsSpecial) +
                       " frame=" + std::to_string(frameCounter.load()), true);
                    TraceAutoActionWakeEvent("p1-wake-fallback", 1, prevMoveID1, moveID1,
                                             "action=" + std::to_string(s_p1WakePrearmActionType) +
                                             " special=" + std::to_string(s_p1WakePrearmIsSpecial));
                    LogFlowSequenceEvent(1, "wake exec attempt", prevMoveID1, moveID1);
                    g_lastActiveTriggerType.store(TRIGGER_ON_WAKEUP);
                    g_lastActiveTriggerFrame.store(frameCounter.load());
                    if (s_p1WakePrearmActionType >= 0) {
                    int at = s_p1WakePrearmActionType;
                    if (s_p1WakePrearmIsSpecial) {
                        bool ok = ExecuteWakeSpecialNow(1, at, moveID1, s_p1WakePrearmStrength);
                        LogOut(std::string("[AUTO-ACTION] P1 wake special deferred inject ") + (ok?"ok":"fail"), true);
                        p1TriggerActive = false; p1TriggerCooldown = 0;
                    } else {
                        int motionType = ConvertTriggerActionToMotion(at, TRIGGER_ON_WAKEUP);
                        int buttonMask = ResolveButtonMaskForAction(at, TRIGGER_ON_WAKEUP, s_p1WakePrearmStrength);
                        if (at == ACTION_BACKDASH || at == ACTION_FORWARD_DASH) {
                            ResetPlayerInputBufferIndex(1);
                            QueueMotionInput(1, motionType, 0);
                        } else if ((motionType >= MOTION_5A && motionType <= MOTION_5D) || (motionType >= MOTION_6A && motionType <= MOTION_4D)) {
                            ImmediateInput::PressFor(1, buttonMask, 2);
                        } else if (motionType >= MOTION_2A && motionType <= MOTION_2C) {
                            ImmediateInput::PressFor(1, GAME_INPUT_DOWN | buttonMask, 2);
                        } else if (motionType >= MOTION_236A) {
                            if (buttonMask == 0) {
                                int atStrength = (s_p1WakePrearmStrength >= 0) ? s_p1WakePrearmStrength : GetSpecialMoveStrength(at, TRIGGER_ON_WAKEUP);
                                buttonMask = (1 << (4 + atStrength));
                            }
                            FreezeBufferForMotion(1, motionType, buttonMask);
                            { std::stringstream bm; bm << std::hex << buttonMask; LogOut(std::string("[AUTO-ACTION] P1 wake special frame1 buffer applied (index frozen) btnMask=0x") + bm.str(), true); }
                        }
                    }
                }
                s_p1WakePrearmed = false;
                s_p1WakePrearmIsSpecial = false;
                s_p1WakePrearmActionType = -1;
                s_p1WakePrearmStrength = -1;
                s_p1WakeMoveID96FrameCount = 0;
                s_p1WakeBufferFrozen = false;
                s_p1WakeHoldPrimed = false;
                s_p1WakeHoldIssued = false;
                s_p1WakeMacro96FrameCount = 0;
                s_p1WakeMacroQueued = false;
                s_p1WakeMacroTargetFrame = -1;
                s_p1WakeMacroSlot = -1;
                s_p1WakeOptionPicked = false;
                s_p1WakePrePickedOption = {};
            }
            if (!s_p1WakePrearmed && IsGroundtech(prevMoveID1) && IsActionable(moveID1)) {
                if (triggerRandomizeEnabled.load()) { if ((rand() & 1) == 0) { if (detailedLogging.load() && canLogTrigDiag()) LogOut("[AUTO-ACTION] P1 On Wakeup skipped by random gate", true); goto p1_wakeup_done; } }
                shouldTrigger = true;
                triggerType = TRIGGER_ON_WAKEUP;
                {
                    int userDelay = s_p1WakeOptionPicked ? s_p1WakePrePickedOption.delay : triggerOnWakeupDelay.load();
                    delay = (userDelay <= 1) ? 0 : (userDelay - 1);
                }
                int actionForMove = s_p1WakeOptionPicked ? s_p1WakePrePickedOption.action : triggerOnWakeupAction.load();
                actionMoveID = GetActionMoveID(actionForMove, TRIGGER_ON_WAKEUP, 1);
                
                LogOut("[AUTO-ACTION] P1 On Wakeup trigger activated", true);
                TraceAutoActionWakeEvent("p1-wakeup-trigger", 1, prevMoveID1, moveID1,
                                         "delayF=" + std::to_string(delay) +
                                         " actionMoveID=" + std::to_string(actionMoveID));
                if (detailedLogging.load()) {
                    LogOut("[TRIGGER_TIMING] P1 wake delay=" + std::to_string(s_p1Timers.lastWakeDelay) +
                           ", sinceWake=" + std::to_string(s_p1Timers.sinceWake), true);
                }
            }
        p1_wakeup_done: ;
        }
        
        // Apply the trigger if any condition was met
        if (shouldTrigger) {
            LogFlowSequenceEvent(1, "trigger delay", prevMoveID1, moveID1);
            AutoActionLogScope seqScope("TriggerSequence", 1, triggerType);
            StartTriggerDelay(1, triggerType, actionMoveID, delay);
        }
    }
    
    // P2 triggers - apply same fix
    if ((targetPlayer == 2 || targetPlayer == 3) && !p2DelayState.isDelaying && !p2ActionApplied) {
        bool shouldTrigger = false;
        int triggerType = TRIGGER_NONE;
        int delay = 0;
        short actionMoveID = 0;
        // Extra diagnostics for P2 trigger evaluation
    if (detailedLogging.load() && canLogTrigDiag()) {
            LogOut("[TRIGGER_DIAG] P2 eval: prev=" + std::to_string(prevMoveID2) + 
                       ", curr=" + std::to_string(moveID2) +
                       ", trigActive=" + std::to_string(p2TriggerActive) +
                       ", cooldown=" + std::to_string(p2TriggerCooldown) +
                   ", actionable(prev/curr)=" + std::to_string(IsActionable(prevMoveID2)) + "/" + std::to_string(IsActionable(moveID2)), true);
        }
        
         // CRITICAL FIX: Complete implementation of After Airtech trigger for P2
    if (!shouldTrigger && triggerAfterAirtechEnabled.load()) {
            // CRITICAL: Don't fire if control restore is pending from previous action
            bool restorePending = g_pendingControlRestore.load();
            // Check for transition from airtech to actionable state
            bool wasAirtech = IsAirtech(prevMoveID2);
            // Post-airtech actionable: allow either general actionable states or explicit FALLING
            bool postAirtechNow = (!IsAirtech(moveID2)) && (IsActionable(moveID2) || moveID2 == FALLING_ID);
            if (detailedLogging.load() && canLogTrigDiag()) {
                LogOut("[TRIGGER_DIAG] P2 AfterAirtech check: wasAirtech=" + std::to_string(wasAirtech) +
                       ", postAirtechNow=" + std::to_string(postAirtechNow) +
                       ", restorePending=" + std::to_string(restorePending) +
                       ", targetPlayer=" + std::to_string(targetPlayer), true);
            }

            if (!restorePending && wasAirtech && postAirtechNow) {
                if (triggerRandomizeEnabled.load()) { if ((rand() & 1) == 0) { if (detailedLogging.load() && canLogTrigDiag()) LogOut("[AUTO-ACTION] P2 After Airtech skipped by random gate", true); goto p2_after_airtech_done; } }
                LogOut("[AUTO-ACTION] P2 After Airtech trigger activated", detailedLogging.load());
                shouldTrigger = true;
                triggerType = TRIGGER_AFTER_AIRTECH;
        // Pass visual frames; StartTriggerDelay converts to internal frames
        delay = triggerAfterAirtechDelay.load();
                actionMoveID = GetActionMoveID(triggerAfterAirtechAction.load(), TRIGGER_AFTER_AIRTECH, 2);
            }
        p2_after_airtech_done: ;
        }
    // Simplified P2 After Block trigger
    if (!shouldTrigger && triggerAfterBlockEnabled.load()) {
        // Suppress After Block trigger if a forward dash follow-up is already pending but not yet fired
        bool dashFollowPending = (g_dashDeferred.pendingSel.load() > 0);
        // CRITICAL: Also suppress if a control restore is pending (we're still executing previous action)
        bool restorePending = g_pendingControlRestore.load();
        if (dashFollowPending && detailedLogging.load() && canLogTrigDiag()) {
            LogOut("[TRIGGER_DIAG] Suppressing P2 After Block: dash follow-up pending", true);
        }
        if (restorePending && detailedLogging.load() && canLogTrigDiag()) {
            LogOut("[TRIGGER_DIAG] Suppressing P2 After Block: control restore pending", true);
        }
        if (!dashFollowPending && !restorePending && IsBlockstun(prevMoveID2) && IsActionable(moveID2) && !p2TriggerActive && p2TriggerCooldown <= 0) {
            if (triggerRandomizeEnabled.load()) { if ((rand() & 1) == 0) { if (detailedLogging.load() && canLogTrigDiag()) LogOut("[AUTO-ACTION] P2 After Block skipped by random gate", true); goto p2_after_block_done; } }
            shouldTrigger = true;
            triggerType = TRIGGER_AFTER_BLOCK;
            delay = triggerAfterBlockDelay.load();
            actionMoveID = GetActionMoveID(triggerAfterBlockAction.load(), TRIGGER_AFTER_BLOCK, 2);
            LogOut("[AUTO-ACTION] P2 After Block trigger", true);
        }
    p2_after_block_done: ;
    }

    // P2: On RG trigger at RG entry (independent of After Block): schedule for RG stun end
    if (!shouldTrigger && triggerOnRGEnabled.load()) {
        bool enteredRG2 = (!IsRecoilGuard(prevMoveID2) && IsRecoilGuard(moveID2));
        if (enteredRG2) {
            if (triggerRandomizeEnabled.load()) { if ((rand() & 1) == 0) { if (detailedLogging.load() && canLogTrigDiag()) LogOut("[AUTO-ACTION] P2 On RG skipped by random gate", true); goto p2_onrg_done; } }
            int baseDelayF = 20;
            if (moveID2 == RG_STAND_ID) baseDelayF = RG_STAND_FREEZE_DEFENDER;
            else if (moveID2 == RG_CROUCH_ID) baseDelayF = RG_CROUCH_FREEZE_DEFENDER;
            else if (moveID2 == RG_AIR_ID) baseDelayF = RG_AIR_FREEZE_DEFENDER;
            int userDelayF = triggerOnRGDelay.load();
            int totalDelayF = baseDelayF + userDelayF;
            if (totalDelayF < 0) totalDelayF = 0;
            LogOut("[TRIGGER_DIAG] P2 entered RG (" + std::to_string(moveID2) + ") baseDelay=" + std::to_string(baseDelayF) +
                   "F + user=" + std::to_string(userDelayF) + "F => total=" + std::to_string(totalDelayF) + "F", true);
            StartTriggerDelay(2, TRIGGER_ON_RG, 0, totalDelayF);
            const int selectedUserDelayF = (p2DelayState.chosenDelay >= 0)
                ? p2DelayState.chosenDelay
                : userDelayF;

            // If user delay is 0 and action is special/FM, pre-arm now; ensure P2 control override for buffer progression
            if (selectedUserDelayF == 0 && !s_p2RGPrearmed) {
                int at = (p2DelayState.chosenAction >= 0) ? p2DelayState.chosenAction : triggerOnRGAction.load();
                int selectedStrength = (p2DelayState.chosenStrength >= 0)
                    ? p2DelayState.chosenStrength
                    : GetSpecialMoveStrength(at, TRIGGER_ON_RG);
                int motion = ConvertTriggerActionToMotion(at, TRIGGER_ON_RG, selectedStrength);
                int buttonMask = 0;
                if ((at >= ACTION_5A && at <= ACTION_2D) || (at >= ACTION_6A && at <= ACTION_4D)) {
                    int button = (at >= ACTION_5A && at <= ACTION_2D) ? ((at - ACTION_5A) % 4) : ((at - ACTION_6A) % 4);
                    buttonMask = (1 << (4 + button));
                } else if (at >= ACTION_JA && at <= ACTION_JD) {
                    int button = (at - ACTION_JA) % 4;
                    buttonMask = (1 << (4 + button));
                } else if (at >= ACTION_QCF && at <= ACTION_6321463214) {
                    buttonMask = (1 << (4 + CLAMP(selectedStrength, 0, 3)));
                }
                bool preOk = false;
                bool isSpec = (motion >= MOTION_236A) || (at == ACTION_FINAL_MEMORY);
                if (isSpec) {
                    EnableP2ControlForAutoAction();
                    if (at == ACTION_FINAL_MEMORY) {
                        int charId = displayData.p2CharID;
                        preOk = ExecuteFinalMemory(2, charId);
                        s_p2RGPrearmIsSpecial = true;
                        // longer timeout for FM sequences
                        g_lastP2MoveID.store(moveID2);
                        g_pendingControlRestore.store(true);
                        g_pendingRestoreTimestamp.store(GetTickCount());
                    } else {
                        // Allow buffer writes
                        g_injectImmediateOnly[2].store(false);
                        preOk = FreezeBufferForMotion(2, motion, buttonMask);
                        s_p2RGPrearmIsSpecial = true;
                        g_lastP2MoveID.store(moveID2);
                        g_pendingControlRestore.store(true);
                        g_pendingRestoreTimestamp.store(GetTickCount());
                    }
                }
                if (preOk) {
                    s_p2RGPrearmed = true;
                    s_p2RGPrearmActionType = at;
                    s_p2RGPrearmExpiry = frameCounter.load() + (baseDelayF * 3) + 90;
                    LogOut("[AUTO-ACTION][RG] P2 pre-armed at RG entry: action=" + std::to_string(at) +
                           " motion=" + std::to_string(motion) + " btnMask=" + std::to_string(buttonMask) +
                           " (control overridden)", true);
                    // Enable fast restore so we hand back control as soon as the move actually starts
                    if (g_counterRGEnabled.load()) {
                        g_crgFastRestore.store(true);
                    }
                } else if (isSpec) {
                    LogOut("[AUTO-ACTION][RG] P2 pre-arm failed; will attempt on delay expiry", true);
                }
            }
        }
    p2_onrg_done: ;
    }
        
        // After Hitstun trigger
    if (!shouldTrigger && triggerAfterHitstunEnabled.load()) {
            // CRITICAL: Don't fire if control restore is pending from previous action
            bool restorePending = g_pendingControlRestore.load();
            if (restorePending && detailedLogging.load() && canLogTrigDiag()) {
                LogOut("[TRIGGER_DIAG] Suppressing P2 After Hitstun: control restore pending", true);
            }
                if (!restorePending && IsHitstun(prevMoveID2) && !IsHitstun(moveID2) && !IsAirtech(moveID2)) {
                if (IsActionable(moveID2)) {
                        if (triggerRandomizeEnabled.load()) { if ((rand() & 1) == 0) { if (detailedLogging.load() && canLogTrigDiag()) LogOut("[AUTO-ACTION] P2 After Hitstun skipped by random gate", true); goto p2_after_hitstun_done; } }
                    shouldTrigger = true;
                    triggerType = TRIGGER_AFTER_HITSTUN;
                    delay = triggerAfterHitstunDelay.load();
                    actionMoveID = GetActionMoveID(triggerAfterHitstunAction.load(), TRIGGER_AFTER_HITSTUN, 2);
                    LogOut("[AUTO-ACTION] P2 after hitstun trigger activated", true);
                    if (detailedLogging.load()) {
                        LogOut("[TRIGGER_TIMING] P2 hitstun dur=" + std::to_string(s_p2Timers.lastHitDuration) +
                               ", sinceEnd=" + std::to_string(s_p2Timers.sinceHitEnd), true);
                    }
                }
            }
        p2_after_hitstun_done: ;
        }
        
        // On Wakeup handling for P2.
        // DEBUG: Always log wake block entry check
    if (detailedLogging.load()) {
        // LogOut("[AUTO-ACTION][WAKE-ENTRY-CHECK] wakeEnabled=" + std::to_string(triggerOnWakeupEnabled.load()) +
        //        " preArmed=" + std::to_string(s_p2WakePrearmed) +
        //        " move2=" + std::to_string(moveID2) +
        //        " prev2=" + std::to_string(prevMoveID2), true);
    }

    // Dedicated path for pre-armed wake macros (uses wake-buffering timing only).
    // This runs independently of the generic wake pre-arm machinery so that
    // macros never interfere with or depend on the special/hold pre-arm flags.
    // Note: We track the entire groundtech sequence (PRE/START/END/RECOVERY),
    // not just the final GROUNDTECH_RECOVERY state, so that longer macros
    // have enough time to be pre-buffered before the character becomes
    // actionable.
    bool isInGroundtech = IsGroundtech(moveID2);
    bool wakeBlockEnabled = triggerOnWakeupEnabled.load() && g_wakeBufferingEnabled.load();
    
    // Wake state logging (disabled - uncomment for debugging)
    // if (detailedLogging.load()) {
    //     LogOut("[AUTO-ACTION][WAKE-STATE] moveID2=" + std::to_string(moveID2) +
    //            " prevMoveID2=" + std::to_string(prevMoveID2) +
    //            " isGroundtech=" + std::to_string(isInGroundtech) +
    //            " counter=" + std::to_string(s_p2WakeMacro96FrameCount) +
    //            " target=" + std::to_string(s_p2WakeMacroTargetFrame) +
    //            " queued=" + std::to_string(s_p2WakeMacroQueued) +
    //            " slot=" + std::to_string(s_p2WakeMacroSlot), true);
    // }
    
    if (wakeBlockEnabled && isInGroundtech) {
        // Track frames during any groundtech state for potential macro pre-buffering.
        // On first frame entering groundtech, pre-pick the option row to get macro slot early.
        bool enteringGroundtechThisFrame = !IsGroundtech(prevMoveID2) && IsGroundtech(moveID2);
        if (enteringGroundtechThisFrame) {
            s_p2WakeMacro96FrameCount = 0;
            s_p2WakeMacroQueued = false;
            s_p2WakeMacroTargetFrame = -1;
            s_p2WakeMacroSlot = -1;
            s_p2WakeMacroStartFrame = -1;
            s_p2WakeMacroStartTick = 0;
            s_p2WakeOptionPicked = false;
            TraceAutoActionWakeEvent("p2-wake-macro-groundtech-enter", 2, prevMoveID2, moveID2);
            // if (detailedLogging.load()) {
            //     LogOut("[AUTO-ACTION][MACRO-COUNTER] FIRST FRAME in groundtech, prev=" + std::to_string(prevMoveID2) + " counter=0", true);
            // }
            // Pre-pick a row now so we know macro slot for pre-buffering timing
            if (PickRandomPoolOption(TRIGGER_ON_WAKEUP, 2, s_p2WakePrePickedOption)) {
                s_p2WakeOptionPicked = true;
                TraceAutoActionWakeEvent("p2-wake-pool-picked", 2, prevMoveID2, moveID2,
                                         "delay=" + std::to_string(s_p2WakePrePickedOption.delay) +
                                         " action=" + std::to_string(s_p2WakePrePickedOption.action) +
                                         " strength=" + std::to_string(s_p2WakePrePickedOption.strength));
                if (detailedLogging.load()) {
                    LogOut("[AUTO-ACTION] P2 wake pool pre-picked option: action=" +
                           std::to_string(s_p2WakePrePickedOption.action) +
                           " strength=" + std::to_string(s_p2WakePrePickedOption.strength), true);
                }
            } else if (HasEnabledRows(TRIGGER_ON_WAKEUP)) {
                if (PickRandomRow(TRIGGER_ON_WAKEUP, s_p2WakePrePickedOption)) {
                    s_p2WakeOptionPicked = true;
                    TraceAutoActionWakeEvent("p2-wake-option-picked", 2, prevMoveID2, moveID2,
                                             "macroSlot=" + std::to_string(s_p2WakePrePickedOption.macroSlot) +
                                             " delay=" + std::to_string(s_p2WakePrePickedOption.delay) +
                                             " action=" + std::to_string(s_p2WakePrePickedOption.action) +
                                             " strength=" + std::to_string(s_p2WakePrePickedOption.strength));
                    if (detailedLogging.load()) {
                        LogOut("[AUTO-ACTION] P2 wake pre-picked option: macroSlot=" + std::to_string(s_p2WakePrePickedOption.macroSlot) +
                               " delay=" + std::to_string(s_p2WakePrePickedOption.delay) +
                               " action=" + std::to_string(s_p2WakePrePickedOption.action), true);
                    }
                }
            }
        } else {
            // Only count ticks when in GROUNDTECH_RECOVERY (state 96), not the
            // earlier groundtech states (97=PRE, 98=START, 99=END). The wakeup
            // timing calculation uses the state 96 duration specifically.
            if (moveID2 == GROUNDTECH_RECOVERY) {
                ++s_p2WakeMacro96FrameCount;
                // if (detailedLogging.load()) {
                //     LogOut("[AUTO-ACTION][MACRO-COUNTER] tick=" + std::to_string(s_p2WakeMacro96FrameCount) +
                //            " prev=" + std::to_string(prevMoveID2) + " curr=" + std::to_string(moveID2), true);
                // }
            }
            // Logging for pre-recovery states disabled
            // else if (detailedLogging.load()) {
            //     LogOut("[AUTO-ACTION][MACRO-COUNTER] pre-recovery state prev=" + std::to_string(prevMoveID2) + 
            //            " curr=" + std::to_string(moveID2) + " (not counting)", true);
            // }
        }

        // Determine macro slot and delay: prefer pre-picked option, fallback to global.
        int macroSlot = s_p2WakeOptionPicked ? s_p2WakePrePickedOption.macroSlot : triggerOnWakeupMacroSlot.load();
        s_p2WakeMacroSlot = macroSlot;
        int userDelayF = s_p2WakeOptionPicked ? s_p2WakePrePickedOption.delay : triggerOnWakeupDelay.load();
        // For wake trigger, delay mapping: 0/1 => 0, else (n-1)
        if (s_p2WakeOptionPicked) {
            userDelayF = (userDelayF <= 1) ? 0 : (userDelayF - 1);
        }

        // Only care about macros in this path.
        int macroSel = CLAMP(macroSlot, 1, MacroController::GetSlotCount());
        bool macroSlotSelected = (macroSlot > 0);
        bool macroHasData = macroSlotSelected &&
                            MacroController::GetState() != MacroController::State::Recording &&
                            !MacroController::IsSlotEmpty(macroSel);

        // When wake buffering is enabled and a macro slot is configured with 0F delay,
        // compute a target frame during GROUNDTECH_RECOVERY so that the first attack
        // button lands in the wake buffer window.
        //
        // State 96 (GROUNDTECH_RECOVERY) lasts the full wakeup duration (13-39 visual frames
        // depending on character, as defined in s_wakeupTimings).
        //
        // Strategy: We want the first attack button to be pressed ~3 frames BEFORE actionable
        // so it lands inside the ~10 frame buffer window and executes immediately on wakeup.
        //
        // Formula: playStartFrame = risingFrames - framesToButton - bufferMargin
        // where framesToButton = ceil(firstButtonTick / 3) = how many frames until button in macro
        //       bufferMargin = 3 frames (ensures button lands inside buffer window)
        if (macroHasData && userDelayF == 0) {
            int charID = displayData.p2CharID;
            int risingFrames = GetWakeupRisingFrames(charID); // Visual frames at 64Hz (13-39)
            int firstButtonTick = MacroController::GetFirstButtonTick(macroSel);
            
            if (firstButtonTick < 0) {
                // No attack button in macro, nothing to buffer
                s_p2WakeMacroTargetFrame = -1;
                s_p2WakeMacroStartTick = 0;
            } else {
                // Convert ticks to frames (ceil division to be safe)
                int framesToButton = (firstButtonTick + 2) / 3;
                
                // We want the attack button pressed before actionable (inside buffer window)
                // playStartFrame = when to start the macro during state 96
                // kBufferMargin accounts for the buffer window + timing margin
                // Tested with regular Mizuka and Kano as well as Mayu- this value seems to be fumbleproof so macros won't fail buffering
                 // For Mizuka, 623c(3 hits) 22c was used as a test, for Kano - 214236c 22c, for Mayu - 623c delay 22c -  all of these seems to be properly buffering on wakeup and having f1 invlunerability
                constexpr int kBufferMargin = 12;
                int playStartFrame = risingFrames - framesToButton - kBufferMargin;
                if (playStartFrame < 1) playStartFrame = 1;
                
                // For this approach, we play from tick 0 (full macro) at the calculated frame
                // NOTE: Counter increments at 192Hz (ticks), so convert target to ticks
                s_p2WakeMacroStartTick = 0;
                s_p2WakeMacroTargetFrame = playStartFrame * 3; // Convert visual frames to ticks
                
                if (detailedLogging.load() && s_p2WakeMacro96FrameCount == 0) {
                    LogOut("[AUTO-ACTION][MACRO-TIMING-INIT] P2 charID=" + std::to_string(charID) +
                           " risingFrames=" + std::to_string(risingFrames) +
                           " firstBtnTick=" + std::to_string(firstButtonTick) +
                           " framesToBtn=" + std::to_string(framesToButton) +
                           " playStartFrame=" + std::to_string(playStartFrame), true);
                }
                if (s_p2WakeMacro96FrameCount == 0) {
                    TraceAutoActionWakeEvent("p2-wake-macro-target", 2, prevMoveID2, moveID2,
                                             "charID=" + std::to_string(charID) +
                                             " risingFrames=" + std::to_string(risingFrames) +
                                             " firstButtonTick=" + std::to_string(firstButtonTick) +
                                             " framesToButton=" + std::to_string(framesToButton) +
                                             " targetTick=" + std::to_string(s_p2WakeMacroTargetFrame) +
                                             " playStartFrame=" + std::to_string(playStartFrame));
                }
            }
        } else if (s_p2WakeMacro96FrameCount == 0) {
            // No valid wake pre-buffer for this 96 cycle
            s_p2WakeMacroTargetFrame = -1;
            s_p2WakeMacroStartTick = 0;
        }

        // Force Neutral while waiting for macro to prevent AI inputs (e.g. holding Down)
        // from causing accidental transitions (e.g. Crouch) before the macro starts.
        if (!s_p2WakeMacroQueued && s_p2WakeMacroSlot > 0) {
             bool macroPlaying = (MacroController::GetState() == MacroController::State::Replaying);
             if (!macroPlaying) {
                 g_manualInputMask[2].store(0, std::memory_order_relaxed);
                 g_manualInputOverride[2].store(true, std::memory_order_relaxed);
                 g_injectImmediateOnly[2].store(true, std::memory_order_relaxed);
                 s_p2ForcedNeutral = true;
             }
        }

        // Macro pre-buffering for P2: if we have a computed target frame,
        // start playback once the counter reaches that frame.
        if (!s_p2WakeMacroQueued && s_p2WakeMacroSlot > 0 && s_p2WakeMacroTargetFrame >= 0) {
            // if (detailedLogging.load()) {
            //     LogOut("[AUTO-ACTION][MACRO-CHECK] counter=" + std::to_string(s_p2WakeMacro96FrameCount) +
            //            " target=" + std::to_string(s_p2WakeMacroTargetFrame) +
            //            " reached=" + std::to_string(s_p2WakeMacro96FrameCount >= s_p2WakeMacroTargetFrame), true);
            // }
            if (s_p2WakeMacro96FrameCount >= s_p2WakeMacroTargetFrame) {
                int sel = CLAMP(s_p2WakeMacroSlot, 1, MacroController::GetSlotCount());
                if (MacroController::GetState() != MacroController::State::Recording &&
                    !MacroController::IsSlotEmpty(sel)) {
                    MacroController::SetCurrentSlot(sel);
                    LogOut("[AUTO-ACTION][MACRO] P2 Pre-buffering wake macro (slot=" + std::to_string(sel) +
                           ") at frame " + std::to_string(s_p2WakeMacro96FrameCount) +
                           " (target=" + std::to_string(s_p2WakeMacroTargetFrame) + ")", true);
                    TraceAutoActionWakeEvent("p2-wake-macro-play", 2, prevMoveID2, moveID2,
                                             "slot=" + std::to_string(sel) +
                                             " count96=" + std::to_string(s_p2WakeMacro96FrameCount) +
                                             " targetTick=" + std::to_string(s_p2WakeMacroTargetFrame) +
                                             " startTick=" + std::to_string(s_p2WakeMacroStartTick));
                    // Mark this macro as a wake pre-buffer so that when playback
                    // completes, the restore path preserves the buffered motion.
                    g_macroWakePreserveBuffer.store(true);
                    // Start playback (from tick 0 or calculated offset)
                    if (s_p2WakeMacroStartTick > 0) {
                        MacroController::PlayFromTick(s_p2WakeMacroStartTick);
                    } else {
                        MacroController::Play();
                    }
                    s_p2WakeMacroQueued = true;
                    s_p2WakeMacroTargetFrame = -1;
                    // Treat as buffered to skip generic wake fallback.
                    s_p2WakeBufferFrozen = true;
                }
            }
        }
    }
    // Wake outside groundtech logging disabled
    // else if (wakeBlockEnabled && detailedLogging.load()) {
    //     LogOut("[AUTO-ACTION][WAKE-OUTSIDE] Not in groundtech: moveID2=" + std::to_string(moveID2) +
    //            " prevMoveID2=" + std::to_string(prevMoveID2) +
    //            " IsGroundtech(prev)=" + std::to_string(IsGroundtech(prevMoveID2)) +
    //            " counter=" + std::to_string(s_p2WakeMacro96FrameCount) +
    //            " queued=" + std::to_string(s_p2WakeMacroQueued), true);
    // }

    // Standard wake pre-arm path for specials/holds (non-macro).
    if (triggerOnWakeupEnabled.load() && !s_p2WakePrearmed) {
            if (moveID2 == GROUNDTECH_RECOVERY) {
                if (triggerRandomizeEnabled.load()) {
                    if ((rand() & 1) == 0) {
                        if (detailedLogging.load() && canLogTrigDiag()) {
                            LogOut("[AUTO-ACTION] P2 Wake prearm skipped by random gate", true);
                        }
                        goto p2_wake_prearm_done;
                    }
                }

                // Determine the selected action row (if any) for wake pre-arm.
                int userDelayF = s_p2WakeOptionPicked ? s_p2WakePrePickedOption.delay : triggerOnWakeupDelay.load();
                if (s_p2WakeOptionPicked) {
                    userDelayF = (userDelayF <= 1) ? 0 : (userDelayF - 1);
                }
                int actionType = s_p2WakeOptionPicked ? s_p2WakePrePickedOption.action : triggerOnWakeupAction.load();
                int motionType = ConvertTriggerActionToMotion(actionType, TRIGGER_ON_WAKEUP);

                int macroSlot = s_p2WakeOptionPicked ? s_p2WakePrePickedOption.macroSlot : triggerOnWakeupMacroSlot.load();
                int macroSel = CLAMP(macroSlot, 1, MacroController::GetSlotCount());
                bool macroSlotSelected = (macroSlot > 0);
                bool macroHasData = macroSlotSelected &&
                                    MacroController::GetState() != MacroController::State::Recording &&
                                    !MacroController::IsSlotEmpty(macroSel);
                bool isDash = (motionType == MOTION_FORWARD_DASH || motionType == MOTION_BACK_DASH);
                bool isSpecial = (actionType == ACTION_FINAL_MEMORY) || (motionType >= MOTION_236A && !isDash);

                // If a macro slot is selected for On Wakeup we do NOT want to
                // treat this as a hold/special pre-arm. That case is handled
                // entirely by the dedicated macro pre-buffer path above.
                bool holdEligible = (userDelayF == 0) && !isSpecial &&
                                    SupportsImmediateWakeHold(actionType) && !macroSlotSelected;
                bool shouldPrearm = (userDelayF == 0) && (isSpecial || holdEligible);

                if (shouldPrearm) {
                    s_p2WakePrearmed = true;
                    s_p2WakePrearmIsSpecial = isSpecial;
                    s_p2WakePrearmIsMacro = false;
                    s_p2WakePrearmActionType = actionType;
                    int chosenStrength = triggerOnWakeupStrength.load();
                    if (s_p2WakeOptionPicked) {
                        chosenStrength = s_p2WakePrePickedOption.strength;
                    }
                    chosenStrength = CLAMP(chosenStrength, 0, 3);
                    s_p2WakePrearmStrength = chosenStrength;
                    s_p2WakePrearmExpiry = frameCounter.load() + 120;
                    s_p2WakeMoveID96FrameCount = 0;
                    s_p2WakeBufferFrozen = false;
                    s_p2WakeHoldPrimed = holdEligible;
                    s_p2WakeHoldIssued = false;
                    const char* tag = isSpecial ? "special" : "hold";
                    if (detailedLogging.load()) {
                        LogOut(std::string("[AUTO-ACTION] P2 wake ") + tag +
                               " metadata stored action=" + std::to_string(actionType) +
                               " strength=" + std::to_string(s_p2WakePrearmStrength) +
                               " macroPicked=" + (s_p2WakeOptionPicked ? "true" : "false"), true);
                    }
                    TraceAutoActionWakeEvent("p2-prearm-stored", 2, prevMoveID2, moveID2,
                                             "kind=" + std::string(tag) +
                                             " action=" + std::to_string(actionType) +
                                             " strength=" + std::to_string(s_p2WakePrearmStrength) +
                                             " delayF=" + std::to_string(userDelayF) +
                                             " optionPicked=" + std::to_string(s_p2WakeOptionPicked));
                    LogFlowSequenceEvent(2, "wake metadata stored",
                                         prevMoveID2, moveID2);
                } else if (detailedLogging.load()) {
                    std::string reason;
                    if (macroSlotSelected && macroHasData && userDelayF == 0) {
                        reason = "macro row (handled by dedicated macro pre-buffer path)";
                    } else if (userDelayF != 0) {
                        reason = "delay>0";
                    } else {
                        reason = "unsupported action";
                    }
                    LogOut("[AUTO-ACTION] P2 wake pre-arm skipped (" + reason + ")", true);
                }
            p2_wake_prearm_done: ;
            }
        }

        // On Wakeup trigger (fallback when not pre-armed)
        bool p2BecameActionableFromGroundtech = IsGroundtech(prevMoveID2) && IsActionable(moveID2);
        const bool p2LeavingGroundtechThisFrame = IsGroundtech(prevMoveID2) && !IsGroundtech(moveID2);
        
        // if (detailedLogging.load()) {
        //     LogOut("[AUTO-ACTION][FALLBACK-CHECK] prevMove=" + std::to_string(prevMoveID2) +
        //            " currMove=" + std::to_string(moveID2) +
        //            " IsGroundtech(prev)=" + std::to_string(IsGroundtech(prevMoveID2)) +
        //            " IsGroundtech(curr)=" + std::to_string(IsGroundtech(moveID2)) +
        //            " leaving=" + std::to_string(p2LeavingGroundtechThisFrame) +
        //            " queued=" + std::to_string(s_p2WakeMacroQueued) +
        //            " slot=" + std::to_string(s_p2WakeMacroSlot), true);
        // }

        // If groundtech recovery ended and we never started the macro in-state,
        // queue it on exit so the wake buffer still fires as early as possible.
        if (g_wakeBufferingEnabled.load() && p2LeavingGroundtechThisFrame && !s_p2WakeMacroQueued &&
            s_p2WakeMacroSlot > 0) {
            int sel = CLAMP(s_p2WakeMacroSlot, 1, MacroController::GetSlotCount());
            if (MacroController::GetState() != MacroController::State::Recording && !MacroController::IsSlotEmpty(sel)) {
                MacroController::SetCurrentSlot(sel);
                LogOut("[AUTO-ACTION][MACRO] Fallback wake macro queue on 96 exit (slot=" + std::to_string(sel) +
                       ", count=" + std::to_string(s_p2WakeMacro96FrameCount) +
                       ", target=" + std::to_string(s_p2WakeMacroTargetFrame) +
                       ", prevMove=" + std::to_string(prevMoveID2) +
                       ", currMove=" + std::to_string(moveID2) + ")", true);
                TraceAutoActionWakeEvent("p2-wake-macro-fallback-96-exit", 2, prevMoveID2, moveID2,
                                         "slot=" + std::to_string(sel) +
                                         " count96=" + std::to_string(s_p2WakeMacro96FrameCount) +
                                         " targetTick=" + std::to_string(s_p2WakeMacroTargetFrame));
                // Fallback wake macro should also preserve the buffered motion.
                g_macroWakePreserveBuffer.store(true);
                MacroController::Play();
                s_p2WakeMacroQueued = true;
            }
            s_p2WakeMacroTargetFrame = -1;
            s_p2WakeMacroSlot = -1;
        }

        // Cleanup Forced Neutral if we exited 96
        if (s_p2ForcedNeutral && moveID2 != GROUNDTECH_RECOVERY) {
             bool macroPlaying = (MacroController::GetState() == MacroController::State::Replaying);
             if (!macroPlaying) {
                 g_manualInputOverride[2].store(false, std::memory_order_relaxed);
                 g_injectImmediateOnly[2].store(false, std::memory_order_relaxed);
             }
             s_p2ForcedNeutral = false;
        }

        if (s_p2WakePrearmed) {
            // Count logical frames (192Hz) in moveID 96, buffer on last rising frame
            if (moveID2 == GROUNDTECH_RECOVERY) {
                s_p2WakeMoveID96FrameCount++;
                s_p2WakeMacro96FrameCount++;  // Also increment macro frame counter (64Hz equivalent)
                // Get character-specific rising frame count
                int charID = displayData.p2CharID;
                int risingTicks = GetWakeupRisingTicks(charID);
                int bufferFrame = risingTicks - 1;  // Buffer on last frame before actionable
                // Allow 3-frame window for safety (buffer-3 to buffer)
                if (s_p2WakePrearmIsSpecial && !s_p2WakeBufferFrozen && 
                    s_p2WakeMoveID96FrameCount >= (bufferFrame - 2) && s_p2WakeMoveID96FrameCount <= bufferFrame) {
                    int at = s_p2WakePrearmActionType;
                    bool p2Facing = GetPlayerFacingDirection(2);
                    bool ok = ExecuteWakeSpecialNow(2, at, moveID2, s_p2WakePrearmStrength);
                    LogOut("[AUTO-ACTION] P2 wake special early buffer (frame " + std::to_string(s_p2WakeMoveID96FrameCount) + "/" + std::to_string(risingTicks) + ", charID=" + std::to_string(charID) + ", facing=" + (p2Facing?"right":"left") + ") " + std::string(ok?"ok":"fail"), true);
                    TraceAutoActionWakeEvent("p2-early-special-freeze", 2, prevMoveID2, moveID2,
                                             "count96=" + std::to_string(s_p2WakeMoveID96FrameCount) +
                                             " risingTicks=" + std::to_string(risingTicks) +
                                             " bufferFrame=" + std::to_string(bufferFrame) +
                                             " charID=" + std::to_string(charID) +
                                             " action=" + std::to_string(at) +
                                             " ok=" + std::to_string(ok));
                    LogFlowSequenceEvent(2, "wake early buffer", prevMoveID2, moveID2);
                    s_p2WakeBufferFrozen = ok;
                }
            }
            // Check for 96→0 transition (must be outside the moveID==96 block!)
            if (!s_p2WakePrearmIsSpecial && !s_p2WakePrearmIsMacro && s_p2WakeHoldPrimed && !s_p2WakeHoldIssued) {
                const bool leavingGroundtechThisFrame = (prevMoveID2 == GROUNDTECH_RECOVERY) && (moveID2 != GROUNDTECH_RECOVERY);
                if (leavingGroundtechThisFrame) {
                    if (IssueWakeImmediateHold(2, s_p2WakePrearmActionType)) {
                        s_p2WakeHoldIssued = true;
                        TraceAutoActionWakeEvent("p2-wake-hold-issued", 2, prevMoveID2, moveID2,
                                                 "action=" + std::to_string(s_p2WakePrearmActionType));
                    }
                }
            }
            bool actionableButLostGround = !IsGroundtech(prevMoveID2) && IsActionable(moveID2) && !IsGroundtech(moveID2);
            if (actionableButLostGround && !s_p2WakeActionableWarning) {
                LogOut("[AUTO-ACTION][FLOW] P2 wake actionable while prev state not groundtech (prev=" + std::to_string(prevMoveID2) +
                       ", curr=" + std::to_string(moveID2) + ")", true);
                LogFlowSequenceEvent(2, "wake actionable lost-ground", prevMoveID2, moveID2);
                s_p2WakeActionableWarning = true;
            } else if (!actionableButLostGround) {
                s_p2WakeActionableWarning = false;
            }
        } else {
            s_p2WakeActionableWarning = false;
        }

        if (!shouldTrigger && triggerOnWakeupEnabled.load()) {
            // Diagnostic summary: when we actually become actionable from groundtech,
            // log what wake source we think is active (macro/special/hold/none).
            if (p2BecameActionableFromGroundtech && detailedLogging.load()) {
                const char* wakeSrc = "none";
                if (s_p2WakeMacroQueued) {
                    wakeSrc = "macro";
                } else if (s_p2WakePrearmed && s_p2WakePrearmIsSpecial) {
                    wakeSrc = "special";
                } else if (s_p2WakePrearmed && s_p2WakeHoldIssued) {
                    wakeSrc = "hold";
                } else if (s_p2WakePrearmed) {
                    wakeSrc = "prearmed";
                }
                LogOut(std::string("[AUTO-ACTION][WAKE-SUMMARY] P2 actionable-from-96 src=") + wakeSrc +
                       " currMove=" + std::to_string(moveID2) +
                       " prevMove=" + std::to_string(prevMoveID2) +
                       " macroQueued=" + std::to_string(s_p2WakeMacroQueued) +
                       " macroSlot=" + std::to_string(s_p2WakeMacroSlot) +
                       " macroFrameCount=" + std::to_string(s_p2WakeMacro96FrameCount),
                       true);
            }

            // If special was already frozen during moveID 96, just clear state on actionable
            if (s_p2WakePrearmed && s_p2WakeBufferFrozen && p2BecameActionableFromGroundtech) {
                LogFlowSequenceEvent(2, "wake exec (buffered early)", prevMoveID2, moveID2);
                p2TriggerActive = false;
                p2TriggerCooldown = 0;
                s_p2WakePrearmed = false;
                s_p2WakePrearmIsSpecial = false;
                s_p2WakePrearmIsMacro = false;
                s_p2WakePrearmActionType = -1;
                s_p2WakePrearmStrength = -1;
                s_p2WakeMoveID96FrameCount = 0;
                s_p2WakeBufferFrozen = false;
                s_p2WakeHoldPrimed = false;
                s_p2WakeHoldIssued = false;
            } else if (s_p2WakePrearmed && !s_p2WakePrearmIsSpecial && s_p2WakeHoldIssued && p2BecameActionableFromGroundtech) {
                LogOut("[AUTO-ACTION] P2 wake hold exec (inputs maintained through wake)", true);
                LogFlowSequenceEvent(2, "wake hold exec", prevMoveID2, moveID2);
                
                // Enable jump tracking
                s_p2WakeJumpTrackFrame = frameCounter.load();
                
                s_p2WakePrearmed = false;
                s_p2WakePrearmActionType = -1;
                s_p2WakePrearmStrength = -1;
                s_p2WakeMoveID96FrameCount = 0;
                s_p2WakeBufferFrozen = false;
                s_p2WakeHoldPrimed = false;
                s_p2WakeHoldIssued = false;
            } else if (s_p2WakePrearmed && !s_p2WakePrearmIsMacro && !s_p2WakeBufferFrozen && p2BecameActionableFromGroundtech) {
                AutoActionLogScope wakeScope("WakeImmediateExec", 2, TRIGGER_ON_WAKEUP);
                LogOut("[AUTO-ACTION] P2 wake exec (fallback): action=" + std::to_string(s_p2WakePrearmActionType) +
                       " special=" + std::to_string(s_p2WakePrearmIsSpecial) +
                       " frame=" + std::to_string(frameCounter.load()), true);
                    TraceAutoActionWakeEvent("p2-wake-fallback", 2, prevMoveID2, moveID2,
                                             "action=" + std::to_string(s_p2WakePrearmActionType) +
                                             " special=" + std::to_string(s_p2WakePrearmIsSpecial));
                    LogFlowSequenceEvent(2, "wake exec attempt", prevMoveID2, moveID2);
                    g_lastActiveTriggerType.store(TRIGGER_ON_WAKEUP);
                    g_lastActiveTriggerFrame.store(frameCounter.load());
                    if (s_p2WakePrearmActionType >= 0) {
                    int at = s_p2WakePrearmActionType;
                    if (s_p2WakePrearmIsSpecial) {
                        bool ok = ExecuteWakeSpecialNow(2, at, moveID2, s_p2WakePrearmStrength);
                        LogOut(std::string("[AUTO-ACTION] P2 wake special deferred inject ") + (ok?"ok":"fail"), true);
                        p2TriggerActive = false; p2TriggerCooldown = 0;
                    } else {
                        int motionType = ConvertTriggerActionToMotion(at, TRIGGER_ON_WAKEUP);
                        int buttonMask = ResolveButtonMaskForAction(at, TRIGGER_ON_WAKEUP, s_p2WakePrearmStrength);
                        if (at == ACTION_BACKDASH || at == ACTION_FORWARD_DASH) {
                            g_injectImmediateOnly[2].store(false);
                            EnableP2ControlForAutoAction();
                            ResetPlayerInputBufferIndex(2);
                            QueueMotionInput(2, motionType, 0);
                            g_pendingControlRestore.store(true);
                            g_pendingRestoreTimestamp.store(GetTickCount());
                        } else if ((motionType >= MOTION_5A && motionType <= MOTION_5D) || (motionType >= MOTION_6A && motionType <= MOTION_4D)) {
                            ImmediateInput::PressFor(2, buttonMask, 2);
                        } else if (motionType >= MOTION_2A && motionType <= MOTION_2C) {
                            ImmediateInput::PressFor(2, GAME_INPUT_DOWN | buttonMask, 2);
                        } else if (motionType >= MOTION_236A) {
                            if (buttonMask == 0) {
                                int atStrength = (s_p2WakePrearmStrength >= 0) ? s_p2WakePrearmStrength : GetSpecialMoveStrength(at, TRIGGER_ON_WAKEUP);
                                buttonMask = (1 << (4 + atStrength));
                            }
                            EnableP2ControlForAutoAction();
                            FreezeBufferForMotion(2, motionType, buttonMask);
                            g_pendingControlRestore.store(true);
                            g_pendingRestoreTimestamp.store(GetTickCount());
                            { std::stringstream bm; bm << std::hex << buttonMask; LogOut(std::string("[AUTO-ACTION] P2 wake special frame1 buffer applied (index frozen) btnMask=0x") + bm.str(), true); }
                        }
                    }
                }
                s_p2WakePrearmed = false;
                s_p2WakePrearmIsSpecial = false;
                s_p2WakePrearmIsMacro = false;
                s_p2WakePrearmActionType = -1;
                s_p2WakeMoveID96FrameCount = 0;
                s_p2WakeBufferFrozen = false;
                s_p2WakeHoldPrimed = false;
                s_p2WakeHoldIssued = false;
            } else if (s_p2WakeMacroQueued && IsGroundtech(prevMoveID2) && IsActionable(moveID2)) {
                // Macro was pre-buffered during moveID 96, just clear state
                if (detailedLogging.load()) {
                    LogOut("[AUTO-ACTION] P2 wake macro pre-buffered; skipping normal trigger currMove=" +
                           std::to_string(moveID2) + " prevMove=" + std::to_string(prevMoveID2), true);
                }
                s_p2WakePrearmed = false;
                s_p2WakePrearmIsSpecial = false;
                s_p2WakePrearmIsMacro = false;
                s_p2WakePrearmActionType = -1;
                s_p2WakeMacroQueued = false;
                s_p2WakeOptionPicked = false;
                s_p2WakeMacro96FrameCount = 0;
                s_p2WakeMacroTargetFrame = -1;
                s_p2WakeMacroSlot = -1;
            } else if (!s_p2WakePrearmed && !s_p2WakeMacroQueued && IsGroundtech(prevMoveID2) && IsActionable(moveID2)) {
                if (detailedLogging.load() && s_p2WakeMacroTargetFrame >= 0) {
                    LogOut("[AUTO-ACTION][MACRO] Wake pre-buffer missed (count=" + std::to_string(s_p2WakeMacro96FrameCount) +
                           ", target=" + std::to_string(s_p2WakeMacroTargetFrame) + ")", true);
                }
                s_p2WakeMacroTargetFrame = -1;
                s_p2WakeMacroSlot = -1;
                if (triggerRandomizeEnabled.load()) { if ((rand() & 1) == 0) { if (detailedLogging.load() && canLogTrigDiag()) LogOut("[AUTO-ACTION] P2 On Wakeup skipped by random gate", true); goto p2_wakeup_done; } }
                shouldTrigger = true;
                triggerType = TRIGGER_ON_WAKEUP;
                {
                    int userDelay = triggerOnWakeupDelay.load();
                    delay = (userDelay <= 1) ? 0 : (userDelay - 1);
                }
                actionMoveID = GetActionMoveID(triggerOnWakeupAction.load(), TRIGGER_ON_WAKEUP, 2);
                
                LogOut("[AUTO-ACTION] P2 On Wakeup trigger activated", detailedLogging.load());
                TraceAutoActionWakeEvent("p2-wakeup-trigger", 2, prevMoveID2, moveID2,
                                         "delayF=" + std::to_string(delay) +
                                         " actionMoveID=" + std::to_string(actionMoveID));
                if (detailedLogging.load()) {
                    LogOut("[TRIGGER_TIMING] P2 wake delay=" + std::to_string(s_p2Timers.lastWakeDelay) +
                           ", sinceWake=" + std::to_string(s_p2Timers.sinceWake), true);
                }
            }
        p2_wakeup_done: ;
        }
        
        if (shouldTrigger) {
            LogFlowSequenceEvent(2, "trigger delay", prevMoveID2, moveID2);
            AutoActionLogScope seqScope("TriggerSequence", 2, triggerType);
            StartTriggerDelay(2, triggerType, actionMoveID, delay);
        } else {
            // Don't spam repetitive diagnostics; throttling handled above
        }
    }

    // Post-wake cleanup for wake-prebuffered macros: once playback has
    // finished and P2 has left any groundtech state, monitor for successful
    // buffered move execution. When we detect moveID transition from 96
    // (standing after wake) to a bufferable action (attack/walk/dash/jump),
    // we know the buffered move executed successfully. At that point:
    // - If macro has finished: neutralize token immediately
    // - If macro still playing: wait for macro end signal before neutralizing
    if (g_wakeMacroPlaybackCompleted.load()) {
        if (!IsGroundtech(moveID2)) {
            // Macro playback finished; start monitoring for buffered move execution
            const int kWakeMacroTokenNeutralizeDelay = 20; // ~20 frames (~0.3s) at 192 Hz
            s_p2WakeMacroTokenNeutralizeCountdown = kWakeMacroTokenNeutralizeDelay;
            g_wakeMacroPlaybackCompleted.store(false);
            // Seed tracking with current moveID (should be 96 = standing after wake)
            s_p2LastWakeMoveID = moveID2;
            s_p2WakeMoveIDChanged = false;
            if (detailedLogging.load()) {
                LogOut("[AUTO-ACTION][MACRO-CLEANUP] Wake macro playback finished; monitoring for buffered move execution (current moveID=" +
                       std::to_string(moveID2) + ")", true);
            }
        }
    }

    // Wake macro cleanup countdown: monitor for moveID transition from 96 to bufferable action
    if (s_p2WakeMacroTokenNeutralizeCountdown > 0) {
        // Check if moveID transitioned from 96 (standing) to a bufferable action
        bool moveIDWas96 = (s_p2LastWakeMoveID == GROUNDTECH_RECOVERY);
        bool moveIDNowBufferable = IsBufferableAction(moveID2);
        
        if (moveIDWas96 && moveIDNowBufferable) {
            // Buffered move executed! Check if macro is still playing
            bool macroStillPlaying = (MacroController::GetState() == MacroController::State::Replaying);
            
            if (macroStillPlaying) {
                // Multi-move macro: buffered first move, but macro continues
                // Don't neutralize yet - wait for macro to finish naturally
                if (detailedLogging.load()) {
                    LogOut("[AUTO-ACTION][MACRO-CLEANUP] Buffered move executed (96→" + std::to_string(moveID2) +
                           "), macro still playing - waiting for macro end", true);
                }
                // Keep countdown active but reset it so we wait for macro end signal
                s_p2WakeMacroTokenNeutralizeCountdown = 1; // Keep alive
                s_p2LastWakeMoveID = moveID2;
            } else {
                // Single-move macro: buffered move executed and macro finished
                // Clean up immediately
                (void)NeutralizeMotionToken(2);
                (void)ClearPlayerInputBuffer(2);
                s_p2WakeMacroTokenNeutralizeCountdown = 0;
                s_p2WakeMoveIDChanged = false;
                if (detailedLogging.load()) {
                    LogOut("[AUTO-ACTION][MACRO-CLEANUP] Buffered move executed (96→" + std::to_string(moveID2) +
                           "), macro finished - immediate cleanup", true);
                }
            }
        } else {
            // Update tracking
            if (moveID2 != s_p2LastWakeMoveID) {
                s_p2WakeMoveIDChanged = true;
                s_p2LastWakeMoveID = moveID2;
            }
            
            // Decrement countdown
            s_p2WakeMacroTokenNeutralizeCountdown--;
            
            if (s_p2WakeMacroTokenNeutralizeCountdown == 0) {
                // Fallback: countdown expired without detecting buffered move
                // This shouldn't happen in normal cases, but clean up anyway
                (void)NeutralizeMotionToken(2);
                if (moveID2 >= 200) {
                    (void)ClearPlayerInputBuffer(2);
                }
                if (detailedLogging.load()) {
                    LogOut("[AUTO-ACTION][MACRO-CLEANUP] Countdown expired (moveID=" + std::to_string(moveID2) +
                           ") - fallback cleanup", true);
                }
            }
        }
    }
    
    // Action flag reset logic (existing code)
    static int p1ActionAppliedFrame = -1;
    static int p2ActionAppliedFrame = -1;
    
    if (p1ActionApplied && p1ActionAppliedFrame == -1) {
        p1ActionAppliedFrame = frameCounter.load();
    }
    if (p2ActionApplied && p2ActionAppliedFrame == -1) {
        p2ActionAppliedFrame = frameCounter.load();
    }
    
    int currentFrame = frameCounter.load();
    
    if (p1ActionApplied && p1ActionAppliedFrame != -1) {
        if (currentFrame - p1ActionAppliedFrame >= 30) {
            p1ActionApplied = false;
            p1ActionAppliedFrame = -1;
        }
    }
    
    if (p2ActionApplied && p2ActionAppliedFrame != -1) {
        if (currentFrame - p2ActionAppliedFrame >= 30) {
            p2ActionApplied = false;
            p2ActionAppliedFrame = -1;
        }
    }
    
    // Note: prev handled by caller when using the optimized path
    
    // Process auto control restore at the end of every monitor cycle
    ProcessAutoControlRestore();

    // Now that all moveID-based transitions processed, handle any deferred dash follow-ups
    ProcessDeferredDashFollowups(moveID1, prevMoveID1, moveID2, prevMoveID2);

    MaybeAutoCloseFlowSequence(1, prevMoveID1, moveID1, s_p1WakePrearmed, false);
    // For P2, also treat a queued wake macro as keeping the sequence open.
    MaybeAutoCloseFlowSequence(2, prevMoveID2, moveID2,
                               s_p2WakePrearmed || s_p2WakeMacroQueued,
                               g_pendingControlRestore.load());

    // Clear pre-arm flags when window passes to avoid sticking
    int now = frameCounter.load();
    if (s_p1WakePrearmed && now > s_p1WakePrearmExpiry) {
        int delta = now - s_p1WakePrearmExpiry;
        LogOut("[AUTO-ACTION] P1 wake pre-arm expired (no execution, delta=" + std::to_string(delta) +
               ", currMove=" + std::to_string(moveID1) + ")", true);
        LogFlowSequenceEvent(1, "wake pre-arm expired", prevMoveID1, moveID1);
        EndFlowSequence(1, "WakePrearmExpired", moveID1);
        s_p1WakePrearmed = false;
        s_p1WakePrearmIsSpecial = false;
        s_p1WakePrearmActionType = -1;
        s_p1WakePrearmStrength = -1;
        s_p1WakeMoveID96FrameCount = 0;
        s_p1WakeBufferFrozen = false;
        s_p1WakeHoldPrimed = false;
        s_p1WakeHoldIssued = false;
        s_p1WakeOptionPicked = false;
        s_p1WakePrePickedOption = {};
    }
    if (s_p2WakePrearmed && now > s_p2WakePrearmExpiry) {
        int delta = now - s_p2WakePrearmExpiry;
        LogOut("[AUTO-ACTION] P2 wake pre-arm expired (no execution, delta=" + std::to_string(delta) +
               ", currMove=" + std::to_string(moveID2) + ")", true);
        LogFlowSequenceEvent(2, "wake pre-arm expired", prevMoveID2, moveID2);
        EndFlowSequence(2, "WakePrearmExpired", moveID2);
        s_p2WakePrearmed = false;
        s_p2WakePrearmIsSpecial = false;
        s_p2WakePrearmIsMacro = false;
        s_p2WakePrearmActionType = -1;
        s_p2WakePrearmStrength = -1;
        s_p2WakeMoveID96FrameCount = 0;
        s_p2WakeBufferFrozen = false;
        s_p2WakeHoldPrimed = false;
        s_p2WakeHoldIssued = false;
        s_p2WakeOptionPicked = false;
        s_p2WakePrePickedOption = {};
    }
    // Clear RG pre-arm flags if window passed without execution
    if (s_p1RGPrearmed && now > s_p1RGPrearmExpiry) {
        LogOut("[AUTO-ACTION][RG] P1 RG pre-arm expired (no execution)", detailedLogging.load());
        s_p1RGPrearmed = false; s_p1RGPrearmIsSpecial = false; s_p1RGPrearmActionType = -1;
    }
    if (s_p2RGPrearmed && now > s_p2RGPrearmExpiry) {
        LogOut("[AUTO-ACTION][RG] P2 RG pre-arm expired (no execution)", detailedLogging.load());
        s_p2RGPrearmed = false; s_p2RGPrearmIsSpecial = false; s_p2RGPrearmActionType = -1;
    }
}

// Back-compat wrapper now using unified per-frame sample to avoid extra memory reads
void MonitorAutoActions() {
    static short s_prevMoveID1 = 0, s_prevMoveID2 = 0;
    if (Mission::Engine::Demo::IsActive()) return;
    if (!AutoActionWorkPending()) {
        if (detailedLogging.load()) {
            static int s_lastIdleLogFrame = 0;
            int nowF = frameCounter.load();
            if (nowF - s_lastIdleLogFrame >= 5760) { // ~30s at 192Hz
                LogOut("[AUTO-ACTION] Idle (disabled/no triggers) - skipping per-frame processing", true);
                s_lastIdleLogFrame = nowF;
            }
        }
        return;
    }
    const PerFrameSample &sample = GetCurrentPerFrameSample();
    short m1 = sample.moveID1;
    short m2 = sample.moveID2;
    MonitorAutoActionsImpl(m1, m2, s_prevMoveID1, s_prevMoveID2);
    s_prevMoveID1 = m1; s_prevMoveID2 = m2;
}

void MonitorAutoActions(short moveID1, short moveID2, short prevMoveID1, short prevMoveID2) {
    if (Mission::Engine::Demo::IsActive()) return;
    const PerFrameSample &sample = GetCurrentPerFrameSample();
    short m1  = (sample.moveID1     == moveID1     ? sample.moveID1     : moveID1);
    short m2  = (sample.moveID2     == moveID2     ? sample.moveID2     : moveID2);
    short pm1 = (sample.prevMoveID1 == prevMoveID1 ? sample.prevMoveID1 : prevMoveID1);
    short pm2 = (sample.prevMoveID2 == prevMoveID2 ? sample.prevMoveID2 : prevMoveID2);
    MonitorAutoActionsImpl(m1, m2, pm1, pm2);
}

void ResetActionFlags() {
    p1ActionApplied = false;
    p2ActionApplied = false;
    RestoreP2ControlState();
    LogOut("[AUTO-ACTION] ResetActionFlags invoked (control restored if overridden)", true);
}

void ClearDelayStatesIfNonActionable(short moveID1, short moveID2, short prevMoveID1, short prevMoveID2, const char* source) {
    if (!p1DelayState.isDelaying && !p2DelayState.isDelaying) return;
    const std::string clearSource = source ? source : "unknown";
    std::string p1Reason;
    std::string p2Reason;
    const bool p1ShouldCancel = ShouldCancelDelayForState(p1DelayState, prevMoveID1, moveID1, p1Reason);
    const bool p2ShouldCancel = ShouldCancelDelayForState(p2DelayState, prevMoveID2, moveID2, p2Reason);
    if (p1DelayState.isDelaying && p1ShouldCancel) {
        const int clearedTriggerType = p1DelayState.triggerType;
        const int remainingInternal = p1DelayState.delayFramesRemaining;
        const int remainingVisual = InternalTicksToVisualFramesCeil(remainingInternal);
        ResetDelayState(p1DelayState);
        LogOut("[AUTO-ACTION][DELAY] Cleared P1 delay source=" + clearSource +
               " trigger=" + TriggerTypeLabel(clearedTriggerType) +
               " reason=" + p1Reason +
               " prevMoveID=" + std::to_string(prevMoveID1) +
               " moveID=" + std::to_string(moveID1) +
               " visualRemaining=" + std::to_string(remainingVisual) +
               " internalRemaining=" + std::to_string(remainingInternal),
               true);
        if (clearedTriggerType == TRIGGER_ON_WAKEUP) {
            TraceAutoActionWakeEvent("p1-delay-cleared", 1, prevMoveID1, moveID1,
                                     "source=" + clearSource +
                                     " reason=" + p1Reason +
                                     " remainingInternal=" + std::to_string(remainingInternal));
        }
        p1TriggerActive = false;
        p1TriggerCooldown = 0;
    }
    if (p2DelayState.isDelaying && p2ShouldCancel) {
        const int clearedTriggerType = p2DelayState.triggerType;
        const int remainingInternal = p2DelayState.delayFramesRemaining;
        const int remainingVisual = InternalTicksToVisualFramesCeil(remainingInternal);
        ResetDelayState(p2DelayState);
        LogOut("[AUTO-ACTION][DELAY] Cleared P2 delay source=" + clearSource +
               " trigger=" + TriggerTypeLabel(clearedTriggerType) +
               " reason=" + p2Reason +
               " prevMoveID=" + std::to_string(prevMoveID2) +
               " moveID=" + std::to_string(moveID2) +
               " visualRemaining=" + std::to_string(remainingVisual) +
               " internalRemaining=" + std::to_string(remainingInternal),
               true);
        if (clearedTriggerType == TRIGGER_ON_WAKEUP) {
            TraceAutoActionWakeEvent("p2-delay-cleared", 2, prevMoveID2, moveID2,
                                     "source=" + clearSource +
                                     " reason=" + p2Reason +
                                     " remainingInternal=" + std::to_string(remainingInternal));
        }
        p2TriggerActive = false;
        p2TriggerCooldown = 0;
    }
}

void ClearDelayStatesIfNonActionable() {
    const PerFrameSample &sample = GetCurrentPerFrameSample();
    ClearDelayStatesIfNonActionable(sample.moveID1, sample.moveID2, sample.prevMoveID1, sample.prevMoveID2, "sample");
}

// Replace the ApplyAutoAction function with this implementation:
void ApplyAutoAction(int playerNum, uintptr_t moveIDAddr, short currentMoveID, short prevMoveID) {
    // If caller didn't provide current move id, try to read it for better restore tracking
    if (currentMoveID == 0 && moveIDAddr != 0) {
        SafeReadMemory(moveIDAddr, &currentMoveID, sizeof(short));
    }
    // Get the trigger type from the player's delay state
    int triggerType = (playerNum == 1) ? p1DelayState.triggerType : p2DelayState.triggerType;

    AutoActionLogScope applyScope("ApplyAutoAction", playerNum, triggerType);
    (void)applyScope;

    // Get the appropriate action for this trigger
    int actionType = 0;
    // Prefer row-based chosen action if one was set at arming time
    const TriggerDelayState& dstate = (playerNum == 1) ? p1DelayState : p2DelayState;
    if (dstate.chosenAction >= 0) {
        actionType = dstate.chosenAction;
    } else {
        // Else prefer multi-pool selection if enabled and configured
        TriggerOption poolPick{};
        if (PickRandomPoolOption(triggerType, playerNum, poolPick)) {
            actionType = poolPick.action;
        } else {
            switch (triggerType) {
                case TRIGGER_AFTER_BLOCK:
                    actionType = triggerAfterBlockAction.load();
                    break;
                case TRIGGER_ON_WAKEUP:
                    actionType = triggerOnWakeupAction.load();
                    break;
                case TRIGGER_AFTER_HITSTUN:
                    actionType = triggerAfterHitstunAction.load();
                    break;
                case TRIGGER_AFTER_AIRTECH:
                    actionType = triggerAfterAirtechAction.load();
                    break;
                case TRIGGER_ON_RG:
                    actionType = triggerOnRGAction.load();
                    break;
                default:
                    actionType = ACTION_5A; // Default to 5A
                    break;
            }
        }
    }

    // Special early handling: Final Memory bypasses motion mapping and injects bespoke pattern
    if (actionType == ACTION_FINAL_MEMORY) {
        int charId = (playerNum == 1) ? displayData.p1CharID : displayData.p2CharID;
        if (charId < 0) {
            LogOut("[AUTO-ACTION][FM] Aborting: unknown character id", true);
        } else {
            LogOut("[AUTO-ACTION][FM] Attempting Final Memory for player " + std::to_string(playerNum) +
                   " charId=" + std::to_string(charId), true);
            // For P2 we must force human control so buffer writes advance like specials/supers
            if (playerNum == 2) {
                EnableP2ControlForAutoAction();
            }
            bool ok = ExecuteFinalMemory(playerNum, charId);
            if (ok) {
                if (playerNum == 2) {
                    p2TriggerActive = false; p2TriggerCooldown = 0; 
                    // Mirror special-move restore flow so AI control is returned after FM executes
                    g_lastP2MoveID.store(currentMoveID); // starting move id baseline
                    g_pendingControlRestore.store(true);
                    g_pendingRestoreTimestamp.store(GetTickCount());
                    LogOut("[AUTO-ACTION][FM] Scheduled P2 control restore (timeout=240)", true);
                } else {
                    p1TriggerActive = false; p1TriggerCooldown = 0; 
                }
                LogOut("[AUTO-ACTION][FM] Final Memory pattern frozen", true);
                return; // Fully handled
            } else {
                LogOut("[AUTO-ACTION][FM] Gate failed or pattern apply error", true);
            }
        }
        // Fall through to normal handling if FM failed; do NOT early return
    }

    // If this is an RG-triggered execution and we already pre-armed a special/FM at RG entry,
    // avoid double-injecting. Just clear trigger/cooldown and return.
    if (triggerType == TRIGGER_ON_RG) {
        if (playerNum == 1 && s_p1RGPrearmed && s_p1RGPrearmIsSpecial) {
            LogOut("[AUTO-ACTION][RG] P1 was pre-armed; skipping duplicate ApplyAutoAction", true);
            p1TriggerActive = false; p1TriggerCooldown = 0;
            s_p1RGPrearmed = false; s_p1RGPrearmIsSpecial = false; s_p1RGPrearmActionType = -1;
            return;
        }
        if (playerNum == 2 && s_p2RGPrearmed && s_p2RGPrearmIsSpecial) {
            LogOut("[AUTO-ACTION][RG] P2 was pre-armed; skipping duplicate ApplyAutoAction", true);
            p2TriggerActive = false; p2TriggerCooldown = 0;
            s_p2RGPrearmed = false; s_p2RGPrearmIsSpecial = false; s_p2RGPrearmActionType = -1;
            return;
        }
    }

    // Determine strength BEFORE converting to motion; strength drives motionType selection
    int resolvedStrength = (dstate.chosenStrength >= 0) ? dstate.chosenStrength : GetSpecialMoveStrength(actionType, triggerType); // 0=A 1=B 2=C 3=D
    // Convert action to motion type and determine button mask. Use resolvedStrength so row-specific
    // strength overrides (per-option rows) correctly influence which super variant is chosen.
    int motionType = ConvertTriggerActionToMotion(actionType, triggerType, resolvedStrength);
    int buttonMask = 0;
    
    // Determine button mask based on action type
    // Use explicit switch to avoid overlapping ID ranges
    switch (actionType) {
        // Neutral normals
        case ACTION_5A: buttonMask = GAME_INPUT_A; break;
        case ACTION_5B: buttonMask = GAME_INPUT_B; break;
        case ACTION_5C: buttonMask = GAME_INPUT_C; break;
        case ACTION_5D: buttonMask = GAME_INPUT_D; break;
        
        // Crouch normals
        case ACTION_2A: buttonMask = GAME_INPUT_A; break;
        case ACTION_2B: buttonMask = GAME_INPUT_B; break;
        case ACTION_2C: buttonMask = GAME_INPUT_C; break;
        case ACTION_2D: buttonMask = GAME_INPUT_D; break;
        
        // Jump normals
        case ACTION_JA: buttonMask = GAME_INPUT_A; break;
        case ACTION_JB: buttonMask = GAME_INPUT_B; break;
        case ACTION_JC: buttonMask = GAME_INPUT_C; break;
        case ACTION_JD: buttonMask = GAME_INPUT_D; break;
        
        // Forward normals
        case ACTION_6A: buttonMask = GAME_INPUT_A; break;
        case ACTION_6B: buttonMask = GAME_INPUT_B; break;
        case ACTION_6C: buttonMask = GAME_INPUT_C; break;
        case ACTION_6D: buttonMask = GAME_INPUT_D; break;
        
        // Back normals
        case ACTION_4A: buttonMask = GAME_INPUT_A; break;
        case ACTION_4B: buttonMask = GAME_INPUT_B; break;
        case ACTION_4C: buttonMask = GAME_INPUT_C; break;
        case ACTION_4D: buttonMask = GAME_INPUT_D; break;
        
        // Special moves - use strength from config / row override
        case ACTION_QCF:
        case ACTION_DP:
        case ACTION_QCB:
        case ACTION_421:
        case ACTION_SUPER1:
        case ACTION_SUPER2:
        case ACTION_236236:
        case ACTION_214214:
        case ACTION_641236:
        case ACTION_463214:
        case ACTION_412:
        case ACTION_22:
        case ACTION_4123641236:
        case ACTION_6321463214: {
            int strength = resolvedStrength; // 0=A 1=B 2=C 3=D
            if (strength < 0) strength = 0;
            if (strength > 3) strength = 3;
            buttonMask = (1 << (4 + strength));  // A=16, B=32, C=64, D=128
            break;
        }
        
        default:
            break;
    }

    LogOut("[AUTO-ACTION] Converting action=" + std::to_string(actionType) +
           " trigger=" + std::to_string(triggerType) +
           " strength=" + std::to_string(resolvedStrength) +
           " => motionType=" + std::to_string(motionType) +
           " prelimButtonMask=" + std::to_string(buttonMask), true);
    if (triggerType == TRIGGER_ON_WAKEUP) {
        const short traceMoveID = static_cast<short>(GetPlayerMoveID(playerNum));
        TraceAutoActionWakeEvent("apply-auto-action:wakeup", playerNum,
                                 (prevMoveID >= 0) ? prevMoveID : traceMoveID,
                                 traceMoveID,
                                 "action=" + std::to_string(actionType) +
                                 " strength=" + std::to_string(resolvedStrength) +
                                 " motion=" + std::to_string(motionType) +
                                 " buttonMask=" + std::to_string(buttonMask) +
                                 " currentArg=" + std::to_string(currentMoveID));
    }

    // If buttonMask is zero, use default
    if (buttonMask == 0) {
        LogOut("[AUTO-ACTION] Button mask was 0; defaulting to A (likely non-button action)", true);
        buttonMask = GAME_INPUT_A; // Default to A button (16)
    }
    else {
        LogOut("[AUTO-ACTION] Final buttonMask=" + std::to_string(buttonMask) +
               " (A=16,B=32,C=64,D=128)", true);
    }
    
    bool success = false;
    bool isRegularMove = (motionType >= MOTION_5A && motionType <= MOTION_JD) || (motionType >= MOTION_6A && motionType <= MOTION_4D);
    bool isSpecialMove = (motionType >= MOTION_236A);
    // Exclude dashes from being treated as specials for restore/control tracking
    if (actionType == ACTION_FORWARD_DASH || actionType == ACTION_BACKDASH || actionType == ACTION_BACK_DASH) {
        isSpecialMove = false;
    }

    // Special handling: Jump must be injected via immediate input register with direction
    if (actionType == ACTION_JUMP) {
        // Determine jump direction: 0=neutral, 1=forward, 2=backward from strength slot
        int dir = (dstate.chosenStrength >= 0) ? dstate.chosenStrength : 0;
        if (dstate.chosenStrength < 0) {
            switch (triggerType) {
                case TRIGGER_AFTER_BLOCK: dir = triggerAfterBlockStrength.load(); break;
                case TRIGGER_ON_WAKEUP: dir = triggerOnWakeupStrength.load(); break;
                case TRIGGER_AFTER_HITSTUN: dir = triggerAfterHitstunStrength.load(); break;
                case TRIGGER_AFTER_AIRTECH: dir = triggerAfterAirtechStrength.load(); break;
                case TRIGGER_ON_RG: dir = triggerOnRGStrength.load(); break;
                default: dir = 0; break;
            }
        }

        // Build immediate mask: UP plus optional left/right based on facing and dir
        uint8_t mask = GAME_INPUT_UP;
        if (dir == 1 || dir == 2) {
            bool facingRight = GetPlayerFacingDirection(playerNum);
            if (dir == 1) {
                // forward
                mask |= (facingRight ? GAME_INPUT_RIGHT : GAME_INPUT_LEFT);
            } else if (dir == 2) {
                // backward
                mask |= (facingRight ? GAME_INPUT_LEFT : GAME_INPUT_RIGHT);
            }
        }

    LogOut("[AUTO-ACTION] Injecting Jump via immediate writer (dir=" + std::to_string(dir) + ")", true);
    ImmediateInput::PressFor(playerNum, mask, 3);
    // Jump is transient: free trigger immediately
    if (playerNum == 2) { p2TriggerActive = false; p2TriggerCooldown = 0; }
    else { p1TriggerActive = false; p1TriggerCooldown = 0; }
    LogOut("[AUTO-ACTION] Jump scheduled (cooldown cleared)", true);

        success = true;
    // No control override needed for jumps
    }

    // Special handling: Dashes should be executed by writing to the buffer via the queue
    if (!success && (actionType == ACTION_BACKDASH || actionType == ACTION_FORWARD_DASH)) {
        LogOut("[AUTO-ACTION] Queuing dash motion via buffer (" + GetMotionTypeName(motionType) + ")", true);
        g_injectImmediateOnly[playerNum].store(false);
        bool isForwardDash = (actionType == ACTION_FORWARD_DASH);
        if (playerNum == 2) {
            // Revert: always override P2 control for any auto dash so that subsequent immediate
            // injections (dash normals) occur under human control. This addresses reliability
            // concerns when AI logic might otherwise consume/alter buffered inputs.
            EnableP2ControlForAutoAction();
        }
        // CRITICAL FIX: Reset buffer index to 0 before writing dash pattern.
        // Game's dash detector only checks last 5 buffer positions (index-5 to index).
        // Without reset, dash pattern lands at arbitrary position causing recognition failures.
        // Dash follow-ups use immediate registers, not buffer, so this is safe.
        ResetPlayerInputBufferIndex(playerNum);
        success = QueueMotionInput(playerNum, motionType, 0);
        if (success && isForwardDash) {
            if (playerNum == 2) {
                g_recentDashQueued.store(true);
                g_recentDashQueuedFrame.store(frameCounter.load());
            }
            int fdf = forwardDashFollowup.load();
            bool dashMode = forwardDashFollowupDashMode.load();
            if (fdf > 0) {
                // Always treat as dash-normal; ignore previous dashMode toggle for now.
                ScheduleForwardDashFollowup(playerNum, fdf, true);
            }
            // Always arm restore monitor for P2 so control returns after dash (or dash+follow-up)
            if (playerNum == 2 && !g_pendingControlRestore.load()) {
                g_lastP2MoveID.store(-1);
                g_pendingControlRestore.store(true);
                g_pendingRestoreTimestamp.store(GetTickCount());
                LogOut(std::string("[AUTO-ACTION][DASH] Armed control restore monitor (") + (fdf>0?"follow-up":"no follow-up") + ")", true);
            }
        } else if (success && actionType == ACTION_BACKDASH) {
            // Treat backdash like forward dash for restore guarding: mark as recently queued
            if (playerNum == 2) {
                g_recentDashQueued.store(true);
                g_recentDashQueuedFrame.store(frameCounter.load());
            }
            int fdf = forwardDashFollowup.load();
            if (fdf > 0) {
                ScheduleBackDashFollowup(playerNum, fdf);
            }
            if (playerNum == 2 && !g_pendingControlRestore.load()) {
                g_lastP2MoveID.store(-1);
                g_pendingControlRestore.store(true);
                g_pendingRestoreTimestamp.store(GetTickCount());
                LogOut(std::string("[AUTO-ACTION][DASH] Armed control restore monitor (backdash ") + (fdf>0?"+ follow-up)":"no follow-up)"), true);
            }
        }
    }
    
    if (!success && isRegularMove) {
        // For regular moves, use a simple manual-override hold; DO allow buffer writes (do not set immediate-only)
        LogOut("[AUTO-ACTION] Applying regular move " + GetMotionTypeName(motionType) +
               " via manual hold (buffered)", detailedLogging.load());

        // Build the mask for this normal (directional variants need direction bits)
        uint8_t inputMask = GAME_INPUT_NEUTRAL;
        bool facingRight = GetPlayerFacingDirection(playerNum);
        uint8_t forwardDir = facingRight ? GAME_INPUT_RIGHT : GAME_INPUT_LEFT;
        uint8_t backDir    = facingRight ? GAME_INPUT_LEFT  : GAME_INPUT_RIGHT;

        if (motionType >= MOTION_5A && motionType <= MOTION_5D) {
            inputMask = buttonMask; // Neutral ground normals (A/B/C/D)
        } else if (motionType >= MOTION_2A && motionType <= MOTION_2D) {
            inputMask = GAME_INPUT_DOWN | buttonMask; // Crouching normals (2A/B/C/D)
        } else if (motionType >= MOTION_JA && motionType <= MOTION_JD) {
            inputMask = buttonMask; // Air normals (JA/B/C/D - jump direction handled separately at jump time)
        } else if (motionType >= MOTION_6A && motionType <= MOTION_6D) {
            inputMask = forwardDir | buttonMask; // Forward direction + button (6A/B/C/D)
        } else if (motionType >= MOTION_4A && motionType <= MOTION_4D) {
            inputMask = backDir | buttonMask; // Back direction + button (4A/B/C/D)
        }

    // Use immediate writer for the button (A/B/C) and optionally direction DOWN for 2A/2B/2C.
    // Do not push to buffer here; motions and freezes remain buffer-driven.
    ImmediateInput::PressFor(playerNum, inputMask, 2);
    // Normals quick: free trigger for chaining
    if (playerNum == 2) { p2TriggerActive = false; p2TriggerCooldown = 0; }
    else { p1TriggerActive = false; p1TriggerCooldown = 0; }

        success = true;
    }
    else if (!success && isSpecialMove) {
        // For special moves, switch to human control (P2 only) and use buffer freezing
        if (playerNum == 2) {
            EnableP2ControlForAutoAction();
        }
        LogOut("[AUTO-ACTION] Applying special move " + GetMotionTypeName(motionType) +
               " via buffer freeze", true);
        // Ensure immediate-only is disabled so buffer writes progress
        g_injectImmediateOnly[playerNum].store(false);
        success = FreezeBufferForMotion(playerNum, motionType, buttonMask);
        // If this came from an On RG trigger, enable fast restore
        if (success && triggerType == TRIGGER_ON_RG && playerNum == 2) {
            if (g_counterRGEnabled.load()) {
                g_crgFastRestore.store(true);
            }
        }
    }
    
    // (Deferred dash follow-ups processed centrally each frame now)

    if (success) {
    if (playerNum == 2 && isSpecialMove) {
            LogOut("[AUTO-ACTION] Tracking P2 special for restore", true);
            g_lastP2MoveID.store(currentMoveID);
            g_pendingControlRestore.store(true);
            g_pendingRestoreTimestamp.store(GetTickCount());
        }
    } else {
        LogOut("[AUTO-ACTION] Failed to apply move " + GetMotionTypeName(motionType), true);
        RestoreP2ControlState();
    }

    // AttackReader disabled to reduce CPU usage
    // short moveID = GetActionMoveID(actionType, triggerType, playerNum);
    // AttackReader::LogMoveData(playerNum, moveID);
}

// Enable P2 human control for auto-action and save original state
void EnableP2ControlForAutoAction() {
    // CRITICAL: Never modify game state during online mode
    if (g_onlineModeActive.load()) return;

    uintptr_t base = GetEFZBase();
    if (!base) {
        LogOut("[AUTO-ACTION] Failed to get EFZ base address", true);
        return;
    }
    
    uintptr_t p2CharPtr = 0;
    if (!SafeReadMemory(base + EFZ_BASE_OFFSET_P2, &p2CharPtr, sizeof(uintptr_t)) || !p2CharPtr) {
        LogOut("[AUTO-ACTION] Failed to get P2 pointer for control override", true);
        return;
    }
    
    // IMPORTANT: Always verify the ACTUAL control flag value
    uint32_t currentAIFlag = 1;
    if (!SafeReadMemory(p2CharPtr + AI_CONTROL_FLAG_OFFSET, &currentAIFlag, sizeof(uint32_t))) {
        LogOut("[AUTO-ACTION] Failed to read P2 control state, aborting override", true);
        return;
    }
    
    // Only save the original flag the first time we override it
    if (!g_p2ControlOverridden) {
        g_originalP2ControlFlag = currentAIFlag;
        LogOut("[AUTO-ACTION] Saving original P2 AI control flag: " + std::to_string(g_originalP2ControlFlag), true);
    } else if (currentAIFlag != 0) {
        // If flag was reset by the game, log it
        LogOut("[AUTO-ACTION] P2 control flag was reset to " + std::to_string(currentAIFlag) + ", setting back to human control", true);
    }
    
    // Always set to human control (0) regardless of our tracking variable, with audit logging
    uint32_t humanControlFlag = 0;
    uint32_t before = 0xFFFFFFFFu, after = 0xFFFFFFFFu;
    SafeReadMemory(p2CharPtr + AI_CONTROL_FLAG_OFFSET, &before, sizeof(uint32_t));
    bool okWrite = SafeWriteMemory(p2CharPtr + AI_CONTROL_FLAG_OFFSET, &humanControlFlag, sizeof(uint32_t));
    SafeReadMemory(p2CharPtr + AI_CONTROL_FLAG_OFFSET, &after, sizeof(uint32_t));
    std::ostringstream oss; oss << "[AUDIT][AI] EnableP2ControlForAutoAction @0x" << std::hex << (p2CharPtr + AI_CONTROL_FLAG_OFFSET)
                                << std::dec << " before=" << before << " write=0 after=" << after
                                << " okWrite=" << (okWrite?"1":"0");
    LogOut(oss.str(), true);
    if (okWrite && after == 0) {
        g_p2ControlOverridden = true;
        LogOut("[AUTO-ACTION] P2 control successfully set to human (0) for auto-action", true);
    } else {
        LogOut("[AUTO-ACTION] P2 control write failed verification, flag still = " + std::to_string(after), true);
    }
}

// ==================================================================================
// RESTORE P2 CONTROL STATE (FULL RESTORE)
// ==================================================================================
// This is the STANDARD restore function used for most auto-actions.
// 
// What it does:
//   1. Stops buffer freezing (allows input to flow normally)
//   2. Clears input buffer completely (prevents AI from reading stale patterns)
//   3. Clears immediate input registers
//   4. Restores CPU control flag at game state level (gameStatePtr + 4931)
//   5. Restores AI control flag at character level (p2CharPtr + 0x42C)
//   6. Starts grace period to prevent immediate re-triggering
//
// When to use:
//   ✅ Forward/backward dashes (after dash completes)
//   ✅ Specials and supers (after move executes or times out)
//   ✅ Normal attacks (after attack completes)
//   ✅ Any action where you want a CLEAN state transition back to AI control
//   ✅ Any action where stale inputs could cause problems (dash infinite loops, etc.)
//
// When NOT to use:
//   ❌ Counter RG scenarios where you want the pre-armed special to execute
//      (use RestoreP2ControlFlagOnly instead)
// ==================================================================================
void RestoreP2ControlState() {
    // CRITICAL: Never modify game state during online mode
    if (g_onlineModeActive.load()) {
        g_p2ControlOverridden = false;
        return;
    }

    if (g_p2ControlOverridden) {
        const PerFrameSample &restoreSample = GetCurrentPerFrameSample();
        short restoreMoveID2 = restoreSample.moveID2;
        // Make sure to stop any buffer freezing when restoring control
        StopBufferFreezing();
        
        // IMPORTANT: Force a longer cooldown period to prevent immediate re-triggering
        p2TriggerActive = true;
    p2TriggerCooldown = TRIGGER_COOLDOWN_FRAMES;
        LogOut("[AUTO-ACTION] Enforcing extended trigger cooldown after control restore", true);
        
        // Always neutralize the motion token when returning control to AI to kill any latent motion parsing
        (void)NeutralizeMotionToken(2);

        uintptr_t base = GetEFZBase();
        if (!base) {
            LogOut("[AUTO-ACTION] Failed to get EFZ base for control restore, marking as restored anyway", true);
            g_p2ControlOverridden = false; // Reset flag anyway to avoid getting stuck
            return;
        }
        
        // CRITICAL: Get game state pointer to restore CPU control flag
        uintptr_t gameStatePtr = 0;
        if (!SafeReadMemory(base + EFZ_BASE_OFFSET_GAME_STATE, &gameStatePtr, sizeof(uintptr_t)) || !gameStatePtr) {
            LogOut("[AUTO-ACTION] Failed to get game state pointer for control restore", true);
            g_p2ControlOverridden = false;
            return;
        }
        
        uintptr_t p2CharPtr = 0;
        if (!SafeReadMemory(base + EFZ_BASE_OFFSET_P2, &p2CharPtr, sizeof(uintptr_t)) || !p2CharPtr) {
            LogOut("[AUTO-ACTION] Failed to get P2 pointer for control restore, marking as restored anyway", true);
            g_p2ControlOverridden = false; // Reset flag anyway to avoid getting stuck
            return;
        }
        
        // CRITICAL: Clear buffer and immediate registers BEFORE restoring AI control
        // to prevent AI from reading stale input patterns and re-executing moves
        ClearPlayerInputBuffer(2);
        WritePlayerInputImmediate(2, 0x00);
        LogOut("[AUTO-ACTION] Cleared buffer and immediate registers before AI restore", true);
        LogFlowSequenceEvent(2, "control restore buffer cleared", restoreMoveID2, restoreMoveID2);
        
        // CRITICAL: Restore CPU control flag at game state level FIRST
        // This is what actually determines if the character is under player or AI control
        const uintptr_t P2_CPU_FLAG_OFFSET = 4932; // RIGHT shutter/side
        uint8_t cpuControlled = 1; // 1 = CPU/AI controlled
        if (SafeWriteMemory(gameStatePtr + P2_CPU_FLAG_OFFSET, &cpuControlled, sizeof(uint8_t))) {
            LogOut("[AUTO-ACTION] Restored P2 CPU control flag to 1 (AI controlled)", true);
        } else {
            LogOut("[AUTO-ACTION] Failed to restore P2 CPU control flag", true);
        }
        
        // Restore character-level AI control flag
        uint32_t before = 0xFFFFFFFFu, after = 0xFFFFFFFFu;
        SafeReadMemory(p2CharPtr + AI_CONTROL_FLAG_OFFSET, &before, sizeof(uint32_t));
        bool okWrite = SafeWriteMemory(p2CharPtr + AI_CONTROL_FLAG_OFFSET, &g_originalP2ControlFlag, sizeof(uint32_t));
        SafeReadMemory(p2CharPtr + AI_CONTROL_FLAG_OFFSET, &after, sizeof(uint32_t));
        std::ostringstream oss; oss << "[AUDIT][AI] RestoreP2ControlState @0x" << std::hex << (p2CharPtr + AI_CONTROL_FLAG_OFFSET)
                                    << std::dec << " before=" << before << " write=" << g_originalP2ControlFlag
                                    << " after=" << after << " okWrite=" << (okWrite?"1":"0");
        LogOut(oss.str(), true);
        if (okWrite) {
            LogOut("[AUTO-ACTION] P2 control restored (requested=" + std::to_string(g_originalP2ControlFlag) + ")", true);
        } else {
            LogOut("[AUTO-ACTION] Failed to write P2 control state for restore", true);
        }
        EndFlowSequence(2, "ControlRestored", restoreMoveID2);
        
        // Reset flag regardless of write success to avoid getting stuck
        g_p2ControlOverridden = false;
    }
}

// ==================================================================================
// RESTORE P2 CONTROL FLAGS ONLY (PARTIAL RESTORE - PRESERVES BUFFER)
// ==================================================================================
// This is a SPECIALIZED restore function for Counter RG scenarios.
//
// What it does:
//   1. DOES NOT stop buffer freezing (keeps motion buffer active)
//   2. DOES NOT clear input buffer (preserves pre-armed special motion)
//   3. Restores CPU control flag at game state level (gameStatePtr + 4931)
//   4. Restores AI control flag at character level (p2CharPtr + 0x42C)
//
// When to use:
//   ✅ Counter RG fast-restore (opponent attacks, need to return control but keep special queued)
//   ✅ Any scenario where you need to return control WITHOUT clearing buffered inputs
//
// When NOT to use:
//   ❌ Dashes (stale dash pattern causes infinite dash loops)
//   ❌ Regular specials/supers (stale inputs cause move repetition)
//   ❌ Normal attacks (buffer should be cleared for clean transitions)
//   ❌ Most standard auto-actions (use RestoreP2ControlState instead)
//
// CRITICAL: Still restores BOTH CPU and AI control flags to fully return control to AI.
// The only difference from RestoreP2ControlState is that it preserves the input buffer.
// ==================================================================================
static void RestoreP2ControlFlagOnly() {
    // CRITICAL: Never modify game state during online mode
    if (g_onlineModeActive.load()) {
        g_p2ControlOverridden = false;
        return;
    }

    if (!g_p2ControlOverridden) return;
    const PerFrameSample &restoreSample = GetCurrentPerFrameSample();
    short restoreMoveID2 = restoreSample.moveID2;
    uintptr_t base = GetEFZBase();
    if (!base) return;
    
    // Even for flag-only restores, neutralize motion token so AI won't pick up a half-parsed motion
    (void)NeutralizeMotionToken(2);

    // CRITICAL: Get game state pointer to restore CPU control flag
    uintptr_t gameStatePtr = 0;
    if (!SafeReadMemory(base + EFZ_BASE_OFFSET_GAME_STATE, &gameStatePtr, sizeof(uintptr_t)) || !gameStatePtr) {
        LogOut("[AUTO-ACTION] RestoreP2ControlFlagOnly: Failed to get game state pointer", true);
        g_p2ControlOverridden = false;
        return;
    }
    
    uintptr_t p2CharPtr = 0;
    if (!SafeReadMemory(base + EFZ_BASE_OFFSET_P2, &p2CharPtr, sizeof(uintptr_t)) || !p2CharPtr) {
        g_p2ControlOverridden = false;
        return;
    }
    
    // CRITICAL: Restore CPU control flag at game state level FIRST
    // This is what actually determines if the character is under player or AI control
    const uintptr_t P2_CPU_FLAG_OFFSET = 4932; // RIGHT shutter/side
    uint8_t cpuControlled = 1; // 1 = CPU/AI controlled
    if (SafeWriteMemory(gameStatePtr + P2_CPU_FLAG_OFFSET, &cpuControlled, sizeof(uint8_t))) {
        LogOut("[AUTO-ACTION] RestoreP2ControlFlagOnly: Restored P2 CPU control flag to 1 (AI controlled)", true);
    } else {
        LogOut("[AUTO-ACTION] RestoreP2ControlFlagOnly: Failed to restore P2 CPU control flag", true);
    }
    
    // Restore character-level AI control flag
    uint32_t before = 0xFFFFFFFFu, after = 0xFFFFFFFFu;
    SafeReadMemory(p2CharPtr + AI_CONTROL_FLAG_OFFSET, &before, sizeof(uint32_t));
    bool okWrite = SafeWriteMemory(p2CharPtr + AI_CONTROL_FLAG_OFFSET, &g_originalP2ControlFlag, sizeof(uint32_t));
    SafeReadMemory(p2CharPtr + AI_CONTROL_FLAG_OFFSET, &after, sizeof(uint32_t));
    std::ostringstream oss; oss << "[AUDIT][AI] RestoreP2ControlFlagOnly @0x" << std::hex << (p2CharPtr + AI_CONTROL_FLAG_OFFSET)
                                << std::dec << " before=" << before << " write=" << g_originalP2ControlFlag
                                << " after=" << after << " okWrite=" << (okWrite?"1":"0");
    LogOut(oss.str(), true);
    EndFlowSequence(2, "ControlFlagOnly", restoreMoveID2);
    
    // NOTE: We deliberately DO NOT clear the input buffer here because Counter RG needs the
    // pre-armed special motion to remain in the buffer so it can execute after RG completes.
    // The buffer will be cleared naturally when the special executes or times out.
    
    g_p2ControlOverridden = false;
}

// Specialized helper for wake macros: restore control flags but
// explicitly preserve both the input buffer and motion token so
// pre-buffered motions (e.g. DP) can still be recognized on wake.
void RestoreP2ControlFlagsPreserveBufferAndTokenForMacro() {
    // CRITICAL: Never modify game state during online mode
    if (g_onlineModeActive.load()) {
        g_p2ControlOverridden = false;
        return;
    }

    if (!g_p2ControlOverridden) return;
    const PerFrameSample &restoreSample = GetCurrentPerFrameSample();
    short restoreMoveID2 = restoreSample.moveID2;
    uintptr_t base = GetEFZBase();
    if (!base) {
        g_p2ControlOverridden = false;
        return;
    }

    // NOTE: Unlike RestoreP2ControlState/RestoreP2ControlFlagOnly, we
    // deliberately do NOT neutralize the motion token here and we do
    // NOT clear the input buffer. The wake macro path uses this to keep
    // the full DP motion history intact across the wake transition.

    uintptr_t gameStatePtr = 0;
    if (!SafeReadMemory(base + EFZ_BASE_OFFSET_GAME_STATE, &gameStatePtr, sizeof(uintptr_t)) || !gameStatePtr) {
        g_p2ControlOverridden = false;
        return;
    }

    uintptr_t p2CharPtr = 0;
    if (!SafeReadMemory(base + EFZ_BASE_OFFSET_P2, &p2CharPtr, sizeof(uintptr_t)) || !p2CharPtr) {
        g_p2ControlOverridden = false;
        return;
    }

    const uintptr_t P2_CPU_FLAG_OFFSET = 4932; // RIGHT shutter/side
    uint8_t cpuControlled = 1; // 1 = CPU/AI controlled
    if (SafeWriteMemory(gameStatePtr + P2_CPU_FLAG_OFFSET, &cpuControlled, sizeof(uint8_t))) {
        LogOut("[AUTO-ACTION] RestoreP2ControlFlagsPreserveBufferAndTokenForMacro: Restored P2 CPU control flag to 1 (AI controlled)", true);
    } else {
        LogOut("[AUTO-ACTION] RestoreP2ControlFlagsPreserveBufferAndTokenForMacro: Failed to restore P2 CPU control flag", true);
    }

    uint32_t before = 0xFFFFFFFFu, after = 0xFFFFFFFFu;
    SafeReadMemory(p2CharPtr + AI_CONTROL_FLAG_OFFSET, &before, sizeof(uint32_t));
    bool okWrite = SafeWriteMemory(p2CharPtr + AI_CONTROL_FLAG_OFFSET, &g_originalP2ControlFlag, sizeof(uint32_t));
    SafeReadMemory(p2CharPtr + AI_CONTROL_FLAG_OFFSET, &after, sizeof(uint32_t));
    std::ostringstream oss; oss << "[AUDIT][AI] RestoreP2ControlFlagsPreserveBufferAndTokenForMacro @0x" << std::hex << (p2CharPtr + AI_CONTROL_FLAG_OFFSET)
                                << std::dec << " before=" << before << " write=" << g_originalP2ControlFlag
                                << " after=" << after << " okWrite=" << (okWrite?"1":"0");
    LogOut(oss.str(), true);
    EndFlowSequence(2, "ControlFlagOnlyWakeMacro", restoreMoveID2);
    g_p2ControlOverridden = false;
}

void ProcessAutoControlRestore() {
    // CRITICAL: Never process control restoration during online mode
    if (g_onlineModeActive.load()) {
        g_p2ControlOverridden = false;
        g_pendingControlRestore.store(false);
        return;
    }

    if (!IsMatchPhase()) {
        // Likely transitioning to character select / menu; invalidate caches
        InvalidateCachedCharacterIDs("phase change");
        if (g_pendingControlRestore.load()) {
            LogOut("[AUTO-ACTION] Phase left MATCH during restore; forcing cleanup", true);
            RestoreP2ControlState();
            g_pendingControlRestore.store(false);
            g_restoreGraceCounter = 0;
        }
        return;
    }

    // If a macro just ended and we still have human override but no restore pending,
    // force a cleanup to avoid lingering override.
    if (MacroController::GetState() != MacroController::State::Replaying &&
        g_p2ControlOverridden && !g_pendingControlRestore.load() &&
        !(g_bufferFreezingActive.load() && g_activeFreezePlayer.load() == 2)) {
        LogOut("[AUTO-ACTION][MACRO] Macro inactive but override present; forcing control restore", true);
        RestoreP2ControlState();
        g_restoreGraceCounter = RESTORE_GRACE_PERIOD;
    }
    
    // CRITICAL: Handle grace period countdown. This prevents triggers from firing
    // immediately after control restore, giving character time to stabilize.
    if (g_restoreGraceCounter > 0) {
        g_restoreGraceCounter--;
        if (g_restoreGraceCounter == 0) {
            LogOut("[AUTO-ACTION] Grace period expired, clearing g_pendingControlRestore", true);
            g_pendingControlRestore.store(false);
        }
        // Don't return yet - still process restore logic in case of new interrupts
    }
    
    if (g_pendingControlRestore.load()) {
        // If there's no active override and no active buffer-freeze for P2,
        // treat this as a stale pending state and clear it without restoring.
        // This prevents redundant restores that would re-neutralize inputs
        // when AI control is already in effect.
        if (!g_p2ControlOverridden && !(g_bufferFreezingActive.load() && g_activeFreezePlayer.load() == 2)) {
            if (detailedLogging.load()) {
                LogOut("[AUTO-ACTION] Clearing stale g_pendingControlRestore (no override/buffer-freeze)", true);
            }
            g_pendingControlRestore.store(false);
            return;
        }

        // Use unified per-frame sample instead of raw address caching for MOVE_ID
        static int s_throttle = 0;
        static short s_prevMoveID2 = 0; // track previous for dash entry detection
        // NOTE: We originally throttled sampling to 96Hz (every other frame) when not in fast-restore mode
        // to reduce overhead. Kaori's forward dash start (ID 250) appears to be extremely short-lived (often 1 frame),
        // causing us to MISS it under throttling which then triggers a 120f requeue loop.
        // Fix: Disable throttle for Kaori so we sample every frame until dash start detected.
        // (We can't know the character yet until after resolving base & cached IDs, so we move throttle decision below.)
        bool deferThrottleDecision = true; // placeholder flag so later logic can decide to early-return

        uintptr_t base = GetEFZBase();
        if (!base) return;
        const PerFrameSample &dashSample = GetCurrentPerFrameSample();
        short moveID2 = dashSample.moveID2;
        short oppMoveID = dashSample.moveID1;

        // Ensure cached character IDs (once per MATCH phase) BEFORE deciding on throttle
        EnsureCachedCharacterIDs(base);
        bool isKaori = (g_cachedP2CharID == CHAR_ID_KAORI);

        if (!g_crgFastRestore.load()) {
            // While a dash was just queued and we're waiting for dash start, sample every frame to avoid missing
            // short-lived start IDs (e.g., immediate transitions). Kaori also remains unthrottled.
            bool waitingDash = g_recentDashQueued.load();
            if (!waitingDash) {
                // Only apply throttle if NOT Kaori. Kaori remains unthrottled due to ID 250 being very brief.
                if (!isKaori) {
                    if ((s_throttle++ & 1) != 0) return; // 96 Hz sampling for non-Kaori normal path
                } else if ((s_throttle % 192) == 0 && detailedLogging.load()) {
                    LogOut("[AUTO-ACTION][DASH][KAORI] Full-rate sampling active (disabling throttle to catch ID 250)", true);
                }
            }
            if (ValidationMetricsEnabled() && g_recentDashQueued.load()) { GetValidationMetrics().dashQueued++; }
        }

        // DASH RESTORE RULES:
        // 1. If we just entered forward/back dash start and no follow-up configured -> immediate restore
        // 2. If any normal (>=200) starts after a dash-follow sequence -> restore
        // 3. If dash cancelled into blockstun/hitstun/airtech -> restore
        // 4. Fallback to existing timeout / move completion logic
    // isKaori already computed above
    bool dashStartNow = (moveID2 == FORWARD_DASH_START_ID || moveID2 == BACKWARD_DASH_START_ID || (isKaori && moveID2 == KAORI_FORWARD_DASH_START_ID));
    bool dashStartJustEntered = dashStartNow && (s_prevMoveID2 != moveID2);
    if (ValidationMetricsEnabled() && dashStartJustEntered) { GetValidationMetrics().dashStartDetected++; }
    // Treat Kaori's forward dash start (250) as a dash state, NOT a normal. Only consider a "dash normal" when the new move
    // ID is a normal/special (>=200) AND it is not part of any dash start/recovery sequence.
    bool dashNormalStarted = (moveID2 >= 200);
    bool moveIsDash = (moveID2 == FORWARD_DASH_START_ID || moveID2 == FORWARD_DASH_RECOVERY_ID ||
               moveID2 == FORWARD_DASH_RECOVERY_SENTINEL_ID || moveID2 == BACKWARD_DASH_START_ID ||
               moveID2 == BACKWARD_DASH_RECOVERY_ID || (isKaori && moveID2 == KAORI_FORWARD_DASH_START_ID));
    // Recompute dashNormalStarted excluding any dash state (prevents Kaori 250 from being misclassified)
    if (dashNormalStarted && moveIsDash) dashNormalStarted = false;
    bool dashCancelled = (!moveIsDash) && (IsBlockstun(moveID2) || IsHitstun(moveID2) || IsAirtech(moveID2));
        // HARD GUARD (informational): If we just queued a dash and have NOT yet observed the dash start ID
    // (163 forward, 165 back, or 250 Kaori), we will suppress generic restore later. Keep timeout >=90.
    bool waitingDashStart = g_recentDashQueued.load() && !dashStartNow;
    if (waitingDashStart && detailedLogging.load()) {
        LogOut("[AUTO-ACTION][DASH] Holding restore (pre-start): queuedDash & dashStart not yet observed", true);
    }
    
        // CRITICAL: If we queued a dash and got INTERRUPTED BEFORE any dash start ID (163/165/250) appears,
        // immediately restore control and clear the queued-dash guard. This avoids an infinite pre-start hold
        // when we are put into blockstun/hitstun/airtech or other non-dash states by the opponent.
        if (g_recentDashQueued.load() && !dashStartNow && dashCancelled) {
            LogOut("[AUTO-ACTION][DASH] Pre-start interrupted (hit/block/airtech) - cancelling dash and restoring control", true);
            if (ValidationMetricsEnabled()) { GetValidationMetrics().dashPreStartInterrupts++; }
            // Cancel any pending follow-up as it will never fire now
            if (g_dashDeferred.pendingSel.load() > 0) {
                g_dashDeferred.pendingSel.store(0);
                g_dashDeferred.dashStartLatched.store(-1);
            }
            g_recentDashQueued.store(false);
            RestoreP2ControlState();
            if (ValidationMetricsEnabled()) { GetValidationMetrics().dashRestoreEvents++; }
            // Allow immediate re-arming on the next actionable window (second hit scenario):
            // clear trigger active/cooldown so After Block/Hitstun can fire even if the gap is small (3-4F).
            p2TriggerActive = false;
            p2TriggerCooldown = 0;
            g_restoreGraceCounter = RESTORE_GRACE_PERIOD;
            g_crgFastRestore.store(false);
            g_lastP2MoveID.store(-1);
            // Clear pending restore so we don't keep sampling in a dead loop
            g_pendingControlRestore.store(false);
            s_prevMoveID2 = moveID2;
            return;
        }
        // If we had only scheduled a follow-up but no longer waiting a queued dash, clear follow-up quietly.
        if (g_dashDeferred.pendingSel.load() > 0 && dashCancelled && !g_recentDashQueued.load()) {
            LogOut("[AUTO-ACTION][DASH] Dash follow-up cancelled by interrupt before dash started", true);
            if (ValidationMetricsEnabled()) { GetValidationMetrics().dashFollowupCancelled++; }
            g_dashDeferred.pendingSel.store(0);
            g_dashDeferred.dashStartLatched.store(-1);
        }
    int fdfSel = forwardDashFollowup.load();
        // CRITICAL FIX: Don't restore immediately when dash starts (163/165).
        // Wait until we've LEFT the dash state entirely, regardless of what state we transition to:
        // - Natural completion: 163 → 0/1/2 (idle/walk)
        // - Dash normal: 163 → attack moveID (200+)
        // - Interrupted by hit: 163 → hitstun (50-71)
        // - Canceled into special: 163 → special moveID
    // Consider both start and recovery IDs as "dashing" so we can detect leaving the entire dash state cleanly
    bool wasDashing = (s_prevMoveID2 == FORWARD_DASH_START_ID || s_prevMoveID2 == FORWARD_DASH_RECOVERY_ID ||
                       s_prevMoveID2 == FORWARD_DASH_RECOVERY_SENTINEL_ID ||
                       s_prevMoveID2 == BACKWARD_DASH_START_ID || s_prevMoveID2 == BACKWARD_DASH_RECOVERY_ID ||
                       (isKaori && s_prevMoveID2 == KAORI_FORWARD_DASH_START_ID));
    bool stillDashing = (moveID2 == FORWARD_DASH_START_ID || moveID2 == BACKWARD_DASH_START_ID ||
                         moveID2 == FORWARD_DASH_RECOVERY_ID || moveID2 == BACKWARD_DASH_RECOVERY_ID ||
                         (isKaori && moveID2 == KAORI_FORWARD_DASH_START_ID));
        // Track dash start observation per attempt to avoid stale prev-move causing false "left dash" restores
        static bool s_dashStartSeenThisAttempt = false;
        // If a new dash attempt was just queued (tracked below), we'll reset this flag
        if (dashStartJustEntered) {
            s_dashStartSeenThisAttempt = true;
        }
        bool leftDashState = s_dashStartSeenThisAttempt && wasDashing && !stillDashing;
        
        bool doDashRestore = false;
        
        // PRIORITY 1: If we left the dash state entirely (163/165 → anything else), restore immediately.
        // This handles all transitions: natural completion, dash normals, cancels, interrupts, etc.
        if (leftDashState && !waitingDashStart) {
            LogOut("[AUTO-ACTION][DASH] Restore: left dash state (163/165/250 → " + std::to_string(moveID2) + ")", true);
            doDashRestore = true;
        }
        // PRIORITY 2: Legacy paths for edge cases
        else if (dashNormalStarted && wasDashing && s_dashStartSeenThisAttempt) {
            LogOut("[AUTO-ACTION][DASH] Restore: dash normal detected (moveID=" + std::to_string(moveID2) + ")", true);
            doDashRestore = true;
        } else if (dashCancelled && wasDashing && s_dashStartSeenThisAttempt) {
            LogOut("[AUTO-ACTION][DASH] Restore: dash cancelled by state (moveID=" + std::to_string(moveID2) + ")", true);
            doDashRestore = true;
        } else if (moveID2 == FORWARD_DASH_RECOVERY_SENTINEL_ID && s_dashStartSeenThisAttempt) {
            LogOut("[AUTO-ACTION][DASH] Restore: sentinel recovery ID detected (178)", true);
            doDashRestore = true;
        }

        if (doDashRestore) {
            // Per-frame debounce to avoid multiple restores & log spam when logic re-enters in the same frame
            static int s_lastDashRestoreFrame = -1;
            int curF = frameCounter.load();
            if (s_lastDashRestoreFrame != curF) {
                RestoreP2ControlState();
                s_lastDashRestoreFrame = curF;
                // Clear any leftover dash queued flag so we don't immediately re-arm
                g_recentDashQueued.store(false);
            }
            // Grace period disabled; clear pending immediately after restore.
            g_restoreGraceCounter = RESTORE_GRACE_PERIOD;
            g_pendingControlRestore.store(false);
            g_crgFastRestore.store(false);
            g_lastP2MoveID.store(-1);
            s_prevMoveID2 = moveID2; // update before return
            return;
        }

        // PRE-DASH START GUARD: if we queued a forward dash but haven't seen any non-zero move yet, delay restore
        static bool sawNonZeroMoveID = false; // Tracks if ANY non-zero move id observed during the CURRENT pending restore attempt
        // NEW: Tag dash attempts by the frame they were queued so stale state from a previous attempt
        // (where we already saw a non-zero move) cannot leak into a fresh attempt and cause an
        // immediate generic restore (moveChangedCandidate) before the dash even starts.
        static int s_lastDashQueueFrame = -1;
        int dashQueueFrame = g_recentDashQueuedFrame.load();
        if (g_recentDashQueued.load() && dashQueueFrame != s_lastDashQueueFrame) {
            // New dash attempt detected -> reset observation flag
            sawNonZeroMoveID = false;
            s_lastDashQueueFrame = dashQueueFrame;
            s_dashStartSeenThisAttempt = false;
            if (detailedLogging.load()) {
                LogOut("[AUTO-ACTION][DASH] New dash attempt detected; resetting sawNonZeroMoveID", true);
            }
        }
        // No timeout-based fallback: rely on dash age and state only
    if (g_recentDashQueued.load() && moveID2 == 0 && !sawNonZeroMoveID) {
            int age = frameCounter.load() - g_recentDashQueuedFrame.load();
            extern int p2CurrentMotionType; extern bool p2QueueActive; extern int p2QueueIndex; extern int p2FrameCounter; extern std::vector<InputFrame> p2InputQueue;
            int qSize = (int)p2InputQueue.size();
            // Throttle noisy trace logs behind detailedLogging to reduce idle overhead
            if (detailedLogging.load()) {
                static int s_nextDashTraceLogFrame = 0; // ~8 logs/sec at 192Hz
                int nowF = frameCounter.load();
                if (nowF >= s_nextDashTraceLogFrame) {
                    s_nextDashTraceLogFrame = nowF + 24; // 24 internal frames ~125ms
                    LogOut(std::string("[AUTO-ACTION][DASH][TRACE] waiting age=") + std::to_string(age) +
                           " queueActive=" + (p2QueueActive?"1":"0") +
                           " qIdx=" + std::to_string(p2QueueIndex) + "/" + std::to_string(qSize) +
                           " frameInStep=" + std::to_string(p2FrameCounter) +
                           " motionType=" + std::to_string(p2CurrentMotionType), true);
                    if (p2QueueActive && p2QueueIndex < qSize) {
                        uint8_t mask = p2InputQueue[p2QueueIndex].inputMask;
                        LogOut(std::string("[AUTO-ACTION][DASH][TRACE] current mask=") + std::to_string((int)mask), true);
                    }
                }
            }
            if (age < 120) {
                s_prevMoveID2 = moveID2; return; // hold restore
            } else if (age == 120) {
                // Requeue the same dash type that was attempted (forward or back)
                extern int p2CurrentMotionType;
                bool wasBack = (p2CurrentMotionType == MOTION_BACK_DASH);
                LogOut(std::string("[AUTO-ACTION][DASH] Requeue ") + (wasBack?"backdash":"forward dash") + " after 120f no-start", true);
                QueueMotionInput(2, wasBack ? MOTION_BACK_DASH : MOTION_FORWARD_DASH, 0);
                g_recentDashQueuedFrame.store(frameCounter.load());
                s_prevMoveID2 = moveID2; return;
            } else if (age < 160) {
                s_prevMoveID2 = moveID2; return; // continue holding
            } else {
                LogOut("[AUTO-ACTION][DASH] Abandon wait; dash never produced non-zero moveID", true);
                g_recentDashQueued.store(false);
            }
        }
        // (Reverted timing expansion) Do not add extra pre-start requeue/abandon paths here.

        // Removed timeout countdown and related logging; rely on move/state transitions only

        if (moveID2 > 0) {
            sawNonZeroMoveID = true;
            // Only clear the queued-dash guard once we actually see a dash start ID.
            if (dashStartNow) {
                g_recentDashQueued.store(false);
            }
        }

        // Counter RG fast-restore: if opponent enters attack frames (>=200) AND we are actionable,
        // restore immediately with control-flag-only to preserve the buffered special. Fallback: if
        // our move starts (non-RG state), perform full restore.
        if (g_crgFastRestore.load() && g_counterRGEnabled.load()) {
            bool opponentAttacking = (oppMoveID >= 200);
            bool moveStarted = (moveID2 > 0 && moveID2 != RG_STAND_ID && moveID2 != RG_CROUCH_ID && moveID2 != RG_AIR_ID);
            bool p2ActionableNow = IsActionable(moveID2);
            bool isRGPrearmedSpecial = s_p2RGPrearmIsSpecial; // true when we pre-armed a special at RG entry
            if (opponentAttacking && p2ActionableNow) {
                if (!isRGPrearmedSpecial) {
                    LogOut("[AUTO-ACTION][CRG] Early restore: opponent entered attack frames", true);
                    RestoreP2ControlFlagOnly();
                    // Grace period disabled; clear pending immediately.
                    g_restoreGraceCounter = RESTORE_GRACE_PERIOD;
                    g_pendingControlRestore.store(false);
                    g_crgFastRestore.store(false);
                    g_lastP2MoveID.store(-1);
                    sawNonZeroMoveID = false;
                    return;
                } else {
                    // For specials, defer restore until the move actually starts to avoid losing the final button press
                    if (detailedLogging.load()) {
                        LogOut("[AUTO-ACTION][CRG] Opponent attacking, but special pre-armed: deferring restore until move starts", true);
                    }
                }
            }
            if (moveStarted) {
                LogOut("[AUTO-ACTION][CRG] Early restore: special started after RG", true);
                RestoreP2ControlState();
                // Grace period disabled; clear pending immediately.
                g_restoreGraceCounter = RESTORE_GRACE_PERIOD;
                g_pendingControlRestore.store(false);
                g_crgFastRestore.store(false);
                g_lastP2MoveID.store(-1);
                sawNonZeroMoveID = false;
                p2TriggerCooldown = 0; p2TriggerActive = false;
                return;
            }
        }

    // A dash-specific guard: if we queued a forward/back dash and never observed ANY non-zero move id yet,
    // treat the transition back to 0 as non-completion (prevents premature restore destroying dash attempt).
    // Updated: treat any queued-dash prior to actual dash start as a no-restore condition,
    // even if the engine briefly shows non-zero idle/actionable states.
    bool queuedDashNoStart = waitingDashStart;
    // Also suppress generic restore while the dash motion queue is actively writing frames.
    extern bool p2QueueActive;
    bool dashQueueActiveNow = p2QueueActive;
    bool moveChangedCandidate = (moveID2 != g_lastP2MoveID.load() && moveID2 == 0 && sawNonZeroMoveID);
    bool dashFollowPending = (g_dashDeferred.pendingSel.load() > 0);
    bool suppressGenericRestore = (moveChangedCandidate && dashFollowPending);
        if (suppressGenericRestore) {
            // Diagnostic: show we deliberately ignored a generic completion while a dash follow-up is pending
            LogOut("[AUTO-ACTION][DASH] Suppressing generic restore (pending follow-up active)", detailedLogging.load());
        }
        bool moveChanged = moveChangedCandidate && !suppressGenericRestore;
    // No timeout path; only state-change driven restores
        
        // If pending restore flag is set but we are no longer actually overriding control (and not in grace),
        // clear it defensively to avoid indefinite sampling spam.
        if (g_pendingControlRestore.load() && !g_p2ControlOverridden && g_restoreGraceCounter == 0 &&
            !dashFollowPending && !g_recentDashQueued.load()) {
            LogOut("[AUTO-ACTION] Clearing stale g_pendingControlRestore (no override active)", detailedLogging.load());
            g_pendingControlRestore.store(false);
            s_prevMoveID2 = moveID2;
            return;
        }

        // PRE-START CANCEL FALLBACK (covers Kaori and brief dash-start cases):
        // If we queued a dash, the queue is now inactive, and we've seen a sustained non-dash, non-zero state
        // for a short window without ever observing a dash start ID, cancel and restore to avoid indefinite hold.
        if (waitingDashStart && !dashQueueActiveNow && moveID2 != 0 && !moveIsDash) {
            int ageSinceQueue = frameCounter.load() - g_recentDashQueuedFrame.load();
            if (ageSinceQueue >= 12) { // ~1/6 sec at 192 Hz; enough to rule out 1F start IDs
                LogOut("[AUTO-ACTION][DASH] Pre-start cancelled (no start observed, non-dash state persisted) - restoring", true);
                // Cancel any pending follow-up as it will never fire now
                if (g_dashDeferred.pendingSel.load() > 0) {
                    g_dashDeferred.pendingSel.store(0);
                    g_dashDeferred.dashStartLatched.store(-1);
                }
                g_recentDashQueued.store(false);
                RestoreP2ControlState();
                g_restoreGraceCounter = RESTORE_GRACE_PERIOD;
                g_crgFastRestore.store(false);
                g_lastP2MoveID.store(-1);
                g_pendingControlRestore.store(false);
                s_prevMoveID2 = moveID2;
                return;
            } else {
                // Briefly hold to see if dash start appears
                s_prevMoveID2 = moveID2;
                return;
            }
        }

        // Early restore: as soon as the move actually starts (first non-zero, non-RG, non-dash state), restore immediately.
        // This covers specials/supers even if their first startup IDs are outside the >=200 attack range.
        bool restorePendingNow = g_pendingControlRestore.load();
        // Only allow early restore when we actually overrode control or P2 is in an active freeze.
        bool allowEarlyRestore = (g_p2ControlOverridden || (g_bufferFreezingActive.load() && g_activeFreezePlayer.load() == 2));
        // keep trackable move only for diagnostics; don't use it to authorize restore
        bool haveTrackableMove = (g_lastP2MoveID.load() != -1);
        bool nonRGState = (moveID2 != RG_STAND_ID && moveID2 != RG_CROUCH_ID && moveID2 != RG_AIR_ID);
        // Early-restore only when the move becomes ACTIVE:
        // - Specials: first active frame assumed at moveID >= 250
        // - Supers:   first active frame assumed at moveID >= 300
        // Dashes are handled elsewhere (dash start IDs), so keep excluding dash here.
        bool specialActiveNow = (moveID2 >= 250 && moveID2 < 300);
        bool superActiveNow   = (moveID2 >= 300);
        bool activeNow = (specialActiveNow || superActiveNow) && nonRGState && !moveIsDash;
        bool stateEnteredNow = (s_prevMoveID2 != moveID2);
        // Tighten gating: require actual override or active freeze to avoid spurious restores
        if (restorePendingNow && allowEarlyRestore && stateEnteredNow && activeNow &&
            !dashFollowPending && !queuedDashNoStart && !g_recentDashQueued.load() && !dashQueueActiveNow) {
            LogOut("[AUTO-ACTION] Early restore on active frame (special>=250|super>=300) moveID=" + std::to_string(moveID2), true);
            RestoreP2ControlState();
            g_restoreGraceCounter = RESTORE_GRACE_PERIOD;
            g_crgFastRestore.store(false);
            g_lastP2MoveID.store(-1);
            sawNonZeroMoveID = false;
            s_prevMoveID2 = moveID2;
            // Grace disabled; pending restore no longer needed.
            g_pendingControlRestore.store(false);
            g_pendingRestoreTimestamp.store(0);
            p2TriggerCooldown = 0; p2TriggerActive = false;
            return;
        }

        // Fallback: if we still have a pending restore and human override, and the buffer-freeze is no longer
        // active for P2 (owner released), perform an immediate restore. This covers cases where the active-frame
        // early-restore window was missed while the freeze thread was still pushing frames, and avoids waiting
        // for moveID to return to 0 (which we no longer use as a restore signal).
        // Grace period: Don't trigger fallback restore within 20ms of setting g_pendingControlRestore to allow
        // buffer freeze thread time to initialize and execute the motion.
        bool freezeActiveForP2 = (g_bufferFreezingActive.load() && g_activeFreezePlayer.load() == 2);
        DWORD restoreTimestamp = g_pendingRestoreTimestamp.load();
        DWORD currentTime = GetTickCount();
        bool gracePeriodElapsed = (restoreTimestamp == 0) || ((currentTime - restoreTimestamp) >= 20);
        if (g_pendingControlRestore.load() && g_p2ControlOverridden && !freezeActiveForP2 && !dashQueueActiveNow && gracePeriodElapsed) {
            LogOut("[AUTO-ACTION] Fallback restore: freeze ended, pending restore active", true);
            RestoreP2ControlState();
            g_restoreGraceCounter = RESTORE_GRACE_PERIOD; // 0, kept for symmetry/logging
            g_crgFastRestore.store(false);
            g_lastP2MoveID.store(-1);
            g_pendingControlRestore.store(false);
            g_pendingRestoreTimestamp.store(0);
            sawNonZeroMoveID = false;
            p2TriggerCooldown = 0; p2TriggerActive = false;
            s_prevMoveID2 = moveID2;
            return;
        }

        // Generic restore: require pending flag + valid context to avoid repeated restores when already clean.
        restorePendingNow = g_pendingControlRestore.load();
        haveTrackableMove = (g_lastP2MoveID.load() != -1);
    static int s_lastGenericRestoreFrame = -1; // dedupe generic restore per frame
    bool allowAnyRestore = (g_p2ControlOverridden || (g_bufferFreezingActive.load() && g_activeFreezePlayer.load() == 2));
    // Never use generic restore when the move returns to 0; we restore earlier on active frames or via dash/CRG paths.
    bool baseRestoreCond = moveChanged && (moveID2 != 0) &&
                !dashFollowPending &&
                !queuedDashNoStart &&
                !g_recentDashQueued.load() &&
                !dashQueueActiveNow &&
                restorePendingNow &&
                allowAnyRestore;
    bool shouldGenericRestore = baseRestoreCond && (frameCounter.load() != s_lastGenericRestoreFrame);

        if (shouldGenericRestore) {
            LogOut("[AUTO-ACTION] Auto-restoring P2 control state after move execution", detailedLogging.load());
         LogOut("[AUTO-ACTION] Reason: Move completed, MoveID: " + std::to_string(moveID2), detailedLogging.load());

            RestoreP2ControlState();
            // Grace disabled; clear pending immediately.
            g_restoreGraceCounter = RESTORE_GRACE_PERIOD;
            g_pendingControlRestore.store(false);
            g_pendingRestoreTimestamp.store(0);
            g_crgFastRestore.store(false);
            g_lastP2MoveID.store(-1);
            sawNonZeroMoveID = false;
            s_lastGenericRestoreFrame = frameCounter.load();

            // If we just restored after a dash and a forward dash follow-up is still pending,
            // keep a minimal cooldown (1 *internal* frame => ~instant visually) so we do not
            // immediately retrigger the same After Block while the dash follow-up logic runs.
            p2TriggerCooldown = 0; // allow prompt re-trigger after restoring
            p2TriggerActive = false;
            LogOut("[AUTO-ACTION] Cleared P2 trigger cooldown after restore", detailedLogging.load());
        } else {
            if (moveChanged && restorePendingNow && !shouldGenericRestore && detailedLogging.load()) {
                LogOut(std::string("[AUTO-ACTION][TRACE] Suppressed generic restore: pending=") + (restorePendingNow?"1":"0") +
                       " override=" + (g_p2ControlOverridden?"1":"0") +
                       " lastMoveValid=" + (haveTrackableMove?"1":"0") +
                       " dashFollow=" + (dashFollowPending?"1":"0") +
                       " queuedDashNoStart=" + (queuedDashNoStart?"1":"0") +
                       " recentDashQueued=" + (g_recentDashQueued.load()?"1":"0") +
                       " dashQueueActive=" + (dashQueueActiveNow?"1":"0"), true);
            }
            if (moveID2 != 0) {
                g_lastP2MoveID.store(moveID2);
            }
            if (dashFollowPending && moveChangedCandidate) {
                if (detailedLogging.load()) {
                    LogOut("[AUTO-ACTION][DASH] Suppressed restore while follow-up pending (moveID2=0)", true);
                }
            }
            if (queuedDashNoStart && detailedLogging.load()) {
                LogOut("[AUTO-ACTION][DASH] Holding restore: dash queue completed but no dash start MoveID observed yet", true);
            }
        }
        s_prevMoveID2 = moveID2; // remember last sampled
    }
}

bool AutoGuard(int playerNum, int opponentPtr) {
    uintptr_t playerPtr = GetPlayerPointer(playerNum);
    if (!playerPtr || !opponentPtr)
        return false;
        
    // Get opponent's current move ID
    short moveID = 0;
    if (!SafeReadMemory(opponentPtr + 0x8, &moveID, sizeof(short)) || moveID <= 0)
        return false;
        
    // Log what move we're trying to block
    LogOut("[AUTO_GUARD] Attempting to block move ID: " + std::to_string(moveID), true);
    
    // Get attack height (disabled AttackReader use: assume mid as a safe default)
    AttackHeight height = ATTACK_HEIGHT_MID;
    
    // Check if in air
    double yPos = 0.0;
    SafeReadMemory(playerPtr + 40, &yPos, sizeof(double));
    bool inAir = (yPos < 0.0);
    
    // Determine block stance
    bool playerFacingRight = GetPlayerFacingDirection(playerNum);
    uint8_t blockInput = 0;
    
    LogOut("[AUTO_GUARD] Attack height: " + std::to_string(height) + 
           ", Player in air: " + (inAir ? "yes" : "no"), true);
    
    switch (height) {
        case ATTACK_HEIGHT_LOW:
            // Must crouch block
            blockInput = GAME_INPUT_DOWN | (playerFacingRight ? GAME_INPUT_LEFT : GAME_INPUT_RIGHT);
            LogOut("[AUTO_GUARD] Using crouch block for low attack", true);
            break;
            
        case ATTACK_HEIGHT_HIGH:
            if (inAir) {
                // Air block
                blockInput = playerFacingRight ? GAME_INPUT_LEFT : GAME_INPUT_RIGHT;
                LogOut("[AUTO_GUARD] Using air block for high attack", true);
            } else {
                // Stand block
                blockInput = playerFacingRight ? GAME_INPUT_LEFT : GAME_INPUT_RIGHT;
                LogOut("[AUTO_GUARD] Using stand block for high attack", true);
            }
            break;
            
        case ATTACK_HEIGHT_MID:
            if (inAir) {
                // Air block
                blockInput = playerFacingRight ? GAME_INPUT_LEFT : GAME_INPUT_RIGHT;
                LogOut("[AUTO_GUARD] Using air block for mid attack", true);
            } else {
                // Either stand or crouch block works, prefer crouch
                blockInput = GAME_INPUT_DOWN | (playerFacingRight ? GAME_INPUT_LEFT : GAME_INPUT_RIGHT);
                LogOut("[AUTO_GUARD] Using crouch block for mid attack", true);
            }
            break;
            
        case ATTACK_HEIGHT_THROW:
            LogOut("[AUTO_GUARD] Cannot block unblockable attack", true);
            return false;
            
        default:
            LogOut("[AUTO_GUARD] Unknown attack height", true);
            return false;
    }
    
    // Apply block input
    return WritePlayerInput(playerPtr, blockInput);
}

// Cancel active auto-actions and macros without disabling trigger settings.
// Resets execution state so triggers can fire again cleanly after state/position changes.
void CancelAutoActionsAndMacros() {
    std::ostringstream logStream;
    logStream << "[CANCEL] === BEGIN CancelAutoActionsAndMacros ===";
    LogOut(logStream.str(), true);

    // =========================================================================
    // 1. MACRO STATE CAPTURE AND CANCEL
    // =========================================================================
    MacroController::State macroSt = MacroController::GetState();
    int macroSlot = MacroController::GetCurrentSlot();
    
    logStream.str(""); logStream.clear();
    logStream << "[CANCEL][MACRO] Current state: "
              << (macroSt == MacroController::State::Idle ? "Idle" :
                  macroSt == MacroController::State::PreRecord ? "PreRecord" :
                  macroSt == MacroController::State::Recording ? "Recording" : "Replaying")
              << " slot=" << macroSlot;
    LogOut(logStream.str(), true);
    
    if (macroSt != MacroController::State::Idle) {
        // Use UnswapThenStop for PreRecord/Recording to properly restore default mapping
        // (same pattern as backing to Character Select) 
        if (macroSt == MacroController::State::PreRecord || macroSt == MacroController::State::Recording) {
            MacroController::UnswapThenStop();
            logStream.str(""); logStream.clear();
            logStream << "[CANCEL][MACRO] Stopped PreRecord/Recording with UnswapThenStop";
            LogOut(logStream.str(), true);
            // Show "Macro: Cancelled" with darker red than Recording color (255,80,80)
            DirectDrawHook::AddMessage("Macro: Cancelled", "MACRO", RGB(180, 40, 40), 1200, 0, 120);
        } else {
            MacroController::Stop();
            logStream.str(""); logStream.clear();
            logStream << "[CANCEL][MACRO] Stopped active macro playback";
            LogOut(logStream.str(), true);
            // Show cancellation message for replay as well
            DirectDrawHook::AddMessage("Macro: Cancelled", "MACRO", RGB(180, 40, 40), 1200, 0, 120);
        }
    }

    // =========================================================================
    // 2. DELAY STATE CAPTURE AND RESET
    // =========================================================================
    logStream.str(""); logStream.clear();
    logStream << "[CANCEL][DELAY] P1: isDelaying=" << (p1DelayState.isDelaying ? "true" : "false")
              << " framesRemaining=" << p1DelayState.delayFramesRemaining
              << " triggerType=" << p1DelayState.triggerType;
    LogOut(logStream.str(), true);
    
    logStream.str(""); logStream.clear();
    logStream << "[CANCEL][DELAY] P2: isDelaying=" << (p2DelayState.isDelaying ? "true" : "false")
              << " framesRemaining=" << p2DelayState.delayFramesRemaining
              << " triggerType=" << p2DelayState.triggerType;
    LogOut(logStream.str(), true);
    
    p1DelayState = {false, 0, TRIGGER_NONE, 0, -1, -1, 0, -1, -1};
    p2DelayState = {false, 0, TRIGGER_NONE, 0, -1, -1, 0, -1, -1};
    LogOut("[CANCEL][DELAY] Reset both delay states", true);

    // =========================================================================
    // 3. ACTION APPLIED FLAGS RESET
    // =========================================================================
    logStream.str(""); logStream.clear();
    logStream << "[CANCEL][FLAGS] p1ActionApplied=" << (p1ActionApplied ? "true" : "false")
              << " p2ActionApplied=" << (p2ActionApplied ? "true" : "false");
    LogOut(logStream.str(), true);
    
    p1ActionApplied = false;
    p2ActionApplied = false;

    // =========================================================================
    // 4. TRIGGER FEEDBACK RESET + SET CANCELLED FLAG
    // =========================================================================
    int lastTrigger = g_lastActiveTriggerType.load();
    int lastFrame = g_lastActiveTriggerFrame.load();
    logStream.str(""); logStream.clear();
    logStream << "[CANCEL][TRIGGER] lastActiveTriggerType=" << lastTrigger
              << " lastActiveTriggerFrame=" << lastFrame;
    LogOut(logStream.str(), true);
    
    g_lastActiveTriggerType.store(TRIGGER_NONE);
    g_lastActiveTriggerFrame.store(0);
    
    // Set the cancelled flag to trigger red flash on triggers for ~0.5 seconds
    g_triggersCancelledActive.store(true);
    g_triggersCancelledFrame.store(frameCounter.load());
    LogOut("[CANCEL][TRIGGER] Set triggers cancelled flag for red flash feedback", true);

    // =========================================================================
    // 5. COOLDOWNS RESET
    // =========================================================================
    logStream.str(""); logStream.clear();
    logStream << "[CANCEL][COOLDOWN] p1TriggerActive=" << (p1TriggerActive ? "true" : "false")
              << " p1Cooldown=" << p1TriggerCooldown
              << " p2TriggerActive=" << (p2TriggerActive ? "true" : "false")
              << " p2Cooldown=" << p2TriggerCooldown;
    LogOut(logStream.str(), true);
    
    p1TriggerActive = false; p1TriggerCooldown = 0;
    p2TriggerActive = false; p2TriggerCooldown = 0;

    // =========================================================================
    // 6. P2 CONTROL STATE RESTORE
    // =========================================================================
    bool wasOverridden = g_p2ControlOverridden;
    uint32_t origFlag = g_originalP2ControlFlag;
    bool pendingRestore = g_pendingControlRestore.load();
    
    logStream.str(""); logStream.clear();
    logStream << "[CANCEL][CONTROL] g_p2ControlOverridden=" << (wasOverridden ? "true" : "false")
              << " g_originalP2ControlFlag=" << origFlag
              << " g_pendingControlRestore=" << (pendingRestore ? "true" : "false");
    LogOut(logStream.str(), true);
    
    if (g_p2ControlOverridden) {
        RestoreP2ControlState();
        LogOut("[CANCEL][CONTROL] Restored P2 control state", true);
    }
    g_pendingControlRestore.store(false);
    g_pendingRestoreTimestamp.store(0);
    g_lastP2MoveID.store(-1);
    g_crgFastRestore.store(false);

    // =========================================================================
    // 7. INPUT BUFFER FREEZE STOP
    // =========================================================================
    extern std::atomic<bool> g_bufferFreezingActive;
    extern std::atomic<int> g_activeFreezePlayer;
    bool freezeActive = g_bufferFreezingActive.load();
    int freezePlayer = g_activeFreezePlayer.load();
    
    logStream.str(""); logStream.clear();
    logStream << "[CANCEL][FREEZE] g_bufferFreezingActive=" << (freezeActive ? "true" : "false")
              << " g_activeFreezePlayer=" << freezePlayer;
    LogOut(logStream.str(), true);
    
    StopBufferFreezing();

    // =========================================================================
    // 8. RG PENDING/PRE-ARM STATE RESET
    // =========================================================================
    logStream.str(""); logStream.clear();
    logStream << "[CANCEL][RG] P1: pending=" << (s_p1RGPending ? "true" : "false")
              << " prearm=" << (s_p1RGPrearmed ? "true" : "false")
              << " P2: pending=" << (s_p2RGPending ? "true" : "false")
              << " prearm=" << (s_p2RGPrearmed ? "true" : "false");
    LogOut(logStream.str(), true);
    
    s_p1RGPending = false; s_p1RGExpiry = 0;
    s_p2RGPending = false; s_p2RGExpiry = 0;
    s_p1RGPrearmed = false; s_p1RGPrearmIsSpecial = false; s_p1RGPrearmActionType = -1; s_p1RGPrearmExpiry = 0;
    s_p2RGPrearmed = false; s_p2RGPrearmIsSpecial = false; s_p2RGPrearmActionType = -1; s_p2RGPrearmExpiry = 0;

    // =========================================================================
    // 9. WAKE PRE-ARM STATE RESET
    // =========================================================================
    logStream.str(""); logStream.clear();
    logStream << "[CANCEL][WAKE] P1: prearm=" << (s_p1WakePrearmed ? "true" : "false")
              << " isMacro=" << (s_p1WakePrearmIsMacro ? "true" : "false")
              << " 96count=" << s_p1WakeMoveID96FrameCount
              << " frozen=" << (s_p1WakeBufferFrozen ? "true" : "false");
    LogOut(logStream.str(), true);
    
    logStream.str(""); logStream.clear();
    logStream << "[CANCEL][WAKE] P2: prearm=" << (s_p2WakePrearmed ? "true" : "false")
              << " isMacro=" << (s_p2WakePrearmIsMacro ? "true" : "false")
              << " 96count=" << s_p2WakeMoveID96FrameCount
              << " frozen=" << (s_p2WakeBufferFrozen ? "true" : "false");
    LogOut(logStream.str(), true);
    
    s_p1WakePrearmed = false; s_p1WakePrearmExpiry = 0; s_p1WakePrearmActionType = -1;
    s_p1WakePrearmStrength = -1; s_p1WakePrearmIsSpecial = false; s_p1WakePrearmIsMacro = false;
    s_p1WakeActionableWarning = false; s_p1WakeMoveID96FrameCount = 0;
    s_p1WakeBufferFrozen = false; s_p1WakeHoldPrimed = false; s_p1WakeHoldIssued = false;
    
    s_p2WakePrearmed = false; s_p2WakePrearmExpiry = 0; s_p2WakePrearmActionType = -1;
    s_p2WakePrearmStrength = -1; s_p2WakePrearmIsSpecial = false; s_p2WakePrearmIsMacro = false;
    s_p2WakeActionableWarning = false; s_p2WakeMoveID96FrameCount = 0;
    s_p2WakeBufferFrozen = false; s_p2WakeHoldPrimed = false; s_p2WakeHoldIssued = false;

    // =========================================================================
    // 10. WAKE MACRO STATE RESET
    // =========================================================================
    logStream.str(""); logStream.clear();
    logStream << "[CANCEL][WAKE_MACRO] P1: queued=" << (s_p1WakeMacroQueued ? "true" : "false")
              << " slot=" << s_p1WakeMacroSlot
              << " P2: queued=" << (s_p2WakeMacroQueued ? "true" : "false")
              << " slot=" << s_p2WakeMacroSlot;
    LogOut(logStream.str(), true);
    
    s_p1WakeMacro96FrameCount = 0; s_p1WakeMacroQueued = false;
    s_p1WakeMacroTargetFrame = -1; s_p1WakeMacroSlot = -1; s_p1WakeMacroStartTick = 0;
    s_p2WakeMacro96FrameCount = 0; s_p2WakeMacroQueued = false;
    s_p2WakeMacroTargetFrame = -1; s_p2WakeMacroSlot = -1; s_p2WakeMacroStartFrame = -1; s_p2WakeMacroStartTick = 0;
    s_p1WakeOptionPicked = false;
    s_p1WakePrePickedOption = {};
    s_p2WakeOptionPicked = false;
    s_p2WakePrePickedOption = {};

    // =========================================================================
    // 11. WAKE MACRO COMPLETION TRACKING RESET
    // =========================================================================
    bool wakePreserve = g_macroWakePreserveBuffer.load();
    bool wakeCompleted = g_wakeMacroPlaybackCompleted.load();
    
    logStream.str(""); logStream.clear();
    logStream << "[CANCEL][WAKE_COMPLETE] preserveBuffer=" << (wakePreserve ? "true" : "false")
              << " playbackCompleted=" << (wakeCompleted ? "true" : "false")
              << " tokenCountdown=" << s_p2WakeMacroTokenNeutralizeCountdown;
    LogOut(logStream.str(), true);
    
    g_macroWakePreserveBuffer.store(false);
    g_wakeMacroPlaybackCompleted.store(false);
    s_p2WakeMacroTokenNeutralizeCountdown = 0;

    // =========================================================================
    // 12. INPUT INJECTION STATE RESET (POLL OVERRIDE, FORCE BYPASS, MANUAL OVERRIDE)
    // =========================================================================
    extern std::atomic<bool> g_forceBypass[3];
    extern std::atomic<bool> g_pollOverrideActive[3];
    extern std::atomic<uint8_t> g_pollOverrideMask[3];
    extern std::atomic<uint8_t> g_manualInputMask[3];
    extern std::atomic<bool> g_injectImmediateOnly[3];
    
    logStream.str(""); logStream.clear();
    logStream << "[CANCEL][INJECT] P1: pollOverride=" << (g_pollOverrideActive[1].load() ? "true" : "false")
              << " mask=0x" << std::hex << (int)g_pollOverrideMask[1].load() << std::dec
              << " manualOverride=" << (g_manualInputOverride[1].load() ? "true" : "false")
              << " forceBypass=" << (g_forceBypass[1].load() ? "true" : "false");
    LogOut(logStream.str(), true);
    
    logStream.str(""); logStream.clear();
    logStream << "[CANCEL][INJECT] P2: pollOverride=" << (g_pollOverrideActive[2].load() ? "true" : "false")
              << " mask=0x" << std::hex << (int)g_pollOverrideMask[2].load() << std::dec
              << " manualOverride=" << (g_manualInputOverride[2].load() ? "true" : "false")
              << " forceBypass=" << (g_forceBypass[2].load() ? "true" : "false");
    LogOut(logStream.str(), true);
    
    // Clear all input injection state for both players
    for (int p = 1; p <= 2; ++p) {
        g_forceBypass[p].store(false);
        g_pollOverrideActive[p].store(false);
        g_pollOverrideMask[p].store(0);
        g_manualInputOverride[p].store(false);
        g_manualInputMask[p].store(0);
        g_injectImmediateOnly[p].store(false);
    }

    // Queued motion injection is a separate owner from the atomics above. If it
    // survives cancellation, input_hook can still overwrite immediate inputs
    // (and can contaminate an exclusive mission demonstration).
    p1QueueActive = false;
    p2QueueActive = false;
    p1QueueIndex = p2QueueIndex = 0;
    p1FrameCounter = p2FrameCounter = 0;
    p1CurrentMotionType = p2CurrentMotionType = MOTION_NONE;
    
    // Clear immediate input registers
    ImmediateInput::Clear(1);
    ImmediateInput::Clear(2);
    LogOut("[CANCEL][INJECT] Cleared injection state, motion queues, and immediate registers", true);

    LogOut("[CANCEL] === END CancelAutoActionsAndMacros - Triggers remain enabled ===", true);
}

// Hard reset of all auto-action trigger related runtime state
void ClearAllAutoActionTriggers() {
    // Throttle repeated full-clear logs (can fire rapidly during Character Select stability scans)
    static int s_lastClearFrame = -1000000;
    int nowF = frameCounter.load();
    extern std::atomic<bool> g_suppressAutoActionClearLogging; // declared in frame_monitor.cpp
    bool logThis = !g_suppressAutoActionClearLogging.load() && (nowF - s_lastClearFrame) >= 480; // ~2.5s at 192fps
    if (logThis) {
        LogOut("[AUTO-ACTION] Forcing full clear of trigger/delay/cooldown state", true);
        s_lastClearFrame = nowF;
    }

    // Reset delay states (including chosen row fields)
    p1DelayState = {false, 0, TRIGGER_NONE, 0, -1, -1, 0, -1, -1};
    p2DelayState = {false, 0, TRIGGER_NONE, 0, -1, -1, 0, -1, -1};

    // Reset action applied markers
    p1ActionApplied = false;
    p2ActionApplied = false;

    // Reset last active trigger overlay feedback
    g_lastActiveTriggerType.store(TRIGGER_NONE);
    g_lastActiveTriggerFrame.store(0);

    // Reset internal cooldown / active guards (file-scope statics in this TU)
    p1TriggerActive = false; p1TriggerCooldown = 0;
    p2TriggerActive = false; p2TriggerCooldown = 0;

    // Cancel any pending restore logic & control overrides
    if (g_p2ControlOverridden) {
        RestoreP2ControlState();
    }
    g_pendingControlRestore.store(false);
    g_pendingRestoreTimestamp.store(0);
    g_lastP2MoveID.store(-1);
    g_crgFastRestore.store(false);

    // Ensure input buffer freeze (for special motions) is lifted
    StopBufferFreezing();

    // Clear RG pending windows
    s_p1RGPending = false; s_p1RGExpiry = 0;
    s_p2RGPending = false; s_p2RGExpiry = 0;

    // Clear RG pre-arm state
    s_p1RGPrearmed = false; s_p1RGPrearmIsSpecial = false; s_p1RGPrearmActionType = -1; s_p1RGPrearmExpiry = 0;
    s_p2RGPrearmed = false; s_p2RGPrearmIsSpecial = false; s_p2RGPrearmActionType = -1; s_p2RGPrearmExpiry = 0;

    if (logThis) {
        LogOut("[AUTO-ACTION] All trigger states cleared", true);
    }
    // Invalidate cached character IDs since a full clear typically aligns with character select transitions
    InvalidateCachedCharacterIDs("trigger clear");
}

// -------------------------
// Row selection helpers
// -------------------------
static inline bool HasEnabledRows(int triggerType) {
    auto countEnabled = [](const TriggerOption* arr, int cnt){ int n=0; for (int i=0;i<cnt;i++) if (arr[i].enabled) ++n; return n; };
    bool mainEnabled = false;
    switch (triggerType) {
        case TRIGGER_AFTER_BLOCK:   mainEnabled = triggerAfterBlockEnabled.load(); break;
        case TRIGGER_ON_WAKEUP:     mainEnabled = triggerOnWakeupEnabled.load(); break;
        case TRIGGER_AFTER_HITSTUN: mainEnabled = triggerAfterHitstunEnabled.load(); break;
        case TRIGGER_AFTER_AIRTECH: mainEnabled = triggerAfterAirtechEnabled.load(); break;
        case TRIGGER_ON_RG:         mainEnabled = triggerOnRGEnabled.load(); break;
        default: mainEnabled = false; break;
    }
    switch (triggerType) {
        case TRIGGER_AFTER_BLOCK:   return mainEnabled || countEnabled(g_afterBlockOptions, g_afterBlockOptionCount) > 0;
        case TRIGGER_ON_WAKEUP:     return mainEnabled || countEnabled(g_onWakeupOptions, g_onWakeupOptionCount) > 0;
        case TRIGGER_AFTER_HITSTUN: return mainEnabled || countEnabled(g_afterHitstunOptions, g_afterHitstunOptionCount) > 0;
        case TRIGGER_AFTER_AIRTECH: return mainEnabled || countEnabled(g_afterAirtechOptions, g_afterAirtechOptionCount) > 0;
        case TRIGGER_ON_RG:         return mainEnabled || countEnabled(g_onRGOptions, g_onRGOptionCount) > 0;
        default: return mainEnabled;
    }
}

static inline bool PickRandomRow(int triggerType, TriggerOption &out) {
    const TriggerOption* arr = nullptr; int cnt = 0;
    switch (triggerType) {
        case TRIGGER_AFTER_BLOCK:   arr = g_afterBlockOptions;   cnt = g_afterBlockOptionCount; break;
        case TRIGGER_ON_WAKEUP:     arr = g_onWakeupOptions;     cnt = g_onWakeupOptionCount; break;
        case TRIGGER_AFTER_HITSTUN: arr = g_afterHitstunOptions; cnt = g_afterHitstunOptionCount; break;
        case TRIGGER_AFTER_AIRTECH: arr = g_afterAirtechOptions; cnt = g_afterAirtechOptionCount; break;
        case TRIGGER_ON_RG:         arr = g_onRGOptions;         cnt = g_onRGOptionCount; break;
        default: return false;
    }

    // Build candidate list from enabled rows and include the main trigger row as an implicit candidate
    TriggerOption cands[MAX_TRIGGER_OPTIONS + 1];
    int n = 0;
    for (int i = 0; i < cnt && i < MAX_TRIGGER_OPTIONS; ++i) {
        if (arr[i].enabled) cands[n++] = arr[i];
    }

    // Append main trigger row (always considered a candidate when the trigger is enabled)
    TriggerOption mainOpt{};
    switch (triggerType) {
        case TRIGGER_AFTER_BLOCK:
            mainOpt = { true, triggerAfterBlockAction.load(), triggerAfterBlockStrength.load(), triggerAfterBlockDelay.load(), triggerAfterBlockCustomID.load(), triggerAfterBlockMacroSlot.load() };
            break;
        case TRIGGER_ON_WAKEUP:
            mainOpt = { true, triggerOnWakeupAction.load(), triggerOnWakeupStrength.load(), triggerOnWakeupDelay.load(), triggerOnWakeupCustomID.load(), triggerOnWakeupMacroSlot.load() };
            break;
        case TRIGGER_AFTER_HITSTUN:
            mainOpt = { true, triggerAfterHitstunAction.load(), triggerAfterHitstunStrength.load(), triggerAfterHitstunDelay.load(), triggerAfterHitstunCustomID.load(), triggerAfterHitstunMacroSlot.load() };
            break;
        case TRIGGER_AFTER_AIRTECH:
            mainOpt = { true, triggerAfterAirtechAction.load(), triggerAfterAirtechStrength.load(), triggerAfterAirtechDelay.load(), triggerAfterAirtechCustomID.load(), triggerAfterAirtechMacroSlot.load() };
            break;
        case TRIGGER_ON_RG:
            mainOpt = { true, triggerOnRGAction.load(), triggerOnRGStrength.load(), triggerOnRGDelay.load(), triggerOnRGCustomID.load(), triggerOnRGMacroSlot.load() };
            break;
        default: break;
    }
    cands[n++] = mainOpt;

    if (n <= 0) return false;
    int seed = frameCounter.load() + triggerType * 13;
    int pick = (seed < 0 ? -seed : seed) % n;
    out = cands[pick];
    return true;
}

static bool SupportsImmediateWakeHold(int actionType) {
    if (actionType == ACTION_FINAL_MEMORY) return false;
    if (actionType == ACTION_FORWARD_DASH || actionType == ACTION_BACKDASH || actionType == ACTION_BACK_DASH) {
        return false;
    }
    if (actionType == ACTION_JUMP) return true;
    int motionType = ConvertTriggerActionToMotion(actionType, TRIGGER_ON_WAKEUP);
    if (motionType >= MOTION_236A) return false;
    if ((motionType >= MOTION_5A && motionType <= MOTION_5D) ||
        (motionType >= MOTION_2A && motionType <= MOTION_2D) ||
        (motionType >= MOTION_JA && motionType <= MOTION_JD) ||
        (motionType >= MOTION_6A && motionType <= MOTION_4D)) {
        return true;
    }
    if (actionType == ACTION_BLOCK) return true;
    return false;
}

static bool BuildImmediateWakeHoldMask(int playerNum, int actionType, uint8_t &outMask) {
    const bool facingRight = GetPlayerFacingDirection(playerNum);
    const uint8_t forwardDir = facingRight ? GAME_INPUT_RIGHT : GAME_INPUT_LEFT;
    const uint8_t backDir    = facingRight ? GAME_INPUT_LEFT  : GAME_INPUT_RIGHT;
    int motionType = ConvertTriggerActionToMotion(actionType, TRIGGER_ON_WAKEUP);
    int buttonMask = ResolveButtonMaskForAction(actionType, TRIGGER_ON_WAKEUP);
    if (buttonMask == 0) {
        buttonMask = GAME_INPUT_A;
    }

    if (actionType == ACTION_JUMP) {
        int dir = triggerOnWakeupStrength.load();
        if (playerNum == 1 && s_p1WakePrearmStrength >= 0) dir = s_p1WakePrearmStrength;
        if (playerNum == 2 && s_p2WakePrearmStrength >= 0) dir = s_p2WakePrearmStrength;
        dir = CLAMP(dir, 0, 2);
        uint8_t mask = GAME_INPUT_UP;
        if (dir == 1) {
            mask |= forwardDir;
        } else if (dir == 2) {
            mask |= backDir;
        }
        outMask = mask;
        return true;
    }

    if (actionType == ACTION_BLOCK) {
        outMask = backDir;
        return true;
    }

    if (motionType >= MOTION_5A && motionType <= MOTION_5D) {
        outMask = static_cast<uint8_t>(buttonMask);
        return true;
    }
    if (motionType >= MOTION_2A && motionType <= MOTION_2D) {
        outMask = static_cast<uint8_t>(GAME_INPUT_DOWN | buttonMask);
        return true;
    }
    if (motionType >= MOTION_JA && motionType <= MOTION_JD) {
        outMask = static_cast<uint8_t>(buttonMask);
        return true;
    }
    if (motionType >= MOTION_6A && motionType <= MOTION_6D) {
        outMask = static_cast<uint8_t>(forwardDir | buttonMask);
        return true;
    }
    if (motionType >= MOTION_4A && motionType <= MOTION_4D) {
        outMask = static_cast<uint8_t>(backDir | buttonMask);
        return true;
    }

    return false;
}

static bool IssueWakeImmediateHold(int playerNum, int actionType) {
    uint8_t mask = 0;
    if (!BuildImmediateWakeHoldMask(playerNum, actionType, mask)) {
        LogOut("[AUTO-ACTION] Wake immediate hold skipped (no mask) action=" + std::to_string(actionType), detailedLogging.load());
        return false;
    }
    constexpr int kWakeHoldTicks = 3; // 3 visual frames covers last rising + 2 neutral frames
    
    // Log current moveID before scheduling
    uint16_t currentMoveID = 0;
    uintptr_t playerPtr = GetPlayerPointer(playerNum);
    if (playerPtr) {
        SafeReadMemory(playerPtr + MOVE_ID_OFFSET, &currentMoveID, sizeof(uint16_t));
    }
    
    ImmediateInput::PressFor(playerNum, mask, kWakeHoldTicks);
    TraceAutoActionWakeEvent("wake-immediate-hold-pressfor", playerNum,
                             static_cast<short>(currentMoveID),
                             static_cast<short>(currentMoveID),
                             "action=" + std::to_string(actionType) +
                             " mask=" + std::to_string(mask) +
                             " ticks=" + std::to_string(kWakeHoldTicks));
    LogOut("[AUTO-ACTION] Wake immediate hold issued (p" + std::to_string(playerNum) +
           ", action=" + std::to_string(actionType) +
           ", mask=" + std::to_string(mask) +
           ", ticks=" + std::to_string(kWakeHoldTicks) +
           ", moveID=" + std::to_string(currentMoveID) + ")", detailedLogging.load());
    return true;
}

static int ResolveButtonMaskForAction(int actionType, int triggerType, int strengthOverride) {
    int buttonMask = 0;
    // If an explicit strength override is provided for a special, honor it immediately.
    if (strengthOverride >= 0 && actionType >= ACTION_QCF && actionType <= ACTION_6321463214) {
        int strength = CLAMP(strengthOverride, 0, 3);
        buttonMask = (1 << (4 + strength));
        if (detailedLogging.load()) {
            std::stringstream bm;
            bm << std::hex << buttonMask;
            LogOut("[AUTO-ACTION][BTN-RESOLVE] (override-fastpath) actionType=" + std::to_string(actionType) +
                   " triggerType=" + std::to_string(triggerType) +
                   " strengthOverride=" + std::to_string(strengthOverride) +
                   " -> btnMask=0x" + bm.str(), true);
        }
        return buttonMask;
    }
    if ((actionType >= ACTION_5A && actionType <= ACTION_2D) ||
        (actionType >= ACTION_6A && actionType <= ACTION_4D)) {
        int button = 0;
        if (actionType >= ACTION_5A && actionType <= ACTION_2D) {
            button = (actionType - ACTION_5A) % 4;  // Changed from % 3 to % 4 to handle A/B/C/D
        } else {
            int offset = actionType - ACTION_6A;
            button = offset % 4;  // Changed from % 3 to % 4 to handle A/B/C/D
        }
        buttonMask = (1 << (4 + button));
    } else if (actionType >= ACTION_JA && actionType <= ACTION_JD) {
        int button = (actionType - ACTION_JA) % 4;  // Changed from % 3 to % 4 to handle A/B/C/D
        buttonMask = (1 << (4 + button));
    } else if (actionType >= ACTION_QCF && actionType <= ACTION_6321463214) {
        int strength = (strengthOverride >= 0) ? strengthOverride : GetSpecialMoveStrength(actionType, triggerType);
        strength = CLAMP(strength, 0, 3);
        buttonMask = (1 << (4 + strength));
    }
    if (detailedLogging.load()) {
        std::stringstream bm;
        bm << std::hex << buttonMask;
        LogOut("[AUTO-ACTION][BTN-RESOLVE] actionType=" + std::to_string(actionType) +
               " triggerType=" + std::to_string(triggerType) +
               " strengthOverride=" + std::to_string(strengthOverride) +
               " -> btnMask=0x" + bm.str(), true);
    }
    return buttonMask;
}

static bool ExecuteWakeSpecialNow(int playerNum, int actionType, short currentMoveID, int strengthOverride) {
    if (actionType == ACTION_FINAL_MEMORY) {
        int charId = (playerNum == 1) ? displayData.p1CharID : displayData.p2CharID;
        if (charId < 0) {
            LogOut("[AUTO-ACTION][FM] Wake special aborted: unknown character", true);
            return false;
        }
        if (playerNum == 2) {
            EnableP2ControlForAutoAction();
        }
        bool ok = ExecuteFinalMemory(playerNum, charId);
        if (ok && playerNum == 2) {
            g_lastP2MoveID.store(currentMoveID);
            g_pendingControlRestore.store(true);
            g_pendingRestoreTimestamp.store(GetTickCount());
        }
        return ok;
    }

    int motionType = ConvertTriggerActionToMotion(actionType, TRIGGER_ON_WAKEUP);
    if (motionType < MOTION_236A) {
        LogOut("[AUTO-ACTION] Wake special requested for non-special action", true);
        return false;
    }

    int buttonMask = ResolveButtonMaskForAction(actionType, TRIGGER_ON_WAKEUP, strengthOverride);
    if (buttonMask == 0 && motionType != MOTION_FORWARD_DASH && motionType != MOTION_BACK_DASH) {
        int fallbackStrength = (strengthOverride >= 0) ? strengthOverride : 0;
        buttonMask = (1 << (4 + CLAMP(fallbackStrength, 0, 3)));
    }

    if (playerNum == 2) {
        EnableP2ControlForAutoAction();
    }
    g_injectImmediateOnly[playerNum].store(false);
    bool facing = GetPlayerFacingDirection(playerNum);
    std::stringstream btnFmt;
    btnFmt << std::hex << buttonMask;
    LogOut("[AUTO-ACTION] Wake special freeze request p" + std::to_string(playerNum) +
           " motion=" + std::to_string(motionType) + " btnMask=0x" + btnFmt.str() +
            " currMove=" + std::to_string(currentMoveID) + " facing=" + (facing?"right":"left") +
            " strengthOverride=" + std::to_string(strengthOverride), true);
    TraceAutoActionWakeEvent("wake-special-freeze-request", playerNum, currentMoveID, currentMoveID,
                             "motion=" + std::to_string(motionType) +
                             " btnMask=" + std::to_string(buttonMask) +
                             " facing=" + std::string(facing ? "right" : "left") +
                             " strengthOverride=" + std::to_string(strengthOverride));
    bool ok = FreezeBufferForMotion(playerNum, motionType, buttonMask);
    TraceAutoActionWakeEvent("wake-special-freeze-result", playerNum, currentMoveID, currentMoveID,
                             "motion=" + std::to_string(motionType) +
                             " btnMask=" + std::to_string(buttonMask) +
                             " ok=" + std::to_string(ok));
    if (ok && playerNum == 2) {
        g_lastP2MoveID.store(currentMoveID);
        g_pendingControlRestore.store(true);
        g_pendingRestoreTimestamp.store(GetTickCount());
    }
    if (!ok) {
        LogOut("[AUTO-ACTION] Wake special freeze failed for player " + std::to_string(playerNum), true);
    }
    return ok;
}
