#include "../../../include/game/mission/mission_engine.h"
#include "../../../include/game/mission/mission_data.h"
#include "../../../include/game/mission/mission_moves.h"
#include "../../../include/game/mission/mission_movedata.h"
#include "../../../include/game/mission/mission_sequence_policy.h"
#include "../../../include/game/mission/mission_setup.h"
#include "../../../include/game/mission/move_notation_tables.h"  // per-char notation (generated)
#include "../../../include/game/character_settings.h"            // GetCharacterInternalName
#include "../../../include/game/macro_controller.h"

#include "../../../include/core/constants.h"
#include "../../../include/core/logger.h"
#include "../../../include/core/memory.h"
#include "../../../include/utils/utilities.h"
#include "../../../include/gui/overlay.h"
#include "../../../include/gui/gui.h"                 // OpenMenu (mission mode)
#include "../../../include/gui/imgui_impl.h"           // menu visibility (baseline save gate)
#include "../../../include/game/savestate_hook.h"      // TrialMode-style auto retry
#include "../../../include/game/mission/mission_state_dump.h" // embedded start states
#include "../../../include/game/character_hotswap.h"   // char-select auto-drive
#include "../../../include/gui/custom_menu/screens.h" // OpenMissionBrowser
#include "../../../include/game/practice_menu/mission_title_screen.h"
#include "../../../include/game/game_state.h"
#include "../../../include/game/auto_action.h"
#include "../../../include/input/injection_control.h"
#include "../../../include/input/input_hook.h"
#include "../../../include/utils/config.h"
#include "../../../include/utils/pause_integration.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

