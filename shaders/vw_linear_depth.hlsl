#include "vf_common.hlsli"

sampler2D sDepth : register(s0);
// The player's pixels (player_mask.cpp): cleared to 1 each frame, 0 where their own batches drew, on the world target's
// pixel grid. cPlayerMask.x: 1 when it was drawn this frame (0: none, the copy says "not the player" everywhere); yz:
// 1 / its size in pixels.
sampler2D sPlayerMask : register(s1);
float4 cPlayerMask : register(c9);

static const float kPackedDepthLevels = 16777215;
static const float kHighByteWeight = 65536;
static const float kMidByteWeight = 256;
static const float kByteMax = 255;

float4 PackedDepth(float normalisedDepth)
{
    float packed = floor(saturate(normalisedDepth) * kPackedDepthLevels + 0.5);
    float high = floor(packed / kHighByteWeight);
    float rest = packed - high * kHighByteWeight;
    float mid = floor(rest / kMidByteWeight);
    float low = rest - mid * kMidByteWeight;
    return float4(high, mid, low, kByteMax) / kByteMax;
}

float4 main(float2 copyTexel : VPOS) : COLOR0
{
    float2 pixel = ViewportOrigin() + copyTexel + 0.5;
    float viewZ = LinearDepth(SampleDepth(sDepth, pixel));
#if PACKED_DEPTH
    return PackedDepth(viewZ / MaxFogDistance());
#else
    // The view depth, and in g whether the player is drawn there (vw_reflection.hlsli looks past them by it).
    float player = cPlayerMask.x > 0 ? 1 - tex2Dlod(sPlayerMask, float4(pixel * cPlayerMask.yz, 0, 0)).r : 0;
    return float4(viewZ, player, 0, 0);
#endif
}
