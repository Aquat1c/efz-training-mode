#include "../../../include/game/mission/mission_engine.h"
#include "../../../include/game/mission/mission_data.h"
#include "../../../include/game/mission/mission_moves.h"
#include "../../../include/game/mission/mission_movedata.h"
#include "../../../include/game/mission/mission_sequence_policy.h"
#include "../../../include/game/mission/mission_setup.h"
#include "../../../include/game/mission/recorder_entity_trace.h"
#include "../../../include/game/mission/move_notation_tables.h"  // per-char notation (generated)
#include "../../../include/game/character_settings.h"            // GetCharacterInternalName
#include "../../../include/game/macro_controller.h"
#include "../../../include/game/collision_hook.h"
#include "../../../include/game/collision_display.h"  // bounded raw entity-ring probe

#include "../../../include/core/constants.h"
#include "../../../include/core/logger.h"
#include "../../../include/core/memory.h"
#include "../../../include/utils/utilities.h"
#include "../../../include/gui/overlay.h"
#include "../../../include/gui/gui.h"                 // OpenMenu (mission mode)
#include "../../../include/gui/imgui_impl.h"           // menu visibility (baseline save gate)
#include "../../../include/game/savestate_hook.h"      // TrialMode-style auto retry
#include "../../../include/game/mission/mission_pause_menu.h" // session pause surface
#include "../../../include/game/mission/tutorial_session.h"    // tutorialSchema runtime
#include "../../../include/game/mission/tutorial_support.h"    // lesson launch preflight
#include "../../../include/game/mission/mission_state_dump.h" // embedded start states
#include "../../../include/game/character_hotswap.h"   // char-select auto-drive
#include "../../../include/gui/custom_menu/screens.h" // OpenMissionBrowser
#include "../../../include/game/game_state.h"
#include "../../../include/game/auto_action.h"
#include "../../../include/input/injection_control.h"
#include "../../../include/input/input_hook.h"
#include "../../../include/utils/config.h"
#include "../../../include/utils/pause_integration.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace Mission::Engine {

namespace {

Snapshot g_snapshot;
uint32_t g_contactReadCursor = 0;
uint32_t g_contactReadEpoch = 0;
uint32_t g_contactOverflowLoggedEpoch = 0;
uint32_t g_contactOverflowLoggedCursor = 0;
std::atomic<bool> g_inspector{false};
std::atomic<bool> g_pendingMissionMode{false}; // title MISSION -> auto-open browser
std::atomic<bool> g_pendingMissionSawCharacterSelect{false};
std::atomic<bool> g_pendingRecordMode{false};  // title RECORD -> auto-arm recorder
std::atomic<bool> g_pendingRecordSawCharacterSelect{false};
// One title-picked launch is prepared before the browser releases ownership.
// Both fields are guarded by the same mutex and are published/consumed/cleared
// together: the source path remains presentation/diagnostic identity, while the
// parsed Mission is the immutable runtime payload. Match entry must never
// reopen a file that may have changed after the player selected it.
std::mutex  g_pendingLoadMx;
std::string g_pendingLoadPath;
std::optional<::Mission::Mission> g_pendingLoadMission;
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

void ClearPendingMatchMetadata() {
    g_pendingHaveChars.store(false, std::memory_order_release);
    g_pendingCsQueued.store(false, std::memory_order_release);
    g_pendingP1SelectId.store(-1, std::memory_order_relaxed);
    g_pendingP2SelectId.store(-1, std::memory_order_relaxed);
    g_pendingP1Palette.store(0, std::memory_order_relaxed);
    g_pendingP2Palette.store(0, std::memory_order_relaxed);
    g_pendingStage.store(-1, std::memory_order_relaxed);
    g_pendingBgm.store(-1, std::memory_order_relaxed);
}

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
    bool        connected = false; // legacy +0x168 sampled-edge heuristic used by
                                   // format 1; not an authoritative typed contact
    int         charState = -1;    // P1 character-state stamp at move start (-1 = n/a;
                                   // Akiko = bullet cycle 0..2 -> rekka crit variant)
    int         damage = 0;        // combo-damage delta this step's hits produced
                                   // (0 = whiff/blocked/no requirement)
    short       automaticFollowupId = 0; // observed no-input phase accepted as part
                                         // of this action (character-specific)
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
std::atomic<unsigned long> g_recSaveSequence{0};
struct RecorderBannerInputs {
    RecorderPhase phase = RecorderPhase::Idle;
    int countIn = 0;
    int steps = 0;
    int comboEnds = 0;
    int displayedSecond = -1;
    bool interrupted = true;
    std::string binding;
};
std::mutex g_recBannerMx;
RecorderBannerInputs g_recBannerInputs;
bool g_recBannerInputsValid = false;
int g_recReleaseFrames = 0;
int g_recCountInFrames = 0;
int g_recCountInTargetTicks = 0;
bool g_recCountInElapsed = false; // capture/start on the following clear tick
std::vector<CapturedStep> g_recSteps;   // kept after stop until the next start / save
short g_recLastAttackId = 0;   // last attack move-ID recorded (0 = none / reset)
int   g_recComboAtStart = 0;   // combo count when the current step's move began
int   g_recDamageAtStart = 0;  // combo damage when the current step's move began
                               // (per-step delta = clean-hit/crit variant fingerprint)
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
int   g_recPrevHitState = 0;   // previous raw +0x168 (legacy edge heuristic)
short g_recPrevMove = 0;       // same-ID re-entry detection (5A -> 5A, etc.)
short g_recPrevFrameIdx = 0;
// Fresh-input evidence for same-move re-entry: a genuine re-cancel is caused
// by a button press or a newly recognized command; an animation-loop frame
// wrap has neither. Ages are in internal ticks and freeze-gated like g_recFrame.
int   g_recAttackEdgeAge = 999;   // ticks since EFZ consumed a P1 attack-button edge
int   g_recTokenEdgeAge = 999;    // ticks since the token went 99 -> value (edge, not level:
                                  // +0x262 can hold its value for a move's whole duration)
short g_recPrevTokenSample = 99;
int   g_recAkikoRekkaRep = 0;     // C-rekka rep index (Akiko crosses behind per rep, so
                                  // the PHYSICAL input alternates 623C/421C)

// Authoring-only, cast-wide raw entity trace. This is deliberately separate
// from CapturedStep: sampled ring edges cannot become strict format-1 recipe
// requirements or notation suffixes. The fixed POD buffer bounds memory even
// during an abnormally long take; overflow is written into the sidecar header.
enum class RecEntityTraceKind : uint8_t {
    Baseline = 0,
    Spawn,
    Morph,
    Despawn,
    Contact,
};

struct RecEntitySlotState {
    bool established = false;
    bool alive = false;
    uint16_t pattern = 0;
    uint16_t frame = 0;
    int16_t life = 0;
    uint32_t destroyed = 0;
};

struct RecEntityTraceEvent {
    RecEntityTraceKind kind = RecEntityTraceKind::Baseline;
    int effectiveFrame = 0;
    int player = 1;
    int characterId = -1;
    int afterStep = -1;       // zero-based CapturedStep index; -1 = before opener
    int slot = -1;
    int pattern = -1;         // current pattern; -1 on a sampled despawn
    int priorPattern = -1;
    int entityFrame = 0;
    int life = 0;
    uint32_t destroyed = 0;
    ::Mission::MoveData::Cls entityClass = ::Mission::MoveData::Cls::Unknown;
    bool attack = false;
    bool sampled = true;

    // Populated only for exact entity->player resolver journal records.
    uint32_t contactSequence = 0;
    uint32_t contactBatch = 0;
    int defender = 0;
    ::Mission::Contact::Result contactResult = ::Mission::Contact::Result::None;
    int lifeBefore = 0;
    int destroyedBefore = 0;
    int comboBefore = 0;
    int comboAfter = 0;
    int hpBefore = 0;
    int hpAfter = 0;
};

constexpr std::size_t kRecEntityTraceCapacity = 4096;
std::array<std::array<RecEntitySlotState,
                      CollisionDisplay::kProjectileRingSlotCapacity>, 2>
    g_recEntitySlots{};
std::array<RecEntityTraceEvent, kRecEntityTraceCapacity> g_recEntityTrace{};
std::size_t g_recEntityTraceCount = 0;
uint32_t g_recEntityTraceDropped = 0;
bool g_recEntityContactHookObserved = false;
bool g_recContactJournalOverflowObserved = false;
std::array<bool, 2> g_recEntityProbeUnavailableLogged{};
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
    const bool hasButton = (in & (INPUT_A | INPUT_B | INPUT_C | INPUT_D)) != 0;
    std::string s;
    // Neutral (5) with a button is written as just the button ("5A" -> "A"); keep the
    // digit for pure directions and every non-neutral input (2A, 6C, dash, jump...).
    if (!(numpad == 5 && hasButton)) s.push_back(static_cast<char>('0' + numpad));
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
int  g_runDamageBaseline = 0; // combo damage at the current step's baseline
                              // (per-step delta check against Step::damage)
int  g_runRetryDelay = 0;   // monitor ticks left before the auto-retry load fires
                            // (grace so the player can SEE what failed)
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
int  g_runPrevHitState = 0;      // previous raw +0x168 (legacy edge heuristic)
bool g_runConnectSeen = false;   // format-1 Connect heuristic observed an edge
// Score across explicit combo parts. EFZ's live combo fields reset at recovery,
// so retain each segment's maxima before consuming combo.end.
::Mission::SequencePolicy::SegmentScore g_runScore;
// TrialMode-style retry: on mission load/reset, SAVE (Revival savestate) once the
// first unpaused gameplay frame settles; every DROP auto-LOADs it, snapping the
// match back to the mission's start state for the next attempt.
bool g_runSavePending = false;   // a baseline save is owed
bool g_runStateSaved = false;    // baseline exists -> drops may auto-load
int  g_runSaveSettle = 0;        // consecutive eligible ticks before saving
int  g_runSaveRetries = 0;       // failed TriggerSave attempts (controller race)
int  g_runSaveElapsed = 0;       // all startup ticks, including pre-eligibility stalls
std::string g_runSaveBlocker;    // latest concrete reason the checkpoint could not settle
constexpr int kRunSaveDeadlineTicks = 5760; // ~30 seconds at the 192 Hz monitor cadence
bool g_runSelfLoad = false;      // a TriggerLoad WE issued is in flight (so the
                                 // savestate-load notification can tell a manual
                                 // load from our own auto-retry)
class ScopedRunnerSelfLoad {
public:
    ScopedRunnerSelfLoad() : previous_(g_runSelfLoad) { g_runSelfLoad = true; }
    ~ScopedRunnerSelfLoad() { g_runSelfLoad = previous_; }
    ScopedRunnerSelfLoad(const ScopedRunnerSelfLoad&) = delete;
    ScopedRunnerSelfLoad& operator=(const ScopedRunnerSelfLoad&) = delete;
private:
    bool previous_;
};
// Embedded start state (Mission.savestate): restore is deferred until the
// match settles after the mission's hotswap; the restored buffer then doubles
// as the auto-retry baseline (no separate baseline save needed).
bool g_runRestorePending = false;
int  g_runRestoreSettle = 0;
int  g_runRestoreTimeout = 0;   // stuck-pending safety (exact startup fails closed)
std::atomic<bool> g_runStartupInputHeld{false};
bool g_runStartupPollSnapshotValid = false;
bool g_runStartupPollSavedActive = false;
uint8_t g_runStartupPollSavedMask = 0;

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
bool g_demoTutorialHandoff = false;
Demo::RestoreResult g_demoRestoreResult = Demo::RestoreResult::None;
std::string g_demoRestoreMessage;

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
    int id = -1;
    {
        std::lock_guard<std::mutex> lock(g_recBannerMx);
        g_recBannerInputsValid = false;
        id = g_recBannerId.exchange(-1, std::memory_order_acq_rel);
    }
    if (id != -1) DirectDrawHook::RemovePermanentMessage(id);
}

bool SameRecorderBannerInputs(const RecorderBannerInputs& a,
                              const RecorderBannerInputs& b) {
    return a.phase == b.phase &&
           a.countIn == b.countIn &&
           a.steps == b.steps &&
           a.comboEnds == b.comboEnds &&
           a.displayedSecond == b.displayedSecond &&
           a.interrupted == b.interrupted &&
           a.binding == b.binding;
}

void InvalidateRecorderBannerInputs() {
    std::lock_guard<std::mutex> lock(g_recBannerMx);
    g_recBannerInputsValid = false;
}

