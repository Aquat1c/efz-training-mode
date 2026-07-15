#include "../include/game/game_state.h"
#include "../include/game/mission/mission_engine.h"

#include "../include/core/memory.h"
#include "../include/core/constants.h"
#include "../include/utils/utilities.h"
#include "../include/utils/network.h"
#include "../include/utils/pause_integration.h"
#include "../include/utils/switch_players.h"
#include "../include/utils/bgm_control.h"
#include "../include/gui/gui.h"
#include "../include/gui/imgui_impl.h"
#include "../include/gui/custom_menu/screens.h"
#include "frame_monitor.h"
#include "../include/core/logger.h"
#include "../3rdparty/minhook/include/MinHook.h"
#include <windows.h>
#include <atomic>
#include <sstream>
#include <iomanip>
#include <cstring>
#include <mutex>

static std::atomic<GamePhase> g_phaseCache{ GamePhase::Unknown };
static std::atomic<int> g_phaseStableFrames{0};
static std::atomic<uint8_t> g_lastRawScreenState{ 255 };

namespace {
    constexpr uintptr_t RVA_GAME_MODE_ARRAY = 0x00390110;
    constexpr uintptr_t RVA_BATTLE_UPDATE = 0x00363C20;      // efz.exe 0x00763C20
    constexpr uintptr_t RVA_BATTLE_HOTKEYS = 0x00365660;     // efz.exe 0x00765660
    constexpr uint8_t SIG_BATTLE_UPDATE[] =
        {0x55,0x8B,0xEC,0x83,0xEC,0x40,0x89,0x4D,0xD0,0xFF,0x15,0xE0};
    constexpr uint8_t SIG_BATTLE_HOTKEYS[] =
        {0x55,0x8B,0xEC,0x81,0xEC,0x10,0x01,0x00,0x00,0xA1,0x50,0xF0};
    constexpr uint8_t SCREEN_TITLE = 0;
    constexpr uint8_t SCREEN_CHARACTER_SELECT = 1;
    constexpr uint8_t SCREEN_LOADING = 2;
    constexpr uint8_t SCREEN_BATTLE = 3;
    constexpr uintptr_t SCREEN_EXIT_FLAG_OFFSET = 45;
    constexpr uintptr_t BATTLE_ENGINE_PAUSE_OFFSET = 1416;

    enum class PendingExitOverride : int {
        None = 0,
        Title = 1,
        Loading = 2,
    };

    using BattleUpdateFn = char (__thiscall *)(void* battleContext);
    using BattleHotkeysFn = int (__thiscall *)(void* battleContext);

    BattleUpdateFn oBattleUpdate = nullptr;
    BattleHotkeysFn oBattleHotkeys = nullptr;
    std::atomic<bool> s_frontendHooksInstalled{false};
    std::atomic<bool> s_frontendHooksAttempted{false};
    std::atomic<bool> s_frontendHooksPartial{false};
    std::mutex s_frontendHookMutex;
    std::atomic<BattleUpdateCallback> s_beforeBattleUpdate{nullptr};
    std::atomic<BattleUpdateCallback> s_afterBattleUpdate{nullptr};
    std::atomic<uint32_t> s_currentBattleBatch{0};
    std::atomic<uint32_t> s_completedBattleBatch{0};
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
        const uint32_t batch = s_currentBattleBatch.fetch_add(
            1, std::memory_order_acq_rel) + 1;
        if (auto callback = s_beforeBattleUpdate.load(std::memory_order_relaxed)) {
            callback(battleContext);
        }

        const char result = oBattleUpdate ? oBattleUpdate(battleContext) : SCREEN_BATTLE;

        if (auto callback = s_afterBattleUpdate.load(std::memory_order_relaxed)) {
            callback(battleContext);
        }
        s_completedBattleBatch.store(batch, std::memory_order_release);

