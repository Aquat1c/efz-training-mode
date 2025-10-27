#include "../include/input/input_freeze.h"
#include "../include/core/globals.h"
#include "../include/input/input_core.h"
#include "../include/core/memory.h"  
#include "../include/core/logger.h"
#include "../include/core/constants.h"
#include "../include/input/input_buffer.h"
#include "../include/utils/utilities.h"
#include "../include/game/game_state.h"
#include "../include/input/motion_system.h"
#include "../include/input/input_motion.h"  // Add this include for motion functions
#include "../include/game/frame_monitor.h" // Add this include for shared constants
// These functions are implemented in input_buffer.cpp
extern void FreezeBufferValuesThread(int playerNum);
extern bool CaptureAndFreezeBuffer(int playerNum, uint16_t startIndex, uint16_t length);
extern bool FreezeBufferIndex(int playerNum, uint16_t indexValue);
extern void StopBufferFreezing(void);
// Helper to freeze the perfect Dragon Punch motion
bool FreezePerfectDragonPunch(int playerNum) {
    // Stop any existing freeze thread
    StopBufferFreezing();
    
    LogOut("[BUFFER_FREEZE] Starting Dragon Punch buffer freeze for P" + std::to_string(playerNum), true);
    
    // Use the exact values from your successful DP execution
    std::vector<uint8_t> dpMotion = {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x02, 0x02, 0x02, 0x04, 
        0x04, 0x04, 0x26, 0x06, 0x06, 0x06, 0x00, 0x00
    };
    
    // Detailed logging of input sequence
    std::stringstream ss;
    ss << "[BUFFER_FREEZE] DP motion buffer values: ";
    for (size_t i = 0; i < dpMotion.size(); i++) {
        if (dpMotion[i] != 0) {
            ss << std::hex << std::setw(2) << std::setfill('0') 
               << static_cast<int>(dpMotion[i]) << "(" << DecodeInputMask(dpMotion[i]) << ") ";
        } else if (i > 0 && dpMotion[i-1] != 0) {
            ss << "00 ";  // Only show zeros that follow non-zero values
        }
    }
    LogOut(ss.str(), true);
    
    // Setup the frozen buffer values
    g_frozenBufferValues = dpMotion;
    g_frozenBufferLength = static_cast<uint16_t>(dpMotion.size());
    g_frozenBufferStartIndex = 128; // Place in middle of buffer for visibility
    
    // Set index to point at position 148 (128+20)
    g_frozenIndexValue = (g_frozenBufferStartIndex + 20) % INPUT_BUFFER_SIZE;
    g_indexFreezingActive = true;
    
    // Start freezing thread
    g_bufferFreezingActive = true;
    g_bufferFreezeThread = std::thread(FreezeBufferValuesThread, playerNum);
    g_bufferFreezeThread.detach();  // Detach to prevent termination
    
    LogOut("[BUFFER_FREEZE] Perfect Dragon Punch motion freezing activated at index " + 
           std::to_string(g_frozenIndexValue), true);
    return true;
}

