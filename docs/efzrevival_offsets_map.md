# EfzRevival pointers used by efz-training-mode (inventory + 1.02e→1.02h notes)

This document lists every EfzRevival.dll pointer/RVA and Practice-struct offset that this mod currently relies on, with where each appears in our code, and mapping evidence from the 1.02e decompilation. It also proposes initial candidates for 1.02h based on the unrefactored decompile, plus next steps to confirm via signatures at runtime.

Note: RVAs are relative to the EfzRevival.dll module base. 32-bit x86.

---

## Global/runtime state

- Online state flag
  - 1.02e: RVA 0x00A05D0 (symbol: `dword_100A05D0`)
    - Evidence: out/docs/EfzRevival.dll_1.02e.c: `int dword_100A05D0;`
    - Code use: `src/utils/network.cpp` reads base + 0xA05D0 and maps values to OnlineState {0,1,2,3}.
  1.02h: RVA 0x00A05F0 (symbol: `dword_100A05F0`)
    - Evidence: out/docs/EfzRevival.dll.1.02h.c:
      - Line ~71935: `if ( sub_1006D340() == 8 && dword_100A05F0 == 2 && sub_1006C860() == 4 )` — matches the 1.02e pattern (`EFZ_GameMode_GetCurrentIndex() == 8 && dword_100A05D0 == 2 && EFZ_Mode0_ReadFlag1084() == 4`).
      - Line ~72839 (init): `dword_100A05F0 = v7;` followed by a switch on `v7` constructing mode-specific controllers.
    - Notes:
      - Neighbor `dword_100A05F4` is used as a last-seen guard/cached value around mode changes (see lines ~71926..~71932 and reset at ~72816).
      - Cheat Engine: EfzRevival.dll+0xA05F0 (int; 0..3) appears to be the online/netplay mode id.

- Patch context (EFZ internal “patch toggler” context)
  - 1.02e: RVA 0x00A0760 (symbol: `dword_100A0760`)
    - Evidence: out/docs/EfzRevival.dll_1.02e.c lines around decomp show multiple uses.
    - Code use: Patch freeze/unfreeze and cleanups read this address.
  1.02h: 0x00A05F0 (symbol: `dword_100A05F0`) — CONFIRMED and wired
  1.02h: 0x00A0780 (symbol: `dword_100A0780`) — CONFIRMED in decomp and wired
  - 1.02h: RVA 0x0076170 (`sub_10076170`) — CONFIRMED; parameter to patch toggler is (0/3)
  - 1.02h: RVA 0x006BB00 (`sub_1006BB00(int* this, char enable)`) — enable=3 unfreezes

- Practice tick (captures Practice controller `this`)
  - 1.02e: RVA 0x0074F70 (decomp: `sub_10074F70`)
    - Code use: Hook target `EFZREV_RVA_PRACTICE_TICK` in `pause_integration.cpp`.
  - 1.02h: candidate RVA 0x0074F40 (`sub_10074F40`)
    - Rationale: Nearby cluster includes `sub_10074E80`, `sub_10074EF0`, `sub_10074F40`. In 1.02e, a refresh helper sits at 0x75100; these shifted earlier by ~0x2C0–0x3C0. PracticeTick plausibly tracked from 74F70→74F40.
    - Next step: verify by checking for `++` on the step counter at `this + 0xB0` or toggling logic around `this + 0xB4` inside the function body (pattern scan).

- Pause toggle (official Practice pause; Space/P)
  - 1.02e: RVA 0x0075720 (`sub_10075720`)
    - Code use: `EFZREV_RVA_TOGGLE_PAUSE` hook/call in `pause_integration.cpp`.
  - 1.02h: TBD
    - Rationale: direct symbol not present near 0x757xx; likely moved into 0x74Cxx–0x750xx neighborhood (e.g., `sub_10074C60` looks like a small boolean-returning toggle). Needs pattern search for writes to `*(byte*)(this+0xB4)`.

- Refresh mapping block from patch ctx into Practice (+4..+0x24)
  - 1.02e: RVA 0x0075100 (`sub_10075100`)
    - Code use: `EFZREV_RVA_REFRESH_MAPPING_BLOCK` in `switch_players.cpp`.
  - 1.02h: candidate 0x0074E80 (`sub_10074E80`) or 0x0074EF0 (`sub_10074EF0`)
    - Rationale: clustered around the Practice helpers; naming and call sites suggest close proximity.

- Patch toggler (apply/revert byte patches to freeze/unfreeze engine)
  - 1.02e: RVA 0x006B2A0 (`sub_1006B2A0(int* this, char enable)`)
    - Code use: `EFZREV_RVA_PATCH_TOGGLER` in pause integration and switching.
  - 1.02h: candidate 0x006BB00 (`sub_1006BB00(int* this, char enable)`)
    - Rationale: Clear `__thiscall` signature taking `this` and a `char` flag; nearby 6Bxx region shows a likely renumbering.

- Map reset helper (used during init/switch)
  - 1.02e: RVA 0x006D640 (`sub_1006D640(char** this)` returning bool)
    - Code use: `EFZREV_RVA_MAP_RESET` in `switch_players.cpp` (SehSafe wrapper call with `char**`).
  - 1.02h: candidate 0x006D5B0 (`sub_1006D5B0(char **this, const char *pExceptionObject, unsigned int a3)`)
    - Rationale: same calling convention on `char** this` and returns `bool`; extra args may be inlined defaults; neighboring renumbering aligns with +0xFFF0 cluster.

