#pragma once
//
// Full-screen title browsers for Missions and hands-on Tutorials. Both use a
// persistent tab + list + detail layout; no nested drill-down state.
//
// practice_menu owns the title update loop: it feeds input and launches the
// selected mission; this module owns state + rendering (mutex-shared).
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
    std::string type;        // "combo" (trial) / other (scenario)
    std::string character;   // P1 internal name ("" = unknown)
    std::string dummy;       // P2 internal name
    std::string source;      // pack name or "RECORDED"
    std::string author;
    std::string category;    // tutorial topic / optional mission grouping
    std::string recipe;      // compact notation preview
    std::string hint;        // first authored hint
    int steps = 0;
    int stage = -1;
    int bgm = -1;
    int difficulty = 0;
    bool recorded = false;
    bool locked = false;
    bool hasDemo = false;
    bool hasSavestate = false;
};

// What a confirm press did (practice_menu acts on Launch and Record).
// Record = the CREATE > RECORD NEW SESSION row: launch practice with the
// mission engine's pending-record mode armed (recorder starts on match settle).
enum class ConfirmAction : int { None = 0, Launch, Record };

enum class LaunchStage : int {
    Preparing = 0,
    Fighters,
    Loading,
    MatchSetup,
};

// ---- game-thread control ----
void Open(Screen screen);            // instant swap (menus never fade)
void Close();
bool Active();
Screen Current();

void SetMissions(std::vector<MissionInfo>&& missions);  // rebuilds categories
void MoveSelection(int dir);         // vertical list navigation
void MoveTab(int dir);               // horizontal topic/mode navigation
ConfirmAction Confirm();             // launch selected entry / record action
bool Back();                         // currently always false: caller closes screen
std::string SelectedMissionPath();   // valid after Confirm() == Launch

// Keeps a polished progress screen over direct fighter construction, Loading,
// and the guarded Character Select fallback while a session is created.
void BeginSelectedLaunch();
void SetLaunchStage(LaunchStage stage);
void FinishLaunch();

// ---- render-thread ----
bool WantsDraw();
void Draw(ImDrawList* dl);

} // namespace PracticeMenu::TitleScreen
