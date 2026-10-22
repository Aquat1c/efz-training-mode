#pragma once

#include <cstdint>
#include <array>
#include <string>
#include <vector>

#include "contact_event.h"

namespace Mission {
struct Mission;
namespace Authoring { struct MissionMetadata; }
}

namespace Mission::Engine {

struct Snapshot {
    // Must match the producer journal capacity; smaller drains discard dense barrages.
    static constexpr size_t kMaxContactEvents = 128;
    static constexpr size_t kMaxPolledAttackEdges = 128;
    struct PolledAttackEdge {
        uint32_t serial = 0;
        uint8_t mask = 0;
    };
    bool  valid = false;   // both player bases resolved this frame
    short p1Move = 0;      // P1 current move-ID (playerBase + 0x8)
    short p2Move = 0;
    short p2FrameIdx = 0;
    int   p1Combo = 0;     // attacker combo-hit counter (+0x174)
    int   p1ComboDamage = 0; // (+0x100)
    uint8_t p1Inputs = 0;  // sampled post-poll
    uint8_t p1PolledAttackEdges = 0;
    uint32_t p1PolledAttackEdgeSerial = 0;
    // Ordered per-poll evidence; the aggregates above cannot preserve A,B vs coalesced A.
    std::array<PolledAttackEdge, kMaxPolledAttackEdges>
        p1PolledAttackEdgeEvents{};
    uint8_t p1PolledAttackEdgeCount = 0;
    uint32_t p1PolledAttackEdgesDropped = 0;
    bool p1PolledAttackEdgeOverflow = false;
    uint32_t p1InputPollSerial = 0;
    bool  p1Attacking = false; // IsAttackMove(p1Move)
    bool  p2InStun = false;    // hitstun / launched / blockstun
    short p1Token = 99;        // engine command index (+0x262, MOTION_TOKEN_OFFSET; 99 = none), facing-independent
    short p1FrameIdx = 0;      // frame index within current move (+0xA); reset marks same-move re-entry
    // superflash (+0x14C) or contact hitstop (+0x14A); mission timers must not tick during it
    bool freezeActive = false;
    // raw +0x168 producer latch; values overlap producers and are not a typed-contact result
    int p1HitState = 0;
    // closed game-thread contact transactions from the collision hook since the prior Tick
    std::array<::Mission::Contact::Event, kMaxContactEvents> contactEvents{};
    uint8_t contactEventCount = 0;
    uint32_t contactEventEpoch = 0;
    // older-batch events precede a newly observed step
    uint32_t completedBattleUpdateBatch = 0;
    bool contactEventOverflow = false;
    bool directContactHookReady = false;
    bool entityContactHookReady = false;
};

// Per-frame update (call from the frame monitor).
void Tick();

// Title-menu MISSION entry: the mission browser opens itself once the match is running.
void SetPendingMissionMode(bool on);

// Pins the selected Mission + source path before Practice leaves the browser.
// False means the browser must remain open.
bool SetPendingMissionLoad(const std::string& missionPath, std::string& errorOut);

// Read-only preflight; does not publish or mutate a pending launch transaction.
bool ValidateMissionRuntimeReadiness(const ::Mission::Mission& mission,
                                     const std::string& sourcePath,
                                     std::string& outError);

// Clears one unconsumed title-picked mission; the generic "open browser after
// Practice" mode is preserved. True only when a specific pending mission was cleared.
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

    // Path and value publish under one mutex; partial states must fail visibly.
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
    // Character Select fallback, or its path survives into a later Practice match.
    constexpr bool ReachedSelectorExit(bool missionModePending,
                                       bool sawCharacterSelect,
                                       bool nowAtMenu) {
        return missionModePending && sawCharacterSelect && nowAtMenu;
    }

    // One zero-poll owner must span title transaction through runner
    // restore/save with no release frame between.
    constexpr bool StartupInputOwned(bool concreteTitlePending,
                                     bool runnerRestorePending,
                                     bool runnerSavePending) {
        return concreteTitlePending || runnerRestorePending || runnerSavePending;
    }

    constexpr bool CanReleaseStartupInput(bool concreteTitlePending,
                                          bool runnerRestorePending,
                                          bool runnerSavePending,
                                          bool tutorialActive,
                                          bool tutorialOwnerEstablished) {
        return !StartupInputOwned(concreteTitlePending,
                                  runnerRestorePending,
                                  runnerSavePending) &&
               (!tutorialActive || tutorialOwnerEstablished);
    }
}

namespace StartupFailurePolicy {
    enum class Effect : uint8_t {
        AbortRunner,
        RetainTutorialError,
    };

    // A tutorial keeps a visible error surface; ordinary missions must abort.
    constexpr Effect Decide(bool isTutorial) {
        return isTutorial ? Effect::RetainTutorialError : Effect::AbortRunner;
    }
}

namespace StartupRestorePolicy {
    // Controller ownership is a restore precondition, not a terminal restore error.
    constexpr bool CanAttempt(bool worldSettled,
                              bool practiceControllerReady) {
        return worldSettled && practiceControllerReady;
    }
}

// Title-menu RECORD entry: enters PreRecord once the match settles.
// Mutually exclusive with the pending mission mode/load above.
void SetPendingRecordMode(bool on);

// Called by the savestate hook after any Revival state restore; the runner's
// own auto-retry loads are filtered out internally.
void NotifyStateLoaded();

// Practice-session teardown; only a mission-owned Match->Loading transaction
// keeps the Runner alive across the native reload.
void NotifyPracticeSessionReset(const char* reason);

Snapshot GetSnapshot();

void SetInspectorEnabled(bool on);
bool IsInspectorEnabled();

namespace Recorder {
    enum class Phase : uint8_t { Idle, PreRecord, CountIn, Recording, Review };

