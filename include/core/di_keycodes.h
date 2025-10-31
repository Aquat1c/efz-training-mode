#pragma once
#include <windows.h>
#include <string>

/****************************************************************************
 *
 *      DirectInput keyboard scan codes
 *
 ****************************************************************************/
#define DIK_ESCAPE          0x01
#define DIK_1               0x02
#define DIK_2               0x03
#define DIK_3               0x04
#define DIK_4               0x05
#define DIK_5               0x06
#define DIK_6               0x07
#define DIK_7               0x08
#define DIK_8               0x09
#define DIK_9               0x0A
#define DIK_0               0x0B
#define DIK_MINUS           0x0C    /* - on main keyboard */
#define DIK_EQUALS          0x0D
#define DIK_BACK            0x0E    /* backspace */
#define DIK_TAB             0x0F
#define DIK_Q               0x10
#define DIK_W               0x11
#define DIK_E               0x12
#define DIK_R               0x13
#define DIK_T               0x14
#define DIK_Y               0x15
#define DIK_U               0x16
#define DIK_I               0x17
#define DIK_O               0x18
#define DIK_P               0x19
#define DIK_LBRACKET        0x1A
#define DIK_RBRACKET        0x1B
#define DIK_RETURN          0x1C    /* Enter on main keyboard */
#define DIK_LCONTROL        0x1D
#define DIK_A               0x1E
#define DIK_S               0x1F
#define DIK_D               0x20
#define DIK_F               0x21
#define DIK_G               0x22
#define DIK_H               0x23
#define DIK_J               0x24
#define DIK_K               0x25
#define DIK_L               0x26
#define DIK_SEMICOLON       0x27
#define DIK_APOSTROPHE      0x28
#define DIK_GRAVE           0x29    /* accent grave */
#define DIK_LSHIFT          0x2A
#define DIK_BACKSLASH       0x2B
#define DIK_Z               0x2C
#define DIK_X               0x2D
#define DIK_C               0x2E
#define DIK_V               0x2F
#define DIK_B               0x30
#define DIK_N               0x31
#define DIK_M               0x32
#define DIK_COMMA           0x33
#define DIK_PERIOD          0x34    /* . on main keyboard */
#define DIK_SLASH           0x35    /* / on main keyboard */
#define DIK_RSHIFT          0x36
#define DIK_MULTIPLY        0x37    /* * on numeric keypad */
#define DIK_LMENU           0x38    /* left Alt */
#define DIK_SPACE           0x39
#define DIK_CAPITAL         0x3A
#define DIK_F1              0x3B
#define DIK_F2              0x3C
#define DIK_F3              0x3D
#define DIK_F4              0x3E
#define DIK_F5              0x3F
#define DIK_F6              0x40
#define DIK_F7              0x41
#define DIK_F8              0x42
#define DIK_F9              0x43
#define DIK_F10             0x44
#define DIK_NUMLOCK         0x45
#define DIK_SCROLL          0x46    /* Scroll Lock */
#define DIK_NUMPAD7         0x47
#define DIK_NUMPAD8         0x48
#define DIK_NUMPAD9         0x49
#define DIK_SUBTRACT        0x4A    /* - on numeric keypad */
#define DIK_NUMPAD4         0x4B
#define DIK_NUMPAD5         0x4C
#define DIK_NUMPAD6         0x4D
#define DIK_ADD             0x4E    /* + on numeric keypad */
#define DIK_NUMPAD1         0x4F
#define DIK_NUMPAD2         0x50
#define DIK_NUMPAD3         0x51
#define DIK_NUMPAD0         0x52
#define DIK_DECIMAL         0x53    /* . on numeric keypad */
#define DIK_OEM_102         0x56    /* <> or \| on RT 102-key keyboard (Non-U.S.) */
#define DIK_F11             0x57
#define DIK_F12             0x58
#define DIK_F13             0x64    /*                     (NEC PC98) */
#define DIK_F14             0x65    /*                     (NEC PC98) */
#define DIK_F15             0x66    /*                     (NEC PC98) */
#define DIK_KANA            0x70    /* (Japanese keyboard)            */
#define DIK_ABNT_C1         0x73    /* /? on Brazilian keyboard */
#define DIK_CONVERT         0x79    /* (Japanese keyboard)            */
#define DIK_NOCONVERT       0x7B    /* (Japanese keyboard)            */
#define DIK_YEN             0x7D    /* (Japanese keyboard)            */
#define DIK_ABNT_C2         0x7E    /* Numpad . on Brazilian keyboard */
#define DIK_NUMPADEQUALS    0x8D    /* = on numeric keypad (NEC PC98) */
#define DIK_PREVTRACK       0x90    /* Previous Track (DIK_CIRCUMFLEX on Japanese keyboard) */
#define DIK_AT              0x91    /*                     (NEC PC98) */
#define DIK_COLON           0x92    /*                     (NEC PC98) */
#define DIK_UNDERLINE       0x93    /*                     (NEC PC98) */
#define DIK_KANJI           0x94    /* (Japanese keyboard)            */
#define DIK_STOP            0x95    /*                     (NEC PC98) */
#define DIK_AX              0x96    /*                     (Japan AX) */
#define DIK_UNLABELED       0x97    /*                        (J3100) */
#define DIK_NEXTTRACK       0x99    /* Next Track */
#define DIK_NUMPADENTER     0x9C    /* Enter on numeric keypad */
#define DIK_RCONTROL        0x9D
#define DIK_MUTE            0xA0    /* Mute */
#define DIK_CALCULATOR      0xA1    /* Calculator */
#define DIK_PLAYPAUSE       0xA2    /* Play / Pause */
#define DIK_MEDIASTOP       0xA4    /* Media Stop */
#define DIK_VOLUMEDOWN      0xAE    /* Volume - */
#define DIK_VOLUMEUP        0xB0    /* Volume + */
#define DIK_WEBHOME         0xB2    /* Web home */
#define DIK_NUMPADCOMMA     0xB3    /* , on numeric keypad (NEC PC98) */
#define DIK_DIVIDE          0xB5    /* / on numeric keypad */
#define DIK_SYSRQ           0xB7
#define DIK_RMENU           0xB8    /* right Alt */
#define DIK_PAUSE           0xC5    /* Pause */
#define DIK_HOME            0xC7    /* Home on arrow keypad */
#define DIK_UP              0xC8    /* UpArrow on arrow keypad */
#define DIK_PRIOR           0xC9    /* PgUp on arrow keypad */
#define DIK_LEFT            0xCB    /* LeftArrow on arrow keypad */
#define DIK_RIGHT           0xCD    /* RightArrow on arrow keypad */
#define DIK_END             0xCF    /* End on arrow keypad */
#define DIK_DOWN            0xD0    /* DownArrow on arrow keypad */
#define DIK_NEXT            0xD1    /* PgDn on arrow keypad */
#define DIK_INSERT          0xD2    /* Insert on arrow keypad */
#define DIK_DELETE          0xD3    /* Delete on arrow keypad */
#define DIK_LWIN            0xDB    /* Left Windows key */
#define DIK_RWIN            0xDC    /* Right Windows key */
#define DIK_APPS            0xDD    /* AppMenu key */
#define DIK_POWER           0xDE    /* System Power */
#define DIK_SLEEP           0xDF    /* System Sleep */
#define DIK_WAKE            0xE3    /* System Wake */
#define DIK_WEBSEARCH       0xE5    /* Web Search */
#define DIK_WEBFAVORITES    0xE6    /* Web Favorites */
#define DIK_WEBREFRESH      0xE7    /* Web Refresh */
#define DIK_WEBSTOP         0xE8    /* Web Stop */
#define DIK_WEBFORWARD      0xE9    /* Web Forward */
#define DIK_WEBBACK         0xEA    /* Web Back */
#define DIK_MYCOMPUTER      0xEB    /* My Computer */
#define DIK_MAIL            0xEC    /* Mail */
#define DIK_MEDIASELECT     0xED    /* Media Select */

