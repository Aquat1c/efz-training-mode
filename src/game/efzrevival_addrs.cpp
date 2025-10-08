#include "../include/game/efzrevival_addrs.h"
#include "../include/utils/network.h" // GetEfzRevivalVersion
#include "../include/core/logger.h"
#include <atomic>
#include <sstream>
#include <windows.h>

static inline bool IsE() {
    return GetEfzRevivalVersion() == EfzRevivalVersion::Revival102e;
}
static inline bool IsH() {
    EfzRevivalVersion v = GetEfzRevivalVersion();
    return v == EfzRevivalVersion::Revival102h || v == EfzRevivalVersion::Revival102i;
}
static inline bool IsI() {
    return GetEfzRevivalVersion() == EfzRevivalVersion::Revival102i;
}

static void LogAddrOnce(const char* label, uintptr_t rva) {
    static std::atomic<bool> s_once{false};
    if (s_once.exchange(true)) return;
    std::ostringstream oss;
    const char* verName = "other";
    if (IsE()) verName = "1.02e"; else if (IsH()) {
        EfzRevivalVersion vdet = GetEfzRevivalVersion();
        verName = (vdet == EfzRevivalVersion::Revival102i) ? "1.02i" : "1.02h";
    }
    oss << "[ADDR] Version=" << verName
        << " | " << label << " RVA=0x" << std::hex << rva;
    HMODULE h = GetModuleHandleA("EfzRevival.dll");
    if (h && rva) {
        oss << " VA=0x" << (reinterpret_cast<uintptr_t>(h) + rva);
    }
    LogOut(oss.str(), true);
}

int EFZ_PatchToggleUnfreezeParam() {
    if (IsH()) return 3; // 1.02h/1.02i use 0=freeze, 3=unfreeze
    return 1;            // 1.02e uses 0=freeze, 1=unfreeze
}

uintptr_t EFZ_RVA_PatchToggler() {
    uintptr_t r = 0;
    if (IsE()) r = 0x006B2A0;
    else if (IsI()) r = 0x006BD50; // 1.02i: sub_1006BD50(ctx, 0/3) - DIFFERENT from 1.02h!
    else if (IsH()) r = 0x006BB00; // 1.02h: sub_1006BB00(ctx, 0/3)
    LogAddrOnce("PatchToggler", r);
    return r;
}

uintptr_t EFZ_RVA_PatchCtx() {
    uintptr_t r = 0;
    if (IsE()) r = 0x00A0760;
    else if (IsI()) r = 0x00A1790; // 1.02i: dword_100A1790 (corrected)
    else if (IsH()) r = 0x00A0780; // 1.02h: dword_100A0780
    LogAddrOnce("PatchCtx", r);
    return r;
}

uintptr_t EFZ_RVA_TogglePause() {
    uintptr_t r = 0;
    if (IsE()) r = 0x0075720;  // sub_10075720
    else if (IsI()) r = 0x0076710;  // 1.02i: sub_10076710
    else if (IsH()) r = 0x0076170;  // 1.02h: sub_10076170
    LogAddrOnce("TogglePause", r);
    return r;
}

uintptr_t EFZ_RVA_PracticeTick() {
    uintptr_t r = 0;
    if (IsE()) r = 0x0074F70;  // sub_10074F70
    else if (IsI()) r = 0x0074FF0;  // 1.02i: sub_10074FF0
    else if (IsH()) r = 0x0074F40;  // 1.02h: sub_10074F40
    LogAddrOnce("PracticeTick", r);
    return r;
}

uintptr_t EFZ_RVA_RefreshMappingBlock() {
    uintptr_t r = 0;
    if (IsE()) r = 0x0075100;  // sub_10075100
    else if (IsI()) r = 0x00760F0;  // 1.02i: sub_100760F0 (ctx -> Practice)
    else if (IsH()) r = 0x0075B50;  // 1.02h: sub_10075B50 (ctx -> Practice)
    LogAddrOnce("RefreshMappingBlock", r);
    return r;
}

uintptr_t EFZ_RVA_RefreshMappingBlock_PracToCtx() {
    uintptr_t r = 0;
    if (IsE()) r = 0;              // N/A on e (single variant only)
    else if (IsI()) r = 0x00760D0; // 1.02i: sub_100760D0 (Practice -> ctx)
    else if (IsH()) r = 0x0075B30; // 1.02h: sub_10075B30 (Practice -> ctx)
    LogAddrOnce("RefreshMappingBlock_PracToCtx", r);
    return r;
}

