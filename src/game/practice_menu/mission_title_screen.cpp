#include "../../../include/game/practice_menu/mission_title_screen.h"

#include "../../../include/gui/custom_menu/layout.h"
#include "../../../include/gui/custom_menu/theme.h"
#include "../../../include/gui/custom_menu/scale.h"
#include "../../../include/game/character_hotswap.h"
#include "../../../include/game/mission/mission_engine.h"
#include "../../../include/utils/config.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <mutex>
#include <string>
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
const char* const kTutorialTabs[] = { "START HERE", "MOVEMENT", "OFFENSE", "SYSTEMS" };
const char* const kTutorialKeys[] = { "start", "movement", "offense", "systems" };
constexpr int kTabCount = 4;

constexpr ImU32 kBg             = IM_COL32(4, 8, 11, 244);
constexpr ImU32 kPanel          = IM_COL32(7, 17, 22, 228);
constexpr ImU32 kPanelStrong    = IM_COL32(8, 23, 29, 244);
constexpr ImU32 kPanelBorder    = IM_COL32(105, 235, 235, 55);
constexpr ImU32 kAccent         = IM_COL32(105, 235, 235, 255);
constexpr ImU32 kAccentSoft     = IM_COL32(105, 235, 235, 170);
constexpr ImU32 kAccentFaint    = IM_COL32(105, 235, 235, 55);
constexpr ImU32 kSelection      = IM_COL32(28, 150, 185, 112);
constexpr ImU32 kSelectionLine  = IM_COL32(105, 235, 235, 205);
constexpr ImU32 kRule           = IM_COL32(255, 255, 255, 42);
constexpr ImU32 kHintBg         = IM_COL32(0, 0, 0, 232);
constexpr ImU32 kGood           = IM_COL32(135, 235, 150, 255);
constexpr ImU32 kWarn           = IM_COL32(255, 214, 112, 255);

std::mutex g_mx;
Screen g_screen = Screen::None;
int g_tab = 0;
int g_selection = 0;
int g_scroll = 0;
int g_visibleRows = 9;
std::vector<MissionInfo> g_missions;
std::vector<int> g_visible;

bool g_launchActive = false;
LaunchStage g_launchStage = LaunchStage::Preparing;
MissionInfo g_launchMission;

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

void RebuildVisibleLocked() {
    g_visible.clear();
    for (int i = 0; i < static_cast<int>(g_missions.size()); ++i) {
        const MissionInfo& m = g_missions[i];
        bool include = false;
        if (g_screen == Screen::Tutorial) {
            include = IsTutorial(m) && m.category == kTutorialKeys[g_tab];
        } else if (g_screen == Screen::Missions) {
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
    std::vector<MissionInfo> entries;
    bool launchActive = false;
    LaunchStage launchStage = LaunchStage::Preparing;
    MissionInfo launchMission;
};

Snapshot CaptureSnapshot() {
    std::lock_guard<std::mutex> lk(g_mx);
    Snapshot out;
    out.screen = g_screen;
    out.tab = g_tab;
    out.selection = g_selection;
    out.scroll = g_scroll;
    out.visibleRows = g_visibleRows;
    out.launchActive = g_launchActive;
    out.launchStage = g_launchStage;
    out.launchMission = g_launchMission;
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
    float headerBottom = 54.0f;
    float tabsTop = 60.0f;
    float tabsBottom = 92.0f;
    float contentTop = 104.0f;
    float hintTop = 450.0f;
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
    dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(T::kCanvasW, 110.0f),
                                IM_COL32(0, 0, 0, 150), IM_COL32(0, 0, 0, 150),
                                IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0));
    L::DrawString(dl, g.body, g.smallPx, g.margin, 9.0f, kAccentSoft, kicker);
    L::DrawString(dl, g.header, g.headerPx, g.margin, 25.0f, T::kTextActive, title);
    if (status && *status) {
        const float w = L::MeasureTextW(g.body, g.bodyPx, status);
        L::DrawString(dl, g.body, g.bodyPx, T::kCanvasW - g.margin - w, 29.0f,
                      T::kTextStatus, status);
    }
    dl->AddLine(ImVec2(0, g.headerBottom), ImVec2(T::kCanvasW, g.headerBottom), T::kRuleDim, 1.0f);
    const float titleW = L::MeasureTextW(g.header, g.headerPx, title);
    dl->AddRectFilled(ImVec2(g.margin, g.headerBottom - 2.0f),
                      ImVec2(g.margin + titleW, g.headerBottom + 1.0f), kAccent);
    return g;
}

