#pragma once
//
// PracticeMenu - reroutes the EFZ title-screen PRACTICE entry (index 3) into a
// native submenu offering TUTORIAL / TRIALS / PRACTICE.
//
// Coexistence: we do NOT patch the title menu count, wrap bounds, render
// geometry, jump table, or vtable - so we never conflict with InGameNetplay
// (which adds a NETPLAY entry by doing exactly those). Instead we:
//   * jmp-patch the Practice CASE (0x776352, a jump-table target both vanilla
//     and InGameNetplay route to) so confirming Practice enters our submenu
//     BEFORE the game's blocking fade-out runs;
//   * MinHook the real title update/render functions (which InGameNetplay calls
//     through by address), so our detours compose in either install order.
//
// See project memory: tutorial-trial-mode, and shared_documentation/
// NETPLAY_MENU_INTEGRATION.md.

namespace PracticeMenu {

// Install the title hooks + Practice-case entry patch. Safe to call once during
// delayed init. Verifies the vanilla title code is present first; if efz.exe
// looks patched/relocated beyond recognition it skips (Practice keeps working
// as vanilla). Returns true if the entry hook was installed.
bool Install();

// Remove all hooks/patches and free the loaded submenu surface.
void Uninstall();

// True while the Practice submenu is on screen (owns title input/render).
bool IsSubmenuActive();

} // namespace PracticeMenu
