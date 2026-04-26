#include "../include/gui/custom_menu/layout.h"
#include "../include/gui/custom_menu/fonts.h"
#include "../3rdparty/imgui/imgui.h"

#include <algorithm>
#include <cstring>

namespace CustomMenu::Layout {

namespace {

float PxFromFont(ImFont* font) {
    return (font && font->FontSize > 0.0f) ? font->FontSize : 13.0f;
}

// Center a line of text vertically within a row of height h by choosing a top
// y such that text baseline visually centers.
float CenterTextY(float y, float h, float textPx) {
    return y + (h - textPx) * 0.5f;
}

void DrawRowDisabledOverlay(ImDrawList* dl, float x, float y, float w) {
    using namespace Theme;
    dl->AddRectFilled(ImVec2(x - kPanelPadX, y), ImVec2(x + w + kPanelPadX, y + kRowHeight), kDisabledFill);
}

void DrawMenuStrip(ImDrawList* dl, float x, float y, float w, float h, bool strong = false) {
    using namespace Theme;
    const float sx = x - kPanelPadX;
    const float sw = w + kPanelPadX * 2.0f;
    dl->AddRectFilled(ImVec2(sx, y), ImVec2(sx + sw, y + h), strong ? kStripStrong : kStrip);
    dl->AddLine(ImVec2(sx, y), ImVec2(sx + sw, y), kRule, 1.0f);
    dl->AddLine(ImVec2(sx, y + h - 1.0f), ImVec2(sx + sw, y + h - 1.0f), kRule, 1.0f);
}

// Draw the focused-row background tint + left-side cursor glyph (">"). This
// is the visual anchor that indicates which row is currently selected;
// matches EFZ's native menu convention.
void DrawRowChromeFocused(ImDrawList* dl, float x, float y, float w, bool disabled) {
    using namespace Theme;
    const float sx = x - kPanelPadX;
    const float sw = w + kPanelPadX * 2.0f;
    const ImU32 fill = disabled ? kDisabledFocus : kSelectedFill;
    const ImU32 cursorCol = disabled ? kCursorDisabled : kTextActive;
    dl->AddRectFilled(ImVec2(sx, y + 2.0f), ImVec2(sx + sw, y + kRowHeight - 2.0f), fill);
    if (!disabled) {
        dl->AddLine(ImVec2(sx, y + kRowHeight - 3.0f), ImVec2(sx + sw, y + kRowHeight - 3.0f), kSelectedLine, 1.0f);
    }
    ImFont* f  = Fonts::Body();
    const float px = PxFromFont(f);
    const float textY = CenterTextY(y, kRowHeight, px);
    // Cursor sits inside kRowPadX (12px) so it doesn't collide with the label,
    // which starts at x + kRowPadX.
    if (dl && f) {
        dl->AddText(f, px, ImVec2(x + 3.0f, textY), cursorCol, ">");
    } else if (dl) {
        dl->AddText(ImVec2(x + 3.0f, textY), cursorCol, ">");
    }
    if (disabled) {
        dl->AddRect(
            ImVec2(sx + 0.5f, y + 0.5f),
            ImVec2(sx + sw - 0.5f, y + kRowHeight - 0.5f),
            kRuleDim, 0.0f, 0, 1.0f);
    }
}

} // namespace

ImFont* BodyFont()   { return Fonts::Body(); }
ImFont* HeaderFont() { return Fonts::Header(); }

void DrawString(ImDrawList* dl, ImFont* font, float px, float x, float y, ImU32 col, const char* text) {
    if (!dl || !text || !*text) return;
    if (font) {
        const float sizePx = (px > 0.0f) ? px : font->FontSize;
        dl->AddText(font, sizePx, ImVec2(x, y), col, text);
    } else {
        dl->AddText(ImVec2(x, y), col, text);
    }
}

float MeasureTextW(ImFont* font, float px, const char* text) {
    if (!text || !*text) return 0.0f;
    if (font) {
        const float sizePx = (px > 0.0f) ? px : font->FontSize;
        ImVec2 sz = font->CalcTextSizeA(sizePx, FLT_MAX, 0.0f, text);
        return sz.x;
    }
    return ImGui::CalcTextSize(text).x;
}

// ===== Panel + title =====

ImVec2 DrawPanel(ImDrawList* dl, const char* title) {
    using namespace Theme;

    // Full-screen backdrop
    dl->AddRectFilled(ImVec2(0, 0), ImVec2(kCanvasW, kCanvasH), kBackdrop);

    // Full-width translucent panel area
    const ImVec2 tl = PanelTopLeft();
    const ImVec2 br = PanelBottomRight();
    dl->AddRectFilled(tl, br, kPanel);

    ImFont* hFont = HeaderFont();
    const float hPx = PxFromFont(hFont);

    // Title strip: black band with hard white rails like EFZ option screens.
    DrawMenuStrip(dl, tl.x + kPanelPadX, tl.y, kPanelW - kPanelPadX * 2.0f, kTitleRowHeight, true);

    if (title && *title) {
        const float tw = MeasureTextW(hFont, hPx, title);
        const float tx = tl.x + (kPanelW - tw) * 0.5f;
        const float titleY = CenterTextY(tl.y, kTitleRowHeight, hPx);
        DrawString(dl, hFont, hPx, tx, titleY, kTextActive, title);
    }

    (void)br;
    return ImVec2(tl.x + kPanelPadX, tl.y + kTitleRowHeight + kPanelPadY);
}

void DrawSectionRule(ImDrawList* dl, float panelX, float y, float panelW, ImU32 col) {
    using namespace Theme;
    dl->AddLine(
        ImVec2(panelX + kRuleInsetX - kPanelPadX, y),
        ImVec2(panelX + panelW - kRuleInsetX - kPanelPadX, y),
        col, 1.0f);
}

// ===== Tab bar =====

void DrawTabBar(ImDrawList* dl, float x, float y, float w,
                const char* const* labels, int count, int activeIdx,
                int focusedIdx)
{
    using namespace Theme;
    if (!labels || count <= 0) return;

    ImFont* bFont = BodyFont();
    const float bPx = PxFromFont(bFont);

    DrawMenuStrip(dl, x + kPanelPadX, y, w - kPanelPadX * 2.0f, kTabBarHeight, true);

    // Measure all widths
    float totalW = 0.0f;
    for (int i = 0; i < count; ++i) {
        totalW += MeasureTextW(bFont, bPx, labels[i]);
        if (i + 1 < count) totalW += kTabGapX;
    }

    // Center inside (x, x+w)
    float cursorX = x + (w - totalW) * 0.5f;
    if (cursorX < x) cursorX = x;

    const float textY = CenterTextY(y, kTabBarHeight, bPx);

    for (int i = 0; i < count; ++i) {
        const float tw = MeasureTextW(bFont, bPx, labels[i]);
        const bool isActive  = (i == activeIdx);
        const bool isFocused = (i == focusedIdx);
        const ImU32 col = isActive ? kTextActive : kTextInactive;

        if (isActive) {
            dl->AddRectFilled(
                ImVec2(cursorX - 5.0f, y + 3.0f),
                ImVec2(cursorX + tw + 5.0f, y + kTabBarHeight - 4.0f),
                kSelectedFill);
        } else if (isFocused) {
            const float pad = 2.0f;
            dl->AddRect(
                ImVec2(cursorX - pad, textY - pad),
                ImVec2(cursorX + tw + pad, textY + bPx + pad),
                kRuleDim, 0.0f, 0, 1.0f);
        }

        DrawString(dl, bFont, bPx, cursorX, textY, col, labels[i]);

        cursorX += tw + kTabGapX;
    }
}

// ===== Rows =====

void DrawHeader(ImDrawList* dl, float x, float y, float w, const char* text) {
    using namespace Theme;
    if (!text || !*text) return;

    ImFont* bFont = BodyFont();
    const float bPx = PxFromFont(bFont);

    DrawMenuStrip(dl, x, y, w, kRowHeight, true);

    const float textY = CenterTextY(y, kRowHeight, bPx);
    DrawString(dl, bFont, bPx, x + kRowPadX, textY, kTextHeader, text);
}

void DrawRowSelectedBg(ImDrawList* dl, float x, float y, float w, float h) {
    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), Theme::kSelectedFill);
}

