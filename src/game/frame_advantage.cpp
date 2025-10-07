#include "../include/game/frame_advantage.h"
#include "../include/core/constants.h"
#include "../include/utils/utilities.h"

#include "../include/core/memory.h"
#include "../include/core/logger.h"
#include "../include/game/frame_analysis.h"
#include "../include/game/frame_monitor.h"
#include "../include/gui/overlay.h"
#include "../include/utils/pause_integration.h"

// Global state for frame advantage tracking
FrameAdvantageState frameAdvState = {
    false, false, false, false,    // p1InBlockstun, p2InBlockstun, p1InHitstun, p2InHitstun
    false, false,                  // p1Defending, p2Defending
    false, false,                  // p1Attacking, p2Attacking
    -1, -1,                        // p1AttackStartInternalFrame, p2AttackStartInternalFrame
    0, 0,                          // p1AttackMoveID, p2AttackMoveID
    -1, -1,                        // p1BlockstunStartInternalFrame, p2BlockstunStartInternalFrame
    -1, -1,                        // p1HitstunStartInternalFrame, p2HitstunStartInternalFrame
    -1, -1,                        // p1ActionableInternalFrame, p2ActionableInternalFrame
    -1, -1,                        // p1DefenderFreeInternalFrame, p2DefenderFreeInternalFrame
    0.0, 0.0,                      // p1FrameAdvantage, p2FrameAdvantage
    false, false,                  // p1AdvantageCalculated, p2AdvantageCalculated
    0, 0,                          // p1InitialBlockstunMoveID, p2InitialBlockstunMoveID
    -1                             // displayUntilInternalFrame
};

// Suppress regular FA overlay updates until this internal frame (0 = off)
std::atomic<int> g_SkipRegularFAOverlayUntilFrame{0};

void ResetFrameAdvantageState() {
    frameAdvState.p1InBlockstun = false;
    frameAdvState.p2InBlockstun = false;
    frameAdvState.p1InHitstun = false;
    frameAdvState.p2InHitstun = false;
    frameAdvState.p1Defending = false;
    frameAdvState.p2Defending = false;
    frameAdvState.p1Attacking = false;
    frameAdvState.p2Attacking = false;
    frameAdvState.p1AttackStartInternalFrame = -1;
    frameAdvState.p2AttackStartInternalFrame = -1;
    frameAdvState.p1AttackMoveID = 0;
    frameAdvState.p2AttackMoveID = 0;
    frameAdvState.p1BlockstunStartInternalFrame = -1;
    frameAdvState.p2BlockstunStartInternalFrame = -1;
    frameAdvState.p1HitstunStartInternalFrame = -1;
    frameAdvState.p2HitstunStartInternalFrame = -1;
    frameAdvState.p1ActionableInternalFrame = -1;
    frameAdvState.p2ActionableInternalFrame = -1;
    frameAdvState.p1DefenderFreeInternalFrame = -1;
    frameAdvState.p2DefenderFreeInternalFrame = -1;
    frameAdvState.p1FrameAdvantage = 0.0;
    frameAdvState.p2FrameAdvantage = 0.0;
    frameAdvState.p1AdvantageCalculated = false;
    frameAdvState.p2AdvantageCalculated = false;
    frameAdvState.p1InitialBlockstunMoveID = 0;
    frameAdvState.p2InitialBlockstunMoveID = 0;
    frameAdvState.displayUntilInternalFrame = -1;
    
    if (detailedLogging.load()) {
    #if defined(ENABLE_FRAME_ADV_DEBUG)
    LogOut("[FRAME_ADV_DEBUG] Frame advantage state reset", false);
    #endif
    }

    // Clear any existing display
    if (g_FrameAdvantageId != -1) {
        DirectDrawHook::RemovePermanentMessage(g_FrameAdvantageId);
        g_FrameAdvantageId = -1;
    }
    if (g_FrameAdvantage2Id != -1) {
        DirectDrawHook::RemovePermanentMessage(g_FrameAdvantage2Id);
        g_FrameAdvantage2Id = -1;
    }
}

