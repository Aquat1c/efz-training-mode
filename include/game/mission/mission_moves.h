#pragma once
//
// Move-ID -> notation table for combo missions.
//
// Entries are either:
//   * ACTION  - attacks / specials / supers / IC. Always a combo step ("land").
//   * MOVEMENT- jumps / dashes. Only a combo step when performed mid-combo
//               (jump-cancel / IAD / dash-cancel), so neutral movement is
//               ignored. Requirement is "move" (perform, no hit).
//
// Universal move-IDs (same across characters) come from our existing
// core/constants.h names: base normals 200-209, jumps (4/5/6/14-16), dash (163),
// IC (167/171). Per-character specials (Mizuka 623B=251, family pattern
// 623/236/214/412 in A/B/C order, supers 300+) are NOT globally tabled - each
// mission stores its own step moveIds+notation (recorder-captured, editor-fixed).

namespace Mission::Moves {

// Notation for a known move-ID, or "" if unknown.
const char* Notation(int moveId);

// In the table at all (action or movement).
bool IsKnownComboMove(int moveId);

// A movement move (jump/dash): only counted as a step while a combo is active.
bool IsMovementMove(int moveId);

} // namespace Mission::Moves
