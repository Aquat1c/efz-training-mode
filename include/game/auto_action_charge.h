#pragma once

#include "auto_action_charge_policy.h"

// Runtime owner for the IC/FIC follow-up attached to one auto-action execution.
// Both modes ask EFZ to consume its native 22C command; they differ only in the
// evidence required before submission. Arm publishes a passive observer at
// primary-input admission; it cannot submit 22C until that exact primary
// generation is accepted and the source's IC/FIC gate is proven. Pass 0 only
// for legacy producers without an attributed motion generation.
bool ArmAutoActionChargeFollowup(
    int playerNum, AutoActionChargePolicy::Mode mode,
    int triggerType, int actionType,
    uint64_t* reservationTokenOut = nullptr,
    uint64_t primaryMotionGeneration = 0);

// Bounded and never sleeps; called once per internal battle update from the
// tick-integrated auto-action path.
void TickAutoActionChargeFollowups();

bool IsAutoActionChargeFollowupActive(int playerNum);

// Synchronous; also retires an admitted 22C transaction. Safe on an inactive slot.
void CancelAutoActionChargeFollowup(int playerNum, const char* reason);
// Generation-safe: a stale wake/RG cleanup must not retire a newer follow-up
// which replaced its reservation.
bool CancelAutoActionChargeFollowupIfReservation(
    int playerNum, uint64_t reservationToken, const char* reason);
void CancelAllAutoActionChargeFollowups(const char* reason);
