#pragma once
#include <stdint.h>

// PauseIntegration: Mirror EfzRevival Practice pause behavior when our ImGui menu is shown.
// When the menu opens in Practice mode, we freeze the game via EfzRevival's patch toggler.
// When the menu closes, we unfreeze only if we were the ones who froze it (don’t fight user pause).

namespace PauseIntegration {
    using StepAdvanceCallback = void (*)(uint32_t beforeCounter, uint32_t afterCounter);

    // More than one in-game surface can legitimately share the same physical
    // Practice pause.  In particular, the recorder pause menu remains open
    // underneath the ordinary Practice settings menu so Back returns to the
    // recording controls instead of resuming capture.  Track those owners
    // independently: only the first visible surface acquires the pause and
    // only the last one releases it.
    enum class MenuSurface : uint8_t {
        ImGui = 1u << 0,
        MissionPause = 1u << 1,
        TutorialPage = 1u << 2,
        RecorderHandoff = 1u << 3,
        RecorderCommandHandoff = 1u << 4,
        // Short-lived exact-state demonstration transaction.  It freezes
        // both fighters and world entities while a mission baseline is being
        // restored and tick zero is published to the native input lane.
        DemoTransition = 1u << 5,
    };

    constexpr uint8_t MenuSurfaceBit(MenuSurface surface) {
        return static_cast<uint8_t>(surface);
    }

    constexpr uint8_t UpdatedMenuSurfaceMask(uint8_t current,
                                             MenuSurface surface,
                                             bool visible) {
        const uint8_t bit = MenuSurfaceBit(surface);
        return visible ? static_cast<uint8_t>(current | bit)
                       : static_cast<uint8_t>(current & ~bit);
    }

    void OnMenuSurfaceVisibilityChanged(MenuSurface surface, bool visible);
    // Keep a surface logically visible while briefly releasing only the
    // physical Practice pause. This is for bounded render-state refreshes;
    // ordinary callers should continue to use visibility ownership above.
    // A refresh is exclusive (no nested surface may be visible), tokenized,
    // and cancelled automatically by any surface ownership change.
    bool BeginMenuSurfaceRefresh(MenuSurface surface, uint64_t& tokenOut);
    bool EndMenuSurfaceRefresh(MenuSurface surface, uint64_t token);
    // Re-acquire the aggregate physical pause when an owned surface remains
    // visible but a game-state transition recreated/cleared Practice pause.
    void ReassertMenuSurfacePause(MenuSurface surface);
    // Emergency/session teardown path. Normal UI code should release only its
    // own surface with OnMenuSurfaceVisibilityChanged().
    void ForceCloseAllMenuSurfaces();

    // Notify of menu visibility change; applies/removes pause accordingly (Practice mode only)
    // Legacy ImGui entry point; equivalent to the ImGui surface above.
    void OnMenuVisibilityChanged(bool visible);
    // Ensure the Practice pointer capture hook is installed (no-op if already)
    void EnsurePracticePointerCapture();
    // Returns the current Practice controller pointer (or nullptr if not yet captured)
    void* GetPracticeControllerPtr();
    // Record a Practice controller pointer observed by a trusted Revival hook.
    void NotePracticeControllerCandidate(void* practicePtr, const char* source);
    // Force an immediate Practice controller resolution attempt.
    // `allowCharacterSelect` is only for menu/reset paths that legitimately run in CS.
    // `allowLooseValidation` allows a match-only fallback that accepts a live Practice object
    // even if the stricter pause/menu invariants are not initialized yet.
    void* ResolvePracticeControllerPtrNow(bool allowCharacterSelect = false,
                                          bool allowLooseValidation = false,
                                          const char* reason = nullptr);
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
    // Exposes the Practice step counter (+0xB0) which increments each native Revival step while paused.
    // Returns true if we could read the counter; outCounter unchanged on failure.
    bool ReadStepCounter(uint32_t &outCounter);
    // Mirrors EfzRevival's official pause toggle state without calling the hotkey path.
    // Resets the Practice step counter like the official toggle.
    bool SetPracticePausedForFramestep(bool paused);
    // Queues one EfzRevival native step by setting Practice pause (+0xB4)
    // and step-request (+0xAC) flags. Current framestep code uses this only
    // for the 1.02f subframe build; other Revival builds use engine stepping.
    bool RequestPracticeSubframeStep();
    // Returns true if (a) paused and (b) the internal step counter advanced since last call to this function.
    // Safe to call every tick; internally debounces using a static snapshot.
    bool ConsumeStepAdvance();

    // Called from the hooked Revival Practice tick after it consumes a
    // step-request flag and increments the Practice step counter.
    void SetPracticeStepAdvanceCallback(StepAdvanceCallback callback);

    // Clears cached Practice/battle/gamespeed pointers that are only valid for the
    // current gameplay session. Hooks remain installed.
    void ResetCachedPointers(const char* reason);

    // Suspend or resume the lightweight pause/battle-context capture hooks used by the mod.
    void SetRuntimeHooksActive(bool active);
}
