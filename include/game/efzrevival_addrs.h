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
uintptr_t EFZ_RVA_MapReset();
uintptr_t EFZ_RVA_CleanupPair();
uintptr_t EFZ_RVA_RenderBattleScreen();
uintptr_t EFZ_RVA_GameModePtrArray();

// Version-aware Practice controller offsets
// These return the correct offset based on detected EfzRevival version
// 1.02e uses different offsets than 1.02h/i for pause/step fields
uintptr_t EFZ_Practice_PauseFlagOffset();    // 0xB4 for 1.02e, 0x180 for 1.02h/i
uintptr_t EFZ_Practice_StepFlagOffset();     // 0xAC for 1.02e, 0x172 for 1.02h/i
uintptr_t EFZ_Practice_StepCounterOffset();  // 0xB0 for 1.02e, 0x176 for 1.02h/i
