#pragma once

#include "game/mission/mission_data.h"

// Minimal per-frame renderer projection of the mission data.
namespace Mission::RenderSnapshot {

inline void CopyMissionView(const ::Mission::Mission& source,
                            ::Mission::Mission& destination) {
    destination = ::Mission::Mission{};
    destination.type = source.type;
    destination.name = source.name;
    destination.description = source.description;
    // Entity PAT IDs are character-local; the renderer cannot resolve an exact
    // producer without the player resource name.
    destination.player.character = source.player.character;
    destination.steps = source.steps;
    destination.strictEntityContacts = source.strictEntityContacts;
    destination.entityContacts = source.entityContacts;
    destination.entityLifecycles = source.entityLifecycles;
    destination.hints = source.hints;
}

} // namespace Mission::RenderSnapshot
