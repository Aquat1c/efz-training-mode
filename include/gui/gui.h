#pragma once
#include "../utils/utilities.h"

void OpenMenu();

// Opens the ordinary Practice configuration UI without the mission/lesson/
// recorder routing performed by OpenMenu().  Single chokepoint for every live
// opener, so the gameplay-context gate lives here; pass allowOutOfContext=true
// only from a pause surface that owns the screen during setup/restore.
bool OpenPracticeMenuDirect(bool allowOutOfContext = false);
