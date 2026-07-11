#include "../../../include/game/mission/mission_data.h"

#include <nlohmann/json.hpp>

#include <windows.h>

#include <fstream>
#include <algorithm>
#include <cctype>
#include <sstream>

// Mission data (de)serialization. nlohmann is included ONLY here to keep its
// heavy compile cost off the rest of the build. All parsing is tolerant: missing
// fields fall back to struct defaults; malformed files return false + a message.

using nlohmann::json;

namespace Mission {

const char* StepReqToString(StepReq req) {
    switch (req) {
        case StepReq::Move: return "move";
        case StepReq::Hits: return "hits";
        case StepReq::Connect: return "connect";
        case StepReq::Land: default: return "land";
    }
}
StepReq StepReqFromString(const std::string& s) {
    if (s == "move") return StepReq::Move;
    if (s == "hits") return StepReq::Hits;
    if (s == "connect") return StepReq::Connect;
    return StepReq::Land;
}

namespace {

std::string ReadFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return std::string();
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

bool WriteFile(const std::string& path, const std::string& content) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(content.data(), static_cast<std::streamsize>(content.size()));
    return static_cast<bool>(f);
}

// ---- json -> struct (tolerant) ----
Step ParseStep(const json& j) {
    Step s;
    s.notation = j.value("notation", std::string());
    if (j.contains("ids") && j["ids"].is_array()) {
        for (const auto& id : j["ids"]) {
            if (id.is_number_integer()) s.moveIds.push_back(id.get<int>());
        }
    }
    s.req = StepReqFromString(j.value("req", std::string("land")));
    s.hitsRequired = j.value("hits", 1);
    s.optional = j.value("optional", false);
    s.maxDelay = j.value("maxDelay", 0);
    s.maxGap = j.value("maxGap", 0);
    // Current format-1 bridge to the explicit format-2 combo.end boundary.
    // Accept the old development boolean spelling as an alias, but serialize
    // only the descriptive field below.
    s.comboEndAfter = j.value("comboEndAfter", j.value("requireComboEndAfter", false));
    if (j.contains("comboEnd") && j["comboEnd"].is_string()) {
        const std::string v = j["comboEnd"].get<std::string>();
        if (v == "must" || v == "required") s.comboEndAfter = true;
    }
    return s;
}

std::map<std::string, int> ParseResources(const json& j) {
    std::map<std::string, int> out;
    if (j.contains("resources") && j["resources"].is_object()) {
        for (auto it = j["resources"].begin(); it != j["resources"].end(); ++it) {
            if (it.value().is_number_integer()) out[it.key()] = it.value().get<int>();
        }
    }
    return out;
}

PlayerSetup ParsePlayer(const json& j) {
    PlayerSetup p;
    if (!j.is_object()) return p;
    p.character = j.value("character", std::string());
    if (j.contains("pos") && j["pos"].is_object()) {
        p.posX = j["pos"].value("x", 0.0);
        p.posY = j["pos"].value("y", 0.0);
    } else {
        p.posX = j.value("pos", 0.0); // allow scalar shorthand
    }
    p.palette = j.value("palette", 0);
    p.rf = j.value("rf", -1);
    p.meter = j.value("meter", -1);
    p.hp = j.value("hp", -1);
    p.blueIC = j.value("blueIC", -1);
    p.guard = j.value("guard", -1);
    p.resources = ParseResources(j);
    return p;
}

DummySetup ParseDummy(const json& j) {
    DummySetup d;
    if (!j.is_object()) return d;
    d.character = j.value("character", std::string());
    if (j.contains("pos") && j["pos"].is_object()) {
        d.posX = j["pos"].value("x", 0.0);
        d.posY = j["pos"].value("y", 0.0);
    }
    d.palette = j.value("palette", 0);
    d.rf = j.value("rf", -1);
    d.meter = j.value("meter", -1);
    d.hp = j.value("hp", -1);
    d.blueIC = j.value("blueIC", -1);
    d.guard = j.value("guard", -1);
    d.crouch = j.value("crouch", false);
    d.jump = j.value("jump", false);
    d.airtech = j.value("airtech", std::string("none"));
    d.resources = ParseResources(j);
    return d;
}

ScoreTier ParseScore(const json& j) {
    ScoreTier t;
    t.minHits = j.value("minHits", j.value("min_hits", 0));
    t.minDamage = j.value("minDamage", j.value("min_damage", 0));
    t.maxAttempts = j.value("maxAttempts", j.value("max_attempts", 0));
    t.label = j.value("label", std::string());
    return t;
}

// ---- struct -> json ----
json StepToJson(const Step& s) {
    json j;
    j["notation"] = s.notation;
    j["ids"] = s.moveIds;
    j["req"] = StepReqToString(s.req);
    if (s.req == StepReq::Hits) j["hits"] = s.hitsRequired;
    if (s.optional) j["optional"] = true;
    if (s.maxDelay > 0) j["maxDelay"] = s.maxDelay;
    if (s.maxGap > 0) j["maxGap"] = s.maxGap;
    if (s.comboEndAfter) j["comboEndAfter"] = true;
    return j;
}

bool IsGeneratedRecordingPath(const std::string& path) {
    const size_t slash = path.find_last_of("\\/");
    std::string name = path.substr(slash == std::string::npos ? 0 : slash + 1);
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return name.rfind("recorded_", 0) == 0;
}

json PlayerToJson(const PlayerSetup& p) {
    json j;
    if (!p.character.empty()) j["character"] = p.character;
    j["pos"] = { {"x", p.posX}, {"y", p.posY} };
    j["palette"] = p.palette;
    if (p.rf >= 0) j["rf"] = p.rf;
    if (p.meter >= 0) j["meter"] = p.meter;
    if (p.hp >= 0) j["hp"] = p.hp;
    if (p.blueIC >= 0) j["blueIC"] = p.blueIC;
    if (p.guard >= 0) j["guard"] = p.guard;
    if (!p.resources.empty()) j["resources"] = p.resources;
    return j;
}

json DummyToJson(const DummySetup& d) {
    json j;
    if (!d.character.empty()) j["character"] = d.character;
    j["pos"] = { {"x", d.posX}, {"y", d.posY} };
    j["palette"] = d.palette;
    if (d.rf >= 0) j["rf"] = d.rf;
    if (d.meter >= 0) j["meter"] = d.meter;
    if (d.hp >= 0) j["hp"] = d.hp;
    if (d.blueIC >= 0) j["blueIC"] = d.blueIC;
    if (d.guard >= 0) j["guard"] = d.guard;
    if (d.crouch) j["crouch"] = true;
    if (d.jump) j["jump"] = true;
    if (d.airtech != "none") j["airtech"] = d.airtech;
    if (!d.resources.empty()) j["resources"] = d.resources;
    return j;
}

json ScoreToJson(const ScoreTier& t) {
    json j;
    if (t.minHits > 0) j["minHits"] = t.minHits;
    if (t.minDamage > 0) j["minDamage"] = t.minDamage;
    if (t.maxAttempts > 0) j["maxAttempts"] = t.maxAttempts;
    if (!t.label.empty()) j["label"] = t.label;
    return j;
}

} // namespace

