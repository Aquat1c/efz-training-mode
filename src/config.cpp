#include "../include/config.h"
#include "../include/logger.h"
#include "../include/utilities.h"
#include <fstream>
#include <sstream>
#include <filesystem>

namespace Config {
    // Internal settings storage
    static Settings settings;
    
    // Path to config file
    static std::string configFilePath;
    
    // Internal representation of the ini file
    static std::unordered_map<std::string, std::unordered_map<std::string, std::string>> iniData;
    
    // Forward declare helper methods
    void SetIniValue(const std::string& section, const std::string& key, const std::string& value);
    bool GetValueBool(const std::string& section, const std::string& key, bool defaultValue);
    int GetValueInt(const std::string& section, const std::string& key, int defaultValue);
    
    // Helper method implementations
    void SetIniValue(const std::string& section, const std::string& key, const std::string& value) {
        iniData[section][key] = value;
    }
    
    bool GetValueBool(const std::string& section, const std::string& key, bool defaultValue) {
        auto sectionIt = iniData.find(section);
        if (sectionIt != iniData.end()) {
            auto keyIt = sectionIt->second.find(key);
            if (keyIt != sectionIt->second.end()) {
                return keyIt->second == "1" || keyIt->second == "true" || keyIt->second == "yes";
            }
        }
        return defaultValue;
    }
    
