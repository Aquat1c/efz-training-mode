#include "../include/gui/pause_menu.h"
#include "../include/core/logger.h"
#include "../include/utils/config.h"
#include "../include/gui/imgui_gui.h" // reuse full tab renderers & Refresh/Apply
#include "../include/gui/imgui_impl.h"
#include "../include/utils/pause_integration.h" // pause/unpause integration
#include "../include/game/game_state.h"
#include "../include/game/always_rg.h" // AlwaysRG::IsEnabled
#include "../include/game/macro_controller.h"
#include "../include/gui/imgui_settings.h"
#include "../include/utils/config.h"
#include "../include/gui/overlay.h" // DirectDrawHook for toasts
#include <windows.h>
#include <vector>
#include <string>
#include <imgui.h>

namespace PauseMenu {
    namespace {
    std::atomic<bool> g_visible{false};
    bool g_initialized = false;
        int g_category = 0; // index into categories vector
        bool g_wasF1Down = false; // edge detect F1 so we don't conflict with other F1 logic
        bool g_edgeQ = false, g_edgeE = false; // per-frame edges
        uint64_t g_lastToggleTick = 0; // debounce

        struct Category { const char* name; void (*renderFn)(); };

        // Wrapper functions calling full tab renderers from main GUI implementation
        void RenderGameFull()       { ImGuiGui::RenderGameValuesTab(); }
        void RenderAutoFull()       { ImGuiGui::RenderAutoActionTab(); }
        void RenderCharacterFull()  { ImGuiGui::RenderCharacterTab(); }
        void RenderSettingsFull()   { ImGuiSettings::RenderSettingsTab(); }
        void RenderDebugFull()      { ImGuiGui::RenderDebugInputTab(); }
        void RenderHelpFull()       { ImGuiGui::RenderHelpTab(); }

        // Macros tab replica (extracted minimal code from main GUI to avoid editing it)
        void RenderMacrosFull() {
            const auto& cfg = Config::GetSettings();
            ImGui::SeparatorText("Macro Controller");
            ImGui::Text("State: %s", MacroController::GetStatusLine().c_str());
            ImGui::Text("Current Slot: %d / %d", MacroController::GetCurrentSlot(), MacroController::GetSlotCount());
            bool empty = MacroController::IsSlotEmpty(MacroController::GetCurrentSlot());
            ImGui::Text("Slot Empty: %s", empty ? "Yes" : "No");
            auto stats = MacroController::GetSlotStats(MacroController::GetCurrentSlot());
            ImGui::SeparatorText("Slot Stats");
            ImGui::BulletText("Spans: %d", stats.spanCount);
            ImGui::BulletText("Total Ticks: %d (~%.2fs)", stats.totalTicks, stats.totalTicks / 64.0f);
            ImGui::BulletText("Buffer Entries: %d", stats.bufEntries);
            ImGui::BulletText("Buf Idx Start: %u", (unsigned)stats.bufStartIdx);
            ImGui::BulletText("Buf Idx End: %u", (unsigned)stats.bufEndIdx);
            ImGui::BulletText("Has Data: %s", stats.hasData ? "Yes" : "No");
            if (ImGui::Button("Toggle Record")) { MacroController::ToggleRecord(); }
            ImGui::SameLine(); if (ImGui::Button("Play")) { MacroController::Play(); }
            ImGui::SameLine(); if (ImGui::Button("Stop")) { MacroController::Stop(); }
            if (ImGui::Button("Prev Slot")) { MacroController::PrevSlot(); }
            ImGui::SameLine(); if (ImGui::Button("Next Slot")) { MacroController::NextSlot(); }
            ImGui::Spacing(); ImGui::SeparatorText("Hotkeys");
            ImGui::BulletText("Record: %s", GetKeyName(cfg.macroRecordKey).c_str());
            ImGui::BulletText("Play: %s", GetKeyName(cfg.macroPlayKey).c_str());
            ImGui::BulletText("Next Slot: %s", GetKeyName(cfg.macroSlotKey).c_str());
        }

        std::vector<Category> g_categories = {
            {"Game",     RenderGameFull},
            {"Auto",     RenderAutoFull},
            {"Char",     RenderCharacterFull},
            {"Macros",   RenderMacrosFull},
            {"Settings", RenderSettingsFull},
            {"Debug",    RenderDebugFull},
            {"Help",     RenderHelpFull}
        };

        bool IsKeyJustPressed(int vk) {
            SHORT s = GetAsyncKeyState(vk);
            static SHORT prev[256] = {0};
            bool pressed = (s & 0x8000) && !(prev[vk] & 0x8000);
            prev[vk] = s;
            return pressed;
        }
    }

    void EnsureInitialized() {
        if (g_initialized) return;
        LogOut("[PAUSE_MENU] Initialized", true);
        // Initial snapshot for summaries (may be slightly stale by first open if game state changes rapidly; acceptable)
        ImGuiGui::RefreshLocalData();
        g_initialized = true;
    }

    bool IsVisible() { return g_visible.load(); }