/* Alternate names for keys, to facilitate transition from DOS apps to Win 32 */
#define DIK_BACKSPACE       DIK_BACK            /* backspace */
#define DIK_NUMPADSTAR      DIK_MULTIPLY        /* * on numeric keypad */
#define DIK_LALT            DIK_LMENU           /* left Alt */
#define DIK_CAPSLOCK        DIK_CAPITAL         /* CapsLock */
#define DIK_NUMPADMINUS     DIK_SUBTRACT        /* - on numeric keypad */
#define DIK_NUMPADPLUS      DIK_ADD             /* + on numeric keypad */
#define DIK_NUMPADPERIOD    DIK_DECIMAL         /* . on numeric keypad */
#define DIK_NUMPADSLASH     DIK_DIVIDE          /* / on numeric keypad */
#define DIK_RALT            DIK_RMENU           /* right Alt */
#define DIK_UPARROW         DIK_UP              /* UpArrow on arrow keypad */
#define DIK_PGUP            DIK_PRIOR           /* PgUp on arrow keypad */
#define DIK_LEFTARROW       DIK_LEFT            /* LeftArrow on arrow keypad */
#define DIK_RIGHTARROW      DIK_RIGHT           /* RightArrow on arrow keypad */
#define DIK_DOWNARROW       DIK_DOWN            /* DownArrow on arrow keypad */
#define DIK_PGDN            DIK_NEXT            /* PgDn on arrow keypad */

