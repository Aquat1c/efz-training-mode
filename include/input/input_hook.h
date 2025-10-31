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