// Enhanced version with diagnostic dump and adjusted index placement
bool FreezePerfectDragonPunchEnhanced(int playerNum) {
    // Stop any existing freeze thread
    StopBufferFreezing();
    
    LogOut("[BUFFER_DEBUG] Starting enhanced DP buffer freeze for P" + std::to_string(playerNum), true);
    
    // Get player pointer
    uintptr_t playerPtr = GetPlayerPointer(playerNum);
    if (!playerPtr) {
        LogOut("[BUFFER_DEBUG] Failed to get player pointer", true);
        return false;
    }
    
    // Read current buffer index for reference
    uint16_t currentIndex = 0;
    if (!SafeReadMemory(playerPtr + INPUT_BUFFER_INDEX_OFFSET, &currentIndex, sizeof(uint16_t))) {
        LogOut("[BUFFER_DEBUG] Failed to read current buffer index", true);
        return false;
    }
    
    // First dump the current buffer contents for diagnostic purposes
    DumpInputBuffer(playerNum);
    
    // Use the exact DP motion sequence from the console dump
    std::vector<uint8_t> dpMotion = {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x02, 0x02, 0x02, 0x04, 
        0x04, 0x04, 0x26, 0x06, 0x06, 0x00, 0x00, 0x00
    };
    
    // Calculate index based on current buffer position
    // We want to place the motion sequence just before the current read position
    g_frozenBufferStartIndex = (currentIndex + INPUT_BUFFER_SIZE - dpMotion.size()) % INPUT_BUFFER_SIZE;
    g_frozenBufferValues = dpMotion;
    g_frozenBufferLength = static_cast<uint16_t>(dpMotion.size());
    
    // Set the index to point at the start of the important part of the sequence
    g_frozenIndexValue = (g_frozenBufferStartIndex + 20) % INPUT_BUFFER_SIZE;
    g_indexFreezingActive = true;
    
    LogOut("[BUFFER_DEBUG] Current buffer index: " + std::to_string(currentIndex), true);
    LogOut("[BUFFER_DEBUG] Setting buffer start at: " + std::to_string(g_frozenBufferStartIndex), true);
    LogOut("[BUFFER_DEBUG] Setting frozen index to: " + std::to_string(g_frozenIndexValue), true);
    
    // Detailed logging of input sequence
    std::stringstream ss;
    ss << "[BUFFER_DEBUG] DP motion sequence: ";
    for (size_t i = 0; i < dpMotion.size(); i++) {
        ss << std::hex << std::setw(2) << std::setfill('0') 
           << static_cast<int>(dpMotion[i]) << "(" << DecodeInputMask(dpMotion[i]) << ") ";
    }
    LogOut(ss.str(), true);
    
    // Start freezing thread
    g_bufferFreezingActive = true;
    g_bufferFreezeThread = std::thread([playerNum]() {
    LogOut("[BUFFER_DEBUG] Enhanced freeze thread starting", true);
        FreezeBufferValuesThread(playerNum);
    });
    g_bufferFreezeThread.detach();  // Detach to prevent termination
    
    LogOut("[BUFFER_DEBUG] Enhanced DP buffer freeze activated for P" + 
           std::to_string(playerNum), true);
    return true;
}

