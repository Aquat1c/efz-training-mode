#pragma once
#include <windows.h>
#include "utilities.h"

void OpenMenu();
void ShowEditDataDialog(HWND hParent);
INT_PTR CALLBACK EditDataDlgProc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam);