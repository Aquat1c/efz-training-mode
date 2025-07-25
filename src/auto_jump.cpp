#include "../include/auto_jump.h"
#include "../include/auto_action.h"
#include "../include/constants.h"
#include "../include/utilities.h"
#include "../include/memory.h"
#include "../include/logger.h"


void ApplyJump(uintptr_t moveIDAddr, int playerNum, int jumpType) {
    if (!moveIDAddr) {
        LogOut("[AUTO-JUMP] Invalid moveID address for P" + std::to_string(playerNum), true);
        return;
    }
    
    short jumpMoveID;
    std::string jumpTypeName;
    
    switch (jumpType) {
        case 0: // Straight
            jumpMoveID = STRAIGHT_JUMP_ID;
            jumpTypeName = "straight";
            break;
        case 1: // Forward
            jumpMoveID = FORWARD_JUMP_ID;
            jumpTypeName = "forward";
            break;
        case 2: // Backward
            jumpMoveID = BACKWARD_JUMP_ID;
            jumpTypeName = "backward";
            break;
        default:
            jumpMoveID = STRAIGHT_JUMP_ID;
            jumpTypeName = "straight (default)";
            break;
    }
    
    // Apply the jump moveID directly without excessive logging
    if (SafeWriteMemory(moveIDAddr, &jumpMoveID, sizeof(short))) {
        // Only log in detailed mode
        if (detailedLogging.load()) {
            LogOut("[AUTO-JUMP] P" + std::to_string(playerNum) + " " + jumpTypeName + " jump applied", false);
        }
    } else {
        // Only log failures
        LogOut("[AUTO-JUMP] Failed to apply jump for P" + std::to_string(playerNum), true);
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
    if (!autoJumpEnabled.load()) {
        return;
    }

    uintptr_t base = GetEFZBase();
    if (!base) return;

    uintptr_t moveIDAddr1 = ResolvePointer(base, EFZ_BASE_OFFSET_P1, MOVE_ID_OFFSET);
    uintptr_t moveIDAddr2 = ResolvePointer(base, EFZ_BASE_OFFSET_P2, MOVE_ID_OFFSET);

    if (!moveIDAddr1 || !moveIDAddr2) {
        return;
    }

    short moveID1 = 0, moveID2 = 0;
    SafeReadMemory(moveIDAddr1, &moveID1, sizeof(short));
    SafeReadMemory(moveIDAddr2, &moveID2, sizeof(short));

    int targetPlayer = jumpTarget.load();
    int direction = jumpDirection.load();

    static int p1JumpCooldown = 0;
    static int p2JumpCooldown = 0;
    static short p1LastMoveID = -1;
    static short p2LastMoveID = -1;

    // P1 auto-jump logic
    if ((targetPlayer == 1 || targetPlayer == 3) && p1JumpCooldown <= 0) {
        bool wasNotActionable = !IsActionable(p1LastMoveID);
        bool isNowActionable = IsActionable(moveID1);
        bool justBecameActionable = wasNotActionable && isNowActionable;

        bool isGrounded = true;
        uintptr_t yAddr1 = ResolvePointer(base, EFZ_BASE_OFFSET_P1, YPOS_OFFSET);
        uintptr_t yVelAddr1 = ResolvePointer(base, EFZ_BASE_OFFSET_P1, YVEL_OFFSET);
        if (yAddr1 && yVelAddr1) {
            double yPos = 0.0, yVel = 0.0;
            SafeReadMemory(yAddr1, &yPos, sizeof(double));
            SafeReadMemory(yVelAddr1, &yVel, sizeof(double));
            isGrounded = (yPos <= 0.1 && fabs(yVel) < 0.1);
        }

        bool notStuckInLanding = !(moveID1 == LANDING_ID && p1LastMoveID == LANDING_ID);

        if (justBecameActionable && notStuckInLanding && isGrounded && !IsAutoActionActiveForPlayer(1)) {
            ApplyJump(moveIDAddr1, 1, direction);
            p1JumpCooldown = 60;
            LogOut("[AUTO-JUMP] Applied jump for P1 (direction: " + std::to_string(direction) + ")", true);
        }
    }

    // P2 auto-jump logic
    if ((targetPlayer == 2 || targetPlayer == 3) && p2JumpCooldown <= 0) {
        bool wasNotActionable = !IsActionable(p2LastMoveID);
        bool isNowActionable = IsActionable(moveID2);
        bool justBecameActionable = wasNotActionable && isNowActionable;

        bool isGrounded = true;
        uintptr_t yAddr2 = ResolvePointer(base, EFZ_BASE_OFFSET_P2, YPOS_OFFSET);
        uintptr_t yVelAddr2 = ResolvePointer(base, EFZ_BASE_OFFSET_P2, YVEL_OFFSET);
        if (yAddr2 && yVelAddr2) {
            double yPos = 0.0, yVel = 0.0;
            SafeReadMemory(yAddr2, &yPos, sizeof(double));
            SafeReadMemory(yVelAddr2, &yVel, sizeof(double));
            isGrounded = (yPos <= 0.1 && fabs(yVel) < 0.1);
        }

        bool notStuckInLanding = !(moveID2 == LANDING_ID && p2LastMoveID == LANDING_ID);

        if (justBecameActionable && notStuckInLanding && isGrounded && !IsAutoActionActiveForPlayer(2)) {
            ApplyJump(moveIDAddr2, 2, direction);
            p2JumpCooldown = 60;
        }
    }

    p1LastMoveID = moveID1;
    p2LastMoveID = moveID2;

    if (p1JumpCooldown > 0) p1JumpCooldown--;
    if (p2JumpCooldown > 0) p2JumpCooldown--;
}