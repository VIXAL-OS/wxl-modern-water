#pragma once

#include "water_types.h"

#include <cstdint>

constexpr uint32_t kLiquidMaterialWater = 1;
// Ocean sound-bank water is the ocean class (see ClassifyLiquid).
constexpr uint32_t kLiquidSoundBankOcean = 1;
constexpr uint32_t kLiquidTypeFastWater = 9;
// Liquid type 17 is WMO interior water. It is intentionally never surfaced as its
// own class; the classifier folds it into the lake/ocean result (see ClassifyLiquid).
constexpr uint32_t kLiquidTypeWmoWaterInterior = 17;
constexpr const char* kRiverTextureFolder = "\\river\\";
constexpr const char* kOceanTextureFolder = "\\ocean\\";

inline char AsciiLower(char c)
{
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

inline bool StartsWithIgnoringCase(const char* text, const char* prefix)
{
    for (; *prefix; ++text, ++prefix)
        if (AsciiLower(*text) != AsciiLower(*prefix))
            return false;
    return true;
}

inline bool ContainsIgnoringCase(const char* text, const char* part)
{
    if (!text)
        return false;
    for (; *text; ++text)
        if (StartsWithIgnoringCase(text, part))
            return true;
    return false;
}

inline bool HasWaterSurfaceTexture(const char* texture0)
{
    return ContainsIgnoringCase(texture0, kRiverTextureFolder) || ContainsIgnoringCase(texture0, kOceanTextureFolder);
}

// WMO interior water (liquid type 17) has no surface identity of its own: it is folded into the lake class. Fast water
// (type 9) is the river class. Ocean sound-bank water is the ocean class: the same preset as lake water
// (ForeverLiquidOf), shaded in a pass of its own, which alone gets the surf at the coast (cCoast; the user,
// 2026-10-11: "waves at coastlines specifically").
inline WaterClass ClassifyLiquid(uint32_t liquidTypeId, uint32_t soundBank, uint32_t materialId, const char* texture0)
{
    if (materialId != kLiquidMaterialWater || !HasWaterSurfaceTexture(texture0))
        return WaterClass::None;
    if (liquidTypeId == kLiquidTypeFastWater)
        return WaterClass::River;
    if (soundBank == kLiquidSoundBankOcean && liquidTypeId != kLiquidTypeWmoWaterInterior)
        return WaterClass::Ocean;
    return WaterClass::Lake;
}

inline const char* WaterClassLabel(WaterClass waterClass)
{
    switch (waterClass)
    {
    case WaterClass::Lake:
        return "lake";
    case WaterClass::River:
        return "river";
    case WaterClass::Ocean:
        return "ocean";
    case WaterClass::Interior:
        return "interior";
    default:
        return "none";
    }
}
