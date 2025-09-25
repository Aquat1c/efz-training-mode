#include "../include/game/auto_airtech.h"
#include "../include/core/constants.h"
#include "../include/utils/utilities.h"

#include "../include/core/memory.h"
#include "../include/core/logger.h"
#include "../include/input/input_core.h" // for WritePlayerInputImmediate and GAME_INPUT_*
#include <thread>
#include <atomic>
#include <chrono>

// Legacy patching variables retained for cleanup but no longer used for operation
char originalEnableBytes[2] = {0x74, 0x71};
char originalForwardBytes[2] = {0x75, 0x24};
char originalBackwardBytes[2] = {0x75, 0x21};
bool patchesApplied = false;

// Track if player is currently airteching
bool p1IsAirteching = false;
bool p2IsAirteching = false;

// Public atomic flags (visible to other modules/UI)
std::atomic<bool> g_airtechP1Active{false};
std::atomic<bool> g_airtechP2Active{false};
std::atomic<bool> g_airtechP1Airtechable{false};
std::atomic<bool> g_airtechP2Airtechable{false};
std::atomic<bool> g_airtechP1FacingRight{true};
std::atomic<bool> g_airtechP2FacingRight{true};

// Deprecated: No longer used. We now inject inputs directly.
void ApplyAirtechPatches() {
    uintptr_t base = GetEFZBase();
    if (!base) {
        LogOut("[AUTO-AIRTECH] Cannot get game base address", true);
        return;
    }
    // Do nothing in safe mode; ensure we mark as not applied
    if (patchesApplied) {
        LogOut("[AUTO-AIRTECH] Patches were unexpectedly applied; removing for safe mode", true);
        PatchMemory(base + AIRTECH_ENABLE_ADDR, originalEnableBytes, 2);
        PatchMemory(base + AIRTECH_FORWARD_ADDR, originalForwardBytes, 2);
        PatchMemory(base + AIRTECH_BACKWARD_ADDR, originalBackwardBytes, 2);
        patchesApplied = false;
    }
}

void RemoveAirtechPatches() {
    uintptr_t base = GetEFZBase();
    if (!base) {
        LogOut("[AUTO-AIRTECH] Cannot get game base address", true);
        return;
    }

    // Restore all patches to original bytes
    PatchMemory(base + AIRTECH_ENABLE_ADDR, originalEnableBytes, 2);
    PatchMemory(base + AIRTECH_FORWARD_ADDR, originalForwardBytes, 2);
    PatchMemory(base + AIRTECH_BACKWARD_ADDR, originalBackwardBytes, 2);

    patchesApplied = false;
    LogOut("[AUTO-AIRTECH] Patches removed", detailedLogging.load());
}

// Check if the player is currently in an airtechable state
bool IsPlayerAirtechable(short moveID, int playerNum) {
    uintptr_t base = GetEFZBase();
    if (!base) return false;
    
    uintptr_t baseOffset = (playerNum == 1) ? EFZ_BASE_OFFSET_P1 : EFZ_BASE_OFFSET_P2;
    
    // Check untech value
    short untechValue = 0;
    uintptr_t untechAddr = ResolvePointer(base, baseOffset, UNTECH_OFFSET);
    if (untechAddr) {
        SafeReadMemory(untechAddr, &untechValue, sizeof(short));
    }
    
    // Check moveID for airtechable states: launched hitstun or special stun only
    // Do NOT include AIR_GUARD to avoid spurious triggers while air-blocking
    bool airtechableMoveID = (moveID >= LAUNCHED_HITSTUN_START && moveID <= LAUNCHED_HITSTUN_END) ||
                             (moveID == FIRE_STATE || moveID == ELECTRIC_STATE);
    
    // Untech value == 0 means player can tech
    // AND moveID should be in an airtechable state
    return airtechableMoveID && (untechValue == 0);
}

// Check if player is in airtech animation
bool IsAirtechAnimation(short moveID) {
    return moveID == FORWARD_AIRTECH || moveID == BACKWARD_AIRTECH;
}

