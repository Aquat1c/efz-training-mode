#include "../include/gui.h"
#include "../include/constants.h"
#include "../include/memory.h"
#include "../include/utilities.h"
#include "../include/logger.h"
#include <windows.h>
#include <string>
#include <thread>
#include <commctrl.h>
#include <windowsx.h>
#include <sstream>

// Add near the top of the file, after includes but before the dialog proc
extern "C" const int ComboIndexToActionType[];  // Import the array from gui_auto_action.cpp

// Forward declaration of the PageSubclassProc function
LRESULT CALLBACK PageSubclassProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR dwRefData);

void ShowEditDataDialog(HWND hParent) {
    // Create a dialog template with sizes that match our controls
    static WORD dlgTemplate[128];
    ZeroMemory(dlgTemplate, sizeof(dlgTemplate));
    DLGTEMPLATE* dlg = (DLGTEMPLATE*)dlgTemplate;
    dlg->style = DS_SETFONT | DS_MODALFRAME | WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_FIXEDSYS;
    dlg->dwExtendedStyle = 0;
    dlg->cdit = 0;
    dlg->x = 10; dlg->y = 10;
    dlg->cx = 320; //Width of the dialog
    dlg->cy = 280; // Height of the dialog
    
    // Create and show the dialog
    DialogBoxIndirectParamA(GetModuleHandle(NULL), (LPCDLGTEMPLATEA)dlgTemplate, hParent, EditDataDlgProc, (LPARAM)&displayData);
}

