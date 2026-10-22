#pragma once
#include <windows.h>
#include <atomic>

// Active: currently playing an airtech animation (forward/backward).
// Airtechable: untech==0 and in an eligible hitstun/guard state.
extern std::atomic<bool> g_airtechP1Active;
extern std::atomic<bool> g_airtechP2Active;
extern std::atomic<bool> g_airtechP1Airtechable;
extern std::atomic<bool> g_airtechP2Airtechable;
// true = facing right, false = facing left
extern std::atomic<bool> g_airtechP1FacingRight;
extern std::atomic<bool> g_airtechP2FacingRight;

bool IsPlayerAirtechable(short moveID, int playerNum);
bool IsAirtechAnimation(short moveID);

void MonitorAutoAirtech(short moveID1, short moveID2);

// No-ops kept for compatibility.
void ApplyAirtechPatches();
void RemoveAirtechPatches();