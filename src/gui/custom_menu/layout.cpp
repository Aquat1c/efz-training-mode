#include "../include/gui/custom_menu/layout.h"
#include "../include/gui/custom_menu/fonts.h"
#include "../include/gui/custom_menu/scale.h"
#include "../3rdparty/imgui/imgui.h"

#include <algorithm>
#include <cstring>

namespace CustomMenu::Layout {

namespace {

float PxFromFont(ImFont* font) {
    return (font && font->FontSize > 0.0f) ? font->FontSize : Scale::Get().bodyPx;
}

// Center a line of text vertically within a row of height h by choosing a top
// y such that text baseline visually centers.
float CenterTextY(float y, float h, float textPx) {
    return Scale::Snap(y + (h - textPx) * 0.5f);
}

void DrawRowDisabledOverlay(ImDrawList* dl, float x, float y, float w) {
    using namespace Theme;
    const Scale::Metrics& metrics = Scale::Get();
    dl->AddRectFilled(ImVec2(Scale::Snap(x - metrics.panelPadX), Scale::Snap(y)),
                      ImVec2(Scale::Snap(x + w + metrics.panelPadX), Scale::Snap(y + metrics.rowHeight)),
                      kDisabledFill);
}

void DrawMenuStrip(ImDrawList* dl, float x, float y, float w, float h, bool strong = false) {
    using namespace Theme;
    const Scale::Metrics& metrics = Scale::Get();
    const float sx = Scale::Snap(x - metrics.panelPadX);
    const float sy = Scale::Snap(y);
    const float sw = Scale::Snap(w + metrics.panelPadX * 2.0f);
    const float sh = Scale::Snap(h);
    dl->AddRectFilled(ImVec2(sx, sy), ImVec2(sx + sw, sy + sh), strong ? kStripStrong : kStrip);
    dl->AddLine(ImVec2(sx, sy), ImVec2(sx + sw, sy), kRule, 1.0f);
    dl->AddLine(ImVec2(sx, sy + sh - 1.0f), ImVec2(sx + sw, sy + sh - 1.0f), kRule, 1.0f);
}

void DrawOutlinedString(ImDrawList* dl, ImFont* font, float px, float x, float y,
                        ImU32 textCol, ImU32 outlineCol, const char* text) {
    if (!dl || !text || !*text) return;
    DrawString(dl, font, px, x - 1.0f, y,        outlineCol, text);
    DrawString(dl, font, px, x + 1.0f, y,        outlineCol, text);
    DrawString(dl, font, px, x,        y - 1.0f, outlineCol, text);
    DrawString(dl, font, px, x,        y + 1.0f, outlineCol, text);
    DrawString(dl, font, px, x + 1.0f, y + 1.0f, IM_COL32(0, 0, 0, 150), text);
    DrawString(dl, font, px, x,        y,        textCol, text);
}

// Draw the focused-row background tint + left-side cursor glyph (">"). This
// is the visual anchor that indicates which row is currently selected;
// matches EFZ's native menu convention.
void DrawRowChromeFocused(ImDrawList* dl, float x, float y, float w, bool disabled) {
    using namespace Theme;
    const Scale::Metrics& metrics = Scale::Get();
    const float sx = Scale::Snap(x - metrics.panelPadX);
    const float sy = Scale::Snap(y);
    const float sw = Scale::Snap(w + metrics.panelPadX * 2.0f);
    const ImU32 fill = disabled ? kDisabledFocus : kSelectedFill;
    const ImU32 cursorCol = disabled ? kCursorDisabled : kTextActive;
    dl->AddRectFilled(ImVec2(sx, sy + 2.0f), ImVec2(sx + sw, sy + metrics.rowHeight - 2.0f), fill);
    if (!disabled) {
        dl->AddLine(ImVec2(sx, sy + metrics.rowHeight - 3.0f), ImVec2(sx + sw, sy + metrics.rowHeight - 3.0f), kSelectedLine, 1.0f);
    }
    ImFont* f  = Fonts::Body();
    const float px = PxFromFont(f);
    const float textY = CenterTextY(sy, metrics.rowHeight, px);
    // Cursor sits inside the row padding so it doesn't crowd the label.
    if (dl && f) {
        dl->AddText(f, px, ImVec2(Scale::Snap(x + 5.0f), textY), cursorCol, ">");
    } else if (dl) {
        dl->AddText(ImVec2(Scale::Snap(x + 5.0f), textY), cursorCol, ">");
    }
    if (disabled) {
        dl->AddRect(
            ImVec2(sx + 0.5f, sy + 0.5f),
            ImVec2(sx + sw - 0.5f, sy + metrics.rowHeight - 0.5f),
            kRuleDim, 0.0f, 0, 1.0f);
    }
}

} // namespace

ImFont* BodyFont()   { return Fonts::Body(); }
ImFont* HeaderFont() { return Fonts::Header(); }

void DrawString(ImDrawList* dl, ImFont* font, float px, float x, float y, ImU32 col, const char* text) {
    if (!dl || !text || !*text) return;
    const float sx = Scale::Snap(x);
    const float sy = Scale::Snap(y);
    if (font) {
        const float sizePx = Scale::Snap((px > 0.0f) ? px : font->FontSize);
        dl->AddText(font, sizePx, ImVec2(sx, sy), col, text);
    } else {
        dl->AddText(ImVec2(sx, sy), col, text);
    }
}

float MeasureTextW(ImFont* font, float px, const char* text) {
    if (!text || !*text) return 0.0f;
    if (font) {
        const float sizePx = Scale::Snap((px > 0.0f) ? px : font->FontSize);
        ImVec2 sz = font->CalcTextSizeA(sizePx, FLT_MAX, 0.0f, text);
        return sz.x;
    }
    return ImGui::CalcTextSize(text).x;
}

// ===== Panel + title =====

ImVec2 DrawPanel(ImDrawList* dl, const char* title) {
    using namespace Theme;
    const Scale::Metrics& metrics = Scale::Get();

    // Full-screen backdrop
    dl->AddRectFilled(ImVec2(0, 0), ImVec2(kCanvasW, kCanvasH), kBackdrop);

    // Full-width translucent panel area
    const ImVec2 tl = PanelTopLeft();
    const ImVec2 br = PanelBottomRight();
    dl->AddRectFilled(tl, br, kPanel);

    ImFont* hFont = HeaderFont();
    const float hPx = PxFromFont(hFont);

    // Title strip: black band with hard white rails like EFZ option screens.
    DrawMenuStrip(dl, tl.x + metrics.panelPadX, tl.y, kPanelW - metrics.panelPadX * 2.0f, metrics.titleRowHeight, true);

    if (title && *title) {
        const float tw = MeasureTextW(hFont, hPx, title);
        const float tx = Scale::Snap(tl.x + (kPanelW - tw) * 0.5f);
        const float titleY = CenterTextY(tl.y, metrics.titleRowHeight, hPx);
        DrawString(dl, hFont, hPx, tx, titleY, kTextActive, title);
    }

    (void)br;
    return ImVec2(Scale::Snap(tl.x + metrics.panelPadX),
                  Scale::Snap(tl.y + metrics.titleRowHeight + metrics.panelPadY));
}

void DrawSectionRule(ImDrawList* dl, float panelX, float y, float panelW, ImU32 col) {
    using namespace Theme;
    const Scale::Metrics& metrics = Scale::Get();
    dl->AddLine(
        ImVec2(Scale::Snap(panelX + kRuleInsetX - metrics.panelPadX), Scale::Snap(y)),
        ImVec2(Scale::Snap(panelX + panelW - kRuleInsetX - metrics.panelPadX), Scale::Snap(y)),
        col, 1.0f);
}

// ===== Tab bar =====

void DrawTabBar(ImDrawList* dl, float x, float y, float w,
                const char* const* labels, int count, int activeIdx,
                int focusedIdx)
{
    using namespace Theme;
    if (!labels || count <= 0) return;
    const Scale::Metrics& metrics = Scale::Get();

    ImFont* bFont = BodyFont();
    const float bPx = PxFromFont(bFont);

    // Measure all widths
    float totalW = 0.0f;
    for (int i = 0; i < count; ++i) {
        totalW += MeasureTextW(bFont, bPx, labels[i]);
        if (i + 1 < count) totalW += metrics.tabGapX;
    }

    // Center inside (x, x+w)
    float cursorX = Scale::Snap(x + (w - totalW) * 0.5f);
    if (cursorX < x) cursorX = x;

    const float textY = CenterTextY(y, metrics.tabBarHeight, bPx);

    for (int i = 0; i < count; ++i) {
        const float tw = MeasureTextW(bFont, bPx, labels[i]);
        const bool isActive  = (i == activeIdx);
        const bool isFocused = (i == focusedIdx);
        const ImU32 col = isActive ? kTextActive : kTextInactive;
        const float boxPadX = Scale::Snap(7.0f * metrics.layoutScale);
        const float boxTop = Scale::Snap(textY - 4.0f);
        const float boxBottom = Scale::Snap(textY + bPx + 4.0f);

        if (isActive) {
            dl->AddRectFilled(ImVec2(cursorX - boxPadX, boxTop),
                              ImVec2(cursorX + tw + boxPadX, boxBottom),
                              IM_COL32(0, 0, 0, 118));
            dl->AddRect(ImVec2(cursorX - boxPadX + 0.5f, boxTop + 0.5f),
                        ImVec2(cursorX + tw + boxPadX - 0.5f, boxBottom - 0.5f),
                        isFocused ? kTextActive : kRuleDim, 0.0f, 0, 1.0f);
            const float underlineY = Scale::Snap(y + metrics.tabBarHeight - 5.0f);
            dl->AddLine(ImVec2(cursorX - 3.0f, underlineY),
                        ImVec2(cursorX + tw + 3.0f, underlineY),
                        kTextActive, 1.0f);
            dl->AddLine(ImVec2(cursorX - 3.0f, underlineY + 2.0f),
                        ImVec2(cursorX + tw + 3.0f, underlineY + 2.0f),
                        kSelectedLine, 1.0f);
        } else if (isFocused) {
            dl->AddRectFilled(ImVec2(cursorX - boxPadX, boxTop),
                              ImVec2(cursorX + tw + boxPadX, boxBottom),
                              IM_COL32(0, 0, 0, 96));
            dl->AddRect(ImVec2(cursorX - boxPadX + 0.5f, boxTop + 0.5f),
                        ImVec2(cursorX + tw + boxPadX - 0.5f, boxBottom - 0.5f),
                        kRuleDim, 0.0f, 0, 1.0f);
            const float underlineY = Scale::Snap(y + metrics.tabBarHeight - 6.0f);
            dl->AddLine(ImVec2(cursorX - 2.0f, underlineY),
                        ImVec2(cursorX + tw + 2.0f, underlineY),
                        kRuleDim, 1.0f);
        }

        DrawOutlinedString(dl, bFont, bPx, cursorX, textY, col, IM_COL32(0, 0, 0, 230), labels[i]);

        cursorX = Scale::Snap(cursorX + tw + metrics.tabGapX);
    }
}

// ===== Rows =====

void DrawHeader(ImDrawList* dl, float x, float y, float w, const char* text) {
    using namespace Theme;
    if (!text || !*text) return;
    const Scale::Metrics& metrics = Scale::Get();

    ImFont* bFont = BodyFont();
    const float bPx = PxFromFont(bFont);

    // Strip background + rules (unchanged from before).
    DrawMenuStrip(dl, x, y, w, metrics.rowHeight, true);

    // Left-edge accent: a 3px bright vertical bar that anchors the header
    // visually to the left edge of the panel. The strip already extends
    // past `x` by the panel padding on both sides, so painting at the strip
    // left sits flush with the panel border.
    const float stripLeft = Scale::Snap(x - metrics.panelPadX);
    dl->AddRectFilled(
        ImVec2(stripLeft,        Scale::Snap(y + 1.0f)),
        ImVec2(stripLeft + 3.0f, Scale::Snap(y + metrics.rowHeight - 1.0f)),
        kTextActive);

    // Text now starts just past the accent bar (a few extra pixels of
    // breathing room) instead of the standard row indent. Headers visually
    // hang off the left edge instead of floating in the middle.
    const float headerTextX = Scale::Snap(stripLeft + 8.0f);
    const float textY = CenterTextY(y, metrics.rowHeight, bPx);
    DrawString(dl, bFont, bPx, headerTextX, textY, kTextHeader, text);
}

void DrawRowSelectedBg(ImDrawList* dl, float x, float y, float w, float h) {
    dl->AddRectFilled(ImVec2(Scale::Snap(x), Scale::Snap(y)),
                      ImVec2(Scale::Snap(x + w), Scale::Snap(y + h)),
                      Theme::kSelectedFill);
}

void DrawRowLabelValue(
    ImDrawList* dl, float x, float y, float w,
    const char* label, const char* value,
    bool focused, bool disabled)
{
    using namespace Theme;
    const Scale::Metrics& metrics = Scale::Get();

    DrawMenuStrip(dl, x, y, w, metrics.rowHeight);
    if (disabled) DrawRowDisabledOverlay(dl, x, y, w);
    if (focused) DrawRowChromeFocused(dl, x, y, w, disabled);

    ImFont* bFont = BodyFont();
    const float bPx = PxFromFont(bFont);
    const float textY = CenterTextY(y, metrics.rowHeight, bPx);

    const ImU32 labelCol = disabled ? kTextDisabled : (focused ? kTextActive : kTextInactive);
    const ImU32 valueCol = disabled ? kTextDisabled : (focused ? kTextActive : kTextInactive);

    DrawString(dl, bFont, bPx, x + metrics.rowPadX, textY, labelCol, label);

    if (value && *value) {
        const float vw = MeasureTextW(bFont, bPx, value);
        DrawString(dl, bFont, bPx, x + w - metrics.rowPadX - vw, textY, valueCol, value);
    }
}

void DrawRowInlineChoices(
    ImDrawList* dl, float x, float y, float w,
    const char* label,
    const char* const* choices, int choiceCount, int currentIdx,
    bool focused, bool disabled)
{
    using namespace Theme;
    const Scale::Metrics& metrics = Scale::Get();

    DrawMenuStrip(dl, x, y, w, metrics.rowHeight);
    if (disabled) DrawRowDisabledOverlay(dl, x, y, w);
    if (focused) DrawRowChromeFocused(dl, x, y, w, disabled);

    ImFont* bFont = BodyFont();
    const float bPx = PxFromFont(bFont);
    const float textY = CenterTextY(y, metrics.rowHeight, bPx);

    const ImU32 labelCol = disabled ? kTextDisabled : (focused ? kTextActive : kTextInactive);
    DrawString(dl, bFont, bPx, x + metrics.rowPadX, textY, labelCol, label);

    if (!choices || choiceCount <= 0) return;

    // Measure total choices width
    const float kChoiceGapX = Scale::Snap(14.0f * metrics.layoutScale);
    float totalW = 0.0f;
    for (int i = 0; i < choiceCount; ++i) {
        totalW += MeasureTextW(bFont, bPx, choices[i]);
        if (i + 1 < choiceCount) totalW += kChoiceGapX;
    }

    float cursorX = Scale::Snap(x + w - metrics.rowPadX - totalW);
    for (int i = 0; i < choiceCount; ++i) {
        const float cw = MeasureTextW(bFont, bPx, choices[i]);
        const bool selected = (i == currentIdx);
        const ImU32 col = disabled ? kTextDisabled
                         : (selected ? kTextActive : kTextInactive);
        DrawString(dl, bFont, bPx, cursorX, textY, col, choices[i]);
        cursorX = Scale::Snap(cursorX + cw + kChoiceGapX);
    }
}

void DrawRowToggle(
    ImDrawList* dl, float x, float y, float w,
    const char* label, bool value,
    bool focused, bool disabled)
{
    const char* v = value ? "[ON]" : "[OFF]";
    DrawRowLabelValue(dl, x, y, w, label, v, focused, disabled);
}

void DrawRowNumber(
    ImDrawList* dl, float x, float y, float w,
    const char* label, const char* valueText,
    bool focused, bool disabled)
{
    using namespace Theme;
    const Scale::Metrics& metrics = Scale::Get();

    DrawMenuStrip(dl, x, y, w, metrics.rowHeight);
    if (disabled) DrawRowDisabledOverlay(dl, x, y, w);
    if (focused) DrawRowChromeFocused(dl, x, y, w, disabled);

    ImFont* bFont = BodyFont();
    const float bPx = PxFromFont(bFont);
    const float textY = CenterTextY(y, metrics.rowHeight, bPx);

    const ImU32 labelCol = disabled ? kTextDisabled : (focused ? kTextActive : kTextInactive);
    DrawString(dl, bFont, bPx, x + metrics.rowPadX, textY, labelCol, label);

    if (!valueText) valueText = "";
    const char* left  = focused ? "<" : " ";
    const char* right = focused ? ">" : " ";
    const ImU32 bracketCol = disabled ? kTextDisabled : kTextActive;
    const ImU32 valueCol   = disabled ? kTextDisabled : kTextActive;

    const float vw   = MeasureTextW(bFont, bPx, valueText);
    const float bw   = MeasureTextW(bFont, bPx, "<");
    constexpr float gap = 8.0f;
    const float totalW = bw + gap + vw + gap + bw;

    float cursorX = Scale::Snap(x + w - metrics.rowPadX - totalW);
    DrawString(dl, bFont, bPx, cursorX, textY, bracketCol, left);
    cursorX += bw + gap;
    DrawString(dl, bFont, bPx, cursorX, textY, valueCol, valueText);
    cursorX += vw + gap;
    DrawString(dl, bFont, bPx, cursorX, textY, bracketCol, right);
}

void DrawRowDrill(
    ImDrawList* dl, float x, float y, float w,
    const char* label, const char* valueText,
    bool focused, bool disabled)
{
    using namespace Theme;
    const Scale::Metrics& metrics = Scale::Get();

    DrawMenuStrip(dl, x, y, w, metrics.rowHeight);
    if (disabled) DrawRowDisabledOverlay(dl, x, y, w);
    if (focused) DrawRowChromeFocused(dl, x, y, w, disabled);

    ImFont* bFont = BodyFont();
    const float bPx = PxFromFont(bFont);
    const float textY = CenterTextY(y, metrics.rowHeight, bPx);

    const ImU32 labelCol = disabled ? kTextDisabled : (focused ? kTextActive : kTextInactive);
    const ImU32 valueCol = disabled ? kTextDisabled : (focused ? kTextActive : kTextInactive);
    const ImU32 arrowCol = disabled ? kTextDisabled : kTextActive;

    DrawString(dl, bFont, bPx, x + metrics.rowPadX, textY, labelCol, label);

    // Draw arrow as a simple ">" (the kDrillGlyph UTF-8 triangle requires the
    // font to include it - ITC Bolt may not, so use ASCII for safety).
    const char* arrow = ">";
    const float aw = MeasureTextW(bFont, bPx, arrow);

    float cursorX = Scale::Snap(x + w - metrics.rowPadX - aw);
    DrawString(dl, bFont, bPx, cursorX, textY, arrowCol, arrow);

    if (valueText && *valueText) {
        const float vw = MeasureTextW(bFont, bPx, valueText);
        constexpr float gap = 10.0f;
        cursorX -= (gap + vw);
        DrawString(dl, bFont, bPx, cursorX, textY, valueCol, valueText);
    }
}

void DrawRowDrillSegments(
    ImDrawList* dl, float x, float y, float w,
    const char* label,
    const TextSegment* valueSegments, int valueSegmentCount,
    bool focused, bool disabled)
{
    using namespace Theme;
    const Scale::Metrics& metrics = Scale::Get();

    DrawMenuStrip(dl, x, y, w, metrics.rowHeight);
    if (disabled) DrawRowDisabledOverlay(dl, x, y, w);
    if (focused) DrawRowChromeFocused(dl, x, y, w, disabled);

    ImFont* bFont = BodyFont();
    const float bPx = PxFromFont(bFont);
    const float textY = CenterTextY(y, metrics.rowHeight, bPx);

    const ImU32 labelCol = disabled ? kTextDisabled : (focused ? kTextActive : kTextInactive);
    const ImU32 arrowCol = disabled ? kTextDisabled : kTextActive;

    DrawString(dl, bFont, bPx, x + metrics.rowPadX, textY, labelCol, label);

    const char* arrow = ">";
    const float aw = MeasureTextW(bFont, bPx, arrow);

    float cursorX = Scale::Snap(x + w - metrics.rowPadX - aw);
    DrawString(dl, bFont, bPx, cursorX, textY, arrowCol, arrow);

    if (!valueSegments || valueSegmentCount <= 0) {
        return;
    }

    float valueW = 0.0f;
    for (int i = 0; i < valueSegmentCount; ++i) {
        if (!valueSegments[i].text || !*valueSegments[i].text) continue;
        valueW += MeasureTextW(bFont, bPx, valueSegments[i].text);
    }
    if (valueW <= 0.0f) {
        return;
    }

    constexpr float gap = 10.0f;
    cursorX = Scale::Snap(cursorX - (gap + valueW));
    for (int i = 0; i < valueSegmentCount; ++i) {
        const char* text = valueSegments[i].text;
        if (!text || !*text) continue;
        const ImU32 col = disabled ? kTextDisabled : valueSegments[i].color;
        DrawString(dl, bFont, bPx, cursorX, textY, col, text);
        cursorX += MeasureTextW(bFont, bPx, text);
    }
}

void DrawRowSlider(
    ImDrawList* dl, float x, float y, float w,
    const char* label, const char* valueText, float progress01,
    bool focused, bool disabled)
{
    using namespace Theme;
    const Scale::Metrics& metrics = Scale::Get();

    DrawMenuStrip(dl, x, y, w, metrics.rowHeight);
    if (disabled) DrawRowDisabledOverlay(dl, x, y, w);
    if (focused) DrawRowChromeFocused(dl, x, y, w, disabled);

    ImFont* bFont = BodyFont();
    const float bPx = PxFromFont(bFont);
    const float textY = CenterTextY(y, metrics.rowHeight, bPx);

    const ImU32 labelCol = disabled ? kTextDisabled : (focused ? kTextActive : kTextInactive);
    DrawString(dl, bFont, bPx, x + metrics.rowPadX, textY, labelCol, label);

    // Clamp
    if (progress01 < 0.0f) progress01 = 0.0f;
    if (progress01 > 1.0f) progress01 = 1.0f;

    // Slider track positioned between the label and the right-edge value text
    float valueW = 0.0f;
    if (valueText && *valueText) {
        valueW = MeasureTextW(bFont, bPx, valueText);
    }

    const float trackW = metrics.sliderTrackW;
    const float trackH = metrics.sliderTrackH;
    constexpr float gap = 10.0f;

    const float rightX = Scale::Snap(x + w - metrics.rowPadX);
    const float valueX = Scale::Snap(rightX - valueW);
    const float trackRight = Scale::Snap((valueW > 0.0f) ? (valueX - gap) : rightX);
    const float trackLeft  = Scale::Snap(trackRight - trackW);
    const float trackY = Scale::Snap(y + (metrics.rowHeight - trackH) * 0.5f);

    // Track off portion
    const ImU32 trackOffCol = disabled ? kTextDisabled : kSliderTrackOff;
    const ImU32 trackOnCol  = disabled ? kTextDisabled : kSliderTrackOn;

    dl->AddRectFilled(
        ImVec2(trackLeft, trackY),
        ImVec2(trackRight, trackY + trackH),
        trackOffCol);

    const float onRight = Scale::Snap(trackLeft + trackW * progress01);
    if (onRight > trackLeft) {
        dl->AddRectFilled(
            ImVec2(trackLeft, trackY),
            ImVec2(onRight, trackY + trackH),
            trackOnCol);
    }

    if (valueText && *valueText) {
        const ImU32 valueCol = disabled ? kTextDisabled : (focused ? kTextActive : kTextInactive);
        DrawString(dl, bFont, bPx, valueX, textY, valueCol, valueText);
    }
}

void DrawButton(
    ImDrawList* dl, float x, float y, float w, float h,
    const char* label, bool focused, bool disabled)
{
    using namespace Theme;
    x = Scale::Snap(x);
    y = Scale::Snap(y);
    w = Scale::Snap(w);
    h = Scale::Snap(h);

    ImU32 bg = focused ? (disabled ? kDisabledFocus : kButtonActiveBg) : IM_COL32(0,0,0,0);
    if (bg & 0xFF000000) {
        dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), bg);
    }
    if (disabled) {
        dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), kDisabledFill);
    }

    // 1px frame; bright when focused, dim otherwise
    const ImU32 frame = disabled ? kTextDisabled : (focused ? kTextActive : kRule);
    dl->AddRect(ImVec2(x + 0.5f, y + 0.5f), ImVec2(x + w - 0.5f, y + h - 0.5f), frame, 0.0f, 0, 1.0f);

    ImFont* bFont = BodyFont();
    const float bPx = PxFromFont(bFont);
    const float tw = MeasureTextW(bFont, bPx, label);
    const float tx = Scale::Snap(x + (w - tw) * 0.5f);
    const float ty = CenterTextY(y, h, bPx);
    const ImU32 textCol = disabled ? kTextDisabled : (focused ? kTextActive : kTextInactive);
    DrawString(dl, bFont, bPx, tx, ty, textCol, label);
}

