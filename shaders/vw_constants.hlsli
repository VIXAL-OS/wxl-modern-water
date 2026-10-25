float4 cLight : register(c9);
float4 cSunColour : register(c10);
float4 cAmbient : register(c11);
float4 cIsotropicLight : register(c12);
float4 cSkyTop : register(c13);
float4 cSkyMiddle : register(c14);
float4 cSkyUpperBand : register(c15);
float4 cSkyLowerBand : register(c16);
float4 cSkyHorizon : register(c17);
float4 cStockFogColour : register(c18);
float4 cStockFog : register(c19);
float4 cAbsorption : register(c20);
float4 cScatteringIntensities : register(c21);
float4 cScatteringTop : register(c22);
float4 cScatteringBottom : register(c23);
float4 cDepthFadeFoam : register(c24);
float4 cShoreFoam : register(c25);
float4 cWaveFoam : register(c26);
float4 cWaveFoamScaling : register(c27);
float4 cSurfaceResponse : register(c28);
float4 cInverseTileSizes : register(c29);
float4 cWaveControl : register(c30);
float4 cFoamScroll : register(c31);
float4 cDepthFoamScroll : register(c32);
float4 cDepthDecode : register(c33);
float4 cMaskTints[10] : register(c34);
float4 cReflectionFogLayers[20] : register(c44);
float4 cReflectionFogRange : register(c64);
float4 cRippleWindow : register(c65);
float4 cRippleShape : register(c66);
float4 cRippleFade : register(c67);
float4 cRainControl : register(c68); // strength x observed outdoor rain; time modulo 256 seconds
// Waterfall white water: count, strength, time (seconds modulo 256); per fall the base segment (a.xy, b.xy) and
// its shape (radius yd, strength, base height, height band yd).
static const int kMaxFallFoam = 16;
float4 cFallFoamControl : register(c69);
float4 cFallFoamSegments[kMaxFallFoam] : register(c70);
float4 cFallFoamShape[kMaxFallFoam] : register(c86);
static const int kMaxLips = 8;
float4 cLipControl : register(c102);          // count, strength, seconds mod 256
float4 cLipFrame[kMaxLips] : register(c103);  // lip centre xy, flow direction zw
float4 cLipShape[kMaxLips] : register(c111);  // half width at the lip, reach upstream, surface height, strength
float4 cCoast : register(c119);               // surf strength (the ocean pass only), seconds mod 256
float4 cPlayer : register(c120);              // the player's feet (world), reach of their reflection (0: none)
// The lamps near the camera that matter on the water (water_lamps.h: Living Azeroth's light list, else the client's
// own point lights): count, the strength of their light on the water, the strength of their reflections, the smallest
// roughness of their reflections (the unresolved ripples; 0 on flat water); per lamp its world position and the reach
// of its light (yd), its colour (linear) and window (0: the client's, linear over the last quarter of the reach; above
// 0: Living Azeroth's, ((1 - (d / reach)^2)^2)^this, the light a yard across), and its attenuation (constant, linear,
// quadratic) and how far its reflection reaches (yd).
static const int kMaxLamps = 8;
float4 cLampControl : register(c121);
float4 cLampPosition[kMaxLamps] : register(c122);
float4 cLampColour[kMaxLamps] : register(c130);
float4 cLampFalloff[kMaxLamps] : register(c138);
float4 cRiverFlowMap : register(c146);      // sRiverFlow at world p: u = p.y * x + y, v = p.x * x + z; w = 1 with any flow
float4 cRiverFlowControl : register(c147);  // seconds mod 256
float4 cFloorFit0 : register(c148);         // the floor around the player (water_floor.h): z = x + y dx + z dy + ..., w = on
float4 cFloorFit1 : register(c149);         // ... + x dx^2 + y dx dy + z dy^2
float4 cFloorFit2 : register(c150);         // (dx, dy) = position - xy; z = the half span sampled
float4 cFloorWide0 : register(c151);        // the same fitted wider (water_floor.h kWideReach)
float4 cFloorWide1 : register(c152);
float4 cFloorWide2 : register(c153);
// Sun glitter (vw_optics.hlsli SunGlitter): strength (0: none; less in the rain), how far the facets tilt (more wind,
// more), seconds mod 256, the finest sparkle cell (yd).
float4 cGlitter : register(c154);
// The reflections' trace (vw_reflection.hlsli): 1 = steady steps (fixed angles as seen from the camera, so a reflection
// stays put as the camera turns), 0 = spread over the screen as before; the screen's height as an angle (radians).
float4 cReflectionSteps : register(c155);
// Lightning strikes near the camera (water_lamps.h: Living Azeroth's flash lights): count, strength; per strike its
// world position and how far its flash reaches (yd), and its colour (linear, this frame's).
static const int kMaxFlashes = 2;
float4 cFlashControl : register(c156);
float4 cFlashPosition[kMaxFlashes] : register(c157);
float4 cFlashColour[kMaxFlashes] : register(c159);

