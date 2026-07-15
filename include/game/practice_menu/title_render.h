#pragma once
//
// Native rendering for the Practice submenu. Loads our own title_ob2.dat object
// sheet into a PRIVATE surface (never touches the title's own title_ob surface,
// so it can't fight InGameNetplay over that asset) and blits the submenu rows
// over the title's menu area using the game's own blit function.
//
// All coordinates are in the game's native 320x240 front-end space.

#include <cstdint>

namespace PracticeMenu::Render {

// Logical submenu rows, in draw order (top -> bottom). The enum index IS the sheet
// row AND the screen row (DrawRows uses it for both srcY and destY), so this order
// MUST match the label order baked into assets/title_ob2.dat (currently
// Practice / Mission / Tutorial). The two sprite lanes are unselected + selected/
// highlight. There is no BACK row - the cancel button returns to the title menu.
enum Row : int {
    ROW_PRACTICE = 0,
    ROW_MISSION,
    ROW_TUTORIAL,
    ROW_COUNT
};

// Lazily loads assets/title_ob2.dat into a private surface (idempotent).
// Returns false if the asset could not be found/loaded - callers should treat
// rendering as unavailable but keep the submenu logic working.
bool EnsureLoaded(uint32_t screenContext);

// Free the private surface + restore the title palette. Called on submenu exit
// and on Uninstall().
void Release(uint32_t screenContext);

// True once EnsureLoaded() has succeeded and a surface is available.
bool IsLoaded();

// (Re)load our sheet's palette into the overlay slots and apply it. MUST be
// called on every submenu entry: leaving the submenu restores the vanilla title
// palette, so without re-applying, a re-entry renders our surface with the wrong
// colors (garbled glyphs).
void ApplyPalette(uint32_t screenContext);

// Draw ONLY the title background (surface +1076: logo + sky) full-screen, with
// no menu. Used while the submenu owns the frame so the vanilla menu (which
// lives in the +1080 objects surface) is fully replaced by our rows rather than
// showing underneath. Call before DrawRows.
void DrawBackground(uint32_t screenContext);

// Draw all submenu rows for this frame, highlighting `selectedRow`, shifted
// right by `slideOffsetX` (used for the enter/leave slide animation; 0 = final
// resting position). No-op (but safe) when the sheet failed to load.
void DrawRows(uint32_t screenContext, int selectedRow, int slideOffsetX);

} // namespace PracticeMenu::Render