bool LoadMission(const std::string& path, Mission& out, std::string& errorOut) {
    const std::string text = ReadFile(path);
    if (text.empty()) { errorOut = "cannot open " + path; return false; }
    try {
        const json j = json::parse(text);
        Mission m;
        m.format = j.value("format", 1);
        m.type = j.value("type", std::string("combo"));
        m.category = j.value("category", std::string());
        m.difficulty = j.value("difficulty", 0);
        m.name = j.value("name", std::string());
        m.description = j.value("description", std::string());
        if (j.contains("player")) m.player = ParsePlayer(j["player"]);
        if (j.contains("dummy"))  m.dummy  = ParseDummy(j["dummy"]);
        m.stage = j.value("stage", -1);
        m.bgm = j.value("bgm", -1);
        if (j.contains("bgmKind") && j["bgmKind"].is_string()) {
            const std::string kind = j["bgmKind"].get<std::string>();
            if (kind == "native") m.bgm = -1;
        } else if (m.bgm >= 0 && m.bgm != 150 &&
                   (IsGeneratedRecordingPath(path) || m.bgm > 32)) {
            // Recorder builds before logical-track tracking wrote the active
            // DirectSound buffer index to `bgm`. Every unmarked generated
            // `recorded_*.json` value therefore has buffer provenance, even
            // when it happens to overlap the valid logical track range 0..32.
            // For other legacy/authored files, only an out-of-range unmarked
            // value is unambiguously unsafe. Track 150 means OFF in both uses.
            m.bgm = -1;
        }
        if (j.contains("steps") && j["steps"].is_array()) {
            for (const auto& s : j["steps"]) m.steps.push_back(ParseStep(s));
        }
        m.demo = j.value("demo", std::string());
        m.savestate = j.value("savestate", std::string());
        m.failTimer = j.value("failTimer", j.value("fail_timer", 60));
        if (j.contains("score") && j["score"].is_array()) {
            for (const auto& s : j["score"]) m.scores.push_back(ParseScore(s));
        }
        if (j.contains("hint") && j["hint"].is_array()) {
            for (const auto& h : j["hint"]) if (h.is_string()) m.hints.push_back(h.get<std::string>());
        }
        m.sourcePath = path;
        out = std::move(m);
        return true;
    } catch (const std::exception& e) {
        errorOut = std::string("JSON error in ") + path + ": " + e.what();
        return false;
    }
}

