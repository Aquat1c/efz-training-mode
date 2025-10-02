#pragma once
#include <stdint.h>

// Gate the Practice hotkey evaluation (Pause/Step/Record/etc.) while ImGui menu is visible.
namespace PracticeHotkeyGate {
    // Install hook (idempotent). Returns true on success or if already installed.
    bool Install();
    // Optional: force uninstall (not strictly required; kept for symmetry)
    void Uninstall();
    // For debugging: number of suppressed frames so far.
    uint64_t GetSuppressedFrameCount();
    // Notify gate of current menu visibility (call from GUI toggle path)
    void NotifyMenuVisibility(bool visible);
}