// Add a helper structure to map between DIK codes and VK codes
struct KeyCodeMapping {
    DWORD dikCode;
    DWORD vkCode;
    const char* keyName;
};

// Define mappings for commonly used keys
const KeyCodeMapping KeyMappings[] = {
    // Arrows and Navigation
    { DIK_UP,        VK_UP,       "Up Arrow" },
    { DIK_DOWN,      VK_DOWN,     "Down Arrow" },
    { DIK_LEFT,      VK_LEFT,     "Left Arrow" },
    { DIK_RIGHT,     VK_RIGHT,    "Right Arrow" },
    { DIK_HOME,      VK_HOME,     "Home" },
    { DIK_END,       VK_END,      "End" },
    { DIK_PRIOR,     VK_PRIOR,    "Page Up" },
    { DIK_NEXT,      VK_NEXT,     "Page Down" },
    { DIK_INSERT,    VK_INSERT,   "Insert" },
    { DIK_DELETE,    VK_DELETE,   "Delete" },
    
    // Letters
    { DIK_A,         'A',         "A" },
    { DIK_B,         'B',         "B" },
    { DIK_C,         'C',         "C" },
    { DIK_D,         'D',         "D" },
    { DIK_E,         'E',         "E" },
    { DIK_F,         'F',         "F" },
    { DIK_G,         'G',         "G" },
    { DIK_H,         'H',         "H" },
    { DIK_I,         'I',         "I" },
    { DIK_J,         'J',         "J" },
    { DIK_K,         'K',         "K" },
    { DIK_L,         'L',         "L" },
    { DIK_M,         'M',         "M" },
    { DIK_N,         'N',         "N" },
    { DIK_O,         'O',         "O" },
    { DIK_P,         'P',         "P" },
    { DIK_Q,         'Q',         "Q" },
    { DIK_R,         'R',         "R" },
    { DIK_S,         'S',         "S" },
    { DIK_T,         'T',         "T" },
    { DIK_U,         'U',         "U" },
    { DIK_V,         'V',         "V" },
    { DIK_W,         'W',         "W" },
    { DIK_X,         'X',         "X" },
    { DIK_Y,         'Y',         "Y" },
    { DIK_Z,         'Z',         "Z" },
    
    // Numbers
    { DIK_0,         '0',         "0" },
    { DIK_1,         '1',         "1" },
    { DIK_2,         '2',         "2" },
    { DIK_3,         '3',         "3" },
    { DIK_4,         '4',         "4" },
    { DIK_5,         '5',         "5" },
    { DIK_6,         '6',         "6" },
    { DIK_7,         '7',         "7" },
    { DIK_8,         '8',         "8" },
    { DIK_9,         '9',         "9" },
    
    // Numpad
    { DIK_NUMPAD0,   VK_NUMPAD0,  "Numpad 0" },
    { DIK_NUMPAD1,   VK_NUMPAD1,  "Numpad 1" },
    { DIK_NUMPAD2,   VK_NUMPAD2,  "Numpad 2" },
    { DIK_NUMPAD3,   VK_NUMPAD3,  "Numpad 3" },
    { DIK_NUMPAD4,   VK_NUMPAD4,  "Numpad 4" },
    { DIK_NUMPAD5,   VK_NUMPAD5,  "Numpad 5" },
    { DIK_NUMPAD6,   VK_NUMPAD6,  "Numpad 6" },
    { DIK_NUMPAD7,   VK_NUMPAD7,  "Numpad 7" },
    { DIK_NUMPAD8,   VK_NUMPAD8,  "Numpad 8" },
    { DIK_NUMPAD9,   VK_NUMPAD9,  "Numpad 9" },
    { DIK_DECIMAL,   VK_DECIMAL,  "Numpad ." },
    { DIK_ADD,       VK_ADD,      "Numpad +" },
    { DIK_SUBTRACT,  VK_SUBTRACT, "Numpad -" },
    { DIK_MULTIPLY,  VK_MULTIPLY, "Numpad *" },
    { DIK_DIVIDE,    VK_DIVIDE,   "Numpad /" },
    { DIK_NUMPADENTER, VK_RETURN, "Numpad Enter" },
    
    // Function keys
    { DIK_F1,        VK_F1,       "F1" },
    { DIK_F2,        VK_F2,       "F2" },
    { DIK_F3,        VK_F3,       "F3" },
    { DIK_F4,        VK_F4,       "F4" },
    { DIK_F5,        VK_F5,       "F5" },
    { DIK_F6,        VK_F6,       "F6" },
    { DIK_F7,        VK_F7,       "F7" },
    { DIK_F8,        VK_F8,       "F8" },
    { DIK_F9,        VK_F9,       "F9" },
    { DIK_F10,       VK_F10,      "F10" },
    { DIK_F11,       VK_F11,      "F11" },
    { DIK_F12,       VK_F12,      "F12" },
    
    // Special keys
    { DIK_ESCAPE,    VK_ESCAPE,   "Esc" },
    { DIK_TAB,       VK_TAB,      "Tab" },
    { DIK_RETURN,    VK_RETURN,   "Enter" },
    { DIK_SPACE,     VK_SPACE,    "Space" },
    { DIK_BACK,      VK_BACK,     "Backspace" },
    { DIK_CAPITAL,   VK_CAPITAL,  "Caps Lock" },
    { DIK_LSHIFT,    VK_LSHIFT,   "L-Shift" },
    { DIK_RSHIFT,    VK_RSHIFT,   "R-Shift" },
    { DIK_LCONTROL,  VK_LCONTROL, "L-Ctrl" },
    { DIK_RCONTROL,  VK_RCONTROL, "R-Ctrl" },
    { DIK_LMENU,     VK_LMENU,    "L-Alt" },
    { DIK_RMENU,     VK_RMENU,    "R-Alt" },
    { DIK_LWIN,      VK_LWIN,     "L-Win" },
    { DIK_RWIN,      VK_RWIN,     "R-Win" },
    { DIK_APPS,      VK_APPS,     "Menu" },
    { DIK_PAUSE,     VK_PAUSE,    "Pause" },
    { DIK_SYSRQ,     VK_SNAPSHOT, "Print Screen" },
    
    // Punctuation/symbols
    { DIK_MINUS,     VK_OEM_MINUS,    "Minus" },
    { DIK_EQUALS,    VK_OEM_PLUS,     "Equals" },
    { DIK_LBRACKET,  VK_OEM_4,        "Left Bracket" },
    { DIK_RBRACKET,  VK_OEM_6,        "Right Bracket" },
    { DIK_SEMICOLON, VK_OEM_1,        "Semicolon" },
    { DIK_APOSTROPHE,VK_OEM_7,        "Apostrophe" },
    { DIK_GRAVE,     VK_OEM_3,        "Grave" },
    { DIK_BACKSLASH, VK_OEM_5,        "Backslash" },
    { DIK_COMMA,     VK_OEM_COMMA,    "Comma" },
    { DIK_PERIOD,    VK_OEM_PERIOD,   "Period" },
    { DIK_SLASH,     VK_OEM_2,        "Slash" },
    
    // Media keys
    { DIK_PREVTRACK, VK_MEDIA_PREV_TRACK, "Previous Track" },
    { DIK_NEXTTRACK, VK_MEDIA_NEXT_TRACK, "Next Track" },
    { DIK_MUTE,      VK_VOLUME_MUTE,      "Mute" },
    { DIK_VOLUMEDOWN,VK_VOLUME_DOWN,      "Volume Down" },
    { DIK_VOLUMEUP,  VK_VOLUME_UP,        "Volume Up" },
    { DIK_PLAYPAUSE, VK_MEDIA_PLAY_PAUSE, "Play/Pause" },
    { DIK_MEDIASTOP, VK_MEDIA_STOP,       "Media Stop" },
};

