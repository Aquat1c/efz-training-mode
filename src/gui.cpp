#include <fstream>
#include <iomanip>
#include <chrono>
#include "../include/gui.h"
#include "../include/constants.h"
#include "../include/memory.h"
#include "../include/utilities.h"
#include "../include/logger.h"
//#include "../resource/cpp_resource.h" 
#include <windows.h>
#include <string>
#include <thread>
#include <commctrl.h>
#include <windowsx.h>

// Add this link to the Common Controls library
#pragma comment(lib, "comctl32.lib")

// Forward declaration of the PageSubclassProc function
LRESULT CALLBACK PageSubclassProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR dwRefData);

void OpenMenu() {
    // Initialize common controls for tab control support
    INITCOMMONCONTROLSEX icc;
    icc.dwSize = sizeof(INITCOMMONCONTROLSEX);
    icc.dwICC = ICC_TAB_CLASSES;
    InitCommonControlsEx(&icc);

    // Check if we're in EFZ window
    if (!IsEFZWindowActive()) {
        LogOut("[GUI] Cannot open menu: EFZ window not active", true);
        return;
    }

    // Don't open menu if it's already open
    if (menuOpen) {
        return;
    }

    menuOpen = true;
    LogOut("[GUI] Opening config menu", true);

    // Get current values from memory
    uintptr_t base = GetEFZBase();
    
    if (base) {
        // Enhanced logging to debug P2 data loading
        LogOut("[GUI] Reading values from memory with base: " + std::to_string(base), detailedLogging);
        
        // Resolve P1 pointers with validation
        uintptr_t hpAddr1 = ResolvePointer(base, EFZ_BASE_OFFSET_P1, HP_OFFSET);
        uintptr_t meterAddr1 = ResolvePointer(base, EFZ_BASE_OFFSET_P1, METER_OFFSET);
        uintptr_t rfAddr1 = ResolvePointer(base, EFZ_BASE_OFFSET_P1, RF_OFFSET);
        uintptr_t xAddr1 = ResolvePointer(base, EFZ_BASE_OFFSET_P1, XPOS_OFFSET);
        uintptr_t yAddr1 = ResolvePointer(base, EFZ_BASE_OFFSET_P1, YPOS_OFFSET);
        
        // Resolve P2 pointers with validation
        uintptr_t hpAddr2 = ResolvePointer(base, EFZ_BASE_OFFSET_P2, HP_OFFSET);
        uintptr_t meterAddr2 = ResolvePointer(base, EFZ_BASE_OFFSET_P2, METER_OFFSET);
        uintptr_t rfAddr2 = ResolvePointer(base, EFZ_BASE_OFFSET_P2, RF_OFFSET);
        uintptr_t xAddr2 = ResolvePointer(base, EFZ_BASE_OFFSET_P2, XPOS_OFFSET);
        uintptr_t yAddr2 = ResolvePointer(base, EFZ_BASE_OFFSET_P2, YPOS_OFFSET);

        // Debug output of resolved addresses
        LogOut("[GUI] P1 HP Addr: " + std::to_string(hpAddr1) + 
               ", P2 HP Addr: " + std::to_string(hpAddr2), detailedLogging);
        LogOut("[GUI] P1 Meter Addr: " + std::to_string(meterAddr1) + 
               ", P2 Meter Addr: " + std::to_string(meterAddr2), detailedLogging);

        // Read P1 values with explicit error checking
        if (hpAddr1) {
            memcpy(&displayData.hp1, (void*)hpAddr1, sizeof(WORD));
        } else {
            displayData.hp1 = 2000; // Default value
            LogOut("[GUI] Failed to resolve P1 HP address", detailedLogging);
        }
        
        if (meterAddr1) {
            memcpy(&displayData.meter1, (void*)meterAddr1, sizeof(WORD));
        } else {
            displayData.meter1 = 0; // Default value
            LogOut("[GUI] Failed to resolve P1 Meter address", detailedLogging);
        }
        
        if (rfAddr1) {
            double rf = 0.0;
            memcpy(&rf, (void*)rfAddr1, sizeof(double));
            displayData.rf1 = rf;
        } else {
            displayData.rf1 = 0.0; // Default value
        }
        
        if (xAddr1) {
            memcpy(&displayData.x1, (void*)xAddr1, sizeof(double));
        } else {
            displayData.x1 = 240.0; // Default value
        }
        
        if (yAddr1) {
            memcpy(&displayData.y1, (void*)yAddr1, sizeof(double));
        } else {
            displayData.y1 = 0.0; // Default value
        }
        
        // Read P2 values with explicit error checking
        if (hpAddr2) {
            memcpy(&displayData.hp2, (void*)hpAddr2, sizeof(WORD));
            LogOut("[GUI] Successfully read P2 HP: " + std::to_string(displayData.hp2), detailedLogging);
        } else {
            displayData.hp2 = 2000; // Default value
            LogOut("[GUI] Failed to resolve P2 HP address", true); // Log as error
        }
        
        if (meterAddr2) {
            memcpy(&displayData.meter2, (void*)meterAddr2, sizeof(WORD));
            LogOut("[GUI] Successfully read P2 Meter: " + std::to_string(displayData.meter2), detailedLogging);
        } else {
            displayData.meter2 = 0; // Default value
            LogOut("[GUI] Failed to resolve P2 Meter address", true); // Log as error
        }
        
        if (rfAddr2) {
            double rf = 0.0;
            memcpy(&rf, (void*)rfAddr2, sizeof(double));
            displayData.rf2 = rf;
        } else {
            displayData.rf2 = 0.0; // Default value
        }
        
        if (xAddr2) {
            memcpy(&displayData.x2, (void*)xAddr2, sizeof(double));
        } else {
            displayData.x2 = 400.0; // Default value
        }
        
        if (yAddr2) {
            memcpy(&displayData.y2, (void*)yAddr2, sizeof(double));
        } else {
            displayData.y2 = 0.0; // Default value
        }

        // Load auto-airtech settings
        displayData.autoAirtech = autoAirtechEnabled.load();
        displayData.airtechDirection = autoAirtechDirection.load();

        // Load auto-jump settings
        displayData.autoJump = autoJumpEnabled.load();
        displayData.jumpDirection = jumpDirection.load();
        displayData.jumpTarget = jumpTarget.load();

        // Show dialog
        HWND hWnd = GetForegroundWindow();
        
        // Call the dialog in a separate thread to avoid blocking the main thread
        std::thread([hWnd]() {
            ShowEditDataDialog(hWnd);
            menuOpen = false;
        }).detach();
    }
    else {
        LogOut("[GUI] Failed to get EFZ base address", true);
        menuOpen = false;
    }
}

