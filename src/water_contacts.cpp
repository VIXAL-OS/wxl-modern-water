#include "water_contacts.h"

#include <algorithm>
#include <cmath>

namespace
{
constexpr float kClientSplashDepthPerHeight = 0.4f;
constexpr float kClientFullStrength = 1.0f;
constexpr float kClientDeepestStrength = 0.5f;
constexpr float kFootprintRadiusPerCollisionRadius = 1.25f;
constexpr float kMinFootprintRadius = 0.3f;
constexpr float kMaxFootprintRadius = 6.0f;
constexpr float kFullImmersionDepthPerHeight = 0.25f;
constexpr float kFullyImmersedFootprintLevel = 3.0f;
constexpr float kStandingFootprintShare = 0.2f;
constexpr float kFullWakeSpeed = 4.5f;
constexpr float kSplashRadiusPerCollisionRadius = 3.0f;
constexpr float kMinSplashRadius = 0.75f;
constexpr float kMaxSplashRadius = 9.0f;
constexpr float kEntryImpulsePerStrength = -1.5f;
constexpr double kDepartedEntrySeconds = 30.0;
constexpr uint32_t kMaxBobberRipplesPerStep = 8;

bool TakeEntrySplash(WaterEntryState& entry, const WaterContact& contact, double seconds)
{
    if (contact.swimming)
    {
        entry.armed = false;
        return false;
    }
    const float depth = WaterContactDepth(contact);
    if (depth <= kWaterSplashRearmDepthPerHeight * contact.height)
        entry.armed = true;
    if (!entry.armed || depth <= kClientSplashDepthPerHeight * contact.height)
        return false;
    entry.armed = false;
    if (seconds - entry.splashedAt < kMinWaterSplashIntervalSeconds)
        return false;
    entry.splashedAt = seconds;
    return true;
}

double SmoothUnitStep(double x)
{
    const double t = std::clamp(x, 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

float GroundDistance(const float a[3], const float b[3])
{
    return std::hypot(a[0] - b[0], a[1] - b[1]);
}

void PlaceFootprint(WaterContactTrack& track, const float position[3], double seconds)
{
    WaterFootprint& footprint = track.footprint;
    for (int axis = 0; axis < 2; ++axis)
        footprint.from[axis] = footprint.stamped[axis] = position[axis];
    footprint.fromSeconds = seconds;
    footprint.placedAt = seconds;
    footprint.motion = 0.0f;
    footprint.steppedAt = -std::numeric_limits<double>::infinity();
    ++track.placements;
}

void FollowMotion(WaterFootprint& footprint, float travelled, double stepSeconds)
{
    const double elapsed = stepSeconds - footprint.steppedAt;
    footprint.steppedAt = stepSeconds;
    if (!(elapsed > 0.0) || !std::isfinite(elapsed))
        return;
    const float ramp = static_cast<float>(elapsed / kWaterWakeRampSeconds);
    const float target = WaterWakeMotion(static_cast<float>(travelled / elapsed));
    footprint.motion += std::clamp(target - footprint.motion, -ramp, ramp);
}

void MoveFootprint(WaterContactTrack& track)
{
    WaterFootprint& footprint = track.footprint;
    for (int axis = 0; axis < 2; ++axis)
        footprint.from[axis] = track.position[axis];
    footprint.fromSeconds = track.seenAt;
}

float PathFraction(const WaterContactTrack& track, double stepSeconds)
{
    const double span = track.seenAt - track.footprint.fromSeconds;
    if (!(span > 0.0))
        return 1.0f;
    return static_cast<float>(std::clamp((stepSeconds - track.footprint.fromSeconds) / span, 0.0, 1.0));
}

WaterRippleDisturbance FootprintStep(WaterContactTrack& track, double stepSeconds)
{
    WaterFootprint& footprint = track.footprint;
    const float along = PathFraction(track, stepSeconds);
    WaterRippleDisturbance step;
    for (int axis = 0; axis < 2; ++axis)
    {
        step.from[axis] = footprint.stamped[axis];
        step.to[axis] = footprint.from[axis] + (track.position[axis] - footprint.from[axis]) * along;
        footprint.stamped[axis] = step.to[axis];
    }
    FollowMotion(footprint, std::hypot(step.to[0] - step.from[0], step.to[1] - step.from[1]), stepSeconds);
    step.radius = footprint.radius;
    step.amplitude = footprint.level *
                     WaterFootprintFade(stepSeconds - footprint.placedAt, stepSeconds - track.seenAt) *
                     WaterFootprintDepthShare(footprint.motion);
    step.held = true;
    return step;
}

WaterRippleDisturbance SplashOf(const WaterContactTrack& track)
{
    WaterRippleDisturbance splash;
    for (int axis = 0; axis < 2; ++axis)
        splash.from[axis] = splash.to[axis] = track.position[axis];
    splash.radius = track.splashRadius;
    splash.amplitude = track.pendingSplash;
    return splash;
}
}

float ClientRippleStrength(float depth, float height)
{
    const float limit = ClientRippleDepthLimit(height);
    if (depth <= 0.5f * limit)
        return kClientFullStrength;
    return std::max(kClientDeepestStrength, kClientDeepestStrength + (limit - depth) / limit);
}

float WaterFootprintRadius(float collisionRadius)
{
    return std::clamp(kFootprintRadiusPerCollisionRadius * collisionRadius, kMinFootprintRadius, kMaxFootprintRadius);
}

float WaterFootprintLevel(float depth, float height)
{
    const float immersion = std::clamp(depth / (kFullImmersionDepthPerHeight * height), 0.0f, 1.0f);
    return kFullyImmersedFootprintLevel * immersion * ClientRippleStrength(depth, height);
}

float WaterWakeMotion(float speed)
{
    return std::clamp(speed / kFullWakeSpeed, 0.0f, 1.0f);
}

float WaterFootprintDepthShare(float motion)
{
    return kStandingFootprintShare + (1.0f - kStandingFootprintShare) * motion;
}

float WaterFootprintFade(double sincePlaced, double sinceSeen)
{
    return static_cast<float>(SmoothUnitStep(sincePlaced / kWaterFootprintFadeSeconds) *
                              SmoothUnitStep(1.0 - sinceSeen / kStaleWaterTrackSeconds));
}

bool PlausibleWaterWakeStep(float distance, double seconds)
{
    return distance <= kMaxWaterWakeSpeed * std::max(seconds, 0.0) + kWaterWakeStepSlack;
}

float WaterSplashRadius(float collisionRadius)
{
    return std::clamp(kSplashRadiusPerCollisionRadius * collisionRadius, kMinSplashRadius, kMaxSplashRadius);
}

float WaterEntryImpulse(float depth, float height)
{
    return kEntryImpulsePerStrength * ClientRippleStrength(depth, height);
}

bool WaterBobberBites(const WaterBobberTrack& before, const WaterBobber& now)
{
    return (before.state == kBobberReadyState && now.state == kBobberActiveState) ||
           (!(before.flags & kBobberNoDespawnFlag) && (now.flags & kBobberNoDespawnFlag));
}

// Deterministic per-bobber jitter, so the faint rings of two bobbers side by side do not pulse in step.
double WaterBobberIdleDelay(uint64_t guid, uint32_t index)
{
    uint64_t x = guid + 0x9E3779B97F4A7C15ull * (static_cast<uint64_t>(index) + 1);
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    x ^= x >> 31;
    const double unit = static_cast<double>(x >> 11) / static_cast<double>(1ull << 53);
    return kMinBobberIdleSeconds + kBobberIdleJitterSeconds * unit;
}

void WaterContactTracker::Reset()
{
    m_tracks.clear();
    m_departed.clear();
    // Bobbers still in the water must not land again when the simulation restarts after a gap.
    for (const WaterBobberTrack& bobber : m_bobbers)
        RememberBobber(bobber);
    m_bobbers.clear();
    m_bobberRipples.clear();
    m_seconds = -1.0;
    m_contacts = 0;
}

void WaterContactTracker::QueueBobberRipple(const float position[2], double dueAt, float radius, float amplitude)
{
    if (m_bobberRipples.size() >= kMaxPendingBobberRipples)
        return;
    PendingBobberRipple ripple;
    ripple.position[0] = position[0];
    ripple.position[1] = position[1];
    ripple.dueAt = dueAt;
    ripple.radius = radius;
    ripple.amplitude = amplitude;
    m_bobberRipples.push_back(ripple);
}

void WaterContactTracker::RememberBobber(const WaterBobberTrack& track)
{
    const RememberedBobber memory = {track, track.seenAt};
    auto same = std::find_if(m_rememberedBobbers.begin(), m_rememberedBobbers.end(),
                             [&track](const RememberedBobber& r) { return r.track.guid == track.guid; });
    if (same != m_rememberedBobbers.end())
        *same = memory;
    else if (m_rememberedBobbers.size() < kMaxRememberedBobbers)
        m_rememberedBobbers.push_back(memory);
    else
        *std::min_element(m_rememberedBobbers.begin(), m_rememberedBobbers.end(),
                          [](const RememberedBobber& a, const RememberedBobber& b) { return a.seenAt < b.seenAt; }) =
            memory;
}

void WaterContactTracker::FollowBobber(const WaterBobber& bobber, double seconds)
{
    auto found = std::find_if(m_bobbers.begin(), m_bobbers.end(),
                              [&bobber](const WaterBobberTrack& t) { return t.guid == bobber.guid; });
    if (found == m_bobbers.end())
    {
        WaterBobberTrack track;
        const auto remembered =
            std::find_if(m_rememberedBobbers.begin(), m_rememberedBobbers.end(),
                         [&bobber](const RememberedBobber& r) { return r.track.guid == bobber.guid; });
        if (remembered != m_rememberedBobbers.end())
        {
            track = remembered->track;
            m_rememberedBobbers.erase(remembered);
            track.nextIdleAt = std::min(track.nextIdleAt, seconds + WaterBobberIdleDelay(track.guid, track.idles));
        }
        else
        {
            track.guid = bobber.guid;
            track.state = bobber.state;
            track.flags = bobber.flags;
            track.bitten = bobber.state != kBobberReadyState;
            track.nextIdleAt = seconds + WaterBobberIdleDelay(bobber.guid, 0);
            QueueBobberRipple(bobber.position, seconds, kBobberLandingRadius, kBobberLandingImpulse);
            QueueBobberRipple(bobber.position, seconds + kBobberLandingEchoSeconds, kBobberLandingEchoRadius,
                              kBobberLandingEchoImpulse);
        }
        m_bobbers.push_back(track);
        found = m_bobbers.end() - 1;
    }
    WaterBobberTrack& track = *found;
    if (!track.bitten && WaterBobberBites(track, bobber))
    {
        track.bitten = true;
        QueueBobberRipple(bobber.position, seconds, kBobberBiteRadius, kBobberBiteImpulse);
        for (int echo = 0; echo < 2; ++echo)
            QueueBobberRipple(bobber.position, seconds + kBobberBiteEchoSeconds[echo], kBobberBiteEchoRadius[echo],
                              kBobberBiteEchoImpulse[echo]);
    }
    track.state = bobber.state;
    track.flags = bobber.flags;
    track.own = bobber.own;
    track.position[0] = bobber.position[0];
    track.position[1] = bobber.position[1];
    track.seenAt = seconds;
    if (seconds >= track.nextIdleAt)
    {
        QueueBobberRipple(track.position, seconds, kBobberIdleRadius, kBobberIdleImpulse);
        track.nextIdleAt = seconds + WaterBobberIdleDelay(track.guid, ++track.idles);
    }
}

bool WaterContactTracker::OwnBobber(float position[2]) const
{
    for (const WaterBobberTrack& track : m_bobbers)
        if (track.own)
        {
            position[0] = track.position[0];
            position[1] = track.position[1];
            return true;
        }
    return false;
}

bool WaterContactTracker::ContactNear(const float from[2], float radius) const
{
    for (const WaterContactTrack& track : m_tracks)
        if (std::hypot(track.position[0] - from[0], track.position[1] - from[1]) <= radius)
            return true;
    return false;
}

// The bobber the ripple window should lean toward: the player's own if there is one, else the nearest bobber
// beyond reach (so another player's long cast still ripples). False when nothing needs a lean.
bool WaterContactTracker::FocusBobber(const float from[2], float reach, float position[2], bool* own) const
{
    if (own)
        *own = false;
    if (OwnBobber(position))
    {
        if (own)
            *own = true;
        return true;
    }
    float best = std::numeric_limits<float>::infinity();
    for (const WaterBobberTrack& track : m_bobbers)
    {
        const float distance = std::hypot(track.position[0] - from[0], track.position[1] - from[1]);
        if (distance > reach && distance < best)
        {
            best = distance;
            position[0] = track.position[0];
            position[1] = track.position[1];
        }
    }
    return std::isfinite(best);
}

void WaterContactTracker::FollowBobbers(const WaterContactFrame& frame, double seconds)
{
    const uint32_t count = std::min(frame.bobberCount, kMaxWaterBobbers);
    for (uint32_t i = 0; i < count; ++i)
        FollowBobber(frame.bobbers[i], seconds);
    const auto gone = [seconds](const WaterBobberTrack& track) {
        return seconds - track.seenAt > kStaleWaterTrackSeconds;
    };
    for (const WaterBobberTrack& track : m_bobbers)
        if (gone(track))
            RememberBobber(track);
    m_bobbers.erase(std::remove_if(m_bobbers.begin(), m_bobbers.end(), gone), m_bobbers.end());
    m_rememberedBobbers.erase(std::remove_if(m_rememberedBobbers.begin(), m_rememberedBobbers.end(),
                                             [seconds](const RememberedBobber& r) {
                                                 return seconds - r.seenAt > kBobberMemorySeconds;
                                             }),
                              m_rememberedBobbers.end());
    m_bobberRipples.erase(std::remove_if(m_bobberRipples.begin(), m_bobberRipples.end(),
                                         [seconds](const PendingBobberRipple& r) {
                                             return seconds - r.dueAt > kBobberRippleExpirySeconds;
                                         }),
                          m_bobberRipples.end());
}

uint32_t WaterContactTracker::TakeBobberRipples(double stepSeconds, WaterRippleDisturbance* out, uint32_t capacity)
{
    capacity = std::min(capacity, kMaxBobberRipplesPerStep);
    uint32_t count = 0;
    auto ripple = m_bobberRipples.begin();
    while (ripple != m_bobberRipples.end() && count < capacity)
    {
        if (ripple->dueAt > stepSeconds)
        {
            ++ripple;
            continue;
        }
        WaterRippleDisturbance& d = out[count++];
        d = {};
        for (int axis = 0; axis < 2; ++axis)
            d.from[axis] = d.to[axis] = ripple->position[axis];
        d.radius = ripple->radius;
        d.amplitude = ripple->amplitude;
        ripple = m_bobberRipples.erase(ripple);
    }
    m_bobberRipplesTaken += count;
    return count;
}

const WaterContactTrack* WaterContactTracker::Find(uint64_t guid) const
{
    for (const WaterContactTrack& track : m_tracks)
        if (track.guid == guid)
            return &track;
    return nullptr;
}

WaterContactTrack& WaterContactTracker::TrackOf(uint64_t guid, bool& created)
{
    for (WaterContactTrack& track : m_tracks)
        if (track.guid == guid)
        {
            created = false;
            return track;
        }
    created = true;
    WaterContactTrack track;
    track.guid = guid;
    m_tracks.push_back(track);
    return m_tracks.back();
}

WaterEntryState WaterContactTracker::FirstEntryState(uint64_t guid, double& lastSeen)
{
    lastSeen = -std::numeric_limits<double>::infinity();
    const auto departed = std::find_if(m_departed.begin(), m_departed.end(),
                                       [guid](const DepartedWaterEntry& entry) { return entry.guid == guid; });
    if (departed == m_departed.end())
        return {};
    const WaterEntryState entry = departed->entry;
    lastSeen = departed->seenAt;
    m_departed.erase(departed);
    return entry;
}

void WaterContactTracker::Follow(WaterContactTrack& track, const WaterContact& contact, double seconds, bool created)
{
    double lastSeen = track.seenAt;
    if (created)
        track.entry = FirstEntryState(contact.guid, lastSeen);
    // A water walker is a contact only within half a yard of the surface, so a jump drops it for a moment; coming
    // back after that is a landing and slaps the water (stepping onto the water from the shore doesn't). Only
    // while the tracker itself kept being updated: a gap in the water being drawn is no jump.
    const double away = seconds - lastSeen;
    const bool continuous = m_seconds >= 0.0 && seconds - m_seconds < kWaterWalkerAirborneSeconds;
    if (contact.waterWalking && continuous && away >= kWaterWalkerAirborneSeconds &&
        away <= kWaterWalkerLandingWindowSeconds &&
        seconds - track.entry.splashedAt >= kMinWaterWalkerLandingIntervalSeconds)
    {
        track.entry.splashedAt = seconds;
        track.pendingSplash += contact.mounted ? kMountedWalkerLandingImpulse : kWaterWalkerLandingImpulse;
        ++m_landings;
    }
    const bool plausible =
        !created && PlausibleWaterWakeStep(GroundDistance(track.position, contact.position), seconds - track.seenAt);
    const float previous[2] = {track.position[0], track.position[1]};
    if (!plausible)
        PlaceFootprint(track, contact.position, seconds);
    else
        MoveFootprint(track);
    std::copy(contact.position, contact.position + 3, track.position);
    track.seenAt = seconds;
    const float depth = WaterContactDepth(contact);
    track.footprint.radius = WaterFootprintRadius(contact.radius);
    track.footprint.level = WaterFootprintLevel(depth, contact.height);
    // Standing on the surface leaves nothing immersed: a water walker leaves one ring per footstep instead of a
    // wake, each stride along its path, left and right in turn (several in one frame if it moved that far).
    if (contact.waterWalking)
    {
        track.footprint.level = 0.0f;
        const float dx = contact.position[0] - previous[0], dy = contact.position[1] - previous[1];
        const float moved = std::hypot(dx, dy);
        const float stride = contact.mounted ? kMountedWalkerStrideYards : kWaterWalkerStrideYards;
        const float sideStep = contact.mounted ? kMountedWalkerStepSideYards : kWaterWalkerStepSideYards;
        if (!plausible)
            track.stride = 0.0f;
        else if (moved > 1e-4f)
        {
            track.stride += moved;
            while (track.stride >= stride)
            {
                track.stride -= stride;
                const float back = track.stride / moved; // this step's place: that much of the frame's move ago
                const float side = track.leftFoot ? sideStep : -sideStep;
                track.leftFoot = !track.leftFoot;
                const float at[2] = {contact.position[0] - dx * back - dy / moved * side,
                                     contact.position[1] - dy * back + dx / moved * side};
                QueueBobberRipple(at, seconds, contact.mounted ? kMountedWalkerStepRadius : kWaterWalkerStepRadius,
                                  contact.mounted ? kMountedWalkerStepImpulse : kWaterWalkerStepImpulse);
                ++m_footsteps;
            }
        }
    }
    track.splashRadius = WaterSplashRadius(contact.radius);
    if (TakeEntrySplash(track.entry, contact, seconds))
        track.pendingSplash += WaterEntryImpulse(depth, contact.height);
}

void WaterContactTracker::RememberEntry(const WaterContactTrack& track)
{
    const DepartedWaterEntry memory = {track.guid, track.entry, track.seenAt};
    auto same = std::find_if(m_departed.begin(), m_departed.end(),
                             [&track](const DepartedWaterEntry& departed) { return departed.guid == track.guid; });
    if (same != m_departed.end())
        *same = memory;
    else if (m_departed.size() < kMaxDepartedWaterEntries)
        m_departed.push_back(memory);
    else
        *std::min_element(m_departed.begin(), m_departed.end(),
                          [](const DepartedWaterEntry& a, const DepartedWaterEntry& b) {
                              return a.seenAt < b.seenAt;
                          }) = memory;
}

void WaterContactTracker::DropStaleTracks(double seconds)
{
    const auto stale = [seconds](const WaterContactTrack& track) {
        return seconds - track.seenAt > kStaleWaterTrackSeconds;
    };
    for (const WaterContactTrack& track : m_tracks)
        if (stale(track))
            RememberEntry(track);
    m_tracks.erase(std::remove_if(m_tracks.begin(), m_tracks.end(), stale), m_tracks.end());
    m_departed.erase(std::remove_if(m_departed.begin(), m_departed.end(),
                                    [seconds](const DepartedWaterEntry& departed) {
                                        return seconds - departed.seenAt > kDepartedEntrySeconds;
                                    }),
                     m_departed.end());
}

void WaterContactTracker::Update(const WaterContactFrame& frame, double seconds)
{
    if (m_seconds >= 0.0 && seconds < m_seconds)
        Reset();
    m_contacts = std::min(frame.count, kMaxWaterContacts);
    for (uint32_t i = 0; i < m_contacts; ++i)
    {
        const WaterContact& contact = frame.contacts[i];
        bool created = false;
        WaterContactTrack& track = TrackOf(contact.guid, created);
        if (!created && track.seenAt == seconds)
            continue;
        Follow(track, contact, seconds, created);
    }
    FollowBobbers(frame, seconds);
    m_schoolCount = std::min(frame.schoolCount, kMaxWaterSchools);
    for (uint32_t i = 0; i < m_schoolCount; ++i)
        m_schools[i] = frame.schools[i];
    m_seconds = seconds;
    DropStaleTracks(seconds);
}

void WaterContactTracker::PlaceFootprints(double seconds)
{
    for (WaterContactTrack& track : m_tracks)
        PlaceFootprint(track, track.position, seconds);
}

uint32_t WaterContactTracker::TakeFootprints(double stepSeconds, bool fresh, WaterRippleDisturbance* out,
                                             uint32_t capacity)
{
    uint32_t count = 0;
    for (WaterContactTrack& track : m_tracks)
        if ((track.seenAt == m_seconds) == fresh && count < capacity && track.footprint.level > 0.0f) // water walkers: rings
            out[count++] = FootprintStep(track, stepSeconds);
    return count;
}

uint32_t WaterContactTracker::TakeSplashes(WaterRippleDisturbance* out, uint32_t capacity)
{
    uint32_t count = 0;
    for (WaterContactTrack& track : m_tracks)
    {
        if (track.pendingSplash == 0.0f || count == capacity)
            continue;
        out[count++] = SplashOf(track);
        track.pendingSplash = 0.0f;
    }
    return count;
}

// Taller falls hit harder, up to a little under a bobber bite (-4.5), so a 100 yd fall churns rather than explodes.
// The first build used 0.5-1.5 and the user could barely see the Underbelly falls (9-14 yd tall) churn.
float WaterfallStrength(const WaterfallEmitter& e)
{
    switch (e.kind)
    {
    case kWaterfallFall:
        return std::clamp(1.0f + e.height / 30.0f, 1.0f, 2.5f);
    case kWaterfallDecal:
    case kWaterfallSplash:
        return 0.9f;
    default:
        return 0.6f;
    }
}

// The chop's own impulses are narrow and saturate the slope within a few yards of the base, so a pulse has to be wide
// to show: the first one (3 yd, 1.4x the chop, 8% a step) vanished in it, in game and in a CPU copy of
// vw_ripple_step. A 5-6 yd one at 6-7x boils up at the foot and spreads as a ring for two or three seconds.
float FallPulseRadius(const WaterfallEmitter& e)
{
    const float length = std::hypot(e.b[0] - e.a[0], e.b[1] - e.a[1]);
    return std::clamp(1.5f + 0.06f * length + 0.015f * e.height, kFallPulseMinRadius, kFallPulseMaxRadius);
}

float FallPulseChance(const WaterfallEmitter& e)
{
    const float length = std::hypot(e.b[0] - e.a[0], e.b[1] - e.a[1]);
    return std::clamp(0.01f + 0.0005f * length, kFallPulseMinChance, kFallPulseMaxChance);
}

// A rock in a current: the water is held a little raised where it stands (it doesn't ripple through the rock), and
// eddies break off its downstream side, small impulses the current (the ripple step's drift) carries away as a
// trail behind it. A held rock alone shed no wake: the simulation's waves (about 2.6 yd/s) outrun the current, so it
// only swept its start-up ring downstream. All by the current's speed there, full at 2 yd/s: the held level up to
// 0.4 (a standing wading player's is 0.6), an eddy on up to 60% of steps (30 a second).
constexpr float kRockLevel = 0.4f;
constexpr float kRockFullSpeed = 2.0f;
constexpr float kRockEddyChance = 0.6f;
constexpr float kRockEddyImpulse = -0.9f;     // x (0.6 .. 1.2)
constexpr float kRockEddyMinRadius = 0.35f;   // yd
constexpr float kRockEddyMaxRadius = 0.7f;
constexpr float kRockEddySpread = 1.2f;       // radians either side of straight downstream
constexpr float kRockEddyReach = 0.5f;        // yd past the rock's edge

void WaterContactTracker::SetRocks(const RiverRock* const* rocks, const float* flows, uint32_t count)
{
    m_rockCount = std::min(count, kMaxActiveRocks);
    for (uint32_t i = 0; i < m_rockCount; ++i)
    {
        m_rocks[i] = rocks[i];
        m_rockFlows[i][0] = flows[2 * i];
        m_rockFlows[i][1] = flows[2 * i + 1];
    }
}

uint32_t WaterContactTracker::TakeRocks(WaterRippleDisturbance* out, uint32_t capacity)
{
    uint32_t count = 0;
    for (uint32_t i = 0; i < m_rockCount && count < capacity; ++i)
    {
        const RiverRock& r = *m_rocks[i];
        const float speed = std::hypot(m_rockFlows[i][0], m_rockFlows[i][1]);
        const float strength = std::clamp(speed / kRockFullSpeed, 0.0f, 1.0f);
        if (strength <= 0.0f)
            continue;
        WaterRippleDisturbance& d = out[count++];
        d = {};
        d.from[0] = d.to[0] = r.x;
        d.from[1] = d.to[1] = r.y;
        d.radius = r.radius;
        d.amplitude = kRockLevel * strength;
        d.held = true;
        if (count < capacity && Random() < kRockEddyChance * strength)
        {
            const float heading = std::atan2(m_rockFlows[i][1], m_rockFlows[i][0]) + kRockEddySpread * (2.0f * Random() - 1.0f);
            const float reach = r.radius + kRockEddyReach * Random();
            WaterRippleDisturbance& e = out[count++];
            e = {};
            e.from[0] = e.to[0] = r.x + reach * std::cos(heading);
            e.from[1] = e.to[1] = r.y + reach * std::sin(heading);
            e.radius = kRockEddyMinRadius + (kRockEddyMaxRadius - kRockEddyMinRadius) * Random();
            e.amplitude = kRockEddyImpulse * strength * (0.6f + 0.6f * Random());
        }
    }
    return count;
}

void WaterContactTracker::SetWaterfalls(const WaterfallEmitter* const* active, uint32_t count)
{
    m_waterfallCount = std::min(count, kMaxActiveWaterfalls);
    for (uint32_t i = 0; i < m_waterfallCount; ++i)
        m_waterfalls[i] = active[i];
}

float WaterContactTracker::Random()
{
    m_random ^= m_random >> 12;
    m_random ^= m_random << 25;
    m_random ^= m_random >> 27;
    return static_cast<float>((m_random * 0x2545F4914F6CDD1Dull) >> 40) / static_cast<float>(1u << 24);
}

// Each step scatters impulses along the active bases, nearest emitters first but starting one further along each
// step, so a crowded budget still reaches every fall over a few steps. A fall drops a few per step along its width,
// a splash decal fewer and broader, a fountain one small one.
uint32_t WaterContactTracker::TakeTurbulence(WaterRippleDisturbance* out, uint32_t capacity)
{
    capacity = std::min(capacity, kMaxTurbulencePerStep);
    uint32_t count = 0;
    for (uint32_t visited = 0; visited < m_waterfallCount && count < capacity; ++visited)
    {
        const WaterfallEmitter& e = *m_waterfalls[(m_turbulenceCursor + visited) % m_waterfallCount];
        const float dx = e.b[0] - e.a[0], dy = e.b[1] - e.a[1];
        const float length = std::hypot(dx, dy);
        float across[2] = {0.0f, 0.0f};
        if (length > 0.01f)
        {
            across[0] = -dy / length;
            across[1] = dx / length;
        }
        uint32_t points = 1;
        float spread = 0.0f, minRadius = 0.3f, maxRadius = 0.6f;
        if (e.kind == kWaterfallFall)
        {
            points = std::clamp(static_cast<uint32_t>(length / 3.0f) + 1u, 1u, 5u);
            spread = 0.5f * e.thickness + 0.75f;
            minRadius = 0.5f;
            maxRadius = 1.1f;
        }
        else if (e.kind == kWaterfallDecal || e.kind == kWaterfallSplash)
        {
            points = std::clamp(static_cast<uint32_t>(length / 10.0f) + 1u, 1u, 3u);
            spread = 0.5f * e.thickness;
            minRadius = 0.6f;
            maxRadius = 1.4f;
        }
        else
        {
            spread = 0.5f * std::min(e.thickness, 3.0f);
        }
        const float strength = WaterfallStrength(e);
        for (uint32_t i = 0; i < points && count < capacity; ++i)
        {
            const float along = Random();
            const float side = (2.0f * Random() - 1.0f) * spread;
            WaterRippleDisturbance& d = out[count++];
            d = {};
            for (int axis = 0; axis < 2; ++axis)
                d.from[axis] = d.to[axis] = e.a[axis] + along * (e.b[axis] - e.a[axis]) + side * across[axis];
            if (length <= 0.01f)
            {
                const float angle = 6.2831853f * Random();
                d.from[0] = d.to[0] = e.a[0] + side * std::cos(angle);
                d.from[1] = d.to[1] = e.a[1] + side * std::sin(angle);
            }
            d.radius = minRadius + (maxRadius - minRadius) * Random();
            d.amplitude = -strength * (0.4f + 0.6f * Random());
        }
        // Around the foam: broader impulses on both sides of the base, from the edge of its white band out to 2.5 times
        // its radius (at least 4 yd past it), so the pool ripples where it isn't white. The churn above all lands under
        // the foam, and Glacial Falls' pool, its splash decals folded into the fall, rippled only there ("bottom
        // ripples ... barely visible beneath the foam. rather than making the foam less visible i'd like to make the
        // ripples broader", the user, 2026-10-11).
        if (e.kind == kWaterfallFall && length > 0.01f)
        {
            float segment[4], shape[4];
            FallFoamShape(e, segment, shape);
            const float inner = shape[0], reach = std::max(kFallOuterMinReach, kFallOuterReachShare * shape[0]);
            const uint32_t outer = std::clamp(static_cast<uint32_t>(length / 6.0f) + 1u, 1u, kFallOuterMaxPoints);
            for (uint32_t i = 0; i < outer && count < capacity; ++i)
            {
                const float along = Random();
                const float side = (Random() < 0.5f ? -1.0f : 1.0f) * (inner + reach * Random());
                WaterRippleDisturbance& d = out[count++];
                d = {};
                for (int axis = 0; axis < 2; ++axis)
                    d.from[axis] = d.to[axis] = e.a[axis] + along * (e.b[axis] - e.a[axis]) + side * across[axis];
                d.radius = kFallOuterMinRadius + (kFallOuterMaxRadius - kFallOuterMinRadius) * Random();
                d.amplitude = -strength * (0.3f + 0.5f * Random());
            }
        }
        // Now and then (about once a second under a big fall) a fall also drops one broad pulse right on its base
        // line: a swell that rolls out from the foot as a ring over the chop (the user asked for "stronger ripples
        // directly from the bottom" on top of the turbulence).
        if (e.kind == kWaterfallFall && count < capacity && Random() < FallPulseChance(e))
        {
            const float along = 0.2f + 0.6f * Random();
            const float side = (2.0f * Random() - 1.0f) * std::min(0.15f * e.thickness, 1.0f);
            WaterRippleDisturbance& d = out[count++];
            d = {};
            for (int axis = 0; axis < 2; ++axis)
                d.from[axis] = d.to[axis] = e.a[axis] + along * (e.b[axis] - e.a[axis]) + side * across[axis];
            d.radius = FallPulseRadius(e);
            d.amplitude = -strength * kFallPulseDepthPerYard * d.radius;
        }
    }
    if (m_waterfallCount)
        m_turbulenceCursor = (m_turbulenceCursor + 1) % m_waterfallCount;
    return count;
}

uint32_t WaterContactTracker::TakeSchoolRipples(WaterRippleDisturbance* out, uint32_t capacity)
{
    uint32_t count = 0;
    for (uint32_t i = 0; i < m_schoolCount && count < capacity; ++i)
    {
        const WaterSchool& school = m_schools[i];
        const bool ring = Random() < kSchoolRingChance, jump = Random() < kSchoolJumpChance;
        for (int k = 0; k < 2 && count < capacity; ++k)
        {
            if (!(k == 0 ? ring : jump))
                continue;
            const float angle = 6.2831853f * Random();
            const float reach = kSchoolRadius * std::sqrt(Random());
            WaterRippleDisturbance& d = out[count++];
            d = {};
            d.from[0] = d.to[0] = school.position[0] + reach * std::cos(angle);
            d.from[1] = d.to[1] = school.position[1] + reach * std::sin(angle);
            if (k == 0)
            {
                d.radius = kSchoolRingMinRadius + (kSchoolRingMaxRadius - kSchoolRingMinRadius) * Random();
                d.amplitude = kSchoolRingMinImpulse + (kSchoolRingMaxImpulse - kSchoolRingMinImpulse) * Random();
            }
            else
            {
                d.radius = kSchoolJumpRadius;
                d.amplitude = kSchoolJumpImpulse;
            }
            ++m_schoolRipplesTaken;
        }
    }
    return count;
}

uint32_t WaterContactTracker::TakeDisturbances(double stepSeconds, WaterRippleDisturbance* out, uint32_t capacity)
{
    // Bobber rings go first: they are one-shot and few, and footprints are re-emitted every step anyway.
    uint32_t count = TakeBobberRipples(stepSeconds, out, capacity);
    count += TakeFootprints(stepSeconds, true, out + count, capacity - count);
    count += TakeFootprints(stepSeconds, false, out + count, capacity - count);
    count += TakeSplashes(out + count, capacity - count);
    count += TakeSchoolRipples(out + count, capacity - count);
    count += TakeRocks(out + count, capacity - count);
    return count + TakeTurbulence(out + count, capacity - count);
}