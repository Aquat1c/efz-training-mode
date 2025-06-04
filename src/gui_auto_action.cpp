#include "../include/gui.h"
#include "../include/constants.h"
#include "../include/utilities.h"
#include "../include/logger.h"
#include "../include/memory.h"
#include <windows.h>
#include <commctrl.h>

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

    SendMessage(hAfterBlockAction, CB_ADDSTRING, 0, (LPARAM)"5A");
    SendMessage(hAfterBlockAction, CB_ADDSTRING, 0, (LPARAM)"5B");
    SendMessage(hAfterBlockAction, CB_ADDSTRING, 0, (LPARAM)"5C");
    SendMessage(hAfterBlockAction, CB_ADDSTRING, 0, (LPARAM)"2A");
    SendMessage(hAfterBlockAction, CB_ADDSTRING, 0, (LPARAM)"2B");
    SendMessage(hAfterBlockAction, CB_ADDSTRING, 0, (LPARAM)"2C");
    SendMessage(hAfterBlockAction, CB_ADDSTRING, 0, (LPARAM)"j.A");
    SendMessage(hAfterBlockAction, CB_ADDSTRING, 0, (LPARAM)"j.B");
    SendMessage(hAfterBlockAction, CB_ADDSTRING, 0, (LPARAM)"j.C");
    SendMessage(hAfterBlockAction, CB_ADDSTRING, 0, (LPARAM)"Jump");
    SendMessage(hAfterBlockAction, CB_ADDSTRING, 0, (LPARAM)"Backdash");
    SendMessage(hAfterBlockAction, CB_ADDSTRING, 0, (LPARAM)"Block");
    SendMessage(hAfterBlockAction, CB_ADDSTRING, 0, (LPARAM)"Custom");
    SendMessage(hAfterBlockAction, CB_SETCURSEL, triggerAfterBlockAction.load() - 1, 0);

    CreateWindowEx(0, "STATIC", "Delay:", WS_CHILD | WS_VISIBLE | SS_LEFT,
        310, yPos, 40, 25, hParent, NULL, GetModuleHandle(NULL), NULL);

    sprintf_s(delayText, "%d", triggerAfterBlockDelay.load());
    CreateWindowEx(WS_EX_CLIENTEDGE, "EDIT", delayText, 
        WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NUMBER,
        355, yPos, 40, 25, hParent, (HMENU)IDC_TRIGGER_AFTER_BLOCK_DELAY, GetModuleHandle(NULL), NULL);

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
    SendMessage(hAfterHitstunAction, CB_ADDSTRING, 0, (LPARAM)"Jump");
    SendMessage(hAfterHitstunAction, CB_ADDSTRING, 0, (LPARAM)"Backdash");
    SendMessage(hAfterHitstunAction, CB_ADDSTRING, 0, (LPARAM)"Block");
    SendMessage(hAfterHitstunAction, CB_ADDSTRING, 0, (LPARAM)"Custom");
    SendMessage(hAfterHitstunAction, CB_SETCURSEL, triggerAfterHitstunAction.load() - 1, 0);

    CreateWindowEx(0, "STATIC", "Delay:", WS_CHILD | WS_VISIBLE | SS_LEFT,
        310, yPos, 40, 25, hParent, NULL, GetModuleHandle(NULL), NULL);
    sprintf_s(delayText, "%d", triggerAfterHitstunDelay.load());
    CreateWindowEx(WS_EX_CLIENTEDGE, "EDIT", delayText, 
        WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NUMBER,
        355, yPos, 40, 25, hParent, (HMENU)IDC_TRIGGER_AFTER_HITSTUN_DELAY, GetModuleHandle(NULL), NULL);

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
    SendMessage(hOnWakeupAction, CB_ADDSTRING, 0, (LPARAM)"Jump");
    SendMessage(hOnWakeupAction, CB_ADDSTRING, 0, (LPARAM)"Backdash");
    SendMessage(hOnWakeupAction, CB_ADDSTRING, 0, (LPARAM)"Block");
    SendMessage(hOnWakeupAction, CB_ADDSTRING, 0, (LPARAM)"Custom");
    SendMessage(hOnWakeupAction, CB_SETCURSEL, triggerOnWakeupAction.load() - 1, 0);

    CreateWindowEx(0, "STATIC", "Delay:", WS_CHILD | WS_VISIBLE | SS_LEFT,
        310, yPos, 40, 25, hParent, NULL, GetModuleHandle(NULL), NULL);
    sprintf_s(delayText, "%d", triggerOnWakeupDelay.load());
    CreateWindowEx(WS_EX_CLIENTEDGE, "EDIT", delayText, 
        WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NUMBER,
        355, yPos, 40, 25, hParent, (HMENU)IDC_TRIGGER_ON_WAKEUP_DELAY, GetModuleHandle(NULL), NULL);

    // After Airtech trigger
    yPos += 35;
    HWND hAfterAirtechCheck = CreateWindowEx(0, "BUTTON", "After Airtech:", 
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
        50, yPos, 120, 25, hParent, (HMENU)IDC_TRIGGER_AFTER_AIRTECH_CHECK, GetModuleHandle(NULL), NULL);
    SendMessage(hAfterAirtechCheck, BM_SETCHECK, triggerAfterAirtechEnabled.load() ? BST_CHECKED : BST_UNCHECKED, 0);

    HWND hAfterAirtechAction = CreateWindowEx(0, "COMBOBOX", "", 
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
        180, yPos, 120, 200, hParent, (HMENU)IDC_TRIGGER_AFTER_AIRTECH_ACTION, GetModuleHandle(NULL), NULL);
    SendMessage(hAfterAirtechAction, CB_ADDSTRING, 0, (LPARAM)"5A");
    SendMessage(hAfterAirtechAction, CB_ADDSTRING, 0, (LPARAM)"5B");
    SendMessage(hAfterAirtechAction, CB_ADDSTRING, 0, (LPARAM)"5C");
    SendMessage(hAfterAirtechAction, CB_ADDSTRING, 0, (LPARAM)"2A");
    SendMessage(hAfterAirtechAction, CB_ADDSTRING, 0, (LPARAM)"2B");
    SendMessage(hAfterAirtechAction, CB_ADDSTRING, 0, (LPARAM)"2C");
    SendMessage(hAfterAirtechAction, CB_ADDSTRING, 0, (LPARAM)"j.A");
    SendMessage(hAfterAirtechAction, CB_ADDSTRING, 0, (LPARAM)"j.B");
    SendMessage(hAfterAirtechAction, CB_ADDSTRING, 0, (LPARAM)"j.C");
    SendMessage(hAfterAirtechAction, CB_ADDSTRING, 0, (LPARAM)"Jump");
    SendMessage(hAfterAirtechAction, CB_ADDSTRING, 0, (LPARAM)"Backdash");
    SendMessage(hAfterAirtechAction, CB_ADDSTRING, 0, (LPARAM)"Block");
    SendMessage(hAfterAirtechAction, CB_ADDSTRING, 0, (LPARAM)"Custom");
    SendMessage(hAfterAirtechAction, CB_SETCURSEL, triggerAfterAirtechAction.load() - 1, 0);

    CreateWindowEx(0, "STATIC", "Delay:", WS_CHILD | WS_VISIBLE | SS_LEFT,
        310, yPos, 40, 25, hParent, NULL, GetModuleHandle(NULL), NULL);
    sprintf_s(delayText, "%d", triggerAfterAirtechDelay.load());
    CreateWindowEx(WS_EX_CLIENTEDGE, "EDIT", delayText, 
        WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NUMBER,
        355, yPos, 40, 25, hParent, (HMENU)IDC_TRIGGER_AFTER_AIRTECH_DELAY, GetModuleHandle(NULL), NULL);
}