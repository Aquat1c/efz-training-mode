#include "../include/utils/utilities.h"

#include "../include/core/constants.h"
#include "../include/core/logger.h"
#include "../include/core/memory.h"
#include "../include/input/input_handler.h"
#include "../include/core/di_keycodes.h"
#include "../include/input/framestep.h"
#include "../include/game/frame_analysis.h"   
#include "../include/game/frame_advantage.h"
#include "../include/game/combo_overlay.h"
#include "../include/utils/config.h"
#include "../include/gui/imgui_impl.h"
#include "../include/gui/imgui_gui.h"
#include "../include/gui/overlay.h"
#include "../include/input/input_handler.h"
#include "../include/game/auto_airtech.h"
#include "../include/game/auto_action.h"
#include "../include/game/auto_action_charge.h"
#include "../include/game/kaori_recoil_duck.h"
#include "../include/game/frame_monitor.h"
#include "../include/input/input_freeze.h"
#include "../include/game/practice_offsets.h"
#include "../include/game/practice_patch.h"
#include "../include/game/always_rg.h"
#include "../include/game/random_rg.h"
#include "../include/game/hud_disable.h"
#include "../include/game/random_block.h"
#include "../include/utils/switch_players.h"
#include <sstream>
#include <iomanip>
#include <iostream>
#include <algorithm> 
#include <cwctype>    
#include <locale>     
#include <fstream>
#include <iomanip>
#include <chrono>
#include <vector>
#include <limits>
#include "../include/game/character_settings.h"
#include "../include/game/game_state.h"
#include "../include/gui/gui.h"   // OpenPracticeMenuDirect (single gated opener)
#include "../include/game/macro_controller.h"
#include "../include/game/character_hotswap.h"
#include "../include/game/mission/mission_engine.h"
#include "../include/game/collision_hook.h"
#include "../include/game/final_memory_patch.h"
#include "../include/game/savestate_hook.h"
#include "../include/input/input_hook.h"         
#include "../3rdparty/minhook/include/MinHook.h" 
#include "../include/input/immediate_input.h"
#include "../include/input/injection_control.h"
#include "../include/input/input_buffer.h"
#include "../include/input/motion_system.h"
#include "../include/input/input_core.h"
#include "../include/input/auto_action_motion_transaction.h"

#include "../include/utils/bgm_control.h"
#include "../include/utils/network.h"
#include "../include/utils/pause_integration.h"
#include "../include/utils/xp_compat.h"
#include "../include/core/globals.h"

std::atomic<bool> g_efzWindowActive(false);
std::atomic<bool> g_guiActive(false);
std::atomic<bool> g_onlineModeActive(false);
// Suppress auto-action clear logging (used for one-shot CS persistent clear)
std::atomic<bool> g_suppressAutoActionClearLogging(false);

namespace {
std::atomic<uint32_t> g_runtimeLifecycleGeneration{1};
std::atomic<bool> g_runtimeLifecycleResyncPending{false};
std::mutex g_runtimeLifecycleReasonMutex;
std::string g_runtimeLifecycleReason;
}

// NEW: Define the manual input override atomics
std::atomic<bool> g_manualInputOverride[3] = {false, false, false};
std::atomic<uint8_t> g_manualInputMask[3] = {0, 0, 0};
std::atomic<bool> g_manualJumpHold[3] = {false, false, false}; // NEW: Definition for jump hold

// Continuous Recovery runtime settings (defaults)
std::atomic<bool> g_contRecoveryEnabled{false};
std::atomic<int>  g_contRecoveryApplyTo{3}; // default Both
std::atomic<int>  g_contRecHpMode{0};
std::atomic<int>  g_contRecHpCustom{MAX_HP};
std::atomic<int>  g_contRecMeterMode{0};
std::atomic<int>  g_contRecMeterCustom{MAX_METER};
std::atomic<int>  g_contRecRfMode{0};
std::atomic<double> g_contRecRfCustom{MAX_RF};
std::atomic<bool> g_contRecRfForceBlueIC{false};

// NEW: Per-player Continuous Recovery runtime settings (defaults OFF)
std::atomic<bool> g_contRecEnabledP1{false};
std::atomic<int>  g_contRecHpModeP1{0};
std::atomic<int>  g_contRecHpCustomP1{MAX_HP};
std::atomic<int>  g_contRecMeterModeP1{0};
std::atomic<int>  g_contRecMeterCustomP1{MAX_METER};
std::atomic<int>  g_contRecRfModeP1{0};
std::atomic<double> g_contRecRfCustomP1{MAX_RF};
std::atomic<bool> g_contRecRfForceBlueICP1{false};
std::atomic<bool> g_contRecEnabledP2{false};
std::atomic<int>  g_contRecHpModeP2{0};
std::atomic<int>  g_contRecHpCustomP2{MAX_HP};
std::atomic<int>  g_contRecMeterModeP2{0};
std::atomic<int>  g_contRecMeterCustomP2{MAX_METER};
std::atomic<int>  g_contRecRfModeP2{0};
std::atomic<double> g_contRecRfCustomP2{MAX_RF};
std::atomic<bool> g_contRecRfForceBlueICP2{false};

// (Removed restoration of previous trigger states; triggers must always be manually re-enabled after mode changes)

// Reset DisplayData to default values (called on startup and character switches)
void ResetDisplayDataToDefaults() {
    // Simply copy the default initialization values
    displayData.hp1 = 9999;
    displayData.hp2 = 9999;
    displayData.meter1 = 3000;
    displayData.meter2 = 3000;
    displayData.rf1 = 1000.0;
    displayData.rf2 = 1000.0;
    displayData.x1 = 240.0;
    displayData.y1 = 0.0;
    displayData.x2 = 400.0;
    displayData.y2 = 0.0;
    displayData.autoAirtech = false;
    displayData.airtechDirection = 0;
    displayData.airtechDelay = 0;
    displayData.autoJump = false;
    displayData.jumpDirection = 0;
    displayData.jumpTarget = 3;
    displayData.p1CharName[0] = '\0';
    displayData.p2CharName[0] = '\0';
    displayData.p1CharID = 0;
    displayData.p2CharID = 0;
    // Ikumi
    displayData.p1IkumiBlood = 0;
    displayData.p2IkumiBlood = 0;
    displayData.p1IkumiGenocide = 0;
    displayData.p2IkumiGenocide = 0;
    displayData.p1IkumiLevelGauge = 0;
    displayData.p2IkumiLevelGauge = 0;
    displayData.infiniteBloodMode = false;
    displayData.infiniteShioriShield = false;  // Shiori (reuses Ikumi's +0x314C slot)
    // Misuzu
    displayData.p1MisuzuFeathers = 0;
    displayData.p2MisuzuFeathers = 0;
    displayData.infiniteFeatherMode = false;
    displayData.p1MisuzuPoisonTimer = 0;
    displayData.p2MisuzuPoisonTimer = 0;
    displayData.p1MisuzuPoisonLevel = 0;
    displayData.p2MisuzuPoisonLevel = 0;
    displayData.p1MisuzuInfinitePoison = false;
    displayData.p2MisuzuInfinitePoison = false;
    // Mishio
    displayData.p1MishioElement = 0;
    displayData.p2MishioElement = 0;
    displayData.p1MishioAwakenedTimer = 0;
    displayData.p2MishioAwakenedTimer = 0;
    displayData.infiniteMishioElement = false;
    displayData.infiniteMishioAwakened = false;
    // IC
    displayData.p1BlueIC = false;
    displayData.p2BlueIC = false;
    displayData.p2ControlEnabled = false;
    // Auto-action
    displayData.autoAction = false;
    displayData.autoActionType = ACTION_5A;
    displayData.autoActionCustomID = 200;
    displayData.autoActionPlayer = 2;
    displayData.triggerAfterBlock = false;
    displayData.triggerOnWakeup = false;
    displayData.triggerAfterHitstun = false;
    displayData.triggerAfterAirtech = false;
    displayData.triggerOnRG = false;
    displayData.delayAfterBlock = 0;
    displayData.delayOnWakeup = 0;
    displayData.delayAfterHitstun = 0;
    displayData.delayAfterAirtech = 0;
    displayData.delayOnRG = 0;
    displayData.actionAfterBlock = ACTION_5A;
    displayData.actionOnWakeup = ACTION_5A;
    displayData.actionAfterHitstun = ACTION_5A;
    displayData.actionAfterAirtech = ACTION_5A;
    displayData.actionOnRG = ACTION_5A;
    displayData.customAfterBlock = BASE_ATTACK_5A;
    displayData.customOnWakeup = BASE_ATTACK_5A;
    displayData.customAfterHitstun = BASE_ATTACK_5A;
    displayData.customAfterAirtech = BASE_ATTACK_JA;
    displayData.customOnRG = BASE_ATTACK_5A;
    displayData.strengthAfterBlock = 0;
    displayData.strengthOnWakeup = 0;
    displayData.strengthAfterHitstun = 0;
    displayData.strengthAfterAirtech = 0;
    displayData.strengthOnRG = 0;
    displayData.chargeAfterBlock = 0;
    displayData.chargeOnWakeup = 0;
    displayData.chargeAfterHitstun = 0;
    displayData.chargeAfterAirtech = 0;
    displayData.chargeOnRG = 0;
    displayData.macroSlotAfterBlock = 0;
    displayData.macroSlotOnWakeup = 0;
    displayData.macroSlotAfterHitstun = 0;
    displayData.macroSlotAfterAirtech = 0;
    displayData.macroSlotOnRG = 0;
    // Doppel
    displayData.p1DoppelEnlightened = false;
    displayData.p2DoppelEnlightened = false;
    // Rumi
    displayData.p1RumiBarehanded = false;
    displayData.p2RumiBarehanded = false;
    displayData.p1RumiInfiniteShinai = false;
    displayData.p2RumiInfiniteShinai = false;
    displayData.p1RumiKimchiActive = false;
    displayData.p2RumiKimchiActive = false;
    displayData.p1RumiKimchiTimer = 0;
    displayData.p2RumiKimchiTimer = 0;
    displayData.p1RumiInfiniteKimchi = false;
    displayData.p2RumiInfiniteKimchi = false;
    // Akiko
    displayData.p1AkikoBulletCycle = 0;
    displayData.p2AkikoBulletCycle = 0;
    displayData.p1AkikoTimeslowTrigger = 0;
    displayData.p2AkikoTimeslowTrigger = 0;
    displayData.p1AkikoFreezeCycle = false;
    displayData.p2AkikoFreezeCycle = false;
    displayData.p1AkikoShowCleanHit = false;
    displayData.p2AkikoShowCleanHit = false;
    displayData.p1AkikoInfiniteTimeslow = false;
    displayData.p2AkikoInfiniteTimeslow = false;
    // Neyuki
    displayData.p1NeyukiJamCount = 0;
    displayData.p2NeyukiJamCount = 0;
    displayData.p1NeyukiLockJam = false;
    displayData.p2NeyukiLockJam = false;
    // Mio
    displayData.p1MioStance = 0;
    displayData.p2MioStance = 0;
    displayData.p1MioLockStance = false;
    displayData.p2MioLockStance = false;
    // Kano
    displayData.p1KanoMagic = 0;
    displayData.p2KanoMagic = 0;
    displayData.p1KanoLockMagic = false;
    displayData.p2KanoLockMagic = false;
    // Mai
    displayData.p1MaiStatus = 0;
    displayData.p1MaiGhostTime = 0;
    displayData.p1MaiGhostCharge = 0;
    displayData.p1MaiAwakeningTime = 0;
    displayData.p2MaiStatus = 0;
    displayData.p2MaiGhostTime = 0;
    displayData.p2MaiGhostCharge = 0;
    displayData.p2MaiAwakeningTime = 0;
    displayData.p1MaiInfiniteGhost = false;
    displayData.p2MaiInfiniteGhost = false;
    displayData.p1MaiInfiniteCharge = false;
    displayData.p2MaiInfiniteCharge = false;
    displayData.p1MaiInfiniteAwakening = false;
    displayData.p2MaiInfiniteAwakening = false;
    displayData.p1MaiNoChargeCD = false;
    displayData.p2MaiNoChargeCD = false;
    displayData.p1MaiForceSummon = false;
    displayData.p2MaiForceSummon = false;
    displayData.p1MaiForceDespawn = false;
    displayData.p2MaiForceDespawn = false;
    displayData.p1MaiAggressiveOverride = false;
    displayData.p2MaiAggressiveOverride = false;
    // Continuous Recovery defaults
    displayData.continuousRecoveryEnabled = false;
    displayData.continuousRecoveryApplyTo = 3; // Both
    displayData.recoveryHpMode = 0;
    displayData.recoveryHpCustom = MAX_HP;
    displayData.recoveryMeterMode = 0;
    displayData.recoveryMeterCustom = MAX_METER;
    displayData.recoveryRfMode = 0;
    displayData.recoveryRfCustom = MAX_RF;
    displayData.recoveryRfForceBlueIC = false;
    // Per-player Continuous Recovery defaults (OFF)
    displayData.p1ContinuousRecoveryEnabled = false;
    displayData.p1RecoveryHpMode = 0;
    displayData.p1RecoveryHpCustom = MAX_HP;
    displayData.p1RecoveryMeterMode = 0;
    displayData.p1RecoveryMeterCustom = MAX_METER;
    displayData.p1RecoveryRfMode = 0;
    displayData.p1RecoveryRfCustom = MAX_RF;
    displayData.p1RecoveryRfForceBlueIC = false;
    displayData.p2ContinuousRecoveryEnabled = false;
    displayData.p2RecoveryHpMode = 0;
    displayData.p2RecoveryHpCustom = MAX_HP;
    displayData.p2RecoveryMeterMode = 0;
    displayData.p2RecoveryMeterCustom = MAX_METER;
    displayData.p2RecoveryRfMode = 0;
    displayData.p2RecoveryRfCustom = MAX_RF;
    displayData.p2RecoveryRfForceBlueIC = false;
    displayData.p1MaiGhostX = std::numeric_limits<double>::quiet_NaN();
    displayData.p1MaiGhostY = std::numeric_limits<double>::quiet_NaN();
    displayData.p2MaiGhostX = std::numeric_limits<double>::quiet_NaN();
    displayData.p2MaiGhostY = std::numeric_limits<double>::quiet_NaN();
    displayData.p1MaiGhostSetX = std::numeric_limits<double>::quiet_NaN();
    displayData.p1MaiGhostSetY = std::numeric_limits<double>::quiet_NaN();
    displayData.p2MaiGhostSetX = std::numeric_limits<double>::quiet_NaN();
    displayData.p2MaiGhostSetY = std::numeric_limits<double>::quiet_NaN();
    displayData.p1MaiApplyGhostPos = false;
    displayData.p2MaiApplyGhostPos = false;
    // Nayuki (Awake)
    displayData.p1NayukiSnowbunnies = 0;
    displayData.p2NayukiSnowbunnies = 0;
    displayData.p1NayukiInfiniteSnow = false;
    displayData.p2NayukiInfiniteSnow = false;
    // Minagi
    displayData.p1MinagiPuppetX = std::numeric_limits<double>::quiet_NaN();
    displayData.p1MinagiPuppetY = std::numeric_limits<double>::quiet_NaN();
    displayData.p2MinagiPuppetX = std::numeric_limits<double>::quiet_NaN();
    displayData.p2MinagiPuppetY = std::numeric_limits<double>::quiet_NaN();
    displayData.p1MinagiPuppetSetX = std::numeric_limits<double>::quiet_NaN();
    displayData.p1MinagiPuppetSetY = std::numeric_limits<double>::quiet_NaN();
    displayData.p2MinagiPuppetSetX = std::numeric_limits<double>::quiet_NaN();
    displayData.p2MinagiPuppetSetY = std::numeric_limits<double>::quiet_NaN();
    displayData.p1MinagiApplyPos = false;
    displayData.p2MinagiApplyPos = false;
    displayData.minagiConvertNewProjectiles = false;
    displayData.p1MinagiAlwaysReadied = false;
    displayData.p2MinagiAlwaysReadied = false;
    displayData.p1MichiruCurrentId = -1;
    displayData.p2MichiruCurrentId = -1;
    displayData.p1MichiruLastX = std::numeric_limits<double>::quiet_NaN();
    displayData.p1MichiruLastY = std::numeric_limits<double>::quiet_NaN();
    displayData.p2MichiruLastX = std::numeric_limits<double>::quiet_NaN();
    displayData.p2MichiruLastY = std::numeric_limits<double>::quiet_NaN();
    displayData.p1MichiruFrame = -1;
    displayData.p1MichiruSubframe = -1;
    displayData.p2MichiruFrame = -1;
    displayData.p2MichiruSubframe = -1;
    // Multi-action pools (defaults: disabled, empty masks)
    displayData.afterBlockActionPoolMask   = 0;
    displayData.onWakeupActionPoolMask     = 0;
    displayData.afterHitstunActionPoolMask = 0;
    displayData.afterAirtechActionPoolMask = 0;
    displayData.onRGActionPoolMask         = 0;
    displayData.afterBlockActionPoolMaskLo   = 0;
    displayData.afterBlockActionPoolMaskHi   = 0;
    displayData.onWakeupActionPoolMaskLo     = 0;
    displayData.onWakeupActionPoolMaskHi     = 0;
    displayData.afterHitstunActionPoolMaskLo = 0;
    displayData.afterHitstunActionPoolMaskHi = 0;
    displayData.afterAirtechActionPoolMaskLo = 0;
    displayData.afterAirtechActionPoolMaskHi = 0;
    displayData.onRGActionPoolMaskLo         = 0;
    displayData.onRGActionPoolMaskHi         = 0;
    displayData.afterBlockUseActionPool    = false;
    displayData.onWakeupUseActionPool      = false;
    displayData.afterHitstunUseActionPool  = false;
    displayData.afterAirtechUseActionPool  = false;
    displayData.onRGUseActionPool          = false;
    for (int i = 0; i < MAX_ACTION_POOL_OPTIONS; ++i) {
        displayData.afterBlockActionPoolDelays[i]   = -1;
        displayData.onWakeupActionPoolDelays[i]     = -1;
        displayData.afterHitstunActionPoolDelays[i] = -1;
        displayData.afterAirtechActionPoolDelays[i] = -1;
        displayData.onRGActionPoolDelays[i]         = -1;
        displayData.afterBlockActionPoolCharges[i]   = 0;
        displayData.onWakeupActionPoolCharges[i]     = 0;
        displayData.afterHitstunActionPoolCharges[i] = 0;
        displayData.afterAirtechActionPoolCharges[i] = 0;
        displayData.onRGActionPoolCharges[i]         = 0;
    }

    // Per-trigger option rows (randomized selection)
    displayData.afterBlockOptionCount = 0;
    displayData.onWakeupOptionCount = 0;
    displayData.afterHitstunOptionCount = 0;
    displayData.afterAirtechOptionCount = 0;
    displayData.onRGOptionCount = 0;
    for (int i = 0; i < MAX_TRIGGER_OPTIONS; ++i) {
        displayData.afterBlockOptions[i]   = { false, ACTION_5A, 0, 0, (int)BASE_ATTACK_5A, 0, 0 };
        displayData.onWakeupOptions[i]     = { false, ACTION_5A, 0, 0, (int)BASE_ATTACK_5A, 0, 0 };
        displayData.afterHitstunOptions[i] = { false, ACTION_5A, 0, 0, (int)BASE_ATTACK_5A, 0, 0 };
        displayData.afterAirtechOptions[i] = { false, ACTION_JA, 0, 0, (int)BASE_ATTACK_JA, 0, 0 };
        displayData.onRGOptions[i]         = { false, ACTION_5A, 0, 0, (int)BASE_ATTACK_5A, 0, 0 };
    }
    
    LogOut("[SYSTEM] DisplayData reset to defaults", true);
}

