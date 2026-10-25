#include "water_renderer.h"

#include "fog_model.h"
#include "fullscreen_triangle.h"
#include "log.h"
#include "player_mask.h"
#include "water_classify.h"
#include "water_data.h"
#include "WaterRain.hpp"

#include "ps_vw_depth.h"
#include "ps_vw_depth_packed.h"
#include "ps_vw_depth_push.h"
#include "ps_vw_shade_high.h"
#include "ps_vw_shade_high_mask.h"
#include "ps_vw_shade_low.h"
#include "ps_vw_shade_mid.h"
#include "ps_vw_shade_mid_mask.h"
#include "vs_fullscreen.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace
{
constexpr UINT kWaterPixelConstants = 161;
constexpr DWORD kWaterSamplerStages = 16;
constexpr UINT kCommonConstants = 9;
constexpr UINT kShadingFirstConstant = 9;
constexpr DWORD kMaxRenderTargets = 4;
constexpr DWORD kSceneColourStage = 0;
constexpr DWORD kSceneDepthStage = 1;
constexpr DWORD kWaterDepthStage = 2;
constexpr DWORD kFirstSurfaceStage = 3;
constexpr DWORD kFirstFoamStateStage = 7;
constexpr DWORD kWaveFoamMaskStage = 11;
constexpr DWORD kRippleStage = 12;
constexpr DWORD kRiverFlowStage = 13;
constexpr DWORD kShoreFoamMaskStage = 14;
constexpr DWORD kDepthFoamMaskStage = 15;
constexpr int kShoreFoamSlot = static_cast<int>(WaterMaskSlot::ShoreFoam);
constexpr int kDepthFoamSlot = static_cast<int>(WaterMaskSlot::DepthFoam);
constexpr double kRippleGapSeconds = 1.0;
constexpr float kRippleEdgeRampPerExtent = 1.0f / 16.0f;
constexpr float kRippleFadeYards = 4.0f;
constexpr float kRippleBobberMarginYards = 1.0f;
constexpr float kWaterfallFoamRange = 120.0f;
// How strongly a lip smooths and streams the water before the edge (vw_surface.hlsli SampleLips), 0 = off.
constexpr float kLipStrength = 1.0f;
// How strongly the ocean's swell rolls in and breaks at the shore (vw_surface.hlsli CoastSurf), 0 = off.
constexpr float kCoastSurfStrength = 1.0f;
// How far from the middle of the player (and mount) a reflection hit counts as the player (vw_reflection.hlsli: not
// shown in water more than 5 yd below their feet). A mount's wings and tail reach well past 3.5 yd: a flyer on a drake
// still saw it in the sea far below ("i do still see incorrect far reflections tho", the user, 2026-10-11).
constexpr float kPlayerReflectionReach = 3.5f;
constexpr float kMountedReflectionReach = 9.0f;
// On foot the body's reach over the bed (vw_surface.hlsli kBodyRadius).
constexpr float kBodyReach = 3.0f;
// How strongly the lamps (water_lamps.h) shine on the water (vw_optics.hlsli LampsOnWater), x WaterLamps; 0 = off.
constexpr float kLampStrength = 1.0f;
// The smallest roughness of a lamp's reflection: the ripples too small for the wave maps stretch it into a streak. Full
// from WaterWaves kLampRoughWaves up, none on flat water (a lamp shows as a small sharp spot there).
constexpr float kLampRoughness = 0.45f;
constexpr float kLampRoughWaves = 0.5f;
// Sun glitter (vw_optics.hlsli SunGlitter): its finest cell (yd); how far its facets tilt, kGlitterTilt +
// kGlitterTiltPerWind x WaterWind (0.17 at the default wind 3), half that on calm water; full from WaterWaves
// kGlitterFullWaves up, none on flat water; kGlitterRainDim less in full rain.
constexpr float kGlitterCell = 0.18f;
constexpr float kGlitterTilt = 0.065f;
constexpr float kGlitterTiltPerWind = 0.035f;
constexpr float kGlitterFullWaves = 0.5f;
constexpr float kGlitterRainDim = 0.85f;
constexpr float kRippleBiasYardsPerSecond = 40.0f;
constexpr double kRippleBiasMaxStepSeconds = 0.25;
// Another player's far bobber doesn't pull the window while the player is touching the water (a unit in the
// water this close to the camera target): the player's own ripples come first.
constexpr float kRippleSelfContactYards = 2.5f;
// Once shaded, the water's depth moves this far straight down (vw_depth_push.hlsl), but never past the scene behind
// it, so models made to lie on the water or just under it draw over it as over the client's own water: a fountain's
// foam sheet 4 cm down, splash decals, and the foam and ripple sheets laid on rivers at falls. Those sit up to 0.37 yd
// under (whole sheets, in a scan of every placed fall model against its map's liquid); the Sholazar fall's foam on the
// Frenzyheart River 0.08 to 0.16 yd, which a tenth of a yard cut into flickering slabs (the user, 2026-10-16 and
// 2026-10-24). At a grazing view the move along the view is capped (ten times the push, as before).
constexpr float kWaterDepthPushYards = 0.5f;
constexpr float kWaterDepthPushMaxViewYards = 5.0f;
// At the lips of the falls nearby (vw_depth_push.hlsl cLip): the river's surface overhangs the edge, so there the water
// moves down by the fall's height (at most kLipPushMaxYards) within kLipReachYards of the box's top edge beyond half its
// thickness, for falls at least kLipPushMinHeight tall.
constexpr uint32_t kMaxPushLips = 8;
constexpr float kLipReachYards = 3.0f;
constexpr float kLipPushMinHeight = 3.0f;
constexpr float kLipPushMaxYards = 40.0f;
constexpr DWORD kStencilAllBits = 0xFF;
constexpr DWORD kColourWriteRgb = D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE;
constexpr DWORD kColourWriteAll = kColourWriteRgb | D3DCOLORWRITEENABLE_ALPHA;
constexpr float kWaterMaxViewDepth = 65536.0f;
constexpr float kMinViewportDepthExtent = 0.01f;
constexpr float kDeepestWorldDepthInFullRangeViewport = 0.9999995f;
constexpr float kWorldDepthMargin = 2.0e-6f;
constexpr UINT kMinWorldViewportSide = 16;
constexpr float kPackedDepthLevels = 16777215.0f;
constexpr float kByteMax = 255.0f;
constexpr float kHighByteWeight = 65536.0f;
constexpr float kMidByteWeight = 256.0f;
constexpr float kDisplayGamma = 2.2f;
constexpr float kSunVisibilityHalfWidth = 0.1f;
constexpr float kWaterF0 = 0.02f;
constexpr int kSchlickExponent = 5;
constexpr float kMinClarity = 0.01f;
constexpr float kDefaultWaterClarity = 0.7f;
constexpr float kShoreDistancePerDepth = 4.0f;
constexpr float kMinStockFogRange = 1e-3f;
constexpr double kMaxFoamStepSeconds = 0.1;
constexpr double kWaveUpdateSeconds = 1.0 / 30.0;
constexpr double kSummarySeconds = 60.0;
constexpr size_t kSummaryTextSize = 96;
constexpr int kLowQuality = 1;
constexpr int kHighQuality = 3;
constexpr int kSkyReflectionsOnlyVariant = 0;
constexpr int kMaskUploadRetryPasses = 60;
constexpr float kReflectionFogActive = 1.0f;
constexpr int kFftResolutionLow = 128;
constexpr int kFftResolution = 256;
constexpr int kFftReferenceResolution = 256;
constexpr float kWindDirection[2] = {0.8f, 0.6f};
constexpr int kSkyTop = 0;
constexpr int kSkyMiddle = 1;
constexpr int kSkyUpperBand = 2;
constexpr int kSkyLowerBand = 3;
constexpr int kSkyHorizon = 4;
constexpr int kSkyColourOfBand[kWaterSkyBands] = {kSkyTop, kSkyMiddle, kSkyUpperBand, kSkyLowerBand, kSkyHorizon};
constexpr int kCloseWaterColour = 0;
constexpr int kFarWaterColour = 1;

constexpr DWORD kUntaggedStencil = 0;

struct StencilSetting
{
    D3DRENDERSTATETYPE state;
    DWORD value;
};

const StencilSetting kPassStencil[kWaterStencilStates] = {
    {D3DRS_STENCILENABLE, TRUE},
    {D3DRS_STENCILFUNC, D3DCMP_ALWAYS},
    {D3DRS_STENCILPASS, D3DSTENCILOP_REPLACE},
    {D3DRS_STENCILFAIL, D3DSTENCILOP_KEEP},
    {D3DRS_STENCILZFAIL, D3DSTENCILOP_KEEP},
    {D3DRS_STENCILREF, kUntaggedStencil},
    {D3DRS_STENCILMASK, kStencilAllBits},
    {D3DRS_STENCILWRITEMASK, kStencilAllBits},
    {D3DRS_TWOSIDEDSTENCILMODE, FALSE},
};

const D3DRENDERSTATETYPE kPassRenderStates[] = {
    D3DRS_ZENABLE,           D3DRS_ZWRITEENABLE,     D3DRS_ZFUNC,
    D3DRS_ALPHATESTENABLE,   D3DRS_ALPHABLENDENABLE, D3DRS_SRCBLEND,
    D3DRS_DESTBLEND,         D3DRS_BLENDOP,          D3DRS_SEPARATEALPHABLENDENABLE,
    D3DRS_CULLMODE,          D3DRS_STENCILENABLE,    D3DRS_STENCILFUNC,
    D3DRS_STENCILPASS,       D3DRS_STENCILFAIL,      D3DRS_STENCILZFAIL,
    D3DRS_STENCILREF,        D3DRS_STENCILMASK,      D3DRS_STENCILWRITEMASK,
    D3DRS_TWOSIDEDSTENCILMODE, D3DRS_CCW_STENCILFUNC, D3DRS_CCW_STENCILPASS,
    D3DRS_CCW_STENCILFAIL,   D3DRS_CCW_STENCILZFAIL, D3DRS_SCISSORTESTENABLE,
    D3DRS_COLORWRITEENABLE,  D3DRS_SRGBWRITEENABLE,  D3DRS_FOGENABLE,
    D3DRS_CLIPPLANEENABLE,   D3DRS_FILLMODE,
};

const D3DSAMPLERSTATETYPE kPassSamplerStates[] = {
    D3DSAMP_ADDRESSU,   D3DSAMP_ADDRESSV,      D3DSAMP_ADDRESSW,    D3DSAMP_BORDERCOLOR,
    D3DSAMP_MAGFILTER,  D3DSAMP_MINFILTER,     D3DSAMP_MIPFILTER,   D3DSAMP_MIPMAPLODBIAS,
    D3DSAMP_MAXMIPLEVEL, D3DSAMP_MAXANISOTROPY, D3DSAMP_SRGBTEXTURE,
};

constexpr DWORD kInjectedWaterFault = 0xE0564657;

double g_secondsOverride = -1.0;
bool g_waveSimulationDisabled = false;
bool g_packedDepthForced = false;
WaterFaultStage g_injectedFault = WaterFaultStage::None;
int g_failingMaskUploads = 0;
int g_forcedShadingVariant = -1;
bool g_summaryForced = false;

void RaiseInjectedFault(WaterFaultStage stage)
{
    if (g_injectedFault != stage)
        return;
    g_injectedFault = WaterFaultStage::None;
    RaiseException(kInjectedWaterFault, 0, 0, nullptr);
}

template <typename T>
void SafeRelease(T*& p)
{
    if (p)
    {
        p->Release();
        p = nullptr;
    }
}

double QpcSeconds()
{
    static const double frequency = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return static_cast<double>(f.QuadPart);
    }();
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return static_cast<double>(now.QuadPart) / frequency;
}

double WaterSeconds()
{
    return g_secondsOverride >= 0.0 ? g_secondsOverride : QpcSeconds();
}

bool CreateTarget(IDirect3DDevice9* dev, UINT w, UINT h, D3DFORMAT format, IDirect3DTexture9** out)
{
    return SUCCEEDED(dev->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, format, D3DPOOL_DEFAULT, out, nullptr));
}

void SetTarget(IDirect3DDevice9* dev, IDirect3DTexture9* texture)
{
    IDirect3DSurface9* surface = nullptr;
    if (SUCCEEDED(texture->GetSurfaceLevel(0, &surface)))
    {
        dev->SetRenderTarget(0, surface);
        surface->Release();
    }
}

