#pragma once

#include "auto_action_charge_policy.h"

// Runtime owner for the optional IC/FIC follow-up attached to one auto-action
// execution.  Both modes ultimately ask EFZ to consume its native 22C command;
// the distinction is the evidence required before that command is submitted.
//
// Arm publishes only a passive observer. It must be called when the primary
// input is admitted (before a projectile can spawn or connect), but it cannot
// submit 22C until that exact primary generation is accepted and the source's
// IC/FIC gate is proven. Pass 0 only for legacy producers which cannot expose
// an attributed motion generation yet.
bool ArmAutoActionChargeFollowup(
    int playerNum, AutoActionChargePolicy::Mode mode,
    int triggerType, int actionType,
    uint64_t* reservationTokenOut = nullptr,
    uint64_t primaryMotionGeneration = 0);

// Called once per internal battle update from the tick-integrated auto-action
// path.  This function is bounded and never sleeps.
void TickAutoActionChargeFollowups();

bool IsAutoActionChargeFollowupActive(int playerNum);

// Cancellation is synchronous and also retires an admitted 22C transaction
// owned by the selected fighter.  It is safe to call on an inactive slot.
void CancelAutoActionChargeFollowup(int playerNum, const char* reason);
// Generation-safe cancellation for a coordinator which admitted the primary
// action and charge reservation as one compound request.  A stale wake/RG
// cleanup must not retire a newer follow-up which replaced its reservation.
bool CancelAutoActionChargeFollowupIfReservation(
    int playerNum, uint64_t reservationToken, const char* reason);
void CancelAllAutoActionChargeFollowups(const char* reason);
