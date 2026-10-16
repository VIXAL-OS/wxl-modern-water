#pragma once

#include "engine_actors.h"
#include "water_falls.h"
#include "water_flow.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

constexpr size_t kMaxDepartedWaterEntries = 256;
constexpr float kWaterSplashRearmDepthPerHeight = 0.15f;
constexpr double kMinWaterSplashIntervalSeconds = 2.0;
constexpr double kStaleWaterTrackSeconds = 0.5;
constexpr double kWaterFootprintFadeSeconds = 1.0;
constexpr double kWaterWakeRampSeconds = 0.3;
constexpr float kMaxWaterWakeSpeed = 40.0f;
// Footprint level for a unit walking on the surface: half of a fully immersed one (3.0), so walking drags a V
// and standing leaves a faint dimple.
constexpr float kWaterWalkingFootprintLevel = 1.5f;
constexpr float kWaterWakeStepSlack = 0.5f;
// A water walker landing from a jump (gone from the contacts for a moment, back on the surface): a slap a bit
// stronger than a character jumping in (-1.5), at most one every half second.
constexpr float kWaterWalkerLandingImpulse = -2.2f;
constexpr double kWaterWalkerAirborneSeconds = 0.15;
constexpr double kWaterWalkerLandingWindowSeconds = 2.5;
constexpr double kMinWaterWalkerLandingIntervalSeconds = 0.5;
// Walking on the surface: one small ring per footstep (every stride travelled), left and right of the path in
// turn, instead of a swimmer's dragged wake.
constexpr float kWaterWalkerStrideYards = 1.6f;
constexpr float kWaterWalkerStepSideYards = 0.22f;
constexpr float kWaterWalkerStepRadius = 0.55f;
constexpr float kWaterWalkerStepImpulse = -1.1f;
// On a mount (the user, 2026-10-11, passing on a friend's idea: "mounted water walking should cause bigger ripples
// than unmounted"): hooves, a longer gait, broader and deeper rings, and a harder landing.
constexpr float kMountedWalkerStrideYards = 2.6f;
constexpr float kMountedWalkerStepSideYards = 0.45f;
constexpr float kMountedWalkerStepRadius = 0.95f;
constexpr float kMountedWalkerStepImpulse = -1.9f;
constexpr float kMountedWalkerLandingImpulse = -3.2f;

struct WaterRippleDisturbance
{
    float from[2] = {};
    float to[2] = {};
    float radius = 0.0f;
    float amplitude = 0.0f;
    bool held = false;
};

struct WaterEntryState
{
    bool armed = false;
    double splashedAt = -std::numeric_limits<double>::infinity();
};

struct WaterFootprint
{
    float from[2] = {};
    double fromSeconds = 0.0;
    float stamped[2] = {};
    double placedAt = 0.0;
    double steppedAt = -std::numeric_limits<double>::infinity();
    float radius = 0.0f;
    float level = 0.0f;
    float motion = 0.0f;
};

struct WaterContactTrack
{
    uint64_t guid = 0;
    float position[3] = {};
    double seenAt = 0.0;
    WaterEntryState entry;
    WaterFootprint footprint;
    uint32_t placements = 0;
    float splashRadius = 0.0f;
    float pendingSplash = 0.0f;
    float stride = 0.0f;      // water walkers: yards walked since the last footstep ring
    bool leftFoot = false;
};

struct DepartedWaterEntry
{
    uint64_t guid = 0;
    WaterEntryState entry;
    double seenAt = 0.0;
};

// Fishing bobbers. The server creates one GO_STATE_READY; when a fish bites it sets GO_STATE_ACTIVE and
// GO_FLAG_NODESPAWN together.
constexpr uint8_t kBobberReadyState = 1;
constexpr uint8_t kBobberActiveState = 0;
constexpr uint32_t kBobberNoDespawnFlag = 0x20;
constexpr size_t kMaxRememberedBobbers = 32;
constexpr size_t kMaxPendingBobberRipples = 32;
constexpr double kBobberMemorySeconds = 60.0;
constexpr double kBobberRippleExpirySeconds = 1.0;
constexpr double kMinBobberIdleSeconds = 1.2;
constexpr double kBobberIdleJitterSeconds = 1.3;
// Bobber rings, in the same units as a unit's entry splash (-1.5 for a character jumping in), sized to read at
// fishing distance: a big plop and echo when it lands, a bigger triple pulse when a fish bites, and a small ring
// now and then while it floats.
constexpr float kBobberLandingRadius = 1.2f;
constexpr float kBobberLandingImpulse = -3.0f;
constexpr double kBobberLandingEchoSeconds = 0.3;
constexpr float kBobberLandingEchoRadius = 0.9f;
constexpr float kBobberLandingEchoImpulse = -1.5f;
constexpr float kBobberBiteRadius = 1.8f;
constexpr float kBobberBiteImpulse = -4.5f;
constexpr double kBobberBiteEchoSeconds[2] = {0.2, 0.45};
constexpr float kBobberBiteEchoRadius[2] = {1.4f, 1.1f};
constexpr float kBobberBiteEchoImpulse[2] = {-2.5f, -1.8f};
constexpr float kBobberIdleRadius = 0.6f;
constexpr float kBobberIdleImpulse = -0.8f;

