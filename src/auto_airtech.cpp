#include "../include/auto_airtech.h"
#include "../include/constants.h"
#include "../include/utilities.h"
#include "../include/memory.h"
#include "../include/logger.h"
#include <thread>
#include <atomic>

// Global variables
bool p1InAirHitstun = false;
bool p2InAirHitstun = false;
int p1LastHitstunFrame = -1;
int p2LastHitstunFrame = -1;

// Original bytes for patches (exactly matching Cheat Engine)
char originalEnableBytes[2] = {0x74, 0x71};    // "je +71h" -> "nop nop"
char originalForwardBytes[2] = {0x75, 0x24};   // "jne +24h" -> "nop nop"
char originalBackwardBytes[2] = {0x75, 0x21};  // "jne +21h" -> "nop nop"
bool patchesApplied = false;

// NEW: Delay tracking for auto-airtech
struct AirtechDelayState {
    bool isDelaying;
    int delayFramesRemaining;
    int airtechAvailableFrame;
    bool wasInLaunchedHitstun;
};

static AirtechDelayState p1AirtechDelay = {false, 0, -1, false};
static AirtechDelayState p2AirtechDelay = {false, 0, -1, false};

bool CanAirtech(short moveID) {
    // Standard launched hitstun
    bool isLaunched = moveID >= LAUNCHED_HITSTUN_START && moveID <= LAUNCHED_HITSTUN_END;
    
    // Fire and electric states
    bool isFireOrElectric = moveID == FIRE_STATE || moveID == ELECTRIC_STATE;
    
    // Air blockstun states - make sure to include AIR_GUARD_ID (156)
    bool isAirBlockstun = moveID == AIR_GUARD_ID;
    
    return isLaunched || isFireOrElectric || isAirBlockstun;
}

void MonitorAutoAirtech(short moveID1, short moveID2) {
    static bool prevEnabled = false;
    static int prevDirection = -1;
    static int prevDelay = -1;

    bool directionChanged = prevDirection != autoAirtechDirection.load();
    bool enabledChanged = prevEnabled != autoAirtechEnabled.load();
    bool delayChanged = prevDelay != autoAirtechDelay.load();

    if (enabledChanged || directionChanged || delayChanged) {
        if (autoAirtechEnabled.load()) {
            if (autoAirtechDelay.load() == 0) {
                ApplyAirtechPatches();
            } else {
                RemoveAirtechPatches();
            }
        } else {
            RemoveAirtechPatches();
            LogOut("[AUTO-AIRTECH] Disabled", true);
        }
        prevEnabled = autoAirtechEnabled.load();
        prevDirection = autoAirtechDirection.load();
        prevDelay = autoAirtechDelay.load();
    }

    if (autoAirtechEnabled.load() && autoAirtechDelay.load() > 0) {
        static bool p1WasInAirtechState = false;
        static bool p2WasInAirtechState = false;
        static int p1DelayCounter = 0;
        static int p2DelayCounter = 0;
        static bool temporaryPatchesApplied = false;
        static int patchDuration = 0;

        bool p1InAirtechState = CanAirtech(moveID1);
        bool p2InAirtechState = CanAirtech(moveID2);

        // P1: Just LEFT airtechable state
        if (p1WasInAirtechState && !p1InAirtechState) {
            p1DelayCounter = autoAirtechDelay.load() * 3;
            temporaryPatchesApplied = false;
            LogOut("[AUTO-AIRTECH] P1 delay started: " + std::to_string(autoAirtechDelay.load()) + " frames after leaving airtechable state", true);
        }
        // P2: Just LEFT airtechable state
        if (p2WasInAirtechState && !p2InAirtechState) {
            p2DelayCounter = autoAirtechDelay.load() * 3;
            temporaryPatchesApplied = false;
            LogOut("[AUTO-AIRTECH] P2 delay started: " + std::to_string(autoAirtechDelay.load()) + " frames after leaving airtechable state", true);
        }

        // P1 delay processing
        if (p1DelayCounter > 0) {
            p1DelayCounter--;
            if (p1DelayCounter == 0) {
                if (!temporaryPatchesApplied) {
                    ApplyAirtechPatches();
                    temporaryPatchesApplied = true;
                }
                patchDuration = 20;
                LogOut("[AUTO-AIRTECH] P1 delay complete - applying airtech patches for 20 frames", true);
            }
        }
        // P2 delay processing
        if (p2DelayCounter > 0) {
            p2DelayCounter--;
            if (p2DelayCounter == 0) {
                if (!temporaryPatchesApplied) {
                    ApplyAirtechPatches();
                    temporaryPatchesApplied = true;
                }
                patchDuration = 20;
                LogOut("[AUTO-AIRTECH] P2 delay complete - applying airtech patches for 20 frames", true);
            }
        }
        // Manage patch duration
        if (temporaryPatchesApplied) {
            if (patchDuration > 0) {
                patchDuration--;
            } else {
                RemoveAirtechPatches();
                temporaryPatchesApplied = false;
            }
        }
        p1WasInAirtechState = p1InAirtechState;
        p2WasInAirtechState = p2InAirtechState;
    }
}

