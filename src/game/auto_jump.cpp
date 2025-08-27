#include "../include/game/auto_jump.h"
#include "../include/game/auto_action.h"
#include "../include/core/constants.h"
#include "../include/utils/utilities.h"

#include "../include/core/memory.h"
#include "../include/core/logger.h"
#include "../include/input/input_motion.h"
#include <chrono>

void ApplyJump(uintptr_t moveIDAddr, int playerNum, int jumpType) {
    // Ignore moveIDAddr parameter - we won't use it anymore
    using clock = std::chrono::steady_clock;
    static clock::time_point lastLogAt[3] = { clock::time_point(), clock::time_point(), clock::time_point() };
    const auto now = clock::now();
    const auto logInterval = std::chrono::seconds(2);
    bool shouldLog = (!detailedLogging.load()) ? false : (lastLogAt[playerNum] == clock::time_point() || (now - lastLogAt[playerNum]) >= logInterval);
    if (shouldLog) {
        LogOut("[AUTO-JUMP] Executing jump via input system for P" + std::to_string(playerNum), true);
        lastLogAt[playerNum] = now;
    }

    std::string jumpTypeName;
    uint8_t inputMask = MOTION_INPUT_UP; // Default to straight jump (UP)

    // Determine direction dynamically; prefer relative position to opponent to avoid a stray backward jump after side switch
    // Cache X position addresses to avoid repeated ResolvePointer calls
    static uintptr_t cachedBase = 0;
    static uintptr_t xAddr[3] = { 0, 0, 0 }; // [1]=P1, [2]=P2
    {
        uintptr_t baseNow = GetEFZBase();
        if (baseNow != 0 && baseNow != cachedBase) {
            cachedBase = baseNow;
            xAddr[1] = ResolvePointer(baseNow, EFZ_BASE_OFFSET_P1, XPOS_OFFSET);
            xAddr[2] = ResolvePointer(baseNow, EFZ_BASE_OFFSET_P2, XPOS_OFFSET);
        }
    }
    auto getXPos = [&](int p) -> double {
        double x = 0.0;
        if (p >= 1 && p <= 2 && xAddr[p]) {
            SafeReadMemory(xAddr[p], &x, sizeof(double));
        }
        return x;
    };
    auto forwardRightFor = [&](int p) -> bool {
        int opp = (p == 1 ? 2 : 1);
        double selfX = getXPos(p);
        double oppX = getXPos(opp);
        if (fabs(selfX - oppX) > 0.5) {
            return oppX > selfX; // forward is toward opponent
        }
        // Fallback to facing flag if positions are ambiguous
        return GetPlayerFacingDirection(p);
    };

    switch (jumpType) {
        case 0: // Straight
            inputMask = MOTION_INPUT_UP;
            jumpTypeName = "straight";
            break;
        case 1: // Forward
            if (forwardRightFor(playerNum)) {
                inputMask = MOTION_INPUT_UP | MOTION_INPUT_RIGHT;
            } else {
                inputMask = MOTION_INPUT_UP | MOTION_INPUT_LEFT;
            }
            jumpTypeName = "forward";
            break;
        case 2: // Backward
            if (forwardRightFor(playerNum)) {
                inputMask = MOTION_INPUT_UP | MOTION_INPUT_LEFT;
            } else {
                inputMask = MOTION_INPUT_UP | MOTION_INPUT_RIGHT;
            }
            jumpTypeName = "backward";
            break;
        default:
            inputMask = MOTION_INPUT_UP;
            jumpTypeName = "straight (default)";
            break;
    }

    // Engage immediate-only manual override; release is handled by MonitorAutoJump timing
    g_injectImmediateOnly[playerNum].store(true);
    g_manualInputMask[playerNum].store(inputMask);
    g_manualInputOverride[playerNum].store(true);

    if (shouldLog) {
        LogOut("[AUTO-JUMP] Holding " + jumpTypeName + " jump input for P" +
               std::to_string(playerNum) + " (immediate-only)", true);
    }
}

// Add this function to check if auto-action is active
bool IsAutoActionActiveForPlayer(int playerNum) {
    if (!autoActionEnabled.load()) {
        return false;
    }
    
    int targetPlayer = autoActionPlayer.load(); // 1=P1, 2=P2, 3=Both
    bool affectsThisPlayer = (targetPlayer == playerNum || targetPlayer == 3);
    
    if (!affectsThisPlayer) {
        return false;
    }
    
    // Check if any triggers are enabled
    bool anyTriggerEnabled = triggerAfterBlockEnabled.load() || 
                            triggerOnWakeupEnabled.load() || 
                            triggerAfterHitstunEnabled.load() || 
                            triggerAfterAirtechEnabled.load();
    
    if (!anyTriggerEnabled) {
        return false;
    }
    
    // Check if this player has active delays or applied actions
    if (playerNum == 1) {
        return p1DelayState.isDelaying || p1ActionApplied;
    } else if (playerNum == 2) {
        return p2DelayState.isDelaying || p2ActionApplied;
    }
    
    return false;
}

