# EFZ Training Mode – Project Analysis (2025-08-25)

This document maps the architecture, flags risks/bugs, and lists a prioritized optimization backlog. It’s meant as a living reference while we fix issues and tune performance.

## Overview

- Output: 32-bit Windows DLL (C++17) injected into EFZ, built with CMake/MSBuild.
- Hooking/libs: MinHook (D3D9 EndScene), Microsoft Detours (legacy DirectDraw), ImGui (DX9 + Win32 backend).
- Key subsystems:
  - core: memory, globals, logger
  - game: state detection, frame advantage/analysis, auto actions/airtech, collision hook
  - input: hooks, handler, buffer/motion, debug, freeze
  - gui: overlay (D3D9), ImGui UI, settings, gif player
  - utils: config INI, window/console helpers, network/bgm

## Build & runtime notes

- CMake forces Win32 and static runtime (/MT). DLL builds cleanly (Debug) on current setup.
- MinHook is initialized in DllMain; overlay also sets up EndScene hook at runtime.
- Several background threads: delayed init, title updater, frame monitor, online status, screen state monitor, RF freeze.

## Architecture map (high level)

- Entry: `src/dllmain.cpp` launches DelayedInitialization →
  - Config init → console creation (optional)
  - Hook install: input, collision, BGM suppression poller
  - Start threads: title, frame data, online status, screen state
  - D3D9 hook bootstrap (`DirectDrawHook::InitializeD3D9`) on background thread
  - Feature enable gates via `g_featuresEnabled`
- Overlay/UI:
  - D3D9 EndScene hook drives ImGui + overlay text.
  - Legacy DirectDraw detours remain stubbed; D3D9 path is active.
- Game state:
  - `game_state.*` derives GameMode (raw byte) and `GamePhase` using spawn heuristics.
  - Heavily used by UI/overlays and auto-actions.
- Input:
  - Windows input path (DirectInput disabled). Input buffer inspection/manipulation for freeze/motions.
- Config/logging:
  - INI parsed into `Config::Settings` with verbose startup logging.
  - Console logs buffered until console window is ready.

## Hotspots and likely issues

1) RF value type inconsistency
- `UpdatePlayerValues` writes RF as float (4 bytes). `SetRFValuesDirect` and RF freeze treat as double (8 bytes).
- Risk: corruption or inconsistent behavior depending on call path.
- Action: centralize RF accessors with one definitive type (verify with CE/structs). Prefer a single helper: ReadRF/WriteRF.

2) Global state definition sprawl
- `g_featuresEnabled` and `g_initialized` are declared in multiple headers; `g_featuresEnabled` is defined in `dllmain.cpp`, while `globals.cpp` defines only `g_isShuttingDown`.
- Risk: accidental ODR violations when refactoring; tight coupling to `dllmain.cpp`.
- Action: define all globals in `src/core/globals.cpp` and declare in `include/core/globals.h` only. Remove duplicate externs from other headers; keep a single include site.

3) Deprecated pointer validation APIs
- Use of `IsBadReadPtr/IsBadWritePtr` across memory helpers.
- Risk: API is unreliable and discouraged; can cause TOCTOU and false results.
- Action: replace with `__try/__except` guarded reads/writes or `VirtualQuery`-based region checks. Wrap in SafeRead/SafeWrite helpers.

4) Dual overlay hook mechanisms
- Detours for DirectDraw are still initialized (stubs) while D3D9 path is the active renderer.
- Risk: unnecessary detour install, maintenance overhead, potential conflicts.
- Action: remove DirectDraw hooking code paths (or compile-time disable) to simplify.

5) Thread lifecycle/shutdown
- Many detached threads (title, monitors, RF freeze) rely on atomics for exit, but not all check `g_isShuttingDown` consistently. Some use infinite loops without timely exit conditions.
- Risk: dangling work at detach, noisy logs during teardown.
- Action: standardize a `ShouldRun()` check using `g_isShuttingDown` for all background loops; add short joins or graceful timeouts at DLL detach.

6) Logging noise/perf
- Console logging is frequent; some categories bypass `detailedLogging`. Overlay render path conditionally logs as well.
- Risk: runtime overhead and readability issues.
- Action: ensure all non-critical logs respect `detailedLogging`. Consider per-category toggles, rate-limiters for high-frequency logs.

