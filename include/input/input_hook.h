#pragma once

#include <cstdint>
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

} // namespace InputHookPolicy

// Installs the hook on the game's input processing function.
void InstallInputHook();

// Trigger normals use an engine-pass-owned pulse instead of the detached
// ImmediateInput timer. AI-controlled fighters keep their AI flag and receive
// a post-AI raw pulse; already-human/tutorial fighters use the native poll.
// Both transports retain the press through the matching character consumer,
// then retain release-neutral through the next matching fighter pass.
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
// True while either normal transport owns +392..397 through EFZ's later
// character consumer. Legacy immediate/stance writers pause in that window.
bool IsAutoActionNormalPulseOwningImmediateRegisters(int playerNum);
// Serializes legacy ImmediateInput writes against the AI post-write transport.
// A successful acquire must be paired with Release; false means the normal
// pulse owns the raw registers and the legacy writer should pause/retry.
bool TryAcquireImmediateInputWriteLease(int playerNum);
void ReleaseImmediateInputWriteLease(int playerNum);
void CancelAutoActionNormalPulse(int playerNum);
void CancelAllAutoActionNormalPulses();

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

// Mission deadline bridge: records attack-button rising edges from the exact
// value returned by EFZ's input-poll hook. Consume clears the pending edge bits;
// the monotonic serial is diagnostic and does not itself identify a game event.
uint8_t ConsumeInputPollAttackEdges(int playerNum);
uint32_t GetInputPollSerial(int playerNum);

// Tutorial neutral gate: while an authoritative zero poll override is active,
// optionally sample the underlying physical poll before returning the override.
// This lets the session require a real release-to-neutral without allowing the
// page-confirm press or held direction to move/attack in game.
void SetPollOverridePhysicalObservation(int playerNum, bool enabled);
bool IsPollOverridePhysicalObservationEnabled(int playerNum);
uint8_t GetLastObservedPhysicalPollMask(int playerNum);

// Arm a late-in-frame motion-token neutralization for the given player. If alsoDoFullCleanup
// is true, the hook will wait for the input buffer head to be stable for a couple frames
// (and no buffer-freeze is active) before performing a FullCleanupAfterToggle.
void InputHook_ArmTokenNeutralize(int playerNum, bool alsoDoFullCleanup);
