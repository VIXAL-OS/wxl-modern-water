static const float kSkyLowerBandElevation = 0.06;
static const float kSkyUpperBandElevation = 0.15;
static const float kSkyMiddleElevation = 0.35;
static const float kSkyTopElevation = 0.8;

float3 ReflectedDirection(float3 V, float3 N)
{
    float3 r = reflect(-V, N);
    return float3(r.xy, abs(r.z));
}

float3 SkyColour(float elevation)
{
    float3 colour = lerp(cSkyHorizon.rgb, cSkyLowerBand.rgb, smoothstep(0, kSkyLowerBandElevation, elevation));
    colour = lerp(colour, cSkyUpperBand.rgb, smoothstep(kSkyLowerBandElevation, kSkyUpperBandElevation, elevation));
    colour = lerp(colour, cSkyMiddle.rgb, smoothstep(kSkyUpperBandElevation, kSkyMiddleElevation, elevation));
    return lerp(colour, cSkyTop.rgb, smoothstep(kSkyMiddleElevation, kSkyTopElevation, elevation));
}

#if SSR_STEPS > 0
static const int kReflectionSteps = SSR_STEPS;
static const int kReflectionRefinements = 3;
static const float kMinReflectionViewZ = 1e-3;
static const float kMinInverseViewZ = 1e-8;
static const float kMinScreenStep = 1e-6;
static const float kReflectionThicknessYards = 1.5;
static const float kReflectionRelativeThickness = 0.05;
static const float kVerticalFadeStart = 0.9;
static const float kVerticalFadeRate = 10;
static const float kNoReflectionHit = 2;
static const float kPlayerReflectionHeight = 5;   // yd the player's feet may be above the water and still reflect
static const float kMountedReflectionHeight = 1;  // the same, mounted
static const float kMountedReach = 5;             // cPlayer.w past this: mounted (water_renderer.cpp: 9, on foot 3.5)
static const float kPlayerReflectionLift = 1.5;   // yd from the feet to the middle of the player (and mount)

struct ReflectionRay
{
    float2 startPixel;
    float2 vanishingPixel;
    float startInverseZ;
};

float3 WorldToViewDirection(float3 direction)
{
    return float3(dot(direction, cViewToWorld[0].xyz), dot(direction, cViewToWorld[1].xyz),
                  dot(direction, cViewToWorld[2].xyz));
}

float2 ViewDirectionToPixel(float3 viewDirection)
{
    return NdcToPixel(viewDirection.xy / viewDirection.z * ViewToNdcScale() + ViewToNdcOffset());
}

float RayViewZ(ReflectionRay ray, float s)
{
    return 1 / max(ray.startInverseZ * (1 - s), kMinInverseViewZ);
}

float2 RayUv(ReflectionRay ray, float s)
{
    return CopyUv(lerp(ray.startPixel, ray.vanishingPixel, s));
}

float OnScreenExtent(ReflectionRay ray)
{
    float rise = ray.vanishingPixel.y - ray.startPixel.y;
    float top = ViewportOrigin().y;
    float bottom = top + ViewportSize().y;
    float limit = rise < 0 ? (top - ray.startPixel.y) / rise : (bottom - ray.startPixel.y) / max(rise, kMinScreenStep);
    return saturate(limit);
}

static const float kSeeThroughOutside = 1.25;      // x the reach squared: the camera at least this far from their middle
static const float kSceneryDepthFraction = 0.99;   // of the deepest fog distance: nearer is scenery, not the sky
static const float kOverheadYards = 60;            // scenery above the line of sight nearer than this covers the sky...
static const float kOverheadFadeYards = 20;        // ...fading out over this much further

