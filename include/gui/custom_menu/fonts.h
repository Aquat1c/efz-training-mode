#pragma once

struct ImFont;

namespace CustomMenu::Fonts {

// Load the embedded ITC Bolt Bold Regular font at two sizes into the current
// ImGui context's atlas. Returns true on success; false means we failed to
// locate the embedded resource, in which case the caller should fall back to
// ImGui's default font.
//
// Sizes use io.Fonts->AddFontFromMemoryTTF with pixel-snap and no oversampling
// to match EFZ's pixel-aligned bitmap aesthetic. The custom menu renders in
// the game's fixed 640x480 canvas, so only the user scale participates here;
// window/DPI presentation scale is handled outside the menu's logical space.
bool Rebuild(float uiScale);

// Access the loaded faces. Returns nullptr before Rebuild() succeeds.
// Body is the ~11px row font; Header is the ~16px section-header font.
// Fall back to the caller's choice (ImGui default) when either is null.
ImFont* Body();
ImFont* Header();

// Returns true if both faces have been successfully loaded.
bool IsLoaded();

} // namespace CustomMenu::Fonts
