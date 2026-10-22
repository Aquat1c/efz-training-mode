#pragma once
#include <string>
// Draws the on-screen combo recipe as a horizontal row of step boxes near the
// bottom of the inner 4:3 area, notation rendered as control icons from
// assets/controls (1-9 numpad dirs + A/B/C/D/S buttons), colored by status.
// Called from DirectDrawHook::RenderD3D9Overlays via ImGui's background draw
// list, using the same letterbox-safe ox/oy/scale mapping as the framebar.

struct ImDrawList;
struct ImFont;

namespace Mission::Render {

struct RichTextMetrics {
    float width = 0.0f;
    float height = 0.0f;
};

// `device` is the live IDirect3DDevice9* (as void*); ox/oy/scale map the
// 640x480 virtual space into the render target's inner 4:3 area.
// WantsDraw is a lock-free render-reason hint; Draw performs the coherent gate.
bool WantsDraw();
void Draw(void* device, ImDrawList* dl, float ox, float oy, float scale);

// Shared rich-text renderer: resolves {dir:}, {btn:}, {input:} tokens through
// assets/controls plus cached semantic spans like {tone:life|Life Gauge};
// wraps atomically so a notation or colored phrase is never split.
float MeasureRichTextHeight(void* device, ImFont* font, float fontPx,
                            const std::string& text, float maxWidth);
// Returns widest used line and total wrapped height from the same cached
// layout as DrawRichText.
RichTextMetrics MeasureRichText(void* device, ImFont* font, float fontPx,
                                const std::string& text, float maxWidth);
void DrawRichText(void* device, ImDrawList* dl, ImFont* font, float fontPx,
                  float x, float y, unsigned int color,
                  const std::string& text, float maxWidth);

// Font-atlas rebuilds invalidate cached ImFont/layout associations only.
void InvalidateTextLayouts();

// Release icon textures (device loss / shutdown).
void ReleaseTextures();

} // namespace Mission::Render
