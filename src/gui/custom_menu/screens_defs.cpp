// Per-screen row definitions for the custom menu's secondary screens.
// The generic list-screen infrastructure lives in screens.cpp; this file
// only declares row arrays and a handful of lambdas/callbacks wired to the
// underlying atomics and config settings.

#include "../include/gui/custom_menu/screens.h"
#include "../include/gui/custom_menu/renderer.h"
#include "../include/gui/custom_menu/layout.h"
#include "../include/gui/custom_menu/theme.h"
#include "../include/gui/custom_menu/input.h"
#include "../include/gui/imgui_gui.h"
#include "../include/utils/utilities.h"
#include "../include/utils/config.h"
#include "../include/core/constants.h"
#include "../include/core/version.h"
#include "../include/game/practice_patch.h"
#include "../include/game/always_rg.h"
#include "../include/game/random_rg.h"
#include "../include/game/random_block.h"
#include "../include/game/final_memory_patch.h"
#include "../include/game/macro_controller.h"
#include "../include/game/fm_commands.h"
#include "../include/gui/overlay.h"
#include "../include/utils/xinput_shim.h"
#include "../include/utils/network.h"
#include "../include/utils/bgm_control.h"
#include "../include/input/framestep.h"
#include "../include/core/memory.h"
#include "../3rdparty/imgui/imgui.h"

#include <windows.h>
#include <cstdio>
#include <cstring>

