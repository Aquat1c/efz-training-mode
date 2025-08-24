#include "../include/gui/imgui_gui.h"
#include "../include/gui/imgui_impl.h"
#include "../include/utils/utilities.h"

#include "../include/core/constants.h"
#include "../include/core/memory.h"
#include "../include/core/logger.h"
#include "../include/gui/gui.h"
#include "../include/utils/config.h"
#include "../include/gui/overlay.h"
#include "../include/game/character_settings.h"
#include "../include/game/frame_monitor.h"
#include "../include/input/input_motion.h"
#include "../include/input/input_motion.h"
#include "../include/utils/bgm_control.h"
#include "../include/input/input_debug.h"
#include <algorithm> // Add this for std::max
// For opening links from Help tab
#include <shellapi.h>
#pragma comment(lib, "shell32.lib")
#include "../include/gui/gif_player.h"
// Forward declare SpamAttackButton so we can use it in this file
extern void SpamAttackButton(uintptr_t playerBase, uint8_t button, int frames, const char* buttonName);
#include "../include/game/practice_patch.h"
#include "../include/gui/imgui_settings.h"

// Add these constants at the top of the file after includes
// These are from input_motion.cpp but we need them here

// Button constants
#define BUTTON_A    GAME_INPUT_A
#define BUTTON_B    GAME_INPUT_B
#define BUTTON_C    GAME_INPUT_C
#define BUTTON_D    GAME_INPUT_D

namespace ImGuiGui {
    // Define static variable at namespace level
    static bool s_randomInputActive = false;

    // Action type mapping (same as in gui_auto_action.cpp)
    static const int ComboIndexToActionType[] = {
        ACTION_5A,          // 0 = 5A
        ACTION_5B,          // 1 = 5B
        ACTION_5C,          // 2 = 5C
        ACTION_2A,          // 3 = 2A
        ACTION_2B,          // 4 = 2B
        ACTION_2C,          // 5 = 2C
        ACTION_JA,          // 6 = j.A 
        ACTION_JB,          // 7 = j.B
        ACTION_JC,          // 8 = j.C
        ACTION_QCF,         // 9 = 236 (QCF)
        ACTION_DP,          // 10 = 623 (DP)
        ACTION_QCB,         // 11 = 214 (QCB)
        ACTION_421,         // 12 = 421 (Half-circle Down)
        ACTION_SUPER1,      // 13 = 41236 (HCF)
        ACTION_SUPER2,      // 14 = 63214 (HCB)
        ACTION_236236,      // 15 = 236236 (Double QCF)
        ACTION_214214,      // 16 = 214214 (Double QCB)
        ACTION_641236,      // 17 = 641236 (Half-Circle Forward + QCF)
        ACTION_JUMP,        // 18 = Jump
        ACTION_BACKDASH,    // 19 = Backdash
        ACTION_FORWARD_DASH,// 20 = Forward Dash
        ACTION_BLOCK,       // 21 = Block
        ACTION_CUSTOM       // 22 = Custom ID
    };

    // Helper function to convert action type to combo index
    static int ActionTypeToComboIndex(int actionType) {
        for (int i = 0; i < IM_ARRAYSIZE(ComboIndexToActionType); i++) {
            if (ComboIndexToActionType[i] == actionType) {
                return i;
            }
        }
        return 0; // Default to 5A
    }

    // Initialize GUI state
    ImGuiGuiState guiState = {
        false,  // visible
        0,      // currentTab
        -1,     // requestedTab
        {}      // localData (initialized with default values)
    };

    // Initialize the GUI
    void Initialize() {
        // Copy current display data into our local copy
        guiState.localData = displayData;
        
        // Only show in detailed mode
        LogOut("[IMGUI_GUI] GUI state initialized", detailedLogging.load());
    }

