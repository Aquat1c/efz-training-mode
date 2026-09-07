#include "../include/game/sayuri_counter.h"

#include "../include/core/constants.h"
#include "../include/core/memory.h"
#include "../include/core/logger.h"
#include "../include/game/game_state.h"
#include "../include/utils/utilities.h"   // GetEFZBase, g_featuresEnabled
#include "../include/utils/network.h"     // IsNetplaySuspendActive

// The only translation unit that pulls in the notation catalogue. It is a
// header-only compile-time table with no build-flag guard, but each including TU
// gets its own copy of ~24KB of tables, so the whole feature reads it from here
// and hands the menus a ready-made list of string pointers instead.
#include "../include/game/mission/move_notation_tables.h"

#include <atomic>
#include <cstdint>
#include <cstring>   // _strnicmp, strcmp
#include <string>

// Forward-declared rather than pulling a GUI header into a game module. The
// modal dropdown popup keeps the raw items pointer alive across frames, so the
// picker list must not be rebuilt underneath an open popup.
namespace CustomMenu { namespace Screens { bool IsPopupActive(); } }

namespace SayuriCounter {

namespace {

// ---------------------------------------------------------------------------
// Published settings.
//
// File-scope atomics, matching doppel_tech.cpp. The shipping v141_xp build
// compiles with /Zc:threadSafeInit-, so a function-local static on a path the
// monitor thread reaches would not be thread-safe. Index 0 is unused so callers
// can pass playerNum 1/2 directly.
//
// Every array below relies on static-storage zero initialisation, which lands on
// exactly the wanted defaults (MEMORY_OFF == 0, CUTTER_NORMAL == 0, "nothing
// pinned" == 0, "no table" == 0 which the -1 convention below is offset for).
// Braced initialisers are avoided on purpose: std::atomic has a deleted copy
// constructor, so pre-C++17 copy-list-initialisation of array elements is not
// portable.
// ---------------------------------------------------------------------------
std::atomic<int> g_memoryChoice[3];   // MEMORY_OFF - presentation index only
std::atomic<int> g_cutterMode[3];     // CUTTER_NORMAL
std::atomic<int> g_pickedMove[3];     // resolved opponent move ID, 0 = none

// Catalogue table index the pinned move was resolved against, biased by +1 so
// that the zero-initialised default means "nothing pinned". Tick() refuses to
// write a pinned move unless the live opponent still resolves to the same table,
// which is what stops a pick made against one opponent from silently meaning a
// different attack against the next.
std::atomic<int> g_pickedTableBias[3];

// Set when the picker list was rebuilt for a DIFFERENT opponent, cleared by the
// next publish. Until then any stored index at or past the fixed head reads as
// OFF, because that index names a different attack on the new character. It is
// a level rather than a one-shot event so that both menus, and any apply that
// happens while neither is showing her pane, all see it.
std::atomic<int> g_selectionStale[3];

// Sayuri's move ID on the previous poll, per side. LAST BLOCKED samples the
// opponent ONCE, on the tick she enters grounded blockstun, so the move that
// actually caused the block is what gets remembered - the attacker very often
// lands, recovers or returns to neutral while she is still frozen, and
// re-sampling would leave that later state in the memory instead.
std::atomic<int> g_prevMoveId[3];

// ---------------------------------------------------------------------------
// The rebuilt picker list. Menu/render thread only - Tick() never touches it.
// ---------------------------------------------------------------------------
constexpr int kFixedChoices = 3;    // OFF / NOTHING / LAST BLOCKED
constexpr int kMaxMoveChoices = 96; // never reached today (the largest opponent
                                    // list is 66 moves) and comfortably under
                                    // the menu popup's own list ceiling
constexpr int kMaxChoices = kFixedChoices + kMaxMoveChoices;
constexpr int kLabelLen = 64;       // longest catalogue notation is 54 chars

// The move-ID band an opponent's own attacks live in. Below it the catalogue
// only holds movement and system states (jumps, dashes, IC, landing, neutral);
// above it is the entity ring. Both the picker and LAST BLOCKED use it, so a
// landing or idle state can never end up in the memory.
constexpr int kOpponentMoveIdMin = 200;
constexpr int kOpponentMoveIdMax = 399;

char        s_labels[3][kMaxChoices][kLabelLen];
const char* s_items[3][kMaxChoices];
int         s_moveId[3][kMaxChoices];
int         s_count[3];
int         s_tableIndex[3];        // index into kCharTables, or -1
bool        s_listAvailable[3];
bool        s_truncated[3];
char        s_oppName[3][20];       // opponent resource name the list was built from
bool        s_built[3];

const char* const kFixedLabels[kFixedChoices] = { "OFF", "NOTHING", "LAST BLOCKED" };

inline int ClampPlayer(int playerNum) {
    return (playerNum == 2) ? 2 : 1;
}

// The six grounded blockstun states. Air blockstun (156) is excluded by the
// engine's own bound and is excluded here too, and the recoil-guard states sit
// outside the range entirely.
inline bool IsGroundedBlockstun(int moveID) {
    return moveID >= STANDING_BLOCK_LVL1 && moveID <= CROUCHING_BLOCK_LVL2_B;
}

// Case-insensitive match on the resource name in the player struct. Deliberately
// stricter than CharacterSettings::GetCharacterID(), whose partial-match fallback
// could resolve an unrelated name, and free of the per-tick std::string that
// helper needs. No other character resource name starts with "sayuri".
bool SideIsSayuri(uintptr_t base, uintptr_t baseOffset) {
    uintptr_t nameAddr = ResolvePointer(base, baseOffset, CHARACTER_NAME_OFFSET);
    if (!nameAddr) return false;
    char name[16] = {0};
    if (!SafeReadMemory(nameAddr, name, sizeof(name) - 1)) return false;
    name[sizeof(name) - 1] = '\0';
    return _strnicmp(name, "sayuri", 6) == 0;
}

// Reads the opponent's resource name straight out of the player struct. The menu
// mirror in DisplayData is only refreshed when the menu is opened, so a hot-swap
// with the menu already up would leave it stale.
bool ReadOpponentName(uintptr_t base, uintptr_t oppOffset, char* out, size_t outSize) {
    if (!out || outSize == 0) return false;
    out[0] = '\0';
    uintptr_t nameAddr = ResolvePointer(base, oppOffset, CHARACTER_NAME_OFFSET);
    if (!nameAddr) return false;
    char name[16] = {0};
    if (!SafeReadMemory(nameAddr, name, sizeof(name) - 1)) return false;
    name[sizeof(name) - 1] = '\0';
    if (name[0] == '\0') return false;
    strncpy_s(out, outSize, name, _TRUNCATE);
    return true;
}

int TableIndexForName(const char* name) {
    if (!name || !name[0]) return -1;
    for (int t = 0; t < Mission::MoveNames::kCharTableCount; ++t) {
        if (_stricmp(Mission::MoveNames::kCharTables[t].name, name) == 0) return t;
    }
    return -1;
}

// Live opponent table index for the side SAYURI is on, or -1.
int LiveOpponentTableIndex(uintptr_t base, int sayuriSide) {
    const uintptr_t oppOffset = (sayuriSide == 1) ? EFZ_BASE_OFFSET_P2
                                                  : EFZ_BASE_OFFSET_P1;
    char name[20] = {0};
    if (!ReadOpponentName(base, oppOffset, name, sizeof(name))) return -1;
    return TableIndexForName(name);
}

bool NoteContains(const char* note, const char* needle) {
    const size_t len = std::strlen(needle);
    for (const char* p = note; *p; ++p) {
        if (_strnicmp(p, needle, len) == 0) return true;
    }
    return false;
}

// Throws and command grabs produce no blockstun at all, so they can never arm
// the memory and the counter cannot catch them either.
bool NoteIsThrowOrGrab(const char* note) {
    return NoteContains(note, "throw") || NoteContains(note, "grab");
}

// The command part of a notation: everything before the first space or bracket.
// "236A (S)" -> "236A", "5S (magic charged)" -> "5S", "214*~6S (forward roll)"
// -> "214*~6S", "FM (S): 641236C starter" -> "FM".
void CommandToken(const char* note, char* out, size_t outSize) {
    size_t n = 0;
    for (const char* p = note; *p && n + 1 < outSize; ++p) {
        if (*p == ' ' || *p == '(') break;
        out[n++] = *p;
    }
    out[n] = '\0';
}

bool TokenEndsWith(const char* token, const char* suffix) {
    const size_t tl = std::strlen(token);
    const size_t sl = std::strlen(suffix);
    return tl >= sl && _stricmp(token + (tl - sl), suffix) == 0;
}

// S is EFZ's system button: stance switches, summons, jam, magic charge, FM
// installs and the feint/roll follow-ups. None of them is a strike, so none can
// put Sayuri in blockstun. Matched on the command token only, so Mio's Short
// stance moves ("623A (S)", "236B (S)") are kept - their S is a stance marker,
// not the button.
bool CommandUsesSystemButton(const char* token) {
    return TokenEndsWith(token, "S");
}

// Pure movement states the catalogue keeps alongside the attacks.
bool CommandIsMovement(const char* token) {
    return TokenEndsWith(token, "backdash") || TokenEndsWith(token, "fwddash");
}

// Leading motion digits of a command token: "41236" out of "41236A", "41236*"
// and "41236*~236B"; empty for "FM" or "throw".
void MotionDigits(const char* token, char* out, size_t outSize) {
    size_t n = 0;
    for (const char* p = token; *p >= '0' && *p <= '9' && n + 1 < outSize; ++p) {
        out[n++] = *p;
    }
    out[n] = '\0';
}

// A command throw is recognised from the catalogue itself: its motion has an
// automatic "success" or "catch" phase in the same table (Doppel's and Ikumi's
// 41236, Mayu's 41236, Rumi's 41236). A blocked strike never has one - the
// catalogue spells a strike's connect phase "hit", and the "successful-contact"
// follow-ups belong to counter/parry moves, which is why both are excluded.
// Sibling tokens carrying ~ or / are rekka continuations, not the base motion.
bool MotionHasThrowSuccessPhase(const Mission::MoveNames::CharTable& ct,
                                const char* digits) {
    using Mission::MoveNames::MoveRole;
    using Mission::MoveNames::HasRole;
    if (!digits[0]) return false;
    for (int i = 0; i < ct.count; ++i) {
        const Mission::MoveNames::MoveNote& s = ct.entries[i];
        if (!s.note || !HasRole(s.role, MoveRole::AutomaticPhase)) continue;
        if (NoteContains(s.note, "successful")) continue;
        if (!NoteContains(s.note, "success") && !NoteContains(s.note, "catch")) continue;
        char token[64];
        CommandToken(s.note, token, sizeof(token));
        if (std::strchr(token, '~') || std::strchr(token, '/')) continue;
        char sibling[32];
        MotionDigits(token, sibling, sizeof(sibling));
        if (sibling[0] && std::strcmp(sibling, digits) == 0) return true;
    }
    return false;
}

// Which catalogue entries the opponent can actually throw at a blocking Sayuri.
//
//  * DirectInput / InputFollowup only - the automatic and internal phases are
//    engine continuations the opponent cannot choose;
//  * move IDs 200..399 - below that the catalogue only holds movement and system
//    states (jumps, dashes, IC), above it is the entity ring;
//  * no throws, command grabs or their follow-ups;
//  * no system-button actions and no movement states;
//  * a real notation string.
//
// The point of every rule here is that the entry must be able to put her in
// GROUNDED BLOCKSTUN. Anything that cannot would sit in the picker looking
// selected while the feature quietly did nothing, so an entry that cannot be
// judged from its notation is dropped rather than offered.
//
// Projectile-cast patterns are deliberately kept: arming compares the opponent's
// live move ID on the blockstun entry frame, and a blocked fireball usually
// catches the caster still inside the cast pattern, so those are genuinely
// armable even though the counter itself could never have caught one.
bool EntryIsOfferable(const Mission::MoveNames::CharTable& ct,
                      const Mission::MoveNames::MoveNote& e) {
    using Mission::MoveNames::MoveRole;
    using Mission::MoveNames::HasRole;
    if (!HasRole(e.role, MoveRole::DirectInput) &&
        !HasRole(e.role, MoveRole::InputFollowup)) return false;
    if (e.id < kOpponentMoveIdMin || e.id > kOpponentMoveIdMax) return false;
    if (!e.note || !e.note[0]) return false;
    if (NoteIsThrowOrGrab(e.note)) return false;

    char token[64];
    CommandToken(e.note, token, sizeof(token));
    if (!token[0]) return false;
    if (CommandUsesSystemButton(token)) return false;
    if (CommandIsMovement(token)) return false;

    char digits[32];
    MotionDigits(token, digits, sizeof(digits));
    if (MotionHasThrowSuccessPhase(ct, digits)) return false;
    return true;
}

void BuildFixedOnly(int side) {
    for (int i = 0; i < kFixedChoices; ++i) {
        strncpy_s(s_labels[side][i], kLabelLen, kFixedLabels[i], _TRUNCATE);
        s_items[side][i] = s_labels[side][i];
        s_moveId[side][i] = 0;
    }
    s_count[side] = kFixedChoices;
    s_truncated[side] = false;
}

void BuildListForSide(int side, int tableIndex) {
    BuildFixedOnly(side);
    s_tableIndex[side] = tableIndex;
    s_listAvailable[side] = (tableIndex >= 0);
    s_built[side] = true;
    if (tableIndex < 0) return;

    const Mission::MoveNames::CharTable& ct = Mission::MoveNames::kCharTables[tableIndex];
    int n = kFixedChoices;
    for (int i = 0; i < ct.count; ++i) {
        const Mission::MoveNames::MoveNote& e = ct.entries[i];
        if (!EntryIsOfferable(ct, e)) continue;
        // A few characters carry two engine moves under one notation (Ikumi's
        // three supers and Kanna's 236236B each have a second variant). Showing
        // the same label twice is unusable, so the first one in the table wins
        // and the row below says some moves are not listed - the alternative is
        // a picker where two identical entries do different things.
        bool dupe = false;
        for (int k = kFixedChoices; k < n; ++k) {
            if (std::strcmp(s_items[side][k], e.note) == 0) { dupe = true; break; }
        }
        if (dupe) { s_truncated[side] = true; continue; }
        if (n >= kMaxChoices) { s_truncated[side] = true; break; }
        strncpy_s(s_labels[side][n], kLabelLen, e.note, _TRUNCATE);
        s_items[side][n] = s_labels[side][n];
        s_moveId[side][n] = (int)e.id;
        ++n;
    }
    s_count[side] = n;
}

void RefreshSide(int side, uintptr_t base) {
    const uintptr_t oppOffset = (side == 1) ? EFZ_BASE_OFFSET_P2
                                            : EFZ_BASE_OFFSET_P1;
    char name[20] = {0};
    if (!base || !ReadOpponentName(base, oppOffset, name, sizeof(name))) {
        // No opponent readable (out of a match, or characters still mid-init).
        // Fall back to the three fixed entries and forget the cached name, so a
        // real read rebuilds the list once the match is properly up instead of
        // matching the stale name and staying degraded.
        if (!s_built[side] || s_listAvailable[side] || s_oppName[side][0]) {
            BuildFixedOnly(side);
            s_oppName[side][0] = 0;
            s_tableIndex[side] = -1;
            s_listAvailable[side] = false;
            s_built[side] = true;
        }
        return;
    }
    if (s_built[side] && _stricmp(name, s_oppName[side]) == 0) return;  // unchanged

    strncpy_s(s_oppName[side], sizeof(s_oppName[side]), name, _TRUNCATE);
    BuildListForSide(side, TableIndexForName(name));

    // A pick only ever means something against the opponent it was made for.
    // Never try to carry it across by notation string: "236A" is a different
    // attack on every character.
    g_pickedMove[side].store(0, std::memory_order_release);
    g_pickedTableBias[side].store(0, std::memory_order_release);
    g_selectionStale[side].store(1, std::memory_order_release);
    const int choice = g_memoryChoice[side].load(std::memory_order_acquire);
    if (choice >= kFirstMoveChoice) {
        g_memoryChoice[side].store(MEMORY_OFF, std::memory_order_release);
    }
}

bool ReadWord(uintptr_t base, uintptr_t baseOffset, uintptr_t offset, int& out) {
    uintptr_t addr = ResolvePointer(base, baseOffset, offset);
    if (!addr) return false;
    uint16_t v = 0;
    if (!SafeReadMemory(addr, &v, sizeof(v))) return false;
    out = (int)v;
    return true;
}

bool ReadDword(uintptr_t base, uintptr_t baseOffset, uintptr_t offset, uint32_t& out) {
    uintptr_t addr = ResolvePointer(base, baseOffset, offset);
    if (!addr) return false;
    return SafeReadMemory(addr, &out, sizeof(out));
}

bool WriteDword(uintptr_t base, uintptr_t baseOffset, uintptr_t offset, uint32_t value) {
    uintptr_t addr = ResolvePointer(base, baseOffset, offset);
    if (!addr) return false;
    return SafeWriteMemory(addr, &value, sizeof(value));
}

// Mirrors the flat-colour blit the engine paints when it arms the cutter itself,
// so ALWAYS READY still shows a cue. The engine sometimes runs half this
// countdown; that condition is not modelled here, so the mirrored flash always
// uses the longer of the two.
void MirrorReadyFlash(uintptr_t base, uintptr_t baseOffset) {
    uintptr_t colorAddr = ResolvePointer(base, baseOffset, SAYURI_FLASH_COLOR_OFFSET);
    if (colorAddr) {
        uint8_t color = (uint8_t)SAYURI_FLASH_COLOR_WHITE;
        SafeWriteMemory(colorAddr, &color, sizeof(color));
    }
    WriteDword(base, baseOffset, SAYURI_FLASH_TIMER_OFFSET, (uint32_t)SAYURI_FLASH_TICKS);
}

void TickSide(int playerNum, uintptr_t base) {
    const int choice = g_memoryChoice[playerNum].load(std::memory_order_acquire);
    const int cutter = g_cutterMode[playerNum].load(std::memory_order_acquire);
    if (choice <= MEMORY_OFF && cutter == CUTTER_NORMAL) return;

    const uintptr_t baseOffset = (playerNum == 1) ? EFZ_BASE_OFFSET_P1
                                                  : EFZ_BASE_OFFSET_P2;

    // CHARACTER GATE. Both offsets this module touches are shared slots that
    // mean something else entirely on every other character, so nothing below
    // this line may run without it.
    if (!SideIsSayuri(base, baseOffset)) return;

    // MOVE-ID GATE, read HERE rather than reused from the top of the monitor
    // iteration: several hundred lines of other monitoring run in between and
    // the engine can have moved her on in that time.
    int moveID = 0;
    if (!ReadWord(base, baseOffset, MOVE_ID_OFFSET, moveID)) return;
    const bool inBlockstun = IsGroundedBlockstun(moveID);

    // Blockstun ENTRY edge. The poll runs several times per internal game frame,
    // so the transition into blockstun is never missed.
    const int prevMove = g_prevMoveId[playerNum].exchange(moveID, std::memory_order_acq_rel);
    const bool blockstunEntry = inBlockstun && !IsGroundedBlockstun(prevMove);

    // ----- REMEMBERED MOVE -------------------------------------------------
    if (choice == MEMORY_NOTHING) {
        uint32_t stored = 0;
        if (ReadDword(base, baseOffset, SAYURI_COUNTER_MEMORY_OFFSET, stored) &&
            stored != (uint32_t)SAYURI_COUNTER_MEMORY_EMPTY) {
            WriteDword(base, baseOffset, SAYURI_COUNTER_MEMORY_OFFSET,
                       (uint32_t)SAYURI_COUNTER_MEMORY_EMPTY);
        }
    } else if (choice == MEMORY_LAST_BLOCKED) {
        // Sampled ONCE, on the tick she enters grounded blockstun, which is the
        // only tick on which the opponent is reliably still inside the attack
        // that caused it. Sampling for the whole of blockstun would let the
        // attacker's landing, recovery or neutral state overwrite it - and a
        // memory holding neutral never matches anything again.
        //
        // The engine compares on the blockstun ENTRY frame, so a write made
        // during blockstun deliberately takes effect on the NEXT entry. That is
        // race-free by construction; do not try to beat the engine to it.
        if (blockstunEntry) {
            const uintptr_t oppOffset = (playerNum == 1) ? EFZ_BASE_OFFSET_P2
                                                         : EFZ_BASE_OFFSET_P1;
            int oppMove = 0;
            if (ReadWord(base, oppOffset, MOVE_ID_OFFSET, oppMove) &&
                oppMove >= kOpponentMoveIdMin && oppMove <= kOpponentMoveIdMax) {
                uint32_t stored = 0;
                if (ReadDword(base, baseOffset, SAYURI_COUNTER_MEMORY_OFFSET, stored) &&
                    stored != (uint32_t)oppMove) {
                    WriteDword(base, baseOffset, SAYURI_COUNTER_MEMORY_OFFSET,
                               (uint32_t)oppMove);
                }
            }
        }
    } else if (choice >= kFirstMoveChoice) {
        const int picked = g_pickedMove[playerNum].load(std::memory_order_acquire);
        const int pickedTable = g_pickedTableBias[playerNum].load(std::memory_order_acquire) - 1;
        if (picked > 0 && pickedTable >= 0 &&
            pickedTable == LiveOpponentTableIndex(base, playerNum)) {
            uint32_t stored = 0;
            if (ReadDword(base, baseOffset, SAYURI_COUNTER_MEMORY_OFFSET, stored) &&
                stored != (uint32_t)picked) {
                WriteDword(base, baseOffset, SAYURI_COUNTER_MEMORY_OFFSET,
                           (uint32_t)picked);
            }
        }
    }

    // ----- MAGICAL CUTTER --------------------------------------------------
    // The armed flag is generic per-move scratch outside grounded blockstun, so
    // this branch sits inside BOTH the character gate above and the move-ID gate
    // here. Inside those six states her own handler only ever reads it, which is
    // what makes a gated write safe.
    if (cutter == CUTTER_ALWAYS_READY && inBlockstun) {
        // The branch above can spend a little time reading the opponent, so the
        // gate is re-proved against a FRESH move ID immediately before the
        // write. Outside these six states the field is her own scratch and must
        // never carry a value this module put there.
        int guardMove = 0;
        if (!ReadWord(base, baseOffset, MOVE_ID_OFFSET, guardMove)) return;
        if (!IsGroundedBlockstun(guardMove)) return;

        uint32_t armed = 0;
        if (ReadDword(base, baseOffset, SAYURI_CUTTER_ARMED_OFFSET, armed) && armed != 1u) {
            if (WriteDword(base, baseOffset, SAYURI_CUTTER_ARMED_OFFSET, 1u)) {
                MirrorReadyFlash(base, baseOffset);
            }
        }
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Picker list
// ---------------------------------------------------------------------------

void RefreshMoveChoices() {
    // The dropdown popup holds the raw items pointer across frames, so rewriting
    // the labels underneath an open popup would change the visible list mid
    // selection. The rebuild is a no-op unless the opponent changed anyway.
    if (CustomMenu::Screens::IsPopupActive()) return;
    uintptr_t base = GetEFZBase();
    RefreshSide(1, base);
    RefreshSide(2, base);
}

const char* const* MoveChoiceItems(int playerNum) {
    const int p = ClampPlayer(playerNum);
    if (!s_built[p]) BuildFixedOnly(p);
    return s_items[p];
}

int MoveChoiceCount(int playerNum) {
    const int p = ClampPlayer(playerNum);
    if (!s_built[p]) BuildFixedOnly(p);
    return (s_count[p] < kFixedChoices) ? kFixedChoices : s_count[p];
}

int MoveIdForChoice(int playerNum, int idx) {
    const int p = ClampPlayer(playerNum);
    if (idx < kFixedChoices || idx >= s_count[p]) return 0;
    return s_moveId[p][idx];
}

bool OpponentListAvailable(int playerNum) {
    return s_listAvailable[ClampPlayer(playerNum)];
}

bool MoveListTruncated(int playerNum) {
    return s_truncated[ClampPlayer(playerNum)];
}

int ValidateChoiceIndex(int playerNum, int idx) {
    const int p = ClampPlayer(playerNum);
    if (idx < 0) return MEMORY_OFF;
    if (idx < kFixedChoices) {
        // OFF / NOTHING / LAST BLOCKED mean the same thing against every
        // character, so once the stored index is back at the fixed head there is
        // nothing left for an opponent change to invalidate. Clearing here is
        // what makes the FIRST pick off a freshly rebuilt list stick: the menus
        // validate the stored index before the user can open the dropdown, so
        // the flag is always down by the time a pick arrives.
        g_selectionStale[p].store(0, std::memory_order_release);
        return idx;
    }
    // Past the fixed head the index only means anything against the opponent the
    // list was built for. An opponent change, an index now out of range, or no
    // move behind it all collapse to OFF - never to a different move. The flag
    // deliberately stays raised here: the caller has not yet written the
    // collapsed value back, so a second reader must still reject the old index.
    if (g_selectionStale[p].load(std::memory_order_acquire) != 0) return MEMORY_OFF;
    if (idx >= s_count[p]) return MEMORY_OFF;
    if (s_moveId[p][idx] == 0) return MEMORY_OFF;
    return idx;
}

// ---------------------------------------------------------------------------
// Publishers
// ---------------------------------------------------------------------------

void SetMemory(int playerNum, int choiceIndex, int resolvedMoveId) {
    const int p = ClampPlayer(playerNum);
    if (choiceIndex < MEMORY_OFF) choiceIndex = MEMORY_OFF;
    // An index at or past the fixed head that resolved to no move ID means the
    // list changed underneath the stored selection. Fall back to OFF rather than
    // pinning whatever now happens to sit at that position.
    if (choiceIndex >= kFirstMoveChoice && resolvedMoveId <= 0) {
        choiceIndex = MEMORY_OFF;
        resolvedMoveId = 0;
    }
    if (choiceIndex < kFirstMoveChoice) resolvedMoveId = 0;

    const int table = (resolvedMoveId > 0) ? s_tableIndex[p] : -1;
    g_pickedMove[p].store(resolvedMoveId, std::memory_order_release);
    g_pickedTableBias[p].store(table + 1, std::memory_order_release);
    // This publish is a decision made against the list as it stands now, so the
    // opponent-change flag has been answered.
    g_selectionStale[p].store(0, std::memory_order_release);

    const int prev = g_memoryChoice[p].exchange(choiceIndex, std::memory_order_acq_rel);
    if (prev != choiceIndex) {
        LogOut(std::string("[SAYURI] P") + std::to_string(p) +
               " remembered move -> index " + std::to_string(choiceIndex) +
               " (moveID " + std::to_string(resolvedMoveId) + ")", true);
    }
}

void SetCutter(int playerNum, int mode) {
    const int p = ClampPlayer(playerNum);
    if (mode < CUTTER_NORMAL || mode >= CUTTER_COUNT) mode = CUTTER_NORMAL;
    const int prev = g_cutterMode[p].exchange(mode, std::memory_order_acq_rel);
    if (prev != mode) {
        LogOut(std::string("[SAYURI] P") + std::to_string(p) +
               " magical cutter -> " +
               (mode == CUTTER_ALWAYS_READY ? "ALWAYS READY" : "NORMAL"), true);
    }
}

int GetMemory(int playerNum) {
    return g_memoryChoice[ClampPlayer(playerNum)].load(std::memory_order_acquire);
}

int GetCutter(int playerNum) {
    return g_cutterMode[ClampPlayer(playerNum)].load(std::memory_order_acquire);
}

void ResetState() {
    for (int p = 1; p <= 2; ++p) {
        g_pickedMove[p].store(0, std::memory_order_release);
        g_pickedTableBias[p].store(0, std::memory_order_release);
        g_selectionStale[p].store(0, std::memory_order_release);
        g_prevMoveId[p].store(0, std::memory_order_release);
    }
}

void Tick() {
    // Master switch and the exact offline-Practice gates auto_airtech and the
    // Doppel module use.
    if (!g_featuresEnabled.load()) return;
    if (GetCurrentGameMode() != GameMode::Practice) return;
    if (IsNetplaySuspendActive()) return;

    // Cheapest possible exit when both rows are at their defaults on both sides.
    if (g_memoryChoice[1].load(std::memory_order_acquire) == MEMORY_OFF &&
        g_memoryChoice[2].load(std::memory_order_acquire) == MEMORY_OFF &&
        g_cutterMode[1].load(std::memory_order_acquire) == CUTTER_NORMAL &&
        g_cutterMode[2].load(std::memory_order_acquire) == CUTTER_NORMAL) {
        return;
    }

    uintptr_t base = GetEFZBase();
    if (!base) return;

    // Each side is polled through its own engine slot. Switching players is a
    // CONTROL swap only, so a Sayuri sitting in the P1 slot keeps P1 behaviour
    // no matter who holds the pad.
    //
    // docs/SAYURI_COUNTER_MEMORY.md section 5 records a SUSPECTED P1/P2
    // difference in how the ENGINE arms the memory (evaluation order inside
    // battleUpdate). It is explicitly unresolved and it concerns the engine's
    // own arming comparison, not this module: ALWAYS READY never reads the
    // opponent, so both sides are symmetric here. Do not model the asymmetry
    // until a live trace of the armed flag on each side settles it.
    TickSide(1, base);
    TickSide(2, base);
}

} // namespace SayuriCounter
