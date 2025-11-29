#include "../include/utils/utilities.h"

#include "../include/core/constants.h"
#include "../include/core/logger.h"
#include "../include/core/memory.h"
#include "../include/input/input_handler.h"
#include "../include/core/di_keycodes.h"
#include "../include/game/frame_analysis.h"    // ADD THIS - for IsBlockstunState
#include "../include/game/frame_advantage.h"
#include "../include/utils/config.h"
#include "../include/gui/imgui_impl.h"
#include "../include/gui/imgui_gui.h"
#include "../include/gui/overlay.h"
#include "../include/input/input_handler.h"
#include "../include/game/auto_airtech.h"
#include "../include/game/auto_action.h"
#include "../include/game/frame_monitor.h"
#include "../include/input/input_freeze.h"
#include "../include/game/practice_offsets.h" // For GAMESTATE_OFF_P1_CPU_FLAG, GAMESTATE_OFF_P2_CPU_FLAG
#include <sstream>
#include <iomanip>
#include <iostream>  // Add this include for std::cout and std::cerr
#include <algorithm>  // For std::transform
#include <cwctype>    // For wide character functions
#include <locale>     // For std::locale
#include <fstream>
#include <iomanip>
#include <chrono>
#include <vector>
#include <limits>
#include "../include/game/character_settings.h"
#include "../include/game/game_state.h"
#include "../include/input/input_hook.h"          // For RemoveInputHook
#include "../include/game/collision_hook.h"       // For RemoveCollisionHook
#include "../3rdparty/minhook/include/MinHook.h"  // For MH_DisableHook(MH_ALL_HOOKS)
#include "../include/input/immediate_input.h"

#include "../include/utils/bgm_control.h"
#include "../include/core/globals.h"

std::atomic<bool> g_efzWindowActive(false);
std::atomic<bool> g_guiActive(false);
std::atomic<bool> g_onlineModeActive(false);
// Suppress auto-action clear logging (used for one-shot CS persistent clear)
std::atomic<bool> g_suppressAutoActionClearLogging(false);
// Sticky, one-way hard stop once online is confirmed
static std::atomic<bool> g_hardStoppedOnce{false};

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
    displayData.autoActionPlayer = 0;
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
    displayData.afterBlockUseActionPool    = false;
    displayData.onWakeupUseActionPool      = false;
    displayData.afterHitstunUseActionPool  = false;
    displayData.afterAirtechUseActionPool  = false;
    displayData.onRGUseActionPool          = false;

    // Per-trigger option rows (randomized selection)
    displayData.afterBlockOptionCount = 0;
    displayData.onWakeupOptionCount = 0;
    displayData.afterHitstunOptionCount = 0;
    displayData.afterAirtechOptionCount = 0;
    displayData.onRGOptionCount = 0;
    for (int i = 0; i < MAX_TRIGGER_OPTIONS; ++i) {
        displayData.afterBlockOptions[i]   = { false, ACTION_5A, 0, 0, (int)BASE_ATTACK_5A, 0 };
        displayData.onWakeupOptions[i]     = { false, ACTION_5A, 0, 0, (int)BASE_ATTACK_5A, 0 };
        displayData.afterHitstunOptions[i] = { false, ACTION_5A, 0, 0, (int)BASE_ATTACK_5A, 0 };
        displayData.afterAirtechOptions[i] = { false, ACTION_JA, 0, 0, (int)BASE_ATTACK_JA, 0 };
        displayData.onRGOptions[i]         = { false, ACTION_5A, 0, 0, (int)BASE_ATTACK_5A, 0 };
    }
    
    LogOut("[SYSTEM] DisplayData reset to defaults", true);
}