void BindSampler(IDirect3DDevice9* dev, DWORD stage, IDirect3DBaseTexture9* texture, D3DTEXTUREADDRESS address,
                 D3DTEXTUREFILTERTYPE filter, D3DTEXTUREFILTERTYPE mipFilter)
{
    dev->SetTexture(stage, texture);
    dev->SetSamplerState(stage, D3DSAMP_ADDRESSU, address);
    dev->SetSamplerState(stage, D3DSAMP_ADDRESSV, address);
    dev->SetSamplerState(stage, D3DSAMP_ADDRESSW, address);
    dev->SetSamplerState(stage, D3DSAMP_MAGFILTER, filter);
    dev->SetSamplerState(stage, D3DSAMP_MINFILTER, filter);
    dev->SetSamplerState(stage, D3DSAMP_MIPFILTER, mipFilter);
    dev->SetSamplerState(stage, D3DSAMP_MIPMAPLODBIAS, 0);
    dev->SetSamplerState(stage, D3DSAMP_MAXMIPLEVEL, 0);
    dev->SetSamplerState(stage, D3DSAMP_SRGBTEXTURE, FALSE);
}

void BindPointSampler(IDirect3DDevice9* dev, DWORD stage, IDirect3DBaseTexture9* texture, D3DTEXTUREADDRESS address)
{
    BindSampler(dev, stage, texture, address, D3DTEXF_POINT, D3DTEXF_NONE);
}

RECT ViewportRect(const D3DVIEWPORT9& vp)
{
    return {static_cast<LONG>(vp.X), static_cast<LONG>(vp.Y), static_cast<LONG>(vp.X + vp.Width),
            static_cast<LONG>(vp.Y + vp.Height)};
}

struct WorldDepthMapping
{
    float atInfinity;
    float perInverseViewDepth;
    float deepest;
};

WorldDepthMapping MapWorldDepth(const float* proj, const D3DVIEWPORT9& vp)
{
    const bool usableRange = vp.MaxZ - vp.MinZ > kMinViewportDepthExtent && vp.MinZ >= 0.0f && vp.MaxZ <= 1.0f;
    const float worldMinZ = usableRange ? vp.MinZ : 0.0f;
    const float worldExtent = usableRange ? vp.MaxZ - vp.MinZ : 1.0f;
    const float worldMaxZ = worldMinZ + worldExtent;
    return {worldMinZ + worldExtent * (1.0f + proj[10]) * 0.5f, worldExtent * proj[14] * 0.5f,
            worldMaxZ >= kDeepestWorldDepthInFullRangeViewport ? kDeepestWorldDepthInFullRangeViewport
                                                               : worldMaxZ + kWorldDepthMargin};
}

bool BuildCommonConstants(const FrameInputs& in, const D3DSURFACE_DESC& depthDesc, float (&common)[9][4])
{
    float viewToWorld[16];
    if (!Invert4x4(in.cameraRelativeView, viewToWorld))
        return false;
    viewToWorld[12] = in.camPos[0];
    viewToWorld[13] = in.camPos[1];
    viewToWorld[14] = in.camPos[2];
    const D3DVIEWPORT9& vp = in.viewport;
    const float* proj = in.glProjection;
    const WorldDepthMapping depth = MapWorldDepth(proj, vp);
    const float width = static_cast<float>(vp.Width);
    const float height = static_cast<float>(vp.Height);
    const float rows[9][4] = {
        {static_cast<float>(vp.X), static_cast<float>(vp.Y), width, height},
        {1.0f, 0.0f, 1.0f / depthDesc.Width, 1.0f / depthDesc.Height},
        {proj[0], proj[5], proj[8], proj[9]},
        {depth.atInfinity, depth.perInverseViewDepth, kWaterMaxViewDepth, depth.deepest},
        {},
        {},
        {},
        {},
        {width, height, 1.0f / width, 1.0f / height},
    };
    std::memcpy(common, rows, sizeof(rows));
    std::memcpy(common[4], viewToWorld, sizeof(viewToWorld));
    return true;
}

float Saturate(float x)
{
    return std::clamp(x, 0.0f, 1.0f);
}

float SmoothStep(float edge0, float edge1, float x)
{
    const float t = Saturate((x - edge0) / (edge1 - edge0));
    return t * t * (3.0f - 2.0f * t);
}

float FresnelSchlick(float cosine)
{
    return kWaterF0 + (1.0f - kWaterF0) * std::pow(1.0f - Saturate(cosine), static_cast<float>(kSchlickExponent));
}

void LinearColour(uint32_t argb, float* rgb)
{
    UnpackColor(argb, rgb);
    for (int c = 0; c < 3; ++c)
        rgb[c] = std::pow(rgb[c], kDisplayGamma);
}

float LinearLuma(const float* rgb)
{
    return 0.2126f * rgb[0] + 0.7152f * rgb[1] + 0.0722f * rgb[2];
}

void ClampedTint(const float* linearTint, float* rgb)
{
    for (int c = 0; c < 3; ++c)
        rgb[c] = Saturate(linearTint[c]);
}

float ScrollOffset(double seconds, int axis, float multiplier)
{
    const double travelled = seconds * kWindDirection[axis] * multiplier;
    return static_cast<float>(travelled - std::floor(travelled));
}

uint32_t MipSide(uint32_t size, uint32_t level)
{
    const uint32_t side = size >> level;
    return side ? side : 1;
}

bool FillMaskLevel(IDirect3DTexture9* texture, UINT level, const std::vector<uint8_t>& texels, uint32_t side,
                   bool luminance)
{
    D3DLOCKED_RECT locked = {};
    if (FAILED(texture->LockRect(level, &locked, nullptr, 0)))
        return false;
    for (uint32_t y = 0; y < side; ++y)
    {
        const uint8_t* source = texels.data() + static_cast<size_t>(y) * side;
        auto* row = static_cast<uint8_t*>(locked.pBits) + static_cast<size_t>(y) * locked.Pitch;
        if (luminance)
        {
            std::memcpy(row, source, side);
            continue;
        }
        auto* argb = reinterpret_cast<DWORD*>(row);
        for (uint32_t x = 0; x < side; ++x)
            argb[x] = 0xFF000000u | source[x] * 0x00010101u;
    }
    return SUCCEEDED(texture->UnlockRect(level));
}

IDirect3DTexture9* CreateMaskStaging(IDirect3DDevice9* dev, const WaterMaskLevels& mask, D3DFORMAT format)
{
    const UINT levels = static_cast<UINT>(mask.levels.size());
    const UINT size = mask.info.size;
    IDirect3DTexture9* staging = nullptr;
    if (FAILED(dev->CreateTexture(size, size, levels, 0, format, D3DPOOL_SYSTEMMEM, &staging, nullptr)))
        return nullptr;
    for (UINT level = 0; level < levels; ++level)
        if (!FillMaskLevel(staging, level, mask.levels[level], MipSide(size, level), format == D3DFMT_L8))
        {
            staging->Release();
            return nullptr;
        }
    return staging;
}

bool TakeInjectedMaskUploadFailure()
{
    if (g_failingMaskUploads <= 0)
        return false;
    --g_failingMaskUploads;
    return true;
}

bool FillPackedLevel(IDirect3DTexture9* texture, UINT level, const std::vector<uint32_t>& texels, uint32_t side)
{
    D3DLOCKED_RECT locked = {};
    if (FAILED(texture->LockRect(level, &locked, nullptr, 0)))
        return false;
    for (uint32_t y = 0; y < side; ++y)
        std::memcpy(static_cast<uint8_t*>(locked.pBits) + static_cast<size_t>(y) * locked.Pitch,
                    texels.data() + static_cast<size_t>(y) * side, side * sizeof(uint32_t));
    return SUCCEEDED(texture->UnlockRect(level));
}

IDirect3DTexture9* CreatePackedMaskTexture(IDirect3DDevice9* dev, const WaterPackedMask& packed)
{
    if (TakeInjectedMaskUploadFailure() || packed.levels.empty())
        return nullptr;
    const UINT levels = static_cast<UINT>(packed.levels.size());
    IDirect3DTexture9* staging = nullptr;
    IDirect3DTexture9* texture = nullptr;
    bool uploaded = SUCCEEDED(dev->CreateTexture(packed.size, packed.size, levels, 0, D3DFMT_A8R8G8B8,
                                                 D3DPOOL_SYSTEMMEM, &staging, nullptr));
    for (UINT level = 0; uploaded && level < levels; ++level)
        uploaded = FillPackedLevel(staging, level, packed.levels[level], MipSide(packed.size, level));
    uploaded = uploaded &&
               SUCCEEDED(dev->CreateTexture(packed.size, packed.size, levels, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
                                            &texture, nullptr)) &&
               SUCCEEDED(dev->UpdateTexture(staging, texture));
    if (staging)
        staging->Release();
    if (!uploaded && texture)
    {
        texture->Release();
        texture = nullptr;
    }
    return texture;
}

IDirect3DTexture9* CreateMaskTexture(IDirect3DDevice9* dev, const WaterMaskLevels& mask)
{
    if (TakeInjectedMaskUploadFailure())
        return nullptr;
    const UINT levels = static_cast<UINT>(mask.levels.size());
    const UINT size = mask.info.size;
    for (D3DFORMAT format : {D3DFMT_L8, D3DFMT_A8R8G8B8})
    {
        IDirect3DTexture9* texture = nullptr;
        if (FAILED(dev->CreateTexture(size, size, levels, 0, format, D3DPOOL_DEFAULT, &texture, nullptr)))
            continue;
        IDirect3DTexture9* staging = CreateMaskStaging(dev, mask, format);
        const bool uploaded = staging && SUCCEEDED(dev->UpdateTexture(staging, texture));
        if (staging)
            staging->Release();
        if (uploaded)
            return texture;
        texture->Release();
    }
    return nullptr;
}

int QualityIndex(const Config& cfg)
{
    return std::clamp(cfg.waterQuality, 1, kWaterQualityLevels) - 1;
}

int ShadingVariant(const Config& cfg)
{
    return cfg.waterReflections > 0.0f ? QualityIndex(cfg) : kSkyReflectionsOnlyVariant;
}
}

double WaterClockSeconds()
{
    return WaterSeconds();
}

void OverrideWaterSeconds(double seconds)
{
    g_secondsOverride = seconds;
}

void DisableWaveSimulation(bool disabled)
{
    g_waveSimulationDisabled = disabled;
}

void ForcePackedWaterDepth(bool forced)
{
    g_packedDepthForced = forced;
}

void InjectWaterFault(WaterFaultStage stage)
{
    g_injectedFault = stage;
}

void FailWaterMaskUploads(int count)
{
    g_failingMaskUploads = count;
}

void ForceWaterShadingVariant(int variant)
{
    g_forcedShadingVariant = variant;
}

void ForceWaterSummary()
{
    g_summaryForced = true;
}

// The zone colours are the client's river colours, and in linear light they are dark next to the preset: Stormwind's
// canal teal (41, 67, 58) is about 0.05 against the preset's sky blue at 0.5. Blended as they were, the preset's blue
// won at any weight short of 1, and at 1 the water went dark: the canals looked "blue raspberry blue" (the user,
// 2026-10-10). The zone now lends its hue at the preset's brightness, then the weight blends as before.
void ZoneTintedColour(const float* authored, const float* zone, float weight, float* out)
{
    const float zoneLuma = LinearLuma(zone);
    const float scale = zoneLuma > 1e-4f ? LinearLuma(authored) / zoneLuma : 0.0f;
    const float w = Saturate(weight);
    for (int c = 0; c < 3; ++c)
    {
        const float target = zoneLuma > 1e-4f ? zone[c] * scale : authored[c];
        out[c] = authored[c] + (target - authored[c]) * w;
    }
}

