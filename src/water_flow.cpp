#include "water_flow.h"

#include "river_flow_data.inc"
#include "river_rocks.inc"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <unordered_map>

namespace
{
constexpr float kFlowUnit = 1.0f / 32.0f; // yd/s per stored step
constexpr float kOrigin = 4096.0f;        // 32 tiles of 128 cells: the cell coordinate of world 0

bool g_overrideOn = false;
double g_clockOffset = 0.0;
float g_override[2] = {};

uint64_t Key(int map, int row, int col)
{
    return (static_cast<uint64_t>(static_cast<uint16_t>(map)) << 32) |
           (static_cast<uint64_t>(static_cast<uint16_t>(row)) << 16) | static_cast<uint16_t>(col);
}

const std::unordered_map<uint64_t, uint32_t>& Index()
{
    static const std::unordered_map<uint64_t, uint32_t> index = [] {
        std::unordered_map<uint64_t, uint32_t> m;
        m.reserve(std::size(kRiverFlowChunks) * 2);
        for (uint32_t i = 0; i < std::size(kRiverFlowChunks); ++i)
            m.emplace(Key(kRiverFlowChunks[i].map, kRiverFlowChunks[i].row, kRiverFlowChunks[i].col), i);
        return m;
    }();
    return index;
}

// The cell (global row R, column C): its flow, or false.
bool CellFlow(int map, int R, int C, float& fx, float& fy)
{
    if (g_overrideOn)
    {
        fx = g_override[0];
        fy = g_override[1];
        return true;
    }
    if (map < 0 || map > 0x7FFF || R < 0 || C < 0)
        return false;
    const auto& index = Index();
    const auto it = index.find(Key(map, R >> 3, C >> 3));
    if (it == index.end())
        return false;
    const int8_t* cell = kRiverFlowCells + static_cast<size_t>(it->second) * 128 + ((R & 7) * 8 + (C & 7)) * 2;
    fx = cell[0] * kFlowUnit;
    fy = cell[1] * kFlowUnit;
    return cell[0] != 0 || cell[1] != 0;
}

uint8_t Encode(float v)
{
    const float t = 0.5f + std::clamp(v / (2.0f * kRiverFlowMaxSpeed), -0.5f, 0.5f);
    return static_cast<uint8_t>(std::lround(t * 255.0f));
}
}

bool RiverFlowAt(int map, float x, float y, float& fx, float& fy)
{
    fx = fy = 0.0f;
    if (!std::isfinite(x) || !std::isfinite(y))
        return false;
    const int R = static_cast<int>(std::floor(kOrigin - x / kRiverFlowCell));
    const int C = static_cast<int>(std::floor(kOrigin - y / kRiverFlowCell));
    return CellFlow(map, R, C, fx, fy);
}

uint32_t RiverFlowChunkCount() { return static_cast<uint32_t>(std::size(kRiverFlowChunks)); }

uint32_t RiverRockCount() { return static_cast<uint32_t>(std::size(kRiverRocks)); }

uint32_t CollectRiverRocks(int map, const float centre[2], float height, float range, const RiverRock** out,
                           uint32_t capacity)
{
    if (!capacity || map < 0 || map > 0x7FFF || !std::isfinite(centre[0]) || !std::isfinite(centre[1]))
        return 0;
    // The table is sorted by map, then x.
    const RiverRock* first = std::lower_bound(std::begin(kRiverRocks), std::end(kRiverRocks), map,
                                              [](const RiverRock& r, int m) { return r.map < m; });
    const RiverRock* begin = std::lower_bound(first, std::end(kRiverRocks), centre[0] - range,
                                              [map](const RiverRock& r, float x) { return r.map == map && r.x < x; });
    uint32_t count = 0;
    float distances[kMaxActiveRocks * 4] = {};
    const uint32_t keep = std::min<uint32_t>(capacity, kMaxActiveRocks * 4);
    for (const RiverRock* r = begin; r != std::end(kRiverRocks) && r->map == map && r->x <= centre[0] + range; ++r)
    {
        if (r->surface < height - kRockBandBelow || r->surface > height + kRockBandAbove)
            continue;
        const float d = std::hypot(r->x - centre[0], r->y - centre[1]);
        if (d > range)
            continue;
        // Insert nearest first; past `keep` the farthest drops out.
        uint32_t at = count < keep ? count++ : keep;
        if (at == keep && d >= distances[keep - 1])
            continue;
        if (at == keep)
            at = keep - 1;
        while (at > 0 && distances[at - 1] > d)
        {
            distances[at] = distances[at - 1];
            out[at] = out[at - 1];
            --at;
        }
        distances[at] = d;
        out[at] = r;
    }
    return count;
}

