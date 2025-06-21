#pragma once
#include <windows.h>

// Global variables
extern bool p1InAirHitstun;
extern bool p2InAirHitstun;
extern int p1LastHitstunFrame;
extern int p2LastHitstunFrame;

// Function declarations
bool CanAirtech(short moveID);
void MonitorAutoAirtech(short moveID1, short moveID2);  // Removed ApplyAirtech
void ApplyAirtechPatches();
void RemoveAirtechPatches();