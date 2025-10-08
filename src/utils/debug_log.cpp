#include "utils/debug_log.h"
#include <windows.h>
#include <chrono>
#include <ctime>

namespace DebugLog {
    // Enable detailed logging (set to true for debugging)
    bool g_EnableDebugLog = true;
    
    static std::ofstream g_LogFile;
    static std::mutex g_LogMutex;
    static bool g_Initialized = false;
    
    // Get the DLL directory
    static std::string GetDllDirectory() {
        char path[MAX_PATH];
        HMODULE hModule = NULL;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | 
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)&GetDllDirectory, &hModule)) {
            GetModuleFileNameA(hModule, path, sizeof(path));
            std::string fullPath(path);
            size_t pos = fullPath.find_last_of("\\/");
            return fullPath.substr(0, pos);
        }
        return "";
    }
    
    void Initialize() {
        if (g_Initialized) return;
        
        if (!g_EnableDebugLog) {
            g_Initialized = true;
            return;
        }
        
        std::lock_guard<std::mutex> lock(g_LogMutex);
        
        std::string dllDir = GetDllDirectory();
        std::string logPath = dllDir + "\\efz_training_debug.log";
        
        g_LogFile.open(logPath, std::ios::out | std::ios::trunc);
        
        if (g_LogFile.is_open()) {
            // Write header with timestamp
            auto now = std::chrono::system_clock::now();
            auto time = std::chrono::system_clock::to_time_t(now);
            char timeStr[100];
            strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", localtime(&time));
            
            g_LogFile << "========================================\n";
            g_LogFile << "EFZ Training Mode Debug Log\n";
            g_LogFile << "Session Start: " << timeStr << "\n";
            g_LogFile << "Log File: " << logPath << "\n";
            g_LogFile << "========================================\n\n";
            g_LogFile.flush();
        }
        
        g_Initialized = true;
    }
    
    void Write(const std::string& message) {
        if (!g_EnableDebugLog || !g_Initialized) return;
        
        std::lock_guard<std::mutex> lock(g_LogMutex);
        
        if (g_LogFile.is_open()) {
            // Get timestamp
            auto now = std::chrono::system_clock::now();
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                now.time_since_epoch()) % 1000;
            auto time = std::chrono::system_clock::to_time_t(now);
            char timeStr[100];
            strftime(timeStr, sizeof(timeStr), "%H:%M:%S", localtime(&time));
            
            g_LogFile << "[" << timeStr << "." << std::setfill('0') << std::setw(3) 
                      << ms.count() << "] " << message << "\n";
        }
    }
    
    void WriteHex(const std::string& label, uintptr_t address, const void* data, size_t size) {
        if (!g_EnableDebugLog || !g_Initialized) return;
        
        std::ostringstream oss;
        oss << "[HEXDUMP] " << label << " @0x" << std::hex << std::uppercase << address 
            << " [" << std::dec << size << " bytes]: ";
        
        const uint8_t* bytes = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < size; ++i) {
            oss << std::hex << std::setw(2) << std::setfill('0') << (int)bytes[i] << " ";
        }
        
        Write(oss.str());
    }
    
    void Flush() {
        if (!g_EnableDebugLog || !g_Initialized) return;
        
        std::lock_guard<std::mutex> lock(g_LogMutex);
        if (g_LogFile.is_open()) {
            g_LogFile.flush();
        }
    }
    
    void Shutdown() {
        if (!g_Initialized) return;
        
        std::lock_guard<std::mutex> lock(g_LogMutex);
        
        if (g_LogFile.is_open()) {
            g_LogFile << "\n========================================\n";
            g_LogFile << "Session End\n";
            g_LogFile << "========================================\n";
            g_LogFile.close();
        }
        
        g_Initialized = false;
    }
}
