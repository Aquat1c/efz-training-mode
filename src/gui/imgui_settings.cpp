#include "../include/gui/imgui_settings.h"
#include "../include/utils/controller_names.h"
#include "../include/utils/config.h"
#include "../include/core/logger.h"
#include "../include/utils/switch_players.h"
#include "../include/game/game_state.h"
#include <windows.h>
#include <Xinput.h>
#pragma comment(lib, "xinput9_1_0.lib")

namespace ImGuiSettings {
    static void CheckboxApply(const char* label, bool& value, const char* section, const char* key) {
        if (ImGui::Checkbox(label, &value)) {
            Config::SetSetting(section, key, value ? "1" : "0");
            // Instant effects
            if (std::string(key) == "DetailedLogging") {
                detailedLogging.store(value);
                LogOut(std::string("[CONFIG/UI] detailedLogging set to ") + (value ? "true" : "false"), false);
            }
        }
    }

    static bool InputKeyHex(const char* label, int& keyCode, const char* setKeyName) {
        char buf[16] = {0};
        // Show as 0xHH
        snprintf(buf, sizeof(buf), "0x%X", keyCode);
        ImGui::SetNextItemWidth(90);
        if (ImGui::InputText(label, buf, sizeof(buf))) {
            // Parse hex or decimal
            int parsed = Config::ParseKeyValue(buf);
            keyCode = parsed;
            Config::SetSetting("Hotkeys", setKeyName, buf);
            return true;
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(%s)", GetKeyName(keyCode).c_str());
        ImGui::SameLine();
        // Press-to-bind helper
        static bool capturing = false;
        static std::string capturingKey;
        std::string btnId = std::string("Bind##") + setKeyName;
        if (!capturing) {
            if (ImGui::Button(btnId.c_str())) {
                capturing = true;
                capturingKey = setKeyName;
                LogOut(std::string("[CONFIG/UI] Capturing key for ") + setKeyName + "... press any key", false);
            }
        } else if (capturing && capturingKey == setKeyName) {
            ImGui::TextColored(ImVec4(1,1,0,1), "Press any key...");
            // Scan for any pressed virtual-key
            for (int vk = 0x01; vk <= 0xFE; ++vk) {
                SHORT state = GetAsyncKeyState(vk);
                if (state & 0x8000) {
                    keyCode = vk;
                    char hexBuf[16];
                    snprintf(hexBuf, sizeof(hexBuf), "0x%X", keyCode);
                    Config::SetSetting("Hotkeys", setKeyName, hexBuf);
                    LogOut(std::string("[CONFIG/UI] ") + setKeyName + " bound to " + GetKeyName(keyCode) + " (" + hexBuf + ")", false);
                    capturing = false;
                    capturingKey.clear();
                    break;
                }
            }
        }
        return false;
    }

    // Specialized footer hotkey rebinder that blocks Enter/Escape/Space and duplicates
    static void FooterHotkeyRebind(const char* actionLabel, int& keyCode, const char* cfgKey) {
        ImGui::TextUnformatted(actionLabel);
        ImGui::SameLine();
        ImGui::TextDisabled("[%s]", GetKeyName(keyCode).c_str());
        ImGui::SameLine();
        std::string btnId = std::string("Rebind##") + cfgKey;
        static bool capturing = false; // shared (only one at a time)
        static std::string which;
        if (!capturing) {
            if (ImGui::Button(btnId.c_str())) { capturing = true; which = cfgKey; }
        } else if (capturing && which == cfgKey) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1,1,0,1), "Press a key (no Enter/Escape/Space)...");
            for (int vk = 0x01; vk <= 0xFE; ++vk) {
                SHORT st = GetAsyncKeyState(vk);
                if (st & 0x8000) {
                    if (vk == VK_RETURN || vk == VK_ESCAPE || vk == VK_SPACE) {
                        LogOut("[CONFIG/UI] Disallowed footer key (Enter/Escape/Space) ignored", false);
                        capturing = false; which.clear();
                        break;
                    }
                    keyCode = vk;
                    char hexBuf[16]; snprintf(hexBuf, sizeof(hexBuf), "0x%X", keyCode);
                    Config::SetSetting("Hotkeys", cfgKey, hexBuf);
                    LogOut(std::string("[CONFIG/UI] ") + cfgKey + " footer key -> " + GetKeyName(keyCode), false);
                    capturing = false; which.clear();
                    break;
                }
            }
        }
    }

    void RenderSettingsTab() {
        const Config::Settings& cfg = Config::GetSettings();

        // We copy values locally so we can mutate with ImGui widgets
    bool useImGui = cfg.useImGui;
    bool logVerbose = cfg.detailedLogging;
    bool fpsDiag = cfg.enableFpsDiagnostics;
    bool restrictPractice = cfg.restrictToPracticeMode;
    bool enableConsole = cfg.enableConsole;
    float uiScale = cfg.uiScale;
    int uiFontMode = cfg.uiFontMode; // 0=Default, 1=Segoe UI

    if (ImGui::CollapsingHeader("General")) {
            CheckboxApply("Use ImGui UI (else legacy dialog)", useImGui, "General", "UseImGui");
            ImGui::SameLine();
            ImGui::TextDisabled("(applies on next menu open)");

            CheckboxApply("Detailed logging", logVerbose, "General", "DetailedLogging");

            CheckboxApply("Enable FPS/timing diagnostics", fpsDiag, "General", "enableFpsDiagnostics");

            // UI Scale slider (live-applied)
            ImGui::Text("UI Scale:");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(140);
            if (ImGui::SliderFloat("##UiScale", &uiScale, 0.70f, 1.50f, "%.2f")) {
                // Persist to config store
                Config::SetSetting("General", "uiScale", std::to_string(uiScale));
            }
            ImGui::SameLine();
            if (ImGui::Button("Reset##UiScale")) {
                uiScale = 0.90f;
                Config::SetSetting("General", "uiScale", "0.90");
            }
            ImGui::SameLine();
            ImGui::TextDisabled("(applies immediately)");

            // UI Font selection
            ImGui::Text("UI Font:");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(180);
            const char* fontItems[] = { "Default (ImGui)", "Segoe UI (Windows)" };
            if (ImGui::Combo("##UiFont", &uiFontMode, fontItems, IM_ARRAYSIZE(fontItems))) {
                Config::SetSetting("General", "uiFont", std::to_string(uiFontMode));
            }
            ImGui::SameLine();
            ImGui::TextDisabled("(applies immediately)");

            CheckboxApply("Restrict features to Practice Mode", restrictPractice, "General", "restrictToPracticeMode");

            // Console visibility toggle
            if (ImGui::Checkbox("Show debug console (restart not required)", &enableConsole)) {
                Config::SetSetting("General", "enableConsole", enableConsole ? "1" : "0");
                if (enableConsole) {
                    if (!GetConsoleWindow()) {
                        CreateDebugConsole();
                    } else {
                        SetConsoleVisibility(true);
                    }
                    SetConsoleReady(true);
                    FlushPendingConsoleLogs();
                } else {
                    // Hide instead of fully destroying to avoid handle churn
                    SetConsoleVisibility(false);
                }
            }
        }

    if (ImGui::CollapsingHeader("Hotkeys")) {
            // Copy to locals
            int teleport = cfg.teleportKey;
            int record = cfg.recordKey;
            int menu = cfg.configMenuKey;
            int toggleTitle = cfg.toggleTitleKey;
            int resetFrame = cfg.resetFrameCounterKey;
            int help = cfg.helpKey;
            int toggleImGui = cfg.toggleImGuiKey;

            InputKeyHex("Teleport/Load", teleport, "TeleportKey");
            InputKeyHex("Record/Save", record, "RecordKey");
            InputKeyHex("Open Config Menu", menu, "ConfigMenuKey");
            InputKeyHex("Toggle Title", toggleTitle, "ToggleTitleKey");
            InputKeyHex("Reset Frame Counter", resetFrame, "ResetFrameCounterKey");
            InputKeyHex("Help", help, "HelpKey");
            InputKeyHex("Toggle ImGui Overlay", toggleImGui, "ToggleImGuiKey");

            ImGui::Separator();
            if (ImGui::Button("Save to disk")) {
                Config::SaveSettings();
                LogOut("[CONFIG/UI] Settings saved to ini", false);
            }
            ImGui::SameLine();
            if (ImGui::Button("Reload from disk")) {
                Config::LoadSettings();
                detailedLogging.store(Config::GetSettings().detailedLogging);
                LogOut("[CONFIG/UI] Settings reloaded from ini", false);
            }

            ImGui::Dummy(ImVec2(1,6));
            ImGui::SeparatorText("Footer Hotkeys");
            int accept = cfg.uiAcceptKey;
            int refresh = cfg.uiRefreshKey;
            int exitK = cfg.uiExitKey;
            FooterHotkeyRebind("Apply", accept, "UIAcceptKey");
            FooterHotkeyRebind("Refresh", refresh, "UIRefreshKey");
            FooterHotkeyRebind("Exit", exitK, "UIExitKey");
            ImGui::TextDisabled("Disallowed: Enter / Escape / Space");
        }

        if (ImGui::CollapsingHeader("Controller Bindings")) {
            const char* help = "Rebind: capture next button (or LT/RT trigger). Disable: set -1. Holds persist after Save.";
            ImGui::TextWrapped("%s", help);
            ImGui::Separator();
            struct Row { const char* label; const char* key; int val; };
            Row rows[] = {
                {"Teleport / Load", "gpTeleportButton", cfg.gpTeleportButton},
                {"Save Position",   "gpSavePositionButton", cfg.gpSavePositionButton},
                {"Switch Players",  "gpSwitchPlayersButton", cfg.gpSwitchPlayersButton},
                {"Swap Positions",  "gpSwapPositionsButton", cfg.gpSwapPositionsButton},
                {"Macro Record",    "gpMacroRecordButton", cfg.gpMacroRecordButton},
                {"Macro Play",      "gpMacroPlayButton", cfg.gpMacroPlayButton},
                {"Macro Slot Next", "gpMacroSlotButton", cfg.gpMacroSlotButton},
                {"Toggle Menu",     "gpToggleMenuButton", cfg.gpToggleMenuButton},
                {"Toggle Overlay",  "gpToggleImGuiButton", cfg.gpToggleImGuiButton}
            };
            static bool capturing = false; static std::string which; static WORD lastButtons = 0; static bool prevLT=false, prevRT=false;
            for (auto &r : rows) {
                ImGui::PushID(r.key);
                ImGui::TextUnformatted(r.label); ImGui::SameLine();
                ImGui::TextDisabled("[%s]", Config::GetGamepadButtonName(r.val).c_str()); ImGui::SameLine();
                std::string btn = std::string("Rebind##") + r.key;
                if (!capturing) {
                    if (ImGui::SmallButton(btn.c_str())) { capturing = true; which = r.key; lastButtons = 0; prevLT=false; prevRT=false; }
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Disable")) {
                        Config::SetSetting("Hotkeys", r.key, "-1");
                    }
                } else if (capturing && which == r.key) {
                    ImGui::TextColored(ImVec4(1,1,0,1), "Press a controller button..."); ImGui::SameLine();
                    if (ImGui::SmallButton("Cancel")) { capturing=false; which.clear(); }
                    // Poll all controllers for capture
                    ::XINPUT_STATE st{}; bool have=false; int picked=-1;
                    for (int i=0;i<4;++i){ if (::XInputGetState(i,&st)==ERROR_SUCCESS){ picked=i; have=true; break; } }
                    if (have) {
                        WORD newly = st.Gamepad.wButtons & ~lastButtons;
                        int assignMask = 0;
                        if (newly) assignMask = newly; // take first new pressed combination as mask
                        bool lt = st.Gamepad.bLeftTrigger > 30; bool rt = st.Gamepad.bRightTrigger > 30;
                        int trigMask = 0; if (lt && !prevLT) trigMask |= 0x10000; if (rt && !prevRT) trigMask |= 0x20000;
                        prevLT = lt; prevRT = rt;
                        if (trigMask) assignMask |= trigMask;
                        if (assignMask) {
                            std::string name = Config::GetGamepadButtonName(assignMask);
                            Config::SetSetting("Hotkeys", which, name);
                            capturing=false; which.clear();
                        }
                        lastButtons = st.Gamepad.wButtons;
                        ImGui::SameLine(); ImGui::TextDisabled("(from pad %d)", picked);
                    } else {
                        ImGui::SameLine(); ImGui::TextDisabled("(no controller found)");
                    }
                }
                ImGui::PopID();
            }
            ImGui::Separator();
            if (ImGui::Button("Save Controller Binds")) { Config::SaveSettings(); LogOut("[CONFIG/UI] Controller binds saved", false); }
            ImGui::SameLine(); if (ImGui::Button("Reload")) { Config::LoadSettings(); LogOut("[CONFIG/UI] Controller binds reloaded", false); }
        }

        ImGui::Separator();
        // Controller selection (XInput)
        {
            int idx = Config::GetSettings().controllerIndex;
            int current = idx; // -1 = All
            // Friendly names for XInput devices (global function)
            std::string labelAll = "All (Any)";
            std::string lbl0 = ::GetControllerNameForIndex(0);
            std::string lbl1 = ::GetControllerNameForIndex(1);
            std::string lbl2 = ::GetControllerNameForIndex(2);
            std::string lbl3 = ::GetControllerNameForIndex(3);
            const char* items[] = { labelAll.c_str(), lbl0.c_str(), lbl1.c_str(), lbl2.c_str(), lbl3.c_str() };
            // Map -1..3 to 0..4 for combo
            int comboIndex = (current < 0) ? 0 : (current + 1);
            ImGui::TextUnformatted("Controller for mod inputs"); ImGui::SameLine();
            if (ImGui::BeginCombo("##controllerIndex", items[comboIndex])) {
                for (int i = 0; i < 5; ++i) {
                    bool sel = (i == comboIndex);
                    if (ImGui::Selectable(items[i], sel)) {
                        int newVal = (i == 0) ? -1 : (i - 1);
                        Config::SetSetting("General", "controllerIndex", std::to_string(newVal));
                    }
                    if (sel) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            ImGui::SameLine(); if (ImGui::SmallButton("Save Controller")) { Config::SaveSettings(); }
        }
        ImGui::TextDisabled("Config file: %s", Config::GetConfigFilePath().c_str());

        // Switch Players control moved to Debug tab
    }
}
