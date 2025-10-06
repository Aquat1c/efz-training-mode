#include "../include/gui/gui.h"
#include "../include/core/constants.h"
#include "../include/utils/utilities.h"

#include <windows.h>
#include <commctrl.h>
#include "../include/core/logger.h" // Make sure logger is included

void GameValuesPage_CreateContent(HWND hParent, DisplayData* pData) {
    LogOut("[GUI_DEBUG] GameValuesPage_CreateContent: Called with hParent=" + Logger::hwndToString(hParent), true);
    // Get the size of the parent window for better scaling
    RECT rc;
    GetClientRect(hParent, &rc);
    int width = rc.right - rc.left;
    int height = rc.bottom - rc.top;

    // Layout constants
    const int margin = 12;
    const int groupPadding = 10;
    const int lineHeight = max(18, height / 24);
    const int labelWidth = max(60, width / 10);
    const int editHeight = lineHeight + 6;

    // Group boxes: Player 1 (left), Player 2 (right)
    int groupWidth = (width - (margin * 3)) / 2;
    int groupHeight = height * 5 / 10; // upper half for P1/P2

    HWND grpP1 = CreateWindowEx(0, "BUTTON", "Player 1", WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
        margin, margin, groupWidth, groupHeight, hParent, NULL, GetModuleHandle(NULL), NULL);
    HWND grpP2 = CreateWindowEx(0, "BUTTON", "Player 2", WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
        margin * 2 + groupWidth, margin, groupWidth, groupHeight, hParent, NULL, GetModuleHandle(NULL), NULL);

    auto addLabeledEdit = [&](HWND parent, int x, int y, const char* label, int id, const char* valueText, int editW) {
        CreateWindowEx(0, "STATIC", label, WS_CHILD | WS_VISIBLE | SS_LEFT,
            x, y, labelWidth, lineHeight, hParent, NULL, GetModuleHandle(NULL), NULL);
        CreateWindowEx(WS_EX_CLIENTEDGE, "EDIT", valueText, WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
            x + labelWidth + 6, y - 2, editW, editHeight, hParent, (HMENU)id, GetModuleHandle(NULL), NULL);
    };

    // P1 fields
    int p1x = margin + groupPadding;
    int p1y = margin + groupPadding + lineHeight; // leave room for group title
    int editWShort = max(80, groupWidth / 3);
    int editWLong = max(120, groupWidth / 2);
    char buf[32];
    sprintf_s(buf, "%d", pData->hp1);         addLabeledEdit(grpP1, p1x, p1y, "HP:", IDC_HP1, buf, editWShort); p1y += lineHeight + 8;
    sprintf_s(buf, "%d", pData->meter1);      addLabeledEdit(grpP1, p1x, p1y, "Meter:", IDC_METER1, buf, editWShort); p1y += lineHeight + 8;
    sprintf_s(buf, "%.1f", pData->rf1);       addLabeledEdit(grpP1, p1x, p1y, "RF:", IDC_RF1, buf, editWShort); p1y += lineHeight + 8;
    sprintf_s(buf, "%.2f", pData->x1);        addLabeledEdit(grpP1, p1x, p1y, "X:", IDC_X1, buf, editWLong); p1y += lineHeight + 8;
    sprintf_s(buf, "%.2f", pData->y1);        addLabeledEdit(grpP1, p1x, p1y, "Y:", IDC_Y1, buf, editWLong); p1y += lineHeight + 8;

    // P2 fields
    int p2x = margin * 2 + groupWidth + groupPadding;
    int p2y = margin + groupPadding + lineHeight;
    sprintf_s(buf, "%d", pData->hp2);         addLabeledEdit(grpP2, p2x, p2y, "HP:", IDC_HP2, buf, editWShort); p2y += lineHeight + 8;
    sprintf_s(buf, "%d", pData->meter2);      addLabeledEdit(grpP2, p2x, p2y, "Meter:", IDC_METER2, buf, editWShort); p2y += lineHeight + 8;
    sprintf_s(buf, "%.1f", pData->rf2);       addLabeledEdit(grpP2, p2x, p2y, "RF:", IDC_RF2, buf, editWShort); p2y += lineHeight + 8;
    sprintf_s(buf, "%.2f", pData->x2);        addLabeledEdit(grpP2, p2x, p2y, "X:", IDC_X2, buf, editWLong); p2y += lineHeight + 8;
    sprintf_s(buf, "%.2f", pData->y2);        addLabeledEdit(grpP2, p2x, p2y, "Y:", IDC_Y2, buf, editWLong); p2y += lineHeight + 8;

    // Automation group (bottom)
    int autoY = margin * 2 + groupHeight;
    int autoH = height - autoY - margin;
    HWND grpAuto = CreateWindowEx(0, "BUTTON", "Automation", WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
        margin, autoY, width - margin * 2, autoH, hParent, NULL, GetModuleHandle(NULL), NULL);

    int ax = margin + groupPadding;
    int ay = autoY + groupPadding + lineHeight; // leave space for title

    // Auto-Airtech direction
    CreateWindowEx(0, "STATIC", "Auto-Airtech Direction:", WS_CHILD | WS_VISIBLE | SS_LEFT,
        ax, ay, labelWidth + 120, lineHeight, hParent, NULL, GetModuleHandle(NULL), NULL);
    HWND hAirtechCombo = CreateWindowEx(0, "COMBOBOX", "", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
        ax + labelWidth + 130, ay - 2, max(180, width / 4), 120, hParent, (HMENU)IDC_AIRTECH_DIRECTION, GetModuleHandle(NULL), NULL);

    SendMessage(hAirtechCombo, CB_RESETCONTENT, 0, 0);
    SendMessage(hAirtechCombo, CB_ADDSTRING, 0, (LPARAM)"Neutral (Disabled)");
    SendMessage(hAirtechCombo, CB_ADDSTRING, 0, (LPARAM)"Forward");
    SendMessage(hAirtechCombo, CB_ADDSTRING, 0, (LPARAM)"Backward");

    int airtechIndex = 0;
    if (pData->autoAirtech) airtechIndex = pData->airtechDirection + 1;
    if (airtechIndex < 0 || airtechIndex > 2) airtechIndex = 0;
    SendMessage(hAirtechCombo, CB_SETCURSEL, airtechIndex, 0);

    // Auto-Airtech delay
    ay += lineHeight + 10;
    CreateWindowEx(0, "STATIC", "Delay:", WS_CHILD | WS_VISIBLE | SS_LEFT,
        ax, ay, labelWidth, lineHeight, hParent, NULL, GetModuleHandle(NULL), NULL);
    char airtechDelayText[8];
    sprintf_s(airtechDelayText, sizeof(airtechDelayText), "%d", autoAirtechDelay.load());
    CreateWindowEx(WS_EX_CLIENTEDGE, "EDIT", airtechDelayText, WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NUMBER,
        ax + labelWidth + 6, ay - 2, max(80, width / 12), editHeight, hParent, (HMENU)IDC_AIRTECH_DELAY, GetModuleHandle(NULL), NULL);
    CreateWindowEx(0, "STATIC", "frames", WS_CHILD | WS_VISIBLE | SS_LEFT,
        ax + labelWidth + 6 + max(80, width / 12) + 6, ay, 60, lineHeight, hParent, NULL, GetModuleHandle(NULL), NULL);

    // Tooltip for Auto-Airtech
    HWND hToolTip = CreateWindowEx(0, TOOLTIPS_CLASS, NULL, WS_POPUP | TTS_ALWAYSTIP,
        CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, hParent, NULL, GetModuleHandle(NULL), NULL);
    TOOLINFO toolInfo = { 0 };
    toolInfo.cbSize = sizeof(toolInfo);
    toolInfo.hwnd = hParent;
    toolInfo.uFlags = TTF_SUBCLASS | TTF_IDISHWND;
    toolInfo.uId = (UINT_PTR)hAirtechCombo;
    toolInfo.lpszText = (LPSTR)"Auto-Airtech makes your character automatically recover when hit in the air.\r\nSelect a direction or Neutral to disable this feature.\r\nDelay: 0=instant, 1+=frames to wait after becoming airtech-capable.";
    SendMessage(hToolTip, TTM_ADDTOOL, 0, (LPARAM)&toolInfo);

}