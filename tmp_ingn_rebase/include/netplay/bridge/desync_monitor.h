#pragma once

#include <cstdint>

// Experimental desync tracing and forensics. Gated by
// [Others] ExperimentalDesyncMonitor.
//
// For each accepted post-tick sample the monitor checksums a fixed set of
// sync-relevant EFZ memory regions (game-system state slice, battle screen
// slice, both character objects, the corresponding history input pair) into
// a static ring buffer. Steady-state capture performs no per-sample heap
// allocation and no game-thread file or socket I/O; boundary/event logs remain
// diagnostics. The
// frame/commit cursor pair is bounded to one frame but is attribution evidence,
// not a claim that the sample is Revival's exact snapshot boundary. Before
// capture can reach the negotiated future start, both peers negotiate the same
// protocol/schema and start frame. Checksums are exchanged over a tiny UDP
// side channel (host binds hostPort+2; the joiner sends to the host's literal
// IPv4 address). This is a separate UDP mapping and may require LAN use or an
// explicit forward; handshake failure simply leaves capture idle. A peer
// without the mod never completes the handshake, so the recorder remains a
// constant-time no-op. After the 64-sample selected-byte calibration, the
// first comparable effect projection, RNG scalar, or selected-gameplay
// difference freezes a causal pre/post window. A separate 96-frame contiguous
// selected-gameplay run is retained if confirmation is later reached. Both
// are evidence from selected projections, not proof that all state differs or
// that a later matching scalar repaired hidden state.
//
// A captured checksum window only marks in-memory evidence. Disk output
// is deferred to session end so one peer cannot perturb active rollback by
// dumping synchronously. The dump is written to
// <dll_dir>\logs\desync_<timestamp>\ with
// a report, hex dumps of the recorded frames around the divergence point,
// and copies of logEfz.txt / logNet.txt / native_host logs / the mod log.
// The session itself is never touched.
//
// An additional fixed-address EFZ hook ring records type-47 create/clear,
// effect-pass, and before/after-update state only after the peer-negotiated
// trace window is armed. Slot membership is read from the live EFZ ring on
// every update, so Revival rollback cannot stale observer-side tracking.
// This layer can attribute an observed RNG advance to a type-47 update, but
// it does not yet mark Revival snapshot SAVE/LOAD boundaries: the supported
// Revival profiles do not expose byte-verified snapshot-hook RVAs/signatures.
// Until those are added, the dump must not claim whether a first unequal
// pre-update tuple originated in restore, fan-out, or scheduling.
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

// Lightweight negotiation gate used by the frame hook. Disabled sessions do
// no monitor-specific memory reads. During an explicitly enabled handshake,
// ObserveFrame publishes only the native session frame so both peers can pick
// a common future start. Full state capture starts only after IsCaptureArmed.
bool IsSessionTracing();
bool IsCaptureArmed();
void ObserveFrame(int frame);

// Per-frame recorder, called from the per-frame tick hook after the original
// tick. Cheap no-op when disabled or not in an online battle. |frame| and
// |commitFrame| are the post-tick session counters; |sessionPtr| is the
// validated rollback session object.
void RecordFrameTick(uintptr_t sessionPtr, int frame, int commitFrame);
}
