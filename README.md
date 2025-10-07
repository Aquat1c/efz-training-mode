# EFZ Training Mode

A comprehensive training mode enhancement tool for Eternal Fighter Zero. It provides frame data analysis, RG (Recoil Guard) timing, robust auto-actions, and a modern in-game ImGui overlay with live configuration.

## Features

- Real-time frame data: block advantage, RG advantage, gap timing
- Detailed RG analysis: Stand/Crouch/Air windows and outcomes
- Position tools: record and teleport with directional modifiers
- Player status editor: HP, meter, RF, positions from the overlay
- Move ID tracking and a visual frame counter
- Smart online mode detection (auto-safe disable to prevent desyncs)
- ImGui overlay UI with a dedicated Settings tab
- Auto-tech options with direction and delay
- Auto-action system: After Block, After Hitstun, On Wakeup, After Airtech
- Auto-jump and dashes: jump via immediate input; dashes via buffer motions
- New: Live hotkey rebinding in UI (press-to-bind) and on-disk config save/reload
- New: Toggle the debug console from the UI (off by default); logs are buffered from startup so enabling later shows full history

## Installation

1) Build the DLL or download a release
- Build with CMake or Visual Studio (see Building below)

2) Install the DLL
- Place `efz_training_mode.dll` in your EFZ mods folder (same place you put other EFZ Mod Manager DLLs)
- Add this line to the bottom of `EfzModManager.ini`:
  - `efz_training_mode=1`
  - EFZ Mod Manager download: [link](https://docs.google.com/spreadsheets/d/1r0nBAaQczj9K4RG5zAVV4uXperDeoSnXaqQBal2-8Us/edit?usp=sharing)

3) First run
- The overlay loads in-game; open the menu via the hotkey (default: 7)
- A config file `efz_training_config.ini` is created next to the DLL
- Console is OFF by default (you can enable it in Settings → General). All logs since startup are buffered and will appear when you enable it.

## Online Play Support

The training mode now features automatic network detection:

- When a connection to another player is detected, the tool will:
  - Temporarily hide the console window (after a short countdown)
  - Disable training-mode hotkeys
  - Prevent injected inputs that could cause desyncs
- When the online match ends, the console/hotkeys restore automatically

## Controls (default hotkeys)

These can be changed in Settings → Hotkeys (press-to-bind) or in the INI; they’re automatically disabled during online play.

- 1: Teleport players to recorded/default positions
  - With Left/Right: move both to that side
  - With Up: swap P1 and P2
  - With Down: place at center; with Down+Z: round-start positions
- 2: Record current player positions
- 3: Open config menu (overlay)
- 4: Toggle detailed vs standard console title
- 5: Reset frame counter
- 6: Show help and clear console
- 7: Toggle ImGui overlay

## Frame Data Monitoring

When active, the tool will automatically detect and display:

- Frame advantage on block
- RG frame advantage including:
  - RG type (Stand/Crouch/Air)
  - Immediate attack window after RG freeze
  - Frame advantage if RG stun isn't canceled
- Frame gaps between defensive states

## Automated Training Features

The tool includes advanced training features accessible through the in-game overlay:

- Auto-Airtech: automatic recovery using immediate inputs, with forward/back direction and optional delay; improved detection around FALLING/actionable states
- Auto-Action system:
  - After Block: perform actions after blocking
  - After Hitstun: act on recovery from hitstun
  - On Wakeup: automatic wakeup actions after knockdown
  - After Airtech: act right after airtech becomes actionable
- Jumps/Dashes:
  - Jumps use immediate input injection with direction (Forward/Neutral/Back) respecting facing
  - Dashes are queued to the input buffer as double-tap motions

## Macros

Record and play back inputs with engine-accurate timing. Macros capture both immediate button presses and the real in-game input buffer (for directions and motions).

