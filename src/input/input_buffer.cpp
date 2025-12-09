#include "../include/input/input_buffer.h"
#include "../include/input/input_core.h"
#include "../include/core/constants.h"
#include "../include/core/memory.h"
#include "../include/core/logger.h"
#include "../include/utils/utilities.h"
#include "../include/input/motion_constants.h"
#include "../include/core/globals.h"
#include "../include/input/shared_constants.h"
#include "../include/input/input_debug.h"
#include "../include/input/motion_system.h"
#include "../include/input/input_motion.h"  
#include "../include/game/game_state.h"  
#include "../include/input/input_freeze.h"
#include <vector>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cstdint>

// Define the buffer constants here
// IMPORTANT: The actual input buffer is 180 bytes long. Using 0x180 (384)
// would overwrite into other fields (including the index at 0x260), causing anomalies.
const uint16_t INPUT_BUFFER_SIZE = 180;    // 180 bytes circular buffer (0xB4)
const uintptr_t INPUT_BUFFER_OFFSET = 0x1AB;  // Buffer start offset in player struct
const uintptr_t INPUT_BUFFER_INDEX_OFFSET = 0x260;  // Current buffer index offset

// Define the freeze buffer variables here
std::atomic<bool> g_bufferFreezingActive(false);
std::atomic<bool> g_indexFreezingActive(false);
std::atomic<bool> g_freezeThreadInitialized(false);  // Thread startup confirmation
std::thread g_bufferFreezeThread;
std::vector<uint8_t> g_frozenBufferValues;
uint16_t g_frozenBufferStartIndex = 0;
uint16_t g_frozenBufferLength = 0;
uint16_t g_frozenIndexValue = 0;
std::atomic<int> g_activeFreezePlayer{0};

// Store motion info for dynamic pattern regeneration on facing changes
int g_frozenMotionType = -1;
int g_frozenButtonMask = 0;
bool g_lastKnownFacing = false;

