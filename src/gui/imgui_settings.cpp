//
// Clean implementation with tabbed UI and robust controller rebind flow
//

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
    // Pseudo-bits for triggers when mapping to a button mask
    static constexpr uint32_t GP_LT_BIT = 0x10000; // Left Trigger
    static constexpr uint32_t GP_RT_BIT = 0x20000; // Right Trigger
    static constexpr int GP_TRIGGER_THRESH = 30;

    static void CheckboxApply(const char* label, bool& value, const char* section, const char* key) {
        if (ImGui::Checkbox(label, &value)) {
            Config::SetSetting(section, key, value ? "1" : "0");
            if (std::string(key) == "DetailedLogging") {
                detailedLogging.store(value);
                LogOut(std::string("[CONFIG/UI] detailedLogging set to ") + (value ? "true" : "false"), false);
            }
        }
    }

    static bool InputKeyHex(const char* label, int& keyCode, const char* setKeyName) {
        char buf[16] = {0};
        snprintf(buf, sizeof(buf), "0x%X", keyCode);
        ImGui::SetNextItemWidth(90);
        if (ImGui::InputText(label, buf, sizeof(buf))) {
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

    // Aggregate all XInput pads into a single logical mask with trigger pseudo-bits.
    static uint32_t PollAggregatedGamepadMask(uint32_t* padsConnectedMask = nullptr) {
        uint32_t agg = 0;
        uint32_t connected = 0;
        for (int i = 0; i < 4; ++i) {
            XINPUT_STATE st{};
            if (XInputGetState(i, &st) == ERROR_SUCCESS) {
                connected |= (1u << i);
                agg |= st.Gamepad.wButtons;
                if (st.Gamepad.bLeftTrigger > GP_TRIGGER_THRESH) agg |= GP_LT_BIT;
                if (st.Gamepad.bRightTrigger > GP_TRIGGER_THRESH) agg |= GP_RT_BIT;
            }
        }
        if (padsConnectedMask) *padsConnectedMask = connected;
        return agg;
    }

    // Specialized footer hotkey rebinder that blocks Enter/Escape/Space
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

        // Local copies for UI mutation
        bool useImGui = cfg.useImGui;
        bool logVerbose = cfg.detailedLogging;
        bool fpsDiag = cfg.enableFpsDiagnostics;
        bool restrictPractice = cfg.restrictToPracticeMode;
        bool enableConsole = cfg.enableConsole;
        float uiScale = cfg.uiScale;
        int uiFontMode = cfg.uiFontMode; // 0=Default, 1=Segoe UI

        if (ImGui::BeginTabBar("##SettingsTabs")) {
            if (ImGui::BeginTabItem("General")) {
                CheckboxApply("Use ImGui UI (else legacy dialog)", useImGui, "General", "UseImGui");
                ImGui::SameLine();
                ImGui::TextDisabled("(applies on next menu open)");

                CheckboxApply("Detailed logging", logVerbose, "General", "DetailedLogging");
                CheckboxApply("Enable FPS/timing diagnostics", fpsDiag, "General", "enableFpsDiagnostics");

                ImGui::Text("UI Scale:");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(140);
                if (ImGui::SliderFloat("##UiScale", &uiScale, 0.70f, 1.50f, "%.2f")) {
                    Config::SetSetting("General", "uiScale", std::to_string(uiScale));
                }
                ImGui::SameLine();
                if (ImGui::Button("Reset##UiScale")) {
                    uiScale = 0.90f;
                    Config::SetSetting("General", "uiScale", "0.90");
                }
                ImGui::SameLine();
                ImGui::TextDisabled("(applies immediately)");

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

                if (ImGui::Checkbox("Show debug console (restart not required)", &enableConsole)) {
                    Config::SetSetting("General", "enableConsole", enableConsole ? "1" : "0");
                    if (enableConsole) {
                        if (!GetConsoleWindow()) { CreateDebugConsole(); } else { SetConsoleVisibility(true); }
                        SetConsoleReady(true);
                        FlushPendingConsoleLogs();
                    } else {
                        SetConsoleVisibility(false);
                    }
                }

                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Keyboard Hotkeys")) {
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

                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Controller")) {
                ImGui::SeparatorText("Bindings");
                ImGui::TextWrapped("%s", "Rebind: activates capture, waits for release, then captures the next button/trigger pressed. Disable sets -1.");
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

                static bool capturing = false;
                static std::string which;
                static bool armed = false; // set after full release and small delay
                static uint32_t prevAggMask = 0;
                static double startTime = 0.0;

                const size_t rowCount = sizeof(rows) / sizeof(rows[0]);
                for (size_t i = 0; i < rowCount; ++i) {
                    const Row& r = rows[i];
                    ImGui::PushID(r.key);
                    ImGui::TextUnformatted(r.label);
                    ImGui::SameLine();
                    ImGui::TextDisabled("[%s]", Config::GetGamepadButtonName(r.val).c_str());
                    ImGui::SameLine();
                    std::string btn = std::string("Rebind##") + r.key;
                    if (!capturing) {
                        if (ImGui::SmallButton(btn.c_str())) {
                            capturing = true; which = r.key; armed = false; prevAggMask = 0; startTime = ImGui::GetTime();
                        }
                        ImGui::SameLine();
                        if (ImGui::SmallButton("Disable")) {
                            Config::SetSetting("Hotkeys", r.key, "-1");
                        }
                    } else if (capturing && which == r.key) {
                        ImGui::TextColored(ImVec4(1,1,0,1), armed ? "Waiting for next input..." : "Release all buttons/triggers...");
                        ImGui::SameLine();
                        if (ImGui::SmallButton("Cancel")) { capturing = false; which.clear(); armed=false; }

                        uint32_t padsMask = 0;
                        uint32_t agg = PollAggregatedGamepadMask(&padsMask);

                        const double elapsed = ImGui::GetTime() - startTime;
                        if (!armed) {
                            if (elapsed > 0.12 && agg == 0) { // 120ms debounce and full release
                                armed = true;
                                prevAggMask = 0;
                            }
                        } else {
                            uint32_t edge = agg & ~prevAggMask;
                            if (edge != 0) {
                                std::string name = Config::GetGamepadButtonName((int)edge);
                                Config::SetSetting("Hotkeys", which, name);
                                capturing = false; which.clear(); armed = false; prevAggMask = 0; startTime = 0.0;
                            } else {
                                prevAggMask = agg;
                            }
                        }
                    }
                    ImGui::PopID();
                }

                ImGui::Separator();
                if (ImGui::Button("Save Controller Binds")) { Config::SaveSettings(); LogOut("[CONFIG/UI] Controller binds saved", false); }
                ImGui::SameLine(); if (ImGui::Button("Reload")) { Config::LoadSettings(); LogOut("[CONFIG/UI] Controller binds reloaded", false); }

                ImGui::Dummy(ImVec2(1,6));
                ImGui::SeparatorText("Device Selection");
                {
                    int idx = Config::GetSettings().controllerIndex;
                    int current = idx; // -1 = All
                    std::string labelAll = "All (Any)";
                    std::string lbl0 = ::GetControllerNameForIndex(0);
                    std::string lbl1 = ::GetControllerNameForIndex(1);
                    std::string lbl2 = ::GetControllerNameForIndex(2);
                    std::string lbl3 = ::GetControllerNameForIndex(3);
                    const char* items[] = { labelAll.c_str(), lbl0.c_str(), lbl1.c_str(), lbl2.c_str(), lbl3.c_str() };
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

                ImGui::EndTabItem();
            }

            ImGui::EndTabBar();
        }

        ImGui::TextDisabled("Config file: %s", Config::GetConfigFilePath().c_str());
    }
}