INT_PTR CALLBACK EditDataDlgProc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam) {
    static DisplayData* pData = nullptr;
    static HWND hTabControl = NULL;
    static HWND hPage1 = NULL;
    static HWND hPage2 = NULL;
    static HWND hPage3 = NULL;
    
    // Round start positions
    const double p1StartX_const = 240.0, p2StartX_const = 400.0, startY_const = 0.0;

    // Log entry for every message to EditDataDlgProc for deep debugging if needed, can be noisy
    // LogOut("[GUI_DEBUG] EditDataDlgProc: hDlg=" + Logger::hwndToString(hDlg) + ", msg=" + std::to_string(message), true);

    switch (message) {
    case WM_INITDIALOG: {
        LogOut("[GUI_DEBUG] EditDataDlgProc: WM_INITDIALOG received for hDlg=" + Logger::hwndToString(hDlg), true);
        pData = (DisplayData*)lParam;
        
        // Initialize common controls
        INITCOMMONCONTROLSEX icc;
        icc.dwSize = sizeof(INITCOMMONCONTROLSEX);
        icc.dwICC = ICC_TAB_CLASSES;
        InitCommonControlsEx(&icc);
        
        // Create a tab control that fits the larger dialog
        hTabControl = CreateWindowEx(0, WC_TABCONTROL, NULL, 
            WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | TCS_FOCUSONBUTTONDOWN,
            10, 10, 580, 390, hDlg, (HMENU)IDC_TAB_CONTROL, GetModuleHandle(NULL), NULL);
        // Add tab items
        TCITEM tie;
        tie.mask = TCIF_TEXT;
        tie.pszText = (LPSTR)"Game Values";
        TabCtrl_InsertItem(hTabControl, 0, &tie);
        tie.pszText = (LPSTR)"Movement Options";
        TabCtrl_InsertItem(hTabControl, 1, &tie);
        tie.pszText = (LPSTR)"Auto Action";
        TabCtrl_InsertItem(hTabControl, 2, &tie);

        // Create page containers
        hPage1 = CreateWindowEx(
            0, "STATIC", "", 
            WS_CHILD | WS_VISIBLE, 
            0, 0, 0, 0, hDlg, NULL, GetModuleHandle(NULL), NULL);
        LogOut("[GUI_DEBUG] EditDataDlgProc: Created hPage1=" + Logger::hwndToString(hPage1), true);
            
        hPage2 = CreateWindowEx(
            0, "STATIC", "", 
            WS_CHILD | WS_VISIBLE, 
            0, 0, 0, 0, hDlg, NULL, GetModuleHandle(NULL), NULL);
        LogOut("[GUI_DEBUG] EditDataDlgProc: Created hPage2=" + Logger::hwndToString(hPage2), true);
        
        hPage3 = CreateWindowEx(
            0, "STATIC", "", 
            WS_CHILD | WS_VISIBLE, 
            0, 0, 0, 0, hDlg, NULL, GetModuleHandle(NULL), NULL);
        LogOut("[GUI_DEBUG] EditDataDlgProc: Created hPage3=" + Logger::hwndToString(hPage3), true);
        
        // Subclass ALL page containers to forward WM_COMMAND messages
        BOOL sub1 = SetWindowSubclass(hPage1, PageSubclassProc, 1, (DWORD_PTR)hDlg);
        LogOut("[GUI_DEBUG] EditDataDlgProc: SetWindowSubclass for hPage1 result: " + std::to_string(sub1) + ", hPage1=" + Logger::hwndToString(hPage1) + ", hDlg=" + Logger::hwndToString(hDlg), true);
        BOOL sub2 = SetWindowSubclass(hPage2, PageSubclassProc, 2, (DWORD_PTR)hDlg);
        LogOut("[GUI_DEBUG] EditDataDlgProc: SetWindowSubclass for hPage2 result: " + std::to_string(sub2), true);
        BOOL sub3 = SetWindowSubclass(hPage3, PageSubclassProc, 3, (DWORD_PTR)hDlg);        
        LogOut("[GUI_DEBUG] EditDataDlgProc: SetWindowSubclass for hPage3 result: " + std::to_string(sub3), true);
        
        // Position pages in tab control
        RECT rc;
        GetClientRect(hTabControl, &rc);
        TabCtrl_AdjustRect(hTabControl, FALSE, &rc);

        // Position pages with the adjusted rectangle
        SetWindowPos(hPage1, NULL, 
            rc.left + 10, rc.top + 10,
            rc.right - rc.left - 20, rc.bottom - rc.top - 20,
            SWP_NOZORDER);
        SetWindowPos(hPage2, NULL, 
            rc.left + 10, rc.top + 10,
            rc.right - rc.left - 20, rc.bottom - rc.top - 20,
            SWP_NOZORDER);
        SetWindowPos(hPage3, NULL, 
            rc.left + 10, rc.top + 10,
            rc.right - rc.left - 20, rc.bottom - rc.top - 20,
            SWP_NOZORDER);
    
        // Create page content
        GameValuesPage_CreateContent(hPage1, pData);
        MovementOptionsPage_CreateContent(hPage2, pData);
        AutoActionPage_CreateContent(hPage3, pData);
        
        // Show first page, hide others
        ShowWindow(hPage1, SW_SHOW);
        ShowWindow(hPage2, SW_HIDE);
        ShowWindow(hPage3, SW_HIDE);
        
        // Update button positions for the larger dialog
        HWND hConfirmBtn = CreateWindowEx(0, "BUTTON", "Confirm", 
            WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
            200, 410, 100, 30, hDlg, (HMENU)IDC_BTN_CONFIRM, GetModuleHandle(NULL), NULL); // Moved down
            
        CreateWindowEx(0, "BUTTON", "Cancel", 
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            320, 410, 100, 30, hDlg, (HMENU)IDC_BTN_CANCEL, GetModuleHandle(NULL), NULL); // Moved down

        // Ensure the confirm button is properly set as default
        SendMessage(hDlg, DM_SETDEFID, IDC_BTN_CONFIRM, 0);

        // Set focus to tab control
        SetFocus(hTabControl);

        return TRUE; // Ensure WM_INITDIALOG returns TRUE if focus was not set manually
    }
    
    case WM_CLOSE:
        LogOut("[GUI_DEBUG] EditDataDlgProc: WM_CLOSE received for hDlg=" + Logger::hwndToString(hDlg), true);
        // Handle the X button the same way as Cancel button
        EndDialog(hDlg, IDCANCEL);
        menuOpen = false;
        LogOut("[GUI] Dialog closed via X button", detailedLogging.load());
        return TRUE;
        
    case WM_COMMAND: {
        WORD cmdID = LOWORD(wParam);
        WORD notifyCode = HIWORD(wParam);
        HWND ctrlHwnd = (HWND)lParam;
        // General WM_COMMAND log kept for broader debugging if needed
        LogOut("[GUI_DEBUG] EditDataDlgProc: WM_COMMAND received. hDlg=" + Logger::hwndToString(hDlg) + ", cmdID=" + std::to_string(cmdID) + ", ctrlHwnd=" + Logger::hwndToString(ctrlHwnd), true);

        if (cmdID == IDC_BTN_SWAP_POS) {
            // LogOut("[GUI_DEBUG] EditDataDlgProc: IDC_BTN_SWAP_POS (2020) detected.", true); // Removed
            char val_x1_text[32], val_y1_text[32], val_x2_text[32], val_y2_text[32];
            double x1, y1, x2, y2;

            // LogOut("[GUI_DEBUG] EditDataDlgProc: Reading X1 from hPage1=" + Logger::hwndToString(hPage1), true); // Removed
            GetDlgItemTextA(hPage1, IDC_X1, val_x1_text, sizeof(val_x1_text));
            x1 = atof(val_x1_text);
            // LogOut("[GUI_DEBUG] EditDataDlgProc: Reading Y1 from hPage1=" + Logger::hwndToString(hPage1), true); // Removed
            GetDlgItemTextA(hPage1, IDC_Y1, val_y1_text, sizeof(val_y1_text));
            y1 = atof(val_y1_text);
            // LogOut("[GUI_DEBUG] EditDataDlgProc: Reading X2 from hPage1=" + Logger::hwndToString(hPage1), true); // Removed
            GetDlgItemTextA(hPage1, IDC_X2, val_x2_text, sizeof(val_x2_text));
            x2 = atof(val_x2_text);
            // LogOut("[GUI_DEBUG] EditDataDlgProc: Reading Y2 from hPage1=" + Logger::hwndToString(hPage1), true); // Removed
            GetDlgItemTextA(hPage1, IDC_Y2, val_y2_text, sizeof(val_y2_text));
            y2 = atof(val_y2_text);
            // LogOut("[GUI_DEBUG] EditDataDlgProc: Values read: x1=" + std::to_string(x1) + ", y1=" + std::to_string(y1) + ", x2=" + std::to_string(x2) + ", y2=" + std::to_string(y2), true); // Removed

            char buffer[32];
            sprintf_s(buffer, "%.2f", x2);
            SetDlgItemTextA(hPage1, IDC_X1, buffer);
            sprintf_s(buffer, "%.2f", y2);
            SetDlgItemTextA(hPage1, IDC_Y1, buffer);
            sprintf_s(buffer, "%.2f", x1);
            SetDlgItemTextA(hPage1, IDC_X2, buffer);
            sprintf_s(buffer, "%.2f", y1);
            SetDlgItemTextA(hPage1, IDC_Y2, buffer);
            
            LogOut("[GUI] Button Swap Positions: Swapped position values in form", true); // Kept this general log
            // LogOut("[GUI_DEBUG] EditDataDlgProc: IDC_BTN_SWAP_POS processed.", true); // Removed
            return TRUE;
        }

        if (cmdID == IDC_BTN_ROUND_START) {
            // LogOut("[GUI_DEBUG] EditDataDlgProc: IDC_BTN_ROUND_START (2021) detected.", true); // Removed
            char buffer[32];
            // LogOut("[GUI_DEBUG] EditDataDlgProc: Setting P1 X1 on hPage1=" + Logger::hwndToString(hPage1) + " to " + std::to_string(p1StartX_const), true); // Removed
            sprintf_s(buffer, "%.2f", p1StartX_const);
            SetDlgItemTextA(hPage1, IDC_X1, buffer);
            // LogOut("[GUI_DEBUG] EditDataDlgProc: Setting P1 Y1 on hPage1=" + Logger::hwndToString(hPage1) + " to " + std::to_string(startY_const), true); // Removed
            sprintf_s(buffer, "%.2f", startY_const);
            SetDlgItemTextA(hPage1, IDC_Y1, buffer);
            // LogOut("[GUI_DEBUG] EditDataDlgProc: Setting P2 X2 on hPage1=" + Logger::hwndToString(hPage1) + " to " + std::to_string(p2StartX_const), true); // Removed
            sprintf_s(buffer, "%.2f", p2StartX_const);
            SetDlgItemTextA(hPage1, IDC_X2, buffer);
            // LogOut("[GUI_DEBUG] EditDataDlgProc: Setting P2 Y2 on hPage1=" + Logger::hwndToString(hPage1) + " to " + std::to_string(startY_const), true); // Removed
            sprintf_s(buffer, "%.2f", startY_const);
            SetDlgItemTextA(hPage1, IDC_Y2, buffer);
            
            LogOut("[GUI] Button Round Start: Set default position values in form", true); // Kept this general log
            // LogOut("[GUI_DEBUG] EditDataDlgProc: IDC_BTN_ROUND_START processed.", true); // Removed
            return TRUE;
        }
        
        // Handle auto-action control changes (typically from hPage3)
        if (cmdID == IDC_AUTOACTION_ACTION && notifyCode == CBN_SELCHANGE) {
            LogOut("[GUI_DEBUG] EditDataDlgProc: IDC_AUTOACTION_ACTION CBN_SELCHANGE detected.", true);
            // Get the selected action type
            int selectedIndex = SendMessage(ctrlHwnd, CB_GETCURSEL, 0, 0);
            
            // Find the Custom MoveID edit control
            HWND hCustomIDEdit = GetDlgItem(hPage3, IDC_AUTOACTION_CUSTOM_ID);
            if (hCustomIDEdit) {
                // Enable Custom MoveID edit when "Custom MoveID" (index 9) is selected
                bool enableCustomEdit = (selectedIndex == 9);
                EnableWindow(hCustomIDEdit, enableCustomEdit);
                
                LogOut("[GUI] Action type changed to index " + std::to_string(selectedIndex) + 
                       ", Custom MoveID field " + (enableCustomEdit ? "enabled" : "disabled"), true);
            }
            return TRUE;
        }
        
        if (cmdID == IDC_BTN_CONFIRM) {
            LogOut("[GUI_DEBUG] EditDataDlgProc: IDC_BTN_CONFIRM detected.", true);
            ProcessFormData(hDlg, hPage1, hPage2, hPage3, pData);
            ApplySettings(pData);
            EndDialog(hDlg, IDOK); 
            menuOpen = false;
            LogOut("[GUI] Confirm button clicked, settings applied", true);
            return TRUE;
        }
        
        if (cmdID == IDC_BTN_CANCEL) { // Changed to 'if' from 'else if' to be safe, though 'else if' is fine here.
            LogOut("[GUI_DEBUG] EditDataDlgProc: IDC_BTN_CANCEL detected.", true);
            EndDialog(hDlg, IDCANCEL);
            menuOpen = false;
            LogOut("[GUI] Cancel button clicked", true);
            return TRUE;
        }
        
        LogOut("[GUI_DEBUG] EditDataDlgProc: WM_COMMAND cmdID=" + std::to_string(cmdID) + " not handled, returning FALSE.", true);
        return FALSE; 
    }
    
    case WM_NOTIFY: { 
        NMHDR* pnmh = (NMHDR*)lParam;
        // LogOut("[GUI_DEBUG] EditDataDlgProc: WM_NOTIFY received. hwndFrom=" + Logger::hwndToString(pnmh->hwndFrom) + ", code=" + std::to_string(pnmh->code), true);
        if (pnmh->hwndFrom == hTabControl && pnmh->code == TCN_SELCHANGE) {
            int iPage = TabCtrl_GetCurSel(hTabControl);
            ShowWindow(hPage1, (iPage == 0) ? SW_SHOW : SW_HIDE);
            ShowWindow(hPage2, (iPage == 1) ? SW_SHOW : SW_HIDE);
            ShowWindow(hPage3, (iPage == 2) ? SW_SHOW : SW_HIDE);
            LogOut("[GUI] Tab changed to page " + std::to_string(iPage), detailedLogging.load());
            return TRUE; // Indicate that we've processed this notification
        }
        return FALSE; // For unhandled notifications
    }
    } // End of switch (message)
    
    return FALSE; // Default processing for unhandled messages
}

