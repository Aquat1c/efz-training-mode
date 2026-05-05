#include "../include/gui/custom_menu/renderer.h"
#include "../include/gui/custom_menu/fonts.h"
#include "../include/gui/custom_menu/theme.h"
#include "../include/gui/custom_menu/layout.h"
#include "../include/gui/custom_menu/input.h"
#include "../include/gui/custom_menu/screens.h"
#include "../include/gui/custom_menu/sound.h"
#include "../include/gui/value_lock_state.h"
#include "../include/gui/imgui_impl.h"
#include "../include/gui/imgui_gui.h"
#include "../include/core/logger.h"
#include "../include/utils/utilities.h"
#include "../include/utils/config.h"
#include "../include/core/constants.h"
#include "../include/core/version.h"
#include "../include/game/character_hotswap.h"
#include "../3rdparty/imgui/imgui.h"

#include <windows.h>
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <string>

namespace CustomMenu {

namespace {

// ===== DPI =====
float GetDpiScaleQuick() {
    HWND hwnd = GetActiveWindow();
    if (!hwnd) hwnd = GetForegroundWindow();
    if (!hwnd) return 1.0f;

    typedef UINT(WINAPI* GetDpiForWindowFunc)(HWND);
    HMODULE user32 = GetModuleHandleA("user32.dll");
    if (user32) {
        auto pGet = (GetDpiForWindowFunc)GetProcAddress(user32, "GetDpiForWindow");
        if (pGet) {
            UINT dpi = pGet(hwnd);
            if (dpi > 0) return (float)dpi / 96.0f;
        }
    }
    HDC hdc = GetDC(hwnd);
    if (hdc) {
        int dpiX = GetDeviceCaps(hdc, LOGPIXELSX);
        ReleaseDC(hwnd, hdc);
        if (dpiX > 0) return (float)dpiX / 96.0f;
    }
    return 1.0f;
}

// ===== Panes / top tabs / sub tabs =====
//
// The menu exposes 5 top-level tabs; all but CHARS have multiple sub-tabs.
// Leaf panes are identified by `Pane`, which is what Tick* functions in
// the Screens module dispatch on. Per-pane focus + scroll state is kept
// in ShellState so tabs remember their last-used row when you switch away.

enum Pane {
    PANE_VALUES = 0,            // MAIN / VALUES  (2-column P1 | P2 editor)
    PANE_OPPONENT,              // MAIN / OPPONENT
    PANE_OPTIONS,               // MAIN / OPTIONS
    PANE_MENU,                  // MAIN / MENU
    PANE_TRIGGERS,              // AUTO / TRIGGERS
    PANE_MACROS,                // AUTO / MACROS
    PANE_CHARS,                 // CHARS
    PANE_SETTINGS_GENERAL,      // SETTINGS / GENERAL
    PANE_SETTINGS_HOTKEYS,      // SETTINGS / HOTKEYS
    PANE_SETTINGS_DEBUG,        // SETTINGS / DEBUG
    PANE_HELP_START,            // HELP / START
    PANE_HELP_GUIDE,            // HELP / GUIDE
    PANE_HELP_RESOURCES,        // HELP / RESOURCES
    PANE_HELP_ABOUT,            // HELP / ABOUT
    PANE_COUNT
};

enum TopTab {
    TT_MAIN = 0,
    TT_AUTO,
    TT_CHARS,
    TT_SETTINGS,
    TT_HELP,
    TT_COUNT
};

struct SubTab {
    const char* label;
    int pane;   // Pane
};

struct TopTabInfo {
    const char* label;
    const SubTab* subs;
    int subCount;
};

static const SubTab kSubs_Main[]     = { {"MENU",     PANE_MENU},
                                         {"VALUES",   PANE_VALUES},
                                         {"OPPONENT", PANE_OPPONENT},
                                         {"OPTIONS",  PANE_OPTIONS} };
static const SubTab kSubs_Auto[]     = { {"TRIGGERS", PANE_TRIGGERS},
                                         {"MACROS",   PANE_MACROS} };
static const SubTab kSubs_Chars[]    = { {"CHARS",    PANE_CHARS} };
static const SubTab kSubs_Settings[] = { {"GENERAL",  PANE_SETTINGS_GENERAL},
                                         {"HOTKEYS",  PANE_SETTINGS_HOTKEYS},
                                         {"DEBUG",    PANE_SETTINGS_DEBUG} };
static const SubTab kSubs_Help[]     = { {"START",     PANE_HELP_START},
                                         {"GUIDE",     PANE_HELP_GUIDE},
                                         {"RESOURCES", PANE_HELP_RESOURCES},
                                         {"ABOUT",     PANE_HELP_ABOUT} };

static const TopTabInfo kTopTabs[TT_COUNT] = {
    {"MAIN",     kSubs_Main,     4},
    {"AUTO",     kSubs_Auto,     2},
    {"CHARS",    kSubs_Chars,    1},
    {"SETTINGS", kSubs_Settings, 3},
    {"HELP",     kSubs_Help,     4},
};

inline int ClampTopTab(int t) {
    if (t < 0) return 0;
    if (t >= TT_COUNT) return TT_COUNT - 1;
    return t;
}
inline int ClampSubTab(int top, int s) {
    const int n = kTopTabs[ClampTopTab(top)].subCount;
    if (s < 0) return 0;
    if (s >= n) return n - 1;
    return s;
}

const char* PaneName(int pane) {
    for (int t = 0; t < TT_COUNT; ++t) {
        for (int s = 0; s < kTopTabs[t].subCount; ++s) {
            if (kTopTabs[t].subs[s].pane == pane) return kTopTabs[t].subs[s].label;
        }
    }
    return "UNKNOWN";
}

void LogMenuDetail(const char* fmt, ...) {
    if (!detailedLogging.load()) return;

    char buf[512];
    va_list args;
    va_start(args, fmt);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, args);
    va_end(args);

    LogOut(std::string("[CUSTOM_MENU] ") + buf, true);
}

// Always-on trace logger used to localize crashes along the menu open/render
// path. Every entry is prefixed [CUSTOM_MENU][TRACE]. Keep messages short.
void LogMenuTrace(const char* fmt, ...) {
    char buf[384];
    va_list args;
    va_start(args, fmt);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, args);
    va_end(args);
    LogOut(std::string("[CUSTOM_MENU][TRACE] ") + buf, true);
}

void LogMenuTiming(const char* fmt, ...) {
    char buf[384];
    va_list args;
    va_start(args, fmt);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, args);
    va_end(args);
    LogOut(std::string("[CUSTOM_MENU][TIMING] ") + buf, true);
}

void LogMenuSehStep(unsigned code, const char* step) {
    char buf[224];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE,
        "[CUSTOM_MENU][TRACE] SEH 0x%08X during %s",
        code, step ? step : "(unknown step)");
    LogOut(buf, true);
}

void LogMenuSehPane(unsigned code, const char* paneName) {
    char buf[192];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE,
        "[CUSTOM_MENU][TRACE] SEH 0x%08X in Tick%s",
        code, paneName ? paneName : "?");
    LogOut(buf, true);
}

// Free-function SEH wrapper used to bracket each menu-open / per-frame step.
// Helper functions are split out from the C++-using callers because mixing
// __try with non-trivial unwindable locals is brittle under MSVC.
typedef void (*MenuStepFn)();
static bool SehInvokeStep(const char* step, MenuStepFn fn) {
    __try {
        fn();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LogMenuSehStep((unsigned)GetExceptionCode(), step);
        return false;
    }
}

typedef void (*TickPaneFn)(ImDrawList*, const Screens::ScreenLayout&,
                           int&, Screens::ScrollState&, bool&);
static bool SehTickPane(const char* paneName, TickPaneFn fn,
                        ImDrawList* dl, const Screens::ScreenLayout& sl,
                        int& focus, Screens::ScrollState& scroll, bool& backEdge) {
    __try {
        fn(dl, sl, focus, scroll, backEdge);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LogMenuSehPane((unsigned)GetExceptionCode(), paneName);
        return false;
    }
}

const char* ScreenName(int pane) { return PaneName(pane); }

// ===== Shell / edit / focus state =====
enum class FocusRegion {
    Content = 0,
    SubTabs,
    TopTabs,
};

struct ShellState {
    int  activeTopTab = TT_MAIN;
    int  subIdxPerTop[TT_COUNT] = { 0, 0, 0, 0, 0 };
    int  focusPerPane[PANE_COUNT] = {};
    Screens::ScrollState scrollPerPane[PANE_COUNT];
    FocusRegion focusRegion = FocusRegion::Content;

    bool menuWasVisible = false;
    bool lastWasCustom  = false;

    // Local VK edge detection for the menu hotkey. `input_handler.cpp`
    // consumes bit-0 of GetAsyncKeyState(configMenuKey) while the menu is
    // visible, so we track bit-15 (held) ourselves.
    bool prevMenuKeyDown = false;
    bool keybindWasActive = false;
};
ShellState g_shell;

inline int ActivePane() {
    const TopTabInfo& t = kTopTabs[ClampTopTab(g_shell.activeTopTab)];
    const int s = ClampSubTab(g_shell.activeTopTab, g_shell.subIdxPerTop[g_shell.activeTopTab]);
    return t.subs[s].pane;
}

bool SettingsTabActive() {
    return ClampTopTab(g_shell.activeTopTab) == TT_SETTINGS;
}

std::string g_transientStatusText;
DWORD g_transientStatusTick = 0;

bool ActiveTopHasSubTabs() {
    return kTopTabs[ClampTopTab(g_shell.activeTopTab)].subCount > 1;
}

