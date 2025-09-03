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
#include "../include/game/final_memory_patch.h"

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

        // Section: Character Data
    if (ImGui::CollapsingHeader("Character Data")) {
            // Two-column layout for P1/P2
            ImGui::Columns(2, "playerColumns", false);

            // P1 Column
            ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.0f, 1.0f), "Player 1 (%s)",
                guiState.localData.p1CharName[0] ? guiState.localData.p1CharName : "Unknown");
            ImGui::Separator();

            int hp1 = guiState.localData.hp1;
            if (ImGui::InputInt("P1 HP", &hp1)) {
                guiState.localData.hp1 = CLAMP(hp1, 0, MAX_HP);
            }

            int meter1 = guiState.localData.meter1;
            if (ImGui::InputInt("P1 Meter", &meter1)) {
                guiState.localData.meter1 = CLAMP(meter1, 0, MAX_METER);
            }

            float rf1 = (float)guiState.localData.rf1;
            if (ImGui::InputFloat("P1 RF", &rf1, 0.1f, 1.0f, "%.1f")) {
                guiState.localData.rf1 = CLAMP(rf1, 0.0f, MAX_RF);
            }

            ImGui::Checkbox("P1 Blue IC", &guiState.localData.p1BlueIC);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Checked = Blue IC (forced), Unchecked = Red IC (normal)\nApply changes to update the game");
            }

            float x1 = (float)guiState.localData.x1;
            float y1 = (float)guiState.localData.y1;
            if (ImGui::InputFloat("P1 X", &x1, 1.0f, 10.0f, "%.2f")) {
                guiState.localData.x1 = x1;
            }
            if (ImGui::InputFloat("P1 Y", &y1, 1.0f, 10.0f, "%.2f")) {
                guiState.localData.y1 = y1;
            }

            // P2 Column
            ImGui::NextColumn();
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.5f, 1.0f), "Player 2 (%s)",
                guiState.localData.p2CharName[0] ? guiState.localData.p2CharName : "Unknown");
            ImGui::Separator();

            int hp2 = guiState.localData.hp2;
            if (ImGui::InputInt("P2 HP", &hp2)) {
                guiState.localData.hp2 = CLAMP(hp2, 0, MAX_HP);
            }

            int meter2 = guiState.localData.meter2;
            if (ImGui::InputInt("P2 Meter", &meter2)) {
                guiState.localData.meter2 = CLAMP(meter2, 0, MAX_METER);
            }

            float rf2 = (float)guiState.localData.rf2;
            if (ImGui::InputFloat("P2 RF", &rf2, 0.1f, 1.0f, "%.1f")) {
                guiState.localData.rf2 = CLAMP(rf2, 0.0f, MAX_RF);
            }

            ImGui::Checkbox("P2 Blue IC", &guiState.localData.p2BlueIC);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Checked = Blue IC (forced), Unchecked = Red IC (normal)\nApply changes to update the game");
            }

            float x2 = (float)guiState.localData.x2;
            float y2 = (float)guiState.localData.y2;
            if (ImGui::InputFloat("P2 X", &x2, 1.0f, 10.0f, "%.2f")) {
                guiState.localData.x2 = x2;
            }
            if (ImGui::InputFloat("P2 Y", &y2, 1.0f, 10.0f, "%.2f")) {
                guiState.localData.y2 = y2;
            }

            ImGui::Columns(1);
        }

        ImGui::Separator();

        // Section: Player options (Auto-Airtech + Auto-Jump)
    if (ImGui::CollapsingHeader("Player Options")) {
            // Auto-Airtech
            ImGui::TextUnformatted("Auto-Airtech:");
            ImGui::SameLine();
            const char* airtechItems[] = { "Neutral (Disabled)", "Forward", "Backward" };
            int airtechDir = guiState.localData.autoAirtech ? guiState.localData.airtechDirection + 1 : 0;
            if (ImGui::Combo("##AirtechDir", &airtechDir, airtechItems, IM_ARRAYSIZE(airtechItems))) {
                guiState.localData.autoAirtech = (airtechDir > 0);
                guiState.localData.airtechDirection = airtechDir > 0 ? airtechDir - 1 : 0;
            }
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

            ImGui::Dummy(ImVec2(1, 6));

            // Auto-Jump
            bool aj = guiState.localData.autoJump;
            if (ImGui::Checkbox("Enable Auto-Jump", &aj)) {
                guiState.localData.autoJump = aj;
            }
            ImGui::SameLine();
            ImGui::TextUnformatted("Direction:");
            const char* jumpDirs[] = { "Neutral", "Forward", "Backward" };
            int jdir = guiState.localData.jumpDirection;
            ImGui::SameLine();
            if (ImGui::Combo("##JumpDir", &jdir, jumpDirs, IM_ARRAYSIZE(jumpDirs))) {
                guiState.localData.jumpDirection = (jdir < 0 ? 0 : (jdir > 2 ? 2 : jdir));
            }
            ImGui::SameLine();
            ImGui::TextUnformatted("Apply To:");
            const char* jumpTargets[] = { "P1 Only", "P2 Only", "Both Players" };
            int jtarget = guiState.localData.jumpTarget - 1; // 0..2
            ImGui::SameLine();
            if (ImGui::Combo("##JumpTarget", &jtarget, jumpTargets, IM_ARRAYSIZE(jumpTargets))) {
                guiState.localData.jumpTarget = (jtarget < 0 ? 1 : (jtarget > 2 ? 3 : jtarget + 1));
            }

            ImGui::Dummy(ImVec2(1, 8));
            ImGui::SeparatorText("Helpers");

            ImGui::Dummy(ImVec2(1, 4));
            // Position helpers
            if (ImGui::Button("Swap Positions", ImVec2(150, 30))) {
                std::swap(guiState.localData.x1, guiState.localData.x2);
            }
            ImGui::SameLine();
            if (ImGui::Button("Round Start", ImVec2(150, 30))) {
                guiState.localData.x1 = 240.0;
                guiState.localData.y1 = 0.0;
                guiState.localData.x2 = 400.0;
                guiState.localData.y2 = 0.0;
            }
        }

        // New Section: Game Settings
    if (ImGui::CollapsingHeader("Game Settings")) {
            // FM bypass toggle (applies immediately, reversible)
            bool fmBypass = IsFinalMemoryBypassEnabled();
            if (ImGui::Checkbox("Final Memory: Allow at any HP", &fmBypass)) {
                int changed = SetFinalMemoryBypass(fmBypass);
                LogOut(std::string("[IMGUI][FM] ") + (fmBypass ? "Enabled" : "Disabled") + " FM HP bypass.", true);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Removes the low-HP restriction for Final Memory for all characters.\nUncheck to restore the original threshold.");
            }

            ImGui::Dummy(ImVec2(1, 6));
            // P2 Control toggle moved here (applied on Apply)
            ImGui::PushItemWidth(-1);
            ImGui::Checkbox("Enable P2 Control (Practice Mode Only)", &guiState.localData.p2ControlEnabled);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Gives you direct control over Player 2 in Practice Mode.\nThis is required for the Debug Input tab to work.\nApply changes to update the game.");
            }
            ImGui::PopItemWidth();

            ImGui::Dummy(ImVec2(1, 6));
            ImGui::SeparatorText("Practice Dummy");
            // Auto-Block Mode (F7 superset)
            const char* abNames[] = { "None", "All (F7)", "First Hit (then off)", "After First Hit (then on)", "(deprecated)" };
            if (GetCurrentGameMode() == GameMode::Practice) {
                int abMode = GetDummyAutoBlockMode();
                ImGui::SetNextItemWidth(200);
                if (ImGui::Combo("Dummy Auto-Block", &abMode, abNames, 4)) { // only first 4 are valid now
                    SetDummyAutoBlockMode(abMode);
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Dummy block behavior:\n- None\n- All: always auto-block (vanilla F7)\n- First Hit: after a block, autoblock is disabled for a short cooldown\n- After First Hit: after you get hit, autoblock is enabled briefly to block the next hit");
                }
                ImGui::SameLine();
                bool adaptive = GetAdaptiveStanceEnabled();
                if (ImGui::Checkbox("Adaptive stance", &adaptive)) {
                    SetAdaptiveStanceEnabled(adaptive);
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Forces dummy stance each frame: stand vs airborne attacker, crouch vs grounded.");
                }
            } else {
                ImGui::BeginDisabled(); int dummyAB = 0; ImGui::Combo("Dummy Auto-Block", &dummyAB, abNames, 4); ImGui::EndDisabled();
            }

            // State (F6 equivalent): 0=Standing, 1=Jumping, 2=Crouching
            int mode = 0; bool modeOk = GetPracticeBlockMode(mode);
            const char* stateNames[] = { "Standing", "Jumping", "Crouching" };
            if (modeOk) {
                int mLocal = (mode < 0 ? 0 : (mode > 2 ? 2 : mode));
                if (ImGui::Combo("Dummy Stance (F6)", &mLocal, stateNames, IM_ARRAYSIZE(stateNames))) {
                    SetPracticeBlockMode(mLocal);
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Sets the dummy's stance: Standing, Jumping, or Crouching.\nWorks regardless of P2 control.");
                }
            } else {
                ImGui::BeginDisabled();
                int dummyState = 0; ImGui::Combo("Dummy Stance (F6)", &dummyState, stateNames, IM_ARRAYSIZE(stateNames));
                ImGui::EndDisabled();
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Available only in Practice Mode.");
                }
            }
        }

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

        // Mishio - Element controls and infinite modes
        if (p1CharID == CHAR_ID_MISHIO || p2CharID == CHAR_ID_MISHIO) {
            hasFeatures = true;

            bool infElem = guiState.localData.infiniteMishioElement;
            if (ImGui::Checkbox("Infinite Element (Mishio)", &infElem)) {
                guiState.localData.infiniteMishioElement = infElem;
            }
            ImGui::SameLine();
            bool infAw = guiState.localData.infiniteMishioAwakened;
            if (ImGui::Checkbox("Infinite Awakened Timer (Mishio)", &infAw)) {
                guiState.localData.infiniteMishioAwakened = infAw;
            }
            ImGui::SameLine();
            ImGui::TextDisabled("(?)");
            if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
                ImGui::TextUnformatted("Element: 0=None, 1=Fire, 2=Lightning, 3=Awakened.\n"
                                       "Infinite Element: keeps your selected element from being cleared.\n"
                                       "Infinite Awakened: while Awakened, the hidden timer is topped up.");
                ImGui::PopTextWrapPos();
                ImGui::EndTooltip();
            }
        }
        
    // IC Color override moved to Game Values tab; no IC controls in Character tab
        
        if (hasFeatures) {
            ImGui::Separator();
            
            // Debug info: enforcement is inline via FrameDataMonitor at ~16 Hz
            if (guiState.localData.infiniteBloodMode || guiState.localData.infiniteFeatherMode ||
                guiState.localData.infiniteMishioElement || guiState.localData.infiniteMishioAwakened ||
                guiState.localData.p1RumiInfiniteShinai || guiState.localData.p2RumiInfiniteShinai ||
                guiState.localData.p1RumiInfiniteKimchi || guiState.localData.p2RumiInfiniteKimchi) {
                ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "Enforcement: inline (~16 Hz)");
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
    // P1 Mishio Settings
        else if (p1CharID == CHAR_ID_MISHIO) {
            // Element selection
            int prevElem = guiState.localData.p1MishioElement;
            int elem = prevElem;
            const char* items[] = { "None", "Fire", "Lightning", "Awakened" };
            ImGui::Text("Element:");
            ImGui::Combo("##P1MishioElem", &elem, items, IM_ARRAYSIZE(items));
            guiState.localData.p1MishioElement = CLAMP(elem, MISHIO_ELEM_NONE, MISHIO_ELEM_AWAKENED);
            // If switched to Awakened and infinite timer is OFF, set timer to full (4500)
            if (elem != prevElem && elem == MISHIO_ELEM_AWAKENED && !guiState.localData.infiniteMishioAwakened) {
                guiState.localData.p1MishioAwakenedTimer = MISHIO_AWAKENED_TARGET;
            }

            // Awakened timer (only editable while Awakened)
            int aw = guiState.localData.p1MishioAwakenedTimer;
            ImGui::Text("Awakened Timer (internal frames):");
            bool p1Awakened = (guiState.localData.p1MishioElement == MISHIO_ELEM_AWAKENED);
            if (!p1Awakened) ImGui::BeginDisabled();
            if (ImGui::SliderInt("##P1MishioAw", &aw, 0, MISHIO_AWAKENED_TARGET)) {
                guiState.localData.p1MishioAwakenedTimer = aw;
            }
            if (!p1Awakened) {
                ImGui::EndDisabled();
                ImGui::SameLine();
                ImGui::TextDisabled("(set Element to Awakened to edit)");
            }
        }
    // P1 Akiko (Minase) Settings
        else if (p1CharID == CHAR_ID_AKIKO) {
            ImGui::Text("Bullet Cycle (shared A/B):");
            const char* cycleItems[] = {
                "0: A=Egg,     B=Tuna",
                "1: A=Carrot,  B=Radish",
                "2: A=Sardine, B=Duriah"
            };
            int bc1 = guiState.localData.p1AkikoBulletCycle;
            bc1 = (bc1 < 0) ? 0 : (bc1 > 2 ? 2 : bc1);
            if (ImGui::Combo("##P1AkikoBullet", &bc1, cycleItems, IM_ARRAYSIZE(cycleItems))) {
                guiState.localData.p1AkikoBulletCycle = bc1;
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Sequence advances on use. Value is shared across A and B: A then B yields Egg→Radish for 0, etc.");
            }
            bool freeze1 = guiState.localData.p1AkikoFreezeCycle;
            if (ImGui::Checkbox("Freeze bullet cycle##p1Akiko", &freeze1)) {
                guiState.localData.p1AkikoFreezeCycle = freeze1;
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Keeps the bullet cycle fixed at the selected value even after using the skill.");
            }
            bool showCH1 = guiState.localData.p1AkikoShowCleanHit;
            if (ImGui::Checkbox("Show Clean Hit helper##p1Akiko", &showCH1)) {
                guiState.localData.p1AkikoShowCleanHit = showCH1;
            }
            ImGui::Text("Time-Slow Trigger:");
            const char* tsItems1[] = { "Inactive", "A Version", "B Version", "C Version", "Infinite timer" };
            int ts1 = guiState.localData.p1AkikoTimeslowTrigger;
            ts1 = CLAMP(ts1, AKIKO_TIMESLOW_INACTIVE, AKIKO_TIMESLOW_INFINITE);
            if (ImGui::Combo("##P1AkikoTimeslow", &ts1, tsItems1, IM_ARRAYSIZE(tsItems1))) {
                guiState.localData.p1AkikoTimeslowTrigger = ts1;
            }
            ImGui::TextDisabled("(Akiko: bullet routes and clock-slow)");
        }

        // P1 Neyuki (Sleepy Nayuki) Settings
        else if (p1CharID == CHAR_ID_NAYUKI) {
            int jam = guiState.localData.p1NeyukiJamCount;
            jam = CLAMP(jam, 0, NEYUKI_JAM_COUNT_MAX);
            ImGui::Text("Jam Count:");
            if (ImGui::SliderInt("##P1NeyukiJam", &jam, 0, NEYUKI_JAM_COUNT_MAX)) {
                guiState.localData.p1NeyukiJamCount = jam;
            }
            ImGui::TextDisabled("(Neyuki only)");
        }

        // P1 Doppel (ExNanase) Settings
        else if (p1CharID == CHAR_ID_EXNANASE) {
            bool enlightened = guiState.localData.p1DoppelEnlightened;
            if (ImGui::Checkbox("Enlightened (Final Memory)##p1Doppel", &enlightened)) {
                guiState.localData.p1DoppelEnlightened = enlightened;
            }
            ImGui::TextDisabled("(sets internal flag to 1 when checked, 0 when unchecked)");
        }
    // P1 Nanase (Rumi) Settings
    else if (p1CharID == CHAR_ID_NANASE) {
            ImGui::Text("Rumi Mode:");
            // Keep combobox in sync with current state and disable when Infinite Shinai is on
            int modeIdx = guiState.localData.p1RumiBarehanded ? 1 : 0; // 0=Shinai, 1=Barehanded
            const char* rumiModes[] = { "Shinai", "Barehanded" };
            bool p1InfNow = guiState.localData.p1RumiInfiniteShinai;
            if (p1InfNow) {
                // While Infinite is on, force UI to show Shinai and keep local state Shinai
                modeIdx = 0;
                guiState.localData.p1RumiBarehanded = false;
                ImGui::BeginDisabled(true);
            }
            if (ImGui::Combo("##p1RumiMode", &modeIdx, rumiModes, IM_ARRAYSIZE(rumiModes))) {
                guiState.localData.p1RumiBarehanded = (modeIdx == 1);
            }
            if (p1InfNow) ImGui::EndDisabled();
            bool infShinai = guiState.localData.p1RumiInfiniteShinai;
            if (ImGui::Checkbox("Infinite Shinai (prevent dropping)##p1RumiInf", &infShinai)) {
                guiState.localData.p1RumiInfiniteShinai = infShinai;
                if (infShinai) {
                    // Override UI intention: always Shinai when Infinite is on
                    guiState.localData.p1RumiBarehanded = false;
                }
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("When enabled, keeps Shinai equipped by forcing mode back to Shinai after specials/supers that drop it. Only applies when mode is Shinai.");
            }
            ImGui::TextDisabled("(Mode swap writes anim/move pointers and syncs gate/mode; safer when idle)");

            ImGui::Separator();
            ImGui::Text("Final Memory (Kimchi):");
            bool kimchi = guiState.localData.p1RumiKimchiActive;
            if (ImGui::Checkbox("Active##p1Kimchi", &kimchi)) {
                guiState.localData.p1RumiKimchiActive = kimchi;
                if (kimchi && guiState.localData.p1RumiKimchiTimer < RUMI_KIMCHI_TARGET)
                    guiState.localData.p1RumiKimchiTimer = RUMI_KIMCHI_TARGET;
            }
            int kt = guiState.localData.p1RumiKimchiTimer;
            if (ImGui::SliderInt("Timer##p1Kimchi", &kt, 0, RUMI_KIMCHI_TARGET)) {
                guiState.localData.p1RumiKimchiTimer = kt;
            }
            bool infKimchi = guiState.localData.p1RumiInfiniteKimchi;
            if (ImGui::Checkbox("Infinite Kimchi (freeze timer)##p1Kimchi", &infKimchi)) {
                guiState.localData.p1RumiInfiniteKimchi = infKimchi;
                if (infKimchi) {
                    guiState.localData.p1RumiKimchiActive = true;
                    guiState.localData.p1RumiKimchiTimer = RUMI_KIMCHI_TARGET;
                }
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
    // P2 Mishio Settings
        else if (p2CharID == CHAR_ID_MISHIO) {
            int prevElem2 = guiState.localData.p2MishioElement;
            int elem = prevElem2;
            const char* items[] = { "None", "Fire", "Lightning", "Awakened" };
            ImGui::Text("Element:");
            ImGui::Combo("##P2MishioElem", &elem, items, IM_ARRAYSIZE(items));
            guiState.localData.p2MishioElement = CLAMP(elem, MISHIO_ELEM_NONE, MISHIO_ELEM_AWAKENED);
            if (elem != prevElem2 && elem == MISHIO_ELEM_AWAKENED && !guiState.localData.infiniteMishioAwakened) {
                guiState.localData.p2MishioAwakenedTimer = MISHIO_AWAKENED_TARGET;
            }

            int aw = guiState.localData.p2MishioAwakenedTimer;
            ImGui::Text("Awakened Timer (internal frames):");
            bool p2Awakened = (guiState.localData.p2MishioElement == MISHIO_ELEM_AWAKENED);
            if (!p2Awakened) ImGui::BeginDisabled();
            if (ImGui::SliderInt("##P2MishioAw", &aw, 0, MISHIO_AWAKENED_TARGET)) {
                guiState.localData.p2MishioAwakenedTimer = aw;
            }
            if (!p2Awakened) {
                ImGui::EndDisabled();
                ImGui::SameLine();
                ImGui::TextDisabled("(set Element to Awakened to edit)");
            }
        }
        // P2 Akiko (Minase) Settings
        else if (p2CharID == CHAR_ID_AKIKO) {
            ImGui::Text("Bullet Cycle (shared A/B):");
            const char* cycleItems2[] = {
                "0: A=Egg,     B=Tuna",
                "1: A=Carrot,  B=Radish",
                "2: A=Sardine, B=Duriah"
            };
            int bc2 = guiState.localData.p2AkikoBulletCycle;
            bc2 = (bc2 < 0) ? 0 : (bc2 > 2 ? 2 : bc2);
            if (ImGui::Combo("##P2AkikoBullet", &bc2, cycleItems2, IM_ARRAYSIZE(cycleItems2))) {
                guiState.localData.p2AkikoBulletCycle = bc2;
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Sequence advances on use. Value is shared across A and B: A then B yields Egg→Radish for 0, etc.");
            }
            bool freeze2 = guiState.localData.p2AkikoFreezeCycle;
            if (ImGui::Checkbox("Freeze bullet cycle##p2Akiko", &freeze2)) {
                guiState.localData.p2AkikoFreezeCycle = freeze2;
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Keeps the bullet cycle fixed at the selected value even after using the skill.");
            }
            bool showCH2 = guiState.localData.p2AkikoShowCleanHit;
            if (ImGui::Checkbox("Show Clean Hit helper##p2Akiko", &showCH2)) {
                guiState.localData.p2AkikoShowCleanHit = showCH2;
            }
            ImGui::Text("Time-Slow Trigger:");
            const char* tsItems2[] = { "Inactive", "A Version", "B Version", "C Version", "Infinite timer" };
            int ts2 = guiState.localData.p2AkikoTimeslowTrigger;
            ts2 = CLAMP(ts2, AKIKO_TIMESLOW_INACTIVE, AKIKO_TIMESLOW_INFINITE);
            if (ImGui::Combo("##P2AkikoTimeslow", &ts2, tsItems2, IM_ARRAYSIZE(tsItems2))) {
                guiState.localData.p2AkikoTimeslowTrigger = ts2;
            }
            ImGui::TextDisabled("(Akiko: bullet routes and clock-slow)");
        }

        // P2 Neyuki (Sleepy Nayuki) Settings
        else if (p2CharID == CHAR_ID_NAYUKI) {
            int jam2 = guiState.localData.p2NeyukiJamCount;
            jam2 = CLAMP(jam2, 0, NEYUKI_JAM_COUNT_MAX);
            ImGui::Text("Jam Count:");
            if (ImGui::SliderInt("##P2NeyukiJam", &jam2, 0, NEYUKI_JAM_COUNT_MAX)) {
                guiState.localData.p2NeyukiJamCount = jam2;
            }
            ImGui::TextDisabled("(Neyuki only)");
        }

        // P2 Doppel (ExNanase) Settings
        else if (p2CharID == CHAR_ID_EXNANASE) {
            bool enlightened2 = guiState.localData.p2DoppelEnlightened;
            if (ImGui::Checkbox("Enlightened (Final Memory)##p2Doppel", &enlightened2)) {
                guiState.localData.p2DoppelEnlightened = enlightened2;
            }
            ImGui::TextDisabled("(sets internal flag to 1 when checked, 0 when unchecked)");
        }
    // P2 Nanase (Rumi) Settings
    else if (p2CharID == CHAR_ID_NANASE) {
            ImGui::Text("Rumi Mode:");
            // Keep combobox in sync with current state and disable when Infinite Shinai is on
            int modeIdx2 = guiState.localData.p2RumiBarehanded ? 1 : 0; // 0=Shinai, 1=Barehanded
            const char* rumiModes[] = { "Shinai", "Barehanded" };
            bool p2InfNow = guiState.localData.p2RumiInfiniteShinai;
            if (p2InfNow) {
                // While Infinite is on, force UI to show Shinai and keep local state Shinai
                modeIdx2 = 0;
                guiState.localData.p2RumiBarehanded = false;
                ImGui::BeginDisabled(true);
            }
            if (ImGui::Combo("##p2RumiMode", &modeIdx2, rumiModes, IM_ARRAYSIZE(rumiModes))) {
                guiState.localData.p2RumiBarehanded = (modeIdx2 == 1);
            }
            if (p2InfNow) ImGui::EndDisabled();
            bool infShinai2 = guiState.localData.p2RumiInfiniteShinai;
            if (ImGui::Checkbox("Infinite Shinai (prevent dropping)##p2RumiInf", &infShinai2)) {
                guiState.localData.p2RumiInfiniteShinai = infShinai2;
                if (infShinai2) {
                    guiState.localData.p2RumiBarehanded = false;
                }
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("When enabled, keeps Shinai equipped by forcing mode back to Shinai after specials/supers that drop it. Only applies when mode is Shinai.");
            }
            ImGui::TextDisabled("(Mode swap writes anim/move pointers and syncs gate/mode; safer when idle)");

            ImGui::Separator();
            ImGui::Text("Final Memory (Kimchi):");
            bool kimchi2 = guiState.localData.p2RumiKimchiActive;
            if (ImGui::Checkbox("Active##p2Kimchi", &kimchi2)) {
                guiState.localData.p2RumiKimchiActive = kimchi2;
                if (kimchi2 && guiState.localData.p2RumiKimchiTimer < RUMI_KIMCHI_TARGET)
                    guiState.localData.p2RumiKimchiTimer = RUMI_KIMCHI_TARGET;
            }
            int kt2 = guiState.localData.p2RumiKimchiTimer;
            if (ImGui::SliderInt("Timer##p2Kimchi", &kt2, 0, RUMI_KIMCHI_TARGET)) {
                guiState.localData.p2RumiKimchiTimer = kt2;
            }
            bool infKimchi2 = guiState.localData.p2RumiInfiniteKimchi;
            if (ImGui::Checkbox("Infinite Kimchi (freeze timer)##p2Kimchi", &infKimchi2)) {
                guiState.localData.p2RumiInfiniteKimchi = infKimchi2;
                if (infKimchi2) {
                    guiState.localData.p2RumiKimchiActive = true;
                    guiState.localData.p2RumiKimchiTimer = RUMI_KIMCHI_TARGET;
                }
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
            "Currently supported: Ikumi (Blood/Genocide), Misuzu (Feathers), Mishio (Element/Awakened), Rumi (Stance, Kimchi), Akiko (Bullet Cycle/Time-Slow), Neyuki (Jam Count 0-9)");
    }
    
    // Add this new function to the ImGuiGui namespace:
    void RenderDebugInputTab() {
        bool showBorders = g_ShowOverlayDebugBorders.load();
        if (ImGui::Checkbox("Show overlay debug borders", &showBorders)) {
            g_ShowOverlayDebugBorders.store(showBorders);
        }
        ImGui::Separator();
        // Final Memory (FM) tools
        ImGui::Text("Final Memory Tools:");
        if (ImGui::Button("Apply FM HP bypass (allow FM at any HP)")) {
            // Call runtime patcher once; log summary only
            static uint64_t s_lastPatchLogTick = 0;
            int sites = 0;
            try {
                sites = ::ApplyFinalMemoryHPBypass();
            } catch (...) {
                LogOut("[IMGUI][FM] Exception while applying FM bypass.", true);
            }
            uint64_t now = GetTickCount64();
            if (now - s_lastPatchLogTick > 2000) { // throttle to 2s
                LogOut(std::string("[IMGUI][FM] FM HP bypass applied. Sites patched: ") + std::to_string(sites), true);
                s_lastPatchLogTick = now;
            }
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
        // Use Appearing so the menu always resets to a visible spot when reopened (prevents off-screen in fullscreen)
        ImGui::SetNextWindowPos(ImVec2(50, 50), ImGuiCond_Appearing);
    ImGui::SetNextWindowSize(ImVec2(520, 460), ImGuiCond_FirstUseEver);
    // Force fully-opaque background to avoid heavy alpha blending on low-end GPUs
    ImGui::SetNextWindowBgAlpha(1.0f);

        // Main window
        // Allow navigation (keyboard/gamepad), disable collapse and saved settings to avoid off-screen positions
        ImGuiWindowFlags winFlags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings;
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
                
                // Add Character tab; refresh character IDs once on open to avoid per-frame work
                if (ImGui::BeginTabItem("Character")) {
                    guiState.currentTab = 2;
                    static bool s_charTabJustOpened = false;
                    if (ImGui::IsItemActivated()) { s_charTabJustOpened = true; }
                    if (s_charTabJustOpened) {
                        // Update IDs once when entering the tab
                    // Character IDs are updated on entering the tab or on manual refresh
                        s_charTabJustOpened = false;
                    }
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

        // Hard gate: don't dereference player pointers until characters are initialized
        if (!AreCharactersInitialized()) {
            LogOut("[IMGUI] RefreshLocalData: Characters not initialized; skipping memory reads", true);
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
        
          // Update character IDs once per refresh
          CharacterSettings::UpdateCharacterIDs(guiState.localData);
    
    // Read character-specific values (once per refresh)
    // Read character-specific values; Rumi path only reads mode/gate and is safe
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
            // Normalize Rumi intent: Infinite Shinai overrides to Shinai mode
            if (updatedData.p1RumiInfiniteShinai) updatedData.p1RumiBarehanded = false;
            if (updatedData.p2RumiInfiniteShinai) updatedData.p2RumiBarehanded = false;
        // If Infinite Kimchi selected, ensure active and timer full on apply
        if (updatedData.p1RumiInfiniteKimchi) { updatedData.p1RumiKimchiActive = true; updatedData.p1RumiKimchiTimer = RUMI_KIMCHI_TARGET; }
        if (updatedData.p2RumiInfiniteKimchi) { updatedData.p2RumiKimchiActive = true; updatedData.p2RumiKimchiTimer = RUMI_KIMCHI_TARGET; }
            displayData = updatedData;

            // Ensure Akiko Clean Hit helper flags propagate immediately
         displayData.p1AkikoShowCleanHit = updatedData.p1AkikoShowCleanHit;
         displayData.p2AkikoShowCleanHit = updatedData.p2AkikoShowCleanHit;
         LogOut("[IMGUI_GUI] CleanHit flags applied: P1=" + std::to_string((int)displayData.p1AkikoShowCleanHit) +
             " P2=" + std::to_string((int)displayData.p2AkikoShowCleanHit), true);

            // Ensure new Rumi flags are preserved
            displayData.p1RumiInfiniteShinai = updatedData.p1RumiInfiniteShinai;
            displayData.p2RumiInfiniteShinai = updatedData.p2RumiInfiniteShinai;
            
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
            
            // Enforce FM bypass state to match UI selection (idempotent)
            // We read current enabled state from the runtime and reapply to ensure consistency
            SetFinalMemoryBypass(IsFinalMemoryBypassEnabled());
            
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
                   ", Mishio Elem Inf: " + std::to_string(displayData.infiniteMishioElement) +
                   ", Mishio Awakened Inf: " + std::to_string(displayData.infiniteMishioAwakened) +
                   ", P1 Blue IC: " + std::to_string(displayData.p1BlueIC) + 
                   ", P2 Blue IC: " + std::to_string(displayData.p2BlueIC), true);
            
            // Apply IC color settings directly
            SetICColorDirect(displayData.p1BlueIC, displayData.p2BlueIC);
            
            // Apply the settings to the game
            uintptr_t base = GetEFZBase();
            if (base) {
                // Defer Rumi mode apply if not actionable to avoid unsafe engine calls
                bool deferred = false;
                if (displayData.p1CharID == CHAR_ID_NANASE) {
                    short mv = 0; if (auto mvAddr = ResolvePointer(base, EFZ_BASE_OFFSET_P1, MOVE_ID_OFFSET)) SafeReadMemory(mvAddr, &mv, sizeof(short));
                    if (!IsActionable(mv)) deferred = true;
                }
                if (displayData.p2CharID == CHAR_ID_NANASE) {
                    short mv = 0; if (auto mvAddr = ResolvePointer(base, EFZ_BASE_OFFSET_P2, MOVE_ID_OFFSET)) SafeReadMemory(mvAddr, &mv, sizeof(short));
                    if (!IsActionable(mv)) deferred = true;
                }
                ApplySettings(&displayData);
                // Run one enforcement tick immediately so infinite toggles take effect without waiting for the next cadence
                CharacterSettings::TickCharacterEnforcements(base, displayData);
                if (deferred) {
                    LogOut("[IMGUI] Rumi mode change deferred; apply again when idle.", true);
                }
                // Refresh trigger overlay text to reflect new settings immediately
                UpdateTriggerOverlay();
            }
        }
    }
}