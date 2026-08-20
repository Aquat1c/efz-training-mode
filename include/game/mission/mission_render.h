#pragma once
#include <string>
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
struct ImFont;

namespace Mission::Render {

struct RichTextMetrics {
    float width = 0.0f;
    float height = 0.0f;
};

// Draw the recipe for the active mission (no-op if none). `device` is the live
// IDirect3DDevice9* (as void*); ox/oy/scale map the 640x480 virtual space into
// the current render target's inner 4:3 area.
// WantsDraw is a conservative, lock-free routing hint used by the top-level
// overlay so an active trial remains a render reason even after its legacy
// status toast is removed. Draw performs the coherent visibility/snapshot gate.
bool WantsDraw();
void Draw(void* device, ImDrawList* dl, float ox, float oy, float scale);

// Shared tutorial rich-text renderer. It resolves {dir:}, {btn:}, and
// {input:} tokens through assets/controls, plus cached semantic spans such as
// {tone:life|Life Gauge}. It includes a styled neutral-5 tile and wraps
// atomically so notation or a colored phrase is never split internally.
float MeasureRichTextHeight(void* device, ImFont* font, float fontPx,
                            const std::string& text, float maxWidth);
// Returns the widest used line as well as total wrapped height. The result is
// produced by the same bounded cached layout used by DrawRichText, allowing
// compact HUD strips to follow their content without re-tokenizing each frame.
RichTextMetrics MeasureRichText(void* device, ImFont* font, float fontPx,
                                const std::string& text, float maxWidth);
void DrawRichText(void* device, ImDrawList* dl, ImFont* font, float fontPx,
                  float x, float y, unsigned int color,
                  const std::string& text, float maxWidth);

// Font-atlas rebuilds invalidate cached ImFont/layout associations without
// requiring the separate control-icon texture to be decoded again.
void InvalidateTextLayouts();

// Release icon textures (device loss / shutdown).
void ReleaseTextures();

} // namespace Mission::Render
