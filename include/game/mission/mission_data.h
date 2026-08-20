#pragma once
//
// Mission (combo trial) data model + JSON (de)serialization.
//
// Two-level structure mirrored from TrialMode (Soku), minus the Soku-specific
// bits (weather/cards/dolls/skills):
//   Pack (pack.json)  -> metadata + a list of Scenarios
//   Scenario          -> points to a Mission json file + list metadata
//   Mission (*.json)  -> player/dummy setup, ordered steps, demo, tiered scoring
//
// Validation model is exact-move-sequence (CCCaster): each Step lists the
// move-IDs that satisfy it and how (land / perform / N hits). The `demo` is an
// EFZMACRO string played back via MacroController.
//
// This header intentionally pulls in NO JSON headers (nlohmann is heavy to
// compile) - only <string>/<vector>. All JSON work lives in mission_data.cpp.

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "mission_semantic_source_policy.h"

namespace Mission {

// How a step is satisfied.
enum class StepReq : uint8_t {
    Land = 0,   // move must HIT (combo hit count increases)
    Move,       // just perform the move (whiff / movement / setup - no contact needed)
    Hits,       // move must produce `hitsRequired` hits (multi-hit)
    Connect,    // legacy format-1 "connect" requirement (hit OR blocked),
                // currently detected with the sampled +0x168 latch heuristic
};

const char* StepReqToString(StepReq req);
StepReq     StepReqFromString(const std::string& s);

// ---- Tutorial lesson model (tutorialSchema 1, TUTORIAL_MODE_DESIGN.md §5) ----

struct LessonPage {
    std::string id;
    std::string title;
    std::string text;     // rich text: {dir:6}/{btn:A}/{input:236A}/{term:..}/{ui:..}
    // While this page is shown the game HUD is hidden so the stage reads cleanly.
    // showHud reveals a group for pages that teach it: ""/"none" hides all;
    // legacy "top"/"bottom" reveal a full band; "life", "meters", "sp",
    // "rf", and "final_memory" reveal and outline the named native elements.
    std::string showHud;
};

struct ChoiceOption {
    std::string id;
    std::string label;
};

// A contact obligation is bound to the exact collision producer, not a sampled
// player-state value. `moveIds` identify the attacker's committed action (or,
// for a future entity source, the authored entity pattern family).
struct ContactPredicate {
    std::string attacker = "learner"; // learner | dummy
    std::string target = "dummy";     // learner | dummy (must be opposite)
    std::string source = "direct";    // direct | entity
    std::string result;                // hit|block|recoil_guard|throw|special|guard_point|whiff
    // Optional state of the target sampled inside the collision resolver,
    // before the contact mutates it.  This must never be approximated from the
    // next mission snapshot: by then an OTG victim is already in hitstun.
    // Empty = no pre-contact constraint; "downed" is the first strict contract.
    std::string targetStateBefore;
    std::vector<int> moveIds;
    int count = 1;                     // repetitions of the whole contact obligation
};

// One stable action inside an ordered tutorial task. This is deliberately
// smaller than a trial Step: tutorial sequences are permissive attempt scopes
// and grade only the explicitly named action/contact evidence. A positive
// inputMask grades a consumed button edge (useful for S on characters whose S
// button has no move animation); otherwise moveIds identifies the action.
struct LessonAction {
    std::string id;
    std::string notation;
    std::vector<int> moveIds;
    // When non-empty, this action is credited only if its move instance starts
    // directly out of one of these sampled move states.  This is the authored
    // cancel/transition contract: "A happened, then B happened later" is not
    // enough.  The source is intentionally attached to the destination action
    // so alternate source states can converge on the same follow-up.
    std::vector<int> fromMoveIds;
    int inputMask = 0;                   // EFZ input bits, 0 = moveIds action
    std::string req = "move";           // input|commit|move|land|hits|connect|block|rg
    int hitsRequired = 1;
    int minGap = 0;                      // non-frozen ticks after prior action
    int maxGap = 0;                      // 0 = no implicit timeout
    // wakeup_state: the action is credited only if it satisfies within this
    // many non-frozen ticks after the dummy's wake edge (the shipped
    // groundtech->actionable transition auto-action already trusts).
    // 0 = no wakeup window authored.
    int afterWakeMaxTicks = 0;
    // dummy_state: the satisfying sample must ALSO observe the dummy in this
    // state - classifier "airborne"|"downed"|"airtech"|"launched"|"blockstun"
    // (move classifiers + live Y read for airborne), or the explicit move-id
    // form. Evaluated on the SAME sample that satisfies the primary req; no
    // cross-sample inference. For a direct contact whose target is the dummy,
    // explicit move IDs bind to the collision journal's defenderMoveBefore,
    // because the ordinary snapshot already contains post-contact hitstun.
    std::string dummyState;
    std::vector<int> dummyStateMoveIds;
    // whiff_window: credited only within N non-frozen ticks after the
    // scripted dummy attack finished without touching the learner.
    int whiffWindowMaxTicks = 0;
    // episode_script: the satisfying moment must fall inside an authored GAP
    // step of the task's script episode.
    bool duringGap = false;
    bool hasContact = false;
    ContactPredicate contact;
    // Curated proof that this destination IC was performed during a
    // projectile move's whiff-only flicker window. `fromMoveIds` identifies
    // the source cast; the projectile must still be alive and must not have
    // contacted the dummy when the committed IC transition begins. This is
    // deliberately action-level because it qualifies the IC destination in
    // an ordered source -> IC route.
    bool hasFlickerIC = false;
    int flickerProjectilePattern = 0;
    int flickerMinDistance = 0;
};

// A runtime-state condition on a player. Resource fields belong to the
// `coherent_state` capability; untech/launched/airtech belong to
// `juggle_state`. A combat task may carry one as its GOAL: the task is
// satisfied when the
// condition holds, independent of any move - for objectives whose success is a
// resource level, not a specific action ("build the first SP level", "cross
// into Red RF"). Values are read live from the player object via documented
// offsets (sp=METER_OFFSET u16, rf=RF_OFFSET f64, guard=GUARD_GAUGE f32,
// hp=HP_OFFSET i32, untech=UNTECH_OFFSET i16, distance=|P1.x-P2.x| from the
// XPOS f64s, ic=IC color latch +0x120 as 0/1) or established move-state
// classifiers (launched/airtech return 0 or 1).
struct StatePredicate {
    std::string field;       // sp|rf|guard|hp|distance|ic | untech|launched|airtech
    int         player = 1;  // 1 = the learner, 2 = the dummy
    std::string op = "ge";   // ge | le | gt | lt | eq
    int         value = 0;
    // Optional witnessed-action guard for resource objectives. A threshold
    // such as RF >= 500 must not clear a "block to build RF" task merely
    // because the learner waited or evaded; at least one fresh learner
    // blockstun entry must have occurred during this task attempt.
    bool        requiresBlock = false;
};

// Optional per-task numeric baseline for one side, applied when the task arms
// and re-applied on every checkpoint restore/re-arm (task_state_seeds
// capability). This is the cheap, savestate-free answer to per-task phase
// baselines: it reuses the proven resource writers (RF double at +0x118, IC
// color latch +0x120, SP u16, HP i32). -1 = leave that resource untouched.
struct TaskStateSeed {
    bool has = false;   // any field authored
    int rf = -1;        // 0..1000
    int blueIC = -1;    // 0 = red, 1 = blue
    int meter = -1;     // SP meter
    int hp = -1;        // HP
    int guard = -1;     // Guard gauge (float 0..360 at +0x134)
};

// One attempt scope. kind=="combat" validates committed actions through the
// format-1 adapter (moveIds/req); kind=="choice" is the controller-navigable
// quiz contract (§5.3) - stable option ids, accepted set, per-option feedback.
struct LessonTask {
    std::string id;
    std::string kind = "combat";          // "combat" | "choice"
    std::string label;
    std::string prompt;
    std::string checkpoint = "lessonStart";
    std::string script;                    // dummy episode id ("" = none)
    std::string continuity = "independent";// independent|continue|sameCombo|setupGap|newCombo|free
    std::vector<int> moveIds;              // combat: any-of committed action ids
    int inputMask = 0;                     // consumed button edge (0 = moveIds)
    std::string req = "move";              // combat adapter: input|commit|move|land|hits|connect|block|rg
    int hitsRequired = 1;
    std::vector<LessonAction> sequence;     // ordered stable actions for routes
    std::vector<ChoiceOption> options;     // choice tasks
    std::vector<std::string> acceptedOptionIds;
    std::map<std::string, std::string> optionFeedback;
    std::string successText;
    std::string failureText;
    std::string demo;                      // demo id ("" = none)
    bool           hasGoalState = false;    // goalState present (field selects capability)
    StatePredicate goalState;               // satisfied when this condition holds
    bool           hasContact = false;      // hook-backed typed contact predicate
    ContactPredicate contact;
    // Narrow, curated projectile-interception contract. This is NOT generic
    // entity contact: it observes the confirmed Sayuri #401 lifetime edge while
    // Shiori shield #423 is alive. Runtime preflight rejects every other pair.
    bool hasProjectileInterception = false;
    int incomingProjectilePattern = 0;
    int guardProjectilePattern = 0;
    int completionHoldTicks = -1;           // -1 = session default; 0 = immediate
    bool restoreOnSuccess = false;           // restore root checkpoint as soon as detected
    bool  hasPos = false;                    // reposition P1 when this task arms
    double posX = 0.0;                       // EFZ fighter coordinates are doubles
    double posY = 0.0;
    bool  hasDummyPos = false;               // independently reposition P2 for this task
    double dummyPosX = 0.0;
    double dummyPosY = 0.0;
    TaskStateSeed playerSeed;                // task_state_seeds: learner baseline
    TaskStateSeed dummySeed;                 // task_state_seeds: dummy baseline
    int afterWakeMaxTicks = 0;               // wakeup_state window (see LessonAction)
    std::string dummyState;                  // dummy_state qualifier (see LessonAction)
    std::vector<int> dummyStateMoveIds;
    int whiffWindowMaxTicks = 0;             // whiff_window (see LessonAction)
    bool duringGap = false;                  // episode_script gap window (see LessonAction)
    // action_absence: succeed by NOT acting. The window opens on `start`
    // ("launched" = P1 enters a launched state, "taskArmed" = immediately,
    // "dummyUntech" = P2 has a positive untech counter) and closes on `end`
    // ("grounded" = P1 actionable on the ground again, "ticks" = after
    // `ticks` non-frozen ticks, "dummyUntechEmpty" = P2's counter reaches
    // zero while still airborne, "dummyAttackEnd" = an observed P2 attack
    // instance ends, "episodeCycleEnd" = the task's scripted dummy episode
    // completes one cycle). Any NEW forbidden P1 action inside the window fails the attempt;
    // an attack animation inherited from a preceding continuation is ignored.
    // Exclusive contract: cannot mix with move/sequence/goal contracts.
    bool hasAbsence = false;
    std::string absenceForbid;               // "airtech" | "attack" ("" = ids only)
    std::vector<int> absenceForbidIds;
    std::string absenceStart = "launched";   // launched | taskArmed | dummyUntech
    std::string absenceEnd = "grounded";     // grounded | ticks | dummyUntechEmpty | dummyAttackEnd | episodeCycleEnd
    int absenceTicks = 0;                    // when end == "ticks"
    bool absenceFailOnHit = false;            // fail if P1 enters a struck/thrown state
    int absenceMinBlocks = 0;                 // require this many fresh P1 blockstun entries
    // combo_lifecycle: after the primary contract satisfies, hold the task
    // open until the COMBO END edge (alive := p2-in-stun || combo>0; end =
    // alive->!alive) so the task's sensed metrics cover the whole combo.
    bool endsCombo = false;
    // result_comparison: compare this attempt's sensed metric against the
    // stored metric of an EARLIER completed task. Without `op` it is a
    // feedback CARD only; with `op` the task succeeds only when
    // metric(this) <op> metric(vs) holds at completion.
    bool hasCompare = false;
    std::string compareVs;                   // earlier task id
    std::string compareMetric;               // comboHits|comboDamage|ticks|p2HpDelta|p1RfDelta|p2GuardDelta|maxUntech|untechTicks
    std::string compareOp;                   // "" (card only) | gt|lt|ge|le
    // branch_on_outcome: commit the starter, then classify the dummy's
    // reaction on the same evidence the engine already trusts - hitstun or
    // launch => HIT branch (run onHit like a sequence), blockstun => BLOCK
    // branch (hold: no NEW attack for onBlockHoldTicks). Either branch
    // completes the task; pressing on block or dropping the hit route fails.
    bool hasBranch = false;
    std::vector<int> branchStarterIds;
    std::vector<LessonAction> branchOnHit;
    int branchOnBlockHoldTicks = 0;
};

// Deterministic dummy episode (§5.6). start=="playerState" parks the episode
// until `predicate` observes the declared player condition.
struct DummyEpisode {
    std::string id;
    std::string ownerTask;
    // macro = the dummy attacks (input injection via `action`); idle = scoped
    // neutral P2 control with guard automation disabled; block = the dummy
    // auto-guards; rg = auto-guard that attempts Recoil Guard; guard = native
    // CPU Practice auto-block in a FIXED stance (clip stand|crouch - engine
    // guard rules apply, so the wrong stance genuinely gets opened up);
    // block_answer / rg_answer =
    // native guard lease + ONE injected `action` on the dummy's own
    // afterBlock/onRG edge (temporary puppet flip); cue = text only.
    std::string kind = "macro";       // macro | idle | block | rg | guard | block_answer | rg_answer | cue
    std::string clip;                 // macro text / held input spec / guard mode
    // Preferred over `clip` for the dummy runtime: an attack the dummy performs
    // by INPUT INJECTION (reusing the auto-action motion queue), no recording.
    // Value is a motion name: normals 5A/5B/5C/2A/2B/2C/6A/6B/6C/j.A/j.B/j.C or
    // specials 236A/623A/214A/... The dummy dashes in first (approach), then
    // performs it, so it is neither point-blank-instant nor out of range.
    std::string action;
    // Optional exact engine states that acknowledge the authored action. When
    // present, a different attack (for example a failed special becoming 5C)
    // is never allowed to complete the episode.
    std::vector<int> expectedMoveIds;
    std::string approach = "dash";    // dash | none (how the dummy closes first)
    // Reaction tell performed before the opener: "jump" makes the dummy hop
    // neutrally in place and land before its dash normal begins, so the
    // learner has a visible cue to react to. Only valid on taskArmed macro
    // episodes whose opener (action or script step 0) is a dash normal.
    std::string telegraph;            // "" | jump
    // taskArmed = loop immediately; playerState = fire when the learner's move
    // matches `predicate` (e.g. "p1move:309" -> Shiori shield) after reactDelay;
    // trigger = fire when the dummy hits an auto-action `trigger` edge.
    std::string start = "taskArmed";  // taskArmed | playerState | trigger
    std::string predicate;            // playerState: "p1move:<id>" to react to
    std::string trigger;              // trigger: afterBlock|onWakeup|onRG|afterHitstun|afterAirtech
    int         reactDelay = 30;      // playerState: session ticks after detection (~10 visual frames)
    std::string cueText;
    int  cueLead = 45;
    std::string repeatMode = "afterFailure";
    int  repeatDelay = 120;
    std::string pausePolicy = "missionFreeze";
    std::string completeOn = "actionEnd";
    // Seeded per-attempt behavior pool (honest hit-confirm variety). Each
    // episode lease picks ONE of these lease-only kinds (idle|block|rg) at
    // random; `kind` must itself be in the pool (the declared default). The
    // pick must not leak through cues - author variant-neutral cue text.
    std::vector<std::string> variants;
    // Episodes in the same non-empty group receive a shuffled, without-
    // replacement assignment from the identical `variants` pool. The
    // assignment is stable across retries for this lesson run, so a pair of
    // hidden hit-confirm checks can guarantee one hit and one block without
    // changing the answer after a failed attempt.
    std::string variantGroup;
    // episode_script: a scripted multi-action string for macro episodes.
    // Steps run in order: an ACTION step injects its move (cancel:true injects
    // during the prior move's cancel window instead of waiting for neutral);
    // a GAP step (gapTicks > 0, no action) is an authored pause - the session
    // exposes it so task contracts can bind "during the gap".
    struct ScriptStep {
        std::string action;   // injectable action ("" = gap step)
        // Non-frozen timing owned by this step. A cancel step waits this long
        // after the preceding action is acknowledged before it is injected;
        // an ordinary step waits this long after its own action resolves.
        int waitTicks = 0;
        int gapTicks = 0;     // gap step length (action must be empty)
        bool cancel = false;  // inject during the prior move (cancel window)
    };
    std::vector<ScriptStep> script;
};

struct Lesson {
    std::string completion = "allTasks";   // pages | allTasks | anyTask (REQUIRED in json)
    std::string requirementPlacement = "upperLeft"; // upperLeft | belowStats; stable for the lesson
    std::string wrongAction = "coach";     // coach | fail | ignore
    std::string failureReset = "taskCheckpoint";
    bool preserveCompletedTasks = true;
    std::vector<LessonPage>   pages;
    std::vector<LessonTask>   tasks;
    std::vector<DummyEpisode> episodes;
};

// Exact entity transition which gives an input-only authored command its
// action identity.  A tagged command has no player move ID: the consumed
// attack-button edge and this restored slot/generation transition are the
// proof that it executed.  `present == false` preserves ordinary Step
// semantics and keeps older mission files source-compatible.
struct EntityCommandOrigin {
    bool present = false;
    int slot = -1;
    int generation = 0;
    int rootPattern = -1;
    int activationPattern = -1;
};

struct Step {
    std::string      notation;       // display string, e.g. "236B" (icons from assets/controls)
    std::vector<int> moveIds;        // any-of: move-IDs that satisfy this step
    EntityCommandOrigin entityCommand; // optional exact input-only entity command
    // Attack-button edge causally consumed by this recorded action. New
    // recordings persist it so delayed deadline grace cannot be armed by an
    // unrelated mash; 0 is the backward-compatible legacy/unknown value.
    int              expectedAttackMask = 0;
    StepReq          req = StepReq::Land;
    int              hitsRequired = 1; // for StepReq::Hits
    // Recorder-created direct multi-hit steps are route requirements, not an
    // instruction to reproduce every possible contact of the move.  When this
    // is true, one or more hits are enough once the player proves the authored
    // route continued (the move ended, the next expected action began, or an
    // authored combo boundary occurred).  Deliberately authored exact hit
    // counts leave this false.  Old direct-contact recordings predate the key;
    // their missing value is migrated to true by the tolerant JSON loader.
    bool             allowPartialHits = false;
    // New format-1 recordings can bind Land/Hits/Connect to the exact direct
    // player collision resolver. Legacy files leave this false and retain the
    // aggregate combo/latch adapter. Concurrent entity obligations use the
    // mission-level entityContacts schedule and never this flag.
    bool             directContact = false;
    // Optional typed resolver result for new recordings. Empty preserves
    // legacy "any committed contact" Connect behavior.
    std::string      contactResult;
    bool             optional = false; // step may be skipped
    int              maxDelay = 0;     // 0 = unlimited; NON-FROZEN frames the armed step may
                                       // wait for its hit (delayed projectile window)
    int              maxGap = 0;       // 0 = mission failTimer; NON-FROZEN frames allowed
                                       // between the previous step and performing this one
                                       // (recorded delays get a generous allowance)
    bool             comboEndAfter = false; // require a real combo alive->dead edge after
                                            // this step before the next step may begin. This
                                            // is an authored setup/okizeme boundary; false
                                            // keeps legacy one-piece-combo behavior.
    int              charState = -1;        // P1 character-state stamp at the step's move
                                            // start; -1 = not recorded/required. Per-char
                                            // meaning: Akiko = bullet cycle 0..2 (rekka
                                            // no/partial/full crit determinism). Validated
                                            // by the runner when >= 0 and the live state is
                                            // readable, so a diverged crit sequence drops
                                            // with an explicit reason instead of a timeout.
    int              damage = 0;            // recorded damage delta this step produced
                                            // (0 = no requirement). Runner drops when the
                                            // observed delta diverges beyond the policy
                                            // tolerance - catches hit-variant mechanics
                                            // (clean hits, crits) that share a move ID.
};

// Exact entity->player contacts run beside the linear player-action recipe.
// They are intentionally concurrent: a projectile may hit repeatedly while
// later normals are already being performed, so squeezing it into Step::Hits
// would stall or misattribute the route. This is an ordered contact schedule,
// not a sampled combo-counter guess: `opensAfterAction` is the exact matched
// Spawn/Morph producer gate, `contactAfterAction` retains the contact-time
// action ordering, and `afterStep` is the preceding exact direct-contact
// barrier, and `dueBeforeStep` is the following authored action barrier (-1 =
// mission end). Optional contact ordinals preserve resolver order when an
// entity lands between hits of one direct multi-hit action. Ordinals are
// one-based; -1 keeps the legacy whole-step barrier. For dueBeforeStepContact,
// zero is the stricter generated-recording contract "before action start".
// v1/v2 schedules treat slot/generation as authoring diagnostics. Newly
// recorded v3 schedules use the exact embedded start state plus a fresh 64-slot
// ring lineage for every attempt, so slot/generation and the contact-linked
// Baseline/Spawn/Morph descriptor become strict runtime identity.
// One exact child admitted by a flexible fanout episode.  The member keeps the
// same restored-ring identity proof as an ordinary v3 contact requirement;
// only sibling ordering and the number of children which ultimately connect
// are flexible. `*Observed` values describe only the authored take; they are
// neither a replay ceiling nor the episode's completion threshold.
struct EntityContactFanoutMember {
    int slot = -1;
    int generation = 0;
    std::vector<int> patterns;
    std::string producerLifecycle;
    int producerPattern = -1;
    int producerPriorPattern = -1;
    int contactsObserved = 1;
    int comboHitsObserved = 0;
    int damageObserved = 0;
};

struct EntityContactRequirement {
    std::string notation;                // display fallback, e.g. "#443 HIT x4"
    int owner = 1;
    int target = 2;
    int slot = -1;
    int generation = 0;
    std::vector<int> patterns;
    std::string result = "hit";          // Contact::ResultMatches vocabulary
    int contactsRequired = 1;
    int comboHitsRequired = 0;
    // Flexible fanout episodes preserve every eligible child identity below,
    // while these minima define completion.  The existing required counts are
    // retained only as aggregate evidence from the authored take; they are not
    // a replay ceiling because a different valid subset may connect. Empty
    // members plus zero minima preserve exact pre-v5 behavior.
    int minimumContactsRequired = 0;
    int minimumComboHitsRequired = 0;
    std::vector<EntityContactFanoutMember> fanoutMembers;
    // v3 producer identity. `producerLifecycle` is baseline|spawn|morph;
    // producerPattern is the ring pattern linked to the resolver contact and
    // producerPriorPattern is required only for morph (-1 otherwise).
    // Empty/default values preserve every v1/v2 file byte-for-byte in meaning.
    std::string producerLifecycle;
    int producerPattern = -1;
    int producerPriorPattern = -1;
    // Exact lifecycle producer/activation gate (Spawn/Morph lineage).
    int opensAfterAction = -1;
    // Presentation provenance is separate from the strict lifecycle gate.
    // -2/-2 means an older file omitted the fields; -1/-1 means a new
    // recording explicitly could not prove one causal setter and must remain
    // a standalone entity hit; non-negative action plus positive move is the
    // exact source action in this authored take.  These fields never weaken
    // slot/generation or producer-lifecycle grading.
    // Fresh in-memory requirements are new data and fail closed until a
    // recorder/author proves a source. ParseEntityContact alone assigns the
    // legacy-absent state when old JSON truly omits both fields.
    int semanticSourceAction =
        ::Mission::SemanticSourcePolicy::kExplicitUnresolved;
    int semanticSourceMove =
        ::Mission::SemanticSourcePolicy::kExplicitUnresolved;
    // Latest authored action observed before this contact run. Kept separate
    // from the producer gate because delayed projectiles often land several
    // normals after their setter was created.
    int contactAfterAction = -1;
    int afterStep = -1;
    int afterStepContact = -1;
    int dueBeforeStep = -1;
    int dueBeforeStepContact = -1;
    int segment = 0;
    int maxDelay = 0;                    // non-frozen ticks after both gates are open
    int damage = 0;                      // exact resolver-local HP delta (diagnostic)
    // The combo segment ends after this entity contact rather than after a
    // linear player Step.  Omitted/false preserves every existing mission.
    bool comboEndAfter = false;
};

// Exact entity activation recorded without a resolver contact.  This keeps a
// whiffed summon/projectile phase gradeable as lifecycle evidence instead of
// weakening it to "the player performed the motion".  Runtime validation is
// responsible for binding the descriptor to an exact restored ring; these
// defaults keep missions written before the field byte-for-byte compatible.
struct EntityLifecycleRequirement {
    std::string notation;             // display fallback, e.g. "236C (SETUP)"
    int owner = 1;
    int slot = -1;
    int generation = 0;
    std::string lifecycle;            // baseline|spawn|morph|despawn
    int pattern = -1;
    int priorPattern = -1;            // required by a strict morph descriptor
    int opensAfterAction = -1;        // exact authored action-order gate
    int segment = 0;
    int maxDelay = 0;                 // non-frozen ticks after the gate opens
};

struct PlayerSetup {
    std::string character;           // internal short name; empty = keep current
    // P0.2: presence-aware position - hasPos distinguishes "author did not set
    // a position" from an explicit (0,0). Legacy files (pos present) set it.
    bool   hasPos = false;
    double posX = 0.0;               // start X in world coords
    double posY = 0.0;
    int    palette = 0;
    int    rf = -1;                  // starting RF (-1 = leave as-is)
    int    meter = -1;               // starting meter (-1 = leave as-is)
    int    hp = -1;                  // starting HP (-1 = leave as-is)
    int    blueIC = -1;              // -1 keep, 0 red IC, 1 blue IC
    int    guard = -1;               // guard gauge 0..360 (-1 = leave as-is)
    // rf_lock: hold the authored rf (and blueIC color when authored) for the
    // whole session via the RF-freeze machinery, so resource-gated dummy
    // actions (RF specials, reversals) never silently downgrade mid-lesson.
    bool   rfLock = false;
    // Character-specific resources captured at record time and restored on load
    // (e.g. mioStance, mishioElement, neyukiJam, ikumiBlood, akikoBulletCycle).
    std::map<std::string, int> resources;
};

struct DummySetup {
    std::string character;           // internal short name; empty = keep current
    bool   hasPos = false;           // P0.2 presence-aware position
    double posX = 0.0, posY = 0.0;
    int    palette = 0;
    int    rf = -1;
    int    meter = -1;
    int    hp = -1;
    int    blueIC = -1;
    int    guard = -1;               // guard gauge 0..360 (-1 = leave as-is)
    bool   rfLock = false;           // rf_lock: session-long RF freeze at `rf`
    bool   crouch = false;
    bool   jump = false;
    std::string airtech = "none";    // none / forward / back
    std::map<std::string, int> resources;
};

// A scoring/rank tier. A tier is "met" when every present (non-zero) constraint
// is satisfied by the run. Tier 0 is the base clear; later tiers are ranks.
struct ScoreTier {
    int         minHits = 0;         // 0 = ignore
    int         minDamage = 0;       // 0 = ignore
    int         maxAttempts = 0;     // 0 = ignore (attempts allowed to earn this rank)
    std::string label;               // optional display, e.g. "Clear" / "Gold"
};

struct Mission {
    int         format = 1;
    // Stable content id used by authored trial/mission packs. Tutorial files
    // historically exposed the same JSON field through `lessonId`; both are
    // populated on load so the older tutorial runtime remains compatible.
    std::string id;
    std::string type = "combo";
    std::string category;            // browser topic (tutorial: start/movement/offense/systems)
    int         difficulty = 0;      // 0 unspecified, otherwise 1..5 browser rating
    int         order = 0;           // authored course order (tutorial browser); 0 = file order
    std::string name;
    std::string description;
    PlayerSetup player;
    DummySetup  dummy;
    int         stage = -1;          // -1 = default / keep
    int         bgm = -1;            // BGM track for hotswap load (-1 = default)
    std::vector<Step>        steps;
    bool        strictEntityContacts = false;
    std::vector<EntityContactRequirement> entityContacts;
    std::vector<EntityLifecycleRequirement> entityLifecycles;
    std::string demo;                // EFZMACRO text (MacroController format)
    // Embedded Revival savestate dump (base64, Mission::StateDump format).
    // Captured at record start; restored after the mission's hotswap settles
    // so the match starts bit-perfect even in a fresh session. Empty = use
    // the value-level setup above (player/dummy fields) only.
    std::string savestate;
    int         failTimer = 60;      // frames of no-progress before the combo is a drop
    std::vector<ScoreTier>   scores; // [0] = base clear; [1..] = ranks
    std::vector<std::string> hints;
    // Recorder guardrail: preserve the take and raw sidecar, but refuse normal
    // playback until these unresolved authoring obligations are reviewed.
    std::vector<std::string> reviewRequired;
    // Non-blocking facts retained from recording (for example, physical
    // attack presses which deterministically produced no move).  Diagnostics
    // survive save/load for authoring and debugging, but unlike
    // reviewRequired they do not make an otherwise valid mission unplayable.
    std::vector<std::string> recordingDiagnostics;

