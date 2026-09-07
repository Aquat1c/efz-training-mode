#pragma once
#include <windows.h>
#include <string>
#include <atomic>
#include <chrono>
#include <cstdint>

// Global state variables
extern std::atomic<bool> menuOpen;
extern std::atomic<int> frameCounter;
extern std::atomic<bool> detailedLogging;
extern std::atomic<bool> g_deepFrameAdvDebug;
extern std::atomic<bool> autoAirtechEnabled;  // New: Controls auto-airtech feature
extern std::atomic<int> autoAirtechDirection; // New: 0=forward, 1=backward
extern std::atomic<bool> autoJumpEnabled;     // Controls auto-jump feature
extern std::atomic<int> jumpDirection;        // 0=straight, 1=forward, 2=backward
extern std::atomic<bool> p1Jumping;           // Tracks if P1 is currently in jump state
extern std::atomic<bool> p2Jumping;           // Tracks if P2 is currently in jump state
extern std::atomic<int> jumpTarget;           // 1=P1, 2=P2, 3=Both
extern std::atomic<bool> inStartupPhase;      // Tracks if the application is in the startup phase
extern std::atomic<bool> g_featuresEnabled;   // NEW: Master switch for all features

// Auto-action settings - replace the single trigger system with individual triggers
extern std::atomic<bool> autoActionEnabled;
extern std::atomic<int> autoActionType;
extern std::atomic<int> autoActionCustomID;
extern std::atomic<int> autoActionPlayer;  // Cached dummy slot (1 or 2) = SwitchPlayers::GetRemotePlayerIndex().
                                           // Write-only outside the tutorial lease: the engine re-resolves the
                                           // target every tick. 3 (Both) is never produced.

// Individual trigger settings - ADD THESE MISSING DECLARATIONS
extern std::atomic<bool> triggerAfterBlockEnabled;
extern std::atomic<bool> triggerOnWakeupEnabled;
extern std::atomic<bool> triggerAfterHitstunEnabled;
extern std::atomic<bool> triggerAfterAirtechEnabled;
// New: On Recoil Guard trigger
extern std::atomic<bool> triggerOnRGEnabled;
// Global: when ON, each trigger attempt has a 50% chance to fire
extern std::atomic<bool> triggerRandomizeEnabled;

// Delay settings (in visual frames) - ADD THESE MISSING DECLARATIONS
extern std::atomic<int> triggerAfterBlockDelay;
extern std::atomic<int> triggerOnWakeupDelay;
extern std::atomic<int> triggerAfterHitstunDelay;
extern std::atomic<int> triggerAfterAirtechDelay;
// New: On RG delay
extern std::atomic<int> triggerOnRGDelay;

// Function declarations
uintptr_t GetEFZBase();
// Invalidate cached EFZ base address (use if module could reload)
void InvalidateEFZBaseCache();

// Cached game state pointer (stable after initial allocation)
uintptr_t GetGameStatePtr();
void InvalidateGameStatePtrCache();
uint32_t GetRuntimeLifecycleGeneration();
void RequestRuntimeLifecycleResync(const std::string& reason);
void ConsumeRuntimeLifecycleResyncRequests();
void LifecycleWatcherThread();

// Cached player base pointers (reinitialized on each character load).
// Returns 0 if characters not initialized or screen not in battle.
uintptr_t GetPlayerBase(int playerIndex); // playerIndex: 1 or 2
void InvalidatePlayerBaseCache();
bool IsActionable(short moveID);
// Wakeup-specific actionable check: treat CROUCH_TO_STAND_ID (7) as non-actionable for wake triggers
// to ensure execution occurs on the true neutral frame (e.g., 96 -> 7 -> 0 sequences).
bool IsBlockstun(short moveID);
bool IsRecoilGuard(short moveID);
bool IsEFZWindowActive();
HWND FindEFZWindow();
void CreateDebugConsole();
void DestroyDebugConsole(); // NEW: Free console and redirect handles
void SetConsoleVisibility(bool visible); // NEW: Show/Hide console window
void ResetFrameCounter();
void ShowHotkeyInfo();
std::string FormatPosition(double x, double y);
bool IsHitstun(short moveID);
bool IsLaunched(short moveID);
bool IsAirtech(short moveID);
bool IsGroundtech(short moveID);
bool IsFrozen(short moveID);
bool IsSpecialStun(short moveID);
bool IsThrown(short moveID);

// Explicitly clear all auto-action triggers (and auto-action) persistently.
// Use when returning to Character Select so user can re-enable manually later.
void ClearAllTriggersPersistently();