void DrawTabs(ImDrawList* dl, const Geom& g, const char* const* labels, int active) {
    const float width = (T::kCanvasW - g.margin * 2.0f) / static_cast<float>(kTabCount);
    for (int i = 0; i < kTabCount; ++i) {
        const float x0 = g.margin + width * i;
        const float x1 = x0 + width;
        const bool selected = i == active;
        if (selected) {
            dl->AddRectFilled(ImVec2(x0, g.tabsTop), ImVec2(x1, g.tabsBottom), kPanelStrong);
            dl->AddRectFilled(ImVec2(x0, g.tabsBottom - 3.0f), ImVec2(x1, g.tabsBottom), kAccent);
        } else {
            dl->AddRectFilled(ImVec2(x0, g.tabsTop), ImVec2(x1, g.tabsBottom), kPanel);
        }
        const float tw = L::MeasureTextW(g.body, g.bodyPx, labels[i]);
        L::DrawString(dl, g.body, g.bodyPx, x0 + (width - tw) * 0.5f,
                      g.tabsTop + (g.tabsBottom - g.tabsTop - g.bodyPx) * 0.5f,
                      selected ? T::kTextActive : T::kTextInactive, labels[i]);
        if (i) dl->AddLine(ImVec2(x0, g.tabsTop + 5.0f), ImVec2(x0, g.tabsBottom - 5.0f), kRule, 1.0f);
    }
}

void DrawHintBar(ImDrawList* dl, const Geom& g, const char* confirmAction) {
    dl->AddRectFilled(ImVec2(0, g.hintTop), ImVec2(T::kCanvasW, T::kCanvasH), kHintBg);
    dl->AddLine(ImVec2(0, g.hintTop), ImVec2(T::kCanvasW, g.hintTop), kRule, 1.0f);
    const float y = g.hintTop + (T::kCanvasH - g.hintTop - g.bodyPx) * 0.5f;
    float x = g.margin;
    const struct { const char* key; const char* value; } hints[] = {
        { "LEFT / RIGHT", "CHANGE TAB" },
        { "UP / DOWN", "SELECT" },
        { "CONFIRM", confirmAction },
        { "CANCEL / ESC", "BACK" },
    };
    for (const auto& h : hints) {
        L::DrawString(dl, g.body, g.smallPx, x, y, kAccentSoft, h.key);
        x += L::MeasureTextW(g.body, g.smallPx, h.key) + 5.0f;
        L::DrawString(dl, g.body, g.smallPx, x, y, T::kTextStatus, h.value);
        x += L::MeasureTextW(g.body, g.smallPx, h.value) + 18.0f;
    }
}

void DrawWrapped(ImDrawList* dl, ImFont* font, float fontSize, ImVec2 pos,
                 ImU32 color, const std::string& text, float width, float height) {
    if (text.empty()) return;
    dl->PushClipRect(pos, ImVec2(pos.x + width, pos.y + height), true);
    dl->AddText(font, fontSize, pos, color, text.c_str(), nullptr, width);
    dl->PopClipRect();
}

void DrawBadge(ImDrawList* dl, const Geom& g, float x, float y, const std::string& text, ImU32 color) {
    if (text.empty()) return;
    const float w = L::MeasureTextW(g.body, g.smallPx, text.c_str()) + 12.0f;
    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + 18.0f), IM_COL32(255, 255, 255, 13), 2.0f);
    dl->AddRect(ImVec2(x, y), ImVec2(x + w, y + 18.0f), color, 2.0f, 0, 1.0f);
    L::DrawString(dl, g.body, g.smallPx, x + 6.0f, y + (18.0f - g.smallPx) * 0.5f, color, text.c_str());
}

