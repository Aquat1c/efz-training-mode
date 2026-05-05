#pragma once

// Savestate Hook
// Hooks the EfzRevival Practice mode save/load state functions so the mod can
// track Revival-owned savestate activity and restore mod-side state around it.
// This remains available even when custom savestates are selected so Revival
// loads/saves can still be observed and used as a fallback path.

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
