#pragma once
#include "../include/utils/utilities.h"

#include <string>

namespace CharacterSettings {
    // Get character name from ID
    std::string GetCharacterName(int charID);

    // Get the internal/resource character name used by auto-actions and game memory.
    std::string GetCharacterInternalName(int charID);
    
    // Get character ID from name
    int GetCharacterID(const std::string& name);
    
    // Check if character name corresponds to a specific character
    bool IsCharacter(const std::string& name, int charID);
    
    // Update character IDs in DisplayData
    void UpdateCharacterIDs(DisplayData& data);
    
    // Read character-specific values from memory. Mission/tutorial setup may
    // force a one-shot snapshot while the ordinary settings GUI is hidden.
    void ReadCharacterValues(uintptr_t base, DisplayData& data, bool forceRead = false);
    
    // Apply character-specific values to memory. Mission/tutorial setup may
    // force a one-shot apply while the ordinary settings GUI is hidden;
    // normal callers retain the GUI/infinite-setting gate.
    void ApplyCharacterValues(uintptr_t base, const DisplayData& data, bool forceApply = false);

    // Inline per-tick enforcement of character-specific features (no threads)
    // Call this periodically (e.g., ~16 Hz) from the main monitor thread.
    void TickCharacterEnforcements(uintptr_t base, const DisplayData& data);

    // Re-apply Rumi's weapon mode NOW through the engine's own toggle routine
    // (safe manual fallback). Defers unless the character is in an actionable
    // state. Exposed for the tutorial's per-cycle sword-mode re-assert on
    // looping armor drills (her 41236C bunt is a one-shot that goes barehanded).
    void ApplyRumiModeNow(int playerIndex, bool barehanded);

    // Clear all cached per-character pointers so they will be
    // recomputed on the next Read/Apply call. Intended to be
    // called when (re)entering a valid game mode (e.g. after
    // returning from character select) to avoid stale addresses.
    void InvalidateAllCharacterPointerCaches();
}