namespace {
void ResetRuntimeSettingsToDisplayDefaults() {
    autoAirtechEnabled.store(displayData.autoAirtech);
    autoAirtechDirection.store(displayData.airtechDirection);
    autoAirtechDelay.store(displayData.airtechDelay);
    autoJumpEnabled.store(displayData.autoJump);
    jumpDirection.store(displayData.jumpDirection);
    jumpTarget.store(displayData.jumpTarget);
    p1Jumping.store(false);
    p2Jumping.store(false);

    displayData.autoAction = HasAnyAutoActionTriggerEnabled(displayData);
    displayData.autoActionPlayer = ResolveAutoActionTargetPlayer();
    autoActionEnabled.store(displayData.autoAction);
    autoActionType.store(displayData.autoActionType);
    autoActionCustomID.store(displayData.autoActionCustomID);
    autoActionPlayer.store(displayData.autoActionPlayer);

    triggerAfterBlockEnabled.store(displayData.triggerAfterBlock);
    triggerOnWakeupEnabled.store(displayData.triggerOnWakeup);
    triggerAfterHitstunEnabled.store(displayData.triggerAfterHitstun);
    triggerAfterAirtechEnabled.store(displayData.triggerAfterAirtech);
    triggerOnRGEnabled.store(displayData.triggerOnRG);
    triggerRandomizeEnabled.store(displayData.randomizeTriggers);

    triggerAfterBlockDelay.store(displayData.delayAfterBlock);
    triggerOnWakeupDelay.store(displayData.delayOnWakeup);
    triggerAfterHitstunDelay.store(displayData.delayAfterHitstun);
    triggerAfterAirtechDelay.store(displayData.delayAfterAirtech);
    triggerOnRGDelay.store(displayData.delayOnRG);

    triggerAfterBlockAction.store(displayData.actionAfterBlock);
    triggerOnWakeupAction.store(displayData.actionOnWakeup);
    triggerAfterHitstunAction.store(displayData.actionAfterHitstun);
    triggerAfterAirtechAction.store(displayData.actionAfterAirtech);
    triggerOnRGAction.store(displayData.actionOnRG);

    triggerAfterBlockCharge.store(displayData.chargeAfterBlock);
    triggerOnWakeupCharge.store(displayData.chargeOnWakeup);
    triggerAfterHitstunCharge.store(displayData.chargeAfterHitstun);
    triggerAfterAirtechCharge.store(displayData.chargeAfterAirtech);
    triggerOnRGCharge.store(displayData.chargeOnRG);

    triggerAfterBlockActionPoolMask.store(displayData.afterBlockActionPoolMask);
    triggerOnWakeupActionPoolMask.store(displayData.onWakeupActionPoolMask);
    triggerAfterHitstunActionPoolMask.store(displayData.afterHitstunActionPoolMask);
    triggerAfterAirtechActionPoolMask.store(displayData.afterAirtechActionPoolMask);
    triggerOnRGActionPoolMask.store(displayData.onRGActionPoolMask);
    triggerAfterBlockActionPoolMaskLo.store(displayData.afterBlockActionPoolMaskLo);
    triggerAfterBlockActionPoolMaskHi.store(displayData.afterBlockActionPoolMaskHi);
    triggerOnWakeupActionPoolMaskLo.store(displayData.onWakeupActionPoolMaskLo);
    triggerOnWakeupActionPoolMaskHi.store(displayData.onWakeupActionPoolMaskHi);
    triggerAfterHitstunActionPoolMaskLo.store(displayData.afterHitstunActionPoolMaskLo);
    triggerAfterHitstunActionPoolMaskHi.store(displayData.afterHitstunActionPoolMaskHi);
    triggerAfterAirtechActionPoolMaskLo.store(displayData.afterAirtechActionPoolMaskLo);
    triggerAfterAirtechActionPoolMaskHi.store(displayData.afterAirtechActionPoolMaskHi);
    triggerOnRGActionPoolMaskLo.store(displayData.onRGActionPoolMaskLo);
    triggerOnRGActionPoolMaskHi.store(displayData.onRGActionPoolMaskHi);
    triggerAfterBlockUsePool.store(displayData.afterBlockUseActionPool);
    triggerOnWakeupUsePool.store(displayData.onWakeupUseActionPool);
    triggerAfterHitstunUsePool.store(displayData.afterHitstunUseActionPool);
    triggerAfterAirtechUsePool.store(displayData.afterAirtechUseActionPool);
    triggerOnRGUsePool.store(displayData.onRGUseActionPool);
    for (int i = 0; i < MAX_ACTION_POOL_OPTIONS; ++i) {
        g_afterBlockActionPoolDelays[i]   = displayData.afterBlockActionPoolDelays[i];
        g_onWakeupActionPoolDelays[i]     = displayData.onWakeupActionPoolDelays[i];
        g_afterHitstunActionPoolDelays[i] = displayData.afterHitstunActionPoolDelays[i];
        g_afterAirtechActionPoolDelays[i] = displayData.afterAirtechActionPoolDelays[i];
        g_onRGActionPoolDelays[i]         = displayData.onRGActionPoolDelays[i];
        g_afterBlockActionPoolCharges[i]   = displayData.afterBlockActionPoolCharges[i];
        g_onWakeupActionPoolCharges[i]     = displayData.onWakeupActionPoolCharges[i];
        g_afterHitstunActionPoolCharges[i] = displayData.afterHitstunActionPoolCharges[i];
        g_afterAirtechActionPoolCharges[i] = displayData.afterAirtechActionPoolCharges[i];
        g_onRGActionPoolCharges[i]         = displayData.onRGActionPoolCharges[i];
    }

    triggerAfterBlockCustomID.store(displayData.customAfterBlock);
    triggerOnWakeupCustomID.store(displayData.customOnWakeup);
    triggerAfterHitstunCustomID.store(displayData.customAfterHitstun);
    triggerAfterAirtechCustomID.store(displayData.customAfterAirtech);
    triggerOnRGCustomID.store(displayData.customOnRG);

    triggerAfterBlockStrength.store(displayData.strengthAfterBlock);
    triggerOnWakeupStrength.store(displayData.strengthOnWakeup);
    triggerAfterHitstunStrength.store(displayData.strengthAfterHitstun);
    triggerAfterAirtechStrength.store(displayData.strengthAfterAirtech);
    triggerOnRGStrength.store(displayData.strengthOnRG);

    triggerAfterBlockMacroSlot.store(displayData.macroSlotAfterBlock);
    triggerOnWakeupMacroSlot.store(displayData.macroSlotOnWakeup);
    triggerAfterHitstunMacroSlot.store(displayData.macroSlotAfterHitstun);
    triggerAfterAirtechMacroSlot.store(displayData.macroSlotAfterAirtech);
    triggerOnRGMacroSlot.store(displayData.macroSlotOnRG);

    auto clampCopy = [](int srcCount, const TriggerOption* srcArr, int& dstCount, TriggerOption* dstArr) {
        int count = srcCount;
        if (count < 0) count = 0;
        if (count > MAX_TRIGGER_OPTIONS) count = MAX_TRIGGER_OPTIONS;
        dstCount = count;
        for (int i = 0; i < count; ++i) {
            dstArr[i] = srcArr[i];
        }
        for (int i = count; i < MAX_TRIGGER_OPTIONS; ++i) {
            dstArr[i] = TriggerOption{false, ACTION_5A, 0, 0, (int)BASE_ATTACK_5A, 0, 0};
        }
    };
    clampCopy(displayData.afterBlockOptionCount, displayData.afterBlockOptions, g_afterBlockOptionCount, g_afterBlockOptions);
    clampCopy(displayData.onWakeupOptionCount, displayData.onWakeupOptions, g_onWakeupOptionCount, g_onWakeupOptions);
    clampCopy(displayData.afterHitstunOptionCount, displayData.afterHitstunOptions, g_afterHitstunOptionCount, g_afterHitstunOptions);
    clampCopy(displayData.afterAirtechOptionCount, displayData.afterAirtechOptions, g_afterAirtechOptionCount, g_afterAirtechOptions);
    clampCopy(displayData.onRGOptionCount, displayData.onRGOptions, g_onRGOptionCount, g_onRGOptions);

    g_contRecoveryEnabled.store(displayData.continuousRecoveryEnabled);
    g_contRecoveryApplyTo.store(displayData.continuousRecoveryApplyTo);
    g_contRecHpMode.store(displayData.recoveryHpMode);
    g_contRecHpCustom.store(displayData.recoveryHpCustom);
    g_contRecMeterMode.store(displayData.recoveryMeterMode);
    g_contRecMeterCustom.store(displayData.recoveryMeterCustom);
    g_contRecRfMode.store(displayData.recoveryRfMode);
    g_contRecRfCustom.store(displayData.recoveryRfCustom);
    g_contRecRfForceBlueIC.store(displayData.recoveryRfForceBlueIC);

    g_contRecEnabledP1.store(displayData.p1ContinuousRecoveryEnabled);
    g_contRecHpModeP1.store(displayData.p1RecoveryHpMode);
    g_contRecHpCustomP1.store(displayData.p1RecoveryHpCustom);
    g_contRecMeterModeP1.store(displayData.p1RecoveryMeterMode);
    g_contRecMeterCustomP1.store(displayData.p1RecoveryMeterCustom);
    g_contRecRfModeP1.store(displayData.p1RecoveryRfMode);
    g_contRecRfCustomP1.store(displayData.p1RecoveryRfCustom);
    g_contRecRfForceBlueICP1.store(displayData.p1RecoveryRfForceBlueIC);

    g_contRecEnabledP2.store(displayData.p2ContinuousRecoveryEnabled);
    g_contRecHpModeP2.store(displayData.p2RecoveryHpMode);
    g_contRecHpCustomP2.store(displayData.p2RecoveryHpCustom);
    g_contRecMeterModeP2.store(displayData.p2RecoveryMeterMode);
    g_contRecMeterCustomP2.store(displayData.p2RecoveryMeterCustom);
    g_contRecRfModeP2.store(displayData.p2RecoveryRfMode);
    g_contRecRfCustomP2.store(displayData.p2RecoveryRfCustom);
    g_contRecRfForceBlueICP2.store(displayData.p2RecoveryRfForceBlueIC);
}
}

