#pragma once
#define DIRECTINPUT_VERSION 0x0800
// Only include these for C++ compiler, not for RC compiler
#ifndef RC_INVOKED

#define HMENU_ID(id) reinterpret_cast<HMENU>(static_cast<UINT_PTR>(id))
#else
#endif

// Constants that can be shared between C++ and RC
#define IDD_EDITDATA 1001
#define IDC_TAB_CONTROL 4000

#define MAX_HP 9999
#define MAX_METER 3000

// HP and Resource limits
#define MAX_RF 1000

// Memory offsets
#define EFZ_BASE_OFFSET_P1 0x390104
#define EFZ_BASE_OFFSET_P2 0x390108
#define EFZ_BASE_OFFSET_GAME_STATE 0x39010C // NEW: For game state
// Screen/state byte:
// 0=Title, 1=Character Select, 2=Loading, 3=In-game, 5=Win screen, 6=Settings, 8=Replay select
#define EFZ_BASE_OFFSET_SCREEN_STATE 0x390148
#define XPOS_OFFSET 0x20
#define YPOS_OFFSET 0x28
#define HP_OFFSET 0x108
#define HP_BAR_OFFSET 0x10C
#define METER_OFFSET 0x148
#define MOVE_ID_OFFSET 0x8
#define RF_OFFSET 0x118
#define GAME_MODE_OFFSET 0x1364 // CORRECTED: The offset is hexadecimal, not decimal.
#define XVEL_OFFSET 0x30  // X velocity offset from player base
#define YVEL_OFFSET 0x38  // Y velocity offset from player base

// IC (Instant Charge) color offset - 0=red IC, 1=blue IC
#define IC_COLOR_OFFSET 0x120  // IC color offset from player base

// -------------------------------------------------------------
// Engine regeneration / recovery param copies 
// In the hotkey handler (F4/F5 logic) the battleContext fields
// at offsets 1396/1398 (words) are copied into each player struct
// at decimal offsets 12524 / 12526. (12524 = 0x30EC, 12526 = 0x30EE)
// We only have direct access to the player bases, so we expose the
// copy offsets here for read-only debug and gating heuristics.
//  Param A (player + 0x30EC): cycles 1000 <-> 2000 via F5 or fine-tuned
//                             0..2000 stepping +5 while F4 held.
//  Param B (player + 0x30EE): becomes 3332 under one branch of F5 cycle,
//                             forced to 9999 while F4 fine-tune active.
// Heuristics used by UI:
//   - Fine-tune active (F4 held)  : Param B == 9999 AND Param A not in {1000,2000}
//   - Cycle / preset (F5 engaged) : Param A == 1000 or 2000 OR Param B == 3332
//   - Normal                      : otherwise.
// refines semantics, update the detection in imgui_gui.cpp.
// PLAYER_PARAM_A=12524 and PLAYER_PARAM_B=12526 (decimal)
// 12524 dec = 0x30EC, 12526 dec = 0x30EE
#define PLAYER_PARAM_A_COPY_OFFSET 0x30EC
#define PLAYER_PARAM_B_COPY_OFFSET 0x30EE

// System flags used by hotkey gating 
// sys + 4944 / 4948 were referenced as gate conditions blocking F4 fine-tune.
#define SYS_FLAG_4944 4944
#define SYS_FLAG_4948 4948

// Character name offset
#define CHARACTER_NAME_OFFSET 0x94  // Character name offset from player base

// Character facing direction offset
#define FACING_DIRECTION_OFFSET 0x50  // 1 = facing right, 255 = facing left

// Move IDs
#define IDLE_MOVE_ID 0
#define WALK_FWD_ID 1
#define WALK_BACK_ID 2
#define CROUCH_ID 3
#define CROUCH_TO_STAND_ID 7
#define PREJUMP_ID 8
#define LANDING_ID 13
#define STAND_GUARD_ID 151
#define CROUCH_GUARD_ID 153
#define CROUCH_GUARD_STUN1 154
#define CROUCH_GUARD_STUN2 155
#define AIR_GUARD_ID 156
#define RG_STAND_ID 168
#define RG_CROUCH_ID 169
#define RG_AIR_ID 170

// Hitstun Move IDs
#define STAND_HITSTUN_START 50
#define STAND_HITSTUN_END 55
#define CROUCH_HITSTUN_START 56
#define CROUCH_HITSTUN_END 58  // Changed from 59 to avoid overlap
#define LAUNCHED_HITSTUN_START 59  // New constant for range start
#define LAUNCHED_HITSTUN_END 71    // New constant for range end
#define SWEEP_HITSTUN 71  // Special case of launch

// Tech Move IDs
#define FORWARD_AIRTECH 157
#define BACKWARD_AIRTECH 158
#define GROUNDTECH_START 98
#define GROUNDTECH_END 99
// Some engine logic references state 97 as a pre-tech/startup marker; include as alias for safety
#define GROUNDTECH_PRE 97
#define GROUNDTECH_RECOVERY 96 

// Jump Move IDs
#define STRAIGHT_JUMP_ID 4
#define FORWARD_JUMP_ID 5
#define BACKWARD_JUMP_ID 6
#define FALLING_ID 9
#define DOUBLE_JUMP_NEUTRAL_ID 14
#define DOUBLE_JUMP_FWD_ID 15
#define DOUBLE_JUMP_BACK_ID 16
// Multiple landing variants 10/11/12 are used; keep 13 as legacy/alt
#define LANDING_1_ID 10
#define LANDING_2_ID 11
#define LANDING_3_ID 12
#define LANDING_ID 13

// Special Stun States
#define FIRE_STATE 81
#define ELECTRIC_STATE 82
#define FROZEN_STATE_START 83
#define FROZEN_STATE_END 86

// Thrown states (defender pre-hit/airborne during throw). Ranges can vary slightly by character.
// Observed examples:
//  - Ayu:   110 -> 100 -> 59 (launch)
//  - Akiko: 110 -> 121 -> 122 -> 59 (launch) -> knockdown
//  - Mai:   108 -> 103 -> 117 -> knockdown (117 is a continuation outside 100..110)
//  - Rumi:  107 -> 101 -> 118 -> 105 -> knockdown (118 continuation outside 100..110)
//  - Unknown: (throw start) 59 -> 113 -> 64 -> 70 (wallbounce) -> knockdown (113 continuation outside 100..110)
// We cover a conservative primary window 100..110 and include known outliers 121..122 and 117, 118, 113.
#define THROWN_STATE_START 100
#define THROWN_STATE_END   110
#define THROWN2_STATE_START 121
#define THROWN2_STATE_END   122
// Single-ID continuation observed for Mai
#define THROWN3_STATE_START 117
#define THROWN3_STATE_END   117
// Single-ID continuation observed for Rumi
#define THROWN4_STATE_START 118
#define THROWN4_STATE_END   118
// Single-ID continuation observed for UNKNOWN and Mizuka
#define THROWN5_STATE_START 113
#define THROWN5_STATE_END   113

