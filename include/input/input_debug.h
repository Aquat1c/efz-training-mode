#pragma once
#include <windows.h>
#include <string>
#include "input_core.h"
#include "input_buffer.h"

// Debug functions
void DiagnoseInputSystem(int playerNum);
void DumpInputBuffer(int playerNum);
std::string GetInputBufferVisualization(int playerNum, int window); // Removed default parameter
void LogNextBufferValue(int playerNum);
void LogButtonPress(const char* buttonName, uintptr_t address, uint8_t value, const char* result);

// Button input functions
bool HoldButtonA(int playerNum);
bool HoldButtonB(int playerNum);
bool HoldButtonC(int playerNum);
bool HoldButtonD(int playerNum);