#include "../include/gui/custom_menu/fonts.h"
#include "../include/gui/custom_menu/resource_ids.h"
#include "../include/gui/custom_menu/scale.h"
#include "../include/core/logger.h"
#include "../include/utils/config.h"
#include "../3rdparty/imgui/imgui.h"
#include "../3rdparty/imgui/backends/imgui_impl_dx9.h"

#include <windows.h>
#include <chrono>
#include <cmath>
#include <cstring>

extern HMODULE g_hSelfModule;

namespace CustomMenu::Fonts {

namespace {

ImFont* g_bodyFont   = nullptr;
ImFont* g_headerFont = nullptr;

float g_appliedScale = 0.0f;

std::chrono::steady_clock::time_point g_lastRebuild{};

struct ResourceBlob {
    const void* data = nullptr;
    DWORD size = 0;
};

ResourceBlob LoadEmbeddedFont() {
    ResourceBlob blob;
    if (!g_hSelfModule) return blob;

    HRSRC hRes = FindResourceA(
        g_hSelfModule,
        MAKEINTRESOURCEA(IDR_ITC_BOLT_BOLD_REGULAR),
        RT_RCDATA);
    if (!hRes) return blob;

    HGLOBAL hGlobal = LoadResource(g_hSelfModule, hRes);
    if (!hGlobal) return blob;

    blob.data = LockResource(hGlobal);
    blob.size = SizeofResource(g_hSelfModule, hRes);
    return blob;
}

bool AtlasContainsFont(const ImFontAtlas* atlas, const ImFont* font) {
    if (!atlas || !font) return false;
    for (int i = 0; i < atlas->Fonts.Size; ++i) {
        if (atlas->Fonts[i] == font) return true;
    }
    return false;
}

} // namespace

bool Rebuild(float uiScale) {
    if (!ImGui::GetCurrentContext()) return false;

    ImGuiIO& io = ImGui::GetIO();

    // Throttle rebuilds: at most once per 500ms to absorb scale oscillations.
    auto now = std::chrono::steady_clock::now();
    const bool everRebuilt = (g_lastRebuild.time_since_epoch().count() != 0);

    Scale::Update(uiScale);
    const Scale::Metrics& metrics = Scale::Get();
    // Round to nearest hundredth so tiny scale drift doesn't trigger rebuilds.
    float rounded = std::floor(metrics.fontScale * 100.0f + 0.5f) / 100.0f;

    const bool scaleChanged = !(std::fabs(rounded - g_appliedScale) < 0.005f);
    const bool fontsMissing =
        !AtlasContainsFont(io.Fonts, g_bodyFont) ||
        !AtlasContainsFont(io.Fonts, g_headerFont) ||
        io.FontDefault == nullptr;
    if (!scaleChanged && !fontsMissing) return true;

    if (!fontsMissing && everRebuilt &&
        (now - g_lastRebuild) < std::chrono::milliseconds(500)) {
        return true; // defer; will pick up on a later call
    }

    ResourceBlob blob = LoadEmbeddedFont();
    if (!blob.data || blob.size == 0) {
        LogOut("[CUSTOM_MENU][FONT] Failed to locate embedded ITC Bolt OTF resource.", true);
        return false;
    }

    // ImGui wants ownership of the blob so it can pass it to stb_truetype
    // during atlas build. Allocate a fresh copy per size because
    // AddFontFromMemoryTTF(FontDataOwnedByAtlas=true) frees it on Clear().
    auto copyBlob = [&]() -> void* {
        void* copy = IM_ALLOC(blob.size);
        if (copy) memcpy(copy, blob.data, blob.size);
        return copy;
    };

    ImGui_ImplDX9_InvalidateDeviceObjects();
    io.Fonts->Clear();
    io.Fonts->Flags |= ImFontAtlasFlags_NoPowerOfTwoHeight;
    io.Fonts->TexGlyphPadding = 1;

    // Rebuild the normal UI default font as well so the custom menu doesn't
    // leak its typeface into the rest of the shared ImGui context.
    ImFont* mainFont = nullptr;
    {
        ImFontConfig cfgMain;
        cfgMain.OversampleH = 3;
        cfgMain.OversampleV = 3;
        cfgMain.PixelSnapH  = false;

        const float mainPx = Scale::Snap(13.0f * rounded);
        const int fontMode = Config::GetSettings().uiFontMode;
        if (fontMode == 1) {
            const char* segoePath = "C:\\Windows\\Fonts\\segoeui.ttf";
            DWORD fa = GetFileAttributesA(segoePath);
            if (fa != INVALID_FILE_ATTRIBUTES && !(fa & FILE_ATTRIBUTE_DIRECTORY)) {
                mainFont = io.Fonts->AddFontFromFileTTF(segoePath, mainPx, &cfgMain);
            }
        }
        if (!mainFont) {
            cfgMain.SizePixels = mainPx;
            mainFont = io.Fonts->AddFontDefault(&cfgMain);
        }
    }

    ImFontConfig cfgBase;
    cfgBase.OversampleH = 1;   // pixel-snapped bitmap feel
    cfgBase.OversampleV = 1;
    cfgBase.PixelSnapH  = true;
    cfgBase.FontDataOwnedByAtlas = true;

    // Load body first so it's the default font.
    const float bodyPx   = metrics.bodyPx;
    const float headerPx = metrics.headerPx;

    ImFontConfig cfgBody = cfgBase;
    cfgBody.SizePixels = bodyPx;
    void* bodyBytes = copyBlob();
    g_bodyFont = bodyBytes
        ? io.Fonts->AddFontFromMemoryTTF(bodyBytes, (int)blob.size, bodyPx, &cfgBody)
        : nullptr;

    ImFontConfig cfgHeader = cfgBase;
    cfgHeader.SizePixels = headerPx;
    void* headerBytes = copyBlob();
    g_headerFont = headerBytes
        ? io.Fonts->AddFontFromMemoryTTF(headerBytes, (int)blob.size, headerPx, &cfgHeader)
        : nullptr;

    // ImGui also needs a default font that covers the rest of the UI context
    // for any code path that doesn't PushFont.
    io.FontDefault = mainFont ? mainFont : g_bodyFont;

    if (!ImGui_ImplDX9_CreateDeviceObjects()) {
        LogOut("[CUSTOM_MENU][FONT] Failed to recreate DX9 device objects after font rebuild.", true);
        g_bodyFont = nullptr;
        g_headerFont = nullptr;
        return false;
    }

    g_appliedScale = rounded;
    g_lastRebuild = now;

    char msg[160];
    _snprintf_s(msg, sizeof(msg), _TRUNCATE,
        "[CUSTOM_MENU][FONT] Loaded ITC Bolt Bold: body=%.0fpx header=%.0fpx scale=%.2f",
        bodyPx, headerPx, rounded);
    LogOut(msg, true);

    return (g_bodyFont != nullptr && g_headerFont != nullptr);
}

ImFont* Body()     { return g_bodyFont; }
ImFont* Header()   { return g_headerFont; }
bool    IsLoaded() {
    if (!ImGui::GetCurrentContext()) return false;
    ImGuiIO& io = ImGui::GetIO();
    return AtlasContainsFont(io.Fonts, g_bodyFont) && AtlasContainsFont(io.Fonts, g_headerFont);
}

} // namespace CustomMenu::Fonts