namespace Mission::Engine {

namespace {

Snapshot g_snapshot;
std::atomic<bool> g_inspector{false};
std::atomic<bool> g_pendingMissionMode{false}; // title MISSION -> auto-open browser
std::atomic<bool> g_pendingRecordMode{false};  // title RECORD -> auto-arm recorder
std::atomic<bool> g_pendingRecordSawCharacterSelect{false};
std::mutex  g_pendingLoadMx;                   // guards g_pendingLoadPath
std::string g_pendingLoadPath;                 // title mission pick -> load on match
// Pre-parsed setup of the pending mission. The preferred route constructs both
// fighters on EFZ's Loading thread and skips Character Select; these fields are
// retained for the guarded selector fallback.
std::atomic<bool> g_pendingHaveChars{false};
std::atomic<bool> g_pendingCsQueued{false};
std::atomic<int>  g_pendingP1SelectId{-1};
std::atomic<int>  g_pendingP2SelectId{-1};
std::atomic<int>  g_pendingP1Palette{0};
std::atomic<int>  g_pendingP2Palette{0};
std::atomic<int>  g_pendingStage{-1};
std::atomic<int>  g_pendingBgm{-1};

// ---- recorder state ----
struct CapturedStep {
    short       moveId = 0;
    bool        landed = false;
    std::string notation;  // auto-derived from the trigger input (author fixes specials)
    int         req = 0;   // ::Mission::StepReq value (default from `landed`)
    bool        optional = false;
    short       cmdToken = 99; // engine command index that produced this move (99 = none/normal)
    int         hits = 0;      // combo hits this step produced (multi-hit -> StepReq::Hits)
    int         startFrame = 0;// recorder frame the move began (delay measurement)
    int         maxDelay = 0;  // measured land delay for projectile steps (0 = immediate)
    int         gap = 0;       // non-frozen frames between the previous step's last
                               // event and this move (delayed-button allowance)
    bool        comboEndAfter = false; // a recorded combo alive->dead edge occurred
                                       // after this step and before the next one
    bool        connected = false; // made CONTACT (hit or blocked) via +0x168 edge -
                                   // separates deliberate whiffs from blocked hits
};
std::atomic<bool> g_recActive{false};
using RecorderPhase = ::Mission::Engine::Recorder::Phase;
enum class RecorderCommand : int { None, Arm, Advance, Retake, Cancel };
std::atomic<RecorderPhase> g_recPhase{RecorderPhase::Idle};
std::atomic<RecorderCommand> g_recCommand{RecorderCommand::None};
std::atomic<int> g_recStepCountView{0};
std::atomic<int> g_recComboEndCountView{0};
std::atomic<int> g_recCountInView{0};
std::atomic<int> g_recBannerId{-1};
int g_recReleaseFrames = 0;
int g_recCountInFrames = 0;
std::vector<CapturedStep> g_recSteps;   // kept after stop until the next start / save
short g_recLastAttackId = 0;   // last attack move-ID recorded (0 = none / reset)
int   g_recComboAtStart = 0;   // combo count when the current step's move began
short g_recRecentToken = 99;   // most recent non-99 motion token (command index), for pairing
int   g_recRecentTokenAge = 999; // frames since g_recRecentToken was seen
int   g_recFrame = 0;          // recorder EFFECTIVE frame counter (freeze frames excluded,
                               // so measured gaps/delays match what the runner will time)
int   g_recLastEventFrame = 0; // effective frame of the last step event (start or hit)
int   g_recPrevCombo = 0;      // previous frame's combo count (rise detection)
// A continuous recording is one authored take. Combo death is retained as an
// explicit setup/okizeme boundary when another step follows; it is never used
// as a destructive "probably a retry" timeout. Explicit savestate/Retake flow
// is the attempt boundary.
bool  g_recAnyLanded = false;  // this attempt landed at least one hit
bool  g_recComboWasAlive = false;
int   g_recPendingComboEndStep = -1; // candidate committed only if a later step exists
int   g_recComboEndFrame = 0;        // gap anchor: actual recovery edge, not last hit
int   g_recPrevHitState = 0;   // previous frame +0x168 (contact edge detection)
short g_recPrevMove = 0;       // same-ID re-entry detection (5A -> 5A, etc.)
short g_recPrevFrameIdx = 0;
::Mission::Mission g_recSetup; // setup metadata snapshot taken at record start

// Derive a numpad+button notation from P1's current input (facing-relative:
// 6=forward, 4=back). Movement digit 1-9, then any held attack buttons (A-D).
// Good for normals ("2B","5A","6C"); specials get a rough guess to fix in the editor.
std::string AutoNotation() {
    const uint8_t in = GetPlayerInputs(1);
    const bool facingRight = GetPlayerFacingDirection(1);
    int h = 0, v = 0;
    if (in & INPUT_RIGHT) h = facingRight ? +1 : -1;
    if (in & INPUT_LEFT)  h = facingRight ? -1 : +1;
    if (in & INPUT_UP)    v = +1;
    if (in & INPUT_DOWN)  v = -1;
    const int numpad = 5 + h + 3 * v;   // 1..9
    std::string s(1, static_cast<char>('0' + numpad));
    if (in & INPUT_A) s.push_back('A');
    if (in & INPUT_B) s.push_back('B');
    if (in & INPUT_C) s.push_back('C');
    if (in & INPUT_D) s.push_back('D');
    return s;
}

// ---- runner (validation) state ----
using RunnerPhase = ::Mission::Engine::Runner::Phase;
std::atomic<bool> g_runActive{false};
::Mission::Mission g_runMission;
std::recursive_mutex g_runStateMx;
RunnerPhase g_runPhase = RunnerPhase::Idle;
int  g_runStep = 0;         // steps satisfied so far
std::atomic<int> g_runFailedStep{-1}; // failed recipe index; survives the retry reset
int  g_runBaseline = 0;     // combo count at the last satisfied step
int  g_runAttempts = 0;
::Mission::SequencePolicy::DelayedActionWindow g_runGapWindow;
int  g_runBestTier = -1;
// Two-phase step matching: a step is ARMED when its move is performed (a NEW
// instance - move-ID edge or frame-index reset), then SATISFIED when its
// hit requirement is met. The arm persists after the move ends, which is what
// makes delayed projectile hits land correctly (the caster is idle by then).
bool g_runArmed = false;
int  g_runArmedFrames = 0;  // frames since arm (checked against step maxDelay)
short g_runPrevMove = 0;    // previous frame move-ID (edge detection)
short g_runPrevFrameIdx = 0;// previous frame index (same-move re-entry detection)
bool g_runComboWasAlive = false; // combo alive last frame (drop = alive->dead edge)
int  g_runPrevComboCount = 0;    // detects sampled N->1 wake-up rollover
uint8_t g_runPrevInputs = 0;     // sampled attack-button edge for deadline grace
bool g_runAwaitingComboEnd = false; // current recipe requires an authored combo.end
int  g_runPrevHitState = 0;      // previous +0x168 (contact edge for Connect steps)
bool g_runConnectSeen = false;   // armed step made contact (hit or blocked)
// Score across explicit combo parts. EFZ's live combo fields reset at recovery,
// so retain each segment's maxima before consuming combo.end.
::Mission::SequencePolicy::SegmentScore g_runScore;
// TrialMode-style retry: on mission load/reset, SAVE (Revival savestate) once the
// first unpaused gameplay frame settles; every DROP auto-LOADs it, snapping the
// match back to the mission's start state for the next attempt.
bool g_runSavePending = false;   // a baseline save is owed
bool g_runStateSaved = false;    // baseline exists -> drops may auto-load
int  g_runSaveSettle = 0;        // consecutive eligible ticks before saving
bool g_runSelfLoad = false;      // a TriggerLoad WE issued is in flight (so the
                                 // savestate-load notification can tell a manual
                                 // load from our own auto-retry)
// Embedded start state (Mission.savestate): restore is deferred until the
// match settles after the mission's hotswap; the restored buffer then doubles
// as the auto-retry baseline (no separate baseline save needed).
bool g_runRestorePending = false;
int  g_runRestoreSettle = 0;
int  g_runRestoreTimeout = 0;   // stuck-pending safety (falls back to values)
std::atomic<bool> g_runStartupInputHeld{false};

// Demonstration session. Preparation/restoration is frame-thread-owned; the
// public frontend only queues a request by changing this phase.
using DemoPhase = ::Mission::Engine::Demo::Phase;
std::atomic<DemoPhase> g_demoPhase{DemoPhase::Idle};
std::atomic<bool> g_demoCancelRequested{false};
bool g_demoBaselineIssued = false;
int  g_demoSettleFrames = 0;
bool g_demoFinishedThisTick = false;
std::string g_demoText;
bool g_demoFromRecorder = false;

std::string MacroRecordBindingLabel() {
    const auto& cfg = Config::GetSettings();
    // Match input_handler's resolved keyboard behavior: non-positive config
    // values currently fall back to the default I key.
    const int key = cfg.macroRecordKey > 0 ? cfg.macroRecordKey : 'I';
    std::string label = "KEY " + Config::GetKeyName(key);
    if (cfg.gpMacroRecordButton >= 0) {
        label += " / PAD " + Config::GetGamepadButtonName(cfg.gpMacroRecordButton);
    }
    return label;
}

void RemoveRecorderBanner() {
    const int id = g_recBannerId.exchange(-1, std::memory_order_acq_rel);
    if (id != -1) DirectDrawHook::RemovePermanentMessage(id);
}

void RefreshRecorderBanner() {
    const RecorderPhase phase = g_recPhase.load(std::memory_order_acquire);
    if (phase == RecorderPhase::Idle || Demo::IsActive() ||
        GetCurrentGamePhase() != GamePhase::Match) {
        RemoveRecorderBanner();
        return;
    }

    const std::string binding = MacroRecordBindingLabel();
    const int steps = g_recStepCountView.load(std::memory_order_acquire);
    const int breaks = g_recComboEndCountView.load(std::memory_order_acquire);
    char text[256];
    COLORREF color = RGB(255, 230, 120);
    switch (phase) {
        case RecorderPhase::PreRecord:
            _snprintf_s(text, sizeof(text), _TRUNCATE,
                        "MISSION PRE-RECORD | arrange the start | %s = start countdown",
                        binding.c_str());
            break;
        case RecorderPhase::CountIn: {
            const int count = g_recCountInView.load(std::memory_order_acquire);
            _snprintf_s(text, sizeof(text), _TRUNCATE,
                        "MISSION COUNT-IN %d | release all controls | %s = cancel",
                        count > 0 ? count : 3,
                        binding.c_str());
            break;
        }
        case RecorderPhase::Recording: {
            color = RGB(255, 100, 100);
            const int seconds = (std::max)(0, g_recFrame / 192);
            _snprintf_s(text, sizeof(text), _TRUNCATE,
                        "MISSION RECORDING %02d:%02d | %d action%s, %d setup break%s | %s = stop & review",
                        seconds / 60, seconds % 60, steps, steps == 1 ? "" : "s",
                        breaks, breaks == 1 ? "" : "s", binding.c_str());
            break;
        }
        case RecorderPhase::Review:
            color = RGB(180, 230, 255);
            _snprintf_s(text, sizeof(text), _TRUNCATE,
                        "MISSION REVIEW | %d action%s, %d setup break%s | open menu to preview, save, retake, or discard",
                        steps, steps == 1 ? "" : "s", breaks,
                        breaks == 1 ? "" : "s");
            break;
        case RecorderPhase::Idle:
        default:
            RemoveRecorderBanner();
            return;
    }

    int id = g_recBannerId.load(std::memory_order_acquire);
    if (id == -1) {
        const int created = DirectDrawHook::AddPermanentMessage(text, color, 20, 60);
        int expected = -1;
        if (!g_recBannerId.compare_exchange_strong(expected, created,
                                                    std::memory_order_acq_rel)) {
            DirectDrawHook::RemovePermanentMessage(created);
            id = expected;
        } else {
            id = created;
        }
    }
    if (id != -1) DirectDrawHook::UpdatePermanentMessage(id, text, color);
}

bool StepMatches(const ::Mission::Step& st, short moveId) {
    for (int id : st.moveIds) if (id == static_cast<int>(moveId)) return true;
    return false;
}

short ReadMove(uintptr_t base) {
    short v = 0;
    if (base) SafeReadMemory(base + MOVE_ID_OFFSET, &v, sizeof(v));
    return v;
}
int ReadInt(uintptr_t base, uintptr_t off) {
    int v = 0;
    if (base) SafeReadMemory(base + off, &v, sizeof(v));
    return v;
}
short ReadShort(uintptr_t base, uintptr_t off) {
    short v = 0;
    if (base) SafeReadMemory(base + off, &v, sizeof(v));
    return v;
}

// ---- round-intro control ----
// Round-start fields inside the game state object - the same fields
// custom_savestate normalizes in NormalizeInitialSnapshotRoundStartState (see
// shared_documentation/REVIVAL_SAVESTATE_ARCHITECTURE.md, region 5). Revival's
// savestate covers this whole object, so a baseline saved mid-intro would
// replay the intro on every retry load; trials instead force the active round.
constexpr uintptr_t kRoundEventOff      = 4944;   // u8  nonzero while a round event (intro/outro) runs
constexpr uintptr_t kRoundDurationOff   = 4946;   // u16 0 during the intro, 300 in the active round
constexpr uintptr_t kRoundGateOff       = 4948;   // u32 round event gate
constexpr uintptr_t kRoundUiCounterOff  = 82444;  // u16 "Ready?/Fight" UI counters
constexpr uintptr_t kRoundUiCounter2Off = 82446;  // u16
constexpr uint16_t  kActiveRoundDuration = 300;

uintptr_t GameStatePtr() {
    const uintptr_t base = GetEFZBase();
    if (!base) return 0;
    uintptr_t gs = 0;
    if (!SafeReadMemory(base + EFZ_BASE_OFFSET_GAME_STATE, &gs, sizeof(gs))) return 0;
    return gs;
}

bool RoundIntroActive() {
    const uintptr_t gs = GameStatePtr();
    if (!gs) return false;
    uint8_t ev = 0; uint16_t dur = 0;
    if (!SafeReadMemory(gs + kRoundEventOff, &ev, sizeof(ev)) ||
        !SafeReadMemory(gs + kRoundDurationOff, &dur, sizeof(dur))) return false;
    return ev != 0 || dur == 0;
}

// Force the round into its active state (what loading a mid-intro custom
// savestate does after normalization): trials never sit through "Ready/Fight".
void SkipRoundIntro(const char* why) {
    const uintptr_t gs = GameStatePtr();
    if (!gs || !RoundIntroActive()) return;
    const uint8_t  ev = 0;
    const uint16_t dur = kActiveRoundDuration;
    const uint32_t gate = 0;
    const uint16_t ui = 0;
    SafeWriteMemory(gs + kRoundEventOff, &ev, sizeof(ev));
    SafeWriteMemory(gs + kRoundDurationOff, &dur, sizeof(dur));
    SafeWriteMemory(gs + kRoundGateOff, &gate, sizeof(gate));
    SafeWriteMemory(gs + kRoundUiCounterOff, &ui, sizeof(ui));
    SafeWriteMemory(gs + kRoundUiCounter2Off, &ui, sizeof(ui));
    LogOut(std::string("[MISSION] round intro skipped (") + why + ")", true);
}

// Active P1 character ID (drives which movedata/<char>.json is queried).
int P1Char() { return displayData.p1CharID; }

// One-line classification of a move-ID for the inspector.
const char* ClassifyMove(short m) {
    if (IsAttackMove(m))  return "ATTACK";
    if (IsHitstun(m))     return "HITSTUN";
    if (IsLaunched(m))    return "LAUNCH";
    if (IsBlockstun(m))   return "BLOCK";
    if (IsRecoilGuard(m)) return "RG";
    if (IsAirtech(m))     return "AIRTECH";
    if (IsGroundtech(m))  return "GNDTECH";
    if (IsDashState(m))   return "DASH";
    if (IsActionable(m))  return "NEUTRAL";
    return "?";
}

void DrawInspector(const Snapshot& s) {
    // Prefer the per-character generated table (252->"41236A" etc.); fall back to
    // the universal normals/system table. Empty string = a known followup whose
    // notation isn't filled yet -> fall through to universal / show move-id only.
    const std::string p1Internal = CharacterSettings::GetCharacterInternalName(P1Char());
    const char* p1Hdr = ::Mission::MoveNames::Lookup(p1Internal.c_str(), s.p1Move);
    const char* p1Note = (p1Hdr && p1Hdr[0]) ? p1Hdr : ::Mission::Moves::Notation(s.p1Move);
    // .pat class for the live P1 move (e.g. "special"/"A"/"projectile") — the
    // authoring aid for mapping a move-ID to its strength/role at a glance.
    const int   p1c    = P1Char();
    const char* p1Pat  = ::Mission::MoveData::ClassName(::Mission::MoveData::GetClass(p1c, s.p1Move));
    // The command index is set for only one frame on recognition, so hold the last
    // non-99 value briefly to make it readable while mapping (facing-independent).
    static short s_lastTok = 99; static int s_tokTtl = 0;
    if (s.p1Token != 99) { s_lastTok = s.p1Token; s_tokTtl = 90; }
    else if (s_tokTtl > 0) { --s_tokTtl; }
    const std::string tokStr = (s_tokTtl > 0)
        ? (std::string(" cmd=") + std::to_string(s_lastTok)) : std::string();
    char buf[288];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE,
        "MISSION INSPECTOR  P1 move=%d [%s]%s%s%s  combo=%d dmg=%d   |   P2 move=%d [%s]%s",
        static_cast<int>(s.p1Move), ClassifyMove(s.p1Move),
        (p1Pat && p1Pat[0]) ? (std::string(" pat=") + p1Pat).c_str() : "",
        (p1Note && p1Note[0]) ? (std::string(" ") + p1Note).c_str() : "",
        tokStr.c_str(),
        s.p1Combo, s.p1ComboDamage,
        static_cast<int>(s.p2Move), ClassifyMove(s.p2Move), s.p2InStun ? " <in combo>" : "");
    // Refreshed each frame (category replace); short duration so it fades if the
    // inspector is turned off or the match ends.
    DirectDrawHook::AddMessage(buf, "mission_inspector", RGB(255, 235, 120), 500, 20, 44);
}

// Capture the ordered attack-move sequence. A new distinct attack (after leaving
// the previous one) starts a new step; a combo-count rise while that move is
// active marks the step landed.
// Preferred notation for a captured move: the per-character generated table
// (623C, 236A (Fire), ...), then the universal table, then the input guess.
std::string CaptureNotation(short moveId) {
    const std::string internal = CharacterSettings::GetCharacterInternalName(P1Char());
    const char* hdr = ::Mission::MoveNames::Lookup(internal.c_str(), moveId);
    if (hdr && hdr[0]) return hdr;
    const char* tn = ::Mission::Moves::Notation(moveId);
    if (tn && tn[0]) return tn;
    return AutoNotation();
}

int LastRecordedLandedStep() {
    for (int i = static_cast<int>(g_recSteps.size()) - 1; i >= 0; --i) {
        if (g_recSteps[i].landed) return i;
    }
    return -1;
}

bool CommitPendingRecordedComboEnd(const char* reason) {
    if (g_recPendingComboEndStep < 0 ||
        g_recPendingComboEndStep >= static_cast<int>(g_recSteps.size())) {
        return false;
    }
    if (!g_recSteps[g_recPendingComboEndStep].comboEndAfter) {
        g_recSteps[g_recPendingComboEndStep].comboEndAfter = true;
        g_recComboEndCountView.fetch_add(1, std::memory_order_release);
    }
    LogOut("[MISSION][REC] committed combo.end after step=" +
           std::to_string(g_recPendingComboEndStep) + " reason=" +
           (reason ? reason : "unknown"), true);
    g_recPendingComboEndStep = -1;
    return true;
}

void RecorderCapture(const Snapshot& s, bool advanceLogicalTick) {
    const short cur = s.p1Move;
    // Observe moves/contact on every 192 Hz monitor pass so short-lived move IDs
    // and +0x168 contact pulses cannot fall between macro samples.  Only advance
    // timing on the macro stream's authoritative 64 Hz tick; one such tick is
    // three EFZ internal ticks, which is also the unit used by mission maxGap /
    // maxDelay and the runner's timers.
    const bool advanceTime = advanceLogicalTick && !s.freezeActive;
    if (advanceTime) g_recFrame += 3;
    // Track the most recent command index (motion token). It is set only on the
    // recognition frame, a frame or two before the move-ID settles, so we remember
    // it briefly and stamp it onto the step the move produces.
    if (s.p1Token != 99) { g_recRecentToken = s.p1Token; g_recRecentTokenAge = 0; }
    else if (advanceTime && g_recRecentTokenAge < 999) {
        g_recRecentTokenAge = (std::min)(999, g_recRecentTokenAge + 3);
    }
    // Capture attacks AND known combo-relevant moves the attack classifier misses
    // (IC / air IC via the table). Movement (jump/dash) counts only mid-combo, so
    // neutral hops/dashes aren't recorded as steps.
    const bool comboActive = (s.p1Combo > 0) || s.p2InStun;
    const bool isMovement  = ::Mission::Moves::IsMovementMove(cur);
    // .pat-classified specials/supers are captured even when the generic classifier
    // misses them: a projectile/summon CAST (e.g. 236X) does no damage on the caster
    // frame, so IsAttackMove is false, yet it's a real combo step.
    const bool isSpecial   = ::Mission::MoveData::IsSpecialOrSuper(P1Char(), cur);
    const bool isCapture   = IsAttackMove(cur) || isSpecial ||
                             (::Mission::Moves::IsKnownComboMove(cur) && (!isMovement || comboActive));

    const bool comboRose = (s.p1Combo > g_recPrevCombo) && (s.p1Combo > 0);
    const auto comboBoundary = ::Mission::SequencePolicy::DetectSampledComboBoundary(
        g_recComboWasAlive, comboActive, g_recPrevCombo, s.p1Combo);
    const bool comboEndedEdge = comboBoundary !=
        ::Mission::SequencePolicy::SampledComboBoundary::None;
    // CONTACT edge (+0x168 leaves 0): the current capture-move touched the
    // opponent - hit OR blocked. A deliberate whiff never produces this.
    const bool contactEdge = (g_recPrevHitState == 0) && (s.p1HitState != 0);
    g_recPrevHitState = s.p1HitState;
    if (contactEdge && !g_recSteps.empty() && cur == g_recLastAttackId) {
        g_recSteps.back().connected = true;
    }

    // Preserve the *observed* one-piece-vs-setup distinction. Do not mutate the
    // previous step yet: if recording stops here this was merely the natural end
    // of the final combo. A later captured step commits this candidate as an
    // explicit comboEndAfter boundary. Actions that start after recovery anchor
    // their gap there; a pre-started meaty is already armed instead.
    if (comboEndedEdge && g_recAnyLanded && !g_recSteps.empty()) {
        const int lastLanded = LastRecordedLandedStep();
        if (lastLanded >= 0) {
            // Attach the boundary to the last accepted hit, not necessarily the
            // latest action: a meaty/setplay action may already be in startup.
            g_recPendingComboEndStep = lastLanded;
            g_recComboEndFrame = g_recFrame;
            const int activeStep = static_cast<int>(g_recSteps.size()) - 1;
            if (cur == g_recLastAttackId && activeStep > lastLanded) {
                CapturedStep& active = g_recSteps.back();
                const int oldBaseline = g_recComboAtStart;
                g_recComboAtStart =
                    ::Mission::SequencePolicy::RecordedActionBaselineAfterBoundary(
                        g_recComboAtStart, activeStep, lastLanded, active.landed);
                if (g_recComboAtStart != oldBaseline) {
                    LogOut("[MISSION][REC] rebased pre-started Part-2 action after "
                           "sampled combo boundary", true);
                }
            }
            LogOut("[MISSION][REC] combo.end candidate after landedStep=" +
                   std::to_string(g_recPendingComboEndStep) +
                   (comboBoundary == ::Mission::SequencePolicy::
                                        SampledComboBoundary::CounterRollover
                        ? " kind=counter-rollover" : " kind=visible-recovery"), true);
        }
    }

    if (isCapture) {
        // Move ID alone is insufficient: many chains re-enter the same move
        // without an intervening neutral ID (5A -> 5A). EFZ resets +0xA to the
        // start of the animation for the new instance, matching Runner's edge
        // detector and preserving both authored steps.
        const bool sameMoveReentered = cur == g_recLastAttackId &&
            cur == g_recPrevMove && ::Mission::SequencePolicy::IsMoveInstanceEdge(
                cur, s.p1FrameIdx, g_recPrevMove, g_recPrevFrameIdx);
        if (cur != g_recLastAttackId || sameMoveReentered) {
            int gapAnchor = g_recLastEventFrame;
            if (g_recPendingComboEndStep >= 0 &&
                g_recPendingComboEndStep < static_cast<int>(g_recSteps.size())) {
                gapAnchor = g_recComboEndFrame;
                CommitPendingRecordedComboEnd("later action");
            }
            CapturedStep cs;
            cs.moveId = cur;
            cs.landed = false;
            cs.notation = CaptureNotation(cur);
            cs.req = static_cast<int>(::Mission::StepReq::Move); // upgraded to Land once it connects
            // Stamp the command index that produced this move (facing-independent),
            // if one was recognized within the last few frames. Confirms specials.
            cs.cmdToken = (g_recRecentTokenAge <= 4) ? g_recRecentToken : 99;
            cs.startFrame = g_recFrame;
            // Gap from the previous step's last event (effective frames): this is
            // the delayed-button measurement the runner gets as its allowance.
            cs.gap = g_recSteps.empty() ? 0 : (g_recFrame - gapAnchor);
            g_recLastEventFrame = g_recFrame;
            g_recSteps.push_back(cs);
            g_recStepCountView.store(static_cast<int>(g_recSteps.size()), std::memory_order_release);
            g_recLastAttackId = cur;
            g_recComboAtStart = s.p1Combo;
            // Persistent moveID<->command pairing for building the per-char table.
            LogOut("[MISSION][REC] step move=" + std::to_string(cur) +
                   " cmd=" + std::to_string(cs.cmdToken) +
                   " pat=" + ::Mission::MoveData::ClassName(::Mission::MoveData::GetClass(P1Char(), cur)) +
                   " note=" + cs.notation, true);
        } else if (!g_recSteps.empty() && s.p1Combo > g_recComboAtStart) {
            // Hit(s) landed while this step's move is active: count them so a
            // multi-hit move becomes StepReq::Hits with the observed count.
            CapturedStep& st = g_recSteps.back();
            st.landed = true;
            st.req = static_cast<int>(::Mission::StepReq::Land);
            const int hits = s.p1Combo - g_recComboAtStart;
            const bool newHit = hits > st.hits;
            if (newHit) {
                st.hits = hits;
                // If this action began during knockdown and first hit only after
                // recovery, the hit itself proves there is a later segment.
                const int currentIndex = static_cast<int>(g_recSteps.size()) - 1;
                if (g_recPendingComboEndStep >= 0 &&
                    currentIndex > g_recPendingComboEndStep) {
                    CommitPendingRecordedComboEnd("post-edge active-move hit");
                }
                g_recLastEventFrame = g_recFrame;
            }
            g_recAnyLanded = true;
        }
    } else {
        // Left the move: the next capture-move (even the same ID) is a new step.
        g_recLastAttackId = 0;
        // DELAYED LAND (projectile/summon): the combo rose while no capture-move is
        // active - attribute the hit to the most recent unlanded step (its entity
        // connected after the cast recovered), and record the observed delay so the
        // saved step gets a maxDelay window.
        if (comboRose) {
            for (int i = static_cast<int>(g_recSteps.size()) - 1; i >= 0; --i) {
                CapturedStep& candidate = g_recSteps[i];
                if (!candidate.landed) {
                    // Never attribute a post-boundary entity hit backwards into
                    // Part 1. A setter/action captured after the boundary may own
                    // it; an older candidate remains format-2 REVIEW ambiguity.
                    if (g_recPendingComboEndStep >= 0 && i <= g_recPendingComboEndStep) {
                        LogOut("[MISSION][REC] delayed land ambiguous across combo.end; "
                               "raw entity trace required", true);
                        break;
                    }
                    candidate.landed = true;
                    candidate.req = static_cast<int>(::Mission::StepReq::Land);
                    candidate.maxDelay = g_recFrame - candidate.startFrame;
                    if (g_recPendingComboEndStep >= 0 && i > g_recPendingComboEndStep) {
                        CommitPendingRecordedComboEnd("post-edge delayed hit");
                    }
                    g_recAnyLanded = true;
                    g_recLastEventFrame = g_recFrame;
                    LogOut("[MISSION][REC] delayed land -> step move=" +
                           std::to_string(candidate.moveId) + " delay=" +
                           std::to_string(candidate.maxDelay) + "f", true);
                    break;
                }
                if (candidate.comboEndAfter) break;
            }
        }
    }
    g_recPrevCombo = s.p1Combo;
    g_recComboWasAlive = comboActive;
    g_recPrevMove = s.p1Move;
    g_recPrevFrameIdx = s.p1FrameIdx;
}

// A NEW instance of a step's move this frame: the move-ID changed onto a match,
// or the same move-ID re-entered (frame index reset - 5A chained into 5A).
bool StepArmEdge(const ::Mission::Step& st, const Snapshot& s) {
    if (!StepMatches(st, s.p1Move)) return false;
    return ::Mission::SequencePolicy::IsMoveInstanceEdge(
        s.p1Move, s.p1FrameIdx, g_runPrevMove, g_runPrevFrameIdx);
}

// With the current step ARMED, is its hit requirement now met? Deliberately does
// NOT require the move to still be active - delayed projectile/summon hits land
// after the caster recovered, and the combo counter (+0x174) rises then.
bool ArmedStepSatisfied(const ::Mission::Step& st, const Snapshot& s) {
    switch (st.req) {
        case ::Mission::StepReq::Move: return true;             // performing was enough
        case ::Mission::StepReq::Hits: return s.p1Combo >= g_runBaseline + st.hitsRequired;
        case ::Mission::StepReq::Connect: return g_runConnectSeen; // contact (hit/blocked)
        case ::Mission::StepReq::Land: default: return s.p1Combo > g_runBaseline;
    }
}

void ResetRunScore() {
    g_runScore.Reset();
}

void ObserveRunScore(const Snapshot& s) {
    g_runScore.Observe(s.p1Combo, s.p1ComboDamage);
}

void FinalizeRunSegment() {
    g_runScore.FinalizeSegment();
}

void RunnerComplete() {
    const int totalHits = g_runScore.TotalHits();
    const int totalDamage = g_runScore.TotalDamage();
    g_runPhase = RunnerPhase::Complete;
    g_runFailedStep.store(-1);
    // Evaluate score tiers: base (tier 0) is the clear; later tiers add rank
    // constraints. bestTier = highest tier whose constraints are all met.
    g_runBestTier = -1;
    for (size_t i = 0; i < g_runMission.scores.size(); ++i) {
        const ::Mission::ScoreTier& t = g_runMission.scores[i];
        bool ok = true;
        if (t.minHits > 0 && totalHits < t.minHits) ok = false;
        if (t.minDamage > 0 && totalDamage < t.minDamage) ok = false;
        if (t.maxAttempts > 0 && (g_runAttempts + 1) > t.maxAttempts) ok = false;
        if (ok) g_runBestTier = static_cast<int>(i);
    }
    LogOut("[MISSION][RUN] COMPLETE tier=" + std::to_string(g_runBestTier) +
           " attempts=" + std::to_string(g_runAttempts + 1) +
           " totalHits=" + std::to_string(totalHits) +
           " totalDamage=" + std::to_string(totalDamage), true);
}

void RunnerDrop() {
    // Preserve the recipe position before resetting progress for the retry. The
    // renderer keeps this exact move red until step 0 of a genuinely new combo
    // is satisfied (merely performing/arming the opener is not enough).
    const int total = static_cast<int>(g_runMission.steps.size());
    const int failedStep = total > 0
        ? (std::min)(g_runStep, total - 1)
        : -1;
    g_runFailedStep.store(failedStep);
    g_runAttempts++;
    g_runPhase = RunnerPhase::Dropped;
    g_runStep = 0;
    g_runBaseline = 0;
    g_runGapWindow.Reset();
    g_runComboWasAlive = false;
    g_runPrevComboCount = 0;
    g_runPrevInputs = 0;
    g_runAwaitingComboEnd = false;
    ResetRunScore();
    LogOut("[MISSION][RUN] DROP step=" + std::to_string(failedStep) +
           " attempts=" + std::to_string(g_runAttempts), true);
    // TrialMode-style retry: snap back to the mission's start state. Never while
    // a hotswap/setup is still driving the match, and never under the recorder
    // (a rollback mid-capture corrupts the recording; recording also unloads the
    // runner, so this is defense in depth).
    if (g_runStateSaved && SavestateHook::IsInstalled() &&
        !g_recActive.load() &&
        GetCurrentGamePhase() == GamePhase::Match &&
        !CharacterHotswap::IsBusy() && !::Mission::Setup::IsPending()) {
        g_runSelfLoad = true;
        const bool ok = SavestateHook::TriggerLoad();
        g_runSelfLoad = false;
        if (ok) {
            LogOut("[MISSION][RUN] auto-loaded baseline savestate for retry", true);
            // The baseline is save-gated to the active round, but stay safe
            // against older/mid-intro states: force the active round on load.
            SkipRoundIntro("retry load");
        }
    }
}

void DrawRunOverlay() {
    const int total = static_cast<int>(g_runMission.steps.size());
    const char* phase = g_runPhase == RunnerPhase::Complete ? "CLEAR!"
                      : g_runPhase == RunnerPhase::Dropped  ? "DROPPED"
                      : g_runStep > 0 ? "GO" : "READY";
    char buf[192];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "MISSION: %s   %d/%d  [%s]   tries=%d%s",
                g_runMission.name.c_str(), g_runStep, total, phase, g_runAttempts,
                g_runPhase == RunnerPhase::Complete && g_runBestTier >= 0
                    ? (std::string("  rank=") + std::to_string(g_runBestTier)).c_str() : "");
    DirectDrawHook::AddMessage(buf, "mission_run",
        g_runPhase == RunnerPhase::Complete ? RGB(140, 255, 140)
      : g_runPhase == RunnerPhase::Dropped  ? RGB(255, 160, 160)
                                            : RGB(200, 220, 255), 500, 20, 80);
}

