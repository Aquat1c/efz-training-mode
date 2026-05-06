#include "../include/gui/custom_menu/screens.h"
#include "../include/gui/custom_menu/layout.h"
#include "../include/gui/custom_menu/theme.h"
#include "../include/gui/custom_menu/input.h"
#include "../include/gui/custom_menu/sound.h"
#include "../include/utils/config.h"
#include "../include/utils/xinput_shim.h"
#include "../3rdparty/imgui/imgui.h"

#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>

namespace CustomMenu::Screens {

namespace {

// Info rows are focusable so keyboard nav can scroll through paragraph text
// (Help pages especially). Activate is a no-op on Info (the switch in
// HandleRowsInput falls through to `default: break;`). Headers and Spacers
// remain non-focusable so the cursor doesn't park on a section divider.
bool RowIsFocusable(const Row& r) {
    switch (r.kind) {
        case RowKind::Header:
        case RowKind::Spacer:
            return false;
        default:
            return true;
    }
}

// Whether a focused row should receive any cursor/highlight visual treatment
// at all. Info rows are focusable for navigation but we keep their look
// understated so the rest of the page doesn't look cluttered.
bool RowDrawsFocusChrome(const Row& r) {
    return r.kind != RowKind::Info;
}

bool RowHidden(const Row& r) {
    return r.isHidden && r.isHidden();
}

bool RowDisabled(const Row& r) {
    return r.isDisabled && r.isDisabled();
}

bool ShiftHeld() {
    if (!Input::IsGameWindowActive()) return false;
    return (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
}

constexpr float kInfoPadX = 8.0f;
constexpr float kInfoTextX = 18.0f;
constexpr float kInfoPadY = 1.0f;
constexpr float kInfoLineGap = 1.0f;

void PushWrappedLine(std::vector<std::string>& out, const std::string& line) {
    if (!line.empty()) {
        out.push_back(line);
    }
}

void WrapTextLine(ImFont* font, float px, const char* text, float maxW,
                  std::vector<std::string>& out) {
    if (!text || !*text) return;
    if (maxW <= 12.0f) {
        out.push_back(text);
        return;
    }

    std::string current;
    std::string word;

    auto flushWord = [&]() {
        if (word.empty()) return;
        if (current.empty()) {
            current = word;
        } else {
            std::string candidate = current;
            candidate.push_back(' ');
            candidate += word;
            if (Layout::MeasureTextW(font, px, candidate.c_str()) <= maxW) {
                current = candidate;
            } else {
                PushWrappedLine(out, current);
                current = word;
            }
        }
        word.clear();
    };

    for (const char* p = text; *p; ++p) {
        const char c = *p;
        if (c == '\r') continue;
        if (c == '\n') {
            flushWord();
            PushWrappedLine(out, current);
            current.clear();
            continue;
        }
        if (c == ' ' || c == '\t') {
            flushWord();
            continue;
        }
        word.push_back(c);
    }
    flushWord();
    PushWrappedLine(out, current);
}

void WrapInfoText(const Row& r, float contentW, std::vector<std::string>& out) {
    out.clear();
    const char* text = r.label ? r.label : "";
    if (!*text) return;

    ImFont* font = Layout::BodyFont();
    const float px = font ? font->FontSize : 13.0f;
    const float textW = (std::max)(32.0f, contentW - kInfoTextX - kInfoPadX);
    WrapTextLine(font, px, text, textW, out);
    if (out.empty()) out.push_back(text);
}

float InfoRowHeight(const Row& r, float contentW) {
    std::vector<std::string> lines;
    WrapInfoText(r, contentW, lines);
    ImFont* font = Layout::BodyFont();
    const float px = font ? font->FontSize : 13.0f;
    const float textH = (static_cast<float>(lines.size()) * px) +
                        (static_cast<float>((lines.size() > 0) ? lines.size() - 1 : 0) * kInfoLineGap);
    const float desired = kInfoPadY * 2.0f + textH;
    return desired;
}

float RowPixelHeight(const Row& r, float contentW) {
    if (r.kind == RowKind::Spacer) return Theme::kRowHeight * 0.5f;
    if (r.kind == RowKind::Header) return Theme::kRowHeight + Theme::kSectionPadY;
    if (r.kind == RowKind::Info) return InfoRowHeight(r, contentW);
    return Theme::kRowHeight;
}

void FormatIntValue(const Row& r, char* buf, size_t bufSz) {
    _snprintf_s(buf, bufSz, _TRUNCATE, "%d", r.intPtr ? *r.intPtr : 0);
}

const char* FormatIntDisplayValue(const Row& r, char* buf, size_t bufSz) {
    if (r.valueFormatter) {
        const char* formatted = r.valueFormatter(r);
        if (formatted && *formatted) {
            return formatted;
        }
    }
    FormatIntValue(r, buf, bufSz);
    return buf;
}

float GetIntSliderProgress01(const Row& r) {
    if (!r.intPtr || r.intMax <= r.intMin) {
        return 0.0f;
    }
    const int value = *r.intPtr;
    const float denom = static_cast<float>(r.intMax - r.intMin);
    return static_cast<float>(value - r.intMin) / denom;
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
    float y = layout.contentTopY + layout.animOffsetY - scrollPx;
    for (int i = 0; i < rowCount; ++i) {
        RowRect& r = out[i];
        r.y = y;
        r.h = RowPixelHeight(rows[i], layout.contentW);
        const bool hidden = RowHidden(rows[i]);
        r.visible = !hidden &&
                    (r.y + r.h) > layout.contentTopY &&
                    r.y < layout.contentBottomY;
        if (!hidden) y += r.h;
    }
    return y - (layout.contentTopY + layout.animOffsetY - scrollPx);
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

constexpr int kMaxSubmenuDepth = 4;
constexpr float kSubmenuAnimMs = 170.0f;
constexpr float kSubmenuSlidePx = 78.0f;
constexpr double kDegToRadDivisor = 57.29579143313326;

struct SubmenuFrame {
    const char* title = nullptr;
    RowListBuilder builder = nullptr;
    int focus = 0;
    ScrollState scroll;
};

struct SubmenuState {
    SubmenuFrame frames[kMaxSubmenuDepth];
    int depth = 0;
    DWORD animTick = 0;
    int animDir = 1;
};

SubmenuState g_submenus;
bool g_focusAboveRequested = false;

float Clamp01(float v) {
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

float AnimT(DWORD startTick, float durationMs) {
    if (startTick == 0 || durationMs <= 0.0f) return 1.0f;
    return Clamp01(static_cast<float>(GetTickCount() - startTick) / durationMs);
}

float EfzCosEase(float t01) {
    const double degrees = 180.0 * Clamp01(t01);
    return static_cast<float>((1.0 - std::cos(degrees / kDegToRadDivisor)) * 0.5);
}

float CurrentSubmenuOffsetX() {
    if (g_submenus.animTick == 0) return 0.0f;
    const float ease = EfzCosEase(AnimT(g_submenus.animTick, kSubmenuAnimMs));
    return (1.0f - ease) * kSubmenuSlidePx * static_cast<float>(g_submenus.animDir);
}

void StartSubmenuAnimation(int dir) {
    g_submenus.animTick = GetTickCount();
    g_submenus.animDir = (dir < 0) ? -1 : 1;
}

void OpenSubmenu(const Row& r) {
    if (!r.submenuBuilder || g_submenus.depth >= kMaxSubmenuDepth) return;
    SubmenuFrame& f = g_submenus.frames[g_submenus.depth++];
    f.title = (r.submenuTitle && r.submenuTitle[0]) ? r.submenuTitle : r.label;
    f.builder = r.submenuBuilder;
    f.focus = 0;
    f.scroll = ScrollState{};
    StartSubmenuAnimation(+1);
    Sound::PlayDecision();
    Input::ResetEdges();
}

void CloseOneSubmenu() {
    if (g_submenus.depth <= 0) return;
    --g_submenus.depth;
    StartSubmenuAnimation(-1);
    Sound::PlayDecision();
    Input::ResetEdges();
}

ScreenLayout ApplySubmenuAnimation(const ScreenLayout& layout) {
    ScreenLayout out = layout;
    out.animOffsetX += CurrentSubmenuOffsetX();
    return out;
}

bool ActiveSubmenuRows(Row*& rows, int& rowCount, int*& focus, ScrollState*& scroll, const char*& title) {
    if (g_submenus.depth <= 0) return false;
    SubmenuFrame& f = g_submenus.frames[g_submenus.depth - 1];
    if (!f.builder) return false;
    rows = f.builder(rowCount);
    focus = &f.focus;
    scroll = &f.scroll;
    title = f.title;
    return true;
}

bool RowStartsInfoBlock(const Row* rows, int idx) {
    if (!rows || idx < 0) return false;
    if (RowHidden(rows[idx]) || rows[idx].kind != RowKind::Info) return false;
    if (idx == 0) return true;
    return RowHidden(rows[idx - 1]) || rows[idx - 1].kind != RowKind::Info;
}

void DrawInfoBlockBackgrounds(ImDrawList* dl, const ScreenLayout& layout,
                              const Row* rows, int rowCount,
                              const RowRect* rects) {
    if (!dl || !rows || !rects) return;
    using namespace Theme;
    const float x = layout.contentX + layout.animOffsetX;
    const float w = layout.contentW;

    for (int i = 0; i < rowCount; ++i) {
        if (!RowStartsInfoBlock(rows, i)) continue;

        int end = i;
        while (end + 1 < rowCount &&
               !RowHidden(rows[end + 1]) &&
               rows[end + 1].kind == RowKind::Info) {
            ++end;
        }

        const float y1 = rects[i].y;
        const float y2 = rects[end].y + rects[end].h;
        if (y2 <= layout.contentTopY || y1 >= layout.contentBottomY) {
            i = end;
            continue;
        }

        const float top = y1 + 1.0f;
        const float bot = y2 - 1.0f;
        dl->AddRectFilled(ImVec2(x - 2.0f, top),
                          ImVec2(x + w + 2.0f, bot),
                          kInfoFill);
        dl->AddRectFilled(ImVec2(x + 1.0f, top),
                          ImVec2(x + 5.0f, bot),
                          kInfoAccent);
        dl->AddLine(ImVec2(x - 2.0f, top),
                    ImVec2(x + w + 2.0f, top),
                    IM_COL32(255, 255, 255, 55), 1.0f);
        dl->AddLine(ImVec2(x - 2.0f, bot),
                    ImVec2(x + w + 2.0f, bot),
                    IM_COL32(255, 255, 255, 42), 1.0f);

        i = end;
    }
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
           bool (*isHidden)(),
           bool useSwitchPlayerToggle) {
    Row r{};
    r.kind = RowKind::Toggle;
    r.label = label;
    r.boolPtr = p;
    r.onChange = onChange;
    r.isDisabled = isDisabled;
    r.isHidden = isHidden;
    r.useSwitchPlayerToggle = useSwitchPlayerToggle;
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

Row IntSlider(const char* label, int* p, int mn, int mx,
              int stepSmall, int stepBig,
              void (*onChange)(),
              bool (*isDisabled)(),
              bool (*isHidden)()) {
    Row r{};
    r.kind = RowKind::IntSlider;
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

Row Submenu(const char* label, const char* title, RowListBuilder builder,
            const char* (*valueFn)(),
            bool (*isDisabled)(),
            bool (*isHidden)()) {
    Row r{};
    r.kind = RowKind::Submenu;
    r.label = label;
    r.submenuTitle = title;
    r.submenuBuilder = builder;
    r.actionValue = valueFn;
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
    Sound::PlayDecision();
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
    Sound::PlayDecision();
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
    ImFont* bFont = Layout::BodyFont();
    const float bPx = bFont ? bFont->FontSize : 13.0f;
    const float prefixW = Layout::MeasureTextW(bFont, bPx, PopupIsMulti() ? "> [X] " : "> ");
    float widestChoiceW = 0.0f;
    for (int i = 0; i < g_popup.choiceCount; ++i) {
        const char* text = (g_popup.choices && g_popup.choices[i]) ? g_popup.choices[i] : "";
        widestChoiceW = (std::max)(widestChoiceW, Layout::MeasureTextW(bFont, bPx, text));
    }
    g.popupW = (std::max)(320.0f, widestChoiceW + prefixW + 28.0f);
    const float maxW = kPanelW - 16.0f;
    if (g.popupW > maxW) g.popupW = maxW;
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
    const bool navLeft  = Input::NavLeft();
    const bool navRight = Input::NavRight();
    const bool activate = Input::Activate();
    const bool back     = Input::Back();
    const bool keyboardOrPadEdge = navUp || navDown || navLeft || navRight ||
                                   activate || back || Input::SwitchPlayer();

    static unsigned int s_popupMouseFrame = ~0u;
    static float s_popupLastMouseX = -1.0f;
    static float s_popupLastMouseY = -1.0f;
    static bool s_popupMouseMoved = false;
    const unsigned int frame = ImGui::GetFrameCount();
    if (frame != s_popupMouseFrame) {
        s_popupMouseFrame = frame;
        s_popupMouseMoved = false;
        auto m = Input::GetMouse();
        if (m.valid) {
            if (s_popupLastMouseX < 0.0f && s_popupLastMouseY < 0.0f) {
                s_popupLastMouseX = m.x;
                s_popupLastMouseY = m.y;
            } else {
                const float dx = m.x - s_popupLastMouseX;
                const float dy = m.y - s_popupLastMouseY;
                if ((dx * dx + dy * dy) > 1.0f) {
                    s_popupMouseMoved = true;
                    s_popupLastMouseX = m.x;
                    s_popupLastMouseY = m.y;
                }
            }
        }
    }

    if (navUp)   { g_popup.focusIdx = (g_popup.focusIdx - 1 + g_popup.choiceCount) % g_popup.choiceCount; Sound::PlayCursor(); }
    if (navDown) { g_popup.focusIdx = (g_popup.focusIdx + 1) % g_popup.choiceCount; Sound::PlayCursor(); }
    if (activate) {
        toggleAt(g_popup.focusIdx);
        Sound::PlayDecision();
        if (!isMulti) { ClosePopup(); return; }
    }
    if (back) {
        Sound::PlayDecision();
        ClosePopup();
        return;
    }

    const float wheel = ImGui::GetIO().MouseWheel;
    if (wheel != 0.0f) g_popup.scrollPx -= wheel * g.rowH * 3.0f;

    const bool mouseLeftEdge = Input::MouseLeftEdge();
    if (!keyboardOrPadEdge && mouseLeftEdge) {
        for (int i = 0; i < g_popup.choiceCount; ++i) {
            const float ry = g.listY + i * g.rowH - g_popup.scrollPx;
            if (ry < g.listY) continue;
            if (ry + g.rowH > g.listY + g.listH) break;
            if (Input::MouseHovering(g.listX, ry, g.listW, g.rowH)) {
                toggleAt(i);
                Sound::PlayDecision();
                if (!isMulti) { ClosePopup(); return; }
                break;
            }
        }
        // Click outside popup dismisses.
        if (!Input::MouseHovering(g.px, g.py, g.popupW, g.popupH)) {
            Sound::PlayDecision();
            ClosePopup();
            return;
        }
    }
    if (!keyboardOrPadEdge && s_popupMouseMoved) {
        for (int i = 0; i < g_popup.choiceCount; ++i) {
            const float ry = g.listY + i * g.rowH - g_popup.scrollPx;
            if (ry < g.listY) continue;
            if (ry + g.rowH > g.listY + g.listH) break;
            if (Input::MouseHovering(g.listX, ry, g.listW, g.rowH)) {
                g_popup.focusIdx = i;
                break;
            }
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
        char line[256];
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
        totalH += RowPixelHeight(rows[i], layout.contentW);
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

    ScreenLayout drawLayout = ApplySubmenuAnimation(layout);
    const Row* drawRows = rows;
    int drawCount = rowCount;
    int drawFocus = focus;
    const ScrollState* drawScroll = &scroll;
    (void)title;

    Row* submenuRows = nullptr;
    int* submenuFocus = nullptr;
    ScrollState* submenuScroll = nullptr;
    const char* submenuTitle = nullptr;
    if (ActiveSubmenuRows(submenuRows, drawCount, submenuFocus, submenuScroll, submenuTitle)) {
        drawRows = submenuRows;
        drawFocus = submenuFocus ? *submenuFocus : 0;
        drawScroll = submenuScroll ? submenuScroll : &scroll;
        (void)submenuTitle;
    }

    RowRect rects[128];
    if (drawCount > 128) drawCount = 128;
    ComputeRects(drawLayout, drawRows, drawCount, drawScroll->scrollPx, rects);

    ImFont* bFont = Layout::BodyFont();
    const float bPx = bFont ? bFont->FontSize : 13.0f;

    // Push a clip rect around the scrollable region so rows partially off the
    // top/bottom edge get correctly clipped instead of bleeding into header/hint.
    dl->PushClipRect(ImVec2(drawLayout.panelX, drawLayout.contentTopY),
                     ImVec2(drawLayout.panelX + kPanelW, drawLayout.contentBottomY),
                     true);

    DrawInfoBlockBackgrounds(dl, drawLayout, drawRows, drawCount, rects);

    for (int i = 0; i < drawCount; ++i) {
        if (!rects[i].visible) continue;
        const Row& r = drawRows[i];
        const bool focused = (i == drawFocus);
        const bool disabled = RowDisabled(r);
        const float x = drawLayout.contentX + drawLayout.animOffsetX;
        const float y = rects[i].y;
        const float w = drawLayout.contentW;

        switch (r.kind) {
            case RowKind::Header:
                Layout::DrawHeader(dl, x, y, w, r.label);
                break;
            case RowKind::Info: {
                std::vector<std::string> lines;
                WrapInfoText(r, w, lines);
                const float rowH = rects[i].h;
                const float textBlockH =
                    static_cast<float>(lines.size()) * bPx +
                    static_cast<float>((lines.size() > 0) ? lines.size() - 1 : 0) * kInfoLineGap;
                const float py0 = y + (std::max)(0.0f, (rowH - textBlockH) * 0.5f);

                // Subtle left-edge cursor when this Info is the focused row,
                // so keyboard scrolling has a visible anchor without making
                // body paragraphs noisy. Active text colour brightens too.
                if (focused) {
                    dl->AddRectFilled(
                        ImVec2(x + 2.0f,            py0 - 1.0f),
                        ImVec2(x + 4.0f,            py0 + textBlockH + 1.0f),
                        kTextActive);
                }
                const ImU32 textColor = focused ? kTextActive : kTextInactive;

                const float px = x + kInfoTextX;
                for (size_t li = 0; li < lines.size(); ++li) {
                    const float py = py0 + static_cast<float>(li) * (bPx + kInfoLineGap);
                    Layout::DrawString(dl, bFont, bPx, px + 1.0f, py + 1.0f,
                                       IM_COL32(0, 0, 0, 190), lines[li].c_str());
                    Layout::DrawString(dl, bFont, bPx, px, py,
                                       textColor, lines[li].c_str());
                }
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
            case RowKind::IntSlider: {
                char buf[32];
                const char* valueText = FormatIntDisplayValue(r, buf, sizeof(buf));
                Layout::DrawRowSlider(dl,
                                      x,
                                      y,
                                      w,
                                      r.label,
                                      valueText,
                                      GetIntSliderProgress01(r),
                                      focused,
                                      disabled);
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
            case RowKind::Submenu: {
                const char* val = r.actionValue ? r.actionValue() : nullptr;
                Layout::DrawRowDrill(dl, x, y, w, r.label, val, focused, disabled);
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
    if (drawScroll->maxScrollPx > 0.0f) {
        const float viewH = drawLayout.contentBottomY - drawLayout.contentTopY;
        const float totalH = viewH + drawScroll->maxScrollPx;
        const float barX = drawLayout.panelX + kPanelW - 4.0f;
        const float frac = drawScroll->scrollPx / drawScroll->maxScrollPx;
        const float barH = viewH * (viewH / totalH);
        const float barY = drawLayout.contentTopY + (viewH - barH) * frac;
        dl->AddRectFilled(ImVec2(barX, barY),
                          ImVec2(barX + 2.0f, barY + barH),
                          kRule);
    }
}

bool HandleRowsInput(const ScreenLayout& layout,
                     const Row* rows, int rowCount,
                     int& focus,
                     ScrollState& scroll,
                     bool submenuContext) {
    if (IsKeybindActive()) {
        return false;
    }

    if (!layout.inputEnabled) {
        EnsureFocusVisible(layout, rows, rowCount, focus, scroll);
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

    const bool navUp    = Input::NavUp();
    const bool navDown  = Input::NavDown();
    const bool navLeft  = Input::NavLeft();
    const bool navRight = Input::NavRight();
    const bool keyboardOrPadEdge = navUp || navDown || navLeft || navRight ||
                                   Input::Activate() || Input::Back() ||
                                   Input::SwitchPlayer();

    // Mouse hover should not continuously steal focus from keyboard/gamepad
    // navigation. Only let hover retarget focus when the cursor actually moved
    // this frame, or when the user clicks a row.
    static unsigned int s_mouseFrame = ~0u;
    static float s_lastMouseX = -1.0f;
    static float s_lastMouseY = -1.0f;
    static bool s_mouseMoved = false;
    const unsigned int frame = ImGui::GetFrameCount();
    if (frame != s_mouseFrame) {
        s_mouseFrame = frame;
        s_mouseMoved = false;
        auto m = Input::GetMouse();
        if (m.valid) {
            if (s_lastMouseX < 0.0f && s_lastMouseY < 0.0f) {
                s_lastMouseX = m.x;
                s_lastMouseY = m.y;
            } else {
                const float dx = m.x - s_lastMouseX;
                const float dy = m.y - s_lastMouseY;
                if ((dx * dx + dy * dy) > 1.0f) {
                    s_mouseMoved = true;
                    s_lastMouseX = m.x;
                    s_lastMouseY = m.y;
                }
            }
        }
    }

    const bool mouseLeftEdge = Input::MouseLeftEdge();
    bool clickActivated = false;
    if (!keyboardOrPadEdge && (s_mouseMoved || mouseLeftEdge)) {
        for (int i = 0; i < rowCount; ++i) {
            if (!rects[i].visible) continue;
            if (!RowIsFocusable(rows[i])) continue;
            if (Input::MouseHovering(layout.contentX + layout.animOffsetX, rects[i].y,
                                     layout.contentW, rects[i].h)) {
                focus = i;
                clickActivated = mouseLeftEdge;
                break;
            }
        }
    }

    const bool activate = Input::Activate() || clickActivated;

    const int firstFocusable = FindFocusable(rows, rowCount, 0, +1);
    const int oldFocus = focus;
    if (navUp && !submenuContext && focus == firstFocusable) {
        g_focusAboveRequested = true;
    } else if (navUp) {
        focus = FindFocusable(rows, rowCount, focus - 1, -1);
    }
    if (navDown) focus = FindFocusable(rows, rowCount, focus + 1, +1);
    if (focus != oldFocus) {
        Sound::PlayCursor();
    }

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
                const bool switchToggle = r.useSwitchPlayerToggle && Input::SwitchPlayer();
                if (activate || switchToggle)  { *r.boolPtr = !*r.boolPtr; changed = true; }
                else if (navLeft && *r.boolPtr)  { *r.boolPtr = false; changed = true; }
                else if (navRight && !*r.boolPtr){ *r.boolPtr = true;  changed = true; }
                if (changed) Sound::PlayCursor();
                fire(changed);
                break;
            }
            case RowKind::IntNumber:
            case RowKind::IntSlider: {
                if (disabled || !r.intPtr) break;
                bool changed = false;
                const int sBig = r.intStepBig > 0 ? r.intStepBig : r.intStepSmall;
                const int s = ShiftHeld() ? sBig : (r.intStepSmall > 0 ? r.intStepSmall : 1);
                if (navLeft)  { *r.intPtr -= s; changed = true; }
                if (navRight) { *r.intPtr += s; changed = true; }
                if (activate) { *r.intPtr += (s > 0 ? s : 1); changed = true; }
                if (*r.intPtr < r.intMin) *r.intPtr = r.intMin;
                if (*r.intPtr > r.intMax) *r.intPtr = r.intMax;
                if (changed) Sound::PlayCursor();
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
                if (changed) Sound::PlayCursor();
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
                if (changed) Sound::PlayCursor();
                fire(changed);
                break;
            }
            case RowKind::Choices: {
                if (disabled || !r.choiceIdxPtr || r.choiceCount <= 0) break;
                int& idx = *r.choiceIdxPtr;
                bool changed = false;
                if (navLeft)              { idx = (idx - 1 + r.choiceCount) % r.choiceCount; changed = true; }
                else if (navRight || activate) { idx = (idx + 1) % r.choiceCount; changed = true; }
                if (changed) Sound::PlayCursor();
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
                if (changed) Sound::PlayCursor();
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
                if (changed) Sound::PlayCursor();
                fire(changed);
                break;
            }
            case RowKind::MaskPicker: {
                if (disabled) break;
                if (activate) { OpenMaskPopup(r); }
                break;
            }
            case RowKind::Submenu: {
                if (disabled) break;
                if (activate) { OpenSubmenu(r); }
                break;
            }
            case RowKind::Action: {
                if (disabled) break;
                if (activate && r.action) { Sound::PlayDecision(); r.action(); }
                break;
            }
            default:
                break;
        }
    }

    // Keep the focused row visible after any nav/click.
    EnsureFocusVisible(layout, rows, rowCount, focus, scroll);

    const bool back = Input::Back();
    if (back && submenuContext) {
        CloseOneSubmenu();
        return false;
    }
    return back;
}

bool HandleListInput(const ScreenLayout& layout,
                     const Row* rows, int rowCount,
                     int& focus,
                     ScrollState& scroll) {
    g_focusAboveRequested = false;
    ScreenLayout activeLayout = ApplySubmenuAnimation(layout);
    Row* submenuRows = nullptr;
    int submenuCount = 0;
    int* submenuFocus = nullptr;
    ScrollState* submenuScroll = nullptr;
    const char* submenuTitle = nullptr;
    if (ActiveSubmenuRows(submenuRows, submenuCount, submenuFocus, submenuScroll, submenuTitle)) {
        if (!submenuFocus || !submenuScroll) return false;
        return HandleRowsInput(activeLayout, submenuRows, submenuCount, *submenuFocus, *submenuScroll, true);
    }
    return HandleRowsInput(activeLayout, rows, rowCount, focus, scroll, false);
}

bool IsPopupActive() { return PopupActive(); }
bool IsSubmenuActive() { return g_submenus.depth > 0; }
bool ConsumeFocusAboveRequest() {
    const bool requested = g_focusAboveRequested;
    g_focusAboveRequested = false;
    return requested;
}

void ResetSubmenus() {
    g_submenus = SubmenuState{};
}

bool TickPopupIfOpen(ImDrawList* dl, const ScreenLayout& layout) {
    if (!g_popup.active) return false;
    // Input has already been processed inside HandleListInput. This pass
    // only renders the popup so it sits on top of the list rows.
    PopupRender(dl, layout);
    return true;
}

// ===== Hotkey binding =====
namespace KeybindAPI { void TickInput(); }

constexpr uint32_t kKeybindLtBit = 0x10000u;
constexpr uint32_t kKeybindRtBit = 0x20000u;
constexpr int kKeybindTriggerThreshold = 30;

uint32_t PollRelevantGamepadMask() {
    XInputShim::RefreshSnapshotOncePerFrame();

    const int controllerIndex = Config::GetSettings().controllerIndex;
    uint32_t mask = 0;
    auto accumulate = [&](const XINPUT_STATE& state) {
        mask |= state.Gamepad.wButtons;
        if (state.Gamepad.bLeftTrigger > kKeybindTriggerThreshold) mask |= kKeybindLtBit;
        if (state.Gamepad.bRightTrigger > kKeybindTriggerThreshold) mask |= kKeybindRtBit;
    };

    if (controllerIndex >= 0 && controllerIndex <= 3) {
        if (const XINPUT_STATE* state = XInputShim::GetCachedState(controllerIndex)) {
            accumulate(*state);
        }
        return mask;
    }

    for (int i = 0; i < 4; ++i) {
        if (const XINPUT_STATE* state = XInputShim::GetCachedState(i)) {
            accumulate(*state);
        }
    }
    return mask;
}

int FirstCapturedGamepadMask(uint32_t mask) {
    static const int kCapturePriority[] = {
        XINPUT_GAMEPAD_A,
        XINPUT_GAMEPAD_B,
        XINPUT_GAMEPAD_X,
        XINPUT_GAMEPAD_Y,
        XINPUT_GAMEPAD_LEFT_SHOULDER,
        XINPUT_GAMEPAD_RIGHT_SHOULDER,
        XINPUT_GAMEPAD_BACK,
        XINPUT_GAMEPAD_START,
        XINPUT_GAMEPAD_LEFT_THUMB,
        XINPUT_GAMEPAD_RIGHT_THUMB,
        XINPUT_GAMEPAD_DPAD_UP,
        XINPUT_GAMEPAD_DPAD_DOWN,
        XINPUT_GAMEPAD_DPAD_LEFT,
        XINPUT_GAMEPAD_DPAD_RIGHT,
        static_cast<int>(kKeybindLtBit),
        static_cast<int>(kKeybindRtBit),
    };

    for (int button : kCapturePriority) {
        if ((mask & static_cast<uint32_t>(button)) != 0) {
            return button;
        }
    }
    return 0;
}

void SnapshotKeyboardState(bool (&prevPressed)[256]) {
    for (int vk = 0; vk < 256; ++vk) {
        prevPressed[vk] = (GetAsyncKeyState(vk) & 0x8000) != 0;
    }
}

bool KeyEdge(bool (&prevPressed)[256], int vk) {
    const bool now = (GetAsyncKeyState(vk) & 0x8000) != 0;
    const bool was = prevPressed[vk];
    prevPressed[vk] = now;
    return now && !was;
}

struct KeybindState {
    bool active = false;
    char title[48] = "";              // shown to user (e.g. "OPEN MENU")
    int* settingsField = nullptr;     // mutable pointer into Config::Settings
    char iniSection[16] = "";         // INI section to persist into
    char iniKey[32]    = "";          // INI key to persist
    bool prevPressed[256] = {};
    uint32_t prevGamepadMask = 0;
    bool captureGamepad = false;
    bool primed = false;              // false on the first frame so a held key
                                      // or button (the Activate that opened binding) is
                                      // treated as already-down
};
KeybindState g_keybind;

bool IsKeybindActive() { return g_keybind.active; }
bool IsGamepadKeybindActive() { return g_keybind.active && g_keybind.captureGamepad; }

void OpenKeybind(const char* title, int* field,
                 const char* section, const char* key) {
    if (!field || !title || !section || !key) return;
    if (!Input::IsGameWindowActive()) return;

    g_keybind.active = true;
    strncpy_s(g_keybind.title, sizeof(g_keybind.title), title, _TRUNCATE);
    g_keybind.settingsField = field;
    strncpy_s(g_keybind.iniSection, sizeof(g_keybind.iniSection), section, _TRUNCATE);
    strncpy_s(g_keybind.iniKey, sizeof(g_keybind.iniKey), key, _TRUNCATE);
    g_keybind.captureGamepad = false;
    g_keybind.prevGamepadMask = 0;
    // Snapshot all keys as currently-pressed so the Activate edge that opened
    // this binding doesn't immediately register as a capture.
    SnapshotKeyboardState(g_keybind.prevPressed);
    g_keybind.primed = false;
}

void OpenGamepadKeybind(const char* title, int* field,
                        const char* section, const char* key) {
    if (!field || !title || !section || !key) return;
    if (!Input::IsGameWindowActive()) return;

    g_keybind.active = true;
    strncpy_s(g_keybind.title, sizeof(g_keybind.title), title, _TRUNCATE);
    g_keybind.settingsField = field;
    strncpy_s(g_keybind.iniSection, sizeof(g_keybind.iniSection), section, _TRUNCATE);
    strncpy_s(g_keybind.iniKey, sizeof(g_keybind.iniKey), key, _TRUNCATE);
    g_keybind.captureGamepad = true;
    SnapshotKeyboardState(g_keybind.prevPressed);
    g_keybind.prevGamepadMask = PollRelevantGamepadMask();
    g_keybind.primed = false;
}

void CloseKeybind() {
    g_keybind.active = false;
    g_keybind.settingsField = nullptr;
    g_keybind.iniSection[0] = '\0';
    g_keybind.iniKey[0] = '\0';
    g_keybind.captureGamepad = false;
    g_keybind.prevGamepadMask = 0;
}

bool VkIsBindable(int vk) {
    // Disallow mouse buttons and pure modifiers — the user almost never wants
    // to bind those, and they'd interfere with menu navigation.
    if (vk >= 0x01 && vk <= 0x06) return false;     // mouse
    if (vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT) return false;
    if (vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL) return false;
    if (vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU) return false;
    if (vk == VK_LWIN || vk == VK_RWIN) return false;
    if (vk == VK_CLEAR) return false;               // often reports as phantom "Num 5"
    if (vk == VK_ESCAPE) return false;              // reserved for cancel
    return true;
}

bool VkCountsTowardsPriming(int vk) {
    return VkIsBindable(vk);
}

namespace KeybindAPI {
    void TickInput() {
        if (!g_keybind.active) return;
        if (!Input::IsGameWindowActive()) {
            memset(g_keybind.prevPressed, 0, sizeof(g_keybind.prevPressed));
            g_keybind.prevGamepadMask = 0;
            g_keybind.primed = false;
            return;
        }

        // Cancel
        if (KeyEdge(g_keybind.prevPressed, VK_ESCAPE)) {
            CloseKeybind();
            Input::ResetEdges();
            return;
        }

        if (g_keybind.captureGamepad) {
            const uint32_t currentMask = PollRelevantGamepadMask();
            const bool anyHeld = currentMask != 0;

            if (!g_keybind.primed) {
                g_keybind.prevGamepadMask = currentMask;
                if (!anyHeld) g_keybind.primed = true;
                return;
            }

            const uint32_t edgeMask = currentMask & ~g_keybind.prevGamepadMask;
            g_keybind.prevGamepadMask = currentMask;

            const int cancelMask = Config::GetSettings().gpToggleMenuButton;
            if (cancelMask >= 0 && (edgeMask & static_cast<uint32_t>(cancelMask)) != 0) {
                CloseKeybind();
                Input::ResetEdges();
                return;
            }

            if (KeyEdge(g_keybind.prevPressed, VK_DELETE) ||
                KeyEdge(g_keybind.prevPressed, VK_BACK)) {
                if (g_keybind.settingsField) {
                    *g_keybind.settingsField = -1;
                    Config::SetSetting(g_keybind.iniSection, g_keybind.iniKey, "-1");
                }
                CloseKeybind();
                Input::ResetEdges();
                return;
            }

            const int captured = FirstCapturedGamepadMask(edgeMask);
            if (captured && g_keybind.settingsField) {
                *g_keybind.settingsField = captured;
                Config::SetSetting(g_keybind.iniSection, g_keybind.iniKey,
                                   Config::GetGamepadButtonName(captured));
                CloseKeybind();
                Input::ResetEdges();
            }
            return;
        }

        // Capture next bindable key edge
        bool anyHeld = false;
        int captured = 0;
        for (int vk = 0; vk < 256; ++vk) {
            if (vk == VK_ESCAPE) continue;
            if (!VkCountsTowardsPriming(vk)) continue;
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
    const float boxH = g_keybind.captureGamepad ? 136.0f : 120.0f;
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
    if (g_keybind.captureGamepad) {
        char cancelBuf[96];
        const int cancelMask = Config::GetSettings().gpToggleMenuButton;
        if (cancelMask >= 0) {
            _snprintf_s(cancelBuf, sizeof(cancelBuf), _TRUNCATE,
                        "%s TO CANCEL",
                        Config::GetGamepadButtonName(cancelMask).c_str());
        } else {
            _snprintf_s(cancelBuf, sizeof(cancelBuf), _TRUNCATE,
                        "ESC TO CANCEL");
        }
        if (!g_keybind.primed) {
            centerText(by + 46.0f, "RELEASE ALL INPUTS...", kTextInactive);
        } else {
            centerText(by + 46.0f, "PRESS A CONTROLLER BUTTON", kTextActive);
        }
        centerText(by + 74.0f, "DELETE / BACKSPACE TO DISABLE", kTextInactive);
        centerText(by + 100.0f, cancelBuf, kTextInactive);
    } else {
        if (!g_keybind.primed) {
            centerText(by + 50.0f, "RELEASE ALL KEYS...", kTextInactive);
        } else {
            centerText(by + 50.0f, "PRESS A KEY", kTextActive);
        }
        centerText(by + 80.0f, "ESC TO CANCEL", kTextInactive);
    }

    return true;
}

} // namespace CustomMenu::Screens
