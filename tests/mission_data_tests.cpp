#include "game/mission/mission_data.h"
#include "game/mission/tutorial_support.h"

#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <process.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

// tutorial_support.cpp's policy code is deliberately exercised in this small
// test binary. Progress-store diagnostics are irrelevant here, so satisfy its
// logger boundary without pulling the injected DLL's logging subsystem in.
void LogOut(const std::string&, bool) {}

namespace {

void Check(bool condition, const char* message) {
    if (condition) return;
    throw std::runtime_error(message);
}

std::string ReadAll(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

class TempArtifactCleanup {
public:
    TempArtifactCleanup(std::initializer_list<std::filesystem::path> paths)
        : paths_(paths) {}

    ~TempArtifactCleanup() {
        for (const auto& path : paths_) {
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
        }
    }

    TempArtifactCleanup(const TempArtifactCleanup&) = delete;
    TempArtifactCleanup& operator=(const TempArtifactCleanup&) = delete;

private:
    std::vector<std::filesystem::path> paths_;
};

} // namespace

int main(int argc, char** argv) {
    // CTest may execute Release and Debug configurations concurrently. Keep
    // their destructive JSON fixtures process-local instead of letting one
    // binary truncate the other's file in the shared build directory.
    const std::string fixtureSuffix = "_" + std::to_string(_getpid()) + ".json";
    const std::string path = "mission_data_boundary_test" + fixtureSuffix;
    const std::string recordedPath = "recorded_legacy_in_range" + fixtureSuffix;
    const std::string lessonPath = "tutorial_roundtrip" + fixtureSuffix;
    TempArtifactCleanup cleanup({path, recordedPath, lessonPath});

    try {
        Mission::Mission mission;
    mission.name = "boundary serde";
    Mission::Step first;
    first.notation = "2C";
    first.moveIds = {206};
    first.comboEndAfter = true;
    Mission::Step second;
    second.notation = "j.B";
    second.moveIds = {208};
    second.maxGap = 460;
    mission.steps = {first, second};

    std::string error;
    Check(Mission::SaveMission(path, mission, error), "save boundary mission");
    const std::string encoded = ReadAll(path);
    Check(encoded.find("\"comboEndAfter\": true") != std::string::npos,
          "true boundary is serialized explicitly");

    Mission::Mission loaded;
    Check(Mission::LoadMission(path, loaded, error), "load boundary mission");
    Check(loaded.steps.size() == 2, "step count roundtrips");
    Check(loaded.steps[0].comboEndAfter, "boundary bool roundtrips");
    Check(loaded.steps[1].maxGap == 460, "delayed-button gap roundtrips");

    loaded.steps[0].comboEndAfter = false;
    Check(Mission::SaveMission(path, loaded, error), "save legacy-compatible mission");
    Check(ReadAll(path).find("comboEndAfter") == std::string::npos,
          "false boundary is omitted for legacy files");

    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << R"json({"format":1,"name":"alias","steps":[{"notation":"5A","ids":[200],"comboEnd":"must"},{"notation":"5B","ids":[201]}]})json";
    }
    Check(Mission::LoadMission(path, loaded, error), "load comboEnd must alias");
    Check(loaded.steps[0].comboEndAfter, "comboEnd must alias is accepted");

    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << R"json({"format":1,"name":"legacy buffer","bgm":59,"steps":[{"notation":"5A","ids":[200]}]})json";
    }
    Check(Mission::LoadMission(path, loaded, error), "load legacy buffer-valued BGM");
    Check(loaded.bgm == -1, "unmarked legacy buffer value falls back to native BGM");

    loaded.bgm = 59;
    Check(Mission::SaveMission(path, loaded, error), "save explicit logical BGM");
    const std::string bgmEncoded = ReadAll(path);
    Check(bgmEncoded.find("\"bgmKind\": \"track\"") != std::string::npos,
          "saved logical BGM carries provenance");
    Check(Mission::LoadMission(path, loaded, error), "reload explicit logical BGM");
    Check(loaded.bgm == 59, "explicit logical BGM is not legacy-sanitized");

    {
        std::ofstream out(recordedPath, std::ios::binary | std::ios::trunc);
        out << R"json({"format":1,"name":"legacy in-range buffer","bgm":12,"steps":[{"notation":"5A","ids":[200]}]})json";
    }
    Check(Mission::LoadMission(recordedPath, loaded, error),
          "load generated legacy recording with in-range buffer value");
    Check(loaded.bgm == -1,
          "unmarked generated recording never treats a buffer index as a logical track");

    // ---- tutorialSchema 1 round-trip + validation (§8.1) ----
    {
        Mission::Mission m;
        m.format = 1;
        m.tutorialSchema = 1;
        m.type = "tutorial";
        m.lessonId = "efz.test.lesson";
        m.revision = 3;
        m.category = "fundamentals";
        m.order = 10;
        m.difficulty = 1;
        m.name = "Test Lesson";
        m.summary = "Round-trip everything.";
        m.requires = { "pages", "tutorial_tasks" };
        m.requiresExactBaseline = true;
        m.player.character = "nayukib";
        m.player.hasPos = true;
        m.player.posX = 0.0;   // explicit zero must survive (P0.2)
        m.player.posY = 0.0;
        m.hasLesson = true;
        m.lesson.completion = "allTasks";
        m.lesson.requirementPlacement = "belowStats";
        m.lesson.pages.push_back({ "p0", "Title", "Hold {dir:4} to guard." });
        Mission::LessonTask combat;
        combat.id = "t0"; combat.kind = "combat"; combat.label = "Jump then attack";
        Mission::LessonAction jump;
        jump.id = "jump"; jump.notation = "9"; jump.moveIds = { 5 }; jump.req = "move";
        Mission::LessonAction attack;
        attack.id = "attack"; attack.notation = "j.A"; attack.moveIds = { 207 };
        attack.fromMoveIds = { 5 };
        attack.req = "land"; attack.minGap = 2; attack.maxGap = 120;
        combat.sequence = { jump, attack };
        m.lesson.tasks.push_back(combat);
        Mission::LessonTask choice;
        choice.id = "t1"; choice.kind = "choice"; choice.prompt = "Pick one";
        choice.options = { { "a", "First" }, { "b", "Second" } };
        choice.acceptedOptionIds = { "a" };
        choice.optionFeedback["b"] = "Nope.";
        m.lesson.tasks.push_back(choice);
        m.nextLessonId = "efz.test.next";
        Check(Mission::SaveMission(lessonPath, m, error), "save tutorial lesson");
    }
    Check(Mission::LoadMission(lessonPath, loaded, error), "reload tutorial lesson");
    Check(loaded.tutorialSchema == 1 && loaded.lessonId == "efz.test.lesson",
          "tutorial identity round-trips");
    Check(loaded.requiresExactBaseline, "requiresExactBaseline round-trips");
    Check(loaded.player.hasPos && loaded.player.posX == 0.0,
          "explicit zero position survives (presence-aware)");
    Check(loaded.hasLesson && loaded.lesson.pages.size() == 1 &&
          loaded.lesson.tasks.size() == 2, "lesson block round-trips");
    Check(loaded.lesson.requirementPlacement == "belowStats",
          "authored requirement placement round-trips");
    Check(loaded.lesson.tasks[1].kind == "choice" &&
          loaded.lesson.tasks[1].acceptedOptionIds.size() == 1 &&
          loaded.lesson.tasks[1].optionFeedback.count("b") == 1,
          "choice contract round-trips");
    Check(loaded.lesson.tasks[0].sequence.size() == 2 &&
          loaded.lesson.tasks[0].sequence[1].id == "attack" &&
          loaded.lesson.tasks[0].sequence[1].req == "land" &&
          loaded.lesson.tasks[0].sequence[1].fromMoveIds.size() == 1 &&
          loaded.lesson.tasks[0].sequence[1].fromMoveIds[0] == 5 &&
          loaded.lesson.tasks[0].sequence[1].minGap == 2 &&
          loaded.lesson.tasks[0].sequence[1].maxGap == 120,
          "ordered tutorial action sequence round-trips");
    Check(loaded.nextLessonId == "efz.test.next", "next link round-trips");

