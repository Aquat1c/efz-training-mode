// ================== Extended FM Implementation ==================
// NOTE: Provide rich logging for Final Memory diagnostics.
#include "../include/game/game_state.h"        // unify relative style with other game sources
#include "../include/core/constants.h"
#include "../include/core/memory.h"
#include "../include/core/logger.h"
#include "../include/input/input_core.h"
#include "../include/input/input_freeze.h"
#include "../include/game/fm_commands.h"
#include "../include/utils/utilities.h"
#include <vector>
#include <string>
#include <sstream>

// Helper: decode a single mask to human-readable token (direction number + buttons)
static std::string MaskToToken(uint8_t m) {
    // Direction resolution priority: diagonals first
    bool up = (m & GAME_INPUT_UP) != 0;
    bool down = (m & GAME_INPUT_DOWN) != 0;
    bool left = (m & GAME_INPUT_LEFT) != 0;
    bool right = (m & GAME_INPUT_RIGHT) != 0;
    int num = 5; // neutral baseline
    if (down && left)  num = 1;
    else if (down && right) num = 3;
    else if (up && left)    num = 7;
    else if (up && right)   num = 9;
    else if (down)          num = 2;
    else if (up)            num = 8;
    else if (left)          num = 4;
    else if (right)         num = 6;
    std::string out = std::to_string(num);
    if (m & GAME_INPUT_A) out += 'A';
    if (m & GAME_INPUT_B) out += 'B';
    if (m & GAME_INPUT_C) out += 'C';
    if (m & GAME_INPUT_D) out += 'D';
    return out;
}

static std::string DescribePattern(const std::vector<uint8_t>& pat) {
    std::ostringstream oss; oss << '[';
    for (size_t i=0;i<pat.size();++i) {
        if (i) oss << ' ';
        oss << MaskToToken(pat[i]);
    }
    oss << ']';
    return oss.str();
}

// Gating helpers (lightweight; add more as needed)
static bool GateAlways(int) { return true; }
static bool GateKanoRG(int playerNum) {
    // Require current moveID to be a recoil guard state (standing/crouch/air RG)
    uintptr_t base = GetPlayerPointer(playerNum);
    if (!base) return false;
    short moveId = 0; SafeReadMemory(ResolvePointer(GetEFZBase(), (playerNum==1?EFZ_BASE_OFFSET_P1:EFZ_BASE_OFFSET_P2), MOVE_ID_OFFSET), &moveId, sizeof(moveId));
    return (moveId == RG_STAND_ID || moveId == RG_CROUCH_ID || moveId == RG_AIR_ID);
}

// (Placeholder) Gate for Mai awakening variants: later we can read resource/mini state; allow always for now
static bool GateMaiAwakening(int) { return true; }

static const int DEFAULT_DIR_FRAMES = 4;   // fallback for direction-only token (no explicit *count)
static const int DEFAULT_BTN_FRAMES = 6;   // fallback for direction+button or button-only token

struct ParsedToken {
    uint8_t mask{0};
    int repeats{0};
};

// Parse one textual token into mask + repeat count (without mirroring adjustments)
static ParsedToken ParseTokenSpec(const std::string& raw, bool facingRight) {
    ParsedToken pt; if (raw.empty()) { pt.mask = GAME_INPUT_NEUTRAL; pt.repeats = DEFAULT_DIR_FRAMES; return pt; }
    // Split on '*'
    std::string core = raw; std::string repeatStr;
    size_t star = raw.find('*');
    if (star != std::string::npos) { core = raw.substr(0, star); repeatStr = raw.substr(star+1); }
    // Determine direction (first char if digit 1-9 or 'N')
    uint8_t dir = GAME_INPUT_NEUTRAL;
    if (!core.empty()) {
        char c = core[0];
        switch (c) {
            case '1': dir = GAME_INPUT_DOWN | (facingRight ? GAME_INPUT_LEFT : GAME_INPUT_RIGHT); break;
            case '2': dir = GAME_INPUT_DOWN; break;
            case '3': dir = GAME_INPUT_DOWN | (facingRight ? GAME_INPUT_RIGHT : GAME_INPUT_LEFT); break;
            case '4': dir = (facingRight ? GAME_INPUT_LEFT : GAME_INPUT_RIGHT); break;
            case '5': dir = GAME_INPUT_NEUTRAL; break;
            case '6': dir = (facingRight ? GAME_INPUT_RIGHT : GAME_INPUT_LEFT); break;
            case '7': dir = GAME_INPUT_UP | (facingRight ? GAME_INPUT_LEFT : GAME_INPUT_RIGHT); break;
            case '8': dir = GAME_INPUT_UP; break;
            case '9': dir = GAME_INPUT_UP | (facingRight ? GAME_INPUT_RIGHT : GAME_INPUT_LEFT); break;
            case 'N': dir = GAME_INPUT_NEUTRAL; break;
            default: break; // could be pure button token
        }
    }
    // Scan rest for buttons (skip leading digit/N if present)
    uint8_t btn = 0;
    for (size_t i = 0; i < core.size(); ++i) {
        char c = core[i];
        if (c >= '1' && c <= '9') continue;
        if (c == 'N' || c=='n') continue;
        if (c=='A') btn |= GAME_INPUT_A; else if (c=='B') btn |= GAME_INPUT_B; else if (c=='C') btn |= GAME_INPUT_C; else if (c=='S' || c=='D') btn |= GAME_INPUT_D;
    }
    pt.mask = dir | btn;
    if (!repeatStr.empty()) {
        int parsed = 1;
        try { parsed = std::stoi(repeatStr); } catch (...) { parsed = 1; }
        if (parsed < 1) parsed = 1;
        pt.repeats = parsed;
    } else {
        bool hasBtn = (btn != 0);
        pt.repeats = hasBtn ? DEFAULT_BTN_FRAMES : DEFAULT_DIR_FRAMES;
    }
    return pt;
}

