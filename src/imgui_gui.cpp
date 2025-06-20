#include "../include/imgui_gui.h"
#include "../include/imgui_impl.h"
#include "../include/utilities.h"
#include "../include/constants.h"
#include "../include/memory.h"
#include "../include/logger.h"
#include "../include/gui.h"

namespace ImGuiGui {
    // Action type mapping (same as in gui_auto_action.cpp)
    static const int ComboIndexToActionType[] = {
        ACTION_5A, ACTION_5B, ACTION_5C, ACTION_2A, ACTION_2B, ACTION_2C,
        ACTION_JA, ACTION_JB, ACTION_JC, ACTION_JUMP, ACTION_BACKDASH,
        ACTION_BLOCK, ACTION_CUSTOM
    };

    // Helper function to convert action type to combo index
    static int ActionTypeToComboIndex(int actionType) {
        for (int i = 0; i < IM_ARRAYSIZE(ComboIndexToActionType); i++) {
            if (ComboIndexToActionType[i] == actionType) return i;
        }
        return 0; // Default to 5A
    }

    // Initialize GUI state
    ImGuiGuiState guiState = {
        false,  // visible
        0,      // currentTab
        {}      // localData (initialized with default values)
    };

    // Initialize the GUI
    void Initialize() {
        // Copy current display data into our local copy
        guiState.localData = displayData;
        LogOut("[IMGUI_GUI] GUI state initialized", true);
    }