void ApplyAirtechPatches() {
    if (patchesApplied) {
        LogOut("[AUTO-AIRTECH] Patches already applied", detailedLogging.load());
        return;
    }
    
    uintptr_t base = GetEFZBase();
    if (!base) {
        LogOut("[AUTO-AIRTECH] Cannot get game base address", true);
        return;
    }
    
    // Store original bytes before patching (exactly as Cheat Engine does)
    SafeReadMemory(base + AIRTECH_ENABLE_ADDR, originalEnableBytes, 2);
    SafeReadMemory(base + AIRTECH_FORWARD_ADDR, originalForwardBytes, 2);
    SafeReadMemory(base + AIRTECH_BACKWARD_ADDR, originalBackwardBytes, 2);
    
    // STEP 1: Always apply the main enable patch (NOP the "je +71h" at F4FF)
    bool mainPatchSuccess = NopMemory(base + AIRTECH_ENABLE_ADDR, 2);
    if (!mainPatchSuccess) {
        LogOut("[AUTO-AIRTECH] Failed to apply main enable patch", true);
        return;
    }
    
    // STEP 2: Apply direction-specific patches (exactly like Cheat Engine)
    if (autoAirtechDirection.load() == 0) {
        // Forward airtech: NOP the forward check at F514 (like "Forwards" script)
        bool forwardPatchSuccess = NopMemory(base + AIRTECH_FORWARD_ADDR, 2);
        if (forwardPatchSuccess) {
            LogOut("[AUTO-AIRTECH] Applied forward airtech patches (NOPed F4FF and F514)", detailedLogging.load());
        } else {
            LogOut("[AUTO-AIRTECH] Failed to apply forward direction patch", true);
        }
    } else {
        // Backward airtech: NOP the backward check at F54F (like "Backwards" script)
        bool backwardPatchSuccess = NopMemory(base + AIRTECH_BACKWARD_ADDR, 2);
        if (backwardPatchSuccess) {
            LogOut("[AUTO-AIRTECH] Applied backward airtech patches (NOPed F4FF and F54F)", detailedLogging.load());
        } else {
            LogOut("[AUTO-AIRTECH] Failed to apply backward direction patch", true);
        }
    }
    
    patchesApplied = true;
}

void RemoveAirtechPatches() {
    if (!patchesApplied) {
        LogOut("[AUTO-AIRTECH] No patches to remove", detailedLogging.load());
        return;
    }
    
    uintptr_t base = GetEFZBase();
    if (!base) {
        LogOut("[AUTO-AIRTECH] Cannot get game base address for cleanup", true);
        return;
    }
    
    // Restore original bytes (exactly as Cheat Engine does)
    bool enableRestored = PatchMemory(base + AIRTECH_ENABLE_ADDR, originalEnableBytes, 2);
    bool forwardRestored = PatchMemory(base + AIRTECH_FORWARD_ADDR, originalForwardBytes, 2);
    bool backwardRestored = PatchMemory(base + AIRTECH_BACKWARD_ADDR, originalBackwardBytes, 2);
    
    if (enableRestored && forwardRestored && backwardRestored) {
        LogOut("[AUTO-AIRTECH] All airtech patches removed successfully", detailedLogging.load());
    } else {
        LogOut("[AUTO-AIRTECH] Warning: Some patches may not have been restored properly", true);
    }
    
    patchesApplied = false;
}