#pragma once
#include <windows.h>
#include <string>
#include <unordered_map>

namespace Config {
    struct Settings {
        // General settings
        bool useImGui;
        bool detailedLogging;
        bool enableDebugFileLog;   // NEW: Enable writing efz_training_debug.log (file debug logging)
        bool restrictToPracticeMode; // NEW: Restrict to practice mode
    bool enableConsole;          // NEW: Show/Hide console window
    bool enableFpsDiagnostics;   // NEW: Enable FPS/timing diagnostics output
    bool enableCharacterSelectLogger; // NEW: Toggle per-frame Character Select flag logger
    bool showPracticeEntryHint;   // NEW: Show practice overlay hint once per session
    float uiScale;               // NEW: UI scale for ImGui window (e.g., 0.80..1.20)
    int uiFontMode;              // NEW: UI font selection (0=ImGui default, 1=Segoe UI)

    // ImGui navigation tuning
    float guiNavAnalogThreshold; // Analog threshold (0..1) to treat stick as a digital dpad for fallback nav
    float guiNavRepeatDelay;     // Seconds before key repeat starts for nav
    float guiNavRepeatRate;      // Seconds between repeats once repeating

    // ImGui scrolling via right stick
    bool  guiScrollRightStickEnable; // Enable right-stick to mouse-wheel mapping in GUI
    float guiScrollRightStickScale;  // Notches per second at full tilt (vertical & horizontal)

    // Virtual cursor (software cursor driven by gamepad) settings
    bool enableVirtualCursor;          // Master enable/disable
    bool virtualCursorAllowWindowed;   // Allow usage when not fullscreen
    float virtualCursorBaseSpeed;      // Base movement speed (px/sec)
    float virtualCursorFastSpeed;      // Fast (shoulder) speed (px/sec)
    float virtualCursorDpadSpeed;      // Dpad nudge speed (px/sec)
    float virtualCursorAccelPower;     // Analog curve exponent (1.0 = linear, >1 slower near center)

    // Controller selection
    // -1 = All controllers, 0..3 = specific XInput user index
    int controllerIndex;

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
    // Swap Positions custom binding (simple)
    bool swapCustomEnabled;     // Enable dedicated swap key
    int  swapCustomKey;         // VK for dedicated swap key

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

    // UI navigation/controller bindings (rebindable)
    // Top-level tabs cycle (logical order: Main, Auto Action, Settings, Character, Help)
    int gpUiTopTabPrev;         // Default: LB
    int gpUiTopTabNext;         // Default: RB
    // Active sub-tab cycle within current tab (e.g., Help pages, Main Menu sub-tabs)
    int gpUiSubTabPrev;         // Default: LT
    int gpUiSubTabNext;         // Default: RT

        // RF freeze behavior
        bool freezeRFAfterContRec;       // Start RF freeze after Continuous Recovery enforcement
        bool freezeRFOnlyWhenNeutral;    // Maintain RF freeze only while in neutral states

        // Continuous Recovery gating
        bool crRequireBothNeutral;       // Require BOTH players to be neutral before CR applies
        int  crBothNeutralDelayMs;       // Delay after both-neutral detected before applying CR

        // Auto-fix HP anomalies
        bool autoFixHPOnNeutral;         // If neutral and HP<=0, set HP to 9999 automatically

        // Frame Advantage display duration (in seconds)
        float frameAdvantageDisplayDuration; // How long to show FA/RG messages (default: 8.0)

        // Practice: Dummy Auto-Block behavior
        // Continuous neutral timeout used by event-driven modes (ms). Defaults to 10000 (10s).
        int autoBlockNeutralTimeoutMs;
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