struct WaterBobberTrack
{
    uint64_t guid = 0;
    float position[2] = {};
    double seenAt = 0.0;
    uint8_t state = 0;
    uint32_t flags = 0;
    bool bitten = false;
    bool own = false;
    double nextIdleAt = 0.0;
    uint32_t idles = 0;
};

struct RememberedBobber
{
    WaterBobberTrack track;
    double seenAt = 0.0;
};

struct PendingBobberRipple
{
    float position[2] = {};
    double dueAt = 0.0;
    float radius = 0.0f;
    float amplitude = 0.0f;
};

bool WaterBobberBites(const WaterBobberTrack& before, const WaterBobber& now);
double WaterBobberIdleDelay(uint64_t guid, uint32_t index);

// Fish schools (fishing pools): the client draws fish splashing at the surface; the water around them rings like a
// bobber's. Each step a small ring with kSchoolRingChance somewhere within kSchoolRadius (uniform over the disc), and
// now and then (kSchoolJumpChance) a fish jumping: a broader, deeper one ("what if schools of fish had random ripples
// in and around them, like fishing bobbers", the user, 2026-10-11).
constexpr float kSchoolRadius = 3.0f;
constexpr float kSchoolRingChance = 0.3f;     // per step (30 a second): about 9 rings a second
constexpr float kSchoolRingMinRadius = 0.35f;
constexpr float kSchoolRingMaxRadius = 0.7f;
constexpr float kSchoolRingMinImpulse = -0.6f;
constexpr float kSchoolRingMaxImpulse = -1.3f;
constexpr float kSchoolJumpChance = 0.012f;   // about one jump every 3 seconds
constexpr float kSchoolJumpRadius = 1.0f;
constexpr float kSchoolJumpImpulse = -2.6f;

// Waterfall churn: impulses scattered along each active emitter's base every step, from whatever budget is left
// after bobbers, footprints and splashes.
constexpr uint32_t kMaxTurbulencePerStep = 16;
// A fall's pulse: one broad swell on its base line now and then. Radius 1.5 yd + 6% of the base's length + 1.5% of the
// drop, within 2.5-6 yd; depth kFallPulseDepthPerYard x radius x the fall's strength, so a wider pulse keeps the same
// slope; chance per step (30 a second) 1% + 0.05% per yard of base, within 1.2-3%: about one a second under a 40 yd fall.
// The churn around a fall's foam: broader impulses from the foam band's edge out to kFallOuterReachShare x its radius
// (at least kFallOuterMinReach yd) further, length / 6 + 1 of them a step, at most kFallOuterMaxPoints.
constexpr float kFallOuterMinRadius = 1.2f;
constexpr float kFallOuterMaxRadius = 2.2f;
constexpr float kFallOuterReachShare = 1.5f;
constexpr float kFallOuterMinReach = 4.0f;
constexpr uint32_t kFallOuterMaxPoints = 4;
constexpr float kFallPulseMinRadius = 2.5f;
constexpr float kFallPulseMaxRadius = 6.0f;
constexpr float kFallPulseDepthPerYard = 1.2f;
constexpr float kFallPulseMinChance = 0.012f;
constexpr float kFallPulseMaxChance = 0.03f;
float WaterfallStrength(const WaterfallEmitter& e);
float FallPulseRadius(const WaterfallEmitter& e);
float FallPulseChance(const WaterfallEmitter& e);

float ClientRippleStrength(float depth, float height);
float WaterFootprintRadius(float collisionRadius);
float WaterFootprintLevel(float depth, float height);
float WaterWakeMotion(float speed);
float WaterFootprintDepthShare(float motion);
float WaterFootprintFade(double sincePlaced, double sinceSeen);
bool PlausibleWaterWakeStep(float distance, double seconds);
float WaterSplashRadius(float collisionRadius);
float WaterEntryImpulse(float depth, float height);