void DrawRowLabelValue(
    ImDrawList* dl, float x, float y, float w,
    const char* label, const char* value,
    bool focused, bool disabled)
{
    using namespace Theme;

    DrawMenuStrip(dl, x, y, w, kRowHeight);
    if (disabled) DrawRowDisabledOverlay(dl, x, y, w);
    if (focused) DrawRowChromeFocused(dl, x, y, w, disabled);

    ImFont* bFont = BodyFont();
    const float bPx = PxFromFont(bFont);
    const float textY = CenterTextY(y, kRowHeight, bPx);

    const ImU32 labelCol = disabled ? kTextDisabled : (focused ? kTextActive : kTextInactive);
    const ImU32 valueCol = disabled ? kTextDisabled : (focused ? kTextActive : kTextInactive);

    DrawString(dl, bFont, bPx, x + kRowPadX, textY, labelCol, label);

    if (value && *value) {
        const float vw = MeasureTextW(bFont, bPx, value);
        DrawString(dl, bFont, bPx, x + w - kRowPadX - vw, textY, valueCol, value);
    }
}

void DrawRowInlineChoices(
    ImDrawList* dl, float x, float y, float w,
    const char* label,
    const char* const* choices, int choiceCount, int currentIdx,
    bool focused, bool disabled)
{
    using namespace Theme;

    DrawMenuStrip(dl, x, y, w, kRowHeight);
    if (disabled) DrawRowDisabledOverlay(dl, x, y, w);
    if (focused) DrawRowChromeFocused(dl, x, y, w, disabled);

    ImFont* bFont = BodyFont();
    const float bPx = PxFromFont(bFont);
    const float textY = CenterTextY(y, kRowHeight, bPx);

    const ImU32 labelCol = disabled ? kTextDisabled : (focused ? kTextActive : kTextInactive);
    DrawString(dl, bFont, bPx, x + kRowPadX, textY, labelCol, label);

    if (!choices || choiceCount <= 0) return;

    // Measure total choices width
    constexpr float kChoiceGapX = 14.0f;
    float totalW = 0.0f;
    for (int i = 0; i < choiceCount; ++i) {
        totalW += MeasureTextW(bFont, bPx, choices[i]);
        if (i + 1 < choiceCount) totalW += kChoiceGapX;
    }

    float cursorX = x + w - kRowPadX - totalW;
    for (int i = 0; i < choiceCount; ++i) {
        const float cw = MeasureTextW(bFont, bPx, choices[i]);
        const bool selected = (i == currentIdx);
        const ImU32 col = disabled ? kTextDisabled
                         : (selected ? kTextActive : kTextInactive);
        DrawString(dl, bFont, bPx, cursorX, textY, col, choices[i]);
        cursorX += cw + kChoiceGapX;
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

    DrawMenuStrip(dl, x, y, w, kRowHeight);
    if (disabled) DrawRowDisabledOverlay(dl, x, y, w);
    if (focused) DrawRowChromeFocused(dl, x, y, w, disabled);

    ImFont* bFont = BodyFont();
    const float bPx = PxFromFont(bFont);
    const float textY = CenterTextY(y, kRowHeight, bPx);

    const ImU32 labelCol = disabled ? kTextDisabled : (focused ? kTextActive : kTextInactive);
    DrawString(dl, bFont, bPx, x + kRowPadX, textY, labelCol, label);

    if (!valueText) valueText = "";
    const char* left  = focused ? "<" : " ";
    const char* right = focused ? ">" : " ";
    const ImU32 bracketCol = disabled ? kTextDisabled : kTextActive;
    const ImU32 valueCol   = disabled ? kTextDisabled : kTextActive;

    const float vw   = MeasureTextW(bFont, bPx, valueText);
    const float bw   = MeasureTextW(bFont, bPx, "<");
    constexpr float gap = 8.0f;
    const float totalW = bw + gap + vw + gap + bw;

    float cursorX = x + w - kRowPadX - totalW;
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

    DrawMenuStrip(dl, x, y, w, kRowHeight);
    if (disabled) DrawRowDisabledOverlay(dl, x, y, w);
    if (focused) DrawRowChromeFocused(dl, x, y, w, disabled);

    ImFont* bFont = BodyFont();
    const float bPx = PxFromFont(bFont);
    const float textY = CenterTextY(y, kRowHeight, bPx);

    const ImU32 labelCol = disabled ? kTextDisabled : (focused ? kTextActive : kTextInactive);
    const ImU32 valueCol = disabled ? kTextDisabled : (focused ? kTextActive : kTextInactive);
    const ImU32 arrowCol = disabled ? kTextDisabled : kTextActive;

    DrawString(dl, bFont, bPx, x + kRowPadX, textY, labelCol, label);

    // Draw arrow as a simple ">" (the kDrillGlyph UTF-8 triangle requires the
    // font to include it — ITC Bolt may not, so use ASCII for safety).
    const char* arrow = ">";
    const float aw = MeasureTextW(bFont, bPx, arrow);

    float cursorX = x + w - kRowPadX - aw;
    DrawString(dl, bFont, bPx, cursorX, textY, arrowCol, arrow);

    if (valueText && *valueText) {
        const float vw = MeasureTextW(bFont, bPx, valueText);
        constexpr float gap = 10.0f;
        cursorX -= (gap + vw);
        DrawString(dl, bFont, bPx, cursorX, textY, valueCol, valueText);
    }
}

void DrawRowSlider(
    ImDrawList* dl, float x, float y, float w,
    const char* label, const char* valueText, float progress01,
    bool focused, bool disabled)
{
    using namespace Theme;

    DrawMenuStrip(dl, x, y, w, kRowHeight);
    if (disabled) DrawRowDisabledOverlay(dl, x, y, w);
    if (focused) DrawRowChromeFocused(dl, x, y, w, disabled);

    ImFont* bFont = BodyFont();
    const float bPx = PxFromFont(bFont);
    const float textY = CenterTextY(y, kRowHeight, bPx);

    const ImU32 labelCol = disabled ? kTextDisabled : (focused ? kTextActive : kTextInactive);
    DrawString(dl, bFont, bPx, x + kRowPadX, textY, labelCol, label);

    // Clamp
    if (progress01 < 0.0f) progress01 = 0.0f;
    if (progress01 > 1.0f) progress01 = 1.0f;

    // Slider track positioned between the label and the right-edge value text
    float valueW = 0.0f;
    if (valueText && *valueText) {
        valueW = MeasureTextW(bFont, bPx, valueText);
    }

    const float trackW = kSliderTrackW;
    const float trackH = kSliderTrackH;
    constexpr float gap = 10.0f;

    const float rightX = x + w - kRowPadX;
    const float valueX = rightX - valueW;
    const float trackRight = (valueW > 0.0f) ? (valueX - gap) : rightX;
    const float trackLeft  = trackRight - trackW;
    const float trackY = y + (kRowHeight - trackH) * 0.5f;

    // Track off portion
    const ImU32 trackOffCol = disabled ? kTextDisabled : kSliderTrackOff;
    const ImU32 trackOnCol  = disabled ? kTextDisabled : kSliderTrackOn;

    dl->AddRectFilled(
        ImVec2(trackLeft, trackY),
        ImVec2(trackRight, trackY + trackH),
        trackOffCol);

    const float onRight = trackLeft + trackW * progress01;
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
    const float tx = x + (w - tw) * 0.5f;
    const float ty = CenterTextY(y, h, bPx);
    const ImU32 textCol = disabled ? kTextDisabled : (focused ? kTextActive : kTextInactive);
    DrawString(dl, bFont, bPx, tx, ty, textCol, label);
}

} // namespace CustomMenu::Layout
