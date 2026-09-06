#pragma once
#include "../utils/utilities.h"

void OpenMenu();

// Opens the ordinary Practice configuration UI without applying the
// mission/lesson/recorder routing performed by OpenMenu(). Session pause
// surfaces use this for an explicit nested "Practice settings" row.
// Returns true only when a menu surface is visible afterwards.
//
// This is the single chokepoint every live opener funnels through (keyboard
// hotkey, gamepad hotkey, the battle-hotkeys ESC hook, and both the in-game
// and external-fallback render backends), so the gameplay-context gate lives
// here. Pass allowOutOfContext=true only from a mission/lesson pause surface
// that legitimately owns the screen during a setup or restore window.
bool OpenPracticeMenuDirect(bool allowOutOfContext = false);
