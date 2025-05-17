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
    
    // Reset state variables
    p1Jumping = false;
    p2Jumping = false;
    
    // Constants for teleport positions
    const double centerX = 320.0;
    const double leftX = 43.6548, rightX = 595.425, teleportY = 0.0;
    const double p1StartX = 240.0, p2StartX = 400.0, startY = 0.0;
    
    // Initialize recorded positions with default values
    double recordedX1 = p1StartX, recordedY1 = startY;
    double recordedX2 = p2StartX, recordedY2 = startY;
    bool hasRecorded = false;  // Start with false to require recording first
    
    // Wait a moment for other initialization to complete
    Sleep(100);
    
    // Clear the console and show hotkey info
    ShowHotkeyInfo();
    
    // Try to detect key bindings every so often
    int detectionCounter = 0;
    
    // Add this near the beginning of MonitorKeys function
    bool enableDebugLogging = false;  // Set to true when needed for deeper investigation
    
    while (keyMonitorRunning) {
        // Check for online mode
        if (isOnlineMatch.load()) {
            Sleep(100);  // Sleep longer in online mode
            continue;
        }
        
        // Only process hotkeys when EFZ window is active
        if (IsEFZWindowActive()) {
            // Get base address
            uintptr_t base = GetEFZBase();
            
            // Periodically try to detect key bindings
            if (++detectionCounter >= 100) {
                detectionCounter = 0;
                if (!detectedBindings.directionsDetected || !detectedBindings.attacksDetected) {
                    DetectKeyBindings();
                }
            }

            // Use simple key state checks
            bool key1Pressed = (GetAsyncKeyState('1') & 0x8000) != 0;
            
            // For key combinations, detect inputs from game memory directly
            uint8_t gameInputs = GetPlayerInputs(1);
            
            // IMPORTANT: Process key combinations ONLY if key 1 is pressed
            if (key1Pressed && base) {
                // Resolve player positions
                uintptr_t p1BaseOffset = EFZ_BASE_OFFSET_P1;
                uintptr_t p2BaseOffset = EFZ_BASE_OFFSET_P2;
                
                // Check for 1 key held down
                if (GetAsyncKeyState('1') & 0x8000) {
                    // Check for direction keys
                    // Simplified direct checks for better reliability
                    if (GetAsyncKeyState(VK_DOWN) & 0x8000) {
                        // 1+Down - Center players close together
                        LogOut("[POSITION] 1+Down pressed - teleporting to center", true);
                        
                        bool aPressed = (GetAsyncKeyState('Z') & 0x8000) != 0;
                        
                        // Check if player bindings are detected and use them
                        if (detectedBindings.attacksDetected) {
                            if (GetAsyncKeyState(detectedBindings.aButton) & 0x8000) {
                                aPressed = true;
                            }
                        }
                        
                        if (aPressed) {
                            // 1+Down+A - Round start positions
                            SetPlayerPosition(base, p1BaseOffset, p1StartX, startY);
                            SetPlayerPosition(base, p2BaseOffset, p2StartX, startY);
                            LogOut("[POSITION] Teleported players to round start positions", true);
                        } else {
                            // 1+Down - Center close together (UPDATED - 3 unit spacing)
                            SetPlayerPosition(base, p1BaseOffset, centerX - 1.5, teleportY);
                            SetPlayerPosition(base, p2BaseOffset, centerX + 1.5, teleportY);
                            LogOut("[POSITION] Teleported players close together at center (3 unit spacing)", true);
                        }
                        Sleep(250); // Prevent multiple activations
                    }
                    else if (GetAsyncKeyState(VK_LEFT) & 0x8000) {
                        // 1+Left - Left side of screen (UPDATED - 3 unit spacing)
                        LogOut("[POSITION] 1+Left pressed - teleporting to left side", true);
                        SetPlayerPosition(base, p1BaseOffset, leftX, teleportY);
                        SetPlayerPosition(base, p2BaseOffset, leftX + 3.0, teleportY);
                        Sleep(250); // Prevent multiple activations
                    }
                    else if (GetAsyncKeyState(VK_RIGHT) & 0x8000) {
                        // 1+Right - Right side of screen (UPDATED - 3 unit spacing)
                        LogOut("[POSITION] 1+Right pressed - teleporting to right side", true);
                        SetPlayerPosition(base, p1BaseOffset, rightX - 3.0, teleportY);
                        SetPlayerPosition(base, p2BaseOffset, rightX, teleportY);
                        Sleep(250); // Prevent multiple activations
                    }
                    else if (GetAsyncKeyState(VK_UP) & 0x8000) {
                        // 1+Up - Swap positions
                        LogOut("[POSITION] 1+Up pressed - swapping positions", true);
                        double tempX1 = 0.0, tempY1 = 0.0;
                        double tempX2 = 0.0, tempY2 = 0.0;
                        
                        // Read current positions
                        uintptr_t xAddr1 = ResolvePointer(base, p1BaseOffset, XPOS_OFFSET);
                        uintptr_t yAddr1 = ResolvePointer(base, p1BaseOffset, YPOS_OFFSET);
                        uintptr_t xAddr2 = ResolvePointer(base, p2BaseOffset, XPOS_OFFSET);
                        uintptr_t yAddr2 = ResolvePointer(base, p2BaseOffset, YPOS_OFFSET);
                        
                        if (xAddr1 && yAddr1 && xAddr2 && yAddr2) {
                            SafeReadMemory(xAddr1, &tempX1, sizeof(double));
                            SafeReadMemory(yAddr1, &tempY1, sizeof(double));
                            SafeReadMemory(xAddr2, &tempX2, sizeof(double));
                            SafeReadMemory(yAddr2, &tempY2, sizeof(double));
                            
                            // Swap positions
                            SetPlayerPosition(base, p1BaseOffset, tempX2, tempY2);
                            SetPlayerPosition(base, p2BaseOffset, tempX1, tempY1);
                            LogOut("[POSITION] Swapped player positions", true);
                        } else {
                            LogOut("[POSITION] Failed to get player positions for swap", true);
                        }
                        Sleep(250); // Prevent multiple activations
                    }
                    else if (hasRecorded) {
                        // 1 only (with recorded positions) - Teleport to recorded positions
                        LogOut("[POSITION] Key 1 pressed - teleporting to recorded positions", true);
                        LogOut("[POSITION] Teleporting to recorded positions - P1(" + 
                               std::to_string(recordedX1) + "," + std::to_string(recordedY1) + 
                               ") P2(" + std::to_string(recordedX2) + "," + std::to_string(recordedY2) + ")", true);
    
                        // Set player positions with updateMoveID=true for Y positions to work
                        SetPlayerPosition(base, p1BaseOffset, recordedX1, recordedY1, true);
                        SetPlayerPosition(base, p2BaseOffset, recordedX2, recordedY2, true);
    
                        // Double-check Y positions after setting
                        uintptr_t yAddr1 = ResolvePointer(base, p1BaseOffset, YPOS_OFFSET);
                        uintptr_t yAddr2 = ResolvePointer(base, p2BaseOffset, YPOS_OFFSET);
    
                        double verifyY1 = 0.0, verifyY2 = 0.0;
                        if (yAddr1) SafeReadMemory(yAddr1, &verifyY1, sizeof(double));
                        if (yAddr2) SafeReadMemory(yAddr2, &verifyY2, sizeof(double));
    
                        LogOut("[POSITION] Verified Y positions after teleport - P1:" + 
                              std::to_string(verifyY1) + ", P2:" + std::to_string(verifyY2), true);
    
                        // If Y positions were not applied, try setting them with air state
                        if ((recordedY1 != 0 && verifyY1 == 0) || (recordedY2 != 0 && verifyY2 == 0)) {
                            LogOut("[POSITION] Y positions were reset by game, forcing air state", true);
                            
                            // Try setting move ID directly to air state
                            uintptr_t moveID1 = ResolvePointer(base, p1BaseOffset, MOVE_ID_OFFSET);
                            uintptr_t moveID2 = ResolvePointer(base, p2BaseOffset, MOVE_ID_OFFSET);
                            
                            if (moveID1 && recordedY1 != 0) {
                                short airState = FALLING_ID;
                                SafeWriteMemory(moveID1, &airState, sizeof(short));
                                SafeWriteMemory(yAddr1, &recordedY1, sizeof(double));
                            }
                            
                            if (moveID2 && recordedY2 != 0) {
                                short airState = FALLING_ID;
                                SafeWriteMemory(moveID2, &airState, sizeof(short));
                                SafeWriteMemory(yAddr2, &recordedY2, sizeof(double));
                            }
                        }
                        
                        // Sleep to prevent multiple activations
                        Sleep(200);
                    }
                }
            }
            
            // Process other hotkeys (2, 3, 4, 5, 6) independently
            if ((GetAsyncKeyState('2') & 0x8000) && base) {
                // Record positions
                uintptr_t xAddr1 = ResolvePointer(base, EFZ_BASE_OFFSET_P1, XPOS_OFFSET);
                uintptr_t yAddr1 = ResolvePointer(base, EFZ_BASE_OFFSET_P1, YPOS_OFFSET);
                uintptr_t xAddr2 = ResolvePointer(base, EFZ_BASE_OFFSET_P2, XPOS_OFFSET);
                uintptr_t yAddr2 = ResolvePointer(base, EFZ_BASE_OFFSET_P2, YPOS_OFFSET);
                
                if (xAddr1 && yAddr1 && xAddr2 && yAddr2) {
                    // Read current positions with explicit error checking
                    if (!SafeReadMemory(xAddr1, &recordedX1, sizeof(double))) {
                        LogOut("[POSITION] Failed to read P1 X position", true);
                    }
                    if (!SafeReadMemory(yAddr1, &recordedY1, sizeof(double))) {
                        LogOut("[POSITION] Failed to read P1 Y position", true);
                    }
                    if (!SafeReadMemory(xAddr2, &recordedX2, sizeof(double))) {
                        LogOut("[POSITION] Failed to read P2 X position", true);
                    }
                    if (!SafeReadMemory(yAddr2, &recordedY2, sizeof(double))) {
                        LogOut("[POSITION] Failed to read P2 Y position", true);
                    }
                    
                    // Log recorded values explicitly
                    LogOut("[POSITION] Recorded P1 X: " + std::to_string(recordedX1), true);
                    LogOut("[POSITION] Recorded P1 Y: " + std::to_string(recordedY1), true);
                    LogOut("[POSITION] Recorded P2 X: " + std::to_string(recordedX2), true);
                    LogOut("[POSITION] Recorded P2 Y: " + std::to_string(recordedY2), true);
                    
                    hasRecorded = true;
                    LogOut("[POSITION] Positions recorded successfully", true);
                    
                    // Sleep to prevent multiple recordings from single press
                    Sleep(200);
                } else {
                    LogOut("[POSITION] Failed to resolve position pointers for recording", true);
                }
            }
            
            // Hotkey 3: Open menu
            if ((GetAsyncKeyState('3') & 0x8000)) {
                if (!menuOpen) {
                    OpenMenu();
                    // Sleep to prevent multiple menu opens
                    Sleep(200);
                }
            }
            
            // Hotkey 4: Toggle title display mode and detailed debug output
            if ((GetAsyncKeyState('4') & 0x8000)) {
                detailedTitleMode = !detailedTitleMode;
                detailedDebugOutput.store(detailedTitleMode.load()); // Link both settings together
                
                LogOut("[DISPLAY] " + 
                       std::string(detailedTitleMode ? "Detailed" : "Normal") + 
                       " display mode enabled", true);
                if (detailedTitleMode) {
                    LogOut("[DISPLAY] Showing detailed debug information (hitstun, positions, frame data)", true);
                } else {
                    LogOut("[DISPLAY] Hiding detailed debug information", true);
                }
                
                // Sleep to prevent multiple activations
                Sleep(200);
            }
            
            // Hotkey 5: Reset frame counter
            if ((GetAsyncKeyState('5') & 0x8000)) {
                ResetFrameCounter();
                LogOut("[FRAME] Frame counter reset", true);
                // Sleep to prevent multiple resets
                Sleep(200);
            }
            
            // Hotkey 6: Show help
            if ((GetAsyncKeyState('6') & 0x8000)) {
                ShowHotkeyInfo();
                // Sleep to prevent multiple help displays
                Sleep(200);
            }
            
            // Debug toggle (Ctrl+D)
            if ((GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState('D') & 0x8000)) {
                detailedLogging = !detailedLogging;
                LogOut("[DEBUG] Detailed logging " + 
                      std::string(detailedLogging ? "enabled" : "disabled"), true);
                
                // Sleep to prevent multiple toggles
                Sleep(200);
            }
        }
        
        // Add this block to enable advanced key logging
        if ((GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_SHIFT) & 0x8000) && (GetAsyncKeyState('D') & 0x8000)) {
            enableDebugLogging = !enableDebugLogging;
            LogOut("[DEBUG] Advanced key logging " + std::string(enableDebugLogging ? "enabled" : "disabled"), true);
            Sleep(300);
        }
        
        // Log key states and game inputs for debugging
        if (enableDebugLogging) {
            // Log all key states
            std::stringstream keyLog;
            keyLog << "[KEYS] ";
            for (int vk = 0x01; vk <= 0x5F; vk++) {
                if (GetAsyncKeyState(vk) & 0x8000) {
                    keyLog << "VK" << std::hex << vk << " ";
                }
            }
            LogOut(keyLog.str(), true);
            
            // Log game inputs separately
            uint8_t p1Input = GetPlayerInputs(1);
            uint8_t p2Input = GetPlayerInputs(2);
            LogOut("[INPUTS] P1: " + std::to_string(p1Input) + " P2: " + std::to_string(p2Input), true);
        }
        
        // Sleep to reduce CPU usage
        Sleep(10);
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
        { 0, "Down", &detectedBindings.downKey },
        { 2, "Up", &detectedBindings.upKey },
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
    // First try an exact match with a DirectInput key code
    for (const auto& mapping : KeyMappings) {
        if (mapping.dikCode == efzKey) {
            return mapping.vkCode;
        }
    }
    
    // Special cases for EFZ-specific codes
    switch (efzKey) {
        case 0xCB: return VK_LEFT;      // DIK_LEFT
        case 0xCD: return VK_RIGHT;     // DIK_RIGHT
        case 0xC8: return VK_UP;        // DIK_UP
        case 0xD0: return VK_DOWN;      // DIK_DOWN
        case 0x2C: return 'Z';          // DIK_Z
        case 0x2D: return 'X';          // DIK_X
        case 0x2E: return 'C';          // DIK_C
        case 0x2F: return 'V';          // DIK_V
        case 0x1E: return 'A';          // DIK_A
        case 0x1F: return 'S';          // DIK_S
        case 0x20: return 'D';          // DIK_D
        case 0x21: return 'F';          // DIK_F
        case 0x39: return VK_SPACE;     // DIK_SPACE
        case 0x1C: return VK_RETURN;    // DIK_RETURN
        
        // Handle specific joystick buttons based on config_EN.exe
        case 0x13:  return VK_LEFT;     // Joystick Left
        case 0x114: return VK_RIGHT;    // Joystick Right
        case 0x111: return VK_UP;       // Joystick Up
        case 0x112: return VK_DOWN;     // Joystick Down
        
        default:
            return efzKey & 0xFF; // Take the low byte as a character code
    }
}

