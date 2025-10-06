#include "../include/gui/gui.h"
#include "../include/core/constants.h"
#include "../include/core/memory.h"
#include "../include/utils/utilities.h"

#include "../include/core/logger.h"
#include "../include/utils/config.h"
#include "../include/gui/imgui_impl.h"  // Add this include
#include "../include/game/character_settings.h" // Add this include for ImGui dialog
#include <windows.h>
#include <string>
#include <thread>
#include <commctrl.h>

// Add this link to the Common Controls library
#pragma comment(lib, "comctl32.lib")

void OpenMenu() {
    // Initialize common controls for tab control support
    INITCOMMONCONTROLSEX icc;
    icc.dwSize = sizeof(INITCOMMONCONTROLSEX);
    icc.dwICC = ICC_TAB_CLASSES;
    InitCommonControlsEx(&icc);

    // Check if we're in EFZ window
    UpdateWindowActiveState();
    if (!g_efzWindowActive.load()) {
        LogOut("[GUI] EFZ window not active, cannot open menu", true);
        return;
    }

    // Don't open menu if it's already open
    if (menuOpen) {
        LogOut("[GUI] Menu already open", detailedLogging.load());
        return;
    }

    menuOpen = true;
    LogOut("[GUI] Opening config menu", detailedLogging.load()); // Use detailed logging

    // Check which UI system to use
    if (Config::GetSettings().useImGui) {
        // Use ImGui interface
        LogOut("[GUI] Using ImGui interface as per config", detailedLogging.load()); // Use detailed logging
        ImGuiImpl::ToggleVisibility();
    } else {
        // Use legacy Win32 dialog
        LogOut("[GUI] Using legacy dialog as per config", true);
        
        // Get current values from memory
        uintptr_t base = GetEFZBase();
        
        if (base) {
            // Read current values into displayData
            uintptr_t hpAddr1 = ResolvePointer(base, EFZ_BASE_OFFSET_P1, HP_OFFSET);
            uintptr_t meterAddr1 = ResolvePointer(base, EFZ_BASE_OFFSET_P1, METER_OFFSET);
            uintptr_t rfAddr1 = ResolvePointer(base, EFZ_BASE_OFFSET_P1, RF_OFFSET);
            uintptr_t xAddr1 = ResolvePointer(base, EFZ_BASE_OFFSET_P1, XPOS_OFFSET);
            uintptr_t yAddr1 = ResolvePointer(base, EFZ_BASE_OFFSET_P1, YPOS_OFFSET);
            
            if (hpAddr1) memcpy(&displayData.hp1, (void*)hpAddr1, sizeof(WORD));
            if (meterAddr1) memcpy(&displayData.meter1, (void*)meterAddr1, sizeof(WORD));
            if (rfAddr1) memcpy(&displayData.rf1, (void*)rfAddr1, sizeof(double));
            if (xAddr1) memcpy(&displayData.x1, (void*)xAddr1, sizeof(double));
            if (yAddr1) memcpy(&displayData.y1, (void*)yAddr1, sizeof(double));
            
            uintptr_t hpAddr2 = ResolvePointer(base, EFZ_BASE_OFFSET_P2, HP_OFFSET);
            uintptr_t meterAddr2 = ResolvePointer(base, EFZ_BASE_OFFSET_P2, METER_OFFSET);
            uintptr_t rfAddr2 = ResolvePointer(base, EFZ_BASE_OFFSET_P2, RF_OFFSET);
            uintptr_t xAddr2 = ResolvePointer(base, EFZ_BASE_OFFSET_P2, XPOS_OFFSET);
            uintptr_t yAddr2 = ResolvePointer(base, EFZ_BASE_OFFSET_P2, YPOS_OFFSET);
            
            if (hpAddr2) memcpy(&displayData.hp2, (void*)hpAddr2, sizeof(WORD));
            if (meterAddr2) memcpy(&displayData.meter2, (void*)meterAddr2, sizeof(WORD));
            if (rfAddr2) memcpy(&displayData.rf2, (void*)rfAddr2, sizeof(double));
            if (xAddr2) memcpy(&displayData.x2, (void*)xAddr2, sizeof(double));
            if (yAddr2) memcpy(&displayData.y2, (void*)yAddr2, sizeof(double));
            
            // Update settings from atomic variables
            displayData.autoAirtech = autoAirtechEnabled.load();
            displayData.airtechDirection = autoAirtechDirection.load();
            displayData.autoJump = autoJumpEnabled.load();
            displayData.jumpDirection = jumpDirection.load();
            displayData.jumpTarget = jumpTarget.load();
            
            displayData.autoAction = autoActionEnabled.load();
            displayData.autoActionType = autoActionType.load();
            displayData.autoActionCustomID = autoActionCustomID.load();
            displayData.autoActionPlayer = autoActionPlayer.load();
            
            displayData.triggerAfterBlock = triggerAfterBlockEnabled.load();
            displayData.triggerOnWakeup = triggerOnWakeupEnabled.load();
            displayData.triggerAfterHitstun = triggerAfterHitstunEnabled.load();
            displayData.triggerAfterAirtech = triggerAfterAirtechEnabled.load();
            displayData.triggerOnRG = triggerOnRGEnabled.load();
            
            displayData.delayAfterBlock = triggerAfterBlockDelay.load();
            displayData.delayOnWakeup = triggerOnWakeupDelay.load();
            displayData.delayAfterHitstun = triggerAfterHitstunDelay.load();
            displayData.delayAfterAirtech = triggerAfterAirtechDelay.load();
            displayData.delayOnRG = triggerOnRGDelay.load();

            // Macro slot selections
            displayData.macroSlotAfterBlock = triggerAfterBlockMacroSlot.load();
            displayData.macroSlotOnWakeup = triggerOnWakeupMacroSlot.load();
            displayData.macroSlotAfterHitstun = triggerAfterHitstunMacroSlot.load();
            displayData.macroSlotAfterAirtech = triggerAfterAirtechMacroSlot.load();
            displayData.macroSlotOnRG = triggerOnRGMacroSlot.load();
        }
        else {
            LogOut("[GUI] Failed to get game base address", true);
            menuOpen = false;
            return;
        }
        
        // Show the dialog
        ShowEditDataDialog(NULL);
    }
}