class WaterContactTracker
{
public:
    void Reset();
    void Update(const WaterContactFrame& frame, double seconds);
    void PlaceFootprints(double seconds);
    uint32_t TakeDisturbances(double stepSeconds, WaterRippleDisturbance* out, uint32_t capacity);
    bool Emitting() const
    {
        return !m_tracks.empty() || !m_bobbers.empty() || !m_bobberRipples.empty() || m_waterfallCount > 0 || m_rockCount > 0 ||
               m_schoolCount > 0;
    }
    void SetWaterfalls(const WaterfallEmitter* const* active, uint32_t count);
    uint32_t Waterfalls() const { return m_waterfallCount; }
    // Rocks in moving water near the player, each with the current there (world x, y yd/s, two floats a rock).
    void SetRocks(const RiverRock* const* rocks, const float* flows, uint32_t count);
    uint32_t Rocks() const { return m_rockCount; }
    uint32_t Tracks() const { return static_cast<uint32_t>(m_tracks.size()); }
    uint32_t Contacts() const { return m_contacts; }
    uint32_t RememberedEntries() const { return static_cast<uint32_t>(m_departed.size()); }
    uint32_t Bobbers() const { return static_cast<uint32_t>(m_bobbers.size()); }
    uint32_t PendingBobberRipples() const { return static_cast<uint32_t>(m_bobberRipples.size()); }
    uint64_t BobberRipplesTaken() const { return m_bobberRipplesTaken; }
    uint32_t Schools() const { return m_schoolCount; }
    uint64_t SchoolRipplesTaken() const { return m_schoolRipplesTaken; }
    uint64_t Landings() const { return m_landings; }
    uint64_t Footsteps() const { return m_footsteps; }
    bool OwnBobber(float position[2]) const;
    // own (optional) says whether the bobber found is the player's own.
    bool FocusBobber(const float from[2], float reach, float position[2], bool* own = nullptr) const;
    // Whether a unit touching the water is within radius of from (the player, when from is the camera target).
    bool ContactNear(const float from[2], float radius) const;
    const WaterContactTrack* Find(uint64_t guid) const;

private:
    WaterContactTrack& TrackOf(uint64_t guid, bool& created);
    WaterEntryState FirstEntryState(uint64_t guid, double& lastSeen);
    void Follow(WaterContactTrack& track, const WaterContact& contact, double seconds, bool created);
    uint32_t TakeFootprints(double stepSeconds, bool fresh, WaterRippleDisturbance* out, uint32_t capacity);
    uint32_t TakeSplashes(WaterRippleDisturbance* out, uint32_t capacity);
    void RememberEntry(const WaterContactTrack& track);
    void DropStaleTracks(double seconds);
    void FollowBobbers(const WaterContactFrame& frame, double seconds);
    void FollowBobber(const WaterBobber& bobber, double seconds);
    void QueueBobberRipple(const float position[2], double dueAt, float radius, float amplitude);
    void RememberBobber(const WaterBobberTrack& track);
    uint32_t TakeBobberRipples(double stepSeconds, WaterRippleDisturbance* out, uint32_t capacity);
    uint32_t TakeTurbulence(WaterRippleDisturbance* out, uint32_t capacity);
    uint32_t TakeSchoolRipples(WaterRippleDisturbance* out, uint32_t capacity);
    uint32_t TakeRocks(WaterRippleDisturbance* out, uint32_t capacity);
    float Random();

    std::vector<WaterContactTrack> m_tracks;
    std::vector<DepartedWaterEntry> m_departed;
    std::vector<WaterBobberTrack> m_bobbers;
    std::vector<RememberedBobber> m_rememberedBobbers;
    std::vector<PendingBobberRipple> m_bobberRipples;
    uint64_t m_bobberRipplesTaken = 0;
    uint64_t m_landings = 0;
    uint64_t m_footsteps = 0;
    const WaterfallEmitter* m_waterfalls[kMaxActiveWaterfalls] = {};
    uint32_t m_waterfallCount = 0;
    const RiverRock* m_rocks[kMaxActiveRocks] = {};
    float m_rockFlows[kMaxActiveRocks][2] = {};
    uint32_t m_rockCount = 0;
    uint32_t m_turbulenceCursor = 0;
    WaterSchool m_schools[kMaxWaterSchools] = {};
    uint32_t m_schoolCount = 0;
    uint64_t m_schoolRipplesTaken = 0;
    uint64_t m_random = 0x9E3779B97F4A7C15ull;
    double m_seconds = -1.0;
    uint32_t m_contacts = 0;
};
