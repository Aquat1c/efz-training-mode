#include "../include/game/game_state.h"

#include "../include/core/memory.h"
#include "../include/core/constants.h"
#include "../include/utils/utilities.h"
#include "frame_monitor.h"
#include "../include/core/logger.h"
#include <sstream>
#include <iomanip>

static std::atomic<GamePhase> g_phaseCache{ GamePhase::Unknown };
static std::atomic<int> g_phaseStableFrames{0};

// REVISED: Now takes an optional out parameter.
GameMode GetCurrentGameMode(uint8_t* rawValueOut) {
    // Static variables to track the previous raw value and guarantee the first read is logged.
    static uint8_t prevRawValue = 99; // Initialize to a value that cannot be the actual first value.

    GameMode currentMode = GameMode::Unknown;
    uint8_t rawValue = 255; // Default to unknown

    uintptr_t efzBase = GetEFZBase();
    uintptr_t gameModeAddr = 0;
    uintptr_t gameStructPtr = 0;

    if (efzBase) {
        // --- MANUAL POINTER RESOLUTION ---
        // Step 1: Get the address of the base pointer.
        uintptr_t basePtrAddr = efzBase + EFZ_BASE_OFFSET_GAME_STATE;
        
        // Step 2: Read the value of the base pointer.
        if (SafeReadMemory(basePtrAddr, &gameStructPtr, sizeof(uintptr_t))) {
            // Step 3: Add the final offset to get the game mode address.
            gameModeAddr = gameStructPtr + GAME_MODE_OFFSET;
            
            // Step 4: Read the final byte value.
            SafeReadMemory(gameModeAddr, &rawValue, sizeof(uint8_t));
        }
    }
    
    // Always convert the raw value to its corresponding enum.
    switch (rawValue) {
        case 0: currentMode = GameMode::Arcade; break;
        case 1: currentMode = GameMode::Practice; break;
        case 3: currentMode = GameMode::VsCpu; break;
        case 4: currentMode = GameMode::VsHuman; break;
        case 5: currentMode = GameMode::Replay; break;
        case 6: currentMode = GameMode::AutoReplay; break;
        default: currentMode = GameMode::Unknown; break;
    }

    // If the raw byte value has changed, log it to the console.
    if (rawValue != prevRawValue) {
        // --- DETAILED DEBUG LOGGING FOR POINTER RESOLUTION ---
        std::ostringstream oss;
        oss << std::hex << std::uppercase;
        LogOut("", true); // Add a blank line for readability
        LogOut("[GAME_STATE_DBG] --- Game Mode Pointer Trace ---", true);
        oss << "[GAME_STATE_DBG] efz.exe Base Address: 0x" << efzBase;
        LogOut(oss.str(), true); oss.str(""); oss.clear();
        
        oss << "[GAME_STATE_DBG] Reading pointer at [efz.exe + 0x" << EFZ_BASE_OFFSET_GAME_STATE << "] = 0x" << (efzBase + EFZ_BASE_OFFSET_GAME_STATE);
        LogOut(oss.str(), true); oss.str(""); oss.clear();
        
        oss << "[GAME_STATE_DBG]   -> Read value (as 4-byte pointer): 0x" << gameStructPtr;
        LogOut(oss.str(), true); oss.str(""); oss.clear();

        oss << "[GAME_STATE_DBG] Reading byte at [0x" << gameStructPtr << " + 0x" << GAME_MODE_OFFSET << "] = 0x" << gameModeAddr;
        LogOut(oss.str(), true); oss.str(""); oss.clear();

        oss << "[GAME_STATE_DBG]   -> Read value (as 1-byte): " << std::dec << (int)rawValue;
        LogOut(oss.str(), true); oss.str(""); oss.clear();
        LogOut("[GAME_STATE_DBG] ---------------------------------", true);
        // --- END DEBUG LOGGING ---

        std::string modeName = GetGameModeName(currentMode);
        LogOut("[GAME STATE] Game mode changed to: " + modeName + " (Value: " + std::to_string(rawValue) + ")", true);
        prevRawValue = rawValue;
    }

    // Pass the raw value back to the caller if requested.
    if (rawValueOut) {
        *rawValueOut = rawValue;
    }

    return currentMode;
}