// Game frame rate settings
#define EFZ_VISUAL_FPS 64.0
#define SUBFRAMES_PER_VISUAL_FRAME 3.0
#define INTERNAL_FRAMES_PER_SECOND (EFZ_VISUAL_FPS * SUBFRAMES_PER_VISUAL_FRAME)  // 192 internal frames per second

// RG Constants
#define RG_STAND_FREEZE_DEFENDER 20
#define RG_STAND_FREEZE_ATTACKER 19.66
#define RG_CROUCH_FREEZE_DEFENDER 22
#define RG_CROUCH_FREEZE_ATTACKER 19.66
#define RG_AIR_FREEZE_DEFENDER 22
#define RG_AIR_FREEZE_ATTACKER 20
#define RG_STUN_DURATION 20     // 21.66 for Sayuri, but 20 for everyone else

// RG Constants - Add these freeze durations in internal frames
#define RG_STAND_FREEZE_DURATION (RG_STAND_FREEZE_DEFENDER * 3)
#define RG_CROUCH_FREEZE_DURATION (RG_CROUCH_FREEZE_DEFENDER * 3) 
#define RG_AIR_FREEZE_DURATION (RG_AIR_FREEZE_DEFENDER * 3)

// Superflash-related constants
#define GROUND_IC_ID 167            // Ground Infinite Cancel moveID
#define AIR_IC_ID 171               // Air Infinite Cancel moveID
#define SUPER_FLASH_DURATION 119    // 39.66 visual frames * 3
#define IC_FLASH_DURATION 89        // 29.66 visual frames * 3
#define SUPERFLASH_BLACK_BG_OFFSET 1  // First subframe of black bg isn't part of freeze

// Untech memory offset (also: "recovery cooldown" in canPerformAirRecovery -
// gates when the defender can air-tech).
#define UNTECH_OFFSET 0x124

// Multi-purpose state-timer (short, +0x14A/330). On the *defender* this is the
// remaining blockstun/hitstun freeze; on the *attacker* it's the hit-hitstop
// frames remaining (set from attack_data +194 / +196 the moment the attack
// resolves - see processProjectileCollision in efz.c). Decremented every
// non-frozen frame for whichever player it belongs to. Despite the historical
// "blockstun" name, this is the field that drives **shared hit-hitstop**:
// when both players have +0x14A > 0 the engine doesn't advance gameplay timers
// (mirrors MBAACC's `nSharedHitstop` heuristic).
#define BLOCKSTUN_OFFSET 0x14A
#define HITSTOP_FREEZE_OFFSET BLOCKSTUN_OFFSET   // alias for attacker-side reads

// "I am causing the screen to freeze" counter (short, +0x14C/332).
// Set by character scripts that enter Initial Charge / Super flash to fixed
// durations (60/90/120 internal frames). While > 0 on either player the
// engine pauses the flash visual counter (+0x30C4) and the screen is frozen
// because of that player's super.
// Decompilation: efz.c:14432, 14538, 14577 (case 0xAB = AIR_IC_ID), 16422 etc.
//                set via `*(_WORD*)(this+332) = 60/90/120`.
//                Read at efz.c:195038 to gate flash counter ticks.
// NOT to be confused with hit-hitstop, which lives in BLOCKSTUN_OFFSET +0x14A.
#define SUPERFLASH_FREEZE_OFFSET 0x14C
// Backwards-compat alias retained while we migrate consumers.
#define HITSTOP_OFFSET SUPERFLASH_FREEZE_OFFSET

// Air time counter (short). Frames since the character last touched the ground.
// canPerformRecoilGuard uses `>= 30` as a threshold for "in air too long".
#define AIRTIME_OFFSET 0x14E

// Combo timer (short). Counts down from 180 frames after each hit; when it
// reaches 0 the combo counter resets. (decompilation: applyAttackDamage)
#define PLAYER_COMBO_TIMER_OFFSET 0x104

// Total combo damage so far (DWORD).
#define PLAYER_COMBO_DAMAGE_OFFSET 0x100

// Combo length the *attacker* has on the opponent (short).
#define PLAYER_COMBO_COUNTER_OFFSET 0x174

// Combo damage scaling multiplier (double, displayed as N/100).
#define PLAYER_COMBO_DAMAGE_SCALING_OFFSET 0x178

// Superflash / IC freeze counter (DWORD). Counts down once per non-frozen frame.
#define PLAYER_SUPERFLASH_COUNTER_OFFSET 0x30C4

// ===== Hit-resolution / RG state (decompiled from sub_767F60) =====
//
// updateCharacterTimers (efz.c:9925) and the post-hit handler at
// processProjectileCollision/handlePlayerToPlayerCollision expose several
// short-counter fields that decrement per non-frozen visual frame. Names are
// based on observed semantics, not the decompilation comments (which are
// AI-generated and unreliable).

// "Cooldown" / lockout counter family (each decrements while opponent's
// hitstop is 0). Their write sites confirm specific semantics:
//   +0x138 (312)  guard-cancel / "no-RG" lockout - gates RG eligibility
//                 (wiki: 10F cooldown after a missed RG attempt)
//   +0x130 (304)  byte-sized frame lockout refreshed by frame hit flags and
//                 checked by collision code for some airborne interactions
//   +0x13A (314)  generic state lockout - set to 1/2 by character scripts
//   +0x13C (316)  "in special state" - checked by hit handler to forbid RG
//   +0x13E (318)  per-state cooldown
//   +0x140 (320)  per-state cooldown
#define PLAYER_FRAME_LOCKOUT_OFFSET    0x130
#define PLAYER_RG_COOLDOWN_OFFSET     0x138
#define PLAYER_STATE_LOCKOUT_OFFSET   0x13A
#define PLAYER_SPECIAL_STATE_OFFSET   0x13C
#define PLAYER_STATE_COOLDOWN3_OFFSET 0x13E
#define PLAYER_STATE_COOLDOWN4_OFFSET 0x140

