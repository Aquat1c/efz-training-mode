#include "../include/game/auto_action.h"
#include "../include/core/constants.h"
#include "../include/utils/utilities.h"
#include "../include/game/game_state.h"
#include "../include/core/memory.h"
#include "../include/core/logger.h"
#include "../include/input/input_core.h"        
#include "../include/input/motion_system.h"     
#include "../include/game/auto_action_helpers.h"
#include "../include/input/motion_constants.h"  
#include "../include/input/input_motion.h"      // Add this include
#include "../include/input/input_freeze.h"     // Add this include near the top with the other includes
#include "../include/game/attack_reader.h"
#include "../include/input/immediate_input.h"
#include "../include/game/fm_commands.h" // Final Memory execution
#include "../include/game/macro_controller.h" // Integrate macros with triggers
#include "../include/game/character_settings.h" // For authoritative character ID mapping
#include "../include/game/frame_analysis.h" // For IsThrown/IsHitstun/IsLaunched helpers

// Safety forward declarations (in case of include-order differences in some build phases)
bool IsThrown(short moveID);
#include <cmath>
#include <algorithm>
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

// Initialize delay states
TriggerDelayState p1DelayState = {false, 0, TRIGGER_NONE, 0};
TriggerDelayState p2DelayState = {false, 0, TRIGGER_NONE, 0};
bool p1ActionApplied = false;
bool p2ActionApplied = false;

// Definitions for trigger tracking globals
std::atomic<int> g_lastActiveTriggerType(TRIGGER_NONE);
std::atomic<int> g_lastActiveTriggerFrame(0);

static bool p1TriggerActive = false;
static bool p2TriggerActive = false;
// Log throttles to avoid spamming the console during tight loops
// (Removed per-trigger throttles for P2 After Block in refactor)
static int p1TriggerCooldown = 0;
static int p2TriggerCooldown = 0;
// Further reduce cooldown to re-arm triggers faster (was 6)
static constexpr int TRIGGER_COOLDOWN_FRAMES = 6; // modest base cooldown; simplified logic allows reliable expiry
static constexpr int RESTORE_GRACE_PERIOD = 12; // frames to wait before clearing g_pendingControlRestore to prevent immediate re-trigger
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

static void InvalidateCachedCharacterIDs(const char* reason) {
    if ((g_cachedP1CharID != -999 || g_cachedP2CharID != -999) && !g_loggedCharInvalidation) {
        LogOut(std::string("[AUTO-ACTION][CHAR] Invalidating cached character IDs (") + reason + ")", detailedLogging.load());
        g_loggedCharInvalidation = true;
    }
    g_cachedP1CharID = -999;
    g_cachedP2CharID = -999;
}

