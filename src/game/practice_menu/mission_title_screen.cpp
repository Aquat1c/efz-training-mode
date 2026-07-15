#include "../../../include/game/practice_menu/mission_title_screen.h"

#include "../../../include/gui/custom_menu/layout.h"
#include "../../../include/gui/custom_menu/theme.h"
#include "../../../include/gui/custom_menu/scale.h"
#include "../../../include/game/character_hotswap.h"
#include "../../../include/game/mission/mission_engine.h"
#include "../../../include/game/mission/mission_render.h"
#include "../../../include/game/mission/tutorial_support.h"
#include "../../../include/utils/config.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace PracticeMenu::TitleScreen {

namespace {

namespace L = CustomMenu::Layout;
namespace T = CustomMenu::Theme;
using CustomMenu::Scale::Snap;

const char* const kStageNames[] = {
    "Courtyard of the Full Moon", "Snowy Park (Night)", "Lunch Break Courtyard",
    "School Road Park (Day)", "School Road Park (Night)", "Sunset Rooftop",
    "Shopping Street", "Tree of Beginnings", "Minase House (Day)", "Gymnasium",
    "Rainy Field", "Behind the School", "World of Eternity",
    "Abandoned Station (Day)", "Shrine Near the Sky (Day)", "Kamio House",
    "The Infinite Sky", "Minase House (Night)", "Abandoned Station (Dusk)",
    "Fargo Research Facility", "Monomi Hill", "Shrine Near the Sky (Night)",
    "Snowy Park (Day)",
};
constexpr int kStageNameCount = static_cast<int>(sizeof(kStageNames) / sizeof(kStageNames[0]));

const char* const kMissionTabs[] = { "TRIALS", "MISSIONS", "RECORDED", "CREATE" };
constexpr int kTabCount = 4;

// Native palette: black panels, white/gray text, steel selection bars - the
// same language as the game's own settings/netplay screens (no accent color).
constexpr ImU32 kBg             = IM_COL32(0, 0, 0, 178);
constexpr ImU32 kPanel          = IM_COL32(0, 0, 0, 218);
constexpr ImU32 kPanelStrong    = IM_COL32(0, 0, 0, 238);
constexpr ImU32 kPanelBorder    = IM_COL32(255, 255, 255, 70);
constexpr ImU32 kPanelBorderHot = IM_COL32(230, 230, 230, 235);
constexpr ImU32 kAccent         = IM_COL32(255, 255, 255, 255);
constexpr ImU32 kAccentSoft     = IM_COL32(196, 196, 196, 235);
constexpr ImU32 kAccentFaint    = IM_COL32(255, 255, 255, 45);
constexpr ImU32 kSelection      = IM_COL32(255, 255, 255, 34);
constexpr ImU32 kSelectionDim   = IM_COL32(255, 255, 255, 16);
constexpr ImU32 kSelectionLine  = IM_COL32(255, 255, 255, 220);
constexpr ImU32 kRule           = IM_COL32(255, 255, 255, 42);
constexpr ImU32 kHintBg         = IM_COL32(0, 0, 0, 240);
constexpr ImU32 kGood           = IM_COL32(214, 214, 214, 255);
constexpr ImU32 kWarn           = IM_COL32(255, 214, 112, 255);

std::mutex g_mx;
Screen g_screen = Screen::None;
int g_tab = 0;                 // missions screen tabs
int g_selection = 0;
int g_scroll = 0;
int g_visibleRows = 9;
std::vector<MissionInfo> g_missions;
std::vector<int> g_visible;

// Tutorial two-pane focus model (doc §3.2): the lesson pane owns focus on
// entry; the compact category rail is on the right. Each category remembers
// its own row and scroll.
bool g_railFocus = false;
int g_category = 0;
std::vector<std::pair<std::string, std::string>> g_packCategories; // ordered key/label
struct CategoryState {
    std::string key;
    std::string label;
    int available = 0;
    int unavailable = 0;
    int cleared = 0;
    int sel = 0;
    int scroll = 0;
};
std::vector<CategoryState> g_categories;

std::string Upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

bool IsTutorial(const MissionInfo& m) {
    return m.type == "tutorial";
}

bool IsTrial(const MissionInfo& m) {
    return !IsTutorial(m) && (m.type.empty() || m.type == "combo" || m.type == "trial");
}

std::string DisplayCharName(const std::string& resourceName) {
    if (resourceName.empty()) return "ANY";
    const int selectId = CharacterHotswap::GetSelectIdForResourceName(resourceName.c_str());
    return selectId >= 0 ? CharacterHotswap::GetDisplayNameForSelectId(selectId)
                         : resourceName;
}

const char* StageName(int stage) {
    return stage >= 0 && stage < kStageNameCount ? kStageNames[stage] : "Current stage";
}

void ClampScrollLocked() {
    if (g_visible.empty()) {
        g_selection = 0;
        g_scroll = 0;
        return;
    }
    if (g_selection < 0) g_selection = 0;
    if (g_selection >= static_cast<int>(g_visible.size())) {
        g_selection = static_cast<int>(g_visible.size()) - 1;
    }
    if (g_selection < g_scroll) g_scroll = g_selection;
    if (g_selection >= g_scroll + g_visibleRows) {
        g_scroll = g_selection - g_visibleRows + 1;
    }
    const int maxScroll = (std::max)(0, static_cast<int>(g_visible.size()) - g_visibleRows);
    if (g_scroll > maxScroll) g_scroll = maxScroll;
    if (g_scroll < 0) g_scroll = 0;
}

// Discover tutorial categories from the loaded entries (authored/file order,
// never a hardcoded array). Also refreshes counts and clear tallies.
void RebuildCategoriesLocked() {
    std::vector<CategoryState> next;
    auto ensure = [&](const std::string& key, const std::string& label) -> CategoryState& {
        for (CategoryState& c : next) if (c.key == key) return c;
        CategoryState c;
        c.key = key;
        c.label = label.empty() ? Upper(key) : label;
        for (const CategoryState& old : g_categories) {
            if (old.key == key) { c.sel = old.sel; c.scroll = old.scroll; break; }
        }
        next.push_back(std::move(c));
        return next.back();
    };
    // Pack taxonomy first (authored order; empty categories stay visible with
    // "No lessons are available in this section."), then discovered leftovers.
    for (const auto& pc : g_packCategories) ensure(pc.first, pc.second);
    for (const MissionInfo& m : g_missions) {
        if (!IsTutorial(m)) continue;
        const std::string key = m.category.empty() ? std::string("general") : m.category;
        CategoryState& cat = ensure(key, std::string());
        if (m.unavailableReason.empty()) {
            ++cat.available;
            if (m.cleared) ++cat.cleared;
        } else {
            ++cat.unavailable;
        }
    }
    g_categories = std::move(next);
    if (g_category >= static_cast<int>(g_categories.size())) g_category = 0;
}

void RebuildVisibleLocked() {
    g_visible.clear();
    if (g_screen == Screen::Tutorial) {
        RebuildCategoriesLocked();
        const std::string key = g_category < static_cast<int>(g_categories.size())
                              ? g_categories[g_category].key : std::string();
        for (int i = 0; i < static_cast<int>(g_missions.size()); ++i) {
            const MissionInfo& m = g_missions[i];
            const std::string mkey = m.category.empty() ? std::string("general") : m.category;
            if (IsTutorial(m) && mkey == key) g_visible.push_back(i);
        }
        // Authored course order. Equal `order` values keep pack/file order
        // (stable sort) - the browser never sorts a course alphabetically.
        std::stable_sort(g_visible.begin(), g_visible.end(), [](int lhs, int rhs) {
            return g_missions[lhs].order < g_missions[rhs].order;
        });
        if (g_category < static_cast<int>(g_categories.size())) {
            g_selection = g_categories[g_category].sel;
            g_scroll = g_categories[g_category].scroll;
        } else {
            g_selection = 0;
            g_scroll = 0;
        }
        ClampScrollLocked();
        return;
    }

    for (int i = 0; i < static_cast<int>(g_missions.size()); ++i) {
        const MissionInfo& m = g_missions[i];
        bool include = false;
        if (g_screen == Screen::Missions) {
            switch (g_tab) {
                case 0: include = !m.recorded && IsTrial(m); break;
                case 1: include = !m.recorded && !IsTutorial(m) && !IsTrial(m); break;
                case 2: include = m.recorded; break;
                default: break;
            }
        }
        if (include) g_visible.push_back(i);
    }
    std::stable_sort(g_visible.begin(), g_visible.end(), [](int lhs, int rhs) {
        const MissionInfo& a = g_missions[lhs];
        const MissionInfo& b = g_missions[rhs];
        if (a.source != b.source) return a.source < b.source;
        if (a.character != b.character) return a.character < b.character;
        return a.name < b.name;
    });
    g_selection = 0;
    g_scroll = 0;
    ClampScrollLocked();
}

void RememberCategoryPosLocked() {
    if (g_screen != Screen::Tutorial) return;
    if (g_category < static_cast<int>(g_categories.size())) {
        g_categories[g_category].sel = g_selection;
        g_categories[g_category].scroll = g_scroll;
    }
}

const MissionInfo* SelectedLocked() {
    if (g_selection < 0 || g_selection >= static_cast<int>(g_visible.size())) return nullptr;
    const int index = g_visible[g_selection];
    return index >= 0 && index < static_cast<int>(g_missions.size()) ? &g_missions[index] : nullptr;
}

struct Snapshot {
    Screen screen = Screen::None;
    int tab = 0;
    int selection = 0;
    int scroll = 0;
    int visibleRows = 9;
    bool railFocus = false;
    int category = 0;
    std::vector<CategoryState> categories;
    std::vector<MissionInfo> entries;
};

Snapshot CaptureSnapshot() {
    std::lock_guard<std::mutex> lk(g_mx);
    Snapshot out;
    out.screen = g_screen;
    out.tab = g_tab;
    out.selection = g_selection;
    out.scroll = g_scroll;
    out.visibleRows = g_visibleRows;
    out.railFocus = g_railFocus;
    out.category = g_category;
    out.categories = g_categories;
    out.entries.reserve(g_visible.size());
    for (int i : g_visible) {
        if (i >= 0 && i < static_cast<int>(g_missions.size())) out.entries.push_back(g_missions[i]);
    }
    return out;
}

struct Geom {
    float ls = 1.0f;
    float bodyPx = 11.0f;
    float headerPx = 17.0f;
    float smallPx = 9.0f;
    float margin = 24.0f;
    float headerBottom = 36.0f;
    float tabsTop = 42.0f;
    float tabsBottom = 68.0f;
    float contentTop = 78.0f;
    float hintTop = 446.0f;
    ImFont* body = nullptr;
    ImFont* header = nullptr;
};

Geom BeginScreen(ImDrawList* dl, const char* kicker, const char* title, const char* status) {
    CustomMenu::Scale::Update(Config::GetSettings().uiScale);
    const CustomMenu::Scale::Metrics& metrics = CustomMenu::Scale::Get();
    Geom g;
    g.ls = metrics.layoutScale;
    g.bodyPx = metrics.bodyPx;
    g.headerPx = metrics.headerPx;
    g.smallPx = (std::max)(8.0f, metrics.bodyPx - 2.0f);
    g.body = L::BodyFont();
    g.header = L::HeaderFont();
    g.margin = Snap(24.0f * g.ls);

    dl->AddRectFilled(ImVec2(0, 0), ImVec2(T::kCanvasW, T::kCanvasH), kBg);
    // Native title band ("GAME SETTINGS" style): black band, centered header,
    // status on the right, hard white rail underneath. The kicker becomes a
    // small label inside the band's left edge.
    L::DrawTitleBand(dl, title, status);
    L::DrawString(dl, g.body, g.smallPx, 14.0f, (T::kBandH * g.ls - g.smallPx) * 0.5f,
                  T::kTextStatus, kicker);
    return g;
}

void DrawTabs(ImDrawList* dl, const Geom& g, const char* const* labels, int active) {
    const float gap = 4.0f;
    const float width = (T::kCanvasW - g.margin * 2.0f - gap * (kTabCount - 1)) /
                        static_cast<float>(kTabCount);
    const float h = g.tabsBottom - g.tabsTop;
    for (int i = 0; i < kTabCount; ++i) {
        const float x0 = g.margin + (width + gap) * i;
        L::DrawNativeBarCentered(dl, x0, g.tabsTop, width, h, labels[i], i == active);
    }
}

struct HintPair { const char* key; const char* value; };

void DrawHintBar(ImDrawList* dl, const Geom& g, const HintPair* hints, int count) {
    // The game's bottom description box: black, thin white border.
    L::DrawInfoBox(dl, 8.0f, g.hintTop, T::kCanvasW - 16.0f, T::kCanvasH - g.hintTop - 6.0f);
    const float y = g.hintTop + (T::kCanvasH - 6.0f - g.hintTop - g.smallPx) * 0.5f;
    float x = g.margin;
    for (int i = 0; i < count; ++i) {
        L::DrawString(dl, g.body, g.smallPx, x, y, T::kTextActive, hints[i].key);
        x += L::MeasureTextW(g.body, g.smallPx, hints[i].key) + 5.0f;
        L::DrawString(dl, g.body, g.smallPx, x, y, T::kTextStatus, hints[i].value);
        x += L::MeasureTextW(g.body, g.smallPx, hints[i].value) + 18.0f;
    }
}

void DrawWrapped(ImDrawList* dl, ImFont* font, float fontSize, ImVec2 pos,
                 ImU32 color, const std::string& text, float width, float height) {
    if (text.empty()) return;
    dl->PushClipRect(pos, ImVec2(pos.x + width, pos.y + height), true);
    dl->AddText(font, fontSize, pos, color, text.c_str(), nullptr, width);
    dl->PopClipRect();
}

void DrawRichWrapped(void* device, ImDrawList* dl, ImFont* font, float fontSize,
                     ImVec2 pos, ImU32 color, const std::string& text,
                     float width, float height) {
    if (text.empty() || width <= 0.0f || height <= 0.0f) return;
    // The shared renderer wraps whole notation atoms, keeping a direction and
    // its button glyph together. Clip here because the title panes have fixed
    // vertical regions even when authored prose is longer than the preview.
    dl->PushClipRect(pos, ImVec2(pos.x + width, pos.y + height), true);
    Mission::Render::DrawRichText(device, dl, font, fontSize,
                                  pos.x, pos.y, color, text, width);
    dl->PopClipRect();
}

// Draws a pill and returns its width so callers can chain badges without fixed
// offsets (a wide difficulty label like "INTERMEDIATE", or a larger UI scale,
// used to overrun a hard-coded gap and overlap the next badge).
float DrawBadge(ImDrawList* dl, const Geom& g, float x, float y, const std::string& text, ImU32 color) {
    if (text.empty()) return 0.0f;
    const float w = L::MeasureTextW(g.body, g.smallPx, text.c_str()) + 12.0f;
    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + 18.0f), IM_COL32(255, 255, 255, 13), 2.0f);
    dl->AddRect(ImVec2(x, y), ImVec2(x + w, y + 18.0f), color, 2.0f, 0, 1.0f);
    L::DrawString(dl, g.body, g.smallPx, x + 6.0f, y + (18.0f - g.smallPx) * 0.5f, color, text.c_str());
    return w;
}

