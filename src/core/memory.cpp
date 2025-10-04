#include <fstream>
#include <iomanip>
#include <chrono>
#include "../include/core/memory.h"
#include "../include/core/constants.h"
#include "../include/utils/utilities.h"
#include "../include/utils/config.h"

#include "../include/core/logger.h"
#include <windows.h>
#include <atomic>
#include <thread>
#include <sstream>
#include <algorithm>
#include <cmath>
// For global shutdown flag
#include "../include/core/globals.h"

// Add static variables for position saving
static double saved_x1 = 240.0, saved_y1 = 0.0;
static double saved_x2 = 400.0, saved_y2 = 0.0;

// Helper function for safe memory reading
bool SafeWriteMemory(uintptr_t address, const void* data, size_t size) {
    if (!address || !data || size == 0) return false;

    DWORD oldProtect;
    if (!VirtualProtect((LPVOID)address, size, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        return false;
    }

    bool success = false;
    if (!IsBadWritePtr((LPVOID)address, size)) {
        memcpy((void*)address, data, size);
        success = true;
    }

    VirtualProtect((LPVOID)address, size, oldProtect, &oldProtect);
    return success;
}

bool SafeReadMemory(uintptr_t address, void* buffer, size_t size) {
    if (!address || !buffer || size == 0) {
        return false;
    }
    
    // Use IsBadReadPtr to check if we can read from the address safely
    if (!IsBadReadPtr((LPVOID)address, size)) {
        memcpy(buffer, (void*)address, size);
        return true;
    }
    
    return false;
}

uintptr_t ResolvePointer(uintptr_t base, uintptr_t baseOffset, uintptr_t offset) {
    if (base == 0) return 0;
    
    uintptr_t ptrAddr = base + baseOffset;
    if (ptrAddr < 0x1000 || ptrAddr > 0xFFFFFFFF) return 0;
    
    uintptr_t ptrValue = 0;
    if (!SafeReadMemory(ptrAddr, &ptrValue, sizeof(uintptr_t))) return 0;
    
    if (ptrValue == 0 || ptrValue > 0xFFFFFFFF) return 0;
    
    uintptr_t finalAddr = ptrValue + offset;
    if (finalAddr < 0x1000 || finalAddr > 0xFFFFFFFF) return 0;
    
    return finalAddr;
}

// Enhanced WriteGameMemory with added protections
void WriteGameMemory(uintptr_t address, const void* data, size_t size) {
    if (address == 0 || !data) {
        LogOut("[MEMORY] Invalid address or data pointer", true); // Keep errors visible
        return;
    }

    // Verify address is within valid range
    if (address < 0x10000 || address > 0x7FFFFFFF) {
        LogOut("[MEMORY] Address out of valid range: " + std::to_string(address), true); // Keep errors visible
        return;
    }

    DWORD oldProtect;
    if (VirtualProtect((LPVOID)address, size, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        memcpy((void*)address, data, size);
        VirtualProtect((LPVOID)address, size, oldProtect, &oldProtect);
        // Change this line to use detailedLogging instead of always showing
        LogOut("[MEMORY] WriteGameMemory: Successfully wrote " + std::to_string(size) + 
               " bytes to 0x" + std::to_string(address), detailedLogging.load());
    } else {
        LogOut("[MEMORY] Failed to change memory protection at address: 0x" + 
               std::to_string(address), true); // Keep errors visible
    }
}

bool PatchMemory(uintptr_t address, const char* bytes, size_t length) {
    DWORD oldProtect;
    
    // Change memory protection to allow writing
    if (!VirtualProtect((LPVOID)address, length, PAGE_EXECUTE_READWRITE, &oldProtect))
        return false;
        
    // Write the bytes
    memcpy((void*)address, bytes, length);
    
    // Restore the old protection
    VirtualProtect((LPVOID)address, length, oldProtect, &oldProtect);
    
    return true;
}

bool NopMemory(uintptr_t address, size_t length) {
    DWORD oldProtect;
    
    // Change memory protection to allow writing
    if (!VirtualProtect((LPVOID)address, length, PAGE_EXECUTE_READWRITE, &oldProtect))
        return false;
        
    // Fill with NOPs (0x90)
    memset((void*)address, 0x90, length);
    
    // Restore the old protection
    VirtualProtect((LPVOID)address, length, oldProtect, &oldProtect);
    
    return true;
}

// Enhanced version of SetPlayerPosition with better Y-position handling
void SetPlayerPosition(uintptr_t base, uintptr_t playerOffset, double x, double y, bool updateMoveID) {
    // Resolve position pointers
    uintptr_t xAddr = ResolvePointer(base, playerOffset, XPOS_OFFSET);
    uintptr_t yAddr = ResolvePointer(base, playerOffset, YPOS_OFFSET);
    uintptr_t moveIDAddr = ResolvePointer(base, playerOffset, MOVE_ID_OFFSET);
    
    if (!xAddr || !yAddr) {
        LogOut("[MEMORY] Failed to resolve position pointers", true);
        return;
    }
    
    // Set X coordinate
    if (!SafeWriteMemory(xAddr, &x, sizeof(double))) {
        LogOut("[MEMORY] Failed to write X position", true);
    }
    
    // Set Y coordinate
    if (!SafeWriteMemory(yAddr, &y, sizeof(double))) {
        LogOut("[MEMORY] Failed to write Y position", true);
    }
    
    // Reset Y velocity to zero to prevent unintended movement
    uintptr_t yVelAddr = ResolvePointer(base, playerOffset, YVEL_OFFSET);
    if (yVelAddr) {
        double zeroVel = 0.0;
        SafeWriteMemory(yVelAddr, &zeroVel, sizeof(double));
    }
    
    // If requested, update moveID to reset the character state
    if (updateMoveID && moveIDAddr) {
        short idleID = IDLE_MOVE_ID;
        if (!SafeWriteMemory(moveIDAddr, &idleID, sizeof(short))) {
            LogOut("[MEMORY] Failed to reset moveID", true);
        }
    }
    
    LogOut("[MEMORY] Set position - X: " + std::to_string(x) + ", Y: " + std::to_string(y), detailedLogging.load());
}

// Direct RF value setter that matches Cheat Engine's approach
bool SetRFValuesDirect(double p1RF, double p2RF) {
    uintptr_t base = GetEFZBase();
    if (!base) return false;
    
    // Use direct pointer access exactly as Cheat Engine does
    uintptr_t* p1Ptr = (uintptr_t*)(base + EFZ_BASE_OFFSET_P1);
    uintptr_t* p2Ptr = (uintptr_t*)(base + EFZ_BASE_OFFSET_P2);
    
    // Validate pointers are readable
    if (IsBadReadPtr(p1Ptr, sizeof(uintptr_t)) || IsBadReadPtr(p2Ptr, sizeof(uintptr_t))) {
        LogOut("[RF] Invalid player base pointers", detailedLogging);
        return false;
    }
    
    // Follow the pointers to get player structures
    uintptr_t p1Base = *p1Ptr;
    uintptr_t p2Base = *p2Ptr;
    
    if (!p1Base || !p2Base) {
        LogOut("[RF] Player structures not initialized", detailedLogging);
        return false;
    }
    
    // Calculate RF addresses
    double* p1RFAddr = (double*)(p1Base + RF_OFFSET);
    double* p2RFAddr = (double*)(p2Base + RF_OFFSET);
    
    // Validate these addresses
    if (IsBadWritePtr(p1RFAddr, sizeof(double)) || IsBadWritePtr(p2RFAddr, sizeof(double))) {
        LogOut("[RF] Invalid RF addresses", detailedLogging);
        return false;
    }
    
    // Write values with memory protection handling
    DWORD oldProtect1, oldProtect2;
    bool success = true;
    
    // P1 RF write
    if (VirtualProtect(p1RFAddr, sizeof(double), PAGE_EXECUTE_READWRITE, &oldProtect1)) {
        *p1RFAddr = p1RF; // Direct write - no memcpy
        VirtualProtect(p1RFAddr, sizeof(double), oldProtect1, &oldProtect1);
        
        // Verify the write
        double verification = 0.0;
        memcpy(&verification, p1RFAddr, sizeof(double));
        if (verification != p1RF) {
            LogOut("[RF] P1 RF verification failed: wrote " + std::to_string(p1RF) + 
                   " but read back " + std::to_string(verification), true);
            success = false;
        }
    } else {
        LogOut("[RF] P1 VirtualProtect failed", true);
        success = false;
    }
    
    // P2 RF write
    if (VirtualProtect(p2RFAddr, sizeof(double), PAGE_EXECUTE_READWRITE, &oldProtect2)) {
        *p2RFAddr = p2RF; // Direct write - no memcpy
        VirtualProtect(p2RFAddr, sizeof(double), oldProtect2, &oldProtect2);
        
        // Verify the write
        double verification = 0.0;
        memcpy(&verification, p2RFAddr, sizeof(double));
        if (verification != p2RF) {
            LogOut("[RF] P2 RF verification failed: wrote " + std::to_string(p2RF) + 
                   " but read back " + std::to_string(verification), true);
            success = false;
        }
    } else {
        LogOut("[RF] P2 VirtualProtect failed", true);
        success = false;
    }
    
    return success;
}

// Set IC Color values directly (similar to SetRFValuesDirect)
bool SetICColorDirect(bool p1BlueIC, bool p2BlueIC) {
    uintptr_t base = GetEFZBase();
    if (!base) return false;
    
    // Use direct pointer access exactly as Cheat Engine does
    uintptr_t* p1Ptr = (uintptr_t*)(base + EFZ_BASE_OFFSET_P1);
    uintptr_t* p2Ptr = (uintptr_t*)(base + EFZ_BASE_OFFSET_P2);
    
    // Validate pointers are readable
    if (IsBadReadPtr(p1Ptr, sizeof(uintptr_t)) || IsBadReadPtr(p2Ptr, sizeof(uintptr_t))) {
        LogOut("[IC] Invalid player base pointers", detailedLogging.load());
        return false;
    }
    
    // Follow the pointers to get player structures
    uintptr_t p1Base = *p1Ptr;
    uintptr_t p2Base = *p2Ptr;
    
    if (!p1Base || !p2Base) {
        LogOut("[IC] Player structures not initialized", detailedLogging.load());
        return false;
    }
    
    // Calculate IC color addresses (offset 0x120 = 4-byte int)
    int* p1ICAddr = (int*)(p1Base + IC_COLOR_OFFSET);
    int* p2ICAddr = (int*)(p2Base + IC_COLOR_OFFSET);
    
    // Validate these addresses
    if (IsBadWritePtr(p1ICAddr, sizeof(int)) || IsBadWritePtr(p2ICAddr, sizeof(int))) {
        LogOut("[IC] Invalid IC color addresses", detailedLogging.load());
        return false;
    }
    
    // Convert bool to int (0=red IC, 1=blue IC)
    int p1ICValue = p1BlueIC ? 1 : 0;
    int p2ICValue = p2BlueIC ? 1 : 0;
    
    // Write values with memory protection handling
    DWORD oldProtect1, oldProtect2;
    bool success = true;
    
    // P1 IC color write
    if (VirtualProtect(p1ICAddr, sizeof(int), PAGE_EXECUTE_READWRITE, &oldProtect1)) {
        *p1ICAddr = p1ICValue;
        VirtualProtect(p1ICAddr, sizeof(int), oldProtect1, &oldProtect1);
        
        // Verify the write
        int verification = 0;
        memcpy(&verification, p1ICAddr, sizeof(int));
        if (verification != p1ICValue) {
            LogOut("[IC] P1 IC verification failed: wrote " + std::to_string(p1ICValue) + 
                   " but read back " + std::to_string(verification), true);
            success = false;
        }
    } else {
        LogOut("[IC] P1 VirtualProtect failed", true);
        success = false;
    }
    
    // P2 IC color write
    if (VirtualProtect(p2ICAddr, sizeof(int), PAGE_EXECUTE_READWRITE, &oldProtect2)) {
        *p2ICAddr = p2ICValue;
        VirtualProtect(p2ICAddr, sizeof(int), oldProtect2, &oldProtect2);
        
        // Verify the write
        int verification = 0;
        memcpy(&verification, p2ICAddr, sizeof(int));
        if (verification != p2ICValue) {
            LogOut("[IC] P2 IC verification failed: wrote " + std::to_string(p2ICValue) + 
                   " but read back " + std::to_string(verification), true);
            success = false;
        }
    } else {
        LogOut("[IC] P2 VirtualProtect failed", true);
        success = false;
    }
    
    return success;
}

void UpdatePlayerValues(uintptr_t base, uintptr_t baseOffsetP1, uintptr_t baseOffsetP2) {
    // This function applies values from the displayData struct to the game
    // (Used by the GUI dialogs)
    uintptr_t hpAddr1 = ResolvePointer(base, baseOffsetP1, HP_OFFSET);
    uintptr_t meterAddr1 = ResolvePointer(base, baseOffsetP1, METER_OFFSET);
    uintptr_t rfAddr1 = ResolvePointer(base, baseOffsetP1, RF_OFFSET);
    uintptr_t xAddr1 = ResolvePointer(base, baseOffsetP1, XPOS_OFFSET);
    uintptr_t yAddr1 = ResolvePointer(base, baseOffsetP1, YPOS_OFFSET);

    uintptr_t hpAddr2 = ResolvePointer(base, baseOffsetP2, HP_OFFSET);
    uintptr_t meterAddr2 = ResolvePointer(base, baseOffsetP2, METER_OFFSET);
    uintptr_t rfAddr2 = ResolvePointer(base, baseOffsetP2, RF_OFFSET);
    uintptr_t xAddr2 = ResolvePointer(base, baseOffsetP2, XPOS_OFFSET);
    uintptr_t yAddr2 = ResolvePointer(base, baseOffsetP2, YPOS_OFFSET);

    if (hpAddr1) WriteGameMemory(hpAddr1, &displayData.hp1, sizeof(WORD));
    if (meterAddr1) WriteGameMemory(meterAddr1, &displayData.meter1, sizeof(WORD));
    
    // CRITICAL FIX: Write RF as float (4 bytes), not double (8 bytes)
    if (rfAddr1) {
        float rf = static_cast<float>(displayData.rf1);
        WriteGameMemory(rfAddr1, &rf, sizeof(float));
    }
    
    if (xAddr1) WriteGameMemory(xAddr1, &displayData.x1, sizeof(double));
    if (yAddr1) WriteGameMemory(yAddr1, &displayData.y1, sizeof(double));

    if (hpAddr2) WriteGameMemory(hpAddr2, &displayData.hp2, sizeof(WORD));
    if (meterAddr2) WriteGameMemory(meterAddr2, &displayData.meter2, sizeof(WORD));
    
    // CRITICAL FIX: Write RF as float (4 bytes), not double (8 bytes)
    if (rfAddr2) {
        float rf = static_cast<float>(displayData.rf2);
        WriteGameMemory(rfAddr2, &rf, sizeof(float));
    }
    
    if (xAddr2) WriteGameMemory(xAddr2, &displayData.x2, sizeof(double));
    if (yAddr2) WriteGameMemory(yAddr2, &displayData.y2, sizeof(double));

    LogOut("[MEMORY] Applied values from dialog: P1[HP:" + std::to_string(displayData.hp1) +
        " Meter:" + std::to_string(displayData.meter1) +
        " RF:" + std::to_string(displayData.rf1) +
        " X:" + std::to_string(displayData.x1) +
        " Y:" + std::to_string(displayData.y1) +
        "] P2[HP:" + std::to_string(displayData.hp2) +
        " Meter:" + std::to_string(displayData.meter2) +
        " RF:" + std::to_string(displayData.rf2) +
        " X:" + std::to_string(displayData.x2) +
        " Y:" + std::to_string(displayData.y2) + "]", true);
}

// New function to save current player positions
void SavePlayerPositions(uintptr_t base) {
    if (!base) return;

    uintptr_t xAddr1 = ResolvePointer(base, EFZ_BASE_OFFSET_P1, XPOS_OFFSET);
    uintptr_t yAddr1 = ResolvePointer(base, EFZ_BASE_OFFSET_P1, YPOS_OFFSET);
    uintptr_t xAddr2 = ResolvePointer(base, EFZ_BASE_OFFSET_P2, XPOS_OFFSET);
    uintptr_t yAddr2 = ResolvePointer(base, EFZ_BASE_OFFSET_P2, YPOS_OFFSET);

    if (xAddr1) SafeReadMemory(xAddr1, &saved_x1, sizeof(double));
    if (yAddr1) SafeReadMemory(yAddr1, &saved_y1, sizeof(double));
    if (xAddr2) SafeReadMemory(xAddr2, &saved_x2, sizeof(double));
    if (yAddr2) SafeReadMemory(yAddr2, &saved_y2, sizeof(double));

    LogOut("[MEMORY] Saved positions: P1(" + std::to_string(saved_x1) + "), P2(" + std::to_string(saved_x2) + ")", true);
}

// New function to load saved player positions
void LoadPlayerPositions(uintptr_t base) {
    if (!base) return;

    LogOut("[POSITION] Teleporting to recorded positions - P1(" + 
           std::to_string(saved_x1) + "," + std::to_string(saved_y1) + 
           ") P2(" + std::to_string(saved_x2) + "," + std::to_string(saved_y2) + ")", true);

    // Set player positions. If Y is non-zero, we don't want to force an idle state.
    SetPlayerPosition(base, EFZ_BASE_OFFSET_P1, saved_x1, saved_y1, (saved_y1 == 0.0));
    SetPlayerPosition(base, EFZ_BASE_OFFSET_P2, saved_x2, saved_y2, (saved_y2 == 0.0));

    // Double-check Y positions after setting
    uintptr_t yAddr1 = ResolvePointer(base, EFZ_BASE_OFFSET_P1, YPOS_OFFSET);
    uintptr_t yAddr2 = ResolvePointer(base, EFZ_BASE_OFFSET_P2, YPOS_OFFSET);

    double verifyY1 = 0.0, verifyY2 = 0.0;
    if (yAddr1) SafeReadMemory(yAddr1, &verifyY1, sizeof(double));
    if (yAddr2) SafeReadMemory(yAddr2, &verifyY2, sizeof(double));

    LogOut("[POSITION] Verified Y positions after teleport - P1:" + 
          std::to_string(verifyY1) + ", P2:" + std::to_string(verifyY2), true);

    // If Y positions were not applied, try setting them with air state
    if ((saved_y1 > 0.1 && verifyY1 < 0.1) || (saved_y2 > 0.1 && verifyY2 < 0.1)) {
        LogOut("[POSITION] Y positions were reset by game, forcing air state", true);
        
        uintptr_t moveIDAddr1 = ResolvePointer(base, EFZ_BASE_OFFSET_P1, MOVE_ID_OFFSET);
        uintptr_t moveIDAddr2 = ResolvePointer(base, EFZ_BASE_OFFSET_P2, MOVE_ID_OFFSET);
        
        if (moveIDAddr1 && saved_y1 > 0.1) {
            short airState = FALLING_ID;
            SafeWriteMemory(moveIDAddr1, &airState, sizeof(short));
            SafeWriteMemory(yAddr1, &saved_y1, sizeof(double));
        }
        
        if (moveIDAddr2 && saved_y2 > 0.1) {
            short airState = FALLING_ID;
            SafeWriteMemory(moveIDAddr2, &airState, sizeof(short));
            SafeWriteMemory(yAddr2, &saved_y2, sizeof(double));
        }
    }
}

// Add this function implementation at the end of the file
uint8_t GetPlayerInputs(int playerNum) {
    uintptr_t base = GetEFZBase();
    if (!base) return 0;
    
    uintptr_t baseOffset = (playerNum == 1) ? EFZ_BASE_OFFSET_P1 : EFZ_BASE_OFFSET_P2;
    uintptr_t playerPtr = 0;
    if (!SafeReadMemory(base + baseOffset, &playerPtr, sizeof(uintptr_t)) || !playerPtr) {
        return 0;
    }
    
    // Read input from offset 0xB8
    uint8_t inputs = 0;
    SafeReadMemory(playerPtr + P1_INPUT_OFFSET, &inputs, sizeof(uint8_t));
    return inputs;
}

bool GetPlayerFacingDirection(int playerNum) {
    uintptr_t base = GetEFZBase();
    if (!base) return true; // Default to facing right if can't read
    
    uintptr_t baseOffset = (playerNum == 1) ? EFZ_BASE_OFFSET_P1 : EFZ_BASE_OFFSET_P2;
    uintptr_t facingAddr = ResolvePointer(base, baseOffset, FACING_DIRECTION_OFFSET);
    
    if (!facingAddr) return true; // Default to facing right if can't resolve
    
    uint8_t facingValue = 0;
    if (!SafeReadMemory(facingAddr, &facingValue, sizeof(uint8_t))) {
        return true; // Default to facing right if can't read
    }
    
    // 1 = facing right, 255 (0xFF) = facing left
    return facingValue == 1;
}

// Add this function that updates all values except RF
void UpdatePlayerValuesExceptRF(uintptr_t base, uintptr_t baseOffsetP1, uintptr_t baseOffsetP2) {
    // Write values from displayData to game memory
    uintptr_t hpAddr1 = ResolvePointer(base, baseOffsetP1, HP_OFFSET);
    uintptr_t meterAddr1 = ResolvePointer(base, baseOffsetP1, METER_OFFSET);
    uintptr_t xAddr1 = ResolvePointer(base, baseOffsetP1, XPOS_OFFSET);
    uintptr_t yAddr1 = ResolvePointer(base, baseOffsetP1, YPOS_OFFSET);

    uintptr_t hpAddr2 = ResolvePointer(base, baseOffsetP2, HP_OFFSET);
    uintptr_t meterAddr2 = ResolvePointer(base, baseOffsetP2, METER_OFFSET);
    uintptr_t xAddr2 = ResolvePointer(base, baseOffsetP2, XPOS_OFFSET);
    uintptr_t yAddr2 = ResolvePointer(base, baseOffsetP2, YPOS_OFFSET);

    if (hpAddr1) WriteGameMemory(hpAddr1, &displayData.hp1, sizeof(WORD));
    if (meterAddr1) WriteGameMemory(meterAddr1, &displayData.meter1, sizeof(WORD));
    if (xAddr1) WriteGameMemory(xAddr1, &displayData.x1, sizeof(double));
    if (yAddr1) WriteGameMemory(yAddr1, &displayData.y1, sizeof(double));

    if (hpAddr2) WriteGameMemory(hpAddr2, &displayData.hp2, sizeof(WORD));
    if (meterAddr2) WriteGameMemory(meterAddr2, &displayData.meter2, sizeof(WORD));
    if (xAddr2) WriteGameMemory(xAddr2, &displayData.x2, sizeof(double));
    if (yAddr2) WriteGameMemory(yAddr2, &displayData.y2, sizeof(double));
}

// Add near other global variables
std::atomic<bool> rfFreezing(false);
std::atomic<bool> rfFreezeP1Active(false);
std::atomic<bool> rfFreezeP2Active(false);
std::atomic<double> rfFreezeValueP1(0.0);
std::atomic<double> rfFreezeValueP2(0.0);
std::thread rfFreezeThread;
bool rfThreadRunning = false;

// Improved RF freeze thread function with better error handling
void RFFreezeThreadFunc() {
    rfThreadRunning = true;
    int sleepMs = 10;                // default ~100 Hz when active
    const int minSleepMs = 5;        // lower bound when values are drifting
    const int maxSleepMs = 40;       // back off to ~25 Hz when stable
    int stableIters = 0;
    auto nearlyEqual = [](double a, double b) {
        return fabs(a - b) < 1e-6;   // tiny tolerance for float write verification
    };
    
    while (rfThreadRunning && !g_isShuttingDown.load()) {
        if (rfFreezing.load()) {
            uintptr_t base = GetEFZBase();
            if (base) {
                // Use direct pointer access exactly as Cheat Engine does
                uintptr_t* p1Ptr = (uintptr_t*)(base + EFZ_BASE_OFFSET_P1);
                uintptr_t* p2Ptr = (uintptr_t*)(base + EFZ_BASE_OFFSET_P2);
                
                // Validate pointers are readable using IsBadReadPtr
                if (!IsBadReadPtr(p1Ptr, sizeof(uintptr_t)) && !IsBadReadPtr(p2Ptr, sizeof(uintptr_t))) {
                    // Follow the pointers to get player structures
                    uintptr_t p1Base = *p1Ptr;
                    uintptr_t p2Base = *p2Ptr;
                    
                    if (p1Base && p2Base) {
                        // Calculate RF addresses
                        double* p1RFAddr = (double*)(p1Base + RF_OFFSET);
                        double* p2RFAddr = (double*)(p2Base + RF_OFFSET);
                        
                        // Validate these addresses using IsBadWritePtr
                        if (!IsBadWritePtr(p1RFAddr, sizeof(double)) && !IsBadWritePtr(p2RFAddr, sizeof(double))) {
                            // Only write if value changed to avoid constant page flips
                            double targetP1 = rfFreezeValueP1.load();
                            double targetP2 = rfFreezeValueP2.load();
                            double curP1 = *p1RFAddr;
                            double curP2 = *p2RFAddr;
                            bool wrote = false;

                            if (rfFreezeP1Active.load() && !nearlyEqual(curP1, targetP1)) {
                                DWORD oldProtect1;
                                if (VirtualProtect(p1RFAddr, sizeof(double), PAGE_EXECUTE_READWRITE, &oldProtect1)) {
                                    *p1RFAddr = targetP1;
                                    VirtualProtect(p1RFAddr, sizeof(double), oldProtect1, &oldProtect1);
                                    wrote = true;
                                }
                            }
                            if (rfFreezeP2Active.load() && !nearlyEqual(curP2, targetP2)) {
                                DWORD oldProtect2;
                                if (VirtualProtect(p2RFAddr, sizeof(double), PAGE_EXECUTE_READWRITE, &oldProtect2)) {
                                    *p2RFAddr = targetP2;
                                    VirtualProtect(p2RFAddr, sizeof(double), oldProtect2, &oldProtect2);
                                    wrote = true;
                                }
                            }

                            // Adjust backoff
                if (wrote) {
                                sleepMs = minSleepMs;
                                stableIters = 0;
                            } else {
                                stableIters++;
                                if (stableIters > 3) {
                    sleepMs = (std::min)(sleepMs * 2, maxSleepMs);
                                }
                            }
                        }
                    }
                }
            }
        }
        else {
            // When not freezing, back off considerably and avoid memory touching
            sleepMs = maxSleepMs;
        }
        
        Sleep(sleepMs);
    }
}

// Initialize the RF freeze thread
void InitRFFreezeThread() {
    if (rfThreadRunning) return;
    
    rfThreadRunning = true;
    rfFreezeThread = std::thread(RFFreezeThreadFunc);
    rfFreezeThread.detach();
    
    // Only show in detailed mode
    LogOut("[RF] RF freeze thread initialized", detailedLogging.load());
}

// Start freezing RF values
void StartRFFreeze(double p1Value, double p2Value) {
    // Save values to atomic variables for thread-safe access
    rfFreezeValueP1.store(p1Value);
    rfFreezeValueP2.store(p2Value);
    
    // Enable freezing
    rfFreezing.store(true);
    rfFreezeP1Active.store(true);
    rfFreezeP2Active.store(true);
    
    LogOut("[RF] Started freezing RF values: P1=" + std::to_string(p1Value) + 
           ", P2=" + std::to_string(p2Value), detailedLogging.load());
}

// Start freezing for one player only
void StartRFFreezeOne(int player, double value) {
    if (player == 1) {
        rfFreezeValueP1.store(value);
        rfFreezeP1Active.store(true);
    } else if (player == 2) {
        rfFreezeValueP2.store(value);
        rfFreezeP2Active.store(true);
    } else {
        return;
    }
    // Ensure global switch is on if any side is active
    rfFreezing.store(rfFreezeP1Active.load() || rfFreezeP2Active.load());
    LogOut(std::string("[RF] Started single-side RF freeze for P") + (player==1?"1":"2") +
           " value=" + std::to_string(value), detailedLogging.load());
}

// Stop freezing RF values
void StopRFFreeze() {
    rfFreezing.store(false);
    rfFreezeP1Active.store(false);
    rfFreezeP2Active.store(false);
    LogOut("[RF] Stopped freezing RF values", detailedLogging.load());
}

void StopRFFreezePlayer(int player) {
    if (player == 1) {
        rfFreezeP1Active.store(false);
    } else if (player == 2) {
        rfFreezeP2Active.store(false);
    } else {
        return;
    }
    // If neither side is active, flip the global switch off
    if (!rfFreezeP1Active.load() && !rfFreezeP2Active.load()) {
        rfFreezing.store(false);
    }
    LogOut(std::string("[RF] Stopped RF freeze for P") + (player==1?"1":"2"), detailedLogging.load());
}

// Stop the RF freeze background thread entirely
void StopRFFreezeThread() {
    if (rfThreadRunning) {
        rfThreadRunning = false;
        LogOut("[RF] RF freeze thread signaled to stop", detailedLogging.load());
    }
}

// Allow external modules to toggle neutral-only RF freeze behavior
void SetRFFreezeNeutralOnly(bool enabled) {
    // Settings are already read inside UpdateRFFreezeTick via Config::GetSettings(),
    // but this setter is kept for compatibility and potential future caching.
    // If a cached flag is introduced later, wire it here.
    (void)enabled; // no-op for now
}

// Lightweight single-tick updater to be driven by FrameDataMonitor when desired
void UpdateRFFreezeTick() {
    if (!rfFreezing.load()) return;
    uintptr_t base = GetEFZBase();
    if (!base) return;
    uintptr_t p1Base = 0, p2Base = 0;
    if (!SafeReadMemory(base + EFZ_BASE_OFFSET_P1, &p1Base, sizeof(p1Base))) return;
    if (!SafeReadMemory(base + EFZ_BASE_OFFSET_P2, &p2Base, sizeof(p2Base))) return;
    if (!p1Base || !p2Base) return;
    double* p1RFAddr = (double*)(p1Base + RF_OFFSET);
    double* p2RFAddr = (double*)(p2Base + RF_OFFSET);
    // Optional neutral-only gating
    bool neutralOnly = Config::GetSettings().freezeRFOnlyWhenNeutral;
    auto isAllowedNeutral = [](short m){ return (m==0 || m==1 || m==2 || m==3 || m==4 || m==7 || m==8 || m==9 || m==13); };
    short m1=0, m2=0;
    if (neutralOnly) {
        short t1=0, t2=0;
        SafeReadMemory(p1Base + MOVE_ID_OFFSET, &t1, sizeof(t1));
        SafeReadMemory(p2Base + MOVE_ID_OFFSET, &t2, sizeof(t2));
        m1 = t1; m2 = t2;
    }
    __try {
        DWORD old1=0, old2=0;
        double t1 = rfFreezeValueP1.load();
        double t2 = rfFreezeValueP2.load();
        bool canP1 = rfFreezeP1Active.load() && (!neutralOnly || isAllowedNeutral(m1));
        bool canP2 = rfFreezeP2Active.load() && (!neutralOnly || isAllowedNeutral(m2));
        if (canP1 && p1RFAddr && VirtualProtect(p1RFAddr, sizeof(double), PAGE_EXECUTE_READWRITE, &old1)) { *p1RFAddr = t1; VirtualProtect(p1RFAddr, sizeof(double), old1, &old1); }
        if (canP2 && p2RFAddr && VirtualProtect(p2RFAddr, sizeof(double), PAGE_EXECUTE_READWRITE, &old2)) { *p2RFAddr = t2; VirtualProtect(p2RFAddr, sizeof(double), old2, &old2); }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        // Ignore access faults; tick is best-effort
    }
}
