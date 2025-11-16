#pragma once

#include <cstdint>
#include <windows.h>
#include "../utils/config.h" // for settings snapshot if needed later
#include "game_state.h" // GamePhase / GameMode enums

// Unified 192Hz sampling context populated once per FrameDataMonitor loop.
// Initial version is read-only and non-invasive: subsystems keep existing APIs.
// Later we can pass this by const reference to avoid duplicated memory reads.
struct PerFrameSample {
    uint32_t        frame;            // Global internal frame counter (192Hz)
    unsigned long long tickMs;        // GetTickCount64 at sample time
    GamePhase       phase;            // Current game phase
    GameMode        mode;             // Current game mode
    bool            charsInitialized; // Are characters initialized
    // Move IDs + previous for edge detections
    short           moveID1;
    short           moveID2;
    short           prevMoveID1;
    short           prevMoveID2;
    bool            actionable1;      // Cached IsActionable(moveID1)
    bool            actionable2;      // Cached IsActionable(moveID2)
    bool            neutral1;         // Cached neutral whitelist for side 1
    bool            neutral2;         // Cached neutral whitelist for side 2
    // Core pointers (best-effort; 0 if unavailable)
    uintptr_t       basePtr;
    uintptr_t       gameStatePtr;
    uintptr_t       p1Ptr;
    uintptr_t       p2Ptr;
    bool            online;           // Netplay/spectating/tournament active
};

// Accessor for current sample (lifetime owned by frame_monitor.cpp)
const PerFrameSample& GetCurrentPerFrameSample();