// One difficulty scale everywhere (TUTORIAL_MODE_DESIGN.md §3.2).
std::string DifficultyLabel(int difficulty) {
    if (difficulty <= 0) return "UNRATED";
    if (difficulty > 5) difficulty = 5;
    static const char* const labels[] = { "", "NOVICE", "BEGINNER", "INTERMEDIATE", "ADVANCED", "EXPERT" };
    return labels[difficulty];
}

void DrawScrollbar(ImDrawList* dl, float x, float y0, float y1, int total, int fit, int first) {
    if (total <= fit) return;
    const float trackH = y1 - y0;
    const float thumbH = trackH * static_cast<float>(fit) / static_cast<float>(total);
    const float thumbY = y0 + trackH * static_cast<float>(first) / static_cast<float>(total);
    dl->AddRectFilled(ImVec2(x, y0), ImVec2(x + 2.0f, y1), IM_COL32(255, 255, 255, 22));
    dl->AddRectFilled(ImVec2(x, thumbY), ImVec2(x + 2.0f, thumbY + thumbH), kAccentSoft);
}

// ---- MISSIONS screen (tab + list + detail) ---------------------------------

void DrawEntryList(ImDrawList* dl, const Geom& g, const Snapshot& s,
                   float x0, float y0, float x1, float y1) {
    dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), kPanel);
    dl->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), kPanelBorder);
    L::DrawString(dl, g.body, g.smallPx, x0 + 12.0f, y0 + 9.0f, kAccentSoft, "AVAILABLE SESSIONS");
    dl->AddLine(ImVec2(x0 + 10.0f, y0 + 28.0f), ImVec2(x1 - 10.0f, y0 + 28.0f), kRule, 1.0f);

    constexpr float rowH = 34.0f;
    const float listTop = y0 + 34.0f;
    const int fit = (std::max)(1, static_cast<int>((y1 - listTop - 6.0f) / rowH));
    {
        std::lock_guard<std::mutex> lk(g_mx);
        g_visibleRows = fit;
        ClampScrollLocked();
    }
    const int first = (std::max)(0, (std::min)(s.scroll, (std::max)(0, static_cast<int>(s.entries.size()) - fit)));
    const int last = (std::min)(static_cast<int>(s.entries.size()), first + fit);
    float y = listTop;
    for (int i = first; i < last; ++i) {
        const MissionInfo& m = s.entries[i];
        const bool selected = i == s.selection;
        if (selected) {
            L::DrawNativeBar(dl, x0 + 4.0f, y, (x1 - 4.0f) - (x0 + 4.0f), rowH - 1.0f, true);
        } else if (i & 1) {
            dl->AddRectFilled(ImVec2(x0 + 4.0f, y), ImVec2(x1 - 4.0f, y + rowH - 1.0f), IM_COL32(255, 255, 255, 6));
        }
        char number[8];
        _snprintf_s(number, sizeof(number), _TRUNCATE, "%02d", i + 1);
        const auto rowText = [&](ImFont* f, float px, float tx, float ty, ImU32 col, const char* txt) {
            if (selected) L::DrawOutlinedText(dl, f, px, tx, ty, col, txt);
            else L::DrawString(dl, f, px, tx, ty, col, txt);
        };
        rowText(g.body, g.smallPx, x0 + 12.0f, y + 6.0f,
                selected ? T::kBarTextSel : T::kTextDisabled, number);
        const ImU32 nameColor = m.locked ? (selected ? T::kBarTextDis : T::kTextDisabled)
                              : (selected ? T::kBarTextSel : T::kTextInactive);
        dl->PushClipRect(ImVec2(x0 + 40.0f, y), ImVec2(x1 - 64.0f, y + rowH), true);
        rowText(g.body, g.bodyPx, x0 + 40.0f, y + 5.0f, nameColor, Upper(m.name).c_str());
        rowText(g.body, g.smallPx, x0 + 40.0f, y + 20.0f,
                selected ? T::kBarTextSel : T::kTextStatus, Upper(m.source).c_str());
        dl->PopClipRect();
        char steps[16];
        _snprintf_s(steps, sizeof(steps), _TRUNCATE, "%d STEP%s", m.steps, m.steps == 1 ? "" : "S");
        const float sw = L::MeasureTextW(g.body, g.smallPx, steps);
        rowText(g.body, g.smallPx, x1 - 12.0f - sw, y + 11.0f,
                m.locked ? (selected ? T::kBarTextDis : T::kTextDisabled)
                         : (selected ? T::kBarTextSel : T::kTextStatus),
                m.locked ? "LOCKED" : steps);
        y += rowH;
    }

    DrawScrollbar(dl, x1 - 4.0f, listTop, y1 - 6.0f,
                  static_cast<int>(s.entries.size()), fit, first);
}

