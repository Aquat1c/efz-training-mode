#pragma once
#include <windows.h>
#include <string>
#include <atomic>

extern std::atomic<bool> isOnlineMatch;

// Minimal EfzRevival version enum detected from the game's window title
enum class EfzRevivalVersion : int {
	Unknown = 0,
	Vanilla,     // No "-Revival-" marker or no version tag
	Revival102e, // Eternal Fighter Zero -Revival- 1.02e
	Revival102h, // Eternal Fighter Zero -Revival- 1.02h (unsupported for now)
	Revival102i, // Eternal Fighter Zero -Revival- 1.02i (treat like 1.02h for RVAs)
	Other        // Some other Revival build
};

// Online state reported by EfzRevival.dll flag (0=netplay, 1=spectating, 2=offline)
enum class OnlineState : int {
	Netplay = 0,
	Spectating = 1,
	Offline = 2,
	Tournament = 3,
	Unknown = -1
};

// Function to check if EFZ is in an online match
bool DetectOnlineMatch();

// Detect EfzRevival version by parsing the EFZ window title. Cached after first call.
EfzRevivalVersion GetEfzRevivalVersion();
// Human-readable name for EfzRevivalVersion
const char* EfzRevivalVersionName(EfzRevivalVersion v);
// Whether this build of the training mode supports the detected Revival version
bool IsEfzRevivalVersionSupported(EfzRevivalVersion v = (EfzRevivalVersion)0 /*use detected*/);


// Optional: read state directly from EfzRevival.dll if available
OnlineState ReadEfzRevivalOnlineState();
// Helper: get human-readable name for OnlineState
const char* OnlineStateName(OnlineState st);