// Helper function to regenerate motion pattern with new facing direction
static bool RegeneratePatternForFacing(int playerNum, int motionType, int buttonMask, bool facingRight) {
    // Define directions based on facing
    uint8_t fwd = facingRight ? GAME_INPUT_RIGHT : GAME_INPUT_LEFT;
    uint8_t back = facingRight ? GAME_INPUT_LEFT : GAME_INPUT_RIGHT;
    uint8_t down = GAME_INPUT_DOWN;
    uint8_t downFwd = down | fwd;
    uint8_t downBack = down | back;
    
    std::vector<uint8_t> pattern;
    
    // Generate pattern based on motion type (matching FreezeBufferForMotion logic)
    switch (motionType) {
        case MOTION_623A: case MOTION_623B: case MOTION_623C: {
            // Dragon Punch (623): Forward, Down, Down-Forward + Button
            pattern = {
                0x00, 0x00,
                fwd, fwd, fwd,
                down, down,
                downFwd, downFwd,
                (uint8_t)(downFwd | buttonMask),
                (uint8_t)(downFwd | buttonMask),
            };
            break;
        }
        case MOTION_214A: case MOTION_214B: case MOTION_214C: {
            // QCB: Down, Down-Back, Back + Button
            pattern = {
                0x00, 0x00,
                down, down, down,
                downBack, downBack,
                back, back,
                (uint8_t)(back | buttonMask),
                (uint8_t)(back | buttonMask),
            };
            break;
        }
        case MOTION_236A: case MOTION_236B: case MOTION_236C: {
            // QCF: Down, Down-Forward, Forward + Button
            pattern = {
                0x00, 0x00,
                down, down, down,
                downFwd, downFwd,
                fwd, fwd,
                (uint8_t)(fwd | buttonMask),
                (uint8_t)(fwd | buttonMask),
            };
            break;
        }
        case MOTION_421A: case MOTION_421B: case MOTION_421C: {
            // 421: Back, Down, Down-Back + Button
            pattern = {
                0x00, 0x00,
                back, back,
                down, down,
                downBack, downBack,
                (uint8_t)(downBack | buttonMask),
                (uint8_t)(downBack | buttonMask),
            };
            break;
        }
        case MOTION_41236A: case MOTION_41236B: case MOTION_41236C: {
            // HCF: Back, Down-Back, Down, Down-Forward, Forward + Button
            pattern = {
                0x00, 0x00,
                back, back,
                downBack, downBack,
                down, down,
                downFwd, downFwd,
                fwd,
                (uint8_t)(fwd | buttonMask),
                (uint8_t)(fwd | buttonMask),
            };
            break;
        }
        case MOTION_214214A: case MOTION_214214B: case MOTION_214214C: {
            // Double QCB: (Down, Down-Back, Back)x2 + Button
            pattern = {
                0x00, 0x00,
                down, down, down,
                downBack, downBack,
                back, back,
                down, down, down,
                downBack, downBack,
                back, back,
                (uint8_t)(back | buttonMask),
                (uint8_t)(back | buttonMask),
            };
            break;
        }
        case MOTION_236236A: case MOTION_236236B: case MOTION_236236C: {
            // Double QCF: (Down, Down-Forward, Forward)x2 + Button
            pattern = {
                0x00, 0x00,
                down, down, down,
                downFwd, downFwd,
                fwd, fwd,
                down, down, down,
                downFwd, downFwd,
                fwd, fwd,
                (uint8_t)(fwd | buttonMask),
                (uint8_t)(fwd | buttonMask),
            };
            break;
        }
        case MOTION_641236A: case MOTION_641236B: case MOTION_641236C: {
            // 641236: Forward, Back, Down-Back, Down, Down-Forward, Forward + Button
            pattern = {
                0x00, 0x00,
                fwd, fwd,
                back, back,
                downBack, downBack,
                down, down,
                downFwd, downFwd,
                fwd,
                (uint8_t)(fwd | buttonMask),
                (uint8_t)(fwd | buttonMask),
            };
            break;
        }
        case MOTION_412A: case MOTION_412B: case MOTION_412C: {
            // 412: Back, Down-Back, Down + Button
            pattern = {
                0x00, 0x00,
                back, back,
                downBack, downBack,
                down,
                (uint8_t)(down | buttonMask),
                (uint8_t)(down | buttonMask),
            };
            break;
        }
        case MOTION_22A: case MOTION_22B: case MOTION_22C: {
            // 22: Down, (small neutral), Down + Button
            pattern = {
                0x00, 0x00,
                down, down,
                0x00, 0x00,
                down,
                (uint8_t)(down | buttonMask),
                (uint8_t)(down | buttonMask),
            };
            break;
        }
        case MOTION_214236A: case MOTION_214236B: case MOTION_214236C: {
            // 214236: Down, Down-Back, Back, Down, Down-Forward, Forward + Button
            pattern = {
                0x00, 0x00,
                down, down,
                downBack, downBack,
                back, back,
                down, down,
                downFwd, downFwd,
                fwd,
                (uint8_t)(fwd | buttonMask),
                (uint8_t)(fwd | buttonMask),
            };
            break;
        }
        case MOTION_463214A: case MOTION_463214B: case MOTION_463214C: {
            // 463214: Back, Forward, Down-Forward, Down, Down-Back, Back + Button
            pattern = {
                0x00, 0x00,
                back, back,
                fwd, fwd,
                downFwd, downFwd,
                down, down,
                downBack, downBack,
                back,
                (uint8_t)(back | buttonMask),
                (uint8_t)(back | buttonMask),
            };
            break;
        }
        case MOTION_4123641236A: case MOTION_4123641236B: case MOTION_4123641236C: {
            // 41236 41236: (Back, Down-Back, Down, Down-Forward, Forward)x2 + Button
            pattern = {
                0x00, 0x00,
                back, back, downBack, downBack, down, down, downFwd, downFwd, fwd, fwd,
                back, back, downBack, downBack, down, down, downFwd, downFwd, fwd,
                (uint8_t)(fwd | buttonMask),
                (uint8_t)(fwd | buttonMask),
            };
            break;
        }
        case MOTION_6321463214A: case MOTION_6321463214B: case MOTION_6321463214C: {
            // 6321463214: (Forward, Down-Forward, Down, Down-Back, Back)x2 + Button
            pattern = {
                0x00, 0x00,
                fwd, fwd,
                downFwd, downFwd,
                down, down,
                downBack, downBack,
                back, back,
                fwd, fwd,
                downFwd, downFwd,
                down, down,
                downBack, downBack,
                back,
                (uint8_t)(back | buttonMask),
                (uint8_t)(back | buttonMask),
            };
            break;
        }
        default:
            LogOut("[BUFFER_FREEZE][CROSSUP] Cannot regenerate pattern for motion type " + std::to_string(motionType), true);
            return false;
    }
    
    if (pattern.empty()) {
        return false;
    }
    
    // Update frozen buffer values
    g_frozenBufferValues = pattern;
    g_frozenBufferLength = static_cast<uint16_t>(pattern.size());
    
    // Write the new pattern to the buffer immediately
    uintptr_t playerPtr = GetPlayerPointer(playerNum);
    if (playerPtr) {
        SafeWriteMemory(playerPtr + INPUT_BUFFER_OFFSET + g_frozenBufferStartIndex, 
                       pattern.data(), static_cast<uint32_t>(pattern.size()));
        if (detailedLogging.load()) {
            LogOut("[BUFFER_FREEZE][CROSSUP] Pattern regenerated for new facing direction", true);
        }
        return true;
    }
    return false;
}

