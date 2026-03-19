#include "../include/game/combo_overlay.h"

#include "../include/core/constants.h"
#include "../include/core/logger.h"
#include "../include/core/memory.h"
#include "../include/game/collision_hook.h"
#include "../include/gui/imgui_impl.h"
#include "../include/gui/overlay.h"
#include "../include/utils/config.h"
#include "../include/utils/utilities.h"
#include "../include/utils/xp_compat.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>

namespace {
    constexpr uintptr_t COMBO_COUNT_OFFSET = 0xF4;
    constexpr uintptr_t COMBO_SCALE_DISPLAY_OFFSET = 0xF8;
    constexpr uintptr_t COMBO_DAMAGE_OFFSET = 0x100;
    constexpr uintptr_t COMBO_TIMER_OFFSET = 0x104;
    constexpr uintptr_t COMBO_SCALE_RAW_OFFSET = 0x178;
    constexpr uintptr_t GAME_DATA_PTR_OFFSET = 0x7C;
    constexpr uintptr_t DIFFICULTY_SETTING_OFFSET = 4964;
    constexpr uintptr_t RF_MODE_SETTING_OFFSET = 4976;

    constexpr uintptr_t ATTACK_FLAGS_OFFSET = 0xAA;
    constexpr uintptr_t ATTACK_DAMAGE_OFFSET = 0xA8;
    constexpr uintptr_t ATTACK_BLOCKSTUN_OFFSET = 0xB2;
    constexpr uintptr_t ATTACK_HITSTOP_OFFSET = 0xC2;
    // Live hitstun is clamped every frame at player + 328 in efz.exe.c.
    // It is not adjacent to the blockstun counter on the runtime player object.
    constexpr uintptr_t HITSTUN_OFFSET = 0x148;
    constexpr uint32_t REALTIME_REFRESH_INTERVAL_TICKS = 12; // 192 Hz / 12 = 16 Hz

    constexpr int OVERLAY_X = 244;
    constexpr int OVERLAY_Y = 92;
    constexpr int LINE_HEIGHT = 15;
    constexpr unsigned char OVERLAY_BG_ALPHA = 84;

    struct ComboRuntime {
        bool valid = false;
        int hitCount = 0;
        int totalDamage = 0;
        int timer = 0;
        double scalePercent = 100.0;
        double scaleRaw = 10000.0;
    };

    struct PlayerMetrics {
        bool valid = false;
        int hp = 0;
        int meter = 0;
        double rf = 0.0;
        int hitstun = 0;
        int untech = 0;
        short moveId = 0;
    };

    struct AttackDetail {
        bool valid = false;
        int damage = 0;
        int blockstun = 0;
        int hitstop = 0;
        int flags = 0;
    };

    struct PreviousPlayerFrame {
        bool valid = false;
        int hp = 0;
        int meter = 0;
        double rf = 0.0;
    };

    struct ComboState {
        bool live = false;
        bool finalized = false;
        bool hiddenByPolicy = false;

        int attackerSide = 0;
        int defenderSide = 0;

        int hitCount = 0;
        int totalDamage = 0;
        int lastHitDamage = 0;
        int comboTimer = 0;

        int defenderHpStart = 0;
        int defenderHpCurrent = 0;
        int defenderHpDelta = 0;

        int attackerMeterStart = 0;
        int attackerMeterCurrent = 0;
        int attackerMeterDelta = 0;

        double attackerRfStart = 0.0;
        double attackerRfCurrent = 0.0;
        double attackerRfDelta = 0.0;

        int defenderMeterStart = 0;
        int defenderMeterCurrent = 0;
        int defenderMeterDelta = 0;

        double defenderRfStart = 0.0;
        double defenderRfCurrent = 0.0;
        double defenderRfDelta = 0.0;

        short attackerMoveId = 0;
        short defenderMoveId = 0;

        int defenderHitstun = 0;
        int defenderUntech = 0;

        double scalePercent = 100.0;
        double scaleRaw = 10000.0;
        double rfMultiplier = 1.0;

        AttackDetail attackDetail{};

        unsigned long long lingerUntilMs = 0;
        uint32_t startFrame = 0;
        uint32_t lastUpdateFrame = 0;
    };

    struct DisplayIds {
        int header = -1;
        int totals = -1;
        int resources = -1;
        int detail = -1;
        int status = -1;
    };

