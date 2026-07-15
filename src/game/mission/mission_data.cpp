#include "../../../include/game/mission/mission_data.h"
#include "../../../include/game/mission/tutorial_episode_policy.h"
#include "../../../include/game/mission/tutorial_state_policy.h"

#include <nlohmann/json.hpp>

#include <windows.h>

#include <fstream>
#include <algorithm>
#include <cctype>
#include <map>
#include <set>
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
    s.charState = j.value("charState", -1);
    s.damage = j.value("damage", 0);
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
        p.hasPos = true;   // P0.2: presence, not zero-as-absent
        p.posX = j["pos"].value("x", 0.0);
        p.posY = j["pos"].value("y", 0.0);
    } else if (j.contains("pos")) {
        p.hasPos = true;
        p.posX = j.value("pos", 0.0); // allow scalar shorthand
    }
    p.palette = j.value("palette", 0);
    p.rf = j.value("rf", -1);
    p.meter = j.value("meter", -1);
    p.hp = j.value("hp", -1);
    p.blueIC = j.value("blueIC", -1);
    p.guard = j.value("guard", -1);
    p.rfLock = j.value("rfLock", false);
    p.resources = ParseResources(j);
    return p;
}

DummySetup ParseDummy(const json& j) {
    DummySetup d;
    if (!j.is_object()) return d;
    d.character = j.value("character", std::string());
    if (j.contains("pos") && j["pos"].is_object()) {
        d.hasPos = true;   // P0.2: presence, not zero-as-absent
        d.posX = j["pos"].value("x", 0.0);
        d.posY = j["pos"].value("y", 0.0);
    }
    d.palette = j.value("palette", 0);
    d.rf = j.value("rf", -1);
    d.meter = j.value("meter", -1);
    d.hp = j.value("hp", -1);
    d.blueIC = j.value("blueIC", -1);
    d.guard = j.value("guard", -1);
    d.rfLock = j.value("rfLock", false);
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
    if (s.charState >= 0) j["charState"] = s.charState;
    if (s.damage > 0) j["damage"] = s.damage;
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
    if (p.hasPos) j["pos"] = { {"x", p.posX}, {"y", p.posY} };
    j["palette"] = p.palette;
    if (p.rf >= 0) j["rf"] = p.rf;
    if (p.meter >= 0) j["meter"] = p.meter;
    if (p.hp >= 0) j["hp"] = p.hp;
    if (p.blueIC >= 0) j["blueIC"] = p.blueIC;
    if (p.guard >= 0) j["guard"] = p.guard;
    if (p.rfLock) j["rfLock"] = true;
    if (!p.resources.empty()) j["resources"] = p.resources;
    return j;
}

