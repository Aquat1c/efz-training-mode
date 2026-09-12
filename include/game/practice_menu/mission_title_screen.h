#pragma once
//
// Full-screen title browsers for Missions and hands-on Tutorials.
//
// MISSIONS and TUTORIAL share one two-pane browser model: the playable list
// owns focus on entry, Right moves into a compact navigation rail, and Left or
// Cancel returns to the list.  Mission recording/pack work is a distinct rail
// destination rather than another playable-library tab.
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
    std::string packFolder;  // runtime identity for legacy packs without ids
    std::string author;
    std::string packVersion; // authored pack release shown in mission details
    std::string packDescription;
    std::string category;    // stable tutorial topic / mission-group id
    std::string categoryLabel;
    std::string categoryDescription;
    int categoryOrder = 0;   // pack-authored group order (0 = unspecified)
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
    bool disambiguateSource = false; // same display name belongs to another pack
};

// What a confirm press did (practice_menu performs the title transition).
// Record launches Practice with PRE-RECORD pending. Author launches Practice
// and opens the dedicated Pack Workshop without arming a recording.
enum class ConfirmAction : int { None = 0, Launch, Record, Author };

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

// Input routing. Both screens use an internal focus model: the content pane
// owns focus; MoveTab(+1) moves focus into the right rail, MoveTab(-1) returns
// to the pane; MoveSelection changes the current row or rail destination.
// Back() returns true when it consumed the press (rail -> pane); false means
// the caller should close the screen.
void MoveSelection(int dir);         // vertical navigation (list or rail)
void MoveTab(int dir);               // pane <-> right-rail focus
ConfirmAction Confirm();             // launch selected entry / record action
bool Back();
std::string SelectedMissionPath();   // valid after Confirm() == Launch

// ---- render-thread ----
bool WantsDraw();
// `device` is the live IDirect3DDevice9*. The tutorial course pane uses it to
// resolve authored {input:...} tokens through the shared control-icon atlas.
void Draw(void* device, ImDrawList* dl);

} // namespace PracticeMenu::TitleScreen