// A move that counts as a combo STEP (attack / special / IC) - movement (jumps,
// dashes) is free positioning and never a "wrong move".
bool IsComboStepMove(short m) {
    if (IsAttackMove(m)) return true;
    if (::Mission::MoveData::IsSpecialOrSuper(P1Char(), m)) return true;
    return ::Mission::Moves::IsKnownComboMove(m) && !::Mission::Moves::IsMovementMove(m);
}

// Advance bookkeeping when the current step is satisfied.
void AdvanceStep(const Snapshot& s, int total) {
    ObserveRunScore(s);
    const bool requireComboEnd = g_runStep >= 0 && g_runStep < total &&
        g_runMission.steps[g_runStep].comboEndAfter;
    g_runStep++;
    g_runBaseline = s.p1Combo;
    g_runGapWindow.Reset();
    g_runArmed = false;
    g_runArmedFrames = 0;
    if (requireComboEnd) {
        // Completion/progression retains a separately observed recovery edge.
        // Setup/meaty startup may begin early, but a hit cannot satisfy Part 2
        // until the boundary is consumed.
        g_runAwaitingComboEnd = true;
        LogOut("[MISSION][RUN] waiting for required combo.end after step=" +
               std::to_string(g_runStep - 1), true);
        return;
    }
    if (g_runStep >= total) {
        if (!g_runAwaitingComboEnd) RunnerComplete();
        return;
    }
    // Delayed-hit overlap: the player may already be doing the NEXT step's move
    // while the previous step's projectile was still in flight. Its arm edge has
    // passed, so arm it now if it is the currently active move.
    const ::Mission::Step& next = g_runMission.steps[g_runStep];
    if (StepMatches(next, s.p1Move) && IsComboStepMove(s.p1Move)) {
        g_runArmed = true;
        g_runArmedFrames = 0;
        g_runConnectSeen = false;
        g_runBaseline = s.p1Combo;
    }
}