void ApplySettings(DisplayData* data) {
    if (!data) {
        LogOut("[GUI] ApplySettings called with null data", true);
        return;
    }

    try {
        // Save GUI values to global variables
        autoAirtechEnabled = data->autoAirtech;
        autoAirtechDirection = data->airtechDirection;
        autoAirtechDelay.store(data->airtechDelay);
        
        // CRITICAL: Make sure jump settings are applied to atomic variables
        autoJumpEnabled.store(data->autoJump);
        jumpDirection.store(data->jumpDirection);
        jumpTarget.store(data->jumpTarget);
        
        // Update auto-action system settings
        autoActionEnabled.store(data->autoAction);
        autoActionPlayer.store(data->autoActionPlayer);
        
        // Individual trigger settings
        triggerAfterBlockEnabled.store(data->triggerAfterBlock);
        triggerOnWakeupEnabled.store(data->triggerOnWakeup);
        triggerAfterHitstunEnabled.store(data->triggerAfterHitstun);
        triggerAfterAirtechEnabled.store(data->triggerAfterAirtech);
    triggerOnRGEnabled.store(data->triggerOnRG);
        
        // Individual action settings
        triggerAfterBlockAction.store(data->actionAfterBlock);
        triggerOnWakeupAction.store(data->actionOnWakeup);
        triggerAfterHitstunAction.store(data->actionAfterHitstun);
        triggerAfterAirtechAction.store(data->actionAfterAirtech);
    triggerOnRGAction.store(data->actionOnRG);
        
        // Individual delay settings
        triggerAfterBlockDelay.store(data->delayAfterBlock);
        triggerOnWakeupDelay.store(data->delayOnWakeup);
        triggerAfterHitstunDelay.store(data->delayAfterHitstun);
        triggerAfterAirtechDelay.store(data->delayAfterAirtech);
    triggerOnRGDelay.store(data->delayOnRG);

    // Macro slot selections
    triggerAfterBlockMacroSlot.store(data->macroSlotAfterBlock);
    triggerOnWakeupMacroSlot.store(data->macroSlotOnWakeup);
    triggerAfterHitstunMacroSlot.store(data->macroSlotAfterHitstun);
    triggerAfterAirtechMacroSlot.store(data->macroSlotAfterAirtech);
    triggerOnRGMacroSlot.store(data->macroSlotOnRG);
        
        // MISSING CODE: Store custom moveID values
        triggerAfterBlockCustomID.store(data->customAfterBlock);
        triggerOnWakeupCustomID.store(data->customOnWakeup);
        triggerAfterHitstunCustomID.store(data->customAfterHitstun);
        triggerAfterAirtechCustomID.store(data->customAfterAirtech);
        
        // NEW: Store strength values
        triggerAfterBlockStrength.store(data->strengthAfterBlock);
        triggerOnWakeupStrength.store(data->strengthOnWakeup);
        triggerAfterHitstunStrength.store(data->strengthAfterHitstun);
        triggerAfterAirtechStrength.store(data->strengthAfterAirtech);
        
        // Update the client memory
        uintptr_t base = GetEFZBase();
        if (base) {
            // Update everything EXCEPT RF values
            UpdatePlayerValuesExceptRF(base, EFZ_BASE_OFFSET_P1, EFZ_BASE_OFFSET_P2);
            
            // Add this line to apply character-specific values
            CharacterSettings::ApplyCharacterValues(base, *data);
            
            // Handle RF values separately using the robust method
            if (!SetRFValuesDirect(data->rf1, data->rf2)) {
                LogOut("[GUI] Failed to set RF values directly, starting freeze thread", true);
                StartRFFreeze(data->rf1, data->rf2);
            }
        }

    // Continuous Recovery (legacy globals retained for compatibility)
    g_contRecoveryEnabled.store(data->continuousRecoveryEnabled);
    g_contRecoveryApplyTo.store(data->continuousRecoveryApplyTo);
    g_contRecHpMode.store(data->recoveryHpMode);
    g_contRecHpCustom.store(data->recoveryHpCustom);
    g_contRecMeterMode.store(data->recoveryMeterMode);
    g_contRecMeterCustom.store(data->recoveryMeterCustom);
    g_contRecRfMode.store(data->recoveryRfMode);
    g_contRecRfCustom.store(data->recoveryRfCustom);
    g_contRecRfForceBlueIC.store(data->recoveryRfForceBlueIC);

    // NEW: Per-player Continuous Recovery atomics
    // P1
    g_contRecEnabledP1.store(data->p1ContinuousRecoveryEnabled);
    g_contRecHpModeP1.store(data->p1RecoveryHpMode);
    g_contRecHpCustomP1.store(data->p1RecoveryHpCustom);
    g_contRecMeterModeP1.store(data->p1RecoveryMeterMode);
    g_contRecMeterCustomP1.store(data->p1RecoveryMeterCustom);
    g_contRecRfModeP1.store(data->p1RecoveryRfMode);
    g_contRecRfCustomP1.store(data->p1RecoveryRfCustom);
    g_contRecRfForceBlueICP1.store(data->p1RecoveryRfForceBlueIC);
    // P2
    g_contRecEnabledP2.store(data->p2ContinuousRecoveryEnabled);
    g_contRecHpModeP2.store(data->p2RecoveryHpMode);
    g_contRecHpCustomP2.store(data->p2RecoveryHpCustom);
    g_contRecMeterModeP2.store(data->p2RecoveryMeterMode);
    g_contRecMeterCustomP2.store(data->p2RecoveryMeterCustom);
    g_contRecRfModeP2.store(data->p2RecoveryRfMode);
    g_contRecRfCustomP2.store(data->p2RecoveryRfCustom);
    g_contRecRfForceBlueICP2.store(data->p2RecoveryRfForceBlueIC);
        
        LogOut("[GUI] All settings applied successfully", detailedLogging.load()); // Use detailed logging
    } catch (const std::exception& e) {
        LogOut("[GUI] Exception in ApplySettings: " + std::string(e.what()), true);
    } catch (...) {
        LogOut("[GUI] Unknown exception in ApplySettings", true);
    }
}

