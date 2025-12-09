#pragma once

#include <string>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <mutex>

namespace DebugLog {
    // Enable/disable detailed logging to file
    extern bool g_EnableDebugLog;
    
    // Initialize the debug log file
    void Initialize();
    
    // Write a message to the debug log
    void Write(const std::string& message);
    
    // Write formatted hex dump
    void WriteHex(const std::string& label, uintptr_t address, const void* data, size_t size);
    
    // Write memory read operation
    template<typename T>
    void LogRead(const std::string& label, uintptr_t address, T value) {
        if (!g_EnableDebugLog) return;
        std::ostringstream oss;
        oss << "[READ] " << label << " @0x" << std::hex << std::uppercase << address 
            << " = 0x" << std::setw(sizeof(T)*2) << std::setfill('0') << (uint64_t)value 
            << " (" << std::dec << (int64_t)value << ")";
        Write(oss.str());
    }
    
    // Write memory write operation with before/after values
    template<typename T>
    void LogWrite(const std::string& label, uintptr_t address, T before, T after) {
        if (!g_EnableDebugLog) return;
        std::ostringstream oss;
        oss << "[WRITE] " << label << " @0x" << std::hex << std::uppercase << address 
            << " : 0x" << std::setw(sizeof(T)*2) << std::setfill('0') << (uint64_t)before 
            << " -> 0x" << std::setw(sizeof(T)*2) << std::setfill('0') << (uint64_t)after;
        Write(oss.str());
    }
    
    // Flush the log file
    void Flush();
    
    // Close the log file
    void Shutdown();
}