// The colour behind clear water is mostly the absorption's: the lake preset takes red fastest and blue slowest, so a
// few yards of it turn a grey canal floor sky blue whatever the scattering colour says, and the canals stayed "still
// blue" with the zone hue in the scattering (the user, 2026-10-10). Giving that light the zone's hue at the preset's
// brightness then made it bright green: the clear preset still showed the lamp-lit floor. The client's own water is
// nearly opaque there (Stormwind's LightParams: waterShallowAlpha 0.85, against 0.5 in most places), so the light
// through kZoneAbsorptionDepth yards now takes the zone's hue at 1 - that opacity: murky canals, and zones with the
// usual 0.5 close to the preset. The coefficients that give it are blended in by the weight.
void ZoneTintedAbsorption(const float* presetPerYard, const float* zone, float opacity, float clarityGain, float weight,
                          float* out)
{
    // Yards: a canal or a pond's shallows. At 3 the canals were "pretty fuckin good in the rising sun but could stand
    // to be a bit less opaque" (the user, 2026-10-11): over 4 a canal floor 3 yd down keeps 31% of its light, not 22%.
    constexpr float kZoneAbsorptionDepth = 4.0f;
    constexpr float kMinTransmittance = 1e-3f;
    constexpr float kMaxTransmittance = 0.999f;
    constexpr float kMinLetThrough = 0.05f;
    constexpr float kMaxLetThrough = 0.95f;
    const float zoneLuma = LinearLuma(zone);
    const float w = Saturate(weight);
    float preset[3];
    for (int c = 0; c < 3; ++c)
        preset[c] = std::exp(-std::max(presetPerYard[c], 0.0f) * kZoneAbsorptionDepth);
    const bool byOpacity = opacity >= 0.0f && opacity <= 1.0f;
    const float letThrough =
        byOpacity ? std::min(std::max(1.0f - opacity, kMinLetThrough), kMaxLetThrough) : LinearLuma(preset);
    const float gain = byOpacity && clarityGain > 0.0f ? clarityGain : 1.0f;
    // The light keeps kZoneHueShare of the zone's hue: all of it turned the pale stone of Stormwind's canal walls
    // chartreuse a yard down ("simon says the walls look too green", the user, 2026-10-11); the brightness is the same.
    constexpr float kZoneHueShare = 0.6f;
    for (int c = 0; c < 3; ++c)
    {
        float target = presetPerYard[c];
        if (zoneLuma > 1e-4f)
        {
            const float hue = 1.0f + (zone[c] / zoneLuma - 1.0f) * kZoneHueShare;
            const float transmitted = std::min(std::max(letThrough * hue, kMinTransmittance), kMaxTransmittance);
            target = -std::log(transmitted) / kZoneAbsorptionDepth * gain;
        }
        out[c] = presetPerYard[c] + (target - presetPerYard[c]) * w;
    }
}

WaterRenderer::~WaterRenderer()
{
    ReleaseAll();
}

void WaterRenderer::ReleaseDefaultPool()
{
    ReleaseTargets();
    m_gpuTimer.Release();
    ReleaseMasks();
    SafeRelease(m_sceneColour);
    SafeRelease(m_sceneDepth);
    SafeRelease(m_waterDepth);
    SafeRelease(m_state);
    m_copyW = m_copyH = 0;
    m_copyFailed = false;
    m_armed = false;
    m_stencilArmed = false;
    m_lastSeconds = -1.0;
    m_lastWaveSeconds = -1.0;
    m_lastFrameSeconds = -1.0;
    m_wavesValid = false;
    m_fft.ReleaseDefaultPool();
    m_ripples.ReleaseDefaultPool();
    m_contacts.Reset();
    m_lastRippleSeconds = -1.0;
    m_ripplesAvailable = true;
}

void WaterRenderer::ReleaseAll()
{
    ReleaseDefaultPool();
    m_unsupportedShaderDevice = nullptr;
    SafeRelease(m_vs);
    SafeRelease(m_decl);
    SafeRelease(m_depthCopy);
    SafeRelease(m_packedDepthCopy);
    SafeRelease(m_depthPush);
    for (auto*& shader : m_shade)
        SafeRelease(shader);
    for (auto*& shader : m_shadeMasked)
        SafeRelease(shader);
    SafeRelease(m_flat);
    m_riverFlow.Release();
    m_fft.ReleaseAll();
    m_ripples.ReleaseAll();
}

void WaterRenderer::ReleaseMasks()
{
    for (FoamMaskTexture& mask : m_foamMasks)
        SafeRelease(mask.texture);
    m_foamMasks.clear();
    m_masksPlanned = false;
    m_masksUploaded = false;
    m_maskRetryPasses = 0;
}

bool WaterRenderer::Skip(const char* reason)
{
    m_skip = reason;
    return false;
}

bool WaterRenderer::EnsureShaders(IDirect3DDevice9* dev)
{
    if (m_unsupportedShaderDevice == dev)
        return Skip("water shader unsupported");
    if (m_vs)
        return true;
    struct PixelShaderRequest
    {
        const char* name;
        const BYTE* code;
        IDirect3DPixelShader9** output;
    };
    const PixelShaderRequest pixels[] = {
        {"ps_vw_depth", g_ps_vw_depth, &m_depthCopy},
        {"ps_vw_depth_packed", g_ps_vw_depth_packed, &m_packedDepthCopy},
        {"ps_vw_depth_push", g_ps_vw_depth_push, &m_depthPush},
        {"ps_vw_shade_low", g_ps_vw_shade_low, &m_shade[0]},
        {"ps_vw_shade_mid", g_ps_vw_shade_mid, &m_shade[1]},
        {"ps_vw_shade_high", g_ps_vw_shade_high, &m_shade[2]},
        {"ps_vw_shade_mid_mask", g_ps_vw_shade_mid_mask, &m_shadeMasked[1]},
        {"ps_vw_shade_high_mask", g_ps_vw_shade_high_mask, &m_shadeMasked[2]},
    };
    static const D3DVERTEXELEMENT9 kElements[] = {
        {0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        D3DDECL_END(),
    };
    const char* failedName = "vs_fullscreen";
    HRESULT result = dev->CreateVertexShader(reinterpret_cast<const DWORD*>(g_vs_fullscreen), &m_vs);
    if (SUCCEEDED(result))
        for (const PixelShaderRequest& shader : pixels)
        {
            failedName = shader.name;
            result = dev->CreatePixelShader(reinterpret_cast<const DWORD*>(shader.code), shader.output);
            if (FAILED(result))
                break;
        }
    if (SUCCEEDED(result))
    {
        failedName = "water vertex declaration";
        result = dev->CreateVertexDeclaration(kElements, &m_decl);
    }
    if (SUCCEEDED(result))
        return true;
    const bool unsupported = (result == D3DERR_INVALIDCALL || result == D3DERR_NOTAVAILABLE ||
                              result == E_INVALIDARG) && dev->TestCooperativeLevel() == D3D_OK;
    VF_LOG_ERROR("water shader initialization failed: %s HRESULT 0x%08lX; %s", failedName,
                 static_cast<unsigned long>(result), unsupported ? "unsupported on this device" : "will retry");
    ReleaseAll();
    if (unsupported)
        m_unsupportedShaderDevice = dev;
    return Skip(unsupported ? "water shader unsupported" : "water shader creation failed");
}

bool WaterRenderer::EnsureStateBlock(IDirect3DDevice9* dev)
{
    if (m_state)
        return true;
    if (FAILED(dev->BeginStateBlock()))
        return Skip("water state block recording failed");
    for (D3DRENDERSTATETYPE state : kPassRenderStates)
        dev->SetRenderState(state, 0);
    for (DWORD stage = 0; stage < kWaterSamplerStages; ++stage)
    {
        dev->SetTexture(stage, nullptr);
        for (D3DSAMPLERSTATETYPE state : kPassSamplerStates)
            dev->SetSamplerState(stage, state, 0);
    }
    float zeros[kWaterPixelConstants * 4] = {};
    dev->SetVertexShader(nullptr);
    dev->SetPixelShader(nullptr);
    dev->SetPixelShaderConstantF(0, zeros, kWaterPixelConstants);
    dev->SetVertexDeclaration(m_decl);
    dev->SetStreamSource(0, nullptr, 0, 0);
    dev->SetStreamSourceFreq(0, 1);
    D3DVIEWPORT9 vp = {0, 0, 1, 1, 0.0f, 1.0f};
    dev->SetViewport(&vp);
    RECT scissor = {0, 0, 1, 1};
    dev->SetScissorRect(&scissor);
    if (FAILED(dev->EndStateBlock(&m_state)) || !m_state)
    {
        m_state = nullptr;
        return Skip("water state block recording failed");
    }
    return true;
}

bool WaterRenderer::EnsureCopies(IDirect3DDevice9* dev, IDirect3DSurface9* target, UINT w, UINT h)
{
    if (m_sceneColour && m_copyW == w && m_copyH == h && m_packedDepthForcedCopies == g_packedDepthForced)
        return true;
    SafeRelease(m_sceneColour);
    SafeRelease(m_sceneDepth);
    SafeRelease(m_waterDepth);
    m_copyW = m_copyH = 0;
    if (m_copyFailed)
        return Skip("water copies unavailable");
    D3DSURFACE_DESC desc = {};
    target->GetDesc(&desc);
    const bool colour = CreateTarget(dev, w, h, desc.Format, &m_sceneColour) ||
                        CreateTarget(dev, w, h, D3DFMT_A8R8G8B8, &m_sceneColour);
    // The scene's depth with the player mask beside it (g) where two-channel float targets can be drawn; plain R32F
    // (the reflections guess the player's shape) or packed RGBA8 where not.
    m_maskInDepth = !g_packedDepthForced && CreateTarget(dev, w, h, D3DFMT_G32R32F, &m_sceneDepth);
    m_packedDepth = g_packedDepthForced ||
                    (!m_maskInDepth && !CreateTarget(dev, w, h, D3DFMT_R32F, &m_sceneDepth)) ||
                    !CreateTarget(dev, w, h, D3DFMT_R32F, &m_waterDepth);
    bool depth = !m_packedDepth;
    if (m_packedDepth)
    {
        m_maskInDepth = false;
        SafeRelease(m_sceneDepth);
        SafeRelease(m_waterDepth);
        depth = CreateTarget(dev, w, h, D3DFMT_A8R8G8B8, &m_sceneDepth) &&
                CreateTarget(dev, w, h, D3DFMT_A8R8G8B8, &m_waterDepth);
    }
    if (!colour || !depth)
    {
        SafeRelease(m_sceneColour);
        SafeRelease(m_sceneDepth);
        SafeRelease(m_waterDepth);
        m_copyFailed = true;
        VF_LOG_ERROR("water scene copies could not be created (%ux%u); water keeps the client's shading", w, h);
        return Skip("water copy creation failed");
    }
    m_copyW = w;
    m_copyH = h;
    m_packedDepthForcedCopies = g_packedDepthForced;
    const bool newCopies = w != m_loggedCopyW || h != m_loggedCopyH || m_packedDepth != m_loggedPackedDepth ||
                           m_maskInDepth != m_loggedMaskInDepth;
    m_loggedMaskInDepth = m_maskInDepth;
    m_loggedCopyW = w;
    m_loggedCopyH = h;
    m_loggedPackedDepth = m_packedDepth;
    LogWrite(newCopies ? LogLevel::Info : LogLevel::Debug, "water copies: %ux%u, depth %s", w, h,
             m_packedDepth ? "packed rgba8" : m_maskInDepth ? "g32r32f (with the player mask)" : "r32f");
    return true;
}

bool WaterRenderer::EnsureFlatTexture(IDirect3DDevice9* dev)
{
    if (m_flat)
        return true;
    if (FAILED(dev->CreateTexture(1, 1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &m_flat, nullptr)))
    {
        m_flat = nullptr;
        return Skip("flat water texture creation failed");
    }
    D3DLOCKED_RECT locked = {};
    if (FAILED(m_flat->LockRect(0, &locked, nullptr, 0)))
    {
        SafeRelease(m_flat);
        return Skip("flat water texture creation failed");
    }
    *static_cast<DWORD*>(locked.pBits) = 0;
    m_flat->UnlockRect(0);
    return true;
}

void WaterRenderer::PlanFoamMasks(const WaterData& data)
{
    ReleaseMasks();
    auto plan = [this](const int32_t (&sources)[kWaveFoamMaskSlots], bool packed) {
        for (const FoamMaskTexture& mask : m_foamMasks)
            if (mask.packed == packed && std::equal(std::begin(sources), std::end(sources), mask.sources))
                return;
        FoamMaskTexture mask = {{sources[0], sources[1], sources[2]}, packed, {}, nullptr};
        m_foamMasks.push_back(mask);
    };
    for (const WaterPreset& preset : data.Presets())
    {
        const int32_t wave[kWaveFoamMaskSlots] = {preset.masks[static_cast<int>(WaterMaskSlot::HighFoam)],
                                                   preset.masks[static_cast<int>(WaterMaskSlot::MidFoam)],
                                                   preset.masks[static_cast<int>(WaterMaskSlot::LowFoam)]};
        if (std::any_of(std::begin(wave), std::end(wave), [](int32_t index) { return index >= 0; }))
            plan(wave, true);
        for (int slot : {kShoreFoamSlot, kDepthFoamSlot})
        {
            const int32_t single[kWaveFoamMaskSlots] = {preset.masks[slot], kWaterNoIndex, kWaterNoIndex};
            if (single[0] >= 0)
                plan(single, false);
        }
    }
    m_masksPlanned = true;
}

void WaterRenderer::EnsureMasks(IDirect3DDevice9* dev)
{
    const WaterData& data = GlobalWaterData();
    if (m_maskRevision != data.Revision() || !m_masksPlanned)
    {
        PlanFoamMasks(data);
        m_maskRevision = data.Revision();
    }
    if (m_masksUploaded)
        return;
    if (m_maskRetryPasses > 0)
    {
        --m_maskRetryPasses;
        return;
    }
    int uploaded = 0;
    for (FoamMaskTexture& mask : m_foamMasks)
    {
        if (!mask.texture && mask.packed)
        {
            const WaterPackedMask packed = PackWaveFoamMasks(data.Masks(), mask.sources);
            mask.texture = CreatePackedMaskTexture(dev, packed);
            std::copy(std::begin(packed.present), std::end(packed.present), mask.present);
        }
        else if (!mask.texture)
            mask.texture = CreateMaskTexture(dev, data.Masks()[mask.sources[0]]);
        uploaded += mask.texture ? 1 : 0;
    }
    m_masksUploaded = uploaded == RequiredMasks();
    m_maskRetryPasses = m_masksUploaded ? 0 : kMaskUploadRetryPasses;
    const bool newData = !m_masksLogged || m_loggedMaskRevision != data.Revision() || uploaded != m_loggedMaskUploads;
    m_loggedMaskRevision = data.Revision();
    m_masksLogged = true;
    m_loggedMaskUploads = uploaded;
    LogWrite(newData ? LogLevel::Info : LogLevel::Debug, "water foam masks uploaded: %d of %d%s", uploaded,
             RequiredMasks(), m_masksUploaded ? "" : "; retrying the others later");
}

unsigned WaterRenderer::HeldResources() const
{
    unsigned held = 0;
    if (m_sceneColour || m_sceneDepth || m_waterDepth)
        held |= kWaterSceneCopiesHeld;
    if (m_fft.HoldsDeviceResources())
        held |= kWaterWaveMapsHeld;
    if (UploadedMasks() > 0)
        held |= kWaterFoamMasksHeld;
    if (m_ripples.HoldsDeviceResources())
        held |= kWaterRippleMapsHeld;
    return held;
}

int WaterRenderer::FoamMaskPool() const
{
    for (const FoamMaskTexture& mask : m_foamMasks)
    {
        D3DSURFACE_DESC desc = {};
        if (mask.texture && SUCCEEDED(mask.texture->GetLevelDesc(0, &desc)))
            return static_cast<int>(desc.Pool);
    }
    return -1;
}

int WaterRenderer::UploadedMasks() const
{
    return static_cast<int>(std::count_if(m_foamMasks.begin(), m_foamMasks.end(), [](const FoamMaskTexture& mask) {
        return mask.texture != nullptr;
    }));
}

const WaterRenderer::FoamMaskTexture* WaterRenderer::SingleMask(int32_t index) const
{
    for (const FoamMaskTexture& mask : m_foamMasks)
        if (!mask.packed && mask.sources[0] == index && mask.texture)
            return &mask;
    return nullptr;
}

const WaterRenderer::FoamMaskTexture* WaterRenderer::WaveFoamMasks(const WaterPreset& preset) const
{
    for (const FoamMaskTexture& mask : m_foamMasks)
        if (mask.packed && mask.texture && std::equal(mask.sources, mask.sources + kWaveFoamMaskSlots, preset.masks))
            return &mask;
    return nullptr;
}

IDirect3DTexture9* WaterRenderer::MaskTexture(int32_t index) const
{
    const FoamMaskTexture* mask = SingleMask(index);
    return mask ? mask->texture : nullptr;
}

bool WaterRenderer::MaskPresent(const WaterPreset& preset, int slot) const
{
    if (slot >= kWaveFoamMaskSlots)
        return MaskTexture(preset.masks[slot]) != nullptr;
    const FoamMaskTexture* wave = WaveFoamMasks(preset);
    return wave && wave->present[slot];
}

WaterRippleStats WaterRenderer::RippleStats() const
{
    WaterRippleStats stats;
    stats.texels = m_ripples.Texels();
    stats.running = m_ripples.Running();
    stats.shaded = m_ripplesShaded;
    stats.contacts = m_contacts.Contacts();
    stats.tracks = m_contacts.Tracks();
    stats.steps = m_ripples.StepsRun();
    stats.droppedSteps = m_ripples.DroppedSteps();
    stats.restarts = m_rippleRestarts;
    return stats;
}

WaterRippleShading WaterRenderer::RippleShading() const
{
    WaterRippleShading shading;
    shading.map = m_ripplesShaded ? m_ripples.Map() : nullptr;
    std::memcpy(shading.window, &m_rippleWindow, sizeof(shading.window));
    std::memcpy(shading.shape, &m_rippleShape, sizeof(shading.shape));
    std::memcpy(shading.fade, &m_rippleFade, sizeof(shading.fade));
    return shading;
}

void WaterRenderer::SaveTargets(IDirect3DDevice9* dev)
{
    ReleaseTargets();
    for (DWORD i = 0; i < kMaxRenderTargets; ++i)
        dev->GetRenderTarget(i, &m_saved.colour[i]);
    dev->GetDepthStencilSurface(&m_saved.depth);
    dev->GetStreamSource(0, &m_saved.stream, &m_saved.streamOffset, &m_saved.streamStride);
}

void WaterRenderer::ReleaseTargets()
{
    m_stateCaptured = false;
    for (auto*& surface : m_saved.colour)
        SafeRelease(surface);
    SafeRelease(m_saved.depth);
    SafeRelease(m_saved.stream);
}

void WaterRenderer::CaptureClientState()
{
    m_state->Capture();
    m_stateCaptured = true;
}

void WaterRenderer::RestoreTargets(IDirect3DDevice9* dev)
{
    const bool captured = m_stateCaptured;
    m_stateCaptured = false;
    dev->SetRenderTarget(0, m_saved.colour[0]);
    for (DWORD i = 1; i < kMaxRenderTargets; ++i)
        dev->SetRenderTarget(i, m_saved.colour[i]);
    if (captured && m_state)
        m_state->Apply();
    dev->SetDepthStencilSurface(m_saved.depth);
    dev->SetStreamSource(0, m_saved.stream, m_saved.streamOffset, m_saved.streamStride);
    ReleaseTargets();
}

bool WaterRenderer::UsableTargets(IDirect3DSurface9* depthSurface, const D3DVIEWPORT9& vp, D3DSURFACE_DESC& depthDesc)
{
    const SavedTargets& saved = m_saved;
    D3DSURFACE_DESC rtDesc = {};
    if (!saved.colour[0])
        return Skip("no render target");
    if (saved.depth != depthSurface)
        return Skip("fog depth surface not bound");
    if (FAILED(saved.colour[0]->GetDesc(&rtDesc)) || FAILED(depthSurface->GetDesc(&depthDesc)))
        return Skip("surface description failed");
    if (rtDesc.Width != depthDesc.Width || rtDesc.Height != depthDesc.Height)
        return Skip("render target and depth sizes differ");
    if (!SameSampleCount(rtDesc, depthDesc))
        return Skip("render target and depth sample counts differ");
    if (vp.Width < kMinWorldViewportSide || vp.Height < kMinWorldViewportSide || vp.X + vp.Width > depthDesc.Width ||
        vp.Y + vp.Height > depthDesc.Height)
        return Skip("world viewport outside the render target");
    return true;
}

void WaterRenderer::SetPassState(IDirect3DDevice9* dev)
{
    dev->SetVertexShader(m_vs);
    dev->SetVertexDeclaration(m_decl);
    dev->SetStreamSourceFreq(0, 1);
    dev->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
    dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    dev->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    dev->SetRenderState(D3DRS_TWOSIDEDSTENCILMODE, FALSE);
    dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, kColourWriteAll);
    dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
    dev->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
    dev->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
}

