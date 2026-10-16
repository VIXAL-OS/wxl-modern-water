static const float kPi = 3.14159265;
static const float kShallowTileAmplitude = 800;
static const float kMaxTanhArgument = 10;
static const float kAdtGridOrigin = 17066.666;
static const float kAdtTilesPerYard = 0.001875;
static const float kSlopeToAdtUv = 0.001;
static const float kMinFoamFade = 1e-4;
static const float kMinRayRise = 1e-4;
static const float kRippleSlopeScale = 3;
static const float kRippleFullDetailTexels = 2;
static const float kRippleDetailFadeTexels = 2;

struct WaterPixel
{
    float2 uv;
    float waterZ;
    float sceneZ;
    float3 position;
    float3 toCamera;
    float cameraDistance;
    float columnDepth;  // the drop to the bed seen through the pixel (at the player's feet where it is their body)
    float2 seenAt;      // where that bed is (world xy): along the view, not straight below
    float bedGap;       // view depth from the water to that bed
    float2 footprintX;
    float2 footprintY;
};

struct WaveState
{
    float4 moments;
    float2 foam;
};

struct RippleSample
{
    float height;
    float2 slope;
    float coverage;
};

float2 CopyUv(float2 pixel)
{
    return (pixel - ViewportOrigin()) / ViewportSize();
}

float CopiedViewDepth(sampler2D copy, float2 uv)
{
    return dot(tex2Dlod(copy, float4(uv, 0, 0)).rgb, cDepthDecode.rgb);
}

float3 GammaToLinear(float3 colour)
{
    return pow(max(colour, 0), 2.2);
}

float3 LinearToGamma(float3 colour)
{
    return pow(saturate(colour), 1 / 2.2);
}

// The player (and their mount) standing in the water: what the depth behind the water shows there is their body,
// a few tenths of a yard under the surface, and everything placed by the depth took it for the bed. The waves died
// down over it as over a shoal, shore and depth foam lay on it and the coast's surf broke on it, all in the shape of
// the mount ("weird breaks in the waves due to mount silhouette", "mount waves are still jank", the user,
// 2026-10-11). Within kBodyRadius yd of their feet (no more than the reach cPlayer.w gives them) and kBodyHeight yd
// above them, the bed is taken at their feet, which stand on it. The refraction still shows the body.
// Mounted (a reach well past kBodyRadius: water_renderer.cpp gives 9 yd), the body is the whole reach: a drake's wings
// and tail lie well past 3 yd from its feet ("mounted wave calculation still broken", the user, 2026-10-11).
// With the floor around the player known (water_floor.h: fitted to the world's own ground, which leaves out models),
// only what stands more than kBodyAboveFloor over it counts as the body, and the bed under it is that floor. The rule
// above took everything within the reach for the body and the bed at the feet: on a sloping shore the real floor within
// 9 yd, rising above the feet, was pushed down to them, and the swell stopped in a circle around a mounted player ("now
// the drake outline in the waves is just a full circle", the user, 2026-10-12).
static const float kBodyRadius = 3;
static const float kBodyHeight = 4;
static const float kBodyBelowFeet = 0.25;
static const float kBodyAboveFloor = 0.5;

float FittedFloor(float2 xy)
{
    float2 d = xy - cFloorFit2.xy;
    return cFloorFit0.x + dot(cFloorFit0.yz, d) + cFloorFit1.x * d.x * d.x + cFloorFit1.y * d.x * d.y + cFloorFit1.z * d.y * d.y;
}

float BodyRadius()
{
    return cPlayer.w > kBodyRadius + 1 ? cPlayer.w : min(cPlayer.w, kBodyRadius);
}

float BedHeight(float3 seen)
{
    float rise = seen.z - cPlayer.z;
    bool body = cPlayer.w > 0 && length(seen.xy - cPlayer.xy) < BodyRadius() &&
                rise > -kBodyBelowFeet && rise < kBodyHeight;
    [branch] if (body && cFloorFit0.w > 0)
    {
        float floorZ = FittedFloor(seen.xy);
        return seen.z > floorZ + kBodyAboveFloor ? floorZ : seen.z;
    }
    return body ? min(seen.z, cPlayer.z) : seen.z;
}

float2 PlaneFootprint(float3 ray, float3 rayStep, float viewZ)
{
    float rise = ray.z < 0 ? min(ray.z, -kMinRayRise) : max(ray.z, kMinRayRise);
    return viewZ * (rayStep.xy - ray.xy * rayStep.z / rise);
}