// Detail card leads with the learning result / objective; character, stage,
// source, and author follow as one compact secondary line (doc §3.2).
void DrawDetail(void* device, ImDrawList* dl, const Geom& g, const MissionInfo* m,
                float x0, float y0, float x1, float y1) {
    dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), kPanelStrong);
    dl->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), kPanelBorder);
    if (!m) {
        L::DrawString(dl, g.body, g.bodyPx, x0 + 18.0f, y0 + 18.0f, T::kTextDisabled, "NO ENTRIES IN THIS TAB");
        DrawWrapped(dl, g.body, g.bodyPx, ImVec2(x0 + 18.0f, y0 + 48.0f), T::kTextStatus,
                    "Add mission JSON files below assets\\missions or choose another tab.",
                    x1 - x0 - 36.0f, y1 - y0 - 60.0f);
        return;
    }

    const float x = x0 + 18.0f;
    const float badgeGap = 8.0f;
    float bx = x;
    bx += DrawBadge(dl, g, bx, y0 + 12.0f, DifficultyLabel(m->difficulty),
                    m->locked ? T::kTextDisabled : kAccentSoft) + badgeGap;
    if (m->hasSavestate)
        bx += DrawBadge(dl, g, bx, y0 + 12.0f, "FULL STATE", kGood) + badgeGap;
    if (m->hasDemo)
        bx += DrawBadge(dl, g, bx, y0 + 12.0f, "DEMO", kWarn) + badgeGap;

    DrawWrapped(dl, g.header, g.headerPx, ImVec2(x, y0 + 39.0f),
                m->locked ? T::kTextDisabled : T::kTextActive,
                Upper(m->name), x1 - x - 18.0f, 32.0f);

    L::DrawString(dl, g.body, g.smallPx, x, y0 + 76.0f, kAccentSoft, "OBJECTIVE");
    const std::string desc = m->description.empty() ? "No description provided." : m->description;
    DrawRichWrapped(device, dl, g.body, g.bodyPx, ImVec2(x, y0 + 91.0f),
                    m->description.empty() ? T::kTextDisabled : T::kTextInactive,
                    desc, x1 - x - 18.0f, 56.0f);

    dl->AddLine(ImVec2(x, y0 + 154.0f), ImVec2(x1 - 18.0f, y0 + 154.0f), kRule, 1.0f);
    std::string meta = DisplayCharName(m->character) + " vs " + DisplayCharName(m->dummy) +
                       "  |  " + StageName(m->stage) + "  |  " + Upper(m->source);
    if (!m->author.empty()) meta += " / " + Upper(m->author);
    DrawWrapped(dl, g.body, g.smallPx, ImVec2(x, y0 + 164.0f), T::kTextStatus,
                meta, x1 - x - 18.0f, 26.0f);

    L::DrawString(dl, g.body, g.smallPx, x, y0 + 198.0f, kAccentSoft, "RECIPE");
    // A long recorded combo (30+ steps) overflows a fixed 52px box. Give the recipe
    // the rest of the pane when there is no coach note below it (recorded combos have
    // no objective/note), so far more of the notation is visible before it clips.
    const float recipeTop = y0 + 213.0f;
    const float recipeBottom = m->hint.empty() ? (y1 - 14.0f) : (y0 + 266.0f);
    DrawRichWrapped(device, dl, g.body, g.bodyPx, ImVec2(x, recipeTop),
                    m->recipe.empty() ? T::kTextDisabled : T::kTextActive,
                    m->recipe.empty() ? "No recipe preview." : m->recipe,
                    x1 - x - 18.0f, recipeBottom - recipeTop);

    if (!m->hint.empty()) {
        L::DrawString(dl, g.body, g.smallPx, x, y0 + 274.0f, kAccentSoft, "COACH NOTE");
        DrawRichWrapped(device, dl, g.body, g.smallPx, ImVec2(x, y0 + 289.0f),
                        T::kTextStatus, m->hint, x1 - x - 18.0f,
                        (std::max)(0.0f, y1 - (y0 + 289.0f) - 6.0f));
    }
}

