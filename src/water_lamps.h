#pragma once

#include "engine.h"

#include <cstdint>

// The lamps that light the water and show in it (vw_optics.hlsli LampsOnWater), chosen once a frame: Seyris Living
// Azeroth's light list when it publishes one with lights in it (WaterLights.hpp: lamps, torches, lit windows, spell
// lights, all of them near the player), else the client's own point lights (engine_lights.h: the eight that light the
// camera's surroundings most, only those of models in view). Never both: Living Azeroth's list holds the client's
// lights already, merged its way.
constexpr uint32_t kMaxWaterLamps = 8;   // vw_constants.hlsli kMaxLamps
// Lightning strikes (the list's flash lights, kWxlLightFlash) are no lamps: they flash over the water (vw_optics.hlsli
// FlashesOnWater) and don't take a lamp's place.
constexpr uint32_t kMaxWaterFlashes = 2; // vw_constants.hlsli kMaxFlashes

enum class WaterLampSource : uint32_t
{
    None,
    Client,
    LivingAzeroth,
};

struct WaterLamp
{
    float position[3];
    float reach;        // yd: its light on the water ends here
    float colour[3];    // linear
    float shape;        // 0: the client's window (linear over the last quarter); above 0: Living Azeroth's
    float falloff[3];   // 1 / (constant + linear d + quadratic d^2)
    float mirrorReach;  // yd: its reflection ends here
};

struct WaterFlash
{
    float position[3];
    float reach;        // yd: the flash ends here (the light's radius x water_lamps.cpp kFlashReachPerRadius)
    float colour[3];    // linear, this frame's (the strike fades on its own)
};

struct WaterLamps
{
    WaterLamp lamp[kMaxWaterLamps] = {};
    uint32_t count = 0;
    WaterFlash flash[kMaxWaterFlashes] = {};
    uint32_t flashes = 0;
    uint32_t offered = 0; // the lights its source had this frame
    WaterLampSource source = WaterLampSource::None;
};

// From the frame's inputs (the client's lights in in.localLights, when they were captured) and Living Azeroth's list.
void ChooseWaterLamps(const FrameInputs& in, WaterLamps& out);
const char* WaterLampSourceName(WaterLampSource source);

// What the last water pass chose (for the settings window and the tests).
struct WaterLampsStatus
{
    uint32_t count = 0;
    uint32_t offered = 0;
    WaterLampSource source = WaterLampSource::None;
    uint32_t flashes = 0;
};
WaterLampsStatus LastWaterLampsStatus();
