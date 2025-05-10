#pragma once

// HP and Resource limits
#define MAX_HP 9999
#define MAX_METER 3000
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

// Game frame rate settings
#define SUBFRAMES_PER_VISUAL_FRAME 3.0

// RG Constants
#define RG_STAND_FREEZE_DEFENDER 20
#define RG_STAND_FREEZE_ATTACKER 19.66
#define RG_CROUCH_FREEZE_DEFENDER 22
#define RG_CROUCH_FREEZE_ATTACKER 19.66
#define RG_AIR_FREEZE_DEFENDER 22
#define RG_AIR_FREEZE_ATTACKER 20
#define RG_STUN_DURATION 20     // 21.66 for Sayuri, but 20 for everyone else

// Helper macros
#define CLAMP(val, min, max) ((val)<(min)?(min):((val)>(max)?(max):(val)))

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