namespace CustomMenu::Screens {

namespace {

// ===== Config helpers =====
// Config::GetSettings() hands back a const reference. For UI editing we want
// a live pointer into the cached Settings struct so the generic row system
// can mutate it. Config::SetSetting() is called in onChange callbacks to
// persist to INI.
Config::Settings& MutableSettings() {
    return const_cast<Config::Settings&>(Config::GetSettings());
}

void PersistBool(const char* section, const char* key, bool v) {
    Config::SetSetting(section, key, v ? "1" : "0");
}

void PersistInt(const char* section, const char* key, int v) {
    char buf[32];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%d", v);
    Config::SetSetting(section, key, buf);
}

void PersistFloat(const char* section, const char* key, float v) {
    char buf[32];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%.4f", v);
    Config::SetSetting(section, key, buf);
}

// ===== Character detection helpers =====
bool P1Or(int charId) {
    const auto& d = ImGuiGui::guiState.localData;
    return d.p1CharID == charId || d.p2CharID == charId;
}

bool HasIkumi()    { return P1Or(CHAR_ID_IKUMI); }
bool HasMisuzu()   { return P1Or(CHAR_ID_MISUZU); }
bool HasMishio()   { return P1Or(CHAR_ID_MISHIO); }
bool HasAkiko()    { return P1Or(CHAR_ID_AKIKO); }
bool HasNayuki()   { return P1Or(CHAR_ID_NAYUKIB); }
bool HasKano()     { return P1Or(CHAR_ID_KANO); }
bool HasRumi()     { return P1Or(CHAR_ID_NANASE); }
bool HasDoppel()   { return P1Or(CHAR_ID_EXNANASE); }
bool HasMio()      { return P1Or(CHAR_ID_MIO); }
bool HasNeyuki()   { return P1Or(CHAR_ID_NAYUKI); }
bool HasMai()      { return P1Or(CHAR_ID_MAI); }
bool HasMinagi()   { return P1Or(CHAR_ID_MINAGI); }

bool NotIkumi()  { return !HasIkumi(); }
bool NotMisuzu() { return !HasMisuzu(); }
bool NotMishio() { return !HasMishio(); }
bool NotAkiko()  { return !HasAkiko(); }
bool NotNayuki() { return !HasNayuki(); }
bool NotKano()   { return !HasKano(); }
bool NotRumi()   { return !HasRumi(); }
bool NotDoppel() { return !HasDoppel(); }
bool NotMio()    { return !HasMio(); }
bool NotNeyuki() { return !HasNeyuki(); }
bool NotMai()    { return !HasMai(); }
bool NotMinagi() { return !HasMinagi(); }

bool NoneOfTheAbove() {
    return !HasIkumi() && !HasMisuzu() && !HasMishio() && !HasAkiko() &&
           !HasNayuki() && !HasKano()  && !HasRumi()   && !HasDoppel() &&
           !HasMio()   && !HasNeyuki() && !HasMai()    && !HasMinagi();
}

bool CharsDetected() {
    const auto& d = ImGuiGui::guiState.localData;
    return d.p1CharID != 0 || d.p2CharID != 0 ||
           d.p1CharName[0] != '\0' || d.p2CharName[0] != '\0';
}

bool CharsNotDetected() { return !CharsDetected(); }

// ===== Shared "on change" hooks =====
void OnAutoApply()       { ImGuiGui::ApplyImGuiSettings(); }

void OnUseCustomMenu()   { PersistBool ("General", "useCustomMenu",          MutableSettings().useCustomMenu); }
void OnUiScale()         { PersistFloat("General", "uiScale",                MutableSettings().uiScale); }
void OnFADuration()      { PersistFloat("General", "frameAdvantageDisplayDuration", MutableSettings().frameAdvantageDisplayDuration); }
void OnShowCombo()       { PersistBool ("General", "showComboStatisticsOverlay",    MutableSettings().showComboStatisticsOverlay); }
void OnCrRequire()       { PersistBool ("General", "crRequireBothNeutral",   MutableSettings().crRequireBothNeutral); }
void OnCrDelay()         { PersistInt  ("General", "crBothNeutralDelayMs",   MutableSettings().crBothNeutralDelayMs); }
void OnAutoFixHp()       { PersistBool ("General", "autoFixHPOnNeutral",     MutableSettings().autoFixHPOnNeutral); }
void OnFreezeRfAfterCr() { PersistBool ("General", "freezeRFAfterContRec",   MutableSettings().freezeRFAfterContRec); }
void OnFreezeRfNeutral() { PersistBool ("General", "freezeRFOnlyWhenNeutral",MutableSettings().freezeRFOnlyWhenNeutral); }
void OnDetailedLogging() { PersistBool ("General", "detailedLogging",        MutableSettings().detailedLogging);
                           detailedLogging.store(MutableSettings().detailedLogging); }
void OnShowConsole()     { PersistBool ("General", "enableConsole",          MutableSettings().enableConsole); }
void OnFpsDiag()         { PersistBool ("General", "enableFpsDiagnostics",   MutableSettings().enableFpsDiagnostics); }
void OnAutoBlockTimeout(){ PersistInt  ("General", "autoBlockNeutralTimeoutMs", MutableSettings().autoBlockNeutralTimeoutMs); }

void SaveSettingsToDisk(){ Config::SaveSettings(); }

// Atomic-backed bools exposed via a local static mirror.
// The generic row system wants a bool*; we refresh from the atomic each frame
// inside the per-screen entry, and write back on change via onChange.
bool g_mirrorAutoAction    = false;
bool g_mirrorRandomize     = false;
bool g_mirrorWakeBuffer    = false;
bool g_mirrorCounterRG     = false;
bool g_mirrorFaOverlay     = false;
bool g_mirrorInfiniteBlood = false;
bool g_mirrorInfiniteFeather = false;
bool g_mirrorInfiniteElement = false;
bool g_mirrorInfiniteAwakened = false;

int  g_mirrorAutoActionPlayer = 1; // 1=P1, 2=P2, 3=Both
int  g_mirrorAutoActionPlayerIdx = 0; // 0=P1, 1=P2, 2=Both (choices index)

// Per-trigger pool mirrors (mask + use-pool flag).
unsigned int g_poolMaskAB, g_poolMaskWU, g_poolMaskAH, g_poolMaskAA, g_poolMaskRG;
bool g_useMaskAB, g_useMaskWU, g_useMaskAH, g_useMaskAA, g_useMaskRG;

void RefreshAutoMirrors() {
    const auto& d = ImGuiGui::guiState.localData;
    g_mirrorAutoAction = d.autoAction;
    g_mirrorRandomize  = d.randomizeTriggers;
    g_mirrorWakeBuffer = g_wakeBufferingEnabled.load();
    g_mirrorCounterRG  = g_counterRGEnabled.load();
    g_mirrorFaOverlay  = g_showFrameAdvantageOverlay.load();

    const int p = d.autoActionPlayer;
    g_mirrorAutoActionPlayerIdx = (p == 2) ? 1 : (p == 3 ? 2 : 0);
    g_mirrorAutoActionPlayer = p;

    g_poolMaskAB = (unsigned int)triggerAfterBlockActionPoolMask.load();
    g_poolMaskWU = (unsigned int)triggerOnWakeupActionPoolMask.load();
    g_poolMaskAH = (unsigned int)triggerAfterHitstunActionPoolMask.load();
    g_poolMaskAA = (unsigned int)triggerAfterAirtechActionPoolMask.load();
    g_poolMaskRG = (unsigned int)triggerOnRGActionPoolMask.load();
    g_useMaskAB  = triggerAfterBlockUsePool.load();
    g_useMaskWU  = triggerOnWakeupUsePool.load();
    g_useMaskAH  = triggerAfterHitstunUsePool.load();
    g_useMaskAA  = triggerAfterAirtechUsePool.load();
    g_useMaskRG  = triggerOnRGUsePool.load();
}

void OnPoolMaskAB() { triggerAfterBlockActionPoolMask.store((uint32_t)g_poolMaskAB); }
void OnPoolMaskWU() { triggerOnWakeupActionPoolMask.store((uint32_t)g_poolMaskWU); }
void OnPoolMaskAH() { triggerAfterHitstunActionPoolMask.store((uint32_t)g_poolMaskAH); }
void OnPoolMaskAA() { triggerAfterAirtechActionPoolMask.store((uint32_t)g_poolMaskAA); }
void OnPoolMaskRG() { triggerOnRGActionPoolMask.store((uint32_t)g_poolMaskRG); }
void OnUseMaskAB()  { triggerAfterBlockUsePool.store(g_useMaskAB); }
void OnUseMaskWU()  { triggerOnWakeupUsePool.store(g_useMaskWU); }
void OnUseMaskAH()  { triggerAfterHitstunUsePool.store(g_useMaskAH); }
void OnUseMaskAA()  { triggerAfterAirtechUsePool.store(g_useMaskAA); }
void OnUseMaskRG()  { triggerOnRGUsePool.store(g_useMaskRG); }

void OnAutoActionToggle()   { ImGuiGui::guiState.localData.autoAction = g_mirrorAutoAction; OnAutoApply(); }
void OnRandomizeToggle()    { ImGuiGui::guiState.localData.randomizeTriggers = g_mirrorRandomize; OnAutoApply(); }
void OnWakeBufferToggle()   { g_wakeBufferingEnabled.store(g_mirrorWakeBuffer); }
void OnCounterRGToggle()    { g_counterRGEnabled.store(g_mirrorCounterRG); OnAutoApply(); }
void OnFaOverlayToggle()    { g_showFrameAdvantageOverlay.store(g_mirrorFaOverlay); OnAutoApply(); }
void OnAutoActionTarget()   {
    const int map[3] = {1, 2, 3};
    const int idx = g_mirrorAutoActionPlayerIdx;
    const int p = (idx >= 0 && idx < 3) ? map[idx] : 1;
    ImGuiGui::guiState.localData.autoActionPlayer = p;
    g_mirrorAutoActionPlayer = p;
    OnAutoApply();
}

void RefreshCharMirrors() {
    const auto& d = ImGuiGui::guiState.localData;
    g_mirrorInfiniteBlood    = d.infiniteBloodMode;
    g_mirrorInfiniteFeather  = d.infiniteFeatherMode;
    g_mirrorInfiniteElement  = d.infiniteMishioElement;
    g_mirrorInfiniteAwakened = d.infiniteMishioAwakened;
}

void OnInfBlood()    { ImGuiGui::guiState.localData.infiniteBloodMode = g_mirrorInfiniteBlood; OnAutoApply(); }
void OnInfFeather()  { ImGuiGui::guiState.localData.infiniteFeatherMode = g_mirrorInfiniteFeather; OnAutoApply(); }
void OnInfElement()  { ImGuiGui::guiState.localData.infiniteMishioElement = g_mirrorInfiniteElement; OnAutoApply(); }
void OnInfAwakened() { ImGuiGui::guiState.localData.infiniteMishioAwakened = g_mirrorInfiniteAwakened; OnAutoApply(); }

// ===== Opponent / Options mirrors =====
// Defense subsystems live in separate namespaces; expose each via bool/int mirrors
// refreshed from their canonical getters each frame.
bool g_mirrorRandomBlock  = false;
bool g_mirrorAlwaysRG     = false;
bool g_mirrorRandomRG     = false;
bool g_mirrorAdaptiveStance = false;
int  g_mirrorDummyBlockMode = 0;   // 0..3 via SetDummyAutoBlockMode
int  g_mirrorPracticeStance = 0;   // 0=Standing,1=Jumping,2=Crouching
bool g_mirrorFmBypass     = false;

void RefreshOpponentMirrors() {
    g_mirrorRandomBlock     = RandomBlock::IsEnabled();
    g_mirrorAlwaysRG        = AlwaysRG::IsEnabled();
    g_mirrorRandomRG        = RandomRG::IsEnabled();
    g_mirrorAdaptiveStance  = GetAdaptiveStanceEnabled();
    g_mirrorDummyBlockMode  = GetDummyAutoBlockMode();
    int stance = 0;
    if (GetPracticeBlockMode(stance)) g_mirrorPracticeStance = stance;
}

void RefreshOptionsMirrors() {
    g_mirrorFaOverlay = g_showFrameAdvantageOverlay.load();
    g_mirrorFmBypass  = IsFinalMemoryBypassEnabled();
}

// Mutex: enabling any of (RandomBlock / AlwaysRG / RandomRG) should turn the
// others off, mirroring the ImGui menu's behavior.
void ForceDefenseMutex(int kept /*0=RandomBlock, 1=AlwaysRG, 2=RandomRG*/) {
    if (kept != 0 && g_mirrorRandomBlock) { g_mirrorRandomBlock = false; RandomBlock::SetEnabled(false); }
    if (kept != 1 && g_mirrorAlwaysRG)    { g_mirrorAlwaysRG    = false; AlwaysRG::SetEnabled(false); }
    if (kept != 2 && g_mirrorRandomRG)    { g_mirrorRandomRG    = false; RandomRG::SetEnabled(false); }
}

void OnRandomBlock() {
    if (g_mirrorRandomBlock) ForceDefenseMutex(0);
    RandomBlock::SetEnabled(g_mirrorRandomBlock);
}
void OnAlwaysRG() {
    if (g_mirrorAlwaysRG) ForceDefenseMutex(1);
    AlwaysRG::SetEnabled(g_mirrorAlwaysRG);
}
void OnRandomRG() {
    if (g_mirrorRandomRG) ForceDefenseMutex(2);
    RandomRG::SetEnabled(g_mirrorRandomRG);
}
void OnAdaptiveStance() { SetAdaptiveStanceEnabled(g_mirrorAdaptiveStance); }
void OnDummyBlockMode() { SetDummyAutoBlockMode(g_mirrorDummyBlockMode); }
void OnPracticeStance() { SetPracticeBlockMode(g_mirrorPracticeStance); }
void OnFmBypass()       { SetFinalMemoryBypass(g_mirrorFmBypass); }

// ===== Choices dictionaries =====
const char* const kTargetChoices[3] = { "P1", "P2", "BOTH" };
const char* const kElementChoices[4] = { "NONE", "FIRE", "LIGHT", "AWAKE" };
const char* const kStanceChoices[2]  = { "SHORT", "LONG" };
const char* const kRumiModeChoices[2] = { "SHINAI", "BARE" };

// Dense action-index list (matches ACTION_xxx constant values 0..38 in constants.h).
const char* const kActionNames[39] = {
    "5A","5B","5C","5D",
    "2A","2B","2C","2D",
    "jA","jB","jC","jD",
    "6A","6B","6C","6D",
    "4A","4B","4C","4D",
    "QCF (236)", "DP (623)", "QCB (214)", "421",
    "SUPER1 (41236)", "SUPER2 (214236)", "236236", "214214",
    "JUMP", "BACKDASH", "FORWARD DASH", "BLOCK", "FINAL MEMORY",
    "641236", "463214", "412", "22", "4123641236", "6321463214"
};
constexpr int kActionCount = 39;

const char* const kStrengthChoices[4] = { "A", "B", "C", "D" };
const char* const kJumpDirChoicesAuto[3] = { "NEUTRAL", "FORWARD", "BACK" };
const char* const kAkikoSlowChoices[4] = { "INACTIVE", "A", "B", "C" };
const char* const kMaiStatusChoices[5] = { "INACTIVE", "ACTIVE GHOST", "UNSUMMON", "CHARGING", "AWAKENING" };

// Random action pools are stored as category bits, not raw ACTION_* ids.
// Keep this in the same order as ApplyAutoAction()'s MapMotionIndexToActionType().
const char* const kActionPoolNames[24] = {
    "5X", "2X", "jX", "QCF (236)", "DP (623)", "QCB (214)", "421",
    "SUPER1 (41236)", "SUPER2 (214236)", "236236", "214214",
    "641236", "463214", "412", "22", "4123641236", "6321463214",
    "JUMP", "BACKDASH", "FORWARD DASH", "BLOCK", "FINAL MEMORY", "6X", "4X"
};
constexpr int kActionPoolCount = 24;

int ClampIndex(int v, int maxExclusive) {
    if (v < 0) return 0;
    if (v >= maxExclusive) return maxExclusive - 1;
    return v;
}

int NormalActionBase(int action) {
    if (action >= ACTION_5A && action <= ACTION_2D) return (action / 4) * 4;
    if (action >= ACTION_JA && action <= ACTION_JD) return ACTION_JA;
    if (action >= ACTION_6A && action <= ACTION_4D) return ACTION_6A + ((action - ACTION_6A) / 4) * 4;
    return -1;
}

bool IsNormalAction(int action) {
    return NormalActionBase(action) >= 0;
}

bool ActionUsesButtonStrength(int action) {
    switch (action) {
        case ACTION_QCF:
        case ACTION_DP:
        case ACTION_QCB:
        case ACTION_421:
        case ACTION_SUPER1:
        case ACTION_SUPER2:
        case ACTION_236236:
        case ACTION_214214:
        case ACTION_641236:
        case ACTION_463214:
        case ACTION_412:
        case ACTION_22:
        case ACTION_4123641236:
        case ACTION_6321463214:
            return true;
        default:
            return false;
    }
}

void OnTriggerActionChanged(int* action, int* strength) {
    if (!action || !strength) return;
    *action = ClampIndex(*action, kActionCount);
    if (IsNormalAction(*action)) {
        *strength = (*action - NormalActionBase(*action)) & 3;
    } else if (*action == ACTION_JUMP) {
        *strength = ClampIndex(*strength, 3);
    } else {
        *strength = ClampIndex(*strength, 4);
    }
}

void OnTriggerStrengthChanged(int* action, int* strength) {
    if (!action || !strength) return;
    *action = ClampIndex(*action, kActionCount);
    if (*action == ACTION_JUMP) {
        *strength = ClampIndex(*strength, 3);
    } else {
        *strength = ClampIndex(*strength, 4);
    }

    const int base = NormalActionBase(*action);
    if (base >= 0) {
        *action = base + *strength;
    }
}

const char* FormatTriggerActionStrength(const Row& row) {
    static char buffers[8][96];
    static int next = 0;
    char* buf = buffers[next++ & 7];

    const int action = row.choiceIdxPtr ? ClampIndex(*row.choiceIdxPtr, kActionCount) : 0;
    const int strength = row.choice2IdxPtr ? *row.choice2IdxPtr : 0;
    const char* actionName = (row.choices && action >= 0 && action < row.choiceCount)
        ? row.choices[action]
        : "?";

    if (IsNormalAction(action)) {
        _snprintf_s(buf, sizeof(buffers[0]), _TRUNCATE, "%s", actionName);
    } else if (action == ACTION_JUMP) {
        const int dir = ClampIndex(strength, 3);
        _snprintf_s(buf, sizeof(buffers[0]), _TRUNCATE, "%s  %s", actionName, kJumpDirChoicesAuto[dir]);
    } else if (ActionUsesButtonStrength(action)) {
        const int btn = ClampIndex(strength, 4);
        _snprintf_s(buf, sizeof(buffers[0]), _TRUNCATE, "%s  %s", actionName, kStrengthChoices[btn]);
    } else {
        _snprintf_s(buf, sizeof(buffers[0]), _TRUNCATE, "%s", actionName);
    }
    return buf;
}

bool HideABSingleAction() { return g_useMaskAB; }
bool HideWUSingleAction() { return g_useMaskWU; }
bool HideAHSingleAction() { return g_useMaskAH; }
bool HideAASingleAction() { return g_useMaskAA; }
bool HideRGSingleAction() { return g_useMaskRG; }
bool HideABPool() { return !g_useMaskAB; }
bool HideWUPool() { return !g_useMaskWU; }
bool HideAHPool() { return !g_useMaskAH; }
bool HideAAPool() { return !g_useMaskAA; }
bool HideRGPool() { return !g_useMaskRG; }

// Macro slot picker list. Rebuilt once per frame to match the current slot count.
constexpr int kMaxMacroSlotEntries = 17;   // 1 "none" + up to 16 slots
static const char* g_macroSlotChoiceArr[kMaxMacroSlotEntries];
static char s_macroSlotLabels[kMaxMacroSlotEntries][16];
static int g_macroSlotChoiceCount = 1;

void RefreshMacroSlotChoices() {
    int slots = MacroController::GetSlotCount();
    if (slots < 0) slots = 0;
    if (slots > kMaxMacroSlotEntries - 1) slots = kMaxMacroSlotEntries - 1;
    g_macroSlotChoiceCount = slots + 1;
    // Index 0 = "NONE" (macroSlot==0 in DisplayData means no macro)
    strncpy_s(s_macroSlotLabels[0], sizeof(s_macroSlotLabels[0]), "NONE", _TRUNCATE);
    g_macroSlotChoiceArr[0] = s_macroSlotLabels[0];
    for (int i = 1; i <= slots; ++i) {
        _snprintf_s(s_macroSlotLabels[i], sizeof(s_macroSlotLabels[i]), _TRUNCATE, "SLOT %d", i);
        g_macroSlotChoiceArr[i] = s_macroSlotLabels[i];
    }
}

// ===== SETTINGS / GENERAL =====
const char* const kUiFontChoices[2] = { "DEFAULT", "SEGOE UI" };
void OnUiFont() {
    PersistInt("General", "uiFont", MutableSettings().uiFontMode);
}
void OnPracticeHint() {
    PersistBool("General", "showPracticeEntryHint", MutableSettings().showPracticeEntryHint);
}
void OnRestrictPractice() {
    PersistBool("General", "restrictToPracticeMode", MutableSettings().restrictToPracticeMode);
}

Row* BuildSettingsGeneralRows(int& count) {
    static Row s_rows[32];
    int n = 0;
    auto& s = MutableSettings();

    s_rows[n++] = Header("INTERFACE");
    s_rows[n++] = Toggle    ("USE CUSTOM MENU",        &s.useCustomMenu,       OnUseCustomMenu);
    s_rows[n++] = FloatNum  ("UI SCALE",               &s.uiScale,      0.70f, 1.50f, 0.05f, 0.10f, "%.2f", OnUiScale);
    s_rows[n++] = ChoicesRow("UI FONT (IMGUI MENU)",   &s.uiFontMode,   kUiFontChoices, 2, OnUiFont);
    s_rows[n++] = Toggle    ("PRACTICE OVERLAY HINT",  &s.showPracticeEntryHint, OnPracticeHint);
    s_rows[n++] = Toggle    ("RESTRICT TO PRACTICE",   &s.restrictToPracticeMode, OnRestrictPractice);

    s_rows[n++] = Header("RECOVERY");
    s_rows[n++] = Toggle ("CR: BOTH NEUTRAL REQD",     &s.crRequireBothNeutral, OnCrRequire);
    s_rows[n++] = IntNum ("CR NEUTRAL DELAY (MS)",     &s.crBothNeutralDelayMs, 0, 5000, 50, 500, OnCrDelay);
    s_rows[n++] = Toggle ("AUTO-FIX HP<=0",            &s.autoFixHPOnNeutral,   OnAutoFixHp);
    s_rows[n++] = Toggle ("FREEZE RF AFTER CR",        &s.freezeRFAfterContRec, OnFreezeRfAfterCr);
    s_rows[n++] = Toggle ("FREEZE RF ONLY NEUTRAL",    &s.freezeRFOnlyWhenNeutral, OnFreezeRfNeutral);

    s_rows[n++] = Header("PRACTICE");
    s_rows[n++] = IntNum ("AUTO-BLOCK TIMEOUT (MS)",   &s.autoBlockNeutralTimeoutMs, 0, 60000, 500, 5000, OnAutoBlockTimeout);

    s_rows[n++] = Spacer();
    s_rows[n++] = Action ("SAVE ALL TO DISK",          SaveSettingsToDisk);

    count = n;
    return s_rows;
}

// ===== SETTINGS / HOTKEYS =====
const char* HotkeyNameValue(int vk) {
    static char buffers[8][64];
    static int next = 0;
    char* buf = buffers[next++ & 7];
    _snprintf_s(buf, sizeof(buffers[0]), _TRUNCATE, "%s",
                Config::GetKeyName(vk).c_str());
    return buf;
}

void BindHotkey(const char* title, int* field, const char* key) {
    OpenKeybind(title, field, "Hotkeys", key);
}

void BindOpenMenu()       { auto& s = MutableSettings(); BindHotkey("OPEN MENU",       &s.configMenuKey,          "ConfigMenuKey"); }
void BindTeleport()       { auto& s = MutableSettings(); BindHotkey("TELEPORT",        &s.teleportKey,           "TeleportKey"); }
void BindSavePosition()   { auto& s = MutableSettings(); BindHotkey("SAVE POSITION",   &s.recordKey,             "RecordKey"); }
void BindToggleStats()    { auto& s = MutableSettings(); BindHotkey("TOGGLE STATS",    &s.toggleTitleKey,        "ToggleTitleKey"); }
void BindResetCounter()   { auto& s = MutableSettings(); BindHotkey("RESET COUNTER",   &s.resetFrameCounterKey,  "ResetFrameCounterKey"); }
void BindHelp()           { auto& s = MutableSettings(); BindHotkey("HELP",            &s.helpKey,               "HelpKey"); }
void BindToggleImGui()    { auto& s = MutableSettings(); BindHotkey("TOGGLE IMGUI",    &s.toggleImGuiKey,        "ToggleImGuiKey"); }
void BindSwitchPlayers()  { auto& s = MutableSettings(); BindHotkey("SWITCH PLAYERS",  &s.switchPlayersKey,      "SwitchPlayersKey"); }
void BindMacroRecord()    { auto& s = MutableSettings(); BindHotkey("MACRO RECORD",    &s.macroRecordKey,        "MacroRecordKey"); }
void BindMacroPlay()      { auto& s = MutableSettings(); BindHotkey("MACRO PLAY",      &s.macroPlayKey,          "MacroPlayKey"); }
void BindMacroSlot()      { auto& s = MutableSettings(); BindHotkey("MACRO NEXT SLOT", &s.macroSlotKey,          "MacroSlotKey"); }
void BindUiAccept()       { auto& s = MutableSettings(); BindHotkey("UI ACCEPT",       &s.uiAcceptKey,           "UIAcceptKey"); }
void BindUiRefresh()      { auto& s = MutableSettings(); BindHotkey("UI REFRESH",      &s.uiRefreshKey,          "UIRefreshKey"); }
void BindUiExit()         { auto& s = MutableSettings(); BindHotkey("UI EXIT",         &s.uiExitKey,             "UIExitKey"); }
void BindFramestepPause() { auto& s = MutableSettings(); BindHotkey("FRAMESTEP PAUSE", &s.framestepPauseKey,     "FramestepPauseKey"); }
void BindFramestepStep()  { auto& s = MutableSettings(); BindHotkey("FRAMESTEP STEP",  &s.framestepStepKey,      "FramestepStepKey"); }
void BindSwapCustom()     { auto& s = MutableSettings(); BindHotkey("SWAP CUSTOM KEY", &s.swapCustomKey,         "SwapCustomKey"); }

const char* ValOpenMenu()       { return HotkeyNameValue(Config::GetSettings().configMenuKey); }
const char* ValTeleport()       { return HotkeyNameValue(Config::GetSettings().teleportKey); }
const char* ValSavePosition()   { return HotkeyNameValue(Config::GetSettings().recordKey); }
const char* ValToggleStats()    { return HotkeyNameValue(Config::GetSettings().toggleTitleKey); }
const char* ValResetCounter()   { return HotkeyNameValue(Config::GetSettings().resetFrameCounterKey); }
const char* ValHelp()           { return HotkeyNameValue(Config::GetSettings().helpKey); }
const char* ValToggleImGui()    { return HotkeyNameValue(Config::GetSettings().toggleImGuiKey); }
const char* ValSwitchPlayers()  { return HotkeyNameValue(Config::GetSettings().switchPlayersKey); }
const char* ValMacroRecord()    { return HotkeyNameValue(Config::GetSettings().macroRecordKey); }
const char* ValMacroPlay()      { return HotkeyNameValue(Config::GetSettings().macroPlayKey); }
const char* ValMacroSlot()      { return HotkeyNameValue(Config::GetSettings().macroSlotKey); }
const char* ValUiAccept()       { return HotkeyNameValue(Config::GetSettings().uiAcceptKey); }
const char* ValUiRefresh()      { return HotkeyNameValue(Config::GetSettings().uiRefreshKey); }
const char* ValUiExit()         { return HotkeyNameValue(Config::GetSettings().uiExitKey); }
const char* ValFramestepPause() { return HotkeyNameValue(Config::GetSettings().framestepPauseKey); }
const char* ValFramestepStep()  { return HotkeyNameValue(Config::GetSettings().framestepStepKey); }
const char* ValSwapCustom()     { return HotkeyNameValue(Config::GetSettings().swapCustomKey); }
const char* ValSwapEnabled()    { return Config::GetSettings().swapCustomEnabled ? "ON" : "OFF"; }

void OnSwapCustomEnabled() {
    PersistBool("Hotkeys", "SwapCustomEnabled", MutableSettings().swapCustomEnabled);
}

bool SwapCustomKeyDisabled() {
    return !Config::GetSettings().swapCustomEnabled;
}

void RefreshHotkeyStrings() {}

Row* BuildSettingsHotkeysRows(int& count) {
    static Row s_rows[40];
    int n = 0;
    auto& s = MutableSettings();

    s_rows[n++] = Header("GAMEPLAY HOTKEYS");
    s_rows[n++] = Action("OPEN MENU",       BindOpenMenu,       ValOpenMenu);
    s_rows[n++] = Action("TELEPORT",        BindTeleport,       ValTeleport);
    s_rows[n++] = Action("SAVE POSITION",   BindSavePosition,   ValSavePosition);
    s_rows[n++] = Action("TOGGLE STATS",    BindToggleStats,    ValToggleStats);
    s_rows[n++] = Action("RESET COUNTER",   BindResetCounter,   ValResetCounter);
    s_rows[n++] = Action("HELP",            BindHelp,           ValHelp);
    s_rows[n++] = Action("TOGGLE IMGUI",    BindToggleImGui,    ValToggleImGui);
    s_rows[n++] = Action("SWITCH PLAYERS",  BindSwitchPlayers,  ValSwitchPlayers);

    s_rows[n++] = Spacer();
    s_rows[n++] = Header("MACROS");
    s_rows[n++] = Action("MACRO RECORD",    BindMacroRecord,    ValMacroRecord);
    s_rows[n++] = Action("MACRO PLAY",      BindMacroPlay,      ValMacroPlay);
    s_rows[n++] = Action("MACRO NEXT SLOT", BindMacroSlot,      ValMacroSlot);

    s_rows[n++] = Spacer();
    s_rows[n++] = Header("MENU CONTROL");
    s_rows[n++] = Action("UI ACCEPT",       BindUiAccept,       ValUiAccept);
    s_rows[n++] = Action("UI REFRESH",      BindUiRefresh,      ValUiRefresh);
    s_rows[n++] = Action("UI EXIT",         BindUiExit,         ValUiExit);
    s_rows[n++] = Action("FRAMESTEP PAUSE", BindFramestepPause, ValFramestepPause);
    s_rows[n++] = Action("FRAMESTEP STEP",  BindFramestepStep,  ValFramestepStep);

    s_rows[n++] = Spacer();
    s_rows[n++] = Header("SWAP POSITIONS");
    s_rows[n++] = Toggle("CUSTOM SWAP KEY", &s.swapCustomEnabled, OnSwapCustomEnabled);
    s_rows[n++] = Action("SWAP CUSTOM KEY", BindSwapCustom, ValSwapCustom, SwapCustomKeyDisabled);

    s_rows[n++] = Spacer();
    s_rows[n++] = Action("SAVE ALL TO DISK", SaveSettingsToDisk);
    count = n;
    return s_rows;
}

// ===== SETTINGS / DEBUG =====
bool g_mirrorOverlayBorders = false;
bool g_mirrorRGToasts       = false;
bool g_mirrorPadInputLog    = false;
bool g_mirrorDeepFA         = false;

void RefreshDebugMirrors() {
    g_mirrorOverlayBorders = g_ShowOverlayDebugBorders.load();
    g_mirrorRGToasts       = g_ShowRGDebugToasts.load();
    g_mirrorPadInputLog    = XInputShim::g_LogGenericPadInputDebug.load();
    g_mirrorDeepFA         = g_deepFrameAdvDebug.load();
}

void OnOverlayBorders() { g_ShowOverlayDebugBorders.store(g_mirrorOverlayBorders); }
void OnRGToasts()       { g_ShowRGDebugToasts.store(g_mirrorRGToasts); }
void OnPadInputLog()    { XInputShim::g_LogGenericPadInputDebug.store(g_mirrorPadInputLog); }
void OnDeepFA()         { g_deepFrameAdvDebug.store(g_mirrorDeepFA); }

int g_bgmSlot = 1;

void RunStopBGM() {
    uintptr_t base = GetEFZBase();
    if (!base) return;
    uintptr_t gameStatePtr = 0;
    if (!SafeReadMemory(base + EFZ_BASE_OFFSET_GAME_STATE, &gameStatePtr, sizeof(gameStatePtr))) return;
    StopBGM(gameStatePtr);
}
void RunPlayBGM() {
    uintptr_t base = GetEFZBase();
    if (!base) return;
    uintptr_t gameStatePtr = 0;
    if (!SafeReadMemory(base + EFZ_BASE_OFFSET_GAME_STATE, &gameStatePtr, sizeof(gameStatePtr))) return;
    if (g_bgmSlot < 0) g_bgmSlot = 0;
    PlayBGM(gameStatePtr, (unsigned short)g_bgmSlot);
}
void RunP1FinalMemory() {
    const auto& d = ImGuiGui::guiState.localData;
    ExecuteFinalMemory(1, d.p1CharID);
}
void RunP2FinalMemory() {
    const auto& d = ImGuiGui::guiState.localData;
    ExecuteFinalMemory(2, d.p2CharID);
}

Row* BuildSettingsDebugRows(int& count) {
    static Row s_rows[32];
    int n = 0;
    auto& s = MutableSettings();

    s_rows[n++] = Header("DEBUG LOGGING");
    s_rows[n++] = Toggle ("DETAILED LOGGING",     &s.detailedLogging,      OnDetailedLogging);
    s_rows[n++] = Toggle ("DEBUG FILE LOG",       &s.enableDebugFileLog,
        [](){ PersistBool("General", "enableDebugFileLog", MutableSettings().enableDebugFileLog); });
    s_rows[n++] = Toggle ("FPS DIAGNOSTICS",      &s.enableFpsDiagnostics, OnFpsDiag);
    s_rows[n++] = Toggle ("SHOW DEBUG CONSOLE",   &s.enableConsole,        OnShowConsole);
    s_rows[n++] = Toggle ("CHAR SELECT LOGGER",   &s.enableCharacterSelectLogger,
        [](){ PersistBool("General", "enableCharacterSelectLogger", MutableSettings().enableCharacterSelectLogger); });
    s_rows[n++] = Toggle ("LOG CONTROLLER INPUT", &g_mirrorPadInputLog,    OnPadInputLog);
    s_rows[n++] = Toggle ("LOG DETAILED FA",      &g_mirrorDeepFA,         OnDeepFA);

    s_rows[n++] = Header("OVERLAYS");
    s_rows[n++] = Toggle ("OVERLAY DEBUG BORDERS",     &g_mirrorOverlayBorders,       OnOverlayBorders);
    s_rows[n++] = Toggle ("RG DEBUG TOASTS",           &g_mirrorRGToasts,             OnRGToasts);
    s_rows[n++] = Toggle ("COMBO STATS OVERLAY",       &s.showComboStatisticsOverlay, OnShowCombo);
    s_rows[n++] = FloatNum("FA DURATION (SEC)",        &s.frameAdvantageDisplayDuration, 0.5f, 30.0f, 0.1f, 1.0f, "%.1f", OnFADuration);

    s_rows[n++] = Header("BGM");
    s_rows[n++] = IntNum ("BGM SLOT",                  &g_bgmSlot, 0, 999, 1, 10);
    s_rows[n++] = Action ("PLAY BGM",                  RunPlayBGM);
    s_rows[n++] = Action ("STOP BGM",                  RunStopBGM);

    s_rows[n++] = Header("FINAL MEMORY");
    s_rows[n++] = Action ("RUN P1 FINAL MEMORY",       RunP1FinalMemory);
    s_rows[n++] = Action ("RUN P2 FINAL MEMORY",       RunP2FinalMemory);

    s_rows[n++] = Spacer();
    s_rows[n++] = Action ("SAVE ALL TO DISK",          SaveSettingsToDisk);
    count = n;
    return s_rows;
}

// ===== HELP screen =====
char g_helpVersionStr[64];
char g_helpBuildStr[64];
char g_helpHotkeyOpenMenu[48];
char g_helpHotkeyTeleport[48];
char g_helpHotkeySavePos[48];
char g_helpHotkeyMacroRec[48];
char g_helpHotkeyMacroPlay[48];
char g_helpHotkeySwitch[48];

void RefreshHelpStrings() {
    const auto& s = Config::GetSettings();
    _snprintf_s(g_helpVersionStr, sizeof(g_helpVersionStr), _TRUNCATE,
                "EFZ TRAINING MODE v%s", EFZ_TRAINING_MODE_VERSION);
    _snprintf_s(g_helpBuildStr, sizeof(g_helpBuildStr), _TRUNCATE,
                "BUILT %s %s", __DATE__, __TIME__);
    _snprintf_s(g_helpHotkeyOpenMenu, sizeof(g_helpHotkeyOpenMenu), _TRUNCATE,
                "OPEN MENU    %s", Config::GetKeyName(s.configMenuKey).c_str());
    _snprintf_s(g_helpHotkeyTeleport, sizeof(g_helpHotkeyTeleport), _TRUNCATE,
                "TELEPORT     %s", Config::GetKeyName(s.teleportKey).c_str());
    _snprintf_s(g_helpHotkeySavePos, sizeof(g_helpHotkeySavePos), _TRUNCATE,
                "SAVE POS     %s", Config::GetKeyName(s.recordKey).c_str());
    _snprintf_s(g_helpHotkeyMacroRec, sizeof(g_helpHotkeyMacroRec), _TRUNCATE,
                "MACRO REC    %s", Config::GetKeyName(s.macroRecordKey).c_str());
    _snprintf_s(g_helpHotkeyMacroPlay, sizeof(g_helpHotkeyMacroPlay), _TRUNCATE,
                "MACRO PLAY   %s", Config::GetKeyName(s.macroPlayKey).c_str());
    _snprintf_s(g_helpHotkeySwitch, sizeof(g_helpHotkeySwitch), _TRUNCATE,
                "SWITCH PLAYERS %s", Config::GetKeyName(s.switchPlayersKey).c_str());
}

void OpenGithubReleases() {
    ShellExecuteA(nullptr, "open",
                  "https://github.com/Aquat1c/EFZ-Training-Mode/releases",
                  nullptr, nullptr, SW_SHOWNORMAL);
}

Row* BuildHelpAboutRows(int& count) {
    static Row s_rows[12];
    int n = 0;
    s_rows[n++] = Header(g_helpVersionStr);
    s_rows[n++] = Info  (g_helpBuildStr);
    s_rows[n++] = Spacer();
    s_rows[n++] = Header("LINKS");
    s_rows[n++] = Action("OPEN GITHUB RELEASES", OpenGithubReleases);
    s_rows[n++] = Spacer();
    s_rows[n++] = Info("EFZ TRAINING MODE IS A 32-BIT DLL MOD");
    s_rows[n++] = Info("FOR ETERNAL FIGHTER ZERO REVIVAL.");
    count = n;
    return s_rows;
}

Row* BuildHelpHotkeysRows(int& count) {
    static Row s_rows[16];
    int n = 0;
    s_rows[n++] = Header("GAMEPLAY HOTKEYS");
    s_rows[n++] = Info  (g_helpHotkeyOpenMenu);
    s_rows[n++] = Info  (g_helpHotkeyTeleport);
    s_rows[n++] = Info  (g_helpHotkeySavePos);
    s_rows[n++] = Info  (g_helpHotkeySwitch);
    s_rows[n++] = Spacer();
    s_rows[n++] = Header("MACROS");
    s_rows[n++] = Info  (g_helpHotkeyMacroRec);
    s_rows[n++] = Info  (g_helpHotkeyMacroPlay);
    count = n;
    return s_rows;
}

Row* BuildHelpControlsRows(int& count) {
    static Row s_rows[20];
    int n = 0;
    s_rows[n++] = Header("NAVIGATION");
    s_rows[n++] = Info  ("UP/DOWN        MOVE FOCUS");
    s_rows[n++] = Info  ("LEFT/RIGHT     ADJUST VALUE");
    s_rows[n++] = Info  ("SHIFT+L/R      BIG STEP");
    s_rows[n++] = Info  ("ENTER / A      ACTIVATE");
    s_rows[n++] = Info  ("ESC / B        BACK / CLOSE");
    s_rows[n++] = Spacer();
    s_rows[n++] = Header("TABS");
    s_rows[n++] = Info  ("LB/RB          TOP TAB PREV/NEXT");
    s_rows[n++] = Info  ("PGUP/PGDN      TOP TAB PREV/NEXT");
    s_rows[n++] = Info  ("1..5           JUMP TO TOP TAB");
    s_rows[n++] = Info  ("LT/RT          SUB-TAB PREV/NEXT");
    s_rows[n++] = Info  ("[ / ]          SUB-TAB PREV/NEXT");
    s_rows[n++] = Spacer();
    s_rows[n++] = Header("MOUSE");
    s_rows[n++] = Info  ("HOVER          STEAL FOCUS");
    s_rows[n++] = Info  ("CLICK          ACTIVATE ROW");
    s_rows[n++] = Info  ("WHEEL          SCROLL LIST");
    count = n;
    return s_rows;
}

// ===== AUTO / TRIGGERS =====
void AddTriggerRows(Row* rows, int& n,
                    const char* title,
                    bool* enabled,
                    int* action,
                    int* strength,
                    int* macroSlot,
                    int* delay,
                    bool* usePool,
                    unsigned int* poolMask,
                    void (*onUsePool)(),
                    void (*onPoolMask)(),
                    bool (*hideSingleAction)(),
                    bool (*hidePool)()) {
    rows[n++] = Header(title);
    rows[n++] = Toggle        ("  ENABLE",      enabled, OnAutoApply);
    rows[n++] = Toggle        ("  RANDOM POOL", usePool, onUsePool);
    rows[n++] = ActionStrengthRow("  ACTION", action, kActionNames, kActionCount,
                                  strength, kStrengthChoices, 4,
                                  FormatTriggerActionStrength,
                                  OnTriggerActionChanged,
                                  OnTriggerStrengthChanged,
                                  OnAutoApply,
                                  nullptr,
                                  hideSingleAction);
    rows[n++] = MaskPickerRow ("  ACTION POOL", poolMask, kActionPoolNames, kActionPoolCount,
                               onPoolMask, nullptr, hidePool);
    rows[n++] = IntNum        ("  DELAY",       delay, 0, 60, 1, 5, OnAutoApply);
    rows[n++] = DropdownRow   ("  MACRO",       macroSlot, g_macroSlotChoiceArr, g_macroSlotChoiceCount, OnAutoApply);
}

Row* BuildTriggersRows(int& count) {
    static Row s_rows[96];
    int n = 0;
    auto& d = ImGuiGui::guiState.localData;

    s_rows[n++] = Header("GLOBAL");
    s_rows[n++] = Toggle    ("ENABLE AUTO ACTION",    &g_mirrorAutoAction, OnAutoActionToggle);
    s_rows[n++] = ChoicesRow("TARGET",                &g_mirrorAutoActionPlayerIdx, kTargetChoices, 3, OnAutoActionTarget);
    s_rows[n++] = Toggle    ("RANDOMIZE TRIGGERS",    &g_mirrorRandomize,  OnRandomizeToggle);
    s_rows[n++] = Toggle    ("PRE-BUFFER WAKEUP",     &g_mirrorWakeBuffer, OnWakeBufferToggle);
    s_rows[n++] = Toggle    ("COUNTER RG",            &g_mirrorCounterRG,  OnCounterRGToggle);
    s_rows[n++] = Toggle    ("FRAME ADV OVERLAY",     &g_mirrorFaOverlay,  OnFaOverlayToggle);

    AddTriggerRows(s_rows, n, "AFTER BLOCK",
                   &d.triggerAfterBlock, &d.actionAfterBlock, &d.strengthAfterBlock,
                   &d.macroSlotAfterBlock, &d.delayAfterBlock,
                   &g_useMaskAB, &g_poolMaskAB, OnUseMaskAB, OnPoolMaskAB,
                   HideABSingleAction, HideABPool);

    AddTriggerRows(s_rows, n, "ON WAKEUP",
                   &d.triggerOnWakeup, &d.actionOnWakeup, &d.strengthOnWakeup,
                   &d.macroSlotOnWakeup, &d.delayOnWakeup,
                   &g_useMaskWU, &g_poolMaskWU, OnUseMaskWU, OnPoolMaskWU,
                   HideWUSingleAction, HideWUPool);

    AddTriggerRows(s_rows, n, "AFTER HITSTUN",
                   &d.triggerAfterHitstun, &d.actionAfterHitstun, &d.strengthAfterHitstun,
                   &d.macroSlotAfterHitstun, &d.delayAfterHitstun,
                   &g_useMaskAH, &g_poolMaskAH, OnUseMaskAH, OnPoolMaskAH,
                   HideAHSingleAction, HideAHPool);

    AddTriggerRows(s_rows, n, "AFTER AIRTECH",
                   &d.triggerAfterAirtech, &d.actionAfterAirtech, &d.strengthAfterAirtech,
                   &d.macroSlotAfterAirtech, &d.delayAfterAirtech,
                   &g_useMaskAA, &g_poolMaskAA, OnUseMaskAA, OnPoolMaskAA,
                   HideAASingleAction, HideAAPool);

    AddTriggerRows(s_rows, n, "ON RG",
                   &d.triggerOnRG, &d.actionOnRG, &d.strengthOnRG,
                   &d.macroSlotOnRG, &d.delayOnRG,
                   &g_useMaskRG, &g_poolMaskRG, OnUseMaskRG, OnPoolMaskRG,
                   HideRGSingleAction, HideRGPool);

    count = n;
    return s_rows;
}

// ===== CHARACTERS screen =====
bool CharHasCustomRows(int charId) {
    switch (charId) {
        case CHAR_ID_IKUMI:
        case CHAR_ID_MISUZU:
        case CHAR_ID_MISHIO:
        case CHAR_ID_AKIKO:
        case CHAR_ID_NAYUKIB:
        case CHAR_ID_NAYUKI:
        case CHAR_ID_KANO:
        case CHAR_ID_NANASE:
        case CHAR_ID_EXNANASE:
        case CHAR_ID_MIO:
        case CHAR_ID_MAI:
        case CHAR_ID_MINAGI:
            return true;
        default:
            return false;
    }
}

void RefreshCharacterDataAction() {
    ImGuiGui::RefreshLocalData();
}

void ForceP1MaiSummon()   { ImGuiGui::guiState.localData.p1MaiForceSummon = true; OnAutoApply(); }
void ForceP2MaiSummon()   { ImGuiGui::guiState.localData.p2MaiForceSummon = true; OnAutoApply(); }
void ForceP1MaiDespawn()  { ImGuiGui::guiState.localData.p1MaiForceDespawn = true; OnAutoApply(); }
void ForceP2MaiDespawn()  { ImGuiGui::guiState.localData.p2MaiForceDespawn = true; OnAutoApply(); }
void ApplyP1MaiGhostPos() { ImGuiGui::guiState.localData.p1MaiApplyGhostPos = true; OnAutoApply(); }
void ApplyP2MaiGhostPos() { ImGuiGui::guiState.localData.p2MaiApplyGhostPos = true; OnAutoApply(); }
void ApplyP1MinagiPos()   { ImGuiGui::guiState.localData.p1MinagiApplyPos = true; OnAutoApply(); }
void ApplyP2MinagiPos()   { ImGuiGui::guiState.localData.p2MinagiApplyPos = true; OnAutoApply(); }

void AddCharacterLockRows(Row* rows, int& n) {
    bool addedHeader = false;
    auto header = [&]() {
        if (!addedHeader) {
            rows[n++] = Header("MATCH-WIDE LOCKS");
            addedHeader = true;
        }
    };

    if (HasIkumi()) {
        header();
        rows[n++] = Toggle("INFINITE IKUMI BLOOD", &g_mirrorInfiniteBlood, OnInfBlood);
    }
    if (HasMisuzu()) {
        header();
        rows[n++] = Toggle("INFINITE MISUZU FEATHER", &g_mirrorInfiniteFeather, OnInfFeather);
    }
    if (HasMishio()) {
        header();
        rows[n++] = Toggle("LOCK MISHIO ELEMENT", &g_mirrorInfiniteElement, OnInfElement);
        rows[n++] = Toggle("INFINITE MISHIO AWAKEN", &g_mirrorInfiniteAwakened, OnInfAwakened);
    }
    if (HasMinagi()) {
        header();
        rows[n++] = Toggle("MINAGI PROJECTILES -> MICHIRU", &ImGuiGui::guiState.localData.minagiConvertNewProjectiles, OnAutoApply);
    }

    if (addedHeader) {
        rows[n++] = Spacer();
    }
}

void AddIkumiRows(Row* rows, int& n, DisplayData& d, int player) {
    if (player == 1) {
        rows[n++] = IntNum("BLOOD STOCK",  &d.p1IkumiBlood, 0, IKUMI_BLOOD_MAX, 1, 1, OnAutoApply);
        rows[n++] = IntNum("GENOCIDE TIMER", &d.p1IkumiGenocide, 0, IKUMI_GENOCIDE_MAX, 30, 300, OnAutoApply);
        rows[n++] = IntNum("LEVEL GAUGE", &d.p1IkumiLevelGauge, 0, 99, 1, 10, OnAutoApply);
    } else {
        rows[n++] = IntNum("BLOOD STOCK",  &d.p2IkumiBlood, 0, IKUMI_BLOOD_MAX, 1, 1, OnAutoApply);
        rows[n++] = IntNum("GENOCIDE TIMER", &d.p2IkumiGenocide, 0, IKUMI_GENOCIDE_MAX, 30, 300, OnAutoApply);
        rows[n++] = IntNum("LEVEL GAUGE", &d.p2IkumiLevelGauge, 0, 99, 1, 10, OnAutoApply);
    }
}

void AddMisuzuRows(Row* rows, int& n, DisplayData& d, int player) {
    if (player == 1) {
        rows[n++] = IntNum("FEATHERS", &d.p1MisuzuFeathers, 0, MISUZU_FEATHER_MAX, 1, 1, OnAutoApply);
        rows[n++] = Toggle("INFINITE POISON", &d.p1MisuzuInfinitePoison, OnAutoApply);
        rows[n++] = IntNum("POISON TIMER", &d.p1MisuzuPoisonTimer, 0, MISUZU_POISON_TIMER_MAX, 30, 300, OnAutoApply);
        rows[n++] = IntNum("POISON LEVEL", &d.p1MisuzuPoisonLevel, 0, 99, 1, 10, OnAutoApply);
    } else {
        rows[n++] = IntNum("FEATHERS", &d.p2MisuzuFeathers, 0, MISUZU_FEATHER_MAX, 1, 1, OnAutoApply);
        rows[n++] = Toggle("INFINITE POISON", &d.p2MisuzuInfinitePoison, OnAutoApply);
        rows[n++] = IntNum("POISON TIMER", &d.p2MisuzuPoisonTimer, 0, MISUZU_POISON_TIMER_MAX, 30, 300, OnAutoApply);
        rows[n++] = IntNum("POISON LEVEL", &d.p2MisuzuPoisonLevel, 0, 99, 1, 10, OnAutoApply);
    }
}

void AddMishioRows(Row* rows, int& n, DisplayData& d, int player) {
    if (player == 1) {
        rows[n++] = ChoicesRow("ELEMENT", &d.p1MishioElement, kElementChoices, 4, OnAutoApply);
        rows[n++] = IntNum("AWAKEN TIMER", &d.p1MishioAwakenedTimer, 0, MISHIO_AWAKENED_TARGET, 60, 600, OnAutoApply);
    } else {
        rows[n++] = ChoicesRow("ELEMENT", &d.p2MishioElement, kElementChoices, 4, OnAutoApply);
        rows[n++] = IntNum("AWAKEN TIMER", &d.p2MishioAwakenedTimer, 0, MISHIO_AWAKENED_TARGET, 60, 600, OnAutoApply);
    }
}

void AddAkikoRows(Row* rows, int& n, DisplayData& d, int player) {
    if (player == 1) {
        rows[n++] = IntNum("BULLET CYCLE", &d.p1AkikoBulletCycle, 0, 5, 1, 1, OnAutoApply);
        rows[n++] = Toggle("FREEZE CYCLE", &d.p1AkikoFreezeCycle, OnAutoApply);
        rows[n++] = Toggle("SHOW CLEAN HIT", &d.p1AkikoShowCleanHit, OnAutoApply);
        rows[n++] = ChoicesRow("TIME-SLOW TRIGGER", &d.p1AkikoTimeslowTrigger, kAkikoSlowChoices, 4, OnAutoApply);
        rows[n++] = Toggle("INFINITE TIMESLOW", &d.p1AkikoInfiniteTimeslow, OnAutoApply);
    } else {
        rows[n++] = IntNum("BULLET CYCLE", &d.p2AkikoBulletCycle, 0, 5, 1, 1, OnAutoApply);
        rows[n++] = Toggle("FREEZE CYCLE", &d.p2AkikoFreezeCycle, OnAutoApply);
        rows[n++] = Toggle("SHOW CLEAN HIT", &d.p2AkikoShowCleanHit, OnAutoApply);
        rows[n++] = ChoicesRow("TIME-SLOW TRIGGER", &d.p2AkikoTimeslowTrigger, kAkikoSlowChoices, 4, OnAutoApply);
        rows[n++] = Toggle("INFINITE TIMESLOW", &d.p2AkikoInfiniteTimeslow, OnAutoApply);
    }
}

void AddNayukiRows(Row* rows, int& n, DisplayData& d, int player) {
    if (player == 1) {
        rows[n++] = Toggle("INFINITE SNOW", &d.p1NayukiInfiniteSnow, OnAutoApply);
        rows[n++] = IntNum("SNOWBUNNY TIMER", &d.p1NayukiSnowbunnies, 0, NAYUKIB_SNOWBUNNY_MAX, 30, 300, OnAutoApply);
    } else {
        rows[n++] = Toggle("INFINITE SNOW", &d.p2NayukiInfiniteSnow, OnAutoApply);
        rows[n++] = IntNum("SNOWBUNNY TIMER", &d.p2NayukiSnowbunnies, 0, NAYUKIB_SNOWBUNNY_MAX, 30, 300, OnAutoApply);
    }
}

void AddNeyukiRows(Row* rows, int& n, DisplayData& d, int player) {
    if (player == 1) {
        rows[n++] = IntNum("JAM COUNT", &d.p1NeyukiJamCount, 0, NEYUKI_JAM_COUNT_MAX, 1, 1, OnAutoApply);
        rows[n++] = Toggle("LOCK JAM COUNT", &d.p1NeyukiLockJam, OnAutoApply);
    } else {
        rows[n++] = IntNum("JAM COUNT", &d.p2NeyukiJamCount, 0, NEYUKI_JAM_COUNT_MAX, 1, 1, OnAutoApply);
        rows[n++] = Toggle("LOCK JAM COUNT", &d.p2NeyukiLockJam, OnAutoApply);
    }
}

void AddKanoRows(Row* rows, int& n, DisplayData& d, int player) {
    if (player == 1) {
        rows[n++] = Toggle("LOCK MAGIC", &d.p1KanoLockMagic, OnAutoApply);
        rows[n++] = IntNum("MAGIC", &d.p1KanoMagic, 0, KANO_MAGIC_MAX, 100, 1000, OnAutoApply);
    } else {
        rows[n++] = Toggle("LOCK MAGIC", &d.p2KanoLockMagic, OnAutoApply);
        rows[n++] = IntNum("MAGIC", &d.p2KanoMagic, 0, KANO_MAGIC_MAX, 100, 1000, OnAutoApply);
    }
}

void AddRumiRows(Row* rows, int& n, DisplayData& d, int player) {
    if (player == 1) {
        rows[n++] = Toggle("INFINITE SHINAI", &d.p1RumiInfiniteShinai, OnAutoApply);
        rows[n++] = Toggle("BAREHANDED MODE", &d.p1RumiBarehanded, OnAutoApply);
        rows[n++] = Toggle("KIMCHI ACTIVE", &d.p1RumiKimchiActive, OnAutoApply);
        rows[n++] = Toggle("INFINITE KIMCHI", &d.p1RumiInfiniteKimchi, OnAutoApply);
        rows[n++] = IntNum("KIMCHI TIMER", &d.p1RumiKimchiTimer, 0, RUMI_KIMCHI_TARGET, 30, 300, OnAutoApply);
    } else {
        rows[n++] = Toggle("INFINITE SHINAI", &d.p2RumiInfiniteShinai, OnAutoApply);
        rows[n++] = Toggle("BAREHANDED MODE", &d.p2RumiBarehanded, OnAutoApply);
        rows[n++] = Toggle("KIMCHI ACTIVE", &d.p2RumiKimchiActive, OnAutoApply);
        rows[n++] = Toggle("INFINITE KIMCHI", &d.p2RumiInfiniteKimchi, OnAutoApply);
        rows[n++] = IntNum("KIMCHI TIMER", &d.p2RumiKimchiTimer, 0, RUMI_KIMCHI_TARGET, 30, 300, OnAutoApply);
    }
}

void AddDoppelRows(Row* rows, int& n, DisplayData& d, int player) {
    rows[n++] = Toggle("ENLIGHTENED", player == 1 ? &d.p1DoppelEnlightened : &d.p2DoppelEnlightened, OnAutoApply);
}

void AddMioRows(Row* rows, int& n, DisplayData& d, int player) {
    if (player == 1) {
        rows[n++] = ChoicesRow("STANCE", &d.p1MioStance, kStanceChoices, 2, OnAutoApply);
        rows[n++] = Toggle("LOCK STANCE", &d.p1MioLockStance, OnAutoApply);
    } else {
        rows[n++] = ChoicesRow("STANCE", &d.p2MioStance, kStanceChoices, 2, OnAutoApply);
        rows[n++] = Toggle("LOCK STANCE", &d.p2MioLockStance, OnAutoApply);
    }
}

void AddMaiRows(Row* rows, int& n, DisplayData& d, int player) {
    if (player == 1) {
        rows[n++] = ChoicesRow("STATUS", &d.p1MaiStatus, kMaiStatusChoices, 5, OnAutoApply);
        rows[n++] = IntNum("GHOST TIME", &d.p1MaiGhostTime, 0, MAI_GHOST_TIME_MAX, 60, 600, OnAutoApply);
        rows[n++] = IntNum("CHARGE TIMER", &d.p1MaiGhostCharge, 0, MAI_GHOST_CHARGE_MAX, 60, 600, OnAutoApply);
        rows[n++] = IntNum("AWAKEN TIMER", &d.p1MaiAwakeningTime, 0, MAI_AWAKENING_MAX, 60, 600, OnAutoApply);
        rows[n++] = Toggle("INFINITE GHOST", &d.p1MaiInfiniteGhost, OnAutoApply);
        rows[n++] = Toggle("INFINITE CHARGE", &d.p1MaiInfiniteCharge, OnAutoApply);
        rows[n++] = Toggle("INFINITE AWAKEN", &d.p1MaiInfiniteAwakening, OnAutoApply);
        rows[n++] = Toggle("NO CHARGE COOLDOWN", &d.p1MaiNoChargeCD, OnAutoApply);
        rows[n++] = Toggle("AGGRESSIVE SUMMON", &d.p1MaiAggressiveOverride, OnAutoApply);
        rows[n++] = Action("FORCE SUMMON", ForceP1MaiSummon);
        rows[n++] = Action("FORCE DESPAWN", ForceP1MaiDespawn);
        rows[n++] = DoubleNum("GHOST TARGET X", &d.p1MaiGhostSetX, -2000.0, 2000.0, 1.0, 10.0, "%.1f");
        rows[n++] = DoubleNum("GHOST TARGET Y", &d.p1MaiGhostSetY, -2000.0, 2000.0, 1.0, 10.0, "%.1f");
        rows[n++] = Action("APPLY GHOST POSITION", ApplyP1MaiGhostPos);
    } else {
        rows[n++] = ChoicesRow("STATUS", &d.p2MaiStatus, kMaiStatusChoices, 5, OnAutoApply);
        rows[n++] = IntNum("GHOST TIME", &d.p2MaiGhostTime, 0, MAI_GHOST_TIME_MAX, 60, 600, OnAutoApply);
        rows[n++] = IntNum("CHARGE TIMER", &d.p2MaiGhostCharge, 0, MAI_GHOST_CHARGE_MAX, 60, 600, OnAutoApply);
        rows[n++] = IntNum("AWAKEN TIMER", &d.p2MaiAwakeningTime, 0, MAI_AWAKENING_MAX, 60, 600, OnAutoApply);
        rows[n++] = Toggle("INFINITE GHOST", &d.p2MaiInfiniteGhost, OnAutoApply);
        rows[n++] = Toggle("INFINITE CHARGE", &d.p2MaiInfiniteCharge, OnAutoApply);
        rows[n++] = Toggle("INFINITE AWAKEN", &d.p2MaiInfiniteAwakening, OnAutoApply);
        rows[n++] = Toggle("NO CHARGE COOLDOWN", &d.p2MaiNoChargeCD, OnAutoApply);
        rows[n++] = Toggle("AGGRESSIVE SUMMON", &d.p2MaiAggressiveOverride, OnAutoApply);
        rows[n++] = Action("FORCE SUMMON", ForceP2MaiSummon);
        rows[n++] = Action("FORCE DESPAWN", ForceP2MaiDespawn);
        rows[n++] = DoubleNum("GHOST TARGET X", &d.p2MaiGhostSetX, -2000.0, 2000.0, 1.0, 10.0, "%.1f");
        rows[n++] = DoubleNum("GHOST TARGET Y", &d.p2MaiGhostSetY, -2000.0, 2000.0, 1.0, 10.0, "%.1f");
        rows[n++] = Action("APPLY GHOST POSITION", ApplyP2MaiGhostPos);
    }
}

void AddMinagiRows(Row* rows, int& n, DisplayData& d, int player) {
    if (player == 1) {
        rows[n++] = Toggle("ALWAYS READIED", &d.p1MinagiAlwaysReadied, OnAutoApply);
        rows[n++] = DoubleNum("MICHIRU TARGET X", &d.p1MinagiPuppetSetX, -2000.0, 2000.0, 1.0, 10.0, "%.1f");
        rows[n++] = DoubleNum("MICHIRU TARGET Y", &d.p1MinagiPuppetSetY, -2000.0, 2000.0, 1.0, 10.0, "%.1f");
        rows[n++] = Action("APPLY MICHIRU POSITION", ApplyP1MinagiPos);
    } else {
        rows[n++] = Toggle("ALWAYS READIED", &d.p2MinagiAlwaysReadied, OnAutoApply);
        rows[n++] = DoubleNum("MICHIRU TARGET X", &d.p2MinagiPuppetSetX, -2000.0, 2000.0, 1.0, 10.0, "%.1f");
        rows[n++] = DoubleNum("MICHIRU TARGET Y", &d.p2MinagiPuppetSetY, -2000.0, 2000.0, 1.0, 10.0, "%.1f");
        rows[n++] = Action("APPLY MICHIRU POSITION", ApplyP2MinagiPos);
    }
}

void AddPlayerCharacterRows(Row* rows, int& n, DisplayData& d, int player, int charId, const char* rawName) {
    static char s_headers[2][48];
    char* header = s_headers[(player == 2) ? 1 : 0];
    const char* name = (rawName && rawName[0]) ? rawName : "(NONE)";
    _snprintf_s(header, sizeof(s_headers[0]), _TRUNCATE, "P%d  %s", player, name);
    rows[n++] = Header(header);

    switch (charId) {
        case CHAR_ID_IKUMI:    AddIkumiRows(rows, n, d, player); break;
        case CHAR_ID_MISUZU:   AddMisuzuRows(rows, n, d, player); break;
        case CHAR_ID_MISHIO:   AddMishioRows(rows, n, d, player); break;
        case CHAR_ID_AKIKO:    AddAkikoRows(rows, n, d, player); break;
        case CHAR_ID_NAYUKIB:  AddNayukiRows(rows, n, d, player); break;
        case CHAR_ID_NAYUKI:   AddNeyukiRows(rows, n, d, player); break;
        case CHAR_ID_KANO:     AddKanoRows(rows, n, d, player); break;
        case CHAR_ID_NANASE:   AddRumiRows(rows, n, d, player); break;
        case CHAR_ID_EXNANASE: AddDoppelRows(rows, n, d, player); break;
        case CHAR_ID_MIO:      AddMioRows(rows, n, d, player); break;
        case CHAR_ID_MAI:      AddMaiRows(rows, n, d, player); break;
        case CHAR_ID_MINAGI:   AddMinagiRows(rows, n, d, player); break;
        default:
            rows[n++] = Info("NO CHARACTER-SPECIFIC CONTROLS");
            break;
    }
}

Row* BuildCharsRows(int& count) {
    static Row s_rows[96];
    int n = 0;
    auto& d = ImGuiGui::guiState.localData;

    s_rows[n++] = Header("CURRENT MATCHUP");

    static char s_p1Label[32];
    static char s_p2Label[32];
    _snprintf_s(s_p1Label, sizeof(s_p1Label), _TRUNCATE,
                "P1  %-12s ID %d", d.p1CharName[0] ? d.p1CharName : "(none)", d.p1CharID);
    _snprintf_s(s_p2Label, sizeof(s_p2Label), _TRUNCATE,
                "P2  %-12s ID %d", d.p2CharName[0] ? d.p2CharName : "(none)", d.p2CharID);
    s_rows[n++] = Info(s_p1Label);
    s_rows[n++] = Info(s_p2Label);
    s_rows[n++] = Action("REFRESH CHARACTER DATA", RefreshCharacterDataAction);
    s_rows[n++] = Action("RUN P1 FINAL MEMORY", RunP1FinalMemory);
    s_rows[n++] = Action("RUN P2 FINAL MEMORY", RunP2FinalMemory);

    s_rows[n++] = Spacer();
    AddCharacterLockRows(s_rows, n);
    AddPlayerCharacterRows(s_rows, n, d, 1, d.p1CharID, d.p1CharName);
    s_rows[n++] = Spacer();
    AddPlayerCharacterRows(s_rows, n, d, 2, d.p2CharID, d.p2CharName);

    if (!CharHasCustomRows(d.p1CharID) && !CharHasCustomRows(d.p2CharID)) {
        s_rows[n++] = Spacer();
        s_rows[n++] = Header("STATUS");
        s_rows[n++] = Info("NO SUPPORTED CHARACTER CONTROLS IN THIS MATCHUP");
    }

    count = n;
    return s_rows;
}

// ===== OPPONENT screen =====
const char* const kAirtechDirChoices[3] = { "NEUTRAL", "FORWARD", "BACK" };
const char* const kJumpDirChoices[3]    = { "NEUTRAL", "FORWARD", "BACK" };
const char* const kJumpTargetChoices[3] = { "P1", "P2", "BOTH" };
const char* const kDummyBlockChoices[4] = { "OFF", "ALL", "FIRST HIT", "AFTER HIT" };
const char* const kDummyStanceChoices[3] = { "STAND", "JUMP", "CROUCH" };

bool AdaptiveHidesStance() { return g_mirrorAdaptiveStance; }

// DisplayData::airtechDelay / jumpDirection etc. are plain ints; no indirection
// mirror needed. Just bind rows to them directly and call OnAutoApply on change.

Row* BuildOpponentRows(int& count) {
    static Row s_rows[32];
    int n = 0;
    auto& d = ImGuiGui::guiState.localData;

    s_rows[n++] = Header("OPPONENT");
    s_rows[n++] = Toggle("ENABLE P2 CONTROL",   &d.p2ControlEnabled, OnAutoApply);

    s_rows[n++] = Header("DEFENSE");
    s_rows[n++] = ChoicesRow("DUMMY AUTO-BLOCK",       &g_mirrorDummyBlockMode,    kDummyBlockChoices, 4, OnDummyBlockMode);
    s_rows[n++] = Toggle    ("RANDOM BLOCK",           &g_mirrorRandomBlock,       OnRandomBlock);
    s_rows[n++] = Toggle    ("ADAPTIVE STANCE",        &g_mirrorAdaptiveStance,    OnAdaptiveStance);
    s_rows[n++] = ChoicesRow("DUMMY STANCE",           &g_mirrorPracticeStance,    kDummyStanceChoices, 3, OnPracticeStance, AdaptiveHidesStance);
    s_rows[n++] = Toggle    ("ALWAYS RECOIL GUARD",    &g_mirrorAlwaysRG,          OnAlwaysRG);
    s_rows[n++] = Toggle    ("RANDOM RECOIL GUARD",    &g_mirrorRandomRG,          OnRandomRG);
    s_rows[n++] = Toggle    ("COUNTER RG",             &g_mirrorCounterRG,         OnCounterRGToggle);

    s_rows[n++] = Header("RECOVERY");
    s_rows[n++] = Toggle   ("AUTO-AIRTECH",            &d.autoAirtech,      OnAutoApply);
    s_rows[n++] = ChoicesRow("  AIRTECH DIRECTION",    &d.airtechDirection, kAirtechDirChoices, 3, OnAutoApply);
    s_rows[n++] = IntNum   ("  AIRTECH DELAY",         &d.airtechDelay,     0, 60, 1, 5, OnAutoApply);

    s_rows[n++] = Header("MOVEMENT");
    s_rows[n++] = Toggle   ("AUTO-JUMP",               &d.autoJump,        OnAutoApply);
    s_rows[n++] = ChoicesRow("  JUMP DIRECTION",       &d.jumpDirection,   kJumpDirChoices, 3, OnAutoApply);
    s_rows[n++] = ChoicesRow("  JUMP TARGET",          &d.jumpTarget,      kJumpTargetChoices, 3, OnAutoApply);

    count = n;
    return s_rows;
}

// ===== OPTIONS screen =====
void OnFaOverlayPersist()  { g_showFrameAdvantageOverlay.store(g_mirrorFaOverlay); }

// ===== Continuous Recovery mirrors =====
const char* const kCrHpModeChoices[]    = { "OFF", "MAX", "FM (3332)", "CUSTOM" };
const char* const kCrMeterModeChoices[] = { "OFF", "0", "1000", "2000", "3000", "CUSTOM" };
const char* const kCrRfModeChoices[]    = { "OFF", "ZERO", "FULL (1000)", "RED (500)", "RED MAX (999)", "CUSTOM" };

bool   g_crEnabledP1, g_crEnabledP2;
int    g_crHpModeP1, g_crHpModeP2;
int    g_crHpCustomP1, g_crHpCustomP2;
int    g_crMeterModeP1, g_crMeterModeP2;
int    g_crMeterCustomP1, g_crMeterCustomP2;
int    g_crRfModeP1, g_crRfModeP2;
float  g_crRfCustomP1, g_crRfCustomP2;
bool   g_crForceBlueICP1, g_crForceBlueICP2;

void RefreshCrMirrors() {
    g_crEnabledP1     = g_contRecEnabledP1.load();
    g_crHpModeP1      = g_contRecHpModeP1.load();
    g_crHpCustomP1    = g_contRecHpCustomP1.load();
    g_crMeterModeP1   = g_contRecMeterModeP1.load();
    g_crMeterCustomP1 = g_contRecMeterCustomP1.load();
    g_crRfModeP1      = g_contRecRfModeP1.load();
    g_crRfCustomP1    = (float)g_contRecRfCustomP1.load();
    g_crForceBlueICP1 = g_contRecRfForceBlueICP1.load();

    g_crEnabledP2     = g_contRecEnabledP2.load();
    g_crHpModeP2      = g_contRecHpModeP2.load();
    g_crHpCustomP2    = g_contRecHpCustomP2.load();
    g_crMeterModeP2   = g_contRecMeterModeP2.load();
    g_crMeterCustomP2 = g_contRecMeterCustomP2.load();
    g_crRfModeP2      = g_contRecRfModeP2.load();
    g_crRfCustomP2    = (float)g_contRecRfCustomP2.load();
    g_crForceBlueICP2 = g_contRecRfForceBlueICP2.load();
}

void OnCrEnabledP1()     { g_contRecEnabledP1.store(g_crEnabledP1); }
void OnCrEnabledP2()     { g_contRecEnabledP2.store(g_crEnabledP2); }
void OnCrHpModeP1()      { g_contRecHpModeP1.store(g_crHpModeP1); }
void OnCrHpModeP2()      { g_contRecHpModeP2.store(g_crHpModeP2); }
void OnCrHpCustomP1()    { g_contRecHpCustomP1.store(g_crHpCustomP1); }
void OnCrHpCustomP2()    { g_contRecHpCustomP2.store(g_crHpCustomP2); }
void OnCrMeterModeP1()   { g_contRecMeterModeP1.store(g_crMeterModeP1); }
void OnCrMeterModeP2()   { g_contRecMeterModeP2.store(g_crMeterModeP2); }
void OnCrMeterCustomP1() { g_contRecMeterCustomP1.store(g_crMeterCustomP1); }
void OnCrMeterCustomP2() { g_contRecMeterCustomP2.store(g_crMeterCustomP2); }
void OnCrRfModeP1()      { g_contRecRfModeP1.store(g_crRfModeP1); }
void OnCrRfModeP2()      { g_contRecRfModeP2.store(g_crRfModeP2); }
void OnCrRfCustomP1()    { g_contRecRfCustomP1.store((double)g_crRfCustomP1); }
void OnCrRfCustomP2()    { g_contRecRfCustomP2.store((double)g_crRfCustomP2); }
void OnCrForceBlueICP1() { g_contRecRfForceBlueICP1.store(g_crForceBlueICP1); }
void OnCrForceBlueICP2() { g_contRecRfForceBlueICP2.store(g_crForceBlueICP2); }

bool CrP1HpCustomHidden()    { return g_crHpModeP1    != 3; }
bool CrP1MeterCustomHidden() { return g_crMeterModeP1 != 5; }
bool CrP1RfCustomHidden()    { return g_crRfModeP1    != 5; }
bool CrP1RfBicHidden()       { return g_crRfModeP1    != 5; }
bool CrP2HpCustomHidden()    { return g_crHpModeP2    != 3; }
bool CrP2MeterCustomHidden() { return g_crMeterModeP2 != 5; }
bool CrP2RfCustomHidden()    { return g_crRfModeP2    != 5; }
bool CrP2RfBicHidden()       { return g_crRfModeP2    != 5; }

// ===== F4 / F5 engine recovery mirrors =====
const char* const kF5ModeChoices[3] = { "DISABLED", "FULL VALUES", "FM (3332)" };
const char* const kF4ModeChoices[3] = { "DISABLED", "FULL BLUE (1000)", "CUSTOM" };
const char* const kF4ColorChoices[2] = { "RED", "BLUE" };

int  g_f5Mode = 0;   // 0=Disabled, 1=Full, 2=FM
int  g_f4Mode = 0;   // 0=Disabled, 1=Full Blue, 2=Custom
int  g_f4Color = 0;  // 0=Red, 1=Blue (only for custom)
int  g_f4RfAmount = 1000;

void RefreshEngineRegenMirrors() {
    uint16_t a = 0, b = 0;
    if (!ReadEngineRegenParams(a, b)) return;
    if (b == 3332) { g_f5Mode = 2; g_f4Mode = 0; }
    else if ((a == 1000 || a == 2000) && b == 9999) { g_f4Mode = 1; g_f5Mode = 0; }
    else if (b == 9999 && a > 0) {
        g_f4Mode = 2;
        float rf = 0.0f; bool blue = false;
        if (DeriveRfFromParamA(a, rf, blue)) {
            g_f4Color = blue ? 1 : 0;
            g_f4RfAmount = (int)rf;
        }
    }
    else if (a == 0 && b == 0) { g_f5Mode = 0; g_f4Mode = 0; }
}

void ApplyF5() {
    switch (g_f5Mode) {
        case 0: WriteEngineRegenParams(0, 0); break;
        case 1: ForceEngineF5Full(); break;
        case 2: WriteEngineRegenParams(1000, 3332); break;
    }
}
void ApplyF4Custom() {
    if (g_f4Mode != 2) return;
    uint16_t a = (uint16_t)((g_f4Color == 1) ? (2000 - g_f4RfAmount) : g_f4RfAmount);
    WriteEngineRegenParams(a, 9999);
}
void OnF5Mode()    { ApplyF5(); }
void OnF4Mode() {
    if (g_f4Mode == 0) WriteEngineRegenParams(0, 0);
    else if (g_f4Mode == 1) WriteEngineRegenParams(1000, 9999);
    else ApplyF4Custom();
}
void OnF4Custom() { if (g_f4Mode == 2) ApplyF4Custom(); }
bool F4CustomHidden() { return g_f4Mode != 2; }

// Framestep
int  g_framestepMode = 0;     // 0=FullFrame, 1=Subframe
bool VanillaHidden() { return GetEfzRevivalVersion() != EfzRevivalVersion::Vanilla; }
void RefreshFramestepMirror() {
    g_framestepMode = (Framestep::GetStepMode() == Framestep::StepMode::Subframe) ? 1 : 0;
}
void OnFramestepMode() {
    Framestep::SetStepMode(g_framestepMode == 1 ? Framestep::StepMode::Subframe
                                                : Framestep::StepMode::FullFrame);
}
const char* const kFramestepChoices[2] = { "FULL FRAMES", "SUBFRAMES" };

Row* BuildOptionsRows(int& count) {
    static Row s_rows[80];
    int n = 0;
    auto& s = MutableSettings();

    s_rows[n++] = Header("GAMEPLAY");
    s_rows[n++] = Toggle ("FINAL MEMORY AT ANY HP",   &g_mirrorFmBypass,         OnFmBypass);

    s_rows[n++] = Header("AUTO RECOVERY (F5)");
    s_rows[n++] = ChoicesRow("F5 MODE",               &g_f5Mode, kF5ModeChoices, 3, OnF5Mode);

    s_rows[n++] = Header("RF RECOVERY (F4)");
    s_rows[n++] = ChoicesRow("F4 MODE",               &g_f4Mode, kF4ModeChoices, 3, OnF4Mode);
    s_rows[n++] = ChoicesRow("  COLOR",               &g_f4Color, kF4ColorChoices, 2, OnF4Custom, nullptr, F4CustomHidden);
    s_rows[n++] = IntNum    ("  RF AMOUNT",           &g_f4RfAmount, 0, 1000, 50, 100, OnF4Custom, nullptr, F4CustomHidden);

    s_rows[n++] = Header("CONTINUOUS RECOVERY P1");
    s_rows[n++] = Toggle    ("  ENABLE P1",           &g_crEnabledP1,     OnCrEnabledP1);
    s_rows[n++] = ChoicesRow("  HP MODE",             &g_crHpModeP1,      kCrHpModeChoices,    4, OnCrHpModeP1);
    s_rows[n++] = IntNum    ("  HP CUSTOM",           &g_crHpCustomP1,    0, 9999, 100, 1000, OnCrHpCustomP1, nullptr, CrP1HpCustomHidden);
    s_rows[n++] = ChoicesRow("  METER MODE",          &g_crMeterModeP1,   kCrMeterModeChoices, 6, OnCrMeterModeP1);
    s_rows[n++] = IntNum    ("  METER CUSTOM",        &g_crMeterCustomP1, 0, 3000, 50, 500,   OnCrMeterCustomP1, nullptr, CrP1MeterCustomHidden);
    s_rows[n++] = ChoicesRow("  RF MODE",             &g_crRfModeP1,      kCrRfModeChoices,    6, OnCrRfModeP1);
    s_rows[n++] = FloatNum  ("  RF CUSTOM",           &g_crRfCustomP1,    0.0f, 1000.0f, 10.0f, 100.0f, "%.0f", OnCrRfCustomP1, nullptr, CrP1RfCustomHidden);
    s_rows[n++] = Toggle    ("  RF FORCE BLUE IC",    &g_crForceBlueICP1, OnCrForceBlueICP1, nullptr, CrP1RfBicHidden);

    s_rows[n++] = Header("CONTINUOUS RECOVERY P2");
    s_rows[n++] = Toggle    ("  ENABLE P2",           &g_crEnabledP2,     OnCrEnabledP2);
    s_rows[n++] = ChoicesRow("  HP MODE",             &g_crHpModeP2,      kCrHpModeChoices,    4, OnCrHpModeP2);
    s_rows[n++] = IntNum    ("  HP CUSTOM",           &g_crHpCustomP2,    0, 9999, 100, 1000, OnCrHpCustomP2, nullptr, CrP2HpCustomHidden);
    s_rows[n++] = ChoicesRow("  METER MODE",          &g_crMeterModeP2,   kCrMeterModeChoices, 6, OnCrMeterModeP2);
    s_rows[n++] = IntNum    ("  METER CUSTOM",        &g_crMeterCustomP2, 0, 3000, 50, 500,   OnCrMeterCustomP2, nullptr, CrP2MeterCustomHidden);
    s_rows[n++] = ChoicesRow("  RF MODE",             &g_crRfModeP2,      kCrRfModeChoices,    6, OnCrRfModeP2);
    s_rows[n++] = FloatNum  ("  RF CUSTOM",           &g_crRfCustomP2,    0.0f, 1000.0f, 10.0f, 100.0f, "%.0f", OnCrRfCustomP2, nullptr, CrP2RfCustomHidden);
    s_rows[n++] = Toggle    ("  RF FORCE BLUE IC",    &g_crForceBlueICP2, OnCrForceBlueICP2, nullptr, CrP2RfBicHidden);

    s_rows[n++] = Header("OVERLAYS");
    s_rows[n++] = Toggle ("FRAME ADVANTAGE OVERLAY",  &g_mirrorFaOverlay,        OnFaOverlayPersist);
    s_rows[n++] = Toggle ("COMBO STATS OVERLAY",      &s.showComboStatisticsOverlay, OnShowCombo);
    s_rows[n++] = Toggle ("  SHOW DETAIL ROW",        &s.comboOverlayShowDetailRow,
        [](){ PersistBool("General", "comboOverlayShowDetailRow", MutableSettings().comboOverlayShowDetailRow); });
    s_rows[n++] = Toggle ("  KEEP FINAL SUMMARY",     &s.comboOverlayShowFinalSummary,
        [](){ PersistBool("General", "comboOverlayShowFinalSummary", MutableSettings().comboOverlayShowFinalSummary); });
    s_rows[n++] = Toggle ("  HIDE WITH MENU",         &s.comboOverlayHideWhenImGuiVisible,
        [](){ PersistBool("General", "comboOverlayHideWhenImGuiVisible", MutableSettings().comboOverlayHideWhenImGuiVisible); });
    s_rows[n++] = Toggle ("  SHOW RF MULTIPLIER",     &s.comboOverlayShowRfMultiplier,
        [](){ PersistBool("General", "comboOverlayShowRfMultiplier", MutableSettings().comboOverlayShowRfMultiplier); });

    s_rows[n++] = Header("TIMING");
    s_rows[n++] = FloatNum("FA DURATION (SEC)",      &s.frameAdvantageDisplayDuration, 0.5f, 30.0f, 0.1f, 1.0f, "%.1f", OnFADuration);

    s_rows[n++] = Header("FRAMESTEP (VANILLA EFZ)");
    s_rows[n++] = ChoicesRow("STEP MODE",            &g_framestepMode, kFramestepChoices, 2, OnFramestepMode, nullptr, VanillaHidden);

    count = n;
    return s_rows;
}

} // namespace

// ===== Public per-screen entry points =====

// ===== Macros stub =====
void MacroRecord()    { MacroController::ToggleRecord(); }
void MacroPlay()      { MacroController::Play(); }
void MacroStop()      { MacroController::Stop(); }

const char* MacroStateStr() {
    static char buf[64];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "SLOTS: %d",
                MacroController::GetSlotCount());
    return buf;
}

