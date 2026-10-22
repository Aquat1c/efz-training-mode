#pragma once

#include "mission_data.h"

#include <cstddef>
#include <string>
#include <vector>

namespace Mission::Authoring {

struct ScenarioSummary {
    std::string id;
    std::string file;
    std::string name;
    std::string description;
    std::string categoryId;
    std::string type;
    int difficulty = 0;
    int order = 0;
};

// Picker data; `categories` keeps the stable id beside its label so UI code
// never infers an id from display text.
struct PackSummary {
    std::string packJsonPath;
    std::string id;
    std::string name;
    std::string author;
    std::string description;
    std::string version;
    bool editable = false;
    std::vector<PackCategory> categories;
    std::vector<ScenarioSummary> scenarios;
    std::size_t missionCount = 0;
};

struct CreatePackResult {
    std::string packJsonPath;
    std::string packId;
    std::string folderName;
    std::string defaultCategoryId;
};

struct MissionMetadata {
    std::string name;
    std::string description;
    // "combo" and "trial" appear in the Trials tab; "mission" appears in
    // the Missions tab. Tutorial content is deliberately not generated here.
    std::string type = "combo";
    int difficulty = 1;
};

struct PackMetadata {
    std::string name;
    std::string author;
    std::string description;
    std::string version;
};

struct CategoryMetadata {
    std::string label;
    std::string description;
};

// Metadata for an already-published local mission; editing `categoryId` changes
// only library placement - stable mission id, filename, order, recorded inputs,
// setup, contacts and entity evidence are never regenerated.
struct ExistingMissionMetadata : MissionMetadata {
    std::string categoryId;
};

struct PublishResult {
    std::string missionPath;
    // Empty only when the caller supplied no authoring trace; recorder
    // publication always supplies one.
    std::string entityTracePath;
    std::string missionId;
    std::string fileName;
    int order = 0;
};

// Converts display text into a deterministic lowercase ASCII slug.
// Empty/non-ASCII-only input becomes "untitled"; Windows device names are
// disambiguated so the result is always a valid path component.
std::string MakeSlug(const std::string& text);

// Strict single-component validation for generated authoring filenames:
// rejects paths, traversal, control/non-ASCII bytes, trailing dots/spaces and
// Windows reserved device names. Read-only pack scenarios use
// ResolveScenarioMissionPath instead so nested layouts stay compatible.
bool ValidateSafeComponent(const std::string& component, std::string& errorOut);

// Resolves an existing pack Scenario::file for playback: nested folders are
// allowed, but every component must be safe and the canonical target must
// stay below packFolder (including through junctions/symlinks). Only an
// existing .json regular file succeeds.
bool ResolveScenarioMissionPath(const std::string& packFolder,
                                const std::string& relativeJsonPath,
                                std::string& absolutePathOut,
                                std::string& errorOut);

bool LoadPackSummary(const std::string& packJsonPath,
                     PackSummary& out,
                     std::string& errorOut);
bool EnumeratePackSummaries(const std::string& missionsRoot,
                            std::vector<PackSummary>& out,
                            std::string& errorOut);

// Creates one unique immediate child of missionsRoot, ready for authoring,
// starting with an ordered "General" category.
bool CreatePack(const std::string& missionsRoot,
                const std::string& title,
                const std::string& author,
                const std::string& description,
                const std::string& version,
                CreatePackResult& out,
                std::string& errorOut);

// Adds a category without changing the pack in place; exact duplicate labels
// fail, and different labels normalizing to the same slug get a stable
// numeric suffix.
bool AddCategory(const std::string& missionsRoot,
                 const std::string& packJsonPath,
                 const std::string& label,
                 const std::string& description,
                 PackCategory& out,
                 std::string& errorOut);

// Display-metadata edits for local editable content: retain every stable
// ID/path/order, reject bundled/read-only packs. Mission edits stage the
// mission JSON and its mirrored Scenario row, rolling the mission file back
// if the manifest commit fails.
bool UpdatePackMetadata(const std::string& missionsRoot,
                        const std::string& packJsonPath,
                        const PackMetadata& metadata,
                        std::string& errorOut);
bool UpdateCategoryMetadata(const std::string& missionsRoot,
                            const std::string& packJsonPath,
                            const std::string& categoryId,
                            const CategoryMetadata& metadata,
                            std::string& errorOut);
bool UpdateMissionMetadata(const std::string& missionsRoot,
                           const std::string& packJsonPath,
                           const std::string& missionId,
                           const ExistingMissionMetadata& metadata,
                           std::string& errorOut);

// Publishes a copy of `recording` with its metadata, stable id and Scenario
// entry; both JSON files are staged beside their destinations. A non-empty
// stagedEntityTracePath must name an already-written file beside pack.json and
// is promoted to <mission>.entities.jsonl in the same transaction; on any
// later failure the promoted files are removed and the original pack remains
// untouched. The staging path is consumed once promotion begins.
bool PublishMission(const std::string& missionsRoot,
                    const std::string& packJsonPath,
                    const std::string& categoryId,
                    const Mission& recording,
                    const MissionMetadata& metadata,
                    PublishResult& out,
                    std::string& errorOut,
                    const std::string& stagedEntityTracePath = {});

} // namespace Mission::Authoring
