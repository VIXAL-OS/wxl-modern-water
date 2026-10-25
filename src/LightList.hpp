// Optional WXL service "chromie.light-list" v1: the point lights Seyris Living Azeroth draws this frame (lamps,
// torches, lit windows, spell lights, lightning), for other extensions to light or reflect by. Read only, on the
// main (render) thread. The same header goes into the provider; extensions find it through WXL_Api::GetInterface.
#pragma once
#include <cstdint>

struct WxlLightV1
{
    float position[3]; // world, yd
    float colour[3];   // as the provider's own shaders add it (this frame's: brightness, fade and flicker applied;
                       // a torch at a yard is about 0.6), falling off as 1 / (e (0.7 + 0.03 e)), e = sqrt(d^2 + 1)
    float radius;      // yd: the light is exactly 0 here
    float falloff;     // its window over the radius, ((1 - (d / radius)^2)^2)^falloff: 1 = smooth, higher = tighter
    uint32_t flags;    // kWxlLight* below
};

constexpr uint32_t kWxlLightIndoor = 1;  // inside an indoor WMO group (a cellar's, a house's): no light outdoors
constexpr uint32_t kWxlLightWindow = 2;  // a lit window's
constexpr uint32_t kWxlLightMoving = 4;  // follows a model: a spell's, a projectile's, a weapon enchant's
constexpr uint32_t kWxlLightFlash = 8;   // lightning, for a frame or two

struct WxlLightListV1
{
    uint32_t size;      // sizeof(WxlLightListV1)
    uint32_t lightSize; // the array's step in bytes: sizeof(WxlLightV1) or more (later versions append fields)
    // This frame's lights: their count, and *lights set to the first (null with none). The array stays as it is
    // until the provider's next per-frame update: read it on the main thread and copy what you keep.
    uint32_t(__cdecl* Lights)(const WxlLightV1** lights);
};

constexpr const char* kWxlLightListName = "chromie.light-list";
constexpr uint32_t kWxlLightListVersion = 1;