std::string DifficultyLabel(int difficulty) {
    if (difficulty <= 0) return "UNRATED";
    if (difficulty > 5) difficulty = 5;
    static const char* const labels[] = { "", "BEGINNER", "EASY", "INTERMEDIATE", "ADVANCED", "EXPERT" };
    return labels[difficulty];
}

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
            dl->AddRectFilled(ImVec2(x0 + 4.0f, y), ImVec2(x1 - 4.0f, y + rowH - 1.0f), kSelection);
            dl->AddRectFilled(ImVec2(x0 + 4.0f, y), ImVec2(x0 + 7.0f, y + rowH - 1.0f), kAccent);
            dl->AddLine(ImVec2(x0 + 4.0f, y + rowH - 1.0f), ImVec2(x1 - 4.0f, y + rowH - 1.0f), kSelectionLine);
        } else if (i & 1) {
            dl->AddRectFilled(ImVec2(x0 + 4.0f, y), ImVec2(x1 - 4.0f, y + rowH - 1.0f), IM_COL32(255, 255, 255, 6));
        }
        char number[8];
        _snprintf_s(number, sizeof(number), _TRUNCATE, "%02d", i + 1);
        L::DrawString(dl, g.body, g.smallPx, x0 + 12.0f, y + 6.0f,
                      selected ? kAccentSoft : T::kTextDisabled, number);
        const ImU32 nameColor = m.locked ? T::kTextDisabled : (selected ? T::kTextActive : T::kTextInactive);
        dl->PushClipRect(ImVec2(x0 + 40.0f, y), ImVec2(x1 - 64.0f, y + rowH), true);
        L::DrawString(dl, g.body, g.bodyPx, x0 + 40.0f, y + 5.0f, nameColor, Upper(m.name).c_str());
        L::DrawString(dl, g.body, g.smallPx, x0 + 40.0f, y + 20.0f,
                      T::kTextStatus, Upper(m.source).c_str());
        dl->PopClipRect();
        char steps[16];
        _snprintf_s(steps, sizeof(steps), _TRUNCATE, "%d STEP%s", m.steps, m.steps == 1 ? "" : "S");
        const float sw = L::MeasureTextW(g.body, g.smallPx, steps);
        L::DrawString(dl, g.body, g.smallPx, x1 - 12.0f - sw, y + 11.0f,
                      m.locked ? T::kTextDisabled : T::kTextStatus, m.locked ? "LOCKED" : steps);
        y += rowH;
    }

    if (static_cast<int>(s.entries.size()) > fit) {
        const float trackY0 = listTop;
        const float trackY1 = y1 - 6.0f;
        const float trackH = trackY1 - trackY0;
        const float thumbH = trackH * static_cast<float>(fit) / static_cast<float>(s.entries.size());
        const float thumbY = trackY0 + trackH * static_cast<float>(first) / static_cast<float>(s.entries.size());
        dl->AddRectFilled(ImVec2(x1 - 4.0f, trackY0), ImVec2(x1 - 2.0f, trackY1), IM_COL32(255, 255, 255, 22));
        dl->AddRectFilled(ImVec2(x1 - 4.0f, thumbY), ImVec2(x1 - 2.0f, thumbY + thumbH), kAccentSoft);
    }
}

