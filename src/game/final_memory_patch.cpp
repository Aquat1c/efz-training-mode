#include <windows.h>
#include <vector>
#include <cstdint>
#include <string>
#include <mutex>
#include "../../include/core/logger.h"
#include "../../include/core/memory.h"
#include "../../include/game/final_memory_patch.h"

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

static bool ScanAndPatch(uint32_t fromImm, uint32_t toImm, std::vector<uint8_t*>* outSites) {
    uint8_t* text = nullptr; size_t textSize = 0;
    if (!GetTextSection(&text, &textSize)) {
        LogOut("[FM_PATCH] Failed to locate .text section", true);
        return false;
    }

    // Robust scan: look for 81 /7 (CMP r/m32, imm32) with operand [reg + disp32] where disp32 == 0x108 (HP), imm32 == 0x0D05 (3333)
    // Then rewrite imm32 to the requested value so the test behaves accordingly.
    const uint32_t HP_OFFSET_DISP = 0x00000108;
    const uint32_t FROM = fromImm;
    const uint32_t TO   = toImm;

    int rewrites = 0;

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
            if (imm != FROM) continue;
            // Patch immediate
            uint8_t* immPtr = const_cast<uint8_t*>(p + dispOff + 4);
            DWORD oldProt;
            if (VirtualProtect(immPtr, 4, PAGE_EXECUTE_READWRITE, &oldProt)) {
                memcpy(immPtr, &TO, sizeof(TO));
                DWORD _tmp; VirtualProtect(immPtr, 4, oldProt, &_tmp);
                ++rewrites;
                if (rewrites <= 4 || detailedLogging.load()) {
                    char buf[160];
                    sprintf_s(buf, "[FM_PATCH] Patched HP compare imm at %p: %u -> %u (cmp [*+0x108], imm32)", immPtr, FROM, TO);
                    LogOut(buf, true);
                }
                if (outSites) outSites->push_back(immPtr);
            }
        }
    }

    LogOut(std::string("[FM_PATCH] HP compare immediates updated: ") + std::to_string(rewrites), true);
    return rewrites > 0;
}

static int PatchToBypass(std::vector<uint8_t*>* outSites) {
    const uint32_t FM_THRESH = 0x00000D05;     // 3333
    const uint32_t BYPASS    = 0x00002710;     // 10000
    int count = 0;
    if (ScanAndPatch(FM_THRESH, BYPASS, outSites)) {
        // count equals outSites size delta; but ScanAndPatch already logged
        count = (int)(outSites ? outSites->size() : 0);
    }
    return count;
}

static int PatchToOriginalFromBypass() {
    // Re-scan for sites that currently have imm=10000 and restore to 3333
    const uint32_t FM_THRESH = 0x00000D05;     // 3333
    const uint32_t BYPASS    = 0x00002710;     // 10000
    // We don't collect sites when reverting via scan
    std::vector<uint8_t*> dummy;
    int count = 0;
    if (ScanAndPatch(BYPASS, FM_THRESH, nullptr)) {
        // Unknown exact count; we will log via ScanAndPatch. Return 1+ to indicate work done.
        count = 1;
    }
    return count;
}

int ApplyFinalMemoryHPBypass() {
    std::lock_guard<std::mutex> _lk(g_fmMutex);
    if (!g_fmBypassSites.empty()) {
        // Already applied in this session
        return 0;
    }
    std::vector<uint8_t*> sites;
    int before = (int)sites.size();
    PatchToBypass(&sites);
    int added = (int)sites.size() - before;
    if (added > 0) {
        g_fmBypassSites.insert(g_fmBypassSites.end(), sites.begin(), sites.end());
    }
    return added;
}

int RevertFinalMemoryHPBypass() {
    std::lock_guard<std::mutex> _lk(g_fmMutex);
    int reverted = 0;
    const uint32_t FM_THRESH = 0x00000D05;
    const uint32_t BYPASS    = 0x00002710;
    if (!g_fmBypassSites.empty()) {
        for (auto* immPtr : g_fmBypassSites) {
            if (!immPtr) continue;
            DWORD oldProt;
            if (VirtualProtect(immPtr, 4, PAGE_EXECUTE_READWRITE, &oldProt)) {
                memcpy(immPtr, &FM_THRESH, sizeof(FM_THRESH));
                DWORD _tmp; VirtualProtect(immPtr, 4, oldProt, &_tmp);
                ++reverted;
            }
        }
        g_fmBypassSites.clear();
        LogOut(std::string("[FM_PATCH] Reverted FM HP bypass at tracked sites: ") + std::to_string(reverted), true);
        return reverted;
    }
    // Fallback: conservative scan and restore compares currently set to BYPASS value
    int rescanned = PatchToOriginalFromBypass();
    LogOut("[FM_PATCH] Reverted FM HP bypass via rescan.", true);
    return rescanned;
}

int SetFinalMemoryBypass(bool enabled) {
    if (enabled) return ApplyFinalMemoryHPBypass();
    return RevertFinalMemoryHPBypass();
}

bool IsFinalMemoryBypassEnabled() {
    std::lock_guard<std::mutex> _lk(g_fmMutex);
    return !g_fmBypassSites.empty();
}
