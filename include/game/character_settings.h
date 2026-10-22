#pragma once
#include "../include/utils/utilities.h"

#include <string>

namespace CharacterSettings {
    std::string GetCharacterName(int charID);

    // Internal/resource name used by auto-actions and game memory.
    std::string GetCharacterInternalName(int charID);
    
    int GetCharacterID(const std::string& name);
    
    bool IsCharacter(const std::string& name, int charID);
    
    void UpdateCharacterIDs(DisplayData& data);
    
    // forceRead: one-shot snapshot while the ordinary settings GUI is hidden
    // (mission/tutorial setup).
    void ReadCharacterValues(uintptr_t base, DisplayData& data, bool forceRead = false);
    
    // forceApply: one-shot apply while the settings GUI is hidden; normal
    // callers retain the GUI/infinite-setting gate.
    void ApplyCharacterValues(uintptr_t base, const DisplayData& data, bool forceApply = false);

    // Per-tick enforcement (no threads); call periodically (~16 Hz) from the
    // main monitor thread.
    void TickCharacterEnforcements(uintptr_t base, const DisplayData& data);

    // Re-applies Rumi's weapon mode via the engine's own toggle routine;
    // defers unless the character is actionable.
    void ApplyRumiModeNow(int playerIndex, bool barehanded);

    // Clears cached per-character pointers (recomputed on the next Read/Apply);
    // call when (re)entering a valid game mode to avoid stale addresses.
    void InvalidateAllCharacterPointerCaches();
}
