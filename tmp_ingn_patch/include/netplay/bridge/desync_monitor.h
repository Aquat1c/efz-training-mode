#pragma once

#include <cstdint>

// Desync detection and forensics. Gated by [Others] DesyncDetection.
//
// Per confirmed battle frame the monitor checksums a fixed set of
// sync-relevant EFZ memory regions (game-system state slice, battle screen
// slice, both character objects, the committed input pair) into a static
// ring buffer - no allocation, no file I/O on the game thread. Checksums are
// exchanged with the peer over a tiny UDP side channel (host binds
// hostPort+2; the joiner sends to the host's address, reusing the same NAT
// path as the game traffic). A desync is declared only after a run of
// consecutive per-frame checksum mismatches with no match in between, which
// makes rollback-transient noise a non-issue. A peer without the mod simply
// never answers and the monitor stays passive.
//
// On detection a worker thread dumps <dll_dir>\logs\desync_<timestamp>\ with
// a report, hex dumps of the recorded frames around the divergence point,
// and copies of logEfz.txt / logNet.txt / native_host logs / the mod log.
// The session itself is never touched.
namespace netplay::bridge::desync_monitor
{
// Session lifecycle. |address| is the join target for clients (empty for the
// host); |port| is the host listen port the side channel derives from.
void NotifySessionStarted(
    int netplayRole,
    const char* address,
    uint16_t port,
    const char* nickname);
void NotifySessionEnded(const char* reason);
void Shutdown();

// Per-frame recorder, called from the per-frame tick hook after the original
// tick. Cheap no-op when disabled or not in an online battle. |frame| and
// |commitFrame| are the post-tick session counters; |sessionPtr| is the
// validated rollback session object.
void RecordFrameTick(uintptr_t sessionPtr, int frame, int commitFrame);
}
