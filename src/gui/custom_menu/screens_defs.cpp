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
#include "../include/game/practice_offsets.h"
#include "../include/game/game_state.h"
#include "../include/game/always_rg.h"
#include "../include/game/random_rg.h"
#include "../include/game/random_block.h"
#include "../include/game/final_memory_patch.h"
#include "../include/game/macro_controller.h"
#include "../include/game/custom_savestate.h"
#include "../include/game/savestate_hook.h"
#include "../include/game/fm_commands.h"
#include "../include/game/character_settings.h"
#include "../include/game/character_hotswap.h"
#include "../include/gui/overlay.h"
#include "../include/gui/framebar.h"
#include "../include/utils/controller_names.h"
#include "../include/utils/xinput_shim.h"
#include "../include/utils/network.h"
#include "../include/utils/pause_integration.h"
#include "../include/utils/switch_players.h"
#include "../include/utils/bgm_control.h"
#include "../include/utils/audio_control.h"
#include "../include/utils/extended_config_bridge.h"
#include "../include/input/framestep.h"
#include "../include/input/input_motion.h"
#include "../include/core/memory.h"
#include "../include/core/logger.h"
#include "../include/gui/gif_player.h"
#include "../3rdparty/imgui/imgui.h"

#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

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
bool HasMizuka()   { return P1Or(CHAR_ID_MIZUKA) || P1Or(CHAR_ID_NAGAMORI); }

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
           !HasMio()   && !HasNeyuki() && !HasMai()    && !HasMinagi() &&
           !HasMizuka();
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
void OnComboDetailRow()  { PersistBool ("General", "comboOverlayShowDetailRow",     MutableSettings().comboOverlayShowDetailRow); }
void OnComboDetailSrc()  { PersistInt  ("General", "comboOverlayDetailRowSource",   MutableSettings().comboOverlayDetailRowSource); }
void OnComboFinal()      { PersistBool ("General", "comboOverlayShowFinalSummary",  MutableSettings().comboOverlayShowFinalSummary); }
void OnComboDuration()   { PersistFloat("General", "comboOverlayDisplayDuration",   MutableSettings().comboOverlayDisplayDuration); }
void OnComboHideMenu()   { PersistBool ("General", "comboOverlayHideWhenImGuiVisible", MutableSettings().comboOverlayHideWhenImGuiVisible); }
void OnComboResume()     { PersistBool ("General", "comboOverlayResumeAfterImGui",  MutableSettings().comboOverlayResumeAfterImGui); }
void OnComboRfMult()     { PersistBool ("General", "comboOverlayShowRfMultiplier",  MutableSettings().comboOverlayShowRfMultiplier); }
void OnComboRawScale()   { PersistBool ("General", "comboOverlayShowRawScale",      MutableSettings().comboOverlayShowRawScale); }
void OnCollisionDisplayHitboxes()    { PersistBool("General", "collisionDisplayHitboxes", MutableSettings().collisionDisplayHitboxes); }
void OnCollisionDisplayHurtboxes()   { PersistBool("General", "collisionDisplayHurtboxes", MutableSettings().collisionDisplayHurtboxes); }
void OnCollisionDisplayPushboxes()   { PersistBool("General", "collisionDisplayCollisionBoxes", MutableSettings().collisionDisplayCollisionBoxes); }
void OnCollisionDisplayProjectiles() { PersistBool("General", "collisionDisplayProjectileInteractions", MutableSettings().collisionDisplayProjectileInteractions); }
void OnCollisionDisplayAlpha()       { PersistInt ("General", "collisionDisplayFillAlphaPercent", MutableSettings().collisionDisplayFillAlphaPercent); }
void OnCollisionProjectileBoxes()    { PersistBool("General", "collisionDisplayProjectileBoxes", MutableSettings().collisionDisplayProjectileBoxes); }
void OnCollisionProjectileOrigins()  { PersistBool("General", "collisionDisplayProjectileOrigins", MutableSettings().collisionDisplayProjectileOrigins); }
void OnCollisionProjectileIntersections() { PersistBool("General", "collisionDisplayProjectileIntersections", MutableSettings().collisionDisplayProjectileIntersections); }
void OnCollisionNagamoriRanges()     { PersistBool("General", "collisionDisplayNagamoriRanges", MutableSettings().collisionDisplayNagamoriRanges); }
void OnCollisionNagamoriAffected()   { PersistBool("General", "collisionDisplayNagamoriAffected", MutableSettings().collisionDisplayNagamoriAffected); }
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

void ReloadSettingsFromDisk() {
    if (Config::LoadSettings()) {
        detailedLogging.store(Config::GetSettings().detailedLogging);
        AudioControl::ApplyConfiguredVolumesNow();
        LogOut("[CONFIG/UI] Settings reloaded from ini", false);
        DirectDrawHook::AddMessage("Settings reloaded from disk", "SYSTEM", RGB(180, 255, 220), 1200, 0, 120);
    } else {
        LogOut("[CONFIG/UI] Settings reload failed", true);
        DirectDrawHook::AddMessage("Settings reload failed", "SYSTEM", RGB(255, 120, 120), 1400, 0, 120);
    }
}

const char* CurrentConfigPathInfo() {
    static char buf[320];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "Config file: %s", Config::GetConfigFilePath().c_str());
    return buf;
}

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

int GetMotionIndexForAction(int action);

// Per-trigger motion index mirrors (0..23 grouped action space).
int g_motionIdxAB, g_motionIdxWU, g_motionIdxAH, g_motionIdxAA, g_motionIdxRG;
int g_mirrorFwdDashFollowup = 0;

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

    g_motionIdxAB = GetMotionIndexForAction(d.actionAfterBlock);
    g_motionIdxWU = GetMotionIndexForAction(d.actionOnWakeup);
    g_motionIdxAH = GetMotionIndexForAction(d.actionAfterHitstun);
    g_motionIdxAA = GetMotionIndexForAction(d.actionAfterAirtech);
    g_motionIdxRG = GetMotionIndexForAction(d.actionOnRG);
    g_mirrorFwdDashFollowup = forwardDashFollowup.load();
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
int  g_mirrorAirtechMode  = 0;     // 0=disabled/neutral,1=forward,2=back
int  g_mirrorAutoJumpTargetIdx = 2; // 0=P1, 1=P2, 2=Both; runtime stores 1/2/3

void RefreshOpponentMirrors() {
    const auto& d = ImGuiGui::guiState.localData;
    g_mirrorRandomBlock     = RandomBlock::IsEnabled();
    g_mirrorAlwaysRG        = AlwaysRG::IsEnabled();
    g_mirrorRandomRG        = RandomRG::IsEnabled();
    g_mirrorAdaptiveStance  = GetAdaptiveStanceEnabled();
    g_mirrorDummyBlockMode  = GetDummyAutoBlockMode();
    g_mirrorAutoJumpTargetIdx = (d.jumpTarget == 2) ? 1 : (d.jumpTarget == 3 ? 2 : 0);
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

const char* const kStrengthChoices[4] = { "A", "B", "C", "S" };
const char* const kJumpDirChoicesAuto[3] = { "NEUTRAL", "FORWARD", "BACK" };
const char* const kFdFollowupChoices[7] = {
    "NO FOLLOW-UP", "A", "B", "C", "2A", "2B", "2C"
};
const char* const kAkikoSlowChoices[4] = { "INACTIVE", "A", "B", "C" };
const char* const kMaiStatusChoices[5] = { "INACTIVE", "ACTIVE GHOST", "UNSUMMON", "CHARGING", "AWAKENING" };

const char* const kTriggerMotionChoices[] = {
    "5X (STANDING)",
    "2X (CROUCHING)",
    "jX (AIR)",
    "QCF (236)",
    "DP (623)",
    "QCB (214)",
    "421",
    "SUPER1 (41236)",
    "SUPER2 (214236)",
    "236236",
    "214214",
    "641236",
    "463214",
    "412",
    "22",
    "4123641236",
    "6321463214",
    "JUMP",
    "BACKDASH",
    "FORWARD DASH",
    "BLOCK",
    "FINAL MEMORY",
    "6X (FORWARD)",
    "4X (BACK)"
};
constexpr int kTriggerMotionCount = sizeof(kTriggerMotionChoices) / sizeof(kTriggerMotionChoices[0]);

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

bool IsSpecialMoveAction(int action) {
    return ActionUsesButtonStrength(action);
}

int GetPostureIndexForAction(int action) {
    switch (action) {
        case ACTION_5A: case ACTION_5B: case ACTION_5C: case ACTION_5D: return 0;
        case ACTION_2A: case ACTION_2B: case ACTION_2C: case ACTION_2D: return 1;
        case ACTION_JA: case ACTION_JB: case ACTION_JC: case ACTION_JD: return 2;
        default: return -1;
    }
}

int ExtractButtonIndex(int action, int strength) {
    const int posture = GetPostureIndexForAction(action);
    if (posture >= 0) {
        const int base = NormalActionBase(action);
        return ClampIndex(action - base, 4);
    }
    if (action >= ACTION_6A && action <= ACTION_6D) {
        return ClampIndex(action - ACTION_6A, 4);
    }
    if (action >= ACTION_4A && action <= ACTION_4D) {
        return ClampIndex(action - ACTION_4A, 4);
    }
    if (action == ACTION_JUMP) {
        return ClampIndex(strength, 3);
    }
    if (ActionUsesButtonStrength(action)) {
        return ClampIndex(strength, 4);
    }
    return 0;
}

int MapPostureAndButtonToAction(int postureIdx, int buttonIdx) {
    buttonIdx = ClampIndex(buttonIdx, 4);
    switch (postureIdx) {
        case 0: return buttonIdx == 0 ? ACTION_5A : (buttonIdx == 1 ? ACTION_5B : (buttonIdx == 2 ? ACTION_5C : ACTION_5D));
        case 1: return buttonIdx == 0 ? ACTION_2A : (buttonIdx == 1 ? ACTION_2B : (buttonIdx == 2 ? ACTION_2C : ACTION_2D));
        case 2: return buttonIdx == 0 ? ACTION_JA : (buttonIdx == 1 ? ACTION_JB : (buttonIdx == 2 ? ACTION_JC : ACTION_JD));
        default: return ACTION_5A;
    }
}

int MapMotionIndexToAction(int motionIdx, int buttonIdx) {
    motionIdx = ClampIndex(motionIdx, kTriggerMotionCount);
    buttonIdx = ClampIndex(buttonIdx, 4);
    switch (motionIdx) {
        case 17: return ACTION_JUMP;
        case 18: return ACTION_BACKDASH;
        case 19: return ACTION_FORWARD_DASH;
        case 20: return ACTION_BLOCK;
        case 21: return ACTION_FINAL_MEMORY;
        case 3: return ACTION_QCF;
        case 4: return ACTION_DP;
        case 5: return ACTION_QCB;
        case 6: return ACTION_421;
        case 7: return ACTION_SUPER1;
        case 8: return ACTION_SUPER2;
        case 9: return ACTION_236236;
        case 10: return ACTION_214214;
        case 11: return ACTION_641236;
        case 12: return ACTION_463214;
        case 13: return ACTION_412;
        case 14: return ACTION_22;
        case 15: return ACTION_4123641236;
        case 16: return ACTION_6321463214;
        case 22: return buttonIdx == 0 ? ACTION_6A : (buttonIdx == 1 ? ACTION_6B : (buttonIdx == 2 ? ACTION_6C : ACTION_6D));
        case 23: return buttonIdx == 0 ? ACTION_4A : (buttonIdx == 1 ? ACTION_4B : (buttonIdx == 2 ? ACTION_4C : ACTION_4D));
        default: return MapPostureAndButtonToAction(motionIdx, buttonIdx);
    }
}

int GetMotionIndexForAction(int action) {
    switch (action) {
        case ACTION_5A: case ACTION_5B: case ACTION_5C: case ACTION_5D: return 0;
        case ACTION_2A: case ACTION_2B: case ACTION_2C: case ACTION_2D: return 1;
        case ACTION_JA: case ACTION_JB: case ACTION_JC: case ACTION_JD: return 2;
        case ACTION_6A: case ACTION_6B: case ACTION_6C: case ACTION_6D: return 22;
        case ACTION_4A: case ACTION_4B: case ACTION_4C: case ACTION_4D: return 23;
        case ACTION_QCF: return 3;
        case ACTION_DP: return 4;
        case ACTION_QCB: return 5;
        case ACTION_421: return 6;
        case ACTION_SUPER1: return 7;
        case ACTION_SUPER2: return 8;
        case ACTION_236236: return 9;
        case ACTION_214214: return 10;
        case ACTION_641236: return 11;
        case ACTION_463214: return 12;
        case ACTION_412: return 13;
        case ACTION_22: return 14;
        case ACTION_4123641236: return 15;
        case ACTION_6321463214: return 16;
        case ACTION_JUMP: return 17;
        case ACTION_BACKDASH: return 18;
        case ACTION_FORWARD_DASH: return 19;
        case ACTION_BLOCK: return 20;
        case ACTION_FINAL_MEMORY: return 21;
        default: return 0;
    }
}

void ApplyMotionToTrigger(int motionIdx, int& action, int& strength) {
    motionIdx = ClampIndex(motionIdx, kTriggerMotionCount);
    const int buttonIdx = ExtractButtonIndex(action, strength);
    action = MapMotionIndexToAction(motionIdx, buttonIdx);
    if (GetPostureIndexForAction(action) >= 0 || motionIdx == 22 || motionIdx == 23) {
        strength = ExtractButtonIndex(action, strength);
    } else if (action == ACTION_JUMP) {
        strength = ClampIndex(strength, 3);
    } else if (ActionUsesButtonStrength(action)) {
        strength = ClampIndex(strength, 4);
    } else {
        strength = 0;
    }
}

enum class TriggerButtonMode {
    Hidden = 0,
    NoneLabel,
    Abcd,
    JumpDir,
    FdFollowup
};

TriggerButtonMode GetTriggerButtonMode(int action) {
    if (action == ACTION_BLOCK || action == ACTION_BACKDASH || action == ACTION_FINAL_MEMORY) {
        return TriggerButtonMode::NoneLabel;
    }
    if (action == ACTION_JUMP) return TriggerButtonMode::JumpDir;
    if (action == ACTION_FORWARD_DASH) return TriggerButtonMode::FdFollowup;
    if (GetPostureIndexForAction(action) >= 0) return TriggerButtonMode::Abcd;
    if (action >= ACTION_6A && action <= ACTION_4D) return TriggerButtonMode::Abcd;
    if (ActionUsesButtonStrength(action)) return TriggerButtonMode::Abcd;
    return TriggerButtonMode::NoneLabel;
}

void ApplyTriggerButtonIndex(int& action, int& strength, int* dashFollowup, TriggerButtonMode mode, int idx) {
    switch (mode) {
        case TriggerButtonMode::Abcd: {
            idx = ClampIndex(idx, 4);
            const int posture = GetPostureIndexForAction(action);
            if (posture >= 0) {
                action = MapPostureAndButtonToAction(posture, idx);
                strength = idx;
            } else if (action >= ACTION_6A && action <= ACTION_6D) {
                action = idx == 0 ? ACTION_6A : (idx == 1 ? ACTION_6B : (idx == 2 ? ACTION_6C : ACTION_6D));
                strength = idx;
            } else if (action >= ACTION_4A && action <= ACTION_4D) {
                action = idx == 0 ? ACTION_4A : (idx == 1 ? ACTION_4B : (idx == 2 ? ACTION_4C : ACTION_4D));
                strength = idx;
            } else {
                strength = idx;
            }
            break;
        }
        case TriggerButtonMode::JumpDir:
            strength = ClampIndex(idx, 3);
            break;
        case TriggerButtonMode::FdFollowup:
            if (dashFollowup) {
                *dashFollowup = ClampIndex(idx, 7);
                forwardDashFollowup.store(*dashFollowup);
            }
            break;
        default:
            break;
    }
}

int GetTriggerButtonIndex(int action, int strength, int dashFollowup, TriggerButtonMode mode) {
    switch (mode) {
        case TriggerButtonMode::Abcd:
            return ExtractButtonIndex(action, strength);
        case TriggerButtonMode::JumpDir:
            return ClampIndex(strength, 3);
        case TriggerButtonMode::FdFollowup:
            return ClampIndex(dashFollowup, 7);
        default:
            return 0;
    }
}

int TriggerButtonChoiceCount(TriggerButtonMode mode) {
    switch (mode) {
        case TriggerButtonMode::Abcd: return 4;
        case TriggerButtonMode::JumpDir: return 3;
        case TriggerButtonMode::FdFollowup: return 7;
        default: return 0;
    }
}

const char* TriggerButtonChoiceLabel(TriggerButtonMode mode, int idx) {
    switch (mode) {
        case TriggerButtonMode::Abcd:
            return kStrengthChoices[ClampIndex(idx, 4)];
        case TriggerButtonMode::JumpDir:
            return kJumpDirChoicesAuto[ClampIndex(idx, 3)];
        case TriggerButtonMode::FdFollowup:
            return kFdFollowupChoices[ClampIndex(idx, 7)];
        default:
            return "(NONE)";
    }
}

void OnTriggerMotionAfterBlock() {
    auto& d = ImGuiGui::guiState.localData;
    ApplyMotionToTrigger(g_motionIdxAB, d.actionAfterBlock, d.strengthAfterBlock);
    OnAutoApply();
}
void OnTriggerMotionOnWakeup() {
    auto& d = ImGuiGui::guiState.localData;
    ApplyMotionToTrigger(g_motionIdxWU, d.actionOnWakeup, d.strengthOnWakeup);
    OnAutoApply();
}
void OnTriggerMotionAfterHitstun() {
    auto& d = ImGuiGui::guiState.localData;
    ApplyMotionToTrigger(g_motionIdxAH, d.actionAfterHitstun, d.strengthAfterHitstun);
    OnAutoApply();
}
void OnTriggerMotionAfterAirtech() {
    auto& d = ImGuiGui::guiState.localData;
    ApplyMotionToTrigger(g_motionIdxAA, d.actionAfterAirtech, d.strengthAfterAirtech);
    OnAutoApply();
}
void OnTriggerMotionOnRG() {
    auto& d = ImGuiGui::guiState.localData;
    ApplyMotionToTrigger(g_motionIdxRG, d.actionOnRG, d.strengthOnRG);
    OnAutoApply();
}

bool HideABSingleAction() {
    const auto& d = ImGuiGui::guiState.localData;
    return g_useMaskAB || d.macroSlotAfterBlock > 0;
}
bool HideWUSingleAction() {
    const auto& d = ImGuiGui::guiState.localData;
    return g_useMaskWU || d.macroSlotOnWakeup > 0;
}
bool HideAHSingleAction() {
    const auto& d = ImGuiGui::guiState.localData;
    return g_useMaskAH || d.macroSlotAfterHitstun > 0;
}
bool HideAASingleAction() {
    const auto& d = ImGuiGui::guiState.localData;
    return g_useMaskAA || d.macroSlotAfterAirtech > 0;
}
bool HideRGSingleAction() {
    const auto& d = ImGuiGui::guiState.localData;
    return g_useMaskRG || d.macroSlotOnRG > 0;
}

bool HideABButton() { return HideABSingleAction(); }
bool HideWUButton() { return HideWUSingleAction(); }
bool HideAHButton() { return HideAHSingleAction(); }
bool HideAAButton() { return HideAASingleAction(); }
bool HideRGButton() { return HideRGSingleAction(); }

const char* FormatTriggerButtonRowImpl(const Row& row) {
    if (!row.choiceIdxPtr) return "(NONE)";
    const int action = *row.choiceIdxPtr;
    const int strength = row.choice2IdxPtr ? *row.choice2IdxPtr : 0;
    const int dashFollowup = row.intPtr ? *row.intPtr : forwardDashFollowup.load();
    const TriggerButtonMode mode = GetTriggerButtonMode(action);
    if (mode == TriggerButtonMode::NoneLabel) return "(NONE)";
    const int idx = GetTriggerButtonIndex(action, strength, dashFollowup, mode);
    return TriggerButtonChoiceLabel(mode, idx);
}

bool AdjustTriggerButtonRowImpl(const Row& row, int direction) {
    if (!row.choiceIdxPtr || direction == 0) return false;
    const TriggerButtonMode mode = GetTriggerButtonMode(*row.choiceIdxPtr);
    const int count = TriggerButtonChoiceCount(mode);
    if (count <= 0) return false;

    int strength = row.choice2IdxPtr ? *row.choice2IdxPtr : 0;
    int dashFollowup = row.intPtr ? *row.intPtr : forwardDashFollowup.load();
    int idx = GetTriggerButtonIndex(*row.choiceIdxPtr, strength, dashFollowup, mode);
    idx = (idx + direction + count) % count;

    int action = *row.choiceIdxPtr;
    ApplyTriggerButtonIndex(action, strength, row.intPtr, mode, idx);
    *row.choiceIdxPtr = action;
    if (row.choice2IdxPtr) *row.choice2IdxPtr = strength;
    if (row.intPtr && mode == TriggerButtonMode::FdFollowup) {
        forwardDashFollowup.store(*row.intPtr);
    }
    if (row.onChange) row.onChange();
    return true;
}

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
void OnSavestateBackendMode() {
    PersistInt("General", "savestateBackendMode", MutableSettings().savestateBackendMode);
}
void OnSavestateLoadCustomPalettes() {
    PersistBool("General", "savestateLoadCustomPalettes", MutableSettings().savestateLoadCustomPalettes);
}
void OnRestrictPractice() {
    PersistBool("General", "restrictToPracticeMode", MutableSettings().restrictToPracticeMode);
}
void OnBgmVolume() {
    PersistInt("General", "bgmVolumePercent", MutableSettings().bgmVolumePercent);
    ExtendedConfigBridge::PublishAudioSettings(MutableSettings().bgmVolumePercent,
                                               MutableSettings().seVolumePercent);
    AudioControl::ApplyConfiguredVolumesNow();
}
void OnSeVolume() {
    PersistInt("General", "seVolumePercent", MutableSettings().seVolumePercent);
    ExtendedConfigBridge::PublishAudioSettings(MutableSettings().bgmVolumePercent,
                                               MutableSettings().seVolumePercent);
    AudioControl::ApplyConfiguredVolumesNow();
}

const char* FormatPercentRowValue(const Row& row) {
    static char buf[32];
    const int value = row.intPtr ? *row.intPtr : 0;
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%d%%", value);
    return buf;
}

const char* ValInterfaceSettings() { return MutableSettings().useCustomMenu ? "CUSTOM" : "IMGUI"; }
const char* ValAudioSettings() {
    static char buf[32];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "BGM %d / SE %d",
                MutableSettings().bgmVolumePercent,
                MutableSettings().seVolumePercent);
    return buf;
}
const char* ValRecoverySettings()  { return MutableSettings().crRequireBothNeutral ? "NEUTRAL" : "ANY"; }
const char* ValPracticeSettings()  { return MutableSettings().restrictToPracticeMode ? "PRACTICE" : "ANY MODE"; }

bool CollisionProjectileOptionsHidden() {
    return !MutableSettings().collisionDisplayProjectileInteractions;
}

const char* ValDisplaySettings() {
    static char buf[48];
    const auto& s = MutableSettings();
    const int enabled =
        (s.collisionDisplayHitboxes ? 1 : 0)
        + (s.collisionDisplayHurtboxes ? 1 : 0)
        + (s.collisionDisplayCollisionBoxes ? 1 : 0)
        + (s.collisionDisplayProjectileInteractions ? 1 : 0);
    if (enabled == 0) {
        return "OFF";
    }
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%d ON / %d%%", enabled, s.collisionDisplayFillAlphaPercent);
    return buf;
}

Row* BuildDisplayOverlayRows(int& count) {
    static Row s_rows[24];
    int n = 0;
    auto& s = MutableSettings();

    s_rows[n++] = Header("HITBOX / COLLISION DISPLAY");
    s_rows[n++] = Toggle("HITBOXES", &s.collisionDisplayHitboxes, OnCollisionDisplayHitboxes);
    s_rows[n++] = Toggle("HURTBOXES", &s.collisionDisplayHurtboxes, OnCollisionDisplayHurtboxes);
    s_rows[n++] = Toggle("COLLISION BOXES", &s.collisionDisplayCollisionBoxes, OnCollisionDisplayPushboxes);
    s_rows[n++] = Toggle("PROJECTILE INTERACTIONS", &s.collisionDisplayProjectileInteractions, OnCollisionDisplayProjectiles);
    Row alpha = IntSlider("BOX FILL ALPHA", &s.collisionDisplayFillAlphaPercent, 0, 100, 1, 10, OnCollisionDisplayAlpha);
    alpha.valueFormatter = FormatPercentRowValue;
    s_rows[n++] = alpha;

    s_rows[n++] = Spacer();
    s_rows[n++] = Header("PROJECTILE SUB-LAYERS");
    s_rows[n++] = Toggle("  PROJECTILE BOXES", &s.collisionDisplayProjectileBoxes,
                         OnCollisionProjectileBoxes, nullptr, CollisionProjectileOptionsHidden);
    s_rows[n++] = Toggle("  ORIGIN / RANGE DOTS", &s.collisionDisplayProjectileOrigins,
                         OnCollisionProjectileOrigins, nullptr, CollisionProjectileOptionsHidden);
    s_rows[n++] = Toggle("  INTERSECTION BOXES", &s.collisionDisplayProjectileIntersections,
                         OnCollisionProjectileIntersections, nullptr, CollisionProjectileOptionsHidden);
    s_rows[n++] = Info("Mizuka note display settings are under Character Settings when Mizuka is in the match.");
    s_rows[n++] = Info("Origin dots are EFZ projectile anchors / activation points, not collision centers.");

    count = n;
    return s_rows;
}

Row* BuildSettingsInterfaceRows(int& count) {
    static Row s_rows[16];
    int n = 0;
    auto& s = MutableSettings();

    s_rows[n++] = Header("INTERFACE");
    s_rows[n++] = Toggle    ("USE CUSTOM MENU",        &s.useCustomMenu,       OnUseCustomMenu);
    s_rows[n++] = FloatNum  ("UI SCALE",               &s.uiScale,      0.70f, 1.50f, 0.05f, 0.10f, "%.2f", OnUiScale);
    s_rows[n++] = ChoicesRow("UI FONT (ADVANCED MENU)", &s.uiFontMode,   kUiFontChoices, 2, OnUiFont);
    s_rows[n++] = Toggle    ("PRACTICE OVERLAY HINT",  &s.showPracticeEntryHint, OnPracticeHint);
    count = n;
    return s_rows;
}

Row* BuildSettingsRecoveryRows(int& count) {
    static Row s_rows[16];
    int n = 0;
    auto& s = MutableSettings();

    s_rows[n++] = Header("RECOVERY");
    s_rows[n++] = Toggle ("CR: BOTH NEUTRAL REQD",     &s.crRequireBothNeutral, OnCrRequire);
    s_rows[n++] = IntNum ("CR NEUTRAL DELAY (MS)",     &s.crBothNeutralDelayMs, 0, 5000, 50, 500, OnCrDelay);
    s_rows[n++] = Toggle ("AUTO-FIX HP<=0",            &s.autoFixHPOnNeutral,   OnAutoFixHp);
    s_rows[n++] = Toggle ("FREEZE RF AFTER CR",        &s.freezeRFAfterContRec, OnFreezeRfAfterCr);
    s_rows[n++] = Toggle ("FREEZE RF ONLY NEUTRAL",    &s.freezeRFOnlyWhenNeutral, OnFreezeRfNeutral);
    count = n;
    return s_rows;
}

Row* BuildSettingsAudioRows(int& count) {
    static Row s_rows[8];
    int n = 0;
    auto& s = MutableSettings();

    s_rows[n++] = Header("AUDIO");
    Row bgmVolume = IntSlider("BGM VOLUME", &s.bgmVolumePercent, 0, 100, 1, 10, OnBgmVolume);
    bgmVolume.valueFormatter = FormatPercentRowValue;
    s_rows[n++] = bgmVolume;
    Row seVolume = IntSlider("SE VOLUME", &s.seVolumePercent, 0, 100, 1, 10, OnSeVolume);
    seVolume.valueFormatter = FormatPercentRowValue;
    s_rows[n++] = seVolume;
    s_rows[n++] = Info("100% preserves the current default mix.");
    count = n;
    return s_rows;
}

Row* BuildSettingsPracticeRows(int& count) {
    static Row s_rows[8];
    int n = 0;
    auto& s = MutableSettings();

    s_rows[n++] = Header("PRACTICE");
    s_rows[n++] = IntNum ("AUTO-BLOCK TIMEOUT (MS)",   &s.autoBlockNeutralTimeoutMs, 0, 60000, 500, 5000, OnAutoBlockTimeout);
    s_rows[n++] = Toggle ("RESTRICT TO PRACTICE",      &s.restrictToPracticeMode, OnRestrictPractice);
    count = n;
    return s_rows;
}

