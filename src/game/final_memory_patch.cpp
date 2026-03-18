#include <windows.h>
#include <vector>
#include <cstdint>
#include <string>
#include <mutex>
#include <sstream>
#include "../../include/core/logger.h"
#include "../../include/core/memory.h"
#include "../../include/game/final_memory_patch.h"
#include "../../include/game/game_state.h"
#include "../../include/utils/utilities.h" // for g_onlineModeActive

namespace {
constexpr uint32_t kFinalMemoryThreshold = 0x00000D05; // 3333
constexpr uint32_t kFinalMemoryBypass    = 0x00002710; // 10000
}

// Helper: get module .text section bounds
static bool GetTextSection(uint8_t** start, size_t* size) {
    HMODULE hMod = GetModuleHandleA(nullptr);
    if (!hMod) return false;
    auto base = reinterpret_cast<uint8_t*>(hMod);
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    auto sec = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        const IMAGE_SECTION_HEADER& s = sec[i];
        if (memcmp(s.Name, ".text", 5) == 0) {
            *start = base + s.VirtualAddress;
            *size = s.Misc.VirtualSize;
            return true;
        }
    }
    return false;
}

extern std::atomic<bool> detailedLogging;

// Track patched immediate addresses so we can revert precisely
static std::vector<uint8_t*> g_fmBypassSites;
static std::mutex g_fmMutex;
static std::atomic<bool> g_fmBypassRequested{false};

static int FindMatchingHpCompareSites(uint32_t immValue, std::vector<uint8_t*>* outSites) {
    uint8_t* text = nullptr; size_t textSize = 0;
    if (!GetTextSection(&text, &textSize)) {
        LogOut("[FM_PATCH] Failed to locate .text section", true);
        return 0;
    }

    // Robust scan: look for 81 /7 (CMP r/m32, imm32) with operand [reg + disp32] where disp32 == 0x108 (HP), imm32 == 0x0D05 (3333)
    // Then rewrite imm32 to the requested value so the test behaves accordingly.
    const uint32_t HP_OFFSET_DISP = 0x00000108;

    int matches = 0;

    for (size_t i = 0; i + 10 < textSize; ++i) {
        const uint8_t* p = text + i;
        if (p[0] != 0x81) continue; // CMP /7 uses 0x81 with modrm.reg = 7
        uint8_t modrm = p[1];
        uint8_t reg = (modrm >> 3) & 0x7;
        uint8_t mod = (modrm >> 6) & 0x3;
        uint8_t rm  = modrm & 0x7;
        if (reg != 7) continue; // not CMP
        // Only memory operands with disp32: mod == 2 and rm != 4 (no SIB)
        size_t dispOff = 2;
        if (mod == 2) {
            if (rm == 4) {
                // SIB present; skip for simplicity
                continue;
            }
            if (i + dispOff + 4 + 4 > textSize) continue;
            uint32_t disp = *reinterpret_cast<const uint32_t*>(p + dispOff);
            if (disp != HP_OFFSET_DISP) continue;
            uint32_t imm = *reinterpret_cast<const uint32_t*>(p + dispOff + 4);
            if (imm != immValue) continue;
            uint8_t* immPtr = const_cast<uint8_t*>(p + dispOff + 4);
            if (outSites) outSites->push_back(immPtr);
            ++matches;
        }
    }

    return matches;
}

static int RewriteImmediateSites(
    const std::vector<uint8_t*>& sites,
    uint32_t fromImm,
    uint32_t toImm,
    const char* phaseTag) {
    int rewrites = 0;
    for (uint8_t* immPtr : sites) {
        if (!immPtr) continue;
        DWORD oldProt = 0;
        if (!VirtualProtect(immPtr, sizeof(uint32_t), PAGE_EXECUTE_READWRITE, &oldProt)) {
            continue;
        }
        memcpy(immPtr, &toImm, sizeof(toImm));
        DWORD dummy = 0;
        VirtualProtect(immPtr, sizeof(uint32_t), oldProt, &dummy);
        ++rewrites;
        if (rewrites <= 4 || detailedLogging.load()) {
            char buf[192];
            sprintf_s(
                buf,
                "[FM_PATCH] %s HP compare imm at %p: %u -> %u (cmp [*+0x108], imm32)",
                phaseTag ? phaseTag : "Rewrote",
                immPtr,
                fromImm,
                toImm);
            LogOut(buf, true);
        }
    }
    LogOut(
        std::string("[FM_PATCH] HP compare immediates updated: ")
        + std::to_string(rewrites),
        true);
    return rewrites;
}

static int RecoverTrackedBypassSitesLocked(const char* reason) {
    if (!g_fmBypassSites.empty()) {
        return static_cast<int>(g_fmBypassSites.size());
    }
    std::vector<uint8_t*> recovered;
    const int found = FindMatchingHpCompareSites(kFinalMemoryBypass, &recovered);
    if (found > 0) {
        g_fmBypassSites = recovered;
        std::ostringstream oss;
        oss << "[FM_PATCH] Recovered " << found << " active bypass site(s)";
        if (reason && *reason) {
            oss << " reason=" << reason;
        }
        LogOut(oss.str(), true);
    }
    return found;
}

