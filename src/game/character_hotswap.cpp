#include "../include/game/character_hotswap.h"

#include "../include/core/constants.h"
#include "../include/core/logger.h"
#include "../include/core/memory.h"
#include "../include/game/character_settings.h"
#include "../include/game/frame_monitor.h"
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
constexpr uintptr_t GAME_SYSTEM_P1_CUSTOM_PALETTE_FLAG_OFFSET = 4920;
constexpr uintptr_t GAME_SYSTEM_P2_CUSTOM_PALETTE_FLAG_OFFSET = 4924;

constexpr uintptr_t CHARACTER_OBJECT_SELECT_ID_OFFSET = 141;
constexpr uintptr_t CHARACTER_OBJECT_PALETTE_INDEX_OFFSET = 142;

constexpr int STAGE_COUNT = 23;
constexpr int PALETTE_SLOT_COUNT = 6;
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
    uint8_t p1ColorReadback = PALETTE_INDEX_FIRST;
    uint8_t p2ColorReadback = PALETTE_INDEX_FIRST;
    DWORD p1CustomFlagReadback = 0;
    DWORD p2CustomFlagReadback = 0;
    DWORD sehCode = 0;
};

struct CustomPaletteLookupResult {
    bool valid = false;
    bool rootExists = false;
    bool systemExists = false;
    bool exists = false;
    std::string resourceName;
    std::string rootPath;
    std::string systemPath;
    std::string resolvedPath;
};

std::atomic<RequestState> s_state{RequestState::Idle};
std::atomic<UiStatus> s_uiStatus{UiStatus::Ready};
std::atomic<int> s_requestedP1Char{CHAR_ID_AKANE};
std::atomic<int> s_requestedP2Char{CHAR_ID_AKIKO};
std::atomic<int> s_requestedP1Color{PALETTE_INDEX_FIRST};
std::atomic<int> s_requestedP2Color{PALETTE_INDEX_FIRST};
std::atomic<int> s_requestedP1CustomPalette{0};
std::atomic<int> s_requestedP2CustomPalette{0};
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

bool IsValidPaletteIndex(int paletteIndex) {
    return paletteIndex >= 0 && paletteIndex < PALETTE_SLOT_COUNT;
}

