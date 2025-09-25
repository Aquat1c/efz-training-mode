#pragma once
#include <atomic>

namespace PauseMenu {
    // Ensure internal state and data snapshot are prepared. Safe to call repeatedly.
    void EnsureInitialized();
    // Legacy name kept for any existing callers; forwards to EnsureInitialized.
    inline void Initialize() { EnsureInitialized(); }
    void Toggle();
    bool IsVisible();
    void Close();
    void Render(); // Called during ImGui frame when visible
    void TickInput(); // Poll Q/E while open (edge detection)
}
