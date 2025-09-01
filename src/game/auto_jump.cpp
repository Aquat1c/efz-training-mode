#include "../include/game/auto_jump.h"
#include "../include/game/auto_action.h"
#include "../include/core/constants.h"
#include "../include/utils/utilities.h"

#include "../include/core/memory.h"
#include "../include/core/logger.h"
#include "../include/input/input_motion.h"
#include "../include/input/immediate_input.h"
#include <chrono>
#include <cmath>

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

    // Use centralized 64fps immediate writer
    ImmediateInput::Set(playerNum, inputMask);

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
    // We now hold the UP input continuously while enabled; timers retained for compatibility but not required
    static clock::time_point holdUntil[3] = { clock::time_point(), clock::time_point(), clock::time_point() };
    static clock::time_point nextApplyAt[3] = { clock::time_point(), clock::time_point(), clock::time_point() };

    // Landing-edge handling: we must present a neutral frame between jumps so the game sees a new UP press.
    static bool s_wasGrounded[3] = { true, true, true };
    static int  s_forceNeutralFrames[3] = { 0, 0, 0 }; // when >0, inject neutral for that many monitor ticks

    // Lightweight grounded check using Y position; cache addresses for speed
    auto isGrounded = [](int p) -> bool {
        static uintptr_t s_cachedBase = 0;
        static uintptr_t s_yAddr[3] = { 0, 0, 0 };
        uintptr_t baseNow = GetEFZBase();
        if (baseNow != 0 && baseNow != s_cachedBase) {
            s_cachedBase = baseNow;
            s_yAddr[1] = ResolvePointer(baseNow, EFZ_BASE_OFFSET_P1, YPOS_OFFSET);
            s_yAddr[2] = ResolvePointer(baseNow, EFZ_BASE_OFFSET_P2, YPOS_OFFSET);
        }
        double y = 0.0;
        if (p >= 1 && p <= 2 && s_yAddr[p]) {
            SafeReadMemory(s_yAddr[p], &y, sizeof(double));
        }
        return (y <= 0.1); // grounded when very close to 0
    };

    auto releaseIfOurJump = [](int p) {
        uint8_t mask = ImmediateInput::GetCurrentDesired(p);
        bool looksLikeJump = (mask & MOTION_INPUT_UP) && ((mask & MOTION_INPUT_BUTTON) == 0);
        if (looksLikeJump) {
            ImmediateInput::Clear(p);
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

    // Keep a light throttle to ~192 Hz caller; no extra self-throttle necessary
    const auto now = clock::now();

    // We no longer need game addresses here; ApplyJump ignores the address argument.
    uintptr_t moveIDAddr1 = 0;
    uintptr_t moveIDAddr2 = 0;

    int targetPlayer = jumpTarget.load();
    int direction = jumpDirection.load();

    // 'now' already computed above
    // Continuous-hold behavior: no periodic reapply gaps
    const auto holdDuration = std::chrono::milliseconds(1000); // unused in continuous mode
    const auto reapplyInterval = std::chrono::milliseconds(1000); // unused in continuous mode

    // Apply every frame; extend hold window for targeted players
    // P1: prioritize auto-actions; if busy, ensure our manual override is released
    if (targetPlayer == 1 || targetPlayer == 3) {
        if (IsAutoActionActiveForPlayer(1)) {
            releaseIfOurJump(1);
        } else {
            // Edge-based auto-jump: neutral on landing, then UP on next tick
            bool grounded = isGrounded(1);
            bool landing = grounded && !s_wasGrounded[1];
            s_wasGrounded[1] = grounded;
            uint8_t wantMask = MOTION_INPUT_UP | (direction == 1 ? MOTION_INPUT_RIGHT : (direction == 2 ? MOTION_INPUT_LEFT : 0));
            uint8_t curMask = ImmediateInput::GetCurrentDesired(1);

            if (landing) {
                // Force a brief neutral to create a clean edge
                s_forceNeutralFrames[1] = 1;
            }

            if (s_forceNeutralFrames[1] > 0) {
                // Hold neutral for this tick
                ImmediateInput::Set(1, 0);
                s_forceNeutralFrames[1]--;
            } else {
                // Apply/refresh desired UP mask
                bool needApply = curMask != wantMask || curMask == 0;
                if (needApply) {
                    ApplyJump(moveIDAddr1, 1, direction);
                    holdUntil[1] = now + holdDuration;
                }
            }
        }
    }

    // P2: prioritize auto-actions; if busy, ensure our manual override is released
    if (targetPlayer == 2 || targetPlayer == 3) {
        if (IsAutoActionActiveForPlayer(2)) {
            releaseIfOurJump(2);
        } else {
            bool grounded = isGrounded(2);
            bool landing = grounded && !s_wasGrounded[2];
            s_wasGrounded[2] = grounded;
            uint8_t wantMask = MOTION_INPUT_UP | (direction == 1 ? MOTION_INPUT_RIGHT : (direction == 2 ? MOTION_INPUT_LEFT : 0));
            uint8_t curMask = ImmediateInput::GetCurrentDesired(2);

            if (landing) {
                s_forceNeutralFrames[2] = 1;
            }

            if (s_forceNeutralFrames[2] > 0) {
                ImmediateInput::Set(2, 0);
                s_forceNeutralFrames[2]--;
            } else {
                bool needApply = curMask != wantMask || curMask == 0;
                if (needApply) {
                    ApplyJump(moveIDAddr2, 2, direction);
                    holdUntil[2] = now + holdDuration;
                }
            }
        }
    }

    // In continuous mode we don't auto-release; releases occur when disabled or when auto-action takes over
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