// ===== Native (in-game) menu primitives =====

void DrawOutlinedText(ImDrawList* dl, ImFont* font, float px, float x, float y,
                      ImU32 col, const char* text) {
    if (!dl || !text || !*text) return;
    DrawOutlinedString(dl, font, px, x, y, col, Theme::kTextOutline, text);
}

void DrawNativeBar(ImDrawList* dl, float x, float y, float w, float h,
                   bool selected, bool disabled) {
    using namespace Theme;
    if (!dl || w <= 0.0f || h <= 0.0f) return;
    const float sx = Scale::Snap(x);
    const float sy = Scale::Snap(y);
    const float ex = Scale::Snap(x + w);
    const float ey = Scale::Snap(y + h);
    const float my = Scale::Snap(y + h * 0.42f);   // highlight sits above center
    const ImU32 top = disabled ? kBarDisTop : (selected ? kBarSelTop : kBarTop);
    const ImU32 mid = disabled ? kBarDisMid : (selected ? kBarSelMid : kBarMid);
    const ImU32 bot = disabled ? kBarDisBot : (selected ? kBarSelBot : kBarBot);
    dl->AddRectFilledMultiColor(ImVec2(sx, sy), ImVec2(ex, my), top, top, mid, mid);
    dl->AddRectFilledMultiColor(ImVec2(sx, my), ImVec2(ex, ey), mid, mid, bot, bot);
    dl->AddLine(ImVec2(sx, sy), ImVec2(ex, sy), kBarEdgeLight, 1.0f);
    dl->AddLine(ImVec2(sx, ey - 1.0f), ImVec2(ex, ey - 1.0f), kBarEdgeDark, 1.0f);
}

