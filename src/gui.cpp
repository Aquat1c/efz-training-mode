#include "../include/gui.h"
#include "../include/constants.h"
#include "../include/memory.h"
#include "../include/utilities.h"
#include "../include/logger.h"
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
    if (!IsEFZWindowActive()) {
        LogOut("[GUI] EFZ window not active, cannot open menu", true);
        return;
    }

    // Don't open menu if it's already open
    if (menuOpen) {
        LogOut("[GUI] Menu already open", detailedLogging);
        return;
    }

    menuOpen = true;
    LogOut("[GUI] Opening config menu", true);

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
        
        displayData.delayAfterBlock = triggerAfterBlockDelay.load();
        displayData.delayOnWakeup = triggerOnWakeupDelay.load();
        displayData.delayAfterHitstun = triggerAfterHitstunDelay.load();
        displayData.delayAfterAirtech = triggerAfterAirtechDelay.load();
    }
    else {
        LogOut("[GUI] Failed to get game base address", true);
        menuOpen = false;
        return;
    }
    
    // Show the dialog
    ShowEditDataDialog(NULL);
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
        
        // Individual action settings
        triggerAfterBlockAction.store(data->actionAfterBlock);
        triggerOnWakeupAction.store(data->actionOnWakeup);
        triggerAfterHitstunAction.store(data->actionAfterHitstun);
        triggerAfterAirtechAction.store(data->actionAfterAirtech);
        
        // Individual delay settings
        triggerAfterBlockDelay.store(data->delayAfterBlock);
        triggerOnWakeupDelay.store(data->delayOnWakeup);
        triggerAfterHitstunDelay.store(data->delayAfterHitstun);
        triggerAfterAirtechDelay.store(data->delayAfterAirtech);
        
        // MISSING CODE: Store custom moveID values
        triggerAfterBlockCustomID.store(data->customAfterBlock);
        triggerOnWakeupCustomID.store(data->customOnWakeup);
        triggerAfterHitstunCustomID.store(data->customAfterHitstun);
        triggerAfterAirtechCustomID.store(data->customAfterAirtech);
        
        // Update the client memory
        uintptr_t base = GetEFZBase();
        if (base) {
            // Update everything EXCEPT RF values
            UpdatePlayerValuesExceptRF(base, EFZ_BASE_OFFSET_P1, EFZ_BASE_OFFSET_P2);
            
            // Handle RF values separately using the robust method
            if (!SetRFValuesDirect(data->rf1, data->rf2)) {
                LogOut("[GUI] Failed to set RF values directly, starting freeze thread", true);
                StartRFFreeze(data->rf1, data->rf2);
            }
        }
        
        LogOut("[GUI] All settings applied successfully", true);
    } catch (const std::exception& e) {
        LogOut("[GUI] Exception in ApplySettings: " + std::string(e.what()), true);
    } catch (...) {
        LogOut("[GUI] Unknown exception in ApplySettings", true);
    }
}