// Whether the scene at the ray's pixel s is the player (within `radius2`'s root of their middle, view space): the
// pixel's view ray at unit depth runs from rayFrom (the ray's start) to rayTo (its vanishing point), linearly.
// Mounted, also a pixel that may lie behind a see-through part of them: wing membranes are drawn without depth, so the
// depth there is the scene behind them while the colour is theirs, and the water took the wing's gold from it ("i feel
// like they look gold bc the drake is gold", the user, 2026-10-13). So a pixel whose line of sight passes within their
// reach (9 yd mounted: a 6 yd one left the outer wing showing, "nope. still not fixed") of their middle, with the scene
// behind that, counts as them too (SeeThroughAt).
// The see-through region: an elliptic cylinder round their middle, the reach (9 yd) across the camera's line to them
// -- the wings' way in a chase view -- and kSeeThroughFore along it, kSeeThroughDown below their middle and
// kSeeThroughUp above (the wings rise in a V, their tips 4-5 yd over the body). A ball of the reach left the wings
// showing with the camera within 10 yd ("STILL THERE AHHHHH", the user, 2026-10-14); a round slab 4 yd each way,
// trimmed to below a camera inside it, cut off exactly the raised outer wings ("OH WAIT WATER REFLECTING DRAKES WINGS
// IS STILL BROKEN AT DISTANCE LOLSOB", 2026-10-15). A camera behind them is outside this one. Closer than that (inside),
// the side it is on is trimmed to just short of it (never below kSeeThroughMinHalfHeight), or, level with their middle,
// the reach narrows.
static const float kSeeThroughFore = 5;
static const float kSeeThroughDown = 3;
static const float kSeeThroughUp = 7;
static const float kSeeThroughMinHalfHeight = 1.5;
// A mount whose middle is this close over the water reflects -- its body like anything else; its wings (no depth: a wing
// and the gap beside it look alike) are still looked past. Looked past whole at any height, the drake was missing from
// the water right under it ("now it also doesn't reflect my drakes' wings when they're *directly* over water lolsob",
// the user, 2026-10-15).
static const float kMountedReflectNear = 8;
// Far over the water, the mount looked past is this much wider than the reach (9 yd -> ~14): a bronze drake's wings
// reach past 9 yd, and their tips, outside the zone, reflected in a lake far below it ("ARGH STILL DRAKE WING PROBLEMS",
// Terokkar, the user, 2026-10-16). Low over the water (near) the reach stays, so the body reflects as it should.
static const float kWideMount = 1.6;

// The player as the trace tests every sample against them; the camera's part is worked out once per pixel.
struct PlayerShape
{
    float3 view;        // their middle, view space
    float radius2;      // the scene within this (squared) of it is their body; -1: nobody to skip
    float3 up;          // world up, in view space
    float lowUp;        // the slab's bottom and top along it (view space: the camera at 0)
    float3 viewAcross;  // their middle across it (horizontal), stretched along the camera's line by kFore (below)
    float highUp;
    float3 along;       // that line's direction (horizontal, unit)
    float across2;      // |viewAcross|^2 less the slab's reach squared; huge: no slab (on foot, nobody)
    float stretch;      // reach / kSeeThroughFore - 1: how much more an offset along that line counts
};