bool ComboFreezeDP(int playerNum) {
    // Stop any existing freeze thread
    StopBufferFreezing();
    
    LogOut("[BUFFER_COMBO] Starting CheatEngine-style DP freeze for P" + std::to_string(playerNum), true);
    
    uintptr_t playerPtr = GetPlayerPointer(playerNum);
    if (!playerPtr) {
        LogOut("[BUFFER_COMBO] Failed to get player pointer", true);
        return false;
    }

    // Read initial buffer state
    uint16_t currentIndex = 0;
    SafeReadMemory(playerPtr + INPUT_BUFFER_INDEX_OFFSET, &currentIndex, sizeof(uint16_t));
    LogOut("[BUFFER_COMBO] Initial buffer index: " + std::to_string(currentIndex), true);
    
    // This is the exact CheatEngine pattern that works
    std::vector<uint8_t> dpMotion = {
        0x02, 0x02, 0x02,  // LEFT x3
        0x04, 0x04, 0x04,  // DOWN x3
        0x26,              // DOWN+LEFT+BUTTON
        0x06, 0x06, 0x06, 0x06, // DOWN+LEFT x4
        0x26, 0x26, 0x26, 0x26  // DOWN+LEFT+BUTTON x4
    };
    
    // Target index where we want to maintain the pattern
    g_frozenIndexValue = 149;
    g_frozenBufferValues = dpMotion;
    g_frozenBufferLength = static_cast<uint16_t>(dpMotion.size());
    
    // Enable buffer and index manipulation
    g_indexFreezingActive = true;
    g_bufferFreezingActive = true;
    
    // Start buffer freeze thread
    g_bufferFreezeThread = std::thread([playerNum, dpMotion]() {
        LogOut("[BUFFER_COMBO] Starting DP pattern buffer freeze thread", true);
        uintptr_t playerPtr = GetPlayerPointer(playerNum);
        if (!playerPtr) return;

        const unsigned long long startMs = GetTickCount64();
        unsigned long long lastLogMs = startMs;
        int counter = 0;
        uint16_t lastIndex = 0;
        short lastMoveID = -1;
        int sleepMs = 2;           // brief aggressive phase
        const int maxSleepMs = 4;  // then back off

        // Main freeze loop
        while (g_bufferFreezingActive) {
            if (g_onlineModeActive.load()) break;                   // never operate online
            if (GetCurrentGamePhase() != GamePhase::Match) break;   // only in match
            unsigned long long now = GetTickCount64();
            if (now - startMs > 4000ULL) break;                     // safety timeout ~4s

            // Read current index and moveID for monitoring
            uint16_t currentIndex = 0;
            short moveID = 0;
            SafeReadMemory(playerPtr + INPUT_BUFFER_INDEX_OFFSET, &currentIndex, sizeof(uint16_t));

            // Optional: Read moveID for sparse logging
            uintptr_t moveIDAddr = ResolvePointer(GetEFZBase(),
                (playerNum == 1) ? EFZ_BASE_OFFSET_P1 : EFZ_BASE_OFFSET_P2,
                MOVE_ID_OFFSET);
            if (moveIDAddr) {
                SafeReadMemory(moveIDAddr, &moveID, sizeof(short));
                if (moveID != lastMoveID && moveID != 0) {
                    if (now - lastLogMs >= 250ULL) { // throttle
                        LogOut("[BUFFER_COMBO] MoveID: " + std::to_string(lastMoveID) + " -> " + std::to_string(moveID), true);
                        lastLogMs = now;
                    }
                    lastMoveID = moveID;
                }
            }

            // Allow index to float in 149-152 range, only reset if it's outside
            if (currentIndex < 147 || currentIndex > 152) {
                SafeWriteMemory(playerPtr + INPUT_BUFFER_INDEX_OFFSET, &g_frozenIndexValue, sizeof(uint16_t));
                currentIndex = g_frozenIndexValue;
            }

            // Write the pattern near the current index with smaller spread (-1..1)
            for (int offset = -1; offset <= 1; offset++) {
                int basePos = (currentIndex - static_cast<int>(dpMotion.size()) / 2 + offset) % INPUT_BUFFER_SIZE;
                if (basePos < 0) basePos += INPUT_BUFFER_SIZE;
                for (size_t i = 0; i < dpMotion.size(); i++) {
                    uint16_t writeIndex = static_cast<uint16_t>((basePos + static_cast<int>(i)) % INPUT_BUFFER_SIZE);
                    SafeWriteMemory(playerPtr + INPUT_BUFFER_OFFSET + writeIndex, &dpMotion[i], sizeof(uint8_t));
                }
            }

            // Write the known-good location less frequently
            if ((counter % 10) == 0) {
                const int knownGoodStart = 144;
                for (size_t i = 0; i < dpMotion.size(); i++) {
                    SafeWriteMemory(playerPtr + INPUT_BUFFER_OFFSET + knownGoodStart + static_cast<int>(i), &dpMotion[i], sizeof(uint8_t));
                }
            }

            // Sparse status log
            if ((currentIndex != lastIndex) && (now - lastLogMs >= 250ULL)) {
                LogOut("[BUFFER_COMBO] Maintaining at index " + std::to_string(currentIndex), true);
                lastIndex = currentIndex;
                lastLogMs = now;
            }

            counter++;
            if (sleepMs < maxSleepMs && (now - startMs) > 250ULL) sleepMs = maxSleepMs;
            std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));
        }
    });
    g_bufferFreezeThread.detach();
    
    LogOut("[BUFFER_COMBO] DP buffer pattern freeze activated", true);
    return true;
}

