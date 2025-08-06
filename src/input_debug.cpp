#include "../include/input/input_debug.h"
#include "../include/input/input_core.h"
#include "../include/core/memory.h"
#include "../include/core/constants.h"
#include "../include/core/logger.h"
#include <sstream>
#include <iomanip>

// Logging helper for button presses
void LogButtonPress(const char* buttonName, uintptr_t address, uint8_t value, const char* result) {
    std::ostringstream oss;
    oss << "[DEBUG_INPUT] Pressed " << buttonName << " | Addr: 0x" << std::hex << address 
        << " | Value: 0x" << std::hex << (int)value << " | Result: " << result;
    LogOut(oss.str(), true);
}

// Add or update the DiagnoseInputSystem function with buffer inspection
void DiagnoseInputSystem(int playerNum) {
    uintptr_t playerPtr = GetPlayerPointer(playerNum);
    if (!playerPtr) {
        LogOut("[DEBUG_INPUT] Failed to get player pointer", true);
        return;
    }
    
    LogOut("[DEBUG_INPUT] Diagnosing input system for P" + std::to_string(playerNum), true);
    
    // Dump memory addresses
    std::ostringstream addrLog;
    addrLog << "[DEBUG_INPUT] P" << playerNum << " Input Register Addresses:\n";
    addrLog << "  Horizontal: 0x" << std::hex << (playerPtr + INPUT_HORIZONTAL_OFFSET) << "\n";
    addrLog << "  Vertical:   0x" << std::hex << (playerPtr + INPUT_VERTICAL_OFFSET) << "\n";
    addrLog << "  Button A:   0x" << std::hex << (playerPtr + INPUT_BUTTON_A_OFFSET) << "\n";
    addrLog << "  Button B:   0x" << std::hex << (playerPtr + INPUT_BUTTON_B_OFFSET) << "\n";
    addrLog << "  Button C:   0x" << std::hex << (playerPtr + INPUT_BUTTON_C_OFFSET) << "\n";
    addrLog << "  Button D:   0x" << std::hex << (playerPtr + INPUT_BUTTON_D_OFFSET) << "\n";
    addrLog << "  Buffer:     0x" << std::hex << (playerPtr + INPUT_BUFFER_OFFSET) << "\n";
    addrLog << "  Index:      0x" << std::hex << (playerPtr + INPUT_BUFFER_INDEX_OFFSET);
    LogOut(addrLog.str(), true);
    
    // Read current input register values
    uint8_t horzValue = 0, vertValue = 0, buttonA = 0, buttonB = 0, buttonC = 0, buttonD = 0;
    SafeReadMemory(playerPtr + INPUT_HORIZONTAL_OFFSET, &horzValue, sizeof(uint8_t));
    SafeReadMemory(playerPtr + INPUT_VERTICAL_OFFSET, &vertValue, sizeof(uint8_t));
    SafeReadMemory(playerPtr + INPUT_BUTTON_A_OFFSET, &buttonA, sizeof(uint8_t));
    SafeReadMemory(playerPtr + INPUT_BUTTON_B_OFFSET, &buttonB, sizeof(uint8_t));
    SafeReadMemory(playerPtr + INPUT_BUTTON_C_OFFSET, &buttonC, sizeof(uint8_t));
    SafeReadMemory(playerPtr + INPUT_BUTTON_D_OFFSET, &buttonD, sizeof(uint8_t));
    
    std::ostringstream valLog;
    valLog << "[DEBUG_INPUT] P" << playerNum << " Input Register Values:\n";
    valLog << "  Horizontal: " << (int)horzValue << " (" 
          << (horzValue == 0 ? "Neutral" : (horzValue == 1 ? "Right" : "Left")) << ")\n";
    valLog << "  Vertical:   " << (int)vertValue << " (" 
          << (vertValue == 0 ? "Neutral" : (vertValue == 1 ? "Down" : "Up")) << ")\n";
    valLog << "  Button A:   " << (int)buttonA << " (" << (buttonA ? "Pressed" : "Released") << ")\n";
    valLog << "  Button B:   " << (int)buttonB << " (" << (buttonB ? "Pressed" : "Released") << ")\n";
    valLog << "  Button C:   " << (int)buttonC << " (" << (buttonC ? "Pressed" : "Released") << ")\n";
    valLog << "  Button D:   " << (int)buttonD << " (" << (buttonD ? "Pressed" : "Released") << ")";
    LogOut(valLog.str(), true);
    
    // Dump the input buffer
    DumpInputBuffer(playerNum);
}

