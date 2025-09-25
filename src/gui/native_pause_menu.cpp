#include "../include/gui/native_pause_menu.h"
#include "../include/utils/pause_integration.h"
#include "../include/core/logger.h"
#include <d3d9.h>
#include <windows.h>
#include <mmsystem.h>
#include <string>
#include <fstream>
#include <vector>
#pragma comment(lib, "winmm.lib")
#include "../include/game/game_state.h"
#include "../include/core/memory.h"
#include "../include/core/constants.h"
#include "../include/gui/imgui_impl.h" // only for toggling the big training GUI; NOT used for rendering here
#include "../include/utils/utilities.h" // GetEFZBase, SetPlayerPosition

namespace NativePauseMenu {
    static std::atomic<bool> g_visible{false};
    static bool g_initialized=false;
    static ULONGLONG g_lastShowTick = 0ULL;
    static float g_fadeAlpha = 0.0f; // 0..1 animation value
    // Cached game root path (folder containing EFZ executable)
    static char g_rootPath[MAX_PATH] = {0};
    static const char* GetGameRootPath(){
        if(g_rootPath[0]) return g_rootPath;
        uintptr_t base = GetEFZBase();
        if(!base) return "";
        char buf[MAX_PATH];
        DWORD n = GetModuleFileNameA((HMODULE)base, buf, MAX_PATH);
        if(n==0 || n>=MAX_PATH) return "";
        // strip filename
        int lastSlash = -1; for(DWORD i=0;i<n;++i){ if(buf[i]=='\\' || buf[i]=='/') lastSlash=(int)i; }
        if(lastSlash>=0){ buf[lastSlash+1]='\0'; }
        strncpy_s(g_rootPath, buf, _TRUNCATE);
        return g_rootPath;
    }
    static void PlayMenuSound(){
        const char* root = GetGameRootPath();
        if(root[0]==0) return;
        std::string path = std::string(root) + "wave\\se\\08.wav"; // navigation sound
        PlaySoundA(path.c_str(), NULL, SND_FILENAME | SND_ASYNC | SND_NODEFAULT);
    }
    void Initialize(){ if(!g_initialized){ g_initialized=true; LogOut("[NATIVE_PAUSE] Initialized", true);} }
    bool IsVisible(){ return g_visible.load(); }
    void Show(){ if(!g_initialized) Initialize(); if(g_visible.exchange(true)) return; g_lastShowTick = GetTickCount64(); g_fadeAlpha = 0.0f; PauseIntegration::OnMenuVisibilityChanged(true); }
    void Hide(){ if(!g_visible.exchange(false)) return; PauseIntegration::OnMenuVisibilityChanged(false); }
    void Toggle(){ IsVisible()?Hide():Show(); }
    void TickInput(){
        // F1 edge detection every frame (practice mode only)
        {
            static bool lastF1=false; 
            SHORT s = GetAsyncKeyState(VK_F1);
            bool down = (s & 0x8000)!=0;
            if(down && !lastF1) {
                if(GetCurrentGameMode()==GameMode::Practice) {
                    Toggle();
                }
            }
            lastF1 = down;
        }
        if(!IsVisible()) return; 
        static SHORT prevEsc=0; SHORT esc=GetAsyncKeyState(VK_ESCAPE); bool d=(esc&0x8000)!=0, p=(prevEsc&0x8000)!=0; if(d && !p) Hide(); prevEsc=esc; }
    struct Vtx { float x,y,z,rhw; unsigned int color; };
    static void Quad(LPDIRECT3DDEVICE9 dev,float x,float y,float w,float h,unsigned int c){ Vtx v[4]={{x,y,0,1,c},{x+w,y,0,1,c},{x+w,y+h,0,1,c},{x,y+h,0,1,c}}; dev->SetFVF(D3DFVF_XYZRHW|D3DFVF_DIFFUSE); dev->DrawPrimitiveUP(D3DPT_TRIANGLEFAN,2,v,sizeof(Vtx)); }