// slabReach2: the mount's reach squared (the slab; on foot or nobody: none); skipRadius2: the body looked past (-1: none).
PlayerShape MakePlayerShape(float3 playerView, float slabReach2, float skipRadius2, bool near)
{
    PlayerShape p;
    p.view = playerView;
    p.radius2 = skipRadius2;
    p.up = float3(cViewToWorld[0].z, cViewToWorld[1].z, cViewToWorld[2].z);
    float viewUp = dot(playerView, p.up);
    float3 flat = playerView - viewUp * p.up;   // their middle from the camera, horizontal: all along the camera's line
    float distance2 = dot(flat, flat);
    p.along = flat * rsqrt(max(distance2, 1e-6));
    // Offsets along that line count reach / fore times: the ellipse becomes a circle of the reach. The camera sits on the
    // line, at distance x stretch + 1 in those units.
    float k = sqrt(max(slabReach2, 0)) / kSeeThroughFore;
    p.stretch = k - 1;
    p.viewAcross = flat * k;
    float across2 = distance2 * k * k;
    float cameraUp = -viewUp; // the camera over their middle
    bool inside = across2 < kSeeThroughOutside * slabReach2 && cameraUp * cameraUp <
                  kSeeThroughOutside * (cameraUp > 0 ? kSeeThroughUp * kSeeThroughUp : kSeeThroughDown * kSeeThroughDown);
    float thinned = abs(cameraUp) * rsqrt(kSeeThroughOutside);
    bool thin = inside && thinned >= kSeeThroughMinHalfHeight;
    float up = thin && cameraUp > 0 ? thinned : kSeeThroughUp, down = thin && cameraUp <= 0 ? thinned : kSeeThroughDown;
    float reach2 = inside && !thin ? across2 / kSeeThroughOutside : slabReach2;
    p.lowUp = viewUp - down;
    p.highUp = viewUp + up;
    p.across2 = slabReach2 > kMountedReach * kMountedReach && (near || skipRadius2 > 0) ? across2 - reach2 : 1e30;
    return p;
}

bool PlayerBodyAt(float3 rayFrom, float3 rayTo, float s, float sceneZ, PlayerShape p)
{
    float3 d = lerp(rayFrom, rayTo, s) * sceneZ - p.view;
    return dot(d, d) < p.radius2;
}

// Whether the line of sight sight * t (t: view depth) meets the slab between the camera and the scene: within the
// slab's height for t in [u0, u1] (clipped to 0..sceneZ), and somewhere in there across within its reach -- the
// across distance squared, a t^2 - 2 b t + across2 over reach^2, is least at b / a, clamped into the span. No roots: a
// level line of sight (rise 0) makes the span all or nothing through the infinities of 1 / rise.
bool SeeThroughAt(float3 sight, float sceneZ, PlayerShape p)
{
    float rise = dot(sight, p.up);
    float3 sightAcross = sight - rise * p.up;
    sightAcross += p.stretch * dot(sightAcross, p.along) * p.along;   // the ellipse as a circle
    float a = dot(sightAcross, sightAcross);
    float b = dot(sightAcross, p.viewAcross);
    float t0 = p.lowUp / rise, t1 = p.highUp / rise;
    float u0 = max(min(t0, t1), 0), u1 = min(max(t0, t1), sceneZ);
    float t = clamp(b / a, u0, u1);
    // ...with the scene beyond it: a surface inside the slab (their body, the ground just under them) is no see-through
    // part, and reflects as it is (low over the water, their body does).
    bool sceneInside = sceneZ > min(t0, t1) && sceneZ < max(t0, t1) && (a * sceneZ - 2 * b) * sceneZ + p.across2 < 0;
    return u0 < u1 && (a * t - 2 * b) * t + p.across2 < 0 && !sceneInside;
}

// Whether the trace looks past the scene at pixel s: their body (far over the water) or a see-through part of them.
bool PlayerAt(float3 rayFrom, float3 rayTo, float s, float sceneZ, PlayerShape p)
{
    float3 sight = lerp(rayFrom, rayTo, s);
    float3 d = sight * sceneZ - p.view;
    return dot(d, d) < p.radius2 || SeeThroughAt(sight, sceneZ, p);
}


// How surely the scenery at a pixel (view ray `sight` at unit depth, at depth sceneZ) hides the sky above the water:
// scenery the line of sight rises to within kOverheadYards -- a canopy, an overhang, a canyon's walls -- covers it, so
// a reflection running off the screen there shows that scenery and not the sky. Under Terokkar's trees the sky came
// through as bright patches on the water ("distant reflections still happening sadly", the user, 2026-10-13). Ground
// seen from above (the line of sight falling) or far scenery leaves the sky.
float OverheadCover(float3 sight, float sceneZ)
{
    float rise = dot(sight, float3(cViewToWorld[0].z, cViewToWorld[1].z, cViewToWorld[2].z));
    return rise > 0 ? saturate((kOverheadYards + kOverheadFadeYards - sceneZ) / kOverheadFadeYards) : 0;
}