/****************************************************************************
 *
 *      DirectInput gamepad constants and mappings
 *
 ****************************************************************************/

// Input device type constants
#define INPUT_DEVICE_KEYBOARD 0
#define INPUT_DEVICE_GAMEPAD  1

// Gamepad-related defines
#define DINPUT_BUFFERSIZE 16
#define MAX_CONTROLLERS 4

// Common gamepad button mappings (generic XInput-style layout)
#define GAMEPAD_BUTTON_A        0
#define GAMEPAD_BUTTON_B        1
#define GAMEPAD_BUTTON_X        2
#define GAMEPAD_BUTTON_Y        3
#define GAMEPAD_BUTTON_LB       4
#define GAMEPAD_BUTTON_RB       5
#define GAMEPAD_BUTTON_BACK     6
#define GAMEPAD_BUTTON_START    7
#define GAMEPAD_BUTTON_LSTICK   8
#define GAMEPAD_BUTTON_RSTICK   9
#define GAMEPAD_BUTTON_DPAD_UP  10
#define GAMEPAD_BUTTON_DPAD_DOWN 11
#define GAMEPAD_BUTTON_DPAD_LEFT 12
#define GAMEPAD_BUTTON_DPAD_RIGHT 13
#define GAMEPAD_BUTTON_GUIDE    14
#define GAMEPAD_AXIS_LX         15  // Left stick X
#define GAMEPAD_AXIS_LY         16  // Left stick Y
#define GAMEPAD_AXIS_RX         17  // Right stick X
#define GAMEPAD_AXIS_RY         18  // Right stick Y
#define GAMEPAD_AXIS_LT         19  // Left trigger
#define GAMEPAD_AXIS_RT         20  // Right trigger

