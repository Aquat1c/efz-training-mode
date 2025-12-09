#pragma once
#include <windows.h>
#include <string>
#include <fstream>
#include <mutex>
    #include <atomic>

extern std::mutex g_logMutex;
extern std::ofstream g_log;
extern std::atomic<bool> detailedTitleMode;
extern std::atomic<bool> detailedDebugOutput;
// Global reduced logging toggle (suppresses repetitive/duplicate spam while keeping critical events)
extern std::atomic<bool> g_reducedLogging;

// NEW: Add Logger namespace and hwndToString declaration
namespace Logger {
    std::string hwndToString(HWND hwnd);
}

void LogOut(const std::string& msg, bool consoleOutput = false);
void InitializeLogging();
void UpdateConsoleTitle();
short GetCurrentMoveID(int player);
// Flush any pending console logs buffered before the console was created
void FlushPendingConsoleLogs();
void SetConsoleReady(bool ready);
// Enable/disable reduced logging mode at runtime
void SetReducedLogging(bool reduced);