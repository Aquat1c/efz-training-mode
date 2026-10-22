#pragma once
// Move-ID -> notation table for combo missions.
// ACTION = attack/special/super/IC step ("land"); MOVEMENT = jump/dash step
// ("move", perform without hit; recorder policy scopes it to combo/setup).
// Universal IDs mirror core/constants.h: base normals 200-209, jumps 4/5/6/14-16,
// dash 163, IC 167/171. Per-character specials are not tabled here; each mission
// stores its own step moveIds+notation.

namespace Mission::Moves {

// Notation for a known move-ID, or "" if unknown.
const char* Notation(int moveId);

// In the table at all (action or movement).
bool IsKnownComboMove(int moveId);

// A movement move (jump/dash): recorder policy decides combo/setup scope.
bool IsMovementMove(int moveId);

} // namespace Mission::Moves