// ADD THESE MISSING FUNCTION DECLARATIONS
bool IsAttackMove(short moveID);      // From frame_advantage.cpp
bool IsBlockstunState(short moveID);  // From frame_analysis.cpp

short GetUntechValue(uintptr_t base, int player);
void WriteStartupLog(const std::string& message); // Logs messages during the startup phase
void SetStartupLogEnabled(bool enabled);          // Enable/disable startup log (call after config loads)
std::string GetKeyName(int virtualKey);
void DetectKeyBindings();
bool IsDashState(short moveID); // New: Check if in dash state
bool CanAirtech(short moveID); 

// NEW: Add feature management functions
extern std::atomic<bool> g_featuresEnabled;
void EnableFeatures();
void DisableFeatures();
void ResetDisplayDataToDefaults();
void ResetPracticeMatchSessionState(const char* reason);

// Add delay support for auto-airtech
extern std::atomic<int> autoAirtechDelay; // 0=instant, 1+=frames to wait

// For features that should inject only into immediate registers (skip buffer writes)
extern std::atomic<bool> g_injectImmediateOnly[3]; // Index 0 unused, 1=P1, 2=P2

// Display data structure
// Max number of per-trigger option rows for randomized selection
#ifndef MAX_TRIGGER_OPTIONS
#define MAX_TRIGGER_OPTIONS 8
#endif

#ifndef MAX_ACTION_POOL_OPTIONS
#define MAX_ACTION_POOL_OPTIONS 128
#endif

// A single row entry for a trigger: action choice with its own strength/button, delay and optional macro/custom
struct TriggerOption {
    bool enabled;     // whether this row participates in random selection
    int  action;      // ACTION_* enum
    int  strength;    // A/B/C or direction index for Jump; 0..2
    int  delay;       // visual frames (0 = immediate)
    int  customId;    // for custom actions (if used)
    int  macroSlot;   // 0=None, 1..MaxSlots
    int  chargeFollowup; // 0=Off, 1=IC after contact, 2=FIC before contact
};
struct DisplayData {
    int hp1, hp2;
    int meter1, meter2;
    double rf1, rf2;
    double x1, y1;
    double x2, y2;
    bool autoAirtech;
    int airtechDirection;
    int airtechDelay;
    bool autoJump;
    int jumpDirection;
    int jumpTarget;
    
    // Add character name fields
    char p1CharName[16];  // Character name with buffer for null termination
    char p2CharName[16];  // Character name 
    
    // Add character ID fields
    int p1CharID;
    int p2CharID;
    
    // Character-specific settings
    // Ikumi
    int p1IkumiBlood;
    int p2IkumiBlood;
    int p1IkumiGenocide;
    int p2IkumiGenocide;
    int p1IkumiLevelGauge; // 0..99 (100 triggers level up)
    int p2IkumiLevelGauge;
    bool infiniteBloodMode;  // Enables freeze patch for blood

    // Shiori (reuses Ikumi's per-character resource slot, player + 0x314C)
    bool infiniteShioriShield;  // Freezes Shiori's shield gauge so it never depletes

    // Misuzu
    int p1MisuzuFeathers;
    int p2MisuzuFeathers;
    bool infiniteFeatherMode;
    // Misuzu poison
    int  p1MisuzuPoisonTimer; // 0..3000
    int  p2MisuzuPoisonTimer; // 0..3000
    int  p1MisuzuPoisonLevel; // 0=inactive, nonzero=active
    int  p2MisuzuPoisonLevel; // 0=inactive, nonzero=active
    bool p1MisuzuInfinitePoison; // keep poison timer topped up
    bool p2MisuzuInfinitePoison; // keep poison timer topped up
    
    // Mishio
    int p1MishioElement;       // 0=None, 1=Fire, 2=Lightning, 3=Awakened
    int p2MishioElement;
    int p1MishioAwakenedTimer; // internal frames
    int p2MishioAwakenedTimer;
    bool infiniteMishioElement;      // freeze/restore chosen element
    bool infiniteMishioAwakened;     // keep awakened timer topped up when element==Awakened

    // Blue IC/Red IC toggle
    bool p1BlueIC;
    bool p2BlueIC;

    // NEW: Add this flag for P2 control
    bool p2ControlEnabled;
    
    // Keep these for backward compatibility
    bool autoAction;
    int autoActionType;
    int autoActionCustomID;
    int autoActionPlayer;
    