    // Main render function
    void RenderGui() {
        if (!ImGuiImpl::IsVisible())
            return;

        // Set window position and size
        ImGui::SetNextWindowPos(ImVec2(50, 50), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(580, 500), ImGuiCond_FirstUseEver);

        // Main window
        if (ImGui::Begin("EFZ Training Mode", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            // Tab bar
            if (ImGui::BeginTabBar("MainTabBar", ImGuiTabBarFlags_None)) {
                // Game Values Tab
                if (ImGui::BeginTabItem("Game Values")) {
                    RenderGameValuesTab();
                    ImGui::EndTabItem();
                }

                // Movement Options Tab
                if (ImGui::BeginTabItem("Movement Options")) {
                    RenderMovementOptionsTab();
                    ImGui::EndTabItem();
                }

                // Auto Action Tab
                if (ImGui::BeginTabItem("Auto Action")) {
                    RenderAutoActionTab();
                    ImGui::EndTabItem();
                }

                ImGui::EndTabBar();
            }

            // Buttons at bottom of window
            ImGui::Separator();
            if (ImGui::Button("Apply", ImVec2(100, 30))) {
                ApplyImGuiSettings();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(100, 30))) {
                ImGuiImpl::ToggleVisibility();
            }
        }
        ImGui::End();
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

    // Movement Options Tab
    void RenderMovementOptionsTab() {
        ImGui::TextUnformatted("Auto-Jump Settings:");
        ImGui::Separator();

        // Jump Direction
        ImGui::TextUnformatted("Jump Direction:");
        const char* jumpDirItems[] = { "Disabled", "Straight Jump", "Forward Jump", "Backward Jump" };
        int jumpDir = guiState.localData.autoJump ? guiState.localData.jumpDirection + 1 : 0;
        if (ImGui::Combo("##JumpDir", &jumpDir, jumpDirItems, IM_ARRAYSIZE(jumpDirItems))) {
            guiState.localData.autoJump = (jumpDir > 0);
            guiState.localData.jumpDirection = jumpDir > 0 ? jumpDir - 1 : 0;
        }

        // Jump Target
        ImGui::TextUnformatted("Apply To:");
        const char* jumpTargetItems[] = { "P1 Only", "P2 Only", "Both Players" };
        int jumpTarget = guiState.localData.jumpTarget - 1;
        if (ImGui::Combo("##JumpTarget", &jumpTarget, jumpTargetItems, IM_ARRAYSIZE(jumpTargetItems))) {
            guiState.localData.jumpTarget = jumpTarget + 1;
        }

        // Help text
        ImGui::Separator();
        ImGui::TextWrapped(
            "Auto-Jump makes the selected player(s) automatically jump when they land.\n"
            "Select Disabled to turn off this feature."
        );
    }

    // Auto Action Tab
    void RenderAutoActionTab() {
        // Master enable checkbox
        bool autoActionEnabled = guiState.localData.autoAction;
        if (ImGui::Checkbox("Enable Auto Action System", &autoActionEnabled)) {
            guiState.localData.autoAction = autoActionEnabled;
        }

        // Player target (applies to all triggers)
        ImGui::TextUnformatted("Apply To:");
        const char* playerTargetItems[] = { "P1 Only", "P2 Only", "Both Players" };
        int playerTarget = guiState.localData.autoActionPlayer - 1;
        if (ImGui::Combo("##PlayerTarget", &playerTarget, playerTargetItems, IM_ARRAYSIZE(playerTargetItems))) {
            guiState.localData.autoActionPlayer = playerTarget + 1;
        }

        ImGui::Separator();

        // Create a table for the triggers
        if (ImGui::BeginTable("AutoActionTable", 4, ImGuiTableFlags_Borders)) {
            ImGui::TableSetupColumn("Trigger");
            ImGui::TableSetupColumn("Action");
            ImGui::TableSetupColumn("Delay");
            ImGui::TableSetupColumn("Custom ID");
            ImGui::TableHeadersRow();

            // Common action items
            const char* actionItems[] = { "5A", "5B", "5C", "2A", "2B", "2C", 
                                         "j.A", "j.B", "j.C", "Jump", "Backdash", "Block", "Custom" };

            // After Block trigger
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            bool triggerAfterBlock = guiState.localData.triggerAfterBlock;
            if (ImGui::Checkbox("After Block", &triggerAfterBlock)) {
                guiState.localData.triggerAfterBlock = triggerAfterBlock;
            }
            
            ImGui::TableNextColumn();
            int actionAfterBlock = ActionTypeToComboIndex(guiState.localData.actionAfterBlock);
            ImGui::PushItemWidth(-1);
            if (ImGui::Combo("##ActionAfterBlock", &actionAfterBlock, actionItems, IM_ARRAYSIZE(actionItems))) {
                guiState.localData.actionAfterBlock = ComboIndexToActionType[actionAfterBlock];
            }
            ImGui::PopItemWidth();
            
            ImGui::TableNextColumn();
            int delayAfterBlock = guiState.localData.delayAfterBlock;
            ImGui::PushItemWidth(-1);
            if (ImGui::InputInt("##DelayAfterBlock", &delayAfterBlock)) {
                guiState.localData.delayAfterBlock = CLAMP(delayAfterBlock, 0, 60);
            }
            ImGui::PopItemWidth();
            
            ImGui::TableNextColumn();
            int customAfterBlock = guiState.localData.customAfterBlock;
            ImGui::PushItemWidth(-1);
            if (ImGui::InputInt("##CustomAfterBlock", &customAfterBlock)) {
                guiState.localData.customAfterBlock = CLAMP(customAfterBlock, 0, 500);
            }
            ImGui::PopItemWidth();

            // After Hitstun trigger
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            bool triggerAfterHitstun = guiState.localData.triggerAfterHitstun;
            if (ImGui::Checkbox("After Hitstun", &triggerAfterHitstun)) {
                guiState.localData.triggerAfterHitstun = triggerAfterHitstun;
            }
            
            ImGui::TableNextColumn();
            int actionAfterHitstun = ActionTypeToComboIndex(guiState.localData.actionAfterHitstun);
            ImGui::PushItemWidth(-1);
            if (ImGui::Combo("##ActionAfterHitstun", &actionAfterHitstun, actionItems, IM_ARRAYSIZE(actionItems))) {
                guiState.localData.actionAfterHitstun = ComboIndexToActionType[actionAfterHitstun];
            }
            ImGui::PopItemWidth();
            
            ImGui::TableNextColumn();
            int delayAfterHitstun = guiState.localData.delayAfterHitstun;
            ImGui::PushItemWidth(-1);
            if (ImGui::InputInt("##DelayAfterHitstun", &delayAfterHitstun)) {
                guiState.localData.delayAfterHitstun = CLAMP(delayAfterHitstun, 0, 60);
            }
            ImGui::PopItemWidth();
            
            ImGui::TableNextColumn();
            int customAfterHitstun = guiState.localData.customAfterHitstun;
            ImGui::PushItemWidth(-1);
            if (ImGui::InputInt("##CustomAfterHitstun", &customAfterHitstun)) {
                guiState.localData.customAfterHitstun = CLAMP(customAfterHitstun, 0, 500);
            }
            ImGui::PopItemWidth();

            // On Wakeup trigger
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            bool triggerOnWakeup = guiState.localData.triggerOnWakeup;
            if (ImGui::Checkbox("On Wakeup", &triggerOnWakeup)) {
                guiState.localData.triggerOnWakeup = triggerOnWakeup;
            }
            
            ImGui::TableNextColumn();
            int actionOnWakeup = ActionTypeToComboIndex(guiState.localData.actionOnWakeup);
            ImGui::PushItemWidth(-1);
            if (ImGui::Combo("##ActionOnWakeup", &actionOnWakeup, actionItems, IM_ARRAYSIZE(actionItems))) {
                guiState.localData.actionOnWakeup = ComboIndexToActionType[actionOnWakeup];
            }
            ImGui::PopItemWidth();
            
            ImGui::TableNextColumn();
            int delayOnWakeup = guiState.localData.delayOnWakeup;
            ImGui::PushItemWidth(-1);
            if (ImGui::InputInt("##DelayOnWakeup", &delayOnWakeup)) {
                guiState.localData.delayOnWakeup = CLAMP(delayOnWakeup, 0, 60);
            }
            ImGui::PopItemWidth();
            
            ImGui::TableNextColumn();
            int customOnWakeup = guiState.localData.customOnWakeup;
            ImGui::PushItemWidth(-1);
            if (ImGui::InputInt("##CustomOnWakeup", &customOnWakeup)) {
                guiState.localData.customOnWakeup = CLAMP(customOnWakeup, 0, 500);
            }
            ImGui::PopItemWidth();

            // After Airtech trigger
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            bool triggerAfterAirtech = guiState.localData.triggerAfterAirtech;
            if (ImGui::Checkbox("After Airtech", &triggerAfterAirtech)) {
                guiState.localData.triggerAfterAirtech = triggerAfterAirtech;
            }
            
            ImGui::TableNextColumn();
            int actionAfterAirtech = ActionTypeToComboIndex(guiState.localData.actionAfterAirtech);
            ImGui::PushItemWidth(-1);
            if (ImGui::Combo("##ActionAfterAirtech", &actionAfterAirtech, actionItems, IM_ARRAYSIZE(actionItems))) {
                guiState.localData.actionAfterAirtech = ComboIndexToActionType[actionAfterAirtech];
            }
            ImGui::PopItemWidth();
            
            ImGui::TableNextColumn();
            int delayAfterAirtech = guiState.localData.delayAfterAirtech;
            ImGui::PushItemWidth(-1);
            if (ImGui::InputInt("##DelayAfterAirtech", &delayAfterAirtech)) {
                guiState.localData.delayAfterAirtech = CLAMP(delayAfterAirtech, 0, 60);
            }
            ImGui::PopItemWidth();
            
            ImGui::TableNextColumn();
            int customAfterAirtech = guiState.localData.customAfterAirtech;
            ImGui::PushItemWidth(-1);
            if (ImGui::InputInt("##CustomAfterAirtech", &customAfterAirtech)) {
                guiState.localData.customAfterAirtech = CLAMP(customAfterAirtech, 0, 500);
            }
            ImGui::PopItemWidth();

            ImGui::EndTable();
        }

        // Help text
        ImGui::Separator();
        ImGui::TextWrapped(
            "The Auto Action system allows you to set up automatic responses to various game situations.\n"
            "Each trigger can have its own action, delay, and custom move ID (if applicable).\n"
            "Delay is measured in visual frames (0 = instant)."
        );
    }

    // Apply settings to the game
    void ApplyImGuiSettings() {
        // Copy our local data to the global display data
        displayData = guiState.localData;
        
        // Apply the settings by calling the global ApplySettings function
        ::ApplySettings(&displayData);  // Use global namespace resolution
        
        LogOut("[IMGUI_GUI] Settings applied", true);
    }
}