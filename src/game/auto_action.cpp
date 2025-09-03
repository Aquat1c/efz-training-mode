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
#include <cmath>
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
static bool p2AfterBlockLoggedInflight = false;     // logs "in-flight override/restore" once per in-flight window
static bool p2AfterBlockLoggedNotActionable = false; // logs "now not actionable yet" once per not-actionable window
static int p1TriggerCooldown = 0;
static int p2TriggerCooldown = 0;
// Further reduce cooldown to re-arm triggers faster (was 6)
static constexpr int TRIGGER_COOLDOWN_FRAMES = 3; // ~0.016s @192fps (~1 visual frame)
bool g_p2ControlOverridden = false;
uint32_t g_originalP2ControlFlag = 1; // Default to AI control

// Add at the top with other global variables (around line 40)
std::atomic<bool> g_pendingControlRestore(false);
std::atomic<int> g_controlRestoreTimeout(0);
std::atomic<short> g_lastP2MoveID(-1);
const int CONTROL_RESTORE_TIMEOUT = 180; // 180 internal frames = 1 second

// Pre-arm flags for On Wakeup: buffer inputs during GROUNDTECH_RECOVERY to fire on wake
static bool s_p1WakePrearmed = false;
static bool s_p2WakePrearmed = false;
static int  s_p1WakePrearmExpiry = 0;
static int  s_p2WakePrearmExpiry = 0;

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
    // If any triggers are enabled, we may need to evaluate
    bool triggersEnabled = triggerAfterBlockEnabled.load() || triggerOnWakeupEnabled.load() ||
                           triggerAfterHitstunEnabled.load() || triggerAfterAirtechEnabled.load();
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
    // First check if it's a custom action
    if (actionType == ACTION_CUSTOM) {
        short customID = 0;
        // Move custom moveID selection logs to detailed mode
        switch (triggerType) {
            case TRIGGER_AFTER_AIRTECH:
                customID = triggerAfterAirtechCustomID.load();
                LogOut("[AUTO-ACTION] Using After Airtech custom moveID: " + std::to_string(customID), detailedLogging.load());
                return customID;
            case TRIGGER_AFTER_BLOCK:
                customID = triggerAfterBlockCustomID.load();
                LogOut("[AUTO-ACTION] Using After Block custom moveID: " + std::to_string(customID), detailedLogging.load());
                return customID;
            case TRIGGER_ON_WAKEUP:
                customID = triggerOnWakeupCustomID.load();
                LogOut("[AUTO-ACTION] Using On Wakeup custom moveID: " + std::to_string(customID), detailedLogging.load());
                return customID;
            case TRIGGER_AFTER_HITSTUN:
                customID = triggerAfterHitstunCustomID.load();
                LogOut("[AUTO-ACTION] Using After Hitstun custom moveID: " + std::to_string(customID), detailedLogging.load());
                return customID;
            default:
                LogOut("[AUTO-ACTION] Using default custom moveID: 200", detailedLogging.load());
                return 200; // Default to 5A if no trigger specified
        }
    }
    
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
            
            uintptr_t moveIDAddr = ResolvePointer(base, EFZ_BASE_OFFSET_P2, MOVE_ID_OFFSET);
            short currentMoveID = 0;
            
            if (moveIDAddr) {
                SafeReadMemory(moveIDAddr, &currentMoveID, sizeof(short));
                
                // Apply action via input system
                ApplyAutoAction(2, moveIDAddr, 0, 0);
                
                LogOut("[AUTO-ACTION] P2 action applied via input system", true);
                
                p2DelayState.isDelaying = false;
                p2DelayState.triggerType = TRIGGER_NONE;
                p2DelayState.pendingMoveID = 0;
                p2ActionApplied = true;
                
                // Restore P2 control state if we changed it
                RestoreP2ControlState();
            } else {
                LogOut("[AUTO-ACTION] Failed to apply P2 action - invalid moveID address", true);
                p2DelayState.isDelaying = false;
            }
        }
    }
}

