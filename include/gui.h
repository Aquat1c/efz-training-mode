#pragma once
#include <windows.h>
#include "utilities.h"

void OpenMenu();
void ShowEditDataDialog(HWND hParent);
INT_PTR CALLBACK EditDataDlgProc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam);
void ApplySettings(DisplayData* data);

// New helper functions
void GameValuesPage_CreateContent(HWND hParent, DisplayData* pData);
void MovementOptionsPage_CreateContent(HWND hParent, DisplayData* pData);