void DrawDetail(ImDrawList* dl, const Geom& g, const MissionInfo* m,
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
    DrawBadge(dl, g, x, y0 + 12.0f, DifficultyLabel(m->difficulty),
              m->locked ? T::kTextDisabled : kAccentSoft);
    float featureBadgeX = x + 102.0f;
    if (m->hasSavestate) {
        DrawBadge(dl, g, featureBadgeX, y0 + 12.0f, "FULL STATE", kGood);
        featureBadgeX += 104.0f;
    }
    if (m->hasDemo) DrawBadge(dl, g, featureBadgeX, y0 + 12.0f, "DEMO", kWarn);

    DrawWrapped(dl, g.header, g.headerPx, ImVec2(x, y0 + 39.0f),
                m->locked ? T::kTextDisabled : T::kTextActive,
                Upper(m->name), x1 - x - 18.0f, 32.0f);
    std::string source = Upper(m->source);
    if (!m->author.empty()) source += "  /  " + Upper(m->author);
    DrawWrapped(dl, g.body, g.smallPx, ImVec2(x, y0 + 76.0f), T::kTextStatus,
                source, x1 - x - 18.0f, 14.0f);

    L::DrawString(dl, g.body, g.smallPx, x, y0 + 98.0f, kAccentSoft, "OBJECTIVE");
    const std::string desc = m->description.empty() ? "No description provided." : m->description;
    DrawWrapped(dl, g.body, g.bodyPx, ImVec2(x, y0 + 113.0f),
                m->description.empty() ? T::kTextDisabled : T::kTextInactive,
                desc, x1 - x - 18.0f, 44.0f);

    dl->AddLine(ImVec2(x, y0 + 164.0f), ImVec2(x1 - 18.0f, y0 + 164.0f), kRule, 1.0f);
    L::DrawString(dl, g.body, g.smallPx, x, y0 + 175.0f, kAccentSoft, "MATCHUP");
    const std::string matchup = DisplayCharName(m->character) + "  vs  " + DisplayCharName(m->dummy);
    L::DrawString(dl, g.body, g.bodyPx, x, y0 + 190.0f, T::kTextActive, matchup.c_str());
    L::DrawString(dl, g.body, g.smallPx, x, y0 + 208.0f, T::kTextStatus, StageName(m->stage));

    L::DrawString(dl, g.body, g.smallPx, x, y0 + 232.0f, kAccentSoft, "RECIPE");
    DrawWrapped(dl, g.body, g.bodyPx, ImVec2(x, y0 + 247.0f),
                m->recipe.empty() ? T::kTextDisabled : T::kTextActive,
                m->recipe.empty() ? "No recipe preview." : m->recipe,
                x1 - x - 18.0f, 36.0f);

    if (!m->hint.empty()) {
        L::DrawString(dl, g.body, g.smallPx, x, y0 + 292.0f, kAccentSoft, "COACH NOTE");
        DrawWrapped(dl, g.body, g.smallPx, ImVec2(x, y0 + 307.0f), T::kTextStatus, m->hint,
                    x1 - x - 18.0f, (std::max)(0.0f, y1 - (y0 + 307.0f) - 6.0f));
    }
}

void DrawCreate(ImDrawList* dl, const Geom& g, float x0, float y0, float x1, float y1) {
    dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), kPanelStrong);
    dl->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), kPanelBorder);
    dl->AddRectFilled(ImVec2(x0 + 12.0f, y0 + 16.0f), ImVec2(x1 - 12.0f, y0 + 72.0f), kSelection);
    dl->AddRectFilled(ImVec2(x0 + 12.0f, y0 + 16.0f), ImVec2(x0 + 16.0f, y0 + 72.0f), kAccent);
    L::DrawString(dl, g.header, g.headerPx, x0 + 30.0f, y0 + 25.0f, T::kTextActive, "RECORD NEW SESSION");
    L::DrawString(dl, g.body, g.bodyPx, x0 + 30.0f, y0 + 49.0f, T::kTextStatus,
                  "Choose fighters, arrange the start, then capture and review.");
    L::DrawString(dl, g.body, g.smallPx, x0 + 18.0f, y0 + 98.0f, kAccentSoft, "AUTHORING FLOW");
    const std::string binding = Mission::Engine::Recorder::GetMacroRecordBindingLabel();
    const std::string flow =
        "1. Launch Practice and choose the fighters and stage.\n"
        "2. Arrange the exact starting position, resources, and dummy state. Nothing is recorded yet.\n"
        "3. Press " + binding + " to start the three-second countdown, then release the controls.\n"
        "4. At GO, perform the full mission exactly as it should be demonstrated.\n"
        "5. Press " + binding + " again to stop safely in Review. Open the menu to preview, save, retake, or discard.";
    DrawWrapped(dl, g.body, g.bodyPx, ImVec2(x0 + 18.0f, y0 + 122.0f), T::kTextInactive,
                flow,
                x1 - x0 - 36.0f, y1 - y0 - 136.0f);
}

