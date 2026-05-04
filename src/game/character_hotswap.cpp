#include "../include/game/character_hotswap.h"

#include "../include/core/constants.h"
#include "../include/core/logger.h"
#include "../include/core/memory.h"
#include "../include/game/character_settings.h"
#include "../include/game/game_state.h"
#include "../include/utils/bgm_control.h"
#include "../include/utils/utilities.h"

#include <windows.h>

#include <atomic>
#include <sstream>
#include <string>

namespace CharacterHotswap {
namespace {

constexpr uintptr_t RVA_SCREEN_TABLE = 0x00390110;
constexpr uintptr_t RVA_INITIALIZE_SELECTED_CHARACTER = 0x003586B0;

constexpr uint8_t SCREEN_CHARACTER_SELECT = 1;

constexpr uintptr_t CS_SLOT_P1_PTR_OFFSET = 20;
constexpr uintptr_t CS_SLOT_P2_PTR_OFFSET = 24;
constexpr uintptr_t CS_P1_STATE_OFFSET = 1182;
constexpr uintptr_t CS_P2_STATE_OFFSET = 1183;
constexpr uintptr_t CS_SELECTED_P1_CHAR_OFFSET = 1340;
constexpr uintptr_t CS_SELECTED_P2_CHAR_OFFSET = 1341;
constexpr uintptr_t CS_SELECTED_P1_COLOR_OFFSET = 1342;
constexpr uintptr_t CS_SELECTED_P2_COLOR_OFFSET = 1343;
constexpr uintptr_t CS_STAGE_AUTOCONFIRM_TIMER_OFFSET = 1344;

constexpr uintptr_t GAME_SYSTEM_STAGE_OFFSET = 3890;

constexpr int STAGE_COUNT = 23;
constexpr int PALETTE_INDEX_FIRST = 0;
constexpr int kWaitExitTicks = 900;
constexpr int kWaitLoadingTicks = 450;
constexpr int kWaitMatchTicks = 1800;

using InitializeSelectedCharacterFn = int(__thiscall*)(int* screenContext,
                                                       int characterSlotPtr,
                                                       int opponentSlotPtr,
                                                       char playerIndex);

enum class RequestState : uint8_t {
    Idle = 0,
    PendingExitRequest,
    AwaitingCharacterSelect,
    PendingApply,
    AwaitingLoading,
    AwaitingMatch,
    Failed,
};

enum class UiStatus : uint8_t {
    Ready = 0,
    Exiting,
    Applying,
    WaitingLoading,
    WaitingMatch,
    Failed,
    Completed,
};

struct RawApplyResult {
    uintptr_t screenContext = 0;
    uintptr_t gameSystem = 0;
    uintptr_t p1SlotStorage = 0;
    uintptr_t p2SlotStorage = 0;
    uintptr_t p1Character = 0;
    uintptr_t p2Character = 0;
    DWORD sehCode = 0;
};

std::atomic<RequestState> s_state{RequestState::Idle};
std::atomic<UiStatus> s_uiStatus{UiStatus::Ready};
std::atomic<int> s_requestedP1Char{CHAR_ID_AKANE};
std::atomic<int> s_requestedP2Char{CHAR_ID_AKIKO};
std::atomic<int> s_requestedStage{0};
std::atomic<int> s_requestedBgmTrack{0};
std::atomic<int> s_waitTicks{0};
GamePhase s_lastActivePhase = GamePhase::Unknown;

bool IsValidCharacterId(int charId) {
    return charId >= CHAR_ID_AKANE && charId <= CHAR_ID_KANO;
}

bool IsValidStageId(int stageId) {
    return stageId >= 0 && stageId < STAGE_COUNT;
}

const char* GetCharacterSelectName(int selectId) {
    switch (selectId) {
        case 0:  return "Rumi";
        case 1:  return "Ayu";
        case 2:  return "Mai";
        case 3:  return "Makoto";
        case 4:  return "Akane";
        case 5:  return "Mayu";
        case 6:  return "Mizuka";
        case 7:  return "Misaki";
        case 8:  return "Shiori";
        case 9:  return "Sayuri";
        case 10: return "Neyuki";
        case 11: return "Mio";
        case 12: return "Doppel";
        case 13: return "Kaori";
        case 14: return "Ikumi";
        case 15: return "Mishio";
        case 16: return "Akiko";
        case 17: return "Nayuki";
        case 18: return "Unknown";
        case 19: return "Kanna";
        case 20: return "Kano";
        case 21: return "Minagi";
        case 22: return "Neyuki (Alt)";
        case 23: return "Misuzu";
        default: return "Invalid";
    }
}

bool IsActiveState(RequestState state) {
    return state != RequestState::Idle && state != RequestState::Failed;
}

const char* StatusText(UiStatus status) {
    switch (status) {
        case UiStatus::Ready:          return "READY";
        case UiStatus::Exiting:        return "EXITING";
        case UiStatus::Applying:       return "APPLYING";
        case UiStatus::WaitingLoading: return "LOADING";
        case UiStatus::WaitingMatch:   return "WAIT MATCH";
        case UiStatus::Failed:         return "FAILED";
        case UiStatus::Completed:      return "DONE";
        default:                       return "READY";
    }
}

const char* PhaseName(GamePhase phase) {
    switch (phase) {
        case GamePhase::Menu:            return "Menu";
        case GamePhase::CharacterSelect: return "CharacterSelect";
        case GamePhase::Loading:         return "Loading";
        case GamePhase::Match:           return "Match";
        default:                         return "Unknown";
    }
}

std::string Hex(uintptr_t value) {
    std::ostringstream oss;
    oss << "0x" << std::hex << std::uppercase << value;
    return oss.str();
}

void SetState(RequestState nextState, UiStatus nextStatus, const char* reason) {
    const RequestState prevState = s_state.load(std::memory_order_relaxed);
    s_state.store(nextState, std::memory_order_release);
    s_uiStatus.store(nextStatus, std::memory_order_release);
    s_waitTicks.store(0, std::memory_order_relaxed);

    std::ostringstream oss;
    oss << "[HOTSWAP] state " << static_cast<int>(prevState)
        << " -> " << static_cast<int>(nextState)
        << " status=" << StatusText(nextStatus);
    if (reason && reason[0] != '\0') {
        oss << " reason=" << reason;
    }
    LogOut(oss.str(), true);
}

void FailRequest(const char* reason, GamePhase currentPhase) {
    std::ostringstream oss;
    oss << "[HOTSWAP] failed in phase=" << PhaseName(currentPhase)
        << " reason=" << (reason ? reason : "unknown");
    LogOut(oss.str(), true);
    s_state.store(RequestState::Failed, std::memory_order_release);
    s_uiStatus.store(UiStatus::Failed, std::memory_order_release);
    s_waitTicks.store(0, std::memory_order_relaxed);
}

bool ResolveCharacterSelectContext(uintptr_t& outScreenContext, uintptr_t& outGameSystem) {
    outScreenContext = 0;
    outGameSystem = 0;

    const uintptr_t base = GetEFZBase();
    if (!base) {
        return false;
    }

    const uintptr_t slot = base + RVA_SCREEN_TABLE + 4u * static_cast<uintptr_t>(SCREEN_CHARACTER_SELECT);
    if (!SafeReadMemory(slot, &outScreenContext, sizeof(outScreenContext)) || !outScreenContext) {
        return false;
    }

    if (!SafeReadMemory(base + EFZ_BASE_OFFSET_GAME_STATE, &outGameSystem, sizeof(outGameSystem)) || !outGameSystem) {
        return false;
    }

    return true;
}

bool SehApplySelections(int p1CharId, int p2CharId, int stageId, RawApplyResult& outResult) {
    outResult = RawApplyResult{};

    __try {
        const uintptr_t base = GetEFZBase();
        if (!base) {
            return false;
        }

        if (!ResolveCharacterSelectContext(outResult.screenContext, outResult.gameSystem)) {
            return false;
        }

        int p1SlotStorage = 0;
        int p2SlotStorage = 0;
        if (!SafeReadMemory(outResult.screenContext + CS_SLOT_P1_PTR_OFFSET, &p1SlotStorage, sizeof(p1SlotStorage)) || !p1SlotStorage) {
            return false;
        }
        if (!SafeReadMemory(outResult.screenContext + CS_SLOT_P2_PTR_OFFSET, &p2SlotStorage, sizeof(p2SlotStorage)) || !p2SlotStorage) {
            return false;
        }

        outResult.p1SlotStorage = static_cast<uintptr_t>(p1SlotStorage);
        outResult.p2SlotStorage = static_cast<uintptr_t>(p2SlotStorage);

        const uint8_t p1Char = static_cast<uint8_t>(p1CharId);
        const uint8_t p2Char = static_cast<uint8_t>(p2CharId);
        const uint8_t p1Color = PALETTE_INDEX_FIRST;
        const uint8_t p2Color = PALETTE_INDEX_FIRST;
        const uint8_t p1State = 4;
        const uint8_t p2State = 4;
        const uint8_t stage = static_cast<uint8_t>(stageId);
        const uint16_t stageAutoconfirmTicks = 1;

        if (!SafeWriteMemory(outResult.gameSystem + GAME_SYSTEM_STAGE_OFFSET, &stage, sizeof(stage))) {
            return false;
        }
        if (!SafeWriteMemory(outResult.screenContext + CS_SELECTED_P1_CHAR_OFFSET, &p1Char, sizeof(p1Char))) {
            return false;
        }
        if (!SafeWriteMemory(outResult.screenContext + CS_SELECTED_P2_CHAR_OFFSET, &p2Char, sizeof(p2Char))) {
            return false;
        }
        if (!SafeWriteMemory(outResult.screenContext + CS_SELECTED_P1_COLOR_OFFSET, &p1Color, sizeof(p1Color))) {
            return false;
        }
        if (!SafeWriteMemory(outResult.screenContext + CS_SELECTED_P2_COLOR_OFFSET, &p2Color, sizeof(p2Color))) {
            return false;
        }

        const auto initSelectedCharacter = reinterpret_cast<InitializeSelectedCharacterFn>(base + RVA_INITIALIZE_SELECTED_CHARACTER);
        if (!initSelectedCharacter) {
            return false;
        }

        initSelectedCharacter(reinterpret_cast<int*>(outResult.screenContext), p1SlotStorage, p2SlotStorage, 0);
        initSelectedCharacter(reinterpret_cast<int*>(outResult.screenContext), p2SlotStorage, p1SlotStorage, 1);

        if (!SafeWriteMemory(outResult.screenContext + CS_P1_STATE_OFFSET, &p1State, sizeof(p1State))) {
            return false;
        }
        if (!SafeWriteMemory(outResult.screenContext + CS_P2_STATE_OFFSET, &p2State, sizeof(p2State))) {
            return false;
        }
        if (!SafeWriteMemory(outResult.screenContext + CS_STAGE_AUTOCONFIRM_TIMER_OFFSET, &stageAutoconfirmTicks, sizeof(stageAutoconfirmTicks))) {
            return false;
        }

        if (!SafeReadMemory(outResult.p1SlotStorage, &outResult.p1Character, sizeof(outResult.p1Character))) {
            return false;
        }
        if (!SafeReadMemory(outResult.p2SlotStorage, &outResult.p2Character, sizeof(outResult.p2Character))) {
            return false;
        }

        return outResult.p1Character != 0 && outResult.p2Character != 0;
    } __except (outResult.sehCode = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void LogQueuedRequest(GamePhase phase, GameMode mode, int p1CharId, int p2CharId, int stageId, unsigned short bgmTrack) {
    std::ostringstream oss;
    oss << "[HOTSWAP] queued reload"
        << " phase=" << PhaseName(phase)
        << " mode=" << GetGameModeName(mode)
        << " p1=" << GetCharacterSelectName(p1CharId) << "(" << p1CharId << ")"
        << " p2=" << GetCharacterSelectName(p2CharId) << "(" << p2CharId << ")"
        << " stage=" << stageId
        << " bgm=" << bgmTrack
        << " palette=0/0";
    LogOut(oss.str(), true);
}

void CompleteReload(GamePhase currentPhase) {
    const unsigned short bgmTrack = static_cast<unsigned short>(s_requestedBgmTrack.load(std::memory_order_relaxed));
    const uintptr_t gameStatePtr = GetGameStatePtr();
    if (gameStatePtr) {
        if (PlayBGM(gameStatePtr, bgmTrack)) {
            LogOut("[HOTSWAP] applied BGM override track=" + std::to_string(bgmTrack), true);
        } else {
            LogOut("[HOTSWAP] failed to apply BGM override track=" + std::to_string(bgmTrack), true);
        }
    } else {
        LogOut("[HOTSWAP] skipped BGM override because game state pointer was unavailable", true);
    }

    CharacterSettings::InvalidateAllCharacterPointerCaches();
    RequestRuntimeLifecycleResync("character hotswap reload complete");
    LogOut("[HOTSWAP] reload completed and lifecycle resync requested", true);
    s_state.store(RequestState::Idle, std::memory_order_release);
    s_uiStatus.store(UiStatus::Completed, std::memory_order_release);
    s_waitTicks.store(0, std::memory_order_relaxed);
    s_lastActivePhase = currentPhase;
}

} // namespace

bool QueueReload(int p1CharId, int p2CharId, int stageId, unsigned short bgmTrack) {
    if (!IsValidCharacterId(p1CharId) || !IsValidCharacterId(p2CharId) || !IsValidStageId(stageId)) {
        FailRequest("menu selection out of range", GetCurrentGamePhase());
        return false;
    }

    const RequestState state = s_state.load(std::memory_order_acquire);
    if (IsActiveState(state)) {
        LogOut("[HOTSWAP] request ignored because another reload is already active", true);
        return false;
    }

    const GamePhase currentPhase = GetCurrentGamePhase();
    const GameMode currentMode = GetCurrentGameMode();

    if (currentPhase == GamePhase::Match && !CanRequestFrontendExit(FrontendExitTarget::CharacterSelect)) {
        FailRequest("frontend exit to character select is unavailable", currentPhase);
        return false;
    }
    if (currentPhase != GamePhase::Match && currentPhase != GamePhase::CharacterSelect) {
        FailRequest("reload is only supported from match or character select", currentPhase);
        return false;
    }

    s_requestedP1Char.store(p1CharId, std::memory_order_relaxed);
    s_requestedP2Char.store(p2CharId, std::memory_order_relaxed);
    s_requestedStage.store(stageId, std::memory_order_relaxed);
    s_requestedBgmTrack.store(static_cast<int>(bgmTrack), std::memory_order_relaxed);

    LogQueuedRequest(currentPhase, currentMode, p1CharId, p2CharId, stageId, bgmTrack);

    if (currentPhase == GamePhase::Match) {
        SetState(RequestState::PendingExitRequest, UiStatus::Exiting, "leaving match for character select");
    } else {
        SetState(RequestState::PendingApply, UiStatus::Applying, "already in character select");
    }

    return true;
}

void Tick(GamePhase currentPhase, GameMode /*currentMode*/) {
    const RequestState state = s_state.load(std::memory_order_acquire);
    if (!IsActiveState(state)) {
        if (state == RequestState::Idle) {
            s_lastActivePhase = GamePhase::Unknown;
        }
        return;
    }

    if (currentPhase != s_lastActivePhase) {
        std::ostringstream oss;
        oss << "[HOTSWAP] observed phase=" << PhaseName(currentPhase);
        LogOut(oss.str(), true);
        s_lastActivePhase = currentPhase;
    }

    const int waited = s_waitTicks.fetch_add(1, std::memory_order_relaxed) + 1;

    switch (state) {
        case RequestState::PendingExitRequest:
            if (currentPhase != GamePhase::Match) {
                SetState(RequestState::AwaitingCharacterSelect, UiStatus::Exiting, "match already leaving");
                return;
            }
            if (!RequestFrontendExit(FrontendExitTarget::CharacterSelect)) {
                FailRequest("RequestFrontendExit(CharacterSelect) failed", currentPhase);
                return;
            }
            SetState(RequestState::AwaitingCharacterSelect, UiStatus::Exiting, "frontend exit requested");
            return;

        case RequestState::AwaitingCharacterSelect:
            if (currentPhase == GamePhase::CharacterSelect) {
                SetState(RequestState::PendingApply, UiStatus::Applying, "character select reached");
                return;
            }
            if (waited > kWaitExitTicks) {
                FailRequest("timed out waiting for character select", currentPhase);
            }
            return;

        case RequestState::PendingApply:
            if (currentPhase != GamePhase::CharacterSelect) {
                if (waited > kWaitExitTicks) {
                    FailRequest("character select disappeared before apply", currentPhase);
                }
                return;
            }
            {
                RawApplyResult result{};
                const int p1CharId = s_requestedP1Char.load(std::memory_order_relaxed);
                const int p2CharId = s_requestedP2Char.load(std::memory_order_relaxed);
                const int stageId = s_requestedStage.load(std::memory_order_relaxed);
                if (!SehApplySelections(p1CharId, p2CharId, stageId, result)) {
                    std::ostringstream oss;
                    oss << "[HOTSWAP] apply failed"
                        << " seh=" << result.sehCode
                        << " cs=" << Hex(result.screenContext)
                        << " gs=" << Hex(result.gameSystem)
                        << " slot1=" << Hex(result.p1SlotStorage)
                        << " slot2=" << Hex(result.p2SlotStorage);
                    LogOut(oss.str(), true);
                    FailRequest("failed to write character select state", currentPhase);
                    return;
                }

                std::ostringstream oss;
                oss << "[HOTSWAP] apply succeeded"
                    << " cs=" << Hex(result.screenContext)
                    << " gs=" << Hex(result.gameSystem)
                    << " p1Obj=" << Hex(result.p1Character)
                    << " p2Obj=" << Hex(result.p2Character)
                    << " stage=" << stageId;
                LogOut(oss.str(), true);
            }
            SetState(RequestState::AwaitingLoading, UiStatus::WaitingLoading, "selections injected into character select");
            return;

        case RequestState::AwaitingLoading:
            if (currentPhase == GamePhase::Loading) {
                SetState(RequestState::AwaitingMatch, UiStatus::WaitingMatch, "loading screen reached");
                return;
            }
            if (currentPhase == GamePhase::Match) {
                CompleteReload(currentPhase);
                return;
            }
            if (waited > kWaitLoadingTicks) {
                FailRequest("timed out waiting for loading", currentPhase);
            }
            return;

        case RequestState::AwaitingMatch:
            if (currentPhase == GamePhase::Match) {
                CompleteReload(currentPhase);
                return;
            }
            if (waited > kWaitMatchTicks) {
                FailRequest("timed out waiting for match re-entry", currentPhase);
            }
            return;

        case RequestState::Idle:
        case RequestState::Failed:
        default:
            return;
    }
}

bool IsBusy() {
    return IsActiveState(s_state.load(std::memory_order_acquire));
}

bool CanQueueReload() {
    if (IsBusy()) {
        return false;
    }

    const GamePhase phase = GetCurrentGamePhase();
    if (phase == GamePhase::CharacterSelect) {
        return true;
    }
    if (phase == GamePhase::Match) {
        return CanRequestFrontendExit(FrontendExitTarget::CharacterSelect);
    }
    return false;
}

const char* GetActionValueText() {
    const RequestState state = s_state.load(std::memory_order_acquire);
    const UiStatus status = s_uiStatus.load(std::memory_order_acquire);
    if (IsActiveState(state)) {
        return StatusText(status);
    }
    if (status == UiStatus::Failed || status == UiStatus::Completed) {
        return StatusText(status);
    }

    const GamePhase phase = GetCurrentGamePhase();
    if (phase == GamePhase::CharacterSelect) {
        return "READY";
    }
    if (phase == GamePhase::Match) {
        return CanRequestFrontendExit(FrontendExitTarget::CharacterSelect) ? "READY" : "MATCH ONLY";
    }
    return "MATCH/CS ONLY";
}

} // namespace CharacterHotswap