// Sayuri Kurata - counter memory ("Ah, got your skill~") and Magical Cutter.
//
// Her 214 counter stance has no attack box. When an attack lands on a counter
// frame the engine copies the OPPONENT's move ID into a dedicated DWORD on
// SAYURI's own struct (SAYURI_COUNTER_MEMORY_OFFSET) and throws them. From then
// on, whenever she enters GROUNDED blockstun the engine compares the opponent's
// live move ID against that stored value on the entry frame: on a match it sets
// the "armed" flag (SAYURI_CUTTER_ARMED_OFFSET), flashes her white, and A, B or
// C cancels the blockstun into Magical Cutter.
//
// This module drives those two fields so the drill can be set up without having
// to land the counter first. Like doppel_tech.cpp it is a CONDITIONAL POLL, not
// an unconditional per-frame write, not a code patch and not input injection:
//
//   * the memory offset is the engine's shared per-character value slot (it is
//     Ikumi's genocide timer, Akiko's bullet cycle, Mio's stance, Kano's magic,
//     Nayuki-B's snowbunny timer and Mai's summon flash on other characters), so
//     the character gate is mandatory on every access;
//   * the armed flag is generic per-move scratch in Sayuri's own handler and is
//     also the field the Doppel module owns, so it carries BOTH the character
//     gate and a freshly read move-ID gate;
//   * LAST BLOCKED writes during blockstun and therefore takes effect on the
//     NEXT blockstun entry, because the engine compares on the entry frame. That
//     is race-free by construction - it is not a fight with the engine.
//
// This is behaviour, not a stored engine value, so it deliberately does NOT go
// through CharacterSettings::ApplyCharacterValues / TickCharacterEnforcements.
#pragma once

namespace SayuriCounter {

// Fixed entries at the head of the REMEMBERED MOVE row. Everything at or above
// kFirstMoveChoice is one move of the CURRENT opponent, rebuilt per opponent.
enum MemoryChoice {
    MEMORY_OFF          = 0,  // do not touch the memory at all
    MEMORY_NOTHING      = 1,  // hold it empty
    MEMORY_LAST_BLOCKED = 2,  // load whatever she most recently blocked
    kFirstMoveChoice    = 3
};

// Values of the MAGICAL CUTTER row (DisplayData::pNSayuriCutterMode).
enum CutterMode {
    CUTTER_NORMAL       = 0,  // only the remembered move opens the window
    CUTTER_ALWAYS_READY = 1,  // every grounded block opens it
    CUTTER_COUNT        = 2
};

// ---------------------------------------------------------------------------
// Per-opponent picker list.
//
// Rebuilt from the opponent's move catalogue whenever the opponent changes.
// Owned by the menu/render thread: RefreshMoveChoices() and every accessor
// below must be called from there. Tick() never reads them - it works from the
// resolved move ID published through SetMemory().
// ---------------------------------------------------------------------------

// No-op unless the opponent actually changed (or a popup is open). Cheap enough
// to call from a row builder every frame.
void RefreshMoveChoices();

// playerNum is 1 or 2 and names the side SAYURI is on.
const char* const* MoveChoiceItems(int playerNum);
int  MoveChoiceCount(int playerNum);
// Move ID behind a picker index, or 0 for the three fixed entries and for any
// index that is out of range for the current list.
int  MoveIdForChoice(int playerNum, int idx);
// False when this opponent has no move list (boss UNKNOWN, a character the
// catalogue does not cover, or characters not initialised yet).
bool OpponentListAvailable(int playerNum);
// True when the opponent has more moves than the row can show.
bool MoveListTruncated(int playerNum);
// Returns the index the UI should display: anything that no longer names a move
// on the CURRENT opponent collapses to MEMORY_OFF rather than pinning something
// else by accident.
int  ValidateChoiceIndex(int playerNum, int idx);

// Published from the GUI apply path. The index is presentation only; the module
// stores the resolved move ID, so an index that means "5A" against one opponent
// can never silently mean a different move against the next.
void SetMemory(int playerNum, int choiceIndex, int resolvedMoveId);
void SetCutter(int playerNum, int mode);
int  GetMemory(int playerNum);
int  GetCutter(int playerNum);

// Per-tick poll, driven by the frame monitor. Reads Sayuri's move ID itself
// rather than reusing a value sampled earlier in the monitor iteration.
void Tick();

// Drop all per-side state (used on lifecycle changes / mode exits).
void ResetState();

} // namespace SayuriCounter