void ResetPracticeMatchSessionState(const char* reason) {
    // Invalidate destination proof before any asynchronous consumer can observe
    // the reset in progress. The lifecycle generation below is a second guard.
    CharacterHotswap::InvalidateCompletedPracticeLoadReceipt();
    Mission::Engine::NotifyPracticeSessionReset(reason);
    Framestep::CancelActiveState(reason ? reason : "practice match reset");
    CancelAllAutoActionChargeFollowups(
        reason ? reason : "practice match reset");
    ResetDisplayDataToDefaults();
    ResetRuntimeSettingsToDisplayDefaults();
    ClearAllAutoActionTriggers();
    ResetActionFlags();
    MacroController::UnswapThenStop();

    forwardDashFollowup.store(0);
    forwardDashFollowupDashMode.store(false);
    g_wakeBufferingEnabled.store(false);
    g_counterRGEnabled.store(false);

    AlwaysRG::SetEnabled(false);
    RandomRG::SetEnabled(false);
    RandomBlock::SetEnabled(false);
    HudDisable::ResetVisible();   // exiting the match brings the HUD back

    SetDummyAutoBlockMode(DAB_None);
    SetAdaptiveStanceEnabled(false);
    if (GetCurrentGameMode() == GameMode::Practice && !IsNetplaySuspendActive()) {
        SetPracticeBlockMode(0);
        DisablePlayer2InPracticeMode();
    }

    WriteEngineRegenParams(0, 0);
    StopRFFreeze();
    StopRFFreezePlayer(1);
    StopRFFreezePlayer(2);
    SetRFFreezeColorDesired(1, false, false);
    SetRFFreezeColorDesired(2, false, false);

    SetFinalMemoryBypass(false);
    CharacterSettings::InvalidateAllCharacterPointerCaches();
    InvalidateAutoActionCharacterCaches(reason ? reason : "practice match reset");
    PauseIntegration::ResetCachedPointers(reason ? reason : "practice match reset");
    ResetCollisionHookSessionCaches(reason ? reason : "practice match reset");
    ComboOverlay::ResetState(reason ? reason : "practice match reset");
    ImGuiGui::ResetForPracticeSession(reason ? reason : "practice match reset");

    std::string lifecycleReason = "practice match session reset";
    if (reason && *reason) {
        lifecycleReason += ": ";
        lifecycleReason += reason;
    }
    RequestRuntimeLifecycleResync(lifecycleReason);

    std::ostringstream oss;
    oss << "[SESSION] Practice match state reset"
        << " reason=" << (reason && *reason ? reason : "unspecified")
        << " fmRequested=" << (IsFinalMemoryBypassEnabled() ? "1" : "0")
        << " fmInstalled=" << (IsFinalMemoryBypassInstalled() ? "1" : "0")
        << " wakeBuf=" << (g_wakeBufferingEnabled.load() ? "1" : "0")
        << " counterRG=" << (g_counterRGEnabled.load() ? "1" : "0")
        << " autoActionTarget=" << ResolveAutoActionTargetPlayer();
    LogOut(oss.str(), true);
}

// NEW: Add feature management functions
void EnableFeatures() {
    if (g_onlineModeActive.load()) return;
    if (g_featuresEnabled.load())
        return;

    LogOut("[SYSTEM] Game in valid mode. Enabling patches and overlays.", true);
    
    // Reset display data to defaults when entering valid mode
    ResetDisplayDataToDefaults();
    ImGuiGui::ResetForPracticeSession("EnableFeatures", false);
    // Invalidate cached character-specific pointers so they get
    // refreshed for the new session (prevents stale addresses
    // when re-entering Practice from character select).
    CharacterSettings::InvalidateAllCharacterPointerCaches();
    // Start centralized immediate input writer (64fps)
    ImmediateInput::Start();

    // Apply patches if the feature is enabled
    if (autoAirtechEnabled.load()) {
        ApplyAirtechPatches();
    }

    g_featuresEnabled.store(true);
    const int fmApplied = SyncFinalMemoryBypassForCurrentMode("EnableFeatures");
    if (fmApplied > 0 || detailedLogging.load()) {
        LogOut(
            std::string("[FM_PATCH] EnableFeatures sync")
            + " requested=" + (IsFinalMemoryBypassEnabled() ? "1" : "0")
            + " installed=" + (IsFinalMemoryBypassInstalled() ? "1" : "0")
            + " changes=" + std::to_string(fmApplied),
            true);
    }

    // No automatic restoration of triggers; user must re-enable manually
    LogOut("[SYSTEM] Triggers remain disabled until manually re-enabled", true);

    // Only reinitialize overlays if characters are initialized and we're in a valid game mode
    if (!g_onlineModeActive.load() && DirectDrawHook::isHooked && AreCharactersInitialized()) {
        GameMode currentMode = GetCurrentGameMode();
        if (IsValidGameMode(currentMode)) {
            ReinitializeOverlays();
            if (g_statsDisplayEnabled.load()) {
                UpdateStatsDisplay();
            }
            // After reinit, ensure trigger overlay reflects current toggles
            UpdateTriggerOverlay();
        } else {
            LogOut("[SYSTEM] Not initializing overlays - invalid game mode: " + 
                   GetGameModeName(currentMode), true);
        }
    }
    
    // --- BGM suppression integration ---
    // BGM suppression flag removed; legacy one-shot enforcement discarded.
}

void DisableFeatures() {
    if (!g_featuresEnabled.load())
        return;
    
    LogOut("[SYSTEM] Game left valid mode. Disabling patches and overlays.", true);
    const int fmRestored = ForceRestoreFinalMemoryHPBypass("DisableFeatures");
    if (fmRestored > 0 || detailedLogging.load()) {
        LogOut(
            std::string("[FM_PATCH] DisableFeatures restore")
            + " requested=" + (IsFinalMemoryBypassEnabled() ? "1" : "0")
            + " installed=" + (IsFinalMemoryBypassInstalled() ? "1" : "0")
            + " changes=" + std::to_string(fmRestored),
            true);
    }
    
    // CRITICAL: Restore normal control flags when leaving Practice mode
    // to prevent control swap issues in other modes
    {
        std::lock_guard<std::recursive_mutex> controlLock(g_p2ControlMutex);
        if (!g_onlineModeActive.load(std::memory_order_acquire)) {
            uintptr_t efzBase = GetEFZBase();
            if (efzBase) {
                uintptr_t gameStatePtr = 0;
                if (SafeReadMemory(efzBase + EFZ_BASE_OFFSET_GAME_STATE, &gameStatePtr, sizeof(uintptr_t)) && gameStatePtr) {
                    // Reset both sides to human (0 = human, 1 = CPU)
                    uint8_t p1Human = 0, p2Human = 0;
                    SafeWriteMemory(gameStatePtr + GAMESTATE_OFF_P1_CPU_FLAG, &p1Human, sizeof(uint8_t));
                    SafeWriteMemory(gameStatePtr + GAMESTATE_OFF_P2_CPU_FLAG, &p2Human, sizeof(uint8_t));
                    LogOut("[SYSTEM] Restored P1/P2 CPU flags to human (0) when disabling features", true);
                }
            }
        }
    }
    
    // Stop immediate input writer
    ImmediateInput::Stop();

    // Stop key monitoring when leaving valid game mode
    if (keyMonitorRunning.load()) {
        LogOut("[SYSTEM] Stopping key monitoring due to invalid game mode.", true);
        keyMonitorRunning.store(false);
    }

    // Remove any active patches
    RemoveAirtechPatches();
    // Character-specific enforcement is inline; nothing to stop explicitly here

    // Do NOT save states; we want a hard reset every time
    ResetPracticeMatchSessionState("DisableFeatures");

    autoActionEnabled.store(false);
    triggerAfterBlockEnabled.store(false);
    triggerOnWakeupEnabled.store(false);
    triggerAfterHitstunEnabled.store(false);
    triggerAfterAirtechEnabled.store(false);

    // Fully clear any in-flight auto-action internal state (delays, cooldowns, control overrides)
    ClearAllAutoActionTriggers();

    // Clear ALL visual overlays
    ComboOverlay::ResetState("DisableFeatures");
    DirectDrawHook::ClearAllMessages();
    
    // Reset stats display IDs since they've been cleared
    g_statsP1ValuesId = -1;
    g_statsP2ValuesId = -1;
    g_statsPositionId = -1;
    g_statsMoveIdId = -1;
    g_statsCleanHitId = -1;
    g_statsAIFlagsId = -1;
    g_statsBlockstunId = -1;
    g_statsUntechId = -1;

    // Also reset trigger/status overlay IDs so they get recreated on next update
    g_TriggerAfterBlockId = -1;
    g_TriggerOnWakeupId = -1;
    g_TriggerAfterHitstunId = -1;
    g_TriggerAfterAirtechId = -1;
    g_TriggerOnRGId = -1;
    g_AirtechStatusId = -1;
    g_JumpStatusId = -1;
    g_FrameAdvantageId = -1;
    g_FrameAdvantage2Id = -1;
    g_FrameGapId = -1;
    
    // Close the menu if it's open
    ImGuiImpl::ForceHide();

    // Reset all core logic states
    ResetFrameAdvantageState();
    ResetActionFlags();
    p1DelayState = {false, 0, TRIGGER_NONE, 0, -1, -1, 0, -1};
    p2DelayState = {false, 0, TRIGGER_NONE, 0, -1, -1, 0, -1};

    g_featuresEnabled.store(false);
    
    // Key monitoring will be handled separately by ManageKeyMonitoring()
}

// --- Lightweight shared positions cache -------------------------------
static std::atomic<double> s_cachedP1Y{0.0};
static std::atomic<double> s_cachedP2Y{0.0};
static std::atomic<unsigned long long> s_posCacheTickMs{0};

void UpdatePositionCache(double /*p1X*/, double p1Y, double /*p2X*/, double p2Y) {
    s_cachedP1Y.store(p1Y, std::memory_order_relaxed);
    s_cachedP2Y.store(p2Y, std::memory_order_relaxed);
    s_posCacheTickMs.store(XPCompat::GetTickCount64Compat(), std::memory_order_relaxed);
}

bool TryGetCachedYPositions(double &p1Y, double &p2Y, unsigned int maxAgeMs) {
    unsigned long long t = s_posCacheTickMs.load(std::memory_order_relaxed);
    if (t == 0) return false;
    unsigned long long now = XPCompat::GetTickCount64Compat();
    if (now - t > static_cast<unsigned long long>(maxAgeMs)) return false;
    p1Y = s_cachedP1Y.load(std::memory_order_relaxed);
    p2Y = s_cachedP2Y.load(std::memory_order_relaxed);
    return true;
}

