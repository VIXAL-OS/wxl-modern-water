static const float kWaterF0 = 0.02;
static const float kInverseFourPi = 0.0795775;
static const float kFoamRoughness = 0.8;
static const float kRoughnessExponent = 0.25;
static const float kMinRoughnessMoment = 1e-8;
static const float kRefractionUvPerSlopeYard = 0.005;
static const float kMaxRefractionDepth = 50;
static const float kShoreFadePerYard = 2;
static const float kFoamStateGain = 10;
static const float kFoamWrap = 0.25;
static const float kFoamCrestSharpness = 8;
static const float kCrestColourBlend = 0.5;
static const float kMinCosine = 1e-5;
static const float kMinPhaseDenominator = 1e-6;
static const float kMinSunAlpha = 1e-3;
static const float kSunDiscAlpha = 0.02;
static const float kMinVisibilityDenominator = 1e-5;

struct Refraction
{
    float3 copied;
    float3 scene;
    float pathDepth;
    float2 offsetPixels; // how far the refraction moved this pixel's look (debug view 7)
};

// The refraction's offset grew with the depth behind the water (up to kMaxRefractionDepth yd) but not with the
// distance, so far off, where the ripples' tilt turns over within a pixel or two, neighbouring pixels looked many
// pixels apart: Stormwind's canal walls seen through the water shimmered in moire ("moire pattern on the stones ...
// the wall seen through the water", the user, 2026-10-11). A surface tilted by s bends the view by about
// kRefractionBend x s, so what is seen d yd behind it moves that times d yd: no more than that many pixels at this
// pixel's footprint (the old offset up to about 25 yd away, less beyond). And where a pixel covers more water than
// kRefractionClearFootprint yd (its longer way), the slopes it averages turn over within a pixel or two: the offset
// fades as the square of that ratio, smoothly from pixel to pixel. (Limiting it by its own screen derivatives worked
// per 2x2 quad and left steps between quads.)
static const float kRefractionBend = 0.25;
static const float kRefractionClearFootprint = 0.05;

float FresnelSchlick(float cosine)
{
    return kWaterF0 + (1 - kWaterF0) * pow(1 - saturate(cosine), 5);
}

float PerceptualRoughness(float parameter, float variance)
{
    return saturate(pow(max(parameter * abs(parameter) + variance, kMinRoughnessMoment), kRoughnessExponent));
}

float EnvironmentBrdf(float roughness, float NoV)
{
    float4 r = roughness * float4(-1, -0.0275, -0.572, 0.022) + float4(1, 0.0425, 1.04, -0.04);
    float a004 = min(r.x * r.x, exp2(-9.28 * NoV)) * r.x + r.y;
    float2 ab = float2(-1.04, 1.04) * a004 + r.zw;
    return kWaterF0 * ab.x + ab.y;
}

Refraction RefractScene(WaterPixel w, float2 slope)
{
    float2 offset = slope * kRefractionUvPerSlopeYard * min(kMaxRefractionDepth, w.sceneZ - w.waterZ);
    // yd per pixel on the water, the longer way (seen at a glance, a pixel stretches far along the view)
    float footprint = max(length(w.footprintX), length(w.footprintY));
    offset *= saturate(kRefractionBend / max(footprint * kRefractionUvPerSlopeYard * ViewportSize().x, 1e-4));
    float clear = saturate(kRefractionClearFootprint / max(footprint, 1e-4));
    offset *= clear * clear;
    float2 refractedUv = w.uv + offset;
    float refractedZ = CopiedViewDepth(sSceneDepth, refractedUv);
    bool unrefracted = refractedZ < w.waterZ;
    float2 colourUv = unrefracted ? w.uv : refractedUv;
    float colourZ = unrefracted ? w.sceneZ : refractedZ;
    Refraction r;
    r.copied = tex2Dlod(sSceneColour, float4(colourUv, 0, 0)).rgb;
    r.scene = GammaToLinear(WithoutStockFog(r.copied, colourZ));
    r.pathDepth = max(0, max(refractedZ, w.sceneZ) - w.waterZ);
    r.offsetPixels = offset * ViewportSize();
    return r;
}

float3 Transmittance(float pathDepth)
{
    return exp(-AbsorptionPerYard() * pathDepth);
}

