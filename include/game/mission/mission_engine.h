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
#include <array>
#include <string>

#include "contact_event.h"

// Forward declaration so the recipe renderer can read the active mission's steps
// without this header pulling in the full data model.
namespace Mission { struct Mission; }

namespace Mission::Engine {

struct Snapshot {
    static constexpr size_t kMaxContactEvents = 16;
    bool  valid = false;   // both player bases resolved this frame
    short p1Move = 0;      // P1 current move-ID (playerBase + 0x8)
    short p2Move = 0;
    // P2 frame index is required by the tutorial dummy director to recognize
    // same-ID action re-entry (for example 5A -> 5A) without mistaking a
    // looping animation for a new scripted action.
    short p2FrameIdx = 0;
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
    // Raw P1 +0x168 producer latch retained for legacy format-1 recording and
    // diagnostics. Its values overlap across hit, defense, script, counter,
    // and entity producers; neither a value nor a 0->nonzero edge is an
    // authoritative typed-contact result.
    int p1HitState = 0;
    // Closed, game-thread contact transactions emitted by the collision hook
    // since the prior mission sample. Raw +0x168 above stays diagnostic only.
    std::array<::Mission::Contact::Event, kMaxContactEvents> contactEvents{};
    uint8_t contactEventCount = 0;
    uint32_t contactEventEpoch = 0;
    bool contactEventOverflow = false;
    bool directContactHookReady = false;
    bool entityContactHookReady = false;
};

// Per-frame update (call from the frame monitor).
void Tick();

// Title-menu MISSION entry: Practice is launched with mission mode pending; once
// the match is running, the mission browser opens itself (menu + submenu).
void SetPendingMissionMode(bool on);

// Title-menu mission selection: parse, runtime-preflight, and pin the selected
// Mission value plus its source path before Practice leaves the browser, then
// publish that exact value after the prepared match settles. False means the
// browser must remain open; no pending/direct-load ownership is published.
bool SetPendingMissionLoad(const std::string& missionPath, std::string& errorOut);

// Cancel one title-picked mission that has not yet been consumed on Match
// entry. The selected path + prepared Mission are one ownership token: both
// absent represents the separate generic "open the mission browser after
// Practice" mode and is deliberately preserved. This does not mutate
// CharacterHotswap's native Loading ownership; the frontend coordinates that
// cancellation separately. Returns true only when a specific pending mission
// was cleared.
bool CancelPendingMissionLoad(const char* reason);

namespace PendingLaunchPolicy {
    enum class CancelEffect : uint8_t {
        PreserveGenericBrowser,
        ClearSpecificMission,
    };

    constexpr CancelEffect DecideCancel(bool hasSpecificMission) {
        return hasSpecificMission
            ? CancelEffect::ClearSpecificMission
            : CancelEffect::PreserveGenericBrowser;
    }

    enum class ConsumeEffect : uint8_t {
        OpenGenericBrowser,
        LoadPreparedMission,
        RejectIncompleteTransaction,
    };

    // Path and value are published under one mutex. Keep the inconsistent
    // states explicit so a future partial-clear bug fails visibly instead of
    // reparsing a path or silently opening the generic browser.
    constexpr ConsumeEffect DecideConsume(bool hasSelectedMissionPath,
                                           bool hasPreparedMission) {
        if (hasSelectedMissionPath && hasPreparedMission) {
            return ConsumeEffect::LoadPreparedMission;
        }
        if (!hasSelectedMissionPath && !hasPreparedMission) {
            return ConsumeEffect::OpenGenericBrowser;
        }
        return ConsumeEffect::RejectIncompleteTransaction;
    }

    // A concrete title launch must retire when the player backs out of its
    // Character Select fallback. Otherwise its path survives into an unrelated
    // later Practice match. The generic "open browser after Practice" mode is
    // still distinguished by DecideCancel and remains preserved.
    constexpr bool ReachedSelectorExit(bool missionModePending,
                                       bool sawCharacterSelect,
                                       bool nowAtMenu) {
        return missionModePending && sawCharacterSelect && nowAtMenu;
    }
}

namespace StartupFailurePolicy {
    enum class Effect : uint8_t {
        AbortRunner,
        RetainTutorialError,
    };

