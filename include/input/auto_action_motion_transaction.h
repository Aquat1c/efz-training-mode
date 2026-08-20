#pragma once

#include <cstdint>
#include <vector>

#include "scoped_input_reservation.h"

enum class AutoActionMotionOutcome : uint8_t {
    Unknown,
    Pending,
    Accepted,
    Failed,
    Cancelled,
};

enum class AutoActionMotionSubmitResult : uint8_t {
    Accepted,
    Busy,
    Invalid,
};

// Compatibility names retained for every existing P2 caller.  The underlying
// transaction is now symmetric; these aliases preserve the old source/API
// contract while new callers select their fighter explicitly.
using P2AutoActionMotionOutcome = AutoActionMotionOutcome;
using P2AutoActionMotionSubmitResult = AutoActionMotionSubmitResult;

// Scoped native producer/consumer transaction for either fighter.  Admission
// snapshots the selected fighter and its exact controller state; the temporary
// native-poll handoff is restored before collision processing, and Accepted is
// published only after that fighter's later command consumer starts the move.
AutoActionMotionSubmitResult SubmitAutoActionMotionTransaction(
    int playerNum, int motionType, int buttonMask,
    int maxConsumerVisits = 1, uint64_t* generationOut = nullptr);
AutoActionMotionSubmitResult SubmitReservedAutoActionMotionTransaction(
    int playerNum, int motionType, int buttonMask, int maxConsumerVisits,
    uint64_t reservationToken, uint64_t* generationOut = nullptr);
// General scoped-owner form used by staged recipes.  The legacy Reserved API
// above remains the AutoActionCharge wrapper so existing callers and their
// ownership contract do not change.
AutoActionMotionSubmitResult SubmitScopedAutoActionMotionTransaction(
    int playerNum, int motionType, int buttonMask, int maxConsumerVisits,
    ScopedInputOwner reservationOwner, uint64_t reservationToken,
    uint64_t* generationOut = nullptr);
AutoActionMotionSubmitResult SubmitAutoActionPatternTransaction(
    int playerNum, const std::vector<uint8_t>& pattern,
    bool patternFacingRight, int maxConsumerVisits = 1,
    uint64_t* generationOut = nullptr);
bool IsAutoActionMotionTransactionActive(int playerNum);
AutoActionMotionOutcome GetAutoActionMotionTransactionOutcome(
    int playerNum, uint64_t generation);
// Cancels only when `generation` is still the active transaction for this
// fighter.  Delayed owners must use this form so cleanup from an older request
// can never retire a successor published in the meantime.
bool CancelAutoActionMotionTransactionIfGeneration(
    int playerNum, uint64_t generation, const char* reason);
void CancelAutoActionMotionTransaction(int playerNum, const char* reason);
void CancelAllAutoActionMotionTransactions(const char* reason);

// Typed admission APIs distinguish a temporary owner collision from a
// malformed/unavailable request. Delayed triggers retry Busy without rerolling
// their selected row; Invalid retires that trigger explicitly.
P2AutoActionMotionSubmitResult SubmitP2AutoActionMotionTransaction(
    int motionType, int buttonMask, int consumerWaitPasses = 1,
    uint64_t* generationOut = nullptr);
P2AutoActionMotionSubmitResult SubmitP2AutoActionPatternTransaction(
    const std::vector<uint8_t>& pattern, bool patternFacingRight,
    int consumerWaitPasses = 1, uint64_t* generationOut = nullptr);

// Queue a P2 buffered motion without changing P2's controller persistently.
// The final mask is delivered by EFZ's native poll; command consumption is
// confirmed at the matching updateEntityState barrier before owned residue is
// removed. consumerWaitPasses=1 is fail-closed; larger values are reserved for
// authored wake/RG pre-buffer windows.
bool QueueP2AutoActionMotionTransaction(int motionType, int buttonMask,
                                        int consumerWaitPasses = 1,
                                        uint64_t* generationOut = nullptr);

// Same transaction for bespoke Final Memory patterns. patternFacingRight says
// which facing the already-encoded pattern represents so a cross-up before the
// producer pass can be mirrored safely.
bool QueueP2AutoActionPatternTransaction(const std::vector<uint8_t>& pattern,
                                         bool patternFacingRight,
                                         int consumerWaitPasses = 1,
                                         uint64_t* generationOut = nullptr);

bool IsP2AutoActionMotionTransactionActive();
P2AutoActionMotionOutcome GetP2AutoActionMotionTransactionOutcome(
    uint64_t generation);
void CancelP2AutoActionMotionTransaction(const char* reason);
