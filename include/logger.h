#pragma once
#include <string>
#include <fstream>
#include <mutex>
    #include <atomic>

extern std::mutex g_logMutex;
extern std::ofstream g_log;
extern std::atomic<bool> detailedTitleMode;
extern std::atomic<bool> detailedDebugOutput; // Add this line

void LogOut(const std::string& msg, bool consoleOutput = false);
void InitializeLogging();
void UpdateConsoleTitle();
short GetCurrentMoveID(int player);