void DrawCreate(ImDrawList* dl, const Geom& g, float x0, float y0, float x1, float y1) {
    dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), kPanelStrong);
    dl->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), kPanelBorder);
    L::DrawNativeBar(dl, x0 + 12.0f, y0 + 16.0f, (x1 - 12.0f) - (x0 + 12.0f), 56.0f, true);
    L::DrawOutlinedText(dl, g.header, g.headerPx, x0 + 30.0f, y0 + 25.0f, T::kBarTextSel,
                        "RECORD NEW SESSION");
    L::DrawOutlinedText(dl, g.body, g.bodyPx, x0 + 30.0f, y0 + 49.0f, T::kBarTextSel,
                        "Choose fighters, arrange the start, then capture and review.");
    L::DrawString(dl, g.body, g.smallPx, x0 + 18.0f, y0 + 98.0f, kAccentSoft, "AUTHORING FLOW");
    const std::string binding = Mission::Engine::Recorder::GetMacroRecordBindingLabel();
    const int countInMs = Config::GetSettings().missionRecorderCountInMs;
    char countInText[64] = {};
    if (countInMs > 0) {
        _snprintf_s(countInText, sizeof(countInText), _TRUNCATE,
                    "the %.1f-second count-in",
                    static_cast<double>(countInMs) / 1000.0);
    } else {
        _snprintf_s(countInText, sizeof(countInText), _TRUNCATE,
                    "recording (the count-in is disabled)");
    }
    const std::string flow =
        "1. Launch Practice and choose the fighters and stage.\n"
        "2. Arrange the exact starting position, resources, and dummy state. Nothing is recorded yet.\n"
        "3. Press " + binding + " to start " + countInText + ", then release the controls.\n"
        "4. The exact start is saved on the next clear frame; then perform the mission exactly as it should be demonstrated.\n"
        "5. Press " + binding + " again to stop safely in Review. Open the menu to preview, save, retake, or discard.";
    DrawWrapped(dl, g.body, g.bodyPx, ImVec2(x0 + 18.0f, y0 + 122.0f), T::kTextInactive,
                flow,
                x1 - x0 - 36.0f, y1 - y0 - 136.0f);
}

