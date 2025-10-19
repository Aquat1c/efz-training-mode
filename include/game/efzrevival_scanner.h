#pragma once
#include <windows.h>
#include <cstdint>
#include <vector>
#include <string>

// Runtime signature scanning for EfzRevival.dll to resolve RVAs on unknown/new versions.
// This provides discovery for:
//  - Patch toggler function (ctx,thiscall; enable=0/3)
//  - Patch context global (passed as ECX at call sites)
//  - Online-status global (cmp [imm32], 2 guard)
// Additional anchors can be added over time.

namespace EfzSigScanner {
    struct Results {
        uintptr_t patchTogglerRva = 0;   // .text RVA
        uintptr_t patchCtxRva      = 0;   // .data/.rdata RVA
        uintptr_t onlineStatusRva  = 0;   // .data/.rdata RVA
        // Additional RVAs we attempt to discover heuristically
        uintptr_t practiceControllerPtrRva = 0; // static pointer to Practice controller
        uintptr_t refreshMappingBlockRva = 0;   // practice -> ctx copy
        uintptr_t refreshMappingBlockPracToCtxRva = 0; // ctx <- practice copy (alt variant)
        uintptr_t togglePauseRva = 0;           // official pause toggle (thiscall)
        uintptr_t practiceTickRva = 0;          // practice tick (used for ECX capture)
        uintptr_t gameModePtrArrayRva = 0;      // global pointer array base
        uintptr_t mapResetRva = 0;              // map reset routine
        uintptr_t cleanupPairRva = 0;           // cleanup pair routine
    };

    // Perform a one-time scan; cached internally. Returns true if at least one key value resolved.
    bool EnsureScanned();

    // Access last scan results (EnsureScanned() first).
    const Results& Get();

    // Utilities (exposed for testing/diagnostics)
    bool IsEfzRevivalLoaded();

    // --------------------------------------
    // String anchor collection (drift aid)
    // --------------------------------------
    // We collect human-stable anchors by scanning the code around resolved RVAs
    // for references to static strings (UTF-16LE or ASCII). These anchors are
    // useful to correlate functions across versions when RVAs drift.

    struct AnchorRef {
        // Offset of the referencing instruction from the function start (if known).
        // For data-backed anchors resolved via xref functions, this offset is
        // relative to the chosen referencing function.
        uint32_t insnOffset = 0;
        // RVA of the referenced string (relative to EfzRevival.dll base)
        uintptr_t strRva = 0;
        // True if the string is UTF-16LE; false if ASCII
        bool wide = false;
        // UTF-8 rendering (truncated to a reasonable length for logs)
        std::string textUtf8;
    };

    struct FunctionAnchors {
        // The function RVA whose body was scanned for string references.
        // For RVAs that refer to data (globals), this will be the RVA of the
        // most relevant referencing function chosen by the collector.
        uintptr_t sourceFuncRva = 0;
        std::vector<AnchorRef> refs;
    };

    struct AnchorCollection {
        // Function targets
        FunctionAnchors patchToggler;                    // from Results.patchTogglerRva
        FunctionAnchors togglePause;                     // from Results.togglePauseRva
        FunctionAnchors practiceTick;                    // from Results.practiceTickRva
        FunctionAnchors refreshMappingBlock;             // from Results.refreshMappingBlockRva
        FunctionAnchors refreshMappingBlockPracToCtx;    // from Results.refreshMappingBlockPracToCtxRva
        FunctionAnchors mapReset;                        // from Results.mapResetRva
        FunctionAnchors cleanupPair;                     // from Results.cleanupPairRva

        // Data targets (collected from a representative referencing function)
        FunctionAnchors patchCtxRefs;                    // from Results.patchCtxRva
        FunctionAnchors onlineStatusRefs;                // from Results.onlineStatusRva
        FunctionAnchors practiceControllerPtrRefs;       // from Results.practiceControllerPtrRva
        FunctionAnchors gameModePtrArrayRefs;            // from Results.gameModePtrArrayRva
    };

    // Collect anchors for all resolved RVAs using the last scan results.
    // Returns true if at least one anchor was collected.
    bool CollectAnchors(AnchorCollection& out);

    // Utility to log anchors in a compact, diff-friendly format.
    void LogAnchors(const AnchorCollection& ac);
}
