#pragma once

// Patches the FM low-HP requirement ([this+0x108] HP CMP 3333 -> 10000) via
// hash-qualified descriptors through the patch ledger; returns the number of
// patched sites.
int ApplyFinalMemoryHPBypass();

// Restores all patched sites to 3333; empty restoration does no
// discovery/write, and failed obligations remain recorded for retry.
int RevertFinalMemoryHPBypass();

// Sets the requested preference, then synchronizes the live patch state if the
// current mode allows; returns the number of changes performed.
int SetFinalMemoryBypass(bool enabled);

// Whether the FM bypass is requested by the local training UI/runtime.
bool IsFinalMemoryBypassEnabled();

// Whether this owner has an installed patch or unresolved restoration.
bool IsFinalMemoryBypassInstalled();

// Reconciles the live code patch with the requested state and the runtime
// safety gates (local practice active, not netplay suspended); returns the
// number of sites changed.
int SyncFinalMemoryBypassForCurrentMode(const char* reason = nullptr);

// Always restores the original FM HP checks, even during suspend/shutdown;
// the requested preference is preserved for later reapply.
int ForceRestoreFinalMemoryHPBypass(const char* reason = nullptr);
