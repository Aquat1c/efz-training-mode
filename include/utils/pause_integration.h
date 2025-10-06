#pragma once
#include <stdint.h>

// PauseIntegration: Mirror EfzRevival Practice pause behavior when our ImGui menu is shown.
// When the menu opens in Practice mode, we freeze the game via EfzRevival's patch toggler.
// When the menu closes, we unfreeze only if we were the ones who froze it (don’t fight user pause).

namespace PauseIntegration {
    // Notify of menu visibility change; applies/removes pause accordingly (Practice mode only)
    void OnMenuVisibilityChanged(bool visible);
    // Ensure the Practice pointer capture hook is installed (no-op if already)
    void EnsurePracticePointerCapture();
    // Returns the current Practice controller pointer (or nullptr if not yet captured)
    void* GetPracticeControllerPtr();
    // While the menu is visible, keep the freeze enforced in gameplay (guards against external unfreeze)
    void MaintainFreezeWhileMenuVisible();

    // Queries for paused/frozen state. Best-effort and safe to call anytime.
    // - IsPracticePaused: true if we can read the Practice pause flag and it's set.
    // - IsGameSpeedFrozen: true if gamespeed byte resolves and equals 0 (frozen time).
    // - IsPausedOrFrozen: convenience OR of the above.
    bool IsPracticePaused();
    bool IsGameSpeedFrozen();
    bool IsPausedOrFrozen();

    // Frame-step support:
    // Exposes the Practice step counter (+0xB0) which increments each single-frame advance while paused.
    // Returns true if we could read the counter; outCounter unchanged on failure.
    bool ReadStepCounter(uint32_t &outCounter);
    // Returns true if (a) paused and (b) the internal step counter advanced since last call to this function.
    // Safe to call every tick; internally debounces using a static snapshot.
    bool ConsumeStepAdvance();
}
