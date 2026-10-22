#pragma once
#include <stdint.h>

// Version-aware EfzRevival RVA accessors: module-relative addresses for
// functions/globals we call or hook, switched on the detected EfzRevival
// version. Unknown/unsupported versions return 0 so callers can skip safely.

// Whether the version has the legacy large Practice routing layout used by
// SwitchPlayers; 1.02j is supported for critical training hooks but must use
// the engine-only side-switch path.
bool EFZ_SupportsNativePracticeSideSwitch();

// Normal-speed parameter for the patch toggler: 1.02f subframe, 1.02g and h/i
// use 3; 1.02e/classic-f use 1.
int EFZ_PatchToggleUnfreezeParam();

uintptr_t EFZ_RVA_PatchToggler();
uintptr_t EFZ_RVA_PatchCtx();
uintptr_t EFZ_RVA_TogglePause();
uintptr_t EFZ_RVA_PracticeTick();
uintptr_t EFZ_RVA_RefreshMappingBlock();
// h/i only: copy mapping block from Practice to ctx
uintptr_t EFZ_RVA_RefreshMappingBlock_PracToCtx();
uintptr_t EFZ_RVA_MapReset();
uintptr_t EFZ_RVA_CleanupPair();
uintptr_t EFZ_RVA_RenderBattleScreen();
uintptr_t EFZ_RVA_GameModePtrArray();
uintptr_t EFZ_RVA_RenderContextGlobal();
// Deprecated: intentionally returns 0; the previously used RVAs overlap
// Revival session-pointer globals and are not valid Practice-controller
// pointers.
uintptr_t EFZ_RVA_PracticeControllerPtr();
// Practice hotkey dispatcher (evaluates Pause/Step/Record/etc.)
uintptr_t EFZ_RVA_PracticeDispatcher();

// Version-aware Practice controller offsets; J uses its compact MinGW Practice
// layout.
uintptr_t EFZ_Practice_PauseFlagOffset();    // e-i: 0xB4, j: 0xDC
uintptr_t EFZ_Practice_StepFlagOffset();     // e-i: 0xAC, j: 0xD4
uintptr_t EFZ_Practice_StepCounterOffset();  // e-i: 0xB0, j: 0xD8
uintptr_t EFZ_Practice_PauseHotkeyOffset();  // e/f/g/h: 0x1D4, i: 0x1D8
uintptr_t EFZ_Practice_StepHotkeyOffset();   // e/f/g/h: 0x1D8, i: 0x1DC
uintptr_t EFZ_Practice_SaveHotkeyOffset();   // e/f/g/h: 0x1DC, i: 0x1E0
uintptr_t EFZ_Practice_LoadHotkeyOffset();   // e/f/g/h: 0x1E0, i: 0x1E4

uintptr_t EFZ_Practice_LocalSideOffset();    // 0x680 for 1.02e/h, 0x688 for 1.02i
uintptr_t EFZ_Practice_RemoteSideOffset();   // 0x684 for 1.02e/h, 0x692 for 1.02i
uintptr_t EFZ_Practice_InitSourceSideOffset(); // 0x944 for 1.02e/h, 0x952 for 1.02i
uintptr_t EFZ_Practice_SideBufPrimaryOffset();   // 0x824 (stable)
uintptr_t EFZ_Practice_SideBufSecondaryOffset(); // 0x828 (stable)
uintptr_t EFZ_Practice_SharedInputVectorOffset(); // 0x1240 (stable)

// MapReset index bias when selecting the map pointer during init/swap:
// 1.02e/h use (local + 104), 1.02i uses (local + 105).
int EFZ_Practice_MapResetIndexBias();

uintptr_t EFZ_RVA_ToggleHurtboxDisplay();
uintptr_t EFZ_RVA_ToggleHitboxDisplay();
uintptr_t EFZ_RVA_ToggleFrameDisplay();

// Debug: logs scanner vs version constants; EFZ_SCAN_FORCE=1 forces scanner usage.
void EFZ_Debug_LogScannerComparison();

// VS/Practice Mode savestate functions (non-recording; the functions behind
// the normal Practice save/load keys).
// Load state function: sub_10075910 (1.02e)
uintptr_t EFZ_RVA_LoadState();
// Save state function: sub_10075980 (1.02e)
uintptr_t EFZ_RVA_SaveState();
// Practice hotkey handler routing keys to Save/Load: sub_100759F0 (1.02e);
// checks this+476 for save key, this+480 for load key.
uintptr_t EFZ_RVA_PracticeHotkeyHandler();

// Replay/Recording Mode savestates (only while recording is active, NOT normal
// Practice mode).
uintptr_t EFZ_RVA_ReplayLoadState();
uintptr_t EFZ_RVA_ReplaySaveState();
