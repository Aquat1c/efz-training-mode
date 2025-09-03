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
#include "../include/input/input_buffer.h"
#include "../include/utils/config.h"
#include "../include/input/input_motion.h"
#include "../include/utils/network.h"
#define DISABLE_ATTACK_READER 1
#include "../include/game/attack_reader.h"
#include "../include/game/practice_patch.h"
#include "../include/game/character_settings.h"
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
            actionType == ACTION_214214 ||
        actionType == ACTION_641236) {
            
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
            case ACTION_641236: return "641236" + strengthLetter; // Double QCF + strength
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
            p1DelayState = {false, 0, TRIGGER_NONE, 0};
            p2DelayState = {false, 0, TRIGGER_NONE, 0};
            break; // exit thread to allow safe self-unload
        }
        auto frameStart = clock::now();
        // Catch-up logic: if we are *very* late (> 10 frames), jump ahead to avoid cascading backlog
        if (frameStart - expectedNext > targetFrameTime * 10) {
            expectedNext = frameStart + targetFrameTime;
        }
        
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
                }
            }

            // If we just arrived at Character Select, clear all triggers persistently AFTER stability check.
            if (currentPhase == GamePhase::CharacterSelect) {
                // Increment consecutive CS frames; only act after a debounce window (e.g. 120 frames ≈ 0.6s @192fps)
                s_characterSelectPhaseFrames++;
                if (s_characterSelectPhaseFrames == 1 && detailedLogging.load()) {
                    LogOut("[FRAME MONITOR] Detected CharacterSelect phase - starting debounce window", true);
                }

                if (s_characterSelectPhaseFrames >= 120) {
                    // Additional safety: ensure we are NOT currently in a valid gameplay mode with initialized characters
                    GameMode gmNow = GetCurrentGameMode();
                    bool charsInit = AreCharactersInitialized();
                    if (!charsInit) {
                        LogOut("[FRAME MONITOR] CharacterSelect phase stable (>=120 frames) and characters not initialized -> clearing triggers", true);
                        ClearAllTriggersPersistently();
                        s_characterSelectPhaseFrames = 0; // reset after action
                    } else if (detailedLogging.load()) {
                        // Likely a false detection (e.g., transient phase glitch) – keep features
                        LogOut("[FRAME MONITOR] CharacterSelect phase stable but characters still initialized; skipping trigger clear (possible false phase)", true);
                    }
                }
            } else if (s_characterSelectPhaseFrames > 0) {
                // Reset debounce counter if we left CharacterSelect before threshold
                s_characterSelectPhaseFrames = 0;
            }

            lastPhase = currentPhase;
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
            uintptr_t base = GetEFZBase();
            if (!base) {
                goto FRAME_MONITOR_FRAME_END;
            }

            // Pointer change logging
            uintptr_t p1Ptr = 0, p2Ptr = 0;
            SafeReadMemory(base + EFZ_BASE_OFFSET_P1, &p1Ptr, sizeof(p1Ptr));
            SafeReadMemory(base + EFZ_BASE_OFFSET_P2, &p2Ptr, sizeof(p2Ptr));
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
                        uintptr_t p1PtrDbg = 0, p2PtrDbg = 0;
                        SafeReadMemory(base + EFZ_BASE_OFFSET_P1, &p1PtrDbg, sizeof(p1PtrDbg));
                        SafeReadMemory(base + EFZ_BASE_OFFSET_P2, &p2PtrDbg, sizeof(p2PtrDbg));
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

            // ALWAYS process frame advantage for precise timing
            if (moveIDsChanged) {
                MonitorFrameAdvantage(moveID1, moveID2, prevMoveID1, prevMoveID2);
            }
            
            // Run dummy auto-block stance early for minimal latency (uses current move IDs)
            MonitorDummyAutoBlock(moveID1, moveID2, prevMoveID1, prevMoveID2);

            if (moveIDsChanged || criticalFeaturesActive) {
                // STEP 1: Process auto-actions FIRST (highest priority)
                ProcessTriggerDelays();      // Handle pending delays
                // Pass cached move IDs to avoid extra reads and enable lighter math inside
                MonitorAutoActions(moveID1, moveID2, prevMoveID1, prevMoveID2);
                
                // STEP 2: Auto-jump
                // Always call to allow internal cleanup when toggled off; function self-checks enable state
                MonitorAutoJump();
                
                // STEP 3: Auto-airtech (every frame for precision, no throttling)
                MonitorAutoAirtech(moveID1, moveID2);  
                ClearDelayStatesIfNonActionable();     
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
                    s_p1YAddr = ResolvePointer(base, EFZ_BASE_OFFSET_P1, YPOS_OFFSET);
                    s_p2YAddr = ResolvePointer(base, EFZ_BASE_OFFSET_P2, YPOS_OFFSET);
                    s_p1XAddr = ResolvePointer(base, EFZ_BASE_OFFSET_P1, XPOS_OFFSET);
                    s_p2XAddr = ResolvePointer(base, EFZ_BASE_OFFSET_P2, XPOS_OFFSET);
                    s_p1HpAddr = ResolvePointer(base, EFZ_BASE_OFFSET_P1, HP_OFFSET);
                    s_p2HpAddr = ResolvePointer(base, EFZ_BASE_OFFSET_P2, HP_OFFSET);
                    s_p1MeterAddr = ResolvePointer(base, EFZ_BASE_OFFSET_P1, METER_OFFSET);
                    s_p2MeterAddr = ResolvePointer(base, EFZ_BASE_OFFSET_P2, METER_OFFSET);
                    s_p1RfAddr = ResolvePointer(base, EFZ_BASE_OFFSET_P1, RF_OFFSET);
                    s_p2RfAddr = ResolvePointer(base, EFZ_BASE_OFFSET_P2, RF_OFFSET);
                    s_p1CharNameAddr = ResolvePointer(base, EFZ_BASE_OFFSET_P1, CHARACTER_NAME_OFFSET);
                    s_p2CharNameAddr = ResolvePointer(base, EFZ_BASE_OFFSET_P2, CHARACTER_NAME_OFFSET);
                    s_cacheCounter = 0;
                }

                // Read values with best-effort safety
                double p1Y=0.0, p2Y=0.0; if (s_p1YAddr) SafeReadMemory(s_p1YAddr, &p1Y, sizeof(p1Y)); if (s_p2YAddr) SafeReadMemory(s_p2YAddr, &p2Y, sizeof(p2Y));
                double p1X=0.0, p2X=0.0; if (s_p1XAddr) SafeReadMemory(s_p1XAddr, &p1X, sizeof(p1X)); if (s_p2XAddr) SafeReadMemory(s_p2XAddr, &p2X, sizeof(p2X));
                int p1Hp=0, p2Hp=0; if (s_p1HpAddr) SafeReadMemory(s_p1HpAddr, &p1Hp, sizeof(p1Hp)); if (s_p2HpAddr) SafeReadMemory(s_p2HpAddr, &p2Hp, sizeof(p2Hp));
                int p1Meter=0, p2Meter=0; if (s_p1MeterAddr) SafeReadMemory(s_p1MeterAddr, &p1Meter, sizeof(p1Meter)); if (s_p2MeterAddr) SafeReadMemory(s_p2MeterAddr, &p2Meter, sizeof(p2Meter));
                double p1Rf=0.0, p2Rf=0.0; if (s_p1RfAddr) SafeReadMemory(s_p1RfAddr, &p1Rf, sizeof(p1Rf)); if (s_p2RfAddr) SafeReadMemory(s_p2RfAddr, &p2Rf, sizeof(p2Rf));

                FrameSnapshot snap{};
                snap.tickMs = GetTickCount64();
                snap.phase = currentPhase;
                snap.mode = currentMode;
                snap.p1Move = moveID1;
                snap.p2Move = moveID2;
                snap.prevP1Move = prevMoveID1;
                snap.prevP2Move = prevMoveID2;
                snap.p2BlockEdge = (IsBlockstun(prevMoveID2) && !IsBlockstun(moveID2) && IsActionable(moveID2));
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

        // Maintain RF freeze inline at a modest tick rate (~32 Hz)
        {
            static int rfDecim = 0; if ((rfDecim++ % 6) == 0) { UpdateRFFreezeTick(); }
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
          LogOut("[FRAME_MONITOR][SECTIONS] samples=" + std::to_string(sec_samples) +
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
        }
        if (g_statsCleanHitId != -1) {
            DirectDrawHook::RemovePermanentMessage(g_statsCleanHitId);
            g_statsCleanHitId = -1;
        }
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
        }
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
        return;
    }

    // Prefer live snapshot; fallback to minimal direct reads only when stale
    FrameSnapshot snap{};
    bool haveSnap = TryGetLatestSnapshot(snap, 300);
    uintptr_t base = GetEFZBase();
    if (!base && !haveSnap) return;

    int p1Hp = 0, p2Hp = 0, p1Meter = 0, p2Meter = 0; double p1Rf = 0.0, p2Rf = 0.0;
    double p1X = 0.0, p1Y = 0.0, p2X = 0.0, p2Y = 0.0; short p1MoveId = 0, p2MoveId = 0;

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
        if (p1HpAddr) SafeReadMemory(p1HpAddr, &p1Hp, sizeof(int));
        if (p1MeterAddr) SafeReadMemory(p1MeterAddr, &p1Meter, sizeof(int));
        if (p1RfAddr) SafeReadMemory(p1RfAddr, &p1Rf, sizeof(double));
        if (p2HpAddr) SafeReadMemory(p2HpAddr, &p2Hp, sizeof(int));
        if (p2MeterAddr) SafeReadMemory(p2MeterAddr, &p2Meter, sizeof(int));
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
            uintptr_t p1BasePtr = 0;
            if (SafeReadMemory(base + EFZ_BASE_OFFSET_P1, &p1BasePtr, sizeof(p1BasePtr)) && p1BasePtr) {
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
    }

    if (statsOn) {
        upsert(g_statsMoveIdId, moveIds.str());
    }

    return;
}