    // ---- tutorialSchema 1 fields (0 = plain mission; see §5.2 of the design) ----
    int         tutorialSchema = 0;
    std::string lessonId;            // stable id ("id"); globally stable in the pack
    int         revision = 1;
    std::string summary;             // browser "YOU'LL PRACTICE" line
    std::vector<std::string> sourceRefs;
    std::vector<std::string> requires;   // exhaustive §5.7 capability list
    bool        requiresExactBaseline = false;
    bool        hasLesson = false;   // lesson block present (pages/tasks runtime)
    Lesson      lesson;
    std::string nextLessonId;        // resolved Next Lesson edge (stable id)
    std::vector<std::string> recommendedAfter;

    std::string sourcePath;          // runtime-only: file this was loaded from
};

// Validates tutorial-schema invariants (§5.2): declared completion mode,
// unique page/task/episode/option ids, choice tasks carry 2..3 options with a
// non-empty accepted set, combat tasks name at least one committed action or
// are page-only. Returns false + errorOut naming the exact field.
bool ValidateLesson(const Mission& m, std::string& errorOut);

// One scenario entry inside a pack (mission file + list metadata + progression).
struct Scenario {
    std::string name;
    std::string file;                // mission json filename, relative to the pack folder
    std::string description;
    std::string category;            // stable PackCategory id (mirrored by Mission::category)
    std::string preview;             // preview image filename (optional)
    bool        locked = false;      // progression gate (Trials only; invalid for tutorials)
    // Course manifest fields (§5.1) - authoritative for tutorial packs:
    std::string id;                  // stable lesson id (must match the file's "id")
    int         order = 0;           // explicit global order (duplicates = lint error)
    int         difficulty = 0;
    std::string next;                // stable id of the explicit Next Lesson edge
    std::vector<std::string> recommendedAfter;
};

// Category taxonomy row (§3.2): belongs in pack metadata, never a C++ array.
struct PackCategory {
    std::string id;
    std::string label;
    std::string description;
    int         order = 0;
};

struct Pack {
    int         format = 1;
    std::string id;                  // stable pack id (e.g. "efz.core_tutorial")
    std::string name;
    std::string author;
    std::string version;             // author-facing release version (e.g. "1.0")
    std::string character;           // primary character short name
    std::string description;
    bool        editable = false;    // created by the in-game local authoring flow
    int         curriculumRevision = 0;
    std::vector<PackCategory> categories;
    std::vector<Scenario> scenarios;