void DrawNativeBarCentered(ImDrawList* dl, float x, float y, float w, float h,
                           const char* label, bool selected, bool disabled) {
    using namespace Theme;
    DrawNativeBar(dl, x, y, w, h, selected, disabled);
    if (!label || !*label) return;
    ImFont* f = Fonts::Body();
    const float px = PxFromFont(f);
    const float tw = MeasureTextW(f, px, label);
    const ImU32 col = disabled ? kBarTextDis : (selected ? kBarTextSel : kBarText);
    DrawOutlinedText(dl, f, px, x + (w - tw) * 0.5f, CenterTextY(y, h, px), col, label);
}

float DrawTitleBand(ImDrawList* dl, const char* title, const char* rightStatus, float y) {
    using namespace Theme;
    const Scale::Metrics& metrics = Scale::Get();
    const float h = Scale::Snap(kBandH * metrics.layoutScale);
    const float y1 = y + h;
    dl->AddRectFilled(ImVec2(0.0f, Scale::Snap(y)), ImVec2(kCanvasW, Scale::Snap(y1)), kBandFill);
    dl->AddLine(ImVec2(0.0f, Scale::Snap(y1) - 1.0f), ImVec2(kCanvasW, Scale::Snap(y1) - 1.0f),
                kBoxBorder, 1.0f);
    ImFont* hf = Fonts::Header();
    const float hpx = (hf && hf->FontSize > 0.0f) ? hf->FontSize : metrics.headerPx;
    if (title && *title) {
        const float tw = MeasureTextW(hf, hpx, title);
        DrawOutlinedText(dl, hf, hpx, (kCanvasW - tw) * 0.5f, CenterTextY(y, h, hpx),
                         kTextActive, title);
    }
    if (rightStatus && *rightStatus) {
        ImFont* bf = Fonts::Body();
        const float bpx = PxFromFont(bf);
        const float sw = MeasureTextW(bf, bpx, rightStatus);
        DrawOutlinedText(dl, bf, bpx, kCanvasW - 14.0f - sw, CenterTextY(y, h, bpx),
                         kTextStatus, rightStatus);
    }
    return y1;
}

void DrawInfoBox(ImDrawList* dl, float x, float y, float w, float h) {
    using namespace Theme;
    const float sx = Scale::Snap(x);
    const float sy = Scale::Snap(y);
    const float ex = Scale::Snap(x + w);
    const float ey = Scale::Snap(y + h);
    dl->AddRectFilled(ImVec2(sx, sy), ImVec2(ex, ey), kBoxFill);
    dl->AddRect(ImVec2(sx + 0.5f, sy + 0.5f), ImVec2(ex - 0.5f, ey - 0.5f), kBoxBorder, 0.0f, 0, 1.0f);
}

} // namespace CustomMenu::Layout
