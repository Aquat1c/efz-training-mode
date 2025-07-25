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
#define XPOS_OFFSET 0x20
#define YPOS_OFFSET 0x28
#define HP_OFFSET 0x108
#define METER_OFFSET 0x148
#define MOVE_ID_OFFSET 0x8
#define RF_OFFSET 0x118
#define YVEL_OFFSET 0x38  // Y velocity offset from player base

// Character name offset
#define CHARACTER_NAME_OFFSET 0x94  // Character name offset from player base

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
#define LANDING_ID 13  // Already defined, no need to add again

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
#define ACTION_5A             1
#define ACTION_5B             2
#define ACTION_5C             3
#define ACTION_2A             4
#define ACTION_2B             5
#define ACTION_2C             6
#define ACTION_JUMP           7
#define ACTION_BACKDASH       8
#define ACTION_BLOCK          9
#define ACTION_CUSTOM        10

// Add these new action types after the existing ones:

#define ACTION_JA            11  // Jumping A
#define ACTION_JB            12  // Jumping B  
#define ACTION_JC            13  // Jumping C
#define ACTION_AIRTHROW      14  // Air throw

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
#define YVEL_OFFSET 0x38