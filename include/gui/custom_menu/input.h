#pragma once

namespace CustomMenu::Input {

// Call once when the menu becomes visible. Snapshots the current physical
// state of all tracked keys/buttons so any already-held inputs (e.g. the
// menu-open key, or a gamepad button from gameplay) do NOT register as a
// rising edge on the first frame.
void ResetEdges();

// Edge-detected navigation queries. Each returns true exactly once per "press"
// — where "press" is whatever the gated input machine in imgui_impl.cpp
// produces (keyboard arrow tap, dpad tap, analog stick tap, plus the
// accelerating-repeat pulses after 1s hold).
//
// Input sources unified into each query:
//   - ImGui's GamepadDpad keys (fed by PreNewFrameInputs aggregation of
//     all XInput pads, analog sticks, game-bound direction keys)
//   - ImGui's Enter / Escape keys (fed from gated face A/B buttons)
//   - Direct WinAPI GetAsyncKeyState for arrow/Enter/Escape as a keyboard-
//     native fallback (catches users whose arrow keys are NOT routed through
//     imgui_impl's gate).
//
// Safe to query multiple times within the same frame — internally backed by
// ImGui::IsKeyPressed which is frame-coherent.

bool NavUp();
bool NavDown();
bool NavLeft();
bool NavRight();

// Activate / Accept: Enter, A (gamepad face-down), mouse left click on
// focused row (future). Returns edge.
bool Activate();

// Back / Cancel: Escape, B (gamepad face-right). Returns edge.
bool Back();

// Switch active player column on the Values screen: EFZ D button / gamepad Y.
bool SwitchPlayer();

// Tab navigation — cycles top-level tabs. LB/RB on controller; PageUp/PageDown
// on keyboard.
bool TopTabPrev();
bool TopTabNext();

// Sub-tab navigation — cycles sub-tabs within the active top tab.
// LT/RT trigger bits on controller; '[' / ']' on keyboard.
bool SubTabPrev();
bool SubTabNext();

// Direct top-tab jump via the 1..9 number-row keys. Returns 0 if no edge this
// frame, or the 1-based index pressed (1..9). The caller decides whether the
// index is valid for the current top-tab count.
int  TopTabNumberEdge();

// ===== Mouse =====
// Returns the current mouse position in our 640x480 virtual canvas space.
// imgui_impl.cpp already remaps the OS cursor into this coord system during
// PreNewFrameInputs, so ImGui::GetMousePos() is authoritative.
struct MousePos { float x, y; bool valid; };
MousePos GetMouse();

// Single-shot hit test helpers (rising-edge button queries).
bool MouseHovering(float x, float y, float w, float h);
bool MouseClickedIn(float x, float y, float w, float h);      // left button
bool MouseRightClickedIn(float x, float y, float w, float h); // right button

// Edge query for the left mouse button anywhere. Useful when the row-level
// hit test isn't appropriate (e.g. tab bar which has variable-width labels).
bool MouseLeftEdge();

} // namespace CustomMenu::Input
