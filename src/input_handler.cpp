#include <fstream>
#include <iomanip>
#include <chrono>
#include "../include/input_handler.h"
#include "../include/utilities.h"
#include "../include/gui.h"
#include "../include/logger.h"
#include "../include/memory.h"
#include "../include/constants.h"
#include "../include/network.h"
#include <windows.h>
#include <thread>
#include <atomic>
#include <locale>
#include <dinput.h>
#include <sstream>
#include <string>
#include <commctrl.h> // Add this include for Common Controls
#include "../include/imgui_impl.h"
#include "../include/overlay.h"  // Add this include for DirectDrawHook class
#include "../include/config.h"
#include "../include/input_motion.h" // For QueueMotionInput
#ifndef VK_NUMPAD_ADD
#define VK_NUMPAD_ADD      0x6B
#endif
#ifndef VK_NUMPAD_SUBTRACT
#define VK_NUMPAD_SUBTRACT 0x6D
#endif
#ifndef VK_NUMPAD_MULTIPLY
#define VK_NUMPAD_MULTIPLY 0x6A
#endif
#ifndef VK_NUMPAD_DIVIDE
#define VK_NUMPAD_DIVIDE   0x6F
#endif

#pragma comment(lib, "dinput8.lib")
#pragma comment(lib, "dxguid.lib")
#pragma comment(lib, "comctl32.lib") // Add this comment directive for Common Controls

// Global DirectInput objects
LPDIRECTINPUT8 g_pDI = nullptr;
LPDIRECTINPUTDEVICE8 g_pKeyboard = nullptr;

// Add these global variables
bool g_directInputReadOnly = true;  // Default to read-only mode
bool g_directInputAvailable = false;

int g_gamepadCount = 0;
GamepadDevice g_gamepads[MAX_CONTROLLERS];
extern void ResetFrameCounter();

#define JOYSTICK_DEADZONE 8000  // Standard deadzone for a 16-bit joystick

// Initialize DirectInput in read-only mode
bool InitDirectInputReadOnly(HINSTANCE hInstance) {
    HRESULT hr;
    
    // First clean up any existing DirectInput objects
    CleanupDirectInput();
    
    LogOut("[INPUT] Initializing DirectInput in read-only mode", true);
    
    // Create DirectInput interface
    hr = DirectInput8Create(hInstance, DIRECTINPUT_VERSION, 
                            IID_IDirectInput8, (void**)&g_pDI, NULL);
    if (FAILED(hr)) {
        LogOut("[INPUT] Failed to create DirectInput interface: " + std::to_string(hr), true);
        return false;
    }
    
    // Create keyboard device
    hr = g_pDI->CreateDevice(GUID_SysKeyboard, &g_pKeyboard, NULL);
    if (FAILED(hr)) {
        LogOut("[INPUT] Failed to create keyboard device: " + std::to_string(hr), true);
        CleanupDirectInput();
        return false;
    }
    
    // Set data format to keyboard
    hr = g_pKeyboard->SetDataFormat(&c_dfDIKeyboard);
    if (FAILED(hr)) {
        LogOut("[INPUT] Failed to set keyboard data format: " + std::to_string(hr), true);
        CleanupDirectInput();
        return false;
    }
    
    // Set cooperative level - use the console window with BACKGROUND flag
    // This ensures we don't interfere with the game's input handling
    hr = g_pKeyboard->SetCooperativeLevel(
        GetConsoleWindow(), 
        DISCL_NONEXCLUSIVE | DISCL_BACKGROUND);
    if (FAILED(hr)) {
        LogOut("[INPUT] Failed to set cooperative level: " + std::to_string(hr), true);
        CleanupDirectInput();
        return false;
    }
    
    // Try to acquire the keyboard, but don't worry if it fails
    // We'll try again when polling
    g_pKeyboard->Acquire();
    
    g_directInputAvailable = true;
    LogOut("[INPUT] DirectInput initialized in read-only mode", true);
    return true;
}

// Clean up DirectInput
void CleanupDirectInput() {
    if (g_pKeyboard) {
        g_pKeyboard->Unacquire();
        g_pKeyboard->Release();
        g_pKeyboard = nullptr;
    }
    
    if (g_pDI) {
        g_pDI->Release();
        g_pDI = nullptr;
    }
    
    LogOut("[INPUT] DirectInput cleaned up", true);
}

// Poll DirectInput state without interfering with the game
bool PollDirectInputState(BYTE* keyboardState) {
    if (!g_pKeyboard || !g_directInputAvailable)
        return false;
    
    HRESULT hr = g_pKeyboard->GetDeviceState(256, keyboardState);
    
    // If we lost the device, try to reacquire it
    if (hr == DIERR_INPUTLOST || hr == DIERR_NOTACQUIRED) {
        hr = g_pKeyboard->Acquire();
        if (SUCCEEDED(hr)) {
            hr = g_pKeyboard->GetDeviceState(256, keyboardState);
        }
    }
    
    return SUCCEEDED(hr);
}