// ---- TUTORIAL screen (course pane + category rail) --------------------------

void DrawCoursePane(void* device, ImDrawList* dl, const Geom& g, const Snapshot& s,
                    float x0, float y0, float x1, float y1) {
    const bool focused = !s.railFocus;
    dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), kPanel);
    dl->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), focused ? kPanelBorderHot : kPanelBorder);

    // Pane header: LESSONS + per-category completion count.
    const CategoryState* cat = s.category < static_cast<int>(s.categories.size())
                             ? &s.categories[s.category] : nullptr;
    L::DrawString(dl, g.body, g.smallPx, x0 + 12.0f, y0 + 9.0f, kAccentSoft, "LESSONS");
    if (cat) {
        char count[64];
        if (cat->unavailable > 0) {
            _snprintf_s(count, sizeof(count), _TRUNCATE, "%d / %d COMPLETE   %d UNAVAILABLE",
                        cat->cleared, cat->available, cat->unavailable);
        } else {
            _snprintf_s(count, sizeof(count), _TRUNCATE, "%d / %d COMPLETE",
                        cat->cleared, cat->available);
        }
        const float cw = L::MeasureTextW(g.body, g.smallPx, count);
        L::DrawString(dl, g.body, g.smallPx, x1 - 12.0f - cw, y0 + 9.0f, T::kTextStatus, count);
    }
    dl->AddLine(ImVec2(x0 + 10.0f, y0 + 28.0f), ImVec2(x1 - 10.0f, y0 + 28.0f), kRule, 1.0f);

    // Selected-lesson summary block claims the pane bottom; the list gets the rest.
    const float summaryH = 128.0f;
    const float summaryTop = y1 - summaryH;
    constexpr float rowH = 30.0f;
    const float listTop = y0 + 34.0f;
    const int fit = (std::max)(1, static_cast<int>((summaryTop - 8.0f - listTop) / rowH));
    {
        std::lock_guard<std::mutex> lk(g_mx);
        g_visibleRows = fit;
        ClampScrollLocked();
    }
    const int first = (std::max)(0, (std::min)(s.scroll, (std::max)(0, static_cast<int>(s.entries.size()) - fit)));
    const int last = (std::min)(static_cast<int>(s.entries.size()), first + fit);
    float y = listTop;
    for (int i = first; i < last; ++i) {
        const MissionInfo& m = s.entries[i];
        const bool selected = i == s.selection;
        if (selected) {
            // Focus and selection differ by shape: the focused pane gets the
            // bright steel bar, an unfocused pane keeps a dim fill while the
            // rail owns input.
            if (focused) {
                L::DrawNativeBar(dl, x0 + 4.0f, y, (x1 - 4.0f) - (x0 + 4.0f), rowH - 1.0f, true);
            } else {
                dl->AddRectFilled(ImVec2(x0 + 4.0f, y), ImVec2(x1 - 4.0f, y + rowH - 1.0f),
                                  kSelectionDim);
            }
        } else if (i & 1) {
            dl->AddRectFilled(ImVec2(x0 + 4.0f, y), ImVec2(x1 - 4.0f, y + rowH - 1.0f), IM_COL32(255, 255, 255, 6));
        }
        const bool onBar = selected && focused;
        const auto rowText = [&](ImFont* f, float px, float tx, float ty, ImU32 col, const char* txt) {
            if (onBar) L::DrawOutlinedText(dl, f, px, tx, ty, col, txt);
            else L::DrawString(dl, f, px, tx, ty, col, txt);
        };
        char number[8];
        _snprintf_s(number, sizeof(number), _TRUNCATE, "%02d", i + 1);
        rowText(g.body, g.smallPx, x0 + 12.0f, y + (rowH - g.smallPx) * 0.5f,
                onBar ? T::kBarTextSel : (selected ? kAccentSoft : T::kTextDisabled), number);
        // Clear mark: filled box + implicit count; open box when uncleared.
        const float mx = x0 + 34.0f;
        const float my = y + rowH * 0.5f;
        if (m.unavailableReason.empty() && m.cleared) {
            dl->AddRectFilled(ImVec2(mx, my - 4.0f), ImVec2(mx + 8.0f, my + 4.0f),
                              onBar ? T::kTextOutline : kGood);
        } else {
            dl->AddRect(ImVec2(mx, my - 4.0f), ImVec2(mx + 8.0f, my + 4.0f),
                        onBar ? T::kTextOutline : IM_COL32(255, 255, 255, 70));
        }
        dl->PushClipRect(ImVec2(mx + 14.0f, y), ImVec2(x1 - 70.0f, y + rowH), true);
        rowText(g.body, g.bodyPx, mx + 14.0f, y + (rowH - g.bodyPx) * 0.5f,
                onBar ? T::kBarTextSel : (selected ? T::kTextActive : T::kTextInactive),
                Upper(m.name).c_str());
        dl->PopClipRect();
        char tasks[24];
        if (!m.unavailableReason.empty()) {
            _snprintf_s(tasks, sizeof(tasks), _TRUNCATE, "UNAVAILABLE");
        } else if (m.updated) {
            _snprintf_s(tasks, sizeof(tasks), _TRUNCATE, "UPDATED");
        } else {
            _snprintf_s(tasks, sizeof(tasks), _TRUNCATE, "%d TASK%s", m.steps, m.steps == 1 ? "" : "S");
        }
        const float tw = L::MeasureTextW(g.body, g.smallPx, tasks);
        rowText(g.body, g.smallPx, x1 - 12.0f - tw, y + (rowH - g.smallPx) * 0.5f,
                m.unavailableReason.empty()
                    ? (onBar ? T::kBarTextSel : T::kTextStatus)
                    : (onBar ? T::kBarTextDis : T::kTextDisabled), tasks);
        y += rowH;
    }
    if (s.entries.empty()) {
        L::DrawString(dl, g.body, g.bodyPx, x0 + 14.0f, listTop + 6.0f, T::kTextDisabled,
                      "No lessons are available in this section.");
    }
    DrawScrollbar(dl, x1 - 4.0f, listTop, summaryTop - 8.0f,
                  static_cast<int>(s.entries.size()), fit, first);

    // Summary: learning result first, then tip, then a compact meta line.
    dl->AddLine(ImVec2(x0 + 10.0f, summaryTop), ImVec2(x1 - 10.0f, summaryTop), kRule, 1.0f);
    const MissionInfo* m = (s.selection >= 0 && s.selection < static_cast<int>(s.entries.size()))
                         ? &s.entries[s.selection] : nullptr;
    if (!m) return;
    const float sx = x0 + 14.0f;
    if (!m->unavailableReason.empty()) {
        L::DrawString(dl, g.body, g.smallPx, sx, summaryTop + 8.0f, kWarn, "UNAVAILABLE");
        DrawWrapped(dl, g.body, g.bodyPx, ImVec2(sx, summaryTop + 23.0f), T::kTextStatus,
                    m->unavailableReason, x1 - sx - 14.0f, 34.0f);
    } else {
        L::DrawString(dl, g.body, g.smallPx, sx, summaryTop + 8.0f, kAccentSoft, "YOU'LL PRACTICE");
        DrawRichWrapped(device, dl, g.body, g.bodyPx, ImVec2(sx, summaryTop + 23.0f),
                        m->description.empty() ? T::kTextDisabled : T::kTextInactive,
                        m->description.empty() ? "No summary provided." : m->description,
                        x1 - sx - 14.0f, 34.0f);
    }
    float metaY = summaryTop + 62.0f;
    if (!m->hint.empty()) {
        L::DrawString(dl, g.body, g.smallPx, sx, metaY, kAccentSoft, "TIP");
        DrawRichWrapped(device, dl, g.body, g.smallPx, ImVec2(sx + 30.0f, metaY),
                        T::kTextStatus, m->hint, x1 - sx - 44.0f, 28.0f);
        metaY += 32.0f;
    }
    std::string meta = DifficultyLabel(m->difficulty);
    if (!m->character.empty() || !m->dummy.empty()) {
        meta += "  |  " + DisplayCharName(m->character) + " vs " + DisplayCharName(m->dummy);
    }
    DrawWrapped(dl, g.body, g.smallPx, ImVec2(sx, metaY), T::kTextStatus,
                meta, x1 - sx - 14.0f, 16.0f);
}