namespace {

struct NetplayMenuPlayerReadback {
    bool pointerAvailable = false;
    bool readbackValid = false;
    uint8_t horizontal = 0;
    uint8_t vertical = 0;
    uint8_t buttonA = 0;
    uint8_t buttonB = 0;
    uint8_t buttonC = 0;
    uint8_t buttonD = 0;
    uint8_t command = 0;
    uint8_t dashCommand = 0;
    uint8_t latch1 = 0;
    uint8_t latch2 = 0;
    uint8_t dashTimer = 0;
    uint8_t motionToken = 0;
    uint16_t bufferIndex = 0;
    size_t nonZeroBufferBytes = 0;
};

void ResetOverlayTrackingIds() {
    g_statsP1ValuesId = -1;
    g_statsP2ValuesId = -1;
    g_statsPositionId = -1;
    g_statsMoveIdId = -1;
    g_statsCleanHitId = -1;
    g_statsNayukiId = -1;
    g_statsMisuzuId = -1;
    g_statsMishioId = -1;
    g_statsRumiId = -1;
    g_statsIkumiId = -1;
    g_statsMaiId = -1;
    g_statsMinagiId = -1;
    g_statsAIFlagsId = -1;
    g_statsBlockstunId = -1;
    g_statsUntechId = -1;

    g_TriggerAfterBlockId = -1;
    g_TriggerOnWakeupId = -1;
    g_TriggerAfterHitstunId = -1;
    g_TriggerAfterAirtechId = -1;
    g_TriggerOnRGId = -1;
    g_AirtechStatusId = -1;
    g_JumpStatusId = -1;
    g_FrameAdvantageId = -1;
    g_FrameAdvantage2Id = -1;
    g_FrameGapId = -1;
}

void ClearTransientInputOverrides() {
    for (int i = 1; i <= 2; ++i) {
        g_manualInputOverride[i].store(false);
        g_manualInputMask[i].store(0);
        g_manualJumpHold[i].store(false);
        g_forceBypass[i].store(false);
        g_pollOverrideActive[i].store(false);
        g_pollOverrideMask[i].store(0);
        g_injectImmediateOnly[i].store(false);
    }
}

void ResetDelayStatesForSuspend() {
    p1DelayState = {false, 0, TRIGGER_NONE, 0, -1, -1, 0, -1};
    p2DelayState = {false, 0, TRIGGER_NONE, 0, -1, -1, 0, -1};
}

bool OverlayTrackingIdsAreClear() {
    return g_statsP1ValuesId == -1
        && g_statsP2ValuesId == -1
        && g_statsPositionId == -1
        && g_statsMoveIdId == -1
        && g_statsCleanHitId == -1
        && g_statsNayukiId == -1
        && g_statsMisuzuId == -1
        && g_statsMishioId == -1
        && g_statsRumiId == -1
        && g_statsIkumiId == -1
        && g_statsMaiId == -1
        && g_statsMinagiId == -1
        && g_statsAIFlagsId == -1
        && g_statsBlockstunId == -1
        && g_statsUntechId == -1
        && g_TriggerAfterBlockId == -1
        && g_TriggerOnWakeupId == -1
        && g_TriggerAfterHitstunId == -1
        && g_TriggerAfterAirtechId == -1
        && g_TriggerOnRGId == -1
        && g_AirtechStatusId == -1
        && g_JumpStatusId == -1
        && g_FrameAdvantageId == -1
        && g_FrameAdvantage2Id == -1
        && g_FrameGapId == -1;
}

NetplayMenuPlayerReadback CaptureNetplayMenuPlayerReadback(int playerNum) {
    NetplayMenuPlayerReadback readback;
    const uintptr_t playerPtr = GetPlayerPointer(playerNum);
    if (!playerPtr) {
        return readback;
    }

    readback.pointerAvailable = true;
    bool ok = true;
    ok = SafeReadMemory(playerPtr + INPUT_HORIZONTAL_OFFSET, &readback.horizontal, sizeof(readback.horizontal)) && ok;
    ok = SafeReadMemory(playerPtr + INPUT_VERTICAL_OFFSET, &readback.vertical, sizeof(readback.vertical)) && ok;
    ok = SafeReadMemory(playerPtr + INPUT_BUTTON_A_OFFSET, &readback.buttonA, sizeof(readback.buttonA)) && ok;
    ok = SafeReadMemory(playerPtr + INPUT_BUTTON_B_OFFSET, &readback.buttonB, sizeof(readback.buttonB)) && ok;
    ok = SafeReadMemory(playerPtr + INPUT_BUTTON_C_OFFSET, &readback.buttonC, sizeof(readback.buttonC)) && ok;
    ok = SafeReadMemory(playerPtr + INPUT_BUTTON_D_OFFSET, &readback.buttonD, sizeof(readback.buttonD)) && ok;
    ok = SafeReadMemory(playerPtr + COMMAND_BUFFER_OFFSET, &readback.command, sizeof(readback.command)) && ok;
    ok = SafeReadMemory(playerPtr + DASH_COMMAND_OFFSET, &readback.dashCommand, sizeof(readback.dashCommand)) && ok;
    ok = SafeReadMemory(playerPtr + INPUT_LATCH1_OFFSET, &readback.latch1, sizeof(readback.latch1)) && ok;
    ok = SafeReadMemory(playerPtr + INPUT_LATCH2_OFFSET, &readback.latch2, sizeof(readback.latch2)) && ok;
    ok = SafeReadMemory(playerPtr + DASH_TIMER_OFFSET, &readback.dashTimer, sizeof(readback.dashTimer)) && ok;
    ok = SafeReadMemory(playerPtr + MOTION_TOKEN_OFFSET, &readback.motionToken, sizeof(readback.motionToken)) && ok;
    ok = SafeReadMemory(playerPtr + INPUT_BUFFER_INDEX_OFFSET, &readback.bufferIndex, sizeof(readback.bufferIndex)) && ok;

    std::vector<uint8_t> buffer(INPUT_BUFFER_SIZE, 0);
    if (!SafeReadMemory(playerPtr + INPUT_BUFFER_OFFSET, buffer.data(), INPUT_BUFFER_SIZE)) {
        ok = false;
    } else {
        readback.nonZeroBufferBytes =
            static_cast<size_t>(std::count_if(buffer.begin(), buffer.end(), [](uint8_t value) {
                return value != 0;
            }));
    }

    readback.readbackValid = ok;
    return readback;
}

void AppendPlayerReadbackResiduals(
    int playerNum,
    const NetplayMenuPlayerReadback& readback,
    std::vector<std::string>& residuals) {
    const std::string prefix = "P" + std::to_string(playerNum);
    if (!readback.pointerAvailable) {
        return;
    }
    if (!readback.readbackValid) {
        residuals.push_back(prefix + "Readback=fail");
        return;
    }
    if (readback.horizontal != 0 || readback.vertical != 0
        || readback.buttonA != 0 || readback.buttonB != 0
        || readback.buttonC != 0 || readback.buttonD != 0) {
        std::ostringstream oss;
        oss << prefix << "Immediate=("
            << static_cast<int>(readback.horizontal) << ","
            << static_cast<int>(readback.vertical) << ","
            << static_cast<int>(readback.buttonA) << ","
            << static_cast<int>(readback.buttonB) << ","
            << static_cast<int>(readback.buttonC) << ","
            << static_cast<int>(readback.buttonD) << ")";
        residuals.push_back(oss.str());
    }
    if (readback.command != 0) {
        residuals.push_back(prefix + "Command=" + std::to_string(readback.command));
    }
    if (readback.dashCommand != 0) {
        residuals.push_back(prefix + "DashCommand=" + std::to_string(readback.dashCommand));
    }
    if (readback.latch1 != 0 || readback.latch2 != 0) {
        std::ostringstream oss;
        oss << prefix << "Latch=("
            << static_cast<int>(readback.latch1) << ","
            << static_cast<int>(readback.latch2) << ")";
        residuals.push_back(oss.str());
    }
    if (readback.dashTimer != 0) {
        residuals.push_back(prefix + "DashTimer=" + std::to_string(readback.dashTimer));
    }
    if (readback.motionToken != 0x63) {
        std::ostringstream oss;
        oss << prefix << "MotionToken=0x"
            << std::hex << std::uppercase << std::setw(2) << std::setfill('0')
            << static_cast<int>(readback.motionToken);
        residuals.push_back(oss.str());
    }
    if (readback.bufferIndex != 0) {
        residuals.push_back(prefix + "BufferIndex=" + std::to_string(readback.bufferIndex));
    }
    if (readback.nonZeroBufferBytes != 0) {
        residuals.push_back(prefix + "BufferNonZero=" + std::to_string(readback.nonZeroBufferBytes));
    }
}

void AppendPlayerReadbackSummary(
    int playerNum,
    bool cleanupAttempted,
    bool cleanupSucceeded,
    const NetplayMenuPlayerReadback& readback,
    std::ostringstream& summary) {
    summary << " P" << playerNum << "[";
    if (!cleanupAttempted) {
        summary << "cleanup=na";
    } else {
        summary << "cleanup=" << (cleanupSucceeded ? "ok" : "partial");
    }

    if (!readback.pointerAvailable) {
        summary << " mem=na]";
        return;
    }
    if (!readback.readbackValid) {
        summary << " mem=readback-fail]";
        return;
    }

    summary << " imm="
            << static_cast<int>(readback.horizontal) << "/"
            << static_cast<int>(readback.vertical) << "/"
            << static_cast<int>(readback.buttonA) << "/"
            << static_cast<int>(readback.buttonB) << "/"
            << static_cast<int>(readback.buttonC) << "/"
            << static_cast<int>(readback.buttonD)
            << " cmd=" << static_cast<int>(readback.command)
            << " dash=" << static_cast<int>(readback.dashCommand)
            << " latch=" << static_cast<int>(readback.latch1) << "/" << static_cast<int>(readback.latch2)
            << " dt=" << static_cast<int>(readback.dashTimer)
            << " token=0x" << std::hex << std::uppercase << std::setw(2) << std::setfill('0')
            << static_cast<int>(readback.motionToken)
            << std::dec << std::nouppercase << std::setfill(' ')
            << " idx=" << readback.bufferIndex
            << " nz=" << readback.nonZeroBufferBytes
            << "]";
}

} // namespace

void EnterNetplaySuspend() {
    // Roll back the offline, identity-bound producer while its world is still
    // eligible for exact cleanup.  Publishing online mode first would make
    // every later input path refuse the restore and strand authored residue.
    bool wasSuspended = false;
    {
        // Queue publication uses this same mutex and rechecks online mode after
        // acquiring it. No new P2 generation can slip between rollback and the
        // online-state publication, and concurrent suspend callers preserve
        // the original one-winner exchange semantics.
        std::lock_guard<std::recursive_mutex> controlLock(g_p2ControlMutex);
        wasSuspended = g_onlineModeActive.load(std::memory_order_acquire);
        if (!wasSuspended) {
            // Drain every offline input owner before publishing the online
            // boundary.  Their post-publication teardown is deliberately
            // bookkeeping-only, so authored raw/ring/controller state must be
            // neutralized while this world still belongs to training mode.
            MacroController::Stop();
            KaoriRecoilDuck::CancelAll("entering netplay suspend");
            CancelAllAutoActionChargeFollowups(
                "entering netplay suspend");
            ImmediateInput::Stop();
            StopBufferFreezingIgnoringTutorialLease();
            (void)ClearMotionInputQueue(1, true);
            (void)ClearMotionInputQueue(2, true);
            if (!DrainAutoActionNormalPulsesForOwnershipBoundary()) {
                LogOut("[NETPLAY] Normal-input cleanup remains pending at ownership boundary", true);
            }
            CancelP2AutoActionMotionTransaction("entering netplay suspend");
            // Cleanup normally completes in one pass; retry a retained
            // fail-closed cleanup obligation before netplay takes controller
            // ownership. The later hook-disable path retries once more.
            for (int retry = 0;
                 retry < 2 && IsP2AutoActionMotionTransactionActive();
                 ++retry) {
                CancelP2AutoActionMotionTransaction(
                    "entering netplay suspend cleanup retry");
            }
            if (IsP2AutoActionMotionTransactionActive()) {
                LogOut("[NETPLAY] P2 input cleanup remains pending while suspension is published", true);
            }
            // The transaction owners above clean only their attributed spans.
            // A completed wake macro can also leave an intentionally retained
            // native command token/ring until its post-wake epilogue. Netplay
            // is a hard ownership boundary, so scrub both complete offline
            // fighter lanes now while the shared publication lock still blocks
            // every new mod-owned producer. After online mode is published no
            // cleanup path is allowed to write these fields.
            (void)FullCleanupAfterToggle(1);
            (void)FullCleanupAfterToggle(2);
            if (g_p2ControlOverridden) {
                RestoreP2ControlState();
            }
        }
        g_onlineModeActive.store(true, std::memory_order_release);
        isOnlineMatch.store(true, std::memory_order_release);

        // A failed pre-publication cleanup must not survive as a deferred
        // memory writer. CleanupP2MotionTxnLocked sees the published online
        // flag and retires any remaining offline obligation without writes.
        CancelP2AutoActionMotionTransaction(
            "netplay ownership boundary published");
    }
    if (wasSuspended) {
        return;
    }

    NetplayRuntimeState state = GetNetplayRuntimeState();
    std::ostringstream oss;
    oss << "[NETPLAY] Entering training suspend"
        << " source=" << NetplayStateSourceName(state.source);
    if (state.exportAvailable) {
        oss << " mode=" << state.exportState.sessionMode
            << " phase=" << state.exportState.sessionPhase
            << " activity=" << static_cast<int>(state.exportState.activityPhase);
    } else {
        oss << " legacy=" << OnlineStateName(state.legacyOnlineState);
    }
    oss << " reason=" << GetLastOnlineDetectionReason();
    LogOut(oss.str(), true);
    RequestRuntimeLifecycleResync("entered netplay suspend");
    const int fmRestored = ForceRestoreFinalMemoryHPBypass("EnterNetplaySuspend");

    ImmediateInput::Stop();
    StopBufferFreezing();
    StopRFFreeze();
    SetInputHookActive(false);
    SetCollisionHookActive(false);
    PauseIntegration::SetRuntimeHooksActive(false);
    DirectDrawHook::SetD3D9Active(false);
    SavestateHook::Uninstall();

    if (g_featuresEnabled.load()) {
        DisableFeatures();
    } else {
        ClearAllAutoActionTriggers();
        ResetActionFlags();
        ResetDelayStatesForSuspend();
        DirectDrawHook::ClearAllMessages();
        ResetOverlayTrackingIds();
    }

    if (g_p2ControlOverridden) {
        RestoreP2ControlState();
    }
    g_pendingControlRestore.store(false);
    ClearTransientInputOverrides();

    if (keyMonitorRunning.load()) {
        keyMonitorRunning.store(false);
    }

    StopBGMSuppressionPoller();
    SetBGMSuppressed(false);

    ImGuiImpl::ForceHide();

    DirectDrawHook::ClearAllMessages();
    ResetOverlayTrackingIds();

    LogOut(
        std::string("[NETPLAY] Suspend cleanup complete")
        + " features=" + (g_featuresEnabled.load() ? "1" : "0")
        + " keyMonitor=" + (keyMonitorRunning.load() ? "1" : "0")
        + " imguiVisible=" + (ImGuiImpl::IsVisible() ? "1" : "0")
        + " fmRequested=" + (IsFinalMemoryBypassEnabled() ? "1" : "0")
        + " fmInstalled=" + (IsFinalMemoryBypassInstalled() ? "1" : "0")
        + " fmRestored=" + std::to_string(fmRestored),
        true);

    if (state.inNetplayMenu || state.exportState.activityPhase == EFZ_ACTIVITY_MENU) {
        AuditNetplayMenuEntryState();
    }
}

