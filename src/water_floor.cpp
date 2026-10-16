#include "water_floor.h"

#include "game/Pick.hpp"

#include <windows.h>

#include <cmath>
#include <cstring>
#include <utility>

namespace water_floor
{
namespace
{
GroundQuery g_testQuery = nullptr;
bool g_lastValid = false;

// The client's world intersect (wxl-core offsets/game/World.hpp kWorldIntersect) as the 12340 client has it: a frame
// set up and torn down around a jump to the intersect itself. Checked once before the first call: anywhere else
// (another build, a test without the client) there is no ground and no fit.
constexpr uintptr_t kWorldIntersect = 0x0077F310;
constexpr unsigned char kWorldIntersectBytes[] = {0x55, 0x8B, 0xEC, 0x5D, 0xE9, 0x57, 0x48, 0x02, 0x00};
int g_intersectOk = -1;

bool IntersectMatchesUnsafe()
{
    return std::memcmp(reinterpret_cast<const void*>(kWorldIntersect), kWorldIntersectBytes, sizeof(kWorldIntersectBytes)) == 0;
}

bool IntersectMatches()
{
    if (g_intersectOk < 0)
    {
        __try
        {
            g_intersectOk = IntersectMatchesUnsafe() ? 1 : 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            g_intersectOk = 0;
        }
    }
    return g_intersectOk == 1;
}

bool ClientGround(float x, float y, float nearZ, float& outZ)
{
    if (!IntersectMatches())
        return false;
    const float from[3] = {x, y, nearZ + kSearchAbove};
    const float to[3] = {x, y, nearZ - kSearchBelow};
    __try
    {
        wxl::game::world::WorldHit hit;
        if (!wxl::game::world::TraceLine(from, to, hit))
            return false;
        outZ = hit.pos.z;
        return std::isfinite(outZ);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// Solves the n x n system m x = r in place (Gaussian elimination with partial pivoting). False when singular.
bool Solve(double m[6][6], double r[6], int n, double x[6])
{
    for (int col = 0; col < n; ++col)
    {
        int pivot = col;
        for (int row = col + 1; row < n; ++row)
            if (std::fabs(m[row][col]) > std::fabs(m[pivot][col]))
                pivot = row;
        if (std::fabs(m[pivot][col]) < 1e-9)
            return false;
        if (pivot != col)
        {
            for (int k = 0; k < n; ++k)
                std::swap(m[col][k], m[pivot][k]);
            std::swap(r[col], r[pivot]);
        }
        for (int row = col + 1; row < n; ++row)
        {
            const double f = m[row][col] / m[col][col];
            for (int k = col; k < n; ++k)
                m[row][k] -= f * m[col][k];
            r[row] -= f * r[col];
        }
    }
    for (int row = n - 1; row >= 0; --row)
    {
        double s = r[row];
        for (int k = row + 1; k < n; ++k)
            s -= m[row][k] * x[k];
        x[row] = s / m[row][row];
    }
    return true;
}

// One least-squares pass over the samples `keep` marks, with `terms` terms (6: quadratic, 3: plane).
bool Pass(const float* xs, const float* ys, const float* zs, const bool* keep, int n, float cx, float cy, int terms,
          Fit& out)
{
    double m[6][6] = {}, r[6] = {}, x[6] = {};
    int used = 0;
    for (int i = 0; i < n; ++i)
    {
        if (!keep[i])
            continue;
        const double dx = xs[i] - cx, dy = ys[i] - cy;
        const double basis[6] = {1.0, dx, dy, dx * dx, dx * dy, dy * dy};
        for (int a = 0; a < terms; ++a)
        {
            for (int b = 0; b < terms; ++b)
                m[a][b] += basis[a] * basis[b];
            r[a] += basis[a] * zs[i];
        }
        ++used;
    }
    if (used < kMinSamples || !Solve(m, r, terms, x))
        return false;
    out = Fit{};
    out.a = static_cast<float>(x[0]);
    out.b = static_cast<float>(x[1]);
    out.c = static_cast<float>(x[2]);
    if (terms == 6)
    {
        out.d = static_cast<float>(x[3]);
        out.e = static_cast<float>(x[4]);
        out.f = static_cast<float>(x[5]);
    }
    out.cx = cx;
    out.cy = cy;
    out.valid = true;
    out.used = used;
    return true;
}

bool FitWith(const float* xs, const float* ys, const float* zs, const bool* keep, int n, float cx, float cy, Fit& out)
{
    return Pass(xs, ys, zs, keep, n, cx, cy, 6, out) || Pass(xs, ys, zs, keep, n, cx, cy, 3, out);
}
} // namespace

float HeightAt(const Fit& fit, float x, float y)
{
    const float dx = x - fit.cx, dy = y - fit.cy;
    return fit.a + fit.b * dx + fit.c * dy + fit.d * dx * dx + fit.e * dx * dy + fit.f * dy * dy;
}

bool FitSamples(const float* xs, const float* ys, const float* zs, int n, float cx, float cy, Fit& out)
{
    constexpr int kMax = kSamplesPerSide * kSamplesPerSide;
    if (n > kMax)
        n = kMax;
    bool keep[kMax];
    for (int i = 0; i < n; ++i)
        keep[i] = std::isfinite(zs[i]);
    Fit first;
    if (!FitWith(xs, ys, zs, keep, n, cx, cy, first))
        return false;
    int kept = 0;
    bool trimmed[kMax];
    for (int i = 0; i < n; ++i)
    {
        trimmed[i] = keep[i] && std::fabs(zs[i] - HeightAt(first, xs[i], ys[i])) <= kOutlierYards;
        kept += trimmed[i] ? 1 : 0;
    }
    if (kept >= kMinSamples && FitWith(xs, ys, zs, trimmed, n, cx, cy, out))
        return true;
    out = first;
    return true;
}

void SetGroundQueryForTest(GroundQuery query)
{
    g_testQuery = query;
}

bool LastFitValidForTest()
{
    return g_lastValid;
}

Fit Recentred(const Fit& fit, float cx, float cy)
{
    // z = a + b dx + c dy + d dx^2 + e dx dy + f dy^2 with dx = dx' + ox, dy = dy' + oy.
    Fit out = fit;
    const float ox = cx - fit.cx, oy = cy - fit.cy;
    out.a = fit.a + fit.b * ox + fit.c * oy + fit.d * ox * ox + fit.e * ox * oy + fit.f * oy * oy;
    out.b = fit.b + 2.0f * fit.d * ox + fit.e * oy;
    out.c = fit.c + fit.e * ox + 2.0f * fit.f * oy;
    out.cx = cx;
    out.cy = cy;
    return out;
}

void FloorFitter::Update(bool hasPlayer, const float feet[3], float radius, double seconds)
{
    if (!hasPlayer || !std::isfinite(feet[0]) || !std::isfinite(feet[1]) || !std::isfinite(feet[2]) || radius <= 0.0f)
    {
        Reset();
        return;
    }
    // The grid: a sample spacing apart, fixed to the world. The feet are between grid points `cell` and `cell` + 1,
    // `along` of the way.
    const float step = 2.0f * radius / (kSamplesPerSide - 1);
    const double ux = feet[0] / static_cast<double>(step), uy = feet[1] / static_cast<double>(step);
    const double gx = std::floor(ux), gy = std::floor(uy);
    if (!(std::fabs(gx) < 1e6 && std::fabs(gy) < 1e6))
    {
        Reset();
        return;
    }
    const int cell[2] = {static_cast<int>(gx), static_cast<int>(gy)};
    const float along[2] = {static_cast<float>(ux - gx), static_cast<float>(uy - gy)};
    constexpr int kHalf = (kSamplesPerSide - 1) / 2;
    const int origin[2] = {cell[0] - kHalf, cell[1] - kHalf};
    const bool resample = !m_have || radius != m_radius || std::fabs(feet[2] - m_atZ) >= kRefitRise ||
                          seconds - m_when >= kRefitSeconds || seconds < m_when;
    if (resample || origin[0] != m_origin[0] || origin[1] != m_origin[1])
    {
        // Where the window overlaps the last one the samples are the same world points and are kept: a cell crossed
        // samples only its new row or column.
        const GroundQuery query = g_testQuery ? g_testQuery : &ClientGround;
        float z[kWindow][kWindow];
        for (int j = 0; j < kWindow; ++j)
            for (int i = 0; i < kWindow; ++i)
            {
                const int oi = origin[0] + i - m_origin[0], oj = origin[1] + j - m_origin[1];
                if (!resample && oi >= 0 && oi < kWindow && oj >= 0 && oj < kWindow)
                {
                    z[j][i] = m_z[oj][oi];
                    continue;
                }
                float hit = 0.0f;
                z[j][i] = query(static_cast<float>(origin[0] + i) * step, static_cast<float>(origin[1] + j) * step,
                                feet[2], hit) ? hit : NAN;
            }
        std::memcpy(m_z, z, sizeof(z));
        m_origin[0] = origin[0];
        m_origin[1] = origin[1];
        if (resample)
        {
            m_have = true;
            m_atZ = feet[2];
            m_radius = radius;
            m_when = seconds;
        }
        // The fit about each of the four grid points around the feet, from the kSamplesPerSide square centred on it.
        constexpr int kN = kSamplesPerSide * kSamplesPerSide;
        for (int b = 0; b < 2; ++b)
            for (int a = 0; a < 2; ++a)
            {
                float xs[kN], ys[kN], zs[kN];
                int n = 0;
                for (int j = 0; j < kSamplesPerSide; ++j)
                    for (int i = 0; i < kSamplesPerSide; ++i, ++n)
                    {
                        xs[n] = static_cast<float>(origin[0] + a + i) * step;
                        ys[n] = static_cast<float>(origin[1] + b + j) * step;
                        zs[n] = m_z[b + j][a + i];
                    }
                Fit fit;
                m_corners[b][a] = FitSamples(xs, ys, zs, n, static_cast<float>(cell[0] + a) * step,
                                             static_cast<float>(cell[1] + b) * step, fit) ? fit : Fit{};
            }
    }
    // Blended about the feet by where they are between the four (bilinearly): as they walk, the weight passes smoothly
    // from one fit to the next, and a fit leaves the four only once it has none. Every fit there is keeps a sliver, so
    // the blend never divides by nothing.
    Fit blend;
    blend.cx = feet[0];
    blend.cy = feet[1];
    float total = 0.0f, heaviest = -1.0f;
    for (int b = 0; b < 2; ++b)
        for (int a = 0; a < 2; ++a)
        {
            if (!m_corners[b][a].valid)
                continue;
            const float w = (a ? along[0] : 1.0f - along[0]) * (b ? along[1] : 1.0f - along[1]) + 1e-4f;
            const Fit about = Recentred(m_corners[b][a], feet[0], feet[1]);
            blend.a += w * about.a;
            blend.b += w * about.b;
            blend.c += w * about.c;
            blend.d += w * about.d;
            blend.e += w * about.e;
            blend.f += w * about.f;
            total += w;
            if (w > heaviest)
            {
                heaviest = w;
                blend.used = about.used;
            }
        }
    if (total > 0.0f)
    {
        blend.a /= total;
        blend.b /= total;
        blend.c /= total;
        blend.d /= total;
        blend.e /= total;
        blend.f /= total;
        blend.valid = true;
    }
    m_target = blend;
    g_lastValid = m_target.valid;
    // Ease the shown fit toward it, about the same centre. No time passed (tests hold the clock), a long pause, or
    // nothing shown yet: take it as it is.
    const double dt = seconds - m_eased;
    m_eased = seconds;
    if (!m_target.valid || !m_shown.valid || !(dt > 0.0) || dt > 1.0)
    {
        m_shown = m_target;
        return;
    }
    const float t = static_cast<float>(1.0 - std::exp(-dt / kSettleSeconds));
    Fit from = Recentred(m_shown, m_target.cx, m_target.cy);
    from.a += (m_target.a - from.a) * t;
    from.b += (m_target.b - from.b) * t;
    from.c += (m_target.c - from.c) * t;
    from.d += (m_target.d - from.d) * t;
    from.e += (m_target.e - from.e) * t;
    from.f += (m_target.f - from.f) * t;
    from.used = m_target.used;
    m_shown = from;
}

void FloorFitter::Constants(float c148[4], float c149[4], float c150[4]) const
{
    c148[0] = m_shown.a;
    c148[1] = m_shown.b;
    c148[2] = m_shown.c;
    c148[3] = m_shown.valid ? 1.0f : 0.0f;
    c149[0] = m_shown.d;
    c149[1] = m_shown.e;
    c149[2] = m_shown.f;
    c149[3] = 0.0f;
    c150[0] = m_shown.cx;
    c150[1] = m_shown.cy;
    c150[2] = m_shown.valid ? m_radius : 0.0f;
    c150[3] = 0.0f;
}
} // namespace water_floor