Row* BuildSettingsGeneralRows(int& count) {
    static Row s_rows[20];
    int n = 0;

    s_rows[n++] = Header("GENERAL MENUS");
    s_rows[n++] = Submenu("INTERFACE", "INTERFACE", BuildSettingsInterfaceRows, ValInterfaceSettings);
    s_rows[n++] = Submenu("DISPLAY",   "DISPLAY OVERLAYS", BuildDisplayOverlayRows, ValDisplaySettings);
    s_rows[n++] = Submenu("AUDIO",     "AUDIO",     BuildSettingsAudioRows,     ValAudioSettings);
    s_rows[n++] = Submenu("RECOVERY",  "RECOVERY",  BuildSettingsRecoveryRows,  ValRecoverySettings);
    s_rows[n++] = Submenu("PRACTICE",  "PRACTICE",  BuildSettingsPracticeRows,  ValPracticeSettings);
    s_rows[n++] = Spacer();
    s_rows[n++] = Info(CurrentConfigPathInfo());
    s_rows[n++] = Action ("SAVE ALL TO DISK",          SaveSettingsToDisk);
    s_rows[n++] = Action ("RELOAD FROM DISK",          ReloadSettingsFromDisk);

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

const char* HotkeyCodeValue(int vk) {
    static char buffers[8][96];
    static int next = 0;
    char* buf = buffers[next++ & 7];
    if (vk < 0) {
        _snprintf_s(buf, sizeof(buffers[0]), _TRUNCATE, "DISABLED");
    } else {
        _snprintf_s(buf, sizeof(buffers[0]), _TRUNCATE, "0x%X  %s", vk, Config::GetKeyName(vk).c_str());
    }
    return buf;
}

void BindHotkey(const char* title, int* field, const char* key) {
    OpenKeybind(title, field, "Hotkeys", key);
}

void BindFooterHotkey(const char* title, int* field, const char* key) {
    OpenKeybind(title, field, "Hotkeys", key, true);
}

void BindOpenMenu()       { auto& s = MutableSettings(); BindHotkey("OPEN MENU",       &s.configMenuKey,          "ConfigMenuKey"); }
void BindTeleport()       { auto& s = MutableSettings(); BindHotkey("TELEPORT",        &s.teleportKey,           "TeleportKey"); }
void BindSavePosition()   { auto& s = MutableSettings(); BindHotkey("SAVE POSITION",   &s.recordKey,             "RecordKey"); }
void BindToggleStats()    { auto& s = MutableSettings(); BindHotkey("TOGGLE STATS",    &s.toggleTitleKey,        "ToggleTitleKey"); }
void BindResetCounter()   { auto& s = MutableSettings(); BindHotkey("RESET COUNTER",   &s.resetFrameCounterKey,  "ResetFrameCounterKey"); }
void BindHelp()           { auto& s = MutableSettings(); BindHotkey("HELP",            &s.helpKey,               "HelpKey"); }
void BindToggleImGui()    { auto& s = MutableSettings(); BindHotkey("TOGGLE OVERLAY",  &s.toggleImGuiKey,        "ToggleImGuiKey"); }
void BindSavestateSave()  { auto& s = MutableSettings(); BindHotkey("SAVESTATE SAVE",  &s.savestateSaveKey,      "SavestateSaveKey"); }
void BindSavestateLoad()  { auto& s = MutableSettings(); BindHotkey("SAVESTATE LOAD",  &s.savestateLoadKey,      "SavestateLoadKey"); }
void BindSavestatePrev()  { auto& s = MutableSettings(); BindHotkey("SLOT PREVIOUS",   &s.savestatePrevSlotKey,  "SavestatePrevSlotKey"); }
void BindSavestateNext()  { auto& s = MutableSettings(); BindHotkey("SLOT NEXT",       &s.savestateNextSlotKey,  "SavestateNextSlotKey"); }
void BindSwitchPlayers()  { auto& s = MutableSettings(); BindHotkey("SWITCH PLAYERS",  &s.switchPlayersKey,      "SwitchPlayersKey"); }
void BindMacroRecord()    { auto& s = MutableSettings(); BindHotkey("MACRO RECORD",    &s.macroRecordKey,        "MacroRecordKey"); }
void BindMacroPlay()      { auto& s = MutableSettings(); BindHotkey("MACRO PLAY",      &s.macroPlayKey,          "MacroPlayKey"); }
void BindMacroSlot()      { auto& s = MutableSettings(); BindHotkey("MACRO NEXT SLOT", &s.macroSlotKey,          "MacroSlotKey"); }
void BindUiAccept()       { auto& s = MutableSettings(); BindFooterHotkey("UI ACCEPT",  &s.uiAcceptKey,          "UIAcceptKey"); }
void BindUiRefresh()      { auto& s = MutableSettings(); BindFooterHotkey("UI REFRESH", &s.uiRefreshKey,         "UIRefreshKey"); }
void BindUiExit()         { auto& s = MutableSettings(); BindFooterHotkey("UI EXIT",    &s.uiExitKey,            "UIExitKey"); }
void BindFramestepPause() { auto& s = MutableSettings(); BindHotkey("FRAMESTEP PAUSE", &s.framestepPauseKey,     "FramestepPauseKey"); }
void BindFramestepStep()  { auto& s = MutableSettings(); BindHotkey("FRAMESTEP STEP",  &s.framestepStepKey,      "FramestepStepKey"); }
void BindSwapCustom()     { auto& s = MutableSettings(); BindHotkey("SWAP CUSTOM KEY", &s.swapCustomKey,         "SwapCustomKey"); }

struct ManualKeybindEditorState {
    bool active = false;
    bool disallowMenuReserved = false;
    int* settingsField = nullptr;
    char title[48] = "";
    char iniSection[16] = "";
    char iniKey[32] = "";
    char valueBuf[32] = "";
    char errorBuf[128] = "";
};
ManualKeybindEditorState g_manualKeybindEditor;

bool IsManualKeybindEditorActive() {
    return g_manualKeybindEditor.active;
}

bool ManualKeybindAllowed(int vk, bool disallowMenuReserved) {
    if (vk < 0) return true;
    if (vk == 0) return false;
    if (vk >= 0x01 && vk <= 0x06) return false;
    if (vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT) return false;
    if (vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL) return false;
    if (vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU) return false;
    if (vk == VK_LWIN || vk == VK_RWIN) return false;
    if (vk == VK_CLEAR) return false;
    if (vk == VK_ESCAPE) return false;
    if (disallowMenuReserved && (vk == VK_RETURN || vk == VK_SPACE)) return false;
    return true;
}

std::string TrimAscii(const char* text) {
    if (!text) return std::string();
    std::string value(text);
    const auto isSpace = [](char c) {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r';
    };
    size_t start = 0;
    while (start < value.size() && isSpace(value[start])) ++start;
    size_t end = value.size();
    while (end > start && isSpace(value[end - 1])) --end;
    return value.substr(start, end - start);
}

void CloseManualKeybindEditor() {
    g_manualKeybindEditor.active = false;
    g_manualKeybindEditor.settingsField = nullptr;
    g_manualKeybindEditor.title[0] = '\0';
    g_manualKeybindEditor.iniSection[0] = '\0';
    g_manualKeybindEditor.iniKey[0] = '\0';
    g_manualKeybindEditor.valueBuf[0] = '\0';
    g_manualKeybindEditor.errorBuf[0] = '\0';
    g_manualKeybindEditor.disallowMenuReserved = false;
}

void OpenManualKeybindEditor(const char* title, int* field,
                             const char* section, const char* key,
                             bool disallowMenuReserved = false) {
    if (!title || !field || !section || !key) return;
    g_manualKeybindEditor.active = true;
    g_manualKeybindEditor.settingsField = field;
    g_manualKeybindEditor.disallowMenuReserved = disallowMenuReserved;
    strncpy_s(g_manualKeybindEditor.title, sizeof(g_manualKeybindEditor.title), title, _TRUNCATE);
    strncpy_s(g_manualKeybindEditor.iniSection, sizeof(g_manualKeybindEditor.iniSection), section, _TRUNCATE);
    strncpy_s(g_manualKeybindEditor.iniKey, sizeof(g_manualKeybindEditor.iniKey), key, _TRUNCATE);
    if (*field < 0) {
        strncpy_s(g_manualKeybindEditor.valueBuf, sizeof(g_manualKeybindEditor.valueBuf), "-1", _TRUNCATE);
    } else {
        _snprintf_s(g_manualKeybindEditor.valueBuf, sizeof(g_manualKeybindEditor.valueBuf), _TRUNCATE, "0x%X", *field);
    }
    g_manualKeybindEditor.errorBuf[0] = '\0';
    Input::ResetEdges();
}

bool ApplyManualKeybindEditor() {
    if (!g_manualKeybindEditor.active || !g_manualKeybindEditor.settingsField) {
        return false;
    }

    const std::string raw = TrimAscii(g_manualKeybindEditor.valueBuf);
    int parsed = 0;
    bool disable = false;
    if (raw.empty()) {
        strncpy_s(g_manualKeybindEditor.errorBuf, sizeof(g_manualKeybindEditor.errorBuf),
                  "Enter a virtual-key code in hex (0x48) or decimal (72).", _TRUNCATE);
        return false;
    }
    if (raw == "-1" || raw == "off" || raw == "OFF" || raw == "disabled" || raw == "DISABLED") {
        disable = true;
        parsed = -1;
    } else {
        parsed = Config::ParseKeyValue(raw);
    }

    if (!disable && !ManualKeybindAllowed(parsed, g_manualKeybindEditor.disallowMenuReserved)) {
        strncpy_s(g_manualKeybindEditor.errorBuf, sizeof(g_manualKeybindEditor.errorBuf),
                  g_manualKeybindEditor.disallowMenuReserved
                      ? "That key is reserved here. Footer bindings disallow Enter, Escape, and Space."
                      : "That key is not allowed for menu binding.",
                  _TRUNCATE);
        return false;
    }

    *g_manualKeybindEditor.settingsField = parsed;
    if (disable) {
        Config::SetSetting(g_manualKeybindEditor.iniSection, g_manualKeybindEditor.iniKey, "-1");
    } else {
        char buf[16];
        _snprintf_s(buf, sizeof(buf), _TRUNCATE, "0x%X", parsed);
        Config::SetSetting(g_manualKeybindEditor.iniSection, g_manualKeybindEditor.iniKey, buf);
    }

    DirectDrawHook::AddMessage(disable ? "Keybind disabled" : "Keybind updated", "HOTKEY", RGB(180, 255, 220), 900, 0, 120);
    CloseManualKeybindEditor();
    Input::ResetEdges();
    return true;
}

std::string ManualKeybindPreviewText() {
    const std::string raw = TrimAscii(g_manualKeybindEditor.valueBuf);
    if (raw.empty()) return "Preview: enter a value";
    if (raw == "-1" || raw == "off" || raw == "OFF" || raw == "disabled" || raw == "DISABLED") {
        return "Preview: DISABLED";
    }
    const int parsed = Config::ParseKeyValue(raw);
    if (!ManualKeybindAllowed(parsed, g_manualKeybindEditor.disallowMenuReserved)) {
        return "Preview: blocked by key restrictions";
    }
    char buf[96];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "Preview: 0x%X  %s", parsed, Config::GetKeyName(parsed).c_str());
    return std::string(buf);
}

bool TickManualKeybindEditorIfActive(ImDrawList*, const ScreenLayout& layout) {
    if (!g_manualKeybindEditor.active) return false;

    const float x = layout.panelX + 52.0f;
    const float y = layout.contentTopY + 26.0f;
    const float w = Theme::kPanelW - 104.0f;
    const float h = 184.0f;

    ImGui::SetNextWindowPos(ImVec2(x, y), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 0.94f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1.0f, 1.0f, 1.0f, 0.86f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.04f, 0.04f, 0.04f, 0.96f));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.12f, 0.12f, 0.12f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.22f, 0.22f, 0.22f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.35f, 0.35f, 0.35f, 1.0f));

    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings;
    if (ImGui::Begin("MANUAL KEYBIND EDITOR", nullptr, flags)) {
        ImGui::TextUnformatted(g_manualKeybindEditor.title);
        ImGui::TextDisabled("Enter a raw virtual-key code as hex or decimal. Example: 0x48 or 72.");
        if (g_manualKeybindEditor.disallowMenuReserved) {
            ImGui::TextDisabled("Footer bindings reserve Enter, Escape, and Space.");
        }

        if (g_manualKeybindEditor.errorBuf[0]) {
            ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "%s", g_manualKeybindEditor.errorBuf);
        } else {
            const std::string preview = ManualKeybindPreviewText();
            ImGui::TextDisabled("%s", preview.c_str());
        }

        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputText("##manual_vk_value", g_manualKeybindEditor.valueBuf, sizeof(g_manualKeybindEditor.valueBuf));

        if (ImGui::Button("Apply")) {
            ApplyManualKeybindEditor();
        }
        ImGui::SameLine();
        if (ImGui::Button("Disable")) {
            strncpy_s(g_manualKeybindEditor.valueBuf, sizeof(g_manualKeybindEditor.valueBuf), "-1", _TRUNCATE);
            ApplyManualKeybindEditor();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            CloseManualKeybindEditor();
            Input::ResetEdges();
        }

        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            CloseManualKeybindEditor();
            Input::ResetEdges();
        }
    }
    ImGui::End();

    ImGui::PopStyleColor(7);
    ImGui::PopStyleVar(2);
    return true;
}

void EditKeyboardHotkey(const char* title, int* field, const char* key, bool disallowMenuReserved = false) {
    OpenManualKeybindEditor(title, field, "Hotkeys", key, disallowMenuReserved);
}

void EditOpenMenuVk()       { auto& s = MutableSettings(); EditKeyboardHotkey("OPEN MENU",       &s.configMenuKey,         "ConfigMenuKey"); }
void EditTeleportVk()       { auto& s = MutableSettings(); EditKeyboardHotkey("TELEPORT",        &s.teleportKey,          "TeleportKey"); }
void EditSavePositionVk()   { auto& s = MutableSettings(); EditKeyboardHotkey("SAVE POSITION",   &s.recordKey,            "RecordKey"); }
void EditToggleStatsVk()    { auto& s = MutableSettings(); EditKeyboardHotkey("TOGGLE STATS",    &s.toggleTitleKey,       "ToggleTitleKey"); }
void EditResetCounterVk()   { auto& s = MutableSettings(); EditKeyboardHotkey("RESET COUNTER",   &s.resetFrameCounterKey, "ResetFrameCounterKey"); }
void EditHelpVk()           { auto& s = MutableSettings(); EditKeyboardHotkey("HELP",            &s.helpKey,              "HelpKey"); }
void EditToggleImGuiVk()    { auto& s = MutableSettings(); EditKeyboardHotkey("TOGGLE OVERLAY",  &s.toggleImGuiKey,       "ToggleImGuiKey"); }
void EditSavestateSaveVk()  { auto& s = MutableSettings(); EditKeyboardHotkey("SAVESTATE SAVE",  &s.savestateSaveKey,     "SavestateSaveKey"); }
void EditSavestateLoadVk()  { auto& s = MutableSettings(); EditKeyboardHotkey("SAVESTATE LOAD",  &s.savestateLoadKey,     "SavestateLoadKey"); }
void EditSavestatePrevVk()  { auto& s = MutableSettings(); EditKeyboardHotkey("SLOT PREVIOUS",   &s.savestatePrevSlotKey, "SavestatePrevSlotKey"); }
void EditSavestateNextVk()  { auto& s = MutableSettings(); EditKeyboardHotkey("SLOT NEXT",       &s.savestateNextSlotKey, "SavestateNextSlotKey"); }
void EditSwitchPlayersVk()  { auto& s = MutableSettings(); EditKeyboardHotkey("SWITCH PLAYERS",  &s.switchPlayersKey,     "SwitchPlayersKey"); }
void EditMacroRecordVk()    { auto& s = MutableSettings(); EditKeyboardHotkey("MACRO RECORD",    &s.macroRecordKey,       "MacroRecordKey"); }
void EditMacroPlayVk()      { auto& s = MutableSettings(); EditKeyboardHotkey("MACRO PLAY",      &s.macroPlayKey,         "MacroPlayKey"); }
void EditMacroSlotVk()      { auto& s = MutableSettings(); EditKeyboardHotkey("MACRO NEXT SLOT", &s.macroSlotKey,         "MacroSlotKey"); }
void EditUiAcceptVk()       { auto& s = MutableSettings(); EditKeyboardHotkey("UI ACCEPT",       &s.uiAcceptKey,          "UIAcceptKey", true); }
void EditUiRefreshVk()      { auto& s = MutableSettings(); EditKeyboardHotkey("UI REFRESH",      &s.uiRefreshKey,         "UIRefreshKey", true); }
void EditUiExitVk()         { auto& s = MutableSettings(); EditKeyboardHotkey("UI EXIT",         &s.uiExitKey,            "UIExitKey", true); }
void EditFramestepPauseVk() { auto& s = MutableSettings(); EditKeyboardHotkey("FRAMESTEP PAUSE", &s.framestepPauseKey,    "FramestepPauseKey"); }
void EditFramestepStepVk()  { auto& s = MutableSettings(); EditKeyboardHotkey("FRAMESTEP STEP",  &s.framestepStepKey,     "FramestepStepKey"); }
void EditSwapCustomVk()     { auto& s = MutableSettings(); EditKeyboardHotkey("SWAP CUSTOM KEY", &s.swapCustomKey,        "SwapCustomKey"); }

const char* ValOpenMenu()       { return HotkeyNameValue(Config::GetSettings().configMenuKey); }
const char* ValTeleport()       { return HotkeyNameValue(Config::GetSettings().teleportKey); }
const char* ValSavePosition()   { return HotkeyNameValue(Config::GetSettings().recordKey); }
const char* ValToggleStats()    { return HotkeyNameValue(Config::GetSettings().toggleTitleKey); }
const char* ValResetCounter()   { return HotkeyNameValue(Config::GetSettings().resetFrameCounterKey); }
const char* ValHelp()           { return HotkeyNameValue(Config::GetSettings().helpKey); }
const char* ValToggleImGui()    { return HotkeyNameValue(Config::GetSettings().toggleImGuiKey); }
const char* ValSavestateSave()  { return HotkeyNameValue(Config::GetSettings().savestateSaveKey); }
const char* ValSavestateLoad()  { return HotkeyNameValue(Config::GetSettings().savestateLoadKey); }
const char* ValSavestatePrev()  { return HotkeyNameValue(Config::GetSettings().savestatePrevSlotKey); }
const char* ValSavestateNext()  { return HotkeyNameValue(Config::GetSettings().savestateNextSlotKey); }
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
const char* ValOpenMenuCode()       { return HotkeyCodeValue(Config::GetSettings().configMenuKey); }
const char* ValTeleportCode()       { return HotkeyCodeValue(Config::GetSettings().teleportKey); }
const char* ValSavePositionCode()   { return HotkeyCodeValue(Config::GetSettings().recordKey); }
const char* ValToggleStatsCode()    { return HotkeyCodeValue(Config::GetSettings().toggleTitleKey); }
const char* ValResetCounterCode()   { return HotkeyCodeValue(Config::GetSettings().resetFrameCounterKey); }
const char* ValHelpCode()           { return HotkeyCodeValue(Config::GetSettings().helpKey); }
const char* ValToggleImGuiCode()    { return HotkeyCodeValue(Config::GetSettings().toggleImGuiKey); }
const char* ValSavestateSaveCode()  { return HotkeyCodeValue(Config::GetSettings().savestateSaveKey); }
const char* ValSavestateLoadCode()  { return HotkeyCodeValue(Config::GetSettings().savestateLoadKey); }
const char* ValSavestatePrevCode()  { return HotkeyCodeValue(Config::GetSettings().savestatePrevSlotKey); }
const char* ValSavestateNextCode()  { return HotkeyCodeValue(Config::GetSettings().savestateNextSlotKey); }
const char* ValSwitchPlayersCode()  { return HotkeyCodeValue(Config::GetSettings().switchPlayersKey); }
const char* ValMacroRecordCode()    { return HotkeyCodeValue(Config::GetSettings().macroRecordKey); }
const char* ValMacroPlayCode()      { return HotkeyCodeValue(Config::GetSettings().macroPlayKey); }
const char* ValMacroSlotCode()      { return HotkeyCodeValue(Config::GetSettings().macroSlotKey); }
const char* ValUiAcceptCode()       { return HotkeyCodeValue(Config::GetSettings().uiAcceptKey); }
const char* ValUiRefreshCode()      { return HotkeyCodeValue(Config::GetSettings().uiRefreshKey); }
const char* ValUiExitCode()         { return HotkeyCodeValue(Config::GetSettings().uiExitKey); }
const char* ValFramestepPauseCode() { return HotkeyCodeValue(Config::GetSettings().framestepPauseKey); }
const char* ValFramestepStepCode()  { return HotkeyCodeValue(Config::GetSettings().framestepStepKey); }
const char* ValSwapCustomCode()     { return HotkeyCodeValue(Config::GetSettings().swapCustomKey); }
const char* ValSwapEnabled()    { return Config::GetSettings().swapCustomEnabled ? "ON" : "OFF"; }
constexpr int kControllerChoiceCount = 5;

char g_controllerChoiceLabels[kControllerChoiceCount][96] = {};
const char* const g_controllerChoicePtrs[kControllerChoiceCount] = {
    g_controllerChoiceLabels[0],
    g_controllerChoiceLabels[1],
    g_controllerChoiceLabels[2],
    g_controllerChoiceLabels[3],
    g_controllerChoiceLabels[4],
};

int g_controllerChoiceIndex = 0;
int g_gpTeleportChoice = 0;

void OnControllerIndexChoice() {
    const int newValue = (g_controllerChoiceIndex <= 0) ? -1 : (g_controllerChoiceIndex - 1);
    PersistInt("General", "controllerIndex", newValue);
}

void BindGamepadHotkey(const char* title, int* field, const char* key) {
    OpenGamepadKeybind(title, field, "Hotkeys", key);
}

void BindGpTeleport()      { auto& s = MutableSettings(); BindGamepadHotkey("LOAD / TELEPORT",  &s.gpTeleportButton,      "gpTeleportButton"); }
void BindGpSavePosition()  { auto& s = MutableSettings(); BindGamepadHotkey("SAVE POSITION",    &s.gpSavePositionButton,  "gpSavePositionButton"); }
void BindGpSwitchPlayers() { auto& s = MutableSettings(); BindGamepadHotkey("SWITCH PLAYERS",   &s.gpSwitchPlayersButton, "gpSwitchPlayersButton"); }
void BindGpSwapPositions() { auto& s = MutableSettings(); BindGamepadHotkey("SWAP POSITIONS",   &s.gpSwapPositionsButton, "gpSwapPositionsButton"); }
void BindGpMacroRecord()   { auto& s = MutableSettings(); BindGamepadHotkey("MACRO RECORD",     &s.gpMacroRecordButton,   "gpMacroRecordButton"); }
void BindGpMacroPlay()     { auto& s = MutableSettings(); BindGamepadHotkey("MACRO PLAY",       &s.gpMacroPlayButton,     "gpMacroPlayButton"); }
void BindGpMacroSlot()     { auto& s = MutableSettings(); BindGamepadHotkey("MACRO NEXT SLOT",  &s.gpMacroSlotButton,     "gpMacroSlotButton"); }
void BindGpToggleMenu()    { auto& s = MutableSettings(); BindGamepadHotkey("TOGGLE MENU",      &s.gpToggleMenuButton,    "gpToggleMenuButton"); }
void BindGpToggleOverlay() { auto& s = MutableSettings(); BindGamepadHotkey("TOGGLE OVERLAY",   &s.gpToggleImGuiButton,   "gpToggleImGuiButton"); }
void BindGpUiTopTabPrev()  { auto& s = MutableSettings(); BindGamepadHotkey("TOP TAB PREVIOUS", &s.gpUiTopTabPrev,        "gpUiTopTabPrev"); }
void BindGpUiTopTabNext()  { auto& s = MutableSettings(); BindGamepadHotkey("TOP TAB NEXT",     &s.gpUiTopTabNext,        "gpUiTopTabNext"); }
void BindGpUiSubTabPrev()  { auto& s = MutableSettings(); BindGamepadHotkey("SUBTAB PREVIOUS",  &s.gpUiSubTabPrev,        "gpUiSubTabPrev"); }
void BindGpUiSubTabNext()  { auto& s = MutableSettings(); BindGamepadHotkey("SUBTAB NEXT",      &s.gpUiSubTabNext,        "gpUiSubTabNext"); }

const char* GamepadBindNameValue(int mask) {
    static char buffers[8][64];
    static int next = 0;
    char* buf = buffers[next++ & 7];
    _snprintf_s(buf, sizeof(buffers[0]), _TRUNCATE, "%s",
                Config::GetGamepadButtonName(mask).c_str());
    return buf;
}

const char* ValGpTeleport()      { return GamepadBindNameValue(Config::GetSettings().gpTeleportButton); }
const char* ValGpSavePosition()  { return GamepadBindNameValue(Config::GetSettings().gpSavePositionButton); }
const char* ValGpSwitchPlayers() { return GamepadBindNameValue(Config::GetSettings().gpSwitchPlayersButton); }
const char* ValGpSwapPositions() { return GamepadBindNameValue(Config::GetSettings().gpSwapPositionsButton); }
const char* ValGpMacroRecord()   { return GamepadBindNameValue(Config::GetSettings().gpMacroRecordButton); }
const char* ValGpMacroPlay()     { return GamepadBindNameValue(Config::GetSettings().gpMacroPlayButton); }
const char* ValGpMacroSlot()     { return GamepadBindNameValue(Config::GetSettings().gpMacroSlotButton); }
const char* ValGpToggleMenu()    { return GamepadBindNameValue(Config::GetSettings().gpToggleMenuButton); }
const char* ValGpToggleOverlay() { return GamepadBindNameValue(Config::GetSettings().gpToggleImGuiButton); }
const char* ValGpUiTopTabPrev()  { return GamepadBindNameValue(Config::GetSettings().gpUiTopTabPrev); }
const char* ValGpUiTopTabNext()  { return GamepadBindNameValue(Config::GetSettings().gpUiTopTabNext); }
const char* ValGpUiSubTabPrev()  { return GamepadBindNameValue(Config::GetSettings().gpUiSubTabPrev); }
const char* ValGpUiSubTabNext()  { return GamepadBindNameValue(Config::GetSettings().gpUiSubTabNext); }

void OnSwapCustomEnabled() {
    PersistBool("Hotkeys", "SwapCustomEnabled", MutableSettings().swapCustomEnabled);
}

bool SwapCustomKeyDisabled() {
    return !Config::GetSettings().swapCustomEnabled;
}

void RefreshHotkeyStrings() {
    const auto& s = Config::GetSettings();

    static DWORD s_lastControllerLabelRefresh = 0;
    static unsigned s_lastControllerMask = 0xFFFFFFFFu;
    static unsigned s_lastNativeMask = 0xFFFFFFFFu;
    static unsigned s_lastGenericMask = 0xFFFFFFFFu;

    const DWORD now = GetTickCount();
    const unsigned controllerMask = XInputShim::GetConnectedMaskCached();
    const unsigned nativeMask = XInputShim::GetNativeConnectedMaskCached();
    const unsigned genericMask = XInputShim::GetGenericConnectedMaskCached();
    // Bumped from 2s to 10s: building these labels can hit the XInput shim, which is
    // cheap when slots are connected but historically expensive when slots are empty.
    // Mask-change still forces an immediate rebuild, so hot-plug is unaffected.
    const bool labelsDue = s_lastControllerLabelRefresh == 0
        || (now - s_lastControllerLabelRefresh) >= 10000
        || controllerMask != s_lastControllerMask
        || nativeMask != s_lastNativeMask
        || genericMask != s_lastGenericMask;

    if (labelsDue) {
        _snprintf_s(g_controllerChoiceLabels[0], sizeof(g_controllerChoiceLabels[0]), _TRUNCATE, "All (Any)");
        for (int i = 0; i < 4; ++i) {
            // Names are pre-computed by the background controller watcher thread; the
            // read here is a brief try_lock copy and never invokes Windows enumeration
            // APIs from the render thread.
            (void)XInputShim::GetPublishedControllerName(
                i, g_controllerChoiceLabels[i + 1], sizeof(g_controllerChoiceLabels[i + 1]));
        }
        s_lastControllerLabelRefresh = now;
        s_lastControllerMask = controllerMask;
        s_lastNativeMask = nativeMask;
        s_lastGenericMask = genericMask;
    }

    g_controllerChoiceIndex = (s.controllerIndex >= 0 && s.controllerIndex <= 3)
        ? (s.controllerIndex + 1)
        : 0;
}

const char* ValHotkeyGameplay() { return "8 KEYS"; }
const char* ValHotkeySavestate() { return "4 KEYS"; }
const char* ValHotkeyMacros()   { return "3 KEYS"; }
const char* ValHotkeyMenu()     { return "5 KEYS"; }
const char* ValHotkeyController() { return "13 BINDS"; }
const char* ValHotkeyManual()   { return "RAW VK"; }

Row* BuildHotkeysGameplayRows(int& count) {
    static Row s_rows[16];
    int n = 0;

    s_rows[n++] = Header("GAMEPLAY HOTKEYS");
    s_rows[n++] = Action("OPEN MENU",       BindOpenMenu,       ValOpenMenu);
    s_rows[n++] = Action("TELEPORT",        BindTeleport,       ValTeleport);
    s_rows[n++] = Action("SAVE POSITION",   BindSavePosition,   ValSavePosition);
    s_rows[n++] = Action("TOGGLE STATS",    BindToggleStats,    ValToggleStats);
    s_rows[n++] = Action("RESET COUNTER",   BindResetCounter,   ValResetCounter);
    s_rows[n++] = Action("HELP",            BindHelp,           ValHelp);
    s_rows[n++] = Action("TOGGLE OVERLAY",  BindToggleImGui,    ValToggleImGui);
    s_rows[n++] = Action("SWITCH PLAYERS",  BindSwitchPlayers,  ValSwitchPlayers);
    count = n;
    return s_rows;
}

Row* BuildHotkeysMacroRows(int& count) {
    static Row s_rows[8];
    int n = 0;

    s_rows[n++] = Header("MACROS");
    s_rows[n++] = Action("MACRO RECORD",    BindMacroRecord,    ValMacroRecord);
    s_rows[n++] = Action("MACRO PLAY",      BindMacroPlay,      ValMacroPlay);
    s_rows[n++] = Action("MACRO NEXT SLOT", BindMacroSlot,      ValMacroSlot);
    count = n;
    return s_rows;
}

Row* BuildHotkeysSavestateRows(int& count) {
    static Row s_rows[10];
    int n = 0;

    s_rows[n++] = Header("SAVESTATE HOTKEYS");
    s_rows[n++] = Action("SAVE ACTIVE SLOT", BindSavestateSave, ValSavestateSave);
    s_rows[n++] = Action("LOAD ACTIVE SLOT", BindSavestateLoad, ValSavestateLoad);
    s_rows[n++] = Action("SLOT PREVIOUS", BindSavestatePrev, ValSavestatePrev);
    s_rows[n++] = Action("SLOT NEXT",     BindSavestateNext, ValSavestateNext);
    s_rows[n++] = Info("Save/load hotkeys capture or restore the active slot immediately. Slot Previous and Slot Next only change which slot those hotkeys use.");
    count = n;
    return s_rows;
}

Row* BuildHotkeysMenuRows(int& count) {
    static Row s_rows[10];
    int n = 0;

    s_rows[n++] = Header("MENU CONTROL");
    s_rows[n++] = Action("UI ACCEPT",       BindUiAccept,       ValUiAccept);
    s_rows[n++] = Action("UI REFRESH",      BindUiRefresh,      ValUiRefresh);
    s_rows[n++] = Action("UI EXIT",         BindUiExit,         ValUiExit);
    s_rows[n++] = Action("FRAMESTEP PAUSE", BindFramestepPause, ValFramestepPause);
    s_rows[n++] = Action("FRAMESTEP STEP",  BindFramestepStep,  ValFramestepStep);
    count = n;
    return s_rows;
}

Row* BuildHotkeysSwapRows(int& count) {
    static Row s_rows[8];
    int n = 0;
    auto& s = MutableSettings();

    s_rows[n++] = Header("SWAP POSITIONS");
    s_rows[n++] = Toggle("CUSTOM SWAP KEY", &s.swapCustomEnabled, OnSwapCustomEnabled);
    s_rows[n++] = Action("SWAP CUSTOM KEY", BindSwapCustom, ValSwapCustom, SwapCustomKeyDisabled);
    count = n;
    return s_rows;
}

Row* BuildHotkeysManualRows(int& count) {
    static Row s_rows[40];
    int n = 0;

    s_rows[n++] = Header("RAW VK CODES");
    s_rows[n++] = Info("Use this page for direct virtual-key entry when press-to-bind is not enough.");
    s_rows[n++] = Info("Examples: 0x48 or 72. Enter -1 in the editor to disable a binding.");
    s_rows[n++] = Spacer();

    s_rows[n++] = Header("GAMEPLAY");
    s_rows[n++] = Action("OPEN MENU",       EditOpenMenuVk,      ValOpenMenuCode);
    s_rows[n++] = Action("TELEPORT",        EditTeleportVk,      ValTeleportCode);
    s_rows[n++] = Action("SAVE POSITION",   EditSavePositionVk,  ValSavePositionCode);
    s_rows[n++] = Action("TOGGLE STATS",    EditToggleStatsVk,   ValToggleStatsCode);
    s_rows[n++] = Action("RESET COUNTER",   EditResetCounterVk,  ValResetCounterCode);
    s_rows[n++] = Action("HELP",            EditHelpVk,          ValHelpCode);
    s_rows[n++] = Action("TOGGLE OVERLAY",  EditToggleImGuiVk,   ValToggleImGuiCode);
    s_rows[n++] = Action("SWITCH PLAYERS",  EditSwitchPlayersVk, ValSwitchPlayersCode);
    s_rows[n++] = Spacer();

    s_rows[n++] = Header("SAVESTATES / MACROS");
    s_rows[n++] = Action("SAVESTATE SAVE",  EditSavestateSaveVk, ValSavestateSaveCode);
    s_rows[n++] = Action("SAVESTATE LOAD",  EditSavestateLoadVk, ValSavestateLoadCode);
    s_rows[n++] = Action("SLOT PREVIOUS",   EditSavestatePrevVk, ValSavestatePrevCode);
    s_rows[n++] = Action("SLOT NEXT",       EditSavestateNextVk, ValSavestateNextCode);
    s_rows[n++] = Action("MACRO RECORD",    EditMacroRecordVk,   ValMacroRecordCode);
    s_rows[n++] = Action("MACRO PLAY",      EditMacroPlayVk,     ValMacroPlayCode);
    s_rows[n++] = Action("MACRO NEXT SLOT", EditMacroSlotVk,     ValMacroSlotCode);
    s_rows[n++] = Spacer();

    s_rows[n++] = Header("MENU / FRAMESTEP");
    s_rows[n++] = Action("UI ACCEPT",       EditUiAcceptVk,       ValUiAcceptCode);
    s_rows[n++] = Action("UI REFRESH",      EditUiRefreshVk,      ValUiRefreshCode);
    s_rows[n++] = Action("UI EXIT",         EditUiExitVk,         ValUiExitCode);
    s_rows[n++] = Info("Footer hotkeys disallow Enter, Escape, and Space so they do not fight menu controls.");
    s_rows[n++] = Action("FRAMESTEP PAUSE", EditFramestepPauseVk, ValFramestepPauseCode);
    s_rows[n++] = Action("FRAMESTEP STEP",  EditFramestepStepVk,  ValFramestepStepCode);
    s_rows[n++] = Action("SWAP CUSTOM KEY", EditSwapCustomVk,     ValSwapCustomCode, SwapCustomKeyDisabled);
    count = n;
    return s_rows;
}

Row* BuildHotkeysControllerRows(int& count) {
    static Row s_rows[40];
    int n = 0;

    s_rows[n++] = Header("CONTROLLER BINDINGS");
    s_rows[n++] = DropdownRow("CONTROLLER FOR MOD INPUTS", &g_controllerChoiceIndex,
                              g_controllerChoicePtrs, kControllerChoiceCount,
                              OnControllerIndexChoice);
    s_rows[n++] = Info("All (Any) lets any connected controller open the menu and trigger mod actions.");
    s_rows[n++] = Info("Press Enter or the confirm button (A / Cross) on any bind row, release your inputs, then press the new controller button.");
    s_rows[n++] = Info("During controller capture, press your Toggle Menu button on pad to cancel. Delete or Backspace still disables the bind from keyboard.");
    s_rows[n++] = Info("Button names are shown with Xbox labels. On many PlayStation-style pads, read A / B / X / Y as Cross / Circle / Square / Triangle.");
    s_rows[n++] = Spacer();

    s_rows[n++] = Header("GAMEPLAY");
    s_rows[n++] = Action("LOAD / TELEPORT", BindGpTeleport, ValGpTeleport);
    s_rows[n++] = Action("SAVE POSITION", BindGpSavePosition, ValGpSavePosition);
    s_rows[n++] = Action("SWITCH PLAYERS", BindGpSwitchPlayers, ValGpSwitchPlayers);
    s_rows[n++] = Action("SWAP POSITIONS", BindGpSwapPositions, ValGpSwapPositions);
    s_rows[n++] = Spacer();

    s_rows[n++] = Header("MACROS");
    s_rows[n++] = Action("MACRO RECORD", BindGpMacroRecord, ValGpMacroRecord);
    s_rows[n++] = Action("MACRO PLAY", BindGpMacroPlay, ValGpMacroPlay);
    s_rows[n++] = Action("MACRO NEXT SLOT", BindGpMacroSlot, ValGpMacroSlot);
    s_rows[n++] = Spacer();

    s_rows[n++] = Header("MENU");
    s_rows[n++] = Action("TOGGLE MENU", BindGpToggleMenu, ValGpToggleMenu);
    s_rows[n++] = Action("TOGGLE OVERLAY", BindGpToggleOverlay, ValGpToggleOverlay);
    s_rows[n++] = Info("Toggle Menu uses the selected controller for both opening and closing the custom menu.");
    s_rows[n++] = Spacer();

    s_rows[n++] = Header("NAVIGATION");
    s_rows[n++] = Action("TOP TAB PREVIOUS", BindGpUiTopTabPrev, ValGpUiTopTabPrev);
    s_rows[n++] = Action("TOP TAB NEXT", BindGpUiTopTabNext, ValGpUiTopTabNext);
    s_rows[n++] = Action("SUBTAB PREVIOUS", BindGpUiSubTabPrev, ValGpUiSubTabPrev);
    s_rows[n++] = Action("SUBTAB NEXT", BindGpUiSubTabNext, ValGpUiSubTabNext);
    s_rows[n++] = Info("These tab and subtab binds also work while you are inside submenus.");
    s_rows[n++] = Spacer();
    s_rows[n++] = Action("SAVE CONTROLLER BINDS", SaveSettingsToDisk);
    s_rows[n++] = Action("RELOAD FROM DISK", ReloadSettingsFromDisk);
    s_rows[n++] = Info(CurrentConfigPathInfo());
    count = n;
    return s_rows;
}

