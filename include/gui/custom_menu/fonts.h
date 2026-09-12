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

// Clear every non-owning ImFont pointer and cached layout that refers to the
// current ImGui font atlas. Call this immediately before any path invokes
// io.Fonts->Clear(), including atlas rebuilds outside the custom-menu renderer.
// The next Rebuild() call will repopulate the custom faces.
void InvalidateAtlasReferences();

// Access the loaded faces. Returns nullptr before Rebuild() succeeds.
// Body is the ~11px row font; Header is the ~16px section-header font.
// Fall back to the caller's choice (ImGui default) when either is null.
ImFont* Body();
ImFont* Header();

// Tutorial-only high-density faces. They are rasterized at roughly twice the
// logical draw size and must therefore always be passed an explicit logical
// pixel size to AddText/CalcTextSizeA. This keeps tutorial prose readable after
// the 640x480 virtual canvas is enlarged without changing any shared menu
// geometry or adding work to the per-frame renderer.
//
// TutorialReadable follows the user's normal UI-font preference (Segoe UI or
// ImGui's default face). TutorialHeader uses the embedded ITC Bolt face so the
// full-page tutorial headings still belong to the rest of the EFZ menu skin.
ImFont* TutorialReadable();
ImFont* TutorialHeader();

// Returns true if the two menu faces and both tutorial faces are loaded.
bool IsLoaded();

} // namespace CustomMenu::Fonts
