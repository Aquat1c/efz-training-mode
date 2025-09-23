#include "../include/gui/imgui_settings.h"
#include "../include/utils/config.h"
#include "../include/core/logger.h"
#include "../include/utils/switch_players.h"
#include "../include/game/game_state.h"
#include <windows.h>

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

    void RenderSettingsTab() {
        const Config::Settings& cfg = Config::GetSettings();

        // We copy values locally so we can mutate with ImGui widgets
    bool useImGui = cfg.useImGui;
    bool logVerbose = cfg.detailedLogging;
    bool fpsDiag = cfg.enableFpsDiagnostics;
    bool restrictPractice = cfg.restrictToPracticeMode;
    bool enableConsole = cfg.enableConsole;
    float uiScale = cfg.uiScale;

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
        }

        ImGui::Separator();
        ImGui::TextDisabled("Config file: %s", Config::GetConfigFilePath().c_str());

        // Switch Players control moved to Debug tab
    }
}