// NEW: Add feature management functions
void EnableFeatures() {
    if (g_onlineModeActive.load()) return;
    if (g_featuresEnabled.load())
        return;

    LogOut("[SYSTEM] Game in valid mode. Enabling patches and overlays.", true);
    
    // Reset display data to defaults when entering valid mode
    ResetDisplayDataToDefaults();
    // Start centralized immediate input writer (64fps)
    ImmediateInput::Start();

    // Apply patches if the feature is enabled
    if (autoAirtechEnabled.load()) {
        ApplyAirtechPatches();
    }

    g_featuresEnabled.store(true);

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
    
    // CRITICAL: Restore normal control flags when leaving Practice mode
    // to prevent control swap issues in other modes
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

    autoActionEnabled.store(false);
    triggerAfterBlockEnabled.store(false);
    triggerOnWakeupEnabled.store(false);
    triggerAfterHitstunEnabled.store(false);
    triggerAfterAirtechEnabled.store(false);

    // Fully clear any in-flight auto-action internal state (delays, cooldowns, control overrides)
    ClearAllAutoActionTriggers();

    // Clear ALL visual overlays
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
    
    // Close the menu if it's open
    if (ImGuiImpl::IsVisible()) {
        ImGuiImpl::ToggleVisibility();
    }

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
    s_posCacheTickMs.store(GetTickCount64(), std::memory_order_relaxed);
}

bool TryGetCachedYPositions(double &p1Y, double &p2Y, unsigned int maxAgeMs) {
    unsigned long long t = s_posCacheTickMs.load(std::memory_order_relaxed);
    if (t == 0) return false;
    unsigned long long now = GetTickCount64();
    if (now - t > static_cast<unsigned long long>(maxAgeMs)) return false;
    p1Y = s_cachedP1Y.load(std::memory_order_relaxed);
    p2Y = s_cachedP2Y.load(std::memory_order_relaxed);
    return true;
}

// Cooperatively stop mod activity when entering online play, then hard-stop all hooks/threads.
void EnterOnlineMode() {
    // Ensure we only run once
    bool wasOnline = g_onlineModeActive.exchange(true);
    if (g_hardStoppedOnce.load()) return;

    LogOut("[ONLINE] Entering online mode: disabling mod features, unhooking, and stopping threads", true);
    // Stop immediate input writer
    ImmediateInput::Stop();

    // Stop any active buffer/index freezing immediately
    StopBufferFreezing();
    // Stop RF freezing loop from acting
    StopRFFreeze();

    // Disable auto features and clear triggers/state
    autoActionEnabled.store(false);
    triggerAfterBlockEnabled.store(false);
    triggerOnWakeupEnabled.store(false);
    triggerAfterHitstunEnabled.store(false);
    triggerAfterAirtechEnabled.store(false);
    ClearAllAutoActionTriggers();

    // Disable features globally (turns off overlays, patches, etc.)
    if (g_featuresEnabled.load()) {
        DisableFeatures();
    }

    // Stop key monitoring
    if (keyMonitorRunning.load()) {
        keyMonitorRunning.store(false);
    }
    // Stop RF freeze worker thread entirely
    StopRFFreezeThread();

    // Stop BGM suppression poller
    StopBGMSuppressionPoller();
    SetBGMSuppressed(false);

    // Hide overlays/GUI
    if (ImGuiImpl::IsVisible()) {
        ImGuiImpl::ToggleVisibility();
    }
    DirectDrawHook::ClearAllMessages();

    // Proactively destroy the debug console so nothing further prints and stop buffering
    DestroyDebugConsole();
    SetConsoleReady(false);

    // --- HARD STOP: Remove hooks and stop rendering ---
    // 1) Unhook EndScene and any D3D9 overlay work
    try {
        DirectDrawHook::ShutdownD3D9();
    } catch (...) { /* swallow */ }

    // 2) Remove input and collision hooks
    try {
        RemoveInputHook();
    } catch (...) { /* swallow */ }
    try {
        RemoveCollisionHook();
    } catch (...) { /* swallow */ }

    // 3) Disable any remaining MinHook hooks (belt-and-suspenders)
    // Avoid Uninitialize at runtime; just disable all hooks safely.
    MH_DisableHook(MH_ALL_HOOKS);

    // 4) Ensure any UI/overlay state is fully cleared
    try {
        DirectDrawHook::Shutdown(); // clears message queues as well
    } catch (...) { /* swallow */ }

    // 5) Prevent any re-initialization attempts for the rest of the process lifetime
    g_hardStoppedOnce.store(true);

    LogOut("[ONLINE] Hard stop complete: hooks removed, threads parked, overlays cleared", true);

    // Optional: Self-unload the DLL to fully detach from the process once we're safely quiesced
    // Guard: only if we have a module handle available
    if (g_hSelfModule) {
        try {
            std::thread([]{
                // Small grace delay to ensure any tail work finishes
                Sleep(250);
                HMODULE h = g_hSelfModule;
                // Use FreeLibraryAndExitThread to safely unload this module from a non-DllMain context
                FreeLibraryAndExitThread(h, 0);
            }).detach();
            LogOut("[ONLINE] Self-unload initiated", true);
        } catch (...) {
            // If spawning fails, we simply remain loaded but inert
            LogOut("[ONLINE] Self-unload spawn failed; remaining parked", true);
        }
    }
}

