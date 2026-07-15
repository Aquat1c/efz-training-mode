#include "../../../include/game/mission/mission_render.h"
#include "../../../include/game/mission/mission_engine.h"
#include "../../../include/game/mission/mission_data.h"
#include "../../../include/game/practice_menu/mission_title_screen.h"

#include "../../../3rdparty/imgui/imgui.h"

#include <windows.h>
#include <d3d9.h>
#include <gdiplus.h>

#include <cfloat>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#pragma comment(lib, "gdiplus.lib")

// M3 scaffold: draw the active mission's combo recipe as a row of status-colored
// step boxes with control icons (assets/controls). Icons are decoded once via
// GDI+ into D3D9 textures (mirroring gif_player.cpp) from the live device.

namespace Mission::Render {

namespace {

// A glyph is a sub-rect of the single shared atlas texture (g_atlasTex). Storing
// UVs instead of a per-glyph texture lets ImGui batch every AddImage into ONE draw
// command - a long trial recipe was ~120 texture switches (draw calls) per frame.
struct Icon { float u0 = 0, v0 = 0, u1 = 1, v1 = 1; UINT w = 0, h = 0; };

ULONG_PTR g_gdiplusToken = 0;
bool g_gdiplusOk = false;
bool g_iconsTried = false;   // attempted a load this device-session
std::map<char, Icon> g_icons;
IDirect3DTexture9* g_atlasTex = nullptr;   // single texture holding every glyph
IDirect3DDevice9* g_atlasDevice = nullptr; // non-owning; texture owns its device
uint64_t g_iconLayoutGeneration = 1;
// Font-atlas rebuilds can recycle an ImFont allocation at the same address.
// Keep an explicit generation in every font-dependent cache key so pointer and
// metric equality can never resurrect placement computed before an invalidate.
uint64_t g_layoutInvalidationGeneration = 1;

void ResetIconAtlas() {
    if (g_atlasTex) {
        g_atlasTex->Release();
        g_atlasTex = nullptr;
    }
    g_icons.clear();
    g_iconsTried = false;
    g_atlasDevice = nullptr;
    ++g_iconLayoutGeneration;
}

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

// Load a glyph file into a BGRA pixel buffer (top-down). No GPU texture here -
// EnsureIcons packs all glyphs into one atlas texture.
struct GlyphPixels { char key = 0; std::vector<uint32_t> px; UINT w = 0, h = 0; };
bool LoadGlyphPixels(const std::wstring& file, GlyphPixels& out) {
    Gdiplus::Bitmap bmp(file.c_str());
    if (bmp.GetLastStatus() != Gdiplus::Ok) return false;
    const UINT w = bmp.GetWidth(), h = bmp.GetHeight();
    if (!w || !h) return false;
    Gdiplus::Rect rect(0, 0, static_cast<INT>(w), static_cast<INT>(h));
    Gdiplus::BitmapData data{};
    if (bmp.LockBits(&rect, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &data) != Gdiplus::Ok)
        return false;
    out.px.resize(static_cast<size_t>(w) * h);
    const BYTE* scan0 = reinterpret_cast<const BYTE*>(data.Scan0);
    for (UINT y = 0; y < h; ++y) {
        const BYTE* src = data.Stride >= 0
            ? scan0 + static_cast<size_t>(y) * data.Stride
            : scan0 + static_cast<size_t>(h - 1 - y) * static_cast<size_t>(-data.Stride);
        memcpy(&out.px[static_cast<size_t>(y) * w], src, static_cast<size_t>(w) * sizeof(uint32_t));
    }
    bmp.UnlockBits(&data);
    out.w = w; out.h = h;
    return true;
}

void EnsureIcons(IDirect3DDevice9* dev) {
    if (g_atlasDevice && g_atlasDevice != dev) {
        // The fallback and live game renderers can be backed by different D3D
        // devices. A managed texture survives Reset on one device, but it must
        // never be submitted to another.
        ResetIconAtlas();
    }
    if (g_iconsTried || !dev) return;
    g_atlasDevice = dev;
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
    // Load every glyph into pixel buffers.
    std::vector<GlyphPixels> glyphs;
    glyphs.reserve(sizeof(entries) / sizeof(entries[0]));
    for (const Entry& e : entries) {
        GlyphPixels g; g.key = e.key;
        if (LoadGlyphPixels(dir + e.file, g)) glyphs.push_back(std::move(g));
    }
    if (glyphs.empty()) return;

    // Pack horizontally into one atlas (1px transparent gutter avoids filter bleed).
    constexpr UINT kPad = 1;
    UINT atlasW = 0, atlasH = 0;
    for (const GlyphPixels& g : glyphs) { atlasW += g.w + kPad; atlasH = (std::max)(atlasH, g.h); }
    if (atlasW == 0 || atlasH == 0) return;

    IDirect3DTexture9* atlas = nullptr;
    if (FAILED(dev->CreateTexture(atlasW, atlasH, 1, 0, D3DFMT_A8R8G8B8,
                                  D3DPOOL_MANAGED, &atlas, nullptr))) return;
    D3DLOCKED_RECT lr{};
    if (FAILED(atlas->LockRect(0, &lr, nullptr, 0))) { atlas->Release(); return; }
    // Clear to transparent, then blit each glyph and record its UV sub-rect.
    for (UINT y = 0; y < atlasH; ++y)
        memset(reinterpret_cast<BYTE*>(lr.pBits) + static_cast<size_t>(y) * lr.Pitch,
               0, static_cast<size_t>(atlasW) * sizeof(uint32_t));
    UINT x = 0;
    for (const GlyphPixels& g : glyphs) {
        for (UINT y = 0; y < g.h; ++y) {
            memcpy(reinterpret_cast<BYTE*>(lr.pBits) + static_cast<size_t>(y) * lr.Pitch
                       + static_cast<size_t>(x) * sizeof(uint32_t),
                   &g.px[static_cast<size_t>(y) * g.w], static_cast<size_t>(g.w) * sizeof(uint32_t));
        }
        Icon ic;
        ic.u0 = static_cast<float>(x) / atlasW;
        ic.v0 = 0.0f;
        ic.u1 = static_cast<float>(x + g.w) / atlasW;
        ic.v1 = static_cast<float>(g.h) / atlasH;
        ic.w = g.w; ic.h = g.h;
        g_icons[g.key] = ic;
        x += g.w + kPad;
    }
    atlas->UnlockRect(0);
    g_atlasTex = atlas;
}

// Split a notation into a shown position PREFIX ("j.", "jj", "c.", "f."), icon
// tokens, and a text suffix. CASE-SENSITIVE parsing of the first whitespace
// token only ("236236 Lv1 (S)" -> icons "236236", text "Lv1 (S)"). Digits 1-9
// (5 = generated neutral tile) and UPPERCASE A/B/C/D/S map to icons; the first
// unrecognized uppercase (I of "IC", F of "FM") turns the rest of the head into
// text - so "c.5B" is "c." + [B] and "j.IC" is "j." + text "IC".
void SplitNotation(const std::string& notation, std::string& prefixOut,
                   std::string& iconsOut, std::string& suffixOut) {
    prefixOut.clear(); iconsOut.clear(); suffixOut.clear();
    std::string source = notation;
    if (!source.empty() && source[0] == '~') {
        prefixOut = "~ ";
        source.erase(0, 1);
        const size_t first = source.find_first_not_of(" \t");
        source = first == std::string::npos ? std::string() : source.substr(first);
    }
    std::string upper = source;
    for (char& ch : upper) {
        ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    }
    if (upper == "IC" || upper == "BIC" || upper == "FIC" ||
        upper == "RG" || upper == "FM" || upper == "RED IC" ||
        upper == "BLUE IC") {
        suffixOut = source;
        return;
    }

    const size_t sp = source.find(' ');
    const std::string head = source.substr(0, sp);
    if (sp != std::string::npos) suffixOut = source.substr(sp + 1);
    size_t i = 0;
    static const char* const knownPrefixes[] = {"dj.", "jc.", "j.", "c.", "f."};
    for (const char* known : knownPrefixes) {
        const size_t length = strlen(known);
        if (head.compare(0, length, known) == 0) {
            prefixOut += known;
            i = length;
            break;
        }
    }
    for (; i < head.size(); ++i) {
        const char c = head[i];
        if (c >= '1' && c <= '9') {
            iconsOut.push_back(c);
        } else if (c == 'A' || c == 'B' || c == 'C' || c == 'D' || c == 'S') {
            iconsOut.push_back(c);
        } else {
            // Unrecognized (I of IC, F of FM, '/', 'x', ...): rest of head = text.
            const std::string rest = head.substr(i);
            suffixOut = suffixOut.empty() ? rest : rest + " " + suffixOut;
            break;
        }
    }
    // Neutral normals show the button only: drop a leading neutral '5' when a button
    // (or another icon) follows it ("5A" -> "A", "j.5B" -> "j.B"). A lone "5" is kept.
    if (iconsOut.size() > 1 && iconsOut[0] == '5') iconsOut.erase(0, 1);
    if (prefixOut.empty() && iconsOut.empty() && suffixOut.empty() && !head.empty())
        suffixOut = head;
}

enum class RichPieceKind { Text, Icon };

struct RichPiece {
    RichPieceKind kind = RichPieceKind::Text;
    std::string text;
    char icon = 0;
    bool accent = false;
};

struct RichAtom {
    std::vector<RichPiece> pieces;
    bool space = false;
    bool newline = false;
};

struct PositionedRichPiece {
    size_t atom = 0;
    size_t piece = 0;
    float x = 0.0f;
    float lineY = 0.0f;
    float width = 0.0f;
};

struct CachedRichLayout {
    ImFont* font = nullptr;
    uint64_t iconGeneration = 0;
    float fontFingerprint = 0.0f;
    float fontPx = 0.0f;
    float maxWidth = 0.0f;
    float iconHeight = 0.0f;
    float lineHeight = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
    std::vector<PositionedRichPiece> pieces;
};

struct CachedRichDocument {
    std::vector<RichAtom> atoms;
    std::vector<CachedRichLayout> layouts;
};

// Fixed-size identity for the ordinary Mission recipe layout.  The previous
// cache used a formatted mission-name string, which both allocated on the hot
// path and aliased same-name recipes, recycled fonts, and letterbox origins.
// Hash the complete Step payload without constructing a serialization, then
// keep every non-recipe input that affects CalcTextSizeA or cell placement as a
// directly comparable scalar.
struct RecipeFingerprint {
    uint64_t first = UINT64_C(1469598103934665603);
    uint64_t second = UINT64_C(1099511628211) ^ UINT64_C(0x9E3779B97F4A7C15);