void RefreshRecorderBanner() {
    RecorderBannerInputs inputs;
    inputs.phase = g_recPhase.load(std::memory_order_acquire);
    inputs.countIn = g_recCountInView.load(std::memory_order_acquire);
    inputs.steps = g_recStepCountView.load(std::memory_order_acquire);
    inputs.comboEnds = g_recComboEndCountView.load(std::memory_order_acquire);
    inputs.displayedSecond = inputs.phase == RecorderPhase::Recording
        ? (std::max)(0, g_recFrame / 192)
        : -1;
    inputs.interrupted = inputs.phase == RecorderPhase::Idle || Demo::IsActive() ||
                         GetCurrentGamePhase() != GamePhase::Match;
    if (inputs.phase != RecorderPhase::Idle) {
        inputs.binding = MacroRecordBindingLabel();
    }

    std::lock_guard<std::mutex> lock(g_recBannerMx);
    if (g_recBannerInputsValid &&
        SameRecorderBannerInputs(inputs, g_recBannerInputs)) {
        return;
    }
    g_recBannerInputs = inputs;
    g_recBannerInputsValid = true;

    if (inputs.interrupted) {
        const int id = g_recBannerId.exchange(-1, std::memory_order_acq_rel);
        if (id != -1) DirectDrawHook::RemovePermanentMessage(id);
        return;
    }

    char text[256];
    COLORREF color = RGB(255, 230, 120);
    switch (inputs.phase) {
        case RecorderPhase::PreRecord:
            if (Config::GetSettings().missionRecorderCountInMs > 0) {
                _snprintf_s(text, sizeof(text), _TRUNCATE,
                            "MISSION PRE-RECORD | arrange the start | %s = record after %.1fs",
                            inputs.binding.c_str(),
                            static_cast<double>(
                                Config::GetSettings().missionRecorderCountInMs) /
                                1000.0);
            } else {
                _snprintf_s(text, sizeof(text), _TRUNCATE,
                            "MISSION PRE-RECORD | arrange the start | %s = record on release",
                            inputs.binding.c_str());
            }
            break;
        case RecorderPhase::CountIn: {
            if (inputs.countIn > 0) {
                _snprintf_s(text, sizeof(text), _TRUNCATE,
                            "MISSION COUNT-IN %.1fs | release controls | %s = cancel",
                            static_cast<double>(inputs.countIn) / 1000.0,
                            inputs.binding.c_str());
            } else {
                _snprintf_s(text, sizeof(text), _TRUNCATE,
                            "MISSION READY | locking exact start | %s = cancel",
                            inputs.binding.c_str());
            }
            break;
        }
        case RecorderPhase::Recording: {
            color = RGB(255, 100, 100);
            _snprintf_s(text, sizeof(text), _TRUNCATE,
                        "MISSION RECORDING %02d:%02d | %d action%s, %d setup break%s | %s = stop & review",
                        inputs.displayedSecond / 60, inputs.displayedSecond % 60,
                        inputs.steps, inputs.steps == 1 ? "" : "s",
                        inputs.comboEnds, inputs.comboEnds == 1 ? "" : "s",
                        inputs.binding.c_str());
            break;
        }
        case RecorderPhase::Review:
            color = RGB(180, 230, 255);
            _snprintf_s(text, sizeof(text), _TRUNCATE,
                        "MISSION REVIEW | %d action%s, %d setup break%s | open menu to preview, save, retake, or discard",
                        inputs.steps, inputs.steps == 1 ? "" : "s",
                        inputs.comboEnds, inputs.comboEnds == 1 ? "" : "s");
            break;
        case RecorderPhase::Idle:
        default:
            return;
    }

    int id = g_recBannerId.load(std::memory_order_acquire);
    if (id == -1) {
        id = DirectDrawHook::AddPermanentMessage(text, color, 20, 60);
        g_recBannerId.store(id, std::memory_order_release);
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
int PlayerChar(int playerIndex) {
    return playerIndex == 2 ? displayData.p2CharID : displayData.p1CharID;
}

// Live P1 character-state read for step stamping/validation. displayData's
// copies can be stale while the menu is closed and no locks force character
// reads, so read the byte directly. Akiko: bullet cycle 0..2 - which 236 item
// comes out next. It does NOT drive rekka clean hits (those are positional,
// see ClassifyAkikoCleanHit); it is recorded so item-based routes reproduce.
// -1 = no trackable state.
int ReadP1CharStateLive() {
    if (P1Char() != CHAR_ID_AKIKO) return -1;
    const uintptr_t base = GetEFZBase();
    if (!base) return -1;
    const uintptr_t addr = ResolvePointer(base, EFZ_BASE_OFFSET_P1,
                                          AKIKO_BULLET_CYCLE_OFFSET);
    int v = -1;
    if (!addr || !SafeReadMemory(addr, &v, sizeof(v))) return -1;
    return (std::min)(2, (std::max)(0, v));
}

// Akiko clean hits are POSITIONAL and roll ONLY on the LAST hit of 623
// (moves 259 A/B, 254 C): the Y-delta attacker-minus-defender at the hit
// decides the variant. Thresholds mirror the live SHOW CLEAN HIT helper in
// frame_monitor.cpp: C full (47,53), C partial (40,60); A/B clean (32,48).
// Returns a notation suffix ("" = no clean hit / not a finisher).
bool IsAkikoRekkaFinisher(short move) {
    return move == AKIKO_MOVE_623_LAST_C || move == AKIKO_MOVE_623_LAST_AB;
}
const char* ClassifyAkikoCleanHit(short move) {
    if (P1Char() != CHAR_ID_AKIKO || !IsAkikoRekkaFinisher(move)) return "";
    const uintptr_t p1 = GetPlayerBase(1);
    const uintptr_t p2 = GetPlayerBase(2);
    if (!p1 || !p2) return "";
    double y1 = 0.0, y2 = 0.0;
    if (!SafeReadMemory(p1 + YPOS_OFFSET, &y1, sizeof(y1)) ||
        !SafeReadMemory(p2 + YPOS_OFFSET, &y2, sizeof(y2))) return "";
    const double diff = y1 - y2;
    if (move == AKIKO_MOVE_623_LAST_C) {
        if (diff > 47.0 && diff < 53.0) return " (clean hit)";
        if (diff > 40.0 && diff < 60.0) return " (partial clean)";
        return "";
    }
    return (diff > 32.0 && diff < 48.0) ? " (clean hit)" : "";
}

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
        // A combo boundary (comboEndAfter) must never attach to an OPTIONAL step -
        // BuildDraft validation rejects that ("comboEndAfter cannot be attached to
        // optional step"). Skip any optional authoring decoration so the boundary
        // lands on the previous required hit. Raw entity observations never enter
        // this list; they live only in the authoring sidecar.
        if (g_recSteps[i].landed && !g_recSteps[i].optional) return i;
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

const char* RecEntityTraceKindName(RecEntityTraceKind kind) {
    switch (kind) {
        case RecEntityTraceKind::Baseline: return "baseline";
        case RecEntityTraceKind::Spawn:    return "spawn";
        case RecEntityTraceKind::Morph:    return "morph";
        case RecEntityTraceKind::Despawn:  return "despawn";
        case RecEntityTraceKind::Contact:  return "contact";
        default:                           return "unknown";
    }
}

int RecorderAfterStep() {
    return g_recSteps.empty() ? -1 : static_cast<int>(g_recSteps.size()) - 1;
}

void FillRecEntityMetadata(RecEntityTraceEvent& event, int metadataPattern) {
    if (metadataPattern < 0) return;
    event.entityClass = ::Mission::MoveData::GetClass(
        event.characterId, metadataPattern);
    event.attack = ::Mission::MoveData::IsAttack(
        event.characterId, metadataPattern);
}

void AppendRecEntityTrace(const RecEntityTraceEvent& event) {
    if (g_recEntityTraceCount < g_recEntityTrace.size()) {
        g_recEntityTrace[g_recEntityTraceCount++] = event;
        return;
    }
    if (g_recEntityTraceDropped++ == 0) {
        LogOut("[MISSION][REC][ENTITY] authoring trace capacity reached; "
               "later raw events will be counted but not stored", true);
    }
}

// Sample both complete player rings once. These are endpoint observations, so a slot
// transition says only what changed between samples. It does not establish
// producer lineage, cross-slot order, or contact and can never alter format-1
// steps. Unknown/cosmetic patterns are retained verbatim for every character.
void RecorderCaptureEntityRing() {
    for (int playerIndex = 1; playerIndex <= 2; ++playerIndex) {
        const std::size_t playerSlot = static_cast<std::size_t>(playerIndex - 1);
        std::array<CollisionDisplay::ProjectileRingSlotProbe,
                   CollisionDisplay::kProjectileRingSlotCapacity> current{};
        if (!CollisionDisplay::ProbeProjectileRing(
                playerIndex, current.data(), current.size())) {
            if (!g_recEntityProbeUnavailableLogged[playerSlot]) {
                g_recEntityProbeUnavailableLogged[playerSlot] = true;
                LogOut("[MISSION][REC][ENTITY] P" + std::to_string(playerIndex) +
                       " ring sample unavailable; prior slot state retained "
                       "(no despawn inferred)", true);
            }
            continue;
        }
        if (g_recEntityProbeUnavailableLogged[playerSlot]) {
            g_recEntityProbeUnavailableLogged[playerSlot] = false;
            LogOut("[MISSION][REC][ENTITY] P" + std::to_string(playerIndex) +
                   " ring sampling recovered", true);
        }

        const int charId = PlayerChar(playerIndex);
        (void)::Mission::MoveData::EnsureLoaded(charId);
        for (std::size_t slot = 0; slot < current.size(); ++slot) {
            const auto& now = current[slot];
            RecEntitySlotState& prior = g_recEntitySlots[playerSlot][slot];
            if (!now.readable) continue;

            const auto transition =
                ::Mission::RecorderEntityTrace::ClassifySampledSlotTransition(
                    prior.established, prior.alive, prior.pattern,
                    now.alive, now.pattern);
            if (transition != ::Mission::RecorderEntityTrace::
                                  SampledSlotTransition::None) {
                RecEntityTraceEvent event;
                event.effectiveFrame = g_recFrame;
                event.player = playerIndex;
                event.characterId = charId;
                event.afterStep = RecorderAfterStep();
                event.slot = static_cast<int>(slot);
                event.sampled = true;

                int metadataPattern = -1;
                switch (transition) {
                case ::Mission::RecorderEntityTrace::
                         SampledSlotTransition::Baseline:
                    event.kind = RecEntityTraceKind::Baseline;
                    event.pattern = now.pattern;
                    metadataPattern = now.pattern;
                    break;
                case ::Mission::RecorderEntityTrace::
                         SampledSlotTransition::Spawn:
                    event.kind = RecEntityTraceKind::Spawn;
                    event.pattern = now.pattern;
                    metadataPattern = now.pattern;
                    break;
                case ::Mission::RecorderEntityTrace::
                         SampledSlotTransition::Morph:
                    event.kind = RecEntityTraceKind::Morph;
                    event.pattern = now.pattern;
                    event.priorPattern = prior.pattern;
                    metadataPattern = now.pattern;
                    break;
                case ::Mission::RecorderEntityTrace::
                         SampledSlotTransition::Despawn:
                    event.kind = RecEntityTraceKind::Despawn;
                    event.pattern = -1;
                    event.priorPattern = prior.pattern;
                    // No current entity entry exists. Preserve the final live
                    // sample explicitly; the JSON labels this basis.
                    event.entityFrame = prior.frame;
                    event.life = prior.life;
                    event.destroyed = prior.destroyed;
                    metadataPattern = prior.pattern;
                    break;
                    default:
                        break;
                }
                if (now.alive && event.kind != RecEntityTraceKind::Despawn) {
                    event.entityFrame = now.frame;
                    event.life = now.life;
                    event.destroyed = now.destroyed;
                }
                FillRecEntityMetadata(event, metadataPattern);
                AppendRecEntityTrace(event);

                LogOut("[MISSION][REC][ENTITY] sampled P" +
                       std::to_string(playerIndex) + " " +
                       std::string(RecEntityTraceKindName(event.kind)) +
                       " slot=" + std::to_string(event.slot) +
                       " pattern=" + std::to_string(event.pattern) +
                       " prior=" + std::to_string(event.priorPattern) +
                       " afterStep=" + std::to_string(event.afterStep), true);
            }

            prior.established = true;
            prior.alive = now.alive;
            if (now.alive) {
                prior.pattern = now.pattern;
                prior.frame = now.frame;
                prior.life = now.life;
                prior.destroyed = now.destroyed;
            } else {
                prior.pattern = 0;
                prior.frame = 0;
                prior.life = 0;
                prior.destroyed = 0;
            }
        }
    }
}

// Preserve hook-backed entity->opponent contacts from either owner beside the
// unordered two-player ring observations.
// observations. The collision journal supplies an exact resolver transaction,
// but this development sidecar still does not compile it into a recipe node or
// assert producer lineage.
void RecorderCaptureEntityContacts(const Snapshot& s) {
    g_recEntityContactHookObserved =
        g_recEntityContactHookObserved || s.entityContactHookReady;
    g_recContactJournalOverflowObserved =
        g_recContactJournalOverflowObserved || s.contactEventOverflow;

    for (std::size_t i = 0; i < s.contactEventCount; ++i) {
        const auto& contact = s.contactEvents[i];
        if (contact.source != ::Mission::Contact::Source::Entity ||
            (contact.attacker != 1 && contact.attacker != 2)) {
            continue;
        }
        // The event itself proves that the hook produced a committed journal
        // record even if the readiness flag changed before this monitor read.
        g_recEntityContactHookObserved = true;

        RecEntityTraceEvent event;
        event.kind = RecEntityTraceKind::Contact;
        event.effectiveFrame = g_recFrame;
        event.player = contact.attacker;
        const int charId = PlayerChar(contact.attacker);
        (void)::Mission::MoveData::EnsureLoaded(charId);
        event.characterId = charId;
        event.afterStep = RecorderAfterStep();
        event.slot = contact.entitySlot;
        event.pattern = contact.entityPattern;
        event.entityFrame = contact.attackerFrame;
        event.life = contact.timerAfter;
        event.destroyed = static_cast<uint32_t>(contact.rawStateAfter);
        event.sampled = false;
        event.contactSequence = contact.sequence;
        event.contactBatch = contact.batchId;
        event.defender = contact.defender;
        event.contactResult = contact.result;
        event.lifeBefore = contact.timerBefore;
        event.destroyedBefore = contact.rawStateBefore;
        event.comboBefore = contact.comboBefore;
        event.comboAfter = contact.comboAfter;
        event.hpBefore = contact.defenderHpBefore;
        event.hpAfter = contact.defenderHpAfter;
        FillRecEntityMetadata(event, event.pattern);
        AppendRecEntityTrace(event);
    }
}

void RecorderCapture(const Snapshot& s, bool advanceLogicalTick) {
    const short cur = s.p1Move;
    // Observe moves and the legacy raw latch on every 192 Hz monitor pass so
    // short-lived move IDs and +0x168 pulses cannot fall between macro samples.
    // Only advance
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
    // Fresh-input evidence used by the same-move re-entry gate below. Attack
    // edges come from EFZ's own input poll; the token contributes only on its
    // 99 -> value EDGE because it can hold the value for the move's duration
    // (the five spurious j.IC steps were all stamped with the lingering 52).
    if (s.p1PolledAttackEdges != 0) g_recAttackEdgeAge = 0;
    else if (advanceTime && g_recAttackEdgeAge < 999) {
        g_recAttackEdgeAge = (std::min)(999, g_recAttackEdgeAge + 3);
    }
    if (s.p1Token != 99 && g_recPrevTokenSample == 99) g_recTokenEdgeAge = 0;
    else if (advanceTime && g_recTokenEdgeAge < 999) {
        g_recTokenEdgeAge = (std::min)(999, g_recTokenEdgeAge + 3);
    }
    g_recPrevTokenSample = s.p1Token;
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
    // Legacy format-1 connect heuristic: associate a raw +0x168 0->nonzero edge
    // with the current capture move. This is retained for file compatibility;
    // scripts/entities can produce false positives and it is not typed contact.
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
                    g_recDamageAtStart = 0;   // new combo counts damage from zero
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
        //
        // The frame-rewind edge alone is still not enough for the recorder.
        // Some move scripts RE-EXECUTE themselves while held in place (air IC
        // float, air dash, a whiffed air normal's falling tail): each restart
        // rewinds the frame index AND re-writes the command token, and mashing
        // a button supplies "fresh input" for restarts that produce nothing.
        // The reliable rule is semantic: a same-move re-entry is only a new
        // authored step if the PREVIOUS instance actually did something
        // (landed or made contact). A contactless instance re-entering itself
        // collapses into the existing step - whiff-mash then hit records as
        // one step that must land, and true chains (5A hit -> 5A) keep both
        // steps. Double projectile casts are unaffected: they pass through
        // recovery/neutral between casts and arrive via the ID-change path.
        constexpr int kReentryInputWindowTicks = 12;   // ~4 visual frames
        const bool freshInput =
            g_recAttackEdgeAge <= kReentryInputWindowTicks ||
            g_recTokenEdgeAge <= kReentryInputWindowTicks;
        const bool priorInstanceDidSomething = !g_recSteps.empty() &&
            g_recSteps.back().moveId == cur &&
            (g_recSteps.back().landed || g_recSteps.back().connected);
        const bool sameMoveReentered = cur == g_recLastAttackId &&
            cur == g_recPrevMove && priorInstanceDidSomething && freshInput &&
            ::Mission::SequencePolicy::IsMoveInstanceEdge(
                cur, s.p1FrameIdx, g_recPrevMove, g_recPrevFrameIdx);
        const bool akikoVacuumHitTransition =
            P1Char() == CHAR_ID_AKIKO && !g_recSteps.empty() &&
            g_recSteps.back().moveId == g_recPrevMove &&
            ::Mission::SequencePolicy::IsAkikoVacuumHitTransition(g_recPrevMove, cur);
        const bool rumiThrowSuccessTransition =
            P1Char() == CHAR_ID_NANASE && !g_recSteps.empty() &&
            g_recSteps.back().moveId == g_recPrevMove &&
            ::Mission::SequencePolicy::IsRumiCommandThrowSuccessTransition(
                g_recPrevMove, cur);
        const bool automaticPhaseTransition =
            akikoVacuumHitTransition || rumiThrowSuccessTransition;
        if (automaticPhaseTransition) {
            // Keep the authored input step armed so the following hit/contact
            // is credited to it. The automatic phase becomes an accepted
            // playback alias instead of a fake second player input.
            g_recSteps.back().automaticFollowupId = cur;
            g_recLastAttackId = cur;
            if (contactEdge) g_recSteps.back().connected = true;
            LogOut(std::string("[MISSION][REC] ") +
                   (akikoVacuumHitTransition ? "Akiko 41236 hit" :
                                               "Rumi 41236 success") +
                   " automatic phase " +
                   std::to_string(g_recPrevMove) + "->" + std::to_string(cur) +
                   " merged into one step", true);
        }
        if (!automaticPhaseTransition &&
            (cur != g_recLastAttackId || sameMoveReentered)) {
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
            // Akiko C-rekka chain (start 257, followups 258, finisher 254):
            // she crosses behind the opponent on reps, so the PHYSICAL input
            // alternates - the full chain is 623C 623C 421C 623C 421C 623C.
            // The generated table's per-move names ("623C rekka2") cannot
            // express that, so track the rep index and name reps by parity.
            if (P1Char() == CHAR_ID_AKIKO) {
                if (cur == 257) {
                    g_recAkikoRekkaRep = 1;
                    cs.notation = "623C";
                } else if ((cur == 258 || cur == 254) && g_recAkikoRekkaRep > 0) {
                    ++g_recAkikoRekkaRep;
                    cs.notation = (g_recAkikoRekkaRep % 2 == 0) ? "623C" : "421C";
                } else {
                    g_recAkikoRekkaRep = 0;
                }
            }
            cs.req = static_cast<int>(::Mission::StepReq::Move); // upgraded to Land once it connects
            // Character-state stamp (Akiko: 236-item bullet cycle) - kept for
            // item-route determinism only. Clean hits are positional and are
            // classified at the finisher's LANDING hit, not here.
            cs.charState = ReadP1CharStateLive();
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
            g_recDamageAtStart = s.p1ComboDamage;
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
                // Akiko rekka finisher: classify the positional clean-hit
                // variant at the LANDING hit (that is when the roll happens)
                // and record it in the notation. The per-step damage check
                // enforces reproduction; this makes the requirement readable.
                if (hits == 1 && st.hits == 0 && IsAkikoRekkaFinisher(st.moveId)) {
                    const char* tag = ClassifyAkikoCleanHit(st.moveId);
                    if (*tag && st.notation.find("clean") == std::string::npos) {
                        st.notation += tag;
                        LogOut(std::string("[MISSION][REC] finisher variant:") + tag, true);
                    }
                }
                st.hits = hits;
                st.damage = (std::max)(0, s.p1ComboDamage - g_recDamageAtStart);
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
    // Ring lifecycle is sampled once per authoritative, unfrozen recorder tick.
    // Hook-backed contact records are drained on every monitor pass so a short
    // resolver event between macro ticks is not lost. Neither path mutates the
    // linear format-1 recipe.
    if (advanceTime) RecorderCaptureEntityRing();
    RecorderCaptureEntityContacts(s);
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
    g_runDamageBaseline = 0;
    g_runGapWindow.Reset();
    g_runComboWasAlive = false;
    g_runPrevComboCount = 0;
    g_runPrevInputs = 0;
    g_runAwaitingComboEnd = false;
    ResetRunScore();
    LogOut("[MISSION][RUN] DROP step=" + std::to_string(failedStep) +
           " attempts=" + std::to_string(g_runAttempts), true);
    // TrialMode-style retry: snap back to the mission's start state - but only
    // after a short grace so the player can SEE what failed (the drop reason
    // message and the red recipe step) instead of being yanked back instantly.
    g_runRetryDelay = 96;   // ~0.5s at the monitor's 192Hz cadence
}

// Deferred auto-retry (armed by RunnerDrop). Never while a hotswap/setup is
// still driving the match, and never under the recorder (a rollback
// mid-capture corrupts the recording; recording also unloads the runner, so
// this is defense in depth). A manual load during the grace clears the phase,
// which cancels the pending fire.
void TickAutoRetry() {
    if (::Mission::TutorialSession::IsActive()) { g_runRetryDelay = 0; return; }
    if (g_runRetryDelay <= 0 || g_runPhase != RunnerPhase::Dropped) {
        if (g_runPhase != RunnerPhase::Dropped) g_runRetryDelay = 0;
        return;
    }
    if (--g_runRetryDelay > 0) return;
    if (g_runStateSaved && SavestateHook::IsInstalled() &&
        !g_recActive.load() &&
        GetCurrentGamePhase() == GamePhase::Match &&
        !CharacterHotswap::IsBusy() && !::Mission::Setup::IsPending()) {
        ScopedRunnerSelfLoad selfLoad;
        const bool ok = SavestateHook::TriggerLoad();
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

// Per-step damage validation at satisfaction time. Hit-variant mechanics
// (Mizuka clean hits, Akiko rekka crits) keep the move ID but change the
// step's damage by thousands, so the rest of the recorded combo can't
// reproduce; ordinary variance stays inside the policy tolerance (500).
// Returns false after dropping the run with the real reason.
bool StepDamageAcceptable(const ::Mission::Step& st, const Snapshot& s, int stepIdx) {
    const int observed = s.p1ComboDamage - g_runDamageBaseline;
    if (!::Mission::SequencePolicy::StepDamageDiverged(st.damage, observed)) {
        return true;
    }
    // Akiko's 623 finisher variant is positional (Y-delta at the last hit) -
    // name the real cause instead of a generic strength complaint.
    const bool cleanHitStep = !st.moveIds.empty() &&
        IsAkikoRekkaFinisher(static_cast<short>(st.moveIds.front())) &&
        P1Char() == CHAR_ID_AKIKO;
    char msg[128];
    if (cleanHitStep) {
        _snprintf_s(msg, sizeof(msg), _TRUNCATE,
                    "Clean-hit spacing was off: +%d damage (recorded +%d)",
                    observed, st.damage);
    } else {
        _snprintf_s(msg, sizeof(msg), _TRUNCATE,
                    "Wrong hit strength: +%d damage (recorded +%d)", observed, st.damage);
    }
    DirectDrawHook::AddMessage(msg, "mission_run", RGB(255, 160, 160), 1800, 20, 96);
    LogOut(std::string("[MISSION][RUN] ") + msg + " at step " +
           std::to_string(stepIdx), true);
    RunnerDrop();
    return false;
}

// Advance bookkeeping when the current step is satisfied.
void AdvanceStep(const Snapshot& s, int total) {
    ObserveRunScore(s);
    const bool requireComboEnd = g_runStep >= 0 && g_runStep < total &&
        g_runMission.steps[g_runStep].comboEndAfter;
    g_runStep++;
    g_runBaseline = s.p1Combo;
    g_runDamageBaseline = s.p1ComboDamage;
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
        g_runDamageBaseline = s.p1ComboDamage;
    }
}

void TickRun(const Snapshot& s) {
    const int total = static_cast<int>(g_runMission.steps.size());
    if (total == 0) { g_runPrevMove = s.p1Move; g_runPrevFrameIdx = s.p1FrameIdx; return; }

    TickAutoRetry();   // deferred post-drop baseline load (grace elapsed)

    // Legacy format-1 Connect heuristic: a raw +0x168 0->nonzero edge while
    // armed satisfies StepReq::Connect. Do not reuse this for typed predicates.
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
            g_runDamageBaseline = (s.p1Combo > 0) ? s.p1ComboDamage : 0;
            g_runPhase = RunnerPhase::Idle;   // clear a shown DROPPED once they retry
        }
        if (g_runArmed) {
            if (!s.freezeActive) ++g_runArmedFrames;
            if (st.maxDelay > 0 && g_runArmedFrames > st.maxDelay) {
                g_runArmed = false;           // window expired; wait for a fresh attempt
            } else if (ArmedStepSatisfied(st, s)) {
                if (StepDamageAcceptable(st, s, 0)) {
                    // This is the retry boundary: the opener actually connected/met
                    // its requirement, so the old failure marker can finally clear.
                    g_runFailedStep.store(-1);
                    g_runPhase = RunnerPhase::InProgress;
                    g_runStep = 0;
                    AdvanceStep(s, total);
                } else {
                    g_runArmed = false;
                }
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
                g_runDamageBaseline = 0;
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
                    // Character-state requirement (Akiko: 236-item bullet
                    // cycle). A diverged cycle means a different item comes
                    // out and an item-based route stops reproducing, so fail
                    // HERE with the real reason instead of a later timeout.
                    // Clean hits are NOT this - they are positional and are
                    // enforced by the per-step damage check at satisfaction.
                    // Unreadable live state (different character, resolve
                    // failure) skips the check.
                    const int liveState = st.charState >= 0 ? ReadP1CharStateLive() : -1;
                    if (st.charState >= 0 && liveState >= 0 && liveState != st.charState) {
                        char msg[96];
                        _snprintf_s(msg, sizeof(msg), _TRUNCATE,
                                    "Wrong item cycle: %d, recorded %d",
                                    liveState, st.charState);
                        DirectDrawHook::AddMessage(msg, "mission_run",
                                                   RGB(255, 160, 160), 1800, 20, 96);
                        LogOut(std::string("[MISSION][RUN] ") + msg +
                               " at step " + std::to_string(g_runStep), true);
                        RunnerDrop();
                        g_runArmed = false;
                    } else {
                        actionStartedThisTick = true;
                        g_runArmed = true;
                        g_runArmedFrames = 0;
                        g_runConnectSeen = false;
                        g_runGapWindow.Reset();   // performing the move is progress
                        // Residual hits from the previous move must not count toward
                        // this step; a consumed combo.end already rebased this to zero.
                        g_runBaseline = ::Mission::SequencePolicy::RunnerArmBaseline(
                            s.p1Combo, consumedRequiredBoundary);
                        g_runDamageBaseline =
                            consumedRequiredBoundary ? 0 : s.p1ComboDamage;
                    }
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
                            if (StepDamageAcceptable(st, s, g_runStep)) {
                                AdvanceStep(s, total);
                            } else {
                                g_runArmed = false;
                            }
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
    g_recDamageAtStart = 0;
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
    g_recAttackEdgeAge = 999;
    g_recTokenEdgeAge = 999;
    g_recPrevTokenSample = 99;
    g_recAkikoRekkaRep = 0;
    for (auto& playerSlots : g_recEntitySlots) {
        playerSlots.fill(RecEntitySlotState{});
    }
    g_recEntityTrace.fill(RecEntityTraceEvent{});
    g_recEntityTraceCount = 0;
    g_recEntityTraceDropped = 0;
    g_recEntityContactHookObserved = false;
    g_recContactJournalOverflowObserved = false;
    g_recEntityProbeUnavailableLogged.fill(false);
    g_recCountInView.store(0, std::memory_order_release);
    InvalidateRecorderBannerInputs();
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
    g_recCountInTargetTicks = 0;
    g_recCountInElapsed = false;
    g_recCountInView.store(0, std::memory_order_release);
    if (!keepSetup) {
        ResetRecorderCaptureState();
        g_recSetup = ::Mission::Mission();
    }
    g_recPhase.store(RecorderPhase::PreRecord, std::memory_order_release);
    LogOut("[MISSION][REC] entered pre-record", true);
}

bool CaptureRecorderBaseline(std::string& outError) {
    outError.clear();
    ResetRecorderCaptureState();
    g_recSetup = ::Mission::Mission();
    ::Mission::Setup::Capture(g_recSetup);
    if (GetCurrentGamePhase() != GamePhase::Match) {
        outError = "recording baseline requires an active match";
        return false;
    }
    if (!::Mission::StateDump::Available()) {
        outError = "exact savestate capture is unavailable";
        return false;
    }
    if (!::Mission::StateDump::Capture(g_recSetup.savestate, outError)) {
        g_recSetup.savestate.clear();
        return false;
    }
    LogOut("[MISSION][REC] frame-zero start-state dump attached (" +
           std::to_string(g_recSetup.savestate.size()) + " chars b64)", true);
    return true;
}

bool RestoreRecorderBaseline() {
    if (g_recSetup.savestate.empty() || !::Mission::StateDump::Available()) {
        LogOut("[MISSION][REC] exact baseline restore unavailable", true);
        return false;
    }
    std::string err;
    if (!::Mission::StateDump::Restore(g_recSetup.savestate, err)) {
        LogOut("[MISSION][REC] exact baseline restore failed: " + err, true);
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
        g_recCountInTargetTicks = 0;
        g_recCountInElapsed = false;
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
        // The author requested recording. The optional count-in is only a short
        // release/ready window; the exact baseline is captured on the clear
        // tick immediately after it ends, directly before synchronized input
        // recording begins.
        CancelAutoActionsAndMacros();
        ResetRecorderCaptureState();
        g_recSetup = ::Mission::Mission();
        g_recReleaseFrames = 0;
        g_recCountInFrames = 0;
        g_recCountInTargetTicks =
            ::Mission::SequencePolicy::RecorderCountInTicksFromMs(
                Config::GetSettings().missionRecorderCountInMs);
        g_recCountInElapsed = false;
        g_recCountInView.store(
            (std::max)(0, Config::GetSettings().missionRecorderCountInMs),
            std::memory_order_release);
        g_recPhase.store(RecorderPhase::CountIn, std::memory_order_release);
        LogOut("[MISSION][REC] waiting for neutral count-in ms=" +
               std::to_string(Config::GetSettings().missionRecorderCountInMs), true);
    } else if (advance == ::Mission::Engine::Recorder::AdvanceEffect::CancelCountIn) {
        // The same physical control is a safe toggle during the countdown. It
        // never starts a partial take and never discards Review data.
        g_recReleaseFrames = 0;
        g_recCountInFrames = 0;
        g_recCountInTargetTicks = 0;
        g_recCountInElapsed = false;
        g_recCountInView.store(0, std::memory_order_release);
        ResetRecorderCaptureState();
        EnterRecorderPreRecord(false);
        ShowRecorderState("Countdown canceled; arrange the start and record again",
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
    // Every reset path names its blocker after ~1s of continuous stall. A
    // gamespeed false positive (heuristic/legacy addresses reading 0 while the
    // game ran) once held this gate forever with no output; a silent count-in
    // reset must not be able to happen again.
    static int s_countInBlockedTicks = 0;
    const char* blocker = nullptr;
    if (!s.valid)                                       blocker = "match state unreadable";
    else if (GetCurrentGamePhase() != GamePhase::Match) blocker = "not in a match";
    else if (ImGuiImpl::IsVisible() ||
             ::Mission::PauseMenu::IsOpen())            blocker = "menu is open";
    else if (::Mission::Setup::IsPending())             blocker = "mission setup pending";
    else if (CharacterHotswap::IsBusy())                blocker = "character load in progress";
    else if (PauseIntegration::IsPracticePaused())      blocker = "practice is paused";
    else if (PauseIntegration::IsGameSpeedFrozen())     blocker = "game speed is frozen";
    else if (GetPlayerInputs(1) != 0)                   blocker = "release all P1 controls";
    if (blocker) {
        g_recReleaseFrames = 0;
        g_recCountInFrames = 0;
        g_recCountInElapsed = false;
        g_recCountInView.store(
            ::Mission::SequencePolicy::RecorderCountInRemainingMs(
                g_recCountInTargetTicks),
            std::memory_order_release);
        if (++s_countInBlockedTicks % 192 == 0) {
            ShowRecorderState((std::string("COUNT-IN waiting: ") + blocker).c_str(),
                              RGB(255, 190, 120), 1100);
            LogOut(std::string("[MISSION][REC] count-in blocked: ") + blocker, true);
        }
        return;
    }
    s_countInBlockedTicks = 0;
    if (g_recReleaseFrames < 6) {
        ++g_recReleaseFrames;
        return;
    }
    if (!g_recCountInElapsed) {
        if (!s.freezeActive && g_recCountInFrames < g_recCountInTargetTicks) {
            ++g_recCountInFrames;
        }
        const int remainingTicks =
            (std::max)(0, g_recCountInTargetTicks - g_recCountInFrames);
        g_recCountInView.store(
            ::Mission::SequencePolicy::RecorderCountInRemainingMs(remainingTicks),
            std::memory_order_release);
        if (g_recCountInFrames < g_recCountInTargetTicks) return;

        // Deliberately wait one complete mission tick after the countdown
        // reaches zero. The next clear tick is the exact frame-zero save/start
        // boundary requested by the author.
        g_recCountInElapsed = true;
        return;
    }

    // Frame-zero boundary: clear stale action/queue owners, capture the exact
    // current world (including both entity rings), seed those rings as baseline
    // observations, then start the P1 input stream without another live tick.
    CancelAutoActionsAndMacros();
    std::string baselineError;
    if (!CaptureRecorderBaseline(baselineError)) {
        ResetRecorderCaptureState();
        EnterRecorderPreRecord(false);
        ShowRecorderState(("Recording did not start: " + baselineError).c_str(),
                          RGB(255, 120, 120), 2400);
        LogOut("[MISSION][REC] frame-zero capture failed: " + baselineError, true);
        return;
    }
    ResetRecorderCaptureState();
    RecorderCaptureEntityRing();
    if (!MacroController::BeginPlayerRecording(1, false) ||
        !MacroController::StartPlayerRecording()) {
        MacroController::Stop();
        EnterRecorderPreRecord(false);
        ShowRecorderState("Mission input capture failed; press Macro Record to retry",
                          RGB(255, 120, 120), 1800);
        return;
    }
    // `s` was sampled at the beginning of this mission tick, before queue
    // cancellation and the exact frame-zero save. Seed from live memory so
    // those boundary operations cannot look like the first recorded transition.
    const uintptr_t frameZeroP1 = GetPlayerBase(1);
    const uintptr_t frameZeroP2 = GetPlayerBase(2);
    const int frameZeroCombo = frameZeroP1
        ? (std::max)(0, static_cast<int>(
              ReadShort(frameZeroP1, PLAYER_COMBO_COUNTER_OFFSET)))
        : 0;
    const short frameZeroP2Move = ReadMove(frameZeroP2);
    g_recPrevCombo = frameZeroCombo;
    g_recComboWasAlive = frameZeroCombo > 0 || IsHitstun(frameZeroP2Move) ||
                         IsLaunched(frameZeroP2Move) || IsBlockstun(frameZeroP2Move);
    g_recPrevHitState = ReadInt(frameZeroP1, 0x168);
    g_recPrevMove = ReadMove(frameZeroP1);
    g_recPrevFrameIdx = ReadShort(frameZeroP1, 0xA);
    g_recCountInView.store(0, std::memory_order_release);
    g_recCountInTargetTicks = 0;
    g_recCountInElapsed = false;
    g_recActive.store(true, std::memory_order_release);
    g_recPhase.store(RecorderPhase::Recording, std::memory_order_release);
    LogOut("[MISSION][REC] synchronized P1 step+input capture started", true);
}

void ResetRunForDemo() {
    g_runPhase = RunnerPhase::Idle;
    g_runStep = 0;
    g_runFailedStep.store(-1, std::memory_order_release);
    g_runBaseline = 0;
    g_runDamageBaseline = 0;
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

// Clear only the terminal tutorial handoff. While a new demonstration is
// replacing it, releaseNeutral is false so ownership moves without exposing a
// physical-input poll. Unload/session-reset paths pass true because no owner
// follows.
void ClearDemoTutorialHandoff(bool releaseNeutral) {
    if (g_demoRestoreResult != Demo::RestoreResult::None && releaseNeutral) {
        HoldDemoP1Neutral(false);
    }
    g_demoRestoreResult = Demo::RestoreResult::None;
    g_demoRestoreMessage.clear();
    g_demoTutorialHandoff = false;
}

void PublishDemoTutorialRestore(Demo::RestoreResult result,
                                const std::string& message) {
    g_demoRestoreResult = result;
    g_demoRestoreMessage = message;
    // Keep the direct demo owner authoritative until TutorialSession freezes
    // or acquires its own release-to-neutral lease on a fresh-snapshot tick.
    HoldDemoP1Neutral(true);
}

void SetRunnerStartupInputHold(bool hold) {
    if (hold) {
        if (!g_runStartupInputHeld.exchange(true, std::memory_order_acq_rel)) {
            g_runStartupPollSnapshotValid = true;
            g_runStartupPollSavedActive =
                g_pollOverrideActive[1].load(std::memory_order_acquire);
            g_runStartupPollSavedMask =
                g_pollOverrideMask[1].load(std::memory_order_acquire);
        }
        g_pollOverrideMask[1].store(0, std::memory_order_relaxed);
        g_pollOverrideActive[1].store(true, std::memory_order_release);
        return;
    }
    if (!g_runStartupInputHeld.exchange(false, std::memory_order_acq_rel)) return;
    if (g_runStartupPollSnapshotValid && !Demo::IsActive() &&
        !MacroController::IsExclusivePlayback()) {
        // Restore only while the zero hold is still recognizably ours. A newer
        // playback owner wins and is never collapsed to the saved state.
        if (g_pollOverrideMask[1].load(std::memory_order_acquire) == 0) {
            g_pollOverrideMask[1].store(g_runStartupPollSavedMask,
                                        std::memory_order_relaxed);
        }
        if (g_pollOverrideActive[1].load(std::memory_order_acquire)) {
            g_pollOverrideActive[1].store(g_runStartupPollSavedActive,
                                          std::memory_order_release);
        }
    }
    g_runStartupPollSnapshotValid = false;
}

// Return the first condition that prevents a value-level mission baseline from
// settling on this sample.  This is intentionally shared by the eligibility
// gate and the eventual deadline report: a tutorial must not sit forever on a
// hidden precondition, and the player should be told which precondition failed.
std::string RunnerBaselineSaveBlocker(const Snapshot& s) {
    if (!g_runActive.load(std::memory_order_acquire)) {
        return "the mission session stopped";
    }
    if (g_runRestorePending) {
        return "the exact start-state restore did not finish";
    }
    if (!s.valid) {
        return "the fighters were not ready";
    }
    if (GetCurrentGamePhase() != GamePhase::Match) {
        return "the Practice match did not become active";
    }
    if (::Mission::PauseMenu::IsOpen()) {
        return "the lesson menu remained open";
    }
    if (ImGuiImpl::IsVisible()) {
        return "another menu remained open";
    }
    if (PauseIntegration::IsPracticePaused()) {
        return "Practice remained paused";
    }
    if (PauseIntegration::IsGameSpeedFrozen()) {
        return "the match remained frozen";
    }
    if (::Mission::Setup::IsPending()) {
        return "the lesson setup did not finish";
    }
    if (CharacterHotswap::IsBusy()) {
        return "the fighter load did not finish";
    }
    if (g_recActive.load(std::memory_order_acquire)) {
        return "mission recording remained active";
    }
    if (RoundIntroActive()) {
        return "the round intro did not finish";
    }
    if (!SavestateHook::IsInstalled()) {
        return "the checkpoint system was not available";
    }
    return std::string();
}

void FailRunnerBaselineSave(const std::string& blocker) {
    const std::string reason = blocker.empty()
        ? std::string("the checkpoint system did not become ready")
        : blocker;
    g_runSavePending = false;
    g_runSaveSettle = 0;
    g_runSaveRetries = 0;
    g_runSaveElapsed = 0;
    g_runSaveBlocker.clear();
    g_runStateSaved = false;

    const bool lesson = g_runMission.tutorialSchema > 0 && g_runMission.hasLesson;
    const std::string message = lesson
        ? "The lesson checkpoint could not be created because " + reason +
              ". Return to Lessons and launch it again."
        : "The mission checkpoint could not be created because " + reason +
              ". Retry will be unavailable for this run.";
    LogOut("[MISSION][RUN] baseline startup failed: " + reason, true);
    if (lesson) {
        ::Mission::TutorialSession::NotifyStartupFailure(message);
    } else {
        DirectDrawHook::AddMessage(message.c_str(), "MISSION",
                                   RGB(255, 120, 120), 4200, 0, 120);
    }
}

bool RestoreDemoBaseline(std::string& errorOut) {
    const ::Mission::Mission& baseline = g_demoFromRecorder ? g_recSetup : g_runMission;
    bool ok = false;
    ScopedRunnerSelfLoad selfLoad;
    // A loaded mission already owns a verified same-session Revival baseline.
    // Prefer it: it is the exact state the runner retries from and avoids
    // re-running cross-session header/layout checks against an attempt that has
    // naturally changed since startup. Unsaved recorder previews use their
    // embedded frame-zero dump because they do not own a runner checkpoint.
    const auto source = ::Mission::SequencePolicy::DecideDemoBaselineSource(
        g_demoFromRecorder,
        g_runStateSaved && SavestateHook::IsInstalled(),
        !baseline.savestate.empty() && ::Mission::StateDump::Available());
    if (source == ::Mission::SequencePolicy::DemoBaselineSource::RunnerCheckpoint) {
        ok = SavestateHook::TriggerLoad();
        if (!ok) errorOut = "mission baseline load was unavailable";
    } else if (source == ::Mission::SequencePolicy::DemoBaselineSource::EmbeddedDump) {
        ok = ::Mission::StateDump::Restore(baseline.savestate, errorOut);
    } else {
        // A demonstration is a baseline-bracketed transaction.  Value-level
        // Setup::Apply is not a restore and, worse, asks whether the *current*
        // live session still matches the mission before replay starts.  A
        // perfectly ordinary played attempt has diverged by definition, so
        // that fallback could reject WATCH DEMONSTRATION before the state was
        // ever rewound.  Only an exact embedded dump or the runner-owned
        // Revival checkpoint is a valid demonstration source.
        errorOut = g_demoFromRecorder
            ? "recording baseline is unavailable; retake the recording"
            : "mission baseline is unavailable; restart the mission";
    }
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
        const std::string error =
            "the demonstration left the active match before the lesson baseline was restored";
        if (g_demoTutorialHandoff) {
            PublishDemoTutorialRestore(Demo::RestoreResult::RestoreFailed, error);
        } else {
            HoldDemoP1Neutral(false);
            ClearDemoTutorialHandoff(false);
        }
        g_demoText.clear();
        g_demoFromRecorder = false;
        g_demoBaselineIssued = false;
        g_demoPhase.store(DemoPhase::Idle, std::memory_order_release);
        // This Snapshot predates the terminal transition. It is never the
        // sample from which runner/tutorial validation resumes.
        g_demoFinishedThisTick = true;
        LogOut("[MISSION][DEMO] restore handoff failed: " + error, true);
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
            const std::string error = err.empty()
                ? std::string("the lesson baseline restore failed") : err;
            if (g_demoTutorialHandoff) {
                PublishDemoTutorialRestore(
                    Demo::RestoreResult::RestoreFailed, error);
            } else {
                HoldDemoP1Neutral(false);
                ClearDemoTutorialHandoff(false);
            }
            g_demoText.clear();
            g_demoFromRecorder = false;
            g_demoBaselineIssued = false;
            g_demoPhase.store(DemoPhase::Idle, std::memory_order_release);
            g_demoFinishedThisTick = true;
            if (!g_demoTutorialHandoff) {
                DirectDrawHook::AddMessage(
                    ("Demo restore failed: " + error).c_str(), "MISSION",
                    RGB(255, 120, 120), 2200, 0, 120);
            }
            LogOut("[MISSION][DEMO] baseline restore failed: " + error, true);
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
        const bool recorderPreview = g_demoFromRecorder;
        MacroController::ReleaseExclusivePlaybackHold();
        if (g_demoTutorialHandoff) {
            PublishDemoTutorialRestore(
                Demo::RestoreResult::Restored, "lesson baseline restored");
        } else {
            HoldDemoP1Neutral(false);
            ClearDemoTutorialHandoff(false);
        }
        g_demoText.clear();
        g_demoBaselineIssued = false;
        g_demoPhase.store(DemoPhase::Idle, std::memory_order_release);
        // This is exclusively a stale-Snapshot guard. Tutorial ownership is
        // carried by g_demoRestoreResult until explicit acknowledgement.
        g_demoFinishedThisTick = true;
        DirectDrawHook::AddMessage(recorderPreview
                                       ? "Preview finished - back to Review"
                                       : "Demonstration finished - your turn", "MISSION",
                                   RGB(180, 255, 220), 1800, 0, 120);
        g_demoFromRecorder = false;
        LogOut(g_demoTutorialHandoff
                   ? "[MISSION][DEMO] baseline restored; awaiting tutorial acknowledgement"
                   : "[MISSION][DEMO] baseline restored; player control released", true);
    }
}

std::string JsonEscape(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size() + 8);
    for (unsigned char c : value) {
        switch (c) {
            case '\"': escaped += "\\\""; break;
            case '\\': escaped += "\\\\"; break;
            case '\b': escaped += "\\b"; break;
            case '\f': escaped += "\\f"; break;
            case '\n': escaped += "\\n"; break;
            case '\r': escaped += "\\r"; break;
            case '\t': escaped += "\\t"; break;
            default:
                // Recorder resource names and paths are ordinary ASCII. Keep
                // hand-written control characters from producing invalid JSON.
                escaped.push_back(c < 0x20 ? ' ' : static_cast<char>(c));
                break;
        }
    }
    return escaped;
}

const char* RecContactResultName(::Mission::Contact::Result result) {
    switch (result) {
        case ::Mission::Contact::Result::Hit:         return "hit";
        case ::Mission::Contact::Result::Block:       return "block";
        case ::Mission::Contact::Result::RecoilGuard: return "recoil_guard";
        case ::Mission::Contact::Result::Throw:       return "throw";
        case ::Mission::Contact::Result::SpecialHit:  return "special_hit";
        case ::Mission::Contact::Result::GuardPoint:  return "guard_point";
        case ::Mission::Contact::Result::Unknown:     return "unknown";
        default:                                      return "none";
    }
}

void WriteNullableInt(std::ostream& out, int value) {
    if (value < 0) out << "null";
    else out << value;
}

bool WriteRecorderEntityTraceSidecar(const std::string& missionPath,
                                     std::string& outPath,
                                     std::string& outError) {
    outPath = missionPath;
    const std::size_t dot = outPath.find_last_of('.');
    if (dot != std::string::npos) outPath.resize(dot);
    outPath += ".entities.jsonl";

    std::ofstream out(outPath, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        outError = "cannot create " + outPath;
        return false;
    }

    const std::string p1Resource = !g_recSetup.player.character.empty()
        ? g_recSetup.player.character
        : CharacterSettings::GetCharacterInternalName(P1Char());
    const std::string p2Resource = !g_recSetup.dummy.character.empty()
        ? g_recSetup.dummy.character
        : CharacterSettings::GetCharacterInternalName(PlayerChar(2));
    out << "{\"record\":\"metadata\",\"schema\":\"efz_recorder_entity_trace\""
           ",\"version\":2,\"authoring_only\":true,\"sampled\":true"
           ",\"unordered\":true,\"non_strict\":true,\"slotCapacity\":"
        << CollisionDisplay::kProjectileRingSlotCapacity
        << ",\"playersSampled\":2"
        << ",\"effectiveFrameUnit\":\"freeze-excluded EFZ internal ticks\""
           ",\"p1CharacterId\":" << P1Char()
        << ",\"p1Resource\":\"" << JsonEscape(p1Resource) << "\""
        << ",\"p2CharacterId\":" << PlayerChar(2)
        << ",\"p2Resource\":\"" << JsonEscape(p2Resource) << "\""
        << ",\"eventCount\":" << g_recEntityTraceCount
        << ",\"droppedEvents\":" << g_recEntityTraceDropped
        << ",\"entityContactHookObserved\":"
        << (g_recEntityContactHookObserved ? "true" : "false")
        << ",\"contactJournalOverflow\":"
        << (g_recContactJournalOverflowObserved ? "true" : "false")
        << "}\n";

    for (std::size_t i = 0; i < g_recEntityTraceCount; ++i) {
        const RecEntityTraceEvent& event = g_recEntityTrace[i];
        const std::string eventResource =
            CharacterSettings::GetCharacterInternalName(event.characterId);
        const char* className = ::Mission::MoveData::ClassName(event.entityClass);
        if (!className || !className[0]) className = "unknown";
        const bool isContact = event.kind == RecEntityTraceKind::Contact;

        out << "{\"record\":\""
            << (isContact ? "entity_contact" : "entity_lifecycle")
            << "\",\"event\":\"" << RecEntityTraceKindName(event.kind) << "\""
            << ",\"evidence\":\""
            << (isContact ? "resolver_hook" : "ring_sample") << "\""
            << ",\"authoring_only\":true"
            << ",\"sampled\":" << (event.sampled ? "true" : "false")
            << ",\"unordered\":true,\"non_strict\":true"
            << ",\"effectiveFrame\":" << event.effectiveFrame
            << ",\"characterId\":" << event.characterId
            << ",\"resource\":\"" << JsonEscape(eventResource) << "\""
            << ",\"player\":" << event.player << ",\"slot\":" << event.slot
            << ",\"pattern\":";
        WriteNullableInt(out, event.pattern);
        out << ",\"priorPattern\":";
        WriteNullableInt(out, event.priorPattern);
        out << ",\"class\":\"" << className << "\""
            << ",\"attack\":" << (event.attack ? "true" : "false")
            << ",\"frame\":" << event.entityFrame
            << ",\"life\":" << event.life
            << ",\"destroyed\":" << event.destroyed
            << ",\"afterStep\":" << event.afterStep
            << ",\"sampleBasis\":\""
            << (isContact ? "resolver_transaction" :
                event.kind == RecEntityTraceKind::Despawn
                    ? "last_alive_sample" : "current_sample")
            << "\"";
        if (isContact) {
            out << ",\"sequence\":" << event.contactSequence
                << ",\"batch\":" << event.contactBatch
                << ",\"defender\":" << event.defender
                << ",\"result\":\""
                << RecContactResultName(event.contactResult) << "\""
                << ",\"lifeBefore\":" << event.lifeBefore
                << ",\"destroyedBefore\":" << event.destroyedBefore
                << ",\"comboBefore\":" << event.comboBefore
                << ",\"comboAfter\":" << event.comboAfter
                << ",\"hpBefore\":" << event.hpBefore
                << ",\"hpAfter\":" << event.hpAfter;
        }
        out << "}\n";
    }
    out.flush();
    if (!out.good()) {
        outError = "write failed for " + outPath;
        return false;
    }
    return true;
}

// Claim the final mission basename before serialization. The former tick-only
// name could collide when two saves happened in one millisecond (or after the
// 32-bit tick counter wrapped), and SaveMission would silently truncate the
// earlier take. Timestamp + tick + a process-local sequence makes collisions
// exceptional; CREATE_NEW is still the authority, with bounded retry for a
// second process or a pre-existing file.
bool ReserveRecordedMissionPath(const std::string& dir,
                                std::string& outPath,
                                std::string& outError) {
    SYSTEMTIME now = {};
    GetLocalTime(&now);
    const unsigned long tick = static_cast<unsigned long>(GetTickCount());
    constexpr int kMaxAttempts = 128;

    for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
        const unsigned long sequence =
            g_recSaveSequence.fetch_add(1, std::memory_order_relaxed) + 1;
        char name[96];
        _snprintf_s(name, sizeof(name), _TRUNCATE,
                    "recorded_%04u%02u%02u_%02u%02u%02u_%08lX_%06lu.json",
                    static_cast<unsigned int>(now.wYear),
                    static_cast<unsigned int>(now.wMonth),
                    static_cast<unsigned int>(now.wDay),
                    static_cast<unsigned int>(now.wHour),
                    static_cast<unsigned int>(now.wMinute),
                    static_cast<unsigned int>(now.wSecond),
                    tick, sequence);
        const std::string candidate = dir + "\\" + name;
        HANDLE file = CreateFileA(candidate.c_str(), GENERIC_WRITE, 0, nullptr,
                                  CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            CloseHandle(file);
            outPath = candidate;
            return true;
        }

        const DWORD error = GetLastError();
        if (error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS) {
            continue;
        }
        outError = "cannot reserve " + candidate + " (Windows error " +
                   std::to_string(static_cast<unsigned long>(error)) + ")";
        return false;
    }

    outError = "cannot choose a unique recorded mission filename after " +
               std::to_string(kMaxAttempts) + " attempts";
    return false;
}

// This is the complete data/capability boundary shared by ordinary in-match
// loads and title-prepared loads. A title transaction is not published until
// this succeeds, so Match entry only has to publish the already accepted value
// object; it never reparses or discovers a late lesson-capability failure.
bool ValidateMissionForRuntime(const ::Mission::Mission& mission,
                               const std::string& sourcePath,
                               std::string& outError) {
    // Steps are the format-1 contract; schema lessons validate through the
    // tutorial session instead and may be pure page/choice content (0-1, 1-3).
    const bool isLesson = mission.tutorialSchema > 0 && mission.hasLesson;
    if (mission.steps.empty() && !isLesson) {
        outError = "mission has no steps: " + sourcePath;
        return false;
    }
    if (isLesson) {
        const auto issues = ::Mission::Tutorial::RuntimeSupportIssues(mission);
        if (!issues.empty()) {
            outError = "lesson is unavailable: ";
            const size_t shown = (std::min)(issues.size(), static_cast<size_t>(3));
            for (size_t i = 0; i < shown; ++i) {
                if (i) outError += "; ";
                outError += issues[i];
            }
            if (issues.size() > shown) {
                outError += "; +" + std::to_string(issues.size() - shown) + " more";
            }
            return false;
        }
        if (mission.requiresExactBaseline &&
            (mission.savestate.empty() || !::Mission::StateDump::Available())) {
            outError = "lesson requires an exact start state, but it is unavailable";
            return false;
        }
    }
    for (size_t i = 0; i < mission.steps.size(); ++i) {
        if (!mission.steps[i].comboEndAfter) continue;
        if (mission.steps[i].optional) {
            outError = "comboEndAfter cannot be attached to optional step " +
                       std::to_string(i);
            return false;
        }
        if (i + 1 >= mission.steps.size()) {
            outError = "comboEndAfter requires a following setup/Part-2 step at " +
                       std::to_string(i);
            return false;
        }
    }
    outError.clear();
    return true;
}

bool ParseAndValidateMission(const std::string& sourcePath,
                             ::Mission::Mission& missionOut,
                             std::string& outError) {
    if (!::Mission::LoadMission(sourcePath, missionOut, outError)) return false;
    return ValidateMissionForRuntime(missionOut, sourcePath, outError);
}

} // namespace

// Internal value-load boundary used by both Runner::Load(path) and the title
// transaction consumer. It deliberately is not part of mission_engine.h: only
// this translation unit may claim that a Mission has passed the shared runtime
// validation above.
namespace Runner {
bool LoadPrepared(::Mission::Mission mission, const std::string& sourcePath,
                  std::string& outMsg, bool forceFreshMatch);
}

void Tick() {
    // Session pause menu first: while it is open the world is frozen and the
    // menu owns navigation input; it also self-closes if the session died.
    ::Mission::PauseMenu::Tick();

    std::lock_guard<std::recursive_mutex> runStateLock(g_runStateMx);
    g_demoFinishedThisTick = false;
    Snapshot s;
    s.contactEventEpoch = GetContactEventEpoch();
    s.directContactHookReady = IsDirectContactHookReady();
    s.entityContactHookReady = IsEntityContactHookReady();
    if (g_contactReadEpoch != s.contactEventEpoch) {
        // Savestate/hotswap/world reset: start after the reset watermark so no
        // pre-restore transaction can satisfy a new attempt.
        g_contactReadEpoch = s.contactEventEpoch;
        g_contactReadCursor = GetContactEventWatermark();
        g_contactOverflowLoggedEpoch = 0;
        g_contactOverflowLoggedCursor = 0;
    } else {
        const auto read = ReadCommittedContactEvents(
            g_contactReadCursor, g_contactReadEpoch,
            GetCompletedBattleUpdateBatch(), s.contactEvents.data(),
            s.contactEvents.size());
        g_contactReadCursor = read.consumedThrough;
        s.contactEventCount = static_cast<uint8_t>(read.count);
        s.contactEventOverflow = read.overflow;
        if (detailedLogging.load(std::memory_order_relaxed)) {
            for (size_t i = 0; i < read.count; ++i) {
                const auto& event = s.contactEvents[i];
                const char* source = event.source == ::Mission::Contact::Source::Entity
                    ? "entity" : event.source == ::Mission::Contact::Source::DirectPlayer
                    ? "direct" : "none";
                std::ostringstream trace;
                trace << "[CONTACT][EVENT] seq=" << event.sequence
                      << " epoch=" << event.worldEpoch
                      << " batch=" << event.batchId
                      << " source=" << source
                      << " owner=" << static_cast<int>(event.attacker)
                      << " target=" << static_cast<int>(event.defender)
                      << " slot=" << event.entitySlot
                      << " pattern=" << event.entityPattern
                      << " result=" << ::Mission::Contact::ResultName(event.result)
                      << " move=" << event.attackerMove
                      << " frame=" << event.attackerFrame
                      << " raw=" << event.rawStateBefore
                      << "->" << event.rawStateAfter
                      << " timer_life=" << event.timerBefore
                      << "->" << event.timerAfter
                      << " combo=" << event.comboBefore
                      << "->" << event.comboAfter
                      << " hp=" << event.defenderHpBefore
                      << "->" << event.defenderHpAfter
                      << " defenderMove=" << event.defenderMoveBefore
                      << "->" << event.defenderMove;
                LogOut(trace.str(), true);
            }
        }
        if (read.overflow &&
            (g_contactOverflowLoggedEpoch != g_contactReadEpoch ||
             g_contactOverflowLoggedCursor != read.consumedThrough)) {
            g_contactOverflowLoggedEpoch = g_contactReadEpoch;
            g_contactOverflowLoggedCursor = read.consumedThrough;
            LogOut("[CONTACT][OVERFLOW] contact evidence was lost epoch=" +
                   std::to_string(g_contactReadEpoch) + " consumedThrough=" +
                   std::to_string(read.consumedThrough), true);
        }
    }
    s.p1PolledAttackEdges = ConsumeInputPollAttackEdges(1);
    s.p1InputPollSerial = GetInputPollSerial(1);
    const uintptr_t p1 = GetPlayerBase(1);
    const uintptr_t p2 = GetPlayerBase(2);
    s.valid = (p1 != 0 && p2 != 0);
    if (s.valid) {
        s.p1Move = ReadMove(p1);
        s.p2Move = ReadMove(p2);
        s.p2FrameIdx = ReadShort(p2, 0xA);              // P2 frame index within the move
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
        // The IC/BIC/FIC superflash is NOT reliably reflected in +0x14C (see
        // frame_advantage.cpp), so also detect it by move ID (167 ground / 171 air
        // IC). Otherwise the sequence gap timer counts through the IC screen freeze
        // and the next step (e.g. Red-IC 5C > 22C > 2C) times out.
        const short p1SF = ReadShort(p1, 0x14C), p2SF = ReadShort(p2, 0x14C);
        const short p1HS = ReadShort(p1, 0x14A), p2HS = ReadShort(p2, 0x14A);
        const bool icFreeze =
            s.p1Move == GROUND_IC_ID || s.p1Move == AIR_IC_ID ||
            s.p2Move == GROUND_IC_ID || s.p2Move == AIR_IC_ID;
        s.freezeActive = (p1SF > 0) || (p2SF > 0) || (p1HS > 0) || (p2HS > 0) || icFreeze;
        s.p1HitState = ReadInt(p1, 0x168);  // raw producer latch; diagnostic/legacy only
    }
    g_snapshot = s;

    // Load the active character's baked move-ID reference once (cached per charId).
    // Only bother when a consumer (recorder/inspector) is active.
    if (s.valid && (g_recActive.load() || g_inspector.load() || g_runActive.load())) {
        ::Mission::MoveData::EnsureLoaded(displayData.p1CharID);
    }

    // Drive a hotswap-deferred mission setup (chars loaded -> apply values).
    ::Mission::Setup::Tick();
    std::string setupFailure;
    if (::Mission::Setup::TakeFailure(setupFailure)) {
        g_runSavePending = false;
        g_runRestorePending = false;
        g_runSaveRetries = 0;
        g_runStateSaved = false;
        SetRunnerStartupInputHold(false);
        const std::string playerMessage =
            "The lesson start state could not be applied: " + setupFailure +
            ". Return to Lessons and try again.";
        if (g_runActive.load(std::memory_order_acquire) &&
            g_runMission.tutorialSchema > 0 && g_runMission.hasLesson) {
            ::Mission::TutorialSession::NotifyStartupFailure(playerMessage);
        } else {
            g_runActive.store(false, std::memory_order_release);
            DirectDrawHook::AddMessage(playerMessage.c_str(), "MISSION",
                                       RGB(255, 120, 120), 3500, 0, 120);
        }
        LogOut("[MISSION][SETUP] runner startup aborted: " + setupFailure, true);
    }

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
    // so it doubles as the auto-retry baseline. Exact-state missions never
    // degrade to value-only setup: a timeout or integrity failure is surfaced
    // and the runner remains unavailable rather than silently dropping staged
    // entities.
    if (g_runRestorePending && s.valid && g_runActive.load() &&
        GetCurrentGamePhase() == GamePhase::Match) {
        const bool clear = !ImGuiImpl::IsVisible() && !::Mission::Setup::IsPending() &&
                           !CharacterHotswap::IsBusy() && !g_recActive.load() &&
                           !RoundIntroActive() && !::Mission::PauseMenu::IsOpen() &&
                           !PauseIntegration::IsPausedOrFrozen();
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
                ScopedRunnerSelfLoad selfLoad; // TriggerLoad inside is ours
                ok = ::Mission::StateDump::Restore(g_runMission.savestate, sdErr);
                // StateDump restore cancels injection owners in its load preamble.
                SetRunnerStartupInputHold(true);
            }
            if (ok) {
                SkipRoundIntro("state restore");
                g_runSavePending = false;   // buffer already holds the baseline
                g_runStateSaved = true;
                LogOut("[MISSION][RUN] embedded start state restored (auto-retry armed)", true);
            } else {
                g_runSavePending = false;
                g_runStateSaved = false;
                const std::string message =
                    "The exact mission start state could not be restored: " + sdErr;
                LogOut("[MISSION][RUN] " + message, true);
                if (g_runMission.tutorialSchema > 0 && g_runMission.hasLesson) {
                    ::Mission::TutorialSession::NotifyStartupFailure(message);
                } else {
                    DirectDrawHook::AddMessage(message.c_str(), "MISSION",
                                               RGB(255, 120, 120), 3500, 0, 120);
                    g_runActive.store(false, std::memory_order_release);
                    SetRunnerStartupInputHold(false);
                }
            }
        }
    } else {
        g_runRestoreSettle = 0;
        if (!g_runRestorePending) g_runRestoreTimeout = 0;
    }

    // Mission baseline savestate: after load/reset, save once the first UNPAUSED
    // gameplay frame settles (menu closed, match running, past the round intro,
    // two stable ticks) so every drop can snap back to it before player input
    // or runner validation is released. The deadline covers the WHOLE pending
    // interval, including Loading/setup/pause stalls before TriggerSave is ever
    // eligible; only the two-tick settle counter is freeze-gated.
    if (g_runSavePending) {
        ++g_runSaveElapsed;
        const std::string blocker = RunnerBaselineSaveBlocker(s);
        if (!blocker.empty()) {
            g_runSaveBlocker = blocker;
            g_runSaveSettle = 0;
        } else if (++g_runSaveSettle >= 2) {
            g_runSaveSettle = 0;
            if (SavestateHook::TriggerSave()) {
                g_runSavePending = false;
                g_runSaveRetries = 0;
                g_runSaveElapsed = 0;
                g_runSaveBlocker.clear();
                g_runStateSaved = true;
                LogOut("[MISSION][RUN] baseline savestate captured (auto-retry armed)", true);
            } else {
                ++g_runSaveRetries;
                g_runSaveBlocker = "the Practice checkpoint controller was not ready";
                // Right after match entry the Practice controller may not be
                // captured yet (PracticeTick lands ~200ms in) - keep the save
                // owed and retry on the next settle until the whole-startup
                // deadline expires.
                if (g_runSaveRetries == 1 || (g_runSaveRetries % 384) == 0) {
                    LogOut("[MISSION][RUN] baseline save not ready (attempt " +
                           std::to_string(g_runSaveRetries) + "); retrying", true);
                }
            }
        }
        if (g_runSavePending && g_runSaveElapsed >= kRunSaveDeadlineTicks) {
            FailRunnerBaselineSave(g_runSaveBlocker);
        }
    } else {
        g_runSaveSettle = 0;
        g_runSaveElapsed = 0;
        g_runSaveBlocker.clear();
    }
    if (g_runStartupInputHeld.load(std::memory_order_acquire) &&
        !g_runRestorePending && !g_runSavePending) {
        SetRunnerStartupInputHold(false);
    }

    // Guarded fallback only. Normal title missions go Title -> Loading -> Match
    // and create fighters on EFZ's game thread without visiting this screen.
    const GamePhase pendingMissionPhase = GetCurrentGamePhase();
    if (g_pendingMissionMode.load(std::memory_order_acquire) &&
        pendingMissionPhase == GamePhase::CharacterSelect) {
        g_pendingMissionSawCharacterSelect.store(true, std::memory_order_release);
    }
    if (PendingLaunchPolicy::ReachedSelectorExit(
            g_pendingMissionMode.load(std::memory_order_acquire),
            g_pendingMissionSawCharacterSelect.load(std::memory_order_acquire),
            pendingMissionPhase == GamePhase::Menu)) {
        const bool canceled = CancelPendingMissionLoad("Character Select exit");
        g_pendingMissionSawCharacterSelect.store(false, std::memory_order_release);
        if (canceled) {
            CharacterHotswap::CancelDirectPracticeLoad("Character Select exit");
            LogOut("[MISSION] pending title launch canceled after player left Character Select",
                   true);
        }
    }
    if (g_pendingMissionMode.load() && g_pendingHaveChars.load() &&
        !g_pendingCsQueued.load() &&
        pendingMissionPhase == GamePhase::CharacterSelect &&
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

    // A failed hotswap request is no longer "busy". Give the auto-drive a short
    // grace window, then hand Character Select back to the player while keeping
    // the pending mission load alive for match entry.
    static int s_pendingCsDriveIdle = 0;
    if (g_pendingMissionMode.load() && g_pendingHaveChars.load() &&
        g_pendingCsQueued.load() &&
        GetCurrentGamePhase() == GamePhase::CharacterSelect &&
        !CharacterHotswap::IsBusy()) {
        if (++s_pendingCsDriveIdle > 30) {
            s_pendingCsDriveIdle = 0;
            g_pendingHaveChars.store(false);
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
        SkipRoundIntro("pending mission entry");
        if (!CharacterHotswap::IsBusy() && ++s_missionEntrySettle >= 2) {
            s_missionEntrySettle = 0;
            g_pendingMissionMode.store(false);
            g_pendingMissionSawCharacterSelect.store(false, std::memory_order_release);
            std::string path;
            std::optional<::Mission::Mission> preparedMission;
            {
                std::lock_guard<std::mutex> lk(g_pendingLoadMx);
                path.swap(g_pendingLoadPath);
                preparedMission.swap(g_pendingLoadMission);
            }
            // The fallback matchup belongs to exactly the transaction just
            // consumed. Leaving it populated lets a later pathless generic
            // browser launch replay stale fighters/stage at Character Select.
            ClearPendingMatchMetadata();
            const auto consume = PendingLaunchPolicy::DecideConsume(
                !path.empty(), preparedMission.has_value());
            if (consume == PendingLaunchPolicy::ConsumeEffect::LoadPreparedMission) {
                // Title-picked mission: publish the exact value accepted by
                // the browser. The source file is intentionally not reopened.
                std::string msg;
                if (Runner::LoadPrepared(std::move(*preparedMission), path, msg,
                                         /*forceFreshMatch=*/false)) {
                    LogOut("[MISSION] mission mode: loaded '" + msg + "' after match entry", true);
                } else {
                    LogOut("[MISSION] mission mode: load failed: " + msg, true);
                    DirectDrawHook::AddMessage(("Mission load failed: " + msg).c_str(), "MISSION",
                                               RGB(255, 120, 120), 2500, 0, 120);
                }
            } else if (consume == PendingLaunchPolicy::ConsumeEffect::OpenGenericBrowser) {
                CustomMenu::Screens::OpenMissionBrowser();
                OpenMenu();
                LogOut("[MISSION] mission mode: opened browser after match entry", true);
            } else {
                const std::string msg =
                    "the prepared title mission transaction was incomplete";
                LogOut("[MISSION] mission mode: " + msg, true);
                DirectDrawHook::AddMessage(("Mission load failed: " + msg).c_str(),
                                           "MISSION", RGB(255, 120, 120),
                                           2500, 0, 120);
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
        if (::Mission::TutorialSession::IsActive()) {
            ::Mission::TutorialSession::Tick(s);   // schema lessons: session owns validation
        } else {
            TickRun(s);
        }
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
        // This API represents the pathless Practice -> mission-browser flow.
        // Retire every field owned by an older concrete title selection.
        {
            std::lock_guard<std::mutex> lk(g_pendingLoadMx);
            g_pendingLoadPath.clear();
            g_pendingLoadMission.reset();
        }
        ClearPendingMatchMetadata();
        CharacterHotswap::CancelDirectPracticeLoad("generic mission browser entry");
        g_pendingMissionSawCharacterSelect.store(false, std::memory_order_release);
        g_pendingRecordMode.store(false);   // mutually exclusive title picks
        g_pendingRecordSawCharacterSelect.store(false, std::memory_order_release);
    }
}

void SetPendingRecordMode(bool on) {
    g_pendingRecordMode.store(on);
    g_pendingRecordSawCharacterSelect.store(false, std::memory_order_release);
    if (on) {
        g_pendingMissionMode.store(false);
        {
            std::lock_guard<std::mutex> lk(g_pendingLoadMx);
            g_pendingLoadPath.clear();
            g_pendingLoadMission.reset();
        }
        ClearPendingMatchMetadata();
        CharacterHotswap::CancelDirectPracticeLoad("recording mode replaced mission launch");
    }
}

bool SetPendingMissionLoad(const std::string& missionPath, std::string& errorOut) {
    errorOut.clear();
    if (CharacterHotswap::IsBusy()) {
        errorOut = "another character load is still in progress";
        LogOut("[MISSION] pending launch rejected while character loader owns a transaction",
               true);
        return false;
    }
    // Parse and run the same runtime/capability boundary as Runner::Load before
    // publishing either half of the title transaction. A stale, malformed, or
    // unsupported lesson must leave the browser open instead of failing only
    // after Character Select/Match entry.
    ::Mission::Mission m;
    if (!ParseAndValidateMission(missionPath, m, errorOut)) {
        LogOut("[MISSION] pending launch rejected during preflight: " + errorOut,
               true);
        return false;
    }
    const int p1SelectId = CharacterHotswap::GetSelectIdForResourceName(
        m.player.character.c_str());
    const int p2SelectId = CharacterHotswap::GetSelectIdForResourceName(
        m.dummy.character.c_str());
    const bool isTutorial = m.tutorialSchema > 0 && m.hasLesson;
    if (isTutorial && (p1SelectId < 0 || p2SelectId < 0)) {
        errorOut = "the lesson's required fighters could not be resolved";
        LogOut("[MISSION] pending tutorial launch rejected: unresolved resource names p1='" +
               m.player.character + "' p2='" + m.dummy.character + "'", true);
        return false;
    }

    // A title launch owns presentation until its baseline is ready. Remove the
    // previous attempt's short-lived status immediately instead of letting it
    // leak through the launch cover.
    DirectDrawHook::RemoveMessagesByCategory("mission_run");
    {
        std::lock_guard<std::mutex> lk(g_pendingLoadMx);
        g_pendingLoadPath = missionPath;
        g_pendingLoadMission = m;
    }
    // Pre-parse the pinned mission matchup before leaving Title. The direct
    // request mirrors Replay PLAY's supported return-to-Loading route while
    // keeping game mode 1; Character Select remains a validated fallback.
    g_pendingHaveChars.store(false);
    g_pendingCsQueued.store(false);
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
                LogOut("[MISSION] direct Practice Loading armed (Character Select skipped)", true);
            } else {
                LogOut("[MISSION] direct Practice Loading unavailable; selector fallback armed", true);
            }
    } else {
        LogOut("[MISSION] cannot prepare pinned matchup: unresolved resource names p1='" +
               m.player.character + "' p2='" + m.dummy.character + "'", true);
    }
    g_pendingMissionSawCharacterSelect.store(false, std::memory_order_release);
    g_pendingMissionMode.store(true);
    g_pendingRecordMode.store(false);   // a mission pick cancels a pending record
    g_pendingRecordSawCharacterSelect.store(false, std::memory_order_release);
    LogOut("[MISSION] pending mission load queued: " + missionPath +
            (CharacterHotswap::IsDirectPracticeLoadPending()
                ? " (direct Loading)"
                : (g_pendingHaveChars.load() ? " (Character Select fallback)"
                                       : " (no chars in json - manual select)")), true);
    return true;
}

bool CancelPendingMissionLoad(const char* reason) {
    bool clearedSpecificMission = false;
    {
        std::lock_guard<std::mutex> lk(g_pendingLoadMx);
        const auto effect = PendingLaunchPolicy::DecideCancel(
            !g_pendingLoadPath.empty() || g_pendingLoadMission.has_value());
        if (effect == PendingLaunchPolicy::CancelEffect::ClearSpecificMission) {
            g_pendingLoadPath.clear();
            g_pendingLoadMission.reset();
            clearedSpecificMission = true;
        }
    }
    if (!clearedSpecificMission) {
        // SetPendingMissionMode(true) intentionally has no selected path: it
        // owns the normal Practice -> browser flow, not this failed launch.
        return false;
    }

    g_pendingMissionMode.store(false, std::memory_order_release);
    g_pendingMissionSawCharacterSelect.store(false, std::memory_order_release);
    ClearPendingMatchMetadata();
    LogOut(std::string("[MISSION] pending title mission canceled: ") +
           (reason && *reason ? reason : "unspecified"), true);
    return true;
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

bool PlayLoaded(std::string& outMsg, bool tutorialHandoff) {
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
    const bool tutorialActive = ::Mission::TutorialSession::IsActive();
    if (tutorialActive && !tutorialHandoff) {
        outMsg = "use Watch Demonstration from the Lesson Menu";
        return false;
    }
    if (tutorialHandoff && !tutorialActive) {
        outMsg = "no active tutorial can receive the demonstration";
        return false;
    }

    // A successful new request supersedes any terminal handoff without
    // exposing P1 between owners. Failed requests above leave it untouched.
    ClearDemoTutorialHandoff(false);
    g_demoText = g_runMission.demo;
    DirectDrawHook::RemoveMessagesByCategory("mission_run");
    g_demoFromRecorder = false;
    g_demoTutorialHandoff = RequiresTutorialRestoreAcknowledgement(
        tutorialHandoff, false, tutorialActive);
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
    ClearDemoTutorialHandoff(false);
    g_demoText = clip;
    DirectDrawHook::RemoveMessagesByCategory("mission_run");
    g_demoFromRecorder = true;
    g_demoTutorialHandoff = RequiresTutorialRestoreAcknowledgement(
        false, true, ::Mission::TutorialSession::IsActive());
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

RestoreResult PeekTutorialRestore(std::string& outMessage) {
    std::lock_guard<std::recursive_mutex> lock(g_runStateMx);
    outMessage = g_demoRestoreMessage;
    return g_demoRestoreResult;
}

void AcknowledgeTutorialRestore() {
    std::lock_guard<std::recursive_mutex> lock(g_runStateMx);
    if (g_demoRestoreResult == RestoreResult::None) return;
    const RestoreResult acknowledged = g_demoRestoreResult;
    ClearDemoTutorialHandoff(true);
    LogOut(acknowledged == RestoreResult::Restored
               ? "[MISSION][DEMO] tutorial acknowledged restored baseline; direct neutral released"
               : "[MISSION][DEMO] tutorial acknowledged restore failure; frozen error owns the return",
           true);
}

} // namespace Demo

void NotifyPracticeSessionReset(const char* reason) {
    std::lock_guard<std::recursive_mutex> lock(g_runStateMx);
    const RecorderPhase recorderPhase =
        g_recPhase.load(std::memory_order_acquire);
    const DemoPhase demoPhase = g_demoPhase.load(std::memory_order_acquire);
    const bool hadRecorder = recorderPhase != RecorderPhase::Idle;
    const bool hadDemo = demoPhase != DemoPhase::Idle;
    const bool hadRunner = g_runActive.load(std::memory_order_acquire) ||
                           ::Mission::TutorialSession::IsActive();
    const std::string resetReason = reason && *reason ? reason : "unspecified";
    const bool runnerOwnedReload = hadRunner &&
        (resetReason == "MissionDirectReload" ||
         (resetReason == "PracticeMatchExit" &&
          GetCurrentGamePhase() == GamePhase::Loading &&
          (CharacterHotswap::IsBusy() || ::Mission::Setup::IsPending())));

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
    g_recCountInTargetTicks = 0;
    g_recCountInElapsed = false;
    ResetRecorderCaptureState();
    g_recSetup = ::Mission::Mission();
    DirectDrawHook::RemoveMessagesByCategory("mission_record");
    RemoveRecorderBanner();

    if (hadDemo) {
        MacroController::ReleaseExclusivePlaybackHold();
        HoldDemoP1Neutral(false);
    }
    // Phase is already Idle while a tutorial terminal result awaits its fresh
    // snapshot acknowledgement, so hadDemo alone cannot detect this owner.
    ClearDemoTutorialHandoff(true);
    g_demoCancelRequested.store(false, std::memory_order_release);
    g_demoPhase.store(DemoPhase::Idle, std::memory_order_release);
    g_demoBaselineIssued = false;
    g_demoSettleFrames = 0;
    g_demoFinishedThisTick = false;
    g_demoText.clear();
    g_demoFromRecorder = false;
    ResetContactEventJournal(reason);

    if (hadRunner && !runnerOwnedReload) {
        if (g_runActive.load(std::memory_order_acquire)) Runner::Unload();
        else ::Mission::TutorialSession::End("Practice-session reset");
        g_runMission = ::Mission::Mission{};
        LogOut("[MISSION][RUN] invalidated for Practice-session reset reason=" +
               resetReason, true);
    } else if (runnerOwnedReload) {
        LogOut("[MISSION][RUN] preserved across owned Practice Loading transaction", true);
    }

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
    ResetContactEventJournal("savestate load");
    ::Mission::TutorialSession::NotifyStateLoaded();
    // Recorder: abort the synchronized take as a unit. Clearing only recipe
    // steps while the macro stream kept its pre-load bytes created an impossible
    // recipe/demo pair. The author explicitly starts a fresh count-in instead.
    if (g_recActive.load()) {
        MacroController::Stop();
        g_recActive.store(false, std::memory_order_release);
        ResetRecorderCaptureState();
        g_recReleaseFrames = 0;
        g_recCountInFrames = 0;
        g_recCountInTargetTicks = 0;
        g_recCountInElapsed = false;
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
    m.savestate = g_recSetup.savestate;   // mandatory exact frame-zero state
    for (const CapturedStep& cs : g_recSteps) {
        ::Mission::Step st;
        st.notation = cs.notation;        // auto-derived; author refines in the editor
        st.moveIds.push_back(static_cast<int>(cs.moveId));
        if (cs.automaticFollowupId > 0) {
            st.moveIds.push_back(static_cast<int>(cs.automaticFollowupId));
        }
        st.req = static_cast<::Mission::StepReq>(cs.req);
        st.optional = cs.optional;
        st.comboEndAfter = cs.comboEndAfter;
        st.charState = cs.charState;      // Akiko rekka crit determinism (-1 = n/a)
        if (cs.landed && cs.damage > 0) {
            st.damage = cs.damage;        // hit-variant fingerprint (clean hit/crit)
        }
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
    if (m.savestate.empty()) {
        outMsg = "exact start state is missing; retake the recording";
        return false;
    }

    const std::string root = ::Mission::ResolveMissionsRoot();
    if (root.empty()) { outMsg = "cannot resolve missions root"; return false; }
    const std::string dir = root + "\\_recorded";
    CreateDirectoryA(root.c_str(), nullptr);
    CreateDirectoryA(dir.c_str(), nullptr);

    std::string err;
    std::string acceptedPath;
    if (!ReserveRecordedMissionPath(dir, acceptedPath, err)) {
        outMsg = err;
        return false;
    }
    if (!::Mission::SaveMission(acceptedPath, m, err)) {
        // This empty reservation belongs to this save attempt. Do not leave it
        // in the mission browser when serialization fails.
        DeleteFileA(acceptedPath.c_str());
        outMsg = err;
        return false;
    }
    std::string tracePath;
    std::string traceError;
    if (WriteRecorderEntityTraceSidecar(acceptedPath, tracePath, traceError)) {
        LogOut("[MISSION][REC][ENTITY] wrote " +
               std::to_string(g_recEntityTraceCount) +
               " raw event(s), dropped=" +
               std::to_string(g_recEntityTraceDropped) + " -> " + tracePath, true);
    } else {
        LogOut("[MISSION][REC][ENTITY] sidecar write failed: " + traceError, true);
        DirectDrawHook::AddMessage(
            "Mission saved, but its raw entity trace could not be written",
            "MISSION", RGB(255, 180, 120), 3000, 0, 120);
    }
    outMsg = acceptedPath;
    g_recPhase.store(Phase::Idle, std::memory_order_release);
    DirectDrawHook::RemoveMessagesByCategory("mission_record");
    RemoveRecorderBanner();
    CustomMenu::Screens::NotifyMissionLibraryChanged();
    LogOut("[MISSION][REC] saved " + std::to_string(m.steps.size()) +
           " step(s) -> " + acceptedPath, true);
    return true;
}

} // namespace Recorder

namespace Runner {

static void ResetProgress() {
    g_runPhase = Phase::Idle;
    g_runStep = 0;
    g_runFailedStep.store(-1);
    g_runBaseline = 0;
    g_runDamageBaseline = 0;
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
    if (!ParseAndValidateMission(path, m, outMsg)) return false;
    return LoadPrepared(std::move(m), path, outMsg, forceFreshMatch);
}

bool LoadPrepared(::Mission::Mission mission, const std::string& sourcePath,
                  std::string& outMsg, bool forceFreshMatch) {
    std::lock_guard<std::recursive_mutex> lock(g_runStateMx);
    // Parsing/preflight happened without blocking the monitor/render threads.
    // Recheck ownership now that mission state can be published atomically.
    if (Demo::IsActive()) {
        outMsg = "cancel the active demonstration first";
        return false;
    }
    if (Recorder::IsSessionActive()) {
        outMsg = "save or discard the active recording session first";
        return false;
    }
    // Replacing the loaded runner is also a tutorial-session reset. Retire a
    // terminal handoff whose old Demo phase is already Idle before publishing
    // the new mission and its input owners.
    ClearDemoTutorialHandoff(true);
    // A mission startup owns P1 until its exact baseline is ready.  Cancel
    // ordinary macros and motion queues before enabling the neutral poll hold,
    // otherwise an immediate-input owner can contaminate that baseline.
    CancelAutoActionsAndMacros();
    g_runMission = std::move(mission);
    g_runAttempts = 0;
    ResetProgress();
    g_runStateSaved = false;
    g_runSaveSettle = 0;
    g_runSaveRetries = 0;
    g_runSaveElapsed = 0;
    g_runSaveBlocker.clear();
    g_runRestoreSettle = 0;
    g_runRestoreTimeout = 0;
    const bool isTutorial =
        g_runMission.tutorialSchema > 0 && g_runMission.hasLesson;
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
        g_runSavePending = false;
        g_runRestorePending = false;
        SetRunnerStartupInputHold(false);
        std::string setupFailure;
        const std::string failureMessage = ::Mission::Setup::TakeFailure(setupFailure)
            ? "lesson start state could not be applied: " + setupFailure
            : "mission setup could not start a clean Practice session";
        if (StartupFailurePolicy::Decide(isTutorial) ==
                StartupFailurePolicy::Effect::RetainTutorialError &&
            ::Mission::TutorialSession::Begin(g_runMission)) {
            // Error deliberately has no restart row until a baseline exists,
            // but it retains the runner/lesson identity, freezes the divergent
            // world, and gives the player a durable Return to Lessons path.
            ::Mission::TutorialSession::NotifyStartupFailure(
                "The " + failureMessage + ". Return to Lessons and try again.");
            outMsg = g_runMission.name.empty() ? sourcePath : g_runMission.name;
            LogOut("[MISSION][RUN] tutorial setup failed; retained durable Error: " +
                   failureMessage, true);
            return true;
        }

        // Ordinary missions have no startup Error frontend. Tear their
        // publication down completely rather than leaving an inactive runner
        // or a previous tutorial/dummy lease behind.
        g_runActive.store(false);
        ::Mission::TutorialSession::End("mission setup failed");
        g_runMission = ::Mission::Mission{};
        ResetProgress();
        outMsg = failureMessage;
        LogOut("[MISSION][RUN] load aborted: " + outMsg, true);
        return false;
    }
    if (g_runRestorePending) {
        LogOut("[MISSION][RUN] embedded start state present - will restore on settle", true);
    }
    outMsg = g_runMission.name.empty() ? sourcePath : g_runMission.name;
    LogOut("[MISSION][RUN] loaded '" + outMsg + "' (" + std::to_string(g_runMission.steps.size()) + " steps)", true);
    // tutorialSchema lessons hand validation/presentation to the session
    // runtime; the runner stays the loader/baseline owner (TickRun bypassed).
    if (isTutorial) {
        ::Mission::TutorialSession::Begin(g_runMission);
    } else {
        ::Mission::TutorialSession::End("non-tutorial load");
    }
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
    ::Mission::Setup::Cancel("runner unloaded");
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
    ClearDemoTutorialHandoff(true);
    g_runActive.store(false);
    g_runSavePending = false;
    g_runStateSaved = false;
    g_runSaveRetries = 0;
    g_runSaveElapsed = 0;
    g_runSaveBlocker.clear();
    g_runRestorePending = false;
    g_runRestoreSettle = 0;
    g_runRestoreTimeout = 0;
    SetRunnerStartupInputHold(false);
    DirectDrawHook::RemoveMessagesByCategory("mission_run");
    ::Mission::TutorialSession::End("runner unload");
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
bool RequestBaselineRestore(std::string& outMsg) {
    std::lock_guard<std::recursive_mutex> lock(g_runStateMx);
    if (!g_runActive.load(std::memory_order_acquire)) { outMsg = "no active session"; return false; }
    if (!g_runStateSaved || !SavestateHook::IsInstalled()) { outMsg = "no baseline saved"; return false; }
    if (g_recActive.load() || CharacterHotswap::IsBusy() || ::Mission::Setup::IsPending()) {
        outMsg = "restore blocked (busy)";
        return false;
    }
    ScopedRunnerSelfLoad selfLoad;
    const bool ok = SavestateHook::TriggerLoad();
    if (ok) SkipRoundIntro("tutorial checkpoint restore");
    else outMsg = "baseline load failed";
    return ok;
}
bool HasBaseline() {
    std::lock_guard<std::recursive_mutex> lock(g_runStateMx);
    return g_runActive.load(std::memory_order_acquire) &&
           g_runStateSaved && SavestateHook::IsInstalled();
}
bool HasDemo() {
    std::lock_guard<std::recursive_mutex> lock(g_runStateMx);
    return g_runActive.load(std::memory_order_acquire) && !g_runMission.demo.empty();
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