    std::mutex g_comboOverlayMutex;
    ComboState g_state{};
    DisplayIds g_displayIds{};
    PreviousPlayerFrame g_prevPlayers[3]{};
    int g_frameDataAttackOffsets[3] = {-1, -1, -1};
    int g_sessionMaxComboDamage = 0;

    bool ReadComboRuntime(uintptr_t playerPtr, ComboRuntime& out) {
        if (!playerPtr) {
            return false;
        }

        int hitCount = 0;
        int totalDamage = 0;
        int timer = 0;
        double scaleDisplayRaw = 100.0;
        double scaleRaw = 10000.0;
        if (!SafeReadMemory(playerPtr + COMBO_COUNT_OFFSET, &hitCount, sizeof(hitCount))) return false;
        if (!SafeReadMemory(playerPtr + COMBO_DAMAGE_OFFSET, &totalDamage, sizeof(totalDamage))) return false;
        SafeReadMemory(playerPtr + COMBO_TIMER_OFFSET, &timer, sizeof(timer));
        SafeReadMemory(playerPtr + COMBO_SCALE_DISPLAY_OFFSET, &scaleDisplayRaw, sizeof(scaleDisplayRaw));
        SafeReadMemory(playerPtr + COMBO_SCALE_RAW_OFFSET, &scaleRaw, sizeof(scaleRaw));

        out.valid = true;
        out.hitCount = (std::max)(0, hitCount);
        out.totalDamage = (std::max)(0, totalDamage);
        out.timer = (std::max)(0, timer);
        out.scaleRaw = (std::isfinite(scaleRaw) && scaleRaw > 0.0) ? scaleRaw : 10000.0;
        out.scalePercent = (std::isfinite(scaleDisplayRaw) && scaleDisplayRaw > 0.0)
            ? scaleDisplayRaw
            : (out.scaleRaw / 100.0);
        if (!std::isfinite(out.scalePercent) || out.scalePercent <= 0.0) {
            out.scalePercent = out.scaleRaw / 100.0;
        }
        return true;
    }

    bool ReadPlayerMetrics(uintptr_t playerPtr, PlayerMetrics& out) {
        if (!playerPtr) {
            return false;
        }

        unsigned short meter = 0;
        short hitstun = 0;
        short untech = 0;

        if (!SafeReadMemory(playerPtr + HP_OFFSET, &out.hp, sizeof(out.hp))) return false;
        if (!SafeReadMemory(playerPtr + METER_OFFSET, &meter, sizeof(meter))) return false;
        if (!SafeReadMemory(playerPtr + RF_OFFSET, &out.rf, sizeof(out.rf))) return false;
        SafeReadMemory(playerPtr + HITSTUN_OFFSET, &hitstun, sizeof(hitstun));
        SafeReadMemory(playerPtr + UNTECH_OFFSET, &untech, sizeof(untech));
        SafeReadMemory(playerPtr + MOVE_ID_OFFSET, &out.moveId, sizeof(out.moveId));

        out.valid = true;
        out.meter = static_cast<int>(meter);
        out.hitstun = (std::max)(0, static_cast<int>(hitstun));
        out.untech = (std::max)(0, static_cast<int>(untech));
        return true;
    }

    bool IsPlausibleAttackDataPtr(uintptr_t attackPtr) {
        if (!attackPtr) return false;
        if (attackPtr < 0x00400000 || attackPtr > 0x0FFFFFFF) return false;

        uint8_t flags = 0;
        uint16_t damage = 0;
        uint16_t blockstun = 0;
        uint16_t hitstop = 0;
        if (!SafeReadMemory(attackPtr + ATTACK_FLAGS_OFFSET, &flags, sizeof(flags))) return false;
        if (!SafeReadMemory(attackPtr + ATTACK_DAMAGE_OFFSET, &damage, sizeof(damage))) return false;
        if (!SafeReadMemory(attackPtr + ATTACK_BLOCKSTUN_OFFSET, &blockstun, sizeof(blockstun))) return false;
        if (!SafeReadMemory(attackPtr + ATTACK_HITSTOP_OFFSET, &hitstop, sizeof(hitstop))) return false;
        return damage <= 5000 && blockstun <= 1000 && hitstop <= 1000;
    }