bool WaterRenderer::CopySceneColour(IDirect3DDevice9* dev, IDirect3DSurface9* target, const D3DVIEWPORT9& vp)
{
    const RECT world = ViewportRect(vp);
    IDirect3DSurface9* copy = nullptr;
    const bool copied = SUCCEEDED(m_sceneColour->GetSurfaceLevel(0, &copy)) &&
                        SUCCEEDED(dev->StretchRect(target, &world, copy, nullptr, D3DTEXF_POINT));
    SafeRelease(copy);
    return copied;
}

void WaterRenderer::CopyLinearDepth(IDirect3DDevice9* dev, IDirect3DTexture9* depthTexture, IDirect3DTexture9* copy,
                                     bool withPlayerMask)
{
    SetTarget(dev, copy);
    dev->SetPixelShader(m_packedDepth ? m_packedDepthCopy : m_depthCopy);
    dev->SetPixelShaderConstantF(0, &m_common[0][0], kCommonConstants);
    BindPointSampler(dev, 0, depthTexture, D3DTADDRESS_CLAMP);
    // The player's pixels this frame (player_mask.h), into the copy's g: vw_linear_depth.hlsl cPlayerMask.
    UINT maskW = 0, maskH = 0;
    IDirect3DTexture9* mask = withPlayerMask && m_maskInDepth ? player_mask::ThisFrame(dev, maskW, maskH) : nullptr;
    const float maskConstant[4] = {mask ? 1.0f : 0.0f, mask ? 1.0f / static_cast<float>(maskW) : 0.0f,
                                   mask ? 1.0f / static_cast<float>(maskH) : 0.0f, 0.0f};
    dev->SetPixelShaderConstantF(kCommonConstants, maskConstant, 1);
    if (mask)
        BindPointSampler(dev, 1, mask, D3DTADDRESS_CLAMP);
    if (withPlayerMask)
        m_playerMaskCopied = mask != nullptr;
    DrawFullscreenTriangle(dev);
}

void WaterRenderer::ClearWaterStencil(IDirect3DDevice9* dev, IDirect3DSurface9* target,
                                      IDirect3DSurface9* depthSurface, const D3DVIEWPORT9& vp)
{
    dev->SetRenderTarget(0, target);
    dev->SetDepthStencilSurface(depthSurface);
    dev->SetViewport(&vp);
    dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_STENCILWRITEMASK, kStencilAllBits);
    const D3DRECT world = {static_cast<LONG>(vp.X), static_cast<LONG>(vp.Y), static_cast<LONG>(vp.X + vp.Width),
                           static_cast<LONG>(vp.Y + vp.Height)};
    dev->Clear(1, &world, D3DCLEAR_STENCIL, 0, 1.0f, 0);
}

void WaterRenderer::ArmStencilWrites(IDirect3DDevice9* dev)
{
    for (int i = 0; i < kWaterStencilStates; ++i)
    {
        dev->GetRenderState(kPassStencil[i].state, &m_clientStencil[i]);
        dev->SetRenderState(kPassStencil[i].state, kPassStencil[i].value);
    }
    m_stencilArmed = true;
}

void WaterRenderer::RestoreClientStencil(IDirect3DDevice9* dev)
{
    if (!m_stencilArmed)
        return;
    m_stencilArmed = false;
    if (!dev)
        return;
    for (int i = 0; i < kWaterStencilStates; ++i)
        dev->SetRenderState(kPassStencil[i].state, m_clientStencil[i]);
}