// Define buffer functions
void FreezeBufferValuesThread(int playerNum) {
    if (detailedLogging.load()) {
        std::stringstream ss;
        ss << "[INPUT_BUFFER] Starting buffer freeze thread for P" << playerNum
           << " startIdx=" << g_frozenBufferStartIndex
           << " len=" << g_frozenBufferLength
           << " idxLock=" << (g_indexFreezingActive.load() ? std::to_string(g_frozenIndexValue) : std::string("off"))
           << " motionType=" << g_frozenMotionType
           << " btnMask=0x" << std::hex << g_frozenButtonMask << std::dec
           << " facing=" << (g_lastKnownFacing ? "right" : "left");
        LogOut(ss.str(), true);
    }
    g_activeFreezePlayer.store(playerNum);
    
    uintptr_t initialPlayerPtr = GetPlayerPointer(playerNum);
    if (!initialPlayerPtr) {
        LogOut("[INPUT_BUFFER] Invalid player pointer at thread start, aborting", true);
        g_bufferFreezingActive = false;
        return;
    }
    
    // One-time sanity check: ensure buffer does not overlap index
    static std::atomic<bool> s_layoutChecked{false};
    if (!s_layoutChecked.load()) {
        uintptr_t testPlayer = initialPlayerPtr;
        if (testPlayer) {
            uintptr_t bufStart = testPlayer + INPUT_BUFFER_OFFSET;
            uintptr_t bufEnd   = bufStart + INPUT_BUFFER_SIZE - 1;
            uintptr_t idxAddr  = testPlayer + INPUT_BUFFER_INDEX_OFFSET;
            if (detailedLogging.load()) {
                std::stringstream ss;
                ss << "[INPUT_BUFFER] Layout: start=0x" << std::hex << bufStart
                   << " end=0x" << bufEnd << " index=0x" << idxAddr
                   << std::dec;
                LogOut(ss.str(), true);
            }
            if (bufEnd >= idxAddr && idxAddr >= bufStart) {
                LogOut("[INPUT_BUFFER][WARN] Buffer region overlaps index! Adjust sizes/offsets.", true);
            }
        }
        s_layoutChecked.store(true);
    }

    // Get the move ID address for monitoring move execution
    uintptr_t base = GetEFZBase();
    uintptr_t moveIDAddr = ResolvePointer(base, 
        (playerNum == 1) ? EFZ_BASE_OFFSET_P1 : EFZ_BASE_OFFSET_P2, 
        MOVE_ID_OFFSET);
    
    // Also monitor motion token to detect successful motion queuing
    uintptr_t motionTokenAddr = initialPlayerPtr + MOTION_TOKEN_OFFSET;
    
    short lastMoveID = -1;
    uint8_t lastMotionToken = 0;
    bool moveIDChanged = false;
    bool motionTokenChanged = false;
    int consecutiveMoveTicks = 0;
    const int freezeLimit = 300; // Hard limit on freeze frames
    
    // Get initial move ID and motion token for comparison
    SafeReadMemory(moveIDAddr, &lastMoveID, sizeof(short));
    SafeReadMemory(motionTokenAddr, &lastMotionToken, sizeof(uint8_t));
    
    // Signal that thread has fully initialized
    g_freezeThreadInitialized.store(true);
    
    // AGGRESSIVE PHASE: Write very frequently at the start
    const int aggressivePhaseFrames = 15;
    for (int i = 0; i < aggressivePhaseFrames && g_bufferFreezingActive; i++) {
        if (g_onlineModeActive.load()) { g_bufferFreezingActive = false; break; }
        // Check player pointer validity
        uintptr_t playerPtr = GetPlayerPointer(playerNum);
        if (!playerPtr || playerPtr != initialPlayerPtr) {
            LogOut("[INPUT_BUFFER] Player pointer changed during aggressive phase, aborting", true);
            g_bufferFreezingActive = false;
            break;
        }
        
        // Check for facing direction changes (cross-up detection)
        if (g_frozenMotionType >= 0) {
            bool currentFacing = GetPlayerFacingDirection(playerNum);
            if (currentFacing != g_lastKnownFacing) {
                if (detailedLogging.load()) {
                    LogOut("[BUFFER_FREEZE][CROSSUP] Facing direction changed: " +
                          std::string(g_lastKnownFacing ? "right" : "left") + " → " +
                          std::string(currentFacing ? "right" : "left") + " during freeze!", true);
                }
                g_lastKnownFacing = currentFacing;
                // Regenerate pattern with new facing direction
                RegeneratePatternForFacing(playerNum, g_frozenMotionType, g_frozenButtonMask, currentFacing);
            }
        }
        
        // Write buffer values very frequently (every iteration)
        // Optimize: two contiguous writes instead of per-byte writes across ring
        if (g_frozenBufferLength > 0) {
            uint16_t start = g_frozenBufferStartIndex;
            uint16_t len1 = (uint16_t)std::min<uint16_t>(g_frozenBufferLength, INPUT_BUFFER_SIZE - start);
            uint16_t len2 = g_frozenBufferLength - len1;
            // First contiguous segment
            if (len1 > 0) {
                SafeWriteMemory(playerPtr + INPUT_BUFFER_OFFSET + start,
                                g_frozenBufferValues.data(), len1 * sizeof(uint8_t));
            }
            // Wrapped segment at buffer start
            if (len2 > 0) {
                SafeWriteMemory(playerPtr + INPUT_BUFFER_OFFSET,
                                g_frozenBufferValues.data() + len1, len2 * sizeof(uint8_t));
            }
        }
        
        // Ensure index stays frozen
        if (g_indexFreezingActive) {
            // Avoid redundant writes if index is already at desired value
            uint16_t curIdx = 0xFFFF;
            if (!SafeReadMemory(playerPtr + INPUT_BUFFER_INDEX_OFFSET, &curIdx, sizeof(uint16_t)) || curIdx != g_frozenIndexValue) {
                SafeWriteMemory(playerPtr + INPUT_BUFFER_INDEX_OFFSET,
                                &g_frozenIndexValue, sizeof(uint16_t));
            }
        }
        
        // Check for move ID changes
        short currentMoveID = 0;
        if (SafeReadMemory(moveIDAddr, &currentMoveID, sizeof(short))) {
            if (currentMoveID != lastMoveID) {
                if (detailedLogging.load()) {
                    LogOut("[BUFFER_FREEZE] MoveID changed: " + std::to_string(lastMoveID) + 
                          " → " + std::to_string(currentMoveID), true);
                }
                lastMoveID = currentMoveID;
                moveIDChanged = true;
            }
        }
        
        // Check for motion token changes (indicates motion was queued)
        uint8_t currentMotionToken = 0;
        if (SafeReadMemory(motionTokenAddr, &currentMotionToken, sizeof(uint8_t))) {
            if (currentMotionToken != lastMotionToken && currentMotionToken != 0 && currentMotionToken != 0x63) {
                if (detailedLogging.load()) {
                    std::stringstream ss;
                    ss << "[BUFFER_FREEZE] Motion token changed: 0x" 
                       << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(lastMotionToken)
                       << " → 0x" << std::setw(2) << std::setfill('0') << static_cast<int>(currentMotionToken)
                       << " (motion queued, waiting for execution)";
                    LogOut(ss.str(), true);
                }
                motionTokenChanged = true;
                // Don't break yet - wait for moveID to change confirming execution
            }
            lastMotionToken = currentMotionToken;
        }
        
        // Very short sleep for aggressive phase
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    
    // NORMAL PHASE: Continue with standard frequency
    int freezeCount = aggressivePhaseFrames;
    while (g_bufferFreezingActive && freezeCount < freezeLimit && !g_isShuttingDown.load()) {
        if (g_onlineModeActive.load()) { LogOut("[INPUT_BUFFER] Online mode active, stopping buffer freeze", true); break; }
        // Check game state and player pointer validity
        GamePhase currentPhase = GetCurrentGamePhase();
        if (currentPhase != GamePhase::Match) {
            LogOut("[INPUT_BUFFER] Game phase changed, stopping buffer freeze", true);
            break;
        }
        
        uintptr_t currentPlayerPtr = GetPlayerPointer(playerNum);
        if (!currentPlayerPtr || currentPlayerPtr != initialPlayerPtr) {
            LogOut("[INPUT_BUFFER] Player pointer changed, stopping buffer freeze", true);
            break;
        }
        
        // Check for facing direction changes (cross-up detection)
        if (g_frozenMotionType >= 0 && freezeCount % 3 == 0) {
            bool currentFacing = GetPlayerFacingDirection(playerNum);
            if (currentFacing != g_lastKnownFacing) {
                if (detailedLogging.load()) {
                    LogOut("[BUFFER_FREEZE][CROSSUP] Facing direction changed: " +
                          std::string(g_lastKnownFacing ? "right" : "left") + " → " +
                          std::string(currentFacing ? "right" : "left") + " during freeze!", true);
                }
                g_lastKnownFacing = currentFacing;
                // Regenerate pattern with new facing direction
                RegeneratePatternForFacing(playerNum, g_frozenMotionType, g_frozenButtonMask, currentFacing);
            }
        }
        
        // Rewrite buffer values every 3rd frame
        if (freezeCount % 3 == 0) {
            if (g_frozenBufferLength > 0) {
                uint16_t start = g_frozenBufferStartIndex;
                uint16_t len1 = (uint16_t)std::min<uint16_t>(g_frozenBufferLength, INPUT_BUFFER_SIZE - start);
                uint16_t len2 = g_frozenBufferLength - len1;
                if (len1 > 0) {
                    SafeWriteMemory(currentPlayerPtr + INPUT_BUFFER_OFFSET + start,
                                    g_frozenBufferValues.data(), len1 * sizeof(uint8_t));
                }
                if (len2 > 0) {
                    SafeWriteMemory(currentPlayerPtr + INPUT_BUFFER_OFFSET,
                                    g_frozenBufferValues.data() + len1, len2 * sizeof(uint8_t));
                }
            }
        }
        
        // Always keep index frozen
        if (g_indexFreezingActive) {
            uint16_t curIdx = 0xFFFF;
            if (!SafeReadMemory(currentPlayerPtr + INPUT_BUFFER_INDEX_OFFSET, &curIdx, sizeof(uint16_t)) || curIdx != g_frozenIndexValue) {
                SafeWriteMemory(currentPlayerPtr + INPUT_BUFFER_INDEX_OFFSET,
                                &g_frozenIndexValue, sizeof(uint16_t));
            }
        }
        
        // Check for move ID changes
        short currentMoveID = 0;
        if (SafeReadMemory(moveIDAddr, &currentMoveID, sizeof(short))) {
            if (currentMoveID != lastMoveID) {
                if (detailedLogging.load()) {
                    LogOut("[BUFFER_FREEZE] MoveID changed: " + std::to_string(lastMoveID) + 
                          " → " + std::to_string(currentMoveID), true);
                }
                lastMoveID = currentMoveID;
                moveIDChanged = true;
            }
            
            // Detect successful special move execution (non-zero moveID for multiple ticks)
            if (currentMoveID != 0 && moveIDChanged) {
                consecutiveMoveTicks++;
                if (consecutiveMoveTicks >= 3) {
                    if (detailedLogging.load()) {
                        LogOut("[BUFFER_FREEZE] Motion recognized! Move ID: " + 
                              std::to_string(currentMoveID), true);
                    }
                    break; // Exit early on successful execution
                }
            } else {
                consecutiveMoveTicks = 0;
            }
        }
        
        // Check for motion token changes (indicates motion was queued)
        uint8_t currentMotionToken = 0;
        if (SafeReadMemory(motionTokenAddr, &currentMotionToken, sizeof(uint8_t))) {
            if (currentMotionToken != lastMotionToken && currentMotionToken != 0 && currentMotionToken != 0x63) {
                if (detailedLogging.load()) {
                    std::stringstream ss;
                    ss << "[BUFFER_FREEZE] Motion token changed: 0x" 
                       << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(lastMotionToken)
                       << " → 0x" << std::setw(2) << std::setfill('0') << static_cast<int>(currentMotionToken)
                       << " (motion queued, waiting for execution)";
                    LogOut(ss.str(), true);
                }
                motionTokenChanged = true;
                // Don't break yet - wait for moveID to change confirming execution
            }
            lastMotionToken = currentMotionToken;
        }
        
        freezeCount++;
        // Standard sleep time
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }
    
    // Cleanup phase - DO NOT forcibly wipe entire buffer here; just neutralize trailing inputs
    if (detailedLogging.load()) {
        LogOut("[BUFFER_FREEZE] Buffer freeze thread ended (counter=" + std::to_string(freezeCount) + ")", true);
    }
    
    // Reset initialization flag so next freeze will wait for thread startup
    g_freezeThreadInitialized.store(false);
    
    if (g_frozenBufferLength > 0) {
        uintptr_t playerPtr = GetPlayerPointer(playerNum);
        if (playerPtr) {
            // Overwrite pattern region with neutral to avoid ghost follow‑ups
            if (g_frozenBufferLength > 0) {
                uint16_t start = g_frozenBufferStartIndex;
                uint16_t len1 = (uint16_t)std::min<uint16_t>(g_frozenBufferLength, INPUT_BUFFER_SIZE - start);
                uint16_t len2 = g_frozenBufferLength - len1;
                // Prepare temporary zero buffers (stack-allocated, small)
                std::vector<uint8_t> zeros1(len1, 0x00);
                std::vector<uint8_t> zeros2(len2, 0x00);
                if (len1 > 0) {
                    SafeWriteMemory(playerPtr + INPUT_BUFFER_OFFSET + start, zeros1.data(), len1 * sizeof(uint8_t));
                }
                if (len2 > 0) {
                    SafeWriteMemory(playerPtr + INPUT_BUFFER_OFFSET, zeros2.data(), len2 * sizeof(uint8_t));
                }
            }
            // Push a few neutral frames at index tail
            uint16_t curIdx = 0;
            SafeReadMemory(playerPtr + INPUT_BUFFER_INDEX_OFFSET, &curIdx, sizeof(uint16_t));
            for (int n=0; n<4; ++n) {
                uint16_t w = (curIdx + INPUT_BUFFER_SIZE - n) % INPUT_BUFFER_SIZE;
                uint8_t z = 0x00;
                SafeWriteMemory(playerPtr + INPUT_BUFFER_OFFSET + w, &z, sizeof(uint8_t));
            }
        }
    }
    
    g_bufferFreezingActive = false;
    g_indexFreezingActive = false;
    g_activeFreezePlayer.store(0);
    
    if (detailedLogging.load()) {
        LogOut("[BUFFER_FREEZE] End session P" + std::to_string(playerNum) + " (thread ended)", true);
    }
}

// Capture current buffer section and begin freezing it
bool CaptureAndFreezeBuffer(int playerNum, uint16_t startIndex, uint16_t length, int motionType, int buttonMask) {
    // Stop any existing freeze thread
    StopBufferFreezing();
    
    // Store motion info for potential pattern regeneration
    g_frozenMotionType = motionType;
    g_frozenButtonMask = buttonMask;
    g_lastKnownFacing = GetPlayerFacingDirection(playerNum);
    
    uintptr_t playerPtr = GetPlayerPointer(playerNum);
    if (!playerPtr) {
        LogOut("[INPUT_BUFFER] Cannot get player pointer", true);
        return false;
    }
    
    // Validate parameters
    if (length > INPUT_BUFFER_SIZE) {
        LogOut("[INPUT_BUFFER] Length too large, capping at " + std::to_string(INPUT_BUFFER_SIZE), true);
        length = INPUT_BUFFER_SIZE;
    }
    
    // Read current buffer index for reference
    uint16_t currentIndex = 0;
    if (!SafeReadMemory(playerPtr + INPUT_BUFFER_INDEX_OFFSET, &currentIndex, sizeof(uint16_t))) {
        LogOut("[INPUT_BUFFER] Failed to read buffer index", true);
        return false;
    }
    
    // Read buffer values
    g_frozenBufferValues.resize(length);
    g_frozenBufferStartIndex = startIndex;
    g_frozenBufferLength = length;
    
    bool readSuccess = true;
    for (size_t i = 0; i < static_cast<size_t>(length); i++) {
        uint16_t readIdx = (startIndex + i) % INPUT_BUFFER_SIZE;
        if (!SafeReadMemory(playerPtr + INPUT_BUFFER_OFFSET + readIdx, &g_frozenBufferValues[i], sizeof(uint8_t))) {
            readSuccess = false;
            LogOut("[INPUT_BUFFER] Failed to read buffer value at " + std::to_string(readIdx), true);
        }
    }
    
    if (!readSuccess) {
        LogOut("[INPUT_BUFFER] Failed to read some buffer values, freezing may be inconsistent", true);
    }
    
    // Output the captured values
    if (detailedLogging.load()) {
        std::stringstream ss;
        ss << "[INPUT_BUFFER] Captured values: ";
        for (size_t i = 0; i < static_cast<size_t>(length); i++) {
            ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(g_frozenBufferValues[i]);
            if (i + 1 < static_cast<size_t>(length)) ss << " ";
        }
        LogOut(ss.str(), true);
    }
    
    // Start freezing
    g_freezeThreadInitialized.store(false);  // Reset before creating thread
    g_bufferFreezingActive = true;
    g_activeFreezePlayer.store(playerNum);
    g_bufferFreezeThread = std::thread(FreezeBufferValuesThread, playerNum);
    g_bufferFreezeThread.detach();  // Detach to prevent termination
    
    // CRITICAL: Wait for thread to fully initialize before returning.
    // This prevents race condition where auto_action's ProcessAutoControlRestore
    // checks g_bufferFreezingActive before the thread has even started its loop.
    // Max wait 10ms (should take <1ms normally).
    auto startWait = std::chrono::steady_clock::now();
    int waitIterations = 0;
    while (!g_freezeThreadInitialized.load()) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - startWait).count();
        if (elapsed > 10) {
            LogOut("[INPUT_BUFFER][WARN] Thread startup timeout after " + std::to_string(elapsed) + 
                  "ms (waited " + std::to_string(waitIterations) + " iterations), active=" + 
                  std::to_string(g_bufferFreezingActive.load()), true);
            break;
        }
        waitIterations++;
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    
    if (detailedLogging.load() && waitIterations > 0) {
        LogOut("[INPUT_BUFFER] Thread initialization confirmed after " + std::to_string(waitIterations) + 
              " wait iterations", true);
    }
    
    LogOut("[INPUT_BUFFER] Buffer freezing activated for P" + std::to_string(playerNum) + 
           " starting at index " + std::to_string(startIndex) +
           " with length " + std::to_string(length) +
           ", owner=P" + std::to_string(g_activeFreezePlayer.load()), true);
    return true;
}

