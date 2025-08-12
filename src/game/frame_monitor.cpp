#include "../include/game/frame_monitor.h"
#include "../include/game/auto_airtech.h"
#include "../include/game/auto_jump.h"
#include "../include/game/auto_action.h"
#include "../include/game/frame_analysis.h"
#include "../include/game/frame_advantage.h"
#include "../include/core/constants.h"
#include "../include/utils/utilities.h"
#include "../include/utils/bgm_control.h"
#include "../include/core/memory.h"
#include "../include/core/logger.h"
#include "../include/gui/overlay.h"
#include "../include/game/game_state.h"
#include "../include/input/input_buffer.h"
#include "../include/utils/config.h"
#include "../include/input/input_motion.h"
#include <deque>
#include <vector>
#include <chrono>
#include <sstream>
#include <algorithm>
#include <thread>
#include <atomic>
#include <iomanip>

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

static uintptr_t fm_lastP1Ptr = 0;
static uintptr_t fm_lastP2Ptr = 0;
static uintptr_t fm_lastMoveAddr1 = 0;
static uintptr_t fm_lastMoveAddr2 = 0;
static bool      fm_lastCharsInit = false;
static int       fm_moveReadFailStreak1 = 0;
static int       fm_moveReadFailStreak2 = 0;
static int       fm_overrunWarnCounter = 0;
static GamePhase fm_lastLoggedPhase = GamePhase::Unknown;
static int       fm_lastLoggedFrame = 0;