// Helper function to process form data
void ProcessFormData(HWND hDlg, HWND hPage1, HWND hPage2, HWND hPage3, DisplayData* pData) {
    char text[32];
    GetDlgItemTextA(hPage1, IDC_HP1, text, sizeof(text)); pData->hp1 = atoi(text);
    GetDlgItemTextA(hPage1, IDC_METER1, text, sizeof(text)); pData->meter1 = atoi(text);
    GetDlgItemTextA(hPage1, IDC_RF1, text, sizeof(text)); pData->rf1 = atof(text);
    GetDlgItemTextA(hPage1, IDC_X1, text, sizeof(text)); pData->x1 = atof(text);
    GetDlgItemTextA(hPage1, IDC_Y1, text, sizeof(text)); pData->y1 = atof(text);
    
    GetDlgItemTextA(hPage1, IDC_HP2, text, sizeof(text)); pData->hp2 = atoi(text);
    GetDlgItemTextA(hPage1, IDC_METER2, text, sizeof(text)); pData->meter2 = atoi(text);
    GetDlgItemTextA(hPage1, IDC_RF2, text, sizeof(text)); pData->rf2 = atof(text);
    GetDlgItemTextA(hPage1, IDC_X2, text, sizeof(text)); pData->x2 = atof(text);
    GetDlgItemTextA(hPage1, IDC_Y2, text, sizeof(text)); pData->y2 = atof(text);
    
    // Process auto-airtech settings
    int airtechDirIndex = SendDlgItemMessage(hPage1, IDC_AIRTECH_DIRECTION, CB_GETCURSEL, 0, 0);
    pData->autoAirtech = (airtechDirIndex > 0);
    pData->airtechDirection = (airtechDirIndex > 0) ? airtechDirIndex - 1 : 0;
    
    // Get airtech delay
    char airtechDelayText[8];
    GetDlgItemTextA(hPage1, IDC_AIRTECH_DELAY, airtechDelayText, sizeof(airtechDelayText));
    pData->airtechDelay = atoi(airtechDelayText);
    
    // Process movement options (auto-jump)
    int jumpDirIndex = SendDlgItemMessage(hPage2, IDC_JUMP_DIRECTION, CB_GETCURSEL, 0, 0);
    pData->autoJump = (jumpDirIndex > 0);  // 0 = disabled, >0 = enabled
    pData->jumpDirection = (jumpDirIndex > 0) ? jumpDirIndex - 1 : 0;  // Convert to 0-based
    
    int jumpTargetIndex = SendDlgItemMessage(hPage2, IDC_JUMP_TARGET, CB_GETCURSEL, 0, 0);
    pData->jumpTarget = jumpTargetIndex + 1;  // Convert to 1-based (1=P1, 2=P2, 3=Both)
    
    // Process auto-action settings
    pData->autoAction = (SendDlgItemMessage(hPage3, IDC_AUTOACTION_ENABLE, BM_GETCHECK, 0, 0) == BST_CHECKED);
    int playerIndex = SendDlgItemMessage(hPage3, IDC_AUTOACTION_PLAYER, CB_GETCURSEL, 0, 0);
    pData->autoActionPlayer = playerIndex + 1;
    
    // After Block trigger
    pData->triggerAfterBlock = (SendDlgItemMessage(hPage3, IDC_TRIGGER_AFTER_BLOCK_CHECK, BM_GETCHECK, 0, 0) == BST_CHECKED);
    int afterBlockActionIndex = SendDlgItemMessage(hPage3, IDC_TRIGGER_AFTER_BLOCK_ACTION, CB_GETCURSEL, 0, 0);
    // Convert to action type using the mapping array
    pData->actionAfterBlock = ComboIndexToActionType[afterBlockActionIndex];
    
    // On Wakeup trigger
    pData->triggerOnWakeup = (SendDlgItemMessage(hPage3, IDC_TRIGGER_ON_WAKEUP_CHECK, BM_GETCHECK, 0, 0) == BST_CHECKED);
    int onWakeupActionIndex = SendDlgItemMessage(hPage3, IDC_TRIGGER_ON_WAKEUP_ACTION, CB_GETCURSEL, 0, 0);
    pData->actionOnWakeup = ComboIndexToActionType[onWakeupActionIndex];
    
    // After Hitstun trigger
    pData->triggerAfterHitstun = (SendDlgItemMessage(hPage3, IDC_TRIGGER_AFTER_HITSTUN_CHECK, BM_GETCHECK, 0, 0) == BST_CHECKED);
    int afterHitstunActionIndex = SendDlgItemMessage(hPage3, IDC_TRIGGER_AFTER_HITSTUN_ACTION, CB_GETCURSEL, 0, 0);
    pData->actionAfterHitstun = ComboIndexToActionType[afterHitstunActionIndex];
    
    // After Airtech trigger
    pData->triggerAfterAirtech = (SendDlgItemMessage(hPage3, IDC_TRIGGER_AFTER_AIRTECH_CHECK, BM_GETCHECK, 0, 0) == BST_CHECKED);
    int afterAirtechActionIndex = SendDlgItemMessage(hPage3, IDC_TRIGGER_AFTER_AIRTECH_ACTION, CB_GETCURSEL, 0, 0);
    pData->actionAfterAirtech = ComboIndexToActionType[afterAirtechActionIndex];
}

