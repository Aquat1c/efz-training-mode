#pragma once
// Native rendering for the Practice submenu: loads assets/title_ob2.dat into a
// PRIVATE surface (the title's own title_ob surface is never touched, so it
// cannot conflict with InGameNetplay) and blits the rows over the title's menu
// area using the game's own blit function.  Coordinates are native 320x240.

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
// Returns false if the asset could not be loaded: rendering is unavailable but
// the submenu logic keeps working.
bool EnsureLoaded(uint32_t screenContext);

// Free the private surface + restore the title palette (submenu exit, Uninstall).
void Release(uint32_t screenContext);

// True once EnsureLoaded() has succeeded and a surface is available.
bool IsLoaded();

// (Re)load our sheet's palette into the overlay slots and apply it. MUST be
// called on every submenu entry: leaving restores the vanilla palette, so a
// re-entry without re-applying renders garbled glyphs.
void ApplyPalette(uint32_t screenContext);

// Draw ONLY the title background (surface +1076: logo + sky) full-screen.
// Replaces the vanilla menu (which lives in the +1080 objects surface).
// Call before DrawRows.
void DrawBackground(uint32_t screenContext);

// Draw all submenu rows, highlighting `selectedRow`, shifted right by
// `slideOffsetX` (enter/leave slide; 0 = final position). Safe no-op when the
// sheet failed to load.
void DrawRows(uint32_t screenContext, int selectedRow, int slideOffsetX);

} // namespace PracticeMenu::Render
