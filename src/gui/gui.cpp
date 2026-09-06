#include "../include/gui/gui.h"
#include "../include/utils/utilities.h"

#include "../include/core/logger.h"
#include "../include/utils/config.h"
#include "../include/gui/imgui_impl.h"
#include "../include/gui/overlay.h"
#include "../include/game/game_state.h"
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

bool OpenPracticeMenuDirect(bool allowOutOfContext) {
    UpdateWindowActiveState();
    if (!g_efzWindowActive.load()) {
        LogOut("[GUI] EFZ window not active, cannot open Practice menu", true);
        return false;
    }

    // Don't open menu if it's already open.
    // This MUST stay above the context gate: an already-open menu is never
    // affected by it, so a user can never be trapped behind a menu they cannot
    // close after the game leaves a gameplay screen.
    if (menuOpen) {
        LogOut("[GUI] Menu already open", detailedLogging.load());
        return ImGuiImpl::IsVisible();
    }

    // Feature liveness is a player-pointer test, not a phase test, and efz.exe's
    // character-select screen leaves that pointer populated from the first
    // character confirm onward. Without this gate the menu opens over character
    // select, the loading screen, and the title screen after a cancelled CS -
    // where nothing suppresses EFZ's own input, so menu navigation also drives
    // the CS cursor, and where the pause/patch transaction runs against a
    // context that is not a live match.
    if (!allowOutOfContext && !IsTrainingMenuContext()) {
        // Throttle: the menu key may be held down.
        static DWORD s_lastBlockLog = 0;
        const DWORD now = GetTickCount();
        if (now - s_lastBlockLog > 1000) {
            s_lastBlockLog = now;
            LogOut("[GUI] Menu open refused: not in a gameplay context", true);
        }
        return false;
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

