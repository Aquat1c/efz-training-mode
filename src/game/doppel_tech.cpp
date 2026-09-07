#include "../include/game/doppel_tech.h"

#include "../include/core/constants.h"
#include "../include/core/memory.h"
#include "../include/core/logger.h"
#include "../include/game/game_state.h"
#include "../include/utils/utilities.h"   // GetEFZBase, g_featuresEnabled
#include "../include/utils/network.h"     // IsNetplaySuspendActive

#include <atomic>
#include <cstdint>
#include <cstdlib>   // rand
#include <cstring>   // _strnicmp
#include <string>

namespace DoppelTech {

namespace {

// ---------------------------------------------------------------------------
// Published settings.
//
// File-scope atomics, matching auto_airtech.cpp. The shipping v141_xp build
// compiles with /Zc:threadSafeInit-, so a function-local static on a path the
// monitor thread reaches would not be thread-safe. Index 0 is unused so callers
// can pass playerNum 1/2 directly.
//
// Every array below relies on static-storage zero initialisation, which lands on
// exactly the wanted defaults (MODE_OFF == 0, STAGE_ALL == 0, "not yet written"
// == 0). Braced initialisers are avoided on purpose: std::atomic has a deleted
// copy constructor, so pre-C++17 copy-list-initialisation of array elements is
// not portable.
std::atomic<int> g_mode[3];    // MODE_OFF
std::atomic<int> g_stage[3];   // STAGE_ALL

// ---------------------------------------------------------------------------
// Per-side polling state. Only the monitor thread drives Tick(), but ResetState()
// and the Set* publishers are callable from elsewhere, so these are atomics too.
//
// g_stageDone    : 1 once this stage entry is finished (written, or handed to
//                  the opponent because they got their escape in first). 0 means
//                  still armed; stored in this polarity so the zero-init default
//                  is "armed".
// g_prevTechMove : the tech-state moveID observed on the previous tick, so that
//                  the 254 -> 255 -> 300/306 progression re-arms at every step
//                  and not only when Doppel leaves the follow-up chain.
// g_rolledValue  : the RANDOM choice for the current stage entry (0 = not rolled).
// g_ourValue     : the value THIS module last wrote for the current stage entry
//                  (0 = we have written nothing yet). Needed to tell our own
//                  write apart from an opponent press when re-reading the latch.
// ---------------------------------------------------------------------------
std::atomic<int> g_stageDone[3];
std::atomic<int> g_prevTechMove[3];
std::atomic<int> g_rolledValue[3];
std::atomic<int> g_ourValue[3];

inline int ClampPlayer(int playerNum) {
    return (playerNum == 2) ? 2 : 1;
}

// The four states in which DOPPEL_TECH_LATCH_OFFSET carries tech-choice meaning.
// Outside them the same DWORD is generic per-move scratch, so this gate is a
// correctness requirement rather than an optimisation.
inline bool IsTechState(int moveID) {
    return moveID == DOPPEL_MOVE_CAPTURE_HOLD ||
           moveID == DOPPEL_MOVE_MAIDEN_CRASH ||
           moveID == DOPPEL_MOVE_BARRAGE ||
           moveID == DOPPEL_MOVE_INNER_SOUL;
}

inline bool StageSelected(int stage, int moveID) {
    switch (stage) {
        case STAGE_1: return moveID == DOPPEL_MOVE_CAPTURE_HOLD;
        case STAGE_2: return moveID == DOPPEL_MOVE_MAIDEN_CRASH;
        case STAGE_3: return moveID == DOPPEL_MOVE_BARRAGE ||
                             moveID == DOPPEL_MOVE_INNER_SOUL;
        case STAGE_ALL:
        default:      return true;   // already narrowed by IsTechState()
    }
}

// ---------------------------------------------------------------------------
// When it is both safe and meaningful to touch the latch, expressed in the
// state's own animation frame index (+0x0A) and subframe counter (+0x0C).
//
// This gate is what makes the feature work at all, and it is why the module does
// NOT write the instant it first sees one of the four states:
//
//  1. The engine RE-ARMS the latch itself a couple of engine frames INTO the
//     state, not on the transition into it:
//        * 255 clears at frame index 1, subframe 0;
//        * 300 and 306 clear on their very first execution (index 0, subframe 0);
//        * 254 has no clear of its own - it is cleared by the state-entry reset.
//     None of the transitions between these states clears it. The monitor polls
//     three times per engine frame, so a write made on first sight always landed
//     in the gap before that clear and was simply wiped - which silently made
//     the Stage 2 and Stage 3 selections do nothing at all.
//  2. It also stops the module from writing before the engine has even begun
//     looking at the opponent's buttons. The engine only samples the opponent
//     inside these same index windows, so waiting for them is what gives the
//     opponent a real chance to get their own escape in first.
//
// Waiting has a third benefit: a non-zero latch observed at this point really is
// the opponent's press, and not this module's own leftover from the previous
// stage of the same chain (which the engine has by then cleared).
bool InWriteWindow(int moveID, int frameIndex, int subFrame) {
    switch (moveID) {
        case DOPPEL_MOVE_CAPTURE_HOLD:
            // The opponent is only sampled on this single animation index; it is
            // a long one, so there is plenty of room.
            return frameIndex == 2;
        case DOPPEL_MOVE_MAIDEN_CRASH:
            // Escape window is index 1..9, and index 1 / subframe 0 is exactly
            // where the engine clears, so start one subframe later.
            if (frameIndex < 1 || frameIndex > 9) return false;
            return frameIndex > 1 || subFrame > 0;
        case DOPPEL_MOVE_BARRAGE:
        case DOPPEL_MOVE_INNER_SOUL:
            // Escape window is index 0..9, cleared at index 0 / subframe 0.
            if (frameIndex > 9) return false;
            return frameIndex > 0 || subFrame > 0;
        default:
            return false;
    }
}

// Case-insensitive match on the resource name in the player struct. Deliberately
// stricter than CharacterSettings::GetCharacterID(), whose partial-match fallback
// could let a name containing "nanase" resolve to Rumi, and free of the per-tick
// std::string allocation that helper needs.
bool SideIsDoppel(uintptr_t base, uintptr_t baseOffset) {
    uintptr_t nameAddr = ResolvePointer(base, baseOffset, CHARACTER_NAME_OFFSET);
    if (!nameAddr) return false;
    char name[16] = {0};
    if (!SafeReadMemory(nameAddr, name, sizeof(name) - 1)) return false;
    name[sizeof(name) - 1] = '\0';
    // Prefix compare: no other character resource name starts with "exnanase",
    // and Rumi's "nanase" does not, so this is exact in practice while staying
    // tolerant of anything the engine leaves after the name.
    return _strnicmp(name, "exnanase", 8) == 0;
}

// ALWAYS: escape whichever branch Doppel actually committed to. Returns 0 when
// the recognised command token is not yet one of the six follow-up encodings,
// which means the stage is still open and the caller must retry next tick.
int ResolveAlwaysValue(uintptr_t base, uintptr_t baseOffset) {
    uintptr_t tokenAddr = ResolvePointer(base, baseOffset, MOTION_TOKEN_OFFSET);
    if (!tokenAddr) return 0;
    uint16_t token = 0;
    if (!SafeReadMemory(tokenAddr, &token, sizeof(token))) return 0;

    switch (token) {
        // A-line follow-ups are refused by a C latch.
        case DOPPEL_TOKEN_A_QCF:
        case DOPPEL_TOKEN_A_QCB:
        case DOPPEL_TOKEN_A_SUPER_QCF:
        case DOPPEL_TOKEN_A_SUPER_QCB:
            return DOPPEL_TECH_C;
        // B-line follow-ups are refused by a B latch.
        case DOPPEL_TOKEN_B_QCF:
        case DOPPEL_TOKEN_B_QCB:
        case DOPPEL_TOKEN_B_SUPER_QCF:
        case DOPPEL_TOKEN_B_SUPER_QCB:
            return DOPPEL_TECH_B;
        default:
            return 0;   // not committed yet - stay armed
    }
}

// Returns the latch value to write, or 0 to write nothing this tick.
int ResolveValue(int playerNum, int mode, uintptr_t base, uintptr_t baseOffset) {
    switch (mode) {
        case MODE_NEVER:  return DOPPEL_TECH_A;
        case MODE_TECH_B: return DOPPEL_TECH_B;
        case MODE_TECH_C: return DOPPEL_TECH_C;
        case MODE_ALWAYS: return ResolveAlwaysValue(base, baseOffset);
        case MODE_RANDOM: {
            // Rolled ONCE per stage entry: g_rolledValue is cleared whenever the
            // tech-state moveID changes or the chain ends.
            int rolled = g_rolledValue[playerNum].load(std::memory_order_acquire);
            if (rolled != DOPPEL_TECH_B && rolled != DOPPEL_TECH_C) {
                // Same RNG the rest of the mod uses for coin flips
                // (auto_action.cpp, random_rg.cpp): the CRT generator, already
                // seeded by the process. No fresh seed, no std::random_device.
                rolled = ((rand() & 1) != 0) ? DOPPEL_TECH_C : DOPPEL_TECH_B;
                g_rolledValue[playerNum].store(rolled, std::memory_order_release);
            }
            return rolled;
        }
        default: return 0;
    }
}

void ClearSide(int playerNum) {
    g_stageDone[playerNum].store(0, std::memory_order_release);
    g_prevTechMove[playerNum].store(0, std::memory_order_release);
    g_rolledValue[playerNum].store(0, std::memory_order_release);
    g_ourValue[playerNum].store(0, std::memory_order_release);
}

const char* ModeName(int mode) {
    switch (mode) {
        case MODE_NEVER:  return "NEVER";
        case MODE_TECH_B: return "TECH B";
        case MODE_TECH_C: return "TECH C";
        case MODE_ALWAYS: return "ALWAYS";
        case MODE_RANDOM: return "RANDOM";
        default:          return "OFF";
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

void TickSide(int playerNum, uintptr_t base) {
    const int mode = g_mode[playerNum].load(std::memory_order_acquire);
    if (mode <= MODE_OFF || mode >= MODE_COUNT) {
        ClearSide(playerNum);
        return;
    }

    const uintptr_t baseOffset = (playerNum == 1) ? EFZ_BASE_OFFSET_P1
                                                  : EFZ_BASE_OFFSET_P2;

    // Character gate. The latch offset is meaningless for anyone else.
    if (!SideIsDoppel(base, baseOffset)) {
        ClearSide(playerNum);
        return;
    }

    // moveID gate, read HERE rather than reused from the top of the monitor
    // iteration: several hundred lines of other monitoring run in between and
    // the engine can have moved Doppel on in that time. Since the latch offset
    // is shared per-move scratch outside these four states, the gate has to be
    // evaluated against a fresh read.
    int moveID = 0;
    if (!ReadWord(base, baseOffset, MOVE_ID_OFFSET, moveID)) return;

    if (!IsTechState(moveID)) {
        // Leaving the follow-up chain re-arms for the next capture.
        ClearSide(playerNum);
        return;
    }

    // Re-arm on every CHANGE between tech states, not only on leaving the set:
    // Doppel walks 254 -> 255 -> 300/306 and the engine re-arms the latch at
    // each stage, so each stage deserves its own decision.
    const int prev = g_prevTechMove[playerNum].load(std::memory_order_acquire);
    if (prev != moveID) {
        g_prevTechMove[playerNum].store(moveID, std::memory_order_release);
        g_stageDone[playerNum].store(0, std::memory_order_release);
        g_rolledValue[playerNum].store(0, std::memory_order_release);
        g_ourValue[playerNum].store(0, std::memory_order_release);
    }

    // This stage entry is already settled.
    if (g_stageDone[playerNum].load(std::memory_order_acquire) != 0) return;

    const int stage = g_stage[playerNum].load(std::memory_order_acquire);
    if (!StageSelected(stage, moveID)) return;

    // Wait for the point in the state where the engine has finished re-arming
    // the latch and has started looking at the opponent's buttons.
    int frameIndex = 0, subFrame = 0;
    if (!ReadWord(base, baseOffset, CURRENT_FRAME_INDEX_OFFSET, frameIndex)) return;
    if (!ReadWord(base, baseOffset, STATE_SUBFRAME_COUNTER_OFFSET, subFrame)) return;
    if (!InWriteWindow(moveID, frameIndex, subFrame)) return;

    uintptr_t latchAddr = ResolvePointer(base, baseOffset, DOPPEL_TECH_LATCH_OFFSET);
    if (!latchAddr) return;

    uint32_t latch = 0;
    if (!SafeReadMemory(latchAddr, &latch, sizeof(latch))) return;

    const int ours = g_ourValue[playerNum].load(std::memory_order_acquire);
    if (latch != (uint32_t)DOPPEL_TECH_NONE && (int)latch != ours) {
        // Somebody else got their escape in for this stage. Never overwrite:
        // that is what preserves "first press wins", so an opponent who presses
        // first still beats the setting.
        g_stageDone[playerNum].store(1, std::memory_order_release);
        return;
    }

    const int value = ResolveValue(playerNum, mode, base, baseOffset);
    if (value == 0) {
        // ALWAYS with no follow-up committed yet. Stay armed and re-evaluate on
        // the next tick; the stage window is still open.
        return;
    }

    if ((int)latch == value) {
        // Already holding what we want. ALWAYS stays armed so a change of
        // follow-up later in the same window is still tracked; every other mode
        // is a fixed choice and is finished.
        if (mode != MODE_ALWAYS) g_stageDone[playerNum].store(1, std::memory_order_release);
        return;
    }

    uint32_t out = (uint32_t)value;
    if (!SafeWriteMemory(latchAddr, &out, sizeof(out))) return;
    g_ourValue[playerNum].store(value, std::memory_order_release);

    // ALWAYS deliberately stays armed. The engine only reads the follow-up
    // Doppel committed to at the very END of the state, and her motion
    // recogniser rewrites that choice every frame, so a value picked from the
    // first frame of the window is routinely not the one she actually uses.
    // Re-checking keeps the escape aimed at the follow-up she really performs;
    // the written value only ever changes when her committed follow-up changes,
    // so this is still one effective decision per stage.
    if (mode != MODE_ALWAYS) g_stageDone[playerNum].store(1, std::memory_order_release);

    LogOut(std::string("[DOPPEL-TECH] P") + std::to_string(playerNum) +
           " mode=" + ModeName(mode) +
           " moveID=" + std::to_string(moveID) +
           " idx=" + std::to_string(frameIndex) +
           " wrote=" + std::to_string(value),
           detailedLogging.load());
}

} // namespace

void SetMode(int playerNum, int mode) {
    const int p = ClampPlayer(playerNum);
    if (mode < MODE_OFF || mode >= MODE_COUNT) mode = MODE_OFF;
    const int prev = g_mode[p].exchange(mode, std::memory_order_acq_rel);
    if (prev != mode) {
        ClearSide(p);
        LogOut(std::string("[DOPPEL-TECH] P") + std::to_string(p) +
               " follow-up tech mode -> " + ModeName(mode), true);
    }
}

void SetStage(int playerNum, int stage) {
    const int p = ClampPlayer(playerNum);
    if (stage < STAGE_ALL || stage >= STAGE_COUNT) stage = STAGE_ALL;
    const int prev = g_stage[p].exchange(stage, std::memory_order_acq_rel);
    if (prev != stage) ClearSide(p);
}

int GetMode(int playerNum)  { return g_mode[ClampPlayer(playerNum)].load(std::memory_order_acquire); }
int GetStage(int playerNum) { return g_stage[ClampPlayer(playerNum)].load(std::memory_order_acquire); }

void ResetState() {
    ClearSide(1);
    ClearSide(2);
}

void Tick() {
    // Master switch and the exact offline-Practice gates used by auto_airtech.
    if (!g_featuresEnabled.load()) return;
    if (GetCurrentGameMode() != GameMode::Practice) return;
    if (IsNetplaySuspendActive()) return;

    // Cheapest possible exit when neither side has the feature on.
    if (g_mode[1].load(std::memory_order_acquire) == MODE_OFF &&
        g_mode[2].load(std::memory_order_acquire) == MODE_OFF) {
        return;
    }

    uintptr_t base = GetEFZBase();
    if (!base) return;

    // Both sides are polled from the player index the row belongs to, so a
    // swapped-side Doppel behaves identically to a P2 Doppel.
    TickSide(1, base);
    TickSide(2, base);
}

} // namespace DoppelTech