bool SaveMission(const std::string& path, const Mission& m, std::string& errorOut) {
    try {
        json j;
        j["format"] = m.format;
        j["type"] = m.type;
        if (!m.category.empty()) j["category"] = m.category;
        if (m.difficulty > 0) j["difficulty"] = m.difficulty;
        j["name"] = m.name;
        if (!m.description.empty()) j["description"] = m.description;
        j["player"] = PlayerToJson(m.player);
        j["dummy"] = DummyToJson(m.dummy);
        if (m.stage >= 0) j["stage"] = m.stage;
        if (m.bgm >= 0) {
            j["bgm"] = m.bgm;
            j["bgmKind"] = "track";
        }
        j["steps"] = json::array();
        for (const Step& s : m.steps) j["steps"].push_back(StepToJson(s));
        if (!m.demo.empty()) j["demo"] = m.demo;
        if (!m.savestate.empty()) j["savestate"] = m.savestate;
        j["failTimer"] = m.failTimer;
        j["score"] = json::array();
        for (const ScoreTier& t : m.scores) j["score"].push_back(ScoreToJson(t));
        if (!m.hints.empty()) j["hint"] = m.hints;
        if (!WriteFile(path, j.dump(4))) { errorOut = "cannot write " + path; return false; }
        return true;
    } catch (const std::exception& e) {
        errorOut = std::string("JSON write error: ") + e.what();
        return false;
    }
}

bool LoadPack(const std::string& path, Pack& out, std::string& errorOut) {
    const std::string text = ReadFile(path);
    if (text.empty()) { errorOut = "cannot open " + path; return false; }
    try {
        const json j = json::parse(text);
        Pack p;
        p.format = j.value("format", 1);
        p.name = j.value("name", std::string());
        p.author = j.value("author", std::string());
        p.character = j.value("character", std::string());
        p.description = j.value("description", std::string());
        if (j.contains("scenarios") && j["scenarios"].is_array()) {
            for (const auto& s : j["scenarios"]) {
                Scenario sc;
                sc.name = s.value("name", std::string());
                sc.file = s.value("file", std::string());
                sc.description = s.value("description", std::string());
                sc.preview = s.value("preview", std::string());
                sc.locked = s.value("locked", s.value("may_be_locked", false));
                p.scenarios.push_back(sc);
            }
        }
        // Pack folder = directory of the pack.json.
        const size_t slash = path.find_last_of("\\/");
        p.folderPath = (slash == std::string::npos) ? std::string() : path.substr(0, slash);
        out = std::move(p);
        return true;
    } catch (const std::exception& e) {
        errorOut = std::string("JSON error in ") + path + ": " + e.what();
        return false;
    }
}

bool SavePack(const std::string& path, const Pack& p, std::string& errorOut) {
    try {
        json j;
        j["format"] = p.format;
        j["name"] = p.name;
        if (!p.author.empty()) j["author"] = p.author;
        if (!p.character.empty()) j["character"] = p.character;
        if (!p.description.empty()) j["description"] = p.description;
        j["scenarios"] = json::array();
        for (const Scenario& s : p.scenarios) {
            json sj;
            sj["name"] = s.name;
            sj["file"] = s.file;
            if (!s.description.empty()) sj["description"] = s.description;
            if (!s.preview.empty()) sj["preview"] = s.preview;
            if (s.locked) sj["locked"] = true;
            j["scenarios"].push_back(sj);
        }
        if (!WriteFile(path, j.dump(4))) { errorOut = "cannot write " + path; return false; }
        return true;
    } catch (const std::exception& e) {
        errorOut = std::string("JSON write error: ") + e.what();
        return false;
    }
}

std::string ResolveMissionsRoot() {
    HMODULE mod = GetModuleHandleA("efz_training_mode.dll");
    char path[MAX_PATH] = {0};
    if (!mod || GetModuleFileNameA(mod, path, MAX_PATH) == 0) return std::string();
    std::string dir(path);
    const size_t slash = dir.find_last_of("\\/");
    if (slash == std::string::npos) return std::string();
    dir.resize(slash + 1);
    return dir + "assets\\missions";
}

std::vector<std::string> DiscoverPackJsonPaths(const std::string& rootDir) {
    std::vector<std::string> result;
    if (rootDir.empty()) return result;
    const std::string search = rootDir + "\\*";
    WIN32_FIND_DATAA fd = {};
    HANDLE h = FindFirstFileA(search.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return result;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (fd.cFileName[0] == '.') continue;
        const std::string packJson = rootDir + "\\" + fd.cFileName + "\\pack.json";
        if (GetFileAttributesA(packJson.c_str()) != INVALID_FILE_ATTRIBUTES) {
            result.push_back(packJson);
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return result;
}

} // namespace Mission