void ShowEditDataDialog(HWND hParent) {
    // Create a dialog template with sizes that match our controls
    static WORD dlgTemplate[128];
    ZeroMemory(dlgTemplate, sizeof(dlgTemplate));
    DLGTEMPLATE* dlg = (DLGTEMPLATE*)dlgTemplate;
    dlg->style = DS_SETFONT | DS_MODALFRAME | WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_FIXEDSYS;
    dlg->dwExtendedStyle = 0;
    dlg->cdit = 0;
    dlg->x = 10; dlg->y = 10;
    dlg->cx = 280; //Width of the dialog
    dlg->cy = 240; // Height of the dialog
    
    // Create and show the dialog
    DialogBoxIndirectParamA(GetModuleHandle(NULL), (LPCDLGTEMPLATEA)dlgTemplate, hParent, EditDataDlgProc, (LPARAM)&displayData);
}

INT_PTR CALLBACK EditDataDlgProc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam) {
    static DisplayData* pData = nullptr;
    static HWND hTabControl = NULL;
    static HWND hPage1 = NULL;
    static HWND hPage2 = NULL;
    
    // Round start positions
    const double p1StartX = 240.0, p2StartX = 400.0, startY = 0.0;

    switch (message) {
    case WM_INITDIALOG: {
        pData = (DisplayData*)lParam;
        
        // Initialize common controls
        INITCOMMONCONTROLSEX icc;
        icc.dwSize = sizeof(INITCOMMONCONTROLSEX);
        icc.dwICC = ICC_TAB_CLASSES;
        InitCommonControlsEx(&icc);
        
        // Create a tab control that fits the dialog
        hTabControl = CreateWindowEx(0, WC_TABCONTROL, NULL, 
            WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | TCS_FOCUSONBUTTONDOWN,
            10, 10, 560, 370, hDlg, (HMENU)IDC_TAB_CONTROL, GetModuleHandle(NULL), NULL);

        // Add tab items
        TCITEM tie;
        tie.mask = TCIF_TEXT;
        tie.pszText = (LPSTR)"Game Values";
        TabCtrl_InsertItem(hTabControl, 0, &tie);
        tie.pszText = (LPSTR)"Movement Options";
        TabCtrl_InsertItem(hTabControl, 1, &tie);

        // Create page containers with subclassing
        hPage1 = CreateWindowEx(
            0, "STATIC", "", 
            WS_CHILD | WS_VISIBLE, 
            0, 0, 0, 0, hDlg, NULL, GetModuleHandle(NULL), NULL);
            
        hPage2 = CreateWindowEx(
            0, "STATIC", "", 
            WS_CHILD | WS_VISIBLE, 
            0, 0, 0, 0, hDlg, NULL, GetModuleHandle(NULL), NULL);
        
        // Subclass both page windows to forward button messages
        SetWindowSubclass(hPage1, PageSubclassProc, 1, (DWORD_PTR)hDlg);
        SetWindowSubclass(hPage2, PageSubclassProc, 2, (DWORD_PTR)hDlg);
        
        // Position pages in tab control
        RECT rc;
        GetClientRect(hTabControl, &rc);
        TabCtrl_AdjustRect(hTabControl, FALSE, &rc);

        // Log tab rect dimensions for debugging
        LogOut("[GUI] Tab client area: left=" + std::to_string(rc.left) + 
               ", top=" + std::to_string(rc.top) + 
               ", right=" + std::to_string(rc.right) + 
               ", bottom=" + std::to_string(rc.bottom), true);

        // Position pages with the adjusted rectangle
        SetWindowPos(hPage1, NULL, 
            rc.left + 10, rc.top + 10,  // Add padding 
            rc.right - rc.left - 20, rc.bottom - rc.top - 20,  // Subtract padding
            SWP_NOZORDER);
        SetWindowPos(hPage2, NULL, 
            rc.left + 10, rc.top + 10,  // Add padding
            rc.right - rc.left - 20, rc.bottom - rc.top - 20,  // Subtract padding
            SWP_NOZORDER);

        // Create page content
        GameValuesPage_CreateContent(hPage1, pData);
        MovementOptionsPage_CreateContent(hPage2, pData);

        // Show first page, hide second page
        ShowWindow(hPage1, SW_SHOW);
        ShowWindow(hPage2, SW_HIDE);

        // Update button positions to be within the dialog
        HWND hConfirmBtn = CreateWindowEx(0, "BUTTON", "Confirm", 
            WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
            180, 410, 100, 30, hDlg, (HMENU)IDC_BTN_CONFIRM, GetModuleHandle(NULL), NULL);
            
        CreateWindowEx(0, "BUTTON", "Cancel", 
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            300, 410, 100, 30, hDlg, (HMENU)IDC_BTN_CANCEL, GetModuleHandle(NULL), NULL);

        // Ensure the confirm button is properly set as default
        SendMessage(hDlg, DM_SETDEFID, IDC_BTN_CONFIRM, 0);

        // Set focus to tab control
        SetFocus(hTabControl);

        return TRUE;
    }
    
    case WM_CLOSE:
        // Handle the X button the same way as Cancel button
        EndDialog(hDlg, IDCANCEL);
        menuOpen = false;  // Make sure we reset the menuOpen flag
        LogOut("[GUI] Dialog closed via X button", detailedLogging);
        return TRUE;
        
    case WM_COMMAND: {
        WORD cmdID = LOWORD(wParam);
        WORD notifyCode = HIWORD(wParam);
        HWND ctrlHwnd = (HWND)lParam;

        // More detailed logging for debugging button clicks
        LogOut("[GUI] Command received: ID=" + std::to_string(cmdID) + 
              ", NotifyCode=" + std::to_string(notifyCode) + 
              ", Control=0x" + std::to_string((uintptr_t)ctrlHwnd), true);
        
        // Handle button clicks
        if (cmdID == IDC_BTN_CONFIRM) {
            // Read values from controls
            char buffer[32];
            
            // Get Player 1 values
            GetDlgItemTextA(hPage1, IDC_HP1, buffer, sizeof(buffer));
            pData->hp1 = CLAMP(atoi(buffer), 0, MAX_HP);

            GetDlgItemTextA(hPage1, IDC_METER1, buffer, sizeof(buffer));
            pData->meter1 = CLAMP(atoi(buffer), 0, MAX_METER);

            GetDlgItemTextA(hPage1, IDC_RF1, buffer, sizeof(buffer));
            pData->rf1 = CLAMP(atof(buffer), 0.0, static_cast<double>(MAX_RF));
            
            // For P1 X coordinate
            GetDlgItemTextA(hPage1, IDC_X1, buffer, sizeof(buffer));
            // Replace commas with periods to ensure proper parsing regardless of locale
            for (char* p = buffer; *p; ++p) {
                if (*p == ',') *p = '.';
            }
            pData->x1 = CLAMP(atof(buffer), -1000.0, 1000.0);
            
            // For P1 Y coordinate  
            GetDlgItemTextA(hPage1, IDC_Y1, buffer, sizeof(buffer));
            for (char* p = buffer; *p; ++p) {
                if (*p == ',') *p = '.';
            }
            pData->y1 = CLAMP(atof(buffer), -1000.0, 1000.0);
            
            // Get Player 2 values
            GetDlgItemTextA(hPage1, IDC_HP2, buffer, sizeof(buffer));
            pData->hp2 = CLAMP(atoi(buffer), 0, MAX_HP);
            
            GetDlgItemTextA(hPage1, IDC_METER2, buffer, sizeof(buffer));
            pData->meter2 = CLAMP(atoi(buffer), 0, MAX_METER);

            GetDlgItemTextA(hPage1, IDC_RF2, buffer, sizeof(buffer));
            pData->rf2 = CLAMP(atof(buffer), 0.0, static_cast<double>(MAX_RF));
            
            // For P2 X coordinate
            GetDlgItemTextA(hPage1, IDC_X2, buffer, sizeof(buffer));
            for (char* p = buffer; *p; ++p) {
                if (*p == ',') *p = '.';
            }
            pData->x2 = CLAMP(atof(buffer), -1000.0, 1000.0);
            
            // For P2 Y coordinate
            GetDlgItemTextA(hPage1, IDC_Y2, buffer, sizeof(buffer));
            for (char* p = buffer; *p; ++p) {
                if (*p == ',') *p = '.';
            }
            pData->y2 = CLAMP(atof(buffer), -1000.0, 1000.0);
            
            // Get airtech settings from combo box
            int airtechSelection = SendMessage(GetDlgItem(hPage1, IDC_AIRTECH_DIRECTION), CB_GETCURSEL, 0, 0);
            if (airtechSelection > 0) {
                pData->autoAirtech = true;
                pData->airtechDirection = airtechSelection - 1;
            } else {
                pData->autoAirtech = false;
                pData->airtechDirection = 0;
            }
            
            // Get jump settings from combo boxes
            int jumpDirSelection = SendMessage(GetDlgItem(hPage2, IDC_JUMP_DIRECTION), CB_GETCURSEL, 0, 0);
            if (jumpDirSelection > 0) {
                pData->autoJump = true;
                pData->jumpDirection = jumpDirSelection - 1;
            } else {
                pData->autoJump = false;
                pData->jumpDirection = 0;
            }
            
            // Get jump target
            int targetSelection = SendMessage(GetDlgItem(hPage2, IDC_JUMP_TARGET), CB_GETCURSEL, 0, 0);
            pData->jumpTarget = targetSelection + 1;  // Convert from 0-based to 1-based
            
            // Apply settings to game memory
            ApplySettings(pData);
            
            EndDialog(hDlg, IDOK);
            return TRUE;
        }
        else if (cmdID == IDC_BTN_CANCEL) {
            EndDialog(hDlg, IDCANCEL);
            return TRUE;
        }
        else if (cmdID == IDC_BTN_SWAP_POS) {
            LogOut("[GUI] Swap Positions button clicked", true);
            
            // Get current position values
            char p1x[32] = {0}, p1y[32] = {0}, p2x[32] = {0}, p2y[32] = {0};
            
            GetDlgItemTextA(hPage1, IDC_X1, p1x, sizeof(p1x));
            GetDlgItemTextA(hPage1, IDC_Y1, p1y, sizeof(p1y));
            GetDlgItemTextA(hPage1, IDC_X2, p2x, sizeof(p2x));
            GetDlgItemTextA(hPage1, IDC_Y2, p2y, sizeof(p2y));
            
            // Log current values for debugging
            LogOut("[GUI] Before swap - P1: " + std::string(p1x) + "," + std::string(p1y) + 
                   " P2: " + std::string(p2x) + "," + std::string(p2y), true);
            
            // Simple swap of text fields
            SetDlgItemTextA(hPage1, IDC_X1, p2x);
            SetDlgItemTextA(hPage1, IDC_Y1, p2y);
            SetDlgItemTextA(hPage1, IDC_X2, p1x);
            SetDlgItemTextA(hPage1, IDC_Y2, p1y);
            
            LogOut("[GUI] Positions swapped", true);
            return TRUE;
        }
        else if (cmdID == IDC_BTN_ROUND_START) {
            LogOut("[GUI] Round Start button clicked", true);
            
            // Set round start positions
            char buffer[32];
            
            sprintf_s(buffer, "%.1f", p1StartX);
            SetDlgItemTextA(hPage1, IDC_X1, buffer);
            
            sprintf_s(buffer, "%.1f", startY);
            SetDlgItemTextA(hPage1, IDC_Y1, buffer);
            
            sprintf_s(buffer, "%.1f", p2StartX);
            SetDlgItemTextA(hPage1, IDC_X2, buffer);
            
            sprintf_s(buffer, "%.1f", startY);
            SetDlgItemTextA(hPage1, IDC_Y2, buffer);
            
            LogOut("[GUI] Round start positions set", true);
            return TRUE;
        }
        else if (((LPNMHDR)lParam)->code == TCN_SELCHANGE) {
            // Tab selection changed
            int iPage = TabCtrl_GetCurSel(hTabControl);
            
            // Show selected page
            if (iPage == 0) {
                ShowWindow(hPage1, SW_SHOW);
                ShowWindow(hPage2, SW_HIDE);
            }
            else if (iPage == 1) {
                ShowWindow(hPage1, SW_HIDE);
                ShowWindow(hPage2, SW_SHOW);
            }
            
            return TRUE;
        }
        
        return FALSE;
    }
    
    case WM_NOTIFY:
        if (((LPNMHDR)lParam)->code == TCN_SELCHANGE) {
            // Tab selection changed
            int iPage = TabCtrl_GetCurSel(hTabControl);
            
            // Hide both pages
            ShowWindow(hPage1, SW_HIDE);
            ShowWindow(hPage2, SW_HIDE);
            
            // Show the selected page
            if (iPage == 0) {
                ShowWindow(hPage1, SW_SHOW);
                SetFocus(hPage1);
            } else if (iPage == 1) {
                ShowWindow(hPage2, SW_SHOW);
                SetFocus(hPage2);
            }
            
            return TRUE;
        }
        break;
    }
    
    return FALSE;
}

