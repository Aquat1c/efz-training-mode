#pragma once
//
// Mission engine - per-frame runtime for combo missions.
//
// Layered so each M2 piece stacks on the previous:
//   * Snapshot     - the game state the mission logic reads each frame
//                    (move-IDs, combo count/damage, attack/stun classification).
//   * Inspector    - authoring aid: live overlay readout of the snapshot.
//   * (M2b) Recorder / (M2c) Validation build on Snapshot.
//
// Tick() is driven once per internal frame from FrameDataMonitor, right next to
// FrameBar::TickSample(), so it shares the same match-phase gating and cadence.

#include <cstdint>
#include <string>

// Forward declaration so the recipe renderer can read the active mission's steps
// without this header pulling in the full data model.
namespace Mission { struct Mission; }

namespace Mission::Engine {

struct Snapshot {
    bool  valid = false;   // both player bases resolved this frame
    short p1Move = 0;      // P1 current move-ID (playerBase + 0x8)
    short p2Move = 0;
    int   p1Combo = 0;     // P1's combo hit count (attacker counter, +0x174)
    int   p1ComboDamage = 0; // P1's combo damage (+0x100)
    uint8_t p1Inputs = 0;  // sampled post-poll P1 input mask (deadline grace only)
    uint8_t p1PolledAttackEdges = 0; // attack edges consumed by EFZ since last Tick
    uint32_t p1InputPollSerial = 0;  // diagnostic poll serial for those edges
    bool  p1Attacking = false; // IsAttackMove(p1Move)
    bool  p2InStun = false;    // P2 in hitstun / launched / blockstun (opponent "in combo")
    // Engine command index for P1 (MOTION_TOKEN_OFFSET, +0x262). The detector writes
    // this the frame a motion is recognized (99 = none); it is FACING-INDEPENDENT
    // (facing already resolved), so it confirms which special/command produced a move
    // without the raw-buffer facing ambiguity. Pairs with p1Move to map commands.
    short p1Token = 99;
    // P1 frame index within the current move (+0xA). Re-canceling a move into
    // itself (5A chain -> 5A) keeps the same move-ID but resets this - the only
    // reliable "new instance of the same move" edge.
    short p1FrameIdx = 0;
    // Global freeze this tick: superflash (+0x14C, IC/super spellflash - engine
    // gameplay timers stop while nonzero on either player) or contact hitstop
    // (+0x14A). Mission timers (fail/gap/delay windows) must not tick during it,
    // otherwise an IC's flash eats the whole fail timer and drops the run.
    bool freezeActive = false;
    // P1 hit-state machine (+0x168): 0=none, 2=block/RG, 3=hit, 6=throw,
    // 7=special. An edge from 0 to nonzero = this side's attack made CONTACT -
    // distinguishes a deliberate whiff (never leaves 0) from hit vs blocked.
    int p1HitState = 0;
};

// Per-frame update (call from the frame monitor).
void Tick();

// Title-menu MISSION entry: Practice is launched with mission mode pending; once
// the match is running, the mission browser opens itself (menu + submenu).
void SetPendingMissionMode(bool on);

// Title-menu mission selection: after Practice launches and the match settles,
// load this mission directly (Setup::Apply hotswaps characters/stage as needed).
void SetPendingMissionLoad(const std::string& missionPath);

// Title-menu RECORD entry: Practice launches with recording pending; once the
// match settles it enters PreRecord. The normal Macro Record action starts the
// count-in and later stops into Review; saving is an explicit menu action.
// Mutually exclusive with the pending mission mode/load above.
void SetPendingRecordMode(bool on);

// Called by the savestate hook after ANY Revival state restore. The recorder
// starts a fresh attempt (timing across a rollback is meaningless) and a manual
// load during a run resets the runner's progress; the runner's own auto-retry
// loads are filtered out internally.
void NotifyStateLoaded();

// Central Practice-session teardown calls this synchronously. It clears only
// mission authoring/demonstration ownership; Runner state is deliberately kept
// because an in-match mission load must survive its own native reload.
void NotifyPracticeSessionReset(const char* reason);

// Latest sampled snapshot.
Snapshot GetSnapshot();

// ---- Inspector (authoring / hand-edit aid) ----
// When enabled, Tick() draws a live readout of P1/P2 move-IDs + combo state via
// the overlay message layer, so mission authors can read the move-ID each attack
// produces (that is what fills a step's moveIds).
void SetInspectorEnabled(bool on);
bool IsInspectorEnabled();

// ---- Recorder (hybrid authoring) ----
// While active, Tick() watches P1's move-ID and captures the ordered sequence of
// attack moves performed (a new distinct attack = a new step; a combo-count rise
// while the move is active marks the step as "landed"). This auto-fills a
// mission's step.moveIds so authors never hand-map IDs.
namespace Recorder {
    enum class Phase : uint8_t { Idle, PreRecord, CountIn, Recording, Review };
    enum class AdvanceEffect : uint8_t {
        None,
        BeginCountIn,
        CancelCountIn,
        StopToReview,
        KeepReview,
    };