// Page subclass procedure
LRESULT CALLBACK PageSubclassProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR dwRefData) {
    HWND hMainDialog = (HWND)dwRefData; 
    // Log entry for every message to PageSubclassProc for deep debugging, can be very noisy
    // LogOut("[GUI_DEBUG] PageSubclassProc: hWnd=" + Logger::hwndToString(hWnd) + ", msg=" + std::to_string(message) + ", hMainDialog=" + Logger::hwndToString(hMainDialog), true);

    switch (message) {
        case WM_COMMAND: {
            WORD cmdID = LOWORD(wParam);
            HWND ctrlHwnd = (HWND)lParam;
            LogOut("[GUI_DEBUG] PageSubclassProc: WM_COMMAND received from hWnd=" + Logger::hwndToString(hWnd) + " (Page" + std::to_string(uIdSubclass) + "). cmdID=" + std::to_string(cmdID) + ", ctrlHwnd=" + Logger::hwndToString(ctrlHwnd) + ". Forwarding to hMainDialog=" + Logger::hwndToString(hMainDialog), true);
            return SendMessage(hMainDialog, message, wParam, lParam);
        }
        // Potentially log other messages if needed for debugging tab behavior etc.
        // case WM_CTLCOLORSTATIC:
        // case WM_CTLCOLORBTN:
        //     LogOut("[GUI_DEBUG] PageSubclassProc: WM_CTLCOLOR received for hWnd=" + Logger::hwndToString(hWnd), true);
        //     break;
    }
    return DefSubclassProc(hWnd, message, wParam, lParam);
}