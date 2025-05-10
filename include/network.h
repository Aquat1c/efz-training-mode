#pragma once
#include <windows.h>
#include <string>
#include <atomic>

extern std::atomic<bool> isOnlineMatch;

// Function to check if EFZ is in an online match
bool DetectOnlineMatch();

// Function to handle online match detection and console management
void MonitorOnlineStatus();