    void Toggle() {
        bool now = !g_visible.load();
    if (now && !g_initialized) EnsureInitialized();
        g_visible.store(now);
        g_lastToggleTick = GetTickCount64();
        // Pause/unpause integration
        PauseIntegration::OnMenuVisibilityChanged(now);
        if (now) {
            LogOut("[PAUSE_MENU] Opened", true);
            ImGuiGui::RefreshLocalData();
        } else {
            LogOut("[PAUSE_MENU] Closed", true);
        }
    }

    void Close() { if (g_visible.load()) Toggle(); }

    void TickInput() {
    // Make sure we are ready before processing inputs (even if never opened yet) so first F1 press is cheap
    EnsureInitialized();
    if (!g_visible.load()) {
            return; // Global thread handles F1 opening
        }
        // When visible, capture Q/E to change category.
        if (IsKeyJustPressed('Q')) {
            g_category = (g_category - 1 + (int)g_categories.size()) % (int)g_categories.size();
        }
        if (IsKeyJustPressed('E')) {
            g_category = (g_category + 1) % (int)g_categories.size();
        }
        // Escape closes
        if (IsKeyJustPressed(VK_ESCAPE)) {
            Close(); // will unpause if we owned the pause
        }
    }

    // (moved summary render functions into anonymous namespace above for correct internal linkage)

    void Render() {
    EnsureInitialized();
        if (!g_visible.load()) return;
        ImGuiIO& io = ImGui::GetIO();
        // Dimmed background layer (simple dark veil)
        ImGui::SetNextWindowPos(ImVec2(0,0), ImGuiCond_Always);
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGuiWindowFlags dimFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoMove |
                                    ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBackground;
        if (ImGui::Begin("##PauseDim", nullptr, dimFlags)) {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(ImVec2(0,0), io.DisplaySize, IM_COL32(0,0,0,140));
        }
        ImGui::End();
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f,0.5f));
        ImGui::SetNextWindowSize(ImVec2(780, 520), ImGuiCond_Once);
        ImGui::SetNextWindowBgAlpha(0.94f);
        ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse;
        if (ImGui::Begin("Pause Menu (Full Training UI)", nullptr, flags)) {
            // Top category bar
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6,4));
            for (int i=0;i<(int)g_categories.size();++i) {
                if (i>0) ImGui::SameLine();
                bool active = (i==g_category);
                if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.25f,0.55f,0.95f,0.85f));
                if (ImGui::Button(g_categories[i].name, ImVec2(80,0))) g_category=i;
                if (active) ImGui::PopStyleColor();
            }
            ImGui::PopStyleVar();
            ImGui::Separator();

            // Scrollable content region
            ImVec2 avail = ImGui::GetContentRegionAvail();
            float footerH = ImGui::GetFrameHeightWithSpacing()*1.3f + ImGui::GetStyle().ItemSpacing.y*2;
            if (footerH < 48.f) footerH = 48.f;
            if (ImGui::BeginChild("##PMContent", ImVec2(avail.x, avail.y - footerH), true)) {
                if (g_category >=0 && g_category < (int)g_categories.size()) {
                    g_categories[g_category].renderFn();
                }
            }
            ImGui::EndChild();

            // Footer with Apply / Refresh / Close
            ImGui::Separator();
            const auto& cfg = Config::GetSettings();
            // Hotkey edge detection (local static)
            static SHORT lastAccept=0,lastRefresh=0,lastExit=0;
            SHORT curAccept = GetAsyncKeyState(cfg.uiAcceptKey);
            SHORT curRefresh = GetAsyncKeyState(cfg.uiRefreshKey);
            SHORT curExit = GetAsyncKeyState(cfg.uiExitKey);
            bool doApply = (curAccept & 0x8000) && !(lastAccept & 0x8000);
            bool doRefresh = (curRefresh & 0x8000) && !(lastRefresh & 0x8000);
            bool doExit = (curExit & 0x8000) && !(lastExit & 0x8000);
            lastAccept = curAccept; lastRefresh = curRefresh; lastExit = curExit;

            std::string applyLabel = std::string("Apply (") + GetKeyName(cfg.uiAcceptKey) + ")";
            std::string refreshLabel = std::string("Refresh (") + GetKeyName(cfg.uiRefreshKey) + ")";
            std::string exitLabel = std::string("Close (") + GetKeyName(cfg.uiExitKey) + ")";
            if (ImGui::Button(applyLabel.c_str(), ImVec2(140,0)) || doApply) {
                ImGuiGui::ApplyImGuiSettings();
                if constexpr (true) { DirectDrawHook::AddMessage("Applied", "PAUSE", RGB(180,255,180), 1000, 0, 60); }
            }
            ImGui::SameLine();
            if (ImGui::Button(refreshLabel.c_str(), ImVec2(140,0)) || doRefresh) {
                ImGuiGui::RefreshLocalData();
                if constexpr (true) { DirectDrawHook::AddMessage("Refreshed", "PAUSE", RGB(180,220,255), 1000, 0, 60); }
            }
            ImGui::SameLine();
            if (ImGui::Button(exitLabel.c_str(), ImVec2(140,0)) || doExit) {
                Close();
            }
            ImGui::SameLine();
            bool paused = PauseIntegration::IsPausedOrFrozen();
            ImGui::TextDisabled("F1 toggles | Q/E cycle categories | Paused=%s", paused?"Yes":"No");
        }
        ImGui::End();
    }
}
