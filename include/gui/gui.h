#pragma once
#include "../utils/utilities.h"

void OpenMenu();

// Opens the ordinary Practice configuration UI without applying the
// mission/lesson/recorder routing performed by OpenMenu(). Session pause
// surfaces use this for an explicit nested "Practice settings" row.
// Returns true only when a menu surface is visible afterwards.
bool OpenPracticeMenuDirect();
