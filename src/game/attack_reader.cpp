#include "../include/game/attack_reader.h"
#include "../include/game/collision_hook.h"
#include "../include/core/memory.h"
#include "../include/core/logger.h"
#include "../include/core/constants.h"
#include "../include/utils/utilities.h"
#include "../include/input/input_core.h"
#include <sstream>  
#include <string>
#include <atomic>

#define ATTACK_FLAGS_OFFSET     0xAA
#define ATTACK_DAMAGE_OFFSET    0xA8
#define ATTACK_BLOCKSTUN_OFFSET 0xB2
#define ATTACK_HITSTOP_OFFSET   0xC2
#define ATTACK_ACTIVE_REMAINING 0xEA

// Attack type flags (these would need to be determined by testing)
#define ATTACK_FLAG_HIGH      0x01
#define ATTACK_FLAG_LOW       0x02
#define ATTACK_FLAG_THROW     0x04
#define ATTACK_FLAG_MID       0x08

// Cache: discovered nested attack-data offset within frame-data for each player
static std::atomic<int> g_fdToAtkOffsetP1{-1};
static std::atomic<int> g_fdToAtkOffsetP2{-1};
auto ToHexString = [](int value) -> std::string {
            std::ostringstream oss;
            oss << "0x" << std::hex << value;
            return oss.str();
        };
static bool IsPlausibleAttackDataPtr(uintptr_t atkPtr) {
    if (!atkPtr) return false;
    // quick range sanity for 32-bit process (efz.exe/heap address range)
    if (atkPtr < 0x00400000 || atkPtr > 0x0FFFFFFF) return false;
    uint8_t flags = 0;
    uint16_t dmg = 0, bs = 0, hs = 0;
    if (!SafeReadMemory(atkPtr + ATTACK_FLAGS_OFFSET, &flags, sizeof(flags))) return false;
    // Flags can be 0; that's okay. Read some other words; if any read fails, reject.
    if (!SafeReadMemory(atkPtr + ATTACK_DAMAGE_OFFSET, &dmg, sizeof(dmg))) return false;
    if (!SafeReadMemory(atkPtr + ATTACK_BLOCKSTUN_OFFSET, &bs, sizeof(bs))) return false;
    if (!SafeReadMemory(atkPtr + ATTACK_HITSTOP_OFFSET, &hs, sizeof(hs))) return false;
    // Basic plausibility bounds (avoid absurd huge values that indicate random pointers)
    if (dmg > 5000 || bs > 1000 || hs > 1000) return false;
    return true;
}

static bool TryFindNestedAttackPtr(uintptr_t frameData, int& outOffset, uintptr_t& outAtk) {
    if (!frameData) return false;
    // scan first 0x200 bytes for a pointer field
    for (int off = 0; off <= 0x200 - 4; off += 4) {
        uintptr_t candidate = 0;
        if (!SafeReadMemory(frameData + off, &candidate, sizeof(candidate))) continue;
        if (IsPlausibleAttackDataPtr(candidate)) {
            outOffset = off;
            outAtk = candidate;
            return true;
        }
    }
    return false;
}

void AttackReader::LogMoveData(int playerNum, short moveID) {
    if (moveID <= 0) {
        return; // Not an attack move
    }
    
    uintptr_t base = GetEFZBase();
    if (!base) return;
    
    uintptr_t playerOffset = (playerNum == 1) ? EFZ_BASE_OFFSET_P1 : EFZ_BASE_OFFSET_P2;
    uintptr_t playerPtr = 0;
    
    if (!SafeReadMemory(base + playerOffset, &playerPtr, sizeof(uintptr_t)) || !playerPtr) {
        return;
    }
    
    // Get attack data pointer
    uintptr_t attackDataPtr = GetAttackDataPtr(playerPtr, moveID);
    if (!attackDataPtr) {
        LogOut("[ATTACK_READER] P" + std::to_string(playerNum) + " move " + 
               std::to_string(moveID) + " - Failed to get attack data", true);
        return;
    }
    
    // Read attack properties
    uint8_t attackTypeFlags = 0;
    uint16_t activeRemain = 0;
    uint16_t damage = 0;
    uint16_t blockstun = 0;
    uint16_t hitstop = 0;
    
    // Only log if we can read the attack type
    if (!SafeReadMemory(attackDataPtr + ATTACK_FLAGS_OFFSET, &attackTypeFlags, sizeof(uint8_t))) {
        return;
    }
    
    // Try to read other properties
    SafeReadMemory(attackDataPtr + ATTACK_ACTIVE_REMAINING, &activeRemain, sizeof(uint16_t));
    SafeReadMemory(attackDataPtr + ATTACK_DAMAGE_OFFSET, &damage, sizeof(uint16_t));
    SafeReadMemory(attackDataPtr + ATTACK_BLOCKSTUN_OFFSET, &blockstun, sizeof(uint16_t));
    SafeReadMemory(attackDataPtr + ATTACK_HITSTOP_OFFSET, &hitstop, sizeof(uint16_t));
    
    // Determine attack height
    std::string heightStr = "Unknown";
    if (attackTypeFlags & 0x40 || attackTypeFlags & 0x80)
        heightStr = "THROW (Unblockable)";
    else if (attackTypeFlags & 0x01)
        heightStr = "LOW";
    else if (attackTypeFlags & 0x04)
        heightStr = "HIGH";
    else if (attackTypeFlags & 0x02)
        heightStr = "MID";
    
    // Log the complete attack data
    std::stringstream ss;
    ss << "[ATTACK_DATA] P" << playerNum << " Move ID: " << moveID 
       << " | Type: 0x" << std::hex << (int)attackTypeFlags << std::dec
       << " (" << heightStr << ")"
    << " | ActiveRemain: " << activeRemain
       << " | Damage: " << damage
       << " | Blockstun: " << blockstun
       << " | Hitstop: " << hitstop;
    
    LogOut(ss.str(), true);
}