    // A tutorial owns a visible recovery surface even when setup fails before
    // a baseline exists. Ordinary missions have no equivalent Error phase and
    // must still abort rather than pretending to be playable.
    constexpr Effect Decide(bool isTutorial) {
        return isTutorial ? Effect::RetainTutorialError : Effect::AbortRunner;
    }
}

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

// Central Practice-session teardown calls this synchronously. It clears
// authoring/demonstration ownership and invalidates a stale Runner/tutorial.
// Only a positively identified mission-owned Match->Loading transaction keeps
// the Runner alive across the native reload.
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
    int  GetCountInValue();    // remaining milliseconds while counting down

    // Human-readable form of the controls that actually reach Macro Record
    // (for example "KEY I / PAD LB"). Mission authoring deliberately reuses
    // that action, so authoring surfaces must not hard-code a key/button.
    std::string GetMacroRecordBindingLabel();

    // Build a draft Mission from the captured session (steps with auto-notation +
    // moveIds + req, plus the synchronized transient P1 input clip as demo).
    // Returns false if nothing was captured.
    bool BuildDraft(::Mission::Mission& out);

    // Build a Mission from the captured steps + synchronized P1 clip, then save
    // it under assets\missions\_recorded\. A sibling `.entities.jsonl` keeps the
    // cast-wide raw ring lifecycle/contact authoring trace; it is explicitly
    // sampled/unordered/non-strict and never adds format-1 steps. Returns true
    // on success (outMsg = mission path) or false (outMsg = error).
    bool SaveRecorded(std::string& outMsg);
}

// ---- Demonstration playback ----
// A loaded mission demo is restored to its mission baseline and replayed as an
// ephemeral, exclusive P1 macro. User gameplay/hotkeys are suppressed; Escape
// is the only keyboard cancellation command.
namespace Demo {
    enum class Phase : uint8_t { Idle, Preparing, Playing, Restoring };

    // Terminal result of a loaded tutorial demonstration's final baseline
    // restore.  The demo engine keeps its direct P1-neutral hold until the
    // tutorial consumes this result on the next fresh Snapshot and explicitly
    // acknowledges it. Recorder previews and ordinary mission demos never
    // create this handoff.
    enum class RestoreResult : uint8_t { None, Restored, RestoreFailed };

    constexpr bool RequiresTutorialRestoreAcknowledgement(
        bool requested, bool fromRecorder, bool tutorialSessionActive) {
        return requested && !fromRecorder && tutorialSessionActive;
    }

    bool PlayLoaded(std::string& outMsg, bool tutorialHandoff = false);
    bool PlayRecording(std::string& outMsg); // preview the unsaved Review clip
    void Cancel();
    bool IsActive();
    Phase GetPhase();

    // Peek does not release P1. Acknowledge is the ownership boundary: frozen
    // return phases must acquire their freeze first; gameplay return phases
    // acknowledge first and acquire the tutorial neutral gate in the same
    // engine tick. Both functions are no-ops when no tutorial handoff exists.
    RestoreResult PeekTutorialRestore(std::string& outMessage);
    void AcknowledgeTutorialRestore();
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
    // Restore the mission/lesson root baseline through the runner's owner
    // (the tutorial session's checkpoint transaction). False when no baseline
    // exists or a restore cannot run right now.
    bool RequestBaselineRestore(std::string& outMsg);
    // True only after this runner captured/accepted its own root checkpoint.
    // Presentation code uses this to avoid offering a restart that cannot
    // possibly succeed (notably when startup failed before the first save).
    bool HasBaseline();

    bool  IsActive();
    Phase GetPhase();
    bool  HasDemo();       // loaded mission carries a demonstration clip
                           // (GetRenderSnapshot deliberately strips the blob)
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