WaterPixel ReconstructWaterPixel(float2 pixel)
{
    WaterPixel w;
    w.uv = CopyUv(pixel);
    w.waterZ = CopiedViewDepth(sWaterDepth, w.uv);
    w.sceneZ = max(CopiedViewDepth(sSceneDepth, w.uv), w.waterZ);
    float3 worldRay = ViewToWorldDirection(ViewRayAtUnitDepth(pixel));
    w.position = CameraPositionWorld() + worldRay * w.waterZ;
    w.toCamera = normalize(-worldRay);
    w.cameraDistance = w.waterZ * length(worldRay);
    float3 seen = CameraPositionWorld() + worldRay * w.sceneZ;
    w.columnDepth = max(0, w.position.z - BedHeight(seen));
    w.seenAt = seen.xy;
    float rise = max(-worldRay.z, kMinRayRise);
    w.bedGap = max(w.sceneZ - w.waterZ, w.columnDepth / rise);
    float3 rayStepX = ViewToWorldDirection(ViewRayAtUnitDepth(pixel + float2(1, 0))) - worldRay;
    float3 rayStepY = ViewToWorldDirection(ViewRayAtUnitDepth(pixel + float2(0, 1))) - worldRay;
    w.footprintX = PlaneFootprint(worldRay, rayStepX, w.waterZ);
    w.footprintY = PlaneFootprint(worldRay, rayStepY, w.waterZ);
    return w;
}

float4 SampleTile(sampler2D tile, WaterPixel w, float inverseSize)
{
    return tex2Dgrad(tile, w.position.xy * inverseSize, w.footprintX * inverseSize, w.footprintY * inverseSize);
}

float2 SampleFoamState(sampler2D state, WaterPixel w, float inverseSize)
{
    return tex2Dlod(state, float4(w.position.xy * inverseSize, 0, 0)).xy;
}

float Tanh(float x)
{
    float e = exp(2 * min(x, kMaxTanhArgument));
    return (e - 1) / (e + 1);
}

float ShallowTileWeight(float inverseTileSize, float columnDepth)
{
    float shallowAmplitude = kShallowTileAmplitude * inverseTileSize * inverseTileSize;
    return saturate(lerp(shallowAmplitude, 1, Tanh(4 * kPi * columnDepth * inverseTileSize)));
}

float RippleHeight(float2 uv)
{
    float2 state = tex2Dlod(sRipples, float4(uv, 0, 0)).rg;
    return lerp(state.g, state.r, RippleWeight());
}

float RippleCoverage(WaterPixel w, float2 uv)
{
    float2 fromCentre = abs(uv - 0.5) * RippleExtent();
    float window = saturate((RippleFadeEnd() - max(fromCentre.x, fromCentre.y)) * RippleInverseFadeWidth());
    float footprintTexels = max(length(w.footprintX), length(w.footprintY)) / RippleTexelYards();
    return window * saturate(1 - (footprintTexels - kRippleFullDetailTexels) / kRippleDetailFadeTexels);
}

RippleSample SampleRipples(WaterPixel w)
{
    RippleSample ripples = (RippleSample)0;
    float2 uv = (w.position.xy - RippleOrigin()) * RippleInverseExtent();
    ripples.coverage = RippleGain() > 0 ? RippleCoverage(w, uv) : 0;
    [branch] if (ripples.coverage > 0)
    {
        float step = RippleTexelUv();
        ripples.height = RippleHeight(uv);
        float dx = RippleHeight(uv + float2(step, 0)) - ripples.height;
        float dy = RippleHeight(uv + float2(0, step)) - ripples.height;
        float2 gradient = float2(dx, dy) * rsqrt((1 + dx * dx) * (1 + dy * dy));
        ripples.slope = gradient * kRippleSlopeScale * RippleGain() * ripples.coverage;
    }
    return ripples;
}

// --- Lips: where a river runs smooth and fast over a fall's edge (falls' "hat" models, cLip*) ----------------------
// Upstream of the edge the water speeds up and goes glassy: its waves stretch into streaks along the flow and slide
// toward the edge, the chop and ripples die down, and a thin bright line marks where it curls over.
static const float kLipSideFade = 3;      // yd: the approach fades out past the river's sides over this
static const float kLipWidening = 0.4;    // yd of extra half width per yard upstream (a river narrows to its lip)
static const float kLipConverge = 0.35;   // how far the flow at the sides bends in towards the middle of the lip
static const float kLipBand = 2.5;        // yd above or below the lip's surface that still counts
static const float kLipEdgeYards = 1.5;   // the bright line along the edge
static const float kLipStretch = 0.28;    // at the edge the pattern is 1 / this longer along the flow
static const float kLipSpeed = 3.5;       // yd/s the pattern slides toward the edge, at the edge
static const float kLipCycle = 2;         // s per flow-map cycle (two phases half a cycle apart, cross-faded)
// Laminar water before a drop is glassy, and long still ridges run with the current: they catch the light as
// parallel lines while the surface's finer texture slides along them. Stretching the chop alone read as dark
// brushstrokes at dusk, not flow ("hmm. still no laminar flow afaict?", the user, 2026-10-11, at Glacial Falls' head).
static const float kLipGlass = 0.8;       // the waves' slopes drop by this much at the edge
static const float kLipCalm = 0.9;        // the ripples' and rain's slopes drop by this much at the edge
static const float kLipEdgeFoam = 0.3;    // the white along the edge line
static const float kLipRidgeSlope = 0.14; // the ridges' tilt across the flow, at the edge
static const float kLipRidgeFine = 0.9;   // yd between the fine ridges
static const float kLipRidgeBroad = 2.3;  // and the broad ones
static const float kLipRidgeShare = 0.6;  // of the tilt in the fine ones
static const float kLipRidgeTexels = 4;   // pixels a ridge spacing must span to show (fainter below; none at 2)