static std::string FM_Hex(uintptr_t v) {
    std::ostringstream oss;
    oss << "0x" << std::hex << std::uppercase << v;
    return oss.str();
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
        uintptr_t p1StructAddr = 0;
        SafeReadMemory(base + EFZ_BASE_OFFSET_P1, &p1StructAddr, sizeof(uintptr_t));
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
            actionType == ACTION_214214) {
            
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
            case ACTION_QCF: return "236" + strengthLetter; // QCF + strength
            case ACTION_DP: return "623" + strengthLetter;  // DP + strength
            case ACTION_QCB: return "214" + strengthLetter; // QCB + strength
            case ACTION_421: return "421" + strengthLetter; // Half-circle down + strength
            case ACTION_SUPER1: return "41236" + strengthLetter; // HCF + strength
            case ACTION_SUPER2: return "63214" + strengthLetter; // HCB + strength
            case ACTION_236236: return "236236" + strengthLetter; // Double QCF + strength
            case ACTION_214214: return "214214" + strengthLetter; // Double QCB + strength
            case ACTION_JUMP: return "Jump";
            case ACTION_BACKDASH: return "Backdash";
            case ACTION_FORWARD_DASH: return "Forward Dash";
            case ACTION_BLOCK: return "Block";
            case ACTION_CUSTOM: return "Custom (" + std::to_string(customId) + ")";
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
            std::string actionName = getActionName(action, customId, strength);
            std::string text = label + actionName;
            if (delay > 0) {
                text += " +" + std::to_string(delay);
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
}

void FrameDataMonitor() {
    using clock = std::chrono::high_resolution_clock;
    
    LogOut("[FRAME MONITOR] Starting frame monitoring at 192fps for maximum precision", true);
    
    // CRITICAL: Set highest possible priority to prevent throttling
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    
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

    
    while (!g_isShuttingDown) {
        auto frameStart = clock::now();
        
        // Check current game phase
        GamePhase currentPhase = GetCurrentGamePhase();
        
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
        
        // Track phase changes
        static GamePhase lastPhase = GamePhase::Unknown;
        if (currentPhase != lastPhase) {
            LogOut("[FRAME MONITOR] Phase changed: " + std::to_string((int)lastPhase) + 
                   " -> " + std::to_string((int)currentPhase), true);
            
            // On ANY phase change away from Match, ensure cleanup
            if (lastPhase == GamePhase::Match && currentPhase != GamePhase::Match) {
                LogOut("[FRAME MONITOR] Exiting Match phase - performing full cleanup", true);
                
                // Force stop everything
                StopBufferFreezing();
                ResetActionFlags();
                
                // Clear all auto-action states
                p1DelayState = {false, 0, TRIGGER_NONE, 0};
                p2DelayState = {false, 0, TRIGGER_NONE, 0};
                p1ActionApplied = false;
                p2ActionApplied = false;
                
                // Restore P2 control
                if (g_p2ControlOverridden) {
                    RestoreP2ControlState();
                    g_p2ControlOverridden = false;
                }
            }
            
            lastPhase = currentPhase;
        }
        
        // SINGLE authoritative frame increment
        int currentFrame = frameCounter.fetch_add(1) + 1;

        // Process any active input queues
        ProcessInputQueues();

        UpdateWindowActiveState();
        UpdateStatsDisplay();

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
            }
            
            // Existing code for transitions TO valid modes
            if (isValidGameMode && isInitialized && 
                (s_previousGameMode != GameMode::Unknown && !IsValidGameMode(s_previousGameMode))) {
                LogOut("[FRAME MONITOR] Detected return to valid game mode with initialized characters, reinitializing overlays", true);
                ReinitializeOverlays();
            }
            
            s_previousGameMode = currentMode;
        }

        // Update initialization tracking
        //wasInitialized = isInitialized;
        
        // Only run the main monitoring logic if features are enabled
        if (g_featuresEnabled.load()) {
            UpdateTriggerOverlay();
            UpdateStatsDisplay();
            uintptr_t base = GetEFZBase();
            if (!base) {
                goto FRAME_MONITOR_FRAME_END;
            }

            // Pointer change logging
            uintptr_t p1Ptr = 0, p2Ptr = 0;
            SafeReadMemory(base + EFZ_BASE_OFFSET_P1, &p1Ptr, sizeof(p1Ptr));
            SafeReadMemory(base + EFZ_BASE_OFFSET_P2, &p2Ptr, sizeof(p2Ptr));
            if ((p1Ptr != fm_lastP1Ptr || p2Ptr != fm_lastP2Ptr) && (p1Ptr || p2Ptr)) {
                LogOut("[FRAME MONITOR][PTR] P1 " + FM_Hex(p1Ptr) + " (was " + FM_Hex(fm_lastP1Ptr) + ")  P2 " +
                       FM_Hex(p2Ptr) + " (was " + FM_Hex(fm_lastP2Ptr) + ")", true);
                fm_lastP1Ptr = p1Ptr;
                fm_lastP2Ptr = p2Ptr;
            }

            // Initialization transition logging
            if (isInitialized != fm_lastCharsInit) {
                LogOut(std::string("[FRAME MONITOR][INIT] CharactersInitialized: ") +
                       (fm_lastCharsInit ? "true" : "false") + " -> " +
                       (isInitialized ? "true" : "false") +
                       " frame=" + std::to_string(currentFrame), true);
                fm_lastCharsInit = isInitialized;
            }

            // Phase gating
            GamePhase phase = GetCurrentGamePhase();
            if (phase != fm_lastLoggedPhase) {
                LogOut("[FRAME MONITOR][PHASE] " + std::to_string((int)fm_lastLoggedPhase) + " -> " +
                       std::to_string((int)phase) + " frame=" + std::to_string(currentFrame), true);
                fm_lastLoggedPhase = phase;
            }

            // Handle enter/leave Match once
            static GamePhase s_lastPhaseLocal = GamePhase::Unknown;
            if (phase != s_lastPhaseLocal) {
                if (s_lastPhaseLocal == GamePhase::Match && phase != GamePhase::Match) {
                    LogOut("[FRAME MONITOR] Leaving MATCH phase -> cleanup", true);
                    StopBufferFreezing();
                    ResetActionFlags();
                    p1DelayState.isDelaying = false;
                    p2DelayState.isDelaying = false;
                    p1DelayState.triggerType = TRIGGER_NONE;
                    p2DelayState.triggerType = TRIGGER_NONE;
                    if (g_pendingControlRestore.load()) {
                        LogOut("[CONTROL] Aborting pending control restore (phase exit)", true);
                        g_pendingControlRestore.store(false);
                    }
                    g_lastP2MoveID.store(-1);
                }
                if (phase == GamePhase::Match && s_lastPhaseLocal != GamePhase::Match) {
                    LogOut("[FRAME MONITOR] Entering MATCH phase -> reinit transient state", true);
                    prevMoveID1 = -1;
                    prevMoveID2 = -1;
                }
                s_lastPhaseLocal = phase;
            }

            // Lightweight ticking (cooldowns) always runs
            auto lightweightTick = []() {
                ProcessTriggerCooldowns(); // make sure cooldowns advance outside Match
            };

            bool skipHeavy = false;

            // Outside actual gameplay -> only do lightweight logic
            if (phase != GamePhase::Match) {
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
            // Refresh addresses periodically
            if (addressCacheCounter++ >= 192) {
                cachedMoveIDAddr1 = ResolvePointer(base, EFZ_BASE_OFFSET_P1, MOVE_ID_OFFSET);
                cachedMoveIDAddr2 = ResolvePointer(base, EFZ_BASE_OFFSET_P2, MOVE_ID_OFFSET);
                addressCacheCounter = 0;
                
                // Log timing performance every second
                auto currentTime = clock::now();
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(currentTime - lastLogTime);
                if (elapsed.count() >= 1000) {
                    double actualFPS = framesSinceLastLog / (elapsed.count() / 1000.0);
                    
                    // Only log if fps is significantly off target or detailed logging is on
                    if (detailedLogging.load() || fabs(actualFPS - 192.0) > 5.0) {
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
            
            // CRITICAL: Increment frame counter IMMEDIATELY for precise tracking
            framesSinceLastLog++;
            
            // Process features in order of priority - NO THROTTLING
            bool moveIDsChanged = (moveID1 != prevMoveID1) || (moveID2 != prevMoveID2);
            bool criticalFeaturesActive = autoJumpEnabled.load() || autoActionEnabled.load() || autoAirtechEnabled.load();

            // ALWAYS process frame advantage for precise timing
            if (moveIDsChanged) {
                MonitorFrameAdvantage(moveID1, moveID2, prevMoveID1, prevMoveID2);
            }
            
            if (moveIDsChanged || criticalFeaturesActive) {
                // STEP 1: Process auto-actions FIRST (highest priority)
                ProcessTriggerDelays();      // Handle pending delays
                MonitorAutoActions();        // Check for new triggers
                
                // STEP 2: Auto-jump logic with conflict detection
                bool autoActionBusy = false;
                
                if (autoActionEnabled.load()) {
                    int targetPlayer = autoActionPlayer.load();
                    
                    // Check if ANY trigger is enabled and could potentially activate
                    bool anyTriggerEnabled = triggerAfterBlockEnabled.load() || 
                                           triggerOnWakeupEnabled.load() || 
                                           triggerAfterHitstunEnabled.load() || 
                                           triggerAfterAirtechEnabled.load();
                    
                    if (anyTriggerEnabled) {
                        // If auto-action is enabled with triggers, check for activity
                        if (targetPlayer == 1 || targetPlayer == 3) {
                            autoActionBusy = autoActionBusy || p1DelayState.isDelaying || p1ActionApplied;
                        }
                        if (targetPlayer == 2 || targetPlayer == 3) {
                            autoActionBusy = autoActionBusy || p2DelayState.isDelaying || p2ActionApplied;
                        }
                    }
                }
                
                // Auto-jump with conflict detection
                if (!autoActionBusy && autoJumpEnabled.load()) {
                    // Check if moveIDs indicate recent auto-action activity
                    bool hasAttackMoves = (moveID1 >= 200 && moveID1 <= 350) ||
                                         (moveID2 >= 200 && moveID2 <= 350);
                    
                    if (!hasAttackMoves) {
                        MonitorAutoJump();
                    }
                }
                
                // STEP 3: Auto-airtech (every frame for precision, no throttling)
                MonitorAutoAirtech(moveID1, moveID2);  
                ClearDelayStatesIfNonActionable();     
            }
            
            prevMoveID1 = moveID1;
            prevMoveID2 = moveID2;
        }
        
FRAME_MONITOR_FRAME_END:
        // Sleep precisely for the remaining time to maintain exact 192fps
        auto frameEnd = clock::now();
        auto frameDuration = frameEnd - frameStart;
        auto sleepTime = targetFrameTime - frameDuration;
        
        // Replace this code:
        // if (sleepTime > std::chrono::nanoseconds::zero()) {
        //     std::this_thread::sleep_for(sleepTime);
        // }

        // With this spin-wait hybrid approach:
        if (sleepTime > std::chrono::milliseconds(1)) {
            // Sleep for most of the time
            std::this_thread::sleep_for(sleepTime - std::chrono::milliseconds(1));
            
            // Spin-wait for the remaining sub-millisecond precision
            auto spinStart = std::chrono::high_resolution_clock::now();
            while (std::chrono::high_resolution_clock::now() - spinStart < std::chrono::milliseconds(1)) {
                // Yield to allow other threads same priority to run but keep spinning
                std::this_thread::yield();
            }
        } else if (sleepTime > std::chrono::nanoseconds::zero()) {
            // For very short sleeps, just spin
            auto spinStart = std::chrono::high_resolution_clock::now();
            while (std::chrono::high_resolution_clock::now() - spinStart < sleepTime) {
                std::this_thread::yield();
            }
        }
    }
    
    LogOut("[FRAME MONITOR] Shutting down frame monitor thread", true);
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
    
    // Reset frame advantage display
    if (g_FrameAdvantageId != -1) {
        DirectDrawHook::RemovePermanentMessage(g_FrameAdvantageId);
        g_FrameAdvantageId = -1;
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
            g_statsP1ValuesId = -1;
            g_statsP2ValuesId = -1;
            g_statsPositionId = -1;
            g_statsMoveIdId = -1;
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
    // Return early if stats display is disabled or DirectDraw hook isn't initialized
    if (!g_statsDisplayEnabled.load() || !DirectDrawHook::isHooked) {
        // Clear existing messages if display is disabled
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
        return;
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
        return;
    }

    // Read current game values
    uintptr_t base = GetEFZBase();
    if (!base) return;

    // Cache memory addresses for efficiency
    static uintptr_t p1HpAddr = 0, p1MeterAddr = 0, p1RfAddr = 0;
    static uintptr_t p2HpAddr = 0, p2MeterAddr = 0, p2RfAddr = 0;
    static uintptr_t p1XAddr = 0, p1YAddr = 0, p2XAddr = 0, p2YAddr = 0;
    static uintptr_t p1MoveIdAddr = 0, p2MoveIdAddr = 0;
    static int cacheCounter = 0;

    // Refresh cache occasionally
    if (cacheCounter++ >= 60 || !p1HpAddr) {
        p1HpAddr = ResolvePointer(base, EFZ_BASE_OFFSET_P1, HP_OFFSET);
        p1MeterAddr = ResolvePointer(base, EFZ_BASE_OFFSET_P1, METER_OFFSET);
        p1RfAddr = ResolvePointer(base, EFZ_BASE_OFFSET_P1, RF_OFFSET);
        p1XAddr = ResolvePointer(base, EFZ_BASE_OFFSET_P1, XPOS_OFFSET);
        p1YAddr = ResolvePointer(base, EFZ_BASE_OFFSET_P1, YPOS_OFFSET);
        p1MoveIdAddr = ResolvePointer(base, EFZ_BASE_OFFSET_P1, MOVE_ID_OFFSET);

        p2HpAddr = ResolvePointer(base, EFZ_BASE_OFFSET_P2, HP_OFFSET);
        p2MeterAddr = ResolvePointer(base, EFZ_BASE_OFFSET_P2, METER_OFFSET);
        p2RfAddr = ResolvePointer(base, EFZ_BASE_OFFSET_P2, RF_OFFSET);
        p2XAddr = ResolvePointer(base, EFZ_BASE_OFFSET_P2, XPOS_OFFSET);
        p2YAddr = ResolvePointer(base, EFZ_BASE_OFFSET_P2, YPOS_OFFSET);
        p2MoveIdAddr = ResolvePointer(base, EFZ_BASE_OFFSET_P2, MOVE_ID_OFFSET);
        
        cacheCounter = 0;
    }

    // Read values
    int p1Hp = 0, p1Meter = 0;
    double p1Rf = 0;
    if (p1HpAddr) SafeReadMemory(p1HpAddr, &p1Hp, sizeof(int));
    if (p1MeterAddr) SafeReadMemory(p1MeterAddr, &p1Meter, sizeof(int));
    if (p1RfAddr) SafeReadMemory(p1RfAddr, &p1Rf, sizeof(double));

    int p2Hp = 0, p2Meter = 0;
    double p2Rf = 0;
    if (p2HpAddr) SafeReadMemory(p2HpAddr, &p2Hp, sizeof(int));
    if (p2MeterAddr) SafeReadMemory(p2MeterAddr, &p2Meter, sizeof(int));
    if (p2RfAddr) SafeReadMemory(p2RfAddr, &p2Rf, sizeof(double));

    double p1X = 0, p1Y = 0, p2X = 0, p2Y = 0;
    if (p1XAddr) SafeReadMemory(p1XAddr, &p1X, sizeof(double));
    if (p1YAddr) SafeReadMemory(p1YAddr, &p1Y, sizeof(double));
    if (p2XAddr) SafeReadMemory(p2XAddr, &p2X, sizeof(double));
    if (p2YAddr) SafeReadMemory(p2YAddr, &p2Y, sizeof(double));

    short p1MoveId = 0, p2MoveId = 0;
    if (p1MoveIdAddr) SafeReadMemory(p1MoveIdAddr, &p1MoveId, sizeof(short));
    if (p2MoveIdAddr) SafeReadMemory(p2MoveIdAddr, &p2MoveId, sizeof(short));

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
    
    // Move IDs
    moveIds << "MoveID:  P1: " << p1MoveId << "  P2: " << p2MoveId;

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

    upsert(g_statsP1ValuesId, p1Values.str());
    upsert(g_statsP2ValuesId, p2Values.str());
    upsert(g_statsPositionId, positions.str());
    upsert(g_statsMoveIdId, moveIds.str());

    return;
}
