#pragma once
#include <string>

// Best-effort friendly name for an XInput user index (0..3); falls back to "Controller N".
std::string GetControllerNameForIndex(int userIndex);