void ExitNetplaySuspend() {
    const bool wasSuspended = g_onlineModeActive.exchange(false);
    isOnlineMatch.store(false, std::memory_order_release);
    if (!wasSuspended) {
        return;
    }

    NetplayRuntimeState state = GetNetplayRuntimeState();
    std::ostringstream oss;
    oss << "[NETPLAY] Leaving training suspend"
        << " source=" << NetplayStateSourceName(state.source)
        << " reason=" << GetLastOnlineDetectionReason();
    LogOut(oss.str(), true);
    RequestRuntimeLifecycleResync("left netplay suspend");

    ImmediateInput::Stop();
    StopBufferFreezing();
    StopRFFreeze();
    ClearAllAutoActionTriggers();
    ResetActionFlags();
    ResetDelayStatesForSuspend();

    if (g_p2ControlOverridden) {
        RestoreP2ControlState();
    }
    g_pendingControlRestore.store(false);
    ClearTransientInputOverrides();

    InvalidateGameStatePtrCache();
    InvalidatePlayerBaseCache();
    CharacterSettings::InvalidateAllCharacterPointerCaches();
    ResetPracticeMatchSessionState("ExitNetplaySuspend");

    DirectDrawHook::ClearAllMessages();
    ResetOverlayTrackingIds();
    InstallInputHook();
    InstallCollisionHook();
    PauseIntegration::SetRuntimeHooksActive(true);
    DirectDrawHook::SetD3D9Active(true);
    SavestateHook::Install();

    LogOut(
        std::string("[NETPLAY] Resume cleanup complete: transient overrides cleared, caches invalidated")
        + " fmRequested=" + (IsFinalMemoryBypassEnabled() ? "1" : "0")
        + " fmInstalled=" + (IsFinalMemoryBypassInstalled() ? "1" : "0"),
        true);
}

void AuditNetplayMenuEntryState() {
    const NetplayRuntimeState state = GetNetplayRuntimeState();

    std::ostringstream start;
    start << "[NETPLAY][VERIFY] Menu entry audit start"
          << " source=" << NetplayStateSourceName(state.source);
    if (state.exportAvailable) {
        start << " mode=" << state.exportState.sessionMode
              << " phase=" << state.exportState.sessionPhase
              << " activity=" << static_cast<int>(state.exportState.activityPhase)
              << " screen=" << static_cast<int>(state.exportState.netplayMenuScreen)
              << " detail=" << static_cast<int>(state.exportState.netplayMenuDetail);
    } else {
        start << " legacy=" << OnlineStateName(state.legacyOnlineState);
    }
    start << " reason=" << GetLastOnlineDetectionReason();
    LogOut(start.str(), true);

    MacroController::Stop();
    ImmediateInput::Stop();
    ImmediateInput::Clear(1);
    ImmediateInput::Clear(2);
    StopBufferFreezing();
    StopRFFreeze();
    ClearAllAutoActionTriggers();
    ResetActionFlags();
    ResetDelayStatesForSuspend();

    if (g_p2ControlOverridden) {
        RestoreP2ControlState();
    }
    g_pendingControlRestore.store(false);
    ClearTransientInputOverrides();

    if (keyMonitorRunning.load()) {
        keyMonitorRunning.store(false);
    }

    ImGuiImpl::ForceHide();
    menuOpen.store(false);
    g_guiActive.store(false);
    PauseIntegration::ForceCloseAllMenuSurfaces();

    DirectDrawHook::ClearAllMessages();
    ResetOverlayTrackingIds();

    // Online ownership has already been published.  This audit is strictly
    // read-only: all writable training owners were drained in
    // EnterNetplaySuspend before publication.
    bool cleanupAttempted[3] = {false, false, false};
    bool cleanupSucceeded[3] = {false, false, false};
    NetplayMenuPlayerReadback readback[3];
    for (int player = 1; player <= 2; ++player) {
        readback[player] = CaptureNetplayMenuPlayerReadback(player);
    }

    std::vector<std::string> residuals;
    if (g_featuresEnabled.load()) {
        residuals.push_back("features=1");
    }
    if (MacroController::GetState() != MacroController::State::Idle) {
        residuals.push_back("macroState=" + std::to_string(static_cast<int>(MacroController::GetState())));
    }
    if (ImmediateInput::IsRunning()) {
        residuals.push_back("immediateThread=1");
    }
    for (int player = 1; player <= 2; ++player) {
        if (ImmediateInput::GetCurrentDesired(player) != 0) {
            residuals.push_back("P" + std::to_string(player) + "ImmediateDesired="
                + std::to_string(ImmediateInput::GetCurrentDesired(player)));
        }
        if (ImmediateInput::GetRemainingTicks(player) != 0) {
            residuals.push_back("P" + std::to_string(player) + "ImmediateTicks="
                + std::to_string(ImmediateInput::GetRemainingTicks(player)));
        }
        if (g_manualInputOverride[player].load()) {
            residuals.push_back("P" + std::to_string(player) + "ManualOverride=1");
        }
        if (g_manualInputMask[player].load() != 0) {
            residuals.push_back("P" + std::to_string(player) + "ManualMask="
                + std::to_string(g_manualInputMask[player].load()));
        }
        if (g_manualJumpHold[player].load()) {
            residuals.push_back("P" + std::to_string(player) + "ManualJumpHold=1");
        }
        if (g_forceBypass[player].load()) {
            residuals.push_back("P" + std::to_string(player) + "ForceBypass=1");
        }
        if (g_pollOverrideActive[player].load()) {
            residuals.push_back("P" + std::to_string(player) + "PollOverride=1");
        }
        if (g_pollOverrideMask[player].load() != 0) {
            residuals.push_back("P" + std::to_string(player) + "PollMask="
                + std::to_string(g_pollOverrideMask[player].load()));
        }
        if (g_injectImmediateOnly[player].load()) {
            residuals.push_back("P" + std::to_string(player) + "ImmediateOnly=1");
        }
    }
    if (g_bufferFreezingActive.load()) {
        residuals.push_back("bufferFreeze=1");
    }
    if (g_indexFreezingActive.load()) {
        residuals.push_back("indexFreeze=1");
    }
    if (g_activeFreezePlayer.load() != 0) {
        residuals.push_back("freezeOwner=P" + std::to_string(g_activeFreezePlayer.load()));
    }
    bool rfActive = false;
    for (int player = 1; player <= 2; ++player) {
        bool active = false;
        double value = 0.0;
        bool colorManaged = false;
        bool colorBlue = false;
        if (GetRFFreezeStatus(player, active, value, colorManaged, colorBlue) && active) {
            rfActive = true;
            std::ostringstream oss;
            oss << "P" << player << "RFFreeze=" << value;
            residuals.push_back(oss.str());
        }
    }
    if (rfActive) {
        residuals.push_back("rfFreeze=1");
    }
    if (g_p2ControlOverridden) {
        residuals.push_back("p2ControlOverride=1");
    }
    if (g_pendingControlRestore.load()) {
        residuals.push_back("pendingControlRestore=1");
    }
    if (ImGuiImpl::IsVisible()) {
        residuals.push_back("imguiVisible=1");
    }
    if (menuOpen.load()) {
        residuals.push_back("menuOpen=1");
    }
    if (g_guiActive.load()) {
        residuals.push_back("guiActive=1");
    }
    if (!OverlayTrackingIdsAreClear()) {
        residuals.push_back("overlayIds=stale");
    }
    if (PauseIntegration::IsPausedOrFrozen()) {
        residuals.push_back("pauseFrozen=1");
    }
    if (IsFinalMemoryBypassInstalled()) {
        residuals.push_back("fmBypassInstalled=1");
    }
    if (IsFinalMemoryBypassEnabled()) {
        residuals.push_back("fmBypassRequested=1");
    }

    AppendPlayerReadbackResiduals(1, readback[1], residuals);
    AppendPlayerReadbackResiduals(2, readback[2], residuals);

    std::ostringstream summary;
    summary << "[NETPLAY][VERIFY] Menu entry state";
    AppendPlayerReadbackSummary(1, cleanupAttempted[1], cleanupSucceeded[1], readback[1], summary);
    AppendPlayerReadbackSummary(2, cleanupAttempted[2], cleanupSucceeded[2], readback[2], summary);
    summary << " overlays=" << (OverlayTrackingIdsAreClear() ? "clear" : "stale")
            << " pause=" << (PauseIntegration::IsPausedOrFrozen() ? "1" : "0")
            << " macro=" << static_cast<int>(MacroController::GetState())
            << " features=" << (g_featuresEnabled.load() ? "1" : "0");
    LogOut(summary.str(), true);

    if (residuals.empty()) {
        LogOut("[NETPLAY][VERIFY] Menu entry clean: no active training overrides or injected state remain", true);
        return;
    }

    std::ostringstream warn;
    warn << "[NETPLAY][VERIFY][WARN] Menu entry residual state:";
    for (const std::string& residual : residuals) {
        warn << " " << residual;
    }
    LogOut(warn.str(), true);
}

// Public helper: permanently clear all triggers so they stay disabled until user re-enables
void ClearAllTriggersPersistently() {
    static uint64_t s_lastClearTick = 0; // throttle identical spam bursts
    uint64_t nowTick = XPCompat::GetTickCount64Compat();
    bool willLogPrimary = (nowTick - s_lastClearTick > 750); // at most ~1 log per 750ms
    if (willLogPrimary) {
        LogOut("[SYSTEM] Clearing all triggers persistently (Character Select / forced)", true);
        s_lastClearTick = nowTick;
    }
    // Disable toggles immediately so they will NOT be auto-restored
    autoActionEnabled.store(false);
    triggerAfterBlockEnabled.store(false);
    triggerOnWakeupEnabled.store(false);
    triggerAfterHitstunEnabled.store(false);
    triggerAfterAirtechEnabled.store(false);

    // Also wipe internal delay/cooldown state so nothing fires after returning
    ClearAllAutoActionTriggers();

    // Explicit hard reset message only if we emitted the primary (avoid paired duplicates)
    if (willLogPrimary) {
        LogOut("[SYSTEM] Trigger states hard-reset (no restoration mechanism active)", true);
    }

    // Remove any trigger overlay lines now
    if (g_TriggerAfterBlockId != -1) { DirectDrawHook::RemovePermanentMessage(g_TriggerAfterBlockId); g_TriggerAfterBlockId = -1; }
    if (g_TriggerOnWakeupId != -1) { DirectDrawHook::RemovePermanentMessage(g_TriggerOnWakeupId); g_TriggerOnWakeupId = -1; }
    if (g_TriggerAfterHitstunId != -1) { DirectDrawHook::RemovePermanentMessage(g_TriggerAfterHitstunId); g_TriggerAfterHitstunId = -1; }
    if (g_TriggerAfterAirtechId != -1) { DirectDrawHook::RemovePermanentMessage(g_TriggerAfterAirtechId); g_TriggerAfterAirtechId = -1; }
    if (g_TriggerOnRGId != -1) { DirectDrawHook::RemovePermanentMessage(g_TriggerOnRGId); g_TriggerOnRGId = -1; }
}


// Global flag to track if we're still in startup mode
std::atomic<bool> inStartupPhase(true);
std::string startupLogPath;
// Flag to enable/disable startup log file (checked after config loads)
static std::atomic<bool> s_startupLogEnabled{true};  // Default true until config says otherwise
// Track if we've written the first message this session (to truncate on first write)
static std::atomic<bool> s_startupLogFirstWrite{true};

// Called after config loads to disable startup log if detailedLogging is off
void SetStartupLogEnabled(bool enabled) {
    s_startupLogEnabled.store(enabled);
}

// Create a function that writes to a log file without requiring the console
void WriteStartupLog(const std::string& message) {
    if (!inStartupPhase) return; // Skip if we're past startup
    if (g_onlineModeActive.load()) return; // Skip entirely during online mode
    if (!s_startupLogEnabled.load()) return; // Skip if disabled by config
    
    try {
        // Determine log file path once
        if (startupLogPath.empty()) {
            char path[MAX_PATH] = {0};
            GetModuleFileNameA(NULL, path, MAX_PATH);
            std::string exePath(path);
            startupLogPath = exePath.substr(0, exePath.find_last_of("\\/")) + "\\efz_startup.log";
        }
        
        // First write this session: truncate (refresh) the file
        // Subsequent writes: append
        bool isFirstWrite = s_startupLogFirstWrite.exchange(false);
        std::ios_base::openmode mode = isFirstWrite ? std::ios::trunc : std::ios::app;
        
        std::ofstream logFile(startupLogPath, mode);
        if (logFile.is_open()) {
            // Get current time
            auto now = std::chrono::system_clock::now();
            auto time = std::chrono::system_clock::to_time_t(now);
            
            // Convert to calendar time
            tm timeInfo;
            localtime_s(&timeInfo, &time);
            
            // Format timestamp: [HH:MM:SS.mmm]
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                now.time_since_epoch()) % 1000;
            
            char timeStr[20];
            std::strftime(timeStr, sizeof(timeStr), "%H:%M:%S", &timeInfo);
            
            // Write timestamped message
            logFile << "[" << timeStr << "." << std::setfill('0') << std::setw(3) << ms.count() 
                   << "] " << message << std::endl;
                   
            logFile.close();
        }
    }
    catch (...) {
        // Failsafe - we can't log the error anywhere reliable
    }
}

std::atomic<bool> menuOpen(false);
std::atomic<int> frameCounter(0);
std::atomic<bool> detailedLogging(false);
std::atomic<bool> autoAirtechEnabled(false);
std::atomic<int> autoAirtechDirection(0);  // 0=forward, 1=backward
std::atomic<bool> autoJumpEnabled(false);     // This was missing!
std::atomic<int> jumpDirection(0);            // 0=straight, 1=forward, 2=backward
std::atomic<bool> p1Jumping(false);
std::atomic<bool> p2Jumping(false);
std::atomic<int> jumpTarget(3);
DisplayData displayData{};

