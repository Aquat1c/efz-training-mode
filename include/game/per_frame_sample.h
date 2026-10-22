#pragma once

#include <cstdint>
#include <windows.h>
#include "../utils/config.h"
#include "game_state.h"

// Unified 192Hz sampling context populated once per FrameDataMonitor loop.
struct PerFrameSample {
    uint32_t        frame;            // Global internal frame counter (192Hz)
    unsigned long long tickMs;        // GetTickCount64 at sample time
    GamePhase       phase;            // Current game phase
    GameMode        mode;             // Current game mode
    bool            charsInitialized; // Are characters initialized
    short           moveID1;
    short           moveID2;
    short           prevMoveID1;
    short           prevMoveID2;
    bool            actionable1;      // Cached IsActionable(moveID1)
    bool            actionable2;      // Cached IsActionable(moveID2)
    bool            neutral1;         // Cached neutral whitelist for side 1
    bool            neutral2;         // Cached neutral whitelist for side 2
    uintptr_t       basePtr;
    uintptr_t       gameStatePtr;
    uintptr_t       p1Ptr;
    uintptr_t       p2Ptr;
    bool            online;           // Netplay/spectating/tournament active
};

// Current sample; lifetime owned by frame_monitor.cpp
const PerFrameSample& GetCurrentPerFrameSample();