void DrawCategoryRail(ImDrawList* dl, const Geom& g, const Snapshot& s,
                      float x0, float y0, float x1, float y1) {
    const bool focused = s.railFocus;
    dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), kPanelStrong);
    dl->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), focused ? kPanelBorderHot : kPanelBorder);
    L::DrawString(dl, g.body, g.smallPx, x0 + 12.0f, y0 + 9.0f, kAccentSoft, "CATEGORIES");
    dl->AddLine(ImVec2(x0 + 10.0f, y0 + 28.0f), ImVec2(x1 - 10.0f, y0 + 28.0f), kRule, 1.0f);

    constexpr float rowH = 44.0f;
    float y = y0 + 36.0f;
    for (int i = 0; i < static_cast<int>(s.categories.size()); ++i) {
        const CategoryState& c = s.categories[i];
        const bool current = i == s.category;
        const bool onBar = current && focused;
        if (current) {
            if (focused) {
                L::DrawNativeBar(dl, x0 + 4.0f, y, (x1 - 4.0f) - (x0 + 4.0f), rowH - 4.0f, true);
            } else {
                dl->AddRectFilled(ImVec2(x0 + 4.0f, y), ImVec2(x1 - 4.0f, y + rowH - 4.0f),
                                  kSelectionDim);
            }
        }
        dl->PushClipRect(ImVec2(x0 + 14.0f, y), ImVec2(x1 - 10.0f, y + rowH), true);
        if (onBar) {
            L::DrawOutlinedText(dl, g.body, g.bodyPx, x0 + 14.0f, y + 6.0f,
                                T::kBarTextSel, c.label.c_str());
        } else {
            L::DrawString(dl, g.body, g.bodyPx, x0 + 14.0f, y + 6.0f,
                          current ? T::kTextActive : T::kTextInactive, c.label.c_str());
        }
        char count[32];
        if (c.cleared >= c.available && c.available > 0 && c.unavailable > 0) {
            _snprintf_s(count, sizeof(count), _TRUNCATE, "COMPLETE   %d UNAVAILABLE",
                        c.unavailable);
        } else if (c.cleared >= c.available && c.available > 0) {
            _snprintf_s(count, sizeof(count), _TRUNCATE, "COMPLETE");
        } else if (c.unavailable > 0) {
            _snprintf_s(count, sizeof(count), _TRUNCATE, "%d / %d   %d UNAVAILABLE",
                        c.cleared, c.available, c.unavailable);
        } else {
            _snprintf_s(count, sizeof(count), _TRUNCATE, "%d / %d", c.cleared, c.available);
        }
        if (onBar) {
            L::DrawOutlinedText(dl, g.body, g.smallPx, x0 + 14.0f, y + 23.0f,
                                T::kBarTextSel, count);
        } else {
            L::DrawString(dl, g.body, g.smallPx, x0 + 14.0f, y + 23.0f,
                          c.cleared >= c.available && c.available > 0 ? kGood : T::kTextStatus,
                          count);
        }
        dl->PopClipRect();
        y += rowH;
        if (y + rowH > y1 - 4.0f) break;
    }
    if (s.categories.empty()) {
        L::DrawString(dl, g.body, g.smallPx, x0 + 12.0f, y0 + 38.0f, T::kTextDisabled,
                      "NO CATEGORIES");
    }
}

} // namespace

