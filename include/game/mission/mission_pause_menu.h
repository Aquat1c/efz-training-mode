#pragma once
// Sole default pause surface while a trial, tutorial, or recording session owns
// the match; menu rows are resolved from that context at open time.
// The game is frozen through PauseIntegration while open.
// Input is polled on the frame-monitor thread (Tick); rendering is per-EndScene (Draw).
#include <string>

struct ImDrawList;

namespace Mission::PauseMenu {

// True while the pause menu owns the screen.
bool IsOpen();

// Fails outside an active runner/recording match, during demo playback, and
// during tutorial Preparing or the DemoSuspended restore handoff.
bool Open();
void Close();

// gui OpenMenu() routes here: first press opens, next press resumes.
void Toggle();

// Reopens the Review context after the recorder's synchronized Stop consumed
// its post-request input boundary.
void NotifyRecorderEnteredReview();

// Frame-monitor thread: freeze maintenance + menu input while open.
void Tick();

// render-thread
bool WantsDraw();
void Draw(ImDrawList* dl);

} // namespace Mission::PauseMenu