// Forward-declared in gui.h, implemented here for convenience
void ShowConfigMenu() {
    // Initialize common controls
    INITCOMMONCONTROLSEX icc;
    icc.dwSize = sizeof(INITCOMMONCONTROLSEX);
    icc.dwICC = ICC_TAB_CLASSES;
    InitCommonControlsEx(&icc);
    
    // Check if EFZ window is active
    if (!IsEFZWindowActive()) {
        LogOut("[GUI] Cannot open menu: EFZ window not active", true);
        return;
    }
    
    // Don't open menu if already open
    if (menuOpen) {
        return;
    }
    
    menuOpen = true;
    LogOut("[GUI] Opening config menu", true);
    
    // Get the game window and show the dialog
    HWND hwnd = FindWindowA(NULL, "Eternal Fighter Zero");
    if (!hwnd) {
        hwnd = GetConsoleWindow();
    }
    
    if (hwnd) {
        ShowEditDataDialog(hwnd);
    } else {
        LogOut("[MENU] Could not find game window for menu display", true);
        menuOpen = false;
    }
}

void DetectKeyBindings() {
    // Try to read mappings from key.ini first
    bool iniMappingsFound = ReadKeyMappingsFromIni();
    
    if (!iniMappingsFound) {
        // If no ini mappings found, use DirectInput detection
        if (g_directInputAvailable) {
            DetectKeyBindingsWithDI();
        }
        // Otherwise, as a fallback, use Windows API detection
        else {
            LogOut("[INPUT] Using WinAPI for key detection", true);
        }
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
                    
                    if ((pov >= 31500 || pov <= 4500) && (prevPov < 31500 && prevPov > 4500)) gpUpPressed = true;
                    if ((pov >= 4500 && pov <= 13500) && (prevPov < 4500 || prevPov > 13500)) gpRightPressed = true;
                    if ((pov >= 13500 && pov <= 22500) && (prevPov < 13500 || prevPov > 22500)) gpDownPressed = true;
                    if ((pov >= 22500 && pov <= 31500) && (prevPov < 22500 || prevPov > 31500)) gpLeftPressed = true;
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