// Dump the input buffer for debugging
void DumpInputBuffer(int playerNum) {
    uintptr_t playerPtr = GetPlayerPointer(playerNum);
    if (!playerPtr) {
        LogOut("[DEBUG_INPUT] Failed to get player pointer for buffer dump", true);
        return;
    }
    
    // Read current buffer index
    uint16_t currentIndex = 0;
    if (!SafeReadMemory(playerPtr + INPUT_BUFFER_INDEX_OFFSET, &currentIndex, sizeof(uint16_t))) {
        LogOut("[DEBUG_INPUT] Failed to read buffer index", true);
        return;
    }
    
    LogOut("[DEBUG_INPUT] P" + std::to_string(playerNum) + 
           " Input Buffer Index: " + std::to_string(currentIndex), true);
    
    // Read and display a section of the buffer
    const int displayLength = 32; // How many entries to show
    std::ostringstream bufferLog;
    bufferLog << "[DEBUG_INPUT] P" << playerNum << " Input Buffer (last " << displayLength << " entries):\n";
    
    for (int i = 0; i < displayLength; i++) {
        // Calculate the index to read (go backward from current index)
        uint16_t readIdx = (currentIndex - displayLength + i + INPUT_BUFFER_SIZE) % INPUT_BUFFER_SIZE;
        uint8_t inputMask = 0;
        SafeReadMemory(playerPtr + INPUT_BUFFER_OFFSET + readIdx, &inputMask, sizeof(uint8_t));
        
        // Add formatting to highlight the current index
        if (i == displayLength - 1) {
            bufferLog << "[*] ";
        } else {
            bufferLog << "    ";
        }
        
        bufferLog << "Buffer[" << std::setw(3) << readIdx << "]: 0x" 
                 << std::hex << std::setw(2) << std::setfill('0') << (int)inputMask 
                 << " (" << DecodeInputMask(inputMask) << ")\n";
    }
    
    LogOut(bufferLog.str(), true);
}

// Visualize the input buffer for display
std::string GetInputBufferVisualization(int playerNum, int window) {
    uintptr_t playerPtr = GetPlayerPointer(playerNum);
    if (!playerPtr) {
        return "Error: Player pointer not found";
    }
    
    // Read current buffer index
    uint16_t currentIndex = 0;
    if (!SafeReadMemory(playerPtr + INPUT_BUFFER_INDEX_OFFSET, &currentIndex, sizeof(uint16_t))) {
        return "Error: Failed to read buffer index";
    }
    
    std::ostringstream visualization;
    visualization << "Buffer[" << currentIndex << "]: ";
    
    // Show recent buffer entries
    for (int i = 0; i < window; i++) {
        uint16_t readIdx = (currentIndex - window + i + INPUT_BUFFER_SIZE) % INPUT_BUFFER_SIZE;
        uint8_t inputMask = 0;
        SafeReadMemory(playerPtr + INPUT_BUFFER_OFFSET + readIdx, &inputMask, sizeof(uint8_t));
        
        // Format the output
        if (i > 0) visualization << " ";
        
        // Highlight the current position
        if (i == window - 1) {
            visualization << "[" << DecodeInputMask(inputMask) << "]";
        } else {
            visualization << DecodeInputMask(inputMask);
        }
    }
    
    return visualization.str();
}

// Log the next value that will be written to the buffer
void LogNextBufferValue(int playerNum) {
    uintptr_t playerPtr = GetPlayerPointer(playerNum);
    if (!playerPtr) {
        LogOut("[DEBUG_INPUT] Failed to get player pointer", true);
        return;
    }
    
    // Read current buffer index
    uint16_t currentIndex = 0;
    if (!SafeReadMemory(playerPtr + INPUT_BUFFER_INDEX_OFFSET, &currentIndex, sizeof(uint16_t))) {
        LogOut("[DEBUG_INPUT] Failed to read buffer index", true);
        return;
    }
    
    // Calculate next index
    uint16_t nextIndex = (currentIndex + 1) % INPUT_BUFFER_SIZE;
    
    LogOut("[DEBUG_INPUT] P" + std::to_string(playerNum) + 
           " Next Buffer Write: Index " + std::to_string(nextIndex), true);
}

// Spam attack buttons for debug menu (6 frames alternating)
bool HoldButtonA(int playerNum) {
    uintptr_t playerPtr = GetPlayerPointer(playerNum);
    if (!playerPtr) return false;
    
    // Button A is at offset 0x18A
    uint8_t value = 1;
    bool result = SafeWriteMemory(playerPtr + INPUT_BUTTON_A_OFFSET, &value, sizeof(uint8_t));
    LogButtonPress("A", playerPtr + INPUT_BUTTON_A_OFFSET, value, result ? "Success" : "Failed");
    return result;
}

bool HoldButtonB(int playerNum) {
    uintptr_t playerPtr = GetPlayerPointer(playerNum);
    if (!playerPtr) return false;
    
    // Button B is at offset 0x18B
    uint8_t value = 1;
    bool result = SafeWriteMemory(playerPtr + INPUT_BUTTON_B_OFFSET, &value, sizeof(uint8_t));
    LogButtonPress("B", playerPtr + INPUT_BUTTON_B_OFFSET, value, result ? "Success" : "Failed");
    return result;
}

bool HoldButtonC(int playerNum) {
    uintptr_t playerPtr = GetPlayerPointer(playerNum);
    if (!playerPtr) return false;
    
    // Button C is at offset 0x18C
    uint8_t value = 1;
    bool result = SafeWriteMemory(playerPtr + INPUT_BUTTON_C_OFFSET, &value, sizeof(uint8_t));
    LogButtonPress("C", playerPtr + INPUT_BUTTON_C_OFFSET, value, result ? "Success" : "Failed");
    return result;
}

bool HoldButtonD(int playerNum) {
    uintptr_t playerPtr = GetPlayerPointer(playerNum);
    if (!playerPtr) return false;
    
    // Button D is at offset 0x18D
    uint8_t value = 1;
    bool result = SafeWriteMemory(playerPtr + INPUT_BUTTON_D_OFFSET, &value, sizeof(uint8_t));
    LogButtonPress("D", playerPtr + INPUT_BUTTON_D_OFFSET, value, result ? "Success" : "Failed");
    return result;
}