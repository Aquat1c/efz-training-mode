#pragma once
#include <stdint.h>

// PauseIntegration: Mirror EfzRevival Practice pause behavior when our ImGui menu is shown.
// When the menu opens in Practice mode, we freeze the game via EfzRevival's patch toggler.
// When the menu closes, we unfreeze only if we were the ones who froze it (don’t fight user pause).

namespace PauseIntegration {
    // Notify of menu visibility change; applies/removes pause accordingly (Practice mode only)
    void OnMenuVisibilityChanged(bool visible);
}
