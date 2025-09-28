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
#include <vector>
// Removed <xinput.h> include: this translation unit no longer uses direct XInput
// symbols (controller footer mappings were stripped). Keeping the include caused
// stale compile diagnostics referencing XINPUT_* despite the code being removed.
// Other modules (input_handler.cpp, imgui_impl.cpp, overlay.cpp) still handle
// XInput polling centrally.
// For opening links from Help tab
#include <shellapi.h>
#pragma comment(lib, "shell32.lib")
#include "../include/gui/gif_player.h"
// Forward declare SpamAttackButton so we can use it in this file
extern void SpamAttackButton(uintptr_t playerBase, uint8_t button, int frames, const char* buttonName);
#include "../include/game/practice_patch.h"
#include "../include/gui/imgui_settings.h"
#include "../include/game/final_memory_patch.h"
#include "../include/game/fm_commands.h"
// Always RG control
#include "../include/game/always_rg.h"
// Switch players
#include "../include/utils/switch_players.h"
#include "../include/game/macro_controller.h"
#include "../include/utils/pause_integration.h"
#include "../include/game/practice_offsets.h"

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
    ACTION_SUPER2,      // 14 = 214236 Hybrid (replaces removed 63214)
    ACTION_236236,      // 15 = 236236 (Double QCF)
    ACTION_214214,      // 16 = 214214 (Double QCB)
    ACTION_JUMP,        // 17 = Jump
    ACTION_BACKDASH,    // 18 = Backdash
    ACTION_FORWARD_DASH,// 19 = Forward Dash
    ACTION_BLOCK,       // 20 = Block
    ACTION_FINAL_MEMORY,// 21 = Final Memory (per-character)
    ACTION_641236,      // 22 = 641236 Super
    ACTION_463214,      // 23 = 463214 Reverse Roll
    ACTION_412,         // 24 = 412 Partial Roll
    ACTION_22,          // 25 = 22 Down-Down
    ACTION_4123641236,  // 26 = 4123641236 Double Roll
    ACTION_6321463214   // 27 = 6321463214 Extended Pretzel
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
            ImGui::SeparatorText("Recoil Guard");
            // Always RG toggle (Practice Match only; enforcement lives in FrameDataMonitor)
            bool alwaysRG = AlwaysRG::IsEnabled();
            if (ImGui::Checkbox("Always Recoil Guard (Practice)", &alwaysRG)) {
                AlwaysRG::SetEnabled(alwaysRG);
                LogOut(std::string("[IMGUI][AlwaysRG] ") + (alwaysRG ? "Enabled" : "Disabled"), true);
                if (g_ShowRGDebugToasts.load()) {
                    DirectDrawHook::AddMessage(std::string("Always RG: ") + (alwaysRG ? "ON" : "OFF"), "ALWAYS_RG", RGB(200,220,255), 1500, 12, 72);
                }
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Keeps the dummy's RG armed by writing [P2+0x334]=0x3C each frame.\n"
                                  "Active only during Practice -> Match. Does not force blocking; normal rules apply.");
            }

            // Counter RG fast-restore toggle
            bool crg = g_counterRGEnabled.load();
            if (ImGui::Checkbox("Enable Counter RG (fast restore)", &crg)) {
                g_counterRGEnabled.store(crg);
                LogOut(std::string("[IMGUI] Counter RG: ") + (crg ? "ENABLED" : "DISABLED"), true);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("When enabled: on On RG specials, returns P2 control as soon as safe (opponent attack frames or our move starts).\nWhen disabled: restores control only after move completes or timeout.");
            }

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

        // Wake buffering toggle (debug): pre-buffer wake specials/dashes vs frame1 inject
        bool wakeBuf = g_wakeBufferingEnabled.load();
        if (ImGui::Checkbox("Pre-buffer wake specials/dashes", &wakeBuf)) {
            g_wakeBufferingEnabled.store(wakeBuf);
            LogOut(std::string("[IMGUI] Wake buffering mode: ") + (wakeBuf ? "BUFFERED (early freeze)" : "FRAME1 (no early freeze)"), true);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("When enabled: wake specials/supers/dashes are buffered early (original behavior).\nWhen disabled: they execute on the first actionable wake frame (no early motion freeze).\nHelps test strict wake timing.");
        }

        // Counter RG toggle moved to Game Settings
        
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
            int* macroSlot; // NEW: Per-trigger macro selection (0=None, 1..Max)
        };
        
        // Define an array of trigger settings
                TriggerSettings triggers[] = {
                        { "After Block", &guiState.localData.triggerAfterBlock, &guiState.localData.actionAfterBlock, 
                            &guiState.localData.delayAfterBlock, &guiState.localData.strengthAfterBlock, &guiState.localData.customAfterBlock, &guiState.localData.macroSlotAfterBlock },
                        { "On Wakeup", &guiState.localData.triggerOnWakeup, &guiState.localData.actionOnWakeup, 
                            &guiState.localData.delayOnWakeup, &guiState.localData.strengthOnWakeup, &guiState.localData.customOnWakeup, &guiState.localData.macroSlotOnWakeup },
                        { "After Hitstun", &guiState.localData.triggerAfterHitstun, &guiState.localData.actionAfterHitstun, 
                            &guiState.localData.delayAfterHitstun, &guiState.localData.strengthAfterHitstun, &guiState.localData.customAfterHitstun, &guiState.localData.macroSlotAfterHitstun },
                        { "After Airtech", &guiState.localData.triggerAfterAirtech, &guiState.localData.actionAfterAirtech, 
                            &guiState.localData.delayAfterAirtech, &guiState.localData.strengthAfterAirtech, &guiState.localData.customAfterAirtech, &guiState.localData.macroSlotAfterAirtech },
                        { "On RG", &guiState.localData.triggerOnRG, &guiState.localData.actionOnRG,
                            &guiState.localData.delayOnRG, &guiState.localData.strengthOnRG, &guiState.localData.customOnRG, &guiState.localData.macroSlotOnRG }
                };
        
        // Motion list (includes directions/stances plus motions and utility actions); we'll append dynamic Macro entries per slot below.
        const char* motionItems[] = {
            "Standing", "Crouching", "Jumping",
            "236 (QCF)", "623 (DP)", "214 (QCB)", "421 (Half-circle Down)",
            "41236 (HCF)", "214236 (Hybrid)", "236236 (Double QCF)", "214214 (Double QCB)",
            "641236", "463214", "412", "22", "4123641236", "6321463214",
            "Jump", "Backdash", "Forward Dash", "Block", "Final Memory"
        }; // NOTE: mapping functions below must stay in sync

        // Compute a compact width that fits the longest action label (plus arrow/padding), so combos aren't overly wide
        ImGuiStyle& _style = ImGui::GetStyle();
        // Keep combobox compact: make it just wide enough for "Final Memory" (longest we care to fully show)
        const float _labelFinalMemory = ImGui::CalcTextSize("Final Memory").x;
        const float _labelMacro = ImGui::CalcTextSize("Macro").x;
        const float _baseline = (std::max)(_labelFinalMemory, _labelMacro);
        // Add room for the combo arrow (roughly frame height), frame padding, and small breathing space
        const float actionComboWidth = _baseline + ImGui::GetFrameHeight() + _style.FramePadding.x * 3.0f + _style.ItemInnerSpacing.x;

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
                   action == ACTION_236236 || action == ACTION_214214 || action == ACTION_641236 ||
                   action == ACTION_463214 || action == ACTION_412 || action == ACTION_22 ||
                   action == ACTION_4123641236 || action == ACTION_6321463214;
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
                case ACTION_QCF: return 3;            // 236
                case ACTION_DP: return 4;             // 623
                case ACTION_QCB: return 5;            // 214
                case ACTION_421: return 6;            // 421
                case ACTION_SUPER1: return 7;         // 41236
                case ACTION_SUPER2: return 8;         // 214236 hybrid
                case ACTION_236236: return 9;         // 236236
                case ACTION_214214: return 10;        // 214214
                case ACTION_641236: return 11;        // 641236
                case ACTION_463214: return 12;        // 463214
                case ACTION_412: return 13;           // 412
                case ACTION_22: return 14;            // 22
                case ACTION_4123641236: return 15;    // 4123641236
                case ACTION_6321463214: return 16;    // 6321463214
                case ACTION_JUMP: return 17;          // Jump
                case ACTION_BACKDASH: return 18;      // Backdash
                case ACTION_FORWARD_DASH: return 19;  // Forward Dash
                case ACTION_BLOCK: return 20;         // Block
                case ACTION_FINAL_MEMORY: return 21;  // Final Memory
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
                case 3: return ACTION_QCF;             // 236
                case 4: return ACTION_DP;              // 623
                case 5: return ACTION_QCB;             // 214
                case 6: return ACTION_421;             // 421
                case 7: return ACTION_SUPER1;          // 41236
                case 8: return ACTION_SUPER2;          // 214236 hybrid
                case 9: return ACTION_236236;          // 236236
                case 10: return ACTION_214214;         // 214214
                case 11: return ACTION_641236;         // 641236
                case 12: return ACTION_463214;         // 463214
                case 13: return ACTION_412;            // 412
                case 14: return ACTION_22;             // 22
                case 15: return ACTION_4123641236;     // 4123641236
                case 16: return ACTION_6321463214;     // 6321463214
                case 17: return ACTION_JUMP;           // Jump
                case 18: return ACTION_BACKDASH;       // Backdash
                case 19: return ACTION_FORWARD_DASH;   // Forward Dash
                case 20: return ACTION_BLOCK;          // Block
                case 21: return ACTION_FINAL_MEMORY;   // Final Memory
                default: return ACTION_5A; // For posture indices 0..2, action set later
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
            ImGui::SetNextItemWidth(actionComboWidth);
            // Build list: base motions + single Macro entry
            std::vector<const char*> items;
            items.reserve(IM_ARRAYSIZE(motionItems) + 1);
            for (int k = 0; k < IM_ARRAYSIZE(motionItems); ++k) items.push_back(motionItems[k]);
            const int macroIndex = (int)items.size();
            items.push_back("Macro");
            // Determine current selection index
            int motionIndex = (*triggers[i].macroSlot > 0) ? macroIndex : GetMotionIndexForAction(*triggers[i].action);
            if (ImGui::Combo("Action", &motionIndex, items.data(), (int)items.size())) { // primary selector
                if (motionIndex == macroIndex) {
                    // Macro chosen: ensure we have a default slot if none selected yet
                    int slots = MacroController::GetSlotCount();
                    if (*triggers[i].macroSlot == 0 && slots > 0) {
                        *triggers[i].macroSlot = 1;
                    }
                } else {
                    // Non-macro: clear macro and set action
                    *triggers[i].macroSlot = 0;
                    if (motionIndex <= 2) {
                        // Posture selected: use current button choice to pick specific normal
                        int currentButtonIdx = 0;
                        if (IsNormalAttackAction(*triggers[i].action)) {
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
                        *triggers[i].action = MapMotionIndexToAction(motionIndex);
                    }
                }
            }

            // Separate Button/Direction combo: for Jump show Forward/Neutral/Backwards; hide for Dashes
            ImGui::SameLine();
            ImGui::SetNextItemWidth(90);
            int buttonIdx = 0;
            int postureIdx = GetPostureIndexForAction(*triggers[i].action);
            bool macroSelected = (*triggers[i].macroSlot > 0);
            if (macroSelected) {
                // Render slot selector instead of button/direction
                int slots = MacroController::GetSlotCount();
                int zeroBased = (*triggers[i].macroSlot > 0) ? (*triggers[i].macroSlot - 1) : 0;
                // Build simple labels: Slot 1..N
                std::vector<std::string> labels; labels.reserve((size_t)slots);
                for (int s = 1; s <= slots; ++s) labels.emplace_back(std::string("Slot ") + std::to_string(s));
                std::vector<const char*> citems; citems.reserve(labels.size());
                for (auto &s : labels) citems.push_back(s.c_str());
                ImGui::TextUnformatted("Slot");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(110);
                if (slots <= 0) {
                    ImGui::BeginDisabled();
                    int dummy = 0; ImGui::Combo("##MacroSlot", &dummy, (const char* const*)nullptr, 0);
                    ImGui::EndDisabled();
                } else {
                    if (ImGui::Combo("##MacroSlot", &zeroBased, citems.data(), (int)citems.size())) {
                        *triggers[i].macroSlot = zeroBased + 1;
                    }
                }
            } else if (*triggers[i].action == ACTION_JUMP) {
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
                if (*triggers[i].action != ACTION_BLOCK) {
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
            
            // Custom action removed; no custom ID field rendered
            
            ImGui::PopID();
        }
    }

    // Help Tab implementation
    void RenderHelpTab() {
        // Wrap help content in a scrollable child so keyboard/gamepad nav can scroll it
        ImGuiWindowFlags helpFlags = ImGuiWindowFlags_NoSavedSettings;
        ImVec2 avail = ImGui::GetContentRegionAvail();
        if (ImGui::BeginChild("##HelpScroll", ImVec2(avail.x, avail.y), true, helpFlags)) {
            // Ensure the child can be focused for keyboard/gamepad scrolling
            if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows)) {
                ImGui::SetItemDefaultFocus();
            }
            // Small hint so users know how to scroll without a mouse
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 0.8f, 0.85f));
            ImGui::TextDisabled("Hint: Use Up/Down/PageUp/PageDown or D-Pad to scroll this Help.");
            ImGui::PopStyleColor();
            // Provide explicit key/gamepad scrolling when the child has focus
            if (ImGui::IsWindowFocused()) {
                ImGuiIO& io = ImGui::GetIO();
                const float line = ImGui::GetTextLineHeightWithSpacing();
                const float page = ImGui::GetWindowHeight() * 0.85f;
                float scroll = 0.0f;
                if (ImGui::IsKeyDown(ImGuiKey_DownArrow)) scroll += line;
                if (ImGui::IsKeyDown(ImGuiKey_UpArrow)) scroll -= line;
                if (ImGui::IsKeyPressed(ImGuiKey_PageDown)) scroll += page;
                if (ImGui::IsKeyPressed(ImGuiKey_PageUp)) scroll -= page;
                // Gamepad DPAD
                if (ImGui::IsKeyDown(ImGuiKey_GamepadDpadDown)) scroll += line * 1.5f;
                if (ImGui::IsKeyDown(ImGuiKey_GamepadDpadUp)) scroll -= line * 1.5f;
                if (scroll != 0.0f) {
                    ImGui::SetScrollY(ImGui::GetScrollY() + scroll);
                }
            }
    ImGui::TextUnformatted("Hotkeys (can be changed in config.ini):");
    ImGui::Separator();

        const Config::Settings& cfg = Config::GetSettings();

        ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "Core Hotkeys:");
    ImGui::BulletText("Open/Close Menu: 3/%s (Gamepad: %s)", GetKeyName(cfg.toggleImGuiKey).c_str(), Config::GetGamepadButtonName(cfg.gpToggleMenuButton).c_str());
    ImGui::BulletText("Load Position: %s (Gamepad: %s)", GetKeyName(cfg.teleportKey).c_str(), Config::GetGamepadButtonName(cfg.gpTeleportButton).c_str());
    ImGui::BulletText("Save Position: %s (Gamepad: %s)", GetKeyName(cfg.recordKey).c_str(), Config::GetGamepadButtonName(cfg.gpSavePositionButton).c_str());
    ImGui::BulletText("Toggle Stats Display: %s", GetKeyName(cfg.toggleTitleKey).c_str());
        //ImGui::BulletText("Reset Frame Counter: %s", GetKeyName(cfg.resetFrameCounterKey).c_str());
        //ImGui::BulletText("Show This Help Screen: %s", GetKeyName(cfg.helpKey).c_str());

        ImGui::Separator();
    ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "Positioning Hotkeys (Hold Load: Keyboard=%s, Gamepad=%s, A/B/C/D = your attack keys):", GetKeyName(cfg.teleportKey).c_str(), Config::GetGamepadButtonName(cfg.gpTeleportButton).c_str());
    ImGui::BulletText("Center Players: %s + DOWN (Gamepad: %s + D-PAD DOWN)", GetKeyName(cfg.teleportKey).c_str(), Config::GetGamepadButtonName(cfg.gpTeleportButton).c_str());
    ImGui::BulletText("Players to Left Corner: %s + LEFT (Gamepad: %s + D-PAD LEFT)", GetKeyName(cfg.teleportKey).c_str(), Config::GetGamepadButtonName(cfg.gpTeleportButton).c_str());
    ImGui::BulletText("Players to Right Corner: %s + RIGHT (Gamepad: %s + D-PAD RIGHT)", GetKeyName(cfg.teleportKey).c_str(), Config::GetGamepadButtonName(cfg.gpTeleportButton).c_str());
    ImGui::BulletText("Round Start Positions: %s + DOWN + A (keyboard only)", GetKeyName(cfg.teleportKey).c_str());
    ImGui::BulletText("Swap Player Positions: %s + D (keyboard only)", GetKeyName(cfg.teleportKey).c_str());

    
    ImGui::Separator();
    ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.8f, 1.0f), "Practice/Macros:");
    ImGui::BulletText("Switch Players (Practice): %s", GetKeyName(cfg.switchPlayersKey).c_str());
    ImGui::BulletText("Macro Record (two-press): %s", GetKeyName(cfg.macroRecordKey).c_str());
    ImGui::BulletText("Macro Play: %s", GetKeyName(cfg.macroPlayKey).c_str());
    ImGui::BulletText("Macro Next Slot: %s", GetKeyName(cfg.macroSlotKey).c_str());

        ImGui::Dummy(ImVec2(1, 6));
        ImGui::SeparatorText("Macros – how they work");
        ImGui::TextWrapped(
            "Macros let you record inputs and play them back on the dummy with engine-accurate timing. \n"
            "They record both immediate inputs (buttons) and the real input buffer (directions/motions).\n"
        );
        ImGui::BulletText("Slots: Macros are stored in numbered slots. Use '%s' to cycle slots.", GetKeyName(cfg.macroSlotKey).c_str());
        ImGui::BulletText("Record: Press '%s' to arm, then press it again to start recording. Press again to stop.", GetKeyName(cfg.macroRecordKey).c_str());
        ImGui::BulletText("Play: Press '%s' to play the current slot. Playback drives P2 while you keep controlling P1.", GetKeyName(cfg.macroPlayKey).c_str());
        ImGui::BulletText("Facing-aware: Directions are flipped automatically based on P2 facing.");
        ImGui::BulletText("Triggers: In Auto Action, choose 'Macro' in the Action combo and pick a Slot to run on that trigger.");
        ImGui::BulletText("Overlay: Trigger overlay will show 'Macro Slot #X' when a macro is configured.");

        ImGui::Dummy(ImVec2(1, 4));
        ImGui::SeparatorText("Recording tips");
        ImGui::TextWrapped(
            "Timing: Recording samples at 64 Hz and plays back at the game\'s input poll rate for accuracy. \n"
            "Buttons are merged per-tick, while directions/motions are taken from the live input buffer.");
        ImGui::BulletText("Frame-step aware: If the game is frozen (Practice pause/frame-step via EfzRevival), recording only advances when a frame is actually stepped. This avoids long held directions.");
        ImGui::BulletText("Empty slots: Attempting to play an empty slot does nothing.");

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
        }
        ImGui::EndChild();
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
        // (Minagi conversion checkbox moved to Debug tab)
        
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

            // Poison Timer & Level
            int p1Poison = guiState.localData.p1MisuzuPoisonTimer;
            p1Poison = CLAMP(p1Poison, 0, MISUZU_POISON_TIMER_MAX);
            ImGui::Text("Poison Timer:");
            if (ImGui::SliderInt("##P1MisuzuPoison", &p1Poison, 0, MISUZU_POISON_TIMER_MAX)) {
                guiState.localData.p1MisuzuPoisonTimer = p1Poison;
            }
            bool p1InfPoison = guiState.localData.p1MisuzuInfinitePoison;
            if (ImGui::Checkbox("Infinite Poison Timer##p1Misuzu", &p1InfPoison)) {
                guiState.localData.p1MisuzuInfinitePoison = p1InfPoison;
                if (p1InfPoison) guiState.localData.p1MisuzuPoisonTimer = MISUZU_POISON_TIMER_MAX;
            }
            int p1PoisonLvl = guiState.localData.p1MisuzuPoisonLevel;
            if (ImGui::InputInt("Poison Level##p1", &p1PoisonLvl)) {
                // No cap: write raw value as requested
                guiState.localData.p1MisuzuPoisonLevel = p1PoisonLvl;
            }
            ImGui::TextDisabled("(Misuzu: feathers @+0x3148, poison timer @+0x345C, level @+0x3460)");
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
            const char* tsItems1[] = { "Inactive", "A Version", "B Version", "C Version" };
            int ts1 = guiState.localData.p1AkikoTimeslowTrigger;
            ts1 = CLAMP(ts1, AKIKO_TIMESLOW_INACTIVE, AKIKO_TIMESLOW_C);
            bool inf1 = guiState.localData.p1AkikoInfiniteTimeslow;
            if (ImGui::Checkbox("Infinite timeslow (freeze 000)##p1Akiko", &inf1)) {
                guiState.localData.p1AkikoInfiniteTimeslow = inf1;
            }
            ImGui::TextDisabled("(Akiko: bullet routes and clock-slow; 'Infinite' now freezes the XYZ digits to 000)");
    }
        // P1 Mai (Kawasumi) Settings
        else if (p1CharID == CHAR_ID_MAI) {
            // Status selector
            const char* statusItems[] = { "0: Inactive", "1: Active Ghost", "2: Unsummon", "3: Charging", "4: Awakening" };
            int st = guiState.localData.p1MaiStatus; st = CLAMP(st,0,4);
            ImGui::Text("Status:");
            ImGui::Combo("##P1MaiStatus", &st, statusItems, IM_ARRAYSIZE(statusItems));
            guiState.localData.p1MaiStatus = st;
            // Determine which timer meaning is active
            int maxVal = (st==3)?MAI_GHOST_CHARGE_MAX:MAI_GHOST_TIME_MAX; // ghost time & awakening share 10000; charge 1200
            if (st==4) maxVal = MAI_AWAKENING_MAX;
            int *activePtr = nullptr;
            const char* label = "Timer";
            if (st==1) { activePtr = &guiState.localData.p1MaiGhostTime; label = "Ghost Time"; }
            else if (st==3) { activePtr = &guiState.localData.p1MaiGhostCharge; label = "Ghost Charge"; }
            else if (st==4) { activePtr = &guiState.localData.p1MaiAwakeningTime; label = "Awakening"; }
            if (activePtr) {
                int val = *activePtr; val = CLAMP(val,0,maxVal);
                float pct = (float)val / (float)maxVal;
                ImGui::Text("%s:", label);
                ImGui::ProgressBar(pct, ImVec2(-1,0), (std::to_string(val) + "/" + std::to_string(maxVal)).c_str());
                if (ImGui::SliderInt("##P1MaiActiveTimer", &val, 0, maxVal)) { *activePtr = val; }
                // Quick set row
                if (ImGui::Button("Max##P1MaiTimer")) { *activePtr = maxVal; }
                ImGui::SameLine();
                if (ImGui::Button("Zero##P1MaiTimer")) { *activePtr = 0; }
            } else {
                ImGui::TextDisabled("No active timer for this status.");
            }
            // Infinite toggles contextual
            bool infGhost = guiState.localData.p1MaiInfiniteGhost;
            bool infCharge = guiState.localData.p1MaiInfiniteCharge;
            bool infAw = guiState.localData.p1MaiInfiniteAwakening;
            ImGui::Checkbox("Inf Ghost##P1MaiInfG", &infGhost); guiState.localData.p1MaiInfiniteGhost = infGhost;
            ImGui::SameLine(); ImGui::Checkbox("Inf Charge##P1MaiInfC", &infCharge); guiState.localData.p1MaiInfiniteCharge = infCharge;
            ImGui::SameLine(); ImGui::Checkbox("Inf Awakening##P1MaiInfA", &infAw); guiState.localData.p1MaiInfiniteAwakening = infAw;
            bool noCD1 = guiState.localData.p1MaiNoChargeCD;
            if (ImGui::Checkbox("No CD (fast charge)##P1MaiNoCD", &noCD1)) guiState.localData.p1MaiNoChargeCD = noCD1;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Effective only while Charging (status 3): forces charge timer to 1 each tick.");
            ImGui::TextDisabled("(Mai: status @0x3144, multi-timer @0x3148 – meaning depends on status)");
            // Ghost coordinate edit controls
            double setGX = guiState.localData.p1MaiGhostSetX;
            double setGY = guiState.localData.p1MaiGhostSetY;
            if (std::isnan(setGX)) { setGX = !std::isnan(guiState.localData.p1MaiGhostX) ? guiState.localData.p1MaiGhostX : guiState.localData.x1; }
            if (std::isnan(setGY)) { setGY = !std::isnan(guiState.localData.p1MaiGhostY) ? guiState.localData.p1MaiGhostY : guiState.localData.y1; }
            ImGui::Text("Ghost Position Override:");
            ImGui::SetNextItemWidth(100); ImGui::InputDouble("X##P1MaiGhostX", &setGX, 1.0, 10.0, "%.1f"); ImGui::SameLine();
            ImGui::SetNextItemWidth(100); ImGui::InputDouble("Y##P1MaiGhostY", &setGY, 1.0, 10.0, "%.1f");
            if (setGX != guiState.localData.p1MaiGhostSetX) guiState.localData.p1MaiGhostSetX = setGX;
            if (setGY != guiState.localData.p1MaiGhostSetY) guiState.localData.p1MaiGhostSetY = setGY;
            if (ImGui::Button("Apply Ghost Pos##P1MaiGhost")) {
                // Commit immediately without needing bottom Apply
                displayData.p1MaiGhostSetX = guiState.localData.p1MaiGhostSetX;
                displayData.p1MaiGhostSetY = guiState.localData.p1MaiGhostSetY;
                displayData.p1MaiApplyGhostPos = true;
                displayData.p1CharID = guiState.localData.p1CharID;
                displayData.p2CharID = guiState.localData.p2CharID;
                if (auto baseNow = GetEFZBase()) {
                    CharacterSettings::TickCharacterEnforcements(baseNow, displayData);
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("P1 Pos##P1MaiGhostUseP1")) {
                // Copy current P1 position into override targets
                guiState.localData.p1MaiGhostSetX = guiState.localData.x1;
                guiState.localData.p1MaiGhostSetY = guiState.localData.y1;
            }
            ImGui::SameLine();
            if (ImGui::Button("P2 Pos##P1MaiGhostUseP2")) {
                // Copy current P2 position into override targets (useful for mirroring setups)
                guiState.localData.p1MaiGhostSetX = guiState.localData.x2;
                guiState.localData.p1MaiGhostSetY = guiState.localData.y2;
            }
    }
        // P1 Kano Settings
        else if (p1CharID == CHAR_ID_KANO) {
            int magic = guiState.localData.p1KanoMagic;
            magic = CLAMP(magic, 0, KANO_MAGIC_MAX);
            float pct = (float)magic / (float)KANO_MAGIC_MAX;
            ImGui::Text("Magic Meter:");
            ImGui::ProgressBar(pct, ImVec2(-1,0), (std::to_string(magic) + "/" + std::to_string(KANO_MAGIC_MAX)).c_str());
            if (ImGui::SliderInt("##P1KanoMagic", &magic, 0, KANO_MAGIC_MAX)) {
                guiState.localData.p1KanoMagic = magic;
            }
            if (ImGui::Button("Max##P1KanoMagic")) { guiState.localData.p1KanoMagic = KANO_MAGIC_MAX; }
            ImGui::SameLine();
            if (ImGui::Button("Zero##P1KanoMagic")) { guiState.localData.p1KanoMagic = 0; }
            bool lock = guiState.localData.p1KanoLockMagic;
            if (ImGui::Checkbox("Lock magic##p1Kano", &lock)) { guiState.localData.p1KanoLockMagic = lock; }
            ImGui::TextDisabled("(Kano: magic meter at +0x3150)");
    }
        // P1 Nayuki (Awake) Settings
        else if (p1CharID == CHAR_ID_NAYUKIB) {
            int t = guiState.localData.p1NayukiSnowbunnies;
            t = CLAMP(t, 0, NAYUKIB_SNOWBUNNY_MAX);
            float pct = (float)t / (float)NAYUKIB_SNOWBUNNY_MAX;
            ImGui::Text("Snowbunnies Timer:");
            ImGui::ProgressBar(pct, ImVec2(-1,0), (std::to_string(t) + "/" + std::to_string(NAYUKIB_SNOWBUNNY_MAX)).c_str());
            if (ImGui::SliderInt("##P1NayukiSnow", &t, 0, NAYUKIB_SNOWBUNNY_MAX)) {
                guiState.localData.p1NayukiSnowbunnies = t;
            }
            bool inf = guiState.localData.p1NayukiInfiniteSnow;
            if (ImGui::Checkbox("Infinite timer##p1Nayuki", &inf)) guiState.localData.p1NayukiInfiniteSnow = inf;
            ImGui::TextDisabled("(Nayuki: snowbunnies timer at +0x3150)");
    }
        // P1 Mio Settings
        else if (p1CharID == CHAR_ID_MIO) {
            ImGui::Text("Mio Stance:");
            int stance = guiState.localData.p1MioStance;
            stance = (stance==MIO_STANCE_LONG)?MIO_STANCE_LONG:MIO_STANCE_SHORT;
            const char* mioStances[] = { "Short", "Long" };
            if (ImGui::Combo("##P1MioStance", &stance, mioStances, IM_ARRAYSIZE(mioStances))) {
                guiState.localData.p1MioStance = stance;
            }
            bool lock = guiState.localData.p1MioLockStance;
            if (ImGui::Checkbox("Lock stance##p1Mio", &lock)) {
                guiState.localData.p1MioLockStance = lock;
            }
            ImGui::TextDisabled("(Mio: stance byte at +0x3150, 0=Short,1=Long)");
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
        // P1 Minagi Settings
        else if (p1CharID == CHAR_ID_MINAGI) {
            bool readied1 = guiState.localData.p1MinagiAlwaysReadied;
            if (ImGui::Checkbox("Always readied##p1Minagi", &readied1)) {
                guiState.localData.p1MinagiAlwaysReadied = readied1;
            }
            ImGui::TextDisabled("(Sets Michiru to ID 401 when idle/unreadied; Practice only)");
            // Michiru position override controls
            double setMX = guiState.localData.p1MinagiPuppetSetX;
            double setMY = guiState.localData.p1MinagiPuppetSetY;
            if (std::isnan(setMX)) {
                if (!std::isnan(guiState.localData.p1MinagiPuppetX)) setMX = guiState.localData.p1MinagiPuppetX; else setMX = guiState.localData.x1;
            }
            if (std::isnan(setMY)) {
                if (!std::isnan(guiState.localData.p1MinagiPuppetY)) setMY = guiState.localData.p1MinagiPuppetY; else setMY = guiState.localData.y1;
            }
            ImGui::Text("Michiru Position Override:");
            ImGui::SetNextItemWidth(100); ImGui::InputDouble("X##P1MinagiMichiruX", &setMX, 1.0, 10.0, "%.1f"); ImGui::SameLine();
            ImGui::SetNextItemWidth(100); ImGui::InputDouble("Y##P1MinagiMichiruY", &setMY, 1.0, 10.0, "%.1f");
            if (setMX != guiState.localData.p1MinagiPuppetSetX) guiState.localData.p1MinagiPuppetSetX = setMX;
            if (setMY != guiState.localData.p1MinagiPuppetSetY) guiState.localData.p1MinagiPuppetSetY = setMY;
            if (ImGui::Button("Apply Michiru Pos##P1Minagi")) {
                // Commit immediately without needing bottom Apply
                displayData.p1MinagiPuppetSetX = guiState.localData.p1MinagiPuppetSetX;
                displayData.p1MinagiPuppetSetY = guiState.localData.p1MinagiPuppetSetY;
                displayData.p1MinagiApplyPos = true;
                // Ensure character ID is correct for enforcement predicate
                displayData.p1CharID = guiState.localData.p1CharID;
                displayData.p2CharID = guiState.localData.p2CharID;
                if (auto baseNow = GetEFZBase()) {
                    CharacterSettings::TickCharacterEnforcements(baseNow, displayData);
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("P1 Pos##P1MinagiUseP1")) {
                guiState.localData.p1MinagiPuppetSetX = guiState.localData.x1;
                guiState.localData.p1MinagiPuppetSetY = guiState.localData.y1;
            }
            ImGui::SameLine();
            if (ImGui::Button("P2 Pos##P2MinagiUseP2")) {
                // Copy P2 player position into P1's Michiru override
                guiState.localData.p1MinagiPuppetSetX = guiState.localData.x2;
                guiState.localData.p1MinagiPuppetSetY = guiState.localData.y2;
            }
            ImGui::SameLine();
            if (ImGui::Button("Michiru Pos##P1MinagiUseMichiru")) {
                if (!std::isnan(guiState.localData.p1MinagiPuppetX) && !std::isnan(guiState.localData.p1MinagiPuppetY)) {
                    guiState.localData.p1MinagiPuppetSetX = guiState.localData.p1MinagiPuppetX;
                    guiState.localData.p1MinagiPuppetSetY = guiState.localData.p1MinagiPuppetY;
                }
            }
            if (!std::isnan(guiState.localData.p1MinagiPuppetX)) {
                ImGui::TextDisabled("Current: (%.1f, %.1f)", guiState.localData.p1MinagiPuppetX, guiState.localData.p1MinagiPuppetY);
            } else {
                ImGui::TextDisabled("Current: (not present)");
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

            // Poison Timer & Level
            int p2Poison = guiState.localData.p2MisuzuPoisonTimer;
            p2Poison = CLAMP(p2Poison, 0, MISUZU_POISON_TIMER_MAX);
            ImGui::Text("Poison Timer:");
            if (ImGui::SliderInt("##P2MisuzuPoison", &p2Poison, 0, MISUZU_POISON_TIMER_MAX)) {
                guiState.localData.p2MisuzuPoisonTimer = p2Poison;
            }
            bool p2InfPoison = guiState.localData.p2MisuzuInfinitePoison;
            if (ImGui::Checkbox("Infinite Poison Timer##p2Misuzu", &p2InfPoison)) {
                guiState.localData.p2MisuzuInfinitePoison = p2InfPoison;
                if (p2InfPoison) guiState.localData.p2MisuzuPoisonTimer = MISUZU_POISON_TIMER_MAX;
            }
            int p2PoisonLvl = guiState.localData.p2MisuzuPoisonLevel;
            if (ImGui::InputInt("Poison Level##p2", &p2PoisonLvl)) {
                // No cap: write raw value as requested
                guiState.localData.p2MisuzuPoisonLevel = p2PoisonLvl;
            }
            ImGui::TextDisabled("(Misuzu: feathers @+0x3148, poison timer @+0x345C, level @+0x3460)");
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
            const char* tsItems2[] = { "Inactive", "A Version", "B Version", "C Version" };
            int ts2 = guiState.localData.p2AkikoTimeslowTrigger;
            ts2 = CLAMP(ts2, AKIKO_TIMESLOW_INACTIVE, AKIKO_TIMESLOW_C);
            if (ImGui::Combo("##P2AkikoTimeslow", &ts2, tsItems2, IM_ARRAYSIZE(tsItems2))) {
                guiState.localData.p2AkikoTimeslowTrigger = ts2;
            }
            bool inf2 = guiState.localData.p2AkikoInfiniteTimeslow;
            if (ImGui::Checkbox("Infinite timeslow (freeze 000)##p2Akiko", &inf2)) {
                guiState.localData.p2AkikoInfiniteTimeslow = inf2;
            }
            ImGui::TextDisabled("(Akiko: bullet routes and clock-slow; 'Infinite' now freezes the XYZ digits to 000)");
    }
        // P2 Mai (Kawasumi) Settings
        else if (p2CharID == CHAR_ID_MAI) {
            const char* statusItems[] = { "0: Inactive", "1: Active Ghost", "2: Unsummon", "3: Charging", "4: Awakening" };
            int st2 = guiState.localData.p2MaiStatus; st2 = CLAMP(st2,0,4);
            ImGui::Text("Status:");
            ImGui::Combo("##P2MaiStatus", &st2, statusItems, IM_ARRAYSIZE(statusItems));
            guiState.localData.p2MaiStatus = st2;
            int maxVal2 = (st2==3)?MAI_GHOST_CHARGE_MAX:MAI_GHOST_TIME_MAX; if (st2==4) maxVal2 = MAI_AWAKENING_MAX;
            int *activePtr2 = nullptr; const char* label2 = "Timer";
            if (st2==1) { activePtr2 = &guiState.localData.p2MaiGhostTime; label2 = "Ghost Time"; }
            else if (st2==3) { activePtr2 = &guiState.localData.p2MaiGhostCharge; label2 = "Ghost Charge"; }
            else if (st2==4) { activePtr2 = &guiState.localData.p2MaiAwakeningTime; label2 = "Awakening"; }
            if (activePtr2) {
                int val2 = *activePtr2; val2 = CLAMP(val2,0,maxVal2); float pct2 = (float)val2 / (float)maxVal2;
                ImGui::Text("%s:", label2);
                ImGui::ProgressBar(pct2, ImVec2(-1,0), (std::to_string(val2) + "/" + std::to_string(maxVal2)).c_str());
                if (ImGui::SliderInt("##P2MaiActiveTimer", &val2, 0, maxVal2)) { *activePtr2 = val2; }
                if (ImGui::Button("Max##P2MaiTimer")) { *activePtr2 = maxVal2; }
                ImGui::SameLine(); if (ImGui::Button("Zero##P2MaiTimer")) { *activePtr2 = 0; }
            } else {
                ImGui::TextDisabled("No active timer for this status.");
            }
            bool infGhost2 = guiState.localData.p2MaiInfiniteGhost; ImGui::Checkbox("Inf Ghost##P2MaiInfG", &infGhost2); guiState.localData.p2MaiInfiniteGhost = infGhost2;
            ImGui::SameLine(); bool infCharge2 = guiState.localData.p2MaiInfiniteCharge; ImGui::Checkbox("Inf Charge##P2MaiInfC", &infCharge2); guiState.localData.p2MaiInfiniteCharge = infCharge2;
            ImGui::SameLine(); bool infAw2 = guiState.localData.p2MaiInfiniteAwakening; ImGui::Checkbox("Inf Awakening##P2MaiInfA", &infAw2); guiState.localData.p2MaiInfiniteAwakening = infAw2;
            bool noCD2 = guiState.localData.p2MaiNoChargeCD;
            if (ImGui::Checkbox("No CD (fast charge)##P2MaiNoCD", &noCD2)) guiState.localData.p2MaiNoChargeCD = noCD2;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Effective only while Charging (status 3): forces charge timer to 1 each tick.");
            ImGui::TextDisabled("(Mai: status @0x3144, multi-timer @0x3148 – meaning depends on status)");
            double setGX2 = guiState.localData.p2MaiGhostSetX;
            double setGY2 = guiState.localData.p2MaiGhostSetY;
            if (std::isnan(setGX2)) { setGX2 = !std::isnan(guiState.localData.p2MaiGhostX) ? guiState.localData.p2MaiGhostX : guiState.localData.x2; }
            if (std::isnan(setGY2)) { setGY2 = !std::isnan(guiState.localData.p2MaiGhostY) ? guiState.localData.p2MaiGhostY : guiState.localData.y2; }
            ImGui::Text("Ghost Position Override:");
            ImGui::SetNextItemWidth(100); ImGui::InputDouble("X##P2MaiGhostX", &setGX2, 1.0, 10.0, "%.1f"); ImGui::SameLine();
            ImGui::SetNextItemWidth(100); ImGui::InputDouble("Y##P2MaiGhostY", &setGY2, 1.0, 10.0, "%.1f");
            if (setGX2 != guiState.localData.p2MaiGhostSetX) guiState.localData.p2MaiGhostSetX = setGX2;
            if (setGY2 != guiState.localData.p2MaiGhostSetY) guiState.localData.p2MaiGhostSetY = setGY2;
            if (ImGui::Button("Apply Ghost Pos##P2MaiGhost")) {
                // Commit immediately without needing bottom Apply
                displayData.p2MaiGhostSetX = guiState.localData.p2MaiGhostSetX;
                displayData.p2MaiGhostSetY = guiState.localData.p2MaiGhostSetY;
                displayData.p2MaiApplyGhostPos = true;
                displayData.p1CharID = guiState.localData.p1CharID;
                displayData.p2CharID = guiState.localData.p2CharID;
                if (auto baseNow = GetEFZBase()) {
                    CharacterSettings::TickCharacterEnforcements(baseNow, displayData);
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("P2 Pos##P2MaiGhostUseP2")) {
                guiState.localData.p2MaiGhostSetX = guiState.localData.x2;
                guiState.localData.p2MaiGhostSetY = guiState.localData.y2;
            }
            ImGui::SameLine();
            if (ImGui::Button("P1 Pos##P2MaiGhostUseP1")) {
                guiState.localData.p2MaiGhostSetX = guiState.localData.x1;
                guiState.localData.p2MaiGhostSetY = guiState.localData.y1;
            }
    }
        // P2 Kano Settings
        else if (p2CharID == CHAR_ID_KANO) {
            int magic2 = guiState.localData.p2KanoMagic;
            magic2 = CLAMP(magic2, 0, KANO_MAGIC_MAX);
            float pct2 = (float)magic2 / (float)KANO_MAGIC_MAX;
            ImGui::Text("Magic Meter:");
            ImGui::ProgressBar(pct2, ImVec2(-1,0), (std::to_string(magic2) + "/" + std::to_string(KANO_MAGIC_MAX)).c_str());
            if (ImGui::SliderInt("##P2KanoMagic", &magic2, 0, KANO_MAGIC_MAX)) {
                guiState.localData.p2KanoMagic = magic2;
            }
            if (ImGui::Button("Max##P2KanoMagic")) { guiState.localData.p2KanoMagic = KANO_MAGIC_MAX; }
            ImGui::SameLine();
            if (ImGui::Button("Zero##P2KanoMagic")) { guiState.localData.p2KanoMagic = 0; }
            bool lock2 = guiState.localData.p2KanoLockMagic;
            if (ImGui::Checkbox("Lock magic##p2Kano", &lock2)) { guiState.localData.p2KanoLockMagic = lock2; }
            ImGui::TextDisabled("(Kano: magic meter at +0x3150)");
    }
        // P2 Nayuki (Awake) Settings
        else if (p2CharID == CHAR_ID_NAYUKIB) {
            int t2 = guiState.localData.p2NayukiSnowbunnies;
            t2 = CLAMP(t2, 0, NAYUKIB_SNOWBUNNY_MAX);
            float pct2 = (float)t2 / (float)NAYUKIB_SNOWBUNNY_MAX;
            ImGui::Text("Snowbunnies Timer:");
            ImGui::ProgressBar(pct2, ImVec2(-1,0), (std::to_string(t2) + "/" + std::to_string(NAYUKIB_SNOWBUNNY_MAX)).c_str());
            if (ImGui::SliderInt("##P2NayukiSnow", &t2, 0, NAYUKIB_SNOWBUNNY_MAX)) {
                guiState.localData.p2NayukiSnowbunnies = t2;
            }
            bool inf2 = guiState.localData.p2NayukiInfiniteSnow;
            if (ImGui::Checkbox("Infinite timer##p2Nayuki", &inf2)) guiState.localData.p2NayukiInfiniteSnow = inf2;
            ImGui::TextDisabled("(Nayuki: snowbunnies timer at +0x3150)");
    }
        // P2 Mio Settings
        else if (p2CharID == CHAR_ID_MIO) {
            ImGui::Text("Mio Stance:");
            int stance2 = guiState.localData.p2MioStance;
            stance2 = (stance2==MIO_STANCE_LONG)?MIO_STANCE_LONG:MIO_STANCE_SHORT;
            const char* mioStances2[] = { "Short", "Long" };
            if (ImGui::Combo("##P2MioStance", &stance2, mioStances2, IM_ARRAYSIZE(mioStances2))) {
                guiState.localData.p2MioStance = stance2;
            }
            bool lock2 = guiState.localData.p2MioLockStance;
            if (ImGui::Checkbox("Lock stance##p2Mio", &lock2)) {
                guiState.localData.p2MioLockStance = lock2;
            }
            ImGui::TextDisabled("(Mio: stance byte at +0x3150, 0=Short,1=Long)");
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
        // P2 Minagi Settings
        else if (p2CharID == CHAR_ID_MINAGI) {
            bool readied2 = guiState.localData.p2MinagiAlwaysReadied;
            if (ImGui::Checkbox("Always readied##p2Minagi", &readied2)) {
                guiState.localData.p2MinagiAlwaysReadied = readied2;
            }
            ImGui::TextDisabled("(Sets Michiru to ID 401 when idle/unreadied; Practice only)");
            // Michiru position override controls (P2)
            double setMX2 = guiState.localData.p2MinagiPuppetSetX;
            double setMY2 = guiState.localData.p2MinagiPuppetSetY;
            if (std::isnan(setMX2)) {
                if (!std::isnan(guiState.localData.p2MinagiPuppetX)) setMX2 = guiState.localData.p2MinagiPuppetX; else setMX2 = guiState.localData.x2;
            }
            if (std::isnan(setMY2)) {
                if (!std::isnan(guiState.localData.p2MinagiPuppetY)) setMY2 = guiState.localData.p2MinagiPuppetY; else setMY2 = guiState.localData.y2;
            }
            ImGui::Text("Michiru Position Override:");
            ImGui::SetNextItemWidth(100); ImGui::InputDouble("X##P2MinagiMichiruX", &setMX2, 1.0, 10.0, "%.1f"); ImGui::SameLine();
            ImGui::SetNextItemWidth(100); ImGui::InputDouble("Y##P2MinagiMichiruY", &setMY2, 1.0, 10.0, "%.1f");
            if (setMX2 != guiState.localData.p2MinagiPuppetSetX) guiState.localData.p2MinagiPuppetSetX = setMX2;
            if (setMY2 != guiState.localData.p2MinagiPuppetSetY) guiState.localData.p2MinagiPuppetSetY = setMY2;
            if (ImGui::Button("Apply Michiru Pos##P2Minagi")) {
                // Commit immediately without needing bottom Apply
                displayData.p2MinagiPuppetSetX = guiState.localData.p2MinagiPuppetSetX;
                displayData.p2MinagiPuppetSetY = guiState.localData.p2MinagiPuppetSetY;
                displayData.p2MinagiApplyPos = true;
                // Ensure character ID is correct for enforcement predicate
                displayData.p1CharID = guiState.localData.p1CharID;
                displayData.p2CharID = guiState.localData.p2CharID;
                if (auto baseNow = GetEFZBase()) {
                    CharacterSettings::TickCharacterEnforcements(baseNow, displayData);
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("P2 Pos##P2MinagiUseP2")) {
                guiState.localData.p2MinagiPuppetSetX = guiState.localData.x2;
                guiState.localData.p2MinagiPuppetSetY = guiState.localData.y2;
            }
            ImGui::SameLine();
            if (ImGui::Button("P1 Pos##P2MinagiUseP1")) {
                guiState.localData.p2MinagiPuppetSetX = guiState.localData.x1;
                guiState.localData.p2MinagiPuppetSetY = guiState.localData.y1;
            }
            ImGui::SameLine();
            if (ImGui::Button("Michiru Pos##P2MinagiUseMichiru")) {
                if (!std::isnan(guiState.localData.p2MinagiPuppetX) && !std::isnan(guiState.localData.p2MinagiPuppetY)) {
                    guiState.localData.p2MinagiPuppetSetX = guiState.localData.p2MinagiPuppetX;
                    guiState.localData.p2MinagiPuppetSetY = guiState.localData.p2MinagiPuppetY;
                }
            }
            if (!std::isnan(guiState.localData.p2MinagiPuppetX)) {
                ImGui::TextDisabled("Current: (%.1f, %.1f)", guiState.localData.p2MinagiPuppetX, guiState.localData.p2MinagiPuppetY);
            } else {
                ImGui::TextDisabled("Current: (not present)");
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
            "Supported: Ikumi (Blood/Genocide), Misuzu (Feathers), Mishio (Element/Awakened), Rumi (Stance, Kimchi), Akiko (Bullet/Time-Slow), Neyuki (Jam 0-9), Kano (Magic), Mio (Stance), Doppel (Enlightened), Mai (Ghost/Awakening), Minagi (Michiru debug + Always readied)");
    }
    
    // Add this new function to the ImGuiGui namespace:
    void RenderDebugInputTab() {
        // Practice Switch Players control
        if (GetCurrentGameMode() == GameMode::Practice) {
            ImGui::SeparatorText("Switch Players (Practice)");
            int curLocal = -1;
            PauseIntegration::EnsurePracticePointerCapture();
            if (void* p = PauseIntegration::GetPracticeControllerPtr()) {
                SafeReadMemory((uintptr_t)p + PRACTICE_OFF_LOCAL_SIDE_IDX, &curLocal, sizeof(curLocal));
            }
            if (ImGui::Button("Toggle Switch Players")) {
                bool ok = SwitchPlayers::ToggleLocalSide();
                if (!ok) {
                    LogOut("[DEBUG/UI] SwitchPlayers toggle failed (Practice controller not ready?)", true);
                    DirectDrawHook::AddMessage("Switch Players: FAILED", "SYSTEM", RGB(255,100,100), 1500, 0, 100);
                } else {
                    // Re-read after toggle for display
                    curLocal = -1;
                    if (void* p2 = PauseIntegration::GetPracticeControllerPtr()) {
                        SafeReadMemory((uintptr_t)p2 + PRACTICE_OFF_LOCAL_SIDE_IDX, &curLocal, sizeof(curLocal));
                    }
                    DirectDrawHook::AddMessage(curLocal == 0 ? "Local: P1" : (curLocal == 1 ? "Local: P2" : "Local: ?"),
                                               "SYSTEM", RGB(100,255,100), 1500, 0, 100);
                }
            }
            ImGui::SameLine();
            ImGui::TextDisabled("Swap which side is local input (P1/P2)");
            if (curLocal == 0) {
                ImGui::Text("Current Local: P1");
            } else if (curLocal == 1) {
                ImGui::Text("Current Local: P2");
            } else {
                ImGui::TextDisabled("Current Local: (unknown)");
            }

            // AI/Human flags and Practice CPU flag status
            ImGui::Separator();
            ImGui::Text("AI Control Flags: ");
            bool p1Human = IsAIControlFlagHuman(1);
            bool p2Human = IsAIControlFlagHuman(2);
            ImGui::BulletText("P1: %s", p1Human ? "Human" : "AI");
            ImGui::BulletText("P2: %s", p2Human ? "Human" : "AI");

            // Practice P2 CPU flag at gameState + 4931 (1=CPU, 0=Human)
            uintptr_t efzBase = GetEFZBase();
            uint8_t p2CpuFlag = 0xFF;
            if (efzBase) {
                uintptr_t gameStatePtr = 0;
                if (SafeReadMemory(efzBase + EFZ_BASE_OFFSET_GAME_STATE, &gameStatePtr, sizeof(gameStatePtr)) && gameStatePtr) {
                    SafeReadMemory(gameStatePtr + 4931, &p2CpuFlag, sizeof(p2CpuFlag));
                }
            }
            if (p2CpuFlag != 0xFF) {
                ImGui::BulletText("Practice P2 CPU flag: %s (byte=%u)", (p2CpuFlag ? "CPU" : "Human"), (unsigned)p2CpuFlag);
            } else {
                ImGui::BulletText("Practice P2 CPU flag: unknown");
            }

            // Current gamespeed for pause troubleshooting
            // efz.exe + 0x39010C -> [ptr] + 0xF7FF8
            uint8_t curSpeed = 0xFF;
            if (efzBase) {
                // Use the same chain described in the cheat table
                uintptr_t basePtr = 0;
                if (SafeReadMemory(efzBase + EFZ_BASE_OFFSET_GAME_STATE, &basePtr, sizeof(basePtr)) && basePtr) {
                    // We don't know if [+0xF7FF8] lives off game state or a sibling object; use PauseIntegration's chain if needed
                    // but for UI we try the CE chain explicitly:
                    HMODULE hEfz = GetModuleHandleA("efz.exe");
                    if (hEfz) {
                        uintptr_t efzBaseVA = reinterpret_cast<uintptr_t>(hEfz);
                        uint32_t rootPtr = 0;
                        if (SafeReadMemory(efzBaseVA + 0x39010C, &rootPtr, sizeof(rootPtr)) && rootPtr) {
                            uint8_t spd = 0xFF;
                            if (SafeReadMemory(static_cast<uintptr_t>(rootPtr) + 0xF7FF8, &spd, sizeof(spd))) {
                                curSpeed = spd;
                            }
                        }
                    }
                }
            }
            if (curSpeed != 0xFF) {
                ImGui::BulletText("Gamespeed: %u (0=freeze, 3=normal)", (unsigned)curSpeed);
            } else {
                ImGui::BulletText("Gamespeed: unknown");
            }
        }

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
        bool showRGToasts = g_ShowRGDebugToasts.load();
        if (ImGui::Checkbox("Show RG debug toasts", &showRGToasts)) {
            g_ShowRGDebugToasts.store(showRGToasts);
        }

        // Always RG toggle moved to Game Settings

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

        ImGui::Separator();
        ImGui::Text("Final Memory (manual execute)");
        int p1Char = guiState.localData.p1CharID;
        int p2Char = guiState.localData.p2CharID;
        if (ImGui::Button("Run P1 FM")) { ExecuteFinalMemory(1, p1Char); }
        ImGui::SameLine();
        if (ImGui::Button("Run P2 FM")) { ExecuteFinalMemory(2, p2Char); }
        ImGui::Separator();
        // Mai debug controls (relocated): show only if Mai present on either side
        if (guiState.localData.p1CharID == CHAR_ID_MAI || guiState.localData.p2CharID == CHAR_ID_MAI) {
            ImGui::SeparatorText("Mai Control");
            ImGui::TextDisabled("Authentic summon/despawn helpers (debug)");
            if (guiState.localData.p1CharID == CHAR_ID_MAI) {
                ImGui::Text("P1 Mai:"); ImGui::SameLine();
                if (ImGui::Button("Force Summon##DbgP1Mai")) guiState.localData.p1MaiForceSummon = true;
                ImGui::SameLine(); if (ImGui::Button("Force Despawn##DbgP1Mai")) guiState.localData.p1MaiForceDespawn = true;
                bool agg1 = guiState.localData.p1MaiAggressiveOverride;
                if (ImGui::Checkbox("Aggressive##DbgP1MaiAgg", &agg1)) guiState.localData.p1MaiAggressiveOverride = agg1;
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("When enabled, Force Summon overrides transitional (status=2) states.");
                if (!std::isnan(guiState.localData.p1MaiGhostX)) {
                    ImGui::Text("Ghost Pos: (%.1f, %.1f)", guiState.localData.p1MaiGhostX, guiState.localData.p1MaiGhostY);
                } else {
                    ImGui::TextDisabled("Ghost Pos: (not active)");
                }
            }
            if (guiState.localData.p2CharID == CHAR_ID_MAI) {
                ImGui::Text("P2 Mai:"); ImGui::SameLine();
                if (ImGui::Button("Force Summon##DbgP2Mai")) guiState.localData.p2MaiForceSummon = true;
                ImGui::SameLine(); if (ImGui::Button("Force Despawn##DbgP2Mai")) guiState.localData.p2MaiForceDespawn = true;
                bool agg2 = guiState.localData.p2MaiAggressiveOverride;
                if (ImGui::Checkbox("Aggressive##DbgP2MaiAgg", &agg2)) guiState.localData.p2MaiAggressiveOverride = agg2;
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("When enabled, Force Summon overrides transitional (status=2) states.");
                if (!std::isnan(guiState.localData.p2MaiGhostX)) {
                    ImGui::Text("Ghost Pos: (%.1f, %.1f)", guiState.localData.p2MaiGhostX, guiState.localData.p2MaiGhostY);
                } else {
                    ImGui::TextDisabled("Ghost Pos: (not active)");
                }
            }
        }

        // Minagi debug controls: conversion toggle shown only when Minagi is present
        if (guiState.localData.p1CharID == CHAR_ID_MINAGI || guiState.localData.p2CharID == CHAR_ID_MINAGI) {
            ImGui::SeparatorText("Minagi Control");
            bool convert = guiState.localData.minagiConvertNewProjectiles;
            if (ImGui::Checkbox("Convert new Minagi projectiles to Michiru (Practice only)", &convert)) {
                guiState.localData.minagiConvertNewProjectiles = convert;
                // Immediate sync to shared displayData so overlay/enforcement pick it up without needing Apply
                displayData.minagiConvertNewProjectiles = convert;
                // Kick one enforcement tick now for responsiveness
                uintptr_t baseNow = GetEFZBase();
                if (baseNow && AreCharactersInitialized()) {
                    CharacterSettings::TickCharacterEnforcements(baseNow, displayData);
                }
            }
            ImGui::SameLine();
            ImGui::TextDisabled("(?)");
            if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
                ImGui::TextUnformatted("When enabled and Minagi is selected, any newly initialized Minagi projectiles (non-character, non-Michiru)\n"
                                       "are rewritten to use Michiru (entity ID 400). This is enforced only in Practice Mode.");
                ImGui::PopTextWrapPos();
                ImGui::EndTooltip();
            }
        }
    }

    // Update the RenderGui function to include the new tab:
    void RenderGui() {
        if (!ImGuiImpl::IsVisible())
            return;

    // Apply a smaller UI scale locally for this window only
    // (shrink widgets via style; text uses crisp font atlas sized in impl)
    ImGuiStyle& __style = ImGui::GetStyle();
    ImGuiStyle __backupStyle = __style; // restore at end of this function
    float __uiScale = Config::GetSettings().uiScale;
    if (__uiScale < 0.70f) __uiScale = 0.70f;
    if (__uiScale > 1.50f) __uiScale = 1.50f;
        __style.ScaleAllSizes(__uiScale);

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
    // Slightly smaller default window size
    ImGui::SetNextWindowSize(ImVec2(500, 440), ImGuiCond_FirstUseEver);
    // Force fully-opaque background to avoid heavy alpha blending on low-end GPUs
    ImGui::SetNextWindowBgAlpha(1.0f);

        // Main window
        // Allow navigation (keyboard/gamepad), disable collapse and saved settings to avoid off-screen positions
        ImGuiWindowFlags winFlags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings;
    if (ImGui::Begin("EFZ Training Mode", nullptr, winFlags)) {
            // Text already crisp-scaled via font atlas; keep per-window font scale at 1.0
            ImGui::SetWindowFontScale(1.0f);

            // Check if a specific tab has been requested
            if (guiState.requestedTab >= 0) {
                guiState.currentTab = guiState.requestedTab;
                guiState.requestedTab = -1; // Reset request
            }
            // Create a scrollable content region with a fixed-height footer for action buttons
            ImVec2 avail = ImGui::GetContentRegionAvail();
            float footerHeight = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y + ImGui::GetStyle().WindowPadding.y;
            // Guard against tiny windows
            if (footerHeight < 32.0f) footerHeight = 32.0f;

            if (ImGui::BeginChild("##MainContent", ImVec2(avail.x, avail.y - footerHeight), true)) {
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
                    
                    // Help tab(s)
                    if (ImGui::BeginTabItem("Macros")) {
                        const auto& cfg = Config::GetSettings();
                        ImGui::SeparatorText("Macro Controller");
                        ImGui::Text("State: %s", MacroController::GetStatusLine().c_str());
                        ImGui::Text("Current Slot: %d / %d", MacroController::GetCurrentSlot(), MacroController::GetSlotCount());
                        bool empty = MacroController::IsSlotEmpty(MacroController::GetCurrentSlot());
                        ImGui::Text("Slot Empty: %s", empty ? "Yes" : "No");
                        // Debug stats for validation
                        {
                            auto stats = MacroController::GetSlotStats(MacroController::GetCurrentSlot());
                            ImGui::SeparatorText("Slot Stats");
                            ImGui::BulletText("Spans: %d", stats.spanCount);
                            ImGui::BulletText("Total Ticks: %d (~%.2fs)", stats.totalTicks, stats.totalTicks / 64.0f);
                            ImGui::BulletText("Buffer Entries: %d", stats.bufEntries);
                            ImGui::BulletText("Buf Idx Start: %u", (unsigned)stats.bufStartIdx);
                            ImGui::BulletText("Buf Idx End: %u", (unsigned)stats.bufEndIdx);
                            ImGui::BulletText("Has Data: %s", stats.hasData ? "Yes" : "No");
                        }
                        if (ImGui::Button("Toggle Record")) { MacroController::ToggleRecord(); }
                        ImGui::SameLine();
                        if (ImGui::Button("Play")) { MacroController::Play(); }
                        ImGui::SameLine();
                        if (ImGui::Button("Stop")) { MacroController::Stop(); }
                        if (ImGui::Button("Prev Slot")) { MacroController::PrevSlot(); }
                        ImGui::SameLine();
                        if (ImGui::Button("Next Slot")) { MacroController::NextSlot(); }
                        ImGui::Spacing();
                        ImGui::SeparatorText("Hotkeys");
                        ImGui::BulletText("Record: %s", GetKeyName(cfg.macroRecordKey).c_str());
                        ImGui::BulletText("Play: %s", GetKeyName(cfg.macroPlayKey).c_str());
                        ImGui::BulletText("Next Slot: %s", GetKeyName(cfg.macroSlotKey).c_str());
                        ImGui::EndTabItem();
                    }
                    if (ImGui::BeginTabItem("Help", nullptr, ImGuiTabItemFlags_NoCloseWithMiddleMouseButton)) {
                        guiState.currentTab = 4;
                        RenderHelpTab();
                        ImGui::EndTabItem();
                    }
                    
                    ImGui::EndTabBar();
                }
            }
            ImGui::EndChild();

            // Fixed footer (always visible) with hotkeys, controller support, tab focus & toasts
            ImGui::Separator();
            bool doApply=false, doRefresh=false, doExit=false;
            const auto& cfg = Config::GetSettings();
            HWND hwnd = FindEFZWindow();
            if (hwnd && GetForegroundWindow()==hwnd) {
                static SHORT lastAccept=0, lastRefresh=0, lastExit=0;
                SHORT curAccept = GetAsyncKeyState(cfg.uiAcceptKey);
                SHORT curRefresh = GetAsyncKeyState(cfg.uiRefreshKey);
                SHORT curExit = GetAsyncKeyState(cfg.uiExitKey);
                if ((curAccept & 0x8000) && !(lastAccept & 0x8000)) doApply = true;
                if ((curRefresh & 0x8000) && !(lastRefresh & 0x8000)) doRefresh = true;
                if ((curExit & 0x8000) && !(lastExit & 0x8000)) doExit = true;
                lastAccept = curAccept; lastRefresh = curRefresh; lastExit = curExit;
                // Controller mapping temporarily disabled (XInput symbols unresolved in this build context)
            }
            // Let ImGui's native keyboard navigation (Tab) handle focus across the entire window.
            // We keep hotkeys/controller triggers for direct actions; no manual Tab interception now.
            std::string applyLabel = std::string("Apply (") + GetKeyName(cfg.uiAcceptKey) + ")";
            std::string refreshLabel = std::string("Refresh (") + GetKeyName(cfg.uiRefreshKey) + ")";
            std::string exitLabel = std::string("Exit (") + GetKeyName(cfg.uiExitKey) + ")";
            bool bApply = ImGui::Button(applyLabel.c_str(), ImVec2(140,0));
            if (bApply || doApply) { ApplyImGuiSettings(); DirectDrawHook::AddMessage("Applied", "FOOTER", RGB(180,255,180), 1000, 0, 60); }
            ImGui::SameLine();
            bool bRefresh = ImGui::Button(refreshLabel.c_str(), ImVec2(140,0));
            if (bRefresh || doRefresh) { RefreshLocalData(); DirectDrawHook::AddMessage("Refreshed", "FOOTER", RGB(180,220,255), 1000, 0, 60); }
            ImGui::SameLine();
            bool bExit = ImGui::Button(exitLabel.c_str(), ImVec2(140,0));
            if (bExit || doExit) { ImGuiImpl::ToggleVisibility(); }
        }
        ImGui::End();

        // Restore global style after rendering our window to avoid affecting other overlays
        ImGui::GetStyle() = __backupStyle;
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

    // Scan Mai ghost slots (Mini-Mai) for each player if character is Mai
    auto ScanMaiGhost = [&](int playerIdx){
        if (!((playerIdx==1 && guiState.localData.p1CharID==CHAR_ID_MAI) || (playerIdx==2 && guiState.localData.p2CharID==CHAR_ID_MAI))) return;
        uintptr_t pBase = ResolvePointer(base, (playerIdx==1)?EFZ_BASE_OFFSET_P1:EFZ_BASE_OFFSET_P2, 0);
        double nanv = std::numeric_limits<double>::quiet_NaN();
        double gx = nanv, gy = nanv;
        if (pBase) {
            for (int i=0;i<MAI_GHOST_SLOT_MAX_SCAN;i++) {
                uintptr_t slot = pBase + MAI_GHOST_SLOTS_BASE + (uintptr_t)i*MAI_GHOST_SLOT_STRIDE;
                unsigned short id=0; if (!SafeReadMemory(slot + MAI_GHOST_SLOT_ID_OFFSET, &id, sizeof(id))) break;
                if (id == 401) {
                    // Read positions
                    SafeReadMemory(slot + MAI_GHOST_SLOT_X_OFFSET, &gx, sizeof(double));
                    SafeReadMemory(slot + MAI_GHOST_SLOT_Y_OFFSET, &gy, sizeof(double));
                    break;
                }
            }
        }
        if (playerIdx==1) { guiState.localData.p1MaiGhostX = gx; guiState.localData.p1MaiGhostY = gy; }
        else { guiState.localData.p2MaiGhostX = gx; guiState.localData.p2MaiGhostY = gy; }
    }; ScanMaiGhost(1); ScanMaiGhost(2);

        // Scan Michiru (Minagi) puppet slots for each player if Minagi is selected.
        auto ScanMichiru = [&](int playerIdx){
            if (!((playerIdx==1 && guiState.localData.p1CharID==CHAR_ID_MINAGI) || (playerIdx==2 && guiState.localData.p2CharID==CHAR_ID_MINAGI))) return;
            uintptr_t pBase = ResolvePointer(base, (playerIdx==1)?EFZ_BASE_OFFSET_P1:EFZ_BASE_OFFSET_P2, 0);
            double nanv = std::numeric_limits<double>::quiet_NaN();
            double mx = nanv, my = nanv;
            if (pBase) {
                for (int i=0;i<MINAGI_PUPPET_SLOT_MAX_SCAN;i++) {
                    uintptr_t slot = pBase + MINAGI_PUPPET_SLOTS_BASE + (uintptr_t)i*MINAGI_PUPPET_SLOT_STRIDE;
                    unsigned short id=0; if (!SafeReadMemory(slot + MINAGI_PUPPET_SLOT_ID_OFFSET, &id, sizeof(id))) break;
                    if (id == MINAGI_PUPPET_ENTITY_ID || id == 401) {
                        SafeReadMemory(slot + MINAGI_PUPPET_SLOT_X_OFFSET, &mx, sizeof(double));
                        SafeReadMemory(slot + MINAGI_PUPPET_SLOT_Y_OFFSET, &my, sizeof(double));
                        break;
                    }
                }
            }
            if (playerIdx==1) { guiState.localData.p1MinagiPuppetX = mx; guiState.localData.p1MinagiPuppetY = my; }
            else { guiState.localData.p2MinagiPuppetX = mx; guiState.localData.p2MinagiPuppetY = my; }
        }; ScanMichiru(1); ScanMichiru(2);

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
    guiState.localData.triggerOnRG         = triggerOnRGEnabled.load();

    // Per-trigger delays
    guiState.localData.delayAfterBlock     = triggerAfterBlockDelay.load();
    guiState.localData.delayOnWakeup       = triggerOnWakeupDelay.load();
    guiState.localData.delayAfterHitstun   = triggerAfterHitstunDelay.load();
    guiState.localData.delayAfterAirtech   = triggerAfterAirtechDelay.load();
    guiState.localData.delayOnRG           = triggerOnRGDelay.load();

    // Per-trigger actions
    guiState.localData.actionAfterBlock    = triggerAfterBlockAction.load();
    guiState.localData.actionOnWakeup      = triggerOnWakeupAction.load();
    guiState.localData.actionAfterHitstun  = triggerAfterHitstunAction.load();
    guiState.localData.actionAfterAirtech  = triggerAfterAirtechAction.load();
    guiState.localData.actionOnRG          = triggerOnRGAction.load();

    // Per-trigger custom IDs
    guiState.localData.customAfterBlock    = triggerAfterBlockCustomID.load();
    guiState.localData.customOnWakeup      = triggerOnWakeupCustomID.load();
    guiState.localData.customAfterHitstun  = triggerAfterHitstunCustomID.load();
    guiState.localData.customAfterAirtech  = triggerAfterAirtechCustomID.load();
    guiState.localData.customOnRG          = triggerOnRGCustomID.load();

    // Per-trigger strengths
    guiState.localData.strengthAfterBlock    = triggerAfterBlockStrength.load();
    guiState.localData.strengthOnWakeup      = triggerOnWakeupStrength.load();
    guiState.localData.strengthAfterHitstun  = triggerAfterHitstunStrength.load();
    guiState.localData.strengthAfterAirtech  = triggerAfterAirtechStrength.load();
    guiState.localData.strengthOnRG          = triggerOnRGStrength.load();

    // Per-trigger macro slot selections
    guiState.localData.macroSlotAfterBlock   = triggerAfterBlockMacroSlot.load();
    guiState.localData.macroSlotOnWakeup     = triggerOnWakeupMacroSlot.load();
    guiState.localData.macroSlotAfterHitstun = triggerAfterHitstunMacroSlot.load();
    guiState.localData.macroSlotAfterAirtech = triggerAfterAirtechMacroSlot.load();
    guiState.localData.macroSlotOnRG         = triggerOnRGMacroSlot.load();
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
            triggerOnRGEnabled.store(displayData.triggerOnRG);

            // Per-trigger delays
            triggerAfterBlockDelay.store(displayData.delayAfterBlock);
            triggerOnWakeupDelay.store(displayData.delayOnWakeup);
            triggerAfterHitstunDelay.store(displayData.delayAfterHitstun);
            triggerAfterAirtechDelay.store(displayData.delayAfterAirtech);
            triggerOnRGDelay.store(displayData.delayOnRG);

            // Per-trigger actions
            triggerAfterBlockAction.store(displayData.actionAfterBlock);
            triggerOnWakeupAction.store(displayData.actionOnWakeup);
            triggerAfterHitstunAction.store(displayData.actionAfterHitstun);
            triggerAfterAirtechAction.store(displayData.actionAfterAirtech);
            triggerOnRGAction.store(displayData.actionOnRG);

            // Per-trigger custom IDs
            triggerAfterBlockCustomID.store(displayData.customAfterBlock);
            triggerOnWakeupCustomID.store(displayData.customOnWakeup);
            triggerAfterHitstunCustomID.store(displayData.customAfterHitstun);
            triggerAfterAirtechCustomID.store(displayData.customAfterAirtech);
            triggerOnRGCustomID.store(displayData.customOnRG);

            // Per-trigger strengths
            triggerAfterBlockStrength.store(displayData.strengthAfterBlock);
            triggerOnWakeupStrength.store(displayData.strengthOnWakeup);
            triggerAfterHitstunStrength.store(displayData.strengthAfterHitstun);
            triggerAfterAirtechStrength.store(displayData.strengthAfterAirtech);
            triggerOnRGStrength.store(displayData.strengthOnRG);

            // Per-trigger macro slots
            triggerAfterBlockMacroSlot.store(displayData.macroSlotAfterBlock);
            triggerOnWakeupMacroSlot.store(displayData.macroSlotOnWakeup);
            triggerAfterHitstunMacroSlot.store(displayData.macroSlotAfterHitstun);
            triggerAfterAirtechMacroSlot.store(displayData.macroSlotAfterAirtech);
            triggerOnRGMacroSlot.store(displayData.macroSlotOnRG);
            
            // Enforce FM bypass state to match UI selection (idempotent)
            // We read current enabled state from the runtime and reapply to ensure consistency
            SetFinalMemoryBypass(IsFinalMemoryBypassEnabled());

            // Persist wake buffering toggle (already live-updated, but ensure consistency on Apply)
            // No additional action needed; atomic already updated through checkbox interaction.
            
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