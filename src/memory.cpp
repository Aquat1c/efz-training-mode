#include "../include/memory.h"
#include "../include/constants.h"
#include "../include/utilities.h"
#include "../include/logger.h"
#include <windows.h>

uintptr_t ResolvePointer(uintptr_t base, uintptr_t baseOffset, uintptr_t offset) {
    if (base == 0) return 0;
    
    uintptr_t ptrAddr = base + baseOffset;
    uintptr_t ptrValue = 0;
    memcpy(&ptrValue, (void*)ptrAddr, sizeof(uintptr_t));
    
    if (ptrValue == 0 || ptrValue > 0x7FFFFFFF)
        return 0;
        
    return ptrValue + offset;
}

void WriteGameMemory(uintptr_t address, const void* data, size_t size) {
    if (address == 0) return;

    DWORD oldProtect;
    if (VirtualProtect((LPVOID)address, size, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        memcpy((void*)address, data, size);
        VirtualProtect((LPVOID)address, size, oldProtect, &oldProtect);
    }
}

void SetPlayerPosition(uintptr_t base, uintptr_t baseOffset, double x, double y) {
    // Position validation to avoid teleporting players offscreen
    const double MIN_X = -100.0;
    const double MAX_X = 700.0;
    const double MIN_Y = 0.0;
    const double MAX_Y = 300.0;

    // Clamp positions to safe values
    x = CLAMP(x, MIN_X, MAX_X);
    y = CLAMP(y, MIN_Y, MAX_Y);

    uintptr_t xAddr = ResolvePointer(base, baseOffset, XPOS_OFFSET);
    uintptr_t yAddr = ResolvePointer(base, baseOffset, YPOS_OFFSET);

    // Log before writing to memory
    LogOut("[POSITION] Setting position to " + FormatPosition(x, y), detailedLogging);

    if (xAddr) WriteGameMemory(xAddr, &x, sizeof(double));
    if (yAddr) WriteGameMemory(yAddr, &y, sizeof(double));
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
    if (rfAddr1) {
        float rf = static_cast<float>(displayData.rf1);
        WriteGameMemory(rfAddr1, &rf, sizeof(float));
    }
    if (xAddr1) WriteGameMemory(xAddr1, &displayData.x1, sizeof(double));
    if (yAddr1) WriteGameMemory(yAddr1, &displayData.y1, sizeof(double));

    if (hpAddr2) WriteGameMemory(hpAddr2, &displayData.hp2, sizeof(WORD));
    if (meterAddr2) WriteGameMemory(meterAddr2, &displayData.meter2, sizeof(WORD));
    if (rfAddr2) {
        float rf = static_cast<float>(displayData.rf2);
        WriteGameMemory(rfAddr2, &rf, sizeof(float));
    }
    if (xAddr2) WriteGameMemory(xAddr2, &displayData.x2, sizeof(double));
    if (yAddr2) WriteGameMemory(yAddr2, &displayData.y2, sizeof(double));

    LogOut("Applied values from dialog: P1[HP:" + std::to_string(displayData.hp1) +
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