void TickRun(const Snapshot& s) {
    const int total = static_cast<int>(g_runMission.steps.size());
    if (total == 0) { g_runPrevMove = s.p1Move; g_runPrevFrameIdx = s.p1FrameIdx; return; }

    // Contact edge (+0x168 leaves 0) while armed: the step's move touched the
    // opponent (hit or blocked) - satisfies StepReq::Connect.
    const bool contactEdge = (g_runPrevHitState == 0) && (s.p1HitState != 0);
    g_runPrevHitState = s.p1HitState;
    if (contactEdge && g_runArmed) g_runConnectSeen = true;

    if (g_runPhase == RunnerPhase::Idle || g_runPhase == RunnerPhase::Dropped) {
        // Wait for the player to (re)start step 0: arm on perform, satisfy on hit.
        // No drop detection here, so a slow projectile opener (cast now, hit later)
        // is fine - the arm persists until the requirement lands.
        const ::Mission::Step& st = g_runMission.steps[0];
        if (!g_runArmed && StepArmEdge(st, s)) {
            g_runArmed = true;
            g_runArmedFrames = 0;
            g_runConnectSeen = false;
            g_runBaseline = (s.p1Combo > 0) ? s.p1Combo : 0;
            g_runPhase = RunnerPhase::Idle;   // clear a shown DROPPED once they retry
        }
        if (g_runArmed) {
            if (!s.freezeActive) ++g_runArmedFrames;
            if (st.maxDelay > 0 && g_runArmedFrames > st.maxDelay) {
                g_runArmed = false;           // window expired; wait for a fresh attempt
            } else if (ArmedStepSatisfied(st, s)) {
                // This is the retry boundary: the opener actually connected/met
                // its requirement, so the old failure marker can finally clear.
                g_runFailedStep.store(-1);
                g_runPhase = RunnerPhase::InProgress;
                g_runStep = 0;
                AdvanceStep(s, total);
            }
        }
    } else if (g_runPhase == RunnerPhase::InProgress) {
        const bool comboAlive = s.p2InStun || s.p1Combo > 0;
        const auto sampledBoundary =
            ::Mission::SequencePolicy::DetectSampledComboBoundary(
                g_runComboWasAlive, comboAlive, g_runPrevComboCount, s.p1Combo);
        const bool comboEndedEdge = sampledBoundary !=
            ::Mission::SequencePolicy::SampledComboBoundary::None;
        bool consumedRequiredBoundary = false;
        bool actionStartedThisTick = false;
        // On an atomic N->1 rollover the current sample already belongs to Part
        // 2. Finalize the previously observed maxima first, then seed the new
        // segment from this sample after consuming the boundary.
        if (sampledBoundary != ::Mission::SequencePolicy::
                                   SampledComboBoundary::CounterRollover) {
            ObserveRunScore(s);
        }

        // Combo recovery is evaluated before the next move on this sample. An
        // authored edge is a first-class boundary; an unmarked edge still means
        // a one-piece combo was dropped. Consuming the boundary also rebases hit
        // validation so Part 2 starts from combo count zero.
        const auto comboEndDecision = ::Mission::SequencePolicy::DecideComboEnd(
            comboEndedEdge, g_runAwaitingComboEnd);
        if (comboEndDecision != ::Mission::SequencePolicy::ComboEndDecision::None) {
            if (comboEndDecision ==
                ::Mission::SequencePolicy::ComboEndDecision::ConsumeRequired) {
                FinalizeRunSegment();
                if (sampledBoundary == ::Mission::SequencePolicy::
                                           SampledComboBoundary::CounterRollover) {
                    ObserveRunScore(s);
                }
                g_runAwaitingComboEnd = false;
                consumedRequiredBoundary = true;
                g_runBaseline = 0;
                g_runGapWindow.Reset();
                LogOut("[MISSION][RUN] consumed required combo.end; nextStep=" +
                       std::to_string(g_runStep) +
                       " kind=" +
                       (sampledBoundary == ::Mission::SequencePolicy::
                                              SampledComboBoundary::CounterRollover
                            ? "counter-rollover" : "visible-recovery") +
                       " finalizedHits=" + std::to_string(g_runScore.finalizedHits) +
                       " finalizedDamage=" + std::to_string(g_runScore.finalizedDamage), true);
                if (g_runStep >= total) RunnerComplete();
            } else {
                LogOut("[MISSION][RUN] unexpected combo.end in one-piece segment at step=" +
                       std::to_string(g_runStep) + " kind=" +
                       (sampledBoundary == ::Mission::SequencePolicy::
                                              SampledComboBoundary::CounterRollover
                            ? "counter-rollover" : "visible-recovery"), true);
                RunnerDrop();
            }
        }

        if (g_runPhase == RunnerPhase::InProgress) {
            // STRICT SEQUENCE (CCCaster model): a NEW combo-step move that doesn't
            // match the expected step is a failure. Optional skip-ahead and a
            // delayed-hit overlap remain the only authored exceptions.
            if (g_runStep < total) {
                const bool moveChanged = (s.p1Move != g_runPrevMove);
                const bool prevStepEcho = (g_runStep > 0) &&
                    StepMatches(g_runMission.steps[g_runStep - 1], s.p1Move);
                if (moveChanged && !prevStepEcho && IsComboStepMove(s.p1Move) &&
                    !StepMatches(g_runMission.steps[g_runStep], s.p1Move)) {
                    int k = g_runStep;
                    while (k < total && g_runMission.steps[k].optional &&
                           !StepMatches(g_runMission.steps[k], s.p1Move)) ++k;
                    const bool matchedAhead = (k < total) && (k != g_runStep) &&
                        StepMatches(g_runMission.steps[k], s.p1Move);
                    const bool nextOverlap = g_runArmed && (g_runStep + 1 < total) &&
                        StepMatches(g_runMission.steps[g_runStep + 1], s.p1Move);
                    if (matchedAhead) {
                        g_runStep = k;
                        g_runArmed = false;
                    } else if (!nextOverlap) {
                        LogOut("[MISSION][RUN] wrong move " + std::to_string(s.p1Move) +
                               " at step " + std::to_string(g_runStep), true);
                        RunnerDrop();
                        g_runArmed = false;
                    }
                }
            }

            if (g_runPhase == RunnerPhase::InProgress && g_runStep < total) {
                const ::Mission::Step& st = g_runMission.steps[g_runStep];
                if (!g_runArmed && StepArmEdge(st, s)) {
                    actionStartedThisTick = true;
                    g_runArmed = true;
                    g_runArmedFrames = 0;
                    g_runConnectSeen = false;
                    g_runGapWindow.Reset();       // performing the move is progress
                    // Residual hits from the previous move must not count toward
                    // this step; a consumed combo.end already rebased this to zero.
                    g_runBaseline = ::Mission::SequencePolicy::RunnerArmBaseline(
                        s.p1Combo, consumedRequiredBoundary);
                }
                if (g_runArmed) {
                    if (!s.freezeActive) ++g_runArmedFrames;
                    if (st.maxDelay > 0 && g_runArmedFrames > st.maxDelay) {
                        g_runArmed = false;
                    } else {
                        const auto satisfaction =
                            ::Mission::SequencePolicy::DecideStepSatisfaction(
                                ArmedStepSatisfied(st, s),
                                g_runAwaitingComboEnd,
                                st.req == ::Mission::StepReq::Move);
                        // Setup/meaty startup may be recorded before recovery.
                        // Move-only setup steps can advance while the boundary
                        // remains pending; contact/hits before the edge prove it
                        // was still one continuous combo and must fail.
                        if (satisfaction == ::Mission::SequencePolicy::
                                                StepSatisfactionDecision::PrematureContactDrop) {
                            LogOut("[MISSION][RUN] step connected before required combo.end step=" +
                                   std::to_string(g_runStep), true);
                            RunnerDrop();
                        } else if (satisfaction == ::Mission::SequencePolicy::
                                                       StepSatisfactionDecision::Advance) {
                            AdvanceStep(s, total);
                        }
                    }
                }
            }
        }

        if (g_runPhase == RunnerPhase::InProgress) {
            // While combo is alive EFZ's own continuation window is authoritative.
            // After an explicit combo.end, the recorded next-button maxGap governs
            // neutral/setup time. An armed delayed hit uses maxDelay instead.
            const int gapLimit = (g_runStep < total &&
                                  g_runMission.steps[g_runStep].maxGap > 0)
                               ? g_runMission.steps[g_runStep].maxGap
                               : g_runMission.failTimer;
            constexpr uint8_t kAttackButtons = INPUT_A | INPUT_B | INPUT_C | INPUT_D;
            const bool attackPressedEdge =
                !actionStartedThisTick &&
                (s.p1PolledAttackEdges != 0 ||
                 (s.p1Inputs & kAttackButtons &
                  static_cast<uint8_t>(~g_runPrevInputs)) != 0);
            bool gapExpired = false;
            if (!g_runAwaitingComboEnd && !g_runArmed && !comboAlive && gapLimit > 0) {
                const int oldGrace = g_runGapWindow.commitGrace;
                gapExpired = g_runGapWindow.Tick(
                    s.freezeActive, false, attackPressedEdge, gapLimit);
                if (oldGrace == 0 && g_runGapWindow.commitGrace > 0) {
                    LogOut("[MISSION][RUN] latched delayed attack input at gap deadline step=" +
                           std::to_string(g_runStep) + " pollSerial=" +
                           std::to_string(s.p1InputPollSerial), true);
                }
            }
            if (gapExpired) {
                LogOut("[MISSION][RUN] delayed-button/setup gap expired step=" +
                       std::to_string(g_runStep) + " limit=" +
                       std::to_string(gapLimit), true);
                RunnerDrop();
            }
        }
    }

    if (g_runPhase != RunnerPhase::Dropped) {
        g_runComboWasAlive = (s.p2InStun || s.p1Combo > 0);
        g_runPrevComboCount = s.p1Combo;
    }
    g_runPrevInputs = s.p1Inputs;

    g_runPrevMove = s.p1Move;
    g_runPrevFrameIdx = s.p1FrameIdx;
    DrawRunOverlay();
}

void ResetRecorderCaptureState() {
    g_recSteps.clear();
    g_recStepCountView.store(0, std::memory_order_release);
    g_recComboEndCountView.store(0, std::memory_order_release);
    g_recLastAttackId = 0;
    g_recComboAtStart = 0;
    g_recRecentToken = 99;
    g_recRecentTokenAge = 999;
    g_recFrame = 0;
    g_recLastEventFrame = 0;
    g_recPrevCombo = 0;
    g_recAnyLanded = false;
    g_recComboWasAlive = false;
    g_recPendingComboEndStep = -1;
    g_recComboEndFrame = 0;
    g_recPrevHitState = 0;
    g_recPrevMove = 0;
    g_recPrevFrameIdx = 0;
    g_recCountInView.store(0, std::memory_order_release);
}

void ShowRecorderState(const char* text, COLORREF color, int duration = 900) {
    // Transient notices sit below the phase banner; routine phase state itself is
    // owned by RefreshRecorderBanner and never expires.
    DirectDrawHook::AddMessage(text, "mission_record", color, duration, 20, 84);
}

void EnterRecorderPreRecord(bool keepSetup) {
    g_recActive.store(false, std::memory_order_release);
    g_recReleaseFrames = 0;
    g_recCountInFrames = 0;
    g_recCountInView.store(0, std::memory_order_release);
    if (!keepSetup) {
        ResetRecorderCaptureState();
        g_recSetup = ::Mission::Mission();
    }
    g_recPhase.store(RecorderPhase::PreRecord, std::memory_order_release);
    LogOut("[MISSION][REC] entered pre-record", true);
}

void CaptureRecorderBaseline() {
    ResetRecorderCaptureState();
    g_recSetup = ::Mission::Mission();
    ::Mission::Setup::Capture(g_recSetup);
    if (GetCurrentGamePhase() == GamePhase::Match && ::Mission::StateDump::Available()) {
        std::string sdErr;
        if (::Mission::StateDump::Capture(g_recSetup.savestate, sdErr)) {
            LogOut("[MISSION][REC] start-state dump attached (" +
                   std::to_string(g_recSetup.savestate.size()) + " chars b64)", true);
        } else {
            LogOut("[MISSION][REC] start-state dump skipped: " + sdErr, true);
        }
    }
}

bool RestoreRecorderBaseline() {
    bool restored = false;
    if (!g_recSetup.savestate.empty() && ::Mission::StateDump::Available()) {
        std::string err;
        restored = ::Mission::StateDump::Restore(g_recSetup.savestate, err);
        if (!restored) LogOut("[MISSION][REC] retake state restore failed: " + err, true);
    }
    if (!restored && !::Mission::Setup::Apply(
            g_recSetup, /*applyValues=*/true, /*forceFreshMatch=*/false,
            /*allowReload=*/false)) {
        LogOut("[MISSION][REC] retake refused because the live session no longer matches the baseline",
               true);
        return false;
    }
    SkipRoundIntro("mission retake");
    return true;
}

