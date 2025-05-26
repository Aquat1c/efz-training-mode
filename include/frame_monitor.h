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
void MonitorAutoJump(); // Remove any parameters from this declaration

void FrameDataMonitor();

// Blockstun and attack level detection
extern short initialBlockstunMoveID;
bool IsBlockstunState(short moveID);
int GetAttackLevel(short blockstunMoveID);
std::string GetBlockStateType(short blockstunMoveID);
bool IsDashState(short moveID);
int GetExpectedFrameAdvantage(int attackLevel, bool isAirBlock, bool isHit = false);