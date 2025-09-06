#pragma once

// System includes
#include <windows.h>
#include <vector>
#include <atomic>
#include <thread>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <string_view>

// Project includes
#include "../include/input/input_motion.h"
#include "../include/core/memory.h"
#include "../include/core/logger.h"
#include "../include/core/constants.h"
#include "../include/utils/utilities.h"

// Global buffer freezing variables
extern std::atomic<bool> g_bufferFreezingActive;
extern std::atomic<bool> g_indexFreezingActive;
extern std::thread g_bufferFreezeThread;
extern std::vector<uint8_t> g_frozenBufferValues;
extern uint16_t g_frozenBufferStartIndex;
extern uint16_t g_frozenBufferLength;
extern uint16_t g_frozenIndexValue;

// External constants referenced in implementation
extern const uint16_t INPUT_BUFFER_SIZE;
extern const uintptr_t INPUT_BUFFER_OFFSET;
extern const uintptr_t INPUT_BUFFER_INDEX_OFFSET;

// Helper to avoid narrowing conversion warnings
inline uint8_t u8(int value) {
    return static_cast<uint8_t>(value);
}

// Core buffer freezing functions
void FreezeBufferValuesThread(int playerNum);
bool CaptureAndFreezeBuffer(int playerNum, uint16_t startIndex, uint16_t length);
bool FreezeBufferIndex(int playerNum, uint16_t indexValue);
void StopBufferFreezing();

// Special move patterns
bool FreezePerfectDragonPunch(int playerNum);
// Developer-only motion freeze helpers removed from public API

/**
 * Freezes the input buffer with a pattern for a specific motion input.
 * 
 * @param playerNum The player number (1 or 2)
 * @param motionType The type of motion to execute (from motion_types.h constants)
 * @param buttonMask The button(s) to press with the motion (A, B, C, etc.)
 * @param optimalIndex Optional parameter to specify a custom buffer index position
 * @return True if the buffer freezing was successfully started, false otherwise
 */
bool FreezeBufferForMotion(int playerNum, int motionType, int buttonMask, int optimalIndex = -1);

// Generic pattern freeze (used for complex Final Memory inputs that don't map to a single motionType)
// Writes an arbitrary already-direction/button encoded pattern (values are the unified GAME_INPUT_* bitmasks)
// Pattern is placed starting at buffer index 0 (same policy as FreezeBufferForMotion) and the index is
// frozen at the last element so the terminating button press remains resident for recognition.
// Returns true if successfully started.
bool FreezeBufferWithPattern(int playerNum, const std::vector<uint8_t>& pattern);
// Variant that allows advancing the frozen index past the final written element by extraNeutralFrames
// (used for patterns where recognition logic expects the index to have progressed further than the
// last non-neutral input; extraNeutralFrames worth of neutral (0) bytes are implicitly assumed.)
bool FreezeBufferWithPattern(int playerNum, const std::vector<uint8_t>& pattern, int extraNeutralFrames);

// Buffer visualization function
std::string GetInputBufferVisualization(int playerNum, int window);

// Session lifecycle helpers
void BeginBufferFreezeSession(int playerNum, std::string_view label);
void EndBufferFreezeSession(int playerNum, const char* reason, bool clearGlobals = true);

// Utility: safely clear (neutralize) a player's input buffer (+ index) within bounds
void ClearPlayerInputBuffer(int playerNum);