void ProcessRecorderCommand() {
    const RecorderCommand cmd = g_recCommand.exchange(RecorderCommand::None,
                                                       std::memory_order_acq_rel);
    if (cmd == RecorderCommand::None) return;

    const RecorderPhase phase = g_recPhase.load(std::memory_order_acquire);
    if (cmd == RecorderCommand::Cancel) {
        if (MacroController::GetState() != MacroController::State::Idle) MacroController::Stop();
        g_recActive.store(false, std::memory_order_release);
        g_recPhase.store(RecorderPhase::Idle, std::memory_order_release);
        ResetRecorderCaptureState();
        g_recSetup = ::Mission::Mission();
        DirectDrawHook::RemoveMessagesByCategory("mission_record");
        RemoveRecorderBanner();
        LogOut("[MISSION][REC] authoring session discarded", true);
        return;
    }

    if (cmd == RecorderCommand::Arm) {
        if (Demo::IsActive()) {
            Demo::Cancel();
            ShowRecorderState("Canceling demonstration; arm recording again when it finishes",
                              RGB(255, 190, 120), 1800);
            return;
        }
        if (MacroController::GetState() != MacroController::State::Idle) MacroController::Stop();
        if (g_runActive.load()) Runner::Unload();
        EnterRecorderPreRecord(false);
        return;
    }

    if (cmd == RecorderCommand::Retake) {
        if (phase != RecorderPhase::Review) return;
        if (MacroController::GetState() != MacroController::State::Idle) MacroController::Stop();
        if (!RestoreRecorderBaseline()) {
            ShowRecorderState("Retake unavailable: return to the recorded matchup and try again",
                              RGB(255, 140, 120), 2200);
            return;
        }
        ResetRecorderCaptureState();
        EnterRecorderPreRecord(true);
        return;
    }

    if (cmd != RecorderCommand::Advance) return;
    const auto advance = ::Mission::Engine::Recorder::DecideAdvance(phase);
    if (advance == ::Mission::Engine::Recorder::AdvanceEffect::BeginCountIn) {
        if (GetCurrentGamePhase() != GamePhase::Match || !g_snapshot.valid ||
            ImGuiImpl::IsVisible() || ::Mission::Setup::IsPending() ||
            CharacterHotswap::IsBusy()) {
            ShowRecorderState("MISSION PRE-RECORD  |  close the menu and wait for the match",
                              RGB(255, 190, 120), 1500);
            return;
        }
        g_recReleaseFrames = 0;
        g_recCountInFrames = 0;
        g_recCountInView.store(3, std::memory_order_release);
        g_recPhase.store(RecorderPhase::CountIn, std::memory_order_release);
        LogOut("[MISSION][REC] waiting for neutral count-in; baseline capture deferred to GO", true);
    } else if (advance == ::Mission::Engine::Recorder::AdvanceEffect::CancelCountIn) {
        // The same physical control is a safe toggle during the countdown. It
        // never starts a partial take and never discards Review data.
        g_recReleaseFrames = 0;
        g_recCountInFrames = 0;
        g_recCountInView.store(0, std::memory_order_release);
        g_recPhase.store(RecorderPhase::PreRecord, std::memory_order_release);
        ShowRecorderState("Countdown canceled; starting position was not captured",
                          RGB(255, 220, 120), 1200);
        LogOut("[MISSION][REC] count-in canceled", true);
    } else if (advance == ::Mission::Engine::Recorder::AdvanceEffect::StopToReview) {
        (void)MacroController::FinishPlayerRecording();
        g_recActive.store(false, std::memory_order_release);
        g_recPhase.store(RecorderPhase::Review, std::memory_order_release);
        LogOut("[MISSION][REC] capture stopped for review: " +
               std::to_string(g_recSteps.size()) + " step(s)", true);
    } else if (advance == ::Mission::Engine::Recorder::AdvanceEffect::KeepReview) {
        ShowRecorderState("Take is safe in Review; open the menu to preview, save, retake, or discard",
                          RGB(180, 230, 255), 1800);
    }
}

void TickRecorderFrontend(const Snapshot& s) {
    const RecorderPhase phase = g_recPhase.load(std::memory_order_acquire);
    if (phase == RecorderPhase::Recording &&
        MacroController::GetState() != MacroController::State::Recording) {
        g_recActive.store(false, std::memory_order_release);
        MacroController::Stop();
        if (!RestoreRecorderBaseline()) {
            g_recActive.store(false, std::memory_order_release);
            g_recPhase.store(RecorderPhase::Review, std::memory_order_release);
            ShowRecorderState("Capture interrupted; baseline could not be restored. Review data was kept",
                              RGB(255, 140, 120), 2400);
            return;
        }
        ResetRecorderCaptureState();
        EnterRecorderPreRecord(true);
        ShowRecorderState("Mission capture was interrupted; baseline restored for a clean retake",
                          RGB(255, 160, 120), 2200);
        LogOut("[MISSION][REC] input recorder invariant lost; discarded desynchronized capture", true);
        return;
    }
    if (phase != RecorderPhase::CountIn) return;
    if (!s.valid || GetCurrentGamePhase() != GamePhase::Match || ImGuiImpl::IsVisible() ||
        ::Mission::Setup::IsPending() || CharacterHotswap::IsBusy() ||
        PauseIntegration::IsPracticePaused() || PauseIntegration::IsGameSpeedFrozen()) {
        g_recReleaseFrames = 0;
        g_recCountInFrames = 0;
        g_recCountInView.store(3, std::memory_order_release);
        return;
    }

    if (GetPlayerInputs(1) != 0) {
        g_recReleaseFrames = 0;
        g_recCountInFrames = 0;
        g_recCountInView.store(3, std::memory_order_release);
        return;
    }
    if (g_recReleaseFrames < 6) {
        ++g_recReleaseFrames;
        return;
    }
    if (!s.freezeActive) ++g_recCountInFrames;
    constexpr int kCountInTicks = 576; // three seconds at EFZ's 192 Hz internal cadence
    const int framesLeft = kCountInTicks - g_recCountInFrames;
    const int count = framesLeft > 384 ? 3 : framesLeft > 192 ? 2 : 1;
    g_recCountInView.store(count, std::memory_order_release);
    if (g_recCountInFrames < kCountInTicks) return;

    // Frame-zero boundary: clear stale action/queue owners, capture the exact
    // baseline, then arm+start the P1 input stream on this same monitor tick.
    CancelAutoActionsAndMacros();
    CaptureRecorderBaseline();
    if (!MacroController::BeginPlayerRecording(1, false) ||
        !MacroController::StartPlayerRecording()) {
        MacroController::Stop();
        g_recPhase.store(RecorderPhase::PreRecord, std::memory_order_release);
        g_recCountInView.store(0, std::memory_order_release);
        ShowRecorderState("Mission input capture failed; press Macro Record to retry",
                          RGB(255, 120, 120), 1800);
        return;
    }
    g_recPrevCombo = s.p1Combo;
    g_recComboWasAlive = (s.p1Combo > 0) || s.p2InStun;
    g_recPrevHitState = s.p1HitState;
    g_recPrevMove = s.p1Move;
    g_recPrevFrameIdx = s.p1FrameIdx;
    g_recCountInView.store(0, std::memory_order_release);
    g_recActive.store(true, std::memory_order_release);
    g_recPhase.store(RecorderPhase::Recording, std::memory_order_release);
    LogOut("[MISSION][REC] synchronized P1 step+input capture started", true);
}

void ResetRunForDemo() {
    g_runPhase = RunnerPhase::Idle;
    g_runStep = 0;
    g_runFailedStep.store(-1, std::memory_order_release);
    g_runBaseline = 0;
    g_runGapWindow.Reset();
    g_runBestTier = -1;
    g_runArmed = false;
    g_runArmedFrames = 0;
    g_runPrevMove = 0;
    g_runPrevFrameIdx = 0;
    g_runComboWasAlive = false;
    g_runPrevComboCount = 0;
    g_runPrevInputs = 0;
    g_runAwaitingComboEnd = false;
    g_runPrevHitState = 0;
    g_runConnectSeen = false;
    ResetRunScore();
}

void HoldDemoP1Neutral(bool hold) {
    g_pollOverrideMask[1].store(0, std::memory_order_relaxed);
    g_pollOverrideActive[1].store(hold, std::memory_order_release);
}

void SetRunnerStartupInputHold(bool hold) {
    g_runStartupInputHeld.store(hold, std::memory_order_release);
    if (hold) {
        g_pollOverrideMask[1].store(0, std::memory_order_relaxed);
        g_pollOverrideActive[1].store(true, std::memory_order_release);
    } else if (!Demo::IsActive() && !MacroController::IsExclusivePlayback()) {
        g_pollOverrideMask[1].store(0, std::memory_order_relaxed);
        g_pollOverrideActive[1].store(false, std::memory_order_release);
    }
}

bool RestoreDemoBaseline(std::string& errorOut) {
    const ::Mission::Mission& baseline = g_demoFromRecorder ? g_recSetup : g_runMission;
    bool ok = false;
    g_runSelfLoad = true;
    if (!baseline.savestate.empty() && ::Mission::StateDump::Available()) {
        ok = ::Mission::StateDump::Restore(baseline.savestate, errorOut);
    } else if (!g_demoFromRecorder && g_runStateSaved && SavestateHook::IsInstalled()) {
        ok = SavestateHook::TriggerLoad();
        if (!ok) errorOut = "mission baseline load was unavailable";
    } else {
        ok = ::Mission::Setup::Apply(
            baseline, /*applyValues=*/true, /*forceFreshMatch=*/false,
            /*allowReload=*/false);
        if (!ok) errorOut = "live session no longer matches the demonstration baseline";
    }
    g_runSelfLoad = false;
    if (ok) SkipRoundIntro("mission demonstration baseline");
    return ok;
}

void BeginDemoRestoring() {
    if (MacroController::GetState() != MacroController::State::Idle) MacroController::Stop();
    g_demoBaselineIssued = false;
    g_demoSettleFrames = 0;
    g_demoPhase.store(DemoPhase::Restoring, std::memory_order_release);
    HoldDemoP1Neutral(true);
}

void TickDemo(const Snapshot& s) {
    DemoPhase phase = g_demoPhase.load(std::memory_order_acquire);
    if (phase == DemoPhase::Idle) return;

    if (g_demoCancelRequested.exchange(false, std::memory_order_acq_rel)) {
        if (phase != DemoPhase::Restoring) {
            BeginDemoRestoring();
            phase = DemoPhase::Restoring;
            LogOut("[MISSION][DEMO] cancellation requested", true);
        }
    }

    if (!s.valid || GetCurrentGamePhase() != GamePhase::Match) {
        if (MacroController::GetState() != MacroController::State::Idle) MacroController::Stop();
        MacroController::ReleaseExclusivePlaybackHold();
        HoldDemoP1Neutral(false);
        g_demoText.clear();
        g_demoFromRecorder = false;
        g_demoBaselineIssued = false;
        g_demoPhase.store(DemoPhase::Idle, std::memory_order_release);
        return;
    }

    if (phase == DemoPhase::Playing) {
        if (RoundIntroActive()) {
            BeginDemoRestoring();
            LogOut("[MISSION][DEMO] round transition detected; restoring baseline", true);
            return;
        }
        if (MacroController::GetState() == MacroController::State::Idle) {
            BeginDemoRestoring();
            LogOut("[MISSION][DEMO] clip ended; restoring player-ready baseline", true);
        }
        return;
    }

    HoldDemoP1Neutral(true);
    if (ImGuiImpl::IsVisible() || ::Mission::Setup::IsPending() ||
        CharacterHotswap::IsBusy() ||
        (!g_demoFromRecorder && (g_runRestorePending || g_runSavePending)) ||
        (phase == DemoPhase::Preparing && RoundIntroActive())) {
        g_demoSettleFrames = 0;
        return;
    }

    if (!g_demoBaselineIssued) {
        if (phase == DemoPhase::Preparing) {
            CancelAutoActionsAndMacros();
            HoldDemoP1Neutral(true);
        }
        std::string err;
        const bool restored = RestoreDemoBaseline(err);
        // The savestate load preamble clears poll overrides; reclaim P1 in the
        // same call so physical input never gets a post-restore poll window.
        HoldDemoP1Neutral(true);
        if (!restored) {
            MacroController::ReleaseExclusivePlaybackHold();
            HoldDemoP1Neutral(false);
            g_demoText.clear();
            g_demoFromRecorder = false;
            g_demoBaselineIssued = false;
            g_demoPhase.store(DemoPhase::Idle, std::memory_order_release);
            DirectDrawHook::AddMessage(("Demo restore failed: " + err).c_str(), "MISSION",
                                       RGB(255, 120, 120), 2200, 0, 120);
            LogOut("[MISSION][DEMO] baseline restore failed: " + err, true);
            return;
        }
        if (!g_demoFromRecorder) ResetRunForDemo();
        g_demoBaselineIssued = true;
        g_demoSettleFrames = 0;
        return;
    }

    if (++g_demoSettleFrames < 9) return;
    g_demoSettleFrames = 0;

    if (phase == DemoPhase::Preparing) {
        std::string err;
        // StartPlayback resets the temporary neutral hold and replaces it with
        // the authoritative P1 demonstration stream.
        if (!MacroController::PlaySerializedForPlayer(g_demoText, 1, true, err)) {
            BeginDemoRestoring();
            DirectDrawHook::AddMessage(("Demo failed: " + err).c_str(), "MISSION",
                                       RGB(255, 120, 120), 2200, 0, 120);
            return;
        }
        g_demoPhase.store(DemoPhase::Playing, std::memory_order_release);
        LogOut("[MISSION][DEMO] exclusive P1 playback started", true);
    } else if (phase == DemoPhase::Restoring) {
        if (!g_demoFromRecorder) ResetRunForDemo();
        MacroController::ReleaseExclusivePlaybackHold();
        HoldDemoP1Neutral(false);
        g_demoText.clear();
        g_demoBaselineIssued = false;
        g_demoPhase.store(DemoPhase::Idle, std::memory_order_release);
        g_demoFinishedThisTick = true;
        DirectDrawHook::AddMessage(g_demoFromRecorder
                                       ? "Preview finished - back to Review"
                                       : "Demonstration finished - your turn", "MISSION",
                                   RGB(180, 255, 220), 1800, 0, 120);
        g_demoFromRecorder = false;
        LogOut("[MISSION][DEMO] baseline restored; player control released", true);
    }
}

} // namespace

