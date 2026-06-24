#pragma once
#include <stdint.h>

// Version-aware EfzRevival RVA accessors. These return module-relative addresses (RVAs)
// for functions/globals we call or hook, switching based on the detected EfzRevival version.
// For unknown/unsupported versions, these return 0 so callers can skip the operation safely.

// Returns the correct normal-speed parameter for the patch toggler.
// 1.02f subframe, 1.02g, and h/i use 3 for normal speed; 1.02e/classic-f use 1.
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
// Deprecated: this accessor intentionally returns 0.
// The previously used RVAs overlap Revival session-pointer globals and are not
// valid Practice-controller pointers.
uintptr_t EFZ_RVA_PracticeControllerPtr();
// Practice hotkey dispatcher (evaluates Pause/Step/Record/etc.)
uintptr_t EFZ_RVA_PracticeDispatcher();

// Version-aware Practice controller offsets.
// Pause/step fields are stable across the supported Revival versions.
uintptr_t EFZ_Practice_PauseFlagOffset();    // 0xB4
uintptr_t EFZ_Practice_StepFlagOffset();     // 0xAC
uintptr_t EFZ_Practice_StepCounterOffset();  // 0xB0
uintptr_t EFZ_Practice_PauseHotkeyOffset();  // e/f/g/h: 0x1D4, i: 0x1D8
uintptr_t EFZ_Practice_StepHotkeyOffset();   // e/f/g/h: 0x1D8, i: 0x1DC
uintptr_t EFZ_Practice_SaveHotkeyOffset();   // e/f/g/h: 0x1DC, i: 0x1E0
uintptr_t EFZ_Practice_LoadHotkeyOffset();   // e/f/g/h: 0x1E0, i: 0x1E4

// Side selection and related Practice controller fields
uintptr_t EFZ_Practice_LocalSideOffset();    // 0x680 for 1.02e/h, 0x688 for 1.02i
uintptr_t EFZ_Practice_RemoteSideOffset();   // 0x684 for 1.02e/h, 0x692 for 1.02i
uintptr_t EFZ_Practice_InitSourceSideOffset(); // 0x944 for 1.02e/h, 0x952 for 1.02i
uintptr_t EFZ_Practice_SideBufPrimaryOffset();   // 0x824 (stable)
uintptr_t EFZ_Practice_SideBufSecondaryOffset(); // 0x828 (stable)
uintptr_t EFZ_Practice_SharedInputVectorOffset(); // 0x1240 (stable)

// MapReset index bias when selecting map pointer from the array during init/swap
// 1.02e/h use (local + 104), 1.02i uses (local + 105)
int EFZ_Practice_MapResetIndexBias();

// Overlay toggle functions - simple bool toggles for display flags
uintptr_t EFZ_RVA_ToggleHurtboxDisplay();
uintptr_t EFZ_RVA_ToggleHitboxDisplay();
uintptr_t EFZ_RVA_ToggleFrameDisplay();

// Debug/testing: log scanner vs version constants and optionally force scanner usage via env (EFZ_SCAN_FORCE=1)
void EFZ_Debug_LogScannerComparison();

// VS/Practice Mode Savestate functions (non-recording)
// These are the actual functions called when user presses save/load keys in normal Practice mode.
// Returns RVA for the load state function: sub_10075910 (1.02e)
uintptr_t EFZ_RVA_LoadState();
// Returns RVA for the save state function: sub_10075980 (1.02e)
uintptr_t EFZ_RVA_SaveState();
// Practice hotkey handler that routes key presses to Save/Load based on configured keybinds.
// sub_100759F0 (1.02e) - checks this+476 for save key, this+480 for load key
uintptr_t EFZ_RVA_PracticeHotkeyHandler();

// Replay/Recording Mode Savestate functions (different code path)
// These are only used when recording is active - NOT in normal Practice mode!
uintptr_t EFZ_RVA_ReplayLoadState();
uintptr_t EFZ_RVA_ReplaySaveState();