struct LipFlow
{
    float weight;   // 0 none .. 1 at the edge
    float2 dir;     // the flow here (world xy, unit)
    float2 centre;  // the lip's centre, which the pattern stretches about
    float edge;     // 0 .. 1 on the edge line itself
};

LipFlow SampleLips(WaterPixel w)
{
    LipFlow lip = (LipFlow)0;
    lip.dir = float2(1, 0);
    int count = (int)cLipControl.x;
    [loop] for (int i = 0; i < kMaxLips; i++)
    {
        [branch] if (i >= count)
            break;
        float4 frame = cLipFrame[i];
        float4 shape = cLipShape[i];
        float2 d = w.position.xy - frame.xy;
        float2 across = float2(-frame.w, frame.z);
        float upstream = -dot(d, frame.zw);
        float lateral = dot(d, across);
        float halfWidth = shape.x + kLipWidening * max(upstream, 0);
        float side = saturate((halfWidth + kLipSideFade - abs(lateral)) / kLipSideFade);
        float level = saturate(1 - (abs(w.position.z - shape.z) - kLipBand) / kLipBand);
        float approach = upstream >= 0 ? pow(saturate(1 - upstream / shape.y), 1.5) : saturate(1 + upstream);
        float weight = shape.w * side * level * approach;
        lip.edge = max(lip.edge, shape.w * side * level * saturate(1 - abs(upstream) / kLipEdgeYards));
        [branch] if (weight > lip.weight)
        {
            lip.weight = weight;
            lip.dir = normalize(frame.zw - across * kLipConverge * clamp(lateral / max(halfWidth, 1), -1, 1));
            lip.centre = frame.xy;
        }
    }
    lip.weight *= cLipControl.y;
    lip.edge *= cLipControl.y;
    return lip;
}

// The tiles' coordinates near a lip: stretched along the flow about the lip's centre and slid `shift` yards back
// upstream (so the pattern moves toward the edge), footprints alike.
WaterPixel FlowPixel(WaterPixel w, LipFlow lip, float shift)
{
    float2 across = float2(-lip.dir.y, lip.dir.x);
    float stretch = lerp(1, kLipStretch, lip.weight);
    float2 d = w.position.xy - lip.centre;
    WaterPixel f = w;
    f.position.xy = lip.centre + lip.dir * (dot(d, lip.dir) * stretch - shift) + across * dot(d, across);
    f.footprintX = lip.dir * (dot(w.footprintX, lip.dir) * stretch) + across * dot(w.footprintX, across);
    f.footprintY = lip.dir * (dot(w.footprintY, lip.dir) * stretch) + across * dot(w.footprintY, across);
    return f;
}

// The still ridges along the flow near a lip, as a slope across it (world xy). They meander slowly with the distance
// along the flow; each spacing fades out where a pixel covers more than 1 / kLipRidgeTexels of it.
float2 LipRidges(WaterPixel w, LipFlow lip)
{
    float2 across = float2(-lip.dir.y, lip.dir.x);
    float2 d = w.position.xy - lip.centre;
    float lateral = dot(d, across), along = dot(d, lip.dir);
    float meander = 0.35 * sin(along * 0.11 + lateral * 0.05) + 0.2 * sin(along * 0.23 + 1.7);
    float footprint = abs(dot(w.footprintX, across)) + abs(dot(w.footprintY, across));
    float fine = saturate(kLipRidgeFine / max(footprint * kLipRidgeTexels, 1e-4) * 2 - 1);
    float broad = saturate(kLipRidgeBroad / max(footprint * kLipRidgeTexels, 1e-4) * 2 - 1);
    float tilt = kLipRidgeShare * fine * sin(2 * kPi * (lateral / kLipRidgeFine + meander)) +
                 (1 - kLipRidgeShare) * broad * sin(2 * kPi * (lateral / kLipRidgeBroad + 0.5 * meander + 0.37));
    return across * (kLipRidgeSlope * lip.weight * tilt);
}

// --- Rivers: the current from every map's own water surfaces (water_flow.h; sRiverFlow, cRiverFlow*) ---------------
// The waves and their foam slide downstream at the river's speed (a flow map: two phases half a cycle apart, each
// starting over while it is faded out), smeared a little along the current, more the faster it runs, so the pattern
// reads as moving water with streaks. The phase drifts slowly across the water so the cross-fade doesn't pulse in step.
// Lips keep their own look where they are stronger.
static const float kRiverMaxSpeed = 4;         // yd/s, as stored (kRiverFlowMaxSpeed)
static const float kRiverMinSpeed = 0.15;      // slower is still water
static const float kRiverCycle = 1.5;          // s per flow-map cycle
static const float kRiverSmear = 0.35;         // yd of smear along the current per yd/s, both ways
static const float kRiverPhaseScatter = 0.3;   // cycles the phase drifts across the water

