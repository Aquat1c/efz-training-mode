#pragma once

// Applies a runtime patch to bypass the low-HP requirement for Final Memory (FM).
// It scans the .text section for comparisons of [this+0x108] (HP) against 3333 and
// rewrites the immediate to 10000 so the HP check is always satisfied.
// Returns the number of patched sites.
int ApplyFinalMemoryHPBypass();

// Reverts the FM HP bypass by restoring all known patched sites to 3333.
// Returns the number of sites reverted. If no tracked sites exist, may perform
// a conservative scan to find matching compares set to 10000 and restore them.
int RevertFinalMemoryHPBypass();

// Convenience: enable/disable the FM bypass in one call. Returns number of
// changes performed (patched or reverted). If the requested state is already
// active, returns 0.
int SetFinalMemoryBypass(bool enabled);

// Returns whether the FM bypass is currently enabled (based on tracked state).
bool IsFinalMemoryBypassEnabled();