    void AddByte(uint8_t value) {
        first ^= value;
        first *= UINT64_C(1099511628211);
        second ^= static_cast<uint64_t>(value) + UINT64_C(0x9D);
        second *= UINT64_C(14029467366897019727);
        second ^= second >> 29;
    }

    void AddU64(uint64_t value) {
        for (unsigned shift = 0; shift < 64; shift += 8) {
            AddByte(static_cast<uint8_t>(value >> shift));
        }
    }

    void AddI32(int value) {
        AddU64(static_cast<uint64_t>(static_cast<int64_t>(value)));
    }

    void AddBool(bool value) {
        AddByte(value ? uint8_t{1} : uint8_t{0});
    }

    void AddString(const std::string& value) {
        AddU64(static_cast<uint64_t>(value.size()));
        for (unsigned char ch : value) AddByte(ch);
    }
};

RecipeFingerprint FingerprintRecipe(const ::Mission::Mission& mission) {
    RecipeFingerprint fingerprint;
    fingerprint.AddU64(static_cast<uint64_t>(mission.steps.size()));
    for (const ::Mission::Step& step : mission.steps) {
        fingerprint.AddString(step.notation);
        fingerprint.AddU64(static_cast<uint64_t>(step.moveIds.size()));
        for (int moveId : step.moveIds) fingerprint.AddI32(moveId);
        fingerprint.AddByte(static_cast<uint8_t>(step.req));
        fingerprint.AddI32(step.hitsRequired);
        fingerprint.AddBool(step.optional);
        fingerprint.AddI32(step.maxDelay);
        fingerprint.AddI32(step.maxGap);
        fingerprint.AddBool(step.comboEndAfter);
        fingerprint.AddI32(step.charState);
        fingerprint.AddI32(step.damage);
    }
    return fingerprint;
}

uint32_t FloatIdentity(float value) {
    uint32_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value), "float identity assumes 32-bit float");
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

struct RecipeLayoutKey {
    uint64_t recipeFirst = 0;
    uint64_t recipeSecond = 0;
    uint64_t layoutGeneration = 0;
    uint64_t iconGeneration = 0;
    uintptr_t fontIdentity = 0;
    uint32_t fontSize = 0;
    uint32_t fontAdvanceM = 0;
    uint32_t fontAdvanceZero = 0;
    uint32_t originX = 0;
    uint32_t originY = 0;
    uint32_t scale = 0;
    int stepCount = 0;
    int currentStep = 0;
    int currentHits = 0;
    bool armed = false;