Row* BuildSettingsHotkeysRows(int& count) {
    static Row s_rows[20];
    int n = 0;

    s_rows[n++] = Header("HOTKEY MENUS");
    s_rows[n++] = Submenu("GAMEPLAY",      "GAMEPLAY HOTKEYS", BuildHotkeysGameplayRows, ValHotkeyGameplay);
    s_rows[n++] = Submenu("SAVESTATE",     "SAVESTATE HOTKEYS", BuildHotkeysSavestateRows, ValHotkeySavestate);
    s_rows[n++] = Submenu("MACROS",        "MACRO HOTKEYS",    BuildHotkeysMacroRows,    ValHotkeyMacros);
    s_rows[n++] = Submenu("MENU CONTROL",  "MENU CONTROL",     BuildHotkeysMenuRows,     ValHotkeyMenu);
    s_rows[n++] = Submenu("RAW VK CODES",  "RAW VK CODES",     BuildHotkeysManualRows,   ValHotkeyManual);
    s_rows[n++] = Submenu("CONTROLLER",    "CONTROLLER BINDINGS", BuildHotkeysControllerRows, ValHotkeyController);
    s_rows[n++] = Submenu("SWAP POSITIONS","SWAP POSITIONS",   BuildHotkeysSwapRows,     ValSwapEnabled);
    s_rows[n++] = Spacer();
    s_rows[n++] = Info(CurrentConfigPathInfo());
    s_rows[n++] = Action("SAVE ALL TO DISK", SaveSettingsToDisk);
    s_rows[n++] = Action("RELOAD FROM DISK", ReloadSettingsFromDisk);
    count = n;
    return s_rows;
}

// ===== SETTINGS / DEBUG =====
bool g_mirrorOverlayBorders = false;
bool g_mirrorRGToasts       = false;
bool g_mirrorPadInputLog    = false;
bool g_mirrorDeepFA         = false;
int g_customSavestateDiskSlot = 0;
CustomSavestate::EditableFields g_customSavestateFields;
char g_customSavestateModeInfo[256] = "Mode: custom savestates active";
char g_customSavestateWorkingInfo[256] = "Current State: empty";
char g_customSavestateMetaInfo[256] = "Meta: n/a";
char g_customSavestateStatusInfo[256] = "Status: idle";
char g_customSavestateDiskInfo[256] = "Slot 0: initial snapshot | memory only";
bool g_customSavestateHotswapPrompt = false;
bool g_customSavestateHotswapDismissed = false;
unsigned int g_customSavestateHotswapWorkingStamp = 0;
char g_customSavestateHotswapInfo[256] = "Loaded slot differs from the current match.";
int g_debugPracticeLocalSide = -1;
bool g_debugSwitchPlayersAvailable = false;
bool g_debugRfFreezeP1Active = false;
bool g_debugRfFreezeP2Active = false;
char g_debugLocalSideInfo[96] = "Current Local: unknown";
char g_debugAiControlInfo[96] = "AI Control Flags: unknown";
char g_debugPracticeCpuInfo[96] = "Practice P2 CPU flag: unknown";
char g_debugGamespeedInfo[96] = "Gamespeed: unknown";
char g_debugRfFreezeP1Info[160] = "P1 RF Freeze: inactive";
char g_debugRfFreezeP2Info[160] = "P2 RF Freeze: inactive";

struct HotswapCurrentState;
int CharacterSelectIdFromInternalCharacterId(int internalCharId);
bool ReadCurrentHotswapState(HotswapCurrentState& state);
bool RevivalBgmMuted();
const char* GetNamedStageLabel(int stageId);
void UpdateCustomSavestateHotswapPromptFromWorking();
void UpdateCustomSavestateHotswapPromptFromSummary(const CustomSavestate::Summary& summary);

const char* const kSavestateBackendChoices[3] = {
    "CUSTOM",
    "REVIVAL",
    "CUSTOM+FALLBACK",
};

int SavedSavestateSelectId(uint8_t rawCharId) {
    if (rawCharId == 0xFF) {
        return -1;
    }

    const int internalCharId = static_cast<int>(rawCharId);
    if (internalCharId < CHAR_ID_AKANE || internalCharId > CHAR_ID_KANO) {
        return -1;
    }

    return CharacterSelectIdFromInternalCharacterId(internalCharId);
}

const char* SavedSavestateDisplayName(uint8_t rawCharId) {
    const int selectId = SavedSavestateSelectId(rawCharId);
    return selectId >= 0 ? CharacterHotswap::GetDisplayNameForSelectId(selectId) : "UNKNOWN";
}

void LogSavestateMirrorStateIfChanged(const char* source,
                                      const CustomSavestate::Summary& summary,
                                      CustomSavestate::BackendMode backendMode,
                                      int activeDiskSlot,
                                      bool fieldsOk) {
    static std::string s_lastLogLine;

    std::ostringstream oss;
    oss << "[SAVESTATE][MENU][" << (source ? source : "MIRROR") << "]"
        << " backend=" << CustomSavestate::BackendModeName(backendMode)
        << " slot=" << activeDiskSlot
        << " stamp=" << summary.workingSnapshotStamp
        << " working=" << (summary.hasWorkingSnapshot ? 1 : 0)
        << " dirty=" << (summary.workingSnapshotDirty ? 1 : 0)
        << " rawChars=" << static_cast<unsigned int>(summary.savedP1CharId)
        << "/" << static_cast<unsigned int>(summary.savedP2CharId)
        << " selectChars=" << SavedSavestateSelectId(summary.savedP1CharId)
        << "/" << SavedSavestateSelectId(summary.savedP2CharId)
        << " stage=" << static_cast<unsigned int>(summary.savedStageId)
        << " compat=" << (summary.currentPairCompatible ? 1 : 0)
        << "/" << (summary.currentStageCompatible ? 1 : 0)
        << "/" << (summary.currentVersionCompatible ? 1 : 0)
        << " restoreAllowed=" << (summary.currentRestoreAllowed ? 1 : 0)
        << " fieldsOk=" << (fieldsOk ? 1 : 0)
        << " saveLoad=" << summary.saveCount << "/" << summary.loadCount
        << " revival=" << summary.savedRevivalVersion;

    const std::string line = oss.str();
    if (line != s_lastLogLine) {
        LogOut(line, true);
        s_lastLogLine = line;
    }
}

bool CustomSavestateDiskSlotExistsCached(int slot, DWORD maxAgeMs = 1000) {
    struct CacheEntry {
        bool valid = false;
        int slot = -999;
        bool exists = false;
        DWORD tick = 0;
    };

    constexpr int kCacheSlots = 16;
    static CacheEntry s_cache[kCacheSlots];
    const int index = (slot >= 0 && slot < kCacheSlots) ? slot : (kCacheSlots - 1);
    CacheEntry& entry = s_cache[index];

    const DWORD now = GetTickCount();
    if (maxAgeMs > 0 &&
        entry.valid &&
        entry.slot == slot &&
        (now - entry.tick) < maxAgeMs) {
        return entry.exists;
    }

    const DWORD start = now;
    const bool exists = CustomSavestate::DoesDiskSlotExist(slot);
    const DWORD elapsed = GetTickCount() - start;

    entry.valid = true;
    entry.slot = slot;
    entry.exists = exists;
    entry.tick = GetTickCount();

    static DWORD s_lastSlowLog = 0;
    if (elapsed >= 25 && (s_lastSlowLog == 0 || (now - s_lastSlowLog) >= 1000)) {
        s_lastSlowLog = now;
        char buf[192];
        _snprintf_s(buf, sizeof(buf), _TRUNCATE,
            "[CUSTOM_MENU][TIMING] Savestate slot existence probe took %lums slot=%d exists=%d",
            static_cast<unsigned long>(elapsed),
            slot,
            exists ? 1 : 0);
        LogOut(buf, true);
    }

    return exists;
}

void RefreshCustomSavestateMirrors() {
    CustomSavestate::Summary summary{};
    CustomSavestate::GetSummary(summary);
    const CustomSavestate::BackendMode backendMode = CustomSavestate::GetConfiguredBackendMode();
    g_customSavestateDiskSlot = CustomSavestate::GetActiveDiskSlot();

    const char* currentVersionName = EfzRevivalVersionName(GetEfzRevivalVersion());
    const char* savedVersionName = summary.savedRevivalVersion != 0
        ? EfzRevivalVersionName(static_cast<EfzRevivalVersion>(summary.savedRevivalVersion))
        : "UNKNOWN/LEGACY";
    const unsigned int revivalSaves = SavestateHook::GetSaveCount();
    const unsigned int revivalLoads = SavestateHook::GetLoadCount();
    if (summary.workingSnapshotStamp != g_customSavestateHotswapWorkingStamp) {
        g_customSavestateHotswapWorkingStamp = summary.workingSnapshotStamp;
        g_customSavestateHotswapDismissed = false;
    }
    UpdateCustomSavestateHotswapPromptFromSummary(summary);

    const bool fieldsOk = summary.hasWorkingSnapshot && CustomSavestate::GetWorkingEditableFields(g_customSavestateFields);
    if (fieldsOk) {
        const std::string p1Name = SavedSavestateDisplayName(summary.savedP1CharId);
        const std::string p2Name = SavedSavestateDisplayName(summary.savedP2CharId);
        const char* savedStageName = GetNamedStageLabel(summary.savedStageId);
        _snprintf_s(g_customSavestateWorkingInfo, sizeof(g_customSavestateWorkingInfo), _TRUNCATE,
                "Current State: %s / %s | %s | stage %s | restore %s/%s/%s | save/load %u/%u | revival %u/%u",
                    p1Name.c_str(),
                    p2Name.c_str(),
                summary.workingSnapshotDirty ? "EDITED" : "READY",
                    savedStageName,
                    summary.currentPairCompatible ? "PAIR OK" : "PAIR BLOCK",
                    summary.currentStageCompatible ? "STAGE OK" : "STAGE BLOCK",
                    summary.currentVersionCompatible ? "VER OK" : "VER BLOCK",
                    summary.saveCount,
                    summary.loadCount,
                    revivalSaves,
                    revivalLoads);
        _snprintf_s(g_customSavestateMetaInfo, sizeof(g_customSavestateMetaInfo), _TRUNCATE,
                    "Meta: saved %s | current %s | BGM %u | P1 0x%X CPU %u | P2 0x%X CPU %u | side %d | BC 0x%X GS 0x%X RB 0x%X",
                    savedVersionName,
                    currentVersionName,
                    summary.savedBgmTrack,
                    summary.savedP1StateSize,
                    summary.savedP1CpuFlag,
                    summary.savedP2StateSize,
                    summary.savedP2CpuFlag,
                    summary.savedLocalSide,
                    summary.savedBattleContextSize,
                    summary.savedGameStateSize,
                    summary.savedRenderBitmapSize);
    } else {
        g_customSavestateFields = CustomSavestate::EditableFields{};
        _snprintf_s(g_customSavestateWorkingInfo, sizeof(g_customSavestateWorkingInfo), _TRUNCATE,
                    "Current State: empty | save/load %u/%u | revival %u/%u",
                    summary.saveCount,
                    summary.loadCount,
                    revivalSaves,
                    revivalLoads);
        _snprintf_s(g_customSavestateMetaInfo, sizeof(g_customSavestateMetaInfo), _TRUNCATE,
                    "Meta: current %s | backend %s",
                    currentVersionName,
                    CustomSavestate::BackendModeName(backendMode));
    }

    switch (backendMode) {
    case CustomSavestate::BackendMode::Revival:
        _snprintf_s(g_customSavestateModeInfo, sizeof(g_customSavestateModeInfo), _TRUNCATE,
                    "Mode: Revival save/load is active. Custom current-state capture, restore, edits, and slot files are locked.");
        break;
    case CustomSavestate::BackendMode::CustomWithRevivalFallback:
        _snprintf_s(g_customSavestateModeInfo, sizeof(g_customSavestateModeInfo), _TRUNCATE,
                    "Mode: Custom savestates are active. Revival stays installed for tracking and supported fallback cases.");
        break;
    case CustomSavestate::BackendMode::Custom:
    default:
        _snprintf_s(g_customSavestateModeInfo, sizeof(g_customSavestateModeInfo), _TRUNCATE,
                    "Mode: Custom savestates are active. Current State editing and slot files are available.");
        break;
    }

    _snprintf_s(g_customSavestateStatusInfo, sizeof(g_customSavestateStatusInfo), _TRUNCATE,
                "Status: %s",
                CustomSavestate::GetLastStatus().c_str());

    constexpr DWORD kDiskInfoRefreshMs = 1000;
    static int s_lastDiskInfoSlot = -999;
    static unsigned int s_lastDiskInfoSaveCount = 0xFFFFFFFFu;
    static unsigned int s_lastDiskInfoLoadCount = 0xFFFFFFFFu;
    static DWORD s_lastDiskInfoRefresh = 0;

    const DWORD diskNow = GetTickCount();
    const bool diskInfoDue = s_lastDiskInfoRefresh == 0
        || (diskNow - s_lastDiskInfoRefresh) >= kDiskInfoRefreshMs
        || s_lastDiskInfoSlot != g_customSavestateDiskSlot
        || s_lastDiskInfoSaveCount != summary.saveCount
        || s_lastDiskInfoLoadCount != summary.loadCount;

    if (diskInfoDue) {
        const DWORD diskStart = diskNow;
        if (g_customSavestateDiskSlot == 0) {
            _snprintf_s(g_customSavestateDiskInfo, sizeof(g_customSavestateDiskInfo), _TRUNCATE,
                        "Slot 0: %s | memory snapshot | next save uses slot 1",
                        CustomSavestateDiskSlotExistsCached(0, 0) ? "READY" : "EMPTY");
        } else {
            const std::string slotPath = CustomSavestate::GetDiskSlotPath(g_customSavestateDiskSlot);
            _snprintf_s(g_customSavestateDiskInfo, sizeof(g_customSavestateDiskInfo), _TRUNCATE,
                        "Slot %d: %s | %s",
                        g_customSavestateDiskSlot,
                        CustomSavestateDiskSlotExistsCached(g_customSavestateDiskSlot, 0) ? "HAS FILE" : "EMPTY",
                        slotPath.c_str());
        }

        s_lastDiskInfoSlot = g_customSavestateDiskSlot;
        s_lastDiskInfoSaveCount = summary.saveCount;
        s_lastDiskInfoLoadCount = summary.loadCount;
        s_lastDiskInfoRefresh = GetTickCount();

        const DWORD diskElapsed = s_lastDiskInfoRefresh - diskStart;
        static DWORD s_lastSlowDiskLog = 0;
        if (diskElapsed >= 25 && (s_lastSlowDiskLog == 0 || (diskNow - s_lastSlowDiskLog) >= 1000)) {
            s_lastSlowDiskLog = diskNow;
            char buf[192];
            _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                "[CUSTOM_MENU][TIMING] Savestate disk mirror took %lums slot=%d",
                static_cast<unsigned long>(diskElapsed),
                g_customSavestateDiskSlot);
            LogOut(buf, true);
        }
    }

    LogSavestateMirrorStateIfChanged("SUMMARY", summary, backendMode, g_customSavestateDiskSlot, fieldsOk);
}

void RefreshDebugRuntimeMirrors() {
    g_debugSwitchPlayersAvailable = GetCurrentGameMode() == GameMode::Practice;
    g_debugPracticeLocalSide = -1;
    if (g_debugSwitchPlayersAvailable) {
        g_debugPracticeLocalSide = SwitchPlayers::GetLocalSide();
    }

    if (g_debugPracticeLocalSide == 0) {
        strncpy_s(g_debugLocalSideInfo, sizeof(g_debugLocalSideInfo), "Current Local: P1", _TRUNCATE);
    } else if (g_debugPracticeLocalSide == 1) {
        strncpy_s(g_debugLocalSideInfo, sizeof(g_debugLocalSideInfo), "Current Local: P2", _TRUNCATE);
    } else if (g_debugSwitchPlayersAvailable) {
        strncpy_s(g_debugLocalSideInfo, sizeof(g_debugLocalSideInfo), "Current Local: (unknown)", _TRUNCATE);
    } else {
        strncpy_s(g_debugLocalSideInfo, sizeof(g_debugLocalSideInfo), "Current Local: Practice mode only", _TRUNCATE);
    }

    _snprintf_s(g_debugAiControlInfo, sizeof(g_debugAiControlInfo), _TRUNCATE,
                "AI Control Flags: P1=%s  P2=%s",
                IsAIControlFlagHuman(1) ? "Human" : "AI",
                IsAIControlFlagHuman(2) ? "Human" : "AI");

    uint8_t p2CpuFlag = 0xFF;
    uintptr_t efzBase = GetEFZBase();
    if (efzBase) {
        uintptr_t gameStatePtr = 0;
        if (SafeReadMemory(efzBase + EFZ_BASE_OFFSET_GAME_STATE, &gameStatePtr, sizeof(gameStatePtr)) && gameStatePtr) {
            SafeReadMemory(gameStatePtr + 4931, &p2CpuFlag, sizeof(p2CpuFlag));
        }
    }
    if (p2CpuFlag != 0xFF) {
        _snprintf_s(g_debugPracticeCpuInfo, sizeof(g_debugPracticeCpuInfo), _TRUNCATE,
                    "Practice P2 CPU flag: %s (byte=%u)", p2CpuFlag ? "CPU" : "Human", (unsigned)p2CpuFlag);
    } else {
        strncpy_s(g_debugPracticeCpuInfo, sizeof(g_debugPracticeCpuInfo), "Practice P2 CPU flag: unknown", _TRUNCATE);
    }

    uint8_t curSpeed = 0xFF;
    if (HMODULE hEfz = GetModuleHandleA("efz.exe")) {
        uint32_t rootPtr = 0;
        if (SafeReadMemory(reinterpret_cast<uintptr_t>(hEfz) + 0x39010C, &rootPtr, sizeof(rootPtr)) && rootPtr) {
            SafeReadMemory(static_cast<uintptr_t>(rootPtr) + 0xF7FF8, &curSpeed, sizeof(curSpeed));
        }
    }
    if (curSpeed != 0xFF) {
        _snprintf_s(g_debugGamespeedInfo, sizeof(g_debugGamespeedInfo), _TRUNCATE,
                    "Gamespeed: %u (0=freeze, 3=normal)", (unsigned)curSpeed);
    } else {
        strncpy_s(g_debugGamespeedInfo, sizeof(g_debugGamespeedInfo), "Gamespeed: unknown", _TRUNCATE);
    }

    auto fillRfInfo = [](int player, char* buf, size_t bufSz, bool& activeOut) {
        bool active = false;
        bool colorManaged = false;
        bool colorBlue = false;
        double value = 0.0;
        activeOut = false;
        if (!GetRFFreezeStatus(player, active, value, colorManaged, colorBlue)) {
            _snprintf_s(buf, bufSz, _TRUNCATE, "P%d RF Freeze: unknown", player);
            return;
        }
        activeOut = active;
        if (!active) {
            _snprintf_s(buf, bufSz, _TRUNCATE, "P%d RF Freeze: inactive", player);
            return;
        }

        const RFFreezeOrigin origin = GetRFFreezeOrigin(player);
        const char* originLabel = "Unknown";
        if (origin == RFFreezeOrigin::ManualUI) originLabel = "Manual UI";
        else if (origin == RFFreezeOrigin::ContinuousRecovery) originLabel = "Continuous Recovery";
        else if (origin == RFFreezeOrigin::Other) originLabel = "Other";

        _snprintf_s(buf, bufSz, _TRUNCATE,
                    "P%d RF Freeze: ACTIVE  RF=%.1f  Color=%s%s  Source=%s",
                    player,
                    value,
                    colorManaged ? "Locked" : "Off",
                    colorManaged ? (colorBlue ? " (Blue)" : " (Red)") : "",
                    originLabel);
    };
    fillRfInfo(1, g_debugRfFreezeP1Info, sizeof(g_debugRfFreezeP1Info), g_debugRfFreezeP1Active);
    fillRfInfo(2, g_debugRfFreezeP2Info, sizeof(g_debugRfFreezeP2Info), g_debugRfFreezeP2Active);
}

void RefreshDebugMirrors() {
    g_mirrorOverlayBorders = g_ShowOverlayDebugBorders.load();
    g_mirrorRGToasts       = g_ShowRGDebugToasts.load();
    g_mirrorPadInputLog    = XInputShim::g_LogGenericPadInputDebug.load();
    g_mirrorDeepFA         = g_deepFrameAdvDebug.load();
    RefreshCustomSavestateMirrors();
}

void OnOverlayBorders() { g_ShowOverlayDebugBorders.store(g_mirrorOverlayBorders); }
void OnRGToasts()       { g_ShowRGDebugToasts.store(g_mirrorRGToasts); }
void OnPadInputLog()    { XInputShim::g_LogGenericPadInputDebug.store(g_mirrorPadInputLog); }
void OnDeepFA()         { g_deepFrameAdvDebug.store(g_mirrorDeepFA); }
int g_bgmSlot = 1;

char g_hotswapOstTrack08Label[64] = "08 - Character Selection (BME)";

const char* const kNamedStageChoices[] = {
    "00 - Courtyard of the Full Moon",
    "01 - Snowy Park (Night)",
    "02 - Lunch Break Courtyard",
    "03 - School Road Park (Day)",
    "04 - School Road Park (Night)",
    "05 - Sunset Rooftop",
    "06 - Shopping Street",
    "07 - Tree of Beginnings",
    "08 - Minase House (Day)",
    "09 - Gymnasium",
    "10 - Rainy Field",
    "11 - Behind the School",
    "12 - World of Eternity",
    "13 - Abandoned Station (Day)",
    "14 - Shrine Near the Sky (Day)",
    "15 - Kamio House",
    "16 - The Infinite Sky",
    "17 - Minase House (Night)",
    "18 - Abandoned Station (Dusk)",
    "19 - FARGO Research Facility",
    "20 - Monomi Hill",
    "21 - Shrine Near the Sky (Night)",
    "22 - Snowy Park (Day)",
};

const unsigned short kNamedOstTracks[] = {
    150,
    0,
    1,
    5,
    6,
    7,
    8,
    10,
    11,
    12,
    13,
    14,
    15,
    16,
    17,
    18,
    19,
    20,
    21,
    22,
    23,
    24,
    25,
    26,
    27,
    28,
    29,
    30,
    31,
    32,
};

const char* const kNamedOstChoices[] = {
    "OFF (150)",
    "00 - Character Selection",
    "01 - Character Selection (Practice)",
    "05 - Staff Roll",
    "06 - Replay Menu OST",
    "07 - Config Menu OST",
    g_hotswapOstTrack08Label,
    "10 - Courtyard of the Full Moon",
    "11 - Snowy Park (Night)",
    "12 - Lunch Break Courtyard",
    "13 - School Road Park (Day)",
    "14 - School Road Park (Night)",
    "15 - Sunset Rooftop",
    "16 - Shopping Street",
    "17 - Tree of Beginnings",
    "18 - Minase House (Day)",
    "19 - Gymnasium",
    "20 - Rainy Field",
    "21 - Behind the School",
    "22 - World of Eternity",
    "23 - Abandoned Station (Day)",
    "24 - Shrine Near the Sky (Day)",
    "25 - Kamio House",
    "26 - The Infinite Sky",
    "27 - Minase House (Night)",
    "28 - Abandoned Station (Dusk)",
    "29 - FARGO Research Facility",
    "30 - Monomi Hill",
    "31 - Shrine Near the Sky (Night)",
    "32 - Snowy Park (Day)",
};

constexpr int kNamedStageChoiceCount = static_cast<int>(sizeof(kNamedStageChoices) / sizeof(kNamedStageChoices[0]));
constexpr int kNamedOstChoiceCount = static_cast<int>(sizeof(kNamedOstChoices) / sizeof(kNamedOstChoices[0]));

const char* GetNamedStageLabel(int stageId) {
    return (stageId >= 0 && stageId < kNamedStageChoiceCount)
        ? kNamedStageChoices[stageId]
        : "UNKNOWN STAGE";
}

bool NetplayModLoaded() {
    return GetModuleHandleA("efz_netplay_mod.dll") != nullptr
        || GetModuleHandleA("efz_netplay_mod") != nullptr;
}

void RefreshNamedOstChoices() {
    _snprintf_s(g_hotswapOstTrack08Label,
                sizeof(g_hotswapOstTrack08Label),
                _TRUNCATE,
                "%s",
                NetplayModLoaded() ? "08 - Netplay Menu OST" : "08 - Character Selection (BME)");
}

int FindNamedOstChoiceIndexByTrack(int trackNumber) {
    for (int i = 0; i < kNamedOstChoiceCount; ++i) {
        if (static_cast<int>(kNamedOstTracks[i]) == trackNumber) {
            return i;
        }
    }
    return -1;
}

unsigned short TrackForNamedOstChoice(int choiceIdx) {
    if (choiceIdx < 0 || choiceIdx >= kNamedOstChoiceCount) {
        return 150;
    }
    return kNamedOstTracks[choiceIdx];
}

int CharacterSelectIdFromInternalCharacterId(int internalCharId) {
    switch (internalCharId) {
        case CHAR_ID_AKANE:    return 4;
        case CHAR_ID_AKIKO:    return 16;
        case CHAR_ID_IKUMI:    return 14;
        case CHAR_ID_MISAKI:   return 7;
        case CHAR_ID_SAYURI:   return 9;
        case CHAR_ID_KANNA:    return 19;
        case CHAR_ID_KAORI:    return 13;
        case CHAR_ID_MAKOTO:   return 3;
        case CHAR_ID_MINAGI:   return 21;
        case CHAR_ID_MIO:      return 11;
        case CHAR_ID_MISHIO:   return 15;
        case CHAR_ID_MISUZU:   return 23;
        case CHAR_ID_MIZUKA:   return 6;
        case CHAR_ID_NAGAMORI: return 6;
        case CHAR_ID_NANASE:   return 0;
        case CHAR_ID_EXNANASE: return 12;
        case CHAR_ID_NAYUKI:   return 10;
        case CHAR_ID_NAYUKIB:  return 17;
        case CHAR_ID_SHIORI:   return 8;
        case CHAR_ID_AYU:      return 1;
        case CHAR_ID_MAI:      return 2;
        case CHAR_ID_MAYU:     return 5;
        case CHAR_ID_MIZUKAB:  return 18;
        case CHAR_ID_KANO:     return 20;
        default:               return 4;
    }
}

bool g_bgmChoiceSeeded = false;

void SeedDebugBgmChoiceIfNeeded() {
    RefreshNamedOstChoices();
    if (g_bgmChoiceSeeded) {
        return;
    }

    uintptr_t gameStatePtr = GetGameStatePtr();
    if (gameStatePtr) {
        const int currentTrack = GetBGMSlot(gameStatePtr);
        const int ostChoice = FindNamedOstChoiceIndexByTrack(currentTrack);
        if (ostChoice >= 0) {
            g_bgmSlot = ostChoice;
        }
    }

    if (g_bgmSlot < 0 || g_bgmSlot >= kNamedOstChoiceCount) {
        g_bgmSlot = 0;
    }
    g_bgmChoiceSeeded = true;
}

void RunStopBGM() {
    uintptr_t gameStatePtr = GetGameStatePtr();
    if (!gameStatePtr) return;
    StopBGM(gameStatePtr);
}
void RunPlayBGM() {
    SeedDebugBgmChoiceIfNeeded();
    uintptr_t gameStatePtr = GetGameStatePtr();
    if (!gameStatePtr) return;
    if (RevivalBgmMuted()) {
        LogOut("[BGM] PLAY BGM ignored because Revival MuteBGM is enabled", true);
        return;
    }
    PlayBGM(gameStatePtr, TrackForNamedOstChoice(g_bgmSlot));
}
void RunP1FinalMemory() {
    const auto& d = ImGuiGui::guiState.localData;
    ExecuteFinalMemory(1, d.p1CharID);
}
void RunP2FinalMemory() {
    const auto& d = ImGuiGui::guiState.localData;
    ExecuteFinalMemory(2, d.p2CharID);
}

void OnFrameBarPersist();
void OnFrameBarTiming();
void OnFrameBarDetail();
bool FrameBarOptionsHidden();
const char* const kFrameBarTimingChoices[2] = { "SUBFRAMES", "VISUAL FRAMES" };
const char* const kFrameBarDetailChoices[3] = { "FULL", "COMPACT", "BARS ONLY" };

const char* ValDebugLogging() { return MutableSettings().detailedLogging ? "DETAILED" : "NORMAL"; }
const char* ValDebugOverlays() {
    return (g_ShowOverlayDebugBorders.load() || g_ShowRGDebugToasts.load()) ? "ON" : "TOOLS";
}
const char* ValDebugSavestate() {
    return CustomSavestate::BackendModeName(CustomSavestate::GetConfiguredBackendMode());
}
const char* ValDebugBgm() {
    SeedDebugBgmChoiceIfNeeded();
    if (g_bgmSlot >= 0 && g_bgmSlot < kNamedOstChoiceCount) {
        return kNamedOstChoices[g_bgmSlot];
    }
    return "OFF (150)";
}
const char* ValFinalMemoryTools() { return "RUN"; }
const char* ValDebugRuntime() {
    if (g_debugPracticeLocalSide == 0) return "P1 LOCAL";
    if (g_debugPracticeLocalSide == 1) return "P2 LOCAL";
    if (g_debugRfFreezeP1Active || g_debugRfFreezeP2Active) return "RF ACTIVE";
    return g_debugSwitchPlayersAvailable ? "LIVE" : "PRACTICE";
}

Row* BuildDebugLoggingRows(int& count) {
    static Row s_rows[16];
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
    count = n;
    return s_rows;
}

Row* BuildDebugOverlayRows(int& count) {
    static Row s_rows[8];
    int n = 0;

    s_rows[n++] = Header("DEBUG OVERLAYS");
    s_rows[n++] = Info("Gameplay overlays moved to Options > Display Overlays.");
    s_rows[n++] = Toggle ("OVERLAY DEBUG BORDERS",     &g_mirrorOverlayBorders,       OnOverlayBorders);
    s_rows[n++] = Toggle ("RG DEBUG TOASTS",           &g_mirrorRGToasts,             OnRGToasts);
    count = n;
    return s_rows;
}

Row* BuildDebugBgmRows(int& count) {
    static Row s_rows[8];
    int n = 0;

    SeedDebugBgmChoiceIfNeeded();

    s_rows[n++] = Header("BGM");
    s_rows[n++] = DropdownRow("BGM TRACK",            &g_bgmSlot, kNamedOstChoices, kNamedOstChoiceCount);
    s_rows[n++] = Action ("PLAY BGM",                  RunPlayBGM);
    s_rows[n++] = Action ("STOP BGM",                  RunStopBGM);
    count = n;
    return s_rows;
}

bool DebugSwitchPlayersDisabled() {
    return !g_debugSwitchPlayersAvailable;
}

bool DebugCancelRFP1Disabled() {
    return !g_debugRfFreezeP1Active;
}

bool DebugCancelRFP2Disabled() {
    return !g_debugRfFreezeP2Active;
}

void RunDebugToggleSwitchPlayers() {
    const bool ok = SwitchPlayers::ToggleLocalSide();
    if (!ok) {
        LogOut("[DEBUG/UI] SwitchPlayers toggle failed (Practice controller not ready?)", true);
        DirectDrawHook::AddMessage("Switch Players: FAILED", "SYSTEM", RGB(255, 100, 100), 1500, 0, 100);
        return;
    }
    RefreshDebugMirrors();
    if (g_debugPracticeLocalSide == 0) {
        DirectDrawHook::AddMessage("Local: P1", "SYSTEM", RGB(100, 255, 100), 1500, 0, 100);
    } else if (g_debugPracticeLocalSide == 1) {
        DirectDrawHook::AddMessage("Local: P2", "SYSTEM", RGB(100, 255, 100), 1500, 0, 100);
    } else {
        DirectDrawHook::AddMessage("Local side changed", "SYSTEM", RGB(100, 255, 100), 1200, 0, 100);
    }
}

void RunCancelRFP1() {
    StopRFFreezePlayer(1);
    RefreshDebugMirrors();
}

void RunCancelRFP2() {
    StopRFFreezePlayer(2);
    RefreshDebugMirrors();
}

Row* BuildDebugRuntimeRows(int& count) {
    static Row s_rows[24];
    int n = 0;

    s_rows[n++] = Header("PRACTICE ROUTING");
    s_rows[n++] = Info("This is the custom-menu port of the ImGui debug runtime readouts for switch-player and pause troubleshooting.");
    s_rows[n++] = Action("TOGGLE SWITCH PLAYERS", RunDebugToggleSwitchPlayers, nullptr, DebugSwitchPlayersDisabled);
    s_rows[n++] = Info(g_debugLocalSideInfo);
    s_rows[n++] = Info(g_debugAiControlInfo);
    s_rows[n++] = Info(g_debugPracticeCpuInfo);
    s_rows[n++] = Info(g_debugGamespeedInfo);
    s_rows[n++] = Spacer();
    s_rows[n++] = Header("RF FREEZE STATUS");
    s_rows[n++] = Info(g_debugRfFreezeP1Info);
    s_rows[n++] = Action("CANCEL P1 RF FREEZE", RunCancelRFP1, nullptr, DebugCancelRFP1Disabled);
    s_rows[n++] = Info(g_debugRfFreezeP2Info);
    s_rows[n++] = Action("CANCEL P2 RF FREEZE", RunCancelRFP2, nullptr, DebugCancelRFP2Disabled);
    count = n;
    return s_rows;
}

bool CustomSavestateWorkingMissing() {
    return !CustomSavestate::HasWorkingSnapshot();
}

bool CustomSavestateDiskSlotMissing() {
    return !CustomSavestateDiskSlotExistsCached(g_customSavestateDiskSlot);
}

bool CustomSavestateUsingRevivalBackend() {
    return CustomSavestate::GetConfiguredBackendMode() == CustomSavestate::BackendMode::Revival;
}

bool CustomSavestateMutationLocked() {
    return CustomSavestateUsingRevivalBackend();
}

bool CustomSavestateInitialSlotSelected() {
    return g_customSavestateDiskSlot == 0;
}

bool CustomSavestateRestoreDisabled() {
    return CustomSavestateMutationLocked() || CustomSavestateWorkingMissing();
}

bool CustomSavestateDiskSaveDisabled() {
    return CustomSavestateMutationLocked() || CustomSavestateWorkingMissing();
}