void MonitorAutoJump() {
    using clock = std::chrono::steady_clock;
    static clock::time_point holdUntil[3] = { clock::time_point(), clock::time_point(), clock::time_point() };
    static clock::time_point nextApplyAt[3] = { clock::time_point(), clock::time_point(), clock::time_point() };

    auto releaseIfOurJump = [](int p) {
        uint8_t mask = g_manualInputMask[p].load();
        bool looksLikeJump = (mask & MOTION_INPUT_UP) && ((mask & MOTION_INPUT_BUTTON) == 0);
        if (g_manualInputOverride[p].load() && g_injectImmediateOnly[p].load() && looksLikeJump) {
            g_manualInputOverride[p].store(false);
            g_manualInputMask[p].store(0);
            g_injectImmediateOnly[p].store(false);
        }
    };

    // If disabled, release any jump we own and reset timers
    if (!autoJumpEnabled.load()) {
        releaseIfOurJump(1);
        releaseIfOurJump(2);
        holdUntil[1] = holdUntil[2] = clock::time_point();
        nextApplyAt[1] = nextApplyAt[2] = clock::time_point();
        return;
    }

    // Micro-throttle: avoid heavy work more than once every ~8 ms
    static clock::time_point lastRun = clock::time_point();
    const auto now = clock::now();
    if (lastRun != clock::time_point() && (now - lastRun) < std::chrono::milliseconds(8)) {
        // Still allow releases if our hold expired
        for (int p = 1; p <= 2; ++p) {
            if (g_manualInputOverride[p].load() && now >= holdUntil[p]) {
                uint8_t mask = g_manualInputMask[p].load();
                bool looksLikeJump = (mask & MOTION_INPUT_UP) && ((mask & MOTION_INPUT_BUTTON) == 0);
                if (g_injectImmediateOnly[p].load() && looksLikeJump) {
                    g_manualInputOverride[p].store(false);
                    g_manualInputMask[p].store(0);
                    g_injectImmediateOnly[p].store(false);
                }
            }
        }
        return;
    }

    // We no longer need game addresses here; ApplyJump ignores the address argument.
    uintptr_t moveIDAddr1 = 0;
    uintptr_t moveIDAddr2 = 0;

    int targetPlayer = jumpTarget.load();
    int direction = jumpDirection.load();

    // 'now' already computed above
    const auto holdDuration = std::chrono::milliseconds(50); // ~3 visual frames
    const auto reapplyInterval = std::chrono::milliseconds(80); // small spacing to avoid constant hold

    // Apply every frame; extend hold window for targeted players
    // P1: prioritize auto-actions; if busy, ensure our manual override is released
    if (targetPlayer == 1 || targetPlayer == 3) {
        if (IsAutoActionActiveForPlayer(1)) {
            releaseIfOurJump(1);
        } else if (!g_manualInputOverride[1].load()) {
            if (nextApplyAt[1] == clock::time_point() || now >= nextApplyAt[1]) {
                ApplyJump(moveIDAddr1, 1, direction);
                holdUntil[1] = now + holdDuration;
                nextApplyAt[1] = now + reapplyInterval;
            }
        }
    }

    // P2: prioritize auto-actions; if busy, ensure our manual override is released
    if (targetPlayer == 2 || targetPlayer == 3) {
        if (IsAutoActionActiveForPlayer(2)) {
            releaseIfOurJump(2);
        } else if (!g_manualInputOverride[2].load()) {
            if (nextApplyAt[2] == clock::time_point() || now >= nextApplyAt[2]) {
                ApplyJump(moveIDAddr2, 2, direction);
                holdUntil[2] = now + holdDuration;
                nextApplyAt[2] = now + reapplyInterval;
            }
        }
    }

    // Release overrides when their hold windows elapse
    for (int p = 1; p <= 2; ++p) {
        if (g_manualInputOverride[p].load() && now >= holdUntil[p]) {
            // Only release if it's our jump hold
            uint8_t mask = g_manualInputMask[p].load();
            bool looksLikeJump = (mask & MOTION_INPUT_UP) && ((mask & MOTION_INPUT_BUTTON) == 0);
            if (g_injectImmediateOnly[p].load() && looksLikeJump) {
                g_manualInputOverride[p].store(false);
                g_manualInputMask[p].store(0);
                g_injectImmediateOnly[p].store(false);
            }
        }
    }
    lastRun = now;
}

void AutoJumpReleaseForPlayer(int playerNum) {
    if (playerNum < 1 || playerNum > 2) return;
    uint8_t mask = g_manualInputMask[playerNum].load();
    bool looksLikeJump = (mask & MOTION_INPUT_UP) && ((mask & MOTION_INPUT_BUTTON) == 0);
    if (g_manualInputOverride[playerNum].load() && g_injectImmediateOnly[playerNum].load() && looksLikeJump) {
        g_manualInputOverride[playerNum].store(false);
        g_manualInputMask[playerNum].store(0);
        g_injectImmediateOnly[playerNum].store(false);
    }
}