    uintptr_t ResolveAttackDataPtrFromFrameData(uintptr_t frameDataPtr, int attackerSide) {
        if (!frameDataPtr) {
            return 0;
        }

        if (attackerSide >= 1 && attackerSide <= 2) {
            int& cachedOffset = g_frameDataAttackOffsets[attackerSide];
            if (cachedOffset >= 0) {
                uintptr_t candidate = 0;
                if (SafeReadMemory(frameDataPtr + cachedOffset, &candidate, sizeof(candidate))
                    && IsPlausibleAttackDataPtr(candidate)) {
                    return candidate;
                }
                cachedOffset = -1;
            }
        }

        for (int off = 0; off <= 0x200 - 4; off += 4) {
            uintptr_t candidate = 0;
            if (!SafeReadMemory(frameDataPtr + off, &candidate, sizeof(candidate))) {
                continue;
            }
            if (IsPlausibleAttackDataPtr(candidate)) {
                if (attackerSide >= 1 && attackerSide <= 2) {
                    g_frameDataAttackOffsets[attackerSide] = off;
                }
                return candidate;
            }
        }
        return 0;
    }

    AttackDetail ReadAttackDetail(int attackerSide) {
        AttackDetail detail{};
        const uintptr_t frameDataPtr = GetCachedAttackDataForPlayer(attackerSide);
        const uintptr_t attackDataPtr = ResolveAttackDataPtrFromFrameData(frameDataPtr, attackerSide);
        if (!attackDataPtr) {
            return detail;
        }

        uint8_t flags = 0;
        uint16_t damage = 0;
        uint16_t blockstun = 0;
        uint16_t hitstop = 0;
        if (!SafeReadMemory(attackDataPtr + ATTACK_FLAGS_OFFSET, &flags, sizeof(flags))) return detail;
        if (!SafeReadMemory(attackDataPtr + ATTACK_DAMAGE_OFFSET, &damage, sizeof(damage))) return detail;
        if (!SafeReadMemory(attackDataPtr + ATTACK_BLOCKSTUN_OFFSET, &blockstun, sizeof(blockstun))) return detail;
        if (!SafeReadMemory(attackDataPtr + ATTACK_HITSTOP_OFFSET, &hitstop, sizeof(hitstop))) return detail;

        detail.valid = true;
        detail.flags = static_cast<int>(flags);
        detail.damage = static_cast<int>(damage);
        detail.blockstun = static_cast<int>(blockstun);
        detail.hitstop = static_cast<int>(hitstop);
        return detail;
    }

    double CalculateRfMultiplier(uintptr_t attackerPtr, double rfCurrent) {
        uintptr_t gameDataPtr = 0;
        if (!attackerPtr || !SafeReadMemory(attackerPtr + GAME_DATA_PTR_OFFSET, &gameDataPtr, sizeof(gameDataPtr)) || !gameDataPtr) {
            return 1.0 + (rfCurrent / 10000.0);
        }

        uint8_t difficultySetting = 0;
        uint8_t rfModeSetting = 0;
        SafeReadMemory(gameDataPtr + DIFFICULTY_SETTING_OFFSET, &difficultySetting, sizeof(difficultySetting));
        SafeReadMemory(gameDataPtr + RF_MODE_SETTING_OFFSET, &rfModeSetting, sizeof(rfModeSetting));

        const double rfBonus = rfCurrent / 10000.0;
        const double baseMultiplier = 1.0 + rfBonus;
        if (difficultySetting == 2) {
            return baseMultiplier;
        }
        if (rfModeSetting == 1) {
            return baseMultiplier / 2.0;
        }
        if (rfModeSetting == 2) {
            return baseMultiplier;
        }
        return baseMultiplier + rfBonus;
    }

    int ScoreRuntime(const ComboRuntime& runtime) {
        return (runtime.hitCount * 100000) + (runtime.totalDamage * 10) + runtime.timer;
    }

    bool IsComboVictimState(const PlayerMetrics& player) {
        return player.hitstun > 0 || player.untech > 0;
    }

    ComboRuntime BuildSyntheticRuntime(const ComboRuntime& hint, int hitCount, int totalDamage) {
        ComboRuntime runtime = hint;
        runtime.valid = true;
        runtime.hitCount = (std::max)(1, hitCount);
        runtime.totalDamage = (std::max)(0, totalDamage);
        if (!std::isfinite(runtime.scalePercent) || runtime.scalePercent <= 0.0) {
            runtime.scalePercent = 100.0;
        }
        if (!std::isfinite(runtime.scaleRaw) || runtime.scaleRaw <= 0.0) {
            runtime.scaleRaw = 10000.0;
        }
        runtime.timer = (std::max)(0, runtime.timer);
        return runtime;
    }

