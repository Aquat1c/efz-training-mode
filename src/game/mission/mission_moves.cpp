#include "../../../include/game/mission/mission_moves.h"
#include "../../../include/core/constants.h"

#include <unordered_map>

namespace Mission::Moves {

namespace {

struct Entry { const char* notation; bool movement; };

// Universal move-IDs. Normals 200-209 are user-confirmed as identical across
// characters; the sub-200 system moves come from core/constants.h (the states
// our mod already classifies). Extend as more universal IDs are confirmed via
// the in-game move-ID inspector.
const std::unordered_map<int, Entry>& Table() {
    static const std::unordered_map<int, Entry> t = {
        // --- IC (system cancels IsAttackMove misses) ---
        {GROUND_IC_ID, {"IC",   false}},   // 167
        {AIR_IC_ID,    {"j.IC", false}},   // 171 (air IC / j.22C)

        // --- combo-relevant movement (only counted mid-combo) ---
        {STRAIGHT_JUMP_ID,     {"8",   true}},  // 4  neutral jump
        {FORWARD_JUMP_ID,      {"9",   true}},  // 5  jump forward
        {BACKWARD_JUMP_ID,     {"7",   true}},  // 6  jump back
        {DOUBLE_JUMP_NEUTRAL_ID,{"dj8",true}},  // 14
        {DOUBLE_JUMP_FWD_ID,   {"dj9", true}},  // 15
        {DOUBLE_JUMP_BACK_ID,  {"dj7", true}},  // 16
        // Airdashes are cancels (user-confirmed IDs): 165 forward, 166 backward.
        // (constants.h names these BACKWARD_DASH_START/RECOVERY - the empirical
        // in-game behaviour is the airdashes; captured mid-combo.)
        {165,                  {"j.66", true}}, // forward airdash
        {166,                  {"j.44", true}}, // backward airdash
        {FORWARD_DASH_START_ID,{"66",  true}},  // 163 dash

        // --- universal ground normals ---
        {200, {"5A", false}},
        {201, {"5B", false}},   // close 5B
        {202, {"5B", false}},   // far 5B (distinct ID, same notation)
        {203, {"5C", false}},
        {204, {"2A", false}},
        {205, {"2B", false}},
        {206, {"2C", false}},

        // --- universal air normals ---
        {207, {"j.A", false}},
        {208, {"j.B", false}},
        {209, {"j.C", false}},
    };
    return t;
}

} // namespace

const char* Notation(int moveId) {
    const auto& t = Table();
    auto it = t.find(moveId);
    return it != t.end() ? it->second.notation : "";
}

bool IsKnownComboMove(int moveId) { return Table().count(moveId) != 0; }

bool IsMovementMove(int moveId) {
    const auto& t = Table();
    auto it = t.find(moveId);
    return it != t.end() && it->second.movement;
}

} // namespace Mission::Moves