AttackHeight AttackReader::GetAttackHeight(int playerPtr, short moveID) {
    if (moveID <= 0) {
        return ATTACK_HEIGHT_UNKNOWN;
    }
    
    // Need to find the move data pointer from playerPtr + moveID table
    uintptr_t attackDataPtr = GetAttackDataPtr(playerPtr, moveID);
    if (!attackDataPtr) {
        return ATTACK_HEIGHT_UNKNOWN;
    }
    
    // Attack type is at offset 170 in the attack data
    int attackType = 0;
    if (!SafeReadMemory(attackDataPtr + ATTACK_FLAGS_OFFSET, &attackType, sizeof(uint8_t))) {
        return ATTACK_HEIGHT_UNKNOWN;
    }
    
    // Log the attack properties we found
    LogOut(std::string("[ATTACK_READER] Attack height flags: 0x") + 
           std::to_string(attackType) + " for moveID " + std::to_string(moveID), true);
    
    // Check flags in order of priority
    if (attackType & 0x40 || attackType & 0x80)
        return ATTACK_HEIGHT_THROW; // Unblockable
    else if (attackType & 0x01)
        return ATTACK_HEIGHT_LOW;   // Low attack
    else if (attackType & 0x04)
        return ATTACK_HEIGHT_HIGH;  // High attack
    else if (attackType & 0x02)
        return ATTACK_HEIGHT_MID;   // Mid attack
    
    return ATTACK_HEIGHT_MID; // Default to mid if unknown
}

bool AttackReader::IsMoveActive(int playerPtr, short moveID) {
    if (moveID <= 0)
        return false;
        
    uintptr_t attackDataPtr = GetAttackDataPtr(playerPtr, moveID);
    if (!attackDataPtr)
        return false;
    
    // Read current frame from player data (offset would need to be determined)
    int currentFrame = 0;
    if (!SafeReadMemory(playerPtr + 0x44, &currentFrame, sizeof(int))) {
        return false;
    }
    
    // Read active frames remaining
    uint16_t activeRemain = 0;
    if (!SafeReadMemory(attackDataPtr + ATTACK_ACTIVE_REMAINING, &activeRemain, sizeof(uint16_t))) {
        return false;
    }
    
    return activeRemain > 0;
}

