# EFZ Training Mode

Modern training mode for Eternal Fighter Zero with a live in-game overlay, frame analysis, robust auto-actions, macro playback, and online-safe behavior. Designed for players first, with practical tools and clear UI.

This project is a DLL mod that loads via EFZ Mod Manager. It adds a configurable ImGui overlay, accurate frame and RG analysis, input automation, macros, and quality-of-life training tools while automatically disabling risky features during online play to avoid desyncs.

---



**Installation**
- Build the DLL from source (see Building) or download a release.
- Place `efz_training_mode.dll` in your EFZ mods folder (where you put other EFZ Mod Manager DLLs).
- Edit `EfzModManager.ini` and append:
  - `efz_training_mode=1`
  - EFZ Mod Manager download: https://docs.google.com/spreadsheets/d/1r0nBAaQczj9K4RG5zAVV4uXperDeoSnXaqQBal2-8Us/edit?usp=sharing

**First Run**
- The overlay loads in-game; open the menu via the hotkey (default: `3`).
- A config file `efz_training_config.ini` appears next to the DLL.
- The console is OFF by default. Enable it in Settings → General. All logs since startup are buffered and will appear when you enable it.

---

**Quick Start**
- Press the menu hotkey to open the overlay and explore the Basics/Help sections.
- Try Auto-Actions: choose a trigger (e.g., After Block), then select an action (jump, macro, etc.).
- Record a macro: press Record, perform inputs, press Record again; then Play.
- Use the Teleport tools to position players quickly for testing.

---

**Features**

- **Auto-Actions:** Configurable triggers and actions that fire at precise training moments:
  - Triggers: After Block, After Hitstun, On Wakeup, After Airtech
  - Actions: Macro slot playback, auto-jump, buffered dashes, button/direction inputs
  - Designed for stability in practice mode and disabled automatically during online play

- **Auto-Airtech:** Automatic air recovery with direction (Forward/Neutral/Back) and optional delay; tuned for actionable states around FALLING.

- **Frame Analysis:** Shows frame advantage on block, hit, and RG, plus the gap between blocked hits.

- **Jumps and Dashes:**
  - Jumps use immediate input injection (direction respects facing)
  - Dashes use buffer-accurate double-tap motions

- **Macros:** Record and play inputs with engine-accurate timing:
  - Captures both immediate button presses and the real input buffer at 64 Hz
  - Slots: select and play any slot; overlays show the active slot
  - Triggers integration: auto-run a macro from Auto-Actions
  - Facing-aware: directions flip appropriately based on character facing
  - Frame-step aware: recording advances only when the game consumes a buffer frame

- **Position Tools:**
  - Record current positions and teleport players
  - Move both players to a side, center, or round-start positions
  - Quick swap of P1/P2

- **Player State Editor:**
  - Adjust HP, meter, RF, and positions from the overlay

- **Character-Specific Settings:**
  - Toggle or adjust unique character parameters directly from the overlay
  - Examples: Ikumi (Blood/Genocide), Misuzu (Feathers), Mishio (Element/Awakened), Rumi (Stance/Kimchi), Akiko (Bullet/Time-Slow), Neyuki (Jam 0–9), Kano (Magic), Mio (Stance), Doppel (Enlightened/Gold), Mai (Ghost/Awakening), Minagi (Michiru position + Always readied)

- **Random Defense Tools:**
  - Random Block and Random RG for realistic defensive variance
  - Adaptive Guard: automatically chooses Stand/Crouch/Air guard based on incoming attack properties and state. Doesn't walk backwards on high-block.
  - Guard overrides (force stand/crouch/air where applicable)

- **Overlay Controls:**
  - Toggle overlay visibility and stats
  - Optional debug console view; log buffering from startup

- **Settings UI:**
  - General: Use ImGui UI, detailed logging, restrict to Practice Mode, show/hide debug console
  - Hotkeys: press-to-bind directly in the UI; Save/Reload config

- **Online-Safe Behavior:**
  - Detects active online sessions and automatically:
    - Hides the console after a short countdown
    - Disables training hotkeys and injected inputs
  - Restores console/hotkeys after the match ends

---

**Online Play Safety**
- When connected to another player, training-only features are disabled to avoid desyncs.
- Console auto-hides; hotkeys are suppressed; injected inputs are blocked.
---

**Default Hotkeys**

- `1`: Teleport players to recorded/default positions
  - With Left/Right: move both to that side
  - With Up: swap P1 and P2
  - With Down: place at center; with Down+Z: round-start positions
- `2`: Record current player positions
- `3`: Open config menu (ImGui overlay)
- `4`: Toggle stats
- `7`: Toggle overlay visibility

Macros (configurable):
- `I`: Record
- `O`: Play
- `K`: Next Slot

All hotkeys are configurable in Settings → Hotkeys or in `efz_training_config.ini`.

---

**Configuration**
- Config file is created next to `efz_training_mode.dll` as `efz_training_config.ini`.
- Settings → General:
  - Use ImGui UI, detailed logging, restrict to Practice mode, show debug console (off by default; logs are buffered)
- Settings → Hotkeys:
  - Click Bind to reassign; or edit INI (virtual-key codes, hex like `0x31`)
  - Save to disk / Reload from disk from within the UI

---

**Troubleshooting**
- Console shows nothing when enabled
  - Ensure Settings → General → Show debug console is ON; logs are buffered and should flush immediately
- Hotkeys don’t respond
  - Verify online mode isn’t active (features are disabled during online play)
  - Rebind hotkeys in Settings → Hotkeys and try again
- Overlay not visible
  - Press the Toggle Overlay hotkey (default `7`) or ensure ImGui UI is enabled in Settings → General
- Performance issues on lower-end systems
  - Disabling the EfzRevival GUI (default key Enter) can improve FPS if for some reason there're issues with it.

---

**Building from Source**

Using CMake (cross-platform):
```bash
# Clone the repository
git clone https://github.com/Aquat1c/efz-training-mode.git

# Create build directory
cd efz-training-mode
mkdir build && cd build

# Generate build files with CMake
cmake ..

# Build with your platform's build system
cmake --build . --config Release
```

Windows/VS Code tip:
- The workspace provides a `build-dll` task that builds and outputs `efz_training_mode.dll` to `build/bin/<Config>/`.

External libraries:
- MinHook (function hooking)
- Dear ImGui (UI)
- Microsoft Detours (Win32 API interception)
- DirectX (D3D9 overlay rendering)

---

**Compatibility**
- The mod fully tested on:
  - Vanilla EFZ (no Revival)
  - Eternal Fighter Zero -Revival- 1.02e
  - Eternal Fighter Zero -Revival- 1.02h
  - Eternal Fighter Zero -Revival- 1.02i
- On other versions, the mod should work fine as well but some features might not work properly.

---

**Contributing**
- Contributions are welcome! Areas of interest include:
  - More precise RG/frame calculations
  - Improved defender state detection
  - Additional training quality-of-life features

Please open issues/PRs with clear descriptions and steps to reproduce where relevant.

---

**Special Thanks**
- fishshapedfish — early tables and references used to bootstrap character state work
- Ev.Geniy, yagelbagel, Micro and others — testing and feedback

---

**License**
- This project is provided for educational purposes. Eternal Fighter Zero is property of Twilight Frontier and its respective owners.

---