static void EnsureCachedCharacterIDs(uintptr_t base) {
    if (g_cachedP1CharID == -999) {
        char nameBuf1[16] = {0};
        uintptr_t nameAddr1 = ResolvePointer(base, EFZ_BASE_OFFSET_P1, CHARACTER_NAME_OFFSET);
        if (nameAddr1) SafeReadMemory(nameAddr1, &nameBuf1, sizeof(nameBuf1)-1);
        g_cachedP1CharID = CharacterSettings::GetCharacterID(std::string(nameBuf1));
        if (g_cachedP1CharID < 0) g_cachedP1CharID = -1;
        LogOut(std::string("[AUTO-ACTION][CHAR] Cached P1 char name='") + nameBuf1 + "' id=" + std::to_string(g_cachedP1CharID), detailedLogging.load());
        g_loggedCharInvalidation = false;
    }
    if (g_cachedP2CharID == -999) {
        char nameBuf2[16] = {0};
        uintptr_t nameAddr2 = ResolvePointer(base, EFZ_BASE_OFFSET_P2, CHARACTER_NAME_OFFSET);
        if (nameAddr2) SafeReadMemory(nameAddr2, &nameBuf2, sizeof(nameBuf2)-1);
        g_cachedP2CharID = CharacterSettings::GetCharacterID(std::string(nameBuf2));
        if (g_cachedP2CharID < 0) g_cachedP2CharID = -1;
        LogOut(std::string("[AUTO-ACTION][CHAR] Cached P2 char name='") + nameBuf2 + "' id=" + std::to_string(g_cachedP2CharID), detailedLogging.load());
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
    LogOut(std::string("[AUTO-ACTION] Forward dash follow-up armed (sel=") + std::to_string(sel) + ")", true);
    if (playerNum == 2) {
        EnableP2ControlForAutoAction();
        if (!g_pendingControlRestore.load()) {
            g_lastP2MoveID.store(-1);
            g_controlRestoreTimeout.store(120); // generous window; we'll restore shortly after injection
            g_pendingControlRestore.store(true);
            LogOut("[AUTO-ACTION][DASH] Armed control restore monitor (simple dash-normal)", true);
        }
    }
}

// Refactored: use authoritative moveID transitions (163->164) instead of queue heuristics.
// current/prev move IDs for both players are passed each frame for precise detection.
static void ProcessDeferredDashFollowups(short curP1, short prevP1, short curP2, short prevP2) {
    int sel = g_dashDeferred.pendingSel.load();
    if (sel <= 0) return;
    int targetP = g_dashDeferred.player.load();
    short prev = (targetP == 1) ? prevP1 : prevP2;
    short curr = (targetP == 1) ? curP1  : curP2;
    
    // Debug: Log MoveID transitions while waiting for dash
    static short lastLoggedP1 = -999;
    static short lastLoggedP2 = -999;
    if (targetP == 1 && curr != lastLoggedP1) {
        LogOut("[DASH_DEBUG] P1 MoveID: " + std::to_string(prev) + " -> " + std::to_string(curr) + 
               " (waiting for " + std::to_string(FORWARD_DASH_START_ID) + ")", true);
        lastLoggedP1 = curr;
        if (curr == FORWARD_DASH_START_ID) {
            DumpInputBuffer(1, "DASH_START_DETECTED");
        }
    }
    if (targetP == 2 && curr != lastLoggedP2) {
        LogOut("[DASH_DEBUG] P2 MoveID: " + std::to_string(prev) + " -> " + std::to_string(curr) + 
               " (waiting for " + std::to_string(FORWARD_DASH_START_ID) + ")", true);
        lastLoggedP2 = curr;
        if (curr == FORWARD_DASH_START_ID) {
            DumpInputBuffer(2, "DASH_START_DETECTED");
        }
    }
    
    // Fire exactly on first frame we see FORWARD_DASH_START_ID.
    if (curr == FORWARD_DASH_START_ID && prev != FORWARD_DASH_START_ID) {
        bool facingRight = GetPlayerFacingDirection(targetP);
        uint8_t forwardDir = facingRight ? GAME_INPUT_RIGHT : GAME_INPUT_LEFT;
        uint8_t mask = 0;
        switch (sel) {
            case 1: mask = forwardDir | GAME_INPUT_A; break; // dash A
            case 2: mask = forwardDir | GAME_INPUT_B; break; // dash B
            case 3: mask = forwardDir | GAME_INPUT_C; break; // dash C
            case 4: mask = (forwardDir | GAME_INPUT_DOWN) | GAME_INPUT_A; break; // dash 2A variant
            case 5: mask = (forwardDir | GAME_INPUT_DOWN) | GAME_INPUT_B; break; // dash 2B
            case 6: mask = (forwardDir | GAME_INPUT_DOWN) | GAME_INPUT_C; break; // dash 2C
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
std::atomic<int> g_controlRestoreTimeout(0);
std::atomic<short> g_lastP2MoveID(-1);
const int CONTROL_RESTORE_TIMEOUT = 180; // 180 internal frames = 1 second
// Counter-RG fast restore: when active, restore AI control as soon as the special actually starts
std::atomic<bool> g_crgFastRestore(false);
// Forward dash tracking to prevent premature restore before MoveID 163 appears
static std::atomic<bool> g_recentDashQueued(false);
static std::atomic<int> g_recentDashQueuedFrame(0);

// Pre-arm flags for On Wakeup: buffer inputs during GROUNDTECH_RECOVERY to fire on wake
static bool s_p1WakePrearmed = false; // wake pre-arm without early triggerActive
static bool s_p2WakePrearmed = false; // same
static int  s_p1WakePrearmExpiry = 0;
static int  s_p2WakePrearmExpiry = 0;
// Wake pre-arm action metadata (so we can execute normals/dashes at the actionable frame)
static int  s_p1WakePrearmActionType = -1;
static int  s_p2WakePrearmActionType = -1;
static bool s_p1WakePrearmIsSpecial = false; // specials rely on buffer freeze; normals/dashes injected later
static bool s_p2WakePrearmIsSpecial = false;

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

// (Deprecated) Pending window for On RG: retained for state reset only
static bool s_p1RGPending = false;
static int  s_p1RGExpiry = 0;
static bool s_p2RGPending = false;
static int  s_p2RGExpiry = 0;

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
    if (!autoActionEnabled.load()) return false;
    // If macro playback is active, suppress P2 auto-action work to avoid conflicts.
    if (MacroController::GetState() == MacroController::State::Replaying) {
        return false;
    }
    // If any triggers are enabled, we may need to evaluate
    bool triggersEnabled = triggerAfterBlockEnabled.load() || triggerOnWakeupEnabled.load() ||
                           triggerAfterHitstunEnabled.load() || triggerAfterAirtechEnabled.load() ||
                           triggerOnRGEnabled.load();
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
            default:
                // If in air and trying to do a ground move, use a safe fallback
                return STRAIGHT_JUMP_ID;
        }
    }
    else { // Character is on the ground
        // Air move attempted on ground - check if it's a jump-related action
        if (actionType == ACTION_JA || actionType == ACTION_JB || actionType == ACTION_JC) {
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
            case ACTION_2A:
                return BASE_ATTACK_2A;  // 204
            case ACTION_2B:
                return BASE_ATTACK_2B;  // 205
            case ACTION_2C:
                return BASE_ATTACK_2C;  // 206
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

void ProcessTriggerDelays() {
    uintptr_t base = GetEFZBase();
    if (!base) return;

    // P1 delay processing
    if (p1DelayState.isDelaying) {
     LogOut("[DELAY] P1 delaying: framesRemaining=" + std::to_string(p1DelayState.delayFramesRemaining) +
         ", triggerType=" + std::to_string(p1DelayState.triggerType), detailedLogging.load());
     p1DelayState.delayFramesRemaining--;
        
        if (p1DelayState.delayFramesRemaining % 64 == 0 && p1DelayState.delayFramesRemaining > 0) {
            LogOut("[AUTO-ACTION] P1 delay countdown: " + std::to_string(p1DelayState.delayFramesRemaining/3) + 
                   " visual frames remaining", detailedLogging.load());
        }
        
    if (p1DelayState.delayFramesRemaining <= 0) {
            LogOut("[AUTO-ACTION] P1 delay expired, applying action", true);
            
            // Get moveIDAddr just to pass to ApplyAutoAction - it won't be used to write to anymore
            uintptr_t moveIDAddr = ResolvePointer(base, EFZ_BASE_OFFSET_P1, MOVE_ID_OFFSET);
            short currentMoveID = 0;
            
            if (moveIDAddr) {
                SafeReadMemory(moveIDAddr, &currentMoveID, sizeof(short)); // Read just for logging
                
                // Apply action via input system
                ApplyAutoAction(1, moveIDAddr, 0, 0); // moveID parameters are ignored now
                
                LogOut("[AUTO-ACTION] P1 action applied via input system", true);
                
                p1DelayState.isDelaying = false;
                p1DelayState.triggerType = TRIGGER_NONE;
                p1DelayState.pendingMoveID = 0;
            } else {
                LogOut("[AUTO-ACTION] Failed to apply P1 action - invalid moveID address", true);
                p1DelayState.isDelaying = false;
            }
        }
    }
    
    // P2 delay processing
    if (p2DelayState.isDelaying) {
     LogOut("[DELAY] P2 delaying: framesRemaining=" + std::to_string(p2DelayState.delayFramesRemaining) +
         ", triggerType=" + std::to_string(p2DelayState.triggerType), detailedLogging.load());
     p2DelayState.delayFramesRemaining--;
        
        if (p2DelayState.delayFramesRemaining % 64 == 0 && p2DelayState.delayFramesRemaining > 0) {
            LogOut("[AUTO-ACTION] P2 delay countdown: " + std::to_string(p2DelayState.delayFramesRemaining/3) + 
                   " visual frames remaining", detailedLogging.load());
        }
        
    if (p2DelayState.delayFramesRemaining <= 0) {
            LogOut("[AUTO-ACTION] P2 delay expired, applying action", true);

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
            if (sel > 0 && MacroController::GetState() != MacroController::State::Recording) {
                sel = CLAMP(sel, 1, MacroController::GetSlotCount());
                if (!MacroController::IsSlotEmpty(sel)) {
                    MacroController::SetCurrentSlot(sel);
                    if (MacroController::GetState() != MacroController::State::Replaying) {
                        LogOut("[AUTO-ACTION][MACRO] Starting macro playback (slot=" + std::to_string(sel) + ") for P2 on trigger expiry", true);
                        MacroController::Play();
                        // Clear the delay state and exit
                        p2DelayState.isDelaying = false;
                        p2DelayState.triggerType = TRIGGER_NONE;
                        p2DelayState.pendingMoveID = 0;
                        return;
                    }
                }
            }

            // Fallback: apply the configured single action via input system
            uintptr_t moveIDAddr = ResolvePointer(base, EFZ_BASE_OFFSET_P2, MOVE_ID_OFFSET);
            short currentMoveID = 0;

            if (moveIDAddr) {
                SafeReadMemory(moveIDAddr, &currentMoveID, sizeof(short));

                // Apply action via input system
                // If macro is currently playing, do not inject conflicting actions for P2.
                if (MacroController::GetState() != MacroController::State::Replaying) {
                    ApplyAutoAction(2, moveIDAddr, 0, 0);
                } else {
                    LogOut("[AUTO-ACTION][MACRO] P2 macro active; skipping ApplyAutoAction", detailedLogging.load());
                }

                LogOut("[AUTO-ACTION] P2 action applied via input system", true);

                p2DelayState.isDelaying = false;
                p2DelayState.triggerType = TRIGGER_NONE;
                p2DelayState.pendingMoveID = 0;
                p2ActionApplied = true;
                // Do NOT restore P2 control immediately here; specials/supers use
                // buffer freezing and set g_pendingControlRestore to return control
                // after execution or timeout. Immediate restore would cancel the buffer.
                if (detailedLogging.load()) {
                    LogOut(std::string("[AUTO-ACTION] P2 delayed action applied; pendingRestore=") +
                           (g_pendingControlRestore.load()?"true":"false"), true);
                }
            } else {
                LogOut("[AUTO-ACTION] Failed to apply P2 action - invalid moveID address", true);
                p2DelayState.isDelaying = false;
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

    LogOut("[AUTO-ACTION] StartTriggerDelay called: Player=" + std::to_string(playerNum) + 
           ", triggerType=" + std::to_string(triggerType) + 
           ", delay=" + std::to_string(delayFrames) +
           ", p1ActApplied=" + std::to_string(p1ActionApplied) +
           ", p2ActApplied=" + std::to_string(p2ActionApplied) +
           ", p1TrigActive=" + std::to_string(p1TriggerActive) +
           ", p2TrigActive=" + std::to_string(p2TriggerActive) +
           ", p1Cooldown=" + std::to_string(p1TriggerCooldown) +
           ", p2Cooldown=" + std::to_string(p2TriggerCooldown), detailedLogging.load());
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
        int internalFrames = delayFrames * 3;
        
        if (playerNum == 1) {
            p1DelayState.isDelaying = true;
            p1DelayState.delayFramesRemaining = internalFrames;
            // triggerType already set above
            p1DelayState.pendingMoveID = 0;
         LogOut("[DELAY] Armed P1 delay: internalFrames=" + std::to_string(internalFrames) +
             ", triggerType=" + std::to_string(triggerType), detailedLogging.load());
        } else {
            p2DelayState.isDelaying = true;
            p2DelayState.delayFramesRemaining = internalFrames;
            // triggerType already set above
            p2DelayState.pendingMoveID = 0;
            LogOut("[AUTO-ACTION] P2 delay armed: internalFrames=" + std::to_string(internalFrames), detailedLogging.load());
        }
    }
}

void ProcessTriggerCooldowns() {
    // Avoid building strings unless diagnostic logging is enabled, and throttle to ~5s
    if (detailedLogging.load()) {
        static int s_nextCooldownDiagFrame = 0; // 5s throttle at 192 fps => 960 frames
        int now = frameCounter.load();
        if (now >= s_nextCooldownDiagFrame) {
            LogOut("[COOLDOWN] p1Active=" + std::to_string(p1TriggerActive) +
                   " p1CD=" + std::to_string(p1TriggerCooldown) +
                   " | p2Active=" + std::to_string(p2TriggerActive) +
                   " p2CD=" + std::to_string(p2TriggerCooldown), true);
            s_nextCooldownDiagFrame = now + 960;
        }
    }
    // Simplified cooldown progression (no pinning) to avoid deadlocks
    if (p1TriggerActive && p1TriggerCooldown > 0) {
        p1TriggerCooldown--;
        if (p1TriggerCooldown <= 0) {
            p1TriggerActive = false;
            LogOut("[AUTO-ACTION] P1 trigger cooldown expired, new triggers allowed", detailedLogging.load());
        }
    }
    if (p2TriggerActive && p2TriggerCooldown > 0) {
        p2TriggerCooldown--;
        if (p2TriggerCooldown <= 0) {
            p2TriggerActive = false;
            LogOut("[AUTO-ACTION] P2 trigger cooldown expired, new triggers allowed", detailedLogging.load());
        }
    }
}

// Core implementation that uses caller-provided move IDs for better cache locality
static void MonitorAutoActionsImpl(short moveID1, short moveID2, short prevMoveID1, short prevMoveID2) {
    if (!autoActionEnabled.load()) {
        return;
    }
    
    // Fast-path out when there's definitively no work to do this frame
    if (!AutoActionWorkPending()) {
        return;
    }
    ProcessTriggerCooldowns();
    // (Deferred dash follow-ups handled after we have moveID context below)

    // Update timing trackers once per tick for both players
    UpdateStunTimersForPlayer(s_p1Timers, prevMoveID1, moveID1);
    UpdateStunTimersForPlayer(s_p2Timers, prevMoveID2, moveID2);

    // Note: Do NOT restore on RG exit; we must keep buffer-freeze active until the special actually begins.
    
    int targetPlayer = autoActionPlayer.load();
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
            LogOut("[TRIGGER_DIAG] P1 eval: prev=" + std::to_string(prevMoveID1) + 
                       ", curr=" + std::to_string(moveID1) +
                       ", trigActive=" + std::to_string(p1TriggerActive) +
                       ", cooldown=" + std::to_string(p1TriggerCooldown) +
                   ", actionable(prev/curr)=" + std::to_string(IsActionable(prevMoveID1)) + "/" + std::to_string(IsActionable(moveID1)), true);
        }
        
        if (!shouldTrigger && triggerAfterAirtechEnabled.load()) {
            // Check if player was in airtech last frame
            bool wasInAirtech = IsAirtech(prevMoveID1);
            // Post-airtech actionable: allow either general actionable states or explicit FALLING
            bool postAirtechNow = (!IsAirtech(moveID1)) && (IsActionable(moveID1) || moveID1 == FALLING_ID);
            if (detailedLogging.load() && canLogTrigDiag()) {
                LogOut("[TRIGGER_DIAG] P1 AfterAirtech check: wasAirtech=" + std::to_string(wasInAirtech) +
                       ", postAirtechNow=" + std::to_string(postAirtechNow) +
                       ", targetPlayer=" + std::to_string(targetPlayer), true);
            }

            // P1 After-Airtech trigger condition
            if (wasInAirtech && postAirtechNow) {
                LogOut("[AUTO-ACTION] P1 After Airtech trigger activated (from moveID " + 
                       std::to_string(prevMoveID1) + " to " + std::to_string(moveID1) + ")", detailedLogging.load());
                shouldTrigger = true;
                triggerType = TRIGGER_AFTER_AIRTECH;
                delay = triggerAfterAirtechDelay.load();

                // Get the appropriate action moveID for After Airtech trigger
                int actionType = triggerAfterAirtechAction.load();
                actionMoveID = GetActionMoveID(actionType, TRIGGER_AFTER_AIRTECH, 1);
            }
        }


        // After Block trigger
    if (!shouldTrigger && triggerAfterBlockEnabled.load()) {
            bool wasInBlockstun = IsBlockstun(prevMoveID1);
            bool nowNotInBlockstun = !IsBlockstun(moveID1);
            bool wasNotActionable = !IsActionable(prevMoveID1);
            bool isNowActionable = IsActionable(moveID1);
            bool justBecameActionable = wasNotActionable && isNowActionable;

            if (wasInBlockstun && nowNotInBlockstun && justBecameActionable) {
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
        }

        // On RG trigger: arm at RG entry and schedule for RG stun end (stand/crouch/air specific)
        if (!shouldTrigger && triggerOnRGEnabled.load()) {
            bool enteredRG = (!IsRecoilGuard(prevMoveID1) && IsRecoilGuard(moveID1));
            if (enteredRG) {
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

                // If user requested 0F delay and action is a special/FM, pre-arm now so motion exists on the first actionable frame
                if (userDelayF == 0 && !s_p1RGPrearmed) {
                    int at = triggerOnRGAction.load();
                    int motion = ConvertTriggerActionToMotion(at, TRIGGER_ON_RG);
                    int buttonMask = 0;
                    if ((at >= ACTION_5A && at <= ACTION_2C) || (at >= ACTION_6A && at <= ACTION_4C)) {
                        int button;
                        if (at >= ACTION_5A && at <= ACTION_2C) {
                            button = (at - ACTION_5A) % 3;
                        } else { // directional normals contiguous groups 6A..6C then 4A..4C
                            int offset = at - ACTION_6A; // 0..5
                            button = offset % 3; // 0=A,1=B,2=C
                        }
                        buttonMask = (1 << (4 + button));
                    } else if (at >= ACTION_JA && at <= ACTION_JC) {
                        int button = (at - ACTION_JA) % 3;
                        buttonMask = (1 << (4 + button));
                    } else if (at >= ACTION_QCF && at <= ACTION_6321463214) {
                        int strength = GetSpecialMoveStrength(at, TRIGGER_ON_RG);
                        buttonMask = (1 << (4 + strength));
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
        }
        
        // After Hitstun trigger
        if (!shouldTrigger && triggerAfterHitstunEnabled.load()) {
            bool wasInHitstun = IsHitstun(prevMoveID1);
            bool nowNotInHitstun = !IsHitstun(moveID1);
            bool wasNotActionable = !IsActionable(prevMoveID1);
            bool isNowActionable = IsActionable(moveID1);
            bool justBecameActionable = wasNotActionable && isNowActionable;

            if (wasInHitstun && nowNotInHitstun && justBecameActionable) {
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
        }
        
        // On Wakeup pre-arm: buffer the motion on last groundtech step so it executes at wake
    if (triggerOnWakeupEnabled.load() && !s_p1WakePrearmed) {
            if (moveID1 == GROUNDTECH_RECOVERY) {
                int actionType = triggerOnWakeupAction.load();
                int motionType = ConvertTriggerActionToMotion(actionType, TRIGGER_ON_WAKEUP);
                // Determine button mask (A/B/C) for specials
                int buttonMask = 0;
                if ((actionType >= ACTION_5A && actionType <= ACTION_2C) || (actionType >= ACTION_6A && actionType <= ACTION_4C)) {
                    int button = (actionType - ACTION_5A) % 3;  // 0=A, 1=B, 2=C
                    buttonMask = (1 << (4 + button));
                } else if (actionType >= ACTION_JA && actionType <= ACTION_JC) {
                    int button = (actionType - ACTION_JA) % 3;
                    buttonMask = (1 << (4 + button));
                } else if (actionType >= ACTION_QCF && actionType <= ACTION_6321463214) {
                    int strength = GetSpecialMoveStrength(actionType, TRIGGER_ON_WAKEUP);
                    buttonMask = (1 << (4 + strength));
                }

                bool prearmed = false;
                // Conditional wake buffering: if disabled, NEVER freeze early; inject on first actionable frame
                if (actionType == ACTION_FINAL_MEMORY) {
                    // Treat FM like a special: allow early buffer freeze when wake buffering enabled.
                    int charId = displayData.p1CharID;
                    if (g_wakeBufferingEnabled.load()) {
                        // Early freeze pattern now so it is resident on first actionable frame.
                        bool fmOk = ExecuteFinalMemory(1, charId);
                        if (fmOk) {
                            prearmed = true;
                            s_p1WakePrearmIsSpecial = true; // skip duplicate inject at wake
                            LogOut("[AUTO-ACTION][FM] P1 wake FM buffered early (toggle=ON)", true);
                        } else if (detailedLogging.load()) {
                            LogOut("[AUTO-ACTION][FM] P1 wake FM early buffer gate failed", true);
                        }
                    } else {
                        // Frame1 injection path: mark prearmed metadata only; inject FM at wake exec.
                        prearmed = true;
                        s_p1WakePrearmIsSpecial = false; // cause ApplyAutoAction path at wake
                        LogOut("[AUTO-ACTION][FM] P1 wake FM scheduled for frame1 inject (toggle=OFF)", true);
                    }
                } else if (g_wakeBufferingEnabled.load()) {
                    if (motionType >= MOTION_236A) {
                        prearmed = FreezeBufferForMotion(1, motionType, buttonMask);
                        s_p1WakePrearmIsSpecial = true;
                        if (prearmed) {
                            LogOut("[AUTO-ACTION] P1 wake special buffered early (toggle=ON)", true);
                        }
                    } else if (actionType == ACTION_BACKDASH || actionType == ACTION_FORWARD_DASH) {
                        // Allow early queue for dash only when buffering enabled
                        prearmed = QueueMotionInput(1, motionType, 0);
                        s_p1WakePrearmIsSpecial = false; // dash executes on wake via queue
                        if (prearmed) {
                            LogOut("[AUTO-ACTION] P1 wake dash queued early (toggle=ON)", true);
                        }
                    } else if (motionType >= MOTION_5A && motionType <= MOTION_JC) {
                        // Normals never need early queue; mark as prearmed metadata only
                        prearmed = true;
                        s_p1WakePrearmIsSpecial = false;
                    }
                } else {
                    // Frame1 injection path: just mark as prearmed metadata for all supported actions
                    if (motionType >= MOTION_5A && motionType <= MOTION_JC) {
                        prearmed = true; s_p1WakePrearmIsSpecial = false;
                    } else if (actionType == ACTION_BACKDASH || actionType == ACTION_FORWARD_DASH) {
                        prearmed = true; s_p1WakePrearmIsSpecial = false; // inject dash on wake
                    } else if (motionType >= MOTION_236A) {
                        prearmed = true; s_p1WakePrearmIsSpecial = false; // treat special like normal for frame1 inject
                        LogOut("[AUTO-ACTION] P1 wake special set for frame1 inject (toggle=OFF)", true);
                    }
                }
                if (prearmed) {
                    s_p1WakePrearmed = true;
                    s_p1WakePrearmExpiry = frameCounter.load() + 120;
                    s_p1WakePrearmActionType = actionType;
                    LogOut("[AUTO-ACTION] P1 wake prearm stored: action=" + std::to_string(actionType) +
                           " motion=" + std::to_string(motionType) +
                           " special=" + std::to_string(s_p1WakePrearmIsSpecial) +
                           " currMoveID=" + std::to_string(moveID1) +
                           " frame=" + std::to_string(frameCounter.load()), true);
                } else if (detailedLogging.load()) {
                    LogOut("[AUTO-ACTION] P1 wake pre-arm failed", true);
                }
            }
        }

        // On Wakeup trigger (fallback if not pre-armed or for normals/jumps)
        if (!shouldTrigger && triggerOnWakeupEnabled.load()) {
            if (s_p1WakePrearmed && IsGroundtech(prevMoveID1) && IsActionable(moveID1)) {
                LogOut("[AUTO-ACTION] P1 wake exec: action=" + std::to_string(s_p1WakePrearmActionType) +
                       " special=" + std::to_string(s_p1WakePrearmIsSpecial) +
                       " prevMoveID=" + std::to_string(prevMoveID1) +
                       " currMoveID=" + std::to_string(moveID1) +
                       " frame=" + std::to_string(frameCounter.load()), true);
                g_lastActiveTriggerType.store(TRIGGER_ON_WAKEUP);
                g_lastActiveTriggerFrame.store(frameCounter.load());
                // If it was NOT an early-buffered special, inject now (special already handled by freeze thread)
                if (!s_p1WakePrearmIsSpecial && s_p1WakePrearmActionType >= 0) {
                    int at = s_p1WakePrearmActionType;
                    if (at == ACTION_FINAL_MEMORY) {
                        int charId = displayData.p1CharID;
                        bool ok = ExecuteFinalMemory(1, charId);
                        LogOut(std::string("[AUTO-ACTION][FM] P1 wake frame1 FM inject ") + (ok?"ok":"fail"), true);
                        // Clear trigger immediately regardless outcome
                        p1TriggerActive = false; p1TriggerCooldown = 0;
                        s_p1WakePrearmed = false; s_p1WakePrearmActionType = -1;
                    } else {
                        int motionType = ConvertTriggerActionToMotion(at, TRIGGER_ON_WAKEUP);
                        int buttonMask = 0;
                        if ((at >= ACTION_5A && at <= ACTION_2C) || (at >= ACTION_6A && at <= ACTION_4C)) {
                            int button;
                            if (at >= ACTION_5A && at <= ACTION_2C) {
                                button = (at - ACTION_5A) % 3;
                            } else {
                                int offset = at - ACTION_6A;
                                button = offset % 3;
                            }
                            buttonMask = (1 << (4 + button));
                        } else if (at >= ACTION_JA && at <= ACTION_JC) {
                            int button = (at - ACTION_JA) % 3;
                            buttonMask = (1 << (4 + button));
                        }
                        if (at == ACTION_BACKDASH || at == ACTION_FORWARD_DASH) {
                            QueueMotionInput(1, motionType, 0);
                        } else if ((motionType >= MOTION_5A && motionType <= MOTION_5C) || (motionType >= MOTION_6A && motionType <= MOTION_4C)) {
                            ImmediateInput::PressFor(1, buttonMask, 2);
                        } else if (motionType >= MOTION_2A && motionType <= MOTION_2C) {
                            ImmediateInput::PressFor(1, GAME_INPUT_DOWN | buttonMask, 2);
                        } else if (motionType >= MOTION_236A) {
                            if (buttonMask == 0) {
                                int atStrength = GetSpecialMoveStrength(at, TRIGGER_ON_WAKEUP);
                                buttonMask = (1 << (4 + atStrength));
                            }
                            FreezeBufferForMotion(1, motionType, buttonMask);
                            { std::stringstream bm; bm << std::hex << buttonMask; LogOut(std::string("[AUTO-ACTION] P1 wake special frame1 buffer applied (index frozen) btnMask=0x") + bm.str(), true); }
                        }
                    }
                }
                s_p1WakePrearmed = false;
                s_p1WakePrearmActionType = -1;
            }
            if (!s_p1WakePrearmed && IsGroundtech(prevMoveID1) && IsActionable(moveID1)) {
                shouldTrigger = true;
                triggerType = TRIGGER_ON_WAKEUP;
                delay = triggerOnWakeupDelay.load();
                actionMoveID = GetActionMoveID(triggerOnWakeupAction.load(), TRIGGER_ON_WAKEUP, 1);
                
                LogOut("[AUTO-ACTION] P1 On Wakeup trigger activated", true);
                if (detailedLogging.load()) {
                    LogOut("[TRIGGER_TIMING] P1 wake delay=" + std::to_string(s_p1Timers.lastWakeDelay) +
                           ", sinceWake=" + std::to_string(s_p1Timers.sinceWake), true);
                }
            }
        }
        
        // Apply the trigger if any condition was met
        if (shouldTrigger) {
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
                LogOut("[AUTO-ACTION] P2 After Airtech trigger activated", detailedLogging.load());
                shouldTrigger = true;
                triggerType = TRIGGER_AFTER_AIRTECH;
        // Pass visual frames; StartTriggerDelay converts to internal frames
        delay = triggerAfterAirtechDelay.load();
                actionMoveID = GetActionMoveID(triggerAfterAirtechAction.load(), TRIGGER_AFTER_AIRTECH, 2);
            }
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
            shouldTrigger = true;
            triggerType = TRIGGER_AFTER_BLOCK;
            delay = triggerAfterBlockDelay.load();
            actionMoveID = GetActionMoveID(triggerAfterBlockAction.load(), TRIGGER_AFTER_BLOCK, 2);
            LogOut("[AUTO-ACTION] P2 After Block trigger", true);
        }
    }

    // P2: On RG trigger at RG entry (independent of After Block): schedule for RG stun end
    if (!shouldTrigger && triggerOnRGEnabled.load()) {
        bool enteredRG2 = (!IsRecoilGuard(prevMoveID2) && IsRecoilGuard(moveID2));
        if (enteredRG2) {
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

            // If user delay is 0 and action is special/FM, pre-arm now; ensure P2 control override for buffer progression
            if (userDelayF == 0 && !s_p2RGPrearmed) {
                int at = triggerOnRGAction.load();
                int motion = ConvertTriggerActionToMotion(at, TRIGGER_ON_RG);
                int buttonMask = 0;
                if ((at >= ACTION_5A && at <= ACTION_2C) || (at >= ACTION_6A && at <= ACTION_4C)) {
                    int button = (at >= ACTION_5A && at <= ACTION_2C) ? ((at - ACTION_5A) % 3) : ((at - ACTION_6A) % 3);
                    buttonMask = (1 << (4 + button));
                } else if (at >= ACTION_JA && at <= ACTION_JC) {
                    int button = (at - ACTION_JA) % 3;
                    buttonMask = (1 << (4 + button));
                } else if (at >= ACTION_QCF && at <= ACTION_6321463214) {
                    int strength = GetSpecialMoveStrength(at, TRIGGER_ON_RG);
                    buttonMask = (1 << (4 + strength));
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
                        g_controlRestoreTimeout.store(240);
                    } else {
                        // Allow buffer writes
                        g_injectImmediateOnly[2].store(false);
                        preOk = FreezeBufferForMotion(2, motion, buttonMask);
                        s_p2RGPrearmIsSpecial = true;
                        g_lastP2MoveID.store(moveID2);
                        g_pendingControlRestore.store(true);
                        g_controlRestoreTimeout.store(180);
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
        }
        
        // On Wakeup pre-arm for P2: buffer during GROUNDTECH_RECOVERY (96)
    if (triggerOnWakeupEnabled.load() && !s_p2WakePrearmed) {
            if (moveID2 == GROUNDTECH_RECOVERY) {
                int actionType = triggerOnWakeupAction.load();
                int motionType = ConvertTriggerActionToMotion(actionType, TRIGGER_ON_WAKEUP);
                int buttonMask = 0;
                if (actionType >= ACTION_5A && actionType <= ACTION_2C) {
                    int button = (actionType - ACTION_5A) % 3;  // 0=A, 1=B, 2=C
                    buttonMask = (1 << (4 + button));
                } else if (actionType >= ACTION_JA && actionType <= ACTION_JC) {
                    int button = (actionType - ACTION_JA) % 3;
                    buttonMask = (1 << (4 + button));
                } else if (actionType >= ACTION_QCF && actionType <= ACTION_6321463214) {
                    int strength = GetSpecialMoveStrength(actionType, TRIGGER_ON_WAKEUP);
                    buttonMask = (1 << (4 + strength));
                }

                bool prearmed = false;
                if (actionType == ACTION_FINAL_MEMORY) {
                    int charId = displayData.p2CharID;
                    if (g_wakeBufferingEnabled.load()) {
                        bool fmOk = ExecuteFinalMemory(2, charId);
                        if (fmOk) {
                            prearmed = true;
                            s_p2WakePrearmIsSpecial = true;
                            EnableP2ControlForAutoAction(); // ensure buffer progression semantics
                            LogOut("[AUTO-ACTION][FM] P2 wake FM buffered early (toggle=ON)", true);
                        } else if (detailedLogging.load()) {
                            LogOut("[AUTO-ACTION][FM] P2 wake FM early buffer gate failed", true);
                        }
                    } else {
                        prearmed = true; // frame1 path
                        s_p2WakePrearmIsSpecial = false; // inject at wake exec via ApplyAutoAction
                        LogOut("[AUTO-ACTION][FM] P2 wake FM scheduled for frame1 inject (toggle=OFF)", true);
                    }
                } else if (g_wakeBufferingEnabled.load()) {
                    if (motionType >= MOTION_236A) {
                        prearmed = FreezeBufferForMotion(2, motionType, buttonMask);
                        if (prearmed) LogOut("[AUTO-ACTION] P2 wake special buffered early (toggle=ON)", true);
                    } else if (actionType == ACTION_BACKDASH || actionType == ACTION_FORWARD_DASH) {
                        prearmed = QueueMotionInput(2, motionType, 0);
                        if (prearmed) LogOut("[AUTO-ACTION] P2 wake dash queued early (toggle=ON)", true);
                    } else if (motionType >= MOTION_5A && motionType <= MOTION_JC) {
                        // Normals metadata only
                        prearmed = true;
                    }
                } else {
                    // Frame1 injection path
                    if (motionType >= MOTION_5A && motionType <= MOTION_JC) {
                        prearmed = true;
                    } else if (actionType == ACTION_BACKDASH || actionType == ACTION_FORWARD_DASH) {
                        prearmed = true; // inject on wake
                    } else if (motionType >= MOTION_236A) {
                        prearmed = true; // special to inject frame1
                        LogOut("[AUTO-ACTION] P2 wake special set for frame1 inject (toggle=OFF)", true);
                    }
                }
                if (prearmed) {
                    if (g_wakeBufferingEnabled.load() && motionType >= MOTION_236A) {
                        EnableP2ControlForAutoAction();
                        s_p2WakePrearmIsSpecial = true;
                    } else {
                        s_p2WakePrearmIsSpecial = false;
                    }
                    s_p2WakePrearmed = true;
                    s_p2WakePrearmExpiry = frameCounter.load() + 120;
                    s_p2WakePrearmActionType = actionType;
                    LogOut("[AUTO-ACTION] P2 wake prearm stored: action=" + std::to_string(actionType) +
                           " motion=" + std::to_string(motionType) +
                           " special=" + std::to_string(s_p2WakePrearmIsSpecial) +
                           " currMoveID=" + std::to_string(moveID2) +
                           " frame=" + std::to_string(frameCounter.load()), true);
                } else if (detailedLogging.load()) {
                    LogOut("[AUTO-ACTION] P2 wake pre-arm failed", true);
                }
            }
        }

        // On Wakeup trigger (fallback when not pre-armed)
        if (!shouldTrigger && triggerOnWakeupEnabled.load()) {
            if (s_p2WakePrearmed && IsGroundtech(prevMoveID2) && IsActionable(moveID2)) {
                LogOut("[AUTO-ACTION] P2 wake exec: action=" + std::to_string(s_p2WakePrearmActionType) +
                       " special=" + std::to_string(s_p2WakePrearmIsSpecial) +
                       " prevMoveID=" + std::to_string(prevMoveID2) +
                       " currMoveID=" + std::to_string(moveID2) +
                       " frame=" + std::to_string(frameCounter.load()), true);
                g_lastActiveTriggerType.store(TRIGGER_ON_WAKEUP);
                g_lastActiveTriggerFrame.store(frameCounter.load());
                if (!s_p2WakePrearmIsSpecial && s_p2WakePrearmActionType >= 0) {
                    int at = s_p2WakePrearmActionType;
                    if (at == ACTION_FINAL_MEMORY) {
                        int charId = displayData.p2CharID;
                        EnableP2ControlForAutoAction();
                        bool ok = ExecuteFinalMemory(2, charId);
                        LogOut(std::string("[AUTO-ACTION][FM] P2 wake frame1 FM inject ") + (ok?"ok":"fail"), true);
                        p2TriggerActive = false; p2TriggerCooldown = 0;
                        s_p2WakePrearmed = false; s_p2WakePrearmActionType = -1;
                    } else {
                        int motionType = ConvertTriggerActionToMotion(at, TRIGGER_ON_WAKEUP);
                        int buttonMask = 0;
                        if ((at >= ACTION_5A && at <= ACTION_2C) || (at >= ACTION_6A && at <= ACTION_4C)) {
                            int button = (at >= ACTION_5A && at <= ACTION_2C) ? ((at - ACTION_5A) % 3) : ((at - ACTION_6A) % 3);
                            buttonMask = (1 << (4 + button));
                        } else if (at >= ACTION_JA && at <= ACTION_JC) {
                            int button = (at - ACTION_JA) % 3;
                            buttonMask = (1 << (4 + button));
                        }
                        if (at == ACTION_BACKDASH || at == ACTION_FORWARD_DASH) {
                            QueueMotionInput(2, motionType, 0);
                        } else if ((motionType >= MOTION_5A && motionType <= MOTION_5C) || (motionType >= MOTION_6A && motionType <= MOTION_4C)) {
                            ImmediateInput::PressFor(2, buttonMask, 2);
                        } else if (motionType >= MOTION_2A && motionType <= MOTION_2C) {
                            ImmediateInput::PressFor(2, GAME_INPUT_DOWN | buttonMask, 2);
                        } else if (motionType >= MOTION_236A) {
                            if (buttonMask == 0) {
                                int atStrength = GetSpecialMoveStrength(at, TRIGGER_ON_WAKEUP);
                                buttonMask = (1 << (4 + atStrength));
                            }
                            EnableP2ControlForAutoAction();
                            FreezeBufferForMotion(2, motionType, buttonMask);
                            { std::stringstream bm; bm << std::hex << buttonMask; LogOut(std::string("[AUTO-ACTION] P2 wake special frame1 buffer applied (index frozen) btnMask=0x") + bm.str(), true); }
                        }
                    }
                }
                s_p2WakePrearmed = false;
                s_p2WakePrearmActionType = -1;
            } else if (!s_p2WakePrearmed && IsGroundtech(prevMoveID2) && IsActionable(moveID2)) {
                shouldTrigger = true;
                triggerType = TRIGGER_ON_WAKEUP;
                delay = triggerOnWakeupDelay.load();
                actionMoveID = GetActionMoveID(triggerOnWakeupAction.load(), TRIGGER_ON_WAKEUP, 2);
                
                LogOut("[AUTO-ACTION] P2 On Wakeup trigger activated", detailedLogging.load());
                if (detailedLogging.load()) {
                    LogOut("[TRIGGER_TIMING] P2 wake delay=" + std::to_string(s_p2Timers.lastWakeDelay) +
                           ", sinceWake=" + std::to_string(s_p2Timers.sinceWake), true);
                }
            }
        }
        
        if (shouldTrigger) {
            StartTriggerDelay(2, triggerType, actionMoveID, delay);
        } else {
            // Don't spam repetitive diagnostics; throttling handled above
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

    // Clear pre-arm flags when window passes to avoid sticking
    int now = frameCounter.load();
    if (s_p1WakePrearmed && now > s_p1WakePrearmExpiry) {
        LogOut("[AUTO-ACTION] P1 wake pre-arm expired (no execution)", detailedLogging.load());
        s_p1WakePrearmed = false;
    }
    if (s_p2WakePrearmed && now > s_p2WakePrearmExpiry) {
        LogOut("[AUTO-ACTION] P2 wake pre-arm expired (no execution)", detailedLogging.load());
        s_p2WakePrearmed = false;
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

// Back-compat wrapper: fetch move IDs once here (with basic caching) and forward to core
void MonitorAutoActions() {
    static short s_prevMoveID1 = 0, s_prevMoveID2 = 0;
    // Fast-path: if nothing could possibly generate work (feature disabled, no triggers, no delays,
    // cooldowns, wake prearms, or pending control restores) skip all memory reads & processing.
    if (!AutoActionWorkPending()) {
        if (detailedLogging.load()) {
            static int s_lastIdleLogFrame = 0;
            int nowF = frameCounter.load();
            // Roughly every 30 real seconds at 192fps (192 * 30 = 5760)
            if (nowF - s_lastIdleLogFrame >= 5760) {
                LogOut("[AUTO-ACTION] Idle (disabled/no triggers) - skipping per-frame processing", true);
                s_lastIdleLogFrame = nowF;
            }
        }
        return;
    }
    uintptr_t base = GetEFZBase();
    if (!base) return;
    static uintptr_t addr1 = 0, addr2 = 0; static int cacheCtr = 0;
    if (cacheCtr++ >= 192 || !addr1 || !addr2) {
        addr1 = ResolvePointer(base, EFZ_BASE_OFFSET_P1, MOVE_ID_OFFSET);
        addr2 = ResolvePointer(base, EFZ_BASE_OFFSET_P2, MOVE_ID_OFFSET);
        cacheCtr = 0;
    }
    short m1 = 0, m2 = 0;
    if (addr1) SafeReadMemory(addr1, &m1, sizeof(short));
    if (addr2) SafeReadMemory(addr2, &m2, sizeof(short));
    MonitorAutoActionsImpl(m1, m2, s_prevMoveID1, s_prevMoveID2);
    s_prevMoveID1 = m1; s_prevMoveID2 = m2;
}

void MonitorAutoActions(short moveID1, short moveID2, short prevMoveID1, short prevMoveID2) {
    MonitorAutoActionsImpl(moveID1, moveID2, prevMoveID1, prevMoveID2);
}

void ResetActionFlags() {
    p1ActionApplied = false;
    p2ActionApplied = false;

    // If we're resetting action flags, also restore P2 control if needed
    RestoreP2ControlState();
    LogOut("[AUTO-ACTION] ResetActionFlags invoked (control restored if overridden)", true);
}

void ClearDelayStatesIfNonActionable() {
    // Only run this if there are actually delays active
    if (!p1DelayState.isDelaying && !p2DelayState.isDelaying) {
        return;
    }
    
    uintptr_t base = GetEFZBase();
    if (!base) return;
    
    // Use cached addresses
    static uintptr_t cachedMoveIDAddr1 = 0;
    static uintptr_t cachedMoveIDAddr2 = 0;
    static int cacheRefreshCounter = 0;
    
    if (cacheRefreshCounter++ >= 64) {
        cachedMoveIDAddr1 = ResolvePointer(base, EFZ_BASE_OFFSET_P1, MOVE_ID_OFFSET);
        cachedMoveIDAddr2 = ResolvePointer(base, EFZ_BASE_OFFSET_P2, MOVE_ID_OFFSET);
        cacheRefreshCounter = 0;
    }
    
    short moveID1 = 0, moveID2 = 0;
    if (cachedMoveIDAddr1) SafeReadMemory(cachedMoveIDAddr1, &moveID1, sizeof(short));
    if (cachedMoveIDAddr2) SafeReadMemory(cachedMoveIDAddr2, &moveID2, sizeof(short));
    
    // CRITICAL FIX: Don't clear delays if the player is in the middle of executing the action we just applied
    // Only clear if they're in clearly bad states (hitstun, blockstun, etc.)
    bool p1InBadState = IsBlockstun(moveID1) || IsHitstun(moveID1) || IsFrozen(moveID1) || IsThrown(moveID1) || IsLaunched(moveID1) || IsAirtech(moveID1) || IsGroundtech(moveID1);
    bool p2InBadState = IsBlockstun(moveID2) || IsHitstun(moveID2) || IsFrozen(moveID2) || IsThrown(moveID2) || IsLaunched(moveID2) || IsAirtech(moveID2) || IsGroundtech(moveID2);
    
    if (p1DelayState.isDelaying && p1InBadState) {
        p1DelayState.isDelaying = false;
        p1DelayState.triggerType = TRIGGER_NONE;
        p1DelayState.pendingMoveID = 0;
        LogOut("[AUTO-ACTION] Cleared P1 delay - in bad state (moveID " + std::to_string(moveID1) + ")", true);
    }
    
    if (p2DelayState.isDelaying && p2InBadState) {
        p2DelayState.isDelaying = false;
        p2DelayState.triggerType = TRIGGER_NONE;
        p2DelayState.pendingMoveID = 0;
        LogOut("[AUTO-ACTION] Cleared P2 delay - in bad state (moveID " + std::to_string(moveID2) + ")", true);
        // Also clear trigger cooldown so next opportunity can re-arm promptly
        p2TriggerActive = false;
        p2TriggerCooldown = 0;
    }
}

// Replace the ApplyAutoAction function with this implementation:
void ApplyAutoAction(int playerNum, uintptr_t moveIDAddr, short currentMoveID, short prevMoveID) {
    // If caller didn't provide current move id, try to read it for better restore tracking
    if (currentMoveID == 0 && moveIDAddr != 0) {
        SafeReadMemory(moveIDAddr, &currentMoveID, sizeof(short));
    }
    // Get the trigger type from the player's delay state
    int triggerType = (playerNum == 1) ? p1DelayState.triggerType : p2DelayState.triggerType;

    // Get the appropriate action for this trigger
    int actionType = 0;
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
                    g_controlRestoreTimeout.store(240); // a little longer for FM sequences
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
    int resolvedStrength = GetSpecialMoveStrength(actionType, triggerType); // 0=A 1=B 2=C
    // Convert action to motion type and determine button mask
    int motionType = ConvertTriggerActionToMotion(actionType, triggerType);
    int buttonMask = 0;
    
    // Determine button mask based on action type
    if (actionType >= ACTION_5A && actionType <= ACTION_2C) {
        // Neutral and crouching normals (5A/B/C,2A/B/C)
        int button = (actionType - ACTION_5A) % 3;  // 0=A, 1=B, 2=C
        buttonMask = (1 << (4 + button));  // A=16, B=32, C=64
    } else if (actionType >= ACTION_JA && actionType <= ACTION_JC) {
        // Jump attacks
        int button = (actionType - ACTION_JA) % 3;  // 0=A, 1=B, 2=C
        buttonMask = (1 << (4 + button));  // A=16, B=32, C=64
    } else if (actionType >= ACTION_6A && actionType <= ACTION_4C) {
        // Directional forward/back normals (6A/B/C, 4A/B/C)
        int button = (actionType - ACTION_6A) % 3; // groups of 3
        buttonMask = (1 << (4 + button));
    } else if (actionType >= ACTION_QCF) {
        // Strength-based actions only (exclude jump/dash/block/custom)
        switch (actionType) {
            case ACTION_QCF:
            case ACTION_DP:
            case ACTION_QCB:
            case ACTION_421:
            case ACTION_SUPER1:
            case ACTION_SUPER2:
            case ACTION_236236:
            case ACTION_214214:
            case ACTION_641236: {
                int strength = GetSpecialMoveStrength(actionType, triggerType); // 0=A 1=B 2=C
                buttonMask = (1 << (4 + strength));
                break; }
            default: break;
        }
    }

    LogOut("[AUTO-ACTION] Converting action=" + std::to_string(actionType) +
           " trigger=" + std::to_string(triggerType) +
           " strength=" + std::to_string(resolvedStrength) +
           " => motionType=" + std::to_string(motionType) +
           " prelimButtonMask=" + std::to_string(buttonMask), true);

    // If buttonMask is zero, use default
    if (buttonMask == 0) {
        LogOut("[AUTO-ACTION] Button mask was 0; defaulting to A (likely non-button action)", true);
        buttonMask = GAME_INPUT_A; // Default to A button (16)
    }
    else {
        LogOut("[AUTO-ACTION] Final buttonMask=" + std::to_string(buttonMask) +
               " (A=16,B=32,C=64)", true);
    }
    
    bool success = false;
    bool isRegularMove = (motionType >= MOTION_5A && motionType <= MOTION_JC) || (motionType >= MOTION_6A && motionType <= MOTION_4C);
    bool isSpecialMove = (motionType >= MOTION_236A);
    // Exclude dashes from being treated as specials for restore/control tracking
    if (actionType == ACTION_FORWARD_DASH || actionType == ACTION_BACKDASH || actionType == ACTION_BACK_DASH) {
        isSpecialMove = false;
    }

    // Special handling: Jump must be injected via immediate input register with direction
    if (actionType == ACTION_JUMP) {
        // Determine jump direction: 0=neutral, 1=forward, 2=backward from strength slot
        int dir = 0;
        switch (triggerType) {
            case TRIGGER_AFTER_BLOCK: dir = triggerAfterBlockStrength.load(); break;
            case TRIGGER_ON_WAKEUP: dir = triggerOnWakeupStrength.load(); break;
            case TRIGGER_AFTER_HITSTUN: dir = triggerAfterHitstunStrength.load(); break;
            case TRIGGER_AFTER_AIRTECH: dir = triggerAfterAirtechStrength.load(); break;
            case TRIGGER_ON_RG: dir = triggerOnRGStrength.load(); break;
            default: dir = 0; break;
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
                // If follow-up configured, give slightly longer window for dash-normal injection
                g_controlRestoreTimeout.store(fdf > 0 ? 120 : 90);
                g_pendingControlRestore.store(true);
                LogOut(std::string("[AUTO-ACTION][DASH] Armed control restore monitor (") + (fdf>0?"follow-up":"no follow-up") + ")", true);
            }
        } else if (success && actionType == ACTION_BACKDASH) {
            if (playerNum == 2 && !g_pendingControlRestore.load()) {
                g_lastP2MoveID.store(-1);
                g_controlRestoreTimeout.store(90);
                g_pendingControlRestore.store(true);
                LogOut("[AUTO-ACTION][DASH] Armed control restore monitor (backdash)", true);
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

        if (motionType >= MOTION_5A && motionType <= MOTION_5C) {
            inputMask = buttonMask; // Neutral ground normals
        } else if (motionType >= MOTION_2A && motionType <= MOTION_2C) {
            inputMask = GAME_INPUT_DOWN | buttonMask; // Crouching normals
        } else if (motionType >= MOTION_JA && motionType <= MOTION_JC) {
            inputMask = buttonMask; // Air normals (jump direction handled separately at jump time)
        } else if (motionType >= MOTION_6A && motionType <= MOTION_6C) {
            inputMask = forwardDir | buttonMask; // Forward direction + button
        } else if (motionType >= MOTION_4A && motionType <= MOTION_4C) {
            inputMask = backDir | buttonMask; // Back direction + button
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
            g_controlRestoreTimeout.store(180);
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
    if (g_p2ControlOverridden) {
        // Make sure to stop any buffer freezing when restoring control
        StopBufferFreezing();
        
        // IMPORTANT: Force a longer cooldown period to prevent immediate re-triggering
        p2TriggerActive = true;
    p2TriggerCooldown = TRIGGER_COOLDOWN_FRAMES;
        LogOut("[AUTO-ACTION] Enforcing extended trigger cooldown after control restore", true);
        
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
        
        // CRITICAL: Restore CPU control flag at game state level FIRST
        // This is what actually determines if the character is under player or AI control
        const uintptr_t P2_CPU_FLAG_OFFSET = 4931;
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
    if (!g_p2ControlOverridden) return;
    uintptr_t base = GetEFZBase();
    if (!base) return;
    
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
    const uintptr_t P2_CPU_FLAG_OFFSET = 4931;
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
    
    // NOTE: We deliberately DO NOT clear the input buffer here because Counter RG needs the
    // pre-armed special motion to remain in the buffer so it can execute after RG completes.
    // The buffer will be cleared naturally when the special executes or times out.
    
    g_p2ControlOverridden = false;
}

// Add this function to auto_action.h
void ProcessAutoControlRestore() {
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
        // Cache address and throttle to every other frame to reduce CPU
        static uintptr_t moveIDAddr2 = 0; // our player (P2)
        static uintptr_t moveIDAddr1 = 0; // opponent (P1)
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
        if (!moveIDAddr2) moveIDAddr2 = ResolvePointer(base, EFZ_BASE_OFFSET_P2, MOVE_ID_OFFSET);
        if (!moveIDAddr1) moveIDAddr1 = ResolvePointer(base, EFZ_BASE_OFFSET_P1, MOVE_ID_OFFSET);

        short moveID2 = 0;
        short oppMoveID = 0;
        if (moveIDAddr2) {
            SafeReadMemory(moveIDAddr2, &moveID2, sizeof(short));
        }
        if (moveIDAddr1) {
            SafeReadMemory(moveIDAddr1, &oppMoveID, sizeof(short));
        }

        // Ensure cached character IDs (once per MATCH phase) BEFORE deciding on throttle
        EnsureCachedCharacterIDs(base);
        bool isKaori = (g_cachedP2CharID == CHAR_ID_KAORI);

        if (!g_crgFastRestore.load()) {
            // Only apply throttle if NOT Kaori (or once we've already seen her dash start). We can't reference
            // sawNonZeroMoveID yet (declared later) so we just blanket disable throttle for Kaori.
            if (!isKaori) {
                if ((s_throttle++ & 1) != 0) return; // 96 Hz sampling for non-Kaori normal path
            } else if ((s_throttle % 192) == 0 && detailedLogging.load()) {
                LogOut("[AUTO-ACTION][DASH][KAORI] Full-rate sampling active (disabling throttle to catch ID 250)", true);
            }
        }
        // DASH RESTORE RULES:
        // 1. If we just entered forward/back dash start and no follow-up configured -> immediate restore
        // 2. If any normal (>=200) starts after a dash-follow sequence -> restore
        // 3. If dash cancelled into blockstun/hitstun/airtech -> restore
        // 4. Fallback to existing timeout / move completion logic
    // isKaori already computed above
    bool dashStartNow = (moveID2 == FORWARD_DASH_START_ID || moveID2 == BACKWARD_DASH_START_ID || (isKaori && moveID2 == KAORI_FORWARD_DASH_START_ID));
    bool dashStartJustEntered = dashStartNow && (s_prevMoveID2 != moveID2);
    // Treat Kaori's forward dash start (250) as a dash state, NOT a normal. Only consider a "dash normal" when the new move
    // ID is a normal/special (>=200) AND it is not part of any dash start/recovery sequence.
    bool dashNormalStarted = (moveID2 >= 200);
    bool moveIsDash = (moveID2 == FORWARD_DASH_START_ID || moveID2 == FORWARD_DASH_RECOVERY_ID ||
               moveID2 == FORWARD_DASH_RECOVERY_SENTINEL_ID || moveID2 == BACKWARD_DASH_START_ID ||
               moveID2 == BACKWARD_DASH_RECOVERY_ID || (isKaori && moveID2 == KAORI_FORWARD_DASH_START_ID));
    // Recompute dashNormalStarted excluding any dash state (prevents Kaori 250 from being misclassified)
    if (dashNormalStarted && moveIsDash) dashNormalStarted = false;
    bool dashCancelled = (!moveIsDash) && (IsBlockstun(moveID2) || IsHitstun(moveID2) || IsAirtech(moveID2));
    
    // CRITICAL: If we queued a dash with follow-up but got interrupted BEFORE dash started (never reached 163/165),
    // clear the pending follow-up so the generic restore path can execute and clear the buffer.
    if (g_dashDeferred.pendingSel.load() > 0 && dashCancelled && !g_recentDashQueued.load()) {
        LogOut("[AUTO-ACTION][DASH] Dash follow-up cancelled by interrupt before dash started", true);
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
    bool wasDashing = (s_prevMoveID2 == FORWARD_DASH_START_ID || s_prevMoveID2 == BACKWARD_DASH_START_ID || (isKaori && s_prevMoveID2 == KAORI_FORWARD_DASH_START_ID));
    bool stillDashing = (moveID2 == FORWARD_DASH_START_ID || moveID2 == BACKWARD_DASH_START_ID ||
                 moveID2 == FORWARD_DASH_RECOVERY_ID || moveID2 == BACKWARD_DASH_RECOVERY_ID ||
                 (isKaori && moveID2 == KAORI_FORWARD_DASH_START_ID));
        bool leftDashState = wasDashing && !stillDashing;
        
        bool doDashRestore = false;
        
        // PRIORITY 1: If we left the dash state entirely (163/165 → anything else), restore immediately.
        // This handles all transitions: natural completion, dash normals, cancels, interrupts, etc.
        if (leftDashState) {
            LogOut("[AUTO-ACTION][DASH] Restore: left dash state (163/165/250 → " + std::to_string(moveID2) + ")", true);
            doDashRestore = true;
        }
        // PRIORITY 2: Legacy paths for edge cases
        else if (dashNormalStarted && wasDashing) {
            LogOut("[AUTO-ACTION][DASH] Restore: dash normal detected (moveID=" + std::to_string(moveID2) + ")", true);
            doDashRestore = true;
        } else if (dashCancelled && wasDashing) {
            LogOut("[AUTO-ACTION][DASH] Restore: dash cancelled by state (moveID=" + std::to_string(moveID2) + ")", true);
            doDashRestore = true;
        } else if (moveID2 == FORWARD_DASH_RECOVERY_SENTINEL_ID) {
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
            // CRITICAL: Don't clear g_pendingControlRestore immediately. Start grace period counter
            // to prevent triggers from firing again during hitstun recovery.
            g_restoreGraceCounter = RESTORE_GRACE_PERIOD;
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
            if (detailedLogging.load()) {
                LogOut("[AUTO-ACTION][DASH] New dash attempt detected; resetting sawNonZeroMoveID", true);
            }
        }
        int timeoutSnapshot = g_controlRestoreTimeout.load();
        if (g_recentDashQueued.load() && moveID2 == 0 && !sawNonZeroMoveID) {
            int age = frameCounter.load() - g_recentDashQueuedFrame.load();
            extern int p2CurrentMotionType; extern bool p2QueueActive; extern int p2QueueIndex; extern int p2FrameCounter; extern std::vector<InputFrame> p2InputQueue;
            int qSize = (int)p2InputQueue.size();
            if ((age % 4) == 0) {
                LogOut(std::string("[AUTO-ACTION][DASH][TRACE] waiting age=") + std::to_string(age) +
                       " queueActive=" + (p2QueueActive?"1":"0") +
                       " qIdx=" + std::to_string(p2QueueIndex) + "/" + std::to_string(qSize) +
                       " frameInStep=" + std::to_string(p2FrameCounter) +
                       " motionType=" + std::to_string(p2CurrentMotionType) +
                       " timeout=" + std::to_string(timeoutSnapshot), true);
                if (p2QueueActive && p2QueueIndex < qSize) {
                    uint8_t mask = p2InputQueue[p2QueueIndex].inputMask;
                    LogOut(std::string("[AUTO-ACTION][DASH][TRACE] current mask=") + std::to_string((int)mask), true);
                }
            }
            if (age < 120) {
                if (timeoutSnapshot < 90) { g_controlRestoreTimeout.store(90); }
                s_prevMoveID2 = moveID2; return; // hold restore
            } else if (age == 120) {
                LogOut("[AUTO-ACTION][DASH] Requeue forward dash after 120f no-start", true);
                QueueMotionInput(2, MOTION_FORWARD_DASH, 0);
                g_recentDashQueuedFrame.store(frameCounter.load());
                if (timeoutSnapshot < 120) g_controlRestoreTimeout.store(120);
                s_prevMoveID2 = moveID2; return;
            } else if (age < 160) {
                s_prevMoveID2 = moveID2; return; // continue holding
            } else {
                LogOut("[AUTO-ACTION][DASH] Abandon wait; dash never produced non-zero moveID", true);
                g_recentDashQueued.store(false);
            }
        }

        int timeout = g_controlRestoreTimeout.load();
        if (timeout > 0) {
            // Only decrement while positive to avoid unbounded negative underflow
            int prev = g_controlRestoreTimeout.fetch_sub(1);
            timeout = prev; // value before decrement for logging consistency
        } else {
            // Clamp once (idempotent) and keep it at 0
            if (timeout < 0) {
                g_controlRestoreTimeout.store(0);
                static bool s_loggedUnderflow = false;
                if (!s_loggedUnderflow) {
                    LogOut("[AUTO-ACTION][WARN] Control restore timeout underflow detected; clamping to 0", true);
                    s_loggedUnderflow = true;
                }
            }
            timeout = 0;
        }
        // Throttled monitoring logging:
        //  - While timeout > 0: roughly once per second (192 frames)
        //  - After timeout reaches 0: once every 960 frames (~5s) to prevent log spam
        if (detailedLogging.load()) {
            static int s_lastZeroTimeoutLogFrame = -1000000;
            int fcNow = frameCounter.load();
            bool doLog = false;
            if (timeout > 0) {
                if ((timeout % 192) == 0) doLog = true; // snapshot at countdown intervals
            } else { // timeout == 0
                if (fcNow - s_lastZeroTimeoutLogFrame >= 960) { // ~5 seconds at 192fps
                    doLog = true;
                    s_lastZeroTimeoutLogFrame = fcNow;
                }
            }
            if (doLog) {
                LogOut("[AUTO-ACTION] Monitoring move execution: MoveID=" +
                       std::to_string(moveID2) + ", LastMoveID=" +
                       std::to_string(g_lastP2MoveID.load()) +
                       ", Timeout=" + std::to_string(timeout), true);
            }
        }

        if (moveID2 > 0) { sawNonZeroMoveID = true; g_recentDashQueued.store(false); }

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
                    // CRITICAL: Don't clear immediately, start grace period
                    g_restoreGraceCounter = RESTORE_GRACE_PERIOD;
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
                // CRITICAL: Don't clear immediately, start grace period
                g_restoreGraceCounter = RESTORE_GRACE_PERIOD;
                g_crgFastRestore.store(false);
                g_lastP2MoveID.store(-1);
                sawNonZeroMoveID = false;
                p2TriggerCooldown = 0; p2TriggerActive = false;
                return;
            }
        }

    // A dash-specific guard: if we queued a forward/back dash and never observed ANY non-zero move id yet,
    // treat the transition back to 0 as non-completion (prevents premature restore destroying dash attempt).
    bool queuedDashNoStart = g_recentDashQueued.load() && !sawNonZeroMoveID && moveID2 == 0;
    bool moveChangedCandidate = (moveID2 != g_lastP2MoveID.load() && moveID2 == 0 && sawNonZeroMoveID);
    bool dashFollowPending = (g_dashDeferred.pendingSel.load() > 0);
    bool suppressGenericRestore = (moveChangedCandidate && dashFollowPending);
        if (suppressGenericRestore) {
            // Diagnostic: show we deliberately ignored a generic completion while a dash follow-up is pending
            LogOut("[AUTO-ACTION][DASH] Suppressing generic restore (pending follow-up active)", detailedLogging.load());
        }
        bool moveChanged = moveChangedCandidate && !suppressGenericRestore;
    bool timeoutExpired = (timeout == 0);
        
        // If pending restore flag is set but we are no longer actually overriding control (and not in grace),
        // clear it defensively to avoid indefinite sampling spam.
        if (g_pendingControlRestore.load() && !g_p2ControlOverridden && g_restoreGraceCounter == 0 &&
            !dashFollowPending && !g_recentDashQueued.load()) {
            LogOut("[AUTO-ACTION] Clearing stale g_pendingControlRestore (no override active)", detailedLogging.load());
            g_pendingControlRestore.store(false);
            s_prevMoveID2 = moveID2;
            return;
        }

        // Generic restore: require pending flag + valid context to avoid repeated restores when already clean.
        bool restorePendingNow = g_pendingControlRestore.load();
        bool haveTrackableMove = (g_lastP2MoveID.load() != -1);
    static int s_lastGenericRestoreFrame = -1; // dedupe generic restore per frame
    bool baseRestoreCond = (moveChanged || timeoutExpired) &&
                !dashFollowPending &&
                !queuedDashNoStart &&
                restorePendingNow &&
                (g_p2ControlOverridden || haveTrackableMove);
    bool shouldGenericRestore = baseRestoreCond && (frameCounter.load() != s_lastGenericRestoreFrame);

        if (shouldGenericRestore) {
            LogOut("[AUTO-ACTION] Auto-restoring P2 control state after move execution", detailedLogging.load());
            LogOut("[AUTO-ACTION] Reason: " +
                   std::string(moveChanged ? "Move completed" : "Timeout expired") +
                   ", MoveID: " + std::to_string(moveID2), detailedLogging.load());

            RestoreP2ControlState();
            // CRITICAL: Don't clear g_pendingControlRestore immediately. Start grace period counter.
            g_restoreGraceCounter = RESTORE_GRACE_PERIOD;
            g_crgFastRestore.store(false);
            g_lastP2MoveID.store(-1);
            sawNonZeroMoveID = false;
            s_lastGenericRestoreFrame = frameCounter.load();

            // If we are no longer overriding control after restore, there's nothing left to monitor except grace.
            // Convert pending restore into grace-only sentinel by clearing pending early; grace counter
            // already blocks triggers until it reaches 0.
            if (!g_p2ControlOverridden) {
                g_pendingControlRestore.store(false);
            }

            // If we just restored after a dash and a forward dash follow-up is still pending,
            // keep a minimal cooldown (1 *internal* frame => ~instant visually) so we do not
            // immediately retrigger the same After Block while the dash follow-up logic runs.
            p2TriggerCooldown = 0; // allow prompt re-trigger after restoring
            p2TriggerActive = false;
            LogOut("[AUTO-ACTION] Cleared P2 trigger cooldown after restore", detailedLogging.load());
        } else {
            if ((moveChanged || timeoutExpired) && restorePendingNow && !shouldGenericRestore && detailedLogging.load()) {
                LogOut(std::string("[AUTO-ACTION][TRACE] Suppressed generic restore: pending=") + (restorePendingNow?"1":"0") +
                       " override=" + (g_p2ControlOverridden?"1":"0") +
                       " lastMoveValid=" + (haveTrackableMove?"1":"0") +
                       " dashFollow=" + (dashFollowPending?"1":"0") +
                       " queuedDashNoStart=" + (queuedDashNoStart?"1":"0"), true);
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

    // Reset delay states
    p1DelayState = {false, 0, TRIGGER_NONE, 0};
    p2DelayState = {false, 0, TRIGGER_NONE, 0};

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
    g_controlRestoreTimeout.store(0);
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