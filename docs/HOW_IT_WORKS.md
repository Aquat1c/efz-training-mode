# How EfzExtendedPractice works

A tour of what the mod does inside the game, for curious users and modders.
Symbols are named as they appear in the source, with the file that owns them.

## The short version

The mod is a 32-bit DLL that EFZ Mod Manager loads into the game process.
At startup it identifies the exact game build (vanilla Memorial efz.exe or one
of the EfzRevival wrapper generations), then installs a small set of hooks:
one on the place where the game reads player input, one on the frame loop,
one on the final draw call, and a handful of byte patches that are tracked so
they can be undone. Everything else - the training menu, frame data, macros,
missions - is ordinary C++ code running off those hooks. While an online
session is live, every feature that could desync the match is switched off.

## Repository map

| Path | What lives there |
| --- | --- |
| `src/dllmain.cpp` | Entry point: `DllMain`, `DelayedInitialization` |
| `src/core/` | Memory access (`memory.cpp`), logging (`logger.cpp`), global state (`globals.cpp`), key codes |
| `src/game/` | Training features: auto-actions, macros, savestates, frame data, dummy control, character quirks |
| `src/game/mission/` | Mission/tutorial engine, data model, rendering, tutorial sessions |
| `src/gui/` | ImGui menu, overlay rendering, frame bar, custom menu |
| `src/gui/custom_menu/` | The hand-drawn Revival-look menu (fonts, layout, screens) |
| `src/input/` | Input hooks, input buffer control, motion patterns, frame stepping |
| `src/runtime/` | Hook registry, patch ledger, practice-mode isolation runtime |
| `src/utils/` | Config, audio, netplay detection, update check, controllers, misc |
| `include/` | Headers mirroring the above; `include/compat/` holds cross-mod ABI |
| `tests/` | Plain C++ policy tests run under CTest |
| `assets/` | Shipped data: fonts, control icons, movedata tables, mission packs |
| `3rdparty/` | Bundled MinHook, Detours, Dear ImGui, nlohmann/json |
| `tools/` | `build-task.ps1`, the build wrapper used by the VS Code tasks |

## Startup

`DllMain` (`src/dllmain.cpp`) installs the crash handler and spawns
`DelayedInitialization` on its own thread; the detach path is signal-only
because it runs under the loader lock. `DelayedInitialization` sets up
logging and config, initializes MinHook, installs the feature hooks in
stages, and restores the Final Memory and timer-freeze patch baselines.

Nothing is hooked until the game build is known.
`GetQualifiedMemorialImage` (`src/runtime/native_game_profile.cpp`) admits
the executable only on an exact PE-identity and hash match, and
`src/utils/network.cpp` detects which EfzRevival version (1.02e through
1.02j, including the two 1.02j build profiles in
`include/utils/efz_revival_profile.h`) is hosting the process. Per-version
addresses come from the accessors in `src/game/efzrevival_addrs.cpp`.

## How the game is modified

Two mechanisms, both reversible:

- **Function hooks.** MinHook detours on native game functions, managed by
  `PracticeHooks::Registry` (`src/runtime/hook_registry.cpp`) with preimage
  verification, admission control, and retirement receipts so a hook can be
  taken down cleanly while callbacks are in flight.
- **Byte patches.** `PatchLedger` (`src/runtime/patch_ledger.cpp`) records
  every code byte the mod changes, with page protection handled and an
  owner on each patch. The Final Memory HP bypass
  (`src/game/final_memory_patch.cpp`) and the timer freezes
  (`src/game/timer_freeze_patch.cpp`) work this way; both are applied only
  inside practice contexts and restored afterward.

## The input pipeline

`src/input/input_hook.cpp` detours the game's character-input, poll, and
entity-update functions. On top of that raw access sit three injection
paths:

- **Immediate writes.** `ImmediateInput`
  (`src/input/immediate_input.cpp`) writes direction/button bytes straight
  into the player struct for same-frame effects (jumps, one-shot inputs),
  with strict ownership rules: a write lease is held for the whole
  transaction, delayed values are never delivered by an out-of-date
  worker, and everything is refused while netplay owns the fighters.
