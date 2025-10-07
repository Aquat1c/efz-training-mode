// practice_offsets.h
// Centralized EfzRevival RVAs and Practice controller offsets used by the mod.
// This replaces the previous inclusion of out/efz_practice_offsets.h.

#pragma once

#include <stdint.h>

// Helper: convert module base + RVA to VA
#ifndef EFZ_RVA_TO_VA
#define EFZ_RVA_TO_VA(hmod, rva) (reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(hmod) + static_cast<uintptr_t>(rva)))
#endif

// ==============================
// EfzRevival.dll RVAs (x86)
// ==============================
// NOTE: New code should prefer the version-aware accessors in efzrevival_addrs.h.
// These macros remain for reference, docs, and places that have not yet been migrated
// or are known-stable across builds.

// Practice tick (thiscall ECX=this) — we hook this to capture the Practice controller pointer
#ifndef EFZREV_RVA_PRACTICE_TICK
#define EFZREV_RVA_PRACTICE_TICK 0x0074F70
#endif

// Global array: pointers to game mode structs (index * 4 + 0x790110)
// Index 3 corresponds to Practice/Training environment; the pointer at
// [EfzRevival.dll + 0x790110 + 3*4] is the Practice controller 'this'.
#ifndef EFZREV_RVA_GAME_MODE_PTR_ARRAY
#define EFZREV_RVA_GAME_MODE_PTR_ARRAY 0x790110
#endif

// Pause toggle function (not directly used by current code but kept for reference)
#ifndef EFZREV_RVA_TOGGLE_PAUSE
#define EFZREV_RVA_TOGGLE_PAUSE 0x0075720
#endif

// Practice hotkey evaluation function (scans and dispatches Pause/Step/Record/etc.).
// Initial RVA determined via reverse engineering of clustered reads of offsets 0x1D4..0x230
// and calls to sub_10075720 (official pause). If this shifts in later versions, the
// runtime scanner in practice_hotkey_gate.cpp can recover; keep this as a fast-path.
#ifndef EFZREV_RVA_PRACTICE_HOTKEY_EVAL
#define EFZREV_RVA_PRACTICE_HOTKEY_EVAL 0x00773A0
#endif

// Overlay / display toggle tiny stubs (cmp/sete/mov/ret patterns)
#ifndef EFZREV_RVA_TOGGLE_HURTBOXES
#define EFZREV_RVA_TOGGLE_HURTBOXES 0x0075140
#endif
#ifndef EFZREV_RVA_TOGGLE_HITBOXES
#define EFZREV_RVA_TOGGLE_HITBOXES 0x0075160
#endif
#ifndef EFZREV_RVA_TOGGLE_DISPLAY
#define EFZREV_RVA_TOGGLE_DISPLAY 0x00756E0
#endif

// Step logic is embedded near PracticeTick; we neutralize via PracticeTick hook rather than separate RVA.

// Battle screen render (thiscall battleContext) – we hook this to capture battleContext
// Name in decompile: renderBattleScreen, original label sub_7642A0
#ifndef EFZ_RVA_RENDER_BATTLE_SCREEN
#define EFZ_RVA_RENDER_BATTLE_SCREEN 0x007642A0
#endif

// Central patch toggler context/function (used internally by EfzRevival)
#ifndef EFZREV_RVA_PATCH_TOGGLER
#define EFZREV_RVA_PATCH_TOGGLER 0x006B2A0
#endif

// Optional: patch context struct in EfzRevival (address passed to patch toggler)
#ifndef EFZREV_RVA_PATCH_CTX
#define EFZREV_RVA_PATCH_CTX 0x00A0760
#endif

// Mapping reset used by init after switching sides: sub_1006D640((char **)(this + 8 * (local + 104)))
#ifndef EFZREV_RVA_MAP_RESET
#define EFZREV_RVA_MAP_RESET 0x006D640
#endif

// Cleanup pair called after switching to local==1 during init: EFZ_Obj_SubStruct448_CleanupPair(&dword_100A0760)
#ifndef EFZREV_RVA_CLEANUP_PAIR
#define EFZREV_RVA_CLEANUP_PAIR 0x006CAD0
#endif

