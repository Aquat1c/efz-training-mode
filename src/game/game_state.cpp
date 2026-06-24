#include "../include/game/game_state.h"

#include "../include/core/memory.h"
#include "../include/core/constants.h"
#include "../include/utils/utilities.h"
#include "../include/utils/network.h"
#include "../include/utils/pause_integration.h"
#include "../include/utils/switch_players.h"
#include "../include/utils/bgm_control.h"
#include "../include/gui/gui.h"
#include "../include/gui/imgui_impl.h"
#include "frame_monitor.h"
#include "../include/core/logger.h"
#include "../3rdparty/minhook/include/MinHook.h"
#include <windows.h>
#include <atomic>
#include <sstream>
#include <iomanip>

static std::atomic<GamePhase> g_phaseCache{ GamePhase::Unknown };
static std::atomic<int> g_phaseStableFrames{0};
static std::atomic<uint8_t> g_lastRawScreenState{ 255 };

namespace {
    constexpr uintptr_t RVA_GAME_MODE_ARRAY = 0x00390110;
    constexpr uintptr_t RVA_BATTLE_UPDATE = 0x00363C20;      // efz.exe 0x00763C20
    constexpr uintptr_t RVA_BATTLE_HOTKEYS = 0x00365660;     // efz.exe 0x00765660
    constexpr uint8_t SCREEN_TITLE = 0;
    constexpr uint8_t SCREEN_CHARACTER_SELECT = 1;
    constexpr uint8_t SCREEN_BATTLE = 3;
    constexpr uintptr_t SCREEN_EXIT_FLAG_OFFSET = 45;
    constexpr uintptr_t BATTLE_ENGINE_PAUSE_OFFSET = 1416;

    enum class PendingExitOverride : int {
        None = 0,
        Title = 1,
    };

    using BattleUpdateFn = char (__thiscall *)(void* battleContext);
    using BattleHotkeysFn = int (__thiscall *)(void* battleContext);

    BattleUpdateFn oBattleUpdate = nullptr;
    BattleHotkeysFn oBattleHotkeys = nullptr;
    std::atomic<bool> s_frontendHooksInstalled{false};
    std::atomic<bool> s_frontendHooksAttempted{false};
    std::atomic<BattleUpdateCallback> s_beforeBattleUpdate{nullptr};
    std::atomic<BattleUpdateCallback> s_afterBattleUpdate{nullptr};
    std::atomic<int> s_pendingExitOverride{static_cast<int>(PendingExitOverride::None)};
    std::atomic<int> s_pendingExitOverrideFrames{0};
    std::atomic<bool> s_practiceEscHeld{false};

    uint8_t ReadRawScreenStateNoDebounce() {
        uintptr_t base = GetEFZBase();
        uint8_t v = 255;
        if (base) {
            SafeReadMemory(base + EFZ_BASE_OFFSET_SCREEN_STATE, &v, sizeof(v));
        }
        return v;
    }

    bool ResolveScreenContext(uint8_t screenIndex, uintptr_t& outContext) {
        outContext = 0;
        uintptr_t base = GetEFZBase();
        if (!base) return false;

        uintptr_t slot = base + RVA_GAME_MODE_ARRAY + 4u * static_cast<uintptr_t>(screenIndex);
        return SafeReadMemory(slot, &outContext, sizeof(outContext)) && outContext != 0;
    }

    bool RequestScreenExitFlag(uint8_t screenIndex) {
        uintptr_t context = 0;
        if (!ResolveScreenContext(screenIndex, context)) {
            return false;
        }

        uint8_t exitRequested = 1;
        bool ok = SafeWriteMemory(context + SCREEN_EXIT_FLAG_OFFSET, &exitRequested, sizeof(exitRequested));

        if (ok && screenIndex == SCREEN_BATTLE) {
            uint32_t unpaused = 0;
            SafeWriteMemory(context + BATTLE_ENGINE_PAUSE_OFFSET, &unpaused, sizeof(unpaused));
        }

        return ok;
    }

    void CloseTrainingMenuForFrontendExit() {
        if (ImGuiImpl::IsVisible()) {
            ImGuiImpl::ToggleVisibility();
        }
        menuOpen.store(false);
    }

