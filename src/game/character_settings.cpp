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
#include <sstream>

namespace CharacterSettings {
    // Forward declaration for GUI visibility flag (defined in ImGui layer)
    extern std::atomic<bool> g_guiVisible;
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
    
    // Cached per-player character-specific pointers to reduce ResolvePointer calls
    struct PlayerCharPointers {
        uintptr_t base = 0;
        int       charId = -1;
        // Ikumi
        uintptr_t ikumiBlood = 0;
        uintptr_t ikumiGenocide = 0;
        // Mishio
        uintptr_t mishioElement = 0;
        uintptr_t mishioAwakenedTimer = 0;
        // Misuzu
        uintptr_t misuzuFeather = 0;
        uintptr_t misuzuPoisonTimer = 0;
        uintptr_t misuzuPoisonLevel = 0;
        // Doppel / Rumi
        uintptr_t doppelEnlightened = 0;
        uintptr_t rumiModeByte = 0;
        uintptr_t rumiWeaponGate = 0;
        uintptr_t rumiKimchiFlag = 0;
        uintptr_t rumiKimchiTimer = 0;
        // Akiko
        uintptr_t akikoBulletCycle = 0;
        uintptr_t akikoTimeslowTrigger = 0;
        uintptr_t akikoDigitFirst = 0;
        uintptr_t akikoDigitSecond = 0;
        uintptr_t akikoDigitThird = 0;
        // Mio
        uintptr_t mioStance = 0;
        // Kano
        uintptr_t kanoMagic = 0;
        // Neyuki / NayukiB
        uintptr_t neyukiJamCount = 0;
        uintptr_t nayukiSnowbunnyTimer = 0;
        // Mai
        uintptr_t maiStatus = 0;
        uintptr_t maiMultiTimer = 0;
        uintptr_t maiSummonFlashFlag = 0;
        // Minagi puppet / Mai ghost share base pointer only (computed via base+off)
    };

    static PlayerCharPointers s_pointersP1;
    static PlayerCharPointers s_pointersP2;

    void InvalidateAllCharacterPointerCaches() {
        // Zero out all cached addresses so they get refreshed
        // on the next Read/Apply call. This is important when
        // re-entering Practice after character select, since
        // the underlying player objects are reallocated and
        // previously-resolved pointers can become stale.
        s_pointersP1 = PlayerCharPointers{};
        s_pointersP2 = PlayerCharPointers{};
        LogOut("[CHAR] Invalidating all character pointer caches", detailedLogging.load());
    }

    static void RefreshCharacterPointers(uintptr_t base, int playerIndex, int charId) {
        PlayerCharPointers &p = (playerIndex == 1) ? s_pointersP1 : s_pointersP2;
        const int off = (playerIndex == 1) ? EFZ_BASE_OFFSET_P1 : EFZ_BASE_OFFSET_P2;
        p.base = base + off;
        p.charId = charId;

        // Clear all first
        p.ikumiBlood = p.ikumiGenocide = 0;
        p.mishioElement = p.mishioAwakenedTimer = 0;
        p.misuzuFeather = p.misuzuPoisonTimer = p.misuzuPoisonLevel = 0;
        p.doppelEnlightened = 0;
        p.rumiModeByte = p.rumiWeaponGate = 0;
        p.rumiKimchiFlag = p.rumiKimchiTimer = 0;
        p.akikoBulletCycle = p.akikoTimeslowTrigger = 0;
        p.akikoDigitFirst = p.akikoDigitSecond = p.akikoDigitThird = 0;
        p.mioStance = 0;
        p.kanoMagic = 0;
        p.neyukiJamCount = 0;
        p.nayukiSnowbunnyTimer = 0;
        p.maiStatus = p.maiMultiTimer = p.maiSummonFlashFlag = 0;

        if (charId == CHAR_ID_IKUMI) {
            p.ikumiBlood    = ResolvePointer(base, off, IKUMI_BLOOD_OFFSET);
            p.ikumiGenocide = ResolvePointer(base, off, IKUMI_GENOCIDE_OFFSET);
        }
        if (charId == CHAR_ID_MISHIO) {
            p.mishioElement       = ResolvePointer(base, off, MISHIO_ELEMENT_OFFSET);
            p.mishioAwakenedTimer = ResolvePointer(base, off, MISHIO_AWAKENED_TIMER_OFFSET);
        }
        if (charId == CHAR_ID_MISUZU) {
            p.misuzuFeather     = ResolvePointer(base, off, MISUZU_FEATHER_OFFSET);
            p.misuzuPoisonTimer = ResolvePointer(base, off, MISUZU_POISON_TIMER_OFFSET);
            p.misuzuPoisonLevel = ResolvePointer(base, off, MISUZU_POISON_LEVEL_OFFSET);
        }
        if (charId == CHAR_ID_EXNANASE) {
            p.doppelEnlightened = ResolvePointer(base, off, DOPPEL_ENLIGHTENED_OFFSET);
        }
        if (charId == CHAR_ID_NANASE) {
            p.rumiModeByte    = ResolvePointer(base, off, RUMI_MODE_BYTE_OFFSET);
            p.rumiWeaponGate  = ResolvePointer(base, off, RUMI_WEAPON_GATE_OFFSET);
            p.rumiKimchiFlag  = ResolvePointer(base, off, RUMI_KIMCHI_ACTIVE_OFFSET);
            p.rumiKimchiTimer = ResolvePointer(base, off, RUMI_KIMCHI_TIMER_OFFSET);
        }
        if (charId == CHAR_ID_AKIKO) {
            p.akikoBulletCycle    = ResolvePointer(base, off, AKIKO_BULLET_CYCLE_OFFSET);
            p.akikoTimeslowTrigger= ResolvePointer(base, off, AKIKO_TIMESLOW_TRIGGER_OFFSET);
            p.akikoDigitFirst     = ResolvePointer(base, off, AKIKO_TIMESLOW_FIRST_OFFSET);
            p.akikoDigitSecond    = ResolvePointer(base, off, AKIKO_TIMESLOW_SECOND_OFFSET);
            p.akikoDigitThird     = ResolvePointer(base, off, AKIKO_TIMESLOW_THIRD_OFFSET);
        }
        if (charId == CHAR_ID_MIO) {
            p.mioStance = ResolvePointer(base, off, MIO_STANCE_OFFSET);
        }
        if (charId == CHAR_ID_KANO) {
            p.kanoMagic = ResolvePointer(base, off, KANO_MAGIC_OFFSET);
        }
        if (charId == CHAR_ID_NAYUKI) {
            p.neyukiJamCount = ResolvePointer(base, off, NEYUKI_JAM_COUNT_OFFSET);
        }
        if (charId == CHAR_ID_NAYUKIB) {
            p.nayukiSnowbunnyTimer = ResolvePointer(base, off, NAYUKIB_SNOWBUNNY_TIMER_OFFSET);
        }
        if (charId == CHAR_ID_MAI) {
            p.maiStatus         = ResolvePointer(base, off, MAI_STATUS_OFFSET);
            p.maiMultiTimer     = ResolvePointer(base, off, MAI_MULTI_TIMER_OFFSET);
            p.maiSummonFlashFlag= ResolvePointer(base, off, MAI_SUMMON_FLASH_FLAG_OFFSET);
        }
    }

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
        int oldP1 = data.p1CharID;
        int oldP2 = data.p2CharID;
        data.p1CharID = GetCharacterID(data.p1CharName);
        data.p2CharID = GetCharacterID(data.p2CharName);
        
        LogOut("[CHAR] Updated character IDs - P1: " + std::string(data.p1CharName) + 
               " (ID: " + std::to_string(data.p1CharID) + "), P2: " + 
               std::string(data.p2CharName) + " (ID: " + std::to_string(data.p2CharID) + ")",
               detailedLogging.load());

