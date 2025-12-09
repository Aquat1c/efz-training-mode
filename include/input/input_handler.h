#pragma once
#define DIRECTINPUT_VERSION 0x0800 // Define before including dinput.h
#include <windows.h>
#include <atomic>
#include <thread>
#include <dinput.h>
#include <vector>
#include <string>
#include <sstream>
// DirectInput global objects
extern LPDIRECTINPUT8 g_pDI;
extern LPDIRECTINPUTDEVICE8 g_pKeyboard;

// Add DirectInput-specific defines
#include "../core/di_keycodes.h"

// Declare external variables
extern std::atomic<bool> autoAirtechEnabled;
extern std::atomic<bool> autoJumpEnabled;
extern std::atomic<int> autoAirtechDirection;
extern std::atomic<int> jumpDirection;
extern std::atomic<int> jumpTarget;
extern std::atomic<bool> menuOpen;
extern std::atomic<bool> keyMonitorRunning;

// Flag to track if DirectInput is available
extern bool g_directInputAvailable;

// Flag to control DirectInput behavior
extern bool g_directInputReadOnly;
static std::atomic<bool> globalF1ThreadRunning{true};

// Function declarations
void MonitorKeys();
void RestartKeyMonitoring();
bool InitDirectInput(HINSTANCE hInstance);
void CleanupDirectInput();
bool ReadDirectInputKeyboardState(BYTE* keyboardState);
bool IsDIKeyPressed(BYTE* keyboardState, DWORD dikCode);
bool InitDirectInputReadOnly(HINSTANCE hInstance);
bool PollDirectInputState(BYTE* keyboardState);

// DirectInput helper functions (declared in core/di_keycodes.h)
void DetectKeyBindingsWithDI();
void DetectKeyBindingsWithWinAPI();

// Add these function declarations (placed after existing declarations)
int MapEFZKeyToVK(unsigned short efzKey);
bool ReadKeyMappingsFromIni();
bool ReadDirectInputKeyboardState(BYTE* keyboardState);
void ShowEditDataDialog(HWND hParent); // Forward declaration for GUI function

// Hotkey cooldown management (called when menu closes)
void StartHotkeyCooldown();

// Add these forward declarations at the top of the file (after existing includes)
int MapEFZKeyToVK(unsigned short efzKey);
extern std::atomic<int> frameCount;
extern std::atomic<int> startFrameCount;

// Add these after the DirectInput global objects

// Gamepad device structure
struct GamepadDevice {
    LPDIRECTINPUTDEVICE8 device;
    DIJOYSTATE2 state;
    DIJOYSTATE2 prevState;
    bool connected;
    std::string name;
};

// Global gamepad variables
extern GamepadDevice g_gamepads[MAX_CONTROLLERS];
extern int g_gamepadCount;

// Gamepad functions
bool InitDirectInputGamepads();
BOOL CALLBACK EnumGamepadsCallback(const DIDEVICEINSTANCE* pdidInstance, VOID* pContext);
bool UpdateGamepadState(int gamepadIndex);
bool IsGamepadButtonPressed(int gamepadIndex, int buttonIndex);
bool IsGamepadButtonHeld(int gamepadIndex, int buttonIndex);
bool IsGamepadAxisActive(int gamepadIndex, int axisIndex, int threshold = 500);
void GlobalF1MonitorThread();