void SetFocusRegion(FocusRegion region, const char* reason) {
    if (g_shell.focusRegion == region) return;
    LogMenuDetail("FocusRegion %d -> %d (%s)",
        static_cast<int>(g_shell.focusRegion),
        static_cast<int>(region),
        reason ? reason : "n/a");
    g_shell.focusRegion = region;
    if (!reason || !strstr(reason, "mouse")) {
        Sound::PlayCursor();
    }
}

void MoveFocusAboveContent(const char* reason) {
    SetFocusRegion(ActiveTopHasSubTabs() ? FocusRegion::SubTabs : FocusRegion::TopTabs,
                   reason ? reason : "content nav up");
}

// EFZ's replay menu animates submenu transitions by accumulating a
// degree-like value from 0..180 and using cosine for the slide offset.
// We mirror that curve here so panes ease in with the same snappy slowdown.
struct AnimState {
    DWORD openTick = 0;
    DWORD paneTick = 0;
    int   paneDir = 1;
};
AnimState g_anim;

constexpr float kOpenAnimMs = 180.0f;
constexpr float kPaneAnimMs = 170.0f;
constexpr float kPaneSlidePx = 72.0f;
constexpr float kOpenSlidePx = 18.0f;
constexpr double kDegToRadDivisor = 57.29579143313326;
constexpr DWORD kTransientStatusMs = 1800;

void SetTransientStatus(const char* text) {
    g_transientStatusText = text ? text : "";
    g_transientStatusTick = GetTickCount();
}

const char* CurrentTransientStatus() {
    if (g_transientStatusText.empty()) {
        return nullptr;
    }
    if ((GetTickCount() - g_transientStatusTick) > kTransientStatusMs) {
        g_transientStatusText.clear();
        return nullptr;
    }
    return g_transientStatusText.c_str();
}

bool SettingsQuickSaveAllowed() {
    return SettingsTabActive()
        && !Screens::IsPopupActive()
        && !Screens::IsKeybindActive()
        && !Screens::IsTextEditorActive();
}

void HandleSettingsQuickSave() {
    if (!SettingsQuickSaveAllowed() || !Input::SwitchPlayer()) {
        return;
    }

    const bool ok = Config::SaveSettings();
    if (ok) {
        Sound::PlayDecision();
        SetTransientStatus("SETTINGS SAVED TO DISK");
        LogOut(std::string("[CUSTOM_MENU] Settings saved to disk via D button from ") + ScreenName(ActivePane()), false);
    } else {
        SetTransientStatus("SAVE FAILED");
        LogOut(std::string("[CUSTOM_MENU] Failed to save settings via D button from ") + ScreenName(ActivePane()), true);
    }
}

