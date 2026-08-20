#include "../../../include/game/mission/mission_engine.h"
#include "../../../include/game/mission/mission_data.h"
#include "../../../include/game/mission/mission_authoring.h"
#include "../../../include/game/mission/mission_moves.h"
#include "../../../include/game/mission/mission_movedata.h"
#include "../../../include/game/mission/mission_contact_attribution_policy.h"
#include "../../../include/game/mission/mission_entity_lifecycle_policy.h"
#include "../../../include/game/mission/mission_entity_fanout_policy.h"
#include "../../../include/game/mission/mission_entity_schedule_policy.h"
#include "../../../include/game/mission/mission_entity_review_policy.h"
#include "../../../include/game/mission/mission_render_snapshot.h"
#include "../../../include/game/mission/mission_legacy_entity_migration.h"
#include "../../../include/game/mission/mission_sequence_policy.h"
#include "../../../include/game/mission/mission_setup.h"
#include "../../../include/game/mission/recorder_entity_trace.h"
#include "../../../include/game/mission/entity_contact_phase_bridge_policy.h"
#include "../../../include/game/mission/entity_command_origin_policy.h"
#include "../../../include/game/mission/move_notation_tables.h"  // compiled per-char notation
#include "../../../include/game/mission/entity_notation_tables.h" // compiled per-char entity notation
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
#include "../../../include/input/input_core.h"
#include "../../../include/input/input_hook.h"
#include "../../../include/utils/config.h"
#include "../../../include/utils/pause_integration.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace Mission::Engine {

namespace {

constexpr const char* kEntityScheduleRuntimeMarkerV1 =
    "runtime:entity-contact-schedule-v1";
constexpr const char* kEntityScheduleRuntimeMarkerV2 =
    "runtime:entity-contact-schedule-v2";
constexpr const char* kEntityScheduleRuntimeMarkerV3 =
    "runtime:entity-contact-schedule-v3";
constexpr const char* kEntityScheduleRuntimeMarkerV4 =
    "runtime:entity-lifecycle-schedule-v4";
constexpr const char* kEntityScheduleRuntimeMarkerV5 =
    "runtime:entity-fanout-schedule-v5";

static_assert(
    ::Mission::EntityCommandOriginPolicy::kEntityRingSlotCapacity ==
        static_cast<int>(CollisionDisplay::kProjectileRingSlotCapacity),
    "entity-command policy must match the probed projectile-ring capacity");

bool IsKnownRuntimeMarker(const std::string& reason) {
    return reason == kEntityScheduleRuntimeMarkerV1 ||
           reason == kEntityScheduleRuntimeMarkerV2 ||
           reason == kEntityScheduleRuntimeMarkerV3 ||
           reason == kEntityScheduleRuntimeMarkerV4 ||
           reason == kEntityScheduleRuntimeMarkerV5 ||
           ::Mission::SequencePolicy::IsLegacyUnmatchedInputDiagnostic(reason);
}

int EntityScheduleRuntimeMarkerVersion(const ::Mission::Mission& mission) {
    for (const std::string& reason : mission.reviewRequired) {
        if (reason == kEntityScheduleRuntimeMarkerV5) return 5;
        if (reason == kEntityScheduleRuntimeMarkerV4) return 4;
        if (reason == kEntityScheduleRuntimeMarkerV3) return 3;
        if (reason == kEntityScheduleRuntimeMarkerV2) return 2;
        if (reason == kEntityScheduleRuntimeMarkerV1) return 1;
    }
    return 0;
}

::Mission::EntitySchedulePolicy::LifecycleKind EntityLifecycleKind(
    const std::string& value) {
    using Kind = ::Mission::EntitySchedulePolicy::LifecycleKind;
    if (value == "baseline") return Kind::Baseline;
    if (value == "spawn") return Kind::Spawn;
    if (value == "morph") return Kind::Morph;
    if (value == "despawn") return Kind::Despawn;
    return Kind::None;
}

bool HasAuthorReviewObligations(const ::Mission::Mission& mission) {
    for (const std::string& reason : mission.reviewRequired) {
        if (!IsKnownRuntimeMarker(reason) &&
            !::Mission::EntityReviewPolicy::IsObsoletePostContactReview(
                mission, reason)) {
            return true;
        }
    }
    return false;
}

Snapshot g_snapshot;
uint32_t g_contactReadCursor = 0;
uint32_t g_contactReadEpoch = 0;
uint32_t g_contactOverflowLoggedEpoch = 0;
uint32_t g_contactOverflowLoggedCursor = 0;
std::atomic<bool> g_inspector{false};
std::atomic<bool> g_pendingMissionMode{false}; // title MISSION -> auto-open browser
std::atomic<bool> g_pendingMissionSawCharacterSelect{false};
// A concrete title-picked mission owns P1 from the moment the browser accepts
// it until Runner::LoadPrepared has published the restore/save transaction.
// Without this pre-runner owner, direct Loading exposes one or two live Match
// polls before the runner is loaded; a retained menu direction can start a
// jump and contaminate the baseline that follows.
std::atomic<bool> g_pendingStartupInputHeld{false};
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
    // Mini-Mai's bound S commands execute entirely in the existing summon
    // entity and never assign Mai a player move ID.  Preserve the exact
    // command transition instead of fabricating one.
    ::Mission::EntityCommandOrigin entityCommand;
    bool        landed = false;
    std::string notation;  // auto-derived from the trigger input (author fixes specials)
    int         req = 0;   // ::Mission::StepReq value (default from `landed`)
    bool        optional = false;
    short       cmdToken = 99; // engine command index that produced this move (99 = none/normal)
    int         hits = 0;      // combo hits this step produced (multi-hit -> StepReq::Hits)
    int         startFrame = 0;// recorder frame the move began (delay measurement)
    uint32_t    startBattleBatch = 0; // closed Battle update exposing this action
    uint32_t    startActionOrder = 0; // exact monotonic recorder action order
    int         maxDelay = 0;  // measured land delay for projectile steps (0 = immediate)
    int         gap = 0;       // non-frozen frames between the previous step's last
                               // event and this move (delayed-button allowance)
    bool        comboEndAfter = false; // a recorded combo alive->dead edge occurred
                                       // after this step and before the next one
    bool        connected = false; // legacy +0x168 sampled-edge heuristic used by
                                   // format 1; not an authoritative typed contact
    bool        directContact = false; // exact player resolver owns this contact;
                                       // entity contacts can never satisfy it
    std::string directContactResult;   // typed exact resolver result requirement
    int         charState = -1;    // P1 character-state stamp at move start (-1 = n/a;
                                   // Akiko = bullet cycle 0..2 -> rekka crit variant)
    int         damage = 0;        // combo-damage delta this step's hits produced
                                   // (0 = whiff/blocked/no requirement)
    uint32_t    firstContactSequence = 0;
    uint32_t    lastContactSequence = 0;
    uint8_t     expectedAttackMask = 0; // actual causal EFZ poll edge
    std::vector<short> automaticFollowupIds; // observed no-input phases accepted
                                              // as part of this authored action
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
int g_recCountInFrames = 0;
int g_recCountInTargetTicks = 0;
bool g_recCountInElapsed = false; // capture/start on the following clear tick
int g_recLeasePollWaitTicks = 0;
// Count-in owns P1's poll lane all the way through the exact StateDump + macro
// start transaction.  Without this lease, a short press at "0" can enter an
// attack and be released between monitor samples: the resulting move is baked
// into the savestate while its input is absent from the demonstration.
bool g_recInputLeaseHeld = false;
bool g_recPollSnapshotValid = false;
bool g_recPollSavedActive = false;
bool g_recPollSavedObservation = false;
uint8_t g_recPollSavedMask = 0;
std::atomic<bool> g_recCaptureMenuRequested{false};
// Set when a capture menu closes. This survives an open+close pair that occurs
// entirely between recorder ticks, allowing the next tick to install the same
// neutral-input release handshake as an ordinary longer pause.
std::atomic<bool> g_recCaptureMenuReleasePending{false};
std::atomic<bool> g_recCaptureMenuSuspendedView{false};
bool g_recCaptureMenuSuspended = false;
uint32_t g_recCaptureMenuLastPollSerial = 0;
int g_recCaptureMenuNeutralPolls = 0;
std::atomic<bool> g_recMenuCommandHandoffRequested{false};
std::atomic<bool> g_recMenuCommandHandoffDone{false};
bool g_recMenuCommandHandoffActive = false;
bool g_recMenuCommandPauseOwned = false;
uint32_t g_recMenuCommandHandoffLastPollSerial = 0;
int g_recMenuCommandHandoffNeutralPolls = 0;
bool g_recStopPending = false;
bool g_recStopSawPostRequestTick = false;
int g_recStopWaitTicks = 0;
bool g_recStartedThisTick = false;
bool g_recTakeIntegrityValid = true;
std::string g_recTakeIntegrityError;
std::string g_recSealedDemo;
std::atomic<bool> g_recPreviewAvailableView{false};
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
// Once a real combo recovery is observed, neutral jump/dash actions become
// authored setup until the next hit begins Part 2. Pre-combo positioning is
// still arranged in PRE-RECORD and therefore stays out of the recipe.
bool  g_recSetupSegmentOpen = false;
int   g_recPendingComboEndStep = -1; // candidate committed only if a later step exists
uint32_t g_recPendingComboEndEntitySequence = 0; // same candidate when the
                                                 // last accepted hit belonged to
                                                 // the strict entity side-lane
int   g_recPendingComboEndAfterAction = -1; // latest action known to have begun
                                            // before the pending recovery edge
int   g_recComboEndFrame = 0;        // gap anchor: actual recovery edge, not last hit
uint32_t g_recLastLandedContactSequence = 0;
int   g_recLastLandedDirectStep = -1;
uint32_t g_recLastLandedEntitySequence = 0;
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
int   g_recUncommittedAttackFrame = -1; // physical attack edge not followed by a move instance
struct PendingRecorderAttack {
    uint32_t serial = 0;
    uint8_t mask = 0;
    int frame = 0;
};
constexpr std::size_t kPendingRecorderAttackCapacity =
    Snapshot::kMaxPolledAttackEdges;
std::array<PendingRecorderAttack, kPendingRecorderAttackCapacity>
    g_recPendingAttacks{};
std::size_t g_recPendingAttackCount = 0;
bool g_recPendingAttackOverflow = false;

int SelectPendingRecorderAttack(
    uint32_t currentPollSerial, uint8_t expectedMask,
    int windowTicks =
        ::Mission::SequencePolicy::kRecorderInputCausalityWindowTicks) {
    std::array<::Mission::SequencePolicy::RecorderAttackEdgeCandidate,
               kPendingRecorderAttackCapacity> candidates{};
    for (std::size_t index = 0; index < g_recPendingAttackCount; ++index) {
        candidates[index].serial = g_recPendingAttacks[index].serial;
        candidates[index].mask = g_recPendingAttacks[index].mask;
        candidates[index].frame = g_recPendingAttacks[index].frame;
    }
    return ::Mission::SequencePolicy::SelectNewestEligibleAttackEdge(
        candidates.data(), g_recPendingAttackCount, currentPollSerial,
        g_recFrame, windowTicks, expectedMask);
}

void ErasePendingRecorderAttack(std::size_t chosen) {
    if (chosen >= g_recPendingAttackCount) return;
    for (std::size_t index = chosen + 1;
         index < g_recPendingAttackCount; ++index) {
        g_recPendingAttacks[index - 1] = g_recPendingAttacks[index];
    }
    --g_recPendingAttackCount;
}

int   g_recAkikoRekkaRep = 0;     // C-rekka rep index (Akiko crosses behind per rep, so
                                  // the PHYSICAL input alternates 623C/421C)
uint32_t g_recNextActionOrder = 0;
uint64_t g_recEntitySampleSerial = 0;

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
    uint16_t frameTick = 0;
    double x = 0.0;
    double y = 0.0;
    int16_t life = 0;
    uint32_t destroyed = 0;
    uint32_t generation = 0;
};

struct RecEntityTraceEvent {
    RecEntityTraceKind kind = RecEntityTraceKind::Baseline;
    int effectiveFrame = 0;
    // Monotonic RecorderCapture invocation. Ring edges and exact resolver
    // records carrying the same value were observed in one closed sample.
    uint64_t sampleSerial = 0;
    int player = 1;
    int characterId = -1;
    int afterStep = -1;       // zero-based CapturedStep index; -1 = before opener
    uint32_t afterActionOrder = 0; // exact action order observed before this event
    int slot = -1;
    int pattern = -1;         // current pattern; -1 on a sampled despawn
    int priorPattern = -1;
    int entityFrame = 0;
    int entityFrameTick = 0;
    double x = 0.0;
    double y = 0.0;
    int life = 0;
    uint32_t destroyed = 0;
    uint32_t generation = 0;
    ::Mission::MoveData::Cls entityClass = ::Mission::MoveData::Cls::Unknown;
    bool attack = false;
    bool sampled = true;

    // Populated only for exact entity->player resolver journal records.
    uint32_t contactSequence = 0;
    uint32_t contactBatch = 0;
    uint32_t contactWorldEpoch = 0;
    ::Mission::Contact::Source contactSource = ::Mission::Contact::Source::None;
    short attackerMove = 0;
    int attributedStep = -1; // exact CapturedStep selected for a direct contact
    int defender = 0;
    ::Mission::Contact::Result contactResult = ::Mission::Contact::Result::None;
    int lifeBefore = 0;
    int destroyedBefore = 0;
    int comboBefore = 0;
    int comboAfter = 0;
    int hpBefore = 0;
    int hpAfter = 0;
    // A later semantic action/contact proved that the observed combo recovery
    // was an intentional setup boundary rather than merely the end of the take.
    // Entity-owned boundaries compile into the concurrent schedule instead of
    // being attached to an unrelated linear move step.
    bool comboEndAfter = false;
};

constexpr std::size_t kRecEntityTraceCapacity =
    ::Mission::RecorderEntityTrace::kTraceEventCapacity;
std::array<std::array<RecEntitySlotState,
                      CollisionDisplay::kProjectileRingSlotCapacity>, 2>
    g_recEntitySlots{};
std::array<RecEntityTraceEvent, kRecEntityTraceCapacity> g_recEntityTrace{};
std::size_t g_recEntityTraceCount = 0;
uint32_t g_recEntityTraceDropped = 0;
bool g_recEntityContactHookObserved = false;
bool g_recDirectContactHookObserved = false;
bool g_recEntityContactHookComplete = true;
uint32_t g_recContactWorldEpoch = 0;
bool g_recContactEpochDiscontinuity = false;
bool g_recContactJournalOverflowObserved = false;
bool g_recEntityAttributionRequired = false;
bool g_recUnclassifiedContactRequired = false;
bool g_recLegacyContactAttributionRequired = false;
bool g_recMixedDirectResultsRequired = false;
bool g_recUnboundCommandOriginRequired = false;
std::array<bool, 2> g_recEntityProbeUnavailableLogged{};
struct RecEntityCursorState {
    bool established = false;
    uint16_t allocationCursor = 0;
};
std::array<RecEntityCursorState, 2> g_recEntityCursors{};
std::array<bool, 2> g_recEntityProbeIncomplete{};
std::array<bool, 2> g_recEntityAllocationAmbiguous{};
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
std::atomic<int> g_runFailedEntityRequirement{-1}; // causal projectile row, if any
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
struct PendingRunEntityCommandEdge {
    bool valid = false;
    int action = -1;
    uint32_t serial = 0;
    uint8_t mask = 0;
    int age = 0;
};
PendingRunEntityCommandEdge g_runPendingEntityCommandEdge;
int g_runEntityCommandActionObservedThisTick = -1;
bool g_runAwaitingComboEnd = false; // current recipe requires an authored combo.end
uint32_t g_runAwaitingComboEndAfterSequence = 0; // accepted contact owning that edge
int  g_runPrevHitState = 0;      // previous raw +0x168 (legacy edge heuristic)
bool g_runConnectSeen = false;   // format-1 Connect heuristic observed an edge
bool g_runDirectConnectSeen = false;
int g_runDirectHits = 0;
int g_runDirectContacts = 0;
int g_runDirectDamage = 0;
uint32_t g_runDirectObservedThrough = 0;
uint32_t g_runDirectFirstSequence = 0;
uint32_t g_runDirectLastSequence = 0;
// Concurrent strict entity-contact schedule. These counters survive ordinary
// action-step advancement and reset only with the attempt/world baseline.
std::vector<int> g_runEntityContactsSeen;
std::vector<int> g_runEntityComboHitsSeen;
std::vector<int> g_runEntityDamageSeen;
std::vector<int> g_runEntityElapsed;
std::vector<uint32_t> g_runEntityFirstSequence;
std::vector<uint32_t> g_runEntityLastSequence;
std::vector<::Mission::EntityLifecyclePolicy::ObjectiveState>
    g_runEntityLifecycleStates;
std::vector<int> g_runEntityLifecycleElapsed;
uint32_t g_runEntityObservedThrough = 0;
int g_runEntitySegment = 0;
// Once a recorder-flexible move lands fewer contacts, EFZ's proration changes
// every later damage delta in that combo part. Keep grading action/contact
// identity, but suspend recorded damage fingerprints until the authored
// combo.end rebases the segment.
bool g_runSegmentHasVariableHitCount = false;
struct RunEntityLifecycleObservation {
    uint32_t generation = 0;
    ::Mission::EntitySchedulePolicy::LifecycleKind kind =
        ::Mission::EntitySchedulePolicy::LifecycleKind::None;
    int pattern = -1;
    int priorPattern = -1;
    int producerAction = -1;
    int segment = 0;
    uint64_t sampleSerial = 0;
    bool lifecycleClaimed = false;
};
struct RunEntitySlotLineage {
    bool established = false;
    bool alive = false;
    uint16_t pattern = 0;
    uint32_t generation = 0;
    std::vector<RunEntityLifecycleObservation> producers;
};
std::array<RunEntitySlotLineage,
           CollisionDisplay::kProjectileRingSlotCapacity>
    g_runEntitySlots{};
bool g_runEntityLineageReady = false;
bool g_runEntityCursorEstablished = false;
uint16_t g_runEntityAllocationCursor = 0;
std::size_t g_runEntityProducerHistoryCount = 0;
uint64_t g_runEntitySampleSerial = 0;
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

// Demonstration session. Preparation/restoration is frame-thread-owned; the
// public frontend only queues a request by changing this phase.
using DemoPhase = ::Mission::Engine::Demo::Phase;
std::atomic<DemoPhase> g_demoPhase{DemoPhase::Idle};
std::atomic<bool> g_demoCancelRequested{false};
bool g_demoBaselineIssued = false;
bool g_demoFinishedThisTick = false;
bool g_worldRestoredThisTick = false;
bool g_demoTransitionFreezeOwned = false;
int g_demoRoundRestoreSettleTicks = 0;
int g_demoTerminalRecipeHoldTicks = 0;
int g_demoRecipeSettleTicksRemaining = 0;
std::string g_demoText;
bool g_demoFromRecorder = false;
bool g_demoTutorialHandoff = false;
Demo::RestoreResult g_demoRestoreResult = Demo::RestoreResult::None;
std::string g_demoRestoreMessage;

enum class RunObservationMode : uint8_t {
    PlayerAttempt = 0,
    DemoPresentation,
};

RunObservationMode g_runObservationMode = RunObservationMode::PlayerAttempt;

class ScopedRunObservationMode {
public:
    explicit ScopedRunObservationMode(RunObservationMode mode)
        : previous_(g_runObservationMode) {
        g_runObservationMode = mode;
    }

    ~ScopedRunObservationMode() {
        g_runObservationMode = previous_;
    }

private:
    RunObservationMode previous_;
};

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
                            "MISSION COUNT-IN %.1fs | get ready | %s = cancel",
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
    // Prefer the per-character compiled table (252->"41236A" etc.); fall back to
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
// Preferred notation for a captured move: the per-character compiled table
// (623C, 236A (Fire), ...), then the universal table, then the input guess.
const char* KnownCaptureNotation(short moveId) {
    const std::string internal =
        CharacterSettings::GetCharacterInternalName(P1Char());
    const char* header =
        ::Mission::MoveNames::Lookup(internal.c_str(), moveId);
    if (header && header[0]) return header;
    const char* universal = ::Mission::Moves::Notation(moveId);
    return (universal && universal[0]) ? universal : "";
}

uint8_t ExpectedAttackMaskForKnownMove(short moveId) {
    const std::string internal =
        CharacterSettings::GetCharacterInternalName(P1Char());
    const auto* catalogued =
        ::Mission::MoveNames::Find(internal.c_str(), moveId);
    if (catalogued && ::Mission::MoveNames::HasRole(
            catalogued->role, ::Mission::MoveNames::MoveRole::InputFollowup)) {
        return ::Mission::SequencePolicy::
            ExpectedAttackMaskFromFinalNotationStep(catalogued->note);
    }
    return ::Mission::SequencePolicy::ExpectedAttackMaskForAction(
        KnownCaptureNotation(moveId), moveId);
}

std::string CaptureNotation(short moveId) {
    const char* known = KnownCaptureNotation(moveId);
    if (known[0]) return known;
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

bool HasPendingRecordedComboEnd() {
    return g_recPendingComboEndStep >= 0 ||
           g_recPendingComboEndEntitySequence != 0;
}

int PendingRecordedComboEndAfterAction() {
    if (g_recPendingComboEndAfterAction >= 0) {
        return g_recPendingComboEndAfterAction;
    }
    return g_recPendingComboEndStep;
}

RecEntityTraceEvent* FindRecordedEntityContact(uint32_t sequence) {
    if (sequence == 0) return nullptr;
    for (std::size_t i = g_recEntityTraceCount; i-- > 0;) {
        RecEntityTraceEvent& event = g_recEntityTrace[i];
        if (event.kind == RecEntityTraceKind::Contact &&
            event.contactSource == ::Mission::Contact::Source::Entity &&
            event.contactSequence == sequence) {
            return &event;
        }
    }
    return nullptr;
}

bool CommitPendingRecordedComboEnd(const char* reason) {
    bool committed = false;
    std::string anchor;
    if (g_recPendingComboEndEntitySequence != 0) {
        RecEntityTraceEvent* event =
            FindRecordedEntityContact(g_recPendingComboEndEntitySequence);
        if (!event) {
            g_recUnclassifiedContactRequired = true;
            LogOut("[MISSION][REC] could not resolve pending entity combo.end sequence=" +
                   std::to_string(g_recPendingComboEndEntitySequence), true);
        } else {
            if (!event->comboEndAfter) {
                event->comboEndAfter = true;
                g_recComboEndCountView.fetch_add(1, std::memory_order_release);
            }
            committed = true;
            anchor = "entitySequence=" +
                std::to_string(g_recPendingComboEndEntitySequence);
        }
    } else if (g_recPendingComboEndStep >= 0 &&
               g_recPendingComboEndStep < static_cast<int>(g_recSteps.size())) {
        if (!g_recSteps[g_recPendingComboEndStep].comboEndAfter) {
            g_recSteps[g_recPendingComboEndStep].comboEndAfter = true;
            g_recComboEndCountView.fetch_add(1, std::memory_order_release);
        }
        committed = true;
        anchor = "step=" + std::to_string(g_recPendingComboEndStep);
    }
    if (committed) {
        LogOut("[MISSION][REC] committed combo.end after " + anchor +
               " reason=" + (reason ? reason : "unknown"), true);
    }
    g_recPendingComboEndStep = -1;
    g_recPendingComboEndEntitySequence = 0;
    g_recPendingComboEndAfterAction = -1;
    return committed;
}

bool LatestRecordedLandedSourceEndsCombo() {
    if (g_recLastLandedEntitySequence != 0) {
        const RecEntityTraceEvent* event =
            FindRecordedEntityContact(g_recLastLandedEntitySequence);
        return event && event->comboEndAfter;
    }
    return g_recLastLandedDirectStep >= 0 &&
           g_recLastLandedDirectStep < static_cast<int>(g_recSteps.size()) &&
           g_recSteps[static_cast<std::size_t>(g_recLastLandedDirectStep)]
               .comboEndAfter;
}

bool ArmRecordedComboEndFromLatestContact(int currentAction,
                                          const char* reason) {
    if (HasPendingRecordedComboEnd()) return true;
    if (LatestRecordedLandedSourceEndsCombo()) return false;

    int landedAction = -1;
    std::string anchor;
    if (g_recLastLandedEntitySequence != 0) {
        if (RecEntityTraceEvent* event =
                FindRecordedEntityContact(g_recLastLandedEntitySequence)) {
            g_recPendingComboEndEntitySequence =
                g_recLastLandedEntitySequence;
            g_recPendingComboEndStep = -1;
            landedAction = event->afterStep;
            anchor = "entitySequence=" +
                std::to_string(g_recLastLandedEntitySequence);
        }
    }
    if (anchor.empty() && g_recLastLandedDirectStep >= 0 &&
        g_recLastLandedDirectStep < static_cast<int>(g_recSteps.size())) {
        g_recPendingComboEndStep = g_recLastLandedDirectStep;
        g_recPendingComboEndEntitySequence = 0;
        landedAction = g_recLastLandedDirectStep;
        anchor = "step=" + std::to_string(g_recLastLandedDirectStep);
    }
    if (anchor.empty()) return false;

    g_recPendingComboEndAfterAction =
        (std::max)(landedAction, currentAction);
    g_recComboEndFrame = g_recFrame;
    g_recSetupSegmentOpen = true;
    LogOut("[MISSION][REC] exact combo.end candidate after " + anchor +
           " beforeSequence=" +
           std::to_string(g_recLastLandedContactSequence) + " reason=" +
           (reason ? reason : "ordered contact"), true);
    return true;
}

void CommitExactComboBoundaryBeforeHit(
    const ::Mission::Contact::Event& contact, int currentAction,
    const char* reason) {
    namespace Policy = ::Mission::ContactAttributionPolicy;
    if (!::Mission::SequencePolicy::ExactContactBeginsNewCombo(
            g_recAnyLanded, Policy::IsHitResult(contact.result),
            contact.comboBefore, contact.comboAfter, contact.sequence,
            g_recLastLandedContactSequence)) {
        return;
    }

    // Resolver-local zero baseline or N->smaller rollover is stronger evidence
    // than a sampled zero-combo frame. Part 2 can recover and hit entirely
    // between monitor samples, so the sampled heuristic cannot see every edge.
    if (ArmRecordedComboEndFromLatestContact(currentAction, reason)) {
        (void)CommitPendingRecordedComboEnd(
            "ordered contact began a fresh combo baseline");
    }
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

uint32_t RecorderActionOrderAfterStep(int step) {
    return step >= 0 && step < static_cast<int>(g_recSteps.size())
        ? g_recSteps[static_cast<std::size_t>(step)].startActionOrder
        : 0;
}

int RecorderAfterStepForContactBatch(uint32_t contactBatch,
                                     int legacyFallbackStep) {
    if (contactBatch == 0) return legacyFallbackStep;
    int afterStep = -1;
    for (int i = 0; i < static_cast<int>(g_recSteps.size()); ++i) {
        const CapturedStep& step = g_recSteps[static_cast<std::size_t>(i)];
        if (!::Mission::EntitySchedulePolicy::RecordedActionCanPrecedeContact(
                step.startBattleBatch, contactBatch)) {
            break;
        }
        afterStep = i;
    }
    return afterStep;
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
        CollisionDisplay::ProjectileRingCursorProbe cursors{};
        if (!CollisionDisplay::ProbeProjectileRing(
                playerIndex, current.data(), current.size(), &cursors) ||
            !cursors.readable) {
            g_recEntityProbeIncomplete[playerSlot] = true;
            if (!g_recEntityProbeUnavailableLogged[playerSlot]) {
                g_recEntityProbeUnavailableLogged[playerSlot] = true;
                LogOut("[MISSION][REC][ENTITY] P" + std::to_string(playerIndex) +
                       " ring/cursor sample unavailable; prior slot state retained "
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
        std::size_t sampledSpawnCount = 0;
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
                if (transition == ::Mission::RecorderEntityTrace::
                                      SampledSlotTransition::Baseline &&
                    now.alive && prior.generation == 0) {
                    prior.generation = 1;
                } else if (transition == ::Mission::RecorderEntityTrace::
                                             SampledSlotTransition::Spawn) {
                    ++sampledSpawnCount;
                    ++prior.generation;
                    if (prior.generation == 0) prior.generation = 1;
                }
                RecEntityTraceEvent event;
                event.effectiveFrame = g_recFrame;
                event.sampleSerial = g_recEntitySampleSerial;
                event.player = playerIndex;
                event.characterId = charId;
                event.afterStep = RecorderAfterStep();
                event.afterActionOrder =
                    RecorderActionOrderAfterStep(event.afterStep);
                event.slot = static_cast<int>(slot);
                event.generation = prior.generation;
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
                    event.entityFrameTick = prior.frameTick;
                    event.x = prior.x;
                    event.y = prior.y;
                    event.life = prior.life;
                    event.destroyed = prior.destroyed;
                    metadataPattern = prior.pattern;
                    break;
                    default:
                        break;
                }
                if (now.alive && event.kind != RecEntityTraceKind::Despawn) {
                    event.entityFrame = now.frame;
                    event.entityFrameTick = now.frameTick;
                    event.x = now.x;
                    event.y = now.y;
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
                prior.frameTick = now.frameTick;
                prior.x = now.x;
                prior.y = now.y;
                prior.life = now.life;
                prior.destroyed = now.destroyed;
            } else {
                prior.pattern = 0;
                prior.frame = 0;
                prior.frameTick = 0;
                prior.x = 0.0;
                prior.y = 0.0;
                prior.life = 0;
                prior.destroyed = 0;
            }
        }

        RecEntityCursorState& priorCursor = g_recEntityCursors[playerSlot];
        if (::Mission::RecorderEntityTrace::
                AllocationAdvancedWithoutObservedSpawn(
                    priorCursor.established,
                    priorCursor.allocationCursor,
                    cursors.allocationCursor,
                    sampledSpawnCount)) {
            if (!g_recEntityAllocationAmbiguous[playerSlot]) {
                LogOut("[MISSION][REC][ENTITY] P" +
                       std::to_string(playerIndex) +
                       " allocator cursor advanced "
                       "without a visible spawn; exact slot generation is "
                       "ambiguous and this take requires review", true);
            }
            g_recEntityAllocationAmbiguous[playerSlot] = true;
        }
        priorCursor.established = true;
        priorCursor.allocationCursor = cursors.allocationCursor;
    }
}

void ConsumeRecordedEntityCommandOrigins(std::size_t firstTraceIndex,
                                         uint32_t currentPollSerial,
                                         uint32_t currentBattleBatch,
                                         int stepBeforeSample) {
    if (firstTraceIndex >= g_recEntityTraceCount ||
        g_recSetup.player.character.empty()) {
        return;
    }
    namespace Policy = ::Mission::EntityCommandOriginPolicy;
    for (std::size_t index = firstTraceIndex;
         index < g_recEntityTraceCount; ++index) {
        RecEntityTraceEvent& event = g_recEntityTrace[index];
        if (event.player != 1 || event.kind != RecEntityTraceKind::Morph ||
            event.priorPattern <= 0 || event.priorPattern > 0xFFFF ||
            event.pattern <= 0 || event.pattern > 0xFFFF) {
            continue;
        }
        const Policy::Origin* origin = Policy::FindByTransition(
            g_recSetup.player.character.c_str(),
            static_cast<uint16_t>(event.priorPattern),
            static_cast<uint16_t>(event.pattern));
        if (!origin) continue;

        // A player move and an entity-only command first exposed by the same
        // closed sample have no portable relative ordering in the endpoint
        // trace.  Do not silently append the command after that move: a later
        // replay could then grade the inverse order.  Retake at a sample where
        // the ordered input/action evidence is unambiguous.
        if (RecorderAfterStep() != stepBeforeSample) {
            g_recUnboundCommandOriginRequired = true;
            LogOut("[MISSION][REC][INPUT] command-origin transition " +
                       std::to_string(event.priorPattern) + "->" +
                       std::to_string(event.pattern) + " (" +
                       origin->notation +
                       ") shared an unordered sample with another authored "
                       "action; retained for review",
                   true);
            continue;
        }

        const int chosen = SelectPendingRecorderAttack(
            currentPollSerial, origin->attackMask);
        if (chosen < 0) {
            g_recUnboundCommandOriginRequired = true;
            LogOut("[MISSION][REC][INPUT] command-origin transition " +
                       std::to_string(event.priorPattern) + "->" +
                       std::to_string(event.pattern) + " (" +
                       origin->notation +
                       ") has no fresh causal D/S edge; retained for review",
                   true);
            continue;
        }
        const PendingRecorderAttack& pending =
            g_recPendingAttacks[static_cast<std::size_t>(chosen)];

        int gapAnchor = g_recLastEventFrame;
        if (HasPendingRecordedComboEnd()) {
            gapAnchor = g_recComboEndFrame;
            CommitPendingRecordedComboEnd("later entity command");
        }
        CapturedStep command;
        command.notation = origin->notation;
        command.req = static_cast<int>(::Mission::StepReq::Move);
        command.startFrame = pending.frame;
        command.startBattleBatch = currentBattleBatch;
        command.startActionOrder = ++g_recNextActionOrder;
        if (command.startActionOrder == 0) {
            command.startActionOrder = ++g_recNextActionOrder;
        }
        command.gap = g_recSteps.empty()
            ? 0 : (std::max)(0, pending.frame - gapAnchor);
        command.expectedAttackMask = origin->attackMask;
        command.entityCommand.present = true;
        command.entityCommand.slot = event.slot;
        command.entityCommand.generation =
            static_cast<int>(event.generation);
        command.entityCommand.rootPattern = event.priorPattern;
        command.entityCommand.activationPattern = event.pattern;
        g_recSteps.push_back(command);
        const int commandStep = static_cast<int>(g_recSteps.size()) - 1;
        g_recStepCountView.store(static_cast<int>(g_recSteps.size()),
                                 std::memory_order_release);
        g_recLastEventFrame = pending.frame;

        // The ring transition was sampled before this input-only action could
        // be appended.  Rewrite only the literal bound transition, plus its
        // curated same-sample hit child (Blitz #454).  Unrelated lifecycle
        // edges in the same inventory retain their original action gate.
        for (std::size_t boundIndex = firstTraceIndex;
             boundIndex < g_recEntityTraceCount; ++boundIndex) {
            RecEntityTraceEvent& bound = g_recEntityTrace[boundIndex];
            const bool exactTransition =
                boundIndex == index && bound.slot == event.slot &&
                bound.generation == event.generation;
            const bool sameSampleCuratedChild =
                origin->contactPattern != origin->activationPattern &&
                bound.sampleSerial == event.sampleSerial &&
                bound.player == event.player &&
                (bound.kind == RecEntityTraceKind::Spawn ||
                 bound.kind == RecEntityTraceKind::Morph) &&
                bound.pattern == origin->contactPattern;
            if (!exactTransition && !sameSampleCuratedChild) continue;
            bound.afterStep = commandStep;
            bound.afterActionOrder = command.startActionOrder;
        }

        LogOut("[MISSION][REC][INPUT] bound command-origin " +
                   std::string(origin->notation) + " transition=" +
                   std::to_string(event.priorPattern) + "->" +
                   std::to_string(event.pattern) + " slot=" +
                   std::to_string(event.slot) + " generation=" +
                   std::to_string(event.generation) + " frame=" +
                   std::to_string(pending.frame) + " poll=" +
                   std::to_string(pending.serial) + " step=" +
                   std::to_string(commandStep),
               true);
        ErasePendingRecorderAttack(static_cast<std::size_t>(chosen));
    }
}

int FindCapturedStepForDirectMove(short moveId) {
    const int lastIndex = static_cast<int>(g_recSteps.size()) - 1;
    for (int i = lastIndex; i >= 0; --i) {
        const CapturedStep& step = g_recSteps[static_cast<std::size_t>(i)];
        const bool crossedCommittedBoundary = step.comboEndAfter && i < lastIndex;
        const int pendingAfterAction = PendingRecordedComboEndAfterAction();
        const bool crossedPendingBoundary = HasPendingRecordedComboEnd() &&
            lastIndex > pendingAfterAction && i <= pendingAfterAction;
        if (crossedCommittedBoundary || crossedPendingBoundary) break;
        if (step.moveId == moveId ||
            std::find(step.automaticFollowupIds.begin(),
                      step.automaticFollowupIds.end(), moveId) !=
                step.automaticFollowupIds.end()) {
            return i;
        }
    }
    return -1;
}

const char* ExactContactRequirementName(::Mission::Contact::Result result) {
    using Result = ::Mission::Contact::Result;
    switch (result) {
        case Result::Hit:         return "hit";
        case Result::Block:       return "block";
        case Result::RecoilGuard: return "recoil_guard";
        case Result::Throw:       return "throw";
        case Result::SpecialHit:  return "special";
        case Result::GuardPoint:  return "guard_point";
        default:                  return nullptr;
    }
}

bool ApplyDirectRecorderContact(const ::Mission::Contact::Event& contact) {
    namespace Policy = ::Mission::ContactAttributionPolicy;
    if (!Policy::IsDirectLearnerContact(contact)) return false;

    const int stepIndex = FindCapturedStepForDirectMove(contact.attackerMove);
    if (stepIndex < 0) {
        g_recUnclassifiedContactRequired = true;
        LogOut("[MISSION][REC][CONTACT] direct contact move=" +
               std::to_string(contact.attackerMove) +
               " has no captured action; author review required", true);
        return false;
    }

    CapturedStep& step = g_recSteps[static_cast<std::size_t>(stepIndex)];
    step.directContact = true;
    step.connected = true;
    if (const char* result = ExactContactRequirementName(contact.result)) {
        if (step.directContactResult.empty()) {
            step.directContactResult = result;
        } else if (step.directContactResult != result) {
            g_recMixedDirectResultsRequired = true;
        }
    }
    if (step.firstContactSequence == 0) {
        step.firstContactSequence = contact.sequence;
    }
    step.lastContactSequence = contact.sequence;
    const int hitContribution = Policy::DirectHitContribution(contact);
    if (hitContribution > 0) {
        const bool firstHit = step.hits == 0;
        step.landed = true;
        step.req = static_cast<int>(::Mission::StepReq::Land);
        step.hits += hitContribution;
        step.damage += Policy::DirectDamageContribution(contact);

        if (firstHit && IsAkikoRekkaFinisher(step.moveId)) {
            const char* tag = ClassifyAkikoCleanHit(step.moveId);
            if (*tag && step.notation.find("clean") == std::string::npos) {
                step.notation += tag;
                LogOut(std::string("[MISSION][REC] finisher variant:") + tag, true);
            }
        }
    }
    LogOut("[MISSION][REC][CONTACT] direct move=" +
           std::to_string(contact.attackerMove) + " -> step=" +
           std::to_string(stepIndex) + " result=" +
           ::Mission::Contact::ResultName(contact.result) + " hits+=" +
           std::to_string(hitContribution), true);
    return true;
}

void ObserveOrderedDirectRecorderContact(
    const ::Mission::Contact::Event& contact, int stepIndex) {
    namespace Policy = ::Mission::ContactAttributionPolicy;
    if (!Policy::IsDirectLearnerContact(contact) || stepIndex < 0) return;

    CommitExactComboBoundaryBeforeHit(
        contact, RecorderAfterStep(), "exact direct resolver hit");
    const int hitContribution = Policy::DirectHitContribution(contact);
    if (hitContribution > 0) {
        if (HasPendingRecordedComboEnd() &&
            stepIndex > PendingRecordedComboEndAfterAction()) {
            (void)CommitPendingRecordedComboEnd(
                "post-edge exact direct hit");
        }
        g_recAnyLanded = true;
        if (contact.comboBefore == 0) g_recSetupSegmentOpen = false;
        if (contact.sequence >= g_recLastLandedContactSequence) {
            g_recLastLandedContactSequence = contact.sequence;
            g_recLastLandedDirectStep = stepIndex;
            g_recLastLandedEntitySequence = 0;
        }
    }
    g_recLastEventFrame = g_recFrame;
}

// Preserve every hook-backed resolver transaction beside the unordered ring
// observations. Direct P1 contacts also provide the narrow format-1 ownership
// bridge above. Entity contacts remain authoring evidence only: without a
// producer/lineage graph they must never be compiled into a linear step.
int RecorderCaptureContacts(
    const Snapshot& s,
    const std::array<bool, Snapshot::kMaxContactEvents>& preAppliedContacts,
    const std::array<int, Snapshot::kMaxContactEvents>& preAppliedSteps,
    int lastStepBeforeSample) {
    namespace Policy = ::Mission::ContactAttributionPolicy;
    g_recEntityContactHookObserved =
        g_recEntityContactHookObserved || s.entityContactHookReady;
    g_recEntityContactHookComplete =
        g_recEntityContactHookComplete && s.entityContactHookReady;
    g_recDirectContactHookObserved =
        g_recDirectContactHookObserved || s.directContactHookReady;
    g_recContactJournalOverflowObserved =
        g_recContactJournalOverflowObserved || s.contactEventOverflow;

    int directLearnerHits = 0;

    for (std::size_t i = 0; i < s.contactEventCount; ++i) {
        const auto& contact = s.contactEvents[i];
        int directAttributedStep = preAppliedSteps[i];
        bool directApplied = false;
        if ((contact.source != ::Mission::Contact::Source::Entity &&
             contact.source != ::Mission::Contact::Source::DirectPlayer) ||
            (contact.attacker != 1 && contact.attacker != 2)) {
            continue;
        }
        if (contact.source == ::Mission::Contact::Source::Entity) {
            g_recEntityContactHookObserved = true;
        } else {
            g_recDirectContactHookObserved = true;
        }

        if (contact.attacker == 1 && contact.defender == 2) {
            if (Policy::IsUnresolvedLearnerEntityContact(contact)) {
                g_recEntityAttributionRequired = true;
            } else if (contact.source == ::Mission::Contact::Source::DirectPlayer &&
                       contact.result == ::Mission::Contact::Result::Unknown) {
                g_recUnclassifiedContactRequired = true;
            }
            if (!s.contactEventOverflow && s.directContactHookReady &&
                Policy::IsDirectLearnerContact(contact)) {
                const bool preApplied = preAppliedContacts[i];
                if (!preApplied) {
                    directAttributedStep =
                        FindCapturedStepForDirectMove(contact.attackerMove);
                }
                const bool applied = preApplied || ApplyDirectRecorderContact(contact);
                if (applied) {
                    directApplied = true;
                    directLearnerHits += Policy::DirectHitContribution(contact);
                }
            }
        }
        if (directApplied) {
            // ApplyDirectRecorderContact may have run in the prepass so a new
            // same-sample step could be opened safely. Timeline ownership is
            // deliberately updated only here, in global resolver-sequence
            // order, so an interleaved entity cannot be reordered behind it.
            ObserveOrderedDirectRecorderContact(contact,
                                                directAttributedStep);
        }

        RecEntityTraceEvent event;
        event.kind = RecEntityTraceKind::Contact;
        event.effectiveFrame = g_recFrame;
        event.sampleSerial = g_recEntitySampleSerial;
        event.player = contact.attacker;
        const int charId = PlayerChar(contact.attacker);
        (void)::Mission::MoveData::EnsureLoaded(charId);
        event.characterId = charId;
        const int coarseAfterStep =
            contact.batchId != 0 && s.completedBattleUpdateBatch != 0 &&
                    contact.batchId < s.completedBattleUpdateBatch
                ? lastStepBeforeSample
                : RecorderAfterStep();
        event.afterStep = RecorderAfterStepForContactBatch(
            contact.batchId, coarseAfterStep);
        event.afterActionOrder =
            RecorderActionOrderAfterStep(event.afterStep);
        event.slot = contact.source == ::Mission::Contact::Source::Entity
            ? contact.entitySlot : -1;
        event.pattern = contact.source == ::Mission::Contact::Source::Entity
            ? contact.entityPattern : -1;
        event.entityFrame = contact.attackerFrame;
        if (contact.source == ::Mission::Contact::Source::Entity &&
            contact.attacker >= 1 && contact.attacker <= 2 &&
            contact.entitySlot >= 0 &&
            contact.entitySlot < static_cast<int>(CollisionDisplay::kProjectileRingSlotCapacity)) {
            const auto& slotState = g_recEntitySlots[
                static_cast<std::size_t>(contact.attacker - 1)][
                static_cast<std::size_t>(contact.entitySlot)];
            event.generation = slotState.generation;
            event.entityFrameTick = slotState.frameTick;
            event.x = slotState.x;
            event.y = slotState.y;
        }
        event.life = contact.timerAfter;
        event.destroyed = static_cast<uint32_t>(contact.rawStateAfter);
        event.sampled = false;
        event.contactSequence = contact.sequence;
        event.contactBatch = contact.batchId;
        event.contactWorldEpoch = contact.worldEpoch;
        if (contact.worldEpoch != 0) {
            if (g_recContactWorldEpoch == 0) {
                g_recContactWorldEpoch = contact.worldEpoch;
            } else if (g_recContactWorldEpoch != contact.worldEpoch) {
                g_recContactEpochDiscontinuity = true;
            }
        }
        event.contactSource = contact.source;
        event.attackerMove = contact.attackerMove;
        if (contact.source == ::Mission::Contact::Source::DirectPlayer &&
            contact.result != ::Mission::Contact::Result::None &&
            contact.result != ::Mission::Contact::Result::Unknown) {
            event.attributedStep = directAttributedStep;
        }
        event.defender = contact.defender;
        event.contactResult = contact.result;
        event.lifeBefore = contact.timerBefore;
        event.destroyedBefore = contact.rawStateBefore;
        event.comboBefore = contact.comboBefore;
        event.comboAfter = contact.comboAfter;
        event.hpBefore = contact.defenderHpBefore;
        event.hpAfter = contact.defenderHpAfter;
        if (contact.source == ::Mission::Contact::Source::Entity) {
            FillRecEntityMetadata(event, event.pattern);
        }

        const bool committedLearnerEntityContact =
            contact.source == ::Mission::Contact::Source::Entity &&
            contact.attacker == 1 && contact.defender == 2 &&
            Policy::IsCommittedResult(contact.result);
        const bool learnerEntityHit = committedLearnerEntityContact &&
            Policy::IsHitResult(contact.result);
        if (learnerEntityHit) {
            CommitExactComboBoundaryBeforeHit(
                contact, event.afterStep, "exact entity resolver hit");
            // A contact beginning a fresh combo (0->1 or a sampled N->1
            // counter rollover) after recovery proves the next segment exists,
            // even when the player had already
            // armed its setter/meaty before the defender recovered.
            if (HasPendingRecordedComboEnd() &&
                ::Mission::EntitySchedulePolicy::OrderedContactBeginsNewCombo(
                    contact.comboBefore, contact.comboAfter) &&
                contact.sequence > g_recLastLandedContactSequence &&
                event.afterStep >= PendingRecordedComboEndAfterAction()) {
                CommitPendingRecordedComboEnd("post-edge exact entity hit");
            }
        }
        if (committedLearnerEntityContact) {
            // Delayed setup timing is measured from semantic contact, not from
            // the setter's much earlier cast frame.
            g_recLastEventFrame = g_recFrame;
        }
        AppendRecEntityTrace(event);
        if (learnerEntityHit) {
            // Publish this entity as the latest landed source only after its
            // trace record exists; a later ordered contact in the same batch
            // can then anchor comboEndAfter to this exact sequence.
            g_recAnyLanded = true;
            if (::Mission::EntitySchedulePolicy::OrderedContactBeginsNewCombo(
                    contact.comboBefore, contact.comboAfter)) {
                g_recSetupSegmentOpen = false;
            }
            if (contact.sequence >= g_recLastLandedContactSequence) {
                g_recLastLandedContactSequence = contact.sequence;
                g_recLastLandedDirectStep = -1;
                g_recLastLandedEntitySequence = contact.sequence;
            }
        }
    }
    return directLearnerHits;
}

void RecorderCapture(const Snapshot& s, bool advanceLogicalTick) {
    ++g_recEntitySampleSerial;
    if (g_recEntitySampleSerial == 0) ++g_recEntitySampleSerial;
    const short cur = s.p1Move;
    // Contact batches may precede the action visible in this Snapshot. Keep
    // the prior tail so RecorderCaptureContacts can place those transactions
    // before a step opened by the later closed battle-update batch.
    const int lastStepBeforeSample = RecorderAfterStep();
    const uint32_t lastLandedSequenceBeforeSample =
        g_recLastLandedContactSequence;
    const int lastLandedDirectStepBeforeSample =
        g_recLastLandedDirectStep;
    const uint32_t lastLandedEntitySequenceBeforeSample =
        g_recLastLandedEntitySequence;
    bool openedPlayerAction = false;
    int openedPlayerActionStep = -1;
    uint8_t openedPlayerActionExpectedMask = 0;
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
    const auto queuePendingAttack = [](uint32_t serial, uint8_t mask) {
        if (serial == 0 || mask == 0) return;
        PendingRecorderAttack pending;
        pending.serial = serial;
        pending.mask = mask;
        pending.frame = g_recFrame;
        if (g_recPendingAttackCount < g_recPendingAttacks.size()) {
            g_recPendingAttacks[g_recPendingAttackCount++] = pending;
        } else {
            // Preserve the oldest unexplained inputs and fail closed in Review;
            // silently overwriting one would certify a take whose action stream
            // no longer matches its demonstration.
            g_recPendingAttackOverflow = true;
        }
        g_recUncommittedAttackFrame = pending.frame;
    };
    if (s.p1PolledAttackEdgeOverflow) {
        if (!g_recPendingAttackOverflow) {
            LogOut("[MISSION][REC][INPUT] ordered poll-edge journal overflowed; "
                   "dropped=" +
                       std::to_string(s.p1PolledAttackEdgesDropped),
                   true);
        }
        g_recPendingAttackOverflow = true;
    }
    if (s.p1PolledAttackEdgeCount > 0) {
        for (std::size_t i = 0; i < s.p1PolledAttackEdgeCount; ++i) {
            queuePendingAttack(s.p1PolledAttackEdgeEvents[i].serial,
                               s.p1PolledAttackEdgeEvents[i].mask);
        }
    } else if (s.p1PolledAttackEdges != 0 &&
               s.p1PolledAttackEdgeSerial != 0) {
        // Compatibility for synthetic Snapshot tests and older producers.
        queuePendingAttack(s.p1PolledAttackEdgeSerial,
                           s.p1PolledAttackEdges);
    }
    // Capture attacks AND known combo-relevant moves the attack classifier misses
    // (IC / air IC via the table). Movement (jump/dash) counts only mid-combo, so
    // neutral hops/dashes aren't recorded as steps.
    const bool comboActive = (s.p1Combo > 0) || s.p2InStun;
    const bool isMovement  = ::Mission::Moves::IsMovementMove(cur);
    const bool opensSetupOnThisSample = g_recComboWasAlive && !comboActive;
    // .pat-classified specials/supers are captured even when the generic classifier
    // misses them: a projectile/summon CAST (e.g. 236X) does no damage on the caster
    // frame, so IsAttackMove is false, yet it's a real combo step.
    const bool isSpecial   = ::Mission::MoveData::IsSpecialOrSuper(P1Char(), cur);
    const bool isCapture   = IsAttackMove(cur) || isSpecial ||
                             (::Mission::Moves::IsKnownComboMove(cur) &&
                              (!isMovement || comboActive ||
                               g_recSetupSegmentOpen ||
                               opensSetupOnThisSample));

    // Work out whether the current sample entered a new instance before the
    // direct-contact prepass. A same-ID cancel (5A -> 5A) can rewind the move
    // frame and resolve its first hit in the same monitor batch. Applying that
    // transaction before creating the new CapturedStep credits it backwards to
    // the previous 5A.
    constexpr int kReentryInputWindowTicks = 12;   // ~4 visual frames
    const uint8_t currentMoveExpectedMask =
        ExpectedAttackMaskForKnownMove(cur);
    const bool matchingFreshAttack = SelectPendingRecorderAttack(
        s.p1InputPollSerial, currentMoveExpectedMask,
        kReentryInputWindowTicks) >= 0;
    const bool freshInput = matchingFreshAttack ||
        g_recTokenEdgeAge <= kReentryInputWindowTicks;
    const bool physicalSameMoveReentry = isCapture && cur == g_recLastAttackId &&
        cur == g_recPrevMove && freshInput &&
        ::Mission::SequencePolicy::IsMoveInstanceEdge(
            cur, s.p1FrameIdx, g_recPrevMove, g_recPrevFrameIdx);
    // A hit from the old instance and the rewind into the next same-ID attack
    // can land in one monitor sample. The journal retains the attack frame at
    // contact time; a frame beyond the newly rewound live frame belongs to the
    // prior instance rather than the new one.
    bool priorSameIdContactInSample = false;
    if (physicalSameMoveReentry) {
        for (std::size_t i = 0; i < s.contactEventCount; ++i) {
            const auto& contact = s.contactEvents[i];
            if (::Mission::ContactAttributionPolicy::IsDirectLearnerContact(contact) &&
                ::Mission::ContactAttributionPolicy::
                    SameIdContactBelongsToPriorInstance(
                        physicalSameMoveReentry, contact.attackerMove, cur,
                        contact.attackerFrame, s.p1FrameIdx)) {
                priorSameIdContactInSample = true;
                break;
            }
        }
    }
    const bool priorInstanceDidSomething = (!g_recSteps.empty() &&
        g_recSteps.back().moveId == cur &&
        (g_recSteps.back().landed || g_recSteps.back().connected)) ||
        priorSameIdContactInSample;
    const bool sameMoveReentered =
        physicalSameMoveReentry && priorInstanceDidSomething;
    const bool enteringNewCurrentInstance = isCapture &&
        (cur != g_recLastAttackId || sameMoveReentered);

    const int aggregateComboRise = (std::max)(0, s.p1Combo - g_recPrevCombo);
    const bool comboRose = aggregateComboRise > 0 && s.p1Combo > 0;
    // When the direct resolver journal is unavailable/overflowed, retain the
    // legacy aggregate adapter only so old environments can still record. Any
    // take that actually relies on it is stamped for review and cannot silently
    // become a strict mission.
    const bool useLegacyContactAttribution =
        !s.directContactHookReady || s.contactEventOverflow;
    std::array<bool, Snapshot::kMaxContactEvents> preAppliedDirectContacts{};
    std::array<int, Snapshot::kMaxContactEvents> preAppliedDirectSteps{};
    preAppliedDirectSteps.fill(-1);
    if (!useLegacyContactAttribution) {
        for (std::size_t i = 0; i < s.contactEventCount; ++i) {
            const auto& contact = s.contactEvents[i];
            // A newly entered different move does not have its CapturedStep yet.
            // Defer that contact to the postpass; otherwise a later 5A in
            // 5A > 5B > 5A could be assigned backwards to step zero.
            const bool priorSameIdContact =
                ::Mission::ContactAttributionPolicy::
                    SameIdContactBelongsToPriorInstance(
                        physicalSameMoveReentry, contact.attackerMove, cur,
                        contact.attackerFrame, s.p1FrameIdx);
            const bool belongsToExistingInstance = priorSameIdContact ||
                contact.attackerMove != cur || !enteringNewCurrentInstance;
            const int existingStep = belongsToExistingInstance
                ? FindCapturedStepForDirectMove(contact.attackerMove)
                : -1;
            if (existingStep >= 0 &&
                ::Mission::ContactAttributionPolicy::IsDirectLearnerContact(contact) &&
                ApplyDirectRecorderContact(contact)) {
                preAppliedDirectContacts[i] = true;
                preAppliedDirectSteps[i] = existingStep;
            }
        }
    }
    const auto comboBoundary = ::Mission::SequencePolicy::DetectSampledComboBoundary(
        g_recComboWasAlive, comboActive, g_recPrevCombo, s.p1Combo);
    const bool comboEndedEdge = comboBoundary !=
        ::Mission::SequencePolicy::SampledComboBoundary::None;
    // Legacy format-1 connect heuristic: associate a raw +0x168 0->nonzero edge
    // with the current capture move. This is retained for file compatibility;
    // scripts/entities can produce false positives and it is not typed contact.
    const bool contactEdge = (g_recPrevHitState == 0) && (s.p1HitState != 0);
    g_recPrevHitState = s.p1HitState;
    if (useLegacyContactAttribution && contactEdge && !g_recSteps.empty() &&
        cur == g_recLastAttackId) {
        g_recSteps.back().connected = true;
        g_recLegacyContactAttributionRequired = true;
    }

    // Preserve the *observed* one-piece-vs-setup distinction. Do not mutate the
    // previous step yet: if recording stops here this was merely the natural end
    // of the final combo. A later captured step commits this candidate as an
    // explicit comboEndAfter boundary. Actions that start after recovery anchor
    // their gap there; a pre-started meaty is already armed instead.
    if (comboEndedEdge && g_recAnyLanded && !g_recSteps.empty()) {
        const bool rollover = comboBoundary ==
            ::Mission::SequencePolicy::SampledComboBoundary::CounterRollover;
        // N->1 means the first Part-2 hit is already present in this sample.
        // Its direct resolver event may have been pre-applied above; the
        // boundary still belongs to the last hit from the preceding sample.
        const uint32_t boundaryLandedSequence = rollover
            ? lastLandedSequenceBeforeSample
            : g_recLastLandedContactSequence;
        const int boundaryDirectStep = rollover
            ? lastLandedDirectStepBeforeSample
            : g_recLastLandedDirectStep;
        const uint32_t boundaryEntitySequence = rollover
            ? lastLandedEntitySequenceBeforeSample
            : g_recLastLandedEntitySequence;
        int landedAction = -1;
        std::string landedAnchor;
        if (boundaryEntitySequence != 0) {
            if (RecEntityTraceEvent* event =
                    FindRecordedEntityContact(boundaryEntitySequence)) {
                g_recPendingComboEndEntitySequence =
                    boundaryEntitySequence;
                g_recPendingComboEndStep = -1;
                landedAction = event->afterStep;
                landedAnchor = "entitySequence=" +
                    std::to_string(g_recPendingComboEndEntitySequence);
            }
        }
        if (landedAnchor.empty()) {
            const int lastLanded = boundaryDirectStep >= 0
                ? boundaryDirectStep : LastRecordedLandedStep();
            if (lastLanded >= 0) {
                g_recPendingComboEndStep = lastLanded;
                g_recPendingComboEndEntitySequence = 0;
                landedAction = lastLanded;
                landedAnchor = "landedStep=" + std::to_string(lastLanded);
            }
        }
        if (!landedAnchor.empty()) {
            // Remember every action already authored before recovery. A setter
            // can be in startup here, so its later entity contact proves Part 2
            // without requiring a second button press after the boundary.
            const int activeStep = static_cast<int>(g_recSteps.size()) - 1;
            g_recPendingComboEndAfterAction = activeStep;
            g_recSetupSegmentOpen = true;
            g_recComboEndFrame = g_recFrame;
            if (cur == g_recLastAttackId && activeStep > landedAction) {
                CapturedStep& active = g_recSteps.back();
                const int oldBaseline = g_recComboAtStart;
                g_recComboAtStart =
                    ::Mission::SequencePolicy::RecordedActionBaselineAfterBoundary(
                        g_recComboAtStart, activeStep, landedAction,
                        active.landed);
                if (g_recComboAtStart != oldBaseline) {
                    g_recDamageAtStart = 0;   // new combo counts damage from zero
                    LogOut("[MISSION][REC] rebased pre-started Part-2 action after "
                           "sampled combo boundary", true);
                }
            }
            LogOut("[MISSION][REC] combo.end candidate after " + landedAnchor +
                   " priorSequence=" +
                   std::to_string(boundaryLandedSequence) +
                   " preRecoveryAction=" + std::to_string(activeStep) +
                   (rollover ? " kind=counter-rollover"
                             : " kind=visible-recovery"), true);
            if (rollover) {
                CommitPendingRecordedComboEnd(
                    "same-sample Part-2 counter rollover");
                g_recSetupSegmentOpen = false;
            }
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
        const bool akikoVacuumHitTransition =
            P1Char() == CHAR_ID_AKIKO && !g_recSteps.empty() &&
            g_recSteps.back().moveId == g_recPrevMove &&
            ::Mission::SequencePolicy::IsAkikoVacuumHitTransition(g_recPrevMove, cur);
        const bool rumiThrowSuccessTransition =
            P1Char() == CHAR_ID_NANASE && !g_recSteps.empty() &&
            g_recSteps.back().moveId == g_recPrevMove &&
            ::Mission::SequencePolicy::IsRumiCommandThrowSuccessTransition(
                g_recPrevMove, cur);
        const std::string p1Resource =
            CharacterSettings::GetCharacterInternalName(P1Char());
        const bool previousBelongsToLastAction = !g_recSteps.empty() &&
            (g_recSteps.back().moveId == g_recPrevMove ||
             std::find(g_recSteps.back().automaticFollowupIds.begin(),
                       g_recSteps.back().automaticFollowupIds.end(),
                        g_recPrevMove) !=
                 g_recSteps.back().automaticFollowupIds.end());
        const uint8_t transitionExpectedMask =
            ExpectedAttackMaskForKnownMove(cur);
        const bool tableKnownAutomaticPhase =
            ::Mission::SequencePolicy::ShouldFoldKnownAutomaticPhase(
                !g_recSteps.empty() && previousBelongsToLastAction &&
                ::Mission::MoveNames::IsAutomaticPhase(
                    p1Resource.c_str(), cur));
        const bool automaticPhaseTransition =
            akikoVacuumHitTransition || rumiThrowSuccessTransition ||
            tableKnownAutomaticPhase;
        if (automaticPhaseTransition) {
            // Keep the authored input step armed so the following hit/contact
            // is credited to it. The automatic phase becomes an accepted
            // playback alias instead of a fake second player input.
            auto& aliases = g_recSteps.back().automaticFollowupIds;
            if (std::find(aliases.begin(), aliases.end(), cur) == aliases.end()) {
                aliases.push_back(cur);
            }
            g_recLastAttackId = cur;
            if (useLegacyContactAttribution && contactEdge) {
                g_recSteps.back().connected = true;
                g_recLegacyContactAttributionRequired = true;
            }
            LogOut(std::string("[MISSION][REC] ") +
                   (akikoVacuumHitTransition ? "Akiko 41236 hit" :
                    rumiThrowSuccessTransition ? "Rumi 41236 success" :
                                                  "catalogued automatic/internal") +
                   " automatic phase " +
                   std::to_string(g_recPrevMove) + "->" + std::to_string(cur) +
                   " merged into one step", true);
        }
        if (!automaticPhaseTransition &&
            (cur != g_recLastAttackId || sameMoveReentered)) {
            int gapAnchor = g_recLastEventFrame;
            if (HasPendingRecordedComboEnd()) {
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
            cs.startBattleBatch = s.completedBattleUpdateBatch;
            cs.startActionOrder = ++g_recNextActionOrder;
            if (cs.startActionOrder == 0) {
                cs.startActionOrder = ++g_recNextActionOrder;
            }
            // Gap from the previous step's last event (effective frames): this is
            // the delayed-button measurement the runner gets as its allowance.
            cs.gap = g_recSteps.empty() ? 0 : (g_recFrame - gapAnchor);
            g_recLastEventFrame = g_recFrame;
            g_recSteps.push_back(cs);
            openedPlayerAction = !isMovement;
            openedPlayerActionStep = openedPlayerAction
                ? static_cast<int>(g_recSteps.size()) - 1 : -1;
            if (openedPlayerAction) {
                openedPlayerActionExpectedMask = transitionExpectedMask != 0
                    ? transitionExpectedMask
                    : ::Mission::SequencePolicy::ExpectedAttackMaskForAction(
                          cs.notation.c_str(), cs.moveId);
            }
            g_recStepCountView.store(static_cast<int>(g_recSteps.size()), std::memory_order_release);
            g_recLastAttackId = cur;
            g_recComboAtStart = s.p1Combo;
            g_recDamageAtStart = s.p1ComboDamage;
            // Persistent moveID<->command pairing for building the per-char table.
            LogOut("[MISSION][REC] step move=" + std::to_string(cur) +
                   " cmd=" + std::to_string(cs.cmdToken) +
                   " pat=" + ::Mission::MoveData::ClassName(::Mission::MoveData::GetClass(P1Char(), cur)) +
                   " note=" + cs.notation, true);
        } else if (!automaticPhaseTransition && physicalSameMoveReentry) {
            // Whiffed repeated instances intentionally collapse into one recipe
            // step, but each instance is still a real player action. Consume
            // its coherent edge so a legitimate same-ID retry is not reported
            // later as an input that never became a move.
            openedPlayerAction = !isMovement;
            openedPlayerActionStep = openedPlayerAction && !g_recSteps.empty()
                ? static_cast<int>(g_recSteps.size()) - 1 : -1;
            if (openedPlayerActionStep >= 0) {
                const CapturedStep& action = g_recSteps[
                    static_cast<std::size_t>(openedPlayerActionStep)];
                openedPlayerActionExpectedMask =
                    action.expectedAttackMask != 0
                    ? action.expectedAttackMask
                    : ::Mission::SequencePolicy::ExpectedAttackMaskForAction(
                          action.notation.c_str(), action.moveId);
            }
        } else if (useLegacyContactAttribution && !g_recSteps.empty() &&
                   s.p1Combo > g_recComboAtStart) {
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
                if (HasPendingRecordedComboEnd() &&
                    currentIndex > PendingRecordedComboEndAfterAction()) {
                    CommitPendingRecordedComboEnd("post-edge active-move hit");
                }
                g_recLastLandedDirectStep = currentIndex;
                g_recLastLandedEntitySequence = 0;
                g_recLastEventFrame = g_recFrame;
            }
            g_recAnyLanded = true;
            if (comboRose) g_recSetupSegmentOpen = false;
            g_recLegacyContactAttributionRequired = true;
        }
    } else {
        // Left the move: the next capture-move (even the same ID) is a new step.
        g_recLastAttackId = 0;
        // DELAYED LAND (projectile/summon): the combo rose while no capture-move is
        // active - attribute the hit to the most recent unlanded step (its entity
        // connected after the cast recovered), and record the observed delay so the
        // saved step gets a maxDelay window.
        if (useLegacyContactAttribution && comboRose) {
            for (int i = static_cast<int>(g_recSteps.size()) - 1; i >= 0; --i) {
                CapturedStep& candidate = g_recSteps[i];
                if (!candidate.landed) {
                    // Never attribute a post-boundary entity hit backwards into
                    // Part 1. A setter/action captured after the boundary may own
                    // it; an older candidate remains format-2 REVIEW ambiguity.
                    if (HasPendingRecordedComboEnd() &&
                        i <= PendingRecordedComboEndAfterAction()) {
                        LogOut("[MISSION][REC] delayed land ambiguous across combo.end; "
                               "raw entity trace required", true);
                        break;
                    }
                    candidate.landed = true;
                    candidate.req = static_cast<int>(::Mission::StepReq::Land);
                    candidate.maxDelay = g_recFrame - candidate.startFrame;
                    if (HasPendingRecordedComboEnd() &&
                        i > PendingRecordedComboEndAfterAction()) {
                        CommitPendingRecordedComboEnd("post-edge delayed hit");
                    }
                    g_recAnyLanded = true;
                    g_recSetupSegmentOpen = false;
                    g_recLastLandedDirectStep = i;
                    g_recLastLandedEntitySequence = 0;
                    g_recLegacyContactAttributionRequired = true;
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
    bool entityContactInBatch = false;
    for (std::size_t i = 0; i < s.contactEventCount; ++i) {
        if (s.contactEvents[i].source == ::Mission::Contact::Source::Entity) {
            entityContactInBatch = true;
            break;
        }
    }
    // The ordinary ring inventory remains 64 Hz. If a resolver transaction
    // arrives on one of the two intervening monitor passes, take one targeted
    // inventory immediately so a short-lived producer can still acquire its
    // exact slot/generation before the contact trace is appended.
    const std::size_t entityTraceBeforeRing = g_recEntityTraceCount;
    if (advanceTime || entityContactInBatch) {
        RecorderCaptureEntityRing();
        ConsumeRecordedEntityCommandOrigins(entityTraceBeforeRing,
                                            s.p1InputPollSerial,
                                            s.completedBattleUpdateBatch,
                                            lastStepBeforeSample);
    }
    const int directLearnerHits =
        RecorderCaptureContacts(s, preAppliedDirectContacts,
                                preAppliedDirectSteps,
                                lastStepBeforeSample);
    // Without the entity resolver, a combo rise not explained by an exact
    // direct event may be a projectile/summon. Preserve the take, but do not
    // certify a format-1 recipe from that ambiguity.
    if (!s.entityContactHookReady && comboRose &&
        directLearnerHits < aggregateComboRise) {
        g_recEntityAttributionRequired = true;
    }

    if (openedPlayerAction && openedPlayerActionStep >= 0 &&
        openedPlayerActionStep < static_cast<int>(g_recSteps.size()) &&
        g_recPendingAttackCount > 0) {
        // Bind this move to the newest eligible edge of its own strength.
        // A fresher wrong-button mash stays in the diagnostics journal while
        // an older buffered matching press remains available to explain the
        // action. Unknown notation falls back only to a recent causal edge.
        CapturedStep& action =
            g_recSteps[static_cast<std::size_t>(openedPlayerActionStep)];
        const uint8_t derivedExpected = action.expectedAttackMask != 0
            ? action.expectedAttackMask
            : openedPlayerActionExpectedMask != 0
                ? openedPlayerActionExpectedMask
                : ::Mission::SequencePolicy::ExpectedAttackMaskForAction(
                      action.notation.c_str(), action.moveId);
        const int chosen = SelectPendingRecorderAttack(
            s.p1InputPollSerial, derivedExpected);
        if (chosen >= 0) {
            const uint8_t observedCausalMask = static_cast<uint8_t>(
                g_recPendingAttacks[static_cast<std::size_t>(chosen)].mask &
                ::Mission::SequencePolicy::kAttackButtonMask);
            const uint8_t causalMask =
                ::Mission::SequencePolicy::BoundCausalAttackMask(
                    observedCausalMask, derivedExpected);
            if (action.expectedAttackMask == 0) {
                action.expectedAttackMask = causalMask;
            }
            LogOut("[MISSION][REC][INPUT] bound step=" +
                   std::to_string(openedPlayerActionStep) + " move=" +
                   std::to_string(action.moveId) + " expected=" +
                   DecodeInputMask(derivedExpected) + " causal=" +
                   DecodeInputMask(causalMask) + " observed=" +
                   DecodeInputMask(observedCausalMask) + " frame=" +
                   std::to_string(g_recPendingAttacks[
                       static_cast<std::size_t>(chosen)].frame) + " poll=" +
                   std::to_string(g_recPendingAttacks[
                       static_cast<std::size_t>(chosen)].serial), true);
            ErasePendingRecorderAttack(static_cast<std::size_t>(chosen));
        } else if (derivedExpected != 0) {
            LogOut("[MISSION][REC][INPUT] no matching causal edge for step=" +
                   std::to_string(openedPlayerActionStep) + " move=" +
                   std::to_string(action.moveId) + " expected=" +
                   DecodeInputMask(derivedExpected), true);
        }
    }
    g_recUncommittedAttackFrame = g_recPendingAttackCount > 0
        ? g_recPendingAttacks[g_recPendingAttackCount - 1].frame
        : -1;
}

// A NEW instance of a step's move this frame: the move-ID changed onto a match,
// or the same move-ID re-entered (frame index reset - 5A chained into 5A).
bool StepArmEdge(const ::Mission::Step& st, const Snapshot& s) {
    // Entity-only commands are armed by their exact restored entity
    // transition plus a fresh S-button edge in UpdateRunEntityLineage.  A
    // button edge by itself must never advance them.
    if (st.entityCommand.present) return false;
    if (!StepMatches(st, s.p1Move)) return false;
    constexpr uint8_t kAttackButtons = INPUT_A | INPUT_B | INPUT_C | INPUT_D;
    const uint8_t sampledAttackEdge = static_cast<uint8_t>(
        s.p1Inputs & kAttackButtons &
        static_cast<uint8_t>(~g_runPrevInputs));
    const uint8_t observedAttackEdges = static_cast<uint8_t>(
        s.p1PolledAttackEdges | sampledAttackEdge);
    const uint8_t expectedMask = st.expectedAttackMask != 0
        ? static_cast<uint8_t>(st.expectedAttackMask)
        : ExpectedAttackMaskForKnownMove(s.p1Move);
    const bool matchingAttackEdge =
        ::Mission::SequencePolicy::DeadlineAttackEdgeMatches(
            observedAttackEdges, expectedMask);
    return ::Mission::SequencePolicy::IsCausallyCredibleMoveInstanceEdge(
        s.p1Move, s.p1FrameIdx, g_runPrevMove, g_runPrevFrameIdx,
        matchingAttackEdge);
}

void ResetRunEntityCommandInput() {
    g_runPendingEntityCommandEdge = PendingRunEntityCommandEdge{};
    g_runEntityCommandActionObservedThisTick = -1;
}

const ::Mission::EntityCommandOriginPolicy::Origin*
ExpectedRunEntityCommandOrigin() {
    if (g_runStep < 0 ||
        g_runStep >= static_cast<int>(g_runMission.steps.size())) {
        return nullptr;
    }
    const ::Mission::Step& step =
        g_runMission.steps[static_cast<std::size_t>(g_runStep)];
    if (!step.entityCommand.present) return nullptr;
    return ::Mission::EntityCommandOriginPolicy::ValidateBoundCommand(
        g_runMission.player.character.c_str(), step.entityCommand.slot,
        step.entityCommand.generation, step.entityCommand.rootPattern,
        step.entityCommand.activationPattern, step.expectedAttackMask);
}

void ObserveRunEntityCommandInput(const Snapshot& s) {
    const auto* origin = ExpectedRunEntityCommandOrigin();
    if (!origin || g_runPhase == RunnerPhase::Complete ||
        g_runPhase == RunnerPhase::Dropped) {
        ResetRunEntityCommandInput();
        return;
    }
    if (g_runPendingEntityCommandEdge.action != g_runStep) {
        g_runPendingEntityCommandEdge = PendingRunEntityCommandEdge{};
    }

    uint8_t freshMask = 0;
    uint32_t freshSerial = 0;
    for (std::size_t index = 0;
         index < s.p1PolledAttackEdgeCount; ++index) {
        const auto& edge = s.p1PolledAttackEdgeEvents[index];
        if (!::Mission::SequencePolicy::DeadlineAttackEdgeMatches(
                edge.mask, origin->attackMask)) {
            continue;
        }
        if (freshSerial == 0 || edge.serial >= freshSerial) {
            freshMask = edge.mask;
            freshSerial = edge.serial;
        }
    }
    if (freshMask == 0 &&
        ::Mission::SequencePolicy::DeadlineAttackEdgeMatches(
            s.p1PolledAttackEdges, origin->attackMask)) {
        freshMask = s.p1PolledAttackEdges;
        freshSerial = s.p1PolledAttackEdgeSerial;
    }
    constexpr uint8_t kAttackButtons =
        INPUT_A | INPUT_B | INPUT_C | INPUT_D;
    const uint8_t sampledEdge = static_cast<uint8_t>(
        s.p1Inputs & kAttackButtons &
        static_cast<uint8_t>(~g_runPrevInputs));
    if (freshMask == 0 &&
        ::Mission::SequencePolicy::DeadlineAttackEdgeMatches(
            sampledEdge, origin->attackMask)) {
        freshMask = sampledEdge;
        freshSerial = s.p1InputPollSerial;
    }

    if (freshMask != 0 &&
        (!g_runPendingEntityCommandEdge.valid || freshSerial == 0 ||
         freshSerial != g_runPendingEntityCommandEdge.serial)) {
        g_runPendingEntityCommandEdge.valid = true;
        g_runPendingEntityCommandEdge.action = g_runStep;
        g_runPendingEntityCommandEdge.serial = freshSerial;
        g_runPendingEntityCommandEdge.mask = freshMask;
        g_runPendingEntityCommandEdge.age = 0;
        LogOut("[MISSION][RUN][ENTITY][COMMAND] latched " +
                   DecodeInputMask(freshMask) + " for action=" +
                   std::to_string(g_runStep) + " poll=" +
                   std::to_string(freshSerial),
               true);
    } else if (g_runPendingEntityCommandEdge.valid && !s.freezeActive) {
        ++g_runPendingEntityCommandEdge.age;
    }
}

void ResetRunContactEvidence() {
    g_runConnectSeen = false;
    g_runDirectConnectSeen = false;
    g_runDirectHits = 0;
    g_runDirectContacts = 0;
    g_runDirectDamage = 0;
    g_runDirectObservedThrough = 0;
    g_runDirectFirstSequence = 0;
    g_runDirectLastSequence = 0;
}

bool RunRequiresStrictEntityLineage() {
    return EntityScheduleRuntimeMarkerVersion(g_runMission) >= 3 &&
           (!g_runMission.entityContacts.empty() ||
            !g_runMission.entityLifecycles.empty());
}

void ClearRunEntityLineage() {
    for (RunEntitySlotLineage& slot : g_runEntitySlots) {
        slot = RunEntitySlotLineage{};
    }
    g_runEntityLineageReady = false;
    g_runEntityCursorEstablished = false;
    g_runEntityAllocationCursor = 0;
    g_runEntityProducerHistoryCount = 0;
    g_runEntitySampleSerial = 0;
}

bool AppendRunEntityProducer(
    RunEntitySlotLineage& slot,
    int slotIndex,
    ::Mission::EntitySchedulePolicy::LifecycleKind kind,
    int pattern, int priorPattern, int producerAction,
    std::string& errorOut) {
    if (g_runEntityProducerHistoryCount >=
        ::Mission::RecorderEntityTrace::kTraceEventCapacity) {
        errorOut = "entity lifecycle history exceeded the recorder trace budget";
        return false;
    }
    RunEntityLifecycleObservation producer;
    producer.generation = slot.generation;
    producer.kind = kind;
    producer.pattern = pattern;
    producer.priorPattern = priorPattern;
    producer.producerAction = producerAction;
    producer.segment = g_runEntitySegment;
    producer.sampleSerial = g_runEntitySampleSerial;
    const ::Mission::EntityLifecyclePolicy::ProducerObservation observed{
        slotIndex, slot.generation, kind, pattern, priorPattern,
        producerAction};
    producer.segment = ::Mission::EntityLifecyclePolicy::
        SegmentForPendingExactObjective(
            producer.segment, g_runMission.entityLifecycles,
            g_runEntityLifecycleStates, observed);
    slot.producers.push_back(producer);
    ++g_runEntityProducerHistoryCount;
    return true;
}

// Establish generation 1 for every entity already present in the exact
// restored frame-zero state. Empty slots begin at generation 0; each later
// dead->live transition increments that same deterministic counter.
bool InitializeRunEntityLineageBaseline(std::string& errorOut) {
    std::lock_guard<std::recursive_mutex> lock(g_runStateMx);
    errorOut.clear();
    ClearRunEntityLineage();
    if (!RunRequiresStrictEntityLineage()) return true;

    std::array<CollisionDisplay::ProjectileRingSlotProbe,
               CollisionDisplay::kProjectileRingSlotCapacity> current{};
    CollisionDisplay::ProjectileRingCursorProbe cursors{};
    if (!CollisionDisplay::ProbeProjectileRing(
            1, current.data(), current.size(), &cursors) ||
        !cursors.readable) {
        errorOut = "the restored P1 entity ring/cursors could not be read";
        return false;
    }
    using Kind = ::Mission::EntitySchedulePolicy::LifecycleKind;
    for (std::size_t index = 0; index < current.size(); ++index) {
        const auto& now = current[index];
        if (!now.readable) {
            errorOut = "the restored P1 entity ring contains an unreadable live slot";
            ClearRunEntityLineage();
            return false;
        }
        RunEntitySlotLineage& slot = g_runEntitySlots[index];
        slot.established = true;
        slot.alive = now.alive;
        slot.pattern = now.alive ? now.pattern : 0;
        slot.generation = now.alive ? 1u : 0u;
        if (now.alive && !AppendRunEntityProducer(
                slot, static_cast<int>(index), Kind::Baseline,
                now.pattern, -1, -1, errorOut)) {
            ClearRunEntityLineage();
            return false;
        }
    }
    g_runEntityCursorEstablished = true;
    g_runEntityAllocationCursor = cursors.allocationCursor;
    g_runEntityLineageReady = true;
    LogOut("[MISSION][RUN][ENTITY] strict frame-zero ring lineage established", true);
    return true;
}

bool UpdateRunEntityLineage(int producerAction, std::string& errorOut) {
    errorOut.clear();
    g_runEntityCommandActionObservedThisTick = -1;
    if (!RunRequiresStrictEntityLineage()) return true;
    if (!g_runEntityLineageReady) {
        errorOut = "the exact entity lineage baseline was not established";
        return false;
    }

    ++g_runEntitySampleSerial;
    if (g_runEntitySampleSerial == 0) ++g_runEntitySampleSerial;

    std::array<CollisionDisplay::ProjectileRingSlotProbe,
               CollisionDisplay::kProjectileRingSlotCapacity> current{};
    CollisionDisplay::ProjectileRingCursorProbe cursors{};
    if (!CollisionDisplay::ProbeProjectileRing(
            1, current.data(), current.size(), &cursors) ||
        !cursors.readable) {
        errorOut = "the P1 entity ring/cursors became unavailable";
        return false;
    }
    namespace Trace = ::Mission::RecorderEntityTrace;
    using Kind = ::Mission::EntitySchedulePolicy::LifecycleKind;
    const auto* expectedCommand = ExpectedRunEntityCommandOrigin();
    std::size_t sampledSpawnCount = 0;
    for (std::size_t index = 0; index < current.size(); ++index) {
        const auto& now = current[index];
        RunEntitySlotLineage& slot = g_runEntitySlots[index];
        if (!slot.established || !now.readable) {
            errorOut = "the P1 entity ring lost a readable slot";
            return false;
        }
        const auto transition = Trace::ClassifySampledSlotTransition(
            true, slot.alive, slot.pattern, now.alive, now.pattern);
        if (transition == Trace::SampledSlotTransition::Spawn) {
            ++sampledSpawnCount;
            ++slot.generation;
            if (slot.generation == 0) {
                errorOut = "an entity slot generation wrapped";
                return false;
            }
            if (!AppendRunEntityProducer(
                    slot, static_cast<int>(index), Kind::Spawn,
                    now.pattern, -1,
                    producerAction, errorOut)) {
                return false;
            }
        } else if (transition == Trace::SampledSlotTransition::Morph) {
            int morphProducerAction = producerAction;
            if (expectedCommand &&
                slot.pattern == expectedCommand->rootPattern &&
                now.pattern == expectedCommand->activationPattern) {
                const ::Mission::Step& commandStep =
                    g_runMission.steps[static_cast<std::size_t>(g_runStep)];
                if (!::Mission::EntityCommandOriginPolicy::
                        RuntimeTransitionMatches(
                            expectedCommand,
                            commandStep.entityCommand.slot,
                            commandStep.entityCommand.generation,
                            static_cast<int>(index),
                            static_cast<int>(slot.generation),
                            slot.pattern, now.pattern)) {
                    errorOut = "the entity-only command transitioned on a "
                               "different restored entity instance";
                    return false;
                }
                if (!g_runPendingEntityCommandEdge.valid ||
                    g_runPendingEntityCommandEdge.action != g_runStep ||
                    g_runPendingEntityCommandEdge.age >
                        ::Mission::SequencePolicy::
                            kRecorderInputCausalityWindowTicks ||
                    !::Mission::SequencePolicy::DeadlineAttackEdgeMatches(
                        g_runPendingEntityCommandEdge.mask,
                        expectedCommand->attackMask)) {
                    errorOut = "the entity-only command transition had no "
                               "fresh causal S-button edge";
                    return false;
                }
                if (g_runEntityCommandActionObservedThisTick >= 0) {
                    errorOut = "more than one entity-only command transition "
                               "claimed the same input edge";
                    return false;
                }
                morphProducerAction = g_runStep;
                g_runEntityCommandActionObservedThisTick = g_runStep;
                LogOut("[MISSION][RUN][ENTITY][COMMAND] matched action=" +
                           std::to_string(g_runStep) + " transition=" +
                           std::to_string(slot.pattern) + "->" +
                           std::to_string(now.pattern) + " slot=" +
                           std::to_string(index) + " generation=" +
                           std::to_string(slot.generation) + " poll=" +
                           std::to_string(
                               g_runPendingEntityCommandEdge.serial),
                       true);
                g_runPendingEntityCommandEdge =
                    PendingRunEntityCommandEdge{};
            }
            if (slot.generation == 0 || !AppendRunEntityProducer(
                    slot, static_cast<int>(index), Kind::Morph,
                    now.pattern, slot.pattern,
                    morphProducerAction, errorOut)) {
                if (errorOut.empty()) {
                    errorOut = "a morph was observed without a live entity generation";
                }
                return false;
            }
        }
        // Despawn preserves generation and producer history. The resolver may
        // destroy an entity in the same closed batch as its contact, and that
        // contact must still bind to the producer that was alive beforehand.
        slot.alive = now.alive;
        slot.pattern = now.alive ? now.pattern : 0;
    }
    if (expectedCommand &&
        g_runEntityCommandActionObservedThisTick >= 0 &&
        expectedCommand->contactPattern !=
            expectedCommand->activationPattern) {
        // Blitz can allocate its #454 hit child in the same endpoint sample as
        // the bound #401->#436 controller morph. Ring slot order is not causal;
        // stamp the literal curated child after the complete sample so a lower
        // numbered child slot cannot be assigned to the preceding action.
        for (RunEntitySlotLineage& slot : g_runEntitySlots) {
            if (slot.producers.empty()) continue;
            RunEntityLifecycleObservation& producer = slot.producers.back();
            if (producer.sampleSerial == g_runEntitySampleSerial &&
                producer.pattern == expectedCommand->contactPattern &&
                (producer.kind == Kind::Spawn ||
                 producer.kind == Kind::Morph)) {
                producer.producerAction =
                    g_runEntityCommandActionObservedThisTick;
            }
        }
    }
    if (expectedCommand && g_runPendingEntityCommandEdge.valid &&
        g_runPendingEntityCommandEdge.action == g_runStep &&
        g_runPendingEntityCommandEdge.age >
            ::Mission::SequencePolicy::kRecorderInputCausalityWindowTicks) {
        errorOut = "the S-button command did not produce its exact entity "
                   "transition in time";
        return false;
    }
    if (Trace::AllocationAdvancedWithoutObservedSpawn(
            g_runEntityCursorEstablished,
            g_runEntityAllocationCursor,
            cursors.allocationCursor,
            sampledSpawnCount)) {
        errorOut = "the entity allocator advanced without an observable spawn; "
                   "exact slot generation became ambiguous";
        return false;
    }
    g_runEntityCursorEstablished = true;
    g_runEntityAllocationCursor = cursors.allocationCursor;
    return true;
}

bool StrictEntityLineageMatches(
    int requiredSlot,
    int requiredGeneration,
    const std::vector<int>& requiredPatterns,
    const std::string& requiredLifecycle,
    int requiredProducerPattern,
    int requiredPriorPattern,
    int requiredProducerAction,
    const ::Mission::Contact::Event& event) {
    if (!RunRequiresStrictEntityLineage() ||
        requiredSlot < 0 ||
        requiredSlot >= static_cast<int>(g_runEntitySlots.size())) {
        return !RunRequiresStrictEntityLineage();
    }
    if (std::find(requiredPatterns.begin(), requiredPatterns.end(),
                  event.entityPattern) == requiredPatterns.end()) {
        return false;
    }
    const auto requiredKind = EntityLifecycleKind(requiredLifecycle);
    const RunEntitySlotLineage& slot = g_runEntitySlots[
        static_cast<std::size_t>(requiredSlot)];
    // The resolver journal has a slot but no generation field. The live ring
    // generation at this closed-batch sample is therefore the only safe owner.
    // If the slot was already reused, reject rather than letting an older
    // matching descriptor in history bless the new instance.
    if (slot.generation != static_cast<uint32_t>(requiredGeneration)) {
        return false;
    }
    if (slot.producers.empty()) return false;
    // The newest sampled Baseline/Spawn/Morph is authoritative for the live
    // generation. Searching farther back would let an entity morph away and
    // later return to the same numeric pattern while an obsolete producer
    // descriptor incorrectly satisfies the contact.
    const RunEntityLifecycleObservation& producer = slot.producers.back();
    const auto strictMatch =
        [&](const RunEntityLifecycleObservation& observed) {
            return ::Mission::EntitySchedulePolicy::StrictLineageMatches(
        requiredSlot, static_cast<uint32_t>(requiredGeneration),
        requiredKind, requiredProducerPattern, requiredPriorPattern,
        requiredProducerAction,
        event.entitySlot, event.entityPattern,
        requiredSlot, observed.generation,
        observed.kind, observed.pattern, observed.priorPattern,
        observed.producerAction);
        };
    if (strictMatch(producer)) return true;

    // The exact contact resolver and the ring inventory are consumed in the
    // same closed sample. Some resource-specific entity state machines can
    // advance past a short attacking PAT before the endpoint ring is read.
    // Consult only the literal resource/prior/current/contact transition table;
    // family membership is never sufficient to bridge strict lineage.
    if (g_runMission.player.character.empty() ||
        producer.sampleSerial == 0 ||
        producer.sampleSerial != g_runEntitySampleSerial ||
        producer.kind !=
            ::Mission::EntitySchedulePolicy::LifecycleKind::Morph ||
        producer.pattern <= 0 || producer.pattern > 0xFFFF ||
        producer.priorPattern <= 0 || producer.priorPattern > 0xFFFF ||
        event.entityPattern == 0) {
        return false;
    }
    using BridgeKind =
        ::Mission::EntityContactPhaseBridgePolicy::BridgeKind;
    const BridgeKind bridge =
        ::Mission::EntityContactPhaseBridgePolicy::ClassifyBridge(
            g_runMission.player.character.c_str(),
            static_cast<uint16_t>(producer.priorPattern),
            static_cast<uint16_t>(producer.pattern), event.entityPattern);
    if (bridge == BridgeKind::DirectPostContactRetirement) {
        for (std::size_t index = slot.producers.size() - 1; index-- > 0;) {
            const RunEntityLifecycleObservation& prior =
                slot.producers[index];
            if (prior.generation != producer.generation) break;
            // The nearest older lifecycle is authoritative. A different phase
            // must not be skipped merely because an even older descriptor has
            // a convenient numeric pattern.
            return prior.pattern == event.entityPattern && strictMatch(prior);
        }
        return false;
    }
    if (bridge == BridgeKind::CollapsedIntermediateContact) {
        RunEntityLifecycleObservation synthetic = producer;
        synthetic.pattern = event.entityPattern;
        synthetic.priorPattern = producer.priorPattern;
        return strictMatch(synthetic);
    }
    return false;
}

bool StrictEntityContactLineageMatches(
    const ::Mission::EntityContactRequirement& requirement,
    const ::Mission::Contact::Event& event) {
    return StrictEntityLineageMatches(
        requirement.slot, requirement.generation, requirement.patterns,
        requirement.producerLifecycle, requirement.producerPattern,
        requirement.producerPriorPattern, requirement.opensAfterAction,
        event);
}

bool StrictEntityFanoutMemberMatches(
    const ::Mission::EntityContactRequirement& requirement,
    const ::Mission::EntityContactFanoutMember& member,
    const ::Mission::Contact::Event& event) {
    return StrictEntityLineageMatches(
        member.slot, member.generation, member.patterns,
        member.producerLifecycle, member.producerPattern,
        member.producerPriorPattern, requirement.opensAfterAction,
        event);
}

void ResetRunEntityEvidence() {
    const std::size_t count = g_runMission.entityContacts.size();
    g_runEntityContactsSeen.assign(count, 0);
    g_runEntityComboHitsSeen.assign(count, 0);
    g_runEntityDamageSeen.assign(count, 0);
    g_runEntityElapsed.assign(count, 0);
    g_runEntityFirstSequence.assign(count, 0);
    g_runEntityLastSequence.assign(count, 0);
    g_runEntityLifecycleStates.assign(
        g_runMission.entityLifecycles.size(),
        ::Mission::EntityLifecyclePolicy::ObjectiveState{});
    g_runEntityLifecycleElapsed.assign(
        g_runMission.entityLifecycles.size(), 0);
    g_runEntityObservedThrough = 0;
    g_runEntitySegment = 0;
    g_runSegmentHasVariableHitCount = false;
    ClearRunEntityLineage();
    ResetRunEntityCommandInput();
}

// With the current step ARMED, is its hit requirement now met? Deliberately does
// NOT require the move to still be active - delayed projectile/summon hits land
// after the caster recovered, and the combo counter (+0x174) rises then.
bool ArmedStepSatisfied(const ::Mission::Step& st, const Snapshot& s) {
    if (st.directContact) {
        switch (st.req) {
            case ::Mission::StepReq::Move: return true;
            case ::Mission::StepReq::Hits:
                return g_runDirectHits >= st.hitsRequired;
            case ::Mission::StepReq::Connect:
                return g_runDirectConnectSeen;
            case ::Mission::StepReq::Land: default:
                return g_runDirectHits > 0;
        }
    }
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
    g_runFailedEntityRequirement.store(-1, std::memory_order_release);
    if (g_runObservationMode == RunObservationMode::DemoPresentation) {
        g_runBestTier = -1;
        LogOut("[MISSION][DEMO][RECIPE] COMPLETE totalHits=" +
                   std::to_string(totalHits) +
                   " totalDamage=" + std::to_string(totalDamage),
               true);
        return;
    }
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
    const bool demoPresentation =
        g_runObservationMode == RunObservationMode::DemoPresentation;
    g_runAttempts += ::Mission::SequencePolicy::RunnerDropAttemptDelta(
        demoPresentation);
    g_runPhase = RunnerPhase::Dropped;
    g_runStep = 0;
    g_runBaseline = 0;
    g_runDamageBaseline = 0;
    g_runGapWindow.Reset();
    g_runComboWasAlive = false;
    g_runPrevComboCount = 0;
    g_runPrevInputs = 0;
    g_runAwaitingComboEnd = false;
    g_runAwaitingComboEndAfterSequence = 0;
    g_runArmed = false;
    g_runArmedFrames = 0;
    ResetRunContactEvidence();
    ResetRunEntityEvidence();
    ResetRunScore();
    LogOut(std::string(demoPresentation
                           ? "[MISSION][DEMO][RECIPE] DIVERGED step="
                           : "[MISSION][RUN] DROP step=") +
               std::to_string(failedStep) +
               " attempts=" + std::to_string(g_runAttempts),
           true);
    // TrialMode-style retry: snap back to the mission's start state - but only
    // after a short grace so the player can SEE what failed (the drop reason
    // message and the red recipe step) instead of being yanked back instantly.
    g_runRetryDelay = ::Mission::SequencePolicy::RunnerDropRetryDelay(
        demoPresentation,
        96);   // ~0.5s at the monitor's 192Hz cadence for player attempts
    if (demoPresentation) {
        // Validation details remain in the log and the failed recipe token
        // remains red. Player-attempt coaching toasts must not survive the
        // demonstration's terminal restore into the learner's next attempt.
        DirectDrawHook::RemoveMessagesByCategory("mission_run");
    }
}

bool ObserveArmedDirectContacts(const ::Mission::Step& st, const Snapshot& s,
                                bool splitConsecutiveSameId = false,
                                uint32_t* deferredDirectSequence = nullptr) {
    if (!g_runArmed || !st.directContact) return true;
    if (!s.directContactHookReady || s.contactEventOverflow) {
        const char* reason = s.contactEventOverflow
            ? "Exact contact evidence overflowed; retry the attempt"
            : "Exact direct-contact tracking became unavailable";
        DirectDrawHook::AddMessage(reason, "mission_run", RGB(255, 160, 160),
                                   1800, 20, 96);
        LogOut(std::string("[MISSION][RUN][CONTACT] ") + reason +
               " at step=" + std::to_string(g_runStep), true);
        RunnerDrop();
        return false;
    }

    namespace Policy = ::Mission::ContactAttributionPolicy;
    for (std::size_t i = 0; i < s.contactEventCount; ++i) {
        const auto& event = s.contactEvents[i];
        if (event.sequence <= g_runDirectObservedThrough) continue;
        const bool learnerDirectLane =
            event.source == ::Mission::Contact::Source::DirectPlayer &&
            event.attacker == 1 && event.defender == 2;
        const bool belongsToNextSameIdInstance = learnerDirectLane &&
            StepMatches(st, event.attackerMove) &&
            ::Mission::ContactAttributionPolicy::
                SameIdContactBelongsToNextInstance(
                    splitConsecutiveSameId, event.attackerMove, s.p1Move,
                    event.attackerFrame, s.p1FrameIdx);
        if (learnerDirectLane &&
            (!StepMatches(st, event.attackerMove) ||
             belongsToNextSameIdInstance)) {
            // A final hit from the old action and the first hit from its cancel
            // can share one closed resolver batch. Preserve the first event we
            // do not own so AdvanceStep can hand it to the newly armed step;
            // consuming the whole batch here silently lost that next contact.
            if (detailedLogging.load(std::memory_order_relaxed)) {
                LogOut("[MISSION][RUN][CONTACT] preserving later direct event seq=" +
                           std::to_string(event.sequence) + " move=" +
                           std::to_string(event.attackerMove) +
                           (belongsToNextSameIdInstance
                                ? " (next same-ID instance)"
                                : "") +
                           " while step=" +
                           std::to_string(g_runStep) + " owns move=" +
                           std::to_string(st.moveIds.empty() ? 0
                                                             : st.moveIds.front()),
                       true);
            }
            if (deferredDirectSequence && *deferredDirectSequence == 0) {
                *deferredDirectSequence = event.sequence;
            }
            break;
        }
        g_runDirectObservedThrough = event.sequence;
        const bool matchingDirectLane =
            learnerDirectLane && StepMatches(st, event.attackerMove);
        if (matchingDirectLane && g_runDirectFirstSequence == 0) {
            g_runDirectFirstSequence = event.sequence;
        }
        if (matchingDirectLane &&
            event.result == ::Mission::Contact::Result::Unknown) {
            DirectDrawHook::AddMessage(
                "The contact could not be classified; retry the attempt",
                "mission_run", RGB(255, 160, 160), 1800, 20, 96);
            LogOut("[MISSION][RUN][CONTACT] unclassified direct contact move=" +
                   std::to_string(event.attackerMove) + " step=" +
                   std::to_string(g_runStep), true);
            RunnerDrop();
            return false;
        }
        if (matchingDirectLane && !st.contactResult.empty() &&
            !::Mission::Contact::ResultMatches(st.contactResult, event.result)) {
            DirectDrawHook::AddMessage(
                "That attack's contact result differed from the recording",
                "mission_run", RGB(255, 160, 160), 1800, 20, 96);
            LogOut("[MISSION][RUN][CONTACT] expected=" + st.contactResult +
                   " observed=" + ::Mission::Contact::ResultName(event.result) +
                   " step=" + std::to_string(g_runStep), true);
            RunnerDrop();
            return false;
        }
        if (!Policy::IsDirectLearnerContact(event) ||
            !StepMatches(st, event.attackerMove)) {
            continue;
        }
        ++g_runDirectContacts;
        g_runDirectLastSequence = event.sequence;
        const int hitContribution = Policy::DirectHitContribution(event);
        if ((st.req == ::Mission::StepReq::Land ||
             st.req == ::Mission::StepReq::Hits) && hitContribution == 0) {
            DirectDrawHook::AddMessage(
                "That attack was defended; land it on the next attempt",
                "mission_run", RGB(255, 160, 160), 1800, 20, 96);
            LogOut("[MISSION][RUN][CONTACT] non-hit result=" +
                   std::string(::Mission::Contact::ResultName(event.result)) +
                   " for land step=" + std::to_string(g_runStep), true);
            RunnerDrop();
            return false;
        }
        g_runDirectConnectSeen = true;
        g_runDirectHits += hitContribution;
        g_runDirectDamage += Policy::DirectDamageContribution(event);
        LogOut("[MISSION][RUN][CONTACT] step=" + std::to_string(g_runStep) +
               " move=" + std::to_string(event.attackerMove) + " result=" +
               ::Mission::Contact::ResultName(event.result) + " directHits=" +
               std::to_string(g_runDirectHits) + " directDamage=" +
               std::to_string(g_runDirectDamage), true);
    }
    return true;
}

// Deferred auto-retry (armed by RunnerDrop). Never while a hotswap/setup is
// still driving the match, and never under the recorder (a rollback
// mid-capture corrupts the recording; recording also unloads the runner, so
// this is defense in depth). A manual load during the grace clears the phase,
// which cancels the pending fire.
bool TickAutoRetry() {
    if (g_runObservationMode == RunObservationMode::DemoPresentation) {
        g_runRetryDelay = 0;
        return false;
    }
    if (::Mission::TutorialSession::IsActive()) { g_runRetryDelay = 0; return false; }
    if (g_runRetryDelay <= 0 || g_runPhase != RunnerPhase::Dropped) {
        if (g_runPhase != RunnerPhase::Dropped) g_runRetryDelay = 0;
        return false;
    }
    if (--g_runRetryDelay > 0) return true;
    if (g_runStateSaved && SavestateHook::IsInstalled() &&
        !g_recActive.load() &&
        GetCurrentGamePhase() == GamePhase::Match &&
        !CharacterHotswap::IsBusy() && !::Mission::Setup::IsPending()) {
        ScopedRunnerSelfLoad selfLoad;
        const bool ok = SavestateHook::TriggerLoad();
        if (ok) {
            // RunnerDrop already reset all attempt evidence. Publish Idle only
            // after the synchronous world restore, and make TickRun discard its
            // pre-load Snapshot; the next fresh sample may then arm step zero.
            g_runPhase = RunnerPhase::Idle;
            g_runPrevMove = 0;
            g_runPrevFrameIdx = 0;
            std::string lineageError;
            if (!InitializeRunEntityLineageBaseline(lineageError)) {
                LogOut("[MISSION][RUN][ENTITY] retry lineage baseline failed: " +
                       lineageError, true);
            }
            LogOut("[MISSION][RUN] auto-loaded baseline savestate for retry", true);
            // The baseline is save-gated to the active round, but stay safe
            // against older/mid-intro states: force the active round on load.
            SkipRoundIntro("retry load");
            return true;
        }
    }
    return false;
}

void DrawRunOverlay() {
    // The D3D recipe is the sole playback progress surface. Re-publishing this
    // legacy toast from the read-only demo observer would duplicate the bar
    // and put the player's try count on somebody else's demonstration.
    if (g_runObservationMode == RunObservationMode::DemoPresentation) return;
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

bool EntityRequirementSatisfied(std::size_t index) {
    if (index >= g_runMission.entityContacts.size() ||
        index >= g_runEntityContactsSeen.size()) {
        return false;
    }
    const auto& requirement = g_runMission.entityContacts[index];
    if (!requirement.fanoutMembers.empty()) {
        return ::Mission::EntitySchedulePolicy::FanoutSatisfied(
            {g_runEntityContactsSeen[index],
             g_runEntityComboHitsSeen[index]},
            requirement.minimumContactsRequired,
            requirement.minimumComboHitsRequired);
    }
    return ::Mission::EntitySchedulePolicy::Satisfied(
        {g_runEntityContactsSeen[index], g_runEntityComboHitsSeen[index]},
        requirement.contactsRequired, requirement.comboHitsRequired);
}

bool EntityLifecycleRequirementSatisfied(std::size_t index) {
    return index < g_runEntityLifecycleStates.size() &&
           g_runEntityLifecycleStates[index].satisfied;
}

bool AllEntityRequirementsSatisfied() {
    for (std::size_t i = 0; i < g_runMission.entityContacts.size(); ++i) {
        if (!EntityRequirementSatisfied(i)) return false;
    }
    for (std::size_t i = 0; i < g_runMission.entityLifecycles.size(); ++i) {
        if (!EntityLifecycleRequirementSatisfied(i)) return false;
    }
    return true;
}

bool EntityPatternMatches(const ::Mission::EntityContactRequirement& requirement,
                          int pattern) {
    return std::find(requirement.patterns.begin(), requirement.patterns.end(),
                     pattern) != requirement.patterns.end();
}

bool EntityFanoutMemberLineageMatches(
    const ::Mission::EntityContactRequirement& requirement,
    const ::Mission::Contact::Event& event) {
    return std::any_of(
        requirement.fanoutMembers.begin(),
        requirement.fanoutMembers.end(),
        [&requirement, &event](
            const ::Mission::EntityContactFanoutMember& member) {
            return StrictEntityFanoutMemberMatches(
                requirement, member, event);
        });
}

bool EntityContactMatchesRequirement(
    const ::Mission::EntityContactRequirement& requirement,
    const ::Mission::Contact::Event& event) {
    const bool resultMatches = ::Mission::Contact::ResultMatches(
        requirement.result, event.result);
    if (requirement.fanoutMembers.empty()) {
        return resultMatches &&
               EntityPatternMatches(requirement, event.entityPattern) &&
               StrictEntityContactLineageMatches(requirement, event);
    }
    return ::Mission::EntitySchedulePolicy::FanoutMemberContactMatches(
        resultMatches,
        EntityFanoutMemberLineageMatches(requirement, event));
}

int RunStartedThroughAction(const Snapshot& s) {
    const int total = static_cast<int>(g_runMission.steps.size());
    if (g_runStep >= total) return total - 1;
    int startedThrough = g_runStep - 1;
    if (g_runStep >= 0 && g_runStep < total &&
        (g_runArmed ||
         StepArmEdge(g_runMission.steps[static_cast<std::size_t>(g_runStep)], s))) {
        startedThrough = g_runStep;
    }
    return startedThrough;
}

int FirstUnfinishedEntityRequirement() {
    for (std::size_t i = 0; i < g_runMission.entityContacts.size(); ++i) {
        if (!EntityRequirementSatisfied(i)) return static_cast<int>(i);
    }
    return -1;
}

bool DropForEntitySchedule(const std::string& reason,
                           int failedRequirement = -1) {
    if (failedRequirement < 0) {
        failedRequirement = FirstUnfinishedEntityRequirement();
    }
    g_runFailedEntityRequirement.store(failedRequirement,
                                       std::memory_order_release);
    DirectDrawHook::AddMessage(reason.c_str(), "mission_run",
                               RGB(255, 160, 160), 1900, 20, 96);
    LogOut("[MISSION][RUN][ENTITY] " + reason + " step=" +
           std::to_string(g_runStep), true);
    RunnerDrop();
    return false;
}

bool DropForEntityLifecycle(const std::string& reason,
                            std::size_t requirementIndex) {
    const int ownerStep = requirementIndex <
            g_runMission.entityLifecycles.size()
        ? g_runMission.entityLifecycles[requirementIndex].opensAfterAction
        : -1;
    DirectDrawHook::AddMessage(reason.c_str(), "mission_run",
                               RGB(255, 160, 160), 1900, 20, 96);
    LogOut("[MISSION][RUN][ENTITY][LIFECYCLE] " + reason +
               " objective=" + std::to_string(requirementIndex) +
               " step=" + std::to_string(g_runStep), true);
    RunnerDrop();
    if (ownerStep >= 0 &&
        ownerStep < static_cast<int>(g_runMission.steps.size())) {
        g_runFailedStep.store(ownerStep, std::memory_order_release);
    }
    return false;
}

bool ObserveRunEntityLifecycles(const Snapshot& s, int startedThrough) {
    namespace Lifecycle = ::Mission::EntityLifecyclePolicy;
    for (std::size_t requirementIndex = 0;
         requirementIndex < g_runMission.entityLifecycles.size();
         ++requirementIndex) {
        if (EntityLifecycleRequirementSatisfied(requirementIndex)) continue;
        const auto& requirement =
            g_runMission.entityLifecycles[requirementIndex];

        if (g_runEntitySegment > requirement.segment) {
            return DropForEntityLifecycle(
                "The recorded projectile setup was missing before the next combo part",
                requirementIndex);
        }
        if (g_runEntitySegment < requirement.segment ||
            startedThrough < requirement.opensAfterAction) {
            continue;
        }

        if (requirement.slot >= 0 &&
            requirement.slot < static_cast<int>(g_runEntitySlots.size())) {
            RunEntitySlotLineage& slot = g_runEntitySlots[
                static_cast<std::size_t>(requirement.slot)];
            for (RunEntityLifecycleObservation& producer : slot.producers) {
                if (producer.lifecycleClaimed ||
                    producer.segment != requirement.segment) {
                    continue;
                }
                const Lifecycle::ProducerObservation observed{
                    requirement.slot, producer.generation, producer.kind,
                    producer.pattern, producer.priorPattern,
                    producer.producerAction};
                if (Lifecycle::TrySatisfyOnce(
                        g_runEntityLifecycleStates[requirementIndex],
                        requirement, observed)) {
                    producer.lifecycleClaimed = true;
                    LogOut("[MISSION][RUN][ENTITY][LIFECYCLE] objective=" +
                               std::to_string(requirementIndex) +
                               " satisfied " + requirement.lifecycle + ":" +
                               std::to_string(requirement.priorPattern) + "->" +
                               std::to_string(requirement.pattern) +
                               " slot=" + std::to_string(requirement.slot) +
                               " generation=" +
                               std::to_string(requirement.generation) +
                               " action=" +
                               std::to_string(requirement.opensAfterAction),
                           true);
                    break;
                }
            }
        }
        if (EntityLifecycleRequirementSatisfied(requirementIndex)) continue;

        // Once a later authored action has begun, a future ring transition
        // cannot carry the recorded action identity. Fail at the setup move,
        // rather than allowing a later lookalike entity to repair the route.
        if (startedThrough > requirement.opensAfterAction) {
            return DropForEntityLifecycle(
                "The next action started before the recorded projectile setup appeared",
                requirementIndex);
        }
        if (!s.freezeActive &&
            ++g_runEntityLifecycleElapsed[requirementIndex] >
                requirement.maxDelay) {
            return DropForEntityLifecycle(
                "The recorded projectile setup did not appear in time",
                requirementIndex);
        }
    }
    return true;
}

bool EntityScheduleAllowsActionStart(int stepIndex) {
    for (std::size_t i = 0; i < g_runMission.entityContacts.size(); ++i) {
        const auto& requirement = g_runMission.entityContacts[i];
        if (requirement.dueBeforeStep != stepIndex ||
            requirement.dueBeforeStepContact != 0) {
            continue;
        }
        if (!EntityRequirementSatisfied(i)) {
            return DropForEntitySchedule(
                requirement.fanoutMembers.empty()
                    ? "The next action started before the recorded projectile sequence finished"
                    : "The next action started before any projectile from the recorded move hit",
                static_cast<int>(i));
        }
    }
    return true;
}

bool EntityScheduleAllowsDirectContact(int stepIndex,
                                       int contactOrdinal,
                                       uint32_t contactSequence) {
    for (std::size_t i = 0; i < g_runMission.entityContacts.size(); ++i) {
        const auto& requirement = g_runMission.entityContacts[i];
        if (!::Mission::EntitySchedulePolicy::DueDirectContactReached(
                stepIndex, contactOrdinal, requirement.dueBeforeStep,
                requirement.dueBeforeStepContact)) {
            continue;
        }
        if (!EntityRequirementSatisfied(i)) {
            return DropForEntitySchedule(
                requirement.fanoutMembers.empty()
                    ? "A direct hit arrived before the recorded projectile sequence finished"
                    : "A direct hit arrived before any projectile from the recorded move hit",
                static_cast<int>(i));
        }
        if (!::Mission::EntitySchedulePolicy::EntityPrecedesDirect(
                g_runEntityLastSequence[i], contactSequence)) {
            return DropForEntitySchedule(
                "Projectile and direct-contact order differed from the recording",
                static_cast<int>(i));
        }
    }
    return true;
}

bool SnapshotHasExactAwaitedComboBoundary(const Snapshot& s) {
    if (!g_runAwaitingComboEnd) return false;
    for (std::size_t eventIndex = 0;
         eventIndex < s.contactEventCount; ++eventIndex) {
        const auto& event = s.contactEvents[eventIndex];
        // Snapshot contact batches are globally drained exactly once before
        // TickRun. The entity observer may already have consumed this same
        // batch locally, but that must not hide an exact Part-2 baseline from
        // the linear combo-boundary state machine later in this tick. The
        // owning contact sequence is the authoritative replay guard.
        if (event.sequence <= g_runAwaitingComboEndAfterSequence) {
            continue;
        }
        const bool committedContact = event.attacker == 1 && event.defender == 2 &&
            ::Mission::ContactAttributionPolicy::IsCommittedResult(event.result);
        if (::Mission::EntitySchedulePolicy::
                ExactOrderedContactConsumesAwaitedBoundary(
                    true, committedContact,
                    event.comboBefore, event.comboAfter)) {
            return true;
        }
    }
    return false;
}

// One monitor sample can contain the last resolver transaction of the armed
// action and the first transaction of its authored successor.  The direct
// lane consumes its own evidence before the entity lane runs, so preserve the
// counters from the start of the closed batch and the exact hand-off sequence.
// ObserveRunEntityContacts can then replay the journal once, in order, without
// double-counting the direct events already reflected in g_runDirect*.
struct RunDirectBatchContext {
    int initialStep = -1;
    int initialHits = 0;
    int initialContacts = 0;
    bool initialConnect = false;
    bool initialStepSatisfied = false;
    bool initialStepWillFinalize = false;
    bool initialVariableHitCount = false;
    bool initialFlexibleFinalizedByNextAction = false;
    bool initialStepRequiresComboEnd = false;
    uint32_t comboEndOwnerSequence = 0;
    uint32_t nextStepSequence = 0;
    int nextStep = -1;

    bool HasStartedNextStep() const {
        return nextStep >= 0;
    }

    bool HasOrderedNextStep() const {
        return nextStepSequence != 0 && HasStartedNextStep();
    }
};

bool ObserveRunEntityContacts(const Snapshot& s,
                              bool exactOrderedBoundary,
                              const RunDirectBatchContext& directBatch) {
    if (g_runMission.entityContacts.empty() &&
        g_runMission.entityLifecycles.empty()) {
        return true;
    }
    if (!s.entityContactHookReady || s.contactEventOverflow) {
        return DropForEntitySchedule(
            s.contactEventOverflow
                ? "Exact entity-contact evidence overflowed; retry the attempt"
                : "Exact entity-contact tracking became unavailable");
    }

    const int totalSteps = static_cast<int>(g_runMission.steps.size());
    const bool actionStarting = !g_runArmed && g_runStep >= 0 &&
        g_runStep < totalSteps &&
        StepArmEdge(g_runMission.steps[static_cast<std::size_t>(g_runStep)], s);
    // Enforce an action-start due barrier before consuming any later resolver
    // record from this same closed Battle batch. Otherwise the later setter's
    // projectile could incorrectly repair the missed earlier run first.
    if (actionStarting && !EntityScheduleAllowsActionStart(g_runStep)) {
        return false;
    }
    if (directBatch.HasStartedNextStep() &&
        !EntityScheduleAllowsActionStart(directBatch.nextStep)) {
        return false;
    }

    int startedThrough = RunStartedThroughAction(s);
    if (directBatch.HasStartedNextStep()) {
        startedThrough = (std::max)(startedThrough, directBatch.nextStep);
    }
    if (RunRequiresStrictEntityLineage()) {
        std::string lineageError;
        // A value-only/manual reset has no safe frame-zero identity. Permit a
        // lazy baseline only while no authored action or resolver event has
        // begun; once play started, missing lineage is irrecoverable for this
        // attempt and must fail closed.
        if (!g_runEntityLineageReady && startedThrough < 0 &&
            s.contactEventCount == 0) {
            (void)InitializeRunEntityLineageBaseline(lineageError);
        }
        if (!g_runEntityLineageReady ||
            !UpdateRunEntityLineage(startedThrough, lineageError)) {
            return DropForEntitySchedule(
                "Exact projectile lineage could not be verified: " +
                (lineageError.empty() ? std::string("unknown ring state")
                                      : lineageError));
        }
        if (g_runEntityCommandActionObservedThisTick >= 0) {
            if (!EntityScheduleAllowsActionStart(
                    g_runEntityCommandActionObservedThisTick)) {
                return false;
            }
            startedThrough = (std::max)(
                startedThrough,
                g_runEntityCommandActionObservedThisTick);
        }
        if (!ObserveRunEntityLifecycles(s, startedThrough)) return false;
    }
    int orderedDirectStep = directBatch.initialStep >= 0
        ? directBatch.initialStep
        : g_runStep;
    int satisfiedThrough = orderedDirectStep - 1;
    int effectiveSegment = g_runEntitySegment;
    bool segmentAdvancePending = g_runAwaitingComboEnd ||
        (directBatch.initialStepWillFinalize &&
         directBatch.initialStepRequiresComboEnd);
    uint32_t segmentAdvanceAfterSequence = g_runAwaitingComboEnd
        ? g_runAwaitingComboEndAfterSequence
        : directBatch.comboEndOwnerSequence;
    bool sampledComboEndedEdge = false;
    int orderedDirectHits = directBatch.initialHits;
    int orderedDirectContacts = directBatch.initialContacts;
    bool orderedDirectConnect = directBatch.initialConnect;
    if (directBatch.initialStepSatisfied ||
        directBatch.initialFlexibleFinalizedByNextAction) {
        // The successor's causally verified action-start is already structural
        // proof that a variable multi-hit finished. Its first collision may be
        // later in this same batch; keeping the old whole-step gate closed until
        // that collision would wrongly reject a projectile which activates in
        // between the action start and the direct hit.
        satisfiedThrough = (std::max)(satisfiedThrough, orderedDirectStep);
    }
    if (g_runPhase == RunnerPhase::InProgress) {
        const bool comboAlive = s.p2InStun || s.p1Combo > 0;
        const auto boundary = ::Mission::SequencePolicy::DetectSampledComboBoundary(
            g_runComboWasAlive, comboAlive, g_runPrevComboCount, s.p1Combo);
        sampledComboEndedEdge =
            boundary != ::Mission::SequencePolicy::SampledComboBoundary::None;
        if (exactOrderedBoundary ||
            ::Mission::SequencePolicy::DecideComboEnd(
                sampledComboEndedEdge,
                g_runAwaitingComboEnd) ==
                ::Mission::SequencePolicy::ComboEndDecision::ConsumeRequired) {
            // TickRun consumes this boundary later in the same sample. Advance
            // the local segment at the first ordered committed contact whose
            // combo baseline is already zero; an earlier late Part-1 entity in
            // the same journal batch must remain in the old segment.
            segmentAdvancePending = true;
        }
    }
    for (std::size_t eventIndex = 0;
         eventIndex < s.contactEventCount; ++eventIndex) {
        const auto& event = s.contactEvents[eventIndex];
        if (event.sequence <= g_runEntityObservedThrough) continue;
        g_runEntityObservedThrough = event.sequence;

        const int eventDirectStep =
            ::Mission::EntitySchedulePolicy::DirectStepForOrderedEvent(
                directBatch.initialStep, directBatch.nextStep,
                directBatch.nextStepSequence, event.sequence);
        if (directBatch.HasOrderedNextStep() &&
            orderedDirectStep != eventDirectStep) {
            if (directBatch.initialStepWillFinalize) {
                satisfiedThrough = (std::max)(
                    satisfiedThrough, directBatch.initialStep);
            }
            orderedDirectStep = eventDirectStep;
            orderedDirectHits = 0;
            orderedDirectContacts = 0;
            orderedDirectConnect = false;
        }

        const bool committedContact = event.attacker == 1 && event.defender == 2 &&
            event.result != ::Mission::Contact::Result::None &&
            event.result != ::Mission::Contact::Result::Unknown;
        const bool boundaryContactIsAfterOwner =
            event.sequence > segmentAdvanceAfterSequence;
        const int orderedSegment =
            ::Mission::EntitySchedulePolicy::SegmentForOrderedContact(
                effectiveSegment,
                segmentAdvancePending && boundaryContactIsAfterOwner,
                committedContact, event.comboBefore, event.comboAfter);
        if (orderedSegment != effectiveSegment) {
            effectiveSegment = orderedSegment;
            segmentAdvancePending = false;
        }

        // The collision journal is ordered more precisely than the sampled
        // runner state. If a direct barrier and the following projectile hit
        // both land between two monitor passes, open that barrier only for the
        // later journal entries in this same batch.
        if (event.source == ::Mission::Contact::Source::DirectPlayer &&
            event.attacker == 1 && event.defender == 2 &&
            event.result != ::Mission::Contact::Result::None &&
            event.result != ::Mission::Contact::Result::Unknown &&
            orderedDirectStep >= 0 &&
            orderedDirectStep < static_cast<int>(g_runMission.steps.size()) &&
            StepMatches(g_runMission.steps[static_cast<std::size_t>(orderedDirectStep)],
                        event.attackerMove)) {
            const int nextDirectContact = orderedDirectContacts + 1;
            if (!EntityScheduleAllowsDirectContact(
                    orderedDirectStep, nextDirectContact, event.sequence)) {
                return false;
            }
            orderedDirectContacts = nextDirectContact;
            const auto& directStep =
                g_runMission.steps[static_cast<std::size_t>(orderedDirectStep)];
            startedThrough = (std::max)(startedThrough, orderedDirectStep);
            orderedDirectConnect = true;
            orderedDirectHits +=
                ::Mission::ContactAttributionPolicy::DirectHitContribution(event);
            const bool typedResultMatches = directStep.contactResult.empty() ||
                ::Mission::Contact::ResultMatches(directStep.contactResult,
                                                  event.result);
            const int requiredHits = directStep.req == ::Mission::StepReq::Hits
                ? directStep.hitsRequired : 1;
            const bool barrierSatisfied =
                ::Mission::EntitySchedulePolicy::DirectBarrierSatisfied(
                    directStep.req == ::Mission::StepReq::Connect,
                    requiredHits, typedResultMatches,
                    orderedDirectConnect, orderedDirectHits);
            if (barrierSatisfied) {
                satisfiedThrough = (std::max)(satisfiedThrough,
                                              orderedDirectStep);
            }
            continue;
        }
        if (event.source != ::Mission::Contact::Source::Entity ||
            event.attacker != 1 || event.defender != 2) {
            continue;
        }
        if (event.result == ::Mission::Contact::Result::Unknown) {
            return DropForEntitySchedule(
                "A projectile contact could not be classified");
        }

        std::size_t firstUnfinished = 0;
        while (firstUnfinished < g_runMission.entityContacts.size() &&
               EntityRequirementSatisfied(firstUnfinished)) {
            ++firstUnfinished;
        }
        const auto gatesOpenFor = [&](std::size_t requirementIndex) {
            const auto& candidate =
                g_runMission.entityContacts[requirementIndex];
            return ::Mission::EntitySchedulePolicy::OrderedGatesOpen(
                startedThrough, satisfiedThrough, orderedDirectStep,
                orderedDirectContacts, effectiveSegment,
                candidate.opensAfterAction, candidate.contactAfterAction,
                candidate.afterStep, candidate.afterStepContact,
                candidate.segment);
        };

        std::size_t current = g_runMission.entityContacts.size();
        if (firstUnfinished < g_runMission.entityContacts.size() &&
            gatesOpenFor(firstUnfinished) &&
            EntityContactMatchesRequirement(
                g_runMission.entityContacts[firstUnfinished], event)) {
            current = firstUnfinished;
        } else {
            // Reaching a fanout's one-hit minimum must not turn its remaining
            // exact siblings into "extra projectiles". Search only already-
            // satisfied earlier episodes, and only while their recorded due
            // window remains open. Future obligations stay strictly ordered.
            for (std::size_t i = 0; i < firstUnfinished; ++i) {
                const auto& candidate = g_runMission.entityContacts[i];
                if (candidate.fanoutMembers.empty()) {
                    continue;
                }
                const bool dueWindowOpen =
                    ::Mission::EntitySchedulePolicy::
                        OptionalFanoutWindowOpen(
                            startedThrough, satisfiedThrough,
                            orderedDirectStep, orderedDirectContacts,
                            candidate.dueBeforeStep,
                            candidate.dueBeforeStepContact);
                const bool resultMatches =
                    ::Mission::Contact::ResultMatches(
                        candidate.result, event.result);
                const bool memberLineageMatches =
                    EntityFanoutMemberLineageMatches(candidate, event);
                if (!::Mission::EntitySchedulePolicy::
                        OptionalFanoutContactCanBeConsumed(
                            gatesOpenFor(i), dueWindowOpen, resultMatches,
                            memberLineageMatches)) {
                    continue;
                }
                current = i;
                break;
            }
        }
        if (current >= g_runMission.entityContacts.size()) {
            return DropForEntitySchedule(
                RunRequiresStrictEntityLineage()
                    ? "Projectile producer lineage, order, or result differed from the recording"
                    : "Projectile contact order or result differed from the recording",
                firstUnfinished < g_runMission.entityContacts.size()
                    ? static_cast<int>(firstUnfinished)
                    : (g_runMission.entityContacts.empty()
                        ? -1
                        : static_cast<int>(
                              g_runMission.entityContacts.size() - 1)));
        }
        const auto& requirement = g_runMission.entityContacts[current];

        const bool satisfiedBefore = EntityRequirementSatisfied(current);
        if (g_runEntityFirstSequence[current] == 0) {
            g_runEntityFirstSequence[current] = event.sequence;
        }
        g_runEntityLastSequence[current] = event.sequence;
        ++g_runEntityContactsSeen[current];
        g_runEntityComboHitsSeen[current] +=
            ::Mission::EntitySchedulePolicy::OrderedHitContribution(
                ::Mission::ContactAttributionPolicy::IsHitResult(event.result),
                event.comboBefore, event.comboAfter);
        g_runEntityDamageSeen[current] +=
            (std::max)(0, event.defenderHpBefore - event.defenderHpAfter);
        const bool flexibleFanout = !requirement.fanoutMembers.empty();
        const ::Mission::EntitySchedulePolicy::Progress entityProgress{
            g_runEntityContactsSeen[current],
            g_runEntityComboHitsSeen[current]};
        const bool aggregateRejected = flexibleFanout
            ? !::Mission::EntitySchedulePolicy::FanoutAggregateAllowed(
                  entityProgress, requirement.contactsRequired,
                  requirement.comboHitsRequired)
            : ::Mission::EntitySchedulePolicy::Overshot(
                  entityProgress, requirement.contactsRequired,
                  requirement.comboHitsRequired);
        if (aggregateRejected) {
            return DropForEntitySchedule(
                "A projectile produced more contacts or hits than the recording",
                static_cast<int>(current));
        }
        if (flexibleFanout) {
            // The number and order of Shiori's randomized siblings which hit
            // changes proration for every later resolver transaction. Exact
            // identity/result/order still grade; aggregate damage cannot.
            g_runSegmentHasVariableHitCount = true;
        }
        const bool newlySatisfied =
            !satisfiedBefore && EntityRequirementSatisfied(current);
        const bool variableProrationApplies =
            g_runSegmentHasVariableHitCount ||
            (directBatch.initialVariableHitCount &&
             effectiveSegment == g_runEntitySegment);
        if (newlySatisfied && !flexibleFanout &&
            !variableProrationApplies &&
            ::Mission::EntitySchedulePolicy::DamageDiverged(
                requirement.damage, g_runEntityDamageSeen[current])) {
            char message[144];
            _snprintf_s(message, sizeof(message), _TRUNCATE,
                        "Projectile damage differed: +%d (recorded +%d)",
                        g_runEntityDamageSeen[current], requirement.damage);
            return DropForEntitySchedule(message, static_cast<int>(current));
        }
        if (newlySatisfied && requirement.comboEndAfter) {
            // The collision-owned obligation, rather than a linear player
            // action, owns this recovery edge.  Arm it immediately so TickRun
            // can consume a boundary already visible in this same snapshot.
            g_runAwaitingComboEnd = true;
            g_runAwaitingComboEndAfterSequence = event.sequence;
            // A later contact in this same closed batch may already be the first
            // contact of Part 2 even when the sampled combo counter never showed
            // recovery. Keep the exact owner sequence so the owning projectile
            // contact itself cannot consume its own boundary.
            segmentAdvancePending = true;
            segmentAdvanceAfterSequence = event.sequence;
            LogOut("[MISSION][RUN][ENTITY] waiting for required combo.end after obligation=" +
                   std::to_string(current), true);
        }
        LogOut("[MISSION][RUN][ENTITY] obligation=" +
               std::to_string(current) + " pattern=" +
               std::to_string(event.entityPattern) + " result=" +
               ::Mission::Contact::ResultName(event.result) + " contacts=" +
               std::to_string(g_runEntityContactsSeen[current]) + "/" +
               std::to_string(flexibleFanout
                   ? requirement.minimumContactsRequired
                   : requirement.contactsRequired) +
               (flexibleFanout
                    ? " (flexible siblings; recorded=" +
                          std::to_string(requirement.contactsRequired) + ")"
                    : std::string()),
               true);
    }

    // Only the earliest unfinished run owns the schedule clock. Later runs may
    // be concurrently armed by actions but cannot consume contacts out of order.
    std::size_t current = 0;
    while (current < g_runMission.entityContacts.size() &&
           EntityRequirementSatisfied(current)) {
        ++current;
    }
    if (current < g_runMission.entityContacts.size()) {
        const auto& requirement = g_runMission.entityContacts[current];
        const bool gatesOpen = ::Mission::EntitySchedulePolicy::OrderedGatesOpen(
            startedThrough, satisfiedThrough, orderedDirectStep,
            orderedDirectContacts, effectiveSegment,
            requirement.opensAfterAction, requirement.contactAfterAction,
            requirement.afterStep,
            requirement.afterStepContact, requirement.segment);
        if (gatesOpen && !s.freezeActive &&
            ++g_runEntityElapsed[current] > requirement.maxDelay) {
            return DropForEntitySchedule(
                requirement.fanoutMembers.empty()
                    ? "A recorded projectile contact did not arrive in time"
                    : "No projectile from the recorded move hit in time");
        }
    }
    return true;
}

bool EntityScheduleAllowsDirectStep(int stepIndex) {
    for (std::size_t i = 0; i < g_runMission.entityContacts.size(); ++i) {
        const auto& requirement = g_runMission.entityContacts[i];
        if (requirement.dueBeforeStep != stepIndex) continue;
        // v2 action-start and contact-ordinal barriers are enforced at their
        // exact ordered moment. Omitted/-1 is the v1 whole-step contract.
        if (requirement.dueBeforeStepContact >= 0) continue;
        if (!EntityRequirementSatisfied(i)) {
            return DropForEntitySchedule(
                requirement.fanoutMembers.empty()
                    ? "The next action completed before the recorded projectile sequence finished"
                    : "The next action completed before any projectile from the recorded move hit",
                static_cast<int>(i));
        }
        if (!::Mission::EntitySchedulePolicy::EntityPrecedesDirect(
                g_runEntityLastSequence[i], g_runDirectFirstSequence)) {
            return DropForEntitySchedule(
                "Projectile and direct-hit order differed from the recording",
                static_cast<int>(i));
        }
    }
    return true;
}

bool PendingEntityScheduleOwnsWait(const Snapshot& s) {
    const int total = static_cast<int>(g_runMission.steps.size());
    for (std::size_t i = 0; i < g_runMission.entityContacts.size(); ++i) {
        if (EntityRequirementSatisfied(i)) continue;
        const auto& requirement = g_runMission.entityContacts[i];
        const bool dueAtCurrentAction = requirement.dueBeforeStep == g_runStep;
        const bool terminal = requirement.dueBeforeStep < 0 && g_runStep >= total;
        if (!dueAtCurrentAction && !terminal) return false;
        return ::Mission::EntitySchedulePolicy::OrderedGatesOpen(
            RunStartedThroughAction(s), g_runStep - 1, g_runStep,
            g_runDirectContacts, g_runEntitySegment,
            requirement.opensAfterAction, requirement.contactAfterAction,
            requirement.afterStep,
            requirement.afterStepContact, requirement.segment);
    }
    if (g_runStep >= total) {
        for (std::size_t i = 0; i < g_runMission.entityLifecycles.size(); ++i) {
            if (EntityLifecycleRequirementSatisfied(i)) continue;
            const auto& requirement = g_runMission.entityLifecycles[i];
            return RunStartedThroughAction(s) >= requirement.opensAfterAction &&
                   g_runEntitySegment == requirement.segment;
        }
    }
    return false;
}

// Per-step damage validation at satisfaction time. Hit-variant mechanics
// (Mizuka clean hits, Akiko rekka crits) keep the move ID but change the
// step's damage by thousands, so the rest of the recorded combo can't
// reproduce; ordinary variance stays inside the policy tolerance (500).
// Returns false after dropping the run with the real reason.
bool StepDamageAcceptable(const ::Mission::Step& st, const Snapshot& s, int stepIdx) {
    if (g_runSegmentHasVariableHitCount) {
        LogOut("[MISSION][RUN] skipped proration-dependent damage fingerprint at step " +
                   std::to_string(stepIdx) +
                   " after an accepted variable hit count",
               true);
        return true;
    }
    const int observed = st.directContact
        ? g_runDirectDamage
        : s.p1ComboDamage - g_runDamageBaseline;
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
    const uint32_t priorEvidenceThrough = g_runDirectObservedThrough;
    const uint32_t comboEndOwnerSequence = g_runDirectLastSequence;
    const bool requireComboEnd = g_runStep >= 0 && g_runStep < total &&
        g_runMission.steps[g_runStep].comboEndAfter;
    const bool completedEntityCommand =
        g_runStep >= 0 && g_runStep < total &&
        g_runMission.steps[static_cast<std::size_t>(g_runStep)]
            .entityCommand.present;
    g_runStep++;
    g_runBaseline = s.p1Combo;
    g_runDamageBaseline = s.p1ComboDamage;
    g_runGapWindow.Reset();
    g_runArmed = false;
    g_runArmedFrames = 0;
    ResetRunContactEvidence();
    if (requireComboEnd) {
        // Completion/progression retains a separately observed recovery edge.
        // Setup/meaty startup may begin early, but a hit cannot satisfy Part 2
        // until the boundary is consumed.
        g_runAwaitingComboEnd = true;
        g_runAwaitingComboEndAfterSequence = comboEndOwnerSequence;
        LogOut("[MISSION][RUN] waiting for required combo.end after step=" +
               std::to_string(g_runStep - 1), true);
        return;
    }
    if (g_runStep >= total) {
        if (!g_runAwaitingComboEnd && AllEntityRequirementsSatisfied()) {
            RunnerComplete();
        }
        return;
    }
    // Delayed-hit overlap: the player may already be doing the NEXT step's move
    // while the previous step's projectile was still in flight. Its arm edge has
    // passed, so arm it now if it is the currently active move.
    const ::Mission::Step& next = g_runMission.steps[g_runStep];
    if (!completedEntityCommand && StepMatches(next, s.p1Move) &&
        IsComboStepMove(s.p1Move)) {
        if (!EntityScheduleAllowsActionStart(g_runStep)) return;
        g_runArmed = true;
        g_runArmedFrames = 0;
        ResetRunContactEvidence();
        // A direct step already inspected this snapshot's complete journal
        // batch. Never replay those same transactions into an overlapping next
        // step (especially a same-ID chain).
        g_runDirectObservedThrough = priorEvidenceThrough;
        g_runBaseline = s.p1Combo;
        g_runDamageBaseline = s.p1ComboDamage;
        (void)ObserveArmedDirectContacts(next, s);
    }
}

bool CurrentFlexibleMultiHitHasEvidence() {
    if (!g_runArmed || g_runStep < 0 ||
        g_runStep >= static_cast<int>(g_runMission.steps.size())) {
        return false;
    }
    const auto& step = g_runMission.steps[static_cast<std::size_t>(g_runStep)];
    return step.allowPartialHits && step.directContact && g_runDirectHits > 0;
}

bool SnapshotContainsExactComboRestartAfter(const Snapshot& s,
                                             uint32_t afterSequence) {
    for (std::size_t i = 0; i < s.contactEventCount; ++i) {
        const auto& event = s.contactEvents[i];
        if (event.sequence <= afterSequence ||
            event.attacker != 1 || event.defender != 2 ||
            !::Mission::ContactAttributionPolicy::IsCommittedResult(
                event.result)) {
            continue;
        }
        if (::Mission::SequencePolicy::ContactBeginsNewComboBaseline(
                event.comboBefore, event.comboAfter)) {
            return true;
        }
    }
    return false;
}

// Accept a recorder-flexible multi-hit only when its continuation proves that
// omitted contacts did not invalidate the route. Exact authored hit counts
// never call this path. Damage is intentionally not compared against the full
// recorded delta when fewer contacts landed.
bool FinalizeCurrentFlexibleMultiHit(const Snapshot& s, int total,
                                     const char* reason) {
    if (!CurrentFlexibleMultiHitHasEvidence()) return false;
    const int stepIndex = g_runStep;
    const auto& step = g_runMission.steps[static_cast<std::size_t>(stepIndex)];
    if (!EntityScheduleAllowsDirectStep(stepIndex)) return false;
    LogOut("[MISSION][RUN] accepted variable multi-hit step=" +
               std::to_string(stepIndex) + " contacts=" +
               std::to_string(g_runDirectHits) + "/" +
               std::to_string(step.hitsRequired) + " proof=" +
               (reason ? reason : "continuation"),
           true);
    if (g_runDirectHits < step.hitsRequired) {
        g_runSegmentHasVariableHitCount = true;
    }
    AdvanceStep(s, total);
    return g_runPhase != RunnerPhase::Dropped;
}

int NextExpectedActionIndexForLiveMove(const Snapshot& s, int total) {
    int nextIndex = g_runStep + 1;
    while (nextIndex < total &&
           g_runMission.steps[static_cast<std::size_t>(nextIndex)].optional &&
           !StepMatches(g_runMission.steps[static_cast<std::size_t>(nextIndex)],
                        s.p1Move)) {
        ++nextIndex;
    }
    return nextIndex;
}

bool TryArmOpeningStep(const Snapshot& s, int total) {
    if ((g_runPhase != RunnerPhase::Idle &&
         g_runPhase != RunnerPhase::Dropped) ||
        g_runArmed || total <= 0) {
        return false;
    }
    const ::Mission::Step& opener = g_runMission.steps.front();
    if (!StepArmEdge(opener, s)) return false;
    if (!EntityScheduleAllowsActionStart(0)) return false;

    g_runArmed = true;
    g_runArmedFrames = 0;
    ResetRunContactEvidence();
    g_runBaseline = s.p1Combo > 0 ? s.p1Combo : 0;
    g_runDamageBaseline = s.p1Combo > 0 ? s.p1ComboDamage : 0;
    // A manual/tutorial retry can arm directly from Dropped. The old red row is
    // retained until this opener actually satisfies its requirement.
    g_runPhase = RunnerPhase::Idle;
    return true;
}

bool ShouldSplitConsecutiveSameIdBatch(const Snapshot& s, int total,
                                       int& nextStepOut) {
    nextStepOut = -1;
    if (!g_runArmed || g_runStep < 0 || g_runStep >= total ||
        s.p1Move != g_runPrevMove) {
        return false;
    }
    const int nextIndex = NextExpectedActionIndexForLiveMove(s, total);
    if (nextIndex >= total ||
        !StepMatches(g_runMission.steps[static_cast<std::size_t>(g_runStep)],
                     s.p1Move) ||
        !StepMatches(g_runMission.steps[static_cast<std::size_t>(nextIndex)],
                     s.p1Move) ||
        !StepArmEdge(g_runMission.steps[static_cast<std::size_t>(nextIndex)], s)) {
        return false;
    }
    nextStepOut = nextIndex;
    return true;
}

void TickRun(const Snapshot& s,
             RunObservationMode observationMode =
                 RunObservationMode::PlayerAttempt) {
    ScopedRunObservationMode observationScope(observationMode);
    const int total = static_cast<int>(g_runMission.steps.size());
    if (total == 0) { g_runPrevMove = s.p1Move; g_runPrevFrameIdx = s.p1FrameIdx; return; }

    // A playback divergence or completion is a terminal presentation state.
    // Keep its red/all-green bar latched until the demo's terminal baseline
    // restore instead of allowing later clip inputs to start another attempt.
    if (observationMode == RunObservationMode::DemoPresentation &&
        (g_runPhase == RunnerPhase::Dropped ||
         g_runPhase == RunnerPhase::Complete)) {
        return;
    }

    if (observationMode == RunObservationMode::PlayerAttempt &&
        TickAutoRetry()) { // grace or synchronous restore: never consume stale evidence
        DrawRunOverlay();
        return;
    }

    // Entity-only commands have no player move-ID edge. Retain their ordered
    // S-button edge until the exact restored slot/generation transition is
    // sampled; UpdateRunEntityLineage consumes both proofs atomically.
    ObserveRunEntityCommandInput(s);

    // Arm step zero before consuming this closed resolver batch. A frame-one
    // opener can otherwise land between monitor samples while the runner is
    // still Idle, permanently losing its first (and sometimes only) contact.
    (void)TryArmOpeningStep(s, total);

    RunDirectBatchContext directBatch;
    directBatch.initialStep = g_runStep;
    directBatch.initialHits = g_runDirectHits;
    directBatch.initialContacts = g_runDirectContacts;
    directBatch.initialConnect = g_runDirectConnectSeen;

    int expectedNextStep = -1;
    bool expectedNextAction = false;
    bool splitConsecutiveSameId = false;
    if (g_runArmed && g_runStep >= 0 && g_runStep < total) {
        expectedNextStep = NextExpectedActionIndexForLiveMove(s, total);
        expectedNextAction = expectedNextStep < total &&
            StepArmEdge(g_runMission.steps[
                static_cast<std::size_t>(expectedNextStep)], s);
        int sameIdNextStep = -1;
        splitConsecutiveSameId =
            ShouldSplitConsecutiveSameIdBatch(s, total, sameIdNextStep);
        if (splitConsecutiveSameId) expectedNextStep = sameIdNextStep;
        if (expectedNextAction && expectedNextStep >= 0 &&
            expectedNextStep < total) {
            directBatch.nextStep = expectedNextStep;
        }
    }

    uint32_t deferredDirectSequence = 0;
    bool observedArmedDirectStep = false;
    bool satisfiedAtBatchStart = false;
    if (g_runPhase != RunnerPhase::Dropped &&
        g_runPhase != RunnerPhase::Complete && g_runArmed &&
        g_runStep >= 0 && g_runStep < total &&
        g_runMission.steps[static_cast<std::size_t>(g_runStep)].directContact) {
        const ::Mission::Step& armedStep =
            g_runMission.steps[static_cast<std::size_t>(g_runStep)];
        observedArmedDirectStep = true;
        satisfiedAtBatchStart = ArmedStepSatisfied(armedStep, s);
        if (!ObserveArmedDirectContacts(
                armedStep, s, splitConsecutiveSameId,
                &deferredDirectSequence)) {
            g_runPrevMove = s.p1Move;
            g_runPrevFrameIdx = s.p1FrameIdx;
            DrawRunOverlay();
            return;
        }
    }

    // Confirm that the preserved journal transaction really belongs to the
    // authored successor. A different unexpected action is left unclaimed and
    // will be rejected by the strict action-instance grader below.
    if (deferredDirectSequence != 0 && expectedNextAction &&
        expectedNextStep >= 0 && expectedNextStep < total) {
        for (std::size_t i = 0; i < s.contactEventCount; ++i) {
            const auto& event = s.contactEvents[i];
            if (event.sequence != deferredDirectSequence) continue;
            if (event.source == ::Mission::Contact::Source::DirectPlayer &&
                event.attacker == 1 && event.defender == 2 &&
                StepMatches(g_runMission.steps[
                    static_cast<std::size_t>(expectedNextStep)],
                    event.attackerMove)) {
                directBatch.nextStepSequence = deferredDirectSequence;
            }
            break;
        }
    }

    bool directStepSatisfied = false;
    bool flexibleCanFinalize = false;
    bool acceptedPartialHitCount = false;
    const char* flexibleProof = nullptr;
    if (observedArmedDirectStep && g_runPhase != RunnerPhase::Dropped &&
        g_runStep == directBatch.initialStep) {
        const ::Mission::Step& armedStep =
            g_runMission.steps[static_cast<std::size_t>(g_runStep)];
        directStepSatisfied = ArmedStepSatisfied(armedStep, s);
        const bool comboAlive = s.p2InStun || s.p1Combo > 0;
        const bool sampledComboBoundary =
            ::Mission::SequencePolicy::DetectSampledComboBoundary(
                g_runComboWasAlive, comboAlive,
                g_runPrevComboCount, s.p1Combo) !=
            ::Mission::SequencePolicy::SampledComboBoundary::None;
        const bool exactComboRestart =
            SnapshotContainsExactComboRestartAfter(
                s, g_runDirectLastSequence);
        const bool moveEnded = s.p1Move != g_runPrevMove &&
            !IsComboStepMove(s.p1Move);
        flexibleCanFinalize =
            ::Mission::SequencePolicy::FlexibleMultiHitCanFinalize(
                armedStep.allowPartialHits, g_runDirectHits, moveEnded,
                expectedNextAction,
                sampledComboBoundary || exactComboRestart);
        acceptedPartialHitCount = flexibleCanFinalize &&
            !directStepSatisfied;
        if (expectedNextAction) flexibleProof = "next authored action";
        else if (exactComboRestart) flexibleProof = "exact combo restart";
        else if (sampledComboBoundary) flexibleProof = "combo boundary";
        else if (moveEnded) flexibleProof = "move recovery";

        const auto satisfaction =
            ::Mission::SequencePolicy::DecideStepSatisfaction(
                directStepSatisfied || flexibleCanFinalize,
                g_runAwaitingComboEnd,
                armedStep.req == ::Mission::StepReq::Move);
        if (satisfaction == ::Mission::SequencePolicy::
                                StepSatisfactionDecision::PrematureContactDrop) {
            LogOut("[MISSION][RUN] step connected before required combo.end step=" +
                       std::to_string(g_runStep), true);
            RunnerDrop();
        } else {
            directBatch.initialStepWillFinalize =
                satisfaction == ::Mission::SequencePolicy::
                                    StepSatisfactionDecision::Advance;
            directBatch.initialFlexibleFinalizedByNextAction =
                directBatch.initialStepWillFinalize && acceptedPartialHitCount &&
                expectedNextAction;
            directBatch.initialVariableHitCount =
                directBatch.initialStepWillFinalize && acceptedPartialHitCount;
            directBatch.initialStepSatisfied = satisfiedAtBatchStart;
            directBatch.initialStepRequiresComboEnd =
                armedStep.comboEndAfter;
            directBatch.comboEndOwnerSequence = g_runDirectLastSequence;
        }
    }

    // A later direct action in this batch cannot be carried into the next
    // monitor sample. If the armed step cannot legitimately finish now, fail
    // at the real sequence edge instead of silently losing that later contact.
    if (g_runPhase != RunnerPhase::Dropped &&
        deferredDirectSequence != 0 &&
        !directBatch.initialStepWillFinalize) {
        LogOut("[MISSION][RUN] next direct action arrived before step=" +
                   std::to_string(directBatch.initialStep) +
                   " satisfied its recorded requirement", true);
        RunnerDrop();
    }

    // The sampled aggregate can read the same value on both sides of a complete
    // recovery/restart. Preserve the resolver journal's exact boundary for both
    // entity-segment grading and the linear combo-end state machine below.
    const bool exactBoundaryBeforeAdvance =
        g_runPhase == RunnerPhase::InProgress &&
        SnapshotHasExactAwaitedComboBoundary(s);

    if (g_runPhase != RunnerPhase::Dropped &&
        g_runPhase != RunnerPhase::Complete &&
        !ObserveRunEntityContacts(
            s, exactBoundaryBeforeAdvance, directBatch)) {
        g_runPrevMove = s.p1Move;
        g_runPrevFrameIdx = s.p1FrameIdx;
        DrawRunOverlay();
        return;
    }

    // The command transition and its same-batch entity contacts have now been
    // consumed in strict journal order. Advance the input-only action only at
    // this point; advancing on the S edge alone would let a missing/wrong
    // summon transition pass.
    if (g_runPhase != RunnerPhase::Dropped &&
        g_runPhase != RunnerPhase::Complete &&
        g_runEntityCommandActionObservedThisTick >= 0) {
        const int commandAction =
            g_runEntityCommandActionObservedThisTick;
        if (commandAction != g_runStep ||
            commandAction < 0 || commandAction >= total ||
            !g_runMission.steps[static_cast<std::size_t>(commandAction)]
                 .entityCommand.present) {
            DropForEntitySchedule(
                "The entity-only command occurred outside its recorded action");
        } else if (EntityScheduleAllowsDirectStep(commandAction)) {
            if (g_runPhase == RunnerPhase::Idle) {
                g_runFailedStep.store(-1);
                g_runFailedEntityRequirement.store(
                    -1, std::memory_order_release);
                g_runPhase = RunnerPhase::InProgress;
            }
            g_runEntityCommandActionObservedThisTick = -1;
            AdvanceStep(s, total);
        }
    }

    // Entity requirements that precede this direct step have now consumed the
    // same ordered journal. Only now is it safe to advance the linear recipe.
    if (g_runPhase != RunnerPhase::Dropped &&
        directBatch.initialStepWillFinalize && g_runArmed &&
        g_runStep == directBatch.initialStep) {
        const ::Mission::Step& completedStep =
            g_runMission.steps[static_cast<std::size_t>(g_runStep)];
        if (EntityScheduleAllowsDirectStep(g_runStep) &&
            (acceptedPartialHitCount ||
             StepDamageAcceptable(completedStep, s, g_runStep))) {
            if (g_runPhase == RunnerPhase::Idle) {
                g_runFailedStep.store(-1);
                g_runFailedEntityRequirement.store(
                    -1, std::memory_order_release);
                g_runPhase = RunnerPhase::InProgress;
            }
            if (acceptedPartialHitCount) {
                (void)FinalizeCurrentFlexibleMultiHit(
                    s, total, flexibleProof ? flexibleProof : "continuation");
            } else {
                AdvanceStep(s, total);
            }
        }
    }

    const bool exactOrderedBoundary =
        g_runPhase == RunnerPhase::InProgress &&
        SnapshotHasExactAwaitedComboBoundary(s);
    if (g_runPhase == RunnerPhase::InProgress && g_runStep >= total &&
        !g_runAwaitingComboEnd && AllEntityRequirementsSatisfied()) {
        ObserveRunScore(s);
        RunnerComplete();
    }

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
            if (!EntityScheduleAllowsActionStart(0)) {
                g_runPrevMove = s.p1Move;
                g_runPrevFrameIdx = s.p1FrameIdx;
                DrawRunOverlay();
                return;
            }
            g_runArmed = true;
            g_runArmedFrames = 0;
            ResetRunContactEvidence();
            g_runBaseline = (s.p1Combo > 0) ? s.p1Combo : 0;
            g_runDamageBaseline = (s.p1Combo > 0) ? s.p1ComboDamage : 0;
            g_runPhase = RunnerPhase::Idle;   // clear a shown DROPPED once they retry
        }
        if (g_runArmed) {
            if (!ObserveArmedDirectContacts(st, s)) {
                g_runPrevMove = s.p1Move;
                g_runPrevFrameIdx = s.p1FrameIdx;
                DrawRunOverlay();
                return;
            }
            if (!s.freezeActive) ++g_runArmedFrames;
            if (st.maxDelay > 0 && g_runArmedFrames > st.maxDelay) {
                g_runArmed = false;           // window expired; wait for a fresh attempt
                ResetRunContactEvidence();
            } else if (ArmedStepSatisfied(st, s)) {
                if (EntityScheduleAllowsDirectStep(0) &&
                    StepDamageAcceptable(st, s, 0)) {
                    // This is the retry boundary: the opener actually connected/met
                    // its requirement, so the old failure marker can finally clear.
                    g_runFailedStep.store(-1);
                    g_runFailedEntityRequirement.store(-1,
                                                       std::memory_order_release);
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
        const bool comboEndedEdge = exactOrderedBoundary ||
            sampledBoundary != ::Mission::SequencePolicy::SampledComboBoundary::None;
        const bool boundaryIncludesCurrentSample = exactOrderedBoundary ||
            sampledBoundary ==
                ::Mission::SequencePolicy::SampledComboBoundary::CounterRollover;
        bool consumedRequiredBoundary = false;
        bool actionStartedThisTick = false;
        // On an atomic N->1 rollover the current sample already belongs to Part
        // 2. Finalize the previously observed maxima first, then seed the new
        // segment from this sample after consuming the boundary.
        if (!boundaryIncludesCurrentSample) {
            ObserveRunScore(s);
        }

        // A recorded 5/6-hit move may make only four contacts at different
        // spacing. The authored combo.end is structural proof that the route
        // continued; promote the step before evaluating that same boundary.
        if (comboEndedEdge && CurrentFlexibleMultiHitHasEvidence()) {
            (void)FinalizeCurrentFlexibleMultiHit(s, total,
                                                  "combo boundary");
        }

        // Combo recovery is evaluated before the next move on this sample. An
        // authored edge is a first-class boundary; an unmarked edge still means
        // a one-piece combo was dropped. Consuming the boundary also rebases hit
        // validation so Part 2 starts from combo count zero.
        const auto comboEndDecision = g_runPhase == RunnerPhase::InProgress
            ? ::Mission::SequencePolicy::DecideComboEnd(
                  comboEndedEdge, g_runAwaitingComboEnd)
            : ::Mission::SequencePolicy::ComboEndDecision::None;
        if (comboEndDecision != ::Mission::SequencePolicy::ComboEndDecision::None) {
            if (comboEndDecision ==
                ::Mission::SequencePolicy::ComboEndDecision::ConsumeRequired) {
                FinalizeRunSegment();
                if (boundaryIncludesCurrentSample) {
                    ObserveRunScore(s);
                }
                g_runAwaitingComboEnd = false;
                g_runAwaitingComboEndAfterSequence = 0;
                ++g_runEntitySegment;
                g_runSegmentHasVariableHitCount = false;
                consumedRequiredBoundary = true;
                g_runBaseline = 0;
                g_runDamageBaseline = 0;
                g_runGapWindow.Reset();
                LogOut("[MISSION][RUN] consumed required combo.end; nextStep=" +
                       std::to_string(g_runStep) +
                       " kind=" +
                       (sampledBoundary == ::Mission::SequencePolicy::
                                              SampledComboBoundary::CounterRollover
                            ? "counter-rollover"
                            : exactOrderedBoundary
                                ? "exact-ordered-contact"
                                : "visible-recovery") +
                       " finalizedHits=" + std::to_string(g_runScore.finalizedHits) +
                       " finalizedDamage=" + std::to_string(g_runScore.finalizedDamage), true);
                if (g_runStep >= total && AllEntityRequirementsSatisfied()) {
                    RunnerComplete();
                }
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
            // STRICT SEQUENCE (CCCaster model): grade every NEW action instance,
            // including a same-ID normal whose frame counter rewinds. Looking at
            // move-ID changes alone let an extra 5A disappear between authored
            // 5A and 5B steps. Optional skip-ahead, flattened automatic phases,
            // and a delayed-hit overlap remain the authored exceptions.
            if (g_runStep < total) {
                const bool moveChanged = (s.p1Move != g_runPrevMove);
                constexpr uint8_t kStrictAttackButtons =
                    INPUT_A | INPUT_B | INPUT_C | INPUT_D;
                const uint8_t sampledStrictEdge = static_cast<uint8_t>(
                    s.p1Inputs & kStrictAttackButtons &
                    static_cast<uint8_t>(~g_runPrevInputs));
                const uint8_t observedStrictEdges = static_cast<uint8_t>(
                    s.p1PolledAttackEdges | sampledStrictEdge);
                const uint8_t currentMoveMask =
                    ExpectedAttackMaskForKnownMove(s.p1Move);
                const bool matchingCurrentMoveEdge =
                    ::Mission::SequencePolicy::DeadlineAttackEdgeMatches(
                        observedStrictEdges, currentMoveMask);
                const bool actionInstanceEdge =
                    ::Mission::SequencePolicy::
                        IsCausallyCredibleMoveInstanceEdge(
                            s.p1Move, s.p1FrameIdx,
                            g_runPrevMove, g_runPrevFrameIdx,
                            matchingCurrentMoveEdge);

                bool flexibleTransitionAccepted = false;
                if (CurrentFlexibleMultiHitHasEvidence()) {
                    int nextIndex = g_runStep + 1;
                    while (nextIndex < total &&
                           g_runMission.steps[static_cast<std::size_t>(nextIndex)]
                               .optional &&
                           !StepMatches(
                               g_runMission.steps[static_cast<std::size_t>(nextIndex)],
                               s.p1Move)) {
                        ++nextIndex;
                    }
                    const bool expectedNextAction =
                        actionInstanceEdge && nextIndex < total &&
                        StepMatches(
                            g_runMission.steps[static_cast<std::size_t>(nextIndex)],
                            s.p1Move);
                    const bool moveEnded = moveChanged &&
                        !IsComboStepMove(s.p1Move);
                    if (::Mission::SequencePolicy::
                            FlexibleMultiHitCanFinalize(
                                true, g_runDirectHits, moveEnded,
                                expectedNextAction, false)) {
                        flexibleTransitionAccepted =
                            FinalizeCurrentFlexibleMultiHit(
                                s, total,
                                expectedNextAction ? "next authored action"
                                                   : "move recovery");
                        if (flexibleTransitionAccepted && g_runArmed) {
                            actionStartedThisTick = true;
                        }
                    }
                }

                if (g_runStep >= total ||
                    g_runPhase != RunnerPhase::InProgress) {
                    // A terminal flexible step may have completed above.
                } else if (flexibleTransitionAccepted && g_runArmed) {
                    // AdvanceStep already armed this exact next action from the
                    // current sample; do not grade it twice as another instance.
                } else {
                const bool comboStepMove = IsComboStepMove(s.p1Move);
                const bool matchesCurrent =
                    StepMatches(g_runMission.steps[g_runStep], s.p1Move);
                const bool previousMoveMatchesCurrent =
                    StepMatches(g_runMission.steps[g_runStep], g_runPrevMove);
                const bool prevStepEcho = (g_runStep > 0) &&
                    StepMatches(g_runMission.steps[g_runStep - 1], s.p1Move);
                const bool previousMoveMatchesPreviousStep = (g_runStep > 0) &&
                    StepMatches(g_runMission.steps[g_runStep - 1], g_runPrevMove);
                int optionalIndex = g_runStep;
                while (optionalIndex < total &&
                       g_runMission.steps[optionalIndex].optional &&
                       !StepMatches(g_runMission.steps[optionalIndex],
                                    s.p1Move)) {
                    ++optionalIndex;
                }
                const bool matchedAhead = optionalIndex < total &&
                    optionalIndex != g_runStep &&
                    StepMatches(g_runMission.steps[optionalIndex], s.p1Move);
                const bool nextOverlap = g_runArmed &&
                    g_runStep + 1 < total &&
                    StepMatches(g_runMission.steps[g_runStep + 1], s.p1Move);
                const auto strictDecision =
                    ::Mission::SequencePolicy::DecideStrictActionInstance(
                        actionInstanceEdge, comboStepMove, g_runArmed,
                        matchesCurrent, previousMoveMatchesCurrent,
                        prevStepEcho, previousMoveMatchesPreviousStep,
                        moveChanged, nextOverlap, matchedAhead);
                if (strictDecision == ::Mission::SequencePolicy::
                                          StrictActionInstanceDecision::SkipOptional) {
                    g_runStep = optionalIndex;
                    g_runArmed = false;
                    ResetRunContactEvidence();
                } else if (strictDecision == ::Mission::SequencePolicy::
                                                 StrictActionInstanceDecision::RejectUnexpected) {
                    const bool sameIdReentry = !moveChanged && actionInstanceEdge;
                    LogOut("[MISSION][RUN] " +
                               std::string(sameIdReentry
                                   ? "extra same-ID action instance "
                                   : "wrong action instance ") +
                               std::to_string(s.p1Move) + " frame=" +
                               std::to_string(g_runPrevFrameIdx) + "->" +
                               std::to_string(s.p1FrameIdx) + " at step " +
                               std::to_string(g_runStep) +
                               " armed=" + (g_runArmed ? "true" : "false"),
                           true);
                    RunnerDrop();
                    g_runArmed = false;
                }
                }
            }

            if (g_runPhase == RunnerPhase::InProgress && g_runStep < total) {
                const ::Mission::Step& st = g_runMission.steps[g_runStep];
                if (!g_runArmed && StepArmEdge(st, s)) {
                    if (!EntityScheduleAllowsActionStart(g_runStep)) {
                        g_runPrevMove = s.p1Move;
                        g_runPrevFrameIdx = s.p1FrameIdx;
                        DrawRunOverlay();
                        return;
                    }
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
                        ResetRunContactEvidence();
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
                    if (!ObserveArmedDirectContacts(st, s)) {
                        g_runPrevMove = s.p1Move;
                        g_runPrevFrameIdx = s.p1FrameIdx;
                        DrawRunOverlay();
                        return;
                    }
                    if (!s.freezeActive) ++g_runArmedFrames;
                    if (st.maxDelay > 0 && g_runArmedFrames > st.maxDelay) {
                        g_runArmed = false;
                        ResetRunContactEvidence();
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
                            if (EntityScheduleAllowsDirectStep(g_runStep) &&
                                StepDamageAcceptable(st, s, g_runStep)) {
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
            const uint8_t sampledAttackEdge = static_cast<uint8_t>(
                s.p1Inputs & kAttackButtons &
                static_cast<uint8_t>(~g_runPrevInputs));
            const uint8_t observedAttackEdges = static_cast<uint8_t>(
                s.p1PolledAttackEdges | sampledAttackEdge);
            const uint8_t expectedAttackMask = g_runStep < total
                ? static_cast<uint8_t>(
                      g_runMission.steps[g_runStep].expectedAttackMask)
                : 0;
            const bool attackPressedEdge = !actionStartedThisTick &&
                ::Mission::SequencePolicy::DeadlineAttackEdgeMatches(
                    observedAttackEdges, expectedAttackMask);
            bool gapExpired = false;
            if (!g_runAwaitingComboEnd && !g_runArmed && !comboAlive &&
                !PendingEntityScheduleOwnsWait(s) && gapLimit > 0) {
                const int oldGrace = g_runGapWindow.commitGrace;
                gapExpired = g_runGapWindow.Tick(
                    s.freezeActive, false, attackPressedEdge, gapLimit);
                if (oldGrace == 0 && g_runGapWindow.commitGrace > 0) {
                    LogOut("[MISSION][RUN] latched delayed attack input at gap deadline step=" +
                           std::to_string(g_runStep) + " pollSerial=" +
                           std::to_string(s.p1InputPollSerial) + " observed=" +
                           DecodeInputMask(observedAttackEdges) + " expected=" +
                           DecodeInputMask(expectedAttackMask), true);
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
    g_recSetupSegmentOpen = false;
    g_recPendingComboEndStep = -1;
    g_recPendingComboEndEntitySequence = 0;
    g_recPendingComboEndAfterAction = -1;
    g_recComboEndFrame = 0;
    g_recLastLandedContactSequence = 0;
    g_recLastLandedDirectStep = -1;
    g_recLastLandedEntitySequence = 0;
    g_recPrevHitState = 0;
    g_recPrevMove = 0;
    g_recPrevFrameIdx = 0;
    g_recAttackEdgeAge = 999;
    g_recTokenEdgeAge = 999;
    g_recPrevTokenSample = 99;
    g_recUncommittedAttackFrame = -1;
    g_recPendingAttacks.fill(PendingRecorderAttack{});
    g_recPendingAttackCount = 0;
    g_recPendingAttackOverflow = false;
    g_recStopPending = false;
    g_recStopSawPostRequestTick = false;
    g_recStopWaitTicks = 0;
    g_recStartedThisTick = false;
    g_recTakeIntegrityValid = true;
    g_recTakeIntegrityError.clear();
    g_recSealedDemo.clear();
    g_recPreviewAvailableView.store(false, std::memory_order_release);
    g_recAkikoRekkaRep = 0;
    g_recNextActionOrder = 0;
    g_recEntitySampleSerial = 0;
    for (auto& playerSlots : g_recEntitySlots) {
        playerSlots.fill(RecEntitySlotState{});
    }
    g_recEntityTrace.fill(RecEntityTraceEvent{});
    g_recEntityTraceCount = 0;
    g_recEntityTraceDropped = 0;
    g_recEntityContactHookObserved = false;
    g_recDirectContactHookObserved = false;
    g_recEntityContactHookComplete = true;
    g_recContactWorldEpoch = 0;
    g_recContactEpochDiscontinuity = false;
    g_recContactJournalOverflowObserved = false;
    g_recEntityAttributionRequired = false;
    g_recUnclassifiedContactRequired = false;
    g_recLegacyContactAttributionRequired = false;
    g_recMixedDirectResultsRequired = false;
    g_recUnboundCommandOriginRequired = false;
    g_recEntityProbeUnavailableLogged.fill(false);
    g_recEntityCursors.fill(RecEntityCursorState{});
    g_recEntityProbeIncomplete.fill(false);
    g_recEntityAllocationAmbiguous.fill(false);
    g_recCountInView.store(0, std::memory_order_release);
    InvalidateRecorderBannerInputs();
}

void SetRecorderInputLease(bool on) {
    if (on) {
        if (!g_recInputLeaseHeld) {
            g_recPollSnapshotValid = true;
            g_recPollSavedActive =
                g_pollOverrideActive[1].load(std::memory_order_acquire);
            g_recPollSavedMask =
                g_pollOverrideMask[1].load(std::memory_order_acquire);
            g_recPollSavedObservation =
                IsPollOverridePhysicalObservationEnabled(1);
            g_recInputLeaseHeld = true;
        }
        // Reassert on every CountIn tick.  Savestate/control cleanup paths may
        // clear the atomics, but no physical P1 poll may reach EFZ between the
        // countdown and the synchronized recording boundary.
        g_pollOverrideMask[1].store(0, std::memory_order_relaxed);
        SetPollOverridePhysicalObservation(1, true);
        g_pollOverrideActive[1].store(true, std::memory_order_release);
        return;
    }

    if (!g_recInputLeaseHeld) return;
    g_recInputLeaseHeld = false;
    if (!g_recPollSnapshotValid) return;

    // Restore only if the recognizable zero lease is still ours.  A newer
    // exclusive playback/runner owner always wins this hand-off.
    const bool curActive =
        g_pollOverrideActive[1].load(std::memory_order_acquire);
    const uint8_t curMask =
        g_pollOverrideMask[1].load(std::memory_order_acquire);
    if (IsPollOverridePhysicalObservationEnabled(1)) {
        SetPollOverridePhysicalObservation(1, g_recPollSavedObservation);
    }
    if (!Demo::IsActive() && !MacroController::IsExclusivePlayback() &&
        curMask == 0) {
        g_pollOverrideMask[1].store(g_recPollSavedMask,
                                    std::memory_order_relaxed);
    }
    if (!Demo::IsActive() && !MacroController::IsExclusivePlayback() &&
        curActive) {
        g_pollOverrideActive[1].store(g_recPollSavedActive,
                                      std::memory_order_release);
    }
    g_recPollSnapshotValid = false;
}

void ClearRecorderCaptureMenuSuspension() {
    g_recCaptureMenuSuspended = false;
    g_recCaptureMenuLastPollSerial = 0;
    g_recCaptureMenuNeutralPolls = 0;
    MacroController::SetRecordingCaptureSuspended(false);
    PauseIntegration::OnMenuSurfaceVisibilityChanged(
        PauseIntegration::MenuSurface::RecorderHandoff, false);
    // Retire the physical owner before publishing that hotkey/input gates may
    // reopen. A short over-suppression is safe; a gate-free frozen interval is
    // not.
    g_recCaptureMenuRequested.store(false, std::memory_order_release);
    g_recCaptureMenuReleasePending.store(false, std::memory_order_release);
    g_recCaptureMenuSuspendedView.store(false, std::memory_order_release);
}

void ClearRecorderMenuCommandHandoff() {
    g_recMenuCommandHandoffDone.store(false, std::memory_order_release);
    g_recMenuCommandHandoffActive = false;
    g_recMenuCommandPauseOwned = false;
    g_recMenuCommandHandoffLastPollSerial = 0;
    g_recMenuCommandHandoffNeutralPolls = 0;
    PauseIntegration::OnMenuSurfaceVisibilityChanged(
        PauseIntegration::MenuSurface::RecorderCommandHandoff, false);
    // Publish the gate release last. It is safe for hotkeys to remain blocked
    // a few instructions after the physical pause leaves; the inverse would
    // let Practice actions run underneath an owned handoff surface.
    g_recMenuCommandHandoffRequested.store(false, std::memory_order_release);
}

// Phase-changing dedicated-menu actions are frontend commands consumed later
// in this mission tick. Keep a separate physical freeze through command
// completion, then retain only the zero-poll lease until two fresh
// physical-neutral polls have been seen. The capture-menu handoff uses another
// surface bit and may overlap this one.
void TickRecorderMenuCommandHandoff() {
    if (!g_recMenuCommandHandoffRequested.load(std::memory_order_acquire)) return;

    if (!g_recMenuCommandHandoffActive) {
        g_recMenuCommandHandoffActive = true;
        g_recMenuCommandHandoffLastPollSerial = GetInputPollSerial(1);
        g_recMenuCommandHandoffNeutralPolls = 0;
        ResetInputPollOverrideHitCount(1);
        uint32_t discardedSerial = 0;
        (void)ConsumeInputPollAttackEdgeBatch(1, discardedSerial);
        LogOut("[MISSION][REC] recorder menu command handoff acquired", true);
    }

    SetRecorderInputLease(true);
    uint32_t discardedSerial = 0;
    (void)ConsumeInputPollAttackEdgeBatch(1, discardedSerial);
    if (!g_recMenuCommandHandoffDone.load(std::memory_order_acquire)) return;

    const bool anyMenuVisible =
        ::Mission::PauseMenu::IsOpen() || ImGuiImpl::IsVisible();
    // The native input serial below cannot advance while EFZ's official
    // Practice pause remains engaged. Retire only this physical pause owner
    // once the command is complete; the zero-mask P1 lease remains active
    // until the existing two-neutral-poll handshake finishes.
    if (g_recMenuCommandPauseOwned &&
        ::Mission::SequencePolicy::RecorderPhysicalPauseReleaseReady(
            true, anyMenuVisible)) {
        PauseIntegration::OnMenuSurfaceVisibilityChanged(
            PauseIntegration::MenuSurface::RecorderCommandHandoff, false);
        g_recMenuCommandPauseOwned = false;
        LogOut("[MISSION][REC] recorder command pause owner released; P1 remains quarantined",
               true);
    }
    const uint32_t serial = GetInputPollSerial(1);
    const bool freshPoll = serial != g_recMenuCommandHandoffLastPollSerial;
    if (freshPoll) g_recMenuCommandHandoffLastPollSerial = serial;
    g_recMenuCommandHandoffNeutralPolls =
        ::Mission::SequencePolicy::RecorderMenuNeutralPollCount(
            anyMenuVisible, freshPoll,
            GetLastObservedPhysicalPollMask(1),
            g_recMenuCommandHandoffNeutralPolls);
    if (!::Mission::SequencePolicy::RecorderMenuReleaseReady(
            anyMenuVisible, g_recMenuCommandHandoffNeutralPolls)) {
        return;
    }

    const RecorderPhase phase = g_recPhase.load(std::memory_order_acquire);
    if (phase != RecorderPhase::CountIn && phase != RecorderPhase::Recording) {
        SetRecorderInputLease(false);
    }
    ClearRecorderMenuCommandHandoff();
    LogOut("[MISSION][REC] recorder menu command released after two neutral P1 polls",
           true);
}

// Returns true while capture work must remain stopped for the recorder pause
// surface, its nested Practice settings, or the release-to-neutral handoff.
bool TickRecorderCaptureMenuSuspension() {
    const RecorderPhase phase =
        g_recPhase.load(std::memory_order_acquire);
    const bool capturePhase = phase == RecorderPhase::CountIn ||
                              phase == RecorderPhase::Recording;
    const bool requested =
        g_recCaptureMenuRequested.load(std::memory_order_acquire);
    const bool releasePending = !requested &&
        g_recCaptureMenuReleasePending.exchange(false,
                                                std::memory_order_acq_rel);

    auto beginSuspension = [&]() {
        g_recCaptureMenuSuspended = true;
        g_recCaptureMenuSuspendedView.store(true, std::memory_order_release);
        g_recCaptureMenuLastPollSerial = GetInputPollSerial(1);
        g_recCaptureMenuNeutralPolls = 0;
        ResetInputPollOverrideHitCount(1);
        uint32_t discardedSerial = 0;
        (void)ConsumeInputPollAttackEdgeBatch(1, discardedSerial);
    };

    if (requested && capturePhase) {
        if (!g_recCaptureMenuSuspended) {
            beginSuspension();
            LogOut("[MISSION][REC] capture suspended for recorder menu", true);
        }
        MacroController::SetRecordingCaptureSuspended(true);
        SetRecorderInputLease(true);
        uint32_t discardedSerial = 0;
        (void)ConsumeInputPollAttackEdgeBatch(1, discardedSerial);
        return true;
    }

    if (!g_recCaptureMenuSuspended) {
        if (!releasePending || !capturePhase) {
            // No live capture handoff exists. A stale provisional owner can
            // only come from a phase transition/teardown, so retire it.
            if (releasePending) {
                MacroController::SetRecordingCaptureSuspended(false);
                PauseIntegration::OnMenuSurfaceVisibilityChanged(
                    PauseIntegration::MenuSurface::RecorderHandoff, false);
            }
            return false;
        }
        // The menu opened and closed entirely between recorder ticks. Install
        // the suspension now and run the normal two-poll neutral handshake;
        // otherwise the close/confirm button could become the next macro byte.
        beginSuspension();
        MacroController::SetRecordingCaptureSuspended(true);
        SetRecorderInputLease(true);
        LogOut("[MISSION][REC] capture close raced recorder tick; neutral handoff recovered",
               true);
    }

    // Count-in owns the same zero-poll lease, but a zero-duration count-in can
    // otherwise expose the A/B press used to close this menu at frame zero.
    // Require the same physical-neutral handshake before its timer continues.
    if (phase == RecorderPhase::CountIn) {
        SetRecorderInputLease(true);
        uint32_t discardedSerial = 0;
        (void)ConsumeInputPollAttackEdgeBatch(1, discardedSerial);
        const bool anyMenuVisible =
            ::Mission::PauseMenu::IsOpen() || ImGuiImpl::IsVisible();
        if (::Mission::SequencePolicy::RecorderPhysicalPauseReleaseReady(
                true, anyMenuVisible)) {
            PauseIntegration::OnMenuSurfaceVisibilityChanged(
                PauseIntegration::MenuSurface::RecorderHandoff, false);
        }
        const uint32_t serial = GetInputPollSerial(1);
        const bool freshPoll = serial != g_recCaptureMenuLastPollSerial;
        if (freshPoll) g_recCaptureMenuLastPollSerial = serial;
        g_recCaptureMenuNeutralPolls =
            ::Mission::SequencePolicy::RecorderMenuNeutralPollCount(
                anyMenuVisible, freshPoll,
                GetLastObservedPhysicalPollMask(1),
                g_recCaptureMenuNeutralPolls);
        if (!::Mission::SequencePolicy::RecorderMenuReleaseReady(
                anyMenuVisible, g_recCaptureMenuNeutralPolls)) {
            return true;
        }
        MacroController::SetRecordingCaptureSuspended(false);
        g_recCaptureMenuSuspended = false;
        g_recCaptureMenuNeutralPolls = 0;
        PauseIntegration::OnMenuSurfaceVisibilityChanged(
            PauseIntegration::MenuSurface::RecorderHandoff, false);
        g_recCaptureMenuSuspendedView.store(false, std::memory_order_release);
        LogOut("[MISSION][REC] countdown resumed after two neutral P1 polls", true);
        return false;
    }

    if (phase != RecorderPhase::Recording) {
        if (!g_recMenuCommandHandoffRequested.load(
                std::memory_order_acquire)) {
            SetRecorderInputLease(false);
        }
        ClearRecorderCaptureMenuSuspension();
        return false;
    }

    // Keep the world/input stream quarantined until both UI surfaces are gone.
    // The override observes the underlying physical poll while returning zero,
    // allowing a real release check without leaking the menu's A/B press.
    MacroController::SetRecordingCaptureSuspended(true);
    SetRecorderInputLease(true);
    uint32_t discardedSerial = 0;
    (void)ConsumeInputPollAttackEdgeBatch(1, discardedSerial);
    const bool anyMenuVisible =
        ::Mission::PauseMenu::IsOpen() || ImGuiImpl::IsVisible();
    if (::Mission::SequencePolicy::RecorderPhysicalPauseReleaseReady(
            true, anyMenuVisible)) {
        PauseIntegration::OnMenuSurfaceVisibilityChanged(
            PauseIntegration::MenuSurface::RecorderHandoff, false);
    }
    if (anyMenuVisible) {
        g_recCaptureMenuNeutralPolls =
            ::Mission::SequencePolicy::RecorderMenuNeutralPollCount(
                true, false, 0, g_recCaptureMenuNeutralPolls);
        g_recCaptureMenuLastPollSerial = GetInputPollSerial(1);
        return true;
    }

    const uint32_t serial = GetInputPollSerial(1);
    const bool freshPoll = serial != g_recCaptureMenuLastPollSerial;
    if (freshPoll) {
        g_recCaptureMenuLastPollSerial = serial;
        g_recCaptureMenuNeutralPolls =
            ::Mission::SequencePolicy::RecorderMenuNeutralPollCount(
                false, true, GetLastObservedPhysicalPollMask(1),
                g_recCaptureMenuNeutralPolls);
    }
    if (!::Mission::SequencePolicy::RecorderMenuReleaseReady(
            false, g_recCaptureMenuNeutralPolls)) return true;

    // Rebase the macro buffer cursor while the zero-poll lease still owns P1,
    // then expose physical input on the following poll.
    MacroController::SetRecordingCaptureSuspended(false);
    SetRecorderInputLease(false);
    g_recCaptureMenuSuspended = false;
    g_recCaptureMenuNeutralPolls = 0;
    PauseIntegration::OnMenuSurfaceVisibilityChanged(
        PauseIntegration::MenuSurface::RecorderHandoff, false);
    g_recCaptureMenuSuspendedView.store(false, std::memory_order_release);
    LogOut("[MISSION][REC] capture resumed after two neutral P1 polls", true);
    return false;
}

void ShowRecorderState(const char* text, COLORREF color, int duration = 900) {
    // Transient notices sit below the phase banner; routine phase state itself is
    // owned by RefreshRecorderBanner and never expires.
    DirectDrawHook::AddMessage(text, "mission_record", color, duration, 20, 84);
}

void EnterRecorderPreRecord(bool keepSetup) {
    ClearRecorderCaptureMenuSuspension();
    if (!g_recMenuCommandHandoffRequested.load(std::memory_order_acquire)) {
        SetRecorderInputLease(false);
    }
    g_recActive.store(false, std::memory_order_release);
    g_recCountInFrames = 0;
    g_recCountInTargetTicks = 0;
    g_recCountInElapsed = false;
    g_recLeasePollWaitTicks = 0;
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
    // This Tick's Snapshot was sampled before the exact world replacement.
    // StateDump::Restore can also mutate the world before a later integrity
    // check reports failure, so invalidate the sample before entering it just
    // like the runner/demo restore transactions do.
    g_worldRestoredThisTick = true;
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

    struct CompleteMenuCommandHandoff {
        bool armed = false;
        ~CompleteMenuCommandHandoff() {
            if (armed) {
                g_recMenuCommandHandoffDone.store(true,
                                                   std::memory_order_release);
            }
        }
    } completeHandoff{
        g_recMenuCommandHandoffRequested.load(std::memory_order_acquire)};

    const RecorderPhase phase = g_recPhase.load(std::memory_order_acquire);
    if (cmd == RecorderCommand::Cancel) {
        if (MacroController::GetState() != MacroController::State::Idle) MacroController::Stop();
        ClearRecorderCaptureMenuSuspension();
        if (!g_recMenuCommandHandoffRequested.load(std::memory_order_acquire)) {
            SetRecorderInputLease(false);
        }
        g_recActive.store(false, std::memory_order_release);
        g_recPhase.store(RecorderPhase::Idle, std::memory_order_release);
        g_recCountInTargetTicks = 0;
        g_recCountInElapsed = false;
        g_recLeasePollWaitTicks = 0;
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
        // The author requested recording. The optional count-in is an input-owned
        // ready window; the exact baseline is captured on the clear
        // tick immediately after it ends, directly before synchronized input
        // recording begins.
        CancelAutoActionsAndMacros();
        ResetRecorderCaptureState();
        g_recSetup = ::Mission::Mission();
        g_recCountInFrames = 0;
        g_recCountInTargetTicks =
            ::Mission::SequencePolicy::RecorderCountInTicksFromMs(
                Config::GetSettings().missionRecorderCountInMs);
        g_recCountInElapsed = false;
        g_recCountInView.store(
            (std::max)(0, Config::GetSettings().missionRecorderCountInMs),
            std::memory_order_release);
        SetRecorderInputLease(true);
        (void)FullCleanupAfterToggle(1);
        // FullCleanup may retire an older input owner. Reassert our zero-mask
        // lease afterwards, then prove that EFZ actually consumed it before a
        // world snapshot can be accepted as frame zero.
        SetRecorderInputLease(true);
        ResetInputPollOverrideHitCount(1);
        ResetInputPollAttackEdgeJournal(1);
        g_recLeasePollWaitTicks = 0;
        g_recPhase.store(RecorderPhase::CountIn, std::memory_order_release);
        LogOut("[MISSION][REC] waiting for neutral count-in ms=" +
               std::to_string(Config::GetSettings().missionRecorderCountInMs), true);
    } else if (advance == ::Mission::Engine::Recorder::AdvanceEffect::CancelCountIn) {
        // The same physical control is a safe toggle during the countdown. It
        // never starts a partial take and never discards Review data.
        g_recCountInFrames = 0;
        g_recCountInTargetTicks = 0;
        g_recCountInElapsed = false;
        g_recLeasePollWaitTicks = 0;
        g_recCountInView.store(0, std::memory_order_release);
        ResetRecorderCaptureState();
        EnterRecorderPreRecord(false);
        ShowRecorderState("Countdown canceled; arrange the start and record again",
                          RGB(255, 220, 120), 1200);
        LogOut("[MISSION][REC] count-in canceled", true);
    } else if (advance == ::Mission::Engine::Recorder::AdvanceEffect::StopToReview) {
        // Seal only after a NEW post-request 64 Hz macro boundary.  The old
        // path stopped MacroController before RecorderCapture ran, truncating a
        // final move/contact or leaving the recipe and demo one tick apart.
        if (!g_recStopPending) {
            g_recStopPending = true;
            g_recStopSawPostRequestTick = false;
            g_recStopWaitTicks = 0;
            ShowRecorderState("Finishing the current input frame...",
                              RGB(180, 230, 255), 900);
            LogOut("[MISSION][REC] synchronized stop requested", true);
        }
    } else if (advance == ::Mission::Engine::Recorder::AdvanceEffect::KeepReview) {
        ShowRecorderState("Take is safe in Review; open the menu to preview, save, retake, or discard",
                          RGB(180, 230, 255), 1800);
    }
}

void TickRecorderFrontend(const Snapshot& s) {
    const RecorderPhase phase = g_recPhase.load(std::memory_order_acquire);
    if (TickRecorderCaptureMenuSuspension()) return;
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
    SetRecorderInputLease(true);
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
    if (blocker) {
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

    if (GetInputPollOverrideHitCount(1) == 0) {
        if (++g_recLeasePollWaitTicks <= 192) {
            return;
        }
        ResetRecorderCaptureState();
        EnterRecorderPreRecord(false);
        ShowRecorderState(
            "Recording did not start: the neutral P1 input lease was not observed",
            RGB(255, 120, 120), 2600);
        LogOut("[MISSION][REC] frame-zero rejected: P1 poll override was never consumed",
               true);
        return;
    }

    // Frame-zero boundary: clear stale action/queue owners, capture the exact
    // current world (including both entity rings), seed those rings as baseline
    // observations, then start the P1 input stream without another live tick.
    CancelAutoActionsAndMacros();
    // Global cleanup clears poll overrides. Reclaim the recorder lease and
    // scrub EFZ's native human-input ring while P1 is still forced neutral.
    SetRecorderInputLease(true);
    if (!FullCleanupAfterToggle(1)) {
        ResetRecorderCaptureState();
        EnterRecorderPreRecord(false);
        ShowRecorderState("Recording did not start: P1 input history could not be cleared",
                          RGB(255, 120, 120), 2400);
        LogOut("[MISSION][REC] frame-zero input cleanup failed", true);
        return;
    }
    ResetInputPollAttackEdgeJournal(1);
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
    // A baseline may intentionally contain an in-progress action.  It is start
    // state, not a player-authored step, until it exits and a fresh instance is
    // observed after recording begins.
    g_recLastAttackId = g_recPrevMove;
    g_recCountInView.store(0, std::memory_order_release);
    g_recCountInTargetTicks = 0;
    g_recCountInElapsed = false;
    g_recActive.store(true, std::memory_order_release);
    g_recPhase.store(RecorderPhase::Recording, std::memory_order_release);
    g_recStartedThisTick = true;
    // Recording owns both streams before physical input is exposed. A button
    // held for GO becomes the first macro poll instead of contaminating the
    // saved baseline.
    SetRecorderInputLease(false);
    LogOut("[MISSION][REC] synchronized P1 step+input capture started", true);
}

void FinishRecorderStopAfterCapture(bool macroTickAdvanced) {
    if (!g_recStopPending ||
        g_recPhase.load(std::memory_order_acquire) != RecorderPhase::Recording) {
        return;
    }
    // The request is consumed after MacroController::Tick in this monitor pass;
    // never mistake that already-finished tick for the requested seal boundary.
    if (!g_recStopSawPostRequestTick) {
        g_recStopSawPostRequestTick = true;
        return;
    }
    constexpr int kStopSealWatchdogTicks = 384; // 2s at monitor cadence
    const bool paused = PauseIntegration::IsPausedOrFrozen();
    bool sealedByWatchdog = false;
    if (!macroTickAdvanced) {
        // A paused/frozen pass cannot be the promised post-request macro
        // boundary. Wait until the recorder handoff releases the world; the
        // watchdog is intentionally running-game-only.
        if (paused) return;
        ++g_recStopWaitTicks;
        sealedByWatchdog =
            ::Mission::SequencePolicy::RecorderStopWaitExpired(
                macroTickAdvanced, paused, g_recStopWaitTicks,
                kStopSealWatchdogTicks);
        if (!sealedByWatchdog) return;
        LogOut("[MISSION][REC] stop watchdog expired before a new macro tick", true);
    }

    // MacroController already ticked for this monitor pass. Set and consume the
    // boundary flag synchronously so FinishRecording(false) cannot append raw
    // ring tail bytes beyond the recipe watermark.
    MacroController::RequestRecordingStopAtBoundary();
    std::string sealedDemo;
    if (!MacroController::FinishPlayerRecordingAndSerialize(sealedDemo, true)) {
        g_recStopPending = false;
        g_recStopWaitTicks = 0;
        g_recTakeIntegrityValid = false;
        g_recTakeIntegrityError = "the demonstration input stream did not seal";
        g_recPreviewAvailableView.store(false, std::memory_order_release);
        g_recActive.store(false, std::memory_order_release);
        g_recPhase.store(RecorderPhase::Review, std::memory_order_release);
        ::Mission::PauseMenu::NotifyRecorderEnteredReview();
        ClearRecorderCaptureMenuSuspension();
        ShowRecorderState("Input capture could not seal cleanly; retake is recommended",
                          RGB(255, 150, 120), 2200);
        LogOut("[MISSION][REC] synchronized macro seal failed", true);
        return;
    }
    g_recSealedDemo = std::move(sealedDemo);
    if (sealedByWatchdog) {
        g_recTakeIntegrityValid = false;
        g_recTakeIntegrityError =
            "the input stream stopped advancing before the requested seal boundary";
    }
    std::string demoError;
    constexpr char kMacroHeader[] = "EFZMACRO 1";
    const std::size_t payload = g_recSealedDemo.find_first_not_of(
        " \t\r\n", sizeof(kMacroHeader) - 1);
    if (payload == std::string::npos ||
        !MacroController::ValidateSerialized(g_recSealedDemo, demoError)) {
        g_recTakeIntegrityValid = false;
        g_recTakeIntegrityError = demoError.empty()
            ? "the recorded actions have no matching input stream"
            : "the demonstration input stream is invalid: " + demoError;
        g_recSealedDemo.clear();
    }
    g_recPreviewAvailableView.store(!g_recSealedDemo.empty(),
                                    std::memory_order_release);
    g_recStopPending = false;
    g_recStopWaitTicks = 0;
    g_recActive.store(false, std::memory_order_release);
    g_recPhase.store(RecorderPhase::Review, std::memory_order_release);
    ::Mission::PauseMenu::NotifyRecorderEnteredReview();
    ClearRecorderCaptureMenuSuspension();
    LogOut("[MISSION][REC] synchronized capture stopped for review: " +
           std::to_string(g_recSteps.size()) + " step(s)", true);
}

void ResetRunForDemo() {
    g_runPhase = RunnerPhase::Idle;
    g_runStep = 0;
    g_runFailedStep.store(-1, std::memory_order_release);
    g_runFailedEntityRequirement.store(-1, std::memory_order_release);
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
    g_runAwaitingComboEndAfterSequence = 0;
    g_runPrevHitState = 0;
    g_runRetryDelay = 0;
    ResetRunContactEvidence();
    ResetRunEntityEvidence();
    std::string lineageError;
    if (!InitializeRunEntityLineageBaseline(lineageError)) {
        LogOut("[MISSION][RUN][ENTITY] demo-return lineage baseline failed: " +
               lineageError, true);
    }
    ResetRunScore();
}

void HoldDemoP1Neutral(bool hold) {
    g_pollOverrideMask[1].store(0, std::memory_order_relaxed);
    g_pollOverrideActive[1].store(hold, std::memory_order_release);
}

void SetDemoTransitionFreeze(bool freeze) {
    const bool changed = freeze != g_demoTransitionFreezeOwned;
    g_demoTransitionFreezeOwned = freeze;
    // Visibility updates are idempotent. Always publish them so a global
    // emergency surface close cannot desynchronize this local owner bit.
    PauseIntegration::OnMenuSurfaceVisibilityChanged(
        PauseIntegration::MenuSurface::DemoTransition, freeze);
    if (changed) {
        LogOut(std::string("[MISSION][DEMO][TRANSITION] world freeze ") +
                   (freeze ? "acquired" : "released"),
               true);
    }
}

void ReassertDemoTransitionFreeze() {
    if (!g_demoTransitionFreezeOwned) return;
    // Both Revival's checkpoint load and the embedded state-dump load restore
    // the Practice pause byte/patch state from the saved world.  The logical
    // surface survives that load, so immediately rebuild its physical pause
    // before either fighter or any entity can receive another world update.
    // Preserve the existing physical ownership bookkeeping when the load kept
    // the pause intact. Reasserting an already-paused official owner would
    // misclassify it as an external pause and leave the demo frozen on release.
    if (!PauseIntegration::IsPausedOrFrozen()) {
        PauseIntegration::ReassertMenuSurfacePause(
            PauseIntegration::MenuSurface::DemoTransition);
    }
}

// Clear only the terminal tutorial handoff. While a new demonstration is
// replacing it, releaseNeutral is false so ownership moves without exposing a
// physical-input poll. Unload/session-reset paths pass true because no owner
// follows.
void ClearDemoTutorialHandoff(bool releaseNeutral) {
    if (releaseNeutral) {
        if (g_demoRestoreResult != Demo::RestoreResult::None) {
            HoldDemoP1Neutral(false);
        }
        SetDemoTransitionFreeze(false);
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
    std::lock_guard<std::recursive_mutex> lock(g_runStateMx);
    if (hold) {
        g_runStartupInputHeld.store(true, std::memory_order_release);
        SetP1StartupNeutralGate(true);
        return;
    }
    g_runStartupInputHeld.store(false, std::memory_order_release);
    SetP1StartupNeutralGate(false);
}

void AcquirePendingStartupInputHold() {
    std::lock_guard<std::recursive_mutex> lock(g_runStateMx);
    const bool newlyOwned =
        !g_pendingStartupInputHeld.exchange(true, std::memory_order_acq_rel);
    SetRunnerStartupInputHold(true);
    if (newlyOwned) {
        LogOut("[MISSION][PENDING][INPUT] P1 neutral acquired before leaving title",
               true);
    }
}

void ReleasePendingStartupInputHold(const char* reason) {
    std::lock_guard<std::recursive_mutex> lock(g_runStateMx);
    if (!g_pendingStartupInputHeld.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    SetRunnerStartupInputHold(false);
    LogOut(std::string("[MISSION][PENDING][INPUT] P1 neutral released reason=") +
               (reason && *reason ? reason : "unspecified"),
           true);
}

bool HandoffPendingStartupInputHold() {
    std::lock_guard<std::recursive_mutex> lock(g_runStateMx);
    const bool held =
        g_pendingStartupInputHeld.exchange(false, std::memory_order_acq_rel);
    if (held) {
        // Do not touch the dedicated gate. LoadPrepared adopts the existing
        // runner hold synchronously and keeps it through baseline
        // restore/save, so there is no physical-input poll between owners.
        LogOut("[MISSION][PENDING][INPUT] P1 neutral handed to runner", true);
    }
    return held;
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
    // Stop the live world before retiring the playback lane.  The next
    // monitor sample may be several native battle calls away; without this
    // transition owner the dummy, projectiles, and timers could advance past
    // the clip's end before the baseline load ran.
    SetDemoTransitionFreeze(true);
    if (MacroController::GetState() != MacroController::State::Idle) MacroController::Stop();
    g_demoBaselineIssued = false;
    g_demoRoundRestoreSettleTicks = 0;
    g_demoTerminalRecipeHoldTicks = 0;
    g_demoRecipeSettleTicksRemaining = 0;
    g_demoPhase.store(DemoPhase::Restoring, std::memory_order_release);
    HoldDemoP1Neutral(true);
}

void BeginDemoTerminalRecipeHold() {
    // The input stream has ended and the recipe reached a terminal state.
    // Freeze that exact resolved world without leaving Playing, allowing the
    // render thread to present the final green/red cursor before restore.
    HoldDemoP1Neutral(true);
    SetDemoTransitionFreeze(true);
    g_demoRecipeSettleTicksRemaining = 0;
    g_demoTerminalRecipeHoldTicks =
        ::Mission::SequencePolicy::kDemoTerminalRecipeHoldTicks;
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
        SetDemoTransitionFreeze(false);
        g_demoText.clear();
        g_demoFromRecorder = false;
        g_demoBaselineIssued = false;
        g_demoRoundRestoreSettleTicks = 0;
        g_demoTerminalRecipeHoldTicks = 0;
        g_demoRecipeSettleTicksRemaining = 0;
        g_demoPhase.store(DemoPhase::Idle, std::memory_order_release);
        // This Snapshot predates the terminal transition. It is never the
        // sample from which runner/tutorial validation resumes.
        g_demoFinishedThisTick = true;
        LogOut("[MISSION][DEMO] restore handoff failed: " + error, true);
        return;
    }

    if (phase == DemoPhase::Playing) {
        const bool trialRecipeActive =
            ::Mission::SequencePolicy::DemoTrialRecipeActive(
                /*demoPlaying=*/true, g_demoFromRecorder,
                g_runActive.load(std::memory_order_acquire),
                !g_runMission.steps.empty());
        if (g_demoTerminalRecipeHoldTicks > 0) {
            if (--g_demoTerminalRecipeHoldTicks == 0) {
                BeginDemoRestoring();
                LogOut("[MISSION][DEMO] final recipe frame shown; restoring player-ready baseline",
                       true);
            }
            return;
        }
        using ::Mission::SequencePolicy::DemoRoundEventAction;
        const bool roundEventActive = RoundIntroActive();
        int p1Hp = 0;
        int p2Hp = 0;
        const uintptr_t liveP1 = GetPlayerBase(1);
        const uintptr_t liveP2 = GetPlayerBase(2);
        const bool bothFightersAlive = liveP1 && liveP2 &&
            SafeReadMemory(liveP1 + HP_OFFSET, &p1Hp, sizeof(p1Hp)) &&
            SafeReadMemory(liveP2 + HP_OFFSET, &p2Hp, sizeof(p2Hp)) &&
            p1Hp > 0 && p2Hp > 0;
        const DemoRoundEventAction roundAction =
            ::Mission::SequencePolicy::DecideDemoRoundEvent(
                roundEventActive, g_demoRoundRestoreSettleTicks,
                bothFightersAlive);
        if (roundAction == DemoRoundEventAction::NormalizeRestoreTransient) {
            SkipRoundIntro("demonstration restore settle");
            LogOut("[MISSION][DEMO] normalized post-restore round-event "
                   "transient; settleTicks=" +
                       std::to_string(g_demoRoundRestoreSettleTicks) +
                       " hp=" + std::to_string(p1Hp) + "/" +
                       std::to_string(p2Hp),
                   true);
        } else if (roundAction == DemoRoundEventAction::RestoreBaseline) {
            BeginDemoRestoring();
            LogOut("[MISSION][DEMO] round transition detected; restoring baseline", true);
            return;
        }
        if (g_demoRoundRestoreSettleTicks > 0) {
            --g_demoRoundRestoreSettleTicks;
        }
        // Observe the exact same action/contact/entity evidence as a live
        // attempt so the on-screen trial bar follows the demonstrated route.
        // This runs before the Idle check to retain the clip's final closed
        // resolver batch. Recorder Review previews intentionally have no
        // loaded-runner cursor and remain presentation-free.
        if (trialRecipeActive) {
            TickRun(s, RunObservationMode::DemoPresentation);
        }
        const bool macroIdle =
            MacroController::GetState() == MacroController::State::Idle;
        if (!trialRecipeActive) {
            if (macroIdle) {
                BeginDemoRestoring();
                LogOut("[MISSION][DEMO] clip ended; restoring player-ready baseline",
                       true);
            }
            return;
        }

        const bool runnerTerminal =
            g_runPhase == RunnerPhase::Complete ||
            g_runPhase == RunnerPhase::Dropped;
        using ::Mission::SequencePolicy::DemoRecipeEndAction;
        switch (::Mission::SequencePolicy::DecideDemoRecipeEnd(
                    macroIdle, runnerTerminal,
                    g_demoRecipeSettleTicksRemaining)) {
            case DemoRecipeEndAction::ContinuePlayback:
                break;
            case DemoRecipeEndAction::HoldTerminalFrame:
                BeginDemoTerminalRecipeHold();
                LogOut("[MISSION][DEMO] trial recipe resolved; holding final frame",
                       true);
                break;
            case DemoRecipeEndAction::SettleFinalContacts:
                // The final command may only just have activated. Keep the
                // world live but P1 neutral while startup, a slow projectile,
                // or a summon contact completes the authored route.
                HoldDemoP1Neutral(true);
                if (g_demoRecipeSettleTicksRemaining <= 0) {
                    g_demoRecipeSettleTicksRemaining =
                        ::Mission::SequencePolicy::
                            kDemoRecipeSettleTimeoutTicks;
                    LogOut("[MISSION][DEMO] input clip ended; waiting for final trial contacts",
                           true);
                } else {
                    --g_demoRecipeSettleTicksRemaining;
                }
                break;
            case DemoRecipeEndAction::FailThenHold:
                LogOut("[MISSION][DEMO][RECIPE] final-contact settle timed out",
                       true);
                {
                    ScopedRunObservationMode observationScope(
                        RunObservationMode::DemoPresentation);
                    RunnerDrop();
                }
                BeginDemoTerminalRecipeHold();
                break;
        }
        return;
    }

    HoldDemoP1Neutral(true);
    if (ImGuiImpl::IsVisible() || ::Mission::Setup::IsPending() ||
        CharacterHotswap::IsBusy() ||
        (!g_demoFromRecorder && (g_runRestorePending || g_runSavePending))) {
        return;
    }

    if (!g_demoBaselineIssued) {
        // The request path acquires this before publishing Preparing and the
        // restore path acquires it before stopping playback.  Reclaim it here
        // defensively if a lifecycle transition recreated the owner state.
        SetDemoTransitionFreeze(true);
        if (phase == DemoPhase::Preparing) {
            CancelAutoActionsAndMacros();
            HoldDemoP1Neutral(true);
        }
        std::string err;
        // Conservatively invalidate the pre-transaction Snapshot before the
        // restore call. A restore may load successfully and then fail a
        // post-load verification, in which case its boolean result alone
        // cannot tell us that the live world already changed.
        g_worldRestoredThisTick = true;
        const bool restored = RestoreDemoBaseline(err);
        // The savestate load preamble clears poll overrides; reclaim P1 in the
        // same call so physical input never gets a post-restore poll window.
        // It can also restore an unpaused Practice flag and old patch bytes,
        // so rebuild the transition pause before returning to the live world.
        HoldDemoP1Neutral(true);
        ReassertDemoTransitionFreeze();
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
            SetDemoTransitionFreeze(false);
            g_demoText.clear();
            g_demoFromRecorder = false;
            g_demoBaselineIssued = false;
            g_demoRoundRestoreSettleTicks = 0;
            g_demoTerminalRecipeHoldTicks = 0;
            g_demoRecipeSettleTicksRemaining = 0;
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
    }

    if (phase == DemoPhase::Preparing) {
        std::string err;
        // Tick zero is published while the restored world remains physically
        // frozen.  Releasing the transition surface only after this call
        // means the first native P1 poll after restore sees the recorded byte,
        // never an artificial neutral frame. Ordinary Practice macros retain
        // their historical deferred-start mode.
        if (!MacroController::PlaySerializedForPlayer(
                g_demoText, 1, true, err,
                MacroController::PlaybackStartMode::PrimeBeforeNextPoll)) {
            BeginDemoRestoring();
            DirectDrawHook::AddMessage(("Demo failed: " + err).c_str(), "MISSION",
                                       RGB(255, 120, 120), 2200, 0, 120);
            return;
        }
        g_demoPhase.store(DemoPhase::Playing, std::memory_order_release);
        g_demoRoundRestoreSettleTicks =
            ::Mission::SequencePolicy::kDemoRoundRestoreSettleTicks;
        SetDemoTransitionFreeze(false);
        LogOut("[MISSION][DEMO] exact baseline restored; P1 tick 0 primed; world released",
               true);
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
            SetDemoTransitionFreeze(false);
        }
        g_demoText.clear();
        g_demoBaselineIssued = false;
        g_demoRoundRestoreSettleTicks = 0;
        g_demoTerminalRecipeHoldTicks = 0;
        g_demoRecipeSettleTicksRemaining = 0;
        g_demoPhase.store(DemoPhase::Idle, std::memory_order_release);
        // This is exclusively a stale-Snapshot guard. Tutorial ownership is
        // carried by g_demoRestoreResult until explicit acknowledgement.
        g_demoFinishedThisTick = true;
        DirectDrawHook::AddMessage(recorderPreview
                                       ? "Preview finished - back to Review"
                                       : "Demonstration finished - your turn", "MISSION",
                                   RGB(180, 255, 220), 1800, 0, 120);
        if (!recorderPreview) {
            DirectDrawHook::RemoveMessagesByCategory("mission_run");
        }
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

const char* RecContactSourceName(::Mission::Contact::Source source) {
    switch (source) {
        case ::Mission::Contact::Source::DirectPlayer: return "direct";
        case ::Mission::Contact::Source::Entity:       return "entity";
        default:                                       return "none";
    }
}

void WriteNullableInt(std::ostream& out, int value) {
    if (value < 0) out << "null";
    else out << value;
}

void WriteFiniteDouble(std::ostream& out, double value) {
    if (std::isfinite(value)) out << value;
    else out << "null";
}

std::string RecorderEntityTracePathForMission(const std::string& missionPath) {
    std::string path = missionPath;
    const std::size_t dot = path.find_last_of('.');
    if (dot != std::string::npos) path.resize(dot);
    path += ".entities.jsonl";
    return path;
}

bool WriteRecorderEntityTraceSidecarAtPath(const std::string& outputPath,
                                           std::string& outError) {
    outError.clear();
    std::ofstream out(outputPath, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        outError = "cannot create " + outputPath;
        return false;
    }

    const std::string p1Resource = !g_recSetup.player.character.empty()
        ? g_recSetup.player.character
        : CharacterSettings::GetCharacterInternalName(P1Char());
    const std::string p2Resource = !g_recSetup.dummy.character.empty()
        ? g_recSetup.dummy.character
        : CharacterSettings::GetCharacterInternalName(PlayerChar(2));
    out << "{\"record\":\"metadata\",\"schema\":\"efz_recorder_entity_trace\""
           ",\"version\":5,\"authoring_only\":true,\"sampled\":true"
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
        << ",\"entityContactHookComplete\":"
        << (g_recEntityContactHookComplete ? "true" : "false")
        << ",\"contactEpochDiscontinuity\":"
        << (g_recContactEpochDiscontinuity ? "true" : "false")
        << ",\"directContactHookObserved\":"
        << (g_recDirectContactHookObserved ? "true" : "false")
        << ",\"contactJournalOverflow\":"
        << (g_recContactJournalOverflowObserved ? "true" : "false")
        << ",\"p1EntityProbeIncomplete\":"
        << (g_recEntityProbeIncomplete[0] ? "true" : "false")
        << ",\"p2EntityProbeIncomplete\":"
        << (g_recEntityProbeIncomplete[1] ? "true" : "false")
        << ",\"p1AllocationCursorAmbiguous\":"
        << (g_recEntityAllocationAmbiguous[0] ? "true" : "false")
        << ",\"p2AllocationCursorAmbiguous\":"
        << (g_recEntityAllocationAmbiguous[1] ? "true" : "false")
        << ",\"requiresEntityAttributionReview\":"
        << (g_recEntityAttributionRequired ? "true" : "false")
        << ",\"containsUnclassifiedContact\":"
        << (g_recUnclassifiedContactRequired ? "true" : "false")
        << ",\"usedLegacyContactAttribution\":"
        << (g_recLegacyContactAttributionRequired ? "true" : "false")
        << "}\n";

    for (std::size_t i = 0; i < g_recSteps.size(); ++i) {
        const CapturedStep& step = g_recSteps[i];
        out << "{\"record\":\"action_order\",\"step\":" << i
            << ",\"move\":" << step.moveId
            << ",\"expectedAttackMask\":"
            << static_cast<unsigned int>(step.expectedAttackMask)
            << ",\"effectiveFrame\":" << step.startFrame
            << ",\"battleBatch\":" << step.startBattleBatch
            << ",\"actionOrder\":" << step.startActionOrder
            << "}\n";
    }

    for (std::size_t i = 0; i < g_recEntityTraceCount; ++i) {
        const RecEntityTraceEvent& event = g_recEntityTrace[i];
        const std::string eventResource =
            CharacterSettings::GetCharacterInternalName(event.characterId);
        const char* className = ::Mission::MoveData::ClassName(event.entityClass);
        if (!className || !className[0]) className = "unknown";
        const bool isContact = event.kind == RecEntityTraceKind::Contact;

        out << "{\"record\":\""
            << (isContact ? "contact" : "entity_lifecycle")
            << "\",\"event\":\"" << RecEntityTraceKindName(event.kind) << "\""
            << ",\"evidence\":\""
            << (isContact ? "resolver_hook" : "ring_sample") << "\""
            << ",\"authoring_only\":true"
            << ",\"sampled\":" << (event.sampled ? "true" : "false")
            << ",\"unordered\":" << (isContact ? "false" : "true")
            << ",\"non_strict\":true"
            << ",\"effectiveFrame\":" << event.effectiveFrame
            << ",\"sampleSerial\":" << event.sampleSerial
            << ",\"characterId\":" << event.characterId
            << ",\"resource\":\"" << JsonEscape(eventResource) << "\""
            << ",\"player\":" << event.player << ",\"slot\":";
        WriteNullableInt(out, event.slot);
        out << ",\"generation\":" << event.generation
            << ",\"pattern\":";
        WriteNullableInt(out, event.pattern);
        out << ",\"priorPattern\":";
        WriteNullableInt(out, event.priorPattern);
        out << ",\"class\":\"" << className << "\""
            << ",\"attack\":" << (event.attack ? "true" : "false")
            << ",\"frame\":" << event.entityFrame
            << ",\"frameTick\":" << event.entityFrameTick
            << ",\"x\":";
        WriteFiniteDouble(out, event.x);
        out << ",\"y\":";
        WriteFiniteDouble(out, event.y);
        if (!isContact) {
            out << ",\"life\":" << event.life
                << ",\"destroyed\":" << event.destroyed;
        }
        out << ",\"observedAfterStep\":" << event.afterStep
            << ",\"observedAfterActionOrder\":"
            << event.afterActionOrder
            << ",\"sampleBasis\":\""
            << (isContact ? "resolver_transaction" :
                event.kind == RecEntityTraceKind::Despawn
                    ? "last_alive_sample" : "current_sample")
            << "\"";
        if (isContact) {
            out << ",\"sequence\":" << event.contactSequence
                << ",\"worldEpoch\":" << event.contactWorldEpoch
                << ",\"batch\":" << event.contactBatch
                << ",\"source\":\""
                << RecContactSourceName(event.contactSource) << "\""
                << ",\"attackerMove\":" << event.attackerMove
                << ",\"attributedStep\":" << event.attributedStep
                << ",\"attackerFrame\":" << event.entityFrame
                << ",\"defender\":" << event.defender
                << ",\"result\":\""
                << RecContactResultName(event.contactResult) << "\""
                << ",\"timerBefore\":" << event.lifeBefore
                << ",\"timerAfter\":" << event.life
                << ",\"rawStateBefore\":" << event.destroyedBefore
                << ",\"rawStateAfter\":" << event.destroyed
                << ",\"comboBefore\":" << event.comboBefore
                << ",\"comboAfter\":" << event.comboAfter
                << ",\"hpBefore\":" << event.hpBefore
                << ",\"hpAfter\":" << event.hpAfter
                << ",\"comboEndAfter\":"
                << (event.comboEndAfter ? "true" : "false");
        }
        out << "}\n";
    }
    out.flush();
    if (!out.good()) {
        outError = "write failed for " + outputPath;
        return false;
    }
    return true;
}

// Claim a mission basename with a non-JSON sentinel before serialization.
// The browser-visible .json itself is promoted last, after its entity trace,
// so a process exit cannot expose an empty reservation or a mission missing
// its required authoring evidence. Timestamp + tick + a process-local sequence
// makes collisions exceptional; CREATE_NEW remains the authority.
bool ReserveRecordedMissionPath(const std::string& dir,
                                std::string& outPath,
                                std::string& outReservationPath,
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
        const std::string reservation = candidate + ".reserve";
        HANDLE file = CreateFileA(reservation.c_str(), GENERIC_WRITE, 0, nullptr,
                                  CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            CloseHandle(file);
            const std::string trace = RecorderEntityTracePathForMission(candidate);
            const DWORD missionAttributes = GetFileAttributesA(candidate.c_str());
            const DWORD missionError = missionAttributes == INVALID_FILE_ATTRIBUTES
                ? GetLastError() : ERROR_SUCCESS;
            const DWORD traceAttributes = GetFileAttributesA(trace.c_str());
            const DWORD traceError = traceAttributes == INVALID_FILE_ATTRIBUTES
                ? GetLastError() : ERROR_SUCCESS;
            const bool missionFree = missionAttributes == INVALID_FILE_ATTRIBUTES &&
                (missionError == ERROR_FILE_NOT_FOUND || missionError == ERROR_PATH_NOT_FOUND);
            const bool traceFree = traceAttributes == INVALID_FILE_ATTRIBUTES &&
                (traceError == ERROR_FILE_NOT_FOUND || traceError == ERROR_PATH_NOT_FOUND);
            if (!missionFree || !traceFree) {
                DeleteFileA(reservation.c_str());
                if (missionAttributes != INVALID_FILE_ATTRIBUTES ||
                    traceAttributes != INVALID_FILE_ATTRIBUTES) {
                    continue;
                }
                outError = "cannot inspect a recorded mission destination "
                           "(Windows error " +
                    std::to_string(static_cast<unsigned long>(
                        !missionFree ? missionError : traceError)) + ")";
                return false;
            }
            outPath = candidate;
            outReservationPath = reservation;
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

bool LegacyRecordedSidecarHasEntityContacts(const std::string& sourcePath) {
    if (sourcePath.find("\\_recorded\\") == std::string::npos &&
        sourcePath.find("/_recorded/") == std::string::npos) {
        return false;
    }
    const std::size_t dot = sourcePath.find_last_of('.');
    if (dot == std::string::npos) return false;
    const std::string sidecar =
        sourcePath.substr(0, dot) + ".entities.jsonl";
    std::ifstream in(sidecar, std::ios::binary);
    if (!in) return false;
    std::string line;
    while (std::getline(in, line)) {
        const bool legacyEntityContact =
            line.find("\"record\":\"entity_contact\"") != std::string::npos;
        const bool currentEntityContact =
            line.find("\"record\":\"contact\"") != std::string::npos &&
            line.find("\"source\":\"entity\"") != std::string::npos;
        if (legacyEntityContact || currentEntityContact) return true;
    }
    return false;
}

// This is the complete data/capability boundary shared by ordinary in-match
// loads and title-prepared loads. A title transaction is not published until
// this succeeds, so Match entry only has to publish the already accepted value
// object; it never reparses or discovers a late lesson-capability failure.
bool ValidateMissionForRuntime(const ::Mission::Mission& mission,
                               const std::string& sourcePath,
                               std::string& outError) {
    if (mission.format != 1) {
        outError = "unsupported mission format " +
            std::to_string(mission.format) + ": " + sourcePath;
        return false;
    }
    if (!::Mission::SequencePolicy::EmbeddedSavestateCanLoad(
            !mission.savestate.empty(), ::Mission::StateDump::Available())) {
        outError = "mission carries an exact start state, but savestate "
                   "restore is unavailable";
        return false;
    }
    for (std::size_t stepIndex = 0;
         stepIndex < mission.steps.size(); ++stepIndex) {
        if (!::Mission::SequencePolicy::ValidExpectedAttackMask(
                mission.steps[stepIndex].expectedAttackMask)) {
            outError = "step " + std::to_string(stepIndex) +
                " has invalid expectedAttackMask (A/B/C/D bits only)";
            return false;
        }
    }
    int entityScheduleMarkerVersion = 0;
    int entityScheduleMarkerCount = 0;
    std::vector<std::string> authorReview;
    for (const std::string& reason : mission.reviewRequired) {
        if (reason == kEntityScheduleRuntimeMarkerV1 ||
            reason == kEntityScheduleRuntimeMarkerV2 ||
            reason == kEntityScheduleRuntimeMarkerV3 ||
            reason == kEntityScheduleRuntimeMarkerV4 ||
            reason == kEntityScheduleRuntimeMarkerV5) {
            ++entityScheduleMarkerCount;
            const int version = reason == kEntityScheduleRuntimeMarkerV5
                ? 5 : reason == kEntityScheduleRuntimeMarkerV4
                ? 4 : reason == kEntityScheduleRuntimeMarkerV3
                    ? 3 : reason == kEntityScheduleRuntimeMarkerV2 ? 2 : 1;
            if (entityScheduleMarkerVersion == 0) {
                entityScheduleMarkerVersion = version;
            } else if (entityScheduleMarkerVersion != version) {
                entityScheduleMarkerVersion = -1;
            }
        } else if (::Mission::SequencePolicy::
                       IsLegacyUnmatchedInputDiagnostic(reason)) {
            // Compatibility migration is intentionally read-only. A later
            // editor save can move this text to recordingDiagnostics, but an
            // existing valid recording must not be blocked at launch.
            continue;
        } else if (::Mission::EntityReviewPolicy::
                       IsObsoletePostContactReview(mission, reason)) {
            LogOut("[MISSION][ENTITY] ignored obsolete post-contact review: " +
                   reason, true);
            continue;
        } else {
            authorReview.push_back(reason);
        }
    }
    if (!authorReview.empty()) {
        outError = "mission needs author review: ";
        for (std::size_t i = 0; i < authorReview.size(); ++i) {
            if (i) outError += ", ";
            outError += authorReview[i];
        }
        return false;
    }
    if (entityScheduleMarkerCount > 1 || entityScheduleMarkerVersion < 0) {
        outError = "entity-contact schedule has ambiguous runtime markers";
        return false;
    }
    const bool hasEntitySchedule = !mission.entityContacts.empty() ||
                                   !mission.entityLifecycles.empty();
    const bool hasEntityScheduleMarker = entityScheduleMarkerVersion > 0;
    if (hasEntitySchedule != hasEntityScheduleMarker ||
        mission.strictEntityContacts != hasEntitySchedule) {
        outError = "entity schedule is missing its strict runtime marker";
        return false;
    }
    if (!mission.entityLifecycles.empty() &&
        entityScheduleMarkerVersion != 4 &&
        entityScheduleMarkerVersion != 5) {
        outError = "entity lifecycle objectives require the v4 or v5 runtime marker";
        return false;
    }
    const bool hasFlexibleFanout = std::any_of(
        mission.entityContacts.begin(), mission.entityContacts.end(),
        [](const ::Mission::EntityContactRequirement& requirement) {
            return !requirement.fanoutMembers.empty();
        });
    if (hasFlexibleFanout && entityScheduleMarkerVersion != 5) {
        outError = "flexible entity fanout requires the v5 runtime marker";
        return false;
    }
    if (mission.entityContacts.empty() &&
        LegacyRecordedSidecarHasEntityContacts(sourcePath)) {
        outError = "legacy recorded mission contains uncompiled entity contacts; retake it";
        return false;
    }
    if (entityScheduleMarkerVersion >= 3 && hasEntitySchedule &&
        mission.savestate.empty()) {
        outError = "strict entity lineage requires an exact embedded start state";
        return false;
    }
    if (hasEntitySchedule && !IsEntityContactHookReady()) {
        outError = "mission requires exact entity tracking, but the collision "
                   "hook is unavailable";
        return false;
    }
    if (!mission.entityContacts.empty()) {
        const bool usesV2Ordering = std::any_of(
            mission.entityContacts.begin(), mission.entityContacts.end(),
            [](const ::Mission::EntityContactRequirement& requirement) {
                return ::Mission::EntitySchedulePolicy::RequiresV2Ordering(
                    requirement.contactAfterAction,
                    requirement.afterStepContact,
                    requirement.dueBeforeStepContact);
            });
        if (!::Mission::EntitySchedulePolicy::RuntimeMarkerSupportsOrdering(
                entityScheduleMarkerVersion, usesV2Ordering)) {
            outError = "entity-contact ordinal/action ordering requires the v2 runtime marker";
            return false;
        }
        const bool usesV3Lineage = std::any_of(
            mission.entityContacts.begin(), mission.entityContacts.end(),
            [](const ::Mission::EntityContactRequirement& requirement) {
                return !requirement.producerLifecycle.empty() ||
                       requirement.producerPattern >= 0 ||
                       requirement.producerPriorPattern >= 0 ||
                       !requirement.fanoutMembers.empty();
            });
        if (!::Mission::EntitySchedulePolicy::RuntimeMarkerSupportsLineage(
                entityScheduleMarkerVersion, usesV3Lineage)) {
            outError = "entity producer lineage requires the v3 runtime marker";
            return false;
        }
        const int totalSteps = static_cast<int>(mission.steps.size());
        const int stepComboBoundaryCount = static_cast<int>(std::count_if(
            mission.steps.begin(), mission.steps.end(),
            [](const ::Mission::Step& step) { return step.comboEndAfter; }));
        const int entityComboBoundaryCount = static_cast<int>(std::count_if(
            mission.entityContacts.begin(), mission.entityContacts.end(),
            [](const ::Mission::EntityContactRequirement& requirement) {
                return requirement.comboEndAfter;
            }));
        const int comboBoundaryCount =
            stepComboBoundaryCount + entityComboBoundaryCount;
        int priorDueNormalized = -1;
        int priorDueContact = 0;
        int priorSegment = 0;
        int entityBoundariesBefore = 0;
        bool priorRequirementEndsCombo = false;
        for (std::size_t i = 0; i < mission.entityContacts.size(); ++i) {
            const auto& requirement = mission.entityContacts[i];
            const auto validStepIndex = [totalSteps](int index) {
                return index >= -1 && index < totalSteps;
            };
            if (!::Mission::SemanticSourcePolicy::ValidPersistedPair(
                    requirement.semanticSourceAction,
                    requirement.semanticSourceMove)) {
                outError = "entity-contact schedule entry " +
                    std::to_string(i) +
                    " has invalid semantic source provenance";
                return false;
            }
            const bool exactSemanticSource =
                ::Mission::SemanticSourcePolicy::IsExact(
                    requirement.semanticSourceAction,
                    requirement.semanticSourceMove);
            if (exactSemanticSource) {
                if (requirement.semanticSourceAction >= totalSteps ||
                    std::find(
                        mission.steps[static_cast<std::size_t>(
                            requirement.semanticSourceAction)].moveIds.begin(),
                        mission.steps[static_cast<std::size_t>(
                            requirement.semanticSourceAction)].moveIds.end(),
                        requirement.semanticSourceMove) ==
                        mission.steps[static_cast<std::size_t>(
                            requirement.semanticSourceAction)].moveIds.end() ||
                    (requirement.opensAfterAction >= 0 &&
                     requirement.semanticSourceAction >
                         requirement.opensAfterAction) ||
                    (requirement.contactAfterAction >= 0 &&
                     requirement.semanticSourceAction >
                         requirement.contactAfterAction)) {
                    outError = "entity-contact schedule entry " +
                        std::to_string(i) +
                        " has semantic source outside its exact action/gates";
                    return false;
                }
            }
            bool validPatterns = !requirement.patterns.empty();
            std::vector<int> uniquePatterns;
            for (int pattern : requirement.patterns) {
                if (pattern <= 0 || pattern > 65535 ||
                    std::find(uniquePatterns.begin(), uniquePatterns.end(), pattern) !=
                        uniquePatterns.end()) {
                    validPatterns = false;
                } else {
                    uniquePatterns.push_back(pattern);
                }
            }
            int nonContactPattern = -1;
            const int semanticProducerMove = exactSemanticSource
                ? requirement.semanticSourceMove
                : ::Mission::SemanticSourcePolicy::IsLegacyAbsent(
                      requirement.semanticSourceAction,
                      requirement.semanticSourceMove) &&
                  requirement.opensAfterAction >= 0 &&
                requirement.opensAfterAction < totalSteps &&
                !mission.steps[static_cast<std::size_t>(
                     requirement.opensAfterAction)].moveIds.empty()
                    ? mission.steps[static_cast<std::size_t>(
                          requirement.opensAfterAction)].moveIds.front()
                    : -1;
            if (validPatterns) {
                for (int pattern : requirement.patterns) {
                    const auto* semantic =
                        ::Mission::EntityNames::LookupSemantic(
                            mission.player.character.c_str(), pattern,
                            semanticProducerMove);
                    if (semantic &&
                        !::Mission::EntityNames::CanOwnRecordedContact(
                            semantic->disposition)) {
                        nonContactPattern = pattern;
                        break;
                    }
                }
            }
            if (nonContactPattern >= 0) {
                outError = "entity-contact schedule entry " +
                    std::to_string(i) + " targets non-contact lifecycle phase #" +
                    std::to_string(nonContactPattern);
                return false;
            }
            const auto producerKind = EntityLifecycleKind(
                requirement.producerLifecycle);
            const bool flexibleFanout = !requirement.fanoutMembers.empty();
            bool validFanoutLineage = flexibleFanout;
            std::vector<std::pair<int, int>> fanoutIdentities;
            if (flexibleFanout) {
                for (const auto& member : requirement.fanoutMembers) {
                    bool validMemberPatterns = !member.patterns.empty();
                    std::vector<int> uniqueMemberPatterns;
                    for (int pattern : member.patterns) {
                        if (pattern <= 0 || pattern > 65535 ||
                            std::find(uniqueMemberPatterns.begin(),
                                      uniqueMemberPatterns.end(), pattern) !=
                                uniqueMemberPatterns.end() ||
                            !::Mission::EntityFanoutPolicy::IsFlexibleFanout(
                                mission.player.character, pattern,
                                semanticProducerMove)) {
                            validMemberPatterns = false;
                            break;
                        }
                        uniqueMemberPatterns.push_back(pattern);
                    }
                    const auto memberKind =
                        EntityLifecycleKind(member.producerLifecycle);
                    const std::pair<int, int> identity{
                        member.slot, member.generation};
                    const bool duplicateIdentity = std::find(
                        fanoutIdentities.begin(), fanoutIdentities.end(),
                        identity) != fanoutIdentities.end();
                    validFanoutLineage = validFanoutLineage &&
                        validMemberPatterns && !duplicateIdentity &&
                        member.slot >= 0 &&
                        member.slot < static_cast<int>(
                            CollisionDisplay::kProjectileRingSlotCapacity) &&
                        member.generation > 0 &&
                        member.patterns.size() == 1 &&
                        member.producerPattern == member.patterns.front() &&
                        member.contactsObserved >= 0 &&
                        member.comboHitsObserved >= 0 &&
                        member.damageObserved >= 0 &&
                        (memberKind != ::Mission::EntitySchedulePolicy::
                                           LifecycleKind::Baseline ||
                         requirement.opensAfterAction == -1) &&
                        ::Mission::EntitySchedulePolicy::
                            ValidProducerDescriptor(
                                memberKind, member.producerPattern,
                                member.producerPriorPattern);
                    fanoutIdentities.push_back(identity);
                }
                validFanoutLineage = validFanoutLineage &&
                    entityScheduleMarkerVersion == 5 &&
                    requirement.fanoutMembers.size() >= 2 &&
                    requirement.slot == -1 &&
                    requirement.generation == 0 &&
                    requirement.patterns.size() == 1 &&
                    requirement.producerLifecycle == "spawn" &&
                    requirement.producerPattern ==
                        requirement.patterns.front() &&
                    requirement.producerPriorPattern < 0 &&
                    ::Mission::EntityFanoutPolicy::IsFlexibleFanout(
                        mission.player.character,
                        requirement.patterns.front(),
                        semanticProducerMove) &&
                    requirement.result == "hit" &&
                    requirement.minimumContactsRequired == 1 &&
                    requirement.minimumComboHitsRequired == 1 &&
                    requirement.contactsRequired >=
                        requirement.minimumContactsRequired &&
                    requirement.comboHitsRequired >=
                        requirement.minimumComboHitsRequired &&
                    requirement.damage >= 0;
            }
            const bool validV3Lineage = entityScheduleMarkerVersion < 3 ||
                (flexibleFanout
                    ? validFanoutLineage
                    : (requirement.slot >= 0 &&
                       requirement.slot < static_cast<int>(
                           CollisionDisplay::kProjectileRingSlotCapacity) &&
                       requirement.generation > 0 &&
                       requirement.patterns.size() == 1 &&
                       requirement.producerPattern ==
                           requirement.patterns.front() &&
                       (producerKind != ::Mission::EntitySchedulePolicy::
                                            LifecycleKind::Baseline ||
                        requirement.opensAfterAction == -1) &&
                       ::Mission::EntitySchedulePolicy::
                           ValidProducerDescriptor(
                               producerKind, requirement.producerPattern,
                               requirement.producerPriorPattern)));
            if (requirement.owner != 1 || requirement.target != 2 ||
                !validPatterns ||
                !validV3Lineage ||
                !::Mission::Contact::ValidResultRequirement(requirement.result) ||
                requirement.result == "whiff" ||
                requirement.contactsRequired < 1 ||
                requirement.comboHitsRequired < 0 ||
                (!flexibleFanout &&
                 (requirement.minimumContactsRequired != 0 ||
                  requirement.minimumComboHitsRequired != 0)) ||
                !validStepIndex(requirement.opensAfterAction) ||
                !validStepIndex(requirement.contactAfterAction) ||
                !validStepIndex(requirement.afterStep) ||
                !validStepIndex(requirement.dueBeforeStep) ||
                requirement.afterStepContact < -1 ||
                requirement.afterStepContact == 0 ||
                requirement.dueBeforeStepContact < -1 ||
                (requirement.afterStepContact > 0 &&
                 requirement.afterStep < 0) ||
                (requirement.dueBeforeStep < 0 &&
                 requirement.dueBeforeStepContact != -1) ||
                requirement.segment < 0 || requirement.maxDelay < 1 ||
                !::Mission::EntitySchedulePolicy::BarrierWindowValid(
                    requirement.afterStep, requirement.afterStepContact,
                    requirement.dueBeforeStep,
                    requirement.dueBeforeStepContact) ||
                (requirement.dueBeforeStep >= 0 &&
                 requirement.opensAfterAction > requirement.dueBeforeStep) ||
                (requirement.opensAfterAction >= 0 &&
                 requirement.contactAfterAction >= 0 &&
                 requirement.opensAfterAction >
                     requirement.contactAfterAction) ||
                (requirement.dueBeforeStep >= 0 &&
                 requirement.contactAfterAction >
                     requirement.dueBeforeStep) ||
                (requirement.dueBeforeStepContact == 0 &&
                 requirement.contactAfterAction >=
                     requirement.dueBeforeStep) ||
                 requirement.segment > comboBoundaryCount ||
                 requirement.segment < priorSegment ||
                 (priorRequirementEndsCombo &&
                  requirement.segment <= priorSegment)) {
                outError = "invalid entity-contact schedule entry " +
                    std::to_string(i);
                return false;
            }
            if (requirement.dueBeforeStepContact > 0 &&
                (requirement.dueBeforeStep < 0 ||
                 !mission.steps[static_cast<std::size_t>(
                     requirement.dueBeforeStep)].directContact)) {
                outError = "entity-contact schedule entry " +
                    std::to_string(i) +
                    " names a non-direct due contact ordinal";
                return false;
            }
            if (requirement.dueBeforeStepContact > 0) {
                const auto& dueStep = mission.steps[static_cast<std::size_t>(
                    requirement.dueBeforeStep)];
                const int authoredContacts =
                    ::Mission::EntitySchedulePolicy::AuthoredDirectContactCount(
                        dueStep.req == ::Mission::StepReq::Hits,
                        dueStep.hitsRequired);
                if (!::Mission::EntitySchedulePolicy::
                        ContactOrdinalWithinAuthoredRequirement(
                            requirement.dueBeforeStepContact,
                            authoredContacts)) {
                    outError = "entity-contact schedule entry " +
                        std::to_string(i) +
                        " has an impossible due contact ordinal";
                    return false;
                }
            }
            if (requirement.afterStep >= 0 &&
                !mission.steps[static_cast<std::size_t>(requirement.afterStep)]
                     .directContact) {
                outError = "entity-contact schedule entry " +
                    std::to_string(i) + " names a non-direct afterStep barrier";
                return false;
            }
            if (requirement.afterStepContact > 0) {
                const auto& afterStep = mission.steps[static_cast<std::size_t>(
                    requirement.afterStep)];
                const int authoredContacts =
                    ::Mission::EntitySchedulePolicy::AuthoredDirectContactCount(
                        afterStep.req == ::Mission::StepReq::Hits,
                        afterStep.hitsRequired);
                if (!::Mission::EntitySchedulePolicy::
                        ContactOrdinalWithinAuthoredRequirement(
                            requirement.afterStepContact,
                            authoredContacts)) {
                    outError = "entity-contact schedule entry " +
                        std::to_string(i) +
                        " has an impossible after contact ordinal";
                    return false;
                }
            }
            const int dueNormalized = requirement.dueBeforeStep >= 0
                ? requirement.dueBeforeStep : totalSteps;
            int boundariesBeforeDue = entityBoundariesBefore;
            int boundariesBeforeAfter = entityBoundariesBefore;
            for (int stepIndex = 0; stepIndex < totalSteps; ++stepIndex) {
                if (!mission.steps[static_cast<std::size_t>(stepIndex)]
                         .comboEndAfter) {
                    continue;
                }
                if (stepIndex < dueNormalized) ++boundariesBeforeDue;
                if (stepIndex < requirement.afterStep) ++boundariesBeforeAfter;
            }
            if (requirement.segment > boundariesBeforeDue ||
                requirement.segment < boundariesBeforeAfter) {
                outError = "entity-contact schedule entry " +
                    std::to_string(i) + " cannot occur in its declared combo segment";
                return false;
            }
            const int dueContact = requirement.dueBeforeStep >= 0
                ? requirement.dueBeforeStepContact : -1;
            if (priorDueNormalized >= 0 &&
                !::Mission::EntitySchedulePolicy::DueBarrierDoesNotRegress(
                    priorDueNormalized, priorDueContact,
                    dueNormalized, dueContact)) {
                outError = "entity-contact schedule entries are out of order";
                return false;
            }
            if (requirement.comboEndAfter) {
                const bool hasFollowingAction = requirement.dueBeforeStep >= 0;
                const bool hasFollowingEntitySegment =
                    i + 1 < mission.entityContacts.size() &&
                    mission.entityContacts[i + 1].segment > requirement.segment;
                if (!hasFollowingAction && !hasFollowingEntitySegment) {
                    outError = "entity-contact schedule entry " +
                        std::to_string(i) +
                        " ends a combo without a following setup/Part-2 obligation";
                    return false;
                }
            }
            priorDueNormalized = dueNormalized;
            priorDueContact = dueContact;
            priorSegment = requirement.segment;
            priorRequirementEndsCombo = requirement.comboEndAfter;
            if (requirement.comboEndAfter) ++entityBoundariesBefore;
        }
    }
    if (!mission.entityLifecycles.empty()) {
        const int totalSteps = static_cast<int>(mission.steps.size());
        const int comboBoundaryCount =
            static_cast<int>(std::count_if(
                mission.steps.begin(), mission.steps.end(),
                [](const ::Mission::Step& step) {
                    return step.comboEndAfter;
                })) +
            static_cast<int>(std::count_if(
                mission.entityContacts.begin(), mission.entityContacts.end(),
                [](const ::Mission::EntityContactRequirement& requirement) {
                    return requirement.comboEndAfter;
                }));
        int priorAction = -1;
        int priorSegment = 0;
        for (std::size_t i = 0; i < mission.entityLifecycles.size(); ++i) {
            const auto& requirement = mission.entityLifecycles[i];
            const auto kind = EntityLifecycleKind(requirement.lifecycle);
            const bool supportedKind =
                kind == ::Mission::EntitySchedulePolicy::LifecycleKind::Spawn ||
                kind == ::Mission::EntitySchedulePolicy::LifecycleKind::Morph;
            const bool validIdentity =
                ::Mission::EntityLifecyclePolicy::ValidIdentity(
                    requirement.slot, requirement.generation, kind,
                    requirement.pattern, requirement.priorPattern,
                    requirement.opensAfterAction);
            const int producerMove =
                requirement.opensAfterAction >= 0 &&
                requirement.opensAfterAction < totalSteps &&
                !mission.steps[static_cast<std::size_t>(
                     requirement.opensAfterAction)].moveIds.empty()
                    ? mission.steps[static_cast<std::size_t>(
                          requirement.opensAfterAction)].moveIds.front()
                    : -1;
            const auto* semantic = ::Mission::EntityNames::LookupSemantic(
                mission.player.character.c_str(), requirement.pattern,
                producerMove);
            if (requirement.owner != 1 ||
                requirement.slot < 0 ||
                requirement.slot >= static_cast<int>(
                    CollisionDisplay::kProjectileRingSlotCapacity) ||
                !supportedKind || !validIdentity ||
                requirement.opensAfterAction < 0 ||
                requirement.opensAfterAction >= totalSteps ||
                requirement.opensAfterAction < priorAction ||
                requirement.segment < priorSegment ||
                requirement.segment > comboBoundaryCount ||
                requirement.maxDelay < 1 || !semantic ||
                !::Mission::EntityNames::LifecyclePromisesContact(
                    semantic->disposition)) {
                outError = "invalid entity-lifecycle schedule entry " +
                    std::to_string(i);
                return false;
            }
            const bool duplicate = ::Mission::EntityLifecyclePolicy::
                HasPriorDuplicateObjective(mission.entityLifecycles, i);
            if (duplicate) {
                outError = "duplicate entity-lifecycle schedule entry " +
                    std::to_string(i);
                return false;
            }
            priorAction = requirement.opensAfterAction;
            priorSegment = requirement.segment;
        }
    }
    // Validate the ordinary linear contract as a whole, not only the newer
    // exact-contact subset.  A malformed loose/recorded step must be disabled
    // in the browser instead of arming a runner state that can never advance.
    for (std::size_t i = 0; i < mission.steps.size(); ++i) {
        const auto& step = mission.steps[i];
        if (step.notation.empty()) {
            outError = "step " + std::to_string(i) + " has no display notation";
            return false;
        }
        const auto* commandOrigin = step.entityCommand.present
            ? ::Mission::EntityCommandOriginPolicy::ValidateBoundCommand(
                  mission.player.character.c_str(),
                  step.entityCommand.slot,
                  step.entityCommand.generation,
                  step.entityCommand.rootPattern,
                  step.entityCommand.activationPattern,
                  step.expectedAttackMask)
            : nullptr;
        if (step.entityCommand.present) {
            const bool hasBoundLifecycle = std::any_of(
                mission.entityLifecycles.begin(),
                mission.entityLifecycles.end(),
                [i, &step, commandOrigin](
                    const ::Mission::EntityLifecycleRequirement& requirement) {
                    return requirement.opensAfterAction ==
                               static_cast<int>(i) &&
                           ::Mission::EntityCommandOriginPolicy::
                               LifecycleObjectiveBindsCommand(
                                   commandOrigin,
                                   step.entityCommand.slot,
                                   step.entityCommand.generation,
                                   requirement.slot,
                                   requirement.generation,
                                   requirement.lifecycle == "morph",
                                   requirement.pattern,
                                   requirement.priorPattern);
                });
            const bool hasBoundContact = std::any_of(
                mission.entityContacts.begin(), mission.entityContacts.end(),
                [i, &step, commandOrigin](
                    const ::Mission::EntityContactRequirement& requirement) {
                    return requirement.opensAfterAction ==
                               static_cast<int>(i) &&
                           requirement.fanoutMembers.empty() &&
                           ::Mission::EntityCommandOriginPolicy::
                               ContactObjectiveBindsCommand(
                                   commandOrigin,
                                   step.entityCommand.slot,
                                   step.entityCommand.generation,
                                   requirement.slot,
                                   requirement.generation,
                                   requirement.patterns.size() == 1,
                                   requirement.patterns.empty()
                                       ? -1
                                       : requirement.patterns.front(),
                                   requirement.producerLifecycle == "morph",
                                   requirement.producerLifecycle == "spawn",
                                   requirement.producerPattern,
                                   requirement.producerPriorPattern);
                });
            if (!commandOrigin || !step.moveIds.empty() ||
                step.req != ::Mission::StepReq::Move ||
                step.directContact || step.allowPartialHits || step.optional ||
                step.comboEndAfter || !step.contactResult.empty() ||
                step.hitsRequired != 1 || step.damage != 0 ||
                step.charState != -1 || step.maxDelay != 0 ||
                (!hasBoundLifecycle && !hasBoundContact)) {
                outError = "step " + std::to_string(i) +
                    " has an invalid input-only entity command contract";
                return false;
            }
        } else if (step.moveIds.empty()) {
            outError = "step " + std::to_string(i) + " has no move IDs";
            return false;
        }
        std::vector<int> uniqueMoveIds;
        for (int moveId : step.moveIds) {
            if (moveId <= 0 || moveId > 32767 ||
                std::find(uniqueMoveIds.begin(), uniqueMoveIds.end(), moveId) !=
                    uniqueMoveIds.end()) {
                outError = "step " + std::to_string(i) +
                    " has invalid or duplicate move IDs";
                return false;
            }
            uniqueMoveIds.push_back(moveId);
        }
        if (step.req == ::Mission::StepReq::Hits && step.hitsRequired < 1) {
            outError = "step " + std::to_string(i) +
                " has an invalid hit count";
            return false;
        }
        if (step.allowPartialHits &&
            (!step.directContact || step.req != ::Mission::StepReq::Hits ||
             step.hitsRequired <= 1)) {
            outError = "step " + std::to_string(i) +
                " has allowPartialHits without a direct multi-hit requirement";
            return false;
        }
        if (step.maxDelay < 0 || step.maxGap < 0 || step.damage < 0 ||
            step.charState < -1) {
            outError = "step " + std::to_string(i) +
                " has invalid timing, damage, or character-state data";
            return false;
        }
        if (!step.directContact && !step.contactResult.empty()) {
            outError = "step " + std::to_string(i) +
                " has a typed result without direct-contact ownership";
            return false;
        }
    }

    bool needsDirectContactHook = false;
    for (std::size_t i = 0; i < mission.steps.size(); ++i) {
        const auto& step = mission.steps[i];
        if (!step.directContact) continue;
        needsDirectContactHook = true;
        if (step.req == ::Mission::StepReq::Move) {
            outError = "direct-contact step " + std::to_string(i) +
                " cannot use req=move";
            return false;
        }
        if (!step.contactResult.empty() &&
            !::Mission::Contact::ValidResultRequirement(step.contactResult)) {
            outError = "direct-contact step " + std::to_string(i) +
                " has an invalid typed result";
            return false;
        }
    }
    if (needsDirectContactHook && !IsDirectContactHookReady()) {
        outError = "mission requires exact direct-contact tracking, but the "
                   "collision hook is unavailable";
        return false;
    }
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
    const auto migration =
        ::Mission::LegacyEntityMigration::UpgradeLegacyLifecycleReviews(
            missionOut, sourcePath);
    if (migration.result ==
        ::Mission::LegacyEntityMigration::Result::Upgraded) {
        LogOut("[MISSION][ENTITY][MIGRATION] " + migration.diagnostic +
               ": " + sourcePath, true);
    } else if (migration.result ==
               ::Mission::LegacyEntityMigration::Result::EvidenceRejected) {
        LogOut("[MISSION][ENTITY][MIGRATION] kept legacy review: " +
               migration.diagnostic + ": " + sourcePath, true);
    }
    if (migration.result ==
        ::Mission::LegacyEntityMigration::Result::Upgraded) {
        ::Mission::NormalizeFlexibleEntityFanoutEpisodes(
            missionOut, true);
    }
    return ValidateMissionForRuntime(missionOut, sourcePath, outError);
}

} // namespace

bool ValidateMissionRuntimeReadiness(const ::Mission::Mission& mission,
                                     const std::string& sourcePath,
                                     std::string& outError) {
    // The browser owns an independently parsed const metadata value. Upgrade a
    // copy for the same preflight decision used by the launch path; the source
    // mission and its authoring files remain untouched.
    ::Mission::Mission candidate = mission;
    const auto migration =
        ::Mission::LegacyEntityMigration::UpgradeLegacyLifecycleReviews(
            candidate, sourcePath);
    if (migration.result ==
        ::Mission::LegacyEntityMigration::Result::Upgraded) {
        LogOut("[MISSION][ENTITY][MIGRATION] browser preflight " +
               migration.diagnostic + ": " + sourcePath, true);
    }
    if (migration.result ==
        ::Mission::LegacyEntityMigration::Result::Upgraded) {
        ::Mission::NormalizeFlexibleEntityFanoutEpisodes(
            candidate, true);
    }
    return ValidateMissionForRuntime(candidate, sourcePath, outError);
}

// Internal value-load boundary used by both Runner::Load(path) and the title
// transaction consumer. It deliberately is not part of mission_engine.h: only
// this translation unit may claim that a Mission has passed the shared runtime
// validation above.
namespace Runner {
bool LoadPrepared(::Mission::Mission mission, const std::string& sourcePath,
                  std::string& outMsg, bool forceFreshMatch);
void ResetAfterWorldRestore();
}

void Tick() {
    // Session pause menu first: while it is open the world is frozen and the
    // menu owns navigation input; it also self-closes if the session died.
    ::Mission::PauseMenu::Tick();

    std::lock_guard<std::recursive_mutex> runStateLock(g_runStateMx);
    g_demoFinishedThisTick = false;
    g_worldRestoredThisTick = false;
    Snapshot s;
    s.completedBattleUpdateBatch = GetCompletedBattleUpdateBatch();
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
            s.completedBattleUpdateBatch, s.contactEvents.data(),
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
    std::array<InputPollAttackEdgeEvent,
               Snapshot::kMaxPolledAttackEdges> polledAttackEdges{};
    const InputPollAttackEdgeDrainResult attackDrain =
        DrainInputPollAttackEdgeJournal(
            1, polledAttackEdges.data(), polledAttackEdges.size());
    s.p1PolledAttackEdgeCount = static_cast<uint8_t>(attackDrain.count);
    s.p1PolledAttackEdgesDropped = attackDrain.droppedEvents;
    s.p1PolledAttackEdgeOverflow = attackDrain.overflowed;
    for (std::size_t i = 0; i < attackDrain.count; ++i) {
        s.p1PolledAttackEdgeEvents[i].serial = polledAttackEdges[i].serial;
        s.p1PolledAttackEdgeEvents[i].mask = polledAttackEdges[i].mask;
        s.p1PolledAttackEdges |= polledAttackEdges[i].mask;
        s.p1PolledAttackEdgeSerial = polledAttackEdges[i].serial;
    }
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
        const std::string playerMessage =
            "The lesson start state could not be applied: " + setupFailure +
            ". Return to Lessons and try again.";
        if (g_runActive.load(std::memory_order_acquire) &&
            g_runMission.tutorialSchema > 0 && g_runMission.hasLesson) {
            // Establish the durable frozen Error surface before releasing the
            // cross-thread P1 startup gate.
            ::Mission::TutorialSession::NotifyStartupFailure(playerMessage);
            SetRunnerStartupInputHold(false);
        } else {
            SetRunnerStartupInputHold(false);
            g_runActive.store(false, std::memory_order_release);
            DirectDrawHook::AddMessage(playerMessage.c_str(), "MISSION",
                                       RGB(255, 120, 120), 3500, 0, 120);
        }
        LogOut("[MISSION][SETUP] runner startup aborted: " + setupFailure, true);
    }

    // Frontend threads only enqueue authoring commands. Consume them here so
    // setup capture and recorder state are never mutated concurrently.
    // Acquire a queued menu-command zero-poll lease before a restore/cancel
    // command can mutate recorder ownership, then tick again afterwards so a
    // synchronously completed command can begin its neutral-release count.
    TickRecorderMenuCommandHandoff();
    ProcessRecorderCommand();
    TickRecorderMenuCommandHandoff();

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

    // The startup gate is independent of ordinary poll overrides and survives
    // hotswap/state cleanup. Reassert the atomic publication defensively while
    // either the prepared title transaction or runner baseline still owns it.
    const bool startupInputOwned = PendingLaunchPolicy::StartupInputOwned(
        g_pendingStartupInputHeld.load(std::memory_order_acquire),
        g_runRestorePending, g_runSavePending);
    if (g_runStartupInputHeld.load(std::memory_order_acquire) &&
        startupInputOwned && s.valid &&
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
        const bool worldSettled =
            !ImGuiImpl::IsVisible() && !::Mission::Setup::IsPending() &&
            !CharacterHotswap::IsBusy() && !g_recActive.load() &&
            !RoundIntroActive() && !::Mission::PauseMenu::IsOpen() &&
            !PauseIntegration::IsPausedOrFrozen();
        // Lifecycle/menu re-entry deliberately invalidates cached controller
        // pointers. The first trusted PracticeTick normally republishes this
        // around 200 ms after Match begins; StateDump::Restore must not race it
        // and turn a transient TriggerSave miss into a dead mission.
        const bool practiceControllerReady =
            PauseIntegration::GetPracticeControllerPtr() != nullptr;
        const bool clear = StartupRestorePolicy::CanAttempt(
            worldSettled, practiceControllerReady);
        if (!clear) g_runRestoreSettle = 0;
        // Setup/hotswap/round-intro/pause are already hard gates above. Two
        // consecutive clear monitor samples match the normal baseline-save
        // policy and avoid leaking ~0.32s of an un-restored world.
        const bool doRestore = clear && ++g_runRestoreSettle >= 2;
        const bool doTimeout = !doRestore && ++g_runRestoreTimeout > 5760;
        if (doRestore || doTimeout) {
            g_runRestoreSettle = 0;
            g_runRestoreTimeout = 0;
            g_runRestorePending = false;
            std::string sdErr = doTimeout
                ? (practiceControllerReady
                       ? std::string("restore window never settled")
                       : std::string(
                             "Practice checkpoint controller never became ready"))
                : std::string();
            bool ok = false;
            if (doRestore) {
                ScopedRunnerSelfLoad selfLoad; // TriggerLoad inside is ours
                // Invalidate this Tick's pre-restore Snapshot before calling:
                // Restore can change the live world and still report false if
                // its post-load entity verification fails.
                g_worldRestoredThisTick = true;
                ok = ::Mission::StateDump::Restore(g_runMission.savestate, sdErr);
                // StateDump restore cancels injection owners in its load preamble.
                SetRunnerStartupInputHold(true);
            }
            if (ok) {
                SkipRoundIntro("state restore");
                std::string lineageError;
                if (!InitializeRunEntityLineageBaseline(lineageError)) {
                    ok = false;
                    sdErr = "strict entity lineage baseline failed: " +
                        lineageError;
                }
            }
            if (ok) {
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
                    // Keep the zero-poll owner through this stale-snapshot
                    // pass. The ordinary release gate below retires it from a
                    // fresh sample on the next monitor Tick.
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
    const bool tutorialStartupHandoff =
        ::Mission::TutorialSession::IsActive();
    if (!g_worldRestoredThisTick &&
        g_runStartupInputHeld.load(std::memory_order_acquire) &&
        PendingLaunchPolicy::CanReleaseStartupInput(
            g_pendingStartupInputHeld.load(std::memory_order_acquire),
            g_runRestorePending, g_runSavePending,
            tutorialStartupHandoff,
            /*tutorialOwnerEstablished=*/false)) {
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
                const bool inheritedStartupHold =
                    HandoffPendingStartupInputHold();
                std::string msg;
                if (Runner::LoadPrepared(std::move(*preparedMission), path, msg,
                                         /*forceFreshMatch=*/false)) {
                    LogOut("[MISSION] mission mode: loaded '" + msg + "' after match entry", true);
                } else {
                    // Early LoadPrepared preflight failures happen before it
                    // adopts/releases the runner hold. This is idempotent for
                    // later setup failures which already released it.
                    if (inheritedStartupHold) SetRunnerStartupInputHold(false);
                    LogOut("[MISSION] mission mode: load failed: " + msg, true);
                    DirectDrawHook::AddMessage(("Mission load failed: " + msg).c_str(), "MISSION",
                                               RGB(255, 120, 120), 2500, 0, 120);
                }
            } else if (consume == PendingLaunchPolicy::ConsumeEffect::OpenGenericBrowser) {
                ReleasePendingStartupInputHold("generic browser entry");
                CustomMenu::Screens::OpenMissionBrowser();
                OpenMenu();
                LogOut("[MISSION] mission mode: opened browser after match entry", true);
            } else {
                ReleasePendingStartupInputHold("incomplete pending transaction");
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
                CustomMenu::Screens::PrepareNewMissionAuthoringSession();
                Recorder::Arm();
                // Arm is normally a frontend queue, but ProcessRecorderCommand
                // already ran earlier in this monitor tick. Publish PRE-RECORD
                // now before opening UI; otherwise OpenMenu still sees Idle
                // and falls through to the ordinary Practice menu. The
                // dedicated setup menu exposes Authoring Options and Practice
                // Settings explicitly.
                ProcessRecorderCommand();
                if (!::Mission::PauseMenu::Open()) {
                    LogOut("[MISSION][REC] dedicated setup menu could not open; ESC can retry",
                           true);
                } else {
                    LogOut("[MISSION][REC] opened dedicated recording setup menu at title entry",
                           true);
                }
            }
        } else {
            s_recSettle = 0;
        }
    }

    TickDemo(s);
    if (g_worldRestoredThisTick) {
        // GetSnapshot callers run after this lock is released. Do not publish
        // the sample taken from the world that has just been replaced.
        g_snapshot.valid = false;
    }
    TickRecorderFrontend(s);
    if (s.valid && g_recActive.load() &&
        !g_recCaptureMenuSuspendedView.load(std::memory_order_acquire) &&
        !g_recCaptureMenuRequested.load(std::memory_order_acquire)) {
        if (g_recStartedThisTick) {
            // `s` predates the exact save/cleanup performed above. Never feed
            // that stale snapshot into a newly started take.
            g_recStartedThisTick = false;
        } else {
            const bool macroTickAdvanced = MacroController::DidAdvanceRecordingTick();
            RecorderCapture(s, macroTickAdvanced);
            FinishRecorderStopAfterCapture(macroTickAdvanced);
        }
    } else if (s.valid && g_recActive.load() && !g_recStartedThisTick &&
               (g_recCaptureMenuSuspendedView.load(std::memory_order_acquire) ||
                 g_recCaptureMenuRequested.load(std::memory_order_acquire))) {
        // The pause surface is entered at the beginning of this monitor pass,
        // after the preceding closed battle-update batch may already have
        // produced a move or resolver contact. The contact journal was drained
        // above, so skipping capture here would discard that final evidence.
        // Observe it without advancing the macro/effective timeline; repeated
        // frozen passes are idempotent because no move-instance edge occurs.
        // The macro recorder deliberately rebases across menu ownership, so
        // attack edges consumed by the button that opened/confirmed that menu
        // are not part of the sealed demonstration.  Do retain the closed
        // battle batch's move/contact evidence, but never re-import those UI
        // edges into the recipe-side pending-input journal.
        Snapshot evidenceOnly = s;
        evidenceOnly.p1PolledAttackEdges = 0;
        evidenceOnly.p1PolledAttackEdgeSerial = 0;
        evidenceOnly.p1PolledAttackEdgeCount = 0;
        evidenceOnly.p1PolledAttackEdgesDropped = 0;
        evidenceOnly.p1PolledAttackEdgeOverflow = false;
        evidenceOnly.p1PolledAttackEdgeEvents.fill(
            Snapshot::PolledAttackEdge{});
        RecorderCapture(evidenceOnly, false);
    }
    // Runner validation waits until the mission's setup actually landed - with
    // a restore or value-apply still pending the player would be "playing" a
    // mission whose start state is not set yet.
    const bool demoSnapshotCanValidate =
        ::Mission::SequencePolicy::SnapshotCanResumeAfterRestore(
            g_worldRestoredThisTick, Demo::IsActive(),
            g_demoFinishedThisTick);
    if (s.valid && g_runActive.load() && !g_runRestorePending && !g_runSavePending &&
        !::Mission::Setup::IsPending() && demoSnapshotCanValidate) {
        if (::Mission::TutorialSession::IsActive()) {
            ::Mission::TutorialSession::Tick(s);   // schema lessons: session owns validation
            // Preparing has now synchronously become a frozen Intro/Error or
            // an armed task with its own neutral-input lease. Retire the
            // dedicated startup gate only after that owner is proven live.
            const bool tutorialOwnerEstablished =
                ::Mission::TutorialSession::HasStartupInputOwnership();
            if (g_runStartupInputHeld.load(std::memory_order_acquire) &&
                PendingLaunchPolicy::CanReleaseStartupInput(
                    g_pendingStartupInputHeld.load(std::memory_order_acquire),
                    g_runRestorePending, g_runSavePending,
                    /*tutorialActive=*/true,
                    tutorialOwnerEstablished)) {
                SetRunnerStartupInputHold(false);
                LogOut("[MISSION][RUN] P1 startup neutral handed to tutorial", true);
            }
        } else {
            TickRun(s);
        }
    }
    if (g_inspector.load() && s.valid && !g_worldRestoredThisTick) {
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
        ReleasePendingStartupInputHold("replaced by generic browser entry");
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
    } else {
        ReleasePendingStartupInputHold("pending mission mode disabled");
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
        ReleasePendingStartupInputHold("replaced by mission authoring");
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
    // Arm before the title browser returns control to EFZ. This hold spans the
    // direct Loading route and the Character Select fallback, then transfers
    // to Runner::LoadPrepared without ever exposing a live Match input poll.
    AcquirePendingStartupInputHold();
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
    ReleasePendingStartupInputHold(reason && *reason ? reason
                                                     : "pending launch canceled");
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
void SetCaptureMenuOpen(bool open) {
    if (open) {
        g_recCaptureMenuReleasePending.store(false, std::memory_order_release);
        g_recCaptureMenuRequested.store(true, std::memory_order_release);
        // Stop the macro lane immediately; the mission-thread Tick installs
        // the zero-poll lease and samples release state.
        MacroController::SetRecordingCaptureSuspended(true);
        return;
    }
    // Publish the close edge before requested=false. An acquire load which
    // observes the closed request must also see this release-handoff token.
    if (IsCapturePhase(g_recPhase.load(std::memory_order_acquire))) {
        g_recCaptureMenuReleasePending.store(true, std::memory_order_release);
    }
    g_recCaptureMenuRequested.store(false, std::memory_order_release);
}
bool IsCaptureMenuSuspended() {
    return g_recCaptureMenuSuspendedView.load(std::memory_order_acquire) ||
           g_recCaptureMenuRequested.load(std::memory_order_acquire);
}
bool IsMenuInputHandoffActive() {
    return IsCaptureMenuSuspended() ||
           g_recCaptureMenuReleasePending.load(std::memory_order_acquire) ||
           g_recMenuCommandHandoffRequested.load(std::memory_order_acquire);
}
void BeginMenuCommandHandoff() {
    // PauseMenu::Tick is the mission-thread caller. Quarantine P1 immediately,
    // before the rest of Mission::Tick samples contacts/state or a queued
    // retake reaches StateDump::Restore; EFZ continues polling while physically
    // paused, so deferring this lease would let Confirm enter its native ring.
    SetRecorderInputLease(true);
    ResetInputPollOverrideHitCount(1);
    uint32_t discardedSerial = 0;
    (void)ConsumeInputPollAttackEdgeBatch(1, discardedSerial);
    g_recMenuCommandHandoffActive = true;
    g_recMenuCommandHandoffLastPollSerial = GetInputPollSerial(1);
    g_recMenuCommandHandoffNeutralPolls = 0;
    g_recMenuCommandHandoffDone.store(false, std::memory_order_release);
    PauseIntegration::OnMenuSurfaceVisibilityChanged(
        PauseIntegration::MenuSurface::RecorderCommandHandoff, true);
    g_recMenuCommandPauseOwned = true;
    g_recMenuCommandHandoffRequested.store(true, std::memory_order_release);
}
Phase GetPhase() { return g_recPhase.load(std::memory_order_acquire); }
int  GetStepCount() { return g_recStepCountView.load(std::memory_order_acquire); }
int  GetComboEndCount() { return g_recComboEndCountView.load(std::memory_order_acquire); }
int  GetCountInValue() { return g_recCountInView.load(std::memory_order_acquire); }
bool HasPreviewableTake() {
    return g_recPreviewAvailableView.load(std::memory_order_acquire);
}
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
    g_demoRoundRestoreSettleTicks = 0;
    g_demoTerminalRecipeHoldTicks = 0;
    g_demoRecipeSettleTicksRemaining = 0;
    // Acquire both layers before publishing the request. The frontend surface
    // that launched the demo may close immediately after this function, but
    // DemoTransition remains as the aggregate pause owner across that handoff.
    HoldDemoP1Neutral(true);
    SetDemoTransitionFreeze(true);
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

    // Review owns an immutable clip sealed at the synchronized stop boundary.
    // The ordinary macro slot is mutable and may have been recorded/replayed
    // since this take; it must never replace the mission demonstration.
    const std::string clip = g_recSealedDemo;
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
    g_demoRoundRestoreSettleTicks = 0;
    g_demoTerminalRecipeHoldTicks = 0;
    g_demoRecipeSettleTicksRemaining = 0;
    HoldDemoP1Neutral(true);
    SetDemoTransitionFreeze(true);
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

bool IsPracticeAutomationSuppressed() {
    const RecorderPhase recorderPhase =
        g_recPhase.load(std::memory_order_acquire);
    const bool recorderCapture = recorderPhase == RecorderPhase::CountIn ||
                                 recorderPhase == RecorderPhase::Recording;
    const bool demoActive =
        g_demoPhase.load(std::memory_order_acquire) != DemoPhase::Idle;
    return ::Mission::SequencePolicy::PracticeAutomationSuppressed(
        demoActive, recorderCapture);
}

void NotifyPracticeSessionReset(const char* reason) {
    // Session teardown may run while the frame monitor is suspended. Close
    // the dedicated surface synchronously so its open flag and pause owner
    // cannot survive into the next Practice match.
    ::Mission::PauseMenu::Close();
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
    ClearRecorderCaptureMenuSuspension();
    ClearRecorderMenuCommandHandoff();
    SetRecorderInputLease(false);
    g_recActive.store(false, std::memory_order_release);
    g_recPhase.store(RecorderPhase::Idle, std::memory_order_release);
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
    g_demoRoundRestoreSettleTicks = 0;
    g_demoTerminalRecipeHoldTicks = 0;
    g_demoRecipeSettleTicksRemaining = 0;
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
        ClearRecorderCaptureMenuSuspension();
        SetRecorderInputLease(false);
        g_recActive.store(false, std::memory_order_release);
        ResetRecorderCaptureState();
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
        // The world has already been replaced, so a strict-v3 lineage reset is
        // safe here. Public Reset deliberately cannot make that guarantee.
        Runner::ResetAfterWorldRestore();
        std::string lineageError;
        if (!InitializeRunEntityLineageBaseline(lineageError)) {
            LogOut("[MISSION][RUN][ENTITY] manual-load lineage baseline failed: " +
                   lineageError, true);
        }
        LogOut("[MISSION][RUN] manual savestate load - run reset", true);
    }
}

namespace Recorder {

namespace {

const char* ContactRequirementName(::Mission::Contact::Result result) {
    using Result = ::Mission::Contact::Result;
    switch (result) {
        case Result::Hit:         return "hit";
        case Result::SpecialHit:  return "special";
        case Result::Block:       return "block";
        case Result::RecoilGuard: return "recoil_guard";
        case Result::Throw:       return "throw";
        case Result::GuardPoint:  return "guard_point";
        default:                  return nullptr;
    }
}

struct CompiledEntityContact {
    ::Mission::EntityContactRequirement requirement;
    uint32_t firstSequence = 0;
    uint32_t lastSequence = 0;
    int lastEffectiveFrame = 0;
    uint32_t lastContactBatch = 0;
    uint32_t lastAfterActionOrder = 0;
};

struct RecordedDirectContactOrder {
    uint32_t sequence = 0;
    int step = -1;
    int ordinal = 0;
};

bool CompleteRecordedEntityPhaseBridgeEvidence() {
    return !g_recSetup.player.character.empty() &&
           !g_recSetup.savestate.empty() &&
           g_recEntityContactHookObserved &&
           g_recEntityContactHookComplete &&
           !g_recContactEpochDiscontinuity &&
           !g_recContactJournalOverflowObserved &&
           g_recEntityTraceDropped == 0 &&
           !g_recEntityProbeIncomplete[0] &&
           !g_recEntityAllocationAmbiguous[0];
}

::Mission::EntityContactPhaseBridgePolicy::BridgeKind
RecordedEntityPhaseBridge(const RecEntityTraceEvent& endpoint,
                          const RecEntityTraceEvent& contact) {
    using BridgeKind =
        ::Mission::EntityContactPhaseBridgePolicy::BridgeKind;
    if (!CompleteRecordedEntityPhaseBridgeEvidence() ||
        endpoint.kind != RecEntityTraceKind::Morph ||
        contact.kind != RecEntityTraceKind::Contact ||
        contact.sampled ||
        contact.contactSource != ::Mission::Contact::Source::Entity ||
        contact.defender != 2 ||
        !::Mission::ContactAttributionPolicy::IsCommittedResult(
            contact.contactResult) ||
        endpoint.player != 1 || contact.player != 1 ||
        endpoint.player != contact.player || endpoint.slot != contact.slot ||
        endpoint.generation != contact.generation ||
        endpoint.sampleSerial == 0 ||
        endpoint.sampleSerial != contact.sampleSerial ||
        endpoint.afterStep != contact.afterStep ||
        endpoint.afterActionOrder != contact.afterActionOrder ||
        endpoint.pattern <= 0 || endpoint.pattern > 0xFFFF ||
        endpoint.priorPattern <= 0 || endpoint.priorPattern > 0xFFFF ||
        contact.pattern <= 0 || contact.pattern > 0xFFFF) {
        return BridgeKind::None;
    }
    return ::Mission::EntityContactPhaseBridgePolicy::ClassifyBridge(
        g_recSetup.player.character.c_str(),
        static_cast<uint16_t>(endpoint.priorPattern),
        static_cast<uint16_t>(endpoint.pattern),
        static_cast<uint16_t>(contact.pattern));
}

bool FindRecordedEntityProducer(std::size_t contactTraceIndex,
                                const RecEntityTraceEvent& contact,
                                RecEntityTraceEvent& producerOut,
                                std::size_t* producerTraceIndexOut = nullptr) {
    producerOut = RecEntityTraceEvent{};
    if (producerTraceIndexOut) *producerTraceIndexOut = g_recEntityTraceCount;
    if (contact.slot < 0 || contact.generation == 0) return false;
    for (std::size_t i = contactTraceIndex; i-- > 0;) {
        const RecEntityTraceEvent& candidate = g_recEntityTrace[i];
        if (candidate.player != contact.player ||
            candidate.slot != contact.slot ||
            candidate.generation != contact.generation) {
            continue;
        }
        if (candidate.kind != RecEntityTraceKind::Baseline &&
            candidate.kind != RecEntityTraceKind::Spawn &&
            candidate.kind != RecEntityTraceKind::Morph) {
            continue;
        }
        // The nearest lifecycle observation is authoritative. Do not skip a
        // later mismatching morph to find an older same-pattern spawn: doing so
        // would silently bind the contact to the wrong phase of the entity.
        producerOut = candidate;
        if (producerTraceIndexOut) *producerTraceIndexOut = i;
        if (candidate.pattern == contact.pattern) return true;

        // Some entities can advance through a short damaging phase before the
        // endpoint ring is sampled. The exact resolver record is drained
        // immediately after that sample. Bridge only an exact, resource-bound
        // transition tuple; every other mismatch remains authoritative and
        // fails closed here.
        using BridgeKind =
            ::Mission::EntityContactPhaseBridgePolicy::BridgeKind;
        const BridgeKind bridge =
            RecordedEntityPhaseBridge(candidate, contact);
        if (bridge == BridgeKind::DirectPostContactRetirement) {
            for (std::size_t priorIndex = i; priorIndex-- > 0;) {
                const RecEntityTraceEvent& prior =
                    g_recEntityTrace[priorIndex];
                if (prior.player != contact.player ||
                    prior.slot != contact.slot ||
                    prior.generation != contact.generation) {
                    continue;
                }
                if (prior.kind != RecEntityTraceKind::Baseline &&
                    prior.kind != RecEntityTraceKind::Spawn &&
                    prior.kind != RecEntityTraceKind::Morph) {
                    continue;
                }
                // The immediately preceding lifecycle must be the exact
                // contact phase retired by this endpoint. A different phase
                // is authoritative evidence against the bridge; do not return
                // the later mismatching endpoint and rely on a caller to
                // notice the mismatch.
                if (prior.pattern != contact.pattern) return false;
                producerOut = prior;
                if (producerTraceIndexOut) {
                    *producerTraceIndexOut = priorIndex;
                }
                return true;
            }
            return false;
        }
        if (bridge == BridgeKind::CollapsedIntermediateContact) {
            // Represent the exact hook-observed phase which existed between
            // the sampled controller and recovery endpoints. This synthetic
            // producer is never accepted without the complete same-sample
            // resolver proof checked by RecordedEntityPhaseBridge.
            producerOut = candidate;
            producerOut.pattern = contact.pattern;
            producerOut.priorPattern = candidate.priorPattern;
            // Keep the synthetic phase tied to the sampled controller edge.
            // The common path above already assigns this index; repeat it here
            // so a future refactor cannot make semantic-source resolution
            // reject an otherwise fully proved collapsed bridge.
            if (producerTraceIndexOut) *producerTraceIndexOut = i;
            return true;
        }
        return false;
    }
    return false;
}

struct RecordedSemanticSource {
    int action = ::Mission::SemanticSourcePolicy::kExplicitUnresolved;
    int move = ::Mission::SemanticSourcePolicy::kExplicitUnresolved;
    int distinctCandidates = 0;
    bool baselineOrigin = false;
};

// Resolve presentation provenance without changing the strict collision
// producer.  Move-ID search alone is not causal: an old persistent child may
// connect while the player performs the same setter again.  Only lifecycle
// edges from this exact slot/generation and this contact episode may nominate
// an action.  If more than one action qualifies, retain the contact as an
// explicitly standalone entity hit rather than guessing which setter owns it.
RecordedSemanticSource ResolveRecordedSemanticSource(
    std::size_t contactTraceIndex,
    std::size_t contactProducerTraceIndex,
    const RecEntityTraceEvent& contact) {
    RecordedSemanticSource out;
    if (contact.slot < 0 || contact.generation == 0 ||
        contact.pattern <= 0 ||
        contactProducerTraceIndex >= g_recEntityTraceCount) {
        return out;
    }

    const auto* wildcardSemantic = ::Mission::EntityNames::LookupSemantic(
        g_recSetup.player.character.c_str(), contact.pattern);
    const bool unambiguousOrdinaryProjectile = wildcardSemantic &&
        wildcardSemantic->role == ::Mission::EntityNames::
            PresentationRole::InlineProjectile &&
        ::Mission::EntityNames::HasResolvedContactPresentation(
            wildcardSemantic->disposition) &&
        !::Mission::EntityNames::
            HasProducerQualifiedPersistentContactVariant(
                g_recSetup.player.character.c_str(), contact.pattern);
    // A generation already alive in the authored savestate has no in-recipe
    // setter. A later identical command is temporal correlation, not proof
    // that this older child belongs to that command.
    bool observedGenerationBirth = false;
    std::size_t generationBirthTraceIndex = g_recEntityTraceCount;
    std::size_t generationBegin = 0;
    for (std::size_t i = 0; i < contactTraceIndex; ++i) {
        const RecEntityTraceEvent& event = g_recEntityTrace[i];
        if (event.player != contact.player || event.slot != contact.slot ||
            event.generation != contact.generation ||
            (event.kind != RecEntityTraceKind::Baseline &&
             event.kind != RecEntityTraceKind::Spawn &&
             event.kind != RecEntityTraceKind::Morph)) {
            continue;
        }
        if (event.kind == RecEntityTraceKind::Baseline) {
            out.baselineOrigin = true;
            generationBegin = i + 1;
            break;
        }
        if (event.kind != RecEntityTraceKind::Spawn) {
            // The recorder did not observe this generation's birth. Exact
            // semantic ownership is unavailable even if later timing looks
            // convenient.
            return out;
        }
        observedGenerationBirth = true;
        generationBirthTraceIndex = i;
        generationBegin = i;
        break;
    }
    if (!observedGenerationBirth && !out.baselineOrigin) return out;

    // Ordinary bullets belong to the action which created this exact entity
    // generation, not to a later contact-time morph. This preserves the slow
    // projectile case: a normal performed while the bullet finally reaches
    // the opponent cannot steal its presentation ownership. Immediate-vs-
    // delayed timing is decided later from the independently persisted
    // contact gate.
    if (unambiguousOrdinaryProjectile) {
        // A bullet which was already present in the authored start state has
        // no in-recipe cast. It remains a standalone contact.
        if (!observedGenerationBirth) return out;
        const RecEntityTraceEvent& birth =
            g_recEntityTrace[generationBirthTraceIndex];
        const int sourceAction =
            ::Mission::SemanticSourcePolicy::ResolveOrdinaryBirthAction(
                true, false, birth.afterStep, contact.afterStep,
                birth.afterStep >= 0 &&
                    birth.afterStep < static_cast<int>(g_recSteps.size()) &&
                    birth.afterActionOrder != 0 &&
                    birth.afterActionOrder ==
                        RecorderActionOrderAfterStep(birth.afterStep));
        if (sourceAction >= 0) {
            out.action = sourceAction;
            out.move = g_recSteps[
                static_cast<std::size_t>(sourceAction)].moveId;
            out.distinctCandidates = 1;
        }
        return out;
    }

    // A completed contact owned by another producer phase starts a new
    // semantic episode for persistent summons. Contacts linked to the current
    // phase are ordinary multi-hit members and do not erase its source.
    std::size_t episodeBegin = generationBegin;
    for (std::size_t priorIndex = contactTraceIndex; priorIndex-- > 0;) {
        const RecEntityTraceEvent& prior = g_recEntityTrace[priorIndex];
        if (prior.kind != RecEntityTraceKind::Contact ||
            prior.contactSource != ::Mission::Contact::Source::Entity ||
            prior.player != contact.player || prior.defender != 2 ||
            prior.slot != contact.slot ||
            prior.generation != contact.generation ||
            !::Mission::ContactAttributionPolicy::IsCommittedResult(
                prior.contactResult)) {
            continue;
        }
        RecEntityTraceEvent priorProducer;
        std::size_t priorProducerIndex = g_recEntityTraceCount;
        if (!FindRecordedEntityProducer(
                priorIndex, prior, priorProducer, &priorProducerIndex)) {
            return out;
        }
        if (priorProducerIndex != contactProducerTraceIndex) {
            episodeBegin = (std::max)(episodeBegin, priorIndex + 1);
            break;
        }
    }

    ::Mission::SemanticSourcePolicy::CandidateSelection selection;
    for (std::size_t i = episodeBegin; i < contactTraceIndex; ++i) {
        const RecEntityTraceEvent& candidate = g_recEntityTrace[i];
        const bool candidateIsSpawn =
            candidate.kind == RecEntityTraceKind::Spawn;
        const bool candidateIsMorph =
            candidate.kind == RecEntityTraceKind::Morph;
        if (candidate.player != contact.player ||
            candidate.slot != contact.slot ||
            candidate.generation != contact.generation ||
            candidate.afterStep < 0 ||
            candidate.afterStep >= static_cast<int>(g_recSteps.size()) ||
            candidate.afterStep > contact.afterStep ||
            !::Mission::SemanticSourcePolicy::CandidateLifecycleEligible(
                out.baselineOrigin, observedGenerationBirth,
                candidateIsSpawn, candidateIsMorph,
                candidate.pattern == contact.pattern,
                candidate.afterActionOrder != 0 &&
                    candidate.afterActionOrder ==
                        RecorderActionOrderAfterStep(candidate.afterStep))) {
            continue;
        }
        const CapturedStep& sourceStep = g_recSteps[
            static_cast<std::size_t>(candidate.afterStep)];
        const auto moveMatches = [&](short candidateMove) {
            if (candidateMove <= 0) return false;
            const auto* semantic = ::Mission::EntityNames::LookupSemantic(
                g_recSetup.player.character.c_str(), contact.pattern,
                candidateMove);
            return semantic && semantic->producerMove == candidateMove &&
                ::Mission::EntityNames::HasResolvedContactPresentation(
                    semantic->disposition);
        };
        const bool primaryMatches = moveMatches(sourceStep.moveId);
        if (sourceStep.automaticFollowupIds.empty()) {
            selection = ::Mission::SemanticSourcePolicy::
                AccumulateStepMoveCandidate(
                    selection, candidate.afterStep, sourceStep.moveId,
                    primaryMatches, -1, false);
        } else {
            for (short followup : sourceStep.automaticFollowupIds) {
                selection = ::Mission::SemanticSourcePolicy::
                    AccumulateStepMoveCandidate(
                        selection, candidate.afterStep, sourceStep.moveId,
                        primaryMatches, followup, moveMatches(followup));
            }
        }
    }

    selection = ::Mission::SemanticSourcePolicy::FinalizeCandidate(selection);
    out.action = selection.action;
    out.move = selection.move;
    out.distinctCandidates = selection.distinctCandidates;
    return out;
}

int SegmentAtEntityContact(int precedingDirectStep,
                           const RecEntityTraceEvent& event) {
    int segment = 0;
    for (int i = 0; i < static_cast<int>(g_recSteps.size()); ++i) {
        if (!g_recSteps[static_cast<std::size_t>(i)].comboEndAfter) continue;
        if (i < precedingDirectStep ||
            (i == precedingDirectStep &&
             ::Mission::ContactAttributionPolicy::IsCommittedResult(
                 event.contactResult) &&
             ::Mission::EntitySchedulePolicy::OrderedContactBeginsNewCombo(
                 event.comboBefore, event.comboAfter))) {
            ++segment;
        }
    }
    return segment;
}

::Mission::EntityContactPhaseBridgePolicy::BridgeKind
CuratedEntityRecoveryBridgeInSameSample(
    std::size_t lifecycleIndex,
    const RecEntityTraceEvent& lifecycle) {
    using BridgeKind =
        ::Mission::EntityContactPhaseBridgePolicy::BridgeKind;
    if (!CompleteRecordedEntityPhaseBridgeEvidence() ||
        lifecycle.kind != RecEntityTraceKind::Morph ||
        lifecycle.sampleSerial == 0) {
        return BridgeKind::None;
    }
    for (std::size_t contactIndex = lifecycleIndex + 1;
         contactIndex < g_recEntityTraceCount; ++contactIndex) {
        const RecEntityTraceEvent& contact = g_recEntityTrace[contactIndex];
        if (contact.sampleSerial != lifecycle.sampleSerial) break;
        if (contact.kind != RecEntityTraceKind::Contact ||
            contact.contactSource != ::Mission::Contact::Source::Entity ||
            contact.defender != 2 ||
            !::Mission::ContactAttributionPolicy::IsCommittedResult(
                contact.contactResult)) {
            continue;
        }
        const BridgeKind bridge =
            RecordedEntityPhaseBridge(lifecycle, contact);
        if (bridge != BridgeKind::None) return bridge;
    }
    return BridgeKind::None;
}

void CompileRecordedEntityLifecycles(
    std::vector<::Mission::EntityLifecycleRequirement>& out,
    std::vector<std::string>& reviewReasons) {
    out.clear();
    reviewReasons.clear();
    const auto producerMoveForStep = [](int step) -> int {
        return step >= 0 && step < static_cast<int>(g_recSteps.size())
            ? g_recSteps[static_cast<std::size_t>(step)].moveId
            : -1;
    };
    for (std::size_t lifecycleIndex = 0;
         lifecycleIndex < g_recEntityTraceCount; ++lifecycleIndex) {
        const RecEntityTraceEvent& lifecycle =
            g_recEntityTrace[lifecycleIndex];
        if (lifecycle.player != 1 ||
            !::Mission::MoveData::Detail::LifecycleRequiresGrade(
                lifecycle.entityClass, lifecycle.attack,
                lifecycle.pattern) ||
            (lifecycle.kind != RecEntityTraceKind::Spawn &&
             lifecycle.kind != RecEntityTraceKind::Morph) ||
            lifecycle.slot < 0 || lifecycle.generation == 0) {
            continue;
        }
        const int lifecycleProducerMove =
            producerMoveForStep(lifecycle.afterStep);
        const auto* lifecycleSemantic =
            ::Mission::EntityNames::LookupSemantic(
                g_recSetup.player.character.c_str(), lifecycle.pattern,
                lifecycleProducerMove);

        // Non-contact lifecycle phases are still sampled and serialized in
        // the authoring sidecar, but cannot invent a visible/gradeable hit.
        // Post-contact recovery needs additional trace proof below.
        if (lifecycleSemantic &&
            !::Mission::EntityNames::LifecyclePromisesContact(
                lifecycleSemantic->disposition) &&
            !::Mission::EntityNames::IsPostContactRecovery(
                lifecycleSemantic->disposition)) {
            continue;
        }
        // Contact attribution and lifecycle suppression have different
        // contracts. The exact bridge may bind a contact to an immediately
        // preceding phase, but the sampled destination is suppressible only
        // when the catalog independently identifies it as recovery. If the
        // destination can itself contact, it remains a separate objective.
        using BridgeKind =
            ::Mission::EntityContactPhaseBridgePolicy::BridgeKind;
        const BridgeKind sameSampleBridge = lifecycleSemantic
            ? CuratedEntityRecoveryBridgeInSameSample(
                  lifecycleIndex, lifecycle)
            : BridgeKind::None;
        if (lifecycleSemantic &&
            ::Mission::EntityContactPhaseBridgePolicy::
                CanSuppressSampledEndpointObjective(
                    sameSampleBridge,
                    ::Mission::EntityNames::LifecyclePromisesContact(
                        lifecycleSemantic->disposition),
                    ::Mission::EntityNames::IsPostContactRecovery(
                        lifecycleSemantic->disposition))) {
            LogOut("[MISSION][REC][ENTITY] curated recovery #" +
                       std::to_string(lifecycle.pattern) + " resource=" +
                       g_recSetup.player.character + " slot=" +
                       std::to_string(lifecycle.slot) + " gen=" +
                       std::to_string(lifecycle.generation) +
                       " was closed by an exact same-sample contact; "
                       "no recovery objective", true);
            continue;
        }
        bool hasCommittedContact = false;
        for (std::size_t contactIndex = lifecycleIndex + 1;
             contactIndex < g_recEntityTraceCount; ++contactIndex) {
            const RecEntityTraceEvent& contact = g_recEntityTrace[contactIndex];
            if (contact.kind == RecEntityTraceKind::Contact &&
                contact.contactSource == ::Mission::Contact::Source::Entity &&
                contact.player == lifecycle.player &&
                contact.defender == 2 &&
                contact.slot == lifecycle.slot &&
                contact.generation == lifecycle.generation &&
                ::Mission::ContactAttributionPolicy::IsCommittedResult(
                    contact.contactResult)) {
                RecEntityTraceEvent linkedProducer;
                std::size_t linkedProducerIndex = g_recEntityTraceCount;
                if (FindRecordedEntityProducer(
                        contactIndex, contact, linkedProducer,
                        &linkedProducerIndex) &&
                    ::Mission::RecorderEntityTrace::ContactGradesLifecycle(
                        lifecycleIndex, linkedProducerIndex) &&
                    linkedProducer.pattern == contact.pattern) {
                    hasCommittedContact = true;
                    break;
                }
            }
        }
        if (!hasCommittedContact && lifecycleSemantic &&
            ::Mission::EntityNames::IsPostContactRecovery(
                lifecycleSemantic->disposition)) {
            // The recovery must immediately follow the contact episode it is
            // retiring. Michiru keeps one slot/generation alive across several
            // commands, so "any older contact in this generation" would let a
            // prior 421/214 incorrectly excuse a later ungraded 236 recovery.
            bool immediatelyPrecededBySameFamilyContact = false;
            for (std::size_t priorIndex = lifecycleIndex;
                 priorIndex-- > 0;) {
                const RecEntityTraceEvent& prior =
                    g_recEntityTrace[priorIndex];
                if (prior.player != lifecycle.player ||
                    prior.slot != lifecycle.slot ||
                    prior.generation != lifecycle.generation) {
                    continue;
                }
                if (prior.kind == RecEntityTraceKind::Contact &&
                    prior.contactSource == ::Mission::Contact::Source::Entity &&
                    prior.defender == 2 &&
                    ::Mission::ContactAttributionPolicy::IsCommittedResult(
                        prior.contactResult)) {
                    const auto* priorSemantic =
                        ::Mission::EntityNames::LookupSemantic(
                            g_recSetup.player.character.c_str(), prior.pattern,
                            producerMoveForStep(prior.afterStep));
                    immediatelyPrecededBySameFamilyContact = priorSemantic &&
                        priorSemantic->family && lifecycleSemantic->family &&
                        std::strcmp(priorSemantic->family,
                                    lifecycleSemantic->family) == 0;
                }
                // First same-instance event is the episode boundary. Do not
                // search through another morph/contact for convenient proof.
                break;
            }
            bool laterReturnedInertOrDespawned = false;
            for (std::size_t laterIndex = lifecycleIndex + 1;
                 laterIndex < g_recEntityTraceCount; ++laterIndex) {
                const RecEntityTraceEvent& later = g_recEntityTrace[laterIndex];
                if (later.player != lifecycle.player ||
                    later.slot != lifecycle.slot ||
                    later.generation != lifecycle.generation) {
                    continue;
                }
                if (later.kind == RecEntityTraceKind::Despawn) {
                    laterReturnedInertOrDespawned = true;
                    break;
                }
                if (later.kind != RecEntityTraceKind::Morph) continue;
                const auto* laterSemantic =
                    ::Mission::EntityNames::LookupSemantic(
                        g_recSetup.player.character.c_str(), later.pattern,
                        producerMoveForStep(later.afterStep));
                if (laterSemantic &&
                    !::Mission::EntityNames::LifecyclePromisesContact(
                        laterSemantic->disposition)) {
                    laterReturnedInertOrDespawned = true;
                    break;
                }
            }
            if (::Mission::RecorderEntityTrace::
                    CanSuppressCompletedPostContactRecovery(
                        true, immediatelyPrecededBySameFamilyContact,
                        laterReturnedInertOrDespawned)) {
                LogOut("[MISSION][REC][ENTITY] post-contact phase #" +
                       std::to_string(lifecycle.pattern) + " slot=" +
                       std::to_string(lifecycle.slot) + " gen=" +
                       std::to_string(lifecycle.generation) +
                       " completed after an earlier contact; no second objective");
                continue;
            }
        }
        if (!hasCommittedContact) {
            const bool unresolved = !lifecycleSemantic ||
                lifecycleSemantic->disposition ==
                    ::Mission::EntityNames::LifecycleDisposition::Unresolved ||
                lifecycleSemantic->disposition ==
                    ::Mission::EntityNames::LifecycleDisposition::DormantOrOrphan;
            // A contact-capable phase is a proven whiff only when the exact
            // resolver journal was continuously available.  Ring sampling can
            // prove that the phase existed, but by itself it cannot prove that
            // no contact happened; an unavailable/overflowed journal would
            // otherwise turn lost hit evidence into a false lifecycle-only
            // objective.
            const bool exactTrace =
                ::Mission::EntityLifecyclePolicy::
                    ExactWhiffEvidenceAvailable(
                        !g_recSetup.savestate.empty(),
                        g_recEntityContactHookObserved,
                        g_recEntityContactHookComplete,
                        !g_recContactEpochDiscontinuity,
                        !g_recContactJournalOverflowObserved,
                        g_recEntityTraceDropped == 0,
                        !g_recEntityProbeIncomplete[0],
                        !g_recEntityAllocationAmbiguous[0]);
            const bool boundAction = lifecycle.afterStep >= 0 &&
                lifecycle.afterStep < static_cast<int>(g_recSteps.size());
            const auto kind = EntityLifecycleKind(
                RecEntityTraceKindName(lifecycle.kind));
            const bool validDescriptor =
                ::Mission::EntityLifecyclePolicy::ValidIdentity(
                    lifecycle.slot,
                    static_cast<int>(lifecycle.generation), kind,
                    lifecycle.pattern, lifecycle.priorPattern,
                    lifecycle.afterStep);
            const bool gradeableWhiff = !unresolved && exactTrace &&
                boundAction && validDescriptor &&
                lifecycleSemantic &&
                ::Mission::EntityNames::LifecyclePromisesContact(
                    lifecycleSemantic->disposition);
            if (!gradeableWhiff) {
                reviewReasons.push_back(
                    std::string(unresolved ? "unresolved" : "attack") +
                    " entity setup #" +
                    std::to_string(lifecycle.pattern) + " (slot " +
                    std::to_string(lifecycle.slot) + ", generation " +
                    std::to_string(lifecycle.generation) +
                    ") could not be bound to an exact lifecycle objective; retake it");
                continue;
            }

            ::Mission::EntityLifecycleRequirement requirement;
            requirement.owner = 1;
            requirement.slot = lifecycle.slot;
            requirement.generation =
                static_cast<int>(lifecycle.generation);
            requirement.lifecycle = RecEntityTraceKindName(lifecycle.kind);
            requirement.pattern = lifecycle.pattern;
            requirement.priorPattern = lifecycle.priorPattern;
            requirement.opensAfterAction = lifecycle.afterStep;
            requirement.segment = 0;
            for (int stepIndex = 0; stepIndex < lifecycle.afterStep;
                 ++stepIndex) {
                if (g_recSteps[static_cast<std::size_t>(stepIndex)]
                        .comboEndAfter) {
                    ++requirement.segment;
                }
            }
            for (std::size_t priorIndex = 0;
                 priorIndex < lifecycleIndex; ++priorIndex) {
                const RecEntityTraceEvent& prior =
                    g_recEntityTrace[priorIndex];
                if (prior.kind == RecEntityTraceKind::Contact &&
                    prior.player == 1 && prior.comboEndAfter) {
                    ++requirement.segment;
                }
            }
            const int anchorFrame = g_recSteps[static_cast<std::size_t>(
                requirement.opensAfterAction)].startFrame;
            const int observedDelay =
                (std::max)(0, lifecycle.effectiveFrame - anchorFrame);
            requirement.maxDelay = observedDelay * 2 + 30;
            requirement.notation =
                ::Mission::EntityNames::FormatSemanticContact(
                    g_recSetup.player.character.c_str(), lifecycle.pattern,
                    "SETUP", lifecycleProducerMove);

            const bool duplicate = std::any_of(
                out.begin(), out.end(),
                [&requirement](
                    const ::Mission::EntityLifecycleRequirement& existing) {
                    return existing.owner == requirement.owner &&
                           existing.slot == requirement.slot &&
                           existing.generation == requirement.generation &&
                           existing.lifecycle == requirement.lifecycle &&
                           existing.pattern == requirement.pattern &&
                           existing.priorPattern == requirement.priorPattern &&
                           existing.opensAfterAction ==
                               requirement.opensAfterAction &&
                           existing.segment == requirement.segment;
                });
            if (!duplicate) {
                LogOut("[MISSION][REC][ENTITY] compiled lifecycle pattern=" +
                           std::to_string(requirement.pattern) +
                           " slot=" + std::to_string(requirement.slot) +
                           " generation=" +
                           std::to_string(requirement.generation) +
                           " producer=" + requirement.lifecycle + ":" +
                           std::to_string(requirement.priorPattern) + "->" +
                           std::to_string(requirement.pattern) +
                           " producerAction=" +
                           std::to_string(requirement.opensAfterAction) +
                           " notation=\"" + requirement.notation + "\"" +
                           " maxDelay=" +
                           std::to_string(requirement.maxDelay), true);
                out.push_back(std::move(requirement));
            }
        }
    }
}

bool CompileRecordedEntityContacts(
    std::vector<::Mission::EntityContactRequirement>& out,
    std::string& outError) {
    out.clear();
    outError.clear();
    std::vector<CompiledEntityContact> compiled;
    int precedingDirectStep = -1;
    int compiledSegment = 0;
    int entityBoundariesSeen = 0;
    int learnerEntityEvents = 0;

    // Precompute exact one-based resolver ordinals per authored direct action.
    // These are the portable barriers that let a projectile land between hit 1
    // and hit 2 of the same multi-hit move without flattening either side.
    std::vector<int> directContactTotals(g_recSteps.size(), 0);
    std::vector<RecordedDirectContactOrder> directOrder;
    for (std::size_t i = 0; i < g_recEntityTraceCount; ++i) {
        const RecEntityTraceEvent& event = g_recEntityTrace[i];
        if (event.kind != RecEntityTraceKind::Contact ||
            event.contactSource != ::Mission::Contact::Source::DirectPlayer ||
            event.player != 1 || event.defender != 2 ||
            event.contactResult == ::Mission::Contact::Result::None ||
            event.contactResult == ::Mission::Contact::Result::Unknown) {
            continue;
        }
        if (event.attributedStep < 0 ||
            event.attributedStep >= static_cast<int>(g_recSteps.size())) {
            outError = "an exact direct-contact barrier has no attributed action";
            return false;
        }
        const std::size_t step = static_cast<std::size_t>(event.attributedStep);
        RecordedDirectContactOrder ordered;
        ordered.sequence = event.contactSequence;
        ordered.step = event.attributedStep;
        ordered.ordinal = ++directContactTotals[step];
        directOrder.push_back(ordered);
    }

    std::vector<int> directContactsSeen(g_recSteps.size(), 0);
    int precedingDirectOrdinal = -1;
    for (std::size_t i = 0; i < g_recEntityTraceCount; ++i) {
        const RecEntityTraceEvent& event = g_recEntityTrace[i];
        if (event.kind != RecEntityTraceKind::Contact ||
            event.player != 1 || event.defender != 2) {
            continue;
        }
        if (event.contactSource == ::Mission::Contact::Source::DirectPlayer) {
            if (event.contactResult != ::Mission::Contact::Result::None &&
                event.contactResult != ::Mission::Contact::Result::Unknown) {
                if (event.attributedStep < 0) {
                    outError = "an exact direct-contact barrier has no attributed action";
                    return false;
                }
                precedingDirectStep = event.attributedStep;
                precedingDirectOrdinal = ++directContactsSeen[
                    static_cast<std::size_t>(precedingDirectStep)];
            }
            continue;
        }
        if (event.contactSource != ::Mission::Contact::Source::Entity ||
            event.contactResult == ::Mission::Contact::Result::None) {
            continue;
        }

        ++learnerEntityEvents;
        const char* resultName = ContactRequirementName(event.contactResult);
        if (!resultName || event.pattern <= 0 ||
            event.afterStep < -1 ||
            event.afterStep >= static_cast<int>(g_recSteps.size())) {
            outError = "entity contact trace contains an unrepresentable event";
            return false;
        }
        const int segment = ::Mission::EntitySchedulePolicy::CompiledSegment(
            SegmentAtEntityContact(precedingDirectStep, event),
            entityBoundariesSeen, compiledSegment);
        compiledSegment = segment;
        RecEntityTraceEvent producer;
        std::size_t producerTraceIndex = g_recEntityTraceCount;
        if (!FindRecordedEntityProducer(
                i, event, producer, &producerTraceIndex)) {
            outError = "entity contact has no exact matching lifecycle lineage";
            return false;
        }
        if (producer.pattern != event.pattern) {
            outError = "entity contact pattern has no contact-linked lifecycle transition";
            return false;
        }
        if (producer.afterStep > event.afterStep ||
            producer.afterActionOrder > event.afterActionOrder) {
            outError = "entity producer lineage was observed after its contact";
            return false;
        }
        const int producerAfterStep = producer.afterStep;
        const RecordedSemanticSource semanticSource =
            ResolveRecordedSemanticSource(
                i, producerTraceIndex, event);
        const bool precedingVariableMultiHit = precedingDirectStep >= 0 &&
            g_recSteps[static_cast<std::size_t>(precedingDirectStep)].directContact &&
            g_recSteps[static_cast<std::size_t>(precedingDirectStep)].hits > 1;
        // A projectile interleaved after (say) hit 4/5 of a variable multi-hit
        // cannot retain ordinal 4 as a hard barrier when a valid replay may
        // produce only three contacts. Preserve the stable lower bound (the
        // move connected at least once); a contact after the recorded final hit
        // remains a whole-action gate. Strict authored hit-count steps never
        // enter this compiler.
        int afterDirectContact = -1;
        if (precedingDirectStep >= 0 && precedingDirectOrdinal > 0 &&
            precedingDirectOrdinal < directContactTotals[
                static_cast<std::size_t>(precedingDirectStep)]) {
            afterDirectContact = precedingVariableMultiHit
                ? 1 : precedingDirectOrdinal;
        }
        const bool joinsRun = !compiled.empty() &&
            compiled.back().requirement.patterns.size() == 1 &&
            compiled.back().requirement.patterns.front() == event.pattern &&
            compiled.back().requirement.result == resultName &&
            compiled.back().requirement.producerLifecycle ==
                RecEntityTraceKindName(producer.kind) &&
            compiled.back().requirement.producerPattern == producer.pattern &&
            compiled.back().requirement.producerPriorPattern ==
                producer.priorPattern &&
            compiled.back().requirement.semanticSourceAction ==
                semanticSource.action &&
            compiled.back().requirement.semanticSourceMove ==
                semanticSource.move &&
            ::Mission::EntitySchedulePolicy::SameRecordedProducerLineage(
                compiled.back().requirement.slot,
                static_cast<uint32_t>(compiled.back().requirement.generation),
                compiled.back().requirement.opensAfterAction,
                event.slot, event.generation, producerAfterStep) &&
            ::Mission::EntitySchedulePolicy::CanJoinContactRun(
                compiled.back().requirement.comboEndAfter,
                compiled.back().requirement.segment, segment,
                compiled.back().requirement.slot,
                static_cast<uint32_t>(compiled.back().requirement.generation),
                event.slot, event.generation,
                compiled.back().requirement.contactAfterAction, event.afterStep,
                compiled.back().requirement.afterStep,
                compiled.back().requirement.afterStepContact,
                precedingDirectStep, afterDirectContact);
        if (!joinsRun) {
            CompiledEntityContact run;
            run.requirement.owner = 1;
            run.requirement.target = 2;
            // The exact embedded start state makes this sampled slot/generation
            // deterministic for v3 playback. Every contact in the run must
            // remain on that same producer instance.
            run.requirement.slot = event.slot;
            run.requirement.generation = static_cast<int>(event.generation);
            run.requirement.patterns.push_back(event.pattern);
            run.requirement.result = resultName;
            run.requirement.contactsRequired = 0;
            run.requirement.producerLifecycle =
                RecEntityTraceKindName(producer.kind);
            run.requirement.producerPattern = producer.pattern;
            run.requirement.producerPriorPattern = producer.priorPattern;
            run.requirement.opensAfterAction = producerAfterStep;
            run.requirement.semanticSourceAction = semanticSource.action;
            run.requirement.semanticSourceMove = semanticSource.move;
            if (::Mission::SemanticSourcePolicy::IsExplicitUnresolved(
                    semanticSource.action, semanticSource.move)) {
                LogOut("[MISSION][REC][ENTITY] semantic source unresolved "
                       "for pattern=" + std::to_string(event.pattern) +
                       " slot=" + std::to_string(event.slot) +
                       " generation=" +
                       std::to_string(event.generation) +
                       " candidates=" +
                       std::to_string(semanticSource.distinctCandidates) +
                       " baselineOrigin=" +
                       std::string(semanticSource.baselineOrigin
                                       ? "true" : "false") +
                       "; contact remains standalone", true);
            }
            run.requirement.contactAfterAction = event.afterStep;
            run.requirement.afterStep = precedingDirectStep;
            run.requirement.afterStepContact = afterDirectContact;
            run.requirement.segment = segment;
            run.firstSequence = event.contactSequence;
            compiled.push_back(std::move(run));
        }

        CompiledEntityContact& run = compiled.back();
        ++run.requirement.contactsRequired;
        run.requirement.comboHitsRequired +=
            ::Mission::EntitySchedulePolicy::OrderedHitContribution(
                ::Mission::ContactAttributionPolicy::IsHitResult(
                    event.contactResult),
                event.comboBefore, event.comboAfter);
        run.requirement.damage +=
            (std::max)(0, event.hpBefore - event.hpAfter);
        run.requirement.comboEndAfter =
            run.requirement.comboEndAfter || event.comboEndAfter;
        run.lastSequence = event.contactSequence;
        run.lastEffectiveFrame = event.effectiveFrame;
        run.lastContactBatch = event.contactBatch;
        run.lastAfterActionOrder = event.afterActionOrder;
        if (event.comboEndAfter) {
            ++entityBoundariesSeen;
            compiledSegment = segment + 1;
        }
    }

    if (learnerEntityEvents == 0) {
        outError = "no exact learner entity contacts were captured";
        return false;
    }

    for (CompiledEntityContact& run : compiled) {
        int followingActionStep = -1;
        for (int stepIndex = 0;
             stepIndex < static_cast<int>(g_recSteps.size()); ++stepIndex) {
            const CapturedStep& step = g_recSteps[static_cast<std::size_t>(stepIndex)];
            if (::Mission::EntitySchedulePolicy::RecordedActionFollowsContact(
                    step.startBattleBatch, step.startActionOrder,
                    run.lastContactBatch, run.lastAfterActionOrder)) {
                followingActionStep = stepIndex;
                break;
            }
        }
        int followingDirectStep = -1;
        int followingDirectContact = -1;
        for (const RecordedDirectContactOrder& direct : directOrder) {
            if (direct.sequence <= run.lastSequence) continue;
            followingDirectStep = direct.step;
            followingDirectContact = direct.ordinal;
            break;
        }
        if (followingDirectStep >= 0 &&
            g_recSteps[static_cast<std::size_t>(followingDirectStep)].directContact &&
            g_recSteps[static_cast<std::size_t>(followingDirectStep)].hits > 1 &&
            followingDirectContact > 1) {
            // See the preceding-barrier rule above: an ordinal inside a
            // recorder-flexible multi-hit is not reproducible across spacing.
            // Contact one remains a stable exact deadline; later ordinals become
            // a whole-action deadline so an interleaved entity can still occur.
            followingDirectContact = -1;
        }
        const auto due = ::Mission::EntitySchedulePolicy::SelectDueBarrier(
            followingActionStep, followingDirectStep, followingDirectContact);
        run.requirement.dueBeforeStep = due.step;
        run.requirement.dueBeforeStepContact = due.contact;
        const int anchorFrame = run.requirement.opensAfterAction >= 0
            ? g_recSteps[static_cast<std::size_t>(
                  run.requirement.opensAfterAction)].startFrame
            : 0;
        const int observedDelay =
            (std::max)(0, run.lastEffectiveFrame - anchorFrame);
        run.requirement.maxDelay = observedDelay * 2 + 30;
        const char* outcome =
            run.requirement.result == "hit" ? "HIT" :
            run.requirement.result == "special" ? "SPECIAL HIT" :
            run.requirement.result == "block" ? "BLOCK" :
            run.requirement.result == "recoil_guard" ? "RG" :
            run.requirement.result == "throw" ? "THROW" :
            run.requirement.result == "guard_point" ? "GUARD POINT" :
            "CONTACT";
        const int semanticProducerMove =
            ::Mission::SemanticSourcePolicy::IsExact(
                run.requirement.semanticSourceAction,
                run.requirement.semanticSourceMove)
                ? run.requirement.semanticSourceMove
                : -1;
        run.requirement.notation = ::Mission::EntityNames::FormatSemanticContact(
            g_recSetup.player.character.c_str(),
            run.requirement.patterns.front(), outcome,
            semanticProducerMove);
        if (run.requirement.contactsRequired > 1) {
            run.requirement.notation += " x" +
                std::to_string(run.requirement.contactsRequired);
        }
        const auto* compiledSemantic = ::Mission::EntityNames::LookupSemantic(
            g_recSetup.player.character.c_str(),
            run.requirement.patterns.front(), semanticProducerMove);
        LogOut("[MISSION][REC][ENTITY] compiled pattern=" +
                   std::to_string(run.requirement.patterns.front()) +
                   " slot=" + std::to_string(run.requirement.slot) +
                   " generation=" +
                   std::to_string(run.requirement.generation) +
                   " producer=" + run.requirement.producerLifecycle +
                   ":" + std::to_string(run.requirement.producerPriorPattern) +
                   "->" + std::to_string(run.requirement.producerPattern) +
                   " producerAction=" +
                   std::to_string(run.requirement.opensAfterAction) +
                   " semanticSource=" +
                   std::to_string(run.requirement.semanticSourceAction) +
                   ":" +
                   std::to_string(run.requirement.semanticSourceMove) +
                   " producerMove=" + std::to_string(semanticProducerMove) +
                   " family=" +
                   (compiledSemantic ? compiledSemantic->family : "<raw>") +
                   " label=\"" +
                   (compiledSemantic ? compiledSemantic->label : "<raw>") +
                   "\" disposition=" +
                   (compiledSemantic
                        ? ::Mission::EntityNames::DispositionLabel(
                              compiledSemantic->disposition)
                        : "unmapped") +
                   " notation=\"" + run.requirement.notation + "\"" +
                   " contactAction=" +
                   std::to_string(run.requirement.contactAfterAction) +
                   " after=" + std::to_string(run.requirement.afterStep) +
                   ":" +
                   std::to_string(run.requirement.afterStepContact) +
                   " due=" +
                   std::to_string(run.requirement.dueBeforeStep) + ":" +
                   std::to_string(run.requirement.dueBeforeStepContact) +
                   " segment=" + std::to_string(run.requirement.segment) +
                   " contacts=" +
                   std::to_string(run.requirement.contactsRequired) +
                   " comboHits=" +
                   std::to_string(run.requirement.comboHitsRequired),
               true);
        out.push_back(std::move(run.requirement));
    }
    return static_cast<int>(out.size()) > 0;
}

} // namespace

bool BuildDraft(::Mission::Mission& out) {
    std::lock_guard<std::recursive_mutex> lock(g_runStateMx);
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
        if (cs.moveId > 0) {
            st.moveIds.push_back(static_cast<int>(cs.moveId));
        }
        st.entityCommand = cs.entityCommand;
        st.expectedAttackMask = cs.expectedAttackMask;
        for (short followup : cs.automaticFollowupIds) {
            if (followup > 0) st.moveIds.push_back(static_cast<int>(followup));
        }
        st.req = static_cast<::Mission::StepReq>(cs.req);
        st.directContact = cs.directContact;
        st.contactResult = cs.directContactResult;
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
            // The observed count describes the take, but a variable multi-hit
            // move (Mizuka 2C is the common example) may legitimately make
            // fewer contacts at slightly different spacing.  Playback still
            // requires at least one exact resolver hit and proof that the
            // authored route continued; it does not pass on the first hit.
            st.allowPartialHits = st.directContact;
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
    std::vector<std::string> lifecycleReviews;
    CompileRecordedEntityLifecycles(
        m.entityLifecycles, lifecycleReviews);
    m.reviewRequired.insert(m.reviewRequired.end(),
                            lifecycleReviews.begin(),
                            lifecycleReviews.end());
    if (g_recEntityAttributionRequired) {
        std::string compileError;
        if (g_recEntityContactHookObserved &&
            g_recEntityContactHookComplete &&
            !g_recContactEpochDiscontinuity &&
            !g_recContactJournalOverflowObserved &&
            !g_recEntityProbeIncomplete[0] &&
            !g_recEntityAllocationAmbiguous[0] &&
            g_recEntityTraceDropped == 0 &&
            CompileRecordedEntityContacts(m.entityContacts, compileError)) {
        } else {
            m.reviewRequired.push_back(
                compileError.empty()
                    ? "entity contacts could not be compiled into a strict schedule"
                    : "entity contacts need review: " + compileError);
        }
    }
    if (!m.entityContacts.empty() || !m.entityLifecycles.empty()) {
        m.strictEntityContacts = true;
        // Older DLLs ignore unknown top-level fields. Start from the strict
        // v3/v4 contract; the curated fanout adapter below upgrades to v5 when
        // it emits unordered sibling members, so an older DLL fails closed.
        m.reviewRequired.push_back(
            m.entityLifecycles.empty()
                ? kEntityScheduleRuntimeMarkerV3
                : kEntityScheduleRuntimeMarkerV4);
        // The recorder first compiles every child as an exact v3/v4
        // obligation. Only the curated fanout adapter may relax interchangeable
        // Shiori siblings, retaining every exact slot/generation as a member
        // and upgrading the marker to v5. No authored/non-recorded schedule is
        // inferred here.
        if (::Mission::NormalizeFlexibleEntityFanoutEpisodes(m, true)) {
            for (std::size_t requirementIndex = 0;
                 requirementIndex < m.entityContacts.size();
                 ++requirementIndex) {
                const auto& requirement =
                    m.entityContacts[requirementIndex];
                if (requirement.fanoutMembers.empty()) continue;
                LogOut(
                    "[MISSION][REC][ENTITY][FANOUT] obligation=" +
                        std::to_string(requirementIndex) + " members=" +
                        std::to_string(requirement.fanoutMembers.size()) +
                        " minimum=" +
                        std::to_string(
                            requirement.minimumContactsRequired) + "/" +
                        std::to_string(
                            requirement.minimumComboHitsRequired) +
                        " observed=" +
                        std::to_string(requirement.contactsRequired) + "/" +
                        std::to_string(requirement.comboHitsRequired),
                    true);
            }
        }
    }
    if (g_recUnclassifiedContactRequired) {
        m.reviewRequired.push_back("one or more contacts could not be classified");
    }
    if (g_recContactJournalOverflowObserved) {
        m.reviewRequired.push_back("exact contact journal overflowed during recording");
    }
    if (g_recLegacyContactAttributionRequired) {
        m.reviewRequired.push_back("legacy aggregate contact attribution was used");
    }
    if (g_recMixedDirectResultsRequired) {
        m.reviewRequired.push_back(
            "one direct action produced mixed contact results that need author review");
    }
    if (g_recUnboundCommandOriginRequired) {
        m.reviewRequired.push_back(
            "an entity-only command transition had no unambiguous causal S-button action");
    }
    if (g_recEntityTraceDropped > 0) {
        m.reviewRequired.push_back("raw entity trace overflowed during recording");
    }
    if (g_recEntityProbeIncomplete[0]) {
        m.reviewRequired.push_back(
            "entity ring/cursor sampling was incomplete during recording");
    }
    if (g_recEntityAllocationAmbiguous[0]) {
        m.reviewRequired.push_back(
            "an entity allocation occurred between samples without a visible spawn");
    }
    if (g_recUncommittedAttackFrame >= 0 && g_recPendingAttackCount > 0) {
        std::ostringstream detail;
        detail << g_recPendingAttackCount
               << " attack input batch(es) did not become recorded moves: ";
        for (std::size_t i = 0; i < g_recPendingAttackCount; ++i) {
            if (i) detail << ", ";
            const PendingRecorderAttack& pending = g_recPendingAttacks[i];
            detail << DecodeInputMask(pending.mask)
                   << " at frame " << pending.frame
                   << " (poll " << pending.serial << ")";
            LogOut("[MISSION][REC][INPUT] unmatched " +
                   DecodeInputMask(pending.mask) + " frame=" +
                   std::to_string(pending.frame) + " poll=" +
                   std::to_string(pending.serial), true);
        }
        detail << "; kept only in the exact demonstration";
        m.recordingDiagnostics.push_back(detail.str());
    }
    if (g_recPendingAttackOverflow) {
        m.reviewRequired.push_back(
            "too many unmatched attack inputs were observed to retain them all");
    }
    if (!g_recTakeIntegrityValid) {
        m.reviewRequired.push_back(
            "recording integrity failed: " + g_recTakeIntegrityError);
    }
    // Demo = the P1 clip captured in lockstep with this authoring session. It is
    // ephemeral and never aliases/overwrites the user's ordinary P2 macro slot.
    m.demo = g_recSealedDemo;
    m.scores.push_back(::Mission::ScoreTier{ 0, 0, 0, "Clear" });
    out = std::move(m);
    return true;
}

bool SaveRecorded(std::string& outMsg) {
    ::Mission::Authoring::MissionMetadata metadata;
    metadata.name = "Recorded Combo";
    metadata.type = "combo";
    // The legacy quick-save path produced unrated drafts. Keep that behaviour
    // for callers that do not open the new details form.
    metadata.difficulty = 0;
    return SaveRecordedDraft(metadata, outMsg);
}

namespace {

bool PrepareRecordedForSave(const ::Mission::Authoring::MissionMetadata& metadata,
                            ::Mission::Mission& outMission,
                            std::string& outMsg) {
    const Phase phase = GetPhase();
    if (phase != Phase::Review) {
        outMsg = "finish the capture and enter Review first";
        return false;
    }
    if (!BuildDraft(outMission)) { outMsg = "no steps captured"; return false; }
    if (outMission.savestate.empty()) {
        outMsg = "exact start state is missing; retake the recording";
        return false;
    }
    if (!metadata.name.empty()) outMission.name = metadata.name;
    if (!metadata.description.empty()) outMission.description = metadata.description;
    if (!metadata.type.empty()) outMission.type = metadata.type;
    if (metadata.difficulty >= 0 && metadata.difficulty <= 5) {
        outMission.difficulty = metadata.difficulty;
    }
    return true;
}

void FinishSuccessfulRecordedSave(const ::Mission::Mission& mission,
                                  const std::string& acceptedPath) {
    if (HasAuthorReviewObligations(mission)) {
        DirectDrawHook::AddMessage(
            "Recording saved for author review; see its entity trace sidecar",
            "MISSION", RGB(255, 200, 120), 3600, 0, 120);
        LogOut("[MISSION][REC] saved with " +
               std::to_string(mission.reviewRequired.size()) +
               " unresolved authoring obligation(s)", true);
    }
    g_recPhase.store(Phase::Idle, std::memory_order_release);
    ClearRecorderCaptureMenuSuspension();
    DirectDrawHook::RemoveMessagesByCategory("mission_record");
    RemoveRecorderBanner();
    CustomMenu::Screens::NotifyMissionLibraryChanged();
    LogOut("[MISSION][REC] saved " + std::to_string(mission.steps.size()) +
           " step(s) -> " + acceptedPath, true);
}

void LogRecordedTraceCommitted(const std::string& tracePath) {
    LogOut("[MISSION][REC][ENTITY] committed " +
           std::to_string(g_recEntityTraceCount) +
           " raw event(s), dropped=" +
           std::to_string(g_recEntityTraceDropped) + " -> " + tracePath, true);
}

bool ReservePublishedTraceStagingPath(const std::string& packJsonPath,
                                      std::string& outPath,
                                      std::string& outError) {
    const std::filesystem::path folder =
        std::filesystem::path(packJsonPath).parent_path();
    if (folder.empty()) {
        outError = "cannot resolve the destination pack folder";
        return false;
    }
    constexpr int kMaxAttempts = 128;
    for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
        const unsigned long sequence =
            g_recSaveSequence.fetch_add(1, std::memory_order_relaxed) + 1;
        const std::string fileName =
            ".recorder-entity-trace-" +
            std::to_string(static_cast<unsigned long>(GetCurrentProcessId())) +
            "-" + std::to_string(static_cast<unsigned long>(GetTickCount())) +
            "-" + std::to_string(sequence) + ".tmp";
        const std::filesystem::path candidate = folder / fileName;
        HANDLE file = CreateFileA(candidate.string().c_str(), GENERIC_WRITE, 0,
                                  nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL,
                                  nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            CloseHandle(file);
            outPath = candidate.string();
            return true;
        }
        const DWORD code = GetLastError();
        if (code == ERROR_FILE_EXISTS || code == ERROR_ALREADY_EXISTS) continue;
        outError = "cannot stage the entity trace beside the destination pack "
                   "(Windows error " +
                   std::to_string(static_cast<unsigned long>(code)) + ")";
        return false;
    }
    outError = "cannot allocate a unique entity trace staging file";
    return false;
}

} // namespace

bool SaveRecordedDraft(const ::Mission::Authoring::MissionMetadata& metadata,
                       std::string& outMsg) {
    ::Mission::Mission m;
    if (!PrepareRecordedForSave(metadata, m, outMsg)) return false;

    const std::string root = ::Mission::ResolveMissionsRoot();
    if (root.empty()) { outMsg = "cannot resolve missions root"; return false; }
    const std::string dir = root + "\\_recorded";
    CreateDirectoryA(root.c_str(), nullptr);
    CreateDirectoryA(dir.c_str(), nullptr);

    std::string err;
    std::string acceptedPath;
    std::string reservationPath;
    if (!ReserveRecordedMissionPath(dir, acceptedPath, reservationPath, err)) {
        outMsg = err;
        return false;
    }
    const std::string stagedPath = acceptedPath + ".tmp";
    const std::string tracePath = RecorderEntityTracePathForMission(acceptedPath);
    const std::string stagedTracePath = tracePath + ".tmp";
    DeleteFileA(stagedPath.c_str());
    DeleteFileA(stagedTracePath.c_str());
    if (!::Mission::SaveMission(stagedPath, m, err)) {
        // The non-JSON reservation and staging file belong to this attempt.
        // Neither may become a browser entry after serialization fails.
        DeleteFileA(stagedPath.c_str());
        DeleteFileA(reservationPath.c_str());
        outMsg = err;
        return false;
    }
    if (!WriteRecorderEntityTraceSidecarAtPath(stagedTracePath, err)) {
        DeleteFileA(stagedPath.c_str());
        DeleteFileA(stagedTracePath.c_str());
        DeleteFileA(reservationPath.c_str());
        outMsg = "cannot stage the recording's entity trace: " + err;
        LogOut("[MISSION][REC][ENTITY] draft staging failed: " + err, true);
        return false;
    }
    // The sidecar is promoted first. It is not browser-visible and can be
    // rolled back if the final mission commit marker cannot be claimed.
    if (!MoveFileExA(stagedTracePath.c_str(), tracePath.c_str(),
                     MOVEFILE_WRITE_THROUGH)) {
        const DWORD code = GetLastError();
        DeleteFileA(stagedPath.c_str());
        DeleteFileA(stagedTracePath.c_str());
        DeleteFileA(reservationPath.c_str());
        outMsg = "cannot commit the recording's entity trace (Windows error " +
                 std::to_string(static_cast<unsigned long>(code)) + ")";
        return false;
    }
    if (!MoveFileExA(stagedPath.c_str(), acceptedPath.c_str(),
                     MOVEFILE_WRITE_THROUGH)) {
        const DWORD code = GetLastError();
        // The mission is the visibility/commit marker. Remove the trace we
        // promoted in this transaction if that final claim fails.
        std::string rollbackDetail;
        if (!DeleteFileA(tracePath.c_str())) {
            const DWORD rollbackCode = GetLastError();
            if (rollbackCode != ERROR_FILE_NOT_FOUND) {
                rollbackDetail = "; rollback could not remove the entity trace "
                                 "(Windows error " +
                    std::to_string(static_cast<unsigned long>(rollbackCode)) +
                    ")";
            }
        }
        DeleteFileA(stagedPath.c_str());
        DeleteFileA(reservationPath.c_str());
        outMsg = "cannot commit recorded draft (Windows error " +
                 std::to_string(static_cast<unsigned long>(code)) + ")" +
                 rollbackDetail;
        LogOut("[MISSION][REC][ENTITY] draft mission commit failed; trace rolled back: " +
               outMsg, true);
        return false;
    }
    if (!DeleteFileA(reservationPath.c_str())) {
        const DWORD code = GetLastError();
        if (code != ERROR_FILE_NOT_FOUND) {
            LogOut("[MISSION][REC] saved draft but could not remove reservation " +
                   reservationPath + " (Windows error " +
                   std::to_string(static_cast<unsigned long>(code)) + ")", true);
        }
    }
    LogRecordedTraceCommitted(tracePath);
    outMsg = acceptedPath;
    FinishSuccessfulRecordedSave(m, acceptedPath);
    return true;
}

bool CanPublishRecorded(std::string& outReason) {
    outReason.clear();
    if (GetPhase() != Phase::Review) {
        outReason = "finish the capture and enter Review first";
        return false;
    }
    ::Mission::Mission mission;
    if (!BuildDraft(mission)) {
        outReason = "no steps were captured";
        return false;
    }
    if (mission.savestate.empty()) {
        outReason = "the exact start state is missing; retake the recording";
        return false;
    }
    for (const std::string& reason : mission.reviewRequired) {
        if (!IsKnownRuntimeMarker(reason)) {
            outReason = reason;
            return false;
        }
    }
    return true;
}

bool PublishRecorded(const std::string& packJsonPath,
                     const std::string& categoryId,
                     const ::Mission::Authoring::MissionMetadata& metadata,
                     std::string& outMsg) {
    ::Mission::Mission mission;
    if (!PrepareRecordedForSave(metadata, mission, outMsg)) return false;
    if (HasAuthorReviewObligations(mission)) {
        outMsg = "this take still needs author review; save it as a draft or retake it";
        return false;
    }

    const std::string missionsRoot = ::Mission::ResolveMissionsRoot();
    if (missionsRoot.empty()) {
        outMsg = "cannot resolve missions root";
        return false;
    }

    std::string stagedTracePath;
    if (!ReservePublishedTraceStagingPath(packJsonPath, stagedTracePath, outMsg)) {
        return false;
    }
    std::string traceError;
    if (!WriteRecorderEntityTraceSidecarAtPath(stagedTracePath, traceError)) {
        DeleteFileA(stagedTracePath.c_str());
        outMsg = "cannot stage the recording's entity trace: " + traceError;
        LogOut("[MISSION][REC][ENTITY] publish staging failed: " + traceError,
               true);
        return false;
    }

    ::Mission::Authoring::PublishResult published;
    if (!::Mission::Authoring::PublishMission(missionsRoot,
                                               packJsonPath, categoryId,
                                               mission, metadata,
                                               published, outMsg,
                                               stagedTracePath)) {
        // The authoring service may have consumed the staging file before a
        // later manifest failure. DeleteFile is deliberately harmless then.
        DeleteFileA(stagedTracePath.c_str());
        return false;
    }

    if (published.entityTracePath.empty()) {
        // The service contract guarantees this for a non-empty staging path;
        // retain a deterministic diagnostic destination if an older mixed
        // object file ever violates that result-only invariant. The artifact
        // transaction itself has already committed successfully here.
        published.entityTracePath =
            RecorderEntityTracePathForMission(published.missionPath);
        LogOut("[MISSION][REC][ENTITY] publication result omitted trace path; "
               "using the mission-derived destination", true);
    }
    LogRecordedTraceCommitted(published.entityTracePath);
    outMsg = published.missionPath;
    FinishSuccessfulRecordedSave(mission, published.missionPath);
    return true;
}

} // namespace Recorder

namespace Runner {

static void ResetProgress() {
    g_runPhase = Phase::Idle;
    g_runStep = 0;
    g_runFailedStep.store(-1);
    g_runFailedEntityRequirement.store(-1, std::memory_order_release);
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
    g_runAwaitingComboEndAfterSequence = 0;
    g_runPrevHitState = 0;
    ResetRunContactEvidence();
    ResetRunEntityEvidence();
    ResetRunScore();
}

void ResetAfterWorldRestore() {
    std::lock_guard<std::recursive_mutex> lock(g_runStateMx);
    if (g_runPhase == Phase::InProgress || g_runPhase == Phase::Complete) {
        ++g_runAttempts;
    }
    ResetProgress();
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
    // Capability can disappear after the earlier browser/title preflight
    // (for example during hook teardown).  Recheck before clearing the prior
    // tutorial handoff, cancelling input owners, or publishing runner state.
    if (!::Mission::SequencePolicy::EmbeddedSavestateCanLoad(
            !mission.savestate.empty(), ::Mission::StateDump::Available())) {
        outMsg = "mission carries an exact start state, but savestate "
                 "restore is unavailable";
        return false;
    }
    // Replacing the loaded runner is also a tutorial-session reset. Retire a
    // terminal handoff whose old Demo phase is already Idle before publishing
    // the new mission and its input owners.
    ClearDemoTutorialHandoff(true);
    // A mission startup owns P1 until its exact baseline is ready. The
    // dedicated startup gate is intentionally outside generic macro/action
    // cleanup, so an inherited title owner stays authoritative here.
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
            SetRunnerStartupInputHold(false);
            outMsg = g_runMission.name.empty() ? sourcePath : g_runMission.name;
            LogOut("[MISSION][RUN] tutorial setup failed; retained durable Error: " +
                   failureMessage, true);
            return true;
        }

        // Ordinary missions have no startup Error frontend. Tear their
        // publication down completely rather than leaving an inactive runner
        // or a previous tutorial/dummy lease behind.
        SetRunnerStartupInputHold(false);
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
        g_demoRoundRestoreSettleTicks = 0;
        g_demoTerminalRecipeHoldTicks = 0;
        g_demoRecipeSettleTicksRemaining = 0;
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
    if (::Mission::EntitySchedulePolicy::RequiresRestoreBoundReset(
            EntityScheduleRuntimeMarkerVersion(g_runMission),
            !g_runMission.entityContacts.empty() ||
                !g_runMission.entityLifecycles.empty())) {
        LogOut("[MISSION][RUN] refused logical-only reset for strict "
               "entity schedule; restore the runner baseline instead", true);
        return;
    }
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
    if (ok) {
        SkipRoundIntro("tutorial checkpoint restore");
        ResetProgress();
        std::string lineageError;
        if (!InitializeRunEntityLineageBaseline(lineageError)) {
            outMsg = "strict entity lineage baseline failed: " + lineageError;
            return false;
        }
    } else outMsg = "baseline load failed";
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
    if (g_runStep >= 0 &&
        g_runStep < static_cast<int>(g_runMission.steps.size()) &&
        g_runMission.steps[static_cast<std::size_t>(g_runStep)].directContact) {
        return g_runDirectHits;
    }
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
                       int& failedStepOut, bool& armedOut, int& currentHitsOut,
                       std::vector<int>* entityContactsSeenOut,
                       std::vector<int>* entityComboHitsSeenOut,
                       int* entitySegmentOut,
                       int* failedEntityRequirementOut,
                       bool requireVisibleRecipe,
                       std::vector<int>* entityLifecyclesSeenOut) {
    std::lock_guard<std::recursive_mutex> lock(g_runStateMx);
    if (!g_runActive.load(std::memory_order_acquire)) return false;
    if (requireVisibleRecipe) {
        const bool demoRecipe =
            ::Mission::SequencePolicy::DemoTrialRecipeActive(
                g_demoPhase.load(std::memory_order_acquire) ==
                    DemoPhase::Playing,
                g_demoFromRecorder,
                /*runnerActive=*/true,
                !g_runMission.steps.empty());
        const bool playerRecipe =
            !g_runRestorePending && !g_runSavePending &&
            !g_runStartupInputHeld.load(std::memory_order_acquire) &&
            g_demoPhase.load(std::memory_order_acquire) == DemoPhase::Idle;
        if (!demoRecipe && !playerRecipe) return false;
    }

    // Copy only fields consumed by mission_render. In particular, do not copy
    // the potentially large base64 savestate or demonstration every draw.
    // The centralized projection deliberately retains player.character:
    // entity PAT identities and producer aliases are character-local.
    ::Mission::RenderSnapshot::CopyMissionView(g_runMission, missionOut);
    if (entityContactsSeenOut) {
        *entityContactsSeenOut = g_runEntityContactsSeen;
    }
    if (entityComboHitsSeenOut) {
        *entityComboHitsSeenOut = g_runEntityComboHitsSeen;
    }
    if (entityLifecyclesSeenOut) {
        entityLifecyclesSeenOut->assign(
            g_runEntityLifecycleStates.size(), 0);
        for (std::size_t i = 0; i < g_runEntityLifecycleStates.size(); ++i) {
            (*entityLifecyclesSeenOut)[i] =
                g_runEntityLifecycleStates[i].satisfied ? 1 : 0;
        }
    }
    if (entitySegmentOut) *entitySegmentOut = g_runEntitySegment;
    if (failedEntityRequirementOut) {
        *failedEntityRequirementOut =
            g_runFailedEntityRequirement.load(std::memory_order_acquire);
    }
    currentStepOut = g_runStep;
    failedStepOut = g_runFailedStep.load(std::memory_order_acquire);
    armedOut = g_runArmed;
    const bool directStep = armedOut && currentStepOut >= 0 &&
        currentStepOut < static_cast<int>(g_runMission.steps.size()) &&
        g_runMission.steps[static_cast<std::size_t>(currentStepOut)].directContact;
    const int hits = !armedOut ? 0 : directStep
        ? g_runDirectHits
        : g_snapshot.p1Combo - g_runBaseline;
    currentHitsOut = hits > 0 ? hits : 0;
    return true;
}

} // namespace Runner

} // namespace Mission::Engine
