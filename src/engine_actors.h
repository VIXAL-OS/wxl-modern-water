#pragma once

#include <cstdint>

constexpr uint32_t kMaxWaterContacts = 32;
constexpr float kWaterContactRange = 48.0f;
constexpr uint32_t kMaxWaterBobbers = 8;
// Bobbers are read further out than units: the ripple window leans toward the player's own one.
constexpr float kWaterBobberRange = 64.0f;
// Fish schools (fishing pools) are read as far out as bobbers.
constexpr uint32_t kMaxWaterSchools = 8;

struct WaterContact
{
    uint64_t guid = 0;
    float position[3] = {};
    float surface = 0.0f;
    float radius = 0.0f;
    float height = 0.0f;
    bool swimming = false;
    bool onTransport = false;
    bool waterWalking = false; // standing on the surface (Water Walking, Path of Frost, ...)
    bool mounted = false;      // UNIT_FIELD_MOUNTDISPLAYID set: on a mount (its footsteps are bigger)
};

// A fishing bobber (game object type 17). state is GAMEOBJECT_BYTES_1 byte 0 and flags is GAMEOBJECT_FLAGS;
// the server moves both when a fish bites. own: OBJECT_FIELD_CREATED_BY is the active player.
struct WaterBobber
{
    uint64_t guid = 0;
    float position[3] = {};
    uint8_t state = 0;
    uint32_t flags = 0;
    bool own = false;
};

// A fish school: a fishing pool (game object type 25), read where the client reads its position.
struct WaterSchool
{
    uint64_t guid = 0;
    float position[3] = {};
};

struct WaterContactFrame
{
    WaterContact contacts[kMaxWaterContacts] = {};
    uint32_t count = 0;
    bool truncated = false;
    WaterBobber bobbers[kMaxWaterBobbers] = {};
    uint32_t bobberCount = 0;
    // Walk diagnostics: fishing-node game objects seen, and how many of them failed the position-slot check.
    uint32_t fishingNodes = 0;
    uint32_t rejectedNodes = 0;
    WaterSchool schools[kMaxWaterSchools] = {};
    uint32_t schoolCount = 0;
    // The active player's position (world position on a transport), when the walk found them. Contacts and bobbers
    // are then picked around the player, and the ripple window centres there: the client's "camera target" is a
    // yard in front of the camera, which a zoomed-out camera holds 10-50 yd from the player.
    bool hasPlayer = false;
    float player[3] = {};
    bool playerMounted = false; // the active player is on a mount (UNIT_FIELD_MOUNTDISPLAYID set)
};

float WaterContactDepth(const WaterContact& contact);
// Whether a water walker whose body the surface does not cross stands on it, from how far above its feet the
// client's liquid surface reads: up to 3 yd above (Stormwind's canals read 1.8), at most half a yard below.
bool WaterWalkerOnSurface(float above);
float ClientRippleDepthLimit(float height);

namespace engine
{
bool SelectWaterContact(WaterContactFrame& frame, const WaterContact& contact, const float centre[3]);
bool SelectWaterBobber(WaterContactFrame& frame, const WaterBobber& bobber, const float centre[3]);
bool SelectWaterSchool(WaterContactFrame& frame, const WaterSchool& school, const float centre[3]);
bool WaterBobbersSupported();
bool CaptureWaterContactsFrom(uintptr_t objectManager, const float centre[3], WaterContactFrame& out,
                              bool withBobbers = false);
bool WaterContactsSupported();
bool CaptureWaterContacts(const float centre[3], WaterContactFrame& out);
// The liquid query asked under a water walker the client reported no liquid for (the client's 0x007A0B00 once its
// bytes check out). Tests put in their own.
using WaterLiquidQuery = char(__cdecl*)(const float* position, uint32_t* type, float* surface, uint32_t* cleared,
                                        int flag);
void SetWaterLiquidQueryForTests(WaterLiquidQuery query);
// The active player's unit object (0: not in the world, or the client's layout differs). Main thread.
uintptr_t ActivePlayerUnit();
}
