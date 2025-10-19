#include "../include/game/efzrevival_scanner.h"
#include "../include/core/logger.h"
#include "../include/utils/utilities.h" // for detailedLogging
#include "../include/core/memory.h"
#include "../include/game/efzrevival_addrs.h"
#include <vector>
#include <atomic>
#include <cstring>
#include <sstream>
#include <iomanip>
#include <unordered_map>
#include <algorithm>
#include <string>
#include <cctype>

// Avoid Windows macros clobbering std::min/std::max
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

namespace {
    std::atomic<bool> g_scanned{ false };
    EfzSigScanner::Results g_results{};

    struct Section {
        uint8_t* base;
        size_t size;
        uint32_t rva;
        uint32_t characteristics;
        char name[9];
    };

    bool GetModule(HMODULE& mod, uint8_t*& imageBase, size_t& imageSize) {
        mod = GetModuleHandleA("EfzRevival.dll");
        if (!mod) return false;
        imageBase = reinterpret_cast<uint8_t*>(mod);
        auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(imageBase);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
        auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(imageBase + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
        imageSize = nt->OptionalHeader.SizeOfImage;
        return true;
    }

    static bool SigDebug() {
        static int s = -1;
        if (s < 0) {
            char buf[8] = {0};
            DWORD n = GetEnvironmentVariableA("EFZ_SIG_DEBUG", buf, sizeof(buf));
            s = (n > 0 && buf[0] == '1') ? 1 : 0;
        }
        return s == 1;
    }

    void LogBytes(const char* label, uint8_t* addr, size_t count) {
        std::ostringstream oss; oss << label << ": ";
        for (size_t i = 0; i < count; ++i) {
            oss << std::hex << std::uppercase << std::setfill('0') << std::setw(2) << (int)addr[i] << ' ';
        }
        LogOut(oss.str(), true);
    }

    int CountRetWithin(uint8_t* start, size_t maxBytes) {
        int cnt = 0; for (size_t i = 0; i < maxBytes; ++i) { if (start[i] == 0xC3 || start[i] == 0xC2) ++cnt; }
        return cnt;
    }

    int CountCallsToTarget(uint8_t* textBase, size_t textSize, uint8_t* target) {
        int cnt = 0; for (size_t i = 0; i + 5 <= textSize; ++i) { if (textBase[i] == 0xE8) {
            int32_t rel = *reinterpret_cast<int32_t*>(textBase + i + 1);
            uint8_t* dest = (textBase + i + 5) + rel; if (dest == target) ++cnt; } }
        return cnt;
    }

    bool HasRepMovsNear(uint8_t* func, size_t window) {
        for (size_t i = 0; i + 1 < window; ++i) {
            if (func[i] == 0xF3 && (func[i+1] == 0xA5 || func[i+1] == 0xA4)) return true;
        }
        return false;
    }

    bool GetModuleByHandle(HMODULE mod, uint8_t*& imageBase, size_t& imageSize) {
        if (!mod) return false;
        imageBase = reinterpret_cast<uint8_t*>(mod);
        auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(imageBase);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
        auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(imageBase + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
        imageSize = nt->OptionalHeader.SizeOfImage;
        return true;
    }

    bool GetSections(std::vector<Section>& out) {
        HMODULE mod; uint8_t* base; size_t size;
        if (!GetModule(mod, base, size)) return false;
        auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
        auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        auto sec = IMAGE_FIRST_SECTION(nt);
        out.clear();
        for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
            Section s{};
            s.base = base + sec[i].VirtualAddress;
            s.size = sec[i].Misc.VirtualSize;
            s.rva = sec[i].VirtualAddress;
            s.characteristics = sec[i].Characteristics;
            memset(s.name, 0, sizeof(s.name));
            memcpy(s.name, sec[i].Name, (sizeof(sec[i].Name) < sizeof(s.name) ? sizeof(sec[i].Name) : sizeof(s.name) - 1));
            out.push_back(s);
        }
        return true;
    }

    // (removed GetSectionsOf - not needed after dropping exe online scan)

    // Simple byte-pattern search
    uint8_t* FindBytes(uint8_t* hay, size_t hlen, const uint8_t* pat, const char* mask) {
        size_t mlen = std::strlen(mask);
        if (mlen == 0 || hlen < mlen) return nullptr;
        for (size_t i = 0; i <= hlen - mlen; ++i) {
            bool ok = true;
            for (size_t j = 0; j < mlen; ++j) {
                if (mask[j] != '?' && hay[i + j] != pat[j]) { ok = false; break; }
            }
            if (ok) return hay + i;
        }
        return nullptr;
    }

    // Find all occurrences of a UTF-16LE literal in the module; returns VAs
    std::vector<uintptr_t> FindWideStringVAs(const wchar_t* wstr) {
        std::vector<uintptr_t> hits;
        if (!wstr || !*wstr) return hits;
        size_t wlen = wcslen(wstr) + 1; // include null terminator for stronger match
        const uint8_t* pat = reinterpret_cast<const uint8_t*>(wstr);
        std::vector<Section> secs; if (!GetSections(secs)) return hits;
        for (auto &s : secs) {
            // scan all sections; strings typically in .rdata/.data
            for (size_t off = 0; off + wlen * sizeof(wchar_t) <= s.size; ++off) {
                if (memcmp(s.base + off, pat, wlen * sizeof(wchar_t)) == 0) {
                    hits.push_back(reinterpret_cast<uintptr_t>(s.base + off));
                }
            }
        }
        return hits;
    }

    // Find all occurrences of an ASCII literal in the module; returns VAs
    std::vector<uintptr_t> FindAsciiStringVAs(const char* str) {
        std::vector<uintptr_t> hits;
        if (!str || !*str) return hits;
        size_t len = std::strlen(str) + 1; // include null terminator for stronger match
        const uint8_t* pat = reinterpret_cast<const uint8_t*>(str);
        std::vector<Section> secs; if (!GetSections(secs)) return hits;
        for (auto &s : secs) {
            // scan all readable sections; strings usually in .rdata/.data
            for (size_t off = 0; off + len <= s.size; ++off) {
                if (memcmp(s.base + off, pat, len) == 0) {
                    hits.push_back(reinterpret_cast<uintptr_t>(s.base + off));
                }
            }
        }
        return hits;
    }

    // Find PUSH imm32 xrefs to a target VA inside executable sections; returns RVAs of callsites
    std::vector<uintptr_t> FindPushImmXrefs(uintptr_t targetVa) {
        std::vector<uintptr_t> refs;
        std::vector<Section> secs; if (!GetSections(secs)) return refs;
        for (auto &s : secs) {
            if (!(s.characteristics & IMAGE_SCN_MEM_EXECUTE)) continue; // only code
            uint8_t* p = s.base; size_t n = s.size;
            while (n >= 5) {
                if (p[0] == 0x68) { // push imm32
                    uintptr_t va = *reinterpret_cast<uint32_t*>(p + 1);
                    if (va == targetVa) {
                        uintptr_t rva = static_cast<uintptr_t>(s.rva + static_cast<uint32_t>(p - s.base));
                        refs.push_back(rva);
                    }
                }
                ++p; --n;
            }
        }
        return refs;
    }

    // Backtrack from an RVA inside a section to the function prologue (55 8B EC) within the same section
    uintptr_t FindFunctionStartRva(uintptr_t rvaWithin) {
        std::vector<Section> secs; if (!GetSections(secs)) return 0;
        HMODULE mod; uint8_t* base; size_t imageSize; if (!GetModule(mod, base, imageSize)) return 0;
        for (auto &s : secs) {
            if (rvaWithin < s.rva || rvaWithin >= s.rva + s.size) continue;
            size_t offset = static_cast<size_t>(rvaWithin - s.rva);
            uint8_t* p = s.base + offset;
            // search back up to 256 bytes for prologue
            size_t back = (offset < 256) ? offset : 256;
            for (size_t i = 0; i < back; ++i) {
                uint8_t* f = p - i;
                if (f - s.base >= 3 && f[-3] == 0x55 && f[-2] == 0x8B && f[-1] == 0xEC) {
                    return static_cast<uintptr_t>(s.rva + static_cast<uint32_t>(f - s.base - 3));
                }
            }
            // fallback: if this section begins with a prologue at its start
            if (s.size >= 3 && s.base[0] == 0x55 && s.base[1] == 0x8B && s.base[2] == 0xEC) return s.rva;
            return 0;
        }
        return 0;
    }

    // Forward declaration for data xref finder used by anchor hit maps
    std::vector<uintptr_t> FindDataImmXrefs(uintptr_t targetVa);

    // Build a map of functionRva -> hitCount for a given wide string anchor (by imm-address xrefs)
    std::unordered_map<uintptr_t, int> BuildAnchorFunctionHitMap(const wchar_t* wstr) {
        std::unordered_map<uintptr_t, int> hits;
        auto vas = FindWideStringVAs(wstr);
        for (auto va : vas) {
            auto xrefs = FindDataImmXrefs(va);
            for (auto xrva : xrefs) {
                auto frva = FindFunctionStartRva(xrva);
                if (frva) hits[frva]++;
            }
        }
        return hits;
    }

    // ASCII variant of anchor hit map builder
    std::unordered_map<uintptr_t, int> BuildAnchorFunctionHitMapAscii(const char* str) {
        std::unordered_map<uintptr_t, int> hits;
        auto vas = FindAsciiStringVAs(str);
        for (auto va : vas) {
            auto xrefs = FindDataImmXrefs(va);
            for (auto xrva : xrefs) {
                auto frva = FindFunctionStartRva(xrva);
                if (frva) hits[frva]++;
            }
        }
        return hits;
    }

    // Merge multiple anchor maps into one aggregated hit map
    std::unordered_map<uintptr_t, int> MergeAnchorMaps(const std::vector<std::unordered_map<uintptr_t,int>>& maps) {
        std::unordered_map<uintptr_t, int> agg;
        for (auto &m : maps) {
            for (auto &kv : m) agg[kv.first] += kv.second;
        }
        return agg;
    }

    // Heuristic: locate online-status guard: cmp dword ptr [imm32], 2 (supports 83 3D and 81 3D forms)
    uintptr_t ScanOnlineStatusRva() {
        std::vector<Section> secs; if (!GetSections(secs)) return 0;
        HMODULE mod; uint8_t* base; size_t imageSize; if (!GetModule(mod, base, imageSize)) return 0;
        // Aggregate candidate globals: imm32 that is compared to small constants 0..3 from multiple sites
        std::unordered_map<uintptr_t, int> counts;
        auto bump = [&](uintptr_t va){ if (va >= reinterpret_cast<uintptr_t>(base) && va < reinterpret_cast<uintptr_t>(base) + imageSize) counts[va]++; };
        auto bump_if = [&](bool cond, uintptr_t va){ if (cond) bump(va); };
        for (auto& s : secs) {
            // Direct cmp patterns (dword and byte)
            for (size_t off = 0; off + 7 <= s.size; ++off) {
                uint8_t* p = s.base + off;
                // 83 3D imm32 imm8
                if (p[0] == 0x83 && p[1] == 0x3D) {
                    uintptr_t va = *reinterpret_cast<uint32_t*>(p + 2);
                    uint8_t imm8 = p[6];
                    if (imm8 <= 3) bump(va);
                }
                // 81 3D imm32 imm32
                if (p[0] == 0x81 && p[1] == 0x3D) {
                    uintptr_t va = *reinterpret_cast<uint32_t*>(p + 2);
                    uint32_t imm32 = *reinterpret_cast<uint32_t*>(p + 6);
                    if (imm32 <= 3) bump(va);
                }
                // 80 3D imm32 imm8
                if (p[0] == 0x80 && p[1] == 0x3D) {
                    uintptr_t va = *reinterpret_cast<uint32_t*>(p + 2);
                    uint8_t imm8 = p[6];
                    if (imm8 <= 3) bump(va);
                }
            }

            // mov eax, [imm32]; cmp eax, imm
            for (size_t off = 0; off + 10 <= s.size; ++off) {
                uint8_t* p = s.base + off;
                if (p[0] == 0xA1) {
                    uintptr_t va = *reinterpret_cast<uint32_t*>(p + 1);
                    uint8_t* q = p + 5; size_t m = s.size - off - 5; if (m >= 3) {
                        if (q[0] == 0x83 && q[1] == 0xF8 && q[2] <= 3) bump(va);
                        if (m >= 5 && q[0] == 0x3D && *reinterpret_cast<uint32_t*>(q + 1) <= 3) bump(va);
                    }
                }
                // movzx eax, byte ptr [imm32]; cmp eax, imm
                if (p[0] == 0x0F && p[1] == 0xB6 && p[2] == 0x05 && off + 11 <= s.size) {
                    uintptr_t va = *reinterpret_cast<uint32_t*>(p + 3);
                    uint8_t* q = p + 7; size_t m = s.size - off - 7;
                    if (m >= 3 && q[0] == 0x83 && q[1] == 0xF8 && q[2] <= 3) bump(va);
                    if (m >= 5 && q[0] == 0x3D && *reinterpret_cast<uint32_t*>(q + 1) <= 3) bump(va);
                }
            }
        }
        // Choose the most-referenced candidate
        uintptr_t bestVa = 0; int bestCount = 0;
        for (auto &kv : counts) { if (kv.second > bestCount) { bestCount = kv.second; bestVa = kv.first; } }
        if (!bestVa) return 0;
        return bestVa - reinterpret_cast<uintptr_t>(base);
    }

    // (removed ScanOnlineStatusRvaEx - stick to EfzRevival.dll only)

    // Heuristic: locate pause tail helper that reads GameMode(3)->+1400 and calls PatchToggler(ctx, 0/3)
    // We'll scan for the read of +1400 and a nearby call; then backtrack to find the called function address.
    // Robustly find PatchToggler and PatchCtx via callsite pattern: mov ecx, imm32; push {0,1,3}; call rel32
    uintptr_t ScanPatchTogglerRva(uintptr_t& outCtxRva) {
        outCtxRva = 0;
        std::vector<Section> secs; if (!GetSections(secs)) return 0;
        HMODULE mod; uint8_t* base; size_t imageSize; if (!GetModule(mod, base, imageSize)) return 0;

        struct Candidate { uintptr_t targetRva; uintptr_t ctxRva; int count; };
        std::vector<Candidate> cands;

        auto bump = [&](uintptr_t trg, uintptr_t ctx){
            for (auto &c : cands) {
                if (c.targetRva == trg) { c.count++; if (!c.ctxRva) c.ctxRva = ctx; return; }
            }
            cands.push_back(Candidate{trg, ctx, 1});
        };

        for (auto& s : secs) {
            uint8_t* p = s.base; size_t n = s.size;
            while (n >= 10) {
                // mov ecx, imm32
                if (p[0] == 0xB9) {
                    uintptr_t ctxVa = *reinterpret_cast<uint32_t*>(p + 1);
                    if (!(ctxVa >= reinterpret_cast<uintptr_t>(base) && ctxVa < reinterpret_cast<uintptr_t>(base) + imageSize)) { ++p; --n; continue; }
                    // look ahead for push imm8 {0,1,3} then call rel32
                    uint8_t* q = p + 5; size_t m = (n >= 10) ? n - 5 : 0;
                    for (size_t i = 0; i + 5 < m; ++i) {
                        if (q[i] == 0x6A) {
                            uint8_t imm8 = q[i + 1];
                            if (imm8 == 0 || imm8 == 1 || imm8 == 3) {
                                if (q[i + 2] == 0xE8) {
                                    int32_t rel = *reinterpret_cast<int32_t*>(q + i + 3);
                                    uint8_t* target = (q + i + 7) + rel;
                                    if (target >= base && target < base + imageSize) {
                                        uintptr_t trgRva = reinterpret_cast<uintptr_t>(target) - reinterpret_cast<uintptr_t>(base);
                                        uintptr_t ctxRva = ctxVa - reinterpret_cast<uintptr_t>(base);
                                        bump(trgRva, ctxRva);
                                    }
                                }
                            }
                        }
                    }
                }
                ++p; --n;
            }
        }
        // Choose target with highest count
        int bestCount = 0; uintptr_t bestTrg = 0; uintptr_t bestCtx = 0;
        for (auto& c : cands) {
            if (c.count > bestCount) { bestCount = c.count; bestTrg = c.targetRva; bestCtx = c.ctxRva; }
        }
        outCtxRva = bestCtx;
        return bestTrg;
    }

    // Dispatcher-anchored toggler/ctx: scan a small region around the practice dispatcher function
    uintptr_t ScanPatchTogglerViaDispatcher(uintptr_t& outCtxRva) {
        outCtxRva = 0;
        uintptr_t dispRva = EFZ_RVA_PracticeDispatcher();
        if (!dispRva) return 0;
        HMODULE mod; uint8_t* base; size_t imageSize; if (!GetModule(mod, base, imageSize)) return 0;
        uint8_t* start = base + dispRva;
        const size_t window = 0x1200; // scan ~4.5KB
        uint8_t* end = start + window; if (end > base + imageSize) end = base + imageSize;
        uintptr_t bestTrg = 0; uintptr_t bestCtx = 0; int hits = 0;
        for (uint8_t* p = start; p + 10 < end; ++p) {
            if (p[0] == 0xB9) {
                uintptr_t ctxVa = *reinterpret_cast<uint32_t*>(p + 1);
                if (!(ctxVa >= reinterpret_cast<uintptr_t>(base) && ctxVa < reinterpret_cast<uintptr_t>(base) + imageSize)) continue;
                for (size_t i = 0; i < 64 && p + 5 + i + 5 < end; ++i) {
                    if (p[5 + i] == 0x6A) {
                        uint8_t imm8 = p[6 + i]; if (imm8 != 0 && imm8 != 1 && imm8 != 3) continue;
                        if (p[7 + i] == 0xE8) {
                            int32_t rel = *reinterpret_cast<int32_t*>(p + 8 + i);
                            uint8_t* target = (p + 12 + i) + rel;
                            if (target >= base && target < base + imageSize) {
                                bestTrg = reinterpret_cast<uintptr_t>(target) - reinterpret_cast<uintptr_t>(base);
                                bestCtx = ctxVa - reinterpret_cast<uintptr_t>(base);
                                ++hits; // keep last seen
                            }
                        }
                    }
                }
            }
        }
        outCtxRva = bestCtx;
        return bestTrg;
    }

    // Practice controller static pointer: look for a dword in .data that points into module and is referenced by code via mov reg, [imm32]
    uintptr_t ScanPracticeControllerPtrRva() {
        std::vector<Section> secs; if (!GetSections(secs)) return 0;
        HMODULE mod; uint8_t* base; size_t imageSize; if (!GetModule(mod, base, imageSize)) return 0;
        // Scan .text for mov reg, [imm32] shape: A1 imm32 (mov eax, [imm32]) or 8B 0D imm32 (mov ecx, [imm32]) etc.
        for (auto& s : secs) {
            uint8_t* p = s.base; size_t n = s.size;
            while (n >= 6) {
                // A1 imm32
                if (p[0] == 0xA1) {
                    uintptr_t va = *reinterpret_cast<uint32_t*>(p + 1);
                    if (va >= reinterpret_cast<uintptr_t>(base) && va < reinterpret_cast<uintptr_t>(base) + imageSize) {
                        // Treat this as a candidate; convert to RVA and return first hit
                        return va - reinterpret_cast<uintptr_t>(base);
                    }
                }
                // 8B 0D imm32 (mov ecx, [imm32]) or 8B 15 imm32 (mov edx, [imm32]) etc. — accept 8B 0D/15/1D/05
                if (p[0] == 0x8B && (p[1] == 0x0D || p[1] == 0x15 || p[1] == 0x1D || p[1] == 0x05)) {
                    uintptr_t va = *reinterpret_cast<uint32_t*>(p + 2);
                    if (va >= reinterpret_cast<uintptr_t>(base) && va < reinterpret_cast<uintptr_t>(base) + imageSize) {
                        return va - reinterpret_cast<uintptr_t>(base);
                    }
                }
                ++p; --n;
            }
        }
        return 0;
    }

    // Refresh mapping block routines: find a memcpy-like copy from Practice to ctx used during character switches.
    // Heuristic: look for patterns that copy a fixed-size block via rep movsd or a tight loop near calls from switcher code.
    // For simplicity, search for functions that reference the Practice controller pointer (imm32 we found) and have rep movs (F3 A5 or A4/A5 sequences).
    uintptr_t ScanRefreshMappingRvAs(uintptr_t practicePtrRva, bool toCtxVariant, uintptr_t& outAlt) {
        outAlt = 0;
        if (!practicePtrRva) return 0;
        std::vector<Section> secs; if (!GetSections(secs)) return 0;
        HMODULE mod; uint8_t* base; size_t imageSize; if (!GetModule(mod, base, imageSize)) return 0;
        uint8_t* practicePtrVa = base + practicePtrRva;
        // We'll search .text for XREFs to practicePtrVa and then inside the containing function for rep movs
        for (auto& s : secs) {
            uint8_t* p = s.base; size_t n = s.size;
            while (n >= 6) {
                // Match mov reg, [imm32] with that imm32 == practicePtrVa
                bool xref = false;
                if (p[0] == 0xA1) {
                    xref = (*reinterpret_cast<uint32_t*>(p + 1) == reinterpret_cast<uint32_t>(practicePtrVa));
                } else if (p[0] == 0x8B && (p[1] == 0x0D || p[1] == 0x15 || p[1] == 0x1D || p[1] == 0x05)) {
                    xref = (*reinterpret_cast<uint32_t*>(p + 2) == reinterpret_cast<uint32_t>(practicePtrVa));
                }
                if (xref) {
                    // Scan a window around for rep movs opcode F3 A5 (dword) or F3 A4 (byte)
                    const size_t window = 256;
                    uint8_t* begin = (p > s.base + window) ? (p - window) : s.base;
                    uint8_t* end = (p + window < s.base + s.size) ? (p + window) : (s.base + s.size);
                    for (uint8_t* q = begin; q + 2 <= end; ++q) {
                        if (q[0] == 0xF3 && (q[1] == 0xA5 || q[1] == 0xA4)) {
                            // Found a memcpy-like loop near a practice xref — take this function start by backtracking to a prologue push ebp (55 8B EC)
                            uint8_t* f = q;
                            while (f > s.base + 3) {
                                if (f[-3] == 0x55 && f[-2] == 0x8B && f[-1] == 0xEC) { f -= 3; break; }
                                --f;
                            }
                            uintptr_t funcRva = reinterpret_cast<uintptr_t>(f) - reinterpret_cast<uintptr_t>(base);
                            // Return first match as primary; store another nearby as alt
                            if (toCtxVariant && outAlt == 0) {
                                outAlt = funcRva;
                            }
                            return funcRva;
                        }
                    }
                }
                ++p; --n;
            }
        }
        return 0;
    }

    // Locate TogglePause: function that toggles practice pause flag and resets step counter, often called from hotkey dispatcher.
    // Heuristic: find a function that writes zero to an offset (B0/B4/176 depending on version) after reading practice ECX.
    uintptr_t ScanTogglePauseRva(uintptr_t practicePtrRva) {
        (void)practicePtrRva;
        std::vector<Section> secs; if (!GetSections(secs)) return 0;
        HMODULE mod; uint8_t* base; size_t imageSize; if (!GetModule(mod, base, imageSize)) return 0;
        // Build anchor hit maps for practice pause-related keys
        auto mapPractice = BuildAnchorFunctionHitMap(L"Practice");
        auto mapPause    = BuildAnchorFunctionHitMap(L"Pause");
        uintptr_t bestFuncRva = 0; int bestScore = -1;
        for (auto& s : secs) {
            uint8_t* p = s.base; size_t n = s.size;
            while (n >= 6) {
                // Look for pattern: C7 81 ?? ?? 00 00 00 00 00 00 (mov dword ptr [ecx+disp], 0)
                if (p[0] == 0xC7 && (p[1] & 0xC7) == 0x81 && p[6] == 0 && p[7] == 0 && p[8] == 0 && p[9] == 0) {
                    // Backtrack to function start
                    uintptr_t xrva = static_cast<uintptr_t>(s.rva + static_cast<uint32_t>(p - s.base));
                    uintptr_t funcRva = FindFunctionStartRva(xrva);
                    if (funcRva) {
                        int score = 1;
                        auto itP = mapPractice.find(funcRva); if (itP != mapPractice.end()) score += itP->second * 2;
                        auto itZ = mapPause.find(funcRva);    if (itZ != mapPause.end())    score += itZ->second * 3;
                        if (score > bestScore) { bestScore = score; bestFuncRva = funcRva; }
                    }
                }
                ++p; --n;
            }
        }
        return bestFuncRva;
    }

    // PracticeTick: function called every frame in practice; heuristic: same module calls toggle pause and touches step counter frequently.
    // Best effort: return a nearby function that writes to [ecx+stepCounterOff] with an increment pattern (FF 81 disp) or add 1 (83 81 disp 01)
    uintptr_t ScanPracticeTickRva() {
        std::vector<Section> secs; if (!GetSections(secs)) return 0;
        HMODULE mod; uint8_t* base; size_t imageSize; if (!GetModule(mod, base, imageSize)) return 0;
        auto mapPractice   = BuildAnchorFunctionHitMap(L"Practice");
        auto mapStepFrame  = BuildAnchorFunctionHitMap(L"StepFrame");
        uintptr_t bestFuncRva = 0; int bestScore = -1;
        for (auto& s : secs) {
            uint8_t* p = s.base; size_t n = s.size;
            while (n >= 7) {
                // inc dword ptr [ecx+disp] => FF 81 disp32
                if (p[0] == 0xFF && (p[1] & 0xC7) == 0x81) {
                    uintptr_t xrva = static_cast<uintptr_t>(s.rva + static_cast<uint32_t>(p - s.base));
                    uintptr_t funcRva = FindFunctionStartRva(xrva);
                    if (funcRva) {
                        int score = 1;
                        auto itP = mapPractice.find(funcRva);  if (itP != mapPractice.end())  score += itP->second * 2;
                        auto itS = mapStepFrame.find(funcRva); if (itS != mapStepFrame.end()) score += itS->second * 3;
                        if (score > bestScore) { bestScore = score; bestFuncRva = funcRva; }
                    }
                }
                ++p; --n;
            }
        }
        return bestFuncRva;
    }

    // GameModePtrArray: look for a large array of pointers in .data referenced via base+const (we approximate with the known number and usage pattern is complex; fallback from known RVA is fine).
    // Here we leave as 0 to avoid false positives; versioned RVA remains primary.
    bool IsInModuleRange(uintptr_t va, uint8_t* base, size_t size) {
        return va >= reinterpret_cast<uintptr_t>(base) && va < reinterpret_cast<uintptr_t>(base) + size;
    }
    uintptr_t ScanGameModePtrArrayRva() {
        std::vector<Section> secs; if (!GetSections(secs)) return 0;
        HMODULE hRev = nullptr; uint8_t* revBase = nullptr; size_t revSize = 0; if (!GetModule(hRev, revBase, revSize)) return 0;
        HMODULE hExe = GetModuleHandleA(nullptr); uint8_t* exeBase = nullptr; size_t exeSize = 0; (void)GetModuleByHandle(hExe, exeBase, exeSize);

        auto validateArray = [&](uint8_t* arrayVa)->bool {
            // Look at first 16 entries; count pointers that are null or inside efz.exe/EfzRevival.dll
            int good = 0, total = 16;
            for (int i = 0; i < total; ++i) {
                uintptr_t ptr = 0;
                if (!SafeReadMemory(reinterpret_cast<uintptr_t>(arrayVa + i * 4), &ptr, sizeof(ptr))) {
                    ptr = 0;
                }
                if (ptr == 0) { ++good; continue; }
                bool inExe = exeBase && exeSize && IsInModuleRange(ptr, exeBase, exeSize);
                bool inRev = IsInModuleRange(ptr, revBase, revSize);
                if (inExe || inRev) ++good;
            }
            return good >= 8; // at least half look sane
        };

        auto isSibIndex4Disp32 = [](uint8_t sib)->bool {
            // scale must be 2 (4x) => bits7-6 == 10b; base must be 101b (disp32)
            return ( (sib & 0xC0) == 0x80 ) && ( (sib & 0x07) == 0x05 );
        };

        for (auto& s : secs) {
            uint8_t* p = s.base; size_t n = s.size;
            while (n >= 6) {
                if (p[0] == 0x8B && p[1] == 0x04 && isSibIndex4Disp32(p[2])) {
                    uintptr_t va = *reinterpret_cast<uint32_t*>(p + 3);
                    // VA should be inside efzrevival image (typical) or readable memory
                    if (!IsInModuleRange(va, revBase, revSize)) { ++p; --n; continue; }
                    uint8_t* arr = reinterpret_cast<uint8_t*>(va);
                    if (validateArray(arr)) {
                        return va - reinterpret_cast<uintptr_t>(revBase);
                    }
                }
                ++p; --n;
            }
        }
        return 0;
    }

    // MapReset heuristic: look for address computation of (ecx*8 + ecx + 0x340) via either LEA or IMUL/ADD sequences
    // Recognized patterns inside the function body:
    //   A) LEA r?, [ECX + ECX*8 + 0x340] => 8D ?? C9 40 03 00 00 (masking ModRM.reg)
    //   B) IMUL r?, r?, 8   (6B /r 08) plus ADD r?, 0x340 (81 C? 40 03 00 00) plus ADD r?, ECX (03 C?) in a small window
    // We'll attribute the containing function as MapReset candidate.
    uintptr_t ScanMapResetRva() {
        std::vector<Section> secs; if (!GetSections(secs)) return 0;
        HMODULE mod; uint8_t* base; size_t imageSize; if (!GetModule(mod, base, imageSize)) return 0;
        auto isPrologueAt = [](uint8_t* f){ return f[0] == 0x55 && f[1] == 0x8B && f[2] == 0xEC; };
        auto looksLikeLeaEcxEcxTimes8Disp340 = [](uint8_t* p, size_t n)->bool {
            // Need at least 7 bytes: 8D, ModRM, SIB, disp32
            if (n < 7) return false;
            if (p[0] != 0x8D) return false; // LEA
            uint8_t modrm = p[1];
            // require r/m == 100b (SIB follows)
            if ( (modrm & 0x07) != 0x04 ) return false;
            // allow mod 10b (disp32) or 00b with base==101b (SIB base disp32); we'll just accept either, but we expect disp32 present
            uint8_t sib = p[2];
            // scale must be 3 (8x)
            if ( (sib >> 6) != 0x3 ) return false; // scale==3 => 8x
            // index must be ECX (001b)
            if ( ((sib >> 3) & 0x7) != 0x1 ) return false;
            // base must be ECX (001b)
            if ( (sib & 0x7) != 0x1 ) return false;
            // disp32 should be 0x340
            uint32_t disp = *reinterpret_cast<uint32_t*>(p + 3);
            return disp == 0x00000340;
        };

        uintptr_t best = 0;
        for (auto &s : secs) {
            if (!(s.characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
            uint8_t* p = s.base; size_t n = s.size;
            while (n >= 12) {
                bool found = false;
                // A) direct LEA form
                if (looksLikeLeaEcxEcxTimes8Disp340(p, n)) {
                    found = true;
                }
                // B) IMUL/ADD/ADD form within a small local window
                if (!found) {
                    bool foundImul8 = false, foundAddDisp = false, foundAddEcx = false;
                    // scan a sliding window of ~128 bytes for the 3 fragments
                    size_t window = (n > 128 ? 128 : n);
                    for (size_t i = 0; i + 6 < window; ++i) {
                        uint8_t* q = p + i;
                        // imul r32, r/m32, imm8 (6B /r ib)
                        if (q[0] == 0x6B && (q[2] == 0x08)) foundImul8 = true;
                        // add r32, imm32 (81 C? imm32) with 0x340
                        if (q[0] == 0x81 && (q[1] & 0xC0) == 0xC0) {
                            uint32_t imm = *reinterpret_cast<uint32_t*>(q + 2);
                            if (imm == 0x00000340) foundAddDisp = true;
                        }
                        // add r32, ecx (03 C?)
                        if (q[0] == 0x03 && (q[1] & 0xC7) == 0xC1) foundAddEcx = true; // 0xC1 = add eax, ecx etc.
                    }
                    found = foundImul8 && foundAddDisp && foundAddEcx;
                }
                if (found) {
                    // backtrack to function start (first prologue within previous 128 bytes)
                    uint8_t* f = p;
                    for (size_t back = 0; back < 128 && f > s.base + 3; ++back, --f) {
                        if (f[-3] == 0x55 && f[-2] == 0x8B && f[-1] == 0xEC) { f -= 3; break; }
                    }
                    if (isPrologueAt(f)) {
                        uintptr_t rva = reinterpret_cast<uintptr_t>(f) - reinterpret_cast<uintptr_t>(base);
                        best = rva; // take first/last seen; heuristic
                        break;
                    }
                }
                ++p; --n;
            }
            if (best) break;
        }
        return best;
    }

    // Helper: find nearest function start around an RVA by searching back/forward for prologue within a window
    uintptr_t FindNearestFunctionStartAround(uintptr_t rva, size_t scanWindow = 0x300) {
        if (!rva) return 0;
        std::vector<Section> secs; if (!GetSections(secs)) return 0;
        HMODULE mod; uint8_t* base; size_t imageSize; if (!GetModule(mod, base, imageSize)) return 0;
        for (auto &s : secs) {
            if (rva < s.rva || rva >= s.rva + s.size) continue;
            uint8_t* p = s.base + (rva - s.rva);
            // backtrack
            size_t back = std::min(scanWindow, (size_t)(p - s.base));
            for (size_t i = 0; i < back; ++i) {
                if (p[-(int)i - 3] == 0x55 && p[-(int)i - 2] == 0x8B && p[-(int)i - 1] == 0xEC) {
                    return s.rva + (uint32_t)(p - s.base - i - 3);
                }
            }
            // forward
            size_t fwd = std::min(scanWindow, (size_t)(s.base + s.size - p));
            for (size_t i = 0; i + 3 < fwd; ++i) {
                if (p[i] == 0x55 && p[i+1] == 0x8B && p[i+2] == 0xEC) {
                    return s.rva + (uint32_t)(p - s.base + i);
                }
            }
            return 0;
        }
        return 0;
    }

    // Check if a function body looks like MapReset: presence of LEA [ecx+ecx*8+0x340] or (imul*8 + add 0x340 + add ecx) within first ~192 bytes
    bool FunctionHasMapResetSignature(uintptr_t funcRva) {
        if (!funcRva) return false;
        std::vector<Section> secs; if (!GetSections(secs)) return false;
        HMODULE mod; uint8_t* base; size_t imageSize; if (!GetModule(mod, base, imageSize)) return false;
        for (auto &s : secs) {
            if (funcRva < s.rva || funcRva >= s.rva + s.size) continue;
            uint8_t* f = s.base + (funcRva - s.rva);
            size_t n = std::min<size_t>(192, s.size - (funcRva - s.rva));
            bool imul8=false, addDisp=false, addEcx=false, leaForm=false;
            for (size_t i = 0; i + 6 < n; ++i) {
                uint8_t* q = f + i;
                // LEA variant
                if (!leaForm) {
                    if (q[0] == 0x8D) {
                        if ((i + 7) <= n) {
                            uint8_t modrm = q[1];
                            if ( (modrm & 0x07) == 0x04 ) { // SIB
                                uint8_t sib = q[2];
                                if ( (sib >> 6) == 0x3 && ((sib >> 3) & 0x7) == 0x1 && (sib & 0x7) == 0x1 ) {
                                    uint32_t disp = *reinterpret_cast<uint32_t*>(q + 3);
                                    if (disp == 0x00000340) leaForm = true;
                                }
                            }
                        }
                    }
                }
                // IMUL/ADD/ADD variant
                if (q[0] == 0x6B && q[2] == 0x08) imul8 = true;
                if (q[0] == 0x81 && (q[1] & 0xC0) == 0xC0 && *reinterpret_cast<uint32_t*>(q+2) == 0x00000340) addDisp = true;
                if (q[0] == 0x03 && (q[1] & 0xC7) == 0xC1) addEcx = true;
            }
            return leaForm || (imul8 && addDisp && addEcx);
        }
        return false;
    }

    // Compute an RVA drift estimate using pairs of version-constant RVAs vs scanned RVAs we already resolved
    // Optional pivot limits the samples to those whose version RVA is near the pivot (within localityWindow)
    // Filters outliers where |scan - ver| > maxOutlier
    // Returns true if at least one sample remains; driftOut is median(scan - ver)
    bool ComputeRvaDriftEstimate(int& driftOut, uintptr_t pivotVer = 0, size_t localityWindow = 0x0, int maxOutlier = 0x20000) {
        struct Pair { uintptr_t ver; uintptr_t scan; };
        std::vector<Pair> pairs = {
            { EFZ_RVA_PatchToggler(),        g_results.patchTogglerRva },
            { EFZ_RVA_PatchCtx(),            g_results.patchCtxRva },
            { EFZ_RVA_TogglePause(),         g_results.togglePauseRva },
            { EFZ_RVA_PracticeTick(),        g_results.practiceTickRva },
            { EFZ_RVA_RefreshMappingBlock(), g_results.refreshMappingBlockRva }
        };
        std::vector<int> diffs;
        for (auto &p : pairs) {
            if (!p.ver || !p.scan) continue;
            if (pivotVer && localityWindow) {
                if (p.ver < pivotVer - localityWindow || p.ver > pivotVer + localityWindow) continue;
            }
            int diff = (int)p.scan - (int)p.ver;
            if (std::abs(diff) > maxOutlier) continue; // filter outliers
            diffs.push_back(diff);
        }
        if (diffs.empty()) return false;
        std::sort(diffs.begin(), diffs.end());
        driftOut = diffs[diffs.size()/2];
        return true;
    }

    // Try to detect CleanupPair using drift from version constants with a light sanity check
    uintptr_t ScanCleanupPairRva() {
        // Try localized drift first (near PatchToggler/MapReset region)
        uintptr_t ver = EFZ_RVA_CleanupPair();
        int drift = 0;
        if (ver && ComputeRvaDriftEstimate(drift, ver, 0x10000)) {
            uintptr_t guess = (uintptr_t)((int)ver + drift);
            uintptr_t frva = FindNearestFunctionStartAround(guess, 0x600);
            if (frva) return frva;
        }
        // Fallback: global drift
        if (ver && ComputeRvaDriftEstimate(drift)) {
            uintptr_t guess = (uintptr_t)((int)ver + drift);
            uintptr_t frva = FindNearestFunctionStartAround(guess, 0x600);
            if (frva) return frva;
        }
        // Last resort: trust version constant
        return ver;
    }
    // Find all ASCII strings (min length threshold) in module and return pair (va,len)
    struct AsciiSpan { uint8_t* va; size_t len; };
    std::vector<AsciiSpan> FindAsciiStrings(size_t minLen = 4) {
        std::vector<AsciiSpan> out;
        std::vector<Section> secs; if (!GetSections(secs)) return out;
        for (auto &s : secs) {
            if (!(s.characteristics & (IMAGE_SCN_MEM_READ))) continue;
            uint8_t* p = s.base; size_t n = s.size;
            size_t run = 0; uint8_t* runStart = nullptr;
            for (size_t i = 0; i < n; ++i) {
                uint8_t ch = p[i];
                bool ok = (ch >= 0x20 && ch <= 0x7E); // printable ASCII
                if (ok) {
                    if (run == 0) runStart = p + i;
                    ++run;
                } else {
                    if (run >= minLen) {
                        out.push_back({ runStart, run });
                    }
                    run = 0; runStart = nullptr;
                }
            }
            if (run >= minLen) out.push_back({ runStart, run });
        }
        return out;
    }

    // Find all UTF-16LE strings (min wchar length) in module and return pair (va,len)
    struct WideSpan { uint8_t* va; size_t wlen; };
    std::vector<WideSpan> FindWideStrings(size_t minWlen = 4) {
        std::vector<WideSpan> out;
        std::vector<Section> secs; if (!GetSections(secs)) return out;
        for (auto &s : secs) {
            if (!(s.characteristics & (IMAGE_SCN_MEM_READ))) continue;
            uint8_t* p = s.base; size_t n = s.size;
            size_t run = 0; uint8_t* runStart = nullptr;
            for (size_t i = 0; i + 1 < n; i += 2) {
                uint16_t wc = *reinterpret_cast<uint16_t*>(p + i);
                bool ok = (wc >= 0x20 && wc <= 0x7E); // ASCII subset in UTF-16LE
                if (ok) {
                    if (run == 0) runStart = p + i;
                    ++run;
                } else {
                    if (run >= minWlen) {
                        out.push_back({ runStart, run });
                    }
                    run = 0; runStart = nullptr;
                }
            }
            if (run >= minWlen) out.push_back({ runStart, run });
        }
        return out;
    }

    // Convert module VA string to UTF-8 text (ascii or wide)
    static std::string ToUtf8FromAscii(uint8_t* va, size_t len) {
        return std::string(reinterpret_cast<const char*>(va), reinterpret_cast<const char*>(va) + len);
    }
    static std::string ToUtf8FromWide(uint8_t* va, size_t wlen) {
        const wchar_t* w = reinterpret_cast<const wchar_t*>(va);
        int need = WideCharToMultiByte(CP_UTF8, 0, w, (int)wlen, nullptr, 0, nullptr, nullptr);
        std::string out; out.resize((size_t)need);
        WideCharToMultiByte(CP_UTF8, 0, w, (int)wlen, out.data(), need, nullptr, nullptr);
        return out;
    }

    // Collect string anchors found in a function body window: scan for push/mov of imm32 that points into any known string VA
    void CollectFunctionStringAnchors(uintptr_t funcRva, EfzSigScanner::FunctionAnchors& outFA) {
        outFA.sourceFuncRva = funcRva;
        if (!funcRva) return;
        HMODULE mod; uint8_t* base; size_t imageSize; if (!GetModule(mod, base, imageSize)) return;

        // Pre-compute all strings to a set for quick lookup
        auto ascii = FindAsciiStrings();
        auto wide  = FindWideStrings();
        auto isStringVa = [&](uint32_t imm, bool &isWide, size_t &spanLen)->uint8_t* {
            uint8_t* ptr = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(imm));
            // Check ASCII spans
            for (auto &s : ascii) {
                if (ptr >= s.va && ptr < s.va + s.len) { isWide = false; spanLen = s.len - (size_t)(ptr - s.va); return ptr; }
            }
            // Check WIDE spans
            for (auto &w : wide) {
                if (ptr >= w.va && ptr < w.va + w.wlen * 2) { isWide = true; spanLen = w.wlen - ((size_t)(ptr - w.va)/2); return ptr; }
            }
            return nullptr;
        };

        uint8_t* f = base + funcRva;
        // Scan up to 0x800 bytes in the function for literal loads; stop at first RET if seen to bound
        const size_t maxScan = 0x1400;
        size_t scanned = 0;
        while (scanned + 6 <= maxScan) {
            uint8_t* p = f + scanned;
            bool matched = false;
            // push imm32 (68 imm32)
            if (p[0] == 0x68) {
                uint32_t imm = *reinterpret_cast<uint32_t*>(p + 1);
                bool w; size_t rem; uint8_t* at = isStringVa(imm, w, rem);
                if (at) {
                    EfzSigScanner::AnchorRef ar{};
                    ar.insnOffset = (uint32_t)scanned;
                    ar.strRva = reinterpret_cast<uintptr_t>(at) - reinterpret_cast<uintptr_t>(base);
                    ar.wide = w;
                    ar.textUtf8 = w ? ToUtf8FromWide(at, std::min<size_t>(rem, 64)) : ToUtf8FromAscii(at, std::min<size_t>(rem, 64));
                    outFA.refs.push_back(std::move(ar));
                    matched = true;
                }
            }
            // mov reg, imm32 (B8..BF imm32) — sometimes used to load string addresses
            if (!matched && (p[0] >= 0xB8 && p[0] <= 0xBF)) {
                uint32_t imm = *reinterpret_cast<uint32_t*>(p + 1);
                bool w; size_t rem; uint8_t* at = isStringVa(imm, w, rem);
                if (at) {
                    EfzSigScanner::AnchorRef ar{};
                    ar.insnOffset = (uint32_t)scanned;
                    ar.strRva = reinterpret_cast<uintptr_t>(at) - reinterpret_cast<uintptr_t>(base);
                    ar.wide = w;
                    ar.textUtf8 = w ? ToUtf8FromWide(at, std::min<size_t>(rem, 64)) : ToUtf8FromAscii(at, std::min<size_t>(rem, 64));
                    outFA.refs.push_back(std::move(ar));
                    matched = true;
                }
            }
            // lea reg, [imm32] will not appear; absolute addresses are encoded as imm32 in push/mov
            // don't stop at first RET; functions can have multiple returns
            ++scanned;
        }
    }

    // Find code xrefs to an absolute data VA using common instruction patterns with imm32
    // Includes absolute forms and SIB+disp32 addressing (e.g., [disp32 + reg*scale])
    // Returns RVAs of instruction locations
    std::vector<uintptr_t> FindDataImmXrefs(uintptr_t targetVa) {
        std::vector<uintptr_t> refs;
        std::vector<Section> secs; if (!GetSections(secs)) return refs;
        for (auto &s : secs) {
            if (!(s.characteristics & IMAGE_SCN_MEM_EXECUTE)) continue; // code only
            uint8_t* p = s.base; size_t n = s.size;
            while (n >= 6) {
                // Helper lambdas to test for SIB+disp32 memory operands
                auto isModRmSibDisp32 = [](uint8_t modrm){ return ( (modrm & 0xC0) == 0x00 ) && ( (modrm & 0x07) == 0x04 ); };
                auto sibBaseIsDisp32 = [](uint8_t sib){ return ( (sib & 0x07) == 0x05 ); };
                auto readDisp32At = [](uint8_t* addr)->uintptr_t { return *reinterpret_cast<uint32_t*>(addr); };
                // mov eax, [imm32]
                if (p[0] == 0xA1) {
                    uintptr_t va = *reinterpret_cast<uint32_t*>(p + 1);
                    if (va == targetVa) refs.push_back(static_cast<uintptr_t>(s.rva + static_cast<uint32_t>(p - s.base)));
                }
                // mov r32, [imm32] (8B 0D/15/1D/05)
                if (p[0] == 0x8B && (p[1] == 0x0D || p[1] == 0x15 || p[1] == 0x1D || p[1] == 0x05)) {
                    uintptr_t va = *reinterpret_cast<uint32_t*>(p + 2);
                    if (va == targetVa) refs.push_back(static_cast<uintptr_t>(s.rva + static_cast<uint32_t>(p - s.base)));
                }
                // mov r32, imm32 (B8..BF) — loading an absolute address into a register
                if (p[0] >= 0xB8 && p[0] <= 0xBF) {
                    uintptr_t va = *reinterpret_cast<uint32_t*>(p + 1);
                    if (va == targetVa) refs.push_back(static_cast<uintptr_t>(s.rva + static_cast<uint32_t>(p - s.base)));
                }
                // cmp dword ptr [imm32], imm8  (83 3D imm32 imm8)
                if (p[0] == 0x83 && p[1] == 0x3D) {
                    uintptr_t va = *reinterpret_cast<uint32_t*>(p + 2);
                    if (va == targetVa) refs.push_back(static_cast<uintptr_t>(s.rva + static_cast<uint32_t>(p - s.base)));
                }
                // cmp dword ptr [imm32], imm32 (81 3D imm32 imm32)
                if (p[0] == 0x81 && p[1] == 0x3D) {
                    uintptr_t va = *reinterpret_cast<uint32_t*>(p + 2);
                    if (va == targetVa) refs.push_back(static_cast<uintptr_t>(s.rva + static_cast<uint32_t>(p - s.base)));
                }
                // cmp byte ptr [imm32], imm8 (80 3D imm32 imm8)
                if (p[0] == 0x80 && p[1] == 0x3D) {
                    uintptr_t va = *reinterpret_cast<uint32_t*>(p + 2);
                    if (va == targetVa) refs.push_back(static_cast<uintptr_t>(s.rva + static_cast<uint32_t>(p - s.base)));
                }
                // SIB + disp32 memory addressing forms where base=disp32 (sib.base==101b)
                // Example patterns (not exhaustive):
                //   8B 04 xx <disp32>          mov r32,  [disp32 + index*scale]
                //   89 04 xx <disp32>          mov [disp32 + index*scale], r32
                //   C7 04 xx <disp32> <imm32>  mov dword ptr [disp32 + index*scale], imm32
                //   83 3C xx <disp32> <imm8>   cmp dword ptr [disp32 + index*scale], imm8
                //   81 3C xx <disp32> <imm32>  cmp dword ptr [disp32 + index*scale], imm32
                if (p[0] == 0x8B && isModRmSibDisp32(p[1]) && sibBaseIsDisp32(p[2]) && n >= 7) {
                    uintptr_t va = readDisp32At(p + 3);
                    if (va == targetVa) refs.push_back(static_cast<uintptr_t>(s.rva + static_cast<uint32_t>(p - s.base)));
                }
                if (p[0] == 0x89 && isModRmSibDisp32(p[1]) && sibBaseIsDisp32(p[2]) && n >= 7) {
                    uintptr_t va = readDisp32At(p + 3);
                    if (va == targetVa) refs.push_back(static_cast<uintptr_t>(s.rva + static_cast<uint32_t>(p - s.base)));
                }
                if (p[0] == 0xC7 && isModRmSibDisp32(p[1]) && sibBaseIsDisp32(p[2]) && n >= 11) {
                    uintptr_t va = readDisp32At(p + 3);
                    if (va == targetVa) refs.push_back(static_cast<uintptr_t>(s.rva + static_cast<uint32_t>(p - s.base)));
                }
                if (p[0] == 0x83 && isModRmSibDisp32(p[1]) && sibBaseIsDisp32(p[2]) && n >= 8) {
                    uintptr_t va = readDisp32At(p + 3);
                    if (va == targetVa) refs.push_back(static_cast<uintptr_t>(s.rva + static_cast<uint32_t>(p - s.base)));
                }
                if (p[0] == 0x81 && isModRmSibDisp32(p[1]) && sibBaseIsDisp32(p[2]) && n >= 11) {
                    uintptr_t va = readDisp32At(p + 3);
                    if (va == targetVa) refs.push_back(static_cast<uintptr_t>(s.rva + static_cast<uint32_t>(p - s.base)));
                }
                // push imm32 (address used as pointer)
                if (p[0] == 0x68) {
                    uintptr_t va = *reinterpret_cast<uint32_t*>(p + 1);
                    if (va == targetVa) refs.push_back(static_cast<uintptr_t>(s.rva + static_cast<uint32_t>(p - s.base)));
                }
                ++p; --n;
            }
        }
        return refs;
    }

    // Choose a representative referencing function for a data RVA using immediate-address xrefs
    uintptr_t ChooseRefFuncForDataRva(uintptr_t dataRva) {
        if (!dataRva) return 0;
        HMODULE mod; uint8_t* base; size_t imageSize; if (!GetModule(mod, base, imageSize)) return 0;
        auto refs = FindDataImmXrefs(reinterpret_cast<uintptr_t>(base) + dataRva);
        std::unordered_map<uintptr_t,int> counts;
        for (auto r : refs) { auto f = FindFunctionStartRva(r); if (f) counts[f]++; }
        uintptr_t best = 0; int bestCnt = 0;
        for (auto &kv : counts) { if (kv.second > bestCnt) { bestCnt = kv.second; best = kv.first; } }
        if (best) return best;
        // Drift-based fallback using version constants
        int drift = 0; uintptr_t ver = EFZ_RVA_MapReset();
        if (ver && ComputeRvaDriftEstimate(drift)) {
            uintptr_t guess = (uintptr_t)((int)ver + drift);
            uintptr_t frva = FindNearestFunctionStartAround(guess, 0x600);
            if (frva && FunctionHasMapResetSignature(frva)) return frva;
            if (frva) return frva;
        }
        return 0;
    }

    // Specialized chooser for onlineStatus: prefer functions that have online-related strings in-body
    uintptr_t ChooseRefFuncForOnlineStatus(uintptr_t dataRva) {
        if (!dataRva) return 0;
        HMODULE mod; uint8_t* base; size_t imageSize; if (!GetModule(mod, base, imageSize)) return 0;
        auto xrvas = FindDataImmXrefs(reinterpret_cast<uintptr_t>(base) + dataRva);
        if (xrvas.empty()) return 0;
        // Map candidate function -> xref count
        std::unordered_map<uintptr_t,int> candCounts;
        for (auto r : xrvas) { auto f = FindFunctionStartRva(r); if (f) candCounts[f]++; }
        // Evaluate each candidate by scanning its in-body string anchors for online-related keywords
        auto scoreKeywords = [&](uintptr_t funcRva)->int {
            EfzSigScanner::FunctionAnchors tmp{}; tmp.sourceFuncRva = funcRva;
            CollectFunctionStringAnchors(funcRva, tmp);
            if (tmp.refs.empty()) return 0;
            // ASCII HUD/log keywords and INI value names
            static const char* kws[] = {
                // HUD/log and runtime labels
                "Ping", "Delay", "Rollback", "Spectator", "Match", "Host", "Client", "NAT", "Port", "Connect", "Disconnect", "Frames",
                "LoadMatch", "InputP1", "InputP2", "PaletteP1", "PaletteP2", "Start init", "Switch inputs", "FPU control word", "Input delay", "Max Roll", "VS ", "Init", "Sync", "Quit",
                // INI-like tokens that sometimes appear as ASCII too
                "Name", "Protocol"
            };
            // Wide-config keys we often see converted to UTF-8 in decomp (will still match in our ToUtf8)
            static const char* wideLike[] = {
                "Network", "MaxRollback", "HolePunchingServer", "AllowSpectating", "AllowPracticeKeys", "BattleLogFile", "SaveBattleLog",
                "IncreaseInputDelay", "DecreaseInputDelay", "IncreaseSpecSpeed", "DecreaseSpecSpeed", "ToggleRemotePalettes", "DisplayScore", "LogScore",
                "SaveAllReplays", "ReplayFolder", "SavePreviousReplay", "ReplayFormat", "Player"
            };
            int sc = 0;
            for (auto &r : tmp.refs) {
                std::string t = r.textUtf8;
                // case-insensitive matching for robustness
                std::string u = t; for (auto &c : u) c = (char)tolower((unsigned char)c);
                for (auto kw : kws) {
                    std::string k = kw; for (auto &c : k) c = (char)tolower((unsigned char)c);
                    if (u.find(k) != std::string::npos) sc += 3; // strong keyword
                }
                for (auto kw : wideLike) {
                    std::string k = kw; for (auto &c : k) c = (char)tolower((unsigned char)c);
                    if (u.find(k) != std::string::npos) sc += 2; // config-flavored hints
                }
                // mild hints
                if (u.find("input") != std::string::npos && u.find("delay") != std::string::npos) sc += 2;
                if (u.find("spectat") != std::string::npos) sc += 2;
            }
            return sc;
        };

        uintptr_t bestFunc = 0; int bestScore = -1; int bestCount = -1;
        for (auto &kv : candCounts) {
            int kwScore = scoreKeywords(kv.first);
            if (kwScore > bestScore || (kwScore == bestScore && kv.second > bestCount)) {
                bestScore = kwScore; bestCount = kv.second; bestFunc = kv.first;
            }
        }
        // Fallbacks: if keyword scores tie at 0, prefer generic count heuristic
        if (bestScore <= 0) return ChooseRefFuncForDataRva(dataRva);
        return bestFunc;
    }
}

namespace EfzSigScanner {
    bool IsEfzRevivalLoaded() {
        return GetModuleHandleA("EfzRevival.dll") != nullptr;
    }

    bool EnsureScanned() {
        if (g_scanned.load()) return true;
        if (!IsEfzRevivalLoaded()) return false;

    uintptr_t ctx = 0;
        g_results.onlineStatusRva = ScanOnlineStatusRva();
        g_results.patchTogglerRva = ScanPatchTogglerRva(ctx);
        if (!g_results.patchTogglerRva || !ctx) {
            uintptr_t ctx2 = 0; uintptr_t alt = ScanPatchTogglerViaDispatcher(ctx2);
            if (alt) { g_results.patchTogglerRva = alt; if (!ctx) ctx = ctx2; }
        }
        g_results.patchCtxRva = ctx;

    // Practice controller static pointer
    g_results.practiceControllerPtrRva = ScanPracticeControllerPtrRva();
    g_results.togglePauseRva = ScanTogglePauseRva(g_results.practiceControllerPtrRva);
    g_results.practiceTickRva = ScanPracticeTickRva();
    g_results.gameModePtrArrayRva = ScanGameModePtrArrayRva();
    // MapReset with robust fallbacks: try signature, then drift near version, then version constant as last resort
    g_results.mapResetRva = ScanMapResetRva();
    if (!g_results.mapResetRva) {
        int drift = 0; uintptr_t verMapReset = EFZ_RVA_MapReset();
        if (verMapReset && ComputeRvaDriftEstimate(drift, verMapReset, 0x10000)) {
            uintptr_t guess = (uintptr_t)((int)verMapReset + drift);
            uintptr_t frva = FindNearestFunctionStartAround(guess, 0x600);
            if (frva && FunctionHasMapResetSignature(frva)) {
                g_results.mapResetRva = frva;
            }
        }
        if (!g_results.mapResetRva && verMapReset) {
            // fall back to version constant if nothing better was found
            g_results.mapResetRva = verMapReset;
        }
    }
    g_results.cleanupPairRva = ScanCleanupPairRva();

    // Refresh mapping routines (best-effort heuristics)
    uintptr_t alt = 0;
    g_results.refreshMappingBlockRva = ScanRefreshMappingRvAs(g_results.practiceControllerPtrRva, true, alt);
    g_results.refreshMappingBlockPracToCtxRva = alt; // keep the alt variant if found

        g_scanned.store(true);

        char buf[256];
        _snprintf_s(buf, sizeof(buf), _TRUNCATE,
            "[EFZ-SIG] scan results: toggler=0x%08lX ctx=0x%08lX online=0x%08lX pracPtr=0x%08lX refMap=0x%08lX refMapAlt=0x%08lX togglePause=0x%08lX pracTick=0x%08lX gmArray=0x%08lX mapReset=0x%08lX cleanupPair=0x%08lX",
            (unsigned long)g_results.patchTogglerRva,
            (unsigned long)g_results.patchCtxRva,
            (unsigned long)g_results.onlineStatusRva,
            (unsigned long)g_results.practiceControllerPtrRva,
            (unsigned long)g_results.refreshMappingBlockRva,
            (unsigned long)g_results.refreshMappingBlockPracToCtxRva,
            (unsigned long)g_results.togglePauseRva,
            (unsigned long)g_results.practiceTickRva,
            (unsigned long)g_results.gameModePtrArrayRva,
            (unsigned long)g_results.mapResetRva,
            (unsigned long)g_results.cleanupPairRva);
        LogOut(buf, true);

    // Optional deep-dive diagnostics, enabled with EFZ_SIG_DEBUG=1
        if (SigDebug()) {
            HMODULE mod; uint8_t* base; size_t imageSize; 
            if (GetModule(mod, base, imageSize)) {
                auto log_u32 = [](const char* prefix, unsigned long v){ char b[128]; _snprintf_s(b, sizeof(b), _TRUNCATE, "%s%08lX", prefix, v); LogOut(b, true); };

                // Patch toggler function
                if (g_results.patchTogglerRva) {
                    uint8_t* f = base + g_results.patchTogglerRva;
                    LogBytes("[EFZ-SIGDBG] toggler bytes", f, 12);
                    int callers = CountCallsToTarget(base, imageSize, f);
                    int rets = CountRetWithin(f, 64);
                    char m[160]; _snprintf_s(m, sizeof(m), _TRUNCATE, "[EFZ-SIGDBG] toggler callers=%d ret-in-64=%d rva=0x%08lX", callers, rets, (unsigned long)g_results.patchTogglerRva);
                    LogOut(m, true);
                }

                // Patch context VA (global object) — immediate loaded into ECX before calling toggler
                if (g_results.patchCtxRva) {
                    char m[160]; _snprintf_s(m, sizeof(m), _TRUNCATE, "[EFZ-SIGDBG] ctx va=0x%08lX (in-module=%d)",
                        (unsigned long)(reinterpret_cast<uintptr_t>(base) + g_results.patchCtxRva),
                        (int)1);
                    LogOut(m, true);
                }

                // Practice controller static pointer and its current value
                if (g_results.practiceControllerPtrRva) {
                    uintptr_t ptrVa = reinterpret_cast<uintptr_t>(base) + g_results.practiceControllerPtrRva;
                    uint32_t cur = 0; (void)SafeReadMemory(ptrVa, &cur, sizeof(cur));
                    char m[200]; _snprintf_s(m, sizeof(m), _TRUNCATE, "[EFZ-SIGDBG] pracPtr varVA=0x%08lX -> value=0x%08lX (in-module=%d)",
                        (unsigned long)ptrVa, (unsigned long)cur, (int)(cur >= (unsigned long)reinterpret_cast<uintptr_t>(base) && cur < (unsigned long)(reinterpret_cast<uintptr_t>(base)+imageSize)));
                    LogOut(m, true);
                }

                // RefreshMapping routines: dump presence of rep movs nearby
                if (g_results.refreshMappingBlockRva) {
                    uint8_t* f = base + g_results.refreshMappingBlockRva;
                    bool hasRep = HasRepMovsNear(f, 256);
                    char m[160]; _snprintf_s(m, sizeof(m), _TRUNCATE, "[EFZ-SIGDBG] refreshMapping rva=0x%08lX rep-movs-near=%d", (unsigned long)g_results.refreshMappingBlockRva, (int)hasRep);
                    LogOut(m, true);
                }
                if (g_results.refreshMappingBlockPracToCtxRva) {
                    uint8_t* f = base + g_results.refreshMappingBlockPracToCtxRva;
                    bool hasRep = HasRepMovsNear(f, 256);
                    char m[180]; _snprintf_s(m, sizeof(m), _TRUNCATE, "[EFZ-SIGDBG] refreshMappingAlt rva=0x%08lX rep-movs-near=%d", (unsigned long)g_results.refreshMappingBlockPracToCtxRva, (int)hasRep);
                    LogOut(m, true);
                }

                // TogglePause details
                if (g_results.togglePauseRva) {
                    uint8_t* f = base + g_results.togglePauseRva;
                    LogBytes("[EFZ-SIGDBG] togglePause bytes", f, 12);
                    int callers = CountCallsToTarget(base, imageSize, f);
                    int rets = CountRetWithin(f, 64);
                    char m[160]; _snprintf_s(m, sizeof(m), _TRUNCATE, "[EFZ-SIGDBG] togglePause callers=%d ret-in-64=%d rva=0x%08lX", callers, rets, (unsigned long)g_results.togglePauseRva);
                    LogOut(m, true);
                }

                // PracticeTick details
                if (g_results.practiceTickRva) {
                    uint8_t* f = base + g_results.practiceTickRva;
                    LogBytes("[EFZ-SIGDBG] practiceTick bytes", f, 12);
                    int callers = CountCallsToTarget(base, imageSize, f);
                    int rets = CountRetWithin(f, 64);
                    char m[160]; _snprintf_s(m, sizeof(m), _TRUNCATE, "[EFZ-SIGDBG] practiceTick callers=%d ret-in-64=%d rva=0x%08lX", callers, rets, (unsigned long)g_results.practiceTickRva);
                    LogOut(m, true);
                }

                // Online status current value
                if (g_results.onlineStatusRva) {
                    uintptr_t va = reinterpret_cast<uintptr_t>(base) + g_results.onlineStatusRva;
                    uint32_t v = 0xFFFFFFFF; (void)SafeReadMemory(va, &v, sizeof(v));
                    char m[160]; _snprintf_s(m, sizeof(m), _TRUNCATE, "[EFZ-SIGDBG] onlineStatus varVA=0x%08lX value=%lu", (unsigned long)va, (unsigned long)v);
                    LogOut(m, true);
                }
                // (no exe online status; all online scanning restricted to EfzRevival.dll)

                // Game mode pointer array – dump first 8 entries
                if (g_results.gameModePtrArrayRva) {
                    uintptr_t baseVa = reinterpret_cast<uintptr_t>(base) + g_results.gameModePtrArrayRva;
                    char m[256]; m[0] = 0; size_t off = 0; off += _snprintf_s(m+off, sizeof(m)-off, _TRUNCATE, "[EFZ-SIGDBG] gmArray first8: ");
                    for (int i = 0; i < 8; ++i) {
                        uint32_t v = 0; (void)SafeReadMemory(baseVa + i*4, &v, sizeof(v));
                        off += _snprintf_s(m+off, sizeof(m)-off, _TRUNCATE, "%s0x%08lX", (i?",":""), (unsigned long)v);
                    }
                    LogOut(m, true);
                }

                // String anchor diagnostics: find common config/hotkey strings and log xref counts
                struct AnchorGroup { const wchar_t* name; const wchar_t* const* items; size_t count; };
                // Define multiple domains with multiple strings each
                const wchar_t* practiceAnchors[] = { L"Practice", L"Pause", L"StepFrame", L"SaveState", L"LoadState", L"ToggleReplay", L"ToggleRecord", L"SwitchStartCondition", L"ToggleReplayRandom", L"DIK_PAUSE" };
                const wchar_t* networkAnchors[]  = {
                    L"Network", L"Name", L"Player", L"AllowSpectating", L"AllowPracticeKeys", L"HolePunchingServer",
                    L"IncreaseInputDelay", L"DecreaseInputDelay", L"IncreaseSpecSpeed", L"DecreaseSpecSpeed",
                    L"Port", L"Protocol", L"MaxRollback", L"BattleLogFile", L"SaveBattleLog",
                    L"ReplayFolder", L"SavePreviousReplay", L"ReplayFormat", L"SaveAllReplays"
                };
                const wchar_t* tourneyAnchors[]  = { L"Tournament", L"TournamentFolder" };
                const wchar_t* miscAnchors[]     = { L"h!!!", L"i!!!" };
                AnchorGroup groups[] = {
                    { L"PracticeDomain", practiceAnchors, sizeof(practiceAnchors)/sizeof(practiceAnchors[0]) },
                    { L"NetworkDomain",  networkAnchors,  sizeof(networkAnchors)/sizeof(networkAnchors[0]) },
                    { L"TournamentDomain", tourneyAnchors, sizeof(tourneyAnchors)/sizeof(tourneyAnchors[0]) },
                    { L"VersionMarkers", miscAnchors, sizeof(miscAnchors)/sizeof(miscAnchors[0]) }
                };

                for (auto &g : groups) {
                    for (size_t idx = 0; idx < g.count; ++idx) {
                        const wchar_t* w = g.items[idx];
                        auto vas = FindWideStringVAs(w);
                        for (auto va : vas) {
                            auto refs = FindPushImmXrefs(va);
                            char m[220]; _snprintf_s(m, sizeof(m), _TRUNCATE, "[EFZ-SIGDBG] anchor[%ls] '%ls' va=0x%08lX xrefs=%zu", g.name, w, (unsigned long)va, refs.size());
                            LogOut(m, true);
                            // Summarize top function hits for this anchor
                            std::unordered_map<uintptr_t,int> funcHits;
                            for (auto xrva : refs) { auto frva = FindFunctionStartRva(xrva); if (frva) funcHits[frva]++; }
                            // Log up to 3 functions with highest counts
                            // Build vector for sorting
                            std::vector<std::pair<uintptr_t,int>> sorted(funcHits.begin(), funcHits.end());
                            std::sort(sorted.begin(), sorted.end(), [](auto&a, auto&b){ return a.second > b.second; });
                            for (size_t k = 0; k < sorted.size() && k < 3; ++k) {
                                char mf[180]; _snprintf_s(mf, sizeof(mf), _TRUNCATE, "[EFZ-SIGDBG]   funcHit rva=0x%08lX hits=%d", (unsigned long)sorted[k].first, sorted[k].second);
                                LogOut(mf, true);
                            }
                        }
                    }
                }
            }
        }
        // Anchor collection logs: enabled by default when our debug flags are on
        // Gated behind code flags (detailedLogging or detailedDebugOutput) or EFZ_SIG_DEBUG
        auto AnchorsDebug = [](){
            // Prefer code-driven flags; still allow EFZ_SIG_DEBUG env as a quick override
            if (detailedLogging.load() || detailedDebugOutput.load()) return true;
            return SigDebug();
        };
        if (AnchorsDebug()) {
            AnchorCollection ac{};
            if (CollectAnchors(ac)) {
                LogAnchors(ac);
            } else {
                LogOut("[EFZ-ANCHORS] no anchors collected", true);
            }
        }
        return (g_results.onlineStatusRva | g_results.patchTogglerRva | g_results.patchCtxRva | g_results.practiceControllerPtrRva) != 0;
    }

    const Results& Get() { return g_results; }

    // Collect anchors around resolved RVAs to aid cross-version correlation
    bool CollectAnchors(AnchorCollection& out) {
        if (!g_scanned.load()) return false;
        bool any = false;
        auto collectIf = [&](uintptr_t rva, FunctionAnchors& dst){ if (rva) { CollectFunctionStringAnchors(rva, dst); any = any || !dst.refs.empty(); } };

        // Functions
        collectIf(g_results.patchTogglerRva, out.patchToggler);
        collectIf(g_results.togglePauseRva, out.togglePause);
        collectIf(g_results.practiceTickRva, out.practiceTick);
        collectIf(g_results.refreshMappingBlockRva, out.refreshMappingBlock);
        collectIf(g_results.refreshMappingBlockPracToCtxRva, out.refreshMappingBlockPracToCtx);
        collectIf(g_results.mapResetRva, out.mapReset);
        collectIf(g_results.cleanupPairRva, out.cleanupPair);

        // Data: pick a representative referencing function and collect there
        auto collectData = [&](uintptr_t dataRva, FunctionAnchors& dst){
            if (!dataRva) return;
            uintptr_t f = ChooseRefFuncForDataRva(dataRva);
            if (f) { CollectFunctionStringAnchors(f, dst); any = any || !dst.refs.empty(); }
        };
        collectData(g_results.patchCtxRva, out.patchCtxRefs);
        // For onlineStatus, prefer functions that reference strong online anchors (ping/delay/match/spectator/network)
        if (g_results.onlineStatusRva) {
            uintptr_t f = ChooseRefFuncForOnlineStatus(g_results.onlineStatusRva);
            if (f) { CollectFunctionStringAnchors(f, out.onlineStatusRefs); any = any || !out.onlineStatusRefs.refs.empty(); }
        }
        else {
            collectData(g_results.onlineStatusRva, out.onlineStatusRefs);
        }
        collectData(g_results.practiceControllerPtrRva, out.practiceControllerPtrRefs);
        collectData(g_results.gameModePtrArrayRva, out.gameModePtrArrayRefs);
        return any;
    }

    void LogAnchors(const AnchorCollection& ac) {
        auto logSet = [&](const char* name, const FunctionAnchors& fa){
            std::ostringstream oss;
            oss << "[EFZ-ANCHORS] " << name << " funcRva=0x" << std::hex << (unsigned long)fa.sourceFuncRva
                << " refs=" << std::dec << fa.refs.size();
            LogOut(oss.str(), true);
            for (size_t i = 0; i < fa.refs.size(); ++i) {
                const auto& r = fa.refs[i];
                std::string txt = r.textUtf8;
                if (txt.size() > 96) txt.resize(96);
                // Escape newlines for single-line logs
                for (auto &ch : txt) { if (ch == '\n' || ch == '\r') ch = ' '; }
                char buf[512];
                _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                    "  - off=0x%04lX strRva=0x%08lX wide=%d text=\"%s\"",
                    (unsigned long)r.insnOffset,
                    (unsigned long)r.strRva,
                    (int)r.wide,
                    txt.c_str());
                LogOut(buf, true);
            }
        };
        logSet("patchToggler",                 ac.patchToggler);
        logSet("togglePause",                  ac.togglePause);
        logSet("practiceTick",                 ac.practiceTick);
        logSet("refreshMappingBlock",          ac.refreshMappingBlock);
        logSet("refreshMappingBlockPracToCtx", ac.refreshMappingBlockPracToCtx);
        logSet("mapReset",                     ac.mapReset);
        logSet("cleanupPair",                  ac.cleanupPair);
        logSet("patchCtxRefs",                 ac.patchCtxRefs);
        logSet("onlineStatusRefs",             ac.onlineStatusRefs);
        logSet("practiceControllerPtrRefs",    ac.practiceControllerPtrRefs);
        logSet("gameModePtrArrayRefs",         ac.gameModePtrArrayRefs);
    }
}