// Producer-dependent collision/result latch (DWORD). Diagnostic only: this is
// not a stable contact-result enum and a raw value must not satisfy a typed
// mission predicate by itself. Observed writers include:
//   0 = no latched value
//   2 = block, RG, or Guard Point paths
//   3 = ordinary hit paths; some FIC scripts also force 3 on whiff
//   5 = entity-interaction paths
//   6 = throw paths and character-specific counter paths
//   7 = a hit-path variant
// Direct player collision usually writes the attacker's field, but scripts and
// entity resolvers write it too. Use resolver-local evidence for exact results.
#define PLAYER_HIT_STATE_OFFSET   0x168

// Attacker move countdown - decremented by 1 every time an attack resolves
// (RG / block / hit / throw). Used by the engine to time "attack ended" state.
#define PLAYER_ATTACK_TIMER_OFFSET 0x16C

// Guard / countered flag (DWORD).
//   0 = not in a guard state
//   1 = in block/guard state OR (on the *attacker*) "marked as countered" by RG
#define PLAYER_GUARD_FLAG_OFFSET   0x170

// Guard Gauge (float, max 360). Depletes per subframe at:
//   neutral             0.075
//   grounded hitstun    0.2
//   air hitstun         1.0
// Refilled by chip damage (BlockedMoveBaseDamage / 30) per blocked move.
#define PLAYER_GUARD_GAUGE_OFFSET  0x134

// Counter-hit flag (DWORD) on the *attacker*. Set when their attack landed on
// a defender flagged as counter-hit eligible (defender frame_data+176 bit
// 0x2000). Cleared at the start of every collision pass.
#define PLAYER_COUNTER_HIT_FLAG_OFFSET 0x144

// Knockback velocities applied by the most recent hit (set on the defender).
#define PLAYER_HIT_XVEL_OFFSET 0xC0   // double
#define PLAYER_HIT_YVEL_OFFSET 0xC8   // double
#define PLAYER_PHYSICS_FLAG_OFFSET 0xD0  // DWORD set to 1 after a hit lands

// Per-hit knockdown flags written by the hit handler.
#define PLAYER_WALLBOUNCE_FLAG_OFFSET 0x128 // DWORD (attack flag bit 0x400)
#define PLAYER_GROUND_BOUNCE_FLAG_OFFSET 0x12C // DWORD (attack flag bit 0x800)

// Pre-hit HP snapshot - combo display reads this minus current HP for damage.
// Already exists as HP_BAR_OFFSET (0x10C); see existing constant above.

// ===== Attack-data fields (within frame_data 200-byte block) =====
// Verified from `applyAttackDamage` and the hit handler. All offsets here are
// relative to the start of the same 200-byte frame_data block whose first 80
// bytes hold the 5 hurtboxes and the next 64 bytes hold the 4 attack boxes.
#define ATTACK_DATA_BASE_DAMAGE_OFFSET    0xA0  // short
#define ATTACK_DATA_CHIP_DAMAGE_OFFSET    0xA8  // short
#define ATTACK_DATA_FLAGS_OFFSET          0xAA  // word (already FRAME_ATTACK_PROPS_OFFSET)
#define ATTACK_DATA_AIR_HIT_OVERRIDE      0xAC  // short - defender moveID for air-hit reaction
#define ATTACK_DATA_GROUND_HIT_OVERRIDE   0xAE  // short
#define ATTACK_DATA_HIT_FLAGS_OFFSET      0xB0  // word (already FRAME_HIT_PROPS_OFFSET)
#define ATTACK_DATA_GUARD_FLAGS_OFFSET    0xB2  // word - stun duration scalar
#define ATTACK_DATA_METER_GAIN_BLOCK      0xB6  // short
#define ATTACK_DATA_KNOCKBACK_X           0xB8  // float
#define ATTACK_DATA_KNOCKBACK_Y           0xBC  // float
#define ATTACK_DATA_ATTACKER_HITSTOP      0xC2  // short
#define ATTACK_DATA_DEFENDER_HITSTOP      0xC4  // short

// Newly-mapped attack flag bits (frame_data + 0xAA, set on attacker frame):
#define FRAME_ATTACK_FLAG_THROW_ATTACK    0x0100  // throw vs strike
#define FRAME_ATTACK_FLAG_WALLBOUNCE      0x0400
#define FRAME_ATTACK_FLAG_GROUND_BOUNCE   0x0800
#define FRAME_ATTACK_FLAG_COUNTER_MOVE    0x2000  // CH-causing move

// Newly-mapped hit-property flag bits (frame_data + 0xB0):
#define FRAME_HIT_FLAG_COUNTER_SETUP      0x0200  // counter-eligible state
#define FRAME_HIT_FLAG_AIRTHROW_VULN      0x0800
#define FRAME_HIT_FLAG_GROUND_THROW_VULN  0x1000
#define FRAME_HIT_FLAG_COUNTER_VULN       0x2000  // defender in CH-vulnerable state

// Frame-data block flags (within the 200-byte frame_data block at +80 attack-boxes).
//   ATTACK_PROPS_OFFSET (170/0xAA) low byte:
//     0x01 = stand-blockable, 0x02 = crouch-blockable, 0x04 = "special anim",
//     0x40 = airborne-attack flag, 0x80 = block-disable
//   HIT_PROPS_OFFSET (176/0xB0):
//     0x10 = blockable hit, 0x20 = defender immune
#define FRAME_ATTACK_FLAG_STAND_BLOCKABLE   0x0001
#define FRAME_ATTACK_FLAG_CROUCH_BLOCKABLE  0x0002
#define FRAME_ATTACK_FLAG_SPECIAL_ANIM      0x0004
#define FRAME_ATTACK_FLAG_AIRBORNE          0x0040
#define FRAME_ATTACK_FLAG_BLOCK_DISABLE     0x0080
#define FRAME_HIT_FLAG_BLOCKABLE            0x0010
#define FRAME_HIT_FLAG_DEFENDER_IMMUNE      0x0020

// Tech recovery frames
#define AIRTECH_VULNERABLE_FRAMES 16
#define RUMI_AIRTECH_DOUBLEJUMP_LOCKOUT 31

// Helper macros
#define CLAMP(val, min, max) ((val)<(min)?(min):((val)>(max)?(max):(val)))

// Auto-Airtech patch configuration
#define AIRTECH_ENABLE_ADDR 0xF4FF
#define AIRTECH_ENABLE_BYTES "\x74\x71"
#define AIRTECH_FORWARD_ADDR 0xF514
#define AIRTECH_FORWARD_BYTES "\x75\x24"
#define AIRTECH_BACKWARD_ADDR 0xF54F
#define AIRTECH_BACKWARD_BYTES "\x75\x21"

