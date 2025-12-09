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
#include "../include/game/per_frame_sample.h" // Unified per-frame sample (fix build: undefined PerFrameSample)
#include "../include/input/input_motion.h"
#include "../include/input/input_motion.h"
#include "../include/utils/bgm_control.h"
#include "../include/input/input_debug.h"
#include <algorithm> // Add this for std::max
#include <vector>
#include <string>
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
// Random RG control
#include "../include/game/random_rg.h"
// Random Block control
#include "../include/game/random_block.h"
#include "../include/game/auto_action.h" // g_p2ControlOverridden
// Switch players
#include "../include/utils/switch_players.h"
#include "../include/game/macro_controller.h"
#include "../include/utils/pause_integration.h"
#include "../include/game/practice_offsets.h"
#include "../include/core/version.h"
#include "../include/utils/network.h"
#include "../include/input/framestep.h"

// Add these constants at the top of the file after includes
// These are from input_motion.cpp but we need them here

// Button constants
#define BUTTON_A    GAME_INPUT_A
#define BUTTON_B    GAME_INPUT_B
#define BUTTON_C    GAME_INPUT_C
#define BUTTON_D    GAME_INPUT_D

namespace ImGuiGui {
    constexpr float kGameClientWidth = 640.0f;
    constexpr float kGameClientHeight = 480.0f;

    // Define static variable at namespace level
    static bool s_randomInputActive = false;
    // Track current RF Recovery (F4) UI selection across frames for Apply gating (0=Disabled,1=Full,2=Custom)
    static int g_f4UiMode = 0;
    static bool s_f4Blue = false;
    static int s_f4RfAmount = 100;

