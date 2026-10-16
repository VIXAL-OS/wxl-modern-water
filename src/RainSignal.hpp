// Optional WXL service. Read on the main/render thread; returns a cached snapshot.
#pragma once
#include <cstdint>
struct WxlRainSignalV1
{
    uint32_t size;
    float (__cdecl* Intensity)(); // 0..1; zero indoors, outside the world, or in non-rain weather.
};
constexpr const char* kWxlRainSignalName = "chromie.rain-signal";
constexpr uint32_t kWxlRainSignalVersion = 1;
