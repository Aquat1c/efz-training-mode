#include "../include/game/character_settings.h"
#include "../include/core/constants.h"
#include "../include/core/memory.h"
#include "../include/core/logger.h"
#include "../include/game/game_state.h"
#include "../include/game/frame_monitor.h"
#include "../include/utils/utilities.h"

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
        
        // Read Mishio's values if either player is using her
        if (data.p1CharID == CHAR_ID_MISHIO) {
            uintptr_t elemAddr = ResolvePointer(base, EFZ_BASE_OFFSET_P1, MISHIO_ELEMENT_OFFSET);
            uintptr_t awAddr   = ResolvePointer(base, EFZ_BASE_OFFSET_P1, MISHIO_AWAKENED_TIMER_OFFSET);
            if (elemAddr) SafeReadMemory(elemAddr, &data.p1MishioElement, sizeof(int));
            if (awAddr)   SafeReadMemory(awAddr,   &data.p1MishioAwakenedTimer, sizeof(int));
            LogOut("[CHAR] Read P1 Mishio values: Element=" + std::to_string(data.p1MishioElement) +
                   ", AwTimer=" + std::to_string(data.p1MishioAwakenedTimer), detailedLogging.load());
        }
        if (data.p2CharID == CHAR_ID_MISHIO) {
            uintptr_t elemAddr = ResolvePointer(base, EFZ_BASE_OFFSET_P2, MISHIO_ELEMENT_OFFSET);
            uintptr_t awAddr   = ResolvePointer(base, EFZ_BASE_OFFSET_P2, MISHIO_AWAKENED_TIMER_OFFSET);
            if (elemAddr) SafeReadMemory(elemAddr, &data.p2MishioElement, sizeof(int));
            if (awAddr)   SafeReadMemory(awAddr,   &data.p2MishioAwakenedTimer, sizeof(int));
            LogOut("[CHAR] Read P2 Mishio values: Element=" + std::to_string(data.p2MishioElement) +
                   ", AwTimer=" + std::to_string(data.p2MishioAwakenedTimer), detailedLogging.load());
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

        // Doppel Nanase (ExNanase) - read Enlightened flag (0/1)
        if (data.p1CharID == CHAR_ID_EXNANASE) {
            uintptr_t flagAddr = ResolvePointer(base, EFZ_BASE_OFFSET_P1, DOPPEL_ENLIGHTENED_OFFSET);
            int tmp = 0; if (flagAddr) SafeReadMemory(flagAddr, &tmp, sizeof(int));
            data.p1DoppelEnlightened = (tmp != 0);
            LogOut("[CHAR] Read P1 Doppel Enlightened=" + std::to_string(data.p1DoppelEnlightened), detailedLogging.load());
        }
        if (data.p2CharID == CHAR_ID_EXNANASE) {
            uintptr_t flagAddr = ResolvePointer(base, EFZ_BASE_OFFSET_P2, DOPPEL_ENLIGHTENED_OFFSET);
            int tmp = 0; if (flagAddr) SafeReadMemory(flagAddr, &tmp, sizeof(int));
            data.p2DoppelEnlightened = (tmp != 0);
            LogOut("[CHAR] Read P2 Doppel Enlightened=" + std::to_string(data.p2DoppelEnlightened), detailedLogging.load());
        }

        // Nanase (Rumi) – Safe read of mode/gate only (no pointer derefs to anim/move tables)
        auto ReadRumiState = [&](int playerIndex) {
            const int baseOffset = (playerIndex == 1) ? EFZ_BASE_OFFSET_P1 : EFZ_BASE_OFFSET_P2;
            uintptr_t modeAddr = ResolvePointer(base, baseOffset, RUMI_MODE_BYTE_OFFSET);
            uintptr_t gateAddr = ResolvePointer(base, baseOffset, RUMI_WEAPON_GATE_OFFSET);
            uintptr_t kimchiFlagAddr = ResolvePointer(base, baseOffset, RUMI_KIMCHI_ACTIVE_OFFSET);
            uintptr_t kimchiTimerAddr = ResolvePointer(base, baseOffset, RUMI_KIMCHI_TIMER_OFFSET);
            uint8_t mode = 0, gate = 0;
            if (modeAddr) SafeReadMemory(modeAddr, &mode, sizeof(uint8_t));
            if (gateAddr) SafeReadMemory(gateAddr, &gate, sizeof(uint8_t));
            bool bare = (mode != 0) || (gate != 0);
            if (playerIndex == 1) {
                data.p1RumiBarehanded = bare;
                if (kimchiFlagAddr) { int f=0; SafeReadMemory(kimchiFlagAddr, &f, sizeof(int)); data.p1RumiKimchiActive = (f!=0); }
                if (kimchiTimerAddr) { int t=0; SafeReadMemory(kimchiTimerAddr, &t, sizeof(int)); data.p1RumiKimchiTimer = t; }
            } else {
                data.p2RumiBarehanded = bare;
                if (kimchiFlagAddr) { int f=0; SafeReadMemory(kimchiFlagAddr, &f, sizeof(int)); data.p2RumiKimchiActive = (f!=0); }
                if (kimchiTimerAddr) { int t=0; SafeReadMemory(kimchiTimerAddr, &t, sizeof(int)); data.p2RumiKimchiTimer = t; }
            }
            LogOut(std::string("[CHAR] Read Rumi state ") + (playerIndex==1?"P1":"P2") + ": mode=" + std::to_string((int)mode) + ", gate=" + std::to_string((int)gate), detailedLogging.load());
        };

        if (data.p1CharID == CHAR_ID_NANASE) {
            ReadRumiState(1);
        }
        if (data.p2CharID == CHAR_ID_NANASE) {
            ReadRumiState(2);
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
        
        // Apply Mishio's values (element and awakened timer)
        if (data.p1CharID == CHAR_ID_MISHIO) {
            uintptr_t elemAddr = ResolvePointer(base, EFZ_BASE_OFFSET_P1, MISHIO_ELEMENT_OFFSET);
            uintptr_t awAddr   = ResolvePointer(base, EFZ_BASE_OFFSET_P1, MISHIO_AWAKENED_TIMER_OFFSET);
            if (elemAddr) {
                int elem = CLAMP(data.p1MishioElement, MISHIO_ELEM_NONE, MISHIO_ELEM_AWAKENED);
                SafeWriteMemory(elemAddr, &elem, sizeof(int));
            }
            if (awAddr) {
                int aw = data.p1MishioAwakenedTimer;
                if (aw < 0) aw = 0;
                if (aw > MISHIO_AWAKENED_TARGET) aw = MISHIO_AWAKENED_TARGET;
                SafeWriteMemory(awAddr, &aw, sizeof(int));
            }
            LogOut("[CHAR] Applied P1 Mishio values: Elem=" + std::to_string(data.p1MishioElement) +
                   ", AwTimer=" + std::to_string(data.p1MishioAwakenedTimer), detailedLogging.load());
        }
        if (data.p2CharID == CHAR_ID_MISHIO) {
            uintptr_t elemAddr = ResolvePointer(base, EFZ_BASE_OFFSET_P2, MISHIO_ELEMENT_OFFSET);
            uintptr_t awAddr   = ResolvePointer(base, EFZ_BASE_OFFSET_P2, MISHIO_AWAKENED_TIMER_OFFSET);
            if (elemAddr) {
                int elem = CLAMP(data.p2MishioElement, MISHIO_ELEM_NONE, MISHIO_ELEM_AWAKENED);
                SafeWriteMemory(elemAddr, &elem, sizeof(int));
            }
            if (awAddr) {
                int aw = data.p2MishioAwakenedTimer;
                if (aw < 0) aw = 0;
                if (aw > MISHIO_AWAKENED_TARGET) aw = MISHIO_AWAKENED_TARGET;
                SafeWriteMemory(awAddr, &aw, sizeof(int));
            }
            LogOut("[CHAR] Applied P2 Mishio values: Elem=" + std::to_string(data.p2MishioElement) +
                   ", AwTimer=" + std::to_string(data.p2MishioAwakenedTimer), detailedLogging.load());
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

        // Doppel Enlightened: simple checkbox -> set flag 1 when checked, 0 when unchecked
        if (data.p1CharID == CHAR_ID_EXNANASE) {
            uintptr_t flagAddr = ResolvePointer(base, EFZ_BASE_OFFSET_P1, DOPPEL_ENLIGHTENED_OFFSET);
            if (flagAddr) {
                int v = data.p1DoppelEnlightened ? 1 : 0;
                SafeWriteMemory(flagAddr, &v, sizeof(int));
                LogOut("[CHAR] Applied P1 Doppel Enlightened=" + std::to_string(v), detailedLogging.load());
            }
        }
        if (data.p2CharID == CHAR_ID_EXNANASE) {
            uintptr_t flagAddr = ResolvePointer(base, EFZ_BASE_OFFSET_P2, DOPPEL_ENLIGHTENED_OFFSET);
            if (flagAddr) {
                int v = data.p2DoppelEnlightened ? 1 : 0;
                SafeWriteMemory(flagAddr, &v, sizeof(int));
                LogOut("[CHAR] Applied P2 Doppel Enlightened=" + std::to_string(v), detailedLogging.load());
            }
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

        // Rumi – Prefer the game's own toggle routine for safe mode swaps (with safe fallback)
        auto ApplyRumiMode = [&](int playerIndex, bool barehanded) {
            if (!AreCharactersInitialized()) return;
            const int baseOffset = (playerIndex == 1) ? EFZ_BASE_OFFSET_P1 : EFZ_BASE_OFFSET_P2;
            short moveID = 0;
            if (uintptr_t mv = ResolvePointer(base, baseOffset, MOVE_ID_OFFSET)) {
                SafeReadMemory(mv, &moveID, sizeof(short));
            }
            if (!IsActionable(moveID)) {
                LogOut(std::string("[CHAR] Rumi mode change deferred – not safe state (moveID=") + std::to_string(moveID) + ")", true);
                return;
            }

            uintptr_t modeByteAddr  = ResolvePointer(base, baseOffset, RUMI_MODE_BYTE_OFFSET);
            uintptr_t gateAddr      = ResolvePointer(base, baseOffset, RUMI_WEAPON_GATE_OFFSET);
            if (!modeByteAddr || !gateAddr) return;

            uint8_t curMode = 0, curGate = 0;
            SafeReadMemory(modeByteAddr, &curMode, sizeof(uint8_t));
            SafeReadMemory(gateAddr, &curGate, sizeof(uint8_t));
            const uint8_t desiredMode = barehanded ? 1 : 0;
            if (curMode == desiredMode && curGate == desiredMode) {
                LogOut(std::string("[CHAR] Rumi mode unchanged for ") + (playerIndex==1?"P1":"P2"), detailedLogging.load());
                return;
            }

            uintptr_t gameBase = GetEFZBase();

            // Resolve the player base ("this")
            uintptr_t playerThis = 0;
            SafeReadMemory(gameBase + ((playerIndex == 1) ? EFZ_BASE_OFFSET_P1 : EFZ_BASE_OFFSET_P2), &playerThis, sizeof(uintptr_t));
            if (!playerThis) return;

            // Validate the engine function prologue bytes before calling
            uint8_t prologue[5] = {0};
            bool looksValid = false;
            if (SafeReadMemory(gameBase + TOGGLE_CHARACTER_MODE_RVA, prologue, sizeof(prologue))) {
                // Accept common x86 prologues: push ebp; mov ebp,esp OR mov edi,edi; push ebp; mov ebp,esp
                looksValid = (prologue[0] == 0x55 && prologue[1] == 0x8B && prologue[2] == 0xEC) ||
                             (prologue[0] == 0x8B && prologue[1] == 0xFF && prologue[2] == 0x55 && prologue[3] == 0x8B && prologue[4] == 0xEC);
            }

            bool usedEngine = false;
            if (looksValid) {
                // Call the engine function toggleCharacterMode(char* this, int unusedEDX, char targetMode) using __fastcall
                using ToggleModeFn = int(__fastcall*)(uintptr_t /*this*/, int /*edx_unused*/, char /*targetMode*/);
                ToggleModeFn ToggleCharacterMode = reinterpret_cast<ToggleModeFn>(gameBase + TOGGLE_CHARACTER_MODE_RVA);
                ToggleCharacterMode(playerThis, 0, (char)desiredMode);
                usedEngine = true;
            }

            if (!usedEngine) {
                // Safe fallback: perform manual pointer swap with strict checks
                uintptr_t srcAnimPtrAddr = ResolvePointer(base, baseOffset, barehanded ? RUMI_ALT_ANIM_PTR_OFFSET  : RUMI_NORM_ANIM_PTR_OFFSET);
                uintptr_t srcMovePtrAddr = ResolvePointer(base, baseOffset, barehanded ? RUMI_ALT_MOVE_PTR_OFFSET  : RUMI_NORM_MOVE_PTR_OFFSET);
                uintptr_t dstAnimAddr    = ResolvePointer(base, baseOffset, RUMI_ACTIVE_ANIM_PTR_DST);
                uintptr_t dstMoveAddr    = ResolvePointer(base, baseOffset, RUMI_ACTIVE_MOVE_PTR_DST);
                if (!srcAnimPtrAddr || !srcMovePtrAddr || !dstAnimAddr || !dstMoveAddr) {
                    LogOut("[CHAR] Rumi fallback swap aborted: missing pointer addresses", true);
                    return;
                }
                uintptr_t srcAnim = 0, srcMove = 0;
                SafeReadMemory(srcAnimPtrAddr, &srcAnim, sizeof(uintptr_t));
                SafeReadMemory(srcMovePtrAddr, &srcMove, sizeof(uintptr_t));
                if (!srcAnim || !srcMove) {
                    LogOut("[CHAR] Rumi fallback swap aborted: null source pointers", true);
                    return;
                }
                // Write destination pointers and mode byte
                SafeWriteMemory(dstAnimAddr, &srcAnim, sizeof(uintptr_t));
                SafeWriteMemory(dstMoveAddr, &srcMove, sizeof(uintptr_t));
                uint8_t modeVal = desiredMode; SafeWriteMemory(modeByteAddr, &modeVal, sizeof(uint8_t));
            }

            // Sync the gate byte to match the selected mode
            uint8_t gate = desiredMode;
            SafeWriteMemory(gateAddr, &gate, sizeof(uint8_t));
            LogOut(std::string("[CHAR] Rumi set to ") + (barehanded?"barehand":"shinai") + (usedEngine?" (engine)":" (fallback)"), detailedLogging.load());
        };

        if (data.p1CharID == CHAR_ID_NANASE) {
            // Infinite Shinai overrides UI selection; force Shinai when enabled
            const bool wantBarehand = data.p1RumiInfiniteShinai ? false : data.p1RumiBarehanded;
            ApplyRumiMode(1, wantBarehand);
            // Apply Kimchi activation/timer if fields are present
            if (uintptr_t flag = ResolvePointer(base, EFZ_BASE_OFFSET_P1, RUMI_KIMCHI_ACTIVE_OFFSET)) {
                int newV = data.p1RumiKimchiActive ? 1 : 0;
                int curV = 0; SafeReadMemory(flag, &curV, sizeof(int));
                if (curV != newV) { SafeWriteMemory(flag, &newV, sizeof(int)); }
                // If activating now (rising edge), set MoveID to Kimchi (307) once
                if (curV == 0 && newV == 1) {
                    if (auto mvAddr = ResolvePointer(base, EFZ_BASE_OFFSET_P1, MOVE_ID_OFFSET)) {
                        short k = RUMI_KIMCHI_MOVE_ID; SafeWriteMemory(mvAddr, &k, sizeof(short));
                        LogOut("[CHAR] P1 Kimchi activated -> set MoveID 307 once", true);
                    }
                }
            }
            if (uintptr_t tim = ResolvePointer(base, EFZ_BASE_OFFSET_P1, RUMI_KIMCHI_TIMER_OFFSET)) {
                int t = data.p1RumiKimchiTimer; if (t < 0) t = 0; if (t > RUMI_KIMCHI_TARGET) t = RUMI_KIMCHI_TARGET; SafeWriteMemory(tim, &t, sizeof(int));
            }
        }
        if (data.p2CharID == CHAR_ID_NANASE) {
            const bool wantBarehand = data.p2RumiInfiniteShinai ? false : data.p2RumiBarehanded;
            ApplyRumiMode(2, wantBarehand);
            if (uintptr_t flag = ResolvePointer(base, EFZ_BASE_OFFSET_P2, RUMI_KIMCHI_ACTIVE_OFFSET)) {
                int newV = data.p2RumiKimchiActive ? 1 : 0;
                int curV = 0; SafeReadMemory(flag, &curV, sizeof(int));
                if (curV != newV) { SafeWriteMemory(flag, &newV, sizeof(int)); }
                if (curV == 0 && newV == 1) {
                    if (auto mvAddr = ResolvePointer(base, EFZ_BASE_OFFSET_P2, MOVE_ID_OFFSET)) {
                        short k = RUMI_KIMCHI_MOVE_ID; SafeWriteMemory(mvAddr, &k, sizeof(short));
                        LogOut("[CHAR] P2 Kimchi activated -> set MoveID 307 once", true);
                    }
                }
            }
            if (uintptr_t tim = ResolvePointer(base, EFZ_BASE_OFFSET_P2, RUMI_KIMCHI_TIMER_OFFSET)) {
                int t = data.p2RumiKimchiTimer; if (t < 0) t = 0; if (t > RUMI_KIMCHI_TARGET) t = RUMI_KIMCHI_TARGET; SafeWriteMemory(tim, &t, sizeof(int));
            }
        }
        
        // Apply any character-specific patches if enabled
        // Always restart character patches when applying values
        // This ensures the monitoring thread is updated with the latest settings
        RemoveCharacterPatches();
        
    bool wantRumiMonitor = (data.p1CharID == CHAR_ID_NANASE && (data.p1RumiInfiniteShinai || data.p1RumiInfiniteKimchi)) ||
                (data.p2CharID == CHAR_ID_NANASE && (data.p2RumiInfiniteShinai || data.p2RumiInfiniteKimchi));
        if (data.infiniteBloodMode || data.infiniteFeatherMode || data.infiniteMishioElement || data.infiniteMishioAwakened ||
            data.p1BlueIC || data.p2BlueIC || wantRumiMonitor) {
            ApplyCharacterPatches(data);
        }
    }
    
    // Track if character value monitoring thread is active
    static std::atomic<bool> valueMonitoringActive(false);
    static std::thread valueMonitoringThread;
    
    // Track previous values for Misuzu's feather count
    static int p1LastFeatherCount = 0;
    static int p2LastFeatherCount = 0;
    // Track Mishio's last observed values for preservation logic
    static int p1LastMishioElem = -1;
    static int p2LastMishioElem = -1;

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
                
                // Mishio's element preservation (freeze/restore chosen element)
                if (localData.infiniteMishioElement) {
                    if (localData.p1CharID == CHAR_ID_MISHIO) {
                        uintptr_t elemAddr = ResolvePointer(base, EFZ_BASE_OFFSET_P1, MISHIO_ELEMENT_OFFSET);
                        if (elemAddr) {
                            int current = 0;
                            SafeReadMemory(elemAddr, &current, sizeof(int));
                            int target = CLAMP(localData.p1MishioElement, MISHIO_ELEM_NONE, MISHIO_ELEM_AWAKENED);
                            // Initialize last if first time
                            if (p1LastMishioElem == -1) p1LastMishioElem = current;
                            if (current != target) {
                                SafeWriteMemory(elemAddr, &target, sizeof(int));
                                p1LastMishioElem = target;
                                didWriteThisLoop = true;
                            }
                        }
                    } else {
                        p1LastMishioElem = -1;
                    }
                    if (localData.p2CharID == CHAR_ID_MISHIO) {
                        uintptr_t elemAddr = ResolvePointer(base, EFZ_BASE_OFFSET_P2, MISHIO_ELEMENT_OFFSET);
                        if (elemAddr) {
                            int current = 0;
                            SafeReadMemory(elemAddr, &current, sizeof(int));
                            int target = CLAMP(localData.p2MishioElement, MISHIO_ELEM_NONE, MISHIO_ELEM_AWAKENED);
                            if (p2LastMishioElem == -1) p2LastMishioElem = current;
                            if (current != target) {
                                SafeWriteMemory(elemAddr, &target, sizeof(int));
                                p2LastMishioElem = target;
                                didWriteThisLoop = true;
                            }
                        }
                    } else {
                        p2LastMishioElem = -1;
                    }
                } else {
                    p1LastMishioElem = -1;
                    p2LastMishioElem = -1;
                }

                // Mishio's awakened timer preservation (only while Awakened)
                if (localData.infiniteMishioAwakened) {
                    if (localData.p1CharID == CHAR_ID_MISHIO) {
                        uintptr_t elemAddr = ResolvePointer(base, EFZ_BASE_OFFSET_P1, MISHIO_ELEMENT_OFFSET);
                        uintptr_t awAddr   = ResolvePointer(base, EFZ_BASE_OFFSET_P1, MISHIO_AWAKENED_TIMER_OFFSET);
                        if (elemAddr && awAddr) {
                            int elem=0, cur=0; SafeReadMemory(elemAddr, &elem, sizeof(int)); SafeReadMemory(awAddr, &cur, sizeof(int));
                            if (elem == MISHIO_ELEM_AWAKENED) {
                                int target = localData.p1MishioAwakenedTimer;
                                if (target < MISHIO_AWAKENED_TARGET) target = MISHIO_AWAKENED_TARGET;
                                if (target > MISHIO_AWAKENED_TARGET) target = MISHIO_AWAKENED_TARGET; // cap
                                if (cur < target) { SafeWriteMemory(awAddr, &target, sizeof(int)); didWriteThisLoop = true; }
                            }
                        }
                    }
                    if (localData.p2CharID == CHAR_ID_MISHIO) {
                        uintptr_t elemAddr = ResolvePointer(base, EFZ_BASE_OFFSET_P2, MISHIO_ELEMENT_OFFSET);
                        uintptr_t awAddr   = ResolvePointer(base, EFZ_BASE_OFFSET_P2, MISHIO_AWAKENED_TIMER_OFFSET);
                        if (elemAddr && awAddr) {
                            int elem=0, cur=0; SafeReadMemory(elemAddr, &elem, sizeof(int)); SafeReadMemory(awAddr, &cur, sizeof(int));
                            if (elem == MISHIO_ELEM_AWAKENED) {
                                int target = localData.p2MishioAwakenedTimer;
                                if (target < MISHIO_AWAKENED_TARGET) target = MISHIO_AWAKENED_TARGET;
                                if (target > MISHIO_AWAKENED_TARGET) target = MISHIO_AWAKENED_TARGET; // cap
                                if (cur < target) { SafeWriteMemory(awAddr, &target, sizeof(int)); didWriteThisLoop = true; }
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

                // Rumi Infinite Shinai: keep gate at 0 always, and restore mode via engine when safe
                // Avoid engine toggles during problematic supers (4123641236x, 308-310); delay restore briefly after
                static int p1RestoreDelay = 0;
                static int p2RestoreDelay = 0;
                auto EnforceRumiShinai = [&](int playerIndex) {
                    const bool wantInf = (playerIndex == 1) ? (localData.p1RumiInfiniteShinai && localData.p1CharID == CHAR_ID_NANASE)
                                                           : (localData.p2RumiInfiniteShinai && localData.p2CharID == CHAR_ID_NANASE);
                    if (!wantInf) return;
                    const int baseOffset = (playerIndex == 1) ? EFZ_BASE_OFFSET_P1 : EFZ_BASE_OFFSET_P2;
                    uintptr_t modeByteAddr  = ResolvePointer(base, baseOffset, RUMI_MODE_BYTE_OFFSET);
                    uintptr_t gateAddr      = ResolvePointer(base, baseOffset, RUMI_WEAPON_GATE_OFFSET);
                    if (!modeByteAddr || !gateAddr) return;
                    uint8_t curMode=0, curGate=0; SafeReadMemory(modeByteAddr, &curMode, sizeof(uint8_t)); SafeReadMemory(gateAddr, &curGate, sizeof(uint8_t));
                    // Always keep gate = 0 to block entering barehand specials mid-move
                    if (curGate != 0) { uint8_t zero = 0; SafeWriteMemory(gateAddr, &zero, sizeof(uint8_t)); didWriteThisLoop = true; }

                    // If mode is barehanded, restore to Shinai when actionable, but skip during toss supers
                    short mv = 0; if (auto mvAddr = ResolvePointer(base, baseOffset, MOVE_ID_OFFSET)) SafeReadMemory(mvAddr, &mv, sizeof(short));
                    bool inTossSuper = (mv == RUMI_SUPER_TOSS_A || mv == RUMI_SUPER_TOSS_B || mv == RUMI_SUPER_TOSS_C);
                    int& delayRef = (playerIndex == 1) ? p1RestoreDelay : p2RestoreDelay;
                    if (inTossSuper) {
                        // While super active, never toggle; just ensure gate is 0 and set a short post delay
                        delayRef = 60; // ~20 visual frames grace
                        return;
                    }

                    if (delayRef > 0) { delayRef--; return; }

                    if (curMode != 0 && IsActionable(mv)) {
                        using ToggleModeFn = int(__fastcall*)(uintptr_t, int, char);
                        uintptr_t gameBase = GetEFZBase();
                        ToggleModeFn ToggleCharacterMode = reinterpret_cast<ToggleModeFn>(gameBase + TOGGLE_CHARACTER_MODE_RVA);
                        uintptr_t playerThis = 0;
                        SafeReadMemory(gameBase + ((playerIndex == 1) ? EFZ_BASE_OFFSET_P1 : EFZ_BASE_OFFSET_P2), &playerThis, sizeof(uintptr_t));
                        if (!playerThis) return;
                        ToggleCharacterMode(playerThis, 0, 0);
                        uint8_t zero2 = 0; SafeWriteMemory(gateAddr, &zero2, sizeof(uint8_t));
                        didWriteThisLoop = true;
                    }
                };
                if (AreCharactersInitialized()) {
                    EnforceRumiShinai(1);
                    EnforceRumiShinai(2);
                }

                // Rumi Kimchi enforcement:
                // - If Infinite enabled: keep timer topped and ensure Active=1
                auto EnforceRumiKimchi = [&](int playerIndex) {
                    const bool isRumi = (playerIndex == 1) ? (localData.p1CharID == CHAR_ID_NANASE)
                                                          : (localData.p2CharID == CHAR_ID_NANASE);
                    if (!isRumi) return;
                    const int baseOffset = (playerIndex == 1) ? EFZ_BASE_OFFSET_P1 : EFZ_BASE_OFFSET_P2;
                    uintptr_t flagAddr = ResolvePointer(base, baseOffset, RUMI_KIMCHI_ACTIVE_OFFSET);
                    uintptr_t timerAddr = ResolvePointer(base, baseOffset, RUMI_KIMCHI_TIMER_OFFSET);
                    if (!timerAddr) return;
                    // Infinite handling
                    const bool wantInf = (playerIndex == 1) ? localData.p1RumiInfiniteKimchi : localData.p2RumiInfiniteKimchi;
                    if (wantInf) {
                        int curTimer = 0; SafeReadMemory(timerAddr, &curTimer, sizeof(int));
                        int target = RUMI_KIMCHI_TARGET;
                        if (curTimer < target) { SafeWriteMemory(timerAddr, &target, sizeof(int)); didWriteThisLoop = true; }
                        if (flagAddr) {
                            int curFlag = 0; SafeReadMemory(flagAddr, &curFlag, sizeof(int));
                            if (!curFlag) { int one = 1; SafeWriteMemory(flagAddr, &one, sizeof(int)); didWriteThisLoop = true; }
                        }
                    }
                };
                EnforceRumiKimchi(1);
                EnforceRumiKimchi(2);
                
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

    bool shouldMonitorMishioElem = data.infiniteMishioElement &&
            (data.p1CharID == CHAR_ID_MISHIO || data.p2CharID == CHAR_ID_MISHIO);
        bool shouldMonitorMishioAw   = data.infiniteMishioAwakened &&
            (data.p1CharID == CHAR_ID_MISHIO || data.p2CharID == CHAR_ID_MISHIO);
    bool shouldMonitorRumiShinai = (data.p1CharID == CHAR_ID_NANASE && data.p1RumiInfiniteShinai) ||
                       (data.p2CharID == CHAR_ID_NANASE && data.p2RumiInfiniteShinai);
    bool shouldMonitorRumiKimchi =
        (data.p1CharID == CHAR_ID_NANASE && data.p1RumiInfiniteKimchi) ||
        (data.p2CharID == CHAR_ID_NANASE && data.p2RumiInfiniteKimchi);
        
    if (!shouldMonitorIkumi && !shouldMonitorMisuzu && !shouldMonitorMishioElem && !shouldMonitorMishioAw && !shouldMonitorIC && !shouldMonitorRumiShinai && !shouldMonitorRumiKimchi) {
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

                // Initialize Mishio last values if needed
                if (shouldMonitorMishioElem || shouldMonitorMishioAw) {
                    p1LastMishioElem = -1;
                    p2LastMishioElem = -1;
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