bool CustomSavestateDiskLoadDisabled() {
    return CustomSavestateMutationLocked() || CustomSavestateDiskSlotMissing();
}

bool CustomSavestateEditorDisabled() {
    return CustomSavestateMutationLocked() || CustomSavestateWorkingMissing();
}

bool CustomSavestateHotswapDisabled() {
    return !CharacterHotswap::CanQueueReload();
}

const char* ValSavestateLive() {
    CustomSavestate::Summary summary{};
    if (!CustomSavestate::GetSummary(summary) || !summary.hasWorkingSnapshot) {
        return "EMPTY";
    }
    return summary.workingSnapshotDirty ? "EDITED" : "READY";
}

const char* ValSavestateSlots() {
    static char text[32] = {};
    const int slot = CustomSavestate::GetActiveDiskSlot();
    if (slot == 0) {
        return "SLOT 0";
    }
    _snprintf_s(text, sizeof(text), _TRUNCATE, "SLOT %d", slot);
    return text;
}

const char* ValSavestateEdit() {
    if (CustomSavestateWorkingMissing()) {
        return "EMPTY";
    }
    return CustomSavestateEditorDisabled() ? "LOCKED" : "READY";
}

void RunCustomSavestateCapture() {
    CustomSavestate::CaptureWorkingSnapshot();
    g_customSavestateHotswapDismissed = false;
    g_customSavestateHotswapPrompt = false;
}

void RunCustomSavestateRestore() {
    CustomSavestate::RestoreWorkingSnapshot();
    g_customSavestateHotswapDismissed = false;
    UpdateCustomSavestateHotswapPromptFromWorking();
}

void RunCustomSavestateSaveToDisk() {
    CustomSavestate::SaveWorkingSnapshotToDisk(g_customSavestateDiskSlot);
}

void RunCustomSavestateLoadFromDisk() {
    if (CustomSavestate::LoadWorkingSnapshotFromDisk(g_customSavestateDiskSlot)) {
        g_customSavestateHotswapDismissed = false;
        UpdateCustomSavestateHotswapPromptFromWorking();
    } else {
        g_customSavestateHotswapPrompt = false;
    }
}

void OnCustomSavestateDiskSlotChanged() {
    CustomSavestate::SetActiveDiskSlot(g_customSavestateDiskSlot);
}

void RunCustomSavestateClear() {
    CustomSavestate::ClearWorkingSnapshot();
    g_customSavestateHotswapDismissed = false;
    g_customSavestateHotswapPrompt = false;
}

void RunCustomSavestateDismissHotswapPrompt() {
    g_customSavestateHotswapDismissed = true;
    g_customSavestateHotswapPrompt = false;
}

void RunCustomSavestateQueueWorkingHotswap() {
    CustomSavestate::Summary summary{};
    if (!CustomSavestate::GetSummary(summary)
        || !summary.hasWorkingSnapshot
        || summary.savedStageId == 0xFF) {
        g_customSavestateHotswapPrompt = false;
        return;
    }

    bool queued = false;
    CharacterHotswap::PaletteSelection paletteSelection{};
    if (CharacterHotswap::ReadCurrentPaletteSelection(paletteSelection)) {
        CharacterHotswap::SanitizePaletteSelection(summary.savedP1CharId,
                                                   summary.savedP2CharId,
                                                   paletteSelection);
        queued = CharacterHotswap::QueueReload(summary.savedP1CharId,
                                               summary.savedP2CharId,
                                               summary.savedStageId,
                                               paletteSelection,
                                               static_cast<unsigned short>(summary.savedBgmTrack));
    } else {
        queued = CharacterHotswap::QueueReload(summary.savedP1CharId,
                                               summary.savedP2CharId,
                                               summary.savedStageId,
                                               static_cast<unsigned short>(summary.savedBgmTrack));
    }

    if (queued && CustomSavestate::QueueWorkingSnapshotRestoreAfterHotswap()) {
        g_customSavestateHotswapDismissed = false;
        g_customSavestateHotswapPrompt = false;
    }
}

void OnCustomSavestateEditorChanged() {
    CustomSavestate::SetWorkingEditableFields(g_customSavestateFields);
}

Row* BuildSavestateLiveRows(int& count) {
    static Row s_rows[12];
    int n = 0;

    s_rows[n++] = Header("CURRENT STATE");
    s_rows[n++] = Info("This is the in-memory savestate used for manual restore, editing, and slot saves.");
    s_rows[n++] = Info("Save Current Match captures the live Practice match here. Load Current State applies it back to the match.");
    s_rows[n++] = Info(g_customSavestateWorkingInfo);
    s_rows[n++] = Info(g_customSavestateMetaInfo);
    s_rows[n++] = Info(g_customSavestateStatusInfo);
    s_rows[n++] = Action("SAVE CURRENT MATCH", RunCustomSavestateCapture, nullptr, CustomSavestateMutationLocked);
    s_rows[n++] = Action("LOAD CURRENT STATE", RunCustomSavestateRestore, nullptr, CustomSavestateRestoreDisabled);
    s_rows[n++] = Action("CLEAR CURRENT STATE", RunCustomSavestateClear, nullptr, CustomSavestateMutationLocked);
    count = n;
    return s_rows;
}

Row* BuildSavestateSlotRows(int& count) {
    static Row s_rows[16];
    int n = 0;

    s_rows[n++] = Header("SLOTS");
    s_rows[n++] = Info("Savestate hotkeys use the active slot directly. Loading a slot here only updates Current State for review, edits, or hotswap.");
    s_rows[n++] = IntNum("ACTIVE SLOT", &g_customSavestateDiskSlot, 0, 8, 1, 1, OnCustomSavestateDiskSlotChanged);
    s_rows[n++] = Info(g_customSavestateDiskInfo);
    s_rows[n++] = Action("LOAD SLOT TO CURRENT STATE", RunCustomSavestateLoadFromDisk, nullptr, CustomSavestateDiskLoadDisabled);
    s_rows[n++] = Action("SAVE CURRENT STATE TO SLOT", RunCustomSavestateSaveToDisk, nullptr, CustomSavestateDiskSaveDisabled);
    if (g_customSavestateHotswapPrompt) {
        s_rows[n++] = Info(g_customSavestateHotswapInfo);
        s_rows[n++] = Action("HOTSWAP TO LOADED MATCH", RunCustomSavestateQueueWorkingHotswap, CharacterHotswap::GetActionValueText, CustomSavestateHotswapDisabled);
        s_rows[n++] = Action("KEEP CURRENT MATCH", RunCustomSavestateDismissHotswapPrompt);
    }
    count = n;
    return s_rows;
}

Row* BuildSavestateP1EditorRows(int& count) {
    static Row s_rows[12];
    int n = 0;

    s_rows[n++] = Header("EDIT P1 STATE");
    s_rows[n++] = IntNum("HP", &g_customSavestateFields.p1Hp, 0, 9999, 1, 100, OnCustomSavestateEditorChanged, CustomSavestateEditorDisabled);
    s_rows[n++] = IntNum("METER", &g_customSavestateFields.p1Meter, 0, 1000, 1, 25, OnCustomSavestateEditorChanged, CustomSavestateEditorDisabled);
    s_rows[n++] = DoubleNum("RF", &g_customSavestateFields.p1Rf, 0.0, 2000.0, 1.0, 25.0, "%.1f", OnCustomSavestateEditorChanged, CustomSavestateEditorDisabled);
    s_rows[n++] = DoubleNum("X", &g_customSavestateFields.p1X, -5000.0, 5000.0, 1.0, 10.0, "%.1f", OnCustomSavestateEditorChanged, CustomSavestateEditorDisabled);
    s_rows[n++] = DoubleNum("Y", &g_customSavestateFields.p1Y, -5000.0, 5000.0, 1.0, 10.0, "%.1f", OnCustomSavestateEditorChanged, CustomSavestateEditorDisabled);
    s_rows[n++] = DoubleNum("X VEL", &g_customSavestateFields.p1XVel, -5000.0, 5000.0, 1.0, 10.0, "%.1f", OnCustomSavestateEditorChanged, CustomSavestateEditorDisabled);
    s_rows[n++] = DoubleNum("Y VEL", &g_customSavestateFields.p1YVel, -5000.0, 5000.0, 1.0, 10.0, "%.1f", OnCustomSavestateEditorChanged, CustomSavestateEditorDisabled);
    s_rows[n++] = IntNum("CPU", &g_customSavestateFields.p1CpuFlag, 0, 1, 1, 1, OnCustomSavestateEditorChanged, CustomSavestateEditorDisabled);
    count = n;
    return s_rows;
}

Row* BuildSavestateP2EditorRows(int& count) {
    static Row s_rows[12];
    int n = 0;

    s_rows[n++] = Header("EDIT P2 STATE");
    s_rows[n++] = IntNum("HP", &g_customSavestateFields.p2Hp, 0, 9999, 1, 100, OnCustomSavestateEditorChanged, CustomSavestateEditorDisabled);
    s_rows[n++] = IntNum("METER", &g_customSavestateFields.p2Meter, 0, 1000, 1, 25, OnCustomSavestateEditorChanged, CustomSavestateEditorDisabled);
    s_rows[n++] = DoubleNum("RF", &g_customSavestateFields.p2Rf, 0.0, 2000.0, 1.0, 25.0, "%.1f", OnCustomSavestateEditorChanged, CustomSavestateEditorDisabled);
    s_rows[n++] = DoubleNum("X", &g_customSavestateFields.p2X, -5000.0, 5000.0, 1.0, 10.0, "%.1f", OnCustomSavestateEditorChanged, CustomSavestateEditorDisabled);
    s_rows[n++] = DoubleNum("Y", &g_customSavestateFields.p2Y, -5000.0, 5000.0, 1.0, 10.0, "%.1f", OnCustomSavestateEditorChanged, CustomSavestateEditorDisabled);
    s_rows[n++] = DoubleNum("X VEL", &g_customSavestateFields.p2XVel, -5000.0, 5000.0, 1.0, 10.0, "%.1f", OnCustomSavestateEditorChanged, CustomSavestateEditorDisabled);
    s_rows[n++] = DoubleNum("Y VEL", &g_customSavestateFields.p2YVel, -5000.0, 5000.0, 1.0, 10.0, "%.1f", OnCustomSavestateEditorChanged, CustomSavestateEditorDisabled);
    s_rows[n++] = IntNum("CPU", &g_customSavestateFields.p2CpuFlag, 0, 1, 1, 1, OnCustomSavestateEditorChanged, CustomSavestateEditorDisabled);
    count = n;
    return s_rows;
}

Row* BuildSavestateMatchEditorRows(int& count) {
    static Row s_rows[8];
    int n = 0;

    s_rows[n++] = Header("EDIT MATCH STATE");
    s_rows[n++] = Info(g_customSavestateMetaInfo);
    s_rows[n++] = IntNum("LOCAL SIDE", &g_customSavestateFields.localSide, 0, 1, 1, 1, OnCustomSavestateEditorChanged, CustomSavestateEditorDisabled);
    count = n;
    return s_rows;
}

Row* BuildDebugSavestateRows(int& count) {
    static Row s_rows[20];
    int n = 0;
    auto& s = MutableSettings();

    s_rows[n++] = Header("SAVESTATES");
    s_rows[n++] = Info("Hotkey save/load uses the active slot. Current State is the in-memory snapshot you can restore, edit, and write to a slot.");
    s_rows[n++] = ChoicesRow("BACKEND", &s.savestateBackendMode, kSavestateBackendChoices, 3, OnSavestateBackendMode);
    s_rows[n++] = Toggle("LOAD CUSTOM PALETTES", &s.savestateLoadCustomPalettes, OnSavestateLoadCustomPalettes);
    s_rows[n++] = Info("When off, savestate loads keep the saved palette number but force default palettes instead of custom .pal files.");
    s_rows[n++] = Info("If a savestate load gives you broken colors or other graphical issues, turn Load Custom Palettes off and load again.");
    s_rows[n++] = Info(g_customSavestateModeInfo);
    s_rows[n++] = Info(g_customSavestateWorkingInfo);
    s_rows[n++] = Info(g_customSavestateDiskInfo);
    s_rows[n++] = Info(g_customSavestateStatusInfo);
    s_rows[n++] = Submenu("CURRENT STATE", "CURRENT STATE", BuildSavestateLiveRows, ValSavestateLive);
    s_rows[n++] = Submenu("SLOTS",         "SAVESTATE SLOTS", BuildSavestateSlotRows, ValSavestateSlots);
    s_rows[n++] = Submenu("EDIT P1",       "EDIT P1 STATE", BuildSavestateP1EditorRows, ValSavestateEdit);
    s_rows[n++] = Submenu("EDIT P2",       "EDIT P2 STATE", BuildSavestateP2EditorRows, ValSavestateEdit);
    s_rows[n++] = Submenu("EDIT MATCH",    "EDIT MATCH STATE", BuildSavestateMatchEditorRows, ValSavestateEdit);
    count = n;
    return s_rows;
}

Row* BuildDebugFinalMemoryRows(int& count) {
    static Row s_rows[8];
    int n = 0;

    s_rows[n++] = Header("FINAL MEMORY");
    s_rows[n++] = Action ("RUN P1 FINAL MEMORY",       RunP1FinalMemory);
    s_rows[n++] = Action ("RUN P2 FINAL MEMORY",       RunP2FinalMemory);
    count = n;
    return s_rows;
}

Row* BuildSettingsDebugRows(int& count) {
    static Row s_rows[20];
    int n = 0;

    s_rows[n++] = Header("DEBUG MENUS");
    s_rows[n++] = Submenu("LOGGING",      "DEBUG LOGGING", BuildDebugLoggingRows,     ValDebugLogging);
    s_rows[n++] = Submenu("OVERLAYS",     "DEBUG OVERLAYS", BuildDebugOverlayRows,     ValDebugOverlays);
    s_rows[n++] = Submenu("INPUT / RUNTIME", "INPUT / RUNTIME", BuildDebugRuntimeRows, ValDebugRuntime);
    s_rows[n++] = Submenu("BGM",          "BGM",            BuildDebugBgmRows,         ValDebugBgm);
    s_rows[n++] = Submenu("FINAL MEMORY", "FINAL MEMORY",   BuildDebugFinalMemoryRows, ValFinalMemoryTools);
    s_rows[n++] = Spacer();
    s_rows[n++] = Info(CurrentConfigPathInfo());
    s_rows[n++] = Action ("SAVE ALL TO DISK",          SaveSettingsToDisk);
    s_rows[n++] = Action ("RELOAD FROM DISK",          ReloadSettingsFromDisk);
    count = n;
    return s_rows;
}

// ===== HELP screen =====
char g_helpVersionStr[64];
char g_helpBuildStr[64];
char g_helpOpenHelp[96];
char g_helpToggleOverlay[128];
char g_helpSavePos[128];
char g_helpLoadPos[128];
char g_helpSwapPos[128];
char g_helpToggleStats[96];
char g_helpSwitchPlayers[128];
char g_helpUiFooter[128];
char g_helpTopTabs[160];
char g_helpSubTabs[192];
char g_helpControllerSupport[192];
char g_helpMacroRecord[128];
char g_helpMacroPlay[128];
char g_helpMacroSlot[96];
char g_helpSavestateSave[128];
char g_helpSavestateLoad[128];
char g_helpFaDuration[80];
char g_helpDetectedVersion[96];
char g_helpP1WikiLabel[96];
char g_helpP2WikiLabel[96];
char g_helpP1WikiUrl[192];
char g_helpP2WikiUrl[192];

void RefreshHelpStrings() {
    const auto& s = Config::GetSettings();
    _snprintf_s(g_helpVersionStr, sizeof(g_helpVersionStr), _TRUNCATE,
                "EFZ Training Mode v%s", EFZ_TRAINING_MODE_VERSION);
    _snprintf_s(g_helpBuildStr, sizeof(g_helpBuildStr), _TRUNCATE,
                "Build %s %s", EFZ_TRAINING_MODE_BUILD_DATE, EFZ_TRAINING_MODE_BUILD_TIME);
    _snprintf_s(g_helpOpenHelp, sizeof(g_helpOpenHelp), _TRUNCATE,
                "Open this Help page: %s.", Config::GetKeyName(s.helpKey).c_str());
    _snprintf_s(g_helpToggleOverlay, sizeof(g_helpToggleOverlay), _TRUNCATE,
                "Toggle the overlay: %s (Controller: %s).",
                Config::GetKeyName(s.toggleImGuiKey).c_str(),
                Config::GetGamepadButtonName(s.gpToggleMenuButton).c_str());
    _snprintf_s(g_helpSavePos, sizeof(g_helpSavePos), _TRUNCATE,
                "Save the current position: %s (Controller: %s).",
                Config::GetKeyName(s.recordKey).c_str(),
                Config::GetGamepadButtonName(s.gpSavePositionButton).c_str());
    _snprintf_s(g_helpLoadPos, sizeof(g_helpLoadPos), _TRUNCATE,
                "Load a saved or directional position: %s (Controller: %s).",
                Config::GetKeyName(s.teleportKey).c_str(),
                Config::GetGamepadButtonName(s.gpTeleportButton).c_str());
    _snprintf_s(g_helpSwapPos, sizeof(g_helpSwapPos), _TRUNCATE,
                "Swap positions: hold Load + D, or press %s on controller.",
                Config::GetGamepadButtonName(s.gpSwapPositionsButton).c_str());
    _snprintf_s(g_helpToggleStats, sizeof(g_helpToggleStats), _TRUNCATE,
                "Toggle the stats display: %s.", Config::GetKeyName(s.toggleTitleKey).c_str());
    _snprintf_s(g_helpSwitchPlayers, sizeof(g_helpSwitchPlayers), _TRUNCATE,
                "Switch player control: %s (Controller: %s).",
                Config::GetKeyName(s.switchPlayersKey).c_str(),
                Config::GetGamepadButtonName(s.gpSwitchPlayersButton).c_str());
    _snprintf_s(g_helpUiFooter, sizeof(g_helpUiFooter), _TRUNCATE,
                "Footer shortcuts: Apply=%s, Refresh=%s, Exit=%s.",
                Config::GetKeyName(s.uiAcceptKey).c_str(),
                Config::GetKeyName(s.uiRefreshKey).c_str(),
                Config::GetKeyName(s.uiExitKey).c_str());
    _snprintf_s(g_helpTopTabs, sizeof(g_helpTopTabs), _TRUNCATE,
                "Top tabs: %s / %s on controller, or PgUp / PgDn on keyboard.",
                Config::GetGamepadButtonName(s.gpUiTopTabPrev).c_str(),
                Config::GetGamepadButtonName(s.gpUiTopTabNext).c_str());
    _snprintf_s(g_helpSubTabs, sizeof(g_helpSubTabs), _TRUNCATE,
                "Subtabs: %s / %s on controller, or [ / ] on keyboard. These also work while you are inside submenus.",
                Config::GetGamepadButtonName(s.gpUiSubTabPrev).c_str(),
                Config::GetGamepadButtonName(s.gpUiSubTabNext).c_str());
    _snprintf_s(g_helpControllerSupport, sizeof(g_helpControllerSupport), _TRUNCATE,
                "Controller support: native XInput pads and DirectInput fallback pads use the same binds. Button names are shown with Xbox labels; on many PlayStation-style pads A/B/X/Y map to Cross/Circle/Square/Triangle.");
    _snprintf_s(g_helpMacroRecord, sizeof(g_helpMacroRecord), _TRUNCATE,
                "Record macro: %s (Controller: %s).",
                Config::GetKeyName(s.macroRecordKey).c_str(),
                Config::GetGamepadButtonName(s.gpMacroRecordButton).c_str());
    _snprintf_s(g_helpMacroPlay, sizeof(g_helpMacroPlay), _TRUNCATE,
                "Play macro: %s (Controller: %s).",
                Config::GetKeyName(s.macroPlayKey).c_str(),
                Config::GetGamepadButtonName(s.gpMacroPlayButton).c_str());
    _snprintf_s(g_helpMacroSlot, sizeof(g_helpMacroSlot), _TRUNCATE,
                "Cycle macro slots: %s.", Config::GetKeyName(s.macroSlotKey).c_str());
    _snprintf_s(g_helpSavestateSave, sizeof(g_helpSavestateSave), _TRUNCATE,
                "Save Practice snapshot: %s.", Config::GetKeyName(s.savestateSaveKey).c_str());
    _snprintf_s(g_helpSavestateLoad, sizeof(g_helpSavestateLoad), _TRUNCATE,
                "Load Practice snapshot: %s.", Config::GetKeyName(s.savestateLoadKey).c_str());
    _snprintf_s(g_helpFaDuration, sizeof(g_helpFaDuration), _TRUNCATE,
                "Frame advantage labels last about %.1f sec.", s.frameAdvantageDisplayDuration);

    const EfzRevivalVersion rv = GetEfzRevivalVersion();
    _snprintf_s(g_helpDetectedVersion, sizeof(g_helpDetectedVersion), _TRUNCATE,
                "Detected: %s %s",
                EfzRevivalVersionName(rv),
                IsEfzRevivalVersionSupported(rv) ? "(supported)" : "(unsupported)");

    g_helpP1WikiLabel[0] = '\0';
    g_helpP2WikiLabel[0] = '\0';
    g_helpP1WikiUrl[0] = '\0';
    g_helpP2WikiUrl[0] = '\0';
    const auto& d = ImGuiGui::guiState.localData;
    auto fillWiki = [](int charId, const char* rawName, int player, char* label, size_t labelSz, char* url, size_t urlSz) {
        const char* path = nullptr;
        switch (charId) {
            case CHAR_ID_AKANE:    path = "Akane_Satomura"; break;
            case CHAR_ID_AKIKO:    path = "Akiko_Minase"; break;
            case CHAR_ID_AYU:      path = "Ayu_Tsukimiya"; break;
            case CHAR_ID_EXNANASE: path = "Doppel_Nanase"; break;
            case CHAR_ID_IKUMI:    path = "Ikumi_Amasawa"; break;
            case CHAR_ID_KANNA:    path = "Kanna"; break;
            case CHAR_ID_KANO:     path = "Kano_Kirishima"; break;
            case CHAR_ID_KAORI:    path = "Kaori_Misaka"; break;
            case CHAR_ID_MAI:      path = "Mai_Kawasumi"; break;
            case CHAR_ID_MAKOTO:   path = "Makoto_Sawatari"; break;
            case CHAR_ID_MAYU:     path = "Mayu_Shiina"; break;
            case CHAR_ID_MINAGI:   path = "Minagi_Tohno"; break;
            case CHAR_ID_MIO:      path = "Mio_Kouzuki"; break;
            case CHAR_ID_MISAKI:   path = "Misaki_Kawana"; break;
            case CHAR_ID_MISHIO:   path = "Mishio_Amano"; break;
            case CHAR_ID_MISUZU:   path = "Misuzu_Kamio"; break;
            case CHAR_ID_MIZUKA:
            case CHAR_ID_NAGAMORI: path = "Mizuka_Nagamori"; break;
            case CHAR_ID_NANASE:   path = "Rumi_Nanase"; break;
            case CHAR_ID_SAYURI:   path = "Sayuri_Kurata"; break;
            case CHAR_ID_SHIORI:   path = "Shiori_Misaka"; break;
            case CHAR_ID_NAYUKI:   path = "Nayuki_Minase_(asleep)"; break;
            case CHAR_ID_NAYUKIB:  path = "Nayuki_Minase_(awake)"; break;
            case CHAR_ID_MIZUKAB:  path = "UNKNOWN"; break;
            default: break;
        }
        if (!path) return;
        std::string name = CharacterSettings::GetCharacterName(charId);
        if (name.empty() || name == "Undefined") {
            name = (rawName && rawName[0]) ? rawName : "CHARACTER";
        }
        _snprintf_s(label, labelSz, _TRUNCATE, "OPEN P%d WIKI: %s", player, name.c_str());
        _snprintf_s(url, urlSz, _TRUNCATE, "https://wiki.gbl.gg/w/Eternal_Fighter_Zero/%s", path);
    };
    fillWiki(d.p1CharID, d.p1CharName, 1, g_helpP1WikiLabel, sizeof(g_helpP1WikiLabel), g_helpP1WikiUrl, sizeof(g_helpP1WikiUrl));
    fillWiki(d.p2CharID, d.p2CharName, 2, g_helpP2WikiLabel, sizeof(g_helpP2WikiLabel), g_helpP2WikiUrl, sizeof(g_helpP2WikiUrl));
}

void OpenUrl(const char* url) {
    if (!url || !url[0]) return;
    ShellExecuteA(nullptr, "open", url, nullptr, nullptr, SW_SHOWNORMAL);
}

void OpenGithubReleases() { OpenUrl("https://github.com/Aquat1c/efz-training-mode/releases"); }
void OpenEternalFighterZeroWiki() { OpenUrl("https://wiki.gbl.gg/w/Eternal_Fighter_Zero"); }
void OpenTrainingModeWiki() { OpenUrl("https://wiki.gbl.gg/w/Eternal_Fighter_Zero/Training_Mode"); }
void OpenEfzDiscord() { OpenUrl("https://discord.gg/aUgqXAt"); }
void OpenP1Wiki() { OpenUrl(g_helpP1WikiUrl); }
void OpenP2Wiki() { OpenUrl(g_helpP2WikiUrl); }
bool P1WikiDisabled() { return g_helpP1WikiUrl[0] == '\0'; }
bool P2WikiDisabled() { return g_helpP2WikiUrl[0] == '\0'; }
const char* ValWiki() { return "OPEN"; }

void DrawMichiruInline(ImDrawList* dl, float x, float y, float w, float h) {
    if (!dl) return;

    const float left = x;
    const float top = y + 2.0f;
    const float right = x + w;
    const float bottom = y + h;

    unsigned gw = 0, gh = 0;
    if (IDirect3DTexture9* tex = GifPlayer::GetTexture(gw, gh)) {
        float drawW = static_cast<float>(gw);
        float drawH = static_cast<float>(gh);
        const float maxW = (std::min)(right - left, 300.0f);
        const float maxH = bottom - top;
        if (drawW > maxW) {
            const float scale = maxW / drawW;
            drawW *= scale;
            drawH *= scale;
        }
        if (drawH > maxH) {
            const float scale = maxH / drawH;
            drawW *= scale;
            drawH *= scale;
        }

        const float imgX = left + (right - left - drawW) * 0.5f;
        const float imgY = top;
        dl->AddImage((ImTextureID)tex, ImVec2(imgX, imgY), ImVec2(imgX + drawW, imgY + drawH));
    }
}

Row* BuildHelpQuickStartRows(int& count) {
    static Row s_rows[32];
    int n = 0;
    s_rows[n++] = Header("QUICK START");
    s_rows[n++] = Info("Open the menu during Practice, set up the drill, then close it to keep playing. The game pauses while the menu is open.");
    s_rows[n++] = Info("Use Main > Opponent for dummy behavior, Main > Options for recovery and overlays, Auto for triggers/macros, and Chars for matchup-specific tools.");
    s_rows[n++] = Info("Use Main > Menu when you want to return to Character Select or the Title Screen.");
    s_rows[n++] = Info("Most toggles apply as soon as you change them. HP, meter, RF, position, and some character values are applied when you adjust them or confirm an edit.");
    s_rows[n++] = Info("Practice hotkeys are ignored while the menu is open, then briefly cooled down when it closes so one press does not leak into gameplay.");
    s_rows[n++] = Spacer();
    s_rows[n++] = Header("FAST SETUP");
    s_rows[n++] = Info("1. Save a position once for quick spacing resets, or save a full Practice snapshot under Main > Options > Savestates for entire match state.");
    s_rows[n++] = Info("2. Pick dummy defense, recovery, or movement in Main > Opponent.");
    s_rows[n++] = Info("3. Turn on Frame Advantage, Combo Statistics, Frame Bar, or Framestep in Main > Options when you need extra feedback.");
    s_rows[n++] = Info("4. Use Auto Actions or Macros for repeatable wakeup, block, hitstun, airtech, and RG tests.");
    s_rows[n++] = Spacer();
    s_rows[n++] = Header("COMMON HOTKEYS");
    s_rows[n++] = Info(g_helpOpenHelp);
    s_rows[n++] = Info(g_helpToggleOverlay);
    s_rows[n++] = Info(g_helpSavePos);
    s_rows[n++] = Info(g_helpLoadPos);
    s_rows[n++] = Info(g_helpToggleStats);
    count = n;
    return s_rows;
}

Row* BuildHelpPositionRows(int& count) {
    static Row s_rows[18];
    int n = 0;
    s_rows[n++] = Header("POSITION TOOLS");
    s_rows[n++] = Info("Tap Load Position by itself to return to your saved spot.");
    s_rows[n++] = Info("Hold Load Position with a direction to place both players without needing to save first.");
    s_rows[n++] = Info("Load + Down centers both players. Load + Left or Right moves them to the nearest corner.");
    s_rows[n++] = Info("Load + Down + A returns to round-start spacing. On controller, use D-Pad Down + A + Load.");
    s_rows[n++] = Info(g_helpSwapPos);
    s_rows[n++] = Spacer();
    s_rows[n++] = Header("BINDINGS");
    s_rows[n++] = Info(g_helpLoadPos);
    s_rows[n++] = Info(g_helpSavePos);
    count = n;
    return s_rows;
}

Row* BuildHelpMenuTipsRows(int& count) {
    static Row s_rows[32];
    int n = 0;
    s_rows[n++] = Header("NAVIGATION");
    s_rows[n++] = Info  ("Use Up/Down or the D-Pad to move focus. Press Enter, Space, or the controller confirm button (A / Cross) to pick the highlighted row.");
    s_rows[n++] = Info  ("Left/Right adjusts the selected value. Hold Shift while pressing Left/Right to use the larger adjustment step.");
    s_rows[n++] = Info  ("Esc or the controller back button (B / Circle) returns from a submenu, closes a picker, or closes the menu.");
    s_rows[n++] = Spacer();
    s_rows[n++] = Header("TABS");
    s_rows[n++] = Info  (g_helpTopTabs);
    s_rows[n++] = Info  (g_helpSubTabs);
    s_rows[n++] = Info  ("Number keys 1..5 jump directly to a top tab. From the first row in a list, press Up to move focus into subtab and tab selection.");
    s_rows[n++] = Spacer();
    s_rows[n++] = Header("MOUSE");
    s_rows[n++] = Info  ("Mouse hover only takes focus after the pointer moves. Keyboard and gamepad edges take priority over a resting cursor.");
    s_rows[n++] = Info  ("Click activates a row; the mouse wheel scrolls long lists.");
    s_rows[n++] = Spacer();
    s_rows[n++] = Header("TIPS");
    s_rows[n++] = Info  (g_helpUiFooter);
    s_rows[n++] = Info  (g_helpControllerSupport);
    s_rows[n++] = Info  ("Controller bindings can be changed in Settings > Hotkeys > Controller.");
    s_rows[n++] = Info  ("If text feels too small or large, adjust UI Scale in Settings > General > Interface.");
    s_rows[n++] = Info  (g_helpOpenHelp);
    count = n;
    return s_rows;
}

Row* BuildHelpStartRows(int& count) {
    static Row s_rows[16];
    int n = 0;
    s_rows[n++] = Header("GETTING STARTED");
    s_rows[n++] = Info("Use this page when you need a reminder without leaving Practice.");
    s_rows[n++] = Info("Quick Start covers the normal training flow. Position Tools lists save/load shortcuts. Menu Tips covers navigation.");
    s_rows[n++] = Info("Long pages scroll with Up/Down, D-Pad, mouse wheel, or the list controls shown in the footer.");
    s_rows[n++] = Spacer();
    s_rows[n++] = Header("START MENUS");
    s_rows[n++] = Submenu("QUICK START",    "QUICK START",    BuildHelpQuickStartRows, nullptr);
    s_rows[n++] = Submenu("POSITION TOOLS", "POSITION TOOLS", BuildHelpPositionRows,   nullptr);
    s_rows[n++] = Submenu("MENU TIPS",      "MENU TIPS",      BuildHelpMenuTipsRows,   nullptr);
    count = n;
    return s_rows;
}

Row* BuildHelpBasicsRows(int& count) {
    static Row s_rows[40];
    int n = 0;
    s_rows[n++] = Header("BASICS");
    s_rows[n++] = Info("Core dummy behavior lives in Main > Opponent. These are the first options to check when building a drill.");
    s_rows[n++] = Info(g_helpSwitchPlayers);
    s_rows[n++] = Info("P2 Control lets you play from Player 2's side. Some side-switch training keys are disabled while it is on.");
    s_rows[n++] = Info("Dummy Auto-Block supports Off, Block All, Only Block First Hit, and Block After First Hit.");
    s_rows[n++] = Info("Adaptive Stance automatically picks high guard against air or overhead attacks and low guard against grounded attacks, so manual stance is hidden while it is on.");
    s_rows[n++] = Spacer();
    s_rows[n++] = Header("BLOCK AND RG");
    s_rows[n++] = Info("Random Block flips a coin when the dummy is allowed to block; it is useful for hit-confirm practice.");
    s_rows[n++] = Info("Always RG treats eligible blocks as Recoil Guard. Random RG flips a coin each time the dummy tries to block.");
    s_rows[n++] = Info("Counter RG tries to RG back after you Recoil Guard, where the game allows it.");
    s_rows[n++] = Info("Random Block, Random RG, and Always RG can conflict. Turning one on can turn others off automatically.");
    s_rows[n++] = Spacer();
    s_rows[n++] = Header("TRAINING TOOLS");
    s_rows[n++] = Info("Auto-Airtech uses Neutral as disabled, or Forward and Back to recover in that direction. Delay adds frames before tech, which is useful for testing late airtech situations.");
    s_rows[n++] = Info("Auto-Jump makes P1, P2, or both sides jump neutral, forward, or backward when able.");
    s_rows[n++] = Info("Final Memory: Allow at any HP removes HP checks. Turn it off when you want normal game requirements.");
    s_rows[n++] = Spacer();
    s_rows[n++] = Header("FRAME ADVANTAGE");
    s_rows[n++] = Info(g_helpFaDuration);
    s_rows[n++] = Info("Frame Advantage appears after both sides recover. Gaps briefly flash during strings when there is a hole.");
    s_rows[n++] = Info("During Recoil Guard, FA1 and FA2 labels show advantage for each part.");
    count = n;
    return s_rows;
}

