#include "../include/imgui_gui.h"
#include "../include/imgui_impl.h"
#include "../include/utilities.h"
#include "../include/constants.h"
#include "../include/memory.h"
#include "../include/logger.h"
#include "../include/gui.h"
#include "../include/config.h" // Add this include for GetKeyName
#include "../include/overlay.h" // Add this include for DirectDrawHook
#include "../include/character_settings.h"
#include "../include/frame_monitor.h"
#include "../include/input_motion.h"


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
        ACTION_QCF,         // 9 = QCF
        ACTION_DP,          // 10 = DP
        ACTION_QCB,         // 11 = QCB
        ACTION_SUPER1,      // 12 = Super 1
        ACTION_SUPER2,      // 13 = Super 2
        ACTION_JUMP,        // 14 = Jump
        ACTION_BACKDASH,    // 15 = Backdash
        ACTION_BLOCK,       // 16 = Block
        ACTION_CUSTOM       // 17 = Custom
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
            const char* actionItems[] = { 
                "5A", "5B", "5C", "2A", "2B", "2C", 
                "j.A", "j.B", "j.C",
                "QCF", "DP", "QCB", "Super 1", "Super 2",
                "Jump", "Backdash", "Block", "Custom" 
            };
            
            // Make sure the number of items matches the ComboIndexToActionType array
            static_assert(IM_ARRAYSIZE(actionItems) == IM_ARRAYSIZE(ComboIndexToActionType), 
                         "Action items and action types must have the same number of elements");

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

    // NEW: Help Tab implementation
    void RenderHelpTab() {
        ImGui::TextUnformatted("Hotkeys (can be changed in config.ini):");
        ImGui::Separator();

        const Config::Settings& cfg = Config::GetSettings();

        ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "Core Hotkeys:");
        ImGui::BulletText("Open/Close this Menu: %s", GetKeyName(cfg.toggleImGuiKey).c_str());
        ImGui::BulletText("Load Position: %s", GetKeyName(cfg.teleportKey).c_str());
        ImGui::BulletText("Save Position: %s", GetKeyName(cfg.recordKey).c_str());
        ImGui::BulletText("Toggle Detailed Title: %s", GetKeyName(cfg.toggleTitleKey).c_str());
        ImGui::BulletText("Reset Frame Counter: %s", GetKeyName(cfg.resetFrameCounterKey).c_str());
        ImGui::BulletText("Show This Help Screen: %s", GetKeyName(cfg.helpKey).c_str());

        ImGui::Separator();
        ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "Positioning Hotkeys (Hold Load Key + ...):");
        ImGui::BulletText("Swap Player Positions: %s + UP", GetKeyName(cfg.teleportKey).c_str());
        ImGui::BulletText("Center Players: %s + DOWN", GetKeyName(cfg.teleportKey).c_str());
        ImGui::BulletText("Players to Left Corner: %s + LEFT", GetKeyName(cfg.teleportKey).c_str());
        ImGui::BulletText("Players to Right Corner: %s + RIGHT", GetKeyName(cfg.teleportKey).c_str());
        ImGui::BulletText("Round Start Positions: %s + DOWN + A", GetKeyName(cfg.teleportKey).c_str());

        ImGui::Separator();
        ImGui::TextWrapped("Press the key again to close this help screen.");
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
        if (!ImGui::CollapsingHeader("Input Testing", ImGuiTreeNodeFlags_DefaultOpen))
            return;
            
        ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "Direct Input Testing");
        ImGui::Separator();
        
        static int testPlayer = 1;
        static uint8_t testMask = 0x10; // Default to A button
        
        ImGui::RadioButton("Test P1 Input", &testPlayer, 1); ImGui::SameLine();
        ImGui::RadioButton("Test P2 Input", &testPlayer, 2);
        
        // Display memory addresses
        uintptr_t base = GetEFZBase();
        if (base) {
            uintptr_t p1Base = 0, p2Base = 0;
            SafeReadMemory(base + EFZ_BASE_OFFSET_P1, &p1Base, sizeof(uintptr_t));
            SafeReadMemory(base + EFZ_BASE_OFFSET_P2, &p2Base, sizeof(uintptr_t));
            
            ImGui::Text("Input Addresses:");
            ImGui::Text("P1 Base: 0x%llX", p1Base);
            ImGui::Text("P2 Base: 0x%llX", p2Base);
            ImGui::Text("Input Buffer Offset: 0x%X", P1_INPUT_BUFFER_OFFSET);
            ImGui::Text("Index Offset: 0x%X", P1_INPUT_BUFFER_INDEX_OFFSET);
            
            uint8_t p1Index = 0, p2Index = 0;
            SafeReadMemory(p1Base + P1_INPUT_BUFFER_INDEX_OFFSET, &p1Index, sizeof(uint8_t));
            SafeReadMemory(p2Base + P1_INPUT_BUFFER_INDEX_OFFSET, &p2Index, sizeof(uint8_t));
            
            ImGui::Text("Current Indices: P1=%d, P2=%d", p1Index, p2Index);
        }
        
        // Display current inputs
        uint8_t p1Input = GetPlayerInputs(1);
        uint8_t p2Input = GetPlayerInputs(2);
        
        ImGui::Spacing();
        ImGui::Text("Current Inputs:");
        ImGui::Text("P1: 0x%02X | %s%s%s%s | %s%s%s%s", 
            p1Input,
            (p1Input & INPUT_UP) ? "↑" : " ",
            (p1Input & INPUT_DOWN) ? "↓" : " ",
            (p1Input & INPUT_LEFT) ? "←" : " ",
            (p1Input & INPUT_RIGHT) ? "→" : " ",
            (p1Input & INPUT_A) ? "A" : " ",
            (p1Input & INPUT_B) ? "B" : " ",
            (p1Input & INPUT_C) ? "C" : " ",
            (p1Input & INPUT_D) ? "D" : " ");
            
        ImGui::Text("P2: 0x%02X | %s%s%s%s | %s%s%s%s", 
            p2Input,
            (p2Input & INPUT_UP) ? "↑" : " ",
            (p2Input & INPUT_DOWN) ? "↓" : " ",
            (p2Input & INPUT_LEFT) ? "←" : " ",
            (p2Input & INPUT_RIGHT) ? "→" : " ",
            (p2Input & INPUT_A) ? "A" : " ",
            (p2Input & INPUT_B) ? "B" : " ",
            (p2Input & INPUT_C) ? "C" : " ",
            (p2Input & INPUT_D) ? "D" : " ");
        
        ImGui::Spacing();
        
        // Test full input sequence
        if (ImGui::Button("Run Input Test Sequence")) {
            int playerNum = testPlayer;
            std::thread([playerNum]() {
                TestInputSequence(playerNum);
            }).detach();
        }
        
        ImGui::SameLine();
        if (ImGui::Button("Monitor Input Buffer")) {
            int playerNum = testPlayer;
            std::thread([playerNum]() {
                MonitorInputBuffer(playerNum, 300);
            }).detach();
        }
    }

    // Update the RenderGui function to include the new tab:
    void RenderGui() {
        if (!ImGuiImpl::IsVisible())
            return;

        // Auto-refresh data when UI first becomes visible
        static bool lastVisible = false;
        bool currentVisible = ImGuiImpl::IsVisible();
        if (currentVisible && !lastVisible) {
            // UI just became visible, refresh data
            RefreshLocalData();
        }
        lastVisible = currentVisible;

        // Set window position and size
        ImGui::SetNextWindowPos(ImVec2(50, 50), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(580, 520), ImGuiCond_FirstUseEver);

        // Main window
        if (ImGui::Begin("EFZ Training Mode", nullptr, ImGuiWindowFlags_NoCollapse)) {
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
            
            // Add this to log character-specific settings being applied
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
            }
        }
    }
}