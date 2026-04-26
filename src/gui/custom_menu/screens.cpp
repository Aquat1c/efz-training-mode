#include "../include/gui/custom_menu/screens.h"
#include "../include/gui/custom_menu/layout.h"
#include "../include/gui/custom_menu/theme.h"
#include "../include/gui/custom_menu/input.h"
#include "../include/utils/config.h"
#include "../3rdparty/imgui/imgui.h"

#include <windows.h>
#include <cstdio>
#include <cstring>

namespace CustomMenu::Screens {

namespace {

bool RowIsFocusable(const Row& r) {
    switch (r.kind) {
        case RowKind::Header:
        case RowKind::Info:
        case RowKind::Spacer:
            return false;
        default:
            return true;
    }
}

bool RowHidden(const Row& r) {
    return r.isHidden && r.isHidden();
}

bool RowDisabled(const Row& r) {
    return r.isDisabled && r.isDisabled();
}

bool ShiftHeld() {
    return (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
}

float RowPixelHeight(const Row& r) {
    if (r.kind == RowKind::Spacer) return Theme::kRowHeight * 0.5f;
    if (r.kind == RowKind::Header) return Theme::kRowHeight + Theme::kSectionPadY;
    return Theme::kRowHeight;
}

void FormatIntValue(const Row& r, char* buf, size_t bufSz) {
    _snprintf_s(buf, bufSz, _TRUNCATE, "%d", r.intPtr ? *r.intPtr : 0);
}

void FormatFloatValue(const Row& r, char* buf, size_t bufSz) {
    const char* fmt = r.floatFmt ? r.floatFmt : "%.2f";
    _snprintf_s(buf, bufSz, _TRUNCATE, fmt, r.floatPtr ? *r.floatPtr : 0.0f);
}

void FormatDoubleValue(const Row& r, char* buf, size_t bufSz) {
    const char* fmt = r.doubleFmt ? r.doubleFmt : "%.1f";
    _snprintf_s(buf, bufSz, _TRUNCATE, fmt, r.doublePtr ? *r.doublePtr : 0.0);
}

// Compute the Y of each row once per frame so render + hit-test agree.
struct RowRect {
    float y = 0.0f;
    float h = 0.0f;
    bool  visible = false;
};

// Compute row rects. Returns total content height (unscrolled). With a
// scroll offset, visible rows are those whose rect intersects
// [contentTopY, contentBottomY].
float ComputeRects(const ScreenLayout& layout, const Row* rows, int rowCount,
                   float scrollPx, RowRect* out) {
    float y = layout.contentTopY - scrollPx;
    for (int i = 0; i < rowCount; ++i) {
        RowRect& r = out[i];
        r.y = y;
        r.h = RowPixelHeight(rows[i]);
        const bool hidden = RowHidden(rows[i]);
        r.visible = !hidden &&
                    (r.y + r.h) > layout.contentTopY &&
                    r.y < layout.contentBottomY;
        if (!hidden) y += r.h;
    }
    return y - (layout.contentTopY - scrollPx);
}

// Forward / backward scan to find next focusable (non-hidden, non-deco) row.
int FindFocusable(const Row* rows, int rowCount, int from, int dir) {
    if (from < 0) from = 0;
    if (from >= rowCount) from = rowCount - 1;
    for (int i = 0; i < rowCount; ++i) {
        int idx = from + dir * i;
        if (idx < 0) continue;
        if (idx >= rowCount) continue;
        if (RowHidden(rows[idx])) continue;
        if (RowIsFocusable(rows[idx])) return idx;
    }
    // If we didn't find in the preferred direction, search the other way.
    for (int i = 0; i < rowCount; ++i) {
        if (RowHidden(rows[i])) continue;
        if (RowIsFocusable(rows[i])) return i;
    }
    return -1;
}

} // namespace

// ===== Builders =====

Row Header(const char* label) {
    Row r{};
    r.kind = RowKind::Header;
    r.label = label;
    return r;
}

Row Info(const char* label) {
    Row r{};
    r.kind = RowKind::Info;
    r.label = label;
    return r;
}

Row Spacer() {
    Row r{};
    r.kind = RowKind::Spacer;
    r.label = "";
    return r;
}

Row Toggle(const char* label, bool* p,
           void (*onChange)(),
           bool (*isDisabled)(),
           bool (*isHidden)()) {
    Row r{};
    r.kind = RowKind::Toggle;
    r.label = label;
    r.boolPtr = p;
    r.onChange = onChange;
    r.isDisabled = isDisabled;
    r.isHidden = isHidden;
    return r;
}

Row IntNum(const char* label, int* p, int mn, int mx,
           int stepSmall, int stepBig,
           void (*onChange)(),
           bool (*isDisabled)(),
           bool (*isHidden)()) {
    Row r{};
    r.kind = RowKind::IntNumber;
    r.label = label;
    r.intPtr = p;
    r.intMin = mn;
    r.intMax = mx;
    r.intStepSmall = stepSmall;
    r.intStepBig = stepBig;
    r.onChange = onChange;
    r.isDisabled = isDisabled;
    r.isHidden = isHidden;
    return r;
}

Row FloatNum(const char* label, float* p, float mn, float mx,
             float stepSmall, float stepBig,
             const char* fmt,
             void (*onChange)(),
             bool (*isDisabled)(),
             bool (*isHidden)()) {
    Row r{};
    r.kind = RowKind::FloatNumber;
    r.label = label;
    r.floatPtr = p;
    r.floatMin = mn;
    r.floatMax = mx;
    r.floatStepSmall = stepSmall;
    r.floatStepBig = stepBig;
    r.floatFmt = fmt;
    r.onChange = onChange;
    r.isDisabled = isDisabled;
    r.isHidden = isHidden;
    return r;
}

Row DoubleNum(const char* label, double* p, double mn, double mx,
              double stepSmall, double stepBig,
              const char* fmt,
              void (*onChange)(),
              bool (*isDisabled)(),
              bool (*isHidden)()) {
    Row r{};
    r.kind = RowKind::DoubleNumber;
    r.label = label;
    r.doublePtr = p;
    r.doubleMin = mn;
    r.doubleMax = mx;
    r.doubleStepSmall = stepSmall;
    r.doubleStepBig = stepBig;
    r.doubleFmt = fmt;
    r.onChange = onChange;
    r.isDisabled = isDisabled;
    r.isHidden = isHidden;
    return r;
}

Row ChoicesRow(const char* label, int* idx, const char* const* items, int n,
               void (*onChange)(),
               bool (*isDisabled)(),
               bool (*isHidden)()) {
    Row r{};
    r.kind = RowKind::Choices;
    r.label = label;
    r.choiceIdxPtr = idx;
    r.choices = items;
    r.choiceCount = n;
    r.onChange = onChange;
    r.isDisabled = isDisabled;
    r.isHidden = isHidden;
    return r;
}

Row ActionStrengthRow(const char* label,
                      int* actionIdx, const char* const* actions, int actionCount,
                      int* strengthIdx, const char* const* strengths, int strengthCount,
                      RowValueFormatter formatter,
                      PairedChoiceChange onPrimaryChange,
                      PairedChoiceChange onSecondaryChange,
                      void (*onChange)(),
                      bool (*isDisabled)(),
                      bool (*isHidden)()) {
    Row r{};
    r.kind = RowKind::ActionStrength;
    r.label = label;
    r.choiceIdxPtr = actionIdx;
    r.choices = actions;
    r.choiceCount = actionCount;
    r.choice2IdxPtr = strengthIdx;
    r.choices2 = strengths;
    r.choice2Count = strengthCount;
    r.valueFormatter = formatter;
    r.onPrimaryChoiceChange = onPrimaryChange;
    r.onSecondaryChoiceChange = onSecondaryChange;
    r.onChange = onChange;
    r.isDisabled = isDisabled;
    r.isHidden = isHidden;
    return r;
}

Row DropdownRow(const char* label, int* idx, const char* const* items, int n,
                void (*onChange)(),
                bool (*isDisabled)(),
                bool (*isHidden)()) {
    Row r{};
    r.kind = RowKind::Dropdown;
    r.label = label;
    r.choiceIdxPtr = idx;
    r.choices = items;
    r.choiceCount = n;
    r.onChange = onChange;
    r.isDisabled = isDisabled;
    r.isHidden = isHidden;
    return r;
}

Row MaskPickerRow(const char* label, unsigned int* mask,
                  const char* const* items, int n,
                  void (*onChange)(),
                  bool (*isDisabled)(),
                  bool (*isHidden)()) {
    Row r{};
    r.kind = RowKind::MaskPicker;
    r.label = label;
    r.maskPtr = mask;
    r.choices = items;
    r.choiceCount = n;
    r.onChange = onChange;
    r.isDisabled = isDisabled;
    r.isHidden = isHidden;
    return r;
}

Row Action(const char* label, void (*fn)(),
           const char* (*valueFn)(),
           bool (*isDisabled)(),
           bool (*isHidden)()) {
    Row r{};
    r.kind = RowKind::Action;
    r.label = label;
    r.action = fn;
    r.actionValue = valueFn;
    r.isDisabled = isDisabled;
    r.isHidden = isHidden;
    return r;
}

// ===== Popup (modal dropdown) =====
// One singleton: only one dropdown can be open at a time.
struct PopupState {
    bool active = false;
    int* choiceIdxPtr = nullptr;             // single-select target
    unsigned int* maskPtr = nullptr;         // multi-select target
    int* companionIdxPtr = nullptr;          // paired-choice secondary target
    const char* const* choices = nullptr;
    int choiceCount = 0;
    int focusIdx = 0;
    float scrollPx = 0.0f;
    void (*onChange)() = nullptr;
    PairedChoiceChange onPrimaryChoiceChange = nullptr;
};
PopupState g_popup;

inline bool PopupIsMulti() { return g_popup.maskPtr != nullptr; }

void OpenDropdownPopup(const Row& r) {
    if (!r.choiceIdxPtr || r.choiceCount <= 0 || !r.choices) return;
    g_popup.active = true;
    g_popup.choiceIdxPtr = r.choiceIdxPtr;
    g_popup.maskPtr = nullptr;
    g_popup.companionIdxPtr = (r.kind == RowKind::ActionStrength) ? r.choice2IdxPtr : nullptr;
    g_popup.choices = r.choices;
    g_popup.choiceCount = r.choiceCount;
    g_popup.focusIdx = *r.choiceIdxPtr;
    g_popup.scrollPx = 0.0f;
    g_popup.onChange = r.onChange;
    g_popup.onPrimaryChoiceChange = r.onPrimaryChoiceChange;
}

void OpenMaskPopup(const Row& r) {
    if (!r.maskPtr || r.choiceCount <= 0 || !r.choices) return;
    g_popup.active = true;
    g_popup.choiceIdxPtr = nullptr;
    g_popup.maskPtr = r.maskPtr;
    g_popup.companionIdxPtr = nullptr;
    g_popup.choices = r.choices;
    g_popup.choiceCount = r.choiceCount;
    g_popup.focusIdx = 0;
    g_popup.scrollPx = 0.0f;
    g_popup.onChange = r.onChange;
    g_popup.onPrimaryChoiceChange = nullptr;
}

void ClosePopup() {
    g_popup.active = false;
    g_popup.choiceIdxPtr = nullptr;
    g_popup.maskPtr = nullptr;
    g_popup.companionIdxPtr = nullptr;
    g_popup.choices = nullptr;
    g_popup.choiceCount = 0;
    g_popup.focusIdx = 0;
    g_popup.scrollPx = 0.0f;
    g_popup.onChange = nullptr;
    g_popup.onPrimaryChoiceChange = nullptr;
}

bool PopupActive() { return g_popup.active; }

void PopupEnsureFocusVisible(float viewportH) {
    const float rowH = Theme::kRowHeight;
    const float desiredY = g_popup.focusIdx * rowH;
    if (desiredY < g_popup.scrollPx) {
        g_popup.scrollPx = desiredY;
    } else if (desiredY + rowH > g_popup.scrollPx + viewportH) {
        g_popup.scrollPx = desiredY + rowH - viewportH;
    }
    const float total = g_popup.choiceCount * rowH;
    const float maxScroll = (total > viewportH) ? (total - viewportH) : 0.0f;
    if (g_popup.scrollPx < 0.0f) g_popup.scrollPx = 0.0f;
    if (g_popup.scrollPx > maxScroll) g_popup.scrollPx = maxScroll;
}

struct PopupGeom {
    float px, py, popupW, popupH;
    float listX, listY, listW, listH;
    float rowH;
};

PopupGeom ComputePopupGeom(const ScreenLayout& layout) {
    using namespace Theme;
    PopupGeom g{};
    g.rowH   = kRowHeight;
    g.popupW = 320.0f;
    const float maxH = (layout.contentBottomY - layout.contentTopY) - 20.0f;
    const float desiredH = g_popup.choiceCount * g.rowH + 16.0f;
    g.popupH = (desiredH < maxH) ? desiredH : maxH;
    g.px = layout.panelX + (kPanelW - g.popupW) * 0.5f;
    g.py = layout.contentTopY + ((layout.contentBottomY - layout.contentTopY) - g.popupH) * 0.5f;
    g.listX = g.px + 8.0f;
    g.listY = g.py + 8.0f;
    g.listW = g.popupW - 16.0f;
    g.listH = g.popupH - 16.0f;
    return g;
}

// Input-only pass. Called inside HandleListInput when the popup is active so
// the popup sees edges BEFORE the list drains them.
void PopupTickInputOnly(const ScreenLayout& layout) {
    if (!g_popup.active) return;
    const PopupGeom g = ComputePopupGeom(layout);
    const bool isMulti = PopupIsMulti();

    auto toggleAt = [&](int i) {
        if (i < 0 || i >= g_popup.choiceCount) return;
        if (isMulti) {
            *g_popup.maskPtr ^= (1u << i);
        } else {
            *g_popup.choiceIdxPtr = i;
            if (g_popup.onPrimaryChoiceChange) {
                g_popup.onPrimaryChoiceChange(g_popup.choiceIdxPtr, g_popup.companionIdxPtr);
            }
        }
        if (g_popup.onChange) g_popup.onChange();
    };

    const bool navUp    = Input::NavUp();
    const bool navDown  = Input::NavDown();
    const bool activate = Input::Activate();
    const bool back     = Input::Back();
    if (navUp)   g_popup.focusIdx = (g_popup.focusIdx - 1 + g_popup.choiceCount) % g_popup.choiceCount;
    if (navDown) g_popup.focusIdx = (g_popup.focusIdx + 1) % g_popup.choiceCount;
    if (activate) {
        toggleAt(g_popup.focusIdx);
        if (!isMulti) { ClosePopup(); return; }
    }
    if (back) {
        ClosePopup();
        return;
    }

    const float wheel = ImGui::GetIO().MouseWheel;
    if (wheel != 0.0f) g_popup.scrollPx -= wheel * g.rowH * 3.0f;

    if (Input::MouseLeftEdge()) {
        for (int i = 0; i < g_popup.choiceCount; ++i) {
            const float ry = g.listY + i * g.rowH - g_popup.scrollPx;
            if (ry < g.listY) continue;
            if (ry + g.rowH > g.listY + g.listH) break;
            if (Input::MouseHovering(g.listX, ry, g.listW, g.rowH)) {
                toggleAt(i);
                if (!isMulti) { ClosePopup(); return; }
                break;
            }
        }
        // Click outside popup dismisses.
        if (!Input::MouseHovering(g.px, g.py, g.popupW, g.popupH)) {
            ClosePopup();
            return;
        }
    }
    for (int i = 0; i < g_popup.choiceCount; ++i) {
        const float ry = g.listY + i * g.rowH - g_popup.scrollPx;
        if (ry < g.listY) continue;
        if (ry + g.rowH > g.listY + g.listH) break;
        if (Input::MouseHovering(g.listX, ry, g.listW, g.rowH)) {
            g_popup.focusIdx = i;
            break;
        }
    }

    PopupEnsureFocusVisible(g.listH);
}

void PopupRender(ImDrawList* dl, const ScreenLayout& layout) {
    if (!g_popup.active || !dl) return;
    using namespace Theme;
    const PopupGeom g = ComputePopupGeom(layout);

    ImFont* bFont = Layout::BodyFont();
    const float bPx = bFont ? bFont->FontSize : 13.0f;

    // Backdrop + frame
    dl->AddRectFilled(ImVec2(layout.panelX - 2.0f, layout.contentTopY - 2.0f),
                      ImVec2(layout.panelX + kPanelW + 2.0f, layout.contentBottomY + 2.0f),
                      IM_COL32(0, 0, 0, 160));
    dl->AddRectFilled(ImVec2(g.px, g.py), ImVec2(g.px + g.popupW, g.py + g.popupH), kPanel);
    dl->AddRect      (ImVec2(g.px, g.py), ImVec2(g.px + g.popupW, g.py + g.popupH), kRule);

    const float titleY = g.py + 4.0f;
    const bool isMulti = PopupIsMulti();
    Layout::DrawString(dl, bFont, bPx, g.px + 10.0f, titleY, kTextHeader,
                       isMulti ? "MULTI-SELECT (ESC TO CLOSE)" : "SELECT");
    dl->AddLine(ImVec2(g.px + 8.0f, titleY + bPx + 4.0f),
                ImVec2(g.px + g.popupW - 8.0f, titleY + bPx + 4.0f), kRule, 1.0f);

    dl->PushClipRect(ImVec2(g.listX, g.listY),
                     ImVec2(g.listX + g.listW, g.listY + g.listH),
                     true);
    for (int i = 0; i < g_popup.choiceCount; ++i) {
        const float ry = g.listY + i * g.rowH - g_popup.scrollPx;
        if (ry + g.rowH <= g.listY) continue;
        if (ry >= g.listY + g.listH) break;
        const bool focused = (i == g_popup.focusIdx);
        if (focused) {
            dl->AddRectFilled(ImVec2(g.listX, ry),
                              ImVec2(g.listX + g.listW, ry + g.rowH),
                              kSelectedFill);
        }
        char line[128];
        if (isMulti) {
            const bool checked = ((*g_popup.maskPtr) >> i) & 1u;
            const char* mark = focused ? ">" : " ";
            _snprintf_s(line, sizeof(line), _TRUNCATE, "%s [%c] %s",
                        mark, checked ? 'X' : ' ', g_popup.choices[i]);
        } else {
            const char* mark = focused ? ">" : " ";
            _snprintf_s(line, sizeof(line), _TRUNCATE, "%s %s", mark, g_popup.choices[i]);
        }
        Layout::DrawString(dl, bFont, bPx,
                           g.listX + 4.0f,
                           ry + (g.rowH - bPx) * 0.5f,
                           focused ? kTextActive : kTextInactive, line);
    }
    dl->PopClipRect();

    // Scroll indicator
    const float total = g_popup.choiceCount * g.rowH;
    if (total > g.listH) {
        const float barX = g.px + g.popupW - 4.0f;
        const float frac = g_popup.scrollPx / (total - g.listH);
        const float barH = g.listH * (g.listH / total);
        const float barY = g.listY + (g.listH - barH) * frac;
        dl->AddRectFilled(ImVec2(barX, barY),
                          ImVec2(barX + 2.0f, barY + barH),
                          kRule);
    }
}

// ===== Public driver =====

void ClampFocus(const Row* rows, int rowCount, int& focus) {
    if (rowCount <= 0) { focus = -1; return; }
    // If current index is hidden or non-focusable, snap to nearest focusable.
    if (focus < 0 || focus >= rowCount ||
        RowHidden(rows[focus]) || !RowIsFocusable(rows[focus])) {
        focus = FindFocusable(rows, rowCount, focus >= 0 ? focus : 0, +1);
    }
}

void EnsureFocusVisible(const ScreenLayout& layout,
                        const Row* rows, int rowCount,
                        int focus, ScrollState& scroll) {
    if (focus < 0 || focus >= rowCount) return;
    RowRect rects[128];
    if (rowCount > 128) rowCount = 128;
    ComputeRects(layout, rows, rowCount, scroll.scrollPx, rects);

    const float viewTop = layout.contentTopY;
    const float viewBot = layout.contentBottomY;
    const float fy = rects[focus].y;
    const float fh = rects[focus].h;

    if (fy < viewTop) {
        scroll.scrollPx -= (viewTop - fy);
    } else if (fy + fh > viewBot) {
        scroll.scrollPx += (fy + fh) - viewBot;
    }

    // Compute max scroll so we can't scroll past the end.
    float totalH = 0.0f;
    for (int i = 0; i < rowCount; ++i) {
        if (RowHidden(rows[i])) continue;
        totalH += RowPixelHeight(rows[i]);
    }
    const float viewH = viewBot - viewTop;
    scroll.maxScrollPx = (totalH > viewH) ? (totalH - viewH) : 0.0f;
    if (scroll.scrollPx < 0.0f) scroll.scrollPx = 0.0f;
    if (scroll.scrollPx > scroll.maxScrollPx) scroll.scrollPx = scroll.maxScrollPx;
}

void RenderList(ImDrawList* dl, const ScreenLayout& layout,
                const char* title,
                const Row* rows, int rowCount,
                int focus,
                const ScrollState& scroll) {
    using namespace Theme;

    RowRect rects[128];
    if (rowCount > 128) rowCount = 128;
    ComputeRects(layout, rows, rowCount, scroll.scrollPx, rects);

    ImFont* bFont = Layout::BodyFont();
    const float bPx = bFont ? bFont->FontSize : 13.0f;

    // Screen title as a centered bright header over the content area.
    if (title && title[0]) {
        const float tw = Layout::MeasureTextW(bFont, bPx, title);
        float tx = layout.panelX + (kPanelW - tw) * 0.5f;
        if (tx < layout.contentX) tx = layout.contentX;
        Layout::DrawString(dl, bFont, bPx, tx,
                           layout.contentTopY - (kRowHeight + 2.0f),
                           kTextActive, title);
    }

    // Push a clip rect around the scrollable region so rows partially off the
    // top/bottom edge get correctly clipped instead of bleeding into header/hint.
    dl->PushClipRect(ImVec2(layout.panelX, layout.contentTopY),
                     ImVec2(layout.panelX + kPanelW, layout.contentBottomY),
                     true);

    for (int i = 0; i < rowCount; ++i) {
        if (!rects[i].visible) continue;
        const Row& r = rows[i];
        const bool focused = (i == focus);
        const bool disabled = RowDisabled(r);
        const float x = layout.contentX;
        const float y = rects[i].y;
        const float w = layout.contentW;

        switch (r.kind) {
            case RowKind::Header:
                Layout::DrawHeader(dl, x, y, w, r.label);
                break;
            case RowKind::Info: {
                dl->AddRectFilled(ImVec2(layout.contentX - kPanelPadX, y),
                                  ImVec2(layout.contentX + w + kPanelPadX, y + kRowHeight),
                                  kStrip);
                dl->AddLine(ImVec2(layout.contentX - kPanelPadX, y),
                            ImVec2(layout.contentX + w + kPanelPadX, y),
                            kRule, 1.0f);
                dl->AddLine(ImVec2(layout.contentX - kPanelPadX, y + kRowHeight - 1.0f),
                            ImVec2(layout.contentX + w + kPanelPadX, y + kRowHeight - 1.0f),
                            kRule, 1.0f);
                const float px = layout.contentX + kRuleInsetX;
                Layout::DrawString(dl, bFont, bPx, px,
                                   y + (kRowHeight - bPx) * 0.5f,
                                   kTextInactive, r.label);
                break;
            }
            case RowKind::Spacer:
                break;
            case RowKind::Toggle: {
                const bool v = r.boolPtr ? *r.boolPtr : false;
                Layout::DrawRowToggle(dl, x, y, w, r.label, v, focused, disabled);
                break;
            }
            case RowKind::IntNumber: {
                char buf[32];
                FormatIntValue(r, buf, sizeof(buf));
                Layout::DrawRowNumber(dl, x, y, w, r.label, buf, focused, disabled);
                break;
            }
            case RowKind::FloatNumber: {
                char buf[32];
                FormatFloatValue(r, buf, sizeof(buf));
                Layout::DrawRowNumber(dl, x, y, w, r.label, buf, focused, disabled);
                break;
            }
            case RowKind::DoubleNumber: {
                char buf[32];
                FormatDoubleValue(r, buf, sizeof(buf));
                Layout::DrawRowNumber(dl, x, y, w, r.label, buf, focused, disabled);
                break;
            }
            case RowKind::Choices: {
                const int idx = (r.choiceIdxPtr ? *r.choiceIdxPtr : 0);
                Layout::DrawRowInlineChoices(dl, x, y, w, r.label,
                                             r.choices, r.choiceCount, idx,
                                             focused, disabled);
                break;
            }
            case RowKind::ActionStrength: {
                static char s_buf[96];
                const char* val = r.valueFormatter ? r.valueFormatter(r) : nullptr;
                if (!val) {
                    const int idx = r.choiceIdxPtr ? *r.choiceIdxPtr : 0;
                    const int idx2 = r.choice2IdxPtr ? *r.choice2IdxPtr : 0;
                    const char* primary = (r.choices && idx >= 0 && idx < r.choiceCount) ? r.choices[idx] : "?";
                    const char* secondary = (r.choices2 && idx2 >= 0 && idx2 < r.choice2Count) ? r.choices2[idx2] : "?";
                    _snprintf_s(s_buf, sizeof(s_buf), _TRUNCATE, "%s / %s", primary, secondary);
                    val = s_buf;
                }
                Layout::DrawRowDrill(dl, x, y, w, r.label, val, focused, disabled);
                break;
            }
            case RowKind::Dropdown: {
                const int idx = (r.choiceIdxPtr ? *r.choiceIdxPtr : 0);
                const char* val = (r.choices && idx >= 0 && idx < r.choiceCount)
                                  ? r.choices[idx]
                                  : "?";
                Layout::DrawRowDrill(dl, x, y, w, r.label, val, focused, disabled);
                break;
            }
            case RowKind::MaskPicker: {
                int popcount = 0;
                if (r.maskPtr) {
                    unsigned int m = *r.maskPtr;
                    while (m) { popcount += (m & 1); m >>= 1; }
                }
                static char s_buf[24];
                _snprintf_s(s_buf, sizeof(s_buf), _TRUNCATE,
                            "%d / %d", popcount, r.choiceCount);
                Layout::DrawRowDrill(dl, x, y, w, r.label, s_buf, focused, disabled);
                break;
            }
            case RowKind::Action: {
                const char* val = r.actionValue ? r.actionValue() : nullptr;
                Layout::DrawRowDrill(dl, x, y, w, r.label, val, focused, disabled);
                break;
            }
        }
    }

    dl->PopClipRect();

    // Scroll indicator (right edge, inside panel pad area).
    if (scroll.maxScrollPx > 0.0f) {
        const float viewH = layout.contentBottomY - layout.contentTopY;
        const float totalH = viewH + scroll.maxScrollPx;
        const float barX = layout.panelX + kPanelW - 4.0f;
        const float frac = scroll.scrollPx / scroll.maxScrollPx;
        const float barH = viewH * (viewH / totalH);
        const float barY = layout.contentTopY + (viewH - barH) * frac;
        dl->AddRectFilled(ImVec2(barX, barY),
                          ImVec2(barX + 2.0f, barY + barH),
                          kRule);
    }
}

bool HandleListInput(const ScreenLayout& layout,
                     const Row* rows, int rowCount,
                     int& focus,
                     ScrollState& scroll) {
    if (IsKeybindActive()) {
        return false;
    }

    // If a dropdown popup is open, let it consume input first so we don't
    // accidentally drain edges before the popup sees them. Because this
    // function runs before the renderer's own TickPopupIfOpen, we dispatch
    // a silent input-only pass here (we still draw the popup after the
    // list so it sits above the rows).
    if (PopupActive()) {
        PopupTickInputOnly(layout);
        return false;
    }

    RowRect rects[128];
    if (rowCount > 128) rowCount = 128;
    ComputeRects(layout, rows, rowCount, scroll.scrollPx, rects);

    ClampFocus(rows, rowCount, focus);

    // Mouse wheel scrolls the list
    const float wheel = ImGui::GetIO().MouseWheel;
    if (wheel != 0.0f) {
        scroll.scrollPx -= wheel * Theme::kRowHeight * 3.0f;
        if (scroll.scrollPx < 0.0f) scroll.scrollPx = 0.0f;
        if (scroll.scrollPx > scroll.maxScrollPx) scroll.scrollPx = scroll.maxScrollPx;
    }

    // Mouse hover → focus (only consider visible, focusable rows)
    for (int i = 0; i < rowCount; ++i) {
        if (!rects[i].visible) continue;
        if (!RowIsFocusable(rows[i])) continue;
        if (Input::MouseHovering(layout.contentX, rects[i].y,
                                 layout.contentW, rects[i].h)) {
            focus = i;
            break;
        }
    }

    // Mouse click on a row = Activate that row
    bool clickActivated = false;
    if (Input::MouseLeftEdge()) {
        for (int i = 0; i < rowCount; ++i) {
            if (!rects[i].visible) continue;
            if (!RowIsFocusable(rows[i])) continue;
            if (Input::MouseHovering(layout.contentX, rects[i].y,
                                     layout.contentW, rects[i].h)) {
                focus = i;
                clickActivated = true;
                break;
            }
        }
    }

    const bool navUp    = Input::NavUp();
    const bool navDown  = Input::NavDown();
    const bool navLeft  = Input::NavLeft();
    const bool navRight = Input::NavRight();
    const bool activate = Input::Activate() || clickActivated;

    if (navUp)   focus = FindFocusable(rows, rowCount, focus - 1, -1);
    if (navDown) focus = FindFocusable(rows, rowCount, focus + 1, +1);

    if (focus >= 0 && focus < rowCount) {
        Row& r = const_cast<Row&>(rows[focus]);
        const bool disabled = RowDisabled(r);

        auto fire = [&](bool changed) {
            if (changed && r.onChange) r.onChange();
        };

        switch (r.kind) {
            case RowKind::Toggle: {
                if (disabled || !r.boolPtr) break;
                bool changed = false;
                if (activate)  { *r.boolPtr = !*r.boolPtr; changed = true; }
                else if (navLeft && *r.boolPtr)  { *r.boolPtr = false; changed = true; }
                else if (navRight && !*r.boolPtr){ *r.boolPtr = true;  changed = true; }
                fire(changed);
                break;
            }
            case RowKind::IntNumber: {
                if (disabled || !r.intPtr) break;
                bool changed = false;
                const int sBig = r.intStepBig > 0 ? r.intStepBig : r.intStepSmall;
                const int s = ShiftHeld() ? sBig : (r.intStepSmall > 0 ? r.intStepSmall : 1);
                if (navLeft)  { *r.intPtr -= s; changed = true; }
                if (navRight) { *r.intPtr += s; changed = true; }
                if (activate) { *r.intPtr += (s > 0 ? s : 1); changed = true; }
                if (*r.intPtr < r.intMin) *r.intPtr = r.intMin;
                if (*r.intPtr > r.intMax) *r.intPtr = r.intMax;
                fire(changed);
                break;
            }
            case RowKind::FloatNumber: {
                if (disabled || !r.floatPtr) break;
                bool changed = false;
                const float sBig = r.floatStepBig > 0.0f ? r.floatStepBig : r.floatStepSmall;
                const float s = ShiftHeld() ? sBig : r.floatStepSmall;
                if (navLeft)  { *r.floatPtr -= s; changed = true; }
                if (navRight) { *r.floatPtr += s; changed = true; }
                if (activate) { *r.floatPtr += s; changed = true; }
                if (*r.floatPtr < r.floatMin) *r.floatPtr = r.floatMin;
                if (*r.floatPtr > r.floatMax) *r.floatPtr = r.floatMax;
                fire(changed);
                break;
            }
            case RowKind::DoubleNumber: {
                if (disabled || !r.doublePtr) break;
                bool changed = false;
                const double sBig = r.doubleStepBig > 0.0 ? r.doubleStepBig : r.doubleStepSmall;
                const double s = ShiftHeld() ? sBig : r.doubleStepSmall;
                if (navLeft)  { *r.doublePtr -= s; changed = true; }
                if (navRight) { *r.doublePtr += s; changed = true; }
                if (activate) { *r.doublePtr += s; changed = true; }
                if (*r.doublePtr < r.doubleMin) *r.doublePtr = r.doubleMin;
                if (*r.doublePtr > r.doubleMax) *r.doublePtr = r.doubleMax;
                fire(changed);
                break;
            }
            case RowKind::Choices: {
                if (disabled || !r.choiceIdxPtr || r.choiceCount <= 0) break;
                int& idx = *r.choiceIdxPtr;
                bool changed = false;
                if (navLeft)              { idx = (idx - 1 + r.choiceCount) % r.choiceCount; changed = true; }
                else if (navRight || activate) { idx = (idx + 1) % r.choiceCount; changed = true; }
                fire(changed);
                break;
            }
            case RowKind::ActionStrength: {
                if (disabled) break;
                if (activate) {
                    OpenDropdownPopup(r);
                    break;
                }
                bool changed = false;
                if (ShiftHeld() && r.choice2IdxPtr && r.choice2Count > 0 && (navLeft || navRight)) {
                    int& idx = *r.choice2IdxPtr;
                    idx = navLeft
                        ? (idx - 1 + r.choice2Count) % r.choice2Count
                        : (idx + 1) % r.choice2Count;
                    if (r.onSecondaryChoiceChange) {
                        r.onSecondaryChoiceChange(r.choiceIdxPtr, r.choice2IdxPtr);
                    }
                    changed = true;
                } else if (r.choiceIdxPtr && r.choiceCount > 0 && (navLeft || navRight)) {
                    int& idx = *r.choiceIdxPtr;
                    idx = navLeft
                        ? (idx - 1 + r.choiceCount) % r.choiceCount
                        : (idx + 1) % r.choiceCount;
                    if (r.onPrimaryChoiceChange) {
                        r.onPrimaryChoiceChange(r.choiceIdxPtr, r.choice2IdxPtr);
                    }
                    changed = true;
                }
                fire(changed);
                break;
            }
            case RowKind::Dropdown: {
                if (disabled) break;
                if (activate) { OpenDropdownPopup(r); break; }
                // L/R still cycles inline for quick tweaks
                if (!r.choiceIdxPtr || r.choiceCount <= 0) break;
                int& idx = *r.choiceIdxPtr;
                bool changed = false;
                if (navLeft)  { idx = (idx - 1 + r.choiceCount) % r.choiceCount; changed = true; }
                if (navRight) { idx = (idx + 1) % r.choiceCount; changed = true; }
                fire(changed);
                break;
            }
            case RowKind::MaskPicker: {
                if (disabled) break;
                if (activate) { OpenMaskPopup(r); }
                break;
            }
            case RowKind::Action: {
                if (disabled) break;
                if (activate && r.action) r.action();
                break;
            }
            default:
                break;
        }
    }

    // Keep the focused row visible after any nav/click.
    EnsureFocusVisible(layout, rows, rowCount, focus, scroll);

    return Input::Back();
}

bool IsPopupActive() { return PopupActive(); }

bool TickPopupIfOpen(ImDrawList* dl, const ScreenLayout& layout) {
    if (!g_popup.active) return false;
    // Input has already been processed inside HandleListInput. This pass
    // only renders the popup so it sits on top of the list rows.
    PopupRender(dl, layout);
    return true;
}

// ===== Hotkey binding =====
namespace KeybindAPI { void TickInput(); }

struct KeybindState {
    bool active = false;
    char title[48] = "";              // shown to user (e.g. "OPEN MENU")
    int* settingsField = nullptr;     // mutable pointer into Config::Settings
    char iniSection[16] = "";         // INI section to persist into
    char iniKey[32]    = "";          // INI key to persist
    bool prevPressed[256] = {};
    bool primed = false;              // false on the first frame so a held key
                                      // (the Activate that opened binding) is
                                      // treated as already-down
};
KeybindState g_keybind;

bool IsKeybindActive() { return g_keybind.active; }

void OpenKeybind(const char* title, int* field,
                 const char* section, const char* key) {
    if (!field || !title || !section || !key) return;
    g_keybind.active = true;
    strncpy_s(g_keybind.title, sizeof(g_keybind.title), title, _TRUNCATE);
    g_keybind.settingsField = field;
    strncpy_s(g_keybind.iniSection, sizeof(g_keybind.iniSection), section, _TRUNCATE);
    strncpy_s(g_keybind.iniKey, sizeof(g_keybind.iniKey), key, _TRUNCATE);
    // Snapshot all keys as currently-pressed so the Activate edge that opened
    // this binding doesn't immediately register as a capture.
    for (int vk = 0; vk < 256; ++vk) {
        g_keybind.prevPressed[vk] = (GetAsyncKeyState(vk) & 0x8000) != 0;
    }
    g_keybind.primed = false;
}

void CloseKeybind() {
    g_keybind.active = false;
    g_keybind.settingsField = nullptr;
    g_keybind.iniSection[0] = '\0';
    g_keybind.iniKey[0] = '\0';
}

bool VkIsBindable(int vk) {
    // Disallow mouse buttons and pure modifiers — the user almost never wants
    // to bind those, and they'd interfere with menu navigation.
    if (vk >= 0x01 && vk <= 0x06) return false;     // mouse
    if (vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT) return false;
    if (vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL) return false;
    if (vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU) return false;
    if (vk == VK_LWIN || vk == VK_RWIN) return false;
    if (vk == VK_ESCAPE) return false;              // reserved for cancel
    return true;
}

namespace KeybindAPI {
    void TickInput() {
        if (!g_keybind.active) return;

        // Cancel
        const bool escNow = (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
        const bool escWas = g_keybind.prevPressed[VK_ESCAPE];
        g_keybind.prevPressed[VK_ESCAPE] = escNow;
        if (g_keybind.primed && escNow && !escWas) {
            CloseKeybind();
            Input::ResetEdges();
            return;
        }

        // Capture next bindable key edge
        bool anyHeld = false;
        int captured = 0;
        for (int vk = 0; vk < 256; ++vk) {
            if (vk == VK_ESCAPE) continue;
            const bool now = (GetAsyncKeyState(vk) & 0x8000) != 0;
            const bool was = g_keybind.prevPressed[vk];
            g_keybind.prevPressed[vk] = now;
            if (now) anyHeld = true;
            if (g_keybind.primed && now && !was && captured == 0 && VkIsBindable(vk)) {
                captured = vk;
            }
        }

        if (!g_keybind.primed) {
            // Only treat input as "real" once everything is released.
            if (!anyHeld) g_keybind.primed = true;
            return;
        }

        if (captured && g_keybind.settingsField) {
            *g_keybind.settingsField = captured;
            // Persist to INI as hex (Config::ParseKeyValue accepts hex/dec).
            char buf[16];
            _snprintf_s(buf, sizeof(buf), _TRUNCATE, "0x%X", captured);
            Config::SetSetting(g_keybind.iniSection, g_keybind.iniKey, buf);
            CloseKeybind();
            Input::ResetEdges();
        }
    }
} // namespace KeybindAPI

bool TickKeybindIfActive(ImDrawList* dl, const ScreenLayout& layout) {
    if (!g_keybind.active) return false;

    KeybindAPI::TickInput();
    if (!g_keybind.active) return true; // closed this frame

    using namespace Theme;

    // Centered modal box similar to popup geom but smaller.
    const float boxW = 320.0f;
    const float boxH = 120.0f;
    const float bx = layout.panelX + (kPanelW - boxW) * 0.5f;
    const float by = layout.contentTopY + ((layout.contentBottomY - layout.contentTopY) - boxH) * 0.5f;

    dl->AddRectFilled(ImVec2(layout.panelX - 2.0f, layout.contentTopY - 2.0f),
                      ImVec2(layout.panelX + kPanelW + 2.0f, layout.contentBottomY + 2.0f),
                      IM_COL32(0, 0, 0, 160));
    dl->AddRectFilled(ImVec2(bx, by), ImVec2(bx + boxW, by + boxH), kPanel);
    dl->AddRect      (ImVec2(bx, by), ImVec2(bx + boxW, by + boxH), kRule);

    ImFont* bFont = Layout::BodyFont();
    const float bPx = bFont ? bFont->FontSize : 13.0f;

    auto centerText = [&](float ty, const char* s, ImU32 col) {
        const float sw = Layout::MeasureTextW(bFont, bPx, s);
        Layout::DrawString(dl, bFont, bPx, bx + (boxW - sw) * 0.5f, ty, col, s);
    };

    centerText(by + 14.0f, g_keybind.title, kTextHeader);
    if (!g_keybind.primed) {
        centerText(by + 50.0f, "RELEASE ALL KEYS...", kTextInactive);
    } else {
        centerText(by + 50.0f, "PRESS A KEY", kTextActive);
    }
    centerText(by + 80.0f, "ESC TO CANCEL", kTextInactive);

    return true;
}

} // namespace CustomMenu::Screens
