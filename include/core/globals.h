#pragma once
#include <atomic>

// Global shutdown flag used across all threads
extern std::atomic<bool> g_isShuttingDown;

// Global initialization flag
extern std::atomic<bool> g_initialized;

// Global feature enable flag  
extern std::atomic<bool> g_featuresEnabled;