json DummyToJson(const DummySetup& d) {
    json j;
    if (!d.character.empty()) j["character"] = d.character;
    if (d.hasPos) j["pos"] = { {"x", d.posX}, {"y", d.posY} };
    j["palette"] = d.palette;
    if (d.rf >= 0) j["rf"] = d.rf;
    if (d.meter >= 0) j["meter"] = d.meter;
    if (d.hp >= 0) j["hp"] = d.hp;
    if (d.blueIC >= 0) j["blueIC"] = d.blueIC;
    if (d.guard >= 0) j["guard"] = d.guard;
    if (d.rfLock) j["rfLock"] = true;
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


// ---- tutorialSchema 1 (TUTORIAL_MODE_DESIGN.md S5) ----

ContactPredicate ParseContactPredicate(const json& j) {
    ContactPredicate c;
    if (j.is_string()) {
        c.result = j.get<std::string>(); // early development alias
        return c;
    }
    if (!j.is_object()) return c;
    c.attacker = j.value("attacker", std::string("learner"));
    c.target = j.value("target", c.attacker == "learner"
                                     ? std::string("dummy") : std::string("learner"));
    c.source = j.value("source", std::string("direct"));
    c.result = j.value("result", std::string());
    c.targetStateBefore = j.value("targetStateBefore", std::string());
    c.count = j.value("count", 1);
    if (j.contains("moveIds") && j["moveIds"].is_array()) {
        for (const auto& v : j["moveIds"]) {
            if (v.is_number_integer()) c.moveIds.push_back(v.get<int>());
        }
    }
    return c;
}

json ContactPredicateToJson(const ContactPredicate& c) {
    json j = {
        {"attacker", c.attacker}, {"target", c.target},
        {"source", c.source}, {"result", c.result}
    };
    if (!c.moveIds.empty()) j["moveIds"] = c.moveIds;
    if (!c.targetStateBefore.empty()) {
        j["targetStateBefore"] = c.targetStateBefore;
    }
    if (c.count != 1) j["count"] = c.count;
    return j;
}

LessonPage ParseLessonPage(const json& j) {
    LessonPage p;
    p.id = j.value("id", std::string());
    p.title = j.value("title", std::string());
    p.text = j.value("text", std::string());
    p.showHud = j.value("showHud", std::string());
    return p;
}

LessonAction ParseLessonAction(const json& j) {
    LessonAction a;
    a.id = j.value("id", std::string());
    a.notation = j.value("notation", std::string());
    if (j.contains("moveIds") && j["moveIds"].is_array()) {
        for (const auto& v : j["moveIds"]) {
            if (v.is_number_integer()) a.moveIds.push_back(v.get<int>());
        }
    }
    if (j.contains("fromMoveIds") && j["fromMoveIds"].is_array()) {
        for (const auto& v : j["fromMoveIds"]) {
            if (v.is_number_integer()) a.fromMoveIds.push_back(v.get<int>());
        }
    }
    a.inputMask = j.value("inputMask", 0);
    a.req = j.value("req", std::string("move"));
    a.hitsRequired = j.value("hits", 1);
    a.minGap = j.value("minGap", 0);
    a.maxGap = j.value("maxGap", 0);
    if (j.contains("afterWake") && j["afterWake"].is_object()) {
        a.afterWakeMaxTicks = j["afterWake"].value("maxTicks", 0);
    }
    if (j.contains("dummyState")) {
        if (j["dummyState"].is_string()) {
            a.dummyState = j["dummyState"].get<std::string>();
        } else if (j["dummyState"].is_array()) {
            for (const auto& v : j["dummyState"]) {
                if (v.is_number_integer()) a.dummyStateMoveIds.push_back(v.get<int>());
            }
        }
    }
    if (j.contains("withinWhiffWindow") && j["withinWhiffWindow"].is_object()) {
        a.whiffWindowMaxTicks = j["withinWhiffWindow"].value("maxTicks", 0);
    }
    a.duringGap = j.value("duringGap", false);
    if (j.contains("contact") && (j["contact"].is_object() || j["contact"].is_string())) {
        a.hasContact = true;
        a.contact = ParseContactPredicate(j["contact"]);
    }
    if (j.contains("flickerIC") && j["flickerIC"].is_object()) {
        a.hasFlickerIC = true;
        a.flickerProjectilePattern =
            j["flickerIC"].value("projectilePattern", 0);
        a.flickerMinDistance = j["flickerIC"].value("minDistance", 0);
    }
    return a;
}

LessonTask ParseLessonTask(const json& j) {
    LessonTask t;
    t.id = j.value("id", std::string());
    t.kind = j.value("kind", std::string("combat"));
    t.label = j.value("label", std::string());
    t.prompt = j.value("prompt", std::string());
    t.checkpoint = j.value("checkpoint", std::string("lessonStart"));
    t.script = j.value("script", std::string());
    t.continuity = j.value("continuity", std::string("independent"));
    if (j.contains("moveIds") && j["moveIds"].is_array()) {
        for (const auto& v : j["moveIds"]) if (v.is_number_integer()) t.moveIds.push_back(v.get<int>());
    }
    t.inputMask = j.value("inputMask", 0);
    t.req = j.value("req", std::string("move"));
    t.hitsRequired = j.value("hits", 1);
    if (j.contains("sequence") && j["sequence"].is_array()) {
        for (const auto& a : j["sequence"]) {
            if (a.is_object()) t.sequence.push_back(ParseLessonAction(a));
        }
    }
    if (j.contains("options") && j["options"].is_array()) {
        for (const auto& o : j["options"]) {
            ChoiceOption c;
            c.id = o.value("id", std::string());
            c.label = o.value("label", std::string());
            t.options.push_back(std::move(c));
        }
    }
    if (j.contains("acceptedOptionIds") && j["acceptedOptionIds"].is_array()) {
        for (const auto& v : j["acceptedOptionIds"]) if (v.is_string()) t.acceptedOptionIds.push_back(v.get<std::string>());
    }
    if (j.contains("feedback") && j["feedback"].is_object()) {
        const auto& f = j["feedback"];
        t.successText = f.value("success", std::string());
        t.failureText = f.value("failure", std::string());
        for (auto it = f.begin(); it != f.end(); ++it) {
            if (it.key() != "success" && it.key() != "failure" && it.value().is_string()) {
                t.optionFeedback[it.key()] = it.value().get<std::string>();
            }
        }
    }
    t.demo = j.value("demo", std::string());
    if (j.contains("contact") && (j["contact"].is_object() || j["contact"].is_string())) {
        t.hasContact = true;
        t.contact = ParseContactPredicate(j["contact"]);
    }
    t.completionHoldTicks = j.value("completionHoldTicks", -1);
    t.restoreOnSuccess = j.value("restoreOnSuccess", false);
    if (j.contains("afterWake") && j["afterWake"].is_object()) {
        t.afterWakeMaxTicks = j["afterWake"].value("maxTicks", 0);
    }
    if (j.contains("dummyState")) {
        if (j["dummyState"].is_string()) {
            t.dummyState = j["dummyState"].get<std::string>();
        } else if (j["dummyState"].is_array()) {
            for (const auto& v : j["dummyState"]) {
                if (v.is_number_integer()) t.dummyStateMoveIds.push_back(v.get<int>());
            }
        }
    }
    if (j.contains("withinWhiffWindow") && j["withinWhiffWindow"].is_object()) {
        t.whiffWindowMaxTicks = j["withinWhiffWindow"].value("maxTicks", 0);
    }
    t.duringGap = j.value("duringGap", false);
    if (j.contains("absence") && j["absence"].is_object()) {
        const auto& ab = j["absence"];
        t.hasAbsence = true;
        if (ab.contains("forbid")) {
            if (ab["forbid"].is_string()) {
                t.absenceForbid = ab["forbid"].get<std::string>();
            } else if (ab["forbid"].is_array()) {
                for (const auto& v : ab["forbid"]) {
                    if (v.is_number_integer()) t.absenceForbidIds.push_back(v.get<int>());
                }
            }
        }
        t.absenceStart = ab.value("start", std::string("launched"));
        t.absenceFailOnHit = ab.value("failOnHit", false);
        t.absenceMinBlocks = ab.value("minBlocks", 0);
        if (ab.contains("end")) {
            if (ab["end"].is_string()) {
                t.absenceEnd = ab["end"].get<std::string>();
            } else if (ab["end"].is_object()) {
                t.absenceEnd = "ticks";
                t.absenceTicks = ab["end"].value("ticks", 0);
            }
        }
    }
    t.endsCombo = j.value("endsCombo", false);
    if (j.contains("compare") && j["compare"].is_object()) {
        const auto& c = j["compare"];
        t.hasCompare = true;
        t.compareVs = c.value("vs", std::string());
        t.compareMetric = c.value("metric", std::string());
        t.compareOp = c.value("op", std::string());
    }
    if (j.contains("branch") && j["branch"].is_object()) {
        const auto& b = j["branch"];
        t.hasBranch = true;
        if (b.contains("starterMoveIds") && b["starterMoveIds"].is_array()) {
            for (const auto& v : b["starterMoveIds"]) {
                if (v.is_number_integer()) t.branchStarterIds.push_back(v.get<int>());
            }
        }
        if (b.contains("onHit") && b["onHit"].is_array()) {
            for (const auto& a : b["onHit"]) {
                if (a.is_object()) t.branchOnHit.push_back(ParseLessonAction(a));
            }
        }
        t.branchOnBlockHoldTicks = b.value("onBlockHoldTicks", 0);
    }
    if (j.contains("goalState") && j["goalState"].is_object()) {
        const auto& g = j["goalState"];
        t.hasGoalState = true;
        t.goalState.field = g.value("field", std::string());
        t.goalState.player = g.value("player", 1);
        t.goalState.op = g.value("op", std::string("ge"));
        t.goalState.value = g.value("value", 0);
        t.goalState.requiresBlock = g.value("requiresBlock", false);
    }
    if (j.contains("projectileInterception") &&
        j["projectileInterception"].is_object()) {
        const auto& e = j["projectileInterception"];
        t.hasProjectileInterception = true;
        t.incomingProjectilePattern = e.value("incomingPattern", 0);
        t.guardProjectilePattern = e.value("guardPattern", 0);
    }
    if (j.contains("pos") && j["pos"].is_object()) {
        t.hasPos = true;
        t.posX = j["pos"].value("x", 0.0f);
        t.posY = j["pos"].value("y", 0.0f);
    }
    if (j.contains("dummyPos") && j["dummyPos"].is_object()) {
        t.hasDummyPos = true;
        t.dummyPosX = j["dummyPos"].value("x", 0.0f);
        t.dummyPosY = j["dummyPos"].value("y", 0.0f);
    }
    if (j.contains("seed") && j["seed"].is_object()) {
        auto parseSeed = [](const json& s, TaskStateSeed& out) {
            out.rf = s.value("rf", -1);
            out.blueIC = s.value("blueIC", -1);
            out.meter = s.value("meter", -1);
            out.hp = s.value("hp", -1);
            out.guard = s.value("guard", -1);
            out.has = out.rf >= 0 || out.blueIC >= 0 ||
                      out.meter >= 0 || out.hp >= 0 || out.guard >= 0;
        };
        const auto& seed = j["seed"];
        if (seed.contains("player") && seed["player"].is_object()) {
            parseSeed(seed["player"], t.playerSeed);
        }
        if (seed.contains("dummy") && seed["dummy"].is_object()) {
            parseSeed(seed["dummy"], t.dummySeed);
        }
    }
    return t;
}

DummyEpisode ParseEpisode(const json& j) {
    DummyEpisode e;
    e.id = j.value("id", std::string());
    e.ownerTask = j.value("ownerTask", std::string());
    e.kind = j.value("kind", std::string("macro"));
    e.clip = j.value("clip", std::string());
    e.action = j.value("action", std::string());
    if (j.contains("expectedMoveIds") && j["expectedMoveIds"].is_array()) {
        for (const auto& id : j["expectedMoveIds"]) {
            if (id.is_number_integer()) e.expectedMoveIds.push_back(id.get<int>());
        }
    }
    e.approach = j.value("approach", std::string("dash"));
    e.telegraph = j.value("telegraph", std::string());
    e.trigger = j.value("trigger", std::string());
    e.reactDelay = j.value("reactDelay", 30);
    if (j.contains("start")) {
        if (j["start"].is_string()) {
            e.start = j["start"].get<std::string>();
        } else if (j["start"].is_object()) {
            e.start = j["start"].value("kind", std::string("playerState"));
            e.predicate = j["start"].value("predicate", std::string());
        }
    }
    e.predicate = j.value("predicate", e.predicate);
    if (j.contains("cue") && j["cue"].is_object()) {
        e.cueText = j["cue"].value("text", std::string());
        e.cueLead = j["cue"].value("lead", 45);
    }
    if (j.contains("repeat") && j["repeat"].is_object()) {
        e.repeatMode = j["repeat"].value("mode", std::string("afterFailure"));
        e.repeatDelay = j["repeat"].value("delay", 120);
    }
    e.pausePolicy = j.value("pausePolicy", std::string("missionFreeze"));
    e.completeOn = j.value("completeOn", std::string("actionEnd"));
    if (j.contains("variants") && j["variants"].is_array()) {
        for (const auto& v : j["variants"]) {
            if (v.is_string()) e.variants.push_back(v.get<std::string>());
        }
    }
    e.variantGroup = j.value("variantGroup", std::string());
    if (j.contains("script") && j["script"].is_array()) {
        for (const auto& st : j["script"]) {
            if (!st.is_object()) continue;
            DummyEpisode::ScriptStep step;
            step.action = st.value("action", std::string());
            step.waitTicks = st.value("waitTicks", 0);
            step.gapTicks = st.value("gapTicks", 0);
            step.cancel = st.value("cancel", false);
            e.script.push_back(step);
        }
    }
    return e;
}

json LessonPageToJson(const LessonPage& p) {
    json j;
    j["id"] = p.id;
    if (!p.title.empty()) j["title"] = p.title;
    j["text"] = p.text;
    if (!p.showHud.empty()) j["showHud"] = p.showHud;
    return j;
}

json LessonActionToJson(const LessonAction& a) {
    json j;
    j["id"] = a.id;
    if (!a.notation.empty()) j["notation"] = a.notation;
    if (!a.moveIds.empty()) j["moveIds"] = a.moveIds;
    if (!a.fromMoveIds.empty()) j["fromMoveIds"] = a.fromMoveIds;
    if (a.inputMask != 0) j["inputMask"] = a.inputMask;
    if (a.req != "move") j["req"] = a.req;
    if (a.hitsRequired > 1) j["hits"] = a.hitsRequired;
    if (a.minGap > 0) j["minGap"] = a.minGap;
    if (a.maxGap > 0) j["maxGap"] = a.maxGap;
    if (a.afterWakeMaxTicks > 0) j["afterWake"] = { {"maxTicks", a.afterWakeMaxTicks} };
    if (!a.dummyState.empty()) j["dummyState"] = a.dummyState;
    else if (!a.dummyStateMoveIds.empty()) j["dummyState"] = a.dummyStateMoveIds;
    if (a.whiffWindowMaxTicks > 0) j["withinWhiffWindow"] = { {"maxTicks", a.whiffWindowMaxTicks} };
    if (a.duringGap) j["duringGap"] = true;
    if (a.hasContact) j["contact"] = ContactPredicateToJson(a.contact);
    if (a.hasFlickerIC) {
        j["flickerIC"] = {
            {"projectilePattern", a.flickerProjectilePattern},
            {"minDistance", a.flickerMinDistance}
        };
    }
    return j;
}

json LessonTaskToJson(const LessonTask& t) {
    json j;
    j["id"] = t.id;
    j["kind"] = t.kind;
    if (!t.label.empty()) j["label"] = t.label;
    if (!t.prompt.empty()) j["prompt"] = t.prompt;
    if (t.checkpoint != "lessonStart") j["checkpoint"] = t.checkpoint;
    if (!t.script.empty()) j["script"] = t.script;
    if (t.continuity != "independent") j["continuity"] = t.continuity;
    if (!t.moveIds.empty()) j["moveIds"] = t.moveIds;
    if (t.inputMask != 0) j["inputMask"] = t.inputMask;
    if (t.req != "move") j["req"] = t.req;
    if (t.hitsRequired > 1) j["hits"] = t.hitsRequired;
    if (!t.sequence.empty()) {
        json actions = json::array();
        for (const auto& a : t.sequence) actions.push_back(LessonActionToJson(a));
        j["sequence"] = actions;
    }
    if (!t.options.empty()) {
        json arr = json::array();
        for (const auto& o : t.options) arr.push_back({ {"id", o.id}, {"label", o.label} });
        j["options"] = arr;
    }
    if (!t.acceptedOptionIds.empty()) j["acceptedOptionIds"] = t.acceptedOptionIds;
    json fb;
    if (!t.successText.empty()) fb["success"] = t.successText;
    if (!t.failureText.empty()) fb["failure"] = t.failureText;
    for (const auto& kv : t.optionFeedback) fb[kv.first] = kv.second;
    if (!fb.empty()) j["feedback"] = fb;
    if (!t.demo.empty()) j["demo"] = t.demo;
    if (t.hasContact) j["contact"] = ContactPredicateToJson(t.contact);
    if (t.completionHoldTicks >= 0) j["completionHoldTicks"] = t.completionHoldTicks;
    if (t.restoreOnSuccess) j["restoreOnSuccess"] = true;
    if (t.afterWakeMaxTicks > 0) j["afterWake"] = { {"maxTicks", t.afterWakeMaxTicks} };
    if (!t.dummyState.empty()) j["dummyState"] = t.dummyState;
    else if (!t.dummyStateMoveIds.empty()) j["dummyState"] = t.dummyStateMoveIds;
    if (t.whiffWindowMaxTicks > 0) j["withinWhiffWindow"] = { {"maxTicks", t.whiffWindowMaxTicks} };
    if (t.duringGap) j["duringGap"] = true;
    if (t.hasAbsence) {
        json ab = json::object();
        if (!t.absenceForbid.empty()) ab["forbid"] = t.absenceForbid;
        else if (!t.absenceForbidIds.empty()) ab["forbid"] = t.absenceForbidIds;
        ab["start"] = t.absenceStart;
        if (t.absenceEnd == "ticks") ab["end"] = { {"ticks", t.absenceTicks} };
        else ab["end"] = t.absenceEnd;
        if (t.absenceFailOnHit) ab["failOnHit"] = true;
        if (t.absenceMinBlocks > 0) ab["minBlocks"] = t.absenceMinBlocks;
        j["absence"] = ab;
    }
    if (t.endsCombo) j["endsCombo"] = true;
    if (t.hasCompare) {
        json c = json::object();
        c["vs"] = t.compareVs;
        c["metric"] = t.compareMetric;
        if (!t.compareOp.empty()) c["op"] = t.compareOp;
        j["compare"] = c;
    }
    if (t.hasBranch) {
        json b = json::object();
        b["starterMoveIds"] = t.branchStarterIds;
        json onHit = json::array();
        for (const auto& a : t.branchOnHit) onHit.push_back(LessonActionToJson(a));
        b["onHit"] = onHit;
        b["onBlockHoldTicks"] = t.branchOnBlockHoldTicks;
        j["branch"] = b;
    }
    if (t.hasGoalState) {
        j["goalState"] = { {"field", t.goalState.field}, {"player", t.goalState.player},
                           {"op", t.goalState.op}, {"value", t.goalState.value} };
        if (t.goalState.requiresBlock) j["goalState"]["requiresBlock"] = true;
    }
    if (t.hasProjectileInterception) {
        j["projectileInterception"] = {
            {"incomingPattern", t.incomingProjectilePattern},
            {"guardPattern", t.guardProjectilePattern}
        };
    }
    if (t.hasPos) j["pos"] = { {"x", t.posX}, {"y", t.posY} };
    if (t.hasDummyPos) {
        j["dummyPos"] = { {"x", t.dummyPosX}, {"y", t.dummyPosY} };
    }
    if (t.playerSeed.has || t.dummySeed.has) {
        auto seedJson = [](const TaskStateSeed& s) {
            json o = json::object();
            if (s.rf >= 0) o["rf"] = s.rf;
            if (s.blueIC >= 0) o["blueIC"] = s.blueIC;
            if (s.meter >= 0) o["meter"] = s.meter;
            if (s.hp >= 0) o["hp"] = s.hp;
            if (s.guard >= 0) o["guard"] = s.guard;
            return o;
        };
        json seed = json::object();
        if (t.playerSeed.has) seed["player"] = seedJson(t.playerSeed);
        if (t.dummySeed.has) seed["dummy"] = seedJson(t.dummySeed);
        j["seed"] = seed;
    }
    return j;
}

json EpisodeToJson(const DummyEpisode& e) {
    json j;
    j["id"] = e.id;
    if (!e.ownerTask.empty()) j["ownerTask"] = e.ownerTask;
    j["kind"] = e.kind;
    if (!e.clip.empty()) j["clip"] = e.clip;
    if (!e.action.empty()) j["action"] = e.action;
    if (!e.expectedMoveIds.empty()) j["expectedMoveIds"] = e.expectedMoveIds;
    if (e.approach != "dash") j["approach"] = e.approach;
    if (!e.telegraph.empty()) j["telegraph"] = e.telegraph;
    if (!e.trigger.empty()) j["trigger"] = e.trigger;
    if (e.reactDelay != 30) j["reactDelay"] = e.reactDelay;
    if (e.predicate.empty()) {
        j["start"] = e.start;
    } else {
        j["start"] = { {"kind", e.start}, {"predicate", e.predicate} };
    }
    if (!e.cueText.empty()) j["cue"] = { {"text", e.cueText}, {"lead", e.cueLead} };
    if (!e.variants.empty()) j["variants"] = e.variants;
    if (!e.variantGroup.empty()) j["variantGroup"] = e.variantGroup;
    if (!e.script.empty()) {
        json arr = json::array();
        for (const auto& st : e.script) {
            json o = json::object();
            if (!st.action.empty()) o["action"] = st.action;
            if (st.waitTicks > 0) o["waitTicks"] = st.waitTicks;
            if (st.gapTicks > 0) o["gapTicks"] = st.gapTicks;
            if (st.cancel) o["cancel"] = true;
            arr.push_back(o);
        }
        j["script"] = arr;
    }
    j["repeat"] = { {"mode", e.repeatMode}, {"delay", e.repeatDelay} };
    if (e.pausePolicy != "missionFreeze") j["pausePolicy"] = e.pausePolicy;
    if (e.completeOn != "actionEnd") j["completeOn"] = e.completeOn;
    return j;
}

} // namespace

