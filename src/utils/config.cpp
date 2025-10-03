#include "../include/utils/config.h"
#include "../include/core/logger.h"
#include "../include/utils/utilities.h"

#include <fstream>
#include <sstream>
#include <filesystem>
#include <algorithm>
#include <cctype>
#include <map>
#include <xinput.h>

namespace Config {
    // Internal settings storage
    static Settings settings;
    // Initialize defaults for safety
    // Note: remaining fields are populated by LoadSettings/CreateDefaultConfig
    
    
    // Path to config file
    static std::string configFilePath;
    
    // Internal representation of the ini file
    static std::unordered_map<std::string, std::unordered_map<std::string, std::string>> iniData;
    
    // Forward declare helper methods
    void SetIniValue(const std::string& section, const std::string& key, const std::string& value);
    bool GetValueBool(const std::string& section, const std::string& key, bool defaultValue);
    int GetValueInt(const std::string& section, const std::string& key, int defaultValue);
    bool LoadIniFromFile();
    static std::string ToLower(std::string s) { std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c){ return (char)std::tolower(c); }); return s; }
    
    // Helper method implementations
    void SetIniValue(const std::string& section, const std::string& key, const std::string& value) {
        // Normalize to lowercase section/key for consistent lookups
        iniData[ToLower(section)][ToLower(key)] = value;
    }
    
    bool GetValueBool(const std::string& section, const std::string& key, bool defaultValue) {
        auto sectionIt = iniData.find(ToLower(section));
        if (sectionIt != iniData.end()) {
            auto keyIt = sectionIt->second.find(ToLower(key));
            if (keyIt != sectionIt->second.end()) {
                return keyIt->second == "1" || keyIt->second == "true" || keyIt->second == "yes";
            }
        }
        return defaultValue;
    }
    
    int GetValueInt(const std::string& section, const std::string& key, int defaultValue) {
        auto sectionIt = iniData.find(ToLower(section));
        if (sectionIt != iniData.end()) {
            auto keyIt = sectionIt->second.find(ToLower(key));
            if (keyIt != sectionIt->second.end()) {
                return ParseKeyValue(keyIt->second);
            }
        }
        return defaultValue;
    }
    
    bool Initialize() {
        // Get the DLL path, not the executable path
        char modulePath[MAX_PATH] = {0};
        
        // Log the start of config initialization with timestamp
        auto now = std::chrono::system_clock::now();
        auto time_t_now = std::chrono::system_clock::to_time_t(now);
        char timeBuffer[26];
        ctime_s(timeBuffer, sizeof(timeBuffer), &time_t_now);
        std::string timestamp(timeBuffer);
        timestamp.pop_back(); // Remove trailing newline
        
        LogOut("[CONFIG] Initializing config system at " + timestamp, true);
        LogOut("[CONFIG] Current working directory: " + GetCurrentWorkingDirectory(), true);
        
        // Try multiple methods to get our module path
        HMODULE hModule = GetModuleHandleA("efz_training_mode.dll");
        if (hModule) {
            DWORD result = GetModuleFileNameA(hModule, modulePath, MAX_PATH);
            if (result > 0) {
                LogOut("[CONFIG] Found module path via GetModuleHandleA: " + std::string(modulePath), true);
            } else {
                LogOut("[CONFIG] GetModuleFileNameA failed with error: " + std::to_string(GetLastError()), true);
            }
        } else {
            LogOut("[CONFIG] GetModuleHandleA failed to find efz_training_mode.dll, error: " + std::to_string(GetLastError()), true);
            
            // Try with NULL to get executable path
            if (GetModuleFileNameA(NULL, modulePath, MAX_PATH) > 0) {
                LogOut("[CONFIG] Using executable path instead: " + std::string(modulePath), true);
            } else {
                LogOut("[CONFIG] GetModuleFileNameA(NULL) failed with error: " + std::to_string(GetLastError()), true);
            }
        }
        
        try {
            // Extract directory path
            std::filesystem::path basePath(modulePath);
            std::filesystem::path dirPath = basePath.parent_path();
            LogOut("[CONFIG] Base directory path: " + dirPath.string(), true);
            
            // Create config path in same directory as DLL/executable
            std::filesystem::path configPath = dirPath / "efz_training_config.ini";
            LogOut("[CONFIG] Attempting to use config path: " + configPath.string(), true);
            
            configFilePath = configPath.string();
            
            // Test if we can write to this location with detailed error reporting
            {
                LogOut("[CONFIG] Testing write permissions...", true);
                std::ofstream testFile(configPath, std::ios::app);
                if (testFile) {
                    LogOut("[CONFIG] File location is writable", true);
                    testFile.close();
                } else {
                    LogOut("[CONFIG] Cannot write to file location, error: " + std::to_string(errno), true);
                    
                    // Get error message
                    char errMsg[256];
                    strerror_s(errMsg, sizeof(errMsg), errno);
                    LogOut("[CONFIG] Error message: " + std::string(errMsg), true);
                    
                    // Try current directory as fallback
                    char currentDir[MAX_PATH];
                    GetCurrentDirectoryA(MAX_PATH, currentDir);
                    LogOut("[CONFIG] Falling back to current directory: " + std::string(currentDir), true);
                    
                    configPath = std::filesystem::path(currentDir) / "efz_training_config.ini";
                    configFilePath = configPath.string();
                    LogOut("[CONFIG] Fallback config path: " + configFilePath, true);
                    
                    // Test the fallback location
                    std::ofstream testFile2(configPath, std::ios::app);
                    if (testFile2) {
                        LogOut("[CONFIG] Fallback location is writable", true);
                        testFile2.close();
                    } else {
                        LogOut("[CONFIG] Cannot write to fallback location either, error: " + std::to_string(errno), true);
                        strerror_s(errMsg, sizeof(errMsg), errno);
                        LogOut("[CONFIG] Error message: " + std::string(errMsg), true);
                    }
                }
            }
            
            // Check if config file exists, create if not
            LogOut("[CONFIG] Checking if config file exists at: " + configFilePath, true);
            bool fileExists = std::filesystem::exists(configPath);
            LogOut("[CONFIG] File exists: " + std::string(fileExists ? "YES" : "NO"), true);
            
            if (!fileExists) {
                LogOut("[CONFIG] Config file does not exist, creating default...", true);
                if (!CreateDefaultConfig()) {
                    LogOut("[CONFIG] Failed to create default config file", true);
                    return false;
                }
            }
            else {
                if (std::filesystem::file_size(configPath) == 0) {
                    LogOut("[CONFIG] Config file exists but is empty, creating default content", true);
                    if (!CreateDefaultConfig()) {
                        LogOut("[CONFIG] Failed to create default config file", true);
                        return false;
                    }
                } else {
                    LogOut("[CONFIG] Config file exists with size: " + std::to_string(std::filesystem::file_size(configPath)) + " bytes", true);
                }
            }
            
            // Parse INI then load settings
            if (!LoadIniFromFile()) {
                LogOut("[CONFIG] Failed to parse ini file, continuing with defaults", true);
            }
            bool loadResult = LoadSettings();
            LogOut("[CONFIG] LoadSettings result: " + std::to_string(loadResult), true);
            return loadResult;
        }
        catch (const std::exception& e) {
            LogOut("[CONFIG] Exception in Initialize: " + std::string(e.what()), true);
            return false;
        }
    }
    
    // Add this helper function to get the current working directory
    std::string GetCurrentWorkingDirectory() {
        char currentDir[MAX_PATH];
        if (GetCurrentDirectoryA(MAX_PATH, currentDir)) {
            return std::string(currentDir);
        }
        return "Unknown";
    }
    
    bool CreateDefaultConfig() {
        LogOut("[CONFIG] Creating default config file at: " + configFilePath, true);
        
        try {
            std::ofstream file(configFilePath);
            if (!file.is_open()) {
                LogOut("[CONFIG] Error: Could not open config file for writing.", true);
                return false;
            }
            
            file << "[General]\n";
            file << "; Use the modern ImGui interface (1) or the legacy Win32 dialog (0)\n";
            file << "useImGui = 1\n";
            file << "; Enable detailed debug messages in the console (1 = yes, 0 = no)\n";
            file << "detailedLogging = 0\n";
            file << "; Show the debug console window (1 = yes, 0 = no)\n";
            file << "enableConsole = 0\n";
            file << "; Restrict functionality to Practice Mode only (1 = yes, 0 = no)\n";
            file << "restrictToPracticeMode = 1\n\n";
            file << "; Enable FPS/timing diagnostics in logs (1 = yes, 0 = no)\n";
            file << "enableFpsDiagnostics = 0\n\n";
            file << "; Log active player / CPU flags during Character Select (1 = yes, 0 = no)\n";
            file << "enableCharacterSelectLogger = 1\n\n";

            file << "; UI scale for ImGui window (0.80 - 1.20 recommended)\n";
            file << "uiScale = 0.90\n\n";
            file << "; UI font: 0 = ImGui default font, 1 = Segoe UI (Windows)\n";
            file << "uiFont = 0\n\n";

            file << "; Virtual Cursor (software controller-driven cursor) settings\n";
            file << "; Master enable (1=on,0=off)\n";
            file << "enableVirtualCursor = 1\n";
            file << "; Allow virtual cursor while windowed (1=yes,0=only fullscreen)\n";
            file << "virtualCursorAllowWindowed = 0\n";
            file << "; Movement speeds in pixels/second (base, fast/shoulder, dpad nudge)\n";
            file << "virtualCursorBaseSpeed = 900\n";
            file << "virtualCursorFastSpeed = 1800\n";
            file << "virtualCursorDpadSpeed = 700\n";
            file << "; Analog stick acceleration exponent (1.0=linear, 1.5=gentler near center)\n";
            file << "virtualCursorAccelPower = 1.35\n\n";

            // (Practice-specific tuning is hardcoded now)
            
            file << "[Hotkeys]\n";
            file << "; Use virtual-key codes (hexadecimal, e.g., 0x70 for F1)\n";
            file << "; See key code definitions at: https://docs.microsoft.com/en-us/windows/win32/inputdev/virtual-key-codes\n";
            file << "; Common keys: \n";
            file << "; - 0x31-0x39: Number keys 1-9\n";
            file << "; - 0x41-0x5A: Letter keys A-Z\n";
            file << "; - 0x70-0x7B: Function keys F1-F12\n\n";
            
            file << "; Teleport players to recorded positions\n";
            file << "TeleportKey=0x31    # Default: '1' key\n\n";
            
            file << "; Record current player positions\n";
            file << "RecordKey=0x32      # Default: '2' key\n\n";
            
            file << "; Open config menu\n";
            file << "ConfigMenuKey=0x33  # Default: '3' key\n\n";
            
            file << "; Toggle title display mode\n";
            file << "ToggleTitleKey=0x34 # Default: '4' key\n\n";
            
            file << "; Reset frame counter\n";
            file << "ResetFrameCounterKey=0x35 # Default: '5' key\n\n";
            
            file << "; Show help and clear console\n";
            file << "HelpKey=0x36        # Default: '6' key\n\n";
            
            file << "; Toggle ImGui overlay\n";
            file << "ToggleImGuiKey=0x37 # Default: '7' key\n";

            file << "\n; Practice: Switch Players toggle (Practice only)\n";
            file << "SwitchPlayersKey=0x4C  # Default: 'L' key\n";
            file << "; Macro: Record (two-press)\n";
            file << "MacroRecordKey=0x49     # Default: 'I' key\n";
            file << "; Macro: Play (replay)\n";
            file << "MacroPlayKey=0x4F       # Default: 'O' key\n";
            file << "; Macro: Cycle Slot (next)\n";
            file << "MacroSlotKey=0x4B       # Default: 'K' key\n";
            file << "\n; UI footer actions (Apply / Refresh / Exit)\n";
            file << "; Avoid using in-game bound keys (Enter/Escape/Space). Defaults: E, R, Q.\n";
            file << "UIAcceptKey=0x45        # 'E' (Apply)\n";
            file << "UIRefreshKey=0x52       # 'R' (Refresh)\n";
            file << "UIExitKey=0x51          # 'Q' (Exit)\n";

            // --- Gamepad binding defaults ---
            file << "\n; Controller bindings (XInput) \n";
            file << "; Use symbolic names (e.g. A, B, X, Y, LB, RB, BACK, START, L3, R3, DPAD_UP, DPAD_DOWN, DPAD_LEFT, DPAD_RIGHT) or hex mask (e.g. 0x2000). -1 disables.\n";
            file << "; Triggers (LT, RT) are treated as virtual buttons (threshold based).\n";
            file << "gpTeleportButton=BACK\n";       // Teleport
            file << "gpSavePositionButton=L3\n";     // Save current positions
            file << "gpSwitchPlayersButton=RB\n";    // Toggle local side (practice)
            // ABXY reserved for UI (A=confirm, B=back). Use shoulders/triggers/sticks instead.
            file << "gpSwapPositionsButton=R3\n";    // Swap player coordinates (was Y)
            file << "gpMacroRecordButton=LB\n";      // Macro record (was X)
            file << "gpMacroPlayButton=RT\n";        // Macro play (was A)
            file << "gpMacroSlotButton=LT\n";        // Cycle macro slot (was B)
            file << "gpToggleMenuButton=START\n";    // Open training menu
            file << "gpToggleImGuiButton=-1\n";      // Toggle overlay (disabled by default)
            
            file.close();
            
            if (file.fail()) {
                char errMsg[256];
                strerror_s(errMsg, sizeof(errMsg), errno);
                LogOut("[CONFIG] Error occurred while closing file: " + std::string(errMsg), true);
                return false;
            }
            
            // Verify the file was created
            if (std::filesystem::exists(configFilePath)) {
                LogOut("[CONFIG] Default config file created successfully, size: " + 
                       std::to_string(std::filesystem::file_size(configFilePath)) + " bytes", true);
                
                // Print first few lines of the file to confirm content
                std::ifstream verifyFile(configFilePath);
                if (verifyFile) {
                    std::string line;
                    int lineCount = 0;
                    LogOut("[CONFIG] File content sample:", true);
                    while (std::getline(verifyFile, line) && lineCount < 5) {
                        LogOut("[CONFIG] " + line, true);
                        lineCount++;
                    }
                    verifyFile.close();
                }
                // Refresh in-memory iniData from defaults we just wrote
                LoadIniFromFile();
                return true;
            } else {
                LogOut("[CONFIG] File creation appeared to succeed, but file does not exist!", true);
                return false;
            }
        }
        catch (const std::exception& e) {
            LogOut("[CONFIG] Exception creating default config: " + std::string(e.what()), true);
            return false;
        }
    }
    
    bool LoadSettings() {
        LogOut("[CONFIG] Loading settings from: " + configFilePath, true);
        // Always refresh iniData from disk before reading
        LoadIniFromFile();
        
        try {
            settings.useImGui = GetValueBool("General", "useImGui", true);
            settings.detailedLogging = GetValueBool("General", "detailedLogging", false);
            settings.enableConsole = GetValueBool("General", "enableConsole", false);
            settings.restrictToPracticeMode = GetValueBool("General", "restrictToPracticeMode", true);
            settings.enableFpsDiagnostics = GetValueBool("General", "enableFpsDiagnostics", false);
            // Default ON so older configs without this key enable it automatically
            settings.enableCharacterSelectLogger = GetValueBool("General", "enableCharacterSelectLogger", true);
            {
                // Clamp scale to a sensible range
                int raw = 0; // we parse as int/float via string later; reuse GetValueInt if needed
                auto sectionIt = iniData.find("general");
                float scale = 0.90f;
                if (sectionIt != iniData.end()) {
                    auto keyIt = sectionIt->second.find("uiscale");
                    if (keyIt != sectionIt->second.end()) {
                        try {
                            scale = std::stof(keyIt->second);
                        } catch (...) { scale = 0.90f; }
                    }
                }
                if (scale < 0.70f) scale = 0.70f;
                if (scale > 1.50f) scale = 1.50f;
                settings.uiScale = scale;
            }

            // Load UI font mode (0=default, 1=Segoe UI)
            settings.uiFontMode = 0;
            {
                auto sectionIt = iniData.find("general");
                if (sectionIt != iniData.end()) {
                    auto keyIt = sectionIt->second.find("uifont");
                    if (keyIt != sectionIt->second.end()) {
                        try { settings.uiFontMode = std::stoi(keyIt->second); } catch (...) { settings.uiFontMode = 0; }
                        if (settings.uiFontMode < 0 || settings.uiFontMode > 1) settings.uiFontMode = 0;
                    }
                }
            }

            // Practice: no runtime-tunable settings
            // Virtual cursor settings
            settings.enableVirtualCursor = GetValueBool("General", "enableVirtualCursor", true);
            settings.virtualCursorAllowWindowed = GetValueBool("General", "virtualCursorAllowWindowed", false);
            settings.virtualCursorBaseSpeed = (float)GetValueInt("General", "virtualCursorBaseSpeed", 900);
            settings.virtualCursorFastSpeed = (float)GetValueInt("General", "virtualCursorFastSpeed", 1800);
            settings.virtualCursorDpadSpeed = (float)GetValueInt("General", "virtualCursorDpadSpeed", 700);
            {
                auto sectionIt = iniData.find("general");
                float p = 1.35f;
                if (sectionIt != iniData.end()) {
                    auto keyIt = sectionIt->second.find("virtualcursoraccelpower");
                    if (keyIt != sectionIt->second.end()) {
                        try { p = std::stof(keyIt->second); } catch (...) { p = 1.35f; }
                    }
                }
                if (p < 0.5f) p = 0.5f; if (p > 3.0f) p = 3.0f;
                settings.virtualCursorAccelPower = p;
            }
            
            // Hotkey settings - REVERTED to number key defaults
            settings.teleportKey = GetValueInt("Hotkeys", "TeleportKey", 0x31);          // Default: '1'
            settings.recordKey = GetValueInt("Hotkeys", "RecordKey", 0x32);            // Default: '2'
            settings.configMenuKey = GetValueInt("Hotkeys", "ConfigMenuKey", 0x33);      // Default: '3'
            settings.toggleTitleKey = GetValueInt("Hotkeys", "ToggleTitleKey", 0x34);     // Default: '4'
            settings.resetFrameCounterKey = GetValueInt("Hotkeys", "ResetFrameCounterKey", 0x35); // Default: '5'
            settings.helpKey = GetValueInt("Hotkeys", "HelpKey", 0x36);                // Default: '6'
            settings.toggleImGuiKey = GetValueInt("Hotkeys", "ToggleImGuiKey", 0x37);      // Default: '7'            
            // Additional configurable hotkeys
            settings.switchPlayersKey = GetValueInt("Hotkeys", "SwitchPlayersKey", 0x4C); // 'L'
            settings.macroRecordKey   = GetValueInt("Hotkeys", "MacroRecordKey",   0x49); // 'I'
            settings.macroPlayKey     = GetValueInt("Hotkeys", "MacroPlayKey",     0x4F); // 'O'
            settings.macroSlotKey     = GetValueInt("Hotkeys", "MacroSlotKey",     0x4B); // 'K'
            settings.uiAcceptKey      = GetValueInt("Hotkeys", "UIAcceptKey",     0x45); // 'E'
            settings.uiRefreshKey     = GetValueInt("Hotkeys", "UIRefreshKey",    0x52); // 'R'
            settings.uiExitKey        = GetValueInt("Hotkeys", "UIExitKey",       0x51); // 'Q'
            // Gamepad bindings (defaults mirror CreateDefaultConfig)
            auto getPad = [&](const char* name, const char* defStr){
                auto sectionIt = iniData.find("hotkeys");
                if (sectionIt != iniData.end()) {
                    auto keyIt = sectionIt->second.find(std::string(name));
                    if (keyIt != sectionIt->second.end()) return ParseGamepadButton(keyIt->second);
                }
                return ParseGamepadButton(defStr);
            };
            settings.gpTeleportButton       = getPad("gpteleportbutton", "BACK");
            settings.gpSavePositionButton   = getPad("gpsavepositionbutton", "L3");
            settings.gpSwitchPlayersButton  = getPad("gpswitchplayersbutton", "RB");
            // New non-ABXY defaults
            settings.gpSwapPositionsButton  = getPad("gpswappositionsbutton", "R3");
            settings.gpMacroRecordButton    = getPad("gpmacrorecordbutton", "LB");
            settings.gpMacroPlayButton      = getPad("gpmacroplaybutton", "RT");
            settings.gpMacroSlotButton      = getPad("gpmacroslotbutton", "LT");
            settings.gpToggleMenuButton     = getPad("gptogglemenubutton", "START");
            settings.gpToggleImGuiButton    = getPad("gptoggleimguibutton", "-1");
            LogOut("[CONFIG] Settings loaded successfully", true);
            LogOut("[CONFIG] UseImGui: " + std::to_string(settings.useImGui), true);
            LogOut("[CONFIG] DetailedLogging: " + std::to_string(settings.detailedLogging), true);
            LogOut("[CONFIG] EnableConsole: " + std::to_string(settings.enableConsole), true);
            LogOut("[CONFIG] TeleportKey: " + std::to_string(settings.teleportKey) + " (" + GetKeyName(settings.teleportKey) + ")", true);
            LogOut("[CONFIG] RecordKey: " + std::to_string(settings.recordKey) + " (" + GetKeyName(settings.recordKey) + ")", true);
            LogOut("[CONFIG] ConfigMenuKey: " + std::to_string(settings.configMenuKey) + " (" + GetKeyName(settings.configMenuKey) + ")", true);
            LogOut("[CONFIG] ToggleTitleKey: " + std::to_string(settings.toggleTitleKey) + " (" + GetKeyName(settings.toggleTitleKey) + ")", true);
            LogOut("[CONFIG] ResetFrameCounterKey: " + std::to_string(settings.resetFrameCounterKey) + " (" + GetKeyName(settings.resetFrameCounterKey) + ")", true);
            LogOut("[CONFIG] HelpKey: " + std::to_string(settings.helpKey) + " (" + GetKeyName(settings.helpKey) + ")", true);
            LogOut("[CONFIG] ToggleImGuiKey: " + std::to_string(settings.toggleImGuiKey) + " (" + GetKeyName(settings.toggleImGuiKey) + ")", true);
            LogOut("[CONFIG] SwitchPlayersKey: " + std::to_string(settings.switchPlayersKey) + " (" + GetKeyName(settings.switchPlayersKey) + ")", true);
            LogOut("[CONFIG] MacroRecordKey: " + std::to_string(settings.macroRecordKey) + " (" + GetKeyName(settings.macroRecordKey) + ")", true);
            LogOut("[CONFIG] MacroPlayKey: " + std::to_string(settings.macroPlayKey) + " (" + GetKeyName(settings.macroPlayKey) + ")", true);
            LogOut("[CONFIG] MacroSlotKey: " + std::to_string(settings.macroSlotKey) + " (" + GetKeyName(settings.macroSlotKey) + ")", true);
            LogOut("[CONFIG] UIAcceptKey: " + std::to_string(settings.uiAcceptKey) + " (" + GetKeyName(settings.uiAcceptKey) + ")", true);
            LogOut("[CONFIG] UIRefreshKey: " + std::to_string(settings.uiRefreshKey) + " (" + GetKeyName(settings.uiRefreshKey) + ")", true);
            LogOut("[CONFIG] UIExitKey: " + std::to_string(settings.uiExitKey) + " (" + GetKeyName(settings.uiExitKey) + ")", true);
            LogOut("[CONFIG] gpTeleportButton: " + GetGamepadButtonName(settings.gpTeleportButton), true);
            LogOut("[CONFIG] gpSavePositionButton: " + GetGamepadButtonName(settings.gpSavePositionButton), true);
            LogOut("[CONFIG] gpSwitchPlayersButton: " + GetGamepadButtonName(settings.gpSwitchPlayersButton), true);
            LogOut("[CONFIG] gpSwapPositionsButton: " + GetGamepadButtonName(settings.gpSwapPositionsButton), true);
            LogOut("[CONFIG] gpMacroRecordButton: " + GetGamepadButtonName(settings.gpMacroRecordButton), true);
            LogOut("[CONFIG] gpMacroPlayButton: " + GetGamepadButtonName(settings.gpMacroPlayButton), true);
            LogOut("[CONFIG] gpMacroSlotButton: " + GetGamepadButtonName(settings.gpMacroSlotButton), true);
            LogOut("[CONFIG] gpToggleMenuButton: " + GetGamepadButtonName(settings.gpToggleMenuButton), true);
            LogOut("[CONFIG] gpToggleImGuiButton: " + GetGamepadButtonName(settings.gpToggleImGuiButton), true);
            LogOut("[CONFIG] enableFpsDiagnostics: " + std::to_string(settings.enableFpsDiagnostics), true);
            LogOut("[CONFIG] uiScale: " + std::to_string(settings.uiScale), true);
            LogOut("[CONFIG] uiFontMode: " + std::to_string(settings.uiFontMode), true);
            
            return true;
        }
        catch (const std::exception& e) {
            LogOut("[CONFIG] Exception loading settings: " + std::string(e.what()), true);
            return false;
        }
    }
    
    bool SaveSettings() {
        // Serialize current settings to disk with comments
        try {
            std::ofstream file(configFilePath, std::ios::trunc);
            if (!file.is_open()) {
                LogOut("[CONFIG] SaveSettings: failed to open file for writing", true);
                return false;
            }

            auto toHexString = [](int value) -> std::string {
                std::ostringstream oss;
                oss << "0x" << std::hex << std::uppercase << value;
                return oss.str();
            };

            file << "[General]\n";
            file << "; Use the modern ImGui interface (1) or the legacy Win32 dialog (0)\n";
            file << "useImGui = " << (settings.useImGui ? "1" : "0") << "\n";
            file << "; Enable detailed debug messages in the console (1 = yes, 0 = no)\n";
            file << "detailedLogging = " << (settings.detailedLogging ? "1" : "0") << "\n";
            file << "; Show the debug console window (1 = yes, 0 = no)\n";
            file << "enableConsole = " << (settings.enableConsole ? "1" : "0") << "\n";
            file << "; Restrict functionality to Practice Mode only (1 = yes, 0 = no)\n";
            file << "restrictToPracticeMode = " << (settings.restrictToPracticeMode ? "1" : "0") << "\n\n";
            file << "; Enable FPS/timing diagnostics in logs (1 = yes, 0 = no)\n";
            file << "enableFpsDiagnostics = " << (settings.enableFpsDiagnostics ? "1" : "0") << "\n\n";
            file << "; Log active player / CPU flags during Character Select (1 = yes, 0 = no)\n";
            file << "enableCharacterSelectLogger = " << (settings.enableCharacterSelectLogger ? "1" : "0") << "\n\n";
            file << "; UI scale for ImGui window (0.80 - 1.20 recommended)\n";
            file << "uiScale = " << settings.uiScale << "\n\n";
            file << "; UI font: 0 = ImGui default font, 1 = Segoe UI (Windows)\n";
            file << "uiFont = " << settings.uiFontMode << "\n\n";
            file << "; Virtual Cursor settings\n";
            file << "enableVirtualCursor = " << (settings.enableVirtualCursor?"1":"0") << "\n";
            file << "virtualCursorAllowWindowed = " << (settings.virtualCursorAllowWindowed?"1":"0") << "\n";
            file << "virtualCursorBaseSpeed = " << (int)settings.virtualCursorBaseSpeed << "\n";
            file << "virtualCursorFastSpeed = " << (int)settings.virtualCursorFastSpeed << "\n";
            file << "virtualCursorDpadSpeed = " << (int)settings.virtualCursorDpadSpeed << "\n";
            file << "virtualCursorAccelPower = " << settings.virtualCursorAccelPower << "\n\n";

            // (Practice tuning omitted)
            file << "; Show the debug console window (1 = yes, 0 = no)\n";
            // Note: keep console toggle alongside General fields
            // We append here for clarity; order doesn't matter for parsing
            // but the default file includes it in the General section above too.
            // To ensure it's in [General], write again before Hotkeys if missing.
            // For simplicity, include it earlier with General fields.

            file << "[Hotkeys]\n";
            file << "; Use virtual-key codes (hexadecimal, e.g., 0x70 for F1)\n";
            file << "TeleportKey=" << toHexString(settings.teleportKey) << "\n";
            file << "RecordKey=" << toHexString(settings.recordKey) << "\n";
            file << "ConfigMenuKey=" << toHexString(settings.configMenuKey) << "\n";
            file << "ToggleTitleKey=" << toHexString(settings.toggleTitleKey) << "\n";
            file << "ResetFrameCounterKey=" << toHexString(settings.resetFrameCounterKey) << "\n";
            file << "HelpKey=" << toHexString(settings.helpKey) << "\n";
            file << "ToggleImGuiKey=" << toHexString(settings.toggleImGuiKey) << "\n";
            file << "SwitchPlayersKey=" << toHexString(settings.switchPlayersKey) << "\n";
            file << "MacroRecordKey=" << toHexString(settings.macroRecordKey) << "\n";
            file << "MacroPlayKey=" << toHexString(settings.macroPlayKey) << "\n";
            file << "MacroSlotKey=" << toHexString(settings.macroSlotKey) << "\n";
            file << "UIAcceptKey=" << toHexString(settings.uiAcceptKey) << "\n";
            file << "UIRefreshKey=" << toHexString(settings.uiRefreshKey) << "\n";
            file << "UIExitKey=" << toHexString(settings.uiExitKey) << "\n";

            file << "\n; Controller bindings (symbolic names or hex). -1 disables.\n";
            file << "; NOTE: ABXY reserved: A=UI confirm, B=UI back. Defaults map actions to shoulders/triggers/sticks.\n";
            auto writePad = [&](const char* key, int mask){
                file << key << "=" << GetGamepadButtonName(mask) << "\n";
            };
            writePad("gpTeleportButton", settings.gpTeleportButton);
            writePad("gpSavePositionButton", settings.gpSavePositionButton);
            writePad("gpSwitchPlayersButton", settings.gpSwitchPlayersButton);
            writePad("gpSwapPositionsButton", settings.gpSwapPositionsButton);
            writePad("gpMacroRecordButton", settings.gpMacroRecordButton);
            writePad("gpMacroPlayButton", settings.gpMacroPlayButton);
            writePad("gpMacroSlotButton", settings.gpMacroSlotButton);
            writePad("gpToggleMenuButton", settings.gpToggleMenuButton);
            writePad("gpToggleImGuiButton", settings.gpToggleImGuiButton);

            file.close();
            if (file.fail()) {
                LogOut("[CONFIG] SaveSettings: error when closing the file", true);
                return false;
            }

            // Refresh iniData from what we wrote
            LoadIniFromFile();
            LogOut("[CONFIG] Settings saved to disk", true);
            return true;
        } catch (const std::exception& e) {
            LogOut(std::string("[CONFIG] SaveSettings exception: ") + e.what(), true);
            return false;
        }
    }
    
    const Settings& GetSettings() {
        return settings;
    }
    
    void SetSetting(const std::string& section, const std::string& key, const std::string& value) {
        // Normalize for storage
        std::string sec = ToLower(section);
        std::string k = ToLower(key);
        SetIniValue(sec, k, value);
        
        // Update the appropriate setting (case-insensitive keys)
        if (sec == "general") {
            if (k == "useimgui" || k == "useimgui ") settings.useImGui = (value == "1");
            if (k == "detailedlogging") settings.detailedLogging = (value == "1");
            if (k == "enableconsole") settings.enableConsole = (value == "1");
            if (k == "restricttopracticemode") settings.restrictToPracticeMode = (value == "1");
            if (k == "uiscale") {
                try { settings.uiScale = std::stof(value); } catch (...) {}
            }
            if (k == "uifont") {
                try { settings.uiFontMode = std::stoi(value); } catch (...) {}
                if (settings.uiFontMode < 0 || settings.uiFontMode > 1) settings.uiFontMode = 0;
            }
            if (k == "enablevirtualcursor") settings.enableVirtualCursor = (value == "1");
            if (k == "virtualcursorallowwindowed") settings.virtualCursorAllowWindowed = (value == "1");
            if (k == "virtualcursorbasespeed") { try { settings.virtualCursorBaseSpeed = std::stof(value); } catch(...){} }
            if (k == "virtualcursorfastspeed") { try { settings.virtualCursorFastSpeed = std::stof(value); } catch(...){} }
            if (k == "virtualcursordpadspeed") { try { settings.virtualCursorDpadSpeed = std::stof(value); } catch(...){} }
            if (k == "virtualcursoraccelpower") { try { settings.virtualCursorAccelPower = std::stof(value); } catch(...){} }
        }
        else if (sec == "hotkeys") {
            int intValue = ParseKeyValue(value);
            if (k == "teleportkey") settings.teleportKey = intValue;
            if (k == "recordkey") settings.recordKey = intValue;
            if (k == "configmenukey") settings.configMenuKey = intValue;
            if (k == "toggletitlekey") settings.toggleTitleKey = intValue;
            if (k == "resetframecounterkey") settings.resetFrameCounterKey = intValue;
            if (k == "helpkey") settings.helpKey = intValue;
            if (k == "toggleimguikey") settings.toggleImGuiKey = intValue;
            if (k == "switchplayerskey") settings.switchPlayersKey = intValue;
            if (k == "macrorecordkey") settings.macroRecordKey = intValue;
            if (k == "macroplaykey") settings.macroPlayKey = intValue;
            if (k == "macroslotkey") settings.macroSlotKey = intValue;
            if (k == "uiacceptkey") settings.uiAcceptKey = intValue;
            if (k == "uirefreshkey") settings.uiRefreshKey = intValue;
            if (k == "uiexitkey") settings.uiExitKey = intValue;
            // Gamepad button updates (these accept names or hex values)
            if (k == "gpteleportbutton") settings.gpTeleportButton = ParseGamepadButton(value);
            if (k == "gpsavepositionbutton") settings.gpSavePositionButton = ParseGamepadButton(value);
            if (k == "gpswitchplayersbutton") settings.gpSwitchPlayersButton = ParseGamepadButton(value);
            if (k == "gpswappositionsbutton") settings.gpSwapPositionsButton = ParseGamepadButton(value);
            if (k == "gpmacrorecordbutton") settings.gpMacroRecordButton = ParseGamepadButton(value);
            if (k == "gpmacroplaybutton") settings.gpMacroPlayButton = ParseGamepadButton(value);
            if (k == "gpmacroslotbutton") settings.gpMacroSlotButton = ParseGamepadButton(value);
            if (k == "gptogglemenubutton") settings.gpToggleMenuButton = ParseGamepadButton(value);
            if (k == "gptoggleimguibutton") settings.gpToggleImGuiButton = ParseGamepadButton(value);
        }
    // Practice: no mutable settings currently
    }
    
    std::string GetConfigFilePath() {
        return configFilePath;
    }
    
    // Helper methods
    
    int ParseKeyValue(const std::string& value) {
        try {
            // Check if it's a hex value
            if (value.substr(0, 2) == "0x") {
                return std::stoi(value.substr(2), nullptr, 16);
            }
            // Otherwise, treat as decimal
            return std::stoi(value);
        }
        catch (...) {
            LogOut("[CONFIG] Failed to parse key value: " + value, true);
            return 0;
        }
    }
    
    std::string GetKeyName(int keyCode) {
        // Reuse existing GetKeyName from utilities.cpp
        return ::GetKeyName(keyCode);
    }

    // --- Gamepad parsing helpers ---
    int ParseGamepadButton(const std::string& value) {
        std::string v = value;
        // Trim
        auto trim = [](std::string &s){
            size_t a = s.find_first_not_of(" \t\r\n");
            size_t b = s.find_last_not_of(" \t\r\n");
            if (a == std::string::npos) { s.clear(); return; }
            s = s.substr(a, b - a + 1);
        };
        trim(v);
        if (v.empty()) return -1;
        // Hex / decimal numeric
        if (v.size() > 2 && (v[0]=='0') && (v[1]=='x' || v[1]=='X')) {
            try { return std::stoi(v.substr(2), nullptr, 16); } catch(...) { return -1; }
        }
        bool numeric = std::all_of(v.begin(), v.end(), [](unsigned char c){ return std::isdigit(c) || c=='-'; });
        if (numeric) {
            try { return std::stoi(v); } catch(...) { return -1; }
        }
        // Upper-case symbolic
        std::string u; u.reserve(v.size());
        for (char c: v) u.push_back((char)std::toupper((unsigned char)c));
        static const std::unordered_map<std::string,int> map = {
            {"A", XINPUT_GAMEPAD_A}, {"B", XINPUT_GAMEPAD_B}, {"X", XINPUT_GAMEPAD_X}, {"Y", XINPUT_GAMEPAD_Y},
            {"LB", XINPUT_GAMEPAD_LEFT_SHOULDER}, {"RB", XINPUT_GAMEPAD_RIGHT_SHOULDER},
            {"BACK", XINPUT_GAMEPAD_BACK}, {"START", XINPUT_GAMEPAD_START},
            {"L3", XINPUT_GAMEPAD_LEFT_THUMB}, {"R3", XINPUT_GAMEPAD_RIGHT_THUMB},
            {"DPAD_UP", XINPUT_GAMEPAD_DPAD_UP}, {"DPAD_DOWN", XINPUT_GAMEPAD_DPAD_DOWN},
            {"DPAD_LEFT", XINPUT_GAMEPAD_DPAD_LEFT}, {"DPAD_RIGHT", XINPUT_GAMEPAD_DPAD_RIGHT},
            // Virtual trigger pseudo-bits (outside wButtons range to avoid collision)
            {"LT", 0x10000}, {"RT", 0x20000}
        };
        auto it = map.find(u);
        if (it != map.end()) return it->second;
        return -1;
    }

    std::string GetGamepadButtonName(int mask) {
        if (mask < 0) return "-1"; // disabled
        // Recognize pseudo trigger bits first
        if (mask == 0x10000) return "LT";
        if (mask == 0x20000) return "RT";
        switch(mask) {
            case XINPUT_GAMEPAD_A: return "A";
            case XINPUT_GAMEPAD_B: return "B";
            case XINPUT_GAMEPAD_X: return "X";
            case XINPUT_GAMEPAD_Y: return "Y";
            case XINPUT_GAMEPAD_LEFT_SHOULDER: return "LB";
            case XINPUT_GAMEPAD_RIGHT_SHOULDER: return "RB";
            case XINPUT_GAMEPAD_BACK: return "BACK";
            case XINPUT_GAMEPAD_START: return "START";
            case XINPUT_GAMEPAD_LEFT_THUMB: return "L3";
            case XINPUT_GAMEPAD_RIGHT_THUMB: return "R3";
            case XINPUT_GAMEPAD_DPAD_UP: return "DPAD_UP";
            case XINPUT_GAMEPAD_DPAD_DOWN: return "DPAD_DOWN";
            case XINPUT_GAMEPAD_DPAD_LEFT: return "DPAD_LEFT";
            case XINPUT_GAMEPAD_DPAD_RIGHT: return "DPAD_RIGHT";
            default: {
                std::ostringstream oss; oss << "0x" << std::hex << std::uppercase << mask; return oss.str();
            }
        }
    }

    // Very simple INI parser to populate iniData from file
    bool LoadIniFromFile() {
        iniData.clear();
        std::ifstream file(configFilePath);
        if (!file.is_open()) {
            LogOut("[CONFIG] LoadIniFromFile: failed to open ini file", true);
            return false;
        }
        std::string line;
        std::string currentSection;
        while (std::getline(file, line)) {
            // Trim whitespace
            auto trim = [](std::string &s){
                size_t start = s.find_first_not_of(" \t\r\n");
                size_t end = s.find_last_not_of(" \t\r\n");
                if (start == std::string::npos) { s.clear(); return; }
                s = s.substr(start, end - start + 1);
            };
            trim(line);
            if (line.empty()) continue;
            // Skip comments
            if (line[0] == ';' || line[0] == '#') continue;
            if (line.front() == '[' && line.back() == ']') {
                currentSection = ToLower(line.substr(1, line.size()-2));
                continue;
            }
            // key=value; strip inline comments after '#' or ';'
            size_t hashPos = line.find('#');
            size_t semiPos = line.find(';');
            auto minPos = [](size_t a, size_t b) -> size_t {
                if (a == std::string::npos) return b;
                if (b == std::string::npos) return a;
                return (a < b) ? a : b;
            };
            size_t commentPos = minPos(hashPos, semiPos);
            std::string kv = (commentPos == std::string::npos) ? line : line.substr(0, commentPos);
            trim(kv);
            if (kv.empty()) continue;
            size_t eq = kv.find('=');
            if (eq == std::string::npos) continue;
            std::string key = kv.substr(0, eq);
            std::string value = kv.substr(eq + 1);
            trim(key); trim(value);
            if (!currentSection.empty() && !key.empty()) {
                SetIniValue(currentSection, key, value);
            }
        }
        return true;
    }
}