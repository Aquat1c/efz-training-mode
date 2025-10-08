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
    else if (IsH()) r = 0x007642A0; // assume stable for 1.02h/1.02i
    LogAddrOnce("RenderBattleScreen", r);
    return r;
}

uintptr_t EFZ_RVA_GameModePtrArray() {
    uintptr_t r = 0;
    if (IsE()) r = 0x790110;
    else if (IsH()) r = 0x790110; // likely unchanged for 1.02h/1.02i; fast-path only
    LogAddrOnce("GameModePtrArray", r);
    return r;
}

// Version-aware Practice controller offset accessors
// CRITICAL: 1.02h/i use different offsets than 1.02e for pause/step fields!
uintptr_t EFZ_Practice_PauseFlagOffset() {
    // 1.02e: +0xB4, 1.02h/i: +0x180
    if (IsH()) return 0x180;  // 1.02h/i
    return 0xB4;              // 1.02e (default)
}

uintptr_t EFZ_Practice_StepFlagOffset() {
    // 1.02e: +0xAC, 1.02h/i: +0x172
    if (IsH()) return 0x172;  // 1.02h/i
    return 0xAC;              // 1.02e (default)
}

uintptr_t EFZ_Practice_StepCounterOffset() {
    // 1.02e: +0xB0, 1.02h/i: +0x176 (note: also exists at +0xB0 per docs)
    if (IsH()) return 0x176;  // 1.02h/i
    return 0xB0;              // 1.02e (default)
}
