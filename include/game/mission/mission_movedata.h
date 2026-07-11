#pragma once
//
// Per-character move-ID reference, baked from the retail .pat by
// tools/pat_movedata and shipped as assets/movedata/<internalName>.json.
//
// Loads the active character's profile (resolved via CharacterSettings internal
// name) and answers class / attack / special queries. Nanase and Ikumi use a
// conservative base+alternate profile union because their runtime table can swap
// without changing character ID; GetClass is therefore merged metadata, not proof
// of the table active on this exact tick. The mission inspector uses it to
// LABEL each live move-ID (so authors read "251 -> special" on screen), and the
// recorder uses it to CAPTURE dmg-0 special spawners (e.g. a 236X projectile cast)
// that the generic attack classifier misses.
//
// The .pat gives move-ID + strength class, NOT the input motion — notation still
// comes from the recorder. See shared_documentation/MOVEID_EXTRACTION_AND_MAPPING.md.
//
#include <vector>

namespace Mission::MoveData {

// Coarse strength class (mirrors the JSON "class" field).
enum class Cls {
    Unknown, System, A, B, C, Command, Special, Super, Projectile, SuperEntity, Entity
};

namespace Detail {

constexpr int MergePriority(Cls c, int id) {
    if (id >= 400) {
        return c == Cls::SuperEntity ? 4
             : c == Cls::Projectile  ? 3
             : c == Cls::Entity      ? 2
             : c == Cls::Unknown     ? 0 : 1;
    }
    return c == Cls::Super   ? 7
         : c == Cls::Special ? 6
         : c == Cls::Command ? 5
         : c == Cls::C       ? 4
         : c == Cls::B       ? 3
         : c == Cls::A       ? 2
         : c == Cls::System  ? 1 : 0;
}

constexpr Cls MergeProfileClass(Cls existing, Cls incoming, int id) {
    return MergePriority(incoming, id) > MergePriority(existing, id)
        ? incoming : existing;
}

constexpr bool MergeProfileAttack(bool existing, bool incoming) {
    return existing || incoming;
}

static_assert(MergeProfileClass(Cls::Super, Cls::System, 301) == Cls::Super,
              "alternate union must retain special/super capture");
static_assert(MergeProfileClass(Cls::Projectile, Cls::Entity, 421) == Cls::Projectile,
              "alternate union must retain attack-capable entity class");

} // namespace Detail

// Load (and cache) the reference for a character ID. Cheap to call every frame:
// the file is read once per character, then answered from memory. Returns false if
// the character has no required baked profile (queries then return Unknown / empty).
bool EnsureLoaded(int charId);

Cls         GetClass(int charId, int moveId);
const char* ClassName(Cls c);            // short label e.g. "special"
bool        IsAttack(int charId, int moveId);
bool        IsSpecialOrSuper(int charId, int moveId);
// Metadata query only. Never use this to gate raw ring-event eligibility: an
// unknown live >=400 pattern must still be captured and displayed as #<id>.
bool        IsKnownEntityPattern(int charId, int moveId);
bool        IsCommonSystemHelperCandidate(int moveId); // presentation hint: #495..#499

// The character's special+super move-IDs, ascending — the mapping checklist.
// Empty if the character isn't loaded.
const std::vector<int>& Specials(int charId);
const std::vector<int>& EntityPatterns(int charId);
const std::vector<int>& AttackEntityPatterns(int charId);

} // namespace Mission::MoveData
