#include "game/mission/mission_data.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace {

void Check(bool condition, const char* message) {
    if (condition) return;
    std::cerr << "FAILED: " << message << '\n';
    std::exit(1);
}

std::string ReadAll(const char* path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

} // namespace

int main() {
    const char* path = "mission_data_boundary_test.json";
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

    const char* recordedPath = "recorded_legacy_in_range.json";
    {
        std::ofstream out(recordedPath, std::ios::binary | std::ios::trunc);
        out << R"json({"format":1,"name":"legacy in-range buffer","bgm":12,"steps":[{"notation":"5A","ids":[200]}]})json";
    }
    Check(Mission::LoadMission(recordedPath, loaded, error),
          "load generated legacy recording with in-range buffer value");
    Check(loaded.bgm == -1,
          "unmarked generated recording never treats a buffer index as a logical track");

    std::remove(path);
    std::remove(recordedPath);
    std::cout << "mission_data_tests passed\n";
    return 0;
}
