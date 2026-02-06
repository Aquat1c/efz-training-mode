#pragma once

// Savestate Hook
// Hooks the EfzRevival Practice mode save/load state functions to track when
// savestates are saved and loaded. Provides console logging and status messages.
// Currently only supports EfzRevival 1.02e.

namespace SavestateHook {
    // Install the savestate hooks (save and load)
    // Returns true if hooks were successfully installed
    bool Install();

    // Uninstall the savestate hooks
    void Uninstall();

    // Check if hooks are currently installed
    bool IsInstalled();

    // Get the count of save operations detected since hook installation
    unsigned int GetSaveCount();

    // Get the count of load operations detected since hook installation
    unsigned int GetLoadCount();
}