    // duplicate task ids / undeclared completion are rejected with the field named
    {
        std::ofstream out(lessonPath, std::ios::binary | std::ios::trunc);
        out << R"json({"format":1,"tutorialSchema":1,"type":"tutorial","id":"efz.test.dup",
            "lesson":{"completion":"allTasks","tasks":[
              {"id":"t0","kind":"combat","moveIds":[4]},
              {"id":"t0","kind":"combat","moveIds":[5]}]}})json";
    }
    Check(!Mission::LoadMission(lessonPath, loaded, error) &&
          error.find("duplicate") != std::string::npos,
          "duplicate task ids are rejected");
    {
        std::ofstream out(lessonPath, std::ios::binary | std::ios::trunc);
        out << R"json({"format":1,"tutorialSchema":1,"type":"tutorial","id":"efz.test.badmode",
            "lesson":{"completion":"whenever","tasks":[{"id":"t0","kind":"combat","moveIds":[4]}]}})json";
    }
    Check(!Mission::LoadMission(lessonPath, loaded, error) &&
          error.find("completion") != std::string::npos,
          "undeclared/unknown completion mode is rejected");
    {
        std::ofstream out(lessonPath, std::ios::binary | std::ios::trunc);
        out << R"json({"format":1,"tutorialSchema":1,"type":"tutorial","id":"efz.test.defaultplacement",
            "lesson":{"completion":"allTasks","tasks":[{"id":"t0","kind":"combat","moveIds":[4]}]}})json";
    }
    Check(Mission::LoadMission(lessonPath, loaded, error) &&
          loaded.lesson.requirementPlacement == "upperLeft",
          "omitted requirement placement uses the stable upper-left default");
    {
        std::ofstream out(lessonPath, std::ios::binary | std::ios::trunc);
        out << R"json({"format":1,"tutorialSchema":1,"type":"tutorial","id":"efz.test.badplacement",
            "lesson":{"completion":"allTasks","requirementPlacement":"movingTarget",
            "tasks":[{"id":"t0","kind":"combat","moveIds":[4]}]}})json";
    }
    Check(!Mission::LoadMission(lessonPath, loaded, error) &&
          error.find("requirementPlacement") != std::string::npos,
          "unknown requirement placement is rejected with the field named");
    {
        std::ofstream out(lessonPath, std::ios::binary | std::ios::trunc);
        out << R"json({"format":1,"tutorialSchema":1,"type":"tutorial","id":"efz.test.badchoice",
            "lesson":{"completion":"allTasks","tasks":[{"id":"q","kind":"choice",
            "prompt":"Pick","options":[{"id":"a","label":"A"},{"id":"b","label":"B"}],
            "acceptedOptionIds":["missing"]}]}})json";
    }
    Check(!Mission::LoadMission(lessonPath, loaded, error) &&
          error.find("missing option") != std::string::npos,
          "choice accepted IDs must name real options");
    {
        std::ofstream out(lessonPath, std::ios::binary | std::ios::trunc);
        out << R"json({"format":1,"tutorialSchema":1,"type":"tutorial","id":"efz.test.badsequence",
            "lesson":{"completion":"allTasks","tasks":[{"id":"route","kind":"combat",
            "sequence":[{"id":"a","moveIds":[200]},{"id":"a","moveIds":[201]}]}]}})json";
    }
    Check(!Mission::LoadMission(lessonPath, loaded, error) &&
          error.find("duplicates sequence action") != std::string::npos,
          "ordered action IDs are stable and unique");
    {
        std::ofstream out(lessonPath, std::ios::binary | std::ios::trunc);
        out << R"json({"format":1,"tutorialSchema":1,"type":"tutorial","id":"efz.test.badsource",
            "lesson":{"completion":"allTasks","tasks":[{"id":"route","kind":"combat",
            "sequence":[{"id":"a","moveIds":[163],"fromMoveIds":[0]},
            {"id":"b","moveIds":[207]}]}]}})json";
    }
    Check(!Mission::LoadMission(lessonPath, loaded, error) &&
          error.find("first action") != std::string::npos,
          "the first ordered action cannot claim a cancel source");

    // ---- coherent_state goalState (SP/RF/guard/hp thresholds) ----
    {
        std::ofstream out(lessonPath, std::ios::binary | std::ios::trunc);
        out << R"json({"format":1,"tutorialSchema":1,"type":"tutorial","id":"efz.test.goal",
            "lesson":{"completion":"allTasks","tasks":[{"id":"build","kind":"combat",
            "goalState":{"field":"sp","player":1,"op":"ge","value":1000,
            "requiresBlock":true}}]}})json";
    }
    Check(Mission::LoadMission(lessonPath, loaded, error),
          "goalState task with no move contract loads");
    Check(loaded.lesson.tasks[0].hasGoalState &&
          loaded.lesson.tasks[0].goalState.field == "sp" &&
          loaded.lesson.tasks[0].goalState.op == "ge" &&
          loaded.lesson.tasks[0].goalState.value == 1000 &&
          loaded.lesson.tasks[0].goalState.requiresBlock,
          "goalState round-trips through load");
    Check(Mission::SaveMission(lessonPath, loaded, error) &&
          ReadAll(lessonPath).find("\"requiresBlock\": true") != std::string::npos,
          "goalState is serialized");
    {
        std::ofstream out(lessonPath, std::ios::binary | std::ios::trunc);
        out << R"json({"format":1,"tutorialSchema":1,"type":"tutorial","id":"efz.test.juggle_goal",
            "lesson":{"completion":"allTasks","tasks":[{"id":"wait","kind":"combat",
            "goalState":{"field":"untech","player":2,"op":"le","value":0}}]}})json";
    }
    Check(Mission::LoadMission(lessonPath, loaded, error) &&
          loaded.lesson.tasks[0].goalState.field == "untech" &&
          loaded.lesson.tasks[0].goalState.player == 2,
          "juggle-state untech goal parses without a fake move contract");
    {
        std::ofstream out(lessonPath, std::ios::binary | std::ios::trunc);
        out << R"json({"format":1,"tutorialSchema":1,"type":"tutorial","id":"efz.test.airtech_goal",
            "lesson":{"completion":"allTasks","tasks":[{"id":"tech","kind":"combat",
            "goalState":{"field":"airtech","player":2,"op":"eq","value":1}}]}})json";
    }
    Check(Mission::LoadMission(lessonPath, loaded, error),
          "juggle-state airtech classifier goal loads");
    {
        std::ofstream out(lessonPath, std::ios::binary | std::ios::trunc);
        out << R"json({"format":1,"tutorialSchema":1,"type":"tutorial","id":"efz.test.bad_interception",
            "lesson":{"completion":"allTasks","tasks":[{"id":"probe","kind":"combat",
            "moveIds":[309],"req":"projectileInterception"}]}})json";
    }
    Check(!Mission::LoadMission(lessonPath, loaded, error) &&
          error.find("needs projectileInterception") != std::string::npos,
          "projectile interception cannot omit its exact pattern pair");
    {
        std::ofstream out(lessonPath, std::ios::binary | std::ios::trunc);
        out << R"json({"format":1,"tutorialSchema":1,"type":"tutorial","id":"efz.test.interception",
            "lesson":{"completion":"allTasks","tasks":[{"id":"probe","kind":"combat",
            "moveIds":[309],"req":"projectileInterception",
            "projectileInterception":{"incomingPattern":401,"guardPattern":423}}]}})json";
    }
    Check(Mission::LoadMission(lessonPath, loaded, error) &&
          loaded.lesson.tasks[0].hasProjectileInterception &&
          loaded.lesson.tasks[0].incomingProjectilePattern == 401 &&
          loaded.lesson.tasks[0].guardProjectilePattern == 423,
          "curated projectile interception pattern pair parses exactly");
    Check(Mission::SaveMission(lessonPath, loaded, error) &&
          ReadAll(lessonPath).find("\"projectileInterception\"") != std::string::npos,
          "projectile interception pattern pair serializes");
    {
        std::ofstream out(lessonPath, std::ios::binary | std::ios::trunc);
        out << R"json({"format":1,"tutorialSchema":1,"type":"tutorial","id":"efz.test.fic",
            "lesson":{"completion":"allTasks","tasks":[{"id":"fic","kind":"combat",
            "sequence":[{"id":"cast","moveIds":[250],"req":"move"},
            {"id":"ic","moveIds":[167],"fromMoveIds":[250],"inputMask":64,
            "req":"commit","flickerIC":{"projectilePattern":401,"minDistance":300}}]}]}})json";
    }
    Check(Mission::LoadMission(lessonPath, loaded, error) &&
          loaded.lesson.tasks[0].sequence[1].hasFlickerIC &&
          loaded.lesson.tasks[0].sequence[1].flickerProjectilePattern == 401 &&
          loaded.lesson.tasks[0].sequence[1].flickerMinDistance == 300,
          "curated FIC evidence parses on the committed IC destination");
    Check(Mission::SaveMission(lessonPath, loaded, error) &&
          ReadAll(lessonPath).find("\"flickerIC\"") != std::string::npos,
          "curated FIC evidence serializes");
    // 2026-07-14: an omitted/zero minDistance now means NO spacing gate - the
    // FIC proof rests on the live-projectile / no-contact / untouched-defender
    // invariants alone (a spawn-equals-threshold gate was a field-proven trap).
    {
        std::ofstream out(lessonPath, std::ios::binary | std::ios::trunc);
        out << R"json({"format":1,"tutorialSchema":1,"type":"tutorial","id":"efz.test.gatelessfic",
            "lesson":{"completion":"allTasks","tasks":[{"id":"fic","kind":"combat",
            "sequence":[{"id":"cast","moveIds":[250],"req":"move"},
            {"id":"ic","moveIds":[167],"fromMoveIds":[250],"inputMask":64,
            "req":"commit","flickerIC":{"projectilePattern":401}}]}]}})json";
    }
    Check(Mission::LoadMission(lessonPath, loaded, error) &&
          loaded.lesson.tasks[0].sequence[1].hasFlickerIC &&
          loaded.lesson.tasks[0].sequence[1].flickerMinDistance == 0,
          "FIC evidence may omit the spacing gate (proof carried by contact invariants)");
    {
        std::ofstream out(lessonPath, std::ios::binary | std::ios::trunc);
        out << R"json({"format":1,"tutorialSchema":1,"type":"tutorial","id":"efz.test.badgoal",
            "lesson":{"completion":"allTasks","tasks":[{"id":"g","kind":"combat",
            "goalState":{"field":"nonsense","op":"ge","value":1}}]}})json";
    }
    Check(!Mission::LoadMission(lessonPath, loaded, error) &&
          error.find("unknown field") != std::string::npos,
          "goalState with an unknown field is rejected");
    {
        std::ofstream out(lessonPath, std::ios::binary | std::ios::trunc);
        out << R"json({"format":1,"tutorialSchema":1,"type":"tutorial","id":"efz.test.mixgoal",
            "lesson":{"completion":"allTasks","tasks":[{"id":"g","kind":"combat","moveIds":[200],
            "goalState":{"field":"sp","op":"ge","value":1000}}]}})json";
    }
    Check(!Mission::LoadMission(lessonPath, loaded, error) &&
          error.find("cannot mix") != std::string::npos,
          "goalState cannot mix with a move contract");
    {
        std::ofstream out(lessonPath, std::ios::binary | std::ios::trunc);
        out << R"json({"format":1,"tutorialSchema":1,"type":"tutorial","id":"efz.test.untech_absence",
            "lesson":{"completion":"allTasks","tasks":[{"id":"watch","kind":"combat",
            "absence":{"forbid":"attack","start":"dummyUntech","end":"dummyUntechEmpty"}}]}})json";
    }
    Check(Mission::LoadMission(lessonPath, loaded, error) &&
          loaded.lesson.tasks[0].absenceStart == "dummyUntech" &&
          loaded.lesson.tasks[0].absenceEnd == "dummyUntechEmpty",
          "untech-driven absence window parses exactly");
    Check(Mission::SaveMission(lessonPath, loaded, error) &&
          ReadAll(lessonPath).find("dummyUntechEmpty") != std::string::npos,
          "untech-driven absence window serializes");
    {
        std::ofstream out(lessonPath, std::ios::binary | std::ios::trunc);
        out << R"json({"format":1,"tutorialSchema":1,"type":"tutorial","id":"efz.test.dummy_attack_absence",
            "lesson":{"completion":"allTasks","tasks":[{"id":"bait","kind":"combat","script":"ep",
            "absence":{"forbid":"attack","start":"taskArmed","end":"dummyAttackEnd"}}],
            "dummyScript":{"episodes":[{"id":"ep","ownerTask":"bait","kind":"macro",
            "action":"623C","start":"trigger","trigger":"onWakeup"}]}}})json";
    }
    Check(Mission::LoadMission(lessonPath, loaded, error) &&
          loaded.lesson.tasks[0].absenceEnd == "dummyAttackEnd",
          "dummy-attack-driven absence window parses exactly");
    Check(Mission::SaveMission(lessonPath, loaded, error) &&
          ReadAll(lessonPath).find("dummyAttackEnd") != std::string::npos,
          "dummy-attack-driven absence window serializes");
    {
        std::ofstream out(lessonPath, std::ios::binary | std::ios::trunc);
        out << R"json({"format":1,"tutorialSchema":1,"type":"tutorial","id":"efz.test.episode_cycle_absence",
            "lesson":{"completion":"allTasks","tasks":[{"id":"defend","kind":"combat","script":"ep",
            "absence":{"forbid":"attack","start":"taskArmed","end":"episodeCycleEnd",
            "failOnHit":true,"minBlocks":2}}],
            "dummyScript":{"episodes":[{"id":"ep","ownerTask":"defend","kind":"macro",
            "action":"5A","approach":"none","start":"taskArmed"}]}}})json";
    }
    Check(Mission::LoadMission(lessonPath, loaded, error) &&
          loaded.lesson.tasks[0].absenceEnd == "episodeCycleEnd" &&
          loaded.lesson.tasks[0].absenceFailOnHit &&
          loaded.lesson.tasks[0].absenceMinBlocks == 2,
          "episode-cycle absence evidence parses exactly");
    Check(Mission::SaveMission(lessonPath, loaded, error),
          "episode-cycle absence evidence serializes");
    Check(ReadAll(lessonPath).find("\"episodeCycleEnd\"") != std::string::npos &&
          ReadAll(lessonPath).find("\"failOnHit\": true") != std::string::npos &&
          ReadAll(lessonPath).find("\"minBlocks\": 2") != std::string::npos,
          "episode-cycle absence fields survive serialization");
    Check(Mission::LoadMission(lessonPath, loaded, error) &&
          loaded.lesson.tasks[0].absenceFailOnHit &&
          loaded.lesson.tasks[0].absenceMinBlocks == 2,
          "episode-cycle absence fields round-trip through disk");
    {
        std::ofstream out(lessonPath, std::ios::binary | std::ios::trunc);
        out << R"json({"format":1,"tutorialSchema":1,"type":"tutorial","id":"efz.test.bad_absence_blocks",
            "lesson":{"completion":"allTasks","tasks":[{"id":"watch","kind":"combat",
            "absence":{"forbid":"attack","start":"taskArmed","end":{"ticks":30},
            "minBlocks":-1}}]}})json";
    }
    Check(!Mission::LoadMission(lessonPath, loaded, error) &&
          error.find("minBlocks must be nonnegative") != std::string::npos,
          "absence minBlocks rejects negative counts");
    {
        std::ofstream out(lessonPath, std::ios::binary | std::ios::trunc);
        out << R"json({"format":1,"tutorialSchema":1,"type":"tutorial","id":"efz.test.bad_episode_cycle_absence",
            "lesson":{"completion":"allTasks","tasks":[{"id":"watch","kind":"combat",
            "absence":{"forbid":"attack","start":"taskArmed","end":"episodeCycleEnd"}}]}})json";
    }
    Check(!Mission::LoadMission(lessonPath, loaded, error) &&
          error.find("episodeCycleEnd needs") != std::string::npos,
          "episode-cycle absence requires a task-owned script");
    {
        std::ofstream out(lessonPath, std::ios::binary | std::ios::trunc);
        out << R"json({"format":1,"tutorialSchema":1,"type":"tutorial","id":"efz.test.bad_episode_cycle_start",
            "lesson":{"completion":"allTasks","tasks":[{"id":"watch","kind":"combat","script":"ep",
            "absence":{"forbid":"attack","start":"launched","end":"episodeCycleEnd"}}],
            "dummyScript":{"episodes":[{"id":"ep","ownerTask":"watch","kind":"macro",
            "action":"5A","approach":"none","start":"taskArmed"}]}}})json";
    }
    Check(!Mission::LoadMission(lessonPath, loaded, error) &&
          error.find("episodeCycleEnd needs") != std::string::npos,
          "episode-cycle absence requires a taskArmed window");
    {
        std::ofstream out(lessonPath, std::ios::binary | std::ios::trunc);
        out << R"json({"format":1,"tutorialSchema":1,"type":"tutorial","id":"efz.test.bad_absence_gate",
            "lesson":{"completion":"allTasks","tasks":[{"id":"watch","kind":"combat",
            "absence":{"forbid":"attack","start":"dummyStanding","end":"dummyUntechEmpty"}}]}})json";
    }
    Check(!Mission::LoadMission(lessonPath, loaded, error) &&
          error.find("absence start") != std::string::npos,
          "unknown absence start gate is rejected");

    // ---- structured, producer-bound contact contract ----
    {
        std::ofstream out(lessonPath, std::ios::binary | std::ios::trunc);
        out << R"json({"format":1,"tutorialSchema":1,"type":"tutorial","id":"efz.test.contact",
            "lesson":{"completion":"allTasks","tasks":[{"id":"throw","kind":"combat",
            "moveIds":[249],"req":"move","completionHoldTicks":0,"restoreOnSuccess":true,
            "contact":{"attacker":"learner","target":"dummy","source":"direct",
            "result":"throw","targetStateBefore":"downed",
            "moveIds":[249],"count":1}}]}})json";
    }
    Check(Mission::LoadMission(lessonPath, loaded, error),
          "structured direct-contact task loads");
    Check(loaded.lesson.tasks[0].hasContact &&
          loaded.lesson.tasks[0].contact.result == "throw" &&
          loaded.lesson.tasks[0].contact.targetStateBefore == "downed" &&
          loaded.lesson.tasks[0].contact.moveIds.size() == 1 &&
          loaded.lesson.tasks[0].completionHoldTicks == 0 &&
          loaded.lesson.tasks[0].restoreOnSuccess,
          "contact and immediate-restore policy parse exactly");
    Check(Mission::SaveMission(lessonPath, loaded, error) &&
          ReadAll(lessonPath).find("\"contact\"") != std::string::npos &&
          ReadAll(lessonPath).find("\"targetStateBefore\": \"downed\"") != std::string::npos &&
          ReadAll(lessonPath).find("\"restoreOnSuccess\": true") != std::string::npos,
          "contact and restore policy serialize");
    {
        std::ofstream out(lessonPath, std::ios::binary | std::ios::trunc);
        out << R"json({"format":1,"tutorialSchema":1,"type":"tutorial","id":"efz.test.badcontact",
            "lesson":{"completion":"allTasks","tasks":[{"id":"bad","kind":"combat",
            "moveIds":[200],"contact":{"result":"maybe","moveIds":[200]}}]}})json";
    }
    Check(!Mission::LoadMission(lessonPath, loaded, error) &&
          error.find("unknown result") != std::string::npos,
          "unknown contact outcomes are rejected");
    {
        std::ofstream out(lessonPath, std::ios::binary | std::ios::trunc);
        out << R"json({"format":1,"tutorialSchema":1,"type":"tutorial","id":"efz.test.commit",
            "lesson":{"completion":"allTasks","tasks":[{"id":"versions","kind":"combat",
            "sequence":[{"id":"a","moveIds":[256],"inputMask":16,"req":"commit"},
            {"id":"b","moveIds":[256],"inputMask":32,"req":"commit"}]}]}})json";
    }
    Check(Mission::LoadMission(lessonPath, loaded, error) &&
          loaded.lesson.tasks[0].sequence[1].req == "commit",
          "input-to-move commit sequence loads");
    {
        std::ofstream out(lessonPath, std::ios::binary | std::ios::trunc);
        out << R"json({"format":1,"tutorialSchema":1,"type":"tutorial","id":"efz.test.block",
            "lesson":{"completion":"allTasks","tasks":[{"id":"tick","kind":"combat",
            "sequence":[{"id":"jab","moveIds":[200],"req":"block"},
            {"id":"throw","moveIds":[221],"req":"move"}]}]}})json";
    }
    Check(Mission::LoadMission(lessonPath, loaded, error) &&
          loaded.lesson.tasks[0].sequence[0].req == "block",
          "defender-block-state sequence action loads");

    // ---- dummy episode vocabulary + task ownership ----
    {
        std::ofstream out(lessonPath, std::ios::binary | std::ios::trunc);
        out << R"json({"format":1,"tutorialSchema":1,"type":"tutorial","id":"efz.test.episode",
            "lesson":{"completion":"allTasks","tasks":[{"id":"guard","kind":"combat",
            "moveIds":[168],"script":"ep"}],"dummyScript":{"episodes":[{"id":"ep",
            "ownerTask":"guard","kind":"macro","action":"5B","approach":"none",
            "start":"taskArmed","repeat":{"mode":"once","delay":30}}]}}})json";
    }
    Check(Mission::LoadMission(lessonPath, loaded, error),
          "supported once-per-attempt dummy episode loads");
    {
        std::ofstream out(lessonPath, std::ios::binary | std::ios::trunc);
        out << R"json({"format":1,"tutorialSchema":1,"type":"tutorial","id":"efz.test.episode_scripts",
            "lesson":{"completion":"allTasks","tasks":[
            {"id":"dash_cancel","kind":"combat","moveIds":[168],"script":"ep_dash_cancel"},
            {"id":"dash_approach","kind":"combat","moveIds":[168],"script":"ep_dash_approach"}],
            "dummyScript":{"episodes":[
            {"id":"ep_dash_cancel","ownerTask":"dash_cancel","kind":"macro","approach":"none",
             "start":"taskArmed","script":[{"action":"66B"},{"action":"623C","cancel":true,"waitTicks":8}]},
            {"id":"ep_dash_approach","ownerTask":"dash_approach","kind":"macro","approach":"dash",
             "start":"taskArmed","script":[{"action":"5A"},{"action":"5B","cancel":true,"waitTicks":10}]}]}}})json";
    }
    Check(Mission::LoadMission(lessonPath, loaded, error) &&
          loaded.lesson.episodes.size() == 2 &&
          loaded.lesson.episodes[0].script.size() == 2 &&
          loaded.lesson.episodes[0].script[1].waitTicks == 8 &&
          loaded.lesson.episodes[1].approach == "dash" &&
          loaded.lesson.episodes[1].script[1].waitTicks == 10,
          "multi-action episodes load a first-step dash normal and a scripted dash approach");
    Check(Mission::SaveMission(lessonPath, loaded, error) &&
          ReadAll(lessonPath).find("\"waitTicks\": 8") != std::string::npos,
          "script destination delay serializes");
    Check(Mission::LoadMission(lessonPath, loaded, error) &&
          loaded.lesson.episodes.size() == 2 &&
          loaded.lesson.episodes[1].approach == "dash" &&
          loaded.lesson.episodes[1].script[1].waitTicks == 10,
          "script destination delay and default dash approach round-trip");
    {
        std::ofstream out(lessonPath, std::ios::binary | std::ios::trunc);
        out << R"json({"format":1,"tutorialSchema":1,"type":"tutorial","id":"efz.test.bad_late_dash_normal",
            "lesson":{"completion":"allTasks","tasks":[{"id":"pattern","kind":"combat",
            "moveIds":[168],"script":"ep"}],"dummyScript":{"episodes":[{"id":"ep",
            "ownerTask":"pattern","kind":"macro","start":"taskArmed",
            "script":[{"action":"5A"},{"action":"66B","cancel":true,"waitTicks":8}]}]}}})json";
    }
    Check(!Mission::LoadMission(lessonPath, loaded, error) &&
          error.find("cannot ride the ordinary injection lanes") != std::string::npos,
          "only the first scripted action may use the dedicated dash-normal driver");
    // 2026-07-14: dash normals from a standing start are unreactable, so
    // reaction drills author a neutral-hop telegraph before the opener.
    {
        std::ofstream out(lessonPath, std::ios::binary | std::ios::trunc);
        out << R"json({"format":1,"tutorialSchema":1,"type":"tutorial","id":"efz.test.telegraph",
            "lesson":{"completion":"allTasks","tasks":[{"id":"rg","kind":"combat",
            "moveIds":[168],"script":"ep"}],"dummyScript":{"episodes":[{"id":"ep",
            "ownerTask":"rg","kind":"macro","action":"66B","approach":"none","telegraph":"jump",
            "start":"taskArmed","repeat":{"mode":"loop","delay":150}}]}}})json";
    }
    Check(Mission::LoadMission(lessonPath, loaded, error) &&
          loaded.lesson.episodes[0].telegraph == "jump",
          "a dash-normal episode loads its neutral-hop telegraph");
    Check(Mission::SaveMission(lessonPath, loaded, error) &&
          ReadAll(lessonPath).find("\"telegraph\": \"jump\"") != std::string::npos,
          "episode telegraph serializes");
    // 2026-07-14 (second pass): the telegraph is valid on ANY taskArmed macro
    // opener - buffered specials (Rumi's armored 41236C) are as unreactable
    // from a standing start as dash normals.
    {
        std::ofstream out(lessonPath, std::ios::binary | std::ios::trunc);
        out << R"json({"format":1,"tutorialSchema":1,"type":"tutorial","id":"efz.test.telegraph_special",
            "lesson":{"completion":"allTasks","tasks":[{"id":"armor","kind":"combat",
            "moveIds":[221],"script":"ep"}],"dummyScript":{"episodes":[{"id":"ep",
            "ownerTask":"armor","kind":"macro","action":"41236C","approach":"none","telegraph":"jump",
            "start":"taskArmed","repeat":{"mode":"loop","delay":150}}]}}})json";
    }
    Check(Mission::LoadMission(lessonPath, loaded, error) &&
          loaded.lesson.episodes[0].telegraph == "jump" &&
          loaded.lesson.episodes[0].action == "41236C",
          "a buffered-special opener accepts the neutral-hop telegraph");
    {
        std::ofstream out(lessonPath, std::ios::binary | std::ios::trunc);
        out << R"json({"format":1,"tutorialSchema":1,"type":"tutorial","id":"efz.test.bad_telegraph",
            "lesson":{"completion":"allTasks","tasks":[{"id":"rg","kind":"combat",
            "moveIds":[168],"script":"ep"}],"dummyScript":{"episodes":[{"id":"ep",
            "ownerTask":"rg","kind":"macro","action":"236A","approach":"none","telegraph":"jump",
            "start":"playerState","predicate":"p1move:200",
            "repeat":{"mode":"loop","delay":150}}]}}})json";
    }
    Check(!Mission::LoadMission(lessonPath, loaded, error) &&
          error.find("telegraph needs a taskArmed macro opener") != std::string::npos,
          "telegraph is rejected on a reactive (non-taskArmed) episode");
    // 2026-07-14: rf_lock holds the authored rf (and IC color) all session so
    // resource-gated dummy actions (RF specials/reversals) never downgrade.
    {
        std::ofstream out(lessonPath, std::ios::binary | std::ios::trunc);
        out << R"json({"format":1,"tutorialSchema":1,"type":"tutorial","id":"efz.test.rflock",
            "player":{"rf":1000,"blueIC":0,"rfLock":true},
            "dummy":{"rf":1000,"blueIC":0,"rfLock":true},
            "lesson":{"completion":"allTasks","tasks":[{"id":"t","kind":"combat",
            "moveIds":[200]}]}})json";
    }
    Check(Mission::LoadMission(lessonPath, loaded, error) &&
          loaded.player.rfLock && loaded.dummy.rfLock &&
          loaded.player.rf == 1000 && loaded.dummy.rf == 1000,
          "session rf locks parse on both sides");
    Check(Mission::SaveMission(lessonPath, loaded, error) &&
          ReadAll(lessonPath).find("\"rfLock\": true") != std::string::npos,
          "session rf lock serializes");
    {
        std::ofstream out(lessonPath, std::ios::binary | std::ios::trunc);
        out << R"json({"format":1,"tutorialSchema":1,"type":"tutorial","id":"efz.test.badrflock",
            "player":{"rfLock":true},
            "lesson":{"completion":"allTasks","tasks":[{"id":"t","kind":"combat",
            "moveIds":[200]}]}})json";
    }
    Check(!Mission::LoadMission(lessonPath, loaded, error) &&
          error.find("rfLock needs an authored rf value") != std::string::npos,
          "an rf lock without an authored rf value is rejected");
    {
        std::ofstream out(lessonPath, std::ios::binary | std::ios::trunc);
        out << R"json({"format":1,"tutorialSchema":1,"type":"tutorial","id":"efz.test.rg_chain",
            "lesson":{"completion":"allTasks","tasks":[{"id":"pattern","kind":"combat",
            "moveIds":[168],"script":"ep","pos":{"x":20,"y":0},
            "dummyPos":{"x":80,"y":0}}],"dummyScript":{"episodes":[{"id":"ep",
            "ownerTask":"pattern","kind":"macro","action":"5A>5B>2C","approach":"none",
            "start":"taskArmed","repeat":{"mode":"loop","delay":120}}]}}})json";
    }
    Check(Mission::LoadMission(lessonPath, loaded, error) &&
          loaded.lesson.tasks[0].hasPos && loaded.lesson.tasks[0].posX == 20.0f &&
          loaded.lesson.tasks[0].hasDummyPos &&
          loaded.lesson.tasks[0].dummyPosX == 80.0f &&
          loaded.lesson.episodes[0].action == "5A>5B>2C",
          "contact-chain episode and task-scoped positions for both sides load");
    Check(Mission::SaveMission(lessonPath, loaded, error) &&
          ReadAll(lessonPath).find("\"dummyPos\"") != std::string::npos,
          "task-scoped dummy position serializes");
    {
        std::ofstream out(lessonPath, std::ios::binary | std::ios::trunc);
        out << R"json({"format":1,"tutorialSchema":1,"type":"tutorial","id":"efz.test.bad_rg_chain",
            "lesson":{"completion":"allTasks","tasks":[{"id":"pattern","kind":"combat",
            "moveIds":[168],"script":"ep"}],"dummyScript":{"episodes":[{"id":"ep",
            "ownerTask":"pattern","kind":"macro","action":"5A>5B>2C","approach":"dash",
            "start":"taskArmed"}]}}})json";
    }
    Check(!Mission::LoadMission(lessonPath, loaded, error) &&
          error.find("requires taskArmed start and no approach") != std::string::npos,
          "contact-chain script rejects an extra automatic approach");
    {
        std::ofstream out(lessonPath, std::ios::binary | std::ios::trunc);
        // 41236C became genuinely drivable with the Phase-2 injector motions
        // (buffer-freeze pattern + button mapping both exist), so the negative
        // example is now 63214C - absent from the injectable vocabulary.
        out << R"json({"format":1,"tutorialSchema":1,"type":"tutorial","id":"efz.test.bad_episode_action",
            "lesson":{"completion":"allTasks","tasks":[{"id":"guard","kind":"combat",
            "moveIds":[168],"script":"ep"}],"dummyScript":{"episodes":[{"id":"ep",
            "ownerTask":"guard","kind":"macro","action":"63214C","start":"taskArmed"}]}}})json";
    }
    Check(!Mission::LoadMission(lessonPath, loaded, error) &&
          error.find("unsupported action") != std::string::npos,
          "dummy episode cannot claim an injection action the runtime cannot drive");
    {
        std::ofstream out(lessonPath, std::ios::binary | std::ios::trunc);
        out << R"json({"format":1,"tutorialSchema":1,"type":"tutorial","id":"efz.test.variant_group",
            "lesson":{"completion":"allTasks","tasks":[
            {"id":"one","kind":"combat","moveIds":[200],"script":"ep1"},
            {"id":"two","kind":"combat","moveIds":[200],"script":"ep2"}],
            "dummyScript":{"episodes":[
            {"id":"ep1","ownerTask":"one","kind":"idle","variants":["idle","block"],"variantGroup":"pair"},
            {"id":"ep2","ownerTask":"two","kind":"idle","variants":["idle","block"],"variantGroup":"pair"}]}}})json";
    }
    Check(Mission::LoadMission(lessonPath, loaded, error) &&
          loaded.lesson.episodes.size() == 2 &&
          loaded.lesson.episodes[0].variantGroup == "pair" &&
          loaded.lesson.episodes[1].variantGroup == "pair",
          "a balanced dummy-variant group parses");
    Check(Mission::SaveMission(lessonPath, loaded, error) &&
          ReadAll(lessonPath).find("\"variantGroup\": \"pair\"") != std::string::npos,
          "a balanced dummy-variant group serializes");
    {
        std::ofstream out(lessonPath, std::ios::binary | std::ios::trunc);
        out << R"json({"format":1,"tutorialSchema":1,"type":"tutorial","id":"efz.test.bad_variant_group",
            "lesson":{"completion":"allTasks","tasks":[
            {"id":"one","kind":"combat","moveIds":[200],"script":"ep1"},
            {"id":"two","kind":"combat","moveIds":[200],"script":"ep2"}],
            "dummyScript":{"episodes":[
            {"id":"ep1","ownerTask":"one","kind":"idle","variants":["idle","block"],"variantGroup":"pair"},
            {"id":"ep2","ownerTask":"two","kind":"idle","variants":["idle","rg"],"variantGroup":"pair"}]}}})json";
    }
    Check(!Mission::LoadMission(lessonPath, loaded, error) &&
          error.find("mismatched variants pools") != std::string::npos,
          "a grouped variant bag rejects mismatched member pools");
    {
        std::ofstream out(lessonPath, std::ios::binary | std::ios::trunc);
        out << R"json({"format":1,"tutorialSchema":1,"type":"tutorial","id":"efz.test.bad_episode_owner",
            "lesson":{"completion":"allTasks","tasks":[{"id":"one","kind":"combat",
            "moveIds":[200],"script":"ep"},{"id":"two","kind":"combat","moveIds":[201]}],
            "dummyScript":{"episodes":[{"id":"ep","ownerTask":"two","kind":"idle"}]}}})json";
    }
    Check(!Mission::LoadMission(lessonPath, loaded, error) &&
          error.find("owned by task") != std::string::npos,
          "a task cannot borrow another task's dummy episode");

    // Validate the shipped curriculum with the same parser used by Runner::Load.
    // This catches malformed/unloadable lesson assets before an in-game Next
    // transition can publish them.
    if (argc > 1) {
        int lessonCount = 0;
        bool sawInputEdgeLesson = false;
        bool sawOrderedLesson = false;
        bool sawCorrectNayukiCancelIds = false;
        bool sawJumpingMisakiAirString = false;
        bool sawCorrectAirRecoveryDrills = false;
        bool sawCorrectAirRecoveryDecisionDrills = false;
        bool sawCorrectAirRecoveryPunish = false;
        bool sawCorrectNayukiRfIds = false;
        bool sawCorrectBicSafetyContract = false;
        bool sawAllTakeSpaceGuardReactions = false;
        bool sawCorrectNayukiAntiAirs = false;
        bool sawOrderedWakeupThrow = false;
        bool sawWakeupBaitCarry = false;
        bool sawWakeupKnockdownRoutes = false;
        bool sawMisaki214CReversal = false;
        bool sawMisaki214CResources = false;
        bool sawLiveJuggleContinuation = false;
        bool sawStrictWakeupJb = false;
        bool sawCharacterCounterFiveB = false;
        bool sawCorrectRedIcContract = false;
        bool sawCorrectBicRetryContract = false;
        bool sawSpendRfBlueParity = false;
        bool sawCorrectRgDrills = false;
        bool sawStrictRgResponse = false;
        bool sawAuthoredMaiOtgRoute = false;
        bool sawStrictFlickerIc = false;
        bool sawCenteredFlickerIc = false;
        bool sawPowerStatsPlacement = false;
        bool sawPowerSingleComboRoute = false;
        bool sawDirectDashMomentumJump = false;
        bool sawDirectArmorCancelEscape = false;
        bool sawBufferedArmorCommandThrow = false;
        bool sawRfBlockCompletion = false;
        bool sawGuidedHitConfirm = false;
        bool sawStableGuardOpeners = false;
        bool sawTrueGuardAttackDashNormal = false;
        bool sawGuardGaugeRecoveryLauncher = false;
        bool sawDashWhiffPunishRoute = false;
        bool sawPressureInteractionProofs = false;
        bool sawChooseRgBlockProof = false;
        bool sawAttackIntoRgCancelRace = false;
        bool sawEndTurnInteractionProof = false;
        bool sawStrategicRfProof = false;
        int launchableCount = 0;
        int placeholderTaskCount = 0;
        int missingEpisodeCount = 0;
        int sourcedLessonCount = 0;
        int shippedTaskCount = 0;
        int shippedEpisodeCount = 0;
        const std::filesystem::path root(argv[1]);
        const std::filesystem::path repositoryRoot =
            root.parent_path().parent_path().parent_path();
        for (const auto& entry : std::filesystem::directory_iterator(root)) {
            if (!entry.is_regular_file() || entry.path().extension() != ".json" ||
                entry.path().filename() == "pack.json") {
                continue;
            }
            Mission::Mission curriculumLesson;
            error.clear();
            if (!Mission::LoadMission(entry.path().string(), curriculumLesson, error)) {
                std::cerr << "FAILED: shipped lesson " << entry.path().string()
                          << " did not load: " << error << '\n';
                return 1;
            }
            Check(curriculumLesson.tutorialSchema == 1 && curriculumLesson.hasLesson,
                  "shipped curriculum entry uses the tutorial schema");
            shippedTaskCount += static_cast<int>(curriculumLesson.lesson.tasks.size());
            shippedEpisodeCount += static_cast<int>(curriculumLesson.lesson.episodes.size());
            if (!curriculumLesson.sourceRefs.empty()) ++sourcedLessonCount;
            for (const auto& sourceRef : curriculumLesson.sourceRefs) {
                static const std::string localPrefix = "local:";
                if (sourceRef.rfind(localPrefix, 0) != 0) continue;
                std::string relative = sourceRef.substr(localPrefix.size());
                const std::size_t locator = relative.find("::");
                if (locator != std::string::npos) relative.resize(locator);
                Check(!relative.empty() &&
                      std::filesystem::exists(repositoryRoot / relative),
                      "tutorial local source reference resolves inside the repository");
            }
            for (const auto& task : curriculumLesson.lesson.tasks) {
                if (task.inputMask != 0 && task.req == "input") sawInputEdgeLesson = true;
                if (!task.sequence.empty()) sawOrderedLesson = true;
                for (int id : task.moveIds) if (id <= 0) ++placeholderTaskCount;
            }
            if (curriculumLesson.lessonId == "efz.offense.power_scaling") {
                sawPowerStatsPlacement =
                    curriculumLesson.lesson.requirementPlacement == "belowStats";
                for (const auto& task : curriculumLesson.lesson.tasks) {
                    if (task.id == "watch_power") {
                        sawPowerSingleComboRoute = task.continuity == "sameCombo" &&
                            task.sequence.size() == 3 && task.endsCombo;
                    }
                }
            }
            if (curriculumLesson.lessonId == "efz.fundamentals.ground_dash") {
                const bool declaresCancelTransition =
                    std::find(curriculumLesson.requires.begin(),
                              curriculumLesson.requires.end(),
                              "cancel_transition") != curriculumLesson.requires.end();
                for (const auto& task : curriculumLesson.lesson.tasks) {
                    if (task.id != "dash_jump" || task.sequence.size() != 2) continue;
                    const auto& dash = task.sequence[0];
                    const auto& jump = task.sequence[1];
                    sawDirectDashMomentumJump = declaresCancelTransition &&
                        dash.moveIds == std::vector<int>{163} &&
                        jump.moveIds == std::vector<int>{4} &&
                        jump.fromMoveIds == std::vector<int>{163};
                }
            }
            if (curriculumLesson.lessonId == "efz.systems.recoil_armor") {
                const bool declaresCancelTransition =
                    std::find(curriculumLesson.requires.begin(),
                              curriculumLesson.requires.end(),
                              "cancel_transition") != curriculumLesson.requires.end();
                bool superBreaksArmor = false;
                bool breakUsesCStartup = false;
                bool noDummyRfGate = curriculumLesson.dummy.rf < 0;
                for (const auto& task : curriculumLesson.lesson.tasks) {
                    noDummyRfGate = noDummyRfGate && task.dummySeed.rf < 0;
                    if (task.id == "super_armor" && task.sequence.size() == 2) {
                        // 2026-07-14: throwing the armored bunt proved
                        // impossible live, so the drill teaches the
                        // armor-BREAKING, invuln-startup 236236 (300/301/302)
                        // with seeded SP. Correct play is fire ON PREDICTION
                        // after the hop so the super's startup overlaps the
                        // armor - the armored proof therefore lives on the
                        // CONNECT: a resolver contact during 300-302 whose
                        // defenderMoveBefore is 264 (the connect itself deals
                        // 0 damage, throw-like, so contact evidence is the
                        // only sampler). The 303 translation seals it.
                        const auto& connect = task.sequence[0];
                        const auto& seal = task.sequence[1];
                        superBreaksArmor =
                            connect.moveIds == std::vector<int>{300, 301, 302} &&
                            connect.req == "land" &&
                            connect.dummyStateMoveIds == std::vector<int>{264} &&
                            connect.hasContact &&
                            connect.contact.source == "direct" &&
                            connect.contact.result == "hit" &&
                            connect.contact.moveIds ==
                                std::vector<int>{300, 301, 302} &&
                            // Each strength owns its connect state (live
                            // trace: 301 translated to 304, failing a
                            // 303-only seal): 300->303, 301->304, 302->305.
                            seal.moveIds == std::vector<int>{303, 304, 305} &&
                            seal.fromMoveIds == std::vector<int>{300, 301, 302} &&
                            seal.req == "move" &&
                            task.playerSeed.meter == 3000;
                    } else if (task.id == "break_armor") {
                        // 2026-07-14: 2C BREAKS the armor, so Rumi leaves 264
                        // the same frame the damage lands - only the resolver
                        // contact's defenderMoveBefore can prove the pre-hit
                        // armored state (the Guard Attacks use_gp pattern).
                        breakUsesCStartup = task.dummyStateMoveIds ==
                                std::vector<int>{264} &&
                            task.hasContact &&
                            task.contact.source == "direct" &&
                            task.contact.result == "hit" &&
                            task.contact.moveIds == std::vector<int>{206};
                    } else if (task.id == "cancel_escape" &&
                               task.sequence.size() == 2) {
                        // 2026-07-14 (user design): Rumi LOOPS hop -> armored
                        // 41236C exactly like the other two drills; the poke
                        // goes INTO the armor (dummyState 264 at the 5A), so
                        // contact is guaranteed and the direct 5A->4 jump
                        // cancel is the honest escape proof again (the "J on
                        // armor" cancel class this lesson teaches).
                        const auto& poke = task.sequence[0];
                        const auto& jump = task.sequence[1];
                        sawDirectArmorCancelEscape = declaresCancelTransition &&
                            poke.moveIds == std::vector<int>{200} &&
                            poke.dummyStateMoveIds == std::vector<int>{264} &&
                            jump.moveIds == std::vector<int>{4} &&
                            jump.fromMoveIds == std::vector<int>{200};
                    }
                }
                int bufferedCCommandEpisodes = 0;
                for (const auto& episode : curriculumLesson.lesson.episodes) {
                    if (episode.kind != "macro" ||
                        episode.action != "41236C" ||
                        episode.approach != "none") {
                        continue;
                    }
                    // 2026-07-14: ALL THREE armor drills run the same
                    // telegraphed loop - hop, land, armored 41236C - so the
                    // learner answers a visible, identical rep each cycle
                    // (super through it, bow 2C through it, or 5A poke into
                    // the armor + jump-cancel out).
                    if ((episode.id == "ep_armor_super" &&
                         episode.ownerTask == "super_armor" &&
                         episode.start == "taskArmed" &&
                         episode.telegraph == "jump") ||
                        (episode.id == "ep_armor_break" &&
                         episode.ownerTask == "break_armor" &&
                         episode.start == "taskArmed" &&
                         episode.telegraph == "jump") ||
                        (episode.id == "ep_cmd_throw" &&
                         episode.ownerTask == "cancel_escape" &&
                         episode.start == "taskArmed" &&
                         episode.telegraph == "jump")) {
                        ++bufferedCCommandEpisodes;
                    }
                }
                sawBufferedArmorCommandThrow = noDummyRfGate &&
                    superBreaksArmor &&
                    breakUsesCStartup && bufferedCCommandEpisodes == 3;
            }
            if (curriculumLesson.lessonId == "efz.offense.cancel_routes") {
                bool saw236A = false;
                bool saw623A = false;
                for (const auto& task : curriculumLesson.lesson.tasks) {
                    for (const auto& action : task.sequence) {
                        if (task.id == "showcase" && action.id == "236a" &&
                            action.moveIds == std::vector<int>{253}) {
                            saw236A = true;
                        }
                        if (task.id == "dash_special" && action.id == "623a" &&
                            action.moveIds == std::vector<int>{250}) {
                            saw623A = true;
                        }
                    }
                }
                sawCorrectNayukiCancelIds = saw236A && saw623A;
            }
            if (curriculumLesson.lessonId == "efz.offense.rf_gauge") {
                bool blockGoal = false;
                bool pressure = false;
                for (const auto& task : curriculumLesson.lesson.tasks) {
                    if (task.id != "block_faster") continue;
                    blockGoal = task.hasGoalState &&
                        task.goalState.field == "rf" &&
                        task.goalState.player == 1 &&
                        task.goalState.op == "ge" && task.goalState.value == 500 &&
                        task.goalState.requiresBlock &&
                        task.hasCompare && task.compareVs == "reach_red" &&
                        task.compareMetric == "ticks" && task.compareOp.empty() &&
                        task.script == "ep_rf_pressure";
                }
                for (const auto& episode : curriculumLesson.lesson.episodes) {
                    pressure |= episode.id == "ep_rf_pressure" &&
                        episode.ownerTask == "block_faster" &&
                        episode.kind == "macro" &&
                        episode.action == "5A>5B>2C" &&
                        episode.approach == "none" &&
                        episode.repeatMode == "loop";
                }
                sawRfBlockCompletion = blockGoal && pressure;
            }
            if (curriculumLesson.lessonId == "efz.offense.jump_cancels") {
                for (const auto& episode : curriculumLesson.lesson.episodes) {
                    if (episode.id == "ep_air_string_jump" &&
                        episode.ownerTask == "air_string" &&
                        episode.kind == "macro" && episode.action == "jump" &&
                        episode.approach == "none" && episode.start == "taskArmed" &&
                        episode.repeatMode == "loop") {
                        sawJumpingMisakiAirString = true;
                    }
                }
            }
            if (curriculumLesson.lessonId == "efz.fundamentals.air_recovery") {
                bool away = false, toward = false, noTech = false;
                for (const auto& task : curriculumLesson.lesson.tasks) {
                    if (task.id == "tech_away") {
                        away = task.moveIds == std::vector<int>{158} &&
                               task.req == "move" && task.script == "ep_tech_away";
                    } else if (task.id == "tech_toward") {
                        toward = task.moveIds == std::vector<int>{157} &&
                                 task.req == "move" && task.script == "ep_tech_toward";
                    } else if (task.id == "no_tech") {
                        noTech = task.hasAbsence && task.absenceForbid == "airtech" &&
                                 task.absenceStart == "launched" &&
                                 task.absenceEnd == "grounded" &&
                                 task.script == "ep_no_tech";
                    }
                }
                bool awayLauncher = false, towardLauncher = false, noTechLauncher = false;
                for (const auto& episode : curriculumLesson.lesson.episodes) {
                    const bool isLauncher = episode.kind == "macro" &&
                        episode.action == "236A" && episode.approach == "none" &&
                        episode.start == "taskArmed";
                    awayLauncher |= isLauncher && episode.id == "ep_tech_away" &&
                                    episode.ownerTask == "tech_away";
                    towardLauncher |= isLauncher && episode.id == "ep_tech_toward" &&
                                      episode.ownerTask == "tech_toward";
                    noTechLauncher |= isLauncher && episode.id == "ep_no_tech" &&
                                      episode.ownerTask == "no_tech";
                }
                sawCorrectAirRecoveryDrills = away && toward && noTech &&
                    awayLauncher && towardLauncher && noTechLauncher;
            }
            if (curriculumLesson.lessonId == "efz.defense.air_recovery_decisions") {
                bool observe = false, noRecover = false, recoverAway = false;
                for (const auto& task : curriculumLesson.lesson.tasks) {
                    if (task.id == "observe_launch") {
                        observe = task.hasGoalState && task.goalState.field == "launched" &&
                                  task.goalState.player == 1 &&
                                  task.script == "ep_observe_launch";
                    } else if (task.id == "no_recover") {
                        noRecover = task.hasAbsence && task.absenceForbid == "airtech" &&
                                    task.absenceStart == "launched" &&
                                    task.absenceEnd == "grounded" &&
                                    task.script == "ep_no_recover";
                    } else if (task.id == "recover_away") {
                        recoverAway = task.moveIds == std::vector<int>{158} &&
                                      task.req == "move" &&
                                      task.script == "ep_recover_away";
                    }
                }
                bool observeLauncher = false, nearLauncher = false, farLauncher = false;
                for (const auto& episode : curriculumLesson.lesson.episodes) {
                    const bool isProjectileLauncher = episode.kind == "macro" &&
                        episode.action == "236A" && episode.approach == "none" &&
                        episode.start == "taskArmed";
                    observeLauncher |= isProjectileLauncher && episode.id == "ep_observe_launch" &&
                                       episode.ownerTask == "observe_launch";
                    nearLauncher |= isProjectileLauncher && episode.id == "ep_no_recover" &&
                                    episode.ownerTask == "no_recover";
                    farLauncher |= episode.kind == "macro" && episode.action == "66C" &&
                                   episode.approach == "none" &&
                                   episode.start == "taskArmed" &&
                                   episode.repeatMode == "loop" &&
                                   episode.repeatDelay == 240 &&
                                   episode.id == "ep_recover_away" &&
                                   episode.ownerTask == "recover_away";
                }
                sawCorrectAirRecoveryDecisionDrills = observe && noRecover && recoverAway &&
                    observeLauncher && nearLauncher && farLauncher;
            }
            if (curriculumLesson.lessonId == "efz.defense.air_recovery_punish") {
                bool route = false;
                for (const auto& task : curriculumLesson.lesson.tasks) {
                    if (task.id != "recovery_punish" || task.sequence.size() != 2) continue;
                    const auto& launch = task.sequence[0];
                    const auto& punish = task.sequence[1];
                    route = launch.moveIds == std::vector<int>{254} &&
                        launch.req == "land" &&
                        punish.moveIds == std::vector<int>{249} &&
                        punish.req == "move" && task.script == "ep_airtech_fwd";
                }
                // 2026-07-14 user fix: the dummy techs FORWARD (toward the
                // player, tech 157) so the recovery lands inside j.6C range;
                // a backward tech flies away from the punish.
                bool ownsForwardTech = false;
                for (const auto& episode : curriculumLesson.lesson.episodes) {
                    ownsForwardTech |= episode.id == "ep_airtech_fwd" &&
                        episode.ownerTask == "recovery_punish" &&
                        episode.kind == "airtech" && episode.clip == "forward";
                }
                sawCorrectAirRecoveryPunish = route && ownsForwardTech &&
                    curriculumLesson.player.character == "misaki" &&
                    curriculumLesson.dummy.character == "nayukib";
            }
            if (curriculumLesson.lessonId == "efz.offense.rf_attacks") {
                bool saw623C = false;
                bool saw623BFallback = false;
                bool sawBicSafety = false;
                bool sawBlockingAnswer = false;
                for (const auto& task : curriculumLesson.lesson.tasks) {
                    if (task.id == "rf_version" && task.moveIds == std::vector<int>{252}) {
                        saw623C = true;
                    }
                    if (task.id == "rf_fallback" && task.moveIds == std::vector<int>{251}) {
                        saw623BFallback = true;
                    }
                    if (task.id == "bic_safety" && task.sequence.size() == 3) {
                        const auto& blocked = task.sequence[0];
                        const auto& bic = task.sequence[1];
                        const auto& answer = task.sequence[2];
                        // 623C leaves Nayuki AIRBORNE (user correction
                        // 2026-07-14): the blocked-recovery IC is the AIR IC
                        // 171 (167 kept for a grounded edge case), and the
                        // safe follow-ups include air guard 156 and the
                        // backward double jump 16.
                        sawBicSafety = task.playerSeed.rf == 1000 &&
                            task.playerSeed.blueIC == 1 &&
                            blocked.moveIds == std::vector<int>{252} &&
                            blocked.inputMask == 64 && blocked.req == "block" &&
                            bic.moveIds == std::vector<int>{167, 171} &&
                            bic.fromMoveIds == std::vector<int>{252} &&
                            bic.inputMask == 64 && bic.req == "commit" &&
                            answer.moveIds ==
                                std::vector<int>{150, 151, 152, 153, 154, 155,
                                                 156, 16};
                    }
                }
                for (const auto& episode : curriculumLesson.lesson.episodes) {
                    sawBlockingAnswer |= episode.id == "ep_bic_answer" &&
                        episode.ownerTask == "bic_safety" &&
                        episode.kind == "block_answer" && episode.action == "5A";
                }
                sawCorrectNayukiRfIds = saw623C && saw623BFallback;
                sawCorrectBicSafetyContract = sawBicSafety && sawBlockingAnswer;
            }
            if (curriculumLesson.lessonId == "efz.offense.red_ic") {
                const bool declaresInputEdges =
                    std::find(curriculumLesson.requires.begin(),
                              curriculumLesson.requires.end(), "input_edges") !=
                    curriculumLesson.requires.end();
                const bool declaresCommitMapping =
                    std::find(curriculumLesson.requires.begin(),
                              curriculumLesson.requires.end(), "input_commit_mapping") !=
                    curriculumLesson.requires.end();
                for (const auto& task : curriculumLesson.lesson.tasks) {
                    if (task.id != "ic_drill" || task.sequence.size() != 3) continue;
                    const auto& ic = task.sequence[1];
                    sawCorrectRedIcContract = declaresInputEdges && declaresCommitMapping &&
                        ic.moveIds == std::vector<int>{167} &&
                        ic.fromMoveIds == std::vector<int>{203} &&
                        ic.inputMask == 64 && ic.req == "commit";
                }
            }
            if (curriculumLesson.lessonId == "efz.offense.blue_ic") {
                bool bicUsesC = false;
                for (const auto& task : curriculumLesson.lesson.tasks) {
                    if (task.id != "bic_route" || task.sequence.size() != 5) continue;
                    const auto& bic = task.sequence[1];
                    bicUsesC = bic.moveIds == std::vector<int>{167} &&
                        bic.fromMoveIds == std::vector<int>{232} &&
                        bic.inputMask == 64 && bic.req == "commit";
                }
                sawCorrectBicRetryContract = bicUsesC &&
                    curriculumLesson.player.rf == 1000 &&
                    curriculumLesson.player.blueIC == 1 &&
                    curriculumLesson.player.meter >= 2000 &&
                    curriculumLesson.player.posX == 20.0 &&
                    curriculumLesson.dummy.posX == 160.0 &&
                    curriculumLesson.lesson.wrongAction == "fail" &&
                    curriculumLesson.lesson.failureReset == "taskCheckpoint";
            }
            if (curriculumLesson.lessonId == "efz.offense.flicker_ic") {
                bool route = false;
                for (const auto& task : curriculumLesson.lesson.tasks) {
                    if (task.id != "fic_projectile" || task.sequence.size() != 2) continue;
                    const auto& cast = task.sequence[0];
                    const auto& fic = task.sequence[1];
                    route = cast.moveIds == std::vector<int>{250} &&
                        cast.req == "move" &&
                        fic.moveIds == std::vector<int>{167} &&
                        fic.fromMoveIds == std::vector<int>{250} &&
                        fic.inputMask == 64 && fic.req == "commit" &&
                        fic.hasFlickerIC && fic.flickerProjectilePattern == 401 &&
                        // 2026-07-14 user decision: no spacing gate on the
                        // shipped drill; the contact invariants are the proof.
                        fic.flickerMinDistance == 0 &&
                        task.script == "ep_fic_idle";
                }
                sawStrictFlickerIc = route &&
                    curriculumLesson.player.character == "sayuri" &&
                    curriculumLesson.player.rf == 500 &&
                    curriculumLesson.player.blueIC == 0 &&
                    curriculumLesson.dummy.character == "misaki";
                // 2026-07-14 (second pass): the spacing gate is GONE by user
                // decision - the live-projectile/no-contact/untouched-defender
                // invariants carry the proof - and the fighters spawn a bit
                // closer, still centered on 320 with room for the star to fly.
                sawCenteredFlickerIc = curriculumLesson.player.hasPos &&
                    curriculumLesson.dummy.hasPos &&
                    curriculumLesson.player.posX == 190.0 &&
                    curriculumLesson.dummy.posX == 450.0 &&
                    (curriculumLesson.player.posX +
                        curriculumLesson.dummy.posX) / 2.0 == 320.0;
            }
            if (curriculumLesson.lessonId == "efz.defense.recoil_guard") {
                bool pattern = false;
                if (curriculumLesson.lesson.tasks.size() == 3) {
                    const auto& task = curriculumLesson.lesson.tasks[2];
                    pattern = task.id == "rg_pattern" && task.sequence.size() == 3 &&
                        task.sequence[0].moveIds == std::vector<int>{168} &&
                        task.sequence[1].moveIds == std::vector<int>{151} &&
                        task.sequence[1].maxGap == 90 &&
                        task.sequence[2].moveIds == std::vector<int>{169} &&
                        task.sequence[2].maxGap == 90 &&
                        task.hasPos && task.posX == 20.0f &&
                        task.hasDummyPos && task.dummyPosX == 80.0f;
                }
                bool dashA = false, dashLowA = false, chain = false;
                for (const auto& episode : curriculumLesson.lesson.episodes) {
                    // 2026-07-14: reaction drills telegraph their dash normals
                    // with a neutral hop so the opener is humanly reactable.
                    dashA |= episode.ownerTask == "rg_stand" &&
                        episode.action == "66A" && episode.telegraph == "jump";
                    dashLowA |= episode.ownerTask == "rg_low" &&
                        episode.action == "662A" && episode.telegraph == "jump";
                    chain |= episode.ownerTask == "rg_pattern" &&
                        episode.action == "5A>5B>2C" && episode.approach == "none";
                }
                sawCorrectRgDrills = pattern && dashA && dashLowA && chain &&
                    curriculumLesson.player.posX == 290.0 &&
                    curriculumLesson.dummy.posX == 350.0;
            }
            if (curriculumLesson.lessonId == "efz.defense.rg_response") {
                bool challenge = false, blockThenPunish = false;
                for (const auto& task : curriculumLesson.lesson.tasks) {
                    if (task.id == "rg_challenge" && task.sequence.size() == 2) {
                        const auto& punish = task.sequence[1];
                        challenge = punish.moveIds == std::vector<int>{200} &&
                            punish.req == "land" && punish.maxGap == 150 &&
                            punish.dummyStateMoveIds == std::vector<int>{231} &&
                            punish.hasContact && punish.contact.source == "direct" &&
                            punish.contact.result == "hit" &&
                            punish.contact.moveIds == std::vector<int>{200};
                    } else if (task.id == "rg_block" && task.sequence.size() == 3) {
                        const auto& guard = task.sequence[1];
                        const auto& punish = task.sequence[2];
                        blockThenPunish =
                            guard.moveIds == std::vector<int>{151, 153} &&
                            guard.req == "move" && guard.maxGap == 150 &&
                            guard.dummyStateMoveIds == std::vector<int>{258} &&
                            punish.moveIds == std::vector<int>{200} &&
                            punish.req == "land" && punish.maxGap == 150 &&
                            punish.dummyStateMoveIds == std::vector<int>{258} &&
                            punish.hasContact && punish.contact.source == "direct" &&
                            punish.contact.result == "hit" &&
                            punish.contact.moveIds == std::vector<int>{200};
                    }
                }
                sawStrictRgResponse = challenge && blockThenPunish;
            }
            if (curriculumLesson.lessonId == "efz.strategy.take_space") {
                bool walkThenGuard = false;
                bool jumpAfterLow = false;
                for (const auto& task : curriculumLesson.lesson.tasks) {
                    if (task.id == "walk_guard" && task.sequence.size() == 2) {
                        const auto& walk = task.sequence[0];
                        const auto& guard = task.sequence[1];
                        walkThenGuard = walk.moveIds == std::vector<int>{1} &&
                            walk.req == "move" &&
                            guard.moveIds ==
                                std::vector<int>{150, 151, 152, 153, 154, 155} &&
                            guard.req == "move" && guard.maxGap == 240 &&
                            guard.dummyStateMoveIds == std::vector<int>{203};
                    }
                    if (task.id == "dash_whiff" && task.sequence.size() == 2) {
                        const auto& dash = task.sequence[0];
                        const auto& punish = task.sequence[1];
                        sawDashWhiffPunishRoute =
                            dash.moveIds == std::vector<int>{163} &&
                            dash.req == "move" && dash.whiffWindowMaxTicks == 120 &&
                            punish.req == "land" && punish.maxGap == 120 &&
                            punish.whiffWindowMaxTicks == 120;
                    } else if (task.id == "jump_low") {
                        jumpAfterLow = task.moveIds == std::vector<int>{5} &&
                            task.req == "move" && task.whiffWindowMaxTicks == 100;
                    }
                }
                sawAllTakeSpaceGuardReactions = walkThenGuard && jumpAfterLow;
            }
            if (curriculumLesson.lessonId == "efz.defense.guard_gauge") {
                bool build = false, empty = false, stored = false;
                for (const auto& task : curriculumLesson.lesson.tasks) {
                    if (task.id == "build_gauge") {
                        build = task.playerSeed.has && task.playerSeed.guard == 0 &&
                            task.hasGoalState && task.goalState.field == "guard" &&
                            task.goalState.requiresBlock;
                    } else if (task.id == "juggle_empty") {
                        empty = task.moveIds == std::vector<int>{254} &&
                            task.req == "land" && task.endsCombo &&
                            task.dummySeed.guard == 0;
                    } else if (task.id == "compare_juggle") {
                        stored = task.moveIds == std::vector<int>{254} &&
                            task.req == "land" && task.endsCombo &&
                            task.dummySeed.guard == 320 && task.hasCompare &&
                            task.compareMetric == "untechTicks" && task.compareOp == "gt";
                    }
                }
                sawGuardGaugeRecoveryLauncher = build && empty && stored &&
                    curriculumLesson.player.character == "misaki" &&
                    curriculumLesson.dummy.character == "nayukib";
            }
            if (curriculumLesson.lessonId == "efz.strategy.survive_pressure") {
                bool blockAll = false, escape = false;
                for (const auto& task : curriculumLesson.lesson.tasks) {
                    if (task.id == "block_all") {
                        blockAll = task.hasAbsence && task.absenceForbid == "attack" &&
                            task.absenceEnd == "episodeCycleEnd" &&
                            task.absenceFailOnHit && task.absenceMinBlocks == 3;
                    } else if (task.id == "escape_gap" && task.sequence.size() == 2) {
                        const auto& guarded = task.sequence[0];
                        const auto& jump = task.sequence[1];
                        escape = guarded.moveIds ==
                                     std::vector<int>{150, 151, 152, 153, 154, 155} &&
                            guarded.req == "move" &&
                            guarded.dummyStateMoveIds == std::vector<int>{200, 201} &&
                            jump.moveIds == std::vector<int>{4, 5, 6} &&
                            jump.req == "move" && jump.duringGap && jump.maxGap == 360;
                    }
                }
                sawPressureInteractionProofs = blockAll && escape;
            }
            if (curriculumLesson.lessonId == "efz.strategy.choose_rg") {
                for (const auto& task : curriculumLesson.lesson.tasks) {
                    if (task.id == "no_challenge") {
                        sawChooseRgBlockProof = task.hasAbsence &&
                            task.absenceEnd == "episodeCycleEnd" &&
                            task.absenceFailOnHit && task.absenceMinBlocks == 4;
                    }
                }
            }
            if (curriculumLesson.lessonId == "efz.strategy.attack_into_rg") {
                bool cancelRace = false, counterEpisode = false;
                bool poke = false, answer = false;
                for (const auto& task : curriculumLesson.lesson.tasks) {
                    if (task.id == "safe_cancel" && task.sequence.size() == 2) {
                        const auto& c5b = task.sequence[0];
                        const auto& f5b = task.sequence[1];
                        cancelRace = task.script == "ep_rg_dummy" &&
                            c5b.moveIds == std::vector<int>{201} &&
                            c5b.req == "rg" &&
                            f5b.moveIds == std::vector<int>{202} &&
                            f5b.fromMoveIds == std::vector<int>{201} &&
                            f5b.req == "land";
                    }
                    poke |= task.id == "end_turn_poke" &&
                        task.moveIds == std::vector<int>{200} && task.req == "block" &&
                        task.script == "ep_block_50";
                    answer |= task.id == "end_turn" && task.continuity == "continue" &&
                        task.hasAbsence && task.absenceEnd == "episodeCycleEnd" &&
                        task.absenceFailOnHit && task.absenceMinBlocks == 1 &&
                        task.script == "ep_answer_50";
                }
                for (const auto& episode : curriculumLesson.lesson.episodes) {
                    counterEpisode |= episode.id == "ep_rg_dummy" &&
                        episode.ownerTask == "safe_cancel" &&
                        episode.kind == "rg_answer" && episode.action == "5A" &&
                        episode.start == "taskArmed";
                }
                sawAttackIntoRgCancelRace = cancelRace && counterEpisode;
                sawEndTurnInteractionProof = poke && answer;
            }
            if (curriculumLesson.lessonId == "efz.strategy.spend_rf") {
                bool redCommit = false, blueCommit = false, defense = false;
                for (const auto& task : curriculumLesson.lesson.tasks) {
                    if (task.id == "spend_red" && task.sequence.size() == 3) {
                        const auto& ic = task.sequence[1];
                        redCommit = ic.moveIds == std::vector<int>{167} &&
                            ic.inputMask == 64 && ic.req == "commit";
                    } else if (task.id == "spend_blue" && task.sequence.size() == 5) {
                        const auto& opener = task.sequence[0];
                        const auto& ic = task.sequence[1];
                        blueCommit = ic.moveIds == std::vector<int>{167} &&
                            ic.inputMask == 64 && ic.req == "commit";
                        const auto& first66B = task.sequence[2];
                        const auto& delayed66B = task.sequence[3];
                        const auto& super = task.sequence[4];
                        sawSpendRfBlueParity = task.hasPos && task.posX == 20.0f &&
                            task.hasDummyPos && task.dummyPosX == 160.0f &&
                            task.playerSeed.has && task.playerSeed.rf == 1000 &&
                            task.playerSeed.blueIC == 1 &&
                            task.playerSeed.meter == 2000 &&
                            task.continuity == "sameCombo" &&
                            opener.moveIds == std::vector<int>{232} &&
                            opener.req == "land" &&
                            ic.fromMoveIds == std::vector<int>{232} &&
                            ic.maxGap == 120 &&
                            first66B.moveIds == std::vector<int>{231} &&
                            first66B.maxGap == 240 &&
                            delayed66B.moveIds == std::vector<int>{231} &&
                            delayed66B.minGap == 6 && delayed66B.maxGap == 300 &&
                            super.moveIds == std::vector<int>{304} &&
                            super.fromMoveIds == std::vector<int>{231} &&
                            super.req == "land" &&
                            super.maxGap == 180;
                    } else if (task.id == "defensive" && task.sequence.size() == 3) {
                        const auto& rf = task.sequence[0];
                        const auto& bic = task.sequence[1];
                        const auto& guard = task.sequence[2];
                        // 623C leaves Nayuki AIRBORNE (user correction
                        // 2026-07-14): accept the AIR IC 171 and the airborne
                        // escapes (air guard 156, backward double jump 16).
                        defense = rf.moveIds == std::vector<int>{252} &&
                            rf.inputMask == 64 && rf.req == "block" &&
                            bic.moveIds == std::vector<int>{167, 171} &&
                            bic.inputMask == 64 && bic.req == "commit" &&
                            bic.fromMoveIds == std::vector<int>{252} &&
                            guard.moveIds ==
                                std::vector<int>{150, 151, 152, 153, 154, 155,
                                                 156, 16};
                    }
                }
                sawStrategicRfProof = redCommit && blueCommit && defense;
            }
            if (curriculumLesson.lessonId == "efz.strategy.anti_air") {
                sawCorrectNayukiAntiAirs = !curriculumLesson.lesson.tasks.empty();
                for (const auto& task : curriculumLesson.lesson.tasks) {
                    if (task.moveIds != std::vector<int>{251, 252} ||
                        task.req != "land" || task.dummyState != "airborne") {
                        sawCorrectNayukiAntiAirs = false;
                    }
                }
            }
            if (curriculumLesson.lessonId == "efz.strategy.hit_confirm") {
                auto validBranch = [](const Mission::LessonTask& task) {
                    if (!task.hasBranch ||
                        task.branchStarterIds != std::vector<int>{201} ||
                        task.branchOnHit.size() != 3 ||
                        task.branchOnBlockHoldTicks != 90) return false;
                    const auto& f5b = task.branchOnHit[0];
                    const auto& c = task.branchOnHit[1];
                    const auto& special = task.branchOnHit[2];
                    return f5b.moveIds == std::vector<int>{202} &&
                        f5b.fromMoveIds == std::vector<int>{201} &&
                        f5b.req == "land" &&
                        c.moveIds == std::vector<int>{203} &&
                        c.fromMoveIds == std::vector<int>{202} &&
                        c.req == "land" &&
                        special.moveIds == std::vector<int>{253} &&
                        special.fromMoveIds == std::vector<int>{203} &&
                        special.req == "land";
                };
                bool hitTask = false, blockTask = false;
                int mixedTasks = 0;
                for (const auto& task : curriculumLesson.lesson.tasks) {
                    if (!validBranch(task)) continue;
                    if (task.id == "learn_hit" && task.script == "ep_learn_hit") {
                        hitTask = true;
                    } else if (task.id == "learn_block" &&
                               task.script == "ep_learn_block") {
                        blockTask = true;
                    } else if ((task.id == "mixed_1" || task.id == "mixed_2") &&
                               task.script == "ep_" + task.id) {
                        ++mixedTasks;
                    }
                }
                bool hitEpisode = false, blockEpisode = false;
                int mixedEpisodes = 0;
                for (const auto& episode : curriculumLesson.lesson.episodes) {
                    hitEpisode |= episode.id == "ep_learn_hit" &&
                        episode.ownerTask == "learn_hit" && episode.kind == "idle" &&
                        episode.variants.empty();
                    blockEpisode |= episode.id == "ep_learn_block" &&
                        episode.ownerTask == "learn_block" && episode.kind == "block" &&
                        episode.variants.empty();
                    if ((episode.id == "ep_mixed_1" || episode.id == "ep_mixed_2") &&
                        episode.variants == std::vector<std::string>{"idle", "block"} &&
                        episode.variantGroup == "mixed_confirm") {
                        ++mixedEpisodes;
                    }
                }
                sawGuidedHitConfirm = curriculumLesson.lesson.tasks.size() == 4 &&
                    hitTask && blockTask && mixedTasks == 2 && hitEpisode &&
                    blockEpisode && mixedEpisodes == 2;
            }
            if (curriculumLesson.lessonId == "efz.systems.guard_attacks") {
                bool taskGrades66B = false;
                for (const auto& task : curriculumLesson.lesson.tasks) {
                    if (task.id == "use_gp" && task.sequence.size() == 3) {
                        const auto& start = task.sequence[0];
                        const auto& absorb = task.sequence[1];
                        const auto& hit = task.sequence[2];
                        taskGrades66B =
                            start.moveIds == std::vector<int>{211} &&
                            start.req == "move" &&
                            absorb.moveIds.empty() && absorb.hasContact &&
                            absorb.contact.attacker == "dummy" &&
                            absorb.contact.target == "learner" &&
                            absorb.contact.source == "direct" &&
                            absorb.contact.result == "guard_point" &&
                            absorb.contact.moveIds == std::vector<int>{231} &&
                            hit.moveIds.empty() && hit.hasContact &&
                            hit.dummyStateMoveIds == std::vector<int>{231} &&
                            hit.contact.attacker == "learner" &&
                            hit.contact.target == "dummy" &&
                            hit.contact.source == "direct" &&
                            hit.contact.result == "hit" &&
                            hit.contact.moveIds == std::vector<int>{211};
                    }
                }
                bool trueDashNormal = false;
                for (const auto& episode : curriculumLesson.lesson.episodes) {
                    if (episode.id == "ep_cued_strike") {
                        // 2026-07-14: the guard-point drills hop before their
                        // dash normals so the learner can react on sight.
                        trueDashNormal = episode.kind == "macro" &&
                            episode.action == "66B" &&
                            episode.approach == "none" &&
                            episode.telegraph == "jump";
                    }
                }
                const bool declaresDirectContactState =
                    std::find(curriculumLesson.requires.begin(),
                              curriculumLesson.requires.end(),
                              "direct_contact_state") !=
                        curriculumLesson.requires.end();
                const bool declaresGuardPointContact =
                    std::find(curriculumLesson.requires.begin(),
                              curriculumLesson.requires.end(),
                              "guard_point_contact") !=
                        curriculumLesson.requires.end();
                sawTrueGuardAttackDashNormal =
                    taskGrades66B && trueDashNormal &&
                    declaresDirectContactState && declaresGuardPointContact;
            }
            if (curriculumLesson.lessonId == "efz.strategy.open_guard") {
                // 2026-07-14: the low/high openers use stable mids
                // c.5B(201) > f.5B(202), canceled into the
                // opener - low 2C(206) vs standing guard, overhead 236C(255)
                // vs crouching guard. ONLY the throw task is pokes: one
                // blocked poke (5A 200 or 66A 230, either) then the delayed
                // walk-up throw.
                bool low = false, high = false, delayedThrow = false;
                for (const auto& task : curriculumLesson.lesson.tasks) {
                    if (task.id == "open_low" && task.sequence.size() == 3) {
                        low = task.sequence[0].moveIds == std::vector<int>{201} &&
                            task.sequence[0].req == "block" &&
                            task.sequence[1].moveIds == std::vector<int>{202} &&
                            task.sequence[1].fromMoveIds == std::vector<int>{201} &&
                            task.sequence[1].req == "block" &&
                            task.sequence[2].moveIds == std::vector<int>{206} &&
                            task.sequence[2].fromMoveIds == std::vector<int>{202} &&
                            task.sequence[2].req == "land" &&
                            task.script == "ep_guard_stand";
                    } else if (task.id == "open_high" && task.sequence.size() == 3) {
                        high = task.sequence[0].moveIds == std::vector<int>{201} &&
                            task.sequence[0].req == "block" &&
                            task.sequence[1].moveIds == std::vector<int>{202} &&
                            task.sequence[1].fromMoveIds == std::vector<int>{201} &&
                            task.sequence[1].req == "block" &&
                            task.sequence[2].moveIds == std::vector<int>{255} &&
                            task.sequence[2].fromMoveIds == std::vector<int>{202} &&
                            task.sequence[2].req == "land" &&
                            task.script == "ep_guard_crouch";
                    } else if (task.id == "open_throw" && task.sequence.size() == 2) {
                        const auto& throwAction = task.sequence[1];
                        delayedThrow = task.sequence[0].moveIds ==
                                std::vector<int>{200, 230} &&
                            task.sequence[0].req == "block" &&
                            throwAction.moveIds == std::vector<int>{221} &&
                            throwAction.req == "move" && throwAction.minGap == 30 &&
                            throwAction.maxGap == 720 && task.script == "ep_guard_throw";
                    }
                }
                bool stand = false, crouch = false, adaptive = false;
                for (const auto& episode : curriculumLesson.lesson.episodes) {
                    stand |= episode.id == "ep_guard_stand" &&
                        episode.ownerTask == "open_low" &&
                        episode.kind == "guard" && episode.clip == "stand" &&
                        episode.start == "taskArmed";
                    crouch |= episode.id == "ep_guard_crouch" &&
                        episode.ownerTask == "open_high" &&
                        episode.kind == "guard" && episode.clip == "crouch" &&
                        episode.start == "taskArmed";
                    adaptive |= episode.id == "ep_guard_throw" &&
                        episode.ownerTask == "open_throw" &&
                        episode.kind == "block" &&
                        episode.start == "taskArmed";
                }
                sawStableGuardOpeners = low && high && delayedThrow &&
                    stand && crouch && adaptive;
            }
            if (curriculumLesson.lessonId == "efz.strategy.wakeup_offense") {
                sawMisaki214CResources = curriculumLesson.player.rf == 1000 &&
                    curriculumLesson.dummy.rf == 1000;
                const auto isKnockdownRoute = [](const Mission::LessonTask& task) {
                    if (task.sequence.size() < 4) return false;
                    const auto& a = task.sequence[0];
                    const auto& b = task.sequence[1];
                    const auto& c = task.sequence[2];
                    const auto& d = task.sequence[3];
                    return a.moveIds == std::vector<int>{200} && a.req == "land" &&
                        b.moveIds == std::vector<int>{201} &&
                        b.fromMoveIds == std::vector<int>{200} && b.req == "land" &&
                        c.moveIds == std::vector<int>{203} &&
                        c.fromMoveIds == std::vector<int>{201} && c.req == "land" &&
                        d.moveIds == std::vector<int>{253} &&
                        d.fromMoveIds == std::vector<int>{203} && d.req == "land";
                };
                bool meatySetup = false;
                bool throwSetup = false;
                bool baitSetup = false;
                for (const auto& task : curriculumLesson.lesson.tasks) {
                    if (task.id == "meaty" && task.sequence.size() == 5) {
                        const auto& meaty = task.sequence[4];
                        meatySetup = isKnockdownRoute(task) &&
                            meaty.moveIds == std::vector<int>{200, 204} &&
                            meaty.req == "hits" && meaty.afterWakeMaxTicks == 5;
                    } else if (task.id == "throw_setup") {
                        throwSetup = task.sequence.size() == 4 && isKnockdownRoute(task);
                    } else if (task.id == "wake_throw") {
                        sawOrderedWakeupThrow = task.sequence.empty() &&
                            task.moveIds == std::vector<int>{221} &&
                            task.req == "move" && task.afterWakeMaxTicks == 192 &&
                            task.continuity == "continue" && task.script == "ep_block";
                    } else if (task.id == "bait_setup") {
                        baitSetup = task.sequence.size() == 4 && isKnockdownRoute(task);
                    } else if (task.id == "bait" && task.continuity == "continue" &&
                        task.hasAbsence && task.absenceStart == "taskArmed" &&
                        task.absenceEnd == "dummyAttackEnd") {
                        sawWakeupBaitCarry = true;
                    }
                }
                sawWakeupKnockdownRoutes = meatySetup && throwSetup && baitSetup;
                for (const auto& episode : curriculumLesson.lesson.episodes) {
                    if (episode.id == "ep_reversal" && episode.ownerTask == "bait" &&
                        episode.kind == "macro" && episode.action == "214C" &&
                        episode.expectedMoveIds == std::vector<int>{252} &&
                        episode.start == "trigger" && episode.trigger == "onWakeup" &&
                        episode.repeatMode == "once") {
                        sawMisaki214CReversal = true;
                    }
                }
            }
            if (curriculumLesson.lessonId == "efz.offense.juggle_gauge") {
                for (const auto& task : curriculumLesson.lesson.tasks) {
                    if (task.id == "time_out" && task.continuity == "continue" &&
                        task.hasAbsence && task.absenceForbid == "attack" &&
                        task.absenceStart == "dummyUntech" &&
                        task.absenceEnd == "dummyUntechEmpty") {
                        sawLiveJuggleContinuation = true;
                    }
                }
            }
            if (curriculumLesson.lessonId == "efz.systems.wakeup_vulnerability") {
                for (const auto& task : curriculumLesson.lesson.tasks) {
                    if (task.id != "wakeup_hit" || task.sequence.size() != 2) continue;
                    const auto& meaty = task.sequence[1];
                    sawStrictWakeupJb = meaty.moveIds == std::vector<int>{208} &&
                        meaty.req == "hits" && meaty.afterWakeMaxTicks == 5;
                }
            }
            if (curriculumLesson.lessonId == "efz.systems.character_counters") {
                bool route = false;
                for (const auto& task : curriculumLesson.lesson.tasks) {
                    if (task.id != "counter_escape" || task.sequence.size() != 3) continue;
                    const auto& trigger = task.sequence[0];
                    const auto& redIc = task.sequence[1];
                    const auto& guard = task.sequence[2];
                    route = trigger.moveIds == std::vector<int>{201} &&
                        trigger.req == "move" &&
                        trigger.dummyStateMoveIds == std::vector<int>{261} &&
                        redIc.moveIds == std::vector<int>{167} &&
                        redIc.fromMoveIds == std::vector<int>{201} &&
                        redIc.inputMask == 64 && redIc.req == "commit" &&
                        guard.moveIds == std::vector<int>{151, 153} &&
                        guard.req == "move" &&
                        task.hasPos && task.posX == 330.0f &&
                        task.playerSeed.rf == 500 && task.playerSeed.blueIC == 0;
                }
                bool counterEpisode = false;
                for (const auto& episode : curriculumLesson.lesson.episodes) {
                    counterEpisode |= episode.id == "ep_counter_stance" &&
                        episode.ownerTask == "counter_escape" &&
                        episode.kind == "macro" && episode.action == "22A" &&
                        episode.approach == "none" && episode.start == "taskArmed";
                }
                const auto declares = [&](const char* capability) {
                    return std::find(curriculumLesson.requires.begin(),
                                     curriculumLesson.requires.end(), capability) !=
                           curriculumLesson.requires.end();
                };
                sawCharacterCounterFiveB = route && counterEpisode &&
                    declares("input_edges") && declares("input_commit_mapping") &&
                    curriculumLesson.player.character == "shiori" &&
                    curriculumLesson.dummy.character == "misaki";
            }
            if (curriculumLesson.lessonId == "efz.systems.special_knockdown") {
                const auto hasCapability = [&](const char* capability) {
                    return std::find(curriculumLesson.requires.begin(),
                                     curriculumLesson.requires.end(), capability) !=
                           curriculumLesson.requires.end();
                };
                bool routeIsExact = false;
                for (const auto& task : curriculumLesson.lesson.tasks) {
                    if (task.id != "otg_route" || task.sequence.size() != 4) continue;
                    const auto& throwStartup = task.sequence[0];
                    const auto& throwSuccess = task.sequence[1];
                    const auto& redIc = task.sequence[2];
                    const auto& pickup = task.sequence[3];
                    routeIsExact = task.continuity == "sameCombo" &&
                        throwStartup.moveIds == std::vector<int>{220} &&
                        throwStartup.req == "move" &&
                        throwSuccess.moveIds == std::vector<int>{221} &&
                        throwSuccess.fromMoveIds == std::vector<int>{220} &&
                        throwSuccess.req == "move" &&
                        redIc.moveIds == std::vector<int>{167} &&
                        redIc.fromMoveIds == std::vector<int>{221} &&
                        redIc.inputMask == 64 && redIc.req == "commit" &&
                        pickup.moveIds == std::vector<int>{206} &&
                        // 2026-07-14 live trace: the IC (167) is a completed system
                        // action, not a cancel-source - Mai passes through neutral
                        // (167 -> 8 -> 206), so a direct fromMoveIds proof on the
                        // pickup can never match. The OTG evidence is the downed
                        // direct-hit contact below plus sameCombo + maxGap.
                        pickup.fromMoveIds.empty() &&
                        pickup.req == "land" && pickup.hasContact &&
                        pickup.contact.source == "direct" &&
                        pickup.contact.result == "hit" &&
                        pickup.contact.targetStateBefore == "downed" &&
                        pickup.contact.moveIds == std::vector<int>{206} &&
                        pickup.dummyState.empty() && pickup.dummyStateMoveIds.empty();
                }
                bool ownsIdle = false;
                for (const auto& episode : curriculumLesson.lesson.episodes) {
                    ownsIdle |= episode.id == "ep_otg_idle" &&
                        episode.ownerTask == "otg_route" && episode.kind == "idle";
                }
                sawAuthoredMaiOtgRoute = routeIsExact && ownsIdle &&
                    curriculumLesson.player.character == "mai" &&
                    curriculumLesson.player.rf == 500 &&
                    curriculumLesson.player.blueIC == 0 &&
                    curriculumLesson.player.posX == 310.0 &&
                    curriculumLesson.dummy.character == "misaki" &&
                    curriculumLesson.dummy.posX == 330.0 &&
                    !hasCapability("combo_lifecycle") &&
                    hasCapability("otg_contact");
            }
            const auto runtimeIssues =
                Mission::Tutorial::RuntimeSupportIssues(curriculumLesson);
            bool launchable = runtimeIssues.empty();
            if (std::find(curriculumLesson.requires.begin(), curriculumLesson.requires.end(),
                          "dummy_script") != curriculumLesson.requires.end() &&
                curriculumLesson.lesson.episodes.empty()) {
                ++missingEpisodeCount;
            }
            for (const auto& task : curriculumLesson.lesson.tasks) {
                for (int id : task.moveIds) if (id <= 0) launchable = false;
                if (task.hitsRequired > 1 && task.req != "hits") launchable = false;
            }
            if (!launchable) {
                std::cerr << "PREFLIGHT UNAVAILABLE " << curriculumLesson.lessonId;
                for (const auto& issue : runtimeIssues) {
                    std::cerr << "\n  - " << issue;
                }
                std::cerr << '\n';
            }
            if (launchable) ++launchableCount;
            ++lessonCount;
        }
        Check(lessonCount == 53, "all 53 shipped Core tutorial lessons load");
        Check(shippedTaskCount == 147,
              "the audited Core tutorial contains exactly 147 tasks");
        Check(shippedEpisodeCount == 85,
              "the audited Core tutorial contains exactly 85 dummy episodes");
        Check(sourcedLessonCount == 53,
              "all 53 shipped Core tutorial lessons cite an auditable source");
        Check(sawInputEdgeLesson, "Core tutorial teaches an animation-less button through input edges");
        Check(sawOrderedLesson, "Core tutorial contains a real ordered action task");
        Check(sawCorrectNayukiCancelIds,
              "Nayuki cancel routes use the verified 236A and 623A move ids");
        Check(sawJumpingMisakiAirString,
              "the air jump-cancel task owns a looping Misaki jump episode");
        Check(sawCorrectAirRecoveryDrills,
              "Air Recovery uses Misaki 236A and grades 157 toward / 158 away separately");
        Check(sawCorrectAirRecoveryDecisionDrills,
              "Recovery Decisions uses 236A nearby, far-reaching 66C at range, and grades away as 158");
        Check(sawCorrectAirRecoveryPunish,
              "Recovery Punish uses Misaki 236A to launch Nayuki, then grades air throw 249");
        Check(sawCorrectNayukiRfIds,
              "Nayuki RF lesson uses the verified 623C and fallback 623B move ids");
        Check(sawCorrectBicSafetyContract,
              "BIC safety makes the learner BIC blocked 623C and guard the answer");
        Check(sawCorrectRedIcContract,
              "Red IC uses the universal 22C input and an adjacent 5C source");
        Check(sawCorrectBicRetryContract,
              "Blue IC uses the universal 22C input and restores a cornered RF/SP baseline on failure");
        Check(sawStrictFlickerIc,
              "FIC uses Sayuri 236A, the C-edge 22C cancel, live projectile, and safe spacing");
        Check(sawCenteredFlickerIc,
              "FIC keeps its exact 300-unit proof centered inside the visible arena");
        Check(sawCorrectRgDrills,
              "Recoil Guard uses 66A, 662A, then a cornered 5A-5B-2C pattern");
        Check(sawStrictRgResponse,
              "RG Response requires ordinary block and binds each punish to the current recovery");
        Check(sawAllTakeSpaceGuardReactions,
              "Take Space proves walk-then-block and a forward jump after the low whiffs");
        Check(sawCorrectNayukiAntiAirs,
              "Nayuki anti-air tasks accept only 623B/C, never 623A or normals");
        Check(sawOrderedWakeupThrow,
              "wakeup offense carries its authored knockdown into the delayed wakeup throw");
        Check(sawWakeupKnockdownRoutes,
              "every wakeup-offense drill grades 5A-c.5B-5C-236A as its knockdown setup");
        Check(sawWakeupBaitCarry,
              "wakeup reversal bait carries the knockdown and waits for a real dummy attack end");
        Check(sawMisaki214CReversal,
              "wakeup reversal bait injects Misaki 214C on wakeup, not 623C");
        Check(sawMisaki214CResources,
              "wakeup reversal bait gives both fighters full RF so Misaki 214C is driveable");
        Check(sawLiveJuggleContinuation,
              "Juggle Gauge observation continues from the live launched untech state");
        Check(sawStrictWakeupJb,
              "Wakeup Vulnerability requires j.B inside the sub-two-frame window");
        Check(sawCharacterCounterFiveB,
              "Character Counters requires Shiori 5B into C-committed Red IC, then guard");
        Check(sawPowerStatsPlacement,
              "Power Scaling keeps its requirements below the native combo/Power readout");
        Check(sawPowerSingleComboRoute,
              "Power Scaling keeps its three-hit observation inside one combo");
        Check(sawDirectDashMomentumJump,
              "Ground Dash momentum drill must jump directly from the dash state");
        Check(sawDirectArmorCancelEscape,
              "Recoil Armor cancel escape requires a direct 5A-to-neutral-jump transition");
        Check(sawBufferedArmorCommandThrow,
              "Recoil Armor drives Rumi's buffered 41236C in all three tasks, grades startup 264, and does not gate it on RF");
        Check(sawGuardGaugeRecoveryLauncher,
              "Guard Gauge starts empty and compares Misaki 236A recovery launches");
        Check(sawDashWhiffPunishRoute,
              "Take Space requires both the whiff-window dash and its landed punish");
        Check(sawPressureInteractionProofs,
              "Survive Pressure proves the guarded string and a guarded opener before gap escape");
        Check(sawChooseRgBlockProof,
              "Choose RG ends its no-challenge drill on a real guarded episode");
        Check(sawAttackIntoRgCancelRace,
              "Attack into RG makes Misaki answer c.5B with 5A and grades the direct f.5B cancel hit");
        Check(sawEndTurnInteractionProof,
              "Attack into RG separates the blocked poke from the no-attack guarded answer");
        Check(sawStrategicRfProof,
              "Spend RF proves C-committed IC and blocked defensive BIC safety");
        Check(sawSpendRfBlueParity,
              "Spend RF task 2 reproduces the standalone Blue IC setup and route contract");
        Check(sawAuthoredMaiOtgRoute,
              "Special Knockdown authors Mai throw-Red-IC-2C with direct OTG proof and no unused combo-end contract");
        Check(sawRfBlockCompletion,
              "RF blocking requires a real blocked hit plus Red, with timing comparison informational only");
        Check(sawGuidedHitConfirm,
              "Hit Confirm teaches guaranteed hit, guaranteed block, then two hidden decisions with one graded route");
        Check(sawStableGuardOpeners,
              "Open Guard uses stable c.5B-f.5B low/high strings and an auto-block walk-up throw");
        Check(sawTrueGuardAttackDashNormal,
              "Guard Attacks proves Misaki 66B was guard-pointed before Ayu 6C lands");
        Check(placeholderTaskCount == 4,
              "curriculum backlog has the audited 4 scenario placeholders");
        Check(missingEpisodeCount == 1,
              "curriculum backlog has the audited 1 missing dummy episode set");
        Check(launchableCount == 51,
              "capability/content preflight exposes exactly 51 honest lessons");
    }
    std::cout << "mission_data_tests passed\n";
    return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAILED: " << e.what() << '\n';
        return 1;
    }
}
