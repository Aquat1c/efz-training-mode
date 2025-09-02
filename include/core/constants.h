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
// Screen/state byte (observed via Cheat Engine):
// 0=Title, 1=Character Select, 2=Loading, 3=In-game, 5=Win screen, 6=Settings, 8=Replay select
#define EFZ_BASE_OFFSET_SCREEN_STATE 0x390148
#define XPOS_OFFSET 0x20
#define YPOS_OFFSET 0x28
#define HP_OFFSET 0x108
#define METER_OFFSET 0x148
#define MOVE_ID_OFFSET 0x8
#define RF_OFFSET 0x118
#define GAME_MODE_OFFSET 0x1364 // CORRECTED: The offset is hexadecimal, not decimal.
#define YVEL_OFFSET 0x38  // Y velocity offset from player base

// IC (Instant Charge) color offset - 0=red IC, 1=blue IC
#define IC_COLOR_OFFSET 0x120  // IC color offset from player base

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
#define GROUNDTECH_RECOVERY 96  // Add this new constant for the recovery state

// Jump Move IDs - add these after the Tech Move IDs section
#define STRAIGHT_JUMP_ID 4
#define FORWARD_JUMP_ID 5
#define BACKWARD_JUMP_ID 6
#define FALLING_ID 9
#define LANDING_ID 13 

// Special Stun States
#define FIRE_STATE 81
#define ELECTRIC_STATE 82
#define FROZEN_STATE_START 83
#define FROZEN_STATE_END 86

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

// Untech memory offset
#define UNTECH_OFFSET 0x124

// Tech recovery frames
#define AIRTECH_VULNERABLE_FRAMES 16
#define RUMI_AIRTECH_DOUBLEJUMP_LOCKOUT 31

// Helper macros
#define CLAMP(val, min, max) ((val)<(min)?(min):((val)>(max)?(max):(val)))

// Auto-Airtech patch addresses and original bytes
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
#define FORWARD_DASH_START_ID 163
#define FORWARD_DASH_RECOVERY_ID 164
#define BACKWARD_DASH_START_ID 165
#define BACKWARD_DASH_RECOVERY_ID 166

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
#define BASE_AIRTHROW         241   // Air throw

// Auto-action trigger points
#define TRIGGER_NONE          0
#define TRIGGER_AFTER_BLOCK   1
#define TRIGGER_ON_WAKEUP     2
#define TRIGGER_AFTER_HITSTUN 3
#define TRIGGER_AFTER_AIRTECH 4  // New trigger

// Auto-action types (keep existing)
#define ACTION_NONE           0
#define ACTION_5A             0
#define ACTION_5B             1
#define ACTION_5C             2
#define ACTION_2A             3
#define ACTION_2B             4
#define ACTION_2C             5
#define ACTION_JA             6
#define ACTION_JB             7
#define ACTION_JC             8
#define ACTION_QCF            9
#define ACTION_DP            10
#define ACTION_QCB           11
#define ACTION_SUPER1        12
#define ACTION_SUPER2        13
#define ACTION_JUMP          14
#define ACTION_BACKDASH      15
#define ACTION_BLOCK         16
#define ACTION_CUSTOM        17
#define ACTION_421           18 // or next available value

// Default delay for triggers
#define DEFAULT_TRIGGER_DELAY 0  // Default delay for all triggers

// Control IDs for auto-action UI
#define IDC_AUTOACTION_ENABLE     5001
#define IDC_AUTOACTION_ACTION     5003
#define IDC_AUTOACTION_CUSTOM_ID  5004
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

// Custom action IDs for each trigger
#define IDC_TRIGGER_AFTER_BLOCK_CUSTOM     5022
#define IDC_TRIGGER_ON_WAKEUP_CUSTOM       5023
#define IDC_TRIGGER_AFTER_HITSTUN_CUSTOM   5024
#define IDC_TRIGGER_AFTER_AIRTECH_CUSTOM   5025

// After your existing character constants, add:

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
#define CHAR_ID_MIZUKA    12  // Actually Nagamori in files
#define CHAR_ID_NAGAMORI  13
#define CHAR_ID_NANASE    14  // Actually Rumi in files
#define CHAR_ID_EXNANASE  15  // Actually Doppel in files
#define CHAR_ID_NAYUKI    16  // Actually NayukiB (Neyuki) in files
#define CHAR_ID_NAYUKIB   17  // Actually Nayuki in files
#define CHAR_ID_SHIORI    18
#define CHAR_ID_AYU       19
#define CHAR_ID_MAI       20
#define CHAR_ID_MAYU      21
#define CHAR_ID_MIZUKAB   22  // Actually Unknown in files
#define CHAR_ID_KANO      23

// Character-specific offsets
// Ikumi's blood meter and genocide timer
#define IKUMI_BLOOD_OFFSET   0x314C  // Blood meter (0-8)
#define IKUMI_GENOCIDE_OFFSET 0x3150  // Genocide timer

// Maximum values
#define IKUMI_BLOOD_MAX      8       // Blood ranges from 0-8
#define IKUMI_GENOCIDE_MAX   1260    // Depletes by 3 every frames

// Patch addresses for infinite blood mode
#define IKUMI_GENOCIDE_TIMER_ADDR 0x2A718    // Address to patch genocide timer decrement
#define IKUMI_GENOCIDE_TIMER_ORIGINAL "\x89\x86\x50\x31\x00\x00"  // Original bytes
#define IKUMI_GENOCIDE_TIMER_PATCH "\x90\x90\x90\x90\x90\x90"     // NOP sequence
//Misuzu
#define MISUZU_FEATHER_OFFSET 0x3464
#define MISUZU_FEATHER_MAX 3 // Feather count ranges from 0-3

// Mishio (Element system)
// Offsets relative to player base (from reverse-engineering/CE)
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
// From decomp: *(DWORD*)(playerBase + 13396) toggles 0/1 when Enlightened is active
#define DOPPEL_ENLIGHTENED_OFFSET 0x3454

// Nanase (Rumi) weapon/barehand mode swap
// RVA of the engine's native toggleCharacterMode routine
// Decomp VA: 0x0048E140; Module base: 0x00400000; RVA = 0x0008E140
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

// Nanase (Rumi) super that forcibly drops Shinai (moveIDs from reverse-engineering)
#define RUMI_SUPER_TOSS_A 308
#define RUMI_SUPER_TOSS_B 309
#define RUMI_SUPER_TOSS_C 310

// Nanase (Rumi) – Final Memory ("Kimchi") state
// From decomp: *(DWORD*)(playerBase + 0x3148) toggles 0/1 around special state cases
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

// Add these input offset constants after the existing offset definitions

// Raw input offsets from player base (from Cheat Engine findings)
#define HORIZONTAL_INPUT_OFFSET 0x188  // Left = 255, Right = 1, Neutral = 0
#define VERTICAL_INPUT_OFFSET   0x189  // Up = 255, Down = 1, Neutral = 0
// Immediate button registers are contiguous bytes after vertical input
#define BUTTON_A_OFFSET         0x18A  // 0 = not pressed, 1 = pressed
#define BUTTON_B_OFFSET         0x18B
#define BUTTON_C_OFFSET         0x18C
#define BUTTON_D_OFFSET         0x18D

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
#define AKIKO_TIMESLOW_INFINITE          4