#include "../../../include/game/practice_menu/practice_menu.h"
#include "../../../include/game/practice_menu/efz_title.h"
#include "../../../include/game/practice_menu/title_render.h"

#include "../../../include/core/logger.h"
#include "../../../include/core/memory.h"
#include "../../../include/utils/minhook_utils.h"
#include "../../../include/utils/utilities.h"  // detailedLogging
#include "../../../include/game/mission/mission_engine.h"  // SetPendingMissionLoad
#include "../../../include/game/mission/mission_data.h"      // mission list scan
#include "../../../include/game/mission/tutorial_support.h"  // capabilities/progress
#include "../../../include/game/mission/tutorial_session.h"  // Next Lesson registry
#include "../../../include/game/character_hotswap.h"
#include "../../../include/game/practice_menu/mission_title_screen.h"
#include "../../../include/gui/overlay.h"

#include <algorithm>
#include <string>
#include <vector>

#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

// PracticeMenu - see practice_menu.h for the coexistence rationale.
//
// Two interception points, neither of which touches the title menu count,
// geometry, jump table, or vtable (so InGameNetplay is unaffected):
//   1) A 5-byte jmp patch on the Practice CASE (0x776352). When the player
//      confirms Practice, execution reaches this jump-table target; we jmp to a
//      naked thunk that enters our submenu and returns 0 (stay on title) via the
//      confirm-switch epilogue - BEFORE the game's blocking fade-out runs.
//   2) A MinHook trampoline on updateTitleScreenLogic (0x775FB0). While the
//      submenu is active this detour owns the frame: it polls input, drives
//      navigation, renders, and returns 0. When inactive it calls through to the
//      original (so vanilla + InGameNetplay behave normally).

using namespace PracticeMenu::EfzTitle;

