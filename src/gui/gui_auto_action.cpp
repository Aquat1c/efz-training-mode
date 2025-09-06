#include "../include/gui/imgui_gui.h"  // Include this for IM_ARRAYSIZE macro
#include "../include/gui/gui.h"
#include "../include/core/constants.h"
#include "../include/utils/utilities.h"
#include "../include/input/motion_system.h"
#include "../include/input/motion_constants.h"
#include "../include/core/logger.h"
#include "../include/core/memory.h"
#include <windows.h>
#include <commctrl.h>

// Maps combobox indices to action type constants 
extern "C" const int ComboIndexToActionType[] = {
    ACTION_5A,          // 0 = 5A
    ACTION_5B,          // 1 = 5B
    ACTION_5C,          // 2 = 5C
    ACTION_2A,          // 3 = 2A
    ACTION_2B,          // 4 = 2B
    ACTION_2C,          // 5 = 2C
    ACTION_JA,          // 6 = j.A
    ACTION_JB,          // 7 = j.B
    ACTION_JC,          // 8 = j.C
    ACTION_QCF,         // 9 = 236 (QCF)
    ACTION_DP,          // 10 = 623 (DP)
    ACTION_QCB,         // 11 = 214 (QCB)
    ACTION_421,         // 12 = 421 (Half-circle Down)
    ACTION_SUPER1,      // 13 = 41236 (HCF)
    ACTION_SUPER2,      // 14 = 63214 (HCB)
    ACTION_236236,      // 15 = 236236 (Double QCF)
    ACTION_214214,      // 16 = 214214 (Double QCB)
    ACTION_JUMP,        // 17 = Jump
    ACTION_BACKDASH,    // 18 = Backdash
    ACTION_FORWARD_DASH,// 19 = Forward Dash
    ACTION_BLOCK,       // 20 = Block
    ACTION_CUSTOM       // 21 = Custom ID
};

// Helper function to convert action type to combobox index
static int ActionTypeToComboIndex(int actionType) {
    for (int i = 0; i < IM_ARRAYSIZE(ComboIndexToActionType); i++) {
        if (ComboIndexToActionType[i] == actionType) {
            return i;
        }
    }
    return 0; // Default to 5A
}

