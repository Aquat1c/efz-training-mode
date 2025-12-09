#pragma once

// Installs the hook on the game's input processing function.
void InstallInputHook();

// Removes the hook.
void RemoveInputHook();

// Vanilla-only control routing: when enabled and EfzRevival is NOT loaded, the poll hook
// will swap player indices so that P2 uses P1's controls (and vice versa). This mirrors
// Revival's live control swap behavior without touching engine internals.
//
// enable=false: normal routing (P1 controls -> P1, P2 controls -> P2)
// enable=true:  swapped routing (P1 controls -> P2, P2 controls -> P1)
void SetVanillaSwapInputRouting(bool enable);

// Arm a late-in-frame motion-token neutralization for the given player. If alsoDoFullCleanup
// is true, the hook will wait for the input buffer head to be stable for a couple frames
// (and no buffer-freeze is active) before performing a FullCleanupAfterToggle.
void InputHook_ArmTokenNeutralize(int playerNum, bool alsoDoFullCleanup);