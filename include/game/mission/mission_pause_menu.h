#pragma once
//
// Mission/Lesson/Recording pause menu - the ONLY pause surface while a trial,
// tutorial, or active recording capture owns the session. The Practice
// training menu (custom/ImGui menu) is blocked in those contexts: its
// savestates, character tools, macros, and auto-actions would corrupt the
// authored session or splice into the capture (TUTORIAL_MODE_DESIGN.md §3.8).
//
// Context is resolved at open time:
//   trial/mission     : RESUME / RETRY MISSION / WATCH DEMO / RETURN TO MISSIONS
//   tutorial          : RESUME LESSON / RESTART LESSON / WATCH EXAMPLE /
//                       RETURN TO LESSONS
//   count-in          : RESUME / CANCEL COUNTDOWN / DISCARD RECORDING
//   recording         : RESUME / STOP & REVIEW / DISCARD RECORDING
//
// The recorder's PRE-RECORD and REVIEW phases deliberately keep the ordinary
// Practice menu (arranging the start position needs the practice tools; Review
// needs the authoring pane) - gui.cpp's OpenMenu() gate only routes here while
// capture owns the session (CountIn/Recording) or a runner session is active.
// STOP & REVIEW hands off to the Practice menu automatically once the recorder
// reaches Review.
//
// The game is frozen through PauseIntegration while open (same freeze the
// Practice menu uses). Input is polled on the frame-monitor thread (Tick) and
// follows the tutorial UI contract: arrows or configured EFZ directions,
// Enter/EFZ A to confirm, Backspace/EFZ B to cancel, and the ordinary
// Escape/configured controller Menu toggle to resume. Rendering happens per-
// EndScene over the match (Draw) using the custom menu's Layout/Theme
// primitives so it matches the Practice menu visuals.
//
#include <string>

struct ImDrawList;

namespace Mission::PauseMenu {

// True while the pause menu owns the screen.
bool IsOpen();

// Open only succeeds while a runner session or a recording capture is active
// in a match and no demonstration is playing. Tutorial Preparing and the
// terminal DemoSuspended restore handoff are also rejected: startup/restore
// must settle before a pause command can mutate the lesson. Returns false
// otherwise.
bool Open();
void Close();

// gui OpenMenu() routes here for the owning contexts: first press opens, next
// press resumes.
void Toggle();

// Frame-monitor thread: freeze maintenance + menu input while open.
void Tick();

// ---- render-thread ----
bool WantsDraw();
void Draw(ImDrawList* dl);

} // namespace Mission::PauseMenu