- **Buffer freezes.** `src/input/input_freeze.cpp` builds a motion pattern
  from numpad notation (QCF, DP, dashes, ...) and
  `StartBufferFreezeWorker` (`src/input/input_buffer.cpp`) rewrites the
  game's real 180-entry input ring each tick until the move comes out.
  This is what makes buffered specials and dash timing engine-accurate.
- **Motion transactions.** A producer/consumer pair in `input_hook.cpp`
  that queues a notation, feeds it through the buffer on consecutive
  frames, and confirms execution by move ID - the path auto-actions and
  Final Memory commands use.

Frame stepping (`src/input/framestep.cpp`) pauses the engine through the
gamespeed byte and steps it a visual frame (or a subframe) at a time.

## Auto-actions and macros

Auto-actions (`src/game/auto_action.cpp`) arm on triggers - after block,
after hitstun, on wakeup, after airtech, on RG - and fire an action through
the motion-transaction path, a macro slot, or an immediate jump. Delay
states and scoped input reservations keep two features from fighting over
the same player. The IC/FIC follow-up watcher
(`src/game/auto_action_charge.cpp`) is a small state machine that watches a
source attack land and then submits the 22C charge motion in the exact
window the engine accepts.

Macros (`src/game/macro_controller.cpp`) record both the raw button state
and the game's input buffer at 64 Hz. Playback
(`PrepareNextStreamTick` / `PublishPreparedStreamFrame`) republishes the
buffer one consumed frame at a time, so replays are frame-step aware and
facing-aware (directions flip with the character).

## Frame data

`src/game/frame_monitor.cpp` runs the analysis loop at the game's internal
tick rate (192 steps per second, three per visual frame): pointer caches,
phase transitions, RG analysis, continuous recovery, and the paginated
stats overlay. Frame advantage
(`src/game/frame_advantage.cpp`) compares when the attacker recovers
against when the defender becomes actionable and publishes the +/- numbers
and block-gap strings the overlay shows; `GetCurrentInternalFrame` is the
pause-aware clock everything else uses. The frame bar
(`src/gui/framebar.cpp`) mines both player structs into a per-frame
activity strip.

## Practice control, savestates, character tools

- `src/game/practice_patch.cpp` enables P2 in practice mode and drives the
  dummy: auto-block, adaptive guard stance, random block
  (`random_block.cpp`) and random RG (`random_rg.cpp`).
- Savestates ride on EfzRevival's own snapshot system:
  `src/game/savestate_hook.cpp` hooks its save/load entry points and
  carries the mod's own state (side, macros, enforcements) across a
  rollback; `src/game/custom_savestate.cpp` captures and restores full
  match state with pointer rehydration.
- Character hotswap (`src/game/character_hotswap.cpp`) reloads the match
  with different characters by driving the game's own character-select
  frontend and bootstrapping practice fighters inside the loading screen.
