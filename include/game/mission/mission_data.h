#pragma once
//
// Mission (combo trial) data model + JSON (de)serialization.
//
// Two-level structure mirrored from TrialMode (Soku), minus the Soku-specific
// bits (weather/cards/dolls/skills):
//   Pack (pack.json)  -> metadata + a list of Scenarios
//   Scenario          -> points to a Mission json file + list metadata
//   Mission (*.json)  -> player/dummy setup, ordered steps, demo, tiered scoring
//
// Validation model is exact-move-sequence (CCCaster): each Step lists the
// move-IDs that satisfy it and how (land / perform / N hits). The `demo` is an
// EFZMACRO string played back via MacroController.
//
// This header intentionally pulls in NO JSON headers (nlohmann is heavy to
// compile) - only <string>/<vector>. All JSON work lives in mission_data.cpp.

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace Mission {

// How a step is satisfied.
enum class StepReq : uint8_t {
    Land = 0,   // move must HIT (combo hit count increases)
    Move,       // just perform the move (whiff / movement / setup - no contact needed)
    Hits,       // move must produce `hitsRequired` hits (multi-hit)
    Connect,    // move must make CONTACT (hit OR blocked) - blockstring steps,
                // detected via the attacker hit-state machine (+0x168)
};

const char* StepReqToString(StepReq req);
StepReq     StepReqFromString(const std::string& s);

struct Step {
    std::string      notation;       // display string, e.g. "236B" (icons from assets/controls)
    std::vector<int> moveIds;        // any-of: move-IDs that satisfy this step
    StepReq          req = StepReq::Land;
    int              hitsRequired = 1; // for StepReq::Hits
    bool             optional = false; // step may be skipped
    int              maxDelay = 0;     // 0 = unlimited; NON-FROZEN frames the armed step may
                                       // wait for its hit (delayed projectile window)
    int              maxGap = 0;       // 0 = mission failTimer; NON-FROZEN frames allowed
                                       // between the previous step and performing this one
                                       // (recorded delays get a generous allowance)
    bool             comboEndAfter = false; // require a real combo alive->dead edge after
                                            // this step before the next step may begin. This
                                            // is an authored setup/okizeme boundary; false
                                            // keeps legacy one-piece-combo behavior.
};

struct PlayerSetup {
    std::string character;           // internal short name; empty = keep current
    double posX = 0.0;               // start X in world coords (0 = default)
    double posY = 0.0;
    int    palette = 0;
    int    rf = -1;                  // starting RF (-1 = leave as-is)
    int    meter = -1;               // starting meter (-1 = leave as-is)
    int    hp = -1;                  // starting HP (-1 = leave as-is)
    int    blueIC = -1;              // -1 keep, 0 red IC, 1 blue IC
    int    guard = -1;               // guard gauge 0..360 (-1 = leave as-is)
    // Character-specific resources captured at record time and restored on load
    // (e.g. mioStance, mishioElement, neyukiJam, ikumiBlood, akikoBulletCycle).
    std::map<std::string, int> resources;
};

struct DummySetup {
    std::string character;           // internal short name; empty = keep current
    double posX = 0.0, posY = 0.0;
    int    palette = 0;
    int    rf = -1;
    int    meter = -1;
    int    hp = -1;
    int    blueIC = -1;
    int    guard = -1;               // guard gauge 0..360 (-1 = leave as-is)
    bool   crouch = false;
    bool   jump = false;
    std::string airtech = "none";    // none / forward / back
    std::map<std::string, int> resources;
};

// A scoring/rank tier. A tier is "met" when every present (non-zero) constraint
// is satisfied by the run. Tier 0 is the base clear; later tiers are ranks.
struct ScoreTier {
    int         minHits = 0;         // 0 = ignore
    int         minDamage = 0;       // 0 = ignore
    int         maxAttempts = 0;     // 0 = ignore (attempts allowed to earn this rank)
    std::string label;               // optional display, e.g. "Clear" / "Gold"
};

struct Mission {
    int         format = 1;
    std::string type = "combo";
    std::string category;            // browser topic (tutorial: start/movement/offense/systems)
    int         difficulty = 0;      // 0 unspecified, otherwise 1..5 browser rating
    std::string name;
    std::string description;
    PlayerSetup player;
    DummySetup  dummy;
    int         stage = -1;          // -1 = default / keep
    int         bgm = -1;            // BGM track for hotswap load (-1 = default)
    std::vector<Step>        steps;
    std::string demo;                // EFZMACRO text (MacroController format)
    // Embedded Revival savestate dump (base64, Mission::StateDump format).
    // Captured at record start; restored after the mission's hotswap settles
    // so the match starts bit-perfect even in a fresh session. Empty = use
    // the value-level setup above (player/dummy fields) only.
    std::string savestate;
    int         failTimer = 60;      // frames of no-progress before the combo is a drop
    std::vector<ScoreTier>   scores; // [0] = base clear; [1..] = ranks
    std::vector<std::string> hints;

    std::string sourcePath;          // runtime-only: file this was loaded from
};

// One scenario entry inside a pack (mission file + list metadata + progression).
struct Scenario {
    std::string name;
    std::string file;                // mission json filename, relative to the pack folder
    std::string description;
    std::string preview;             // preview image filename (optional)
    bool        locked = false;      // progression gate
};

struct Pack {
    int         format = 1;
    std::string name;
    std::string author;
    std::string character;           // primary character short name
    std::string description;
    std::vector<Scenario> scenarios;

    std::string folderPath;          // runtime-only: the pack folder
};

// ---- Load / save (JSON via nlohmann, tolerant: missing fields use defaults) ----
// Return true on success; on failure fill `errorOut` and leave the out param
// unchanged. All are exception-safe (JSON parse errors become `errorOut`).
bool LoadPack   (const std::string& packJsonPath,    Pack&    out, std::string& errorOut);
bool LoadMission(const std::string& missionJsonPath, Mission& out, std::string& errorOut);
bool SaveMission(const std::string& missionJsonPath, const Mission& mission, std::string& errorOut);
bool SavePack   (const std::string& packJsonPath,    const Pack& pack,       std::string& errorOut);

// Discover pack folders under a root dir: returns each "<root>/<sub>/pack.json"
// that exists. Used to populate the mission-select menu.
std::vector<std::string> DiscoverPackJsonPaths(const std::string& rootDir);

// Resolve the missions root: "<dll-dir>\assets\missions". Empty on failure.
std::string ResolveMissionsRoot();

} // namespace Mission
