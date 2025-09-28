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
    // Use explicit strength selection based on trigger type
    switch (triggerType) {
        case TRIGGER_AFTER_BLOCK:
            return triggerAfterBlockStrength.load();
        
        case TRIGGER_ON_WAKEUP:
            return triggerOnWakeupStrength.load();
        
        case TRIGGER_AFTER_HITSTUN:
            return triggerAfterHitstunStrength.load();
        
        case TRIGGER_AFTER_AIRTECH:
            return triggerAfterAirtechStrength.load();
        case TRIGGER_ON_RG:
            return triggerOnRGStrength.load();
        
        default:
            // Default to medium (B) strength if no specific trigger
            return 1;
    }
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
        case TRIGGER_ON_RG:
            return "On RG";
        default:
            return "Unknown Trigger";
    }
}

int ConvertTriggerActionToMotion(int actionType, int triggerType) {
    // Get the strength for this trigger (0=A, 1=B, 2=C)
    int strength = GetSpecialMoveStrength(actionType, triggerType);

    switch (actionType) {
        // Normal attacks (unchanged)
        case ACTION_5A: return MOTION_5A;
        case ACTION_5B: return MOTION_5B;
        case ACTION_5C: return MOTION_5C;
        case ACTION_2A: return MOTION_2A;
        case ACTION_2B: return MOTION_2B;
        case ACTION_2C: return MOTION_2C;
        case ACTION_JA: return MOTION_JA;
        case ACTION_JB: return MOTION_JB;
        case ACTION_JC: return MOTION_JC;

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

        // Half-circle down (421)
        case ACTION_421:
            if (strength == 1) return MOTION_421B;
            if (strength == 2) return MOTION_421C;
            return MOTION_421A;

        // Half-circle forward (41236)
        case ACTION_SUPER1:
            if (strength == 1) return MOTION_41236B;
            if (strength == 2) return MOTION_41236C;
            return MOTION_41236A;

        // Previously Half-circle back (63214) removed. Remapped ACTION_SUPER2 to 214236 hybrid
        case ACTION_SUPER2:
            if (strength == 1) return MOTION_214236B;
            if (strength == 2) return MOTION_214236C;
            return MOTION_214236A;

        // Double QCF (236236)
        case ACTION_236236:
            if (strength == 1) return MOTION_236236B;
            if (strength == 2) return MOTION_236236C;
            return MOTION_236236A;

        // Double QCB (214214)
        case ACTION_214214:
            if (strength == 1) return MOTION_214214B;
            if (strength == 2) return MOTION_214214C;
            return MOTION_214214A;

        // 641236 Super
        case ACTION_641236:
            if (strength == 1) return MOTION_641236B;
            if (strength == 2) return MOTION_641236C;
            return MOTION_641236A;

        case ACTION_463214:
            if (strength == 1) return MOTION_463214B;
            if (strength == 2) return MOTION_463214C;
            return MOTION_463214A;

        case ACTION_412:
            if (strength == 1) return MOTION_412B;
            if (strength == 2) return MOTION_412C;
            return MOTION_412A;

        case ACTION_22:
            if (strength == 1) return MOTION_22B;
            if (strength == 2) return MOTION_22C;
            return MOTION_22A;

        case ACTION_4123641236:
            if (strength == 1) return MOTION_4123641236B;
            if (strength == 2) return MOTION_4123641236C;
            return MOTION_4123641236A;

        case ACTION_6321463214:
            if (strength == 1) return MOTION_6321463214B;
            if (strength == 2) return MOTION_6321463214C;
            return MOTION_6321463214A;

    // Other actions
    // For Jump, we handle immediate input injection elsewhere; no motion mapping needed
    case ACTION_JUMP: return MOTION_NONE;
    case ACTION_BACKDASH: return MOTION_BACK_DASH;       // explicit back dash motion token
    case ACTION_FORWARD_DASH: return MOTION_FORWARD_DASH; // explicit forward dash motion token
        case ACTION_BLOCK: return MOTION_NONE;
    case ACTION_FINAL_MEMORY: return MOTION_NONE; // Handled by FM subsystem

        default:
            LogOut("[AUTO-ACTION] WARNING: Unknown action type " + std::to_string(actionType) + 
                  ", defaulting to 5A", true);
            return MOTION_5A;
    }
}