int GetCurrentInternalFrame() {
    // Pause-aware internal frame counter for FA timings:
    // - When not paused (or not in Practice), mirror the global frameCounter.
    // - When paused in Practice, advance only when the Practice step counter increments
    //   (each step is one visual frame = 3 internal frames).
    static bool s_initialized = false;
    static int s_faInternal = 0;
    static bool s_prevPaused = false;
    static uint32_t s_lastStep = 0;

    int fcNow = frameCounter.load();
    GameMode mode = GetCurrentGameMode();
    bool inPractice = (mode == GameMode::Practice);
    bool paused = PauseIntegration::IsPracticePaused();

    if (!s_initialized) {
        s_faInternal = fcNow;
        s_initialized = true;
        s_prevPaused = paused;
        s_lastStep = 0;
        // Try to seed last step value if available
        uint32_t seed = 0; if (PauseIntegration::ReadStepCounter(seed)) s_lastStep = seed;
        return s_faInternal;
    }

    if (!inPractice || !paused) {
        // Unpaused or not in Practice: follow real time and keep in sync
        s_faInternal = fcNow;
        // Reset step debouncer on transition out of pause
        if (s_prevPaused && !paused) {
            s_lastStep = 0; uint32_t seed = 0; if (PauseIntegration::ReadStepCounter(seed)) s_lastStep = seed;
        }
        s_prevPaused = paused;
        return s_faInternal;
    }

    // Paused in Practice: advance on step increments only
    uint32_t cur = 0;
    if (PauseIntegration::ReadStepCounter(cur)) {
        if (s_lastStep == 0) {
            s_lastStep = cur;
        } else if (cur != s_lastStep) {
            uint32_t delta = cur - s_lastStep; // handles wrap naturally
            if (delta > 1000u) delta = 1u;     // sanity clamp
            // Each step = 1 visual frame = 3 internal frames
            s_faInternal += static_cast<int>(delta) * 3;
            s_lastStep = cur;
        }
    }
    s_prevPaused = paused;
    return s_faInternal;
}

double GetCurrentVisualFrame() {
    int internalFrame = frameCounter.load();
    
    // Convert internal frame to visual frame with subframe precision
    // Each visual frame = 3 internal frames
    // Subframe 0 = .00, subframe 1 = .33, subframe 2 = .66
    int visualFrameWhole = internalFrame / 3;
    int subframe = internalFrame % 3;
    
    double subframeDecimal = 0.0;
    switch (subframe) {
        case 0: subframeDecimal = 0.0; break;   // First subframe (.00)
        case 1: subframeDecimal = 0.33; break;  // Second subframe (.33)
        case 2: subframeDecimal = 0.66; break;  // Third subframe (.66)
    }
    
    return visualFrameWhole + subframeDecimal;
}

// Fix the FormatFrameAdvantage function to handle signs properly
std::string FormatFrameAdvantage(int advantageInternal) {
    // Convert internal frames to visual frames with subframe precision
    int visualFrames = advantageInternal / 3;
    int subframes = std::abs(advantageInternal % 3);

    // Only add the sign for positive values - this prevents double + signs later
    std::string sign = (advantageInternal >= 0) ? "+" : "";
    std::string subframeStr = "";
    
    if (subframes == 1) {
        subframeStr = ".33";
    } else if (subframes == 2) {
        subframeStr = ".66";
    }
    
    // Special case for negative values where the whole part is zero
    if (visualFrames == 0 && advantageInternal < 0) {
        return "-0" + subframeStr;
    }

    return sign + std::to_string(visualFrames) + subframeStr;
}

bool IsAttackMove(short moveID) {
    // Attack moves are typically in specific ID ranges
    return (moveID >= 200 && moveID <= 350) ||           
           (moveID >= 400 && moveID <= 500);
}

