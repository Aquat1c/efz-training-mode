#pragma once

// Generic "list screen" framework for the custom menu.
//
// Secondary screens (AUTO, CHARS, SETTINGS, HELP and their sub-pages) all
// follow the same pattern: a vertical list of heterogeneous rows. Rather
// than hand-writing render + input for each, they are expressed as arrays
// of `Row` descriptors. This file defines the descriptor, the builder
// helpers, and the render/input driver.

#include <cstdint>

struct ImDrawList;

namespace CustomMenu::Screens {

enum class RowKind : uint8_t {
    // Decorative / non-focusable
    Header = 0,  // section header (uppercase, dim)
    Info,        // plain text line (dim)
    Spacer,      // blank vertical gap

    // Focusable
    Toggle,       // bool value; Activate/L/R toggles
    IntNumber,    // int value;  L/R small step, U/D big step, Shift=big
    FloatNumber,  // float value
    DoubleNumber, // double value
    Choices,      // int index into choices[]; L/R cycles inline
    ActionStrength, // paired choices; Activate opens primary picker, Shift+L/R adjusts secondary
    Dropdown,     // int index into choices[]; Activate opens modal popup
    MaskPicker,   // uint32 bitmask; Activate opens multi-select popup
    Action,       // fire callback on Activate; optional right-side value
};

struct Row;
using RowValueFormatter = const char* (*)(const Row& row);
using PairedChoiceChange = void (*)(int* primary, int* secondary);

struct Row {
    RowKind kind;
    const char* label;

    // Data hooks — only the one relevant to `kind` is used.
    bool*  boolPtr;

    int*   intPtr;
    int    intMin;
    int    intMax;
    int    intStepSmall;
    int    intStepBig;

    float* floatPtr;
    float  floatMin;
    float  floatMax;
    float  floatStepSmall;
    float  floatStepBig;
    const char* floatFmt;

    double* doublePtr;
    double  doubleMin;
    double  doubleMax;
    double  doubleStepSmall;
    double  doubleStepBig;
    const char* doubleFmt;

    int*   choiceIdxPtr;
    const char* const* choices;
    int    choiceCount;

    int*   choice2IdxPtr;
    const char* const* choices2;
    int    choice2Count;

    unsigned int* maskPtr;          // for MaskPicker: bitmask backing store

    void (*action)();

    // Optional right-side display string for Action rows (e.g. a current value).
    const char* (*actionValue)();

    // Optional formatter/callbacks for paired-choice rows.
    RowValueFormatter valueFormatter;
    PairedChoiceChange onPrimaryChoiceChange;
    PairedChoiceChange onSecondaryChoiceChange;

    // Runtime disabled / hidden tests. nullptr = always-enabled / always-shown.
    bool (*isDisabled)();
    bool (*isHidden)();