        const PendingExitOverride exitOverride = static_cast<PendingExitOverride>(
            s_pendingExitOverride.load(std::memory_order_acquire));
        if (exitOverride == PendingExitOverride::None) {
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
            if (exitOverride == PendingExitOverride::Loading) {
                LogOut("[FRONTEND] Battle cleanup completed; overriding next screen to Loading", true);
                return SCREEN_LOADING;
            }

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

        if (practiceBattle && Mission::Engine::Demo::IsActive()) {
            const bool wasHeld = s_practiceEscHeld.exchange(gameActive && escDown,
                                                             std::memory_order_acq_rel);
            if (gameActive && escDown && !wasHeld) {
                LogOut("[MISSION][DEMO] Practice ESC intercepted; canceling demonstration", true);
                Mission::Engine::Demo::Cancel();
            }
            // The native battle dispatcher owns pause/menu and several battle
            // shortcuts; none may run during an exclusive demonstration.
            return 0;
        }

        if (practiceBattle && Mission::Engine::Recorder::OwnsCaptureHotkeys()) {
            const bool wasHeld = s_practiceEscHeld.exchange(gameActive && escDown,
                                                             std::memory_order_acq_rel);
            if (gameActive && escDown && !wasHeld) {
                // The capture phases own their own pause surface (the gate in
                // OpenMenu routes there). The take is NOT auto-sealed anymore:
                // the pause menu offers Resume / Cancel Countdown / Stop &
                // Review / Discard explicitly.
                OpenMenu();
            }
            return 0;
        }

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
                if (Mission::Engine::Recorder::IsSessionActive()) {
                    CustomMenu::Screens::OpenMissionBrowser();
                }
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
    if (s_frontendHooksPartial.load(std::memory_order_acquire)) return false;

    std::lock_guard<std::mutex> hookLock(s_frontendHookMutex);
    if (s_frontendHooksInstalled.load(std::memory_order_acquire)) return true;
    if (s_frontendHooksPartial.load(std::memory_order_acquire)) return false;

    uintptr_t base = GetEFZBase();
    if (!base) {
        return false;
    }

    LPVOID battleUpdateTarget = reinterpret_cast<LPVOID>(base + RVA_BATTLE_UPDATE);
    LPVOID battleHotkeysTarget = reinterpret_cast<LPVOID>(base + RVA_BATTLE_HOTKEYS);
    auto removeOwned = [](LPVOID target, void** original, const char* label) {
        (void)MH_DisableHook(target);
        const MH_STATUS status = MH_RemoveHook(target);
        if (status == MH_OK || status == MH_ERROR_NOT_CREATED) {
            if (original) *original = nullptr;
            return true;
        }
        LogOut(std::string("[FRONTEND] Failed to remove ") + label +
               "; retaining trampoline for safety", true);
        s_frontendHooksPartial.store(true, std::memory_order_release);
        return false;
    };
    // The signature match is ADVISORY, not a gate. SIG_BATTLE_* only equal the
    // PRISTINE retail prologue ("55 8B EC ..."). Under EfzRevival the battle
    // update/hotkey routines are already JMP-patched by Revival's own frontend
    // hooks by the time we run, so the LIVE prologue is a jump stub ("E9 .." /
    // "FF 25 ..") and never matches the on-disk bytes. That is expected: MinHook
    // chains onto the pre-hooked target fine, exactly like every other efz.exe
    // hook installed here (input @0x411BE0, collision @0x767F60, HUD 6/6).
    // A HARD mismatch-gate here silently disabled the keyboard-ESC menu
    // suppression (the sole way ESC->Character-Select is blocked in
    // Training/Mission/Tutorial), leaving only the gamepad Start path. So refuse
    // only when the target is unreadable; otherwise proceed and let MH_CreateHook
    // be the real gate. The live bytes are logged once for diagnostics.
    uint8_t updateBytes[sizeof(SIG_BATTLE_UPDATE)] = {};
    uint8_t hotkeyBytes[sizeof(SIG_BATTLE_HOTKEYS)] = {};
    const bool readUpdate = SafeReadMemory(reinterpret_cast<uintptr_t>(battleUpdateTarget),
                                           updateBytes, sizeof(updateBytes));
    const bool readHotkey = SafeReadMemory(reinterpret_cast<uintptr_t>(battleHotkeysTarget),
                                           hotkeyBytes, sizeof(hotkeyBytes));
    if (!readUpdate || !readHotkey) {
        const bool alreadyAttempted = s_frontendHooksAttempted.exchange(true);
        if (!alreadyAttempted) {
            LogOut("[FRONTEND] Battle hook targets unreadable; control hooks disabled", true);
        }
        return false;
    }
    const bool sigMatch =
        std::memcmp(updateBytes, SIG_BATTLE_UPDATE, sizeof(updateBytes)) == 0 &&
        std::memcmp(hotkeyBytes, SIG_BATTLE_HOTKEYS, sizeof(hotkeyBytes)) == 0;
    if (!sigMatch) {
        static std::atomic<bool> s_sigMismatchLogged{false};
        if (!s_sigMismatchLogged.exchange(true)) {
            std::ostringstream os;
            os << "[FRONTEND] Battle prologue differs from retail signature "
                  "(expected under Revival pre-hook) - hooking anyway. update=";
            os << std::hex << std::uppercase << std::setfill('0');
            for (int i = 0; i < 5; ++i) os << std::setw(2) << static_cast<int>(updateBytes[i]) << ' ';
            os << "hotkey=";
            for (int i = 0; i < 5; ++i) os << std::setw(2) << static_cast<int>(hotkeyBytes[i]) << ' ';
            LogOut(os.str(), true);
        }
    }

    const MH_STATUS updateStatus = MH_CreateHook(
        battleUpdateTarget,
        reinterpret_cast<LPVOID>(&HookedBattleUpdate),
        reinterpret_cast<void**>(&oBattleUpdate));
    if (updateStatus != MH_OK) {
        LogOut(updateStatus == MH_ERROR_ALREADY_CREATED
            ? "[FRONTEND] Battle update hook already belongs to another owner"
            : "[FRONTEND] Failed to create battle update hook", true);
        oBattleUpdate = nullptr;
        s_frontendHooksAttempted.store(true);
        return false;
    }

    if (MH_EnableHook(battleUpdateTarget) != MH_OK) {
        (void)removeOwned(battleUpdateTarget,
                          reinterpret_cast<void**>(&oBattleUpdate),
                          "battle update hook");
        LogOut("[FRONTEND] Failed to enable battle update hook", true);
        s_frontendHooksAttempted.store(true);
        return false;
    }

    const MH_STATUS hotkeyStatus = MH_CreateHook(
        battleHotkeysTarget,
        reinterpret_cast<LPVOID>(&HookedBattleHotkeys),
        reinterpret_cast<void**>(&oBattleHotkeys));
    if (hotkeyStatus != MH_OK) {
        (void)removeOwned(battleUpdateTarget,
                          reinterpret_cast<void**>(&oBattleUpdate),
                          "battle update hook");
        oBattleHotkeys = nullptr;
        LogOut(hotkeyStatus == MH_ERROR_ALREADY_CREATED
            ? "[FRONTEND] Battle hotkey hook already belongs to another owner"
            : "[FRONTEND] Failed to create battle hotkey gate", true);
        s_frontendHooksAttempted.store(true);
        return false;
    }

    if (MH_EnableHook(battleHotkeysTarget) != MH_OK) {
        (void)removeOwned(battleHotkeysTarget,
                          reinterpret_cast<void**>(&oBattleHotkeys),
                          "battle hotkey hook");
        (void)removeOwned(battleUpdateTarget,
                          reinterpret_cast<void**>(&oBattleUpdate),
                          "battle update hook");
        LogOut("[FRONTEND] Failed to enable battle hotkey gate; install rolled back", true);
        s_frontendHooksAttempted.store(true);
        return false;
    }

    s_frontendHooksInstalled.store(true, std::memory_order_release);
    s_frontendHooksPartial.store(false, std::memory_order_release);
    LogOut("[FRONTEND] Control hooks installed transactionally", true);
    return true;
}

void SetBattleUpdateCallbacks(BattleUpdateCallback beforeUpdate, BattleUpdateCallback afterUpdate) {
    s_beforeBattleUpdate.store(beforeUpdate, std::memory_order_relaxed);
    s_afterBattleUpdate.store(afterUpdate, std::memory_order_relaxed);
}

uint32_t GetCurrentBattleUpdateBatch() {
    return s_currentBattleBatch.load(std::memory_order_acquire);
}

uint32_t GetCompletedBattleUpdateBatch() {
    return s_completedBattleBatch.load(std::memory_order_acquire);
}

bool CanRequestFrontendExit(FrontendExitTarget target) {
    if (IsNetplaySuspendActive() || IsNetplaySessionActive()) {
        return false;
    }

    const uint8_t screen = ReadRawScreenStateNoDebounce();
    switch (target) {
    case FrontendExitTarget::CharacterSelect:
        return screen == SCREEN_BATTLE;
    case FrontendExitTarget::Loading:
        return screen == SCREEN_BATTLE && GetCurrentGameMode() == GameMode::Practice;
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
        if (target == FrontendExitTarget::Title || target == FrontendExitTarget::Loading) {
            if (!hooksReady) {
                LogOut("[FRONTEND] Requested exit override unavailable because the battle update hook is inactive", true);
                return false;
            }
            const PendingExitOverride overrideTarget = target == FrontendExitTarget::Title
                ? PendingExitOverride::Title
                : PendingExitOverride::Loading;
            s_pendingExitOverride.store(static_cast<int>(overrideTarget), std::memory_order_release);
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

        const char* resetReason = target == FrontendExitTarget::Title
            ? "MenuExitToTitle"
            : (target == FrontendExitTarget::Loading
                ? "MissionDirectReload"
                : "MenuExitToCharacterSelect");
        ResetModSessionForFrontendExit(resetReason);
        CloseTrainingMenuForFrontendExit();

        const char* requestLog = target == FrontendExitTarget::Title
            ? "[FRONTEND] Requested exit to Title through battle cleanup"
            : (target == FrontendExitTarget::Loading
                ? "[FRONTEND] Requested direct Loading through battle cleanup"
                : "[FRONTEND] Requested exit to Character Select through battle cleanup");
        LogOut(requestLog, true);
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