void MonitorAutoAirtech(short moveID1, short moveID2) {
    static bool prevEnabled = false;
    static int prevDirection = -1;
    static int prevDelay = -1;
    static bool p1WasAirtechable = false;
    static bool p2WasAirtechable = false;
    static int p1DelayCounter = 0;
    static int p2DelayCounter = 0;
    static int debugCounter = 0;
    static bool p1WasInjecting = false;
    static bool p2WasInjecting = false;
    // Short press windows (in internal frames) for immediate injection
    static int p1InjectRemaining = 0;
    static int p2InjectRemaining = 0;
    // Armed flags to ensure we inject once per edge into airtechable
    static bool p1Armed = false;
    static bool p2Armed = false;
    // Re-attempt support: count attempts during a single airtechable window so we can retry if timing misses
    static int p1AttemptCount = 0;
    static int p2AttemptCount = 0;
    // Configurable (via constant for now) injection hold length in internal frames (192hz). Original was 3 (~1 visual frame).
    // New strategy: perform discrete buffered press attempts: Neutral -> (Dir+Button) for a few frames -> release.
    // We cycle buttons A,B,C across attempts to maximize acceptance.
    constexpr int kPreNeutralFrames = 3;          // internal frames (~1 visual) of forced neutral before each attempt
    constexpr int kInjectHoldFramesInitial = 6;   // internal frames (~2 visual) hold of dir+button
    constexpr int kMaxRetryAttempts = 3;          // A, then B, then C

    // Cache per-frame facing direction early (single reads per player)
    bool p1FacingRightNow = GetPlayerFacingDirection(1);
    bool p2FacingRightNow = GetPlayerFacingDirection(2);
    g_airtechP1FacingRight.store(p1FacingRightNow);
    g_airtechP2FacingRight.store(p2FacingRightNow);

    // FAST-PATH: If feature is disabled and we are not in detailed diagnostics, avoid all heavy work.
    // This skips: untech/moveID classification, delay/injection state machines, and heartbeat/status logs.
    // We still honor a very slow (30s) diagnostic heartbeat if detailedLogging is enabled so the user
    // can confirm the system is idle without flooding the log.
    if (!autoAirtechEnabled.load()) {
        // Ensure exported flags reflect idle state
        g_airtechP1Active.store(false);
        g_airtechP2Active.store(false);
        g_airtechP1Airtechable.store(false);
        g_airtechP2Airtechable.store(false);
        // Release any lingering manual overrides (paranoia)
        if (g_manualInputOverride[1].load()) { g_manualInputOverride[1].store(false); g_manualInputMask[1].store(0); }
        if (g_manualInputOverride[2].load()) { g_manualInputOverride[2].store(false); g_manualInputMask[2].store(0); }
        static auto s_lastDisabledBeat = std::chrono::steady_clock::now();
        auto nowBeat = std::chrono::steady_clock::now();
        if (detailedLogging.load() && (nowBeat - s_lastDisabledBeat) >= std::chrono::seconds(30)) {
            LogOut("[AUTO-AIRTECH] Idle (disabled) - skipping per-frame processing", true);
            s_lastDisabledBeat = nowBeat;
        }
        return; // nothing else to do while disabled
    }

    // Track if player is in airtech animation
    bool p1CurrentlyAirteching = IsAirtechAnimation(moveID1);
    bool p2CurrentlyAirteching = IsAirtechAnimation(moveID2);
    
    // Detect transitions into airtech animation
    if (p1CurrentlyAirteching && !p1IsAirteching) {
        LogOut("[AUTO-AIRTECH] P1 entered airtech animation", detailedLogging.load());
    if (patchesApplied) RemoveAirtechPatches();
    p1DelayCounter = 0;
    p1InjectRemaining = 0;
    p1Armed = false;
    p1AttemptCount = 0;
    }
    
    if (p2CurrentlyAirteching && !p2IsAirteching) {
        LogOut("[AUTO-AIRTECH] P2 entered airtech animation", detailedLogging.load());
    if (patchesApplied) RemoveAirtechPatches();
    p2DelayCounter = 0;
    p2InjectRemaining = 0;
    p2Armed = false;
    p2AttemptCount = 0;
    }
    
    // Update tracking variables
    p1IsAirteching = p1CurrentlyAirteching;
    p2IsAirteching = p2CurrentlyAirteching;

    // Update exported flags and log only on change (with a slow heartbeat)
    static bool lastP1Active=false, lastP2Active=false, lastP1Able=false, lastP2Able=false;
    static auto lastHeartbeat = std::chrono::steady_clock::now();
    bool p1ActiveNow = p1IsAirteching;
    bool p2ActiveNow = p2IsAirteching;
    // Compute airtechable using helpers (untech + moveID classification)
    bool p1AbleNow = IsPlayerAirtechable(moveID1, 1);
    bool p2AbleNow = IsPlayerAirtechable(moveID2, 2);
    g_airtechP1Active.store(p1ActiveNow);
    g_airtechP2Active.store(p2ActiveNow);
    g_airtechP1Airtechable.store(p1AbleNow);
    g_airtechP2Airtechable.store(p2AbleNow);

    bool changed = (p1ActiveNow!=lastP1Active) || (p2ActiveNow!=lastP2Active) ||
                   (p1AbleNow!=lastP1Able) || (p2AbleNow!=lastP2Able);
    auto nowTs = std::chrono::steady_clock::now();
    // Heartbeat only when enabled & something interesting happened recently OR players are in/near an airtech window.
    bool heartbeat = (nowTs - lastHeartbeat) >= std::chrono::seconds(5) && (p1ActiveNow || p2ActiveNow || p1AbleNow || p2AbleNow);
    if (detailedLogging.load() && (changed || heartbeat)) {
        LogOut("[AUTO-AIRTECH] Status: Enabled=" + std::to_string(autoAirtechEnabled.load()) +
               ", Direction=" + std::to_string(autoAirtechDirection.load()) +
               ", Delay=" + std::to_string(autoAirtechDelay.load()) +
               ", Patches=" + (patchesApplied ? "YES" : "NO") +
               ", P1Active=" + std::string(p1ActiveNow?"YES":"NO") +
               ", P2Active=" + std::string(p2ActiveNow?"YES":"NO") +
               ", P1Able=" + std::string(p1AbleNow?"YES":"NO") +
               ", P2Able=" + std::string(p2AbleNow?"YES":"NO"),
               true);
        lastP1Active=p1ActiveNow; lastP2Active=p2ActiveNow;
        lastP1Able=p1AbleNow; lastP2Able=p2AbleNow;
        if (heartbeat) lastHeartbeat = nowTs;
    }

    // Settings changed
    bool settingsChanged = (prevEnabled != autoAirtechEnabled.load() ||
                           prevDirection != autoAirtechDirection.load() ||
                           prevDelay != autoAirtechDelay.load());
    
    if (settingsChanged) {
        // On any change, clear state and ensure no patches remain
        if (patchesApplied) RemoveAirtechPatches();
    p1DelayCounter = 0;
    p2DelayCounter = 0;
    p1InjectRemaining = 0;
    p2InjectRemaining = 0;
    p1AttemptCount = 0;
    p2AttemptCount = 0;
        
        prevEnabled = autoAirtechEnabled.load();
        prevDirection = autoAirtechDirection.load();
        prevDelay = autoAirtechDelay.load();

        LogOut(
            std::string("[AUTO-AIRTECH] Settings changed -> Enabled=") + (prevEnabled ? "1" : "0") +
            ", Direction=" + std::to_string(prevDirection) +
            ", Delay=" + std::to_string(prevDelay),
            detailedLogging.load());
    }

    // Helper to request injection via the central input hook (immediate-only path)
    auto composeMaskForAttempt = [&](int playerNum, int attemptIdx) -> uint8_t {
        bool facingRight = (playerNum == 1) ? g_airtechP1FacingRight.load() : g_airtechP2FacingRight.load();
        bool forward = autoAirtechDirection.load() == 0;
        uint8_t horzMask = 0;
        if (forward) horzMask = facingRight ? GAME_INPUT_RIGHT : GAME_INPUT_LEFT;
        else         horzMask = facingRight ? GAME_INPUT_LEFT  : GAME_INPUT_RIGHT;
        uint8_t buttonMask = 0;
        switch (attemptIdx) {
            case 0: buttonMask = GAME_INPUT_A; break;
            case 1: buttonMask = GAME_INPUT_B; break;
            default: buttonMask = GAME_INPUT_C; break;
        }
        return horzMask | buttonMask;
    };

    auto applyOverrideMask = [](int playerNum, uint8_t mask, bool buffered) {
        g_manualInputMask[playerNum].store(mask);
        g_manualInputOverride[playerNum].store(true);
        // For airtech we WANT buffer writes so the engine sees the edge in history
        g_injectImmediateOnly[playerNum].store(!buffered ? true : false);
    };

    // Per-player attempt state machine additions
    static int p1PreNeutral = 0, p2PreNeutral = 0;     // countdown of neutral frames before injecting
    static bool p1AttemptActive = false, p2AttemptActive = false; // injecting window

    // If enabled, manage delay/injection per player independently
    if (autoAirtechEnabled.load()) {
        // Pre-compute edge detection for this frame
        bool p1Airtechable = p1AbleNow;
        bool p2Airtechable = p2AbleNow;
        bool p1JustBecameAirtechable = (p1Airtechable && !p1WasAirtechable);
        bool p2JustBecameAirtechable = (p2Airtechable && !p2WasAirtechable);

        // --- P1 ---
    if (!p1IsAirteching) {
            if (p1JustBecameAirtechable) {
        p1DelayCounter = autoAirtechDelay.load();
        p1Armed = true;
                p1AttemptCount = 0;
                // Read untech for richer diagnostics (one-time on transition)
                short untechValue = 0;
                uintptr_t base = GetEFZBase();
                uintptr_t untechAddr = ResolvePointer(base, EFZ_BASE_OFFSET_P1, UNTECH_OFFSET);
                if (untechAddr) {
                    SafeReadMemory(untechAddr, &untechValue, sizeof(short));
                }
                LogOut("[AUTO-AIRTECH] P1 became airtechable (moveID=" + std::to_string(moveID1) +
                       ", untech=" + std::to_string(untechValue) + "), starting delay: " + 
                       std::to_string(autoAirtechDelay.load()) + " frames", detailedLogging.load());
            }
            if (p1DelayCounter > 0) {
                p1DelayCounter--;
                if (p1DelayCounter == 0 && p1Airtechable && p1Armed) {
                    // Single continuous hold attempt
                    // Start pre-neutral phase then injection attempt
                    p1PreNeutral = kPreNeutralFrames;
                    p1InjectRemaining = kInjectHoldFramesInitial; // used AFTER neutral
                    p1Armed = false;
                    p1AttemptCount++;
                    bool facingRight = p1FacingRightNow;
                    bool forward = autoAirtechDirection.load() == 0;
                    uint8_t horz = 0;
                    if (forward) horz = facingRight ? 1 : 255; else horz = facingRight ? 255 : 1;
                    const char* dirStr = forward ? "FORWARD" : "BACKWARD";
                    LogOut(std::string("[AUTO-AIRTECH] P1 delay expired -> injecting A+dir (dir=") + dirStr +
                           ", facingRight=" + (facingRight ? "1" : "0") + ", horz=" + std::to_string(horz) + ") attempt=" + std::to_string(p1AttemptCount),
                           true);
                }
            }
            p1WasAirtechable = p1Airtechable;
        }

        // --- P2 ---
    if (!p2IsAirteching) {
            if (p2JustBecameAirtechable) {
        p2DelayCounter = autoAirtechDelay.load();
        p2Armed = true;
                p2AttemptCount = 0;
                short untechValue = 0;
                uintptr_t base = GetEFZBase();
                uintptr_t untechAddr = ResolvePointer(base, EFZ_BASE_OFFSET_P2, UNTECH_OFFSET);
                if (untechAddr) {
                    SafeReadMemory(untechAddr, &untechValue, sizeof(short));
                }
                LogOut("[AUTO-AIRTECH] P2 became airtechable (moveID=" + std::to_string(moveID2) +
                       ", untech=" + std::to_string(untechValue) + "), starting delay: " + 
                       std::to_string(autoAirtechDelay.load()) + " frames", detailedLogging.load());
            }
            if (p2DelayCounter > 0) {
                p2DelayCounter--;
    if (p2DelayCounter == 0 && p2Airtechable && p2Armed) {
            p2PreNeutral = kPreNeutralFrames;
            p2InjectRemaining = kInjectHoldFramesInitial;
        p2Armed = false;
            p2AttemptCount++;
                    bool facingRight = p2FacingRightNow;
                    bool forward = autoAirtechDirection.load() == 0;
                    uint8_t horz = 0;
                    if (forward) horz = facingRight ? 1 : 255; else horz = facingRight ? 255 : 1;
                    const char* dirStr = forward ? "FORWARD" : "BACKWARD";
                    LogOut(std::string("[AUTO-AIRTECH] P2 delay expired -> injecting A+dir (dir=") + dirStr +
               ", facingRight=" + (facingRight ? "1" : "0") + ", horz=" + std::to_string(horz) + ") attempt=" + std::to_string(p2AttemptCount),
               true);
                }
            }
            p2WasAirtechable = p2Airtechable;
        }
    }

    // Instant-mode or active inject windows: perform immediate input writes
    if (autoAirtechEnabled.load()) {
    // If delay is zero, start inject window only on the edge when becoming airtechable
    if (autoAirtechDelay.load() == 0 && !p1IsAirteching && p1InjectRemaining == 0 && p1Armed) {
            p1PreNeutral = kPreNeutralFrames;
            p1InjectRemaining = kInjectHoldFramesInitial;
        p1Armed = false;
        p1AttemptCount++;
        bool facingRight = p1FacingRightNow;
            bool forward = autoAirtechDirection.load() == 0;
            uint8_t horz = 0;
            if (forward) horz = facingRight ? 1 : 255; else horz = facingRight ? 255 : 1;
            const char* dirStr = forward ? "FORWARD" : "BACKWARD";
            LogOut(std::string("[AUTO-AIRTECH] P1 instant inject -> A+dir (dir=") + dirStr +
                   ", facingRight=" + (facingRight ? "1" : "0") + ", horz=" + std::to_string(horz) + ") attempt=" + std::to_string(p1AttemptCount),
                   detailedLogging.load());
        }
    if (autoAirtechDelay.load() == 0 && !p2IsAirteching && p2InjectRemaining == 0 && p2Armed) {
            p2PreNeutral = kPreNeutralFrames;
            p2InjectRemaining = kInjectHoldFramesInitial;
        p2Armed = false;
        p2AttemptCount++;
        bool facingRight = p2FacingRightNow;
            bool forward = autoAirtechDirection.load() == 0;
            uint8_t horz = 0;
            if (forward) horz = facingRight ? 1 : 255; else horz = facingRight ? 255 : 1;
            const char* dirStr = forward ? "FORWARD" : "BACKWARD";
            LogOut(std::string("[AUTO-AIRTECH] P2 instant inject -> A+dir (dir=") + dirStr +
                   ", facingRight=" + (facingRight ? "1" : "0") + ", horz=" + std::to_string(horz) + ") attempt=" + std::to_string(p2AttemptCount),
                   detailedLogging.load());
        }

        // Drive injections if windows are active
        // P1 state machine
        if (p1PreNeutral > 0) {
            // Force neutral (buffered) so a clean edge is generated
            applyOverrideMask(1, 0, true);
            p1PreNeutral--;
            p1AttemptActive = false;
        } else if (p1InjectRemaining > 0) {
            if (!p1AttemptActive) {
                uint8_t mask = composeMaskForAttempt(1, p1AttemptCount - 1);
                applyOverrideMask(1, mask, true); // buffered
                p1AttemptActive = true;
            }
            p1InjectRemaining--;
        } else if (p1AttemptActive) {
            // Release after attempt
            g_manualInputOverride[1].store(false);
            g_manualInputMask[1].store(0);
            g_injectImmediateOnly[1].store(false);
            p1AttemptActive = false;
            LogOut("[AUTO-AIRTECH] P1 attempt window complete", detailedLogging.load());
            // Schedule next attempt if still airtechable and attempts remain
            if (!p1IsAirteching && IsPlayerAirtechable(moveID1,1) && p1AttemptCount < kMaxRetryAttempts) {
                p1PreNeutral = kPreNeutralFrames;
                p1InjectRemaining = kInjectHoldFramesInitial;
                p1AttemptCount++;
                LogOut("[AUTO-AIRTECH] P1 scheduling retry attempt=" + std::to_string(p1AttemptCount), detailedLogging.load());
            }
        }

        // P2 state machine
        if (p2PreNeutral > 0) {
            applyOverrideMask(2, 0, true);
            p2PreNeutral--;
            p2AttemptActive = false;
        } else if (p2InjectRemaining > 0) {
            if (!p2AttemptActive) {
                uint8_t mask = composeMaskForAttempt(2, p2AttemptCount - 1);
                applyOverrideMask(2, mask, true);
                p2AttemptActive = true;
            }
            p2InjectRemaining--;
        } else if (p2AttemptActive) {
            g_manualInputOverride[2].store(false);
            g_manualInputMask[2].store(0);
            g_injectImmediateOnly[2].store(false);
            p2AttemptActive = false;
            LogOut("[AUTO-AIRTECH] P2 attempt window complete", detailedLogging.load());
            if (!p2IsAirteching && IsPlayerAirtechable(moveID2,2) && p2AttemptCount < kMaxRetryAttempts) {
                p2PreNeutral = kPreNeutralFrames;
                p2InjectRemaining = kInjectHoldFramesInitial;
                p2AttemptCount++;
                LogOut("[AUTO-AIRTECH] P2 scheduling retry attempt=" + std::to_string(p2AttemptCount), detailedLogging.load());
            }
        }
    } else {
        // If disabled mid-window, ensure we release any active overrides cleanly
        if (p1WasInjecting || g_manualInputOverride[1].load()) {
            g_manualInputOverride[1].store(false);
            g_manualInputMask[1].store(0);
            g_injectImmediateOnly[1].store(false);
            p1WasInjecting = false;
            p1InjectRemaining = 0;
            p1DelayCounter = 0;
            p1Armed = false;
            p1AttemptCount = 0;
            p1PreNeutral = 0;
            p1AttemptActive = false;
        }
        if (p2WasInjecting || g_manualInputOverride[2].load()) {
            g_manualInputOverride[2].store(false);
            g_manualInputMask[2].store(0);
            g_injectImmediateOnly[2].store(false);
            p2WasInjecting = false;
            p2InjectRemaining = 0;
            p2DelayCounter = 0;
            p2Armed = false;
            p2AttemptCount = 0;
            p2PreNeutral = 0;
            p2AttemptActive = false;
        }
    }
}

// Helper to read the input buffer and current index for a player
/*bool ReadPlayerInputBuffer(int playerNum, uint8_t* outBuffer, int bufferLen, int& outCurrentIndex) {
    uintptr_t base = GetEFZBase();
    uintptr_t playerPtr = 0;
    uintptr_t baseOffset = (playerNum == 1) ? EFZ_BASE_OFFSET_P1 : EFZ_BASE_OFFSET_P2;
    if (!SafeReadMemory(base + baseOffset, &playerPtr, sizeof(uintptr_t)) || !playerPtr)
        return false;
    // Read buffer (0x1AB, length 0x180)
    if (!SafeReadMemory(playerPtr + 0x1AB, outBuffer, bufferLen))
        return false;
    // Read current index (0x260, 2 bytes)
    uint16_t idx = 0;
    if (!SafeReadMemory(playerPtr + 0x260, &idx, sizeof(uint16_t)))
        return false;
    outCurrentIndex = idx;
    return true;
}*/