#pragma once

#include <cstddef>
#include <cstdint>

namespace CollisionDisplay {

struct Status {
    bool hitEnabled = false;
    bool hurtEnabled = false;
    bool collisionEnabled = false;
    bool projectileInteractionsEnabled = false;
    uint32_t toggleCount = 0;
};

enum OverlayShape : uint8_t {
    ShapeRectangle = 0,
    ShapeDot = 1,
};

// Virtual 640x480 overlay rectangle. Colors are stored as AARRGGBB.
struct OverlayBox {
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;
    uint32_t fillArgb = 0;
    uint32_t outlineArgb = 0;
    uint8_t shape = ShapeRectangle;
};

void Initialize();
void Shutdown();

bool IsAnyLayerEnabled();
Status GetStatus();
void SetLayerEnabled(int layerIndex, bool enabled); // 0=hit, 1=hurt, 2=collision/push, 3=projectile interactions

void RebuildFrame();
std::size_t GetOverlayBoxCount();
bool GetOverlayBox(std::size_t index, OverlayBox* outBox);

// Called from the Revival Practice hotkey dispatcher gate. Returns true when
// the key was consumed by our replacement display and must not reach Revival.
bool ShouldSuppressRevivalHotkey(void* practiceController, int key);

// Fixed-layout, read-only view of EFZ's 64-slot per-player entity ring.  A
// successful ProbeProjectileRing call writes exactly kProjectileRingSlotCapacity
// entries. `readable == false` means an active entry could not be sampled and
// consumers must retain their prior state rather than infer a despawn.
constexpr std::size_t kProjectileRingSlotCapacity = 64;
struct ProjectileRingSlotProbe {
    int slot = -1;
    bool readable = false;
    bool alive = false;
    uint16_t pattern = 0;
    uint16_t frame = 0;
    uint16_t frameTick = 0;
    double x = 0.0;
    double y = 0.0;
    int16_t life = 0;
    uint32_t destroyed = 0;
};

// Reads the alive table once and each active entry at most once. No transforms,
// allocation, pattern filtering, or gameplay mutation occurs. `outCapacity`
// must be at least kProjectileRingSlotCapacity; false means no ring snapshot
// was available. A successful sample is still an endpoint observation, not an
// atomic cross-slot event ordering guarantee.
bool ProbeProjectileRing(int playerIndex, ProjectileRingSlotProbe* outSlots,
                         std::size_t outCapacity);

// Read-only probe of a player's projectile ring: returns the max `life` among that
// player's alive projectiles whose pattern == `pattern`, or -1 if a complete
// snapshot proves none is alive. kProjectileLifeProbeUnavailable means the
// sample was incomplete and must not be interpreted as a despawn.
// Used to detect a projectile being destroyed (life 1->0), e.g. a shield blocking a
// fireball. playerIndex is 1 or 2. Uses the same one-table/one-read-per-active-slot
// bounded sampler as ProbeProjectileRing; no transforms or gameplay mutation.
constexpr int kProjectileLifeProbeUnavailable = -2;
int ProbeProjectileLifeForPattern(int playerIndex, uint16_t pattern);

} // namespace CollisionDisplay