        // Refresh cached pointers when IDs change; base will be patched on first Read/Apply call
        if (oldP1 != data.p1CharID || oldP2 != data.p2CharID) {
            uintptr_t base = GetEFZBase();
            if (base) {
                RefreshCharacterPointers(base, 1, data.p1CharID);
                RefreshCharacterPointers(base, 2, data.p2CharID);
            }
        }
    }
    
    // Helper to check if any background infinite/lock options are active
    static bool AnyInfiniteOrLockEnabled(const DisplayData& d) {
        return d.infiniteBloodMode ||
               d.infiniteMishioElement || d.infiniteMishioAwakened ||
               d.infiniteFeatherMode ||
               d.p1MisuzuInfinitePoison || d.p2MisuzuInfinitePoison ||
               d.p1RumiInfiniteShinai || d.p2RumiInfiniteShinai ||
               d.p1RumiInfiniteKimchi || d.p2RumiInfiniteKimchi ||
               d.p1AkikoInfiniteTimeslow || d.p2AkikoInfiniteTimeslow ||
               d.p1AkikoFreezeCycle || d.p2AkikoFreezeCycle ||
               d.p1MioLockStance || d.p2MioLockStance ||
               d.p1KanoLockMagic || d.p2KanoLockMagic ||
               d.p1NayukiInfiniteSnow || d.p2NayukiInfiniteSnow ||
               d.p1MaiInfiniteGhost || d.p2MaiInfiniteGhost ||
               d.p1MaiInfiniteCharge || d.p2MaiInfiniteCharge ||
               d.p1MaiInfiniteAwakening || d.p2MaiInfiniteAwakening ||
               d.p1MaiNoChargeCD || d.p2MaiNoChargeCD ||
               d.p1MinagiAlwaysReadied || d.p2MinagiAlwaysReadied ||
               d.minagiConvertNewProjectiles;
    }

    static std::string PtrHex(uintptr_t v) {
        char buf[32];
        _snprintf_s(buf, sizeof(buf), _TRUNCATE, "0x%p", (void*)v);
        return std::string(buf);
    }

    void ReadCharacterValues(uintptr_t base, DisplayData& data) {
        // Ensure pointer caches have correct base for this session
        if (base) {
            if (s_pointersP1.base == 0 || s_pointersP1.base != base + EFZ_BASE_OFFSET_P1) {
                  LogOut("[CHAR][READ] P1 pointer cache invalid or base changed (old=" +
                      PtrHex(s_pointersP1.base) + ", new=" + PtrHex(base + EFZ_BASE_OFFSET_P1) + ")",
                       detailedLogging.load());
                RefreshCharacterPointers(base, 1, data.p1CharID);
            }
            if (s_pointersP2.base == 0 || s_pointersP2.base != base + EFZ_BASE_OFFSET_P2) {
                  LogOut("[CHAR][READ] P2 pointer cache invalid or base changed (old=" +
                      PtrHex(s_pointersP2.base) + ", new=" + PtrHex(base + EFZ_BASE_OFFSET_P2) + ")",
                       detailedLogging.load());
                RefreshCharacterPointers(base, 2, data.p2CharID);
            }
        } else {
            LogOut("[CHAR][READ] ReadCharacterValues called with base=0; skipping character reads", detailedLogging.load());
        }

        // If GUI is hidden and no infinite/lock option needs these values, we can skip reads entirely.
        // Log this skip only once per "GUI-hidden" period to avoid spamming.
        static bool s_loggedSkipOnce = false;
        if (!g_guiVisible.load()) {
            if (!AnyInfiniteOrLockEnabled(data)) {
                if (!s_loggedSkipOnce) {
                    LogOut("[CHAR][READ] Skipping character reads (GUI hidden, no infinites/locks)", detailedLogging.load());
                    s_loggedSkipOnce = true;
                }
                return;
            }
        } else {
            // Reset one-shot skip log when GUI becomes visible again
            s_loggedSkipOnce = false;
        }
        // Read Ikumi's values if either player is using her
        if (data.p1CharID == CHAR_ID_IKUMI) {
            uintptr_t levelAddr    = ResolvePointer(base, EFZ_BASE_OFFSET_P1, IKUMI_LEVEL_GAUGE_OFFSET);
            // NOTE: Resolve blood/genocide dynamically each read to avoid stale cached
            // addresses across character-select / re-entry transitions.
            uintptr_t bloodAddr    = ResolvePointer(base, EFZ_BASE_OFFSET_P1, IKUMI_BLOOD_OFFSET);
            uintptr_t genocideAddr = ResolvePointer(base, EFZ_BASE_OFFSET_P1, IKUMI_GENOCIDE_OFFSET);

            if (levelAddr)   { SafeReadMemory(levelAddr,   &data.p1IkumiLevelGauge, sizeof(int)); data.p1IkumiLevelGauge = CLAMP(data.p1IkumiLevelGauge, 0, 99); }
            if (bloodAddr)   SafeReadMemory(bloodAddr,   &data.p1IkumiBlood, sizeof(int));
            if (genocideAddr)SafeReadMemory(genocideAddr,&data.p1IkumiGenocide, sizeof(int));
            // Change-only logging with periodic heartbeat
            bool changed = (data.p1IkumiBlood != s_lastP1IkumiBlood) || (data.p1IkumiGenocide != s_lastP1IkumiGenocide);
            auto now = std::chrono::steady_clock::now();
            bool heartbeat = (s_lastIkumiLogP1.time_since_epoch().count() == 0) || ((now - s_lastIkumiLogP1) >= IKUMI_LOG_HEARTBEAT);
            if (detailedLogging.load() && (changed || heartbeat)) {
                LogOut(std::string("[CHAR][READ][IKUMI] P1 ") +
                       "LvlAddr=" + PtrHex(levelAddr) +
                       " BloodAddr=" + PtrHex(bloodAddr) +
                       " GenocideAddr=" + PtrHex(genocideAddr) +
                       " Lvl=" + std::to_string(data.p1IkumiLevelGauge) +
                       " Blood=" + std::to_string(data.p1IkumiBlood) +
                       " Genocide=" + std::to_string(data.p1IkumiGenocide),
                       true);
                s_lastIkumiLogP1 = now;
            }
            s_lastP1IkumiBlood = data.p1IkumiBlood;
            s_lastP1IkumiGenocide = data.p1IkumiGenocide;
        }
        
        if (data.p2CharID == CHAR_ID_IKUMI) {
            uintptr_t levelAddr    = ResolvePointer(base, EFZ_BASE_OFFSET_P2, IKUMI_LEVEL_GAUGE_OFFSET);
            uintptr_t bloodAddr    = ResolvePointer(base, EFZ_BASE_OFFSET_P2, IKUMI_BLOOD_OFFSET);
            uintptr_t genocideAddr = ResolvePointer(base, EFZ_BASE_OFFSET_P2, IKUMI_GENOCIDE_OFFSET);

            if (levelAddr)   { SafeReadMemory(levelAddr,   &data.p2IkumiLevelGauge, sizeof(int)); data.p2IkumiLevelGauge = CLAMP(data.p2IkumiLevelGauge, 0, 99); }
            if (bloodAddr)   SafeReadMemory(bloodAddr,   &data.p2IkumiBlood, sizeof(int));
            if (genocideAddr)SafeReadMemory(genocideAddr,&data.p2IkumiGenocide, sizeof(int));
            // Change-only logging with periodic heartbeat
            bool changed = (data.p2IkumiBlood != s_lastP2IkumiBlood) || (data.p2IkumiGenocide != s_lastP2IkumiGenocide);
            auto now = std::chrono::steady_clock::now();
            bool heartbeat = (s_lastIkumiLogP2.time_since_epoch().count() == 0) || ((now - s_lastIkumiLogP2) >= IKUMI_LOG_HEARTBEAT);
            if (detailedLogging.load() && (changed || heartbeat)) {
                LogOut(std::string("[CHAR][READ][IKUMI] P2 ") +
                       "LvlAddr=" + PtrHex(levelAddr) +
                       " BloodAddr=" + PtrHex(bloodAddr) +
                       " GenocideAddr=" + PtrHex(genocideAddr) +
                       " Lvl=" + std::to_string(data.p2IkumiLevelGauge) +
                       " Blood=" + std::to_string(data.p2IkumiBlood) +
                       " Genocide=" + std::to_string(data.p2IkumiGenocide),
                       true);
                s_lastIkumiLogP2 = now;
            }
            s_lastP2IkumiBlood = data.p2IkumiBlood;
            s_lastP2IkumiGenocide = data.p2IkumiGenocide;
        }

        // Read Neyuki (Sleepy Nayuki) jam count (0..9)
        auto ReadNeyuki = [&](int playerIndex){
            PlayerCharPointers &p = (playerIndex==1)?s_pointersP1:s_pointersP2;
            const int off = (playerIndex==1)?EFZ_BASE_OFFSET_P1:EFZ_BASE_OFFSET_P2;
            uintptr_t jamAddr = p.neyukiJamCount;
            // Lazily resolve Neyuki jam pointer if cache was cleared or unresolved.
            if (base && !jamAddr) {
                jamAddr = p.neyukiJamCount = ResolvePointer(base, off, NEYUKI_JAM_COUNT_OFFSET);
            }
            if (!jamAddr) return;
                 int jam = 0; SafeReadMemory(jamAddr, &jam, sizeof(int));
            if (jam < 0) jam = 0; else if (jam > NEYUKI_JAM_COUNT_MAX) jam = NEYUKI_JAM_COUNT_MAX;
            if (playerIndex==1) data.p1NeyukiJamCount = jam; else data.p2NeyukiJamCount = jam;
                 LogOut(std::string("[CHAR][READ][NEYUKI] ") + (playerIndex==1?"P1":"P2") +
                     " addr=" + PtrHex(jamAddr) +
                     " JamCount=" + std::to_string(jam),
                     detailedLogging.load());
        };
        if (data.p1CharID == CHAR_ID_NAYUKI) ReadNeyuki(1);
        if (data.p2CharID == CHAR_ID_NAYUKI) ReadNeyuki(2);
        
        // Read Mishio's values if either player is using her
        if (data.p1CharID == CHAR_ID_MISHIO) {
                 uintptr_t elemAddr = s_pointersP1.mishioElement;
                 uintptr_t awAddr   = s_pointersP1.mishioAwakenedTimer;
            // Lazily resolve Mishio pointers if cache was cleared or unresolved.
            if (base) {
                if (!elemAddr) elemAddr = s_pointersP1.mishioElement       = ResolvePointer(base, EFZ_BASE_OFFSET_P1, MISHIO_ELEMENT_OFFSET);
                if (!awAddr)   awAddr   = s_pointersP1.mishioAwakenedTimer = ResolvePointer(base, EFZ_BASE_OFFSET_P1, MISHIO_AWAKENED_TIMER_OFFSET);
            }
            if (elemAddr) SafeReadMemory(elemAddr, &data.p1MishioElement, sizeof(int));
            if (awAddr)   SafeReadMemory(awAddr,   &data.p1MishioAwakenedTimer, sizeof(int));
                 LogOut(std::string("[CHAR][READ][MISHIO] P1 elemAddr=") + PtrHex(elemAddr) +
                     " awAddr=" + PtrHex(awAddr) +
                     " Element=" + std::to_string(data.p1MishioElement) +
                     " AwTimer=" + std::to_string(data.p1MishioAwakenedTimer),
                     detailedLogging.load());
        }
        if (data.p2CharID == CHAR_ID_MISHIO) {
            uintptr_t elemAddr = s_pointersP2.mishioElement;
            uintptr_t awAddr   = s_pointersP2.mishioAwakenedTimer;
            if (base) {
                if (!elemAddr) elemAddr = s_pointersP2.mishioElement       = ResolvePointer(base, EFZ_BASE_OFFSET_P2, MISHIO_ELEMENT_OFFSET);
                if (!awAddr)   awAddr   = s_pointersP2.mishioAwakenedTimer = ResolvePointer(base, EFZ_BASE_OFFSET_P2, MISHIO_AWAKENED_TIMER_OFFSET);
            }
            if (elemAddr) SafeReadMemory(elemAddr, &data.p2MishioElement, sizeof(int));
            if (awAddr)   SafeReadMemory(awAddr,   &data.p2MishioAwakenedTimer, sizeof(int));
                 LogOut(std::string("[CHAR][READ][MISHIO] P2 elemAddr=") + PtrHex(elemAddr) +
                     " awAddr=" + PtrHex(awAddr) +
                     " Element=" + std::to_string(data.p2MishioElement) +
                     " AwTimer=" + std::to_string(data.p2MishioAwakenedTimer),
                     detailedLogging.load());
        }

        // Read Misuzu's values if either player is using her
        if (data.p1CharID == CHAR_ID_MISUZU) {
                uintptr_t featherAddr = s_pointersP1.misuzuFeather;
            uintptr_t poisonTimerAddr = s_pointersP1.misuzuPoisonTimer;
            uintptr_t poisonLevelAddr = s_pointersP1.misuzuPoisonLevel;
            // Lazily resolve Misuzu pointers if cache was cleared or unresolved.
            if (base) {
                const int off = EFZ_BASE_OFFSET_P1;
                if (!featherAddr)      featherAddr      = s_pointersP1.misuzuFeather     = ResolvePointer(base, off, MISUZU_FEATHER_OFFSET);
                if (!poisonTimerAddr)  poisonTimerAddr  = s_pointersP1.misuzuPoisonTimer = ResolvePointer(base, off, MISUZU_POISON_TIMER_OFFSET);
                if (!poisonLevelAddr)  poisonLevelAddr  = s_pointersP1.misuzuPoisonLevel = ResolvePointer(base, off, MISUZU_POISON_LEVEL_OFFSET);
            }
            
            if (featherAddr) SafeReadMemory(featherAddr, &data.p1MisuzuFeathers, sizeof(int));
         if (poisonTimerAddr) { SafeReadMemory(poisonTimerAddr, &data.p1MisuzuPoisonTimer, sizeof(int)); data.p1MisuzuPoisonTimer = CLAMP(data.p1MisuzuPoisonTimer, 0, MISUZU_POISON_TIMER_MAX); }
         if (poisonLevelAddr)  SafeReadMemory(poisonLevelAddr, &data.p1MisuzuPoisonLevel, sizeof(int));
            
         LogOut(std::string("[CHAR][READ][MISUZU] P1 featherAddr=") + PtrHex(featherAddr) +
             " poisonTimerAddr=" + PtrHex(poisonTimerAddr) +
             " poisonLevelAddr=" + PtrHex(poisonLevelAddr) +
             " Feathers=" + std::to_string(data.p1MisuzuFeathers) +
             " PoisonTimer=" + std::to_string(data.p1MisuzuPoisonTimer) +
             " PoisonLvl=" + std::to_string(data.p1MisuzuPoisonLevel), 
                   detailedLogging.load());
        }
        
        if (data.p2CharID == CHAR_ID_MISUZU) {
                uintptr_t featherAddr = s_pointersP2.misuzuFeather;
            uintptr_t poisonTimerAddr = s_pointersP2.misuzuPoisonTimer;
            uintptr_t poisonLevelAddr = s_pointersP2.misuzuPoisonLevel;
            if (base) {
                const int off = EFZ_BASE_OFFSET_P2;
                if (!featherAddr)      featherAddr      = s_pointersP2.misuzuFeather     = ResolvePointer(base, off, MISUZU_FEATHER_OFFSET);
                if (!poisonTimerAddr)  poisonTimerAddr  = s_pointersP2.misuzuPoisonTimer = ResolvePointer(base, off, MISUZU_POISON_TIMER_OFFSET);
                if (!poisonLevelAddr)  poisonLevelAddr  = s_pointersP2.misuzuPoisonLevel = ResolvePointer(base, off, MISUZU_POISON_LEVEL_OFFSET);
            }
            
            if (featherAddr) SafeReadMemory(featherAddr, &data.p2MisuzuFeathers, sizeof(int));
         if (poisonTimerAddr) { SafeReadMemory(poisonTimerAddr, &data.p2MisuzuPoisonTimer, sizeof(int)); data.p2MisuzuPoisonTimer = CLAMP(data.p2MisuzuPoisonTimer, 0, MISUZU_POISON_TIMER_MAX); }
         if (poisonLevelAddr)  SafeReadMemory(poisonLevelAddr, &data.p2MisuzuPoisonLevel, sizeof(int));
            
         LogOut(std::string("[CHAR][READ][MISUZU] P2 featherAddr=") + PtrHex(featherAddr) +
             " poisonTimerAddr=" + PtrHex(poisonTimerAddr) +
             " poisonLevelAddr=" + PtrHex(poisonLevelAddr) +
             " Feathers=" + std::to_string(data.p2MisuzuFeathers) +
             " PoisonTimer=" + std::to_string(data.p2MisuzuPoisonTimer) +
             " PoisonLvl=" + std::to_string(data.p2MisuzuPoisonLevel), 
                   detailedLogging.load());
        }

        // Doppel Nanase (ExNanase) - read Enlightened flag (0/1)
        if (data.p1CharID == CHAR_ID_EXNANASE) {
                 uintptr_t flagAddr = s_pointersP1.doppelEnlightened;
                 if (base && !flagAddr) {
                     flagAddr = s_pointersP1.doppelEnlightened = ResolvePointer(base, EFZ_BASE_OFFSET_P1, DOPPEL_ENLIGHTENED_OFFSET);
                 }
                 int tmp = 0; if (flagAddr) SafeReadMemory(flagAddr, &tmp, sizeof(int));
                 data.p1DoppelEnlightened = (tmp != 0);
                 LogOut(std::string("[CHAR][READ][DOPPEL] P1 addr=") + PtrHex(flagAddr) +
                     " Enlightened=" + std::to_string(data.p1DoppelEnlightened),
                     detailedLogging.load());
        }
        if (data.p2CharID == CHAR_ID_EXNANASE) {
                 uintptr_t flagAddr = s_pointersP2.doppelEnlightened;
                 if (base && !flagAddr) {
                     flagAddr = s_pointersP2.doppelEnlightened = ResolvePointer(base, EFZ_BASE_OFFSET_P2, DOPPEL_ENLIGHTENED_OFFSET);
                 }
                 int tmp = 0; if (flagAddr) SafeReadMemory(flagAddr, &tmp, sizeof(int));
                 data.p2DoppelEnlightened = (tmp != 0);
                 LogOut(std::string("[CHAR][READ][DOPPEL] P2 addr=") + PtrHex(flagAddr) +
                     " Enlightened=" + std::to_string(data.p2DoppelEnlightened),
                     detailedLogging.load());
        }

        // Nanase (Rumi) – Safe read of mode/gate only (no pointer derefs to anim/move tables)
        auto ReadRumiState = [&](int playerIndex) {
            PlayerCharPointers &p = (playerIndex==1)?s_pointersP1:s_pointersP2;
            const int off = (playerIndex==1)?EFZ_BASE_OFFSET_P1:EFZ_BASE_OFFSET_P2;
            uintptr_t modeAddr = p.rumiModeByte;
            uintptr_t gateAddr = p.rumiWeaponGate;
            uintptr_t kimchiFlagAddr = p.rumiKimchiFlag;
            uintptr_t kimchiTimerAddr = p.rumiKimchiTimer;

            // If any Rumi pointers are missing (e.g. after character re-select),
            // lazily resolve them using the current EFZ base and cache for future calls.
            if (base) {
                if (!modeAddr)        { modeAddr        = p.rumiModeByte    = ResolvePointer(base, off, RUMI_MODE_BYTE_OFFSET); }
                if (!gateAddr)        { gateAddr        = p.rumiWeaponGate  = ResolvePointer(base, off, RUMI_WEAPON_GATE_OFFSET); }
                if (!kimchiFlagAddr)  { kimchiFlagAddr  = p.rumiKimchiFlag  = ResolvePointer(base, off, RUMI_KIMCHI_ACTIVE_OFFSET); }
                if (!kimchiTimerAddr) { kimchiTimerAddr = p.rumiKimchiTimer = ResolvePointer(base, off, RUMI_KIMCHI_TIMER_OFFSET); }
            }

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
                          LogOut(std::string("[CHAR][READ][RUMI] P1 modeAddr=") + PtrHex(modeAddr) +
                              " gateAddr=" + PtrHex(gateAddr) +
                              " kimchiFlagAddr=" + PtrHex(kimchiFlagAddr) +
                              " kimchiTimerAddr=" + PtrHex(kimchiTimerAddr) +
                              " mode=" + std::to_string((int)mode) + ", gate=" + std::to_string((int)gate) +
                              ", KimchiActive=" + std::to_string((int)data.p1RumiKimchiActive) + ", KimchiTimer=" + std::to_string((int)data.p1RumiKimchiTimer),
                              true);
                    s_lastRumiLogP1 = now;
                }
                s_lastRumiModeP1 = (int)mode; s_lastRumiGateP1 = (int)gate;
            } else {
                changed = (s_lastRumiModeP2 != (int)mode) || (s_lastRumiGateP2 != (int)gate);
                heartbeat = (s_lastRumiLogP2.time_since_epoch().count() == 0) || ((now - s_lastRumiLogP2) >= RUMI_LOG_HEARTBEAT);
                if (detailedLogging.load() && (changed || heartbeat)) {
                          LogOut(std::string("[CHAR][READ][RUMI] P2 modeAddr=") + PtrHex(modeAddr) +
                              " gateAddr=" + PtrHex(gateAddr) +
                              " kimchiFlagAddr=" + PtrHex(kimchiFlagAddr) +
                              " kimchiTimerAddr=" + PtrHex(kimchiTimerAddr) +
                              " mode=" + std::to_string((int)mode) + ", gate=" + std::to_string((int)gate) +
                              ", KimchiActive=" + std::to_string((int)data.p2RumiKimchiActive) + ", KimchiTimer=" + std::to_string((int)data.p2RumiKimchiTimer),
                              true);
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
        // If switching to Akiko, reset the timeslow trigger to Inactive to avoid stale carry-over
        bool p1JustSwitchedToAkiko = (data.p1CharID == CHAR_ID_AKIKO && s_prevCharIDP1 != CHAR_ID_AKIKO);
        bool p2JustSwitchedToAkiko = (data.p2CharID == CHAR_ID_AKIKO && s_prevCharIDP2 != CHAR_ID_AKIKO);
        auto ReadAkiko = [&](int playerIndex){
            PlayerCharPointers &p = (playerIndex==1)?s_pointersP1:s_pointersP2;
            uintptr_t bulletAddr = p.akikoBulletCycle;
            uintptr_t timeAddr   = p.akikoTimeslowTrigger;
            // Lazily resolve Akiko pointers if cache was cleared or unresolved.
            if (base) {
                const int off = (playerIndex==1)?EFZ_BASE_OFFSET_P1:EFZ_BASE_OFFSET_P2;
                if (!bulletAddr) bulletAddr = p.akikoBulletCycle    = ResolvePointer(base, off, AKIKO_BULLET_CYCLE_OFFSET);
                if (!timeAddr)   timeAddr   = p.akikoTimeslowTrigger= ResolvePointer(base, off, AKIKO_TIMESLOW_TRIGGER_OFFSET);
            }
            int bullet=0, t=0; if (bulletAddr) SafeReadMemory(bulletAddr,&bullet,sizeof(int)); if (timeAddr) SafeReadMemory(timeAddr,&t,sizeof(int));
            // Clamp bullet cycle to valid range [0..2]; timeslow is [0..3] (0=Inactive,1=A,2=B,3=C)
            bullet = CLAMP(bullet, 0, 2);
            if (t < AKIKO_TIMESLOW_INACTIVE || t > AKIKO_TIMESLOW_C) t = AKIKO_TIMESLOW_INACTIVE;
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
                          LogOut(std::string("[CHAR][READ][AKIKO] P1 bulletAddr=") + PtrHex(bulletAddr) +
                              " timeAddr=" + PtrHex(timeAddr) +
                              " BulletCycle=" + std::to_string(bullet) + ", TimeSlow=" + std::to_string(t),
                              true);
                    s_lastAkikoLogP1 = now;
                }
                s_lastAkikoBulletP1=bullet; s_lastAkikoTimeP1=t;
            } else {
                data.p2AkikoBulletCycle = bullet; data.p2AkikoTimeslowTrigger = t;
                bool changed = (s_lastAkikoBulletP2!=bullet)||(s_lastAkikoTimeP2!=t);
                bool heartbeat = (s_lastAkikoLogP2.time_since_epoch().count()==0)||((now - s_lastAkikoLogP2) >= AKIKO_LOG_HEARTBEAT);
                if (detailedLogging.load() && (changed || heartbeat)) {
                          LogOut(std::string("[CHAR][READ][AKIKO] P2 bulletAddr=") + PtrHex(bulletAddr) +
                              " timeAddr=" + PtrHex(timeAddr) +
                              " BulletCycle=" + std::to_string(bullet) + ", TimeSlow=" + std::to_string(t),
                              true);
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

        // Mio stance (simple byte/int at shared offset 0x3150; 0=Short,1=Long)
        auto ReadMio = [&](int playerIndex){
            PlayerCharPointers &p = (playerIndex==1)?s_pointersP1:s_pointersP2;
            if ((playerIndex==1 && data.p1CharID!=CHAR_ID_MIO) || (playerIndex==2 && data.p2CharID!=CHAR_ID_MIO)) return;
            const int off = (playerIndex==1)?EFZ_BASE_OFFSET_P1:EFZ_BASE_OFFSET_P2;
            uintptr_t stanceAddr = p.mioStance;
            // Lazily resolve Mio stance pointer if cache was cleared between matches.
            if (base && !stanceAddr) {
                stanceAddr = p.mioStance = ResolvePointer(base, off, MIO_STANCE_OFFSET);
            }
            if (!stanceAddr) return;
            int stance=0; SafeReadMemory(stanceAddr,&stance,sizeof(int)); stance = (stance==MIO_STANCE_LONG)?MIO_STANCE_LONG:MIO_STANCE_SHORT;
            if (playerIndex==1) data.p1MioStance = stance; else data.p2MioStance = stance;
                 LogOut(std::string("[CHAR][READ][MIO] ") + (playerIndex==1?"P1":"P2") +
                     " addr=" + PtrHex(stanceAddr) +
                     " stance=" + (stance==MIO_STANCE_LONG?"Long":"Short"),
                     detailedLogging.load());
        }; ReadMio(1); ReadMio(2);

        // Kano magic meter (0..10000) at same 0x3150 slot
        auto ReadKano = [&](int playerIndex){
            PlayerCharPointers &p = (playerIndex==1)?s_pointersP1:s_pointersP2;
            if ((playerIndex==1 && data.p1CharID!=CHAR_ID_KANO) || (playerIndex==2 && data.p2CharID!=CHAR_ID_KANO)) return;
            const int off = (playerIndex==1)?EFZ_BASE_OFFSET_P1:EFZ_BASE_OFFSET_P2;
            uintptr_t magicAddr = p.kanoMagic;
            // Lazily resolve Kano magic pointer if cache was cleared between matches.
            if (base && !magicAddr) {
                magicAddr = p.kanoMagic = ResolvePointer(base, off, KANO_MAGIC_OFFSET);
            }
            if (!magicAddr) return;
            int val=0; SafeReadMemory(magicAddr,&val,sizeof(int));
            val = CLAMP(val, 0, KANO_MAGIC_MAX);
            if (playerIndex==1) data.p1KanoMagic = val; else data.p2KanoMagic = val;
                 LogOut(std::string("[CHAR][READ][KANO] ") + (playerIndex==1?"P1":"P2") +
                     " addr=" + PtrHex(magicAddr) +
                     " magic=" + std::to_string(val),
                     detailedLogging.load());
        }; ReadKano(1); ReadKano(2);

        // Nayuki (Awake) – Snowbunnies timer at shared 0x3150 (0..3000)
        auto ReadNayukiB = [&](int playerIndex){
            if ((playerIndex==1 && data.p1CharID!=CHAR_ID_NAYUKIB) || (playerIndex==2 && data.p2CharID!=CHAR_ID_NAYUKIB)) return;
            PlayerCharPointers &p = (playerIndex==1)?s_pointersP1:s_pointersP2;
            uintptr_t snowAddr = p.nayukiSnowbunnyTimer;
            if (base && !snowAddr) {
                const int offBase = (playerIndex==1)?EFZ_BASE_OFFSET_P1:EFZ_BASE_OFFSET_P2;
                snowAddr = p.nayukiSnowbunnyTimer = ResolvePointer(base, offBase, NAYUKIB_SNOWBUNNY_TIMER_OFFSET);
            }
            if (!snowAddr) return;
            int v=0; SafeReadMemory(snowAddr,&v,sizeof(int)); v = CLAMP(v,0,NAYUKIB_SNOWBUNNY_MAX);
            if (playerIndex==1) data.p1NayukiSnowbunnies = v; else data.p2NayukiSnowbunnies = v;
            
            // Read snowbunny active flags array (8 snowbunnies, each flag is 4 bytes at +0x3154 + 4*i)
            int activeFlags[8] = {0};
            std::string flagsStr = "";
            const int off = (playerIndex==1)?EFZ_BASE_OFFSET_P1:EFZ_BASE_OFFSET_P2;
            for (int i = 0; i < 8; i++) {
                uintptr_t flagAddr = ResolvePointer(base, off, NAYUKIB_SNOWBUNNY_ACTIVE_FLAGS_BASE + (4 * i));
                if (flagAddr) {
                    SafeReadMemory(flagAddr, &activeFlags[i], sizeof(int));
                    flagsStr += std::to_string(activeFlags[i]);
                    if (i < 7) flagsStr += ",";
                }
            }
            
                 LogOut(std::string("[CHAR][READ][NAYUKIB] ") + (playerIndex==1?"P1":"P2") +
                     " timerAddr=" + PtrHex(snowAddr) +
                     " timer=" + std::to_string(v) +
                     " active=[" + flagsStr + "]", detailedLogging.load());
        }; ReadNayukiB(1); ReadNayukiB(2);

        // Mai (Kawasumi) – Unified status + single multi-purpose timer model
        // Status byte @ MAI_STATUS_OFFSET (0=inactive,1=active ghost,2=unsummon,3=charging,4=awakening)
        // Multi timer  @ MAI_MULTI_TIMER_OFFSET
        auto ReadMai = [&](int playerIndex){
            if ((playerIndex==1 && data.p1CharID!=CHAR_ID_MAI) || (playerIndex==2 && data.p2CharID!=CHAR_ID_MAI)) return;
            const int off = (playerIndex==1)?EFZ_BASE_OFFSET_P1:EFZ_BASE_OFFSET_P2;
            uint8_t status = 0; int multi=0;
            if (auto sAddr = ResolvePointer(base, off, MAI_STATUS_OFFSET)) {
                SafeReadMemory(sAddr, &status, sizeof(uint8_t));
            }
            if (auto tAddr = ResolvePointer(base, off, MAI_MULTI_TIMER_OFFSET)) {
                SafeReadMemory(tAddr, &multi, sizeof(int));
            }
            int displayGhost=0, displayCharge=0, displayAw=0; // populate only the active meaning
            switch (status) {
                case 1: // active ghost time (0..10000)
                    multi = CLAMP(multi, 0, MAI_GHOST_TIME_MAX);
                    displayGhost = multi; break;
                case 3: // charging (0..1200)
                    multi = CLAMP(multi, 0, MAI_GHOST_CHARGE_MAX);
                    displayCharge = multi; break;
                case 4: // awakening active (0..10000)
                    multi = CLAMP(multi, 0, MAI_AWAKENING_MAX);
                    displayAw = multi; break;
                default:
                    multi = CLAMP(multi, 0, MAI_GHOST_TIME_MAX); // clamp generically but don't map
                    break;
            }
            if (playerIndex==1) {
                data.p1MaiStatus = (int)status;
                data.p1MaiGhostTime = displayGhost; // retain old fields for GUI compatibility
                data.p1MaiGhostCharge = displayCharge;
                data.p1MaiAwakeningTime = displayAw;
            } else {
                data.p2MaiStatus = (int)status;
                data.p2MaiGhostTime = displayGhost;
                data.p2MaiGhostCharge = displayCharge;
                data.p2MaiAwakeningTime = displayAw;
            }
            if (detailedLogging.load()) {
                LogOut(std::string("[CHAR] Read ") + (playerIndex==1?"P1":"P2") + " Mai: Status=" + std::to_string((int)status) +
                       ", Timer=" + std::to_string(multi) + " (ghost=" + std::to_string(displayGhost) +
                       ", charge=" + std::to_string(displayCharge) + ", awakening=" + std::to_string(displayAw) + ")", true);
            }
        }; ReadMai(1); ReadMai(2);
    }
    
    void ApplyCharacterValues(uintptr_t base, const DisplayData& data) {
        // Ensure pointer caches have correct base for this session
        if (base) {
            if (s_pointersP1.base == 0 || s_pointersP1.base != base + EFZ_BASE_OFFSET_P1) {
                  LogOut("[CHAR][APPLY] P1 pointer cache invalid or base changed (old=" +
                      PtrHex(s_pointersP1.base) + ", new=" + PtrHex(base + EFZ_BASE_OFFSET_P1) + ")",
                       detailedLogging.load());
                RefreshCharacterPointers(base, 1, data.p1CharID);
            }
            if (s_pointersP2.base == 0 || s_pointersP2.base != base + EFZ_BASE_OFFSET_P2) {
                  LogOut("[CHAR][APPLY] P2 pointer cache invalid or base changed (old=" +
                      PtrHex(s_pointersP2.base) + ", new=" + PtrHex(base + EFZ_BASE_OFFSET_P2) + ")",
                       detailedLogging.load());
                RefreshCharacterPointers(base, 2, data.p2CharID);
            }
        } else {
            LogOut("[CHAR][APPLY] ApplyCharacterValues called with base=0; skipping character writes", detailedLogging.load());
            return;
        }

        // If GUI is hidden and no infinite/lock is enabled, Apply is effectively a no-op.
        if (!g_guiVisible.load() && !AnyInfiniteOrLockEnabled(data)) {
            LogOut("[CHAR][APPLY] Skipping ApplyCharacterValues (GUI hidden, no infinites/locks)", detailedLogging.load());
            return;
        }
        // Apply Ikumi's values (P1)
        if (data.p1CharID == CHAR_ID_IKUMI) {
            // Resolve dynamically each Apply to avoid stale cached addresses across
            // character select / re-entry.
            uintptr_t bloodAddr    = ResolvePointer(base, EFZ_BASE_OFFSET_P1, IKUMI_BLOOD_OFFSET);
            uintptr_t genocideAddr = ResolvePointer(base, EFZ_BASE_OFFSET_P1, IKUMI_GENOCIDE_OFFSET);
            int bloodValue = std::max<int>(0, std::min<int>(IKUMI_BLOOD_MAX, data.p1IkumiBlood));
            int genocideValue = data.infiniteBloodMode ? IKUMI_GENOCIDE_MAX
                                                       : std::max<int>(0, std::min<int>(IKUMI_GENOCIDE_MAX, data.p1IkumiGenocide));
            if (bloodAddr) SafeWriteMemory(bloodAddr, &bloodValue, sizeof(int));
            if (genocideAddr) SafeWriteMemory(genocideAddr, &genocideValue, sizeof(int));
            LogOut(std::string("[CHAR][APPLY][IKUMI] P1 bloodAddr=") + PtrHex(bloodAddr) +
                   " genocideAddr=" + PtrHex(genocideAddr) +
                   " Blood=" + std::to_string(bloodValue) +
                   " Genocide=" + std::to_string(genocideValue) +
                   " (infinite=" + (data.infiniteBloodMode ? "ON" : "OFF") + ")",
                   detailedLogging.load());
        }
        
        // Apply Mishio's values (P1 element and awakened timer)
        if (data.p1CharID == CHAR_ID_MISHIO) {
            uintptr_t elemAddr = s_pointersP1.mishioElement;
            uintptr_t awAddr   = s_pointersP1.mishioAwakenedTimer;
            if (base) {
                if (!elemAddr) elemAddr = s_pointersP1.mishioElement       = ResolvePointer(base, EFZ_BASE_OFFSET_P1, MISHIO_ELEMENT_OFFSET);
                if (!awAddr)   awAddr   = s_pointersP1.mishioAwakenedTimer = ResolvePointer(base, EFZ_BASE_OFFSET_P1, MISHIO_AWAKENED_TIMER_OFFSET);
            }
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
            LogOut(std::string("[CHAR][APPLY][MISHIO] P1 elemAddr=") + PtrHex(elemAddr) +
                   " awAddr=" + PtrHex(awAddr) +
                   " Elem=" + std::to_string(data.p1MishioElement) +
                   " AwTimer=" + std::to_string(data.p1MishioAwakenedTimer),
                   detailedLogging.load());
        }
        if (data.p2CharID == CHAR_ID_MISHIO) {
            uintptr_t elemAddr = s_pointersP2.mishioElement;
            uintptr_t awAddr   = s_pointersP2.mishioAwakenedTimer;
            if (base) {
                if (!elemAddr) elemAddr = s_pointersP2.mishioElement       = ResolvePointer(base, EFZ_BASE_OFFSET_P2, MISHIO_ELEMENT_OFFSET);
                if (!awAddr)   awAddr   = s_pointersP2.mishioAwakenedTimer = ResolvePointer(base, EFZ_BASE_OFFSET_P2, MISHIO_AWAKENED_TIMER_OFFSET);
            }
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
            LogOut(std::string("[CHAR][APPLY][MISHIO] P2 elemAddr=") + PtrHex(elemAddr) +
                   " awAddr=" + PtrHex(awAddr) +
                   " Elem=" + std::to_string(data.p2MishioElement) +
                   " AwTimer=" + std::to_string(data.p2MishioAwakenedTimer),
                   detailedLogging.load());
        }

        // Ikumi values (P2)
        if (data.p2CharID == CHAR_ID_IKUMI) {
            // Resolve dynamically each Apply to avoid stale cached addresses across
            // character select / re-entry.
            uintptr_t bloodAddr    = ResolvePointer(base, EFZ_BASE_OFFSET_P2, IKUMI_BLOOD_OFFSET);
            uintptr_t genocideAddr = ResolvePointer(base, EFZ_BASE_OFFSET_P2, IKUMI_GENOCIDE_OFFSET);

            int bloodValue = std::max<int>(0, std::min<int>(IKUMI_BLOOD_MAX, data.p2IkumiBlood));
            // For infinite mode, set genocide timer to max, otherwise use the provided value
            int genocideValue = data.infiniteBloodMode ? IKUMI_GENOCIDE_MAX : 
                              std::max<int>(0, std::min<int>(IKUMI_GENOCIDE_MAX, data.p2IkumiGenocide));
            
            if (bloodAddr) SafeWriteMemory(bloodAddr, &bloodValue, sizeof(int));
            if (genocideAddr) SafeWriteMemory(genocideAddr, &genocideValue, sizeof(int));
            
                 LogOut(std::string("[CHAR][APPLY][IKUMI] P2 bloodAddr=") + PtrHex(bloodAddr) +
                     " genocideAddr=" + PtrHex(genocideAddr) +
                     " Blood=" + std::to_string(bloodValue) +
                     " Genocide=" + std::to_string(genocideValue),
                     detailedLogging.load());
        }
        
    // Apply Misuzu's values if either player is using her
        if (data.p1CharID == CHAR_ID_MISUZU) {
             uintptr_t featherAddr = s_pointersP1.misuzuFeather;
          uintptr_t poisonTimerAddr = s_pointersP1.misuzuPoisonTimer;
          uintptr_t poisonLevelAddr = s_pointersP1.misuzuPoisonLevel;
            if (base) {
                const int off = EFZ_BASE_OFFSET_P1;
                if (!featherAddr)      featherAddr      = s_pointersP1.misuzuFeather     = ResolvePointer(base, off, MISUZU_FEATHER_OFFSET);
                if (!poisonTimerAddr)  poisonTimerAddr  = s_pointersP1.misuzuPoisonTimer = ResolvePointer(base, off, MISUZU_POISON_TIMER_OFFSET);
                if (!poisonLevelAddr)  poisonLevelAddr  = s_pointersP1.misuzuPoisonLevel = ResolvePointer(base, off, MISUZU_POISON_LEVEL_OFFSET);
            }
            
            int featherValue = std::max<int>(0, std::min<int>(MISUZU_FEATHER_MAX, data.p1MisuzuFeathers));
            
            if (featherAddr) SafeWriteMemory(featherAddr, &featherValue, sizeof(int));
            if (poisonTimerAddr) { int t = CLAMP(data.p1MisuzuPoisonTimer, 0, MISUZU_POISON_TIMER_MAX); if (data.p1MisuzuInfinitePoison) t = MISUZU_POISON_TIMER_MAX; SafeWriteMemory(poisonTimerAddr, &t, sizeof(int)); }
         if (poisonLevelAddr)  { int l = data.p1MisuzuPoisonLevel; SafeWriteMemory(poisonLevelAddr, &l, sizeof(int)); }
            
         LogOut(std::string("[CHAR][APPLY][MISUZU] P1 featherAddr=") + PtrHex(featherAddr) +
             " poisonTimerAddr=" + PtrHex(poisonTimerAddr) +
             " poisonLevelAddr=" + PtrHex(poisonLevelAddr) +
             " Feathers=" + std::to_string(featherValue) +
             " PoisonTimer=" + std::to_string(data.p1MisuzuPoisonTimer) +
             " PoisonLvl=" + std::to_string(data.p1MisuzuPoisonLevel), 
                   detailedLogging.load());
        }
        
          if (data.p2CharID == CHAR_ID_MISUZU) {
                uintptr_t featherAddr = s_pointersP2.misuzuFeather;
            uintptr_t poisonTimerAddr = s_pointersP2.misuzuPoisonTimer;
            uintptr_t poisonLevelAddr = s_pointersP2.misuzuPoisonLevel;
            if (base) {
                const int off = EFZ_BASE_OFFSET_P2;
                if (!featherAddr)      featherAddr      = s_pointersP2.misuzuFeather     = ResolvePointer(base, off, MISUZU_FEATHER_OFFSET);
                if (!poisonTimerAddr)  poisonTimerAddr  = s_pointersP2.misuzuPoisonTimer = ResolvePointer(base, off, MISUZU_POISON_TIMER_OFFSET);
                if (!poisonLevelAddr)  poisonLevelAddr  = s_pointersP2.misuzuPoisonLevel = ResolvePointer(base, off, MISUZU_POISON_LEVEL_OFFSET);
            }
            
            int featherValue = std::max<int>(0, std::min<int>(MISUZU_FEATHER_MAX, data.p2MisuzuFeathers));
            
            if (featherAddr) SafeWriteMemory(featherAddr, &featherValue, sizeof(int));
            if (poisonTimerAddr) { int t = CLAMP(data.p2MisuzuPoisonTimer, 0, MISUZU_POISON_TIMER_MAX); if (data.p2MisuzuInfinitePoison) t = MISUZU_POISON_TIMER_MAX; SafeWriteMemory(poisonTimerAddr, &t, sizeof(int)); }
         if (poisonLevelAddr)  { int l = data.p2MisuzuPoisonLevel; SafeWriteMemory(poisonLevelAddr, &l, sizeof(int)); }

         LogOut(std::string("[CHAR][APPLY][MISUZU] P2 featherAddr=") + PtrHex(featherAddr) +
             " poisonTimerAddr=" + PtrHex(poisonTimerAddr) +
             " poisonLevelAddr=" + PtrHex(poisonLevelAddr) +
             " Feathers=" + std::to_string(featherValue) +
             " PoisonTimer=" + std::to_string(data.p2MisuzuPoisonTimer) +
             " PoisonLvl=" + std::to_string(data.p2MisuzuPoisonLevel), 
                   detailedLogging.load());
        }

        // Doppel Enlightened: simple checkbox -> set flag 1 when checked, 0 when unchecked
        if (data.p1CharID == CHAR_ID_EXNANASE) {
            uintptr_t flagAddr = s_pointersP1.doppelEnlightened;
            if (base && !flagAddr) {
                flagAddr = s_pointersP1.doppelEnlightened = ResolvePointer(base, EFZ_BASE_OFFSET_P1, DOPPEL_ENLIGHTENED_OFFSET);
            }
            if (flagAddr) {
                int v = data.p1DoppelEnlightened ? 1 : 0;
                SafeWriteMemory(flagAddr, &v, sizeof(int));
                LogOut(std::string("[CHAR][APPLY][DOPPEL] P1 addr=") + PtrHex(flagAddr) +
                       " value=" + std::to_string(v),
                       detailedLogging.load());
            }
        }
        if (data.p2CharID == CHAR_ID_EXNANASE) {
            uintptr_t flagAddr = s_pointersP2.doppelEnlightened;
            if (base && !flagAddr) {
                flagAddr = s_pointersP2.doppelEnlightened = ResolvePointer(base, EFZ_BASE_OFFSET_P2, DOPPEL_ENLIGHTENED_OFFSET);
            }
            if (flagAddr) {
                int v = data.p2DoppelEnlightened ? 1 : 0;
                SafeWriteMemory(flagAddr, &v, sizeof(int));
                LogOut(std::string("[CHAR][APPLY][DOPPEL] P2 addr=") + PtrHex(flagAddr) +
                       " value=" + std::to_string(v),
                       detailedLogging.load());
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

            PlayerCharPointers &pc = (playerIndex==1)?s_pointersP1:s_pointersP2;
            uintptr_t modeByteAddr  = pc.rumiModeByte;
            uintptr_t gateAddr      = pc.rumiWeaponGate;

            // If caches are empty (e.g. after returning from Character Select),
            // refresh Rumi mode/gate pointers using the current base.
            if (base) {
                if (!modeByteAddr) modeByteAddr = pc.rumiModeByte = ResolvePointer(base, baseOffset, RUMI_MODE_BYTE_OFFSET);
                if (!gateAddr)     gateAddr     = pc.rumiWeaponGate = ResolvePointer(base, baseOffset, RUMI_WEAPON_GATE_OFFSET);
            }
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

            // Lazily resolve Kimchi pointers if caches are empty (e.g. after re-selecting Rumi).
            PlayerCharPointers &p1 = s_pointersP1;
            if (base) {
                const int off1 = EFZ_BASE_OFFSET_P1;
                if (!p1.rumiKimchiFlag)  p1.rumiKimchiFlag  = ResolvePointer(base, off1, RUMI_KIMCHI_ACTIVE_OFFSET);
                if (!p1.rumiKimchiTimer) p1.rumiKimchiTimer = ResolvePointer(base, off1, RUMI_KIMCHI_TIMER_OFFSET);
            }

            // Apply Kimchi activation/timer if fields are present
            if (uintptr_t flag = p1.rumiKimchiFlag) {
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
            if (uintptr_t tim = p1.rumiKimchiTimer) {
                int t = data.p1RumiKimchiTimer; if (t < 0) t = 0; if (t > RUMI_KIMCHI_TARGET) t = RUMI_KIMCHI_TARGET; SafeWriteMemory(tim, &t, sizeof(int));
            }
        }
        if (data.p2CharID == CHAR_ID_NANASE) {
            const bool wantBarehand = data.p2RumiInfiniteShinai ? false : data.p2RumiBarehanded;
            ApplyRumiMode(2, wantBarehand);

            PlayerCharPointers &p2 = s_pointersP2;
            if (base) {
                const int off2 = EFZ_BASE_OFFSET_P2;
                if (!p2.rumiKimchiFlag)  p2.rumiKimchiFlag  = ResolvePointer(base, off2, RUMI_KIMCHI_ACTIVE_OFFSET);
                if (!p2.rumiKimchiTimer) p2.rumiKimchiTimer = ResolvePointer(base, off2, RUMI_KIMCHI_TIMER_OFFSET);
            }

            if (uintptr_t flag = p2.rumiKimchiFlag) {
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
            if (uintptr_t tim = p2.rumiKimchiTimer) {
                int t = data.p2RumiKimchiTimer; if (t < 0) t = 0; if (t > RUMI_KIMCHI_TARGET) t = RUMI_KIMCHI_TARGET; SafeWriteMemory(tim, &t, sizeof(int));
            }
        }
        
        // No background threads anymore; enforcement happens inline via TickCharacterEnforcements()

        // Apply Akiko values directly when set in DisplayData
        auto ApplyAkiko = [&](int pi){
            if ((pi==1 && data.p1CharID != CHAR_ID_AKIKO) || (pi==2 && data.p2CharID != CHAR_ID_AKIKO)) return;
            PlayerCharPointers &p = (pi==1)?s_pointersP1:s_pointersP2;
            uintptr_t bulletAddr = p.akikoBulletCycle;
            if (base && !bulletAddr) {
                const int off = (pi==1)?EFZ_BASE_OFFSET_P1:EFZ_BASE_OFFSET_P2;
                bulletAddr = p.akikoBulletCycle = ResolvePointer(base, off, AKIKO_BULLET_CYCLE_OFFSET);
            }
            int bullet = (pi==1)?data.p1AkikoBulletCycle:data.p2AkikoBulletCycle;
            int t      = (pi==1)?data.p1AkikoTimeslowTrigger:data.p2AkikoTimeslowTrigger;
            bullet = CLAMP(bullet, 0, 2);
            // Timeslow trigger respects only 0..3 now; any other value becomes Inactive
                 if (t < AKIKO_TIMESLOW_INACTIVE || t > AKIKO_TIMESLOW_C) t = AKIKO_TIMESLOW_INACTIVE;
                 if (bulletAddr) SafeWriteMemory(bulletAddr,&bullet,sizeof(int));
                 LogOut(std::string("[CHAR][APPLY][AKIKO] ") + (pi==1?"P1":"P2") +
                     " bulletAddr=" + PtrHex(bulletAddr) +
                     " BulletCycle=" + std::to_string(bullet) +
                     " TimeSlow=" + std::to_string(t),
                     detailedLogging.load());
        }; ApplyAkiko(1); ApplyAkiko(2);

        // Apply Neyuki jam count if present
        auto ApplyNeyuki = [&](int pi){
            if ((pi==1 && data.p1CharID != CHAR_ID_NAYUKI) || (pi==2 && data.p2CharID != CHAR_ID_NAYUKI)) return;
            PlayerCharPointers &p = (pi==1)?s_pointersP1:s_pointersP2;
            if (uintptr_t jamAddr = p.neyukiJamCount) {
                int jam = (pi==1)? data.p1NeyukiJamCount : data.p2NeyukiJamCount;
                if (jam < 0) jam = 0; else if (jam > NEYUKI_JAM_COUNT_MAX) jam = NEYUKI_JAM_COUNT_MAX;
                SafeWriteMemory(jamAddr, &jam, sizeof(int));
                LogOut(std::string("[CHAR][APPLY][NEYUKI] ") + (pi==1?"P1":"P2") +
                       " addr=" + PtrHex(jamAddr) +
                       " JamCount=" + std::to_string(jam),
                       detailedLogging.load());
            }
        }; ApplyNeyuki(1); ApplyNeyuki(2);

        // Mio stance application (only write when user changed value; locking handled in TickCharacterEnforcements)
        auto ApplyMio = [&](int pi){
            if ((pi==1 && data.p1CharID != CHAR_ID_MIO) || (pi==2 && data.p2CharID != CHAR_ID_MIO)) return;
            PlayerCharPointers &p = (pi==1)?s_pointersP1:s_pointersP2;
            const int off = (pi==1)?EFZ_BASE_OFFSET_P1:EFZ_BASE_OFFSET_P2;
            uintptr_t stanceAddr = p.mioStance;
            // Lazily resolve Mio stance pointer if cache was cleared between matches.
            if (base && !stanceAddr) {
                stanceAddr = p.mioStance = ResolvePointer(base, off, MIO_STANCE_OFFSET);
            }
            if (!stanceAddr) return;
            int desired = (pi==1)?data.p1MioStance:data.p2MioStance;
            desired = (desired==MIO_STANCE_LONG)?MIO_STANCE_LONG:MIO_STANCE_SHORT;
            int cur=0; SafeReadMemory(stanceAddr,&cur,sizeof(int));
            if (cur != desired) {
                SafeWriteMemory(stanceAddr,&desired,sizeof(int));
                LogOut(std::string("[CHAR][APPLY][MIO] ") + (pi==1?"P1":"P2") +
                       " addr=" + PtrHex(stanceAddr) +
                       " stance=" + (desired==MIO_STANCE_LONG?"Long":"Short"),
                       detailedLogging.load());
            }
        }; ApplyMio(1); ApplyMio(2);

        // Kano magic meter (respect user value; locking handled per-tick)
        auto ApplyKano = [&](int pi){
            if ((pi==1 && data.p1CharID != CHAR_ID_KANO) || (pi==2 && data.p2CharID != CHAR_ID_KANO)) return;
            PlayerCharPointers &p = (pi==1)?s_pointersP1:s_pointersP2;
            const int off = (pi==1)?EFZ_BASE_OFFSET_P1:EFZ_BASE_OFFSET_P2;
            uintptr_t magicAddr = p.kanoMagic;
            // Lazily resolve Kano magic pointer if cache was cleared between matches.
            if (base && !magicAddr) {
                magicAddr = p.kanoMagic = ResolvePointer(base, off, KANO_MAGIC_OFFSET);
            }
            if (!magicAddr) return;
            int desired = (pi==1)?data.p1KanoMagic:data.p2KanoMagic;
            desired = CLAMP(desired,0,KANO_MAGIC_MAX);
            int cur=0; SafeReadMemory(magicAddr,&cur,sizeof(int));
            if (cur != desired) {
                SafeWriteMemory(magicAddr,&desired,sizeof(int));
                LogOut(std::string("[CHAR][APPLY][KANO] ") + (pi==1?"P1":"P2") +
                       " addr=" + PtrHex(magicAddr) +
                       " magic=" + std::to_string(desired),
                       detailedLogging.load());
            }
        }; ApplyKano(1); ApplyKano(2);

        // Nayuki (Awake) – apply snowbunnies timer
        auto ApplyNayukiB = [&](int pi){
            if ((pi==1 && data.p1CharID != CHAR_ID_NAYUKIB) || (pi==2 && data.p2CharID != CHAR_ID_NAYUKIB)) return;
            PlayerCharPointers &p = (pi==1)?s_pointersP1:s_pointersP2;
            if (auto addr = p.nayukiSnowbunnyTimer) {
                if (base && !addr) {
                    const int off = (pi==1)?EFZ_BASE_OFFSET_P1:EFZ_BASE_OFFSET_P2;
                    addr = p.nayukiSnowbunnyTimer = ResolvePointer(base, off, NAYUKIB_SNOWBUNNY_TIMER_OFFSET);
                }
                if (!addr) return;
                int desired = (pi==1)?data.p1NayukiSnowbunnies:data.p2NayukiSnowbunnies;
                // If Infinite is enabled, force to 3000 immediately here as well
                bool wantInf = (pi==1)?data.p1NayukiInfiniteSnow:data.p2NayukiInfiniteSnow;
                if (wantInf) desired = NAYUKIB_SNOWBUNNY_MAX;
                desired = CLAMP(desired,0,NAYUKIB_SNOWBUNNY_MAX);
                int cur=0; SafeReadMemory(addr,&cur,sizeof(int));
                if (cur != desired) { SafeWriteMemory(addr,&desired,sizeof(int));
                    LogOut(std::string("[CHAR][APPLY][NAYUKIB] ") + (pi==1?"P1":"P2") +
                           " addr=" + PtrHex(addr) +
                           " snowbunnies=" + std::to_string(desired),
                           detailedLogging.load());
                }
            }
        }; ApplyNayukiB(1); ApplyNayukiB(2);

        // Mai – apply status + unified timer
        auto ApplyMai = [&](int pi){
            if ((pi==1 && data.p1CharID!=CHAR_ID_MAI) || (pi==2 && data.p2CharID!=CHAR_ID_MAI)) return;
            const int off = (pi==1)?EFZ_BASE_OFFSET_P1:EFZ_BASE_OFFSET_P2;
            PlayerCharPointers &pc = (pi==1)?s_pointersP1:s_pointersP2;
            // Lazily resolve Mai core pointers if cache was cleared or unresolved.
            if (base) {
                if (!pc.maiStatus)         pc.maiStatus         = ResolvePointer(base, off, MAI_STATUS_OFFSET);
                if (!pc.maiMultiTimer)     pc.maiMultiTimer     = ResolvePointer(base, off, MAI_MULTI_TIMER_OFFSET);
                if (!pc.maiSummonFlashFlag)pc.maiSummonFlashFlag= ResolvePointer(base, off, MAI_SUMMON_FLASH_FLAG_OFFSET);
            }
            int status = (pi==1)?data.p1MaiStatus:data.p2MaiStatus;
            status = CLAMP(status,0,4);
            bool forceSummon = (pi==1)?data.p1MaiForceSummon:data.p2MaiForceSummon;
            bool forceDespawn = (pi==1)?data.p1MaiForceDespawn:data.p2MaiForceDespawn;
            bool aggressive = (pi==1)?data.p1MaiAggressiveOverride:data.p2MaiAggressiveOverride;
            // Determine desired timer source based on status
            int desiredTimer = 0;
            bool infGhost   = (pi==1)?data.p1MaiInfiniteGhost:data.p2MaiInfiniteGhost;
            bool infCharge  = (pi==1)?data.p1MaiInfiniteCharge:data.p2MaiInfiniteCharge;
            bool infAw      = (pi==1)?data.p1MaiInfiniteAwakening:data.p2MaiInfiniteAwakening;
            bool noCD       = (pi==1)?data.p1MaiNoChargeCD:data.p2MaiNoChargeCD;

            // Read live status/timer for decision logic
            int liveStatus = 0; int liveTimer = 0;
            if (auto sAddrLive = pc.maiStatus) { uint8_t ls=0; SafeReadMemory(sAddrLive,&ls,sizeof(uint8_t)); liveStatus = ls; }
            if (auto tAddrLive = pc.maiMultiTimer) { SafeReadMemory(tAddrLive,&liveTimer,sizeof(int)); }

            // Handle Force Despawn: convert to unsummon path (simulate timer out)
            if (forceDespawn) {
                // Only if currently active (1) or awakening (4); otherwise just clear request
                if (liveStatus == 1 || liveStatus == 4) {
                    if (auto tAddr = pc.maiMultiTimer) {
                        int zero = 0; SafeWriteMemory(tAddr,&zero,sizeof(int));
                    }
                    // Let natural state machine shift to unsummon (will set status=2)
                }
                // Clear flag
                if (pi==1) { ((DisplayData&)data).p1MaiForceDespawn = false; } else { ((DisplayData&)data).p2MaiForceDespawn = false; }
            }

            // Handle Force Summon (safe path): only if not already active OR aggressive override
            if (forceSummon) {
                bool canSummon = (liveStatus==0 || liveStatus==3 || liveStatus==4 || (aggressive && liveStatus==2));
                if (canSummon) {
                    // Authentic summon: trigger native summon move (0x104) so engine performs all side effects.
                    if (auto mvAddr = ResolvePointer(base, off, MOVE_ID_OFFSET)) {
                        short summonMove = (short)MAI_SUMMON_MOVE_ID; SafeWriteMemory(mvAddr, &summonMove, sizeof(short));
                    }
                    // Reset frame index and subframe counters (engine uses +0x0A/+0x0C) so script starts clean.
                    if (auto frameIdxAddr = ResolvePointer(base, off, STATE_FRAME_INDEX_OFFSET)) {
                        unsigned short z=0; SafeWriteMemory(frameIdxAddr,&z,sizeof(unsigned short));
                    }
                    if (auto subFrameAddr = ResolvePointer(base, off, STATE_SUBFRAME_COUNTER_OFFSET)) {
                        unsigned short z=0; SafeWriteMemory(subFrameAddr,&z,sizeof(unsigned short));
                    }
                    // Seed flash flag (duplicate write harmless; engine sets it first tick).
                    if (auto flashAddr = pc.maiSummonFlashFlag) { int one=1; SafeWriteMemory(flashAddr,&one,sizeof(int)); }
                    // Cache desired timer target for later infinite enforcement once status becomes 1.
                    int desired = (pi==1)?data.p1MaiGhostTime:data.p2MaiGhostTime; desired = CLAMP(desired,1,MAI_GHOST_TIME_MAX);
                    if (infGhost && desired < MAI_GHOST_TIME_MAX) desired = MAI_GHOST_TIME_MAX;
                    if (pi==1) { ((DisplayData&)data).p1MaiGhostTime = desired; } else { ((DisplayData&)data).p2MaiGhostTime = desired; }
                }
                // Clear request flag regardless (so button acts edge-triggered)
                if (pi==1) { ((DisplayData&)data).p1MaiForceSummon = false; } else { ((DisplayData&)data).p2MaiForceSummon = false; }
            }
            static bool p1MaiNoCDArmed=false, p2MaiNoCDArmed=false;
            // No CD now simply handled per-tick when status==3 (charge) in enforcement; no one-shot logic needed here.

            if (status == 1) {
                desiredTimer = (pi==1)?data.p1MaiGhostTime:data.p2MaiGhostTime; desiredTimer = CLAMP(desiredTimer,0,MAI_GHOST_TIME_MAX);
            } else if (status == 3) {
                desiredTimer = (pi==1)?data.p1MaiGhostCharge:data.p2MaiGhostCharge; desiredTimer = CLAMP(desiredTimer,0,MAI_GHOST_CHARGE_MAX);
            } else if (status == 4) {
                desiredTimer = (pi==1)?data.p1MaiAwakeningTime:data.p2MaiAwakeningTime; desiredTimer = CLAMP(desiredTimer,0,MAI_AWAKENING_MAX);
            } else {
                // Avoid wiping underlying timer if an infinite toggle is active; leave as-is
                if (infGhost || infCharge || infAw) {
                    desiredTimer = -1; // sentinel skip write
                } else {
                    desiredTimer = 0;
                }
            }
            if (auto sAddr = pc.maiStatus) {
                uint8_t cur=0; SafeReadMemory(sAddr,&cur,sizeof(uint8_t)); uint8_t st = (uint8_t)status; if (cur!=st) SafeWriteMemory(sAddr,&st,sizeof(uint8_t));
            }
            if (desiredTimer != -1) {
                if (auto tAddr = pc.maiMultiTimer) {
                    int cur=0; SafeReadMemory(tAddr,&cur,sizeof(int)); if (cur!=desiredTimer) SafeWriteMemory(tAddr,&desiredTimer,sizeof(int));
                }
            }
            // Ghost coordinate override write (one-shot): write only when Apply is pressed
            double setX = (pi==1)?data.p1MaiGhostSetX:data.p2MaiGhostSetX;
            double setY = (pi==1)?data.p1MaiGhostSetY:data.p2MaiGhostSetY;
            bool applyGhost = (pi==1)?data.p1MaiApplyGhostPos:data.p2MaiApplyGhostPos;
            if (applyGhost && !std::isnan(setX) && !std::isnan(setY)) {
                if (auto basePtr = ResolvePointer(base, off, 0)) {
                    for (int i=0;i<MAI_GHOST_SLOT_MAX_SCAN;i++) {
                        uintptr_t slot = basePtr + MAI_GHOST_SLOTS_BASE + (uintptr_t)i*MAI_GHOST_SLOT_STRIDE;
                        unsigned short id=0; if (!SafeReadMemory(slot + MAI_GHOST_SLOT_ID_OFFSET,&id,sizeof(id))) break;
                        if (id==401) {
                            SafeWriteMemory(slot + MAI_GHOST_SLOT_X_OFFSET,&setX,sizeof(double));
                            SafeWriteMemory(slot + MAI_GHOST_SLOT_Y_OFFSET,&setY,sizeof(double));
                            // Clear the apply flag (one-shot semantics)
                            if (pi==1) { const_cast<DisplayData&>(data).p1MaiApplyGhostPos = false; }
                            else { const_cast<DisplayData&>(data).p2MaiApplyGhostPos = false; }
                            break;
                        }
                    }
                }
            }
            if (detailedLogging.load()) {
                LogOut(std::string("[CHAR][APPLY][MAI] ") + (pi==1?"P1":"P2") +
                       " status=" + std::to_string(status) +
                       " desiredTimer=" + std::to_string(desiredTimer) +
                       (noCD?" (NoCD armed)":""),
                       true);
            }
        }; ApplyMai(1); ApplyMai(2);
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

        // If GUI is hidden and no infinite/lock is enabled, skip all enforcement work.
        if (!g_guiVisible.load() && !AnyInfiniteOrLockEnabled(localData)) {
            return;
        }

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

        // Misuzu feather freeze / auto-refill on super usage
        if (localData.infiniteFeatherMode) {
            auto keepFeathers = [&](int pi){
                if ((pi==1 && localData.p1CharID != CHAR_ID_MISUZU) || (pi==2 && localData.p2CharID != CHAR_ID_MISUZU)) return;
                const int off = (pi==1)?EFZ_BASE_OFFSET_P1:EFZ_BASE_OFFSET_P2;

                // Detect supers that consume a feather and replenish by +1 (up to max)
                short mv = 0;
                if (auto mvAddr = ResolvePointer(base, off, MOVE_ID_OFFSET)) {
                    SafeReadMemory(mvAddr, &mv, sizeof(short));
                    if (mv == 313 || mv == 314 || mv == 315) {
                        if (auto addr = ResolvePointer(base, off, MISUZU_FEATHER_OFFSET)) {
                            int cur = 0; SafeReadMemory(addr, &cur, sizeof(int));
                            int next = cur + 1;
                            if (next > MISUZU_FEATHER_MAX) next = MISUZU_FEATHER_MAX;
                            if (next != cur) {
                                SafeWriteMemory(addr, &next, sizeof(int));
                                didWriteThisTick = true;
                            }
                        }
                    }
                }

                // Classic infinite-feather behaviour: prevent feathers from decreasing
                if (auto addr = ResolvePointer(base, off, MISUZU_FEATHER_OFFSET)) {
                    int cur=0; SafeReadMemory(addr,&cur,sizeof(int));
                    int &last = (pi==1)?p1LastFeatherCount:p2LastFeatherCount;
                    if (last == 0) last = cur; // initialize
                    if (cur < last) { SafeWriteMemory(addr,&last,sizeof(int)); didWriteThisTick = true; }
                    else if (cur > last) { last = cur; }
                }
            }; keepFeathers(1); keepFeathers(2);
        } else { p1LastFeatherCount = 0; p2LastFeatherCount = 0; }

        // Misuzu: Infinite Poison timer — hard-set to max every tick for selected side(s)
        auto enforceMisuzuPoison = [&](int pi){
            bool isMisuzu = (pi==1)?(localData.p1CharID==CHAR_ID_MISUZU):(localData.p2CharID==CHAR_ID_MISUZU);
            bool wantInf = (pi==1)?localData.p1MisuzuInfinitePoison:localData.p2MisuzuInfinitePoison;
            if (!(isMisuzu && wantInf)) return;
            const int off = (pi==1)?EFZ_BASE_OFFSET_P1:EFZ_BASE_OFFSET_P2;
            auto tAddr = ResolvePointer(base, off, MISUZU_POISON_TIMER_OFFSET); if (!tAddr) return;
            int cur=0; SafeReadMemory(tAddr,&cur,sizeof(int));
            const int target = MISUZU_POISON_TIMER_MAX;
            if (cur != target) { SafeWriteMemory(tAddr,&target,sizeof(int)); }
        }; enforceMisuzuPoison(1); enforceMisuzuPoison(2);

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

        // Akiko: enforce Infinite by freezing on-screen timer digits to 000 when the checkbox is enabled
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
            // Only digits are controlled when Infinite is enabled; no writes to trigger value at all
            const bool wantInf = (pi==1)?localData.p1AkikoInfiniteTimeslow:localData.p2AkikoInfiniteTimeslow;
            if (wantInf) {
                // Continuously write zeros to the three on-screen digits to prevent any countdown
                if (auto d3 = ResolvePointer(base, off, AKIKO_TIMESLOW_THIRD_OFFSET)) { int z=0; SafeWriteMemory(d3,&z,sizeof(int)); }
                if (auto d2 = ResolvePointer(base, off, AKIKO_TIMESLOW_SECOND_OFFSET)) { int z=0; SafeWriteMemory(d2,&z,sizeof(int)); }
                if (auto d1 = ResolvePointer(base, off, AKIKO_TIMESLOW_FIRST_OFFSET)) { int z=0; SafeWriteMemory(d1,&z,sizeof(int)); }
            }
        }; enforceAkiko(1); enforceAkiko(2);

        // Mio stance lock
        auto enforceMio = [&](int pi){
            bool lock = (pi==1)?localData.p1MioLockStance:localData.p2MioLockStance;
            if (!lock) return;
            if ((pi==1 && localData.p1CharID != CHAR_ID_MIO) || (pi==2 && localData.p2CharID != CHAR_ID_MIO)) return;
            const int off = (pi==1)?EFZ_BASE_OFFSET_P1:EFZ_BASE_OFFSET_P2;
            auto addr = ResolvePointer(base, off, MIO_STANCE_OFFSET); if (!addr) return;
            int want = (pi==1)?localData.p1MioStance:localData.p2MioStance; want = (want==MIO_STANCE_LONG)?MIO_STANCE_LONG:MIO_STANCE_SHORT;
            int cur=0; SafeReadMemory(addr,&cur,sizeof(int));
            if (cur != want) { SafeWriteMemory(addr,&want,sizeof(int)); }
        }; enforceMio(1); enforceMio(2);

        // Kano magic lock
        auto enforceKano = [&](int pi){
            bool lock = (pi==1)?localData.p1KanoLockMagic:localData.p2KanoLockMagic;
            if (!lock) return;
            if ((pi==1 && localData.p1CharID != CHAR_ID_KANO) || (pi==2 && localData.p2CharID != CHAR_ID_KANO)) return;
            const int off = (pi==1)?EFZ_BASE_OFFSET_P1:EFZ_BASE_OFFSET_P2;
            auto addr = ResolvePointer(base, off, KANO_MAGIC_OFFSET); if (!addr) return;
            int want = (pi==1)?localData.p1KanoMagic:localData.p2KanoMagic; want = CLAMP(want,0,KANO_MAGIC_MAX);
            int cur=0; SafeReadMemory(addr,&cur,sizeof(int)); if (cur != want) { SafeWriteMemory(addr,&want,sizeof(int)); }
        }; enforceKano(1); enforceKano(2);

        // Nayuki (Awake) – infinite snowbunnies: hard-set to max (3000) every tick to prevent any decay
        auto enforceNayukiB = [&](int pi){
            bool isAwake = (pi==1)?(localData.p1CharID==CHAR_ID_NAYUKIB):(localData.p2CharID==CHAR_ID_NAYUKIB);
            bool wantInf = (pi==1)?localData.p1NayukiInfiniteSnow:localData.p2NayukiInfiniteSnow;
            if (!(isAwake && wantInf)) return;
            const int off = (pi==1)?EFZ_BASE_OFFSET_P1:EFZ_BASE_OFFSET_P2;
            auto addr = ResolvePointer(base, off, NAYUKIB_SNOWBUNNY_TIMER_OFFSET); if (!addr) return;
            int cur=0; SafeReadMemory(addr,&cur,sizeof(int));
            // Always force to absolute max; this acts as both freeze and set-to-3k behavior.
            const int target = NAYUKIB_SNOWBUNNY_MAX;
            if (cur != target) { SafeWriteMemory(addr,&target,sizeof(int)); }
        }; enforceNayukiB(1); enforceNayukiB(2);

        // Mai – per-tick enforcement for infinite modes (status-aware)
        static int s_p1MaiFrozenTimer = -1, s_p2MaiFrozenTimer = -1;
        static int s_p1MaiFrozenStatus = -1, s_p2MaiFrozenStatus = -1;
        auto enforceMai = [&](int pi){
            if ((pi==1 && localData.p1CharID!=CHAR_ID_MAI) || (pi==2 && localData.p2CharID!=CHAR_ID_MAI)) return;
            const int off = (pi==1)?EFZ_BASE_OFFSET_P1:EFZ_BASE_OFFSET_P2;
            int status = (pi==1)?localData.p1MaiStatus:localData.p2MaiStatus;
            bool infGhost   = (pi==1)?localData.p1MaiInfiniteGhost:localData.p2MaiInfiniteGhost;
            bool infCharge  = (pi==1)?localData.p1MaiInfiniteCharge:localData.p2MaiInfiniteCharge;
            bool infAw      = (pi==1)?localData.p1MaiInfiniteAwakening:localData.p2MaiInfiniteAwakening;
            bool noCD       = (pi==1)?localData.p1MaiNoChargeCD:localData.p2MaiNoChargeCD;
            bool anyInf = infGhost || infCharge || infAw || noCD; // treat No CD as needing enforcement pass
            if (!anyInf) { if (pi==1){s_p1MaiFrozenTimer=-1; s_p1MaiFrozenStatus=-1;} else {s_p2MaiFrozenTimer=-1; s_p2MaiFrozenStatus=-1;} return; }
            auto tAddr = ResolvePointer(base, off, MAI_MULTI_TIMER_OFFSET);
            auto sAddr = ResolvePointer(base, off, MAI_STATUS_OFFSET);
            if (!(tAddr && sAddr)) return;
            // Read current raw timer & status
            uint8_t curStatus=0; SafeReadMemory(sAddr,&curStatus,sizeof(uint8_t));
            int curTimer=0; SafeReadMemory(tAddr,&curTimer,sizeof(int));
            // No CD handling: if charging (status 3) and No CD enabled, force timer to 1 every tick; allow other inf (e.g., infCharge) to stack if user chooses
            if (curStatus == 3 && noCD) {
                if (curTimer != 1) { int one=1; SafeWriteMemory(tAddr,&one,sizeof(int)); }
                // Don't return if infCharge also active; we still want infCharge semantics to set/hold a user value (but No CD should dominate to 1)
                if (!infCharge && !infGhost && !infAw) {
                    // Pure No CD case: clear caches and return early
                    if (pi==1){s_p1MaiFrozenTimer=-1; s_p1MaiFrozenStatus=-1;} else {s_p2MaiFrozenTimer=-1; s_p2MaiFrozenStatus=-1;}
                    return;
                }
            }
            // Infinite Ghost: always top up to max (hard set) when status is Active (1)
            if (curStatus == 1 && infGhost) {
                const int target = MAI_GHOST_TIME_MAX; // full duration
                if (curTimer != target) { SafeWriteMemory(tAddr,&target,sizeof(int)); }
                // Keep frozen caches clear so switching to other statuses reinitializes appropriately
                if (pi==1){s_p1MaiFrozenTimer=-1; s_p1MaiFrozenStatus=-1;} else {s_p2MaiFrozenTimer=-1; s_p2MaiFrozenStatus=-1;}
                return; // skip rest (charge/awakening logic not relevant here)
            }
            int desiredStatus = (int)curStatus; // default keep
            int desiredTimer = curTimer;
            // Determine which infinite applies based on current status *only*; never force value for a different mode
            if (curStatus == 1 && infGhost) {
                // Handled earlier (hard set to max); unreachable, but keep for clarity
                desiredTimer = MAI_GHOST_TIME_MAX;
            } else if (curStatus == 3 && infCharge) {
                desiredTimer = (pi==1)?localData.p1MaiGhostCharge:localData.p2MaiGhostCharge; desiredTimer = CLAMP(desiredTimer,0,MAI_GHOST_CHARGE_MAX);
            } else if (curStatus == 4 && infAw) {
                desiredTimer = (pi==1)?localData.p1MaiAwakeningTime:localData.p2MaiAwakeningTime; desiredTimer = CLAMP(desiredTimer,0,MAI_AWAKENING_MAX);
            } else {
                // No matching infinite for current status; clear frozen cache for this player
                if (pi==1){s_p1MaiFrozenTimer=-1; s_p1MaiFrozenStatus=-1;} else {s_p2MaiFrozenTimer=-1; s_p2MaiFrozenStatus=-1;}
                return;
            }
            // Initialize / update frozen cache
            int &frozenTimer = (pi==1)?s_p1MaiFrozenTimer:s_p2MaiFrozenTimer;
            int &frozenStatus = (pi==1)?s_p1MaiFrozenStatus:s_p2MaiFrozenStatus;
            if (frozenTimer == -1 || frozenStatus != (int)curStatus) {
                frozenTimer = desiredTimer; frozenStatus = (int)curStatus;
            }
            // For Ghost & Charge we want a hard freeze (write back frozen value if it drifts)
            if (curStatus == 1 || curStatus == 3) {
                if (curTimer != frozenTimer) { SafeWriteMemory(tAddr,&frozenTimer,sizeof(int)); }
            } else if (curStatus == 4) {
                // Awakening: treat as top-up (if game decrements below target, push back up)
                if (curTimer < frozenTimer) { SafeWriteMemory(tAddr,&frozenTimer,sizeof(int)); }
            }
        }; enforceMai(1); enforceMai(2);

        // Minagi – Always Readied: when Minagi is selected and Michiru puppet is idle & unreadied (ID 400), set to 401 (readied).
        auto enforceMinagiReadied = [&](int pi){
            bool isMinagi = (pi==1)?(localData.p1CharID==CHAR_ID_MINAGI):(localData.p2CharID==CHAR_ID_MINAGI);
            bool wantReadied = (pi==1)?localData.p1MinagiAlwaysReadied:localData.p2MinagiAlwaysReadied;
            if (!(isMinagi && wantReadied)) return;
            const int off = (pi==1)?EFZ_BASE_OFFSET_P1:EFZ_BASE_OFFSET_P2;
            uintptr_t playerBase = 0; SafeReadMemory(base + off, &playerBase, sizeof(playerBase)); if (!playerBase) return;
            for (int i=0;i<MINAGI_PUPPET_SLOT_MAX_SCAN;i++) {
                uintptr_t slot = playerBase + MINAGI_PUPPET_SLOTS_BASE + (uintptr_t)i*MINAGI_PUPPET_SLOT_STRIDE;
                uint16_t id=0; if (!SafeReadMemory(slot + MINAGI_PUPPET_SLOT_ID_OFFSET, &id, sizeof(id))) continue;
                if (id == MINAGI_PUPPET_ENTITY_ID) {
                    // ID 400 = unreadied idle. If currently idle-ish, promote to 401 (readied) without freezing animation.
                    uint16_t frame=0, sub=0; SafeReadMemory(slot + MINAGI_PUPPET_SLOT_FRAME_OFFSET,&frame,sizeof(frame)); SafeReadMemory(slot + MINAGI_PUPPET_SLOT_SUBFRAME_OFFSET,&sub,sizeof(sub));
                    if (frame <= 1) {
                        uint16_t readyId = 401; SafeWriteMemory(slot + MINAGI_PUPPET_SLOT_ID_OFFSET, &readyId, sizeof(readyId));
                    }
                    break;
                }
            }
        }; enforceMinagiReadied(1); enforceMinagiReadied(2);

        // Minagi – Debug: Convert select Minagi projectiles to Michiru (ID 400) for non-character entities.
        // Implementation: sweep the slot array and rewrite ID on entries where id != 0/400/401.
        // Gate strictly by the requested move ID groups only:
        //  - 429–432 (41236)
        //  - 440–443 (bubble: 641236)
        //  - 447–452 (stars)
        //  - 453     (airthrow animation)
        //  - 463–465 (236236 particles)
        if (localData.minagiConvertNewProjectiles) {
            auto convertSlots = [&](int pi){
                bool isMinagi = (pi==1)?(localData.p1CharID==CHAR_ID_MINAGI):(localData.p2CharID==CHAR_ID_MINAGI);
                if (!isMinagi) return;
                const int off = (pi==1)?EFZ_BASE_OFFSET_P1:EFZ_BASE_OFFSET_P2;
                uintptr_t playerBase = 0; SafeReadMemory(base + off, &playerBase, sizeof(playerBase)); if (!playerBase) return;
                short moveId=0; if (auto mv = ResolvePointer(base, off, MOVE_ID_OFFSET)) SafeReadMemory(mv,&moveId,sizeof(moveId));
                // Only act during the specified animations to avoid touching character core states
                auto isConversionMove = [](short mv)->bool {
                    return (mv >= 429 && mv <= 432) ||
                           (mv >= 440 && mv <= 443) ||
                           (mv >= 447 && mv <= 452) ||
                           (mv == 453) ||
                           (mv >= 463 && mv <= 465);
                };
                if (!isConversionMove(moveId)) return;
                for (int i=0;i<MINAGI_PUPPET_SLOT_MAX_SCAN;i++) {
                    uintptr_t slot = playerBase + MINAGI_PUPPET_SLOTS_BASE + (uintptr_t)i*MINAGI_PUPPET_SLOT_STRIDE;
                    uint16_t id=0; if (!SafeReadMemory(slot + MINAGI_PUPPET_SLOT_ID_OFFSET, &id, sizeof(id))) continue;
                    if (id == 0) continue; // empty slot
                    if (id == MINAGI_PUPPET_ENTITY_ID) continue; // already Michiru (400)
                    if (id == 401) continue; // readied Michiru stays readied; do not downgrade to 400
                    // Reassign to Michiru entity
                    uint16_t newId = (uint16_t)MINAGI_PUPPET_ENTITY_ID; SafeWriteMemory(slot + MINAGI_PUPPET_SLOT_ID_OFFSET, &newId, sizeof(newId));
                }
            }; convertSlots(1); convertSlots(2);
        }

        // Minagi – Position override (one-shot): write to Michiru slot only when Apply is pressed
        auto enforceMinagiPos = [&](int pi){
            bool isMinagi = (pi==1)?(localData.p1CharID==CHAR_ID_MINAGI):(localData.p2CharID==CHAR_ID_MINAGI);
            if (!isMinagi) return;
            double setX = (pi==1)?localData.p1MinagiPuppetSetX:localData.p2MinagiPuppetSetX;
            double setY = (pi==1)?localData.p1MinagiPuppetSetY:localData.p2MinagiPuppetSetY;
            bool applyNow = (pi==1)?localData.p1MinagiApplyPos:localData.p2MinagiApplyPos;
            if (!applyNow) return;
            if (std::isnan(setX) || std::isnan(setY)) return;
            const int off = (pi==1)?EFZ_BASE_OFFSET_P1:EFZ_BASE_OFFSET_P2;
            uintptr_t playerBase = 0; SafeReadMemory(base + off, &playerBase, sizeof(playerBase)); if (!playerBase) return;
            for (int i=0;i<MINAGI_PUPPET_SLOT_MAX_SCAN;i++) {
                uintptr_t slot = playerBase + MINAGI_PUPPET_SLOTS_BASE + (uintptr_t)i*MINAGI_PUPPET_SLOT_STRIDE;
                uint16_t id=0; if (!SafeReadMemory(slot + MINAGI_PUPPET_SLOT_ID_OFFSET, &id, sizeof(id))) continue;
                if (id == 0) continue;
                if (id == MINAGI_PUPPET_ENTITY_ID || id == 401) {
                    SafeWriteMemory(slot + MINAGI_PUPPET_SLOT_X_OFFSET,&setX,sizeof(double));
                    SafeWriteMemory(slot + MINAGI_PUPPET_SLOT_Y_OFFSET,&setY,sizeof(double));
                    // Clear the apply flag (one-shot semantics)
                    if (pi==1) { const_cast<DisplayData&>(localData).p1MinagiApplyPos = false; }
                    else { const_cast<DisplayData&>(localData).p2MinagiApplyPos = false; }
                    break;
                }
            }
        }; enforceMinagiPos(1); enforceMinagiPos(2);

    }
}