    // Individual trigger settings
    bool triggerAfterBlock;
    bool triggerOnWakeup;
    bool triggerAfterHitstun;
    bool triggerAfterAirtech;
    bool triggerOnRG; // new
    // Global randomization for triggers (coin flip per attempt)
    bool randomizeTriggers;
    
    // Delay settings
    int delayAfterBlock;
    int delayOnWakeup;
    int delayAfterHitstun;
    int delayAfterAirtech;
    int delayOnRG;
    
    // Individual action settings for each trigger
    int actionAfterBlock;
    int actionOnWakeup;
    int actionAfterHitstun;
    int actionAfterAirtech;
    int actionOnRG;
    
    // Custom action IDs for each trigger
    int customAfterBlock;
    int customOnWakeup;
    int customAfterHitstun;
    int customAfterAirtech;
    int customOnRG;

    // Add strength settings for each trigger
    int strengthAfterBlock;
    int strengthOnWakeup;
    int strengthAfterHitstun;
    int strengthAfterAirtech;
    int strengthOnRG;

    // Optional native 22C follow-up for the selected attack.
    int chargeAfterBlock;
    int chargeOnWakeup;
    int chargeAfterHitstun;
    int chargeAfterAirtech;
    int chargeOnRG;

    // Per-trigger macro selection (0=None, 1..MaxSlots)
    int macroSlotAfterBlock;
    int macroSlotOnWakeup;
    int macroSlotAfterHitstun;
    int macroSlotAfterAirtech;
    int macroSlotOnRG;

    // Doppel Nanase (ExNanase) - Enlightened FM checkbox state per player
    bool p1DoppelEnlightened;
    bool p2DoppelEnlightened;

    // Doppel Nanase (ExNanase) - how the OPPONENT escapes her command-throw
    // follow-ups. The row lives on Doppel's side because the field driven by it
    // sits on Doppel's own struct, but the behaviour described is the opponent's.
    // Mode:  0=OFF, 1=NEVER, 2=TECH B, 3=TECH C, 4=ALWAYS, 5=RANDOM
    int p1DoppelTechMode;
    int p2DoppelTechMode;
    // Stage: 0=ALL, 1=STAGE 1, 2=STAGE 2, 3=STAGE 3
    int p1DoppelTechStage;
    int p2DoppelTechStage;

    // Sayuri Kurata - the move she remembers countering, and whether Magical
    // Cutter is always available out of a grounded block.
    // Memory choice: 0=OFF, 1=NOTHING, 2=LAST BLOCKED, 3+ = one move of the
    // CURRENT opponent. The index is presentation only - the resolved move ID
    // lives in the SayuriCounter module, because the list is rebuilt per
    // opponent and the same index means a different move against the next one.
    int p1SayuriMemoryChoice;
    int p2SayuriMemoryChoice;
    // Cutter: 0=NORMAL, 1=ALWAYS READY
    int p1SayuriCutterMode;
    int p2SayuriCutterMode;

    // Nanase (Rumi) – Barehanded mode (full swap of normals+specials)
    bool p1RumiBarehanded;
    bool p2RumiBarehanded;

    // Nanase (Rumi) – Infinite Shinai: prevent weapon from being dropped (auto-restore to Shinai)
    bool p1RumiInfiniteShinai;
    bool p2RumiInfiniteShinai;

    // Nanase (Rumi) – Final Memory (Kimchi) state and controls
    bool p1RumiKimchiActive;   // reflects activation flag at +0x3148
    bool p2RumiKimchiActive;
    int  p1RumiKimchiTimer;    // reflects timer at +0x314C
    int  p2RumiKimchiTimer;
    bool p1RumiInfiniteKimchi; // keep timer topped up
    bool p2RumiInfiniteKimchi;

    // Akiko (Minase)
    int  p1AkikoBulletCycle;
    int  p2AkikoBulletCycle;
    int  p1AkikoTimeslowTrigger; // 0=inactive,1=A,2=B,3=C (legacy 4=Infinite removed)
    int  p2AkikoTimeslowTrigger;
    bool p1AkikoFreezeCycle;     // keep bullet cycle fixed at selected value
    bool p2AkikoFreezeCycle;
    bool p1AkikoShowCleanHit;    // show Clean Hit helper overlay when Akiko is P1
    bool p2AkikoShowCleanHit;    // show Clean Hit helper overlay when Akiko is P2
    // Akiko: new model for Infinite timeslow (freeze on-screen XYZ digits at 000)
    bool p1AkikoInfiniteTimeslow;
    bool p2AkikoInfiniteTimeslow;