7) Window/ImGui state coupling
- Multiple checks for window focus and UI visibility; cursor drawing and overlays depend on several flags.
- Risk: redundant work and edge cases while minimized/inactive.
- Action: centralize focus/visibility decisions and early-outs in one place (utilities/overlay), and reuse.

8) CMake configuration
- Forces `CMAKE_GENERATOR_PLATFORM Win32` in script; this is typically passed by user/generator.
- Risk: makes cross-configure/CI brittle.
- Action: remove forcing inside CMake; document generator platform in build docs.

9) Unused or misleading declarations
- `extern std::ofstream g_log;` declared but no definition/usage.
- Risk: confusion; potential link issues if referenced later.
- Action: remove or implement consistently.

10) Include hygiene and duplication
- Some headers declare the same externs as others (e.g., `g_featuresEnabled`), and there are repeated includes.
- Action: reduce surface area of shared headers; prefer forward decls; use one “globals” header for atomics.

## Performance opportunities

- ImGui rendering:
  - Already reduces AA/rounding when menu visible; keep the minimal style for DX9.
  - Cap overlay messages when menu open (in place). Consider batching text draws further.
- Render target filtering:
  - EndScene path now checks RT size (640x480) and skips otherwise; good. Ensure any future draws follow same gate.
- Logging:
  - Guard all hot-path logs under `detailedLogging` and consider a simple ring buffer for startup-only logs.
- Memory ops:
  - Cache resolved pointers aggressively; already done for console title. Consider similar caching for frequent reads.
- Thread loops:
  - Use consistent sleep cadences; avoid too-small sleeps when not required. Prefer aligning to internal frames where possible.

## Security/robustness

- Replace `IsBad*Ptr` usage (see above).
- Guard all overlay device calls with null/size checks (mostly done).
- Ensure MinHook init/uninit balance (current: MH_Initialize in DllMain; Shutdown removes specific hooks and Uninitialize at detach – OK).

## Quick-win fixes (low risk)

- Centralize RF access type (helper functions) without changing behavior; add TODO to verify layout.
- Move `g_featuresEnabled` and `g_initialized` definitions to `globals.cpp`; keep externs in one header; update includes.
- Wrap `SafeReadMemory/SafeWriteMemory` with a `VirtualQuery`-based probe; deprecate `IsBad*Ptr` usage behind a single shim.
- Remove DirectDraw detour installation (keep code under `#if LEGACY_DDRAW` guard) to avoid extra detours.
- Make all non-essential LogOut calls respect `detailedLogging`.

## Prioritized backlog

P0 – Correctness
- [ ] RF type consistency: single accessor; audit all uses (UpdatePlayerValues, SetRFValuesDirect, RF freeze).
- [ ] Thread exit condition: add `g_isShuttingDown` checks to all loops (title/monitors) and ensure clean exit.
- [ ] Replace `IsBadReadPtr/IsBadWritePtr` with safer validation in memory helpers.

P1 – Stability/Code health
- [ ] Consolidate globals in `globals.cpp` and `globals.h`; remove duplicate externs from other headers.
- [ ] Remove/disable legacy DirectDraw detours; keep D3D9-only overlay path.
- [ ] Trim unused externs (`g_log`) or implement properly.
- [ ] Reduce header coupling; ensure only `globals.h` exposes atomics.

P2 – Performance/Ergonomics
- [ ] Normalize logging to respect `detailedLogging`; add rate limits where needed.
- [ ] Broaden pointer caching for frequently read addresses.
- [ ] CMake: stop forcing Win32 in script; document in README.

## Suggested “contracts” for risky areas

- RF Accessors
  - Inputs: base addr, player (1/2), value (double/float TBD)
  - Output: success bool
  - Error modes: invalid base/pointer; protection change failure
  - Success criteria: write/read-back matches within epsilon

- SafeRead/Write
  - Inputs: addr, buffer, size
  - Behavior: probe with VirtualQuery; try/except read/write; never throw
  - Log: only on failure with `detailedLogging`

## Edge cases to watch

- Minimized window: ImGui asserts if DisplaySize is zero (guard present; keep).
- Game mode transitions: ensure overlays/triggers clear at Character Select; current code clears on phase changes.
- Replay/auto-replay modes: features should be disabled if config restricts to Practice.

## Current quality gates (local)

- Build: PASS (Debug, Windows, x86)
- Lint/Typecheck: N/A
- Unit tests: N/A
- Smoke: Not executed here (requires EFZ runtime)

---
This file will be updated as fixes land. Add notes inline when verifying RF layout and after removing legacy detours.