namespace {

// Additional verified helper not in efz_title.h.
constexpr uintptr_t kVaProcessPlayerInput = 0x00406590; // processPlayerInput(inputManager)
typedef void (__thiscall* ProcessInputFn)(void* inputManager);

// ---- module state ---------------------------------------------------------
// Submenu lifecycle: Inactive -> Entering (slide in) -> Active (input) ->
// Leaving (slide out) -> Inactive. Input is only handled while Active; the
// enter/leave phases just animate the slide.
enum Phase { PHASE_INACTIVE = 0, PHASE_ENTERING, PHASE_ACTIVE, PHASE_LEAVING };

std::atomic<bool> g_installed{false};
std::atomic<int>  g_phase{PHASE_INACTIVE};
std::atomic<bool> g_needPaletteApply{false}; // re-apply our palette on each entry
float g_animT = 0.0f;   // 0 = fully off-screen (right), 1 = fully in place
int  g_selection = PracticeMenu::Render::ROW_PRACTICE;   // default to the top row
uint32_t g_lastScreenContext = 0;

// Slide animation tuning (native 320x240 space).
constexpr float kAnimStep     = 0.16f; // ~6-7 frames edge-to-edge
constexpr int   kSlideDistPx  = 200;   // start off the right edge

TitleUpdateFn g_origUpdate = nullptr;      // MinHook trampoline
uintptr_t     g_titleEpilogueAddr = 0;     // resolved 0x776483, read by the naked thunk

// Practice-case jmp patch bookkeeping.
uintptr_t g_practiceCaseAddr = 0;
uint8_t   g_practiceCaseOriginal[5] = {0};
bool      g_practiceCasePatched = false;

// Per-frame input edge tracking (our own latch; format-agnostic).
struct InputState { int horiz = 0; int vert = 0; bool confirm = false; bool cancel = false; };
InputState g_prevInput;

// Deferred browser reopen (RETURN TO MISSIONS/LESSONS from the session pause
// menu). Consumed by the title update detour on the first title frame.
// 0 = none, otherwise TitleScreen::Screen value.
std::atomic<int> g_pendingReopen{0};

inline float Smoothstep(float t) {
    if (t < 0.0f) t = 0.0f; else if (t > 1.0f) t = 1.0f;
    return t * t * (3.0f - 2.0f * t);
}
// Current horizontal slide offset in px (0 when fully in place).
inline int SlideOffsetPx() {
    return static_cast<int>((1.0f - Smoothstep(g_animT)) * static_cast<float>(kSlideDistPx));
}

void Log(const char* msg) { LogOut(std::string("[PRACTICE_MENU] ") + msg, true); }

bool SupportIssueContains(const std::vector<std::string>& issues, const char* needle) {
    for (const std::string& issue : issues) {
        if (issue.find(needle) != std::string::npos) return true;
    }
    return false;
}

// RuntimeSupportIssues intentionally uses precise capability/task language for
// authors and logs. The course browser is for players, so reduce that list to
// the first useful explanation without exposing schema names or task IDs.
std::string PlayerFacingUnavailableReason(const std::vector<std::string>& issues) {
    const bool setup = SupportIssueContains(issues, "exact start state");
    const bool opponent = SupportIssueContains(issues, "dummy episode") ||
                          SupportIssueContains(issues, "dummy_script") ||
                          SupportIssueContains(issues, "injectable action") ||
                          SupportIssueContains(issues, " trigger '") ||
                          SupportIssueContains(issues, " start '");
    const bool exercise = SupportIssueContains(issues, "placeholder") ||
                          SupportIssueContains(issues, "not authored") ||
                          SupportIssueContains(issues, "not declared") ||
                          SupportIssueContains(issues, "unknown dummy episode");
    const bool demonstration = SupportIssueContains(issues, "demonstration");
    const bool retry = SupportIssueContains(issues, "failure reset") ||
                       SupportIssueContains(issues, "checkpoint");

    if (setup) {
        return "The exact starting position for this lesson is still being prepared.";
    }
    if (opponent && exercise) {
        return "This lesson's exercise and scripted opponent are still being prepared.";
    }
    if (opponent) {
        return "The scripted opponent for this lesson is not ready yet.";
    }
    if (exercise) {
        return "The exercise for this lesson is still being authored.";
    }
    if (demonstration) {
        return "This lesson's demonstration is not ready yet.";
    }
    if (retry) {
        return "This lesson's retry behavior is not ready yet.";
    }
    return "The game-event tracking needed to score this lesson accurately is still being completed.";
}

// Build the browser's rich mission list: every pack scenario + recorded file
// is fully parsed (name/desc/type/chars/stage/steps) so the screen can group
// by character and show a proper detail footer. Own frame: safe for C++
// objects; UpdateDetour's __try only calls it.
void PopulateMissionEntries() {
    Mission::TutorialSession::BeginLessonRegistryRefresh();
    std::vector<PracticeMenu::TitleScreen::MissionInfo> out;
    const std::string root = Mission::ResolveMissionsRoot();
    int libraryErrorCount = 0;
    std::string firstLibraryError;
    auto recordLibraryError = [&](const std::string& path,
                                  const std::string& error) {
        ++libraryErrorCount;
        const std::string detail = path + ": " +
            (error.empty() ? std::string("unknown parse error") : error);
        if (firstLibraryError.empty()) firstLibraryError = detail;
        LogOut("[PRACTICE_MENU] library entry skipped: " + detail, true);
    };
    auto parseOne = [&](const std::string& path,
                        const std::string& source,
                        const std::string& author,
                        const std::string& scenarioDescription,
                        bool locked,
                        bool recorded,
                        const std::string& packId = std::string()) {
        Mission::Mission mi;
        std::string err;
        if (!Mission::LoadMission(path, mi, err)) {
            recordLibraryError(path, err);
            return;
        }
        PracticeMenu::TitleScreen::MissionInfo e;
        const size_t slash = path.find_last_of("\\/");
        e.name = !mi.name.empty() ? mi.name
               : (slash != std::string::npos ? path.substr(slash + 1) : path);
        e.path = path;
        e.description = !mi.description.empty() ? mi.description : scenarioDescription;
        e.type = mi.type;
        e.character = mi.player.character;
        e.dummy = mi.dummy.character;
        e.source = source;
        e.packId = packId;
        e.author = author;
        e.category = mi.category;
        e.difficulty = mi.difficulty;
        e.order = mi.order;
        e.lessonId = mi.lessonId;
        e.recommendedAfter = mi.recommendedAfter;
        if (!mi.summary.empty()) e.description = mi.summary;   // YOU'LL PRACTICE line
        if (mi.tutorialSchema > 0) {
            // Capability preflight (§5.7): unsupported requirement = visible
            // UNAVAILABLE row with a plain reason, never a silent approximation.
            const auto issues = Mission::Tutorial::RuntimeSupportIssues(mi);
            if (!issues.empty()) {
                e.unavailableReason = PlayerFacingUnavailableReason(issues);
            }
            if (!packId.empty() && !mi.lessonId.empty()) {
                const Mission::Tutorial::LessonProgress progress =
                    Mission::Tutorial::ProgressGet(packId, mi.lessonId);
                e.cleared = progress.cleared;
                e.updated = progress.cleared &&
                    progress.revisionCleared < mi.revision;
            }
            if (!mi.lessonId.empty()) {
                Mission::TutorialSession::RegisterLessonPath(
                    packId, mi.lessonId, path, mi.name, mi.nextLessonId,
                    e.unavailableReason.empty());
            }
        }
        e.steps = static_cast<int>(mi.hasLesson
                                       ? mi.lesson.tasks.size()
                                       : mi.steps.size());
        e.stage = mi.stage;
        e.bgm = mi.bgm;
        e.recorded = recorded;
        e.locked = locked;
        e.hasDemo = !mi.demo.empty();
        e.hasSavestate = !mi.savestate.empty();
        if (!mi.hints.empty()) e.hint = mi.hints.front();
        // Neutral normals show the button only, matching the in-match glyph strip
        // (SplitNotation): skip a lowercase position prefix (j./c./f.), then drop a
        // leading neutral '5' when something follows it ("5A"->"A", "c.5B"->"c.B").
        auto stripNeutral = [](const std::string& n) -> std::string {
            size_t i = 0;
            while (i < n.size() && ((n[i] >= 'a' && n[i] <= 'z') || n[i] == '.')) ++i;
            if (i < n.size() && n[i] == '5' && i + 1 < n.size() && n[i + 1] != ' ')
                return n.substr(0, i) + n.substr(i + 1);
            return n;
        };
        for (const Mission::Step& step : mi.steps) {
            if (!e.recipe.empty()) e.recipe += "  >  ";
            if (!step.notation.empty()) e.recipe += stripNeutral(step.notation);
            else if (!step.moveIds.empty()) e.recipe += "#" + std::to_string(step.moveIds.front());
            else e.recipe += "?";
        }
        out.push_back(std::move(e));
    };
    std::vector<std::pair<std::string, std::string>> tutorialCats;
    if (!root.empty()) {
        for (const std::string& pj : Mission::DiscoverPackJsonPaths(root)) {
            Mission::Pack pack; std::string err;
            if (!Mission::LoadPack(pj, pack, err)) {
                recordLibraryError(pj, err);
                continue;
            }
            const std::string src = pack.name.empty() ? std::string("PACK") : pack.name;
            if (!pack.categories.empty()) {
                std::vector<const Mission::PackCategory*> ordered;
                for (const auto& c : pack.categories) ordered.push_back(&c);
                std::stable_sort(ordered.begin(), ordered.end(),
                                 [](const Mission::PackCategory* a, const Mission::PackCategory* b) {
                                     return a->order < b->order;
                                 });
                for (const auto* c : ordered) tutorialCats.emplace_back(c->id, c->label);
            }
            for (const auto& sc : pack.scenarios) {
                parseOne(pack.folderPath + "\\" + sc.file,
                         src,
                         pack.author,
                         sc.description,
                         sc.locked,
                         false,
                         pack.id);
            }
        }
        WIN32_FIND_DATAA fd{};
        HANDLE h = FindFirstFileA((root + "\\_recorded\\*.json").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                parseOne(root + "\\_recorded\\" + fd.cFileName,
                         "RECORDED", "LOCAL CAPTURE", std::string(), false, true);
            } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
    }
    Mission::TutorialSession::FinishLessonRegistryRefresh();
    PracticeMenu::TitleScreen::SetTutorialCategories(std::move(tutorialCats));
    PracticeMenu::TitleScreen::SetMissions(std::move(out));
    if (libraryErrorCount > 0) {
        std::string message = "Session library skipped " +
            std::to_string(libraryErrorCount) +
            (libraryErrorCount == 1 ? " unreadable file" : " unreadable files");
        if (!firstLibraryError.empty()) message += "; see the log for details";
        DirectDrawHook::AddMessage(message.c_str(), "SESSION LIBRARY",
                                   RGB(255, 175, 120), 4200, 20, 96);
    }
}

// Consume the progress store's one-shot recovery warning only when Tutorial
// is actually entered. A prior Mission-browser scan may have lazily loaded the
// store through ProgressGet; tutorial_support preserves the warning for here.
void SurfaceTutorialProgressWarning() {
    std::string warning;
    if (Mission::Tutorial::ProgressLoad(warning) || warning.empty()) return;
    DirectDrawHook::AddMessage(
        "Tutorial progress was unreadable. It was reset, and the old file was kept as .bad.",
        "TUTORIAL", RGB(255, 175, 120), 5200, 20, 96);
    LogOut("[PRACTICE_MENU] " + warning, true);
}

// Confirm on the MISSIONS screen (own frame): queue the pick for after match
// entry. Returns false when nothing is selected.
bool QueueSelectedMission(bool& directLoading) {
    directLoading = false;
    const std::string path = PracticeMenu::TitleScreen::SelectedMissionPath();
    if (path.empty()) return false;
    std::string error;
    if (!Mission::Engine::SetPendingMissionLoad(path, error)) {
        if (error.empty()) error = "the selected session could not be prepared";
        const bool tutorial = PracticeMenu::TitleScreen::Current() ==
                              PracticeMenu::TitleScreen::Screen::Tutorial;
        const std::string message = std::string(tutorial ? "Lesson" : "Mission") +
                                    " could not start: " + error;
        DirectDrawHook::AddMessage(message.c_str(),
                                   tutorial ? "TUTORIAL" : "MISSION",
                                   RGB(255, 150, 120), 3000, 20, 96);
        LogOut("[PRACTICE_MENU] selected launch rejected before leaving browser: " +
               error, true);
        return false;
    }
    directLoading = CharacterHotswap::IsDirectPracticeLoadPending();
    return true;
}

} // namespace

