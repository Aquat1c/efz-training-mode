#pragma once

namespace CustomMenu {

// Called once per EndScene frame from HookedEndScene (gated by
// Config::useCustomMenu). Queries ImGuiImpl::IsVisible() internally — only
// draws when the menu is open.
//
// Uses ImGui::GetBackgroundDrawList() on the 640x480 render target; does not
// call ImGui::Begin/End. Fonts are loaded/rebuilt on demand; safe to call
// every frame.
void Render();

} // namespace CustomMenu
