// practice_offsets.h

#pragma once

#include <stdint.h>
// Version-aware accessors for Practice controller offsets
#include "efzrevival_addrs.h"

// Helper: convert module base + RVA to VA
#ifndef EFZ_RVA_TO_VA
#define EFZ_RVA_TO_VA(hmod, rva) (reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(hmod) + static_cast<uintptr_t>(rva)))
#endif

// ==============================
// Engine RVAs (x86)
// ==============================

// Practice tick (thiscall ECX=this)
#ifndef EFZREV_RVA_PRACTICE_TICK
#define EFZREV_RVA_PRACTICE_TICK 0x0074F70
#endif

// Global array: pointers to game mode structs (index * 4 + 0x790110)
#ifndef EFZREV_RVA_GAME_MODE_PTR_ARRAY
#define EFZREV_RVA_GAME_MODE_PTR_ARRAY 0x790110
#endif

// Pause toggle function (not directly used by current code)
#ifndef EFZREV_RVA_TOGGLE_PAUSE
#define EFZREV_RVA_TOGGLE_PAUSE 0x0075720
#endif

// Practice hotkey evaluation function (scans and dispatches Pause/Step/Record/etc.).
#ifndef EFZREV_RVA_PRACTICE_HOTKEY_EVAL
#define EFZREV_RVA_PRACTICE_HOTKEY_EVAL 0x00773A0
#endif

// Overlay / display toggle stubs
#ifndef EFZREV_RVA_TOGGLE_HURTBOXES
#define EFZREV_RVA_TOGGLE_HURTBOXES 0x0075140
#endif
#ifndef EFZREV_RVA_TOGGLE_HITBOXES
#define EFZREV_RVA_TOGGLE_HITBOXES 0x0075160
#endif
#ifndef EFZREV_RVA_TOGGLE_DISPLAY
#define EFZREV_RVA_TOGGLE_DISPLAY 0x00756E0
#endif

// Step logic is embedded near PracticeTick.

// Battle screen render (thiscall battleContext)
#ifndef EFZ_RVA_RENDER_BATTLE_SCREEN
#define EFZ_RVA_RENDER_BATTLE_SCREEN 0x007642A0
#endif

// Central patch toggler context/function
#ifndef EFZREV_RVA_PATCH_TOGGLER
#define EFZREV_RVA_PATCH_TOGGLER 0x006B2A0
#endif

// Optional: patch context struct (address passed to patch toggler)
#ifndef EFZREV_RVA_PATCH_CTX
#define EFZREV_RVA_PATCH_CTX 0x00A0760
#endif

// Mapping reset used by init after switching sides.
#ifndef EFZREV_RVA_MAP_RESET
#define EFZREV_RVA_MAP_RESET 0x006D640
#endif

// Cleanup pair called after switching to local==1 during init.
#ifndef EFZREV_RVA_CLEANUP_PAIR
#define EFZREV_RVA_CLEANUP_PAIR 0x006CAD0
#endif

// Copies 0x20 bytes of mapping block into Practice (+4..+0x24 region)
#ifndef EFZREV_RVA_REFRESH_MAPPING_BLOCK
#define EFZREV_RVA_REFRESH_MAPPING_BLOCK 0x0075100
#endif

// ======================================
// Practice controller layout (offsets)
// Offsets are relative to the Practice controller "this" pointer.
// ======================================

// Pause/Step core fields
// ⚠️ WARNING: These offsets are VERSION-SPECIFIC!

#ifndef PRACTICE_OFF_STEP_FLAG
#define PRACTICE_OFF_STEP_FLAG        (EFZ_Practice_StepFlagOffset())
#endif

#ifndef PRACTICE_OFF_STEP_COUNTER
#define PRACTICE_OFF_STEP_COUNTER     (EFZ_Practice_StepCounterOffset())
#endif

#ifndef PRACTICE_OFF_PAUSE_FLAG
#define PRACTICE_OFF_PAUSE_FLAG       (EFZ_Practice_PauseFlagOffset())
#endif

// Optional: speed scalar double at +0xC0/+0xC4
#ifndef PRACTICE_OFF_SPEED_DBL_HI
#define PRACTICE_OFF_SPEED_DBL_HI     0xC0
#endif
#ifndef PRACTICE_OFF_SPEED_DBL_LO
#define PRACTICE_OFF_SPEED_DBL_LO     0xC4
#endif

