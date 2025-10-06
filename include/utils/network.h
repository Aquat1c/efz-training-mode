#pragma once
#include <windows.h>
#include <string>
#include <atomic>

extern std::atomic<bool> isOnlineMatch;

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


// Optional: read state directly from EfzRevival.dll if available
OnlineState ReadEfzRevivalOnlineState();
// Helper: get human-readable name for OnlineState
const char* OnlineStateName(OnlineState st);