void Tick() {
    std::lock_guard<std::recursive_mutex> runStateLock(g_runStateMx);
    g_demoFinishedThisTick = false;
    Snapshot s;
    s.p1PolledAttackEdges = ConsumeInputPollAttackEdges(1);
    s.p1InputPollSerial = GetInputPollSerial(1);
    const uintptr_t p1 = GetPlayerBase(1);
    const uintptr_t p2 = GetPlayerBase(2);
    s.valid = (p1 != 0 && p2 != 0);
    if (s.valid) {
        s.p1Move = ReadMove(p1);
        s.p2Move = ReadMove(p2);
        const int rawCombo = static_cast<int>(ReadShort(p1, PLAYER_COMBO_COUNTER_OFFSET));
        s.p1Combo = rawCombo > 0 ? rawCombo : 0;
        s.p1ComboDamage = ReadInt(p1, PLAYER_COMBO_DAMAGE_OFFSET);
        s.p1Inputs = GetPlayerInputs(1);
        s.p1Attacking = IsAttackMove(s.p1Move);
        s.p2InStun = IsHitstun(s.p2Move) || IsLaunched(s.p2Move) || IsBlockstun(s.p2Move);
        s.p1Token = ReadShort(p1, MOTION_TOKEN_OFFSET); // +0x262 command index (99 = none)
        s.p1FrameIdx = ReadShort(p1, 0xA);              // frame index within the move
        // Global freeze: superflash (+0x14C, spellflash of IC/supers - engine
        // timers stop while nonzero on either side) or contact hitstop (+0x14A).
        const short p1SF = ReadShort(p1, 0x14C), p2SF = ReadShort(p2, 0x14C);
        const short p1HS = ReadShort(p1, 0x14A), p2HS = ReadShort(p2, 0x14A);
        s.freezeActive = (p1SF > 0) || (p2SF > 0) || (p1HS > 0) || (p2HS > 0);
        s.p1HitState = ReadInt(p1, 0x168);  // 0=none, 2=block, 3=hit (contact edge)
    }
    g_snapshot = s;

    // Load the active character's baked move-ID reference once (cached per charId).
    // Only bother when a consumer (recorder/inspector) is active.
    if (s.valid && (g_recActive.load() || g_inspector.load() || g_runActive.load())) {
        ::Mission::MoveData::EnsureLoaded(displayData.p1CharID);
    }

    // Drive a hotswap-deferred mission setup (chars loaded -> apply values).
    ::Mission::Setup::Tick();

    // Frontend threads only enqueue authoring commands. Consume them here so
    // setup capture and recorder state are never mutated concurrently.
    ProcessRecorderCommand();

    // The recorder is match-scoped: leaving the match (title, char select, a
    // hotswap reload) discards the in-progress capture - steps recorded across
    // a dead session are junk and would pollute the RECORDED list.
    if (Recorder::IsSessionActive() && GetCurrentGamePhase() != GamePhase::Match) {
        Recorder::Cancel();
        ProcessRecorderCommand();
        DirectDrawHook::AddMessage("Mission recording discarded (left match)", "MISSION",
                                   RGB(255, 180, 120), 1500, 0, 120);
    }

    // Trials skip the round intro outright (the custom-savestate behavior): once
    // a mission is up and the match itself is running, force the active round.
    if (s.valid && g_runActive.load() && !Demo::IsActive() &&
        GetCurrentGamePhase() == GamePhase::Match &&
        !::Mission::Setup::IsPending() && !CharacterHotswap::IsBusy()) {
        SkipRoundIntro("mission start");
    }

    // Character hotswap/session-reset paths clear every poll override.  The
    // runner's flag deliberately survives those resets, so reclaim P1 on every
    // valid Match tick until the embedded restore or value baseline is ready.
    if (g_runStartupInputHeld.load(std::memory_order_acquire) &&
        (g_runRestorePending || g_runSavePending) && s.valid &&
        GetCurrentGamePhase() == GamePhase::Match) {
        SetRunnerStartupInputHold(true);
    }

    // Embedded start-state restore: once the match settles after the mission's
    // hotswap, inject the mission's Revival savestate dump (buffer swap +
    // pointer reconciliation). The restored buffer IS the mission start state,
    // so it doubles as the auto-retry baseline. If the clear-window never
    // settles (~30s of Match with something blocking), fall back to values -
    // a mission must never run with NO setup at all.
    if (g_runRestorePending && s.valid && g_runActive.load() &&
        GetCurrentGamePhase() == GamePhase::Match) {
        const bool clear = !ImGuiImpl::IsVisible() && !::Mission::Setup::IsPending() &&
                           !CharacterHotswap::IsBusy() && !g_recActive.load() &&
                           !RoundIntroActive();
        if (!clear) g_runRestoreSettle = 0;
        const bool doRestore = clear && ++g_runRestoreSettle > 60;
        const bool doTimeout = !doRestore && ++g_runRestoreTimeout > 5760;
        if (doRestore || doTimeout) {
            g_runRestoreSettle = 0;
            g_runRestoreTimeout = 0;
            g_runRestorePending = false;
            std::string sdErr = doTimeout ? std::string("restore window never settled")
                                          : std::string();
            bool ok = false;
            if (doRestore) {
                g_runSelfLoad = true;   // the TriggerLoad inside is ours
                ok = ::Mission::StateDump::Restore(g_runMission.savestate, sdErr);
                g_runSelfLoad = false;
                // StateDump restore cancels injection owners in its load preamble.
                SetRunnerStartupInputHold(true);
            }
            if (ok) {
                SkipRoundIntro("state restore");
                g_runSavePending = false;   // buffer already holds the baseline
                g_runStateSaved = true;
                LogOut("[MISSION][RUN] embedded start state restored (auto-retry armed)", true);
                PracticeMenu::TitleScreen::FinishLaunch();
            } else {
                LogOut("[MISSION][RUN] state restore failed (" + sdErr +
                       ") - falling back to value-level setup", true);
                ::Mission::Setup::Apply(g_runMission);   // chars already match
                g_runSavePending = true;    // classic baseline save instead
                PracticeMenu::TitleScreen::FinishLaunch();
            }
        }
    } else {
        g_runRestoreSettle = 0;
        if (!g_runRestorePending) g_runRestoreTimeout = 0;
    }

    // Mission baseline savestate: after load/reset, save once the first UNPAUSED
    // gameplay frame settles (menu closed, match running, past the round intro,
    // two stable ticks) so every drop can snap back to it before player input
    // or runner validation is released.
    if (g_runSavePending && !g_runRestorePending && s.valid && g_runActive.load() &&
        GetCurrentGamePhase() == GamePhase::Match && !ImGuiImpl::IsVisible() &&
        !::Mission::Setup::IsPending() && !CharacterHotswap::IsBusy() &&
        !g_recActive.load() && !RoundIntroActive()) {
        if (++g_runSaveSettle >= 2) {
            g_runSavePending = false;
            g_runSaveSettle = 0;
            if (SavestateHook::IsInstalled() && SavestateHook::TriggerSave()) {
                g_runStateSaved = true;
                LogOut("[MISSION][RUN] baseline savestate captured (auto-retry armed)", true);
            } else {
                LogOut("[MISSION][RUN] baseline save unavailable (no auto-retry)", true);
            }
        }
    } else {
        g_runSaveSettle = 0;
    }
    if (g_runStartupInputHeld.load(std::memory_order_acquire) &&
        !g_runRestorePending && !g_runSavePending) {
        SetRunnerStartupInputHold(false);
    }

    if (g_pendingMissionMode.load()) {
        const GamePhase phase = GetCurrentGamePhase();
        if (phase == GamePhase::CharacterSelect) {
            PracticeMenu::TitleScreen::SetLaunchStage(
                PracticeMenu::TitleScreen::LaunchStage::Fighters);
        } else if (phase == GamePhase::Loading) {
            PracticeMenu::TitleScreen::SetLaunchStage(
                PracticeMenu::TitleScreen::LaunchStage::Loading);
        }
    }

    // Guarded fallback only. Normal title missions go Title -> Loading -> Match
    // and create fighters on EFZ's game thread without visiting this screen.
    if (g_pendingMissionMode.load() && g_pendingHaveChars.load() &&
        !g_pendingCsQueued.load() &&
        GetCurrentGamePhase() == GamePhase::CharacterSelect &&
        CharacterHotswap::CanQueueReload()) {
        const int stage = g_pendingStage.load() >= 0 ? g_pendingStage.load() : 0;
        const int bgm = g_pendingBgm.load();
        CharacterHotswap::PaletteSelection palette{};
        palette.p1Color = g_pendingP1Palette.load();
        palette.p2Color = g_pendingP2Palette.load();
        if (CharacterHotswap::QueueReload(g_pendingP1SelectId.load(),
                                          g_pendingP2SelectId.load(), stage,
                                          palette, bgm)) {
            g_pendingCsQueued.store(true);
            LogOut("[MISSION] char select auto-drive queued (mission chars)", true);
        }
    }

    // A failed hotswap request is no longer "busy". Without an escape hatch the
    // launch cover would then sit over a perfectly usable character-select
    // screen forever. Give the driver a short grace window, then reveal manual
    // selection while keeping the pending mission load alive for match entry.
    static int s_pendingCsDriveIdle = 0;
    if (g_pendingMissionMode.load() && g_pendingHaveChars.load() &&
        g_pendingCsQueued.load() &&
        GetCurrentGamePhase() == GamePhase::CharacterSelect &&
        !CharacterHotswap::IsBusy()) {
        if (++s_pendingCsDriveIdle > 30) {
            s_pendingCsDriveIdle = 0;
            g_pendingHaveChars.store(false);
            PracticeMenu::TitleScreen::FinishLaunch();
            LogOut("[MISSION] character-select auto-drive stopped; revealing manual selection",
                   true);
            DirectDrawHook::AddMessage(
                "Automatic mission setup failed - select fighters manually",
                "MISSION", RGB(255, 180, 120), 2500, 0, 120);
        }
    } else {
        s_pendingCsDriveIdle = 0;
    }

    // Title-picked sessions used to wait 90 monitor ticks before loading the
    // runner. That was the visible ~0.5s where Ready/Fight and the unmodified
    // start state leaked through. Normalize from the first valid match tick;
    // two stable ticks are enough because CharacterHotswap has already observed
    // Match and invalidated/rebuilt its session caches earlier in this loop.
    static int s_missionEntrySettle = 0;
    if (g_pendingMissionMode.load() && s.valid &&
        GetCurrentGamePhase() == GamePhase::Match) {
        PracticeMenu::TitleScreen::SetLaunchStage(
            PracticeMenu::TitleScreen::LaunchStage::MatchSetup);
        SkipRoundIntro("pending mission entry");
        if (!CharacterHotswap::IsBusy() && ++s_missionEntrySettle >= 2) {
            s_missionEntrySettle = 0;
            g_pendingMissionMode.store(false);
            std::string path;
            {
                std::lock_guard<std::mutex> lk(g_pendingLoadMx);
                path.swap(g_pendingLoadPath);
            }
            if (!path.empty()) {
                // Title-picked mission: load it directly (Setup::Apply hotswaps).
                std::string msg;
                if (Runner::Load(path, msg, /*forceFreshMatch=*/false)) {
                    LogOut("[MISSION] mission mode: loaded '" + msg + "' after match entry", true);
                    // Embedded states keep the launch cover up through their
                    // pointer-reconciled restore; value-only missions are ready
                    // as soon as Runner::Load applies their setup.
                    if (!g_runRestorePending) {
                        PracticeMenu::TitleScreen::FinishLaunch();
                    }
                } else {
                    LogOut("[MISSION] mission mode: load failed: " + msg, true);
                    DirectDrawHook::AddMessage(("Mission load failed: " + msg).c_str(), "MISSION",
                                               RGB(255, 120, 120), 2500, 0, 120);
                    PracticeMenu::TitleScreen::FinishLaunch();
                }
            } else {
                PracticeMenu::TitleScreen::FinishLaunch();
                CustomMenu::Screens::OpenMissionBrowser();
                OpenMenu();
                LogOut("[MISSION] mission mode: opened browser after match entry", true);
            }
        }
    } else {
        s_missionEntrySettle = 0;
    }

    // Title RECORD entry: enter a visible pre-record phase as soon as the match
    // is stable. Capture does not begin until the player presses Macro Record.
    {
        static int s_recSettle = 0;
        const GamePhase recordPhase = GetCurrentGamePhase();
        if (g_pendingRecordMode.load() && recordPhase == GamePhase::CharacterSelect) {
            g_pendingRecordSawCharacterSelect.store(true, std::memory_order_release);
        }
        if (g_pendingRecordMode.load() &&
            g_pendingRecordSawCharacterSelect.load(std::memory_order_acquire) &&
            recordPhase == GamePhase::Menu) {
            // Backing out of Character Select cancels this title transaction.
            // Otherwise an unrelated later Practice match would unexpectedly
            // enter mission authoring.
            g_pendingRecordMode.store(false, std::memory_order_release);
            g_pendingRecordSawCharacterSelect.store(false, std::memory_order_release);
            LogOut("[MISSION][REC] pending title authoring canceled on Character Select exit", true);
        }
        if (g_pendingRecordMode.load() && s.valid &&
            recordPhase == GamePhase::Match &&
            !CharacterHotswap::IsBusy() && !::Mission::Setup::IsPending()) {
            SkipRoundIntro("mission authoring entry");
            if (++s_recSettle >= 2) {
                s_recSettle = 0;
                g_pendingRecordMode.store(false);
                g_pendingRecordSawCharacterSelect.store(false, std::memory_order_release);
                Recorder::Arm();
            }
        } else {
            s_recSettle = 0;
        }
    }

    TickDemo(s);
    TickRecorderFrontend(s);
    if (s.valid && g_recActive.load()) {
        RecorderCapture(s, MacroController::DidAdvanceRecordingTick());
    }
    // Runner validation waits until the mission's setup actually landed - with
    // a restore or value-apply still pending the player would be "playing" a
    // mission whose start state is not set yet.
    if (s.valid && g_runActive.load() && !g_runRestorePending && !g_runSavePending &&
        !::Mission::Setup::IsPending() && !Demo::IsActive() && !g_demoFinishedThisTick) {
        TickRun(s);
    }
    if (g_inspector.load() && s.valid) {
        DrawInspector(s);
    }
    RefreshRecorderBanner();
}