Row* BuildMacrosRows(int& count) {
    static Row s_rows[10];
    int n = 0;
    s_rows[n++] = Header("MACROS");
    s_rows[n++] = Action("RECORD (TOGGLE)", MacroRecord, MacroStateStr);
    s_rows[n++] = Action("PLAY",            MacroPlay);
    s_rows[n++] = Action("STOP",            MacroStop);
    s_rows[n++] = Spacer();
    s_rows[n++] = Info("ADVANCED MACRO EDITOR LIVES IN");
    s_rows[n++] = Info("THE IMGUI MENU FOR NOW.");
    count = n;
    return s_rows;
}

void RefreshSecondaryScreenMirrors() {
    RefreshAutoMirrors();
    RefreshCharMirrors();
    RefreshOpponentMirrors();
    RefreshOptionsMirrors();
    RefreshHelpStrings();
    RefreshHotkeyStrings();
    RefreshMacroSlotChoices();
    RefreshDebugMirrors();
    RefreshCrMirrors();
    RefreshEngineRegenMirrors();
    RefreshFramestepMirror();
}

static void TickListScreen(ImDrawList* dl, const ScreenLayout& layout,
                           const char* title, Row* rows, int n,
                           int& focus, ScrollState& scroll, bool& backEdge) {
    ClampFocus(rows, n, focus);
    backEdge = HandleListInput(layout, rows, n, focus, scroll);
    RenderList(dl, layout, title, rows, n, focus, scroll);
}