Row* BuildHelpRecoveryRows(int& count) {
    static Row s_rows[36];
    int n = 0;
    s_rows[n++] = Header("CONTINUOUS RECOVERY");
    s_rows[n++] = Info("Continuous Recovery restores HP, meter, and RF when a side returns to neutral. Configure it per player under Main > Values > Continuous Recovery.");
    s_rows[n++] = Info("It disables itself while the game's own HP, meter, or RF recovery is active through F4/F5, so both recovery systems do not fight each other.");
    s_rows[n++] = Info("HP and meter can be Off, preset values, or Custom. RF can use presets or a custom amount; Blue IC is available under RF Custom.");
    s_rows[n++] = Info("RF Freeze can hold RF after Recovery sets it until you turn Recovery (RF) off, so it will not increase by itself.");
    s_rows[n++] = Spacer();
    s_rows[n++] = Header("ENGINE RECOVERY");
    s_rows[n++] = Info("Configure vanilla regeneration directly under Main > Values.");
    s_rows[n++] = Info("While F5 or F4 is active, manual value edits are disallowed. X/Y positions can still be changed in the Values tab.");
    s_rows[n++] = Info("Tip: if numbers look wrong, press F4/F5 until the game returns to Normal mode, then re-apply your training values.");
    count = n;
    return s_rows;
}

Row* BuildHelpCharacterRows(int& count) {
    static Row s_rows[48];
    int n = 0;
    s_rows[n++] = Header("CHARACTER SETTINGS");
    s_rows[n++] = Info("Chars only shows controls for characters currently in the match. Match-wide locks appear above the player menus when the matchup supports them.");
    s_rows[n++] = Info("Match-wide locks: Infinite Ikumi Blood, Infinite Misuzu Feather, Lock Mishio Element, Infinite Mishio Awaken, and Minagi Projectiles -> Michiru.");
    s_rows[n++] = Spacer();
    s_rows[n++] = Header("CHARACTER ROWS");
    s_rows[n++] = Info("Ikumi: set Blood Stock, Genocide Timer, and Level Gauge.");
    s_rows[n++] = Info("Misuzu: set Feathers, Infinite Poison, Poison Timer, and Poison Level.");
    s_rows[n++] = Info("Mishio: choose Element and set Awaken Timer; the match-wide locks keep element and awakened state from decaying.");
    s_rows[n++] = Info("Akiko: set Bullet Cycle, Freeze Cycle, Show Clean Hit, Time-Slow Trigger, and Infinite Timeslow.");
    s_rows[n++] = Info("Nayuki (Awake): Infinite Snow and Snowbunny Timer control bunny duration.");
    s_rows[n++] = Info("Nayuki (Asleep): Jam Count and Lock Jam Count control stored jams.");
    s_rows[n++] = Info("Kano: Magic sets the stored magic value, and Lock Magic keeps it from being spent.");
    s_rows[n++] = Info("Nanase (Rumi): Infinite Shinai, Barehanded Mode, Kimchi Active, Infinite Kimchi, and Kimchi Timer control her weapon and Final Memory state.");
    s_rows[n++] = Info("Doppel: Enlightened puts Doppel in the Final Memory state.");
    s_rows[n++] = Info("Mio: Stance switches Short/Long stance, and Lock Stance prevents automatic stance changes.");
    s_rows[n++] = Info("Mai: Status, Ghost Time, Charge Timer, Awaken Timer, Infinite Ghost/Charge/Awaken, No Charge Cooldown, and Aggressive Summon control Mini-Mai setups.");
    s_rows[n++] = Info("Mai also has Force Summon, Force Despawn, and Ghost Target X/Y with Apply Ghost Position for exact setup placement.");
    s_rows[n++] = Info("Minagi: Always Readied keeps Michiru ready, and Michiru Target X/Y with Apply Michiru Position places her for setup testing.");
    s_rows[n++] = Info("Mizuka: Note Trigger Ranges and Affected Notes control the note interaction overlays.");
    count = n;
    return s_rows;
}

Row* BuildHelpAutoActionRows(int& count) {
    static Row s_rows[44];
    int n = 0;
    s_rows[n++] = Header("AUTO ACTIONS");
    s_rows[n++] = Info("Auto Actions make the dummy act on key moments: On Wakeup, After Block, After Hitstun, After Airtech, or on Recoil Guard.");
    s_rows[n++] = Info("Enable the Auto Action system first, then enable the triggers you want. Target defaults to Player 2, but you can apply it to P1 or both players.");
    s_rows[n++] = Info("Randomize Triggers adds a coin-flip so a trigger can sometimes skip activation. Pre-buffer Wakeup performs wake specials, dashes, and macros slightly early.");
    s_rows[n++] = Spacer();
    s_rows[n++] = Header("PER TRIGGER");
    s_rows[n++] = Info("Pick an action: normals, forward/back normals, specials, supers, jump, dash/backdash, block, Final Memory, or a macro slot.");
    s_rows[n++] = Info("Set the button if the action needs one, add an optional delay, or turn on Random Pool to pick from several actions.");
    s_rows[n++] = Spacer();
    s_rows[n++] = Header("NOTES");
    s_rows[n++] = Info("This feature briefly enables P2 controls for specials, supers, dashes, and other input-buffer actions. Regular attacks and jumps use direct writes and keep AI control.");
    s_rows[n++] = Info("By default, wakeup actions try to use the move on the last wakeup frame, with character-specific wakeup handling and crossup support.");
    s_rows[n++] = Info("Actions are rate-limited to avoid spam; toggling a trigger clears it. After Airtech is separate from Auto-Airtech, so Auto-Airtech must be enabled first.");
    s_rows[n++] = Info("Tip: enable Pre-buffer Wakeup when testing wakeup macros or input crossups that need early buffering.");
    count = n;
    return s_rows;
}

Row* BuildHelpMacroRows(int& count) {
    static Row s_rows[44];
    int n = 0;
    s_rows[n++] = Header("MACROS");
    s_rows[n++] = Info("Macros record, play, and edit input sequences. Playback flips left/right automatically for Player 2.");
    s_rows[n++] = Info(g_helpMacroRecord);
    s_rows[n++] = Info(g_helpMacroPlay);
    s_rows[n++] = Info(g_helpMacroSlot);
    s_rows[n++] = Info("Record enters Pre-recording, where P1 controls drive P2. Press Record again to start recording, then press it a third time to save.");
    s_rows[n++] = Info("Play runs the current slot and also exits Pre-recording. Empty slots do nothing.");
    s_rows[n++] = Info("Playback flips directions for Player 2 automatically. Framestep tools work during playback.");
    s_rows[n++] = Spacer();
    s_rows[n++] = Header("CUSTOM MENU TOOLS");
    s_rows[n++] = Info("Serialized Macro opens the text editor, Apply To Slot, Reload From Slot, Clear Slot, clipboard actions, undo/redo, and sample insertion.");
    s_rows[n++] = Info("Slot Stats shows slot state, total ticks, effective ticks, first button tick, and buffer capture details.");
    s_rows[n++] = Info("The editor's Apply button saves the text into the current slot. Done closes the editor and keeps the draft text; Cancel reloads from the slot.");
    s_rows[n++] = Spacer();
    s_rows[n++] = Header("NOTATION");
    s_rows[n++] = Info("Write macros as plain text: a header plus tick tokens. Use numpad directions 1..9, with 5 or N as neutral, and A/B/C/D for buttons.");
    s_rows[n++] = Info("Examples include 5A, 6B, and 236C. Repeat packs like 5x3 insert neutral ticks, and per-tick buffers like {3: 6 6 6} write several inputs inside one tick.");
    s_rows[n++] = Info("Whitespace is flexible; Apply To Slot normalizes the text after it parses successfully.");
    s_rows[n++] = Info("Write notation as if Player 1 is facing right. Player 2 playback flips 4 and 6.");
    s_rows[n++] = Info("Example: EFZMACRO 1 5A 5x3 5B 5x3 5C 6 {3: 6 6 6} 2 {3: 2 2 2} 3 {3: 3 3 3} 5B");
    count = n;
    return s_rows;
}

Row* BuildHelpComboStatisticsRows(int& count) {
    static Row s_rows[40];
    int n = 0;
    s_rows[n++] = Header("COMBO STATISTICS");
    s_rows[n++] = Info("Combo Statistics is the compact combo summary overlay drawn at 640x480 game coordinates x=244, y=92 during Practice.");
    s_rows[n++] = Info("It appears only in supported local match states. It clears outside Practice, online sessions, character select, or when the overlay is disabled.");
    s_rows[n++] = Info("The combo starts from the game's combo counter and damage values. If those are unavailable, the mod can fall back to HP loss while the defender is in hitstun or untech.");
    s_rows[n++] = Spacer();
    s_rows[n++] = Header("WHAT IT SHOWS");
    s_rows[n++] = Info("Move is last hit damage. Combo is total combo damage. Max is the best combo damage seen this match session.");
    s_rows[n++] = Info("HP shows defender HP at combo start, current HP, and total HP lost.");
    s_rows[n++] = Info("P1 and P2 rows show meter and RF changes during the combo. Resource spends update immediately; passive gains are kept stable so the numbers stay useful.");
    s_rows[n++] = Info("Proration shows the current damage scaling. Detail Row can add defender untech and, when using Last Hit details, the attacker's current move ID.");
    s_rows[n++] = Info("Optional RFx and Raw fields show the RF multiplier and raw scale value for deeper combo testing.");
    s_rows[n++] = Spacer();
    s_rows[n++] = Header("OPTIONS");
    s_rows[n++] = Info("Enable it from Main > Options > Combo Statistics.");
    s_rows[n++] = Info("Show Detail Row adds extra proration and untech information. Detail source chooses between current combo state and last-hit data.");
    s_rows[n++] = Info("Keep Final Summary leaves the finished combo visible after the combo drops. Summary Time controls how long it lingers.");
    s_rows[n++] = Info("Hide With Menu removes Combo Statistics while this menu is open. Resume Summary lets the final summary return when the menu closes.");
    s_rows[n++] = Spacer();
    s_rows[n++] = Header("NOTES");
    s_rows[n++] = Info("Max resets when the match/session resets. Combo Statistics is for quick damage and resource checks; use Framebar when you need timing and state breakdowns.");
    count = n;
    return s_rows;
}

Row* BuildHelpFramebarRows(int& count) {
    static Row s_rows[56];
    int n = 0;
    s_rows[n++] = Header("FRAMEBAR");
    s_rows[n++] = Info("Framebar is a per-player timeline near the bottom-center of the screen. Each cell is one subframe by default.");
    s_rows[n++] = Info("Set Cell Step to Visual Frames if you prefer one cell per displayed game frame instead of subframe detail.");
    s_rows[n++] = Info("The right edge is the current frame. Older frames are on the left, so scan left-to-right to follow the sequence into the present.");
    s_rows[n++] = Info("It starts advancing when something important happens: attacks, stun, projectiles, lockout, Recoil Guard, or an overlapping block/RG check.");
    s_rows[n++] = Info("After roughly one second of calm, it freezes in place until the next action. This keeps the last useful sequence visible instead of scrolling through neutral forever.");
    s_rows[n++] = Spacer();
    s_rows[n++] = Header("COLORS");
    s_rows[n++] = Info("Green means neutral movement, walking, crouching, landing, or falling.");
    s_rows[n++] = Info("Yellow means prejump, jump, double jump, airtech, or ground tech.");
    s_rows[n++] = Info("Blue and cyan mean dashes, air dashes, and Recoil Guard windows.");
    s_rows[n++] = Info("Red means attack startup, active frames, or recovery. Grey means blockstun, hitstun, or launch.");
    s_rows[n++] = Info("Purple and pink cover throws and superflash. Blue-grey marks shared hitstop.");
    s_rows[n++] = Spacer();
    s_rows[n++] = Header("MARKERS");
    s_rows[n++] = Info("A white vertical line marks the first active frame detected in the current attack sequence.");
    s_rows[n++] = Info("Orange top ticks mark live projectile slots. A second orange tick means the projectile's current frame has attack boxes.");
    s_rows[n++] = Info("Bright red lower strips mean collision-active character boxes. Dark red means attack data without active collision yet.");
    s_rows[n++] = Info("Muted brown strips mark the engine's post-hit attack timer; they are not active frames by themselves.");
    s_rows[n++] = Info("Blue and cyan small strips mean that side can block or Recoil Guard the overlapping character or projectile attack.");
    s_rows[n++] = Info("A dark blue band means shared hitstop. Yellow/magenta flashes call out untech, hit, block/RG, throw, and counter-hit moments.");
    s_rows[n++] = Info("Purple middle marks track air-mobility counters, useful when checking double jumps and air dashes.");
    s_rows[n++] = Info("Detail controls how much of this appears: Full shows every marker and status line, Compact keeps the main timing data, and Bars Only hides text and extra marker strips.");
    s_rows[n++] = Spacer();
    s_rows[n++] = Header("STATUS LINES");
    s_rows[n++] = Info("ID/F is move ID and current animation frame. BX is active/raw character boxes. PB is projectile attack boxes. P is live projectile slots.");
    s_rows[n++] = Info("ST is the engine state timer. UT is untech or stun duration. SF is superflash freeze. AM is air-mobility counters.");
    s_rows[n++] = Info("FA is first active frame. ACT is active or projectile frames. TOT is total engine-busy frames.");
    s_rows[n++] = Info("ATK is the post-hit attacker countdown. FL and CL are frame and collision lockouts. GG is guard gauge.");
    s_rows[n++] = Info("B and RG tell whether the side can block or Recoil Guard the opponent's overlapping attack. G is the current guard flag.");
    s_rows[n++] = Info("HS is the hit-state flag. CH is counter-hit. HST means shared hitstop.");
    s_rows[n++] = Spacer();
    s_rows[n++] = Header("FRAMESTEP");
    s_rows[n++] = Info("When the game is paused or framestepping, Framebar advances only when the game actually steps. That makes it useful for reviewing one frame at a time.");
    s_rows[n++] = Info("Enable it from Main > Options with the Frame Bar category.");
    count = n;
    return s_rows;
}

Row* BuildHelpBoxDisplayRows(int& count) {
    static Row s_rows[40];
    int n = 0;
    s_rows[n++] = Header("BOX DISPLAY");
    s_rows[n++] = Info("Box Display draws simple colored shapes over the match so you can see what the game is checking.");
    s_rows[n++] = Info("Enable it from Main > Options > Display Overlays. It is meant for Practice match screens.");
    s_rows[n++] = Spacer();
    s_rows[n++] = Header("CHARACTER BOXES");
    s_rows[n++] = Info("Red boxes are hitboxes. If they touch the opponent's hurtbox, the move can hit.");
    s_rows[n++] = Info("Green boxes are hurtboxes. This is where that character can be hit.");
    s_rows[n++] = Info("Yellow boxes are collision boxes, also called pushboxes. They show the body space characters use for pushing and spacing.");
    s_rows[n++] = Spacer();
    s_rows[n++] = Header("PROJECTILE BOXES");
    s_rows[n++] = Info("Blue boxes on bullets/projectiles show the projectile collision area the engine is checking.");
    s_rows[n++] = Info("Bright blue means the projectile is active for projectile interaction. Faint blue means it exists, but is not active for that check right now.");
    s_rows[n++] = Info("The blue box is not always the full sprite. Some projectiles are larger or smaller than the picture on screen.");
    s_rows[n++] = Spacer();
    s_rows[n++] = Header("PROJECTILE HELPERS");
    s_rows[n++] = Info("White dots mark projectile origin points. Think of them as the projectile's anchor, not the center of its blue box.");
    s_rows[n++] = Info("Magenta boxes show where two active projectile boxes overlap. Use this to check clashes and projectile interactions.");
    s_rows[n++] = Info("If a projectile returns or changes state, its visible sprite may keep moving even when its blue interaction box is gone.");
    s_rows[n++] = Spacer();
    s_rows[n++] = Header("READING IT");
    s_rows[n++] = Info("Boxes are engine data, not artwork. Trust the boxes when they disagree with the sprite.");
    s_rows[n++] = Info("Use Box Fill Alpha to make filled areas lighter or darker. The outlines stay strong so the box edges remain readable.");
    s_rows[n++] = Info("Turn layers on one at a time if the screen gets noisy: Hitboxes, Hurtboxes, Collision Boxes, then Projectile Interactions.");
    count = n;
    return s_rows;
}

Row* BuildHelpSavestatesRows(int& count) {
    static Row s_rows[28];
    int n = 0;
    s_rows[n++] = Header("PRACTICE SNAPSHOTS");
    s_rows[n++] = Info("Practice save/load runs through EfzRevival. Open Main > Options > Savestates for menu actions and palette behavior.");
    s_rows[n++] = Info("Use snapshots for retry loops, setup lab work, and preserving the full Practice match between attempts.");
    s_rows[n++] = Info("Position save/load hotkeys only move players. Snapshots capture the whole match state through Revival.");
    s_rows[n++] = Spacer();
    s_rows[n++] = Header("MENU ACTIONS");
    s_rows[n++] = Info("Save Practice State captures the live match. Load Practice State restores the last save.");
    s_rows[n++] = Info("The status counter on the Savestates row shows how many saves and loads have run this session.");
    s_rows[n++] = Spacer();
    s_rows[n++] = Header("OPTIONS");
    s_rows[n++] = Info("Load Custom Palettes controls whether loads restore saved custom .pal colors.");
    s_rows[n++] = Info("When it is off, loads keep the saved palette number but use the game's default palette instead.");
    s_rows[n++] = Info("If a load gives you broken colors or missing effects, turn Load Custom Palettes off and load again.");
    s_rows[n++] = Spacer();
    s_rows[n++] = Header("HOTKEYS");
    s_rows[n++] = Info(g_helpSavestateSave);
    s_rows[n++] = Info(g_helpSavestateLoad);
    s_rows[n++] = Info("Slot Previous and Slot Next change which slot the save/load hotkeys target. Bind them under Settings > Hotkeys > Savestate.");
    s_rows[n++] = Info("Hotkeys write to the active slot immediately. The menu buttons always target the live match.");
    s_rows[n++] = Spacer();
    s_rows[n++] = Header("NOTES");
    s_rows[n++] = Info("EfzRevival must be present for snapshots to work. Unsupported Revival builds may disable or break save/load.");
    s_rows[n++] = Info("For spacing-only drills, position save/load is usually faster than a full snapshot.");
    count = n;
    return s_rows;
}

Row* BuildHelpIssuesRows(int& count) {
    static Row s_rows[36];
    int n = 0;
    s_rows[n++] = Header("CONFLICTS");
    s_rows[n++] = Info("Some features auto-disable others to avoid clashes. Random Block, Random RG, and Always RG are mutually exclusive, so turning one on can turn another off.");
    s_rows[n++] = Info("Counter RG cannot work while Always RG is on.");
    s_rows[n++] = Info("While the menu is open, practice hotkeys are disabled and the game auto-pauses. Closing the menu starts a brief hotkey cooldown.");
    s_rows[n++] = Spacer();
    s_rows[n++] = Header("TROUBLESHOOTING");
    s_rows[n++] = Info("If values look wrong, press F4/F5 until the game returns to Normal mode, then re-apply your values.");
    s_rows[n++] = Info("Continuous Recovery can be limited to neutral in Settings. If something seems off, go back to the main menu and return to Practice.");
    s_rows[n++] = Info(g_helpOpenHelp);
    s_rows[n++] = Spacer();
    s_rows[n++] = Header("UNSUPPORTED REVIVAL");
    s_rows[n++] = Info("If your EfzRevival build is not listed as supported, some tools may be disabled and newer builds can introduce unexpected issues.");
    s_rows[n++] = Info("Avoid unsupported versions for netplay. If problems occur, launching the game directly through efz.exe can be a fallback.");
    s_rows[n++] = Info("Hotkeys may still be recognized by unsupported builds while this menu is open, but the game should remain paused.");
    count = n;
    return s_rows;
}

Row* BuildHelpGuideRows(int& count) {
    static Row s_rows[20];
    int n = 0;
    s_rows[n++] = Header("GUIDE MENUS");
    s_rows[n++] = Info("Open these sections for feature behavior, caveats, setup notes, and troubleshooting.");
    s_rows[n++] = Submenu("BASICS",               "BASICS",               BuildHelpBasicsRows,     nullptr);
    s_rows[n++] = Submenu("PRACTICE SNAPSHOTS",   "PRACTICE SNAPSHOTS",   BuildHelpSavestatesRows, nullptr);
    s_rows[n++] = Submenu("COMBO STATISTICS",   "COMBO STATISTICS",   BuildHelpComboStatisticsRows, nullptr);
    s_rows[n++] = Submenu("FRAMEBAR",           "FRAMEBAR",           BuildHelpFramebarRows,   nullptr);
    s_rows[n++] = Submenu("BOX DISPLAY",        "BOX DISPLAY",        BuildHelpBoxDisplayRows, nullptr);
    s_rows[n++] = Submenu("RECOVERY",           "RECOVERY",           BuildHelpRecoveryRows,   nullptr);
    s_rows[n++] = Submenu("CHARACTER SETTINGS", "CHARACTER SETTINGS", BuildHelpCharacterRows,  nullptr);
    s_rows[n++] = Submenu("AUTO ACTIONS",       "AUTO ACTIONS",       BuildHelpAutoActionRows, nullptr);
    s_rows[n++] = Submenu("MACROS",             "MACROS",             BuildHelpMacroRows,      nullptr);
    s_rows[n++] = Submenu("ISSUES",             "ISSUES",             BuildHelpIssuesRows,     nullptr);
    count = n;
    return s_rows;
}

Row* BuildHelpResourcesRows(int& count) {
    static Row s_rows[18];
    int n = 0;
    s_rows[n++] = Header("GAME RESOURCES");
    s_rows[n++] = Info("Open helpful external resources in your browser.");
    s_rows[n++] = Action("EFZ WIKI",           OpenEternalFighterZeroWiki, ValWiki);
    s_rows[n++] = Action("TRAINING MODE WIKI", OpenTrainingModeWiki,       ValWiki);
    s_rows[n++] = Action("EFZ GLOBAL DISCORD", OpenEfzDiscord,             ValWiki);
    s_rows[n++] = Spacer();
    s_rows[n++] = Header("CURRENT CHARACTERS");
    s_rows[n++] = Info("Character wiki links appear when the current matchup can be identified.");
    s_rows[n++] = Action(g_helpP1WikiLabel[0] ? g_helpP1WikiLabel : "OPEN P1 WIKI", OpenP1Wiki, ValWiki, P1WikiDisabled);
    s_rows[n++] = Action(g_helpP2WikiLabel[0] ? g_helpP2WikiLabel : "OPEN P2 WIKI", OpenP2Wiki, ValWiki, P2WikiDisabled);
    count = n;
    return s_rows;
}

Row* BuildHelpAboutRows(int& count) {
    static Row s_rows[48];
    int n = 0;
    s_rows[n++] = Header("EFZ TRAINING MODE");
    s_rows[n++] = Info(g_helpVersionStr);
    s_rows[n++] = Info(g_helpBuildStr);
    s_rows[n++] = Info("A training toolkit for Eternal Fighter Zero - frame data, drills, macros, and matchup tools in one place.");
    s_rows[n++] = Info("Find the newest version and release notes on GitHub.");
    s_rows[n++] = Action("OPEN GITHUB RELEASES", OpenGithubReleases, ValWiki);
    s_rows[n++] = Spacer();
    s_rows[n++] = Header("YOUR GAME");
    s_rows[n++] = Info(g_helpDetectedVersion);
    s_rows[n++] = Info("Works with vanilla EFZ and supported EfzRevival builds (1.02e through the verified 1.02j MinGW build).");
    s_rows[n++] = Info("Some features may be limited on unsupported or very new Revival versions.");
    s_rows[n++] = Spacer();
    s_rows[n++] = Header("WHAT YOU GET");
    s_rows[n++] = Info("Frame advantage display, Combo Statistics, Framebar, practice snapshots, and continuous recovery.");
    s_rows[n++] = Info("Dummy auto-actions, input macros, character-specific settings, and match hotswap without leaving Practice.");
    s_rows[n++] = Info("Everything is configurable from this menu - no editing files by hand.");
    s_rows[n++] = Spacer();
    s_rows[n++] = Header("CONTROLS");
    s_rows[n++] = Info("Play with keyboard and mouse, or use a gamepad - extensive controller support is built in.");
    s_rows[n++] = Info("Xbox, PlayStation, and most common gamepads use the same button bindings.");
    s_rows[n++] = Info("Open, navigate, and close the menu from your pad. Rebind anything under Settings > Hotkeys.");
    s_rows[n++] = Spacer();
    s_rows[n++] = Header("OBLIGATORY MICHIRU");
    s_rows[n++] = Custom(216.0f, DrawMichiruInline);
    count = n;
    return s_rows;
}

// ===== AUTO / TRIGGERS =====
void AddTriggerRows(Row* rows, int& n,
                    const char* title,
                    bool* enabled,
                    int* motionIdx,
                    void (*onMotion)(),
                    int* action,
                    int* strength,
                    int* macroSlot,
                    int* delay,
                    bool* usePool,
                    unsigned int* poolMask,
                    void (*onUsePool)(),
                    void (*onPoolMask)(),
                    bool (*hideSingleAction)(),
                    bool (*hideButton)(),
                    bool (*hidePool)()) {
    rows[n++] = Header(title);
    rows[n++] = Toggle        ("  ENABLE",      enabled, OnAutoApply);
    rows[n++] = Toggle        ("  RANDOM POOL", usePool, onUsePool);
    rows[n++] = DropdownRow   ("  ACTION",      motionIdx, kTriggerMotionChoices, kTriggerMotionCount,
                               onMotion, nullptr, hideSingleAction);
    rows[n++] = TriggerButtonRow("  BUTTON",    action, strength, &g_mirrorFwdDashFollowup,
                                 OnAutoApply, nullptr, hideButton);
    rows[n++] = MaskPickerRow ("  ACTION POOL", poolMask, kActionPoolNames, kActionPoolCount,
                               onPoolMask, nullptr, hidePool);
    rows[n++] = IntNum        ("  DELAY",       delay, 0, 60, 1, 5, OnAutoApply);
    rows[n++] = DropdownRow   ("  MACRO",       macroSlot, g_macroSlotChoiceArr, g_macroSlotChoiceCount, OnAutoApply);
}

const char* FormatTriggerSummary(bool enabled, bool usePool, int delay) {
    static char s_buf[5][32];
    static int s_idx = 0;
    char* buf = s_buf[s_idx++ % 5];
    if (!enabled) {
        _snprintf_s(buf, sizeof(s_buf[0]), _TRUNCATE, "OFF");
    } else if (usePool) {
        _snprintf_s(buf, sizeof(s_buf[0]), _TRUNCATE, "POOL / %dF", delay);
    } else {
        _snprintf_s(buf, sizeof(s_buf[0]), _TRUNCATE, "ON / %dF", delay);
    }
    return buf;
}

const char* ValTriggerAB() { const auto& d = ImGuiGui::guiState.localData; return FormatTriggerSummary(d.triggerAfterBlock,    g_useMaskAB, d.delayAfterBlock); }
const char* ValTriggerWU() { const auto& d = ImGuiGui::guiState.localData; return FormatTriggerSummary(d.triggerOnWakeup,       g_useMaskWU, d.delayOnWakeup); }
const char* ValTriggerAH() { const auto& d = ImGuiGui::guiState.localData; return FormatTriggerSummary(d.triggerAfterHitstun,   g_useMaskAH, d.delayAfterHitstun); }
const char* ValTriggerAA() { const auto& d = ImGuiGui::guiState.localData; return FormatTriggerSummary(d.triggerAfterAirtech,   g_useMaskAA, d.delayAfterAirtech); }
const char* ValTriggerRG() { const auto& d = ImGuiGui::guiState.localData; return FormatTriggerSummary(d.triggerOnRG,           g_useMaskRG, d.delayOnRG); }

Row* BuildAfterBlockRows(int& count) {
    static Row s_rows[20];
    int n = 0;
    auto& d = ImGuiGui::guiState.localData;
    g_motionIdxAB = GetMotionIndexForAction(d.actionAfterBlock);
    AddTriggerRows(s_rows, n, "AFTER BLOCK",
                   &d.triggerAfterBlock, &g_motionIdxAB, OnTriggerMotionAfterBlock,
                   &d.actionAfterBlock, &d.strengthAfterBlock,
                   &d.macroSlotAfterBlock, &d.delayAfterBlock,
                   &g_useMaskAB, &g_poolMaskAB, OnUseMaskAB, OnPoolMaskAB,
                   HideABSingleAction, HideABButton, HideABPool);
    count = n;
    return s_rows;
}

Row* BuildWakeupRows(int& count) {
    static Row s_rows[20];
    int n = 0;
    auto& d = ImGuiGui::guiState.localData;
    g_motionIdxWU = GetMotionIndexForAction(d.actionOnWakeup);
    AddTriggerRows(s_rows, n, "ON WAKEUP",
                   &d.triggerOnWakeup, &g_motionIdxWU, OnTriggerMotionOnWakeup,
                   &d.actionOnWakeup, &d.strengthOnWakeup,
                   &d.macroSlotOnWakeup, &d.delayOnWakeup,
                   &g_useMaskWU, &g_poolMaskWU, OnUseMaskWU, OnPoolMaskWU,
                   HideWUSingleAction, HideWUButton, HideWUPool);
    count = n;
    return s_rows;
}

Row* BuildHitstunRows(int& count) {
    static Row s_rows[20];
    int n = 0;
    auto& d = ImGuiGui::guiState.localData;
    g_motionIdxAH = GetMotionIndexForAction(d.actionAfterHitstun);
    AddTriggerRows(s_rows, n, "AFTER HITSTUN",
                   &d.triggerAfterHitstun, &g_motionIdxAH, OnTriggerMotionAfterHitstun,
                   &d.actionAfterHitstun, &d.strengthAfterHitstun,
                   &d.macroSlotAfterHitstun, &d.delayAfterHitstun,
                   &g_useMaskAH, &g_poolMaskAH, OnUseMaskAH, OnPoolMaskAH,
                   HideAHSingleAction, HideAHButton, HideAHPool);
    count = n;
    return s_rows;
}

Row* BuildAirtechRows(int& count) {
    static Row s_rows[20];
    int n = 0;
    auto& d = ImGuiGui::guiState.localData;
    g_motionIdxAA = GetMotionIndexForAction(d.actionAfterAirtech);
    AddTriggerRows(s_rows, n, "AFTER AIRTECH",
                   &d.triggerAfterAirtech, &g_motionIdxAA, OnTriggerMotionAfterAirtech,
                   &d.actionAfterAirtech, &d.strengthAfterAirtech,
                   &d.macroSlotAfterAirtech, &d.delayAfterAirtech,
                   &g_useMaskAA, &g_poolMaskAA, OnUseMaskAA, OnPoolMaskAA,
                   HideAASingleAction, HideAAButton, HideAAPool);
    count = n;
    return s_rows;
}

Row* BuildRecoilGuardRows(int& count) {
    static Row s_rows[20];
    int n = 0;
    auto& d = ImGuiGui::guiState.localData;
    g_motionIdxRG = GetMotionIndexForAction(d.actionOnRG);
    AddTriggerRows(s_rows, n, "ON RECOIL GUARD",
                   &d.triggerOnRG, &g_motionIdxRG, OnTriggerMotionOnRG,
                   &d.actionOnRG, &d.strengthOnRG,
                   &d.macroSlotOnRG, &d.delayOnRG,
                   &g_useMaskRG, &g_poolMaskRG, OnUseMaskRG, OnPoolMaskRG,
                   HideRGSingleAction, HideRGButton, HideRGPool);
    count = n;
    return s_rows;
}

