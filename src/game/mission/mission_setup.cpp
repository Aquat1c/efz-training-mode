#include "../../../include/game/mission/mission_setup.h"
#include "../../../include/game/character_settings.h"
#include "../../../include/game/character_hotswap.h"
#include "../../../include/core/constants.h"
#include "../../../include/core/logger.h"
#include "../../../include/core/memory.h"
#include "../../../include/utils/utilities.h"
#include "../../../include/utils/bgm_control.h"
#include "../../../include/game/game_state.h"

#include <windows.h>

#include <cstring>

namespace Mission::Setup {

namespace {

bool ResourceNamesEqual(const std::string& lhs, const std::string& rhs) {
    return _stricmp(lhs.c_str(), rhs.c_str()) == 0;
}

// ---- character-specific resources <-> displayData ----------------------------
// Small curated keys, stable across saves; extend as new resources matter.
// Capture reads displayData (kept fresh by the frame monitor); Apply writes the
// displayData fields then lets CharacterSettings::ApplyCharacterValues push them.

void CaptureResources(int charId, bool p1, std::map<std::string, int>& out) {
    DisplayData& d = displayData;
    switch (charId) {
        case CHAR_ID_MISHIO:
            out["element"] = p1 ? d.p1MishioElement : d.p2MishioElement; break;
        case CHAR_ID_MIO:
            out["stance"] = p1 ? d.p1MioStance : d.p2MioStance; break;
        case CHAR_ID_KANO:
            out["magic"] = p1 ? d.p1KanoMagic : d.p2KanoMagic; break;
        case CHAR_ID_MAI:
            out["status"]     = p1 ? d.p1MaiStatus : d.p2MaiStatus;
            out["ghostTime"]  = p1 ? d.p1MaiGhostTime : d.p2MaiGhostTime;
            out["ghostCharge"]= p1 ? d.p1MaiGhostCharge : d.p2MaiGhostCharge;
            break;
        case CHAR_ID_IKUMI:
            out["blood"]      = p1 ? d.p1IkumiBlood : d.p2IkumiBlood;
            out["genocide"]   = p1 ? d.p1IkumiGenocide : d.p2IkumiGenocide;
            out["levelGauge"] = p1 ? d.p1IkumiLevelGauge : d.p2IkumiLevelGauge;
            break;
        case CHAR_ID_MISUZU:
            out["feathers"]    = p1 ? d.p1MisuzuFeathers : d.p2MisuzuFeathers;
            out["poisonTimer"] = p1 ? d.p1MisuzuPoisonTimer : d.p2MisuzuPoisonTimer;
            out["poisonLevel"] = p1 ? d.p1MisuzuPoisonLevel : d.p2MisuzuPoisonLevel;
            break;
        case CHAR_ID_NANASE:
            out["barehanded"]  = (p1 ? d.p1RumiBarehanded : d.p2RumiBarehanded) ? 1 : 0;
            out["kimchiTimer"] = p1 ? d.p1RumiKimchiTimer : d.p2RumiKimchiTimer;
            break;
        case CHAR_ID_AKIKO:
            out["bulletCycle"] = p1 ? d.p1AkikoBulletCycle : d.p2AkikoBulletCycle;
            break;
        case CHAR_ID_NAYUKI:   // sleepy Nayuki (Neyuki) - jam stock
            out["jam"] = p1 ? d.p1NeyukiJamCount : d.p2NeyukiJamCount;
            break;
        default: break;
    }
}

void ApplyResources(int charId, bool p1, const std::map<std::string, int>& res) {
    if (res.empty()) return;
    DisplayData& d = displayData;
    auto get = [&](const char* k, int cur) {
        auto it = res.find(k);
        return it != res.end() ? it->second : cur;
    };
    switch (charId) {
        case CHAR_ID_MISHIO:
            (p1 ? d.p1MishioElement : d.p2MishioElement) = get("element", p1 ? d.p1MishioElement : d.p2MishioElement);
            break;
        case CHAR_ID_MIO:
            (p1 ? d.p1MioStance : d.p2MioStance) = get("stance", p1 ? d.p1MioStance : d.p2MioStance);
            break;
        case CHAR_ID_KANO:
            (p1 ? d.p1KanoMagic : d.p2KanoMagic) = get("magic", p1 ? d.p1KanoMagic : d.p2KanoMagic);
            break;
        case CHAR_ID_MAI:
            (p1 ? d.p1MaiStatus : d.p2MaiStatus)           = get("status", p1 ? d.p1MaiStatus : d.p2MaiStatus);
            (p1 ? d.p1MaiGhostTime : d.p2MaiGhostTime)     = get("ghostTime", p1 ? d.p1MaiGhostTime : d.p2MaiGhostTime);
            (p1 ? d.p1MaiGhostCharge : d.p2MaiGhostCharge) = get("ghostCharge", p1 ? d.p1MaiGhostCharge : d.p2MaiGhostCharge);
            break;
        case CHAR_ID_IKUMI:
            (p1 ? d.p1IkumiBlood : d.p2IkumiBlood)           = get("blood", p1 ? d.p1IkumiBlood : d.p2IkumiBlood);
            (p1 ? d.p1IkumiGenocide : d.p2IkumiGenocide)     = get("genocide", p1 ? d.p1IkumiGenocide : d.p2IkumiGenocide);
            (p1 ? d.p1IkumiLevelGauge : d.p2IkumiLevelGauge) = get("levelGauge", p1 ? d.p1IkumiLevelGauge : d.p2IkumiLevelGauge);
            break;
        case CHAR_ID_MISUZU:
            (p1 ? d.p1MisuzuFeathers : d.p2MisuzuFeathers)       = get("feathers", p1 ? d.p1MisuzuFeathers : d.p2MisuzuFeathers);
            (p1 ? d.p1MisuzuPoisonTimer : d.p2MisuzuPoisonTimer) = get("poisonTimer", p1 ? d.p1MisuzuPoisonTimer : d.p2MisuzuPoisonTimer);
            (p1 ? d.p1MisuzuPoisonLevel : d.p2MisuzuPoisonLevel) = get("poisonLevel", p1 ? d.p1MisuzuPoisonLevel : d.p2MisuzuPoisonLevel);
            break;
        case CHAR_ID_NANASE:
            (p1 ? d.p1RumiBarehanded : d.p2RumiBarehanded) = get("barehanded", (p1 ? d.p1RumiBarehanded : d.p2RumiBarehanded) ? 1 : 0) != 0;
            (p1 ? d.p1RumiKimchiTimer : d.p2RumiKimchiTimer) = get("kimchiTimer", p1 ? d.p1RumiKimchiTimer : d.p2RumiKimchiTimer);
            break;
        case CHAR_ID_AKIKO:
            (p1 ? d.p1AkikoBulletCycle : d.p2AkikoBulletCycle) = get("bulletCycle", p1 ? d.p1AkikoBulletCycle : d.p2AkikoBulletCycle);
            break;
        case CHAR_ID_NAYUKI:
            (p1 ? d.p1NeyukiJamCount : d.p2NeyukiJamCount) = get("jam", p1 ? d.p1NeyukiJamCount : d.p2NeyukiJamCount);
            break;
        default: break;
    }
}

// ---- deferred apply state ----------------------------------------------------
bool  g_pending = false;
int   g_pendingTimeout = 0;
::Mission::Mission g_pendingMission;   // copy of the mission whose setup to apply

// Write the value block (positions/HP/meter/RF/IC/resources) - characters must
// already be correct.
void ApplyValues(const ::Mission::Mission& m) {
    uintptr_t base = GetEFZBase();
    if (!base) return;

    // Positions (0 = keep default).
    if (m.player.posX != 0.0 || m.player.posY != 0.0)
        SetPlayerPosition(base, EFZ_BASE_OFFSET_P1, m.player.posX, m.player.posY, true);
    if (m.dummy.posX != 0.0 || m.dummy.posY != 0.0)
        SetPlayerPosition(base, EFZ_BASE_OFFSET_P2, m.dummy.posX, m.dummy.posY, true);

    // HP / meter (direct writes; only the fields the mission specifies).
    auto writeInt = [&](uintptr_t off, uintptr_t field, int v) {
        uintptr_t addr = ResolvePointer(base, off, field);
        if (addr) SafeWriteMemory(addr, &v, sizeof(int));
    };
    if (m.player.hp >= 0)    writeInt(EFZ_BASE_OFFSET_P1, HP_OFFSET, m.player.hp);
    if (m.dummy.hp >= 0)     writeInt(EFZ_BASE_OFFSET_P2, HP_OFFSET, m.dummy.hp);
    if (m.player.meter >= 0) writeInt(EFZ_BASE_OFFSET_P1, METER_OFFSET, m.player.meter);
    if (m.dummy.meter >= 0)  writeInt(EFZ_BASE_OFFSET_P2, METER_OFFSET, m.dummy.meter);

    // RF (helper handles both players + engine regen params).
    if (m.player.rf >= 0 || m.dummy.rf >= 0) {
        const double rf1 = m.player.rf >= 0 ? static_cast<double>(m.player.rf) : displayData.rf1;
        const double rf2 = m.dummy.rf  >= 0 ? static_cast<double>(m.dummy.rf)  : displayData.rf2;
        SetRFValuesDirect(rf1, rf2);
    }

    // IC color.
    if (m.player.blueIC >= 0) SetICColorPlayer(1, m.player.blueIC != 0);
    if (m.dummy.blueIC  >= 0) SetICColorPlayer(2, m.dummy.blueIC != 0);

    // Guard gauge (float 0..360).
    auto writeGuard = [&](uintptr_t off, int v) {
        if (v < 0) return;
        uintptr_t addr = ResolvePointer(base, off, PLAYER_GUARD_GAUGE_OFFSET);
        float f = static_cast<float>(v);
        if (addr) SafeWriteMemory(addr, &f, sizeof(f));
    };
    writeGuard(EFZ_BASE_OFFSET_P1, m.player.guard);
    writeGuard(EFZ_BASE_OFFSET_P2, m.dummy.guard);

    // Character-specific resources -> displayData -> game.
    ApplyResources(displayData.p1CharID, true, m.player.resources);
    ApplyResources(displayData.p2CharID, false, m.dummy.resources);
    if (!m.player.resources.empty() || !m.dummy.resources.empty()) {
        CharacterSettings::ApplyCharacterValues(base, displayData);
    }

    LogOut("[MISSION][SETUP] values applied (" + m.name + ")", true);
}

} // namespace

void Capture(::Mission::Mission& m) {
    const DisplayData& d = displayData;
    m.player.character = CharacterSettings::GetCharacterInternalName(d.p1CharID);
    m.dummy.character  = CharacterSettings::GetCharacterInternalName(d.p2CharID);
    CharacterHotswap::PaletteSelection palette{};
    if (CharacterHotswap::ReadCurrentPaletteSelection(palette)) {
        m.player.palette = palette.p1Color;
        m.dummy.palette = palette.p2Color;
    }
    m.player.posX = d.x1; m.player.posY = d.y1;
    m.dummy.posX  = d.x2; m.dummy.posY  = d.y2;
    m.player.hp = d.hp1;       m.dummy.hp = d.hp2;
    m.player.meter = d.meter1; m.dummy.meter = d.meter2;
    m.player.rf = static_cast<int>(d.rf1);
    m.dummy.rf  = static_cast<int>(d.rf2);
    m.player.blueIC = d.p1BlueIC ? 1 : 0;
    m.dummy.blueIC  = d.p2BlueIC ? 1 : 0;
    m.player.resources.clear();
    m.dummy.resources.clear();
    CaptureResources(d.p1CharID, true, m.player.resources);
    CaptureResources(d.p2CharID, false, m.dummy.resources);

    // Guard gauge (player+0x134, float 0..360).
    uintptr_t base = GetEFZBase();
    if (base) {
        auto readGuard = [&](uintptr_t off) -> int {
            uintptr_t addr = ResolvePointer(base, off, PLAYER_GUARD_GAUGE_OFFSET);
            float v = -1.0f;
            if (addr && SafeReadMemory(addr, &v, sizeof(v)) && v >= 0.0f) return static_cast<int>(v);
            return -1;
        };
        m.player.guard = readGuard(EFZ_BASE_OFFSET_P1);
        m.dummy.guard  = readGuard(EFZ_BASE_OFFSET_P2);
    }

    // Stage + logical BGM track. GameSystem+0xF26 is a DirectSound buffer index,
    // not the track number accepted by PlayBGM, so recording that value produced
    // invalid mission metadata and reload loops. The audio entry-point tracker is
    // the only authoritative source; unknown remains -1 (native stage music).
    const uintptr_t gameStatePtr = GetGameStatePtr();
    if (gameStatePtr) {
        uint8_t stage = 0;
        if (SafeReadMemory(gameStatePtr + 3890, &stage, sizeof(stage))) {
            m.stage = static_cast<int>(stage);
        }
        const unsigned short bgmTrack = GetLastBgmTrack();
        const int nativeStageTrack = m.stage >= 0 ? 10 + m.stage : -1;
        m.bgm = bgmTrack == 0xFFFFu || static_cast<int>(bgmTrack) == nativeStageTrack
            ? -1 : static_cast<int>(bgmTrack);
    }
    LogOut("[MISSION][SETUP] captured " + m.player.character + " vs " + m.dummy.character +
           " stage=" + std::to_string(m.stage) + " bgm=" + std::to_string(m.bgm), true);
}

bool Apply(const ::Mission::Mission& m, bool applyValues, bool forceFreshMatch,
           bool allowReload, bool acceptPreparedSession) {
    const std::string currentP1Resource =
        CharacterSettings::GetCharacterInternalName(displayData.p1CharID);
    const std::string currentP2Resource =
        CharacterSettings::GetCharacterInternalName(displayData.p2CharID);
    const bool wantsP1 = !m.player.character.empty();
    const bool wantsP2 = !m.dummy.character.empty();
    const int wantP1SelectId = wantsP1
        ? CharacterHotswap::GetSelectIdForResourceName(m.player.character.c_str()) : -1;
    const int wantP2SelectId = wantsP2
        ? CharacterHotswap::GetSelectIdForResourceName(m.dummy.character.c_str()) : -1;
    bool characterMismatch =
        (wantsP1 && !ResourceNamesEqual(m.player.character, currentP1Resource)) ||
        (wantsP2 && !ResourceNamesEqual(m.dummy.character, currentP2Resource));

    int liveStage = -1;
    const uintptr_t gameStatePtr = GetGameStatePtr();
    if (gameStatePtr) {
        uint8_t stage = 0;
        if (SafeReadMemory(gameStatePtr + 3890, &stage, sizeof(stage))) {
            liveStage = static_cast<int>(stage);
        }
    }
    bool stageMismatch = m.stage >= 0 && m.stage != liveStage;
    const unsigned short observedBgmTrack = GetLastBgmTrack();
    const int liveBgm = observedBgmTrack == 0xFFFFu
        ? -1 : static_cast<int>(observedBgmTrack);
    bool bgmNeedsApply = m.bgm >= 0 && m.bgm != liveBgm;

    CharacterHotswap::PaletteSelection requestedPalette{};
    requestedPalette.p1Color = m.player.palette;
    requestedPalette.p2Color = m.dummy.palette;
    const int resolvedP1SelectId = wantsP1 ? wantP1SelectId
        : CharacterHotswap::GetSelectIdForResourceName(currentP1Resource.c_str());
    const int resolvedP2SelectId = wantsP2 ? wantP2SelectId
        : CharacterHotswap::GetSelectIdForResourceName(currentP2Resource.c_str());
    if (resolvedP1SelectId >= 0 && resolvedP2SelectId >= 0) {
        CharacterHotswap::SanitizePaletteSelection(
            resolvedP1SelectId, resolvedP2SelectId, requestedPalette);
    }
    CharacterHotswap::PaletteSelection livePalette{};
    const bool haveLivePalette = CharacterHotswap::ReadCurrentPaletteSelection(livePalette);
    bool paletteMismatch = haveLivePalette &&
        (livePalette.p1Color != requestedPalette.p1Color ||
         livePalette.p2Color != requestedPalette.p2Color ||
         livePalette.p1UseCustomPalette != requestedPalette.p1UseCustomPalette ||
         livePalette.p2UseCustomPalette != requestedPalette.p2UseCustomPalette);

    const int requestedStage = m.stage >= 0 ? m.stage : (liveStage >= 0 ? liveStage : 0);
    const bool acceptedPreparedSession = acceptPreparedSession && !forceFreshMatch &&
        resolvedP1SelectId >= 0 && resolvedP2SelectId >= 0 &&
        CharacterHotswap::ConsumeCompletedPracticeLoad(
            resolvedP1SelectId, resolvedP2SelectId, requestedStage,
            requestedPalette, m.bgm);
    if (acceptedPreparedSession) {
        // Completion is an atomic receipt for the direct/selector transaction.
        // It is stronger than first-Match-tick displayData and palette reads.
        characterMismatch = false;
        stageMismatch = false;
        paletteMismatch = false;
        bgmNeedsApply = false;
        LogOut("[MISSION][SETUP] accepted completed Practice launch receipt", true);
    }

    // Music is presentation state, not match construction state. A track change
    // can be applied safely in place and must never tear down/recreate fighters.
    const bool needFreshMatch = forceFreshMatch || characterMismatch || stageMismatch ||
                                paletteMismatch;

    if (needFreshMatch) {
        if (!allowReload) {
            LogOut("[MISSION][SETUP] in-place restore rejected because the prepared session no longer matches",
                   true);
            return false;
        }
        const int p1SelectId = resolvedP1SelectId;
        const int p2SelectId = resolvedP2SelectId;
        if (p1SelectId < 0 || p2SelectId < 0) {
            LogOut("[MISSION][SETUP] reload rejected: unresolved resource-to-select mapping p1='" +
                   (wantsP1 ? m.player.character : currentP1Resource) + "' p2='" +
                   (wantsP2 ? m.dummy.character : currentP2Resource) + "'", true);
            return false;
        }

        const int stage = requestedStage;
        const int bgm = m.bgm;
        LogOut("[MISSION][SETUP] fresh match required current=" + currentP1Resource + "/" +
               currentP2Resource + " requested=" +
               (wantsP1 ? m.player.character : currentP1Resource) + "/" +
               (wantsP2 ? m.dummy.character : currentP2Resource) + " select=" +
               std::to_string(p1SelectId) + "/" + std::to_string(p2SelectId) +
               " reasons=force:" + std::to_string(forceFreshMatch ? 1 : 0) +
               " chars:" + std::to_string(characterMismatch ? 1 : 0) +
               " stage:" + std::to_string(stageMismatch ? 1 : 0) +
               " palette:" + std::to_string(paletteMismatch ? 1 : 0) +
                " bgmInPlace:" + std::to_string(bgmNeedsApply ? 1 : 0), true);

        bool queued = CharacterHotswap::QueueDirectPracticeLoad(
            p1SelectId, p2SelectId, stage, requestedPalette, bgm);
        if (!queued && CharacterHotswap::CanQueueReload()) {
            queued = CharacterHotswap::QueueReload(
                p1SelectId, p2SelectId, stage, requestedPalette, bgm);
        }
        if (queued) {
            if (applyValues) {
                g_pending = true;
                g_pendingTimeout = 0;
                g_pendingMission = m;
                LogOut("[MISSION][SETUP] native reload queued -> values deferred", true);
            } else {
                g_pending = false;
                LogOut("[MISSION][SETUP] native reload queued (values left to state restore)", true);
            }
            return true;
        }
        LogOut("[MISSION][SETUP] reload queue failed; refusing to apply setup to the wrong session", true);
        return false;
    }
    if (bgmNeedsApply) {
        if (gameStatePtr && PlayBGM(gameStatePtr, static_cast<unsigned short>(m.bgm))) {
            LogOut("[MISSION][SETUP] applied BGM track in place=" +
                   std::to_string(m.bgm), true);
        } else {
            // BGM is non-authoritative presentation. A failed music request must
            // not invalidate an otherwise deterministic mission start state.
            LogOut("[MISSION][SETUP] could not apply BGM track in place=" +
                   std::to_string(m.bgm), true);
        }
    }
    if (applyValues) ApplyValues(m);
    return true;
}

void Tick() {
    if (!g_pending) return;
    if (++g_pendingTimeout > 60 * 30) {   // ~30s safety
        g_pending = false;
        LogOut("[MISSION][SETUP] deferred apply timed out", true);
        return;
    }
    if (CharacterHotswap::IsBusy()) return;
    if (GetCurrentGamePhase() != GamePhase::Match) return;
    // Hotswap done and we're in a match: push the values.
    g_pending = false;
    ApplyValues(g_pendingMission);
}

bool IsPending() { return g_pending; }

} // namespace Mission::Setup
