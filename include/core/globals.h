#pragma once
#include <atomic>
#include <windows.h>

// Global shutdown flag used across all threads
extern std::atomic<bool> g_isShuttingDown;

// Global initialization flag
extern std::atomic<bool> g_initialized;

// Global feature enable flag  
extern std::atomic<bool> g_featuresEnabled;

// Handle to our own module (set in DllMain on attach)
extern HMODULE g_hSelfModule;