// GUI Dialog IDs
#define IDD_EDITDATA 1001

// Player 1 controls
#define IDC_HP1     2001
#define IDC_METER1  2002
#define IDC_RF1     2003
#define IDC_X1      2004
#define IDC_Y1      2005

// Player 2 controls
#define IDC_HP2     2011
#define IDC_METER2  2012
#define IDC_RF2     2013
#define IDC_X2      2014
#define IDC_Y2      2015

// Button controls
#define IDC_BTN_SWAP_POS 2020
#define IDC_BTN_ROUND_START 2021
#define IDC_BTN_CONFIRM 2022
#define IDC_BTN_CANCEL 2023

#define IDC_AUTO_AIRTECH_CHECK 3001
#define IDC_AIRTECH_FORWARD 3002
#define IDC_AIRTECH_BACKWARD 3003

// Combo box control IDs (replacing radio buttons)
#define IDC_AIRTECH_DIRECTION 3010  // Combo box for airtech direction
#define IDC_JUMP_DIRECTION    4020  // Combo box for jump direction
#define IDC_JUMP_TARGET       4021  // Combo box for jump target
#define IDC_AIRTECH_DELAY     3020  // For the airtech delay field

// Add these after your existing Move ID section
// Attack level blockstun MoveIDs
#define STANDING_BLOCK_LVL1 150
#define STANDING_BLOCK_LVL2 151  // Same as STAND_GUARD_ID
#define STANDING_BLOCK_LVL3 152

#define CROUCHING_BLOCK_LVL1 153  // Same as CROUCH_GUARD_ID
#define CROUCHING_BLOCK_LVL2_A 154  // Same as CROUCH_GUARD_STUN1
#define CROUCHING_BLOCK_LVL2_B 155  // Same as CROUCH_GUARD_STUN2

// Dash states
// Verified universal movement IDs.  The older START/RECOVERY aliases below
// predate the move-ID audit and are retained for source compatibility only;
// 164/166 are directional dash states, not recovery phases.
#define GROUND_FORWARD_DASH_ID 163
#define GROUND_BACKWARD_DASH_ID 164
#define AIR_FORWARD_DASH_ID 165
#define AIR_BACKWARD_DASH_ID 166
#define FORWARD_DASH_START_ID 163
#define FORWARD_DASH_RECOVERY_ID 164
#define FORWARD_DASH_RECOVERY_SENTINEL_ID 178 // New: variant recovery state used for clean control handoff
#define BACKWARD_DASH_START_ID 165
#define BACKWARD_DASH_RECOVERY_ID 166
// Character-specific exceptions
// Kaori's forward dash start state uses MoveID 250 instead of the universal 163.
#define KAORI_FORWARD_DASH_START_ID 250
// Kaori's 44~66 Recoil Ducking destination.  EFZ enters this only when a
// native forward dash command is consumed during backdash 164, frame 4/5.
#define KAORI_RECOIL_DUCK_ID 251

// Frame advantage constants (internal frames)
#define FRAME_ADV_LVL1_BLOCK 9
#define FRAME_ADV_LVL2_BLOCK 14
#define FRAME_ADV_LVL3_BLOCK 21
#define FRAME_ADV_LVL1_HIT 10
#define FRAME_ADV_LVL2_HIT 16
#define FRAME_ADV_LVL3_HIT 23
#define FRAME_ADV_AIR_BLOCK 19

// Missing input constants
#define INPUT_DEVICE_KEYBOARD 0
#define INPUT_DEVICE_GAMEPAD 1
#define MAX_CONTROLLERS 4

// After the existing Move IDs section

// Base moveIDs for common attacks (most characters follow this pattern)
#define BASE_ATTACK_5A        200  // Standing A
#define BASE_ATTACK_5B        201  // Standing B
#define BASE_ATTACK_F5B       202  // Far B
#define BASE_ATTACK_5C        203  // Standing C
#define BASE_ATTACK_2A        204  // Crouching A 
#define BASE_ATTACK_2B        205  // Crouching B
#define BASE_ATTACK_2C        206  // Crouching C

// Air attack Move IDs - add these after the base attack constants
#define BASE_ATTACK_JA        207   // Jumping A
#define BASE_ATTACK_JB        208   // Jumping B  
#define BASE_ATTACK_JC        209   // Jumping C

// D button (Special button) attack Move IDs
#define BASE_ATTACK_5D        210   // Standing D
#define BASE_ATTACK_2D        211   // Crouching D
#define BASE_ATTACK_JD        212   // Jumping D

// Universal air-throw action: 248 is the attempt/startup; a successful catch
// transitions the thrower to 249. Grade 249 so a missed check that becomes j.C
// cannot satisfy an air-throw objective.
#define AIR_THROW_STARTUP_ID  248
#define AIR_THROW_SUCCESS_ID  249
#define BASE_AIRTHROW         AIR_THROW_SUCCESS_ID

// Auto-action trigger points
#define TRIGGER_NONE          0
#define TRIGGER_AFTER_BLOCK   1
#define TRIGGER_ON_WAKEUP     2
#define TRIGGER_AFTER_HITSTUN 3
#define TRIGGER_AFTER_AIRTECH 4  // New trigger
#define TRIGGER_ON_RG         5  // New: On Recoil Guard actionable

