#pragma once

#include <cstdint>

// Installs the hook on the game's input processing function.
void InstallInputHook();

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

// Arm a late-in-frame motion-token neutralization for the given player. If alsoDoFullCleanup
// is true, the hook will wait for the input buffer head to be stable for a couple frames
// (and no buffer-freeze is active) before performing a FullCleanupAfterToggle.
void InputHook_ArmTokenNeutralize(int playerNum, bool alsoDoFullCleanup);