bool WaterRenderer::Begin(IDirect3DDevice9* dev, const SceneDepth& depth, const FrameInputs& in,
                          const WaterInputs& water, const Config& cfg)
{
    m_skip = "";
    m_playerMaskCopied = false;
    if (m_armed)
        Abort(dev);
    if (!cfg.water)
        return Skip("water disabled");
    if (in.inLiquid)
        return Skip("camera under water");
    if (!GlobalWaterData().Loaded())
        return Skip("no water data");
    if (!dev || !depth.texture || !depth.bound)
        return Skip("fog depth unavailable");
    if (dev->TestCooperativeLevel() != D3D_OK)
        return Skip("device not ready");
    if (!EnsureShaders(dev) || !EnsureStateBlock(dev) || !EnsureFlatTexture(dev))
        return false;

    SaveTargets(dev);
    D3DSURFACE_DESC depthDesc = {};
    const D3DVIEWPORT9& vp = in.viewport;
    if (!UsableTargets(depth.bound, vp, depthDesc) || !EnsureCopies(dev, m_saved.colour[0], vp.Width, vp.Height))
    {
        ReleaseTargets();
        return false;
    }
    if (!BuildCommonConstants(in, depthDesc, m_common))
    {
        ReleaseTargets();
        return Skip("view matrix not invertible");
    }
    EnsureMasks(dev);

    if (LogEnabled(LogLevel::Info))
        m_gpuTimer.Begin(dev);
    CaptureClientState();
    for (DWORD i = 1; i < kMaxRenderTargets; ++i)
        dev->SetRenderTarget(i, nullptr);
    const bool colourCopied = CopySceneColour(dev, m_saved.colour[0], vp);
    const bool copied = colourCopied && depth.Refresh(dev);
    if (copied)
    {
        dev->SetDepthStencilSurface(nullptr);
        SetPassState(dev);
        RaiseInjectedFault(WaterFaultStage::Begin);
        CopyLinearDepth(dev, depth.texture, m_sceneDepth, true);
        ClearWaterStencil(dev, m_saved.colour[0], depth.bound, vp);
    }
    RestoreTargets(dev);
    if (!copied)
    {
        m_gpuTimer.Cancel();
        return Skip(colourCopied ? "multisampled depth copy failed" : "scene colour copy failed");
    }
    m_gpuTimer.Pause();

    ArmStencilWrites(dev);
    m_in = in;
    m_water = water;
    m_cfg = cfg;
    ChooseWaterLamps(in, m_lamps);
    std::fill(std::begin(m_draws), std::end(m_draws), 0u);
    m_armed = true;
    return true;
}

void WaterRenderer::Tag(IDirect3DDevice9* dev, WaterClass waterClass)
{
    const int index = static_cast<int>(waterClass);
    if (!m_stencilArmed || index < 0 || index >= kWaterClassCount)
        return;
    dev->SetRenderState(D3DRS_STENCILREF, static_cast<DWORD>(index));
    ++m_draws[index];
}

void WaterRenderer::Untag(IDirect3DDevice9* dev)
{
    if (m_stencilArmed)
        dev->SetRenderState(D3DRS_STENCILREF, kUntaggedStencil);
}

void WaterRenderer::Abort(IDirect3DDevice9* dev)
{
    if (dev && m_stateCaptured)
        RestoreTargets(dev);
    else
        ReleaseTargets();
    RestoreClientStencil(dev);
    m_gpuTimer.Cancel();
    m_armed = false;
}

bool WaterRenderer::AnyClassDrawn() const
{
    for (int index = 1; index < kWaterClassCount; ++index)
        if (m_draws[index])
            return true;
    return false;
}

uint32_t WaterRenderer::DrawnTileMask() const
{
    const WaterData& data = GlobalWaterData();
    uint32_t mask = 0;
    for (int index = 1; index < kWaterClassCount; ++index)
    {
        const WaterPreset* preset = m_draws[index] ? data.Preset(static_cast<WaterClass>(index)) : nullptr;
        if (!preset)
            continue;
        for (int32_t tile : preset->tiles)
            if (tile >= 0 && tile < kWaterMaxTiles && static_cast<size_t>(tile) < data.Tiles().size())
                mask |= 1u << tile;
    }
    return mask;
}

bool WaterRenderer::PrepareWaves(IDirect3DDevice9* dev)
{
    m_waveTiles = DrawnTileMask();
    m_waveSettings = {};
    m_waveSettings.resolution = m_cfg.waterQuality >= kHighQuality ? kFftResolution : kFftResolutionLow;
    m_waveSettings.referenceResolution = kFftReferenceResolution;
    m_waveSettings.windSpeed = m_cfg.waterWind;
    m_waveSettings.windDirection[0] = kWindDirection[0];
    m_waveSettings.windDirection[1] = kWindDirection[1];
    m_waveSettings.amplitudeScale = m_cfg.waterWaves * m_cfg.waterWaves;
    m_wavesAttempted = !g_waveSimulationDisabled && m_waveTiles != 0 && m_cfg.waterWaves > 0.0f;
    return m_wavesAttempted && m_fft.Prepare(dev, m_waveSettings, GlobalWaterData().Tiles(), m_waveTiles);
}

bool WaterRenderer::SimulateWaves(IDirect3DDevice9* dev, double seconds)
{
    const double step = m_lastSeconds < 0.0 ? 0.0 : std::clamp(seconds - m_lastSeconds, 0.0, kMaxFoamStepSeconds);
    return m_fft.Run(dev, m_waveSettings, GlobalWaterData().Tiles(), seconds, static_cast<float>(step));
}

bool WaterRenderer::RippleContinuityBroken(double seconds) const
{
    return m_lastRippleSeconds >= 0.0 && (seconds < m_lastRippleSeconds ||
                                          seconds - m_lastRippleSeconds > kRippleGapSeconds ||
                                          m_in.mapId != m_rippleMapId);
}

void WaterRenderer::RestartRipples()
{
    m_ripples.Restart();
    m_contacts.Reset();
    ++m_rippleRestarts;
}

void WaterRenderer::ReleaseRipples()
{
    m_ripples.ReleaseAll();
    m_contacts.Reset();
    m_lastRippleSeconds = -1.0;
    m_ripplesAvailable = true;
}

bool WaterRenderer::SimulateRipples(IDirect3DDevice9* dev, double seconds)
{
    if (m_cfg.waterRipples <= 0.0f)
    {
        ReleaseRipples();
        return false;
    }
    if (RippleContinuityBroken(seconds))
        RestartRipples();
    m_lastRippleSeconds = seconds;
    m_rippleMapId = m_in.mapId;
    m_contacts.Update(m_water.contacts, seconds);
    m_summaryContacts = std::max(m_summaryContacts, m_contacts.Contacts());
    m_summaryBobbers = std::max(m_summaryBobbers, m_water.contacts.bobberCount);
    m_summaryFishingNodes = std::max(m_summaryFishingNodes, m_water.contacts.fishingNodes);
    m_summaryRejectedNodes = std::max(m_summaryRejectedNodes, m_water.contacts.rejectedNodes);
    m_summarySchools = std::max(m_summarySchools, m_water.contacts.schoolCount);
    const int texels = m_cfg.waterQuality == kLowQuality ? kWaterRippleTexelsLow : kWaterRippleTexels;
    float centre[2];
    RippleCentre(seconds, texels, centre);
    CollectRippleWaterfalls(centre, texels);
    const bool supported = m_ripples.Supported(dev);
    if (!m_ripples.Running() && !m_contacts.Emitting())
    {
        m_ripplesAvailable = m_ripplesAvailable && supported;
        return false;
    }
    m_ripplesAvailable = m_ripples.Prepare(dev, texels);
    if (!m_ripplesAvailable)
    {
        const char* failure = m_ripples.LastFailure();
        if (std::strcmp(failure, m_loggedRippleFailure) != 0)
            VF_LOG_INFO("water ripples skipped: %s", failure);
        m_loggedRippleFailure = failure;
        return false;
    }
    m_loggedRippleFailure = "";
    if (!m_ripples.Running())
        m_contacts.PlaceFootprints(seconds);
    const WaterRippleSchedule schedule = m_ripples.Schedule(seconds);
    WaterRippleDisturbance disturbances[kMaxWaterRippleDisturbances];
    // The current around the player (water_flow.h): the ripples drift with it.
    float flowMapping[4] = {};
    const bool riverFlow = m_cfg.waterRiverFlow && m_riverFlow.Update(dev, m_in.mapId, RippleFocus());
    if (riverFlow)
        m_riverFlow.Mapping(flowMapping);
    for (int step = 0; step < schedule.steps; ++step)
    {
        const uint32_t count =
            m_contacts.TakeDisturbances(schedule.stepSeconds[step], disturbances, kMaxWaterRippleDisturbances);
        m_ripples.Step(dev, centre, disturbances, count, riverFlow ? m_riverFlow.Texture() : nullptr,
                       riverFlow ? flowMapping : nullptr);
    }
    m_summaryRippleTexels = std::max(m_summaryRippleTexels, texels);
    m_summaryDroppedSteps += schedule.dropped;
    return m_ripples.Visible();
}

// Where ripples centre: the player when the contact walk found them, else the client's camera target (a yard in
// front of the camera, which a zoomed-out camera holds 10-50 yd from the player: centred there, the window swung
// with the camera and the player's own wakes and footsteps slid out of it and blinked away).
const float* WaterRenderer::RippleFocus() const
{
    return m_water.contacts.hasPlayer ? m_water.contacts.player : m_in.camTarget;
}

// The window follows the player, leaning toward a bobber that a long cast put in the faded edge: the
// player's own if there is one, else the nearest far one (another player's), just far enough that it is at full
// strength and at most halfway, so the player stays in too. It leans out at once, so the landing plop (due the
// frame the bobber appears) is inside, and glides back once the bobber is gone so wakes behind the player do not
// pop back in. While the player is in the water (swimming, wading, walking on it), only their own bobber leans it:
// leaning toward someone else's slid the player's own wakes and footsteps to the faded edge, so they blinked out.
void WaterRenderer::RippleCentre(double seconds, int texels, float centre[2])
{
    const float* focus = RippleFocus();
    float target[2] = {0.0f, 0.0f};
    float bobber[2];
    bool own = false;
    const float extent = texels * kWaterRippleTexelYards;
    const float reach = extent * (0.5f - kRippleEdgeRampPerExtent) - kRippleFadeYards - kRippleBobberMarginYards;
    if (m_contacts.FocusBobber(focus, reach, bobber, &own) &&
        (own || !m_contacts.ContactNear(focus, kRippleSelfContactYards)))
    {
        const float dx = bobber[0] - focus[0];
        const float dy = bobber[1] - focus[1];
        const float distance = std::hypot(dx, dy);
        if (reach > 0.0f && distance > reach)
        {
            const float shift = std::min(distance - reach, 0.5f * distance);
            target[0] = dx / distance * shift;
            target[1] = dy / distance * shift;
        }
    }
    const double elapsed = m_lastBiasSeconds >= 0.0
                               ? std::clamp(seconds - m_lastBiasSeconds, 0.0, kRippleBiasMaxStepSeconds)
                               : kRippleBiasMaxStepSeconds;
    m_lastBiasSeconds = seconds;
    const bool leaningOut = std::hypot(target[0], target[1]) > std::hypot(m_rippleBias[0], m_rippleBias[1]);
    const float step = static_cast<float>(kRippleBiasYardsPerSecond * elapsed);
    const float delta[2] = {target[0] - m_rippleBias[0], target[1] - m_rippleBias[1]};
    const float length = std::hypot(delta[0], delta[1]);
    const float scale = leaningOut || length <= step ? 1.0f : step / length;
    for (int axis = 0; axis < 2; ++axis)
    {
        m_rippleBias[axis] += delta[axis] * scale;
        centre[axis] = focus[axis] + m_rippleBias[axis];
    }
}

// Waterfalls on this map whose base lies inside the ripple window (plus a little, for wide falls at the edge), in
// the height band around the player. One line per map change says how many the map has nearby.
void WaterRenderer::CollectRippleWaterfalls(const float centre[2], int texels)
{
    const WaterfallEmitter* active[kMaxActiveWaterfalls];
    const float range = 0.5f * texels * kWaterRippleTexelYards + kRippleFadeYards;
    const float height = RippleFocus()[2];
    const uint32_t count = CollectWaterfalls(m_in.mapId, centre, height, range, active, kMaxActiveWaterfalls);
    m_contacts.SetWaterfalls(active, count);
    // Rocks in moving water inside the window (water_flow.h): held still while the current runs past them.
    const RiverRock* rocks[kMaxActiveRocks];
    float flows[kMaxActiveRocks * 2] = {};
    uint32_t rockCount = 0;
    if (m_cfg.waterRiverFlow)
    {
        const RiverRock* found[kMaxActiveRocks];
        const uint32_t n = CollectRiverRocks(m_in.mapId, centre, height, range, found, kMaxActiveRocks);
        for (uint32_t i = 0; i < n; ++i)
        {
            float fx = 0.0f, fy = 0.0f;
            if (!RiverFlowAt(m_in.mapId, found[i]->x, found[i]->y, fx, fy))
                continue;
            rocks[rockCount] = found[i];
            flows[2 * rockCount] = fx;
            flows[2 * rockCount + 1] = fy;
            ++rockCount;
        }
    }
    m_contacts.SetRocks(rocks, flows, rockCount);
    m_summaryWaterfalls = std::max(m_summaryWaterfalls, count);
    float nearest = 0.0f, baseHeight = 0.0f;
    if (NearestWaterfall(m_in.mapId, centre, kWaterfallLogRange, nearest, baseHeight) && nearest < m_summaryNearestFall)
    {
        m_summaryNearestFall = nearest;
        m_summaryNearestFallDrop = height - baseHeight;
    }
    if (m_in.mapId != m_loggedWaterfallMap)
    {
        m_loggedWaterfallMap = m_in.mapId;
        uint32_t onMap = 0, total = 0;
        const WaterfallEmitter* table = WaterfallTable(total);
        for (uint32_t i = 0; i < total; ++i)
            onMap += table[i].map == m_in.mapId;
        VF_LOG_INFO("water waterfalls: map %d has %u turbulence sources (of %u)", m_in.mapId, onMap, total);
    }
}