// ===== MAIN sub-panes =====
void TickOpponent(ImDrawList* dl, const ScreenLayout& layout, int& focus, ScrollState& scroll, bool& backEdge) {
    int n = 0; Row* rows = BuildOpponentRows(n);
    TickListScreen(dl, layout, "OPPONENT", rows, n, focus, scroll, backEdge);
}
void TickOptions(ImDrawList* dl, const ScreenLayout& layout, int& focus, ScrollState& scroll, bool& backEdge) {
    int n = 0; Row* rows = BuildOptionsRows(n);
    TickListScreen(dl, layout, "OPTIONS", rows, n, focus, scroll, backEdge);
}

// ===== AUTO sub-panes =====
void TickTriggers(ImDrawList* dl, const ScreenLayout& layout, int& focus, ScrollState& scroll, bool& backEdge) {
    int n = 0; Row* rows = BuildTriggersRows(n);
    TickListScreen(dl, layout, "TRIGGERS", rows, n, focus, scroll, backEdge);
}
void TickMacros(ImDrawList* dl, const ScreenLayout& layout, int& focus, ScrollState& scroll, bool& backEdge) {
    int n = 0; Row* rows = BuildMacrosRows(n);
    TickListScreen(dl, layout, "MACROS", rows, n, focus, scroll, backEdge);
}

