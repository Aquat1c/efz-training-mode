#include "../include/game/efzrevival_addrs.h"
// EfzRevival address scanner has been fully retired; offsets are now static.
#include "../include/utils/network.h" // GetEfzRevivalVersion
#include "../include/core/logger.h"
#include <atomic>
#include <sstream>
#include <windows.h>

static inline bool IsE() {
    EfzRevivalVersion v = GetEfzRevivalVersion();
    return v == EfzRevivalVersion::Revival102e || v == EfzRevivalVersion::Revival102f || v == EfzRevivalVersion::Revival102g;
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
    if (IsE()) {
        EfzRevivalVersion vdet = GetEfzRevivalVersion();
        verName = (vdet == EfzRevivalVersion::Revival102g) ? "1.02g" :
                  (vdet == EfzRevivalVersion::Revival102f) ? "1.02f" : "1.02e";
    } else if (IsH()) {
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

// Legacy SigDebug/EFZ_Debug_LogScannerComparison removed along with scanner support.

int EFZ_PatchToggleUnfreezeParam() {
    if (IsH()) return 3; // 1.02h/1.02i use 0=freeze, 3=unfreeze
    return 1;            // 1.02e uses 0=freeze, 1=unfreeze
}

uintptr_t EFZ_RVA_PatchToggler() {
    uintptr_t r = 0;
    EfzRevivalVersion v = GetEfzRevivalVersion();
    if (v == EfzRevivalVersion::Revival102f) r = 0x006B2A0; // 1.02f: sub_1006B2A0(ctx, on/off)
    else if (IsE()) r = 0x006B2A0;
    else if (IsI()) r = 0x006BD50; // 1.02i: sub_1006BD50(ctx, 0/3) - DIFFERENT from 1.02h!
        else if (IsH()) r = 0x006BB00; // 1.02h: sub_1006BB00(ctx, 0/3)
    LogAddrOnce("PatchToggler", r);
    return r;
}

uintptr_t EFZ_RVA_PatchCtx() {
    uintptr_t r = 0;
    EfzRevivalVersion v = GetEfzRevivalVersion();
    if (v == EfzRevivalVersion::Revival102f) r = 0x00A0760; // 1.02f: dword_100A0760
    else if (IsE()) r = 0x00A0760;
        else if (IsI()) r = 0x00A1790; // 1.02i: dword_100A1790 (corrected)
        else if (IsH()) r = 0x00A0780; // 1.02h: dword_100A0780
    LogAddrOnce("PatchCtx", r);
    return r;
}

uintptr_t EFZ_RVA_TogglePause() {
    uintptr_t r = 0;
    EfzRevivalVersion v = GetEfzRevivalVersion();
    if (v == EfzRevivalVersion::Revival102f) r = 0x0075750;  // 1.02f: sub_10075750
        else if (v == EfzRevivalVersion::Revival102e) r = 0x0075720;  // 1.02e: sub_10075720
        else if (v == EfzRevivalVersion::Revival102g) r = 0x00759C0;  // 1.02g: sub_100759C0
        else if (IsI()) r = 0x0076710;  // 1.02i: sub_10076710
        else if (IsH()) r = 0x0076170;  // 1.02h: sub_10076170
    LogAddrOnce("TogglePause", r);
    return r;
}

uintptr_t EFZ_RVA_PracticeTick() {
    uintptr_t r = 0;
        EfzRevivalVersion v = GetEfzRevivalVersion();
        if (v == EfzRevivalVersion::Revival102f) r = 0x00757A0;  // 1.02f: sub_100757A0
        else if (IsE()) r = 0x0074F70;  // sub_10074F70
        else if (IsI()) r = 0x0074FF0;  // 1.02i: sub_10074FF0
        else if (IsH()) r = 0x0074F40;  // 1.02h: sub_10074F40
    LogAddrOnce("PracticeTick", r);
    return r;
}

uintptr_t EFZ_RVA_RefreshMappingBlock() {
    uintptr_t r = 0;
        EfzRevivalVersion v = GetEfzRevivalVersion();
        if (v == EfzRevivalVersion::Revival102f) r = 0x0075130; // 1.02f: sub_10075130 (ctx -> Practice)
        else if (IsE()) r = 0x0075100;  // sub_10075100
        else if (IsI()) r = 0x00760F0;  // 1.02i: sub_100760F0 (ctx -> Practice)
        else if (IsH()) r = 0x0075B50;  // 1.02h: sub_10075B50 (ctx -> Practice)
    LogAddrOnce("RefreshMappingBlock", r);
    return r;
}

uintptr_t EFZ_RVA_RefreshMappingBlock_PracToCtx() {
    uintptr_t r = 0;
        EfzRevivalVersion v = GetEfzRevivalVersion();
        if (v == EfzRevivalVersion::Revival102f) r = 0; // keep disabled for f (match e/g behavior)
        else if (IsE()) r = 0;              // N/A on e (single variant only)
        else if (IsI()) r = 0x00760D0; // 1.02i: sub_100760D0 (Practice -> ctx)
        else if (IsH()) r = 0x0075B30; // 1.02h: sub_10075B30 (Practice -> ctx)
    LogAddrOnce("RefreshMappingBlock_PracToCtx", r);
    return r;
}

uintptr_t EFZ_RVA_MapReset() {
    uintptr_t r = 0;
    EfzRevivalVersion v = GetEfzRevivalVersion();
    if (v == EfzRevivalVersion::Revival102f) r = 0x006D640;  // 1.02f: sub_1006D640
    else if (v == EfzRevivalVersion::Revival102e) r = 0x006D640;  // sub_1006D640
    else if (v == EfzRevivalVersion::Revival102g) r = 0x006D850;  // sub_1006D850
    else if (v == EfzRevivalVersion::Revival102i) r = 0x006E190;  // sub_1006E190
    else if (v == EfzRevivalVersion::Revival102h) r = 0x006DEC0;  // sub_1006DEC0
    // For unsupported versions: return 0 (vanilla behavior - no player switching)
    LogAddrOnce("MapReset", r);
    return r;
}

uintptr_t EFZ_RVA_CleanupPair() {
    uintptr_t r = 0;
    EfzRevivalVersion v = GetEfzRevivalVersion();
    if (v == EfzRevivalVersion::Revival102f) r = 0x006CAD0;  // 1.02f: sub_1006CAD0
    else if (v == EfzRevivalVersion::Revival102e) r = 0x006CAD0;  // sub_1006CAD0
    else if (v == EfzRevivalVersion::Revival102g) r = 0x006CCE0;  // sub_1006CCE0
    else if (v == EfzRevivalVersion::Revival102i) r = 0x006D5F0;  // sub_1006D5F0
    else if (v == EfzRevivalVersion::Revival102h) r = 0x006D320;  // sub_1006D320
    // For unsupported versions: return 0 (vanilla behavior - no player switching)
    LogAddrOnce("CleanupPair", r);
    return r;
}

uintptr_t EFZ_RVA_RenderBattleScreen() {
    uintptr_t r = 0;
    // RVA lives in efz.exe; observed stable across e/h/i at 0x007642A0.
    // For vanilla, try the same RVA; if a build differs, we can swap to AOB later.
    r = 0x007642A0;
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
    // 1.02g: EfzRevival.dll+0xA02CC (same as 1.02e)
    // 1.02h: EfzRevival.dll+0xA02EC
    // 1.02i: EfzRevival.dll+0xA15F8
    // These point directly to the Practice struct (not mode array which returns character structs)
    if (IsE()) return 0xA02CC; // Covers both 1.02e and 1.02g
    if (IsH()) {
        EfzRevivalVersion v = GetEfzRevivalVersion();
        if (v == EfzRevivalVersion::Revival102h) return 0xA02EC;
        if (v == EfzRevivalVersion::Revival102i) return 0xA15F8;
    }
    if (IsI()) return 0xA15F8;
    // Unsupported versions: return 0 (no scanning fallback).
    return 0;
}

uintptr_t EFZ_RVA_PracticeDispatcher() {
    uintptr_t r = 0;
    // Practice hotkey dispatcher RVAs per version
    // e: 0x00773A0, g: 0x0075CC0, h: 0x0076490, i: 0x0076A30
    EfzRevivalVersion v = GetEfzRevivalVersion();
    // CRITICAL: Disable dispatcher on 1.02e/1.02f/1.02g entirely to avoid Replay-mode crashes.
    if (v == EfzRevivalVersion::Revival102e) r = 0;  // disabled for 1.02e
    else if (v == EfzRevivalVersion::Revival102f) r = 0;  // disabled for 1.02f
    else if (v == EfzRevivalVersion::Revival102g) r = 0;  // disabled for 1.02g
    else if (IsI()) r = 0x0076A30;
    else if (IsH()) r = 0x0076490;
    // For unsupported versions: return 0 (don't guess addresses)
    LogAddrOnce("PracticeDispatcher", r);
    return r;
}

// Version-aware Practice controller offset accessors
// CheatEngine confirmed: Practice struct layout is IDENTICAL across e/h/i
// Only the static pointer location differs (e: +0xA02CC, h: +0xA02EC, i: +0xA15F8)
uintptr_t EFZ_Practice_PauseFlagOffset() {
    return 0xB4;  // All versions
}

uintptr_t EFZ_Practice_StepFlagOffset() {
    return 0xAC;  // All versions
}

uintptr_t EFZ_Practice_StepCounterOffset() {
    return 0xB0;  // All versions (CheatEngine confirmed: inc [edi+0xB0])
}

uintptr_t EFZ_Practice_LocalSideOffset() {
    // 1.02i shifts a few Practice fields by +8 relative to e/h per decompilation
    return IsI() ? 0x688 : 0x680;
}
uintptr_t EFZ_Practice_RemoteSideOffset() {
    return IsI() ? 0x692 : 0x684;
}

uintptr_t EFZ_Practice_InitSourceSideOffset() {
    return IsI() ? 0x952 : 0x944;
}

uintptr_t EFZ_Practice_SideBufPrimaryOffset() {
    // Side buffer pointers - ALL VERSIONS use these during init
    // IDA shows decimal offsets, so 824 decimal = 0x338 hex
    // Verified in h-version decompilation at line 76084: *(_DWORD *)(this + 824) = v5;
    // Where v5 = this + 788, and 824/828 are buffer pointer slots
    return 0x338;  // 824 decimal = 0x338 hex (ALL VERSIONS)
}
uintptr_t EFZ_Practice_SideBufSecondaryOffset() {
    return 0x33C;  // 828 decimal = 0x33C hex (ALL VERSIONS)
}
uintptr_t EFZ_Practice_SharedInputVectorOffset() { return 0x1240; }

int EFZ_Practice_MapResetIndexBias() {
    // Map array index base used at init when calling MapReset
    // 1.02i uses (local + 105); e/h use (local + 104)
    return IsI() ? 105 : 104;
}

// Overlay toggle functions - simple bool toggles for display flags
uintptr_t EFZ_RVA_ToggleHurtboxDisplay() {
    uintptr_t r = 0;
    EfzRevivalVersion v = GetEfzRevivalVersion();
    if (v == EfzRevivalVersion::Revival102f) r = 0x0075170;  // 1.02f: sub_10075170 - toggles this[183]
    else if (v == EfzRevivalVersion::Revival102e) r = 0x0075140;  // sub_10075140 - toggles this[183]
    else if (v == EfzRevivalVersion::Revival102g) r = 0x00753E0;  // sub_100753E0 - toggles this[183]
    // TODO: Add h/i versions if needed
    LogAddrOnce("ToggleHurtboxDisplay", r);
    return r;
}

uintptr_t EFZ_RVA_ToggleHitboxDisplay() {
    uintptr_t r = 0;
    EfzRevivalVersion v = GetEfzRevivalVersion();
    if (v == EfzRevivalVersion::Revival102f) r = 0x0075190;  // 1.02f: sub_10075190 - toggles this[182]
    else if (v == EfzRevivalVersion::Revival102e) r = 0x0075160;  // sub_10075160 - toggles this[182]
    else if (v == EfzRevivalVersion::Revival102g) r = 0x0075400;  // sub_10075400 - toggles this[182]
    // TODO: Add h/i versions if needed
    LogAddrOnce("ToggleHitboxDisplay", r);
    return r;
}

uintptr_t EFZ_RVA_ToggleFrameDisplay() {
    uintptr_t r = 0;
    EfzRevivalVersion v = GetEfzRevivalVersion();
    if (v == EfzRevivalVersion::Revival102f) r = 0x0075710;  // 1.02f: sub_10075710 - toggles this[181]
    else if (v == EfzRevivalVersion::Revival102e) r = 0x00756E0;  // sub_100756E0 - toggles this[181]
    else if (v == EfzRevivalVersion::Revival102g) r = 0x0075980;  // sub_10075980 - toggles this[181]
    // TODO: Add h/i versions if needed
    LogAddrOnce("ToggleFrameDisplay", r);
    return r;
}