Snapshot GetSnapshot() {
    std::lock_guard<std::recursive_mutex> lock(g_runStateMx);
    return g_snapshot;
}

void SetPendingMissionMode(bool on) {
    g_pendingMissionMode.store(on);
    if (on) {
        g_pendingRecordMode.store(false);   // mutually exclusive title picks
        g_pendingRecordSawCharacterSelect.store(false, std::memory_order_release);
    }
}

void SetPendingRecordMode(bool on) {
    g_pendingRecordMode.store(on);
    g_pendingRecordSawCharacterSelect.store(false, std::memory_order_release);
    if (on) {
        g_pendingMissionMode.store(false);
        std::lock_guard<std::mutex> lk(g_pendingLoadMx);
        g_pendingLoadPath.clear();
    }
}

void SetPendingMissionLoad(const std::string& missionPath) {
    // A title launch owns presentation until its baseline is ready. Remove the
    // previous attempt's short-lived status immediately instead of letting it
    // leak through the launch cover.
    DirectDrawHook::RemoveMessagesByCategory("mission_run");
    {
        std::lock_guard<std::mutex> lk(g_pendingLoadMx);
        g_pendingLoadPath = missionPath;
    }
    // Pre-parse the pinned mission matchup before leaving Title. The direct
    // request mirrors Replay PLAY's supported return-to-Loading route while
    // keeping game mode 1; Character Select remains a validated fallback.
    g_pendingHaveChars.store(false);
    g_pendingCsQueued.store(false);
    ::Mission::Mission m;
    std::string err;
    if (::Mission::LoadMission(missionPath, m, err)) {
        const int p1SelectId = CharacterHotswap::GetSelectIdForResourceName(
            m.player.character.c_str());
        const int p2SelectId = CharacterHotswap::GetSelectIdForResourceName(
            m.dummy.character.c_str());
        if (p1SelectId >= 0 && p2SelectId >= 0) {
            g_pendingP1SelectId.store(p1SelectId);
            g_pendingP2SelectId.store(p2SelectId);
            g_pendingP1Palette.store(m.player.palette);
            g_pendingP2Palette.store(m.dummy.palette);
            g_pendingStage.store(m.stage);
            g_pendingBgm.store(m.bgm);
            g_pendingHaveChars.store(true);
            LogOut("[MISSION] pending resource-to-select mapping " +
                   m.player.character + "=" + std::to_string(p1SelectId) + " " +
                   m.dummy.character + "=" + std::to_string(p2SelectId), true);

            CharacterHotswap::PaletteSelection palette{};
            palette.p1Color = m.player.palette;
            palette.p2Color = m.dummy.palette;
            const int stage = m.stage >= 0 ? m.stage : 0;
            if (CharacterHotswap::QueueDirectPracticeLoad(
                    p1SelectId, p2SelectId, stage, palette, m.bgm)) {
                PracticeMenu::TitleScreen::SetLaunchStage(
                    PracticeMenu::TitleScreen::LaunchStage::Fighters);
                LogOut("[MISSION] direct Practice Loading armed (Character Select skipped)", true);
            } else {
                LogOut("[MISSION] direct Practice Loading unavailable; selector fallback armed", true);
            }
        } else {
            LogOut("[MISSION] cannot prepare pinned matchup: unresolved resource names p1='" +
                   m.player.character + "' p2='" + m.dummy.character + "'", true);
            PracticeMenu::TitleScreen::FinishLaunch();
        }
    } else {
        LogOut("[MISSION] cannot pre-parse pending mission: " + err, true);
        PracticeMenu::TitleScreen::FinishLaunch();
    }
    g_pendingMissionMode.store(true);
    g_pendingRecordMode.store(false);   // a mission pick cancels a pending record
    g_pendingRecordSawCharacterSelect.store(false, std::memory_order_release);
    LogOut("[MISSION] pending mission load queued: " + missionPath +
            (CharacterHotswap::IsDirectPracticeLoadPending()
                ? " (direct Loading)"
                : (g_pendingHaveChars.load() ? " (Character Select fallback)"
                                       : " (no chars in json - manual select)")), true);
}

void SetInspectorEnabled(bool on) {
    if (!on && g_inspector.load()) {
        DirectDrawHook::RemoveMessagesByCategory("mission_inspector");
    }
    g_inspector.store(on);
}
bool IsInspectorEnabled() { return g_inspector.load(); }

namespace Recorder {

void SetActive(bool on) {
    if (on) Arm(); else Cancel();
}
void Arm()    { g_recCommand.store(RecorderCommand::Arm, std::memory_order_release); }
void Advance(){
    if (g_recPhase.load(std::memory_order_acquire) == Phase::Recording) {
        MacroController::RequestRecordingStopAtBoundary();
    }
    g_recCommand.store(RecorderCommand::Advance, std::memory_order_release);
}
void Retake() { g_recCommand.store(RecorderCommand::Retake, std::memory_order_release); }
void Cancel() { g_recCommand.store(RecorderCommand::Cancel, std::memory_order_release); }
bool IsActive() { return g_recActive.load(std::memory_order_acquire); }
bool IsSessionActive() { return g_recPhase.load(std::memory_order_acquire) != Phase::Idle; }
bool OwnsCaptureHotkeys() {
    const Phase phase = g_recPhase.load(std::memory_order_acquire);
    return phase == Phase::CountIn || phase == Phase::Recording;
}
Phase GetPhase() { return g_recPhase.load(std::memory_order_acquire); }
int  GetStepCount() { return g_recStepCountView.load(std::memory_order_acquire); }
int  GetComboEndCount() { return g_recComboEndCountView.load(std::memory_order_acquire); }
int  GetCountInValue() { return g_recCountInView.load(std::memory_order_acquire); }
std::string GetMacroRecordBindingLabel() { return MacroRecordBindingLabel(); }

} // namespace Recorder

namespace Demo {

bool PlayLoaded(std::string& outMsg) {
    std::lock_guard<std::recursive_mutex> lock(g_runStateMx);
    outMsg.clear();
    if (!g_runActive.load(std::memory_order_acquire)) {
        outMsg = "load a mission first";
        return false;
    }
    if (g_runMission.demo.empty()) {
        outMsg = "loaded mission has no demonstration";
        return false;
    }
    std::string parseError;
    if (!MacroController::ValidateSerialized(g_runMission.demo, parseError)) {
        outMsg = "invalid demonstration: " + parseError;
        return false;
    }
    if (Recorder::IsSessionActive()) {
        outMsg = "finish or discard mission recording first";
        return false;
    }
    if (MacroController::GetState() != MacroController::State::Idle) {
        outMsg = "stop the active macro first";
        return false;
    }
    if (GetCurrentGamePhase() != GamePhase::Match) {
        outMsg = "demonstrations are available during Match";
        return false;
    }
    if (g_demoPhase.load(std::memory_order_acquire) != Phase::Idle) {
        outMsg = "demonstration is already active";
        return false;
    }

    g_demoText = g_runMission.demo;
    DirectDrawHook::RemoveMessagesByCategory("mission_run");
    g_demoFromRecorder = false;
    g_demoCancelRequested.store(false, std::memory_order_release);
    g_demoBaselineIssued = false;
    g_demoSettleFrames = 0;
    g_demoPhase.store(Phase::Preparing, std::memory_order_release);
    outMsg = "preparing demonstration";
    LogOut("[MISSION][DEMO] request queued", true);
    return true;
}

bool PlayRecording(std::string& outMsg) {
    std::lock_guard<std::recursive_mutex> lock(g_runStateMx);
    outMsg.clear();
    if (Recorder::GetPhase() != Recorder::Phase::Review) {
        outMsg = "finish the capture and enter Review first";
        return false;
    }
    if (MacroController::GetState() != MacroController::State::Idle) {
        outMsg = "stop the active macro first";
        return false;
    }
    if (GetCurrentGamePhase() != GamePhase::Match) {
        outMsg = "previews are available during Match";
        return false;
    }
    if (g_demoPhase.load(std::memory_order_acquire) != Phase::Idle) {
        outMsg = "demonstration is already active";
        return false;
    }

    const std::string clip = MacroController::SerializeLastPlayerRecording(true);
    std::string parseError;
    if (!MacroController::ValidateSerialized(clip, parseError)) {
        outMsg = "recorded clip is invalid: " + parseError;
        return false;
    }
    g_demoText = clip;
    DirectDrawHook::RemoveMessagesByCategory("mission_run");
    g_demoFromRecorder = true;
    g_demoCancelRequested.store(false, std::memory_order_release);
    g_demoBaselineIssued = false;
    g_demoSettleFrames = 0;
    g_demoPhase.store(Phase::Preparing, std::memory_order_release);
    outMsg = "preparing recorded preview";
    LogOut("[MISSION][DEMO] unsaved Review preview queued", true);
    return true;
}

void Cancel() {
    if (IsActive()) g_demoCancelRequested.store(true, std::memory_order_release);
}
bool IsActive() { return g_demoPhase.load(std::memory_order_acquire) != Phase::Idle; }
Phase GetPhase() { return g_demoPhase.load(std::memory_order_acquire); }

} // namespace Demo

void NotifyPracticeSessionReset(const char* reason) {
    std::lock_guard<std::recursive_mutex> lock(g_runStateMx);
    const RecorderPhase recorderPhase =
        g_recPhase.load(std::memory_order_acquire);
    const DemoPhase demoPhase = g_demoPhase.load(std::memory_order_acquire);
    const bool hadRecorder = recorderPhase != RecorderPhase::Idle;
    const bool hadDemo = demoPhase != DemoPhase::Idle;

    // Reset synchronously: this path is also used while the frame monitor is
    // suspended, where queuing Recorder::Cancel/Demo::Cancel would leave stale
    // phase state and permanent-message handles until an unrelated later Match.
    if ((hadRecorder || hadDemo) &&
        MacroController::GetState() != MacroController::State::Idle) {
        MacroController::Stop();
    }

    g_recCommand.store(RecorderCommand::None, std::memory_order_release);
    g_recActive.store(false, std::memory_order_release);
    g_recPhase.store(RecorderPhase::Idle, std::memory_order_release);
    g_recReleaseFrames = 0;
    g_recCountInFrames = 0;
    ResetRecorderCaptureState();
    g_recSetup = ::Mission::Mission();
    DirectDrawHook::RemoveMessagesByCategory("mission_record");
    RemoveRecorderBanner();

    if (hadDemo) {
        MacroController::ReleaseExclusivePlaybackHold();
        HoldDemoP1Neutral(false);
    }
    g_demoCancelRequested.store(false, std::memory_order_release);
    g_demoPhase.store(DemoPhase::Idle, std::memory_order_release);
    g_demoBaselineIssued = false;
    g_demoSettleFrames = 0;
    g_demoFinishedThisTick = false;
    g_demoText.clear();
    g_demoFromRecorder = false;

    if (hadRecorder || hadDemo) {
        LogOut(std::string("[MISSION] cleared ") +
               (hadRecorder && hadDemo ? "authoring and demonstration" :
                hadRecorder ? "authoring" : "demonstration") +
               " ownership for Practice-session reset reason=" +
               (reason && *reason ? reason : "unspecified"), true);
    }
}

