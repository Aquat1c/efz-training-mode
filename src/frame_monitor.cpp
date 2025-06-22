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
#include <deque>
#include <vector>
#include <chrono>
#include <sstream>
#include <algorithm>
#include <thread>
#include <atomic>
#include <iomanip>

MonitorState state = Idle;

void FrameDataMonitor() {
    using clock = std::chrono::high_resolution_clock;
    
    LogOut("[FRAME MONITOR] Starting frame monitoring with PRECISE subframe tracking", true);
    
    // CRITICAL: Set highest possible priority to prevent throttling
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    
    short prevMoveID1 = -1, prevMoveID2 = -1;
    
    // CRITICAL: Ultra-precise timing that NEVER changes (exactly 192 Hz)
    const auto targetFrameTime = std::chrono::nanoseconds(5208333); // 1,000,000,000 / 192
    
    // Cache addresses to reduce ResolvePointer calls
    static uintptr_t cachedMoveIDAddr1 = 0;
    static uintptr_t cachedMoveIDAddr2 = 0;
    static int addressCacheCounter = 0;
    
    // Performance tracking
    auto lastLogTime = clock::now();
    int framesSinceLastLog = 0;
    
    // Add reference to the shutdown flag
    extern std::atomic<bool> g_isShuttingDown;
    
    while (!g_isShuttingDown) {  // Check flag in loop condition
        auto frameStartTime = clock::now();
        
        uintptr_t base = GetEFZBase();
        if (base == 0) {
            goto maintain_timing;
        }
        
        // Refresh address cache every 192 frames (exactly 1 second at 192Hz)
        if (addressCacheCounter++ >= 192) {
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
        
        maintain_timing:
        // CRITICAL: Always maintain EXACT timing regardless of processing load
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