#pragma once

#include <cstdint>

// Turbulence sources found in the client's map data by waterfall-scan (waterfalls, the splash and rapids decals
// designers put under them, fountains). Each is the bottom edge of the model's box in world yards, so a wide
// waterfall churns along its whole width.
struct WaterfallEmitter
{
    uint16_t map;
    uint8_t kind;
    uint8_t indoor; // inside a WMO: levels can stack, so it needs a tight height band
    float a[3];
    float b[3];
    float thickness;
    float height;
};

constexpr uint8_t kWaterfallFall = 0;
constexpr uint8_t kWaterfallDecal = 1;
constexpr uint8_t kWaterfallFountain = 2;
// Where a river pours over the edge (a fall's "hat" model): a -> b is the lip across the river at its surface height,
// the flow is the right-hand normal of a -> b ((b - a) turned -90 degrees), thickness is how far upstream the
// approach shows. Never churn or foam: the water there runs smooth and fast (features in the water shader).
constexpr uint8_t kWaterfallLip = 3;
// A splash decal at a fall's foot (the client draws its ripples there): churns across its pool like a decal, foams
// nothing, the fall foaming where its water lands. Foamed along its own centre line it drew a second white band out
// in the pool; dropped, the pool lost its broad ripples ("can we keep the splash decals if they're placed correctly
// and our ripples are visible underneath them", the user, 2026-10-11).
constexpr uint8_t kWaterfallSplash = 4;
constexpr float kMaxDecalFoamRadius = 6.0f; // yd
// A fall's foam reaches at most this far from its base line. Its radius grows with the model's box, which for a
// combined cliff-and-fall model is no measure of the stream: Dragonblight's dragonblight_combin_waterfall_06 (box 56 yd
// thick) foamed 30 yd either side of its 84 yd base, a white band under its pool's own foam decal ("this fall has a
// white foam stripe underneath the regular foam decal", the user, 2026-10-11). Glacial Falls' (4.8) is under it.
constexpr float kMaxFallFoamRadius = 7.0f; // yd

constexpr uint32_t kMaxActiveWaterfalls = 16;
constexpr uint32_t kMaxActiveLips = 8;
// A base may lie this far below / above the camera target outdoors (a fall seen from its cliff top or from below).
// Indoors levels stack (the Underbelly's lower sewer is 51 yd under the canal), so the band is tighter, but it
// still reaches down to the water from a raised pier or walkway.
constexpr float kWaterfallOutdoorBelow = 80.0f;
constexpr float kWaterfallOutdoorAbove = 20.0f;
constexpr float kWaterfallIndoorBelow = 30.0f;
constexpr float kWaterfallIndoorAbove = 10.0f;

const WaterfallEmitter* WaterfallTable(uint32_t& count);

// The nearest emitter on `map` regardless of the height band, for the log: false when none is within `range`.
bool NearestWaterfall(int map, const float centre[2], float range, float& distance, float& baseHeight);

// Emitters on `map` whose base comes within `range` yards (ground distance) of `centre`, inside the height band
// around `referenceHeight`, nearest first. Lips are never among them: CollectLips finds those.
uint32_t CollectWaterfalls(int map, const float centre[2], float referenceHeight, float range,
                           const WaterfallEmitter** out, uint32_t capacity);
// The same lookup for the lips only.
uint32_t CollectLips(int map, const float centre[2], float referenceHeight, float range, const WaterfallEmitter** out,
                     uint32_t capacity);

// The white water drawn at an emitter's base (the water shader's c70/c86 pair): segment = the base line, shortened
// to the middle of a narrow fall's box (a model's box is wider than its stream); shape = (radius around it, strength
// 0..1, base height, height band). Sized by the fall's width and height, so a small sewer outflow foams a little
// and a tall, wide fall a lot; a fountain's stream by its drop, within a cap.
void FallFoamShape(const WaterfallEmitter& emitter, float segment[4], float shape[4]);
