#pragma once
#include <stdint.h>

// Gates the Practice hotkey evaluation (Pause/Step/Record/etc.) while the
// ImGui menu is visible.
namespace PracticeHotkeyGate {
    // Idempotent; true on success or if already installed.
    bool Install();
    void Uninstall();
    uint64_t GetSuppressedFrameCount();
    void NotifyMenuVisibility(bool visible);
}
