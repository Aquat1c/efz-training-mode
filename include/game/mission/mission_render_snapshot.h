#pragma once

#include "game/mission/mission_data.h"

// Keep the per-frame renderer snapshot deliberately small without silently
// dropping identity needed by character-local entity notation. Centralizing
// the projection also makes additions to the render view unit-testable.
namespace Mission::RenderSnapshot {

inline void CopyMissionView(const ::Mission::Mission& source,
                            ::Mission::Mission& destination) {
    destination = ::Mission::Mission{};
    destination.type = source.type;
    destination.name = source.name;
    destination.description = source.description;
    // Entity PAT IDs are character-local. The renderer cannot resolve an
    // exact producer (and therefore cannot fold an immediate projectile hit
    // into its move) without the player resource name.
    destination.player.character = source.player.character;
    destination.steps = source.steps;
    destination.strictEntityContacts = source.strictEntityContacts;
    destination.entityContacts = source.entityContacts;
    destination.entityLifecycles = source.entityLifecycles;
    destination.hints = source.hints;
}

} // namespace Mission::RenderSnapshot
