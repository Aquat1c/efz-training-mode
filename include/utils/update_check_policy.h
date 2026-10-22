#pragma once

#include <cctype>
#include <string>

#include "version_compare.h"

#include "nlohmann/json.hpp"

// Pure decision logic for the GitHub update check; total function of its arguments
// (no globals, I/O, clock or Win32). update_check.cpp keeps the WinHTTP round trip,
// worker thread, sidecar ini and atomics.
namespace UpdateCheck {
namespace Policy {

// Tags are attacker-influenced text that reaches a UI buffer; anything longer
// or outside this charset is rejected rather than rendered into the menu.
constexpr size_t kMaxTagLength = 32;

inline bool IsSafeTag(const std::string& tag) {
    if (tag.empty() || tag.size() > kMaxTagLength) return false;
    for (char c : tag) {
        const auto uc = static_cast<unsigned char>(c);
        if (!(std::isalnum(uc) || c == '.' || c == '_' || c == '-')) return false;
    }
    return true;
}

// Strips a leading v/V only when a digit follows ("v1.2" -> "1.2").
inline std::string NormalizeTag(std::string tag) {
    if (tag.size() >= 2 && (tag[0] == 'v' || tag[0] == 'V')
        && std::isdigit(static_cast<unsigned char>(tag[1]))) {
        tag.erase(0, 1);
    }
    return tag;
}

// Extracts and normalizes tag_name from a GitHub /releases/latest document; returns ""
// for anything malformed, non-object, missing, non-string or unsafe. A real JSON parse:
// the release object embeds the changelog in "body", so a substring search for
// "tag_name" could match release-note text.
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

// The badge decision. |acknowledged| is the tag the user last looked at; it suppresses the badge for that tag only.
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