    static void ClampMainWindowToClientBounds() {
        ImGuiViewport* viewport = ImGui::GetMainViewport();
        if (!viewport) return;

        ImVec2 pos = ImGui::GetWindowPos();
        ImVec2 size = ImGui::GetWindowSize();

        float minX = viewport->Pos.x;
        float minY = viewport->Pos.y;
        auto minf = [](float a, float b) { return (a < b) ? a : b; };
        float boundsWidth = minf(kGameClientWidth, viewport->Size.x);
        float boundsHeight = minf(kGameClientHeight, viewport->Size.y);

        float maxX = minX + boundsWidth - size.x;
        float maxY = minY + boundsHeight - size.y;
        if (maxX < minX) maxX = minX;
        if (maxY < minY) maxY = minY;

        auto clamp = [](float value, float minV, float maxV) {
            if (value < minV) return minV;
            if (value > maxV) return maxV;
            return value;
        };

        float clampedX = clamp(pos.x, minX, maxX);
        float clampedY = clamp(pos.y, minY, maxY);
        if (clampedX != pos.x || clampedY != pos.y) {
            ImGui::SetWindowPos(ImVec2(clampedX, clampedY), ImGuiCond_Always);
            ImGui::ResetMouseDragDelta(ImGuiMouseButton_Left);
        }
    }

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
    ,ACTION_6A          // 28 = 6A (Forward A)
    ,ACTION_6B          // 29 = 6B
    ,ACTION_6C          // 30 = 6C
    ,ACTION_4A          // 31 = 4A (Back A)
    ,ACTION_4B          // 32 = 4B
    ,ACTION_4C          // 33 = 4C
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
        0,      // mainMenuSubTab (Opponent)
        0,      // autoActionSubTab (Triggers)
        0,      // helpSubTab (first help tab)
        -1,     // requestedMainMenuSubTab
        -1,     // requestedAutoActionSubTab
        -1,     // requestedHelpSubTab
        {}      // localData (initialized with default values)
    };

    // Initialize the GUI
    void Initialize() {
        // Copy current display data into our local copy
        guiState.localData = displayData;
        
        // Only show in detailed mode
        LogOut("[IMGUI_GUI] GUI state initialized", detailedLogging.load());
    }

    // Game Values Tab (reworked layout)
    void RenderGameValuesTab() {
        ImGui::PushItemWidth(120);

        if (ImGui::BeginTabBar("##MainMenuSubTabs", ImGuiTabBarFlags_None)) {
            // Apply any requested sub-tab selection once
            int rq = guiState.requestedMainMenuSubTab; guiState.requestedMainMenuSubTab = -1;
            // Opponent sub-tab (Practice Dummy controls)
            ImGuiTabItemFlags _setOpp = (rq == 0) ? ImGuiTabItemFlags_SetSelected : 0;
            if (ImGui::BeginTabItem("Opponent", nullptr, _setOpp)) {
                guiState.mainMenuSubTab = 0;
                // Control (requires Apply)
                // When auto-actions temporarily override P2 control, disable this checkbox to avoid conflicts.
                if (g_p2ControlOverridden) {
                    ImGui::BeginDisabled(true);
                    ImGui::Checkbox("Enable P2 Control (Practice Only)", &guiState.localData.p2ControlEnabled);
                    ImGui::EndDisabled();
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Disabled: P2 control is temporarily overridden by Auto Actions.");
                } else {
                    ImGui::Checkbox("Enable P2 Control (Practice Only)", &guiState.localData.p2ControlEnabled);
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Let you play P2 in Practice.\nClick Apply to enable/disable.");
                // Inform about default training hotkeys behavior while P2 control is enabled
                if (guiState.localData.p2ControlEnabled) {
                    ImGui::TextDisabled("F6/F7 training keys won't work while this is ON");
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("While P2 Control is enabled, the game's F6 (stance) and F7 (auto-block) keys are ignored.");
                }

                // Defense/Blocking
                ImGui::SeparatorText("Defense");
                const char* abNames[] = { "None", "All (F7)", "First Hit (then off)", "After First Hit (then on)", "(deprecated)" };
                if (GetCurrentGameMode() == GameMode::Practice) {
                    int abMode = GetDummyAutoBlockMode();
                    ImGui::SetNextItemWidth(200);
                    // Unlabeled combo; keep ID stable with a hidden label
                    if (ImGui::Combo("##RandomBlockMode", &abMode, abNames, 4)) { SetDummyAutoBlockMode(abMode); }
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("How the dummy blocks: Off / Block All / Block Only First Hit / Start Blocking After First Hit.\nRandom Block flips a coin each frame when it's turned ON.");
                    // After the combo: Random Block and Adaptive Stance checkboxes
                    bool randomBlock = RandomBlock::IsEnabled();
                    if (ImGui::Checkbox("Random Block", &randomBlock)) {
                        // Random Block toggles the game's autoblock flag per frame; avoid conflicts with RG modes
                        bool alwaysRG = AlwaysRG::IsEnabled();
                        bool randomRG = RandomRG::IsEnabled();
                        if (randomBlock && alwaysRG) { AlwaysRG::SetEnabled(false); }
                        if (randomBlock && randomRG) { RandomRG::SetEnabled(false); }
                        RandomBlock::SetEnabled(randomBlock);
                        LogOut(std::string("[IMGUI][RandomBlock] ") + (randomBlock ? "Enabled" : "Disabled"), true);
                        if (g_ShowRGDebugToasts.load()) {
                            DirectDrawHook::AddMessage(std::string("Random Block: ") + (randomBlock ? "ON" : "OFF"), "RANDOM_BLOCK", RGB(220,200,255), 1500, 12, 108);
                        }
                    }
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Each frame flips a coin; 50% chance to set autoblock ON during the mode's ON window.");
                    ImGui::SameLine();
                    bool adaptive = GetAdaptiveStanceEnabled();
                    if (ImGui::Checkbox("Adaptive stance", &adaptive)) { SetAdaptiveStanceEnabled(adaptive); }
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Auto pick high vs overhead/air attacks, low vs grounded attacks.");
                } else {
                    ImGui::BeginDisabled(); int dummyAB = 0; ImGui::Combo("##RandomBlockMode", &dummyAB, abNames, 4); if (ImGui::IsItemHovered()) ImGui::SetTooltip("How the dummy blocks: Off / Block All / Block Only First Hit / Start Blocking After First Hit.\nRandom Block flips a coin each frame during the mode's ON window; OFF toggles are deferred while guarding."); ImGui::EndDisabled();
                }

                // RG aids
                bool alwaysRG = AlwaysRG::IsEnabled();
                bool randomRG = RandomRG::IsEnabled();
                if (ImGui::Checkbox("Always Recoil Guard", &alwaysRG)) {
                    // Mutually exclusive with Random RG
                    if (alwaysRG && randomRG) { RandomRG::SetEnabled(false); randomRG = false; }
                    if (alwaysRG && RandomBlock::IsEnabled()) { RandomBlock::SetEnabled(false); }
                    AlwaysRG::SetEnabled(alwaysRG);
                    LogOut(std::string("[IMGUI][AlwaysRG] ") + (alwaysRG ? "Enabled" : "Disabled"), true);
                    if (g_ShowRGDebugToasts.load()) {
                        DirectDrawHook::AddMessage(std::string("Always RG: ") + (alwaysRG ? "ON" : "OFF"), "ALWAYS_RG", RGB(200,220,255), 1500, 12, 72);
                    }
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("The dummy will RG if possible.");

                ImGui::SameLine();
                if (ImGui::Checkbox("Random RG", &randomRG)) {
                    // Mutually exclusive with Always RG
                    if (randomRG && alwaysRG) { AlwaysRG::SetEnabled(false); alwaysRG = false; }
                    if (randomRG && RandomBlock::IsEnabled()) { RandomBlock::SetEnabled(false); }
                    RandomRG::SetEnabled(randomRG);
                    LogOut(std::string("[IMGUI][RandomRG] ") + (randomRG ? "Enabled" : "Disabled"), true);
                    if (g_ShowRGDebugToasts.load()) {
                        DirectDrawHook::AddMessage(std::string("Random RG: ") + (randomRG ? "ON" : "OFF"), "RANDOM_RG", RGB(200,255,200), 1500, 12, 90);
                    }
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Flip a coin each time the dummy tries to block; 50% chance to RG the move.");

                ImGui::SameLine();
                bool crg = g_counterRGEnabled.load();
                if (ImGui::Checkbox("Counter RG", &crg)) {
                    g_counterRGEnabled.store(crg);
                    LogOut(std::string("[IMGUI] Counter RG: ") + (crg ? "ENABLED" : "DISABLED"), true);
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Try to RG back after getting RG'd (Recoil Counter) where the game allows it.");

                // Stance
                int mode = 0; bool modeOk = GetPracticeBlockMode(mode);
                const char* stateNames[] = { "Standing", "Jumping", "Crouching" };
                bool adaptiveNow = GetAdaptiveStanceEnabled();
                if (modeOk) {
                    int mLocal = (mode < 0 ? 0 : (mode > 2 ? 2 : mode));
                    if (adaptiveNow) {
                        // Gray out stance selection while Adaptive stance is enabled
                        ImGui::BeginDisabled();
                        ImGui::Combo("Dummy Stance (F6)", &mLocal, stateNames, IM_ARRAYSIZE(stateNames));
                        ImGui::EndDisabled();
                        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Disabled while Adaptive stance is ON.");
                    } else {
                        if (ImGui::Combo("Dummy Stance (F6)", &mLocal, stateNames, IM_ARRAYSIZE(stateNames))) { SetPracticeBlockMode(mLocal); }
                    }
                } else {
                    ImGui::BeginDisabled(); int dummyState = 0; ImGui::Combo("Dummy Stance (F6)", &dummyState, stateNames, IM_ARRAYSIZE(stateNames)); ImGui::EndDisabled();
                }

                // Recovery & Movement
                ImGui::SeparatorText("Recovery & Movement");
                ImGui::TextUnformatted("Auto-Airtech:"); ImGui::SameLine();
                {
                    const char* airtechItems[] = { "Neutral (Disabled)", "Forward", "Backward" };
                    int airtechDir = guiState.localData.autoAirtech ? guiState.localData.airtechDirection + 1 : 0;
                    if (ImGui::Combo("##AirtechDir", &airtechDir, airtechItems, IM_ARRAYSIZE(airtechItems))) {
                        guiState.localData.autoAirtech = (airtechDir > 0);
                        guiState.localData.airtechDirection = airtechDir > 0 ? airtechDir - 1 : 0;
                    }
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Auto air-recover in the chosen direction.\nTip: EFZ locks actions for 16f after a tech; use Delay to time it.");
                    ImGui::SameLine(); ImGui::TextUnformatted("Delay:");
                    int airtechDelay = guiState.localData.airtechDelay; ImGui::SameLine();
                    ImGui::PushItemWidth(60);
                    if (ImGui::InputInt("##AirtechDelay", &airtechDelay)) { guiState.localData.airtechDelay = CLAMP(airtechDelay, 0, 60); }
                    ImGui::SameLine(); ImGui::TextUnformatted("frames");
                    ImGui::PopItemWidth();
                }

                bool aj = guiState.localData.autoJump;
                if (ImGui::Checkbox("Auto-Jump", &aj)) { guiState.localData.autoJump = aj; }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Make the selected side(s) jump automatically when able.");
                ImGui::SameLine(); ImGui::TextUnformatted("Direction:");
                {
                    const char* jumpDirs[] = { "Neutral", "Forward", "Backward" };
                    int jdir = guiState.localData.jumpDirection; ImGui::SameLine();
                    if (ImGui::Combo("##JumpDir", &jdir, jumpDirs, IM_ARRAYSIZE(jumpDirs))) {
                        guiState.localData.jumpDirection = (jdir < 0 ? 0 : (jdir > 2 ? 2 : jdir));
                    }
                    ImGui::SameLine(); ImGui::TextUnformatted("Apply To:");
                    const char* jumpTargets[] = { "P1 Only", "P2 Only", "Both Players" };
                    int jtarget = guiState.localData.jumpTarget - 1;
                    ImGui::SameLine(); if (ImGui::Combo("##JumpTarget", &jtarget, jumpTargets, IM_ARRAYSIZE(jumpTargets))) {
                        guiState.localData.jumpTarget = (jtarget < 0 ? 1 : (jtarget > 2 ? 3 : jtarget + 1));
                    }
                }

                ImGui::EndTabItem();
            }

            // Values sub-tab (P1/P2 values)
            ImGuiTabItemFlags _setVals = (rq == 1) ? ImGuiTabItemFlags_SetSelected : 0;
            if (ImGui::BeginTabItem("Values", nullptr, _setVals)) {
                guiState.mainMenuSubTab = 1;
                // Detect opening of the Values tab to resync dropdown defaults once
                bool valuesJustOpened = ImGui::IsItemActivated();
                // Read engine regen params for debug/gating using stateful inference to avoid false F4
                uint16_t engineParamA=0, engineParamB=0; EngineRegenMode regenMode = EngineRegenMode::Unknown;
                bool gotParams = GetEngineRegenStatus(regenMode, engineParamA, engineParamB);
                // Automatic Recovery control (top of section)
                ImGui::SeparatorText("Automatic Recovery (F5)");
                int curAutoIdx = 0; // Disabled
                if (gotParams && regenMode == EngineRegenMode::F5_FullOrPreset) {
                    curAutoIdx = (engineParamB == 3332) ? 2 : 1; // 1=Full values (A==1000/2000), 2=FM values (B==3332)
                }
                int autoIdx = curAutoIdx;
                const char* autoItems[] = { "Disabled", "Full values", "FM values (3332)" };
                ImGui::SetNextItemWidth(260);
                if (ImGui::Combo("##AutoRecoveryMode", &autoIdx, autoItems, IM_ARRAYSIZE(autoItems))) {
                    // Apply immediately on change
                    if (autoIdx == 0) {
                        // Disabled
                        WriteEngineRegenParams(0, 0);
                    } else if (autoIdx == 1) {
                        // Full values
                        ForceEngineF5Full();
                    } else if (autoIdx == 2) {
                        // FM values
                        WriteEngineRegenParams(1000, 3332);
                    }

                    // Additionally, set HP/Meter/RF in display and memory for both players
                    // Per request: ONLY when switching FROM Full/FM TO Disabled, snap values to 9999/0/0.0
                    if (autoIdx != curAutoIdx && autoIdx == 0 && (curAutoIdx == 1 || curAutoIdx == 2)) {
                        uintptr_t base = GetEFZBase();
                        if (base) {
                            // Targets when disabling Automatic Recovery
                            int hpTarget = MAX_HP; // 9999
                            int hpTargetP2 = MAX_HP; // 9999
                            WORD meterTarget = 0; // always 0 for both Full/FM per request
                            double rfTarget = 0.0; // always 0.0 for both Full/FM per request

                            // Update displayData (used by Apply flow)
                            displayData.hp1 = hpTarget;
                            displayData.hp2 = hpTargetP2;
                            displayData.meter1 = meterTarget;
                            displayData.meter2 = meterTarget;
                            displayData.rf1 = rfTarget;
                            displayData.rf2 = rfTarget;

                            // Update ImGui-local mirrors so the Values tab reflects the snap immediately
                            guiState.localData.hp1 = hpTarget;
                            guiState.localData.hp2 = hpTargetP2;
                            guiState.localData.meter1 = meterTarget;
                            guiState.localData.meter2 = meterTarget;
                            guiState.localData.rf1 = rfTarget;
                            guiState.localData.rf2 = rfTarget;
                            // RF color should be Red when Disabled per request
                            guiState.localData.p1BlueIC = false;
                            guiState.localData.p2BlueIC = false;

                            // Write to memory immediately
                            uintptr_t p1B=0, p2B=0; SafeReadMemory(base + EFZ_BASE_OFFSET_P1, &p1B, sizeof(p1B)); SafeReadMemory(base + EFZ_BASE_OFFSET_P2, &p2B, sizeof(p2B));
                            if (p1B && p2B) {
                                // HP
                                SafeWriteMemory(p1B + HP_OFFSET, &hpTarget, sizeof(hpTarget));
                                SafeWriteMemory(p1B + HP_BAR_OFFSET, &hpTarget, sizeof(hpTarget));
                                SafeWriteMemory(p2B + HP_OFFSET, &hpTargetP2, sizeof(hpTargetP2));
                                SafeWriteMemory(p2B + HP_BAR_OFFSET, &hpTargetP2, sizeof(hpTargetP2));
                                // Meter
                                SafeWriteMemory(p1B + METER_OFFSET, &meterTarget, sizeof(meterTarget));
                                SafeWriteMemory(p2B + METER_OFFSET, &meterTarget, sizeof(meterTarget));
                                // RF and IC color (force Red)
                                (void)SetRFValuesDirect(rfTarget, rfTarget);
                                SetICColorDirect(false, false);
                                LogOut("[IMGUI][F5] Disabled from " + std::to_string(curAutoIdx) + ": Applied targets HP=9999, Meter=0, RF=0.0 and IC=Red for both players", detailedLogging.load());
                            }
                        }
                    }
                }
                ImGui::SameLine();
                ImGui::TextDisabled("(applies immediately)");
                // RF Recovery (F4) control (above manual values)
                ImGui::Separator();
                ImGui::SeparatorText("RF Recovery (F4)");
                int curF4Idx = 0; // Disabled
                bool derivedRfValid = false;
                float derivedRf = 0.0f;
                bool derivedBlue = false;
                if (gotParams && engineParamB == 9999) {
                    if (engineParamA == 0) {
                        curF4Idx = 0;
                    } else if (engineParamA == 1000) {
                        curF4Idx = 1; // Full Blue
                    } else {
                        curF4Idx = 2; // Custom tuning
                        if (DeriveRfFromParamA(engineParamA, derivedRf, derivedBlue)) {
                            derivedRfValid = true;
                        }
                    }
                }
                static int s_f4ModeIdx = 0; // Disabled, Full, Custom
                // Always reflect actual engine state to keep combobox in sync
                s_f4ModeIdx = curF4Idx;
                if (curF4Idx == 2 && derivedRfValid) {
                    s_f4Blue = derivedBlue;
                    int roundedRf = (int)(derivedRf + 0.5f);
                    if (roundedRf < 0) roundedRf = 0;
                    if (roundedRf > (int)MAX_RF) roundedRf = (int)MAX_RF;
                    s_f4RfAmount = roundedRf;
                }
                const char* f4Items[] = { "Disabled", "Full (Blue 1000)", "Custom" };
                ImGui::SetNextItemWidth(260);
                bool f5Active = (regenMode == EngineRegenMode::F5_FullOrPreset);
                if (f5Active) ImGui::BeginDisabled();
                int prevF4Idx = s_f4ModeIdx;
                ImGui::Combo("##F4Mode", &s_f4ModeIdx, f4Items, IM_ARRAYSIZE(f4Items));
                g_f4UiMode = s_f4ModeIdx; // persist selection for Apply gating
                // Apply immediately on mode change
                if (s_f4ModeIdx != prevF4Idx) {
                    if (s_f4ModeIdx == 0) {
                        // Clear B to disable recovery; zero A for clarity
                        WriteEngineRegenParams(0, 0);
                    } else if (s_f4ModeIdx == 1) {
                        // Full RF on Blue gauge
                        WriteEngineRegenParams(1000, 9999);
                    }
                }
                if (s_f4ModeIdx == 2) {
                    // Custom: choose Red/Blue and RF amount 0..1000, map to Param A, and set B=9999. Apply on any change.
                    bool writeNow = false;
                    ImGui::TextUnformatted("Color:"); ImGui::SameLine();
                    bool wasBlue = s_f4Blue;
                    if (ImGui::RadioButton("Red##F4", !s_f4Blue)) { s_f4Blue = false; }
                    ImGui::SameLine(); if (ImGui::RadioButton("Blue##F4", s_f4Blue)) { s_f4Blue = true; }
                    if (s_f4Blue != wasBlue) writeNow = true;
                    ImGui::SetNextItemWidth(200);
                    int rfPrev = s_f4RfAmount;
                    if (ImGui::InputInt("RF amount (0..1000)", &s_f4RfAmount)) {
                        if (s_f4RfAmount < 0) s_f4RfAmount = 0; else if (s_f4RfAmount > 1000) s_f4RfAmount = 1000;
                        if (s_f4RfAmount != rfPrev) writeNow = true;
                    }
                    if (prevF4Idx != 2) writeNow = true;
                    if (writeNow) {
                        int rf = s_f4RfAmount;
                        uint16_t a = 0;
                        if (!s_f4Blue) {
                            // Red: A = RF (0..999)
                            if (rf > 999) rf = 999;
                            a = (uint16_t)rf;
                        } else {
                            // Blue: A = (rf==1000 ? 1000 : 2000 - rf)
                            a = (rf >= 1000) ? 1000 : (uint16_t)(2000 - rf);
                        }
                        WriteEngineRegenParams(a, 9999);
                    }
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Type a value or use arrows to set RF recovery amount (0-999 for Red, 1000 - Blue \n If <1000 and Blue is selected - will make the gauge blue with the said values).");
                }
                if (f5Active) {
                    ImGui::EndDisabled();
                    ImGui::SameLine();
                    ImGui::TextDisabled("(disabled: Automatic Recovery enabled)");
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Disable Automatic Recovery to adjust RF Recovery (F4).");
                }
                
                // Track if F4 is actively controlling values (anything except Disabled)
                bool f4Active = (s_f4ModeIdx != 0);
                ImGui::SameLine();
                ImGui::TextDisabled("(also maxes out HP and Meter)");
                // Lock entire Values when engine F5 cycle OR F4 fine-tune is active, or when user selected F4 mode != Disabled
                bool engineLocksValues = (regenMode == EngineRegenMode::F5_FullOrPreset) || (regenMode == EngineRegenMode::F4_FineTuneActive);
                // Continuous Recovery global lock: if either player has any active CR target (hp/meter/rf)
                bool crAny = (guiState.localData.p1ContinuousRecoveryEnabled && (guiState.localData.p1RecoveryHpMode>0 || guiState.localData.p1RecoveryMeterMode>0 || guiState.localData.p1RecoveryRfMode>0)) ||
                              (guiState.localData.p2ContinuousRecoveryEnabled && (guiState.localData.p2RecoveryHpMode>0 || guiState.localData.p2RecoveryMeterMode>0 || guiState.localData.p2RecoveryRfMode>0));
                bool globalValuesLocked = engineLocksValues || crAny || f4Active;
                // Distinct section for manual values below F4/F5
                ImGui::Separator();
                ImGui::SeparatorText("Manual Values");
                if (globalValuesLocked) ImGui::BeginDisabled();
                if (ImGui::BeginTable("values_table", 3, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp)) {
                    // Compute a compact label column width based on the longest label text
                    const char* labelTexts[] = { "HP", "Meter", "RF", "Freeze RF", "RF color", "X", "Y" };
                    float maxLabelW = 0.0f;
                    for (const char* s : labelTexts) {
                        maxLabelW = (std::max)(maxLabelW, ImGui::CalcTextSize(s).x);
                    }
                    const ImGuiStyle& st = ImGui::GetStyle();
                    const float labelColWidth = maxLabelW + st.CellPadding.x * 2.0f + 6.0f; // small breathing space
                    ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, labelColWidth);
                    ImGui::TableSetupColumn("P1");
                    ImGui::TableSetupColumn("P2");

                    auto headerCell = [&](const char* label, ImVec4 color){
                        ImGui::TableNextColumn();
                        ImGui::TextColored(color, "%s", label);
                    };

                    // Header row: empty label + P1/P2 names
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn(); ImGui::TextUnformatted("");
                    headerCell((guiState.localData.p1CharName[0] ? guiState.localData.p1CharName : "Unknown"), ImVec4(0.5f, 0.8f, 1.0f, 1.0f));
                    headerCell((guiState.localData.p2CharName[0] ? guiState.localData.p2CharName : "Unknown"), ImVec4(1.0f, 0.5f, 0.5f, 1.0f));

                    struct ManualPreset {
                        const char* label;
                        int hp;
                        int meter;
                        float rf;
                    };
                    static const ManualPreset kManualPresets[] = {
                        { "Default", 9999, 0, 0.0f },
                        { "Max", 9999, MAX_METER, MAX_RF },
                        { "FM", 3333, MAX_METER, MAX_RF }
                    };
                    auto applyPreset = [&](int player, const ManualPreset& preset) {
                        if (player == 1) {
                            guiState.localData.hp1 = CLAMP(preset.hp, 0, MAX_HP);
                            guiState.localData.meter1 = CLAMP(preset.meter, 0, MAX_METER);
                            guiState.localData.rf1 = CLAMP(preset.rf, 0.0f, MAX_RF);
                        } else {
                            guiState.localData.hp2 = CLAMP(preset.hp, 0, MAX_HP);
                            guiState.localData.meter2 = CLAMP(preset.meter, 0, MAX_METER);
                            guiState.localData.rf2 = CLAMP(preset.rf, 0.0f, MAX_RF);
                        }
                    };
                    auto renderPresetButtons = [&](int player) {
                        bool first = true;
                        for (const auto& preset : kManualPresets) {
                            if (!first) ImGui::SameLine();
                            first = false;
                            std::string btnLabel = std::string(preset.label) + (player == 1 ? "##preset_p1_" : "##preset_p2_") + preset.label;
                            if (ImGui::SmallButton(btnLabel.c_str())) {
                                applyPreset(player, preset);
                            }
                        }
                    };

                    // Preset buttons row
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn(); ImGui::TextUnformatted("Presets");
                    ImGui::TableNextColumn(); renderPresetButtons(1);
                    ImGui::TableNextColumn(); renderPresetButtons(2);

                    // HP
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn(); ImGui::TextUnformatted("HP");
                    ImGui::TableNextColumn(); {
                        int v = guiState.localData.hp1; if (ImGui::InputInt("##hp_p1", &v)) guiState.localData.hp1 = CLAMP(v, 0, MAX_HP);
                        ImGui::SameLine(); if (ImGui::SmallButton("Max##hp_p1")) guiState.localData.hp1 = MAX_HP;
                        ImGui::SameLine(); if (ImGui::SmallButton("Zero##hp_p1")) guiState.localData.hp1 = 0;
                    }
                    ImGui::TableNextColumn(); {
                        int v = guiState.localData.hp2; if (ImGui::InputInt("##hp_p2", &v)) guiState.localData.hp2 = CLAMP(v, 0, MAX_HP);
                        ImGui::SameLine(); if (ImGui::SmallButton("Max##hp_p2")) guiState.localData.hp2 = MAX_HP;
                        ImGui::SameLine(); if (ImGui::SmallButton("Zero##hp_p2")) guiState.localData.hp2 = 0;
                    }

                    // Meter
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn(); ImGui::TextUnformatted("Meter");
                    ImGui::TableNextColumn(); {
                        int v = guiState.localData.meter1; if (ImGui::InputInt("##meter_p1", &v)) guiState.localData.meter1 = CLAMP(v, 0, MAX_METER);
                        ImGui::SameLine(); if (ImGui::SmallButton("Max##meter_p1")) guiState.localData.meter1 = MAX_METER;
                        ImGui::SameLine(); if (ImGui::SmallButton("Zero##meter_p1")) guiState.localData.meter1 = 0;
                    }
                    ImGui::TableNextColumn(); {
                        int v = guiState.localData.meter2; if (ImGui::InputInt("##meter_p2", &v)) guiState.localData.meter2 = CLAMP(v, 0, MAX_METER);
                        ImGui::SameLine(); if (ImGui::SmallButton("Max##meter_p2")) guiState.localData.meter2 = MAX_METER;
                        ImGui::SameLine(); if (ImGui::SmallButton("Zero##meter_p2")) guiState.localData.meter2 = 0;
                    }

                    // RF
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn(); ImGui::TextUnformatted("RF");
                    ImGui::TableNextColumn(); {
                        float v = (float)guiState.localData.rf1; if (ImGui::InputFloat("##rf_p1", &v, 0.1f, 1.0f, "%.1f")) guiState.localData.rf1 = CLAMP(v, 0.0f, MAX_RF);
                        ImGui::SameLine(); if (ImGui::SmallButton("Max##rf_p1")) guiState.localData.rf1 = MAX_RF;
                        ImGui::SameLine(); if (ImGui::SmallButton("Zero##rf_p1")) guiState.localData.rf1 = 0.0f;
                    }
                    ImGui::TableNextColumn(); {
                        float v = (float)guiState.localData.rf2; if (ImGui::InputFloat("##rf_p2", &v, 0.1f, 1.0f, "%.1f")) guiState.localData.rf2 = CLAMP(v, 0.0f, MAX_RF);
                        ImGui::SameLine(); if (ImGui::SmallButton("Max##rf_p2")) guiState.localData.rf2 = MAX_RF;
                        ImGui::SameLine(); if (ImGui::SmallButton("Zero##rf_p2")) guiState.localData.rf2 = 0.0f;
                    }

                    // Freeze RF + Lock color (placed directly below RF)
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn(); ImGui::TextUnformatted("Freeze RF");
                    ImGui::TableNextColumn();
                    {
                        static bool s_uiFreezeP1 = false; static bool s_uiFreezeP1ColorBlue = true;
                        bool fr1 = s_uiFreezeP1; if (ImGui::Checkbox("Freeze##rf_p1", &fr1)) {
                            s_uiFreezeP1 = fr1;
                            if (fr1) { StartRFFreezeOneFromUI(1, guiState.localData.rf1); }
                            else { StopRFFreezePlayer(1); }
                        }
                        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Continuously holds RF at the current value.");
                        ImGui::SameLine(); ImGui::TextDisabled("Lock:"); ImGui::SameLine();
                        bool lockBlue1 = s_uiFreezeP1ColorBlue; if (ImGui::RadioButton("Red##p1Lock", !lockBlue1)) { s_uiFreezeP1ColorBlue = false; }
                        ImGui::SameLine(); if (ImGui::RadioButton("Blue##p1Lock", lockBlue1)) { s_uiFreezeP1ColorBlue = true; }
                        SetRFFreezeColorDesired(1, s_uiFreezeP1, s_uiFreezeP1ColorBlue);
                    }
                    ImGui::TableNextColumn();
                    {
                        static bool s_uiFreezeP2 = false; static bool s_uiFreezeP2ColorBlue = true;
                        bool fr2 = s_uiFreezeP2; if (ImGui::Checkbox("Freeze##rf_p2", &fr2)) {
                            s_uiFreezeP2 = fr2;
                            if (fr2) { StartRFFreezeOneFromUI(2, guiState.localData.rf2); }
                            else { StopRFFreezePlayer(2); }
                        }
                        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Continuously holds RF at the current value.");
                        ImGui::SameLine(); ImGui::TextDisabled("Lock:"); ImGui::SameLine();
                        bool lockBlue2 = s_uiFreezeP2ColorBlue; if (ImGui::RadioButton("Red##p2Lock", !lockBlue2)) { s_uiFreezeP2ColorBlue = false; }
                        ImGui::SameLine(); if (ImGui::RadioButton("Blue##p2Lock", lockBlue2)) { s_uiFreezeP2ColorBlue = true; }
                        SetRFFreezeColorDesired(2, s_uiFreezeP2, s_uiFreezeP2ColorBlue);
                    }

                    // RF color selection (radio buttons choose desired color; applied on Apply)
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn(); ImGui::TextUnformatted("RF color");
                    // Determine if either side's color is currently managed by Freeze/CR
                    auto crColorManagedRow = [&](int player)->bool {
                        if (player == 1) {
                            if (!guiState.localData.p1ContinuousRecoveryEnabled) return false;
                            if (guiState.localData.p1RecoveryRfMode == 3 || guiState.localData.p1RecoveryRfMode == 4) return true; // Red presets force Red
                            if (guiState.localData.p1RecoveryRfMode == 5 && guiState.localData.p1RecoveryRfForceBlueIC) return true; // Custom+BIC forces Blue
                            return false;
                        } else {
                            if (!guiState.localData.p2ContinuousRecoveryEnabled) return false;
                            if (guiState.localData.p2RecoveryRfMode == 3 || guiState.localData.p2RecoveryRfMode == 4) return true;
                            if (guiState.localData.p2RecoveryRfMode == 5 && guiState.localData.p2RecoveryRfForceBlueIC) return true;
                            return false;
                        }
                    };
                    bool p1ManagedColor = IsRFFreezeColorManaging(1) || crColorManagedRow(1);
                    bool p2ManagedColor = IsRFFreezeColorManaging(2) || crColorManagedRow(2);

                    ImGui::TableNextColumn();
                    {
                        if (p1ManagedColor) ImGui::BeginDisabled();
                        bool p1Blue = guiState.localData.p1BlueIC;
                        if (ImGui::RadioButton("Red##p1RF", !p1Blue)) { guiState.localData.p1BlueIC = false; p1Blue = false; }
                        ImGui::SameLine();
                        if (ImGui::RadioButton("Blue##p1RF", p1Blue)) { guiState.localData.p1BlueIC = true; p1Blue = true; }
                        if (p1ManagedColor) {
                            ImGui::EndDisabled();
                            ImGui::SameLine();
                            ImGui::TextDisabled("(managed)");
                            if (ImGui::IsItemHovered()) ImGui::SetTooltip("P1 color is currently managed by RF Freeze or Continuous Recovery.");
                        } else {
                            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Select P1 RF gauge color. Takes effect on Apply. Not a continuous lock.");
                        }
                    }
                    ImGui::TableNextColumn();
                    {
                        if (p2ManagedColor) ImGui::BeginDisabled();
                        bool p2Blue = guiState.localData.p2BlueIC;
                        if (ImGui::RadioButton("Red##p2RF", !p2Blue)) { guiState.localData.p2BlueIC = false; p2Blue = false; }
                        ImGui::SameLine();
                        if (ImGui::RadioButton("Blue##p2RF", p2Blue)) { guiState.localData.p2BlueIC = true; p2Blue = true; }
                        if (p2ManagedColor) {
                            ImGui::EndDisabled();
                            ImGui::SameLine();
                            ImGui::TextDisabled("(managed)");
                            if (ImGui::IsItemHovered()) ImGui::SetTooltip("P2 color is currently managed by RF Freeze or Continuous Recovery.");
                        } else {
                            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Select P2 RF gauge color. Takes effect on Apply. Not a continuous lock.");
                        }
                    }

                    // Temporarily unlock X/Y even when values are globally locked by F4/F5/CR
                    if (globalValuesLocked) ImGui::EndDisabled();
                    // X position
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn(); ImGui::TextUnformatted("X");
                    ImGui::TableNextColumn(); { float v = (float)guiState.localData.x1; if (ImGui::InputFloat("##x_p1", &v, 1.0f, 10.0f, "%.2f")) guiState.localData.x1 = v; }
                    ImGui::TableNextColumn(); { float v = (float)guiState.localData.x2; if (ImGui::InputFloat("##x_p2", &v, 1.0f, 10.0f, "%.2f")) guiState.localData.x2 = v; }

                    // Y position
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn(); ImGui::TextUnformatted("Y");
                    ImGui::TableNextColumn(); { float v = (float)guiState.localData.y1; if (ImGui::InputFloat("##y_p1", &v, 1.0f, 10.0f, "%.2f")) guiState.localData.y1 = v; }
                    ImGui::TableNextColumn(); { float v = (float)guiState.localData.y2; if (ImGui::InputFloat("##y_p2", &v, 1.0f, 10.0f, "%.2f")) guiState.localData.y2 = v; }
                    if (globalValuesLocked) ImGui::BeginDisabled();

                    ImGui::EndTable();
                }
                if (globalValuesLocked) ImGui::EndDisabled();
                // Short guidance message when engine regen is active
                if (regenMode == EngineRegenMode::F4_FineTuneActive) {
                    ImGui::TextWrapped("Currently, the game has RF Recovery enabled (F4). To edit values here, set Automatic Recovery (F5) to Disabled in-game.");
                } else if (regenMode == EngineRegenMode::F5_FullOrPreset) {
                    ImGui::TextWrapped("Currently, the game has Automatic regeneration enabled (F5). To edit values here, set Automatic Recovery (F5) to Disabled in-game.");
                }
                ImGui::Dummy(ImVec2(1, 4));
                ImGui::EndTabItem();
            }

            // Options sub-tab
            ImGuiTabItemFlags _setOpts = (rq == 2) ? ImGuiTabItemFlags_SetSelected : 0;
            if (ImGui::BeginTabItem("Options", nullptr, _setOpts)) {
                guiState.mainMenuSubTab = 2;
                // Continuous Recovery (Per-Player)
                ImGui::SeparatorText("Continuous Recovery");
                ImGui::TextWrapped("Restores values when returning to neutral/crouch/jump. Per-player; defaults OFF.");
                if (ImGui::IsItemHovered()) {
                    ImGui::BeginTooltip();
                    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
                    ImGui::TextUnformatted("On transitions from unactionable states into neutral/crouch/jump, restore HP/Meter/RF to chosen targets. ");
                    ImGui::PopTextWrapPos();
                    ImGui::EndTooltip();
                }

                auto renderContRecFor = [&](int player){
                    const bool isP1 = (player==1);
                    // Row: Enable
                    bool en = isP1 ? guiState.localData.p1ContinuousRecoveryEnabled : guiState.localData.p2ContinuousRecoveryEnabled;
                    if (ImGui::Checkbox(isP1?"Enable (P1)##contrecp1":"Enable (P2)##contrecp2", &en)) {
                        if (isP1) guiState.localData.p1ContinuousRecoveryEnabled = en; else guiState.localData.p2ContinuousRecoveryEnabled = en;
                        // When enabling CR, disable engine-managed recovery (F4/F5) to avoid conflicts
                        if (en) {
                            WriteEngineRegenParams(0, 0);
                        }
                    }
                    // HP presets
                    ImGui::TextUnformatted("HP:"); ImGui::SameLine();
                    const char* hpItems[] = { "Off", "Full (9999)", "FM (3332)", "Custom" };
                    ImGui::SetNextItemWidth(140);
                    int hpMode = isP1 ? guiState.localData.p1RecoveryHpMode : guiState.localData.p2RecoveryHpMode;
                    if (ImGui::Combo(isP1?"##contrec_hp_p1":"##contrec_hp_p2", &hpMode, hpItems, IM_ARRAYSIZE(hpItems))) {
                        if (isP1) guiState.localData.p1RecoveryHpMode = hpMode; else guiState.localData.p2RecoveryHpMode = hpMode;
                    }
                    if (hpMode == 3) {
                        int hpVal = isP1 ? guiState.localData.p1RecoveryHpCustom : guiState.localData.p2RecoveryHpCustom;
                        ImGui::SetNextItemWidth(120);
                        if (ImGui::InputInt(isP1?"##contrec_hp_custom_p1":"##contrec_hp_custom_p2", &hpVal)) {
                            hpVal = CLAMP(hpVal, 0, MAX_HP);
                            if (isP1) guiState.localData.p1RecoveryHpCustom = hpVal; else guiState.localData.p2RecoveryHpCustom = hpVal;
                        }
                    }
                    // Meter presets
                    ImGui::TextUnformatted("Meter:"); ImGui::SameLine();
                    const char* mItems[] = { "Off", "0", "1000", "2000", "3000", "Custom" };
                    ImGui::SetNextItemWidth(140);
                    int mMode = isP1 ? guiState.localData.p1RecoveryMeterMode : guiState.localData.p2RecoveryMeterMode;
                    if (ImGui::Combo(isP1?"##contrec_meter_p1":"##contrec_meter_p2", &mMode, mItems, IM_ARRAYSIZE(mItems))) {
                        if (isP1) guiState.localData.p1RecoveryMeterMode = mMode; else guiState.localData.p2RecoveryMeterMode = mMode;
                    }
                    if (mMode == 5) {
                        int mVal = isP1 ? guiState.localData.p1RecoveryMeterCustom : guiState.localData.p2RecoveryMeterCustom;
                        ImGui::SetNextItemWidth(120);
                        if (ImGui::InputInt(isP1?"##contrec_meter_custom_p1":"##contrec_meter_custom_p2", &mVal)) {
                            mVal = CLAMP(mVal, 0, MAX_METER);
                            if (isP1) guiState.localData.p1RecoveryMeterCustom = mVal; else guiState.localData.p2RecoveryMeterCustom = mVal;
                        }
                    }
                    // RF presets
                    ImGui::TextUnformatted("RF:"); ImGui::SameLine();
                    const char* rfItems[] = { "Off", "Zero", "Full (1000)", "Red (500)", "Red Max (999)", "Custom" };
                    ImGui::SetNextItemWidth(160);
                    int rfMode = isP1 ? guiState.localData.p1RecoveryRfMode : guiState.localData.p2RecoveryRfMode;
                    if (ImGui::Combo(isP1?"##contrec_rf_p1":"##contrec_rf_p2", &rfMode, rfItems, IM_ARRAYSIZE(rfItems))) {
                        if (isP1) guiState.localData.p1RecoveryRfMode = rfMode; else guiState.localData.p2RecoveryRfMode = rfMode;
                        // Red != BIC: ensure BIC flag is cleared when choosing Red presets (500 or 999)
                        if (rfMode == 3 || rfMode == 4) {
                            if (isP1) guiState.localData.p1RecoveryRfForceBlueIC = false; else guiState.localData.p2RecoveryRfForceBlueIC = false;
                        }
                    }
                    if (rfMode == 5) {
                        float rfVal = isP1 ? (float)guiState.localData.p1RecoveryRfCustom : (float)guiState.localData.p2RecoveryRfCustom;
                        ImGui::SetNextItemWidth(120);
                        if (ImGui::InputFloat(isP1?"##contrec_rf_custom_p1":"##contrec_rf_custom_p2", &rfVal, 0.1f, 1.0f, "%.1f")) {
                            rfVal = CLAMP(rfVal, 0.0f, MAX_RF);
                            if (isP1) guiState.localData.p1RecoveryRfCustom = rfVal; else guiState.localData.p2RecoveryRfCustom = rfVal;
                        }
                        bool bic = isP1 ? guiState.localData.p1RecoveryRfForceBlueIC : guiState.localData.p2RecoveryRfForceBlueIC;
                        if (ImGui::Checkbox(isP1?"BIC##contrec_p1":"BIC##contrec_p2", &bic)) {
                            if (isP1) guiState.localData.p1RecoveryRfForceBlueIC = bic; else guiState.localData.p2RecoveryRfForceBlueIC = bic;
                        }
                        if (ImGui::IsItemHovered()) {
                            ImGui::BeginTooltip();
                            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
                            ImGui::TextUnformatted("Force Blue IC when applying Custom RF.");
                            ImGui::PopTextWrapPos();
                            ImGui::EndTooltip();
                        }
                    }
                };

                // Two columns: P1 and P2
                ImGui::Columns(2, "contrec_cols", false);
                ImGui::TextColored(ImVec4(0.5f,0.8f,1.0f,1.0f), "P1");
                renderContRecFor(1);
                ImGui::NextColumn();
                ImGui::TextColored(ImVec4(1.0f,0.5f,0.5f,1.0f), "P2");
                renderContRecFor(2);
                ImGui::Columns(1);

                ImGui::Dummy(ImVec2(1, 4));

                bool fmBypass = IsFinalMemoryBypassEnabled();
                if (ImGui::Checkbox("Final Memory: Allow at any HP", &fmBypass)) {
                    (void)SetFinalMemoryBypass(fmBypass);
                    LogOut(std::string("[IMGUI][FM] ") + (fmBypass ? "Enabled" : "Disabled") + " FM HP bypass.", true);
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Let any character use FM regardless of HP.");

                // Frame Advantage overlay visibility
                bool showFA = g_showFrameAdvantageOverlay.load();
                if (ImGui::Checkbox("Show Frame Advantage Overlay", &showFA)) {
                    g_showFrameAdvantageOverlay.store(showFA);
                    // If turning off, immediately clear any existing FA messages
                    if (!showFA) {
                        if (g_FrameAdvantageId != -1) { DirectDrawHook::RemovePermanentMessage(g_FrameAdvantageId); g_FrameAdvantageId = -1; }
                        if (g_FrameAdvantage2Id != -1) { DirectDrawHook::RemovePermanentMessage(g_FrameAdvantage2Id); g_FrameAdvantage2Id = -1; }
                    }
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Toggles the numeric frame advantage readout (including RG FA1/FA2).");

                // Framestep mode (Vanilla EFZ only)
                if (GetEfzRevivalVersion() == EfzRevivalVersion::Vanilla) {
                    ImGui::Spacing();
                    ImGui::SeparatorText("Framestep (Vanilla EFZ)");
                    ImGui::Text("Step Mode:");
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(200);
                    const char* stepModeItems[] = { "Full Frames", "Subframes" };
                    int currentMode = (Framestep::GetStepMode() == Framestep::StepMode::FullFrame) ? 0 : 1;
                    if (ImGui::Combo("##FramestepMode", &currentMode, stepModeItems, IM_ARRAYSIZE(stepModeItems))) {
                        Framestep::SetStepMode(currentMode == 0 ? Framestep::StepMode::FullFrame : Framestep::StepMode::Subframe);
                    }
                    if (ImGui::IsItemHovered()) {
                        ImGui::BeginTooltip();
                        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
                        ImGui::TextUnformatted("Full Frames: Each step advances 3 subframes (64fps visual frame).");
                        ImGui::TextUnformatted("Subframes: Each step advances 1 logical frame (192fps). Shows fractional visual frames.");
                        //ImGui::TextUnformatted("\nNote: Input buffer updates at 64fps (every 3 subframes), so buffer index advances every 3rd step in Subframe mode.");
                        ImGui::TextUnformatted("\nHotkeys: Space = Pause/Resume, P = Step Forward");
                        ImGui::TextUnformatted("Only works in Practice mode (or any mode if 'Restrict to Practice' is off).");
                        ImGui::PopTextWrapPos();
                        ImGui::EndTooltip();
                    }
                }

                ImGui::EndTabItem();
            }

            ImGui::EndTabBar();
        }

        ImGui::PopItemWidth();
    }

    // Auto Action Tab
    void RenderAutoActionTab() {
        // Sub-tabs: Triggers | Macros
    if (ImGui::BeginTabBar("##AutoActionTabs", ImGuiTabBarFlags_None)) {
                // Track Macros tab open/close to trigger reload on entry
                static bool s_macrosActivePrev = false;
                bool macrosActiveThisFrame = false;
                int rq2 = guiState.requestedAutoActionSubTab; guiState.requestedAutoActionSubTab = -1;
                // Triggers sub-tab
                ImGuiTabItemFlags _setTrig = (rq2 == 0) ? ImGuiTabItemFlags_SetSelected : 0;
                if (ImGui::BeginTabItem("Triggers", nullptr, _setTrig)) {
                    guiState.autoActionSubTab = 0;
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
                    ImGui::SetTooltip("On: buffer wake moves slightly early. Off: do them on the first actionable frame.\nUseful for testing tight wakeup timing.");
                }

                // Global: Randomize all triggers toggle (placed with master/wake settings)
                {
                    bool randTrig = guiState.localData.randomizeTriggers;
                    if (ImGui::Checkbox("Randomize chance to fire the trigger", &randTrig)) {
                        guiState.localData.randomizeTriggers = randTrig;
                    }
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("When ON, each trigger attempt has a 50% chance to be skipped.");
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
            int* macroSlot; // NEW: Per-trigger macro selection (0=None, 1..Max)
            uint32_t* poolMask; // NEW: Multi-action pool bitmask (UI motion indices)
            bool* usePool;      // NEW: Enable random pick from pool
        };
        
        // Define an array of trigger settings
                TriggerSettings triggers[] = {
                        { "After Block", &guiState.localData.triggerAfterBlock, &guiState.localData.actionAfterBlock, 
                            &guiState.localData.delayAfterBlock, &guiState.localData.strengthAfterBlock, &guiState.localData.customAfterBlock, &guiState.localData.macroSlotAfterBlock,
                            &guiState.localData.afterBlockActionPoolMask, &guiState.localData.afterBlockUseActionPool },
                        { "On Wakeup", &guiState.localData.triggerOnWakeup, &guiState.localData.actionOnWakeup, 
                            &guiState.localData.delayOnWakeup, &guiState.localData.strengthOnWakeup, &guiState.localData.customOnWakeup, &guiState.localData.macroSlotOnWakeup,
                            &guiState.localData.onWakeupActionPoolMask, &guiState.localData.onWakeupUseActionPool },
                        { "After Hitstun", &guiState.localData.triggerAfterHitstun, &guiState.localData.actionAfterHitstun, 
                            &guiState.localData.delayAfterHitstun, &guiState.localData.strengthAfterHitstun, &guiState.localData.customAfterHitstun, &guiState.localData.macroSlotAfterHitstun,
                            &guiState.localData.afterHitstunActionPoolMask, &guiState.localData.afterHitstunUseActionPool },
                        { "After Airtech", &guiState.localData.triggerAfterAirtech, &guiState.localData.actionAfterAirtech, 
                            &guiState.localData.delayAfterAirtech, &guiState.localData.strengthAfterAirtech, &guiState.localData.customAfterAirtech, &guiState.localData.macroSlotAfterAirtech,
                            &guiState.localData.afterAirtechActionPoolMask, &guiState.localData.afterAirtechUseActionPool },
                        { "On RG", &guiState.localData.triggerOnRG, &guiState.localData.actionOnRG,
                            &guiState.localData.delayOnRG, &guiState.localData.strengthOnRG, &guiState.localData.customOnRG, &guiState.localData.macroSlotOnRG,
                            &guiState.localData.onRGActionPoolMask, &guiState.localData.onRGUseActionPool }
                };
        
        // Motion list with categories - NOTE: mapping functions below must stay in sync
        struct MotionItem {
            const char* label;
            int motionIndex;      // Index for mapping
            bool isCategory;      // True for category headers (non-selectable)
            bool isSeparator;     // True for visual separator lines
        };
        
        const MotionItem motionItemsWithCategories[] = {
            // Normals category
            { "NORMALS", -1, true, false },
            { "  Standing", 0, false, false },
            { "  Crouching", 1, false, false },
            { "  Jumping", 2, false, false },
            { "  Forward", 22, false, false },
            { "  Back", 23, false, false },
            { "", -1, false, true }, // Separator
            
            // Specials category
            { "SPECIALS", -1, true, false },
            { "  236 (QCF)", 3, false, false },
            { "  623 (DP)", 4, false, false },
            { "  214 (QCB)", 5, false, false },
            { "  41236 (HCF)", 7, false, false },
            { "  421 (Half-circle Down)", 6, false, false },
            { "  412", 13, false, false },
            { "  22", 14, false, false },
            { "", -1, false, true }, // Separator
            
            // Supers category
            { "SUPERS", -1, true, false },
            { "  214236 (Hybrid)", 8, false, false },
            { "  236236 (Double QCF)", 9, false, false },
            { "  214214 (Double QCB)", 10, false, false },
            { "  641236", 11, false, false },
            { "  463214", 12, false, false },
            { "  4123641236", 15, false, false },
            { "  6321463214", 16, false, false },
            { "  Final Memory", 21, false, false },
            { "", -1, false, true }, // Separator
            
            // Others category
            { "OTHERS", -1, true, false },
            { "  Jump", 17, false, false },
            { "  Dash", 19, false, false },
            { "  Backdash", 18, false, false }
        };
        
        // Keep old flat list for backwards compatibility with some functions
        const char* motionItems[] = {
            "Standing", "Crouching", "Jumping",
            "236 (QCF)", "623 (DP)", "214 (QCB)", "421 (Half-circle Down)",
            "41236 (HCF)", "214236 (Hybrid)", "236236 (Double QCF)", "214214 (Double QCB)",
            "641236", "463214", "412", "22", "4123641236", "6321463214",
            "Jump", "Backdash", "Forward Dash", "Block", "Final Memory",
            "Forward Normal", "Back Normal"
        };

    // Compute a compact width that fits the longest action label (plus arrow/padding), so combos aren't overly wide
        ImGuiStyle& _style = ImGui::GetStyle();
        // Keep combobox compact: make it just wide enough for the longer of "Final Memory" or "Macro"
        const float _labelFinalMemory = ImGui::CalcTextSize("Final Memory").x;
        const float _labelMacro = ImGui::CalcTextSize("Macro").x;
        const float _baseline = (std::max)(_labelFinalMemory, _labelMacro);
        const float actionComboWidth = _baseline + _style.FramePadding.x * 2.0f + ImGui::GetFrameHeight();
        // Shared A/B/C/D (notated as S for Special button) choice for normals/specials strength
        const char* buttonItems[] = { "A", "B", "C", "S" };

        // Mapping helpers between UI motion indices and internal ACTION_* enums
        auto GetPostureIndexForAction = [](int action)->int {
            switch (action) {
                case ACTION_5A: case ACTION_5B: case ACTION_5C: case ACTION_5D: return 0; // Standing
                case ACTION_2A: case ACTION_2B: case ACTION_2C: case ACTION_2D: return 1; // Crouching
                case ACTION_JA: case ACTION_JB: case ACTION_JC: case ACTION_JD: return 2; // Jumping
                default: return -1;
            }
        };
        auto IsNormalAttackAction = [&](int action)->bool {
            return GetPostureIndexForAction(action) >= 0
                || action == ACTION_6A || action == ACTION_6B || action == ACTION_6C || action == ACTION_6D
                || action == ACTION_4A || action == ACTION_4B || action == ACTION_4C || action == ACTION_4D;
        };
        auto IsSpecialMoveAction = [](int action)->bool {
            switch (action) {
                case ACTION_QCF: case ACTION_DP: case ACTION_QCB: case ACTION_421:
                case ACTION_412: case ACTION_22:
                case ACTION_SUPER1: case ACTION_SUPER2: case ACTION_236236: case ACTION_214214:
                case ACTION_641236: case ACTION_463214: case ACTION_4123641236: case ACTION_6321463214:
                    return true;
                default: return false;
            }
        };
        auto MapPostureAndButtonToAction = [](int postureIdx, int buttonIdx)->int {
            buttonIdx = (buttonIdx < 0 ? 0 : (buttonIdx > 3 ? 3 : buttonIdx));
            switch (postureIdx) {
                case 0: return buttonIdx==0?ACTION_5A:(buttonIdx==1?ACTION_5B:(buttonIdx==2?ACTION_5C:ACTION_5D));
                case 1: return buttonIdx==0?ACTION_2A:(buttonIdx==1?ACTION_2B:(buttonIdx==2?ACTION_2C:ACTION_2D));
                case 2: return buttonIdx==0?ACTION_JA:(buttonIdx==1?ACTION_JB:(buttonIdx==2?ACTION_JC:ACTION_JD));
                default: return ACTION_5A;
            }
        };
        auto MapMotionIndexToAction = [&](int idx)->int {
            switch (idx) {
                case 17: return ACTION_JUMP; case 18: return ACTION_BACKDASH; case 19: return ACTION_FORWARD_DASH; case 20: return ACTION_BLOCK; case 21: return ACTION_FINAL_MEMORY;
                case 3: return ACTION_QCF; case 4: return ACTION_DP; case 5: return ACTION_QCB; case 6: return ACTION_421; case 7: return ACTION_SUPER1;
                case 8: return ACTION_SUPER2; case 9: return ACTION_236236; case 10: return ACTION_214214; case 11: return ACTION_641236; case 12: return ACTION_463214;
                case 13: return ACTION_412; case 14: return ACTION_22; case 15: return ACTION_4123641236; case 16: return ACTION_6321463214;
                case 22: return ACTION_6A; case 23: return ACTION_4A; // default A for fwd/back group; Option column refines button
                default: return ACTION_5A; // for Standing/Crouching/Jumping, actual A/B/C chosen via Option column
            }
        };
        auto GetMotionIndexForAction = [&](int action)->int {
            switch (action) {
                // Group normals
                case ACTION_5A: case ACTION_5B: case ACTION_5C: case ACTION_5D: return 0;
                case ACTION_2A: case ACTION_2B: case ACTION_2C: case ACTION_2D: return 1;
                case ACTION_JA: case ACTION_JB: case ACTION_JC: case ACTION_JD: return 2;
                // Forward/Back normals as separate groups (refined by Option)
                case ACTION_6A: case ACTION_6B: case ACTION_6C: case ACTION_6D: return 22;
                case ACTION_4A: case ACTION_4B: case ACTION_4C: case ACTION_4D: return 23;
                // Specials/Supers/Others
                case ACTION_QCF: return 3; case ACTION_DP: return 4; case ACTION_QCB: return 5; case ACTION_421: return 6; case ACTION_SUPER1: return 7;
                case ACTION_SUPER2: return 8; case ACTION_236236: return 9; case ACTION_214214: return 10; case ACTION_641236: return 11; case ACTION_463214: return 12;
                case ACTION_412: return 13; case ACTION_22: return 14; case ACTION_4123641236: return 15; case ACTION_6321463214: return 16;
                case ACTION_JUMP: return 17; case ACTION_BACKDASH: return 18; case ACTION_FORWARD_DASH: return 19; case ACTION_BLOCK: return 20; case ACTION_FINAL_MEMORY: return 21;
                default: return -1;
            }
        };
        // (quick summary and bulk utilities removed per user feedback)

    // Use default item spacing

        // Render as a table for clarity
        ImGuiTableFlags tflags = ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp;
        if (ImGui::BeginTable("auto_triggers", 6, tflags)) {
            const float onColW = ImGui::GetFrameHeight() + _style.CellPadding.x * 1.5f; // roughly checkbox size
            const float actionColW = actionComboWidth + _style.CellPadding.x * 2.0f;    // match combo width
            const float delayColW = 80.0f;                                              // small, like our input width

            ImGui::TableSetupColumn("On", ImGuiTableColumnFlags_WidthFixed, onColW);
            ImGui::TableSetupColumn("Trigger", ImGuiTableColumnFlags_WidthFixed, 110.0f);
            ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, actionColW);
            ImGui::TableSetupColumn("Button", ImGuiTableColumnFlags_WidthFixed, 140.0f);
            ImGui::TableSetupColumn("Delay", ImGuiTableColumnFlags_WidthFixed, delayColW);
            ImGui::TableSetupColumn("More", ImGuiTableColumnFlags_WidthFixed, 140.0f);
            ImGui::TableHeadersRow();

            for (int i = 0; i < IM_ARRAYSIZE(triggers); i++) {
                ImGui::PushID(i);
                ImGui::TableNextRow();

                // Column: On
                ImGui::TableNextColumn();
                {
                    // Center the checkbox horizontally in the narrow 'On' column
                    float colW = ImGui::GetColumnWidth();
                    float itemW = ImGui::GetFrameHeight(); // approximate checkbox width
                    float x0 = ImGui::GetCursorPosX();
                    float xCentered = x0 + (colW - itemW) * 0.5f;
                    // Avoid drifting into next column padding
                    ImGui::SetCursorPosX((xCentered > x0) ? xCentered : x0);
                    ImGui::Checkbox("##on", triggers[i].enabled);
                }

                // Column: Trigger name
                ImGui::TableNextColumn();
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(triggers[i].name);

                // Column: Action combo with categories (+ Multi selection popup)
                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(actionComboWidth);
                int currentMotionIndex = (*triggers[i].macroSlot > 0) ? -2 : GetMotionIndexForAction(*triggers[i].action);
                // If multi-pool enabled and has selections, show Random(n) label
                auto popcount32 = [](uint32_t m){ int c=0; while(m){ m &= (m-1); ++c; } return c; };
                int selectedCount = (*triggers[i].usePool) ? popcount32(*triggers[i].poolMask) : 0;
                const char* currentLabel = nullptr;
                char randomLabel[32];
                if (selectedCount > 0) {
                    snprintf(randomLabel, sizeof(randomLabel), "Random (%d)", selectedCount);
                    currentLabel = randomLabel;
                } else {
                    currentLabel = (*triggers[i].macroSlot > 0) ? "Macro" : "Unknown";
                }
                if (selectedCount == 0 && currentMotionIndex >= 0) {
                    for (const auto& item : motionItemsWithCategories) {
                        if (!item.isCategory && !item.isSeparator && item.motionIndex == currentMotionIndex) { currentLabel = item.label; break; }
                    }
                }
                bool selectionChanged = false; int newMotionIndex = currentMotionIndex; bool newMacroSelected = false;
                if (ImGui::BeginCombo("##Action", currentLabel)) {
                    for (const auto& item : motionItemsWithCategories) {
                        if (item.isCategory) {
                            ImGui::TextColored(ImVec4(0.7f, 0.9f, 1.0f, 1.0f), "%s", item.label);
                        } else if (item.isSeparator) {
                            ImGui::Separator();
                        } else {
                            bool isSelected = (item.motionIndex == currentMotionIndex);
                            if (ImGui::Selectable(item.label, isSelected)) { newMotionIndex = item.motionIndex; selectionChanged = true; }
                            if (isSelected) ImGui::SetItemDefaultFocus();
                        }
                    }
                    ImGui::Separator();
                    bool isMacroSelected = (*triggers[i].macroSlot > 0);
                    if (ImGui::Selectable("Macro", isMacroSelected)) { newMacroSelected = true; selectionChanged = true; }
                    if (isMacroSelected) ImGui::SetItemDefaultFocus();
                    ImGui::EndCombo();
                }
                if (selectionChanged) {
                    if (newMacroSelected) {
                        int slots = MacroController::GetSlotCount(); if (*triggers[i].macroSlot == 0 && slots > 0) { *triggers[i].macroSlot = 1; }
                    } else {
                        *triggers[i].macroSlot = 0;
                        if (newMotionIndex <= 2) {
                            int currentButtonIdx = 0;
                            if (IsNormalAttackAction(*triggers[i].action)) {
                                switch (*triggers[i].action) { case ACTION_5A: case ACTION_2A: case ACTION_JA: currentButtonIdx = 0; break; case ACTION_5B: case ACTION_2B: case ACTION_JB: currentButtonIdx = 1; break; case ACTION_5C: case ACTION_2C: case ACTION_JC: currentButtonIdx = 2; break; default: currentButtonIdx = 0; break; }
                            } else { currentButtonIdx = *triggers[i].strength; }
                            *triggers[i].action = MapPostureAndButtonToAction(newMotionIndex, currentButtonIdx);
                        } else {
                            *triggers[i].action = MapMotionIndexToAction(newMotionIndex);
                        }
                    }
                }

                // (Multi/Rows controls moved to the 'More' column)

                // Column: Option (button/macro slot/dash follow-up)
                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(140);
                int buttonIdx = 0; int postureIdx = GetPostureIndexForAction(*triggers[i].action); bool macroSelected = (*triggers[i].macroSlot > 0);
                if (macroSelected) {
                    int slots = MacroController::GetSlotCount(); int zeroBased = (*triggers[i].macroSlot > 0) ? (*triggers[i].macroSlot - 1) : 0;
                    std::vector<std::string> labels; labels.reserve((size_t)slots);
                    for (int s = 1; s <= slots; ++s) labels.emplace_back(std::string("Slot ") + std::to_string(s));
                    std::vector<const char*> citems; citems.reserve(labels.size()); for (auto &s : labels) citems.push_back(s.c_str());
                    if (slots <= 0) { ImGui::BeginDisabled(); int dummy = 0; ImGui::Combo("##MacroSlot", &dummy, (const char* const*)nullptr, 0); ImGui::EndDisabled(); }
                    else { if (ImGui::Combo("##MacroSlot", &zeroBased, citems.data(), (int)citems.size())) { *triggers[i].macroSlot = zeroBased + 1; } }
                } else if (*triggers[i].action == ACTION_JUMP) {
                    const char* dirItems[] = { "Neutral", "Forward", "Backwards" }; int dir = *triggers[i].strength;
                    if (ImGui::Combo("##JumpDir", &dir, dirItems, IM_ARRAYSIZE(dirItems))) { *triggers[i].strength = (dir < 0 ? 0 : (dir > 2 ? 2 : dir)); }
                } else if (*triggers[i].action == ACTION_BACKDASH) {
                    ImGui::TextDisabled("(none)");
                } else if (*triggers[i].action == ACTION_FORWARD_DASH) {
                    int fdf = forwardDashFollowup.load(); const char* fdItems[] = { "No Follow-up", "5A", "5B", "5C", "2A", "2B", "2C" };
                    if (ImGui::Combo("##FDFollow", &fdf, fdItems, IM_ARRAYSIZE(fdItems))) { if (fdf < 0) fdf = 0; if (fdf > 6) fdf = 6; forwardDashFollowup.store(fdf); }
                    ImGui::SameLine(); bool dashMode = forwardDashFollowupDashMode.load(); if (ImGui::Checkbox("DashAtk", &dashMode)) { forwardDashFollowupDashMode.store(dashMode); }
                } else if (postureIdx >= 0) {
                    switch (*triggers[i].action) { case ACTION_5A: case ACTION_2A: case ACTION_JA: buttonIdx = 0; break; case ACTION_5B: case ACTION_2B: case ACTION_JB: buttonIdx = 1; break; case ACTION_5C: case ACTION_2C: case ACTION_JC: buttonIdx = 2; break; case ACTION_5D: case ACTION_2D: case ACTION_JD: buttonIdx = 3; break; default: buttonIdx = 0; break; }
                    if (ImGui::Combo("##Btn", &buttonIdx, buttonItems, IM_ARRAYSIZE(buttonItems))) { *triggers[i].action = MapPostureAndButtonToAction(postureIdx, buttonIdx); }
                } else if (IsSpecialMoveAction(*triggers[i].action)) {
                    buttonIdx = *triggers[i].strength; if (ImGui::Combo("##Str", &buttonIdx, buttonItems, IM_ARRAYSIZE(buttonItems))) { *triggers[i].strength = (buttonIdx > 3) ? 3 : buttonIdx; }
                } else if (GetMotionIndexForAction(*triggers[i].action) == 22 || GetMotionIndexForAction(*triggers[i].action) == 23) {
                    int groupIndex = GetMotionIndexForAction(*triggers[i].action);
                    switch (*triggers[i].action) { case ACTION_6A: case ACTION_4A: buttonIdx = 0; break; case ACTION_6B: case ACTION_4B: buttonIdx = 1; break; case ACTION_6C: case ACTION_4C: buttonIdx = 2; break; case ACTION_6D: case ACTION_4D: buttonIdx = 3; break; default: buttonIdx = 0; break; }
                    if (ImGui::Combo("##FwdBackBtn", &buttonIdx, buttonItems, IM_ARRAYSIZE(buttonItems))) {
                        if (groupIndex == 22) { *triggers[i].action = (buttonIdx==0)?ACTION_6A:(buttonIdx==1)?ACTION_6B:(buttonIdx==2)?ACTION_6C:ACTION_6D; }
                        else { *triggers[i].action = (buttonIdx==0)?ACTION_4A:(buttonIdx==1)?ACTION_4B:(buttonIdx==2)?ACTION_4C:ACTION_4D; }
                    }
                } else {
                    buttonIdx = *triggers[i].strength; if (*triggers[i].action != ACTION_BLOCK) { if (ImGui::Combo("##OtherBtn", &buttonIdx, buttonItems, IM_ARRAYSIZE(buttonItems))) { *triggers[i].strength = (buttonIdx > 3) ? 3 : buttonIdx; } }
                }

                // Column: Delay
                ImGui::TableNextColumn(); ImGui::SetNextItemWidth(70);
                int delayValue = *triggers[i].delay; if (ImGui::InputInt("##Delay", &delayValue, 1, 5)) { *triggers[i].delay = (std::max)(0, delayValue); }

                // Column: More (Row controls)
                ImGui::TableNextColumn();
                {
                    // '+' quick-add for rows
                    if (ImGui::SmallButton("+")) {
                        int* optCount = nullptr; TriggerOption* opts = nullptr; const int maxOpts = MAX_TRIGGER_OPTIONS;
                        if (i == 0) { optCount = &guiState.localData.afterBlockOptionCount; opts = guiState.localData.afterBlockOptions; }
                        else if (i == 1) { optCount = &guiState.localData.onWakeupOptionCount; opts = guiState.localData.onWakeupOptions; }
                        else if (i == 2) { optCount = &guiState.localData.afterHitstunOptionCount; opts = guiState.localData.afterHitstunOptions; }
                        else if (i == 3) { optCount = &guiState.localData.afterAirtechOptionCount; opts = guiState.localData.afterAirtechOptions; }
                        else if (i == 4) { optCount = &guiState.localData.onRGOptionCount; opts = guiState.localData.onRGOptions; }
                        if (optCount && opts && *optCount < maxOpts) {
                            TriggerOption def{ true, ACTION_5A, 0, 0, (int)BASE_ATTACK_5A, 0 };
                            if (i == 3) def.action = ACTION_JA; // Airtech default JA
                            opts[*optCount] = def; (*optCount)++;
                        }
                    }
                }

                // Inline rows: render per-trigger option entries under the main row
                int* optCount = nullptr; TriggerOption* opts = nullptr; const int maxOpts = MAX_TRIGGER_OPTIONS;
                if (i == 0) { optCount = &guiState.localData.afterBlockOptionCount; opts = guiState.localData.afterBlockOptions; }
                else if (i == 1) { optCount = &guiState.localData.onWakeupOptionCount; opts = guiState.localData.onWakeupOptions; }
                else if (i == 2) { optCount = &guiState.localData.afterHitstunOptionCount; opts = guiState.localData.afterHitstunOptions; }
                else if (i == 3) { optCount = &guiState.localData.afterAirtechOptionCount; opts = guiState.localData.afterAirtechOptions; }
                else if (i == 4) { optCount = &guiState.localData.onRGOptionCount; opts = guiState.localData.onRGOptions; }

                for (int r = 0; optCount && opts && r < *optCount; ++r) {
                    ImGui::TableNextRow();
                    // On column: empty (no checkbox)
                    ImGui::TableNextColumn(); ImGui::TextUnformatted("");
                    // Trigger name column: show numbered variant, e.g. "After Block (2)"
                    ImGui::TableNextColumn();
                    {
                        ImGui::AlignTextToFramePadding();
                        char label[128];
                        snprintf(label, sizeof(label), "%s (%d)", triggers[i].name, r + 2);
                        ImGui::TextUnformatted(label);
                    }
                    // Action column: action combo only (no enable checkbox)
                    ImGui::TableNextColumn();
                    ImGui::PushID(r);
                    int rowMotionIndex = (opts[r].macroSlot > 0) ? -2 : GetMotionIndexForAction(opts[r].action);
                    const char* rowLabel = nullptr;
                    if (rowMotionIndex >= 0) {
                        for (const auto& it : motionItemsWithCategories) {
                            if (!it.isCategory && !it.isSeparator && it.motionIndex == rowMotionIndex) { rowLabel = it.label; break; }
                        }
                    } else { rowLabel = (opts[r].macroSlot > 0) ? "Macro" : "Unknown"; }
                    ImGui::SetNextItemWidth(actionComboWidth);
                    bool selChanged = false; int newIdx = rowMotionIndex; bool macroPicked = false;
                    if (ImGui::BeginCombo("##rowAction", rowLabel)) {
                        for (const auto& item : motionItemsWithCategories) {
                            if (item.isCategory) ImGui::TextColored(ImVec4(0.7f,0.9f,1.0f,1.0f), "%s", item.label);
                            else if (item.isSeparator) ImGui::Separator();
                            else {
                                bool isSel = (item.motionIndex == rowMotionIndex);
                                if (ImGui::Selectable(item.label, isSel)) { newIdx = item.motionIndex; selChanged = true; }
                                if (isSel) ImGui::SetItemDefaultFocus();
                            }
                        }
                        ImGui::Separator();
                        bool isMacro = (opts[r].macroSlot > 0);
                        if (ImGui::Selectable("Macro", isMacro)) { macroPicked = true; selChanged = true; }
                        if (isMacro) ImGui::SetItemDefaultFocus();
                        ImGui::EndCombo();
                    }
                    if (selChanged) {
                        if (macroPicked) {
                            int slots = MacroController::GetSlotCount(); if (opts[r].macroSlot == 0 && slots > 0) opts[r].macroSlot = 1;
                        } else {
                            opts[r].macroSlot = 0;
                            if (newIdx <= 2) {
                                int btnIdx = opts[r].strength; opts[r].action = MapPostureAndButtonToAction(newIdx, btnIdx);
                            } else { opts[r].action = MapMotionIndexToAction(newIdx); }
                        }
                    }
                    // Button column
                    ImGui::TableNextColumn();
                    ImGui::SetNextItemWidth(140);
                    if (opts[r].macroSlot > 0) {
                        int slots = MacroController::GetSlotCount(); int zeroB = (opts[r].macroSlot>0)?(opts[r].macroSlot-1):0;
                        std::vector<std::string> labels; labels.reserve((size_t)slots);
                        for (int s=1;s<=slots;++s) labels.emplace_back(std::string("Slot ")+std::to_string(s));
                        std::vector<const char*> citems; citems.reserve(labels.size()); for (auto &s : labels) citems.push_back(s.c_str());
                        if (slots <= 0) { ImGui::BeginDisabled(); int dummy=0; ImGui::Combo("##rowMac", &dummy, (const char* const*)nullptr, 0); ImGui::EndDisabled(); }
                        else { if (ImGui::Combo("##rowMac", &zeroB, citems.data(), (int)citems.size())) { opts[r].macroSlot = zeroB + 1; } }
                    } else if (opts[r].action == ACTION_JUMP) {
                        const char* dirItems[] = { "Neutral", "Forward", "Backwards" }; int dir = opts[r].strength;
                        if (ImGui::Combo("##rowJump", &dir, dirItems, IM_ARRAYSIZE(dirItems))) { opts[r].strength = (dir<0?0:(dir>2?2:dir)); }
                    } else if (IsNormalAttackAction(opts[r].action)) {
                        int postIdx = GetPostureIndexForAction(opts[r].action);
                        int b = 0; switch (opts[r].action) { case ACTION_5A: case ACTION_2A: case ACTION_JA: case ACTION_6A: case ACTION_4A: b=0; break; case ACTION_5B: case ACTION_2B: case ACTION_JB: case ACTION_6B: case ACTION_4B: b=1; break; case ACTION_5C: case ACTION_2C: case ACTION_JC: case ACTION_6C: case ACTION_4C: b=2; break; case ACTION_5D: case ACTION_2D: case ACTION_JD: case ACTION_6D: case ACTION_4D: b=3; break; default: b=0; break; }
                        if (ImGui::Combo("##rowBtn", &b, buttonItems, IM_ARRAYSIZE(buttonItems))) {
                            if (postIdx == 0) opts[r].action = (b==0?ACTION_5A:(b==1?ACTION_5B:(b==2?ACTION_5C:ACTION_5D)));
                            else if (postIdx == 1) opts[r].action = (b==0?ACTION_2A:(b==1?ACTION_2B:(b==2?ACTION_2C:ACTION_2D)));
                            else if (postIdx == 2) opts[r].action = (b==0?ACTION_JA:(b==1?ACTION_JB:(b==2?ACTION_JC:ACTION_JD)));
                            else {
                                int groupIndex = GetMotionIndexForAction(opts[r].action);
                                if (groupIndex == 22) opts[r].action = (b==0?ACTION_6A:(b==1?ACTION_6B:(b==2?ACTION_6C:ACTION_6D)));
                                else if (groupIndex == 23) opts[r].action = (b==0?ACTION_4A:(b==1?ACTION_4B:(b==2?ACTION_4C:ACTION_4D)));
                            }
                            opts[r].strength = b;
                        }
                    } else if (IsSpecialMoveAction(opts[r].action)) {
                        int b = opts[r].strength; if (ImGui::Combo("##rowStr", &b, buttonItems, IM_ARRAYSIZE(buttonItems))) { opts[r].strength = (b>3)?3:b; }
                    } else { ImGui::TextDisabled("(none)"); }

                    // Delay column
                    ImGui::TableNextColumn(); ImGui::SetNextItemWidth(70);
                    int d = opts[r].delay; if (ImGui::InputInt("##rowDelay", &d, 1, 5)) { opts[r].delay = (std::max)(0, d); }

                    // More column: remove
                    ImGui::TableNextColumn();
                    if (ImGui::SmallButton("X")) {
                        for (int k=r+1; k<*optCount; ++k) opts[k-1] = opts[k];
                        (*optCount)--; ImGui::PopID();
                        // Skip rendering of the rest since data shifted
                        break;
                    }
                    ImGui::PopID();
                }

                ImGui::PopID();
            }
            ImGui::EndTable();
        }

    // end default spacing scope

                ImGui::EndTabItem();
            }

            // Macros sub-tab (moved from main tab bar)
            ImGuiTabItemFlags _setMacros = (rq2 == 1) ? ImGuiTabItemFlags_SetSelected : 0;
            if (ImGui::BeginTabItem("Macros", nullptr, _setMacros)) {
                bool enteringMacros = !s_macrosActivePrev;
                macrosActiveThisFrame = true;
                guiState.autoActionSubTab = 1;
                const auto& cfg = Config::GetSettings();
                ImGui::SeparatorText("Macro Controller");
                ImGui::Text("State: %s", MacroController::GetStatusLine().c_str());
                ImGui::Text("Current Slot: %d / %d", MacroController::GetCurrentSlot(), MacroController::GetSlotCount());
                bool empty = MacroController::IsSlotEmpty(MacroController::GetCurrentSlot());
                ImGui::Text("Slot Empty: %s", empty ? "Yes" : "No");
                // Debug stats for validation
                /*{
                    auto stats = MacroController::GetSlotStats(MacroController::GetCurrentSlot());
                    ImGui::SeparatorText("Slot Stats");
                    ImGui::BulletText("Spans: %d", stats.spanCount);
                    ImGui::BulletText("Total Ticks: %d (~%.2fs)", stats.totalTicks, stats.totalTicks / 64.0f);
                    ImGui::BulletText("Buffer Entries: %d", stats.bufEntries);
                    ImGui::BulletText("Buf Idx Start: %u", (unsigned)stats.bufStartIdx);
                    ImGui::BulletText("Buf Idx End: %u", (unsigned)stats.bufEndIdx);
                    ImGui::BulletText("Has Data: %s", stats.hasData ? "Yes" : "No");
                }*/
                if (ImGui::Button("Toggle Record")) { MacroController::ToggleRecord(); }
                ImGui::SameLine();
                if (ImGui::Button("Play")) { MacroController::Play(); }
                ImGui::SameLine();
                if (ImGui::Button("Stop")) { MacroController::Stop(); }
                if (ImGui::Button("Prev Slot")) { MacroController::PrevSlot(); }
                ImGui::SameLine();
                if (ImGui::Button("Next Slot")) { MacroController::NextSlot(); }
                ImGui::Spacing();
                // Serialized editor + history
                static bool s_includeBuffers = true;
                static int s_lastSlot = -1;
                static bool s_lastInclude = true;
                static std::string s_macroText;
                static std::vector<char> s_textBuf; // large buffer for editing
                static std::string s_applyError;
                static std::vector<std::string> s_undoStack;
                static std::vector<std::string> s_redoStack;
                static std::string s_prevText; // last committed text for change detection
                static bool s_forceReload = false;
                static bool s_editMode = false; // edit/display toggle for macro editor
                auto ensureBufferFromText = [&](const std::string& txt){
                    const size_t minCap = 8192; // 8KB editing space
                    size_t cap = (txt.size() + 1024 > minCap) ? (txt.size() + 1024) : minCap;
                    s_textBuf.assign(cap, '\0');
                    if (!txt.empty()) memcpy(s_textBuf.data(), txt.data(), txt.size());
                };
                int curSlot = MacroController::GetCurrentSlot();
                if (enteringMacros) s_forceReload = true;
                // Reload text when slot changes or includeBuffers toggles
                if (s_forceReload || s_lastSlot != curSlot || s_lastInclude != s_includeBuffers) {
                    s_macroText = MacroController::SerializeSlot(curSlot, s_includeBuffers);
                    ensureBufferFromText(s_macroText);
                    s_undoStack.clear(); s_redoStack.clear(); s_prevText = s_macroText; s_applyError.clear();
                    s_lastSlot = curSlot; s_lastInclude = s_includeBuffers;
                    s_forceReload = false;
                }
                ImGui::SeparatorText("Serialized Macro");
                ImGui::Checkbox("Include Buffers", &s_includeBuffers);
                ImGui::SameLine();
                if (ImGui::SmallButton("Reload from Slot")) {
                    s_macroText = MacroController::SerializeSlot(curSlot, s_includeBuffers);
                    ensureBufferFromText(s_macroText);
                    s_undoStack.clear(); s_redoStack.clear(); s_prevText = s_macroText; s_applyError.clear();
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("Prev Slot##serialized")) {
                    MacroController::PrevSlot();
                    // Force refresh and leave edit mode so the new slot shows immediately
                    s_forceReload = true;
                    s_editMode = false;
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("Next Slot##serialized")) {
                    MacroController::NextSlot();
                    // Force refresh and leave edit mode so the new slot shows immediately
                    s_forceReload = true;
                    s_editMode = false;
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("Apply to Slot")) {
                    std::string err;
                    // Get text from buffer and clean up newlines/spaces
                    std::string textToApply = s_textBuf.data();
                    
                    // Remove newlines and collapse multiple spaces into single spaces
                    std::string cleaned;
                    bool prevWasSpace = false;
                    for (char c : textToApply) {
                        if (c == '\n' || c == '\r') {
                            // Convert newlines to spaces
                            if (!prevWasSpace && !cleaned.empty()) {
                                cleaned += ' ';
                                prevWasSpace = true;
                            }
                        } else if (c == ' ') {
                            if (!prevWasSpace) {
                                cleaned += ' ';
                                prevWasSpace = true;
                            }
                        } else {
                            cleaned += c;
                            prevWasSpace = false;
                        }
                    }
                    
                    if (!MacroController::DeserializeSlot(curSlot, cleaned, err)) {
                        s_applyError = err;
                    } else {
                        s_applyError.clear();
                        // Refresh serialized text from parsed slot for canonical formatting
                        s_macroText = MacroController::SerializeSlot(curSlot, s_includeBuffers);
                        ensureBufferFromText(s_macroText);
                        s_undoStack.clear(); s_redoStack.clear(); s_prevText = s_macroText;
                        DirectDrawHook::AddMessage("Applied macro to slot", "MACRO", RGB(180,255,180), 1000, 0, 120);
                    }
                }
                // Second row of controls
                ImGui::Dummy(ImVec2(1, 2));
                if (ImGui::SmallButton("Clear Slot")) {
                    std::string err;
                    if (MacroController::DeserializeSlot(curSlot, std::string(), err)) {
                        s_macroText = MacroController::SerializeSlot(curSlot, s_includeBuffers);
                        ensureBufferFromText(s_macroText);
                        s_undoStack.clear(); s_redoStack.clear(); s_prevText = s_macroText; s_applyError.clear();
                        DirectDrawHook::AddMessage("Cleared macro slot", "MACRO", RGB(255,220,120), 900, 0, 120);
                    } else {
                        s_applyError = err;
                    }
                }
                ImGui::SameLine();
                bool canUndo = !s_undoStack.empty();
                bool canRedo = !s_redoStack.empty();
                if (!canUndo) ImGui::BeginDisabled();
                if (ImGui::SmallButton("Undo")) {
                    if (!s_undoStack.empty()) {
                        s_redoStack.push_back(s_macroText);
                        s_macroText = s_undoStack.back(); s_undoStack.pop_back();
                        ensureBufferFromText(s_macroText);
                        s_prevText = s_macroText;
                    }
                }
                if (!canUndo) ImGui::EndDisabled();
                ImGui::SameLine();
                if (!canRedo) ImGui::BeginDisabled();
                if (ImGui::SmallButton("Redo")) {
                    if (!s_redoStack.empty()) {
                        s_undoStack.push_back(s_macroText);
                        s_macroText = s_redoStack.back(); s_redoStack.pop_back();
                        ensureBufferFromText(s_macroText);
                        s_prevText = s_macroText;
                    }
                }
                if (!canRedo) ImGui::EndDisabled();
                ImGui::SameLine();
                if (ImGui::SmallButton("Copy")) {
                    ImGui::SetClipboardText(s_textBuf.data());
                    DirectDrawHook::AddMessage("Copied macro text", "MACRO", RGB(180,255,220), 700, 0, 120);
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("Paste")) {
                    const char* clip = ImGui::GetClipboardText();
                    if (clip && *clip) {
                        s_undoStack.push_back(s_macroText);
                        s_redoStack.clear();
                        s_macroText = std::string(clip);
                        ensureBufferFromText(s_macroText);
                        s_prevText = s_macroText;
                        s_applyError.clear();
                        // Enter edit mode so user can see/adjust pasted content immediately
                        s_editMode = true;
                    }
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("Insert Sample")) {
                    // Sample: 5A, wait 3f, 5B, wait 3f, 5C, then a 623B written with per-tick buffer groups
                    // 623 example provided by user: 6 {3: 6 6 6} 2 {3: 2 2 2} 3 {3: 3 3 3} 5B {3: 5B 5 5}
                    std::string sample =
                        "EFZMACRO 1 "
                        "5A 5x3 5B 5x3 5C "
                        "6 {3: 6 6 6} 2 {3: 2 2 2} 3 {3: 3 3 3} 5B {3: 5B 5 5}";
                    // Push current into undo and clear redo
                    s_undoStack.push_back(s_macroText);
                    s_redoStack.clear();
                    s_macroText = sample;
                    ensureBufferFromText(s_macroText);
                    s_prevText = s_macroText;
                    s_applyError.clear();
                }
                if (!s_applyError.empty()) {
                    ImGui::TextColored(ImVec4(1.0f,0.4f,0.4f,1.0f), "Error: %s", s_applyError.c_str());
                }
                ImVec2 availEd = ImGui::GetContentRegionAvail();
                // Reserve space for hint text and hotkeys section at bottom (~100px), ensure minimum 200px editor height
                float editorH = (std::max)(200.0f, availEd.y - 100.0f);
                float editorW = availEd.x;
                
                // Custom word-wrapped editor with manual line breaking
                ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.10f, 0.10f, 0.10f, 0.94f));
                ImGui::BeginChild("##macro_editor_wrapper", ImVec2(editorW, editorH), true);
                
                // Calculate wrapped lines for display
                auto wrapText = [](const std::string& text, float wrapWidth, ImFont* font, float fontSize) -> std::vector<std::string> {
                    std::vector<std::string> lines;
                    if (text.empty()) {
                        lines.push_back("");
                        return lines;
                    }
                    
                    std::string currentLine;
                    std::string token;
                    bool inBraces = false;
                    int braceDepth = 0;
                    
                    for (size_t i = 0; i < text.size(); ++i) {
                        char c = text[i];
                        
                        if (c == '\n') {
                            // Explicit newline
                            currentLine += token;
                            lines.push_back(currentLine);
                            currentLine.clear();
                            token.clear();
                            inBraces = false;
                            braceDepth = 0;
                        } else if (c == '{') {
                            // Start of buffer group - keep with current token
                            token += c;
                            inBraces = true;
                            braceDepth++;
                        } else if (c == '}') {
                            // End of buffer group
                            token += c;
                            braceDepth--;
                            if (braceDepth <= 0) {
                                inBraces = false;
                                braceDepth = 0;
                            }
                        } else if (c == ' ' && !inBraces) {
                            // Space outside braces - this is a break point
                            // But first, check if adding this token would exceed width
                            std::string testLine = currentLine + token + " ";
                            ImVec2 size = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, testLine.c_str());
                            
                            if (size.x > wrapWidth && !currentLine.empty()) {
                                // Line too long, break before this token
                                lines.push_back(currentLine);
                                currentLine = token + " ";
                            } else {
                                currentLine += token + " ";
                            }
                            token.clear();
                        } else {
                            // Regular character or space inside braces
                            token += c;
                        }
                    }
                    
                    // Add remaining text
                    currentLine += token;
                    if (!currentLine.empty()) {
                        lines.push_back(currentLine);
                    }
                    
                    return lines;
                };
                
                ImFont* font = ImGui::GetFont();
                float fontSize = ImGui::GetFontSize();
                float contentWidth = ImGui::GetContentRegionAvail().x - 10.0f;
                
                if (!s_editMode) {
                    // Display mode with proper word wrapping
                    auto wrappedLines = wrapText(s_macroText, contentWidth, font, fontSize);
                    
                    for (const auto& line : wrappedLines) {
                        ImGui::TextUnformatted(line.c_str());
                    }
                    
                    // Click to enter edit mode
                    if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(0)) {
                        s_editMode = true;
                        // When entering edit mode, insert newlines for visual wrapping
                        auto wrappedLines = wrapText(s_macroText, contentWidth, font, fontSize);
                        std::string wrappedText;
                        for (size_t i = 0; i < wrappedLines.size(); ++i) {
                            wrappedText += wrappedLines[i];
                            if (i < wrappedLines.size() - 1) {
                                // Add newline if the line doesn't already end with one
                                if (!wrappedLines[i].empty() && wrappedLines[i].back() != '\n') {
                                    wrappedText += '\n';
                                }
                            }
                        }
                        s_macroText = wrappedText;
                        strncpy_s(s_textBuf.data(), s_textBuf.size(), s_macroText.c_str(), _TRUNCATE);
                    }
                } else {
                    // Edit mode - use InputTextMultiline
                    ImGui::PushItemWidth(-1);
                    ImGuiInputTextFlags editorFlags = ImGuiInputTextFlags_AllowTabInput;
                    
                    // Auto-focus on first frame of edit mode
                    static bool s_needsFocus = false;
                    if (s_needsFocus) {
                        ImGui::SetKeyboardFocusHere();
                        s_needsFocus = false;
                    }
                    
                    if (ImGui::InputTextMultiline("##macro_edit", s_textBuf.data(), s_textBuf.size(), 
                        ImVec2(-1, -1), editorFlags)) {
                        std::string newText = s_textBuf.data();
                        if (newText != s_macroText) {
                            s_undoStack.push_back(s_macroText);
                            s_redoStack.clear();
                            s_macroText = newText;
                            s_prevText = s_macroText;
                        }
                    }
                    
                    ImGui::PopItemWidth();
                    
                    // Exit edit mode on Escape or when losing focus
                    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                        s_editMode = false;
                        // When exiting edit mode, remove newlines but preserve single spaces
                        std::string unwrapped;
                        bool prevWasSpace = false;
                        for (char c : s_macroText) {
                            if (c == '\n') {
                                if (!prevWasSpace && !unwrapped.empty()) {
                                    unwrapped += ' ';
                                    prevWasSpace = true;
                                }
                            } else if (c == ' ') {
                                if (!prevWasSpace) {
                                    unwrapped += ' ';
                                    prevWasSpace = true;
                                }
                            } else {
                                unwrapped += c;
                                prevWasSpace = false;
                            }
                        }
                        s_macroText = unwrapped;
                        strncpy_s(s_textBuf.data(), s_textBuf.size(), s_macroText.c_str(), _TRUNCATE);
                    }
                    if (!ImGui::IsItemActive() && !ImGui::IsItemFocused() && ImGui::IsMouseClicked(0) && !ImGui::IsItemHovered()) {
                        s_editMode = false;
                        // When exiting edit mode, remove newlines but preserve single spaces
                        std::string unwrapped;
                        bool prevWasSpace = false;
                        for (char c : s_macroText) {
                            if (c == '\n') {
                                if (!prevWasSpace && !unwrapped.empty()) {
                                    unwrapped += ' ';
                                    prevWasSpace = true;
                                }
                            } else if (c == ' ') {
                                if (!prevWasSpace) {
                                    unwrapped += ' ';
                                    prevWasSpace = true;
                                }
                            } else {
                                unwrapped += c;
                                prevWasSpace = false;
                            }
                        }
                        s_macroText = unwrapped;
                        strncpy_s(s_textBuf.data(), s_textBuf.size(), s_macroText.c_str(), _TRUNCATE);
                    }
                }
                
                ImGui::EndChild();
                ImGui::PopStyleColor();
                
                if (!s_editMode) {
                    ImGui::TextDisabled("(Click text to edit)");
                }
                {
                    ImVec4 disabled = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
                    ImGui::PushStyleColor(ImGuiCol_Text, disabled);
                    ImGui::TextWrapped("Hint: Use numpad directions with A/B/C/D and repeats, e.g. 5Ax50, 6, or 2 3 6C. Optional per-tick buffers: {3: 5 0x9A 6A}");
                    ImGui::PopStyleColor();
                }
                ImGui::SeparatorText("Hotkeys");
                ImGui::BulletText("Record: %s", GetKeyName(cfg.macroRecordKey).c_str());
                ImGui::BulletText("Play: %s", GetKeyName(cfg.macroPlayKey).c_str());
                ImGui::BulletText("Next Slot: %s", GetKeyName(cfg.macroSlotKey).c_str());
                ImGui::EndTabItem();
            }

            ImGui::EndTabBar();
            // Remember whether Macros tab was active this frame
            s_macrosActivePrev = macrosActiveThisFrame;
        }
    }

    // Small helper: wrapped bullet text that respects current wrap width
    static void BulletTextWrapped(const char* fmt, ...) {
        char buf[1024];
        va_list args;
        va_start(args, fmt);
        vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        ImGui::Bullet();
        ImGui::SameLine();
        ImGui::TextWrapped("%s", buf);
    }

    // Clickable link helper (blue text, hand cursor, opens URL)
    static void Link(const char* label, const char* url) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.35f, 0.65f, 1.0f, 1.0f));
        ImGui::TextUnformatted(label);
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", url);
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        }
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
            ShellExecuteA(nullptr, "open", url, nullptr, nullptr, SW_SHOWNORMAL);
        }
    }

    // Map character IDs to canonical EFZ wiki pages (explicit list provided by user)
    static const char* GetCharacterWikiPathByID(int charId) {
        switch (charId) {
            case CHAR_ID_AKANE:    return "Akane_Satomura";
            case CHAR_ID_AKIKO:    return "Akiko_Minase";
            case CHAR_ID_AYU:      return "Ayu_Tsukimiya";
            case CHAR_ID_EXNANASE: return "Doppel_Nanase";
            case CHAR_ID_IKUMI:    return "Ikumi_Amasawa";
            case CHAR_ID_KANNA:    return "Kanna";
            case CHAR_ID_KANO:     return "Kano_Kirishima";
            case CHAR_ID_KAORI:    return "Kaori_Misaka";
            case CHAR_ID_MAI:      return "Mai_Kawasumi";
            case CHAR_ID_MAKOTO:   return "Makoto_Sawatari";
            case CHAR_ID_MAYU:     return "Mayu_Shiina";
            case CHAR_ID_MINAGI:   return "Minagi_Tohno";
            case CHAR_ID_MIO:      return "Mio_Kouzuki";
            case CHAR_ID_MISAKI:   return "Misaki_Kawana";
            case CHAR_ID_MISHIO:   return "Mishio_Amano";
            case CHAR_ID_MISUZU:   return "Misuzu_Kamio";
            case CHAR_ID_MIZUKA:   return "Mizuka_Nagamori";   // UNKNOWN(Boss)
            case CHAR_ID_NAGAMORI: return "Mizuka_Nagamori";   // 
            case CHAR_ID_NANASE:   return "Rumi_Nanase";       // Rumi
            case CHAR_ID_SAYURI:   return "Sayuri_Kurata";
            case CHAR_ID_SHIORI:   return "Shiori_Misaka";
            case CHAR_ID_NAYUKI:   return "Nayuki_Minase_(asleep)"; // Sleepy
            case CHAR_ID_NAYUKIB:  return "Nayuki_Minase_(awake)";  // Awake
            case CHAR_ID_MIZUKAB:  return "UNKNOWN";                // Unknown
            default: return nullptr;
        }
    }

    // Help Tab implementation (refactored into sub-tabs)
    void RenderHelpTab() {
        // Wrap help content in a scrollable child so keyboard/controller nav can scroll it
        ImGuiWindowFlags helpFlags = ImGuiWindowFlags_NoSavedSettings;
        ImVec2 avail = ImGui::GetContentRegionAvail();
        if (ImGui::BeginChild("##HelpScroll", ImVec2(avail.x, avail.y), true, helpFlags)) {
            // Enable word-wrap against child width
            ImGui::PushTextWrapPos(0.0f);
            // Read current hotkey/controller settings once for display
            const auto& cfg = Config::GetSettings();
            // Ensure the child can be focused for keyboard/controller scrolling
            if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows)) {
                ImGui::SetItemDefaultFocus();
            }
            // Small hint so users know how to scroll without a mouse
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 0.8f, 0.85f));
            ImGui::TextDisabled("Hint: Use Up/Down/PageUp/PageDown or D-Pad to scroll this Help.");
            ImGui::PopStyleColor();
            // Short intro
            ImGui::TextWrapped("This Help covers the overlay features, hotkeys, and training tools. While the menu is open, practice hotkeys are gated and the game auto-pauses; it resumes on close.");
            ImGui::Dummy(ImVec2(1, 4));

            if (ImGui::BeginTabBar("##HelpTabs", ImGuiTabBarFlags_None)) {
                int rq3 = guiState.requestedHelpSubTab; guiState.requestedHelpSubTab = -1;
                // Getting Started
                ImGuiTabItemFlags _setHelp0 = (rq3 == 0) ? ImGuiTabItemFlags_SetSelected : 0;
                if (ImGui::BeginTabItem("Getting Started", nullptr, _setHelp0)) {
                    guiState.helpSubTab = 0;
                    if (ImGui::BeginTabBar("##HelpGettingStartedTabs", ImGuiTabBarFlags_None)) {
                        // Quick Start
                        if (ImGui::BeginTabItem("Quick Start")) {
                            ImGui::TextWrapped("Open the overlay, adjust options, then press Apply at the bottom. While the menu is open, practice hotkeys are gated and the game auto-pauses; it resumes on close.");
                            BulletTextWrapped("Open Help: %s", GetKeyName(cfg.helpKey).c_str());
                            BulletTextWrapped("Toggle Overlay: %s (Controller: %s)", GetKeyName(cfg.toggleImGuiKey).c_str(), Config::GetGamepadButtonName(cfg.gpToggleMenuButton).c_str());
                            BulletTextWrapped("Save Position: %s (Controller: %s)", GetKeyName(cfg.recordKey).c_str(), Config::GetGamepadButtonName(cfg.gpSavePositionButton).c_str());
                            BulletTextWrapped("Load Position: %s (Controller: %s)", GetKeyName(cfg.teleportKey).c_str(), Config::GetGamepadButtonName(cfg.gpTeleportButton).c_str());
                            BulletTextWrapped("Toggle Stats: %s", GetKeyName(cfg.toggleTitleKey).c_str());
                            ImGui::EndTabItem();
                        }
                        // Position Tools
                        if (ImGui::BeginTabItem("Position Tools")) {
                            ImGui::TextDisabled("Hold Load: Keyboard=%s, Controller=%s", GetKeyName(cfg.teleportKey).c_str(), Config::GetGamepadButtonName(cfg.gpTeleportButton).c_str());
                            BulletTextWrapped("Center Both: Load + Down (D-Pad Down + Load)");
                            BulletTextWrapped("Left Corner: Load + Left (D-Pad Left + Load)");
                            BulletTextWrapped("Right Corner: Load + Right (D-Pad Right + Load)");
                            BulletTextWrapped("Round Start: Load + Down + A (hold Down+A, then press Load)");
                            BulletTextWrapped("Swap Positions: Load + D (Controller: %s)", Config::GetGamepadButtonName(cfg.gpSwapPositionsButton).c_str());
                            ImGui::EndTabItem();
                        }
                        // Menu Tips
                        if (ImGui::BeginTabItem("Menu Tips")) {
                            BulletTextWrapped("Navigation: mouse, arrow keys, or D-Pad; Enter/Space/A to toggle/activate.");
                            BulletTextWrapped("Footer hotkeys: Apply=%s, Refresh=%s, Exit=%s.",
                                GetKeyName(cfg.uiAcceptKey).c_str(), GetKeyName(cfg.uiRefreshKey).c_str(), GetKeyName(cfg.uiExitKey).c_str());
                            BulletTextWrapped("UI sizing: tweak uiScale/uiFont in the config if text feels off.");
                            BulletTextWrapped("Open Help quickly with %s.", GetKeyName(cfg.helpKey).c_str());
                            ImGui::EndTabItem();
                        }
                        ImGui::EndTabBar();
                    }
                    ImGui::EndTabItem();
                }

                // Guide (consolidated info)
                ImGuiTabItemFlags _setHelp1 = (rq3 == 1) ? ImGuiTabItemFlags_SetSelected : 0;
                if (ImGui::BeginTabItem("Guide", nullptr, _setHelp1)) {
                    guiState.helpSubTab = 1;
                    if (ImGui::BeginTabBar("##HelpGuideTabs", ImGuiTabBarFlags_None)) {
                        // Basics
                        if (ImGui::BeginTabItem("Basics")) {
                            ImGui::TextWrapped("Core practice tools and dummy options you'll use most.");
                            ImGui::Dummy(ImVec2(1, 2));
                            ImGui::TextDisabled("Quick setup");
                            BulletTextWrapped("Switch Players: %s (Controller: %s)", GetKeyName(cfg.switchPlayersKey).c_str(), Config::GetGamepadButtonName(cfg.gpSwitchPlayersButton).c_str());
                            BulletTextWrapped("P2 Control: lets you play as P2; F6/F7 won't work while ON.");
                            BulletTextWrapped("Dummy Auto-Block: Off / Block All / Only Block First Hit / Block After First Hit.");
                            BulletTextWrapped("Adaptive Stance: auto-picks high vs air and overheads, low vs grounded; disables manual stance while ON.");

                            ImGui::Dummy(ImVec2(1, 4));
                            ImGui::TextDisabled("Block & RG Modes");
                            ImGui::TextWrapped("Control how the dummy blocks and uses Recoil Guard:");
                            BulletTextWrapped("Random Block: coin-flip to block; great for testing hit-confirms on gaps.");
                            BulletTextWrapped("Always RG: treats eligible blocks as Recoil Guard.");
                            BulletTextWrapped("Random RG: attempts RG at random.");
                            BulletTextWrapped("Counter RG: tries to RG back after you RG.");
                            ImGui::Dummy(ImVec2(1, 2));
                            ImGui::TextDisabled("Notes");
                            ImGui::TextWrapped("These modes can conflict. Turning one on can turn others off automatically.");

                            ImGui::Dummy(ImVec2(1, 4));
                            ImGui::TextDisabled("Training Tools");
                            ImGui::TextWrapped("Helpful automation for common drills:");
                            BulletTextWrapped("Auto-Airtech: Forward/Backward tech; 'Delay' adds frames before tech (great for testing late airtechs).");
                            BulletTextWrapped("Auto-Jump: neutral/forward/back jump when able.");
                            BulletTextWrapped("Final Memory (Global): 'Allow at any HP' removes HP checks. You can uncheck this to disable.");

                            ImGui::Dummy(ImVec2(1, 2));
                            ImGui::TextDisabled("Frame Advantage & Gaps");
                            ImGui::TextWrapped("The overlay shows Frame Advantage after an exchange and Gaps during strings:");
                            ImGui::Indent();
                            BulletTextWrapped("Frame Advantage: appears after both sides recover; stays for about %.1fs (Settings -> General).", Config::GetSettings().frameAdvantageDisplayDuration);
                            BulletTextWrapped("Gaps: briefly flash during strings when there's a hole.");
                            BulletTextWrapped("During Recoil Guard, FA1/FA2 labels show advantage for each part.");
                            ImGui::Unindent();

                            ImGui::EndTabItem();
                        }
                        // Recovery (Consolidated: per-player + Automatic Recovery info)
                        if (ImGui::BeginTabItem("Recovery")) {
                            ImGui::SeparatorText("Continuous Recovery (Per-Player)");
                            ImGui::TextWrapped("Restores HP/Meter/RF when a side returns to neutral. Configure per-side under Main -> Options -> Continuous Recovery. Disabled automatically when game's own HP/meter recovery is active (F4/F5).\n");
                            BulletTextWrapped("HP/Meter: Off, presets, or Custom.");
                            BulletTextWrapped("RF: presets or Custom. BIC (Blue IC) is under RF->Custom. Red presets changes IC back to Red.");
                            BulletTextWrapped("RF Freeze (optional): if enabled in config, freezes RF after Recovery sets it until you turn Recovery (RF) off so it won't increase by itself.");
                            //BulletTextWrapped("Defaults: Recovery is OFF per-player. Enforcement runs in matches; can be limited to neutral-only via config.");
                            ImGui::Dummy(ImVec2(1, 6));
                            ImGui::SeparatorText("Automatic Recovery (F5)");
                            ImGui::TextWrapped("Game-driven recovery modes toggled from Main -> Values: \n- Disabled: no automatic regeneration. \n- Full values: sets all HP/Meter values to max. \n- FM values (3332): sets HP to 3332 and all Meter values to max.");
                            BulletTextWrapped("Switching Automatic Recovery from Full/FM to Disabled in the GUI specifically sets both players to default match start values(unlike F5 button on it's 3rd press).");
                            BulletTextWrapped("While F5 or F4 is active, manual value edits are disallowed. X/Y positions can still be changed in the Values tab.");
                            BulletTextWrapped("Tip: If numbers look off, press F4/F5 to return to Normal mode, then re-Apply.");
                            ImGui::EndTabItem();
                        }
                        // Character Settings
                        if (ImGui::BeginTabItem("Character Settings")) {
                            ImGui::TextWrapped("Options appear only when that character is present. Examples:");
                            BulletTextWrapped("Ikumi: Infinite Blood / Genocide timer tweaks (Practice only).");
                            BulletTextWrapped("Misuzu: Feathers, Poison timer/level with optional freeze.");
                            BulletTextWrapped("Mishio: Element (None/Fire/Lightning/Awakened) and Awakened timer controls; Infinite toggles.");
                            BulletTextWrapped("Akiko: Bullet cycle lock, Clean Hit helper, Timeslow trigger.");
                            BulletTextWrapped("Mai: Ghost/Charge/Awakening timers, 'No CD', Ghost position override.");
                            BulletTextWrapped("Kano: Magic meter controls with optional value lock.");
                            BulletTextWrapped("Nayuki (Awake): Snowbunnies timer with infinite toggle.");
                            BulletTextWrapped("Nayuki (Asleep): Jam count.");
                            BulletTextWrapped("Mio: Stance (Short/Long) with lock.");
                            BulletTextWrapped("Doppel: Golden Doppel toggle.");
                            BulletTextWrapped("Nanase (Rumi): Shinai/Barehanded, Infinite Shinai, FM (Kimchi) timer controls.");
                            BulletTextWrapped("Minagi: Always-readied Michiru toggle and Michiru position override.");
                            ImGui::EndTabItem();
                        }
                        // Auto Actions
                        if (ImGui::BeginTabItem("Auto Actions")) {
                            ImGui::TextWrapped("Make the dummy act on key moments: On Wakeup, After Block/Hitstun/Airtech, or on Recoil Guard.");
                            ImGui::Dummy(ImVec2(1, 2));
                            ImGui::TextDisabled("Quick setup");
                            BulletTextWrapped("Enable it by checking the first checkbox in the menu(Enable Auto Action System) and check the desired triggers as well. You can also change which side it applies to, by default it's always set to P2");
                            BulletTextWrapped("You can enable the Randomize triggers option, which adds a coin-flip to make the triggers sometimes skip the activation.");
                            BulletTextWrapped("Pre-buffering of wake specials/dashes performs wake inputs slightly early. This might help with testing input crossups and some other things.");
                            ImGui::Dummy(ImVec2(1, 4));
                            ImGui::TextDisabled("Per trigger");
                            ImGui::TextWrapped("Pick an action (normals, forward/back normals, specials, supers, jump, dash/backdash, block, Final Memory, or a Macro slot), the button if needed, and an optional delay.");
                            ImGui::TextWrapped("You can add extra rows or turn on Use Pool to randomly pick from several actions(might need to resize the window if you can't see the + button).");
                            ImGui::Dummy(ImVec2(1, 4));
                            ImGui::TextDisabled("Notes");
                            ImGui::TextWrapped("This feature works by enabling P2 controls for a brief period to perform the actions you set up.");
                            BulletTextWrapped("P2 controls are only enabled for specials/supers/dashes(things which use input buffer). Regular attacks and jumps still retain AI controls (since they use direct input writes).");
                            ImGui::TextWrapped("By default on wake-up action tries to use the special move on the last frame of the wakeup(all characters are properly handled).");
                            BulletTextWrapped("It should also properly handle crossups as well.");
                            ImGui::TextWrapped("Actions are rate-limited to avoid spam; toggling the trigger clears it.");
                            ImGui::TextWrapped("'After Airtech' here is separate from Auto-Airtech; you need to enable auto-airtech for After Airtech trigger to work.");
                            ImGui::EndTabItem();
                        }
                        // Macros
                        if (ImGui::BeginTabItem("Macros")) {
                            ImGui::TextWrapped("Record, play, and edit inputs as macros. Slots cycle with a hotkey; playback flips directions for P2 automatically.");
                            ImGui::Dummy(ImVec2(1, 2));
                            ImGui::TextDisabled("Quick setup");
                            BulletTextWrapped("Record: %s enters Pre-recording (P1 controls drive P2); press again to start, then again to save.", GetKeyName(cfg.macroRecordKey).c_str());
                            BulletTextWrapped("Play: %s plays the current slot.", GetKeyName(cfg.macroPlayKey).c_str());
                            BulletTextWrapped("Slots: cycle with %s. Empty slots do nothing.", GetKeyName(cfg.macroSlotKey).c_str());
                            ImGui::Dummy(ImVec2(1, 4));
                            ImGui::TextDisabled("Tips");
                            ImGui::TextWrapped("Exit Pre-recording with Play (Keyboard: %s, Controller: %s). Frame-step tools work during playback.", GetKeyName(cfg.macroPlayKey).c_str(), Config::GetGamepadButtonName(cfg.gpMacroPlayButton).c_str());
                            ImGui::Dummy(ImVec2(1, 4));
                            ImGui::TextDisabled("Notation");
                            ImGui::TextWrapped("Write macros as plain text: a header plus tick tokens. Use numpad directions (1..9, 5=neutral) with A/B/C/D (e.g., 5A, 6B, 236C). 'N' is neutral. Repeat packs with xN. Optional per-tick buffers: {k: v1 v2 ...}. Whitespace is flexible; Apply normalizes. Write for P1-facing; P2 playback flips 4/6.");
                            ImGui::Dummy(ImVec2(1, 2));
                            ImGui::TextDisabled("Example");
                            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.8f, 0.95f, 1.0f, 1.0f));
                            ImGui::TextWrapped("EFZMACRO 1 5A 5x3 5B 5x3 5C 6 {3: 6 6 6} 2 {3: 2 2 2} 3 {3: 3 3 3} 5B {3: 5B 5 5}");
                            ImGui::PopStyleColor();
                            ImGui::Indent();
                            BulletTextWrapped("'x3' inserts neutral ticks between presses.");
                            BulletTextWrapped("The {3: ...} packs perform three writes within a tick.");
                            ImGui::Unindent();
                            ImGui::EndTabItem();
                        }

                        // Issues (merged Conflicts + Troubleshooting)
                        if (ImGui::BeginTabItem("Issues")) {
                            ImGui::SeparatorText("Conflicts");
                            ImGui::TextWrapped("Some features auto-disable others to avoid clashes:");
                            ImGui::Indent();
                            BulletTextWrapped("Random Block, Random RG, and Always RG are mutually exclusive; turning one on can turn others off.");
                            BulletTextWrapped("Counter RG won't work if Always RG is on.");
                            BulletTextWrapped("While the menu is open, practice hotkeys are gated and the game auto-pauses; there's a brief cooldown after closing.");
                            ImGui::Unindent();

                            ImGui::Dummy(ImVec2(1, 6));
                            ImGui::SeparatorText("Troubleshooting");
                            BulletTextWrapped("If values look wrong, press F4/F5 to return to Normal mode, then re-apply.");
                            BulletTextWrapped("Continuous Recovery can be limited to neutral in Settings.");
                            BulletTextWrapped("Open this Help quickly with %s.", GetKeyName(cfg.helpKey).c_str());
                            BulletTextWrapped("If something seems off, go to the main menu and back to Practice.");

                            ImGui::Dummy(ImVec2(1, 8));
                            ImGui::SeparatorText("Unsupported EfzRevival Versions");
                            ImGui::TextWrapped("If your EfzRevival build isn’t listed as supported (see About), you may see quirks:");
                            ImGui::Indent();
                            BulletTextWrapped("Avoid unsupported versions for netplay. Mod may still detect online sessions via active IP checks, but newer builds of Revival can introduce unexpected issues. If problems occur, you may try launching the game directly via efz.exe as an alternative.");
                            BulletTextWrapped("Hotkeys may still be recognized with this menu open, but the game should remain paused.");
                            ImGui::Unindent();
                            ImGui::EndTabItem();
                        }
                        ImGui::EndTabBar();
                    }
                    ImGui::EndTabItem();
                }

                // Resources
                ImGuiTabItemFlags _setHelp2 = (rq3 == 2) ? ImGuiTabItemFlags_SetSelected : 0;
                if (ImGui::BeginTabItem("Resources", nullptr, _setHelp2)) {
                    guiState.helpSubTab = 2;
                    ImGui::SeparatorText("Game Resources");
                    BulletTextWrapped("Open helpful external resources in your browser:");
                    ImGui::Indent();
                    Link("Eternal Fighter Zero Wiki", "https://wiki.gbl.gg/w/Eternal_Fighter_Zero");
                    Link("Training Mode (EFZ)", "https://wiki.gbl.gg/w/Eternal_Fighter_Zero/Training_Mode");
                    Link("EFZ Global Discord", "https://discord.gg/aUgqXAt");
                    ImGui::Unindent();

                    // Character-specific wiki pages
                    int p1Id = guiState.localData.p1CharID;
                    int p2Id = guiState.localData.p2CharID;
                    std::string p1Name = CharacterSettings::GetCharacterName(p1Id);
                    std::string p2Name = CharacterSettings::GetCharacterName(p2Id);
                    const char* p1Path = GetCharacterWikiPathByID(p1Id);
                    if (p1Path) {
                        std::string url = std::string("https://wiki.gbl.gg/w/Eternal_Fighter_Zero/") + p1Path;
                        ImGui::Separator();
                        ImGui::TextWrapped("P1: %s", p1Name.c_str());
                        Link("Open character wiki (P1)", url.c_str());
                    }
                    const char* p2Path = GetCharacterWikiPathByID(p2Id);
                    if (p2Path) {
                        std::string url = std::string("https://wiki.gbl.gg/w/Eternal_Fighter_Zero/") + p2Path;
                        if (!p1Path) ImGui::Separator();
                        ImGui::TextWrapped("P2: %s", p2Name.c_str());
                        Link("Open character wiki (P2)", url.c_str());
                    }
                    ImGui::EndTabItem();
                }

                // About (moved to end)
                ImGuiTabItemFlags _setHelp3 = (rq3 == 3) ? ImGuiTabItemFlags_SetSelected : 0;
                if (ImGui::BeginTabItem("About", nullptr, _setHelp3)) {
                    guiState.helpSubTab = 3;
                    ImGui::SeparatorText("EFZ Training Mode");
                    ImGui::TextWrapped("Version: %s", EFZ_TRAINING_MODE_VERSION);
                    ImGui::TextWrapped("Build: %s %s", EFZ_TRAINING_MODE_BUILD_DATE, EFZ_TRAINING_MODE_BUILD_TIME);
                    // Show detected EfzRevival version and support status (stub)
                    {
                        EfzRevivalVersion rv = GetEfzRevivalVersion();
                        const char* rvName = EfzRevivalVersionName(rv);
                        bool supported = IsEfzRevivalVersionSupported(rv);
                        ImGui::Dummy(ImVec2(1, 4));
                        ImGui::SeparatorText("Game/Revival Version");
                        ImGui::Text("Detected: %s", rvName);
                        if (!supported) {
                            ImGui::SameLine();
                            ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "(unsupported)");
                            ImGui::TextDisabled("Some features are gated. Online detection and certain functionality might be unavailable.");
                        } else {
                            ImGui::SameLine();
                            ImGui::TextDisabled("(supported)");
                        }
                        ImGui::Dummy(ImVec2(1, 4));
                        ImGui::SeparatorText("Compatibility");
                        ImGui::TextWrapped("Supported EfzRevival builds: Vanilla EFZ (no Revival), EfzRevival 1.02e, 1.02g, 1.02h!!!, 1.02i!!!.");
                    }
                    ImGui::Dummy(ImVec2(1, 4));
                    ImGui::SeparatorText("Overview");
                    ImGui::TextWrapped("A comprehensive training mode enhancement tool for Eternal Fighter Zero. It provides frame advantage display, ability to use macros, triggers and other features controlled by a in-game Gui overlay with live configuration.");
                    ImGui::Dummy(ImVec2(1, 4));
                    ImGui::SeparatorText("Obligatory Michiru");
                    unsigned gw = 0, gh = 0;
                    if (IDirect3DTexture9* tex = GifPlayer::GetTexture(gw, gh)) {
                        const float maxW = 260.0f, maxH = 200.0f;
                        float w = (float)gw, h = (float)gh;
                        if (w > maxW) { float s = maxW / w; w *= s; h *= s; }
                        if (h > maxH) { float s = maxH / h; w *= s; h *= s; }
                        ImGui::Dummy(ImVec2(1, 6));
                        ImGui::Image((ImTextureID)tex, ImVec2(w, h));
                    } else {
                        ImGui::TextDisabled("(GIF not loaded yet)");
                    }
                    // No external links here
                    ImGui::EndTabItem();
                }

                ImGui::EndTabBar();
            }
            ImGui::PopTextWrapPos();
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
                ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "Infinite mode is active.");
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
            ImGui::TextDisabled("(Misuzu training helpers)");
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
                ImGui::SetTooltip("Sequence advances on use. Value is shared across A and B: A then B yields Egg->Radish for 0, etc.");
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
            //ImGui::TextDisabled("(Mai: status @0x3144, multi-timer @0x3148 - meaning depends on status)");
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
            ImGui::TextDisabled("(Mio training helpers)");
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
            if (ImGui::Checkbox("Enlightened (Gold Doppel)##p1Doppel", &enlightened)) {
                guiState.localData.p1DoppelEnlightened = enlightened;
            }
            //ImGui::TextDisabled("(sets the FM-ready flag for testing)");
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
            ImGui::TextDisabled("(Switch carefully during idle)");

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
            ImGui::TextDisabled("(Sets Michiru to Readied stance when idle/unreadied; Practice only)");
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
            if (ImGui::Button("Apply Pos##P1Minagi")) {
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
            // ImGui::SameLine();
            // if (ImGui::Button("Michiru Pos##P1MinagiUseMichiru")) {
            //     if (!std::isnan(guiState.localData.p1MinagiPuppetX) && !std::isnan(guiState.localData.p1MinagiPuppetY)) {
            //         guiState.localData.p1MinagiPuppetSetX = guiState.localData.p1MinagiPuppetX;
            //         guiState.localData.p1MinagiPuppetSetY = guiState.localData.p1MinagiPuppetY;
            //     }
            // }
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
            //ImGui::TextDisabled("(Misuzu: feathers @+0x3148, poison timer @+0x345C, level @+0x3460)");
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
                ImGui::SetTooltip("Sequence advances on use. Value is shared across A and B: A then B yields Egg->Radish for 0, etc.");
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
            ImGui::TextDisabled("(Mai timers - meaning depends on status)");
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
            ImGui::TextDisabled("(Kano training helpers)");
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
            ImGui::TextDisabled("(Nayuki training helpers)");
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
            //ImGui::TextDisabled("(Mio: stance byte at +0x3150, 0=Short,1=Long)");
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
            if (ImGui::Button("Apply Pos##P2Minagi")) {
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
            // ImGui::SameLine();
            // if (ImGui::Button("Michiru Pos##P2MinagiUseMichiru")) {
            //     if (!std::isnan(guiState.localData.p2MinagiPuppetX) && !std::isnan(guiState.localData.p2MinagiPuppetY)) {
            //         guiState.localData.p2MinagiPuppetSetX = guiState.localData.p2MinagiPuppetX;
            //         guiState.localData.p2MinagiPuppetSetY = guiState.localData.p2MinagiPuppetY;
            //     }
            // }
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
            "Supported: Ikumi (Blood/Genocide), Misuzu (Feathers), Mishio (Element/Awakened), Rumi (Stance, Kimchi), Akiko (Bullet/Time-Slow), Neyuki (Jam 0-9), Kano (Magic), Mio (Stance), Doppel (Enlightened(Gold)), Mai (Ghost/Awakening), Minagi (Michiru position control + Always readied)");
    }
    
    // Add this new function to the ImGuiGui namespace:
    void RenderDebugInputTab() {
        // Engine regen/CR status moved here from Values tab
        ImGui::SeparatorText("Engine Regen / Continuous Recovery Status");
        uint16_t engineParamA = 0, engineParamB = 0; EngineRegenMode regenMode = EngineRegenMode::Unknown;
        bool gotParams = GetEngineRegenStatus(regenMode, engineParamA, engineParamB);
        static bool s_doDeepScanDbg = false;
        ImGui::Checkbox("Deep Scan Params##dbgdeep_global", &s_doDeepScanDbg);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Brute-force scan window to locate Param A/B if offsets drift. F4 requires +5 cadence.");
        }
        uint32_t scanAOff=0, scanBOff=0; uint16_t scanAVal=0, scanBVal=0; bool scanOk=false;
        if (s_doDeepScanDbg) {
            uintptr_t base = GetEFZBase(); uintptr_t p1Base=0; if (base) SafeReadMemory(base + EFZ_BASE_OFFSET_P1, &p1Base, sizeof(p1Base));
            if (p1Base) scanOk = DebugScanRegenParamWindow(p1Base, scanAOff, scanAVal, scanBOff, scanBVal);
        }
        if (gotParams) {
            const char* modeLabel = (regenMode==EngineRegenMode::F4_FineTuneActive?"F4 Fine-Tune" : (regenMode==EngineRegenMode::F5_FullOrPreset?"F5 Cycle" : (regenMode==EngineRegenMode::Normal?"Normal":"Unknown")));
            ImGui::Text("Param A: %u  Param B: %u  Mode: %s", (unsigned)engineParamA, (unsigned)engineParamB, modeLabel);
            float derivedRF=0.0f; bool derivedBlue=false;
            if (DeriveRfFromParamA(engineParamA, derivedRF, derivedBlue)) {
                ImGui::SameLine();
                ImGui::TextDisabled("[Derived RF: %.1f, %s]", derivedRF, derivedBlue?"Blue":"Red");
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Mapping: 0..999=Red, 1000=Blue full, 1001..2000 => Blue with RF=(2000-A)");
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Heuristic: F5 when A==1000/2000 or B==3332; F4 fine-tune when B==9999 and A stepping.");
        } else {
            ImGui::Text("Param A/B unavailable (not in match or read failed).");
        }
        if (s_doDeepScanDbg) {
            if (scanOk) {
                ImGui::TextDisabled("Scan Offsets: A@0x%X=%u B@0x%X=%u", scanAOff, (unsigned)scanAVal, scanBOff, (unsigned)scanBVal);
                if (scanAOff != PLAYER_PARAM_A_COPY_OFFSET || scanBOff != PLAYER_PARAM_B_COPY_OFFSET) {
                    ImGui::TextColored(ImVec4(1,0.3f,0.3f,1), "Mismatch: defined offsets 0x%X/0x%X differ from scan 0x%X/0x%X", PLAYER_PARAM_A_COPY_OFFSET, PLAYER_PARAM_B_COPY_OFFSET, scanAOff, scanBOff);
                }
            } else {
                ImGui::TextColored(ImVec4(0.9f,0.6f,0.2f,1), "Scan found no candidates in window.");
            }
        }
        // Summarize lock state similar to Values tab
        bool engineLocksValues = (regenMode == EngineRegenMode::F5_FullOrPreset);
        bool crAny = (guiState.localData.p1ContinuousRecoveryEnabled && (guiState.localData.p1RecoveryHpMode>0 || guiState.localData.p1RecoveryMeterMode>0 || guiState.localData.p1RecoveryRfMode>0)) ||
                      (guiState.localData.p2ContinuousRecoveryEnabled && (guiState.localData.p2RecoveryHpMode>0 || guiState.localData.p2RecoveryMeterMode>0 || guiState.localData.p2RecoveryRfMode>0));
        if (engineLocksValues) {
            ImGui::TextColored(ImVec4(1,0.6f,0.2f,1), "Engine-managed regeneration active; manual value edits disabled.");
        }
        if (crAny) {
            ImGui::TextColored(ImVec4(0.8f,0.4f,1,1), "Continuous Recovery active; manual value edits disabled to avoid conflict.");
        }
        if (!engineLocksValues && !crAny) {
            ImGui::TextDisabled("Manual edits enabled (no engine regen or CR overrides detected).");
        }
        ImGui::Separator();
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

        // RF Freeze Status & Controls
        ImGui::Separator();
        ImGui::SeparatorText("RF Freeze Status");
        auto renderFreezeRow = [&](int player){
            bool active=false, colorManaged=false, colorBlue=false; double value=0.0;
            if (!GetRFFreezeStatus(player, active, value, colorManaged, colorBlue)) return;
            const char* side = (player==1? "P1" : "P2");
            ImGui::Text("%s: %s", side, active?"Active":"Inactive");
            if (active) {
                ImGui::SameLine(); ImGui::TextDisabled("RF=%.1f", (float)value);
                ImGui::SameLine(); ImGui::TextDisabled("ColorLock=%s%s", colorManaged?"On":"Off", (colorManaged? (colorBlue?"(Blue)":"(Red)") : ""));
                RFFreezeOrigin origin = GetRFFreezeOrigin(player);
                const char* olabel = (origin==RFFreezeOrigin::ManualUI?"Manual UI":(origin==RFFreezeOrigin::ContinuousRecovery?"Continuous Recovery":(origin==RFFreezeOrigin::Other?"Other":"Unknown")));
                ImGui::SameLine(); ImGui::TextDisabled("Source=%s", olabel);
                ImGui::SameLine(); if (ImGui::SmallButton(player==1?"Cancel##rf_cancel_p1":"Cancel##rf_cancel_p2")) { StopRFFreezePlayer(player); }
            }
        };
        renderFreezeRow(1);
        renderFreezeRow(2);

        // Minagi debug controls: conversion toggle shown only when Minagi is present
        /*if (guiState.localData.p1CharID == CHAR_ID_MINAGI || guiState.localData.p2CharID == CHAR_ID_MINAGI) {
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
        }*/
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
            // Refresh once on menu open
            RefreshLocalData();

            // On menu open, update IC color ONCE if not managed by RF Freeze color lock or Continuous Recovery
            // This ensures the game color matches the last applied Values tab selection without continuous locking.
            auto crColorManaged = [&](int player)->bool {
                if (player == 1) {
                    if (!displayData.p1ContinuousRecoveryEnabled) return false;
                    if (displayData.p1RecoveryRfMode == 3 || displayData.p1RecoveryRfMode == 4) return true; // Red presets force Red later
                    if (displayData.p1RecoveryRfMode == 5 && displayData.p1RecoveryRfForceBlueIC) return true; // Custom+BIC forces Blue
                    return false;
                } else {
                    if (!displayData.p2ContinuousRecoveryEnabled) return false;
                    if (displayData.p2RecoveryRfMode == 3 || displayData.p2RecoveryRfMode == 4) return true;
                    if (displayData.p2RecoveryRfMode == 5 && displayData.p2RecoveryRfForceBlueIC) return true;
                    return false;
                }
            };
            bool p1Managed = IsRFFreezeColorManaging(1) || crColorManaged(1);
            bool p2Managed = IsRFFreezeColorManaging(2) || crColorManaged(2);
            if (!p1Managed || !p2Managed) {
                if (!p1Managed && !p2Managed) {
                    SetICColorDirect(displayData.p1BlueIC, displayData.p2BlueIC);
                } else if (!p1Managed) {
                    SetICColorPlayer(1, displayData.p1BlueIC);
                } else if (!p2Managed) {
                    SetICColorPlayer(2, displayData.p2BlueIC);
                }
            }
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
            ClampMainWindowToClientBounds();

        // Provide current overlay center for virtual cursor recenters (middle click / L3)
        ImVec2 wpos = ImGui::GetWindowPos();
        ImVec2 wsize = ImGui::GetWindowSize();
        ImVec2 center = ImVec2(wpos.x + wsize.x * 0.5f, wpos.y + wsize.y * 0.5f);
            ImGuiImpl::SetOverlayCenter(center);
            // If input layer requested overlay focus (e.g., middle-click/L3 recenter), honor it here
            if (ImGuiImpl::ConsumeOverlayFocusRequest()) {
                ImGui::SetWindowFocus();
                // Place keyboard/gamepad nav at center-most item by setting default focus to the window
                ImGui::SetItemDefaultFocus();
            }

            // Capture any requested top-level tab selection; we'll apply it via SetSelected flags below
            int requestedTopTab = -1;
            if (guiState.requestedTab >= 0) {
                requestedTopTab = guiState.requestedTab;
                guiState.requestedTab = -1; // consume request
            }
            // Create a scrollable content region with a fixed-height footer for action buttons
            ImVec2 avail = ImGui::GetContentRegionAvail();
            float footerHeight = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y + ImGui::GetStyle().WindowPadding.y;
            // Guard against tiny windows
            if (footerHeight < 32.0f) footerHeight = 32.0f;

            if (ImGui::BeginChild("##MainContent", ImVec2(avail.x, avail.y - footerHeight), true)) {
                // Tab bar at the top
                if (ImGui::BeginTabBar("##Tabs", ImGuiTabBarFlags_None)) {
                    // Precompute selection flags for programmatic tab switching
                    ImGuiTabItemFlags __mainFlags = (requestedTopTab == 0) ? ImGuiTabItemFlags_SetSelected : 0;
                    ImGuiTabItemFlags __autoFlags = (requestedTopTab == 1) ? ImGuiTabItemFlags_SetSelected : 0;
                    ImGuiTabItemFlags __settingsFlags = (requestedTopTab == 5) ? ImGuiTabItemFlags_SetSelected : 0;
                    ImGuiTabItemFlags __charFlags = (requestedTopTab == 2) ? ImGuiTabItemFlags_SetSelected : 0;
                    ImGuiTabItemFlags __helpFlags = ImGuiTabItemFlags_NoCloseWithMiddleMouseButton | ((requestedTopTab == 4) ? ImGuiTabItemFlags_SetSelected : 0);

                    // Main Menu tab
                    if (ImGui::BeginTabItem("Main Menu", nullptr, __mainFlags)) {
                        guiState.currentTab = 0;
                        RenderGameValuesTab();
                        ImGui::EndTabItem();
                    }
                    
                    // Auto Actions tab
                    if (ImGui::BeginTabItem("Auto Actions", nullptr, __autoFlags)) {
                        guiState.currentTab = 1;
                        RenderAutoActionTab();
                        ImGui::EndTabItem();
                    }

                    // Characters tab; refresh character IDs once on open to avoid per-frame work
                    if (ImGui::BeginTabItem("Characters", nullptr, __charFlags)) {
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
                    
                    // Settings tab (moved after Characters)
                    if (ImGui::BeginTabItem("Settings", nullptr, __settingsFlags)) {
                        guiState.currentTab = 5;
                        ImGuiSettings::RenderSettingsTab();
                        ImGui::EndTabItem();
                    }
                    
                    // Debug tab moved under Settings -> Debug sub-tab
                    
                    // Help tab(s)
                    if (ImGui::BeginTabItem("Help", nullptr, __helpFlags)) {
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

        // Resolve player base pointers once; avoid multiple ResolvePointer() calls per field.
        uintptr_t p1Base = 0, p2Base = 0;
        SafeReadMemory(base + EFZ_BASE_OFFSET_P1, &p1Base, sizeof(p1Base));
        SafeReadMemory(base + EFZ_BASE_OFFSET_P2, &p2Base, sizeof(p2Base));
        if (!p1Base || !p2Base) {
            LogOut("[IMGUI] RefreshLocalData: Player base ptr(s) unavailable", true);
            return;
        }

        // P1 reads (single pass)
        {
            SafeReadMemory(p1Base + HP_OFFSET, &guiState.localData.hp1, sizeof(int));
            unsigned short m1 = 0; SafeReadMemory(p1Base + METER_OFFSET, &m1, sizeof(m1)); guiState.localData.meter1 = (int)m1;
            SafeReadMemory(p1Base + RF_OFFSET, &guiState.localData.rf1, sizeof(double));
            SafeReadMemory(p1Base + XPOS_OFFSET, &guiState.localData.x1, sizeof(double));
            SafeReadMemory(p1Base + YPOS_OFFSET, &guiState.localData.y1, sizeof(double));
            memset(guiState.localData.p1CharName, 0, sizeof(guiState.localData.p1CharName));
            SafeReadMemory(p1Base + CHARACTER_NAME_OFFSET, guiState.localData.p1CharName, sizeof(guiState.localData.p1CharName) - 1);
        }

        // P2 reads (single pass)
        {
            SafeReadMemory(p2Base + HP_OFFSET, &guiState.localData.hp2, sizeof(int));
            unsigned short m2 = 0; SafeReadMemory(p2Base + METER_OFFSET, &m2, sizeof(m2)); guiState.localData.meter2 = (int)m2;
            SafeReadMemory(p2Base + RF_OFFSET, &guiState.localData.rf2, sizeof(double));
            SafeReadMemory(p2Base + XPOS_OFFSET, &guiState.localData.x2, sizeof(double));
            SafeReadMemory(p2Base + YPOS_OFFSET, &guiState.localData.y2, sizeof(double));
            memset(guiState.localData.p2CharName, 0, sizeof(guiState.localData.p2CharName));
            SafeReadMemory(p2Base + CHARACTER_NAME_OFFSET, guiState.localData.p2CharName, sizeof(guiState.localData.p2CharName) - 1);
        }

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
    guiState.localData.randomizeTriggers   = triggerRandomizeEnabled.load();

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

    // Per-trigger multi-action pool config
    guiState.localData.afterBlockActionPoolMask   = triggerAfterBlockActionPoolMask.load();
    guiState.localData.onWakeupActionPoolMask     = triggerOnWakeupActionPoolMask.load();
    guiState.localData.afterHitstunActionPoolMask = triggerAfterHitstunActionPoolMask.load();
    guiState.localData.afterAirtechActionPoolMask = triggerAfterAirtechActionPoolMask.load();
    guiState.localData.onRGActionPoolMask         = triggerOnRGActionPoolMask.load();
    guiState.localData.afterBlockUseActionPool    = triggerAfterBlockUsePool.load();
    guiState.localData.onWakeupUseActionPool      = triggerOnWakeupUsePool.load();
    guiState.localData.afterHitstunUseActionPool  = triggerAfterHitstunUsePool.load();
    guiState.localData.afterAirtechUseActionPool  = triggerAfterAirtechUsePool.load();
    guiState.localData.onRGUseActionPool          = triggerOnRGUsePool.load();

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

    // Copy character-specific settings from displayData (which may have been reset)
    // This ensures GUI checkboxes reflect the current state after ResetDisplayDataToDefaults()
    guiState.localData.p1NayukiSnowbunnies = displayData.p1NayukiSnowbunnies;
    guiState.localData.p2NayukiSnowbunnies = displayData.p2NayukiSnowbunnies;
    guiState.localData.p1NayukiInfiniteSnow = displayData.p1NayukiInfiniteSnow;
    guiState.localData.p2NayukiInfiniteSnow = displayData.p2NayukiInfiniteSnow;
    guiState.localData.infiniteBloodMode = displayData.infiniteBloodMode;
    guiState.localData.p1MaiInfiniteGhost = displayData.p1MaiInfiniteGhost;
    guiState.localData.p1MaiInfiniteCharge = displayData.p1MaiInfiniteCharge;
    guiState.localData.p1MaiInfiniteAwakening = displayData.p1MaiInfiniteAwakening;
    guiState.localData.p2MaiInfiniteCharge = displayData.p2MaiInfiniteCharge;
    guiState.localData.p2MaiInfiniteAwakening = displayData.p2MaiInfiniteAwakening;
    guiState.localData.p1MisuzuInfinitePoison = displayData.p1MisuzuInfinitePoison;
    guiState.localData.p2MisuzuInfinitePoison = displayData.p2MisuzuInfinitePoison;
    guiState.localData.infiniteMishioElement = displayData.infiniteMishioElement;
    guiState.localData.infiniteMishioAwakened = displayData.infiniteMishioAwakened;
    guiState.localData.p1RumiInfiniteShinai = displayData.p1RumiInfiniteShinai;
    guiState.localData.p2RumiInfiniteShinai = displayData.p2RumiInfiniteShinai;
    guiState.localData.p1RumiInfiniteKimchi = displayData.p1RumiInfiniteKimchi;
    guiState.localData.p2RumiInfiniteKimchi = displayData.p2RumiInfiniteKimchi;
    guiState.localData.p1AkikoInfiniteTimeslow = displayData.p1AkikoInfiniteTimeslow;
    guiState.localData.p2AkikoInfiniteTimeslow = displayData.p2AkikoInfiniteTimeslow;
    // Minagi (puppet settings and flags)
    guiState.localData.p1MinagiAlwaysReadied = displayData.p1MinagiAlwaysReadied;
    guiState.localData.p2MinagiAlwaysReadied = displayData.p2MinagiAlwaysReadied;
    guiState.localData.minagiConvertNewProjectiles = displayData.minagiConvertNewProjectiles;
    guiState.localData.p1MinagiApplyPos = displayData.p1MinagiApplyPos;
    guiState.localData.p2MinagiApplyPos = displayData.p2MinagiApplyPos;
    guiState.localData.p1MinagiPuppetSetX = displayData.p1MinagiPuppetSetX;
    guiState.localData.p1MinagiPuppetSetY = displayData.p1MinagiPuppetSetY;
    guiState.localData.p2MinagiPuppetSetX = displayData.p2MinagiPuppetSetX;
    guiState.localData.p2MinagiPuppetSetY = displayData.p2MinagiPuppetSetY;
    // Note: p1MinagiPuppetX/Y and p2MinagiPuppetX/Y are read from memory by ScanMichiru above
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
            triggerRandomizeEnabled.store(displayData.randomizeTriggers);

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

            // Per-trigger multi-action pools
            triggerAfterBlockActionPoolMask.store(displayData.afterBlockActionPoolMask);
            triggerOnWakeupActionPoolMask.store(displayData.onWakeupActionPoolMask);
            triggerAfterHitstunActionPoolMask.store(displayData.afterHitstunActionPoolMask);
            triggerAfterAirtechActionPoolMask.store(displayData.afterAirtechActionPoolMask);
            triggerOnRGActionPoolMask.store(displayData.onRGActionPoolMask);
            triggerAfterBlockUsePool.store(displayData.afterBlockUseActionPool);
            triggerOnWakeupUsePool.store(displayData.onWakeupUseActionPool);
            triggerAfterHitstunUsePool.store(displayData.afterHitstunUseActionPool);
            triggerAfterAirtechUsePool.store(displayData.afterAirtechUseActionPool);
            triggerOnRGUsePool.store(displayData.onRGUseActionPool);

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

            // Copy per-trigger option rows to runtime mirrors (clamped to MAX_TRIGGER_OPTIONS)
            auto clampCopy = [](int srcCount, const TriggerOption* srcArr, int& dstCount, TriggerOption* dstArr){
                int n = srcCount; if (n < 0) n = 0; if (n > MAX_TRIGGER_OPTIONS) n = MAX_TRIGGER_OPTIONS;
                dstCount = n;
                for (int i=0;i<n;i++) dstArr[i] = srcArr[i];
                for (int i=n;i<MAX_TRIGGER_OPTIONS;i++) dstArr[i] = TriggerOption{false, ACTION_5A, 0, 0, (int)BASE_ATTACK_5A, 0};
            };
            clampCopy(displayData.afterBlockOptionCount,    displayData.afterBlockOptions,    g_afterBlockOptionCount,    g_afterBlockOptions);
            clampCopy(displayData.onWakeupOptionCount,      displayData.onWakeupOptions,      g_onWakeupOptionCount,      g_onWakeupOptions);
            clampCopy(displayData.afterHitstunOptionCount,  displayData.afterHitstunOptions,  g_afterHitstunOptionCount,  g_afterHitstunOptions);
            clampCopy(displayData.afterAirtechOptionCount,  displayData.afterAirtechOptions,  g_afterAirtechOptionCount,  g_afterAirtechOptions);
            clampCopy(displayData.onRGOptionCount,          displayData.onRGOptions,          g_onRGOptionCount,          g_onRGOptions);
            
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
            
            // Apply IC color settings ONCE per Apply if not managed by RF Freeze color lock or Continuous Recovery color logic.
            auto crColorManaged = [&](int player)->bool {
                if (player == 1) {
                    if (!displayData.p1ContinuousRecoveryEnabled) return false;
                    if (displayData.p1RecoveryRfMode == 3 || displayData.p1RecoveryRfMode == 4) return true; // Red presets force Red later
                    if (displayData.p1RecoveryRfMode == 5 && displayData.p1RecoveryRfForceBlueIC) return true; // Custom+BIC forces Blue
                    return false;
                } else {
                    if (!displayData.p2ContinuousRecoveryEnabled) return false;
                    if (displayData.p2RecoveryRfMode == 3 || displayData.p2RecoveryRfMode == 4) return true;
                    if (displayData.p2RecoveryRfMode == 5 && displayData.p2RecoveryRfForceBlueIC) return true;
                    return false;
                }
            };
            bool p1Managed = IsRFFreezeColorManaging(1) || crColorManaged(1);
            bool p2Managed = IsRFFreezeColorManaging(2) || crColorManaged(2);
            if (!p1Managed || !p2Managed) {
                // Only write what is not managed to avoid fighting per-frame logic.
                if (!p1Managed && !p2Managed) {
                    SetICColorDirect(displayData.p1BlueIC, displayData.p2BlueIC);
                } else if (!p1Managed) {
                    SetICColorPlayer(1, displayData.p1BlueIC);
                } else if (!p2Managed) {
                    SetICColorPlayer(2, displayData.p2BlueIC);
                }
            }
            
            // Apply the settings to the game
            uintptr_t base = GetEFZBase();
            if (base) {
                // Defer Rumi mode apply if not actionable to avoid unsafe engine calls
                bool deferred = false;
                if (displayData.p1CharID == CHAR_ID_NANASE) {
                    // Use unified sample for current P1 move when available
                    const PerFrameSample &uiSample = GetCurrentPerFrameSample();
                    short mv = uiSample.moveID1;
                    if (!uiSample.actionable1) deferred = true;
                }
                if (displayData.p2CharID == CHAR_ID_NANASE) {
                    const PerFrameSample &uiSample2 = GetCurrentPerFrameSample();
                    short mv = uiSample2.moveID2;
                    if (!uiSample2.actionable2) deferred = true;
                }
                // Determine if engine F4/F5 or Continuous Recovery is managing values
                uint16_t engA = 0, engB = 0; EngineRegenMode regenMode = EngineRegenMode::Unknown;
                bool gotParams = GetEngineRegenStatus(regenMode, engA, engB);
                bool engineLocksValues = gotParams && (regenMode == EngineRegenMode::F4_FineTuneActive || regenMode == EngineRegenMode::F5_FullOrPreset);
                bool crAny = (displayData.p1ContinuousRecoveryEnabled && (displayData.p1RecoveryHpMode>0 || displayData.p1RecoveryMeterMode>0 || displayData.p1RecoveryRfMode>0)) ||
                             (displayData.p2ContinuousRecoveryEnabled && (displayData.p2RecoveryHpMode>0 || displayData.p2RecoveryMeterMode>0 || displayData.p2RecoveryRfMode>0));

                bool onlyApplyXY = engineLocksValues || crAny;

                if (!onlyApplyXY) {
                    // Normal path: apply all values (HP/Meter/X/Y/RF) via legacy ApplySettings
                    ApplySettings(&displayData);
                } else {
                    // Regen/CR active: only apply X/Y, and only if Values tab is active in ImGui
                    if (guiState.mainMenuSubTab == 1) {
                        // Apply positions directly; skip HP/Meter/RF to avoid fighting engine/CR
                        SetPlayerPosition(base, EFZ_BASE_OFFSET_P1, (double)displayData.x1, (double)displayData.y1);
                        SetPlayerPosition(base, EFZ_BASE_OFFSET_P2, (double)displayData.x2, (double)displayData.y2);
                        LogOut("[IMGUI] Engine/CR active — applied X/Y only (Values tab)", detailedLogging.load());
                    } else {
                        LogOut("[IMGUI] Engine/CR active — skipped HP/Meter/RF and X/Y (Values tab not active)", detailedLogging.load());
                    }
                }

                // Run one enforcement tick immediately so infinite toggles take effect without waiting for the next cadence
                CharacterSettings::TickCharacterEnforcements(base, displayData);
                if (deferred) {
                    LogOut("[IMGUI] Rumi mode change deferred; apply again when idle.", true);
                }
                // Refresh trigger overlay text to reflect new settings immediately
                UpdateTriggerOverlay();

                // One-shot Continuous Recovery enforcement on Apply (per-player)
                auto resolveTargets = [&](bool isP1){
                    struct T { int hp; int meter; double rf; bool bic; bool hpOn; bool meterOn; bool rfOn; bool wantRedIC; } t;
                    t = {0,0,0.0,false,false,false,false,false};
                    if (isP1) {
                        // HP
                        if (displayData.p1RecoveryHpMode > 0) {
                            t.hpOn = true;
                            if (displayData.p1RecoveryHpMode == 1) t.hp = MAX_HP;
                            else if (displayData.p1RecoveryHpMode == 2) t.hp = 3332;
                            else if (displayData.p1RecoveryHpMode == 3) t.hp = CLAMP(displayData.p1RecoveryHpCustom, 0, MAX_HP);
                        }
                        // Meter
                        if (displayData.p1RecoveryMeterMode > 0) {
                            t.meterOn = true;
                            if (displayData.p1RecoveryMeterMode == 1) t.meter = 0;
                            else if (displayData.p1RecoveryMeterMode == 2) t.meter = 1000;
                            else if (displayData.p1RecoveryMeterMode == 3) t.meter = 2000;
                            else if (displayData.p1RecoveryMeterMode == 4) t.meter = 3000;
                            else if (displayData.p1RecoveryMeterMode == 5) t.meter = CLAMP(displayData.p1RecoveryMeterCustom, 0, MAX_METER);
                        }
                        // RF
                        if (displayData.p1RecoveryRfMode > 0) {
                            t.rfOn = true;
                            if (displayData.p1RecoveryRfMode == 1) t.rf = 0.0;
                            else if (displayData.p1RecoveryRfMode == 2) t.rf = 1000.0;
                            else if (displayData.p1RecoveryRfMode == 3) t.rf = 500.0;
                            else if (displayData.p1RecoveryRfMode == 4) t.rf = 999.0;
                            else if (displayData.p1RecoveryRfMode == 5) t.rf = (double)CLAMP((int)displayData.p1RecoveryRfCustom, 0, (int)MAX_RF);
                            if (displayData.p1RecoveryRfMode == 5) t.bic = displayData.p1RecoveryRfForceBlueIC; else t.bic = false;
                            t.wantRedIC = (displayData.p1RecoveryRfMode == 3 || displayData.p1RecoveryRfMode == 4);
                        }
                    } else {
                        if (displayData.p2RecoveryHpMode > 0) {
                            t.hpOn = true;
                            if (displayData.p2RecoveryHpMode == 1) t.hp = MAX_HP;
                            else if (displayData.p2RecoveryHpMode == 2) t.hp = 3332;
                            else if (displayData.p2RecoveryHpMode == 3) t.hp = CLAMP(displayData.p2RecoveryHpCustom, 0, MAX_HP);
                        }
                        if (displayData.p2RecoveryMeterMode > 0) {
                            t.meterOn = true;
                            if (displayData.p2RecoveryMeterMode == 1) t.meter = 0;
                            else if (displayData.p2RecoveryMeterMode == 2) t.meter = 1000;
                            else if (displayData.p2RecoveryMeterMode == 3) t.meter = 2000;
                            else if (displayData.p2RecoveryMeterMode == 4) t.meter = 3000;
                            else if (displayData.p2RecoveryMeterMode == 5) t.meter = CLAMP(displayData.p2RecoveryMeterCustom, 0, MAX_METER);
                        }
                        if (displayData.p2RecoveryRfMode > 0) {
                            t.rfOn = true;
                            if (displayData.p2RecoveryRfMode == 1) t.rf = 0.0;
                            else if (displayData.p2RecoveryRfMode == 2) t.rf = 1000.0;
                            else if (displayData.p2RecoveryRfMode == 3) t.rf = 500.0;
                            else if (displayData.p2RecoveryRfMode == 4) t.rf = 999.0;
                            else if (displayData.p2RecoveryRfMode == 5) t.rf = (double)CLAMP((int)displayData.p2RecoveryRfCustom, 0, (int)MAX_RF);
                            if (displayData.p2RecoveryRfMode == 5) t.bic = displayData.p2RecoveryRfForceBlueIC; else t.bic = false;
                            t.wantRedIC = (displayData.p2RecoveryRfMode == 3 || displayData.p2RecoveryRfMode == 4);
                        }
                    }
                    return t;
                };

                auto enforceForPlayer = [&](int p){
                    bool enabled = (p==1) ? displayData.p1ContinuousRecoveryEnabled : displayData.p2ContinuousRecoveryEnabled;
                    if (!enabled) return;
                    // Resolve player bases once
                    uintptr_t p1B=0, p2B=0; SafeReadMemory(base + EFZ_BASE_OFFSET_P1, &p1B, sizeof(p1B)); SafeReadMemory(base + EFZ_BASE_OFFSET_P2, &p2B, sizeof(p2B));
                    if (!p1B || !p2B) return;
                    auto tg = resolveTargets(p==1);
                    bool wrote = false;
                    uintptr_t pB = (p==1)? p1B : p2B;
                    if (tg.hpOn) {
                        int tgt = CLAMP(tg.hp, 0, MAX_HP);
                        SafeWriteMemory(pB + HP_OFFSET, &tgt, sizeof(tgt));
                        SafeWriteMemory(pB + HP_BAR_OFFSET, &tgt, sizeof(tgt));
                        wrote = true;
                    }
                    if (tg.meterOn) {
                        WORD tgt = (WORD)CLAMP(tg.meter, 0, MAX_METER);
                        SafeWriteMemory(pB + METER_OFFSET, &tgt, sizeof(tgt));
                        wrote = true;
                    }
                    if (tg.rfOn) {
                        double p1rf=0.0, p2rf=0.0;
                        SafeReadMemory(p1B + RF_OFFSET, &p1rf, sizeof(p1rf));
                        SafeReadMemory(p2B + RF_OFFSET, &p2rf, sizeof(p2rf));
                        if (p==1) p1rf = tg.rf; else p2rf = tg.rf;
                        (void)SetRFValuesDirect(p1rf, p2rf);
                        if (Config::GetSettings().freezeRFAfterContRec) {
                            StartRFFreezeOneFromCR(p, tg.rf);
                        }
                        // BIC (Blue IC) only under Custom RF
                        if (tg.bic) {
                            int ic1=1, ic2=1; SafeReadMemory(p1B + IC_COLOR_OFFSET, &ic1, sizeof(ic1)); SafeReadMemory(p2B + IC_COLOR_OFFSET, &ic2, sizeof(ic2));
                            bool p1Blue = (p==1)? true : (ic1 != 0);
                            bool p2Blue = (p==2)? true : (ic2 != 0);
                            SetICColorDirect(p1Blue, p2Blue);
                        } else if (tg.wantRedIC) {
                            int ic1=0, ic2=0; SafeReadMemory(p1B + IC_COLOR_OFFSET, &ic1, sizeof(ic1)); SafeReadMemory(p2B + IC_COLOR_OFFSET, &ic2, sizeof(ic2));
                            bool p1Blue = (ic1 != 0), p2Blue = (ic2 != 0);
                            if ((p==1 && p1Blue) || (p==2 && p2Blue)) {
                                bool newP1Blue = (p==1) ? false : p1Blue;
                                bool newP2Blue = (p==2) ? false : p2Blue;
                                SetICColorDirect(newP1Blue, newP2Blue);
                            }
                        }
                    }
                    if (wrote) {
                        LogOut(std::string("[IMGUI][ContRec] One-shot restore for P") + (p==1?"1":"2"), detailedLogging.load());
                    }
                };

                // Stop per-side RF freeze immediately if CR is disabled or RF mode is Off
                if (!displayData.p1ContinuousRecoveryEnabled || displayData.p1RecoveryRfMode <= 0) { StopRFFreezePlayer(1); }
                if (!displayData.p2ContinuousRecoveryEnabled || displayData.p2RecoveryRfMode <= 0) { StopRFFreezePlayer(2); }

                // Apply one-shot enforcement now (only for sides with CR enabled)
                enforceForPlayer(1);
                enforceForPlayer(2);
            }
        }
    }
}
    // Programmatic navigation helpers implementation
    namespace ImGuiGui {
        // Logical order mapping for top-level tabs
        static int TopTabLogicalToActual(int logicalIndex) {
            // Map (requested order): Main, Auto Actions, Characters, Settings, Help -> actual IDs 0,1,2,5,4
            static const int map[5] = { 0, 1, 2, 5, 4 };
            if (logicalIndex < 0) logicalIndex = 0; if (logicalIndex > 4) logicalIndex = 4;
            return map[logicalIndex];
        }
        static int TopTabActualToLogical(int actual) {
            switch (actual) {
                case 0: return 0; // Main
                case 1: return 1; // Auto Actions
                case 2: return 2; // Characters
                case 5: return 3; // Settings
                case 4: return 4; // Help
                default: return 0;
            }
        }

        void RequestTopTabAbsolute(int logicalIndex) {
            if (logicalIndex < 0) logicalIndex = 0; if (logicalIndex > 4) logicalIndex = 4;
            guiState.requestedTab = TopTabLogicalToActual(logicalIndex);
        }

        void RequestTopTabCycle(int direction) {
            int curLogical = TopTabActualToLogical(guiState.currentTab);
            int next = (curLogical + (direction >= 0 ? 1 : -1));
            if (next < 0) next = 4; else if (next > 4) next = 0;
            guiState.requestedTab = TopTabLogicalToActual(next);
        }

        void RequestActiveSubTabCycle(int direction) {
            // Determine which sub-tab group is active based on the current top-level tab
            const int dir = (direction >= 0) ? 1 : -1;
            if (guiState.currentTab == 0) {
                // Main Menu: 3 sub-tabs
                int idx = guiState.mainMenuSubTab;
                int count = 3;
                idx = (idx + dir) % count; if (idx < 0) idx += count;
                guiState.mainMenuSubTab = idx;
                guiState.requestedMainMenuSubTab = idx;
            } else if (guiState.currentTab == 1) {
                // Auto Action: 2 sub-tabs
                int idx = guiState.autoActionSubTab;
                int count = 2;
                idx = (idx + dir) % count; if (idx < 0) idx += count;
                guiState.autoActionSubTab = idx;
                guiState.requestedAutoActionSubTab = idx;
            } else if (guiState.currentTab == 4) {
                // Help: 4 sub-tabs (Getting Started, Guide, Resources, About)
                int idx = guiState.helpSubTab;
                int count = 4;
                idx = (idx + dir) % count; if (idx < 0) idx += count;
                guiState.helpSubTab = idx;
                guiState.requestedHelpSubTab = idx;
            } else {
                // No sub-tabs for Settings/Character; do nothing
            }
        }
    }