- Cleanup pair (post-switch input device/pair rebind)
  - 1.02e: RVA 0x006CAD0 (`sub_1006CAD0`)
    - Code use: `EFZREV_RVA_CLEANUP_PAIR` in `switch_players.cpp`.
  - 1.02h: candidate 0x006CE00 (`sub_1006CE00(void *a1)`)
    - Rationale: same neighborhood and role; invoked with patch ctx in our code.

- Render battle screen (capture battleContext)
  - 1.02e: used as `EFZ_RVA_RENDER_BATTLE_SCREEN 0x007642A0` in headers; not confirmed in the 1.02e decomp excerpt.
  - 1.02h: TBD
    - Next step: find `thiscall` render function that writes/reads the engine gamespeed byte at `battleContext + 0x1400` (we read/write that); signature scan around accesses to offset 0x1400 can find the owning struct function.

- Game mode pointer array base
  - 1.02e: RVA 0x790110
    - Evidence: out/docs/EfzRevival.dll_1.02e.c lines show reads like `*(int*)(4 * idx + 0x790110)`.
    - Code use: Fast-path Practice pointer resolution when EfzRevival is loaded.
  - 1.02h: TBD
    - Not visible in unrefactored decompile. Plan: signature search for table of 4-byte pointers indexed by mode id; fallback is already robust via tick hook.

---

## Practice controller (structure) offsets

Relative to the Practice controller `this` pointer (captured via tick/pause hooks):

- Pause/step core
  - +0xAC: Step flag (byte)
  - +0xB0: Step counter (dword)
  - +0xB4: Pause flag (byte)

- Speed scalar (not used directly, noted from prior RE)
  - +0xC0, +0xC4: double

- Config hotkey codes
  - +0x1D4: Pause key (dword)
  - +0x1D8: Step key (dword)

- Side switching and input routing
  - +0x680: Local side index (dword: 0=P1, 1=P2)
  - +0x684: Remote side index (dword)
  - +0x788: Buffer base when local=P1
  - +0x800: Buffer base when local=P2
  - +0x824: Primary side buffer pointer
  - +0x828: Secondary side buffer pointer
  - +0x944: Init source side (dword)
  - +0x1240: Shared input vector base

- GUI/buffer display position
  - +0x24: 1 when P1, 0 when P2 (EfzRevival writes via `sete` then `mov [esi+24], eax`).

Status for 1.02h: unknown but likely stable; will confirm by inspecting code that assigns GUI pos (+0x24) and the pause/step flags around tick/pause paths. If needed, we will pattern-match reads/writes to these offsets.

---

## Summary matrix

| Component | 1.02e (confirmed) | 1.02h (candidate) | Status |
|---|---|---|---|
| Online state flag | 0x00A05D0 | TBD | Needs signature/xref search |
| Patch ctx global | 0x00A0760 | TBD | Find via toggler caller |
| Patch toggler | 0x006B2A0 (sub_1006B2A0) | 0x006BB00 (sub_1006BB00) | Candidate – verify by side effects |
| Map reset | 0x006D640 (sub_1006D640) | 0x006D5B0 (sub_1006D5B0) | Candidate – char** thiscall, returns bool |
| Cleanup pair | 0x006CAD0 (sub_1006CAD0) | 0x006CE00 (sub_1006CE00) | Candidate – `__thiscall(void*)` |
| Refresh mapping block | 0x0075100 (sub_10075100) | 0x0074E80 or 0x0074EF0 | Candidate – proximity to tick/toggle |
| Practice tick | 0x0074F70 (sub_10074F70) | 0x0074F40 (sub_10074F40) | Candidate – verify by step/pause field refs |
| Toggle pause | 0x0075720 (sub_10075720) | TBD | Needs field write (+0xB4) pattern scan |
| Mode ptr array | 0x790110 | TBD | Prefer hook fallback |
| Render battle screen | 0x007642A0 | TBD | Find via gamespeed +0x1400 xref |

---

## Validation plan (1.02h)

1) Toggler and patch ctx
- Place a temporary hook at `sub_1006BB00` candidate and call it with (ctx?, 0)/(ctx?, 1) while paused in Practice; observe whether game freezes/unfreezes and whether exceptions occur.
- If the function expects `this` to be the patch ctx, scan references to `sub_1006BB00` to locate the global `patchCtx`: look for MOV ECX, [100A0xxx]; capture that address.

2) Practice tick and pause toggle
- For `sub_10074F40` candidate, inspect body to see:
  - Increment of a dword at `this + 0xB0` when stepping.
  - Checks/assignments on `this + 0xB4` for pause flag.
- For toggle, signature scan for functions that write to `*(byte*)(this+0xB4)` or flip it; short functions returning int/char.

3) Refresh mapping / map reset / cleanup pair
- Invoke each candidate with `SEH` wrappers as we do, log side-effects (no exceptions) and check that mapping block (+4..+0x24) updates and that inputs rebind correctly.

4) Online state flag
- Search code for a function that returns or switches game modes labeled Netplay/Spectate/Tournament; locate a static/global int with values in {0..3} written, then take its address.
- Alternatively, runtime scan the EfzRevival.dll .data segment for a 4-byte int that changes when switching Netplay/Spectate/Offline; debounce by verifying the 1.02e mapping on e build and repeating on h build.

---

## Notes

- The mod is version-gated; these 1.02h candidates are for research only until confirmed.
- We already avoid relying strictly on the mode array pointer by hooking PracticeTick.
- Where practical, we should replace fixed RVAs with small pattern scans keyed on the structure offsets we control (e.g., +0xB0/+0xB4) to survive minor layout shifts across minor releases.
