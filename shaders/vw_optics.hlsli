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
// The lamps near the camera (water_lamps.h: Living Azeroth's light list -- lamps, torches, lit windows, spells -- or the
// client's own point lights) shine on it: each one's reflection, broken up by the waves and ripples like the sun's into
// a streak running toward the eye, and its light on the foam and in the water just under the surface. Their light
// falls off as the client's does (1 / (constant + linear d + quadratic d^2)); on the water it ends at the light's reach
// (the client's lights fade over the last kLampFadeShare of it, Living Azeroth's as its own shaders do). What the water
// mirrors of a lamp is the lamp itself, seen in the water: it reaches further (cLampFalloff.w) and falls off as the
// light would over kLampMirrorShrink of the distance, so a lamp across a canal still streaks the water in front of you
// (lit at the full distance it was a short dim stub by the lamp's foot). The waves the maps don't resolve make a
// lamp's streak long: on wavy water its lobe is never smoother than cLampControl.w (water_renderer.cpp
// kLampRoughness, none on flat water, where a lamp shows as a small sharp spot). A lamp below the surface gives none.
static const float kLampFadeShare = 0.25;
static const float kLampScatter = 0.35;   // of its light scattered back out of the water just under the surface
static const float kLampMirrorShrink = 0.35;

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
        float d2 = dot(toLamp, toLamp);
        float d = sqrt(d2);
        float glintReach = cLampFalloff[i].w;
        [branch] if (d < glintReach && d > 1e-3)
        {
            float3 L = toLamp / d;
            float3 a = cLampFalloff[i].xyz;
            float shape = cLampColour[i].w;
            bool sized = shape > 0;                 // Living Azeroth's lights are a yard across
            float de = sqrt(d2 + (sized ? 1 : 0));
            float3 lamp = cLampColour[i].rgb * saturate(L.z * 4);
            float t = saturate(d / cLampPosition[i].w);
            float window = sized ? pow((1 - t * t) * (1 - t * t), shape) : saturate((1 - t) / kLampFadeShare);
            r.diffuse += lamp * (rcp(max(a.x + a.y * de + a.z * de * de, 1)) * window * saturate(dot(N, L) * 0.5 + 0.5));
            float m = de * kLampMirrorShrink;
            float mirrored = rcp(max(a.x + a.y * m + a.z * m * m, 1)) *
                             saturate((glintReach - d) / (kLampFadeShare * glintReach));
            r.glint += lamp * (mirrored * SunGlint(max(roughness, cLampControl.w), N, V, L));
        }
    }
    r.glint *= cLampControl.z;
    r.diffuse *= cLampControl.y;
    return r;
}

// --- Lightning on the water ----------------------------------------------------------------------------------------
// A lightning strike (Living Azeroth's flash lights) is no lamp: it lights the clouds and the air, so the water flashes
// all over, not in a streak. Within its reach (cFlashPosition.w, smoothly to nothing there) the sky the water mirrors
// brightens in the strike's colour (sky, for the sky's share of the reflection), so does the water's surface (surface,
// as a lamp's light: on the foam and in the water just under it), and the faces of the waves turned toward the strike
// catch it in sharp lines along the crests (glint). Each frame at the strike's own brightness: it fades as it does.
static const float kFlashSky = 1.0;          // of the strike's light the mirrored sky gains
static const float kFlashSurface = 0.5;      // of it falling on the surface
static const float kFlashCrest = 0.35;       // of it on a crest face turned toward it
static const float kFlashCrestStart = 0.06;  // how far a face leans toward the strike (sine of its tilt) to start...
static const float kFlashCrestFull = 0.22;   // ...and to catch it fully

struct FlashLight
{
    float3 sky;
    float3 surface;
    float3 glint;
};

