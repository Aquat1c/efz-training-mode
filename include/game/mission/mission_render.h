#pragma once
//
// Mission recipe renderer (M3 scaffold).
//
// Draws the on-screen combo recipe while a mission is running: a horizontal row
// of step boxes near the bottom of the inner 4:3 game area, each box showing the
// step's notation as control icons (assets/controls: 1-9 numpad dirs + A/B/C/D/S
// buttons) and colored by status (done / current / next / failed).
//
// Rendered from DirectDrawHook::RenderD3D9Overlays via ImGui's background draw
// list, using the same letterbox-safe ox/oy/scale mapping as the framebar. Icon
// textures are loaded lazily from the passed D3D9 device (GDI+ decode, like
// gif_player.cpp).

struct ImDrawList;

namespace Mission::Render {

// Draw the recipe for the active mission (no-op if none). `device` is the live
// IDirect3DDevice9* (as void*); ox/oy/scale map the 640x480 virtual space into
// the current render target's inner 4:3 area.
void Draw(void* device, ImDrawList* dl, float ox, float oy, float scale);

// Release icon textures (device loss / shutdown).
void ReleaseTextures();

} // namespace Mission::Render