float3 InScattering(float3 V, float NoV, float crestTilt, float oxygen, float3 transmittance)
{
    float g = ScatteringAnisotropy();
    float cosTheta = -dot(V, ToLight());
    float phase = (1 - g * g) / pow(max(1 + g * g - 2 * g * cosTheta, kMinPhaseDenominator), 1.5);
    float4 intensity = cScatteringIntensities;
    float viewPart = NoV * NoV * intensity.x + intensity.y * crestTilt;
    float sunPart = SunTransmission() * intensity.z;
    float oxygenPart = saturate(ToLight().z) * oxygen * intensity.w;
    float normaliser = 1 / max(1, sunPart + viewPart + oxygenPart);
    float3 colour = lerp(TroughScatteringColour(), CrestScatteringColour(), kCrestColourBlend);
    float3 anisotropic = phase * SunVisibility() * SunColour() * viewPart * normaliser;
    float3 isotropic = (oxygenPart * normaliser + sunPart) * IsotropicLight();
    return (1 - transmittance) * kInverseFourPi * (isotropic + anisotropic) * colour;
}

float WrappedFoamDiffuse(float NoL, float3 L)
{
    return saturate(NoL + kFoamWrap * saturate(L.z));
}

float3 FoamLight(float3 N, float foamState)
{
    float3 L = ToLight();
    float NoL = clamp(dot(N, L), kMinCosine, 1);
    float state = saturate(foamState * kFoamStateGain);
    float crest = saturate(state * state * (3 - 2 * state) * L.z);
    float diffuse = lerp(WrappedFoamDiffuse(NoL, L), pow(NoL, kFoamCrestSharpness), crest) * SunVisibility();
    return lerp(Ambient(), SunColour(), diffuse);
}

float SunGlint(float roughness, float3 N, float3 V, float3 L);

// --- Lamps on the water --------------------------------------------------------------------------------------------
// At night the water had only the sun, the moon and the sky: in Stormwind the lamp-lit walls glowed beside canals gone
// nearly black ("am i tripping or are the sw canals super dark now? maybe just bc it's night?", the user, 2026-10-11).
// The client's point lights near the camera (the ones the fog takes) now shine on it too: each one's glint, broken up
// by the ripples like the sun's, and its light on the foam and in the water just under the surface. Their light falls
// off as the client's does (1 / (constant + linear d + quadratic d^2)), fading out over the last kLampFadeShare of its
// cutoff; a lamp below the surface gives none.
static const float kLampFadeShare = 0.25;
static const float kLampScatter = 0.35;   // of its light scattered back out of the water just under the surface

struct LampLight
{
    float3 glint;    // reflected off the surface
    float3 diffuse;  // falling on it (wrapped, for foam and the water under it)
};

LampLight LampsOnWater(WaterPixel w, float3 N, float3 V, float roughness)
{
    LampLight r = (LampLight)0;
    int count = (int)cLampControl.x;
    [loop] for (int i = 0; i < kMaxLamps; ++i)
    {
        [branch] if (i >= count)
            break;
        float3 toLamp = cLampPosition[i].xyz - w.position;
        float d = length(toLamp);
        float cutoff = cLampPosition[i].w;
        [branch] if (d < cutoff && d > 1e-3)
        {
            float3 L = toLamp / d;
            float3 a = cLampFalloff[i].xyz;
            float falloff = rcp(max(a.x + a.y * d + a.z * d * d, 1)) *
                            saturate((cutoff - d) / (kLampFadeShare * cutoff)) * saturate(L.z * 4);
            float3 colour = cLampColour[i].rgb * falloff;
            r.glint += colour * SunGlint(roughness, N, V, L);
            r.diffuse += colour * saturate(dot(N, L) * 0.5 + 0.5);
        }
    }
    r.glint *= cLampControl.y;
    r.diffuse *= cLampControl.y;
    return r;
}

float SunGlint(float roughness, float3 N, float3 V, float3 L)
{
    float3 H = normalize(V + L);
    float NoH = saturate(dot(N, H));
    float NoL = saturate(dot(N, L));
    float NoV = max(dot(N, V), kMinCosine);
    float alpha = max(roughness * roughness, kMinSunAlpha);
    float widened = saturate(alpha + kSunDiscAlpha);
    float widened2 = widened * widened;
    float lobe = NoH * NoH * (widened2 - 1) + 1;
    float energy = alpha / widened;
    float distribution = widened2 / (kPi * lobe * lobe) * energy * energy;
    float visibility = 0.5 / max(NoL * (NoV * (1 - alpha) + alpha) + NoV * (NoL * (1 - alpha) + alpha),
                                 kMinVisibilityDenominator);
    return distribution * visibility * FresnelSchlick(dot(V, H)) * NoL;
}