void ApplySettings(DisplayData* data) {
    if (!data) {
        LogOut("[ERROR] ApplySettings called with null data pointer", true);
        return;
    }

    try {
        // Force C locale for consistent decimal handling
        std::locale oldLocale = std::locale::global(std::locale("C"));
        
        // Validate all floating point inputs
        if (!std::isfinite(data->x1)) data->x1 = 240.0;
        if (!std::isfinite(data->y1)) data->y1 = 0.0;
        if (!std::isfinite(data->x2)) data->x2 = 400.0;
        if (!std::isfinite(data->y2)) data->y2 = 0.0;
        if (!std::isfinite(data->rf1)) data->rf1 = 0.0;
        if (!std::isfinite(data->rf2)) data->rf2 = 0.0;
        
        uintptr_t base = GetEFZBase();
        if (!base) {
            LogOut("[SETTINGS] Failed to get game base address", true);
            std::locale::global(oldLocale); // Restore locale before returning
            return;
        }
        
        // Apply player 1 values - with comprehensive error checking
        uintptr_t hpAddr1 = ResolvePointer(base, EFZ_BASE_OFFSET_P1, HP_OFFSET);
        if (hpAddr1) {
            WriteGameMemory(hpAddr1, &data->hp1, sizeof(WORD));
        }
        
        uintptr_t meterAddr1 = ResolvePointer(base, EFZ_BASE_OFFSET_P1, METER_OFFSET);
        if (meterAddr1) {
            WriteGameMemory(meterAddr1, &data->meter1, sizeof(WORD));
        }
        
        uintptr_t rfAddr1 = ResolvePointer(base, EFZ_BASE_OFFSET_P1, RF_OFFSET);
        if (rfAddr1) {
            float rf = static_cast<float>(data->rf1);
            WriteGameMemory(rfAddr1, &rf, sizeof(float));
        }
        
        uintptr_t xAddr1 = ResolvePointer(base, EFZ_BASE_OFFSET_P1, XPOS_OFFSET);
        if (xAddr1) {
            WriteGameMemory(xAddr1, &data->x1, sizeof(double));
        }
        
        uintptr_t yAddr1 = ResolvePointer(base, EFZ_BASE_OFFSET_P1, YPOS_OFFSET);
        if (yAddr1) {
            WriteGameMemory(yAddr1, &data->y1, sizeof(double));
        }
        
        // Apply player 2 values
        uintptr_t hpAddr2 = ResolvePointer(base, EFZ_BASE_OFFSET_P2, HP_OFFSET);
        if (hpAddr2) {
            LogOut("[SETTINGS] P2 HP address: " + std::to_string(hpAddr2) + ", value: " + std::to_string(data->hp2), detailedLogging);
            WriteGameMemory(hpAddr2, &data->hp2, sizeof(WORD));
        } else {
            LogOut("[ERROR] Failed to resolve P2 HP address", true);
        }
        
        uintptr_t meterAddr2 = ResolvePointer(base, EFZ_BASE_OFFSET_P2, METER_OFFSET);
        if (meterAddr2) {
            WriteGameMemory(meterAddr2, &data->meter2, sizeof(WORD));
        }
        
        uintptr_t rfAddr2 = ResolvePointer(base, EFZ_BASE_OFFSET_P2, RF_OFFSET);
        if (rfAddr2) {
            float rf = static_cast<float>(data->rf2);
            WriteGameMemory(rfAddr2, &rf, sizeof(float));
        }
        
        uintptr_t xAddr2 = ResolvePointer(base, EFZ_BASE_OFFSET_P2, XPOS_OFFSET);
        if (xAddr2) {
            WriteGameMemory(xAddr2, &data->x2, sizeof(double));
        }
        
        uintptr_t yAddr2 = ResolvePointer(base, EFZ_BASE_OFFSET_P2, YPOS_OFFSET);
        if (yAddr2) {
            WriteGameMemory(yAddr2, &data->y2, sizeof(double));
        }
        
        // Restore original locale when done
        std::locale::global(oldLocale);
        
        // Update global variables
        autoAirtechEnabled = data->autoAirtech;
        autoAirtechDirection = data->airtechDirection;
        autoJumpEnabled = data->autoJump;
        jumpDirection = data->jumpDirection;
        jumpTarget = data->jumpTarget;
        
        LogOut("[SETTINGS] Applied settings from GUI", true);
    } catch (const std::exception& e) {
        LogOut("[ERROR] Exception in ApplySettings: " + std::string(e.what()), true);
    } catch (...) {
        LogOut("[ERROR] Unknown exception in ApplySettings", true);
    }
}

