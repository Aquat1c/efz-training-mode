#pragma once
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>  
#include "../include/core/constants.h"

// Delay tracking structure
struct TriggerDelayState {
    bool isDelaying;
    int delayFramesRemaining;
    int triggerType;
    short pendingMoveID;
    // Chosen per-execution overrides (used when a per-trigger option row was selected)
    int  chosenAction;     // ACTION_* or -1 if not set
    int  chosenStrength;   // 0..2 or -1 if not set
    int  chosenMacroSlot;  // 0=None or slot index
    int  chosenCustomId;   // optional, default -1
    int  chosenDelay;      // visual-frame delay for the chosen option, or -1 if inherited
    int  chosenChargeFollowup; // 0=Off, 1=IC on contact, 2=FIC window
    // P2 buffered commands are asynchronous: submission only admits a native
    // producer/consumer transaction. Keep that generation attached to the
    // exact selected option until EFZ either consumes or rejects it.
    uint64_t pendingMotionGeneration;
    // Normal pulses have the same two-stage producer/consumer contract and
    // therefore cannot be completed at queue admission either.
    uint64_t pendingNormalGeneration;
    // Multi-command staged recipes publish their own outcome; their first
    // native motion must not be mistaken for completion of the whole action.
    uint64_t pendingRecipeGeneration;
    int      dispatchAttempts;
    // A queued pulse is advanced only by a matching fighter pass, never by
    // time, so an intent whose posture gate can never open (a j.X selected on
    // a grounded trigger, a gate held shut by another owner) would otherwise
    // wait forever - pinning isDelaying/pXTriggerActive and locking every
    // trigger out for the rest of the round. Bound the wait in internal ticks.
    int      pendingWaitTicks;
};

enum class AutoActionApplyResult : uint8_t {
    Accepted,
    Busy,
    Invalid,
};

// Global variables
extern TriggerDelayState p1DelayState;
extern TriggerDelayState p2DelayState;
extern bool p1ActionApplied;
extern bool p2ActionApplied;

// Globals to track the most recently activated trigger for overlay feedback
extern std::atomic<int> g_lastActiveTriggerType;
extern std::atomic<int> g_lastActiveTriggerFrame;

// Globals to track trigger cancelled state (flashes red on cancel)
extern std::atomic<bool> g_triggersCancelledActive;
extern std::atomic<int> g_triggersCancelledFrame;

// Function declarations
short GetActionMoveID(int actionType, int triggerType = TRIGGER_NONE, int playerNum = 2);
void ProcessTriggerDelays();
void ProcessTriggerDelays(short moveID1, short moveID2, short prevMoveID1, short prevMoveID2);
void StartTriggerDelay(int playerNum, int triggerType, short moveID, int delayFrames);
void MonitorAutoActions();
// Optimized overload: avoid per-frame memory reads by passing current/prev move IDs
void MonitorAutoActions(short moveID1, short moveID2, short prevMoveID1, short prevMoveID2);
void ResetActionFlags();
void ClearDelayStatesIfNonActionable();
void ClearDelayStatesIfNonActionable(short moveID1, short moveID2, short prevMoveID1, short prevMoveID2, const char* source);

// Function declaration for the special move logic
AutoActionApplyResult ApplyAutoAction(int playerNum, uintptr_t moveIDAddr,
                                      short currentMoveID, short prevMoveID,
                                      uint64_t* motionGenerationOut = nullptr,
                                      uint64_t* normalGenerationOut = nullptr,
                                      uint64_t* recipeGenerationOut = nullptr);

// Helper functions for motion selection
int GetSpecialMoveStrength(int actionType, int triggerType);
std::string GetTriggerName(int triggerType);

// Variables to track P2 control state for auto-actions
extern std::atomic<bool> g_p2ControlOverridden;
extern uint32_t g_originalP2ControlFlag;
// Serializes every writer of the character-level P2 AI flag.  Scoped motion
// delivery, legacy macro ownership, and the Practice synchronization thread
// must all hold this mutex while reading/changing that flag.
extern std::recursive_mutex g_p2ControlMutex;

void RestoreP2ControlState();
void EnableP2ControlForAutoAction();

// Tutorial dummy input needs exclusive, transactional ownership of P2.  The
// legacy auto-action flag is process-global and its normal restore path assumes
// that P2 started as a CPU.  These token APIs instead snapshot both live
// control flags and restore them only if the same battle objects still exist.
// While a tutorial token is held, legacy auto-actions cannot seize or restore
// P2 control.  Release is idempotent; a stale token is ignored.
bool AcquireTutorialP2Control(uint64_t& tokenOut);
bool TutorialP2ControlLeaseActive();
bool ReleaseTutorialP2Control(uint64_t token);
void ProcessAutoControlRestore();
void ProcessTriggerCooldowns();

// Existing forward declarations...
extern std::atomic<bool> autoActionEnabled;
extern std::atomic<int>  autoActionPlayer;

// ADD these externs for control-restore globals defined in auto_action.cpp
extern std::atomic<bool>  g_pendingControlRestore;
extern std::atomic<short> g_lastP2MoveID;

// Control restore / cleanup helpers
void ProcessAutoControlRestore();
void ClearAllAutoActionTriggers();
void InvalidateAutoActionCharacterCaches(const char* reason);

// Cancel active auto-actions and macros without disabling trigger settings.
// Use this when loading savestates or resetting positions to abort in-progress
// executions while preserving user-configured triggers for future activations.
void CancelAutoActionsAndMacros();

// Scoped native P2 wake producer used by tutorial reversal episodes. Acquire
// snapshots/applies the full trigger tuple under the producer mutex; Release
// synchronously cancels runtime state and conditionally restores each setting.
bool AcquireTutorialP2WakeProducer(int action, int strength, uint64_t& tokenOut);
void ReleaseTutorialP2WakeProducer(uint64_t token);

// Synchronously cancel only P2's native wake runtime. P1's menu lease and
// macros are untouched.
void CancelTutorialP2WakeProducer();

// Tick-integrated execution
// When enabled, auto-actions are evaluated once per internal engine tick from the input hook
// (right before the engine consumes inputs for that tick). The frame monitor will skip
// running its copy to avoid double-processing.
extern std::atomic<bool> g_tickIntegratedAutoActions;

// Lightweight tick entry called from the input hook (once per sub-tick, before P1 processing).
// It executes only the auto-action path using provided move IDs to avoid extra memory reads.
void AutoActionsTick_Inline(short moveID1, short moveID2);