std::vector<uint8_t> BuildPattern(const std::vector<const char*>& tokens, bool facingRight) {
    std::vector<uint8_t> out; out.reserve(tokens.size()*DEFAULT_BTN_FRAMES);
    for (auto* rawTok : tokens) {
        std::string t(rawTok);
        ParsedToken pt = ParseTokenSpec(t, facingRight);
        for (int i=0;i<pt.repeats;++i) out.push_back(pt.mask);
    }
    return out;
}

const std::vector<FinalMemoryCommand>& GetFinalMemoryCommands() {
    static std::vector<FinalMemoryCommand> cmds; static bool built=false; if (built) return cmds; built=true;
    bool facingRight = true;
    // Akane - TESTED
    cmds.push_back({CHAR_ID_AKANE, "Akane", BuildPattern({"5A","5*3","5A","5*3","6*3","5B","5*3","5C"}, facingRight), GateAlways, nullptr});
    // Akiko: requires clear down release (neutral) windows before neutral button presses.
    // Approx: hold 2, neutral gap, neutral+A, hold 2, neutral gap, neutral+B, neutral gap, neutral+C
    // Tunable counts (*values) chosen to mirror Minagi style gaps (2 holds ~6f, neutral gaps 3f, buttons 6f) - TESTED
    cmds.push_back({CHAR_ID_AKIKO, "Akiko", BuildPattern({"2*6","5*3","5A*6","2*6","5*3","5B*6","5*3","5C*6"}, facingRight), GateAlways, "Provisional release windows"});
    // Ayu
    cmds.push_back({CHAR_ID_AYU, "Ayu", BuildPattern({"5A","5A","4*3","5B","5C"}, facingRight), GateAlways, nullptr});
    // Ikumi 236x3 C - TESTED
    cmds.push_back({CHAR_ID_IKUMI, "Ikumi", BuildPattern({"2","3","6","2","3","6","2","3","6","5C"}, facingRight), GateAlways, nullptr});
    // Kaori 666S (triple forward then D) with sustained forward holds - NEW
    // Use forward holds to ensure recognition; pattern: 6*6 6*6 6*6 6S*6
    cmds.push_back({CHAR_ID_KAORI, "Kaori", BuildPattern({"6*6","6*6","6*6","6S*6"}, facingRight), GateAlways, nullptr});
    // Kanna 236236C - TESTED
    cmds.push_back({CHAR_ID_KANNA, "Kanna", BuildPattern({"2","3","6","2","3","6","5C"}, facingRight), GateAlways, nullptr});
    // Kano 214236S gated by recoil guard - No way to test since we don't have RG implemented yet
    cmds.push_back({CHAR_ID_KANO, "Kano", BuildPattern({"2","1","4","2","3","6","S"}, facingRight), GateKanoRG, "Requires recoil guard state"});
    // Mai (two contexts, same input for now)
    cmds.push_back({CHAR_ID_MAI, "Mai", BuildPattern({"2","3","6","2","3","6","S"}, facingRight), GateMaiAwakening, "Awakening context not enforced"});
    // Makoto 263S - TESTED
    cmds.push_back({CHAR_ID_MAKOTO, "Makoto", BuildPattern({"2","6","3","S"}, facingRight), GateAlways, nullptr});
    // Mayu 23693S (9 and 3 already diagonal handled)
    cmds.push_back({CHAR_ID_MAYU, "Mayu", BuildPattern({"2","3","6","9","3","S"}, facingRight), GateAlways, nullptr});
    // Minagi 222S with sustained holds and neutral gaps (tight example provided):
    // Approx pattern: 2 (hold) gap neutral, repeat clusters, final 2 + D press.
    // Using *counts: 2*3 neutral*3 2*3 neutral*3 2*3 2D*6 (tunable) - TESTED
    cmds.push_back({CHAR_ID_MINAGI, "Minagi", BuildPattern({"2*3","5*3","2*3","5*3","2*3","2S*6"}, facingRight), GateAlways, "Provisional hold timing"});
    // Mio short variant only for now
    cmds.push_back({CHAR_ID_MIO, "MioShort", BuildPattern({"6","4","1","2","3","6","5C","5A","5B","5C","5A","5B","5C","S","2","3","6","5C"}, facingRight), GateAlways, "Long range variant pending"});
    // Misaki 222S - TESTED
    cmds.push_back({CHAR_ID_MISAKI, "Misaki", BuildPattern({"2*3","5*3","2*3","5*3","2*3","2S*6"}, facingRight), GateAlways, nullptr});
    // Mishio: B 2 B 5 C A -> add holds & neutral gaps for reliability around standalone neutral '5'
    cmds.push_back({CHAR_ID_MISHIO, "Mishio", BuildPattern({"5B*6","2*6","5*3","5B*6","5*3","5C*6","5A*6"}, facingRight), GateAlways, "Added neutral gaps"});
    // Misuzu AA2B2C (given AA2BC spec but using 2B 2C per list)
    cmds.push_back({CHAR_ID_MISUZU, "Misuzu", BuildPattern({"5A","5*3","5A","5*3","2B","5*3","2C"}, facingRight), GateAlways, nullptr});
    // Mizuka 6 B A 6 A - TESTED
    cmds.push_back({CHAR_ID_MIZUKA, "Mizuka", BuildPattern({"6","5B","5A","6","5A"}, facingRight), GateAlways, nullptr});
    // Neyuki (Sleep) C then 236236
    cmds.push_back({CHAR_ID_NAYUKI, "NeyukiSleep", BuildPattern({"5C","5*3","2","3","6","2","3","6"}, facingRight), GateAlways, nullptr});
    // Nayuki Awake 236236S - TESTED
    cmds.push_back({CHAR_ID_NAYUKIB, "NayukiAwake", BuildPattern({"2","3","6","2","3","6","S"}, facingRight), GateAlways, nullptr});
    // Rumi 4123641236S - TESTED
    cmds.push_back({CHAR_ID_NANASE, "Rumi", BuildPattern({"4","1","2","3","6","4","1","2","3","6","S"}, facingRight), GateAlways, nullptr});
    // Sayuri 2 5 2A 5 5S -> include sustained holds and neutral gaps - TESTED
    cmds.push_back({CHAR_ID_SAYURI, "Sayuri", BuildPattern({"2*6","5*3","2A*6","5*3","5S*6"}, facingRight), GateAlways, "Added neutral gaps"});
    // Shiori sequential B C - TESTED
    cmds.push_back({CHAR_ID_SHIORI, "Shiori", BuildPattern({"2","3","6","2","3","6","5B","5C"}, facingRight), GateAlways, nullptr});
    // Unknown (MizukaB)
    cmds.push_back({CHAR_ID_MIZUKAB, "Unknown", BuildPattern({"5C","5B","5*3","6*3","5*3","5A","5A"}, facingRight), GateAlways, nullptr});
    return cmds;
}

