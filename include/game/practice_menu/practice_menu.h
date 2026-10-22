#pragma once
// PracticeMenu - reroutes the EFZ title-screen PRACTICE entry (index 3) into a
// native submenu offering TUTORIAL / TRIALS / PRACTICE.
// Coexistence: the title menu count, wrap bounds, render geometry, jump table,
// and vtable are NOT patched (InGameNetplay adds NETPLAY that way). Instead:
//   * jmp-patch the Practice CASE (0x776352, a jump-table target both vanilla
//     and InGameNetplay route to) so confirming Practice enters the submenu
//     before the game's blocking fade-out runs;
//   * MinHook the real title update/render functions (InGameNetplay calls
//     them by address), so detours compose in either install order.
// See shared_documentation/NETPLAY_MENU_INTEGRATION.md.

#include "mission_title_screen.h"

namespace PracticeMenu {

// Install the title hooks + Practice-case entry patch. Verifies the vanilla
// title code first; a patched/relocated efz.exe skips (Practice keeps working
// as vanilla). Returns true if the entry hook was installed.
bool Install();

// Remove all hooks/patches and free the loaded submenu surface.
void Uninstall();

// True while the Practice submenu is on screen (owns title input/render).
bool IsSubmenuActive();

// Deferred browser reopen: the next title update after "RETURN TO
// MISSIONS/LESSONS" opens the submenu directly on the given screen.
void RequestTitleReopen(TitleScreen::Screen screen);

} // namespace PracticeMenu