    // Neyuki (Sleepy Nayuki) – Jam count (0..9)
    int  p1NeyukiJamCount;
    int  p2NeyukiJamCount;
    bool p1NeyukiLockJam;  // when true, restore jam count on wakeup/neutral recovery
    bool p2NeyukiLockJam;

    // Mio – stance control (0=Short,1=Long) and optional lock
    int  p1MioStance;      // cached current stance
    int  p2MioStance;
    bool p1MioLockStance;  // when true, enforce chosen stance every tick
    bool p2MioLockStance;

    // Kano – magic meter (0..10000) and locking
    int  p1KanoMagic;
    int  p2KanoMagic;
    bool p1KanoLockMagic;
    bool p2KanoLockMagic;

    // Mai (Kawasumi) – Ghost assist gauges and Awakening install
    int  p1MaiStatus;        // status byte (0=inactive,1=active,2=unsummon,3=charge,4=awakening)
    int  p1MaiGhostTime;      // remaining time of active ghost
    int  p1MaiGhostCharge;    // cooldown/charge until ghost can be summoned again
    int  p1MaiAwakeningTime;  // remaining Awakening install timer
    int  p2MaiStatus;        // status byte
    int  p2MaiGhostTime;
    int  p2MaiGhostCharge;
    int  p2MaiAwakeningTime;
    bool p1MaiInfiniteGhost;  // keep ghost time frozen at selected value
    bool p2MaiInfiniteGhost;
    bool p1MaiInfiniteCharge; // keep charge (or instantly recharge) when enabled
    bool p2MaiInfiniteCharge;
    bool p1MaiInfiniteAwakening; // keep awakening timer topped
    bool p2MaiInfiniteAwakening;
    bool p1MaiNoChargeCD;   // force charge timer to 1 when entering status 3
    bool p2MaiNoChargeCD;
    // Mai control actions (one-shot GUI triggers)
    bool p1MaiForceSummon;   // when set true by GUI, attempt safe ghost summon then auto-clear
    bool p2MaiForceSummon;
    bool p1MaiForceDespawn;  // force unsummon transition
    bool p2MaiForceDespawn;
    bool p1MaiAggressiveOverride; // allow summon even during transitional (status=2) states
    bool p2MaiAggressiveOverride;

    // Runtime (read-only) Mini-Mai ghost world coordinates (updated via RefreshLocalData scan)
    double p1MaiGhostX; // NaN if not present
    double p1MaiGhostY;
    double p2MaiGhostX; // NaN if not present
    double p2MaiGhostY;
    // Editable ghost position targets (user-entered). Not auto-synced; Apply button writes them.
    double p1MaiGhostSetX;
    double p1MaiGhostSetY;
    double p2MaiGhostSetX;
    double p2MaiGhostSetY;
    // One-shot apply flags for Mai ghost position writes (set by UI buttons)
    bool   p1MaiApplyGhostPos;
    bool   p2MaiApplyGhostPos;

    // Nayuki (Awake) – Snowbunnies timer (uses +0x3150) and infinite toggle
    int  p1NayukiSnowbunnies;   // 0..NAYUKIB_SNOWBUNNY_MAX
    int  p2NayukiSnowbunnies;   // 0..NAYUKIB_SNOWBUNNY_MAX
    bool p1NayukiInfiniteSnow;  // keep timer topped/frozen
    bool p2NayukiInfiniteSnow;

    // Minagi – Puppet (Michiru) runtime world coordinates (found by scanning slots for ID 400)
    double p1MinagiPuppetX; // NaN if not present
    double p1MinagiPuppetY;
    double p2MinagiPuppetX; // NaN if not present
    double p2MinagiPuppetY;

    // Editable Michiru position targets (user-entered). Not auto-synced; Apply/enforcement writes them when set.
    double p1MinagiPuppetSetX;
    double p1MinagiPuppetSetY;
    double p2MinagiPuppetSetX;
    double p2MinagiPuppetSetY;

    // One-shot apply flags for Michiru position writes (set by UI buttons)
    bool   p1MinagiApplyPos;
    bool   p2MinagiApplyPos;

    // Minagi – Debug and control
    bool  minagiConvertNewProjectiles; // Practice-only: convert new Minagi projectiles to Michiru (ID 400)
    bool  p1MinagiAlwaysReadied;       // Keep Michiru in Readied stance when idle (P1 Minagi)
    bool  p2MinagiAlwaysReadied;       // Same for P2 Minagi

