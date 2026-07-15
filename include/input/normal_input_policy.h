#pragma once

#include <cstdint>

#include "motion_constants.h"

namespace NormalInputPolicy {

constexpr uint8_t kInputRight = 0x01;
constexpr uint8_t kInputLeft  = 0x02;
constexpr uint8_t kInputDown  = 0x04;
constexpr uint8_t kInputA     = 0x10;
constexpr uint8_t kInputB     = 0x20;
constexpr uint8_t kInputC     = 0x40;
constexpr uint8_t kInputD     = 0x80;
constexpr uint8_t kAttackButtons = kInputA | kInputB | kInputC | kInputD;

enum class RelativeDirection : uint8_t {
    Neutral,
    Down,
    Forward,
    Back,
    DownForward,
    DownBack,
};

struct Intent {
    RelativeDirection direction{RelativeDirection::Neutral};
    uint8_t button{0};
    bool airborne{false};

    constexpr explicit operator bool() const {
        const uint8_t attack = static_cast<uint8_t>(button & kAttackButtons);
        return attack != 0 && (attack & static_cast<uint8_t>(attack - 1)) == 0;
    }
};

constexpr uint8_t ButtonForGroupIndex(int index) {
    return index == 0 ? kInputA
         : index == 1 ? kInputB
         : index == 2 ? kInputC
         : index == 3 ? kInputD
         : 0;
}

// Canonical mapping for every trigger-selectable normal. Keeping this in one
// pure function prevents the wake, delayed, and random-pool paths from drifting
// (the old wake fallback lost 6/4 and omitted 2S/j.X).
constexpr Intent IntentFromMotion(int motionType) {
    if (motionType >= MOTION_5A && motionType <= MOTION_5D) {
        return {RelativeDirection::Neutral,
                ButtonForGroupIndex(motionType - MOTION_5A), false};
    }
    if (motionType >= MOTION_2A && motionType <= MOTION_2D) {
        return {RelativeDirection::Down,
                ButtonForGroupIndex(motionType - MOTION_2A), false};
    }
    if (motionType >= MOTION_JA && motionType <= MOTION_JD) {
        return {RelativeDirection::Neutral,
                ButtonForGroupIndex(motionType - MOTION_JA), true};
    }
    if (motionType >= MOTION_6A && motionType <= MOTION_6D) {
        return {RelativeDirection::Forward,
                ButtonForGroupIndex(motionType - MOTION_6A), false};
    }
    if (motionType >= MOTION_4A && motionType <= MOTION_4D) {
        return {RelativeDirection::Back,
                ButtonForGroupIndex(motionType - MOTION_4A), false};
    }
    return {};
}

constexpr bool IsNormalMotion(int motionType) {
    return static_cast<bool>(IntentFromMotion(motionType));
}

constexpr uint8_t ResolveMask(const Intent& intent, bool facingRight) {
    if (!intent) return 0;

    const uint8_t forward = facingRight ? kInputRight : kInputLeft;
    const uint8_t back = facingRight ? kInputLeft : kInputRight;
    uint8_t direction = 0;
    switch (intent.direction) {
        case RelativeDirection::Neutral:     direction = 0; break;
        case RelativeDirection::Down:        direction = kInputDown; break;
        case RelativeDirection::Forward:     direction = forward; break;
        case RelativeDirection::Back:        direction = back; break;
        case RelativeDirection::DownForward: direction = kInputDown | forward; break;
        case RelativeDirection::DownBack:    direction = kInputDown | back; break;
    }
    return static_cast<uint8_t>(direction | intent.button);
}

// Native processCharacterInput stores directions plus button rising edges in
// its command-history ring; held buttons are omitted after their first poll.
constexpr uint8_t NativeHistorySample(uint8_t pollMask,
                                      uint8_t selectedButton,
                                      bool selectedButtonWasHeld) {
    const uint8_t direction = static_cast<uint8_t>(pollMask & 0x0F);
    const uint8_t risingEdge =
        !selectedButtonWasHeld && (pollMask & selectedButton) != 0
            ? selectedButton : 0;
    return static_cast<uint8_t>(direction | risingEdge);
}

enum class Timing : uint8_t {
    WhenActionable,
    Immediate,
};

constexpr bool PostureEligible(const Intent& intent,
                               bool groundActionable,
                               bool airActionable) {
    return static_cast<bool>(intent) &&
        (intent.airborne ? airActionable : groundActionable);
}

enum class Phase : uint8_t {
    Idle,
    AwaitingStart,
    PreNeutral,
    Press,
    ReleaseNeutral,
};

enum class SubmitResult : uint8_t {
    Accepted,
    Busy,
    Invalid,
};

struct Snapshot {
    uint64_t generation{0};
    Phase phase{Phase::Idle};
    Intent intent{};
    Timing timing{Timing::WhenActionable};

