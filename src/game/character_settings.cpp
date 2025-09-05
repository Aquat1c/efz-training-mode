#include "../include/game/character_settings.h"
#include "../include/core/constants.h"
#include "../include/core/memory.h"
#include "../include/core/logger.h"
#include "../include/game/game_state.h"
#include "../include/game/frame_monitor.h"
#include "../include/utils/utilities.h"
#include "../include/core/globals.h"
#include <atomic>
#pragma message("[build] compiling updated character_settings.cpp")

#include <algorithm>
#include <cctype>
#include <unordered_map>
#include <thread>
#include <atomic>
#include "../include/gui/imgui_impl.h"
#include <chrono>

namespace CharacterSettings {
    // Track if character patches are currently applied
    static bool ikumiBloodPatchApplied = false;
    // Throttle/decimate Ikumi read logs: log on change or heartbeat only
    static int s_lastP1IkumiBlood = -1;
    static int s_lastP1IkumiGenocide = -1;
    static int s_lastP2IkumiBlood = -1;
    static int s_lastP2IkumiGenocide = -1;
    static std::chrono::steady_clock::time_point s_lastIkumiLogP1{};
    static std::chrono::steady_clock::time_point s_lastIkumiLogP2{};
    static constexpr std::chrono::seconds IKUMI_LOG_HEARTBEAT{5};

    // Rumi (Nanase) change-only logging with heartbeat
    static int s_lastRumiModeP1 = -1;
    static int s_lastRumiGateP1 = -1;
    static int s_lastRumiModeP2 = -1;
    static int s_lastRumiGateP2 = -1;
    static std::chrono::steady_clock::time_point s_lastRumiLogP1{};
    static std::chrono::steady_clock::time_point s_lastRumiLogP2{};
    static constexpr std::chrono::seconds RUMI_LOG_HEARTBEAT{5};

    // Akiko change-only logging heartbeat
    static int s_lastAkikoBulletP1 = -1;
    static int s_lastAkikoBulletP2 = -1;
    static int s_lastAkikoTimeP1 = -1;
    static int s_lastAkikoTimeP2 = -1;
    static std::chrono::steady_clock::time_point s_lastAkikoLogP1{};
    static std::chrono::steady_clock::time_point s_lastAkikoLogP2{};
    static constexpr std::chrono::seconds AKIKO_LOG_HEARTBEAT{5};
    // Track previous character IDs to detect switches (for Akiko timeslow reset)
    static int s_prevCharIDP1 = -2;
    static int s_prevCharIDP2 = -2;

