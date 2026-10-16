#include "water_falls.h"

#include "waterfall_data.inc"

#include <algorithm>
#include <cmath>
#include <iterator>

namespace
{
float GroundDistanceToSegment(const WaterfallEmitter& e, const float p[2])
{
    const float dx = e.b[0] - e.a[0], dy = e.b[1] - e.a[1];
    const float lengthSquared = dx * dx + dy * dy;
    float t = lengthSquared > 0.0f ? ((p[0] - e.a[0]) * dx + (p[1] - e.a[1]) * dy) / lengthSquared : 0.0f;
    t = std::clamp(t, 0.0f, 1.0f);
    return std::hypot(p[0] - (e.a[0] + t * dx), p[1] - (e.a[1] + t * dy));
}

bool InHeightBand(const WaterfallEmitter& e, float referenceHeight)
{
    const float base = 0.5f * (e.a[2] + e.b[2]);
    if (e.indoor)
        return base >= referenceHeight - kWaterfallIndoorBelow && base <= referenceHeight + kWaterfallIndoorAbove;
    return base >= referenceHeight - kWaterfallOutdoorBelow && base <= referenceHeight + kWaterfallOutdoorAbove;
}

const WaterfallEmitter* FirstOnMap(int map)
{
    return std::lower_bound(std::begin(kWaterfallEmitters), std::end(kWaterfallEmitters), map,
                            [](const WaterfallEmitter& e, int m) { return e.map < m; });
}
}

bool NearestWaterfall(int map, const float centre[2], float range, float& distance, float& baseHeight)
{
    distance = range;
    bool found = false;
    if (map < 0 || map > 0xFFFF || !std::isfinite(centre[0]) || !std::isfinite(centre[1]))
        return false;
    for (const WaterfallEmitter* e = FirstOnMap(map); e != std::end(kWaterfallEmitters) && e->map == map; ++e)
    {
        if (e->kind == kWaterfallLip)
            continue;
        const float d = GroundDistanceToSegment(*e, centre);
        if (d <= distance)
        {
            distance = d;
            baseHeight = 0.5f * (e->a[2] + e->b[2]);
            found = true;
        }
    }
    return found;
}

const WaterfallEmitter* WaterfallTable(uint32_t& count)
{
    count = static_cast<uint32_t>(std::size(kWaterfallEmitters));
    return kWaterfallEmitters;
}

namespace
{
// The nearest `capacity` emitters within `range` in the height band: the lips, or everything else.
uint32_t CollectNearest(int map, const float centre[2], float referenceHeight, float range,
                        const WaterfallEmitter** out, uint32_t capacity, bool lips)
{
    if (map < 0 || map > 0xFFFF || !std::isfinite(centre[0]) || !std::isfinite(centre[1]) ||
        !std::isfinite(referenceHeight))
        return 0;
    const WaterfallEmitter* first = FirstOnMap(map);
    float distances[kMaxActiveWaterfalls] = {};
    uint32_t count = 0;
    capacity = std::min(capacity, kMaxActiveWaterfalls);
    for (const WaterfallEmitter* e = first; e != std::end(kWaterfallEmitters) && e->map == map; ++e)
    {
        if ((e->kind == kWaterfallLip) != lips)
            continue;
        const float distance = GroundDistanceToSegment(*e, centre);
        if (distance > range || !InHeightBand(*e, referenceHeight))
            continue;
        // Keep the nearest `capacity`, sorted.
        uint32_t at = count;
        while (at > 0 && distances[at - 1] > distance)
            --at;
        if (at >= capacity)
            continue;
        const uint32_t last = std::min(count, capacity - 1);
        for (uint32_t i = last; i > at; --i)
        {
            out[i] = out[i - 1];
            distances[i] = distances[i - 1];
        }
        out[at] = e;
        distances[at] = distance;
        count = std::min(count + 1, capacity);
    }
    return count;
}
}

void FallFoamShape(const WaterfallEmitter& e, float segment[4], float shape[4])
{
    const float width = std::hypot(e.b[0] - e.a[0], e.b[1] - e.a[1]);
    float radius = 0.0f, strength = 0.0f, keep = 1.0f;
    switch (e.kind)
    {
    case kWaterfallFall:
        // Proportional to the fall: a fifth of its width and a third of its thickness around the base line, and
        // stronger with width and drop. A model's box is wider than its stream: narrow falls keep the middle 70%.
        radius = std::min(std::clamp(0.15f * width + 0.3f * e.thickness + 0.5f, 0.75f, 0.5f * e.thickness + 2.0f),
                          kMaxFallFoamRadius);
        strength = std::clamp(0.35f + width / 25.0f + e.height / 80.0f, 0.35f, 1.0f);
        keep = std::clamp(0.7f + width / 40.0f, 0.7f, 1.0f);
        break;
    case kWaterfallDecal:
        // Capped: a decal scaled up across a whole pool (Glacial Falls has two, 62 and 98 yd deep) foamed it all.
        radius = std::min(0.5f * e.thickness + 1.0f, kMaxDecalFoamRadius);
        strength = 0.6f;
        break;
    case kWaterfallSplash:
        // Churn only: its fall foams where the water lands.
        radius = 0.75f;
        strength = 0.0f;
        break;
    default:
        // A fountain's stream: broader and whiter with its drop, so Dalaran's 9 yd sheet foams like the
        // Underbelly's small outflows (about 2 yd, 0.65), capped so a tall one (Ulduar's is 67 yd) can't flood its
        // basin.
        radius = 0.5f * std::min(e.thickness, 3.0f) + 0.75f + std::clamp(0.08f * e.height, 0.0f, 0.75f);
        strength = std::clamp(0.45f + e.height / 40.0f, 0.45f, 0.7f);
        break;
    }
    const float mid[2] = {0.5f * (e.a[0] + e.b[0]), 0.5f * (e.a[1] + e.b[1])};
    segment[0] = mid[0] + (e.a[0] - mid[0]) * keep;
    segment[1] = mid[1] + (e.a[1] - mid[1]) * keep;
    segment[2] = mid[0] + (e.b[0] - mid[0]) * keep;
    segment[3] = mid[1] + (e.b[1] - mid[1]) * keep;
    shape[0] = std::max(radius, 0.75f);
    shape[1] = strength;
    shape[2] = 0.5f * (e.a[2] + e.b[2]);
    shape[3] = e.indoor ? 4.0f : std::clamp(0.1f * e.height, 4.0f, 12.0f);
}

uint32_t CollectWaterfalls(int map, const float centre[2], float referenceHeight, float range,
                           const WaterfallEmitter** out, uint32_t capacity)
{
    return CollectNearest(map, centre, referenceHeight, range, out, capacity, false);
}

uint32_t CollectLips(int map, const float centre[2], float referenceHeight, float range, const WaterfallEmitter** out,
                     uint32_t capacity)
{
    return CollectNearest(map, centre, referenceHeight, range, out, capacity, true);
}