// White water at the waterfalls in view: the nearest 16 within kWaterfallFoamRange of the player, in their height
// band (the same lookup as the churn). Radius and strength by kind; the height band is tight indoors,
// where levels stack, and grows with a tall outdoor fall, whose box can reach well below the pool's surface.
// The lips near the player (falls' "hat" models: where a river runs smooth and fast over the edge), the nearest 8
// within the foam range and the same height band. Per lip: its centre and flow direction (the right-hand normal of
// a -> b), the half width at the edge, how far upstream the approach shows, and its surface height.
void WaterRenderer::FillLips(ShadingConstants& c, const FrameInputs& in, double seconds) const
{
    const WaterfallEmitter* lips[kMaxActiveLips];
    const float* focus = RippleFocus();
    const uint32_t count = CollectLips(in.mapId, focus, focus[2], kWaterfallFoamRange, lips, kMaxActiveLips);
    c.lipControl = {static_cast<float>(count), kLipStrength,
                    static_cast<float>(std::fmod(std::max(seconds, 0.0), 256.0)), 0.0f};
    for (uint32_t i = 0; i < kMaxActiveLips; ++i)
    {
        if (i >= count)
        {
            c.lipFrame[i] = {};
            c.lipShape[i] = {};
            continue;
        }
        const WaterfallEmitter& e = *lips[i];
        const float dx = e.b[0] - e.a[0], dy = e.b[1] - e.a[1];
        const float width = std::hypot(dx, dy);
        const float inverse = width > 0.01f ? 1.0f / width : 0.0f;
        c.lipFrame[i] = {0.5f * (e.a[0] + e.b[0]), 0.5f * (e.a[1] + e.b[1]), dy * inverse, -dx * inverse};
        c.lipShape[i] = {0.5f * width, std::max(e.thickness, 1.0f), 0.5f * (e.a[2] + e.b[2]), 1.0f};
    }
}

void WaterRenderer::FillFallFoam(ShadingConstants& c, const FrameInputs& in, double seconds) const
{
    const WaterfallEmitter* falls[kMaxActiveWaterfalls];
    const float* focus = RippleFocus();
    const uint32_t count =
        CollectWaterfalls(in.mapId, focus, focus[2], kWaterfallFoamRange, falls, kMaxActiveWaterfalls);
    c.fallFoamControl = {static_cast<float>(count), 1.0f, static_cast<float>(std::fmod(std::max(seconds, 0.0), 256.0)),
                         0.0f};
    for (uint32_t i = 0; i < kMaxActiveWaterfalls; ++i)
    {
        if (i >= count)
        {
            c.fallFoamSegments[i] = {};
            c.fallFoamShape[i] = {};
            continue;
        }
        float segment[4], shape[4];
        FallFoamShape(*falls[i], segment, shape);
        c.fallFoamSegments[i] = {segment[0], segment[1], segment[2], segment[3]};
        c.fallFoamShape[i] = {shape[0], shape[1], shape[2], shape[3]};
    }
}

void WaterRenderer::FillRippleConstants(double seconds)
{
    const WaterRippleWindow window = m_ripples.Window(seconds);
    const float extent = window.texels > 0 ? window.extent : 1.0f;
    const float texelUv = window.texels > 0 ? 1.0f / window.texels : 0.0f;
    m_rippleWindow = {window.origin[0], window.origin[1], 1.0f / extent, texelUv};
    m_rippleShape = {window.weight, m_ripplesShaded ? m_cfg.waterRipples : 0.0f, extent, 0.0f};
    m_rippleFade = {extent * (0.5f - kRippleEdgeRampPerExtent), 1.0f / kRippleFadeYards, kWaterRippleTexelYards,
                    0.0f};
}

void WaterRenderer::LogWaveState()
{
    if (!m_wavesAttempted)
        return;
    if (m_wavesSimulated)
        m_fft.LogPlan();
    const char* failure = m_fft.LastFailure();
    const char* state = m_wavesSimulated ? "" : (failure && *failure ? failure : "unknown failure");
    if (m_waveStateLogged && std::strcmp(state, m_loggedWaveState) == 0)
        return;
    if (m_wavesSimulated)
        VF_LOG_INFO("water waves simulated (tiles 0x%02X, %d texels)", m_waveTiles, m_waveSettings.resolution);
    else
        VF_LOG_INFO("water waves unavailable, shading flat water: %s", state);
    m_loggedWaveState = state;
    m_waveStateLogged = true;
}

void WaterRenderer::FillClassConstants(ShadingConstants& c, const WaterPreset& preset, WaterClass waterClass,
                                       double seconds) const
{
    c = {};
    const WaterData& data = GlobalWaterData();
    // Interior water is no longer a special case: it shades exactly like lake/ocean water.
    float toLight[3] = {m_in.toLight[0], m_in.toLight[1], m_in.toLight[2]};
    const float length = std::sqrt(toLight[0] * toLight[0] + toLight[1] * toLight[1] + toLight[2] * toLight[2]);
    for (float& axis : toLight)
        axis = length > 1e-6f ? axis / length : 0.0f;
    const float sunVisibility = SmoothStep(-kSunVisibilityHalfWidth, kSunVisibilityHalfWidth, toLight[2]);
    const float sunTransmission = 1.0f - FresnelSchlick(Saturate(toLight[2]));
    float sun[3];
    LinearColour(m_in.directColor, sun);
    for (float& channel : sun)
        channel *= m_in.lightIsMoon ? kMoonLightScale : 1.0f;
    float sky[kSkyColorCount][3];
    float ambient[3] = {};
    for (int i = 0; i < kSkyColorCount; ++i)
    {
        LinearColour(m_water.skyColors[i], sky[i]);
        for (int channel = 0; channel < 3; ++channel)
            ambient[channel] += sky[i][channel] / kSkyColorCount;
    }

    c.light = {toLight[0], toLight[1], toLight[2], sunVisibility};
    c.sunColour = {sun[0], sun[1], sun[2], sunTransmission};
    c.ambient = {ambient[0], ambient[1], ambient[2], 0.0f};
    c.isotropicLight = Float4{ambient[0] + sunVisibility * sun[0], ambient[1] + sunVisibility * sun[1],
                              ambient[2] + sunVisibility * sun[2], 0.0f};
    for (int band = 0; band < kWaterSkyBands; ++band)
    {
        const float* colour = sky[kSkyColourOfBand[band]];
        c.sky[band] = Float4{colour[0], colour[1], colour[2], 0.0f};
    }
    if (m_water.stockFogApplies)
    {
        float fog[3];
        UnpackColor(m_in.fogColor, fog);
        c.stockFogColour = {fog[0], fog[1], fog[2], 1.0f};
        c.stockFog = {m_in.fogEnd, 1.0f / std::max(m_in.fogEnd - m_in.fogStart, kMinStockFogRange), 0.0f, 0.0f};
    }

    const float absorptionScale = preset.absorption[3] / std::max(m_cfg.waterClarity, kMinClarity);
    const float presetAbsorption[3] = {preset.absorption[0] * absorptionScale, preset.absorption[1] * absorptionScale,
                                       preset.absorption[2] * absorptionScale};
    c.scatteringIntensities = {preset.scatteringIntensities[0], preset.scatteringIntensities[1],
                               preset.scatteringIntensities[2], preset.scatteringIntensities[3]};
    // Lake is the only shading class now; it uses the river/lake zone palette.
    const uint32_t* zone = m_water.riverColors;
    float closeWater[3];
    float farWater[3];
    LinearColour(zone[kCloseWaterColour], closeWater);
    LinearColour(zone[kFarWaterColour], farWater);
    const float zoneWeight = Saturate(m_cfg.waterZoneColors);
    auto towardZone = [zoneWeight](const float* authored, const float* zoneColour, float w) {
        float rgb[3];
        ZoneTintedColour(authored, zoneColour, zoneWeight, rgb);
        return Float4{rgb[0], rgb[1], rgb[2], w};
    };
    c.scatteringTop = towardZone(preset.scatteringTop, closeWater, preset.scatteringTop[3]);
    c.scatteringBottom = towardZone(preset.scatteringBottom, farWater, preset.scatteringBottom[3]);
    float absorption[3];
    // The Clarity slider scales the zone's opacity as it scales the preset (0.7 is its default).
    const float opacity = m_water.waterAlphaValid ? m_water.waterAlpha[0] : -1.0f;
    ZoneTintedAbsorption(presetAbsorption, closeWater, opacity,
                         kDefaultWaterClarity / std::max(m_cfg.waterClarity, kMinClarity), zoneWeight, absorption);
    c.absorption = {absorption[0], absorption[1], absorption[2], 0.0f};

    const float foam = std::max(m_cfg.waterFoam, 0.0f);
    c.depthFadeFoam = {std::max(preset.depthFadeFoam[0], 0.0f) * foam, preset.depthFadeFoam[1],
                       preset.depthFadeFoam[2], 0.0f};
    c.shoreFoam = {std::max(preset.shoreFoam[0], 0.0f) * foam, preset.shoreFoam[1], preset.shoreFoam[2],
                   kShoreDistancePerDepth};
    c.waveFoam = {std::max(preset.waveFoam[0], 0.0f) * foam, 0.0f, 0.0f, 0.0f};
    c.waveFoamScaling = {preset.waveFoamScaling[0], preset.waveFoamScaling[1], preset.waveFoamScaling[2], 0.0f};
    c.surfaceResponse = {preset.roughness[0], preset.roughness[1], preset.roughness[2] * m_cfg.waterReflections,
                         m_cfg.waterSpecular};

    float inverse[kWaterPresetTiles] = {};
    int tileCount = 0;
    for (int k = 0; k < kWaterPresetTiles; ++k)
    {
        const int32_t tile = preset.tiles[k];
        if (tile < 0 || static_cast<size_t>(tile) >= data.Tiles().size() || !(data.Tiles()[tile].size > 0.0f))
            continue;
        inverse[k] = 1.0f / data.Tiles()[tile].size;
        ++tileCount;
    }
    c.inverseTileSizes = {inverse[0], inverse[1], inverse[2], inverse[3]};
    c.waveControl = {1.0f / static_cast<float>(std::max(tileCount, 1)), 0.0f, static_cast<float>(waterClass),
                     static_cast<float>(m_cfg.waterDebugView)};
    c.foamScroll = {ScrollOffset(seconds, 0, preset.waveFoam[1]), ScrollOffset(seconds, 1, preset.waveFoam[1]),
                    ScrollOffset(seconds, 0, preset.shoreFoam[3]), ScrollOffset(seconds, 1, preset.shoreFoam[3])};
    c.depthFoamScroll = {ScrollOffset(seconds, 0, preset.depthFadeFoam[3]),
                         ScrollOffset(seconds, 1, preset.depthFadeFoam[3]), 0.0f, 0.0f};
    c.reflectionFog = m_reflectionFog;
    c.depthDecode = CopiedDepthDecode();
    c.rippleWindow = m_rippleWindow;
    c.rippleShape = m_rippleShape;
    c.rippleFade = m_rippleFade;
    FillFallFoam(c, m_in, seconds);
    FillLips(c, m_in, seconds);
    // Surf at the coast: the ocean pass only (vw_surface.hlsli CoastSurf).
    c.coast = {waterClass == WaterClass::Ocean ? kCoastSurfStrength : 0.0f,
               static_cast<float>(std::fmod(std::max(seconds, 0.0), 256.0)), 0.0f, 0.0f};
    const WaterContactFrame& seen = m_water.contacts;
    const float lampStrength = std::max(m_cfg.waterLamps, 0.0f);
    const uint32_t lamps = lampStrength > 0.0f ? std::min(m_lamps.count, kMaxWaterLamps) : 0u;
    c.lampControl = {static_cast<float>(lamps), kLampStrength * lampStrength, lampStrength,
                     kLampRoughness * std::clamp(m_cfg.waterWaves / kLampRoughWaves, 0.0f, 1.0f)};
    for (uint32_t i = 0; i < kMaxWaterLamps; ++i)
    {
        const WaterLamp& lamp = m_lamps.lamp[i];
        const bool on = i < lamps;
        c.lampPosition[i] = on ? Float4{lamp.position[0], lamp.position[1], lamp.position[2], lamp.reach} : Float4{};
        c.lampColour[i] = on ? Float4{lamp.colour[0], lamp.colour[1], lamp.colour[2], lamp.shape} : Float4{};
        c.lampFalloff[i] = on ? Float4{lamp.falloff[0], lamp.falloff[1], lamp.falloff[2], lamp.mirrorReach} : Float4{};
    }
    // Lightning strikes flash over the water (vw_optics.hlsli FlashesOnWater), at their own brightness this frame; they
    // take no lamp's place.
    const uint32_t flashes = lampStrength > 0.0f ? std::min(m_lamps.flashes, kMaxWaterFlashes) : 0u;
    c.flashControl = {static_cast<float>(flashes), lampStrength, 0.0f, 0.0f};
    for (uint32_t i = 0; i < kMaxWaterFlashes; ++i)
    {
        const WaterFlash& flash = m_lamps.flash[i];
        const bool on = i < flashes;
        c.flashPosition[i] = on ? Float4{flash.position[0], flash.position[1], flash.position[2], flash.reach} : Float4{};
        c.flashColour[i] = on ? Float4{flash.colour[0], flash.colour[1], flash.colour[2], 0.0f} : Float4{};
    }
    // Sun glitter: the facets tilt further with more wind; none on flat water, less in the rain (its rings break the
    // surface and the sun is behind clouds).
    const float rain = water_rain::Intensity();
    const float waves = std::clamp(m_cfg.waterWaves / kGlitterFullWaves, 0.0f, 1.0f);
    c.glitter = {std::max(m_cfg.waterGlitter, 0.0f) * waves * (1.0f - kGlitterRainDim * rain),
                 (kGlitterTilt + kGlitterTiltPerWind * m_cfg.waterWind) * (0.5f + 0.5f * waves),
                 static_cast<float>(std::fmod(std::max(seconds, 0.0), 256.0)), kGlitterCell};
    // The steady steps reach over the screen's height (as an angle) at most.
    const float cotHalfHeight = std::fabs(m_in.glProjection[5]);
    c.reflectionSteps = {m_cfg.waterSteadyReflections ? 1.0f : 0.0f,
                         cotHalfHeight > 1e-3f ? std::clamp(2.0f * std::atan(1.0f / cotHalfHeight), 0.2f, 3.0f) : 1.0f,
                         0.0f, 0.0f};
    c.player = seen.hasPlayer ? Float4{seen.player[0], seen.player[1], seen.player[2],
                                       seen.playerMounted ? kMountedReflectionReach : kPlayerReflectionReach}
                              : Float4{};
    c.rainControl = {m_cfg.waterRainRipples * water_rain::Intensity(),
                     static_cast<float>(std::fmod(std::max(seconds, 0.0), 256.0)),
                     m_cfg.waterRainSize, m_cfg.waterRainDistance};
    for (int slot = 0; slot < kWaterShadedMaskSlots; ++slot)
    {
        const int32_t mask = preset.masks[slot];
        if (!MaskPresent(preset, slot))
            continue;
        const WaterMask& info = data.Masks()[mask].info;
        float low[3];
        float high[3];
        ClampedTint(info.tintLow, low);
        ClampedTint(info.tintHigh, high);
        c.maskTints[slot * 2] = {low[0], low[1], low[2], 1.0f};
        c.maskTints[slot * 2 + 1] = {high[0], high[1], high[2], 0.0f};
    }
}

