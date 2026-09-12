#pragma once
#include <cstdint>

namespace SwitchPlayers {
    // Returns true if swap applied successfully
    bool ToggleLocalSide();
    // Force set local side to 0 (P1) or 1 (P2)
    bool SetLocalSide(int sideIdx);
    // Force-apply the full Practice-controller remap for the given side.
    // This is intended for deferred savestate recovery when stale Practice fields
    // may already claim the target side but routing/GUI were not rebuilt yet.
    bool ReapplyLocalSide(int sideIdx);
    // Restore engine-facing side and control flags without touching Practice controller buffers.
    bool RestoreEngineControlState(int sideIdx, uint8_t p1CpuFlag, uint8_t p2CpuFlag, bool armInputCleanup = true);
    // Get the current local side (0=P1, 1=P2), returns -1 if unable to read
    int GetLocalSide();
    // Resolve the current local and remote players as 1-based gameplay indices.
    int GetLocalPlayerIndex();
    int GetRemotePlayerIndex();
    // Reset menu/control mapping to defaults for Character Select and menus:
    // - For EfzRevival: set Practice local=0 (P1), remote=1, align GUI_POS
    // - For vanilla: disable swapped routing (P1 controls -> P1)
    // Also restores engine active/CPU and AI flags to P1 human / P2 CPU.
    bool ResetControlMappingForMenusToP1();
    // Clear the swap tracking flag (called at match start and CS entry)
    void ClearSwapFlag();
    // Mark that sides were swapped during match (used by callers that perform manual SetLocalSide).
    void MarkSwapped();
}