// Option to also freeze buffer index
bool FreezeBufferIndex(int playerNum, uint16_t indexValue) {
    uintptr_t playerPtr = GetPlayerPointer(playerNum);
    if (!playerPtr) {
        LogOut("[INPUT_BUFFER] Cannot get player pointer", true);
        return false;
    }
    
    g_frozenIndexValue = indexValue;
    g_indexFreezingActive = true;
    
    LogOut("[INPUT_BUFFER] Buffer index freezing activated, locked to " + std::to_string(indexValue), true);
    return true;
}

// Function to stop buffer freezing
void StopBufferFreezing() {
    if (g_bufferFreezingActive) {
        using clock = std::chrono::steady_clock;
        auto tStart = clock::now();
        g_bufferFreezingActive = false;
        g_indexFreezingActive = false;
        int owner = g_activeFreezePlayer.exchange(0);
        if (owner != 0) {
            LogOut("[INPUT_BUFFER] StopBufferFreezing() called (owner=P" + std::to_string(owner) + ")", true);
        } else {
            LogOut("[INPUT_BUFFER] StopBufferFreezing() called (no active owner)", true);
        }
        
    // Wait briefly for thread to wind down (keep minimal to avoid frame hitch)
    auto tWaitStart = clock::now();
    std::this_thread::sleep_for(std::chrono::milliseconds(8));
    auto tWaitEnd = clock::now();
        
        // IMPORTANT: Write neutral inputs to the last few buffer entries
        // to prevent lingering input patterns from triggering moves
        auto tCleanStart = clock::now();
        uintptr_t base = GetEFZBase();
        if (base) {
            for (int player = 1; player <= 2; player++) {
                uintptr_t playerPtr = GetPlayerPointer(player);
                if (playerPtr) {
                    // Read current buffer index
                    uint16_t currentIndex = 0;
                    if (SafeReadMemory(playerPtr + INPUT_BUFFER_INDEX_OFFSET, &currentIndex, sizeof(uint16_t))) {
                        // Write neutral (0x00) to the last 8 buffer entries
                        uint8_t neutral = 0x00;
                        for (int i = 0; i < 8; i++) {
                            int w = static_cast<int>(currentIndex) - i;
                            // wrap into [0, INPUT_BUFFER_SIZE)
                            w %= static_cast<int>(INPUT_BUFFER_SIZE);
                            if (w < 0) w += static_cast<int>(INPUT_BUFFER_SIZE);
                            uint16_t writeIndex = static_cast<uint16_t>(w);
                            SafeWriteMemory(playerPtr + INPUT_BUFFER_OFFSET + writeIndex, &neutral, sizeof(uint8_t));
                        }
                    }
                }
            }
        }
        auto tCleanEnd = clock::now();

        auto waitMs = std::chrono::duration_cast<std::chrono::milliseconds>(tWaitEnd - tWaitStart).count();
        auto cleanMs = std::chrono::duration_cast<std::chrono::milliseconds>(tCleanEnd - tCleanStart).count();
        auto totalMs = std::chrono::duration_cast<std::chrono::milliseconds>(tCleanEnd - tStart).count();
        LogOut(std::string("[INPUT_BUFFER] Buffer freezing stopped ") +
               "wait=" + std::to_string(waitMs) + "ms " +
               "clean=" + std::to_string(cleanMs) + "ms " +
               "total=" + std::to_string(totalMs) + "ms", true);
    }
}
