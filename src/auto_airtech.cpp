#include "../include/game/auto_airtech.h"
#include "../include/core/constants.h"
#include "../include/utils/utilities.h"

#include "../include/core/memory.h"
#include "../include/core/logger.h"
#include <thread>
#include <atomic>

// Original bytes for patches
char originalEnableBytes[2] = {0x74, 0x71};
char originalForwardBytes[2] = {0x75, 0x24};
char originalBackwardBytes[2] = {0x75, 0x21};
bool patchesApplied = false;

// Simple state tracking for delayed patches
bool patchesPending = false;
int patchDelayCountdown = 0;

// Track if player is currently airteching
bool p1IsAirteching = false;
bool p2IsAirteching = false;

void ApplyAirtechPatches() {
    uintptr_t base = GetEFZBase();
    if (!base) {
        LogOut("[AUTO-AIRTECH] Cannot get game base address", true);
        return;
    }

    // Store memory values before patch for debugging
    char currentEnableBytes[2] = {0, 0};
    SafeReadMemory(base + AIRTECH_ENABLE_ADDR, currentEnableBytes, 2);
    
    // Always NOP the main enable address
    if (!NopMemory(base + AIRTECH_ENABLE_ADDR, 2)) {
        LogOut("[AUTO-AIRTECH] Failed to NOP enable address", true);
    }

    // Apply the appropriate directional patch
    int direction = autoAirtechDirection.load();
    if (direction == 0) { // Forward
        if (!NopMemory(base + AIRTECH_FORWARD_ADDR, 2)) {
            LogOut("[AUTO-AIRTECH] Failed to NOP forward address", true);
        }
        // Make sure backward is restored to original
        PatchMemory(base + AIRTECH_BACKWARD_ADDR, originalBackwardBytes, 2);
        LogOut("[AUTO-AIRTECH] Forward airtech enabled", detailedLogging.load());
    } else { // Backward
        if (!NopMemory(base + AIRTECH_BACKWARD_ADDR, 2)) {
            LogOut("[AUTO-AIRTECH] Failed to NOP backward address", true);
        }
        // Make sure forward is restored to original
        PatchMemory(base + AIRTECH_FORWARD_ADDR, originalForwardBytes, 2);
        LogOut("[AUTO-AIRTECH] Backward airtech enabled", detailedLogging.load());
    }

    patchesApplied = true;
    LogOut("[AUTO-AIRTECH] Patches applied successfully", detailedLogging.load());
}

void RemoveAirtechPatches() {
    uintptr_t base = GetEFZBase();
    if (!base) {
        LogOut("[AUTO-AIRTECH] Cannot get game base address", true);
        return;
    }

    // Restore all patches to original bytes
    PatchMemory(base + AIRTECH_ENABLE_ADDR, originalEnableBytes, 2);
    PatchMemory(base + AIRTECH_FORWARD_ADDR, originalForwardBytes, 2);
    PatchMemory(base + AIRTECH_BACKWARD_ADDR, originalBackwardBytes, 2);

    patchesApplied = false;
    LogOut("[AUTO-AIRTECH] Patches removed", detailedLogging.load());
}

// Check if the player is currently in an airtechable state
bool IsPlayerAirtechable(short moveID, int playerNum) {
    uintptr_t base = GetEFZBase();
    if (!base) return false;
    
    uintptr_t baseOffset = (playerNum == 1) ? EFZ_BASE_OFFSET_P1 : EFZ_BASE_OFFSET_P2;
    
    // Check untech value
    short untechValue = 0;
    uintptr_t untechAddr = ResolvePointer(base, baseOffset, UNTECH_OFFSET);
    if (untechAddr) {
        SafeReadMemory(untechAddr, &untechValue, sizeof(short));
    }
    
    // Check moveID for airtechable states
    bool airtechableMoveID = (moveID >= LAUNCHED_HITSTUN_START && moveID <= LAUNCHED_HITSTUN_END) ||
                            (moveID == FIRE_STATE || moveID == ELECTRIC_STATE) ||
                            (moveID == AIR_GUARD_ID);
    
    // Untech value == 0 means player can tech
    // AND moveID should be in an airtechable state
    return airtechableMoveID && (untechValue == 0);
}

