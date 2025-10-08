#pragma once
#include <stdint.h>

// Version-aware EfzRevival RVA accessors. These return module-relative addresses (RVAs)
// for functions/globals we call or hook, switching based on the detected EfzRevival version.
// For unknown/unsupported versions, these return 0 so callers can skip the operation safely.

// Returns the correct unfreeze parameter for the patch toggler: 1 for 1.02e, 3 for 1.02h/i.
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
// Direct static pointer to Practice controller (CheatEngine found all versions)
// e: EfzRevival.dll+0xA02CC, h: EfzRevival.dll+0xA02EC, i: EfzRevival.dll+0xA15F8
uintptr_t EFZ_RVA_PracticeControllerPtr();
// Practice hotkey dispatcher (evaluates Pause/Step/Record/etc.)
uintptr_t EFZ_RVA_PracticeDispatcher();

// Version-aware Practice controller offsets
// These return the correct offset based on detected EfzRevival version
// 1.02e uses different offsets than 1.02h/i for pause/step fields
uintptr_t EFZ_Practice_PauseFlagOffset();    // 0xB4 for 1.02e, 0x180 for 1.02h/i
uintptr_t EFZ_Practice_StepFlagOffset();     // 0xAC for 1.02e, 0x172 for 1.02h/i
uintptr_t EFZ_Practice_StepCounterOffset();  // 0xB0 for 1.02e, 0x176 for 1.02h/i

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