void MonitorFrameAdvantage(short moveID1, short moveID2, short prevMoveID1, short prevMoveID2) {
    int currentInternalFrame = GetCurrentInternalFrame();
    
    // For gap detection
    static int p1_last_defender_free_frame = -1;
    static int p2_last_defender_free_frame = -1;
    // Accumulate freeze frames between defender free and next connect (subtract from gap)
    static int p1_freeze_accum_since_free = 0;
    static int p2_freeze_accum_since_free = 0;
    
    // Cooldowns for hit detection - REDUCED to improve string detection
    static int p1_hit_connect_cooldown = 0;
    static int p2_hit_connect_cooldown = 0;
    
    // Reduce cooldowns
    if (p1_hit_connect_cooldown > 0) p1_hit_connect_cooldown--;
    if (p2_hit_connect_cooldown > 0) p2_hit_connect_cooldown--;

    // Track recent attack start edges to allow fallback arming when defender becomes non-actionable
    static int p1_last_attack_edge_frame = -1;
    static int p2_last_attack_edge_frame = -1;
    const bool p1_attack_edge = IsAttackMove(moveID1) && !IsAttackMove(prevMoveID1);
    const bool p2_attack_edge = IsAttackMove(moveID2) && !IsAttackMove(prevMoveID2);
    if (p1_attack_edge) p1_last_attack_edge_frame = currentInternalFrame;
    if (p2_attack_edge) p2_last_attack_edge_frame = currentInternalFrame;
    
    // Log the current state for debugging
    static int debugCounter = 0;
    if (detailedLogging.load() && ++debugCounter % 60 == 0) {
     #if defined(ENABLE_FRAME_ADV_DEBUG)
     LogOut("[FRAME_ADV_DEBUG] State: p1Block=" + std::to_string(frameAdvState.p1InBlockstun) + 
               " p2Block=" + std::to_string(frameAdvState.p2InBlockstun) + 
               " p1Def=" + std::to_string(frameAdvState.p1Defending) + 
               " p2Def=" + std::to_string(frameAdvState.p2Defending) + 
               " p1Atk=" + std::to_string(frameAdvState.p1Attacking) + 
         " p2Atk=" + std::to_string(frameAdvState.p2Attacking), 
         false);
     #endif
    }
    
    // Check if the display timer has expired
    if (frameAdvState.displayUntilInternalFrame != -1 && currentInternalFrame >= frameAdvState.displayUntilInternalFrame) {
        // Only clear the display, not the entire state
        if (g_FrameAdvantageId != -1) {
            DirectDrawHook::RemovePermanentMessage(g_FrameAdvantageId);
            g_FrameAdvantageId = -1;
        }
        frameAdvState.displayUntilInternalFrame = -1;
    }
    
    // Update player states for proper tracking
    bool p1Actionable = IsActionable(moveID1);
    bool p2Actionable = IsActionable(moveID2);
    
    // Reset if both players have been in neutral for several frames
    static int neutralFrameCount = 0;
    if (p1Actionable && p2Actionable && !IsAttackMove(moveID1) && !IsAttackMove(moveID2)) {
        neutralFrameCount++;
        if (neutralFrameCount > 30) { // 0.5 seconds
            ResetFrameAdvantageState();
            neutralFrameCount = 0;
            // But keep these for gap detection
            p1_last_defender_free_frame = -1;
            p2_last_defender_free_frame = -1;
        }
    } else {
        neutralFrameCount = 0;
    }
    
    // Detect when defender becomes actionable again (robust vs knockdown/tech/wakeup) for gap detection
    bool p1_becomes_actionable = !IsActionable(prevMoveID1) && IsActionable(moveID1);
    bool p2_becomes_actionable = !IsActionable(prevMoveID2) && IsActionable(moveID2);

    if (p1_becomes_actionable) {
        p1_last_defender_free_frame = currentInternalFrame;
        p1_freeze_accum_since_free = 0;
        #if defined(ENABLE_FRAME_ADV_DEBUG)
        LogOut("[FRAME_ADV_DEBUG] P1 became actionable at frame " + std::to_string(currentInternalFrame), 
            detailedLogging.load());
        #endif
    }

    if (p2_becomes_actionable) {
        p2_last_defender_free_frame = currentInternalFrame;
        p2_freeze_accum_since_free = 0;
        #if defined(ENABLE_FRAME_ADV_DEBUG)
        LogOut("[FRAME_ADV_DEBUG] P2 became actionable at frame " + std::to_string(currentInternalFrame), 
            detailedLogging.load());
        #endif
    }

    // While waiting for the next connect, accumulate freeze frames to remove from "gap"
    auto isFreezeFrame = [&]() -> bool {
        // Consider global game-speed freeze and per-side special/frozen states
        if (PauseIntegration::IsGameSpeedFrozen()) return true;
        if (IsFrozen(moveID1) || IsFrozen(moveID2)) return true;
        if (IsSpecialStun(moveID1) || IsSpecialStun(moveID2)) return true;
        return false;
    };
    if (p1_last_defender_free_frame != -1 && isFreezeFrame()) {
        p1_freeze_accum_since_free++;
    }
    if (p2_last_defender_free_frame != -1 && isFreezeFrame()) {
        p2_freeze_accum_since_free++;
    }
    
    // STEP 1: Detect if an attack connects (P1 attacking P2)
    bool p2_entering_blockstun = IsBlockstunState(moveID2) && !IsBlockstunState(prevMoveID2);
    bool p2_entering_hitstun = IsHitstun(moveID2) && !IsHitstun(prevMoveID2);
    bool p2_entering_thrown   = IsThrown(moveID2)    && !IsThrown(prevMoveID2);
    // Fallback: treat transition from actionable->non-actionable as a connect if it occurs shortly after an attack edge
    bool p2_entering_nonactionable = IsActionable(prevMoveID2) && !IsActionable(moveID2);
    bool p1_recent_attack_window = (p1_last_attack_edge_frame >= 0) && (currentInternalFrame - p1_last_attack_edge_frame <= 60);
    
    if (((p2_entering_blockstun || p2_entering_hitstun || p2_entering_thrown || (p2_entering_nonactionable && p1_recent_attack_window))
         || (p1_attack_edge && !IsActionable(moveID2)))
        && p1_hit_connect_cooldown == 0) {
        // Check for gap between moves in a string
        if (p2_last_defender_free_frame != -1) {
            int gapFramesRaw = currentInternalFrame - p2_last_defender_free_frame;
            int gapFrames = gapFramesRaw - p2_freeze_accum_since_free;
            if (gapFrames < 0) gapFrames = 0;
            
            // Only consider gaps that are reasonably small
            if (gapFrames > 0 && gapFrames <= 60) {
                // Format gap WITHOUT a plus sign
                int visualGapFrames = gapFrames / 3;
                int subframes = gapFrames % 3;
                
                std::string gapText = "Gap: " + std::to_string(visualGapFrames);
                
                // Add subframe precision if needed
                if (subframes == 1) {
                    gapText += ".33";
                } else if (subframes == 2) {
                    gapText += ".66";
                }
                
                // Respect the same visibility gate as the Frame Advantage overlay
                if (g_showFrameAdvantageOverlay.load()) {
                    if (g_FrameAdvantageId != -1) {
                        DirectDrawHook::UpdatePermanentMessage(g_FrameAdvantageId, gapText, RGB(255, 255, 0));
                    } else {
                        g_FrameAdvantageId = DirectDrawHook::AddPermanentMessage(gapText, RGB(255, 255, 0), 305, 430);
                    }
                } else {
                    // If hidden, ensure any existing message is cleared immediately
                    if (g_FrameAdvantageId != -1) {
                        DirectDrawHook::RemovePermanentMessage(g_FrameAdvantageId);
                        g_FrameAdvantageId = -1;
                    }
                }
                
                // Display for ~1/3 second (60 internal frames)
                frameAdvState.displayUntilInternalFrame = currentInternalFrame + 60;
                
                if (detailedLogging.load()) {
                    LogOut(std::string("[FRAME_ADV] Gap detected: ") + gapText +
                           " (raw=" + std::to_string(gapFramesRaw) +
                           ", freeze-removed=" + std::to_string(p2_freeze_accum_since_free) + ")", true);
                }
            }
        }
        
        // CRUCIAL CHANGE: Don't reset the entire state for strings of attacks
        // Instead, just update the attack/defend state for this new hit
        
        // Clear any previous attack tracking but keep the rest of the state
        if (frameAdvState.p1Attacking || frameAdvState.p2Attacking) {
            // Just clear attack state variables
            frameAdvState.p1Attacking = false;
            frameAdvState.p2Attacking = false;
            frameAdvState.p1AttackStartInternalFrame = -1;
            frameAdvState.p2AttackStartInternalFrame = -1;
            frameAdvState.p1ActionableInternalFrame = -1;
            frameAdvState.p2ActionableInternalFrame = -1;
            frameAdvState.p1AdvantageCalculated = false;
            frameAdvState.p2AdvantageCalculated = false;
        }
        
        // Always clear previous calculation flags on a brand-new hit to avoid alternating suppression
        frameAdvState.p1AdvantageCalculated = false;
        frameAdvState.p2AdvantageCalculated = false;
        
        // Set up new attack state
        frameAdvState.p1Attacking = true;
        frameAdvState.p2Defending = true;
        frameAdvState.p1Defending = false; // Opponent is attacking, so P1 is not defending in this exchange
        frameAdvState.p1AttackStartInternalFrame = currentInternalFrame;
        frameAdvState.p2DefenderFreeInternalFrame = -1; // Reset only this defender variable
        
        if (p2_entering_blockstun) {
            frameAdvState.p2InBlockstun = true;
            frameAdvState.p2InHitstun = false;
            frameAdvState.p2BlockstunStartInternalFrame = currentInternalFrame;
            frameAdvState.p2InitialBlockstunMoveID = moveID2;
        } else if (p2_entering_hitstun) {
            frameAdvState.p2InBlockstun = false;
            frameAdvState.p2InHitstun = true;
            frameAdvState.p2HitstunStartInternalFrame = currentInternalFrame;
        } else if (p2_entering_thrown) {
            // Thrown: treat as a connect without setting block/hitstun flags; timings will resolve on actionable
            frameAdvState.p2InBlockstun = false;
            frameAdvState.p2InHitstun = false;
        } else {
            // Non-actionable fallback (e.g., knockdown transition not classified as hit/throw)
            frameAdvState.p2InBlockstun = false;
            frameAdvState.p2InHitstun = false;
        }
        
         #if defined(ENABLE_FRAME_ADV_DEBUG)
         LogOut("[FRAME_ADV_DEBUG] P1->P2 hit connected at frame " + std::to_string(currentInternalFrame), 
             detailedLogging.load());
         #endif
        
        // Reset this for accurate gap detection in the next sequence
    p2_last_defender_free_frame = -1;
    p2_freeze_accum_since_free = 0;
        
        // Apply a minimal cooldown (just 3 internal frames = 1 visual frame)
        p1_hit_connect_cooldown = 3;
    }
    
    // STEP 2: Detect if an attack connects (P2 attacking P1) - mirror of P1 logic
    bool p1_entering_blockstun = IsBlockstunState(moveID1) && !IsBlockstunState(prevMoveID1);
    bool p1_entering_hitstun = IsHitstun(moveID1) && !IsHitstun(prevMoveID1);
    bool p1_entering_thrown   = IsThrown(moveID1)    && !IsThrown(prevMoveID1);
    bool p1_entering_nonactionable = IsActionable(prevMoveID1) && !IsActionable(moveID1);
    bool p2_recent_attack_window = (p2_last_attack_edge_frame >= 0) && (currentInternalFrame - p2_last_attack_edge_frame <= 60);
    
    if (((p1_entering_blockstun || p1_entering_hitstun || p1_entering_thrown || (p1_entering_nonactionable && p2_recent_attack_window))
         || (p2_attack_edge && !IsActionable(moveID1)))
        && p2_hit_connect_cooldown == 0) {
        // Check for gap between moves in a string
        if (p1_last_defender_free_frame != -1) {
            int gapFramesRaw = currentInternalFrame - p1_last_defender_free_frame;
            int gapFrames = gapFramesRaw - p1_freeze_accum_since_free;
            if (gapFrames < 0) gapFrames = 0;
            
            // Only consider gaps that are reasonably small
            if (gapFrames > 0 && gapFrames <= 60) {
                // Format gap WITHOUT a plus sign
                int visualGapFrames = gapFrames / 3;
                int subframes = gapFrames % 3;
                
                std::string gapText = "Gap: " + std::to_string(visualGapFrames);
                
                // Add subframe precision if needed
                if (subframes == 1) {
                    gapText += ".33";
                } else if (subframes == 2) {
                    gapText += ".66";
                }
                
                // Respect the same visibility gate as the Frame Advantage overlay
                if (g_showFrameAdvantageOverlay.load()) {
                    if (g_FrameAdvantageId != -1) {
                        DirectDrawHook::UpdatePermanentMessage(g_FrameAdvantageId, gapText, RGB(255, 255, 0));
                    } else {
                        g_FrameAdvantageId = DirectDrawHook::AddPermanentMessage(gapText, RGB(255, 255, 0), 305, 430);
                    }
                } else {
                    // If hidden, ensure any existing message is cleared immediately
                    if (g_FrameAdvantageId != -1) {
                        DirectDrawHook::RemovePermanentMessage(g_FrameAdvantageId);
                        g_FrameAdvantageId = -1;
                    }
                }
                
                // Display for ~1/3 second (60 internal frames)
                frameAdvState.displayUntilInternalFrame = currentInternalFrame + 60;
                
                if (detailedLogging.load()) {
                    LogOut(std::string("[FRAME_ADV] Gap detected: ") + gapText +
                           " (raw=" + std::to_string(gapFramesRaw) +
                           ", freeze-removed=" + std::to_string(p1_freeze_accum_since_free) + ")", true);
                }
            }
        }
        
        // CRUCIAL CHANGE: Don't reset the entire state for strings of attacks
        // Instead, just update the attack/defend state for this new hit
        
        // Clear any previous attack tracking but keep the rest of the state
        if (frameAdvState.p1Attacking || frameAdvState.p2Attacking) {
            // Just clear attack state variables
            frameAdvState.p1Attacking = false;
            frameAdvState.p2Attacking = false;
            frameAdvState.p1AttackStartInternalFrame = -1;
            frameAdvState.p2AttackStartInternalFrame = -1;
            frameAdvState.p1ActionableInternalFrame = -1;
            frameAdvState.p2ActionableInternalFrame = -1;
            frameAdvState.p1AdvantageCalculated = false;
            frameAdvState.p2AdvantageCalculated = false;
        }
        
        // Always clear previous calculation flags on a brand-new hit to avoid alternating suppression
        frameAdvState.p1AdvantageCalculated = false;
        frameAdvState.p2AdvantageCalculated = false;
        
        // Set up new attack state
        frameAdvState.p2Attacking = true;
        frameAdvState.p1Defending = true;
        frameAdvState.p2Defending = false; // Opponent is attacking, so P2 is not defending in this exchange
        frameAdvState.p2AttackStartInternalFrame = currentInternalFrame;
        frameAdvState.p1DefenderFreeInternalFrame = -1; // Reset only this defender variable
        
        if (p1_entering_blockstun) {
            frameAdvState.p1InBlockstun = true;
            frameAdvState.p1InHitstun = false;
            frameAdvState.p1BlockstunStartInternalFrame = currentInternalFrame;
            frameAdvState.p1InitialBlockstunMoveID = moveID1;
        } else if (p1_entering_hitstun) {
            frameAdvState.p1InBlockstun = false;
            frameAdvState.p1InHitstun = true;
            frameAdvState.p1HitstunStartInternalFrame = currentInternalFrame;
        } else if (p1_entering_thrown) {
            frameAdvState.p1InBlockstun = false;
            frameAdvState.p1InHitstun = false;
        } else {
            // Non-actionable fallback
            frameAdvState.p1InBlockstun = false;
            frameAdvState.p1InHitstun = false;
        }
        
         #if defined(ENABLE_FRAME_ADV_DEBUG)
         LogOut("[FRAME_ADV_DEBUG] P2->P1 hit connected at frame " + std::to_string(currentInternalFrame), 
             detailedLogging.load());
         #endif
        
        // Reset this for accurate gap detection in the next sequence
    p1_last_defender_free_frame = -1;
    p1_freeze_accum_since_free = 0;
        
        // Apply a minimal cooldown (just 3 internal frames = 1 visual frame)
        p2_hit_connect_cooldown = 3;
    }
    
    // STEP 3: Detect when attacker exits recovery
    if (frameAdvState.p1Attacking && frameAdvState.p1ActionableInternalFrame == -1) {
        if (!IsActionable(prevMoveID1) && IsActionable(moveID1)) {
            frameAdvState.p1ActionableInternalFrame = currentInternalFrame;
         #if defined(ENABLE_FRAME_ADV_DEBUG)
         LogOut("[FRAME_ADV_DEBUG] P1 attacker recovery ended at frame " + 
             std::to_string(currentInternalFrame), detailedLogging.load());
         #endif
        }
    }
    
    if (frameAdvState.p2Attacking && frameAdvState.p2ActionableInternalFrame == -1) {
        if (!IsActionable(prevMoveID2) && IsActionable(moveID2)) {
            frameAdvState.p2ActionableInternalFrame = currentInternalFrame;
         #if defined(ENABLE_FRAME_ADV_DEBUG)
         LogOut("[FRAME_ADV_DEBUG] P2 attacker recovery ended at frame " + 
             std::to_string(currentInternalFrame), detailedLogging.load());
         #endif
        }
    }
    
    // STEP 4: Detect when defender becomes actionable (covers knockdowns, ground/air tech, wakeup)
    if (frameAdvState.p2Defending && frameAdvState.p2DefenderFreeInternalFrame == -1) {
        bool nowActionable = IsActionable(moveID2);
        bool wasNotActionable = !IsActionable(prevMoveID2);
        if (wasNotActionable && nowActionable) {
            frameAdvState.p2DefenderFreeInternalFrame = currentInternalFrame;
            #if defined(ENABLE_FRAME_ADV_DEBUG)
            LogOut("[FRAME_ADV_DEBUG] P2 defender actionable at frame " + 
                std::to_string(currentInternalFrame), detailedLogging.load());
            #endif
        }
    }

    if (frameAdvState.p1Defending && frameAdvState.p1DefenderFreeInternalFrame == -1) {
        bool nowActionable = IsActionable(moveID1);
        bool wasNotActionable = !IsActionable(prevMoveID1);
        if (wasNotActionable && nowActionable) {
            frameAdvState.p1DefenderFreeInternalFrame = currentInternalFrame;
            #if defined(ENABLE_FRAME_ADV_DEBUG)
            LogOut("[FRAME_ADV_DEBUG] P1 defender actionable at frame " + 
                std::to_string(currentInternalFrame), detailedLogging.load());
            #endif
        }
    }
    
    // STEP 5: Calculate frame advantage when all necessary data is available
    if (frameAdvState.p1Attacking && !frameAdvState.p1AdvantageCalculated &&
        frameAdvState.p1ActionableInternalFrame != -1 && frameAdvState.p2DefenderFreeInternalFrame != -1) {
        
        // Calculate frame advantage (defender free - attacker actionable)
        // Positive: Attacker has advantage
        // Negative: Defender has advantage
        int frameAdvantage = frameAdvState.p2DefenderFreeInternalFrame - frameAdvState.p1ActionableInternalFrame;
        frameAdvState.p1FrameAdvantage = frameAdvantage;
        frameAdvState.p1AdvantageCalculated = true;
        
        // Format the advantage display with proper sign and subframe precision
        // FormatFrameAdvantage already includes the "+" for positive values
        std::string frameAdvText = FormatFrameAdvantage(frameAdvantage);
        
        // Display the calculated advantage (unless suppressed by RG overlay takeover)
        if (g_showFrameAdvantageOverlay.load() && currentInternalFrame >= g_SkipRegularFAOverlayUntilFrame.load()) {
            if (g_FrameAdvantageId != -1) {
                DirectDrawHook::UpdatePermanentMessage(g_FrameAdvantageId, frameAdvText, 
                    frameAdvantage >= 0 ? RGB(0, 255, 0) : RGB(255, 0, 0));
            } else {
                g_FrameAdvantageId = DirectDrawHook::AddPermanentMessage(frameAdvText, 
                    frameAdvantage >= 0 ? RGB(0, 255, 0) : RGB(255, 0, 0), 305, 430);
            }
            // Ensure any secondary RG segment is removed when regular FA takes over
            if (g_FrameAdvantage2Id != -1) {
                DirectDrawHook::RemovePermanentMessage(g_FrameAdvantage2Id);
                g_FrameAdvantage2Id = -1;
            }
            // Set display duration (show for approximately 1 second)
            frameAdvState.displayUntilInternalFrame = currentInternalFrame + 192;
        }
        
        LogOut("[FRAME_ADV] P1->P2 Frame Advantage: " + frameAdvText, true);
        
        // Reset attack state for the next sequence while preserving defender state
        frameAdvState.p1Attacking = false;
        frameAdvState.p1AttackStartInternalFrame = -1;
        frameAdvState.p1ActionableInternalFrame = -1;
    }
    
    if (frameAdvState.p2Attacking && !frameAdvState.p2AdvantageCalculated &&
        frameAdvState.p2ActionableInternalFrame != -1 && frameAdvState.p1DefenderFreeInternalFrame != -1) {
        
        // Calculate advantage (defender free - attacker actionable)
        int frameAdvantage = frameAdvState.p1DefenderFreeInternalFrame - frameAdvState.p2ActionableInternalFrame;
        frameAdvState.p2FrameAdvantage = frameAdvantage;
        frameAdvState.p2AdvantageCalculated = true;
        
        // Format the advantage display with proper sign and subframe precision
        // FormatFrameAdvantage already includes the "+" for positive values
        std::string frameAdvText = FormatFrameAdvantage(frameAdvantage);
        
        // Display the calculated advantage (unless suppressed by RG overlay takeover)
        if (g_showFrameAdvantageOverlay.load() && currentInternalFrame >= g_SkipRegularFAOverlayUntilFrame.load()) {
            if (g_FrameAdvantageId != -1) {
                DirectDrawHook::UpdatePermanentMessage(g_FrameAdvantageId, frameAdvText, 
                    frameAdvantage >= 0 ? RGB(0, 255, 0) : RGB(255, 0, 0));
            } else {
                g_FrameAdvantageId = DirectDrawHook::AddPermanentMessage(frameAdvText, 
                    frameAdvantage >= 0 ? RGB(0, 255, 0) : RGB(255, 0, 0), 305, 430);
            }
            // Ensure any secondary RG segment is removed when regular FA takes over
            if (g_FrameAdvantage2Id != -1) {
                DirectDrawHook::RemovePermanentMessage(g_FrameAdvantage2Id);
                g_FrameAdvantage2Id = -1;
            }
            // Set display duration (show for approximately 1 second)
            frameAdvState.displayUntilInternalFrame = currentInternalFrame + 192;
        }
        
        LogOut("[FRAME_ADV] P2->P1 Frame Advantage: " + frameAdvText, true);
        
        // Reset attack state for the next sequence while preserving defender state
        frameAdvState.p2Attacking = false;
        frameAdvState.p2AttackStartInternalFrame = -1;
        frameAdvState.p2ActionableInternalFrame = -1;
    }
    
    // STEP 6: Timeout detection for stale states (reworked to tolerate long throw/tech sequences)
    // Only increment the stale counter when we're not still waiting for required actionable/free events.
    static int staleFrameCounter = 0;
    bool hasActiveTracking = (frameAdvState.p1Attacking && !frameAdvState.p1AdvantageCalculated) ||
                             (frameAdvState.p2Attacking && !frameAdvState.p2AdvantageCalculated);

    auto stillWaitingForTimings = [&](short curP1, short curP2) -> bool {
        // If attacker actionable time isn't known yet and attacker isn't actionable now, we're still waiting.
        bool waitingP1Atk = frameAdvState.p1Attacking && frameAdvState.p1ActionableInternalFrame == -1 && !IsActionable(curP1);
        bool waitingP2Atk = frameAdvState.p2Attacking && frameAdvState.p2ActionableInternalFrame == -1 && !IsActionable(curP2);
        // If defender free time isn't known yet and defender isn't actionable now, we're still waiting.
        bool waitingP1Def = frameAdvState.p1Defending && frameAdvState.p1DefenderFreeInternalFrame == -1 && !IsActionable(curP1);
        bool waitingP2Def = frameAdvState.p2Defending && frameAdvState.p2DefenderFreeInternalFrame == -1 && !IsActionable(curP2);
        return waitingP1Atk || waitingP2Atk || waitingP1Def || waitingP2Def;
    };

    if (hasActiveTracking) {
        // Pause the stale counter during any non-actionable lockout we're explicitly waiting to resolve
        if (stillWaitingForTimings(moveID1, moveID2)) {
            staleFrameCounter = 0; // actively waiting: don't consider this stale
        } else {
            staleFrameCounter++;
        }
        // If we've been tracking without progress for more than 6 seconds, reset to avoid getting stuck
        if (staleFrameCounter > 1152) {  // ~6 seconds at 192 fps
            LogOut("[FRAME_ADV] Stale state detected (no progress), resetting", true);
            ResetFrameAdvantageState();
            staleFrameCounter = 0;
            // But keep these for gap detection
            p1_last_defender_free_frame = -1;
            p2_last_defender_free_frame = -1;
        }
    } else {
        staleFrameCounter = 0;
    }
}

bool IsFrameAdvantageActive() {
    return frameAdvState.p1Attacking || frameAdvState.p2Attacking ||
           frameAdvState.p1Defending || frameAdvState.p2Defending;
}

FrameAdvantageState GetFrameAdvantageState() {
    return frameAdvState;
}