// Helper to check if a DirectInput key is pressed
bool IsDIKeyPressed(BYTE* keyboardState, DWORD dikCode) {
    if (!keyboardState)
        return false;
    
    return (keyboardState[dikCode] & 0x80) != 0;
}

// Add this helper function at the top to handle keyboard input more reliably
bool IsKeyPressed(int vKey, bool checkState) {
    SHORT keyState;
    
    // Force C locale for consistency
    std::locale::global(std::locale("C"));
    
    // Use both methods to increase reliability across keyboard layouts
    if (checkState) {
        // Check if key is pressed right now (synchronous)
        keyState = GetKeyState(vKey);
        if ((keyState & 0x8000) != 0) {
            return true;
        }
        
        // Also check asynchronous state
        keyState = GetAsyncKeyState(vKey);
        return ((keyState & 0x8000) != 0);
    } else {
        // Check for key press since last call (bit 0)
        keyState = GetAsyncKeyState(vKey);
        return ((keyState & 0x0001) != 0);
    }
}

// Add a flag to track if the monitor thread is running
std::atomic<bool> keyMonitorRunning(true);

void MonitorKeys() {
    LogOut("[KEYBINDS] Key monitoring thread started", true);
    
    // Get config settings for hotkeys
    const Config::Settings& cfg = Config::GetSettings();
    
    // Log the hotkeys for debugging
    LogOut("[KEYBINDS] Hotkey values from config:", true);
    LogOut("[KEYBINDS] Teleport/Load key: " + GetKeyName(cfg.teleportKey), true);
    LogOut("[KEYBINDS] Record/Save key: " + GetKeyName(cfg.recordKey), true);
    LogOut("[KEYBINDS] Config Menu key: " + GetKeyName(cfg.configMenuKey), true);
    LogOut("[KEYBINDS] Toggle ImGui key: " + GetKeyName(cfg.toggleImGuiKey), true);
    
    // Use configured hotkeys, with fallbacks for safety
    int teleportKey = (cfg.teleportKey > 0) ? cfg.teleportKey : '1';
    int recordKey = (cfg.recordKey > 0) ? cfg.recordKey : '2';
    int configMenuKey = (cfg.configMenuKey > 0) ? cfg.configMenuKey : '3';
    int toggleTitleKey = (cfg.toggleTitleKey > 0) ? cfg.toggleTitleKey : '4';
    int resetFrameCounterKey = (cfg.resetFrameCounterKey > 0) ? cfg.resetFrameCounterKey : '5';
    int helpKey = (cfg.helpKey > 0) ? cfg.helpKey : '6';
    int toggleImGuiKey = (cfg.toggleImGuiKey > 0) ? cfg.toggleImGuiKey : VK_F12;

    // Constants for teleport positions
    const double centerX = 320.0;
    const double leftX = 43.6548, rightX = 595.425, teleportY = 0.0;
    const double p1StartX = 240.0, p2StartX = 400.0, startY = 0.0;

    while (keyMonitorRunning) {
        // Update window active state at the beginning of each loop
        UpdateWindowActiveState();
        
        // Only process hotkeys if EFZ window is active AND we're not currently in a GUI
        if (g_efzWindowActive.load() && !g_guiActive.load()) {
            bool keyHandled = false;

            // Check if the config menu should be opened
            if (IsKeyPressed(configMenuKey, false)) {
                OpenMenu();
                // Wait a bit to prevent multiple openings
                Sleep(500);
                continue;
            }

            // Check if ImGui should be toggled
            if (IsKeyPressed(toggleImGuiKey, false)) {
                ImGuiImpl::ToggleVisibility();
                keyHandled = true;
            } else if (IsKeyPressed(teleportKey, true)) {
                // Round start positions
                if (IsKeyPressed(VK_DOWN, true) && IsKeyPressed('A', true)) {
                    uintptr_t base = GetEFZBase();
                    if (base) {
                        SetPlayerPosition(base, EFZ_BASE_OFFSET_P1, p1StartX, startY);
                        SetPlayerPosition(base, EFZ_BASE_OFFSET_P2, p2StartX, startY);
                        DirectDrawHook::AddMessage("Round Start Position", "SYSTEM", RGB(100, 255, 100), 1500, 0, 100);
                    }
                    keyHandled = true;
                }
                // Center players
                else if (IsKeyPressed(VK_DOWN, true)) {
                    uintptr_t base = GetEFZBase();
                    if (base) {
                        SetPlayerPosition(base, EFZ_BASE_OFFSET_P1, centerX, teleportY);
                        SetPlayerPosition(base, EFZ_BASE_OFFSET_P2, centerX, teleportY);
                        DirectDrawHook::AddMessage("Players Centered", "SYSTEM", RGB(100, 255, 100), 1500, 0, 100);
                    }
                    keyHandled = true;
                }
                // Players to left corner
                else if (IsKeyPressed(VK_LEFT, true)) {
                    uintptr_t base = GetEFZBase();
                    if (base) {
                        SetPlayerPosition(base, EFZ_BASE_OFFSET_P1, leftX, teleportY);
                        SetPlayerPosition(base, EFZ_BASE_OFFSET_P2, leftX, teleportY);
                        DirectDrawHook::AddMessage("Left Corner", "SYSTEM", RGB(100, 255, 100), 1500, 0, 100);
                    }
                    keyHandled = true;
                }
                // Players to right corner
                else if (IsKeyPressed(VK_RIGHT, true)) {
                    uintptr_t base = GetEFZBase();
                    if (base) {
                        SetPlayerPosition(base, EFZ_BASE_OFFSET_P1, rightX, teleportY);
                        SetPlayerPosition(base, EFZ_BASE_OFFSET_P2, rightX, teleportY);
                        DirectDrawHook::AddMessage("Right Corner", "SYSTEM", RGB(100, 255, 100), 1500, 0, 100);
                    }
                    keyHandled = true;
                }
                // Swap player positions
                else if (IsKeyPressed(detectedBindings.dButton, true)) {
                    uintptr_t base = GetEFZBase();
                    if (base) {
                        double tempX1 = 0.0, tempY1 = 0.0;
                        double tempX2 = 0.0, tempY2 = 0.0;
                        
                        // Read current positions
                        uintptr_t xAddr1 = ResolvePointer(base, EFZ_BASE_OFFSET_P1, XPOS_OFFSET);
                        uintptr_t yAddr1 = ResolvePointer(base, EFZ_BASE_OFFSET_P1, YPOS_OFFSET);
                        uintptr_t xAddr2 = ResolvePointer(base, EFZ_BASE_OFFSET_P2, XPOS_OFFSET);
                        uintptr_t yAddr2 = ResolvePointer(base, EFZ_BASE_OFFSET_P2, YPOS_OFFSET);
                        
                        if (xAddr1 && yAddr1 && xAddr2 && yAddr2) {
                            SafeReadMemory(xAddr1, &tempX1, sizeof(double));
                            SafeReadMemory(yAddr1, &tempY1, sizeof(double));
                            SafeReadMemory(xAddr2, &tempX2, sizeof(double));
                            SafeReadMemory(yAddr2, &tempY2, sizeof(double));
                            
                            // Swap positions
                            SetPlayerPosition(base, EFZ_BASE_OFFSET_P1, tempX2, tempY2);
                            SetPlayerPosition(base, EFZ_BASE_OFFSET_P2, tempX1, tempY1);
                            DirectDrawHook::AddMessage("Positions Swapped", "SYSTEM", RGB(100, 255, 100), 1500, 0, 100);
                        } else {
                            DirectDrawHook::AddMessage("Swap Failed: Can't read positions", "SYSTEM", RGB(255, 100, 100), 1500, 0, 100);
                        }
                    }
                    keyHandled = true;
                }
                // If no other key was pressed with teleport key, load saved position
                else {
                    uintptr_t base = GetEFZBase();
                    if (base) {
                        LoadPlayerPositions(base);
                        DirectDrawHook::AddMessage("Position Loaded", "SYSTEM", RGB(100, 255, 100), 1500, 0, 100);
                    }
                    keyHandled = true;
                }
            } else if (IsKeyPressed(cfg.recordKey, false)) {
                uintptr_t base = GetEFZBase();
                if (base) {
                    SavePlayerPositions(base);
                    DirectDrawHook::AddMessage("Position Saved", "SYSTEM", RGB(255, 255, 100), 1500, 0, 100);
                }
                keyHandled = true;
            } else if (IsKeyPressed(toggleTitleKey, true)) {
                // Toggle stats display instead of detailed title mode
                bool currentState = g_statsDisplayEnabled.load();
                g_statsDisplayEnabled.store(!currentState);
                
                if (g_statsDisplayEnabled.load()) {
                    LogOut("[STATS] Stats display enabled", true);
                    DirectDrawHook::AddMessage("Stats Display Enabled", "SYSTEM", RGB(255, 255, 0), 1500, 20, 100);
                } else {
                    LogOut("[STATS] Stats display disabled", true);
                    DirectDrawHook::AddMessage("Stats Display Disabled", "SYSTEM", RGB(255, 255, 0), 1500, 20, 100);
                }
                keyHandled = true;
            } else if (IsKeyPressed(cfg.resetFrameCounterKey, false)) {
                ResetFrameCounter();
                keyHandled = true;
            } else if (IsKeyPressed(cfg.helpKey, false)) {
                ShowHotkeyInfo();
                keyHandled = true;
            } else if (IsKeyPressed(VK_F8, false)) {
                std::string status = "OFF";
                if (autoAirtechEnabled) {
                    status = autoAirtechDirection == 0 ? "FORWARD" : "BACKWARD";
                }
                DirectDrawHook::AddMessage(("Auto-Airtech: " + status).c_str(), "SYSTEM", RGB(255, 165, 0), 1500, 0, 100);
                keyHandled = true;
            } else if (IsKeyPressed(VK_F9, false)) {
                autoJumpEnabled = !autoJumpEnabled;
                DirectDrawHook::AddMessage(autoJumpEnabled ? "Auto-Jump: ON" : "Auto-Jump: OFF", "SYSTEM", RGB(255, 165, 0), 1500, 0, 100);
                keyHandled = true;
            }

            // --- P2 Motion Input Debug Hotkeys ---
            if (GetAsyncKeyState(VK_NUMPAD_ADD) & 0x8000) { // Numpad +
                QueueMotionInput(2, MOTION_236B, GAME_INPUT_B); // QCF+B
                LogOut("[HOTKEY] Simulated P2 QCF+B (Numpad +)", true);
            }
            if (GetAsyncKeyState(VK_NUMPAD_SUBTRACT) & 0x8000) { // Numpad -
                QueueMotionInput(2, MOTION_623B, GAME_INPUT_B); // DP+B
                LogOut("[HOTKEY] Simulated P2 DP+B (Numpad -)", true);
            }
            if (GetAsyncKeyState(VK_NUMPAD_MULTIPLY) & 0x8000) { // Numpad *
                QueueMotionInput(2, MOTION_214B, GAME_INPUT_B); // QCB+B
                LogOut("[HOTKEY] Simulated P2 QCB+B (Numpad *)", true);
            }
            if (GetAsyncKeyState(VK_NUMPAD_DIVIDE) & 0x8000) { // Numpad /
                QueueMotionInput(2, MOTION_41236C, GAME_INPUT_C); // Super1 (41236C)
                LogOut("[HOTKEY] Simulated P2 Super1 (41236C) (Numpad /)", true);
            }

            // If a key was handled, wait for it to be released
            if (keyHandled) {
                Sleep(100);
                while (IsKeyPressed(teleportKey, true) || IsKeyPressed(recordKey, true) ||
                       IsKeyPressed(toggleTitleKey, true) || IsKeyPressed(resetFrameCounterKey, true) ||
                       IsKeyPressed(helpKey, true) || IsKeyPressed(VK_F8, true) || IsKeyPressed(VK_F9, true)) {
                    Sleep(10);
                }
            }
        }

        // Sleep to avoid high CPU usage
        Sleep(16); // ~60Hz polling
    }
    
    LogOut("[KEYBINDS] Key monitoring thread exiting", true);
}

