#pragma once
//
// HUD disable — hides the EFZ (Memorial) in-game HUD on demand by hooking the
// battle render pipeline in efz.exe. See shared_documentation/HUD_MAP_AND_DISABLE_HIGHLIGHT.md.
//
// Two granularities:
//  * WHOLE HUD (master): renderBattleScreen wrap that forces the engine's own
//    intro/outro gate (resourceManager+82444) positive around the render, then
//    restores it — reuses the exact path the game uses every round intro, so top
//    HUD + meters + per-character UI all suppress themselves. Plus a skip of the
//    combo/stats panel (gated by the caller, not by 82444).
//  * PER ELEMENT: a blit filter. renderGameHUD / renderResourceMetersAndBlockIndicators
//    set a thread-local "seam" around their draws; the two software blitters
//    (blitSurfaceWithTransparency / blitSurfaceWithPaletteMapping) are hooked and,
//    while inside a HUD seam, skip a draw whose destination rect matches a disabled
//    element (identified by its fixed 320x240 dest-Y). Gameplay sprites (seam==none)
//    always pass through untouched.
//
// All hooks are efz.exe base+RVA, __fastcall+edx, netplay-gated (collision_hook
// profile). Never touches EfzRevival.dll.

namespace HudDisable {

// Per-element bits for SetElementDisabled / IsElementDisabled.
enum Element : unsigned {
    ElemTopBar     = 1u << 0,   // top HUD background bar
    ElemTimer      = 1u << 1,   // round timer digits
    ElemPortraits  = 1u << 2,   // character portraits
    ElemHpBars     = 1u << 3,   // health bars (green + red)
    ElemRoundDots  = 1u << 4,   // round/win indicator dots
    ElemNameplates = 1u << 5,   // character name plates
    ElemBottomBar  = 1u << 6,   // bottom HUD background bar
    ElemSpMeter    = 1u << 7,   // super (SP) meter bars + level icons
    ElemRfGauge    = 1u << 8,   // RF gauge
    ElemComboPanel = 1u << 9,   // combo / score / damage panel
};

// Group masks (for "show only this group" presets, e.g. a lesson page that
// teaches the HP bars = top group, or the RF/SP meters = bottom group).
enum GroupMask : unsigned {
    GroupTop    = ElemTopBar | ElemTimer | ElemPortraits | ElemHpBars | ElemRoundDots | ElemNameplates,
    GroupBottom = ElemBottomBar | ElemSpMeter | ElemRfGauge,
    GroupCombo  = ElemComboPanel,
    GroupAll    = GroupTop | GroupBottom | GroupCombo,
};

void Install();   // create + enable all hooks (idempotent); safe at startup
void Remove();    // disable + remove hooks

void SetHidden(bool hidden);   // whole HUD (master)
bool IsHidden();

void SetElementDisabled(unsigned bit, bool disabled);
bool IsElementDisabled(unsigned bit);

// Set the whole per-element hide mask at once (e.g. a lesson-page preset).
void SetElementMask(unsigned mask);

// Restore the whole HUD (clear master + all per-element bits). Called on match
// exit so the HUD comes back when leaving to the menu/title.
void ResetVisible();

} // namespace HudDisable
