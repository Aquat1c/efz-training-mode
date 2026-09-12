#pragma once

#include <cstdint>

enum class ScopedInputOwner : uint8_t {
    None = 0,
    AutoActionCharge = 1,
    KaoriRecoilDucking = 2,
    WakeForcedNeutral = 3,
    WakeMacroCleanup = 4,
};

// Claims future mod-owned input for one fighter without changing EFZ's
// controller flags.  An already-admitted primary action may finish; new input
// producers must wait until the reservation is released.
bool TryReserveScopedInput(int playerNum, ScopedInputOwner owner,
                           uint64_t& tokenOut);
bool ScopedInputReservationMatches(int playerNum, ScopedInputOwner owner,
                                   uint64_t token);
bool IsScopedInputReserved(int playerNum);
bool IsScopedInputReservedByOther(int playerNum, uint64_t allowedToken = 0);
void ReleaseScopedInputReservation(int playerNum, ScopedInputOwner owner,
                                   uint64_t token);
void ReleaseAllScopedInputReservations(ScopedInputOwner owner);
