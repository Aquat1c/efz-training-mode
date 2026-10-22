#pragma once

#include <string>

// GitHub release check: one successful check per training session; a failed attempt
// only opens a short retry cooldown. Acknowledgement is stored per-tag, so only a
// newer tag re-raises the badge. /releases/latest excludes drafts and pre-releases
// server-side; a final release outranks a pre-release with equal components, and
// beta users are not notified about newer betas. api.github.com requires the TLS
// 1.2 that XP's SChannel cannot negotiate: on XP the check always fails silently.
// The worker never touches game memory, never reads Config after Start(), and
// swallows every exception.
namespace UpdateCheck {

enum class Status {
    Disabled,          // the CheckForUpdates setting is off
    Idle,              // never started (menu not opened yet)
    Checking,          // worker running
    UpToDate,          // fetch succeeded, this build is current or newer
    UpdateAvailable,   // fetch succeeded, a newer release exists
    Failed,            // fetch or parse failed; permanently given up
};

// Idempotent and non-blocking. The CheckForUpdates and netplay gates are
// evaluated BEFORE the one-shot latch, so a gated call does not permanently
// cancel the check for this process.
void Start();

// Latest STABLE release tag so far ("" until the fetch completes or on failure); returned by value.
std::string LatestVersion();

// Fetch completed and the latest tag is strictly newer than this build, regardless of acknowledgement.
bool HasNewerRelease();

// HasNewerRelease() && the user has not opened HELP>ABOUT since that tag appeared; drives the badge.
bool IsUpdateAvailable();

// "[!]" when IsUpdateAvailable(), else ""; always a string literal.
const char* BadgeText();

// Records the current latest tag as seen (persisted beside the DLL); no-op when nothing is newer.
void AcknowledgeLatest();

Status GetStatus();

const char* ReleasesPageUrl();

} // namespace UpdateCheck