// Initialize key bindings with default values
KeyBindings detectedBindings = {
    INPUT_DEVICE_KEYBOARD, // inputDevice (default to keyboard)
    0,                     // gamepadIndex
    "Keyboard",            // deviceName
    VK_UP,                 // upKey (default to arrow keys)
    VK_DOWN,               // downKey
    VK_LEFT,               // leftKey
    VK_RIGHT,              // rightKey
    'Z',                   // aButton (common defaults)
    'X',                   // bButton
    'C',                   // cButton
    'A',                   // dButton
    false,                 // directionsDetected
    false                  // attacksDetected
};

// Add with other global variables
std::atomic<bool> g_statsDisplayEnabled(false);
std::atomic<int> g_statsPageIndex(0);
std::atomic<int> g_statsPageCount(1);
int g_statsP1ValuesId = -1;
int g_statsP2ValuesId = -1;
int g_statsPositionId = -1;
int g_statsMoveIdId = -1;
int g_statsCleanHitId = -1;
int g_statsNayukiId = -1;
int g_statsMisuzuId = -1;
int g_statsMishioId = -1;
int g_statsRumiId = -1;
int g_statsIkumiId = -1;
int g_statsMaiId = -1;
int g_statsMinagiId = -1;
int g_statsAIFlagsId = -1; // new: AI control flags line in stats overlay
int g_statsBlockstunId = -1; // new: Blockstun counters line
int g_statsUntechId = -1;    // new: Hitstun/Untech counters line

// Auto-action settings - replace single trigger with individual triggers
std::atomic<bool> autoActionEnabled(false);
std::atomic<int> autoActionType(ACTION_5A);
std::atomic<int> autoActionCustomID(200); // Default to 5A
std::atomic<int> autoActionPlayer(2);     // Default to P2 (training dummy)

// Individual trigger settings
std::atomic<bool> triggerAfterBlockEnabled(false);
std::atomic<bool> triggerOnWakeupEnabled(false);
std::atomic<bool> triggerAfterHitstunEnabled(false);
std::atomic<bool> triggerAfterAirtechEnabled(false);
std::atomic<bool> triggerOnRGEnabled(false);
// Global trigger randomization toggle (default OFF)
std::atomic<bool> triggerRandomizeEnabled(false);

// Delay settings (in visual frames)
std::atomic<int> triggerAfterBlockDelay(DEFAULT_TRIGGER_DELAY);
std::atomic<int> triggerOnWakeupDelay(DEFAULT_TRIGGER_DELAY);
std::atomic<int> triggerAfterHitstunDelay(DEFAULT_TRIGGER_DELAY);
std::atomic<int> triggerAfterAirtechDelay(DEFAULT_TRIGGER_DELAY);
std::atomic<int> triggerOnRGDelay(DEFAULT_TRIGGER_DELAY);

// Auto-airtech delay support
std::atomic<int> autoAirtechDelay(0); // Default to instant activation

// Immediate-only injection flags (index 0 unused)
std::atomic<bool> g_injectImmediateOnly[3] = {false, false, false};

// Individual action settings for each trigger
std::atomic<int> triggerAfterBlockAction(ACTION_5A);
std::atomic<int> triggerOnWakeupAction(ACTION_5A);
std::atomic<int> triggerAfterHitstunAction(ACTION_5A);
std::atomic<int> triggerAfterAirtechAction(ACTION_5A);
std::atomic<int> triggerOnRGAction(ACTION_5A);

std::atomic<int> triggerAfterBlockCharge(0);
std::atomic<int> triggerOnWakeupCharge(0);
std::atomic<int> triggerAfterHitstunCharge(0);
std::atomic<int> triggerAfterAirtechCharge(0);
std::atomic<int> triggerOnRGCharge(0);

// Multi-action pools per trigger (disabled by default)
std::atomic<uint32_t> triggerAfterBlockActionPoolMask{0};
std::atomic<uint32_t> triggerOnWakeupActionPoolMask{0};
std::atomic<uint32_t> triggerAfterHitstunActionPoolMask{0};
std::atomic<uint32_t> triggerAfterAirtechActionPoolMask{0};
std::atomic<uint32_t> triggerOnRGActionPoolMask{0};
std::atomic<uint64_t> triggerAfterBlockActionPoolMaskLo{0};
std::atomic<uint64_t> triggerAfterBlockActionPoolMaskHi{0};
std::atomic<uint64_t> triggerOnWakeupActionPoolMaskLo{0};
std::atomic<uint64_t> triggerOnWakeupActionPoolMaskHi{0};
std::atomic<uint64_t> triggerAfterHitstunActionPoolMaskLo{0};
std::atomic<uint64_t> triggerAfterHitstunActionPoolMaskHi{0};
std::atomic<uint64_t> triggerAfterAirtechActionPoolMaskLo{0};
std::atomic<uint64_t> triggerAfterAirtechActionPoolMaskHi{0};
std::atomic<uint64_t> triggerOnRGActionPoolMaskLo{0};
std::atomic<uint64_t> triggerOnRGActionPoolMaskHi{0};
std::atomic<bool>     triggerAfterBlockUsePool{false};
std::atomic<bool>     triggerOnWakeupUsePool{false};
std::atomic<bool>     triggerAfterHitstunUsePool{false};
std::atomic<bool>     triggerAfterAirtechUsePool{false};
std::atomic<bool>     triggerOnRGUsePool{false};

int g_afterBlockActionPoolDelays[MAX_ACTION_POOL_OPTIONS] = {};
int g_onWakeupActionPoolDelays[MAX_ACTION_POOL_OPTIONS] = {};
int g_afterHitstunActionPoolDelays[MAX_ACTION_POOL_OPTIONS] = {};
int g_afterAirtechActionPoolDelays[MAX_ACTION_POOL_OPTIONS] = {};
int g_onRGActionPoolDelays[MAX_ACTION_POOL_OPTIONS] = {};
int g_afterBlockActionPoolCharges[MAX_ACTION_POOL_OPTIONS] = {};
int g_onWakeupActionPoolCharges[MAX_ACTION_POOL_OPTIONS] = {};
int g_afterHitstunActionPoolCharges[MAX_ACTION_POOL_OPTIONS] = {};
int g_afterAirtechActionPoolCharges[MAX_ACTION_POOL_OPTIONS] = {};
int g_onRGActionPoolCharges[MAX_ACTION_POOL_OPTIONS] = {};

namespace {
struct InitActionPoolDelayDefaults {
    InitActionPoolDelayDefaults() {
        for (int i = 0; i < MAX_ACTION_POOL_OPTIONS; ++i) {
            g_afterBlockActionPoolDelays[i] = -1;
            g_onWakeupActionPoolDelays[i] = -1;
            g_afterHitstunActionPoolDelays[i] = -1;
            g_afterAirtechActionPoolDelays[i] = -1;
            g_onRGActionPoolDelays[i] = -1;
        }
    }
};
InitActionPoolDelayDefaults g_initActionPoolDelayDefaults;
}

// Runtime per-trigger option rows (populated on Apply)
int           g_afterBlockOptionCount = 0;
TriggerOption g_afterBlockOptions[MAX_TRIGGER_OPTIONS] = {};
int           g_onWakeupOptionCount = 0;
TriggerOption g_onWakeupOptions[MAX_TRIGGER_OPTIONS] = {};
int           g_afterHitstunOptionCount = 0;
TriggerOption g_afterHitstunOptions[MAX_TRIGGER_OPTIONS] = {};
int           g_afterAirtechOptionCount = 0;
TriggerOption g_afterAirtechOptions[MAX_TRIGGER_OPTIONS] = {};
int           g_onRGOptionCount = 0;
TriggerOption g_onRGOptions[MAX_TRIGGER_OPTIONS] = {};

// Forward dash follow-up selection (0=None, 1=5A,2=5B,3=5C,4=2A,5=2B,6=2C)
std::atomic<int> forwardDashFollowup(0);
// 0 = post-dash injection (existing behavior), 1 = dash-normal timing (inject during dash state window)
std::atomic<bool> forwardDashFollowupDashMode(false);

// Custom action IDs for each trigger
std::atomic<int> triggerAfterBlockCustomID{ (int)BASE_ATTACK_5A };
std::atomic<int> triggerOnWakeupCustomID{ (int)BASE_ATTACK_5A };
std::atomic<int> triggerAfterHitstunCustomID{ (int)BASE_ATTACK_5A };
std::atomic<int> triggerAfterAirtechCustomID{ (int)BASE_ATTACK_JA };  // Default to jumping A for airtech
std::atomic<int> triggerOnRGCustomID{ (int)BASE_ATTACK_5A };

// Individual strength settings (0=A, 1=B, 2=C, 3=D)
std::atomic<int> triggerAfterBlockStrength(0);
std::atomic<int> triggerOnWakeupStrength(0);
std::atomic<int> triggerAfterHitstunStrength(0);
std::atomic<int> triggerAfterAirtechStrength(0);
std::atomic<int> triggerOnRGStrength(0);

// Per-trigger macro slot selections (0=None, 1..MaxSlots)
std::atomic<int> triggerAfterBlockMacroSlot{ 0 };
std::atomic<int> triggerOnWakeupMacroSlot{ 0 };
std::atomic<int> triggerAfterHitstunMacroSlot{ 0 };
std::atomic<int> triggerAfterAirtechMacroSlot{ 0 };
std::atomic<int> triggerOnRGMacroSlot{ 0 };

bool HasAnyAutoActionTriggerEnabled(const DisplayData& data) {
    return data.triggerAfterBlock ||
           data.triggerOnWakeup ||
           data.triggerAfterHitstun ||
           data.triggerAfterAirtech ||
           data.triggerOnRG;
}

bool HasAnyAutoActionTriggerEnabled() {
    return triggerAfterBlockEnabled.load() ||
           triggerOnWakeupEnabled.load() ||
           triggerAfterHitstunEnabled.load() ||
           triggerAfterAirtechEnabled.load() ||
           triggerOnRGEnabled.load();
}

int ResolveAutoActionTargetPlayer() {
    const int remotePlayer = SwitchPlayers::GetRemotePlayerIndex();
    if (remotePlayer == 1 || remotePlayer == 2) {
        return remotePlayer;
    }

    const int localPlayer = SwitchPlayers::GetLocalPlayerIndex();
    return (localPlayer == 2) ? 1 : 2;
}

// Pre-buffer Wakeup: early-start a 0F On-Wakeup macro during state 96 (ON) vs first-actionable-frame
// playback (OFF). Does not affect wake specials (always early-buffered) or wake dashes (never).
std::atomic<bool> g_wakeBufferingEnabled{false};

// Global toggle: enable/disable Counter RG early-restore behavior (default OFF)
std::atomic<bool> g_counterRGEnabled{false};

// UI: gate for the regular Frame Advantage overlay (default ON)
std::atomic<bool> g_showFrameAdvantageOverlay{true};

// Deep frame advantage instrumentation toggle
std::atomic<bool> g_deepFrameAdvDebug{false};

std::string FormatPosition(double x, double y) {
    std::stringstream ss;
    ss.imbue(std::locale::classic());
    ss << std::fixed << std::setprecision(2) << "X=" << x << " Y=" << y;
    return ss.str();
}

// Cached EFZ base module handle to avoid repeated GetModuleHandleA calls.
namespace { std::atomic<uintptr_t> g_cachedEfzBase{0}; }

uintptr_t GetEFZBase() {
    uintptr_t val = g_cachedEfzBase.load(std::memory_order_acquire);
    if (val) return val;
    HMODULE h = GetModuleHandleA(NULL); // NULL = current process module
    if (!h) return 0;
    val = reinterpret_cast<uintptr_t>(h);
    g_cachedEfzBase.store(val, std::memory_order_release);
    return val;
}

void InvalidateEFZBaseCache() { g_cachedEfzBase.store(0, std::memory_order_release); }

// -----------------------------------------------------------------------------
// Game state pointer caching
// The game state object (at EFZ_BASE_OFFSET_GAME_STATE) is allocated once at
// startup (initializeGameSystem). Its pointer remains stable; internal fields
// are reset between matches. Safe to cache for lifetime of process unless we
// explicitly disable features / enter online mode.
namespace { std::atomic<uintptr_t> g_cachedGameState{0}; }

uintptr_t GetGameStatePtr() {
    uintptr_t gs = g_cachedGameState.load(std::memory_order_acquire);
    if (gs) return gs;
    uintptr_t base = GetEFZBase(); if (!base) return 0;
    uintptr_t tmp = 0; if (!SafeReadMemory(base + EFZ_BASE_OFFSET_GAME_STATE, &tmp, sizeof(tmp))) return 0;
    if (tmp) g_cachedGameState.store(tmp, std::memory_order_release);
    return tmp;
}

void InvalidateGameStatePtrCache() { g_cachedGameState.store(0, std::memory_order_release); }

uint32_t GetRuntimeLifecycleGeneration() {
    return g_runtimeLifecycleGeneration.load(std::memory_order_acquire);
}

void RequestRuntimeLifecycleResync(const std::string& reason) {
    std::string aggregateReason;
    {
        std::lock_guard<std::mutex> lock(g_runtimeLifecycleReasonMutex);
        if (g_runtimeLifecycleReason.empty()) {
            g_runtimeLifecycleReason = reason;
        } else if (g_runtimeLifecycleReason.find(reason) == std::string::npos) {
            g_runtimeLifecycleReason += "; " + reason;
        }
        aggregateReason = g_runtimeLifecycleReason;
    }

    const uint32_t generation = g_runtimeLifecycleGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;
    const bool wasPending = g_runtimeLifecycleResyncPending.exchange(true, std::memory_order_acq_rel);
    if (!wasPending || detailedLogging.load()) {
        std::ostringstream oss;
        oss << "[LIFECYCLE] Queued runtime resync"
            << " gen=" << generation
            << " pending=" << (wasPending ? "1" : "0")
            << " reason=" << aggregateReason;
        LogOut(oss.str(), true);
    }
}

