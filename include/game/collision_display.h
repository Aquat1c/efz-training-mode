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

} // namespace CollisionDisplay