bool FreezeBufferForMotion(int playerNum, int motionType, int buttonMask, int optimalIndex) {
    // Stop any existing freeze thread
    StopBufferFreezing();
    
    // Get player pointer and facing direction
    uintptr_t playerPtr = GetPlayerPointer(playerNum);
    if (!playerPtr) {
        LogOut("[BUFFER_FREEZE] Invalid player pointer", true);
        return false;
    }
    
    // Log basic information
    bool facingRight = GetPlayerFacingDirection(playerNum);
    LogOut("[BUFFER_FREEZE] Starting buffer freeze for motion " + GetMotionTypeName(motionType) + " (P" + std::to_string(playerNum) + ")", true);
    LogOut("[BUFFER_FREEZE] Begin session (" + GetMotionTypeName(motionType) + ") P" + std::to_string(playerNum), true);
    
    // Define directions based on facing
    uint8_t fwd = facingRight ? GAME_INPUT_RIGHT : GAME_INPUT_LEFT;
    uint8_t back = facingRight ? GAME_INPUT_LEFT : GAME_INPUT_RIGHT;
    uint8_t down = GAME_INPUT_DOWN;
    uint8_t downFwd = down | fwd;
    uint8_t downBack = down | back;
    
    std::string motionLabel = GetMotionTypeName(motionType);
    LogOut("[BUFFER_FREEZE] Player " + std::to_string(playerNum) + " facing " + (facingRight ? "right" : "left"), true);
    
    // Initialize pattern with carefully optimized sequence
    std::vector<uint8_t> pattern;
    
    switch (motionType) {
        case MOTION_623A: case MOTION_623B: case MOTION_623C: {
            // Dragon Punch (623): Forward, Down, Down-Forward + Button
            pattern = {
                0x00, 0x00,                             // Neutral padding (2)
                fwd, fwd, fwd,                          // Forward (3)
                down, down,                             // Down (2)
                downFwd, downFwd,                       // Down-Forward (2)
                (uint8_t)(downFwd | buttonMask),        // Down-Forward+Button (1)
                (uint8_t)(downFwd | buttonMask),        // Down-Forward+Button (1)
            };
            break;
        }
        
        case MOTION_214A: case MOTION_214B: case MOTION_214C: {
            // QCB: Down, Down-Back, Back + Button
            pattern = {
                0x00, 0x00,                             // Neutral padding (2)
                down, down, down,                       // Down (3)
                downBack, downBack,                     // Down-Back (2)
                back, back,                             // Back (2)
                (uint8_t)(back | buttonMask),           // Back+Button (1)
                (uint8_t)(back | buttonMask),           // Back+Button (1)
            };
            break;
        }
        
        case MOTION_236A: case MOTION_236B: case MOTION_236C: {
            // QCF: Down, Down-Forward, Forward + Button
            pattern = {
                0x00, 0x00,                             // Neutral padding (2)
                down, down, down,                       // Down (3)
                downFwd, downFwd,                       // Down-Forward (2)
                fwd, fwd,                               // Forward (2)
                (uint8_t)(fwd | buttonMask),            // Forward+Button (1)
                (uint8_t)(fwd | buttonMask),            // Forward+Button (1)
            };
            break;
        }
        case MOTION_421A: case MOTION_421B: case MOTION_421C: {
            pattern = {
                0x00, 0x00,                             // Neutral padding (2)
                back, back,                             // Back (2)
                down, down,                             // Down (2)
                downBack, downBack,                     // Down-Back (2)
                (uint8_t)(downBack | buttonMask),        // Down-Back+Button (1)
                (uint8_t)(downBack | buttonMask),        // Down-Back+Button (1)
            };
            break;
        }
        case MOTION_41236A: case MOTION_41236B: case MOTION_41236C: {
            // HCF: Back, Down-Back, Down, Down-Forward, Forward + Button
            pattern = {
                0x00, 0x00,                             // Neutral padding (2)
                back, back,                             // Back (2)
                downBack, downBack,                     // Down-Back (2)
                down, down,                             // Down (2)
                downFwd, downFwd,                       // Down-Forward (2)
                fwd,                                    // Forward (1)
                (uint8_t)(fwd | buttonMask),            // Forward+Button (1)
                (uint8_t)(fwd | buttonMask),            // Forward+Button (1)
            };
            break;
        }
        case MOTION_214214A: case MOTION_214214B: case MOTION_214214C: {
            // Double QCB: (Down, Down-Back, Back)x2 + Button
            pattern = {
                0x00, 0x00,                             // Neutral padding (2)
                down, down, down,                       // Down (3)
                downBack, downBack,                     // Down-Back (2)
                back, back,                             // Back (2)
                down, down, down,                       // Down (3)
                downBack, downBack,                     // Down-Back (2)
                back, back,                             // Back (2)
                (uint8_t)(back | buttonMask),           // Back+Button (1)
                (uint8_t)(back | buttonMask),           // Back+Button (1)
            };
            break;
        }
        case MOTION_236236A: case MOTION_236236B: case MOTION_236236C: {
            // Double QCF: (Down, Down-Forward, Forward)x2 + Button
            pattern = {
                0x00, 0x00,                             // Neutral padding (2)
                down, down, down,                       // Down (3)
                downFwd, downFwd,                       // Down-Forward (2)
                fwd, fwd,                               // Forward (2)
                down, down, down,                       // Down (3)
                downFwd, downFwd,                       // Down-Forward (2)
                fwd, fwd,                               // Forward (2)
                (uint8_t)(fwd | buttonMask),            // Forward+Button (1)
                (uint8_t)(fwd | buttonMask),            // Forward+Button (1)
            };
            break;
        }
        case MOTION_641236A: case MOTION_641236B: case MOTION_641236C: {
            pattern = {
                0x00, 0x00,                             // Neutral padding (2)
                fwd, fwd,                               // Forward (2)
                back, back,                             // Back (2)
                downBack, downBack,                     // Down-Back (2)
                down, down,                             // Down (2)
                downFwd, downFwd,                       // Down-Forward (2)
                fwd,                                    // Forward (1)
                (uint8_t)(fwd | buttonMask),            // Forward+Button (1)
                (uint8_t)(fwd | buttonMask),            // Forward+Button (1)
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
                0x00, 0x00, // neutral pause
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
            // 463214: Left, Right, Down-Right, Down, Down-Left, Left + Button
            // Using back=fwd swap logic later for facing; here we build canonical facing-right pattern
            pattern = {
                0x00, 0x00,
                back, back,          // 4 (treat 'back' as initial left when facing right)
                fwd, fwd,            // 6
                downFwd, downFwd,    // 3
                down, down,          // 2
                downBack, downBack,  // 1
                back,
                (uint8_t)(back | buttonMask),
                (uint8_t)(back | buttonMask),
            };
            break;
        }
        case MOTION_4123641236A: case MOTION_4123641236B: case MOTION_4123641236C: {
            // 41236 41236: Back, Down-Back, Down, Down-Forward, Forward x2 + Button only at final Forward
            pattern = {
                0x00, 0x00,
                back, back, downBack, downBack, down, down, downFwd, downFwd, fwd, fwd, // first 41236
                back, back, downBack, downBack, down, down, downFwd, downFwd, fwd,      // second 41236 up to final forward
                (uint8_t)(fwd | buttonMask),
                (uint8_t)(fwd | buttonMask),
            };
            break;
        }
        case MOTION_6321463214A: case MOTION_6321463214B: case MOTION_6321463214C: {
            // 6321463214: Fwd, Down-Fwd, Down, Down-Back, Back, Fwd, Down-Fwd, Down, Down-Back, Back + Button (represented with diagonals)
            // Using canonical facing-right mapping of digits; adapt using fwd/back variables.
            // Sequence digits: 6,3,2,1,4,6,3,2,1,4 + Button
            // Map: 6=fwd, 3=downFwd, 2=down, 1=downBack, 4=back
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
            LogOut("[BUFFER_FREEZE] Unsupported motion type: " + std::to_string(motionType), true);
            return false;
    }
    
    // Log the pattern values with direction names for debugging
    std::stringstream ss;
    ss << "[BUFFER_FREEZE] Pattern values for " << motionLabel << ": ";
    for (size_t i = 0; i < pattern.size(); i++) {
        ss << DecodeInputMask(pattern[i]) << " ";
    }
    LogOut(ss.str(), true);
    
    // OPTIMIZATION: Always place pattern at the beginning of the buffer (index 0)
    const uint16_t startIndex = 0;
    
    // First clear the entire buffer section we'll use plus a few extra bytes for safety
    const int clearPadding = 4;
    const uint16_t clearLength = static_cast<uint16_t>(pattern.size() + clearPadding);
    
    for (uint16_t i = 0; i < clearLength; i++) {
        uint16_t idx = (startIndex + i) % INPUT_BUFFER_SIZE;
        uint8_t zero = 0;
        SafeWriteMemory(playerPtr + INPUT_BUFFER_OFFSET + idx, &zero, sizeof(uint8_t));
    }
    
    // Now write our pattern at the start of the buffer
    for (size_t i = 0; i < pattern.size(); i++) {
        uint16_t idx = (startIndex + i) % INPUT_BUFFER_SIZE;
        SafeWriteMemory(playerPtr + INPUT_BUFFER_OFFSET + idx, &pattern[i], sizeof(uint8_t));
    }
    
    // Set up globals for the freeze thread
    g_frozenBufferValues = pattern;
    g_frozenBufferStartIndex = startIndex;
    g_frozenBufferLength = static_cast<uint16_t>(pattern.size());
    
    // Set index to point at the end of our pattern minus 1 (to ensure button press is read)
    g_frozenIndexValue = (startIndex + g_frozenBufferLength - 1) % INPUT_BUFFER_SIZE;
    g_indexFreezingActive = true;
    
    // Start freezing thread
    g_bufferFreezingActive = true;
    g_bufferFreezeThread = std::thread(FreezeBufferValuesThread, playerNum);
    g_bufferFreezeThread.detach();
    
    LogOut("[BUFFER_FREEZE] Buffer freeze for " + motionLabel + " activated at index " + 
           std::to_string(g_frozenIndexValue), true);
    return true;
}

namespace {
    struct FreezeSessionState {
        std::atomic<bool> active{false};
        std::atomic<bool> threadRunning{false};
        uint16_t originalIndex{0};
        bool originalIndexValid{false};
    };
    FreezeSessionState g_freezeSession[3]; // 1,2 (ignore 0)
}

// Clear (zero) entire buffer + index safely
void ClearPlayerInputBuffer(int playerNum) {
    uintptr_t playerPtr = GetPlayerPointer(playerNum);
    if (!playerPtr) return;
    uint8_t zero = 0x00;
    for (uint16_t i = 0; i < INPUT_BUFFER_SIZE; ++i) {
        SafeWriteMemory(playerPtr + INPUT_BUFFER_OFFSET + i, &zero, sizeof(uint8_t));
    }
    uint16_t idxZero = 0;
    SafeWriteMemory(playerPtr + INPUT_BUFFER_INDEX_OFFSET, &idxZero, sizeof(uint16_t));
    LogOut(std::string("[BUFFER_FREEZE] Cleared full buffer & index for P") + std::to_string(playerNum), true);
}

void BeginBufferFreezeSession(int playerNum, std::string_view label) {
    StopBufferFreezing();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    auto &s = g_freezeSession[playerNum];
    s.active.store(true);
    s.threadRunning.store(false);
    s.originalIndexValid = false;
    uintptr_t playerPtr = GetPlayerPointer(playerNum);
    if (playerPtr) {
        uint16_t curIdx = 0;
        if (SafeReadMemory(playerPtr + INPUT_BUFFER_INDEX_OFFSET, &curIdx, sizeof(uint16_t))) {
            s.originalIndex = curIdx;
            s.originalIndexValid = true;
        }
    }
    LogOut(std::string("[BUFFER_FREEZE] Begin session (") + (label.empty() ? "" : std::string(label)) +
           ") P" + std::to_string(playerNum), true);
}

void EndBufferFreezeSession(int playerNum, const char* reason, bool clearGlobals) {
    auto &s = g_freezeSession[playerNum];
    if (!s.active.load()) return;

    using clock = std::chrono::steady_clock;
    auto tStart = clock::now();

    // Signal stop
    g_bufferFreezingActive = false;
    g_indexFreezingActive  = false;

    // Wait briefly if a thread may still be winding down
    auto tWaitStart = clock::now();
    for (int i=0; i<20 && s.threadRunning.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    auto tWaitEnd = clock::now();

    // Light-weight cleanup: neutralize only the region we wrote (if any) and a small tail near the current index.
    // Avoid clearing the entire buffer which can cause a brief frame hitch.
    auto tCleanStart = clock::now();
    uintptr_t playerPtr = GetPlayerPointer(playerNum);
    if (playerPtr) {
        // Neutralize frozen pattern region
        if (g_frozenBufferLength > 0) {
            uint8_t zero = 0x00;
            for (uint16_t i = 0; i < g_frozenBufferLength; ++i) {
                uint16_t idx = (g_frozenBufferStartIndex + i) % INPUT_BUFFER_SIZE;
                SafeWriteMemory(playerPtr + INPUT_BUFFER_OFFSET + idx, &zero, sizeof(uint8_t));
            }
        }

        // Neutralize a small sliding window behind the current index to prevent ghost follow-ups
        uint16_t curIdx = 0;
        if (SafeReadMemory(playerPtr + INPUT_BUFFER_INDEX_OFFSET, &curIdx, sizeof(uint16_t))) {
            uint8_t zero = 0x00;
            for (int i = 0; i < 8; ++i) {
                int w = static_cast<int>(curIdx) - i;
                w %= static_cast<int>(INPUT_BUFFER_SIZE);
                if (w < 0) w += static_cast<int>(INPUT_BUFFER_SIZE);
                uint16_t idx = static_cast<uint16_t>(w);
                SafeWriteMemory(playerPtr + INPUT_BUFFER_OFFSET + idx, &zero, sizeof(uint8_t));
            }
        }

        // Optionally restore the original index if we captured it at session begin
        if (s.originalIndexValid) {
            SafeWriteMemory(playerPtr + INPUT_BUFFER_INDEX_OFFSET, &s.originalIndex, sizeof(uint16_t));
        }
    }
    auto tCleanEnd = clock::now();

    s.active.store(false);
    s.threadRunning.store(false);

    if (clearGlobals) {
        g_frozenBufferLength = 0;
        g_frozenBufferValues.clear();
    }

    auto tEnd = clock::now();
    auto waitMs = std::chrono::duration_cast<std::chrono::milliseconds>(tWaitEnd - tWaitStart).count();
    auto cleanMs = std::chrono::duration_cast<std::chrono::milliseconds>(tCleanEnd - tCleanStart).count();
    auto totalMs = std::chrono::duration_cast<std::chrono::milliseconds>(tEnd - tStart).count();
    LogOut(std::string("[BUFFER_FREEZE] End session P") + std::to_string(playerNum) +
           " (" + (reason?reason:"no reason") + ")" +
           " wait=" + std::to_string(waitMs) + "ms" +
           " clean=" + std::to_string(cleanMs) + "ms" +
           " total=" + std::to_string(totalMs) + "ms", true);
}

// Generic pattern freeze for bespoke sequences (Final Memory, multi-phase inputs, etc.)
bool FreezeBufferWithPattern(int playerNum, const std::vector<uint8_t>& patternIn) {
    StopBufferFreezing();
    if (patternIn.empty()) return false;
    uintptr_t playerPtr = GetPlayerPointer(playerNum);
    if (!playerPtr) return false;
    // Sanitize: cap extremely large patterns
    std::vector<uint8_t> pattern = patternIn;
    if (pattern.size() > 120) pattern.resize(120); // safety cap

    // Clear target region first (pattern + small padding)
    const uint16_t startIndex = 0;
    const uint16_t clearLength = static_cast<uint16_t>(pattern.size() + 4);
    for (uint16_t i = 0; i < clearLength; ++i) {
        uint8_t z = 0; SafeWriteMemory(playerPtr + INPUT_BUFFER_OFFSET + ((startIndex + i) % INPUT_BUFFER_SIZE), &z, 1);
    }
    // Write pattern
    for (size_t i = 0; i < pattern.size(); ++i) {
        SafeWriteMemory(playerPtr + INPUT_BUFFER_OFFSET + ((startIndex + i) % INPUT_BUFFER_SIZE), &pattern[i], 1);
    }
    g_frozenBufferValues = pattern;
    g_frozenBufferStartIndex = startIndex;
    g_frozenBufferLength = static_cast<uint16_t>(pattern.size());
    g_frozenIndexValue = (startIndex + g_frozenBufferLength - 1) % INPUT_BUFFER_SIZE;
    g_indexFreezingActive = true;
    g_bufferFreezingActive = true;
    g_bufferFreezeThread = std::thread(FreezeBufferValuesThread, playerNum);
    g_bufferFreezeThread.detach();
    LogOut("[BUFFER_FREEZE] Generic pattern freeze active (len=" + std::to_string(pattern.size()) + ") P" + std::to_string(playerNum), true);
    return true;
}

// Overload with index advance capability.
bool FreezeBufferWithPattern(int playerNum, const std::vector<uint8_t>& patternIn, int extraNeutralFrames) {
    if (extraNeutralFrames <= 0) {
        return FreezeBufferWithPattern(playerNum, patternIn);
    }
    StopBufferFreezing();
    if (patternIn.empty()) return false;
    uintptr_t playerPtr = GetPlayerPointer(playerNum);
    if (!playerPtr) return false;
    std::vector<uint8_t> pattern = patternIn;
    if (pattern.size() > 120) pattern.resize(120);
    // Cap extra neutrals so total stays sane
    if (extraNeutralFrames > 30) extraNeutralFrames = 30;
    const uint16_t startIndex = 0;
    const uint16_t clearLength = static_cast<uint16_t>(pattern.size() + extraNeutralFrames + 4);
    for (uint16_t i = 0; i < clearLength; ++i) {
        uint8_t z = 0; SafeWriteMemory(playerPtr + INPUT_BUFFER_OFFSET + ((startIndex + i) % INPUT_BUFFER_SIZE), &z, 1);
    }
    for (size_t i = 0; i < pattern.size(); ++i) {
        SafeWriteMemory(playerPtr + INPUT_BUFFER_OFFSET + ((startIndex + i) % INPUT_BUFFER_SIZE), &pattern[i], 1);
    }
    g_frozenBufferValues = pattern; // store only real pattern (neutrals virtual)
    g_frozenBufferStartIndex = startIndex;
    g_frozenBufferLength = static_cast<uint16_t>(pattern.size());
    g_frozenIndexValue = (startIndex + g_frozenBufferLength + extraNeutralFrames - 1) % INPUT_BUFFER_SIZE;
    g_indexFreezingActive = true;
    g_bufferFreezingActive = true;
    g_bufferFreezeThread = std::thread(FreezeBufferValuesThread, playerNum);
    g_bufferFreezeThread.detach();
    LogOut("[BUFFER_FREEZE] Pattern freeze+advance (len=" + std::to_string(pattern.size()) + "+" + std::to_string(extraNeutralFrames) + ") idx=" + std::to_string(g_frozenIndexValue) + " P" + std::to_string(playerNum), true);
    return true;
}