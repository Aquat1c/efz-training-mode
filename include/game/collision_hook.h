#pragma once
#include <cstdint>

// Installs a hook on the game's handlePlayerToPlayerCollision function to capture
// the live attackerFrameData pointer each time collisions are processed.
void InstallCollisionHook();
void RemoveCollisionHook();

// Returns the last seen attackerFrameData pointer for the given player (1 or 2).
// May return 0 if not yet observed.
uintptr_t GetCachedAttackDataForPlayer(int playerNum);

// Returns the discovered offset (in bytes) from the player base pointer to the
// field that holds the current attack data pointer, or -1 if not yet found.
int GetAttackDataOffsetForPlayer(int playerNum);