// A savestate was just restored (Revival hotkey or our own auto-retry). Timing
// measured across a rollback is meaningless, so both consumers resynchronize.
void NotifyStateLoaded() {
    // Recorder: abort the synchronized take as a unit. Clearing only recipe
    // steps while the macro stream kept its pre-load bytes created an impossible
    // recipe/demo pair. The author explicitly starts a fresh count-in instead.
    if (g_recActive.load()) {
        MacroController::Stop();
        g_recActive.store(false, std::memory_order_release);
        ResetRecorderCaptureState();
        g_recReleaseFrames = 0;
        g_recCountInFrames = 0;
        g_recPhase.store(RecorderPhase::PreRecord, std::memory_order_release);
        ShowRecorderState("State loaded: take discarded; press Macro Record for a fresh count-in",
                          RGB(255, 200, 120), 1800);
        LogOut("[MISSION][REC] savestate load - discarded synchronized recipe+macro take", true);
    }
    // Runner: a MANUAL load mid-run restarts the attempt (our own auto-retry
    // load already reset progress in RunnerDrop, so it is filtered out here).
    if (g_runActive.load() && !g_runSelfLoad) {
        Runner::Reset();
        LogOut("[MISSION][RUN] manual savestate load - run reset", true);
    }
}

namespace Recorder {

bool BuildDraft(::Mission::Mission& out) {
    if (g_recSteps.empty()) return false;
    ::Mission::Mission m;
    m.name = "Recorded Combo";
    m.type = "combo";
    // Setup metadata captured at record start (chars/pos/HP/meter/RF/IC/resources).
    m.player = g_recSetup.player;
    m.dummy  = g_recSetup.dummy;
    m.stage  = g_recSetup.stage;
    m.bgm    = g_recSetup.bgm;
    m.savestate = g_recSetup.savestate;   // exact start state (may be empty)
    for (const CapturedStep& cs : g_recSteps) {
        ::Mission::Step st;
        st.notation = cs.notation;        // auto-derived; author refines in the editor
        st.moveIds.push_back(static_cast<int>(cs.moveId));
        st.req = static_cast<::Mission::StepReq>(cs.req);
        st.optional = cs.optional;
        st.comboEndAfter = cs.comboEndAfter;
        // Multi-hit move: require the observed hit count.
        if (cs.landed && cs.hits > 1) {
            st.req = ::Mission::StepReq::Hits;
            st.hitsRequired = cs.hits;
        }
        // Contact without a combo hit = the move was BLOCKED (blockstring step):
        // require contact on replay. Never-connected stays req=Move (a whiff).
        if (!cs.landed && cs.connected) {
            st.req = ::Mission::StepReq::Connect;
        }
        // Delayed (projectile/summon) land: give the runner a generous window
        // around the observed delay so timing variance doesn't fail the step.
        if (cs.maxDelay > 0) st.maxDelay = cs.maxDelay * 2 + 30;
        // Preserve every observed next-button gap, not only delays beyond an
        // arbitrary 45-tick threshold. After an explicit combo.end this is timed
        // from the recovery edge. A large safety ceiling prevents malformed takes
        // from creating an effectively infinite trial while retaining up to 30s.
        if (cs.gap > 0) {
            st.maxGap = ::Mission::SequencePolicy::RecordedGapAllowance(cs.gap);
        }
        m.steps.push_back(st);
    }
    // Demo = the P1 clip captured in lockstep with this authoring session. It is
    // ephemeral and never aliases/overwrites the user's ordinary P2 macro slot.
    m.demo = MacroController::SerializeLastPlayerRecording(true);
    if (m.demo == "EFZMACRO 1") m.demo.clear();
    m.scores.push_back(::Mission::ScoreTier{ 0, 0, 0, "Clear" });
    out = std::move(m);
    return true;
}

bool SaveRecorded(std::string& outMsg) {
    const Phase phase = GetPhase();
    if (phase == Phase::Recording || phase == Phase::CountIn || phase == Phase::PreRecord) {
        outMsg = "finish the capture and enter Review first";
        return false;
    }
    ::Mission::Mission m;
    if (!BuildDraft(m)) { outMsg = "no steps captured"; return false; }

    const std::string root = ::Mission::ResolveMissionsRoot();
    if (root.empty()) { outMsg = "cannot resolve missions root"; return false; }
    const std::string dir = root + "\\_recorded";
    CreateDirectoryA(root.c_str(), nullptr);
    CreateDirectoryA(dir.c_str(), nullptr);
    char name[64];
    _snprintf_s(name, sizeof(name), _TRUNCATE, "recorded_%lu.json", static_cast<unsigned long>(GetTickCount()));
    const std::string path = dir + "\\" + name;

    std::string err;
    if (!::Mission::SaveMission(path, m, err)) { outMsg = err; return false; }
    outMsg = path;
    g_recPhase.store(Phase::Idle, std::memory_order_release);
    DirectDrawHook::RemoveMessagesByCategory("mission_record");
    RemoveRecorderBanner();
    CustomMenu::Screens::NotifyMissionLibraryChanged();
    LogOut("[MISSION][REC] saved " + std::to_string(m.steps.size()) + " step(s) -> " + path, true);
    return true;
}

} // namespace Recorder

namespace Runner {

static void ResetProgress() {
    g_runPhase = Phase::Idle;
    g_runStep = 0;
    g_runFailedStep.store(-1);
    g_runBaseline = 0;
    g_runGapWindow.Reset();
    g_runBestTier = -1;
    g_runArmed = false;
    g_runArmedFrames = 0;
    g_runPrevMove = 0;
    g_runPrevFrameIdx = 0;
    g_runComboWasAlive = false;
    g_runPrevComboCount = 0;
    g_runPrevInputs = 0;
    g_runAwaitingComboEnd = false;
    g_runPrevHitState = 0;
    g_runConnectSeen = false;
    ResetRunScore();
}

bool Load(const std::string& path, std::string& outMsg, bool forceFreshMatch) {
    if (Demo::IsActive()) {
        outMsg = "cancel the active demonstration first";
        return false;
    }
    if (Recorder::IsSessionActive()) {
        outMsg = "save or discard the active recording session first";
        return false;
    }
    ::Mission::Mission m;
    std::string err;
    if (!::Mission::LoadMission(path, m, err)) { outMsg = err; return false; }
    if (m.steps.empty()) { outMsg = "mission has no steps: " + path; return false; }
    for (size_t i = 0; i < m.steps.size(); ++i) {
        if (!m.steps[i].comboEndAfter) continue;
        if (m.steps[i].optional) {
            outMsg = "comboEndAfter cannot be attached to optional step " +
                     std::to_string(i);
            return false;
        }
        if (i + 1 >= m.steps.size()) {
            outMsg = "comboEndAfter requires a following setup/Part-2 step at " +
                     std::to_string(i);
            return false;
        }
    }

    std::lock_guard<std::recursive_mutex> lock(g_runStateMx);
    // Parsing happened without blocking the monitor/render threads.  Recheck
    // ownership now that mission state can be published atomically.
    if (Demo::IsActive()) {
        outMsg = "cancel the active demonstration first";
        return false;
    }
    if (Recorder::IsSessionActive()) {
        outMsg = "save or discard the active recording session first";
        return false;
    }
    // A mission startup owns P1 until its exact baseline is ready.  Cancel
    // ordinary macros and motion queues before enabling the neutral poll hold,
    // otherwise an immediate-input owner can contaminate that baseline.
    CancelAutoActionsAndMacros();
    g_runMission = std::move(m);
    g_runAttempts = 0;
    ResetProgress();
    g_runStateSaved = false;
    g_runSaveSettle = 0;
    g_runRestoreSettle = 0;
    // Embedded start state: hotswap only, then restore the dump on settle (it
    // becomes the retry baseline). Otherwise: value-level setup + baseline save.
    g_runRestorePending = !g_runMission.savestate.empty() && ::Mission::StateDump::Available();
    g_runSavePending = !g_runRestorePending;
    g_runActive.store(true);
    SetRunnerStartupInputHold(true);
    // Restore the mission's recorded setup (hotswaps characters if they differ).
    if (!::Mission::Setup::Apply(g_runMission,
                                 /*applyValues=*/!g_runRestorePending,
                                 forceFreshMatch,
                                 /*allowReload=*/true,
                                 /*acceptPreparedSession=*/!forceFreshMatch)) {
        g_runActive.store(false);
        g_runSavePending = false;
        g_runRestorePending = false;
        SetRunnerStartupInputHold(false);
        outMsg = "mission setup could not start a clean Practice session";
        LogOut("[MISSION][RUN] load aborted: " + outMsg, true);
        return false;
    }
    if (g_runRestorePending) {
        LogOut("[MISSION][RUN] embedded start state present - will restore on settle", true);
    }
    outMsg = g_runMission.name.empty() ? path : g_runMission.name;
    LogOut("[MISSION][RUN] loaded '" + outMsg + "' (" + std::to_string(g_runMission.steps.size()) + " steps)", true);
    return true;
}

bool LoadLatestRecorded(std::string& outMsg) {
    const std::string dir = ::Mission::ResolveMissionsRoot();
    if (dir.empty()) { outMsg = "cannot resolve missions root"; return false; }
    const std::string search = dir + "\\_recorded\\*.json";
    WIN32_FIND_DATAA fd = {};
    HANDLE h = FindFirstFileA(search.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) { outMsg = "no recorded missions found"; return false; }
    std::string best; FILETIME bestTime = {};
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (best.empty() || CompareFileTime(&fd.ftLastWriteTime, &bestTime) > 0) {
            bestTime = fd.ftLastWriteTime;
            best = dir + "\\_recorded\\" + fd.cFileName;
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    if (best.empty()) { outMsg = "no recorded missions found"; return false; }
    return Load(best, outMsg);
}

void Unload() {
    std::lock_guard<std::recursive_mutex> lock(g_runStateMx);
    if (Demo::IsActive()) {
        MacroController::Stop();
        MacroController::ReleaseExclusivePlaybackHold();
        HoldDemoP1Neutral(false);
        g_demoText.clear();
        g_demoFromRecorder = false;
        g_demoBaselineIssued = false;
        g_demoCancelRequested.store(false, std::memory_order_release);
        g_demoPhase.store(DemoPhase::Idle, std::memory_order_release);
    }
    g_runActive.store(false);
    g_runSavePending = false;
    g_runStateSaved = false;
    g_runRestorePending = false;
    g_runRestoreSettle = 0;
    g_runRestoreTimeout = 0;
    SetRunnerStartupInputHold(false);
    DirectDrawHook::RemoveMessagesByCategory("mission_run");
    ResetProgress();
}

void Reset() {
    std::lock_guard<std::recursive_mutex> lock(g_runStateMx);
    // Retry: keep the mission, count the abandoned run as an attempt.
    if (g_runPhase == Phase::InProgress || g_runPhase == Phase::Complete) g_runAttempts++;
    ResetProgress();
}

bool  IsActive() { return g_runActive.load(std::memory_order_acquire); }
Phase GetPhase() {
    std::lock_guard<std::recursive_mutex> lock(g_runStateMx);
    return g_runPhase;
}
int CurrentStep() {
    std::lock_guard<std::recursive_mutex> lock(g_runStateMx);
    return g_runStep;
}
int FailedStep() {
    std::lock_guard<std::recursive_mutex> lock(g_runStateMx);
    return g_runFailedStep.load(std::memory_order_acquire);
}
int StepCount() {
    std::lock_guard<std::recursive_mutex> lock(g_runStateMx);
    return static_cast<int>(g_runMission.steps.size());
}
int Attempts() {
    std::lock_guard<std::recursive_mutex> lock(g_runStateMx);
    return g_runAttempts;
}
int BestTier() {
    std::lock_guard<std::recursive_mutex> lock(g_runStateMx);
    return g_runBestTier;
}
int   CurrentStepHits() {
    std::lock_guard<std::recursive_mutex> lock(g_runStateMx);
    if (!g_runArmed) return 0;
    const int d = g_snapshot.p1Combo - g_runBaseline;
    return d > 0 ? d : 0;
}
bool CurrentStepArmed() {
    std::lock_guard<std::recursive_mutex> lock(g_runStateMx);
    return g_runArmed;
}

bool IsReadyForPlayer() {
    std::lock_guard<std::recursive_mutex> lock(g_runStateMx);
    return g_runActive.load(std::memory_order_acquire) &&
           !g_runRestorePending && !g_runSavePending &&
           !g_runStartupInputHeld.load(std::memory_order_acquire) &&
           !Demo::IsActive();
}

bool GetRenderSnapshot(::Mission::Mission& missionOut, int& currentStepOut,
                       int& failedStepOut, bool& armedOut, int& currentHitsOut) {
    std::lock_guard<std::recursive_mutex> lock(g_runStateMx);
    if (!g_runActive.load(std::memory_order_acquire)) return false;

    // Copy only fields consumed by mission_render.  In particular, do not copy
    // the potentially large base64 savestate or demonstration every draw.
    missionOut = ::Mission::Mission{};
    missionOut.type = g_runMission.type;
    missionOut.name = g_runMission.name;
    missionOut.description = g_runMission.description;
    missionOut.steps = g_runMission.steps;
    missionOut.hints = g_runMission.hints;
    currentStepOut = g_runStep;
    failedStepOut = g_runFailedStep.load(std::memory_order_acquire);
    armedOut = g_runArmed;
    const int hits = armedOut ? g_snapshot.p1Combo - g_runBaseline : 0;
    currentHitsOut = hits > 0 ? hits : 0;
    return true;
}

} // namespace Runner

} // namespace Mission::Engine
