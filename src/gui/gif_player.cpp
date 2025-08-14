#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <gdiplus.h>
#pragma comment(lib, "gdiplus.lib")
#include <d3d9.h>
#include <vector>
#include <memory>
#include <algorithm>
#include <shlwapi.h>
#pragma comment(lib, "shlwapi.lib")
#include <atlbase.h>

#include "../../include/gui/embedded_gif.h"
#include "../../include/core/logger.h"

namespace GifPlayer {

struct Frame {
    CComPtr<IDirect3DTexture9> tex;
    UINT w = 0, h = 0;
    UINT delayMs = 100;
};

static ULONG_PTR s_gdiplusToken = 0;
static std::vector<Frame> s_frames;
static size_t s_index = 0;
static double s_accum = 0.0;
static bool s_inited = false;

static bool EnsureGdiplus() {
    if (s_gdiplusToken) return true;
    Gdiplus::GdiplusStartupInput gdiplusStartupInput;
    return Gdiplus::GdiplusStartup(&s_gdiplusToken, &gdiplusStartupInput, nullptr) == Gdiplus::Ok;
}

static bool BitmapFrameToTexture(LPDIRECT3DDEVICE9 dev, Gdiplus::Bitmap* bmp, Frame& out) {
    if (!bmp) return false;
    const UINT w = bmp->GetWidth();
    const UINT h = bmp->GetHeight();
    if (w == 0 || h == 0) return false;

    CComPtr<IDirect3DTexture9> tex;
    if (FAILED(dev->CreateTexture(w, h, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex, nullptr)))
        return false;

    D3DLOCKED_RECT lr{};
    if (FAILED(tex->LockRect(0, &lr, nullptr, 0))) return false;

    for (UINT y = 0; y < h; ++y) {
        DWORD* dst = reinterpret_cast<DWORD*>(reinterpret_cast<BYTE*>(lr.pBits) + y * lr.Pitch);
        for (UINT x = 0; x < w; ++x) {
            Gdiplus::Color c;
            bmp->GetPixel(x, y, &c);
            dst[x] = (c.GetA() << 24) | (c.GetR() << 16) | (c.GetG() << 8) | (c.GetB());
        }
    }
    tex->UnlockRect(0);

    out.tex = tex;
    out.w = w; out.h = h;
    return true;
}

bool Initialize(LPDIRECT3DDEVICE9 dev) {
    if (s_inited) return true;
    if (!EnsureGdiplus()) return false;

    IStream* stream = SHCreateMemStream(kEmbeddedGif, (UINT)kEmbeddedGifSize);
    if (!stream) return false;
    std::unique_ptr<Gdiplus::Bitmap> bmp(new Gdiplus::Bitmap(stream, FALSE));
    stream->Release();
    if (!bmp || bmp->GetLastStatus() != Gdiplus::Ok) return false;

    // Get frame dimension
    GUID dim;
    UINT count = bmp->GetFrameDimensionsCount();
    if (count == 0) return false;
    bmp->GetFrameDimensionsList(&dim, 1);
    UINT frames = bmp->GetFrameCount(&dim);
    if (frames == 0) frames = 1;

    // Read delays if present
    std::vector<UINT> delays(frames, 100);
    Gdiplus::PropertyItem* prop = nullptr;
    UINT size = bmp->GetPropertyItemSize(PropertyTagFrameDelay);
    if (size) {
        prop = (Gdiplus::PropertyItem*)malloc(size);
        if (prop && bmp->GetPropertyItem(PropertyTagFrameDelay, size, prop) == Gdiplus::Ok) {
            for (UINT i = 0; i < frames; ++i) {
                delays[i] = 10U * ((UINT*)prop->value)[i]; // 1/100s to ms
                // Avoid std::max/min macro conflicts from windows.h
                if (delays[i] < 10U) delays[i] = 10U;
            }
        }
        if (prop) free(prop);
    }

    s_frames.clear();
    s_frames.reserve(frames);
    for (UINT i = 0; i < frames; ++i) {
        bmp->SelectActiveFrame(&dim, i);
        Frame f; f.delayMs = (i < delays.size() ? delays[i] : 100);
        // Clone current frame to a standalone bitmap to avoid mutation surprises
        std::unique_ptr<Gdiplus::Bitmap> clone(bmp->Clone(0, 0, bmp->GetWidth(), bmp->GetHeight(), PixelFormat32bppARGB));
        if (!clone || clone->GetLastStatus() != Gdiplus::Ok) continue;
        if (BitmapFrameToTexture(dev, clone.get(), f)) s_frames.push_back(std::move(f));
    }
    if (s_frames.empty()) return false;
    s_index = 0; s_accum = 0.0; s_inited = true;
    LogOut("[GIF] Embedded GIF loaded: " + std::to_string(s_frames.size()) + " frames", true);
    return true;
}

void Shutdown() {
    s_frames.clear();
    if (s_gdiplusToken) {
        Gdiplus::GdiplusShutdown(s_gdiplusToken);
        s_gdiplusToken = 0;
    }
    s_inited = false;
}

void Update(double dtSeconds) {
    if (!s_inited || s_frames.empty()) return;
    s_accum += dtSeconds * 1000.0;
    UINT delay = s_frames[s_index].delayMs;
    while (s_accum >= delay) {
        s_accum -= delay;
        s_index = (s_index + 1) % s_frames.size();
        delay = s_frames[s_index].delayMs;
    }
}

IDirect3DTexture9* GetTexture(UINT& w, UINT& h) {
    if (!s_inited || s_frames.empty()) return nullptr;
    w = s_frames[s_index].w; h = s_frames[s_index].h;
    return s_frames[s_index].tex;
}

} // namespace GifPlayer
