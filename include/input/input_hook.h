#pragma once

#include <cstddef>
#include <cstdint>
#include "input_poll_attack_edge_journal.h"
#include "normal_input_policy.h"

namespace InputHookPolicy {

enum class TutorialP2InputSource : uint8_t {
    Neutral,
    MotionQueue,
    ImmediatePress,
    BufferFreeze,
};

enum class ProcessRoute : uint8_t {
    NativePoll,
    BufferedBypass,
};

// Every tutorial-owned P2 source is delivered through HookedPoll while EFZ's
// original character-input processor remains in control. Besides polling, the
// original routine owns command/dash recognition and the character update; a
// direct field-write/bypass route is therefore never valid for an episode.
constexpr ProcessRoute TutorialP2Route(TutorialP2InputSource) {
    return ProcessRoute::NativePoll;
}

// A normal pulse may share a tutorial P2 lease only while that lease is
// authoritatively neutral. Motion/freeze queues and an already-active
// ImmediateInput press keep priority; actionable normals wait, while an
// exact-tick dash normal is retired by the runtime instead of firing late.
constexpr bool TutorialP2AllowsNormalPulse(TutorialP2InputSource source) {
    return source == TutorialP2InputSource::Neutral;
}

// processCharacterInput's fighter context is authoritative when EFZ polls the
// opposite physical binding during a side-routed pass. With no process
// context, fall back to the logical player derived from the poll argument.
constexpr int LogicalPollPlayer(int processPlayer, int polledPlayer) {
    return processPlayer == 1 || processPlayer == 2
        ? processPlayer
        : polledPlayer;
}

constexpr bool P1StartupNeutralApplies(bool gateActive,
                                       int processPlayer,
                                       int /*polledPlayer*/) {
    // Require an actual P1 character-processing context. Character Select and
    // other menus can poll the P1 binding with no fighter context; suppressing
    // that raw poll would strand the guarded manual-selector fallback.
    return gateActive && processPlayer == 1;
}

} // namespace InputHookPolicy

// Installs the hook on the game's input processing function.
void InstallInputHook();

// Dedicated mission/tutorial startup owner. Unlike the general macro and
// auto-action override lanes, this gate survives character-select cleanup and
// savestate restore. It must remain active until the runner baseline is ready
// and the tutorial has synchronously established its freeze/task input owner.
void SetP1StartupNeutralGate(bool active);
bool IsP1StartupNeutralGateActive();

// Trigger normals use an engine-pass-owned pulse instead of the detached
// ImmediateInput timer. AI-controlled fighters keep their AI flag and receive
// a post-AI raw pulse; already-human/tutorial fighters use the native poll.
// Both transports retain the press through the matching character consumer,
// then retain release-neutral through the next matching fighter pass.
enum class AutoActionNormalPulseOutcome : uint8_t {
    Unknown,
    Pending,
    Accepted,
    Failed,
    Cancelled,
};

NormalInputPolicy::SubmitResult QueueAutoActionNormalPulse(
    int playerNum,
    const NormalInputPolicy::Intent& intent,
    NormalInputPolicy::Timing timing = NormalInputPolicy::Timing::WhenActionable,
    uint64_t* generationOut = nullptr);
NormalInputPolicy::SubmitResult QueueAutoActionNormalPulse(
    int playerNum,
    int motionType,
    NormalInputPolicy::Timing timing = NormalInputPolicy::Timing::WhenActionable,
    uint64_t* generationOut = nullptr);
bool IsAutoActionNormalPulseActive(int playerNum);
// Queue admission is not execution. This generation result becomes Accepted
// only after EFZ's later character consumer starts the requested normal.
AutoActionNormalPulseOutcome GetAutoActionNormalPulseOutcome(
    int playerNum, uint64_t generation);