void OverrideRiverFlowForTest(const float* flow)
{
    g_overrideOn = flow != nullptr;
    if (flow)
    {
        g_override[0] = flow[0];
        g_override[1] = flow[1];
    }
}

void OffsetRiverClockForTest(double seconds) { g_clockOffset = seconds; }
double RiverClockOffset() { return g_clockOffset; }

RiverFlowTexture::~RiverFlowTexture() { Release(); }

void RiverFlowTexture::Release()
{
    if (m_texture)
        m_texture->Release();
    m_texture = nullptr;
    m_map = INT_MIN;
}

bool RiverFlowTexture::Update(IDirect3DDevice9* dev, int map, const float centre[2])
{
    if (!m_texture)
    {
        if (FAILED(dev->CreateTexture(kRiverFlowTexels, kRiverFlowTexels, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED,
                                      &m_texture, nullptr)))
        {
            m_texture = nullptr;
            return false;
        }
        m_map = INT_MIN;
    }
    if (!std::isfinite(centre[0]) || !std::isfinite(centre[1]))
        return true;
    const int R = static_cast<int>(std::floor(kOrigin - centre[0] / kRiverFlowCell));
    const int C = static_cast<int>(std::floor(kOrigin - centre[1] / kRiverFlowCell));
    const int half = kRiverFlowTexels / 2;
    if (map == m_map && m_override == g_overrideOn && std::abs(R - (m_r0 + half)) < kRiverFlowRecentreCells &&
        std::abs(C - (m_c0 + half)) < kRiverFlowRecentreCells)
        return true;
    D3DLOCKED_RECT locked = {};
    if (FAILED(m_texture->LockRect(0, &locked, nullptr, 0)))
        return false;
    m_map = map;
    m_override = g_overrideOn;
    m_r0 = R - half;
    m_c0 = C - half;
    m_any = false;
    for (int j = 0; j < kRiverFlowTexels; ++j)
    {
        DWORD* row = reinterpret_cast<DWORD*>(static_cast<uint8_t*>(locked.pBits) + j * locked.Pitch);
        for (int i = 0; i < kRiverFlowTexels; ++i)
        {
            float fx = 0.0f, fy = 0.0f;
            const bool flows = CellFlow(map, m_r0 + j, m_c0 + i, fx, fy);
            m_any |= flows;
            row[i] = (0xFFu << 24) | (static_cast<DWORD>(Encode(fx)) << 16) | (static_cast<DWORD>(Encode(fy)) << 8);
        }
    }
    m_texture->UnlockRect(0);
    ++m_rebuilds;
    return true;
}

void RiverFlowTexture::Mapping(float out[4]) const
{
    // Texel (i, j) holds cell (m_r0 + j, m_c0 + i); its centre is at u = (i + 0.5) / 64, and the continuous cell
    // column of world y is kOrigin - y / CELL, so u = (kOrigin - y / CELL - m_c0) / 64 (rows alike with x).
    const float scale = -1.0f / (kRiverFlowCell * kRiverFlowTexels);
    out[0] = scale;
    out[1] = (kOrigin - static_cast<float>(m_c0)) / kRiverFlowTexels;
    out[2] = (kOrigin - static_cast<float>(m_r0)) / kRiverFlowTexels;
    out[3] = m_texture && m_any ? 1.0f : 0.0f;
}