// Auto-action types
// Normals are grouped by posture with 4 buttons each (A/B/C/D) for easy modulo-4 button extraction.
// Specials and other actions follow after the normals.
#define ACTION_NONE           0
// Standing normals (5X) - indices 0-3
#define ACTION_5A             0
#define ACTION_5B             1
#define ACTION_5C             2
#define ACTION_5D             3
// Crouching normals (2X) - indices 4-7
#define ACTION_2A             4
#define ACTION_2B             5
#define ACTION_2C             6
#define ACTION_2D             7
// Jumping normals (jX) - indices 8-11
#define ACTION_JA             8
#define ACTION_JB             9
#define ACTION_JC            10
#define ACTION_JD            11
// Forward normals (6X) - indices 12-15
#define ACTION_6A            12
#define ACTION_6B            13
#define ACTION_6C            14
#define ACTION_6D            15
// Back normals (4X) - indices 16-19
#define ACTION_4A            16
#define ACTION_4B            17
#define ACTION_4C            18
#define ACTION_4D            19
// Special moves and commands - indices 20+
#define ACTION_QCF           20
#define ACTION_DP            21
#define ACTION_QCB           22
#define ACTION_421           23  // 421 command family
#define ACTION_SUPER1        24  // Legacy name/value: 41236 (ordinary special family)
#define ACTION_41236         ACTION_SUPER1
#define ACTION_SUPER2        25  // Legacy name/value: 2141236 super family
#define ACTION_2141236       ACTION_SUPER2
#define ACTION_236236        26  // Double QCF
#define ACTION_214214        27  // Double QCB
#define ACTION_JUMP          28
#define ACTION_BACKDASH      29
#define ACTION_FORWARD_DASH  30
#define ACTION_BLOCK         31
#define ACTION_FINAL_MEMORY  32  // Virtual action to request per-character Final Memory
#define ACTION_641236        33  // 641236 Super
#define ACTION_463214        34
#define ACTION_412           35
#define ACTION_22            36
#define ACTION_4123641236    37
#define ACTION_6321463214    38
// Character-specific normal recipes.  These are appended so saved ACTION_*
// values and legacy random-pool bit positions never change.
#define ACTION_1X            39
#define ACTION_3X            40
#define ACTION_J2X           41
#define ACTION_J6X           42
#define ACTION_66X           43
#define ACTION_662X          44
#define ACTION_664X          45
// Kaori-only staged recipe: native 44 first, then native 66 during the
// backdash action's frame-index 4/5 cancel window.  It cannot be represented
// faithfully as one combined input-history pattern.
#define ACTION_KAORI_RECOIL_DUCK 46
#define ACTION_COUNT         47

// Default delay for triggers
#define DEFAULT_TRIGGER_DELAY 0  // Default delay for all triggers

// Control IDs for auto-action UI
#define IDC_AUTOACTION_ENABLE     5001
#define IDC_AUTOACTION_ACTION     5003
#define IDC_AUTOACTION_CUSTOM_ID  5004 // (deprecated/no-op)
#define IDC_AUTOACTION_PLAYER     5005

// Add trigger checkboxes and delay controls
#define IDC_TRIGGER_AFTER_BLOCK_CHECK   5010
#define IDC_TRIGGER_AFTER_BLOCK_DELAY   5011
#define IDC_TRIGGER_ON_WAKEUP_CHECK     5012
#define IDC_TRIGGER_ON_WAKEUP_DELAY     5013
#define IDC_TRIGGER_AFTER_HITSTUN_CHECK 5014
#define IDC_TRIGGER_AFTER_HITSTUN_DELAY 5015
#define IDC_TRIGGER_AFTER_AIRTECH_CHECK 5016
#define IDC_TRIGGER_AFTER_AIRTECH_DELAY 5017

// Add these new control IDs after the existing trigger controls:

// Individual action selection for each trigger
#define IDC_TRIGGER_AFTER_BLOCK_ACTION     5018
#define IDC_TRIGGER_ON_WAKEUP_ACTION       5019
#define IDC_TRIGGER_AFTER_HITSTUN_ACTION   5020
#define IDC_TRIGGER_AFTER_AIRTECH_ACTION   5021

// New: On Recoil Guard trigger controls
#define IDC_TRIGGER_ON_RG_CHECK            5027
#define IDC_TRIGGER_ON_RG_DELAY            5028
#define IDC_TRIGGER_ON_RG_ACTION           5029

// Custom action IDs for each trigger
#define IDC_TRIGGER_AFTER_BLOCK_CUSTOM     5022 // deprecated
#define IDC_TRIGGER_ON_WAKEUP_CUSTOM       5023 // deprecated
#define IDC_TRIGGER_AFTER_HITSTUN_CUSTOM   5024 // deprecated
#define IDC_TRIGGER_AFTER_AIRTECH_CUSTOM   5025 // deprecated

// Debug/experimental: wake buffering toggle checkbox
#define IDC_TRIGGER_WAKE_BUFFER_CHECK      5026

// Macro selection combo boxes for each trigger ("None" + Slot 1..N)
#define IDC_TRIGGER_AFTER_BLOCK_MACRO      5030
#define IDC_TRIGGER_ON_WAKEUP_MACRO        5031
#define IDC_TRIGGER_AFTER_HITSTUN_MACRO    5032
#define IDC_TRIGGER_AFTER_AIRTECH_MACRO    5033
#define IDC_TRIGGER_ON_RG_MACRO            5034

// Character IDs - corrected to match internal game IDs
#define CHAR_ID_AKANE     0
#define CHAR_ID_AKIKO     1
#define CHAR_ID_IKUMI     2
#define CHAR_ID_MISAKI    3
#define CHAR_ID_SAYURI    4
#define CHAR_ID_KANNA     5
#define CHAR_ID_KAORI     6
#define CHAR_ID_MAKOTO    7
#define CHAR_ID_MINAGI    8
#define CHAR_ID_MIO       9
#define CHAR_ID_MISHIO    10
#define CHAR_ID_MISUZU    11
#define CHAR_ID_MIZUKA        12  // Mizuka Nagamori; resource/file name "nagamori"
#define CHAR_ID_UNKNOWN_BOSS  13  // Boss UNKNOWN; resource/file name "mizuka"
#define CHAR_ID_NANASE    14  // Actually Rumi in files
#define CHAR_ID_EXNANASE  15  // Actually Doppel in files
#define CHAR_ID_NAYUKI    16  // Actually Nayuki (Neyuki) in files
#define CHAR_ID_NAYUKIB   17  // Actually NayukiB in files
#define CHAR_ID_SHIORI    18
#define CHAR_ID_AYU       19
#define CHAR_ID_MAI       20
#define CHAR_ID_MAYU      21
#define CHAR_ID_UNKNOWN   22  // Playable UNKNOWN; resource/file name "mizukab"
#define CHAR_ID_KANO      23

// Character-specific offsets
// Ikumi's blood meter and genocide timer
#define IKUMI_LEVEL_GAUGE_OFFSET 0x3148  // 0..99 (100 triggers blood level up)
#define IKUMI_BLOOD_OFFSET   0x314C  // Blood meter (0-8)
#define IKUMI_GENOCIDE_OFFSET 0x3150  // Genocide timer

// Maximum values
#define IKUMI_BLOOD_MAX      8       // Blood ranges from 0-8
#define IKUMI_GENOCIDE_MAX   1260    // Depletes by 3 every frames

