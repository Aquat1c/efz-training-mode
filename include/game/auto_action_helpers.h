#pragma once
#include <string>

// Helper function declarations
int GetSpecialMoveStrength(int actionType, int triggerType);
std::string GetTriggerName(int triggerType);
int ConvertTriggerActionToMotion(int actionType, int triggerType); // Renamed function