int ApplyFinalMemoryHPBypass() {
    // CRITICAL: Never modify game code during online mode
    if (g_onlineModeActive.load()) return 0;

    std::lock_guard<std::mutex> _lk(g_fmMutex);
    if (!g_fmBypassSites.empty()) {
        // Already applied in this session
        return 0;
    }

    std::vector<uint8_t*> sites;
    const int foundOriginal = FindMatchingHpCompareSites(kFinalMemoryThreshold, &sites);
    if (foundOriginal > 0) {
        const int rewritten = RewriteImmediateSites(
            sites,
            kFinalMemoryThreshold,
            kFinalMemoryBypass,
            "Patched");
        if (rewritten > 0) {
            g_fmBypassSites = sites;
            LogOut(
                std::string("[FM_PATCH] Applied FM HP bypass at tracked sites: ")
                + std::to_string(rewritten),
                true);
        }
        return rewritten;
    }

    if (RecoverTrackedBypassSitesLocked("apply requested") > 0) {
        LogOut("[FM_PATCH] FM HP bypass was already active; recovered tracked state", true);
    } else if (detailedLogging.load()) {
        LogOut("[FM_PATCH] No original FM HP compare sites found to patch", true);
    }
    return 0;
}

int RevertFinalMemoryHPBypass() {
    std::lock_guard<std::mutex> _lk(g_fmMutex);
    std::vector<uint8_t*> sites = g_fmBypassSites;
    if (sites.empty()) {
        RecoverTrackedBypassSitesLocked("restore requested");
        sites = g_fmBypassSites;
    }
    if (sites.empty()) {
        if (detailedLogging.load()) {
            LogOut("[FM_PATCH] Revert requested but no active FM HP bypass sites were found", true);
        }
        return 0;
    }

    const int reverted = RewriteImmediateSites(
        sites,
        kFinalMemoryBypass,
        kFinalMemoryThreshold,
        "Restored");
    g_fmBypassSites.clear();
    LogOut(
        std::string("[FM_PATCH] Reverted FM HP bypass at tracked sites: ")
        + std::to_string(reverted),
        true);
    return reverted;
}

int SetFinalMemoryBypass(bool enabled) {
    const bool previous = g_fmBypassRequested.exchange(enabled, std::memory_order_release);
    if (previous != enabled) {
        LogOut(
            std::string("[FM_PATCH] Request ")
            + (enabled ? "enabled" : "disabled")
            + " by local training runtime",
            true);
    }
    return SyncFinalMemoryBypassForCurrentMode("SetFinalMemoryBypass");
}

bool IsFinalMemoryBypassEnabled() {
    return g_fmBypassRequested.load(std::memory_order_acquire);
}

bool IsFinalMemoryBypassInstalled() {
    std::lock_guard<std::mutex> _lk(g_fmMutex);
    if (!g_fmBypassSites.empty()) {
        return true;
    }
    return FindMatchingHpCompareSites(kFinalMemoryBypass, nullptr) > 0;
}

int SyncFinalMemoryBypassForCurrentMode(const char* reason) {
    const bool requested = g_fmBypassRequested.load(std::memory_order_acquire);
    const bool featuresActive = g_featuresEnabled.load(std::memory_order_acquire);
    const bool onlineSuspended = g_onlineModeActive.load(std::memory_order_acquire);
    const GameMode currentMode = GetCurrentGameMode();
    const GamePhase currentPhase = GetCurrentGamePhase();
    const bool inPracticeMatch = (currentMode == GameMode::Practice) && (currentPhase == GamePhase::Match);
    const bool shouldInstall = requested && featuresActive && !onlineSuspended && inPracticeMatch;

    if (shouldInstall) {
        const int applied = ApplyFinalMemoryHPBypass();
        if (applied > 0) {
            std::ostringstream oss;
            oss << "[FM_PATCH] Sync applied"
                << " requested=1 features=" << (featuresActive ? "1" : "0")
                << " online=" << (onlineSuspended ? "1" : "0")
                << " mode=" << GetGameModeName(currentMode)
                << " phase=" << static_cast<int>(currentPhase);
            if (reason && *reason) {
                oss << " reason=" << reason;
            }
            LogOut(oss.str(), true);
        }
        return applied;
    }

    const int reverted = RevertFinalMemoryHPBypass();
    if (requested && reverted == 0 && detailedLogging.load()) {
        std::ostringstream oss;
        oss << "[FM_PATCH] Request held but patch not installed"
            << " requested=1 features=" << (featuresActive ? "1" : "0")
            << " online=" << (onlineSuspended ? "1" : "0")
            << " mode=" << GetGameModeName(currentMode)
            << " phase=" << static_cast<int>(currentPhase);
        if (reason && *reason) {
            oss << " reason=" << reason;
        }
        LogOut(oss.str(), true);
    }
    if (!requested && reverted > 0) {
        std::ostringstream oss;
        oss << "[FM_PATCH] Sync restored original FM checks";
        if (reason && *reason) {
            oss << " reason=" << reason;
        }
        LogOut(oss.str(), true);
    }
    return reverted;
}

int ForceRestoreFinalMemoryHPBypass(const char* reason) {
    const int reverted = RevertFinalMemoryHPBypass();
    if (reverted > 0 || detailedLogging.load()) {
        std::ostringstream oss;
        oss << "[FM_PATCH] Forced restore"
            << " requested=" << (IsFinalMemoryBypassEnabled() ? "1" : "0")
            << " reverted=" << reverted;
        if (reason && *reason) {
            oss << " reason=" << reason;
        }
        LogOut(oss.str(), true);
    }
    return reverted;
}
