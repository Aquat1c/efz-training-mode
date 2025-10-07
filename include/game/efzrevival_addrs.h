#pragma once
#include <stdint.h>

// Version-aware EfzRevival RVA accessors. These return module-relative addresses (RVAs)
// for functions/globals we call or hook, switching based on the detected EfzRevival version.
// For unknown/unsupported versions, these return 0 so callers can skip the operation safely.

// Returns the correct unfreeze parameter for the patch toggler: 1 for 1.02e, 3 for 1.02h.
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