// Copies 0x20 bytes of mapping block from EFZ patch ctx into Practice (+4..+0x24 region)
// qmemcpy((this+4), EFZ_Obj_GetSubStructOffset448(&dword_100A0760), 0x20)
#ifndef EFZREV_RVA_REFRESH_MAPPING_BLOCK
#define EFZREV_RVA_REFRESH_MAPPING_BLOCK 0x0075100
#endif

// ======================================
// Practice controller layout (offsets)
// Offsets are relative to the Practice controller "this" pointer captured from tick.
// ======================================

// Pause/Step core fields
#ifndef PRACTICE_OFF_STEP_FLAG
#define PRACTICE_OFF_STEP_FLAG        0xAC   // byte: 1 = advance one frame next tick (while paused)
#endif

#ifndef PRACTICE_OFF_STEP_COUNTER
#define PRACTICE_OFF_STEP_COUNTER     0xB0   // dword: increments each single-frame step
#endif

#ifndef PRACTICE_OFF_PAUSE_FLAG
#define PRACTICE_OFF_PAUSE_FLAG       0xB4   // byte: 1 = paused
#endif

// Optional: speed scalar double at +0xC0/+0xC4 (not used directly by code here)
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
#define PRACTICE_OFF_LOCAL_SIDE_IDX   0x680  // dword: 0 = P1, 1 = P2
#endif
#ifndef PRACTICE_OFF_REMOTE_SIDE_IDX
#define PRACTICE_OFF_REMOTE_SIDE_IDX  0x684  // dword: companion of local index
#endif
#ifndef PRACTICE_OFF_SIDE_BUF_PRIMARY
#define PRACTICE_OFF_SIDE_BUF_PRIMARY 0x824  // ptr: primary side buffer (tracks LOCAL)
#endif
#ifndef PRACTICE_OFF_SIDE_BUF_SECONDARY
#define PRACTICE_OFF_SIDE_BUF_SECONDARY 0x828 // ptr: secondary side buffer (tracks REMOTE)
#endif
// Internal buffer blocks used during init to wire side buffers
#ifndef PRACTICE_OFF_BUF_LOCAL_BASE
#define PRACTICE_OFF_BUF_LOCAL_BASE    0x788  // when local==P1, primary points here
#endif
#ifndef PRACTICE_OFF_BUF_REMOTE_BASE
#define PRACTICE_OFF_BUF_REMOTE_BASE   0x800  // when local==P2, primary points here
#endif
#ifndef PRACTICE_OFF_INIT_SOURCE_SIDE
#define PRACTICE_OFF_INIT_SOURCE_SIDE 0x944  // dword: remembered init source side
#endif
#ifndef PRACTICE_OFF_SHARED_INPUT_VEC
#define PRACTICE_OFF_SHARED_INPUT_VEC 0x1240 // base of shared input slots (InputP1/InputP2), if needed
#endif

// Current GUI/buffer display position used by EfzRevival
// Observed at EfzRevival.dll+75A98: mov [esi+0x24], eax after a sete -> value is 1 when P1, 0 when P2.
#ifndef PRACTICE_OFF_GUI_POS
#define PRACTICE_OFF_GUI_POS            0x24  // dword/byte: 1 = P1, 0 = P2 (GUI position)
#endif

// Deprecated alias: previously misnamed as a P2 human gate; kept for compatibility in code references.
#ifndef PRACTICE_OFF_P2_HUMAN_GATE
#define PRACTICE_OFF_P2_HUMAN_GATE      PRACTICE_OFF_GUI_POS
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
#ifndef GAMESTATE_OFF_P2_CPU_FLAG
#define GAMESTATE_OFF_P2_CPU_FLAG       4931 // byte: P2 CPU/Human
#endif
#ifndef GAMESTATE_OFF_P1_CPU_FLAG
#define GAMESTATE_OFF_P1_CPU_FLAG       4932 // byte: P1 CPU/Human
#endif
