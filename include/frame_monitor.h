#pragma once
#include <atomic>

// Update the monitor state enum
enum MonitorState {
    Idle,
    Monitoring,
    RGMonitoring,
    SuperflashMonitoring
};

extern MonitorState state;

// Auto-airtech related declarations
extern bool p1InAirHitstun;
extern bool p2InAirHitstun;
extern int p1LastHitstunFrame;
extern int p2LastHitstunFrame;

// Add function declarations for the missing functions
void ApplyAirtechPatches();
void RemoveAirtechPatches();
void ApplyAirtech(uintptr_t moveIDAddr, int playerNum, int frameNum);
void MonitorAutoAirtech(short moveID1, short moveID2);

void FrameDataMonitor();