FlashLight FlashesOnWater(WaterPixel w, float3 N)
{
    FlashLight f = (FlashLight)0;
    int count = (int)cFlashControl.x;
    [loop] for (int i = 0; i < kMaxFlashes; ++i)
    {
        [branch] if (i >= count)
            break;
        float3 toStrike = cFlashPosition[i].xyz - w.position;
        float t = saturate(length(toStrike) / cFlashPosition[i].w);
        float3 light = cFlashColour[i].rgb * ((1 - t * t) * (1 - t * t));
        float2 across = normalize(toStrike.xy + float2(1e-4, 0));
        f.sky += light;
        f.surface += light * saturate(dot(N, normalize(toStrike)) * 0.5 + 0.5);
        f.glint += light * smoothstep(kFlashCrestStart, kFlashCrestFull, dot(N.xy, across));
    }
    float strength = cFlashControl.y;
    f.sky *= kFlashSky * strength;
    f.surface *= kFlashSurface * strength;
    f.glint *= kFlashCrest * strength;
    return f;
}

// --- Sun glitter ---------------------------------------------------------------------------------------------------
// The sun's highlight on rough water is many small facets each catching it for a moment, not one smooth patch. The
// water is cut into cells, each holding a facet tilted at random (further with more wind, cGlitter.y) that lasts a
// fraction of a second before the next takes its place; a facet that mirrors the sun to the eye sparkles as a dot in
// its cell, fading in and out. The cells are never smaller than about kGlitterPixels pixels (two sizes, blended by the
// pixel's footprint), so far off the sparkles grow rather than flicker. Strongest on the steeper water and where the
// sun's own highlight is, never on foam, and only while the sun (or the moon) is up.
static const float kGlitterSharpness = 500;   // the facet's lobe, exp((N.H - 1) x this): about 5 degrees across
static const float kGlitterDot = 0.35;        // the dot's radius, of its cell
static const float kGlitterRate = 3;          // facets a second in a cell (each cell between 0.6 and 1.4 times this)
static const float kGlitterPixels = 2.5;      // a cell spans at least this many pixels
static const float kGlitterGain = 40;         // x Fresnel: a sparkle head on is about as bright as the sun's colour

float GlitterCells(float2 position, float cell, float3 N, float3 H)
{
    float2 q = position / cell;
    float2 id = floor(q);
    float3 pace = RainHash(id + 0.37);
    // A whole number of facets per 256 s (the clock's wrap), and the facet's number taken over them, as the rain's
    // rings do: no sparkle changes when the clock starts over.
    float cycles = floor(256 * kGlitterRate * (0.6 + 0.8 * pace.z));
    float life = cGlitter.z * (cycles / 256) + pace.x;
    float n = floor(life);
    n -= cycles * floor(n / cycles);
    float3 facet = RainHash(id + n * float2(19.19, 7.73));
    float3 Nm = normalize(N + float3((facet.xy - 0.5) * (2 * cGlitter.y), 0));
    float lobe = exp((saturate(dot(Nm, H)) - 1) * kGlitterSharpness);
    float2 spot = (frac(q) - lerp(kGlitterDot, 1 - kGlitterDot, facet.zx)) / kGlitterDot;
    float age = frac(life);
    return lobe * saturate(1 - dot(spot, spot)) * age * (1 - age) * 4;
}

float3 SunGlitter(WaterPixel w, float3 N, float3 V, float3 L, float crest, float foam)
{
    float strength = cGlitter.x * SunVisibility() * (1 - foam);
    [branch] if (strength <= 0)
        return 0;
    float footprint = max(length(w.footprintX), length(w.footprintY));
    float level = log2(max(footprint * kGlitterPixels / cGlitter.w, 1));
    float fine = cGlitter.w * exp2(floor(level));
    float3 H = normalize(V + L);
    float sparkle = lerp(GlitterCells(w.position.xy, fine, N, H), GlitterCells(w.position.xy, 2 * fine, N, H),
                         frac(level));
    return SunColour() * (sparkle * strength * saturate(0.3 + 4 * crest) * kGlitterGain * FresnelSchlick(dot(V, H)));
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