std::string GetGameModeName(GameMode mode) {
    switch (mode) {
        case GameMode::Arcade:      return "Arcade";
        case GameMode::Practice:    return "Practice";
        case GameMode::VsCpu:       return "VS CPU";
        case GameMode::VsHuman:     return "VS Human";
        case GameMode::Replay:      return "Replay";
        case GameMode::AutoReplay:  return "Auto-Replay";
        default:                    return "Unknown";
    }
}

// Rolling counters for heuristic
static std::atomic<int> g_spawnedConfirmFrames{0};
static std::atomic<int> g_unspawnedConfirmFrames{0};

// Helper: try read an int safely (returns false if addr invalid)
static bool TryReadInt(uintptr_t addr, int &out) {
    if (!addr) return false;
    return SafeReadMemory(addr, &out, sizeof(int));
}

// Helper: read double (position)
static bool TryReadDouble(uintptr_t addr, double &out) {
    if (!addr) return false;
    return SafeReadMemory(addr, &out, sizeof(double));
}

// Determine if player structs look “spawned”
static bool IsPlayerStructSpawned(int playerNum) {
    uintptr_t base = GetEFZBase();
    if (!base) return false;
    uintptr_t playerOffset = (playerNum == 1) ? EFZ_BASE_OFFSET_P1 : EFZ_BASE_OFFSET_P2;

    uintptr_t pPtr = 0;
    if (!SafeReadMemory(base + playerOffset, &pPtr, sizeof(uintptr_t)) || !pPtr)
        return false;

    // HP
    int hp = 0;
    uintptr_t hpAddr = ResolvePointer(base, playerOffset, HP_OFFSET);
    if (!hpAddr || !SafeReadMemory(hpAddr, &hp, sizeof(int)))
        return false;

    // MoveID (often 0 initially but address must be valid)
    short moveID = 0;
    uintptr_t moveAddr = ResolvePointer(base, playerOffset, MOVE_ID_OFFSET);
    if (!moveAddr || !SafeReadMemory(moveAddr, &moveID, sizeof(short)))
        return false;

    // X position (should become non‑zero / change from strict 0 after spawn)
    double xPos = 0.0;
    uintptr_t xAddr = ResolvePointer(base, playerOffset, XPOS_OFFSET);
    if (!xAddr || !TryReadDouble(xAddr, xPos))
        return false;

    // Heuristic acceptance:
    //   HP > 0  OR (HP == 0 but moveID != 0) (some intro moves may start with 0 HP very briefly)
    //   AND xPos not exactly 0.0 for several frames (we tolerate 0 first frames)
    return (hp > 0 || moveID != 0) && (fabs(xPos) > 0.01);
}

bool AreCharactersSpawned() {
    bool p1 = IsPlayerStructSpawned(1);
    bool p2 = IsPlayerStructSpawned(2);

    if (p1 && p2) {
        int c = g_spawnedConfirmFrames.fetch_add(1) + 1;
        g_unspawnedConfirmFrames.store(0);
        return c >= 2; // require 2 consecutive confirmations to stabilize
    } else {
        g_unspawnedConfirmFrames.fetch_add(1);
        g_spawnedConfirmFrames.store(0);
        return false;
    }
}

// New: robust character select detection
bool IsInCharacterSelectScreen() {
    GameMode m = GetCurrentGameMode();
    // Only care for modes that have a pre‑match selection (Arcade / VsCpu / VsHuman)
    if (!(m == GameMode::Arcade || m == GameMode::VsCpu || m == GameMode::VsHuman))
        return false;

    // Not spawned for several frames ⇒ treat as character select / loading
    if (!AreCharactersSpawned()) {
        if (g_unspawnedConfirmFrames.load() >= 4)  // small debounce
            return true;
    }
    return false;
}

// Improved gameplay state
bool IsInGameplayState() {
    GameMode mode = GetCurrentGameMode();
    if (!(mode == GameMode::Practice || mode == GameMode::Arcade ||
          mode == GameMode::VsCpu    || mode == GameMode::VsHuman))
        return false;

    if (IsInCharacterSelectScreen())
        return false;

    return AreCharactersSpawned();
}