    // Akiko bullet cycle freeze: store captured cycle on enable (rising edge)
    static bool s_prevP1AkikoFreeze = false;
    static bool s_prevP2AkikoFreeze = false;
    static int  s_p1AkikoFrozenCycle = 0;
    static int  s_p2AkikoFrozenCycle = 0;
    
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
        {"nayuki",  CHAR_ID_NAYUKI},     //  "nayuki" is Sleepy variant (Neyuki)
        {"nayukib", CHAR_ID_NAYUKIB},    //  "nayukib" is Awake variant (Nayuki)
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
            case CHAR_ID_NAYUKI:   return "Neyuki";   // Sleepy Nayuki (nayuki in files)
            case CHAR_ID_NAYUKIB:  return "Nayuki";   // Awake Nayuki (nayukib in files) 
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
            // Change-only logging with periodic heartbeat
            bool changed = (data.p1IkumiBlood != s_lastP1IkumiBlood) || (data.p1IkumiGenocide != s_lastP1IkumiGenocide);
            auto now = std::chrono::steady_clock::now();
            bool heartbeat = (s_lastIkumiLogP1.time_since_epoch().count() == 0) || ((now - s_lastIkumiLogP1) >= IKUMI_LOG_HEARTBEAT);
            if (detailedLogging.load() && (changed || heartbeat)) {
                LogOut("[CHAR] Read P1 Ikumi values: Blood=" + std::to_string(data.p1IkumiBlood) +
                       ", Genocide=" + std::to_string(data.p1IkumiGenocide), true);
                s_lastIkumiLogP1 = now;
            }
            s_lastP1IkumiBlood = data.p1IkumiBlood;
            s_lastP1IkumiGenocide = data.p1IkumiGenocide;
        }
        
        if (data.p2CharID == CHAR_ID_IKUMI) {
            uintptr_t bloodAddr = ResolvePointer(base, EFZ_BASE_OFFSET_P2, IKUMI_BLOOD_OFFSET);
            uintptr_t genocideAddr = ResolvePointer(base, EFZ_BASE_OFFSET_P2, IKUMI_GENOCIDE_OFFSET);
            
            if (bloodAddr) SafeReadMemory(bloodAddr, &data.p2IkumiBlood, sizeof(int));
            if (genocideAddr) SafeReadMemory(genocideAddr, &data.p2IkumiGenocide, sizeof(int));
            // Change-only logging with periodic heartbeat
            bool changed = (data.p2IkumiBlood != s_lastP2IkumiBlood) || (data.p2IkumiGenocide != s_lastP2IkumiGenocide);
            auto now = std::chrono::steady_clock::now();
            bool heartbeat = (s_lastIkumiLogP2.time_since_epoch().count() == 0) || ((now - s_lastIkumiLogP2) >= IKUMI_LOG_HEARTBEAT);
            if (detailedLogging.load() && (changed || heartbeat)) {
                LogOut("[CHAR] Read P2 Ikumi values: Blood=" + std::to_string(data.p2IkumiBlood) +
                       ", Genocide=" + std::to_string(data.p2IkumiGenocide), true);
                s_lastIkumiLogP2 = now;
            }
            s_lastP2IkumiBlood = data.p2IkumiBlood;
            s_lastP2IkumiGenocide = data.p2IkumiGenocide;
        }

        // Read Neyuki (Sleepy Nayuki) jam count (0..9)
        auto ReadNeyuki = [&](int playerIndex){
            const int off = (playerIndex==1)?EFZ_BASE_OFFSET_P1:EFZ_BASE_OFFSET_P2;
            uintptr_t jamAddr = ResolvePointer(base, off, NEYUKI_JAM_COUNT_OFFSET);
            if (!jamAddr) return;
            int jam = 0; SafeReadMemory(jamAddr, &jam, sizeof(int));
            if (jam < 0) jam = 0; else if (jam > NEYUKI_JAM_COUNT_MAX) jam = NEYUKI_JAM_COUNT_MAX;
            if (playerIndex==1) data.p1NeyukiJamCount = jam; else data.p2NeyukiJamCount = jam;
            LogOut(std::string("[CHAR] Read ") + (playerIndex==1?"P1":"P2") + " Neyuki: JamCount=" + std::to_string(jam), detailedLogging.load());
        };
        if (data.p1CharID == CHAR_ID_NAYUKI) ReadNeyuki(1);
        if (data.p2CharID == CHAR_ID_NAYUKI) ReadNeyuki(2);
        
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
            // Change-only logging with periodic heartbeat
            auto now = std::chrono::steady_clock::now();
            bool changed = false;
            bool heartbeat = false;
            if (playerIndex == 1) {
                changed = (s_lastRumiModeP1 != (int)mode) || (s_lastRumiGateP1 != (int)gate);
                heartbeat = (s_lastRumiLogP1.time_since_epoch().count() == 0) || ((now - s_lastRumiLogP1) >= RUMI_LOG_HEARTBEAT);
                if (detailedLogging.load() && (changed || heartbeat)) {
                    LogOut(std::string("[CHAR] Read Rumi state P1: mode=") + std::to_string((int)mode) + ", gate=" + std::to_string((int)gate) +
                           ", KimchiActive=" + std::to_string((int)data.p1RumiKimchiActive) + ", KimchiTimer=" + std::to_string((int)data.p1RumiKimchiTimer), true);
                    s_lastRumiLogP1 = now;
                }
                s_lastRumiModeP1 = (int)mode; s_lastRumiGateP1 = (int)gate;
            } else {
                changed = (s_lastRumiModeP2 != (int)mode) || (s_lastRumiGateP2 != (int)gate);
                heartbeat = (s_lastRumiLogP2.time_since_epoch().count() == 0) || ((now - s_lastRumiLogP2) >= RUMI_LOG_HEARTBEAT);
                if (detailedLogging.load() && (changed || heartbeat)) {
                    LogOut(std::string("[CHAR] Read Rumi state P2: mode=") + std::to_string((int)mode) + ", gate=" + std::to_string((int)gate) +
                           ", KimchiActive=" + std::to_string((int)data.p2RumiKimchiActive) + ", KimchiTimer=" + std::to_string((int)data.p2RumiKimchiTimer), true);
                    s_lastRumiLogP2 = now;
                }
                s_lastRumiModeP2 = (int)mode; s_lastRumiGateP2 = (int)gate;
            }
        };

        if (data.p1CharID == CHAR_ID_NANASE) {
            ReadRumiState(1);
        }
        if (data.p2CharID == CHAR_ID_NANASE) {
            ReadRumiState(2);
        }

        // Akiko (Minase) – bullet cycle and timeslow trigger
        // If switching to Akiko, reset the timeslow trigger to Inactive to avoid stale "Infinite" carry-over
        bool p1JustSwitchedToAkiko = (data.p1CharID == CHAR_ID_AKIKO && s_prevCharIDP1 != CHAR_ID_AKIKO);
        bool p2JustSwitchedToAkiko = (data.p2CharID == CHAR_ID_AKIKO && s_prevCharIDP2 != CHAR_ID_AKIKO);
        auto ReadAkiko = [&](int playerIndex){
            const int off = (playerIndex==1)?EFZ_BASE_OFFSET_P1:EFZ_BASE_OFFSET_P2;
            uintptr_t bulletAddr = ResolvePointer(base, off, AKIKO_BULLET_CYCLE_OFFSET);
            uintptr_t timeAddr   = ResolvePointer(base, off, AKIKO_TIMESLOW_TRIGGER_OFFSET);
            int bullet=0, t=0; if (bulletAddr) SafeReadMemory(bulletAddr,&bullet,sizeof(int)); if (timeAddr) SafeReadMemory(timeAddr,&t,sizeof(int));
            // Clamp bullet cycle to valid range [0..2]; timeslow is [0..4]
            bullet = CLAMP(bullet, 0, 2);
            t = CLAMP(t, AKIKO_TIMESLOW_INACTIVE, AKIKO_TIMESLOW_INFINITE);
            // On first switch to Akiko, prefer Inactive regardless of what's in memory
            if ((playerIndex==1 && p1JustSwitchedToAkiko) || (playerIndex==2 && p2JustSwitchedToAkiko)) {
                t = AKIKO_TIMESLOW_INACTIVE;
            }
            auto now = std::chrono::steady_clock::now();
            if (playerIndex==1) {
                data.p1AkikoBulletCycle = bullet; data.p1AkikoTimeslowTrigger = t;
                bool changed = (s_lastAkikoBulletP1!=bullet)||(s_lastAkikoTimeP1!=t);
                bool heartbeat = (s_lastAkikoLogP1.time_since_epoch().count()==0)||((now - s_lastAkikoLogP1) >= AKIKO_LOG_HEARTBEAT);
                if (detailedLogging.load() && (changed || heartbeat)) {
                    LogOut("[CHAR] Read P1 Akiko: BulletCycle=" + std::to_string(bullet) + ", TimeSlow=" + std::to_string(t), true);
                    s_lastAkikoLogP1 = now;
                }
                s_lastAkikoBulletP1=bullet; s_lastAkikoTimeP1=t;
            } else {
                data.p2AkikoBulletCycle = bullet; data.p2AkikoTimeslowTrigger = t;
                bool changed = (s_lastAkikoBulletP2!=bullet)||(s_lastAkikoTimeP2!=t);
                bool heartbeat = (s_lastAkikoLogP2.time_since_epoch().count()==0)||((now - s_lastAkikoLogP2) >= AKIKO_LOG_HEARTBEAT);
                if (detailedLogging.load() && (changed || heartbeat)) {
                    LogOut("[CHAR] Read P2 Akiko: BulletCycle=" + std::to_string(bullet) + ", TimeSlow=" + std::to_string(t), true);
                    s_lastAkikoLogP2 = now;
                }
                s_lastAkikoBulletP2=bullet; s_lastAkikoTimeP2=t;
            }
        };
        if (data.p1CharID == CHAR_ID_AKIKO) ReadAkiko(1);
        if (data.p2CharID == CHAR_ID_AKIKO) ReadAkiko(2);

        // Update previous character IDs at the end of read
        s_prevCharIDP1 = data.p1CharID;
        s_prevCharIDP2 = data.p2CharID;
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
        
        // No background threads anymore; enforcement happens inline via TickCharacterEnforcements()

        // Apply Akiko values directly when set in DisplayData
        auto ApplyAkiko = [&](int pi){
            if ((pi==1 && data.p1CharID != CHAR_ID_AKIKO) || (pi==2 && data.p2CharID != CHAR_ID_AKIKO)) return;
            const int off = (pi==1)?EFZ_BASE_OFFSET_P1:EFZ_BASE_OFFSET_P2;
            uintptr_t bulletAddr = ResolvePointer(base, off, AKIKO_BULLET_CYCLE_OFFSET);
            uintptr_t timeAddr   = ResolvePointer(base, off, AKIKO_TIMESLOW_TRIGGER_OFFSET);
            int bullet = (pi==1)?data.p1AkikoBulletCycle:data.p2AkikoBulletCycle;
            int t      = (pi==1)?data.p1AkikoTimeslowTrigger:data.p2AkikoTimeslowTrigger;
            bullet = CLAMP(bullet, 0, 2);
            t = CLAMP(t, AKIKO_TIMESLOW_INACTIVE, AKIKO_TIMESLOW_INFINITE);
            // If user explicitly chose Inactive, force write Inactive (0) to clear any Infinite flag lingering
            if (t == AKIKO_TIMESLOW_INACTIVE) {
                if (timeAddr) SafeWriteMemory(timeAddr, &t, sizeof(int));
            }
            if (bulletAddr) SafeWriteMemory(bulletAddr,&bullet,sizeof(int));
            if (timeAddr)   SafeWriteMemory(timeAddr,&t,sizeof(int));
            LogOut(std::string("[CHAR] Applied ") + (pi==1?"P1":"P2") + " Akiko: BulletCycle=" + std::to_string(bullet) + ", TimeSlow=" + std::to_string(t), detailedLogging.load());
        }; ApplyAkiko(1); ApplyAkiko(2);

        // Apply Neyuki jam count if present
        auto ApplyNeyuki = [&](int pi){
            if ((pi==1 && data.p1CharID != CHAR_ID_NAYUKI) || (pi==2 && data.p2CharID != CHAR_ID_NAYUKI)) return;
            const int off = (pi==1)?EFZ_BASE_OFFSET_P1:EFZ_BASE_OFFSET_P2;
            if (uintptr_t jamAddr = ResolvePointer(base, off, NEYUKI_JAM_COUNT_OFFSET)) {
                int jam = (pi==1)? data.p1NeyukiJamCount : data.p2NeyukiJamCount;
                if (jam < 0) jam = 0; else if (jam > NEYUKI_JAM_COUNT_MAX) jam = NEYUKI_JAM_COUNT_MAX;
                SafeWriteMemory(jamAddr, &jam, sizeof(int));
                LogOut(std::string("[CHAR] Applied ") + (pi==1?"P1":"P2") + " Neyuki: JamCount=" + std::to_string(jam), detailedLogging.load());
            }
        }; ApplyNeyuki(1); ApplyNeyuki(2);
    }
    
    // Track previous values (used by inline enforcement)
    static int p1LastFeatherCount = 0;
    static int p2LastFeatherCount = 0;
    static int p1LastMishioElem = -1;
    static int p2LastMishioElem = -1;

    // Inline per-tick enforcement (call at low cadence from FrameDataMonitor)
    void TickCharacterEnforcements(uintptr_t base, const DisplayData& localData) {
        if (!base) return;
        if (!g_featuresEnabled.load()) return;
        if (g_onlineModeActive.load()) return; // never enforcements online
        if (GetCurrentGameMode() != GameMode::Practice) return;

        bool didWriteThisTick = false;

        // Ikumi genocide timer keeper
        if (localData.infiniteBloodMode) {
            if (localData.p1CharID == CHAR_ID_IKUMI) {
                if (auto addr = ResolvePointer(base, EFZ_BASE_OFFSET_P1, IKUMI_GENOCIDE_OFFSET)) {
                    int cur=0; SafeReadMemory(addr, &cur, sizeof(int));
                    const int target = IKUMI_GENOCIDE_MAX; if (cur < target) { SafeWriteMemory(addr, &target, sizeof(int)); didWriteThisTick = true; }
                }
            }
            if (localData.p2CharID == CHAR_ID_IKUMI) {
                if (auto addr = ResolvePointer(base, EFZ_BASE_OFFSET_P2, IKUMI_GENOCIDE_OFFSET)) {
                    int cur=0; SafeReadMemory(addr, &cur, sizeof(int));
                    const int target = IKUMI_GENOCIDE_MAX; if (cur < target) { SafeWriteMemory(addr, &target, sizeof(int)); didWriteThisTick = true; }
                }
            }
        }

        // Mishio element freeze
        if (localData.infiniteMishioElement) {
            if (localData.p1CharID == CHAR_ID_MISHIO) {
                if (auto addr = ResolvePointer(base, EFZ_BASE_OFFSET_P1, MISHIO_ELEMENT_OFFSET)) {
                    int cur=0; SafeReadMemory(addr, &cur, sizeof(int));
                    int target = CLAMP(localData.p1MishioElement, MISHIO_ELEM_NONE, MISHIO_ELEM_AWAKENED);
                    if (p1LastMishioElem == -1) p1LastMishioElem = cur;
                    if (cur != target) { SafeWriteMemory(addr, &target, sizeof(int)); p1LastMishioElem = target; didWriteThisTick = true; }
                }
            } else { p1LastMishioElem = -1; }
            if (localData.p2CharID == CHAR_ID_MISHIO) {
                if (auto addr = ResolvePointer(base, EFZ_BASE_OFFSET_P2, MISHIO_ELEMENT_OFFSET)) {
                    int cur=0; SafeReadMemory(addr, &cur, sizeof(int));
                    int target = CLAMP(localData.p2MishioElement, MISHIO_ELEM_NONE, MISHIO_ELEM_AWAKENED);
                    if (p2LastMishioElem == -1) p2LastMishioElem = cur;
                    if (cur != target) { SafeWriteMemory(addr, &target, sizeof(int)); p2LastMishioElem = target; didWriteThisTick = true; }
                }
            } else { p2LastMishioElem = -1; }
        } else { p1LastMishioElem = -1; p2LastMishioElem = -1; }

        // Mishio awakened timer top-up (only when awakened)
        if (localData.infiniteMishioAwakened) {
            auto tickAw = [&](int pi){
                const int off = (pi==1)?EFZ_BASE_OFFSET_P1:EFZ_BASE_OFFSET_P2;
                auto e = ResolvePointer(base, off, MISHIO_ELEMENT_OFFSET);
                auto t = ResolvePointer(base, off, MISHIO_AWAKENED_TIMER_OFFSET);
                if (!(e && t)) return;
                int elem=0, cur=0; SafeReadMemory(e,&elem,sizeof(int)); SafeReadMemory(t,&cur,sizeof(int));
                if (elem == MISHIO_ELEM_AWAKENED) { int tgt = (pi==1)?localData.p1MishioAwakenedTimer:localData.p2MishioAwakenedTimer; if (tgt < MISHIO_AWAKENED_TARGET) tgt = MISHIO_AWAKENED_TARGET; if (tgt > MISHIO_AWAKENED_TARGET) tgt = MISHIO_AWAKENED_TARGET; if (cur < tgt) { SafeWriteMemory(t,&tgt,sizeof(int)); didWriteThisTick = true; } }
            };
            tickAw(1); tickAw(2);
        }

        // Misuzu feather freeze
        if (localData.infiniteFeatherMode) {
            auto keepFeathers = [&](int pi){
                if ((pi==1 && localData.p1CharID != CHAR_ID_MISUZU) || (pi==2 && localData.p2CharID != CHAR_ID_MISUZU)) return;
                const int off = (pi==1)?EFZ_BASE_OFFSET_P1:EFZ_BASE_OFFSET_P2;
                if (auto addr = ResolvePointer(base, off, MISUZU_FEATHER_OFFSET)) {
                    int cur=0; SafeReadMemory(addr,&cur,sizeof(int));
                    int &last = (pi==1)?p1LastFeatherCount:p2LastFeatherCount;
                    if (last == 0) last = cur; // initialize
                    if (cur < last) { SafeWriteMemory(addr,&last,sizeof(int)); didWriteThisTick = true; }
                    else if (cur > last) { last = cur; }
                }
            }; keepFeathers(1); keepFeathers(2);
        } else { p1LastFeatherCount = 0; p2LastFeatherCount = 0; }

        // IC color override (Blue IC = 1)
        auto enforceIC = [&](int pi){
            bool wantBlue = (pi==1)?localData.p1BlueIC:localData.p2BlueIC; if (!wantBlue) return;
            const int off = (pi==1)?EFZ_BASE_OFFSET_P1:EFZ_BASE_OFFSET_P2;
            if (auto addr = ResolvePointer(base, off, IC_COLOR_OFFSET)) { int cur=0; SafeReadMemory(addr,&cur,sizeof(int)); if (cur != 1) { int v=1; SafeWriteMemory(addr,&v,sizeof(int)); didWriteThisTick = true; } }
        }; enforceIC(1); enforceIC(2);

        // Rumi Infinite Shinai (keep gate=0 and restore Shinai mode when safe)
        static int p1RestoreDelay = 0, p2RestoreDelay = 0;
        auto enforceRumi = [&](int pi){
            bool wantInf = (pi==1)?(localData.p1RumiInfiniteShinai && localData.p1CharID==CHAR_ID_NANASE)
                                  :(localData.p2RumiInfiniteShinai && localData.p2CharID==CHAR_ID_NANASE);
            if (!wantInf) return;
            const int off = (pi==1)?EFZ_BASE_OFFSET_P1:EFZ_BASE_OFFSET_P2;
            auto modeAddr = ResolvePointer(base, off, RUMI_MODE_BYTE_OFFSET);
            auto gateAddr = ResolvePointer(base, off, RUMI_WEAPON_GATE_OFFSET);
            if (!(modeAddr && gateAddr)) return;
            uint8_t curMode=0, curGate=0; SafeReadMemory(modeAddr,&curMode,sizeof(uint8_t)); SafeReadMemory(gateAddr,&curGate,sizeof(uint8_t));
            if (curGate != 0) { uint8_t z=0; SafeWriteMemory(gateAddr,&z,sizeof(uint8_t)); didWriteThisTick = true; }
            short mv=0; if (auto mvAddr = ResolvePointer(base, off, MOVE_ID_OFFSET)) SafeReadMemory(mvAddr,&mv,sizeof(short));
            bool inTossSuper = (mv == RUMI_SUPER_TOSS_A || mv == RUMI_SUPER_TOSS_B || mv == RUMI_SUPER_TOSS_C);
            int &delayRef = (pi==1)?p1RestoreDelay:p2RestoreDelay;
            if (inTossSuper) { delayRef = 60; return; }
            if (delayRef > 0) { delayRef--; return; }
            if (curMode != 0 && IsActionable(mv)) {
                using ToggleModeFn = int(__fastcall*)(uintptr_t, int, char);
                uintptr_t gameBase = GetEFZBase();
                ToggleModeFn ToggleCharacterMode = reinterpret_cast<ToggleModeFn>(gameBase + TOGGLE_CHARACTER_MODE_RVA);
                uintptr_t playerThis = 0; SafeReadMemory(gameBase + ((pi==1)?EFZ_BASE_OFFSET_P1:EFZ_BASE_OFFSET_P2), &playerThis, sizeof(uintptr_t));
                if (!playerThis) return; ToggleCharacterMode(playerThis, 0, 0); uint8_t z2=0; SafeWriteMemory(gateAddr,&z2,sizeof(uint8_t)); didWriteThisTick = true;
            }
        }; if (AreCharactersInitialized()) { enforceRumi(1); enforceRumi(2); }

        // Rumi Kimchi infinite: keep timer topped and flag active
        auto enforceKimchi = [&](int pi){
            bool isRumi = (pi==1)?(localData.p1CharID==CHAR_ID_NANASE):(localData.p2CharID==CHAR_ID_NANASE);
            if (!isRumi) return; bool wantInf = (pi==1)?localData.p1RumiInfiniteKimchi:localData.p2RumiInfiniteKimchi; if (!wantInf) return;
            const int off = (pi==1)?EFZ_BASE_OFFSET_P1:EFZ_BASE_OFFSET_P2; auto flag = ResolvePointer(base, off, RUMI_KIMCHI_ACTIVE_OFFSET); auto tim = ResolvePointer(base, off, RUMI_KIMCHI_TIMER_OFFSET); if (!tim) return;
            int curT=0; SafeReadMemory(tim,&curT,sizeof(int)); int tgt = RUMI_KIMCHI_TARGET; if (curT < tgt) { SafeWriteMemory(tim,&tgt,sizeof(int)); didWriteThisTick = true; }
            if (flag) { int curF=0; SafeReadMemory(flag,&curF,sizeof(int)); if (!curF) { int one=1; SafeWriteMemory(flag,&one,sizeof(int)); didWriteThisTick = true; } }
        }; enforceKimchi(1); enforceKimchi(2);

        (void)didWriteThisTick; // reserved for future backoff tuning/logging

        // Akiko: freeze bullet cycle when requested, and enforce infinite timeslow when set
        auto enforceAkikoCycle = [&](int pi){
            bool isAkiko = (pi==1)?(localData.p1CharID==CHAR_ID_AKIKO):(localData.p2CharID==CHAR_ID_AKIKO);
            if (!isAkiko) return;
            bool wantFreeze = (pi==1)?localData.p1AkikoFreezeCycle:localData.p2AkikoFreezeCycle;
            if (!wantFreeze) return;
            const int off = (pi==1)?EFZ_BASE_OFFSET_P1:EFZ_BASE_OFFSET_P2;
            auto cycAddr = ResolvePointer(base, off, AKIKO_BULLET_CYCLE_OFFSET);
            if (!cycAddr) return;
            int cur=0; SafeReadMemory(cycAddr,&cur,sizeof(int));
            int target = CLAMP((pi==1)?localData.p1AkikoBulletCycle:localData.p2AkikoBulletCycle, 0, 2);
            if (cur != target) { SafeWriteMemory(cycAddr,&target,sizeof(int)); didWriteThisTick = true; }
        }; enforceAkikoCycle(1); enforceAkikoCycle(2);

        // Akiko: enforce infinite timeslow when set to AKIKO_TIMESLOW_INFINITE (4)
        auto enforceAkiko = [&](int pi){
            if ((pi==1 && localData.p1CharID != CHAR_ID_AKIKO) || (pi==2 && localData.p2CharID != CHAR_ID_AKIKO)) return;
            const int off = (pi==1)?EFZ_BASE_OFFSET_P1:EFZ_BASE_OFFSET_P2;
            // Freeze bullet cycle: capture once on rising edge, then restore only if diverged
            bool keepCycle = (pi==1)?localData.p1AkikoFreezeCycle:localData.p2AkikoFreezeCycle;
            bool &prevFreeze = (pi==1)?s_prevP1AkikoFreeze:s_prevP2AkikoFreeze;
            int  &frozenVal  = (pi==1)?s_p1AkikoFrozenCycle:s_p2AkikoFrozenCycle;
            if (keepCycle && !prevFreeze) {
                if (auto bAddr = ResolvePointer(base, off, AKIKO_BULLET_CYCLE_OFFSET)) {
                    int cur=0; SafeReadMemory(bAddr,&cur,sizeof(int)); frozenVal = CLAMP(cur, 0, 2);
                } else {
                    // Fallback to GUI value if address missing
                    frozenVal = CLAMP((pi==1)?localData.p1AkikoBulletCycle:localData.p2AkikoBulletCycle, 0, 2);
                }
            }
            prevFreeze = keepCycle;
            if (keepCycle) {
                if (auto bAddr = ResolvePointer(base, off, AKIKO_BULLET_CYCLE_OFFSET)) {
                    int cur=0; SafeReadMemory(bAddr,&cur,sizeof(int));
                    if (cur != frozenVal) { SafeWriteMemory(bAddr,&frozenVal,sizeof(int)); }
                }
            }
            auto tAddr = ResolvePointer(base, off, AKIKO_TIMESLOW_TRIGGER_OFFSET);
            if (!tAddr) return; int cur=0; SafeReadMemory(tAddr,&cur,sizeof(int));
            int want = (pi==1)?localData.p1AkikoTimeslowTrigger:localData.p2AkikoTimeslowTrigger;
            // If GUI wants Infinite, enforce Infinite; otherwise ensure not Infinite (set Inactive)
            if (want == AKIKO_TIMESLOW_INFINITE) {
                if (cur != AKIKO_TIMESLOW_INFINITE) { int inf = AKIKO_TIMESLOW_INFINITE; SafeWriteMemory(tAddr,&inf,sizeof(int)); }
            } else {
                if (cur == AKIKO_TIMESLOW_INFINITE) { int z = AKIKO_TIMESLOW_INACTIVE; SafeWriteMemory(tAddr,&z,sizeof(int)); }
            }
        }; enforceAkiko(1); enforceAkiko(2);
    }
}