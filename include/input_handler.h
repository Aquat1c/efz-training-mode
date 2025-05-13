#pragma once
#include <windows.h>
#include <atomic>
#include <thread>

// Declare external variables
extern std::atomic<bool> autoAirtechEnabled;
extern std::atomic<bool> autoJumpEnabled;
extern std::atomic<int> autoAirtechDirection;
extern std::atomic<int> jumpDirection;
extern std::atomic<int> jumpTarget;
extern std::atomic<bool> menuOpen;
extern std::atomic<bool> keyMonitorRunning;

// Function declarations
void MonitorKeys();
void RestartKeyMonitoring();