#include "../include/auto_action.h"
#include "../include/constants.h"
#include "../include/utilities.h"
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