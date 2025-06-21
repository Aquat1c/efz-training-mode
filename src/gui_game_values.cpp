#include "../include/gui.h"
#include "../include/constants.h"
#include "../include/utilities.h"
#include <windows.h>
#include <commctrl.h>
#include "../include/logger.h" // Make sure logger is included

void GameValuesPage_CreateContent(HWND hParent, DisplayData* pData) {
    LogOut("[GUI_DEBUG] GameValuesPage_CreateContent: Called with hParent=" + Logger::hwndToString(hParent), true);
    // Get the size of the parent window for better scaling
    RECT rc;
    GetClientRect(hParent, &rc);
    int width = rc.right - rc.left;
    int height = rc.bottom - rc.top;
    
    // Calculate better positioned fields based on available space
    struct FieldInfo {
        const char* label;
        int x, y, id, editWidth;
    } fields[] = {
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
        // Create label
        CreateWindowEx(0, "STATIC", fields[i].label, 
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            fields[i].x, fields[i].y, width/7, height/20, 
            hParent, NULL, GetModuleHandle(NULL), NULL);
        
        // Create edit control
        char valueText[16];
        if (i == 0) sprintf_s(valueText, "%d", pData->hp1);
        else if (i == 1) sprintf_s(valueText, "%d", pData->meter1);
        else if (i == 2) sprintf_s(valueText, "%.1f", pData->rf1);
        else if (i == 3) sprintf_s(valueText, "%.2f", pData->x1);
        else if (i == 4) sprintf_s(valueText, "%.2f", pData->y1);
        else if (i == 5) sprintf_s(valueText, "%d", pData->hp2);
        else if (i == 6) sprintf_s(valueText, "%d", pData->meter2);
        else if (i == 7) sprintf_s(valueText, "%.1f", pData->rf2);
        else if (i == 8) sprintf_s(valueText, "%.2f", pData->x2);
        else if (i == 9) sprintf_s(valueText, "%.2f", pData->y2);
        
        CreateWindowEx(WS_EX_CLIENTEDGE, "EDIT", valueText, 
            WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
            fields[i].x + width/6, fields[i].y, fields[i].editWidth, height/20, 
            hParent, (HMENU)fields[i].id, GetModuleHandle(NULL), NULL);
    }

    // Buttons with better positioning
    HWND btnSwapPos = CreateWindowEx(0, "BUTTON", "Swap Positions", 
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        width/5, height*6/10, width/4, height/15, 
        hParent, (HMENU)IDC_BTN_SWAP_POS, GetModuleHandle(NULL), NULL);
    LogOut("[GUI_DEBUG] GameValuesPage_CreateContent: Created IDC_BTN_SWAP_POS with handle=" + Logger::hwndToString(btnSwapPos) + " on parent=" + Logger::hwndToString(hParent), true);
    
    HWND btnRoundStart = CreateWindowEx(0, "BUTTON", "Round Start", 
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        width*3/5, height*6/10, width/4, height/15, 
        hParent, (HMENU)IDC_BTN_ROUND_START, GetModuleHandle(NULL), NULL);
    LogOut("[GUI_DEBUG] GameValuesPage_CreateContent: Created IDC_BTN_ROUND_START with handle=" + Logger::hwndToString(btnRoundStart) + " on parent=" + Logger::hwndToString(hParent), true);

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
        width*4/10, height*8/10, width*3/10, 120,
        hParent, (HMENU)IDC_AIRTECH_DIRECTION, GetModuleHandle(NULL), NULL);
    
    // Add items to the airtech combo box
    SendMessage(hAirtechCombo, CB_RESETCONTENT, 0, 0);
    SendMessage(hAirtechCombo, CB_ADDSTRING, 0, (LPARAM)"Neutral (Disabled)");
    SendMessage(hAirtechCombo, CB_ADDSTRING, 0, (LPARAM)"Forward");
    SendMessage(hAirtechCombo, CB_ADDSTRING, 0, (LPARAM)"Backward");

    // Set selected item based on current settings
    int airtechIndex = 0;
    if (pData->autoAirtech) {
        airtechIndex = pData->airtechDirection + 1;
    }
    if (airtechIndex < 0 || airtechIndex > 2) airtechIndex = 0;
    SendMessage(hAirtechCombo, CB_SETCURSEL, airtechIndex, 0);

    // NEW: Add airtech delay field
    CreateWindowEx(0, "STATIC", "Delay:", 
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        width*7/10, height*8/10, width/10, height/20, 
        hParent, NULL, GetModuleHandle(NULL), NULL);

    char airtechDelayText[8];
    sprintf_s(airtechDelayText, sizeof(airtechDelayText), "%d", autoAirtechDelay.load());
    CreateWindowEx(WS_EX_CLIENTEDGE, "EDIT", airtechDelayText, 
        WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NUMBER,
        width*8/10, height*8/10, width/10, height/20, 
        hParent, (HMENU)IDC_AIRTECH_DELAY, GetModuleHandle(NULL), NULL);

    CreateWindowEx(0, "STATIC", "frames", 
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        width*9/10, height*8/10, width/10, height/20, 
        hParent, NULL, GetModuleHandle(NULL), NULL);

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
    toolInfo.lpszText = (LPSTR)"Auto-Airtech makes your character automatically recover when hit in the air.\r\nSelect a direction or Neutral to disable this feature.\r\nDelay: 0=instant, 1+=frames to wait after becoming airtech-capable.";
    SendMessage(hToolTip, TTM_ADDTOOL, 0, (LPARAM)&toolInfo);
}