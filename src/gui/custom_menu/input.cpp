#include "../include/gui/custom_menu/input.h"
#include "../include/core/logger.h"
#include "../include/utils/config.h"
#include "../include/utils/utilities.h"     // detectedBindings
#include "../include/utils/xinput_shim.h"   // XInputShim::GetCachedState
#include "../3rdparty/imgui/imgui.h"

#include <windows.h>
#include <Xinput.h>
#include <atomic>
#include <cstdarg>
#include <string>

namespace CustomMenu::Input {

bool IsGameWindowActive() {
    return g_efzWindowActive.load(std::memory_order_relaxed);
}

namespace {

// --- Physical state tracker --------------------------------------------------
// We deliberately do NOT read ImGuiKey_Enter / ImGuiKey_GamepadFaceDown / any
// of the imgui_impl-aliased keys because those are fed each frame from
// ReadMenuGameplayInputs(), which sees the game's player-input bitmask.
// Auto-actions, macros, and automation write to those bytes, so relying on
// ImGui's aliased state causes spurious rising edges that would, for example,
// silently enter number-edit mode or swap nav focus.
//
// Instead we poll the physical inputs every frame:
//   - Arrow keys + Enter/Escape via GetAsyncKeyState
//   - User's configured game direction + A/B/C/D keys (from detectedBindings)
//     via GetAsyncKeyState on the same VK they resolved to
//   - XInput dpad + A/B/Y buttons across all connected pads, via XInputShim's
//     cached state (already refreshed at the start of each EndScene by
//     imgui_impl's PreNewFrameInputs)
//
// Edges are computed against the previous frame's combined state. Frame-
// cached, so multiple queries in a single frame are consistent and cheap.

struct CurState {
    bool up       = false;
    bool down     = false;
    bool left     = false;
    bool right    = false;
    bool activate = false;   // any Enter / A source
    bool back     = false;   // any Esc / B source
    bool switchPlayer = false; // EFZ D / Y source
    bool topTabPrev = false;
    bool topTabNext = false;
    bool subTabPrev = false; // LT trigger / '['
    bool subTabNext = false; // RT trigger / ']'
};

struct Edges {
    bool up, down, left, right, activate, back, switchPlayer;
    bool topTabPrev, topTabNext;
    bool subTabPrev, subTabNext;
};

CurState g_prev{};
Edges    g_cachedEdges{};
unsigned int g_cachedFrame = ~0u;

constexpr int kPseudoLeftTriggerMask = 0x10000;
constexpr int kPseudoRightTriggerMask = 0x20000;
constexpr BYTE kTriggerThreshold = 30;

void LogInputDetail(const char* fmt, ...) {
    if (!detailedLogging.load()) return;

    char buf[512];
    va_list args;
    va_start(args, fmt);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, args);
    va_end(args);