float2 RiverFlow(float2 position)
{
    float2 uv = position.yx * cRiverFlowMap.x + cRiverFlowMap.yz;
    float inside = step(abs(uv.x - 0.5), 0.5) * step(abs(uv.y - 0.5), 0.5);
    float2 encoded = tex2Dlod(sRiverFlow, float4(uv, 0, 0)).rg;
    return (encoded * 2 - 1) * kRiverMaxSpeed * inside * cRiverFlowMap.w;
}

// The tiles' coordinates slid `along` yards back up the current (so the pattern moves down it).
WaterPixel RiverPixel(WaterPixel w, float2 dir, float along)
{
    WaterPixel f = w;
    f.position.xy -= dir * along;
    return f;
}

WaveState SampleTiles(WaterPixel w, float4 weight)
{
    float4 inverseSize = cInverseTileSizes;
    WaveState waves = (WaveState)0;
    [branch] if (weight.x > 0)
    {
        waves.moments += weight.x * SampleTile(sSurface0, w, inverseSize.x);
        waves.foam += weight.x * SampleFoamState(sFoamState0, w, inverseSize.x);
    }
    [branch] if (weight.y > 0)
    {
        waves.moments += weight.y * SampleTile(sSurface1, w, inverseSize.y);
        waves.foam += weight.y * SampleFoamState(sFoamState1, w, inverseSize.y);
    }
    [branch] if (weight.z > 0)
    {
        waves.moments += weight.z * SampleTile(sSurface2, w, inverseSize.z);
        waves.foam += weight.z * SampleFoamState(sFoamState2, w, inverseSize.z);
    }
    [branch] if (weight.w > 0)
    {
        waves.moments += weight.w * SampleTile(sSurface3, w, inverseSize.w);
        waves.foam += weight.w * SampleFoamState(sFoamState3, w, inverseSize.w);
    }
    return waves;
}

WaveState SampleRiver(WaterPixel w, float4 weight, float2 flow)
{
    float speed = length(flow);
    float2 dir = flow / speed;
    float scatter = kRiverPhaseScatter * (sin(w.position.x * 0.07) + sin(w.position.y * 0.09 + 1.3));
    float phase = frac(cRiverFlowControl.x / kRiverCycle + scatter);
    float travel = speed * kRiverCycle;
    float smear = kRiverSmear * speed;
    float shiftA = phase * travel, shiftB = frac(phase + 0.5) * travel;
    WaveState a1 = SampleTiles(RiverPixel(w, dir, shiftA - smear), weight);
    WaveState a2 = SampleTiles(RiverPixel(w, dir, shiftA + smear), weight);
    WaveState b1 = SampleTiles(RiverPixel(w, dir, shiftB - smear), weight);
    WaveState b2 = SampleTiles(RiverPixel(w, dir, shiftB + smear), weight);
    float wa = 1 - abs(2 * phase - 1);
    WaveState waves;
    waves.moments = 0.5 * ((a1.moments + a2.moments) * wa + (b1.moments + b2.moments) * (1 - wa));
    waves.foam = 0.5 * ((a1.foam + a2.foam) * wa + (b1.foam + b2.foam) * (1 - wa));
    return waves;
}

WaveState SampleWaves(WaterPixel w, float2 rippleSlope, LipFlow lip, float2 riverFlow)
{
    float4 inverseSize = cInverseTileSizes;
    float4 weight = float4(ShallowTileWeight(inverseSize.x, w.columnDepth),
                           ShallowTileWeight(inverseSize.y, w.columnDepth),
                           ShallowTileWeight(inverseSize.z, w.columnDepth),
                           ShallowTileWeight(inverseSize.w, w.columnDepth));
    WaveState waves;
    [branch] if (lip.weight > 0.01)
    {
        // A flow map: two phases half a cycle apart, each sliding the pattern toward the edge and starting over,
        // weighted so each is invisible as it jumps back.
        float phase = frac(cLipControl.z / kLipCycle);
        float travel = kLipSpeed * kLipCycle * lip.weight;
        WaveState a = SampleTiles(FlowPixel(w, lip, phase * travel), weight);
        WaveState b = SampleTiles(FlowPixel(w, lip, frac(phase + 0.5) * travel), weight);
        float wa = 1 - abs(2 * phase - 1);
        waves.moments = a.moments * wa + b.moments * (1 - wa);
        waves.foam = a.foam * wa + b.foam * (1 - wa);
        // Back to world slopes (the pattern was stretched along the flow, so its slopes there shrink), then the
        // glassy water's gentler slopes; the squared-slope moments by the square of the same.
        float2 across = float2(-lip.dir.y, lip.dir.x);
        float stretch = lerp(1, kLipStretch, lip.weight);
        float glass = 1 - kLipGlass * lip.weight;
        waves.moments.xy = (lip.dir * (dot(waves.moments.xy, lip.dir) * stretch) +
                            across * dot(waves.moments.xy, across)) * glass;
        float squared = glass * glass * 0.5 * (1 + stretch * stretch);
        waves.moments.zw *= squared;
        waves.foam *= 1 - 0.8 * lip.weight;
        waves.moments.xy += LipRidges(w, lip);
    }
    else
    {
        [branch] if (dot(riverFlow, riverFlow) > kRiverMinSpeed * kRiverMinSpeed)
            waves = SampleRiver(w, weight, riverFlow);
        else
            waves = SampleTiles(w, weight);
    }
    waves.moments.xy += rippleSlope;
    return waves;
}

