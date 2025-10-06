#pragma once
#include "../include/utils/utilities.h"

#include <string>

namespace CharacterSettings {
    // Get character name from ID
    std::string GetCharacterName(int charID);
    
    // Get character ID from name
    int GetCharacterID(const std::string& name);
    
    // Check if character name corresponds to a specific character
    bool IsCharacter(const std::string& name, int charID);
    
    // Update character IDs in DisplayData
    void UpdateCharacterIDs(DisplayData& data);
    
    // Read character-specific values from memory
    void ReadCharacterValues(uintptr_t base, DisplayData& data);
    
    // Apply character-specific values to memory
    void ApplyCharacterValues(uintptr_t base, const DisplayData& data);

    // Inline per-tick enforcement of character-specific features (no threads)
    // Call this periodically (e.g., ~16 Hz) from the main monitor thread.
    void TickCharacterEnforcements(uintptr_t base, const DisplayData& data);
}