    std::string folderPath;          // runtime-only: the pack folder
};

// ---- Load / save (JSON via nlohmann, tolerant: missing fields use defaults) ----
// Return true on success; on failure fill `errorOut` and leave the out param
// unchanged. All are exception-safe (JSON parse errors become `errorOut`).
bool LoadPack   (const std::string& packJsonPath,    Pack&    out, std::string& errorOut);
bool LoadMission(const std::string& missionJsonPath, Mission& out, std::string& errorOut);
bool SaveMission(const std::string& missionJsonPath, const Mission& mission, std::string& errorOut);
bool SavePack   (const std::string& packJsonPath,    const Pack& pack,       std::string& errorOut);

// Coalesce only catalog-curated interchangeable projectile children into one
// unordered fanout episode. This is also the in-memory compatibility adapter
// for exact v3/v4 generated recordings; it never changes the file on disk.
// Returns true when a schedule was upgraded.
bool NormalizeFlexibleEntityFanoutEpisodes(Mission& mission,
                                            bool allowInference = false);

// Discover pack folders under a root dir: returns each "<root>/<sub>/pack.json"
// that exists. Used to populate the mission-select menu.
std::vector<std::string> DiscoverPackJsonPaths(const std::string& rootDir);

// Resolve the missions root: "<dll-dir>\assets\missions". Empty on failure.
std::string ResolveMissionsRoot();

} // namespace Mission