// Shiori's character-specific shield gauge reuses the same per-character resource
// slot as Ikumi's blood meter (player + 0x314C), but on a much larger scale.
// Freezing it at this value keeps the shield from ever depleting (user-verified in
// Cheat Engine: freeze [efz.exe+0x390104]+0x314C at 1992 = infinite Shiori shield).
#define SHIORI_SHIELD_OFFSET IKUMI_BLOOD_OFFSET  // 0x314C, shared per-char resource slot
#define SHIORI_SHIELD_FULL   1992                // "full" shield value that never empties

// Patch configuration for infinite blood mode
#define IKUMI_GENOCIDE_TIMER_ADDR 0x2A718    // Address to patch genocide timer decrement
#define IKUMI_GENOCIDE_TIMER_ORIGINAL "\x89\x86\x50\x31\x00\x00"  // Original bytes
#define IKUMI_GENOCIDE_TIMER_PATCH "\x90\x90\x90\x90\x90\x90"     // NOP sequence
//Misuzu
#define MISUZU_FEATHER_OFFSET 0x3464
#define MISUZU_FEATHER_MAX 3 // Feather count ranges from 0-3
// Misuzu Poison (timer and level)
#define MISUZU_POISON_TIMER_OFFSET 0x345C  // Counts down 3000->0 while poisoned
#define MISUZU_POISON_LEVEL_OFFSET 0x3460  // 0=inactive, nonzero=active (any value)
#define MISUZU_POISON_TIMER_MAX    3000

// Mishio (Element system)
// Offsets relative to player base
#define MISHIO_ELEMENT_OFFSET            0x3158  // 0=None, 1=Fire, 2=Lightning, 3=Awakened
#define MISHIO_ELEMENT_COOLDOWN_OFFSET   0x315C  // Decrements while element active
#define MISHIO_ELEMENT_CLOCK_OFFSET      0x3160  // Internal clock used for effects
#define MISHIO_AWAKENED_TIMER_OFFSET     0x3168  // Counts down while Awakened; when <=0, element resets to 0

// Mishio element values
#define MISHIO_ELEM_NONE      0
#define MISHIO_ELEM_FIRE      1
#define MISHIO_ELEM_LIGHTNING 2
#define MISHIO_ELEM_AWAKENED  3

// Target top-up for infinite awakened timer
#define MISHIO_AWAKENED_TARGET 4500

// Doppel Nanase (ExNanase) - Enlightened Final Memory flag
// *(DWORD*)(playerBase + 13396) toggles 0/1 when Enlightened is active
#define DOPPEL_ENLIGHTENED_OFFSET 0x3454

// Doppel Nanase (ExNanase) - Maiden Capture follow-up tech latch.
// DWORD on DOPPEL's OWN player struct at +0x3138 (12600 decimal). The engine
// latches it from the OPPONENT's rising-edge button bytes (+0x18A/+0x18B/+0x18C)
// inside a block gated on "latch == 0", which is what makes the first press stick.
// Values: 0 = nothing latched, 1 = A, 2 = B, 3 = C.
// A-branch follow-ups are refused when the latch is 3 (C); B-branch follow-ups are
// refused when the latch is 2 (B). Value 1 (A) matches neither test, so it is an
// effective lockout.
//
// WARNING: +0x3138 (12600) is NOT a dedicated field. Outside the four follow-up
// states below it is Doppel's generic per-move scratch DWORD, reused as a counter
// by her run (moveID 163), her velocity stash (170/171), her 214X hit loops
// (271-273) and her 623X loops (274-276), and it is meaningless for every other
// character. Reading or writing it without BOTH the character gate and the moveID
// gate corrupts unrelated state. The gate is mandatory, not defensive.
#define DOPPEL_TECH_LATCH_OFFSET 0x3138  // 12600 decimal

// Latch values
#define DOPPEL_TECH_NONE 0  // nothing latched yet
#define DOPPEL_TECH_A    1  // A pressed - escapes nothing (lockout)
#define DOPPEL_TECH_B    2  // B pressed - escapes the B-branch follow-ups
#define DOPPEL_TECH_C    3  // C pressed - escapes the A-branch follow-ups

// The only four Doppel moveIDs during which +0x3138 carries tech-choice meaning.
#define DOPPEL_MOVE_CAPTURE_HOLD    254  // Maiden Capture hold ("Followup Stage 1")
#define DOPPEL_MOVE_MAIDEN_CRASH    255  // Maiden Crash (Stage 2)
#define DOPPEL_MOVE_BARRAGE         300  // Relentless Granite-Breaking Barrage (Stage 3, A-line)
#define DOPPEL_MOVE_INNER_SOUL      306  // Exploding Inner-Soul Fist (Stage 3, B-line)
// Doppel's recovery state after a successful tech (the opponent goes to BACKWARD_AIRTECH 158).
#define DOPPEL_MOVE_TECHED_RECOVERY 258

// Command/motion tokens Doppel's follow-up branch selection reads out of
// MOTION_TOKEN_OFFSET (+0x262). A-line and B-line each have two encodings
// because 236X and 214X follow-ups are interchangeable.
#define DOPPEL_TOKEN_A_QCF   10   // 236A  (stage 1)
#define DOPPEL_TOKEN_B_QCF   11   // 236B  (stage 1)
#define DOPPEL_TOKEN_A_QCB   30   // 214A  (stage 1)
#define DOPPEL_TOKEN_B_QCB   31   // 214B  (stage 1)
#define DOPPEL_TOKEN_A_SUPER_QCF 100  // 236236A (stages 2/3)
#define DOPPEL_TOKEN_B_SUPER_QCF 101  // 236236B (stages 2/3)
#define DOPPEL_TOKEN_A_SUPER_QCB 110  // 214214A (stages 2/3)
#define DOPPEL_TOKEN_B_SUPER_QCB 111  // 214214B (stages 2/3)

// Nanase (Rumi) weapon/barehand mode swap
// Native toggleCharacterMode routine
#define TOGGLE_CHARACTER_MODE_RVA 0x0008E140
// Gate flag that enables barehand specials (1=barehand, 0=shinai)
#define RUMI_WEAPON_GATE_OFFSET   0x344C
// True mode byte toggled by in-game toggleCharacterMode (1=barehand/alternate, 0=shinai/normal)
#define RUMI_MODE_BYTE_OFFSET     0x3458
// Pointers to alternate resources (written during init)
#define RUMI_ALT_ANIM_PTR_OFFSET  0x345C
#define RUMI_ALT_MOVE_PTR_OFFSET  0x3460
// Pointers to normal resources
#define RUMI_NORM_ANIM_PTR_OFFSET 0x3474
#define RUMI_NORM_MOVE_PTR_OFFSET 0x3478
// Destination fields that the game swaps when changing modes
#define RUMI_ACTIVE_ANIM_PTR_DST  0x0010
#define RUMI_ACTIVE_MOVE_PTR_DST  0x0164

