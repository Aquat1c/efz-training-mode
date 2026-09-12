// Doppel Nanase (ExNanase) - Maiden Capture follow-up tech control.
//
// Doppel's command-throw follow-ups are the only techable throw in EFZ. The
// engine decides whether a follow-up is escaped by reading a small latch DWORD
// on DOPPEL's own player struct (DOPPEL_TECH_LATCH_OFFSET). The engine itself
// fills that latch from the OPPONENT's rising-edge button bytes, but only while
// the latch is still zero - that is the "first press wins" rule.
//
// This module drives that latch so the opponent behaves predictably in Practice.
// It is deliberately a CONDITIONAL POLL that makes AT MOST ONE DECISION PER TECH
// STAGE ENTRY, never an unconditional per-frame write and never a code patch:
//
//   * the latch offset is generic per-move scratch outside the four follow-up
//     states, so the character gate and a freshly read moveID gate are both
//     mandatory;
//   * the write is deferred to the animation frame index at which the engine
//     itself samples the opponent, because the engine re-arms (zeroes) the latch
//     a couple of frames INTO states 255/300/306 - an eager write is wiped;
//   * the "latch is still zero" guard, evaluated at that same point, preserves
//     the engine's first-press-wins semantics, so an opponent who presses first
//     still beats the setting.
//
// This is behaviour, not a stored engine value, so it intentionally does NOT go
// through CharacterSettings::ApplyCharacterValues / TickCharacterEnforcements.
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

// Published from the GUI apply path (ImGuiGui::ApplyImGuiSettings).
// playerNum is 1 or 2 and names the side Doppel is on.
void SetMode(int playerNum, int mode);
void SetStage(int playerNum, int stage);
int  GetMode(int playerNum);
int  GetStage(int playerNum);

// Per-tick poll, driven by the frame monitor. Reads Doppel's move ID, animation
// frame index and subframe itself rather than reusing values sampled earlier in
// the monitor iteration, because the gate on those values is a correctness
// requirement and the engine can have advanced in between.
void Tick();

// Drop all per-side arming state (used on lifecycle changes / mode exits).
void ResetState();

} // namespace DoppelTech