// Add these new helper functions for creating page content

// Create all the controls for the Game Values page
void GameValuesPage_CreateContent(HWND hParent, DisplayData* pData) {
    // Get the size of the parent window for better scaling
    RECT rc;
    GetClientRect(hParent, &rc);
    int width = rc.right - rc.left;
    int height = rc.bottom - rc.top;
    
    // Calculate better positioned fields based on available space
    struct { LPCSTR label; int x; int y; int id; int width; } fields[] = {
        {"P1 HP:", width/20, height/10, IDC_HP1, width/7},
        {"P1 Meter:", width/20, height*2/10, IDC_METER1, width/7},
        {"P1 RF:", width/20, height*3/10, IDC_RF1, width/7},
        {"P1 X:", width/20, height*4/10, IDC_X1, width/5},
        {"P1 Y:", width/20, height*5/10, IDC_Y1, width/5},
        {"P2 HP:", width*3/5, height/10, IDC_HP2, width/7},
        {"P2 Meter:", width*3/5, height*2/10, IDC_METER2, width/7},
        {"P2 RF:", width*3/5, height*3/10, IDC_RF2, width/7},
        {"P2 X:", width*3/5, height*4/10, IDC_X2, width/5},
        {"P2 Y:", width*3/5, height*5/10, IDC_Y2, width/5}
    };

    // Create fields with responsive sizing
    for (int i = 0; i < 10; ++i) {
        CreateWindowEx(0, "STATIC", fields[i].label, 
            WS_CHILD | WS_VISIBLE,
            fields[i].x, fields[i].y, width/10, height/20, 
            hParent, NULL, GetModuleHandle(NULL), NULL);
        
        DWORD style = WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL;
        if (fields[i].id != IDC_X1 && fields[i].id != IDC_Y1 &&
            fields[i].id != IDC_X2 && fields[i].id != IDC_Y2) {
            style |= ES_NUMBER;
        }
        
        CreateWindowEx(0, "EDIT", "", style,
            fields[i].x + width/10, fields[i].y, fields[i].width, height/20, 
            hParent, (HMENU)fields[i].id, GetModuleHandle(NULL), NULL);
    }

    // Buttons with better positioning
    CreateWindowEx(0, "BUTTON", "Swap Positions", 
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        width/5, height*6/10, width/4, height/15, 
        hParent, (HMENU)IDC_BTN_SWAP_POS, GetModuleHandle(NULL), NULL);
    
    CreateWindowEx(0, "BUTTON", "Round Start", 
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        width*3/5, height*6/10, width/4, height/15, 
        hParent, (HMENU)IDC_BTN_ROUND_START, GetModuleHandle(NULL), NULL);

    // Separator with better spacing
    CreateWindowEx(0, "STATIC", "", 
        WS_CHILD | WS_VISIBLE | SS_ETCHEDHORZ,
        width/20, height*7/10, width*9/10, 2, 
        hParent, NULL, GetModuleHandle(NULL), NULL);

    // Auto-Airtech section with better positioning
    CreateWindowEx(0, "STATIC", "Auto-Airtech Direction:", 
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        width/10, height*8/10, width/4, height/20, 
        hParent, NULL, GetModuleHandle(NULL), NULL);

    // Create combo box with better positioning
    HWND hAirtechCombo = CreateWindowEx(0, "COMBOBOX", "", 
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
        width*4/10, height*8/10, width*5/10, 120, // Changed from height/6 to fixed 120
        hParent, (HMENU)IDC_AIRTECH_DIRECTION, GetModuleHandle(NULL), NULL);
    
    // Add items to the airtech combo box
    SendMessage(hAirtechCombo, CB_RESETCONTENT, 0, 0);
    SendMessage(hAirtechCombo, CB_ADDSTRING, 0, (LPARAM)"Neutral (Disabled)");
    SendMessage(hAirtechCombo, CB_ADDSTRING, 0, (LPARAM)"Forward");
    SendMessage(hAirtechCombo, CB_ADDSTRING, 0, (LPARAM)"Backward");

    // Set selected item based on current settings
    int airtechIndex = 0; // Default to Neutral (Disabled)
    if (pData->autoAirtech) {
        airtechIndex = pData->airtechDirection + 1; // +1 because index 0 is "Neutral"
    }
    if (airtechIndex < 0 || airtechIndex > 2) airtechIndex = 0; // Validate index
    SendMessage(hAirtechCombo, CB_SETCURSEL, airtechIndex, 0);

    // Add tooltip for auto-airtech combo
    HWND hToolTip = CreateWindowEx(0, TOOLTIPS_CLASS, NULL,
        WS_POPUP | TTS_ALWAYSTIP,
        CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
        hParent, NULL, GetModuleHandle(NULL), NULL);
        
    TOOLINFO toolInfo = { 0 };
    toolInfo.cbSize = sizeof(toolInfo);
    toolInfo.hwnd = hParent;
    toolInfo.uFlags = TTF_SUBCLASS | TTF_IDISHWND;
    toolInfo.uId = (UINT_PTR)hAirtechCombo;
    toolInfo.lpszText = (LPSTR)"Auto-Airtech makes your character automatically recover when hit in the air.\r\nSelect a direction or Neutral to disable this feature.";
    SendMessage(hToolTip, TTM_ADDTOOL, 0, (LPARAM)&toolInfo);
    SendMessage(hToolTip, TTM_ACTIVATE, TRUE, 0);

    // Initialize edit fields with current values
    char buffer[32];
    
    // Player 1 values
    sprintf_s(buffer, "%d", pData->hp1);
    SetDlgItemTextA(hParent, IDC_HP1, buffer);
    
    sprintf_s(buffer, "%d", pData->meter1);
    SetDlgItemTextA(hParent, IDC_METER1, buffer);
    
    sprintf_s(buffer, "%d", (int)pData->rf1);
    SetDlgItemTextA(hParent, IDC_RF1, buffer);
    
    // Player 2 values (add these after P1 values)
    sprintf_s(buffer, "%d", pData->hp2);
    SetDlgItemTextA(hParent, IDC_HP2, buffer);
    
    sprintf_s(buffer, "%d", pData->meter2);
    SetDlgItemTextA(hParent, IDC_METER2, buffer);
    
    sprintf_s(buffer, "%d", (int)pData->rf2);
    SetDlgItemTextA(hParent, IDC_RF2, buffer);
    
    // Force C locale for coordinate string formatting to ensure consistent decimal points
    std::locale oldLocale = std::locale::global(std::locale("C"));

    sprintf_s(buffer, "%.1f", pData->x1);
    SetDlgItemTextA(hParent, IDC_X1, buffer);
    
    sprintf_s(buffer, "%.1f", pData->y1);
    SetDlgItemTextA(hParent, IDC_Y1, buffer);
    
    sprintf_s(buffer, "%.1f", pData->x2);
    SetDlgItemTextA(hParent, IDC_X2, buffer);
    
    sprintf_s(buffer, "%.1f", pData->y2);
    SetDlgItemTextA(hParent, IDC_Y2, buffer);

    // Restore original locale
    std::locale::global(oldLocale);
}