void WaterRenderer::BuildReflectionFog()
{
    m_reflectionFog = {};
    if (m_water.stockFogApplies)
        return;
    AuthoredFog authored = {};
    const bool hasAuthored = m_cfg.dataMode == 1 &&
                             GlobalFogData().Resolve(m_in.mapId, m_in.camPos, m_in.dayFraction, m_in.lightParams,
                                                     authored);
    const FogParams fog = WithMeanNoise(BuildFogParams(m_in, m_cfg, hasAuthored ? &authored : nullptr));
    const float exposure = fog.authored ? m_cfg.classicExposure : m_cfg.exposure;
    auto linearScattered = [&fog, exposure](const float* rgb) {
        float out[3];
        for (int c = 0; c < 3; ++c)
            out[c] = (fog.linear ? rgb[c] : std::pow(std::max(rgb[c], 0.0f), kDisplayGamma)) * exposure;
        return Float4{out[0], out[1], out[2], 0.0f};
    };
    for (int i = 0; i < kFogLayers; ++i)
    {
        const FogLayer& layer = fog.layers[i];
        const float shadow = layer.shadowed * (1.0f - fog.lightAboveHorizon);
        float emissive[3];
        float diffuse[3];
        for (int c = 0; c < 3; ++c)
        {
            emissive[c] = layer.emissive[c] + (layer.shadowEmissive[c] - layer.emissive[c]) * shadow;
            diffuse[c] = layer.diffuse[c] * (1.0f - shadow);
        }
        const float density = layer.density * (1.0f + (layer.shadowDensity - 1.0f) * shadow);
        ReflectionFogLayer& out = m_reflectionFog.layers[i];
        out.curve = {layer.start, density, layer.strength, layer.exponent};
        out.height = {layer.upperHeight, layer.upperFalloff, layer.lowerHeight, layer.lowerFalloff};
        out.emissive = linearScattered(emissive);
        out.diffuse = linearScattered(diffuse);
        out.scattering = {layer.g, layer.isotropic, layer.endDistance, layer.skyFalloff};
    }
    m_reflectionFog.range = {fog.maxDistance, fog.maxDistance, 0.0f, kReflectionFogActive};
}

void WaterRenderer::BindClassTextures(IDirect3DDevice9* dev, const WaterPreset& preset)
{
    for (int k = 0; k < kWaterPresetTiles; ++k)
    {
        const int32_t tile = preset.tiles[k];
        IDirect3DTexture9* surface = m_wavesSimulated && tile >= 0 ? m_fft.Surface(tile) : nullptr;
        IDirect3DTexture9* foam = m_wavesSimulated && tile >= 0 ? m_fft.Foam(tile) : nullptr;
        BindSampler(dev, kFirstSurfaceStage + k, surface ? surface : m_flat, D3DTADDRESS_WRAP, D3DTEXF_LINEAR,
                    D3DTEXF_LINEAR);
        BindSampler(dev, kFirstFoamStateStage + k, foam ? foam : m_flat, D3DTADDRESS_WRAP, D3DTEXF_LINEAR,
                    D3DTEXF_NONE);
    }
    const FoamMaskTexture* wave = WaveFoamMasks(preset);
    IDirect3DTexture9* shore = MaskTexture(preset.masks[kShoreFoamSlot]);
    IDirect3DTexture9* depth = MaskTexture(preset.masks[kDepthFoamSlot]);
    BindSampler(dev, kWaveFoamMaskStage, wave ? wave->texture : m_flat, D3DTADDRESS_WRAP, D3DTEXF_LINEAR,
                D3DTEXF_LINEAR);
    BindSampler(dev, kShoreFoamMaskStage, shore ? shore : m_flat, D3DTADDRESS_WRAP, D3DTEXF_LINEAR, D3DTEXF_LINEAR);
    BindSampler(dev, kDepthFoamMaskStage, depth ? depth : m_flat, D3DTADDRESS_WRAP, D3DTEXF_LINEAR, D3DTEXF_LINEAR);
}

void WaterRenderer::ShadeClasses(IDirect3DDevice9* dev, IDirect3DSurface9* target, IDirect3DSurface9* depthSurface,
                                 double seconds)
{
    const D3DVIEWPORT9& vp = m_in.viewport;
    const RECT world = ViewportRect(vp);
    dev->SetRenderTarget(0, target);
    dev->SetDepthStencilSurface(depthSurface);
    dev->SetViewport(&vp);
    dev->SetScissorRect(&world);
    dev->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, kColourWriteRgb);
    dev->SetRenderState(D3DRS_STENCILENABLE, TRUE);
    dev->SetRenderState(D3DRS_STENCILFUNC, D3DCMP_EQUAL);
    dev->SetRenderState(D3DRS_STENCILPASS, D3DSTENCILOP_KEEP);
    dev->SetRenderState(D3DRS_STENCILFAIL, D3DSTENCILOP_KEEP);
    dev->SetRenderState(D3DRS_STENCILZFAIL, D3DSTENCILOP_KEEP);
    dev->SetRenderState(D3DRS_STENCILMASK, kStencilAllBits);
    dev->SetRenderState(D3DRS_STENCILWRITEMASK, 0);
    m_shadingVariant = g_forcedShadingVariant >= 0 && g_forcedShadingVariant < kWaterQualityLevels
                           ? g_forcedShadingVariant
                           : ShadingVariant(m_cfg);
    // With this frame's player mask in the scene depth copy, the variant that looks past the player by it.
    IDirect3DPixelShader9* masked = m_playerMaskCopied ? m_shadeMasked[m_shadingVariant] : nullptr;
    dev->SetPixelShader(masked ? masked : m_shade[m_shadingVariant]);
    dev->SetPixelShaderConstantF(0, &m_common[0][0], kCommonConstants);
    BindPointSampler(dev, kSceneColourStage, m_sceneColour, D3DTADDRESS_MIRROR);
    BindPointSampler(dev, kSceneDepthStage, m_sceneDepth, D3DTADDRESS_MIRROR);
    BindPointSampler(dev, kWaterDepthStage, m_waterDepth, D3DTADDRESS_CLAMP);
    BindSampler(dev, kRippleStage, m_ripplesShaded ? m_ripples.Map() : m_flat, D3DTADDRESS_CLAMP, D3DTEXF_LINEAR,
                D3DTEXF_NONE);
    // The current around the player (water_flow.h), rebuilt as they move.
    const bool riverFlow = m_cfg.waterRiverFlow && m_riverFlow.Update(dev, m_in.mapId, RippleFocus());
    Float4 riverFlowMap = {};
    if (riverFlow)
        m_riverFlow.Mapping(&riverFlowMap.x);
    BindSampler(dev, kRiverFlowStage, riverFlow ? m_riverFlow.Texture() : m_flat, D3DTADDRESS_CLAMP, D3DTEXF_LINEAR,
                D3DTEXF_NONE);
    // The floor under the player's body (water_floor.h), refitted as they move: their reach, as the shader's BodyRadius.
    {
        const WaterContactFrame& seen = m_water.contacts;
        m_floor.Update(seen.hasPlayer, seen.player, seen.playerMounted ? kMountedReflectionReach : kBodyReach, seconds);
        m_floorWide.Update(seen.hasPlayer, seen.player, water_floor::kWideReach, seconds);
    }
    Float4 floorFit[3] = {}, floorWide[3] = {};
    m_floor.Constants(&floorFit[0].x, &floorFit[1].x, &floorFit[2].x);
    m_floorWide.Constants(&floorWide[0].x, &floorWide[1].x, &floorWide[2].x);
    const WaterData& data = GlobalWaterData();
    static_assert(sizeof(ShadingConstants) % sizeof(Float4) == 0, "water constants are whole registers");
    static_assert(kShadingFirstConstant + sizeof(ShadingConstants) / sizeof(Float4) <= kWaterPixelConstants,
                  "the water state block restores every constant the shading pass sets");
    for (int index = 1; index < kWaterClassCount; ++index)
    {
        const WaterClass waterClass = static_cast<WaterClass>(index);
        const WaterPreset* preset = m_draws[index] ? data.Preset(waterClass) : nullptr;
        if (!preset)
            continue;
        ShadingConstants constants;
        FillClassConstants(constants, *preset, waterClass, seconds);
        constants.riverFlowMap = riverFlowMap;
        constants.riverFlowControl = {static_cast<float>(std::fmod(std::max(seconds + RiverClockOffset(), 0.0), 256.0)),
                                      0.0f, 0.0f, 0.0f};
        constants.floorFit0 = floorFit[0];
        constants.floorFit1 = floorFit[1];
        constants.floorFit2 = floorFit[2];
        constants.floorWide0 = floorWide[0];
        constants.floorWide1 = floorWide[1];
        constants.floorWide2 = floorWide[2];
        dev->SetPixelShaderConstantF(kShadingFirstConstant, &constants.light.x,
                                     sizeof(constants) / sizeof(Float4));
        BindClassTextures(dev, *preset);
        dev->SetRenderState(D3DRS_STENCILREF, static_cast<DWORD>(index));
        DrawFullscreenTriangle(dev);
        ++m_shadedClasses;
        m_shadedClassMask |= 1u << index;
    }
}

