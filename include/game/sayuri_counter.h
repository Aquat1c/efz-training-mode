// Sayuri counter memory and Magical Cutter. Her 214 counter stance has no
// attack box; when an attack lands on a counter frame the engine copies the
// OPPONENT's move ID into SAYURI_COUNTER_MEMORY_OFFSET and throws them. On
// later GROUNDED blockstun entry it compares the opponent's live move ID
// against that value; on a match it sets SAYURI_CUTTER_ARMED_OFFSET, flashes
// her white, and A/B/C cancels the blockstun into Magical Cutter.
//
// This module drives both fields as a CONDITIONAL POLL - not an unconditional
// per-frame write, not a code patch, not input injection. The memory offset is
// the engine's shared per-character value slot (Ikumi genocide timer, Akiko
// bullet cycle, Mio stance, Kano magic, Nayuki-B snowbunny timer, Mai summon
// flash on other characters), so the character gate is mandatory; the armed
// flag is generic per-move scratch in Sayuri's own handler and is also the
// field the Doppel module owns, so it carries BOTH the character gate and a
// freshly read move-ID gate. LAST BLOCKED writes during blockstun and takes
// effect on the NEXT blockstun entry (the engine compares on the entry frame),
// which is race-free by construction.
// Behaviour, not a stored engine value; deliberately outside
// CharacterSettings::ApplyCharacterValues / TickCharacterEnforcements.
#pragma once

namespace SayuriCounter {

// Fixed entries at the head of the REMEMBERED MOVE row; everything at or above
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

// Values of the AUTO CUTTER row (DisplayData::pNSayuriAutoCutter). ON presses
// A on the first fighter pass of an open window - armed, her own blockstop
// over, no opponent superflash - through the auto-action normal pulse, so it
// lands after EFZ's input producer and before her character consumer in
// either controller mode. It is the only way a CPU dummy Sayuri ever cuts out.
enum AutoCutterMode {
    AUTO_CUTTER_OFF   = 0,
    AUTO_CUTTER_ON    = 1,
    AUTO_CUTTER_COUNT = 2
};

// ---------------------------------------------------------------------------
// Per-opponent picker list, rebuilt from the opponent's move catalogue on
// opponent change. Owned by the menu/render thread; Tick() works only from the
// resolved move ID published through SetMemory().
// ---------------------------------------------------------------------------

// No-op unless the opponent changed (or a popup is open); cheap enough for a
// per-frame row builder.
void RefreshMoveChoices();

// playerNum is 1 or 2 and names the side SAYURI is on.
const char* const* MoveChoiceItems(int playerNum);
int  MoveChoiceCount(int playerNum);
// Move ID behind a picker index; 0 for the fixed entries and out-of-range
// indices.
int  MoveIdForChoice(int playerNum, int idx);
// False when this opponent has no move list (boss UNKNOWN, an uncovered
// character, or characters not initialised yet).
bool OpponentListAvailable(int playerNum);
bool MoveListTruncated(int playerNum);
// Anything that no longer names a move on the CURRENT opponent collapses to
// MEMORY_OFF rather than pinning something else by accident.
int  ValidateChoiceIndex(int playerNum, int idx);

// The index is presentation only; the module stores the resolved move ID, so
// an index that means "5A" against one opponent can never silently mean a
// different move against the next.
void SetMemory(int playerNum, int choiceIndex, int resolvedMoveId);
void SetCutter(int playerNum, int mode);
void SetAutoCutter(int playerNum, int mode);
// AUTO CUTTER reaction delay in VISUAL frames (0..kAutoCutterDelayMax),
// counted from the first monitor tick with the cut-out window open. 0 is the
// earliest press the engine accepts; the engine has no later-than-first-tick
// rule, so any value above 0 models the player, not the game.
void SetAutoCutterDelay(int playerNum, int visualFrames);
int  GetMemory(int playerNum);
int  GetCutter(int playerNum);
int  GetAutoCutter(int playerNum);
int  GetAutoCutterDelay(int playerNum);
constexpr int kAutoCutterDelayMax = 60;

// Per-tick poll from the frame monitor; reads Sayuri's move ID itself rather
// than a value sampled earlier in the monitor iteration.
void Tick();

void ResetState();

} // namespace SayuriCounter