void ConsumeRuntimeLifecycleResyncRequests() {
    if (!g_runtimeLifecycleResyncPending.exchange(false, std::memory_order_acq_rel)) {
        return;
    }

    std::string reason;
    {
        std::lock_guard<std::mutex> lock(g_runtimeLifecycleReasonMutex);
        reason = g_runtimeLifecycleReason;
        g_runtimeLifecycleReason.clear();
    }

    InvalidateGameStatePtrCache();
    InvalidatePlayerBaseCache();
    CharacterSettings::InvalidateAllCharacterPointerCaches();
    InvalidateAutoActionCharacterCaches("lifecycle resync");
    PauseIntegration::ResetCachedPointers("lifecycle resync");
    ResetCollisionHookSessionCaches("lifecycle resync");

    const bool isPostRestoreResync =
        reason.find("custom savestate restore complete") != std::string::npos
        || reason.find("character hotswap reload complete") != std::string::npos;

    std::ostringstream oss;
    oss << "[LIFECYCLE] Runtime resync applied"
        << " gen=" << GetRuntimeLifecycleGeneration()
        << " reason=" << reason
        << " caches=gameState,playerBase,charSettings,autoActionCharIds,pauseIntegration,collisionHook";

    if (isPostRestoreResync) {
        LogOut(oss.str(), true);
        return;
    }

    ComboOverlay::ResetState("lifecycle resync");
    LogOut(oss.str(), true);
}

// -----------------------------------------------------------------------------
// Player base pointer caching
// Player pointers (EFZ_BASE_OFFSET_P1/P2) are set to 0 at startup and populated
// during character load sequences. They are reused for each match but may be
// re-assigned when returning to character select and starting a new battle.
// Strategy:
//  - Cache after first successful read when AreCharactersInitialized()==true
//  - Invalidate when AreCharactersInitialized()==false OR screen state != Battle (3)
//  - Provide explicit invalidation for feature disable / online entry.
namespace { std::atomic<uintptr_t> g_cachedPlayerBase[3] = {0,0,0}; }

static bool ShouldInvalidatePlayerCache() {
    // When characters not initialized, cached bases invalid.
    if (!AreCharactersInitialized()) return true;
    // Screen state check: only trust during battle (3) and possibly win (5) for post-match reads.
    uint8_t screenState = 0; uintptr_t base = GetEFZBase();
    if (base) SafeReadMemory(base + EFZ_BASE_OFFSET_SCREEN_STATE, &screenState, sizeof(screenState));
    if (screenState != 3 && screenState != 5) return true; // battle or win screen retain
    return false;
}

uintptr_t GetPlayerBase(int playerIndex) {
    if (playerIndex != 1 && playerIndex != 2) return 0;
    if (ShouldInvalidatePlayerCache()) {
        g_cachedPlayerBase[1].store(0, std::memory_order_release);
        g_cachedPlayerBase[2].store(0, std::memory_order_release);
        return 0;
    }
    uintptr_t cached = g_cachedPlayerBase[playerIndex].load(std::memory_order_acquire);
    if (cached) return cached;
    uintptr_t base = GetEFZBase(); if (!base) return 0;
    uintptr_t ptr = 0; uintptr_t off = (playerIndex==1)?EFZ_BASE_OFFSET_P1:EFZ_BASE_OFFSET_P2;
    if (!SafeReadMemory(base + off, &ptr, sizeof(ptr))) return 0;
    // Basic sanity: require non-null and readable HP field before caching.
    if (ptr) {
        int hpDummy=0; if (!SafeReadMemory(ptr + HP_OFFSET, &hpDummy, sizeof(hpDummy))) return 0;
        g_cachedPlayerBase[playerIndex].store(ptr, std::memory_order_release);
    }
    return ptr;
}

void InvalidatePlayerBaseCache() {
    g_cachedPlayerBase[1].store(0, std::memory_order_release);
    g_cachedPlayerBase[2].store(0, std::memory_order_release);
}

// Add these helper functions to better detect state changes
bool IsActionable(short moveID) {
    // Explicit neutral whitelist
    bool neutral = (moveID == IDLE_MOVE_ID || 
                    moveID == WALK_FWD_ID || 
                    moveID == WALK_BACK_ID || 
                    moveID == CROUCH_ID ||
                    moveID == CROUCH_TO_STAND_ID ||
                    // Airborne falling is considered actionable (air actions possible)
                    moveID == FALLING_ID ||
                    // Treat landing variants as actionable immediately
                    moveID == LANDING_ID || moveID == LANDING_1_ID || moveID == LANDING_2_ID || moveID == LANDING_3_ID);

    if (neutral) return true;

    // Explicit inactionable groups from engine
    bool isDash = (moveID == FORWARD_DASH_START_ID || moveID == FORWARD_DASH_RECOVERY_ID ||
                   moveID == BACKWARD_DASH_START_ID || moveID == BACKWARD_DASH_RECOVERY_ID ||
                   moveID == FORWARD_DASH_RECOVERY_SENTINEL_ID);
    bool isGroundTechSeq = (moveID == GROUNDTECH_RECOVERY || moveID == GROUNDTECH_PRE || moveID == GROUNDTECH_START || moveID == GROUNDTECH_END);
    bool isSuperflash = (moveID == GROUND_IC_ID || moveID == AIR_IC_ID);

    bool prohibited = (IsAttackMove(moveID) || 
                       IsBlockstunState(moveID) || 
                       IsHitstun(moveID) || 
                       IsLaunched(moveID) ||
                       IsThrown(moveID) ||
                       IsAirtech(moveID) || 
                       IsGroundtech(moveID) ||
                       IsFrozen(moveID) ||
                       IsRecoilGuard(moveID) ||
                       isDash || isGroundTechSeq || isSuperflash ||
                       moveID == STAND_GUARD_ID || 
                       moveID == CROUCH_GUARD_ID || 
                       moveID == AIR_GUARD_ID);

    if (prohibited) return false;

    // Treat unknown states as NOT actionable by default (stricter) but allow debug override
    static int unknownLogBudget = 0; // refilled periodically elsewhere if needed
    bool result = false;
    if (g_deepFrameAdvDebug.load() && unknownLogBudget < 200) { // limit spam
        LogOut("[ACTIONABLE_DBG] Treating unknown moveID " + std::to_string(moveID) + " as NOT actionable", false);
        ++unknownLogBudget;
    }
    return result;
}

// Note: Wakeup triggers use IsActionable directly; CROUCH_TO_STAND_ID (7) is considered
// actionable so wake actions can fire ASAP when state 96 ends.

bool IsBlockstun(short moveID) {
    // Directly check for core blockstun IDs
    if (moveID == STAND_GUARD_ID || 
        moveID == CROUCH_GUARD_ID || 
        moveID == CROUCH_GUARD_STUN1 ||
        moveID == CROUCH_GUARD_STUN2 || 
        moveID == AIR_GUARD_ID) {
        return true;
    }
    
    // Check the range that includes many standing blockstun states BUT explicitly
    // exclude dash and airtech IDs so follow-up/restore logic does not treat
    // movement/recovery states as stun.
    if (moveID == 150 || moveID == 152 || 
        (moveID >= 140 && moveID <= 149) ||
        (moveID >= 153 && moveID <= 165)) {
        // Movement/recovery IDs inside this broad range must not be blockstun.
        if (moveID == FORWARD_DASH_START_ID ||
            moveID == FORWARD_DASH_RECOVERY_ID ||
            moveID == FORWARD_DASH_RECOVERY_SENTINEL_ID ||
            moveID == BACKWARD_DASH_START_ID ||
            moveID == BACKWARD_DASH_RECOVERY_ID ||
            moveID == FORWARD_AIRTECH ||
            moveID == BACKWARD_AIRTECH) {
            return false; // explicitly exclude
        }
        return true;
    }
    
    return false;
}

bool IsRecoilGuard(short moveID) {
    return moveID == RG_STAND_ID || moveID == RG_CROUCH_ID || moveID == RG_AIR_ID;
}

bool IsEFZWindowActive() {
    HWND fg = GetForegroundWindow();
    if (!fg)
        return false;
    
    // Try with Unicode API first
    WCHAR wideTitle[256] = { 0 };
    GetWindowTextW(fg, wideTitle, sizeof(wideTitle)/sizeof(WCHAR) - 1);
    
    // Case-insensitive comparison for wide strings
    if (_wcsicmp(wideTitle, L"ETERNAL FIGHTER ZERO") == 0 ||
        wcsstr(_wcslwr(wideTitle), L"efz.exe") != NULL ||
        wcsstr(_wcslwr(wideTitle), L"eternal fighter zero") != NULL ||
        wcsstr(_wcslwr(wideTitle), L"revival") != NULL) {
        return true;
    }
    
    // Fallback to ANSI for compatibility
    char title[256] = { 0 };
    GetWindowTextA(fg, title, sizeof(title) - 1);
    std::string t(title);
    std::transform(t.begin(), t.end(), t.begin(), ::toupper);
    
    return t.find("ETERNAL FIGHTER ZERO") != std::string::npos ||
           t.find("EFZ.EXE") != std::string::npos ||
           t.find("ETERNAL FIGHTER ZERO -REVIVAL-") != std::string::npos;
}

void CreateDebugConsole() {
    // Start diagnostic logging
    WriteStartupLog("CreateDebugConsole() started");
    WriteStartupLog("Current code page: " + std::to_string(GetConsoleOutputCP()));
    
    // Create console and ensure success
    WriteStartupLog("Calling AllocConsole()...");
    if (!AllocConsole()) {
        DWORD lastError = GetLastError();
        WriteStartupLog("AllocConsole() failed with error code: " + std::to_string(lastError));
        
        // If AllocConsole fails, try attaching to parent console first
        WriteStartupLog("Attempting AttachConsole(ATTACH_PARENT_PROCESS)...");
        if (!AttachConsole(ATTACH_PARENT_PROCESS)) {
            lastError = GetLastError();
            WriteStartupLog("AttachConsole() failed with error code: " + std::to_string(lastError));
            MessageBoxA(NULL, "Failed to create debug console", "Error", MB_OK | MB_ICONERROR);
            return;
        }
        WriteStartupLog("AttachConsole() succeeded");
    } else {
        WriteStartupLog("AllocConsole() succeeded");
    }
    
    // Redirect stdout/stderr with error checking
    FILE* fp = nullptr;
    WriteStartupLog("Redirecting stdout to CONOUT$...");
    if (freopen_s(&fp, "CONOUT$", "w", stdout) != 0) {
        DWORD lastError = GetLastError();
        WriteStartupLog("stdout redirection failed with error code: " + std::to_string(lastError));
        MessageBoxA(NULL, "Failed to redirect stdout", "Error", MB_OK | MB_ICONERROR);
    } else {
        WriteStartupLog("stdout redirection succeeded");
    }
    
    WriteStartupLog("Redirecting stderr to CONOUT$...");
    if (freopen_s(&fp, "CONOUT$", "w", stderr) != 0) {
        DWORD lastError = GetLastError();
        WriteStartupLog("stderr redirection failed with error code: " + std::to_string(lastError));
        MessageBoxA(NULL, "Failed to redirect stderr", "Error", MB_OK | MB_ICONERROR);
    } else {
        WriteStartupLog("stderr redirection succeeded");
    }
    
    // Clear stream state
    std::cout.clear();
    std::cerr.clear();
    WriteStartupLog("Cleared stream state");
    
    // Set console title with Unicode
    WriteStartupLog("Setting console title...");
    bool titleSet = SetConsoleTitleW(L"EFZ Training Mode") != 0;
    WriteStartupLog("SetConsoleTitleW returned: " + std::to_string(titleSet));
    
    // Set console code page to UTF-8 for proper character display
    WriteStartupLog("Setting console code page to UTF-8...");
    bool cpSet = SetConsoleOutputCP(CP_UTF8) != 0;
    WriteStartupLog("SetConsoleOutputCP returned: " + std::to_string(cpSet));
    
    bool cpInSet = SetConsoleCP(CP_UTF8) != 0;
    WriteStartupLog("SetConsoleCP returned: " + std::to_string(cpInSet));
    
    // Get console handle
    WriteStartupLog("Getting console handle...");
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut == INVALID_HANDLE_VALUE) {
        DWORD lastError = GetLastError();
        WriteStartupLog("GetStdHandle failed with error: " + std::to_string(lastError));
    } else {
        WriteStartupLog("GetStdHandle succeeded");
        
        // Set console mode to enable virtual terminal processing (for ANSI colors)
        WriteStartupLog("Setting console mode...");
        DWORD dwMode = 0;
        if (!GetConsoleMode(hOut, &dwMode)) {
            DWORD lastError = GetLastError();
            WriteStartupLog("GetConsoleMode failed with error: " + std::to_string(lastError));
        } else {
            WriteStartupLog("Current console mode: " + std::to_string(dwMode));
#if defined(EFZ_XP_COMPAT)
                WriteStartupLog("SetConsoleMode skipped: XP compatibility mode disables VT console");
                LogOut("[XP] Console VT mode disabled for XP compatibility build", true);
#else
                if (!SetConsoleMode(hOut, dwMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING | ENABLE_PROCESSED_OUTPUT)) {
                    DWORD lastError = GetLastError();
                    WriteStartupLog("SetConsoleMode failed with error: " + std::to_string(lastError));
                } else {
                    WriteStartupLog("SetConsoleMode succeeded");
                }
#endif
        }
        
        // Set console buffer size for more history
        WriteStartupLog("Setting console buffer size...");
        CONSOLE_SCREEN_BUFFER_INFO csbi;
        if (!GetConsoleScreenBufferInfo(hOut, &csbi)) {
            DWORD lastError = GetLastError();
            WriteStartupLog("GetConsoleScreenBufferInfo failed with error: " + std::to_string(lastError));
        } else {
            COORD size = { csbi.dwSize.X, 2000 }; // Increase buffer height
            if (!SetConsoleScreenBufferSize(hOut, size)) {
                DWORD lastError = GetLastError();
                WriteStartupLog("SetConsoleScreenBufferSize failed with error: " + std::to_string(lastError));
            } else {
                WriteStartupLog("SetConsoleScreenBufferSize succeeded");
            }
        }
    }
    
    // Ensure console window is visible
    WriteStartupLog("Getting console window handle...");
    HWND consoleWindow = GetConsoleWindow();
    if (consoleWindow == NULL) {
        DWORD lastError = GetLastError();
        WriteStartupLog("GetConsoleWindow returned NULL, error: " + std::to_string(lastError));
    } else {
        WriteStartupLog("GetConsoleWindow succeeded, showing window...");
        ShowWindow(consoleWindow, SW_SHOW);
        WriteStartupLog("ShowWindow called");
    }
    
    WriteStartupLog("CreateDebugConsole() completed");
    // Mark console ready for logging and flush pending logs
    SetConsoleReady(true);
    LogOut("[SYSTEM] Console initialization complete", true);
}