    // Every non-idle authoring phase owns the dedicated Recording menu,
    // separate from capture-hotkey ownership.
    constexpr bool UsesDedicatedPauseMenu(Phase phase) {
        return phase != Phase::Idle;
    }

    constexpr bool IsCapturePhase(Phase phase) {
        return phase == Phase::CountIn || phase == Phase::Recording;
    }

    // Closing a visible capture menu must not expose the close button to EFZ
    // or the synchronized macro stream.
    constexpr bool NeedsCaptureMenuHandoff(bool suspensionOwned, Phase phase) {
        return suspensionOwned && IsCapturePhase(phase);
    }

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

    // Commands are consumed by Tick() on the frame-monitor thread.
    void Arm();                // Idle/Review -> PreRecord (no capture yet)
    void Advance();
    void Retake();             // Review -> restore baseline -> PreRecord
    void Cancel();             // discard the current authoring session
    void SetActive(bool on);   // legacy wrapper: on=Arm, off=Cancel
    bool IsActive();           // true only while frames are being captured
    bool IsSessionActive();    // PreRecord, CountIn, Recording, or Review
    bool OwnsCaptureHotkeys(); // CountIn/Recording: suppress unrelated Practice shortcuts
    // Opening suspends capture and forces P1 neutral; closing resumes only
    // after fresh neutral physical polls.
    void SetCaptureMenuOpen(bool open);
    bool IsCaptureMenuSuspended();
    bool IsMenuInputHandoffActive();
    // Keeps a zero-input owner until physical P1 returns neutral, so Confirm
    // cannot become a gameplay input.
    void BeginMenuCommandHandoff();
    Phase GetPhase();
    int  GetStepCount();       // steps captured so far
    int  GetComboEndCount();   // explicit setup/okizeme boundaries committed in this take
    int  GetCountInValue();    // remaining milliseconds while counting down
    bool HasPreviewableTake(); // Review owns a sealed, parseable input clip

    // Authoring surfaces must not hard-code the Macro Record key/button.
    std::string GetMacroRecordBindingLabel();

    // Returns false if nothing was captured.
    bool BuildDraft(::Mission::Mission& out);

    // Saves to the loose `_recorded` inbox; drafts remain saveable with open
    // author-review obligations.
    bool SaveRecorded(std::string& outMsg);
    bool SaveRecordedDraft(const ::Mission::Authoring::MissionMetadata& metadata,
                           std::string& outMsg);

    // Uses the same obligation policy as PublishRecorded.
    bool CanPublishRecorded(std::string& outReason);

    // Refuses unresolved author-review obligations; failure leaves Review active.
    bool PublishRecorded(const std::string& packJsonPath,
                         const std::string& categoryId,
                         const ::Mission::Authoring::MissionMetadata& metadata,
                         std::string& outMsg);
}

// Demonstration playback: replays a demo as an ephemeral, exclusive P1 macro;
// Escape is the only keyboard cancellation command.
namespace Demo {
    enum class Phase : uint8_t { Idle, Preparing, Playing, Restoring };

    // The demo engine holds P1-neutral until the tutorial consumes this result
    // on the next fresh Snapshot and acknowledges it.
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

    // Acknowledge is the ownership boundary; both are no-ops without a
    // tutorial handoff.
    RestoreResult PeekTutorialRestore(std::string& outMessage);
    void AcknowledgeTutorialRestore();
}

// True for the complete deterministic ownership window: recorder CountIn /
// Recording, or demo Preparing / Playing / terminal Restoring.
bool IsPracticeAutomationSuppressed();

namespace Runner {
    enum class Phase { Idle, InProgress, Complete, Dropped };

    // forceFreshMatch is true for an in-match mission change; title launches pass false.
    bool Load(const std::string& missionPath, std::string& outMsg,
              bool forceFreshMatch = true);
    bool LoadLatestRecorded(std::string& outMsg); // newest assets\missions\_recorded\*.json
    void Unload();
    // Logical-only retry for legacy v1/v2 schedules; strict-v3 entity schedules
    // require RequestBaselineRestore instead.
    void Reset();
    // False when no baseline exists or a restore cannot run right now.
    bool RequestBaselineRestore(std::string& outMsg);
    // True only after this runner captured/accepted its own root checkpoint.
    bool HasBaseline();

    bool  IsActive();
    Phase GetPhase();
    bool  HasDemo();       // loaded mission carries a demonstration clip
                           // (GetRenderSnapshot strips the blob)
    int   CurrentStep();   // steps satisfied so far (0..count)
    int   FailedStep();    // latched failed recipe index (-1 until/after a clean retry)
    int   StepCount();
    int   Attempts();
    int   BestTier();      // highest score tier met on the completing run (-1 = none)
    int   CurrentStepHits(); // hits landed toward the current (armed) step
    bool  CurrentStepArmed(); // current step's move performed, waiting on its hit(s)
    bool  IsReadyForPlayer(); // setup/baseline complete and normal attempt UI may render

    // Mission data and runner status publish under one lock; requireVisibleRecipe
    // folds the player-ready gate into that same lock (pause-menu callers pass false).
    bool GetRenderSnapshot(::Mission::Mission& missionOut, int& currentStepOut,
                           int& failedStepOut, bool& armedOut, int& currentHitsOut,
                           std::vector<int>* entityContactsSeenOut = nullptr,
                           std::vector<int>* entityComboHitsSeenOut = nullptr,
                           int* entitySegmentOut = nullptr,
                           int* failedEntityRequirementOut = nullptr,
                           bool requireVisibleRecipe = false,
                           std::vector<int>* entityLifecyclesSeenOut = nullptr);
}

} // namespace Mission::Engine