void Open(Screen screen, bool resumeLastLesson) {
    std::lock_guard<std::mutex> lk(g_mx);
    g_screen = screen;
    g_tab = 0;
    g_railFocus = false;
    g_selection = 0;
    g_scroll = 0;
    RebuildVisibleLocked();
    // A return from battle restores the exact last lesson row. A fresh browser
    // entry follows the open course's first uncleared available lesson. Neither
    // path locks or hides any other lesson.
    if (screen == Screen::Tutorial) {
        int bestIdx = -1;
        if (resumeLastLesson) {
            std::map<std::string, std::string> lastByPack;
            for (int i = 0; i < static_cast<int>(g_missions.size()); ++i) {
                const MissionInfo& m = g_missions[i];
                if (!IsTutorial(m) || m.packId.empty() ||
                    !m.unavailableReason.empty()) {
                    continue;
                }
                auto inserted = lastByPack.emplace(m.packId, std::string());
                if (inserted.second) {
                    inserted.first->second =
                        ::Mission::Tutorial::ProgressLastLesson(m.packId);
                }
                if (!inserted.first->second.empty() &&
                    inserted.first->second == m.lessonId) {
                    bestIdx = i;
                    break;
                }
            }
        }
        if (bestIdx < 0) {
            std::map<std::string, const MissionInfo*> lessonState;
            for (const MissionInfo& m : g_missions) {
                if (IsTutorial(m) && !m.packId.empty() && !m.lessonId.empty()) {
                    lessonState[m.packId + "\x1f" + m.lessonId] = &m;
                }
            }
            int bestOrder = 0;
            for (int i = 0; i < static_cast<int>(g_missions.size()); ++i) {
                const MissionInfo& m = g_missions[i];
                if (!IsTutorial(m) || m.cleared ||
                    !m.unavailableReason.empty()) {
                    continue;
                }
                bool recommended = true;
                for (const std::string& prerequisite : m.recommendedAfter) {
                    const auto it = lessonState.find(
                        m.packId + "\x1f" + prerequisite);
                    // A capability-gated prerequisite must not dead-end the
                    // soft course recommendation; it remains visible and can
                    // be revisited after the runtime gains that capability.
                    if (it == lessonState.end() ||
                        (!it->second->cleared &&
                         it->second->unavailableReason.empty())) {
                        recommended = false;
                        break;
                    }
                }
                if (!recommended) continue;
                if (bestIdx < 0 || m.order < bestOrder) {
                    bestIdx = i;
                    bestOrder = m.order;
                }
            }
            // Malformed/third-party recommendation graphs never make an open
            // course empty: fall back to its first uncleared available row.
            if (bestIdx < 0) {
                for (int i = 0; i < static_cast<int>(g_missions.size()); ++i) {
                    const MissionInfo& m = g_missions[i];
                    if (!IsTutorial(m) || m.cleared ||
                        !m.unavailableReason.empty()) {
                        continue;
                    }
                    if (bestIdx < 0 || m.order < bestOrder) {
                        bestIdx = i;
                        bestOrder = m.order;
                    }
                }
            }
        }
        if (bestIdx >= 0) {
            const std::string key = g_missions[bestIdx].category.empty()
                                  ? std::string("general") : g_missions[bestIdx].category;
            for (int c = 0; c < static_cast<int>(g_categories.size()); ++c) {
                if (g_categories[c].key == key) { g_category = c; break; }
            }
            RebuildVisibleLocked();
            for (int r = 0; r < static_cast<int>(g_visible.size()); ++r) {
                if (g_visible[r] == bestIdx) { g_selection = r; break; }
            }
            ClampScrollLocked();
        }
    }
}

void SetTutorialCategories(std::vector<std::pair<std::string, std::string>>&& cats) {
    std::lock_guard<std::mutex> lk(g_mx);
    g_packCategories = std::move(cats);
}

void Close() {
    std::lock_guard<std::mutex> lk(g_mx);
    RememberCategoryPosLocked();
    g_screen = Screen::None;
    g_railFocus = false;
}

bool Active() {
    std::lock_guard<std::mutex> lk(g_mx);
    return g_screen != Screen::None;
}

Screen Current() {
    std::lock_guard<std::mutex> lk(g_mx);
    return g_screen;
}

void SetMissions(std::vector<MissionInfo>&& missions) {
    std::lock_guard<std::mutex> lk(g_mx);
    g_missions = std::move(missions);
    RebuildVisibleLocked();
}

