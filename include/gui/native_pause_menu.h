#pragma once
#include <windows.h>
#include <d3d9.h>
#include <atomic>
namespace NativePauseMenu {
    void Initialize();
    void Toggle();
    void Show();
    void Hide();
    bool IsVisible();
    void TickInput(); // ESC close (F1 handled globally)
    void RenderD3D9(LPDIRECT3DDEVICE9 device); // Raw quad rendering
}
