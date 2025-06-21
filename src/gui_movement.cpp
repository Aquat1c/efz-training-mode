#include "../include/gui.h"
#include "../include/constants.h"
#include "../include/utilities.h"
#include "../include/logger.h"
#include <windows.h>
#include <commctrl.h>

void MovementOptionsPage_CreateContent(HWND hParent, DisplayData* pData) {
    // Get the size of the parent window
    RECT rc;
    GetClientRect(hParent, &rc);
    int width = rc.right - rc.left;
    int height = rc.bottom - rc.top;

    // Auto-Jump section
    CreateWindowEx(0, "STATIC", "Auto-Jump Settings:", 
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        width/20, height/10, width/3, height/20, 
        hParent, NULL, GetModuleHandle(NULL), NULL);

    // Jump direction label and combo
    CreateWindowEx(0, "STATIC", "Jump Direction:", 
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        width/20, height*2/10, width/5, height/20, 
        hParent, NULL, GetModuleHandle(NULL), NULL);

    HWND hJumpDirCombo = CreateWindowEx(0, "COMBOBOX", "", 
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
        width/3, height*2/10, width*2/5, 120, 
        hParent, (HMENU)IDC_JUMP_DIRECTION, GetModuleHandle(NULL), NULL);
    
    // Add items to jump direction combo
    SendMessage(hJumpDirCombo, CB_RESETCONTENT, 0, 0);
    SendMessage(hJumpDirCombo, CB_ADDSTRING, 0, (LPARAM)"Disabled");         // Index 0
    SendMessage(hJumpDirCombo, CB_ADDSTRING, 0, (LPARAM)"Straight Jump");    // Index 1
    SendMessage(hJumpDirCombo, CB_ADDSTRING, 0, (LPARAM)"Forward Jump");     // Index 2
    SendMessage(hJumpDirCombo, CB_ADDSTRING, 0, (LPARAM)"Backward Jump");    // Index 3

    // CRITICAL FIX: Set selected item based on current atomic variables, not DisplayData
    int jumpDirIndex = 0;  // Default to disabled
    if (autoJumpEnabled.load()) {
        jumpDirIndex = jumpDirection.load() + 1;  // Convert from 0-based to 1-based for combo
    }
    if (jumpDirIndex < 0 || jumpDirIndex > 3) jumpDirIndex = 0;
    SendMessage(hJumpDirCombo, CB_SETCURSEL, jumpDirIndex, 0);

    // Jump target label and combo
    CreateWindowEx(0, "STATIC", "Apply To:", 
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        width/20, height*3/10, width/5, height/20, 
        hParent, NULL, GetModuleHandle(NULL), NULL);

    HWND hJumpTargetCombo = CreateWindowEx(0, "COMBOBOX", "", 
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
        width/3, height*3/10, width*2/5, 120, 
        hParent, (HMENU)IDC_JUMP_TARGET, GetModuleHandle(NULL), NULL);
    
    // Add items to jump target combo
    SendMessage(hJumpTargetCombo, CB_RESETCONTENT, 0, 0);
    SendMessage(hJumpTargetCombo, CB_ADDSTRING, 0, (LPARAM)"P1 Only");      // Index 0 = jumpTarget 1
    SendMessage(hJumpTargetCombo, CB_ADDSTRING, 0, (LPARAM)"P2 Only");      // Index 1 = jumpTarget 2
    SendMessage(hJumpTargetCombo, CB_ADDSTRING, 0, (LPARAM)"Both Players"); // Index 2 = jumpTarget 3

    // CRITICAL FIX: Set selected item based on current atomic variables
    int jumpTargetIndex = jumpTarget.load() - 1;  // Convert from 1-based to 0-based for combo
    if (jumpTargetIndex < 0 || jumpTargetIndex > 2) jumpTargetIndex = 2;  // Default to both
    SendMessage(hJumpTargetCombo, CB_SETCURSEL, jumpTargetIndex, 0);

    // Add tooltips
    HWND hToolTip = CreateWindowEx(0, TOOLTIPS_CLASS, NULL,
        WS_POPUP | TTS_ALWAYSTIP,
        CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
        hParent, NULL, GetModuleHandle(NULL), NULL);
        
    TOOLINFO toolInfo = { 0 };
    toolInfo.cbSize = sizeof(toolInfo);
    toolInfo.hwnd = hParent;
    toolInfo.uFlags = TTF_SUBCLASS | TTF_IDISHWND;
    
    // Jump direction tooltip
    toolInfo.uId = (UINT_PTR)hJumpDirCombo;
    toolInfo.lpszText = (LPSTR)"Auto-Jump makes the selected player(s) automatically jump when they land.\r\nSelect Disabled to turn off this feature.";
    SendMessage(hToolTip, TTM_ADDTOOL, 0, (LPARAM)&toolInfo);
    
    // Jump target tooltip
    toolInfo.uId = (UINT_PTR)hJumpTargetCombo;
    toolInfo.lpszText = (LPSTR)"Choose which player(s) will automatically jump when landing.";
    SendMessage(hToolTip, TTM_ADDTOOL, 0, (LPARAM)&toolInfo);
    
    // Debug info to track what's happening
    LogOut("[GUI] Auto-Jump GUI created - Atomic values: enabled=" + 
           std::to_string(autoJumpEnabled.load()) + 
           ", direction=" + std::to_string(jumpDirection.load()) + 
           ", target=" + std::to_string(jumpTarget.load()) +
           " | GUI selections: dirIndex=" + std::to_string(jumpDirIndex) +
           ", targetIndex=" + std::to_string(jumpTargetIndex), true);
}