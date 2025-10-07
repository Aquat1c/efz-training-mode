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
    return GetEfzRevivalVersion() == EfzRevivalVersion::Revival102h;
}

static void LogAddrOnce(const char* label, uintptr_t rva) {
    static std::atomic<bool> s_once{false};
    if (s_once.exchange(true)) return;
    std::ostringstream oss;
    oss << "[ADDR] Version=" << (IsE()?"1.02e": IsH()?"1.02h":"other")
        << " | " << label << " RVA=0x" << std::hex << rva;
    HMODULE h = GetModuleHandleA("EfzRevival.dll");
    if (h && rva) {
        oss << " VA=0x" << (reinterpret_cast<uintptr_t>(h) + rva);
    }
    LogOut(oss.str(), true);
}

int EFZ_PatchToggleUnfreezeParam() {
    if (IsH()) return 3; // 1.02h uses 0=freeze, 3=unfreeze
    return 1;            // 1.02e uses 0=freeze, 1=unfreeze
}

uintptr_t EFZ_RVA_PatchToggler() {
    uintptr_t r = 0;
    if (IsE()) r = 0x006B2A0;
    else if (IsH()) r = 0x006BB00; // candidate confirmed by decomp traces
    LogAddrOnce("PatchToggler", r);
    return r;
}

uintptr_t EFZ_RVA_PatchCtx() {
    uintptr_t r = 0;
    if (IsE()) r = 0x00A0760;
    else if (IsH()) r = 0x00A0780; // dword_100A0780
    LogAddrOnce("PatchCtx", r);
    return r;
}

uintptr_t EFZ_RVA_TogglePause() {
    uintptr_t r = 0;
    if (IsE()) r = 0x0075720;  // sub_10075720
    else if (IsH()) r = 0x0076170;  // sub_10076170 (function label minus 0x1000000)
    LogAddrOnce("TogglePause", r);
    return r;
}

uintptr_t EFZ_RVA_PracticeTick() {
    uintptr_t r = 0;
    if (IsE()) r = 0x0074F70;  // sub_10074F70
    else if (IsH()) r = 0x0074F40;  // nearby in 1.02h
    LogAddrOnce("PracticeTick", r);
    return r;
}

uintptr_t EFZ_RVA_RefreshMappingBlock() {
    uintptr_t r = 0;
    if (IsE()) r = 0x0075100;  // sub_10075100
    else if (IsH()) r = 0x0074E80;  // candidate cluster around 74Exx
    LogAddrOnce("RefreshMappingBlock", r);
    return r;
}

uintptr_t EFZ_RVA_MapReset() {
    uintptr_t r = 0;
    if (IsE()) r = 0x006D640;  // sub_1006D640
    else if (IsH()) r = 0x006D5B0;  // sub_1006D5B0
    LogAddrOnce("MapReset", r);
    return r;
}

uintptr_t EFZ_RVA_CleanupPair() {
    uintptr_t r = 0;
    if (IsE()) r = 0x006CAD0;  // sub_1006CAD0
    else if (IsH()) r = 0x006CE00;  // sub_1006CE00
    LogAddrOnce("CleanupPair", r);
    return r;
}

uintptr_t EFZ_RVA_RenderBattleScreen() {
    uintptr_t r = 0;
    if (IsE()) r = 0x007642A0;
    else if (IsH()) r = 0x007642A0; // assume stable; adjust if needed
    LogAddrOnce("RenderBattleScreen", r);
    return r;
}

uintptr_t EFZ_RVA_GameModePtrArray() {
    uintptr_t r = 0;
    if (IsE()) r = 0x790110;
    else if (IsH()) r = 0x790110; // likely unchanged; fast-path only
    LogAddrOnce("GameModePtrArray", r);
    return r;
}