// Entry point invoked from the naked thunk when Practice is confirmed. Runs on
// the render thread, mid-updateTitleScreenLogic. Keep it minimal + guarded.
extern "C" void __cdecl PracticeMenu_EnterFromCase(uint32_t screenContext) {
    // No C++ objects requiring unwinding may live in a function that uses __try
    // (C2712): keep string building inside the Log() helper's own frame.
    __try {
        g_lastScreenContext = screenContext;
        g_selection = PracticeMenu::Render::ROW_PRACTICE;   // enter on the top row
        PracticeMenu::TitleScreen::Close();
        g_animT = 0.0f;                 // start off-screen, slide in
        g_needPaletteApply.store(true); // our palette was restored to vanilla on last exit
        // Assume the confirm button is still held on entry so it does not
        // immediately re-trigger inside the submenu.
        g_prevInput = InputState{0, 0, true, false};
        g_phase.store(PHASE_ENTERING);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_phase.store(PHASE_INACTIVE);
    }
    if (g_phase.load() != PHASE_INACTIVE) Log("Entered submenu from title Practice");
}

namespace {

// Naked thunk mirroring InGameNetplay's NetplayCaseThunk: read the screenContext
// local ([ebp-8] within updateTitleScreenLogic's frame), enter the submenu, then
// return 0 through the confirm-switch epilogue so no fade/mode-change happens.
__declspec(naked) void PracticeCaseThunk() {
    __asm {
        mov eax, dword ptr [ebp-8]
        push eax
        call PracticeMenu_EnterFromCase
        add esp, 4
        mov al, 0
        mov edx, dword ptr [g_titleEpilogueAddr]
        jmp edx
    }
}

// ---- input ----------------------------------------------------------------
InputState ReadInput(uint32_t gc) {
    InputState s;
    if (!gc) return s;
    const int8_t h1 = *reinterpret_cast<int8_t*>(gc + kGcHorizBase);      // P1
    const int8_t h2 = *reinterpret_cast<int8_t*>(gc + kGcHorizBase + 1);  // P2
    const int h = (h1 != 0) ? h1 : h2;
    s.horiz = (h < 0) ? -1 : (h > 0 ? 1 : 0);
    const int8_t v1 = *reinterpret_cast<int8_t*>(gc + kGcVertBase);      // P1
    const int8_t v2 = *reinterpret_cast<int8_t*>(gc + kGcVertBase + 1);  // P2
    const int v = (v1 != 0) ? v1 : v2;
    s.vert = (v < 0) ? -1 : (v > 0 ? 1 : 0);
    s.confirm = (*reinterpret_cast<uint8_t*>(gc + kGcConfirmBase) != 0) ||
                (*reinterpret_cast<uint8_t*>(gc + kGcConfirmBase + 1) != 0);
    s.cancel  = (*reinterpret_cast<uint8_t*>(gc + kGcCancelBase) != 0) ||
                (*reinterpret_cast<uint8_t*>(gc + kGcCancelBase + 1) != 0);
    return s;
}

// ---- actions --------------------------------------------------------------
// Begin the slide-out; the frame loop finishes it (FinishLeave) once the
// animation reaches 0, then hands control back to the vanilla title menu.
void BeginLeave() {
    g_phase.store(PHASE_LEAVING);
}
void FinishLeave(uint32_t sc) {
    PracticeMenu::TitleScreen::Close();
    PracticeMenu::Render::Release(sc);
    g_phase.store(PHASE_INACTIVE);
    Log("Submenu closed -> title menu");
}

// Replicate the vanilla Practice case (efz_memorial_latest.c case 3) BYTE-FOR-
// BYTE, including the confirm SFX and the blocking sound-adjusted fade-out. This
// matters: skipping the fade left the practice-match BGM starting ~1s late. The
// fade is a synchronous ~21-frame transition, exactly as vanilla does it.
void LaunchPractice(uint32_t sc, bool directLoading = false) {
    // Restore the vanilla title palette BEFORE the fade. fadeWithSoundAdjustment
    // re-renders the title menu (from the +1080 objects surface) on each fade
    // step; if our overlay palette (slots 193+) is still applied, that menu fades
    // out with broken colors (the "broken title_ob palette" transition).
    PracticeMenu::Render::Release(sc);
    __try {
        const uint32_t gc = GameContext(sc);
        if (gc) {
            reinterpret_cast<PlaySfxFn>(Resolve(kVaPlaySfx))(
                reinterpret_cast<void*>(gc), kSfxConfirm);
            *reinterpret_cast<uint8_t*>(gc + kGcP1Type) = 0;      // P1 human
            *reinterpret_cast<uint8_t*>(gc + kGcP2Type) = 1;      // P2 CPU/dummy
            *reinterpret_cast<uint8_t*>(gc + kGcActivePlayer) = 0;
            *reinterpret_cast<uint8_t*>(gc + kGcGameMode) = 1;    // Practice
            *reinterpret_cast<uint8_t*>(gc + kGcRoundCount) = 2;
        }
        reinterpret_cast<FadeFn>(Resolve(kVaFade))(
            reinterpret_cast<void*>(sc), static_cast<int>(sc + kOffPalette), 1, 0, 0);
        *reinterpret_cast<uint8_t*>(sc + kOffLifecycle) = 2;      // title re-entry fade state
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    Log(directLoading
        ? "Launching mission/tutorial (mode=1 -> direct Loading)"
        : "Launching Practice (mode=1 -> Character Select)");
}

// Returns the update return code to use (>=0), or -1 to stay in the submenu.
int Activate(uint32_t sc) {
    switch (g_selection) {
        case PracticeMenu::Render::ROW_PRACTICE:
            LaunchPractice(sc); // restores vanilla palette, then fades to char select
            g_phase.store(PHASE_INACTIVE);
            return 1; // go to character select
        case PracticeMenu::Render::ROW_TUTORIAL:
            // Tutorial lessons use the same data-backed library as missions,
            // filtered to type="tutorial" by the dedicated screen.
            SurfaceTutorialProgressWarning();
            PopulateMissionEntries();
            PracticeMenu::TitleScreen::Open(PracticeMenu::TitleScreen::Screen::Tutorial);
            Log("Tutorial screen opened");
            return -1;
        case PracticeMenu::Render::ROW_MISSION:
            // Opens its own SCREEN listing packs + recorded missions.
            PopulateMissionEntries();
            PracticeMenu::TitleScreen::Open(PracticeMenu::TitleScreen::Screen::Missions);
            Log("Missions screen opened");
            return -1;

        default:
            return -1;
    }
}

// ---- self-render ----------------------------------------------------------
// While the submenu owns the frame we draw AND present it ourselves. Critically
// the vanilla title render (renderReplaySelectionScreen) both draws the menu and
// calls presentFrameToScreen - so if we don't present, nothing reaches the
// window and the last vanilla frame stays frozen. We deliberately do NOT call
// the vanilla render (it would re-draw the 8-row main menu we're replacing);
// instead we blit the title background (+1076: logo/sky, no menu), our rows at
// the current slide offset, then present.
BOOL CallPresentGuarded(uint32_t gfxCtx) {
    const uintptr_t presentVa = Resolve(kVaPresent);
    if (!presentVa) return FALSE;
    BOOL r = FALSE;
    __try {
        r = reinterpret_cast<PresentFn>(presentVa)(static_cast<int>(gfxCtx));
    } __except (EXCEPTION_EXECUTE_HANDLER) { r = FALSE; }
    return r;
}

void RenderAndPresent(uint32_t sc) {
    PracticeMenu::Render::DrawBackground(sc);
    // Re-apply our sheet's palette on the first frame of each entry (exit
    // restored the vanilla palette). MUST come AFTER DrawBackground: calling
    // loadCompressedImageFile/setPalette before this frame's first blit disrupts
    // the blit target (the vanilla frame stays and shows washed-out). The
    // palette is applied at present time, so the order vs the blits is
    // color-correct either way; what matters is not perturbing the blits.
    if (g_needPaletteApply.exchange(false)) {
        PracticeMenu::Render::ApplyPalette(sc);
    }
    // While a dedicated screen (MISSIONS/TUTORIAL) is up, the submenu's sprite
    // rows hide - the screen renders per-EndScene over the title backdrop.
    if (!PracticeMenu::TitleScreen::Active()) {
        PracticeMenu::Render::DrawRows(sc, g_selection, SlideOffsetPx());
    }
    const uint32_t gfxCtx = GraphicsSystem(sc);
    const BOOL presentRet = CallPresentGuarded(gfxCtx);

    // Throttled diagnostic (~1/sec), only when detailed logging is enabled.
    static DWORD s_lastLog = 0;
    const DWORD now = GetTickCount();
    if (detailedLogging.load() && now - s_lastLog > 1000) {
        s_lastLog = now;
        char buf[256];
        _snprintf_s(buf, sizeof(buf), _TRUNCATE,
            "render phase=%d animT=%.2f off=%d gfx=0x%08X bg=0x%08X obj=0x%08X loaded=%d present=%d",
            g_phase.load(), g_animT, SlideOffsetPx(), gfxCtx,
            sc ? *reinterpret_cast<uint32_t*>(sc + kOffBgSurface) : 0u,
            sc ? *reinterpret_cast<uint32_t*>(sc + kOffObjSurface) : 0u,
            PracticeMenu::Render::IsLoaded() ? 1 : 0, static_cast<int>(presentRet));
        Log(buf);
    }
}

// Consume a deferred reopen request: enter the submenu directly on the
// requested browser screen (no slide - the player is returning, not arriving).
// Own frame: C++ objects are safe here; the guarded state writes live in a
// separate __try helper.
void ReopenGuardedStateEnter(uint32_t sc) {
    __try {
        g_lastScreenContext = sc;
        g_animT = 1.0f;
        g_needPaletteApply.store(true);
        g_prevInput = InputState{0, 0, true, true};
        g_phase.store(PHASE_ACTIVE);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_phase.store(PHASE_INACTIVE);
    }
}

void ConsumeReopenRequest(uint32_t sc) {
    const int reopen = g_pendingReopen.exchange(0);
    if (reopen == 0) return;
    const auto screen = static_cast<PracticeMenu::TitleScreen::Screen>(reopen);
    g_selection = screen == PracticeMenu::TitleScreen::Screen::Tutorial
                ? PracticeMenu::Render::ROW_TUTORIAL
                : PracticeMenu::Render::ROW_MISSION;
    ReopenGuardedStateEnter(sc);
    if (g_phase.load() != PHASE_INACTIVE) {
        if (screen == PracticeMenu::TitleScreen::Screen::Tutorial) {
            SurfaceTutorialProgressWarning();
        }
        PopulateMissionEntries();
        PracticeMenu::TitleScreen::Open(
            screen, screen == PracticeMenu::TitleScreen::Screen::Tutorial);
        Log("Browser reopened after session exit");
    }
}

// ---- update detour --------------------------------------------------------
char __fastcall UpdateDetour(uint32_t sc, void* /*edx*/) {
    int phase = g_phase.load();
    if (phase == PHASE_INACTIVE) {
        ConsumeReopenRequest(sc);
        phase = g_phase.load();
        if (phase == PHASE_INACTIVE) {
            return g_origUpdate ? g_origUpdate(sc) : 0;
        }
    }

    __try {
        g_lastScreenContext = sc;

        if (phase == PHASE_ENTERING) {
            g_animT += kAnimStep;
            if (g_animT >= 1.0f) { g_animT = 1.0f; g_phase.store(PHASE_ACTIVE); }
        } else if (phase == PHASE_LEAVING) {
            g_animT -= kAnimStep;
            if (g_animT <= 0.0f) {
                g_animT = 0.0f;
                FinishLeave(sc);
                // Hand this frame back to the vanilla title so its menu renders
                // and presents immediately (no blank flash).
                return g_origUpdate ? g_origUpdate(sc) : 0;
            }
        } else { // PHASE_ACTIVE - the only phase that consumes input
            const uint32_t gc = GameContext(sc);
            if (gc) {
                reinterpret_cast<ProcessInputFn>(Resolve(kVaProcessPlayerInput))(reinterpret_cast<void*>(gc));
                const InputState cur = ReadInput(gc);

                // Navigation: edge + HOLD AUTO-REPEAT (a long mission list is
                // unusable with tap-only). ~0.3s initial delay, then ~10/sec.
                static int s_vertHold = 0;
                if (cur.vert != 0 && cur.vert == g_prevInput.vert) ++s_vertHold;
                else s_vertHold = 0;
                int vertEdge = (g_prevInput.vert == 0 && cur.vert != 0) ? cur.vert : 0;
                if (vertEdge == 0 && cur.vert != 0 &&
                    s_vertHold >= 18 && ((s_vertHold - 18) % 6) == 0) {
                    vertEdge = cur.vert;   // auto-repeat while held
                }
                const bool confirmEdge = (!g_prevInput.confirm && cur.confirm);
                const bool cancelEdge  = (!g_prevInput.cancel && cur.cancel);
                const int horizEdge = (g_prevInput.horiz == 0 && cur.horiz != 0) ? cur.horiz : 0;
                g_prevInput = cur;

                // ESC mirrors the cancel button: slide back out to the title menu.
                static bool s_prevEsc = false;
                const bool escDown = (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
                const bool escEdge = escDown && !s_prevEsc;
                s_prevEsc = escDown;

                if (PracticeMenu::TitleScreen::Active()) {
                    // A dedicated screen (MISSIONS / TUTORIAL) owns input.
                    // Confirm/cancel take PRIORITY over navigation so a held
                    // direction can never delay activation.
                    if (confirmEdge) {
                        const auto act = PracticeMenu::TitleScreen::Confirm();
                        bool directLoading = false;
                        if (act == PracticeMenu::TitleScreen::ConfirmAction::Launch &&
                            QueueSelectedMission(directLoading)) {
                            PracticeMenu::TitleScreen::Close();
                            LaunchPractice(sc, directLoading);
                            g_phase.store(PHASE_INACTIVE);
                            // Replay PLAY proves that returning 2 is the native
                            // supported route into Loading. Our one-shot Loading
                            // hook supplies the two fighter objects Practice
                            // normally receives from Character Select.
                            return directLoading ? 2 : 1;
                        }
                        if (act == PracticeMenu::TitleScreen::ConfirmAction::Record) {
                            // CREATE > RECORD NEW SESSION: launch practice with
                            // recording pending - the player picks characters at
                            // select, then the recorder enters PRE-RECORD once
                            // the match settles. Macro Record starts/stops;
                            // Review owns explicit save/retake/discard.
                            Mission::Engine::SetPendingRecordMode(true);
                            PracticeMenu::TitleScreen::Close();
                            LaunchPractice(sc);
                            g_phase.store(PHASE_INACTIVE);
                            return 1;
                        }
                    } else if (cancelEdge || escEdge) {
                        // Walks up one level first; closes only from the top.
                        if (!PracticeMenu::TitleScreen::Back()) {
                            PracticeMenu::TitleScreen::Close();
                        }
                    } else if (horizEdge != 0) {
                        reinterpret_cast<PlaySfxFn>(Resolve(kVaPlaySfx))(
                            reinterpret_cast<void*>(gc), kSfxCursor);
                        PracticeMenu::TitleScreen::MoveTab(horizEdge);
                    } else if (vertEdge != 0) {
                        reinterpret_cast<PlaySfxFn>(Resolve(kVaPlaySfx))(
                            reinterpret_cast<void*>(gc), kSfxCursor);
                        PracticeMenu::TitleScreen::MoveSelection(vertEdge);
                    }
                } else if (confirmEdge) {
                    const int r = Activate(sc);
                    if (r >= 0) return static_cast<char>(r); // e.g. Practice -> char select
                } else if (cancelEdge || escEdge) {
                    BeginLeave(); // start slide-out; keep rendering this frame
                } else if (vertEdge != 0) {
                    g_selection += vertEdge;
                    if (g_selection < 0) g_selection = PracticeMenu::Render::ROW_COUNT - 1;
                    if (g_selection >= PracticeMenu::Render::ROW_COUNT) g_selection = 0;
                }
            }
        }

        RenderAndPresent(sc);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // SetPendingMissionLoad and the direct loader are separate owners. A
        // fault after mission selection but before this detour returns Loading
        // must retire both, otherwise the selected path silently launches on a
        // later manual Practice entry. The MissionEngine API preserves generic
        // browser-only pending mode when no concrete path belongs to this turn.
        Mission::Engine::CancelPendingMissionLoad("title update exception");
        CharacterHotswap::CancelDirectPracticeLoad("title update exception");
        g_phase.store(PHASE_INACTIVE);
    }
    return 0;
}

// ---- install helpers ------------------------------------------------------
bool VerifyVanillaTitle() {
    const uintptr_t updateVa = Resolve(kVaUpdateTitle);
    if (!updateVa) return false;
    static const uint8_t kExpected[6] = {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x0C}; // push ebp; mov ebp,esp; sub esp,0Ch
    uint8_t got[6] = {0};
    if (!SafeReadMemory(updateVa, got, sizeof(got))) return false;
    return std::memcmp(got, kExpected, sizeof(kExpected)) == 0;
}

bool PatchPracticeCase() {
    g_practiceCaseAddr = Resolve(kVaCasePractice);
    if (!g_practiceCaseAddr) return false;
    if (!SafeReadMemory(g_practiceCaseAddr, g_practiceCaseOriginal, sizeof(g_practiceCaseOriginal))) return false;

    uint8_t patch[5];
    patch[0] = 0xE9; // jmp rel32
    const int32_t rel = static_cast<int32_t>(
        reinterpret_cast<uintptr_t>(&PracticeCaseThunk) - (g_practiceCaseAddr + 5));
    std::memcpy(&patch[1], &rel, sizeof(rel));

    DWORD oldProtect = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(g_practiceCaseAddr), sizeof(patch), PAGE_EXECUTE_READWRITE, &oldProtect))
        return false;
    std::memcpy(reinterpret_cast<void*>(g_practiceCaseAddr), patch, sizeof(patch));
    VirtualProtect(reinterpret_cast<void*>(g_practiceCaseAddr), sizeof(patch), oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(g_practiceCaseAddr), sizeof(patch));
    g_practiceCasePatched = true;
    return true;
}

void UnpatchPracticeCase() {
    if (!g_practiceCasePatched || !g_practiceCaseAddr) return;
    DWORD oldProtect = 0;
    if (VirtualProtect(reinterpret_cast<void*>(g_practiceCaseAddr), sizeof(g_practiceCaseOriginal),
                       PAGE_EXECUTE_READWRITE, &oldProtect)) {
        std::memcpy(reinterpret_cast<void*>(g_practiceCaseAddr), g_practiceCaseOriginal, sizeof(g_practiceCaseOriginal));
        VirtualProtect(reinterpret_cast<void*>(g_practiceCaseAddr), sizeof(g_practiceCaseOriginal), oldProtect, &oldProtect);
        FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(g_practiceCaseAddr), sizeof(g_practiceCaseOriginal));
    }
    g_practiceCasePatched = false;
}

} // namespace

