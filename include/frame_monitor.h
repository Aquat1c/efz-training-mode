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

void ApplyAirtech(uintptr_t moveIDAddr, int playerNum, int frameNum);
void MonitorAutoAirtech(
    uintptr_t base,
    uintptr_t moveIDAddr1, uintptr_t moveIDAddr2,
    short moveID1, short moveID2, 
    short prevMoveID1, short prevMoveID2
);

void FrameDataMonitor();