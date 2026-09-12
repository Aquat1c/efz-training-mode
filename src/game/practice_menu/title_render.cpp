#include "../../../include/game/practice_menu/title_render.h"
#include "../../../include/game/practice_menu/efz_title.h"

#include "../../../include/core/logger.h"

#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <string>

// Native rendering of the Practice submenu. We load our own title_ob2.dat into a
// PRIVATE surface (never the title's own +1080 surface) so we never fight
// InGameNetplay over title_ob.dat, and blit rows using the game's blit function
// exactly as the front-end does.
//
// NOTE: the row/dest coordinates below are first-pass estimates in the game's
// native 320x240 space; they are meant to be tuned against title_ob2.png once
// visible in-game. The load/blit *pipeline* is the milestone; pixel-exact
// placement is expected to iterate.

using namespace PracticeMenu::EfzTitle;

namespace {

// ---- sheet layout (measured 1:1 from title_ob2.dat, 120x95, top-down) ------
// The sheet has two lanes of 3 rows each at a 14px pitch. Only the first 8px
// of each mapped row belong to the visible sprite. The remaining cell padding
// contains near-white, non-keyed separator pixels, and the last scanline of a
// full 14px crop reaches the following selected rail. Blit the visible height,
// but retain the authored pitch and on-screen spacing:
//   Unselected lane: src Y = 4, 18, 32   (kUnselBaseY + row*kRowPitch)
//   Selected   lane: src Y = 49, 63, 77  (kSelBaseY   + row*kRowPitch)
constexpr int kSheetW     = 120; // full sprite width
constexpr int kRowH       = 8;   // visible sprite height; do not copy cell padding
constexpr int kRowPitch   = 14;  // stride between rows within a lane
constexpr int kUnselBaseY = 4;   // top of the first unselected row (PRACTICE)
constexpr int kSelBaseY   = 49;  // top of the first selected row (PRACTICE + teal bar)

// ---- on-screen placement (native 320x240) ----------------------------------
// The vanilla title menu draws at screen dest (195,130) with a 14px row pitch
// (the blit is dest-first; the decompiler mislabeled src/dest). We line our rows
// up with that so the submenu sits exactly where the main menu was.
constexpr int kDestX    = 195; // menu left edge on screen
constexpr int kDestY    = 130; // top of the first row (matches vanilla menu top)
constexpr int kDestStep = 14;  // dest Y stride between rows (vanilla menu pitch)

// Palette layout mirrors title_ob.dat's overlay range so the game's index
// offsetting (colorOffset2 = kPaletteBase) lands our pixels on our loaded
// colors. The actual color count is read from the .dat header (byte 0) - loading
// too few leaves the top indices pointing at garbage slots (glyph AA speckle).
constexpr int kPaletteBase        = 193;
constexpr int kPaletteCountFallback = 56;  // title_ob2.dat currently has 56 colors

uint32_t g_surface = 0;      // our private title_ob2 object surface
bool     g_loaded  = false;
bool     g_loadFailed = false;
char     g_transparent = 0;
std::string g_datPath;

void Log(const char* m) { LogOut(std::string("[PRACTICE_MENU][render] ") + m, true); }

// EFZ .dat byte 0 = number of palette colors. Load exactly that many so every
// pixel index has a valid color (clamped so base+count stays within 256).
int ReadDatPaletteCount(const char* path) {
    FILE* f = nullptr;
    if (fopen_s(&f, path, "rb") != 0 || f == nullptr) return 0;
    const int b0 = fgetc(f);
    fclose(f);
    if (b0 <= 0 || b0 > 255) return 0;
    int count = b0;
    if (count > 255 - kPaletteBase) count = 255 - kPaletteBase;
    return count;
}

// Resolve <this-dll-dir>\assets\title_ob2.dat (falling back to <dir>\title_ob2.dat).
bool ResolveDatPath(std::string& out) {
    HMODULE mod = GetModuleHandleA("efz_training_mode.dll");
    char path[MAX_PATH] = {0};
    if (!mod || GetModuleFileNameA(mod, path, MAX_PATH) == 0) return false;
    std::string dir(path);
    const size_t slash = dir.find_last_of("\\/");
    if (slash == std::string::npos) return false;
    dir.resize(slash + 1);

    const std::string candidates[] = { dir + "assets\\title_ob2.dat", dir + "title_ob2.dat" };
    for (const std::string& c : candidates) {
        if (GetFileAttributesA(c.c_str()) != INVALID_FILE_ATTRIBUTES) { out = c; return true; }
    }
    return false;
}

// Reload the vanilla title palettes so the title menu looks correct after we
// leave the submenu (we overwrote the 193.. range with our sheet's colors).
void RestoreTitlePalette(uint32_t sc) {
    const uintptr_t loadPalVa = Resolve(kVaLoadPalette);
    const uintptr_t setPalVa  = Resolve(kVaSetPalette);
    if (!loadPalVa || !setPalVa || !sc) return;
    __try {
        auto loadPal = reinterpret_cast<LoadPaletteFn>(loadPalVa);
        auto setPal  = reinterpret_cast<SetPaletteFn>(setPalVa);
        loadPal(static_cast<int>(sc + kOffPalette), "system\\title.dat", 0, 1, 192);
        loadPal(static_cast<int>(sc + kOffPalette), "system\\title_ob.dat", 0, 193, 48);
        setPal(reinterpret_cast<void*>(GraphicsSystem(sc)), static_cast<int>(sc + kOffPalette));
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

} // namespace

namespace PracticeMenu::Render {

bool EnsureLoaded(uint32_t sc) {
    if (g_loaded) return true;
    if (g_loadFailed || !sc) return false;

    if (g_datPath.empty() && !ResolveDatPath(g_datPath)) {
        Log("title_ob2.dat not found next to the DLL; rendering disabled");
        g_loadFailed = true;
        return false;
    }

    const uintptr_t loadImgVa = Resolve(kVaLoadImage);
    const uintptr_t readPixVa = Resolve(kVaReadPixel);
    if (!loadImgVa || !readPixVa) { g_loadFailed = true; return false; }

    __try {
        auto loadImg = reinterpret_cast<LoadImageFn>(loadImgVa);
        auto readPix = reinterpret_cast<ReadPixelFn>(readPixVa);

        g_surface = 0;
        loadImg(reinterpret_cast<void*>(GameContext(sc)), &g_surface, g_datPath.c_str(),
                0, static_cast<unsigned char>(kPaletteBase));
        if (g_surface == 0) { Log("loadCompressedImageFile returned null surface"); g_loadFailed = true; return false; }

        // Transparent index is a pixel value (palette-independent), safe to read now.
        g_transparent = readPix(static_cast<int>(g_surface));

        g_loaded = true;
        Log("Loaded title_ob2.dat into private surface");
        // Palette is applied separately (ApplyPalette) because leaving the
        // submenu restores the vanilla title palette - so it must be re-applied
        // on EVERY entry, not just the one time the surface is loaded.
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("Exception while loading title_ob2.dat");
        g_loadFailed = true;
        return false;
    }
}

bool IsLoaded() { return g_loaded; }

void ApplyPalette(uint32_t sc) {
    if (!EnsureLoaded(sc)) return;
    const uintptr_t loadPalVa = Resolve(kVaLoadPalette);
    const uintptr_t setPalVa  = Resolve(kVaSetPalette);
    if (!loadPalVa || !setPalVa || !sc) return;
    __try {
        int paletteCount = ReadDatPaletteCount(g_datPath.c_str());
        if (paletteCount <= 0) paletteCount = kPaletteCountFallback;
        reinterpret_cast<LoadPaletteFn>(loadPalVa)(
            static_cast<int>(sc + kOffPalette), g_datPath.c_str(), 0, kPaletteBase, paletteCount);
        reinterpret_cast<SetPaletteFn>(setPalVa)(
            reinterpret_cast<void*>(GraphicsSystem(sc)), static_cast<int>(sc + kOffPalette));
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void DrawBackground(uint32_t sc) {
    const uintptr_t blitVa = Resolve(kVaBlit);
    if (!blitVa || !sc) return;
    __try {
        auto blit = reinterpret_cast<BlitFn>(blitVa);
        void* gfxCtx = reinterpret_cast<void*>(GraphicsSystem(sc));
        const int bgSurface = *reinterpret_cast<int*>(sc + kOffBgSurface); // +1076 title.dat
        if (bgSurface == 0) return;
        // Full-screen opaque blit (transparent color 0 = none), mirroring the
        // background pass of the vanilla title render.
        blit(gfxCtx, 0, 0, 320, 240, bgSurface, 0, 0, 320, 240, 0, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void Release(uint32_t sc) {
    // We keep the private surface allocated for the process lifetime (loaded
    // once); on leaving the submenu we only need to restore the title palette so
    // the vanilla menu renders with correct colors again.
    if (g_loaded) RestoreTitlePalette(sc);
}

void DrawRows(uint32_t sc, int selectedRow, int slideOffsetX) {
    if (!EnsureLoaded(sc)) return;

    const uintptr_t blitVa = Resolve(kVaBlit);
    if (!blitVa) return;

    __try {
        auto blit = reinterpret_cast<BlitFn>(blitVa);
        void* gfxCtx = reinterpret_cast<void*>(GraphicsSystem(sc));
        const int destX = kDestX + slideOffsetX;

        for (int row = 0; row < ROW_COUNT; ++row) {
            // Pick the selected (teal highlight) lane for the focused row, the
            // plain lane for the rest. The white background is keyed out by the
            // blit's transparent color, so only glyph/highlight pixels land.
            const int srcY = (row == selectedRow ? kSelBaseY : kUnselBaseY) + row * kRowPitch;
            const int destY = kDestY + row * kDestStep;
            blit(gfxCtx,
                 destX, destY, destX + kSheetW, destY + kRowH,
                 static_cast<int>(g_surface), 0, srcY, kSheetW, srcY + kRowH,
                 g_transparent, 0);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

} // namespace PracticeMenu::Render