uintptr_t AttackReader::GetAttackDataPtr(int playerPtr, short /*moveID*/) {
    // Prefer hook-cached pointer, then discovered offset field, then fallbacks
    uintptr_t p1 = GetPlayerPointer(1);
    uintptr_t p2 = GetPlayerPointer(2);
    int playerNum = (playerPtr == p1) ? 1 : ((playerPtr == p2) ? 2 : 0);

    if (playerNum != 0) {
        std::atomic<int>& offCache = (playerNum == 1) ? g_fdToAtkOffsetP1 : g_fdToAtkOffsetP2;

        // First, try cached frame-data pointer
        uintptr_t frameData = GetCachedAttackDataForPlayer(playerNum);
    if (frameData) {
            int off = offCache.load();
            if (off >= 0) {
                uintptr_t atk = 0;
                if (SafeReadMemory(frameData + off, &atk, sizeof(atk)) && IsPlausibleAttackDataPtr(atk)) return atk;
            } else {
                uintptr_t atk = 0; int foundOff = -1;
                if (TryFindNestedAttackPtr(frameData, foundOff, atk)) {
                    offCache.store(foundOff);
                    LogOut("[ATTACK_READER] Discovered fd->atk offset P" + std::to_string(playerNum) + ": +0x" + ToHexString((uint32_t)foundOff), true);
                    return atk;
                }
        // fallback heuristic: +0x38
        if (SafeReadMemory(frameData + 0x38, &atk, sizeof(atk)) && IsPlausibleAttackDataPtr(atk)) return atk;
            }
        }

        // Next, try discovered player offset to frame-data
        int playerFdOff = GetAttackDataOffsetForPlayer(playerNum);
        if (playerFdOff >= 0) {
            uintptr_t fd = 0;
        if (SafeReadMemory(playerPtr + playerFdOff, &fd, sizeof(fd)) && fd) {
                int off = offCache.load();
                if (off >= 0) {
                    uintptr_t atk = 0;
                    if (SafeReadMemory(fd + off, &atk, sizeof(atk)) && IsPlausibleAttackDataPtr(atk)) return atk;
                } else {
                    uintptr_t atk = 0; int foundOff = -1;
                    if (TryFindNestedAttackPtr(fd, foundOff, atk)) {
                        offCache.store(foundOff);
                        LogOut("[ATTACK_READER] Discovered fd->atk offset P" + std::to_string(playerNum) + ": +0x" + ToHexString((uint32_t)foundOff), true);
                        return atk;
                    }
            // fallback heuristic: +0x38
            if (SafeReadMemory(fd + 0x38, &atk, sizeof(atk)) && IsPlausibleAttackDataPtr(atk)) return atk;
                }
            }
        }
    }

    // Legacy fallbacks (best-effort)
    uintptr_t currentAttackData = 0;
    // Legacy: treat these as frame-data fields and scan
    if (SafeReadMemory(playerPtr + 0x168, &currentAttackData, sizeof(uintptr_t)) && currentAttackData) {
        uintptr_t atk = 0; int foundOff = -1;
        if (TryFindNestedAttackPtr(currentAttackData, foundOff, atk)) return atk;
    }
    if (SafeReadMemory(playerPtr + 0x234, &currentAttackData, sizeof(uintptr_t)) && currentAttackData) {
        uintptr_t atk = 0; int foundOff = -1;
        if (TryFindNestedAttackPtr(currentAttackData, foundOff, atk)) return atk;
    }
    if (SafeReadMemory(playerPtr + 0x364, &currentAttackData, sizeof(uintptr_t)) && currentAttackData) {
        uintptr_t atk = 0; int foundOff = -1;
        if (TryFindNestedAttackPtr(currentAttackData, foundOff, atk)) return atk;
    }
    LogOut("[ATTACK_READER] Could not find attack data pointer", true);
    return 0;
}

bool AttackReader::CanPlayerBlock(int playerPtr, int attackerPtr, uintptr_t attackDataPtr) {
    // Check if player is in blockable state
    int guardState = 0;
    short guardGauge = 0;
    short specialState = 0;
    uint8_t attackFlags = 0;
    
    SafeReadMemory(playerPtr + 368, &guardState, sizeof(int));
    SafeReadMemory(playerPtr + 334, &guardGauge, sizeof(short));
    SafeReadMemory(playerPtr + 314, &specialState, sizeof(short));
    SafeReadMemory(attackDataPtr + ATTACK_FLAGS_OFFSET, &attackFlags, sizeof(uint8_t));
    
    // Early return conditions - can't block if:
    if (!guardState ||                  // Not in guard state
        guardGauge < 30 ||              // Guard gauge too low
        specialState ||                 // In special state
        (attackFlags & 0x80) ||         // Unblockable attack
        (attackFlags & 0x40)) {         // Another unblockable flag
        return false;
    }
    
    // Check if stance is correct for the attack type
    uint8_t stance = 0;
    SafeReadMemory(playerPtr + 393, &stance, sizeof(uint8_t));
    bool isCrouching = (stance == 1);
    
    // Determine if aerial based on Y position
    double yPos = 0.0;
    SafeReadMemory(playerPtr + 40, &yPos, sizeof(double));
    bool isAerial = (yPos < 0.0);
    
    // Check attack type vs stance
    if (attackFlags & 0x01) {          // Low attack
        return isCrouching;            // Must be crouching to block
    }
    else if (attackFlags & 0x04) {     // High attack
        return !isCrouching || isAerial; // Must be standing or in air
    }
    
    // Mid attacks can be blocked either way
    return true;
}