// Enhanced debug dump: log first 0x40 bytes (byte granularity)
void DebugDumpScreenState() {
    uintptr_t base = GetEFZBase();
    if (!base) {
        LogOut("[SCREEN_STATE] Base missing", true);
        return;
    }
    uintptr_t gameStatePtr = 0;
    if (!SafeReadMemory(base + EFZ_BASE_OFFSET_GAME_STATE, &gameStatePtr, sizeof(uintptr_t)) || !gameStatePtr) {
        LogOut("[SCREEN_STATE] Game state ptr invalid", true);
        return;
    }

    LogOut("[SCREEN_STATE] ---- Game State Raw (first 0x40 bytes) ----", true);
    uint8_t buffer[0x40]{};
    SafeReadMemory(gameStatePtr, buffer, sizeof(buffer));
    std::string line;
    for (int i = 0; i < 0x40; i++) {
        char b[8];
        sprintf_s(b, "%02X ", buffer[i]);
        line += b;
        if ((i & 0x0F) == 0x0F) {
            LogOut("[SCREEN_STATE] " + line, true);
            line.clear();
        }
    }
    if (!line.empty())
        LogOut("[SCREEN_STATE] " + line, true);

    // Summaries
    LogOut("[SCREEN_STATE] Mode=" + GetGameModeName(GetCurrentGameMode()) +
           " Spawned=" + std::to_string(AreCharactersSpawned()) +
           " CharSelect=" + std::to_string(IsInCharacterSelectScreen()), true);
    LogOut("[SCREEN_STATE] ------------------------------------------", true);
}



// Improve spawn heuristic: require both HP > 0 AND move addr readable AND index buffer not all 0 for N frames
static bool PlayersLikelySpawned() {
    if (!AreCharactersSpawned()) return false;
    // Extra guard: read a known runtime-changing value (e.g. MoveID P1)
    uintptr_t base = GetEFZBase();
    if (!base) return false;
    short mv = 0;
    auto mvAddr = ResolvePointer(base, EFZ_BASE_OFFSET_P1, MOVE_ID_OFFSET);
    if (!mvAddr || !SafeReadMemory(mvAddr, &mv, sizeof(mv))) return false;
    return true;
}

GamePhase GetCurrentGamePhase() {
    // Determine raw mode first
    GameMode mode = GetCurrentGameMode();
    bool spawned = PlayersLikelySpawned();

    GamePhase derived = GamePhase::Unknown;

    // 1. If no known gameplay modes -> menu-ish
    if (!(mode == GameMode::Arcade || mode == GameMode::Practice ||
          mode == GameMode::VsCpu  || mode == GameMode::VsHuman ||
          mode == GameMode::Replay || mode == GameMode::AutoReplay)) {
        derived = GamePhase::Menu;
    } else {
        // We are in a gameplay-capable mode
        if (!spawned) {
            // Distinguish CharacterSelect vs Loading: use unspawned duration
            int unspawnFrames = g_unspawnedConfirmFrames.load();
            if (unspawnFrames > 30)       derived = GamePhase::CharacterSelect; // stayed unspawned longer
            else                          derived = GamePhase::Loading;
        } else {
            derived = GamePhase::Match;
        }
    }

    // Debounce: require 3 consecutive frames before committing
    static GamePhase lastRaw = GamePhase::Unknown;
    if (derived == lastRaw) {
        int s = g_phaseStableFrames.fetch_add(1) + 1;
        if (s >= 3) g_phaseCache.store(derived);
    } else {
        g_phaseStableFrames.store(0);
        lastRaw = derived;
    }

    return g_phaseCache.load();
}

// OPTIONAL: expose a small debug hook
void LogPhaseIfChanged() {
    static GamePhase prev = GamePhase::Unknown;
    GamePhase now = g_phaseCache.load();
    if (now != prev) {
        LogOut("[GAME_PHASE] " + std::to_string((int)prev) + " -> " + std::to_string((int)now), true);
        prev = now;
    }
}