namespace PracticeMenu {

bool Install() {
    if (g_installed.load()) return true;

    if (!VerifyVanillaTitle()) {
        Log("Vanilla title code not recognized; skipping (Practice stays vanilla)");
        return false;
    }

    g_titleEpilogueAddr = Resolve(kVaTitleEpilogue);
    if (!g_titleEpilogueAddr) { Log("Failed to resolve title epilogue"); return false; }

    const uintptr_t updateVa = Resolve(kVaUpdateTitle);
    if (!MinHookUtils::CreateAndEnableHook(reinterpret_cast<void*>(updateVa),
                                           reinterpret_cast<void*>(&UpdateDetour),
                                           reinterpret_cast<void**>(&g_origUpdate),
                                           "PracticeMenu", "updateTitleScreenLogic")) {
        Log("Failed to hook updateTitleScreenLogic");
        return false;
    }

    if (!PatchPracticeCase()) {
        Log("Failed to patch Practice case; rolling back update hook");
        MinHookUtils::RemoveHook(reinterpret_cast<void*>(updateVa), "PracticeMenu", "updateTitleScreenLogic");
        g_origUpdate = nullptr;
        return false;
    }

    if (!CharacterHotswap::InstallDirectPracticeBootstrap()) {
        Log("Direct Loading bootstrap unavailable; missions retain Character Select fallback");
    }

    g_installed.store(true);
    Log("Installed (Practice -> submenu; coexists with InGameNetplay)");
    return true;
}

void Uninstall() {
    if (!g_installed.load()) return;
    g_phase.store(PHASE_INACTIVE);
    Mission::Engine::CancelPendingMissionLoad("Practice menu uninstall");
    CharacterHotswap::CancelDirectPracticeLoad("Practice menu uninstall");
    CharacterHotswap::UninstallDirectPracticeBootstrap();
    UnpatchPracticeCase();
    const uintptr_t updateVa = Resolve(kVaUpdateTitle);
    if (updateVa) MinHookUtils::RemoveHook(reinterpret_cast<void*>(updateVa), "PracticeMenu", "updateTitleScreenLogic");
    g_origUpdate = nullptr;
    if (g_lastScreenContext) Render::Release(g_lastScreenContext);
    g_installed.store(false);
    Log("Uninstalled");
}

bool IsSubmenuActive() { return g_phase.load() != PHASE_INACTIVE; }

void RequestTitleReopen(TitleScreen::Screen screen) {
    g_pendingReopen.store(static_cast<int>(screen));
}

} // namespace PracticeMenu
