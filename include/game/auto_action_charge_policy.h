#pragma once

#include <cstdint>

// Pure policy shared by the runtime state machine and its tests. IC and FIC
// both submit EFZ's native 22C command; only the required evidence differs.
namespace AutoActionChargePolicy {

enum class Mode : uint8_t {
    Off = 0,
    InstantCharge = 1,
    FlickerInstantCharge = 2,
};

enum class PendingResolution : uint8_t {
    Continue,
    CompleteAccepted,
    CancelForContact,
};

constexpr int kGroundInstantChargeMove = 167;
constexpr int kAirInstantChargeMove = 171;
constexpr uint16_t kInstantChargeFrameFlag = 0x0008u;
constexpr int kKanoRandomMagicFirstMove = 306;
constexpr int kKanoRandomMagicLastMove = 308;
constexpr int kKanoRandomMagicLoopFromFrame = 18;
constexpr int kKanoRandomMagicLoopToFrame = 16;
constexpr int kKanoRandomMagicLoopCount = 6;

struct NativeGateSnapshot {
    bool readable = false;
    double rf = 0.0;
    int8_t blueCharge = 0;
    int32_t actionPhase = 0;
    uint16_t frameFlags = 0;
    int16_t currentRank = 0;
    int16_t instantChargeRank = 0;
    double y = 0.0;
};

// Mirrors the common character-consumer gate before token 52 is submitted.
// The +0x168 field is deliberately actionPhase, not contact: authored FIC
// scripts may set it without a collision.
constexpr bool NativeGateOpen(const NativeGateSnapshot& gate) {
    // Retail 0x423265 has two resource branches: red RF accepts every action
    // phase above 1; only the light-blue flag branch excludes phase 6. Keeping
    // that exclusion outside the OR rejects a native-valid red IC during the
    // character-specific counter phase.
    const bool resourceGate =
        (gate.rf >= 500.0 && gate.actionPhase > 1) ||
        (gate.blueCharge > 0 && gate.actionPhase > 1 &&
         gate.actionPhase != 6);
    return gate.readable && resourceGate &&
           (gate.frameFlags & kInstantChargeFrameFlag) != 0 &&
           gate.currentRank <= gate.instantChargeRank;
}

constexpr int ExpectedDestination(const NativeGateSnapshot& gate) {
    return gate.y < 0.0 ? kAirInstantChargeMove
                        : kGroundInstantChargeMove;
}

// IC is contact-driven; FIC uses an authored fixed window. Uncatalogued
// sources stay conservative so FIC cannot silently degrade into ordinary
// hit-confirm IC.
constexpr bool ReadyToSubmit(Mode mode, bool committedContactSeen,
                             const NativeGateSnapshot& gate,
                             bool fixedFicWindowAllowsPriorContact = false) {
    if (!NativeGateOpen(gate)) return false;
    switch (mode) {
        case Mode::InstantCharge:
            return committedContactSeen;
        case Mode::FlickerInstantCharge:
            return !committedContactSeen ||
                   fixedFicWindowAllowsPriorContact;
        case Mode::Off:
        default:
            return false;
    }
}

constexpr bool ContactCancels(Mode mode, bool committedContactSeen,
                              bool fixedFicWindowAllowsPriorContact = false) {
    return mode == Mode::FlickerInstantCharge && committedContactSeen &&
           !fixedFicWindowAllowsPriorContact;
}

// Ordinary IC is positive proof: missing entity evidence fails closed for a
// projectile follow-up but cannot manufacture a connection. An uncatalogued
// FIC source needs both resolver streams before arming; verified fixed-window
// sources may retain the request after contact.
constexpr bool ContactEvidenceSufficient(Mode mode, bool directReady,
                                         bool entityReady) {
    switch (mode) {
        case Mode::InstantCharge:
            return directReady;
        case Mode::FlickerInstantCharge:
            return directReady && entityReady;
        case Mode::Off:
        default:
            return false;
    }
}

// Without per-projectile parent-action metadata, every same-owner entity
// contact is recorded as a conservative possible FIC disqualifier; a later
// source-specific fixed-window policy decides whether it matters.
constexpr bool EntityContactCountsForMode(Mode mode,
                                          bool sourceEntityAttributed) {
    return mode == Mode::FlickerInstantCharge || sourceEntityAttributed;
}

// EFZ consumes 22C in the character update, before the battle update's later
// collision resolvers run: exact transaction acceptance therefore wins over a
// later contact from a lingering source-owned projectile. While the
// transaction is still pending, contact continues to cancel an uncatalogued
// FIC normally.
constexpr PendingResolution ResolvePendingObservation(
    Mode mode, bool exactTransactionAccepted, bool committedContactSeen,
    bool fixedFicWindowAllowsPriorContact = false) {
    if (exactTransactionAccepted) {
        return PendingResolution::CompleteAccepted;
    }
    if (ContactCancels(mode, committedContactSeen,
                       fixedFicWindowAllowsPriorContact)) {
        return PendingResolution::CancelForContact;
    }
    return PendingResolution::Continue;
}

constexpr bool EntitySpawnEstablishesTemporalLineage(
    bool priorReadable, bool priorAlive,
    bool currentReadable, bool currentAlive) {
    return priorReadable && !priorAlive && currentReadable && currentAlive;
}

constexpr bool ReadableEntityDespawnClearsLineage(bool currentReadable,
                                                  bool currentAlive) {
    return currentReadable && !currentAlive;
}

constexpr bool ExactDestinationAccepted(int moveId) {
    return moveId == kGroundInstantChargeMove ||
           moveId == kAirInstantChargeMove;
}

// A follow-up belongs to one action instance; a rewind or move change means a
// reset/transition occurred and a buffered 22C must not leak into a later action.
constexpr bool SameSourceInstance(int sourceMove, int sourceFirstFrame,
                                  int currentMove, int currentFrame) {
    return sourceMove == currentMove && currentFrame >= sourceFirstFrame;
}

constexpr bool IsVerifiedKanoRandomMagicFixedFicSource(int moveId) {
    return moveId >= kKanoRandomMagicFirstMove &&
           moveId <= kKanoRandomMagicLastMove;
}

// Kano's three random-magic supers repeat animation records 16..18 six times
// before records 20..23 expose their native FIC flag; that exact 18 -> 16 edge
// is part of one source action, not a new same-ID action.
constexpr bool IsVerifiedKanoRandomMagicLoop(int moveId,
                                             int previousFrame,
                                             int currentFrame,
                                             int completedLoops) {
    return IsVerifiedKanoRandomMagicFixedFicSource(moveId) &&
           previousFrame == kKanoRandomMagicLoopFromFrame &&
           currentFrame == kKanoRandomMagicLoopToFrame &&
           completedLoops >= 0 &&
           completedLoops < kKanoRandomMagicLoopCount;
}

} // namespace AutoActionChargePolicy