void AutoActionPage_CreateContent(HWND hParent, DisplayData* pData) {
    // Master enable checkbox
    HWND hAutoActionCheck = CreateWindowExA(0, "BUTTON", "Enable Auto Action System", 
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
        50, 20, 250, 25, 
        hParent, (HMENU)IDC_AUTOACTION_ENABLE, GetModuleHandle(NULL), NULL);
    SendMessage(hAutoActionCheck, BM_SETCHECK, autoActionEnabled.load() ? BST_CHECKED : BST_UNCHECKED, 0);

    // Player target (applies to all triggers)
    CreateWindowExA(0, "STATIC", "Apply To:", 
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        50, 55, 80, 25, hParent, NULL, GetModuleHandle(NULL), NULL);

    HWND hPlayerCombo = CreateWindowExA(0, "COMBOBOX", "", 
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
        140, 55, 200, 120, hParent, (HMENU)IDC_AUTOACTION_PLAYER, GetModuleHandle(NULL), NULL);

    SendMessageA(hPlayerCombo, CB_ADDSTRING, 0, (LPARAM)"P1 Only");
    SendMessageA(hPlayerCombo, CB_ADDSTRING, 0, (LPARAM)"P2 Only");
    SendMessageA(hPlayerCombo, CB_ADDSTRING, 0, (LPARAM)"Both Players");
    SendMessage(hPlayerCombo, CB_SETCURSEL, autoActionPlayer.load() - 1, 0);

    int yPos = 90;
    char delayText[8];

    // After Block trigger - REMOVE Custom ID field
    HWND hAfterBlockCheck = CreateWindowExA(0, "BUTTON", "After Block:", 
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
        50, yPos, 120, 25, hParent, (HMENU)IDC_TRIGGER_AFTER_BLOCK_CHECK, GetModuleHandle(NULL), NULL);
    SendMessage(hAfterBlockCheck, BM_SETCHECK, triggerAfterBlockEnabled.load() ? BST_CHECKED : BST_UNCHECKED, 0);

    HWND hAfterBlockAction = CreateWindowExA(0, "COMBOBOX", "", 
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
        180, yPos, 120, 200, hParent, (HMENU)IDC_TRIGGER_AFTER_BLOCK_ACTION, GetModuleHandle(NULL), NULL);

    // When adding options to the action combos, add the special moves:
    SendMessageA(hAfterBlockAction, CB_ADDSTRING, 0, (LPARAM)"5A");
    SendMessageA(hAfterBlockAction, CB_ADDSTRING, 0, (LPARAM)"5B");
    SendMessageA(hAfterBlockAction, CB_ADDSTRING, 0, (LPARAM)"5C");
    SendMessageA(hAfterBlockAction, CB_ADDSTRING, 0, (LPARAM)"2A");
    SendMessageA(hAfterBlockAction, CB_ADDSTRING, 0, (LPARAM)"2B");
    SendMessageA(hAfterBlockAction, CB_ADDSTRING, 0, (LPARAM)"2C");
    SendMessageA(hAfterBlockAction, CB_ADDSTRING, 0, (LPARAM)"j.A");
    SendMessageA(hAfterBlockAction, CB_ADDSTRING, 0, (LPARAM)"j.B");
    SendMessageA(hAfterBlockAction, CB_ADDSTRING, 0, (LPARAM)"j.C");

    // Add special moves here
    SendMessageA(hAfterBlockAction, CB_ADDSTRING, 0, (LPARAM)"QCF");
    SendMessageA(hAfterBlockAction, CB_ADDSTRING, 0, (LPARAM)"DP");
    SendMessageA(hAfterBlockAction, CB_ADDSTRING, 0, (LPARAM)"QCB");
    SendMessageA(hAfterBlockAction, CB_ADDSTRING, 0, (LPARAM)"Super 1");
    SendMessageA(hAfterBlockAction, CB_ADDSTRING, 0, (LPARAM)"Super 2");
    SendMessageA(hAfterBlockAction, CB_ADDSTRING, 0, (LPARAM)"641236 Super");
    
    // Then add the remaining options
    SendMessageA(hAfterBlockAction, CB_ADDSTRING, 0, (LPARAM)"Jump");
    SendMessageA(hAfterBlockAction, CB_ADDSTRING, 0, (LPARAM)"Backdash");
    SendMessageA(hAfterBlockAction, CB_ADDSTRING, 0, (LPARAM)"Block");
    // REMOVED: Custom option is removed since we don't support custom moveIDs anymore
    
    SendMessage(hAfterBlockAction, CB_SETCURSEL, 
        ActionTypeToComboIndex(triggerAfterBlockAction.load()), 0);

    CreateWindowExA(0, "STATIC", "Delay:", WS_CHILD | WS_VISIBLE | SS_LEFT,
        310, yPos, 40, 25, hParent, NULL, GetModuleHandle(NULL), NULL);

    sprintf_s(delayText, "%d", triggerAfterBlockDelay.load());
    CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", delayText, 
        WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NUMBER,
        355, yPos, 40, 25, hParent, (HMENU)IDC_TRIGGER_AFTER_BLOCK_DELAY, GetModuleHandle(NULL), NULL);

    // REMOVED: Custom Move ID field

    // After Hitstun trigger
    yPos += 35;
    HWND hAfterHitstunCheck = CreateWindowExA(0, "BUTTON", "After Hitstun:", 
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
        50, yPos, 120, 25, hParent, (HMENU)IDC_TRIGGER_AFTER_HITSTUN_CHECK, GetModuleHandle(NULL), NULL);
    SendMessage(hAfterHitstunCheck, BM_SETCHECK, triggerAfterHitstunEnabled.load() ? BST_CHECKED : BST_UNCHECKED, 0);

    HWND hAfterHitstunAction = CreateWindowExA(0, "COMBOBOX", "", 
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
        180, yPos, 120, 200, hParent, (HMENU)IDC_TRIGGER_AFTER_HITSTUN_ACTION, GetModuleHandle(NULL), NULL);
    SendMessageA(hAfterHitstunAction, CB_ADDSTRING, 0, (LPARAM)"5A");
    SendMessageA(hAfterHitstunAction, CB_ADDSTRING, 0, (LPARAM)"5B");
    SendMessageA(hAfterHitstunAction, CB_ADDSTRING, 0, (LPARAM)"5C");
    SendMessageA(hAfterHitstunAction, CB_ADDSTRING, 0, (LPARAM)"2A");
    SendMessageA(hAfterHitstunAction, CB_ADDSTRING, 0, (LPARAM)"2B");
    SendMessageA(hAfterHitstunAction, CB_ADDSTRING, 0, (LPARAM)"2C");
    SendMessageA(hAfterHitstunAction, CB_ADDSTRING, 0, (LPARAM)"j.A");
    SendMessageA(hAfterHitstunAction, CB_ADDSTRING, 0, (LPARAM)"j.B");
    SendMessageA(hAfterHitstunAction, CB_ADDSTRING, 0, (LPARAM)"j.C");

    // Add special moves here
    SendMessageA(hAfterHitstunAction, CB_ADDSTRING, 0, (LPARAM)"QCF");
    SendMessageA(hAfterHitstunAction, CB_ADDSTRING, 0, (LPARAM)"DP");
    SendMessageA(hAfterHitstunAction, CB_ADDSTRING, 0, (LPARAM)"QCB");
    SendMessageA(hAfterHitstunAction, CB_ADDSTRING, 0, (LPARAM)"Super 1");
    SendMessageA(hAfterHitstunAction, CB_ADDSTRING, 0, (LPARAM)"Super 2");
    SendMessageA(hAfterHitstunAction, CB_ADDSTRING, 0, (LPARAM)"641236 Super");
    
    // Then add the remaining options
    SendMessageA(hAfterHitstunAction, CB_ADDSTRING, 0, (LPARAM)"Jump");
    SendMessageA(hAfterHitstunAction, CB_ADDSTRING, 0, (LPARAM)"Backdash");
    SendMessageA(hAfterHitstunAction, CB_ADDSTRING, 0, (LPARAM)"Block");
    // REMOVED: Custom option is removed since we don't support custom moveIDs anymore
    SendMessage(hAfterHitstunAction, CB_SETCURSEL, 
        ActionTypeToComboIndex(triggerAfterHitstunAction.load()), 0);

    CreateWindowExA(0, "STATIC", "Delay:", WS_CHILD | WS_VISIBLE | SS_LEFT,
        310, yPos, 40, 25, hParent, NULL, GetModuleHandle(NULL), NULL);
    sprintf_s(delayText, "%d", triggerAfterHitstunDelay.load());
    CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", delayText, 
        WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NUMBER,
        355, yPos, 40, 25, hParent, (HMENU)IDC_TRIGGER_AFTER_HITSTUN_DELAY, GetModuleHandle(NULL), NULL);

    // Debug: Wake buffering toggle (before On Wakeup trigger section)
    HWND hWakeBufferCheck = CreateWindowExA(0, "BUTTON", "Pre-buffer wake specials/dashes", 
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
        50, yPos, 220, 25,
        hParent, (HMENU)IDC_TRIGGER_WAKE_BUFFER_CHECK, GetModuleHandle(NULL), NULL);
    SendMessage(hWakeBufferCheck, BM_SETCHECK, g_wakeBufferingEnabled.load() ? BST_CHECKED : BST_UNCHECKED, 0);
    yPos += 30;

    // On Wakeup trigger
    yPos += 35;
    HWND hOnWakeupCheck = CreateWindowExA(0, "BUTTON", "On Wakeup:", 
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
        50, yPos, 120, 25, hParent, (HMENU)IDC_TRIGGER_ON_WAKEUP_CHECK, GetModuleHandle(NULL), NULL);
    SendMessage(hOnWakeupCheck, BM_SETCHECK, triggerOnWakeupEnabled.load() ? BST_CHECKED : BST_UNCHECKED, 0);

    HWND hOnWakeupAction = CreateWindowExA(0, "COMBOBOX", "", 
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
        180, yPos, 120, 200, hParent, (HMENU)IDC_TRIGGER_ON_WAKEUP_ACTION, GetModuleHandle(NULL), NULL);
    SendMessageA(hOnWakeupAction, CB_ADDSTRING, 0, (LPARAM)"5A");
    SendMessageA(hOnWakeupAction, CB_ADDSTRING, 0, (LPARAM)"5B");
    SendMessageA(hOnWakeupAction, CB_ADDSTRING, 0, (LPARAM)"5C");
    SendMessageA(hOnWakeupAction, CB_ADDSTRING, 0, (LPARAM)"2A");
    SendMessageA(hOnWakeupAction, CB_ADDSTRING, 0, (LPARAM)"2B");
    SendMessageA(hOnWakeupAction, CB_ADDSTRING, 0, (LPARAM)"2C");
    SendMessageA(hOnWakeupAction, CB_ADDSTRING, 0, (LPARAM)"j.A");
    SendMessageA(hOnWakeupAction, CB_ADDSTRING, 0, (LPARAM)"j.B");
    SendMessageA(hOnWakeupAction, CB_ADDSTRING, 0, (LPARAM)"j.C");

    // Add special moves here
    SendMessageA(hOnWakeupAction, CB_ADDSTRING, 0, (LPARAM)"QCF");
    SendMessageA(hOnWakeupAction, CB_ADDSTRING, 0, (LPARAM)"DP");
    SendMessageA(hOnWakeupAction, CB_ADDSTRING, 0, (LPARAM)"QCB");
    SendMessageA(hOnWakeupAction, CB_ADDSTRING, 0, (LPARAM)"Super 1");
    SendMessageA(hOnWakeupAction, CB_ADDSTRING, 0, (LPARAM)"Super 2");
    SendMessageA(hOnWakeupAction, CB_ADDSTRING, 0, (LPARAM)"641236 Super");
    
    // Then add the remaining options
    SendMessageA(hOnWakeupAction, CB_ADDSTRING, 0, (LPARAM)"Jump");
    SendMessageA(hOnWakeupAction, CB_ADDSTRING, 0, (LPARAM)"Backdash");
    SendMessageA(hOnWakeupAction, CB_ADDSTRING, 0, (LPARAM)"Block");
    // REMOVED: Custom option is removed since we don't support custom moveIDs anymore
    SendMessage(hOnWakeupAction, CB_SETCURSEL, 
        ActionTypeToComboIndex(triggerOnWakeupAction.load()), 0);

    CreateWindowExA(0, "STATIC", "Delay:", WS_CHILD | WS_VISIBLE | SS_LEFT,
        310, yPos, 40, 25, hParent, NULL, GetModuleHandle(NULL), NULL);
    sprintf_s(delayText, "%d", triggerOnWakeupDelay.load());
    CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", delayText, 
        WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NUMBER,
        355, yPos, 40, 25, hParent, (HMENU)IDC_TRIGGER_ON_WAKEUP_DELAY, GetModuleHandle(NULL), NULL);

    // After Airtech trigger
    yPos += 35;
    HWND hAfterAirtechCheck = CreateWindowExA(0, "BUTTON", "After Airtech:", 
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
        50, yPos, 120, 25, hParent, (HMENU)IDC_TRIGGER_AFTER_AIRTECH_CHECK, GetModuleHandle(NULL), NULL);
    SendMessage(hAfterAirtechCheck, BM_SETCHECK, triggerAfterAirtechEnabled.load() ? BST_CHECKED : BST_UNCHECKED, 0);

    HWND hAfterAirtechAction = CreateWindowExA(0, "COMBOBOX", "", 
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
        180, yPos, 120, 200, hParent, (HMENU)IDC_TRIGGER_AFTER_AIRTECH_ACTION, GetModuleHandle(NULL), NULL);

    // Add combo box items
    SendMessageA(hAfterAirtechAction, CB_ADDSTRING, 0, (LPARAM)"5A");
    SendMessageA(hAfterAirtechAction, CB_ADDSTRING, 0, (LPARAM)"5B");
    SendMessageA(hAfterAirtechAction, CB_ADDSTRING, 0, (LPARAM)"5C");
    SendMessageA(hAfterAirtechAction, CB_ADDSTRING, 0, (LPARAM)"2A");
    SendMessageA(hAfterAirtechAction, CB_ADDSTRING, 0, (LPARAM)"2B");
    SendMessageA(hAfterAirtechAction, CB_ADDSTRING, 0, (LPARAM)"2C");
    SendMessageA(hAfterAirtechAction, CB_ADDSTRING, 0, (LPARAM)"j.A");
    SendMessageA(hAfterAirtechAction, CB_ADDSTRING, 0, (LPARAM)"j.B");
    SendMessageA(hAfterAirtechAction, CB_ADDSTRING, 0, (LPARAM)"j.C");

    // Add special moves here
    SendMessageA(hAfterAirtechAction, CB_ADDSTRING, 0, (LPARAM)"QCF");
    SendMessageA(hAfterAirtechAction, CB_ADDSTRING, 0, (LPARAM)"DP");
    SendMessageA(hAfterAirtechAction, CB_ADDSTRING, 0, (LPARAM)"QCB");
    SendMessageA(hAfterAirtechAction, CB_ADDSTRING, 0, (LPARAM)"Super 1");
    SendMessageA(hAfterAirtechAction, CB_ADDSTRING, 0, (LPARAM)"Super 2");
    SendMessageA(hAfterAirtechAction, CB_ADDSTRING, 0, (LPARAM)"641236 Super");
    
    // Then add the remaining options
    SendMessageA(hAfterAirtechAction, CB_ADDSTRING, 0, (LPARAM)"Jump");
    SendMessageA(hAfterAirtechAction, CB_ADDSTRING, 0, (LPARAM)"Backdash");
    SendMessageA(hAfterAirtechAction, CB_ADDSTRING, 0, (LPARAM)"Block");
    // REMOVED: Custom option is removed since we don't support custom moveIDs anymore
    SendMessage(hAfterAirtechAction, CB_SETCURSEL, 
        ActionTypeToComboIndex(triggerAfterAirtechAction.load()), 0);

    // Add delay field
    CreateWindowExA(0, "STATIC", "Delay:", WS_CHILD | WS_VISIBLE | SS_LEFT,
        310, yPos, 40, 25, hParent, NULL, GetModuleHandle(NULL), NULL);
    sprintf_s(delayText, "%d", triggerAfterAirtechDelay.load());
    CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", delayText, 
        WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NUMBER,
        355, yPos, 40, 25, hParent, (HMENU)IDC_TRIGGER_AFTER_AIRTECH_DELAY, GetModuleHandle(NULL), NULL);

    // CRITICAL FIX: Use ActionTypeToComboIndex function instead of direct mapping
    // to properly handle air moves (j.A, j.B, j.C)

    // For After Block
    int afterBlockActionType = triggerAfterBlockAction.load();
    int afterBlockComboIndex = ActionTypeToComboIndex(afterBlockActionType);
    SendMessage(hAfterBlockAction, CB_SETCURSEL, afterBlockComboIndex, 0);

    // For After Hitstun
    int afterHitstunActionType = triggerAfterHitstunAction.load();
    int afterHitstunComboIndex = ActionTypeToComboIndex(afterHitstunActionType);
    SendMessage(hAfterHitstunAction, CB_SETCURSEL, afterHitstunComboIndex, 0);

    // For On Wakeup
    int onWakeupActionType = triggerOnWakeupAction.load();
    int onWakeupComboIndex = ActionTypeToComboIndex(onWakeupActionType);
    SendMessage(hOnWakeupAction, CB_SETCURSEL, onWakeupComboIndex, 0);

    // For After Airtech
    int afterAirtechActionType = triggerAfterAirtechAction.load();
    int afterAirtechComboIndex = ActionTypeToComboIndex(afterAirtechActionType);
    SendMessage(hAfterAirtechAction, CB_SETCURSEL, afterAirtechComboIndex, 0);
}