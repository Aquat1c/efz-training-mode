#pragma once
// Mission setup capture/apply: characters, positions, HP/meter/RF, IC color,
// character-specific resources, and stage/BGM for direct loading via
// CharacterHotswap (skips character/stage select).
// Apply defers value writes past a fresh-match request or character/stage/
// palette mismatch (native Loading recycle); BGM changes in place.
// Tick drives the deferred apply from the mission engine tick.
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

// Unknown resource keys must fail setup rather than be silently ignored.
// Rumi's "barehanded" weapon-table swap is the one value requiring an
// actionable state; all others are direct memory fields.
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

// A reload becoming "not busy" is not proof the requested match was created;
// only the matching completion receipt may release values or a state restore.
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
// Retire setup ownership on unload; a stale receipt must not apply values later.
void Cancel(const char* reason = nullptr);
// Consume one deferred setup failure before baseline capture so it cannot
// become an authored checkpoint.
bool TakeFailure(std::string& errorOut);

} // namespace Mission::Setup