const char* LaunchStageText(LaunchStage stage) {
    switch (stage) {
        case LaunchStage::Fighters:        return "CREATING FIGHTERS";
        case LaunchStage::Loading:         return "LOADING STAGE";
        case LaunchStage::MatchSetup:      return "APPLYING START STATE";
        case LaunchStage::Preparing:
        default:                           return "PREPARING SESSION";
    }
}

int LaunchStageIndex(LaunchStage stage) {
    return static_cast<int>(stage);
}

void DrawLaunch(ImDrawList* dl, const Snapshot& s) {
    const bool tutorial = s.launchMission.type == "tutorial";
    const Geom g = BeginScreen(dl, tutorial ? "HANDS-ON TUTORIAL" : "MISSION MODE",
                               tutorial ? "STARTING LESSON" : "STARTING SESSION",
                               LaunchStageText(s.launchStage));
    const float x0 = 94.0f, x1 = 546.0f, y0 = 116.0f, y1 = 390.0f;
    dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), kPanelStrong);
    dl->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), kPanelBorder);
    dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + 4.0f, y1), kAccent);

    L::DrawString(dl, g.body, g.smallPx, x0 + 26.0f, y0 + 22.0f, kAccentSoft,
                  tutorial ? "LESSON" : "SELECTED MISSION");
    DrawWrapped(dl, g.header, g.headerPx, ImVec2(x0 + 26.0f, y0 + 43.0f), T::kTextActive,
                Upper(s.launchMission.name), x1 - x0 - 52.0f, 45.0f);
    L::DrawString(dl, g.body, g.bodyPx, x0 + 26.0f, y0 + 95.0f, T::kTextInactive,
                  (DisplayCharName(s.launchMission.character) + "  vs  " +
                   DisplayCharName(s.launchMission.dummy)).c_str());
    L::DrawString(dl, g.body, g.smallPx, x0 + 26.0f, y0 + 116.0f, T::kTextStatus,
                  StageName(s.launchMission.stage));

    const float lineX0 = x0 + 34.0f;
    const float lineX1 = x1 - 34.0f;
    const float lineY = y0 + 178.0f;
    dl->AddRectFilled(ImVec2(lineX0, lineY), ImVec2(lineX1, lineY + 3.0f), IM_COL32(255, 255, 255, 28));
    const int active = LaunchStageIndex(s.launchStage);
    const char* const labels[] = { "QUEUE", "FIGHTERS", "STAGE", "SETUP" };
    for (int i = 0; i < 4; ++i) {
        const float t = static_cast<float>(i) / 3.0f;
        const float cx = lineX0 + (lineX1 - lineX0) * t;
        const bool done = i < active;
        const bool current = i == active;
        const ImU32 col = done ? kGood : (current ? kAccent : IM_COL32(255, 255, 255, 50));
        dl->AddCircleFilled(ImVec2(cx, lineY + 1.5f), current ? 7.0f : 5.0f, col, 18);
        const float tw = L::MeasureTextW(g.body, g.smallPx, labels[i]);
        L::DrawString(dl, g.body, g.smallPx, cx - tw * 0.5f, lineY + 17.0f,
                      current ? T::kTextActive : T::kTextStatus, labels[i]);
    }

    const std::string note = s.launchStage == LaunchStage::Fighters
        ? "Creating both pinned fighters through EFZ's native loader. Character Select is skipped."
        : s.launchStage == LaunchStage::MatchSetup
        ? "Normalizing the round and restoring the authored starting state."
        : "Please wait while EFZ prepares a deterministic practice session.";
    DrawWrapped(dl, g.body, g.bodyPx, ImVec2(x0 + 26.0f, y0 + 230.0f), T::kTextStatus,
                note, x1 - x0 - 52.0f, 34.0f);
}

} // namespace