float SlopeVariance(float4 moments)
{
    float2 meanSlope = moments.xy * InverseTileCount();
    return 0.5 * max(0, moments.z * InverseTileCount() - dot(meanSlope, meanSlope));
}

float CrestTilt(float4 moments)
{
    float coarseSlope = moments.w * InverseTileCount();
    return sqrt(coarseSlope / (coarseSlope + 1));
}

float2 AdtUv(float3 position, float2 slope)
{
    return (kAdtGridOrigin - position.yx) * kAdtTilesPerYard + slope * kSlopeToAdtUv;
}

float4 TintedFoam(int slot, float maskCoverage)
{
    float coverage = maskCoverage * MaskPresent(slot);
    return float4(lerp(MaskTintLow(slot), MaskTintHigh(slot), coverage), coverage);
}

float4 FoamLayer(sampler2D mask, int slot, float2 uv, float2 dx, float2 dy)
{
    return TintedFoam(slot, tex2Dgrad(mask, uv, dx, dy).r);
}

float FoamFade(float distance, float fadeDistance)
{
    float t = saturate(1 - distance / max(fadeDistance, kMinFoamFade));
    float eased = t * t * (3 - 2 * t);
    eased *= eased;
    return eased * eased;
}

float4 WaveFoam(float2 adtUv, float2 dx, float2 dy, float f)
{
    float3 scale = cWaveFoamScaling.xyz;
    float4 high = TintedFoam(kHighFoamSlot, tex2Dgrad(sWaveFoamMasks, adtUv * scale.x + cFoamScroll.xy, dx * scale.x,
                                                      dy * scale.x).r);
    float4 mid = TintedFoam(kMidFoamSlot, tex2Dgrad(sWaveFoamMasks, adtUv * scale.y + cFoamScroll.xy, dx * scale.y,
                                                    dy * scale.y).g);
    float4 low = TintedFoam(kLowFoamSlot, tex2Dgrad(sWaveFoamMasks, adtUv * scale.z + cFoamScroll.xy, dx * scale.z,
                                                    dy * scale.z).b);
    float rootF = sqrt(f);
    return saturate(low * rootF + mid * f * rootF + high * pow(f, 4.5)) * WaveFoamIntensity();
}

// How much white water a waterfall base puts here: full near the base segment, fading out to the fall's radius,
// and only at the fall's own height (stacked sewer levels must not foam each other).
float WaterfallFoamAmount(WaterPixel w)
{
    float amount = 0;
    int count = (int)cFallFoamControl.x;
    [loop] for (int i = 0; i < kMaxFallFoam; i++)
    {
        [branch] if (i >= count)
            break;
        float4 segment = cFallFoamSegments[i];
        float4 shape = cFallFoamShape[i];
        float2 along = segment.zw - segment.xy;
        float t = saturate(dot(w.position.xy - segment.xy, along) / max(dot(along, along), 1e-4));
        float d = length(w.position.xy - segment.xy - along * t) / shape.x;
        float level = saturate(1 - (abs(w.position.z - shape.z) - shape.w) / shape.w);
        amount = max(amount, shape.y * (1 - smoothstep(0.3, 1.0, d)) * level);
    }
    return amount * cFallFoamControl.y;
}

// The foam masks, scrolled two ways over time so the water boils; plain white where the preset has no masks. At a
// fall's base the masks decide where the white is, not only how bright: patches near the base line, ragged edges
// further out. With a floor of 35% wherever a base foamed at all, a wide fall drew a solid white stripe across its
// pool (Glacial Falls, the user 2026-10-11: "this white stripe ... like the game thinks the base is there"). A lip's
// edge line (`edgeLine`, a thin and fainter white) keeps the old floor, or the patches would erase it.
float4 WaterfallFoam(float2 adtUv, float2 dx, float2 dy, float amount, float edgeLine)
{
    float3 scale = cWaveFoamScaling.xyz;
    float2 boil = cFallFoamControl.z * float2(0.31, -0.23);
    float mid = tex2Dgrad(sWaveFoamMasks, adtUv * scale.y + boil, dx * scale.y, dy * scale.y).g;
    float low = tex2Dgrad(sWaveFoamMasks, adtUv * scale.z - boil * 0.7, dx * scale.z, dy * scale.z).b;
    float pattern = max(mid, low);
    float coverage = MaskPresent(kMidFoamSlot) > 0
                         ? max(saturate(amount * 1.3 - (1 - pattern) * 0.9), saturate(edgeLine * (0.35 + 0.65 * pattern)))
                         : saturate(max(amount, edgeLine) * 0.8);
    float3 albedo = MaskPresent(kMidFoamSlot) > 0 ? lerp(MaskTintLow(kMidFoamSlot), MaskTintHigh(kMidFoamSlot), coverage)
                                                  : float3(0.85, 0.88, 0.9);
    return float4(albedo, coverage);
}

