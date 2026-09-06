#pragma once

#include <string>

// GitHub release check: one successful check per training session.
//
// Start() is called every time the training menu is opened. The first call that
// passes the gates spawns a background worker which fetches the latest STABLE
// release tag; once that succeeds, every later call in the process is a no-op.
// A FAILED attempt does not close the session out - it opens a short cooldown
// and may be retried on a later menu open, so a user whose network was not up
// yet is not silently stuck with no check for the rest of the session.
//
// A newly published release raises the badge again on the NEXT session, because
// the acknowledgement is stored per-tag: only a tag newer than the acknowledged
// one counts as available. The menu polls the cheap accessors below to decorate the HELP tab and
// its ABOUT sub-tab with a "[!]" badge while a newer release exists that
// the user has not yet looked at; opening HELP>ABOUT acknowledges the current
// latest tag, and the badge then stays hidden until an even newer tag ships.
//
// Design notes that matter:
//  * /releases/latest is used deliberately: it excludes drafts and pre-releases
//    server-side. A user on "1.2.0_beta6" is still correctly told that "1.2"
//    supersedes them, because a final release outranks a pre-release with equal
//    components - but beta users are NOT notified about newer betas.
//  * The transport is WinHTTP resolved at runtime. Windows XP's SChannel cannot
//    negotiate the TLS 1.2 that api.github.com requires, so on XP this always
//    fails - silently, in under a second, exactly like having no network.
//  * The worker never touches game memory, never reads Config after Start(),
//    and swallows every exception. It must never be able to disturb a match.
namespace UpdateCheck {

enum class Status {
    Disabled,          // the CheckForUpdates setting is off
    Idle,              // never started (menu not opened yet)
    Checking,          // worker running
    UpToDate,          // fetch succeeded, this build is current or newer
    UpdateAvailable,   // fetch succeeded, a newer release exists
    Failed,            // fetch or parse failed; permanently given up
};

// Idempotent and non-blocking. Gated by the CheckForUpdates setting and by
// netplay; those gates are evaluated BEFORE the one-shot latch, so a call made
// while gated does not permanently cancel the check for this process.
void Start();

// Latest STABLE release tag known so far ("" until the fetch completes, or on
// failure). Returned by value - the worker owns the underlying string.
std::string LatestVersion();

// True once the fetch completed and the latest tag is strictly newer than this
// build, regardless of acknowledgement.
bool HasNewerRelease();

// HasNewerRelease() && the user has not opened HELP>ABOUT since that tag
// appeared. This is what drives the badge.
bool IsUpdateAvailable();

// Badge text for label builders: "[!]" when IsUpdateAvailable(), else "".
// Always a string literal - safe to hold across frames.
const char* BadgeText();

// Records the current latest tag as seen (persisted beside the DLL) so the
// badge disappears until a newer release is published. No-op when there is
// nothing newer to acknowledge, so it is safe to call every frame.
void AcknowledgeLatest();

Status GetStatus();

// Public page for the user to fetch the release from.
const char* ReleasesPageUrl();

} // namespace UpdateCheck