    bool operator==(const RecipeLayoutKey& other) const {
        return recipeFirst == other.recipeFirst &&
               recipeSecond == other.recipeSecond &&
               layoutGeneration == other.layoutGeneration &&
               iconGeneration == other.iconGeneration &&
               fontIdentity == other.fontIdentity &&
               fontSize == other.fontSize &&
               fontAdvanceM == other.fontAdvanceM &&
               fontAdvanceZero == other.fontAdvanceZero &&
               originX == other.originX &&
               originY == other.originY &&
               scale == other.scale &&
               stepCount == other.stepCount &&
               currentStep == other.currentStep &&
               currentHits == other.currentHits &&
               armed == other.armed;
    }
};

// Tutorial copy is immutable for a whole lesson, while the same page/task is
// rendered for hundreds of frames. Keep parsed atoms and line placement here
// so tint/progress changes do not allocate, tokenize, wrap, or measure again.
// The cache is deliberately bounded; clearing it is cheaper and safer than an
// unbounded content cache for user-authored mission packs.
constexpr size_t kMaxRichDocuments = 512;
constexpr size_t kMaxLayoutsPerDocument = 12;
std::map<std::string, CachedRichDocument> g_richDocuments;

void AppendRichAtom(std::vector<RichAtom>& out, RichAtom atom, bool glue) {
    if (atom.pieces.empty()) return;
    if (glue && !out.empty() && !out.back().space && !out.back().newline) {
        std::vector<RichPiece>& destination = out.back().pieces;
        destination.insert(destination.end(),
                           std::make_move_iterator(atom.pieces.begin()),
                           std::make_move_iterator(atom.pieces.end()));
        return;
    }
    out.push_back(std::move(atom));
}

void AppendTextAtoms(std::vector<RichAtom>& out, const std::string& text,
                     bool accent = false, bool glueFirst = false) {
    std::string word;
    bool glue = glueFirst;
    auto flush = [&]() {
        if (word.empty()) return;
        RichAtom atom;
        RichPiece piece;
        piece.text.swap(word);
        piece.accent = accent;
        atom.pieces.push_back(std::move(piece));
        AppendRichAtom(out, std::move(atom), glue);
        glue = false;
    };
    for (char ch : text) {
        if (ch == '\n') {
            flush();
            RichAtom atom; atom.newline = true; out.push_back(atom);
            glue = false;
        } else if (std::isspace(static_cast<unsigned char>(ch))) {
            flush();
            if (out.empty() || (!out.back().space && !out.back().newline)) {
                RichAtom atom; atom.space = true; out.push_back(atom);
            }
            glue = false;
        } else {
            word.push_back(ch);
        }
    }
    flush();
}

RichAtom NotationAtom(const std::string& notation) {
    RichAtom atom;
    std::string prefix, icons, suffix;
    SplitNotation(notation, prefix, icons, suffix);
    if (!prefix.empty()) atom.pieces.push_back({RichPieceKind::Text, prefix, 0, false});
    for (char icon : icons) atom.pieces.push_back({RichPieceKind::Icon, {}, icon, false});
    if (!suffix.empty()) {
        if (!atom.pieces.empty()) {
            atom.pieces.push_back({RichPieceKind::Text, " ", 0, false});
        }
        atom.pieces.push_back({RichPieceKind::Text, suffix, 0, false});
    }
    if (atom.pieces.empty()) atom.pieces.push_back({RichPieceKind::Text, notation, 0, false});
    return atom;
}

std::vector<RichAtom> ParseRichAtoms(const std::string& source) {
    std::vector<RichAtom> out;
    std::string plain;
    bool plainGlue = false;
    bool glueNextPlain = false;
    auto appendPlain = [&](char ch) {
        if (plain.empty()) {
            plainGlue = glueNextPlain;
            glueNextPlain = false;
        }
        plain.push_back(ch);
    };
    auto flushPlain = [&]() {
        AppendTextAtoms(out, plain, false, plainGlue);
        plain.clear();
        plainGlue = false;
    };
    for (size_t i = 0; i < source.size();) {
        if (source[i] == '{' && i + 1 < source.size() && source[i + 1] == '{') {
            appendPlain('{'); i += 2; continue;
        }
        if (source[i] == '}' && i + 1 < source.size() && source[i + 1] == '}') {
            appendPlain('}'); i += 2; continue;
        }
        if (source[i] != '{') { appendPlain(source[i++]); continue; }
        const size_t close = source.find('}', i + 1);
        if (close == std::string::npos) { appendPlain(source[i++]); continue; }
        const bool glueToken = i > 0 &&
            !std::isspace(static_cast<unsigned char>(source[i - 1]));
        flushPlain();
        const std::string token = source.substr(i + 1, close - i - 1);
        const size_t colon = token.find(':');
        const std::string kind = colon == std::string::npos ? token : token.substr(0, colon);
        const std::string value = colon == std::string::npos ? std::string() : token.substr(colon + 1);
        if (kind == "dir" || kind == "btn" || kind == "input") {
            AppendRichAtom(out, NotationAtom(value), glueToken);
        } else if (kind == "term") {
            const size_t bar = value.find('|');
            RichAtom atom;
            atom.pieces.push_back({
                RichPieceKind::Text,
                bar == std::string::npos ? value
                    : value.substr(0, bar) + " (" + value.substr(bar + 1) + ")",
                0, true});
            AppendRichAtom(out, std::move(atom), glueToken);
        } else if (kind == "ui") {
            std::string label = value;
            for (char& ch : label) ch = static_cast<char>(
                std::toupper(static_cast<unsigned char>(ch)));
            RichAtom atom;
            atom.pieces.push_back(
                {RichPieceKind::Text, "[" + label + "]", 0, true});
            AppendRichAtom(out, std::move(atom), glueToken);
        } else {
            RichAtom atom;
            atom.pieces.push_back(
                {RichPieceKind::Text, "{" + token + "}", 0, true});
            AppendRichAtom(out, std::move(atom), glueToken);
        }
        glueNextPlain = close + 1 < source.size() &&
            !std::isspace(static_cast<unsigned char>(source[close + 1]));
        i = close + 1;
    }
    flushPlain();
    return out;
}

float IconWidth(char icon, float iconHeight) {
    if (icon == '5') return iconHeight;
    const auto it = g_icons.find(icon);
    if (it != g_icons.end() && it->second.h > 0) {
        return iconHeight * static_cast<float>(it->second.w) /
               static_cast<float>(it->second.h);
    }
    return iconHeight * 0.78f;
}

float PieceWidth(ImFont* font, float fontPx, float iconHeight,
                 const RichPiece& piece) {
    if (piece.kind == RichPieceKind::Icon) return IconWidth(piece.icon, iconHeight);
    return piece.text.empty() ? 0.0f
        : font->CalcTextSizeA(fontPx, FLT_MAX, 0.0f, piece.text.c_str()).x;
}

CachedRichDocument& RichDocument(const std::string& source) {
    auto it = g_richDocuments.find(source);
    if (it != g_richDocuments.end()) return it->second;
    if (g_richDocuments.size() >= kMaxRichDocuments) g_richDocuments.clear();
    CachedRichDocument doc;
    doc.atoms = ParseRichAtoms(source);
    return g_richDocuments.emplace(source, std::move(doc)).first->second;
}

CachedRichLayout& RichLayout(CachedRichDocument& doc, ImFont* font,
                             float fontPx, float maxWidth) {
    const float fontFingerprint = font->FontSize +
        font->GetCharAdvance(static_cast<ImWchar>('M')) * 3.0f +
        font->GetCharAdvance(static_cast<ImWchar>('0')) * 5.0f;
    for (CachedRichLayout& layout : doc.layouts) {
        if (layout.font == font &&
            layout.iconGeneration == g_iconLayoutGeneration &&
            std::abs(layout.fontPx - fontPx) < 0.01f &&
            std::abs(layout.maxWidth - maxWidth) < 0.01f &&
            std::abs(layout.fontFingerprint - fontFingerprint) < 0.01f) {
            return layout;
        }
    }
    if (doc.layouts.size() >= kMaxLayoutsPerDocument) {
        doc.layouts.erase(doc.layouts.begin());
    }

    CachedRichLayout layout;
    layout.font = font;
    layout.iconGeneration = g_iconLayoutGeneration;
    layout.fontFingerprint = fontFingerprint;
    layout.fontPx = fontPx;
    layout.maxWidth = maxWidth;
    layout.iconHeight = (std::max)(12.0f, fontPx + 3.0f);
    layout.lineHeight = (std::max)(fontPx, layout.iconHeight);
    const float lineGap = 3.0f;
    const float spaceWidth = font->CalcTextSizeA(fontPx, FLT_MAX, 0.0f, " ").x;
    float x = 0.0f;
    float y = 0.0f;
    float maxUsedWidth = 0.0f;
    bool haveContent = false;

    for (size_t atomIndex = 0; atomIndex < doc.atoms.size(); ++atomIndex) {
        const RichAtom& atom = doc.atoms[atomIndex];
        if (atom.newline) {
            maxUsedWidth = (std::max)(maxUsedWidth, x);
            x = 0.0f;
            y += layout.lineHeight + lineGap;
            haveContent = true;
            continue;
        }
        if (atom.space) {
            if (x > 0.0f) x += spaceWidth;
            continue;
        }

        std::vector<float> widths;
        widths.reserve(atom.pieces.size());
        float atomWidth = 0.0f;
        for (const RichPiece& piece : atom.pieces) {
            const float width = PieceWidth(font, fontPx, layout.iconHeight, piece);
            widths.push_back(width);
            atomWidth += width;
        }
        if (x > 0.0f && x + atomWidth > maxWidth) {
            x = 0.0f;
            y += layout.lineHeight + lineGap;
        }
        float pieceX = x;
        for (size_t pieceIndex = 0; pieceIndex < atom.pieces.size(); ++pieceIndex) {
            layout.pieces.push_back(
                {atomIndex, pieceIndex, pieceX, y, widths[pieceIndex]});
            pieceX += widths[pieceIndex];
        }
        x += atomWidth;
        maxUsedWidth = (std::max)(maxUsedWidth, x);
        haveContent = true;
    }
    layout.width = maxUsedWidth;
    layout.height = haveContent ? y + layout.lineHeight : 0.0f;
    doc.layouts.push_back(std::move(layout));
    return doc.layouts.back();
}

void DrawNeutralTile(ImDrawList* dl, ImFont* font, float fontPx,
                     float x, float y, float size, ImU32 tint) {
    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + size, y + size),
                      IM_COL32(15, 40, 44, 235), 2.0f);
    dl->AddRect(ImVec2(x, y), ImVec2(x + size, y + size),
                IM_COL32(105, 235, 235, 230), 2.0f, 0, 1.0f);
    const float w = font->CalcTextSizeA(fontPx, FLT_MAX, 0.0f, "5").x;
    dl->AddText(font, fontPx, ImVec2(x + (size - w) * 0.5f,
                y + (size - fontPx) * 0.5f), tint, "5");
}

