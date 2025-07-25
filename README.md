# EFZ Training Mode

A comprehensive training mode enhancement tool for Eternal Fighter Zero that provides frame data analysis, Recoil Guard (RG) timing information, and other essential training features.

## Features

- **Real-time Frame Data Analysis**: Displays frame advantage information for blocked moves and RG situations
- **Detailed RG Analysis**: Shows attack windows and timing differences between Stand, Crouch, and Air RG
- **Position Manipulation**: Easily record and teleport player positions for setup practice
- **Player Status Modification**: Adjust health, meter, RF values, and positions through an in-game menu
- **Move ID Tracking**: Monitor character state transitions with detailed move ID logging
- **Visual Frame Counter**: Track game frames with accurate internal and visual frame displays
- **Smart Online Mode Detection**: Automatically detects online play and temporarily disables training features to prevent desyncs
- **ImGui Overlay Interface**: Modern in-game overlay with configuration options and real-time data display
- **Auto-Tech Options**: Configure automatic air-teching with directional control
- **Auto-Action System**: Set up automatic responses after blocking, hitstun, or on wakeup
- **Auto-Jump Configuration**: Customize automatic jump patterns with directional options

## Installation

1. Build the DLL using Visual Studio or CMake
2. Place the generated `efz_training_mode.dll` in your EFZ mods directory.  
3. Make sure to add `efz_training_mode=1` AT THE BOTTOM of the "EfzModManager.ini" file. You can download and install the EFZ Mod Manager here - [link](https://docs.google.com/spreadsheets/d/1r0nBAaQczj9K4RG5zAVV4uXperDeoSnXaqQBal2-8Us/edit?usp=sharing)
4. A console window will appear with frame data information.

## Online Play Support

The training mode now features automatic network detection:

- When a connection to another player is detected, the tool will:
  - Temporarily hide the console window (after a 3-second countdown)
  - Disable all training mode hotkeys
  - Prevent any accidental inputs that could cause desynchronization
- When the online match ends, the console and hotkeys will be automatically closed.
- This ensures a safe experience when switching between training and online play without needing to disable the mod

## Controls

The tool provides several hotkeys to control its functionality (automatically disabled during online play):

- **1**: Teleport players to recorded/default positions
  - With **Left Arrow**: Move both players to left side
  - With **Right Arrow**: Move both players to right side
  - With **Up Arrow**: Swap P1 and P2 positions
  - With **Down Arrow**: Place players close together at center
  - With **Down Arrow + Z**: Place players at round start positions
- **2**: Record current player positions
- **3**: Open configuration menu: enable airtechs, jumps and change the values like positions, HP, RF and Eternity meter.
- **4**: Toggle title display mode between detailed and standard
- **5**: Reset frame counter
- **6**: Show help information and clear console
- **7**: Toggle ImGui overlay interface

## Frame Data Monitoring

When active, the tool will automatically detect and display:

- Frame advantage on block
- RG frame advantage including:
  - RG type (Stand/Crouch/Air)
  - Immediate attack window after RG freeze
  - Frame advantage if RG stun isn't canceled
- Frame gaps between defensive states

### RG Frame Mechanics

The tool tries to account for EFZ's specific RG system mechanics:
- **Stand RG**: -0.33F disadvantage to defender (can attack after 20F freeze)
- **Crouch RG**: -2.33F disadvantage to defender (can attack after 22F freeze)
- **Air RG**: -2.00F disadvantage to defender (can attack after 22F freeze)

## Automated Training Features

The tool includes advanced training features accessible through the in-game overlay:

- **Auto-Airtech**: Configure automatic recovery from air hitstun with directional control
- **Auto-Jump**: Set up automatic jumping patterns with customizable timing and directions
- **Auto-Action System**:
  - After Block: Automatically perform actions after blocking an attack
  - After Hitstun: Execute specific moves when recovering from hitstun
  - On Wakeup: Set automatic wakeup actions after knockdown

## Known Issues

1. **RG Frame Advantage Calculations**: The frame advantage displayed for some moves may be slightly off due to complexities in EFZ's RG system.

2. **Move Cancellation Detection**: The tool doesn't perfectly account for all cancel options when calculating potential advantages.

3. **Defender Actionable Frame Detection**: In some cases, the precise frame when a defender becomes actionable may be missed.

4. **MoveID Tracking Limitations**: Some rare character-specific states may not be correctly identified.

## Technical Implementation

The project is built in C++ with CMake support and includes:

- **Memory Management**: Read/write EFZ game memory for state tracking and modifications
- **Frame Monitoring**: Track and analyze frame data in real-time
- **Console UI**: Display frame data and debugging information
- **ImGui Integration**: Modern in-game overlay interface with real-time configuration
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
git clone https://github.com/yourusername/efz-training-mode.git

# Create build directory
cd efz-training-mode
mkdir build && cd build

# Generate build files with CMake
cmake ..

# Build with your platform's build system
cmake --build . --config Release
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


## License
This project is provided for educational purposes. Eternal Fighter Zero is property of Twilight Frontier and its respective owners.