float Clamp01(float v) {
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

float AnimT(DWORD startTick, float durationMs) {
    if (startTick == 0 || durationMs <= 0.0f) return 1.0f;
    const DWORD now = GetTickCount();
    return Clamp01(static_cast<float>(now - startTick) / durationMs);
}

float EfzCosEase(float t01) {
    const double degrees = 180.0 * Clamp01(t01);
    return static_cast<float>((1.0 - std::cos(degrees / kDegToRadDivisor)) * 0.5);
}

void StartOpenAnimation() {
    g_anim.openTick = GetTickCount();
    g_anim.paneTick = g_anim.openTick;
    g_anim.paneDir = 1;
}

void StartPaneAnimation(int dir) {
    g_anim.paneTick = GetTickCount();
    g_anim.paneDir = (dir < 0) ? -1 : 1;
}

float CurrentPaneOffsetX() {
    const float ease = EfzCosEase(AnimT(g_anim.paneTick, kPaneAnimMs));
    return (1.0f - ease) * kPaneSlidePx * static_cast<float>(g_anim.paneDir);
}

float CurrentOpenOffsetY() {
    const float ease = EfzCosEase(AnimT(g_anim.openTick, kOpenAnimMs));
    return (1.0f - ease) * -kOpenSlidePx;
}

enum class MainMode {
    Browse = 0,
    Adjust
};

struct MainState {
    int row = 0;
    int player = 0; // 0=P1, 1=P2
    MainMode mode = MainMode::Browse;
};
MainState g_main;

const char* MainModeName(MainMode mode) {
    switch (mode) {
        case MainMode::Browse: return "BROWSE";
        case MainMode::Adjust: return "ADJUST";
    }
    return "UNKNOWN";
}

struct EditState {
    bool active = false;
    int  rowIdx = -1;
    char buf[16] = "";
    int  caret = 0;
};
EditState g_edit;

// Mouse state used to decide whether "hover == focus". We only let mouse
// hover steal focus when the user has actually moved the mouse — otherwise
// a stationary cursor would constantly override keyboard selection.
struct MouseState {
    float lastX = -1.0f;
    float lastY = -1.0f;
    bool  movedThisFrame = false;
};
MouseState g_mouse;

void UpdateMouseState() {
    auto m = Input::GetMouse();
    g_mouse.movedThisFrame = false;
    if (!m.valid) { return; }
    if (g_mouse.lastX < 0.0f && g_mouse.lastY < 0.0f) {
        g_mouse.lastX = m.x; g_mouse.lastY = m.y;
        return;
    }
    const float dx = m.x - g_mouse.lastX;
    const float dy = m.y - g_mouse.lastY;
    if ((dx * dx + dy * dy) > 1.0f) {
        g_mouse.movedThisFrame = true;
        g_mouse.lastX = m.x;
        g_mouse.lastY = m.y;
    }
}

bool ShiftHeld() {
    if (!Input::IsGameWindowActive()) return false;
    return (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
}

// ===== Values rows descriptor =====
// Accessors on DisplayData so a single ValueRow can describe any scalar.
struct ValueRow {
    const char* label;
    int   (*getInt)(const DisplayData&);
    void  (*setInt)(DisplayData&, int);
    double(*getDbl)(const DisplayData&);
    void  (*setDbl)(DisplayData&, double);
    int    minI, maxI;
    double minD, maxD;
    int    stepSmall, stepBig;
    double stepSmallD, stepBigD;
    bool   isDouble;
    const char* fmt; // printf format for formatting only; nullptr = default
};

int  GetHp1(const DisplayData& d) { return d.hp1; }
int  GetHp2(const DisplayData& d) { return d.hp2; }
int  GetMt1(const DisplayData& d) { return d.meter1; }
int  GetMt2(const DisplayData& d) { return d.meter2; }
double GetRf1(const DisplayData& d) { return d.rf1; }
double GetRf2(const DisplayData& d) { return d.rf2; }
double GetX1(const DisplayData& d) { return d.x1; }
double GetY1(const DisplayData& d) { return d.y1; }
double GetX2(const DisplayData& d) { return d.x2; }
double GetY2(const DisplayData& d) { return d.y2; }

void SetHp1(DisplayData& d, int v) { d.hp1 = v; }
void SetHp2(DisplayData& d, int v) { d.hp2 = v; }
void SetMt1(DisplayData& d, int v) { d.meter1 = v; }
void SetMt2(DisplayData& d, int v) { d.meter2 = v; }
void SetRf1(DisplayData& d, double v) { d.rf1 = v; }
void SetRf2(DisplayData& d, double v) { d.rf2 = v; }
void SetX1(DisplayData& d, double v) { d.x1 = v; }
void SetY1(DisplayData& d, double v) { d.y1 = v; }
void SetX2(DisplayData& d, double v) { d.x2 = v; }
void SetY2(DisplayData& d, double v) { d.y2 = v; }

// Two-column Values layout.
//   Left column (PLAYER 1 VALUES):  focus 0..5  = HP, METER, RF, X, Y, IC
//   Right column (PLAYER 2 VALUES): focus 6..11 = HP, METER, RF, X, Y, IC
// Both columns share the same 6 row Y positions so rows visually align across.
constexpr int kP1NumericCount = 5;
constexpr int kICColorP1Idx   = 5;
constexpr int kP1Total        = 6;   // 5 numeric + 1 IC color
constexpr int kP2Start        = 6;
constexpr int kP2NumericCount = 5;
constexpr int kICColorP2Idx   = 11;
constexpr int kP2Total        = 6;
constexpr int kColRowCount    = 6;   // rows per column
constexpr int kDataRowCount   = 12;

ValueRow g_valueRowsAll[12] = {
    // P1 numeric rows
    {"HP",    GetHp1, SetHp1, nullptr, nullptr, 0, MAX_HP,     0.0, 0.0,      50,  500, 0.0,  0.0,   false, nullptr},
    {"METER", GetMt1, SetMt1, nullptr, nullptr, 0, MAX_METER,  0.0, 0.0,      50,  500, 0.0,  0.0,   false, nullptr},
    {"RF",    nullptr, nullptr, GetRf1, SetRf1, 0, 0,          0.0, (double)MAX_RF, 0, 0,   10.0, 100.0, true,  "%.0f"},
    {"X",     nullptr, nullptr, GetX1,  SetX1,  0, 0,          -2000.0, 2000.0, 0, 0, 1.0, 10.0,  true,  "%.1f"},
    {"Y",     nullptr, nullptr, GetY1,  SetY1,  0, 0,          -2000.0, 2000.0, 0, 0, 1.0, 10.0,  true,  "%.1f"},
    // P1 IC color (placeholder — handled specially in render/input)
    {"IC",    nullptr, nullptr, nullptr, nullptr, 0, 0,        0.0, 0.0,      0, 0, 0.0, 0.0,   false, nullptr},
    // P2 numeric rows
    {"HP",    GetHp2, SetHp2, nullptr, nullptr, 0, MAX_HP,     0.0, 0.0,      50,  500, 0.0,  0.0,   false, nullptr},
    {"METER", GetMt2, SetMt2, nullptr, nullptr, 0, MAX_METER,  0.0, 0.0,      50,  500, 0.0,  0.0,   false, nullptr},
    {"RF",    nullptr, nullptr, GetRf2, SetRf2, 0, 0,          0.0, (double)MAX_RF, 0, 0,   10.0, 100.0, true,  "%.0f"},
    {"X",     nullptr, nullptr, GetX2,  SetX2,  0, 0,          -2000.0, 2000.0, 0, 0, 1.0, 10.0,  true,  "%.1f"},
    {"Y",     nullptr, nullptr, GetY2,  SetY2,  0, 0,          -2000.0, 2000.0, 0, 0, 1.0, 10.0,  true,  "%.1f"},
    {"IC",    nullptr, nullptr, nullptr, nullptr, 0, 0,        0.0, 0.0,      0, 0, 0.0, 0.0,   false, nullptr},
};

inline bool RowIsICColor(int idx) { return idx == kICColorP1Idx || idx == kICColorP2Idx; }
inline bool RowIsNumeric(int idx) { return idx >= 0 && idx < kDataRowCount && !RowIsICColor(idx); }
inline bool RowIsPosition(int idx) {
    const int row = (idx >= kP2Start) ? (idx - kP2Start) : idx;
    return row == 3 || row == 4;
}
inline int RowPlayer(int idx) { return (idx >= kP2Start) ? 2 : 1; }
const char* ICColorName(bool blue) { return blue ? "BLUE" : "RED"; }
std::string DescribeFocus(int screen, int focus);

bool RowIsLocked(const GuiValueLocks::State& locks, int rowIdx) {
    if (rowIdx < 0 || rowIdx >= kDataRowCount) return false;
    if (RowIsPosition(rowIdx)) return false;
    if (locks.globalValuesLocked) return true;
    if (RowIsICColor(rowIdx)) {
        return (RowPlayer(rowIdx) == 1) ? locks.p1IcManaged : locks.p2IcManaged;
    }
    return false;
}

const char* RowLockReason(const GuiValueLocks::State& locks, int rowIdx) {
    if (!RowIsLocked(locks, rowIdx)) return "Unlocked";
    if (locks.globalValuesLocked) return GuiValueLocks::DescribeGlobalReason(locks.globalReason);
    if (RowIsICColor(rowIdx)) return GuiValueLocks::DescribeIcManagedReason(locks, RowPlayer(rowIdx));
    return "Locked";
}

std::string LockStatusText(const GuiValueLocks::State& locks, int rowIdx) {
    if (locks.globalValuesLocked) {
        switch (locks.globalReason) {
            case GuiValueLocks::GlobalReason::EngineF4:
                return "RF RECOVERY (F4) ACTIVE - HP/METER/RF/IC LOCKED";
            case GuiValueLocks::GlobalReason::EngineF5:
                return "AUTOMATIC RECOVERY (F5) ACTIVE - HP/METER/RF/IC LOCKED";
            case GuiValueLocks::GlobalReason::ContinuousRecovery:
                return "CONTINUOUS RECOVERY ACTIVE - HP/METER/RF/IC LOCKED";
            case GuiValueLocks::GlobalReason::PendingF4Ui:
                return "RF RECOVERY PENDING - HP/METER/RF/IC LOCKED";
            case GuiValueLocks::GlobalReason::None:
                break;
        }
    }

    if (RowIsICColor(rowIdx) && RowIsLocked(locks, rowIdx)) {
        return std::string(RowPlayer(rowIdx) == 1 ? "P1" : "P2") +
               " IC LOCKED - MANAGED BY " +
               RowLockReason(locks, rowIdx);
    }

    return "";
}

void LogLockedAction(const char* action, const GuiValueLocks::State& locks, int rowIdx) {
    if (!RowIsLocked(locks, rowIdx)) return;
    LogMenuDetail("%s blocked %s (%s)",
        action ? action : "Action",
        DescribeFocus(PANE_VALUES,rowIdx).c_str(),
        RowLockReason(locks, rowIdx));
}

// Focus helpers for the 2-column layout.
inline bool FocusInLeftCol(int f) { return f >= 0 && f < kP2Start; }
inline int  FocusColRow(int f)    { return FocusInLeftCol(f) ? f : (f - kP2Start); }
inline int  MainTotalFocusItems() { return kDataRowCount; }

int ComposeFocus(int row, int player) {
    if (row < 0) row = 0;
    if (row >= kColRowCount) row = kColRowCount - 1;
    if (player < 0) player = 0;
    if (player > 1) player = 1;
    return (player == 0) ? row : (kP2Start + row);
}

void DecodeFocus(int focus, int& row, int& player) {
    if (focus < 0) focus = 0;
    if (focus >= kDataRowCount) focus = kDataRowCount - 1;
    player = (focus >= kP2Start) ? 1 : 0;
    row = (player == 0) ? focus : (focus - kP2Start);
}

int CurFocus() {
    return ComposeFocus(g_main.row, g_main.player);
}

std::string DescribeFocus(int pane, int focus) {
    if (pane != PANE_VALUES) {
        return std::string("index=") + std::to_string(focus);
    }
    if (focus < 0 || focus >= kDataRowCount) {
        return "none";
    }

    int row = 0;
    int player = 0;
    DecodeFocus(focus, row, player);
    const char* side = (player == 0) ? "P1" : "P2";
    return std::string(side) + " " + g_valueRowsAll[focus].label;
}

void SetMainMode(MainMode newMode, const char* reason) {
    if (g_main.mode == newMode) return;

    LogMenuDetail("Mode %s -> %s (%s) focus=%s",
        MainModeName(g_main.mode),
        MainModeName(newMode),
        reason ? reason : "n/a",
        DescribeFocus(PANE_VALUES, CurFocus()).c_str());
    g_main.mode = newMode;
}

void ResetMainState(const char* reason) {
    g_main.row = 0;
    g_main.player = 0;
    g_main.mode = MainMode::Browse;
    g_shell.focusRegion = FocusRegion::Content;
    LogMenuDetail("Main reset (%s) focus=%s mode=%s",
        reason ? reason : "n/a",
        DescribeFocus(PANE_VALUES, CurFocus()).c_str(),
        MainModeName(g_main.mode));
}

void SetActiveTopTab(int newTop, const char* reason) {
    newTop = ClampTopTab(newTop);
    if (g_shell.activeTopTab == newTop) return;

    const int oldTop = g_shell.activeTopTab;
    SetMainMode(MainMode::Browse, "top-tab change");
    LogMenuDetail("TopTab %s -> %s (%s)",
        kTopTabs[g_shell.activeTopTab].label,
        kTopTabs[newTop].label,
        reason ? reason : "n/a");
    Screens::ResetSubmenus();
    g_shell.activeTopTab = newTop;
    if (g_shell.focusRegion == FocusRegion::SubTabs && !ActiveTopHasSubTabs()) {
        g_shell.focusRegion = FocusRegion::TopTabs;
    }
    Sound::PlayCursor();
    StartPaneAnimation((newTop >= oldTop) ? 1 : -1);
}

void SetActiveSubTab(int top, int newSub, const char* reason) {
    top = ClampTopTab(top);
    newSub = ClampSubTab(top, newSub);
    if (g_shell.subIdxPerTop[top] == newSub) return;
    const int oldSub = g_shell.subIdxPerTop[top];
    SetMainMode(MainMode::Browse, "sub-tab change");
    LogMenuDetail("SubTab %s/%s -> %s/%s (%s)",
        kTopTabs[top].label,
        kTopTabs[top].subs[g_shell.subIdxPerTop[top]].label,
        kTopTabs[top].label,
        kTopTabs[top].subs[newSub].label,
        reason ? reason : "n/a");
    Screens::ResetSubmenus();
    g_shell.subIdxPerTop[top] = newSub;
    Sound::PlayCursor();
    StartPaneAnimation((newSub >= oldSub) ? 1 : -1);
}

void SetFocus(int newFocus, const char* reason) {
    if (ActivePane() != PANE_VALUES) return;
    if (newFocus < 0 || newFocus >= MainTotalFocusItems()) return;

    const int focus = CurFocus();
    if (focus == newFocus) return;

    LogMenuDetail("Focus %s -> %s (%s)",
        DescribeFocus(PANE_VALUES, focus).c_str(),
        DescribeFocus(PANE_VALUES, newFocus).c_str(),
        reason ? reason : "n/a");

    int row = 0;
    int player = 0;
    DecodeFocus(newFocus, row, player);
    g_main.row = row;
    g_main.player = player;
    if (!reason || !strstr(reason, "mouse")) {
        Sound::PlayCursor();
    }
}

void FormatNumericRow(const ValueRow& r, char* buf, size_t bufSz) {
    const DisplayData& d = ImGuiGui::guiState.localData;
    const char* fmt = r.fmt ? r.fmt : (r.isDouble ? "%.0f" : "%d");
    if (r.isDouble) {
        _snprintf_s(buf, bufSz, _TRUNCATE, fmt, r.getDbl(d));
    } else {
        _snprintf_s(buf, bufSz, _TRUNCATE, fmt, r.getInt(d));
    }
}

std::string FormatRowValueForLog(int rowIdx) {
    if (RowIsICColor(rowIdx)) {
        const bool blue = (rowIdx == kICColorP1Idx)
            ? ImGuiGui::guiState.localData.p1BlueIC
            : ImGuiGui::guiState.localData.p2BlueIC;
        return ICColorName(blue);
    }
    if (!RowIsNumeric(rowIdx)) {
        return "<n/a>";
    }

    char buf[32];
    FormatNumericRow(g_valueRowsAll[rowIdx], buf, sizeof(buf));
    return buf;
}

void LogRowValueChange(int rowIdx, const char* reason) {
    if (rowIdx < 0 || rowIdx >= kDataRowCount) return;
    LogMenuDetail("%s %s = %s",
        reason ? reason : "Row change",
        DescribeFocus(PANE_VALUES,rowIdx).c_str(),
        FormatRowValueForLog(rowIdx).c_str());
}

void AdjustNumericRow(int rowIdx, int sign, bool forceBig = false) {
    if (!RowIsNumeric(rowIdx)) return;
    const ValueRow& r = g_valueRowsAll[rowIdx];
    DisplayData& d = ImGuiGui::guiState.localData;

    if (r.isDouble) {
        double v = r.getDbl(d);
        const double step = (forceBig || ShiftHeld()) ? r.stepBigD : r.stepSmallD;
        v += sign * step;
        if (v < r.minD) v = r.minD;
        if (v > r.maxD) v = r.maxD;
        r.setDbl(d, v);
    } else {
        int v = r.getInt(d);
        const int step = (forceBig || ShiftHeld()) ? r.stepBig : r.stepSmall;
        v += sign * step;
        if (v < r.minI) v = r.minI;
        if (v > r.maxI) v = r.maxI;
        r.setInt(d, v);
    }
}

// ===== Edit mode =====
void EnterEditMode(int rowIdx) {
    if (!RowIsNumeric(rowIdx)) return;
    g_edit.active = true;
    g_edit.rowIdx = rowIdx;
    FormatNumericRow(g_valueRowsAll[rowIdx], g_edit.buf, sizeof(g_edit.buf));
    g_edit.caret = (int)strlen(g_edit.buf);
    LogMenuDetail("Edit begin %s value=%s",
        DescribeFocus(PANE_VALUES,rowIdx).c_str(),
        g_edit.buf);
}

void CancelEditMode() {
    if (g_edit.active) {
        LogMenuDetail("Edit cancel %s buffer=%s",
            DescribeFocus(PANE_VALUES,g_edit.rowIdx).c_str(),
            g_edit.buf);
    }
    g_edit.active = false;
    g_edit.rowIdx = -1;
    g_edit.buf[0] = '\0';
    g_edit.caret = 0;
    SetMainMode(MainMode::Browse, "edit cancel");
}

void CommitEditMode() {
    if (!g_edit.active) return;
    const int rowIdx = g_edit.rowIdx;
    if (!RowIsNumeric(rowIdx)) { CancelEditMode(); return; }

    const ValueRow& r = g_valueRowsAll[rowIdx];
    DisplayData& d = ImGuiGui::guiState.localData;

    if (g_edit.buf[0] != '\0') {
        if (r.isDouble) {
            double v = atof(g_edit.buf);
            if (v < r.minD) v = r.minD;
            if (v > r.maxD) v = r.maxD;
            r.setDbl(d, v);
        } else {
            long v = strtol(g_edit.buf, nullptr, 10);
            if (v < r.minI) v = r.minI;
            if (v > r.maxI) v = r.maxI;
            r.setInt(d, (int)v);
        }
    }
    LogMenuDetail("Edit commit %s buffer=%s final=%s",
        DescribeFocus(PANE_VALUES,rowIdx).c_str(),
        g_edit.buf,
        FormatRowValueForLog(rowIdx).c_str());
    g_edit.active = false;
    g_edit.rowIdx = -1;
    g_edit.buf[0] = '\0';
    g_edit.caret = 0;
    SetMainMode(MainMode::Browse, "edit commit");
}

void ProcessEditTextInput() {
    if (!g_edit.active) return;
    ImGuiIO& io = ImGui::GetIO();
    const ValueRow& r = g_valueRowsAll[g_edit.rowIdx];
    const bool allowDot   = r.isDouble;
    const bool allowMinus = r.isDouble ? (r.minD < 0.0) : (r.minI < 0);

    for (int i = 0; i < io.InputQueueCharacters.Size; ++i) {
        const ImWchar c = io.InputQueueCharacters[i];
        const bool isDigit = (c >= '0' && c <= '9');
        const bool isDot   = (c == '.') && allowDot && (strchr(g_edit.buf, '.') == nullptr);
        const bool isMinus = (c == '-') && allowMinus && g_edit.caret == 0
                             && (strchr(g_edit.buf, '-') == nullptr);
        if (!isDigit && !isDot && !isMinus) continue;

        const int len = (int)strlen(g_edit.buf);
        if (len >= (int)sizeof(g_edit.buf) - 1) continue;

        for (int k = len; k >= g_edit.caret; --k) g_edit.buf[k + 1] = g_edit.buf[k];
        g_edit.buf[g_edit.caret] = (char)c;
        g_edit.caret++;
    }
    io.InputQueueCharacters.resize(0);
}

// ===== Menu hotkey =====
bool MenuKeyEdge() {
    if (!Input::IsGameWindowActive()) {
        // Mask the first foreground frame in case the user re-focuses EFZ
        // while the menu hotkey is still physically held.
        g_shell.prevMenuKeyDown = true;
        return false;
    }

    const auto& cfg = Config::GetSettings();
    const int menuKey = (cfg.configMenuKey > 0) ? cfg.configMenuKey : '3';
    const bool now = (GetAsyncKeyState(menuKey) & 0x8000) != 0;
    const bool edge = now && !g_shell.prevMenuKeyDown;
    g_shell.prevMenuKeyDown = now;
    if (edge) {
        LogMenuDetail("Menu hotkey edge key=%s", GetKeyName(menuKey).c_str());
    }
    return edge;
}

// ===== Layout rectangles — computed once per render so input and draw agree =====
// Row Y positions for the main screen content area.
struct MainLayout {
    ImVec2 panelTL;
    float  contentX;
    float  contentW;
    float  tabBarY;
    float  tabBarH;
    float  subBarY;                  // Y of the sub-tab strip (0 if none)
    float  subBarH;
    float  dataStartY;               // Y where the column headers begin
    float  rowY[kColRowCount];       // shared Y positions for the 6 rows (both columns)
    float  colLeftX;                 // left edge of P1 column
    float  colRightX;                // left edge of P2 column
    float  colW;                     // width of each column
    // Top-tab rects (hit-test boxes for TT_COUNT tabs)
    float  tabRectX[TT_COUNT];
    float  tabRectY;
    float  tabRectW[TT_COUNT];
    float  tabRectH;
    // Sub-tab rects (up to 8 subs per top; only first `subRectCount` valid)
    float  subRectX[8];
    float  subRectW[8];
    float  subRectY;
    float  subRectH;
    int    subRectCount;
};

void ApplyContentAnimation(MainLayout& L) {
    const float x = CurrentPaneOffsetX();
    const float y = CurrentOpenOffsetY();
    L.colLeftX += x;
    L.colRightX += x;
    L.dataStartY += y;
    for (float& rowY : L.rowY) {
        rowY += y;
    }
}

MainLayout ComputeMainLayout(float contentTopY) {
    using namespace Theme;
    MainLayout L{};
    L.panelTL   = PanelTopLeft();
    L.contentX  = L.panelTL.x;
    L.contentW  = kPanelW;
    L.tabBarY   = contentTopY;
    L.tabBarH   = kTabBarHeight;

    // Two-column split of the panel interior (used by VALUES pane).
    const float colGap = 14.0f;
    L.colW      = (kPanelW - kPanelPadX * 2.0f - colGap) * 0.5f;
    L.colLeftX  = L.panelTL.x + kPanelPadX;
    L.colRightX = L.colLeftX + L.colW + colGap;

    // Sub-tab strip sits just below the top bar when the active top tab
    // has more than one sub-tab.
    float y = contentTopY + L.tabBarH + 2.0f;
    const int top = ClampTopTab(g_shell.activeTopTab);
    if (kTopTabs[top].subCount > 1) {
        L.subBarY = y;
        L.subBarH = kTabBarHeight - 4.0f;
        y += L.subBarH + 4.0f;
    } else {
        L.subBarY = 0.0f;
        L.subBarH = 0.0f;
    }

    L.dataStartY = y;

    // Column header row (same Y for both columns). Only the VALUES pane
    // uses rowY[]; other panes compute layout via ScreenLayout.
    y += kRowHeight + kSectionPadY;
    for (int i = 0; i < kColRowCount; ++i) {
        L.rowY[i] = y;
        y += kRowHeight;
    }

    return L;
}

// Tab bar rects: top tabs + (optional) sub tabs for the active top.
void ComputeTabRects(MainLayout& L) {
    using namespace Theme;
    ImFont* bFont = Layout::BodyFont();
    const float bPx = bFont ? bFont->FontSize : 13.0f;

    // Top tabs
    float totalW = 0.0f;
    float widths[TT_COUNT];
    for (int i = 0; i < TT_COUNT; ++i) {
        widths[i] = Layout::MeasureTextW(bFont, bPx, kTopTabs[i].label);
        totalW += widths[i];
        if (i + 1 < TT_COUNT) totalW += kTabGapX;
    }
    float x = L.panelTL.x + (kPanelW - totalW) * 0.5f;
    if (x < L.panelTL.x + kPanelPadX) x = L.panelTL.x + kPanelPadX;
    const float textY = L.tabBarY + (kTabBarHeight - bPx) * 0.5f;
    L.tabRectY = textY - 2.0f;
    L.tabRectH = bPx + 6.0f;
    for (int i = 0; i < TT_COUNT; ++i) {
        L.tabRectX[i] = x;
        L.tabRectW[i] = widths[i];
        x += widths[i] + kTabGapX;
    }

    // Sub tabs (if any)
    L.subRectCount = 0;
    if (L.subBarH <= 0.0f) return;
    const int top = ClampTopTab(g_shell.activeTopTab);
    const TopTabInfo& tt = kTopTabs[top];
    L.subRectCount = tt.subCount;
    float subTotalW = 0.0f;
    float subWidths[8];
    for (int i = 0; i < tt.subCount && i < 8; ++i) {
        subWidths[i] = Layout::MeasureTextW(bFont, bPx, tt.subs[i].label);
        subTotalW += subWidths[i];
        if (i + 1 < tt.subCount) subTotalW += kTabGapX;
    }
    float sx = L.panelTL.x + (kPanelW - subTotalW) * 0.5f;
    if (sx < L.panelTL.x + kPanelPadX) sx = L.panelTL.x + kPanelPadX;
    const float subTextY = L.subBarY + (L.subBarH - bPx) * 0.5f;
    L.subRectY = subTextY - 2.0f;
    L.subRectH = bPx + 6.0f;
    for (int i = 0; i < tt.subCount && i < 8; ++i) {
        L.subRectX[i] = sx;
        L.subRectW[i] = subWidths[i];
        sx += subWidths[i] + kTabGapX;
    }
}

void SetActiveTopTab(int newTop, const char* reason);
void SetActiveSubTab(int top, int newSub, const char* reason);

bool HandleTabBarClick(const MainLayout& L) {
    if (Screens::IsPopupActive() || Screens::IsKeybindActive() || Screens::IsTextEditorActive()) return false;
    if (!Input::MouseLeftEdge()) return false;

    // Top tabs
    for (int i = 0; i < TT_COUNT; ++i) {
        if (!Input::MouseHovering(L.tabRectX[i] - 2.0f, L.tabRectY,
                                  L.tabRectW[i] + 4.0f, L.tabRectH)) {
            continue;
        }
        if (g_shell.activeTopTab != i) {
            CancelEditMode();
            SetActiveTopTab(i, "top tab click");
        } else {
            LogMenuDetail("Top tab click %s (already active)", kTopTabs[i].label);
        }
        return true;
    }

    // Sub tabs
    for (int i = 0; i < L.subRectCount; ++i) {
        if (!Input::MouseHovering(L.subRectX[i] - 2.0f, L.subRectY,
                                  L.subRectW[i] + 4.0f, L.subRectH)) {
            continue;
        }
        const int top = ClampTopTab(g_shell.activeTopTab);
        if (g_shell.subIdxPerTop[top] != i) {
            CancelEditMode();
            SetActiveSubTab(top, i, "sub tab click");
        }
        return true;
    }

    return false;
}

void MaybeMouseReturnFocusToContent(const MainLayout& L, bool tabClickConsumed) {
    if (g_shell.focusRegion == FocusRegion::Content) return;
    if (tabClickConsumed) return;
    if (!g_mouse.movedThisFrame && !Input::MouseLeftEdge()) return;

    const auto m = Input::GetMouse();
    if (!m.valid) return;
    if (m.y >= L.dataStartY && m.y <= Theme::PanelBottomRight().y - 40.0f) {
        SetFocusRegion(FocusRegion::Content, "mouse over content");
    }
}

bool HandleFocusedTabInput() {
    if (Screens::IsPopupActive() || Screens::IsKeybindActive() ||
        Screens::IsTextEditorActive()) {
        return false;
    }
    if (g_shell.focusRegion == FocusRegion::Content) return false;

    const bool navUp = Input::NavUp();
    const bool navDown = Input::NavDown();
    const bool navLeft = Input::NavLeft();
    const bool navRight = Input::NavRight();
    const bool activate = Input::Activate();
    const bool back = Input::Back();
    const bool any = navUp || navDown || navLeft || navRight || activate || back;
    if (!any) return false;

    if (back) {
        LogMenuDetail("Back pressed -> close from tab focus");
        ImGuiImpl::ToggleVisibility();
        return true;
    }

    if (g_shell.focusRegion == FocusRegion::TopTabs) {
        if (navLeft) {
            SetActiveTopTab((g_shell.activeTopTab + TT_COUNT - 1) % TT_COUNT, "top tab focus left");
        } else if (navRight) {
            SetActiveTopTab((g_shell.activeTopTab + 1) % TT_COUNT, "top tab focus right");
        } else if (navDown || activate) {
            SetFocusRegion(ActiveTopHasSubTabs() ? FocusRegion::SubTabs : FocusRegion::Content,
                           navDown ? "top tab focus down" : "top tab activate");
        }
        return true;
    }

    if (g_shell.focusRegion == FocusRegion::SubTabs) {
        const int top = ClampTopTab(g_shell.activeTopTab);
        const int subCount = kTopTabs[top].subCount;
        if (navLeft && subCount > 1) {
            SetActiveSubTab(top, (g_shell.subIdxPerTop[top] + subCount - 1) % subCount,
                            "sub tab focus left");
        } else if (navRight && subCount > 1) {
            SetActiveSubTab(top, (g_shell.subIdxPerTop[top] + 1) % subCount,
                            "sub tab focus right");
        } else if (navUp) {
            SetFocusRegion(FocusRegion::TopTabs, "sub tab focus up");
        } else if (navDown || activate) {
            SetFocusRegion(FocusRegion::Content,
                           navDown ? "sub tab focus down" : "sub tab activate");
        }
        return true;
    }

    return false;
}

// ===== Screen: Main (Values) =====
void RenderValueRow(ImDrawList* dl, int rowIdx, float x, float y, float w, bool focused, bool disabled) {
    using namespace Theme;

    if (RowIsICColor(rowIdx)) {
        const bool blue = (rowIdx == kICColorP1Idx)
            ? ImGuiGui::guiState.localData.p1BlueIC
            : ImGuiGui::guiState.localData.p2BlueIC;
        const char* choices[2] = { "RED", "BLUE" };
        Layout::DrawRowInlineChoices(
            dl, x, y, w,
            g_valueRowsAll[rowIdx].label,
            choices, 2, blue ? 1 : 0,
            focused, disabled);
        return;
    }

    char buf[32];
    if (g_edit.active && g_edit.rowIdx == rowIdx) {
        const bool caretOn = ((GetTickCount() / 500) & 1) == 0;
        char tmp[24];
        strncpy_s(tmp, sizeof(tmp), g_edit.buf, _TRUNCATE);
        const int len = (int)strlen(tmp);
        if (caretOn && len + 1 < (int)sizeof(tmp)) {
            tmp[len] = '_'; tmp[len + 1] = '\0';
        }
        _snprintf_s(buf, sizeof(buf), _TRUNCATE, "[%s]", tmp[0] ? tmp : " ");
    } else {
        FormatNumericRow(g_valueRowsAll[rowIdx], buf, sizeof(buf));
    }
    Layout::DrawRowNumber(
        dl, x, y, w,
        g_valueRowsAll[rowIdx].label, buf,
        focused, disabled);
}

void RenderMainScreen(const MainLayout& L, const GuiValueLocks::State& locks) {
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    if (!dl) return;

    using namespace Theme;

    const int focus = CurFocus();
    ImFont* bFont = Layout::BodyFont();
    const float bPx = bFont ? bFont->FontSize : 13.0f;
    const float activeRuleY = L.dataStartY + kSectionPadY + bPx + 2.0f;

    // Column headers share a Y.
    Layout::DrawHeader(dl, L.colLeftX,  L.dataStartY, L.colW, "PLAYER 1 VALUES");
    Layout::DrawHeader(dl, L.colRightX, L.dataStartY, L.colW, "PLAYER 2 VALUES");
    if (g_main.player == 0) {
        dl->AddLine(
            ImVec2(L.colLeftX + kRuleInsetX - kPanelPadX, activeRuleY),
            ImVec2(L.colLeftX + L.colW - kRuleInsetX - kPanelPadX, activeRuleY),
            kTextActive, 1.0f);
    } else {
        dl->AddLine(
            ImVec2(L.colRightX + kRuleInsetX - kPanelPadX, activeRuleY),
            ImVec2(L.colRightX + L.colW - kRuleInsetX - kPanelPadX, activeRuleY),
            kTextActive, 1.0f);
    }

    // Both columns draw 6 rows at the shared Y positions.
    for (int i = 0; i < kColRowCount; ++i) {
        const int p1RowIdx = i;
        const int p2RowIdx = kP2Start + i;
        RenderValueRow(dl, p1RowIdx, L.colLeftX,  L.rowY[i], L.colW,
                       focus == p1RowIdx, RowIsLocked(locks, p1RowIdx));
        RenderValueRow(dl, p2RowIdx, L.colRightX, L.rowY[i], L.colW,
                       focus == p2RowIdx, RowIsLocked(locks, p2RowIdx));
    }
}

// ===== Tab bar render =====
void RenderTabBar(const MainLayout& L) {
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    if (!dl) return;

    // Build top-tab label array on the fly.
    const char* labels[TT_COUNT];
    for (int i = 0; i < TT_COUNT; ++i) labels[i] = kTopTabs[i].label;

    Layout::DrawTabBar(
        dl, L.panelTL.x, L.tabBarY, Theme::kPanelW,
        labels, TT_COUNT,
        /*activeIdx=*/g_shell.activeTopTab,
        /*focusedIdx=*/g_shell.focusRegion == FocusRegion::TopTabs ? g_shell.activeTopTab : -1);

    // Sub-tab strip (only rendered when the current top tab has >1 sub).
    const int top = ClampTopTab(g_shell.activeTopTab);
    const TopTabInfo& tt = kTopTabs[top];
    if (tt.subCount > 1 && L.subBarH > 0.0f) {
        const char* subLabels[8];
        const int n = tt.subCount > 8 ? 8 : tt.subCount;
        for (int i = 0; i < n; ++i) subLabels[i] = tt.subs[i].label;
        Layout::DrawTabBar(
            dl, L.panelTL.x, L.subBarY, Theme::kPanelW,
            subLabels, n,
            /*activeIdx=*/g_shell.subIdxPerTop[top],
            /*focusedIdx=*/g_shell.focusRegion == FocusRegion::SubTabs ? g_shell.subIdxPerTop[top] : -1);
    }
}

// ===== Input: Main screen =====
void HandleMainScreenInput(const MainLayout& L, const GuiValueLocks::State& locks) {
    // Edit-mode branch: consume everything relevant to editing, ignore nav.
    if (g_edit.active) {
        if (RowIsLocked(locks, g_edit.rowIdx)) {
            LogMenuDetail("Edit cancelled %s (became locked by %s)",
                DescribeFocus(PANE_VALUES,g_edit.rowIdx).c_str(),
                RowLockReason(locks, g_edit.rowIdx));
            CancelEditMode();
            return;
        }

        ProcessEditTextInput();

        if (ImGui::IsKeyPressed(ImGuiKey_Backspace, true)) {
            if (g_edit.caret > 0) {
                const int len = (int)strlen(g_edit.buf);
                for (int k = g_edit.caret - 1; k < len; ++k) g_edit.buf[k] = g_edit.buf[k + 1];
                g_edit.caret--;
            }
        }

        if (Input::Activate()) { CommitEditMode(); ImGuiGui::ApplyImGuiSettings(); return; }
        if (Input::Back())     { CancelEditMode(); return; }

        (void)Input::NavUp(); (void)Input::NavDown();
        (void)Input::NavLeft(); (void)Input::NavRight();
        (void)Input::SwitchPlayer();
        return;
    }

    const bool keyboardOrPadEdge = Input::NavUp() || Input::NavDown() ||
                                   Input::NavLeft() || Input::NavRight() ||
                                   Input::Activate() || Input::Back() ||
                                   Input::SwitchPlayer();

    // Mouse hover -> focus (if mouse moved this frame). Keyboard/gamepad
    // edges win for the frame, so a stationary cursor cannot steal focus.
    if (!keyboardOrPadEdge && g_mouse.movedThisFrame) {
        for (int i = 0; i < kColRowCount; ++i) {
            if (Input::MouseHovering(L.colLeftX, L.rowY[i], L.colW, Theme::kRowHeight)) {
                SetMainMode(MainMode::Browse, "mouse hover");
                SetFocus(i, "mouse hover");
                break;
            }
            if (Input::MouseHovering(L.colRightX, L.rowY[i], L.colW, Theme::kRowHeight)) {
                SetMainMode(MainMode::Browse, "mouse hover");
                SetFocus(kP2Start + i, "mouse hover");
                break;
            }
        }
    }

    // Mouse click — route per-rect
    if (!keyboardOrPadEdge && Input::MouseLeftEdge()) {
        // Data rows in either column
        for (int i = 0; i < kColRowCount; ++i) {
            int hit = -1;
            if (Input::MouseHovering(L.colLeftX, L.rowY[i], L.colW, Theme::kRowHeight))
                hit = i;
            else if (Input::MouseHovering(L.colRightX, L.rowY[i], L.colW, Theme::kRowHeight))
                hit = kP2Start + i;
            if (hit < 0) continue;

            if (CurFocus() != hit) {
                SetMainMode(MainMode::Browse, "mouse click");
                SetFocus(hit, "mouse click");
                return;
            }

            if (RowIsLocked(locks, hit)) {
                LogLockedAction("Mouse edit", locks, hit);
                SetMainMode(MainMode::Browse, "locked row");
                return;
            }

            if (RowIsICColor(hit)) {
                if (hit == kICColorP1Idx) ImGuiGui::guiState.localData.p1BlueIC = !ImGuiGui::guiState.localData.p1BlueIC;
                else                       ImGuiGui::guiState.localData.p2BlueIC = !ImGuiGui::guiState.localData.p2BlueIC;
                SetMainMode(MainMode::Adjust, "mouse click toggle");
                Sound::PlayCursor();
                LogRowValueChange(hit, "Click toggle");
                ImGuiGui::ApplyImGuiSettings();
            } else if (RowIsNumeric(hit)) {
                SetMainMode(MainMode::Adjust, "mouse click edit");
                Sound::PlayDecision();
                EnterEditMode(hit);
            }
            return;
        }
    }

    // Keyboard/controller nav — explicit browse/adjust modes.
    //   Browse: Up/Down = row, Left/Right = player column, Enter = adjust.
    //   Adjust: Left/Right = small step (or set color), Up/Down = big step,
    //           Enter = text edit for numeric rows, Esc = back to browse.
    const int focus = CurFocus();
    const bool focusLocked = RowIsLocked(locks, focus);
    const bool navUp = Input::NavUp();
    const bool navDown = Input::NavDown();
    const bool navLeft = Input::NavLeft();
    const bool navRight = Input::NavRight();
    const bool activate = Input::Activate();
    const bool back = Input::Back();
    const bool switchPlayer = Input::SwitchPlayer();

    if (g_main.mode == MainMode::Adjust && focusLocked) {
        LogMenuDetail("Adjust cancelled %s (locked by %s)",
            DescribeFocus(PANE_VALUES,focus).c_str(),
            RowLockReason(locks, focus));
        SetMainMode(MainMode::Browse, "row locked");
    }

    if (switchPlayer) {
        SetFocus(ComposeFocus(g_main.row, 1 - g_main.player),
            (g_main.mode == MainMode::Browse) ? "d button switch player" : "d button switch player (adjust)");
        return;
    }

    if (g_main.mode == MainMode::Browse) {
        if (navUp && g_main.row > 0) {
            SetFocus(ComposeFocus(g_main.row - 1, g_main.player), "browse nav up");
        }
        if (navDown && g_main.row < kColRowCount - 1) {
            SetFocus(ComposeFocus(g_main.row + 1, g_main.player), "browse nav down");
        }
        if (navLeft && g_main.player > 0) {
            SetFocus(ComposeFocus(g_main.row, g_main.player - 1), "browse player left");
        }
        if (navRight && g_main.player < 1) {
            SetFocus(ComposeFocus(g_main.row, g_main.player + 1), "browse player right");
        }
        if (activate) {
            if (focusLocked) {
                LogLockedAction("Browse activate", locks, focus);
                return;
            }
            SetMainMode(MainMode::Adjust, "browse activate");
            Sound::PlayDecision();
            return;
        }
        if (back) {
            LogMenuDetail("Back pressed -> close from %s", ScreenName(ActivePane()));
            Sound::PlayDecision();
            ImGuiImpl::ToggleVisibility();
            return;
        }
        return;
    }

    if (RowIsICColor(focus)) {
        bool& blue = (focus == kICColorP1Idx)
            ? ImGuiGui::guiState.localData.p1BlueIC
            : ImGuiGui::guiState.localData.p2BlueIC;
        bool changed = false;
        if (navLeft && blue)  { blue = false; changed = true; }
        if (navRight && !blue){ blue = true;  changed = true; }
        if (activate)         { blue = !blue; changed = true; }
        if (changed) {
            LogRowValueChange(focus, "Adjust color");
            Sound::PlayCursor();
            ImGuiGui::ApplyImGuiSettings();
        }
    } else if (RowIsNumeric(focus)) {
        bool changed = false;
        bool usedBig = false;
        if (navLeft)  { AdjustNumericRow(focus, -1, false); changed = true; }
        if (navRight) { AdjustNumericRow(focus, +1, false); changed = true; }
        if (navUp)    { AdjustNumericRow(focus, +1, true);  changed = true; usedBig = true; }
        if (navDown)  { AdjustNumericRow(focus, -1, true);  changed = true; usedBig = true; }
        if (changed) {
            const char* reason = usedBig
                ? ((navLeft || navRight) ? "Adjust mixed" : "Adjust big")
                : (ShiftHeld() ? "Adjust small (shift)" : "Adjust small");
            LogRowValueChange(focus, reason);
            Sound::PlayCursor();
            ImGuiGui::ApplyImGuiSettings();
        }
        if (activate) {
            Sound::PlayDecision();
            EnterEditMode(focus);
            return;
        }
    }

    if (back) {
        SetMainMode(MainMode::Browse, "adjust back");
        Sound::PlayDecision();
    }
}

// ===== Top-level render/input dispatch =====
void MaybeRefreshOnOpen() {
    const bool visibleNow = ImGuiImpl::IsVisible();
    const bool customNow  = Config::GetSettings().useCustomMenu;

    if (visibleNow && !g_shell.menuWasVisible) {
        LogMenuTrace("MaybeRefreshOnOpen: visibility 0->1 (custom=%d), beginning open sequence", customNow ? 1 : 0);
        // Drop the on-disk .pal lookup cache so any palette files added since
        // last menu open are re-discovered. Cheap (clears 24*6 entries).
        CharacterHotswap::InvalidateCustomPaletteCache();
        SehInvokeStep("open: ImGuiGui::RefreshLocalData", &ImGuiGui::RefreshLocalData);
        LogMenuTrace("open: RefreshLocalData done");
        for (auto& f : g_shell.focusPerPane) f = 0;
        for (auto& s : g_shell.subIdxPerTop) s = 0;
        g_shell.activeTopTab = TT_MAIN;
        ResetMainState("menu open");
        CancelEditMode();
        g_shell.prevMenuKeyDown = true;      // mask the held-open press
        g_shell.keybindWasActive = false;
        g_mouse.lastX = g_mouse.lastY = -1.0f;
        g_mouse.movedThisFrame = false;
        StartOpenAnimation();
        LogMenuTrace("open: state reset done; calling Screens::ResetSubmenus");
        SehInvokeStep("open: Screens::ResetSubmenus", &Screens::ResetSubmenus);
        LogMenuTrace("open: calling Screens::ResetHotswapMenuSeed");
        SehInvokeStep("open: Screens::ResetHotswapMenuSeed", &Screens::ResetHotswapMenuSeed);
        // Snap physical-input edge detector so currently-held keys (the
        // menu-open press, a gamepad button still down from gameplay) do
        // not register as a rising edge on the first input-handling pass.
        LogMenuTrace("open: calling Input::ResetEdges");
        SehInvokeStep("open: Input::ResetEdges", &Input::ResetEdges);
        LogMenuTrace("open: open sequence complete");
        LogMenuDetail("Opened pane=%s focus=%s mode=%s",
            ScreenName(ActivePane()),
            DescribeFocus(ActivePane(), CurFocus()).c_str(),
            MainModeName(g_main.mode));
    } else if (visibleNow && customNow && !g_shell.lastWasCustom) {
        LogMenuTrace("MaybeRefreshOnOpen: switched to custom while visible");
        SehInvokeStep("switch-to-custom: RefreshLocalData", &ImGuiGui::RefreshLocalData);
        for (auto& f : g_shell.focusPerPane) f = 0;
        ResetMainState("switch to custom");
        CancelEditMode();
        g_shell.keybindWasActive = false;
        StartOpenAnimation();
        SehInvokeStep("switch-to-custom: ResetSubmenus", &Screens::ResetSubmenus);
        SehInvokeStep("switch-to-custom: ResetHotswapMenuSeed", &Screens::ResetHotswapMenuSeed);
        SehInvokeStep("switch-to-custom: Input::ResetEdges", &Input::ResetEdges);
        LogMenuDetail("Switched to custom menu while visible; pane=%s focus=%s mode=%s",
            ScreenName(ActivePane()),
            DescribeFocus(ActivePane(), CurFocus()).c_str(),
            MainModeName(g_main.mode));
    } else if (!visibleNow) {
        if (g_shell.menuWasVisible && g_shell.lastWasCustom) {
            LogMenuTrace("MaybeRefreshOnOpen: visibility 1->0, tearing down");
            LogMenuDetail("Closed custom menu");
        }
        CancelEditMode();
        SehInvokeStep("close: ResetSubmenus", &Screens::ResetSubmenus);
        SehInvokeStep("close: ResetHotswapMenuSeed", &Screens::ResetHotswapMenuSeed);
        SehInvokeStep("close: ResetTextEditor", &Screens::ResetTextEditor);
        g_shell.keybindWasActive = false;
    }

    g_shell.menuWasVisible = visibleNow;
    g_shell.lastWasCustom  = customNow;
}

} // namespace

void PrepareFrame() {
    if (!ImGui::GetCurrentContext()) return;
    if (!ImGuiImpl::IsVisible()) return;

    // This can invalidate/recreate the shared DX9 font texture. Keep it out
    // of Render(), because Render() runs after ImGui::NewFrame() and after
    // overlay text may already have queued draw commands using the old atlas.
    (void)Fonts::Rebuild(Config::GetSettings().uiScale, GetDpiScaleQuick());
}

void Render() {
    if (!ImGui::GetCurrentContext()) return;

    // Track open-edge so we can budget how many post-open frames get traced.
    static bool s_prevVisible = false;
    static unsigned long s_renderFrame = 0;
    static unsigned long s_framesSinceOpen = 0xFFFFFFFFul;
    const bool visibleBeforeMaybe = ImGuiImpl::IsVisible();
    if (visibleBeforeMaybe && !s_prevVisible) {
        s_framesSinceOpen = 0;
        LogMenuTrace("Render: visibility edge 0->1 detected at frame %lu", s_renderFrame + 1);
    } else if (!visibleBeforeMaybe) {
        s_framesSinceOpen = 0xFFFFFFFFul;
    } else if (s_framesSinceOpen != 0xFFFFFFFFul && s_framesSinceOpen < 32) {
        ++s_framesSinceOpen;
    }
    s_prevVisible = visibleBeforeMaybe;

    MaybeRefreshOnOpen();

    if (!ImGuiImpl::IsVisible()) return;

    ++s_renderFrame;
    const DWORD renderStartMs = GetTickCount();
    const bool traceThisFrame = (s_framesSinceOpen < 8);
    if (traceThisFrame) {
        LogMenuTrace("Render frame=%lu postOpen=%lu pane=%s topTab=%d sub=%d focusReg=%d",
            s_renderFrame, s_framesSinceOpen, ScreenName(ActivePane()),
            g_shell.activeTopTab,
            g_shell.subIdxPerTop[ClampTopTab(g_shell.activeTopTab)],
            (int)g_shell.focusRegion);
    }

    UpdateMouseState();

    // Global menu-key close — same hotkey used to open.
    if (!Screens::IsKeybindActive() && !Screens::IsTextEditorActive() && MenuKeyEdge()) {
        CancelEditMode();
        LogMenuDetail("Closing menu via menu hotkey from %s", ScreenName(ActivePane()));
        Sound::PlayDecision();
        ImGuiImpl::ToggleVisibility();
        return;
    }

    // Global tab cycling (top + sub). Blocked while editing a numeric value
    // OR while a dropdown popup is open (popup eats input on its own).
    if (!g_edit.active && !Screens::IsPopupActive() && !Screens::IsKeybindActive() &&
        !Screens::IsTextEditorActive()) {
        if (Input::TopTabPrev()) {
            CancelEditMode();
            SetActiveTopTab((g_shell.activeTopTab + TT_COUNT - 1) % TT_COUNT, "top-tab prev");
        } else if (Input::TopTabNext()) {
            CancelEditMode();
            SetActiveTopTab((g_shell.activeTopTab + 1) % TT_COUNT, "top-tab next");
        }

        const int top = ClampTopTab(g_shell.activeTopTab);
        const int subCount = kTopTabs[top].subCount;
        if (subCount > 1) {
            if (Input::SubTabPrev()) {
                CancelEditMode();
                SetActiveSubTab(top, (g_shell.subIdxPerTop[top] + subCount - 1) % subCount, "sub-tab prev");
            } else if (Input::SubTabNext()) {
                CancelEditMode();
                SetActiveSubTab(top, (g_shell.subIdxPerTop[top] + 1) % subCount, "sub-tab next");
            }
        }

        // 1..5 number-row keys jump directly to a top tab.
        const int n = Input::TopTabNumberEdge();
        if (n >= 1 && n <= TT_COUNT) {
            CancelEditMode();
            SetActiveTopTab(n - 1, "number-key jump");
        }
    }

    // Draw panel + title, capture content-area top-left.
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    if (!dl) return;
    const ImVec2 origin = Layout::DrawPanel(dl, "TRAINING SETTINGS");
    const GuiValueLocks::State valueLocks = GuiValueLocks::Compute(ImGuiGui::guiState.localData);

    MainLayout L = ComputeMainLayout(origin.y);
    ComputeTabRects(L);
    const bool tabClickConsumed = HandleTabBarClick(L);
    MaybeMouseReturnFocusToContent(L, tabClickConsumed);
    const bool tabFocusConsumed = HandleFocusedTabInput();
    if (!ImGuiImpl::IsVisible()) return;

    RenderTabBar(L);

    const int activePane = ActivePane();
    HandleSettingsQuickSave();
    MainLayout contentL = L;
    ApplyContentAnimation(contentL);

    if (activePane == PANE_VALUES) {
        bool valuesInputEnabled = !tabClickConsumed &&
                                  g_shell.focusRegion == FocusRegion::Content &&
                                  !tabFocusConsumed;
        if (valuesInputEnabled &&
            g_main.mode == MainMode::Browse &&
            g_main.row == 0 &&
            Input::NavUp()) {
            MoveFocusAboveContent("values top nav up");
            valuesInputEnabled = false;
        }
        if (valuesInputEnabled) HandleMainScreenInput(contentL, valueLocks);
        RenderMainScreen(contentL, valueLocks);
    } else {
        // Secondary list-based panes share the same render/input driver.
        Screens::ScreenLayout sl{};
        sl.panelX         = L.panelTL.x;
        sl.contentX       = L.panelTL.x + Theme::kPanelPadX;
        sl.contentW       = Theme::kPanelW - Theme::kPanelPadX * 2.0f;
        sl.contentTopY    = L.dataStartY;
        sl.contentBottomY = Theme::PanelBottomRight().y - 40.0f;
        sl.animOffsetX    = CurrentPaneOffsetX();
        sl.animOffsetY    = CurrentOpenOffsetY();
        sl.inputEnabled   = g_shell.focusRegion == FocusRegion::Content && !tabFocusConsumed;

        const DWORD mirrorStart = GetTickCount();
        if (traceThisFrame) {
            LogMenuTrace("RefreshSecondaryScreenMirrors begin");
        }
        Screens::RefreshSecondaryScreenMirrors();
        const DWORD mirrorMs = GetTickCount() - mirrorStart;
        if (traceThisFrame || mirrorMs >= 50) {
            LogMenuTrace("RefreshSecondaryScreenMirrors end ms=%lu",
                static_cast<unsigned long>(mirrorMs));
        }

        int& focus = g_shell.focusPerPane[activePane];
        Screens::ScrollState& scroll = g_shell.scrollPerPane[activePane];
        bool backEdge = false;

        if (traceThisFrame) {
            LogMenuTrace("Dispatch pane=%s (id=%d) focus=%d",
                ScreenName(activePane), activePane, focus);
        }

        const DWORD paneStartMs = GetTickCount();
        switch (activePane) {
            case PANE_OPPONENT:           SehTickPane("Opponent",        &Screens::TickOpponent,        dl, sl, focus, scroll, backEdge); break;
            case PANE_OPTIONS:            SehTickPane("Options",         &Screens::TickOptions,         dl, sl, focus, scroll, backEdge); break;
            case PANE_MENU:               SehTickPane("Menu",            &Screens::TickMenu,            dl, sl, focus, scroll, backEdge); break;
            case PANE_TRIGGERS:           SehTickPane("Triggers",        &Screens::TickTriggers,        dl, sl, focus, scroll, backEdge); break;
            case PANE_MACROS:             SehTickPane("Macros",          &Screens::TickMacros,          dl, sl, focus, scroll, backEdge); break;
            case PANE_CHARS:              SehTickPane("Chars",           &Screens::TickChars,           dl, sl, focus, scroll, backEdge); break;
            case PANE_SETTINGS_GENERAL:   SehTickPane("SettingsGeneral", &Screens::TickSettingsGeneral, dl, sl, focus, scroll, backEdge); break;
            case PANE_SETTINGS_HOTKEYS:   SehTickPane("SettingsHotkeys", &Screens::TickSettingsHotkeys, dl, sl, focus, scroll, backEdge); break;
            case PANE_SETTINGS_DEBUG:     SehTickPane("SettingsDebug",   &Screens::TickSettingsDebug,   dl, sl, focus, scroll, backEdge); break;
            case PANE_HELP_START:         SehTickPane("HelpStart",       &Screens::TickHelpStart,       dl, sl, focus, scroll, backEdge); break;
            case PANE_HELP_GUIDE:         SehTickPane("HelpGuide",       &Screens::TickHelpGuide,       dl, sl, focus, scroll, backEdge); break;
            case PANE_HELP_RESOURCES:     SehTickPane("HelpResources",   &Screens::TickHelpResources,   dl, sl, focus, scroll, backEdge); break;
            case PANE_HELP_ABOUT:         SehTickPane("HelpAbout",       &Screens::TickHelpAbout,       dl, sl, focus, scroll, backEdge); break;
            default:
                LogMenuTrace("Dispatch: unknown pane %d", activePane);
                break;
        }
        const DWORD paneMs = GetTickCount() - paneStartMs;
        static DWORD s_lastSlowPaneLog = 0;
        const DWORD paneLogNow = GetTickCount();
        if (paneMs >= 50 && (s_lastSlowPaneLog == 0 || (paneLogNow - s_lastSlowPaneLog) >= 1000)) {
            s_lastSlowPaneLog = paneLogNow;
            LogMenuTiming("Pane %s tick/render took %lums focus=%d submenu=%d popup=%d keybind=%d textEditor=%d",
                ScreenName(activePane),
                static_cast<unsigned long>(paneMs),
                focus,
                Screens::IsSubmenuActive() ? 1 : 0,
                Screens::IsPopupActive() ? 1 : 0,
                Screens::IsKeybindActive() ? 1 : 0,
                Screens::IsTextEditorActive() ? 1 : 0);
        }
        if (traceThisFrame) {
            LogMenuTrace("Dispatch pane=%s done backEdge=%d", ScreenName(activePane), backEdge ? 1 : 0);
        }
        if (g_shell.focusRegion == FocusRegion::Content && Screens::ConsumeFocusAboveRequest()) {
            MoveFocusAboveContent("list top nav up");
        }

        // Draw modal overlays last so they sit on top of rows.
        Screens::TickPopupIfOpen(dl, sl);
        Screens::TickKeybindIfActive(dl, sl);
        if (g_shell.keybindWasActive && !Screens::IsKeybindActive()) {
            g_shell.prevMenuKeyDown = true;
        }
        g_shell.keybindWasActive = Screens::IsKeybindActive();

        if (!tabClickConsumed && g_shell.focusRegion == FocusRegion::Content && backEdge) {
            LogMenuDetail("Back pressed -> close from %s", ScreenName(activePane));
            ImGuiImpl::ToggleVisibility();
            return;
        }
    }

    // Hint line (bottom of panel)
    ImFont* bFont = Layout::BodyFont();
    const float bPx = bFont ? bFont->FontSize : 13.0f;
    const char* hint;
    std::string statusText;
    if (const char* transientStatus = CurrentTransientStatus()) {
        statusText = transientStatus;
    }
    if (g_edit.active) {
        hint = "0-9 / . / BACKSPACE    ENTER COMMIT    ESC CANCEL";
    } else if (Screens::IsKeybindActive()) {
        hint = Screens::IsGamepadKeybindActive()
            ? "RELEASE INPUTS   PRESS BUTTON   MENU CANCEL   DEL DISABLE"
            : "RELEASE KEYS THEN PRESS NEW HOTKEY   ESC CANCEL";
    } else if (Screens::IsPopupActive()) {
        hint = "UP/DOWN MOVE   ENTER SELECT   ESC CANCEL";
    } else if (Screens::IsTextEditorActive()) {
        hint = "EDIT MACRO TEXT   CTRL+V PASTE   APPLY/DONE BUTTONS   ESC CLOSE";
    } else if (g_shell.focusRegion == FocusRegion::TopTabs) {
        hint = SettingsTabActive()
            ? "L/R CHANGE TAB   DOWN ENTER SUBTABS   D SAVE   ESC CLOSE"
            : "L/R CHANGE TAB   DOWN ENTER SUBTABS   ESC CLOSE";
    } else if (g_shell.focusRegion == FocusRegion::SubTabs) {
        hint = SettingsTabActive()
            ? "L/R CHANGE SUBTAB   UP TABS   DOWN ENTER OPTIONS   D SAVE   ESC CLOSE"
            : "L/R CHANGE SUBTAB   UP TABS   DOWN ENTER OPTIONS   ESC CLOSE";
    } else if (Screens::IsSubmenuActive()) {
        hint = SettingsTabActive()
            ? "UP/DOWN MOVE   ENTER SELECT   D SAVE   ESC BACK"
            : "UP/DOWN MOVE   ENTER SELECT   ESC BACK";
    } else if (ActivePane() == PANE_VALUES) {
        const int focus = CurFocus();
        const bool focusLocked = RowIsLocked(valueLocks, focus);
        statusText = LockStatusText(valueLocks, focus);
        if (g_main.mode == MainMode::Browse) {
            if (focusLocked) {
                hint = "UP/DOWN ROW   L/R OR D SWITCH PLAYER   ENTER LOCKED   PGUP/PGDN TABS";
            } else {
                hint = "UP/DOWN ROW   L/R OR D SWITCH PLAYER   ENTER ADJUST   PGUP/PGDN TABS";
            }
        } else if (focusLocked) {
            hint = "THIS ROW IS LOCKED   D SWITCH PLAYER   ESC BACK";
        } else if (RowIsICColor(focus)) {
            hint = "L/R SET COLOR   D SWITCH PLAYER   ENTER TOGGLE   ESC BACK";
        } else {
            hint = "L/R SMALL   U/D BIG   D SWITCH PLAYER   ENTER TYPE   ESC BACK";
        }
    } else if (SettingsTabActive()) {
        hint = "U/D MOVE   L/R ADJUST   D SAVE   ENTER PICK   PGUP/PGDN TOP";
    } else {
        hint = "U/D MOVE   L/R ADJUST   SHIFT+L/R 2ND   ENTER PICK   PGUP/PGDN TOP";
    }

    const float hintBoxX = L.panelTL.x + Theme::kPanelPadX;
    const float hintBoxW = Theme::kPanelW - Theme::kPanelPadX * 2.0f;
    const float hintBoxH = statusText.empty() ? 26.0f : 40.0f;
    const float hintBoxY = Theme::PanelBottomRight().y - hintBoxH - 6.0f;
    dl->AddRectFilled(
        ImVec2(hintBoxX, hintBoxY),
        ImVec2(hintBoxX + hintBoxW, hintBoxY + hintBoxH),
        Theme::kStripStrong);
    dl->AddRect(
        ImVec2(hintBoxX + 0.5f, hintBoxY + 0.5f),
        ImVec2(hintBoxX + hintBoxW - 0.5f, hintBoxY + hintBoxH - 0.5f),
        Theme::kRule, 0.0f, 0, 1.0f);

    if (!statusText.empty()) {
        const float statusY = hintBoxY + 5.0f;
        const float sw = Layout::MeasureTextW(bFont, bPx, statusText.c_str());
        float statusX = L.panelTL.x + (Theme::kPanelW - sw) * 0.5f;
        if (statusX < L.panelTL.x + Theme::kPanelPadX) statusX = L.panelTL.x + Theme::kPanelPadX;
        Layout::DrawString(dl, bFont, bPx, statusX, statusY, Theme::kTextStatus, statusText.c_str());
    }
    const float hintY = statusText.empty()
        ? hintBoxY + (hintBoxH - bPx) * 0.5f
        : hintBoxY + hintBoxH - bPx - 5.0f;
    const float hw = Layout::MeasureTextW(bFont, bPx, hint);
    float hintX = L.panelTL.x + (Theme::kPanelW - hw) * 0.5f;
    if (hintX < L.panelTL.x + Theme::kPanelPadX) hintX = L.panelTL.x + Theme::kPanelPadX;
    Layout::DrawString(dl, bFont, bPx, hintX, hintY, Theme::kTextInactive, hint);

    const DWORD renderMs = GetTickCount() - renderStartMs;
    static DWORD s_lastSlowRenderLog = 0;
    const DWORD renderLogNow = GetTickCount();
    if (renderMs >= 75 && (s_lastSlowRenderLog == 0 || (renderLogNow - s_lastSlowRenderLog) >= 1000)) {
        s_lastSlowRenderLog = renderLogNow;
        LogMenuTiming("Render frame took %lums pane=%s topTab=%d sub=%d focusRegion=%d popup=%d submenu=%d",
            static_cast<unsigned long>(renderMs),
            ScreenName(ActivePane()),
            g_shell.activeTopTab,
            g_shell.subIdxPerTop[ClampTopTab(g_shell.activeTopTab)],
            static_cast<int>(g_shell.focusRegion),
            Screens::IsPopupActive() ? 1 : 0,
            Screens::IsSubmenuActive() ? 1 : 0);
    }
}

} // namespace CustomMenu
