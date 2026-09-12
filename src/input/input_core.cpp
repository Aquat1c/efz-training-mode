#include "../include/input/input_core.h"
#include "../include/core/memory.h"
#include "../include/core/logger.h"
#include "../include/core/constants.h"
#include "../include/utils/utilities.h"
    // For GetEFZBase()
#include "../include/input/input_buffer.h" // For INPUT_BUFFER_* constants
#include <sstream>
#include <cstring>

// Decode input mask to readable string
std::string DecodeInputMask(uint8_t inputMask) {
    if (inputMask == 0)
        return "N";
    std::string result;
    
    // Directions
    if (inputMask & GAME_INPUT_UP) {
        if (inputMask & GAME_INPUT_LEFT) result += "UL";
        else if (inputMask & GAME_INPUT_RIGHT) result += "UR";
        else result += "U";
    } else if (inputMask & GAME_INPUT_DOWN) {
        if (inputMask & GAME_INPUT_LEFT) result += "DL";
        else if (inputMask & GAME_INPUT_RIGHT) result += "DR";
        else result += "D";
    } else if (inputMask & GAME_INPUT_LEFT) {
        result += "L";
    } else if (inputMask & GAME_INPUT_RIGHT) {
        result += "R";
    }
    
    // Buttons
    if (inputMask & GAME_INPUT_A) result += "A";
    if (inputMask & GAME_INPUT_B) result += "B";
    if (inputMask & GAME_INPUT_C) result += "C";
    if (inputMask & GAME_INPUT_D) result += "D";
    
    return result;
}

// Helper function to get player base pointer
uintptr_t GetPlayerPointer(int playerNum) {
    uintptr_t base = GetEFZBase();
    if (!base) return 0;
    
    uintptr_t playerPtrOffset = (playerNum == 1) ? EFZ_BASE_OFFSET_P1 : EFZ_BASE_OFFSET_P2;
    uintptr_t playerPtr = 0;
    
    if (!SafeReadMemory(base + playerPtrOffset, &playerPtr, sizeof(uintptr_t))) {
        return 0;
    }
    
    return playerPtr;
}

// Write to the player's immediate input registers
bool WritePlayerInputImmediate(int playerNum, uint8_t inputMask) {
    return WritePlayerInputImmediateAt(GetPlayerPointer(playerNum), inputMask);
}