    LogOut(std::string("[CUSTOM_MENU][INPUT] ") + buf, true);
}

bool VkDown(int vk) {
    if (vk <= 0) return false;
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

bool InputAllowed() {
    return IsGameWindowActive();
}

bool PadBindingDown(const XINPUT_STATE& state, int mask) {
    if (mask < 0) return false;
    if (mask == kPseudoLeftTriggerMask) return state.Gamepad.bLeftTrigger > kTriggerThreshold;
    if (mask == kPseudoRightTriggerMask) return state.Gamepad.bRightTrigger > kTriggerThreshold;
    return (state.Gamepad.wButtons & static_cast<WORD>(mask & 0xFFFF)) != 0;
}

template <typename Fn>
void ForEachRelevantPadState(const Fn& fn) {
    const int controllerIndex = Config::GetSettings().controllerIndex;
    if (controllerIndex >= 0 && controllerIndex <= 3) {
        const XINPUT_STATE* state = XInputShim::GetCachedState(controllerIndex);
        if (state) {
            fn(*state);
        }
        return;
    }

    for (int i = 0; i < 4; ++i) {
        const XINPUT_STATE* state = XInputShim::GetCachedState(i);
        if (!state) continue;
        fn(*state);
    }
}

bool AnyRelevantPadBindingDown(int mask) {
    bool down = false;
    ForEachRelevantPadState([&](const XINPUT_STATE& state) {
        if (!down && PadBindingDown(state, mask)) {
            down = true;
        }
    });
    return down;
}

CurState SampleCurrent() {
    CurState cur;
    if (!InputAllowed()) return cur;
    const auto& cfg = Config::GetSettings();

    // Keyboard — fixed VKs
    if (VkDown(VK_UP))     cur.up       = true;
    if (VkDown(VK_DOWN))   cur.down     = true;
    if (VkDown(VK_LEFT))   cur.left     = true;
    if (VkDown(VK_RIGHT))  cur.right    = true;
    if (VkDown(VK_RETURN) || VkDown(VK_SEPARATOR)) cur.activate = true;
    if (VkDown(VK_ESCAPE)) cur.back     = true;
    if (VkDown(VK_PRIOR))  cur.topTabPrev = true;   // Page Up
    if (VkDown(VK_NEXT))   cur.topTabNext = true;   // Page Down
    if (VkDown(VK_OEM_4))  cur.subTabPrev = true;   // '[' { bracket
    if (VkDown(VK_OEM_6))  cur.subTabNext = true;   // ']' } bracket

    // User's configured keyboard direction keys. These fire in *parallel* with
    // the arrow keys — an EFZ player who uses arrow keys for gameplay has
    // both paths overlap, which is fine for edge detection.
    if (detectedBindings.directionsDetected) {
        if (VkDown(detectedBindings.upKey))    cur.up    = true;
        if (VkDown(detectedBindings.downKey))  cur.down  = true;
        if (VkDown(detectedBindings.leftKey))  cur.left  = true;
        if (VkDown(detectedBindings.rightKey)) cur.right = true;
    }
    // User's configured attack keys: A/C = activate, B = back, D = switch player.
    if (detectedBindings.attacksDetected) {
        if (VkDown(detectedBindings.aButton)) cur.activate = true;
        if (VkDown(detectedBindings.cButton)) cur.activate = true;
        if (VkDown(detectedBindings.bButton)) cur.back     = true;
        if (VkDown(detectedBindings.dButton)) cur.switchPlayer = true;
    }

    // Controller nav uses the selected controller when configured, otherwise
    // any cached XInput or DirectInput-synthetic pad may drive the menu.
    ForEachRelevantPadState([&](const XINPUT_STATE& state) {
        const WORD b = state.Gamepad.wButtons;
        if (b & XINPUT_GAMEPAD_DPAD_UP)    cur.up       = true;
        if (b & XINPUT_GAMEPAD_DPAD_DOWN)  cur.down     = true;
        if (b & XINPUT_GAMEPAD_DPAD_LEFT)  cur.left     = true;
        if (b & XINPUT_GAMEPAD_DPAD_RIGHT) cur.right    = true;
        if (b & XINPUT_GAMEPAD_A)          cur.activate = true;
        if (b & XINPUT_GAMEPAD_B)          cur.back     = true;
        if (b & XINPUT_GAMEPAD_Y)          cur.switchPlayer = true;
    });

    cur.topTabPrev = cur.topTabPrev || AnyRelevantPadBindingDown(cfg.gpUiTopTabPrev);
    cur.topTabNext = cur.topTabNext || AnyRelevantPadBindingDown(cfg.gpUiTopTabNext);
    cur.subTabPrev = cur.subTabPrev || AnyRelevantPadBindingDown(cfg.gpUiSubTabPrev);
    cur.subTabNext = cur.subTabNext || AnyRelevantPadBindingDown(cfg.gpUiSubTabNext);

    return cur;
}

// Hold-repeat for vertical / horizontal nav. After an initial press fires
// once, repeats every `kRepeatIntervalFrames` once `kRepeatDelayFrames` has
// elapsed since the last press transition. Tuned around 64 visual frames per
// second so 18 / 4 ≈ 280 ms initial delay then ~62 ms between repeats.
constexpr unsigned int kRepeatDelayFrames    = 18;
constexpr unsigned int kRepeatIntervalFrames = 4;

struct HoldState {
    unsigned int heldFrames = 0;
    unsigned int framesSinceFire = 0;
};
HoldState g_holdUp;
HoldState g_holdDown;
HoldState g_holdLeft;
HoldState g_holdRight;

bool TickHoldRepeat(HoldState& s, bool nowPressed, bool risingEdge) {
    if (!nowPressed) {
        s.heldFrames = 0;
        s.framesSinceFire = 0;
        return false;
    }
    if (risingEdge) {
        // Initial tap: fire immediately, restart timers.
        s.heldFrames = 1;
        s.framesSinceFire = 0;
        return true;
    }
    s.heldFrames++;
    if (s.heldFrames < kRepeatDelayFrames) {
        s.framesSinceFire++;
        return false;
    }
    s.framesSinceFire++;
    if (s.framesSinceFire >= kRepeatIntervalFrames) {
        s.framesSinceFire = 0;
        return true;
    }
    return false;
}

const Edges& SampleEdges() {
    const unsigned int f = ImGui::GetFrameCount();
    if (f == g_cachedFrame) return g_cachedEdges;
    g_cachedFrame = f;

    CurState cur = SampleCurrent();

    const bool risingUp    = cur.up    && !g_prev.up;
    const bool risingDown  = cur.down  && !g_prev.down;
    const bool risingLeft  = cur.left  && !g_prev.left;
    const bool risingRight = cur.right && !g_prev.right;

    g_cachedEdges.up       = TickHoldRepeat(g_holdUp,    cur.up,    risingUp);
    g_cachedEdges.down     = TickHoldRepeat(g_holdDown,  cur.down,  risingDown);
    g_cachedEdges.left     = TickHoldRepeat(g_holdLeft,  cur.left,  risingLeft);
    g_cachedEdges.right    = TickHoldRepeat(g_holdRight, cur.right, risingRight);
    g_cachedEdges.activate = cur.activate && !g_prev.activate;
    g_cachedEdges.back     = cur.back     && !g_prev.back;
    g_cachedEdges.switchPlayer = cur.switchPlayer && !g_prev.switchPlayer;
    g_cachedEdges.topTabPrev = cur.topTabPrev && !g_prev.topTabPrev;
    g_cachedEdges.topTabNext = cur.topTabNext && !g_prev.topTabNext;
    g_cachedEdges.subTabPrev = cur.subTabPrev && !g_prev.subTabPrev;
    g_cachedEdges.subTabNext = cur.subTabNext && !g_prev.subTabNext;

    if (g_cachedEdges.up || g_cachedEdges.down || g_cachedEdges.left ||
        g_cachedEdges.right || g_cachedEdges.activate || g_cachedEdges.back ||
        g_cachedEdges.switchPlayer || g_cachedEdges.topTabPrev ||
        g_cachedEdges.topTabNext || g_cachedEdges.subTabPrev ||
        g_cachedEdges.subTabNext) {
        LogInputDetail(
            "Edge U=%d D=%d L=%d R=%d A=%d B=%d SW=%d TP=%d TN=%d SP=%d SN=%d | held U=%d D=%d L=%d R=%d A=%d B=%d SW=%d TP=%d TN=%d SP=%d SN=%d",
            g_cachedEdges.up ? 1 : 0,
            g_cachedEdges.down ? 1 : 0,
            g_cachedEdges.left ? 1 : 0,
            g_cachedEdges.right ? 1 : 0,
            g_cachedEdges.activate ? 1 : 0,
            g_cachedEdges.back ? 1 : 0,
            g_cachedEdges.switchPlayer ? 1 : 0,
            g_cachedEdges.topTabPrev ? 1 : 0,
            g_cachedEdges.topTabNext ? 1 : 0,
            g_cachedEdges.subTabPrev ? 1 : 0,
            g_cachedEdges.subTabNext ? 1 : 0,
            cur.up ? 1 : 0,
            cur.down ? 1 : 0,
            cur.left ? 1 : 0,
            cur.right ? 1 : 0,
            cur.activate ? 1 : 0,
            cur.back ? 1 : 0,
            cur.switchPlayer ? 1 : 0,
            cur.topTabPrev ? 1 : 0,
            cur.topTabNext ? 1 : 0,
            cur.subTabPrev ? 1 : 0,
            cur.subTabNext ? 1 : 0);
    }

    g_prev = cur;
    return g_cachedEdges;
}

} // namespace

void ResetEdges() {
    // Snapshot current physical state into g_prev so any currently-held keys
    // register as "already down" and no rising edge is seen this frame.
    g_prev = SampleCurrent();
    g_cachedEdges = Edges{};   // zero: no edges on the open-frame
    g_cachedFrame = ImGui::GetFrameCount();
    // Wipe the hold-repeat windows so a key already held when the menu opens
    // doesn't trigger continuous nav until it's released and re-pressed.
    g_holdUp = HoldState{};
    g_holdDown = HoldState{};
    g_holdLeft = HoldState{};
    g_holdRight = HoldState{};

    LogInputDetail(
        "ResetEdges held U=%d D=%d L=%d R=%d A=%d B=%d SW=%d | bindings Up=%s Down=%s Left=%s Right=%s A=%s B=%s C=%s D=%s",
        g_prev.up ? 1 : 0,
        g_prev.down ? 1 : 0,
        g_prev.left ? 1 : 0,
        g_prev.right ? 1 : 0,
        g_prev.activate ? 1 : 0,
        g_prev.back ? 1 : 0,
        g_prev.switchPlayer ? 1 : 0,
        detectedBindings.directionsDetected ? GetKeyName(detectedBindings.upKey).c_str() : "<none>",
        detectedBindings.directionsDetected ? GetKeyName(detectedBindings.downKey).c_str() : "<none>",
        detectedBindings.directionsDetected ? GetKeyName(detectedBindings.leftKey).c_str() : "<none>",
        detectedBindings.directionsDetected ? GetKeyName(detectedBindings.rightKey).c_str() : "<none>",
        detectedBindings.attacksDetected ? GetKeyName(detectedBindings.aButton).c_str() : "<none>",
        detectedBindings.attacksDetected ? GetKeyName(detectedBindings.bButton).c_str() : "<none>",
        detectedBindings.attacksDetected ? GetKeyName(detectedBindings.cButton).c_str() : "<none>",
        detectedBindings.attacksDetected ? GetKeyName(detectedBindings.dButton).c_str() : "<none>");
}

bool NavUp()    { return SampleEdges().up;    }
bool NavDown()  { return SampleEdges().down;  }
bool NavLeft()  { return SampleEdges().left;  }
bool NavRight() { return SampleEdges().right; }
bool Activate() { return SampleEdges().activate; }
bool Back()     { return SampleEdges().back;  }
bool SwitchPlayer() { return SampleEdges().switchPlayer; }

bool TopTabPrev() {
    return SampleEdges().topTabPrev;
}

bool TopTabNext() {
    return SampleEdges().topTabNext;
}

bool SubTabPrev() { return SampleEdges().subTabPrev; }
bool SubTabNext() { return SampleEdges().subTabNext; }

// Number-row top-tab jump. We track our own bit-15 edges via GetAsyncKeyState
// for VK '1'..'9' since ImGui may swallow these during text entry. The shell
// should only query this when no numeric-edit is active.
int TopTabNumberEdge() {
    static bool s_prev[9] = {false,false,false,false,false,false,false,false,false};
    if (!InputAllowed()) {
        for (int i = 0; i < 9; ++i) s_prev[i] = false;
        return 0;
    }

    int hitIdx = 0;
    for (int i = 0; i < 9; ++i) {
        const int vk = '1' + i;
        const bool now = (GetAsyncKeyState(vk) & 0x8000) != 0;
        const bool edge = now && !s_prev[i];
        s_prev[i] = now;
        if (edge && hitIdx == 0) hitIdx = i + 1;
    }
    if (hitIdx > 0) {
        LogInputDetail("TopTabNumberEdge %d", hitIdx);
    }
    return hitIdx;
}

// ===== Mouse (unchanged — imgui_impl's PreNewFrameInputs already remaps the
// OS cursor into 640x480 virtual canvas space) =====

MousePos GetMouse() {
    MousePos m{0.0f, 0.0f, false};
    ImGuiIO& io = ImGui::GetIO();
    if (io.MousePos.x <= -1.0e6f || io.MousePos.y <= -1.0e6f) return m;
    m.x = io.MousePos.x;
    m.y = io.MousePos.y;
    m.valid = true;
    return m;
}

bool MouseHovering(float x, float y, float w, float h) {
    auto m = GetMouse();
    if (!m.valid) return false;
    return m.x >= x && m.x <= x + w && m.y >= y && m.y <= y + h;
}

bool MouseClickedIn(float x, float y, float w, float h) {
    return MouseHovering(x, y, w, h) && ImGui::IsMouseClicked(ImGuiMouseButton_Left, false);
}

bool MouseRightClickedIn(float x, float y, float w, float h) {
    return MouseHovering(x, y, w, h) && ImGui::IsMouseClicked(ImGuiMouseButton_Right, false);
}

bool MouseLeftEdge() {
    return ImGui::IsMouseClicked(ImGuiMouseButton_Left, false);
}

} // namespace CustomMenu::Input