// Add this function to restart key monitoring
void RestartKeyMonitoring() {
    LogOut("[KEYBINDS] Restarting key monitoring system", true);
    
    // Signal existing thread to exit
    keyMonitorRunning = false;
    
    // Give it time to exit cleanly
    Sleep(100);
    
    // Reset state
    p1Jumping = false;
    p2Jumping = false;
    // Reset any other state variables...
    
    // Start new monitoring thread
    keyMonitorRunning = true;
    std::thread(MonitorKeys).detach();
    
    LogOut("[KEYBINDS] Key monitoring system restarted", true);
}

void DebugInputs() {
    static uint8_t lastP1Input = 0;
    
    // Only check when EFZ is active
    if (!IsEFZWindowActive()) return;
    
    // Read P1's inputs
    uint8_t p1Input = GetPlayerInputs(1);
    
    // If inputs changed, log them
    if (p1Input != lastP1Input) {
        std::string inputStr = "";
        if (p1Input & INPUT_UP) inputStr += "UP ";
        if (p1Input & INPUT_DOWN) inputStr += "DOWN ";
        if (p1Input & INPUT_LEFT) inputStr += "LEFT ";
        if (p1Input & INPUT_RIGHT) inputStr += "RIGHT ";
        if (p1Input & INPUT_A) inputStr += "A ";
        if (p1Input & INPUT_B) inputStr += "B ";
        if (p1Input & INPUT_C) inputStr += "C ";
        if (p1Input & INPUT_D) inputStr += "D ";
        
        LogOut("[DEBUG] P1 Inputs: " + (inputStr.empty() ? "NONE" : inputStr), true);
        lastP1Input = p1Input;
    }
}

