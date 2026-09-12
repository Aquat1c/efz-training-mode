#pragma once

#include "entity_contact_phase_bridge_policy.h"

#include <cstdint>

namespace Mission::SnowbunnyTransitionPolicy {

// Awake Nayuki's 641236 creates two independently cycling Snowbunny lanes.
// The exact contact hook can observe the short attack phase even when the
// endpoint ring sampler has already advanced to recovery.  These are the only
// transitions for which the recorder/runtime may bridge that sampled gap.
struct Lane {
    std::uint16_t controllerPattern;
    std::uint16_t contactPattern;
    std::uint16_t recoveryPattern;
};

inline constexpr Lane kLaneOne{408, 404, 406};
inline constexpr Lane kLaneTwo{409, 405, 407};

using BridgeKind = ::Mission::EntityContactPhaseBridgePolicy::BridgeKind;

constexpr const Lane* LaneForContact(std::uint16_t contactPattern) {
    return contactPattern == kLaneOne.contactPattern ? &kLaneOne
         : contactPattern == kLaneTwo.contactPattern ? &kLaneTwo
                                                     : nullptr;
}

// This function classifies patterns only.  Callers must independently prove
// resource "nayukib", identical slot/generation, the same closed sample, and
// a complete exact-contact journal before accepting either bridge.
constexpr BridgeKind ClassifyBridge(std::uint16_t sampledFromPattern,
                                    std::uint16_t sampledToPattern,
                                    std::uint16_t exactContactPattern) {
    return ::Mission::EntityContactPhaseBridgePolicy::ClassifyBridge(
        "nayukib", sampledFromPattern, sampledToPattern,
        exactContactPattern);
}

static_assert(ClassifyBridge(404, 406, 404) ==
                  BridgeKind::DirectPostContactRetirement &&
              ClassifyBridge(405, 407, 405) ==
                  BridgeKind::DirectPostContactRetirement,
              "both direct Snowbunny contact retirements are curated");
static_assert(ClassifyBridge(408, 406, 404) ==
                  BridgeKind::CollapsedControllerToRecovery &&
              ClassifyBridge(409, 407, 405) ==
                  BridgeKind::CollapsedControllerToRecovery,
              "both missed Snowbunny contact phases are curated");
static_assert(ClassifyBridge(408, 407, 404) == BridgeKind::None &&
                  ClassifyBridge(409, 406, 405) == BridgeKind::None &&
                  ClassifyBridge(410, 406, 404) == BridgeKind::None &&
                  ClassifyBridge(408, 404, 404) == BridgeKind::None,
              "cross-lane, unknown, and ordinary producer edges stay strict");

} // namespace Mission::SnowbunnyTransitionPolicy
