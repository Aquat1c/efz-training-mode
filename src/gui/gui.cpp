#include "../include/gui/gui.h"
#include "../include/utils/utilities.h"

#include "../include/core/logger.h"
#include "../include/utils/config.h"
#include "../include/gui/imgui_impl.h"
#include "../include/gui/overlay.h"
#include "../include/game/mission/mission_engine.h"
#include "../include/game/mission/mission_pause_menu.h"

void OpenMenu() {
    // Check if we're in EFZ window
    UpdateWindowActiveState();
    if (!g_efzWindowActive.load()) {
        LogOut("[GUI] EFZ window not active, cannot open menu", true);
        return;
    }

    if (Mission::Engine::Recorder::IsMenuInputHandoffActive() &&
        !Mission::PauseMenu::IsOpen()) {
        // A phase-changing recorder command is still frozen behind its
        // neutral-input handoff. Do not replace that transaction with either
        // Practice or another contextual surface.
        LogOut("[GUI] Recording menu handoff active; menu press deferred", true);
        return;
    }

    // Every live mission-authoring phase owns the dedicated Recording menu;
    // setup and review must not silently fall through to Practice. Practice
    // Settings remains available as an explicit nested row in that menu.
    if (Mission::Engine::Recorder::IsSessionActive() ||
        Mission::Engine::Runner::IsActive()) {
        Mission::PauseMenu::Toggle();
        return;
    }

    (void)OpenPracticeMenuDirect();
}

bool OpenPracticeMenuDirect() {
    UpdateWindowActiveState();
    if (!g_efzWindowActive.load()) {
        LogOut("[GUI] EFZ window not active, cannot open Practice menu", true);
        return false;
    }

    // Don't open menu if it's already open
    if (menuOpen) {
        LogOut("[GUI] Menu already open", detailedLogging.load());
        return ImGuiImpl::IsVisible();
    }

    LogOut("[GUI] Opening config menu", detailedLogging.load());

    const bool d3d9Ready = DirectDrawHook::SetD3D9Active(true);
    const bool useFallbackWindow = !d3d9Ready || DirectDrawHook::ShouldUseExternalMenuFallback();
    if (useFallbackWindow) {
        LogOut("[GUI] In-game render unavailable; opening fallback ImGui window", true);
        if (!ImGuiImpl::ShowFallbackWindow()) {
            LogOut("[GUI] Failed to open fallback ImGui window", true);
            menuOpen = false;
            return false;
        }
        menuOpen = true;
        return true;
    }

    LogOut("[GUI] Using in-game ImGui visibility toggle", detailedLogging.load());
    ImGuiImpl::ToggleVisibility();
    menuOpen = ImGuiImpl::IsVisible();
    return menuOpen.load();
}

