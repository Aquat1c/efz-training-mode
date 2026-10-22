// Doppel's command-throw follow-ups are EFZ's only techable throw. The engine
// escapes via a latch DWORD on Doppel's own struct (DOPPEL_TECH_LATCH_OFFSET),
// filled from the OPPONENT's rising-edge button bytes only while the latch is
// zero ("first press wins").
//
// This module drives that latch as a CONDITIONAL POLL with AT MOST ONE DECISION
// PER TECH STAGE ENTRY - never an unconditional per-frame write, never a code
// patch. The latch offset is generic per-move scratch outside the four follow-up
// states, so the character gate and a freshly read moveID gate are both
// mandatory. The write is deferred to the animation frame index at which the
// engine itself samples the opponent: the engine re-arms (zeroes) the latch a
// couple of frames INTO states 255/300/306, wiping an eager write. The
// latch-is-zero guard at that point preserves first-press-wins.
// Behaviour, not a stored engine value; deliberately outside
// CharacterSettings::ApplyCharacterValues / TickCharacterEnforcements.
#pragma once

namespace DoppelTech {

// Values of the FOLLOW-UP TECH row (DisplayData::pNDoppelTechMode).
enum Mode {
    MODE_OFF    = 0,  // do not touch the latch at all
    MODE_NEVER  = 1,  // latch A, which escapes nothing
    MODE_TECH_B = 2,  // latch B, which escapes the B-branch follow-ups
    MODE_TECH_C = 3,  // latch C, which escapes the A-branch follow-ups
    MODE_ALWAYS = 4,  // follow whichever branch Doppel actually committed to
    MODE_RANDOM = 5,  // uniform over B / C, rolled once per stage entry
    MODE_COUNT  = 6
};

// Values of the TECH STAGE row (DisplayData::pNDoppelTechStage).
enum Stage {
    STAGE_ALL = 0,
    STAGE_1   = 1,  // the capture hold
    STAGE_2   = 2,  // Maiden Crash
    STAGE_3   = 3,  // Barrage / Inner-Soul Fist
    STAGE_COUNT = 4
};

// Set from the GUI apply path (ImGuiGui::ApplyImGuiSettings); playerNum is 1
// or 2 and names the side Doppel is on.
void SetMode(int playerNum, int mode);
void SetStage(int playerNum, int stage);
int  GetMode(int playerNum);
int  GetStage(int playerNum);

// Per-tick poll from the frame monitor; reads Doppel's move ID, animation
// frame index and subframe itself, because the engine can have advanced since
// values sampled earlier in the monitor iteration.
void Tick();

// Drops per-side arming state on lifecycle changes / mode exits.
void ResetState();

} // namespace DoppelTech