Row* BuildTriggersRows(int& count) {
    static Row s_rows[32];
    int n = 0;

    s_rows[n++] = Header("GLOBAL");
    s_rows[n++] = Toggle    ("ENABLE AUTO ACTION",    &g_mirrorAutoAction, OnAutoActionToggle);
    s_rows[n++] = ChoicesRow("TARGET",                &g_mirrorAutoActionPlayerIdx, kTargetChoices, 3, OnAutoActionTarget);
    s_rows[n++] = Toggle    ("RANDOMIZE TRIGGERS",    &g_mirrorRandomize,  OnRandomizeToggle);
    s_rows[n++] = Toggle    ("PRE-BUFFER WAKEUP",     &g_mirrorWakeBuffer, OnWakeBufferToggle);

    s_rows[n++] = Spacer();
    s_rows[n++] = Header("TRIGGER MENUS");
    s_rows[n++] = Submenu("AFTER BLOCK",       "AFTER BLOCK",       BuildAfterBlockRows,  ValTriggerAB);
    s_rows[n++] = Submenu("ON WAKEUP",         "ON WAKEUP",         BuildWakeupRows,      ValTriggerWU);
    s_rows[n++] = Submenu("AFTER HITSTUN",     "AFTER HITSTUN",     BuildHitstunRows,     ValTriggerAH);
    s_rows[n++] = Submenu("AFTER AIRTECH",     "AFTER AIRTECH",     BuildAirtechRows,     ValTriggerAA);
    s_rows[n++] = Submenu("ON RECOIL GUARD",   "ON RECOIL GUARD",   BuildRecoilGuardRows, ValTriggerRG);

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

std::string CharacterDisplayName(int charId, const char* rawName) {
    std::string name = CharacterSettings::GetCharacterName(charId);
    if (name.empty() || name == "Undefined") {
        name = (rawName && rawName[0]) ? rawName : "(NONE)";
    }
    return name;
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

void AddMizukaNoteDisplayRows(Row* rows, int& n) {
    if (!HasMizuka()) {
        return;
    }

    rows[n++] = Header("MIZUKA NOTES DISPLAY");
    auto& s = MutableSettings();
    rows[n++] = Toggle("NOTE TRIGGER RANGES", &s.collisionDisplayNagamoriRanges, OnCollisionNagamoriRanges);
    rows[n++] = Toggle("AFFECTED NOTES", &s.collisionDisplayNagamoriAffected, OnCollisionNagamoriAffected);
    rows[n++] = Info("Uses Display Overlays > Projectile Interactions as the master switch.");
    rows[n++] = Spacer();
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

bool AddPlayerCharacterRows(Row* rows, int& n, DisplayData& d, int player, int charId, const char* rawName) {
    if (!CharHasCustomRows(charId)) return false;

    static char s_headers[2][64];
    char* header = s_headers[(player == 2) ? 1 : 0];
    const std::string name = CharacterDisplayName(charId, rawName);
    _snprintf_s(header, sizeof(s_headers[0]), _TRUNCATE, "P%d  %s", player, name.c_str());
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
        default: break;
    }
    return true;
}

const char* ValP1Character() {
    static char s_buf[64];
    const auto& d = ImGuiGui::guiState.localData;
    const std::string name = CharacterDisplayName(d.p1CharID, d.p1CharName);
    _snprintf_s(s_buf, sizeof(s_buf), _TRUNCATE, "%s", name.c_str());
    return s_buf;
}

const char* ValP2Character() {
    static char s_buf[64];
    const auto& d = ImGuiGui::guiState.localData;
    const std::string name = CharacterDisplayName(d.p2CharID, d.p2CharName);
    _snprintf_s(s_buf, sizeof(s_buf), _TRUNCATE, "%s", name.c_str());
    return s_buf;
}

Row* BuildP1CharacterRows(int& count) {
    static Row s_rows[64];
    int n = 0;
    auto& d = ImGuiGui::guiState.localData;
    if (!AddPlayerCharacterRows(s_rows, n, d, 1, d.p1CharID, d.p1CharName)) {
        s_rows[n++] = Header("STATUS");
        s_rows[n++] = Info("No custom controls for Player 1.");
    }
    count = n;
    return s_rows;
}

Row* BuildP2CharacterRows(int& count) {
    static Row s_rows[64];
    int n = 0;
    auto& d = ImGuiGui::guiState.localData;
    if (!AddPlayerCharacterRows(s_rows, n, d, 2, d.p2CharID, d.p2CharName)) {
        s_rows[n++] = Header("STATUS");
        s_rows[n++] = Info("No custom controls for Player 2.");
    }
    count = n;
    return s_rows;
}

Row* BuildCharsRows(int& count) {
    static Row s_rows[32];
    int n = 0;
    auto& d = ImGuiGui::guiState.localData;

    s_rows[n++] = Header("CHARACTER CONTROLS");
    s_rows[n++] = Action("REFRESH CHARACTER DATA", RefreshCharacterDataAction);

    s_rows[n++] = Spacer();
    AddCharacterLockRows(s_rows, n);
    AddMizukaNoteDisplayRows(s_rows, n);

    const bool p1HasRows = CharHasCustomRows(d.p1CharID);
    const bool p2HasRows = CharHasCustomRows(d.p2CharID);
    if (p1HasRows || p2HasRows) {
        s_rows[n++] = Header("PLAYER MENUS");
        if (p1HasRows) {
            s_rows[n++] = Submenu("PLAYER 1", "PLAYER 1 CHARACTER", BuildP1CharacterRows, ValP1Character);
        }
        if (p2HasRows) {
            s_rows[n++] = Submenu("PLAYER 2", "PLAYER 2 CHARACTER", BuildP2CharacterRows, ValP2Character);
        }
    }

    if (!p1HasRows && !p2HasRows && !HasMizuka()) {
        s_rows[n++] = Header("STATUS");
        s_rows[n++] = Info("No supported character controls in this matchup.");
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

// Most recovery/movement settings bind directly to DisplayData. Auto-Airtech uses
// a small mirror because the custom menu exposes disabled/forward/back as one row.

const char* ValDefenseSummary() {
    if (g_mirrorRandomBlock) return "RANDOM BLOCK";
    if (g_mirrorAlwaysRG) return "ALWAYS RG";
    if (g_mirrorRandomRG) return "RANDOM RG";
    if (g_mirrorDummyBlockMode != 0) return kDummyBlockChoices[g_mirrorDummyBlockMode];
    return "OFF";
}

const char* ValRecoverySummary() {
    const auto& d = ImGuiGui::guiState.localData;
    if (!d.autoAirtech) return "OFF";
    return ClampIndex(d.airtechDirection, 2) == 0 ? "FORWARD" : "BACK";
}

void OnAirtechMode() {
    auto& d = ImGuiGui::guiState.localData;
    const int mode = ClampIndex(g_mirrorAirtechMode, 3);
    d.autoAirtech = mode != 0;
    d.airtechDirection = (mode > 0) ? (mode - 1) : 0;
    OnAutoApply();
}

void OnAutoJumpTarget() {
    static constexpr int kTargetMap[3] = {1, 2, 3};
    const int idx = ClampIndex(g_mirrorAutoJumpTargetIdx, 3);
    g_mirrorAutoJumpTargetIdx = idx;
    ImGuiGui::guiState.localData.jumpTarget = kTargetMap[idx];
    OnAutoApply();
}

const char* ValMovementSummary() {
    const auto& d = ImGuiGui::guiState.localData;
    return d.autoJump ? "AUTO-JUMP" : "OFF";
}

Row* BuildOpponentDefenseRows(int& count) {
    static Row s_rows[16];
    int n = 0;

    s_rows[n++] = ChoicesRow("DUMMY AUTO-BLOCK",       &g_mirrorDummyBlockMode,    kDummyBlockChoices, 4, OnDummyBlockMode);
    s_rows[n++] = Toggle    ("RANDOM BLOCK",           &g_mirrorRandomBlock,       OnRandomBlock);
    s_rows[n++] = Toggle    ("ADAPTIVE STANCE",        &g_mirrorAdaptiveStance,    OnAdaptiveStance);
    s_rows[n++] = ChoicesRow("DUMMY STANCE",           &g_mirrorPracticeStance,    kDummyStanceChoices, 3, OnPracticeStance, AdaptiveHidesStance);
    s_rows[n++] = Toggle    ("ALWAYS RECOIL GUARD",    &g_mirrorAlwaysRG,          OnAlwaysRG);
    s_rows[n++] = Toggle    ("RANDOM RECOIL GUARD",    &g_mirrorRandomRG,          OnRandomRG);
    s_rows[n++] = Toggle    ("COUNTER RG",             &g_mirrorCounterRG,         OnCounterRGToggle);
    count = n;
    return s_rows;
}

Row* BuildOpponentRecoveryRows(int& count) {
    static Row s_rows[8];
    int n = 0;
    auto& d = ImGuiGui::guiState.localData;
    g_mirrorAirtechMode = d.autoAirtech ? (ClampIndex(d.airtechDirection, 2) + 1) : 0;

    s_rows[n++] = ChoicesRow("AUTO-AIRTECH",           &g_mirrorAirtechMode, kAirtechDirChoices, 3, OnAirtechMode);
    s_rows[n++] = IntNum   ("  AIRTECH DELAY",         &d.airtechDelay,     0, 60, 1, 5, OnAutoApply);
    count = n;
    return s_rows;
}

Row* BuildOpponentMovementRows(int& count) {
    static Row s_rows[8];
    int n = 0;
    auto& d = ImGuiGui::guiState.localData;

    s_rows[n++] = Toggle   ("AUTO-JUMP",               &d.autoJump,        OnAutoApply);
    s_rows[n++] = ChoicesRow("  JUMP DIRECTION",       &d.jumpDirection,   kJumpDirChoices, 3, OnAutoApply);
    s_rows[n++] = ChoicesRow("  JUMP TARGET",          &g_mirrorAutoJumpTargetIdx, kJumpTargetChoices, 3, OnAutoJumpTarget);
    count = n;
    return s_rows;
}

bool MovementJumpDirHidden() {
    return !ImGuiGui::guiState.localData.autoJump;
}
bool MovementJumpTargetHidden() {
    return !ImGuiGui::guiState.localData.autoJump;
}
bool AirtechDelayHidden() {
    return g_mirrorAirtechMode == 0;
}

Row* BuildOpponentRows(int& count) {
    static Row s_rows[40];
    int n = 0;
    auto& d = ImGuiGui::guiState.localData;
    g_mirrorAirtechMode = d.autoAirtech ? (ClampIndex(d.airtechDirection, 2) + 1) : 0;

    s_rows[n++] = Header("OPPONENT");
    s_rows[n++] = Toggle("ENABLE P2 CONTROL", &d.p2ControlEnabled, OnAutoApply);
    s_rows[n++] = ChoicesRow("AUTO-AIRTECH", &g_mirrorAirtechMode, kAirtechDirChoices, 3, OnAirtechMode);
    s_rows[n++] = IntNum("  AIRTECH DELAY", &d.airtechDelay, 0, 60, 1, 5, OnAutoApply,
                         nullptr, AirtechDelayHidden);
    s_rows[n++] = Toggle("AUTO-JUMP", &d.autoJump, OnAutoApply);
    s_rows[n++] = ChoicesRow("  JUMP DIRECTION", &d.jumpDirection, kJumpDirChoices, 3, OnAutoApply,
                             nullptr, MovementJumpDirHidden);
    s_rows[n++] = ChoicesRow("  JUMP TARGET", &g_mirrorAutoJumpTargetIdx, kJumpTargetChoices, 3, OnAutoJumpTarget,
                             nullptr, MovementJumpTargetHidden);
    s_rows[n++] = Spacer();
    s_rows[n++] = Header("DEFENSE");
    {
        int subN = 0;
        Row* defense = BuildOpponentDefenseRows(subN);
        for (int i = 0; i < subN; ++i) {
            s_rows[n++] = defense[i];
        }
    }

    count = n;
    return s_rows;
}

// ===== MENU screen =====
void RunExitToCharacterSelect() {
    RequestFrontendExit(FrontendExitTarget::CharacterSelect);
}

void RunExitToTitle() {
    RequestFrontendExit(FrontendExitTarget::Title);
}

const char* const kHotswapCharacterChoices[] = {
    "Rumi", "Ayu", "Mai", "Makoto", "Akane", "Mayu",
    "Mizuka", "Misaki", "Shiori", "Sayuri", "Neyuki", "Mio",
    "Doppel", "Kaori", "Ikumi", "Mishio", "Akiko", "Nayuki",
    "Unknown", "Kanna", "Kano", "Minagi", "Misuzu",
};

const int kHotswapCharacterSelectIds[] = {
    0, 1, 2, 3, 4, 5,
    6, 7, 8, 9, 10, 11,
    12, 13, 14, 15, 16, 17,
    18, 19, 20, 21, 23,
};

constexpr int kHotswapCharacterChoiceCount = static_cast<int>(sizeof(kHotswapCharacterChoices) / sizeof(kHotswapCharacterChoices[0]));

constexpr uintptr_t kHotswapScreenTableRva = 0x00390110;
constexpr uint8_t kHotswapCharacterSelectScreen = 1;
constexpr uintptr_t kHotswapCsP1SelectionOffset = 1340;
constexpr uintptr_t kHotswapCsP2SelectionOffset = 1341;

const char* const kHotswapPaletteChoices[] = {
    "1", "2", "3", "4", "5", "6",
};

constexpr int kHotswapPaletteChoiceCount = static_cast<int>(sizeof(kHotswapPaletteChoices) / sizeof(kHotswapPaletteChoices[0]));

int g_hotswapMenuP1Character = 4;
int g_hotswapMenuP2Character = 16;
int g_hotswapMenuP1Palette = 0;
int g_hotswapMenuP2Palette = 0;
int g_hotswapMenuP1PalettePrev = 0;
int g_hotswapMenuP2PalettePrev = 0;
bool g_hotswapMenuP1CustomPalette = false;
bool g_hotswapMenuP2CustomPalette = false;
int g_hotswapMenuStage = 0;
int g_hotswapMenuOstChoice = 0;
bool g_hotswapMenuSeeded = false;

struct HotswapCustomAvailabilityLogState {
    int selectId = -1;
    int palette = -1;
    int available = -1;
    int disabled = -1;
};

HotswapCustomAvailabilityLogState g_hotswapP1CustomAvailabilityLog;
HotswapCustomAvailabilityLogState g_hotswapP2CustomAvailabilityLog;

struct HotswapCurrentState {
    bool charsValid = false;
    int p1SelectId = 4;
    int p2SelectId = 16;
    bool paletteValid = false;
    CharacterHotswap::PaletteSelection paletteSelection;
    bool stageValid = false;
    int stageId = 0;
    bool bgmValid = false;
    int bgmTrack = 0;
};

int CharacterSelectIdForHotswapChoice(int choiceIdx);

bool HotswapCurrentStatesEqual(const HotswapCurrentState& lhs, const HotswapCurrentState& rhs) {
    return lhs.charsValid == rhs.charsValid
        && lhs.p1SelectId == rhs.p1SelectId
        && lhs.p2SelectId == rhs.p2SelectId
        && lhs.paletteValid == rhs.paletteValid
        && lhs.paletteSelection.p1Color == rhs.paletteSelection.p1Color
        && lhs.paletteSelection.p2Color == rhs.paletteSelection.p2Color
        && lhs.paletteSelection.p1UseCustomPalette == rhs.paletteSelection.p1UseCustomPalette
        && lhs.paletteSelection.p2UseCustomPalette == rhs.paletteSelection.p2UseCustomPalette
        && lhs.stageValid == rhs.stageValid
        && lhs.stageId == rhs.stageId
        && lhs.bgmValid == rhs.bgmValid
        && lhs.bgmTrack == rhs.bgmTrack;
}

void LogSavestateHotswapPromptStateIfChanged(const char* reason,
                                             bool promptVisible,
                                             const CustomSavestate::Summary* summary,
                                             const HotswapCurrentState* current,
                                             int savedP1SelectId,
                                             int savedP2SelectId) {
    static std::string s_lastLogLine;

    std::ostringstream oss;
    oss << "[SAVESTATE][MENU][HOTSWAP]"
        << " reason=" << (reason ? reason : "unknown")
        << " prompt=" << (promptVisible ? 1 : 0);

    if (summary) {
        oss << " rawChars=" << static_cast<unsigned int>(summary->savedP1CharId)
            << "/" << static_cast<unsigned int>(summary->savedP2CharId)
            << " selectChars=" << savedP1SelectId
            << "/" << savedP2SelectId
            << " savedStage=" << static_cast<unsigned int>(summary->savedStageId)
            << " compat=" << (summary->currentPairCompatible ? 1 : 0)
            << "/" << (summary->currentStageCompatible ? 1 : 0)
            << "/" << (summary->currentVersionCompatible ? 1 : 0);
    }

    if (current) {
        oss << " currentCharsValid=" << (current->charsValid ? 1 : 0)
            << " currentChars=" << current->p1SelectId
            << "/" << current->p2SelectId
            << " currentStageValid=" << (current->stageValid ? 1 : 0)
            << " currentStage=" << current->stageId
            << " currentBgmValid=" << (current->bgmValid ? 1 : 0)
            << " currentBgm=" << current->bgmTrack;
    }

    const std::string line = oss.str();
    if (line != s_lastLogLine) {
        LogOut(line, true);
        s_lastLogLine = line;
    }
}

void LogHotswapMenuSelection(const char* reason) {
    std::ostringstream oss;
    oss << "[HOTSWAP][MENU] " << (reason ? reason : "state")
        << " p1Char=" << CharacterSelectIdForHotswapChoice(g_hotswapMenuP1Character)
        << " p2Char=" << CharacterSelectIdForHotswapChoice(g_hotswapMenuP2Character)
        << " palette=" << (g_hotswapMenuP1Palette + 1)
        << "/" << (g_hotswapMenuP2Palette + 1)
        << " custom=" << (g_hotswapMenuP1CustomPalette ? 1 : 0)
        << "/" << (g_hotswapMenuP2CustomPalette ? 1 : 0)
        << " stage=" << g_hotswapMenuStage
        << " ostChoice=" << g_hotswapMenuOstChoice
        << " seeded=" << (g_hotswapMenuSeeded ? 1 : 0);
    LogOut(oss.str(), true);
}

void LogHotswapRuntimeStateIfChanged(const HotswapCurrentState& state) {
    static bool s_hasLogged = false;
    static HotswapCurrentState s_lastState{};
    if (s_hasLogged && HotswapCurrentStatesEqual(state, s_lastState)) {
        return;
    }

    std::ostringstream oss;
    oss << "[HOTSWAP][MENU] runtime charsValid=" << (state.charsValid ? 1 : 0)
        << " p1Char=" << state.p1SelectId
        << " p2Char=" << state.p2SelectId
        << " paletteValid=" << (state.paletteValid ? 1 : 0)
        << " palette=" << (state.paletteSelection.p1Color + 1)
        << "/" << (state.paletteSelection.p2Color + 1)
        << " custom=" << (state.paletteSelection.p1UseCustomPalette ? 1 : 0)
        << "/" << (state.paletteSelection.p2UseCustomPalette ? 1 : 0)
        << " stageValid=" << (state.stageValid ? 1 : 0)
        << " stage=" << state.stageId
        << " bgmValid=" << (state.bgmValid ? 1 : 0)
        << " bgm=" << state.bgmTrack;
    LogOut(oss.str(), true);

    s_lastState = state;
    s_hasLogged = true;
}

void LogCustomPaletteAvailabilityIfChanged(const char* playerLabel,
                                          HotswapCustomAvailabilityLogState& state,
                                          int selectId,
                                          int palette,
                                          bool available,
                                          bool disabled,
                                          const char* reason) {
    const int availableInt = available ? 1 : 0;
    const int disabledInt = disabled ? 1 : 0;
    if (state.selectId == selectId
        && state.palette == palette
        && state.available == availableInt
        && state.disabled == disabledInt) {
        return;
    }

    std::ostringstream oss;
    oss << "[HOTSWAP][MENU] custom availability " << playerLabel
        << " char=" << selectId
        << " palette=" << (palette + 1)
        << " available=" << availableInt
        << " disabled=" << disabledInt;
    if (reason && reason[0] != '\0') {
        oss << " reason=" << reason;
    }
    LogOut(oss.str(), true);

    state.selectId = selectId;
    state.palette = palette;
    state.available = availableInt;
    state.disabled = disabledInt;
}

void ResetHotswapMenuSeedState() {
    const bool wasSeeded = g_hotswapMenuSeeded;
    g_hotswapMenuSeeded = false;
    g_hotswapP1CustomAvailabilityLog = HotswapCustomAvailabilityLogState{};
    g_hotswapP2CustomAvailabilityLog = HotswapCustomAvailabilityLogState{};
    if (wasSeeded) {
        LogOut("[HOTSWAP][MENU] reset seed for next open", true);
    }
}

int NormalizePaletteChoiceIndex(int paletteIndex) {
    if (paletteIndex < 0) {
        paletteIndex %= kHotswapPaletteChoiceCount;
        paletteIndex += kHotswapPaletteChoiceCount;
    }
    if (paletteIndex >= kHotswapPaletteChoiceCount) {
        paletteIndex %= kHotswapPaletteChoiceCount;
    }
    return paletteIndex;
}

int InferPaletteCycleDirection(int previousPalette, int currentPalette) {
    previousPalette = NormalizePaletteChoiceIndex(previousPalette);
    currentPalette = NormalizePaletteChoiceIndex(currentPalette);
    if (currentPalette == previousPalette) {
        return 1;
    }
    if (currentPalette == (previousPalette + 1) % kHotswapPaletteChoiceCount) {
        return 1;
    }
    if (currentPalette == (previousPalette + kHotswapPaletteChoiceCount - 1) % kHotswapPaletteChoiceCount) {
        return -1;
    }
    return (currentPalette > previousPalette) ? 1 : -1;
}

bool FindCustomPaletteSlotForCharacter(int selectId, int startPalette, int direction, int& outPalette) {
    if (direction < 0) {
        direction = -1;
    } else {
        direction = 1;
    }

    int palette = NormalizePaletteChoiceIndex(startPalette);
    for (int i = 0; i < kHotswapPaletteChoiceCount; ++i) {
        if (CharacterHotswap::HasCustomPaletteFile(selectId, palette)) {
            outPalette = palette;
            return true;
        }
        palette = NormalizePaletteChoiceIndex(palette + direction);
    }
    return false;
}

bool CharacterHasAnyCustomPaletteSlot(int selectId) {
    int palette = 0;
    return FindCustomPaletteSlotForCharacter(selectId, 0, 1, palette);
}

void NormalizeMenuPaletteSelection(const char* playerLabel,
                                   int selectId,
                                   int& palette,
                                   bool& useCustomPalette,
                                   int& previousPalette) {
    const int originalPalette = palette;
    const bool originalUseCustomPalette = useCustomPalette;
    const int originalPreviousPalette = previousPalette;
    palette = NormalizePaletteChoiceIndex(palette);
    previousPalette = NormalizePaletteChoiceIndex(previousPalette);

    if (useCustomPalette && !CharacterHotswap::HasCustomPaletteFile(selectId, palette)) {
        const int direction = InferPaletteCycleDirection(previousPalette, palette);
        int resolvedPalette = palette;
        if (!FindCustomPaletteSlotForCharacter(selectId, palette, direction, resolvedPalette)) {
            useCustomPalette = false;
        } else {
            palette = resolvedPalette;
        }
    }

    previousPalette = palette;

    if (originalPalette != palette
        || originalUseCustomPalette != useCustomPalette
        || originalPreviousPalette != previousPalette) {
        std::ostringstream oss;
        oss << "[HOTSWAP][MENU] normalize " << playerLabel
            << " char=" << selectId
            << " beforePalette=" << (NormalizePaletteChoiceIndex(originalPalette) + 1)
            << " beforeCustom=" << (originalUseCustomPalette ? 1 : 0)
            << " afterPalette=" << (palette + 1)
            << " afterCustom=" << (useCustomPalette ? 1 : 0)
            << " prev=" << (NormalizePaletteChoiceIndex(originalPreviousPalette) + 1)
            << " nextPrev=" << (previousPalette + 1);
        LogOut(oss.str(), true);
    }
}

void NormalizeHotswapMenuPaletteSelections() {
    const int p1SelectId = CharacterSelectIdForHotswapChoice(g_hotswapMenuP1Character);
    const int p2SelectId = CharacterSelectIdForHotswapChoice(g_hotswapMenuP2Character);
    NormalizeMenuPaletteSelection("P1",
                                  p1SelectId,
                                  g_hotswapMenuP1Palette,
                                  g_hotswapMenuP1CustomPalette,
                                  g_hotswapMenuP1PalettePrev);
    NormalizeMenuPaletteSelection("P2",
                                  p2SelectId,
                                  g_hotswapMenuP2Palette,
                                  g_hotswapMenuP2CustomPalette,
                                  g_hotswapMenuP2PalettePrev);
}

void OnHotswapP1CharacterChanged() {
    NormalizeHotswapMenuPaletteSelections();
    LogHotswapMenuSelection("P1 character changed");
}

void OnHotswapP2CharacterChanged() {
    NormalizeHotswapMenuPaletteSelections();
    LogHotswapMenuSelection("P2 character changed");
}

void OnHotswapP1PaletteChanged() {
    NormalizeHotswapMenuPaletteSelections();
    LogHotswapMenuSelection("P1 palette changed");
}

void OnHotswapP2PaletteChanged() {
    NormalizeHotswapMenuPaletteSelections();
    LogHotswapMenuSelection("P2 palette changed");
}

void OnHotswapP1CustomPaletteChanged() {
    NormalizeHotswapMenuPaletteSelections();
    LogHotswapMenuSelection("P1 custom toggled");
}

void OnHotswapP2CustomPaletteChanged() {
    NormalizeHotswapMenuPaletteSelections();
    LogHotswapMenuSelection("P2 custom toggled");
}

int CharacterSelectIdForHotswapChoice(int choiceIdx) {
    if (choiceIdx < 0 || choiceIdx >= kHotswapCharacterChoiceCount) {
        return 4;
    }
    return kHotswapCharacterSelectIds[choiceIdx];
}

int HotswapChoiceIndexFromCharacterSelectId(int selectId) {
    if (selectId == 22) {
        selectId = 10;
    }
    for (int i = 0; i < kHotswapCharacterChoiceCount; ++i) {
        if (kHotswapCharacterSelectIds[i] == selectId) {
            return i;
        }
    }
    return 4;
}

bool ReadCurrentCharacterSelectIds(HotswapCurrentState& state) {
    const uintptr_t base = GetEFZBase();
    if (!base) {
        return false;
    }

    uintptr_t screenContext = 0;
    const uintptr_t slot = base + kHotswapScreenTableRva + 4u * static_cast<uintptr_t>(kHotswapCharacterSelectScreen);
    if (!SafeReadMemory(slot, &screenContext, sizeof(screenContext)) || !screenContext) {
        return false;
    }

    uint8_t p1SelectId = 0;
    uint8_t p2SelectId = 0;
    if (!SafeReadMemory(screenContext + kHotswapCsP1SelectionOffset, &p1SelectId, sizeof(p1SelectId))) {
        return false;
    }
    if (!SafeReadMemory(screenContext + kHotswapCsP2SelectionOffset, &p2SelectId, sizeof(p2SelectId))) {
        return false;
    }

    state.p1SelectId = static_cast<int>(p1SelectId);
    state.p2SelectId = static_cast<int>(p2SelectId);
    state.charsValid = true;
    return true;
}

bool ReadCurrentHotswapState(HotswapCurrentState& state) {
    state = HotswapCurrentState{};

    const GamePhase phase = GetCurrentGamePhase();
    if (phase == GamePhase::Match) {
        const auto& d = ImGuiGui::guiState.localData;
        state.p1SelectId = HotswapChoiceIndexFromCharacterSelectId(CharacterSelectIdFromInternalCharacterId(d.p1CharID));
        state.p2SelectId = HotswapChoiceIndexFromCharacterSelectId(CharacterSelectIdFromInternalCharacterId(d.p2CharID));
        state.p1SelectId = CharacterSelectIdForHotswapChoice(state.p1SelectId);
        state.p2SelectId = CharacterSelectIdForHotswapChoice(state.p2SelectId);
        state.charsValid = true;
    } else if (phase == GamePhase::CharacterSelect) {
        ReadCurrentCharacterSelectIds(state);
    }

    CharacterHotswap::PaletteSelection paletteSelection{};
    if (CharacterHotswap::ReadCurrentPaletteSelection(paletteSelection)) {
        state.paletteSelection = paletteSelection;
        state.paletteValid = true;
    }

    const uintptr_t gameStatePtr = GetGameStatePtr();
    if (gameStatePtr) {
        uint8_t currentStage = 0;
        if (SafeReadMemory(gameStatePtr + 3890, &currentStage, sizeof(currentStage))) {
            state.stageId = static_cast<int>(currentStage);
            state.stageValid = state.stageId >= 0 && state.stageId < kNamedStageChoiceCount;
        }
        state.bgmTrack = GetBGMSlot(gameStatePtr);
        state.bgmValid = true;
    }

    LogHotswapRuntimeStateIfChanged(state);

    return state.charsValid || state.stageValid || state.bgmValid;
}

bool ReadCurrentHotswapStateCached(HotswapCurrentState& state, DWORD maxAgeMs = 100) {
    static bool s_haveCached = false;
    static bool s_cachedOk = false;
    static DWORD s_cachedTick = 0;
    static HotswapCurrentState s_cachedState{};

    const DWORD now = GetTickCount();
    if (s_haveCached && (now - s_cachedTick) < maxAgeMs) {
        state = s_cachedState;
        return s_cachedOk;
    }

    const DWORD start = now;
    HotswapCurrentState fresh{};
    const bool ok = ReadCurrentHotswapState(fresh);
    const DWORD elapsed = GetTickCount() - start;

    s_cachedState = fresh;
    s_cachedOk = ok;
    s_cachedTick = GetTickCount();
    s_haveCached = true;
    state = fresh;

    static DWORD s_lastSlowLog = 0;
    if (elapsed >= 25 && (s_lastSlowLog == 0 || (now - s_lastSlowLog) >= 1000)) {
        s_lastSlowLog = now;
        char buf[192];
        _snprintf_s(buf, sizeof(buf), _TRUNCATE,
            "[CUSTOM_MENU][TIMING] ReadCurrentHotswapState took %lums ok=%d",
            static_cast<unsigned long>(elapsed),
            ok ? 1 : 0);
        LogOut(buf, true);
    }

    return ok;
}

void UpdateCustomSavestateHotswapPromptFromSummary(const CustomSavestate::Summary& summary) {
    g_customSavestateHotswapPrompt = false;

    if (g_customSavestateHotswapDismissed) {
        LogSavestateHotswapPromptStateIfChanged("dismissed", false, nullptr, nullptr, -1, -1);
        return;
    }
    if (!summary.hasWorkingSnapshot) {
        LogSavestateHotswapPromptStateIfChanged("no-working-snapshot", false, &summary, nullptr, -1, -1);
        return;
    }
    if (summary.savedStageId == 0xFF) {
        LogSavestateHotswapPromptStateIfChanged("saved-stage-unset", false, &summary, nullptr, -1, -1);
        return;
    }

    const int savedP1SelectId = SavedSavestateSelectId(summary.savedP1CharId);
    const int savedP2SelectId = SavedSavestateSelectId(summary.savedP2CharId);
    if (savedP1SelectId < 0 || savedP2SelectId < 0) {
        LogSavestateHotswapPromptStateIfChanged("invalid-char-map", false, &summary, nullptr, savedP1SelectId, savedP2SelectId);
        return;
    }

    HotswapCurrentState current{};
    if (!ReadCurrentHotswapStateCached(current) || !current.charsValid || !current.stageValid) {
        LogSavestateHotswapPromptStateIfChanged("current-unavailable", false, &summary, &current, savedP1SelectId, savedP2SelectId);
        return;
    }

    const bool mismatch = current.p1SelectId != savedP1SelectId
        || current.p2SelectId != savedP2SelectId
        || current.stageId != summary.savedStageId;
    if (!mismatch) {
        LogSavestateHotswapPromptStateIfChanged("already-matching", false, &summary, &current, savedP1SelectId, savedP2SelectId);
        return;
    }

    const std::string p1Name = SavedSavestateDisplayName(summary.savedP1CharId);
    const std::string p2Name = SavedSavestateDisplayName(summary.savedP2CharId);
    const char* stageName = GetNamedStageLabel(summary.savedStageId);
    _snprintf_s(g_customSavestateHotswapInfo,
                sizeof(g_customSavestateHotswapInfo),
                _TRUNCATE,
                "Loaded slot differs from current match. Hotswap to %s / %s on %s?",
                p1Name.c_str(),
                p2Name.c_str(),
                stageName);
    g_customSavestateHotswapPrompt = true;
    LogSavestateHotswapPromptStateIfChanged("prompt-visible", true, &summary, &current, savedP1SelectId, savedP2SelectId);
}

void UpdateCustomSavestateHotswapPromptFromWorking() {
    if (g_customSavestateHotswapDismissed) {
        g_customSavestateHotswapPrompt = false;
        LogSavestateHotswapPromptStateIfChanged("dismissed", false, nullptr, nullptr, -1, -1);
        return;
    }

    CustomSavestate::Summary summary{};
    if (!CustomSavestate::GetSummary(summary)) {
        g_customSavestateHotswapPrompt = false;
        LogSavestateHotswapPromptStateIfChanged("summary-unavailable", false, nullptr, nullptr, -1, -1);
        return;
    }
    UpdateCustomSavestateHotswapPromptFromSummary(summary);
}

bool RevivalBgmMuted() {
    constexpr DWORD kRefreshMs = 2000;
    static bool s_haveCached = false;
    static bool s_cachedMuted = false;
    static bool s_cachedModulePresent = false;
    static bool s_cachedPathOk = false;
    static DWORD s_cachedTick = 0;

    const DWORD now = GetTickCount();
    if (s_haveCached && (now - s_cachedTick) < kRefreshMs) {
        return s_cachedMuted;
    }

    const DWORD start = now;
    bool modulePresent = false;
    bool pathOk = false;
    bool muted = false;

    HMODULE revivalModule = GetModuleHandleA("EfzRevival.dll");
    if (!revivalModule) {
        modulePresent = false;
    } else {
        modulePresent = true;
        char modulePath[MAX_PATH] = {0};
        if (GetModuleFileNameA(revivalModule, modulePath, MAX_PATH)) {
            std::string iniPath(modulePath);
            const size_t slash = iniPath.find_last_of("\\/");
            if (slash != std::string::npos) {
                iniPath.resize(slash + 1);
                iniPath += "EfzRevival.ini";
                muted = GetPrivateProfileIntA("Global Settings", "MuteBGM", 0, iniPath.c_str()) != 0;
                pathOk = true;
            }
        }
    }

    const DWORD elapsed = GetTickCount() - start;
    const bool changed = !s_haveCached ||
        s_cachedMuted != muted ||
        s_cachedModulePresent != modulePresent ||
        s_cachedPathOk != pathOk;

    s_haveCached = true;
    s_cachedMuted = muted;
    s_cachedModulePresent = modulePresent;
    s_cachedPathOk = pathOk;
    s_cachedTick = GetTickCount();

    if (changed) {
        char buf[192];
        _snprintf_s(buf, sizeof(buf), _TRUNCATE,
            "[HOTSWAP][MENU] Revival BGM mute cache module=%d path=%d muted=%d",
            modulePresent ? 1 : 0,
            pathOk ? 1 : 0,
            muted ? 1 : 0);
        LogOut(buf, true);
    }

    static DWORD s_lastSlowLog = 0;
    if (elapsed >= 25 && (s_lastSlowLog == 0 || (now - s_lastSlowLog) >= 1000)) {
        s_lastSlowLog = now;
        char buf[192];
        _snprintf_s(buf, sizeof(buf), _TRUNCATE,
            "[CUSTOM_MENU][TIMING] RevivalBgmMuted poll took %lums module=%d path=%d",
            static_cast<unsigned long>(elapsed),
            modulePresent ? 1 : 0,
            pathOk ? 1 : 0);
        LogOut(buf, true);
    }

    return muted;
}

bool HotswapOstValueDisabled() {
    return CharacterHotswap::IsBusy();
}

bool HotswapHasReloadChanges(const HotswapCurrentState& current) {
    if (!current.charsValid || !current.stageValid) {
        return false;
    }

    return CharacterSelectIdForHotswapChoice(g_hotswapMenuP1Character) != current.p1SelectId
        || CharacterSelectIdForHotswapChoice(g_hotswapMenuP2Character) != current.p2SelectId
        || (current.paletteValid && g_hotswapMenuP1Palette != current.paletteSelection.p1Color)
        || (current.paletteValid && g_hotswapMenuP2Palette != current.paletteSelection.p2Color)
        || (current.paletteValid && g_hotswapMenuP1CustomPalette != current.paletteSelection.p1UseCustomPalette)
        || (current.paletteValid && g_hotswapMenuP2CustomPalette != current.paletteSelection.p2UseCustomPalette)
        || g_hotswapMenuStage != current.stageId;
}

bool HotswapHasOstSelectionChange(const HotswapCurrentState& current) {
    if (!current.bgmValid) {
        return false;
    }
    return static_cast<int>(TrackForNamedOstChoice(g_hotswapMenuOstChoice)) != current.bgmTrack;
}

bool HotswapHasOstChangeForAction(const HotswapCurrentState& current, bool revivalBgmMuted) {
    if (revivalBgmMuted) {
        return false;
    }
    return HotswapHasOstSelectionChange(current);
}

const char* ValHotswapApply() {
    if (CharacterHotswap::IsBusy()) {
        return CharacterHotswap::GetActionValueText();
    }

    HotswapCurrentState current{};
    ReadCurrentHotswapStateCached(current);
    const bool reloadChanged = HotswapHasReloadChanges(current);
    const bool ostChanged = HotswapHasOstSelectionChange(current);

    if (!reloadChanged && !ostChanged) {
        return "NO CHANGES";
    }
    if (reloadChanged) {
        return CharacterHotswap::CanQueueReload() ? "READY" : "MATCH/CS ONLY";
    }
    return current.bgmValid ? "OST ONLY" : "UNAVAILABLE";
}

void LogReadCurrentHotswapStateSeh(unsigned code) {
    char buf[160];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE,
        "[CUSTOM_MENU][TRACE] SEH 0x%08X in ReadCurrentHotswapState",
        code);
    LogOut(buf, true);
}

static bool SehReadCurrentHotswapState(HotswapCurrentState* outState) {
    __try {
        return ReadCurrentHotswapState(*outState);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LogReadCurrentHotswapStateSeh((unsigned)GetExceptionCode());
        if (outState) {
            memset(outState, 0, sizeof(*outState));
        }
        return false;
    }
}

void SeedHotswapMenuSelectionsIfNeeded() {
    RefreshNamedOstChoices();
    if (g_hotswapMenuSeeded) {
        return;
    }

    LogOut("[CUSTOM_MENU][TRACE] SeedHotswapMenuSelectionsIfNeeded: begin", true);
    HotswapCurrentState current{};
    const bool readOk = SehReadCurrentHotswapState(&current);

    if (readOk) {
        if (current.charsValid) {
            g_hotswapMenuP1Character = HotswapChoiceIndexFromCharacterSelectId(current.p1SelectId);
            g_hotswapMenuP2Character = HotswapChoiceIndexFromCharacterSelectId(current.p2SelectId);
        }
        if (current.paletteValid) {
            g_hotswapMenuP1Palette = NormalizePaletteChoiceIndex(current.paletteSelection.p1Color);
            g_hotswapMenuP2Palette = NormalizePaletteChoiceIndex(current.paletteSelection.p2Color);
            g_hotswapMenuP1CustomPalette = current.paletteSelection.p1UseCustomPalette;
            g_hotswapMenuP2CustomPalette = current.paletteSelection.p2UseCustomPalette;
        }
        if (current.stageValid) {
            g_hotswapMenuStage = current.stageId;
        }
        if (current.bgmValid) {
            const int ostChoice = FindNamedOstChoiceIndexByTrack(current.bgmTrack);
            if (ostChoice >= 0) {
                g_hotswapMenuOstChoice = ostChoice;
            } else {
                const int stageTrackChoice = FindNamedOstChoiceIndexByTrack(10 + g_hotswapMenuStage);
                g_hotswapMenuOstChoice = (stageTrackChoice >= 0) ? stageTrackChoice : 0;
            }
        }
    } else {
        const auto& d = ImGuiGui::guiState.localData;
        if (d.p1CharID >= CHAR_ID_AKANE && d.p1CharID <= CHAR_ID_KANO) {
            g_hotswapMenuP1Character = HotswapChoiceIndexFromCharacterSelectId(CharacterSelectIdFromInternalCharacterId(d.p1CharID));
        }
        if (d.p2CharID >= CHAR_ID_AKANE && d.p2CharID <= CHAR_ID_KANO) {
            g_hotswapMenuP2Character = HotswapChoiceIndexFromCharacterSelectId(CharacterSelectIdFromInternalCharacterId(d.p2CharID));
        }
    }

    g_hotswapMenuP1PalettePrev = NormalizePaletteChoiceIndex(g_hotswapMenuP1Palette);
    g_hotswapMenuP2PalettePrev = NormalizePaletteChoiceIndex(g_hotswapMenuP2Palette);
    NormalizeHotswapMenuPaletteSelections();

    g_hotswapMenuSeeded = true;
    LogHotswapMenuSelection(current.charsValid || current.paletteValid || current.stageValid || current.bgmValid
        ? "seeded from runtime"
        : "seeded from fallback local data");
}

void RunMenuHotswapApply() {
    if (CharacterHotswap::IsBusy()) {
        return;
    }

    HotswapCurrentState current{};
    ReadCurrentHotswapState(current);

    const int p1SelectId = CharacterSelectIdForHotswapChoice(g_hotswapMenuP1Character);
    const int p2SelectId = CharacterSelectIdForHotswapChoice(g_hotswapMenuP2Character);
    const bool reloadChanged = HotswapHasReloadChanges(current);
    unsigned short targetTrack = TrackForNamedOstChoice(g_hotswapMenuOstChoice);
    const bool revivalBgmMuted = RevivalBgmMuted();
    bool ostChanged = HotswapHasOstChangeForAction(current, revivalBgmMuted);

    if (revivalBgmMuted) {
        if (current.bgmValid) {
            targetTrack = static_cast<unsigned short>(current.bgmTrack);
        }
        if (HotswapHasOstSelectionChange(current)) {
            LogOut("[HOTSWAP] OST request ignored because Revival MuteBGM is enabled", true);
        }
    }

    if (!reloadChanged && !ostChanged) {
        LogOut("[HOTSWAP] apply ignored because menu selections match current runtime state", true);
        return;
    }

    if (!reloadChanged) {
        const uintptr_t gameStatePtr = GetGameStatePtr();
        if (!gameStatePtr) {
            LogOut("[HOTSWAP] OST-only apply failed because game state pointer was unavailable", true);
            return;
        }
        if (PlayBGM(gameStatePtr, targetTrack)) {
            LogOut("[HOTSWAP] applied OST without reload track=" + std::to_string(targetTrack), true);
        } else {
            LogOut("[HOTSWAP] failed to apply OST without reload track=" + std::to_string(targetTrack), true);
        }
        return;
    }

    CharacterHotswap::PaletteSelection paletteSelection{};
    paletteSelection.p1Color = g_hotswapMenuP1Palette;
    paletteSelection.p2Color = g_hotswapMenuP2Palette;
    paletteSelection.p1UseCustomPalette = g_hotswapMenuP1CustomPalette;
    paletteSelection.p2UseCustomPalette = g_hotswapMenuP2CustomPalette;
    {
        std::ostringstream oss;
        oss << "[HOTSWAP][MENU] apply request"
            << " runtimePaletteValid=" << (current.paletteValid ? 1 : 0)
            << " runtimePalette=" << (current.paletteSelection.p1Color + 1)
            << "/" << (current.paletteSelection.p2Color + 1)
            << " runtimeCustom=" << (current.paletteSelection.p1UseCustomPalette ? 1 : 0)
            << "/" << (current.paletteSelection.p2UseCustomPalette ? 1 : 0)
            << " requestedPalette=" << (paletteSelection.p1Color + 1)
            << "/" << (paletteSelection.p2Color + 1)
            << " requestedCustom=" << (paletteSelection.p1UseCustomPalette ? 1 : 0)
            << "/" << (paletteSelection.p2UseCustomPalette ? 1 : 0);
        LogOut(oss.str(), true);
    }
    CharacterHotswap::SanitizePaletteSelection(p1SelectId, p2SelectId, paletteSelection);
    {
        std::ostringstream oss;
        oss << "[HOTSWAP][MENU] apply sanitized"
            << " palette=" << (paletteSelection.p1Color + 1)
            << "/" << (paletteSelection.p2Color + 1)
            << " custom=" << (paletteSelection.p1UseCustomPalette ? 1 : 0)
            << "/" << (paletteSelection.p2UseCustomPalette ? 1 : 0);
        LogOut(oss.str(), true);
    }

    CharacterHotswap::QueueReload(p1SelectId,
                                  p2SelectId,
                                  g_hotswapMenuStage,
                                  paletteSelection,
                                  targetTrack);
}

bool ExitToCharacterSelectDisabled() {
    return !CanRequestFrontendExit(FrontendExitTarget::CharacterSelect);
}

bool ExitToTitleDisabled() {
    return !CanRequestFrontendExit(FrontendExitTarget::Title);
}

bool HotswapSelectionValueDisabled() {
    return CharacterHotswap::IsBusy();
}

bool HotswapP1CustomPaletteDisabled() {
    if (CharacterHotswap::IsBusy()) {
        LogCustomPaletteAvailabilityIfChanged("P1",
                                             g_hotswapP1CustomAvailabilityLog,
                                             CharacterSelectIdForHotswapChoice(g_hotswapMenuP1Character),
                                             g_hotswapMenuP1Palette,
                                             false,
                                             true,
                                             "busy");
        return true;
    }
    const int selectId = CharacterSelectIdForHotswapChoice(g_hotswapMenuP1Character);
    const bool available = CharacterHotswap::HasCustomPaletteFile(selectId, g_hotswapMenuP1Palette);
    const bool disabled = !available;
    LogCustomPaletteAvailabilityIfChanged("P1",
                                         g_hotswapP1CustomAvailabilityLog,
                                         selectId,
                                         g_hotswapMenuP1Palette,
                                         available,
                                         disabled,
                                         disabled ? "missing custom .pal" : "available");
    return disabled;
}

bool HotswapP2CustomPaletteDisabled() {
    if (CharacterHotswap::IsBusy()) {
        LogCustomPaletteAvailabilityIfChanged("P2",
                                             g_hotswapP2CustomAvailabilityLog,
                                             CharacterSelectIdForHotswapChoice(g_hotswapMenuP2Character),
                                             g_hotswapMenuP2Palette,
                                             false,
                                             true,
                                             "busy");
        return true;
    }
    const int selectId = CharacterSelectIdForHotswapChoice(g_hotswapMenuP2Character);
    const bool available = CharacterHotswap::HasCustomPaletteFile(selectId, g_hotswapMenuP2Palette);
    const bool disabled = !available;
    LogCustomPaletteAvailabilityIfChanged("P2",
                                         g_hotswapP2CustomAvailabilityLog,
                                         selectId,
                                         g_hotswapMenuP2Palette,
                                         available,
                                         disabled,
                                         disabled ? "missing custom .pal" : "available");
    return disabled;
}

bool HotswapReloadDisabled() {
    if (CharacterHotswap::IsBusy()) {
        return false;
    }

    HotswapCurrentState current{};
    ReadCurrentHotswapStateCached(current);
    const bool reloadChanged = HotswapHasReloadChanges(current);
    const bool ostChanged = HotswapHasOstSelectionChange(current);

    if (!reloadChanged && !ostChanged) {
        return true;
    }
    if (reloadChanged) {
        return !CharacterHotswap::CanQueueReload();
    }
    return !current.bgmValid;
}

const char* ValExitToCharacterSelect() {
    return CanRequestFrontendExit(FrontendExitTarget::CharacterSelect) ? "READY" : "MATCH ONLY";
}

const char* ValExitToTitle() {
    return CanRequestFrontendExit(FrontendExitTarget::Title) ? "READY" : "UNAVAILABLE";
}

Row* BuildHotswapOptionsRows(int& count);
Row* BuildSettingsAudioRows(int& count);
Row* BuildHelpMacroRows(int& count);

void QueueMenuNav(int pane, int focusRow, RowListBuilder sub, const char* subTitle, int subFocus) {
    Screens::MenuNavigationRequest req{};
    req.pane = pane;
    req.focusRow = focusRow;
    req.submenuBuilder = sub;
    req.submenuTitle = subTitle;
    req.submenuFocusRow = subFocus;
    Screens::RequestMenuNavigation(req);
}

void NavToHotswap() {
    QueueMenuNav(Screens::MenuPane::Options, 0, BuildHotswapOptionsRows, "MATCH HOTSWAP", 2);
}
void NavToChars() {
    QueueMenuNav(Screens::MenuPane::Chars, 0, nullptr, nullptr, 0);
}
void NavToHelp() {
    QueueMenuNav(Screens::MenuPane::HelpStart, 0, nullptr, nullptr, 0);
}
void NavToAbout() {
    QueueMenuNav(Screens::MenuPane::HelpAbout, 0, nullptr, nullptr, 0);
}
void NavToMacroHelp() {
    QueueMenuNav(Screens::MenuPane::HelpGuide, 0, BuildHelpMacroRows, "MACROS", 1);
}
void NavToSound() {
    QueueMenuNav(Screens::MenuPane::SettingsGeneral, 0, BuildSettingsAudioRows, "AUDIO", 1);
}

Row* BuildMenuRows(int& count) {
    static Row s_rows[16];
    int n = 0;

    s_rows[n++] = Header("SHORTCUTS");
    s_rows[n++] = Action("CHANGE CHARACTERS / STAGE", NavToHotswap, ValHotswapApply);
    s_rows[n++] = Action("CHARACTER SETTINGS", NavToChars);
    s_rows[n++] = Action("HELP", NavToHelp);
    s_rows[n++] = Action("ABOUT", NavToAbout);
    s_rows[n++] = Action("SOUND SETTINGS", NavToSound, ValAudioSettings);

    s_rows[n++] = Spacer();
    s_rows[n++] = Header("EXIT");
    s_rows[n++] = Action("EXIT TO CHARACTER SELECT", RunExitToCharacterSelect, ValExitToCharacterSelect, ExitToCharacterSelectDisabled);
    s_rows[n++] = Action("EXIT TO TITLE SCREEN",     RunExitToTitle,           ValExitToTitle,           ExitToTitleDisabled);

    count = n;
    return s_rows;
}

Row* BuildHotswapOptionsRows(int& count) {
    static Row s_rows[20];
    int n = 0;

    SeedHotswapMenuSelectionsIfNeeded();

    s_rows[n++] = Header("MATCH HOTSWAP");
    s_rows[n++] = Info("Change characters, palettes, stage, or music without leaving Practice.");
    s_rows[n++] = DropdownRow("PLAYER 1 CHARACTER", &g_hotswapMenuP1Character,
                              kHotswapCharacterChoices,
                              kHotswapCharacterChoiceCount,
                              OnHotswapP1CharacterChanged, HotswapSelectionValueDisabled);
    s_rows[n++] = DropdownRow("PLAYER 2 CHARACTER", &g_hotswapMenuP2Character,
                              kHotswapCharacterChoices,
                              kHotswapCharacterChoiceCount,
                              OnHotswapP2CharacterChanged, HotswapSelectionValueDisabled);
    s_rows[n++] = ChoicesRow("PLAYER 1 PALETTE", &g_hotswapMenuP1Palette,
                             kHotswapPaletteChoices,
                             kHotswapPaletteChoiceCount,
                             OnHotswapP1PaletteChanged,
                             HotswapSelectionValueDisabled);
    s_rows[n++] = Toggle("PLAYER 1 CUSTOM PALETTE", &g_hotswapMenuP1CustomPalette,
                         OnHotswapP1CustomPaletteChanged,
                         HotswapP1CustomPaletteDisabled,
                         nullptr,
                         true);
    s_rows[n++] = ChoicesRow("PLAYER 2 PALETTE", &g_hotswapMenuP2Palette,
                             kHotswapPaletteChoices,
                             kHotswapPaletteChoiceCount,
                             OnHotswapP2PaletteChanged,
                             HotswapSelectionValueDisabled);
    s_rows[n++] = Toggle("PLAYER 2 CUSTOM PALETTE", &g_hotswapMenuP2CustomPalette,
                         OnHotswapP2CustomPaletteChanged,
                         HotswapP2CustomPaletteDisabled,
                         nullptr,
                         true);
    s_rows[n++] = DropdownRow("STAGE", &g_hotswapMenuStage,
                              kNamedStageChoices,
                              kNamedStageChoiceCount,
                              nullptr, HotswapSelectionValueDisabled);
    s_rows[n++] = DropdownRow("OST", &g_hotswapMenuOstChoice,
                              kNamedOstChoices,
                              kNamedOstChoiceCount,
                              nullptr, HotswapOstValueDisabled);
    s_rows[n++] = Action("APPLY SELECTIONS", RunMenuHotswapApply,
                         ValHotswapApply,
                         HotswapReloadDisabled);

    count = n;
    return s_rows;
}

// ===== OPTIONS screen =====
void OnFaOverlayPersist()  { g_showFrameAdvantageOverlay.store(g_mirrorFaOverlay); }
void OnFrameBarPersist() {
    PersistBool("General", "showFrameBar", MutableSettings().showFrameBar);
    FrameBar::g_enabled.store(MutableSettings().showFrameBar);
    FrameBar::Reset();
}
void OnFrameBarTiming() {
    PersistInt("General", "frameBarTimingMode", MutableSettings().frameBarTimingMode);
    FrameBar::Reset();
}
void OnFrameBarDetail() {
    PersistInt("General", "frameBarDetailMode", MutableSettings().frameBarDetailMode);
}
bool FrameBarOptionsHidden() { return !MutableSettings().showFrameBar; }

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
    EngineRegenMode regenMode = EngineRegenMode::Unknown;
    if (!GetEngineRegenStatus(regenMode, a, b)) return;

    if (regenMode == EngineRegenMode::F5_FullOrPreset) {
        if (b == 3332) {
            g_f5Mode = 2;
            g_f4Mode = 0;
        } else if (a == 1000 && b == 9999 && g_f4Mode == 1) {
            // F4 "Full Blue" uses the same Param A/B as F5 Full - keep UI on F4.
            g_f5Mode = 0;
        } else {
            g_f5Mode = 1;
            g_f4Mode = 0;
        }
        return;
    }

    g_f5Mode = 0;
    if (regenMode == EngineRegenMode::F4_FineTuneActive && b == 9999 && a > 0) {
        if (a == 1000 && g_f4Mode == 1) {
            g_f4Mode = 1;
        } else {
            g_f4Mode = 2;
            float rf = 0.0f; bool blue = false;
            if (DeriveRfFromParamA(a, rf, blue)) {
                g_f4Color = blue ? 1 : 0;
                g_f4RfAmount = (int)rf;
            }
        }
        return;
    }

    if (a == 0 && b == 0) {
        g_f4Mode = 0;
    }
}

void ApplyF5() {
    if (g_f5Mode != 0) {
        g_f4Mode = 0;
    }
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
    if (g_f4Mode != 0) {
        g_f5Mode = 0;
    }
    if (g_f4Mode == 0) WriteEngineRegenParams(0, 0);
    else if (g_f4Mode == 1) WriteEngineRegenParams(1000, 9999);
    else ApplyF4Custom();
}
void OnF4Custom() { if (g_f4Mode == 2) ApplyF4Custom(); }
bool F4DisabledByF5() { return g_f5Mode != 0; }
bool F4CustomHidden() { return g_f4Mode != 2 || F4DisabledByF5(); }

// Framestep
int  g_framestepMode = 0;     // 0=FullFrame, 1=Subframe
void OnFramestepEnabled() {
    PersistBool("General", "framestepEnabled", MutableSettings().framestepEnabled);
}
void OnSuppressRevivalFramestep() {
    PersistBool("General", "suppressRevivalFramestep", MutableSettings().suppressRevivalFramestep);
}
bool FramestepModeHidden() { return !Framestep::IsEnabled(); }
void RefreshFramestepMirror() {
    g_framestepMode = (Framestep::GetStepMode() == Framestep::StepMode::Subframe) ? 1 : 0;
}
void OnFramestepMode() {
    Framestep::SetStepMode(g_framestepMode == 1 ? Framestep::StepMode::Subframe
                                                : Framestep::StepMode::FullFrame);
}
const char* const kFramestepChoices[2] = { "FULL FRAMES", "SUBFRAMES" };
const char* const kComboDetailChoices[2] = { "COMBO STATE", "LAST HIT" };

const char* ValVanillaRegen() {
    if (g_f5Mode != 0) {
        return (g_f5Mode == 2) ? "F5 FM" : "F5 ON";
    }
    if (g_f4Mode == 1) return "F4 FULL BLUE";
    if (g_f4Mode == 2) return "F4 CUSTOM";
    return "OFF";
}

const char* ValContinuousRecovery() {
    const bool p1 = g_crEnabledP1;
    const bool p2 = g_crEnabledP2;
    if (p1 && p2) return "P1 + P2";
    if (p1) return "P1 ON";
    if (p2) return "P2 ON";
    return "OFF";
}

const char* ValPlayerValuesSummary() {
    return "HP / METER / RF / POS";
}

bool ComboStatsHidden() { return !MutableSettings().showComboStatisticsOverlay; }
bool ComboDetailSourceHidden() { return ComboStatsHidden() || !MutableSettings().comboOverlayShowDetailRow; }
bool ComboFinalDurationHidden() { return ComboStatsHidden() || !MutableSettings().comboOverlayShowFinalSummary; }

Row* BuildVanillaRegenRows(int& count) {
    static Row s_rows[12];
    int n = 0;

    s_rows[n++] = Header("VANILLA REGENERATION");
    s_rows[n++] = Info("These options mirror the game's F4/F5 recovery. F4 and F5 cannot run at the same time.");
    s_rows[n++] = ChoicesRow("AUTOMATIC HEALTH AND METER RECOVERY (F4)",
                             &g_f4Mode, kF4ModeChoices, 3, OnF4Mode, F4DisabledByF5);
    s_rows[n++] = ChoicesRow("  COLOR", &g_f4Color, kF4ColorChoices, 2, OnF4Custom,
                             F4DisabledByF5, F4CustomHidden);
    s_rows[n++] = IntNum("  RF AMOUNT", &g_f4RfAmount, 0, 1000, 50, 100, OnF4Custom,
                         F4DisabledByF5, F4CustomHidden);
    s_rows[n++] = ChoicesRow("PRESETS FOR RECOVERY (F5)",
                             &g_f5Mode, kF5ModeChoices, 3, OnF5Mode);

    count = n;
    return s_rows;
}

Row* BuildPlayerValuesPlaceholderRows(int& count) {
    static Row s_rows[1];
    count = 1;
    s_rows[0] = Info("Edit player values in the columns below.");
    return s_rows;
}

Row* BuildContinuousRecoveryPlaceholderRows(int& count) {
    static Row s_rows[1];
    count = 1;
    s_rows[0] = Info("Configure per-player recovery in the columns below.");
    return s_rows;
}

Row* BuildValuesRootRows(int& count) {
    static Row s_rows[24];
    int n = 0;

    s_rows[n++] = Header("VALUES");
    s_rows[n++] = Submenu("PLAYER VALUES", "PLAYER VALUES",
                          BuildPlayerValuesPlaceholderRows, ValPlayerValuesSummary);

    s_rows[n++] = Spacer();
    s_rows[n++] = Header("VANILLA REGENERATION");
    s_rows[n++] = ChoicesRow("AUTOMATIC HEALTH AND METER RECOVERY (F4)",
                             &g_f4Mode, kF4ModeChoices, 3, OnF4Mode, F4DisabledByF5);
    s_rows[n++] = ChoicesRow("  COLOR", &g_f4Color, kF4ColorChoices, 2, OnF4Custom,
                             F4DisabledByF5, F4CustomHidden);
    s_rows[n++] = IntNum("  RF AMOUNT", &g_f4RfAmount, 0, 1000, 50, 100, OnF4Custom,
                         F4DisabledByF5, F4CustomHidden);
    s_rows[n++] = ChoicesRow("PRESETS FOR RECOVERY (F5)",
                             &g_f5Mode, kF5ModeChoices, 3, OnF5Mode);

    s_rows[n++] = Spacer();
    s_rows[n++] = Submenu("CONTINUOUS RECOVERY", "CONTINUOUS RECOVERY",
                          BuildContinuousRecoveryPlaceholderRows, ValContinuousRecovery);

    count = n;
    return s_rows;
}

void RunPracticeSavestateSave() {
    if (SavestateHook::TriggerSave()) {
        DirectDrawHook::AddMessage("Practice state saved.", "SAVESTATE", RGB(120, 255, 120), 1200, 0, 120);
    } else {
        DirectDrawHook::AddMessage("Save failed - enter Practice mode first.", "SAVESTATE", RGB(255, 120, 120), 1800, 0, 120);
    }
}

void RunPracticeSavestateLoad() {
    if (SavestateHook::TriggerLoad()) {
        DirectDrawHook::AddMessage("Practice state loaded.", "SAVESTATE", RGB(120, 255, 120), 1200, 0, 120);
    } else {
        DirectDrawHook::AddMessage("Load failed - save a state first.", "SAVESTATE", RGB(255, 120, 120), 1800, 0, 120);
    }
}

const char* ValSavestateOptions() {
    static char s_buf[48];
    const unsigned saves = SavestateHook::GetSaveCount();
    const unsigned loads = SavestateHook::GetLoadCount();
    _snprintf_s(s_buf, sizeof(s_buf), _TRUNCATE, "S%u L%u", saves, loads);
    return s_buf;
}

Row* BuildComboStatisticsRows(int& count) {
    static Row s_rows[16];
    int n = 0;
    auto& s = MutableSettings();

    s_rows[n++] = Header("COMBO STATISTICS");
    s_rows[n++] = Toggle("ENABLE OVERLAY", &s.showComboStatisticsOverlay, OnShowCombo);
    s_rows[n++] = Toggle("  SHOW DETAIL ROW", &s.comboOverlayShowDetailRow, OnComboDetailRow, nullptr, ComboStatsHidden);
    s_rows[n++] = ChoicesRow("  DETAIL SOURCE", &s.comboOverlayDetailRowSource, kComboDetailChoices, 2,
                             OnComboDetailSrc, nullptr, ComboDetailSourceHidden);
    s_rows[n++] = Toggle("  KEEP FINAL SUMMARY", &s.comboOverlayShowFinalSummary, OnComboFinal, nullptr, ComboStatsHidden);
    s_rows[n++] = FloatNum("  SUMMARY TIME", &s.comboOverlayDisplayDuration, 0.5f, 30.0f, 0.1f, 1.0f, "%.1f",
                           OnComboDuration, nullptr, ComboFinalDurationHidden);
    s_rows[n++] = Toggle("  HIDE WITH MENU", &s.comboOverlayHideWhenImGuiVisible, OnComboHideMenu, nullptr, ComboStatsHidden);
    s_rows[n++] = Toggle("  RESUME AFTER MENU", &s.comboOverlayResumeAfterImGui, OnComboResume, nullptr, ComboStatsHidden);
    s_rows[n++] = Toggle("  SHOW RF MULTIPLIER", &s.comboOverlayShowRfMultiplier, OnComboRfMult, nullptr, ComboStatsHidden);
    s_rows[n++] = Toggle("  SHOW RAW SCALE", &s.comboOverlayShowRawScale, OnComboRawScale, nullptr, ComboStatsHidden);

    count = n;
    return s_rows;
}

const char* ValComboStatistics() {
    return MutableSettings().showComboStatisticsOverlay ? "ON" : "OFF";
}

Row* BuildSavestateOptionsRows(int& count) {
    static Row s_rows[16];
    int n = 0;
    auto& s = MutableSettings();

    s_rows[n++] = Header("PRACTICE SNAPSHOTS");
    s_rows[n++] = Info("Save and load the current Practice match through EfzRevival. Use this for retry loops and setup practice.");
    s_rows[n++] = Action("SAVE PRACTICE STATE", RunPracticeSavestateSave);
    s_rows[n++] = Action("LOAD PRACTICE STATE", RunPracticeSavestateLoad);
    s_rows[n++] = Spacer();
    s_rows[n++] = Header("OPTIONS");
    s_rows[n++] = Toggle("LOAD CUSTOM PALETTES", &s.savestateLoadCustomPalettes, OnSavestateLoadCustomPalettes);
    s_rows[n++] = Info("Turn off if a load shows broken colors - keeps palette numbers but uses default colors.");
    s_rows[n++] = Spacer();
    s_rows[n++] = Header("HOTKEYS");
    s_rows[n++] = Info("Assign Save, Load, and slot keys under Settings > Hotkeys > Savestate.");
    s_rows[n++] = Info("Hotkeys write to the active slot immediately. The menu buttons above always target the live match.");

    count = n;
    return s_rows;
}

Row* BuildOptionsRows(int& count) {
    static Row s_rows[40];
    int n = 0;
    auto& s = MutableSettings();

    s_rows[n++] = Header("GAMEPLAY");
    s_rows[n++] = Toggle("FINAL MEMORY AT ANY HP",  &g_mirrorFmBypass,  OnFmBypass);
    s_rows[n++] = Toggle("FRAME ADVANTAGE OVERLAY", &g_mirrorFaOverlay, OnFaOverlayPersist);
    s_rows[n++] = FloatNum("FA DURATION (SEC)", &s.frameAdvantageDisplayDuration,
                            0.5f, 30.0f, 0.1f, 1.0f, "%.1f", OnFADuration);
    s_rows[n++] = Submenu("DISPLAY OVERLAYS", "DISPLAY OVERLAYS", BuildDisplayOverlayRows, ValDisplaySettings);

    s_rows[n++] = Spacer();
    s_rows[n++] = Header("FRAME BAR");
    s_rows[n++] = Toggle("SHOW FRAME BAR", &s.showFrameBar, OnFrameBarPersist);
    s_rows[n++] = ChoicesRow("  CELL STEP", &s.frameBarTimingMode, kFrameBarTimingChoices, 2,
                             OnFrameBarTiming, nullptr, FrameBarOptionsHidden);
    s_rows[n++] = ChoicesRow("  DETAIL", &s.frameBarDetailMode, kFrameBarDetailChoices, 3,
                             OnFrameBarDetail, nullptr, FrameBarOptionsHidden);

    s_rows[n++] = Spacer();
    s_rows[n++] = Header("FRAMESTEP");
    s_rows[n++] = Toggle("ENABLE FRAMESTEP", &s.framestepEnabled, OnFramestepEnabled);
    s_rows[n++] = Toggle("SUPPRESS REVIVAL STEP", &s.suppressRevivalFramestep, OnSuppressRevivalFramestep);
    s_rows[n++] = ChoicesRow("STEP MODE", &g_framestepMode, kFramestepChoices, 2,
                             OnFramestepMode, nullptr, FramestepModeHidden);

    s_rows[n++] = Spacer();
    s_rows[n++] = Header("COMBO STATISTICS");
    s_rows[n++] = Toggle("ENABLE OVERLAY", &s.showComboStatisticsOverlay, OnShowCombo);
    s_rows[n++] = Toggle("  SHOW DETAIL ROW", &s.comboOverlayShowDetailRow, OnComboDetailRow, nullptr, ComboStatsHidden);
    s_rows[n++] = ChoicesRow("  DETAIL SOURCE", &s.comboOverlayDetailRowSource, kComboDetailChoices, 2,
                             OnComboDetailSrc, nullptr, ComboDetailSourceHidden);
    s_rows[n++] = Toggle("  KEEP FINAL SUMMARY", &s.comboOverlayShowFinalSummary, OnComboFinal, nullptr, ComboStatsHidden);
    s_rows[n++] = FloatNum("  SUMMARY TIME", &s.comboOverlayDisplayDuration, 0.5f, 30.0f, 0.1f, 1.0f, "%.1f",
                           OnComboDuration, nullptr, ComboFinalDurationHidden);
    s_rows[n++] = Toggle("  HIDE WITH MENU", &s.comboOverlayHideWhenImGuiVisible, OnComboHideMenu, nullptr, ComboStatsHidden);
    s_rows[n++] = Toggle("  RESUME AFTER MENU", &s.comboOverlayResumeAfterImGui, OnComboResume, nullptr, ComboStatsHidden);
    s_rows[n++] = Toggle("  SHOW RF MULTIPLIER", &s.comboOverlayShowRfMultiplier, OnComboRfMult, nullptr, ComboStatsHidden);
    s_rows[n++] = Toggle("  SHOW RAW SCALE", &s.comboOverlayShowRawScale, OnComboRawScale, nullptr, ComboStatsHidden);

    s_rows[n++] = Spacer();
    s_rows[n++] = Header("OPTION MENUS");
    s_rows[n++] = Submenu("SAVESTATES", "SAVESTATES", BuildSavestateOptionsRows, ValSavestateOptions);
    s_rows[n++] = Submenu("HOTSWAP", "MATCH HOTSWAP", BuildHotswapOptionsRows, ValHotswapApply);

    count = n;
    return s_rows;
}

bool CrRowHiddenImpl(int player, int row) {
    if (player == 0) {
        if (row == 2) return CrP1HpCustomHidden();
        if (row == 4) return CrP1MeterCustomHidden();
        if (row == 6) return CrP1RfCustomHidden();
        if (row == 7) return CrP1RfBicHidden();
    } else {
        if (row == 2) return CrP2HpCustomHidden();
        if (row == 4) return CrP2MeterCustomHidden();
        if (row == 6) return CrP2RfCustomHidden();
        if (row == 7) return CrP2RfBicHidden();
    }
    return false;
}

const char* CrRowLabelImpl(int row) {
    static const char* const kLabels[CrEditorRowCount] = {
        "ENABLE", "HP MODE", "HP CUSTOM", "METER MODE", "METER CUSTOM",
        "RF MODE", "RF CUSTOM", "RF FORCE BLUE IC",
    };
    if (row < 0 || row >= CrEditorRowCount) return "";
    return kLabels[row];
}

void CrFormatCellImpl(int player, int row, char* buf, size_t bufSz) {
    if (!buf || bufSz == 0) return;
    buf[0] = '\0';
    if (row < 0 || row >= CrEditorRowCount) return;

    if (player == 0) {
        switch (row) {
            case 0: _snprintf_s(buf, bufSz, _TRUNCATE, "%s", g_crEnabledP1 ? "ON" : "OFF"); break;
            case 1: _snprintf_s(buf, bufSz, _TRUNCATE, "%s", kCrHpModeChoices[g_crHpModeP1]); break;
            case 2: _snprintf_s(buf, bufSz, _TRUNCATE, "%d", g_crHpCustomP1); break;
            case 3: _snprintf_s(buf, bufSz, _TRUNCATE, "%s", kCrMeterModeChoices[g_crMeterModeP1]); break;
            case 4: _snprintf_s(buf, bufSz, _TRUNCATE, "%d", g_crMeterCustomP1); break;
            case 5: _snprintf_s(buf, bufSz, _TRUNCATE, "%s", kCrRfModeChoices[g_crRfModeP1]); break;
            case 6: _snprintf_s(buf, bufSz, _TRUNCATE, "%.0f", g_crRfCustomP1); break;
            case 7: _snprintf_s(buf, bufSz, _TRUNCATE, "%s", g_crForceBlueICP1 ? "ON" : "OFF"); break;
        }
    } else {
        switch (row) {
            case 0: _snprintf_s(buf, bufSz, _TRUNCATE, "%s", g_crEnabledP2 ? "ON" : "OFF"); break;
            case 1: _snprintf_s(buf, bufSz, _TRUNCATE, "%s", kCrHpModeChoices[g_crHpModeP2]); break;
            case 2: _snprintf_s(buf, bufSz, _TRUNCATE, "%d", g_crHpCustomP2); break;
            case 3: _snprintf_s(buf, bufSz, _TRUNCATE, "%s", kCrMeterModeChoices[g_crMeterModeP2]); break;
            case 4: _snprintf_s(buf, bufSz, _TRUNCATE, "%d", g_crMeterCustomP2); break;
            case 5: _snprintf_s(buf, bufSz, _TRUNCATE, "%s", kCrRfModeChoices[g_crRfModeP2]); break;
            case 6: _snprintf_s(buf, bufSz, _TRUNCATE, "%.0f", g_crRfCustomP2); break;
            case 7: _snprintf_s(buf, bufSz, _TRUNCATE, "%s", g_crForceBlueICP2 ? "ON" : "OFF"); break;
        }
    }
}

void CrAdjustCellImpl(int player, int row, int direction, bool bigStep) {
    const int dir = (direction < 0) ? -1 : 1;
    if (player == 0) {
        switch (row) {
            case 0: g_crEnabledP1 = !g_crEnabledP1; OnCrEnabledP1(); break;
            case 1: g_crHpModeP1 = (g_crHpModeP1 + dir + 4) % 4; OnCrHpModeP1(); break;
            case 2: { int step = bigStep ? 1000 : 100; g_crHpCustomP1 += dir * step; if (g_crHpCustomP1 < 0) g_crHpCustomP1 = 0; if (g_crHpCustomP1 > 9999) g_crHpCustomP1 = 9999; OnCrHpCustomP1(); break; }
            case 3: g_crMeterModeP1 = (g_crMeterModeP1 + dir + 6) % 6; OnCrMeterModeP1(); break;
            case 4: { int step = bigStep ? 500 : 50; g_crMeterCustomP1 += dir * step; if (g_crMeterCustomP1 < 0) g_crMeterCustomP1 = 0; if (g_crMeterCustomP1 > 3000) g_crMeterCustomP1 = 3000; OnCrMeterCustomP1(); break; }
            case 5: g_crRfModeP1 = (g_crRfModeP1 + dir + 6) % 6; OnCrRfModeP1(); break;
            case 6: { float step = bigStep ? 100.0f : 10.0f; g_crRfCustomP1 += dir * step; if (g_crRfCustomP1 < 0.0f) g_crRfCustomP1 = 0.0f; if (g_crRfCustomP1 > 1000.0f) g_crRfCustomP1 = 1000.0f; OnCrRfCustomP1(); break; }
            case 7: g_crForceBlueICP1 = !g_crForceBlueICP1; OnCrForceBlueICP1(); break;
        }
    } else {
        switch (row) {
            case 0: g_crEnabledP2 = !g_crEnabledP2; OnCrEnabledP2(); break;
            case 1: g_crHpModeP2 = (g_crHpModeP2 + dir + 4) % 4; OnCrHpModeP2(); break;
            case 2: { int step = bigStep ? 1000 : 100; g_crHpCustomP2 += dir * step; if (g_crHpCustomP2 < 0) g_crHpCustomP2 = 0; if (g_crHpCustomP2 > 9999) g_crHpCustomP2 = 9999; OnCrHpCustomP2(); break; }
            case 3: g_crMeterModeP2 = (g_crMeterModeP2 + dir + 6) % 6; OnCrMeterModeP2(); break;
            case 4: { int step = bigStep ? 500 : 50; g_crMeterCustomP2 += dir * step; if (g_crMeterCustomP2 < 0) g_crMeterCustomP2 = 0; if (g_crMeterCustomP2 > 3000) g_crMeterCustomP2 = 3000; OnCrMeterCustomP2(); break; }
            case 5: g_crRfModeP2 = (g_crRfModeP2 + dir + 6) % 6; OnCrRfModeP2(); break;
            case 6: { float step = bigStep ? 100.0f : 10.0f; g_crRfCustomP2 += dir * step; if (g_crRfCustomP2 < 0.0f) g_crRfCustomP2 = 0.0f; if (g_crRfCustomP2 > 1000.0f) g_crRfCustomP2 = 1000.0f; OnCrRfCustomP2(); break; }
            case 7: g_crForceBlueICP2 = !g_crForceBlueICP2; OnCrForceBlueICP2(); break;
        }
    }
}

void CrActivateCellImpl(int player, int row) {
    CrAdjustCellImpl(player, row, +1, false);
}

void CorrectValueLocksForEngineRegenUiImpl(GuiValueLocks::State& locks) {
    if (g_f4Mode == 1 && g_f5Mode == 0 &&
        locks.globalReason == GuiValueLocks::GlobalReason::EngineF5) {
        locks.globalReason = GuiValueLocks::GlobalReason::EngineF4;
    }
}

} // namespace

const char* FormatTriggerButtonRow(const Row& row) {
    return FormatTriggerButtonRowImpl(row);
}

bool AdjustTriggerButtonRow(const Row& row, int direction) {
    return AdjustTriggerButtonRowImpl(row, direction);
}

// ===== Public per-screen entry points =====

void ResetHotswapMenuSeed() {
    ResetHotswapMenuSeedState();
}

// ===== Macros =====
bool g_macroIncludeBuffers = true;
int  g_macroSlotMirror = 1;
int  g_macroLastSlot = -1;
bool g_macroLastInclude = true;
bool g_macroForceReload = true;
std::string g_macroText;
std::string g_macroApplyError;
std::vector<std::string> g_macroUndoStack;
std::vector<std::string> g_macroRedoStack;

struct MacroTextEditorState {
    bool active = false;
    bool wantFocus = false;
    std::vector<char> buffer;
};
MacroTextEditorState g_macroEditor;

void MacroPushUndo(const std::string& previous) {
    if (!g_macroUndoStack.empty() && g_macroUndoStack.back() == previous) return;
    g_macroUndoStack.push_back(previous);
    if (g_macroUndoStack.size() > 64) {
        g_macroUndoStack.erase(g_macroUndoStack.begin());
    }
}

void MacroEnsureEditorBuffer(const std::string& text) {
    const size_t minCap = 8192;
    const size_t cap = (std::max)(minCap, text.size() + 1024);
    g_macroEditor.buffer.assign(cap, '\0');
    if (!text.empty()) {
        memcpy(g_macroEditor.buffer.data(), text.data(), (std::min)(text.size(), cap - 1));
    }
}

std::string MacroCleanText(const std::string& text) {
    std::string cleaned;
    cleaned.reserve(text.size());
    bool prevWasSpace = false;
    for (char c : text) {
        const bool isSpace = (c == ' ' || c == '\t' || c == '\n' || c == '\r');
        if (isSpace) {
            if (!prevWasSpace && !cleaned.empty()) {
                cleaned.push_back(' ');
                prevWasSpace = true;
            }
        } else {
            cleaned.push_back(c);
            prevWasSpace = false;
        }
    }
    if (!cleaned.empty() && cleaned.back() == ' ') cleaned.pop_back();
    return cleaned;
}

void MacroReloadFromSlot() {
    const int slot = MacroController::GetCurrentSlot();
    g_macroText = MacroController::SerializeSlot(slot, g_macroIncludeBuffers);
    g_macroApplyError.clear();
    g_macroUndoStack.clear();
    g_macroRedoStack.clear();
    g_macroLastSlot = slot;
    g_macroLastInclude = g_macroIncludeBuffers;
    g_macroForceReload = false;
    if (g_macroEditor.active) MacroEnsureEditorBuffer(g_macroText);
}

void MacroMaybeReloadText() {
    if (g_macroEditor.active && !g_macroForceReload) return;
    const int slot = MacroController::GetCurrentSlot();
    if (g_macroForceReload || g_macroLastSlot != slot || g_macroLastInclude != g_macroIncludeBuffers) {
        MacroReloadFromSlot();
    }
}

void MacroSetText(const std::string& text, bool pushUndo) {
    if (pushUndo && text != g_macroText) {
        MacroPushUndo(g_macroText);
        g_macroRedoStack.clear();
    }
    g_macroText = text;
    g_macroApplyError.clear();
    if (g_macroEditor.active) MacroEnsureEditorBuffer(g_macroText);
}

void MacroApplyTextToSlot() {
    const int slot = MacroController::GetCurrentSlot();
    std::string err;
    const std::string cleaned = MacroCleanText(g_macroText);
    if (!MacroController::DeserializeSlot(slot, cleaned, err)) {
        g_macroApplyError = err;
        DirectDrawHook::AddMessage(g_macroApplyError.c_str(), "MACRO", RGB(255, 120, 120), 1400, 0, 120);
        return;
    }

    g_macroApplyError.clear();
    g_macroText = MacroController::SerializeSlot(slot, g_macroIncludeBuffers);
    g_macroUndoStack.clear();
    g_macroRedoStack.clear();
    g_macroLastSlot = slot;
    g_macroLastInclude = g_macroIncludeBuffers;
    g_macroForceReload = false;
    if (g_macroEditor.active) MacroEnsureEditorBuffer(g_macroText);
    DirectDrawHook::AddMessage("Applied macro to slot", "MACRO", RGB(180, 255, 180), 1000, 0, 120);
}

void MacroRecord() {
    MacroController::ToggleRecord();
    DirectDrawHook::AddMessage(MacroController::GetStatusLine().c_str(), "MACRO", RGB(200, 220, 255), 900, 0, 120);
}
void MacroPlay() {
    MacroController::Play();
    DirectDrawHook::AddMessage(MacroController::GetStatusLine().c_str(), "MACRO", RGB(180, 255, 180), 900, 0, 120);
}
void MacroStop() {
    MacroController::Stop();
    DirectDrawHook::AddMessage(MacroController::GetStatusLine().c_str(), "MACRO", RGB(255, 220, 120), 900, 0, 120);
}
void MacroPrevSlot() {
    MacroController::PrevSlot();
    g_macroForceReload = true;
}
void MacroNextSlot() {
    MacroController::NextSlot();
    g_macroForceReload = true;
}

void OnMacroSlotChanged() {
    MacroController::SetCurrentSlot(g_macroSlotMirror);
    g_macroForceReload = true;
}
void OnMacroIncludeBuffers() {
    g_macroForceReload = true;
}

void MacroOpenEditor() {
    MacroMaybeReloadText();
    MacroEnsureEditorBuffer(g_macroText);
    g_macroEditor.active = true;
    g_macroEditor.wantFocus = true;
    Input::ResetEdges();
}

void MacroReloadAction() {
    MacroReloadFromSlot();
    DirectDrawHook::AddMessage("Reloaded macro text from slot", "MACRO", RGB(180, 255, 220), 700, 0, 120);
}

void MacroClearSlot() {
    std::string err;
    const int slot = MacroController::GetCurrentSlot();
    if (MacroController::DeserializeSlot(slot, std::string(), err)) {
        g_macroApplyError.clear();
        g_macroForceReload = true;
        MacroReloadFromSlot();
        DirectDrawHook::AddMessage("Cleared macro slot", "MACRO", RGB(255, 220, 120), 900, 0, 120);
    } else {
        g_macroApplyError = err;
    }
}

void MacroUndo() {
    if (g_macroUndoStack.empty()) return;
    g_macroRedoStack.push_back(g_macroText);
    g_macroText = g_macroUndoStack.back();
    g_macroUndoStack.pop_back();
    g_macroApplyError.clear();
    if (g_macroEditor.active) MacroEnsureEditorBuffer(g_macroText);
}
void MacroRedo() {
    if (g_macroRedoStack.empty()) return;
    g_macroUndoStack.push_back(g_macroText);
    g_macroText = g_macroRedoStack.back();
    g_macroRedoStack.pop_back();
    g_macroApplyError.clear();
    if (g_macroEditor.active) MacroEnsureEditorBuffer(g_macroText);
}
void MacroCopy() {
    MacroMaybeReloadText();
    ImGui::SetClipboardText(g_macroText.c_str());
    DirectDrawHook::AddMessage("Copied macro text", "MACRO", RGB(180, 255, 220), 700, 0, 120);
}
void MacroPaste() {
    const char* clip = ImGui::GetClipboardText();
    if (!clip || !*clip) {
        DirectDrawHook::AddMessage("Clipboard is empty", "MACRO", RGB(255, 220, 120), 700, 0, 120);
        return;
    }
    MacroSetText(std::string(clip), true);
    DirectDrawHook::AddMessage("Pasted macro text", "MACRO", RGB(180, 255, 220), 700, 0, 120);
}
void MacroInsertSample() {
    const char* sample =
        "EFZMACRO 1 "
        "5A 5x3 5B 5x3 5C "
        "6 {3: 6 6 6} 2 {3: 2 2 2} 3 {3: 3 3 3} 5B {3: 5B 5 5}";
    MacroSetText(sample, true);
}

bool MacroUndoDisabled() { return g_macroUndoStack.empty(); }
bool MacroRedoDisabled() { return g_macroRedoStack.empty(); }

const char* MacroStateStr() {
    static char buf[64];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "SLOT %d/%d",
                MacroController::GetCurrentSlot(), MacroController::GetSlotCount());
    return buf;
}