void DestroyDebugConsole() {
    // Hide console first
    HWND hWnd = GetConsoleWindow();
    if (hWnd) {
        ShowWindow(hWnd, SW_HIDE);
    }
    // Redirect stdout/stderr to NUL to avoid invalid handles
    FILE* fp = nullptr;
    freopen_s(&fp, "NUL", "w", stdout);
    freopen_s(&fp, "NUL", "w", stderr);
    // Free the console
    FreeConsole();
}

void SetConsoleVisibility(bool visible) {
    if (HWND hWnd = GetConsoleWindow()) {
        ShowWindow(hWnd, visible ? SW_SHOW : SW_HIDE);
    }
}

void ResetFrameCounter() {
    frameCounter = 0;
    startFrameCount = 0;
    LogOut("[SYSTEM] Frame counter reset", true);
}

// REVISED: This function now opens the ImGui menu to the Help tab.
void ShowHotkeyInfo() {
    // If ImGui is enabled, open it to the help tab
    if (Config::GetSettings().useImGui) {
        // Route through the single opener instead of poking ToggleVisibility
        // directly. OpenPracticeMenuDirect is what applies the window check, the
        // gameplay-context gate, and the external-fallback-window path; a raw
        // toggle here would bypass all three the moment this is wired to a
        // hotkey. (It currently has no callers - keep it gated anyway.)
        if (!ImGuiImpl::IsVisible() && !OpenPracticeMenuDirect()) {
            // The opener already logged why, throttled.
            return;
        }
        // Use logical index 4 for Help; map to actual via helper
        ImGuiGui::RequestTopTabAbsolute(4);
        LogOut("[GUI] Opening ImGui to Help tab", true);
    } else {
        LogOut("[GUI] Help shortcut ignored because ImGui is disabled", true);
    }
}

std::string GetKeyName(int virtualKey) {
    // Handle special cases for clarity
    switch (virtualKey) {
        case VK_LEFT: return "Left Arrow";
        case VK_RIGHT: return "Right Arrow";
        case VK_UP: return "Up Arrow";
        case VK_DOWN: return "Down Arrow";
        case VK_RETURN: return "Enter";
        case VK_ESCAPE: return "Escape";
        case VK_SPACE: return "Space";
        case VK_LSHIFT: return "Left Shift";
        case VK_RSHIFT: return "Right Shift";
        case VK_LCONTROL: return "Left Ctrl";
        case VK_RCONTROL: return "Right Ctrl";
        case VK_LMENU: return "Left Alt";
        case VK_RMENU: return "Right Alt";
        case VK_TAB: return "Tab";
        case VK_CAPITAL: return "Caps Lock";
        case VK_BACK: return "Backspace";
        case VK_INSERT: return "Insert";
        case VK_DELETE: return "Delete";
        case VK_HOME: return "Home";
        case VK_END: return "End";
        case VK_PRIOR: return "Page Up";
        case VK_NEXT: return "Page Down";
    }

    // For F-keys and numbers/letters
    if (virtualKey >= VK_F1 && virtualKey <= VK_F24) {
        return "F" + std::to_string(virtualKey - VK_F1 + 1);
    }
    if (virtualKey >= '0' && virtualKey <= '9') {
        return std::string(1, (char)virtualKey);
    }
    if (virtualKey >= 'A' && virtualKey <= 'Z') {
        return std::string(1, (char)virtualKey);
    }

    // Fallback for other keys using system function
    char keyName[256];
    UINT scanCode = MapVirtualKey(virtualKey, MAPVK_VK_TO_VSC);
    if (GetKeyNameTextA(scanCode << 16, keyName, sizeof(keyName)) > 0) {
        return std::string(keyName);
    }

    return "Unknown Key";
}

bool IsDashState(short moveID) {
    return moveID == FORWARD_DASH_START_ID || 
           moveID == FORWARD_DASH_RECOVERY_ID ||
           moveID == BACKWARD_DASH_START_ID || 
           moveID == BACKWARD_DASH_RECOVERY_ID;
}


HWND FindEFZWindow() {
    // Cache & throttle enumeration: only re-enumerate every 120 internal frames or if handle invalid
    static HWND cached = NULL;
    static int lastRefreshFrame = -99999;
    static DWORD ourPid = GetCurrentProcessId();  // Our process ID (constant per instance)
    
    int currentInternal = frameCounter.load();
    if (cached && IsWindow(cached)) {
        // Validate cached window still belongs to our process
        DWORD cachedPid = 0;
        GetWindowThreadProcessId(cached, &cachedPid);
        if (cachedPid == ourPid) {
            return cached;  // Fast path: cached handle is valid and belongs to us
        }
        // Wrong process - invalidate cache
        cached = NULL;
    }
    if (currentInternal - lastRefreshFrame < 120 && cached) {
        return cached; // avoid hammering EnumWindows()
    }
    lastRefreshFrame = currentInternal;

    // Search context: our PID and result pointer
    struct EnumContext {
        DWORD targetPid;
        HWND foundWindow;
    } ctx = { ourPid, NULL };

    EnumWindows([](HWND hwnd, LPARAM lParam) -> BOOL {
        EnumContext* pCtx = reinterpret_cast<EnumContext*>(lParam);
        if (!IsWindowVisible(hwnd)) return TRUE;
        
        // CRITICAL: Only consider windows belonging to OUR process
        DWORD windowPid = 0;
        GetWindowThreadProcessId(hwnd, &windowPid);
        if (windowPid != pCtx->targetPid) return TRUE;  // Not our window, skip
        
        WCHAR wideTitle[256] = {0};
        GetWindowTextW(hwnd, wideTitle, 255);
        if (wcslen(wideTitle) == 0) return TRUE;
        WCHAR lower[256];
        wcscpy_s(lower, wideTitle);
        _wcslwr_s(lower);
        if (wcsstr(lower, L"eternal fighter zero") || wcsstr(lower, L"efz.exe") || wcsstr(lower, L"revival")) {
            pCtx->foundWindow = hwnd;
            return FALSE;  // Found it, stop enumeration
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));
    
    if (ctx.foundWindow) {
        cached = ctx.foundWindow;
    }
    return cached;
}

void UpdateWindowActiveState() {
    if (g_onlineModeActive.load()) return;
    HWND activeWindow = GetForegroundWindow();
    HWND efzWindow = FindEFZWindow();
    
    // Update EFZ window active state
    g_efzWindowActive.store(activeWindow == efzWindow);
    
    // Check if our GUI is active
    g_guiActive.store(menuOpen.load() || ImGuiImpl::IsVisible());
    
    // Log state changes only (not every update)
    static bool prevEfzActive = false;
    static bool prevGuiActive = false;
    
    if (prevEfzActive != g_efzWindowActive.load() || prevGuiActive != g_guiActive.load()) {
        LogOut("[WINDOW] EFZ window active: " + std::to_string(g_efzWindowActive.load()) + 
               ", GUI active: " + std::to_string(g_guiActive.load()), 
               detailedLogging.load());
        
        prevEfzActive = g_efzWindowActive.load();
        prevGuiActive = g_guiActive.load();
    }
}

// Separate function to manage key monitoring based on window focus
void ManageKeyMonitoring() {
    if (g_onlineModeActive.load()) { if (keyMonitorRunning.load()) keyMonitorRunning.store(false); return; }
    bool currentWindowActive = g_efzWindowActive.load();
    bool currentFeaturesEnabled = g_featuresEnabled.load();
    
    // Check if we should start key monitoring
    bool shouldMonitorKeys = currentWindowActive && currentFeaturesEnabled;
    bool isCurrentlyMonitoring = keyMonitorRunning.load();
    
    // Start key monitoring if we should be monitoring but aren't
    if (shouldMonitorKeys && !isCurrentlyMonitoring) {
        LogOut("[SYSTEM] Starting key monitoring - Window active: " + 
               std::to_string(currentWindowActive) + ", Features enabled: " + 
               std::to_string(currentFeaturesEnabled), true);
        RestartKeyMonitoring();
    }
    // Stop key monitoring if we shouldn't be monitoring but are
    else if (!shouldMonitorKeys && isCurrentlyMonitoring) {
        std::string reason = !currentFeaturesEnabled ? "features disabled" : "window inactive";
        LogOut("[SYSTEM] Stopping key monitoring - " + reason, true);
        keyMonitorRunning.store(false);
    }
}

void LifecycleWatcherThread() {
    struct Snapshot {
        bool suspended = false;
        bool exportAvailable = false;
        bool sessionActive = false;
        bool inNetplayMenu = false;
        uint32_t sessionId = 0;
        int source = 0;
        GameMode mode = GameMode::Unknown;
        bool validMode = false;
        bool charactersInitialized = false;
    };

    auto isValidMode = [](GameMode mode) -> bool {
        const Config::Settings& cfg = Config::GetSettings();
        return !cfg.restrictToPracticeMode || (mode == GameMode::Practice);
    };

    Snapshot previous = {};
    bool havePrevious = false;

    while (!g_isShuttingDown.load(std::memory_order_acquire)) {
        RefreshNetplayRuntimeState();

        const NetplayRuntimeState netplayState = GetNetplayRuntimeState();
        const bool shouldSuspend = netplayState.suspendTraining;
        const bool suspendedNow = g_onlineModeActive.load(std::memory_order_acquire);
        if (shouldSuspend && !suspendedNow) {
            LogOut("[NETPLAY] Lifecycle watcher requested suspend: " + GetLastOnlineDetectionReason(), true);
            EnterNetplaySuspend();
        } else if (!shouldSuspend && suspendedNow) {
            LogOut("[NETPLAY] Lifecycle watcher requested resume: " + GetLastOnlineDetectionReason(), true);
            ExitNetplaySuspend();
        }

        Snapshot current = {};
        current.suspended = g_onlineModeActive.load(std::memory_order_acquire);
        current.exportAvailable = netplayState.exportAvailable;
        current.sessionActive = netplayState.sessionActive;
        current.inNetplayMenu = netplayState.inNetplayMenu;
        current.sessionId = netplayState.exportAvailable ? netplayState.exportState.sessionId : 0;
        current.source = static_cast<int>(netplayState.source);
        if (!current.suspended) {
            current.mode = GetCurrentGameMode();
            current.validMode = isValidMode(current.mode);
            current.charactersInitialized = AreCharactersInitialized();
        }

        if (havePrevious) {
            std::ostringstream reason;
            bool needsResync = false;

            if (current.suspended != previous.suspended) {
                reason << (needsResync ? "; " : "")
                       << "suspend " << (previous.suspended ? "1" : "0")
                       << "->" << (current.suspended ? "1" : "0");
                needsResync = true;
            }
            if (current.source != previous.source) {
                reason << (needsResync ? "; " : "")
                       << "source " << NetplayStateSourceName(static_cast<NetplayStateSource>(previous.source))
                       << "->" << NetplayStateSourceName(static_cast<NetplayStateSource>(current.source));
                needsResync = true;
            }
            if (current.exportAvailable != previous.exportAvailable) {
                reason << (needsResync ? "; " : "")
                       << "export " << (previous.exportAvailable ? "1" : "0")
                       << "->" << (current.exportAvailable ? "1" : "0");
                needsResync = true;
            }
            if (current.sessionActive != previous.sessionActive) {
                reason << (needsResync ? "; " : "")
                       << "sessionActive " << (previous.sessionActive ? "1" : "0")
                       << "->" << (current.sessionActive ? "1" : "0");
                needsResync = true;
            }
            if (current.inNetplayMenu != previous.inNetplayMenu) {
                reason << (needsResync ? "; " : "")
                       << "menu " << (previous.inNetplayMenu ? "1" : "0")
                       << "->" << (current.inNetplayMenu ? "1" : "0");
                needsResync = true;
            }
            if (current.exportAvailable && previous.exportAvailable && current.sessionId != previous.sessionId) {
                reason << (needsResync ? "; " : "")
                       << "sessionId " << previous.sessionId << "->" << current.sessionId;
                needsResync = true;
            }
            if (current.mode != previous.mode) {
                reason << (needsResync ? "; " : "")
                       << "mode " << GetGameModeName(previous.mode)
                       << "->" << GetGameModeName(current.mode);
                needsResync = true;
            }
            if (current.validMode != previous.validMode) {
                reason << (needsResync ? "; " : "")
                       << "validMode " << (previous.validMode ? "1" : "0")
                       << "->" << (current.validMode ? "1" : "0");
                needsResync = true;
            }
            if (current.charactersInitialized != previous.charactersInitialized) {
                reason << (needsResync ? "; " : "")
                       << "charsInit " << (previous.charactersInitialized ? "1" : "0")
                       << "->" << (current.charactersInitialized ? "1" : "0");
                needsResync = true;
            }

            if (needsResync) {
                std::ostringstream watcherLog;
                watcherLog << "[LIFECYCLE] Watcher observed transition: " << reason.str()
                           << " suspended=" << (current.suspended ? "1" : "0")
                           << " source=" << NetplayStateSourceName(static_cast<NetplayStateSource>(current.source))
                           << " export=" << (current.exportAvailable ? "1" : "0")
                           << " session=" << (current.sessionActive ? "1" : "0")
                           << " menu=" << (current.inNetplayMenu ? "1" : "0")
                           << " sessionId=" << current.sessionId
                           << " mode=" << GetGameModeName(current.mode)
                           << " validMode=" << (current.validMode ? "1" : "0")
                           << " charsInit=" << (current.charactersInitialized ? "1" : "0");
                LogOut(watcherLog.str(), true);
                RequestRuntimeLifecycleResync(reason.str());
            }

            if (current.inNetplayMenu && !previous.inNetplayMenu) {
                LogOut("[NETPLAY] Lifecycle watcher detected netplay menu entry; auditing residual training state", true);
                AuditNetplayMenuEntryState();
            }
        }

        previous = current;
        havePrevious = true;
        Sleep(current.suspended ? 250 : 150);
    }
}
