#pragma once

// Pure 640x480 tutorial-HUD policy. Keeping the readability clamps and safe
// regions independent of ImGui makes them testable on every supported compiler
// and prevents a future visual tweak from quietly restoring 8px text or
// overlapping the bottom meters.
namespace Mission::TutorialLayoutPolicy {

struct Typography {
    float prosePx;
    float taskPx;
    float metaPx;
    float bannerPx;
    float pageTitlePx;
};

struct Rect {
    float x;
    float y;
    float w;
    float h;
};

constexpr float Clamp(float value, float low, float high) {
    return value < low ? low : (value > high ? high : value);
}

constexpr Typography TypographyFor(float uiScale) {
    return {
        Clamp(13.0f * uiScale, 12.0f, 16.0f),
        Clamp(12.0f * uiScale, 11.0f, 15.0f),
        Clamp(10.0f * uiScale, 9.0f, 12.0f),
        Clamp(14.0f * uiScale, 13.0f, 18.0f),
        Clamp(16.0f * uiScale, 14.0f, 20.0f),
    };
}

// Reading pages can deliberately leave the native top HUD visible. Begin
// below the round-pip group (through y=92) and meet the page card at y=124.
constexpr Rect PageBanner() { return {48.0f, 92.0f, 544.0f, 32.0f}; }
// Live play deliberately mirrors the existing Trial recipe band: a compact
// upper-left stack beginning below the native Life HUD. This is a bounding
// region, not a panel; every requirement strip uses only its measured width.
constexpr Rect ActiveRequirements() { return {8.0f, 108.0f, 372.0f, 252.0f}; }
// Lessons that explicitly ask the player to inspect the native P1 combo/Power
// readout use this second fixed anchor. It begins below that readout and never
// moves in response to whether the readout happens to be visible this frame.
constexpr Rect BelowStatsRequirements() { return {8.0f, 206.0f, 372.0f, 206.0f}; }
constexpr Rect RequirementBounds(bool belowStats) {
    return belowStats ? BelowStatsRequirements() : ActiveRequirements();
}
constexpr int MaxVisibleRequirements() { return 8; }
// Frozen choice/result/error screens still need their navigation hints. They
// are modal content, not part of the live gameplay HUD.
constexpr Rect ModalActions() { return {238.0f, 414.0f, 332.0f, 26.0f}; }
constexpr Rect PageCard() { return {48.0f, 124.0f, 544.0f, 236.0f}; }
constexpr Rect PageActions() { return {48.0f, 368.0f, 544.0f, 36.0f}; }

constexpr bool WithinCanvas(const Rect& rect) {
    return rect.x >= 0.0f && rect.y >= 0.0f &&
           rect.x + rect.w <= 640.0f && rect.y + rect.h <= 480.0f;
}

constexpr bool SafeBandsDoNotOverlap() {
    return ActiveRequirements().y >= 96.0f &&
           ActiveRequirements().y + ActiveRequirements().h <= 412.0f &&
           BelowStatsRequirements().y >= 206.0f &&
           BelowStatsRequirements().y + BelowStatsRequirements().h <= 412.0f &&
           PageBanner().y + PageBanner().h <= PageCard().y &&
           ModalActions().y + ModalActions().h <= 448.0f &&
           PageCard().y + PageCard().h < PageActions().y &&
           PageActions().y + PageActions().h <= 412.0f;
}

constexpr int RequirementWindowStart(int taskCount, int visibleCount,
                                     int currentIndex) {
    if (taskCount <= 0 || visibleCount <= 0 ||
        visibleCount >= taskCount) return 0;
    int start = currentIndex - visibleCount / 2;
    if (start < 0) start = 0;
    const int lastStart = taskCount - visibleCount;
    return start > lastStart ? lastStart : start;
}

} // namespace Mission::TutorialLayoutPolicy