float4 TraceScreenReflection(WaterPixel w, float2 pixel, float3 R, out float2 hitUv)
{
    hitUv = 0;
    float3 viewDirection = WorldToViewDirection(R);
    [branch] if (viewDirection.z < kMinReflectionViewZ)
        return 0;
    ReflectionRay ray;
    ray.startPixel = pixel;
    ray.vanishingPixel = ViewDirectionToPixel(viewDirection);
    ray.startInverseZ = 1 / w.waterZ;
    float extent = OnScreenExtent(ray);
    // Not the player, seen from water well below them: the screen holds only their front, and a ray passing just
    // behind them counted as a hit, so a flyer saw themselves in a pond far below ("sometimes my reflection will show
    // in water i'm nowhere near", the user, 2026-10-11). Within kPlayerReflectionHeight of the water they still show.
    // Mounted, from kMountedReflectionHeight: a drake hovering over the water is a thick body seen from above, and the
    // rays passing under it found only the thin parts (the wings) and drew them in pieces off to its side ("distant
    // mount reflections still borked", the user, 2026-10-12).
    // The trace looks through them there. Stopping at them and falling back to the sky left the water in front of a
    // hovering drake showing sky -- in a canyon far brighter than the cliffs it should show ("mount reflections at
    // distance are now bright white", the user, 2026-10-12). A ray passing them goes on to what is behind; one that
    // comes out from behind them already behind the scene takes what lies just beside them.
    // Low over the water (kMountedReflectNear) a mount reflects after all, its wings on their plane (PlayerAt).
    bool mounted = cPlayer.w > kMountedReach;
    float above = cPlayer.z - w.position.z;
    bool near = mounted && above + kPlayerReflectionLift < kMountedReflectNear;
    float reflectFrom = mounted ? kMountedReflectionHeight : kPlayerReflectionHeight;
    float reach = mounted && !near ? cPlayer.w * kWideMount : cPlayer.w;
    float playerRadius2 = cPlayer.w > 0 && above > reflectFrom && !near ? reach * reach : -1;
    float3 playerView = WorldToViewDirection(cPlayer.xyz + float3(0, 0, kPlayerReflectionLift) - CameraPositionWorld());
    PlayerShape shape = MakePlayerShape(playerView, mounted ? reach * reach : -1, playerRadius2, near);
    // Out of their zone, still at their own distance (closer than their middle + their reach + 2 yd): the rest of them.
    // A wide drake's wing tips reach past the zone, and the first step out landed on one: taken as what lies beside
    // them, at any depth, so far water showed a stripe of the wing ("STILL DRAKE WING PROBLEMS", Terokkar, the user,
    // 2026-10-16: the drake nowhere near the water). Only scenery clearly beyond them ends the look past them. Not low
    // over the water (near), where their body reflects.
    float beyondPlayer = near ? -1e30 : playerView.z + max(reach, 3) + 2;
#if PLAYER_MASK
    // The exact player mask (player_mask.cpp: their own batches drawn again into a mask, carried in the depth copy's g):
    // far over the water the pixels that are theirs are known, whatever their wings' span, and the guessed shapes above
    // and the rule after them are left out. Low over the water and on foot their body reflects, and so do a mount's wings
    // where they hold their own depth (wxl-seyris-living-azeroth's PlayerDepth); only their pixels holding scenery well
    // behind them -- a see-through part drawn without depth, its colour over the scenery's depth -- are looked past.
    bool useMask = playerRadius2 > 0;
    float maskBehind = playerView.z + max(reach, 3) + 2;
#endif
    float3 rayFrom = ViewRayAtUnitDepth(ray.startPixel), rayTo = ViewRayAtUnitDepth(ray.vanishingPixel);
    float previous = 0;
    float front = 0;
    float behind = kNoReflectionHit;
    float open = 0;   // the last step that wasn't the player
    bool afterPlayer = false;
    float lastZ = 0;  // the scene's depth at the last step (the screen's edge when nothing is hit)
    [loop] for (int step = 1; step <= kReflectionSteps; ++step)
    {
        [branch] if (behind == kNoReflectionHit)
        {
            float s = extent * step / kReflectionSteps;
#if PLAYER_MASK
            float2 texel = tex2Dlod(sSceneDepth, float4(RayUv(ray, s), 0, 0)).rg;
            float sceneZ = texel.r * cDepthDecode.r;
            bool player = texel.g > 0.5 && (useMask || sceneZ > maskBehind);
#else
            float sceneZ = CopiedViewDepth(sSceneDepth, RayUv(ray, s));
            bool player = PlayerAt(rayFrom, rayTo, s, sceneZ, shape) || (afterPlayer && sceneZ < beyondPlayer);
#endif
            float thickness = max(kReflectionThicknessYards, sceneZ * kReflectionRelativeThickness);
            bool crossed = !player && RayViewZ(ray, s) > sceneZ && (RayViewZ(ray, previous) < sceneZ + thickness || afterPlayer);
            front = crossed ? previous : front;
            behind = crossed ? s : behind;
            previous = s;
            open = player ? open : s;
            afterPlayer = player;
            lastZ = sceneZ;
        }
    }
    [branch] if (behind == kNoReflectionHit)
    {
        // Nothing hit on the screen. Where the trace ran out at scenery -- the ray already behind it there, or rising
        // to it close by (OverheadCover) -- the sky past the screen is hidden by it, and the reflection takes its colour.
        // Where it ran out on the player (or behind a see-through part of them), what lies just beside them: the last
        // scenery the trace saw before them, or, starting among them, what is at the end unless that is their body.
        // (Nothing hit: the march ran every step, the last at the extent, so its test and depth there are these.)
        float endZ = lastZ;
        bool onPlayer = afterPlayer;
#if PLAYER_MASK
        bool bodyAtEnd = useMask;   // far over the water the end is them; low, a see-through part
#else
        bool bodyAtEnd = PlayerBodyAt(rayFrom, rayTo, extent, endZ, shape);
#endif
        float last = !onPlayer ? extent : open > 0 ? open : bodyAtEnd ? 0 : extent;
        [branch] if (!(last > 0))
            return 0;
        float2 edgeUv = RayUv(ray, last);
        float edgeZ = CopiedViewDepth(sSceneDepth, edgeUv);
        [branch] if (!(edgeZ < kSceneryDepthFraction * MaxFogDistance()))
            return 0;
        float cover = onPlayer ? 1 : max(RayViewZ(ray, last) >= edgeZ ? 1 : 0, OverheadCover(lerp(rayFrom, rayTo, last), edgeZ));
        [branch] if (cover <= 0)
            return 0;
        hitUv = edgeUv;
        return float4(GammaToLinear(tex2Dlod(sSceneColour, float4(edgeUv, 0, 0)).rgb), cover);
    }
    [loop] for (int refinement = 0; refinement < kReflectionRefinements; ++refinement)
    {
        float middle = 0.5 * (front + behind);
#if PLAYER_MASK
        float2 middleTexel = tex2Dlod(sSceneDepth, float4(RayUv(ray, middle), 0, 0)).rg;
        float middleZ = middleTexel.r * cDepthDecode.r;
        bool middlePlayer = middleTexel.g > 0.5 && (useMask || middleZ > maskBehind);
#else
        float middleZ = CopiedViewDepth(sSceneDepth, RayUv(ray, middle));
        bool middlePlayer = PlayerAt(rayFrom, rayTo, middle, middleZ, shape);
#endif
        bool middleBehind = RayViewZ(ray, middle) > middleZ && !middlePlayer;
        front = middleBehind ? front : middle;
        behind = middleBehind ? middle : behind;
    }
    float2 uv = RayUv(ray, behind);
    hitUv = uv;
    // Near the screen's top and bottom a hit fades into the sky -- except into scenery that covers it (OverheadCover).
    float fade = saturate(1 - saturate((max(uv.y, 1 - uv.y) - kVerticalFadeStart) * kVerticalFadeRate));
    fade = max(fade, OverheadCover(lerp(rayFrom, rayTo, behind), CopiedViewDepth(sSceneDepth, uv)));
    return float4(GammaToLinear(tex2Dlod(sSceneColour, float4(uv, 0, 0)).rgb), fade);
}
#else
float4 TraceScreenReflection(WaterPixel w, float2 pixel, float3 R, out float2 hitUv)
{
    hitUv = 0;
    return 0;
}
#endif

