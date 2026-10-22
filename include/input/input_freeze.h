#pragma once

// System includes
#include <windows.h>
#include <vector>
#include <atomic>
#include <thread>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <cstdint>
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

// Core buffer freezing functions
uint64_t StartBufferFreezeWorker(int playerNum);
bool FreezeBufferIndex(int playerNum, uint16_t indexValue);
void StopBufferFreezing();

// Special move patterns
bool FreezePerfectDragonPunch(int playerNum);

// Freeze the input buffer with a pattern for a specific motion (motionType
// from motion_types.h; buttonMask = A/B/C/... bits). optimalIndex overrides
// the buffer index position. Returns true if the freeze started.
bool FreezeBufferForMotion(int playerNum, int motionType, int buttonMask, int optimalIndex = -1);

// Exclusive tutorial lease for the global buffer-freeze engine. Acquisition
// fails rather than stopping an existing owner's freeze. Legacy starts are
// rejected while held; stop/release affect only the matching tutorial token.
bool AcquireTutorialBufferFreeze(uint64_t& tokenOut);
bool TutorialBufferFreezeLeaseActive();
bool FreezeBufferForTutorial(uint64_t token, int playerNum, int motionType,
                             int buttonMask, int optimalIndex = -1);
void StopTutorialBufferFreeze(uint64_t token);
void ReleaseTutorialBufferFreeze(uint64_t token);

// Generic pattern freeze: writes an arbitrary already-encoded GAME_INPUT_*
// bitmask pattern starting at buffer index 0 (same policy as
// FreezeBufferForMotion); the index freezes at the last element so the
// terminating button press stays resident for recognition.
bool FreezeBufferWithPattern(int playerNum, const std::vector<uint8_t>& pattern);
// Variant advancing the frozen index past the final written element by
// extraNeutralFrames (implicit neutral bytes) for recognition logic that
// expects further index progress.
bool FreezeBufferWithPattern(int playerNum, const std::vector<uint8_t>& pattern, int extraNeutralFrames);

// Buffer visualization function
std::string GetInputBufferVisualization(int playerNum, int window);

// Session lifecycle helpers
void BeginBufferFreezeSession(int playerNum, std::string_view label);
void EndBufferFreezeSession(int playerNum, const char* reason, bool clearGlobals = true);

// Utility: safely clear (neutralize) a player's input buffer (+ index) within bounds
void ClearPlayerInputBuffer(int playerNum);