// True while either normal transport owns +392..397 through EFZ's later
// character consumer. Legacy immediate/stance writers pause in that window.
bool IsAutoActionNormalPulseOwningImmediateRegisters(int playerNum);
// Serializes legacy ImmediateInput writes against the AI post-write transport,
// scoped motion ownership, and the offline->netplay publication barrier. A
// successful acquire must be paired with Release; false means the detached
// writer must pause/retry without touching game memory.
bool TryAcquireImmediateInputWriteLease(int playerNum);
void ReleaseImmediateInputWriteLease(int playerNum);
void CancelAutoActionNormalPulse(int playerNum);
void CancelAllAutoActionNormalPulses();
// Synchronously scrubs and releases both normal lanes and both scoped motion
// transactions while the caller owns the global offline->netplay controller
// barrier. Returns false only when same-world cleanup remains unproven after
// the bounded retry.
bool DrainAutoActionNormalPulsesForOwnershipBoundary();
struct EfzTmEntryV1;
// Runtime holds this exact old world and has drained ordinary producers.
// Does not resolve current fighters or discard mismatched restoration tokens.
uint32_t RetireNativeInputOwners(const EfzTmEntryV1& world);

// Enables or disables the live input detours without destroying their MinHook state.
void SetInputHookActive(bool active);

// Removes the hook.
void RemoveInputHook();

// Engine-only control routing used by vanilla and Revival versions that cannot use the
// native Practice side-switch object. This swaps EFZ's two 16-byte live control maps,
// matching Revival's Practice SwitchPlayers hotkey without invoking netplay state.
//
// enable=false: normal routing (P1 controls -> P1, P2 controls -> P2)
// enable=true:  swapped bindings (P1 controls -> P2, P2 controls -> P1)
// Returns false if the engine control maps could not be updated and verified.
bool SetVanillaSwapInputRouting(bool enable);

// Playback diagnostics: counts how many engine polls actually consumed an active
// per-player override. Macro playback uses this to distinguish cursor progress from
// real input delivery.
void ResetInputPollOverrideHitCount(int playerNum);
uint32_t GetInputPollOverrideHitCount(int playerNum);

// Mission recording bridge: every attack-button rising-edge poll is journaled
// with its exact monotonic poll serial. The journal is a bounded SPSC queue;
// callers provide drain storage, so neither the poll hook nor the consumer
// allocates or takes a lock.
using InputPollAttackEdgeEvent = InputPollAttackEdgeJournalPolicy::Event;
using InputPollAttackEdgeDrainResult =
    InputPollAttackEdgeJournalPolicy::DrainResult;
constexpr std::size_t kInputPollAttackEdgeJournalCapacity = 128;

InputPollAttackEdgeDrainResult DrainInputPollAttackEdgeJournal(
    int playerNum,
    InputPollAttackEdgeEvent* eventsOut,
    std::size_t eventCapacity);
void ResetInputPollAttackEdgeJournal(int playerNum);

// Compatibility consumers aggregate every currently queued event into one
// mask and return the newest drained serial. New recording code should use the
// ordered drain above so separate polls cannot be coalesced.
uint8_t ConsumeInputPollAttackEdges(int playerNum);
uint8_t ConsumeInputPollAttackEdgeBatch(int playerNum, uint32_t& serialOut);
uint32_t GetInputPollSerial(int playerNum);

// Tutorial neutral gate: while an authoritative zero poll override is active,
// optionally sample the underlying physical poll before returning the override.
// This lets the session require a real release-to-neutral without allowing the
// page-confirm press or held direction to move/attack in game.
void SetPollOverridePhysicalObservation(int playerNum, bool enabled);
bool IsPollOverridePhysicalObservationEnabled(int playerNum);
uint8_t GetLastObservedPhysicalPollMask(int playerNum);
// Returns false until the hook has captured a real physical poll after the
// current observation lease was armed. Unlike the legacy 0xFF sentinel, the
// validity bit cannot be mistaken for all eight gameplay inputs.
bool TryGetLastObservedPhysicalPoll(int playerNum, uint8_t& maskOut,
                                    uint32_t* serialOut = nullptr);

// Arm a late-in-frame motion-token neutralization for the given player. If alsoDoFullCleanup
// is true, the hook will wait for the input buffer head to be stable for a couple frames
// (and no buffer-freeze is active) before performing a FullCleanupAfterToggle.
void InputHook_ArmTokenNeutralize(int playerNum, bool alsoDoFullCleanup);
