#include "../../../include/game/mission/mission_render.h"
#include "../../../include/game/mission/mission_engine.h"
#include "../../../include/game/mission/mission_data.h"
#include "../../../include/game/practice_menu/mission_title_screen.h"

#include "../../../3rdparty/imgui/imgui.h"

#include <windows.h>
#include <d3d9.h>
#include <gdiplus.h>

#include <cfloat>
#include <map>
#include <string>
#include <vector>

#pragma comment(lib, "gdiplus.lib")

// M3 scaffold: draw the active mission's combo recipe as a row of status-colored
// step boxes with control icons (assets/controls). Icons are decoded once via
// GDI+ into D3D9 textures (mirroring gif_player.cpp) from the live device.

namespace Mission::Render {

namespace {

struct Icon { IDirect3DTexture9* tex = nullptr; UINT w = 0, h = 0; };

ULONG_PTR g_gdiplusToken = 0;
bool g_gdiplusOk = false;
bool g_iconsTried = false;   // attempted a load this device-session
std::map<char, Icon> g_icons;

// ---- status palette (AABBGGRR for ImGui) ----
constexpr ImU32 kBoxDone    = IM_COL32( 40, 120,  40, 200);
constexpr ImU32 kBoxCurrent = IM_COL32(200, 170,  20, 220);
constexpr ImU32 kBoxNext    = IM_COL32( 25,  25,  30, 190);
constexpr ImU32 kBoxFailed  = IM_COL32(150,  30,  30, 220);
constexpr ImU32 kBorder     = IM_COL32(255, 255, 255, 120);
constexpr ImU32 kTextCol    = IM_COL32(235, 235, 235, 255);

bool EnsureGdiplus() {
    if (g_gdiplusOk) return true;
    Gdiplus::GdiplusStartupInput in;
    g_gdiplusOk = (Gdiplus::GdiplusStartup(&g_gdiplusToken, &in, nullptr) == Gdiplus::Ok);
    return g_gdiplusOk;
}

std::wstring ControlsDir() {
    HMODULE mod = GetModuleHandleA("efz_training_mode.dll");
    char path[MAX_PATH] = {0};
    if (!mod || GetModuleFileNameA(mod, path, MAX_PATH) == 0) return std::wstring();
    std::string dir(path);
    const size_t slash = dir.find_last_of("\\/");
    if (slash == std::string::npos) return std::wstring();
    dir.resize(slash + 1);
    dir += "assets\\controls\\";
    // widen (ASCII path)
    std::wstring w(dir.begin(), dir.end());
    return w;
}

bool LoadTexture(IDirect3DDevice9* dev, const std::wstring& file, Icon& out) {
    Gdiplus::Bitmap bmp(file.c_str());
    if (bmp.GetLastStatus() != Gdiplus::Ok) return false;
    const UINT w = bmp.GetWidth(), h = bmp.GetHeight();
    if (!w || !h) return false;
    IDirect3DTexture9* tex = nullptr;
    if (FAILED(dev->CreateTexture(w, h, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex, nullptr))) return false;
    D3DLOCKED_RECT lr{};
    if (FAILED(tex->LockRect(0, &lr, nullptr, 0))) { tex->Release(); return false; }
    Gdiplus::Rect rect(0, 0, static_cast<INT>(w), static_cast<INT>(h));
    Gdiplus::BitmapData data{};
    if (bmp.LockBits(&rect, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &data) == Gdiplus::Ok) {
        const BYTE* scan0 = reinterpret_cast<const BYTE*>(data.Scan0);
        const size_t rowBytes = static_cast<size_t>(w) * sizeof(DWORD);
        for (UINT y = 0; y < h; ++y) {
            const BYTE* src = data.Stride >= 0
                ? scan0 + static_cast<size_t>(y) * data.Stride
                : scan0 + static_cast<size_t>(h - 1 - y) * static_cast<size_t>(-data.Stride);
            BYTE* dst = reinterpret_cast<BYTE*>(lr.pBits) + static_cast<size_t>(y) * lr.Pitch;
            memcpy(dst, src, rowBytes);
        }
        bmp.UnlockBits(&data);
    }
    tex->UnlockRect(0);
    out.tex = tex; out.w = w; out.h = h;
    return true;
}

void EnsureIcons(IDirect3DDevice9* dev) {
    if (g_iconsTried || !dev) return;
    g_iconsTried = true;
    if (!EnsureGdiplus()) return;
    const std::wstring dir = ControlsDir();
    if (dir.empty()) return;

    struct Entry { char key; const wchar_t* file; };
    static const Entry entries[] = {
        {'1', L"1.gif"}, {'2', L"2.gif"}, {'3', L"3.gif"}, {'4', L"4.gif"},
        {'6', L"6.gif"}, {'7', L"7.gif"}, {'8', L"8.gif"}, {'9', L"9.gif"},
        {'A', L"A.png"}, {'B', L"B.png"}, {'C', L"C.png"}, {'D', L"D.png"}, {'S', L"S.png"},
    };
    for (const Entry& e : entries) {
        Icon ic;
        if (LoadTexture(dev, dir + e.file, ic)) g_icons[e.key] = ic;
    }
}

// Split a notation into a shown position PREFIX ("j.", "jj", "c.", "f."), icon
// tokens, and a text suffix. CASE-SENSITIVE parsing of the first whitespace
// token only ("236236 Lv1 (S)" -> icons "236236", text "Lv1 (S)"). Digits 1-9
// (5 = neutral, skipped) and UPPERCASE A/B/C/D/S map to icons; the first
// unrecognized uppercase (I of "IC", F of "FM") turns the rest of the head into
// text - so "c.5B" is "c." + [B] and "j.IC" is "j." + text "IC".
void SplitNotation(const std::string& notation, std::string& prefixOut,
                   std::string& iconsOut, std::string& suffixOut) {
    prefixOut.clear(); iconsOut.clear(); suffixOut.clear();
    const size_t sp = notation.find(' ');
    const std::string head = notation.substr(0, sp);
    if (sp != std::string::npos) suffixOut = notation.substr(sp + 1);
    size_t i = 0;
    while (i < head.size() &&
           ((head[i] >= 'a' && head[i] <= 'z') || head[i] == '.')) {
        prefixOut.push_back(head[i]); ++i;               // "j." / "jj" / "c." ...
    }
    for (; i < head.size(); ++i) {
        const char c = head[i];
        if (c >= '1' && c <= '9') {
            if (c != '5') iconsOut.push_back(c);
        } else if (c == 'A' || c == 'B' || c == 'C' || c == 'D' || c == 'S') {
            iconsOut.push_back(c);
        } else {
            // Unrecognized (I of IC, F of FM, '/', 'x', ...): rest of head = text.
            const std::string rest = head.substr(i);
            suffixOut = suffixOut.empty() ? rest : rest + " " + suffixOut;
            break;
        }
    }
    if (prefixOut.empty() && iconsOut.empty() && suffixOut.empty() && !head.empty())
        suffixOut = head;
}

} // namespace

void Draw(void* device, ImDrawList* dl, float ox, float oy, float scale) {
    if (!dl || !device) return;
    // Startup/restore, title launch UI, and demonstration playback each own the
    // presentation surface. The trial recipe is player-attempt UI and must not
    // leak behind those layers (the old launch screenshot showed both at once).
    if (PracticeMenu::TitleScreen::WantsDraw() || Engine::Demo::IsActive() ||
        !Engine::Runner::IsReadyForPlayer()) return;
    ::Mission::Mission mission;
    int cur = 0;
    int failed = -1;
    int curHits = 0;
    bool armed = false;
    if (!Engine::Runner::GetRenderSnapshot(mission, cur, failed, armed, curHits) ||
        mission.steps.empty()) return;
    const ::Mission::Mission* m = &mission;

    IDirect3DDevice9* dev = reinterpret_cast<IDirect3DDevice9*>(device);
    EnsureIcons(dev);

    const int total = static_cast<int>(m->steps.size());
    const bool tutorial = m->type == "tutorial";

    // CCCaster-style recipe: input ICONS flow on translucent strips, wrapping at
    // the screen edge. No boxes per step - status is conveyed the CCCaster way:
    //   done    = green-tinted icons/text
    //   current = full-bright + accent underline (yellow while a hit is pending)
    //   failed  = latched red until a new opener is genuinely satisfied
    //   next    = slightly dimmed
    // Arrows keep their native aspect (the 2003 wiki GIFs are 15-19px wide at
    // 16px tall - squares distorted them). Text appears only for tokens with no
    // icon (j./dj prefixes, IC/FM, xN counts, state tags).
    const float S = scale;
    const float iconH = 14.0f * S;
    const float pad = 2.0f * S;
    const float cellH = iconH + pad * 2.0f;
    // Wide inter-step gap with a chevron divider drawn in it: each move reads
    // as its own tight cluster, CCCaster-style.
    const float cellGap = 14.0f * S;
    const float lineGap = 3.0f * S;
    const float fontSz = 11.0f * S;
    const float x0 = ox + 8.0f * S;
    const float xMax = ox + 632.0f * S;           // wrap at the 4:3 right edge
    ImFont* font = ImGui::GetFont();

    // Hands-on tutorials carry their teaching context into the match instead
    // of dropping the player into an unexplained recipe. Keep it compact and
    // leave the lower screen unobstructed for play.
    if (tutorial) {
        const float panelX0 = ox + 8.0f * S;
        const float panelY0 = oy + 38.0f * S;
        const float panelX1 = ox + 632.0f * S;
        const float panelY1 = oy + 86.0f * S;
        dl->AddRectFilled(ImVec2(panelX0, panelY0), ImVec2(panelX1, panelY1),
                          IM_COL32(0, 0, 0, 185), 3.0f * S);
        dl->AddRectFilled(ImVec2(panelX0, panelY0), ImVec2(panelX0 + 3.0f * S, panelY1),
                          IM_COL32(105, 235, 235, 225));
        dl->AddText(font, 12.0f * S, ImVec2(panelX0 + 10.0f * S, panelY0 + 5.0f * S),
                    IM_COL32(255, 255, 255, 255), m->name.c_str());
        const std::string& guidance = !m->description.empty()
            ? m->description : (!m->hints.empty() ? m->hints.front() : std::string());
        if (!guidance.empty()) {
            dl->PushClipRect(ImVec2(panelX0 + 10.0f * S, panelY0 + 21.0f * S),
                             ImVec2(panelX1 - 8.0f * S, panelY1 - 3.0f * S), true);
            dl->AddText(font, 9.5f * S, ImVec2(panelX0 + 10.0f * S, panelY0 + 21.0f * S),
                        IM_COL32(190, 205, 210, 240), guidance.c_str(), nullptr,
                        panelX1 - panelX0 - 20.0f * S);
            dl->PopClipRect();
        }
    }

    struct Cell { std::string prefix, icons, text; float x = 0, y = 0, w = 0; };
    std::vector<Cell> cells(total);
    for (int i = 0; i < total; ++i) {
        const ::Mission::Step& st = m->steps[i];
        std::string note = st.notation;
        if (note.empty()) {   // freshly recorded, unnamed: show the move-id
            char b[16];
            _snprintf_s(b, sizeof(b), _TRUNCATE, "#%d",
                        st.moveIds.empty() ? 0 : st.moveIds[0]);
            note = b;
        }
        SplitNotation(note, cells[i].prefix, cells[i].icons, cells[i].text);
        if (i > 0 && m->steps[i - 1].comboEndAfter) {
            // Explicit recovery boundary: this is a new combo segment/setup,
            // not an ordinary link whose delay should force a retry.
            cells[i].prefix = "[SETUP] " + cells[i].prefix;
        }
        if (st.req == ::Mission::StepReq::Hits && st.hitsRequired > 1) {
            char b[16];
            if (i == cur && armed)
                _snprintf_s(b, sizeof(b), _TRUNCATE, " %d/%d", curHits, st.hitsRequired);
            else
                _snprintf_s(b, sizeof(b), _TRUNCATE, " x%d", st.hitsRequired);
            cells[i].text += b;
        }
        if (st.maxDelay > 0) cells[i].prefix = "~" + cells[i].prefix; // delayed hit
        if (st.optional)     cells[i].text += "?";                     // optional
    }

    // Aspect-correct icon width at the target height.
    auto iconW = [&](char c) -> float {
        auto it = g_icons.find(c);
        if (it != g_icons.end() && it->second.tex && it->second.h > 0) {
            return iconH * (static_cast<float>(it->second.w) / static_cast<float>(it->second.h));
        }
        return iconH * 0.75f;   // fallback glyph slot
    };
    auto textW = [&](const std::string& t) -> float {
        if (t.empty()) return 0.0f;
        return font->CalcTextSizeA(fontSz, FLT_MAX, 0.0f, t.c_str()).x + 1.0f * S;
    };

    // ---- layout pass (positions + wrapping) ----
    float x = x0;
    float y = oy + 96.0f * S + fontSz + 4.0f * S;   // below the header line
    const float headerY = oy + 96.0f * S;
    for (int i = 0; i < total; ++i) {
        Cell& c = cells[i];
        float w = textW(c.prefix) + textW(c.text);
        for (char ch : c.icons) w += iconW(ch);
        if (w < iconH) w = iconH;
        if (x + w > xMax && x > x0) { x = x0; y += cellH + lineGap; }
        c.x = x; c.y = y; c.w = w;
        x += w + cellGap;
    }

    // ---- strips (one translucent band per wrapped line, CCCaster-style) ----
    {
        float lineY = -1.0f, lineEnd = 0.0f;
        auto flush = [&]() {
            if (lineY >= 0.0f) {
                dl->AddRectFilled(ImVec2(x0 - 4.0f * S, lineY - pad),
                                  ImVec2(lineEnd + 4.0f * S, lineY + cellH + pad),
                                  IM_COL32(0, 0, 0, 165), 3.0f * S);
            }
        };
        for (const Cell& c : cells) {
            if (c.y != lineY) { flush(); lineY = c.y; lineEnd = c.x + c.w; }
            else if (c.x + c.w > lineEnd) lineEnd = c.x + c.w;
        }
        flush();
    }

    // Header (progress) on its own mini-strip.
    {
        char b[32];
        const int shownStep = cur < total ? cur + 1 : total;
        _snprintf_s(b, sizeof(b), _TRUNCATE, "%s %d/%d",
                    tutorial ? "LESSON" : "TRIAL", shownStep, total);
        const float w = font->CalcTextSizeA(fontSz, FLT_MAX, 0.0f, b).x;
        dl->AddRectFilled(ImVec2(x0 - 4.0f * S, headerY - 2.0f * S),
                          ImVec2(x0 + w + 4.0f * S, headerY + fontSz + 2.0f * S),
                          IM_COL32(0, 0, 0, 165), 3.0f * S);
        dl->AddText(font, fontSz, ImVec2(x0, headerY),
                    IM_COL32(105, 235, 235, 255), b);   // custom-menu cyan accent
    }

    // ---- draw pass ----
    const ImU32 kTintDone    = IM_COL32(135, 235, 150, 235);  // cleared = green
    const ImU32 kTintCurrent = IM_COL32(255, 255, 255, 255);
    const ImU32 kTintNext    = IM_COL32(255, 255, 255, 175);
    const ImU32 kTintFailed  = IM_COL32(255, 110, 110, 255);
    const ImU32 kSepCol      = IM_COL32(150, 160, 175, 190);  // step divider chevron
    for (int i = 0; i < total; ++i) {
        const Cell& c = cells[i];

        // Divider between consecutive steps on the same line: a small chevron
        // centered in the gap - the CCCaster "next input" separator.
        if (i > 0 && cells[i - 1].y == c.y) {
            const float gapL = cells[i - 1].x + cells[i - 1].w;
            const float sepW = font->CalcTextSizeA(fontSz, FLT_MAX, 0.0f, ">").x;
            dl->AddText(font, fontSz,
                        ImVec2(gapL + (c.x - gapL - sepW) * 0.5f,
                               c.y + (cellH - fontSz) * 0.5f),
                        kSepCol, ">");
        }
        ImU32 tint = kTintNext;
        if (i == failed)               tint = kTintFailed;
        else if (i < cur)              tint = kTintDone;
        else if (i == cur)             tint = kTintCurrent;

        float ix = c.x;
        const float ty = c.y + (cellH - fontSz) * 0.5f;
        if (!c.prefix.empty()) {
            dl->AddText(font, fontSz, ImVec2(ix, ty), tint, c.prefix.c_str());
            ix += textW(c.prefix);
        }
        const float iy = c.y + pad;
        for (char ch : c.icons) {
            const float w = iconW(ch);
            auto it = g_icons.find(ch);
            if (it != g_icons.end() && it->second.tex) {
                dl->AddImage(reinterpret_cast<ImTextureID>(it->second.tex),
                             ImVec2(ix, iy), ImVec2(ix + w, iy + iconH),
                             ImVec2(0, 0), ImVec2(1, 1), tint);
            } else {
                char b[2] = { ch, 0 };
                dl->AddText(font, fontSz, ImVec2(ix + 1.0f * S, ty), tint, b);
            }
            ix += w;
        }
        if (!c.text.empty()) {
            dl->AddText(font, fontSz, ImVec2(ix + 1.0f * S, ty), tint, c.text.c_str());
        }

        // Current-step underline: cyan normally and yellow while a delayed hit
        // is pending. The latched failed step keeps its own red underline even
        // after progress has reset to step 0 for the retry.
        if (i == cur || i == failed) {
            ImU32 bar = IM_COL32(105, 235, 235, 230);
            if (i == failed) bar = IM_COL32(255, 90, 90, 240);
            else if (armed) bar = IM_COL32(255, 235, 110, 240);
            dl->AddRectFilled(ImVec2(c.x - 1.0f * S, c.y + cellH + 1.0f * S),
                              ImVec2(c.x + c.w + 1.0f * S, c.y + cellH + 2.5f * S),
                              bar);
        }
    }
}

void ReleaseTextures() {
    for (auto& kv : g_icons) if (kv.second.tex) kv.second.tex->Release();
    g_icons.clear();
    g_iconsTried = false;
}

} // namespace Mission::Render
