#include "../include/gui.h"
#include "../include/constants.h"
#include "../include/utilities.h"
#include "../include/logger.h"
#include "../include/memory.h"
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
    ACTION_JA,          // 6 = j.A (value 11)
    ACTION_JB,          // 7 = j.B (value 12)
    ACTION_JC,          // 8 = j.C (value 13)
    ACTION_QCF,         // 9 = QCF (Quarter Circle Forward)
    ACTION_DP,          // 10 = DP (Dragon Punch)
    ACTION_QCB,         // 11 = QCB (Quarter Circle Back)
    ACTION_SUPER1,      // 12 = Super 1 (Half Circle Forward)
    ACTION_SUPER2,      // 13 = Super 2 (Half Circle Back)
    ACTION_JUMP,        // 14 = Jump (value 7)
    ACTION_BACKDASH,    // 15 = Backdash (value 8)
    ACTION_BLOCK,       // 16 = Block (value 9)
    ACTION_CUSTOM       // 17 = Custom (value 10)
};

// Helper function to convert action type to combobox index
int ActionTypeToComboIndex(int actionType) {
    // Loop through the mapping array to find the matching index
    for (int i = 0; i < sizeof(ComboIndexToActionType)/sizeof(int); i++) {
        if (ComboIndexToActionType[i] == actionType) {
            return i;
        }
    }
    return 0; // Default to first item (5A) if not found
}