    // Runtime cache: last observed Michiru slot per side (for sticky coordinates and state)
    int   p1MichiruCurrentId;          // current entity id in cached slot (or -1)
    int   p2MichiruCurrentId;
    double p1MichiruLastX;             // last known X (sticky)
    double p1MichiruLastY;
    double p2MichiruLastX;
    double p2MichiruLastY;
    // Michiru slot state (frame/subframe) for monitoring
    int   p1MichiruFrame;
    int   p1MichiruSubframe;
    int   p2MichiruFrame;
    int   p2MichiruSubframe;

    // Continuous Recovery (UI-configurable presets; applied on neutral return)
    // LEGACY global controls (kept for backward-compat; unused when per-player settings are used)
    bool  continuousRecoveryEnabled; // default false
    int   continuousRecoveryApplyTo; // 1=P1, 2=P2, 3=Both
    int   recoveryHpMode;            // 0=Off, 1=Max, 2=FM preset (3332), 3=Custom
    int   recoveryHpCustom;
    int   recoveryMeterMode;         // 0=Off, 1=0, 2=1000, 3=2000, 4=3000, 5=Custom
    int   recoveryMeterCustom;
    int   recoveryRfMode;            // 0=Off, 1=0, 2=1000, 3=500, 4=999, 5=Custom
    double  recoveryRfCustom;
    bool  recoveryRfForceBlueIC;     // Force IC Blue when restoring RF

    // NEW: Per-player Continuous Recovery settings (preferred)
    // P1
    bool  p1ContinuousRecoveryEnabled; // default false
    int   p1RecoveryHpMode;            // 0=Off, 1=Max, 2=FM(3332), 3=Custom
    int   p1RecoveryHpCustom;
    int   p1RecoveryMeterMode;         // 0=Off, 1=0, 2=1000, 3=2000, 4=3000, 5=Custom
    int   p1RecoveryMeterCustom;
    int   p1RecoveryRfMode;            // 0=Off, 1=0, 2=1000, 3=500, 4=999, 5=Custom
    double p1RecoveryRfCustom;
    bool  p1RecoveryRfForceBlueIC;
    // P2
    bool  p2ContinuousRecoveryEnabled; // default false
    int   p2RecoveryHpMode;
    int   p2RecoveryHpCustom;
    int   p2RecoveryMeterMode;
    int   p2RecoveryMeterCustom;
    int   p2RecoveryRfMode;
    double p2RecoveryRfCustom;
    bool  p2RecoveryRfForceBlueIC;

    // Legacy multi-action pools per trigger. Bitmask mapping follows the old
    // grouped motion index space used by the UI (0..23). Kept so older runtime
    // state can be imported into the concrete pools below.
    uint32_t afterBlockActionPoolMask;
    uint32_t onWakeupActionPoolMask;
    uint32_t afterHitstunActionPoolMask;
    uint32_t afterAirtechActionPoolMask;
    uint32_t onRGActionPoolMask;

    // Concrete multi-action pools per trigger. Low/high form a 128-bit mask
    // over explicit action+variant entries: 623A and 623B are separate bits,
    // normals use their concrete ACTION_* ids, and jumps store direction.
    uint64_t afterBlockActionPoolMaskLo;
    uint64_t afterBlockActionPoolMaskHi;
    uint64_t onWakeupActionPoolMaskLo;
    uint64_t onWakeupActionPoolMaskHi;
    uint64_t afterHitstunActionPoolMaskLo;
    uint64_t afterHitstunActionPoolMaskHi;
    uint64_t afterAirtechActionPoolMaskLo;
    uint64_t afterAirtechActionPoolMaskHi;
    uint64_t onRGActionPoolMaskLo;
    uint64_t onRGActionPoolMaskHi;

    // Per concrete pool action delays. -1 means inherit the trigger's normal delay.
    int afterBlockActionPoolDelays[MAX_ACTION_POOL_OPTIONS];
    int onWakeupActionPoolDelays[MAX_ACTION_POOL_OPTIONS];
    int afterHitstunActionPoolDelays[MAX_ACTION_POOL_OPTIONS];
    int afterAirtechActionPoolDelays[MAX_ACTION_POOL_OPTIONS];
    int onRGActionPoolDelays[MAX_ACTION_POOL_OPTIONS];

