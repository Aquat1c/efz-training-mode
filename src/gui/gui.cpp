#include "../include/gui/gui.h"
#include "../include/utils/utilities.h"

#include "../include/core/logger.h"
#include "../include/utils/config.h"
#include "../include/gui/imgui_impl.h"  
#include "../include/gui/overlay.h"

void OpenMenu() {
    // Check if we're in EFZ window
    UpdateWindowActiveState();
    if (!g_efzWindowActive.load()) {
        LogOut("[GUI] EFZ window not active, cannot open menu", true);
        return;
    }

    // Don't open menu if it's already open
    if (menuOpen) {
        LogOut("[GUI] Menu already open", detailedLogging.load());
        return;
    }

    LogOut("[GUI] Opening config menu", detailedLogging.load());

    const bool d3d9Ready = DirectDrawHook::SetD3D9Active(true);
    const bool useFallbackWindow = !d3d9Ready || DirectDrawHook::ShouldUseExternalMenuFallback();
    if (useFallbackWindow) {
        LogOut("[GUI] In-game render unavailable; opening fallback ImGui window", true);
        if (!ImGuiImpl::ShowFallbackWindow()) {
            LogOut("[GUI] Failed to open fallback ImGui window", true);
            menuOpen = false;
            return;
        }
        menuOpen = true;
        return;
    }

    LogOut("[GUI] Using in-game ImGui visibility toggle", detailedLogging.load());
    ImGuiImpl::ToggleVisibility();
    menuOpen = ImGuiImpl::IsVisible();
}

