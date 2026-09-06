#pragma once

#include <string>

// Mod version-string ordering ("1.2.2", "1.2.0_beta6", "v1.2.2"). Used by the
// GitHub update check to decide whether a release tag is newer than this build.
//
// Ported from mod_projects/InGameNetplay/src/netplay/core/version_compare.cpp
// (uncommitted prior art) with two defects fixed - see the notes on IsNewer()
// and on numeric clamping in the .cpp.
//
// TAGGING CONSTRAINTS this comparator imposes on the release process. These are
// deliberate simplifications, not bugs; keep tags inside them:
//   * No trailing words on a final-release tag. ANY non-empty tail after the
//     dotted numbers is treated as a PRE-RELEASE, so "0.8.2-release" (a real
//     tag in this repo's history) sorts BELOW plain "0.8.2".
//   * No build metadata: "1.2.0+build5" likewise sorts below "1.2.0".
//   * At most four dotted components; a fifth is read as a pre-release suffix.
//   * Pre-release tags should be limited to alpha / beta / rc. Ordering of the
//     alphabetic token is plain lexicographic on its lowercased form, so
//     alpha < beta < rc holds, but so does beta < dev and beta < pre.
namespace VersionCompare {

struct ParsedVersion {
    int parts[4] = {0, 0, 0, 0};
    int partCount = 0;
    // Everything after the dotted numbers, e.g. "_beta1" / "-rc2" / "b3".
    // Empty for a final release.
    std::string suffix;
};

// Accepts an optional leading 'v'/'V', 1-4 dotted numeric components and an
// optional pre-release suffix. Returns false when there is no leading number.
bool Parse(const std::string& text, ParsedVersion* out);

// <0 when a<b, 0 when equal, >0 when a>b. Numeric components compare first
// (missing = 0, so "1.2" == "1.2.0"); for an equal tuple a final release
// outranks any pre-release, and two pre-releases order by their alphabetic tag
// then by their trailing number ("beta6" < "beta10", numerically). Unparsable
// strings rank below everything parsable (and equal to each other).
int Compare(const std::string& a, const std::string& b);

// True when BOTH strings parse and |candidate| is strictly newer than
// |current|. Requiring |current| to parse is load-bearing: it is always
// EFZ_TRAINING_MODE_VERSION, a hand-edited constant, and the upstream version
// of this function only guarded |candidate| - so a single malformed edit to
// version.h produced a permanent, unconditional "update available", even when
// the remote tag was OLDER.
bool IsNewer(const std::string& candidate, const std::string& current);

} // namespace VersionCompare