    int GetValueInt(const std::string& section, const std::string& key, int defaultValue) {
        auto sectionIt = iniData.find(section);
        if (sectionIt != iniData.end()) {
            auto keyIt = sectionIt->second.find(key);
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
            
            // Load settings
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
            // Make sure the directory exists
            std::filesystem::path configDir = std::filesystem::path(configFilePath).parent_path();
            if (!std::filesystem::exists(configDir)) {
                LogOut("[CONFIG] Creating directory: " + configDir.string(), true);
                std::error_code ec;
                bool dirCreated = std::filesystem::create_directories(configDir, ec);
                if (!dirCreated) {
                    LogOut("[CONFIG] Failed to create directory, error: " + ec.message(), true);
                    return false;
                }
            }
            
            // Attempt to open file with detailed error handling
            std::ofstream configFile(configFilePath);
            if (!configFile.is_open()) {
                char errMsg[256];
                strerror_s(errMsg, sizeof(errMsg), errno);
                LogOut("[CONFIG] Failed to open config file for writing: " + std::string(errMsg), true);
                return false;
            }
            
            LogOut("[CONFIG] File opened successfully, writing content...", true);
            
            // Write default content
            configFile << "[General]\n";
            configFile << "# Use GUI system: 0 = Legacy (Win32 dialog), 1 = ImGui (overlay)\n";
            configFile << "UseImGui=1\n";
            configFile << "# Enable detailed logging: 0 = disabled, 1 = enabled\n";
            configFile << "DetailedLogging=0\n\n";
            
            configFile << "[Hotkeys]\n";
            configFile << "# Key codes can be specified in hexadecimal (0x##) or decimal\n";
            configFile << "# See key code definitions at: https://docs.microsoft.com/en-us/windows/win32/inputdev/virtual-key-codes\n";
            configFile << "# Common keys: \n";
            configFile << "# - 0x31-0x39: Number keys 1-9\n";
            configFile << "# - 0x41-0x5A: Letter keys A-Z\n";
            configFile << "# - 0x70-0x7B: Function keys F1-F12\n\n";
            
            configFile << "# Teleport players to recorded positions\n";
            configFile << "TeleportKey=0x31    # Default: '1' key\n\n";
            
            configFile << "# Record current player positions\n";
            configFile << "RecordKey=0x32      # Default: '2' key\n\n";
            
            configFile << "# Open config menu\n";
            configFile << "ConfigMenuKey=0x33  # Default: '3' key\n\n";
            
            configFile << "# Toggle title display mode\n";
            configFile << "ToggleTitleKey=0x34 # Default: '4' key\n\n";
            
            configFile << "# Reset frame counter\n";
            configFile << "ResetFrameCounterKey=0x35 # Default: '5' key\n\n";
            
            configFile << "# Show help and clear console\n";
            configFile << "HelpKey=0x36        # Default: '6' key\n\n";
            
            configFile << "# Toggle ImGui overlay\n";
            configFile << "ToggleImGuiKey=0x37 # Default: '7' key\n";
            
            configFile.close();
            
            if (configFile.fail()) {
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
        
        try {
            // Clear current ini data
            iniData.clear();
            
            // Check if file exists before trying to open
            if (!std::filesystem::exists(configFilePath)) {
                LogOut("[CONFIG] Config file does not exist! Path: " + configFilePath, true);
                LogOut("[CONFIG] Creating default config and retrying...", true);
                if (CreateDefaultConfig()) {
                    LogOut("[CONFIG] Default config created, continuing to load", true);
                } else {
                    LogOut("[CONFIG] Failed to create default config, aborting load", true);
                    return false;
                }
            }
            
            std::ifstream configFile(configFilePath);
            if (!configFile.is_open()) {
                LogOut("[CONFIG] Failed to open config file for reading: " + configFilePath, true);
                char errMsg[256];
                strerror_s(errMsg, sizeof(errMsg), errno);
                LogOut("[CONFIG] Error message: " + std::string(errMsg), true);
                return false;
            }
            
            // Report file size
            std::streampos fileSize = 0;
            fileSize = configFile.tellg();
            configFile.seekg(0, std::ios::end);
            fileSize = configFile.tellg() - fileSize;
            configFile.seekg(0, std::ios::beg);
            LogOut("[CONFIG] Config file opened successfully, size: " + std::to_string(fileSize) + " bytes", true);
            
            std::string line;
            std::string currentSection;
            int lineCount = 0;
            
            while (std::getline(configFile, line)) {
                lineCount++;
                
                // Skip empty lines and comments
                if (line.empty() || line[0] == '#' || line[0] == ';')
                    continue;
                    
                // Check for section
                if (line[0] == '[' && line[line.length() - 1] == ']') {
                    currentSection = line.substr(1, line.length() - 2);
                    LogOut("[CONFIG] Found section: " + currentSection, true);
                    continue;
                }
                
                // Look for key=value pairs
                size_t equalsPos = line.find('=');
                if (equalsPos != std::string::npos && !currentSection.empty()) {
                    std::string key = line.substr(0, equalsPos);
                    std::string value = line.substr(equalsPos + 1);
                    
                    // Remove comments after the value
                    size_t commentPos = value.find('#');
                    if (commentPos != std::string::npos)
                        value = value.substr(0, commentPos);
                        
                    // Trim whitespace
                    key.erase(0, key.find_first_not_of(" \t"));
                    key.erase(key.find_last_not_of(" \t") + 1);
                    value.erase(0, value.find_first_not_of(" \t"));
                    value.erase(value.find_last_not_of(" \t") + 1);
                    
                    // Store in our data structure
                    iniData[currentSection][key] = value;
                    LogOut("[CONFIG] Loaded key=" + key + ", value=" + value, true);
                }
            }
            
            configFile.close();
            LogOut("[CONFIG] Finished reading " + std::to_string(lineCount) + " lines from config file", true);
            
            // Parse the settings with defaults if not found
            settings.useImGui = GetValueBool("General", "UseImGui", true);
            settings.detailedLogging = GetValueBool("General", "DetailedLogging", false);
            
            settings.teleportKey = GetValueInt("Hotkeys", "TeleportKey", 0x31);      // '1'
            settings.recordKey = GetValueInt("Hotkeys", "RecordKey", 0x32);          // '2'
            settings.configMenuKey = GetValueInt("Hotkeys", "ConfigMenuKey", 0x33);  // '3'
            settings.toggleTitleKey = GetValueInt("Hotkeys", "ToggleTitleKey", 0x34); // '4'
            settings.resetFrameCounterKey = GetValueInt("Hotkeys", "ResetFrameCounterKey", 0x35); // '5'
            settings.helpKey = GetValueInt("Hotkeys", "HelpKey", 0x36);              // '6'
            settings.toggleImGuiKey = GetValueInt("Hotkeys", "ToggleImGuiKey", 0x37); // '7'
            
            LogOut("[CONFIG] Settings loaded successfully", true);
            LogOut("[CONFIG] UseImGui: " + std::to_string(settings.useImGui), true);
            LogOut("[CONFIG] DetailedLogging: " + std::to_string(settings.detailedLogging), true);
            LogOut("[CONFIG] TeleportKey: " + std::to_string(settings.teleportKey) + " (" + GetKeyName(settings.teleportKey) + ")", true);
            LogOut("[CONFIG] RecordKey: " + std::to_string(settings.recordKey) + " (" + GetKeyName(settings.recordKey) + ")", true);
            LogOut("[CONFIG] ConfigMenuKey: " + std::to_string(settings.configMenuKey) + " (" + GetKeyName(settings.configMenuKey) + ")", true);
            LogOut("[CONFIG] ToggleTitleKey: " + std::to_string(settings.toggleTitleKey) + " (" + GetKeyName(settings.toggleTitleKey) + ")", true);
            LogOut("[CONFIG] ResetFrameCounterKey: " + std::to_string(settings.resetFrameCounterKey) + " (" + GetKeyName(settings.resetFrameCounterKey) + ")", true);
            LogOut("[CONFIG] HelpKey: " + std::to_string(settings.helpKey) + " (" + GetKeyName(settings.helpKey) + ")", true);
            LogOut("[CONFIG] ToggleImGuiKey: " + std::to_string(settings.toggleImGuiKey) + " (" + GetKeyName(settings.toggleImGuiKey) + ")", true);
            
            return true;
        }
        catch (const std::exception& e) {
            LogOut("[CONFIG] Exception loading settings: " + std::string(e.what()), true);
            return false;
        }
    }
    
    bool SaveSettings() {
        // Update the iniData structure with current settings
        SetIniValue("General", "UseImGui", settings.useImGui ? "1" : "0");
        SetIniValue("General", "DetailedLogging", settings.detailedLogging ? "1" : "0");
        
        // Convert to hex string with "0x" prefix
        auto toHexString = [](int value) -> std::string {
            std::stringstream ss;
            ss << "0x" << std::hex << value;
            return ss.str();
        };
        
        SetIniValue("Hotkeys", "TeleportKey", toHexString(settings.teleportKey));
        SetIniValue("Hotkeys", "RecordKey", toHexString(settings.recordKey));
        SetIniValue("Hotkeys", "ConfigMenuKey", toHexString(settings.configMenuKey));
        SetIniValue("Hotkeys", "ToggleTitleKey", toHexString(settings.toggleTitleKey));
        SetIniValue("Hotkeys", "ResetFrameCounterKey", toHexString(settings.resetFrameCounterKey));
        SetIniValue("Hotkeys", "HelpKey", toHexString(settings.helpKey));
        SetIniValue("Hotkeys", "ToggleImGuiKey", toHexString(settings.toggleImGuiKey));
        
        // Write to file, preserving comments if possible
        // For simplicity, we'll just rewrite the file with default comments
        return CreateDefaultConfig();
    }
    
    const Settings& GetSettings() {
        return settings;
    }
    
    void SetSetting(const std::string& section, const std::string& key, const std::string& value) {
        SetIniValue(section, key, value);
        
        // Update the appropriate setting
        if (section == "General") {
            if (key == "UseImGui")
                settings.useImGui = (value == "1" || value == "true");
            else if (key == "DetailedLogging")
                settings.detailedLogging = (value == "1" || value == "true");
        }
        else if (section == "Hotkeys") {
            int keyValue = ParseKeyValue(value);
            
            if (key == "TeleportKey")
                settings.teleportKey = keyValue;
            else if (key == "RecordKey")
                settings.recordKey = keyValue;
            else if (key == "ConfigMenuKey")
                settings.configMenuKey = keyValue;
            else if (key == "ToggleTitleKey")
                settings.toggleTitleKey = keyValue;
            else if (key == "ResetFrameCounterKey")
                settings.resetFrameCounterKey = keyValue;
            else if (key == "HelpKey")
                settings.helpKey = keyValue;
            else if (key == "ToggleImGuiKey")
                settings.toggleImGuiKey = keyValue;
        }
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
}