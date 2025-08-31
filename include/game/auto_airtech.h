#pragma once
#include <windows.h>
#include <atomic>

// Flag-based status for other systems/UI to consume
// - Active: currently playing an airtech animation (forward/backward)
// - Airtechable: can airtech now (untech==0 and in an eligible hitstun/guard state)
extern std::atomic<bool> g_airtechP1Active;
extern std::atomic<bool> g_airtechP2Active;
extern std::atomic<bool> g_airtechP1Airtechable;
extern std::atomic<bool> g_airtechP2Airtechable;
// Facing direction flags (true = facing right, false = facing left)
extern std::atomic<bool> g_airtechP1FacingRight;
extern std::atomic<bool> g_airtechP2FacingRight;

// Function declarations
// Helpers exposed for clarity/tests
bool IsPlayerAirtechable(short moveID, int playerNum);
bool IsAirtechAnimation(short moveID);

// Main monitor (called from frame loop)
void MonitorAutoAirtech(short moveID1, short moveID2);

// Patch no-ops kept for compatibility (we no longer patch in safe mode)
void ApplyAirtechPatches();
void RemoveAirtechPatches();