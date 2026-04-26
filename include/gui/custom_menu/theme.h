#pragma once

#include "../3rdparty/imgui/imgui.h"

namespace CustomMenu::Theme {

// ===== Canvas =====
// Virtual design resolution — all coordinates are in this space.
constexpr float kCanvasW = 640.0f;
constexpr float kCanvasH = 480.0f;

// ===== Colors =====
// EFZ-native menus use hard black strips and white separators.  These colors
// keep that silhouette while staying translucent enough for an overlay.
constexpr ImU32 kBackdrop        = IM_COL32(0,   0,   0,    55);
constexpr ImU32 kPanel           = IM_COL32(0,   0,   0,    55);
constexpr ImU32 kStrip           = IM_COL32(0,   0,   0,   205);
constexpr ImU32 kStripStrong     = IM_COL32(0,   0,   0,   225);
constexpr ImU32 kTextActive      = IM_COL32(255, 255, 255, 255);
constexpr ImU32 kTextInactive    = IM_COL32(150, 150, 150, 235);
constexpr ImU32 kTextHeader      = IM_COL32(235, 235, 235, 255);
constexpr ImU32 kTextDisabled    = IM_COL32( 96,  96,  96, 255);
constexpr ImU32 kTextStatus      = IM_COL32(170, 170, 170, 255);
constexpr ImU32 kSelectedFill    = IM_COL32( 30, 155, 190,  105);
constexpr ImU32 kSelectedLine    = IM_COL32(105, 235, 235,  190);
constexpr ImU32 kDisabledFill    = IM_COL32(0,   0,   0,   120);
constexpr ImU32 kDisabledFocus   = IM_COL32(255, 255, 255,   18);
constexpr ImU32 kCursorDisabled  = IM_COL32(120, 120, 120, 255);
constexpr ImU32 kRule            = IM_COL32(255, 255, 255,  230);
constexpr ImU32 kRuleDim         = IM_COL32(255, 255, 255,  145);
constexpr ImU32 kSliderTrackOn   = IM_COL32(255, 255, 255, 220);
constexpr ImU32 kSliderTrackOff  = IM_COL32(150, 150, 150, 120);
constexpr ImU32 kButtonActiveBg  = IM_COL32(255, 255, 255,  40);
constexpr ImU32 kButtonHoverBg   = IM_COL32(255, 255, 255,  20);

// ===== Panel metrics =====
// Main menu panel — full-width, like EFZ's native option/replay screens.
constexpr float kPanelW          = 640.0f;
constexpr float kPanelH          = 480.0f;
constexpr float kPanelPadX       = 28.0f;
constexpr float kPanelPadY       = 6.0f;

// ===== Row metrics =====
// Row strips stretch the full panel width (minus panel padding).
constexpr float kRowHeight       = 22.0f;
constexpr float kRowPadX         = 7.0f;       // inner horizontal padding
constexpr float kSectionPadY     = 4.0f;       // extra gap above a section header
constexpr float kRuleInsetX      = 0.0f;       // EFZ rows/separators span the full strip

// Title metrics
constexpr float kTitleRowHeight  = 30.0f;
constexpr float kTitleRuleGapY   = 0.0f;

// Tab bar
constexpr float kTabBarHeight    = 28.0f;
constexpr float kTabGapX         = 18.0f;      // horizontal gap between tab labels

// Slider
constexpr float kSliderTrackH    = 4.0f;
constexpr float kSliderTrackW    = 160.0f;

// Drill arrow
constexpr const char* kDrillGlyph = "\xE2\x96\xB6";  // ▶ (U+25B6) in UTF-8

// ===== Helpers =====
inline ImVec2 PanelTopLeft() {
    return ImVec2(0.0f, 0.0f);
}

inline ImVec2 PanelBottomRight() {
    ImVec2 tl = PanelTopLeft();
    return ImVec2(tl.x + kPanelW, tl.y + kPanelH);
}

} // namespace CustomMenu::Theme