static const int kReflectionFogLayers = 4;
static const int kReflectionFogLayerRegisters = 5;
static const float kSkyHitDepthFraction = 0.99;
static const float kMinPhaseDistance = 1e-6;
static const float kMinProfileHeightSpan = 1e-3;
static const float kMinExponentSpan = 1e-2;
static const float kMinPathLength = 1e-3;

struct ReflectedPath
{
    float from;
    float to;
    float startHeight;
    float heightPerYard;
    float skyRise;
    float cosToLight;
};

float ReflectionFogCurveIntegral(float4 curve, float distance)
{
    float travelled = max(distance - curve.x, 0);
    float range = ReflectionFogCurveRange();
    float ramp = saturate(travelled / range);
    float exponent = curve.w + 1;
    return travelled + curve.z * (range * pow(ramp, exponent) / exponent + max(travelled - range, 0));
}

float LogHeightProfile(float4 height, float worldHeight)
{
    return min((height.x - worldHeight) * height.y, 0) + min((worldHeight - height.z) * height.w, 0);
}

float MeanExpAlongRamp(float2 fromLogAndExp, float2 toLogAndExp)
{
    float logSpan = toLogAndExp.x - fromLogAndExp.x;
    return abs(logSpan) < kMinExponentSpan ? fromLogAndExp.y * (1 + 0.5 * logSpan)
                                           : (toLogAndExp.y - fromLogAndExp.y) / logSpan;
}

