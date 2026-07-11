#pragma once
//
// Mission setup capture/apply - the metadata that makes a recorded mission
// self-contained: characters, positions, HP/meter/RF, IC color, and
// character-specific resources (stance/element/jam/...), plus stage/BGM for
// direct loading via CharacterHotswap (skips character/stage select).
//
//   Capture(m)  - snapshot the CURRENT match state into m.player/m.dummy
//                 (called at record start: that is the state to restore).
//   Apply(m)    - restore a mission's setup. A fresh-match request, or a
//                 character/stage/palette mismatch, queues a direct native
//                 Loading recycle and defers values until it completes. BGM is
//                 presentation state and is changed in place.
//   Tick()      - drives the deferred apply (call once per frame from the
//                 mission engine tick).
//
#include "mission_data.h"

namespace Mission::Setup {

void Capture(::Mission::Mission& m);
// applyValues=false: hotswap to the mission's chars/stage/bgm only - value
// writes are skipped entirely (an embedded savestate restore supplies them).
bool Apply(const ::Mission::Mission& m,
           bool applyValues = true,
           bool forceFreshMatch = false,
           bool allowReload = true,
           bool acceptPreparedSession = false);
void Tick();
bool IsPending();   // a hotswap-deferred apply is still waiting

} // namespace Mission::Setup
