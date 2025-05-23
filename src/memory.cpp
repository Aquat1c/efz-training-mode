#include <fstream>
#include <iomanip>
#include <chrono>
#include "../include/memory.h"
#include "../include/constants.h"
#include "../include/utilities.h"
#include "../include/logger.h"
#include <windows.h>
#include <atomic>
#include <thread>
#include <sstream>

// Helper function for safe memory reading
bool SafeReadMemory(uintptr_t address, void* buffer, size_t size) {
    __try {
        memcpy(buffer, (void*)address, size);
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Helper function for safe memory writing
bool SafeWriteMemory(uintptr_t address, const void* data, size_t size) {
    __try {
        memcpy((void*)address, data, size);
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Enhanced version of ResolvePointer with additional validation
uintptr_t ResolvePointer(uintptr_t base, uintptr_t baseOffset, uintptr_t offset) {
    if (base == 0) {
        LogOut("[MEMORY] ResolvePointer called with null base", detailedLogging);
        return 0;
    }
    
    uintptr_t ptrAddr = base + baseOffset;
    
    // Validate the pointer address is within reasonable bounds
    // Using more permissive bounds for better compatibility
    if (ptrAddr < 0x1000 || ptrAddr > 0xFFFFFFFF) {
        LogOut("[MEMORY] Invalid pointer address: " + std::to_string(ptrAddr), detailedLogging);
        return 0;
    }
    
    uintptr_t ptrValue = 0;
    
    // Use our safe helper function instead of __try/__except directly
    if (!SafeReadMemory(ptrAddr, &ptrValue, sizeof(uintptr_t))) {
        LogOut("[MEMORY] Exception reading memory at: " + std::to_string(ptrAddr), true);
        return 0;
    }
    
    if (ptrValue == 0 || ptrValue > 0xFFFFFFFF) {
        LogOut("[MEMORY] Invalid pointer value: " + std::to_string(ptrValue), detailedLogging);
        return 0;
    }
    
    uintptr_t finalAddr = ptrValue + offset;
    
    // Verify the final address is valid with more permissive bounds
    if (finalAddr < 0x1000 || finalAddr > 0xFFFFFFFF) {
        LogOut("[MEMORY] Invalid final address: " + std::to_string(finalAddr), detailedLogging);
        return 0;
    }
    
    return finalAddr;
}

// Enhanced WriteGameMemory with added protections
void WriteGameMemory(uintptr_t address, const void* data, size_t size) {
    if (address == 0 || !data) {
        LogOut("[MEMORY] WriteGameMemory called with invalid parameters", detailedLogging);
        return;
    }

    // Verify address is within valid range
    if (address < 0x10000 || address > 0x7FFFFFFF) {
        LogOut("[MEMORY] Attempt to write to invalid address: " + std::to_string(address), true);
        return;
    }

    DWORD oldProtect;
    if (VirtualProtect((LPVOID)address, size, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        // Use our safe helper function instead of __try/__except directly
        if (!SafeWriteMemory(address, data, size)) {
            LogOut("[MEMORY] Exception writing to memory at: " + std::to_string(address), true);
        }
        VirtualProtect((LPVOID)address, size, oldProtect, &oldProtect);
    } else {
        LogOut("[MEMORY] Failed to change memory protection at: " + std::to_string(address), true);
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
        LogOut("[POSITION] Failed to resolve position addresses", true);
        return;
    }
    
    // Set X coordinate (this always works)
    SafeWriteMemory(xAddr, &x, sizeof(double));
    
    // For Y coordinate, we need to handle it differently
    if (y < 0.0) {
        // If we want negative Y (above ground), we need to put character in air state
        short airState = FALLING_ID;  // Use falling state for air positions
        
        // First set the Y position
        SafeWriteMemory(yAddr, &y, sizeof(double));
        
        // Then change move ID to air state if requested
        if (moveIDAddr && updateMoveID) {
            SafeWriteMemory(moveIDAddr, &airState, sizeof(short));
            LogOut("[POSITION] Changed character to air state for Y-position", detailedLogging);
        }
        
        // For more persistent Y position (less likely to be overridden)
        // Write the position twice with a small delay
        Sleep(1);
        SafeWriteMemory(yAddr, &y, sizeof(double));
    } else {
        // Normal ground position (y = 0 or positive)
        SafeWriteMemory(yAddr, &y, sizeof(double));
        
        // Force to idle state for ground positions if requested
        if (moveIDAddr && updateMoveID) {
            short idleState = IDLE_MOVE_ID;  // 0 = standing idle state
            SafeWriteMemory(moveIDAddr, &idleState, sizeof(short));
            LogOut("[POSITION] Changed character to idle state for ground position", detailedLogging);
        }
    }
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

void UpdatePlayerValues(uintptr_t base, uintptr_t baseOffsetP1, uintptr_t baseOffsetP2) {
    // Write values from displayData to game memory
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

uint8_t GetPlayerInputs(int playerNum) {
    uintptr_t base = GetEFZBase();
    if (!base) return 0;
    
    uintptr_t baseOffset = (playerNum == 1) ? EFZ_BASE_OFFSET_P1 : EFZ_BASE_OFFSET_P2;
    uintptr_t inputAddr = ResolvePointer(base, baseOffset, P1_INPUT_OFFSET);
    
    if (!inputAddr) return 0;
    
    uint8_t inputState = 0;
    memcpy(&inputState, (void*)inputAddr, sizeof(uint8_t));
    
    // Debug log when inputs change (uncommenting for development only)
    // static uint8_t lastInputState = 0;
    // if (inputState != lastInputState && detailedLogging) {
    //     LogOut("[INPUT] P" + std::to_string(playerNum) + " inputs: " + 
    //            std::to_string(inputState) + " (binary: " + 
    //            std::bitset<8>(inputState).to_string() + ")", true);
    //     lastInputState = inputState;
    // }
    
    return inputState;
}

// Add near other global variables
std::atomic<bool> rfFreezing(false);
std::atomic<double> rfFreezeValueP1(0.0);
std::atomic<double> rfFreezeValueP2(0.0);
std::thread rfFreezeThread;
bool rfThreadRunning = false;

// Improved RF freeze thread function with better error handling
void RFFreezeThreadFunc() {
    rfThreadRunning = true;
    
    while (rfThreadRunning) {
        if (rfFreezing.load()) {
            // Get current values to freeze
            double p1RF = rfFreezeValueP1.load();
            double p2RF = rfFreezeValueP2.load();
            
            // Only write if the game is running
            uintptr_t base = GetEFZBase();
            if (base) {
                // Use direct pointer access exactly as Cheat Engine does
                uintptr_t* p1Ptr = (uintptr_t*)(base + EFZ_BASE_OFFSET_P1);
                uintptr_t* p2Ptr = (uintptr_t*)(base + EFZ_BASE_OFFSET_P2);
                
                // Only proceed if pointers are valid
                if (!IsBadReadPtr(p1Ptr, sizeof(uintptr_t)) && !IsBadReadPtr(p2Ptr, sizeof(uintptr_t))) {
                    uintptr_t p1Base = *p1Ptr;
                    uintptr_t p2Base = *p2Ptr;
                    
                    if (p1Base && p2Base) {
                        // Calculate RF addresses
                        double* p1RFAddr = (double*)(p1Base + RF_OFFSET);
                        double* p2RFAddr = (double*)(p2Base + RF_OFFSET);
                        
                        // Only proceed if addresses are valid
                        if (!IsBadWritePtr(p1RFAddr, sizeof(double)) && !IsBadWritePtr(p2RFAddr, sizeof(double))) {
                            // Write values with memory protection handling
                            DWORD oldProtect1, oldProtect2;
                            
                            // P1 RF write - use VirtualProtect for reliable memory access
                            if (VirtualProtect(p1RFAddr, sizeof(double), PAGE_EXECUTE_READWRITE, &oldProtect1)) {
                                *p1RFAddr = p1RF; // Direct write without memcpy
                                VirtualProtect(p1RFAddr, sizeof(double), oldProtect1, &oldProtect1);
                            }
                            
                            // P2 RF write
                            if (VirtualProtect(p2RFAddr, sizeof(double), PAGE_EXECUTE_READWRITE, &oldProtect2)) {
                                *p2RFAddr = p2RF; // Direct write without memcpy
                                VirtualProtect(p2RFAddr, sizeof(double), oldProtect2, &oldProtect2);
                            }
                        }
                    }
                }
            }
        }
        // Update at 60Hz to match game's refresh rate
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
}

// Initialize the RF freeze thread
void InitRFFreezeThread() {
    if (!rfThreadRunning) {
        rfFreezeThread = std::thread(RFFreezeThreadFunc);
        rfFreezeThread.detach();
        LogOut("[RF] RF freeze thread initialized", true);
    }
}

// Start freezing RF values
void StartRFFreeze(double p1Value, double p2Value) {
    // Save values to atomic variables for thread-safe access
    rfFreezeValueP1.store(p1Value);
    rfFreezeValueP2.store(p2Value);
    
    // Enable freezing
    rfFreezing.store(true);
    
    LogOut("[RF] Started freezing RF values: P1=" + std::to_string(p1Value) + 
           ", P2=" + std::to_string(p2Value), true);
}

// Stop freezing RF values
void StopRFFreeze() {
    rfFreezing.store(false);
    LogOut("[RF] Stopped freezing RF values", true);
}