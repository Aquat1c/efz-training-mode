#include "../include/game/character_settings.h"
#include "../include/core/constants.h"
#include "../include/core/memory.h"
#include "../include/core/logger.h"
#include "../include/game/game_state.h"

#include <algorithm>
#include <cctype>
#include <unordered_map>
#include <thread>
#include <atomic>
#include "../include/gui/imgui_impl.h"

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
        {"nagamori", CHAR_ID_MIZUKA},    // Nagamori is actually Mizuka Nagamori
        {"nanase", CHAR_ID_NANASE},      // Nanase is Rumi
        {"exnanase", CHAR_ID_EXNANASE},  // ExNanase is Doppel Nanase
        {"nayuki", CHAR_ID_NAYUKIB},     // Nayuki is actually NayukiB, this one is Neyuki
        {"nayukib", CHAR_ID_NAYUKI},     // NayukiB is actually Nayuki
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
        
        // Read Misuzu's values if either player is using her
        if (data.p1CharID == CHAR_ID_MISUZU) {
            uintptr_t featherAddr = ResolvePointer(base, EFZ_BASE_OFFSET_P1, MISUZU_FEATHER_OFFSET);
            
            if (featherAddr) SafeReadMemory(featherAddr, &data.p1MisuzuFeathers, sizeof(int));
            
            LogOut("[CHAR] Read P1 Misuzu values: Feathers=" + std::to_string(data.p1MisuzuFeathers), 
                   detailedLogging.load());
        }
        
        if (data.p2CharID == CHAR_ID_MISUZU) {
            uintptr_t featherAddr = ResolvePointer(base, EFZ_BASE_OFFSET_P2, MISUZU_FEATHER_OFFSET);
            
            if (featherAddr) SafeReadMemory(featherAddr, &data.p2MisuzuFeathers, sizeof(int));
            
            LogOut("[CHAR] Read P2 Misuzu values: Feathers=" + std::to_string(data.p2MisuzuFeathers), 
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
            // For infinite mode, set genocide timer to max, otherwise use the provided value
            int genocideValue = data.infiniteBloodMode ? IKUMI_GENOCIDE_MAX : 
                              std::max<int>(0, std::min<int>(IKUMI_GENOCIDE_MAX, data.p1IkumiGenocide));
            
            if (bloodAddr) SafeWriteMemory(bloodAddr, &bloodValue, sizeof(int));
            if (genocideAddr) SafeWriteMemory(genocideAddr, &genocideValue, sizeof(int));
            
            LogOut("[CHAR] Applied P1 Ikumi values: Blood=" + std::to_string(bloodValue) + 
                   ", Genocide=" + std::to_string(genocideValue) + " (infinite: " + 
                   (data.infiniteBloodMode ? "ON" : "OFF") + ")", 
                   detailedLogging.load());
        }
        
        // Fix for lines 158-159
        if (data.p2CharID == CHAR_ID_IKUMI) {
            uintptr_t bloodAddr = ResolvePointer(base, EFZ_BASE_OFFSET_P2, IKUMI_BLOOD_OFFSET);
            uintptr_t genocideAddr = ResolvePointer(base, EFZ_BASE_OFFSET_P2, IKUMI_GENOCIDE_OFFSET);
            
            // Fix: Add explicit template parameters to std::max and std::min
            int bloodValue = std::max<int>(0, std::min<int>(IKUMI_BLOOD_MAX, data.p2IkumiBlood));
            // For infinite mode, set genocide timer to max, otherwise use the provided value
            int genocideValue = data.infiniteBloodMode ? IKUMI_GENOCIDE_MAX : 
                              std::max<int>(0, std::min<int>(IKUMI_GENOCIDE_MAX, data.p2IkumiGenocide));
            
            if (bloodAddr) SafeWriteMemory(bloodAddr, &bloodValue, sizeof(int));
            if (genocideAddr) SafeWriteMemory(genocideAddr, &genocideValue, sizeof(int));
            
            LogOut("[CHAR] Applied P2 Ikumi values: Blood=" + std::to_string(bloodValue) + 
                   ", Genocide=" + std::to_string(genocideValue), 
                   detailedLogging.load());
        }
        
        // Apply Misuzu's values if either player is using her
        if (data.p1CharID == CHAR_ID_MISUZU) {
            uintptr_t featherAddr = ResolvePointer(base, EFZ_BASE_OFFSET_P1, MISUZU_FEATHER_OFFSET);
            
            int featherValue = std::max<int>(0, std::min<int>(MISUZU_FEATHER_MAX, data.p1MisuzuFeathers));
            
            if (featherAddr) SafeWriteMemory(featherAddr, &featherValue, sizeof(int));
            
            LogOut("[CHAR] Applied P1 Misuzu values: Feathers=" + std::to_string(featherValue), 
                   detailedLogging.load());
        }
        
        if (data.p2CharID == CHAR_ID_MISUZU) {
            uintptr_t featherAddr = ResolvePointer(base, EFZ_BASE_OFFSET_P2, MISUZU_FEATHER_OFFSET);
            
            int featherValue = std::max<int>(0, std::min<int>(MISUZU_FEATHER_MAX, data.p2MisuzuFeathers));
            
            if (featherAddr) SafeWriteMemory(featherAddr, &featherValue, sizeof(int));
            
            LogOut("[CHAR] Applied P2 Misuzu values: Feathers=" + std::to_string(featherValue), 
                   detailedLogging.load());
        }
        
        // Apply Blue IC/Red IC toggle for both players
        if (data.p1BlueIC || data.p2BlueIC) {
            if (data.p1BlueIC) {
                uintptr_t icAddr = ResolvePointer(base, EFZ_BASE_OFFSET_P1, IC_COLOR_OFFSET);
                if (icAddr) {
                    int icValue = 1; // 1 = Blue IC
                    SafeWriteMemory(icAddr, &icValue, sizeof(int));
                    LogOut("[IC] Applied P1 Blue IC", detailedLogging.load());
                }
            }
            
            if (data.p2BlueIC) {
                uintptr_t icAddr = ResolvePointer(base, EFZ_BASE_OFFSET_P2, IC_COLOR_OFFSET);
                if (icAddr) {
                    int icValue = 1; // 1 = Blue IC
                    SafeWriteMemory(icAddr, &icValue, sizeof(int));
                    LogOut("[IC] Applied P2 Blue IC", detailedLogging.load());
                }
            }
        }
        
        // Apply any character-specific patches if enabled
        // Always restart character patches when applying values
        // This ensures the monitoring thread is updated with the latest settings
        RemoveCharacterPatches();
        
        if (data.infiniteBloodMode || data.infiniteFeatherMode || data.p1BlueIC || data.p2BlueIC) {
            ApplyCharacterPatches(data);
        }
    }
    
    // Track if character value monitoring thread is active
    static std::atomic<bool> valueMonitoringActive(false);
    static std::thread valueMonitoringThread;
    
    // Track previous values for Misuzu's feather count
    static int p1LastFeatherCount = 0;
    static int p2LastFeatherCount = 0;

    // Function to continuously monitor and preserve character-specific values
    void CharacterValueMonitoringThread() {
        LogOut("[CHAR] Starting character value monitoring thread", true);
        
    // Adaptive sleep to reduce CPU when stable
    static int currentSleepMs = 16;          // starts responsive
    const int minSleepMs = 16;               // don't go faster than ~60 Hz
    const int maxSleepMs = 64;               // back off up to ~15 Hz when stable
    int consecutiveStableIters = 0;
        
        while (valueMonitoringActive) {
            uintptr_t base = GetEFZBase();
            if (base && g_featuresEnabled) {
                DisplayData localData = displayData; // Make a local copy to work with
                bool didWriteThisLoop = false;
                
                // Ikumi's genocide timer - write-on-drift approach
                if (localData.infiniteBloodMode) {
                    // P1 Ikumi genocide timer preservation
                    if (localData.p1CharID == CHAR_ID_IKUMI) {
                        uintptr_t genocideAddr = ResolvePointer(base, EFZ_BASE_OFFSET_P1, IKUMI_GENOCIDE_OFFSET);
                        if (genocideAddr) {
                            // Keep timer near max, but avoid constant writes if already there
                            int current = 0;
                            SafeReadMemory(genocideAddr, &current, sizeof(int));
                            const int target = IKUMI_GENOCIDE_MAX;
                            if (current < target) {
                                SafeWriteMemory(genocideAddr, &target, sizeof(int));
                                didWriteThisLoop = true;
                            }
                        }
                    }
                    
                    // P2 Ikumi genocide timer preservation
                    if (localData.p2CharID == CHAR_ID_IKUMI) {
                        uintptr_t genocideAddr = ResolvePointer(base, EFZ_BASE_OFFSET_P2, IKUMI_GENOCIDE_OFFSET);
                        if (genocideAddr) {
                            int current = 0;
                            SafeReadMemory(genocideAddr, &current, sizeof(int));
                            const int target = IKUMI_GENOCIDE_MAX;
                            if (current < target) {
                                SafeWriteMemory(genocideAddr, &target, sizeof(int));
                                didWriteThisLoop = true;
                            }
                        }
                    }
                }
                
                // Misuzu's feather count - continuous overwrite approach (freeze functionality)
                if (localData.infiniteFeatherMode) {
                    // P1 Misuzu feather preservation
                    if (localData.p1CharID == CHAR_ID_MISUZU && p1LastFeatherCount > 0) {
                        uintptr_t featherAddr = ResolvePointer(base, EFZ_BASE_OFFSET_P1, MISUZU_FEATHER_OFFSET);
                        if (featherAddr) {
                            int currentFeatherCount = 0;
                            SafeReadMemory(featherAddr, &currentFeatherCount, sizeof(int));
                            
                            // If feathers have decreased, immediately restore
                            if (currentFeatherCount < p1LastFeatherCount) {
                                SafeWriteMemory(featherAddr, &p1LastFeatherCount, sizeof(int));
                                didWriteThisLoop = true;
                      LogOut("[CHAR] Restored P1 Misuzu feathers from " + 
                          std::to_string(currentFeatherCount) + " to " + 
                          std::to_string(p1LastFeatherCount), 
                          detailedLogging.load());
                            }
                            // Update tracking if feathers increased (player gained feathers)
                            else if (currentFeatherCount > p1LastFeatherCount) {
                                p1LastFeatherCount = currentFeatherCount;
                      LogOut("[CHAR] P1 Misuzu gained feathers, new count: " + 
                          std::to_string(p1LastFeatherCount), 
                          detailedLogging.load());
                            }
                        }
                    }
                    
                    // P2 Misuzu feather preservation
                    if (localData.p2CharID == CHAR_ID_MISUZU && p2LastFeatherCount > 0) {
                        uintptr_t featherAddr = ResolvePointer(base, EFZ_BASE_OFFSET_P2, MISUZU_FEATHER_OFFSET);
                        if (featherAddr) {
                            int currentFeatherCount = 0;
                            SafeReadMemory(featherAddr, &currentFeatherCount, sizeof(int));
                            
                            // If feathers have decreased, immediately restore
                            if (currentFeatherCount < p2LastFeatherCount) {
                                SafeWriteMemory(featherAddr, &p2LastFeatherCount, sizeof(int));
                                didWriteThisLoop = true;
                      LogOut("[CHAR] Restored P2 Misuzu feathers from " + 
                          std::to_string(currentFeatherCount) + " to " + 
                          std::to_string(p2LastFeatherCount), 
                          detailedLogging.load());
                            }
                            // Update tracking if feathers increased (player gained feathers)
                            else if (currentFeatherCount > p2LastFeatherCount) {
                                p2LastFeatherCount = currentFeatherCount;
                      LogOut("[CHAR] P2 Misuzu gained feathers, new count: " + 
                          std::to_string(p2LastFeatherCount), 
                          detailedLogging.load());
                            }
                        }
                    }
                } 
                else {
                    // Reset the stored values when feature is disabled
                    p1LastFeatherCount = 0;
                    p2LastFeatherCount = 0;
                }
                
                // Blue IC/Red IC toggle - apply only when needed
                if (localData.p1BlueIC) {
                    uintptr_t icAddr = ResolvePointer(base, EFZ_BASE_OFFSET_P1, IC_COLOR_OFFSET);
                    if (icAddr) {
                        int currentIC = 0;
                        SafeReadMemory(icAddr, &currentIC, sizeof(int));
                        if (currentIC != 1) {
                            int icValue = 1; // 1 = Blue IC
                            SafeWriteMemory(icAddr, &icValue, sizeof(int));
                            didWriteThisLoop = true;
                        }
                    }
                }
                
                if (localData.p2BlueIC) {
                    uintptr_t icAddr = ResolvePointer(base, EFZ_BASE_OFFSET_P2, IC_COLOR_OFFSET);
                    if (icAddr) {
                        int currentIC = 0;
                        SafeReadMemory(icAddr, &currentIC, sizeof(int));
                        if (currentIC != 1) {
                            int icValue = 1; // 1 = Blue IC
                            SafeWriteMemory(icAddr, &icValue, sizeof(int));
                            didWriteThisLoop = true;
                        }
                    }
                }
                
                // Adjust backoff based on whether we wrote this loop
                if (didWriteThisLoop) {
                    currentSleepMs = minSleepMs;
                    consecutiveStableIters = 0;
                } else {
                    consecutiveStableIters++;
                    if (consecutiveStableIters > 2) { // after a few stable ticks, back off
                        currentSleepMs = (std::min)(currentSleepMs * 2, maxSleepMs);
                    }
                }
            }
            
            // Sleep to avoid hammering the CPU; adaptive backoff keeps things light when stable
            std::this_thread::sleep_for(std::chrono::milliseconds(currentSleepMs));
        }
        
        LogOut("[CHAR] Character value monitoring thread stopped", true);
    }

    // Update ApplyCharacterPatches to start the monitoring thread
    void ApplyCharacterPatches(const DisplayData& data) {
        // Only apply monitoring if:
        // 1. Any infinite mode is enabled OR Blue IC is enabled
        // 2. At least one player is using a supported character (for infinite modes)
        // 3. We're in a valid game mode (practice mode)
        
        bool shouldMonitorIkumi = data.infiniteBloodMode && 
                               (data.p1CharID == CHAR_ID_IKUMI || data.p2CharID == CHAR_ID_IKUMI);
        
        bool shouldMonitorMisuzu = data.infiniteFeatherMode &&
                                (data.p1CharID == CHAR_ID_MISUZU || data.p2CharID == CHAR_ID_MISUZU);
                                
        bool shouldMonitorIC = data.p1BlueIC || data.p2BlueIC;
        
        if (!shouldMonitorIkumi && !shouldMonitorMisuzu && !shouldMonitorIC) {
            LogOut("[CHAR] No character monitoring needed - no infinite modes, Blue IC, or supported characters", true);
            return;
        }
        
        // Verify we're in a valid game state before monitoring
        GameMode currentMode = GetCurrentGameMode();
        if (currentMode != GameMode::Practice) {
            LogOut("[CHAR] Not applying character monitoring - not in practice mode", true);
            return;
        }
        
        // Start value monitoring thread if not already running
        if (!valueMonitoringActive) {
            // Initialize the tracking variables with current values
            uintptr_t base = GetEFZBase();
            if (base) {
                if (data.infiniteFeatherMode) {
                    if (data.p1CharID == CHAR_ID_MISUZU) {
                        uintptr_t featherAddr = ResolvePointer(base, EFZ_BASE_OFFSET_P1, MISUZU_FEATHER_OFFSET);
                        if (featherAddr) {
                            SafeReadMemory(featherAddr, &p1LastFeatherCount, sizeof(int));
                            LogOut("[CHAR] Initialized P1 feather count to: " + std::to_string(p1LastFeatherCount), true);
                        }
                    }
                    if (data.p2CharID == CHAR_ID_MISUZU) {
                        uintptr_t featherAddr = ResolvePointer(base, EFZ_BASE_OFFSET_P2, MISUZU_FEATHER_OFFSET);
                        if (featherAddr) {
                            SafeReadMemory(featherAddr, &p2LastFeatherCount, sizeof(int));
                            LogOut("[CHAR] Initialized P2 feather count to: " + std::to_string(p2LastFeatherCount), true);
                        }
                    }
                }
            }
            
            // Initialize the thread
            valueMonitoringActive = true;
            valueMonitoringThread = std::thread(CharacterValueMonitoringThread);
            LogOut("[CHAR] Started character value monitoring thread", true);
        }
    }

    // Update RemoveCharacterPatches to stop the monitoring thread
    void RemoveCharacterPatches() {
        // Stop the value monitoring thread if it's running
        if (valueMonitoringActive) {
            valueMonitoringActive = false;
            
            if (valueMonitoringThread.joinable()) {
                valueMonitoringThread.join();
            }
            
            LogOut("[CHAR] Stopped character value monitoring thread", true);
        }
    }
    
    // Implementation for Misuzu-specific patches
    void RemoveMisuzuPatches() {
        // We're using value monitoring instead of code patching,
        // so we don't need specific removal code for Misuzu
        // Just reset the tracking variables
        p1LastFeatherCount = 0;
        p2LastFeatherCount = 0;
    }

    // Function to get the status of the monitoring thread for debugging
    bool IsMonitoringThreadActive() {
        return valueMonitoringActive.load();
    }
    
    // Function to get the current feather counts for debugging
    void GetFeatherCounts(int& p1Count, int& p2Count) {
        p1Count = p1LastFeatherCount;
        p2Count = p2LastFeatherCount;
    }
}