#include "../include/core/globals.h"

// Define the global shutdown flag
std::atomic<bool> g_isShuttingDown(false);

// Define other globals as needed

// Our module handle (set once on PROCESS_ATTACH)
HMODULE g_hSelfModule = nullptr;