// Nanase (Rumi) super that forcibly drops Shinai
#define RUMI_SUPER_TOSS_A 308
#define RUMI_SUPER_TOSS_B 309
#define RUMI_SUPER_TOSS_C 310

// Nanase (Rumi) – Final Memory ("Kimchi") state
// *(DWORD*)(playerBase + 0x3148) toggles 0/1 around special state cases
// The timer at +0x314C feeds the special gauge rendering
#define RUMI_KIMCHI_TIMER_OFFSET  0x3148  // timer value used by render gauge
#define RUMI_KIMCHI_ACTIVE_OFFSET 0x314C  // 0=inactive, 1=active
// Clarified: starts at 3000 and decreases by 3 every visual frame
#define RUMI_KIMCHI_TARGET 3000
// Kimchi state MoveID observed to drive timer behavior
#define RUMI_KIMCHI_MOVE_ID 307

// Player input buffer offsets
#define P1_INPUT_BUFFER_OFFSET 0x1AB      // Base address of input buffer
#define P1_INPUT_BUFFER_INDEX_OFFSET 0x260 // Offset to current buffer index
#define P1_INPUT_BUFFER_SIZE 180          // Size of the circular buffer

// Motion token offset (immediately follows the 2-byte buffer head at 0x260)
// Writing 99 here neutralizes any in-flight motion command recognition to prevent
// unintended transitions after control handoffs or macro playback.
#define MOTION_TOKEN_OFFSET 0x262

// Air-mobility runtime counters. Decompilation shows both bytes reset on
// landing (updateCharacterMovementState), and character scripts increment them
// when air dashes / double jumps are consumed. Semantics vary slightly by
// character, so display them as raw engine counters rather than treating them
// as universal limits.
#define AIR_MOBILITY_COUNTER1_OFFSET 0x159
#define AIR_MOBILITY_COUNTER2_OFFSET 0x15A

// Add these input offset constants after the existing offset definitions

// Raw input offsets from player base
#define HORIZONTAL_INPUT_OFFSET 0x188  // Left = 255, Right = 1, Neutral = 0
#define VERTICAL_INPUT_OFFSET   0x189  // Up = 255, Down = 1, Neutral = 0
// Immediate button registers are contiguous bytes after vertical input
#define BUTTON_A_OFFSET         0x18A  // 0 = not pressed, 1 = pressed
#define BUTTON_B_OFFSET         0x18B
#define BUTTON_C_OFFSET         0x18C
#define BUTTON_D_OFFSET         0x18D

// Engine command flags
// These bytes sit just before the circular input buffer region and are toggled by
// the engine when a motion/command or dash pattern is recognized. Clearing them
// prevents residual moveID transitions after external input injection completes.
#define COMMAND_BUFFER_OFFSET   0x1A8  // Byte flag used by command recognizers
#define DASH_COMMAND_OFFSET     0x1A9  // Byte flag for dash recognition/state

// Aliases for clarity in cleanup code
#define INPUT_LATCH1_OFFSET     COMMAND_BUFFER_OFFSET
#define INPUT_LATCH2_OFFSET     DASH_COMMAND_OFFSET
// Dash timer byte that counts dash frames; clear during full cleanup
#define DASH_TIMER_OFFSET       0x1AA

// Block helper aliases (same raw slots as inputs)
#define BLOCK_DIRECTION_OFFSET  HORIZONTAL_INPUT_OFFSET // signed: -1 (left) / +1 (right) / 0 neutral
#define BLOCK_STANCE_OFFSET     VERTICAL_INPUT_OFFSET   // 0=stand, 1=crouch (down held)

// Animation/Frame data traversal (for per-frame block flags)
#define ANIM_TABLE_OFFSET                 0x164   // pointer to animation table
#define CURRENT_FRAME_INDEX_OFFSET        0x0A    // current frame index within state
#define ANIM_ENTRY_STRIDE                 8       // bytes per state entry in anim table
#define ANIM_ENTRY_FRAMES_PTR_OFFSET      4       // within entry, pointer to frames array
#define FRAME_BLOCK_STRIDE                200     // bytes per frame block (0xC8)
#define FRAME_ATTACK_PROPS_OFFSET         170     // word: high/low/any bits (0xAA)
#define FRAME_HIT_PROPS_OFFSET            176     // word: blockable bit at 0x10 (0xB0)
#define FRAME_GUARD_PROPS_OFFSET          178     // word: extra guard metadata (0xB2)

// Raw input values
#define RAW_INPUT_UP    255
#define RAW_INPUT_DOWN  1
#define RAW_INPUT_LEFT  255
#define RAW_INPUT_RIGHT 1
#define RAW_INPUT_NEUTRAL 0

// Akiko (Minase) – character-specific offsets
// From CE table and wiki:
// - Bullet cycle used by 236A/236B routes (integer)
// - Time-slow trigger: 0=inactive, 1=A, 2=B, 3=C, 4=Infinite timer
#define AKIKO_BULLET_CYCLE_OFFSET        0x3150
#define AKIKO_TIMESLOW_TRIGGER_OFFSET    0x0160
#define AKIKO_TIMESLOW_INACTIVE          0
#define AKIKO_TIMESLOW_A                 1
#define AKIKO_TIMESLOW_B                 2
#define AKIKO_TIMESLOW_C                 3
#define AKIKO_TIMESLOW_INFINITE          4 //unused in the mod since it breaks next timeslow activation

// Akiko time-slow on-screen counter digits (observed as XYZ). When set to 000, time-slow persists.
// These are 4-byte integers at the following offsets relative to the player base:
//  - Third number (X):   +0x3154
//  - Second number (Y):  +0x3158
//  - First number (Z):   +0x315C
#define AKIKO_TIMESLOW_THIRD_OFFSET      0x3154
#define AKIKO_TIMESLOW_SECOND_OFFSET     0x3158
#define AKIKO_TIMESLOW_FIRST_OFFSET      0x315C

// Mio – stance (short vs long) reuses the shared 0x3150 slot used by other characters for their
// own mechanics (e.g., Ikumi genocide / Akiko bullet cycle). Safe to alias by character ID.
// Observed values: 0 = Short stance, 1 = Long stance.
#define MIO_STANCE_OFFSET                0x3150
#define MIO_STANCE_SHORT                 0
#define MIO_STANCE_LONG                  1

