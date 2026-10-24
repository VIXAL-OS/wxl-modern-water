// The water's depth, moved below its surface once the water is shaded. The client's water draw writes the
// surface's depth (the shading reads it), but models drawn after the water lie on it or just under it by design: a
// fountain's foam sheet 4 cm under its pool's surface and an "aquarium" sheet within a centimetre of it, splash
// decals at a fall's base, and the foam and ripple sheets laid on rivers at a fall's head or foot, which sit anywhere
// from on the surface to over a third of a yard under it. Against the surface's own depth they failed, or fought it
// pixel by pixel as the view moved. Moved half a yard down, measured straight down, they draw over the water as they
// do over the client's own water (which writes no depth), and whatever lies deeper under the water stays hidden.
// The Frenzyheart River above the Sholazar fall (52.2, 63.5) showed it: the fall's foam sheet lies 0.08 to 0.16 yd
// under the river, so with a tenth of a yard it drew only in pale slabs with straight edges, flickering where it sat
// right at the tenth ("did we ever fix the Frenzyheart River z-fighting thing with Modern Water", the user,
// 2026-10-24). The push never goes past the scene behind the water (the bed of shallow water, a bank, the cliff and
// pool below a lip), so nothing drawn later shows through those, again as over the client's own water.
//
// Reads the water's and the scene's linear depth copies, not the depth buffer this pass writes.
#include "vf_common.hlsli"

sampler2D sWaterDepth : register(s0);
sampler2D sSceneDepth : register(s1); // the scene before the water was drawn: the same kind of copy
float4 cDecode : register(c9);  // rgb: the copies' decode (R32F, or packed bytes)
float4 cPush : register(c10);   // x: yards straight down, y: the most it may move along the view (yards of view depth),
                                // z: lips in cLip
// The lips of the falls nearby, each the top edge of its fall's box (the river pours over there): [2i] end a (world xyz),
// w = reach (yd across); [2i + 1] end b, w = how far down (yd: the fall's height). Where the river's surface data
// overhangs the edge, the falling sheet under the overhang lies deeper than the push and would hide under the water in
// patches (the client's own water writes no depth, so there the sheet always shows). Water within reach of a lip, at
// its height, moves down that far instead, fading out over 1.5 yd across and from 2 to 3 yd above or below it. (Made
// for the Sholazar report of 2026-10-16, which was the foam sheet above; that fall's box top is 50 yd downstream of
// where its river pours over, out of reach of the river.)
float4 cLip[16] : register(c11);

struct PushOutput
{
    float4 colour : COLOR0;
    float depth : DEPTH;
};

PushOutput main(float2 pixelIndex : VPOS)
{
    float2 pixel = pixelIndex + 0.5;
    float2 uv = (pixel - ViewportOrigin()) / ViewportSize();
    float viewZ = dot(tex2Dlod(sWaterDepth, float4(uv, 0, 0)).rgb, cDecode.rgb);
    // The copy stops at the fog's far distance: water out there keeps the depth it has.
    clip(MaxFogDistance() - viewZ - 0.5);
    float3 ray = ViewRayAtUnitDepth(pixel);
    float3 direction = normalize(ray);
    float down = max(-ViewToWorldDirection(direction).z, 0.05);
    float3 world = CameraPositionWorld() + ViewToWorldDirection(ray * viewZ);
    float push = cPush.x, most = cPush.y;
    [unroll] for (int i = 0; i < 8; ++i)
    {
        float4 a = cLip[2 * i], b = cLip[2 * i + 1];
        float2 ab = b.xy - a.xy;
        float t = saturate(dot(world.xy - a.xy, ab) / max(dot(ab, ab), 1e-4));
        float across = length(world.xy - (a.xy + ab * t));
        float w = saturate((a.w + 1.5 - across) / 1.5) * saturate(3 - abs(world.z - lerp(a.z, b.z, t)));
        w = i < cPush.z ? w : 0;
        push = max(push, b.w * w);
        most = max(most, b.w * w / 0.05);
    }
    // Along the view ray, a point `push` lower is push / down further on; its view depth grows by that times the
    // ray's forward component.
    float pushed = viewZ + min(push / down, most) * direction.z;
    // No further than the scene behind the water, nor nearer than the surface.
    float sceneZ = dot(tex2Dlod(sSceneDepth, float4(uv, 0, 0)).rgb, cDecode.rgb);
    pushed = min(pushed, max(sceneZ, viewZ));
    PushOutput output;
    output.colour = 0;
    output.depth = saturate(DepthAtInfiniteViewZ() + DepthPerInverseViewZ() / pushed);
    return output;
}