// Check if player is in airtech animation
bool IsAirtechAnimation(short moveID) {
    return moveID == FORWARD_AIRTECH || moveID == BACKWARD_AIRTECH;
}

void MonitorAutoAirtech(short moveID1, short moveID2) {
    static bool prevEnabled = false;
    static int prevDirection = -1;
    static int prevDelay = -1;
    static bool p1WasAirtechable = false;
    static bool p2WasAirtechable = false;
    static int p1DelayCounter = 0;
    static int p2DelayCounter = 0;
    static int debugCounter = 0;

    // Track if player is in airtech animation
    bool p1CurrentlyAirteching = IsAirtechAnimation(moveID1);
    bool p2CurrentlyAirteching = IsAirtechAnimation(moveID2);
    
    // Detect transitions into airtech animation
    if (p1CurrentlyAirteching && !p1IsAirteching) {
        LogOut("[AUTO-AIRTECH] P1 entered airtech animation", detailedLogging.load());
        if (patchesApplied) RemoveAirtechPatches();
    }
    
    if (p2CurrentlyAirteching && !p2IsAirteching) {
        LogOut("[AUTO-AIRTECH] P2 entered airtech animation", detailedLogging.load());
        if (patchesApplied) RemoveAirtechPatches();
    }
    
    // Update tracking variables
    p1IsAirteching = p1CurrentlyAirteching;
    p2IsAirteching = p2CurrentlyAirteching;

    // Periodic status logging
    if (++debugCounter % 120 == 0) {
        LogOut("[AUTO-AIRTECH] Status: Enabled=" + std::to_string(autoAirtechEnabled.load()) +
               ", Direction=" + std::to_string(autoAirtechDirection.load()) +
               ", Delay=" + std::to_string(autoAirtechDelay.load()) +
               ", Patches=" + (patchesApplied ? "YES" : "NO") +
               ", P1Airteching=" + (p1IsAirteching ? "YES" : "NO") +
               ", P2Airteching=" + (p2IsAirteching ? "YES" : "NO"), 
               detailedLogging.load());
    }

    // Settings changed
    bool settingsChanged = (prevEnabled != autoAirtechEnabled.load() ||
                           prevDirection != autoAirtechDirection.load() ||
                           prevDelay != autoAirtechDelay.load());
    
    if (settingsChanged) {
        // Handle disabled state
        if (!autoAirtechEnabled.load() && patchesApplied) {
            RemoveAirtechPatches();
            p1DelayCounter = 0;
            p2DelayCounter = 0;
        }
        // Handle instant airtech (delay = 0)
        else if (autoAirtechEnabled.load() && autoAirtechDelay.load() == 0) {
            // Only apply patches if nobody is currently airteching
            if (!patchesApplied && !p1IsAirteching && !p2IsAirteching) {
                ApplyAirtechPatches();
            }
            p1DelayCounter = 0;
            p2DelayCounter = 0;
        }
        // Handle new delay settings
        else if (autoAirtechEnabled.load() && autoAirtechDelay.load() > 0) {
            if (patchesApplied) {
                RemoveAirtechPatches(); // Remove any existing patches
            }
            p1DelayCounter = 0;
            p2DelayCounter = 0;
        }
        
        prevEnabled = autoAirtechEnabled.load();
        prevDirection = autoAirtechDirection.load();
        prevDelay = autoAirtechDelay.load();
    }

    // Only process delayed airtech logic if:
    // 1. Feature is enabled
    // 2. Delay > 0
    // 3. NO player is currently in airtech animation
    if (autoAirtechEnabled.load() && autoAirtechDelay.load() > 0 && !p1IsAirteching && !p2IsAirteching) {
        // Check P1 airtechable state
        bool p1Airtechable = IsPlayerAirtechable(moveID1, 1);
        
        // Detect when P1 BECOMES airtechable (start counting)
        if (p1Airtechable && !p1WasAirtechable) {
            p1DelayCounter = autoAirtechDelay.load();
            LogOut("[AUTO-AIRTECH] P1 became airtechable, starting delay: " + 
                   std::to_string(autoAirtechDelay.load()) + " visual frames", detailedLogging.load());
        }
        
        // Count down P1's delay and apply patches when it expires
        if (p1DelayCounter > 0) {
            p1DelayCounter--;
            if (p1DelayCounter == 0 && p1Airtechable) {
                LogOut("[AUTO-AIRTECH] P1 delay expired, applying patches", true);
                ApplyAirtechPatches();
                patchDelayCountdown = 10;
            }
        }
        
        // Check P2 airtechable state
        bool p2Airtechable = IsPlayerAirtechable(moveID2, 2);
        
        // Detect when P2 BECOMES airtechable (start counting)
        if (p2Airtechable && !p2WasAirtechable) {
            p2DelayCounter = autoAirtechDelay.load();
            LogOut("[AUTO-AIRTECH] P2 became airtechable, starting delay: " + 
                   std::to_string(autoAirtechDelay.load()) + " visual frames", detailedLogging.load());
        }
        
        // Count down P2's delay and apply patches when it expires
        if (p2DelayCounter > 0) {
            p2DelayCounter--;
            if (p2DelayCounter == 0 && p2Airtechable) {
                LogOut("[AUTO-AIRTECH] P2 delay expired, applying patches", true);
                ApplyAirtechPatches();
                patchDelayCountdown = 10;
            }
        }
        
        // Update previous states
        p1WasAirtechable = p1Airtechable;
        p2WasAirtechable = p2Airtechable;
        
        // Handle temporary patches
        if (patchDelayCountdown > 0) {
            patchDelayCountdown--;
            if (patchDelayCountdown == 0 && patchesApplied) {
                RemoveAirtechPatches();
                LogOut("[AUTO-AIRTECH] Temporary patches removed", detailedLogging.load());
            }
        }
    } 
    // Instant-mode logic (delay = 0)
    else if (autoAirtechEnabled.load() && autoAirtechDelay.load() == 0) {
        // In instant mode, we still need to disable patches during airtech animation
        if ((p1IsAirteching || p2IsAirteching) && patchesApplied) {
            RemoveAirtechPatches();
            LogOut("[AUTO-AIRTECH] Removed patches during airtech (instant mode)", detailedLogging.load());
        }
        // And re-apply them when both players are no longer airteching
        else if (!p1IsAirteching && !p2IsAirteching && !patchesApplied) {
            ApplyAirtechPatches();
            LogOut("[AUTO-AIRTECH] Re-applied patches after airtech (instant mode)", detailedLogging.load());
        }
    }
}

// Helper to read the input buffer and current index for a player
/*bool ReadPlayerInputBuffer(int playerNum, uint8_t* outBuffer, int bufferLen, int& outCurrentIndex) {
    uintptr_t base = GetEFZBase();
    uintptr_t playerPtr = 0;
    uintptr_t baseOffset = (playerNum == 1) ? EFZ_BASE_OFFSET_P1 : EFZ_BASE_OFFSET_P2;
    if (!SafeReadMemory(base + baseOffset, &playerPtr, sizeof(uintptr_t)) || !playerPtr)
        return false;
    // Read buffer (0x1AB, length 0x180)
    if (!SafeReadMemory(playerPtr + 0x1AB, outBuffer, bufferLen))
        return false;
    // Read current index (0x260, 2 bytes)
    uint16_t idx = 0;
    if (!SafeReadMemory(playerPtr + 0x260, &idx, sizeof(uint16_t)))
        return false;
    outCurrentIndex = idx;
    return true;
}*/