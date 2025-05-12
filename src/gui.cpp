#include "../include/gui.h"
#include "../include/constants.h"
#include "../include/memory.h"
#include "../include/utilities.h"
#include "../include/logger.h"
#include <windows.h>
#include <string>
#include <thread>

void OpenMenu() {
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
        uintptr_t hpAddr1 = ResolvePointer(base, EFZ_BASE_OFFSET_P1, HP_OFFSET);
        uintptr_t meterAddr1 = ResolvePointer(base, EFZ_BASE_OFFSET_P1, METER_OFFSET);
        uintptr_t rfAddr1 = ResolvePointer(base, EFZ_BASE_OFFSET_P1, RF_OFFSET);
        uintptr_t xAddr1 = ResolvePointer(base, EFZ_BASE_OFFSET_P1, XPOS_OFFSET);
        uintptr_t yAddr1 = ResolvePointer(base, EFZ_BASE_OFFSET_P1, YPOS_OFFSET);
        
        uintptr_t hpAddr2 = ResolvePointer(base, EFZ_BASE_OFFSET_P2, HP_OFFSET);
        uintptr_t meterAddr2 = ResolvePointer(base, EFZ_BASE_OFFSET_P2, METER_OFFSET);
        uintptr_t rfAddr2 = ResolvePointer(base, EFZ_BASE_OFFSET_P2, RF_OFFSET);
        uintptr_t xAddr2 = ResolvePointer(base, EFZ_BASE_OFFSET_P2, XPOS_OFFSET);
        uintptr_t yAddr2 = ResolvePointer(base, EFZ_BASE_OFFSET_P2, YPOS_OFFSET);

        // Read current values from memory
        if (hpAddr1) memcpy(&displayData.hp1, (void*)hpAddr1, sizeof(WORD));
        if (meterAddr1) memcpy(&displayData.meter1, (void*)meterAddr1, sizeof(WORD));
        if (rfAddr1) {
            // Read RF value as a double with error checking
            double rf = 0.0;
            memcpy(&rf, (void*)rfAddr1, sizeof(double));
            
            // Validate RF value (should be between 0 and 1000)
            if (rf >= 0.0 && rf <= MAX_RF) {
                displayData.rf1 = rf;
            } else {
                // If value is out of valid range, set to a reasonable default
                displayData.rf1 = 0.0;
            }
        }
        if (xAddr1) memcpy(&displayData.x1, (void*)xAddr1, sizeof(double));
        if (yAddr1) memcpy(&displayData.y1, (void*)yAddr1, sizeof(double));
        
        if (hpAddr2) memcpy(&displayData.hp2, (void*)hpAddr2, sizeof(WORD));
        if (meterAddr2) memcpy(&displayData.meter2, (void*)meterAddr2, sizeof(WORD));
        if (rfAddr2) {
            double rf = 0.0;
            memcpy(&rf, (void*)rfAddr2, sizeof(double));
            
            if (rf >= 0.0 && rf <= MAX_RF) {
                displayData.rf2 = rf;
            } else {
                displayData.rf2 = 0.0;
            }
        }
        if (xAddr2) memcpy(&displayData.x2, (void*)xAddr2, sizeof(double));
        if (yAddr2) memcpy(&displayData.y2, (void*)yAddr2, sizeof(double));

        // Load auto-airtech settings
        displayData.autoAirtech = autoAirtechEnabled.load();
        displayData.airtechDirection = autoAirtechDirection.load();

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

INT_PTR CALLBACK EditDataDlgProc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam) {
    static DisplayData* pData = nullptr;
    // Round start positions
    const double p1StartX = 240.0, p2StartX = 400.0, startY = 0.0;

    switch (message) {
    case WM_INITDIALOG:
        pData = (DisplayData*)lParam;

        // Initialize all form fields with current values
        SetDlgItemInt(hDlg, IDC_HP1, CLAMP((int)pData->hp1, 0, MAX_HP), FALSE);
        SetDlgItemInt(hDlg, IDC_METER1, CLAMP((int)pData->meter1, 0, MAX_METER), FALSE);
        SetDlgItemInt(hDlg, IDC_RF1, (UINT)CLAMP((int)pData->rf1, 0, MAX_RF), FALSE);
        SetDlgItemTextA(hDlg, IDC_X1, std::to_string(pData->x1).c_str());
        SetDlgItemTextA(hDlg, IDC_Y1, std::to_string(pData->y1).c_str());

        SetDlgItemInt(hDlg, IDC_HP2, CLAMP((int)pData->hp2, 0, MAX_HP), FALSE);
        SetDlgItemInt(hDlg, IDC_METER2, CLAMP((int)pData->meter2, 0, MAX_METER), FALSE);
        SetDlgItemInt(hDlg, IDC_RF2, (UINT)CLAMP((int)pData->rf2, 0, MAX_RF), FALSE);
        SetDlgItemTextA(hDlg, IDC_X2, std::to_string(pData->x2).c_str());
        SetDlgItemTextA(hDlg, IDC_Y2, std::to_string(pData->y2).c_str());

        // Initialize auto-airtech controls
        CheckDlgButton(hDlg, IDC_AUTO_AIRTECH_CHECK, pData->autoAirtech ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hDlg, pData->airtechDirection == 0 ? IDC_AIRTECH_FORWARD : IDC_AIRTECH_BACKWARD, BST_CHECKED);

        // Enable/disable the radio buttons based on checkbox state
        EnableWindow(GetDlgItem(hDlg, IDC_AIRTECH_FORWARD), pData->autoAirtech);
        EnableWindow(GetDlgItem(hDlg, IDC_AIRTECH_BACKWARD), pData->autoAirtech);

        return TRUE;

    case WM_COMMAND:
        // Handle Confirm button (same as IDOK)
        if (LOWORD(wParam) == IDC_BTN_CONFIRM || LOWORD(wParam) == IDOK) {
            // Get values from dialog
            BOOL success;
            pData->hp1 = GetDlgItemInt(hDlg, IDC_HP1, &success, FALSE);
            pData->meter1 = GetDlgItemInt(hDlg, IDC_METER1, &success, FALSE);
            pData->rf1 = GetDlgItemInt(hDlg, IDC_RF1, &success, FALSE);
            
            char buffer[32];
            GetDlgItemTextA(hDlg, IDC_X1, buffer, sizeof(buffer));
            pData->x1 = std::stod(buffer);
            GetDlgItemTextA(hDlg, IDC_Y1, buffer, sizeof(buffer));
            pData->y1 = std::stod(buffer);
            
            pData->hp2 = GetDlgItemInt(hDlg, IDC_HP2, &success, FALSE);
            pData->meter2 = GetDlgItemInt(hDlg, IDC_METER2, &success, FALSE);
            pData->rf2 = GetDlgItemInt(hDlg, IDC_RF2, &success, FALSE);
            
            GetDlgItemTextA(hDlg, IDC_X2, buffer, sizeof(buffer));
            pData->x2 = std::stod(buffer);
            GetDlgItemTextA(hDlg, IDC_Y2, buffer, sizeof(buffer));
            pData->y2 = std::stod(buffer);
            
            // Get auto-airtech settings
            pData->autoAirtech = (IsDlgButtonChecked(hDlg, IDC_AUTO_AIRTECH_CHECK) == BST_CHECKED);
            pData->airtechDirection = (IsDlgButtonChecked(hDlg, IDC_AIRTECH_FORWARD) == BST_CHECKED) ? 0 : 1;

            // After dialog values are saved, update the atomic variables
            autoAirtechEnabled = pData->autoAirtech;
            autoAirtechDirection = pData->airtechDirection;

            // Apply values to game
            UpdatePlayerValues(GetEFZBase(), EFZ_BASE_OFFSET_P1, EFZ_BASE_OFFSET_P2);
            
            EndDialog(hDlg, IDOK);
            return TRUE;
        }

        // Handle Cancel button
        if (LOWORD(wParam) == IDC_BTN_CANCEL || LOWORD(wParam) == IDCANCEL) {
            EndDialog(hDlg, IDCANCEL);
            return TRUE;
        }

        // Handle Swap Positions button
        if (LOWORD(wParam) == IDC_BTN_SWAP_POS) {
            char x1[32], y1[32], x2[32], y2[32];
            GetDlgItemTextA(hDlg, IDC_X1, x1, sizeof(x1));
            GetDlgItemTextA(hDlg, IDC_Y1, y1, sizeof(y1));
            GetDlgItemTextA(hDlg, IDC_X2, x2, sizeof(x2));
            GetDlgItemTextA(hDlg, IDC_Y2, y2, sizeof(y2));
            
            // Swap the position values
            SetDlgItemTextA(hDlg, IDC_X1, x2);
            SetDlgItemTextA(hDlg, IDC_Y1, y2);
            SetDlgItemTextA(hDlg, IDC_X2, x1);
            SetDlgItemTextA(hDlg, IDC_Y2, y1);
            return TRUE;
        }

        // Handle Round Start button
        if (LOWORD(wParam) == IDC_BTN_ROUND_START) {
            // Set round start positions
            SetDlgItemTextA(hDlg, IDC_X1, std::to_string(p1StartX).c_str());
            SetDlgItemTextA(hDlg, IDC_Y1, std::to_string(startY).c_str());
            SetDlgItemTextA(hDlg, IDC_X2, std::to_string(p2StartX).c_str());
            SetDlgItemTextA(hDlg, IDC_Y2, std::to_string(startY).c_str());
            return TRUE;
        }

        // Handle checkbox state changes
        if (LOWORD(wParam) == IDC_AUTO_AIRTECH_CHECK) {
            bool checked = (IsDlgButtonChecked(hDlg, IDC_AUTO_AIRTECH_CHECK) == BST_CHECKED);
            EnableWindow(GetDlgItem(hDlg, IDC_AIRTECH_FORWARD), checked);
            EnableWindow(GetDlgItem(hDlg, IDC_AIRTECH_BACKWARD), checked);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

void ShowEditDataDialog(HWND hParent) {
    static WORD dlgTemplate[1024]; // Increased size for additional controls
    ZeroMemory(dlgTemplate, sizeof(dlgTemplate));
    DLGTEMPLATE* dlg = (DLGTEMPLATE*)dlgTemplate;
    dlg->style = DS_MODALFRAME | WS_POPUP | WS_CAPTION | WS_SYSMENU;
    dlg->cdit = 29;  // Total number of controls (10 fields + 10 labels + 4 buttons + 5 new controls)
    dlg->x = 10; dlg->y = 10;
    dlg->cx = 300; // Wider dialog to ensure all elements fit
    dlg->cy = 290;  // Give more space for all controls including the bottom buttons
    WORD* p = (WORD*)(dlg + 1);
    *p++ = 0; // no menu
    *p++ = 0; // default dialog class
    
    // Set dialog title to "Config menu"
    const wchar_t* title = L"Config menu";
    for (int i = 0; title[i]; i++) {
        *p++ = title[i];
    }
    *p++ = 0; // null terminator

    // Define all field positions explicitly
    struct { LPCSTR label; int x; int y; int id; int width; } fields[] = {
        {"P1 HP:", 10, 10, IDC_HP1, 50},
        {"P1 Meter:", 10, 35, IDC_METER1, 50},
        {"P1 RF:", 10, 60, IDC_RF1, 50},
        {"P1 X:", 10, 85, IDC_X1, 80},
        {"P1 Y:", 10, 110, IDC_Y1, 80},
        {"P2 HP:", 160, 10, IDC_HP2, 50},
        {"P2 Meter:", 160, 35, IDC_METER2, 50},
        {"P2 RF:", 160, 60, IDC_RF2, 50},
        {"P2 X:", 160, 85, IDC_X2, 80},
        {"P2 Y:", 160, 110, IDC_Y2, 80}
    };

    DLGITEMTEMPLATE* item = (DLGITEMTEMPLATE*)(((DWORD_PTR)p + 3) & ~3);

    // Create all fields with their labels
    for (int i = 0; i < 10; ++i) {
        // Label control
        item->style = WS_CHILD | WS_VISIBLE;
        item->x = fields[i].x;
        item->y = fields[i].y;
        item->cx = 55;
        item->cy = 14;
        item->id = 0; // Labels don't need IDs
        item->dwExtendedStyle = 0;
        WORD* pi = (WORD*)(item + 1);
        *pi++ = 0xFFFF;
        *pi++ = 0x0082; // Static class
        LPCSTR label = fields[i].label;
        while (*label) *pi++ = *label++;
        *pi++ = 0;
        *pi++ = 0; // no creation data
        item = (DLGITEMTEMPLATE*)(((DWORD_PTR)pi + 3) & ~3);

        // Edit control
        DWORD style = WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL;
        if (fields[i].id != IDC_X1 && fields[i].id != IDC_Y1 &&
            fields[i].id != IDC_X2 && fields[i].id != IDC_Y2) {
            style |= ES_NUMBER;
        }

        item->style = style;
        item->x = fields[i].x + 55;
        item->y = fields[i].y;
        item->cx = fields[i].width;
        item->cy = 14;
        item->id = fields[i].id;
        item->dwExtendedStyle = 0;
        pi = (WORD*)(item + 1);
        *pi++ = 0xFFFF;
        *pi++ = 0x0081; // Edit class
        *pi++ = 0; // no text
        *pi++ = 0; // no creation data
        item = (DLGITEMTEMPLATE*)(((DWORD_PTR)pi + 3) & ~3);
    }

    // Swap Positions button
    item->style = WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON;
    item->x = 35; item->y = 140; item->cx = 100; item->cy = 20;
    item->id = IDC_BTN_SWAP_POS;
    item->dwExtendedStyle = 0;
    WORD* pi = (WORD*)(item + 1);
    *pi++ = 0xFFFF; *pi++ = 0x0080; // Button class
    const char* swapPosText = "Swap Positions";
    while (*swapPosText) *pi++ = *swapPosText++;
    *pi++ = 0;
    *pi++ = 0; // no creation data
    item = (DLGITEMTEMPLATE*)(((DWORD_PTR)pi + 3) & ~3);

    // Round Start Positions button
    item->style = WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON;
    item->x = 160; item->y = 140; item->cx = 100; item->cy = 20;
    item->id = IDC_BTN_ROUND_START;
    item->dwExtendedStyle = 0;
    pi = (WORD*)(item + 1);
    *pi++ = 0xFFFF; *pi++ = 0x0080; // Button class
    const char* roundStartText = "Round Start";
    while (*roundStartText) *pi++ = *roundStartText++;
    *pi++ = 0;
    *pi++ = 0; // no creation data
    item = (DLGITEMTEMPLATE*)(((DWORD_PTR)pi + 3) & ~3);

    // Separator line
    item->style = WS_CHILD | WS_VISIBLE | SS_ETCHEDHORZ;
    item->x = 10; item->y = 180; item->cx = 280; item->cy = 2;
    item->id = IDC_STATIC;
    item->dwExtendedStyle = 0;
    pi = (WORD*)(item + 1);
    *pi++ = 0xFFFF; *pi++ = 0x0082;  // Static class
    *pi++ = 0; // No text
    *pi++ = 0; // No creation data
    item = (DLGITEMTEMPLATE*)(((DWORD_PTR)pi + 3) & ~3);

    // Auto-airtech Settings Label
    item->style = WS_CHILD | WS_VISIBLE | SS_LEFT;
    item->x = 10; item->y = 190; item->cx = 280; item->cy = 16;
    item->id = IDC_STATIC;
    item->dwExtendedStyle = 0;
    pi = (WORD*)(item + 1);
    *pi++ = 0xFFFF; *pi++ = 0x0082;  // Static class
    const char* airtechLabel = "Auto-Airtech Settings:";
    while (*airtechLabel) *pi++ = *airtechLabel++;
    *pi++ = 0;
    *pi++ = 0; // No creation data
    item = (DLGITEMTEMPLATE*)(((DWORD_PTR)pi + 3) & ~3);

    // Auto-Airtech Checkbox
    item->style = WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX;
    item->x = 20; item->y = 210; item->cx = 200; item->cy = 20;
    item->id = IDC_AUTO_AIRTECH_CHECK;
    item->dwExtendedStyle = 0;
    pi = (WORD*)(item + 1);
    *pi++ = 0xFFFF; *pi++ = 0x0080;  // Button class
    const char* airtechCheckText = "Enable Auto-Airtech";
    while (*airtechCheckText) *pi++ = *airtechCheckText++;
    *pi++ = 0;
    *pi++ = 0; // No creation data
    item = (DLGITEMTEMPLATE*)(((DWORD_PTR)pi + 3) & ~3);

    // Forward Airtech Radio Button
    item->style = WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON;
    item->x = 35; item->y = 235; item->cx = 100; item->cy = 20;
    item->id = IDC_AIRTECH_FORWARD;
    item->dwExtendedStyle = 0;
    pi = (WORD*)(item + 1);
    *pi++ = 0xFFFF; *pi++ = 0x0080;  // Button class
    const char* forwardText = "Forward";
    while (*forwardText) *pi++ = *forwardText++;
    *pi++ = 0;
    *pi++ = 0; // No creation data
    item = (DLGITEMTEMPLATE*)(((DWORD_PTR)pi + 3) & ~3);

    // Backward Airtech Radio Button
    item->style = WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON;
    item->x = 150; item->y = 235; item->cx = 100; item->cy = 20;
    item->id = IDC_AIRTECH_BACKWARD;
    item->dwExtendedStyle = 0;
    pi = (WORD*)(item + 1);
    *pi++ = 0xFFFF; *pi++ = 0x0080;  // Button class
    const char* backwardText = "Backward";
    while (*backwardText) *pi++ = *backwardText++;
    *pi++ = 0;
    *pi++ = 0; // No creation data
    item = (DLGITEMTEMPLATE*)(((DWORD_PTR)pi + 3) & ~3);

    // Confirm button
    item->style = WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON;
    item->x = 60; item->y = 265; item->cx = 60; item->cy = 20;
    item->id = IDC_BTN_CONFIRM;
    item->dwExtendedStyle = 0;
    pi = (WORD*)(item + 1);
    *pi++ = 0xFFFF; *pi++ = 0x0080; // Button class
    const char* confirmText = "Confirm";
    while (*confirmText) *pi++ = *confirmText++;
    *pi++ = 0;
    *pi++ = 0; // no creation data
    item = (DLGITEMTEMPLATE*)(((DWORD_PTR)pi + 3) & ~3);

    // Cancel button
    item->style = WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON;
    item->x = 160; item->y = 265; item->cx = 60; item->cy = 20;
    item->id = IDC_BTN_CANCEL;
    item->dwExtendedStyle = 0;
    pi = (WORD*)(item + 1);
    *pi++ = 0xFFFF; *pi++ = 0x0080; // Button class
    const char* cancelText = "Cancel";
    while (*cancelText) *pi++ = *cancelText++;
    *pi++ = 0;
    *pi++ = 0; // no creation data

    // Create and show the dialog
    DialogBoxIndirectParamA(GetModuleHandle(NULL), (LPCDLGTEMPLATEA)dlgTemplate, hParent, EditDataDlgProc, (LPARAM)&displayData);
}