float2 HeightProfileLogAndExp(float4 height, float worldHeight)
{
    float logProfile = LogHeightProfile(height, worldHeight);
    return float2(logProfile, exp(logProfile));
}

float MeanHeightProfile(float4 height, float lowHeight, float highHeight)
{
    float heightSpan = highHeight - lowHeight;
    float firstKink = clamp(min(height.x, height.z), lowHeight, highHeight);
    float secondKink = clamp(max(height.x, height.z), lowHeight, highHeight);
    float2 atLow = HeightProfileLogAndExp(height, lowHeight);
    float2 atFirstKink = HeightProfileLogAndExp(height, firstKink);
    float2 atSecondKink = HeightProfileLogAndExp(height, secondKink);
    float2 atHigh = HeightProfileLogAndExp(height, highHeight);
    float integral = (firstKink - lowHeight) * MeanExpAlongRamp(atLow, atFirstKink) +
                     (secondKink - firstKink) * MeanExpAlongRamp(atFirstKink, atSecondKink) +
                     (highHeight - secondKink) * MeanExpAlongRamp(atSecondKink, atHigh);
    return heightSpan < kMinProfileHeightSpan ? atLow.y : integral / heightSpan;
}

float ReflectionFogPhase(float4 scattering, float cosToLight)
{
    float g = scattering.x;
    float r = (1 - g) / sqrt(max(1 + g * g - 2 * g * cosToLight, kMinPhaseDistance));
    return lerp(r * r * r, 1, scattering.y);
}