    // Minimal 8x16 bitmap font (subset A-Z 0-9 and punctuation used). Each glyph 16 rows of 8 bits.
    // For brevity, not all ASCII provided; unknown chars render as blank.
    static const unsigned char FONT8x16[96][16] = { {0} };// TODO: expand with real glyphs if needed
    // Simple block letter fallback: we'll render using rectangles forming each character from a 5x7 pattern.
    // New 6x10 pixel font (custom) with outline for better readability.
    static void DrawCharRaw(LPDIRECT3DDEVICE9 dev,float x,float y,float scale,unsigned int color,char c){
        // Each row uses lower 6 bits (bit5 leftmost). 10 rows.
        static const unsigned short patterns[][10]={
            /* 'A' */{0x1E,0x33,0x21,0x21,0x3F,0x21,0x21,0x21,0x00,0x00},
            /* 'B' */{0x3E,0x23,0x23,0x3E,0x23,0x23,0x23,0x3E,0x00,0x00},
            /* 'C' */{0x1E,0x21,0x20,0x20,0x20,0x20,0x21,0x1E,0x00,0x00},
            /* 'D' */{0x3C,0x22,0x23,0x21,0x21,0x21,0x23,0x3E,0x00,0x00},
            /* 'E' */{0x3F,0x20,0x20,0x3E,0x20,0x20,0x20,0x3F,0x00,0x00},
            /* 'F' */{0x3F,0x20,0x20,0x3E,0x20,0x20,0x20,0x20,0x00,0x00},
            /* 'G' */{0x1E,0x21,0x20,0x20,0x27,0x21,0x21,0x1F,0x00,0x00},
            /* 'H' */{0x21,0x21,0x21,0x3F,0x21,0x21,0x21,0x21,0x00,0x00},
            /* 'I' */{0x1E,0x08,0x08,0x08,0x08,0x08,0x08,0x1E,0x00,0x00},
            /* 'J' */{0x07,0x02,0x02,0x02,0x02,0x22,0x22,0x1C,0x00,0x00},
            /* 'K' */{0x21,0x22,0x24,0x38,0x24,0x22,0x22,0x21,0x00,0x00},
            /* 'L' */{0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x3F,0x00,0x00},
            /* 'M' */{0x21,0x31,0x2B,0x25,0x21,0x21,0x21,0x21,0x00,0x00},
            /* 'N' */{0x21,0x31,0x29,0x25,0x23,0x21,0x21,0x21,0x00,0x00},
            /* 'O' */{0x1E,0x21,0x21,0x21,0x21,0x21,0x21,0x1E,0x00,0x00},
            /* 'P' */{0x3E,0x23,0x23,0x23,0x3E,0x20,0x20,0x20,0x00,0x00},
            /* 'Q' */{0x1E,0x21,0x21,0x21,0x21,0x25,0x22,0x1D,0x00,0x00},
            /* 'R' */{0x3E,0x23,0x23,0x3E,0x24,0x22,0x22,0x21,0x00,0x00},
            /* 'S' */{0x1F,0x20,0x20,0x1E,0x01,0x01,0x01,0x3E,0x00,0x00},
            /* 'T' */{0x3F,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x00,0x00},
            /* 'U' */{0x21,0x21,0x21,0x21,0x21,0x21,0x21,0x1E,0x00,0x00},
            /* 'V' */{0x21,0x21,0x21,0x21,0x21,0x12,0x12,0x0C,0x00,0x00},
            /* 'W' */{0x21,0x21,0x21,0x25,0x25,0x2B,0x33,0x21,0x00,0x00},
            /* 'X' */{0x21,0x21,0x12,0x0C,0x0C,0x12,0x21,0x21,0x00,0x00},
            /* 'Y' */{0x21,0x21,0x12,0x0C,0x08,0x08,0x08,0x08,0x00,0x00},
            /* 'Z' */{0x3F,0x01,0x02,0x04,0x08,0x10,0x20,0x3F,0x00,0x00},
            /* '0' */{0x1E,0x21,0x23,0x25,0x29,0x31,0x21,0x1E,0x00,0x00},
            /* '1' */{0x08,0x18,0x08,0x08,0x08,0x08,0x08,0x1C,0x00,0x00},
            /* '2' */{0x1E,0x21,0x01,0x02,0x04,0x08,0x10,0x3F,0x00,0x00},
            /* '3' */{0x3F,0x02,0x04,0x0E,0x01,0x01,0x21,0x1E,0x00,0x00},
            /* '4' */{0x02,0x06,0x0A,0x12,0x22,0x3F,0x02,0x02,0x00,0x00},
            /* '5' */{0x3F,0x20,0x20,0x3E,0x01,0x01,0x21,0x1E,0x00,0x00},
            /* '6' */{0x0E,0x10,0x20,0x3E,0x21,0x21,0x21,0x1E,0x00,0x00},
            /* '7' */{0x3F,0x01,0x02,0x04,0x08,0x08,0x10,0x10,0x00,0x00},
            /* '8' */{0x1E,0x21,0x21,0x1E,0x21,0x21,0x21,0x1E,0x00,0x00},
            /* '9' */{0x1E,0x21,0x21,0x21,0x1F,0x01,0x02,0x1C,0x00,0x00},
            /* ':' */{0x00,0x08,0x00,0x00,0x00,0x08,0x00,0x00,0x00,0x00},
            /* '-' */{0x00,0x00,0x00,0x1F,0x00,0x00,0x00,0x00,0x00,0x00},
            /* ' ' */{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
        };
        auto idxFromChar=[&](char ch)->int{
            if(ch>='A'&&ch<='Z') return ch-'A';
            if(ch>='0'&&ch<='9') return 26+(ch-'0');
            if(ch==':') return 36; if(ch=='-') return 37; if(ch==' ') return 38; return -1; };
        int idx=idxFromChar((c>='a'&&c<='z')? (char)(c-32):c);
        if(idx<0) return;
        float cell=scale;
        // Outline (simple 4-direction) for better contrast at small sizes
        unsigned int outline = (color & 0xFF000000) | 0x202020; // dark gray
        float o = cell*0.5f; if(o<1.f) o=1.f; // minimal pixel shift
        auto drawPattern=[&](float ox,float oy,unsigned int col){
            for(int row=0;row<10;++row){ unsigned short bits=patterns[idx][row]; for(int colBit=0; colBit<6; ++colBit){ if(bits & (1<<(5-colBit))){ Quad(dev,x+ox+colBit*cell,y+oy+row*cell,cell,cell,col); } } }
        };
        // Outline passes
        drawPattern(-o,0,outline); drawPattern(o,0,outline); drawPattern(0,-o,outline); drawPattern(0,o,outline);
        // Main glyph
        drawPattern(0,0,color);
    }
    static void DrawTextLine(LPDIRECT3DDEVICE9 dev,float x,float y,float scale,unsigned int color,const char* txt,bool shadow=true){
        float spacing = scale*0.8f; // tighten relative to 6px base
        for(int i=0; txt[i]; ++i){
            float cx = x + i*(6*scale + spacing);
            if(shadow) DrawCharRaw(dev,cx+scale*0.4f,y+scale*0.4f,scale,0x66000000,txt[i]);
            DrawCharRaw(dev,cx,y,scale,color,txt[i]);
        }
    }
    struct RenderStateScope { LPDIRECT3DDEVICE9 d; DWORD aE=0,sB=0,dB=0,zE=0; RenderStateScope(LPDIRECT3DDEVICE9 dev):d(dev){ if(!d) return; d->GetRenderState(D3DRS_ALPHABLENDENABLE,&aE); d->GetRenderState(D3DRS_SRCBLEND,&sB); d->GetRenderState(D3DRS_DESTBLEND,&dB); d->GetRenderState(D3DRS_ZENABLE,&zE); d->SetRenderState(D3DRS_ZENABLE,FALSE); d->SetRenderState(D3DRS_ALPHABLENDENABLE,TRUE); d->SetRenderState(D3DRS_SRCBLEND,D3DBLEND_SRCALPHA); d->SetRenderState(D3DRS_DESTBLEND,D3DBLEND_INVSRCALPHA);} ~RenderStateScope(){ if(!d) return; d->SetRenderState(D3DRS_ZENABLE,zE); d->SetRenderState(D3DRS_ALPHABLENDENABLE,aE); d->SetRenderState(D3DRS_SRCBLEND,sB); d->SetRenderState(D3DRS_DESTBLEND,dB);} };
    static unsigned int ModAlpha(unsigned int argb,float mul){ unsigned int a=(argb>>24)&0xFF; unsigned int na=(unsigned int)(a*mul); return (argb & 0x00FFFFFF)|(na<<24); }
    static float EaseOutCubic(float t){ if(t<0) t=0; if(t>1) t=1; float inv=1.f-t; return 1.f - inv*inv*inv; }

    // -------------------------------------------------------------------------------------------------
    // Game bitmap font support (loads training_font.bmp from game root if present)
    // Expected layout: 16 columns x 16 rows grid (256 glyphs) each cell same size (e.g. 16x16)
    // We sample only ASCII subset used by menu. Transparent = black or low intensity.
    struct GameBitmapFont {
        LPDIRECT3DTEXTURE9 tex = nullptr;
        int cellW=0, cellH=0; int cols=0, rows=0; bool loaded=false; 
    };
    static GameBitmapFont g_gameFont; static bool g_gameFontTried=false;

    #pragma pack(push,1)
    struct BMPFileHeader { uint16_t bfType; uint32_t bfSize; uint16_t bfReserved1; uint16_t bfReserved2; uint32_t bfOffBits; };
    struct BMPInfoHeader { uint32_t biSize; int32_t biWidth; int32_t biHeight; uint16_t biPlanes; uint16_t biBitCount; uint32_t biCompression; uint32_t biSizeImage; int32_t biXPelsPerMeter; int32_t biYPelsPerMeter; uint32_t biClrUsed; uint32_t biClrImportant; };
    #pragma pack(pop)

    static void TryLoadGameFont(LPDIRECT3DDEVICE9 dev){
        if(g_gameFontTried) return; g_gameFontTried=true; if(!dev) return; const char* root=GetGameRootPath(); if(root[0]==0) return;
        std::string path = std::string(root) + "training_font.bmp"; // user-supplied export of game font
    std::ifstream f(path, std::ios::binary);
    if(!f.is_open()) return;
    BMPFileHeader fh{}; BMPInfoHeader ih{}; 
    f.read((char*)&fh,sizeof(fh));
    f.read((char*)&ih,sizeof(ih));
    if(fh.bfType!=0x4D42|| ih.biCompression!=0) return; 
    if(ih.biBitCount!=24 && ih.biBitCount!=32) return; 
    f.seekg(fh.bfOffBits, std::ios::beg); 
    int width=ih.biWidth; int height=ih.biHeight; 
    bool bottomUp = height>0; if(height<0) height=-height; 
    int bpp=ih.biBitCount/8; 
    size_t rowSize = ((size_t)width * bpp + 3) & ~3; 
    std::vector<unsigned char> bmp(rowSize*height); 
    f.read((char*)bmp.data(), bmp.size()); 
    if(!f){ return; }
        // Infer cell size by dividing by 16
        if(width%16!=0 || height%16!=0) return; int cellW=width/16; int cellH=height/16; 
        // Create texture
        IDirect3DTexture9* tex=nullptr; if(FAILED(dev->CreateTexture(width,height,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,&tex,nullptr))) return; D3DLOCKED_RECT lr; if(FAILED(tex->LockRect(0,&lr,nullptr,0))){ tex->Release(); return; }
        for(int y=0;y<height;++y){ unsigned char* dstRow = (unsigned char*)lr.pBits + y*lr.Pitch; int srcY = bottomUp? (height-1 - y): y; unsigned char* src = bmp.data()+ srcY*rowSize; for(int x=0;x<width;++x){ unsigned char r,g,b,a; b=src[x*bpp+0]; g=src[x*bpp+1]; r=src[x*bpp+2]; a = (b|g|r)>20? 255:0; if(bpp==4) a = src[x*bpp+3]; // allow original alpha for 32-bit
                dstRow[x*4+0]=b; dstRow[x*4+1]=g; dstRow[x*4+2]=r; dstRow[x*4+3]=a; }
        }
        tex->UnlockRect(0); g_gameFont.tex=tex; g_gameFont.cellW=cellW; g_gameFont.cellH=cellH; g_gameFont.cols=16; g_gameFont.rows=16; g_gameFont.loaded=true; LogOut("[NATIVE_PAUSE] Loaded training_font.bmp ("+std::to_string(width)+"x"+std::to_string(height)+")", true);
    }

    struct FontVtx { float x,y,z,rhw; unsigned int color; float u,v; };
    static void DrawGameFontText(LPDIRECT3DDEVICE9 dev,float x,float y,float scale,unsigned int color,const char* txt){ if(!g_gameFont.loaded || !dev) return; dev->SetTexture(0,g_gameFont.tex); dev->SetFVF(D3DFVF_XYZRHW|D3DFVF_DIFFUSE|D3DFVF_TEX1); float cw = g_gameFont.cellW*scale; float ch = g_gameFont.cellH*scale; for(int i=0; txt[i]; ++i){ unsigned char c = (unsigned char)txt[i]; int col = c % g_gameFont.cols; int row = c / g_gameFont.cols; float u0 = (float)(col*g_gameFont.cellW)/(g_gameFont.cols*g_gameFont.cellW); float v0 = (float)(row*g_gameFont.cellH)/(g_gameFont.rows*g_gameFont.cellH); float u1 = (float)((col+1)*g_gameFont.cellW)/(g_gameFont.cols*g_gameFont.cellW); float v1 = (float)((row+1)*g_gameFont.cellH)/(g_gameFont.rows*g_gameFont.cellH); float sx = x + i*(cw*0.72f); FontVtx v[4]={{sx,y,0,1,color,u0,v0},{sx+cw,y,0,1,color,u1,v0},{sx+cw,y+ch,0,1,color,u1,v1},{sx,y+ch,0,1,color,u0,v1}}; dev->DrawPrimitiveUP(D3DPT_TRIANGLEFAN,2,v,sizeof(FontVtx)); }
        dev->SetTexture(0,nullptr);
    }

    void RenderD3D9(LPDIRECT3DDEVICE9 dev){
        if(!dev) return; if(!IsVisible() && g_fadeAlpha<=0.01f) return; // allow fade-out if implemented later
        D3DVIEWPORT9 vp{}; if(FAILED(dev->GetViewport(&vp))) return; float W=(float)vp.Width,H=(float)vp.Height; 
        if(IsVisible()){ ULONGLONG now=GetTickCount64(); float t=(float)((now - g_lastShowTick)/250.0f); g_fadeAlpha = (g_fadeAlpha<1.f)? EaseOutCubic(t):1.f; if(g_fadeAlpha>1.f) g_fadeAlpha=1.f; }
        else { g_fadeAlpha -= 0.15f; if(g_fadeAlpha<0.f) g_fadeAlpha=0.f; }
        float a = g_fadeAlpha; if(a<=0.f) return; RenderStateScope rs(dev);
    // Try loading game font once we have a device
    if(!g_gameFont.loaded) TryLoadGameFont(dev);
    // True ~25% translucent full-screen dim (0x40 alpha on black then modded by fade)
        Quad(dev,0,0,W,H, (unsigned int)(0x40 * a) << 24 );
        // Panel metrics + gentle scale pop
    // Resolution adaptive scaling (keeps menu modest size on different resolutions)
    float vScale = ((W/1280.f)+(H/720.f))*0.5f; if(vScale<0.8f) vScale=0.8f; if(vScale>1.25f) vScale=1.25f;
    float baseW=320.f*vScale, baseH=185.f*vScale; float scaleAnim = 0.94f + 0.06f*EaseOutCubic(a); float pw=baseW*scaleAnim, ph=baseH*scaleAnim; float px=(W-pw)/2.f, py=(H-ph)/2.f;
        // Soft shadow
        for(int i=0;i<5;++i){ float inf=i*2.f; unsigned int sa=(unsigned int)( (30 - i*5) * a ); if(sa<4) sa=4; Quad(dev,px-4-inf,py-4-inf,pw+8+inf*2,ph+8+inf*2, sa<<24 ); }
        // Gradient body layers
        Quad(dev,px,py,pw,ph,ModAlpha(0xF0202026,a));
        Quad(dev,px,py,pw,ph*0.55f,ModAlpha(0xF0333A48,a));
        // Border accents
        Quad(dev,px,py,pw,2,ModAlpha(0xFF6FA8FF,a));
        Quad(dev,px,py+ph-2,pw,2,ModAlpha(0xFF2A3E54,a));
        Quad(dev,px,py,2,ph,ModAlpha(0xFF456789,a));
        Quad(dev,px+pw-2,py,2,ph,ModAlpha(0xFF456789,a));
        // Header
    float headerH = 26.f * vScale; if(headerH<24.f) headerH=24.f; // keep readable
    Quad(dev,px,py,pw,headerH,ModAlpha(0xFF25425F,a));
    Quad(dev,px,py+headerH,pw,1,ModAlpha(0xFF6FA8FF,a));
    float titleScale = 1.2f * vScale; if(titleScale<0.95f) titleScale=0.95f; // game font tends to be larger
    if(g_gameFont.loaded){ DrawGameFontText(dev,px+14,py+4*vScale,titleScale,ModAlpha(0xFFFFFFFF,a),"PAUSE"); }
    else { DrawTextLine(dev,px+14,py+6* vScale,titleScale*1.55f,ModAlpha(0xFFFFFFFF,a),"PAUSE",true); }
        // Menu entries
    static const char* items[] = { "RESUME", "TRAINING SETTINGS", "RESET POSITIONS", "QUIT" }; 
        const int itemCount = (int)(sizeof(items)/sizeof(items[0]));
        static int selected=0; if(selected>=itemCount) selected=itemCount-1; if(selected<0) selected=0;
        // Input (handled here to keep selection responsive when visible)
        static bool prevUp=false, prevDown=false, prevEnter=false; 
        bool up = (GetAsyncKeyState(VK_UP)&0x8000)!=0; 
        bool down = (GetAsyncKeyState(VK_DOWN)&0x8000)!=0; 
        bool enter = (GetAsyncKeyState(VK_RETURN)&0x8000)!=0 || (GetAsyncKeyState(VK_SPACE)&0x8000)!=0; 
        if(up && !prevUp){ selected = (selected + itemCount -1)%itemCount; PlayMenuSound(); }
        if(down && !prevDown){ selected = (selected +1)%itemCount; PlayMenuSound(); }
        if(enter && !prevEnter){ PlayMenuSound();
            switch(selected){
                case 0: Hide(); break; // Resume
                case 1: { // Training Settings (open big ImGui GUI)
                    if(!ImGuiImpl::IsVisible()) ImGuiImpl::ToggleVisibility();
                    Hide(); break; }
                case 2: { // Reset Positions (round start style)
                    uintptr_t base = GetEFZBase();
                    if(base){
                        double p1StartX = 240.0, p2StartX = 400.0, startY = 0.0; 
                        SetPlayerPosition(base, EFZ_BASE_OFFSET_P1, p1StartX, startY);
                        SetPlayerPosition(base, EFZ_BASE_OFFSET_P2, p2StartX, startY);
                    }
                    Hide(); break; }
                case 3: // Quit placeholder
                    Hide(); break;
            }
        }
        prevUp=up; prevDown=down; prevEnter=enter; 
        // Visual list with animated highlight pulse
        float itemScale = 1.45f * vScale; if(itemScale<1.1f) itemScale=1.1f; float lineH = itemScale*13.0f; float listY = py + headerH + 14.f * vScale; ULONGLONG now=GetTickCount64(); float pulse = 0.5f + 0.5f*sinf((float)((now % 1200)/1200.0f * 6.28318f));
        for(int i=0;i<itemCount;++i){ bool sel=(i==selected); unsigned int txtCol = sel? ModAlpha(0xFFFFFFFF,a):ModAlpha(0xFFD0DAE4,a); if(sel){ unsigned int glow = ModAlpha(0xFF6FA8FF, a*(0.55f + 0.45f*pulse)); unsigned int glow2 = ModAlpha(0x806FA8FF, a*0.6f); float padY= lineH*0.25f; Quad(dev,px+10,listY-padY,pw-20,lineH-padY*0.3f,ModAlpha(0x402F4F70,a)); Quad(dev,px+10,listY-padY,pw-20,(lineH-padY*0.3f)*0.55f,glow2); Quad(dev,px+10,listY-padY,pw-20,2,glow); }
            if(g_gameFont.loaded){ DrawGameFontText(dev,px+26,listY,itemScale,txtCol,items[i]); }
            else { DrawTextLine(dev,px+24,listY,itemScale,txtCol,items[i],true); }
            listY += lineH; }
        if(g_gameFont.loaded){ DrawGameFontText(dev,px+18, py+ph- (18.f * vScale),0.75f * vScale,ModAlpha(0xFF93A8B8,a),"UP/DOWN  ENTER  ESC"); }
        else { DrawTextLine(dev,px+18, py+ph- (18.f * vScale),0.95f * vScale,ModAlpha(0xFF93A8B8,a),"UP/DOWN  ENTER  ESC",true); }
    }
}