sampler2D sSceneColour : register(s0);
sampler2D sSceneDepth : register(s1);
sampler2D sWaterDepth : register(s2);
sampler2D sSurface0 : register(s3);
sampler2D sSurface1 : register(s4);
sampler2D sSurface2 : register(s5);
sampler2D sSurface3 : register(s6);
sampler2D sFoamState0 : register(s7);
sampler2D sFoamState1 : register(s8);
sampler2D sFoamState2 : register(s9);
sampler2D sFoamState3 : register(s10);
sampler2D sWaveFoamMasks : register(s11);
sampler2D sRipples : register(s12);
sampler2D sRiverFlow : register(s13);   // the current around the player (water_flow.h): r, g = 0.5 + flow / 8 yd/s
sampler2D sShoreFoamMask : register(s14);
sampler2D sDepthFoamMask : register(s15);

static const int kHighFoamSlot = 0;
static const int kMidFoamSlot = 1;
static const int kLowFoamSlot = 2;
static const int kShoreFoamSlot = 3;
static const int kDepthFoamSlot = 4;

float3 ToLight()
{
    return cLight.xyz;
}

float SunVisibility()
{
    return cLight.w;
}

float3 SunColour()
{
    return cSunColour.rgb;
}

float SunTransmission()
{
    return cSunColour.w;
}

float3 Ambient()
{
    return cAmbient.rgb;
}

float3 IsotropicLight()
{
    return cIsotropicLight.rgb;
}

bool StockFogApplies()
{
    return cStockFogColour.w > 0.5;
}

float3 StockFogColour()
{
    return cStockFogColour.rgb;
}

float StockFogVisibility(float viewZ)
{
    return saturate((cStockFog.x - viewZ) * cStockFog.y);
}

static const float kMinStockFogVisibility = 0.05;

float3 WithoutStockFog(float3 encoded, float viewZ)
{
    [branch] if (!StockFogApplies())
        return encoded;
    float visibility = StockFogVisibility(viewZ);
    return saturate((encoded - StockFogColour() * (1 - visibility)) / max(visibility, kMinStockFogVisibility));
}

float3 AbsorptionPerYard()
{
    return cAbsorption.rgb;
}

float3 CrestScatteringColour()
{
    return cScatteringTop.rgb;
}

float3 TroughScatteringColour()
{
    return cScatteringBottom.rgb;
}

float ScatteringAnisotropy()
{
    return cScatteringBottom.w;
}

float WaveFoamIntensity()
{
    return cWaveFoam.x;
}

float ShoreDistancePerDepth()
{
    return cShoreFoam.w;
}

float SunRoughnessParameter()
{
    return cSurfaceResponse.x;
}

float EnvironmentRoughnessParameter()
{
    return cSurfaceResponse.y;
}

float ReflectionStrength()
{
    return cSurfaceResponse.z;
}

float SpecularStrength()
{
    return cSurfaceResponse.w;
}

float InverseTileCount()
{
    return cWaveControl.x;
}

float WaterClassIndex()
{
    return cWaveControl.z;
}

float WaterDebugView()
{
    return cWaveControl.w;
}

float3 MaskTintLow(int slot)
{
    return cMaskTints[slot * 2].rgb;
}

float MaskPresent(int slot)
{
    return cMaskTints[slot * 2].w;
}

bool ReflectionFogActive()
{
    return cReflectionFogRange.w > 0.5;
}

float ReflectionFogCurveRange()
{
    return cReflectionFogRange.x;
}

float ReflectionFogSkyEnd()
{
    return cReflectionFogRange.y;
}

float3 MaskTintHigh(int slot)
{
    return cMaskTints[slot * 2 + 1].rgb;
}

float2 RippleOrigin()
{
    return cRippleWindow.xy;
}

float RippleInverseExtent()
{
    return cRippleWindow.z;
}

float RippleTexelUv()
{
    return cRippleWindow.w;
}

float RippleWeight()
{
    return cRippleShape.x;
}

float RippleGain()
{
    return cRippleShape.y;
}

float RippleExtent()
{
    return cRippleShape.z;
}

float RippleFadeEnd()
{
    return cRippleFade.x;
}

float RippleInverseFadeWidth()
{
    return cRippleFade.y;
}

float RippleTexelYards()
{
    return cRippleFade.z;
}
