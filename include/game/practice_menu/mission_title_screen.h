#pragma once
//
// Full-screen title browsers for Missions and hands-on Tutorials.
//
// MISSIONS keeps the persistent tab + list + detail layout. TUTORIAL is a
// two-pane course browser (left lesson pane owns focus; right compact
// category rail; context-sensitive footer) per TUTORIAL_MODE_DESIGN.md §3.2.
//
// practice_menu owns the title update loop: it feeds input and launches the
// selected mission; this module owns state + rendering (mutex-shared).
// Selecting an entry transitions straight into the native load - there is no
// intermediate "starting session" cover screen.
//
#include <string>
#include <vector>

struct ImDrawList;

namespace PracticeMenu::TitleScreen {

enum class Screen : int { None = 0, Missions, Tutorial };

// Rich per-mission info shown in the browser (parsed from the mission json).
struct MissionInfo {
    std::string name;        // display name (file name fallback)
    std::string path;        // json path
    std::string description;
    std::string type;        // "combo" (trial) / "tutorial" / other (scenario)
    std::string character;   // P1 internal name ("" = unknown)
    std::string dummy;       // P2 internal name
    std::string source;      // pack name or "RECORDED"
    std::string packId;      // stable tutorial progress namespace
    std::string author;
    std::string category;    // tutorial topic / optional mission grouping
    std::string recipe;      // compact notation preview
    std::string hint;        // first authored hint
    int steps = 0;
    int stage = -1;
    int bgm = -1;
    int difficulty = 0;
    int order = 0;           // authored course order (tutorial; 0 = file order)
    std::string lessonId;    // stable id (tutorialSchema lessons)
    std::vector<std::string> recommendedAfter; // soft recommendation only
    std::string unavailableReason; // non-empty = capability-gated UNAVAILABLE row
    bool recorded = false;
    bool locked = false;
    bool cleared = false;    // progress mark (browser display only)
    bool updated = false;    // cleared against an older authored revision
    bool hasDemo = false;
    bool hasSavestate = false;
};

// What a confirm press did (practice_menu acts on Launch and Record).
// Record = the CREATE > RECORD NEW SESSION row: launch practice with the
// mission engine's pending-record mode armed (recorder starts on match settle).
enum class ConfirmAction : int { None = 0, Launch, Record };

// ---- game-thread control ----
void Open(Screen screen, bool resumeLastLesson = false); // instant swap (menus never fade)
void Close();
bool Active();
Screen Current();

void SetMissions(std::vector<MissionInfo>&& missions);  // rebuilds categories

// Tutorial course taxonomy from pack metadata (ordered key/label pairs). The
// browser never hardcodes categories; empty pack list falls back to the keys
// discovered from entries.
void SetTutorialCategories(std::vector<std::pair<std::string, std::string>>&& cats);

// Input routing. The tutorial screen has an internal focus model: the lesson
// pane owns focus; MoveTab(+1) moves focus into the category rail, MoveTab(-1)
// returns to the pane; MoveSelection changes lesson or category by focus.
// Back() returns true when it consumed the press (rail -> pane); false means
// the caller should close the screen.
void MoveSelection(int dir);         // vertical navigation (list or rail)
void MoveTab(int dir);               // missions: tabs; tutorial: pane<->rail focus
ConfirmAction Confirm();             // launch selected entry / record action
bool Back();
std::string SelectedMissionPath();   // valid after Confirm() == Launch

// ---- render-thread ----
bool WantsDraw();
// `device` is the live IDirect3DDevice9*. The tutorial course pane uses it to
// resolve authored {input:...} tokens through the shared control-icon atlas.
void Draw(void* device, ImDrawList* dl);

} // namespace PracticeMenu::TitleScreen