// ===== CHARS =====
void TickChars(ImDrawList* dl, const ScreenLayout& layout, int& focus, ScrollState& scroll, bool& backEdge) {
    int n = 0; Row* rows = BuildCharsRows(n);
    TickListScreen(dl, layout, "CHARACTERS", rows, n, focus, scroll, backEdge);
}

// ===== SETTINGS sub-panes =====
void TickSettingsGeneral(ImDrawList* dl, const ScreenLayout& layout, int& focus, ScrollState& scroll, bool& backEdge) {
    int n = 0; Row* rows = BuildSettingsGeneralRows(n);
    TickListScreen(dl, layout, "GENERAL", rows, n, focus, scroll, backEdge);
}
void TickSettingsHotkeys(ImDrawList* dl, const ScreenLayout& layout, int& focus, ScrollState& scroll, bool& backEdge) {
    int n = 0; Row* rows = BuildSettingsHotkeysRows(n);
    TickListScreen(dl, layout, "HOTKEYS", rows, n, focus, scroll, backEdge);
}
void TickSettingsDebug(ImDrawList* dl, const ScreenLayout& layout, int& focus, ScrollState& scroll, bool& backEdge) {
    int n = 0; Row* rows = BuildSettingsDebugRows(n);
    TickListScreen(dl, layout, "DEBUG", rows, n, focus, scroll, backEdge);
}

// ===== HELP sub-panes =====
void TickHelpAbout(ImDrawList* dl, const ScreenLayout& layout, int& focus, ScrollState& scroll, bool& backEdge) {
    int n = 0; Row* rows = BuildHelpAboutRows(n);
    TickListScreen(dl, layout, "ABOUT", rows, n, focus, scroll, backEdge);
}
void TickHelpHotkeys(ImDrawList* dl, const ScreenLayout& layout, int& focus, ScrollState& scroll, bool& backEdge) {
    int n = 0; Row* rows = BuildHelpHotkeysRows(n);
    TickListScreen(dl, layout, "HOTKEYS", rows, n, focus, scroll, backEdge);
}
void TickHelpControls(ImDrawList* dl, const ScreenLayout& layout, int& focus, ScrollState& scroll, bool& backEdge) {
    int n = 0; Row* rows = BuildHelpControlsRows(n);
    TickListScreen(dl, layout, "CONTROLS", rows, n, focus, scroll, backEdge);
}

} // namespace CustomMenu::Screens
