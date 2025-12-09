#pragma once

namespace Framestep {
    // Framestep mode: how many subframes to advance per step
    enum class StepMode {
        Subframe = 1,      // Advance 1 subframe (192fps logical frame)
        FullFrame = 3      // Advance 3 subframes (64fps visual frame)
    };

    // Initialize framestep system (call once at startup)
    void Initialize();

    // Update framestep state (call every frame)
    void Update();

    // Check if we're currently paused via framestep
    bool IsPaused();

    // Get the current step counter value
    unsigned int GetStepCounter();

    // Reset step counter to zero
    void ResetStepCounter();

    // Handle pause toggle (Space key)
    void TogglePause();

    // Handle frame step request (P key)
    void RequestFrameStep();

    // Check if framestep is enabled (vanilla EFZ only)
    bool IsEnabled();

    // Update overlay display status
    void UpdateOverlayStatus();

    // Get/set step mode
    StepMode GetStepMode();
    void SetStepMode(StepMode mode);

    // Get subframes per step (1 for subframe, 3 for full frame)
    int GetSubframesPerStep();
}
