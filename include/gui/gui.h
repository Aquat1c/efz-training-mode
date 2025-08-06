#pragma once
#include <windows.h>
#include "../utils/utilities.h"

void OpenMenu();
void ShowEditDataDialog(HWND hParent);
INT_PTR CALLBACK EditDataDlgProc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam);
void ApplySettings(DisplayData* data);

// Page content creation functions
void GameValuesPage_CreateContent(HWND hParent, DisplayData* pData);
void AutoActionPage_CreateContent(HWND hParent, DisplayData* pData);

// Helper functions
void ProcessFormData(HWND hDlg, HWND hPage1, HWND hPage3, DisplayData* pData);
LRESULT CALLBACK PageSubclassProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR dwRefData);