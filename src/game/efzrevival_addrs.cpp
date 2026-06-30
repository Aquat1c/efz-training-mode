#include "../include/game/efzrevival_addrs.h"
#include "../include/utils/network.h" // GetEfzRevivalVersion
#include "../include/core/logger.h"
#include <atomic>
#include <sstream>
#include <windows.h>

static inline bool IsE() {
    EfzRevivalVersion v = GetEfzRevivalVersion();
    return v == EfzRevivalVersion::Revival102e || v == EfzRevivalVersion::Revival102g;
}
static inline bool IsFClassic() {
    return GetEfzRevivalVersion() == EfzRevivalVersion::Revival102f && IsEfzRevival102fClassicBuild();
}
static inline bool IsFSubframe() {
    return GetEfzRevivalVersion() == EfzRevivalVersion::Revival102f && IsEfzRevival102fSubframeBuild();
}
static inline bool IsLegacyEFamily() {
    return GetEfzRevivalVersion() == EfzRevivalVersion::Revival102e || IsFClassic();
}
static inline bool IsH() {
    EfzRevivalVersion v = GetEfzRevivalVersion();
    return v == EfzRevivalVersion::Revival102h || v == EfzRevivalVersion::Revival102i;
}
static inline bool IsI() {
    return GetEfzRevivalVersion() == EfzRevivalVersion::Revival102i;
}
static inline bool IsJ() {
    return GetEfzRevivalVersion() == EfzRevivalVersion::Revival102j
        && IsEfzRevival102jVerifiedBuild();
}

bool EFZ_SupportsNativePracticeSideSwitch() {
    const EfzRevivalVersion v = GetEfzRevivalVersion();
    return IsEfzRevivalVersionSupported(v)
        && v != EfzRevivalVersion::Vanilla
        && v != EfzRevivalVersion::Revival102j;
}