// Kano – magic meter (0..10000) shares 0x3150 slot (character-specific reuse)
#define KANO_MAGIC_OFFSET                0x3150
#define KANO_MAGIC_MAX                   10000

// Nayuki (Awake) – Snowbunnies timer uses the shared 0x3150 slot
// Observed behavior (CE): counts down from ~3000 to 0 while active
#define NAYUKIB_SNOWBUNNY_TIMER_OFFSET   0x3150
#define NAYUKIB_SNOWBUNNY_MAX            3000
#define NAYUKIB_SNOWBUNNY_ACTIVE_FLAGS_BASE  0x3154  // Array: +0x3154 + (4*index) for 8 snowbunnies

// Akiko – Clean Hit helper last-hit Move IDs for 623 rekka
// A/B: 3rd hit ends at moveId 259; C: 6th hit ends at moveId 254
#define AKIKO_MOVE_623_LAST_AB           259
#define AKIKO_MOVE_623_LAST_C            254

// Neyuki (Sleepy Nayuki Minase) – Jam count
// integer at +0x3148 on the player struct (same slot other chars repurpose)
#define NEYUKI_JAM_COUNT_OFFSET          0x3148
#define NEYUKI_JAM_COUNT_MAX             9

// Mai (Kawasumi) – Mini-Mai system (single multi-purpose timer + status byte)
// Findings:
//   +0x3144 : status byte
//              0 = inactive
//              1 = Mini-Mai active (summoned)
//              2 = Unsummon sequence
//              3 = Charging (cooldown before next summon)
//              4 = Awakening install
//   +0x3148 : multi-use timer (int). Ranges depend on status:
//              - Active (1)      : up to 10000
//              - Charge (3)      : up to 1200
//              - Awakening (4)   : up to 10000
// We keep legacy field names (ghost/charge/awakening) in DisplayData but they all map to the same timer based on status.
#define MAI_STATUS_OFFSET                0x3144  // byte
#define MAI_MULTI_TIMER_OFFSET           0x3148  // int (shared)
// Backwards compatibility aliases (all point to multi-timer; code paths choose by status)
#define MAI_GHOST_TIME_OFFSET            MAI_MULTI_TIMER_OFFSET
#define MAI_GHOST_CHARGE_OFFSET          MAI_MULTI_TIMER_OFFSET
#define MAI_AWAKENING_TIMER_OFFSET       MAI_MULTI_TIMER_OFFSET
// Status-specific maxima
#define MAI_GHOST_TIME_MAX               10000
#define MAI_GHOST_CHARGE_MAX             1200
#define MAI_AWAKENING_MAX                10000
// Additional Mai internal helper flags (observed in 0x191 state machine):
//  +0x314C : transient int used when entering charge (we saw 12620 cleared) – treat as CHARGE_HELPER
//  +0x3150 : one-shot summon flash flag (12624) reused by multiple characters; when set to 1 before
//            first tick of active (0x191) causes an effect spawn then auto-clears. We expose for
//            Force Summon to mimic natural spawn visuals without needing deep engine calls.
#define MAI_CHARGE_HELPER_OFFSET         0x314C
#define MAI_SUMMON_FLASH_FLAG_OFFSET     0x3150
// Native summon script moveID (case 0x104) – use to trigger authentic spawn sequence
#define MAI_SUMMON_MOVE_ID                0x0104  // 260 decimal
// Internal per-state counters (observed at +0x0A / +0x0C) used by engine switch logic
#define STATE_FRAME_INDEX_OFFSET          0x0A    // already synonymous with CURRENT_FRAME_INDEX_OFFSET
#define STATE_SUBFRAME_COUNTER_OFFSET     0x0C

// Mini-Mai (ghost) runtime slot array:
// Player struct contains an array of small slot structs starting at +0x04D0 (1232) with stride 152 (0x98).
// For Mai, one of these slots (ID == 401) represents the active Mini-Mai entity. Fields (relative to slot start):
//   +0x00 : uint16 id (401 = Mini-Mai, other values reused by engine scripts)
//   +0x02 : uint16 frame index / state counter (matches *(_WORD*)(...+1234))
//   +0x04 : uint16 subframe counter (matches *(_WORD*)(...+1236))
//   +0x18 : double X position  (offset 1256 = 1232 + 0x18)
//   +0x20 : double Y position  (offset 1264 = 1232 + 0x20)
// Additional arrays parallel to slots exist (e.g., int arrays at +0x3158 etc) but not needed for position readout.
// We scan a conservative number of slots (MAX 12) to locate ID 401 each refresh.
#define MAI_GHOST_SLOTS_BASE              0x04D0
#define MAI_GHOST_SLOT_STRIDE             152      // 0x98
#define MAI_GHOST_SLOT_ID_OFFSET          0x00
#define MAI_GHOST_SLOT_FRAME_OFFSET       0x02
#define MAI_GHOST_SLOT_SUBFRAME_OFFSET    0x04
#define MAI_GHOST_SLOT_X_OFFSET           0x18
#define MAI_GHOST_SLOT_Y_OFFSET           0x20
#define MAI_GHOST_SLOT_MAX_SCAN           12       // Safety cap (engine likely uses fewer)

// Minagi (Tohno Minagi) – (Michiru) entity uses the same slot array layout as Mini-Mai
// Entity ID for Michiru observed: 400
// Reuse the same slot base/stride/offsets; only the ID differs
#define MINAGI_PUPPET_ENTITY_ID           400
#define MINAGI_PUPPET_SLOTS_BASE          MAI_GHOST_SLOTS_BASE
#define MINAGI_PUPPET_SLOT_STRIDE         MAI_GHOST_SLOT_STRIDE
#define MINAGI_PUPPET_SLOT_ID_OFFSET      MAI_GHOST_SLOT_ID_OFFSET
#define MINAGI_PUPPET_SLOT_FRAME_OFFSET   MAI_GHOST_SLOT_FRAME_OFFSET
#define MINAGI_PUPPET_SLOT_SUBFRAME_OFFSET MAI_GHOST_SLOT_SUBFRAME_OFFSET
#define MINAGI_PUPPET_SLOT_X_OFFSET       MAI_GHOST_SLOT_X_OFFSET
#define MINAGI_PUPPET_SLOT_Y_OFFSET       MAI_GHOST_SLOT_Y_OFFSET
#define MINAGI_PUPPET_SLOT_MAX_SCAN       MAI_GHOST_SLOT_MAX_SCAN