    void ResetModSessionForFrontendExit(const char* reason) {
        SwitchPlayers::ResetControlMappingForMenusToP1();
        PauseIntegration::SetPracticePausedForFramestep(false);
        ResetPracticeMatchSessionState(reason);
    }

    bool IsPracticeBattleHotkeyContext() {
        if (ReadRawScreenStateNoDebounce() != SCREEN_BATTLE) {
            return false;
        }
        if (GetCurrentGameMode() != GameMode::Practice) {
            return false;
        }
        if (IsNetplaySuspendActive() || IsNetplaySessionActive()) {
            return false;
        }
        return true;
    }

    bool PollEscapeIfGameActive(bool& outEscDown) {
        outEscDown = false;
        UpdateWindowActiveState();
        if (!g_efzWindowActive.load(std::memory_order_relaxed)) {
            return false;
        }
        outEscDown = (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
        return true;
    }

    char __fastcall HookedBattleUpdate(void* battleContext, void* /*edx*/) {
        if (auto callback = s_beforeBattleUpdate.load(std::memory_order_relaxed)) {
            callback(battleContext);
        }

        const char result = oBattleUpdate ? oBattleUpdate(battleContext) : SCREEN_BATTLE;

        if (auto callback = s_afterBattleUpdate.load(std::memory_order_relaxed)) {
            callback(battleContext);
        }

        if (s_pendingExitOverride.load(std::memory_order_acquire) != static_cast<int>(PendingExitOverride::Title)) {
            return result;
        }

        if (result == SCREEN_BATTLE) {
            int framesLeft = s_pendingExitOverrideFrames.load(std::memory_order_relaxed);
            if (framesLeft > 0) {
                s_pendingExitOverrideFrames.store(framesLeft - 1, std::memory_order_relaxed);
            } else {
                s_pendingExitOverride.store(static_cast<int>(PendingExitOverride::None), std::memory_order_release);
            }
            return result;
        }

        s_pendingExitOverride.store(static_cast<int>(PendingExitOverride::None), std::memory_order_release);
        s_pendingExitOverrideFrames.store(0, std::memory_order_relaxed);

        if (result == SCREEN_CHARACTER_SELECT) {
            LogOut("[FRONTEND] Battle cleanup completed; overriding next screen to Title", true);
            // Vanilla silences the OST when returning to the title screen by
            // switching to BGM slot 150. This Battle->Title override skips the
            // character-select screen where that normally happens, so without
            // this the match/stage track (e.g. one set by a hotswap) keeps
            // playing on the title. Mirror vanilla explicitly.
            if (const uintptr_t gameStatePtr = GetGameStatePtr()) {
                PlayBGM(gameStatePtr, 150);
            }
            return SCREEN_TITLE;
        }

        return result;
    }

    int __fastcall HookedBattleHotkeys(void* battleContext, void* /*edx*/) {
        const bool practiceBattle = IsPracticeBattleHotkeyContext();
        bool escDown = false;
        const bool gameActive = practiceBattle && PollEscapeIfGameActive(escDown);

        if (ImGuiImpl::IsVisible()) {
            if (practiceBattle) {
                s_practiceEscHeld.store(gameActive && escDown, std::memory_order_relaxed);
            } else {
                s_practiceEscHeld.store(false, std::memory_order_relaxed);
            }
            return 0;
        }

        if (!practiceBattle) {
            s_practiceEscHeld.store(false, std::memory_order_relaxed);
            return oBattleHotkeys ? oBattleHotkeys(battleContext) : 0;
        }

        if (!gameActive) {
            s_practiceEscHeld.store(false, std::memory_order_relaxed);
            return 0;
        }

        if (escDown) {
            const bool wasHeld = s_practiceEscHeld.exchange(true, std::memory_order_acq_rel);
            if (!wasHeld) {
                LogOut("[FRONTEND] Practice ESC intercepted; opening training menu", true);
                OpenMenu();
            }
            return 0;
        }

        s_practiceEscHeld.store(false, std::memory_order_relaxed);
        return oBattleHotkeys ? oBattleHotkeys(battleContext) : 0;
    }
}

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
        // Do not log anything about mode changes after we've entered online mode
        extern std::atomic<bool> g_onlineModeActive;
        if (g_onlineModeActive.load()) {
            prevRawValue = rawValue; // advance to avoid repeated comparisons
            if (rawValueOut) { *rawValueOut = rawValue; }
            return currentMode;
        }
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

// Replace screen/phase with direct screen-state byte
static uint8_t ReadRawScreenState() {
    uintptr_t base = GetEFZBase();
    uint8_t v = 255;
    if (!base) return v;
    // Direct byte read at efz.exe+0x390148
    SafeReadMemory(base + EFZ_BASE_OFFSET_SCREEN_STATE, &v, sizeof(v));
    return v;
}

// Character Select quick check using the raw byte
bool IsInCharacterSelectScreen() {
    uint8_t st = ReadRawScreenState();
    return st == 1; // 1 = Character Select
}

// Gameplay state: rely on raw byte 3 (in-game) conservatively
bool IsInGameplayState() {
    uint8_t st = ReadRawScreenState();
    return st == 3; // 3 = In-game
}

bool EnsureFrontendControlHooksInstalled() {
    if (s_frontendHooksInstalled.load(std::memory_order_acquire)) {
        return true;
    }

    uintptr_t base = GetEFZBase();
    if (!base) {
        return false;
    }

    bool anyFailed = false;

    LPVOID battleUpdateTarget = reinterpret_cast<LPVOID>(base + RVA_BATTLE_UPDATE);
    MH_STATUS updateStatus = MH_CreateHook(
        battleUpdateTarget,
        reinterpret_cast<LPVOID>(&HookedBattleUpdate),
        reinterpret_cast<void**>(&oBattleUpdate));
    if (updateStatus == MH_OK || updateStatus == MH_ERROR_ALREADY_CREATED) {
        MH_STATUS enableStatus = MH_EnableHook(battleUpdateTarget);
        if (enableStatus != MH_OK && enableStatus != MH_ERROR_ENABLED) {
            anyFailed = true;
            LogOut("[FRONTEND] Failed to enable battle update hook", true);
        }
    } else {
        anyFailed = true;
        LogOut("[FRONTEND] Failed to create battle update hook", true);
    }

    LPVOID battleHotkeysTarget = reinterpret_cast<LPVOID>(base + RVA_BATTLE_HOTKEYS);
    MH_STATUS hotkeyStatus = MH_CreateHook(
        battleHotkeysTarget,
        reinterpret_cast<LPVOID>(&HookedBattleHotkeys),
        reinterpret_cast<void**>(&oBattleHotkeys));
    if (hotkeyStatus == MH_OK || hotkeyStatus == MH_ERROR_ALREADY_CREATED) {
        MH_STATUS enableStatus = MH_EnableHook(battleHotkeysTarget);
        if (enableStatus != MH_OK && enableStatus != MH_ERROR_ENABLED) {
            anyFailed = true;
            LogOut("[FRONTEND] Failed to enable battle hotkey gate", true);
        }
    } else {
        anyFailed = true;
        LogOut("[FRONTEND] Failed to create battle hotkey gate", true);
    }

    if (!anyFailed) {
        s_frontendHooksInstalled.store(true, std::memory_order_release);
        LogOut("[FRONTEND] Control hooks installed", true);
        return true;
    }

    const bool alreadyAttempted = s_frontendHooksAttempted.exchange(true);
    if (!alreadyAttempted) {
        LogOut("[FRONTEND] Control hooks unavailable; menu ESC suppression and title override disabled", true);
    }
    return false;
}

void SetBattleUpdateCallbacks(BattleUpdateCallback beforeUpdate, BattleUpdateCallback afterUpdate) {
    s_beforeBattleUpdate.store(beforeUpdate, std::memory_order_relaxed);
    s_afterBattleUpdate.store(afterUpdate, std::memory_order_relaxed);
}

bool CanRequestFrontendExit(FrontendExitTarget target) {
    if (IsNetplaySuspendActive() || IsNetplaySessionActive()) {
        return false;
    }

    const uint8_t screen = ReadRawScreenStateNoDebounce();
    switch (target) {
    case FrontendExitTarget::CharacterSelect:
        return screen == SCREEN_BATTLE;
    case FrontendExitTarget::Title:
        return screen == SCREEN_BATTLE || screen == SCREEN_CHARACTER_SELECT || screen == SCREEN_TITLE;
    default:
        return false;
    }
}

bool RequestFrontendExit(FrontendExitTarget target) {
    if (!CanRequestFrontendExit(target)) {
        LogOut("[FRONTEND] Exit request ignored in current screen/netplay state", true);
        return false;
    }

    const bool hooksReady = EnsureFrontendControlHooksInstalled();

    const uint8_t screen = ReadRawScreenStateNoDebounce();
    if (target == FrontendExitTarget::Title && screen == SCREEN_TITLE) {
        CloseTrainingMenuForFrontendExit();
        return true;
    }

    if (screen == SCREEN_BATTLE) {
        if (target == FrontendExitTarget::Title) {
            if (!hooksReady) {
                LogOut("[FRONTEND] Exit to Title unavailable because the battle update hook is inactive", true);
                return false;
            }
            s_pendingExitOverride.store(static_cast<int>(PendingExitOverride::Title), std::memory_order_release);
            s_pendingExitOverrideFrames.store(120, std::memory_order_relaxed);
        } else {
            s_pendingExitOverride.store(static_cast<int>(PendingExitOverride::None), std::memory_order_release);
            s_pendingExitOverrideFrames.store(0, std::memory_order_relaxed);
        }

        if (!RequestScreenExitFlag(SCREEN_BATTLE)) {
            s_pendingExitOverride.store(static_cast<int>(PendingExitOverride::None), std::memory_order_release);
            s_pendingExitOverrideFrames.store(0, std::memory_order_relaxed);
            LogOut("[FRONTEND] Failed to request battle cleanup", true);
            return false;
        }

        ResetModSessionForFrontendExit(target == FrontendExitTarget::Title
            ? "MenuExitToTitle"
            : "MenuExitToCharacterSelect");
        CloseTrainingMenuForFrontendExit();

        LogOut(target == FrontendExitTarget::Title
            ? "[FRONTEND] Requested exit to Title through battle cleanup"
            : "[FRONTEND] Requested exit to Character Select through battle cleanup",
            true);
        return true;
    }

    if (screen == SCREEN_CHARACTER_SELECT && target == FrontendExitTarget::Title) {
        if (!RequestScreenExitFlag(SCREEN_CHARACTER_SELECT)) {
            LogOut("[FRONTEND] Failed to request Character Select exit", true);
            return false;
        }
        CloseTrainingMenuForFrontendExit();
        LogOut("[FRONTEND] Requested exit to Title through Character Select cleanup", true);
        return true;
    }

    return false;
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
    uint8_t rawScr = ReadRawScreenState();
    LogOut("[SCREEN_STATE] Mode=" + GetGameModeName(GetCurrentGameMode()) +
           " ScreenByte=" + std::to_string(rawScr) +
           " CharSelect=" + std::to_string(IsInCharacterSelectScreen()), true);
    LogOut("[SCREEN_STATE] ------------------------------------------", true);
}



GamePhase GetCurrentGamePhase() {
    // Map efz.exe+0x390148 directly to our phases with minimal debounce
    uint8_t st = ReadRawScreenState();
    g_lastRawScreenState.store(st);
    GamePhase derived = GamePhase::Unknown;

    switch (st) {
        case 0: // Title
        case 6: // Settings
        case 8: // Replay selection
        case 5: // Win screen (disable features; not relevant for Practice)
            derived = GamePhase::Menu;
            break;
        case 1: // Character Select
            derived = GamePhase::CharacterSelect;
            break;
        case 2: // Loading
            derived = GamePhase::Loading;
            break;
        case 3: // In-game
            derived = GamePhase::Match;
            break;
        default:
            derived = GamePhase::Unknown;
            break;
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
