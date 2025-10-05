#pragma once
#include <string>

// Best-effort friendly controller name for an XInput user index (0..3).
// Returns a non-empty descriptive name when possible; otherwise a fallback like "Controller N".
std::string GetControllerNameForIndex(int userIndex);