    std::string FormatMeterDelta(int delta) {
        std::ostringstream oss;
        oss << (delta >= 0 ? "+" : "-") << std::abs(delta);
        return oss.str();
    }

    std::string FormatRfDelta(double delta) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(1) << (delta >= 0.0 ? "+" : "-") << std::fabs(delta);
        return oss.str();
    }

    std::string FormatScalePercent(double value) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(3) << value << "%";
        return oss.str();
    }

    std::string FormatOptionalDamage(bool valid, int damage) {
        if (!valid || damage <= 0) {
            return "--";
        }
        return std::to_string(damage);
    }

    bool ComboProgressed(const ComboRuntime& runtime) {
        return runtime.hitCount > g_state.hitCount || runtime.totalDamage > g_state.totalDamage;
    }

    void RemoveMessage(int& id) {
        if (id != -1) {
            DirectDrawHook::RemovePermanentMessage(id);
            id = -1;
        }
    }

    void ClearDisplayUnlocked() {
        RemoveMessage(g_displayIds.header);
        RemoveMessage(g_displayIds.totals);
        RemoveMessage(g_displayIds.resources);
        RemoveMessage(g_displayIds.detail);
        RemoveMessage(g_displayIds.status);
    }

    void ResetPrevSamplesUnlocked() {
        g_prevPlayers[1] = PreviousPlayerFrame{};
        g_prevPlayers[2] = PreviousPlayerFrame{};
    }

    void ResetAttackDetailCacheUnlocked() {
        g_frameDataAttackOffsets[1] = -1;
        g_frameDataAttackOffsets[2] = -1;
    }

    void ResetSessionStatsUnlocked() {
        g_sessionMaxComboDamage = 0;
    }

    void ResetStateUnlocked(const char* reason, bool resetSessionStats) {
        const bool hadState = g_state.live || g_state.finalized || g_displayIds.header != -1
            || g_displayIds.totals != -1 || g_displayIds.resources != -1
            || g_displayIds.detail != -1 || g_displayIds.status != -1;

        ClearDisplayUnlocked();
        g_state = ComboState{};
        ResetPrevSamplesUnlocked();
        ResetAttackDetailCacheUnlocked();
        if (resetSessionStats) {
            ResetSessionStatsUnlocked();
        }

        if (hadState && (detailedLogging.load() || reason != nullptr)) {
            LogOut(std::string("[COMBO] Reset combo overlay state reason=") + (reason ? reason : "unspecified"), true);
        }
    }

    void UpdateSessionMaxUnlocked(int comboDamage) {
        if (comboDamage > g_sessionMaxComboDamage) {
            g_sessionMaxComboDamage = comboDamage;
        }
    }

    void UpsertMessage(int& id, const std::string& text, COLORREF color, int x, int y, unsigned char backgroundAlpha) {
        if (text.empty()) {
            RemoveMessage(id);
            return;
        }
        if (id == -1) {
            id = DirectDrawHook::AddPermanentMessage(text, color, x, y, backgroundAlpha);
        } else {
            DirectDrawHook::UpdatePermanentMessage(id, text, color);
        }
    }

    void StartNewComboUnlocked(
        const PerFrameSample& sample,
        int attackerSide,
        const ComboRuntime& runtime,
        const PlayerMetrics& attacker,
        const PlayerMetrics& defender,
        uintptr_t attackerPtr)
    {
        const int defenderSide = (attackerSide == 1) ? 2 : 1;
        const PreviousPlayerFrame& prevAttacker = g_prevPlayers[attackerSide];
        const PreviousPlayerFrame& prevDefender = g_prevPlayers[defenderSide];
        const int defenderHpStart = prevDefender.valid ? prevDefender.hp : defender.hp;
        const int effectiveTotalDamage = (std::max)(runtime.totalDamage, (std::max)(0, defenderHpStart - defender.hp));

        g_state = ComboState{};
        g_state.live = true;
        g_state.attackerSide = attackerSide;
        g_state.defenderSide = defenderSide;
        g_state.hitCount = runtime.hitCount;
        g_state.totalDamage = effectiveTotalDamage;
        g_state.lastHitDamage = effectiveTotalDamage;
        g_state.comboTimer = runtime.timer;
        g_state.defenderHpStart = defenderHpStart;
        g_state.defenderHpCurrent = defender.hp;
        g_state.defenderHpDelta = (std::max)(0, g_state.defenderHpStart - defender.hp);
        g_state.attackerMeterStart = prevAttacker.valid ? prevAttacker.meter : attacker.meter;
        g_state.attackerMeterCurrent = attacker.meter;
        g_state.attackerMeterDelta = attacker.meter - g_state.attackerMeterStart;
        g_state.attackerRfStart = prevAttacker.valid ? prevAttacker.rf : attacker.rf;
        g_state.attackerRfCurrent = attacker.rf;
        g_state.attackerRfDelta = attacker.rf - g_state.attackerRfStart;

        g_state.defenderMeterStart = prevDefender.valid ? prevDefender.meter : defender.meter;
        g_state.defenderMeterCurrent = defender.meter;
        g_state.defenderMeterDelta = defender.meter - g_state.defenderMeterStart;
        g_state.defenderRfStart = prevDefender.valid ? prevDefender.rf : defender.rf;
        g_state.defenderRfCurrent = defender.rf;
        g_state.defenderRfDelta = defender.rf - g_state.defenderRfStart;

        g_state.attackerMoveId = attacker.moveId;
        g_state.defenderMoveId = defender.moveId;
        g_state.defenderHitstun = defender.hitstun;
        g_state.defenderUntech = defender.untech;
        g_state.scalePercent = runtime.scalePercent;
        g_state.scaleRaw = runtime.scaleRaw;
        g_state.rfMultiplier = CalculateRfMultiplier(attackerPtr, attacker.rf);
        g_state.attackDetail = ReadAttackDetail(attackerSide);
        g_state.startFrame = sample.frame;
        g_state.lastUpdateFrame = sample.frame;
        UpdateSessionMaxUnlocked(g_state.totalDamage);

        if (detailedLogging.load()) {
            std::ostringstream oss;
            oss << "[COMBO] Started combo attacker=P" << attackerSide
                << " defender=P" << defenderSide
                << " hits=" << runtime.hitCount
                << " hpStart=" << g_state.defenderHpStart
                << " meterStart=" << g_state.attackerMeterStart
                << " rfStart=" << std::fixed << std::setprecision(1) << g_state.attackerRfStart;
            LogOut(oss.str(), true);
        }
    }

    void UpdateLiveComboUnlocked(
        const PerFrameSample& sample,
        const ComboRuntime& runtime,
        const PlayerMetrics& attacker,
        const PlayerMetrics& defender,
        uintptr_t attackerPtr)
    {
        const int previousTotalDamage = g_state.totalDamage;
        const int updatedTotalDamage = (std::max)(runtime.totalDamage, (std::max)(0, g_state.defenderHpStart - defender.hp));

        g_state.live = true;
        g_state.finalized = false;
        g_state.hitCount = runtime.hitCount;
        g_state.totalDamage = updatedTotalDamage;
        if (updatedTotalDamage > previousTotalDamage) {
            g_state.lastHitDamage = updatedTotalDamage - previousTotalDamage;
        }
        g_state.comboTimer = runtime.timer;
        g_state.defenderHpCurrent = defender.hp;
        g_state.defenderHpDelta = (std::max)(0, g_state.defenderHpStart - defender.hp);
        g_state.attackerMeterCurrent = attacker.meter;
        g_state.attackerMeterDelta = attacker.meter - g_state.attackerMeterStart;
        g_state.attackerRfCurrent = attacker.rf;
        g_state.attackerRfDelta = attacker.rf - g_state.attackerRfStart;

        g_state.defenderMeterCurrent = defender.meter;
        g_state.defenderMeterDelta = defender.meter - g_state.defenderMeterStart;
        g_state.defenderRfCurrent = defender.rf;
        g_state.defenderRfDelta = defender.rf - g_state.defenderRfStart;

        g_state.attackerMoveId = attacker.moveId;
        g_state.defenderMoveId = defender.moveId;
        g_state.defenderHitstun = defender.hitstun;
        g_state.defenderUntech = defender.untech;
        g_state.scalePercent = runtime.scalePercent;
        g_state.scaleRaw = runtime.scaleRaw;
        g_state.rfMultiplier = CalculateRfMultiplier(attackerPtr, attacker.rf);
        AttackDetail detail = ReadAttackDetail(g_state.attackerSide);
        if (detail.valid) {
            g_state.attackDetail = detail;
        }
        g_state.lastUpdateFrame = sample.frame;
        UpdateSessionMaxUnlocked(g_state.totalDamage);
    }

    void RefreshLiveStateUnlocked(const PerFrameSample& sample, const PlayerMetrics& attacker, const PlayerMetrics& defender) {
        g_state.defenderHpCurrent = defender.hp;
        g_state.defenderHitstun = defender.hitstun;
        g_state.defenderUntech = defender.untech;

        // Keep resource gains hit-locked, but apply resource spends immediately. This catches
        // BIC / super-style costs during an active combo without letting passive gains drift.
        if (attacker.meter < g_state.attackerMeterCurrent) {
            g_state.attackerMeterCurrent = attacker.meter;
            g_state.attackerMeterDelta = attacker.meter - g_state.attackerMeterStart;
        }
        if (attacker.rf < g_state.attackerRfCurrent) {
            g_state.attackerRfCurrent = attacker.rf;
            g_state.attackerRfDelta = attacker.rf - g_state.attackerRfStart;
        }
        if (defender.meter < g_state.defenderMeterCurrent) {
            g_state.defenderMeterCurrent = defender.meter;
            g_state.defenderMeterDelta = defender.meter - g_state.defenderMeterStart;
        }
        if (defender.rf < g_state.defenderRfCurrent) {
            g_state.defenderRfCurrent = defender.rf;
            g_state.defenderRfDelta = defender.rf - g_state.defenderRfStart;
        }

        g_state.lastUpdateFrame = sample.frame;
    }

    void FinalizeComboUnlocked(unsigned long long nowMs) {
        const Config::Settings& cfg = Config::GetSettings();
        if (!g_state.live) {
            return;
        }
        if (!cfg.comboOverlayShowFinalSummary) {
            ResetStateUnlocked("combo ended without linger", false);
            return;
        }

        g_state.live = false;
        g_state.finalized = true;
        const float duration = (cfg.comboOverlayDisplayDuration < 0.5f)
            ? 0.5f
            : (std::min)(cfg.comboOverlayDisplayDuration, 30.0f);
        g_state.lingerUntilMs = nowMs + static_cast<unsigned long long>(duration * 1000.0f);

        if (detailedLogging.load()) {
            std::ostringstream oss;
            oss << "[COMBO] Finalized combo attacker=P" << g_state.attackerSide
                << " defender=P" << g_state.defenderSide
                << " hits=" << g_state.hitCount
                << " damage=" << g_state.totalDamage
                << " lingerMs=" << duration * 1000.0f;
            LogOut(oss.str(), true);
        }
    }

    void RenderUnlocked(unsigned long long nowMs) {
        const Config::Settings& cfg = Config::GetSettings();
        if (!cfg.showComboStatisticsOverlay) {
            ClearDisplayUnlocked();
            return;
        }

        if (!g_state.live && !g_state.finalized) {
            ClearDisplayUnlocked();
            return;
        }

        const bool menuVisible = ImGuiImpl::IsVisible();
        if (cfg.comboOverlayHideWhenImGuiVisible && menuVisible) {
            ClearDisplayUnlocked();
            g_state.hiddenByPolicy = true;
            if (g_state.finalized && !cfg.comboOverlayResumeAfterImGui) {
                ResetStateUnlocked("hidden by imgui without resume", false);
            }
            return;
        }
        g_state.hiddenByPolicy = false;

        std::ostringstream line1;
        std::ostringstream line2;
        std::ostringstream line3;
        std::ostringstream line4;
        std::ostringstream line5;

        const bool compact = true;
        const bool showDetail = cfg.comboOverlayShowDetailRow;
        const bool showRfMultiplier = cfg.comboOverlayShowRfMultiplier;
        const bool showRawScale = cfg.comboOverlayShowRawScale;
        const int p1MeterDelta = (g_state.attackerSide == 1) ? g_state.attackerMeterDelta : g_state.defenderMeterDelta;
        const double p1RfDelta = (g_state.attackerSide == 1) ? g_state.attackerRfDelta : g_state.defenderRfDelta;
        const int p2MeterDelta = (g_state.attackerSide == 2) ? g_state.attackerMeterDelta : g_state.defenderMeterDelta;
        const double p2RfDelta = (g_state.attackerSide == 2) ? g_state.attackerRfDelta : g_state.defenderRfDelta;

        if (compact) {
            line1 << "Move " << FormatOptionalDamage(g_state.lastHitDamage > 0, g_state.lastHitDamage)
                  << "  Combo " << g_state.totalDamage
                  << "  Max " << g_sessionMaxComboDamage;
            line2 << "HP " << g_state.defenderHpStart << " > " << g_state.defenderHpCurrent
                  << " (-" << g_state.defenderHpDelta << ")";
            line3 << "P1   M " << FormatMeterDelta(p1MeterDelta)
                  << "   RF " << FormatRfDelta(p1RfDelta);
            line4 << "P2   M " << FormatMeterDelta(p2MeterDelta)
                  << "   RF " << FormatRfDelta(p2RfDelta);
        } else {
            line1 << "MOVE " << FormatOptionalDamage(g_state.lastHitDamage > 0, g_state.lastHitDamage)
                  << "  COMBO " << g_state.totalDamage
                  << "  MAX " << g_sessionMaxComboDamage;
            line2 << "HP " << g_state.defenderHpStart << " -> " << g_state.defenderHpCurrent
                  << " (-" << g_state.defenderHpDelta << ")";
            line3 << "P1   M " << FormatMeterDelta(p1MeterDelta)
                  << "   RF " << FormatRfDelta(p1RfDelta);
            line4 << "P2   M " << FormatMeterDelta(p2MeterDelta)
                  << "   RF " << FormatRfDelta(p2RfDelta);
        }

        if (showDetail) {
            line5 << "PRORATION " << FormatScalePercent(g_state.scalePercent);
            if (cfg.comboOverlayDetailRowSource == 1 && g_state.attackDetail.valid) {
                line5 << "   Move " << g_state.attackerMoveId
                      << "   Hitstun " << g_state.defenderHitstun
                      << "   Untech " << g_state.defenderUntech;
            } else {
                line5 << "   Hitstun " << g_state.defenderHitstun
                      << "   Untech " << g_state.defenderUntech;
            }
            if (showRfMultiplier) {
                line5 << "   RFx " << std::fixed << std::setprecision(3) << g_state.rfMultiplier;
            }
            if (showRawScale) {
                line5 << "   Raw " << static_cast<int>(std::lround(g_state.scaleRaw));
            }
        } else {
            line5 << "PRORATION " << FormatScalePercent(g_state.scalePercent);
            if (showRfMultiplier) {
                line5 << "   RFx " << std::fixed << std::setprecision(3) << g_state.rfMultiplier;
            }
            if (showRawScale) {
                line5 << "   Raw " << static_cast<int>(std::lround(g_state.scaleRaw));
            }
        }

        int y = OVERLAY_Y;
        UpsertMessage(g_displayIds.header, line1.str(), RGB(255, 235, 190), OVERLAY_X, y, OVERLAY_BG_ALPHA);
        y += LINE_HEIGHT;
        UpsertMessage(g_displayIds.totals, line2.str(), RGB(245, 245, 245), OVERLAY_X, y, OVERLAY_BG_ALPHA);
        y += LINE_HEIGHT;
        UpsertMessage(g_displayIds.resources, line3.str(), RGB(150, 215, 255), OVERLAY_X, y, OVERLAY_BG_ALPHA);
        y += LINE_HEIGHT;
        UpsertMessage(g_displayIds.detail, line4.str(), RGB(255, 170, 170), OVERLAY_X, y, OVERLAY_BG_ALPHA);
        y += LINE_HEIGHT;
        UpsertMessage(g_displayIds.status, line5.str(), RGB(220, 220, 220), OVERLAY_X, y, OVERLAY_BG_ALPHA);
    }
}