// The linear depth copies' decode: R32F as is, or packed bytes.
WaterRenderer::Float4 WaterRenderer::CopiedDepthDecode() const
{
    const float packedUnit = kWaterMaxViewDepth / kPackedDepthLevels * kByteMax;
    return m_packedDepth ? Float4{packedUnit * kHighByteWeight, packedUnit * kMidByteWeight, packedUnit, 0.0f}
                         : Float4{1.0f, 0.0f, 0.0f, 0.0f};
}

// After shading, every tagged water pixel's depth moves kWaterDepthPushYards straight down (vw_depth_push.hlsl),
// from the water's linear depth copy, and no further than the scene's copy from before the water; colour is left
// alone. Runs with ShadeClasses' targets and stencil.
void WaterRenderer::PushWaterDepth(IDirect3DDevice9* dev)
{
    if (!m_depthPush || !m_waterDepth || !m_sceneDepth)
        return;
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0);
    dev->SetRenderState(D3DRS_STENCILENABLE, TRUE);
    dev->SetRenderState(D3DRS_STENCILFUNC, D3DCMP_NOTEQUAL);
    dev->SetRenderState(D3DRS_STENCILREF, kUntaggedStencil);
    dev->SetRenderState(D3DRS_STENCILMASK, kStencilAllBits);
    dev->SetRenderState(D3DRS_STENCILWRITEMASK, 0);
    dev->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
    dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_ALWAYS);
    dev->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
    dev->SetPixelShader(m_depthPush);
    dev->SetPixelShaderConstantF(0, &m_common[0][0], kCommonConstants);
    const Float4 decode = CopiedDepthDecode();
    // The falls nearby (as their foam finds them), each fall's lip: the top edge of its box.
    float lips[kMaxPushLips * 8] = {};
    uint32_t lipCount = 0;
    {
        const WaterfallEmitter* falls[kMaxActiveWaterfalls];
        const float* focus = RippleFocus();
        const uint32_t count =
            CollectWaterfalls(m_in.mapId, focus, focus[2], kWaterfallFoamRange, falls, kMaxActiveWaterfalls);
        for (uint32_t i = 0; i < count && lipCount < kMaxPushLips; ++i)
        {
            const WaterfallEmitter& e = *falls[i];
            if (e.kind != kWaterfallFall || e.height < kLipPushMinHeight)
                continue;
            float* l = lips + lipCount * 8;
            l[0] = e.a[0];
            l[1] = e.a[1];
            l[2] = e.a[2] + e.height;
            l[3] = 0.5f * e.thickness + kLipReachYards;
            l[4] = e.b[0];
            l[5] = e.b[1];
            l[6] = e.b[2] + e.height;
            l[7] = std::min(e.height, kLipPushMaxYards);
            ++lipCount;
        }
    }
    const float push[4] = {kWaterDepthPushYards, kWaterDepthPushMaxViewYards, static_cast<float>(lipCount), 0.0f};
    dev->SetPixelShaderConstantF(kCommonConstants, &decode.x, 1);
    dev->SetPixelShaderConstantF(kCommonConstants + 1, push, 1);
    dev->SetPixelShaderConstantF(kCommonConstants + 2, lips, kMaxPushLips * 2);
    BindPointSampler(dev, 0, m_waterDepth, D3DTADDRESS_CLAMP);
    BindPointSampler(dev, 1, m_sceneDepth, D3DTADDRESS_CLAMP);
    DrawFullscreenTriangle(dev);
}

bool WaterRenderer::End(IDirect3DDevice9* dev, const SceneDepth& depth)
{
    m_shadedClasses = 0;
    m_shadedClassMask = 0;
    m_wavesSimulated = false;
    m_ripplesShaded = false;
    if (!m_armed)
        return Skip("water pass not armed");
    RestoreClientStencil(dev);
    m_armed = false;
    m_gpuTimer.Resume();
    const bool shaded = ShadeTaggedWater(dev, depth);
    m_gpuTimer.End();
    AddToSummary(shaded);
    LogSummaryWhenDue(dev);
    return shaded;
}

void WaterRenderer::AddToSummary(bool shaded)
{
    if (!shaded)
        return;
    m_summaryClasses |= m_shadedClassMask;
    if (!m_wavesSimulated)
        return;
    m_summaryWaveResolution = m_waveSettings.resolution;
    int tiles = 0;
    for (uint32_t mask = m_waveTiles; mask; mask &= mask - 1)
        ++tiles;
    m_summaryWaveTiles = std::max(m_summaryWaveTiles, tiles);
}

void WaterRenderer::LogSummaryWhenDue(IDirect3DDevice9* dev)
{
    if (!LogEnabled(LogLevel::Info) || !dev)
        return;
    const double now = QpcSeconds();
    if (m_summaryStart < 0.0)
        m_summaryStart = now;
    if (!g_summaryForced && now - m_summaryStart < kSummarySeconds)
        return;
    g_summaryForced = false;
    m_summaryStart = now;
    char gpu[kSummaryTextSize] = {};
    DescribeGpuTime(m_gpuTimer, dev, gpu, sizeof(gpu));
    char classes[kSummaryTextSize] = {};
    for (int index = 1; index < kWaterClassCount; ++index)
        if (m_summaryClasses & (1u << index))
        {
            const size_t used = std::strlen(classes);
            std::snprintf(classes + used, sizeof(classes) - used, "%s%s", used ? "+" : "",
                          WaterClassLabel(static_cast<WaterClass>(index)));
        }
    char waves[kSummaryTextSize] = "flat";
    if (m_summaryWaveResolution > 0)
        std::snprintf(waves, sizeof(waves), "%d (%d tiles)", m_summaryWaveResolution, m_summaryWaveTiles);
    char ripples[kSummaryTextSize] = "off";
    if (m_summaryRippleTexels > 0)
        std::snprintf(ripples, sizeof(ripples), "%d at %.3f yd, %.0f Hz, up to %u contacts, %u steps dropped",
                      m_summaryRippleTexels, kWaterRippleTexelYards, kWaterRippleStepsPerSecond, m_summaryContacts,
                      m_summaryDroppedSteps);
    else if (m_cfg.waterRipples > 0.0f)
        std::snprintf(ripples, sizeof(ripples), "idle, up to %u contacts", m_summaryContacts);
    char bobbers[kSummaryTextSize] = "";
    const uint64_t bobberRipples = m_contacts.BobberRipplesTaken() - m_summaryBobberRipplesBase;
    if (m_summaryFishingNodes || bobberRipples)
        std::snprintf(bobbers, sizeof(bobbers), "; bobbers up to %u of %u fishing nodes (%u skipped), %llu rings",
                      m_summaryBobbers, m_summaryFishingNodes, m_summaryRejectedNodes,
                      static_cast<unsigned long long>(bobberRipples));
    char schools[kSummaryTextSize] = "";
    const uint64_t schoolRipples = m_contacts.SchoolRipplesTaken() - m_summarySchoolRipplesBase;
    if (m_summarySchools || schoolRipples)
        std::snprintf(schools, sizeof(schools), "; fish schools up to %u, %llu rings", m_summarySchools,
                      static_cast<unsigned long long>(schoolRipples));
    char waterfalls[kSummaryTextSize] = "";
    if (m_summaryWaterfalls || m_summaryNearestFall < kWaterfallLogRange)
        std::snprintf(waterfalls, sizeof(waterfalls), "; waterfalls up to %u, nearest %.0f yd away, %.0f yd below",
                      m_summaryWaterfalls, m_summaryNearestFall, m_summaryNearestFallDrop);
    // Water walking and restarts (a restart clears every ripple at once: after a gap over a second, a map change or
    // the clock going back).
    char walking[kSummaryTextSize] = "";
    const uint64_t footsteps = m_contacts.Footsteps() - m_summaryFootstepsBase;
    const uint64_t landings = m_contacts.Landings() - m_summaryLandingsBase;
    const uint32_t restarts = m_rippleRestarts - m_summaryRestartsBase;
    if (footsteps || landings || restarts)
        std::snprintf(walking, sizeof(walking), "; water walking %llu footsteps, %llu landings; %u ripple restarts",
                      static_cast<unsigned long long>(footsteps), static_cast<unsigned long long>(landings), restarts);
    VF_LOG_INFO("%s, classes %s, waves %s, ripples %s%s%s%s%s", gpu[0] ? gpu : "water gpu timing unavailable",
                classes[0] ? classes : "none", waves, ripples, bobbers, schools, waterfalls, walking);
    m_summaryFootstepsBase = m_contacts.Footsteps();
    m_summaryLandingsBase = m_contacts.Landings();
    m_summaryRestartsBase = m_rippleRestarts;
    m_summaryWaterfalls = 0;
    m_summaryNearestFall = kWaterfallLogRange;
    m_summaryClasses = 0;
    m_summaryWaveResolution = 0;
    m_summaryWaveTiles = 0;
    m_summaryRippleTexels = 0;
    m_summaryContacts = 0;
    m_summaryDroppedSteps = 0;
    m_summaryBobbers = 0;
    m_summaryFishingNodes = 0;
    m_summaryRejectedNodes = 0;
    m_summaryBobberRipplesBase = m_contacts.BobberRipplesTaken();
    m_summarySchools = 0;
    m_summarySchoolRipplesBase = m_contacts.SchoolRipplesTaken();
}

bool WaterRenderer::ShadeTaggedWater(IDirect3DDevice9* dev, const SceneDepth& depth)
{
    if (!AnyClassDrawn())
        return Skip("no water drawn");
    if (!dev || !depth.texture || !depth.bound || dev->TestCooperativeLevel() != D3D_OK || !m_state ||
        !m_sceneColour)
        return Skip("device not ready");
    SaveTargets(dev);
    D3DSURFACE_DESC depthDesc = {};
    if (!UsableTargets(depth.bound, m_in.viewport, depthDesc))
    {
        ReleaseTargets();
        return false;
    }
    const double seconds = WaterSeconds();
    const double frameStep = m_lastFrameSeconds < 0.0 ? 0.0 : seconds - m_lastFrameSeconds;
    m_lastFrameSeconds = seconds;
    const bool waveDue = !m_wavesValid || m_lastWaveSeconds < 0.0 ||
                         seconds - m_lastWaveSeconds + frameStep >= kWaveUpdateSeconds;
    const bool wavesPrepared = waveDue && PrepareWaves(dev);
    BuildReflectionFog();
    CaptureClientState();
    for (DWORD i = 1; i < kMaxRenderTargets; ++i)
        dev->SetRenderTarget(i, nullptr);
    if (!depth.Refresh(dev))
    {
        RestoreTargets(dev);
        return Skip("multisampled depth copy failed");
    }
    dev->SetDepthStencilSurface(nullptr);
    SetPassState(dev);
    RaiseInjectedFault(WaterFaultStage::End);
    if (wavesPrepared && waveDue && SimulateWaves(dev, seconds))
    {
        m_lastSeconds = seconds;
        m_lastWaveSeconds = seconds;
        m_wavesValid = true;
    }
    m_wavesSimulated = m_wavesValid;
    dev->SetRenderTarget(1, nullptr);
    m_ripplesShaded = SimulateRipples(dev, seconds);
    FillRippleConstants(seconds);
    SetPassState(dev);
    CopyLinearDepth(dev, depth.texture, m_waterDepth);
    ShadeClasses(dev, m_saved.colour[0], depth.bound, seconds);
    PushWaterDepth(dev);
    RestoreTargets(dev);
    LogWaveState();
    if (m_shadedClasses == 0)
        return Skip("no water preset for the drawn classes");
    if (!m_loggedFirstShade)
    {
        m_loggedFirstShade = true;
        VF_LOG_INFO("water shaded: %d class%s, waves %s, quality %d", m_shadedClasses,
                    m_shadedClasses == 1 ? "" : "es", m_wavesSimulated ? "simulated" : "flat",
                    QualityIndex(m_cfg) + 1);
    }
    return true;
}
