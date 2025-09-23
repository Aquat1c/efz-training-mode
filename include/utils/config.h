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
    float uiScale;               // NEW: UI scale for ImGui window (e.g., 0.80..1.20)
    int uiFontMode;              // NEW: UI font selection (0=ImGui default, 1=Segoe UI)

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
    
    // Get the current working directory
    std::string GetCurrentWorkingDirectory();
}