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

static GameMode s_previousGameMode = GameMode::Unknown;
static bool s_wasActive = false;


bool IsValidGameMode(GameMode mode) {
    const Config::Settings& cfg = Config::GetSettings();
    return !cfg.restrictToPracticeMode || (mode == GameMode::Practice);
}

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
        
        // Update stats display regardless of whether other features are enabled
        UpdateStatsDisplay();
        
        // Feature management logic
        bool shouldBeActive = ShouldFeaturesBeActive();
        if (shouldBeActive && !g_featuresEnabled.load()) {
            EnableFeatures();
        } else if (!shouldBeActive && g_featuresEnabled.load()) {
            DisableFeatures();
        }
        
        // Track mode transitions and character initialization
        static bool wasInitialized = false;
        bool isInitialized = AreCharactersInitialized();
        GameMode currentMode = GetCurrentGameMode();
        bool isValidGameMode = !Config::GetSettings().restrictToPracticeMode || (currentMode == GameMode::Practice);

        // Detect when characters become initialized in a valid game mode
        if (g_featuresEnabled.load() && isValidGameMode && isInitialized && !wasInitialized) {
            LogOut("[FRAME MONITOR] Characters initialized in valid game mode, reinitializing overlays", true);
            ReinitializeOverlays();
        }

        // Track game mode transitions
        if (g_featuresEnabled.load() && currentMode != s_previousGameMode) {
            // Coming from another game mode to valid mode when characters are initialized
            if (isValidGameMode && isInitialized && 
                (s_previousGameMode != GameMode::Unknown && !IsValidGameMode(s_previousGameMode))) {
                LogOut("[FRAME MONITOR] Detected return to valid game mode with initialized characters, reinitializing overlays", true);
                ReinitializeOverlays();
            }
            s_previousGameMode = currentMode;
        }

        // Update initialization tracking
        wasInitialized = isInitialized;
        
        // Only run the main monitoring logic if features are enabled
        if (g_featuresEnabled.load()) {
            UpdateTriggerOverlay();
            UpdateStatsDisplay();
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

    // Set or update the display
    const int startX = 20;
    const int startY = 30;
    const int lineHeight = 20;
    COLORREF textColor = RGB(255, 255, 0); // Yellow

    // Create or update the permanent messages
    if (g_statsP1ValuesId == -1) {
        g_statsP1ValuesId = DirectDrawHook::AddPermanentMessage(p1Values.str(), textColor, startX, startY);
        g_statsP2ValuesId = DirectDrawHook::AddPermanentMessage(p2Values.str(), textColor, startX, startY + lineHeight);
        g_statsPositionId = DirectDrawHook::AddPermanentMessage(positions.str(), textColor, startX, startY + lineHeight * 2);
        g_statsMoveIdId = DirectDrawHook::AddPermanentMessage(moveIds.str(), textColor, startX, startY + lineHeight * 3);
        
        LogOut("[STATS] Created stats display messages", true);
    } else {
        DirectDrawHook::UpdatePermanentMessage(g_statsP1ValuesId, p1Values.str(), textColor);
        DirectDrawHook::UpdatePermanentMessage(g_statsP2ValuesId, p2Values.str(), textColor);
        DirectDrawHook::UpdatePermanentMessage(g_statsPositionId, positions.str(), textColor);
        DirectDrawHook::UpdatePermanentMessage(g_statsMoveIdId, moveIds.str(), textColor);
    }
}
