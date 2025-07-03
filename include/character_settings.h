#pragma once
#include "../include/utilities.h"
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
    
    // Apply character-specific patches (e.g. infinite blood)
    void ApplyCharacterPatches(const DisplayData& data);
    
    // Remove character-specific patches
    void RemoveCharacterPatches();
    
    // Check if any character patches are applied
    bool AreCharacterPatchesApplied();

    // Character value monitoring thread
    void CharacterValueMonitoringThread();

    // Debugging functions
    bool IsMonitoringThreadActive();
    void GetFeatherCounts(int& p1Count, int& p2Count);
}