    // Game Values Tab
    void RenderGameValuesTab() {
        ImGui::PushItemWidth(120);

        // Layout with two columns
        ImGui::Columns(2, "playerColumns", false);

        // P1 Column
        ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.0f, 1.0f), "Player 1 (%s)", 
            guiState.localData.p1CharName[0] ? guiState.localData.p1CharName : "Unknown");
        ImGui::Separator();

        // P1 HP
        int hp1 = guiState.localData.hp1;
        if (ImGui::InputInt("P1 HP", &hp1)) {
            guiState.localData.hp1 = CLAMP(hp1, 0, MAX_HP);
        }

        // P1 Meter
        int meter1 = guiState.localData.meter1;
        if (ImGui::InputInt("P1 Meter", &meter1)) {
            guiState.localData.meter1 = CLAMP(meter1, 0, MAX_METER);
        }

        // P1 RF
        float rf1 = (float)guiState.localData.rf1;
        if (ImGui::InputFloat("P1 RF", &rf1, 0.1f, 1.0f, "%.1f")) {
            guiState.localData.rf1 = CLAMP(rf1, 0.0f, MAX_RF);
        }

        // P1 Blue IC toggle
        ImGui::Checkbox("P1 Blue IC", &guiState.localData.p1BlueIC);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Checked = Blue IC (forced), Unchecked = Red IC (normal)\nApply changes to update the game");
        }

        // P1 Position
        float x1 = (float)guiState.localData.x1;
        float y1 = (float)guiState.localData.y1;
        if (ImGui::InputFloat("P1 X", &x1, 1.0f, 10.0f, "%.2f")) {
            guiState.localData.x1 = x1;
        }
        if (ImGui::InputFloat("P1 Y", &y1, 1.0f, 10.0f, "%.2f")) {
            guiState.localData.y1 = y1;
        }

        // Next column (P2)
        ImGui::NextColumn();
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.5f, 1.0f), "Player 2 (%s)", 
            guiState.localData.p2CharName[0] ? guiState.localData.p2CharName : "Unknown");
        ImGui::Separator();

        // P2 HP
        int hp2 = guiState.localData.hp2;
        if (ImGui::InputInt("P2 HP", &hp2)) {
            guiState.localData.hp2 = CLAMP(hp2, 0, MAX_HP);
        }

        // P2 Meter
        int meter2 = guiState.localData.meter2;
        if (ImGui::InputInt("P2 Meter", &meter2)) {
            guiState.localData.meter2 = CLAMP(meter2, 0, MAX_METER);
        }

        // P2 RF
        float rf2 = (float)guiState.localData.rf2;
        if (ImGui::InputFloat("P2 RF", &rf2, 0.1f, 1.0f, "%.1f")) {
            guiState.localData.rf2 = CLAMP(rf2, 0.0f, MAX_RF);
        }

        // P2 Blue IC toggle
        ImGui::Checkbox("P2 Blue IC", &guiState.localData.p2BlueIC);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Checked = Blue IC (forced), Unchecked = Red IC (normal)\nApply changes to update the game");
        }

        // P2 Position
        float x2 = (float)guiState.localData.x2;
        float y2 = (float)guiState.localData.y2;
        if (ImGui::InputFloat("P2 X", &x2, 1.0f, 10.0f, "%.2f")) {
            guiState.localData.x2 = x2;
        }
        if (ImGui::InputFloat("P2 Y", &y2, 1.0f, 10.0f, "%.2f")) {
            guiState.localData.y2 = y2;
        }

        ImGui::Columns(1);
        ImGui::Separator();

        // NEW: Add P2 Control checkbox here, before other settings
        ImGui::PushItemWidth(-1); // Make checkbox span width
        ImGui::Checkbox("Enable P2 Control (Practice Mode Only)", &guiState.localData.p2ControlEnabled);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Gives you direct control over Player 2 in Practice Mode.\nThis is required for the Debug Input tab to work.\nApply changes to update the game.");
        }
        ImGui::PopItemWidth();
        ImGui::Separator();


        // Action buttons
        if (ImGui::Button("Swap Positions", ImVec2(150, 30))) {
            // Swap X positions
            std::swap(guiState.localData.x1, guiState.localData.x2);
        }
        ImGui::SameLine();
        if (ImGui::Button("Round Start", ImVec2(150, 30))) {
            // Reset to round start positions
            guiState.localData.x1 = 240.0;
            guiState.localData.y1 = 0.0;
            guiState.localData.x2 = 400.0;
            guiState.localData.y2 = 0.0;
        }

        ImGui::Separator();

        // Auto-Airtech Settings
        ImGui::TextUnformatted("Auto-Airtech Direction:");
        const char* airtechItems[] = { "Neutral (Disabled)", "Forward", "Backward" };
        int airtechDir = guiState.localData.autoAirtech ? guiState.localData.airtechDirection + 1 : 0;
        if (ImGui::Combo("##AirtechDir", &airtechDir, airtechItems, IM_ARRAYSIZE(airtechItems))) {
            guiState.localData.autoAirtech = (airtechDir > 0);
            guiState.localData.airtechDirection = airtechDir > 0 ? airtechDir - 1 : 0;
        }

        // Airtech delay
        ImGui::SameLine();
        ImGui::TextUnformatted("Delay:");
        int airtechDelay = guiState.localData.airtechDelay;
        ImGui::SameLine();
        ImGui::PushItemWidth(60);
        if (ImGui::InputInt("##AirtechDelay", &airtechDelay)) {
            guiState.localData.airtechDelay = CLAMP(airtechDelay, 0, 60);
        }
        ImGui::SameLine();
        ImGui::TextUnformatted("frames");
        ImGui::PopItemWidth();

        ImGui::PopItemWidth();
    }

    // Auto Action Tab
    void RenderAutoActionTab() {
        // Auto-action master toggle
        bool enabled = guiState.localData.autoAction;
        if (ImGui::Checkbox("Enable Auto Action System", &enabled)) {
            guiState.localData.autoAction = enabled;
        }
        
        // Player target selector
        ImGui::Text("Apply To:");
        const char* playerItems[] = { "P1 Only", "P2 Only", "Both Players" };
        int playerIndex = guiState.localData.autoActionPlayer - 1; // Convert 1-based to 0-based
        if (ImGui::Combo("Target", &playerIndex, playerItems, IM_ARRAYSIZE(playerItems))) {
            guiState.localData.autoActionPlayer = playerIndex + 1; // Convert back to 1-based
        }
        
        ImGui::Separator();
        
        // Define a struct for trigger settings to reduce code repetition
        struct TriggerSettings {
            const char* name;
            bool* enabled;
            int* action;
            int* delay;
            int* strength; // NEW: Add strength member
            int* custom;
        };
        
        // Define an array of trigger settings
        TriggerSettings triggers[] = {
            { "After Block", &guiState.localData.triggerAfterBlock, &guiState.localData.actionAfterBlock, 
              &guiState.localData.delayAfterBlock, &guiState.localData.strengthAfterBlock, &guiState.localData.customAfterBlock },
            { "On Wakeup", &guiState.localData.triggerOnWakeup, &guiState.localData.actionOnWakeup, 
              &guiState.localData.delayOnWakeup, &guiState.localData.strengthOnWakeup, &guiState.localData.customOnWakeup },
            { "After Hitstun", &guiState.localData.triggerAfterHitstun, &guiState.localData.actionAfterHitstun, 
              &guiState.localData.delayAfterHitstun, &guiState.localData.strengthAfterHitstun, &guiState.localData.customAfterHitstun },
            { "After Airtech", &guiState.localData.triggerAfterAirtech, &guiState.localData.actionAfterAirtech, 
              &guiState.localData.delayAfterAirtech, &guiState.localData.strengthAfterAirtech, &guiState.localData.customAfterAirtech }
        };
        
        // Motion list (includes directions/stances plus motions and utility actions)
        const char* motionItems[] = {
            "Standing", "Crouching", "Jumping",
            "236 (QCF)", "623 (DP)", "214 (QCB)", "421 (Half-circle Down)",
            "41236 (HCF)", "63214 (HCB)", "236236 (Double QCF)", "214214 (Double QCB)",
            "641236", "Jump", "Backdash", "Forward Dash", "Block", "Custom ID"
        };

        // Button list (applies to both directions and motions)
        const char* buttonItems[] = { "A", "B", "C", "D" };

        // Helpers
        auto IsNormalAttackAction = [](int action) {
            return action == ACTION_5A || action == ACTION_5B || action == ACTION_5C ||
                   action == ACTION_2A || action == ACTION_2B || action == ACTION_2C ||
                   action == ACTION_JA || action == ACTION_JB || action == ACTION_JC;
        };
        auto IsSpecialMoveAction = [](int action) {
            return action == ACTION_QCF || action == ACTION_DP || action == ACTION_QCB ||
                   action == ACTION_421 || action == ACTION_SUPER1 || action == ACTION_SUPER2 ||
                   action == ACTION_236236 || action == ACTION_214214 || action == ACTION_641236;
        };
        auto GetPostureIndexForAction = [](int action) -> int {
            if (action == ACTION_5A || action == ACTION_5B || action == ACTION_5C) return 0; // Standing
            if (action == ACTION_2A || action == ACTION_2B || action == ACTION_2C) return 1; // Crouching
            if (action == ACTION_JA || action == ACTION_JB || action == ACTION_JC) return 2; // Jumping
            return -1;
        };
        auto GetMotionIndexForAction = [&](int action) -> int {
            int postureIdx = GetPostureIndexForAction(action);
            if (postureIdx >= 0) return postureIdx; // 0..2
            switch (action) {
                case ACTION_QCF: return 3;
                case ACTION_DP: return 4;
                case ACTION_QCB: return 5;
                case ACTION_421: return 6;
                case ACTION_SUPER1: return 7; // 41236
                case ACTION_SUPER2: return 8; // 63214
                case ACTION_236236: return 9;
                case ACTION_214214: return 10;
                case ACTION_641236: return 11;
                case ACTION_JUMP: return 12;
                case ACTION_BACKDASH: return 13;
                case ACTION_FORWARD_DASH: return 14;
                case ACTION_BLOCK: return 15;
                case ACTION_CUSTOM: return 16;
                default: return 0; // default Standing
            }
        };
        auto MapPostureAndButtonToAction = [](int postureIdx, int buttonIdx) -> int {
            // buttonIdx: 0=A,1=B,2=C,3=D. D not supported in ACTION_* enums; map D->C for now.
            int b = buttonIdx;
            if (b > 2) b = 2; // clamp D to C for normals
            switch (postureIdx) {
                case 0: // Standing
                    return b == 0 ? ACTION_5A : (b == 1 ? ACTION_5B : ACTION_5C);
                case 1: // Crouching
                    return b == 0 ? ACTION_2A : (b == 1 ? ACTION_2B : ACTION_2C);
                case 2: // Jumping
                    return b == 0 ? ACTION_JA : (b == 1 ? ACTION_JB : ACTION_JC);
                default:
                    return ACTION_5A;
            }
        };
        auto MapMotionIndexToAction = [](int motionIdx) -> int {
            switch (motionIdx) {
                case 3: return ACTION_QCF;
                case 4: return ACTION_DP;
                case 5: return ACTION_QCB;
                case 6: return ACTION_421;
                case 7: return ACTION_SUPER1; // 41236
                case 8: return ACTION_SUPER2; // 63214
                case 9: return ACTION_236236;
                case 10: return ACTION_214214;
                case 11: return ACTION_641236;
                case 12: return ACTION_JUMP;
                case 13: return ACTION_BACKDASH;
                case 14: return ACTION_FORWARD_DASH;
                case 15: return ACTION_BLOCK;
                case 16: return ACTION_CUSTOM;
                default: return ACTION_5A; // For posture indices, action will be set via button mapping
            }
        };
        
        // Render each trigger's settings
        for (int i = 0; i < IM_ARRAYSIZE(triggers); i++) {
            ImGui::PushID(i);
            
            // Create a unique label for the checkbox
            std::string checkboxLabel = std::string(triggers[i].name) + ":";
            if (ImGui::Checkbox(checkboxLabel.c_str(), triggers[i].enabled)) {
                // When enabling a trigger, make sure it's configured with reasonable defaults
                if (*triggers[i].enabled) {
                    if (*triggers[i].action < 0) *triggers[i].action = 0;
                    if (*triggers[i].delay < 0) *triggers[i].delay = 0;
                    if (*triggers[i].strength < 0) *triggers[i].strength = 0; // Default to Light (A)
                }
            }
            
            ImGui::SameLine();
            ImGui::SetNextItemWidth(150);
            // Determine current motion selection from action
            int motionIndex = GetMotionIndexForAction(*triggers[i].action);
            if (ImGui::Combo("Button", &motionIndex, motionItems, IM_ARRAYSIZE(motionItems))) {//Actually directions/motion inputs
                // When motion changes, update action accordingly
                if (motionIndex <= 2) {
                    // Posture selected: use current button choice to pick specific normal
                    int currentButtonIdx = 0;
                    // For normals, derive from action; for specials, derive from strength
                    if (IsNormalAttackAction(*triggers[i].action)) {
                        // Map current action to button index A/B/C
                        switch (*triggers[i].action) {
                            case ACTION_5A: case ACTION_2A: case ACTION_JA: currentButtonIdx = 0; break;
                            case ACTION_5B: case ACTION_2B: case ACTION_JB: currentButtonIdx = 1; break;
                            case ACTION_5C: case ACTION_2C: case ACTION_JC: currentButtonIdx = 2; break;
                            default: currentButtonIdx = 0; break;
                        }
                    } else {
                        currentButtonIdx = *triggers[i].strength; // reuse strength slot
                    }
                    *triggers[i].action = MapPostureAndButtonToAction(motionIndex, currentButtonIdx);
                } else {
                    // Motion selected: set action directly
                    *triggers[i].action = MapMotionIndexToAction(motionIndex);
                }
            }

            // Separate Button/Direction combo: for Jump show Forward/Neutral/Backwards; hide for Dashes
            ImGui::SameLine();
            ImGui::SetNextItemWidth(90);
            int buttonIdx = 0;
            int postureIdx = GetPostureIndexForAction(*triggers[i].action);
            if (*triggers[i].action == ACTION_JUMP) {
                // Use strength field as direction selector for Jump
                const char* dirItems[] = { "Neutral", "Forward", "Backwards" };
                int dir = *triggers[i].strength;
                if (ImGui::Combo("", &dir, dirItems, IM_ARRAYSIZE(dirItems))) {
                    *triggers[i].strength = (dir < 0 ? 0 : (dir > 2 ? 2 : dir));
                }
            } else if (*triggers[i].action == ACTION_BACKDASH || *triggers[i].action == ACTION_FORWARD_DASH) {
                // Hide button combo for dash actions
                ImGui::Dummy(ImVec2(90, 0));
            } else if (postureIdx >= 0) {
                // Derive button from current normal action
                switch (*triggers[i].action) {
                    case ACTION_5A: case ACTION_2A: case ACTION_JA: buttonIdx = 0; break;
                    case ACTION_5B: case ACTION_2B: case ACTION_JB: buttonIdx = 1; break;
                    case ACTION_5C: case ACTION_2C: case ACTION_JC: buttonIdx = 2; break;
                    default: buttonIdx = 0; break;
                }
                if (ImGui::Combo("", &buttonIdx, buttonItems, IM_ARRAYSIZE(buttonItems))) {//Delay
                    // Update to specific normal based on posture + button
                    *triggers[i].action = MapPostureAndButtonToAction(postureIdx, buttonIdx);
                }
                // Skip the generic handler below
                ImGui::PopID();
                ImGui::PushID(i);
            } else if (IsSpecialMoveAction(*triggers[i].action)) {
                // For specials, use strength value as button index (A/B/C). D will be clamped.
                buttonIdx = *triggers[i].strength;
                if (ImGui::Combo("", &buttonIdx, buttonItems, IM_ARRAYSIZE(buttonItems))) {//Delay
                    *triggers[i].strength = (buttonIdx > 2) ? 2 : buttonIdx;
                }
            } else {
                // For other actions (jump, dash, block, custom), keep buttonIdx but it won't affect action
                buttonIdx = *triggers[i].strength;
                if (*triggers[i].action != ACTION_BLOCK && *triggers[i].action != ACTION_CUSTOM) {
                    if (ImGui::Combo("", &buttonIdx, buttonItems, IM_ARRAYSIZE(buttonItems))) {//Delay
                        *triggers[i].strength = (buttonIdx > 2) ? 2 : buttonIdx;
                    }
                }
            }
            
            ImGui::SameLine();
            ImGui::SetNextItemWidth(80);
            
            // Fixed: Use a local variable for InputInt
            int delayValue = *triggers[i].delay;
            if (ImGui::InputInt("Delay", &delayValue, 1, 5)) {
                *triggers[i].delay = (std::max)(0, delayValue); // Add parentheses around std::max
            }
            
            // Only show custom ID input for custom action type
            if (*triggers[i].action == ACTION_CUSTOM) {
                ImGui::SameLine();
                ImGui::SetNextItemWidth(80);
                
                // Fixed: Use a local variable for InputInt
                int customValue = *triggers[i].custom;
                if (ImGui::InputInt("Move ID", &customValue, 1, 10)) {
                    *triggers[i].custom = (std::max)(0, customValue); // Add parentheses around std::max
                }
            }
            
            ImGui::PopID();
        }
    }

    // Help Tab implementation
    void RenderHelpTab() {
    ImGui::TextUnformatted("Hotkeys (can be changed in config.ini):");
        ImGui::Separator();

        const Config::Settings& cfg = Config::GetSettings();

        ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "Core Hotkeys:");
    ImGui::BulletText("Open/Close Menu: 3/%s (Gamepad: START)", GetKeyName(cfg.toggleImGuiKey).c_str());
    ImGui::BulletText("Load Position: %s (Gamepad: BACK)", GetKeyName(cfg.teleportKey).c_str());
    ImGui::BulletText("Save Position: %s (Gamepad: L3)", GetKeyName(cfg.recordKey).c_str());
    ImGui::BulletText("Toggle Stats Display: %s", GetKeyName(cfg.toggleTitleKey).c_str());
        //ImGui::BulletText("Reset Frame Counter: %s", GetKeyName(cfg.resetFrameCounterKey).c_str());
        //ImGui::BulletText("Show This Help Screen: %s", GetKeyName(cfg.helpKey).c_str());

        ImGui::Separator();
    ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "Positioning Hotkeys (Hold Load: Keyboard=%s, Gamepad=BACK, A/B/C/D = your attack keys):", GetKeyName(cfg.teleportKey).c_str());
    ImGui::BulletText("Center Players: %s + DOWN (Gamepad: BACK + D-PAD DOWN)", GetKeyName(cfg.teleportKey).c_str());
    ImGui::BulletText("Players to Left Corner: %s + LEFT (Gamepad: BACK + D-PAD LEFT)", GetKeyName(cfg.teleportKey).c_str());
    ImGui::BulletText("Players to Right Corner: %s + RIGHT (Gamepad: BACK + D-PAD RIGHT)", GetKeyName(cfg.teleportKey).c_str());
    ImGui::BulletText("Round Start Positions: %s + DOWN + A (keyboard only)", GetKeyName(cfg.teleportKey).c_str());
    ImGui::BulletText("Swap Player Positions: %s + D (keyboard only)", GetKeyName(cfg.teleportKey).c_str());

    ImGui::Separator();
        
        // Inline animated preview from embedded bytes
        unsigned gw = 0, gh = 0;
        if (IDirect3DTexture9* tex = GifPlayer::GetTexture(gw, gh)) {
            // Clamp to a reasonable size in the help panel
            const float maxW = 220.0f, maxH = 180.0f;
            float w = (float)gw, h = (float)gh;
            if (w > maxW) { float s = maxW / w; w *= s; h *= s; }
            if (h > maxH) { float s = maxH / h; w *= s; h *= s; }
            ImGui::Dummy(ImVec2(1, 6));
            ImGui::Image((ImTextureID)tex, ImVec2(w, h));
        } else {
            ImGui::TextDisabled("(GIF not loaded yet)");
        }

        ImGui::Separator();
    }

    // Add the implementation for the character tab
    void RenderCharacterTab() {
        ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "Character-Specific Settings");
        ImGui::Separator();
        
        // Check if characters are valid
        if (!AreCharactersInitialized()) {
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.5f, 1.0f), "No valid characters detected.");
            return;
        }
        
        // Update character IDs
        CharacterSettings::UpdateCharacterIDs(guiState.localData);
        
        // Get current characters for easier reference
        int p1CharID = guiState.localData.p1CharID;
        int p2CharID = guiState.localData.p2CharID;
        
        bool hasFeatures = false;
        
        // ---------- GLOBAL SETTINGS SECTION (TOP) ----------
        
        // Ikumi - Infinite Blood Mode
        if (p1CharID == CHAR_ID_IKUMI || p2CharID == CHAR_ID_IKUMI) {
            hasFeatures = true;
            
            bool infiniteBlood = guiState.localData.infiniteBloodMode;
            if (ImGui::Checkbox("Infinite Blood Mode (Ikumi)", &infiniteBlood)) {
                guiState.localData.infiniteBloodMode = infiniteBlood;
            }
            
            ImGui::SameLine();
            ImGui::TextDisabled("(?)");
            if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
                ImGui::TextUnformatted("Prevents Ikumi's genocide timer from depleting when it's active.\n"
                                       "This patch is only applied in Practice Mode.");
                ImGui::PopTextWrapPos();
                ImGui::EndTooltip();
            }
        }
        
        // Misuzu - Infinite Feather Mode
        if (p1CharID == CHAR_ID_MISUZU || p2CharID == CHAR_ID_MISUZU) {
            hasFeatures = true;
            
            bool infiniteFeathers = guiState.localData.infiniteFeatherMode;
            if (ImGui::Checkbox("Infinite Feather Mode (Misuzu)", &infiniteFeathers)) {
                guiState.localData.infiniteFeatherMode = infiniteFeathers;
            }
            
            ImGui::SameLine();
            ImGui::TextDisabled("(?)");
            if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
                ImGui::TextUnformatted("Prevents Misuzu's feathers from being consumed when using special moves.\n"
                                       "This patch is only applied in Practice Mode.");
                ImGui::PopTextWrapPos();
                ImGui::EndTooltip();
            }
        }
        
        // Blue IC/Red IC Toggle (universal for all characters)
        hasFeatures = true; // Always show this section since it works for all characters
        
        ImGui::Text("IC Color Override:");
        
        bool p1BlueIC = guiState.localData.p1BlueIC;
        if (ImGui::Checkbox("P1 Blue IC", &p1BlueIC)) {
            guiState.localData.p1BlueIC = p1BlueIC;
        }
        
        ImGui::SameLine();
        
        bool p2BlueIC = guiState.localData.p2BlueIC;
        if (ImGui::Checkbox("P2 Blue IC", &p2BlueIC)) {
            guiState.localData.p2BlueIC = p2BlueIC;
        }
        
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
            ImGui::TextUnformatted("Forces RF bar to Blue IC state (full RF special properties).\n"
                                   "When unchecked, RF bar returns to normal Red IC state.\n"
                                   "This affects all characters and works in all game modes.");
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
        
        if (hasFeatures) {
            ImGui::Separator();
            
            // Debug information for character features
            if (guiState.localData.infiniteBloodMode || guiState.localData.infiniteFeatherMode) {
                ImGui::Text("Debug Status:");
                ImGui::SameLine();
                if (CharacterSettings::IsMonitoringThreadActive()) {
                    ImGui::TextColored(ImVec4(0, 1, 0, 1), "Thread Active");
                } else {
                    ImGui::TextColored(ImVec4(1, 0, 0, 1), "Thread Inactive");
                }
                
                if (guiState.localData.infiniteFeatherMode) {
                    int p1Count, p2Count;
                    CharacterSettings::GetFeatherCounts(p1Count, p2Count);
                    ImGui::Text("Feather Tracking: P1=%d, P2=%d", p1Count, p2Count);
                }
            }
            
            ImGui::Separator();
        }
        
        // ---------- PLAYER SPECIFIC SECTIONS ----------
        // Create two columns for P1 and P2
        ImGui::Columns(2, "playerColumns", true);
        
        // --- P1 COLUMN ---
        ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.0f, 1.0f), "P1: %s", 
                          CharacterSettings::GetCharacterName(p1CharID).c_str());
        
        // P1 Ikumi Settings
        if (p1CharID == CHAR_ID_IKUMI) {
            // Blood Level
            int p1Blood = guiState.localData.p1IkumiBlood;
            float p1BloodPercent = (float)p1Blood / IKUMI_BLOOD_MAX;
            
            ImGui::Text("Blood Level:");
            
            // Progress bar with color gradient
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, 
                ImVec4(0.7f + p1BloodPercent * 0.3f, 0.3f - p1BloodPercent * 0.3f, 0.3f, 1.0f));
            ImGui::ProgressBar(p1BloodPercent, ImVec2(-1, 0), 
                              (std::to_string(p1Blood) + "/" + std::to_string(IKUMI_BLOOD_MAX)).c_str());
            ImGui::PopStyleColor();
            
            // Slider
            if (ImGui::SliderInt("##P1Blood", &p1Blood, 0, IKUMI_BLOOD_MAX)) {
                guiState.localData.p1IkumiBlood = p1Blood;
            }
            
            // Genocide Timer
            int p1Genocide = guiState.localData.p1IkumiGenocide;
            float genocideSeconds = p1Genocide / 60.0f;
            
            ImGui::Text("Genocide Timer: %.1f sec", genocideSeconds);
            if (ImGui::SliderInt("##P1Genocide", &p1Genocide, 0, IKUMI_GENOCIDE_MAX)) {
                guiState.localData.p1IkumiGenocide = p1Genocide;
            }
            
            // Quick set buttons
            if (ImGui::Button("Max Blood##p1")) {
                guiState.localData.p1IkumiBlood = IKUMI_BLOOD_MAX;
            }
            ImGui::SameLine();
            if (ImGui::Button("Min Blood##p1")) {
                guiState.localData.p1IkumiBlood = 0;
            }
            
            if (ImGui::Button("Max Genocide##p1")) {
                guiState.localData.p1IkumiGenocide = IKUMI_GENOCIDE_MAX;
            }
            ImGui::SameLine();
            if (ImGui::Button("Reset Genocide##p1")) {
                guiState.localData.p1IkumiGenocide = 0;
            }
        }
        // P1 Misuzu Settings
        else if (p1CharID == CHAR_ID_MISUZU) {
            // Feather Count
            int p1Feathers = guiState.localData.p1MisuzuFeathers;
            float p1FeatherPercent = (float)p1Feathers / MISUZU_FEATHER_MAX;
            
            ImGui::Text("Feather Count:");
            
            // Progress bar with blue color gradient
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, 
                ImVec4(0.3f, 0.5f, 0.8f + p1FeatherPercent * 0.2f, 1.0f));
            ImGui::ProgressBar(p1FeatherPercent, ImVec2(-1, 0), 
                              (std::to_string(p1Feathers) + "/" + std::to_string(MISUZU_FEATHER_MAX)).c_str());
            ImGui::PopStyleColor();
            
            // Slider
            if (ImGui::SliderInt("##P1Feathers", &p1Feathers, 0, MISUZU_FEATHER_MAX)) {
                guiState.localData.p1MisuzuFeathers = p1Feathers;
            }
            
            // Quick set buttons
            if (ImGui::Button("Max Feathers##p1")) {
                guiState.localData.p1MisuzuFeathers = MISUZU_FEATHER_MAX;
            }
            ImGui::SameLine();
            if (ImGui::Button("No Feathers##p1")) {
                guiState.localData.p1MisuzuFeathers = 0;
            }
        }
        else {
            ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "No special settings available");
        }
        
        // --- P2 COLUMN ---
        ImGui::NextColumn();
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.5f, 1.0f), "P2: %s", 
                          CharacterSettings::GetCharacterName(p2CharID).c_str());
        
        // P2 Ikumi Settings
        if (p2CharID == CHAR_ID_IKUMI) {
            // Blood Level
            int p2Blood = guiState.localData.p2IkumiBlood;
            float p2BloodPercent = (float)p2Blood / IKUMI_BLOOD_MAX;
            
            ImGui::Text("Blood Level:");
            
            // Progress bar with color gradient
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, 
                ImVec4(0.7f + p2BloodPercent * 0.3f, 0.3f - p2BloodPercent * 0.3f, 0.3f, 1.0f));
            ImGui::ProgressBar(p2BloodPercent, ImVec2(-1, 0), 
                              (std::to_string(p2Blood) + "/" + std::to_string(IKUMI_BLOOD_MAX)).c_str());
            ImGui::PopStyleColor();
            
            // Slider
            if (ImGui::SliderInt("##P2Blood", &p2Blood, 0, IKUMI_BLOOD_MAX)) {
                guiState.localData.p2IkumiBlood = p2Blood;
            }
            
            // Genocide Timer
            int p2Genocide = guiState.localData.p2IkumiGenocide;
            float genocideSeconds = p2Genocide / 60.0f;
            
            ImGui::Text("Genocide Timer: %.1f sec", genocideSeconds);
            if (ImGui::SliderInt("##P2Genocide", &p2Genocide, 0, IKUMI_GENOCIDE_MAX)) {
                guiState.localData.p2IkumiGenocide = p2Genocide;
            }
            
            // Quick set buttons
            if (ImGui::Button("Max Blood##p2")) {
                guiState.localData.p2IkumiBlood = IKUMI_BLOOD_MAX;
            }
            ImGui::SameLine();
            if (ImGui::Button("Min Blood##p2")) {
                guiState.localData.p2IkumiBlood = 0;
            }
            
            if (ImGui::Button("Max Genocide##p2")) {
                guiState.localData.p2IkumiGenocide = IKUMI_GENOCIDE_MAX;
            }
            ImGui::SameLine();
            if (ImGui::Button("Reset Genocide##p2")) {
                guiState.localData.p2IkumiGenocide = 0;
            }
        }
        // P2 Misuzu Settings
        else if (p2CharID == CHAR_ID_MISUZU) {
            // Feather Count
            int p2Feathers = guiState.localData.p2MisuzuFeathers;
            float p2FeatherPercent = (float)p2Feathers / MISUZU_FEATHER_MAX;
            
            ImGui::Text("Feather Count:");
            
            // Progress bar with blue color gradient
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, 
                ImVec4(0.3f, 0.5f, 0.8f + p2FeatherPercent * 0.2f, 1.0f));
            ImGui::ProgressBar(p2FeatherPercent, ImVec2(-1, 0), 
                              (std::to_string(p2Feathers) + "/" + std::to_string(MISUZU_FEATHER_MAX)).c_str());
            ImGui::PopStyleColor();
            
            // Slider
            if (ImGui::SliderInt("##P2Feathers", &p2Feathers, 0, MISUZU_FEATHER_MAX)) {
                guiState.localData.p2MisuzuFeathers = p2Feathers;
            }
            
            // Quick set buttons
            if (ImGui::Button("Max Feathers##p2")) {
                guiState.localData.p2MisuzuFeathers = MISUZU_FEATHER_MAX;
            }
            ImGui::SameLine();
            if (ImGui::Button("No Feathers##p2")) {
                guiState.localData.p2MisuzuFeathers = 0;
            }
        }
        else {
            ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "No special settings available");
        }
        
        // Reset columns
        ImGui::Columns(1);
        
        // Bottom help section
        ImGui::Separator();
        ImGui::TextWrapped(
            "Character-specific settings allow you to modify special parameters unique to each character.\n"
            "Currently supported characters: Ikumi (Blood Meter & Genocide Mode), Misuzu (Feather Count)");
    }
    
    // Add this new function to the ImGuiGui namespace:
    void RenderDebugInputTab() {
        bool showBorders = g_ShowOverlayDebugBorders.load();
        if (ImGui::Checkbox("Show overlay debug borders", &showBorders)) {
            g_ShowOverlayDebugBorders.store(showBorders);
        }
    ImGui::Separator();
        ImGui::Separator();
        ImGui::Text("Manual Input Override (P2)");
        ImGui::Separator();

        ImGui::Text("Directions:");
        if (ImGui::Button("UP")) { QueueMotionInput(2, MOTION_NONE, GAME_INPUT_UP); }
        ImGui::SameLine();
        if (ImGui::Button("DOWN")) { QueueMotionInput(2, MOTION_NONE, GAME_INPUT_DOWN); }
        ImGui::SameLine();
        if (ImGui::Button("LEFT")) { QueueMotionInput(2, MOTION_NONE, GAME_INPUT_LEFT); }
        ImGui::SameLine();
        if (ImGui::Button("RIGHT")) { QueueMotionInput(2, MOTION_NONE, GAME_INPUT_RIGHT); }

        ImGui::Separator();
        ImGui::Text("Attack Buttons (spams for 6 frames):");
        uintptr_t playerPtr = GetPlayerPointer(2);
        if (ImGui::Button("A")) {
            LogOut("[IMGUI] Debug menu: Pressed A for P2, playerPtr=0x" + std::to_string(playerPtr), true);
            if (playerPtr) {
                HoldButtonA(2);
                LogOut("[IMGUI] Called HoldButtonA for P2", true);
            } else {
                LogOut("[DEBUG_INPUT] Failed to get P2 pointer for A", true);
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("B")) {
            LogOut("[IMGUI] Debug menu: Pressed B for P2, playerPtr=0x" + std::to_string(playerPtr), true);
            if (playerPtr) {
                HoldButtonB(2);
                LogOut("[IMGUI] Called HoldButtonB for P2", true);
            } else {
                LogOut("[DEBUG_INPUT] Failed to get P2 pointer for B", true);
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("C")) {
            LogOut("[IMGUI] Debug menu: Pressed C for P2, playerPtr=0x" + std::to_string(playerPtr), true);
            if (playerPtr) {
                HoldButtonC(2);
                LogOut("[IMGUI] Called HoldButtonC for P2", true);
            } else {
                LogOut("[DEBUG_INPUT] Failed to get P2 pointer for C", true);
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("D")) {
            LogOut("[IMGUI] Debug menu: Pressed D for P2, playerPtr=0x" + std::to_string(playerPtr), true);
            if (playerPtr) {
                HoldButtonD(2);
                LogOut("[IMGUI] Called HoldButtonD for P2", true);
            } else {
                LogOut("[DEBUG_INPUT] Failed to get P2 pointer for D", true);
            }
        }

        ImGui::Separator();
        ImGui::Text("Direction + Button:");
        if (ImGui::Button("6A")) {
            QueueMotionInput(2, MOTION_NONE, GAME_INPUT_RIGHT | GAME_INPUT_A);
            LogOut("[IMGUI] Debug menu: Pressed 6A for P2, playerPtr=0x" + std::to_string(playerPtr), true);
            if (playerPtr) {
                HoldButtonA(2);
                LogOut("[IMGUI] Called HoldButtonA for P2 (6A)", true);
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("4B")) {
            QueueMotionInput(2, MOTION_NONE, GAME_INPUT_LEFT | GAME_INPUT_B);
            LogOut("[IMGUI] Debug menu: Pressed 4B for P2, playerPtr=0x" + std::to_string(playerPtr), true);
            if (playerPtr) {
                HoldButtonB(2);
                LogOut("[IMGUI] Called HoldButtonB for P2 (4B)", true);
            }
        }
        // ...add more as needed...

        ImGui::Separator();
        if (ImGui::Button("Release All Inputs")) { ReleaseInputs(2); }

        ImGui::Separator();
        ImGui::Text("BGM Control");
        uintptr_t efzBase = GetEFZBase();
        uintptr_t gameStatePtr = 0;
        if (SafeReadMemory(efzBase + EFZ_BASE_OFFSET_GAME_STATE, &gameStatePtr, sizeof(uintptr_t))) {
            /*if (ImGui::Button("Mute BGM")) {
                MuteBGM(gameStatePtr);
            }
            ImGui::SameLine();
            if (ImGui::Button("Unmute BGM")) {
                UnmuteBGM(gameStatePtr);
            }*/
            if (ImGui::Button("Stop BGM")) {
                StopBGM(gameStatePtr);
            }
            ImGui::SameLine();
            /*if (ImGui::Button("Log BGM State")) {
                LogBGMState(gameStatePtr);
            }*/
            static int bgmSlot = 0;
            ImGui::InputInt("Set BGM Slot (index)", &bgmSlot);
            if (ImGui::Button("Set BGM Slot")) {
                PlayBGM(gameStatePtr, static_cast<unsigned short>(bgmSlot));
            }
            ImGui::Text("Current BGM Slot: %d", GetBGMSlot(gameStatePtr));
            ImGui::Text("Current BGM Volume: %d", GetBGMVolume(gameStatePtr));
        } else {
            ImGui::Text("Game state pointer not available.");
        }
    }

    // Update the RenderGui function to include the new tab:
    void RenderGui() {
        if (!ImGuiImpl::IsVisible())
            return;

        // Refresh data once when UI becomes visible; avoid continuous auto-refresh to reduce work
        static bool lastVisible = false;
        bool currentVisible = ImGuiImpl::IsVisible();
        if (currentVisible && !lastVisible) {
            RefreshLocalData();
        }
        lastVisible = currentVisible;

    // Set window position and size
        ImGui::SetNextWindowPos(ImVec2(50, 50), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(580, 520), ImGuiCond_FirstUseEver);
    // Force fully-opaque background to avoid heavy alpha blending on low-end GPUs
    ImGui::SetNextWindowBgAlpha(1.0f);

        // Main window
    // Allow navigation (keyboard/gamepad) while keeping collapse disabled
    ImGuiWindowFlags winFlags = ImGuiWindowFlags_NoCollapse;
    if (ImGui::Begin("EFZ Training Mode", nullptr, winFlags)) {
            // Check if a specific tab has been requested
            if (guiState.requestedTab >= 0) {
                guiState.currentTab = guiState.requestedTab;
                guiState.requestedTab = -1; // Reset request
            }
            
            // Tab bar at the top
            if (ImGui::BeginTabBar("##Tabs", ImGuiTabBarFlags_None)) {
                // Game Values tab
                if (ImGui::BeginTabItem("Game Values")) {
                    guiState.currentTab = 0;
                    RenderGameValuesTab();
                    ImGui::EndTabItem();
                }
                
                // Auto Action tab
                if (ImGui::BeginTabItem("Auto Action")) {
                    guiState.currentTab = 1;
                    RenderAutoActionTab();
                    ImGui::EndTabItem();
                }

                // Settings tab (new)
                if (ImGui::BeginTabItem("Settings")) {
                    guiState.currentTab = 5;
                    ImGuiSettings::RenderSettingsTab();
                    ImGui::EndTabItem();
                }
                
                // Add Character tab unconditionally for now
                if (ImGui::BeginTabItem("Character")) {
                    guiState.currentTab = 2;
                    RenderCharacterTab();
                    ImGui::EndTabItem();
                }
                
                // Debug tab
                if (ImGui::BeginTabItem("Debug")) {
                    guiState.currentTab = 3;
                    RenderDebugInputTab();
                    ImGui::EndTabItem();
                }
                
                // Help tab
                if (ImGui::BeginTabItem("Help")) {
                    guiState.currentTab = 4;
                    RenderHelpTab();
                    ImGui::EndTabItem();
                }
                
                ImGui::EndTabBar();
            }

            // Add action buttons at the bottom
            ImGui::Separator();
            if (ImGui::Button("Apply", ImVec2(120, 0))) {
                ApplyImGuiSettings();
            }
            ImGui::SameLine();
            if (ImGui::Button("Refresh Values", ImVec2(120, 0))) {
                RefreshLocalData();
            }
            ImGui::SameLine();
            if (ImGui::Button("Exit", ImVec2(120, 0))) {
                ImGuiImpl::ToggleVisibility();
            }
        }
        ImGui::End();
    }

    // Add the helper function to determine if character tab should be shown
    bool ShouldShowCharacterSettings() {
        CharacterSettings::UpdateCharacterIDs(guiState.localData);
        
        // For now, just check for Ikumi since that's the only character with special settings
        bool p1IsIkumi = guiState.localData.p1CharID == CHAR_ID_IKUMI;
        bool p2IsIkumi = guiState.localData.p2CharID == CHAR_ID_IKUMI;
        
        return p1IsIkumi || p2IsIkumi;
    }

    // Update RefreshLocalData to include character-specific data
    void RefreshLocalData() {
        uintptr_t base = GetEFZBase();
        if (!base) {
            LogOut("[IMGUI] RefreshLocalData: Couldn't get base address", true);
            return;
        }

        // P1
        SafeReadMemory(ResolvePointer(base, EFZ_BASE_OFFSET_P1, HP_OFFSET), &guiState.localData.hp1, sizeof(int));
        SafeReadMemory(ResolvePointer(base, EFZ_BASE_OFFSET_P1, METER_OFFSET), &guiState.localData.meter1, sizeof(int));
        SafeReadMemory(ResolvePointer(base, EFZ_BASE_OFFSET_P1, RF_OFFSET), &guiState.localData.rf1, sizeof(double));
        SafeReadMemory(ResolvePointer(base, EFZ_BASE_OFFSET_P1, XPOS_OFFSET), &guiState.localData.x1, sizeof(double));
        SafeReadMemory(ResolvePointer(base, EFZ_BASE_OFFSET_P1, YPOS_OFFSET), &guiState.localData.y1, sizeof(double));
        
        // Read character name and ensure null-termination
        memset(guiState.localData.p1CharName, 0, sizeof(guiState.localData.p1CharName));
        SafeReadMemory(ResolvePointer(base, EFZ_BASE_OFFSET_P1, CHARACTER_NAME_OFFSET), 
                   guiState.localData.p1CharName, sizeof(guiState.localData.p1CharName) - 1);
        
        // P2
        SafeReadMemory(ResolvePointer(base, EFZ_BASE_OFFSET_P2, HP_OFFSET), &guiState.localData.hp2, sizeof(int));
        SafeReadMemory(ResolvePointer(base, EFZ_BASE_OFFSET_P2, METER_OFFSET), &guiState.localData.meter2, sizeof(int));
        SafeReadMemory(ResolvePointer(base, EFZ_BASE_OFFSET_P2, RF_OFFSET), &guiState.localData.rf2, sizeof(double));
        SafeReadMemory(ResolvePointer(base, EFZ_BASE_OFFSET_P2, XPOS_OFFSET), &guiState.localData.x2, sizeof(double));
        SafeReadMemory(ResolvePointer(base, EFZ_BASE_OFFSET_P2, YPOS_OFFSET), &guiState.localData.y2, sizeof(double));
        
        // Read character name and ensure null-termination
        memset(guiState.localData.p2CharName, 0, sizeof(guiState.localData.p2CharName));
        SafeReadMemory(ResolvePointer(base, EFZ_BASE_OFFSET_P2, CHARACTER_NAME_OFFSET), 
                   guiState.localData.p2CharName, sizeof(guiState.localData.p2CharName) - 1);

        // Log the character names we're reading
        LogOut("[IMGUI] Read character names: P1=" + std::string(guiState.localData.p1CharName) + 
           ", P2=" + std::string(guiState.localData.p2CharName), true);
    
        // Update character IDs
        CharacterSettings::UpdateCharacterIDs(guiState.localData);
    
        // Read character-specific values
        CharacterSettings::ReadCharacterValues(base, guiState.localData);

        // Read current IC color values from memory
        int p1ICValue = 0, p2ICValue = 0;
        SafeReadMemory(ResolvePointer(base, EFZ_BASE_OFFSET_P1, IC_COLOR_OFFSET), &p1ICValue, sizeof(int));
        SafeReadMemory(ResolvePointer(base, EFZ_BASE_OFFSET_P2, IC_COLOR_OFFSET), &p2ICValue, sizeof(int));
        
        // Update checkbox states based on current IC color values (1=Blue IC, 0=Red IC)
        guiState.localData.p1BlueIC = (p1ICValue == 1);
        guiState.localData.p2BlueIC = (p2ICValue == 1);

        LogOut("[IMGUI] Refreshed local data from game memory. IC Colors: P1=" + 
               std::to_string(p1ICValue) + ", P2=" + std::to_string(p2ICValue), true);

    // --- Sync auto-action and trigger settings from atomics into the GUI state ---
    // Master auto-action
    guiState.localData.autoAction = autoActionEnabled.load();
    guiState.localData.autoActionPlayer = autoActionPlayer.load();

    // Per-trigger enables
    guiState.localData.triggerAfterBlock   = triggerAfterBlockEnabled.load();
    guiState.localData.triggerOnWakeup     = triggerOnWakeupEnabled.load();
    guiState.localData.triggerAfterHitstun = triggerAfterHitstunEnabled.load();
    guiState.localData.triggerAfterAirtech = triggerAfterAirtechEnabled.load();

    // Per-trigger delays
    guiState.localData.delayAfterBlock     = triggerAfterBlockDelay.load();
    guiState.localData.delayOnWakeup       = triggerOnWakeupDelay.load();
    guiState.localData.delayAfterHitstun   = triggerAfterHitstunDelay.load();
    guiState.localData.delayAfterAirtech   = triggerAfterAirtechDelay.load();

    // Per-trigger actions
    guiState.localData.actionAfterBlock    = triggerAfterBlockAction.load();
    guiState.localData.actionOnWakeup      = triggerOnWakeupAction.load();
    guiState.localData.actionAfterHitstun  = triggerAfterHitstunAction.load();
    guiState.localData.actionAfterAirtech  = triggerAfterAirtechAction.load();

    // Per-trigger custom IDs
    guiState.localData.customAfterBlock    = triggerAfterBlockCustomID.load();
    guiState.localData.customOnWakeup      = triggerOnWakeupCustomID.load();
    guiState.localData.customAfterHitstun  = triggerAfterHitstunCustomID.load();
    guiState.localData.customAfterAirtech  = triggerAfterAirtechCustomID.load();

    // Per-trigger strengths
    guiState.localData.strengthAfterBlock    = triggerAfterBlockStrength.load();
    guiState.localData.strengthOnWakeup      = triggerOnWakeupStrength.load();
    guiState.localData.strengthAfterHitstun  = triggerAfterHitstunStrength.load();
    guiState.localData.strengthAfterAirtech  = triggerAfterAirtechStrength.load();
    }

    // Update ApplyImGuiSettings to include character-specific data
    void ApplyImGuiSettings() {
        if (g_featuresEnabled.load()) {
            LogOut("[IMGUI_GUI] Applying settings from ImGui interface", true);
            
            DisplayData updatedData = guiState.localData;
            displayData = updatedData;
            
            // Update atomic variables from our local copy
            autoAirtechEnabled.store(displayData.autoAirtech);
            autoAirtechDirection.store(displayData.airtechDirection);
            autoAirtechDelay.store(displayData.airtechDelay);
            autoJumpEnabled.store(displayData.autoJump);
            jumpDirection.store(displayData.jumpDirection);
            jumpTarget.store(displayData.jumpTarget);

            // Auto-action master settings
            autoActionEnabled.store(displayData.autoAction);
            autoActionPlayer.store(displayData.autoActionPlayer);

            // Per-trigger enables
            triggerAfterBlockEnabled.store(displayData.triggerAfterBlock);
            triggerOnWakeupEnabled.store(displayData.triggerOnWakeup);
            triggerAfterHitstunEnabled.store(displayData.triggerAfterHitstun);
            triggerAfterAirtechEnabled.store(displayData.triggerAfterAirtech);

            // Per-trigger delays
            triggerAfterBlockDelay.store(displayData.delayAfterBlock);
            triggerOnWakeupDelay.store(displayData.delayOnWakeup);
            triggerAfterHitstunDelay.store(displayData.delayAfterHitstun);
            triggerAfterAirtechDelay.store(displayData.delayAfterAirtech);

            // Per-trigger actions
            triggerAfterBlockAction.store(displayData.actionAfterBlock);
            triggerOnWakeupAction.store(displayData.actionOnWakeup);
            triggerAfterHitstunAction.store(displayData.actionAfterHitstun);
            triggerAfterAirtechAction.store(displayData.actionAfterAirtech);

            // Per-trigger custom IDs
            triggerAfterBlockCustomID.store(displayData.customAfterBlock);
            triggerOnWakeupCustomID.store(displayData.customOnWakeup);
            triggerAfterHitstunCustomID.store(displayData.customAfterHitstun);
            triggerAfterAirtechCustomID.store(displayData.customAfterAirtech);

            // Per-trigger strengths
            triggerAfterBlockStrength.store(displayData.strengthAfterBlock);
            triggerOnWakeupStrength.store(displayData.strengthOnWakeup);
            triggerAfterHitstunStrength.store(displayData.strengthAfterHitstun);
            triggerAfterAirtechStrength.store(displayData.strengthAfterAirtech);
            
            // Apply the P2 control patch based on the checkbox state
            if (displayData.p2ControlEnabled) {
                EnablePlayer2InPracticeMode();
            } else {
                // CORRECTED: Call the existing function to disable P2 control.
                DisablePlayer2InPracticeMode();
            }
            
            // Add this to log character settings being applied
            LogOut("[IMGUI_GUI] Applying character settings - Blood Mode: " + 
                   std::to_string(displayData.infiniteBloodMode) + 
                   ", Feather Mode: " + std::to_string(displayData.infiniteFeatherMode) +
                   ", P1 Blue IC: " + std::to_string(displayData.p1BlueIC) + 
                   ", P2 Blue IC: " + std::to_string(displayData.p2BlueIC), true);
            
            // Apply IC color settings directly
            SetICColorDirect(displayData.p1BlueIC, displayData.p2BlueIC);
            
            // Apply the settings to the game
            uintptr_t base = GetEFZBase();
            if (base) {
                ApplySettings(&displayData);
                // Refresh trigger overlay text to reflect new settings immediately
                UpdateTriggerOverlay();
            }
        }
    }
}