    // Fired after a value is mutated (via toggle / adjust / choice / edit).
    void (*onChange)();
};

// Row builders. Keep these short so screen definitions read like a small DSL.
Row Header(const char* label);
Row Info(const char* label);
Row Spacer();
Row Toggle(const char* label, bool* p,
           void (*onChange)() = nullptr,
           bool (*isDisabled)() = nullptr,
           bool (*isHidden)() = nullptr);
Row IntNum(const char* label, int* p, int mn, int mx,
           int stepSmall = 1, int stepBig = 10,
           void (*onChange)() = nullptr,
           bool (*isDisabled)() = nullptr,
           bool (*isHidden)() = nullptr);
Row FloatNum(const char* label, float* p, float mn, float mx,
             float stepSmall, float stepBig,
             const char* fmt = "%.2f",
             void (*onChange)() = nullptr,
             bool (*isDisabled)() = nullptr,
             bool (*isHidden)() = nullptr);
Row DoubleNum(const char* label, double* p, double mn, double mx,
              double stepSmall, double stepBig,
              const char* fmt = "%.1f",
              void (*onChange)() = nullptr,
              bool (*isDisabled)() = nullptr,
              bool (*isHidden)() = nullptr);
Row ChoicesRow(const char* label, int* idx, const char* const* items, int n,
               void (*onChange)() = nullptr,
               bool (*isDisabled)() = nullptr,
               bool (*isHidden)() = nullptr);
Row ActionStrengthRow(const char* label,
                      int* actionIdx, const char* const* actions, int actionCount,
                      int* strengthIdx, const char* const* strengths, int strengthCount,
                      RowValueFormatter formatter = nullptr,
                      PairedChoiceChange onPrimaryChange = nullptr,
                      PairedChoiceChange onSecondaryChange = nullptr,
                      void (*onChange)() = nullptr,
                      bool (*isDisabled)() = nullptr,
                      bool (*isHidden)() = nullptr);
// Dropdown: shows the currently-selected choice label and opens a modal popup
// listing all options on Activate. Ideal for long option lists (10+ items).
Row DropdownRow(const char* label, int* idx, const char* const* items, int n,
                void (*onChange)() = nullptr,
                bool (*isDisabled)() = nullptr,
                bool (*isHidden)() = nullptr);
// MaskPicker: like Dropdown but multi-select. The popup shows [X]/[ ] per item;
// Activate toggles a bit in `mask`. Closed by Back.
Row MaskPickerRow(const char* label, unsigned int* mask,
                  const char* const* items, int n,
                  void (*onChange)() = nullptr,
                  bool (*isDisabled)() = nullptr,
                  bool (*isHidden)() = nullptr);
Row Action(const char* label, void (*fn)(),
           const char* (*valueFn)() = nullptr,
           bool (*isDisabled)() = nullptr,
           bool (*isHidden)() = nullptr);

// ===== Driver =====

struct ScreenLayout {
    float panelX;
    float contentX;
    float contentW;
    float contentTopY;    // Y below the tab bar where list content begins
    float contentBottomY; // Y above the hint line; limits visible rows
};

// Per-screen scrollable state. The renderer owns one of these per screen and
// passes it through; screens update scrollPx to keep the focused row visible.
struct ScrollState {
    float scrollPx = 0.0f;  // pixels scrolled from the top
    float maxScrollPx = 0.0f; // last-known max; cached for clamping
};

// Clamps focus to a valid focusable row. Call once per frame before input.
void ClampFocus(const Row* rows, int rowCount, int& focus);

// Render the row list inside `layout`. `title` draws as the first header.
// Rows that are `isHidden()` are skipped entirely.
void RenderList(ImDrawList* dl, const ScreenLayout& layout,
                const char* title,
                const Row* rows, int rowCount,
                int focus,
                const ScrollState& scroll);

// Handle input. Mutates `focus` and `scroll`. Caller is responsible for:
//   - calling ClampFocus() beforehand
//   - invoking the global Back-closes-menu / tab-switching logic outside
// Returns true if Back edge was observed AT the list level (i.e. not consumed
// by an open dropdown popup).
bool HandleListInput(const ScreenLayout& layout,
                     const Row* rows, int rowCount,
                     int& focus,
                     ScrollState& scroll);

// ===== Per-screen entry points =====
// Each refreshes its transient atomic/state mirrors, delegates to the list
// driver for render + input, and reports a Back-edge so the caller can
// decide whether to close the menu.

void RefreshSecondaryScreenMirrors();

// MAIN top-tab sub-panes:
void TickOpponent(ImDrawList* dl, const ScreenLayout& layout, int& focus, ScrollState& scroll, bool& backEdge);
void TickOptions (ImDrawList* dl, const ScreenLayout& layout, int& focus, ScrollState& scroll, bool& backEdge);
// (VALUES sub-pane is rendered specially by the 2-column renderer; not a list screen.)

// AUTO top-tab sub-panes:
void TickTriggers(ImDrawList* dl, const ScreenLayout& layout, int& focus, ScrollState& scroll, bool& backEdge);
void TickMacros  (ImDrawList* dl, const ScreenLayout& layout, int& focus, ScrollState& scroll, bool& backEdge);

// CHARS top-tab (single pane):
void TickChars   (ImDrawList* dl, const ScreenLayout& layout, int& focus, ScrollState& scroll, bool& backEdge);

// SETTINGS top-tab sub-panes:
void TickSettingsGeneral(ImDrawList* dl, const ScreenLayout& layout, int& focus, ScrollState& scroll, bool& backEdge);
void TickSettingsHotkeys(ImDrawList* dl, const ScreenLayout& layout, int& focus, ScrollState& scroll, bool& backEdge);
void TickSettingsDebug  (ImDrawList* dl, const ScreenLayout& layout, int& focus, ScrollState& scroll, bool& backEdge);

// HELP top-tab sub-panes:
void TickHelpAbout   (ImDrawList* dl, const ScreenLayout& layout, int& focus, ScrollState& scroll, bool& backEdge);
void TickHelpHotkeys (ImDrawList* dl, const ScreenLayout& layout, int& focus, ScrollState& scroll, bool& backEdge);
void TickHelpControls(ImDrawList* dl, const ScreenLayout& layout, int& focus, ScrollState& scroll, bool& backEdge);

// Popup (modal dropdown) state — exposed so the renderer can draw it last
// (so it overlays the screen) and query whether it's currently consuming
// input.
bool IsPopupActive();
bool TickPopupIfOpen(ImDrawList* dl, const ScreenLayout& layout);

// Hotkey-binding overlay. While active, every keypress is captured and
// written into the active config setting. The renderer should call
// TickKeybindIfActive() to render the overlay and process input. Returns
// true if the overlay is consuming input.
bool IsKeybindActive();
void OpenKeybind(const char* title, int* field,
                 const char* section, const char* key);
bool TickKeybindIfActive(ImDrawList* dl, const ScreenLayout& layout);

} // namespace CustomMenu::Screens