void Open(Screen screen) {
    std::lock_guard<std::mutex> lk(g_mx);
    g_screen = screen;
    g_tab = 0;
    g_selection = 0;
    g_scroll = 0;
    g_launchActive = false;
    RebuildVisibleLocked();
}

void Close() {
    std::lock_guard<std::mutex> lk(g_mx);
    g_screen = Screen::None;
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
    if (g_screen == Screen::None || g_visible.empty() || dir == 0) return;
    const int count = static_cast<int>(g_visible.size());
    g_selection = (g_selection + (dir > 0 ? 1 : -1) + count) % count;
    ClampScrollLocked();
}

void MoveTab(int dir) {
    std::lock_guard<std::mutex> lk(g_mx);
    if (g_screen == Screen::None || dir == 0) return;
    g_tab = (g_tab + (dir > 0 ? 1 : -1) + kTabCount) % kTabCount;
    RebuildVisibleLocked();
}

ConfirmAction Confirm() {
    std::lock_guard<std::mutex> lk(g_mx);
    if (g_screen == Screen::Missions && g_tab == 3) return ConfirmAction::Record;
    const MissionInfo* selected = SelectedLocked();
    if (!selected || selected->locked || selected->path.empty()) return ConfirmAction::None;
    return ConfirmAction::Launch;
}

bool Back() { return false; }

std::string SelectedMissionPath() {
    std::lock_guard<std::mutex> lk(g_mx);
    const MissionInfo* selected = SelectedLocked();
    return selected ? selected->path : std::string();
}

void BeginSelectedLaunch() {
    std::lock_guard<std::mutex> lk(g_mx);
    const MissionInfo* selected = SelectedLocked();
    if (!selected) return;
    g_launchMission = *selected;
    g_launchStage = LaunchStage::Preparing;
    g_launchActive = true;
    g_screen = Screen::None;
}

void SetLaunchStage(LaunchStage stage) {
    std::lock_guard<std::mutex> lk(g_mx);
    if (g_launchActive) g_launchStage = stage;
}

void FinishLaunch() {
    std::lock_guard<std::mutex> lk(g_mx);
    g_launchActive = false;
}

bool WantsDraw() {
    std::lock_guard<std::mutex> lk(g_mx);
    return g_screen != Screen::None || g_launchActive;
}

void Draw(ImDrawList* dl) {
    if (!dl) return;
    const Snapshot s = CaptureSnapshot();
    if (s.launchActive) {
        DrawLaunch(dl, s);
        return;
    }
    if (s.screen == Screen::None) return;

    const bool tutorial = s.screen == Screen::Tutorial;
    char status[48];
    _snprintf_s(status, sizeof(status), _TRUNCATE, "%d AVAILABLE", static_cast<int>(s.entries.size()));
    const Geom g = BeginScreen(dl, tutorial ? "LEARN BY DOING" : "PRACTICE LIBRARY",
                               tutorial ? "TUTORIAL" : "MISSIONS", status);
    DrawTabs(dl, g, tutorial ? kTutorialTabs : kMissionTabs, s.tab);

    const float y0 = g.contentTop;
    const float y1 = g.hintTop - 10.0f;
    if (!tutorial && s.tab == 3) {
        DrawCreate(dl, g, g.margin, y0, T::kCanvasW - g.margin, y1);
        DrawHintBar(dl, g, "OPEN PRACTICE SETUP");
        return;
    }

    const float listX0 = g.margin;
    const float listX1 = 354.0f;
    const float detailX0 = 368.0f;
    const float detailX1 = T::kCanvasW - g.margin;
    DrawEntryList(dl, g, s, listX0, y0, listX1, y1);
    const MissionInfo* selected = (s.selection >= 0 && s.selection < static_cast<int>(s.entries.size()))
                                ? &s.entries[s.selection] : nullptr;
    DrawDetail(dl, g, selected, detailX0, y0, detailX1, y1);
    DrawHintBar(dl, g, selected && selected->locked ? "LOCKED" : (tutorial ? "START LESSON" : "PLAY"));
}

} // namespace PracticeMenu::TitleScreen