// Create all the controls for the Movement Options page
void MovementOptionsPage_CreateContent(HWND hParent, DisplayData* pData) {
    // Jump direction label
    CreateWindowEx(0, "STATIC", "Auto-Jump Direction:", 
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        50, 40, 150, 30, hParent, NULL, GetModuleHandle(NULL), NULL);
        
    // Create Jump Direction Combo Box
    HWND hJumpDirCombo = CreateWindowEx(0, "COMBOBOX", "", 
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
        240, 40, 250, 120, hParent, (HMENU)IDC_JUMP_DIRECTION, GetModuleHandle(NULL), NULL);
    
    // Add items to combo box
    SendMessage(hJumpDirCombo, CB_RESETCONTENT, 0, 0);
    SendMessage(hJumpDirCombo, CB_ADDSTRING, 0, (LPARAM)"Neutral (Disabled)");
    SendMessage(hJumpDirCombo, CB_ADDSTRING, 0, (LPARAM)"Straight (Up)");
    SendMessage(hJumpDirCombo, CB_ADDSTRING, 0, (LPARAM)"Forward");
    SendMessage(hJumpDirCombo, CB_ADDSTRING, 0, (LPARAM)"Backward");
    
    // Set selected item
    int jumpDirIndex = 0; // Default to Neutral
    if (pData->autoJump) {
        jumpDirIndex = pData->jumpDirection + 1; // +1 because index 0 is now "Neutral"
    }
    if (jumpDirIndex < 0 || jumpDirIndex > 3) jumpDirIndex = 0; // Validate index
    SendMessage(hJumpDirCombo, CB_SETCURSEL, jumpDirIndex, 0);
    
    // Jump target label
    CreateWindowEx(0, "STATIC", "Jump Target:", 
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        50, 100, 150, 30, hParent, NULL, GetModuleHandle(NULL), NULL);
    
    // Create Jump Target Combo Box
    HWND hTargetCombo = CreateWindowEx(0, "COMBOBOX", "", 
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
        240, 100, 250, 120, hParent, (HMENU)IDC_JUMP_TARGET, GetModuleHandle(NULL), NULL);
    
    // Add items to combo box
    SendMessage(hTargetCombo, CB_RESETCONTENT, 0, 0);
    SendMessage(hTargetCombo, CB_ADDSTRING, 0, (LPARAM)"P1 Only");
    SendMessage(hTargetCombo, CB_ADDSTRING, 0, (LPARAM)"P2 Only");
    SendMessage(hTargetCombo, CB_ADDSTRING, 0, (LPARAM)"Both Players");
    
    // Set selected item
    int targetIndex = pData->jumpTarget - 1; // Adjust index (jumpTarget is 1-based)
    if (targetIndex < 0 || targetIndex > 2) targetIndex = 2; // Default to "Both"
    SendMessage(hTargetCombo, CB_SETCURSEL, targetIndex, 0);
    
    // Create tooltip control
    HWND hToolTip = CreateWindowEx(0, TOOLTIPS_CLASS, NULL,
        WS_POPUP | TTS_ALWAYSTIP,
        CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
        hParent, NULL, GetModuleHandle(NULL), NULL);
        
    // Define tooltip structure
    TOOLINFO toolInfo = { 0 };
    toolInfo.cbSize = sizeof(toolInfo);
    toolInfo.hwnd = hParent;
    toolInfo.uFlags = TTF_SUBCLASS | TTF_IDISHWND;
    
    // Add tooltip for Jump Direction combo
    toolInfo.uId = (UINT_PTR)hJumpDirCombo;
    toolInfo.lpszText = (LPSTR)"Auto-Jump will automatically make the player jump whenever they get close to the opponent.\r\n\r\nThis is useful for practicing anti-air timing and defensive movement options.";
    SendMessage(hToolTip, TTM_ADDTOOL, 0, (LPARAM)&toolInfo);
    
    // Add tooltip for Jump Target combo
    toolInfo.uId = (UINT_PTR)hTargetCombo;
    toolInfo.lpszText = (LPSTR)"Select which player(s) will automatically jump when they approach the opponent.";
    SendMessage(hToolTip, TTM_ADDTOOL, 0, (LPARAM)&toolInfo);
    
    // Activate tooltips
    SendMessage(hToolTip, TTM_ACTIVATE, TRUE, 0);
}

