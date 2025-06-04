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
    
    LogOut("[FRAME MONITOR] Starting frame monitoring with visual frame tracking", true);
    
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    
    short prevMoveID1 = -1, prevMoveID2 = -1;
    
    // CRITICAL: Static timing that NEVER changes regardless of window focus
    const auto targetFrameTime = std::chrono::microseconds(5208); // Exactly 192 Hz
    
    // Cache addresses to reduce ResolvePointer calls
    static uintptr_t cachedMoveIDAddr1 = 0;
    static uintptr_t cachedMoveIDAddr2 = 0;
    static int addressCacheCounter = 0;
    
    while (true) {
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
        }
        
        // Read move IDs using cached addresses
        short moveID1 = 0, moveID2 = 0;
        if (cachedMoveIDAddr1) SafeReadMemory(cachedMoveIDAddr1, &moveID1, sizeof(short));
        if (cachedMoveIDAddr2) SafeReadMemory(cachedMoveIDAddr2, &moveID2, sizeof(short));
        
        frameCounter.fetch_add(1);
        
        // Process features in order of priority
        bool moveIDsChanged = (moveID1 != prevMoveID1) || (moveID2 != prevMoveID2);
        bool criticalFeaturesActive = autoJumpEnabled.load() || autoActionEnabled.load() || autoAirtechEnabled.load();
        
        if (moveIDsChanged || criticalFeaturesActive) {
            // STEP 1: Process auto-actions FIRST (highest priority)
            ProcessTriggerDelays();      // Handle pending delays
            MonitorAutoActions();        // Check for new triggers
            
            // STEP 2: CRITICAL FIX - Check for auto-action activity MORE THOROUGHLY
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
            
            // CRITICAL: Also check if auto-jump would conflict with auto-action moveIDs
            if (!autoActionBusy && autoJumpEnabled.load()) {
                // Read current moveIDs to see if they were just set by auto-action
                short currentMoveID1 = 0, currentMoveID2 = 0;
                if (cachedMoveIDAddr1) SafeReadMemory(cachedMoveIDAddr1, &currentMoveID1, sizeof(short));
                if (cachedMoveIDAddr2) SafeReadMemory(cachedMoveIDAddr2, &currentMoveID2, sizeof(short));
                
                // Don't run auto-jump if moveIDs are attack moves (likely from auto-action)
                bool hasAttackMoves = (currentMoveID1 >= 200 && currentMoveID1 <= 350) ||
                                     (currentMoveID2 >= 200 && currentMoveID2 <= 350);
                
                if (!hasAttackMoves) {
                    MonitorAutoJump();
                } else {
                    LogOut("[FRAME MONITOR] Skipping auto-jump - attack moves detected", detailedLogging.load());
                }
            } else if (autoActionBusy) {
                LogOut("[FRAME MONITOR] Skipping auto-jump - auto-action is busy", detailedLogging.load());
            }
            
            // STEP 3: Other systems (lowest priority)
            if ((frameCounter.load() % 3) == 0) {
                MonitorAutoAirtech(moveID1, moveID2);  
                ClearDelayStatesIfNonActionable();     
            }
            
            // Frame advantage monitoring
            if (moveIDsChanged) {
                MonitorFrameAdvantage(moveID1, moveID2, prevMoveID1, prevMoveID2);
            }
        }
        
        prevMoveID1 = moveID1;
        prevMoveID2 = moveID2;
        
        maintain_timing:
        // Always maintain exact timing regardless of what happened above
        auto frameEndTime = clock::now();
        auto frameDuration = frameEndTime - frameStartTime;
        
        if (frameDuration < targetFrameTime) {
            auto sleepTime = targetFrameTime - frameDuration;
            
            if (sleepTime < std::chrono::microseconds(500)) {
                while (clock::now() - frameStartTime < targetFrameTime) {
                    std::this_thread::yield();
                }
            } else {
                std::this_thread::sleep_for(sleepTime);
            }
        }
    }
}