float LayoutRichText(IDirect3DDevice9* dev, ImDrawList* dl, ImFont* font,
                     float fontPx, float x0, float y0, ImU32 color,
                     const std::string& text, float maxWidth, bool draw) {
    if (!font || text.empty() || maxWidth <= 0.0f) return 0.0f;
    EnsureIcons(dev);
    CachedRichDocument& doc = RichDocument(text);
    CachedRichLayout& layout = RichLayout(doc, font, fontPx, maxWidth);
    if (!draw || !dl) return layout.height;

    for (const PositionedRichPiece& placed : layout.pieces) {
        const RichPiece& piece = doc.atoms[placed.atom].pieces[placed.piece];
        if (piece.kind == RichPieceKind::Icon) {
            const float iconY = y0 + placed.lineY +
                                (layout.lineHeight - layout.iconHeight) * 0.5f;
            if (piece.icon == '5') {
                DrawNeutralTile(dl, font,
                                (std::min)(fontPx, layout.iconHeight - 2.0f),
                                x0 + placed.x, iconY, layout.iconHeight, color);
            } else {
                const auto it = g_icons.find(piece.icon);
                if (it != g_icons.end() && g_atlasTex) {
                    dl->AddImage(reinterpret_cast<ImTextureID>(g_atlasTex),
                                 ImVec2(x0 + placed.x, iconY),
                                 ImVec2(x0 + placed.x + placed.width,
                                        iconY + layout.iconHeight),
                                 ImVec2(it->second.u0, it->second.v0),
                                 ImVec2(it->second.u1, it->second.v1), color);
                } else {
                    const char glyph[2] = {piece.icon, 0};
                    dl->AddText(font, fontPx,
                                ImVec2(x0 + placed.x,
                                       y0 + placed.lineY +
                                       (layout.lineHeight - fontPx) * 0.5f),
                                color, glyph);
                }
            }
        } else {
            const ImU32 pieceColor = piece.accent
                ? IM_COL32(130, 235, 235, 255) : color;
            dl->AddText(font, fontPx,
                        ImVec2(x0 + placed.x,
                               y0 + placed.lineY +
                               (layout.lineHeight - fontPx) * 0.5f),
                        pieceColor, piece.text.c_str());
        }
    }
    return layout.height;
}

} // namespace