    constexpr AdvanceEffect DecideAdvance(Phase phase) {
        switch (phase) {
            case Phase::PreRecord: return AdvanceEffect::BeginCountIn;
            case Phase::CountIn:   return AdvanceEffect::CancelCountIn;
            case Phase::Recording: return AdvanceEffect::StopToReview;
            case Phase::Review:    return AdvanceEffect::KeepReview;
            case Phase::Idle:
            default:               return AdvanceEffect::None;
        }
    }

    // Commands are consumed by Tick() on the frame-monitor thread. This keeps
    // setup capture, step capture, and macro capture on one owner thread.
    void Arm();                // Idle/Review -> PreRecord (no capture yet)
    // Macro Record action: PreRecord starts CountIn, CountIn cancels safely,
    // Recording stops into Review, and Review never mutates the saved take.
    void Advance();
    void Retake();             // Review -> restore baseline -> PreRecord
    void Cancel();             // discard the current authoring session
    void SetActive(bool on);   // legacy wrapper: on=Arm, off=Cancel
    bool IsActive();           // true only while frames are being captured
    bool IsSessionActive();    // PreRecord, CountIn, Recording, or Review
    bool OwnsCaptureHotkeys(); // CountIn/Recording: suppress unrelated Practice shortcuts
    Phase GetPhase();
    int  GetStepCount();       // steps captured so far
    int  GetComboEndCount();   // explicit setup/okizeme boundaries committed in this take
    int  GetCountInValue();    // 3..1 while counting down, otherwise 0

    // Human-readable form of the controls that actually reach Macro Record
    // (for example "KEY I / PAD LB"). Mission authoring deliberately reuses
    // that action, so authoring surfaces must not hard-code a key/button.
    std::string GetMacroRecordBindingLabel();

    // Build a draft Mission from the captured session (steps with auto-notation +
    // moveIds + req, plus the synchronized transient P1 input clip as demo).
    // Returns false if nothing was captured.
    bool BuildDraft(::Mission::Mission& out);

    // Build a Mission from the captured steps + synchronized P1 clip, then save
    // it under assets\missions\_recorded\. Returns true on
    // success (outMsg = path) or false (outMsg = error).
    bool SaveRecorded(std::string& outMsg);
}

// ---- Demonstration playback ----
// A loaded mission demo is restored to its mission baseline and replayed as an
// ephemeral, exclusive P1 macro. User gameplay/hotkeys are suppressed; Escape
// is the only keyboard cancellation command.
namespace Demo {
    enum class Phase : uint8_t { Idle, Preparing, Playing, Restoring };
    bool PlayLoaded(std::string& outMsg);
    bool PlayRecording(std::string& outMsg); // preview the unsaved Review clip
    void Cancel();
    bool IsActive();
    Phase GetPhase();
}

// ---- Runner (M2c): exact-sequence validation state machine ----
// Loads a mission and, each frame, tracks the player's progress through the
// ordered steps: start on step 0, advance when the player performs the next
// step's move and meets its requirement (land = combo count rose), consume an
// explicitly authored comboEndAfter recovery or otherwise drop when a live combo
// dies, and complete when all steps are done.
namespace Runner {
    enum class Phase { Idle, InProgress, Complete, Dropped };

    // forceFreshMatch is true for an in-match mission change. Title launches
    // already completed their direct Loading bootstrap and pass false.
    bool Load(const std::string& missionPath, std::string& outMsg,
              bool forceFreshMatch = true);
    bool LoadLatestRecorded(std::string& outMsg); // newest assets\missions\_recorded\*.json
    void Unload();
    void Reset();          // retry: keep mission, reset progress, ++attempts

    bool  IsActive();
    Phase GetPhase();
    int   CurrentStep();   // steps satisfied so far (0..count)
    int   FailedStep();    // latched failed recipe index (-1 until/after a clean retry)
    int   StepCount();
    int   Attempts();
    int   BestTier();      // highest score tier met on the completing run (-1 = none)
    int   CurrentStepHits(); // hits landed toward the current (armed) step
    bool  CurrentStepArmed(); // current step's move performed, waiting on its hit(s)
    bool  IsReadyForPlayer(); // setup/baseline complete and normal attempt UI may render

    // Coherent copy for the render thread; mission data and runner status are
    // published under one lock so loading another mission cannot invalidate UI
    // strings/vectors mid-draw.
    bool GetRenderSnapshot(::Mission::Mission& missionOut, int& currentStepOut,
                           int& failedStepOut, bool& armedOut, int& currentHitsOut);
}

} // namespace Mission::Engine
