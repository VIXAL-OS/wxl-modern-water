#pragma once

// The floor around the player, from the world's own ground: terrain and buildings, never models, so never the player
// or their mount. vw_surface.hlsli's BedHeight takes the bed under the player's body from it.
//
// The water reads the bed from the depth behind it, and a player standing in the water shows their body there, which it
// took for a shoal. Within the body's reach the bed used to be taken at the player's feet; mounted, that reach is 9 yd,
// and on a sloping shore the real floor within it, rising above the feet, was pushed down to them: the coast's swell and
// surf stopped in a 9 yd circle around the player ("now the drake outline in the waves is just a full circle", the user,
// 2026-10-12). Now the floor there is a smooth surface fitted to the ground at 25 points around the player, and only what
// stands clearly above it counts as the body.

namespace water_floor
{
// z = a + b dx + c dy + d dx^2 + e dx dy + f dy^2, with (dx, dy) = (x, y) - (cx, cy).
struct Fit
{
    float a = 0.0f, b = 0.0f, c = 0.0f, d = 0.0f, e = 0.0f, f = 0.0f;
    float cx = 0.0f, cy = 0.0f;
    bool valid = false;
    int used = 0;   // samples in the fit (after dropping outliers)
};

// The ground's height at (x, y), searching down from above nearZ; false when nothing is there.
using GroundQuery = bool (*)(float x, float y, float nearZ, float& outZ);

constexpr int kSamplesPerSide = 5;           // 25 samples to a fit
constexpr int kMinSamples = 8;               // fewer than this valid: no fit
constexpr float kOutlierYards = 0.75f;       // a sample this far off the first fit is dropped (a wall, a bank)
constexpr double kRefitSeconds = 1.0;        // resample at least this often (the world loading in)
constexpr float kRefitRise = 2.0f;           // and once the player's height has changed this much (the search window)
// The samples sit on a grid fixed to the world, a sample spacing apart, and the fit drawn with is blended from the fits
// about the four grid points around the feet, by where the feet are between them. Refitted every yard the player moved,
// from samples that moved with them, the fit changed by a little each time and the coast's swell near them jumped with
// it ("whenever I'm moving the framerate of the shoreline waves *specifically* dips", the user, 2026-10-12). Fitted once
// per cell, about the cell the feet were in, it still jumped at every cell they crossed, eased in over 0.4 s, and the
// edge of its square jumped a whole cell ("refit jitter still happens a LITTLE bit"). Blended, it changes smoothly as
// they walk, and crossing into the next cell changes nothing: the fit it lets go of had no weight left.
// A change in the ground itself (loading in, a new search window) is eased in over about this long (exponentially).
constexpr double kSettleSeconds = 0.4;

// The same quadratic about another centre (exact).
Fit Recentred(const Fit& fit, float cx, float cy);
constexpr float kSearchAbove = 4.0f;         // yd above the feet the search starts (the body's height)
constexpr float kSearchBelow = 30.0f;        // and how far below them it looks
// The wide fit (vw_surface.hlsli BedDepthBelow): the floor's change between two spots a few yd apart, out to where the
// swell's looks under the water just past a hovering drake can land on it, and to the bed seen along a slanting view
// through water 5 yd deep (19 yd off in the tests' view).
constexpr float kWideReach = 24.0f;

// Least squares through the samples (x, y, z), centred on (cx, cy), then again without those more than kOutlierYards
// off. A plane when the samples can't hold the curvature. False when there are too few.
bool FitSamples(const float* xs, const float* ys, const float* zs, int n, float cx, float cy, Fit& out);

// The fit's height at (x, y).
float HeightAt(const Fit& fit, float x, float y);

// The client's ground (wxl-core's TraceLine: terrain and map objects), or a stand-in for tests (nullptr: the client's).
void SetGroundQueryForTest(GroundQuery query);
// Whether the last fit any FloorFitter made is valid (tests).
bool LastFitValidForTest();

class FloorFitter
{
public:
    // Samples the ground on the grid (kSamplesPerSide - 1 spacings to twice `radius`), fits about the four grid points
    // around the feet and blends those by where the feet are. No player: no fit.
    void Update(bool hasPlayer, const float feet[3], float radius, double seconds);
    const Fit& Current() const { return m_shown; }
    // c148 = (a, b, c, valid), c149 = (d, e, f, 0), c150 = (cx, cy, the half span sampled, 0), about the feet.
    void Constants(float c148[4], float c149[4], float c150[4]) const;
    void Reset() { m_target = Fit{}; m_shown = Fit{}; m_have = false; }

private:
    static constexpr int kWindow = kSamplesPerSide + 1;   // samples a side the four fits use between them
    float m_z[kWindow][kWindow] = {};   // the ground at them ([y][x]), NAN where there is none
    int m_origin[2] = {};               // the grid point of m_z[0][0], in spacings
    Fit m_corners[2][2];                // the fits about the four grid points around the feet ([y][x])
    Fit m_target;       // blended from them
    Fit m_shown;        // the one drawn with, eased toward it
    bool m_have = false;
    float m_atZ = 0.0f;
    float m_radius = 0.0f;
    double m_when = 0.0;    // when the samples were all taken
    double m_eased = 0.0;   // when the shown fit was last moved
};
} // namespace water_floor
