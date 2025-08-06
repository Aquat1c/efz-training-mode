#include "../include/game/auto_action.h"
#include "../include/core/constants.h"
#include "../include/utils/utilities.h"
#include "../include/core/logger.h"
#include "../include/input/input_motion.h"
#include "../include/game/auto_action_helpers.h"
#include "../include/core/memory.h"
#include "../include/game/game_state.h"
#include "../include/game/character_settings.h"
#include "../include/input/motion_constants.h"  // Add this include
#include <vector>
#include <string>

// Returns the button strength to use for special moves based on action and trigger type
int GetSpecialMoveStrength(int actionType, int triggerType) {
    // Default to light strength (A button = 0)
    int strength = 0;
    
    // For most actions, use medium strength (B button = 1)
    if (actionType == ACTION_QCF || 
        actionType == ACTION_DP || 
        actionType == ACTION_QCB) {
        strength = 1;
    }
    
    // For super moves, use heavy strength (C button = 2)
    if (actionType == ACTION_SUPER1 || 
        actionType == ACTION_SUPER2) {
        strength = 2;
    }
    
    // Special case: After Airtech always uses heavy attacks
    if (triggerType == TRIGGER_AFTER_AIRTECH) {
        strength = 2;
    }
    
    // Special case: On Wakeup always uses medium attacks
    if (triggerType == TRIGGER_ON_WAKEUP) {
        strength = 1;
    }
    
    return strength;
}

// Returns a human-readable name for a trigger type
std::string GetTriggerName(int triggerType) {
    switch (triggerType) {
        case TRIGGER_AFTER_BLOCK:
            return "After Block";
        case TRIGGER_ON_WAKEUP:
            return "On Wakeup";
        case TRIGGER_AFTER_HITSTUN:
            return "After Hitstun";
        case TRIGGER_AFTER_AIRTECH:
            return "After Airtech";
        default:
            return "Unknown Trigger";
    }
}

int ConvertTriggerActionToMotion(int actionType, int triggerType) {
    // Log the conversion request
    LogOut("[AUTO-ACTION] Converting action type " + std::to_string(actionType) + 
           " for trigger type " + std::to_string(triggerType), true);

    // 0 = A, 1 = B, 2 = C
    int strength = GetSpecialMoveStrength(actionType, triggerType);

    switch (actionType) {
        // Normals
        case ACTION_5A:    return MOTION_5A;
        case ACTION_5B:    return MOTION_5B;
        case ACTION_5C:    return MOTION_5C;
        case ACTION_2A:    return MOTION_2A;
        case ACTION_2B:    return MOTION_2B;
        case ACTION_2C:    return MOTION_2C;
        case ACTION_JA:    return MOTION_JA;
        case ACTION_JB:    return MOTION_JB;
        case ACTION_JC:    return MOTION_JC;

        // QCF (236)
        case ACTION_QCF:
            if (strength == 1) return MOTION_236B;
            if (strength == 2) return MOTION_236C;
            return MOTION_236A;

        // DP (623)
        case ACTION_DP:
            if (strength == 1) return MOTION_623B;
            if (strength == 2) return MOTION_623C;
            return MOTION_623A;

        // QCB (214)
        case ACTION_QCB:
            if (strength == 1) return MOTION_214B;
            if (strength == 2) return MOTION_214C;
            return MOTION_214A;

        // 421 (HCB Down)
        case ACTION_421:
            if (strength == 1) return MOTION_421B;
            if (strength == 2) return MOTION_421C;
            return MOTION_421A;

        // HCF (41236)
        case ACTION_SUPER1:
            if (strength == 1) return MOTION_41236B;
            if (strength == 2) return MOTION_41236C;
            return MOTION_41236A;

        // HCB (63214)
        case ACTION_SUPER2:
            if (strength == 1) return MOTION_63214B;
            if (strength == 2) return MOTION_63214C;
            return MOTION_63214A;

        // Dashes, jump, block
        case ACTION_JUMP:      return MOTION_JA; // Or a dedicated jump motion if you have one
        case ACTION_BACKDASH:  return ACTION_BACK_DASH;
        case ACTION_FORWARD_DASH: return ACTION_FORWARD_DASH;
        case ACTION_BLOCK:     return MOTION_NONE; // Or implement block input

        default:
            LogOut("[AUTO-ACTION] WARNING: Unknown action type " + std::to_string(actionType) + 
                  ", defaulting to 5A", true);
            return MOTION_5A; // Default to 5A for unknown action types
    }
}