void AutoActionPage_CreateContent(HWND hParent, DisplayData* pData) {
    // Master enable checkbox
    HWND hAutoActionCheck = CreateWindowEx(0, "BUTTON", "Enable Auto Action System", 
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
        50, 20, 250, 25, 
        hParent, (HMENU)IDC_AUTOACTION_ENABLE, GetModuleHandle(NULL), NULL);
    SendMessage(hAutoActionCheck, BM_SETCHECK, autoActionEnabled.load() ? BST_CHECKED : BST_UNCHECKED, 0);

    // Player target (applies to all triggers)
    CreateWindowEx(0, "STATIC", "Apply To:", 
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        50, 55, 80, 25, hParent, NULL, GetModuleHandle(NULL), NULL);

    HWND hPlayerCombo = CreateWindowEx(0, "COMBOBOX", "", 
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
        140, 55, 200, 120, hParent, (HMENU)IDC_AUTOACTION_PLAYER, GetModuleHandle(NULL), NULL);

    SendMessage(hPlayerCombo, CB_ADDSTRING, 0, (LPARAM)"P1 Only");
    SendMessage(hPlayerCombo, CB_ADDSTRING, 0, (LPARAM)"P2 Only");
    SendMessage(hPlayerCombo, CB_ADDSTRING, 0, (LPARAM)"Both Players");
    SendMessage(hPlayerCombo, CB_SETCURSEL, autoActionPlayer.load() - 1, 0);

    int yPos = 90;
    char delayText[8];

    // After Block trigger
    HWND hAfterBlockCheck = CreateWindowEx(0, "BUTTON", "After Block:", 
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
        50, yPos, 120, 25, hParent, (HMENU)IDC_TRIGGER_AFTER_BLOCK_CHECK, GetModuleHandle(NULL), NULL);
    SendMessage(hAfterBlockCheck, BM_SETCHECK, triggerAfterBlockEnabled.load() ? BST_CHECKED : BST_UNCHECKED, 0);

    HWND hAfterBlockAction = CreateWindowEx(0, "COMBOBOX", "", 
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
        180, yPos, 120, 200, hParent, (HMENU)IDC_TRIGGER_AFTER_BLOCK_ACTION, GetModuleHandle(NULL), NULL);

    // When adding options to the action combos, add the special moves:
    SendMessage(hAfterBlockAction, CB_ADDSTRING, 0, (LPARAM)"5A");
    SendMessage(hAfterBlockAction, CB_ADDSTRING, 0, (LPARAM)"5B");
    SendMessage(hAfterBlockAction, CB_ADDSTRING, 0, (LPARAM)"5C");
    SendMessage(hAfterBlockAction, CB_ADDSTRING, 0, (LPARAM)"2A");
    SendMessage(hAfterBlockAction, CB_ADDSTRING, 0, (LPARAM)"2B");
    SendMessage(hAfterBlockAction, CB_ADDSTRING, 0, (LPARAM)"2C");
    SendMessage(hAfterBlockAction, CB_ADDSTRING, 0, (LPARAM)"j.A");
    SendMessage(hAfterBlockAction, CB_ADDSTRING, 0, (LPARAM)"j.B");
    SendMessage(hAfterBlockAction, CB_ADDSTRING, 0, (LPARAM)"j.C");

    // Add special moves here
    SendMessage(hAfterBlockAction, CB_ADDSTRING, 0, (LPARAM)"QCF");
    SendMessage(hAfterBlockAction, CB_ADDSTRING, 0, (LPARAM)"DP");
    SendMessage(hAfterBlockAction, CB_ADDSTRING, 0, (LPARAM)"QCB");
    SendMessage(hAfterBlockAction, CB_ADDSTRING, 0, (LPARAM)"Super 1");
    SendMessage(hAfterBlockAction, CB_ADDSTRING, 0, (LPARAM)"Super 2");
    
    // Then add the remaining options
    SendMessage(hAfterBlockAction, CB_ADDSTRING, 0, (LPARAM)"Jump");
    SendMessage(hAfterBlockAction, CB_ADDSTRING, 0, (LPARAM)"Backdash");
    SendMessage(hAfterBlockAction, CB_ADDSTRING, 0, (LPARAM)"Block");
    SendMessage(hAfterBlockAction, CB_ADDSTRING, 0, (LPARAM)"Custom");
    SendMessage(hAfterBlockAction, CB_SETCURSEL, 
        ActionTypeToComboIndex(triggerAfterBlockAction.load()), 0);

    CreateWindowEx(0, "STATIC", "Delay:", WS_CHILD | WS_VISIBLE | SS_LEFT,
        310, yPos, 40, 25, hParent, NULL, GetModuleHandle(NULL), NULL);

    sprintf_s(delayText, "%d", triggerAfterBlockDelay.load());
    CreateWindowEx(WS_EX_CLIENTEDGE, "EDIT", delayText, 
        WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NUMBER,
        355, yPos, 40, 25, hParent, (HMENU)IDC_TRIGGER_AFTER_BLOCK_DELAY, GetModuleHandle(NULL), NULL);

    // Add custom move ID field for After Block trigger
    sprintf_s(delayText, "%d", triggerAfterBlockCustomID.load());
    CreateWindowEx(0, "STATIC", "Custom ID:", WS_CHILD | WS_VISIBLE | SS_LEFT,
        415, yPos, 60, 25, hParent, NULL, GetModuleHandle(NULL), NULL);
    CreateWindowEx(WS_EX_CLIENTEDGE, "EDIT", delayText, 
        WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NUMBER,
        480, yPos, 40, 25, hParent, (HMENU)IDC_TRIGGER_AFTER_BLOCK_CUSTOM, GetModuleHandle(NULL), NULL);

    // After Hitstun trigger
    yPos += 35;
    HWND hAfterHitstunCheck = CreateWindowEx(0, "BUTTON", "After Hitstun:", 
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
        50, yPos, 120, 25, hParent, (HMENU)IDC_TRIGGER_AFTER_HITSTUN_CHECK, GetModuleHandle(NULL), NULL);
    SendMessage(hAfterHitstunCheck, BM_SETCHECK, triggerAfterHitstunEnabled.load() ? BST_CHECKED : BST_UNCHECKED, 0);

    HWND hAfterHitstunAction = CreateWindowEx(0, "COMBOBOX", "", 
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
        180, yPos, 120, 200, hParent, (HMENU)IDC_TRIGGER_AFTER_HITSTUN_ACTION, GetModuleHandle(NULL), NULL);
    SendMessage(hAfterHitstunAction, CB_ADDSTRING, 0, (LPARAM)"5A");
    SendMessage(hAfterHitstunAction, CB_ADDSTRING, 0, (LPARAM)"5B");
    SendMessage(hAfterHitstunAction, CB_ADDSTRING, 0, (LPARAM)"5C");
    SendMessage(hAfterHitstunAction, CB_ADDSTRING, 0, (LPARAM)"2A");
    SendMessage(hAfterHitstunAction, CB_ADDSTRING, 0, (LPARAM)"2B");
    SendMessage(hAfterHitstunAction, CB_ADDSTRING, 0, (LPARAM)"2C");
    SendMessage(hAfterHitstunAction, CB_ADDSTRING, 0, (LPARAM)"j.A");
    SendMessage(hAfterHitstunAction, CB_ADDSTRING, 0, (LPARAM)"j.B");
    SendMessage(hAfterHitstunAction, CB_ADDSTRING, 0, (LPARAM)"j.C");

    // Add special moves here
    SendMessage(hAfterHitstunAction, CB_ADDSTRING, 0, (LPARAM)"QCF");
    SendMessage(hAfterHitstunAction, CB_ADDSTRING, 0, (LPARAM)"DP");
    SendMessage(hAfterHitstunAction, CB_ADDSTRING, 0, (LPARAM)"QCB");
    SendMessage(hAfterHitstunAction, CB_ADDSTRING, 0, (LPARAM)"Super 1");
    SendMessage(hAfterHitstunAction, CB_ADDSTRING, 0, (LPARAM)"Super 2");
    
    // Then add the remaining options
    SendMessage(hAfterHitstunAction, CB_ADDSTRING, 0, (LPARAM)"Jump");
    SendMessage(hAfterHitstunAction, CB_ADDSTRING, 0, (LPARAM)"Backdash");
    SendMessage(hAfterHitstunAction, CB_ADDSTRING, 0, (LPARAM)"Block");
    SendMessage(hAfterHitstunAction, CB_ADDSTRING, 0, (LPARAM)"Custom");
    SendMessage(hAfterHitstunAction, CB_SETCURSEL, 
        ActionTypeToComboIndex(triggerAfterHitstunAction.load()), 0);

    CreateWindowEx(0, "STATIC", "Delay:", WS_CHILD | WS_VISIBLE | SS_LEFT,
        310, yPos, 40, 25, hParent, NULL, GetModuleHandle(NULL), NULL);
    sprintf_s(delayText, "%d", triggerAfterHitstunDelay.load());
    CreateWindowEx(WS_EX_CLIENTEDGE, "EDIT", delayText, 
        WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NUMBER,
        355, yPos, 40, 25, hParent, (HMENU)IDC_TRIGGER_AFTER_HITSTUN_DELAY, GetModuleHandle(NULL), NULL);

    // Add custom move ID field for After Hitstun trigger
    sprintf_s(delayText, "%d", triggerAfterHitstunCustomID.load());
    CreateWindowEx(0, "STATIC", "Custom ID:", WS_CHILD | WS_VISIBLE | SS_LEFT,
        415, yPos, 60, 25, hParent, NULL, GetModuleHandle(NULL), NULL);
    CreateWindowEx(WS_EX_CLIENTEDGE, "EDIT", delayText, 
        WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NUMBER,
        480, yPos, 40, 25, hParent, (HMENU)IDC_TRIGGER_AFTER_HITSTUN_CUSTOM, GetModuleHandle(NULL), NULL);

    // On Wakeup trigger
    yPos += 35;
    HWND hOnWakeupCheck = CreateWindowEx(0, "BUTTON", "On Wakeup:", 
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
        50, yPos, 120, 25, hParent, (HMENU)IDC_TRIGGER_ON_WAKEUP_CHECK, GetModuleHandle(NULL), NULL);
    SendMessage(hOnWakeupCheck, BM_SETCHECK, triggerOnWakeupEnabled.load() ? BST_CHECKED : BST_UNCHECKED, 0);

    HWND hOnWakeupAction = CreateWindowEx(0, "COMBOBOX", "", 
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
        180, yPos, 120, 200, hParent, (HMENU)IDC_TRIGGER_ON_WAKEUP_ACTION, GetModuleHandle(NULL), NULL);
    SendMessage(hOnWakeupAction, CB_ADDSTRING, 0, (LPARAM)"5A");
    SendMessage(hOnWakeupAction, CB_ADDSTRING, 0, (LPARAM)"5B");
    SendMessage(hOnWakeupAction, CB_ADDSTRING, 0, (LPARAM)"5C");
    SendMessage(hOnWakeupAction, CB_ADDSTRING, 0, (LPARAM)"2A");
    SendMessage(hOnWakeupAction, CB_ADDSTRING, 0, (LPARAM)"2B");
    SendMessage(hOnWakeupAction, CB_ADDSTRING, 0, (LPARAM)"2C");
    SendMessage(hOnWakeupAction, CB_ADDSTRING, 0, (LPARAM)"j.A");
    SendMessage(hOnWakeupAction, CB_ADDSTRING, 0, (LPARAM)"j.B");
    SendMessage(hOnWakeupAction, CB_ADDSTRING, 0, (LPARAM)"j.C");

    // Add special moves here
    SendMessage(hOnWakeupAction, CB_ADDSTRING, 0, (LPARAM)"QCF");
    SendMessage(hOnWakeupAction, CB_ADDSTRING, 0, (LPARAM)"DP");
    SendMessage(hOnWakeupAction, CB_ADDSTRING, 0, (LPARAM)"QCB");
    SendMessage(hOnWakeupAction, CB_ADDSTRING, 0, (LPARAM)"Super 1");
    SendMessage(hOnWakeupAction, CB_ADDSTRING, 0, (LPARAM)"Super 2");
    
    // Then add the remaining options
    SendMessage(hOnWakeupAction, CB_ADDSTRING, 0, (LPARAM)"Jump");
    SendMessage(hOnWakeupAction, CB_ADDSTRING, 0, (LPARAM)"Backdash");
    SendMessage(hOnWakeupAction, CB_ADDSTRING, 0, (LPARAM)"Block");
    SendMessage(hOnWakeupAction, CB_ADDSTRING, 0, (LPARAM)"Custom");
    SendMessage(hOnWakeupAction, CB_SETCURSEL, 
        ActionTypeToComboIndex(triggerOnWakeupAction.load()), 0);

    CreateWindowEx(0, "STATIC", "Delay:", WS_CHILD | WS_VISIBLE | SS_LEFT,
        310, yPos, 40, 25, hParent, NULL, GetModuleHandle(NULL), NULL);
    sprintf_s(delayText, "%d", triggerOnWakeupDelay.load());
    CreateWindowEx(WS_EX_CLIENTEDGE, "EDIT", delayText, 
        WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NUMBER,
        355, yPos, 40, 25, hParent, (HMENU)IDC_TRIGGER_ON_WAKEUP_DELAY, GetModuleHandle(NULL), NULL);

    // Add custom move ID field for On Wakeup trigger
    sprintf_s(delayText, "%d", triggerOnWakeupCustomID.load());
    CreateWindowEx(0, "STATIC", "Custom ID:", WS_CHILD | WS_VISIBLE | SS_LEFT,
        415, yPos, 60, 25, hParent, NULL, GetModuleHandle(NULL), NULL);
    CreateWindowEx(WS_EX_CLIENTEDGE, "EDIT", delayText, 
        WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NUMBER,
        480, yPos, 40, 25, hParent, (HMENU)IDC_TRIGGER_ON_WAKEUP_CUSTOM, GetModuleHandle(NULL), NULL);

    // After Airtech trigger
    yPos += 35;
    HWND hAfterAirtechCheck = CreateWindowEx(0, "BUTTON", "After Airtech:", 
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
        50, yPos, 120, 25, hParent, (HMENU)IDC_TRIGGER_AFTER_AIRTECH_CHECK, GetModuleHandle(NULL), NULL);
    SendMessage(hAfterAirtechCheck, BM_SETCHECK, triggerAfterAirtechEnabled.load() ? BST_CHECKED : BST_UNCHECKED, 0);

    HWND hAfterAirtechAction = CreateWindowEx(0, "COMBOBOX", "", 
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
        180, yPos, 120, 200, hParent, (HMENU)IDC_TRIGGER_AFTER_AIRTECH_ACTION, GetModuleHandle(NULL), NULL);

    // Add combo box items
    SendMessage(hAfterAirtechAction, CB_ADDSTRING, 0, (LPARAM)"5A");
    SendMessage(hAfterAirtechAction, CB_ADDSTRING, 0, (LPARAM)"5B");
    SendMessage(hAfterAirtechAction, CB_ADDSTRING, 0, (LPARAM)"5C");
    SendMessage(hAfterAirtechAction, CB_ADDSTRING, 0, (LPARAM)"2A");
    SendMessage(hAfterAirtechAction, CB_ADDSTRING, 0, (LPARAM)"2B");
    SendMessage(hAfterAirtechAction, CB_ADDSTRING, 0, (LPARAM)"2C");
    SendMessage(hAfterAirtechAction, CB_ADDSTRING, 0, (LPARAM)"j.A");
    SendMessage(hAfterAirtechAction, CB_ADDSTRING, 0, (LPARAM)"j.B");
    SendMessage(hAfterAirtechAction, CB_ADDSTRING, 0, (LPARAM)"j.C");

    // Add special moves here
    SendMessage(hAfterAirtechAction, CB_ADDSTRING, 0, (LPARAM)"QCF");
    SendMessage(hAfterAirtechAction, CB_ADDSTRING, 0, (LPARAM)"DP");
    SendMessage(hAfterAirtechAction, CB_ADDSTRING, 0, (LPARAM)"QCB");
    SendMessage(hAfterAirtechAction, CB_ADDSTRING, 0, (LPARAM)"Super 1");
    SendMessage(hAfterAirtechAction, CB_ADDSTRING, 0, (LPARAM)"Super 2");
    
    // Then add the remaining options
    SendMessage(hAfterAirtechAction, CB_ADDSTRING, 0, (LPARAM)"Jump");
    SendMessage(hAfterAirtechAction, CB_ADDSTRING, 0, (LPARAM)"Backdash");
    SendMessage(hAfterAirtechAction, CB_ADDSTRING, 0, (LPARAM)"Block");
    SendMessage(hAfterAirtechAction, CB_ADDSTRING, 0, (LPARAM)"Custom");
    SendMessage(hAfterAirtechAction, CB_SETCURSEL, 
        ActionTypeToComboIndex(triggerAfterAirtechAction.load()), 0);

    // Add delay field
    CreateWindowEx(0, "STATIC", "Delay:", WS_CHILD | WS_VISIBLE | SS_LEFT,
        310, yPos, 40, 25, hParent, NULL, GetModuleHandle(NULL), NULL);
    sprintf_s(delayText, "%d", triggerAfterAirtechDelay.load());
    CreateWindowEx(WS_EX_CLIENTEDGE, "EDIT", delayText, 
        WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NUMBER,
        355, yPos, 40, 25, hParent, (HMENU)IDC_TRIGGER_AFTER_AIRTECH_DELAY, GetModuleHandle(NULL), NULL);

    // Add custom move ID field for After Airtech trigger
    sprintf_s(delayText, "%d", triggerAfterAirtechCustomID.load());
    CreateWindowEx(0, "STATIC", "Custom ID:", WS_CHILD | WS_VISIBLE | SS_LEFT,
        415, yPos, 60, 25, hParent, NULL, GetModuleHandle(NULL), NULL);
    CreateWindowEx(WS_EX_CLIENTEDGE, "EDIT", delayText, 
        WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NUMBER,
        480, yPos, 40, 25, hParent, (HMENU)IDC_TRIGGER_AFTER_AIRTECH_CUSTOM, GetModuleHandle(NULL), NULL);

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