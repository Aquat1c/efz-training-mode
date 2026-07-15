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

namespace ResourcePolicy {
enum class Family {
    None,
    Mishio,
    Mio,
    Kano,
    Mai,
    Ikumi,
    Misuzu,
    Rumi,
    Akiko,
    Neyuki,
};

enum class Verification {
    Unsupported,
    Immediate,
    RequiresActionable,
};

// Keep the authoring vocabulary closed. Unknown resource keys must fail setup
// instead of being silently ignored by the DisplayData adapter. Rumi's weapon
// table swap is the one curated value that may refuse to run until she is in
// an actionable state; all other listed values are direct memory fields.
inline Verification Classify(Family family, const std::string& key) {
    switch (family) {
        case Family::Mishio:
            return key == "element" ? Verification::Immediate : Verification::Unsupported;
        case Family::Mio:
            return key == "stance" ? Verification::Immediate : Verification::Unsupported;
        case Family::Kano:
            return key == "magic" ? Verification::Immediate : Verification::Unsupported;
        case Family::Mai:
            return key == "status" || key == "ghostTime" || key == "ghostCharge"
                ? Verification::Immediate : Verification::Unsupported;
        case Family::Ikumi:
            return key == "blood" || key == "genocide" || key == "levelGauge"
                ? Verification::Immediate : Verification::Unsupported;
        case Family::Misuzu:
            return key == "feathers" || key == "poisonTimer" || key == "poisonLevel"
                ? Verification::Immediate : Verification::Unsupported;
        case Family::Rumi:
            if (key == "barehanded") return Verification::RequiresActionable;
            return key == "kimchiActive" || key == "kimchiTimer"
                ? Verification::Immediate : Verification::Unsupported;
        case Family::Akiko:
            return key == "bulletCycle" ? Verification::Immediate : Verification::Unsupported;
        case Family::Neyuki:
            return key == "jam" ? Verification::Immediate : Verification::Unsupported;
        case Family::None:
            return Verification::Unsupported;
    }
    return Verification::Unsupported;
}
}

namespace DeferredPolicy {
enum class SettleEffect {
    Wait,
    Fail,
    ApplyValues,
    ReleaseForStateRestore,
};

// A native reload becoming "not busy" is not proof that the requested match
// was created. Only the matching completion receipt may release setup values or
// an embedded-state restore into that session.
constexpr SettleEffect DecideSettle(bool timedOut, bool reloadBusy,
                                    bool inMatch, bool matchingReceipt,
                                    bool applyValues) {
    if (timedOut) return SettleEffect::Fail;
    if (reloadBusy || !inMatch) return SettleEffect::Wait;
    if (!matchingReceipt) return SettleEffect::Fail;
    return applyValues ? SettleEffect::ApplyValues
                       : SettleEffect::ReleaseForStateRestore;
}
}

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
// Retire setup ownership when its runner/session is unloaded. The native load
// may still finish, but its stale receipt can no longer apply values later.
void Cancel(const char* reason = nullptr);
// Consume one deferred setup failure. The mission runner uses this before
// baseline capture so a timeout/failed value write cannot become an authored
// checkpoint accidentally.
bool TakeFailure(std::string& errorOut);

} // namespace Mission::Setup