const char* MacroSlotEmptyStr() {
    return MacroController::IsSlotEmpty(MacroController::GetCurrentSlot()) ? "EMPTY" : "HAS DATA";
}

const char* MacroSerializedStr() {
    return g_macroIncludeBuffers ? "BUFFERS" : "NO BUFFERS";
}

const char* MacroStatsStr() {
    static char buf[32];
    const int slot = MacroController::GetCurrentSlot();
    const auto stats = MacroController::GetSlotStats(slot);
    if (!stats.hasData) return "EMPTY";
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%d TICKS", stats.totalTicks);
    return buf;
}

int BuildTextPreviewRows(const std::string& text, char lines[][96], int maxLines) {
    if (maxLines <= 0) return 0;
    if (text.empty()) {
        strncpy_s(lines[0], 96, "(EMPTY)", _TRUNCATE);
        return 1;
    }

    const int maxChars = 84;
    int line = 0;
    int pos = 0;
    lines[line][0] = '\0';

    auto finishLine = [&]() {
        if (line + 1 >= maxLines) return false;
        ++line;
        pos = 0;
        lines[line][0] = '\0';
        return true;
    };

    std::string token;
    for (size_t i = 0; i <= text.size(); ++i) {
        const char c = (i < text.size()) ? text[i] : ' ';
        const bool split = (c == ' ' || c == '\t' || c == '\n' || c == '\r' || i == text.size());
        if (!split) {
            token.push_back(c);
            continue;
        }
        if (token.empty()) continue;
        const int tokenLen = (int)token.size();
        if (pos > 0 && pos + 1 + tokenLen > maxChars) {
            if (!finishLine()) break;
        }
        if (pos > 0 && pos + 1 < 95) {
            strncat_s(lines[line], 96, " ", _TRUNCATE);
            ++pos;
        }
        strncat_s(lines[line], 96, token.c_str(), _TRUNCATE);
        pos += tokenLen;
        token.clear();
    }

    if (line == maxLines - 1 && text.size() > 0) {
        const size_t len = strlen(lines[line]);
        if (len < 92) {
            strncat_s(lines[line], 96, " ...", _TRUNCATE);
        }
    }
    return line + 1;
}

