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
    // static HWND hPage2 = NULL; // REMOVED
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
        // tie.pszText = (LPSTR)"Movement Options"; // REMOVED
        // TabCtrl_InsertItem(hTabControl, 1, &tie); // REMOVED
        tie.pszText = (LPSTR)"Auto Action";
        TabCtrl_InsertItem(hTabControl, 1, &tie); // Index changed from 2 to 1

        // Create page containers
        hPage1 = CreateWindowEx(
            0, "STATIC", "", 
            WS_CHILD | WS_VISIBLE, 
            0, 0, 0, 0, hDlg, NULL, GetModuleHandle(NULL), NULL);
        LogOut("[GUI_DEBUG] EditDataDlgProc: Created hPage1=" + Logger::hwndToString(hPage1), true);
            
        /* REMOVED
        hPage2 = CreateWindowEx(
            0, "STATIC", "", 
            WS_CHILD | WS_VISIBLE, 
            0, 0, 0, 0, hDlg, NULL, GetModuleHandle(NULL), NULL);
        LogOut("[GUI_DEBUG] EditDataDlgProc: Created hPage2=" + Logger::hwndToString(hPage2), true);
        */
        
        hPage3 = CreateWindowEx(
            0, "STATIC", "", 
            WS_CHILD | WS_VISIBLE, 
            0, 0, 0, 0, hDlg, NULL, GetModuleHandle(NULL), NULL);
        LogOut("[GUI_DEBUG] EditDataDlgProc: Created hPage3=" + Logger::hwndToString(hPage3), true);
        
        // Subclass ALL page containers to forward WM_COMMAND messages
        BOOL sub1 = SetWindowSubclass(hPage1, PageSubclassProc, 1, (DWORD_PTR)hDlg);
        LogOut("[GUI_DEBUG] EditDataDlgProc: SetWindowSubclass for hPage1 result: " + std::to_string(sub1) + ", hPage1=" + Logger::hwndToString(hPage1) + ", hDlg=" + Logger::hwndToString(hDlg), true);
        // BOOL sub2 = SetWindowSubclass(hPage2, PageSubclassProc, 2, (DWORD_PTR)hDlg); // REMOVED
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
        /* REMOVED
        SetWindowPos(hPage2, NULL, 
            rc.left + 10, rc.top + 10,
            rc.right - rc.left - 20, rc.bottom - rc.top - 20,
            SWP_NOZORDER);
        */
        SetWindowPos(hPage3, NULL, 
            rc.left + 10, rc.top + 10,
            rc.right - rc.left - 20, rc.bottom - rc.top - 20,
            SWP_NOZORDER);
    
        // Create page content
        GameValuesPage_CreateContent(hPage1, pData);
        AutoActionPage_CreateContent(hPage3, pData);

        // Show the first page by default
        ShowWindow(hPage1, SW_SHOW);
        ShowWindow(hPage3, SW_HIDE);
        
        // Set the title of the cancel button to "Exit"
        HWND hCancelButton = GetDlgItem(hDlg, IDC_BTN_CANCEL);
        if (hCancelButton) {
            SetWindowText(hCancelButton, "Exit");
        }

        return (INT_PTR)TRUE;
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
            ProcessFormData(hDlg, hPage1, hPage3, pData);
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
        switch (((LPNMHDR)lParam)->code) {
        case TCN_SELCHANGE: {
            int iPage = TabCtrl_GetCurSel(hTabControl);
            ShowWindow(hPage1, (iPage == 0) ? SW_SHOW : SW_HIDE);
            ShowWindow(hPage3, (iPage == 1) ? SW_SHOW : SW_HIDE); // Index changed from 2 to 1
            break;
        }
        }
        break;
    }
    } // End of switch (message)
    
    return (INT_PTR)FALSE;
}