// Hotkey command codes (from Practice config)
#ifndef PRACTICE_OFF_PAUSE_KEY
#define PRACTICE_OFF_PAUSE_KEY        0x1D4  // dword
#endif
#ifndef PRACTICE_OFF_STEP_KEY
#define PRACTICE_OFF_STEP_KEY         0x1D8  // dword
#endif

// Side switching and input routing
#ifndef PRACTICE_OFF_LOCAL_SIDE_IDX
#define PRACTICE_OFF_LOCAL_SIDE_IDX   (EFZ_Practice_LocalSideOffset())
#endif
#ifndef PRACTICE_OFF_REMOTE_SIDE_IDX
#define PRACTICE_OFF_REMOTE_SIDE_IDX  (EFZ_Practice_RemoteSideOffset())
#endif
#ifndef PRACTICE_OFF_SIDE_BUF_PRIMARY
#define PRACTICE_OFF_SIDE_BUF_PRIMARY (EFZ_Practice_SideBufPrimaryOffset())
#endif
#ifndef PRACTICE_OFF_SIDE_BUF_SECONDARY
#define PRACTICE_OFF_SIDE_BUF_SECONDARY (EFZ_Practice_SideBufSecondaryOffset())
#endif
// Internal buffer blocks used during init to wire side buffers (the actual buffer memory does not move)
#ifndef PRACTICE_OFF_BUF_LOCAL_BASE
#define PRACTICE_OFF_BUF_LOCAL_BASE    0x796  // P1 buffer base (when local==P1, primary points here)
#endif
#ifndef PRACTICE_OFF_BUF_REMOTE_BASE
#define PRACTICE_OFF_BUF_REMOTE_BASE   0x808  // P2 buffer base (when local==P2, primary points here)
#endif
#ifndef PRACTICE_OFF_INIT_SOURCE_SIDE
#define PRACTICE_OFF_INIT_SOURCE_SIDE (EFZ_Practice_InitSourceSideOffset())
#endif
#ifndef PRACTICE_OFF_SHARED_INPUT_VEC
#define PRACTICE_OFF_SHARED_INPUT_VEC (EFZ_Practice_SharedInputVectorOffset())
#endif

// Current GUI/buffer display position value: 1 when P1, 0 when P2.
#ifndef PRACTICE_OFF_GUI_POS
#define PRACTICE_OFF_GUI_POS            0x24  // dword/byte: 1 = P1, 0 = P2 (GUI position)
#endif

// Deprecated alias: previously misnamed as a P2 human gate; kept for compatibility in code references.
#ifndef PRACTICE_OFF_P2_HUMAN_GATE
#define PRACTICE_OFF_P2_HUMAN_GATE      PRACTICE_OFF_GUI_POS
#endif

// MapReset index bias used when selecting the per-side map pointer during init/swap
#ifndef EFZ_PRACTICE_MAPRESET_INDEX_BIAS
#define EFZ_PRACTICE_MAPRESET_INDEX_BIAS (EFZ_Practice_MapResetIndexBias())
#endif

// ======================================
// Engine game state (efz.exe) offsets
// ======================================
// These offsets are relative to the game state pointer stored at
// [efz.exe + EFZ_BASE_OFFSET_GAME_STATE].

// Active player index (used by some HUD/practice logic)
#ifndef GAMESTATE_OFF_ACTIVE_PLAYER
#define GAMESTATE_OFF_ACTIVE_PLAYER     4930 // byte: 0=P1, 1=P2
#endif

// Per-side CPU flags (1 = CPU, 0 = Human)
// NOTE: These are SPATIAL (left/right), not player numbers!
// P1 is always on LEFT, P2 is always on RIGHT
#ifndef GAMESTATE_OFF_LEFT_SIDE_CPU_FLAG
#define GAMESTATE_OFF_LEFT_SIDE_CPU_FLAG  4931 // byte: LEFT side CPU/Human (P1's position)
#endif
#ifndef GAMESTATE_OFF_RIGHT_SIDE_CPU_FLAG  
#define GAMESTATE_OFF_RIGHT_SIDE_CPU_FLAG 4932 // byte: RIGHT side CPU/Human (P2's position)
#endif

// Legacy aliases for compatibility
#define GAMESTATE_OFF_P1_CPU_FLAG GAMESTATE_OFF_LEFT_SIDE_CPU_FLAG   // P1 = left side
#define GAMESTATE_OFF_P2_CPU_FLAG GAMESTATE_OFF_RIGHT_SIDE_CPU_FLAG  // P2 = right side