uintptr_t EFZ_RVA_MapReset() {
    uintptr_t r = 0;
    if (IsE()) r = 0x006D640;  // sub_1006D640
    else if (IsI()) r = 0x006E190;  // 1.02i: sub_1006E190 (corrected)
    else if (IsH()) r = 0x006DEC0;  // 1.02h: sub_1006DEC0 (corrected)
    LogAddrOnce("MapReset", r);
    return r;
}

uintptr_t EFZ_RVA_CleanupPair() {
    uintptr_t r = 0;
    if (IsE()) r = 0x006CAD0;  // 1.02e: EFZ_Obj_SubStruct448_CleanupPair
    else if (IsI()) r = 0x006D5F0;  // 1.02i: sub_1006D5F0 (corrected)
    else if (IsH()) r = 0x006D320;  // 1.02h: sub_1006D320 (corrected)
    LogAddrOnce("CleanupPair", r);
    return r;
}

uintptr_t EFZ_RVA_RenderBattleScreen() {
    uintptr_t r = 0;
    if (IsE()) r = 0x007642A0;
    else if (IsH()) r = 0x007642A0; // stable for 1.02h
    else if (IsI()) r = 0x007642A0; // stable for 1.02i
    LogAddrOnce("RenderBattleScreen", r);
    return r;
}

uintptr_t EFZ_RVA_GameModePtrArray() {
    uintptr_t r = 0;
    if (IsE()) r = 0x790110;
    else if (IsH()) r = 0x790110; // likely unchanged for 1.02h
    else if (IsI()) r = 0x790110; // likely unchanged for 1.02i; fast-path only
    LogAddrOnce("GameModePtrArray", r);
    return r;
}

uintptr_t EFZ_RVA_PracticeControllerPtr() {
    // CheatEngine pointer scans found static pointers to Practice controller base:
    // 1.02e: EfzRevival.dll+0xA02CC
    // 1.02h: EfzRevival.dll+0xA02EC
    // 1.02i: EfzRevival.dll+0xA15F8
    // These point directly to the Practice struct (not mode array which returns character structs)
    if (IsE()) return 0xA02CC;
    if (IsH()) {
        EfzRevivalVersion v = GetEfzRevivalVersion();
        if (v == EfzRevivalVersion::Revival102h) return 0xA02EC;
        if (v == EfzRevivalVersion::Revival102i) return 0xA15F8;
    }
    if (IsI()) return 0xA15F8;
    return 0;
}

uintptr_t EFZ_RVA_PracticeDispatcher() {
    uintptr_t r = 0;
    // Practice hotkey dispatcher RVAs per version
    // e: 0x00759F0, h: 0x0076490, i: 0x0076A30
    if (IsE()) r = 0x00759F0;
    else if (IsI()) r = 0x0076A30;
    else /* h */ r = 0x0076490;
    LogAddrOnce("PracticeDispatcher", r);
    return r;
}

// Version-aware Practice controller offset accessors
// Per decomp, these core offsets are identical across 1.02e/h/i.
uintptr_t EFZ_Practice_PauseFlagOffset() {
    if (IsI()) return 0x180;
    if (IsH()) return 0x180;
    return 0xB4;
}

uintptr_t EFZ_Practice_StepFlagOffset() {
    // CheatEngine shows i uses same offsets as e for step flag
    if (IsH()) return 0x172;
    return 0xAC;  // e and i both use 0xAC
}

uintptr_t EFZ_Practice_StepCounterOffset() {
    // CheatEngine confirmed: i uses +0xB0 (same as e), NOT 0x176!
    if (IsH()) return 0x176;
    return 0xB0;  // e and i both use 0xB0
}

uintptr_t EFZ_Practice_LocalSideOffset() {
    if (IsI()) return 0x688;
    return 0x680;
}
uintptr_t EFZ_Practice_RemoteSideOffset() {
    if (IsI()) return 0x692;
    return 0x684;
}

uintptr_t EFZ_Practice_InitSourceSideOffset() {
    // 1.02i shifted init-source from 0x944->0x952
    return IsI() ? 0x952 : 0x944;
}

uintptr_t EFZ_Practice_SideBufPrimaryOffset() {
    // Side buffer pointers only exist on e-version at these offsets
    // h/i use CleanupPair+RefreshMappingBlock instead, so we return 0 to skip validation
    if (IsE()) return 0x824;
    return 0;  // h/i: not used
}
uintptr_t EFZ_Practice_SideBufSecondaryOffset() {
    if (IsE()) return 0x828;
    return 0;  // h/i: not used
}
uintptr_t EFZ_Practice_SharedInputVectorOffset() { return 0x1240; }

int EFZ_Practice_MapResetIndexBias() {
    // Map array index base used at init when calling MapReset
    // 1.02i uses (local + 105); e/h use (local + 104)
    return IsI() ? 105 : 104;
}
