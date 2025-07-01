#include "../include/frame_monitor.h"
#include "../include/auto_airtech.h"
#include "../include/auto_jump.h"
#include "../include/auto_action.h"
#include "../include/frame_analysis.h"
#include "../include/frame_advantage.h"
#include "../include/constants.h"
#include "../include/utilities.h"
#include "../include/memory.h"
#include "../include/logger.h"
#include "../include/overlay.h"
#include "../include/game_state.h"
#include "../include/config.h"
#include <deque>
#include <vector>
#include <chrono>
#include <sstream>
#include <algorithm>
#include <thread>
#include <atomic>
#include <iomanip>

MonitorState state = Idle;

// REVISED: This function now ONLY determines if features should be active.
// It no longer performs resets itself. That is handled by DisableFeatures.
bool ShouldFeaturesBeActive() {
    // First check if EFZ window is active
    UpdateWindowActiveState();
    if (!g_efzWindowActive.load()) {
        return false;
    }
    
    // Then do the existing checks
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

    // If restriction is off, features are active.
    // If restriction is on, features are only active in Practice mode.
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

    auto getActionName = [](int actionType, int customId) -> std::string {
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
            case ACTION_JUMP: return "Jump";
            case ACTION_BACKDASH: return "Backdash";
            case ACTION_BLOCK: return "Block";
            case ACTION_CUSTOM: return "MoveID " + std::to_string(customId);
            default: return "Unknown";
        }
    };

    auto update_line = [&](int& msgId, bool isEnabled, const std::string& label, int action, int customId, int delay, int triggerType) {
        if (isEnabled) {
            std::string actionName = getActionName(action, customId);
            std::string text = label + actionName;
            if (delay > 0) {
                text += " (" + std::to_string(delay) + "f)";
            }

            bool isActive = false;
            if (g_lastActiveTriggerType.load() == triggerType) {
                // Highlight for 96 internal frames (~0.5 seconds) after activation
                if (frameCounter.load() - g_lastActiveTriggerFrame.load() < 96) {
                    isActive = true;
                }
            }

            COLORREF color = isActive ? RGB(50, 255, 50) : RGB(255, 215, 0);

            if (msgId == -1) {
                // Moved to the right side of the screen
                msgId = DirectDrawHook::AddPermanentMessage(text, color, 510, yPos);
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

    update_line(g_TriggerAfterBlockId, triggerAfterBlockEnabled.load(), "After Block: ", triggerAfterBlockAction.load(), triggerAfterBlockCustomID.load(), triggerAfterBlockDelay.load(), TRIGGER_AFTER_BLOCK);
    update_line(g_TriggerOnWakeupId, triggerOnWakeupEnabled.load(), "On Wakeup: ", triggerOnWakeupAction.load(), triggerOnWakeupCustomID.load(), triggerOnWakeupDelay.load(), TRIGGER_ON_WAKEUP);
    update_line(g_TriggerAfterHitstunId, triggerAfterHitstunEnabled.load(), "After Hitstun: ", triggerAfterHitstunAction.load(), triggerAfterHitstunCustomID.load(), triggerAfterHitstunDelay.load(), TRIGGER_AFTER_HITSTUN);
    update_line(g_TriggerAfterAirtechId, triggerAfterAirtechEnabled.load(), "After Airtech: ", triggerAfterAirtechAction.load(), triggerAfterAirtechCustomID.load(), triggerAfterAirtechDelay.load(), TRIGGER_AFTER_AIRTECH);
}

void FrameDataMonitor() {
    using clock = std::chrono::high_resolution_clock;
    
    LogOut("[FRAME MONITOR] Starting frame monitoring with PRECISE subframe tracking", true);
    
    // CRITICAL: Set highest possible priority to prevent throttling
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    
    short prevMoveID1 = -1, prevMoveID2 = -1;
    const auto targetFrameTime = std::chrono::nanoseconds(5208333);
    static uintptr_t cachedMoveIDAddr1 = 0;
    static uintptr_t cachedMoveIDAddr2 = 0;
    static int addressCacheCounter = 0;
    auto lastLogTime = clock::now();
    int framesSinceLastLog = 0;
    extern std::atomic<bool> g_isShuttingDown;
    
    while (!g_isShuttingDown) {
        auto frameStartTime = clock::now();
        
        // Update window state at the beginning of each frame
        UpdateWindowActiveState();
        
        // Feature management logic
        bool shouldBeActive = ShouldFeaturesBeActive();
        if (shouldBeActive && !g_featuresEnabled.load()) {
            EnableFeatures();
        } else if (!shouldBeActive && g_featuresEnabled.load()) {
            DisableFeatures();
        }
        
        // Only run the main monitoring logic if features are enabled
        if (g_featuresEnabled.load()) {
            UpdateTriggerOverlay();

            uintptr_t base = GetEFZBase();
            if (!base) {
                continue;
            }
            
            // Refresh addresses periodically
            if (addressCacheCounter++ >= 192) { // Refresh every second
                cachedMoveIDAddr1 = ResolvePointer(base, EFZ_BASE_OFFSET_P1, MOVE_ID_OFFSET);
                cachedMoveIDAddr2 = ResolvePointer(base, EFZ_BASE_OFFSET_P2, MOVE_ID_OFFSET);
                addressCacheCounter = 0;
                
                // Log timing performance every second
                auto currentTime = clock::now();
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(currentTime - lastLogTime);
                if (elapsed.count() >= 1000) {
                    double actualFPS = framesSinceLastLog / (elapsed.count() / 1000.0);
                    LogOut("[FRAME MONITOR] Actual FPS: " + std::to_string(actualFPS) + 
                           " (target: 192.0)", detailedLogging.load());
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
            int currentFrame = frameCounter.fetch_add(1) + 1;
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
        
        // Ensure the loop runs at the target rate
        auto frameEndTime = clock::now();
        auto frameDuration = frameEndTime - frameStartTime;
        
        if (frameDuration < targetFrameTime) {
            auto sleepTime = targetFrameTime - frameDuration;
            
            // Use high-precision spinning for very short waits
            if (sleepTime < std::chrono::microseconds(100)) {
                while (clock::now() - frameStartTime < targetFrameTime) {
                    _mm_pause(); // Hint to CPU for spin-wait loop
                }
            } else {
                // Use sleep for longer waits
                std::this_thread::sleep_for(sleepTime);
            }
        } else {
            // Log timing overruns for debugging
            if (detailedLogging.load()) {
                auto overrun = std::chrono::duration_cast<std::chrono::microseconds>(frameDuration - targetFrameTime);
                if (overrun.count() > 1000) { // Only log significant overruns
                    LogOut("[FRAME MONITOR] Timing overrun: " + std::to_string(overrun.count()) + "µs", false);
                }
            }
        }
    }
    
    LogOut("[FRAME MONITOR] Shutting down frame monitor thread", true);
}