// Helper function to process form data
void ProcessFormData(HWND hDlg, HWND hPage1, HWND hPage3, DisplayData* pData) {
    if (!pData) return;

    char buffer[256];

    // --- Page 1: Game Values ---
    GetDlgItemText(hPage1, IDC_HP1, buffer, 256); pData->hp1 = atoi(buffer);
    GetDlgItemText(hPage1, IDC_METER1, buffer, 256); pData->meter1 = atoi(buffer);
    GetDlgItemText(hPage1, IDC_RF1, buffer, 256); pData->rf1 = atof(buffer);
    GetDlgItemText(hPage1, IDC_X1, buffer, 256); pData->x1 = atof(buffer);
    GetDlgItemText(hPage1, IDC_Y1, buffer, 256); pData->y1 = atof(buffer);

    GetDlgItemText(hPage1, IDC_HP2, buffer, 256); pData->hp2 = atoi(buffer);
    GetDlgItemText(hPage1, IDC_METER2, buffer, 256); pData->meter2 = atoi(buffer);
    GetDlgItemText(hPage1, IDC_RF2, buffer, 256); pData->rf2 = atof(buffer);
    GetDlgItemText(hPage1, IDC_X2, buffer, 256); pData->x2 = atof(buffer);
    GetDlgItemText(hPage1, IDC_Y2, buffer, 256); pData->y2 = atof(buffer);

    int airtechIndex = SendMessage(GetDlgItem(hPage1, IDC_AIRTECH_DIRECTION), CB_GETCURSEL, 0, 0);
    pData->autoAirtech = (airtechIndex > 0);
    pData->airtechDirection = (airtechIndex > 0) ? airtechIndex - 1 : 0;

    GetDlgItemText(hPage1, IDC_AIRTECH_DELAY, buffer, 256);
    pData->airtechDelay = atoi(buffer);

    // --- Page 3: Auto Action ---
    pData->autoAction = (IsDlgButtonChecked(hPage3, IDC_AUTOACTION_ENABLE) == BST_CHECKED);
    pData->autoActionPlayer = SendMessage(GetDlgItem(hPage3, IDC_AUTOACTION_PLAYER), CB_GETCURSEL, 0, 0) + 1;

    // After Block
    pData->triggerAfterBlock = (IsDlgButtonChecked(hPage3, IDC_TRIGGER_AFTER_BLOCK_CHECK) == BST_CHECKED);
    pData->actionAfterBlock = ComboIndexToActionType[SendMessage(GetDlgItem(hPage3, IDC_TRIGGER_AFTER_BLOCK_ACTION), CB_GETCURSEL, 0, 0)];
    GetDlgItemText(hPage3, IDC_TRIGGER_AFTER_BLOCK_DELAY, buffer, 256); pData->delayAfterBlock = atoi(buffer);

    // After Hitstun
    pData->triggerAfterHitstun = (IsDlgButtonChecked(hPage3, IDC_TRIGGER_AFTER_HITSTUN_CHECK) == BST_CHECKED);
    pData->actionAfterHitstun = ComboIndexToActionType[SendMessage(GetDlgItem(hPage3, IDC_TRIGGER_AFTER_HITSTUN_ACTION), CB_GETCURSEL, 0, 0)];
    GetDlgItemText(hPage3, IDC_TRIGGER_AFTER_HITSTUN_DELAY, buffer, 256); pData->delayAfterHitstun = atoi(buffer);

    // On Wakeup
    pData->triggerOnWakeup = (IsDlgButtonChecked(hPage3, IDC_TRIGGER_ON_WAKEUP_CHECK) == BST_CHECKED);
    pData->actionOnWakeup = ComboIndexToActionType[SendMessage(GetDlgItem(hPage3, IDC_TRIGGER_ON_WAKEUP_ACTION), CB_GETCURSEL, 0, 0)];
    GetDlgItemText(hPage3, IDC_TRIGGER_ON_WAKEUP_DELAY, buffer, 256); pData->delayOnWakeup = atoi(buffer);

    // After Airtech
    pData->triggerAfterAirtech = (IsDlgButtonChecked(hPage3, IDC_TRIGGER_AFTER_AIRTECH_CHECK) == BST_CHECKED);
    pData->actionAfterAirtech = ComboIndexToActionType[SendMessage(GetDlgItem(hPage3, IDC_TRIGGER_AFTER_AIRTECH_ACTION), CB_GETCURSEL, 0, 0)];
    GetDlgItemText(hPage3, IDC_TRIGGER_AFTER_AIRTECH_DELAY, buffer, 256); pData->delayAfterAirtech = atoi(buffer);
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