static void LogAddrOnce(const char* label, uintptr_t rva) {
    static std::atomic<bool> s_once{false};
    if (s_once.exchange(true)) return;
    std::ostringstream oss;
    const char* verName = "other";
    if (IsE() || GetEfzRevivalVersion() == EfzRevivalVersion::Revival102f) {
        EfzRevivalVersion vdet = GetEfzRevivalVersion();
        verName = (vdet == EfzRevivalVersion::Revival102g) ? "1.02g" :
                  (vdet == EfzRevivalVersion::Revival102f) ? EfzRevivalDllFlavorName(GetEfzRevivalDllFlavor()) : "1.02e";
    } else if (IsH()) {
        EfzRevivalVersion vdet = GetEfzRevivalVersion();
        verName = (vdet == EfzRevivalVersion::Revival102i) ? "1.02i" : "1.02h";
    } else if (IsJ()) {
        verName = "1.02j";
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
    EfzRevivalVersion v = GetEfzRevivalVersion();
    if (IsFSubframe() || v == EfzRevivalVersion::Revival102g || IsH() || IsJ()) return 3; // 0=freeze, 3=normal speed
    return 1; // 1.02e and classic 1.02f behavior
}

uintptr_t EFZ_RVA_PatchToggler() {
    uintptr_t r = 0;
    EfzRevivalVersion v = GetEfzRevivalVersion();
    if (IsFSubframe() || v == EfzRevivalVersion::Revival102g) r = 0x006B4C0; // 1.02f subframe / 1.02g
    else if (IsLegacyEFamily()) r = 0x006B2A0;
    else if (IsI()) r = 0x006BD50; // 1.02i (different from 1.02h)
    else if (IsH()) r = 0x006BB00; // 1.02h
    else if (IsJ()) r = 0x0077F40; // 1.02j MinGW; verified visual patch selector
    LogAddrOnce("PatchToggler", r);
    return r;
}

uintptr_t EFZ_RVA_PatchCtx() {
    uintptr_t r = 0;
    EfzRevivalVersion v = GetEfzRevivalVersion();
    if (v == EfzRevivalVersion::Revival102f) r = 0x00A0760; // 1.02f
    else if (IsE()) r = 0x00A0760;
    else if (IsI()) r = 0x00A1790; // 1.02i
    else if (IsH()) r = 0x00A0780; // 1.02h
    else if (IsJ()) r = 0x014E8C0; // 1.02j patch-context object
    LogAddrOnce("PatchCtx", r);
    return r;
}

uintptr_t EFZ_RVA_TogglePause() {
    uintptr_t r = 0;
    EfzRevivalVersion v = GetEfzRevivalVersion();
    if (IsFSubframe()) r = 0x0075990;  // 1.02f sub_10075990
    else if (IsFClassic()) r = 0x0075750;  // 1.02f classic
        else if (v == EfzRevivalVersion::Revival102e) r = 0x0075720;  // 1.02e
        else if (v == EfzRevivalVersion::Revival102g) r = 0x00759C0;  // 1.02g
    else if (IsI()) r = 0x0076710;  // 1.02i
    else if (IsH()) r = 0x0076170;  // 1.02h
    else if (IsJ()) r = 0x007DB60;  // 1.02j; toggles +0xDC, resets +0xD8
    LogAddrOnce("TogglePause", r);
    return r;
}

uintptr_t EFZ_RVA_PracticeTick() {
    uintptr_t r = 0;
        EfzRevivalVersion v = GetEfzRevivalVersion();
        if (v == EfzRevivalVersion::Revival102e) r = 0x0074F70;  // 1.02e
        else if (IsFSubframe()) r = 0x00759E0;  // 1.02f subframe per-frame Practice tick
        else if (IsFClassic()) r = 0x0074FA0;  // 1.02f classic active Practice tick
        else if (v == EfzRevivalVersion::Revival102g) r = 0x0075210;  // 1.02g
        // For 1.02h/i, prefer the real per-frame Practice update loop because it
        // runs every visual frame with ECX = Practice controller. The older
        // 0x74F40/0x74FF0 helper path is not reliable for match-entry capture.
        else if (IsI()) r = 0x0075F60;  // 1.02i
        else if (IsH()) r = 0x00759C0;  // 1.02h
        else if (IsJ()) r = 0x007D6B0;  // 1.02j single-step/render body
    LogAddrOnce("PracticeTick", r);
    return r;
}

uintptr_t EFZ_RVA_RefreshMappingBlock() {
    uintptr_t r = 0;
        EfzRevivalVersion v = GetEfzRevivalVersion();
        if (IsFSubframe()) r = 0x0075370; // 1.02f subframe (ctx -> Practice)
        else if (IsFClassic()) r = 0x0075130; // 1.02f classic (ctx -> Practice)
        else if (IsE()) r = 0x0075100;  // e/g
        else if (IsI()) r = 0x00760F0;  // 1.02i (ctx -> Practice)
        else if (IsH()) r = 0x0075B50;  // 1.02h (ctx -> Practice)
    LogAddrOnce("RefreshMappingBlock", r);
    return r;
}

uintptr_t EFZ_RVA_RefreshMappingBlock_PracToCtx() {
    uintptr_t r = 0;
        EfzRevivalVersion v = GetEfzRevivalVersion();
        if (v == EfzRevivalVersion::Revival102f) r = 0; // keep disabled for f (match e/g behavior)
        else if (IsE()) r = 0;              // N/A on e (single variant only)
        else if (IsI()) r = 0x00760D0; // 1.02i (Practice -> ctx)
        else if (IsH()) r = 0x0075B30; // 1.02h (Practice -> ctx)
    LogAddrOnce("RefreshMappingBlock_PracToCtx", r);
    return r;
}

uintptr_t EFZ_RVA_MapReset() {
    uintptr_t r = 0;
    EfzRevivalVersion v = GetEfzRevivalVersion();
    if (v == EfzRevivalVersion::Revival102f) r = 0x006D640;  // 1.02f
    else if (v == EfzRevivalVersion::Revival102e) r = 0x006D640;  // 1.02e
    else if (v == EfzRevivalVersion::Revival102g) r = 0x006D850;  // 1.02g
    else if (v == EfzRevivalVersion::Revival102i) r = 0x006E190;  // 1.02i
    else if (v == EfzRevivalVersion::Revival102h) r = 0x006DEC0;  // 1.02h
    // For unsupported versions: return 0 (vanilla behavior - no player switching)
    LogAddrOnce("MapReset", r);
    return r;
}

uintptr_t EFZ_RVA_CleanupPair() {
    uintptr_t r = 0;
    EfzRevivalVersion v = GetEfzRevivalVersion();
    if (v == EfzRevivalVersion::Revival102f) r = 0x006CAD0;  // 1.02f
    else if (v == EfzRevivalVersion::Revival102e) r = 0x006CAD0;  // 1.02e
    else if (v == EfzRevivalVersion::Revival102g) r = 0x006CCE0;  // 1.02g
    else if (v == EfzRevivalVersion::Revival102i) r = 0x006D5F0;  // 1.02i
    else if (v == EfzRevivalVersion::Revival102h) r = 0x006D320;  // 1.02h
    // For unsupported versions: return 0 (vanilla behavior - no player switching)
    LogAddrOnce("CleanupPair", r);
    return r;
}

uintptr_t EFZ_RVA_RenderBattleScreen() {
    uintptr_t r = 0;
    // The decomp label is VA 0x007642A0 with the default 0x00400000 image base.
    // Callers add efz.exe base, so this accessor must return the RVA.
    r = 0x003642A0;
    LogAddrOnce("RenderBattleScreen", r);
    return r;
}

uintptr_t EFZ_RVA_GameModePtrArray() {
    uintptr_t r = 0;
        EfzRevivalVersion v = GetEfzRevivalVersion();
        if (v == EfzRevivalVersion::Revival102f || IsE()) r = 0x790110;
        else if (IsH()) r = 0x790110; // likely unchanged for 1.02h
        else if (IsI()) r = 0x790110; // likely unchanged for 1.02i; fast-path only
        // Deliberately unavailable for J: 0x790110 belongs to efz.exe, while
        // this legacy scanner incorrectly treats it as a Revival DLL RVA.
    LogAddrOnce("GameModePtrArray", r);
    return r;
}

uintptr_t EFZ_RVA_RenderContextGlobal() {
    uintptr_t r = 0;
    EfzRevivalVersion v = GetEfzRevivalVersion();
    if (v == EfzRevivalVersion::Revival102f || IsE()) r = 0x00A0778;
    else if (IsH()) r = 0x00A0798;
    else if (IsI()) r = 0x00A17A8;
    else if (IsJ()) r = 0x014E8D8;
    LogAddrOnce("RenderContextGlobal", r);
    return r;
}

uintptr_t EFZ_RVA_PracticeControllerPtr() {
    // Disabled intentionally.
    // These RVAs were previously assumed to reference the Practice controller, but
    // they match Revival session-pointer globals used by InGameNetplay and are not
    // safe to treat as Practice objects.
    return 0;
}

uintptr_t EFZ_RVA_PracticeDispatcher() {
    uintptr_t r = 0;
    EfzRevivalVersion v = GetEfzRevivalVersion();
    // Hook only builds where the Practice hotkey evaluator has been verified.
    if (v == EfzRevivalVersion::Revival102e) r = 0x00759F0;  // 1.02e sub_100759F0
    else if (IsFSubframe()) r = 0x0075C90;  // 1.02f subframe sub_10075C90
    else if (IsFClassic()) r = 0x0075A20;  // 1.02f classic sub_10075A20
    else if (v == EfzRevivalVersion::Revival102g) r = 0x0075CC0;  // 1.02g sub_10075CC0
    else if (IsI()) r = 0x0076A30;
    else if (IsH()) r = 0x0076490;
    else if (IsJ()) r = 0x007CF60; // J vtable[4]; save/load bodies are inlined here
    // For unsupported versions: return 0 (don't guess addresses)
    LogAddrOnce("PracticeDispatcher", r);
    return r;
}

// Version-aware Practice controller offset accessors
uintptr_t EFZ_Practice_PauseFlagOffset() {
    if (IsJ()) return 0xDC;
    return 0xB4;  // All versions
}

uintptr_t EFZ_Practice_StepFlagOffset() {
    if (IsJ()) return 0xD4;
    return 0xAC;  // All versions
}

uintptr_t EFZ_Practice_StepCounterOffset() {
    if (IsJ()) return 0xD8;
    return 0xB0;  // All versions
}

uintptr_t EFZ_Practice_PauseHotkeyOffset() {
    EfzRevivalVersion v = GetEfzRevivalVersion();
    if (IsJ()) return 0x200;
    if (v == EfzRevivalVersion::Revival102i) return 0x1D8;
    if (v == EfzRevivalVersion::Revival102e
        || v == EfzRevivalVersion::Revival102f
        || v == EfzRevivalVersion::Revival102g
        || v == EfzRevivalVersion::Revival102h) {
        return 0x1D4;
    }
    return 0;
}

uintptr_t EFZ_Practice_StepHotkeyOffset() {
    EfzRevivalVersion v = GetEfzRevivalVersion();
    if (IsJ()) return 0x204;
    if (v == EfzRevivalVersion::Revival102i) return 0x1DC;
    if (v == EfzRevivalVersion::Revival102e
        || v == EfzRevivalVersion::Revival102f
        || v == EfzRevivalVersion::Revival102g
        || v == EfzRevivalVersion::Revival102h) {
        return 0x1D8;
    }
    return 0;
}

uintptr_t EFZ_Practice_SaveHotkeyOffset() {
    EfzRevivalVersion v = GetEfzRevivalVersion();
    if (IsJ()) return 0x208;
    if (v == EfzRevivalVersion::Revival102i) return 0x1E0;
    if (v == EfzRevivalVersion::Revival102e
        || v == EfzRevivalVersion::Revival102f
        || v == EfzRevivalVersion::Revival102g
        || v == EfzRevivalVersion::Revival102h) {
        return 0x1DC;
    }
    return 0;
}

uintptr_t EFZ_Practice_LoadHotkeyOffset() {
    EfzRevivalVersion v = GetEfzRevivalVersion();
    if (IsJ()) return 0x20C;
    if (v == EfzRevivalVersion::Revival102i) return 0x1E4;
    if (v == EfzRevivalVersion::Revival102e
        || v == EfzRevivalVersion::Revival102f
        || v == EfzRevivalVersion::Revival102g
        || v == EfzRevivalVersion::Revival102h) {
        return 0x1E0;
    }
    return 0;
}

uintptr_t EFZ_Practice_LocalSideOffset() {
    if (IsJ()) return 0; // field removed with J's compact Practice layout
    return IsI() ? 0x688 : 0x680;
}
uintptr_t EFZ_Practice_RemoteSideOffset() {
    if (IsJ()) return 0; // field removed with J's compact Practice layout
    return IsI() ? 0x692 : 0x684;
}

uintptr_t EFZ_Practice_InitSourceSideOffset() {
    if (IsJ()) return 0; // field removed with J's compact Practice layout
    return IsI() ? 0x952 : 0x944;
}

uintptr_t EFZ_Practice_SideBufPrimaryOffset() {
    if (IsJ()) return 0;
    return 0x338;  // 824 decimal = 0x338 hex (ALL VERSIONS)
}
uintptr_t EFZ_Practice_SideBufSecondaryOffset() {
    if (IsJ()) return 0;
    return 0x33C;  // 828 decimal = 0x33C hex (ALL VERSIONS)
}
uintptr_t EFZ_Practice_SharedInputVectorOffset() {
    return IsJ() ? 0 : 0x1240;
}

int EFZ_Practice_MapResetIndexBias() {
    // Map array index base used at init when calling MapReset
    // 1.02i uses (local + 105); e/h use (local + 104)
    return IsJ() ? 0 : (IsI() ? 105 : 104);
}

// Overlay toggle functions - simple bool toggles for display flags
uintptr_t EFZ_RVA_ToggleHurtboxDisplay() {
    uintptr_t r = 0;
    EfzRevivalVersion v = GetEfzRevivalVersion();
    if (IsFSubframe()) r = 0x00753B0;  // 1.02f subframe - toggles this[183]
    else if (IsFClassic()) r = 0x0075170;  // 1.02f classic - toggles this[183]
    else if (v == EfzRevivalVersion::Revival102e) r = 0x0075140;  // 1.02e - toggles this[183]
    else if (v == EfzRevivalVersion::Revival102g) r = 0x00753E0;  // 1.02g - toggles this[183]
    // TODO: Add h/i versions if needed
    LogAddrOnce("ToggleHurtboxDisplay", r);
    return r;
}

uintptr_t EFZ_RVA_ToggleHitboxDisplay() {
    uintptr_t r = 0;
    EfzRevivalVersion v = GetEfzRevivalVersion();
    if (IsFSubframe()) r = 0x00753D0;  // 1.02f subframe - toggles this[182]
    else if (IsFClassic()) r = 0x0075190;  // 1.02f classic - toggles this[182]
    else if (v == EfzRevivalVersion::Revival102e) r = 0x0075160;  // 1.02e - toggles this[182]
    else if (v == EfzRevivalVersion::Revival102g) r = 0x0075400;  // 1.02g - toggles this[182]
    // TODO: Add h/i versions if needed
    LogAddrOnce("ToggleHitboxDisplay", r);
    return r;
}

uintptr_t EFZ_RVA_ToggleFrameDisplay() {
    uintptr_t r = 0;
    EfzRevivalVersion v = GetEfzRevivalVersion();
    if (IsFSubframe()) r = 0x0075950;  // 1.02f subframe - toggles this[181]
    else if (IsFClassic()) r = 0x0075710;  // 1.02f classic - toggles this[181]
    else if (v == EfzRevivalVersion::Revival102e) r = 0x00756E0;  // 1.02e - toggles this[181]
    else if (v == EfzRevivalVersion::Revival102g) r = 0x0075980;  // 1.02g - toggles this[181]
    // TODO: Add h/i versions if needed
    LogAddrOnce("ToggleFrameDisplay", r);
    return r;
}

// Savestate functions - VS/Practice Mode (non-recording)
// These are the actual functions called when user presses save/load keys in Practice mode.
// Routed via PracticeHotkeyHandler: this+476=Save, this+480=Load (except 1.02i: this+480=Save, this+484=Load)
uintptr_t EFZ_RVA_LoadState() {
    uintptr_t r = 0;
    EfzRevivalVersion v = GetEfzRevivalVersion();
    // VS/Practice mode load - restores snapshot
    if (v == EfzRevivalVersion::Revival102e) r = 0x0075910;  // sub_10075910
    else if (IsFSubframe()) r = 0x0075BB0;  // sub_10075BB0
    else if (IsFClassic()) r = 0x0075940;  // sub_10075940
    else if (v == EfzRevivalVersion::Revival102g) r = 0x0075BE0;  // sub_10075BE0
    else if (v == EfzRevivalVersion::Revival102h) r = 0x00763B0;  // sub_100763B0
    else if (v == EfzRevivalVersion::Revival102i) r = 0x0076950;  // sub_10076950
    else if (IsJ()) r = 0x007E040;  // callable J load body; hotkey path is inlined
    LogAddrOnce("LoadState", r);
    return r;
}

uintptr_t EFZ_RVA_SaveState() {
    uintptr_t r = 0;
    EfzRevivalVersion v = GetEfzRevivalVersion();
    // VS/Practice mode save - builds snapshot
    if (v == EfzRevivalVersion::Revival102e) r = 0x0075980;  // sub_10075980
    else if (IsFSubframe()) r = 0x0075C20;  // sub_10075C20
    else if (IsFClassic()) r = 0x00759B0;  // sub_100759B0
    else if (v == EfzRevivalVersion::Revival102g) r = 0x0075C50;  // sub_10075C50
    else if (v == EfzRevivalVersion::Revival102h) r = 0x0076420;  // sub_10076420
    else if (v == EfzRevivalVersion::Revival102i) r = 0x00769C0;  // sub_100769C0
    else if (IsJ()) r = 0x007E0F0;  // callable J save body; hotkey path is inlined
    LogAddrOnce("SaveState", r);
    return r;
}

uintptr_t EFZ_RVA_PracticeHotkeyHandler() {
    uintptr_t r = 0;
    EfzRevivalVersion v = GetEfzRevivalVersion();
    // VS/Practice mode hotkey handler - routes key presses to save/load
    // e/f/g/h: this+476=Save, this+480=Load
    // i: this+480=Save, this+484=Load (offsets shifted by 4)
    if (v == EfzRevivalVersion::Revival102e) r = 0x00759F0;  // sub_100759F0
    else if (IsFSubframe()) r = 0x0075C90;  // sub_10075C90
    else if (IsFClassic()) r = 0x0075A20;  // sub_10075A20
    else if (v == EfzRevivalVersion::Revival102g) r = 0x0075CC0;  // sub_10075CC0
    else if (v == EfzRevivalVersion::Revival102h) r = 0x0076490;  // sub_10076490
    else if (v == EfzRevivalVersion::Revival102i) r = 0x0076A30;  // sub_10076A30
    else if (IsJ()) r = 0x007CF60;  // sub_7007CF60, J vtable[4]
    LogAddrOnce("PracticeHotkeyHandler", r);
    return r;
}

// Replay/Recording mode savestate functions (different code path, kept for reference)
// These are only used when recording is active - NOT in normal Practice mode!
uintptr_t EFZ_RVA_ReplayLoadState() {
    uintptr_t r = 0;
    EfzRevivalVersion v = GetEfzRevivalVersion();
    if (v == EfzRevivalVersion::Revival102e) r = 0x0077060;
    else if (v == EfzRevivalVersion::Revival102f) r = 0x0077090;
    else if (v == EfzRevivalVersion::Revival102g) r = 0x0077330;
    else if (v == EfzRevivalVersion::Revival102h) r = 0x0077B00;
    else if (v == EfzRevivalVersion::Revival102i) r = 0x00780E0;
    LogAddrOnce("ReplayLoadState", r);
    return r;
}

uintptr_t EFZ_RVA_ReplaySaveState() {
    uintptr_t r = 0;
    EfzRevivalVersion v = GetEfzRevivalVersion();
    if (v == EfzRevivalVersion::Revival102e) r = 0x00770C0;
    else if (v == EfzRevivalVersion::Revival102f) r = 0x00770F0;
    else if (v == EfzRevivalVersion::Revival102g) r = 0x0077390;
    else if (v == EfzRevivalVersion::Revival102h) r = 0x0077B60;
    else if (v == EfzRevivalVersion::Revival102i) r = 0x0078140;
    LogAddrOnce("ReplaySaveState", r);
    return r;
}