void MoveSelection(int dir) {
    std::lock_guard<std::mutex> lk(g_mx);
    if (g_screen == Screen::None || dir == 0) return;
    if (g_screen == Screen::Tutorial && g_railFocus) {
        // Rail focus: Up/Down changes category and restores that category's
        // remembered lesson row and scroll (doc §3.2).
        if (g_categories.empty()) return;
        RememberCategoryPosLocked();
        const int count = static_cast<int>(g_categories.size());
        g_category = (g_category + (dir > 0 ? 1 : -1) + count) % count;
        RebuildVisibleLocked();
        return;
    }
    if (g_visible.empty()) return;
    const int count = static_cast<int>(g_visible.size());
    g_selection = (g_selection + (dir > 0 ? 1 : -1) + count) % count;
    ClampScrollLocked();
    RememberCategoryPosLocked();
}

void MoveTab(int dir) {
    std::lock_guard<std::mutex> lk(g_mx);
    if (g_screen == Screen::None || dir == 0) return;
    if (g_screen == Screen::Tutorial) {
        // Right moves focus into the category rail; Left returns to the pane.
        g_railFocus = dir > 0;
        return;
    }
    g_tab = (g_tab + (dir > 0 ? 1 : -1) + kTabCount) % kTabCount;
    RebuildVisibleLocked();
}

ConfirmAction Confirm() {
    std::lock_guard<std::mutex> lk(g_mx);
    if (g_screen == Screen::Tutorial && g_railFocus) {
        g_railFocus = false;   // confirm in the rail returns focus to the course pane
        return ConfirmAction::None;
    }
    if (g_screen == Screen::Missions && g_tab == 3) return ConfirmAction::Record;
    const MissionInfo* selected = SelectedLocked();
    if (!selected || selected->locked || selected->path.empty() ||
        !selected->unavailableReason.empty()) {
        return ConfirmAction::None;   // UNAVAILABLE rows are visible, never launchable
    }
    return ConfirmAction::Launch;
}

bool Back() {
    std::lock_guard<std::mutex> lk(g_mx);
    if (g_screen == Screen::Tutorial && g_railFocus) {
        g_railFocus = false;   // cancel in the rail returns to the course pane
        return true;
    }
    return false;
}

std::string SelectedMissionPath() {
    std::lock_guard<std::mutex> lk(g_mx);
    const MissionInfo* selected = SelectedLocked();
    return selected ? selected->path : std::string();
}

bool WantsDraw() {
    std::lock_guard<std::mutex> lk(g_mx);
    return g_screen != Screen::None;
}

void Draw(void* device, ImDrawList* dl) {
    if (!dl) return;
    const Snapshot s = CaptureSnapshot();
    if (s.screen == Screen::None) return;

    if (s.screen == Screen::Tutorial) {
        int available = 0, unavailable = 0, cleared = 0;
        for (const CategoryState& c : s.categories) {
            available += c.available;
            unavailable += c.unavailable;
            cleared += c.cleared;
        }
        char status[64];
        if (unavailable > 0) {
            _snprintf_s(status, sizeof(status), _TRUNCATE,
                        "%d / %d COMPLETE   %d UNAVAILABLE",
                        cleared, available, unavailable);
        } else {
            _snprintf_s(status, sizeof(status), _TRUNCATE,
                        "%d / %d COMPLETE", cleared, available);
        }
        const Geom g = BeginScreen(dl, "LEARN BY DOING", "TUTORIAL", status);
        const float y0 = g.headerBottom + 6.0f;
        const float y1 = g.hintTop - 10.0f;
        const float railX0 = 452.0f;
        DrawCoursePane(device, dl, g, s, g.margin, y0, railX0 - 12.0f, y1);
        DrawCategoryRail(dl, g, s, railX0, y0, T::kCanvasW - g.margin, y1);
        if (s.railFocus) {
            const HintPair hints[] = {
                { "UP / DOWN", "CHANGE CATEGORY" },
                { "LEFT / CONFIRM", "LESSONS" },
                { "CANCEL / ESC", "LESSONS" },
            };
            DrawHintBar(dl, g, hints, 3);
        } else {
            const MissionInfo* sel =
                (s.selection >= 0 && s.selection < static_cast<int>(s.entries.size()))
                    ? &s.entries[s.selection] : nullptr;
            const HintPair hints[] = {
                { "UP / DOWN", "SELECT" },
                { "RIGHT", "CATEGORIES" },
                { "CONFIRM", sel && !sel->unavailableReason.empty() ? "UNAVAILABLE"
                                                                    : "START LESSON" },
                { "CANCEL / ESC", "BACK" },
            };
            DrawHintBar(dl, g, hints, 4);
        }
        return;
    }

    char status[48];
    _snprintf_s(status, sizeof(status), _TRUNCATE, "%d AVAILABLE", static_cast<int>(s.entries.size()));
    const Geom g = BeginScreen(dl, "PRACTICE LIBRARY", "MISSIONS", status);
    DrawTabs(dl, g, kMissionTabs, s.tab);

    const float y0 = g.contentTop;
    const float y1 = g.hintTop - 10.0f;
    if (s.tab == 3) {
        DrawCreate(dl, g, g.margin, y0, T::kCanvasW - g.margin, y1);
        const HintPair hints[] = {
            { "LEFT / RIGHT", "CHANGE TAB" },
            { "CONFIRM", "OPEN PRACTICE SETUP" },
            { "CANCEL / ESC", "BACK" },
        };
        DrawHintBar(dl, g, hints, 3);
        return;
    }

    const float listX0 = g.margin;
    const float listX1 = 354.0f;
    const float detailX0 = 368.0f;
    const float detailX1 = T::kCanvasW - g.margin;
    DrawEntryList(dl, g, s, listX0, y0, listX1, y1);
    const MissionInfo* selected = (s.selection >= 0 && s.selection < static_cast<int>(s.entries.size()))
                                ? &s.entries[s.selection] : nullptr;
    const HintPair hints[] = {
        { "LEFT / RIGHT", "CHANGE TAB" },
        { "UP / DOWN", "SELECT" },
        { "CONFIRM", selected && selected->locked ? "LOCKED" : "PLAY" },
        { "CANCEL / ESC", "BACK" },
    };
    DrawDetail(device, dl, g, selected, detailX0, y0, detailX1, y1);
    DrawHintBar(dl, g, hints, 4);
}

} // namespace PracticeMenu::TitleScreen