- Slots: Macros live in numbered slots. Use the hotkey to switch slots (default: K). The current slot is also shown in the Macros tab in the overlay.
- Record flow: Press the Record hotkey (default: I) once to arm; press again to start recording; press again to stop. Recording is sampled at 64 Hz and stores both buttons and the buffer state.
- Play: Press the Play hotkey (default: O) to play the current slot. While a macro is playing, you continue controlling P1 and the macro controls P2. Directions automatically flip based on P2 facing.
- Triggers integration: In the Auto Action tab, choose "Macro" in the first Action combo and then select the Slot. This runs the macro whenever that trigger fires (After Block, On Wakeup, etc.). The overlay will show "Macro Slot #X" when active.
- Frame-step aware: If you pause Practice and frame-step, recording only advances when the game actually consumes a new input buffer frame so you don’t end up with long held directions.

Default macro hotkeys (configurable in Settings → Hotkeys or in the INI):

- Record: I
- Play: O
- Next Slot: K

## Known Issues

1. **RG Frame Advantage Calculations**: The frame advantage displayed for some moves may be slightly off due to complexities in EFZ's RG system.

2. **Move Cancellation Detection**: The tool doesn't perfectly account for all cancel options when calculating potential advantages.

3. **Defender Actionable Frame Detection**: In some cases, the precise frame when a defender becomes actionable may be missed.

4. **MoveID Tracking Limitations**: Some rare character-specific states may not be correctly identified.

## Settings UI and Configuration

- Open the overlay and switch to the Settings tab
- General:
  - Use ImGui UI (vs legacy dialog)
  - Detailed logging (affects console verbosity)
  - Restrict to Practice Mode
  - Show debug console (off by default). Logs are buffered from startup so enabling later shows full history.
- Hotkeys:
  - Click “Bind” next to a hotkey and press a key to rebind instantly
  - Or edit `efz_training_config.ini` (virtual-key codes, hex like 0x31)
  - Save to disk / Reload from disk buttons included

Config file location: created next to `efz_training_mode.dll` as `efz_training_config.ini`.

## Technical Implementation

The project is built in C++ with CMake support and includes:

- **Memory Management**: Read/write EFZ game memory for state tracking and modifications
- **Frame Monitoring**: Track and analyze frame data in real-time
- **Console**: Optional console window for debugging; can be toggled at runtime; logs buffered until enabled
- **ImGui Integration**: Modern in-game overlay with live configuration
- **DirectX Hooking**: Reliable D3D9 hooking for graphical overlays
- **Network Detection**: Monitor TCP/UDP connections to detect online matches and prevent desyncs

## External Libraries

This project relies on several external libraries:

- [**MinHook**](https://github.com/TsudaKageyu/minhook): Function hooking library for Windows
- [**Dear ImGui**](https://github.com/ocornut/imgui): Immediate-mode graphical user interface library
- [**Microsoft Detours**](https://github.com/microsoft/Detours): API hooking library for intercepting Win32 functions
- **DirectX SDK**: Used for rendering the overlay interface with D3D9

## Building from Source

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

Tip (Windows/VS): building the “ALL_BUILD” or project target produces `efz_training_mode.dll` in `build/bin/<Config>/`.
```

## Contributing
Contributions are welcome! Major areas that need improvement:

1. More accurate RG frame advantage calculations
2. Better state detection for defender actionable frames
3. Additional training mode quality-of-life features


## Special Thanks

Special thanks goes to:
- **fishshapedfish** - Initial CheatEngine tables for the character states and other things.
- **Ev.Geniy**, **kolya_kaban**, **lazerock** and some other people I probably forgot - Testing and feedback.


## Troubleshooting

- The console doesn’t show anything when I enable it
  - Ensure Settings → General → “Show debug console” is on; logs before enabling are buffered and should flush immediately
- Hotkeys don’t respond
  - Verify online mode isn’t active (features are disabled during online play)
  - Check Settings → Hotkeys; rebind using “Bind” and try again
- Overlay not visible
  - Press the Toggle ImGui hotkey (default 7) or check that ImGui UI is enabled in Settings → General

## License
This project is provided for educational purposes. Eternal Fighter Zero is property of Twilight Frontier and its respective owners.
