#pragma once

#include <d3d9.h>

#include <climits>
#include <cstdint>

// River current. WoW stores none: a liquid is a surface with heights. A scan of every map's water
// (waterfall-scan/flow_scan.py, 2026-10-11) gave each terrain cell (4.17 yd) a flow: down the surface where it slopes
// (vertex heights, or the steps between flat layers), carried on along flat stretches from the sloping water around
// them, seeded above every fall's lip, still in the middle of lakes and on the sea. 148 of the 150 lips with water
// sloping above them agree with it. ("ok whatever you did here is good, but how do we get wake effects/laminar flow
// in the water itself?", the user; "go ahead and do the full map scan", 2026-10-11.)
struct RiverFlowChunk
{
    int16_t map;
    int16_t row; // global chunk row (8 cells) along world -x: cell R is centred at x = 32 TILE - (R + 0.5) CELL
    int16_t col; // and column along world -y
};

constexpr float kRiverFlowCell = 1600.0f / 3.0f / 128.0f; // yd per cell
constexpr int kRiverFlowTexels = 64;                      // the texture around the player: 64 cells (267 yd) a side
constexpr int kRiverFlowRecentreCells = 8;                // rebuilt once the player is 8 cells (33 yd) off its centre
constexpr float kRiverFlowMaxSpeed = 4.0f;                // yd/s the texture holds: r, g = 0.5 + flow / (2 x this)

// The stored flow (world yd/s) of the cell at world (x, y) on `map`; false where none is stored (still water, land).
bool RiverFlowAt(int map, float x, float y, float& fx, float& fy);
uint32_t RiverFlowChunkCount();

// Rocks in moving water (waterfall-scan/rock_scan.py): placed rock doodads with their base in the water, and terrain
// poking out with the current on both sides. The ripple simulation holds each as a still footprint; the current
// carries the water past it, which sheds a wake downstream ("go ahead with the rock wakes if it wouldn't interfere
// with the game too much", the user, 2026-10-11).
struct RiverRock
{
    int16_t map;
    float x, y;
    float surface; // the water's surface around it
    float radius;  // yd
};
constexpr uint32_t kMaxActiveRocks = 8;       // held in the ripple simulation at once, nearest first
constexpr float kRockBandBelow = 20.0f;       // a rock's water may lie this far below / above the player's feet
constexpr float kRockBandAbove = 10.0f;
// Rocks on `map` within `range` yards (ground distance) of `centre`, their water in the band around `height`,
// nearest first.
uint32_t CollectRiverRocks(int map, const float centre[2], float height, float range, const RiverRock** out,
                           uint32_t capacity);
uint32_t RiverRockCount();

// Tests: a uniform flow everywhere on every map instead of the table (nullptr: the table again).
void OverrideRiverFlowForTest(const float* flow);
// Tests: seconds added to the current's own clock only (the waves stay as they are), so its motion can be measured.
void OffsetRiverClockForTest(double seconds);
double RiverClockOffset();

// The flow around the player as a texture the water shader samples (bilinear, so it is smooth between cells).
class RiverFlowTexture
{
public:
    ~RiverFlowTexture();
    // Rebuilds it when the map changes or `centre` (world x, y) is kRiverFlowRecentreCells off its centre. False when
    // there is no texture.
    bool Update(IDirect3DDevice9* dev, int map, const float centre[2]);
    IDirect3DTexture9* Texture() const { return m_texture; }
    // The shader's mapping (c146): u = y * x + y_, v = x * x + z (the texture's cells, as rows along world -x and
    // columns along -y); w = 1 when any cell in it flows.
    void Mapping(float out[4]) const;
    bool AnyFlow() const { return m_any; }
    uint32_t Rebuilds() const { return m_rebuilds; }
    void Release();

private:
    IDirect3DTexture9* m_texture = nullptr;
    int m_map = INT_MIN;
    int m_r0 = 0, m_c0 = 0;
    bool m_any = false;
    bool m_override = false;
    uint32_t m_rebuilds = 0;
};
