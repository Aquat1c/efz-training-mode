#pragma once

// Savestate Hook
// Hooks the EfzRevival Practice mode save/load state functions so the mod can
// track Revival-owned savestate activity and restore mod-side state around it.
// This is the mod's sole savestate path (the custom byte-snapshot backend was
// removed); save/load run through EfzRevival's native functions plus our mod-state
// capture/restore layer.

namespace SavestateHook {
    // Install the savestate hooks (save and load)
    // Returns true if hooks were successfully installed
    bool Install();

    // Uninstall the savestate hooks
    void Uninstall();

    // Trigger EfzRevival's native save/load on the captured Practice controller
    // (with the mod-state capture/restore layer). Used by the mod's own
    // save/load hotkeys. Returns false if the hook isn't installed or the
    // Practice controller could not be resolved.
    bool TriggerSave();
    bool TriggerLoad();

    // Check if hooks are currently installed
    bool IsInstalled();

    // Get the count of save operations detected since hook installation
    unsigned int GetSaveCount();

    // Get the count of load operations detected since hook installation
    unsigned int GetLoadCount();
}
