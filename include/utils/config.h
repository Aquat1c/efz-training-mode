#pragma once
#include <windows.h>
#include <string>
#include <unordered_map>

namespace Config {
    struct Settings {
        // General settings
        bool useImGui;
        bool detailedLogging;
        bool restrictToPracticeMode; // NEW: Restrict to practice mode
    bool enableConsole;          // NEW: Show/Hide console window
    bool enableFpsDiagnostics;   // NEW: Enable FPS/timing diagnostics output
    bool enableCharacterSelectLogger; // NEW: Toggle per-frame Character Select flag logger
    float uiScale;               // NEW: UI scale for ImGui window (e.g., 0.80..1.20)
    int uiFontMode;              // NEW: UI font selection (0=ImGui default, 1=Segoe UI)

    // Virtual cursor (software cursor driven by gamepad) settings
    bool enableVirtualCursor;          // Master enable/disable
    bool virtualCursorAllowWindowed;   // Allow usage when not fullscreen
    float virtualCursorBaseSpeed;      // Base movement speed (px/sec)
    float virtualCursorFastSpeed;      // Fast (shoulder) speed (px/sec)
    float virtualCursorDpadSpeed;      // Dpad nudge speed (px/sec)
    float virtualCursorAccelPower;     // Analog curve exponent (1.0 = linear, >1 slower near center)

    // (Practice tuning removed)
        
        // Hotkey settings
        int teleportKey;
        int recordKey;
        int configMenuKey;
        int toggleTitleKey;
        int resetFrameCounterKey;
        int helpKey;
        int toggleImGuiKey;

        // Practice/macro hotkeys (configurable)
        int switchPlayersKey;   // Default: 'L'
        int macroRecordKey;     // Default: 'I'
        int macroPlayKey;       // Default: 'O'
        int macroSlotKey;       // NEW: Cycle macro slot (Default: 'K')

        // ImGui footer action hotkeys (keyboard access without cursor)
        int uiAcceptKey;        // Apply changes
        int uiRefreshKey;       // Refresh values
        int uiExitKey;          // Close menu

        // Gamepad binding settings (XInput button bitmasks; -1 = disabled)
        int gpTeleportButton;       // Default: XINPUT_GAMEPAD_BACK
        int gpSavePositionButton;   // Default: XINPUT_GAMEPAD_LEFT_THUMB
    int gpSwitchPlayersButton;  // Default: XINPUT_GAMEPAD_RIGHT_SHOULDER (RB)
    // ABXY freed for UI confirm/back: new defaults avoid using A/B/X/Y
    int gpSwapPositionsButton;  // Default: XINPUT_GAMEPAD_RIGHT_THUMB (R3)
    int gpMacroRecordButton;    // Default: XINPUT_GAMEPAD_LEFT_SHOULDER (LB)
    int gpMacroPlayButton;      // Default: RT (virtual trigger 0x20000)
    int gpMacroSlotButton;      // Default: LT (virtual trigger 0x10000)
        int gpToggleMenuButton;     // Default: XINPUT_GAMEPAD_START
        int gpToggleImGuiButton;    // Default: -1 (disabled)

        // RF freeze behavior
        bool freezeRFAfterContRec;       // Start RF freeze after Continuous Recovery enforcement
        bool freezeRFOnlyWhenNeutral;    // Maintain RF freeze only while in neutral states
    };
    
    // Initialize configuration system
    bool Initialize();
    
    // Load settings from ini file
    bool LoadSettings();
    
    // Save settings to ini file
    bool SaveSettings();
    
    // Create default config file if none exists
    bool CreateDefaultConfig();
    
    // Get settings
    const Settings& GetSettings();
    
    // Set a specific setting
    void SetSetting(const std::string& section, const std::string& key, const std::string& value);
    
    // Get the path to the config file
    std::string GetConfigFilePath();
    
    // Helper methods
    int ParseKeyValue(const std::string& value);
    std::string GetKeyName(int keyCode);
    int ParseGamepadButton(const std::string& value); // parse textual/hex controller button
    std::string GetGamepadButtonName(int mask);       // friendly name for controller button
    
    // Get the current working directory
    std::string GetCurrentWorkingDirectory();
}