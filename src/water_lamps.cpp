#include "water_lamps.h"

#include "engine_lights.h"
#include "WaterLights.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>

namespace
{
// The client's lights reach about 80 yd by their fog cutoff (1/256 of their colour); on the water their light ends at
// kClientLampReach yd, near Living Azeroth's 16, or one lamp lit a whole canal.
constexpr float kClientLampReach = 20.0f;
// A lamp's reflection reaches kMirrorPerReach times as far as its light, at most kMaxMirrorReach yd.
constexpr float kMirrorPerReach = 4.0f;
constexpr float kMaxMirrorReach = 60.0f;
// Living Azeroth's lights: those within kMaxLampDistance yd of the camera, ranked by their brightness over
// 1 + (d / kLampRanking)^2; those behind the camera (their light still falls on the water in view, their reflection
// doesn't show) count kBehindShare as much.
constexpr float kMaxLampDistance = 120.0f;
constexpr float kLampRanking = 30.0f;
constexpr float kBehindShare = 0.3f;
constexpr float kBehindCosine = -0.2f;
constexpr float kMinLampShape = 0.25f;
constexpr float kLivingAzerothFalloff[3] = {0.0f, 0.7f, 0.03f}; // LightList.hpp
// A lightning strike lights the water half as far again as its light's radius (Living Azeroth's strikes: 60 yd -> 90).
constexpr float kFlashReachPerRadius = 1.5f;

WaterLampsStatus g_last;

bool Finite(const float* values, int count)
{
    for (int i = 0; i < count; ++i)
        if (!std::isfinite(values[i]))
            return false;
    return true;
}

float Peak(const float colour[3])
{
    return std::max(colour[0], std::max(colour[1], colour[2]));
}

void FromClient(const LocalLightInputs& client, WaterLamps& out)
{
    const uint32_t count = std::min(client.pointLightCount, kMaxLocalPointLights);
    out.offered = count;
    for (uint32_t i = 0; i < count && out.count < kMaxWaterLamps; ++i)
    {
        const LocalPointLight& light = client.pointLights[i];
        const float reach = std::min(light.cutoff, kClientLampReach);
        if (!(reach > 0.0f))
            continue;
        WaterLamp& lamp = out.lamp[out.count++];
        std::memcpy(lamp.position, light.position, sizeof(lamp.position));
        lamp.reach = reach;
        std::memcpy(lamp.colour, light.uploadedColor, sizeof(lamp.colour));
        lamp.shape = 0.0f;
        std::memcpy(lamp.falloff, light.attenuation, sizeof(lamp.falloff));
        lamp.mirrorReach = std::min(std::max(light.cutoff, reach), kMaxMirrorReach);
    }
    out.source = out.count ? WaterLampSource::Client : WaterLampSource::None;
}

void FromLivingAzeroth(const WxlLightV1* lights, uint32_t count, uint32_t stride, const FrameInputs& in,
                       WaterLamps& out)
{
    float forward[3] = {in.camTarget[0] - in.camPos[0], in.camTarget[1] - in.camPos[1], in.camTarget[2] - in.camPos[2]};
    const float length = std::sqrt(forward[0] * forward[0] + forward[1] * forward[1] + forward[2] * forward[2]);
    for (float& axis : forward)
        axis = length > 1e-4f ? axis / length : 0.0f;
    struct Ranked
    {
        float score;
        const WxlLightV1* light;
    };
    Ranked best[kMaxWaterLamps] = {};
    uint32_t kept = 0;
    Ranked strikes[kMaxWaterFlashes] = {};
    uint32_t struck = 0;
    // Keeps the `capacity` highest scores in `ranked`, highest first.
    auto rank = [](Ranked* ranked, uint32_t& held, uint32_t capacity, float score, const WxlLightV1* light) {
        uint32_t at = held;
        while (at > 0 && ranked[at - 1].score < score)
            --at;
        if (at >= capacity)
            return;
        for (uint32_t k = std::min(held, capacity - 1); k > at; --k)
            ranked[k] = ranked[k - 1];
        ranked[at] = {score, light};
        held = std::min(held + 1, capacity);
    };
    const auto* bytes = reinterpret_cast<const unsigned char*>(lights);
    for (uint32_t i = 0; i < count; ++i)
    {
        const auto* light = reinterpret_cast<const WxlLightV1*>(bytes + static_cast<size_t>(i) * stride);
        if (!Finite(light->position, 3) || !Finite(light->colour, 3) || !std::isfinite(light->radius) ||
            !(light->radius > 0.0f) || !(Peak(light->colour) > 0.0f))
            continue;
        // A cellar's or a house's light: none on water outdoors.
        if ((light->flags & kWxlLightIndoor) && !in.localLights.cameraInterior)
            continue;
        const float to[3] = {light->position[0] - in.camPos[0], light->position[1] - in.camPos[1],
                             light->position[2] - in.camPos[2]};
        const float d = std::sqrt(to[0] * to[0] + to[1] * to[1] + to[2] * to[2]);
        // A lightning strike: whatever water its flash reaches near the camera, the brightest (by its reach) first.
        if (light->flags & kWxlLightFlash)
        {
            const float reach = light->radius * kFlashReachPerRadius;
            if (d <= reach + kMaxLampDistance)
                rank(strikes, struck, kMaxWaterFlashes, Peak(light->colour) * reach / (reach + d), light);
            continue;
        }
        if (!(d <= kMaxLampDistance))
            continue;
        const float facing = d > 1e-3f ? (to[0] * forward[0] + to[1] * forward[1] + to[2] * forward[2]) / d : 1.0f;
        const float ranked = d / kLampRanking;
        rank(best, kept, kMaxWaterLamps,
             Peak(light->colour) * (facing >= kBehindCosine ? 1.0f : kBehindShare) / (1.0f + ranked * ranked), light);
    }
    const PointLightUpload upload{true, 1.0f};
    for (uint32_t k = 0; k < kept; ++k)
    {
        const WxlLightV1& light = *best[k].light;
        WaterLamp& lamp = out.lamp[out.count++];
        std::memcpy(lamp.position, light.position, sizeof(lamp.position));
        lamp.reach = light.radius;
        engine::UploadedPointLightColor(light.colour, upload, lamp.colour);
        lamp.shape = std::max(std::isfinite(light.falloff) ? light.falloff : 1.0f, kMinLampShape);
        std::memcpy(lamp.falloff, kLivingAzerothFalloff, sizeof(lamp.falloff));
        lamp.mirrorReach = std::min(light.radius * kMirrorPerReach, std::max(kMaxMirrorReach, light.radius));
    }
    for (uint32_t k = 0; k < struck; ++k)
    {
        const WxlLightV1& light = *strikes[k].light;
        WaterFlash& flash = out.flash[out.flashes++];
        std::memcpy(flash.position, light.position, sizeof(flash.position));
        flash.reach = light.radius * kFlashReachPerRadius;
        engine::UploadedPointLightColor(light.colour, upload, flash.colour);
    }
    out.offered = count;
    out.source = WaterLampSource::LivingAzeroth;
}
}

void ChooseWaterLamps(const FrameInputs& in, WaterLamps& out)
{
    out = WaterLamps{};
    const WxlLightV1* lights = nullptr;
    uint32_t stride = 0;
    const uint32_t listed = water_lights::Lights(&lights, &stride);
    if (listed)
        FromLivingAzeroth(lights, listed, stride, in, out);
    else
        FromClient(in.localLights, out);
    g_last = {out.count, out.offered, out.source, out.flashes};
}

const char* WaterLampSourceName(WaterLampSource source)
{
    switch (source)
    {
    case WaterLampSource::Client:
        return "the client";
    case WaterLampSource::LivingAzeroth:
        return "Living Azeroth";
    case WaterLampSource::None:
        break;
    }
    return "none";
}

WaterLampsStatus LastWaterLampsStatus()
{
    return g_last;
}