bool ExecuteFinalMemory(int playerNum, int characterId) {
    uintptr_t playerPtr = GetPlayerPointer(playerNum);
    if (!playerPtr) return false;
    bool facingRight = GetPlayerFacingDirection(playerNum);
    const auto& list = GetFinalMemoryCommands();
    for (const auto& c : list) {
        if (c.characterId == characterId) {
            if (c.gate && !c.gate(playerNum)) {
                std::string msg = std::string("[FM] Gate failed for ") + c.name;
                if (c.gateDesc) { msg += " ("; msg += c.gateDesc; msg += ")"; }
                LogOut(msg, true);
                return false;
            }
            std::vector<uint8_t> pat = c.pattern; // copy before mirroring
            std::string preMirrorDesc = DescribePattern(pat);
            if (!facingRight) {
                for (auto &b : pat) {
                    bool leftOnly = (b & GAME_INPUT_LEFT) && !(b & GAME_INPUT_RIGHT);
                    bool rightOnly = (b & GAME_INPUT_RIGHT) && !(b & GAME_INPUT_LEFT);
                    if (leftOnly || rightOnly) {
                        b ^= (GAME_INPUT_LEFT | GAME_INPUT_RIGHT); // swap directional bit
                    }
                }
            }
            std::string postMirrorDesc = facingRight ? preMirrorDesc : DescribePattern(pat);
            LogOut(std::string("[FM] Executing ") + c.name +
                   " (charId=" + std::to_string(characterId) + ") P" + std::to_string(playerNum) +
                   (facingRight ? " FR" : " FL") +
                   " pattern=" + postMirrorDesc +
                   (facingRight ? "" : std::string(" (src=") + preMirrorDesc + ")"), true);
            bool ok = FreezeBufferWithPattern(playerNum, pat);
            if (!ok) {
                LogOut(std::string("[FM] Failed to freeze buffer for ") + c.name, true);
            } else {
                LogOut(std::string("[FM] Dispatched pattern len=") + std::to_string(pat.size()), detailedLogging.load());
            }
            return ok;
        }
    }
    LogOut(std::string("[FM] Character ID ") + std::to_string(characterId) + " not found in FM table", true);
    return false;
}
