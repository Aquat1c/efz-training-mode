#pragma once

#include "game/mission/mission_data.h"

#include <string>

namespace Mission::LegacyEntityMigration {

enum class Result {
    NotApplicable = 0,
    Upgraded,
    EvidenceRejected,
};

struct Report {
    Result result = Result::NotApplicable;
    int objectivesAdded = 0;
    std::string diagnostic;
};

// Read-only compatibility adapter for generated format-1 recordings. It
// promotes old whiff reviews and repairs the narrowly identified Awake-Nayuki
// Snowbunny producer-ordering bug. The Mission value is replaced only after
// every affected objective has been reconstructed from a complete adjacent v5
// recorder sidecar. The JSON and sidecar on disk are never modified.
Report UpgradeLegacyLifecycleReviews(Mission& mission,
                                     const std::string& sourcePath);

} // namespace Mission::LegacyEntityMigration
