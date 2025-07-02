#include "../include/character_settings.h"
#include "../include/constants.h"
#include "../include/memory.h"
#include "../include/logger.h"
#include "../include/game_state.h"
#include <algorithm>
#include <cctype>
#include <unordered_map>

namespace CharacterSettings {
    // Track if character patches are currently applied
    static bool ikumiBloodPatchApplied = false;
    
    // Updated character name mapping with correct display names
    static const std::unordered_map<std::string, int> characterNameMap = {
        {"akane", CHAR_ID_AKANE},
        {"akiko", CHAR_ID_AKIKO},
        {"ikumi", CHAR_ID_IKUMI},
        {"misaki", CHAR_ID_MISAKI},
        {"sayuri", CHAR_ID_SAYURI},
        {"kanna", CHAR_ID_KANNA},
        {"kaori", CHAR_ID_KAORI},
        {"makoto", CHAR_ID_MAKOTO},
        {"minagi", CHAR_ID_MINAGI},
        {"mio", CHAR_ID_MIO},
        {"mishio", CHAR_ID_MISHIO},
        {"misuzu", CHAR_ID_MISUZU},
        {"nagamori", CHAR_ID_MIZUKA},    // Nagamori is actually Mizuka
        {"nanase", CHAR_ID_NANASE},      // Nanase is Rumi
        {"exnanase", CHAR_ID_EXNANASE},  // ExNanase is Doppel
        {"nayuki", CHAR_ID_NAYUKIB},     // Nayuki is actually NayukiB (Nayuki)
        {"nayukib", CHAR_ID_NAYUKI},     // NayukiB is actually Nayuki (Neyuki)
        {"shiori", CHAR_ID_SHIORI},
        {"ayu", CHAR_ID_AYU},
        {"mai", CHAR_ID_MAI},
        {"mayu", CHAR_ID_MAYU},
        {"mizukab", CHAR_ID_MIZUKAB},    // MizukaB is Unknown
        {"kano", CHAR_ID_KANO}
    };

    std::string GetCharacterName(int charID) {
        switch (charID) {
            case CHAR_ID_AKANE:    return "Akane";
            case CHAR_ID_AKIKO:    return "Akiko";
            case CHAR_ID_IKUMI:    return "Ikumi";
            case CHAR_ID_MISAKI:   return "Misaki";
            case CHAR_ID_SAYURI:   return "Sayuri";
            case CHAR_ID_KANNA:    return "Kanna";
            case CHAR_ID_KAORI:    return "Kaori";
            case CHAR_ID_MAKOTO:   return "Makoto";
            case CHAR_ID_MINAGI:   return "Minagi";
            case CHAR_ID_MIO:      return "Mio";
            case CHAR_ID_MISHIO:   return "Mishio";
            case CHAR_ID_MISUZU:   return "Misuzu";
            case CHAR_ID_MIZUKA:   return "Mizuka";      // Nagamori in files
            case CHAR_ID_NAGAMORI: return "Nagamori";
            case CHAR_ID_NANASE:   return "Rumi";        // Nanase in files
            case CHAR_ID_EXNANASE: return "Doppel";      // ExNanase in files
            case CHAR_ID_NAYUKI:   return "Neyuki";      // NayukiB in files
            case CHAR_ID_NAYUKIB:  return "Nayuki";      // Nayuki in files
            case CHAR_ID_SHIORI:   return "Shiori";
            case CHAR_ID_AYU:      return "Ayu";
            case CHAR_ID_MAI:      return "Mai";
            case CHAR_ID_MAYU:     return "Mayu";
            case CHAR_ID_MIZUKAB:  return "Unknown";     // MizukaB in files
            case CHAR_ID_KANO:     return "Kano";
            default:               return "Undefined";    // Changed from "Unknown"
        }
    }
    