// Public helper: permanently clear all triggers so they stay disabled until user re-enables
void ClearAllTriggersPersistently() {
    static uint64_t s_lastClearTick = 0; // throttle identical spam bursts
    uint64_t nowTick = GetTickCount64();
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

// Create a function that writes to a log file without requiring the console
void WriteStartupLog(const std::string& message) {
    if (!inStartupPhase) return; // Skip if we're past startup
    
    try {
        // Open log file in append mode
        if (startupLogPath.empty()) {
            char path[MAX_PATH] = {0};
            GetModuleFileNameA(NULL, path, MAX_PATH);
            std::string exePath(path);
            startupLogPath = exePath.substr(0, exePath.find_last_of("\\/")) + "\\efz_startup.log";
        }
        
        // Open file and append message with timestamp
        std::ofstream logFile(startupLogPath, std::ios::app);
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

// Multi-action pools per trigger (disabled by default)
std::atomic<uint32_t> triggerAfterBlockActionPoolMask{0};
std::atomic<uint32_t> triggerOnWakeupActionPoolMask{0};
std::atomic<uint32_t> triggerAfterHitstunActionPoolMask{0};
std::atomic<uint32_t> triggerAfterAirtechActionPoolMask{0};
std::atomic<uint32_t> triggerOnRGActionPoolMask{0};
std::atomic<bool>     triggerAfterBlockUsePool{false};
std::atomic<bool>     triggerOnWakeupUsePool{false};
std::atomic<bool>     triggerAfterHitstunUsePool{false};
std::atomic<bool>     triggerAfterAirtechUsePool{false};
std::atomic<bool>     triggerOnRGUsePool{false};

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

// Individual strength settings (0=A, 1=B, 2=C)
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

// Debug/experimental: allow buffering (pre-freeze) of wakeup specials/supers/dashes instead of f1 injection
std::atomic<bool> g_wakeBufferingEnabled{false};

// Global toggle: enable/disable Counter RG early-restore behavior (default OFF)
std::atomic<bool> g_counterRGEnabled{false};

// UI: gate for the regular Frame Advantage overlay (default ON)
std::atomic<bool> g_showFrameAdvantageOverlay{true};

// Deep frame advantage instrumentation toggle
std::atomic<bool> g_deepFrameAdvDebug{false};

void EnsureLocaleConsistency() {
    static bool localeSet = false;
    if (!localeSet) {
        std::locale::global(std::locale("C"));
        localeSet = true;
    }
}

std::string FormatPosition(double x, double y) {
    std::locale::global(std::locale("C")); 
    // This ensures consistent decimal point format
    std::stringstream ss;
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
    std::locale::global(std::locale("C")); 
    
    // Directly check for core blockstun IDs
    if (moveID == STAND_GUARD_ID || 
        moveID == CROUCH_GUARD_ID || 
        moveID == CROUCH_GUARD_STUN1 ||
        moveID == CROUCH_GUARD_STUN2 || 
        moveID == AIR_GUARD_ID) {
        return true;
    }
    
    // Check the range that includes many standing blockstun states BUT explicitly
    // exclude dash related IDs (forward/back dash start & recovery + sentinel) so the
    // auto-action dash follow-up & restore logic does not treat active dashes as stun.
    if (moveID == 150 || moveID == 152 || 
        (moveID >= 140 && moveID <= 149) ||
        (moveID >= 153 && moveID <= 165)) {
        // Forward/back dash IDs must not be blockstun.
        if (moveID == FORWARD_DASH_START_ID ||
            moveID == FORWARD_DASH_RECOVERY_ID ||
            moveID == FORWARD_DASH_RECOVERY_SENTINEL_ID ||
            moveID == BACKWARD_DASH_START_ID ||
            moveID == BACKWARD_DASH_RECOVERY_ID) {
            return false; // explicitly exclude
        }
        return true;
    }
    
    return false;
}

bool IsRecoilGuard(short moveID) {
    std::locale::global(std::locale("C")); 
    // This ensures consistent decimal point format
    return moveID == RG_STAND_ID || moveID == RG_CROUCH_ID || moveID == RG_AIR_ID;
}

bool IsEFZWindowActive() {
    std::locale::global(std::locale("C")); 
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
    
    // Ensure C locale for consistency
    std::locale::global(std::locale("C"));
    WriteStartupLog("Locale set to C");
    
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
            if (!SetConsoleMode(hOut, dwMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING | ENABLE_PROCESSED_OUTPUT)) {
                DWORD lastError = GetLastError();
                WriteStartupLog("SetConsoleMode failed with error: " + std::to_string(lastError));
            } else {
                WriteStartupLog("SetConsoleMode succeeded");
            }
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
    
    // Test that console is working by writing directly to it
    WriteStartupLog("Testing console output...");
    try {
        std::cout << "Console initialization complete!" << std::endl;
        WriteStartupLog("Console test output successful");
    } catch (const std::exception& e) {
        WriteStartupLog("Console test output failed: " + std::string(e.what()));
    } catch (...) {
        WriteStartupLog("Console test output failed with unknown exception");
    }
    
    WriteStartupLog("CreateDebugConsole() completed");
    // Mark console ready for logging and flush pending logs
    SetConsoleReady(true);
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
        if (!ImGuiImpl::IsVisible()) {
            ImGuiImpl::ToggleVisibility();
        }
        ImGuiGui::guiState.requestedTab = 2; // Request the Help tab
        LogOut("[GUI] Opening ImGui to Help tab", true);
    } else {
        // Fallback for legacy dialog
        LogOut("[GUI] ImGui not enabled, showing legacy hotkey dialog", true);
        MessageBoxA(NULL, "Hotkeys:\n\nMove: Arrow Keys\nAttack: A, S, D\nJump: W\nSpecial: Q, E\nPause: P\nToggle Debug: F1\nShow Frame Data: F2\nShow Hitboxes: F3\nShow HUD: F4\nShow Console: F5", "Hotkey Info", MB_OK | MB_ICONINFORMATION);
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
    std::locale::global(std::locale("C")); 
    // This ensures consistent decimal point format
    return moveID == FORWARD_DASH_START_ID || 
           moveID == FORWARD_DASH_RECOVERY_ID ||
           moveID == BACKWARD_DASH_START_ID || 
           moveID == BACKWARD_DASH_RECOVERY_ID;
}


// Add this function after the IsEFZWindowActive() function
HWND FindEFZWindow() {
    // Cache & throttle enumeration: only re-enumerate every 120 internal frames or if handle invalid
    static HWND cached = NULL;
    static int lastRefreshFrame = -99999;
    int currentInternal = frameCounter.load();
    if (cached && IsWindow(cached)) {
        // Fast path use cached
        return cached;
    }
    if (currentInternal - lastRefreshFrame < 120) {
        return cached; // avoid hammering EnumWindows()
    }
    lastRefreshFrame = currentInternal;

    HWND foundWindow = NULL;
    EnumWindows([](HWND hwnd, LPARAM lParam) -> BOOL {
        if (!IsWindowVisible(hwnd)) return TRUE;
        WCHAR wideTitle[256] = {0};
        GetWindowTextW(hwnd, wideTitle, 255);
        if (wcslen(wideTitle) == 0) return TRUE;
        WCHAR lower[256];
        wcscpy_s(lower, wideTitle);
        _wcslwr_s(lower);
        if (wcsstr(lower, L"eternal fighter zero") || wcsstr(lower, L"efz.exe") || wcsstr(lower, L"revival")) {
            *reinterpret_cast<HWND*>(lParam) = hwnd;
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&foundWindow));
    if (foundWindow) {
        cached = foundWindow;
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