// Flow lines on a lip's approach: the mid foam mask stretched kLipStreakAlong / kLipStreakAcross = 8x along the flow
// and sliding toward the edge, thresholded into thin pale streaks that strengthen with the approach weight. Under a
// fall's hat they show through its sheet, as the base ripples show under the foam (the user, 2026-10-11: "laminar
// flow/linearish wake effects underneath the hat ... like how base ripples are underneath their foam"; the stretched
// waves alone were too faint to read). 256 s (cLipControl.z wraps) x the speed x kLipStreakAlong is a whole number of
// repeats, so the clock's wrap doesn't jump them.
static const float kLipStreakAcross = 0.2;     // repeats per yd across the flow
static const float kLipStreakAlong = 0.025;    // and along it
static const float kLipStreakSpeed = 3.75;     // yd/s toward the edge
static const float kLipStreakFoam = 0.5;       // white on a line at the edge

float LipStreaks(WaterPixel w, LipFlow lip)
{
    float2 across = float2(-lip.dir.y, lip.dir.x);
    float2 d = w.position.xy - lip.centre;
    float2 uv = float2(dot(d, across) * kLipStreakAcross,
                       (dot(d, lip.dir) - cLipControl.z * kLipStreakSpeed) * kLipStreakAlong);
    float2 gx = float2(dot(w.footprintX, across) * kLipStreakAcross, dot(w.footprintX, lip.dir) * kLipStreakAlong);
    float2 gy = float2(dot(w.footprintY, across) * kLipStreakAcross, dot(w.footprintY, lip.dir) * kLipStreakAlong);
    float pattern = MaskPresent(kMidFoamSlot) > 0 ? tex2Dgrad(sWaveFoamMasks, uv, gx, gy).g
                                                  : 0.5 + 0.5 * sin(uv.x * 18.849556);
    return saturate((pattern - 0.45) * 2.5) * lip.weight * kLipStreakFoam;
}

// --- The water straight below a point of the surface ----------------------------------------------------------------
// columnDepth is the drop to the bed seen through the pixel, and that bed lies further along the view than the point
// above it (by the depth times the view's slope): whatever is placed by it slides over the bed with the camera, as if
// drawn on the bed. The coast's crests did: "for waves currently it looks like they are attached to the underwater
// ground surface; would it be possible to project them on the water surface?" (the user, 2026-10-11). So look at the
// bed again, toward the point that far straight below the surface, and take that bed's height; each round brings the
// guess nearer the bed under the point, and two settle it on a sloping beach. A look that lands on something in front
// of the water there (the shore, the player) or off the screen keeps the last guess, and so does one that would move
// the guess by more than kBedMaxChange of it (at least kBedMinChange yd): a real correction is a fraction of the
// depth, but a look landing on something standing in the water (a mount's body under the surface) read it as the bed,
// a few tenths of a yard down, and broke the crests around it ("weird breaks in the waves due to mount silhouette").
static const int kBedRounds = 2;
static const float kBedMinDepth = 0.05;    // yd: the shallowest guess looked toward
static const float kBedMinViewZ = 1e-3;
static const float kBedMaxChange = 0.5;
static const float kBedMinChange = 0.75;   // yd
// A look that lands on something more than this far above the water found no bed: a flyer's drake, a pier or a boat's
// deck in front of it. Taken for the bed at the surface, the coast's surf and swell broke in its shape a little way
// past it on the screen ("there's both a regular shadow and a weird projection of the drake in the waves", the user,
// 2026-10-12). The shore itself, just above the waterline, still counts.
static const float kBedAboveSurface = 0.25; // yd
float3 WorldToViewVector(float3 v)
{
    return float3(dot(v, cViewToWorld[0].xyz), dot(v, cViewToWorld[1].xyz), dot(v, cViewToWorld[2].xyz));
}