    int GetCharacterID(const std::string& name) {
        // Convert name to lowercase for case-insensitive comparison
        std::string lowerName = name;
        std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), 
                      [](unsigned char c){ return std::tolower(c); });
        
        // Try exact match first
        auto it = characterNameMap.find(lowerName);
        if (it != characterNameMap.end()) {
            return it->second;
        }
        
        // Try partial match if exact match fails
        for (const auto& entry : characterNameMap) {
            if (lowerName.find(entry.first) != std::string::npos) {
                return entry.second;
            }
        }
        
        return -1; // Unknown character
    }
    
    bool IsCharacter(const std::string& name, int charID) {
        return GetCharacterID(name) == charID;
    }
    
    void UpdateCharacterIDs(DisplayData& data) {
        data.p1CharID = GetCharacterID(data.p1CharName);
        data.p2CharID = GetCharacterID(data.p2CharName);
        
        LogOut("[CHAR] Updated character IDs - P1: " + std::string(data.p1CharName) + 
               " (ID: " + std::to_string(data.p1CharID) + "), P2: " + 
               std::string(data.p2CharName) + " (ID: " + std::to_string(data.p2CharID) + ")",
               detailedLogging.load());
    }
    
    void ReadCharacterValues(uintptr_t base, DisplayData& data) {
        // Read Ikumi's values if either player is using her
        if (data.p1CharID == CHAR_ID_IKUMI) {
            uintptr_t bloodAddr = ResolvePointer(base, EFZ_BASE_OFFSET_P1, IKUMI_BLOOD_OFFSET);
            uintptr_t genocideAddr = ResolvePointer(base, EFZ_BASE_OFFSET_P1, IKUMI_GENOCIDE_OFFSET);
            
            if (bloodAddr) SafeReadMemory(bloodAddr, &data.p1IkumiBlood, sizeof(int));
            if (genocideAddr) SafeReadMemory(genocideAddr, &data.p1IkumiGenocide, sizeof(int));
            
            LogOut("[CHAR] Read P1 Ikumi values: Blood=" + std::to_string(data.p1IkumiBlood) + 
                   ", Genocide=" + std::to_string(data.p1IkumiGenocide), 
                   detailedLogging.load());
        }
        
        if (data.p2CharID == CHAR_ID_IKUMI) {
            uintptr_t bloodAddr = ResolvePointer(base, EFZ_BASE_OFFSET_P2, IKUMI_BLOOD_OFFSET);
            uintptr_t genocideAddr = ResolvePointer(base, EFZ_BASE_OFFSET_P2, IKUMI_GENOCIDE_OFFSET);
            
            if (bloodAddr) SafeReadMemory(bloodAddr, &data.p2IkumiBlood, sizeof(int));
            if (genocideAddr) SafeReadMemory(genocideAddr, &data.p2IkumiGenocide, sizeof(int));
            
            LogOut("[CHAR] Read P2 Ikumi values: Blood=" + std::to_string(data.p2IkumiBlood) + 
                   ", Genocide=" + std::to_string(data.p2IkumiGenocide), 
                   detailedLogging.load());
        }
    }
    
    void ApplyCharacterValues(uintptr_t base, const DisplayData& data) {
        // Apply Ikumi's values if either player is using her
        if (data.p1CharID == CHAR_ID_IKUMI) {
            uintptr_t bloodAddr = ResolvePointer(base, EFZ_BASE_OFFSET_P1, IKUMI_BLOOD_OFFSET);
            uintptr_t genocideAddr = ResolvePointer(base, EFZ_BASE_OFFSET_P1, IKUMI_GENOCIDE_OFFSET);
            
            // Fix: Add explicit template parameters to std::max and std::min
            int bloodValue = std::max<int>(0, std::min<int>(IKUMI_BLOOD_MAX, data.p1IkumiBlood));
            int genocideValue = std::max<int>(0, std::min<int>(IKUMI_GENOCIDE_MAX, data.p1IkumiGenocide));
            
            if (bloodAddr) SafeWriteMemory(bloodAddr, &bloodValue, sizeof(int));
            if (genocideAddr) SafeWriteMemory(genocideAddr, &genocideValue, sizeof(int));
            
            LogOut("[CHAR] Applied P1 Ikumi values: Blood=" + std::to_string(bloodValue) + 
                   ", Genocide=" + std::to_string(genocideValue), 
                   detailedLogging.load());
        }
        
        // Fix for lines 158-159
        if (data.p2CharID == CHAR_ID_IKUMI) {
            uintptr_t bloodAddr = ResolvePointer(base, EFZ_BASE_OFFSET_P2, IKUMI_BLOOD_OFFSET);
            uintptr_t genocideAddr = ResolvePointer(base, EFZ_BASE_OFFSET_P2, IKUMI_GENOCIDE_OFFSET);
            
            // Fix: Add explicit template parameters to std::max and std::min
            int bloodValue = std::max<int>(0, std::min<int>(IKUMI_BLOOD_MAX, data.p2IkumiBlood));
            int genocideValue = std::max<int>(0, std::min<int>(IKUMI_GENOCIDE_MAX, data.p2IkumiGenocide));
            
            if (bloodAddr) SafeWriteMemory(bloodAddr, &bloodValue, sizeof(int));
            if (genocideAddr) SafeWriteMemory(genocideAddr, &genocideValue, sizeof(int));
            
            LogOut("[CHAR] Applied P2 Ikumi values: Blood=" + std::to_string(bloodValue) + 
                   ", Genocide=" + std::to_string(genocideValue), 
                   detailedLogging.load());
        }
        
        // Apply any character-specific patches if enabled
        if (data.infiniteBloodMode) {
            ApplyCharacterPatches(data);
        } else {
            RemoveCharacterPatches();
        }
    }
    
    void ApplyCharacterPatches(const DisplayData& data) {
        // Only apply patches if:
        // 1. Infinite blood mode is enabled
        // 2. At least one player is using Ikumi
        // 3. We're in a valid game mode (practice mode)
        
        if (!data.infiniteBloodMode || 
            (data.p1CharID != CHAR_ID_IKUMI && data.p2CharID != CHAR_ID_IKUMI)) {
            return;
        }
        
        // Verify we're in a valid game state before applying patches
        GameMode currentMode = GetCurrentGameMode();
        if (currentMode != GameMode::Practice) {
            LogOut("[CHAR] Not applying infinite blood patch - not in practice mode", true);
            return;
        }
        
        // Apply NOP patch to prevent genocide timer from decreasing
        uintptr_t base = GetEFZBase();
        if (!base) return;
        
        uintptr_t patchAddr = base + IKUMI_GENOCIDE_TIMER_ADDR;
        
        if (!ikumiBloodPatchApplied) {
            if (PatchMemory(patchAddr, IKUMI_GENOCIDE_TIMER_PATCH, 6)) {
                ikumiBloodPatchApplied = true;
                LogOut("[CHAR] Applied infinite blood patch", true);
            } else {
                LogOut("[CHAR] Failed to apply infinite blood patch", true);
            }
        }
    }
    
    void RemoveCharacterPatches() {
        // Remove the Ikumi blood patch if applied
        if (ikumiBloodPatchApplied) {
            uintptr_t base = GetEFZBase();
            if (!base) return;
            
            uintptr_t patchAddr = base + IKUMI_GENOCIDE_TIMER_ADDR;
            
            if (PatchMemory(patchAddr, IKUMI_GENOCIDE_TIMER_ORIGINAL, 6)) {
                ikumiBloodPatchApplied = false;
                LogOut("[CHAR] Removed infinite blood patch", true);
            } else {
                LogOut("[CHAR] Failed to remove infinite blood patch", true);
            }
        }
    }
    
    bool AreCharacterPatchesApplied() {
        return ikumiBloodPatchApplied;
    }
}