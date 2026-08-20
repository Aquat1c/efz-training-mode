#pragma once
//
// FrameBar - per-player strip showing each sampled state
// (idle/walk/jump/attack/blockstun/hitstun/etc.) color-coded.
//
// Concept ported from MBAACC Extended Training Mode by fangdreth (see
// reference_projects/MBAACC-Extended-Training-Mode-main/Extended-Training-Mode-DLL/FrameBar.cpp).
// The MBAACC implementation reads attack-data pointers and explicit
// inactionable counters; EFZ exposes the equivalent pieces through frame data
// blocks, collision/RG timers, and a few fallback move-ID classifiers.
//
// Sampling cadence: subframes by default, optionally once per visual frame.
// Render cadence: once per EndScene from DirectDrawHook::RenderD3D9Overlays
// when toggled on (config flag), independent of menu visibility.

#include <atomic>
#include <cstdint>

namespace FrameBar {

// Color-coded cell categories. Order matters for the legend strip.
enum class Cat : uint8_t {
    None = 0,       // empty / off
    Neutral,        // idle / actionable
    WalkFwd,
    WalkBack,
    PreJump,
    JumpNeutral,
    JumpFwd,
    JumpBack,
    DoubleJumpNeutral,
    DoubleJumpFwd,
    DoubleJumpBack,
    Falling,
    Landing,
    Crouch,         // crouching
    DashFwd,        // forward dash startup/recovery
    DashBack,       // backward dash startup/recovery
    AirDashNeutral,
    AirDashFwd,
    AirDashBack,
    AttackStartup,  // attack move, before collision-capable boxes are active
    AttackActive,   // character attack frame with collision-capable boxes
    AttackRecovery, // attack move, after active boxes ended
    Blockstun,
    Hitstun,
    Launched,
    AirtechFwd,
    AirtechBack,
    Groundtech,
    SpecialStun,    // fire / electric / frozen
    Thrown,
    RG,             // recoil-guard windows
    SuperFlash,     // IC/super screen freeze
    HitstopShared,  // both players in hit-hitstop
    COUNT_
};

constexpr int kBarMemory = 240;     // ring buffer length (subframe samples by default)

// Master enable. Persisted via Config::SetSetting("General", "showFrameBar").
extern std::atomic<bool> g_enabled;

// Called from FrameDataMonitor. Reads move-IDs and
// edge-detects state to populate the next ring-buffer cell.
void TickSample();

// Called from RenderD3D9Overlays. Draws the bar near the bottom-center
// of the inner 4:3 game area, regardless of menu visibility.
struct DrawCtx {
    float ox;        // letterbox offset X within RT
    float oy;        // letterbox offset Y within RT
    float scale;     // game-area scale factor (1.0 = native 640x480)
};
void Render(const DrawCtx& ctx);

// Reset ring buffers (call on round start / character load / menu open).
void Reset();

} // namespace FrameBar