// Add this subclass procedure to forward button messages to the main dialog
LRESULT CALLBACK PageSubclassProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR dwRefData) {
    HWND hDialogParent = (HWND)dwRefData;
    
    if (message == WM_COMMAND) {
        WORD cmdID = LOWORD(wParam);
        
        // Forward button clicks to main dialog
        if (cmdID == IDC_BTN_SWAP_POS || cmdID == IDC_BTN_ROUND_START) {
            LogOut("[GUI] Forwarding button click to main dialog: " + std::to_string(cmdID), true);
            SendMessage(hDialogParent, message, wParam, lParam);
            return 0; // We handled this message
        }
    }
    // Add this block to handle Enter key presses
    else if (message == WM_KEYDOWN && wParam == VK_RETURN) {
        // Forward Enter key to parent dialog as a click on the Confirm button
        LogOut("[GUI] Forwarding Enter key to Confirm button", detailedLogging);
        SendMessage(hDialogParent, WM_COMMAND, MAKEWPARAM(IDC_BTN_CONFIRM, BN_CLICKED), 
                   (LPARAM)GetDlgItem(hDialogParent, IDC_BTN_CONFIRM));
        return 0; // We handled this message
    }
    
    return DefSubclassProc(hWnd, message, wParam, lParam);
}