    constexpr explicit operator bool() const {
        return generation != 0 && phase != Phase::Idle && static_cast<bool>(intent);
    }
};

// Pure state machine. Time never advances it: only a matching target-character
// ProcessCharacterInput pass may acknowledge a phase. One successor is kept so
// a new trigger on the release tick cannot be silently lost.
class PulseState {
public:
    SubmitResult Submit(const Intent& intent, Timing timing,
                        uint64_t* generationOut = nullptr) {
        if (generationOut) *generationOut = 0;
        if (!intent) return SubmitResult::Invalid;

        // Immediate is used for the exact first tick of a dash normal. Neither
        // direction may queue across it: delaying Immediate behind another
        // pulse, or accepting a successor that an Immediate failure would have
        // to discard, breaks the exact-tick contract.
        if (phase_ != Phase::Idle &&
            (timing == Timing::Immediate || timing_ == Timing::Immediate ||
             pendingGeneration_ != 0)) {
            return SubmitResult::Busy;
        }

        ++nextGeneration_;
        if (nextGeneration_ == 0) ++nextGeneration_;
        const uint64_t generation = nextGeneration_;

        if (phase_ != Phase::Idle) {
            pendingGeneration_ = generation;
            pendingIntent_ = intent;
            pendingTiming_ = timing;
            if (generationOut) *generationOut = pendingGeneration_;
            return SubmitResult::Accepted;
        }

        generation_ = generation;
        intent_ = intent;
        timing_ = timing;
        phase_ = Phase::AwaitingStart;
        if (generationOut) *generationOut = generation_;
        return SubmitResult::Accepted;
    }

    bool Prepare(bool requestedButtonAlreadyHeld) {
        if (phase_ != Phase::AwaitingStart) return false;
        phase_ = requestedButtonAlreadyHeld ? Phase::PreNeutral : Phase::Press;
        return true;
    }

    Snapshot Current() const {
        if (phase_ == Phase::Idle) return {};
        return {generation_, phase_, intent_, timing_};
    }

    bool Acknowledge(const Snapshot& consumed) {
        if (!consumed || consumed.generation != generation_ ||
            consumed.phase != phase_) {
            return false;
        }

        switch (phase_) {
            case Phase::PreNeutral:
                phase_ = Phase::Press;
                break;
            case Phase::Press:
                phase_ = Phase::ReleaseNeutral;
                break;
            case Phase::ReleaseNeutral:
                if (pendingGeneration_ != 0) {
                    generation_ = pendingGeneration_;
                    intent_ = pendingIntent_;
                    timing_ = pendingTiming_;
                    phase_ = Phase::AwaitingStart;
                    pendingGeneration_ = 0;
                    pendingIntent_ = {};
                    pendingTiming_ = Timing::WhenActionable;
                } else {
                    phase_ = Phase::Idle;
                    generation_ = 0;
                    intent_ = {};
                    timing_ = Timing::WhenActionable;
                }
                break;
            case Phase::Idle:
            case Phase::AwaitingStart:
                return false;
        }
        return true;
    }

    bool Active() const { return phase_ != Phase::Idle; }
    bool NeedsReleaseNeutral() const { return phase_ == Phase::ReleaseNeutral; }
    bool HasPending() const { return pendingGeneration_ != 0; }

    bool RestartAwaiting() {
        if (phase_ != Phase::PreNeutral && phase_ != Phase::Press) return false;
        phase_ = Phase::AwaitingStart;
        return true;
    }

    // Retire only the current request. An already accepted successor keeps its
    // delivery contract and becomes the next AwaitingStart generation.
    bool RetireCurrent() {
        if (phase_ == Phase::Idle) return false;
        if (pendingGeneration_ != 0) {
            generation_ = pendingGeneration_;
            intent_ = pendingIntent_;
            timing_ = pendingTiming_;
            phase_ = Phase::AwaitingStart;
            pendingGeneration_ = 0;
            pendingIntent_ = {};
            pendingTiming_ = Timing::WhenActionable;
            return true;
        }
        phase_ = Phase::Idle;
        generation_ = 0;
        intent_ = {};
        timing_ = Timing::WhenActionable;
        return false;
    }

    void Reset() {
        phase_ = Phase::Idle;
        generation_ = 0;
        intent_ = {};
        timing_ = Timing::WhenActionable;
        pendingGeneration_ = 0;
        pendingIntent_ = {};
        pendingTiming_ = Timing::WhenActionable;
    }

private:
    uint64_t nextGeneration_{0};
    uint64_t generation_{0};
    Phase phase_{Phase::Idle};
    Intent intent_{};
    Timing timing_{Timing::WhenActionable};
    uint64_t pendingGeneration_{0};
    Intent pendingIntent_{};
    Timing pendingTiming_{Timing::WhenActionable};
};

} // namespace NormalInputPolicy