bool ValidateLesson(const Mission& m, std::string& errorOut) {
    if (m.tutorialSchema <= 0) return true;
    auto fail = [&](const std::string& msg) { errorOut = m.lessonId + ": " + msg; return false; };
    if (m.lessonId.empty()) { errorOut = m.sourcePath + ": tutorial lesson missing stable id"; return false; }
    if (!m.hasLesson) return fail("tutorialSchema set but no lesson block");
    // rf_lock enforces a value; a lock without an authored rf has nothing to hold.
    if (m.player.rfLock && m.player.rf < 0) {
        return fail("player rfLock needs an authored rf value");
    }
    if (m.dummy.rfLock && m.dummy.rf < 0) {
        return fail("dummy rfLock needs an authored rf value");
    }
    const Lesson& L = m.lesson;
    if (L.completion != "pages" && L.completion != "allTasks" && L.completion != "anyTask") {
        return fail("completion mode must be pages/allTasks/anyTask");
    }
    if (L.requirementPlacement != "upperLeft" &&
        L.requirementPlacement != "belowStats") {
        return fail("requirementPlacement must be upperLeft/belowStats");
    }
    if (L.completion == "pages" && L.pages.empty()) return fail("pages completion with no pages");
    if (L.completion != "pages" && L.tasks.empty()) return fail("task completion with no tasks");
    if (L.wrongAction != "coach" && L.wrongAction != "fail" && L.wrongAction != "ignore") {
        return fail("flow.wrongAction must be coach/fail/ignore");
    }
    if (L.failureReset != "taskCheckpoint" && L.failureReset != "lessonStart") {
        return fail("flow.failureReset must be taskCheckpoint/lessonStart");
    }
    auto validReq = [](const std::string& req) {
        return req == "input" || req == "commit" || req == "move" ||
               req == "land" || req == "hits" || req == "connect" ||
               req == "block" || req == "rg" ||
               req == "projectileInterception";
    };
    auto validContinuity = [](const std::string& continuity) {
        return continuity == "independent" || continuity == "sameCombo" ||
               continuity == "setupGap" || continuity == "newCombo" ||
               continuity == "free" || continuity == "continue";
    };
    auto validateContact = [&](const ContactPredicate& c, const std::string& owner) {
        if (c.attacker != "learner" && c.attacker != "dummy") {
            return fail(owner + " contact attacker must be learner/dummy");
        }
        if (c.target != "learner" && c.target != "dummy") {
            return fail(owner + " contact target must be learner/dummy");
        }
        if (c.attacker == c.target) return fail(owner + " contact attacker and target must differ");
        if (c.source != "direct" && c.source != "entity") {
            return fail(owner + " contact source must be direct/entity");
        }
        if (c.result != "hit" && c.result != "block" &&
            c.result != "recoil_guard" && c.result != "throw" &&
            c.result != "special" && c.result != "guard_point" &&
            c.result != "whiff") {
            return fail(owner + " contact has unknown result '" + c.result + "'");
        }
        if (!c.targetStateBefore.empty() && c.targetStateBefore != "downed") {
            return fail(owner + " contact has unknown targetStateBefore '" +
                        c.targetStateBefore + "'");
        }
        if (c.count < 1) return fail(owner + " contact count must be at least 1");
        if (c.moveIds.empty()) return fail(owner + " contact needs committed moveIds");
        for (int id : c.moveIds) if (id <= 0) return fail(owner + " contact has non-positive move ID");
        return true;
    };
    std::vector<std::string> seen;
    auto unique = [&](const std::string& id, const char* what) {
        if (id.empty()) { errorOut = m.lessonId + ": empty " + std::string(what) + " id"; return false; }
        for (const auto& s : seen) if (s == id) { errorOut = m.lessonId + ": duplicate id '" + id + "'"; return false; }
        seen.push_back(id);
        return true;
    };
    for (const auto& p : L.pages)    if (!unique(p.id, "page")) return false;
    for (const auto& e : L.episodes) if (!unique(e.id, "episode")) return false;
    for (const auto& t : L.tasks) {
        if (!unique(t.id, "task")) return false;
        if (t.kind == "choice") {
            if (t.options.size() < 2 || t.options.size() > 3) return fail("choice task '" + t.id + "' needs 2..3 options");
            if (t.acceptedOptionIds.empty()) return fail("choice task '" + t.id + "' has no accepted options");
            if (t.prompt.empty()) return fail("choice task '" + t.id + "' has no prompt");
            std::vector<std::string> optSeen;
            for (const auto& o : t.options) {
                if (o.id.empty()) return fail("choice task '" + t.id + "' has an empty option id");
                if (o.label.empty()) return fail("choice task '" + t.id + "' option '" + o.id + "' has no label");
                for (const auto& s : optSeen) if (s == o.id) return fail("choice task '" + t.id + "' duplicate option id '" + o.id + "'");
                optSeen.push_back(o.id);
            }
            for (const auto& accepted : t.acceptedOptionIds) {
                if (std::find(optSeen.begin(), optSeen.end(), accepted) == optSeen.end()) {
                    return fail("choice task '" + t.id + "' accepts missing option '" + accepted + "'");
                }
            }
            for (const auto& fb : t.optionFeedback) {
                if (std::find(optSeen.begin(), optSeen.end(), fb.first) == optSeen.end()) {
                    return fail("choice task '" + t.id + "' has feedback for missing option '" + fb.first + "'");
                }
            }
        } else if (t.kind == "combat") {
            if (!validContinuity(t.continuity)) {
                return fail("combat task '" + t.id + "' has unknown continuity '" + t.continuity + "'");
            }
            if (!validReq(t.req)) {
                return fail("combat task '" + t.id + "' has unknown req '" + t.req + "'");
            }
            if (t.hitsRequired < 1) return fail("combat task '" + t.id + "' has hits below 1");
            if (t.inputMask < 0 || t.inputMask > 255) {
                return fail("combat task '" + t.id + "' inputMask is outside 0..255");
            }
            if (!t.sequence.empty() && (!t.moveIds.empty() || t.inputMask != 0)) {
                return fail("combat task '" + t.id + "' cannot mix sequence with task-level moveIds/inputMask");
            }
            if (!t.sequence.empty() && t.hasContact) {
                return fail("combat task '" + t.id + "' cannot mix task contact with sequence actions");
            }
            if (t.completionHoldTicks < -1) {
                return fail("combat task '" + t.id + "' completionHoldTicks must be -1 or greater");
            }
            if (t.afterWakeMaxTicks < 0) {
                return fail("combat task '" + t.id + "' afterWake.maxTicks must be positive");
            }
            auto validDummyState = [&](const std::string& st,
                                       const std::vector<int>& ids,
                                       const std::string& owner) {
                if (!st.empty() && st != "airborne" && st != "downed" &&
                    st != "airtech" && st != "launched" && st != "blockstun") {
                    return fail(owner + " has unknown dummyState '" + st + "'");
                }
                for (int id : ids) {
                    if (id <= 0) return fail(owner + " dummyState has non-positive move ID");
                }
                return true;
            };
            if (!validDummyState(t.dummyState, t.dummyStateMoveIds,
                                 "combat task '" + t.id + "'")) return false;
            if (t.whiffWindowMaxTicks < 0) {
                return fail("combat task '" + t.id + "' withinWhiffWindow.maxTicks must be positive");
            }
            if (t.hasCompare) {
                if (t.compareVs.empty()) {
                    return fail("combat task '" + t.id + "' compare needs a vs task id");
                }
                bool earlier = false;
                for (const auto& prior : L.tasks) {
                    if (&prior == &t) break;
                    if (prior.id == t.compareVs) { earlier = true; break; }
                }
                if (!earlier) {
                    return fail("combat task '" + t.id + "' compare.vs must reference an EARLIER task");
                }
                if (t.compareMetric != "comboHits" && t.compareMetric != "comboDamage" &&
                    t.compareMetric != "ticks" && t.compareMetric != "p2HpDelta" &&
                    t.compareMetric != "p1RfDelta" && t.compareMetric != "p2GuardDelta" &&
                    t.compareMetric != "maxUntech" &&
                    t.compareMetric != "untechTicks") {
                    return fail("combat task '" + t.id + "' compare has unknown metric '" + t.compareMetric + "'");
                }
                if (!t.compareOp.empty() && t.compareOp != "gt" && t.compareOp != "lt" &&
                    t.compareOp != "ge" && t.compareOp != "le") {
                    return fail("combat task '" + t.id + "' compare has unknown op '" + t.compareOp + "'");
                }
            }
            if (t.hasBranch) {
                if (!t.moveIds.empty() || t.inputMask != 0 || !t.sequence.empty() ||
                    t.hasContact || t.hasGoalState || t.hasAbsence ||
                    t.hasProjectileInterception) {
                    return fail("combat task '" + t.id + "' branch cannot mix with other contracts");
                }
                if (t.branchStarterIds.empty()) {
                    return fail("combat task '" + t.id + "' branch needs starterMoveIds");
                }
                for (int id : t.branchStarterIds) {
                    if (id <= 0) return fail("combat task '" + t.id + "' branch has non-positive starter ID");
                }
                if (t.branchOnHit.empty()) {
                    return fail("combat task '" + t.id + "' branch needs an onHit continuation");
                }
                if (t.branchOnBlockHoldTicks < 1) {
                    return fail("combat task '" + t.id + "' branch onBlockHoldTicks must be positive");
                }
                std::vector<std::string> branchIds;
                for (const auto& a : t.branchOnHit) {
                    if (a.id.empty()) return fail("combat task '" + t.id + "' branch action has empty id");
                    if (std::find(branchIds.begin(), branchIds.end(), a.id) != branchIds.end()) {
                        return fail("combat task '" + t.id + "' branch duplicates action '" + a.id + "'");
                    }
                    branchIds.push_back(a.id);
                    if (!validReq(a.req)) {
                        return fail("combat task '" + t.id + "' branch action '" + a.id + "' has unknown req");
                    }
                    if (a.req != "input" && a.moveIds.empty()) {
                        return fail("combat task '" + t.id + "' branch action '" + a.id + "' needs moveIds");
                    }
                    for (int id : a.moveIds) {
                        if (id <= 0) return fail("combat task '" + t.id + "' branch action '" + a.id + "' has non-positive move ID");
                    }
                }
            }
            if (t.endsCombo && (t.hasGoalState || t.hasAbsence || t.hasProjectileInterception)) {
                return fail("combat task '" + t.id + "' endsCombo needs a move/sequence/branch contract");
            }
            if (t.hasAbsence) {
                if (!t.moveIds.empty() || t.inputMask != 0 || !t.sequence.empty() ||
                    t.hasContact || t.hasGoalState || t.hasProjectileInterception) {
                    return fail("combat task '" + t.id + "' absence cannot mix with other contracts");
                }
                if (t.absenceForbid.empty() && t.absenceForbidIds.empty()) {
                    return fail("combat task '" + t.id + "' absence names nothing to forbid");
                }
                if (!t.absenceForbid.empty() && t.absenceForbid != "airtech" &&
                    t.absenceForbid != "attack") {
                    return fail("combat task '" + t.id + "' absence forbid must be airtech/attack or move IDs");
                }
                for (int id : t.absenceForbidIds) {
                    if (id <= 0) return fail("combat task '" + t.id + "' absence has non-positive move ID");
                }
                if (t.absenceStart != "launched" && t.absenceStart != "taskArmed" &&
                    t.absenceStart != "dummyUntech") {
                    return fail("combat task '" + t.id +
                                "' absence start must be launched/taskArmed/dummyUntech");
                }
                if (t.absenceEnd != "grounded" && t.absenceEnd != "ticks" &&
                    t.absenceEnd != "dummyUntechEmpty" &&
                    t.absenceEnd != "dummyAttackEnd" &&
                    t.absenceEnd != "episodeCycleEnd") {
                    return fail("combat task '" + t.id +
                                "' absence end must be grounded/ticks/dummyUntechEmpty/dummyAttackEnd/episodeCycleEnd");
                }
                if (t.absenceEnd == "ticks" && t.absenceTicks < 1) {
                    return fail("combat task '" + t.id + "' absence end.ticks must be positive");
                }
                if (t.absenceMinBlocks < 0) {
                    return fail("combat task '" + t.id + "' absence minBlocks must be nonnegative");
                }
                if (t.absenceEnd == "dummyAttackEnd" &&
                    (t.absenceStart != "taskArmed" || t.script.empty())) {
                    return fail("combat task '" + t.id +
                                "' dummyAttackEnd needs a taskArmed window and scripted dummy attack");
                }
                if (t.absenceEnd == "episodeCycleEnd" &&
                    (t.absenceStart != "taskArmed" || t.script.empty())) {
                    return fail("combat task '" + t.id +
                                "' episodeCycleEnd needs a taskArmed window and scripted dummy episode");
                }
            }
            {
                auto validSeed = [&](const TaskStateSeed& s, const char* side) {
                    if (!s.has) return true;
                    if (s.rf > 1000) {
                        return fail("combat task '" + t.id + "' " + side + " seed rf must be inside 0..1000");
                    }
                    if (s.blueIC > 1) {
                        return fail("combat task '" + t.id + "' " + side + " seed blueIC must be 0 or 1");
                    }
                    if (s.meter > 30000) {
                        return fail("combat task '" + t.id + "' " + side + " seed meter is out of range");
                    }
                    if (s.hp == 0 || s.hp > 99999) {
                        return fail("combat task '" + t.id + "' " + side + " seed hp is out of range");
                    }
                    if (s.guard > 360) {
                        return fail("combat task '" + t.id + "' " + side + " seed guard must be inside 0..360");
                    }
                    return true;
                };
                if (!validSeed(t.playerSeed, "player") ||
                    !validSeed(t.dummySeed, "dummy")) return false;
            }
            if (t.req == "projectileInterception" &&
                !t.hasProjectileInterception) {
                return fail("combat task '" + t.id + "' req=projectileInterception needs projectileInterception");
            }
            if (t.hasProjectileInterception &&
                t.req != "projectileInterception") {
                return fail("combat task '" + t.id + "' projectileInterception needs req=projectileInterception");
            }
            if (t.hasProjectileInterception &&
                (t.incomingProjectilePattern <= 0 ||
                 t.incomingProjectilePattern > 65535 ||
                 t.guardProjectilePattern <= 0 ||
                 t.guardProjectilePattern > 65535)) {
                return fail("combat task '" + t.id + "' projectileInterception patterns must be inside 1..65535");
            }
            if (t.hasContact && !validateContact(t.contact, "combat task '" + t.id + "'")) return false;
            if (t.hasGoalState) {
                if (!::Mission::TutorialStatePolicy::IsKnownField(t.goalState.field)) {
                    return fail("combat task '" + t.id + "' goalState has unknown field '" + t.goalState.field + "'");
                }
                if (!::Mission::TutorialStatePolicy::IsKnownOperator(t.goalState.op)) {
                    return fail("combat task '" + t.id + "' goalState has unknown op '" + t.goalState.op + "'");
                }
                if (t.goalState.player != 1 && t.goalState.player != 2) {
                    return fail("combat task '" + t.id + "' goalState player must be 1 or 2");
                }
                if (t.goalState.requiresBlock && t.goalState.player != 1) {
                    return fail("combat task '" + t.id +
                                "' goalState.requiresBlock only supports the learner");
                }
                if (!t.moveIds.empty() || t.inputMask != 0 || !t.sequence.empty() || t.hasContact) {
                    return fail("combat task '" + t.id + "' goalState cannot mix with a move/input/sequence contract");
                }
            }
            // Combat tasks with no committed-action contract are allowed only
            // for legacy top-level step adapters or a goalState objective.
            // Placeholder move ID 0 stays parseable so development rows can
            // remain visible, but runtime preflight refuses to launch it.
            if (!t.hasGoalState && !t.hasContact && !t.hasAbsence && !t.hasBranch && t.sequence.empty() && t.moveIds.empty() && t.inputMask == 0 && m.steps.empty()) {
                return fail("combat task '" + t.id + "' names no committed action and the lesson has no steps");
            }
            if (t.sequence.empty() && t.req == "input" && t.inputMask == 0 && !t.hasGoalState) {
                return fail("combat task '" + t.id + "' req=input needs inputMask");
            }
            if (t.sequence.empty() && t.req == "commit" &&
                (t.inputMask == 0 || t.moveIds.empty()) && !t.hasGoalState) {
                return fail("combat task '" + t.id + "' req=commit needs inputMask and moveIds");
            }
            if (!t.hasGoalState && !t.hasContact && !t.hasAbsence && !t.hasBranch && t.sequence.empty() && t.req != "input" && t.req != "commit" && t.moveIds.empty() && m.steps.empty()) {
                return fail("combat task '" + t.id + "' req=" + t.req + " needs moveIds");
            }
            std::vector<std::string> actionIds;
            for (size_t actionIndex = 0; actionIndex < t.sequence.size(); ++actionIndex) {
                const auto& a = t.sequence[actionIndex];
                if (a.id.empty()) return fail("combat task '" + t.id + "' has sequence action with empty id");
                if (std::find(actionIds.begin(), actionIds.end(), a.id) != actionIds.end()) {
                    return fail("combat task '" + t.id + "' duplicates sequence action '" + a.id + "'");
                }
                actionIds.push_back(a.id);
                if (!validReq(a.req)) return fail("combat task '" + t.id + "' action '" + a.id + "' has unknown req '" + a.req + "'");
                if (a.hitsRequired < 1) return fail("combat task '" + t.id + "' action '" + a.id + "' has hits below 1");
                if (a.inputMask < 0 || a.inputMask > 255) return fail("combat task '" + t.id + "' action '" + a.id + "' inputMask is outside 0..255");
                if (a.minGap < 0 || a.maxGap < 0 || (a.maxGap > 0 && a.minGap > a.maxGap)) {
                    return fail("combat task '" + t.id + "' action '" + a.id + "' has an invalid gap window");
                }
                if (a.afterWakeMaxTicks < 0) {
                    return fail("combat task '" + t.id + "' action '" + a.id + "' afterWake.maxTicks must be positive");
                }
                if (a.whiffWindowMaxTicks < 0) {
                    return fail("combat task '" + t.id + "' action '" + a.id + "' withinWhiffWindow.maxTicks must be positive");
                }
                if (!validDummyState(a.dummyState, a.dummyStateMoveIds,
                                     "combat task '" + t.id + "' action '" + a.id + "'")) return false;
                if (a.req == "input" && a.inputMask == 0) return fail("combat task '" + t.id + "' action '" + a.id + "' req=input needs inputMask");
                if (a.req == "commit" && (a.inputMask == 0 || a.moveIds.empty())) return fail("combat task '" + t.id + "' action '" + a.id + "' req=commit needs inputMask and moveIds");
                if (a.req != "input" && a.moveIds.empty() && !a.hasContact) return fail("combat task '" + t.id + "' action '" + a.id + "' needs moveIds");
                for (int id : a.moveIds) if (id <= 0) return fail("combat task '" + t.id + "' action '" + a.id + "' has non-positive move ID");
                if (actionIndex == 0 && !a.fromMoveIds.empty()) {
                    return fail("combat task '" + t.id + "' first action cannot require a source move");
                }
                for (int id : a.fromMoveIds) {
                    if (id <= 0) return fail("combat task '" + t.id + "' action '" + a.id + "' has non-positive source move ID");
                }
                if (a.hasFlickerIC) {
                    if (a.req != "commit" || a.inputMask == 0 ||
                        a.fromMoveIds.empty()) {
                        return fail("combat task '" + t.id + "' action '" + a.id +
                                    "' flickerIC needs a committed input and source move");
                    }
                    if (a.flickerProjectilePattern <= 0 ||
                        a.flickerProjectilePattern > 65535) {
                        return fail("combat task '" + t.id + "' action '" + a.id +
                                    "' flickerIC projectilePattern must be inside 1..65535");
                    }
                    // minDistance 0/omitted = no spacing gate: the FIC proof
                    // then rests on the live projectile, no-contact, and
                    // untouched-defender invariants alone.
                    if (a.flickerMinDistance < 0) {
                        return fail("combat task '" + t.id + "' action '" + a.id +
                                    "' flickerIC minDistance must not be negative");
                    }
                }
                if (a.hasContact && !validateContact(a.contact,
                    "combat task '" + t.id + "' action '" + a.id + "'")) return false;
            }
        } else {
            return fail("task '" + t.id + "' has unknown kind '" + t.kind + "'");
        }
        if (!t.script.empty()) {
            const DummyEpisode* found = nullptr;
            for (const auto& e : L.episodes) if (e.id == t.script) { found = &e; break; }
            if (!found) return fail("task '" + t.id + "' references missing episode '" + t.script + "'");
            if (found->ownerTask != t.id) {
                return fail("task '" + t.id + "' references episode '" + t.script +
                            "' owned by task '" + found->ownerTask + "'");
            }
        }
    }
    for (const auto& e : L.episodes) {
        bool ownerFound = false;
        for (const auto& t : L.tasks) if (t.id == e.ownerTask) { ownerFound = true; break; }
        if (!ownerFound) return fail("episode '" + e.id + "' references missing owner task '" + e.ownerTask + "'");
        if (e.kind != "macro" && e.kind != "idle" &&
            e.kind != "block" && e.kind != "rg" &&
            e.kind != "held" && e.kind != "guard" && e.kind != "cue" &&
            e.kind != "block_answer" && e.kind != "rg_answer" &&
            e.kind != "airtech") {
            return fail("episode '" + e.id + "' has unknown kind '" + e.kind + "'");
        }
        // macro drives via `action` (injection) - empty clip is fine; held/guard
        // still need a spec; idle/block/rg/cue need neither. airtech's clip is
        // its direction; the answer composites drive via `action` like macro.
        if (e.kind == "held" || e.kind == "guard") {
            if (e.clip.empty()) return fail("episode '" + e.id + "' has no clip");
        }
        if (e.kind == "airtech" &&
            e.clip != "forward" && e.clip != "backward") {
            return fail("episode '" + e.id + "' airtech clip must be forward or backward");
        }
        if ((e.kind == "macro" || e.kind == "block_answer" || e.kind == "rg_answer") &&
            e.script.empty() &&
            !::Mission::TutorialEpisodePolicy::IsInjectableAction(e.action)) {
            return fail("episode '" + e.id + "' has unsupported action '" + e.action + "'");
        }
        if (!e.script.empty()) {
            // episode_script: scripted multi-action string.
            if (e.kind != "macro") {
                return fail("episode '" + e.id + "' script requires kind macro");
            }
            if (!e.action.empty()) {
                return fail("episode '" + e.id + "' script replaces the single action - leave action empty");
            }
            if (e.start != "taskArmed") {
                return fail("episode '" + e.id + "' script requires taskArmed start");
            }
            if (e.script.front().action.empty()) {
                return fail("episode '" + e.id + "' script must begin with an action step");
            }
            for (size_t si = 0; si < e.script.size(); ++si) {
                const auto& st = e.script[si];
                if (!st.action.empty() && st.gapTicks > 0) {
                    return fail("episode '" + e.id + "' script step mixes action and gap");
                }
                if (st.action.empty() && st.gapTicks <= 0) {
                    return fail("episode '" + e.id + "' script step is neither action nor gap");
                }
                if (st.cancel && (si == 0 || st.action.empty())) {
                    return fail("episode '" + e.id + "' cancel step needs a prior action and its own action");
                }
                if (!st.action.empty() &&
                    !::Mission::TutorialEpisodePolicy::IsSupportedScriptAction(
                        st.action, si)) {
                    return fail("episode '" + e.id + "' script action '" + st.action +
                                "' cannot ride the ordinary injection lanes");
                }
                if (st.waitTicks < 0 || st.gapTicks < 0) {
                    return fail("episode '" + e.id + "' script step has a negative delay");
                }
            }
        }
        if (!e.variants.empty()) {
            bool kindInPool = false;
            for (const auto& v : e.variants) {
                if (v != "idle" && v != "block" && v != "rg") {
                    return fail("episode '" + e.id + "' variant '" + v +
                                "' is not a lease-only kind");
                }
                if (v == e.kind) kindInPool = true;
            }
            if (!kindInPool) {
                return fail("episode '" + e.id + "' kind must be one of its variants");
            }
        }
        if (!e.variantGroup.empty() && e.variants.empty()) {
            return fail("episode '" + e.id + "' variantGroup needs a variants pool");
        }
        if (e.kind == "macro" &&
            (::Mission::TutorialEpisodePolicy::IsDashNormalAction(e.action) ||
             ::Mission::TutorialEpisodePolicy::IsContactChainAction(e.action)) &&
            (e.start != "taskArmed" || e.approach != "none")) {
            return fail("episode '" + e.id + "' scripted action '" + e.action +
                        "' requires taskArmed start and no approach");
        }
        if (!::Mission::TutorialEpisodePolicy::IsSupportedApproach(e.approach)) {
            return fail("episode '" + e.id + "' has unsupported approach '" + e.approach + "'");
        }
        if (!::Mission::TutorialEpisodePolicy::IsSupportedTelegraph(e.telegraph)) {
            return fail("episode '" + e.id + "' has unsupported telegraph '" + e.telegraph + "'");
        }
        if (!e.telegraph.empty()) {
            // 2026-07-14: widened from dash normals to ANY authored opener -
            // buffered specials (e.g. Rumi's armored 41236C) are just as
            // unreactable from a standing start, and the hop is the tell that
            // gives the learner time to move in before the window opens.
            const bool hasOpener =
                (!e.action.empty() && e.action != "jump") ||
                (!e.script.empty() && !e.script.front().action.empty());
            if (e.kind != "macro" || e.start != "taskArmed" || !hasOpener) {
                return fail("episode '" + e.id +
                            "' telegraph needs a taskArmed macro opener");
            }
        }
        if (e.start != "taskArmed" && e.start != "playerState" && e.start != "trigger") {
            return fail("episode '" + e.id + "' has unknown start '" + e.start + "'");
        }
        if (e.start == "playerState" && e.predicate.empty()) {
            return fail("episode '" + e.id + "' playerState start has no predicate");
        }
        if (e.start == "playerState") {
            int ignoredMove = 0;
            if (!::Mission::TutorialEpisodePolicy::ParseP1MovePredicate(
                    e.predicate, ignoredMove)) {
                return fail("episode '" + e.id + "' has unsupported playerState predicate '" +
                            e.predicate + "'");
            }
        }
        if (e.start == "trigger" && e.trigger.empty()) {
            return fail("episode '" + e.id + "' trigger start has no trigger");
        }
        if (e.start == "trigger" &&
            !::Mission::TutorialEpisodePolicy::IsSupportedTrigger(e.trigger)) {
            return fail("episode '" + e.id + "' has unsupported trigger '" + e.trigger + "'");
        }
        if (!e.expectedMoveIds.empty()) {
            if (e.kind != "macro" || e.action.empty()) {
                return fail("episode '" + e.id +
                            "' expectedMoveIds need a macro action");
            }
            const bool nativeWake =
                ::Mission::TutorialEpisodePolicy::UsesNativeWakeProducer(
                    e.kind, e.start, e.trigger);
            const bool supportedOrdinaryGround = e.start == "taskArmed" &&
                e.action != "jump" && e.action.rfind("j.", 0) != 0 &&
                !::Mission::TutorialEpisodePolicy::IsDashNormalAction(e.action) &&
                !::Mission::TutorialEpisodePolicy::IsContactChainAction(e.action);
            if (!nativeWake && !supportedOrdinaryGround) {
                return fail("episode '" + e.id +
                            "' expectedMoveIds are not supported by this driver");
            }
            std::set<int> uniqueExpected;
            for (int moveId : e.expectedMoveIds) {
                if (moveId < 200 || moveId > 32767 ||
                    !uniqueExpected.insert(moveId).second) {
                    return fail("episode '" + e.id +
                                "' has an invalid expected move ID");
                }
            }
        }
        if (!::Mission::TutorialEpisodePolicy::IsSupportedRepeatMode(e.repeatMode)) {
            return fail("episode '" + e.id + "' has unknown repeat mode '" + e.repeatMode + "'");
        }
        if (e.pausePolicy != "missionFreeze") {
            return fail("episode '" + e.id + "' has unsupported pause policy '" +
                        e.pausePolicy + "'");
        }
        if (e.completeOn != "actionEnd") {
            return fail("episode '" + e.id + "' has unsupported completion event '" +
                        e.completeOn + "'");
        }
        if (e.cueLead < 0 || e.repeatDelay < 0) return fail("episode '" + e.id + "' has a negative timing value");
    }
    // A named group is an author-visible promise that its exercises draw from
    // one balanced bag. Reject partial/mismatched groups at load time instead
    // of quietly reverting to independent randomness.
    std::map<std::string, std::vector<const DummyEpisode*>> variantGroups;
    for (const auto& e : L.episodes) {
        if (!e.variantGroup.empty()) variantGroups[e.variantGroup].push_back(&e);
    }
    for (const auto& group : variantGroups) {
        const auto& members = group.second;
        if (members.size() < 2) {
            return fail("variantGroup '" + group.first + "' needs at least two episodes");
        }
        const auto& expected = members.front()->variants;
        for (const DummyEpisode* member : members) {
            if (member->variants != expected) {
                return fail("variantGroup '" + group.first + "' has mismatched variants pools");
            }
        }
    }
    return true;
}

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
        m.order = j.value("order", 0);
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
        // ---- tutorialSchema 1 ----
        m.tutorialSchema = j.value("tutorialSchema", 0);
        m.lessonId = j.value("id", std::string());
        m.revision = j.value("revision", 1);
        m.summary = j.value("summary", std::string());
        if (j.contains("sourceRefs") && j["sourceRefs"].is_array()) {
            for (const auto& v : j["sourceRefs"]) if (v.is_string()) m.sourceRefs.push_back(v.get<std::string>());
        }
        if (j.contains("requires") && j["requires"].is_array()) {
            for (const auto& v : j["requires"]) if (v.is_string()) m.requires.push_back(v.get<std::string>());
        }
        m.requiresExactBaseline = j.value("requiresExactBaseline", false);
        if (j.contains("lesson") && j["lesson"].is_object()) {
            const auto& lj = j["lesson"];
            m.hasLesson = true;
            m.lesson.completion = lj.value("completion", std::string());
            m.lesson.requirementPlacement =
                lj.value("requirementPlacement", std::string("upperLeft"));
            if (lj.contains("flow") && lj["flow"].is_object()) {
                m.lesson.wrongAction = lj["flow"].value("wrongAction", std::string("coach"));
                m.lesson.failureReset = lj["flow"].value("failureReset", std::string("taskCheckpoint"));
                m.lesson.preserveCompletedTasks = lj["flow"].value("preserveCompletedTasks", true);
            }
            if (lj.contains("pages") && lj["pages"].is_array()) {
                for (const auto& p : lj["pages"]) m.lesson.pages.push_back(ParseLessonPage(p));
            }
            if (lj.contains("tasks") && lj["tasks"].is_array()) {
                for (const auto& tk : lj["tasks"]) m.lesson.tasks.push_back(ParseLessonTask(tk));
            }
            if (lj.contains("dummyScript") && lj["dummyScript"].is_object() &&
                lj["dummyScript"].contains("episodes") && lj["dummyScript"]["episodes"].is_array()) {
                for (const auto& e : lj["dummyScript"]["episodes"]) m.lesson.episodes.push_back(ParseEpisode(e));
            }
        }
        if (j.contains("next") && j["next"].is_object()) {
            m.nextLessonId = j["next"].value("lessonId", std::string());
        }
        if (j.contains("recommendedAfter") && j["recommendedAfter"].is_array()) {
            for (const auto& v : j["recommendedAfter"]) if (v.is_string()) m.recommendedAfter.push_back(v.get<std::string>());
        }
        if (m.tutorialSchema > 0) {
            std::string verr;
            if (!ValidateLesson(m, verr)) { errorOut = verr; return false; }
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
        if (m.order > 0) j["order"] = m.order;
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
        // ---- tutorialSchema 1 ----
        if (m.tutorialSchema > 0) {
            j["tutorialSchema"] = m.tutorialSchema;
            j["id"] = m.lessonId;
            j["revision"] = m.revision;
            if (!m.summary.empty()) j["summary"] = m.summary;
            if (!m.sourceRefs.empty()) j["sourceRefs"] = m.sourceRefs;
            if (!m.requires.empty()) j["requires"] = m.requires;
            if (m.requiresExactBaseline) j["requiresExactBaseline"] = true;
            if (m.hasLesson) {
                json lj;
                lj["completion"] = m.lesson.completion;
                if (m.lesson.requirementPlacement != "upperLeft") {
                    lj["requirementPlacement"] = m.lesson.requirementPlacement;
                }
                lj["flow"] = { {"wrongAction", m.lesson.wrongAction},
                               {"failureReset", m.lesson.failureReset},
                               {"preserveCompletedTasks", m.lesson.preserveCompletedTasks} };
                json pages = json::array();
                for (const auto& p : m.lesson.pages) pages.push_back(LessonPageToJson(p));
                if (!pages.empty()) lj["pages"] = pages;
                json tasks = json::array();
                for (const auto& tk : m.lesson.tasks) tasks.push_back(LessonTaskToJson(tk));
                if (!tasks.empty()) lj["tasks"] = tasks;
                if (!m.lesson.episodes.empty()) {
                    json eps = json::array();
                    for (const auto& e : m.lesson.episodes) eps.push_back(EpisodeToJson(e));
                    lj["dummyScript"] = { {"episodes", eps} };
                }
                j["lesson"] = lj;
            }
            if (!m.nextLessonId.empty()) j["next"] = { {"lessonId", m.nextLessonId} };
            if (!m.recommendedAfter.empty()) j["recommendedAfter"] = m.recommendedAfter;
        }
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
        p.id = j.value("id", std::string());
        p.name = j.value("name", std::string());
        p.author = j.value("author", std::string());
        p.character = j.value("character", std::string());
        p.description = j.value("description", std::string());
        p.curriculumRevision = j.value("curriculumRevision", 0);
        if (j.contains("categories") && j["categories"].is_array()) {
            for (const auto& c : j["categories"]) {
                PackCategory cat;
                cat.id = c.value("id", std::string());
                cat.label = c.value("label", std::string());
                cat.description = c.value("description", std::string());
                cat.order = c.value("order", 0);
                p.categories.push_back(std::move(cat));
            }
        }
        if (j.contains("scenarios") && j["scenarios"].is_array()) {
            for (const auto& s : j["scenarios"]) {
                Scenario sc;
                sc.name = s.value("name", std::string());
                sc.file = s.value("file", std::string());
                sc.description = s.value("description", std::string());
                sc.preview = s.value("preview", std::string());
                sc.locked = s.value("locked", s.value("may_be_locked", false));
                sc.id = s.value("id", std::string());
                sc.order = s.value("order", 0);
                sc.difficulty = s.value("difficulty", 0);
                sc.next = s.value("next", std::string());
                if (s.contains("recommendedAfter") && s["recommendedAfter"].is_array()) {
                    for (const auto& v : s["recommendedAfter"]) {
                        if (v.is_string()) sc.recommendedAfter.push_back(v.get<std::string>());
                    }
                }
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
        if (!p.id.empty()) j["id"] = p.id;
        j["name"] = p.name;
        if (!p.author.empty()) j["author"] = p.author;
        if (!p.character.empty()) j["character"] = p.character;
        if (!p.description.empty()) j["description"] = p.description;
        if (p.curriculumRevision > 0) j["curriculumRevision"] = p.curriculumRevision;
        if (!p.categories.empty()) {
            json cats = json::array();
            for (const PackCategory& c : p.categories) {
                json cj;
                cj["id"] = c.id;
                cj["label"] = c.label;
                if (!c.description.empty()) cj["description"] = c.description;
                cj["order"] = c.order;
                cats.push_back(cj);
            }
            j["categories"] = cats;
        }
        j["scenarios"] = json::array();
        for (const Scenario& s : p.scenarios) {
            json sj;
            sj["name"] = s.name;
            sj["file"] = s.file;
            if (!s.description.empty()) sj["description"] = s.description;
            if (!s.preview.empty()) sj["preview"] = s.preview;
            if (s.locked) sj["locked"] = true;
            if (!s.id.empty()) sj["id"] = s.id;
            if (s.order > 0) sj["order"] = s.order;
            if (s.difficulty > 0) sj["difficulty"] = s.difficulty;
            if (!s.next.empty()) sj["next"] = s.next;
            if (!s.recommendedAfter.empty()) sj["recommendedAfter"] = s.recommendedAfter;
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