Row* BuildMacroSerializedRows(int& count) {
    static Row s_rows[48];
    static char preview[12][96];
    static char errorLine[128];
    int n = 0;
    MacroMaybeReloadText();

    s_rows[n++] = Header("SERIALIZED MACRO");
    s_rows[n++] = Toggle("INCLUDE BUFFERS", &g_macroIncludeBuffers, OnMacroIncludeBuffers);
    s_rows[n++] = Action("EDIT TEXT",       MacroOpenEditor, MacroSerializedStr);
    s_rows[n++] = Action("APPLY TO SLOT",   MacroApplyTextToSlot);
    s_rows[n++] = Action("RELOAD FROM SLOT", MacroReloadAction);
    s_rows[n++] = Action("CLEAR SLOT",      MacroClearSlot);

    s_rows[n++] = Spacer();
    s_rows[n++] = Header("CLIPBOARD / HISTORY");
    s_rows[n++] = Action("COPY",            MacroCopy);
    s_rows[n++] = Action("PASTE",           MacroPaste);
    s_rows[n++] = Action("UNDO",            MacroUndo, nullptr, MacroUndoDisabled);
    s_rows[n++] = Action("REDO",            MacroRedo, nullptr, MacroRedoDisabled);
    s_rows[n++] = Action("INSERT SAMPLE",   MacroInsertSample);

    if (!g_macroApplyError.empty()) {
        s_rows[n++] = Spacer();
        s_rows[n++] = Header("ERROR");
        _snprintf_s(errorLine, sizeof(errorLine), _TRUNCATE, "%s", g_macroApplyError.c_str());
        s_rows[n++] = Info(errorLine);
    }

    s_rows[n++] = Spacer();
    s_rows[n++] = Header("PREVIEW");
    const int previewCount = BuildTextPreviewRows(g_macroText, preview, 12);
    for (int i = 0; i < previewCount && n < 47; ++i) {
        s_rows[n++] = Info(preview[i]);
    }

    count = n;
    return s_rows;
}

Row* BuildMacroStatsRows(int& count) {
    static Row s_rows[24];
    static char status[128];
    static char slot[64];
    static char spans[64];
    static char ticks[96];
    static char effective[64];
    static char firstButton[64];
    static char buffers[96];
    static char indexes[96];
    int n = 0;
    const int curSlot = MacroController::GetCurrentSlot();
    const auto stats = MacroController::GetSlotStats(curSlot);
    const int effectiveTicks = MacroController::GetEffectiveTicks(curSlot);
    const int firstButtonTick = MacroController::GetFirstButtonTick(curSlot);

    _snprintf_s(status, sizeof(status), _TRUNCATE, "State: %s", MacroController::GetStatusLine().c_str());
    _snprintf_s(slot, sizeof(slot), _TRUNCATE, "Current slot: %d / %d (%s)",
                curSlot, MacroController::GetSlotCount(), stats.hasData ? "has data" : "empty");
    _snprintf_s(spans, sizeof(spans), _TRUNCATE, "Spans: %d", stats.spanCount);
    _snprintf_s(ticks, sizeof(ticks), _TRUNCATE, "Total ticks: %d (~%.2f sec)",
                stats.totalTicks, stats.totalTicks / 64.0f);
    _snprintf_s(effective, sizeof(effective), _TRUNCATE, "Effective ticks: %d", effectiveTicks);
    _snprintf_s(firstButton, sizeof(firstButton), _TRUNCATE,
                firstButtonTick >= 0 ? "First button tick: %d" : "First button tick: none",
                firstButtonTick);
    _snprintf_s(buffers, sizeof(buffers), _TRUNCATE, "Buffer entries: %d  Buffer ticks: %d",
                stats.bufEntries, stats.bufTicks);
    _snprintf_s(indexes, sizeof(indexes), _TRUNCATE, "Buffer idx: %u -> %u  Samples: %d",
                (unsigned)stats.bufStartIdx, (unsigned)stats.bufEndIdx, stats.bufIndexTicks);

    s_rows[n++] = Header("SLOT STATUS");
    s_rows[n++] = Info(status);
    s_rows[n++] = Info(slot);
    s_rows[n++] = Spacer();
    s_rows[n++] = Header("TIMING");
    s_rows[n++] = Info(spans);
    s_rows[n++] = Info(ticks);
    s_rows[n++] = Info(effective);
    s_rows[n++] = Info(firstButton);
    s_rows[n++] = Spacer();
    s_rows[n++] = Header("BUFFER CAPTURE");
    s_rows[n++] = Info(buffers);
    s_rows[n++] = Info(indexes);
    count = n;
    return s_rows;
}

Row* BuildMacrosRows(int& count) {
    static Row s_rows[24];
    int n = 0;
    MacroMaybeReloadText();
    g_macroSlotMirror = MacroController::GetCurrentSlot();

    static char status[128];
    _snprintf_s(status, sizeof(status), _TRUNCATE, "State: %s", MacroController::GetStatusLine().c_str());

    s_rows[n++] = Header("MACRO CONTROLLER");
    s_rows[n++] = Info(status);
    s_rows[n++] = IntNum("CURRENT SLOT", &g_macroSlotMirror, 1, MacroController::GetSlotCount(),
                         1, 1, OnMacroSlotChanged);
    s_rows[n++] = Action("RECORD (TOGGLE)", MacroRecord, MacroStateStr);
    s_rows[n++] = Action("PLAY",            MacroPlay,   MacroSlotEmptyStr);
    s_rows[n++] = Action("STOP",            MacroStop);
    s_rows[n++] = Action("PREV SLOT",       MacroPrevSlot);
    s_rows[n++] = Action("NEXT SLOT",       MacroNextSlot);

    s_rows[n++] = Spacer();
    s_rows[n++] = Header("MACRO TOOLS");
    s_rows[n++] = Submenu("SERIALIZED MACRO", "SERIALIZED MACRO", BuildMacroSerializedRows, MacroSerializedStr);
    s_rows[n++] = Submenu("SLOT STATS",       "SLOT STATS",       BuildMacroStatsRows,      MacroStatsStr);
    s_rows[n++] = Action("GUIDE",            NavToMacroHelp);
    count = n;
    return s_rows;
}

bool IsTextEditorActive() {
    return g_macroEditor.active;
}

void ResetTextEditor() {
    g_macroEditor.active = false;
    g_macroEditor.wantFocus = false;
}

bool TickMacroTextEditorIfActive(ImDrawList*, const ScreenLayout& layout) {
    if (!g_macroEditor.active) return false;

    const float x = layout.panelX + 38.0f;
    const float y = layout.contentTopY + 8.0f;
    const float w = Theme::kPanelW - 76.0f;
    const float h = layout.contentBottomY - y - 8.0f;

    ImGui::SetNextWindowPos(ImVec2(x, y), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 0.94f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1.0f, 1.0f, 1.0f, 0.86f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.04f, 0.04f, 0.04f, 0.96f));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.12f, 0.12f, 0.12f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.22f, 0.22f, 0.22f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.35f, 0.35f, 0.35f, 1.0f));

    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings;
    if (ImGui::Begin("MACRO TEXT EDITOR", nullptr, flags)) {
        ImGui::Text("Slot %d / %d", MacroController::GetCurrentSlot(), MacroController::GetSlotCount());
        ImGui::SameLine();
        ImGui::TextDisabled(g_macroIncludeBuffers ? "with buffers" : "without buffers");
        if (!g_macroApplyError.empty()) {
            ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "Error: %s", g_macroApplyError.c_str());
        } else {
            ImGui::TextDisabled("Use EFZMACRO text notation. Apply normalizes whitespace.");
        }

        const float buttonH = 28.0f;
        const float editorH = (std::max)(100.0f, ImGui::GetContentRegionAvail().y - buttonH - 10.0f);
        if (g_macroEditor.wantFocus) {
            ImGui::SetKeyboardFocusHere();
            g_macroEditor.wantFocus = false;
        }
        if (ImGui::InputTextMultiline("##macro_text_editor",
                                      g_macroEditor.buffer.data(),
                                      g_macroEditor.buffer.size(),
                                      ImVec2(-1.0f, editorH),
                                      ImGuiInputTextFlags_AllowTabInput)) {
            const std::string newText = g_macroEditor.buffer.data();
            if (newText != g_macroText) {
                MacroPushUndo(g_macroText);
                g_macroRedoStack.clear();
                g_macroText = newText;
                g_macroApplyError.clear();
            }
        }

        if (ImGui::Button("Apply")) {
            g_macroText = g_macroEditor.buffer.data();
            MacroApplyTextToSlot();
        }
        ImGui::SameLine();
        if (ImGui::Button("Done")) {
            g_macroText = g_macroEditor.buffer.data();
            g_macroEditor.active = false;
            Input::ResetEdges();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            MacroReloadFromSlot();
            g_macroEditor.active = false;
            Input::ResetEdges();
        }
        ImGui::SameLine();
        if (ImGui::Button("Reload")) {
            MacroReloadFromSlot();
            g_macroEditor.wantFocus = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Sample")) {
            MacroInsertSample();
            g_macroEditor.wantFocus = true;
        }

        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            g_macroText = g_macroEditor.buffer.data();
            g_macroEditor.active = false;
            Input::ResetEdges();
        }
    }
    ImGui::End();

    ImGui::PopStyleColor(7);
    ImGui::PopStyleVar(2);
    return true;
}

void LogMirrorStepSeh(unsigned code, const char* name) {
    char buf[160];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE,
        "[CUSTOM_MENU][TRACE] SEH 0x%08X in %s",
        code, name ? name : "(unknown mirror)");
    LogOut(buf, true);
}

void LogSlowSecondaryMirrorRefresh(DWORD elapsed) {
    char buf[160];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE,
        "[CUSTOM_MENU][TIMING] Slow secondary mirror refresh: %lums",
        static_cast<unsigned long>(elapsed));
    LogOut(buf, true);
}

static bool SehRefreshMirrorStep(const char* name, void (*fn)()) {
    __try {
        fn();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LogMirrorStepSeh((unsigned)GetExceptionCode(), name);
        return false;
    }
}

void RefreshSecondaryScreenMirrors() {
    // Wrap each step in SEH so a single bad refresher (e.g. one walking
    // game memory while characters are mid-init) cannot kill the whole
    // menu render path.
    struct Step { const char* name; void (*fn)(); };
    static const Step kFastSteps[] = {
        { "RefreshAutoMirrors",        &RefreshAutoMirrors        },
        { "RefreshCharMirrors",        &RefreshCharMirrors        },
        { "RefreshOpponentMirrors",    &RefreshOpponentMirrors    },
        { "RefreshOptionsMirrors",     &RefreshOptionsMirrors     },
    };
    static const Step kSlowSteps[] = {
        { "RefreshHelpStrings",        &RefreshHelpStrings        },
        { "RefreshHotkeyStrings",      &RefreshHotkeyStrings      },
        { "RefreshMacroSlotChoices",   &RefreshMacroSlotChoices   },
        { "RefreshDebugMirrors",       &RefreshDebugMirrors       },
        { "RefreshCrMirrors",          &RefreshCrMirrors          },
        { "RefreshEngineRegenMirrors", &RefreshEngineRegenMirrors },
        { "RefreshFramestepMirror",    &RefreshFramestepMirror    },
    };

    for (const Step& s : kFastSteps) {
        SehRefreshMirrorStep(s.name, s.fn);
    }

    constexpr DWORD kSlowMirrorRefreshMs = 250;
    static DWORD s_lastSlowMirrorRefresh = 0;

    const DWORD now = GetTickCount();
    if (s_lastSlowMirrorRefresh != 0 && (now - s_lastSlowMirrorRefresh) < kSlowMirrorRefreshMs) {
        return;
    }
    s_lastSlowMirrorRefresh = now;

    const DWORD slowStart = GetTickCount();
    for (const Step& s : kSlowSteps) {
        const DWORD stepStart = GetTickCount();
        SehRefreshMirrorStep(s.name, s.fn);
        const DWORD stepElapsed = GetTickCount() - stepStart;
        // Log any slow individual step so we can pinpoint future hangs in one repro.
        // Threshold deliberately low (>=20ms) since the whole loop runs throttled to 250ms.
        if (stepElapsed >= 20) {
            char stepBuf[160];
            _snprintf_s(stepBuf, sizeof(stepBuf), _TRUNCATE,
                "[CUSTOM_MENU][TIMING] step=%s took=%lums",
                s.name ? s.name : "(unknown)",
                static_cast<unsigned long>(stepElapsed));
            LogOut(stepBuf, true);
        }
    }

    const DWORD elapsed = GetTickCount() - slowStart;
    static DWORD s_lastSlowMirrorLog = 0;
    if (elapsed >= 50 && (s_lastSlowMirrorLog == 0 || (now - s_lastSlowMirrorLog) >= 1000)) {
        s_lastSlowMirrorLog = now;
        LogSlowSecondaryMirrorRefresh(elapsed);
    }
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
void TickMenu(ImDrawList* dl, const ScreenLayout& layout, int& focus, ScrollState& scroll, bool& backEdge) {
    int n = 0; Row* rows = BuildMenuRows(n);
    TickListScreen(dl, layout, "MENU", rows, n, focus, scroll, backEdge);
}
void TickValues(ImDrawList* dl, const ScreenLayout& layout, int& focus, ScrollState& scroll, bool& backEdge) {
    int n = 0; Row* rows = BuildValuesRootRows(n);
    TickListScreen(dl, layout, "VALUES", rows, n, focus, scroll, backEdge);
}

bool CrRowHidden(int player, int row) { return CrRowHiddenImpl(player, row); }
const char* CrRowLabel(int row) { return CrRowLabelImpl(row); }
void CrFormatCell(int player, int row, char* buf, size_t bufSz) { CrFormatCellImpl(player, row, buf, bufSz); }
void CrAdjustCell(int player, int row, int direction, bool bigStep) { CrAdjustCellImpl(player, row, direction, bigStep); }
void CrActivateCell(int player, int row) { CrActivateCellImpl(player, row); }
void ResetContinuousRecoveryEditorState() {}
void CorrectValueLocksForEngineRegenUi(GuiValueLocks::State& locks) { CorrectValueLocksForEngineRegenUiImpl(locks); }

// ===== AUTO sub-panes =====
void TickTriggers(ImDrawList* dl, const ScreenLayout& layout, int& focus, ScrollState& scroll, bool& backEdge) {
    int n = 0; Row* rows = BuildTriggersRows(n);
    TickListScreen(dl, layout, "TRIGGERS", rows, n, focus, scroll, backEdge);
}
void TickMacros(ImDrawList* dl, const ScreenLayout& layout, int& focus, ScrollState& scroll, bool& backEdge) {
    int n = 0; Row* rows = BuildMacrosRows(n);
    if (IsTextEditorActive()) {
        ClampFocus(rows, n, focus);
        backEdge = false;
        RenderList(dl, layout, "MACROS", rows, n, focus, scroll);
        TickMacroTextEditorIfActive(dl, layout);
        return;
    }
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
    if (IsManualKeybindEditorActive()) {
        ClampFocus(rows, n, focus);
        backEdge = false;
        RenderList(dl, layout, "HOTKEYS", rows, n, focus, scroll);
        TickManualKeybindEditorIfActive(dl, layout);
        return;
    }
    TickListScreen(dl, layout, "HOTKEYS", rows, n, focus, scroll, backEdge);
}
void TickSettingsDebug(ImDrawList* dl, const ScreenLayout& layout, int& focus, ScrollState& scroll, bool& backEdge) {
    // Throttle the runtime poll: SafeReadMemory, GetModuleHandleA, RF freeze
    // queries, and string formatting all happen on the render thread and only
    // need to feel "live" - 100 ms is well below human perception while
    // dramatically cheaper than per-frame.
    constexpr DWORD kDebugRuntimeMirrorRefreshMs = 100;
    static DWORD s_lastDebugRuntimeRefresh = 0;
    const DWORD now = GetTickCount();
    if (s_lastDebugRuntimeRefresh == 0 || (now - s_lastDebugRuntimeRefresh) >= kDebugRuntimeMirrorRefreshMs) {
        s_lastDebugRuntimeRefresh = now;
        RefreshDebugRuntimeMirrors();
    }
    int n = 0; Row* rows = BuildSettingsDebugRows(n);
    TickListScreen(dl, layout, "DEBUG", rows, n, focus, scroll, backEdge);
}

// ===== HELP sub-panes =====
void TickHelpStart(ImDrawList* dl, const ScreenLayout& layout, int& focus, ScrollState& scroll, bool& backEdge) {
    int n = 0; Row* rows = BuildHelpStartRows(n);
    TickListScreen(dl, layout, "START", rows, n, focus, scroll, backEdge);
}
void TickHelpGuide(ImDrawList* dl, const ScreenLayout& layout, int& focus, ScrollState& scroll, bool& backEdge) {
    int n = 0; Row* rows = BuildHelpGuideRows(n);
    TickListScreen(dl, layout, "GUIDE", rows, n, focus, scroll, backEdge);
}
void TickHelpResources(ImDrawList* dl, const ScreenLayout& layout, int& focus, ScrollState& scroll, bool& backEdge) {
    int n = 0; Row* rows = BuildHelpResourcesRows(n);
    TickListScreen(dl, layout, "RESOURCES", rows, n, focus, scroll, backEdge);
}
void TickHelpAbout(ImDrawList* dl, const ScreenLayout& layout, int& focus, ScrollState& scroll, bool& backEdge) {
    int n = 0; Row* rows = BuildHelpAboutRows(n);
    TickListScreen(dl, layout, "ABOUT", rows, n, focus, scroll, backEdge);
}

} // namespace CustomMenu::Screens