// Near the player the depth comes from the floor fitted to the world's own ground around them (water_floor.h, the wide
// fit), for every pixel alike: the guess taken along the view at `guessAt` (the real bed there) moved to this spot by
// the fit's change between the two, a few yd apart, which holds where the fit's height alone wouldn't; where the bed
// seen along the view lies past the fit, the fit's own height here (blended over kFitBlendYards). The looks under
// the water (below) landed on anything between the camera and the bed -- a drake hovering over it -- and those pixels
// kept another depth than their neighbours, so the coast's swell traced the drake's silhouette just past it on the
// screen ("a weird projection of the drake in the waves", "none of the sliders seem to fix the drake-shaped patch", the
// user, 2026-10-12). Over the fit's last kFitBlendYards the looks take over again.
static const float kFitBlendYards = 4;

float FittedFloorWide(float2 xy)
{
    float2 d = xy - cFloorWide2.xy;
    return cFloorWide0.x + dot(cFloorWide0.yz, d) + cFloorWide1.x * d.x * d.x + cFloorWide1.y * d.x * d.y + cFloorWide1.z * d.y * d.y;
}

float BedDepthBelow(float3 surface, float guess, float2 guessAt)
{
    float near = 0, guessNear = 0;
    [branch] if (cFloorWide0.w > 0)
    {
        float2 off = abs(surface.xy - cFloorWide2.xy), offGuess = abs(guessAt - cFloorWide2.xy);
        near = saturate((cFloorWide2.z - max(off.x, off.y)) / kFitBlendYards);
        guessNear = saturate((cFloorWide2.z - max(offGuess.x, offGuess.y)) / kFitBlendYards);
    }
    float fittedHere = FittedFloorWide(surface.xy);
    float moved = guess + FittedFloorWide(guessAt) - fittedHere;
    float fitted = max(lerp(surface.z - fittedHere, moved, guessNear), 0);
    [branch] if (near >= 1)
        return fitted;
    float depth = guess;
    [unroll] for (int round = 0; round < kBedRounds; ++round)
    {
        float3 view = WorldToViewVector(float3(surface.xy, surface.z - max(depth, kBedMinDepth)) - CameraPositionWorld());
        float2 pixel = NdcToPixel(view.xy / max(view.z, kBedMinViewZ) * ViewToNdcScale() + ViewToNdcOffset());
        float2 uv = CopyUv(pixel);
        float sceneZ = CopiedViewDepth(sSceneDepth, uv);
        float waterZ = CopiedViewDepth(sWaterDepth, uv);
        float3 bed = CameraPositionWorld() + ViewToWorldDirection(ViewRayAtUnitDepth(pixel)) * sceneZ;
        float bedZ = BedHeight(bed);
        float found = max(surface.z - bedZ, 0);
        bool seen = view.z > kBedMinViewZ && all(abs(uv - 0.5) <= 0.5) && sceneZ > waterZ &&
                    bed.z < surface.z + kBedAboveSurface && abs(found - depth) <= max(kBedMaxChange * depth, kBedMinChange);
        depth = seen ? found : depth;
    }
    return lerp(depth, fitted, near);
}

// --- Coasts: swells rolling in to a sea's shore and breaking (the ocean pass only, cCoast.x) ------------------------
// The user, 2026-10-11: "how possible would it be to have waves at coastlines specifically?". The distance to the
// shore is the shore foam's estimate (kShoreDistancePerDepth yd per yd of water); crests travel toward it, closer
// together as it shallows (their phase goes with the square root of the distance, as shallow-water waves slow), and a
// large, slow noise in world position bends them and varies their height, so they don't trace the depth contours.
// Before breaking a crest only brightens a little (kCoastCrestSheen); in the last yard of water it breaks into foam.
// 256 s (the clock wraps) / kCoastPeriod is a whole number, so the swell doesn't jump. The depth is the bed's straight
// below the surface (BedDepthBelow), and the swell tilts the surface too (kCoastSwellHeight from trough to crest,
// along the way to the shore): the crests catch the light and bend reflections as waves on the water.
static const float kCoastReach = 28;          // yd from the shore where the swell starts showing
static const float kCoastCrests = 3;          // crests between there and the shore
static const float kCoastPeriod = 6.4;        // s between crests
static const float kCoastNoise = 1.0 / 45;    // the bending noise, repeats per yd
static const float kCoastBreakDepth = 1.1;    // yd of water where a crest breaks
static const float kCoastCrestSheen = 0.16;   // white on an unbroken crest
static const float kCoastSurf = 1.1;          // white on a breaking one
static const float kCoastSwellHeight = 0.3;   // yd from trough to crest
static const float kCoastSlopeStep = 1.5;     // yd: the bed is looked at this far along x and y for the way ashore
static const float kCoastMaxSlope = 0.35;
static const float kCoastLookFurther = 2;     // the along-the-view depth can read this much deeper than the bed below

struct CoastSwell
{
    float surf;     // white on a breaking crest
    float sheen;    // white on an unbroken one
    float2 slope;   // the swell's tilt of the surface (world xy)
};

