#pragma once
#include <windows.h>
#include <vector>
#include <atomic>
#include <thread>
#include "input_core.h"

// Input buffer constants
extern const uint16_t INPUT_BUFFER_SIZE;
extern const uintptr_t INPUT_BUFFER_OFFSET;
extern const uintptr_t INPUT_BUFFER_INDEX_OFFSET;

// Buffer freezing globals
extern std::atomic<bool> g_bufferFreezingActive;
extern std::atomic<bool> g_indexFreezingActive;
extern std::thread g_bufferFreezeThread;
extern std::vector<uint8_t> g_frozenBufferValues;
extern uint16_t g_frozenBufferStartIndex;
extern uint16_t g_frozenBufferLength;
extern uint16_t g_frozenIndexValue;
// Which player currently owns an active buffer-freeze session (0 = none)
extern std::atomic<int> g_activeFreezePlayer;

// Buffer freezing functions
//bool FreezeBufferForMotion(int playerNum, int motionType, int buttonMask, int optimalIndex);
void FreezeBufferValuesThread(int playerNum);
bool CaptureAndFreezeBuffer(int playerNum, uint16_t startIndex, uint16_t length);
bool FreezeBufferIndex(int playerNum, uint16_t indexValue);
void StopBufferFreezing();