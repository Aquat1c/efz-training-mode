#pragma once
#include <d3d9.h>

namespace GifPlayer {
    bool Initialize(LPDIRECT3DDEVICE9 dev);
    void Shutdown();
    void Update(double dtSeconds);
    IDirect3DTexture9* GetTexture(unsigned& w, unsigned& h);
}