CoastSwell CoastSurf(WaterPixel w)
{
    CoastSwell c = (CoastSwell)0;
    float perDepth = ShoreDistancePerDepth();
    [branch] if (cCoast.x <= 0 || w.columnDepth * perDepth >= kCoastLookFurther * kCoastReach)
        return c;
    float depth = BedDepthBelow(w.position, w.columnDepth, w.seenAt);
    float dist = depth * perDepth;
    [branch] if (dist >= kCoastReach)
        return c;
    float n = MaskPresent(kLowFoamSlot) > 0 ? tex2Dlod(sWaveFoamMasks, float4(w.position.xy * kCoastNoise, 0, 3)).b : 0.5;
    float phase = kCoastCrests * sqrt(dist / kCoastReach) + 0.4 * n + cCoast.y / kCoastPeriod;
    float wave = saturate(0.5 + 0.5 * cos(2 * kPi * phase));
    float envelope = (0.55 + 0.45 * n) * smoothstep(kCoastReach, 0.6 * kCoastReach, dist) * cCoast.x;
    float swell = envelope * pow(wave, 6);
    float breaking = smoothstep(kCoastBreakDepth, 0.35 * kCoastBreakDepth, depth);
    c.surf = saturate(swell * breaking * kCoastSurf);
    c.sheen = swell * (1 - breaking) * kCoastCrestSheen;
    // The crest's height along the way to the shore, which runs down the bed's slope.
    float2 ashore = float2(BedDepthBelow(w.position + float3(kCoastSlopeStep, 0, 0), depth, w.position.xy) - depth,
                           BedDepthBelow(w.position + float3(0, kCoastSlopeStep, 0), depth, w.position.xy) - depth) *
                    (perDepth / kCoastSlopeStep);
    float perPhase = -6 * pow(wave, 5) * kPi * sin(2 * kPi * phase);            // d crest / d phase
    float phasePerYard = kCoastCrests * 0.5 / sqrt(max(dist, 1) * kCoastReach);  // d phase / d dist
    float2 slope = kCoastSwellHeight * envelope * perPhase * phasePerYard * ashore;
    float tilt = length(slope);
    c.slope = tilt > kCoastMaxSlope ? slope * (kCoastMaxSlope / tilt) : slope;
    return c;
}

float4 FoamAlbedo(WaterPixel w, WaveState waves, LipFlow lip, CoastSwell coast)
{
    float2 adtUv = AdtUv(w.position, waves.moments.xy);
    float2 dx = -w.footprintX.yx * kAdtTilesPerYard;
    float2 dy = -w.footprintY.yx * kAdtTilesPerYard;
    float churn = WaterfallFoamAmount(w);
    float edgeLine = lip.edge * kLipEdgeFoam;
    float4 white = 0;
    [branch] if (max(churn, edgeLine) > 0)
        white = WaterfallFoam(adtUv, dx, dy, churn, edgeLine);
    [branch] if (lip.weight > 0.01)
    {
        float streak = LipStreaks(w, lip);
        float3 tint = MaskPresent(kMidFoamSlot) > 0 ? lerp(MaskTintLow(kMidFoamSlot), MaskTintHigh(kMidFoamSlot), streak)
                                                    : float3(0.85, 0.88, 0.9);
        white = streak > white.a ? float4(tint, streak) : white;
    }
    float f = max(waves.foam.x, 0);
    float shoreWeight = cShoreFoam.x * FoamFade(w.columnDepth * ShoreDistancePerDepth(), cShoreFoam.z);
    float depthWeight = cDepthFadeFoam.x * FoamFade(w.bedGap, cDepthFadeFoam.z);
    float4 foam = 0;
    [branch] if (f * WaveFoamIntensity() > 0)
        foam = WaveFoam(adtUv, dx, dy, f);
    [branch] if (shoreWeight > 0)
        foam += shoreWeight * FoamLayer(sShoreFoamMask, kShoreFoamSlot, adtUv * cShoreFoam.y + cFoamScroll.zw,
                                        dx * cShoreFoam.y, dy * cShoreFoam.y);
    [branch] if (depthWeight > 0)
        foam += depthWeight * FoamLayer(sDepthFoamMask, kDepthFoamSlot, adtUv * cDepthFadeFoam.y + cDepthFoamScroll.xy,
                                        dx * cDepthFadeFoam.y, dy * cDepthFadeFoam.y);
    // The surf: the breaking crest textured by the shore foam's mask, the crest's sheen plain white.
    float2 surf = float2(coast.surf, coast.sheen);
    [branch] if (max(surf.x, surf.y) > 0)
    {
        float4 broken = FoamLayer(sShoreFoamMask, kShoreFoamSlot, adtUv * cShoreFoam.y + cFoamScroll.zw,
                                  dx * cShoreFoam.y, dy * cShoreFoam.y);
        float surfCover = max(surf.x * (0.45 + 0.55 * broken.a), surf.y);
        float3 surfTint = MaskPresent(kShoreFoamSlot) > 0 ? broken.rgb : float3(0.85, 0.88, 0.9);
        foam = surfCover > foam.a ? float4(surfTint, surfCover) : foam;
    }
    foam = saturate(foam);
    return white.a > foam.a ? white : foam;
}
