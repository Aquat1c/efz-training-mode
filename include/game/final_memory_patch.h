#pragma once

// Applies a runtime patch to bypass the low-HP requirement for Final Memory (FM).
// Uses hash-qualified complete CMP descriptors for [this+0x108] (HP) against
// 3333 and owns the immediate change to 10000 through the patch ledger.
// Returns the number of patched sites.
int ApplyFinalMemoryHPBypass();

// Reverts the FM HP bypass by restoring all known patched sites to 3333.
// Returns the number of acquired sites fully restored. Empty restoration does
// no discovery/write; failed obligations remain recorded for another attempt.
int RevertFinalMemoryHPBypass();

// Convenience: enable/disable the FM bypass in one call. Returns number of
// changes performed (patched or reverted). This updates the requested local
// training preference and then synchronizes the live patch state if the
// current mode allows it.
int SetFinalMemoryBypass(bool enabled);

// Returns whether the FM bypass is requested by the local training UI/runtime.
bool IsFinalMemoryBypassEnabled();

// Returns whether this owner has an installed patch or unresolved restoration.
bool IsFinalMemoryBypassInstalled();

// Reconcile the live code patch with the current requested state and runtime
// safety gates (local practice active, not netplay suspended). Returns the
// number of sites changed.
int SyncFinalMemoryBypassForCurrentMode(const char* reason = nullptr);

// Always restore the original FM HP checks, even during suspend/shutdown.
// The requested local preference is preserved so the patch can be reapplied
// later when local training becomes active again.
int ForceRestoreFinalMemoryHPBypass(const char* reason = nullptr);
