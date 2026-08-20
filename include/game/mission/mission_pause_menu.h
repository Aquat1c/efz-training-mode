#pragma once
//
// Mission/Lesson/Recording pause menu - the ONLY default pause surface while a
// trial, tutorial, or mission-recording session owns the match. Recording
// exposes Practice Settings through an explicit nested row: the dedicated menu
// and physical pause remain underneath, so Back returns here. Authoring Details
// is a separate explicit handoff to the existing metadata/publish workflow.
//
// Context is resolved at open time:
//   trial/mission     : RESUME / RETRY MISSION / WATCH DEMO / RETURN TO MISSIONS
//   tutorial          : RESUME LESSON / RESTART LESSON / WATCH EXAMPLE /
//                       RETURN TO LESSONS
//   pre-record        : RESUME SETUP / START RECORDING / AUTHORING OPTIONS /
//                       PRACTICE SETTINGS / DISCARD RECORDING
//   count-in          : RESUME COUNTDOWN / PRACTICE SETTINGS /
//                       CANCEL COUNTDOWN / DISCARD RECORDING
//   recording         : RESUME RECORDING / PRACTICE SETTINGS /
//                       STOP & REVIEW / DISCARD RECORDING
//   review            : RETURN TO MATCH / REVIEW & SAVE / PREVIEW DEMO /
//                       RETAKE / PRACTICE SETTINGS / DISCARD RECORDING
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

// Recorder calls this after the synchronized Stop request has consumed its
// required post-request input boundary and entered Review. It reopens the
// dedicated Review context without routing through the Practice menu.
void NotifyRecorderEnteredReview();

// Frame-monitor thread: freeze maintenance + menu input while open.
void Tick();

// ---- render-thread ----
bool WantsDraw();
void Draw(ImDrawList* dl);

} // namespace Mission::PauseMenu