// Add these declarations (near the top, with other global variables)
// Use frameCounter from utilities.cpp instead of redefining
extern std::atomic<int> frameCounter;
// We don't have startFrameCount elsewhere, so properly define it
std::atomic<int> startFrameCount(0);

// Complete the ReadKeyMappingsFromIni function
bool ReadKeyMappingsFromIni() {
    HANDLE configFile = CreateFileA("key.ini", GENERIC_READ, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (configFile == INVALID_HANDLE_VALUE) {
        LogOut("[INPUT] Failed to open key.ini - using default key bindings", true);
        return false;
    }

    DWORD fileSize = GetFileSize(configFile, NULL);
    if (fileSize < 32) { // At least 2 controllers with 16 bytes each
        LogOut("[INPUT] key.ini file too small", true);
        CloseHandle(configFile);
        return false;
    }

    // Allocate buffer and read file
    void* fileBuffer = operator new(fileSize);
    if (!fileBuffer) {
        CloseHandle(configFile);
        return false;
    }

    DWORD bytesRead = 0;
    if (!ReadFile(configFile, fileBuffer, fileSize, &bytesRead, NULL)) {
        operator delete(fileBuffer);
        CloseHandle(configFile);
        return false;
    }

    // Focus on Player 1's mappings (first 16 bytes)
    // In key.ini, the mapping order is: ↓↑←→ABCD
    unsigned char* p1Data = (unsigned char*)fileBuffer;

    // Read P1's direction keys (the first 4 button pairs)
    struct KeyMapping {
        int offset;
        const char* name;
        int* bindingPtr;
    } keyMaps[] = {
        { 0, "Down", &detectedBindings.upKey },
        { 2, "Up", &detectedBindings.downKey },
        { 4, "Left", &detectedBindings.leftKey },
        { 6, "Right", &detectedBindings.rightKey },
        { 8, "A (Light)", &detectedBindings.aButton },
        { 10, "B (Medium)", &detectedBindings.bButton },
        { 12, "C (Heavy)", &detectedBindings.cButton },
        { 14, "D (Special)", &detectedBindings.dButton },
    };

    LogOut("[INPUT] Reading key bindings from key.ini", true);
    for (int i = 0; i < 8; i++) {
        unsigned char byte1 = p1Data[keyMaps[i].offset];
        unsigned char byte2 = p1Data[keyMaps[i].offset + 1];
        
        // Calculate the key value using the same formula as the game
        unsigned short keyValue = (byte1 << 8) | byte2;
        if (byte1 != 0) {
            keyValue--;
        }

        // Map this value to a virtual key code
        int vkKey = MapEFZKeyToVK(keyValue);
        *keyMaps[i].bindingPtr = vkKey;

        LogOut("[INPUT] P1 " + std::string(keyMaps[i].name) + " = " +
               GetKeyName(vkKey) + " (raw value: " + std::to_string(keyValue) + ")", true);
    }

    // Set flags to indicate we've detected the bindings
    detectedBindings.directionsDetected = true;
    detectedBindings.attacksDetected = true;
    detectedBindings.inputDevice = INPUT_DEVICE_KEYBOARD;
    detectedBindings.deviceName = "Keyboard (from key.ini)";

    operator delete(fileBuffer);
    CloseHandle(configFile);
    return true;
}

// Fix boolean/integer type warning by implementing conversion functions
bool ReadDirectInputKeyboardState(BYTE* keyboardState) {
    if (!g_directInputAvailable || !g_pKeyboard || !keyboardState)
        return false;
    
    // Try to get the keyboard state
    HRESULT hr = g_pKeyboard->GetDeviceState(256, keyboardState);
    
    // If we lost the device, try to reacquire it
    if (hr == DIERR_INPUTLOST || hr == DIERR_NOTACQUIRED) {
        hr = g_pKeyboard->Acquire();
        if (SUCCEEDED(hr)) {
            hr = g_pKeyboard->GetDeviceState(256, keyboardState);
        }
    }
    
    return SUCCEEDED(hr);
}

// Implementation of MapEFZKeyToVK
int MapEFZKeyToVK(unsigned short efzKey) {
    switch (efzKey) {
        case 0xCB: return VK_LEFT;
        case 0xCD: return VK_RIGHT;
        case 0xC8: return VK_UP;
        case 0xD0: return VK_DOWN;
        case 0x2C: return 'Z';
        case 0x2D: return 'X';
        case 0x2E: return 'C';
        case 0x2F: return 'V';
        case 0x1E: return 'A';
        case 0x1F: return 'S';
        case 0x20: return 'D';
        case 0x21: return 'F';
        case 0x39: return VK_SPACE;
        case 0x1C: return VK_RETURN;
        
        // Handle specific joystick buttons based on config_EN.exe
        case 0x13:  return VK_LEFT;     // Joystick Left
        case 0x114: return VK_RIGHT;    // Joystick Right
        case 0x111: return VK_UP;       // Joystick Up
        case 0x112: return VK_DOWN;     // Joystick Down
        
        default:
            return 0;
    }
}

// REMOVED redundant ShowConfigMenu function. The functionality is handled by OpenMenu() in gui.cpp

void DetectKeyBindings() {
    // First, try to read from the INI file
    if (ReadKeyMappingsFromIni()) {
        return;
    }

    // If no ini mappings found, use DirectInput detection
    if (g_directInputAvailable) {
        DetectKeyBindingsWithDI();
    }
    // Otherwise, as a fallback, use Windows API detection
    else {
        LogOut("[INPUT] Using WinAPI for key detection", true);
    }
}

bool UpdateGamepadState(int gamepadIndex) {
    // Input validation
    if (gamepadIndex < 0 || gamepadIndex >= g_gamepadCount || !g_gamepads[gamepadIndex].device) {
        return false;
    }
    
    // Store previous state
    memcpy(&g_gamepads[gamepadIndex].prevState, &g_gamepads[gamepadIndex].state, sizeof(DIJOYSTATE2));
    
    // Poll the device
    HRESULT hr = g_gamepads[gamepadIndex].device->Poll();
    
    // If polling failed, try to reacquire
    if (FAILED(hr)) {
        hr = g_gamepads[gamepadIndex].device->Acquire();
        if (FAILED(hr)) {
            g_gamepads[gamepadIndex].connected = false;
            return false;
        }
        
        // Try polling again
        hr = g_gamepads[gamepadIndex].device->Poll();
        if (FAILED(hr)) {
            return false;
        }
    }
    
    // Get device state
    hr = g_gamepads[gamepadIndex].device->GetDeviceState(sizeof(DIJOYSTATE2), &g_gamepads[gamepadIndex].state);
    if (FAILED(hr)) {
        return false;
    }
    
    return true;
}

// Add this implementation:
std::string GetDIKeyName(int dikCode) {
    // Look up the key in our mappings array
    for (const auto& mapping : KeyMappings) {
        if (mapping.dikCode == dikCode) {
            return mapping.keyName;
        }
    }
    
    // If not found, return a generic name
    return "Key(" + std::to_string(dikCode) + ")";
}

void DetectKeyBindingsWithDI() {
    static BYTE prevKeyboardState[256] = {0};
    BYTE keyboardState[256] = {0};
    
    // Read keyboard state
    bool keyboardActive = ReadDirectInputKeyboardState(keyboardState);
    
    // Update all gamepad states
    bool anyGamepadActive = false;
    for (int i = 0; i < g_gamepadCount; i++) {
        if (UpdateGamepadState(i)) {
            anyGamepadActive = true;
        }
    }
    
    // Get current game inputs
    uint8_t currentInputs = GetPlayerInputs(1);
    static uint8_t prevInputs = 0;
    
    // Detect direction key bindings
    if (!detectedBindings.directionsDetected) {
        // Check for keyboard key presses that coincide with direction input changes
        if (keyboardActive) {
            for (int key = 0; key < 256; key++) {
                // Key just pressed
                if ((keyboardState[key] & 0x80) && !(prevKeyboardState[key] & 0x80)) {
                    // Check if this corresponds to a direction input
                    if ((currentInputs & INPUT_UP) && !(prevInputs & INPUT_UP)) {
                        detectedBindings.upKey = key;
                        detectedBindings.inputDevice = INPUT_DEVICE_KEYBOARD;
                        detectedBindings.deviceName = "Keyboard";
                        LogOut("[KEYBIND] Detected Up key (Keyboard): " + GetDIKeyName(key), detailedLogging);
                    }
                    if ((currentInputs & INPUT_DOWN) && !(prevInputs & INPUT_DOWN)) {
                        detectedBindings.downKey = key;
                        detectedBindings.inputDevice = INPUT_DEVICE_KEYBOARD;
                        detectedBindings.deviceName = "Keyboard";
                        LogOut("[KEYBIND] Detected Down key (Keyboard): " + GetDIKeyName(key), detailedLogging);
                    }
                    if ((currentInputs & INPUT_LEFT) && !(prevInputs & INPUT_LEFT)) {
                        detectedBindings.leftKey = key;
                        detectedBindings.inputDevice = INPUT_DEVICE_KEYBOARD;
                        detectedBindings.deviceName = "Keyboard";
                        LogOut("[KEYBIND] Detected Left key (Keyboard): " + GetDIKeyName(key), detailedLogging);
                    }
                    if ((currentInputs & INPUT_RIGHT) && !(prevInputs & INPUT_RIGHT)) {
                        detectedBindings.rightKey = key;
                        detectedBindings.inputDevice = INPUT_DEVICE_KEYBOARD;
                        detectedBindings.deviceName = "Keyboard";
                        LogOut("[KEYBIND] Detected Right key (Keyboard): " + GetDIKeyName(key), detailedLogging);
                    }
                }
            }
        }

        // Fix the boolean/int mixing warnings by using proper boolean operations
        bool leftPressed = ((currentInputs & INPUT_LEFT) != 0);
        if ((GetAsyncKeyState(VK_LEFT) & 0x8000) != 0) {
            leftPressed = true; // Use assignment instead of |=
        }
        
        bool rightPressed = ((currentInputs & INPUT_RIGHT) != 0);
        if ((GetAsyncKeyState(VK_RIGHT) & 0x8000) != 0) {
            rightPressed = true;
        }
        
        bool upPressed = ((currentInputs & INPUT_UP) != 0);
        if ((GetAsyncKeyState(VK_UP) & 0x8000) != 0) {
            upPressed = true;
        }
        
        bool downPressed = ((currentInputs & INPUT_DOWN) != 0);
        if ((GetAsyncKeyState(VK_DOWN) & 0x8000) != 0) {
            downPressed = true;
        }
        
        bool aPressed = ((currentInputs & INPUT_A) != 0);
        if ((GetAsyncKeyState('Z') & 0x8000) != 0) {
            aPressed = true;
        }
        
        // Use detected bindings if available
        if (detectedBindings.directionsDetected) {
            if ((GetAsyncKeyState(detectedBindings.leftKey) & 0x8000) != 0) {
                leftPressed = true;
            }
            if ((GetAsyncKeyState(detectedBindings.rightKey) & 0x8000) != 0) {
                rightPressed = true;
            }
            if ((GetAsyncKeyState(detectedBindings.upKey) & 0x8000) != 0) {
                upPressed = true;
            }
            if ((GetAsyncKeyState(detectedBindings.downKey) & 0x8000) != 0) {
                downPressed = true;
            }
            
            if (detectedBindings.attacksDetected) {
                if ((GetAsyncKeyState(detectedBindings.aButton) & 0x8000) != 0) {
                    aPressed = true;
                }
            }
        }
        
        // Check for gamepad inputs
        if (anyGamepadActive) {
            for (int i = 0; i < g_gamepadCount; i++) {
                if (!g_gamepads[i].connected) continue;
                
                // Check if current gamepad state has directional input that previous didn't
                bool gpLeftPressed = (g_gamepads[i].state.lX < -JOYSTICK_DEADZONE) && 
                                    !(g_gamepads[i].prevState.lX < -JOYSTICK_DEADZONE);
                bool gpRightPressed = (g_gamepads[i].state.lX > JOYSTICK_DEADZONE) && 
                                     !(g_gamepads[i].prevState.lX > JOYSTICK_DEADZONE);
                bool gpUpPressed = (g_gamepads[i].state.lY < -JOYSTICK_DEADZONE) && 
                                  !(g_gamepads[i].prevState.lY < -JOYSTICK_DEADZONE);
                bool gpDownPressed = (g_gamepads[i].state.lY > JOYSTICK_DEADZONE) && 
                                    !(g_gamepads[i].prevState.lY > JOYSTICK_DEADZONE);
                
                // D-Pad inputs
                if (g_gamepads[i].state.rgdwPOV[0] != 0xFFFFFFFF) {
                    DWORD pov = g_gamepads[i].state.rgdwPOV[0];
                    DWORD prevPov = g_gamepads[i].prevState.rgdwPOV[0];
                    
                    if ((pov >= 31500 || pov <= 00) && (prevPov < 31500 && prevPov > 00)) gpUpPressed = true;
                    if ((pov >= 00 && pov <= 13500) && (prevPov < 00 || prevPov > 13500)) gpRightPressed = true;
                    if ((pov >= 13500 && pov <= 200) && (prevPov < 13500 || prevPov > 200)) gpDownPressed = true;
                    if ((pov >= 200 && pov <= 31500) && (prevPov < 200 || prevPov > 31500)) gpLeftPressed = true;
                }
                
                // Associate gamepad inputs with game inputs
                if (gpLeftPressed && (currentInputs & INPUT_LEFT)) {
                    detectedBindings.leftKey = VK_GAMEPAD_DPAD_LEFT;
                    detectedBindings.inputDevice = INPUT_DEVICE_GAMEPAD;
                    detectedBindings.gamepadIndex = i;
                    detectedBindings.deviceName = "Gamepad " + std::to_string(i + 1);
                    leftPressed = true;
                    LogOut("[KEYBIND] Detected Left input (Gamepad " + std::to_string(i + 1) + ")", detailedLogging);
                }
                if (gpRightPressed && (currentInputs & INPUT_RIGHT)) {
                    detectedBindings.rightKey = VK_GAMEPAD_DPAD_RIGHT;
                    detectedBindings.inputDevice = INPUT_DEVICE_GAMEPAD;
                    detectedBindings.gamepadIndex = i;
                    detectedBindings.deviceName = "Gamepad " + std::to_string(i + 1);
                    rightPressed = true;
                    LogOut("[KEYBIND] Detected Right input (Gamepad " + std::to_string(i + 1) + ")", detailedLogging);
                }
                if (gpUpPressed && (currentInputs & INPUT_UP)) {
                    detectedBindings.upKey = VK_GAMEPAD_DPAD_UP;
                    detectedBindings.inputDevice = INPUT_DEVICE_GAMEPAD;
                    detectedBindings.gamepadIndex = i;
                    detectedBindings.deviceName = "Gamepad " + std::to_string(i + 1);
                    upPressed = true;
                    LogOut("[KEYBIND] Detected Up input (Gamepad " + std::to_string(i + 1) + ")", detailedLogging);
                }
                if (gpDownPressed && (currentInputs & INPUT_DOWN)) {
                    detectedBindings.downKey = VK_GAMEPAD_DPAD_DOWN;
                    detectedBindings.inputDevice = INPUT_DEVICE_GAMEPAD;
                    detectedBindings.gamepadIndex = i;
                    detectedBindings.deviceName = "Gamepad " + std::to_string(i + 1);
                    downPressed = true;
                    LogOut("[KEYBIND] Detected Down input (Gamepad " + std::to_string(i + 1) + ")", detailedLogging);
                }
            }
        }

        // Now use the boolean variables in your logic
        if (leftPressed) {
            // Check for attack button combo detection
            if (aPressed) {
                detectedBindings.attacksDetected = true;
                LogOut("[KEYBIND] Detected A button from Left+A combo", detailedLogging);
            }
            
            // Mark directions as detected if we see at least one
            if (!detectedBindings.directionsDetected) {
                detectedBindings.directionsDetected = true;
                LogOut("[KEYBIND] Direction keys detected", true);
            }
        }
        
        if (rightPressed || upPressed || downPressed) {
            // Mark directions as detected
            if (!detectedBindings.directionsDetected) {
                detectedBindings.directionsDetected = true;
                LogOut("[KEYBIND] Direction keys detected", true);
            }
        }
    }
    
    // Also detect attack buttons if directions are already detected
    if (detectedBindings.directionsDetected && !detectedBindings.attacksDetected) {
        // Similar detection logic for attack buttons
        if ((currentInputs & INPUT_A) && !(prevInputs & INPUT_A)) {
            // Look for keyboard key that coincides with A button press
            if (keyboardActive) {
                for (int key = 0; key < 256; key++) {
                    if ((keyboardState[key] & 0x80) && !(prevKeyboardState[key] & 0x80)) {
                        detectedBindings.aButton = key;
                        LogOut("[KEYBIND] Detected A button (Keyboard): " + GetDIKeyName(key), detailedLogging);
                        detectedBindings.attacksDetected = true;
                        break;
                    }
                }
            }
            
            // Also check for gamepad button presses
            if (anyGamepadActive && !detectedBindings.attacksDetected) {
                for (int i = 0; i < g_gamepadCount; i++) {
                    if (!g_gamepads[i].connected) continue;
                    
                    // Check for any button press that wasn't active before
                    for (int btn = 0; btn < 32; btn++) {
                        if ((g_gamepads[i].state.rgbButtons[btn] & 0x80) && 
                            !(g_gamepads[i].prevState.rgbButtons[btn] & 0x80)) {
                            detectedBindings.aButton = VK_GAMEPAD_A + btn; // Use a virtual gamepad key code mapping
                            detectedBindings.inputDevice = INPUT_DEVICE_GAMEPAD;
                            detectedBindings.gamepadIndex = i;
                            detectedBindings.deviceName = "Gamepad " + std::to_string(i + 1);
                            detectedBindings.attacksDetected = true;
                            LogOut("[KEYBIND] Detected A button (Gamepad " + std::to_string(i + 1) + 
                                   " button " + std::to_string(btn) + ")", detailedLogging);
                            break;
                        }
                    }
                    
                    if (detectedBindings.attacksDetected) break;
                }
            }
        }
    
    }
    
    // Store current state for next comparison
    if (keyboardActive) {
        memcpy(prevKeyboardState, keyboardState, sizeof(prevKeyboardState));
    }
    prevInputs = currentInputs;
}

