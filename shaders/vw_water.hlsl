#include "vf_common.hlsli"
#include "vw_constants.hlsli"
#include "vw_surface.hlsli"
#include "vw_rain.hlsli"
#include "vw_optics.hlsli"
#include "vw_reflection.hlsli"

static const float kDebugNormal = 1;
static const float kDebugFoamCoverage = 2;
static const float kDebugTransmittance = 3;
static const float kDebugReflection = 4;
static const float kDebugLiquidClass = 5;
static const float kDebugRipples = 6;
static const float kDebugRefractionOffset = 7;   // the refraction's offset: 0.5 + pixels / 16 (x red, y green)
static const float kDebugRiverFlow = 8;          // the current: 0.5 + flow / 8 yd/s (world x red, y green), speed / 4 blue
static const float kDebugRippleHeightGain = 4;
static const float kDebugRippleWindowShade = 0.25;

float3 ClassColour(float waterClass)
{
    float3 primaries = saturate(1 - abs(waterClass - float3(1, 2, 3)));
    return primaries + saturate(1 - abs(waterClass - 4)) * float3(1, 1, 0);
}

float3 RippleColour(RippleSample ripples)
{
    float height = 0.5 + 0.5 * clamp(ripples.height * kDebugRippleHeightGain, -1, 1);
    return (height * lerp(1 - kDebugRippleWindowShade, 1, ripples.coverage)).xxx;
}

float3 DebugColour(float3 N, float4 foamAlbedo, float3 transmittance, float3 reflected, RippleSample ripples,
                   float2 refractionOffset, float2 riverFlow)
{
    float view = WaterDebugView();
    if (view < kDebugNormal + 0.5)
        return N * 0.5 + 0.5;
    if (view < kDebugFoamCoverage + 0.5)
        return foamAlbedo.aaa;
    if (view < kDebugTransmittance + 0.5)
        return transmittance;
    if (view < kDebugReflection + 0.5)
        return LinearToGamma(reflected);
    if (view < kDebugLiquidClass + 0.5)
        return ClassColour(WaterClassIndex());
    if (view < kDebugRipples + 0.5)
        return RippleColour(ripples);
    if (view < kDebugRefractionOffset + 0.5)
        return float3(saturate(0.5 + refractionOffset / 16), 0);
    return saturate(float3(0.5 + riverFlow / (2 * kRiverMaxSpeed), length(riverFlow) / kRiverMaxSpeed));
}

float4 main(float2 pixelIndex : VPOS) : COLOR0
{
    float2 pixel = pixelIndex + 0.5;
    WaterPixel w = ReconstructWaterPixel(pixel);
    RippleSample ripples = SampleRipples(w);
    float3 rain = SampleRain(w);
    // Before a fall's edge the water runs smooth: the ripples and rain calm down there.
    LipFlow lip = SampleLips(w);
    float calm = 1 - kLipCalm * lip.weight;
    CoastSwell coast = CoastSurf(w);
    float2 riverFlow = RiverFlow(w.position.xy);
    WaveState waves = SampleWaves(w, (ripples.slope + rain.xy) * calm + coast.slope, lip, riverFlow);
    float4 foamAlbedo = FoamAlbedo(w, waves, lip, coast);

    float3 N = normalize(float3(-waves.moments.xy, 1));
    float3 V = w.toCamera;
    float3 L = ToLight();
    float NoV = clamp(dot(N, V), kMinCosine, 1);
    float variance = SlopeVariance(waves.moments);
    float envRoughness = PerceptualRoughness(lerp(EnvironmentRoughnessParameter(), kFoamRoughness, foamAlbedo.a),
                                             variance);
    float sunRoughness = PerceptualRoughness(lerp(SunRoughnessParameter(), kFoamRoughness, foamAlbedo.a), variance);

    Refraction refraction = RefractScene(w, waves.moments.xy);
    float3 transmittance = Transmittance(refraction.pathDepth);
    float3 inscatter = InScattering(V, NoV, CrestTilt(waves.moments), waves.foam.y, transmittance);
    float3 water = transmittance * refraction.scene + inscatter;
    float3 foam = FoamLight(N, waves.foam.x) * foamAlbedo.rgb + inscatter;
    float3 colour = lerp(water, foam, foamAlbedo.a);

    float3 R = ReflectedDirection(V, N);
    float2 hitUv;
    float4 screenReflection = TraceScreenReflection(w, pixel, R, hitUv);
    float3 reflected = FoggedReflection(w, R, lerp(SkyColour(R.z), screenReflection.rgb, screenReflection.a),
                                        screenReflection.a, hitUv);
    float3 environment = reflected * EnvironmentBrdf(envRoughness, NoV) * ReflectionStrength();
    float3 H = normalize(V + L);
    float environmentAttenuation = 1 - FresnelSchlick(dot(H, V)) * SunVisibility();
    float3 sunSpecular = SunVisibility() * SunColour() * SpecularStrength() * SunGlint(sunRoughness, N, V, L);
    sunSpecular += SpecularStrength() * SunGlitter(w, N, V, L, CrestTilt(waves.moments), foamAlbedo.a);
    colour += sunSpecular + environmentAttenuation * environment;
    LampLight lamps = LampsOnWater(w, N, V, sunRoughness);
    float3 underSurface = (1 - transmittance) * kLampScatter * lerp(TroughScatteringColour(), CrestScatteringColour(), kCrestColourBlend);
    colour += lamps.glint * SpecularStrength() + lamps.diffuse * lerp(underSurface, foamAlbedo.rgb, foamAlbedo.a);

    // Normal-only rings disappear under uniform overcast reflections. A small
    // lit splash crest makes the expanding wave readable in that lighting too.
    // Blend towards the existing foam lighting: bounded, non-emissive, and fogged
    // with the rest of the water. It does not brighten the whole water surface.
    float rainCrest = saturate(rain.z) * 0.16;
    colour = lerp(colour, max(colour, FoamLight(N, 0) * 0.65), rainCrest);

    [branch] if (WaterDebugView() > 0.5)
        return float4(DebugColour(N, foamAlbedo, transmittance, reflected, ripples, refraction.offsetPixels, riverFlow), 1);
    float3 encoded = LinearToGamma(colour);
    [branch] if (StockFogApplies())
        encoded = lerp(StockFogColour(), encoded, StockFogVisibility(w.waterZ));
    encoded = lerp(refraction.copied, encoded, saturate(kShoreFadePerYard * refraction.pathDepth));
    return float4(encoded, 1);
}