void StartTriggerDelay(int playerNum, int triggerType, short moveID, int delayFrames) {
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
     // IMPORTANT: If targeting P2, ensure human control is enabled
    if (playerNum == 2) {
        // Add debug logs to track control state changes
    LogOut("[AUTO-ACTION] Enabling human control for P2 auto-action", detailedLogging.load());
        EnableP2ControlForAutoAction();
    }
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
    // Keep triggers locked while an injection/control-restore is in-flight to avoid re-triggers
    bool p1InFlight = g_manualInputOverride[1].load();
    bool p2InFlight = g_manualInputOverride[2].load() || g_pendingControlRestore.load();

    // P1 cooldown processing
    if (p1TriggerActive) {
        if (p1InFlight) {
            // Hold active; don't let cooldown expire mid-override
            if (p1TriggerCooldown <= 1) p1TriggerCooldown = 1;
        } else if (p1TriggerCooldown > 0) {
            p1TriggerCooldown--;
            if (p1TriggerCooldown <= 0) {
                p1TriggerActive = false;
                LogOut("[AUTO-ACTION] P1 trigger cooldown expired, new triggers allowed", detailedLogging.load());
            }
        }
    }
    
    // P2 cooldown processing
    if (p2TriggerActive) {
        if (p2InFlight) {
            // Hold active; don't let cooldown expire mid-override/restore
            if (p2TriggerCooldown <= 1) p2TriggerCooldown = 1;
        } else if (p2TriggerCooldown > 0) {
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
    if (!autoActionEnabled.load()) {
        return;
    }
    
    // Fast-path out when there's definitively no work to do this frame
    if (!AutoActionWorkPending()) {
        return;
    }
    ProcessTriggerCooldowns();

    // Update timing trackers once per tick for both players
    UpdateStunTimersForPlayer(s_p1Timers, prevMoveID1, moveID1);
    UpdateStunTimersForPlayer(s_p2Timers, prevMoveID2, moveID2);
    
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
                if (actionType >= ACTION_5A && actionType <= ACTION_2C) {
                    int button = (actionType - ACTION_5A) % 3;  // 0=A, 1=B, 2=C
                    buttonMask = (1 << (4 + button));
                } else if (actionType >= ACTION_JA && actionType <= ACTION_JC) {
                    int button = (actionType - ACTION_JA) % 3;
                    buttonMask = (1 << (4 + button));
                } else if (actionType >= ACTION_QCF && actionType <= ACTION_CUSTOM) {
                    int strength = GetSpecialMoveStrength(actionType, TRIGGER_ON_WAKEUP);
                    buttonMask = (1 << (4 + strength));
                }

                bool prearmed = false;
                // Queue normals and dashes; freeze buffer for specials. Skip jump pre-arm.
                if (actionType == ACTION_BACKDASH || actionType == ACTION_FORWARD_DASH ||
                    (motionType >= MOTION_5A && motionType <= MOTION_JC)) {
                    prearmed = QueueMotionInput(1, motionType, 0);
                } else if (motionType >= MOTION_236A) {
                    prearmed = FreezeBufferForMotion(1, motionType, buttonMask);
                }
                if (prearmed) {
                    s_p1WakePrearmed = true;
                    s_p1WakePrearmExpiry = frameCounter.load() + 120; // ~0.6s safety window
                    p1TriggerActive = true; // avoid duplicate fire when actionable
                    p1TriggerCooldown = TRIGGER_COOLDOWN_FRAMES;
                    LogOut("[AUTO-ACTION] P1 On Wakeup pre-armed at recovery (96)", true);
                    LogOut("[AUTO-ACTION] P1 On Wakeup pre-armed at recovery (96)", detailedLogging.load());
                }
            }
        }

        // On Wakeup trigger (fallback if not pre-armed or for normals/jumps)
        if (!shouldTrigger && triggerOnWakeupEnabled.load()) {
            if (s_p1WakePrearmed && IsGroundtech(prevMoveID1) && IsActionable(moveID1)) {
                // We just woke; clear pre-arm flag so future cycles can re-arm
                s_p1WakePrearmed = false;
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
            // Check for transition from airtech to actionable state
            bool wasAirtech = IsAirtech(prevMoveID2);
            // Post-airtech actionable: allow either general actionable states or explicit FALLING
            bool postAirtechNow = (!IsAirtech(moveID2)) && (IsActionable(moveID2) || moveID2 == FALLING_ID);
            if (detailedLogging.load() && canLogTrigDiag()) {
                LogOut("[TRIGGER_DIAG] P2 AfterAirtech check: wasAirtech=" + std::to_string(wasAirtech) +
                       ", postAirtechNow=" + std::to_string(postAirtechNow) +
                       ", targetPlayer=" + std::to_string(targetPlayer), true);
            }

            if (wasAirtech && postAirtechNow) {
                LogOut("[AUTO-ACTION] P2 After Airtech trigger activated", detailedLogging.load());
                shouldTrigger = true;
                triggerType = TRIGGER_AFTER_AIRTECH;
        // Pass visual frames; StartTriggerDelay converts to internal frames
        delay = triggerAfterAirtechDelay.load();
                actionMoveID = GetActionMoveID(triggerAfterAirtechAction.load(), TRIGGER_AFTER_AIRTECH, 2);
            }
        }
    // After Block trigger (do not suppress when p2TriggerActive is true)
    if (!shouldTrigger && triggerAfterBlockEnabled.load()) {
        // Block new trigger if an override/restore is in-flight
        if (g_manualInputOverride[2].load() || g_pendingControlRestore.load()) {
            if (detailedLogging.load() && !p2AfterBlockLoggedInflight) {
                LogOut("[AUTO-ACTION] P2 After Block suppressed (in-flight override/restore)", true);
                p2AfterBlockLoggedInflight = true;
            }
            // Reset the other throttle so it can log once when state changes
            p2AfterBlockLoggedNotActionable = false;
        } else {
            if (IsBlockstun(prevMoveID2) && IsActionable(moveID2)) {
                // Add a check to ensure we're not still in a trigger cooldown
                if (p2TriggerCooldown <= 0) {
                    shouldTrigger = true;
                    triggerType = TRIGGER_AFTER_BLOCK;
                    delay = triggerAfterBlockDelay.load();
                    actionMoveID = GetActionMoveID(triggerAfterBlockAction.load(), TRIGGER_AFTER_BLOCK, 2);

                    // Reset throttles on trigger
                    p2AfterBlockLoggedInflight = false;
                    p2AfterBlockLoggedNotActionable = false;

                    if (detailedLogging.load()) {
                        LogOut("[AUTO-ACTION] P2 After Block trigger activated", true);
                        LogOut("[TRIGGER_TIMING] P2 blockstun dur=" + std::to_string(s_p2Timers.lastBlockDuration) +
                               ", sinceEnd=" + std::to_string(s_p2Timers.sinceBlockEnd), true);
                    }
                } else {
                    LogOut("[AUTO-ACTION] P2 After Block condition met but cooldown active: " + 
                           std::to_string(p2TriggerCooldown), detailedLogging.load());
                }
            } else {
                // Not actionable yet: log once until state changes
                p2AfterBlockLoggedInflight = false;
                if (detailedLogging.load() && !p2AfterBlockLoggedNotActionable && IsBlockstun(prevMoveID2) && !IsActionable(moveID2)) {
                    LogOut("[AUTO-ACTION] P2 After Block: now not actionable yet", true);
                    p2AfterBlockLoggedNotActionable = true;
                }
            }
        }
    }
        
        // After Hitstun trigger
        if (!shouldTrigger && triggerAfterHitstunEnabled.load()) {
            if (IsHitstun(prevMoveID2) && !IsHitstun(moveID2) && !IsAirtech(moveID2)) {
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
                } else if (actionType >= ACTION_QCF && actionType <= ACTION_CUSTOM) {
                    int strength = GetSpecialMoveStrength(actionType, TRIGGER_ON_WAKEUP);
                    buttonMask = (1 << (4 + strength));
                }

                bool prearmed = false;
                if (actionType == ACTION_BACKDASH || actionType == ACTION_FORWARD_DASH ||
                    (motionType >= MOTION_5A && motionType <= MOTION_JC)) {
                    prearmed = QueueMotionInput(2, motionType, 0);
                } else if (motionType >= MOTION_236A) {
                    prearmed = FreezeBufferForMotion(2, motionType, buttonMask);
                }
                if (prearmed) {
                    // Ensure human control so the buffered motion executes reliably
                    EnableP2ControlForAutoAction();
                    s_p2WakePrearmed = true;
                    s_p2WakePrearmExpiry = frameCounter.load() + 120;
                    p2TriggerActive = true;
                    p2TriggerCooldown = TRIGGER_COOLDOWN_FRAMES;
                    LogOut("[AUTO-ACTION] P2 On Wakeup pre-armed at recovery (96)", true);

                    // Start control-restore watcher similar to normal apply path
                    g_lastP2MoveID.store(prevMoveID2);
                    g_pendingControlRestore.store(true);
                    g_controlRestoreTimeout.store(180);
                }
            }
        }

        // On Wakeup trigger (fallback when not pre-armed)
        if (!shouldTrigger && triggerOnWakeupEnabled.load()) {
            if (s_p2WakePrearmed && IsGroundtech(prevMoveID2) && IsActionable(moveID2)) {
                s_p2WakePrearmed = false;
            }
            if (!s_p2WakePrearmed && IsGroundtech(prevMoveID2) && IsActionable(moveID2)) {
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

    // Clear pre-arm flags when window passes to avoid sticking
    int now = frameCounter.load();
    if (s_p1WakePrearmed && now > s_p1WakePrearmExpiry) {
        s_p1WakePrearmed = false;
        // Allow immediate re-arm if nothing fired
        p1TriggerActive = false;
        p1TriggerCooldown = 0;
    }
    if (s_p2WakePrearmed && now > s_p2WakePrearmExpiry) {
        s_p2WakePrearmed = false;
        p2TriggerActive = false;
        p2TriggerCooldown = 0;
    }
}

// Back-compat wrapper: fetch move IDs once here (with basic caching) and forward to core
void MonitorAutoActions() {
    static short s_prevMoveID1 = 0, s_prevMoveID2 = 0;
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
    bool p1InBadState = IsBlockstun(moveID1) || IsHitstun(moveID1) || IsFrozen(moveID1);
    bool p2InBadState = IsBlockstun(moveID2) || IsHitstun(moveID2) || IsFrozen(moveID2);
    
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
        default:
            actionType = ACTION_5A; // Default to 5A
            break;
    }

    // Convert action to motion type and determine button mask
    int motionType = ConvertTriggerActionToMotion(actionType, triggerType);
    int buttonMask = 0;
    
    // Determine button mask based on action type
    if (actionType >= ACTION_5A && actionType <= ACTION_2C) {
        // Normal attacks
        int button = (actionType - ACTION_5A) % 3;  // 0=A, 1=B, 2=C
        buttonMask = (1 << (4 + button));  // A=16, B=32, C=64
    } else if (actionType >= ACTION_JA && actionType <= ACTION_JC) {
        // Jump attacks
        int button = (actionType - ACTION_JA) % 3;  // 0=A, 1=B, 2=C
        buttonMask = (1 << (4 + button));  // A=16, B=32, C=64
    } else if (actionType >= ACTION_QCF && actionType <= ACTION_CUSTOM) {
        // Special moves - get strength from helper function
        int strength = GetSpecialMoveStrength(actionType, triggerType);
        buttonMask = (1 << (4 + strength));  // 0=A(16), 1=B(32), 2=C(64)
    }

    LogOut("[AUTO-ACTION] Converting action type " + std::to_string(actionType) + 
           " for trigger type " + std::to_string(triggerType), true);
    LogOut("[AUTO-ACTION] Converted to motion type: " + std::to_string(motionType) + 
           " with button mask: " + std::to_string(buttonMask), true);

    // If buttonMask is zero, use default
    if (buttonMask == 0) {
        LogOut("[AUTO-ACTION] Using default button mask (A) for motion", true);
        buttonMask = 0x10; // Default to A button (16)
    }
    
    bool success = false;
    bool isRegularMove = (motionType >= MOTION_5A && motionType <= MOTION_JC);
    bool isSpecialMove = (motionType >= MOTION_236A);

    // Special handling: Jump must be injected via immediate input register with direction
    if (actionType == ACTION_JUMP) {
        // Determine jump direction: 0=neutral, 1=forward, 2=backward from strength slot
        int dir = 0;
        switch (triggerType) {
            case TRIGGER_AFTER_BLOCK: dir = triggerAfterBlockStrength.load(); break;
            case TRIGGER_ON_WAKEUP: dir = triggerOnWakeupStrength.load(); break;
            case TRIGGER_AFTER_HITSTUN: dir = triggerAfterHitstunStrength.load(); break;
            case TRIGGER_AFTER_AIRTECH: dir = triggerAfterAirtechStrength.load(); break;
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
    // Press for ~3 visual frames (64fps): enough to register jump, then auto-neutral
    ImmediateInput::PressFor(playerNum, mask, 3);
    // Clear cooldown so another trigger can occur soon after
    if (playerNum == 2) { p2TriggerActive = false; p2TriggerCooldown = 0; }
    else { p1TriggerActive = false; p1TriggerCooldown = 0; }
    LogOut("[AUTO-ACTION] Jump press scheduled for P" + std::to_string(playerNum), true);

        success = true;
        // Control restore watcher (only applicable for P2 overrides)
        if (playerNum == 2) {
            g_lastP2MoveID.store(currentMoveID);
            g_pendingControlRestore.store(true);
            g_controlRestoreTimeout.store(90);
        }
    }

    // Special handling: Dashes should be executed by writing to the buffer via the queue
    if (!success && (actionType == ACTION_BACKDASH || actionType == ACTION_FORWARD_DASH)) {
        LogOut("[AUTO-ACTION] Queuing dash motion via buffer (" + GetMotionTypeName(motionType) + ")", true);
        // Ensure immediate-only is disabled so buffer gets written
        g_injectImmediateOnly[playerNum].store(false);
        success = QueueMotionInput(playerNum, motionType, 0);
        // Control restore monitor (only P2)
        if (playerNum == 2) {
            g_lastP2MoveID.store(currentMoveID);
            g_pendingControlRestore.store(true);
            g_controlRestoreTimeout.store(90);
        }
    }
    
    if (!success && isRegularMove) {
        // For regular moves, use a simple manual-override hold; DO allow buffer writes (do not set immediate-only)
        LogOut("[AUTO-ACTION] Applying regular move " + GetMotionTypeName(motionType) +
               " via manual hold (buffered)", detailedLogging.load());

        // Build the mask for this normal
        uint8_t inputMask = GAME_INPUT_NEUTRAL;
        if (motionType >= MOTION_5A && motionType <= MOTION_5C) {
            inputMask = buttonMask; // Neutral + button
        } else if (motionType >= MOTION_2A && motionType <= MOTION_2C) {
            inputMask = GAME_INPUT_DOWN | buttonMask; // Down + button
        } else if (motionType >= MOTION_JA && motionType <= MOTION_JC) {
            inputMask = buttonMask; // Air normals: button only
        }

    // Use immediate writer for the button (A/B/C) and optionally direction DOWN for 2A/2B/2C.
    // Do not push to buffer here; motions and freezes remain buffer-driven.
    ImmediateInput::PressFor(playerNum, inputMask, 2); // ~2 visual ticks press

    // Allow immediate re-trigger after button press completes
    if (playerNum == 2) { p2TriggerActive = false; p2TriggerCooldown = 0; }
    else { p1TriggerActive = false; p1TriggerCooldown = 0; }

        success = true;
    }
    else if (!success && isSpecialMove) {
        // For special moves, use buffer freezing (this works better for motions)
        LogOut("[AUTO-ACTION] Applying special move " + GetMotionTypeName(motionType) + 
               " via buffer freeze", true);
        
        // Use the FreezeBufferForMotion function from input_freeze.cpp
        success = FreezeBufferForMotion(playerNum, motionType, buttonMask);
    }
    
    if (success) {
        if (isRegularMove) {
            LogOut("[AUTO-ACTION] Set up P2 control restoration for normal attack", true);
            if (playerNum == 2) {
                g_lastP2MoveID.store(currentMoveID);
                g_pendingControlRestore.store(true);
                g_controlRestoreTimeout.store(60); // Shorter timeout for normal attacks
            }
                // Keep trigger marked active while executing to prevent re-triggers
                p2TriggerActive = true;
                if (p2TriggerCooldown <= 1) p2TriggerCooldown = 1;
        } else {
            LogOut("[AUTO-ACTION] Set up P2 control restoration monitoring: initial moveID=" + 
                   std::to_string(currentMoveID), true);
            if (playerNum == 2) {
                g_lastP2MoveID.store(currentMoveID);
                g_pendingControlRestore.store(true);
                g_controlRestoreTimeout.store(180); // Longer timeout for special moves
            }
                p2TriggerActive = true;
                if (p2TriggerCooldown <= 1) p2TriggerCooldown = 1;
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
    
    // Always set to human control (0) regardless of our tracking variable
    uint32_t humanControlFlag = 0;
    if (SafeWriteMemory(p2CharPtr + AI_CONTROL_FLAG_OFFSET, &humanControlFlag, sizeof(uint32_t))) {
        // Double-check that it was actually written
        uint32_t verifyFlag = 1;
        if (SafeReadMemory(p2CharPtr + AI_CONTROL_FLAG_OFFSET, &verifyFlag, sizeof(uint32_t)) && verifyFlag == 0) {
            g_p2ControlOverridden = true;
            LogOut("[AUTO-ACTION] P2 control successfully set to human (0) for auto-action", true);
        } else {
            LogOut("[AUTO-ACTION] P2 control write failed verification, flag still = " + 
                  std::to_string(verifyFlag), true);
        }
    } else {
        LogOut("[AUTO-ACTION] Failed to write human control flag to P2", true);
    }
}

// Restore P2 to original control state
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
        
        uintptr_t p2CharPtr = 0;
        if (!SafeReadMemory(base + EFZ_BASE_OFFSET_P2, &p2CharPtr, sizeof(uintptr_t)) || !p2CharPtr) {
            LogOut("[AUTO-ACTION] Failed to get P2 pointer for control restore, marking as restored anyway", true);
            g_p2ControlOverridden = false; // Reset flag anyway to avoid getting stuck
            return;
        }
        
        // Restore original control state
        LogOut("[AUTO-ACTION] Restoring P2 control to original state: " + std::to_string(g_originalP2ControlFlag), true);
        if (SafeWriteMemory(p2CharPtr + AI_CONTROL_FLAG_OFFSET, &g_originalP2ControlFlag, sizeof(uint32_t))) {
            LogOut("[AUTO-ACTION] P2 control restored successfully", true);
        } else {
            LogOut("[AUTO-ACTION] Failed to write P2 control state for restore", true);
        }
        
        // Reset flag regardless of write success to avoid getting stuck
        g_p2ControlOverridden = false;
    }
}

// Add this function to auto_action.h
void ProcessAutoControlRestore() {
    if (!IsMatchPhase()) {
        if (g_pendingControlRestore.load()) {
            LogOut("[AUTO-ACTION] Phase left MATCH during restore; forcing cleanup", true);
            RestoreP2ControlState();
            g_pendingControlRestore.store(false);
        }
        return;
    }
    if (g_pendingControlRestore.load()) {
        // Cache address and throttle to every other frame to reduce CPU
        static uintptr_t moveIDAddr2 = 0;
        static int s_throttle = 0;
        if ((s_throttle++ & 1) != 0) return; // 96 Hz sampling is sufficient

        uintptr_t base = GetEFZBase();
        if (!base) return;
        if (!moveIDAddr2) moveIDAddr2 = ResolvePointer(base, EFZ_BASE_OFFSET_P2, MOVE_ID_OFFSET);

        short moveID2 = 0;
        if (moveIDAddr2) {
            SafeReadMemory(moveIDAddr2, &moveID2, sizeof(short));
        }

        int timeout = g_controlRestoreTimeout.fetch_sub(1);
        // Less frequent logging (once per second @192 fps => ~96 cycles after throttle)
        if ((timeout % 192) == 0) {
            LogOut("[AUTO-ACTION] Monitoring move execution: MoveID=" +
                   std::to_string(moveID2) + ", LastMoveID=" +
                   std::to_string(g_lastP2MoveID.load()) +
                   ", Timeout=" + std::to_string(timeout), detailedLogging.load());
        }

        static bool sawNonZeroMoveID = false;
        if (moveID2 > 0) {
            sawNonZeroMoveID = true;
        }

        bool moveChanged = (moveID2 != g_lastP2MoveID.load() && moveID2 == 0 && sawNonZeroMoveID);
        bool timeoutExpired = (timeout <= 0);

        if (moveChanged || timeoutExpired) {
            LogOut("[AUTO-ACTION] Auto-restoring P2 control state after move execution", detailedLogging.load());
            LogOut("[AUTO-ACTION] Reason: " +
                   std::string(moveChanged ? "Move completed" : "Timeout expired") +
                   ", MoveID: " + std::to_string(moveID2), detailedLogging.load());

            RestoreP2ControlState();
            g_pendingControlRestore.store(false);
            g_lastP2MoveID.store(-1);
            sawNonZeroMoveID = false;

            p2TriggerCooldown = 0; // allow prompt re-trigger after restoring
            p2TriggerActive = false;
            LogOut("[AUTO-ACTION] Cleared P2 trigger cooldown after restore", detailedLogging.load());
        } else {
            if (moveID2 != 0) {
                g_lastP2MoveID.store(moveID2);
            }
        }
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
    LogOut("[AUTO-ACTION] Forcing full clear of trigger/delay/cooldown state", true);

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

    // Ensure input buffer freeze (for special motions) is lifted
    StopBufferFreezing();

    LogOut("[AUTO-ACTION] All trigger states cleared", true);
}