// Add gamepad button name mappings to existing structure
struct GamepadButtonMapping {
    int buttonIndex;
    const char* buttonName;
    const char* displaySymbol;
};

// Common gamepad button names across different controllers
const GamepadButtonMapping GamepadButtonMappings[] = {
    { GAMEPAD_BUTTON_A,         "A Button",      "A" },
    { GAMEPAD_BUTTON_B,         "B Button",      "B" },
    { GAMEPAD_BUTTON_X,         "X Button",      "X" },
    { GAMEPAD_BUTTON_Y,         "Y Button",      "Y" },
    { GAMEPAD_BUTTON_LB,        "Left Bumper",   "LB" },
    { GAMEPAD_BUTTON_RB,        "Right Bumper",  "RB" },
    { GAMEPAD_BUTTON_BACK,      "Back",          "Back" },
    { GAMEPAD_BUTTON_START,     "Start",         "Start" },
    { GAMEPAD_BUTTON_LSTICK,    "Left Stick",    "L3" },
    { GAMEPAD_BUTTON_RSTICK,    "Right Stick",   "R3" },
    { GAMEPAD_BUTTON_DPAD_UP,   "D-Pad Up",      "↑" },
    { GAMEPAD_BUTTON_DPAD_DOWN, "D-Pad Down",    "↓" },
    { GAMEPAD_BUTTON_DPAD_LEFT, "D-Pad Left",    "←" },
    { GAMEPAD_BUTTON_DPAD_RIGHT,"D-Pad Right",   "→" },
    { GAMEPAD_BUTTON_GUIDE,     "Guide Button",  "Guide" },
    { GAMEPAD_AXIS_LX,          "Left Stick X",  "LX" },
    { GAMEPAD_AXIS_LY,          "Left Stick Y",  "LY" },
    { GAMEPAD_AXIS_RX,          "Right Stick X", "RX" },
    { GAMEPAD_AXIS_RY,          "Right Stick Y", "RY" },
    { GAMEPAD_AXIS_LT,          "Left Trigger",  "LT" },
    { GAMEPAD_AXIS_RT,          "Right Trigger", "RT" }
};

// Helper functions for gamepad button name retrieval
std::string GetGamepadButtonName(int buttonIndex);
std::string GetGamepadButtonSymbol(int buttonIndex);

// Function declarations
int MapDIKToVK(int dikCode);
std::string GetDIKeyName(int dikCode);