    // Per concrete pool action charge mode (0=Off, 1=IC, 2=FIC).
    int afterBlockActionPoolCharges[MAX_ACTION_POOL_OPTIONS];
    int onWakeupActionPoolCharges[MAX_ACTION_POOL_OPTIONS];
    int afterHitstunActionPoolCharges[MAX_ACTION_POOL_OPTIONS];
    int afterAirtechActionPoolCharges[MAX_ACTION_POOL_OPTIONS];
    int onRGActionPoolCharges[MAX_ACTION_POOL_OPTIONS];

    bool     afterBlockUseActionPool;
    bool     onWakeupUseActionPool;
    bool     afterHitstunUseActionPool;
    bool     afterAirtechUseActionPool;
    bool     onRGUseActionPool;

    // Per-trigger multi-row options (randomly pick one on trigger fire)
    int           afterBlockOptionCount;
    TriggerOption afterBlockOptions[MAX_TRIGGER_OPTIONS];
    int           onWakeupOptionCount;
    TriggerOption onWakeupOptions[MAX_TRIGGER_OPTIONS];
    int           afterHitstunOptionCount;
    TriggerOption afterHitstunOptions[MAX_TRIGGER_OPTIONS];
    int           afterAirtechOptionCount;
    TriggerOption afterAirtechOptions[MAX_TRIGGER_OPTIONS];
    int           onRGOptionCount;
    TriggerOption onRGOptions[MAX_TRIGGER_OPTIONS];
};

extern DisplayData displayData;

bool HasAnyAutoActionTriggerEnabled();
bool HasAnyAutoActionTriggerEnabled(const DisplayData& data);
int ResolveAutoActionTargetPlayer();

// Structure to hold detected key bindings
struct KeyBindings {
    // Input device type
    int inputDevice;      // 0=keyboard, 1=gamepad
    int gamepadIndex;     // Which gamepad (for multiple controllers)
    std::string deviceName; // Name of the detected input device
    
    // P1 direction keys
    int upKey;
    int downKey;
    int leftKey;
    int rightKey;
    
    // P1 attack buttons
    int aButton;  // Light attack
    int bButton;  // Medium attack
    int cButton;  // Heavy attack
    int dButton;  // Special
    
    // Flags to track if bindings have been detected
    bool directionsDetected;
    bool attacksDetected;
};

extern KeyBindings detectedBindings;

// Individual action settings for each trigger
extern std::atomic<int> triggerAfterBlockAction;
extern std::atomic<int> triggerOnWakeupAction;
extern std::atomic<int> triggerAfterHitstunAction;
extern std::atomic<int> triggerAfterAirtechAction;
extern std::atomic<int> triggerOnRGAction;

extern std::atomic<int> triggerAfterBlockCharge;
extern std::atomic<int> triggerOnWakeupCharge;
extern std::atomic<int> triggerAfterHitstunCharge;
extern std::atomic<int> triggerAfterAirtechCharge;
extern std::atomic<int> triggerOnRGCharge;

// Legacy per-trigger multi-action pool configuration.
// Bitmask uses old UI motion indices (0..23). Runtime imports these only when
// the concrete pool mask below is empty.
extern std::atomic<uint32_t> triggerAfterBlockActionPoolMask;
extern std::atomic<uint32_t> triggerOnWakeupActionPoolMask;
extern std::atomic<uint32_t> triggerAfterHitstunActionPoolMask;
extern std::atomic<uint32_t> triggerAfterAirtechActionPoolMask;
extern std::atomic<uint32_t> triggerOnRGActionPoolMask;

// Concrete per-trigger multi-action pool configuration.
extern std::atomic<uint64_t> triggerAfterBlockActionPoolMaskLo;
extern std::atomic<uint64_t> triggerAfterBlockActionPoolMaskHi;
extern std::atomic<uint64_t> triggerOnWakeupActionPoolMaskLo;
extern std::atomic<uint64_t> triggerOnWakeupActionPoolMaskHi;
extern std::atomic<uint64_t> triggerAfterHitstunActionPoolMaskLo;
extern std::atomic<uint64_t> triggerAfterHitstunActionPoolMaskHi;
extern std::atomic<uint64_t> triggerAfterAirtechActionPoolMaskLo;
extern std::atomic<uint64_t> triggerAfterAirtechActionPoolMaskHi;
extern std::atomic<uint64_t> triggerOnRGActionPoolMaskLo;
extern std::atomic<uint64_t> triggerOnRGActionPoolMaskHi;

extern int g_afterBlockActionPoolDelays[MAX_ACTION_POOL_OPTIONS];
extern int g_onWakeupActionPoolDelays[MAX_ACTION_POOL_OPTIONS];
extern int g_afterHitstunActionPoolDelays[MAX_ACTION_POOL_OPTIONS];
extern int g_afterAirtechActionPoolDelays[MAX_ACTION_POOL_OPTIONS];
extern int g_onRGActionPoolDelays[MAX_ACTION_POOL_OPTIONS];

