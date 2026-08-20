#include "../../../include/game/mission/mission_pause_menu.h"

#include "../../../include/game/mission/mission_data.h"
#include "../../../include/game/mission/mission_engine.h"
#include "../../../include/game/mission/tutorial_session.h"
#include "../../../include/game/game_state.h"
#include "../../../include/game/savestate_hook.h"
#include "../../../include/game/practice_menu/practice_menu.h"
#include "../../../include/gui/custom_menu/layout.h"
#include "../../../include/gui/custom_menu/theme.h"
#include "../../../include/gui/custom_menu/scale.h"
#include "../../../include/gui/custom_menu/screens.h"
#include "../../../include/gui/gui.h"
#include "../../../include/gui/imgui_impl.h"
#include "../../../include/gui/overlay.h"
#include "../../../include/core/logger.h"
#include "../../../include/utils/config.h"
#include "../../../include/utils/pause_integration.h"
#include "../../../include/utils/utilities.h"
#include "../../../include/utils/xinput_shim.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

#include <windows.h>

namespace Mission::PauseMenu {

namespace {

namespace L = CustomMenu::Layout;
namespace T = CustomMenu::Theme;
namespace Rec = ::Mission::Engine::Recorder;

enum class Context : int {
    Mission = 0,   // trial / scenario runner session
    Lesson,        // tutorial runner session
    PreRecord,     // recorder setup is live; no capture yet
    CountIn,       // recorder countdown owns the session
    Recording,     // recorder capture owns the session
    Review,        // sealed recorder take awaits authoring decisions
};

enum class Item : int {
    Resume = 0,
    Retry,             // mission retry / lesson restart
    RetryTask,         // lesson: retry the CURRENT task from its checkpoint
    ReviewLesson,      // lesson: re-read the pages, then return to the task
    NextLesson,        // lesson: after clear only
    WatchDemo,
    ReturnToBrowser,
    StartRecording,
    OpenAuthoring,
    OpenPractice,
    CancelCountdown,
    StopReview,
    PreviewRecording,
    RetakeRecording,
    DiscardRecording,
};

struct State {
    bool open = false;
    Context context = Context::Mission;
    int selection = 0;
    std::string title;         // panel title (MISSION PAUSED / ...)
    std::string sessionName;
    std::string progress;      // STEP X OF N | ATTEMPT n / N ACTIONS CAPTURED
    std::string nextLessonName;
    std::vector<Item> items;
    bool hasDemo = false;
    bool captureSuspensionOwned = false;
    bool confirming = false;
    Item confirmationItem = Item::DiscardRecording;
    int confirmationSelection = 0; // safe KEEP choice is always first
    // Retained for close-path compatibility; new opens always publish their
    // own nested MissionPause surface so tutorial refresh/teardown cannot
    // unpause underneath this menu.
    bool borrowedTutorialFreeze = false;
};

std::mutex g_mx;
State g_state;
std::atomic<bool> g_openFlag{false};   // lock-free fast path for gates
// STOP & REVIEW must briefly resume to seal one post-request macro boundary.
// Once the recorder publishes Review, reopen this dedicated surface rather
// than falling into the ordinary Practice menu.
std::atomic<bool> g_pendingReviewPause{false};
std::atomic<bool> g_inputSyncRequested{false};

// Trial/Tutorial pause rows share the title browser's restrained EFZ menu
// language: hard black strips, a single white separator between rows, and a
// quiet steel-white focus lift. Recorder pause contexts keep their existing
// authoring presentation.
constexpr ImU32 kSessionRowRule    = IM_COL32(255, 255, 255, 78);
constexpr ImU32 kSessionRowRuleHot = IM_COL32(255, 255, 255, 210);

std::string Upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

bool IsRecorderContext(Context c) {
    return c == Context::PreRecord || c == Context::CountIn ||
           c == Context::Recording || c == Context::Review;
}

bool IsCaptureContext(Context c) {
    return c == Context::CountIn || c == Context::Recording;
}

const char* ItemLabel(Item item, Context ctx) {
    const bool lesson = ctx == Context::Lesson;
    switch (item) {
        case Item::Resume:
            return ctx == Context::PreRecord ? "RESUME SETUP"
                 : ctx == Context::CountIn ? "RESUME COUNTDOWN"
                 : ctx == Context::Recording ? "RESUME RECORDING"
                 : ctx == Context::Review ? "RETURN TO MATCH"
                 : (lesson ? "RESUME LESSON" : "RESUME");
        case Item::Retry:           return lesson ? "RESTART LESSON" : "RETRY MISSION";
        case Item::RetryTask:       return "RETRY CURRENT TASK";
        case Item::ReviewLesson:    return "REVIEW LESSON";
        case Item::NextLesson:      return "NEXT LESSON";
        case Item::WatchDemo:       return lesson ? "WATCH EXAMPLE" : "WATCH DEMONSTRATION";
        case Item::ReturnToBrowser: return lesson ? "RETURN TO LESSONS" : "RETURN TO MISSIONS";
        case Item::StartRecording:  return "START RECORDING";
        case Item::OpenAuthoring:   return ctx == Context::Review
                                                ? "REVIEW & SAVE"
                                                : "RECORD & AUTHOR";
        case Item::OpenPractice:    return "PRACTICE SETTINGS";
        case Item::CancelCountdown: return "CANCEL COUNTDOWN";
        case Item::StopReview:      return "STOP & REVIEW";
        case Item::PreviewRecording:return "PREVIEW DEMONSTRATION";
        case Item::RetakeRecording: return "RETAKE FROM BASELINE";
        case Item::DiscardRecording:return "DISCARD RECORDING";
    }
    return "";
}

const char* ItemHint(Item item, Context ctx) {
    const bool lesson = ctx == Context::Lesson;
    switch (item) {
        case Item::Resume:
            return ctx == Context::PreRecord
                       ? "Return to the match and finish arranging the recording setup."
                       : ctx == Context::CountIn
                       ? "Return to the countdown; release the controls to start."
                       : ctx == Context::Recording
                       ? "Continue capturing from exactly where you paused."
                       : ctx == Context::Review
                       ? "Close this menu without changing the saved take."
                       : "Continue from exactly where you paused.";
        case Item::Retry:
            return lesson ? "Restore the lesson's start and try again."
                          : "Restore the mission's start and try again.";
        case Item::RetryTask:
            return "Restore this task's checkpoint without losing earlier tasks.";
        case Item::ReviewLesson:
            return "Re-read the lesson pages, then return to the same task.";
        case Item::NextLesson:
            return "Load the next lesson in the course.";
        case Item::WatchDemo:
            return "Restore the start and watch the authored example.";
        case Item::ReturnToBrowser:
            return lesson ? "Leave this lesson and return to the lesson browser."
                          : "Leave this mission and return to the mission browser.";
        case Item::StartRecording:
            return "Lock the prepared start after the configured count-in, then begin capturing.";
        case Item::OpenAuthoring:
            return ctx == Context::Review
                       ? "Open the authoring form to name, preview, save, or publish this take."
                       : "Open the dedicated recording and pack workshop.";
        case Item::OpenPractice:
            return "Open the normal Practice menu. The recording stays paused; Back returns here.";
        case Item::CancelCountdown:
            return "Return to PRE-RECORD; nothing has been captured yet.";
        case Item::StopReview:
            return "Seal the take, then open the dedicated recording review menu.";
        case Item::PreviewRecording:
            return "Restore the recorded start and watch the captured input demonstration.";
        case Item::RetakeRecording:
            return "Discard this take, restore its exact start, and return to PRE-RECORD.";
        case Item::DiscardRecording:
            return "Throw away this authoring session entirely.";
    }
    return "";
}

Context RecorderContextForPhase(Rec::Phase phase) {
    using Phase = Rec::Phase;
    switch (phase) {
        case Phase::PreRecord: return Context::PreRecord;
        case Phase::CountIn:   return Context::CountIn;
        case Phase::Recording: return Context::Recording;
        case Phase::Review:    return Context::Review;
        case Phase::Idle:
        default:               return Context::Mission;
    }
}

bool ConfigureRecorderStateLocked(Rec::Phase phase) {
    if (!Rec::UsesDedicatedPauseMenu(phase)) return false;

    g_state.context = RecorderContextForPhase(phase);
    g_state.sessionName = "NEW RECORDING";
    g_state.items.clear();
    g_state.selection = 0;
    g_state.confirming = false;
    g_state.confirmationSelection = 0;

    const int steps = Rec::GetStepCount();
    const int breaks = Rec::GetComboEndCount();
    char progress[96] = {};
    switch (g_state.context) {
        case Context::PreRecord:
            g_state.title = "RECORDING SETUP";
            g_state.progress = "PRE-RECORD  |  NOTHING CAPTURED YET";
            g_state.items.push_back(Item::Resume);
            g_state.items.push_back(Item::StartRecording);
            g_state.items.push_back(Item::OpenAuthoring);
            g_state.items.push_back(Item::OpenPractice);
            g_state.items.push_back(Item::DiscardRecording);
            break;
        case Context::CountIn:
            g_state.title = "COUNTDOWN PAUSED";
            g_state.progress = "COUNTDOWN  |  NOTHING CAPTURED YET";
            g_state.items.push_back(Item::Resume);
            g_state.items.push_back(Item::CancelCountdown);
            g_state.items.push_back(Item::OpenPractice);
            g_state.items.push_back(Item::DiscardRecording);
            break;
        case Context::Recording:
            g_state.title = "RECORDING PAUSED";
            _snprintf_s(progress, sizeof(progress), _TRUNCATE,
                        "%d ACTION%s  |  %d SETUP BREAK%s", steps,
                        steps == 1 ? "" : "S", breaks,
                        breaks == 1 ? "" : "S");
            g_state.progress = progress;
            g_state.items.push_back(Item::Resume);
            g_state.items.push_back(Item::StopReview);
            g_state.items.push_back(Item::OpenPractice);
            g_state.items.push_back(Item::DiscardRecording);
            break;
        case Context::Review:
            g_state.title = "RECORDING REVIEW";
            if (Rec::HasPreviewableTake()) {
                _snprintf_s(progress, sizeof(progress), _TRUNCATE,
                            "%d ACTION%s CAPTURED  |  %d SETUP BREAK%s", steps,
                            steps == 1 ? "" : "S", breaks,
                            breaks == 1 ? "" : "S");
            } else {
                _snprintf_s(progress, sizeof(progress), _TRUNCATE,
                            "%d ACTION%s  |  RETAKE RECOMMENDED", steps,
                            steps == 1 ? "" : "S");
            }
            g_state.progress = progress;
            g_state.items.push_back(Item::Resume);
            g_state.items.push_back(Item::OpenAuthoring);
            if (Rec::HasPreviewableTake()) {
                g_state.items.push_back(Item::PreviewRecording);
            }
            g_state.items.push_back(Item::RetakeRecording);
            g_state.items.push_back(Item::OpenPractice);
            g_state.items.push_back(Item::DiscardRecording);
            break;
        case Context::Mission:
        case Context::Lesson:
            return false;
    }
    return true;
}

// Keep a recorder menu coherent if its phase changes while a nested Practice
// surface is visible or while an asynchronous recorder command settles.  The
// visible MissionPause owner remains in place; only live capture phases own
// the recipe/macro suspension lease.
bool SyncRecorderContextLocked() {
    if (!g_state.open || !IsRecorderContext(g_state.context)) return true;

    const Rec::Phase phase = Rec::GetPhase();
    if (!Rec::UsesDedicatedPauseMenu(phase)) return false;

    const bool shouldOwnCapture = Rec::IsCapturePhase(phase);
    if (shouldOwnCapture != g_state.captureSuspensionOwned) {
        Rec::SetCaptureMenuOpen(shouldOwnCapture);
        g_state.captureSuspensionOwned = shouldOwnCapture;
    }

    if (g_state.context != RecorderContextForPhase(phase)) {
        return ConfigureRecorderStateLocked(phase);
    }
    return true;
}

bool NeedsConfirmation(Item item) {
    return item == Item::RetakeRecording ||
           item == Item::DiscardRecording;
}

bool UsesRecorderCommandHandoff(Item item) {
    return item == Item::StartRecording ||
           item == Item::CancelCountdown ||
           item == Item::StopReview ||
           item == Item::RetakeRecording ||
           item == Item::DiscardRecording;
}

const char* ConfirmationPrompt(Item item, Context ctx) {
    return item == Item::RetakeRecording
               ? "RETAKE THIS RECORDING?"
               : (ctx == Context::PreRecord || ctx == Context::CountIn
                      ? "DISCARD THIS SESSION?"
                      : "DISCARD THIS RECORDING?");
}

const char* ConfirmationHint(Item item, Context ctx) {
    if (item == Item::RetakeRecording) {
        return "The current take will be erased. Its exact start and mission details are kept.";
    }
    if (ctx == Context::PreRecord || ctx == Context::CountIn) {
        return "The prepared start and unsaved mission details will be erased. No take has been captured.";
    }
    if (ctx == Context::Recording) {
        return "The live capture and its unsaved mission details will be erased.";
    }
    return "The captured take and its unsaved mission details will be erased.";
}

const char* ConfirmationLabel(Item item, Context ctx, int selection) {
    if (selection == 0) {
        if (item == Item::RetakeRecording) return "KEEP THIS TAKE";
        return (ctx == Context::PreRecord || ctx == Context::CountIn)
                   ? "KEEP SESSION"
                   : (ctx == Context::Review ? "KEEP THIS TAKE"
                                             : "KEEP RECORDING");
    }
    return item == Item::RetakeRecording ? "RETAKE FROM BASELINE"
                                         : "DISCARD RECORDING";
}

void DrawSessionPauseRow(ImDrawList* dl, ImFont* font, float px,
                         float x, float y, float w, float h,
                         const char* label, bool selected) {
    if (!dl || !label || w <= 0.0f || h <= 0.0f) return;
    const float sx = CustomMenu::Scale::Snap(x);
    const float sy = CustomMenu::Scale::Snap(y);
    const float ex = CustomMenu::Scale::Snap(x + w);
    const float ey = CustomMenu::Scale::Snap(y + h);

    L::DrawSessionRowChrome(dl, x, y, w, h, selected);

    const float tw = L::MeasureTextW(font, px, label);
    const float tx = x + (w - tw) * 0.5f;
    const float ty = y + (h - px) * 0.5f;
    dl->PushClipRect(ImVec2(sx + 10.0f, sy), ImVec2(ex - 10.0f, ey), true);
    if (selected) {
        L::DrawOutlinedText(dl, font, px, tx, ty, T::kTextActive, label);
    } else {
        L::DrawString(dl, font, px, tx, ty, T::kTextInactive, label);
    }
    dl->PopClipRect();
}

float MeasureWrappedHeight(ImFont* font, float px, const char* text,
                           float wrapWidth) {
    if (!text || !*text) return 0.0f;
    if (font) {
        return font->CalcTextSizeA(px, 1000000.0f, wrapWidth, text).y;
    }
    return ImGui::CalcTextSize(text, nullptr, false, wrapWidth).y;
}

void DrawWrappedText(ImDrawList* dl, ImFont* font, float px,
                     float x, float y, float width, float height,
                     ImU32 color, const char* text) {
    if (!dl || !text || !*text || width <= 0.0f || height <= 0.0f) return;
    const ImVec2 pos(x, y);
    dl->PushClipRect(pos, ImVec2(x + width, y + height), true);
    if (font) {
        dl->AddText(font, px, pos, color, text, nullptr, width);
    } else {
        L::DrawString(dl, font, px, x, y, color, text);
    }
    dl->PopClipRect();
}

// Executes the confirmed row on the frame-monitor thread after the menu has
// closed.  A freeze owned by the menu is released first; a tutorial-owned
// freeze remains borrowed until the selected session command transitions it.
void Execute(Item item, Context ctx) {
    switch (item) {
        case Item::Resume:
            break;
        case Item::Retry:
            if (::Mission::TutorialSession::IsActive()) {
                if (::Mission::TutorialSession::CommandRestartLesson()) {
                    LogOut("[MISSION][PAUSE] lesson restarted via session", true);
                } else {
                    DirectDrawHook::AddMessage(
                        "Restart unavailable - return to Lessons and launch it again",
                        "TUTORIAL", RGB(255, 180, 120), 2400, 0, 120);
                    LogOut("[MISSION][PAUSE] lesson restart refused: no usable baseline",
                           true);
                }
                break;
            }
            // Never call the raw Revival slot: it may belong to the user or a
            // previous session. Runner proves that this mission captured it.
            {
                std::string restoreMessage;
                if (::Mission::Engine::Runner::RequestBaselineRestore(restoreMessage)) {
                    LogOut("[MISSION][PAUSE] retry via runner-owned baseline", true);
                } else {
                    DirectDrawHook::AddMessage(
                        ("Retry unavailable: " + restoreMessage).c_str(), "MISSION",
                        RGB(255, 180, 120), 2200, 0, 120);
                    LogOut("[MISSION][PAUSE] retry refused: " + restoreMessage, true);
                }
            }
            break;
        case Item::RetryTask:
            ::Mission::TutorialSession::CommandRetryTask();
            break;
        case Item::ReviewLesson:
            ::Mission::TutorialSession::CommandReviewLesson();
            break;
        case Item::NextLesson: {
            std::string msg;
            ::Mission::TutorialSession::CommandNextLesson(msg);
            break;
        }
        case Item::WatchDemo: {
            std::string msg;
            const bool ok = ::Mission::TutorialSession::IsActive()
                                ? ::Mission::TutorialSession::CommandPlayDemo(msg)
                                : ::Mission::Engine::Demo::PlayLoaded(msg);
            if (!ok) {
                DirectDrawHook::AddMessage(("Demo unavailable: " + msg).c_str(), "MISSION",
                                           RGB(255, 180, 120), 2000, 0, 120);
            }
            break;
        }
        case Item::ReturnToBrowser: {
            const auto screen = ctx == Context::Lesson
                                    ? PracticeMenu::TitleScreen::Screen::Tutorial
                                    : PracticeMenu::TitleScreen::Screen::Missions;
            if (!CanRequestFrontendExit(FrontendExitTarget::Title)) {
                DirectDrawHook::AddMessage("Cannot leave the match right now", "MISSION",
                                           RGB(255, 180, 120), 2000, 0, 120);
                break;
            }
            if (RequestFrontendExit(FrontendExitTarget::Title)) {
                ::Mission::Engine::Runner::Unload();
                PracticeMenu::RequestTitleReopen(screen);
                LogOut("[MISSION][PAUSE] returning to title browser", true);
            } else {
                DirectDrawHook::AddMessage("Cannot leave the match right now", "MISSION",
                                           RGB(255, 180, 120), 2000, 0, 120);
            }
            break;
        }
        case Item::StartRecording:
            ::Mission::Engine::Recorder::Advance();
            LogOut("[MISSION][PAUSE] recording count-in started from dedicated menu",
                   true);
            break;
        case Item::OpenAuthoring:
            // Acquire ImGui while this dedicated surface still owns the pause;
            // only then release MissionPause. This avoids a live-world gap and
            // leaves the dedicated menu intact if ImGui cannot initialize.
            CustomMenu::Screens::OpenMissionBrowser();
            if (!OpenPracticeMenuDirect()) {
                DirectDrawHook::AddMessage(
                    "The authoring menu could not be opened; the recording was kept",
                    "MISSION", RGB(255, 180, 120), 2200, 0, 120);
                LogOut("[MISSION][PAUSE] explicit authoring handoff failed", true);
            } else {
                Close();
                LogOut("[MISSION][PAUSE] explicit authoring handoff opened", true);
            }
            break;
        case Item::OpenPractice:
            CustomMenu::Screens::OpenPracticeRoot();
            if (!OpenPracticeMenuDirect()) {
                DirectDrawHook::AddMessage(
                    "Practice settings could not be opened; recording is still paused",
                    "MISSION", RGB(255, 180, 120), 2200, 0, 120);
                LogOut("[MISSION][PAUSE] nested Practice menu failed to open", true);
            } else {
                g_inputSyncRequested.store(true, std::memory_order_release);
                LogOut("[MISSION][PAUSE] nested Practice menu opened; capture remains suspended",
                       true);
            }
            break;
        case Item::CancelCountdown:
            ::Mission::Engine::Recorder::Advance();   // CountIn -> PreRecord (safe)
            LogOut("[MISSION][PAUSE] countdown canceled from pause menu", true);
            break;
        case Item::StopReview:
            ::Mission::Engine::Recorder::Advance();   // Recording -> Review (sealed)
            g_pendingReviewPause.store(true, std::memory_order_release);
            LogOut("[MISSION][PAUSE] capture stopped; dedicated Review menu pending",
                   true);
            break;
        case Item::PreviewRecording: {
            std::string message;
            if (!::Mission::Engine::Demo::PlayRecording(message)) {
                DirectDrawHook::AddMessage(
                    ("Preview unavailable: " + message).c_str(), "MISSION",
                    RGB(255, 180, 120), 2200, 0, 120);
                LogOut("[MISSION][PAUSE] recording preview refused: " + message,
                       true);
            }
            break;
        }
        case Item::RetakeRecording:
            ::Mission::Engine::Recorder::Retake();
            LogOut("[MISSION][PAUSE] confirmed recorder retake", true);
            break;
        case Item::DiscardRecording:
            ::Mission::Engine::Recorder::Cancel();
            LogOut("[MISSION][PAUSE] authoring session discarded from pause menu", true);
            break;
    }
}

void CloseLocked() {
    if (!g_state.open) return;
    const bool borrowedTutorialFreeze = g_state.borrowedTutorialFreeze;
    const bool recorderContext = IsRecorderContext(g_state.context);
    const bool captureSuspensionOwned =
        recorderContext && g_state.captureSuspensionOwned;
    const bool recorderStillCapturing = Rec::NeedsCaptureMenuHandoff(
        captureSuspensionOwned, Rec::GetPhase());
    g_state.open = false;
    g_state.borrowedTutorialFreeze = false;
    g_state.confirming = false;
    g_state.confirmationSelection = 0;
    g_state.captureSuspensionOwned = false;
    g_openFlag.store(false, std::memory_order_release);
    if (captureSuspensionOwned) {
        if (recorderStillCapturing) {
            // Transfer the physical freeze before this visible surface
            // releases it, avoiding a live-world gap. Recorder::Tick then
            // releases this provisional pause owner while its zero-input lease
            // drains the menu button and resynchronizes both streams.
            PauseIntegration::OnMenuSurfaceVisibilityChanged(
                PauseIntegration::MenuSurface::RecorderHandoff, true);
        }
        Rec::SetCaptureMenuOpen(false);
    }
    if (!borrowedTutorialFreeze) {
        PauseIntegration::OnMenuSurfaceVisibilityChanged(
            PauseIntegration::MenuSurface::MissionPause, false);
    }
}

// True while a context this menu owns is live in a match.
bool OwningContextAlive() {
    if (GetCurrentGamePhase() != GamePhase::Match) return false;
    return ::Mission::Engine::Recorder::IsSessionActive() ||
           ::Mission::Engine::Runner::IsActive();
}

} // namespace

bool IsOpen() {
    return g_openFlag.load(std::memory_order_acquire);
}

bool Open() {
    using namespace ::Mission::Engine;
    if (GetCurrentGamePhase() != GamePhase::Match) return false;
    if (Demo::IsActive()) return false;
    if (ImGuiImpl::IsVisible()) return false;

    const Recorder::Phase recPhase = Recorder::GetPhase();
    const bool recorderSession = Recorder::UsesDedicatedPauseMenu(recPhase);
    const bool capture = Recorder::IsCapturePhase(recPhase);
    // Preparing has no durable checkpoint yet. DemoSuspended remains an owned
    // restore transaction for one fresh Tutorial tick after Demo becomes idle;
    // opening Restart in that interval could change phase before Tutorial
    // acknowledges the result and strand Demo's direct P1-neutral lease.
    if (!recorderSession && ::Mission::TutorialSession::IsActive()) {
        const auto tutorialPhase = ::Mission::TutorialSession::GetPhase();
        if (!::Mission::TutorialSession::SessionMenuAllowed(tutorialPhase)) {
            LogOut(tutorialPhase ==
                       ::Mission::TutorialSession::Phase::Preparing
                       ? "[MISSION][PAUSE] open ignored while lesson checkpoint is preparing"
                       : "[MISSION][PAUSE] open ignored while demonstration restore handoff is pending",
                   detailedLogging.load(std::memory_order_relaxed));
            return false;
        }
    }
    std::lock_guard<std::mutex> lk(g_mx);
    if (g_state.open) return true;
    g_state.items.clear();
    g_state.nextLessonName.clear();
    g_state.selection = 0;
    g_state.hasDemo = false;
    g_state.borrowedTutorialFreeze = false;
    g_state.captureSuspensionOwned = false;
    g_state.confirming = false;
    g_state.confirmationSelection = 0;

    if (recorderSession) {
        if (!ConfigureRecorderStateLocked(recPhase)) return false;
    } else {
        if (!Runner::IsActive()) return false;
        ::Mission::Mission mission;
        int currentStep = 0, failedStep = -1, currentHits = 0;
        bool armed = false;
        if (!Runner::GetRenderSnapshot(mission, currentStep, failedStep, armed, currentHits)) {
            return false;
        }
        const bool lesson = mission.type == "tutorial";
        g_state.context = lesson ? Context::Lesson : Context::Mission;
        g_state.title = lesson ? "LESSON PAUSED" : "MISSION PAUSED";
        g_state.sessionName = Upper(mission.name.empty() ? std::string("SESSION") : mission.name);
        // GetRenderSnapshot strips the demo blob; ask the runner directly.
        g_state.hasDemo = Runner::HasDemo();

        if (lesson) {
            g_state.progress = ::Mission::TutorialSession::ProgressLabel();
        } else {
            char progress[64];
            const int total = Runner::StepCount();
            const int shownStep = (std::min)(currentStep + 1, (std::max)(total, 1));
            _snprintf_s(progress, sizeof(progress), _TRUNCATE, "STEP %d OF %d  |  ATTEMPT %d",
                        shownStep, total, (std::max)(1, Runner::Attempts()));
            g_state.progress = progress;
        }
        g_state.items.push_back(Item::Resume);
        if (lesson && ::Mission::TutorialSession::IsActive()) {
            if (::Mission::TutorialSession::CanRetryCurrentTask()) {
                g_state.items.push_back(Item::RetryTask);
            }
            // A startup/setup error can happen before the root savestate is
            // captured. Do not offer a restart backed by no state; Return to
            // Lessons remains available and is the only truthful recovery.
            if (Runner::HasBaseline()) {
                g_state.items.push_back(Item::Retry);       // RESTART LESSON
            }
            const auto tutorialPhase = ::Mission::TutorialSession::GetPhase();
            if (tutorialPhase != ::Mission::TutorialSession::Phase::Intro &&
                tutorialPhase != ::Mission::TutorialSession::Phase::Review &&
                tutorialPhase != ::Mission::TutorialSession::Phase::ConfirmExit &&
                tutorialPhase != ::Mission::TutorialSession::Phase::Preparing &&
                tutorialPhase != ::Mission::TutorialSession::Phase::Error) {
                g_state.items.push_back(Item::ReviewLesson);
            }
            // CommandPlayDemo deliberately refuses Error: the lesson's
            // checkpoint/setup contract is not trustworthy there.  Do not
            // advertise a row which can only close the menu and toast an
            // error. Other frozen phases (Intro/Review/ConfirmExit/Complete)
            // are valid demo sources and preserve their return destination.
            if (g_state.hasDemo &&
                tutorialPhase != ::Mission::TutorialSession::Phase::Error) {
                g_state.items.push_back(Item::WatchDemo);
            }
            if (::Mission::TutorialSession::LessonCleared() &&
                !::Mission::TutorialSession::NextLessonName().empty()) {
                g_state.nextLessonName = ::Mission::TutorialSession::NextLessonName();
                g_state.items.push_back(Item::NextLesson);
            }
        } else {
            // A mission remains playable if root-baseline capture failed, but
            // RequestRetry cannot restore it. Do not offer a dead menu row.
            if (Runner::HasBaseline()) g_state.items.push_back(Item::Retry);
            // Same gate as the lesson branch: Draw never actually dims rows,
            // so an unconditional WatchDemo on a demo-less mission closed the
            // menu, dropped the player back into live play, and only then
            // toasted "Demo unavailable". Offer it only when a demo exists.
            if (g_state.hasDemo) g_state.items.push_back(Item::WatchDemo);
        }
        g_state.items.push_back(Item::ReturnToBrowser);
    }

    g_state.open = true;
    // Always take a nested surface. PauseIntegration retains the existing
    // physical pause when TutorialPage already owns it, while the atomic mask
    // transition also cancels any in-flight tutorial refresh before this menu
    // becomes visible.
    g_state.borrowedTutorialFreeze = false;
    g_openFlag.store(true, std::memory_order_release);
    g_inputSyncRequested.store(true, std::memory_order_release);
    if (!g_state.borrowedTutorialFreeze) {
        PauseIntegration::OnMenuSurfaceVisibilityChanged(
            PauseIntegration::MenuSurface::MissionPause, true);
    }
    if (capture) {
        Recorder::SetCaptureMenuOpen(true);
        g_state.captureSuspensionOwned = true;
    }
    if (g_state.context == Context::Review) {
        g_pendingReviewPause.store(false, std::memory_order_release);
    }
    LogOut(std::string("[MISSION][PAUSE] opened (context=") +
           std::to_string(static_cast<int>(g_state.context)) +
           ", borrowedTutorialFreeze=" +
           (g_state.borrowedTutorialFreeze ? "1" : "0") + ")", true);
    return true;
}

void Close() {
    std::lock_guard<std::mutex> lk(g_mx);
    CloseLocked();
}

void Toggle() {
    // Two watchers can deliver the same menu press (the battle ESC hook and
    // the hotkey thread both route through OpenMenu). The old practice menu
    // was idempotent through its menuOpen guard; a toggle needs an explicit
    // debounce so one physical press cannot open-then-close.
    static std::atomic<DWORD> s_lastToggleTick{0};
    const DWORD now = GetTickCount();
    DWORD previous = s_lastToggleTick.load(std::memory_order_relaxed);
    do {
        if (now - previous < 250) return;
    } while (!s_lastToggleTick.compare_exchange_weak(
        previous, now, std::memory_order_acq_rel, std::memory_order_relaxed));
    if (IsOpen()) {
        bool dismissedConfirmation = false;
        {
            std::lock_guard<std::mutex> lk(g_mx);
            if (g_state.open && g_state.confirming) {
                g_state.confirming = false;
                g_state.confirmationSelection = 0;
                dismissedConfirmation = true;
                g_inputSyncRequested.store(true, std::memory_order_release);
            }
        }
        // Escape/Menu acts like Back while a destructive confirmation is
        // visible. A second deliberate press resumes from the normal list.
        if (!dismissedConfirmation) Close();
    } else {
        Open();
    }
}

void NotifyRecorderEnteredReview() {
    // Only STOP & REVIEW requests an automatic return. Other transitions into
    // Review (for example an external authoring command) must not open a menu
    // the player did not ask for.
    if (!g_pendingReviewPause.exchange(false, std::memory_order_acq_rel)) return;
    if (Rec::GetPhase() != Rec::Phase::Review || !Open()) {
        // Tick provides a bounded fallback if the render/menu state was still
        // changing on this exact recorder boundary.
        g_pendingReviewPause.store(true, std::memory_order_release);
    }
}

void Tick() {
    // STOP & REVIEW handoff: once the queued Advance actually reaches Review,
    // reopen this dedicated menu, never the ordinary Practice menu. The
    // recorder normally calls NotifyRecorderEnteredReview synchronously;
    // this fallback covers a temporarily unavailable menu/render state.
    if (g_pendingReviewPause.load(std::memory_order_acquire) && !IsOpen()) {
        const Rec::Phase phase = Rec::GetPhase();
        if (phase == Rec::Phase::Review) {
            NotifyRecorderEnteredReview();
        } else if (phase != Rec::Phase::Recording) {
            g_pendingReviewPause.store(false, std::memory_order_release);
        }
    }

    if (!IsOpen()) return;

    // A session that ends underneath the menu (match left, runner unloaded,
    // capture sealed externally) must not leave a stuck freeze.
    if (!OwningContextAlive()) {
        Close();
        return;
    }
    {
        std::lock_guard<std::mutex> lk(g_mx);
        if (!SyncRecorderContextLocked()) {
            CloseLocked();
            return;
        }
    }
    PauseIntegration::MaintainFreezeWhileMenuVisible();
    if (ImGuiImpl::IsVisible()) {
        // Practice settings are nested under this recorder surface. Preserve
        // both freeze and capture ownership; Back reveals the recorder pause
        // menu instead of silently resuming the take.
        g_inputSyncRequested.store(true, std::memory_order_release);
        return;
    }
    UpdateWindowActiveState();
    const bool windowActive = g_efzWindowActive.load(std::memory_order_relaxed);
    const HWND gameWindow = FindEFZWindow();
    const bool focused = gameWindow && GetForegroundWindow() == gameWindow;
    auto keyDown = [&](int vk) {
        return focused && vk > 0 &&
               (GetAsyncKeyState(vk) & 0x8000) != 0;
    };

    // ---- input (frame-monitor thread, ~192Hz) ----
    // Match tutorial page controls exactly: arrows and the detected EFZ
    // directions navigate, Enter/EFZ A confirms, and Backspace/EFZ B backs
    // out. Escape and the configured controller Menu action remain owned by
    // input_handler's PauseMenu::Toggle route; consuming Escape here as well
    // would create an open/close race between the two polling threads.
    bool upKey = keyDown(VK_UP);
    bool downKey = keyDown(VK_DOWN);
    const bool confirmK = keyDown(VK_RETURN);
    const bool cancelK = keyDown(VK_BACK);
    bool boundA = false;
    bool boundB = false;
    if (detectedBindings.directionsDetected) {
        upKey |= keyDown(detectedBindings.upKey);
        downKey |= keyDown(detectedBindings.downKey);
    }
    if (detectedBindings.attacksDetected) {
        boundA = keyDown(detectedBindings.aButton);
        boundB = keyDown(detectedBindings.bButton);
    }

    bool padUp = false, padDown = false, padA = false, padB = false;
    XInputShim::Snapshot padSnapshot{};
    if (focused) XInputShim::CopySnapshot(padSnapshot);
    const unsigned mask = padSnapshot.connectedMask;
    const int selectedPad = Config::GetSettings().controllerIndex;
    for (int i = 0; i < 4; ++i) {
        if (!(mask & (1u << i))) continue;
        if (selectedPad >= 0 && selectedPad <= 3 && i != selectedPad) continue;
        const XINPUT_STATE& state = padSnapshot.states[i];
        padUp   |= (state.Gamepad.wButtons & XINPUT_GAMEPAD_DPAD_UP) != 0 ||
                   state.Gamepad.sThumbLY > 16000;
        padDown |= (state.Gamepad.wButtons & XINPUT_GAMEPAD_DPAD_DOWN) != 0 ||
                   state.Gamepad.sThumbLY < -16000;
        padA    |= (state.Gamepad.wButtons & XINPUT_GAMEPAD_A) != 0;
        padB    |= (state.Gamepad.wButtons & XINPUT_GAMEPAD_B) != 0;
    }

    const int vert = (upKey || padUp) ? -1 : ((downKey || padDown) ? 1 : 0);
    const bool confirm = confirmK || boundA || padA;
    const bool cancel = cancelK || boundB || padB;

    static int s_prevVert = 0;
    static int s_vertHold = 0;
    static bool s_prevConfirm = true;   // swallow the press that opened the menu
    static bool s_prevCancel = true;
    static bool s_inputWasActive = false;

    const bool inputActive = windowActive && focused;
    if (g_inputSyncRequested.exchange(false, std::memory_order_acq_rel) ||
        !inputActive || !s_inputWasActive) {
        // Resynchronize on every open and while unfocused. Held A/B/directions
        // can neither act in another application nor fire on the refocus frame.
        s_prevVert = vert;
        s_prevConfirm = confirm;
        s_prevCancel = cancel;
        s_vertHold = 0;
        s_inputWasActive = inputActive;
        return;
    }
    s_inputWasActive = true;

    int vertEdge = (s_prevVert == 0 && vert != 0) ? vert : 0;
    if (vert != 0 && vert == s_prevVert) {
        ++s_vertHold;
        // ~0.3s initial delay then ~10/sec at the monitor's 192Hz cadence.
        if (s_vertHold >= 58 && ((s_vertHold - 58) % 19) == 0) vertEdge = vert;
    } else {
        s_vertHold = 0;
    }
    const bool confirmEdge = confirm && !s_prevConfirm;
    const bool cancelEdge = cancel && !s_prevCancel;
    s_prevVert = vert;
    s_prevConfirm = confirm;
    s_prevCancel = cancel;

    if (cancelEdge) {
        bool closeMenu = true;
        {
            std::lock_guard<std::mutex> lk(g_mx);
            if (g_state.open && g_state.confirming) {
                g_state.confirming = false;
                g_state.confirmationSelection = 0;
                closeMenu = false;
            }
        }
        if (closeMenu) Close();
        return;
    }

    Item confirmed = Item::Resume;
    Context ctx = Context::Mission;
    bool doExecute = false;
    {
        std::lock_guard<std::mutex> lk(g_mx);
        if (!g_state.open) return;
        if (g_state.confirming) {
            constexpr int kConfirmationChoices = 2;
            if (vertEdge != 0) {
                g_state.confirmationSelection =
                    (g_state.confirmationSelection +
                     (vertEdge > 0 ? 1 : -1) + kConfirmationChoices) %
                    kConfirmationChoices;
            }
            if (confirmEdge) {
                if (g_state.confirmationSelection == 0) {
                    // KEEP is deliberately the default and non-destructive.
                    g_state.confirming = false;
                    g_state.confirmationSelection = 0;
                } else {
                    confirmed = g_state.confirmationItem;
                    ctx = g_state.context;
                    doExecute = true;
                    if (UsesRecorderCommandHandoff(confirmed)) {
                        Rec::BeginMenuCommandHandoff();
                    }
                    CloseLocked();
                }
            }
        } else {
            const int count = static_cast<int>(g_state.items.size());
            if (vertEdge != 0 && count > 0) {
                g_state.selection =
                    (g_state.selection + (vertEdge > 0 ? 1 : -1) + count) %
                    count;
            }
            if (confirmEdge && count > 0) {
                confirmed = g_state.items[g_state.selection];
                ctx = g_state.context;
                if (NeedsConfirmation(confirmed)) {
                    g_state.confirming = true;
                    g_state.confirmationItem = confirmed;
                    g_state.confirmationSelection = 0;
                } else {
                    doExecute = true;
                    if (confirmed != Item::OpenPractice &&
                        confirmed != Item::OpenAuthoring) {
                        if (UsesRecorderCommandHandoff(confirmed)) {
                            Rec::BeginMenuCommandHandoff();
                        }
                        CloseLocked();   // release before restore/exit actions
                    }
                }
            }
        }
    }
    if (doExecute) Execute(confirmed, ctx);
}

bool WantsDraw() {
    return IsOpen() && !ImGuiImpl::IsVisible();
}

void Draw(ImDrawList* dl) {
    if (!dl || !IsOpen() || ImGuiImpl::IsVisible()) return;
    State s;
    {
        std::lock_guard<std::mutex> lk(g_mx);
        s = g_state;
    }
    if (!s.open) return;

    // Native in-game look: dim the frozen match, title band, ruled session
    // rows, and the description box at the bottom.
    CustomMenu::Scale::Update(Config::GetSettings().uiScale);
    ImFont* body = L::BodyFont();
    const CustomMenu::Scale::Metrics& metrics = CustomMenu::Scale::Get();
    const float smallPx = (std::max)(8.0f, metrics.bodyPx - 2.0f);
    const float ls = metrics.layoutScale;
    const float barH = T::kBarH * ls;
    const bool sessionContext = s.context == Context::Mission ||
                                s.context == Context::Lesson;
    const bool confirmation = s.confirming;
    const char* displayTitle = confirmation
        ? ConfirmationPrompt(s.confirmationItem, s.context)
        : s.title.c_str();

    dl->AddRectFilled(ImVec2(0, 0), ImVec2(T::kCanvasW, T::kCanvasH), IM_COL32(0, 0, 0, 150));

    // Title and recording/session metadata use separate strips. Combining the
    // recorder's action count with its centered title collided at 1.50x UI
    // scale and made the dedicated menu look like a broken Practice header.
    const float bandBot = L::DrawTitleBand(dl, displayTitle);
    const float metaH = CustomMenu::Scale::Snap(
        (std::max)(20.0f, smallPx + 8.0f));
    const float metaBot = bandBot + metaH;
    const float metaSplit = sessionContext ? 370.0f : 180.0f;
    dl->AddRectFilled(ImVec2(0.0f, bandBot),
                      ImVec2(T::kCanvasW, metaBot), T::kStripStrong);
    dl->AddLine(ImVec2(0.0f, metaBot - 1.0f),
                ImVec2(T::kCanvasW, metaBot - 1.0f),
                kSessionRowRule, 1.0f);
    const float metaY = bandBot + (metaH - smallPx) * 0.5f;
    dl->PushClipRect(ImVec2(14.0f, bandBot),
                     ImVec2(metaSplit - 10.0f, metaBot), true);
    L::DrawString(dl, body, smallPx, 14.0f, metaY,
                  T::kTextActive, s.sessionName.c_str());
    dl->PopClipRect();
    const float progressW = L::MeasureTextW(
        body, smallPx, s.progress.c_str());
    dl->PushClipRect(ImVec2(metaSplit, bandBot),
                     ImVec2(T::kCanvasW - 14.0f, metaBot), true);
    L::DrawString(dl, body, smallPx,
                  (std::max)(metaSplit,
                      T::kCanvasW - 14.0f - progressW),
                  metaY, T::kTextStatus, s.progress.c_str());
    dl->PopClipRect();

    // Action bars, centered stack like the game's netplay/settings menus.
    const float bx0 = 70.0f, bx1 = T::kCanvasW - 70.0f;
    const int count = confirmation ? 2 : static_cast<int>(s.items.size());
    const int requestedSelection = confirmation
        ? s.confirmationSelection : s.selection;
    const int selectedIndex = count > 0
        ? (std::max)(0, (std::min)(requestedSelection, count - 1)) : 0;
    const Item selectedItem = !confirmation && count > 0
        ? s.items[selectedIndex] : Item::Resume;
    const char* selectedHint = confirmation
        ? ConfirmationHint(s.confirmationItem, s.context)
        : (count > 0 ? ItemHint(selectedItem, s.context)
                     : "No actions are available.");
    constexpr const char* controlsLine1 =
        "UP / DOWN: SELECT    CONFIRM: ACCEPT";
    const char* controlsLine2 = confirmation
        ? "BACK / MENU / ESC: CANCEL"
        : "MENU / ESC: RESUME";
    const float footerTextW = T::kCanvasW - 52.0f;
    const float hintH = (std::max)(smallPx,
        MeasureWrappedHeight(body, smallPx, selectedHint, footerTextW));
    const float footerPad = CustomMenu::Scale::Snap(
        (std::max)(5.0f, 7.0f * ls));
    const float footerGap = CustomMenu::Scale::Snap(
        (std::max)(4.0f, 6.0f * ls));
    const float controlGap = CustomMenu::Scale::Snap(
        (std::max)(2.0f, 3.0f * ls));
    const float measuredFootH = footerPad * 2.0f + hintH + footerGap +
                                smallPx * 2.0f + controlGap;
    const float footH = (std::max)(42.0f * ls, measuredFootH);
    const float rowGap = sessionContext ? 0.0f : 5.0f;
    const float stackH = count > 0
        ? count * barH + (count - 1) * rowGap : 0.0f;
    const float areaTop = metaBot + 12.0f;
    const float areaBot = T::kCanvasH - footH - 20.0f;
    float y = areaTop + (std::max)(0.0f, (areaBot - areaTop - stackH) * 0.5f);
    if (sessionContext && count > 0) {
        dl->AddLine(ImVec2(bx0, CustomMenu::Scale::Snap(y)),
                    ImVec2(bx1, CustomMenu::Scale::Snap(y)),
                    kSessionRowRule, 1.0f);
    }
    for (int i = 0; i < count; ++i) {
        const bool disabled = false;
        std::string label;
        if (confirmation) {
            label = ConfirmationLabel(s.confirmationItem, s.context, i);
        } else {
            const Item item = s.items[i];
            label = ItemLabel(item, s.context);
            if (item == Item::NextLesson && !s.nextLessonName.empty()) {
                label += " - " + Upper(s.nextLessonName);
            }
        }
        if (sessionContext) {
            DrawSessionPauseRow(dl, body, metrics.bodyPx,
                                bx0, y, bx1 - bx0, barH,
                                label.c_str(), i == selectedIndex);
        } else {
            L::DrawNativeBarCentered(dl, bx0, y, bx1 - bx0, barH,
                                     label.c_str(), i == selectedIndex, disabled);
        }
        y += barH + rowGap;
    }

    // Bottom description box. Session controls are pre-split and its height
    // follows wrapped text at 1.50x; recorder presentation remains unchanged.
    const float footerY = T::kCanvasH - footH - 8.0f;
    L::DrawInfoBox(dl, 12.0f, footerY, T::kCanvasW - 24.0f, footH);
    float textY = footerY + footerPad;
    DrawWrappedText(dl, body, smallPx, 26.0f, textY,
                    footerTextW, hintH, T::kTextActive, selectedHint);
    textY += hintH + footerGap;
    L::DrawString(dl, body, smallPx, 26.0f, textY,
                  T::kTextStatus, controlsLine1);
    textY += smallPx + controlGap;
    L::DrawString(dl, body, smallPx, 26.0f, textY,
                  T::kTextStatus, controlsLine2);
}

} // namespace Mission::PauseMenu