namespace ComboOverlay {
    void Tick(const PerFrameSample& sample) {
        std::lock_guard<std::mutex> lock(g_comboOverlayMutex);

        if (sample.phase != GamePhase::Match || sample.online || !sample.charsInitialized) {
            ResetStateUnlocked("tick outside supported match state", true);
            return;
        }

        PlayerMetrics p1{};
        PlayerMetrics p2{};
        if (!ReadPlayerMetrics(sample.p1Ptr, p1) || !ReadPlayerMetrics(sample.p2Ptr, p2)) {
            ResetStateUnlocked("tick missing player metrics", false);
            return;
        }

        ComboRuntime combo1{};
        ComboRuntime combo2{};
        ReadComboRuntime(sample.p1Ptr, combo1);
        ReadComboRuntime(sample.p2Ptr, combo2);

        const int damageToP1 = g_prevPlayers[1].valid ? (std::max)(0, g_prevPlayers[1].hp - p1.hp) : 0;
        const int damageToP2 = g_prevPlayers[2].valid ? (std::max)(0, g_prevPlayers[2].hp - p2.hp) : 0;

        int attackerSide = 0;
        ComboRuntime currentCombo{};
        if (combo1.valid && combo1.hitCount > 0) {
            attackerSide = 1;
            currentCombo = combo1;
        }
        if (combo2.valid && combo2.hitCount > 0 && (!attackerSide || ScoreRuntime(combo2) > ScoreRuntime(currentCombo))) {
            attackerSide = 2;
            currentCombo = combo2;
        }

        if (!attackerSide) {
            if (!g_state.live) {
                if (damageToP2 > 0 && damageToP1 == 0 && IsComboVictimState(p2)) {
                    attackerSide = 1;
                    currentCombo = BuildSyntheticRuntime(combo1, 1, damageToP2);
                } else if (damageToP1 > 0 && damageToP2 == 0 && IsComboVictimState(p1)) {
                    attackerSide = 2;
                    currentCombo = BuildSyntheticRuntime(combo2, 1, damageToP1);
                }
            } else {
                const int defenderSide = (g_state.attackerSide == 1) ? 2 : 1;
                const PlayerMetrics& liveDefender = (defenderSide == 1) ? p1 : p2;
                const int liveDamageDelta = (defenderSide == 1) ? damageToP1 : damageToP2;

                if (IsComboVictimState(liveDefender)) {
                    attackerSide = g_state.attackerSide;
                    currentCombo = BuildSyntheticRuntime(
                        (attackerSide == 1) ? combo1 : combo2,
                        g_state.hitCount,
                        g_state.totalDamage);
                    if (liveDamageDelta > 0) {
                        currentCombo.hitCount = g_state.hitCount + 1;
                        currentCombo.totalDamage = g_state.totalDamage + liveDamageDelta;
                    }
                }
            }
        }

        const unsigned long long nowMs = sample.tickMs ? sample.tickMs : XPCompat::GetTickCount64Compat();
        if (attackerSide != 0) {
            const PlayerMetrics& attacker = (attackerSide == 1) ? p1 : p2;
            const PlayerMetrics& defender = (attackerSide == 1) ? p2 : p1;
            const uintptr_t attackerPtr = (attackerSide == 1) ? sample.p1Ptr : sample.p2Ptr;

            const bool newCombo =
                !g_state.live
                || g_state.attackerSide != attackerSide
                || currentCombo.hitCount < g_state.hitCount
                || currentCombo.totalDamage < g_state.totalDamage;

            if (newCombo) {
                StartNewComboUnlocked(sample, attackerSide, currentCombo, attacker, defender, attackerPtr);
            } else {
                g_state.live = true;
                g_state.finalized = false;
                if (ComboProgressed(currentCombo)) {
                    UpdateLiveComboUnlocked(sample, currentCombo, attacker, defender, attackerPtr);
                } else {
                    const uint32_t framesSinceUpdate = (sample.frame >= g_state.lastUpdateFrame)
                        ? (sample.frame - g_state.lastUpdateFrame)
                        : 0;
                    if (framesSinceUpdate >= REALTIME_REFRESH_INTERVAL_TICKS) {
                        RefreshLiveStateUnlocked(sample, attacker, defender);
                    }
                }
            }
        } else if (g_state.live) {
            FinalizeComboUnlocked(nowMs);
        }

        RenderUnlocked(nowMs);

        g_prevPlayers[1] = { true, p1.hp, p1.meter, p1.rf };
        g_prevPlayers[2] = { true, p2.hp, p2.meter, p2.rf };
    }

    void ClearDisplay() {
        std::lock_guard<std::mutex> lock(g_comboOverlayMutex);
        ClearDisplayUnlocked();
    }

    void ResetState(const char* reason) {
        std::lock_guard<std::mutex> lock(g_comboOverlayMutex);
        ResetStateUnlocked(reason, true);
    }
}