bool WritePlayerInputImmediateAt(uintptr_t playerPtr, uint8_t inputMask) {
    if (!playerPtr) return false;
    
    // Extract direction and button components
    bool up = (inputMask & GAME_INPUT_UP) != 0;
    bool down = (inputMask & GAME_INPUT_DOWN) != 0;
    bool left = (inputMask & GAME_INPUT_LEFT) != 0;
    bool right = (inputMask & GAME_INPUT_RIGHT) != 0;
    
    // Convert to game's internal format
    uint8_t horzValue = 0;
    uint8_t vertValue = 0;
    
    // Horizontal: 0=neutral, 1=right, 255=left
    if (right) horzValue = 1;
    else if (left) horzValue = 255;
    
    // Vertical: 0=neutral, 1=down, 255=up
    if (down) vertValue = 1;
    else if (up) vertValue = 255;
    
    static_assert(INPUT_VERTICAL_OFFSET == INPUT_HORIZONTAL_OFFSET + 1 &&
                  INPUT_BUTTON_A_OFFSET == INPUT_HORIZONTAL_OFFSET + 2 &&
                  INPUT_BUTTON_B_OFFSET == INPUT_HORIZONTAL_OFFSET + 3 &&
                  INPUT_BUTTON_C_OFFSET == INPUT_HORIZONTAL_OFFSET + 4 &&
                  INPUT_BUTTON_D_OFFSET == INPUT_HORIZONTAL_OFFSET + 5,
                  "Immediate input registers must remain contiguous");
    const uint8_t staged[6] = {horzValue, vertValue,
        uint8_t((inputMask & GAME_INPUT_A) != 0), uint8_t((inputMask & GAME_INPUT_B) != 0),
        uint8_t((inputMask & GAME_INPUT_C) != 0), uint8_t((inputMask & GAME_INPUT_D) != 0)};
    // The caller retains its producer/control ownership through this copy.
    // Six bytes are not atomic; SEH reports an access fault, not native lifetime.
    // Fighter data is already writable and must not change page protections.
    __try {
        std::memcpy(reinterpret_cast<void*>(playerPtr + INPUT_HORIZONTAL_OFFSET), staged, sizeof(staged));
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Write to the circular buffer (for move history/detection)
bool WritePlayerInputToBuffer(int playerNum, uint8_t inputMask) {
    uintptr_t playerPtr = GetPlayerPointer(playerNum);
    if (!playerPtr) return false;
    
    // Get current buffer index
    uint16_t currentIndex = 0;
    if (!SafeReadMemory(playerPtr + INPUT_BUFFER_INDEX_OFFSET, &currentIndex, sizeof(uint16_t)))
        return false;
    
    // Write to current index in the buffer
    uint16_t bufferIndex = currentIndex % INPUT_BUFFER_SIZE;
    if (!SafeWriteMemory(playerPtr + INPUT_BUFFER_OFFSET + bufferIndex, &inputMask, sizeof(uint8_t)))
        return false;
    
    // Debug logging for dash investigation (gate under detailedLogging)
    if (detailedLogging.load()) {
        LogOut("[BUFFER_WRITE] P" + std::to_string(playerNum) + " wrote " + DecodeInputMask(inputMask) + 
               " at index " + std::to_string(currentIndex) + " -> " + std::to_string(currentIndex + 1), true);
    }
    
    // Increment buffer index
    currentIndex = (currentIndex + 1) % INPUT_BUFFER_SIZE;
    if (!SafeWriteMemory(playerPtr + INPUT_BUFFER_INDEX_OFFSET, &currentIndex, sizeof(uint16_t)))
        return false;
    
    return true;
}

// Main function to write player input
bool WritePlayerInput(int playerNum, uint8_t inputMask) {
    // First write to immediate input registers
    if (!WritePlayerInputImmediate(playerNum, inputMask))
        return false;
    
    // Then write to input buffer for move detection
    if (!WritePlayerInputToBuffer(playerNum, inputMask))
        return false;
    
    return true;
}

// Reset (rewind) the player's circular input buffer index to 0. This gives
// a clean, deterministic window for subsequent queued motions (notably the
// forward dash sequence which is sensitive to relative placement in history).
// We do NOT clear historical bytes here; the dash recognizer only cares about
// the forward/neutral/forward pattern we are about to enqueue. Optionally we
// could neutralize a few trailing bytes later if needed.
bool ResetPlayerInputBufferIndex(int playerNum) {
    uintptr_t playerPtr = GetPlayerPointer(playerNum);
    if (!playerPtr) return false;
    
    // Dump buffer state before reset (debug only)
    if (detailedLogging.load()) {
        DumpInputBuffer(playerNum, "BEFORE_INDEX_RESET");
    }
    
    uint16_t zero = 0;
    if (!SafeWriteMemory(playerPtr + INPUT_BUFFER_INDEX_OFFSET, &zero, sizeof(uint16_t))) {
        LogOut("[INPUT] Failed to reset buffer index for P" + std::to_string(playerNum), true);
        return false;
    }
    LogOut("[INPUT] Reset buffer index to 0 for P" + std::to_string(playerNum), true);
    
    // Dump after reset (debug only)
    if (detailedLogging.load()) {
        DumpInputBuffer(playerNum, "AFTER_INDEX_RESET");
    }
    
    return true;
}

// Read MoveID using authoritative offset (MOVE_ID_OFFSET)
uint16_t GetPlayerMoveID(int playerNum) {
    uintptr_t playerPtr = GetPlayerPointer(playerNum);
    if (!playerPtr) {
        LogOut("[BUFFER_DUMP] GetPlayerMoveID: null playerPtr for P" + std::to_string(playerNum), true);
        return 0; // safest fallback: idle
    }
    uint16_t moveID = 0;
    if (!SafeReadMemory(playerPtr + MOVE_ID_OFFSET, &moveID, sizeof(uint16_t))) {
        LogOut("[BUFFER_DUMP] GetPlayerMoveID: failed to read MoveID for P" + std::to_string(playerNum), true);
        return 0;
    }
    return moveID;
}

// Dump buffer contents for debugging dash issues
void DumpInputBuffer(int playerNum, const std::string& context) {
    uintptr_t playerPtr = GetPlayerPointer(playerNum);
    if (!playerPtr) return;
    
    // Read buffer index
    uint16_t bufferIndex = 0;
    if (!SafeReadMemory(playerPtr + INPUT_BUFFER_INDEX_OFFSET, &bufferIndex, sizeof(uint16_t))) {
        LogOut("[BUFFER_DUMP][" + context + "] Failed to read buffer index for P" + std::to_string(playerNum), true);
        return;
    }
    
    // Read MoveID
    uint16_t moveID = GetPlayerMoveID(playerNum);
    
    // MoveID is authoritative; drop the duplicate/ambiguous CurrentAnim read
    
    // Read last 15 positions (game checks last 5 for dash, but we want more context)
    std::stringstream ss;
    ss << "[BUFFER_DUMP][" << context << "] P" << playerNum 
       << " Index=" << bufferIndex 
    << " MoveID=" << moveID
       << " Last15=[";
    
    int startPos = (bufferIndex >= 15) ? (bufferIndex - 15) : 0;
    for (int i = startPos; i < bufferIndex; ++i) {
        uint8_t value = 0;
        if (SafeReadMemory(playerPtr + INPUT_BUFFER_OFFSET + i, &value, sizeof(uint8_t))) {
            if (i > startPos) ss << ",";
            ss << (int)i << ":" << DecodeInputMask(value);
        }
    }
    ss << "]";
    
    // Also show the critical 5-position recognition window (index-5 to index)
    ss << " RecogWindow=[";
    int recogStart = (bufferIndex >= 5) ? (bufferIndex - 5) : 0;
    for (int i = recogStart; i < bufferIndex; ++i) {
        uint8_t value = 0;
        if (SafeReadMemory(playerPtr + INPUT_BUFFER_OFFSET + i, &value, sizeof(uint8_t))) {
            if (i > recogStart) ss << ",";
            ss << DecodeInputMask(value);
        }
    }
    ss << "]";
    
    LogOut(ss.str(), true);
}

// Zero out the entire circular buffer region and optionally reset index to 0.
static bool ClearPlayerInputBufferAt(uintptr_t playerPtr, int playerNum,
                                     bool resetIndex) {
    if (!playerPtr) return false;
    
    // Dump before clearing for debugging
    if (playerNum && detailedLogging.load()) {
        DumpInputBuffer(playerNum, "BEFORE_CLEAR");
    }
    
    // Bulk clear the entire circular buffer region in one write
    std::vector<uint8_t> zeros(INPUT_BUFFER_SIZE, 0x00);
    bool ok = SafeWriteMemory(playerPtr + INPUT_BUFFER_OFFSET, zeros.data(), INPUT_BUFFER_SIZE);
    if (resetIndex) {
        uint16_t zero = 0;
        if (!SafeWriteMemory(playerPtr + INPUT_BUFFER_INDEX_OFFSET, &zero, sizeof(uint16_t))) {
            ok = false;
        }
    }
    if (playerNum) LogOut(std::string("[INPUT] Cleared input buffer for P") + std::to_string(playerNum) + (resetIndex?" (index reset)":""), true);
    return ok;
}

bool ClearPlayerInputBuffer(int playerNum, bool resetIndex) {
    return ClearPlayerInputBufferAt(GetPlayerPointer(playerNum), playerNum, resetIndex);
}

// Clear engine command flags (command buffer and dash command) to avoid post-macro transitions
bool ClearPlayerCommandFlags(int playerNum) {
    const bool cleared = ClearPlayerCommandFlagsAt(GetPlayerPointer(playerNum));
    LogOut(std::string("[INPUT] Cleared command/dash flags for P") + std::to_string(playerNum), true);
    return cleared;
}

bool ClearPlayerCommandFlagsAt(uintptr_t playerPtr) {
    if (!playerPtr) return false;
    uint8_t zero = 0;
    bool ok1 = SafeWriteMemory(playerPtr + COMMAND_BUFFER_OFFSET, &zero, sizeof(uint8_t));
    bool ok2 = SafeWriteMemory(playerPtr + DASH_COMMAND_OFFSET, &zero, sizeof(uint8_t));
    return ok1 && ok2;
}

// Write the complete 16-bit sentinel used by EFZ's command consumers.  A
// one-byte write leaves the old high byte behind and can turn a neutralized
// command into a different, still-live token.
bool NeutralizeMotionToken(int playerNum) {
    uintptr_t playerPtr = GetPlayerPointer(playerNum);
    if (!playerPtr) return false;
    const uint16_t token = 99;
    bool ok = SafeWriteMemory(playerPtr + MOTION_TOKEN_OFFSET,
                              &token, sizeof(token));
    if (detailedLogging.load()) {
        LogOut(std::string("[INPUT] Neutralized motion token for P") + std::to_string(playerNum) +
               std::string(" -> 99 (word)"), true);
    }
    return ok;
}

// Perform a thorough cleanup after toggling control or finishing macro playback
bool FullCleanupAfterToggle(int playerNum) {
    const bool cleared = FullCleanupAfterToggleAt(GetPlayerPointer(playerNum));
    LogOut(std::string("[INPUT] Full cleanup after toggle for P") + std::to_string(playerNum), true);
    return cleared;
}

bool FullCleanupAfterToggleAt(uintptr_t playerPtr) {
    if (!playerPtr) return false;
    bool okAll = true;

    // 1) Neutralize motion token
    const uint16_t token = 99;
    okAll = SafeWriteMemory(playerPtr + MOTION_TOKEN_OFFSET, &token, sizeof(token)) && okAll;

    // 2) Clear command/latch bytes and dash timer
    uint8_t zero = 0;
    okAll = SafeWriteMemory(playerPtr + INPUT_LATCH1_OFFSET, &zero, sizeof(uint8_t)) && okAll;
    okAll = SafeWriteMemory(playerPtr + INPUT_LATCH2_OFFSET, &zero, sizeof(uint8_t)) && okAll;
    okAll = SafeWriteMemory(playerPtr + DASH_TIMER_OFFSET, &zero, sizeof(uint8_t)) && okAll;

    // 3) Clear circular buffer and reset head
    okAll = ClearPlayerInputBufferAt(playerPtr, 0, /*resetIndex=*/true) && okAll;

    // 4) Clear immediate registers to neutral
    okAll = WritePlayerInputImmediateAt(playerPtr, GAME_INPUT_NEUTRAL) && okAll;

    // 5) Clear EFZ's native held-button and human-direction bookkeeping. These
    // are populated by processCharacterInput independently of +392..397; if
    // they survive a macro/controller hand-off, the next identical button may
    // not produce a rising edge and Practice can retain a stale block direction.
    const uint32_t zero32 = 0;
    const uint16_t zero16 = 0;
    constexpr uintptr_t heldOffsets[] = {400, 404, 408, 412};
    for (uintptr_t offset : heldOffsets) {
        okAll = SafeWriteMemory(playerPtr + offset, &zero32, sizeof(zero32)) &&
                okAll;
    }
    okAll = SafeWriteMemory(playerPtr + 334, &zero16, sizeof(zero16)) && okAll;
    okAll = SafeWriteMemory(playerPtr + 336, &zero32, sizeof(zero32)) && okAll;

    return okAll;
}