float MeasureRichTextHeight(void* device, ImFont* font, float fontPx,
                            const std::string& text, float maxWidth) {
    return MeasureRichText(device, font, fontPx, text, maxWidth).height;
}

RichTextMetrics MeasureRichText(void* device, ImFont* font, float fontPx,
                                const std::string& text, float maxWidth) {
    IDirect3DDevice9* dev = reinterpret_cast<IDirect3DDevice9*>(device);
    ImFont* resolvedFont = font ? font : ImGui::GetFont();
    if (!resolvedFont || text.empty() || maxWidth <= 0.0f) return {};
    EnsureIcons(dev);
    CachedRichDocument& doc = RichDocument(text);
    CachedRichLayout& layout = RichLayout(
        doc, resolvedFont, fontPx, maxWidth);
    RichTextMetrics metrics;
    metrics.width = layout.width;
    metrics.height = layout.height;
    return metrics;
}

void DrawRichText(void* device, ImDrawList* dl, ImFont* font, float fontPx,
                  float x, float y, unsigned int color,
                  const std::string& text, float maxWidth) {
    if (!dl) return;
    IDirect3DDevice9* dev = reinterpret_cast<IDirect3DDevice9*>(device);
    (void)LayoutRichText(dev, dl, font ? font : ImGui::GetFont(), fontPx,
                         x, y, static_cast<ImU32>(color), text, maxWidth, true);
}

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

    struct Cell { std::string prefix, icons, text; float x = 0, y = 0, w = 0, pw = 0; };

    // Aspect-correct icon width at the target height (a map lookup; used by the
    // cached layout AND the per-frame draw pass).
    auto iconW = [&](char c) -> float {
        if (c == '5') return iconH;
        auto it = g_icons.find(c);
        if (it != g_icons.end() && it->second.h > 0) {
            return iconH * (static_cast<float>(it->second.w) / static_cast<float>(it->second.h));
        }
        return iconH * 0.75f;   // fallback glyph slot
    };

    // CACHE the laid-out cells. SplitNotation + a CalcTextSizeA font measurement for
    // EVERY step, redone every render frame, is the trial-specific render-thread cost
    // behind the "FPS dip in the note trial" (a 30-step recorded combo does 30x that
    // work per frame). The fixed key hashes the complete recipe without allocating
    // and directly identifies all placement/font/icon inputs. Only tint is excluded.
    const RecipeFingerprint recipeFingerprint = FingerprintRecipe(*m);
    RecipeLayoutKey layoutKey;
    layoutKey.recipeFirst = recipeFingerprint.first;
    layoutKey.recipeSecond = recipeFingerprint.second;
    layoutKey.layoutGeneration = g_layoutInvalidationGeneration;
    layoutKey.iconGeneration = g_iconLayoutGeneration;
    layoutKey.fontIdentity = reinterpret_cast<uintptr_t>(font);
    layoutKey.fontSize = FloatIdentity(font->FontSize);
    layoutKey.fontAdvanceM = FloatIdentity(
        font->GetCharAdvance(static_cast<ImWchar>('M')));
    layoutKey.fontAdvanceZero = FloatIdentity(
        font->GetCharAdvance(static_cast<ImWchar>('0')));
    layoutKey.originX = FloatIdentity(ox);
    layoutKey.originY = FloatIdentity(oy);
    layoutKey.scale = FloatIdentity(scale);
    layoutKey.stepCount = total;
    layoutKey.currentStep = cur;
    layoutKey.currentHits = curHits;
    layoutKey.armed = armed;

    static RecipeLayoutKey s_layoutKey;
    static bool s_haveLayoutKey = false;
    static std::vector<Cell> s_cells;
    {
        if (!s_haveLayoutKey || !(s_layoutKey == layoutKey)) {
            s_layoutKey = layoutKey;
            s_haveLayoutKey = true;
            s_cells.assign(total, Cell{});
            for (int i = 0; i < total; ++i) {
                const ::Mission::Step& st = m->steps[i];
                std::string note = st.notation;
                if (note.empty()) {   // freshly recorded, unnamed: show the move-id
                    char b[16];
                    _snprintf_s(b, sizeof(b), _TRUNCATE, "#%d",
                                st.moveIds.empty() ? 0 : st.moveIds[0]);
                    note = b;
                }
                SplitNotation(note, s_cells[i].prefix, s_cells[i].icons, s_cells[i].text);
                if (i > 0 && m->steps[i - 1].comboEndAfter) {
                    // Explicit recovery boundary: a new combo segment/setup, not an
                    // ordinary link whose delay should force a retry.
                    s_cells[i].prefix = "[SETUP] " + s_cells[i].prefix;
                }
                if (st.req == ::Mission::StepReq::Hits && st.hitsRequired > 1) {
                    char b[16];
                    if (i == cur && armed)
                        _snprintf_s(b, sizeof(b), _TRUNCATE, " %d/%d", curHits, st.hitsRequired);
                    else
                        _snprintf_s(b, sizeof(b), _TRUNCATE, " x%d", st.hitsRequired);
                    s_cells[i].text += b;
                }
                if (st.maxDelay > 0) s_cells[i].prefix = "~" + s_cells[i].prefix; // delayed hit
                if (st.optional)     s_cells[i].text += "?";                       // optional
            }
            // ---- layout pass (positions + wrapping) ----
            auto textW = [&](const std::string& t) -> float {
                if (t.empty()) return 0.0f;
                return font->CalcTextSizeA(fontSz, FLT_MAX, 0.0f, t.c_str()).x + 1.0f * S;
            };
            float x = x0;
            float y = oy + 96.0f * S + fontSz + 4.0f * S;   // below the header line
            for (int i = 0; i < total; ++i) {
                Cell& c = s_cells[i];
                c.pw = textW(c.prefix);           // cached: reused by the draw pass
                float w = c.pw + textW(c.text);
                for (char ch : c.icons) w += iconW(ch);
                if (w < iconH) w = iconH;
                if (x + w > xMax && x > x0) { x = x0; y += cellH + lineGap; }
                c.x = x; c.y = y; c.w = w;
                x += w + cellGap;
            }
        }
    }
    std::vector<Cell>& cells = s_cells;
    const float headerY = oy + 96.0f * S;

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

    // ---- draw pass (batched by texture) ----
    // Interleaving glyphs (the atlas) and text (the font) per cell would break
    // ImGui's draw-command batching at every texture switch - a 30-step recipe was
    // ~120 draw calls/frame. Emit in THREE passes so all glyphs share one command
    // and all text shares one: backgrounds -> glyphs (atlas) -> text (font).
    const ImU32 kTintDone    = IM_COL32(135, 235, 150, 235);  // cleared = green
    const ImU32 kTintCurrent = IM_COL32(255, 255, 255, 255);
    const ImU32 kTintNext    = IM_COL32(255, 255, 255, 175);
    const ImU32 kTintFailed  = IM_COL32(255, 110, 110, 255);
    const ImU32 kSepCol      = IM_COL32(150, 160, 175, 190);  // step divider chevron
    auto tintOf = [&](int i) -> ImU32 {
        if (i == failed) return kTintFailed;
        if (i < cur)     return kTintDone;
        if (i == cur)    return kTintCurrent;
        return kTintNext;
    };

    // Pass 1: backgrounds (current/failed step underline) - AddRectFilled, no texture.
    for (int i = 0; i < total; ++i) {
        if (i != cur && i != failed) continue;
        const Cell& c = cells[i];
        ImU32 bar = IM_COL32(105, 235, 235, 230);
        if (i == failed) bar = IM_COL32(255, 90, 90, 240);
        else if (armed)  bar = IM_COL32(255, 235, 110, 240);
        dl->AddRectFilled(ImVec2(c.x - 1.0f * S, c.y + cellH + 1.0f * S),
                          ImVec2(c.x + c.w + 1.0f * S, c.y + cellH + 2.5f * S), bar);
    }

    // Pass 2: glyphs - every AddImage uses the one atlas texture -> a single batch.
    for (int i = 0; i < total; ++i) {
        const Cell& c = cells[i];
        const ImU32 tint = tintOf(i);
        const float iy = c.y + pad;
        float ix = c.x + c.pw;   // prefix is drawn in the text pass
        for (char ch : c.icons) {
            const float w = iconW(ch);
            auto it = g_icons.find(ch);
            if (ch == '5') {
                DrawNeutralTile(dl, font, (std::min)(fontSz, iconH - 2.0f), ix, iy, iconH, tint);
            } else if (it != g_icons.end() && g_atlasTex) {
                dl->AddImage(reinterpret_cast<ImTextureID>(g_atlasTex),
                             ImVec2(ix, iy), ImVec2(ix + w, iy + iconH),
                             ImVec2(it->second.u0, it->second.v0),
                             ImVec2(it->second.u1, it->second.v1), tint);
            }
            ix += w;
        }
    }

    // Pass 3: text - prefix, step-divider chevrons, unknown-glyph fallback, suffix -
    // all use the font texture -> a single batch. sepW measured once, not per divider.
    const float sepW = font->CalcTextSizeA(fontSz, FLT_MAX, 0.0f, ">").x;
    for (int i = 0; i < total; ++i) {
        const Cell& c = cells[i];
        const ImU32 tint = tintOf(i);
        const float ty = c.y + (cellH - fontSz) * 0.5f;
        if (i > 0 && cells[i - 1].y == c.y) {
            const float gapL = cells[i - 1].x + cells[i - 1].w;
            dl->AddText(font, fontSz,
                        ImVec2(gapL + (c.x - gapL - sepW) * 0.5f, ty), kSepCol, ">");
        }
        float ix = c.x;
        if (!c.prefix.empty()) {
            dl->AddText(font, fontSz, ImVec2(ix, ty), tint, c.prefix.c_str());
        }
        ix += c.pw;
        for (char ch : c.icons) {
            if (ch != '5' && g_icons.find(ch) == g_icons.end()) {   // rare: missing glyph
                char b[2] = { ch, 0 };
                dl->AddText(font, fontSz, ImVec2(ix + 1.0f * S, ty), tint, b);
            }
            ix += iconW(ch);
        }
        if (!c.text.empty()) {
            dl->AddText(font, fontSz, ImVec2(ix + 1.0f * S, ty), tint, c.text.c_str());
        }
    }
}

void InvalidateTextLayouts() {
    g_richDocuments.clear();
    ++g_layoutInvalidationGeneration;
}

void ReleaseTextures() {
    ResetIconAtlas();
    InvalidateTextLayouts();
}

} // namespace Mission::Render
