#pragma once
//
// EFZ title-screen ABI: verified addresses, screen-context field offsets, and
// function typedefs used by the Practice submenu.
//
// All VAs are for the fixed vanilla efz.exe build (image base 0x00400000,
// relocations stripped). The mod runs under EfzRevival, which hooks efz.exe but
// does NOT relocate it, so these VAs are valid at runtime (InGameNetplay relies
// on the exact same addresses). We resolve them relative to GetEFZBase() so the
// code stays correct even if the loader ever rebases the image.
//
// Sources: shared_documentation/NETPLAY_MENU_INTEGRATION.md (binary-verified),
// EFZ_DECOMPILED_SCREENS_SUMMARY.md, and decompilations/efz/efz_memorial_latest.c
// (updateTitleScreenLogic @ 205871, initializeTitleScreen @ 205798).

#include <windows.h>
#include <cstdint>

#include "../../utils/utilities.h"  // GetEFZBase()

namespace PracticeMenu::EfzTitle {

// ---- efz.exe image base + RVAs -------------------------------------------
constexpr uintptr_t kImageBase = 0x00400000;

// Title screen (vtable off_789980)
constexpr uintptr_t kVaUpdateTitle   = 0x00775FB0; // updateTitleScreenLogic (vtable[1])
constexpr uintptr_t kVaRenderTitle   = 0x007764B0; // renderReplaySelectionScreen (vtable[0]) - title/menu render
constexpr uintptr_t kVaCasePractice  = 0x00776352; // jump-table target for menu selection 3 (Practice)
constexpr uintptr_t kVaTitleEpilogue = 0x00776483; // shared confirm-switch epilogue (returns AL as next-state)

// Game front-end helpers
constexpr uintptr_t kVaLoadImage     = 0x00406DB0; // loadCompressedImageFile(gfxMgr, &dstSurface, path, c1, c2)
constexpr uintptr_t kVaLoadPalette   = 0x0040B760; // loadBGRColorsFromRawFile(paletteBuf, path, srcStart, dstStart, count)
constexpr uintptr_t kVaBlit          = 0x00409A90; // blitSurfaceWithTransparency
constexpr uintptr_t kVaSetPalette    = 0x0040BD30; // setPalette(gfxCtx, paletteBuf)
constexpr uintptr_t kVaReadPixel     = 0x0040BCC0; // readPixelValue(surface) -> transparent index
constexpr uintptr_t kVaPlaySfx       = 0x00406860; // playSoundEffect(gameCtx, sfxId)
constexpr uintptr_t kVaFade          = 0x00759C90; // fadeWithSoundAdjustment(sc, paletteId, dir, baseVol, volAdj)
constexpr uintptr_t kVaPresent       = 0x0040B5E0; // presentFrameToScreen(graphicsContext) - flips the 320x240 frame to the window

// Resolve a VA against the live image base. Returns 0 if base is unavailable.
inline uintptr_t Resolve(uintptr_t va) {
    const uintptr_t base = GetEFZBase();
    return base ? (base + (va - kImageBase)) : 0;
}

// ---- screenContext field offsets (title screen) ---------------------------
constexpr uintptr_t kOffGameContext = 28;   // -> game/resource context pointer
constexpr uintptr_t kOffGraphicsSys = 32;   // -> graphics system pointer (setPalette 'this')
constexpr uintptr_t kOffPalette     = 46;   // palette staging buffer (passed by ADDRESS: sc + 46)
constexpr uintptr_t kOffTransparent = 1070; // transparent color index (BYTE)
constexpr uintptr_t kOffBgSurface   = 1076; // title.dat background surface pointer
constexpr uintptr_t kOffObjSurface  = 1080; // title_ob.dat objects surface pointer
constexpr uintptr_t kOffSelection   = 1084; // current menu selection (BYTE)
constexpr uintptr_t kOffAnimCounter = 1086; // selection animation counter (WORD)
constexpr uintptr_t kOffInputLatch  = 1088; // per-player directional latch (BYTE, +0 P1 / +1 P2)
constexpr uintptr_t kOffLifecycle   = 44;   // +44 init/fade state (0 normal, 1/2/3 transitions)

// ---- gameContext offsets (via *(sc + kOffGameContext)) --------------------
constexpr uintptr_t kGcHorizBase  = 12;   // signed directional byte: +12 P1, +13 P2
constexpr uintptr_t kGcVertBase   = 14;   // signed directional byte: +14 P1, +15 P2
constexpr uintptr_t kGcConfirmBase = 16;   // confirm counter: +16 P1, +17 P2 (== 1 on first frame)
constexpr uintptr_t kGcCancelBase  = 18;   // cancel/back counter: +18 P1, +19 P2 (== 1 edge)
constexpr uintptr_t kGcP1Type      = 4931; // 0=human, 1=CPU
constexpr uintptr_t kGcP2Type      = 4932;
constexpr uintptr_t kGcActivePlayer= 4930;
constexpr uintptr_t kGcGameMode    = 4964; // 1 = Practice
constexpr uintptr_t kGcRoundCount  = 4942;

constexpr int kMenuSelectionPractice = 3;  // Practice is index 3 (stable even with InGameNetplay's NETPLAY at 5)
constexpr unsigned short kSfxConfirm = 6;
constexpr unsigned short kSfxCursor  = 8;

// ---- function typedefs ----------------------------------------------------
typedef void (__thiscall* LoadImageFn)(void* gfxMgr, uint32_t* dstSurface, const char* path,
                                       unsigned char colorOffset1, unsigned char colorOffset2);
typedef int  (__stdcall*  LoadPaletteFn)(int paletteBuf, const char* path, int srcStart, int dstStart, int count);
typedef char (__stdcall*  ReadPixelFn)(int surface);
typedef int  (__thiscall* SetPaletteFn)(void* gfxCtx, int paletteBuf);
typedef int  (__thiscall* PlaySfxFn)(void* gameCtx, unsigned short sfxId);
typedef BOOL (__thiscall* BlitFn)(void* gfxCtx, int destX, int destY, int destRight, int destBottom,
                                  int srcSurface, int srcX, int srcY, int srcRight, int srcBottom,
                                  char transparentColor, int flipHorizontal);
typedef int  (__thiscall* FadeFn)(void* screenContext, int paletteId, unsigned char fadeDir, int baseVol, int volAdj);
typedef BOOL (__thiscall* PresentFn)(int graphicsContext);
typedef char (__thiscall* TitleUpdateFn)(uint32_t screenContext);
typedef BOOL (__thiscall* TitleRenderFn)(uint32_t screenContext);

// ---- small safe field accessors ------------------------------------------
inline uint32_t GameContext(uint32_t sc) {
    return sc ? *reinterpret_cast<uint32_t*>(sc + kOffGameContext) : 0;
}
inline uint32_t GraphicsSystem(uint32_t sc) {
    return sc ? *reinterpret_cast<uint32_t*>(sc + kOffGraphicsSys) : 0;
}

} // namespace PracticeMenu::EfzTitle