- Per-character quirks (Ikumi blood, Misuzu feathers, Doppel tech,
  Sayuri's remembered move, Neyuki jams, ...) live in
  `src/game/character_settings.cpp` and a few dedicated files, re-applied
  every tick by `TickCharacterEnforcements` so the game's own resets
  cannot undo them.

## Menus and rendering

The overlay owns the final draw call: `src/gui/overlay.cpp` hooks
EndScene, validates the device every frame, and runs the registered
external renderers, the ImGui menu, and the mod's own message layer in a
fixed order over the game's 640x480 surface, remapped into whatever
letterboxed area the real render target uses. The ImGui backend
(`src/gui/imgui_impl.cpp`) owns font atlases, gamepad/keyboard navigation
with per-direction repeat gates, and the fallback window used when the
game's own device is unavailable.

There are two menu systems:

- The **ImGui menu** (`src/gui/imgui_gui.cpp`) - tabs for values,
  auto-actions, macros, characters, settings, and help.
- The **custom menu** (`src/gui/custom_menu/`) - a hand-drawn menu in the
  Revival style with its own embedded font atlas, layout engine
  (`layout.cpp`, `renderer.cpp`), and screen definitions
  (`screens.cpp`, `screens_defs.cpp`). It also powers the title-screen
  MISSIONS/TUTORIAL browser (`src/game/practice_menu/`).

Messages for the overlay queue through `AddMessage` /
`AddPermanentMessage` in `overlay.cpp`, which is what auto-actions,
frame advantage, and the update check all use to talk to the player.

## Missions and tutorials

Missions are JSON packs under `assets/missions/`. The data model and
serializer (with migrations for older pack formats) are
`src/game/mission/mission_data.cpp`; authoring/publish transactions are
`mission_authoring.cpp`; the recorder (which captures a real replay plus
an entity trace) and the strict runner are `mission_engine.cpp`
(`RecorderCapture`, `TickRun`). Tutorial sessions
(`src/game/mission/tutorial_session.cpp`) are lesson state machines that
grade tasks - sequences, contacts, goal states - against the same engine
snapshot, with their own pause menu, pages, and dummy control.

## Online safety and cross-mod cooperation

- `include/utils/efz_netplay_state.h` reads the shared-memory block that
  InGameNetplay publishes. The moment an online session is live, injected
  inputs, training hotkeys, and risky patches are refused - most of that
  policy is enforced centrally in `src/utils/utilities.cpp` so no feature
  can forget it.
- The audio control plane (`include/utils/audio_owner_abi.h`) arbitrates
  volume ownership between this mod and ExtendedConfig through
  `EfzAudioOwnerApiV1`; the bridge is `src/utils/extended_config_bridge.cpp`.
- The overlay reset contract
  ([displaymanager-reset-contract.md](displaymanager-reset-contract.md))
  lets EfzDisplayManager reset the Direct3D device without destroying this
  mod's ImGui backend; the export is `EFZ_QueryOverlayResetApiV1`
  (`include/compat/overlay_reset_api.h`), and other mods can draw through
  this overlay via `EFZ_RegisterOverlayRenderer` (`src/gui/overlay_api.cpp`).

## Update check

`src/utils/update_check.cpp` asks the GitHub releases API once per session,
on a detached worker, using only a `User-Agent` of
`EfzExtendedPractice/<version>`. Tag comparison logic lives in
`src/utils/version_compare.cpp`, and the tests
(`tests/update_check_policy_tests.cpp`, `tests/version_compare_tests.cpp`)
pin the behavior, including which tags are safe to suggest.

## Where would I change this?

| I want to... | Start here |
| --- | --- |
| Add a menu screen or row | `src/gui/custom_menu/screens_defs.cpp` (row descriptors), `screens.cpp` (behavior); for the ImGui menu, `src/gui/imgui_gui.cpp` |
| Add a setting | `include/utils/config.h` + `src/utils/config.cpp` (INI schema), then the relevant screen row |
| Support a new game/Revival build | `include/utils/efz_revival_profile.h` (PE profiles), `src/game/efzrevival_addrs.cpp` (per-version addresses) |
| Add a character quirk | `src/game/character_settings.cpp` (+ a dedicated file if it grows) |
| Add an auto-action action type | `src/game/auto_action_helpers.cpp` (action-to-motion mapping), `auto_action.cpp` (arming) |
| Add a new export for other mods | `include/compat/` for the ABI header; keep it append-only and versioned |
| Change what is disabled online | The central gates in `src/utils/utilities.cpp` and `src/game/game_state.cpp` |

## Building and testing

Win32 (x86) with CMake and Visual Studio (2022 or the 2026 presets):

```bat
cmake --preset vs18-win32
cmake --build --preset vs18-release
ctest --test-dir build -C Release --output-on-failure
```

The DLL is written to `build\bin\Release\efz_training_mode.dll`.
`tools/build-task.ps1` wraps the same presets (including the
`v141_xp` XP-compatible build via `-Target xp`) and is what the VS Code
build tasks call.

The tests are plain C++ programs with no game dependency: policy suites for
missions, input, hotswap, updates and versions, plus gate tests for the
overlay reset contract. They compile and run anywhere the toolchain does.