extern int g_afterBlockActionPoolCharges[MAX_ACTION_POOL_OPTIONS];
extern int g_onWakeupActionPoolCharges[MAX_ACTION_POOL_OPTIONS];
extern int g_afterHitstunActionPoolCharges[MAX_ACTION_POOL_OPTIONS];
extern int g_afterAirtechActionPoolCharges[MAX_ACTION_POOL_OPTIONS];
extern int g_onRGActionPoolCharges[MAX_ACTION_POOL_OPTIONS];

extern std::atomic<bool>     triggerAfterBlockUsePool;
extern std::atomic<bool>     triggerOnWakeupUsePool;
extern std::atomic<bool>     triggerAfterHitstunUsePool;
extern std::atomic<bool>     triggerAfterAirtechUsePool;
extern std::atomic<bool>     triggerOnRGUsePool;

// Runtime copies of per-trigger option rows (populated on Apply)
extern int           g_afterBlockOptionCount;
extern TriggerOption g_afterBlockOptions[MAX_TRIGGER_OPTIONS];
extern int           g_onWakeupOptionCount;
extern TriggerOption g_onWakeupOptions[MAX_TRIGGER_OPTIONS];
extern int           g_afterHitstunOptionCount;
extern TriggerOption g_afterHitstunOptions[MAX_TRIGGER_OPTIONS];
extern int           g_afterAirtechOptionCount;
extern TriggerOption g_afterAirtechOptions[MAX_TRIGGER_OPTIONS];
extern int           g_onRGOptionCount;
extern TriggerOption g_onRGOptions[MAX_TRIGGER_OPTIONS];

// Forward dash follow-up (0=None, 1=5A,2=5B,3=5C,4=2A,5=2B,6=2C)
extern std::atomic<int> forwardDashFollowup;
extern std::atomic<bool> forwardDashFollowupDashMode;

// Custom action IDs for each trigger
extern std::atomic<int> triggerAfterBlockCustomID;
extern std::atomic<int> triggerOnWakeupCustomID;
extern std::atomic<int> triggerAfterHitstunCustomID;
extern std::atomic<int> triggerAfterAirtechCustomID;
extern std::atomic<int> triggerOnRGCustomID;

// Add a missing constant that utilities.cpp needs
#define DEFAULT_TRIGGER_DELAY 0

// Add these after the other global state variables
extern std::atomic<bool> g_efzWindowActive;
extern std::atomic<bool> g_guiActive;
// Reversible runtime suspend flag used to keep training systems passive during netplay.
extern std::atomic<bool> g_onlineModeActive;

// Enter/exit reversible netplay suspension.
void EnterNetplaySuspend();
void ExitNetplaySuspend();
void AuditNetplayMenuEntryState();

// NEW: Add these for the debug tab's manual input override feature
extern std::atomic<bool> g_manualInputOverride[3]; // Index 0 unused, 1 for P1, 2 for P2
extern std::atomic<uint8_t> g_manualInputMask[3];
extern std::atomic<bool> g_manualJumpHold[3]; // NEW: For continuous jump on hold

void UpdateWindowActiveState();

// Add these after the other global state variables
extern std::atomic<bool> g_statsDisplayEnabled;
// Debug-info overlay page (scrolled with 5/6 while the overlay is active). Below
// the always-on core stats, an EXTRA-info area is paginated: page 0 = overview,
// then one page per active entity/bullet. g_statsPageCount is published live by
// UpdateStatsDisplay (dynamic with the entity count); the 5/6 handler wraps on it.
extern std::atomic<int> g_statsPageIndex;
extern std::atomic<int> g_statsPageCount;
extern int g_statsP1ValuesId;
extern int g_statsP2ValuesId;
extern int g_statsPositionId;
extern int g_statsMoveIdId;
extern int g_statsCleanHitId; // New: Akiko Clean Hit helper line id
// New: character-specific stats line (currently used for Nayuki(Awake) snowbunnies)
extern int g_statsNayukiId;
extern int g_statsMisuzuId;
// New: character-specific stats lines for Mishio and Rumi (Nanase)
extern int g_statsMishioId;
extern int g_statsRumiId;
// New: character-specific stats lines for Ikumi and Mai
extern int g_statsIkumiId;
extern int g_statsMaiId;
// New: character-specific stats line for Minagi (Michiru puppet)
extern int g_statsMinagiId;
// New: AI control flags stats line id
extern int g_statsAIFlagsId;
// New: Blockstun/Hitstun counters line id
extern int g_statsBlockstunId; // Blockstun (logical frames)
extern int g_statsUntechId;    // Hitstun/Untech (logical frames)

