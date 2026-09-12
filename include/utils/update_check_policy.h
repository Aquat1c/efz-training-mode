#pragma once

#include <cctype>
#include <string>

#include "version_compare.h"

#include "nlohmann/json.hpp"

// Pure decision logic for the GitHub update check, split out from
// update_check.cpp so it can be unit tested without a network, a game process,
// or a published release. Everything here is a total function of its arguments:
// no globals, no I/O, no clock, no Win32.
//
// update_check.cpp keeps only the parts that genuinely cannot be pure - the
// WinHTTP round trip, the worker thread, the sidecar ini, and the atomics.
namespace UpdateCheck {
namespace Policy {

// A release tag is "1.2.2" / "1.2.0_beta6" / "v1.0". Anything longer or outside
// this charset is rejected rather than rendered into the menu, because the tag
// is attacker-influenced text that reaches a UI buffer.
constexpr size_t kMaxTagLength = 32;

inline bool IsSafeTag(const std::string& tag) {
    if (tag.empty() || tag.size() > kMaxTagLength) return false;
    for (char c : tag) {
        const auto uc = static_cast<unsigned char>(c);
        if (!(std::isalnum(uc) || c == '.' || c == '_' || c == '-')) return false;
    }
    return true;
}

// Strips a leading v/V only when a digit follows, so "v1.2" -> "1.2" but a tag
// that genuinely starts with a letter is left alone (and then fails IsSafeTag's
// caller, since VersionCompare will not parse it).
inline std::string NormalizeTag(std::string tag) {
    if (tag.size() >= 2 && (tag[0] == 'v' || tag[0] == 'V')
        && std::isdigit(static_cast<unsigned char>(tag[1]))) {
        tag.erase(0, 1);
    }
    return tag;
}

// Extracts and normalizes tag_name from a GitHub /releases/latest document.
// Returns "" for anything malformed, non-object, missing, non-string, or unsafe.
//
// Deliberately a real JSON parse rather than a substring search: a GitHub
// release object embeds the full markdown changelog in "body", and a naive
// search for the first "tag_name" would happily match text a release author
// wrote inside their own release notes.
inline std::string ExtractTagFromReleaseJson(const std::string& body) {
    auto parsed = nlohmann::json::parse(body, nullptr, /*allow_exceptions=*/false);
    if (parsed.is_discarded() || !parsed.is_object()) return {};
    auto it = parsed.find("tag_name");
    if (it == parsed.end() || !it->is_string()) return {};
    const std::string tag = NormalizeTag(it->get<std::string>());
    return IsSafeTag(tag) ? tag : std::string{};
}

struct Availability {
    bool hasNewerRelease = false;   // latest > current, regardless of acknowledgement
    bool updateAvailable = false;   // hasNewerRelease && the user has not seen this tag
};

// The badge decision. |acknowledged| is the tag the user last looked at in
// HELP > ABOUT; it suppresses the badge for that tag only, so a subsequent
// release re-raises it.
inline Availability Decide(const std::string& latest,
                           const std::string& current,
                           const std::string& acknowledged) {
    Availability out;
    if (latest.empty()) return out;
    out.hasNewerRelease = VersionCompare::IsNewer(latest, current);
    out.updateAvailable = out.hasNewerRelease && latest != acknowledged;
    return out;
}

} // namespace Policy
} // namespace UpdateCheck