float PathHeight(ReflectedPath path, float distance)
{
    return path.startHeight + path.heightPerYard * (distance - path.from);
}

float4 ReflectedPathFog(ReflectedPath path)
{
    float3 inscatter = 0;
    float opticalDepth = 0;
    [unroll] for (int j = 0; j < kReflectionFogLayers; ++j)
    {
        float4 curve = cReflectionFogLayers[j * kReflectionFogLayerRegisters];
        float4 height = cReflectionFogLayers[j * kReflectionFogLayerRegisters + 1];
        float3 emissive = cReflectionFogLayers[j * kReflectionFogLayerRegisters + 2].rgb;
        float3 diffuse = cReflectionFogLayers[j * kReflectionFogLayerRegisters + 3].rgb;
        float4 scattering = cReflectionFogLayers[j * kReflectionFogLayerRegisters + 4];
        float start = max(path.from, curve.x);
        float end = min(path.to, scattering.z);
        [branch] if (curve.y <= 0 || end <= start)
            continue;
        float span = ReflectionFogCurveIntegral(curve, end) - ReflectionFogCurveIntegral(curve, start);
        float startHeight = PathHeight(path, start);
        float endHeight = PathHeight(path, end);
        float profile = MeanHeightProfile(height, min(startHeight, endHeight), max(startHeight, endHeight));
        float layerDepth = curve.y * span * profile * exp(-path.skyRise * scattering.w);
        inscatter += (diffuse * ReflectionFogPhase(scattering, path.cosToLight) + emissive) * layerDepth;
        opticalDepth += layerDepth;
    }
    float transmittance = exp(-opticalDepth);
    return float4(opticalDepth > 0 ? inscatter / opticalDepth * (1 - transmittance) : 0, transmittance);
}

ReflectedPath SkyReflectionPath(WaterPixel w, float3 R)
{
    ReflectedPath path;
    path.from = w.cameraDistance;
    path.to = max(ReflectionFogSkyEnd(), w.cameraDistance);
    path.startHeight = w.position.z;
    path.heightPerYard = max(R.z, 0);
    path.skyRise = path.heightPerYard;
    path.cosToLight = dot(R, ToLight());
    return path;
}

ReflectedPath SurfaceReflectionPath(WaterPixel w, float3 R, float2 hitUv, float hitZ)
{
    float2 hitPixel = ViewportOrigin() + hitUv * ViewportSize();
    float3 hit = CameraPositionWorld() + ViewToWorldDirection(ViewRayAtUnitDepth(hitPixel)) * hitZ;
    float pathLength = distance(hit, w.position);
    ReflectedPath path;
    path.from = w.cameraDistance;
    path.to = w.cameraDistance + pathLength;
    path.startHeight = w.position.z;
    path.heightPerYard = (hit.z - w.position.z) / max(pathLength, kMinPathLength);
    path.skyRise = 0;
    path.cosToLight = dot(R, ToLight());
    return path;
}

float3 FoggedSkyReflection(WaterPixel w, float3 R, float3 sky)
{
    [branch] if (!ReflectionFogActive())
        return sky;
    float4 fog = ReflectedPathFog(SkyReflectionPath(w, R));
    return sky * fog.a + fog.rgb;
}

float3 FoggedScreenReflection(WaterPixel w, float3 R, float3 colour, float2 hitUv)
{
    [branch] if (!ReflectionFogActive())
        return colour;
    float hitZ = CopiedViewDepth(sSceneDepth, hitUv);
    ReflectedPath path = SkyReflectionPath(w, R);
    [flatten] if (hitZ < kSkyHitDepthFraction * MaxFogDistance())
        path = SurfaceReflectionPath(w, R, hitUv, hitZ);
    float4 fog = ReflectedPathFog(path);
    return colour * fog.a + fog.rgb;
}
