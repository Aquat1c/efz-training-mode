#pragma once
#include <string>
#include <fstream>
#include <mutex>
    #include <atomic>

extern std::mutex g_logMutex;
extern std::ofstream g_log;
extern std::atomic<bool> detailedTitleMode;

void LogOut(const std::string& msg, bool consoleOutput = false);
void InitializeLogging();
void UpdateConsoleTitle();
short GetCurrentMoveID(int player);