// Window and key monitoring management
void ManageKeyMonitoring();

// Individual strength settings (0=A, 1=B, 2=C) - ADD THESE DECLARATIONS
extern std::atomic<int> triggerAfterBlockStrength;
extern std::atomic<int> triggerOnWakeupStrength;
extern std::atomic<int> triggerAfterHitstunStrength;
extern std::atomic<int> triggerAfterAirtechStrength;
extern std::atomic<int> triggerOnRGStrength;

// Per-trigger macro slot selections (0=None, 1..MaxSlots)
extern std::atomic<int> triggerAfterBlockMacroSlot;
extern std::atomic<int> triggerOnWakeupMacroSlot;
extern std::atomic<int> triggerAfterHitstunMacroSlot;
extern std::atomic<int> triggerAfterAirtechMacroSlot;
extern std::atomic<int> triggerOnRGMacroSlot;

// Pre-buffer Wakeup: start a 0F On-Wakeup MACRO early during state 96 so its first attack is
// buffered; OFF plays it on the first actionable frame. Wake specials always early-buffer and
// wake dashes never do, regardless of this flag.
extern std::atomic<bool> g_wakeBufferingEnabled;

// UI: Show/hide the on-screen Frame Advantage overlay (default OFF)
extern std::atomic<bool> g_showFrameAdvantageOverlay;

// Global toggle: enable/disable Counter RG early-restore behavior
extern std::atomic<bool> g_counterRGEnabled;

// Attack data structure - NEW
struct AttackData {
    // Offset 0x38: Attack type flags (likely contains high/low data)
    int attackType;  
    
    // Offset 0x3C: Active frames
    short activeFrameStart;
    short activeFrameEnd;
    
    // Other properties...
    int damage;
    int blockstun;
    int hitstun;
};

// Lightweight shared positions cache (fed by stats overlay)
// - Call UpdatePositionCache from the stats update path when fresh values are read.
// - Consumers can use TryGetCachedYPositions with a maxAgeMs freshness bound; they should
//   fall back to direct memory reads if this returns false.
void UpdatePositionCache(double p1X, double p1Y, double p2X, double p2Y);
bool TryGetCachedYPositions(double &p1Y, double &p2Y, unsigned int maxAgeMs);

// Continuous Recovery runtime settings (atomics)
// Legacy global atomics (kept for compatibility; superseded by per-player below)
extern std::atomic<bool> g_contRecoveryEnabled;       // master enable (legacy)
extern std::atomic<int>  g_contRecoveryApplyTo;       // 1=P1,2=P2,3=Both (legacy)
extern std::atomic<int>  g_contRecHpMode;             // legacy
extern std::atomic<int>  g_contRecHpCustom;           // legacy
extern std::atomic<int>  g_contRecMeterMode;          // legacy
extern std::atomic<int>  g_contRecMeterCustom;        // legacy
extern std::atomic<int>  g_contRecRfMode;             // legacy
extern std::atomic<double> g_contRecRfCustom;         // legacy
extern std::atomic<bool> g_contRecRfForceBlueIC;      // legacy

// NEW: Per-player Continuous Recovery atomics
// P1
extern std::atomic<bool> g_contRecEnabledP1;
extern std::atomic<int>  g_contRecHpModeP1;
extern std::atomic<int>  g_contRecHpCustomP1;
extern std::atomic<int>  g_contRecMeterModeP1;
extern std::atomic<int>  g_contRecMeterCustomP1;
extern std::atomic<int>  g_contRecRfModeP1;
extern std::atomic<double> g_contRecRfCustomP1;
extern std::atomic<bool> g_contRecRfForceBlueICP1;
// P2
extern std::atomic<bool> g_contRecEnabledP2;
extern std::atomic<int>  g_contRecHpModeP2;
extern std::atomic<int>  g_contRecHpCustomP2;
extern std::atomic<int>  g_contRecMeterModeP2;
extern std::atomic<int>  g_contRecMeterCustomP2;
extern std::atomic<int>  g_contRecRfModeP2;
extern std::atomic<double> g_contRecRfCustomP2;
extern std::atomic<bool> g_contRecRfForceBlueICP2;