bool PaletteSelectionsEqual(const PaletteSelection& lhs, const PaletteSelection& rhs) {
    return lhs.p1Color == rhs.p1Color
        && lhs.p2Color == rhs.p2Color
        && lhs.p1UseCustomPalette == rhs.p1UseCustomPalette
        && lhs.p2UseCustomPalette == rhs.p2UseCustomPalette;
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

const char* GetCharacterResourceName(int selectId) {
    switch (selectId) {
        case 0:  return "nanase";
        case 1:  return "ayu";
        case 2:  return "mai";
        case 3:  return "makoto";
        case 4:  return "akane";
        case 5:  return "mayu";
        case 6:  return "nagamori";
        case 7:  return "misaki";
        case 8:  return "shiori";
        case 9:  return "sayuri";
        case 10: return "nayuki";
        case 11: return "mio";
        case 12: return "exnanase";
        case 13: return "kaori";
        case 14: return "ikumi";
        case 15: return "mishio";
        case 16: return "akiko";
        case 17: return "nayukib";
        case 18: return "mizukab";
        case 19: return "kanna";
        case 20: return "kano";
        case 21: return "minagi";
        case 22: return "nayuki";
        case 23: return "misuzu";
        default: return nullptr;
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

std::string BuildGameRelativePath(const std::string& relativePath) {
    char exePath[MAX_PATH] = {0};
    const DWORD len = GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        return relativePath;
    }

    std::string absolutePath(exePath, len);
    const size_t slash = absolutePath.find_last_of("\\/");
    if (slash == std::string::npos) {
        return relativePath;
    }

    absolutePath.resize(slash + 1);
    absolutePath += relativePath;
    return absolutePath;
}

bool FileExists(const std::string& path) {
    const DWORD attributes = GetFileAttributesA(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

CustomPaletteLookupResult ProbeCharacterCustomPalette(int selectId, int paletteIndex) {
    CustomPaletteLookupResult result{};

    const char* resourceName = GetCharacterResourceName(selectId);
    if (!resourceName || !IsValidPaletteIndex(paletteIndex)) {
        return result;
    }

    result.valid = true;
    result.resourceName = resourceName;

    const std::string fileName = result.resourceName + std::to_string(paletteIndex + 1) + ".pal";
    result.rootPath = BuildGameRelativePath(result.resourceName + "\\" + fileName);
    result.systemPath = BuildGameRelativePath(std::string("system\\") + result.resourceName + "\\" + fileName);

    result.rootExists = FileExists(result.rootPath);
    result.systemExists = FileExists(result.systemPath);
    result.exists = result.rootExists || result.systemExists;

    if (result.rootExists) {
        result.resolvedPath = result.rootPath;
    } else if (result.systemExists) {
        result.resolvedPath = result.systemPath;
    } else {
        result.resolvedPath = result.rootPath;
    }

    return result;
}

void AppendCustomPaletteLookupDetails(std::ostringstream& oss, const CustomPaletteLookupResult& result) {
    oss << " resource=" << (result.resourceName.empty() ? "<invalid>" : result.resourceName)
        << " root=" << (result.rootPath.empty() ? "<none>" : result.rootPath)
        << " rootExists=" << (result.rootExists ? 1 : 0)
        << " system=" << (result.systemPath.empty() ? "<none>" : result.systemPath)
        << " systemExists=" << (result.systemExists ? 1 : 0)
        << " chosen=" << (result.exists ? result.resolvedPath : "<none>");
}

void LogCustomPaletteLookupIfChanged(const char* source,
                                     int selectId,
                                     int paletteIndex,
                                     const CustomPaletteLookupResult& result) {
    static bool s_probeStateInitialized = false;
    static int s_probeState[24][PALETTE_SLOT_COUNT];

    if (!s_probeStateInitialized) {
        for (int charIdx = 0; charIdx < 24; ++charIdx) {
            for (int slotIdx = 0; slotIdx < PALETTE_SLOT_COUNT; ++slotIdx) {
                s_probeState[charIdx][slotIdx] = -99;
            }
        }
        s_probeStateInitialized = true;
    }

    const int stateCode = result.valid
        ? ((result.rootExists ? 1 : 0) | (result.systemExists ? 2 : 0))
        : -1;
    if (selectId >= 0 && selectId < 24 && IsValidPaletteIndex(paletteIndex)) {
        if (s_probeState[selectId][paletteIndex] == stateCode) {
            return;
        }
        s_probeState[selectId][paletteIndex] = stateCode;
    }

    std::ostringstream oss;
    oss << "[HOTSWAP][PAL][FILE][" << (source ? source : "CHECK") << "]"
        << " char=" << GetCharacterSelectName(selectId) << "(" << selectId << ")"
        << " slot=" << (paletteIndex + 1);
    AppendCustomPaletteLookupDetails(oss, result);
    LogOut(oss.str(), true);
}

void LogMatchPaletteReadinessIfChanged(bool charactersInitialized,
                                       uintptr_t gameSystem,
                                       uintptr_t p1Character,
                                       uintptr_t p2Character) {
    static int s_lastCharsInitialized = -1;
    static uintptr_t s_lastGameSystem = 0;
    static uintptr_t s_lastP1Character = 0;
    static uintptr_t s_lastP2Character = 0;

    const int charsInitializedInt = charactersInitialized ? 1 : 0;
    if (charsInitializedInt == s_lastCharsInitialized
        && gameSystem == s_lastGameSystem
        && p1Character == s_lastP1Character
        && p2Character == s_lastP2Character) {
        return;
    }

    std::ostringstream oss;
    oss << "[HOTSWAP][PAL][READ][MATCH_READY]"
        << " charsInit=" << charsInitializedInt
        << " gs=" << Hex(gameSystem)
        << " p1Obj=" << Hex(p1Character)
        << " p2Obj=" << Hex(p2Character);
    LogOut(oss.str(), true);

    s_lastCharsInitialized = charsInitializedInt;
    s_lastGameSystem = gameSystem;
    s_lastP1Character = p1Character;
    s_lastP2Character = p2Character;
}

bool CharacterCustomPaletteExists(int selectId, int paletteIndex, std::string* outPath = nullptr) {
    const CustomPaletteLookupResult lookup = ProbeCharacterCustomPalette(selectId, paletteIndex);
    if (!lookup.valid) {
        if (outPath) {
            outPath->clear();
        }
        return false;
    }

    if (outPath) {
        *outPath = lookup.resolvedPath;
    }

    return lookup.exists;
}

void LogPaletteSelectionSnapshot(const char* source,
                                 const PaletteSelection& selection,
                                 uintptr_t contextA,
                                 uintptr_t contextB,
                                 uintptr_t contextC = 0,
                                 uintptr_t contextD = 0) {
    std::ostringstream oss;
    oss << "[HOTSWAP][PAL][READ][" << source << "]"
        << " palette=" << (selection.p1Color + 1) << "/" << (selection.p2Color + 1)
        << " custom=" << (selection.p1UseCustomPalette ? 1 : 0) << "/" << (selection.p2UseCustomPalette ? 1 : 0)
        << " a=" << Hex(contextA)
        << " b=" << Hex(contextB);
    if (contextC) {
        oss << " c=" << Hex(contextC);
    }
    if (contextD) {
        oss << " d=" << Hex(contextD);
    }
    LogOut(oss.str(), true);
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

bool CaptureCharacterSelectPaletteSelection(PaletteSelection& outSelection) {
    uintptr_t screenContext = 0;
    uintptr_t gameSystem = 0;
    if (!ResolveCharacterSelectContext(screenContext, gameSystem)) {
        return false;
    }

    uint8_t p1Color = PALETTE_INDEX_FIRST;
    uint8_t p2Color = PALETTE_INDEX_FIRST;
    DWORD p1CustomFlag = 0;
    DWORD p2CustomFlag = 0;
    if (!SafeReadMemory(screenContext + CS_SELECTED_P1_COLOR_OFFSET, &p1Color, sizeof(p1Color))) {
        return false;
    }
    if (!SafeReadMemory(screenContext + CS_SELECTED_P2_COLOR_OFFSET, &p2Color, sizeof(p2Color))) {
        return false;
    }
    if (!SafeReadMemory(gameSystem + GAME_SYSTEM_P1_CUSTOM_PALETTE_FLAG_OFFSET, &p1CustomFlag, sizeof(p1CustomFlag))) {
        return false;
    }
    if (!SafeReadMemory(gameSystem + GAME_SYSTEM_P2_CUSTOM_PALETTE_FLAG_OFFSET, &p2CustomFlag, sizeof(p2CustomFlag))) {
        return false;
    }

    outSelection.p1Color = IsValidPaletteIndex(static_cast<int>(p1Color)) ? static_cast<int>(p1Color) : PALETTE_INDEX_FIRST;
    outSelection.p2Color = IsValidPaletteIndex(static_cast<int>(p2Color)) ? static_cast<int>(p2Color) : PALETTE_INDEX_FIRST;
    outSelection.p1UseCustomPalette = p1CustomFlag != 0;
    outSelection.p2UseCustomPalette = p2CustomFlag != 0;

    static bool s_hasSnapshot = false;
    static PaletteSelection s_lastSelection{};
    static uintptr_t s_lastScreenContext = 0;
    static uintptr_t s_lastGameSystem = 0;
    if (!s_hasSnapshot
        || !PaletteSelectionsEqual(outSelection, s_lastSelection)
        || screenContext != s_lastScreenContext
        || gameSystem != s_lastGameSystem) {
        LogPaletteSelectionSnapshot("CS", outSelection, screenContext, gameSystem);
        s_lastSelection = outSelection;
        s_lastScreenContext = screenContext;
        s_lastGameSystem = gameSystem;
        s_hasSnapshot = true;
    }
    return true;
}

bool CaptureMatchPaletteSelection(PaletteSelection& outSelection) {
    const uintptr_t base = GetEFZBase();
    const uintptr_t gameSystem = GetGameStatePtr();
    if (!base || !gameSystem) {
        return false;
    }

    uintptr_t p1Character = 0;
    uintptr_t p2Character = 0;
    uint8_t p1Color = PALETTE_INDEX_FIRST;
    uint8_t p2Color = PALETTE_INDEX_FIRST;
    DWORD p1CustomFlag = 0;
    DWORD p2CustomFlag = 0;
    if (!SafeReadMemory(base + EFZ_BASE_OFFSET_P1, &p1Character, sizeof(p1Character)) || !p1Character) {
        return false;
    }
    if (!SafeReadMemory(base + EFZ_BASE_OFFSET_P2, &p2Character, sizeof(p2Character)) || !p2Character) {
        return false;
    }
    LogMatchPaletteReadinessIfChanged(AreCharactersInitialized(), gameSystem, p1Character, p2Character);
    if (!SafeReadMemory(p1Character + CHARACTER_OBJECT_PALETTE_INDEX_OFFSET, &p1Color, sizeof(p1Color))) {
        return false;
    }
    if (!SafeReadMemory(p2Character + CHARACTER_OBJECT_PALETTE_INDEX_OFFSET, &p2Color, sizeof(p2Color))) {
        return false;
    }
    if (!SafeReadMemory(gameSystem + GAME_SYSTEM_P1_CUSTOM_PALETTE_FLAG_OFFSET, &p1CustomFlag, sizeof(p1CustomFlag))) {
        return false;
    }
    if (!SafeReadMemory(gameSystem + GAME_SYSTEM_P2_CUSTOM_PALETTE_FLAG_OFFSET, &p2CustomFlag, sizeof(p2CustomFlag))) {
        return false;
    }

    outSelection.p1Color = IsValidPaletteIndex(static_cast<int>(p1Color)) ? static_cast<int>(p1Color) : PALETTE_INDEX_FIRST;
    outSelection.p2Color = IsValidPaletteIndex(static_cast<int>(p2Color)) ? static_cast<int>(p2Color) : PALETTE_INDEX_FIRST;
    outSelection.p1UseCustomPalette = p1CustomFlag != 0;
    outSelection.p2UseCustomPalette = p2CustomFlag != 0;

    static bool s_hasSnapshot = false;
    static PaletteSelection s_lastSelection{};
    static uintptr_t s_lastGameSystem = 0;
    static uintptr_t s_lastP1Character = 0;
    static uintptr_t s_lastP2Character = 0;
    if (!s_hasSnapshot
        || !PaletteSelectionsEqual(outSelection, s_lastSelection)
        || gameSystem != s_lastGameSystem
        || p1Character != s_lastP1Character
        || p2Character != s_lastP2Character) {
        LogPaletteSelectionSnapshot("MATCH", outSelection, gameSystem, p1Character, p2Character, base);
        s_lastSelection = outSelection;
        s_lastGameSystem = gameSystem;
        s_lastP1Character = p1Character;
        s_lastP2Character = p2Character;
        s_hasSnapshot = true;
    }
    return true;
}

bool CaptureCurrentPaletteSelection(GamePhase currentPhase, PaletteSelection& outSelection) {
    outSelection = PaletteSelection{};
    if (currentPhase == GamePhase::Match) {
        return CaptureMatchPaletteSelection(outSelection);
    }
    if (currentPhase == GamePhase::CharacterSelect) {
        return CaptureCharacterSelectPaletteSelection(outSelection);
    }
    return false;
}

void ResolveRequestedCustomPalette(int selectId,
                                   const char* playerLabel,
                                   uint8_t colorIndex,
                                   bool& useCustomPalette) {
    if (!useCustomPalette) {
        return;
    }

    const CustomPaletteLookupResult lookup = ProbeCharacterCustomPalette(selectId, static_cast<int>(colorIndex));
    LogCustomPaletteLookupIfChanged(playerLabel, selectId, static_cast<int>(colorIndex), lookup);
    if (lookup.exists) {
        std::ostringstream oss;
        oss << "[HOTSWAP][PAL] keeping custom palette for " << playerLabel
            << " char=" << GetCharacterSelectName(selectId) << "(" << selectId << ")"
            << " slot=" << (static_cast<int>(colorIndex) + 1);
        AppendCustomPaletteLookupDetails(oss, lookup);
        LogOut(oss.str(), true);
        return;
    }

    std::ostringstream oss;
    oss << "[HOTSWAP] custom palette disabled for " << playerLabel
        << " char=" << GetCharacterSelectName(selectId) << "(" << selectId << ")"
        << " slot=" << (static_cast<int>(colorIndex) + 1)
        << " reason=missing custom .pal";
    AppendCustomPaletteLookupDetails(oss, lookup);
    LogOut(oss.str(), true);
    useCustomPalette = false;
}

void SanitizeRequestedPaletteSelection(int p1CharId, int p2CharId, PaletteSelection& selection) {
    const PaletteSelection originalSelection = selection;
    if (!IsValidPaletteIndex(static_cast<int>(selection.p1Color))) {
        selection.p1Color = PALETTE_INDEX_FIRST;
        selection.p1UseCustomPalette = false;
    }
    if (!IsValidPaletteIndex(static_cast<int>(selection.p2Color))) {
        selection.p2Color = PALETTE_INDEX_FIRST;
        selection.p2UseCustomPalette = false;
    }

    ResolveRequestedCustomPalette(p1CharId, "P1", selection.p1Color, selection.p1UseCustomPalette);
    ResolveRequestedCustomPalette(p2CharId, "P2", selection.p2Color, selection.p2UseCustomPalette);

    std::ostringstream oss;
    oss << "[HOTSWAP][PAL] sanitize chars="
        << GetCharacterSelectName(p1CharId) << "(" << p1CharId << ")/"
        << GetCharacterSelectName(p2CharId) << "(" << p2CharId << ")"
        << " before=" << (originalSelection.p1Color + 1) << "/" << (originalSelection.p2Color + 1)
        << " custom=" << (originalSelection.p1UseCustomPalette ? 1 : 0) << "/" << (originalSelection.p2UseCustomPalette ? 1 : 0)
        << " after=" << (selection.p1Color + 1) << "/" << (selection.p2Color + 1)
        << " custom=" << (selection.p1UseCustomPalette ? 1 : 0) << "/" << (selection.p2UseCustomPalette ? 1 : 0)
        << " changed=" << (!PaletteSelectionsEqual(originalSelection, selection) ? 1 : 0);
    LogOut(oss.str(), true);
}

bool SehApplySelections(int p1CharId,
                        int p2CharId,
                        int stageId,
                        int p1ColorIndex,
                        int p2ColorIndex,
                        bool p1UseCustomPalette,
                        bool p2UseCustomPalette,
                        RawApplyResult& outResult) {
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
        const uint8_t p1Color = static_cast<uint8_t>(IsValidPaletteIndex(p1ColorIndex) ? p1ColorIndex : PALETTE_INDEX_FIRST);
        const uint8_t p2Color = static_cast<uint8_t>(IsValidPaletteIndex(p2ColorIndex) ? p2ColorIndex : PALETTE_INDEX_FIRST);
        const uint8_t p1State = 4;
        const uint8_t p2State = 4;
        const uint8_t stage = static_cast<uint8_t>(stageId);
        const uint16_t stageAutoconfirmTicks = 1;
        const DWORD p1CustomFlag = p1UseCustomPalette ? 1u : 0u;
        const DWORD p2CustomFlag = p2UseCustomPalette ? 1u : 0u;

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
        if (!SafeWriteMemory(outResult.gameSystem + GAME_SYSTEM_P1_CUSTOM_PALETTE_FLAG_OFFSET, &p1CustomFlag, sizeof(p1CustomFlag))) {
            return false;
        }
        if (!SafeWriteMemory(outResult.gameSystem + GAME_SYSTEM_P2_CUSTOM_PALETTE_FLAG_OFFSET, &p2CustomFlag, sizeof(p2CustomFlag))) {
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
        if (!SafeWriteMemory(outResult.p1Character + CHARACTER_OBJECT_PALETTE_INDEX_OFFSET, &p1Color, sizeof(p1Color))) {
            return false;
        }
        if (!SafeWriteMemory(outResult.p2Character + CHARACTER_OBJECT_PALETTE_INDEX_OFFSET, &p2Color, sizeof(p2Color))) {
            return false;
        }
        SafeReadMemory(outResult.p1Character + CHARACTER_OBJECT_PALETTE_INDEX_OFFSET,
                       &outResult.p1ColorReadback,
                       sizeof(outResult.p1ColorReadback));
        SafeReadMemory(outResult.p2Character + CHARACTER_OBJECT_PALETTE_INDEX_OFFSET,
                       &outResult.p2ColorReadback,
                       sizeof(outResult.p2ColorReadback));
        SafeReadMemory(outResult.gameSystem + GAME_SYSTEM_P1_CUSTOM_PALETTE_FLAG_OFFSET,
                       &outResult.p1CustomFlagReadback,
                       sizeof(outResult.p1CustomFlagReadback));
        SafeReadMemory(outResult.gameSystem + GAME_SYSTEM_P2_CUSTOM_PALETTE_FLAG_OFFSET,
                       &outResult.p2CustomFlagReadback,
                       sizeof(outResult.p2CustomFlagReadback));

        return outResult.p1Character != 0 && outResult.p2Character != 0;
    } __except (outResult.sehCode = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void LogQueuedRequest(GamePhase phase,
                      GameMode mode,
                      int p1CharId,
                      int p2CharId,
                      int stageId,
                      unsigned short bgmTrack,
                      const PaletteSelection& paletteSelection) {
    std::ostringstream oss;
    oss << "[HOTSWAP] queued reload"
        << " phase=" << PhaseName(phase)
        << " mode=" << GetGameModeName(mode)
        << " p1=" << GetCharacterSelectName(p1CharId) << "(" << p1CharId << ")"
        << " p2=" << GetCharacterSelectName(p2CharId) << "(" << p2CharId << ")"
        << " stage=" << stageId
        << " bgm=" << bgmTrack
        << " palette=" << (static_cast<int>(paletteSelection.p1Color) + 1)
        << "/" << (static_cast<int>(paletteSelection.p2Color) + 1)
        << " custom=" << (paletteSelection.p1UseCustomPalette ? 1 : 0)
        << "/" << (paletteSelection.p2UseCustomPalette ? 1 : 0);
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
    PaletteSelection paletteSelection{};
    if (!ReadCurrentPaletteSelection(paletteSelection)) {
        LogOut("[HOTSWAP] failed to capture current palette selection; defaulting to slot 1 without custom palettes", true);
    }
    return QueueReload(p1CharId, p2CharId, stageId, paletteSelection, bgmTrack);
}

bool QueueReload(int p1CharId, int p2CharId, int stageId, const PaletteSelection& requestedPaletteSelection, unsigned short bgmTrack) {
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

    PaletteSelection paletteSelection = requestedPaletteSelection;
    SanitizeRequestedPaletteSelection(p1CharId, p2CharId, paletteSelection);

    s_requestedP1Char.store(p1CharId, std::memory_order_relaxed);
    s_requestedP2Char.store(p2CharId, std::memory_order_relaxed);
    s_requestedP1Color.store(static_cast<int>(paletteSelection.p1Color), std::memory_order_relaxed);
    s_requestedP2Color.store(static_cast<int>(paletteSelection.p2Color), std::memory_order_relaxed);
    s_requestedP1CustomPalette.store(paletteSelection.p1UseCustomPalette ? 1 : 0, std::memory_order_relaxed);
    s_requestedP2CustomPalette.store(paletteSelection.p2UseCustomPalette ? 1 : 0, std::memory_order_relaxed);
    s_requestedStage.store(stageId, std::memory_order_relaxed);
    s_requestedBgmTrack.store(static_cast<int>(bgmTrack), std::memory_order_relaxed);

    LogQueuedRequest(currentPhase, currentMode, p1CharId, p2CharId, stageId, bgmTrack, paletteSelection);

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
                const int p1ColorIndex = s_requestedP1Color.load(std::memory_order_relaxed);
                const int p2ColorIndex = s_requestedP2Color.load(std::memory_order_relaxed);
                const bool p1UseCustomPalette = s_requestedP1CustomPalette.load(std::memory_order_relaxed) != 0;
                const bool p2UseCustomPalette = s_requestedP2CustomPalette.load(std::memory_order_relaxed) != 0;
                const int stageId = s_requestedStage.load(std::memory_order_relaxed);
                if (!SehApplySelections(p1CharId,
                                        p2CharId,
                                        stageId,
                                        p1ColorIndex,
                                        p2ColorIndex,
                                        p1UseCustomPalette,
                                        p2UseCustomPalette,
                                        result)) {
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
                    << " stage=" << stageId
                    << " palette=" << (p1ColorIndex + 1)
                    << "/" << (p2ColorIndex + 1)
                    << " custom=" << (p1UseCustomPalette ? 1 : 0)
                    << "/" << (p2UseCustomPalette ? 1 : 0)
                    << " readbackPalette=" << (static_cast<int>(result.p1ColorReadback) + 1)
                    << "/" << (static_cast<int>(result.p2ColorReadback) + 1)
                    << " readbackCustom=" << result.p1CustomFlagReadback
                    << "/" << result.p2CustomFlagReadback;
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

bool ReadCurrentPaletteSelection(PaletteSelection& outSelection) {
    const GamePhase phase = GetCurrentGamePhase();
    const bool ok = CaptureCurrentPaletteSelection(phase, outSelection);
    static bool s_lastOk = true;
    static GamePhase s_lastPhase = GamePhase::Unknown;
    if (!ok && (s_lastOk || s_lastPhase != phase)) {
        LogOut(std::string("[HOTSWAP][PAL][READ] failed phase=") + PhaseName(phase), true);
    }
    s_lastOk = ok;
    s_lastPhase = phase;
    return ok;
}

bool HasCustomPaletteFile(int selectId, int paletteIndex) {
    const CustomPaletteLookupResult lookup = ProbeCharacterCustomPalette(selectId, paletteIndex);
    LogCustomPaletteLookupIfChanged("HAS", selectId, paletteIndex, lookup);
    return lookup.exists;
}

void SanitizePaletteSelection(int p1CharId, int p2CharId, PaletteSelection& selection) {
    SanitizeRequestedPaletteSelection(p1CharId, p2CharId, selection);
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