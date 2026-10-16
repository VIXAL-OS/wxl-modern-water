#include "engine_actors.h"

#include "log.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>

namespace
{
constexpr uintptr_t kClientImageBase = 0x00400000;
constexpr uint32_t kClientTimestamp = 0x4C2452FE;
constexpr uintptr_t kClientConnection = 0x00C79CE0;
constexpr uintptr_t kConnectionObjectManager = 0x2ED0;
constexpr uintptr_t kObjectManagerSize = 0xD8;
constexpr uintptr_t kManagerVisibleLinkOffset = 0xA4;
constexpr uintptr_t kManagerVisibleHead = 0xA8;
constexpr uintptr_t kManagerVisibleFirst = 0xAC;
constexpr uint32_t kVisibleListLinkOffset = 0x38;
constexpr uintptr_t kListEndMarker = 1;
constexpr uint32_t kMaxVisibleObjects = 4096;
constexpr uintptr_t kObjectDescriptors = 0x08;
constexpr uintptr_t kObjectGuid = 0x30;
constexpr uintptr_t kObjectPreviousLink = kVisibleListLinkOffset;
constexpr uintptr_t kObjectNext = kVisibleListLinkOffset + sizeof(uintptr_t);
constexpr uintptr_t kObjectWorldEntity = 0xB8;
constexpr uintptr_t kDescriptorTypeMask = 0x08;
constexpr uint32_t kUnitTypeBit = 0x8;
constexpr uintptr_t kUnitMovement = 0xD8;
constexpr uintptr_t kUnitMovementBlock = 0x788;
constexpr uintptr_t kUnitTransportGuid = 0x790;
constexpr uintptr_t kUnitPosition = 0x798;
constexpr uintptr_t kUnitMovementFlags = 0x7CC;
constexpr uintptr_t kUnitCollisionRadius = 0x850;
constexpr uintptr_t kUnitCollisionHeight = 0x854;
constexpr uintptr_t kUnitSize = kUnitCollisionHeight + sizeof(float);
constexpr uint32_t kSwimmingMovementFlag = 0x200000;
// UNIT_FIELD_MOUNTDISPLAYID: unit field 0x3F after the object block's 6 (the client's update-field table entry at
// 0x00AC6300 says so; checked once, KMountField below), so descriptors + 0x114. Non-zero while mounted.
constexpr uintptr_t kUnitMountDisplay = (6 + 0x3F) * sizeof(uint32_t);
// The client treats liquid as ground for a unit with this flag that is not swimming (0x004F76BF, 0x0075E405).
constexpr uint32_t kWaterWalkingMovementFlag = 0x10000000;
constexpr float kWaterWalkingSurfaceYards = 0.5f;
// How far above a water walker's feet the client's liquid surface may read and still be the water it walks on. In
// Stormwind's canals it read 1.82 yd above a shaman walking on them (the session log, 2026-10-10), so the half-yard
// test passed only on frames where the body-crossing flag happened to be set, and the footstep rings came and went.
// A surface above the feet of a unit that is not swimming can only be the water under it; one below them by more
// than kWaterWalkingSurfaceYards is a dock or the shore.
constexpr float kWaterWalkingSurfaceAboveYards = 3.0f;
constexpr uintptr_t kEntityWorldPosition = 0x6C;
constexpr uintptr_t kEntityLiquidFlags = 0x7C;
constexpr uintptr_t kEntityLiquidSurface = 0x80;
constexpr uintptr_t kEntitySize = 0xC0;
constexpr uint32_t kEntityLiquidFound = 0x20;
constexpr uint32_t kEntitySurfaceCrossesBody = 0x100;
constexpr float kMaxWorldCoordinate = 100000.0f;
constexpr float kMaxCollisionRadius = 50.0f;
constexpr float kMaxCollisionHeight = 100.0f;
constexpr float kMinClientRippleDepthLimit = 1.0f;
constexpr float kClientRippleDepthLimitPerHeight = 2.0f;
constexpr size_t kMaxGuardBytes = 20;
// Game objects. Descriptor offsets follow the client's own update-field table (object block of 6 fields, then
// GAMEOBJECT_FLAGS at 3 and GAMEOBJECT_BYTES_1 at 11). The position is what vtable slot 12 (raw position) reads:
// two classes read +0xE8 outright. The class fishing bobbers use (vtable 0x00A34640, seen live) asks its per-type
// handler at +0x1A0 (virtual +0x58) when there is one; the handler fishing nodes get answers with its owner's
// +0xE8 (0x0070C2E0), so the read is accepted only when the handler's +0x58 is that function and its owner is this
// object. Each vtable is pinned by the constructors that install it.
constexpr uintptr_t kManagerActivePlayer = 0xC0;
constexpr uint32_t kGameObjectTypeBit = 0x20;
constexpr uintptr_t kGameObjectCreatedBy = 0x18;
constexpr uintptr_t kGameObjectFlags = 0x24;
constexpr uintptr_t kGameObjectState = 0x44;
constexpr uintptr_t kGameObjectType = 0x45;
constexpr uintptr_t kGameObjectDescriptorsSize = 0x48;
constexpr uintptr_t kGameObjectPosition = 0xE8;
constexpr uintptr_t kGameObjectTypeHandler = 0x1A0;
constexpr uintptr_t kGameObjectSize = kGameObjectTypeHandler + sizeof(uintptr_t);
constexpr uintptr_t kTypeHandlerOwner = 0x04;
constexpr uintptr_t kTypeHandlerRawPosition = 0x58;
constexpr uintptr_t kTypeHandlerOwnerPositionFn = 0x0070C2E0;
constexpr uintptr_t kFishingHandlerVtable = 0x00A33FD0;
constexpr uintptr_t kRawPositionSlot = 12;
constexpr uintptr_t kGameObjectVtable = 0x00A33020;
constexpr uintptr_t kGameObjectVtableSecond = 0x00A331C8;
constexpr uintptr_t kAttachableGameObjectVtable = 0x00A34640;
constexpr uintptr_t kGameObjectPositionFn = 0x00706500;
constexpr uintptr_t kAttachableGameObjectPositionFn = 0x00712D50;
constexpr uint8_t kFishingNodeType = 17;
constexpr uint8_t kFishingHoleType = 25;

struct GuardBytes
{
    const char* name;
    uintptr_t address;
    size_t size;
    unsigned char bytes[kMaxGuardBytes];
};

const GuardBytes kWaterContactLayout[] = {
    {"object manager store", 0x004D77A9, 12, {0x8B, 0x0D, 0xE0, 0x9C, 0xC7, 0x00, 0x89, 0x81, 0xD0, 0x2E, 0x00, 0x00}},
    {"visible list first object", 0x004D4B44, 14,
     {0x8B, 0x86, 0x08, 0x00, 0x00, 0x00, 0x05, 0xA8, 0x00, 0x00, 0x00, 0x8B, 0x40, 0x04}},
    {"visible list next object", 0x004D4B80, 18,
     {0x8B, 0x86, 0x08, 0x00, 0x00, 0x00, 0x05, 0xA4, 0x00, 0x00, 0x00, 0x8B, 0x00, 0x03, 0xC3, 0x8B, 0x58, 0x04}},
    {"visible list link offset", 0x004D6193, 5, {0xBA, 0x38, 0x00, 0x00, 0x00}},
    {"visible list link offset store", 0x004D6233, 6, {0x89, 0x96, 0xA4, 0x00, 0x00, 0x00}},
    {"object guid", 0x004D4B6D, 6, {0x8B, 0x4B, 0x34, 0x8B, 0x43, 0x30}},
    {"object type mask", 0x004D4DF1, 9, {0x8B, 0x48, 0x08, 0x8B, 0x55, 0x10, 0x85, 0x51, 0x08}},
    {"unit movement block", 0x0073F67A, 12, {0x8D, 0x86, 0x88, 0x07, 0x00, 0x00, 0x89, 0x86, 0xD8, 0x00, 0x00, 0x00}},
    {"unit position", 0x006E6F13, 9, {0x8B, 0x89, 0xD8, 0x00, 0x00, 0x00, 0x8B, 0x51, 0x10}},
    {"unit transport guid", 0x006E6F70, 12, {0x8B, 0x81, 0x90, 0x07, 0x00, 0x00, 0x8B, 0x91, 0x94, 0x07, 0x00, 0x00}},
    {"unit swimming flag", 0x00730DA2, 8, {0x8B, 0x40, 0x44, 0xA9, 0x00, 0x00, 0x20, 0x00}},
    {"unit water walking flag", 0x004F76B6, 20,
     {0x8B, 0x96, 0xD8, 0x00, 0x00, 0x00, 0x8B, 0x42, 0x44, 0xA9, 0x00, 0x00, 0x00, 0x10, 0xBF, 0x01, 0x00, 0x00, 0x00,
      0x74}},
    {"unit collision radius store", 0x006E95AF, 6, {0xD9, 0x9E, 0xC8, 0x00, 0x00, 0x00}},
    {"unit collision height store", 0x006E95CE, 6, {0xD9, 0x9E, 0xCC, 0x00, 0x00, 0x00}},
    {"unit ripple depth limit", 0x0071CC28, 10, {0xD9, 0x86, 0x54, 0x08, 0x00, 0x00, 0xDC, 0xC0, 0xD9, 0xE8}},
    {"object world entity store", 0x007438B0, 6, {0x89, 0x86, 0xB8, 0x00, 0x00, 0x00}},
    {"entity liquid surface", 0x0077F1EA, 12, {0xF6, 0x40, 0x7C, 0x20, 0x74, 0x27, 0xD9, 0x80, 0x80, 0x00, 0x00, 0x00}},
    {"entity liquid body crossing", 0x0077F230, 12,
     {0x8B, 0x48, 0x7C, 0x8B, 0x55, 0x0C, 0xC1, 0xE9, 0x08, 0x83, 0xE1, 0x01}},
    {"entity world position store", 0x007803BD, 12,
     {0x89, 0x79, 0x6C, 0xD8, 0x65, 0xBC, 0x89, 0x59, 0x70, 0x89, 0x51, 0x74}},
};

const GuardBytes kMountField = {"UNIT_FIELD_MOUNTDISPLAYID field", 0x00AC6300, 12,
                                 {0xD4, 0x8E, 0x9F, 0x00, 0x3F, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00}};

const GuardBytes kWaterBobberLayout[] = {
    {"game object position", 0x00706503, 20,
     {0x8B, 0x91, 0xE8, 0x00, 0x00, 0x00, 0x8B, 0x45, 0x08, 0x89, 0x10, 0x8B, 0x91, 0xEC, 0x00, 0x00, 0x00, 0x8B,
      0x89, 0xF0}},
    {"game object vtable install", 0x007053CE, 6, {0xC7, 0x06, 0x20, 0x30, 0xA3, 0x00}},
    {"game object vtable install (second)", 0x00706462, 6, {0xC7, 0x06, 0xC8, 0x31, 0xA3, 0x00}},
    {"game object position slot", kGameObjectVtable + kRawPositionSlot * sizeof(uintptr_t), 4, {0x00, 0x65, 0x70, 0x00}},
    {"game object position slot (second table)", kGameObjectVtableSecond + kRawPositionSlot * sizeof(uintptr_t), 4,
     {0x00, 0x65, 0x70, 0x00}},
    {"attachable game object vtable install", 0x00712B91, 6, {0xC7, 0x03, 0x40, 0x46, 0xA3, 0x00}},
    {"attachable game object vtable install (second)", 0x00714284, 6, {0xC7, 0x06, 0x40, 0x46, 0xA3, 0x00}},
    {"attachable game object position slot", kAttachableGameObjectVtable + kRawPositionSlot * sizeof(uintptr_t), 4,
     {0x50, 0x2D, 0x71, 0x00}},
    {"attachable game object handler test", 0x00712D56, 9, {0x83, 0xB9, 0xA0, 0x01, 0x00, 0x00, 0x00, 0x74, 0x13}},
    {"attachable game object handler call", 0x00712D5F, 15,
     {0x8B, 0x89, 0xA0, 0x01, 0x00, 0x00, 0x8B, 0x01, 0x8B, 0x40, 0x58, 0x8D, 0x55, 0xF4, 0x52}},
    {"attachable game object position", 0x00712D72, 18,
     {0x8B, 0x81, 0xEC, 0x00, 0x00, 0x00, 0x8B, 0x91, 0xE8, 0x00, 0x00, 0x00, 0x8B, 0x89, 0xF0, 0x00, 0x00, 0x00}},
    {"type handler owner position", 0x0070C2E3, 20,
     {0x8B, 0x49, 0x04, 0x8B, 0x91, 0xE8, 0x00, 0x00, 0x00, 0x8B, 0x45, 0x08, 0x81, 0xC1, 0xD0, 0x00, 0x00, 0x00, 0x89,
      0x10}},
    {"type handler owner store", 0x007124BF, 3, {0x89, 0x46, 0x04}},
    {"fishing handler vtable install", 0x00712718, 6, {0xC7, 0x06, 0xD0, 0x3F, 0xA3, 0x00}},
    {"fishing handler position slot", kFishingHandlerVtable + kTypeHandlerRawPosition, 4, {0xE0, 0xC2, 0x70, 0x00}},
    {"OBJECT_FIELD_PADDING field", 0x00AC5C98, 12, {0xF0, 0x96, 0x9F, 0x00, 0x05, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00}},
    {"OBJECT_FIELD_CREATED_BY field", 0x00AC77A8, 16,
     {0x84, 0x73, 0x9F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00}},
    {"active player guid", 0x004D37AE, 12, {0x8B, 0x81, 0xC0, 0x00, 0x00, 0x00, 0x8B, 0x91, 0xC4, 0x00, 0x00, 0x00}},
    {"GAMEOBJECT_FLAGS field", 0x00AC77D0, 12, {0x58, 0x73, 0x9F, 0x00, 0x03, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00}},
    {"GAMEOBJECT_BYTES_1 field", 0x00AC7834, 16,
     {0xEC, 0x72, 0x9F, 0x00, 0x0B, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00}},
};

template <typename T>
T Read(uintptr_t address)
{
    T value;
    std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(value));
    return value;
}

bool ValidPointer(uintptr_t address, size_t size)
{
    return address >= 0x10000 && !(address & 3) && address < 0xFFF00000 && size <= 0xFFF00000 - address;
}

bool FiniteWithin(float value, float minimum, float maximum)
{
    return std::isfinite(value) && value >= minimum && value <= maximum;
}

bool PlausibleContact(const WaterContact& c)
{
    for (float axis : c.position)
        if (!FiniteWithin(axis, -kMaxWorldCoordinate, kMaxWorldCoordinate))
            return false;
    return FiniteWithin(c.surface, -kMaxWorldCoordinate, kMaxWorldCoordinate) && c.radius > 0.0f &&
           FiniteWithin(c.radius, 0.0f, kMaxCollisionRadius) && c.height > 0.0f &&
           FiniteWithin(c.height, 0.0f, kMaxCollisionHeight) && WaterContactDepth(c) < ClientRippleDepthLimit(c.height);
}

double SquaredGroundDistance(const WaterContact& c, const float centre[3])
{
    const double dx = static_cast<double>(c.position[0]) - centre[0];
    const double dy = static_cast<double>(c.position[1]) - centre[1];
    return dx * dx + dy * dy;
}

bool Precedes(const WaterContact& a, const WaterContact& b, const float centre[3])
{
    const double aDistance = SquaredGroundDistance(a, centre);
    const double bDistance = SquaredGroundDistance(b, centre);
    if (aDistance != bDistance)
        return aDistance < bDistance;
    return a.guid < b.guid;
}

bool LayoutMatchesUnsafe()
{
    const auto* base = reinterpret_cast<const unsigned char*>(GetModuleHandleA(nullptr));
    if (reinterpret_cast<uintptr_t>(base) != kClientImageBase)
        return false;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0 || dos->e_lfanew > 0x1000)
        return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.TimeDateStamp != kClientTimestamp ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC)
        return false;
    for (const GuardBytes& guard : kWaterContactLayout)
        if (std::memcmp(reinterpret_cast<const void*>(guard.address), guard.bytes, guard.size) != 0)
        {
            VF_LOG_ERROR("water contacts: the client's %s at 0x%08X differs from the 12340 client; ripples get no "
                         "contacts and the client's splash and wake sprites stay",
                         guard.name, static_cast<unsigned>(guard.address));
            return false;
        }
    return true;
}

bool LayoutMatches()
{
    __try
    {
        return LayoutMatchesUnsafe();
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool BobberLayoutMatchesUnsafe()
{
    for (const GuardBytes& guard : kWaterBobberLayout)
        if (std::memcmp(reinterpret_cast<const void*>(guard.address), guard.bytes, guard.size) != 0)
        {
            VF_LOG_ERROR("water contacts: the client's %s at 0x%08X differs from the 12340 client; fishing bobbers "
                         "make no ripples",
                         guard.name, static_cast<unsigned>(guard.address));
            return false;
        }
    return true;
}

bool BobberLayoutMatches()
{
    __try
    {
        return BobberLayoutMatchesUnsafe();
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool PlausibleBobber(const WaterBobber& b)
{
    for (float axis : b.position)
        if (!FiniteWithin(axis, -kMaxWorldCoordinate, kMaxWorldCoordinate))
            return false;
    return true;
}

double SquaredGroundDistance(const float position[3], const float centre[3])
{
    const double dx = static_cast<double>(position[0]) - centre[0];
    const double dy = static_cast<double>(position[1]) - centre[1];
    return dx * dx + dy * dy;
}

// The player's own bobber first, then nearest first.
bool Precedes(const WaterBobber& a, const WaterBobber& b, const float centre[3])
{
    if (a.own != b.own)
        return a.own;
    const double aDistance = SquaredGroundDistance(a.position, centre);
    const double bDistance = SquaredGroundDistance(b.position, centre);
    if (aDistance != bDistance)
        return aDistance < bDistance;
    return a.guid < b.guid;
}

bool IsUnit(uintptr_t object)
{
    const uintptr_t descriptors = Read<uintptr_t>(object + kObjectDescriptors);
    return ValidPointer(descriptors, kDescriptorTypeMask + sizeof(uint32_t)) &&
           (Read<uint32_t>(descriptors + kDescriptorTypeMask) & kUnitTypeBit) && ValidPointer(object, kUnitSize) &&
           Read<uintptr_t>(object + kUnitMovement) == object + kUnitMovementBlock;
}

// Water walkers the surface test turned away, for the once-a-minute line (frames, and the surface's range).
struct WalkerRejections
{
    uint32_t frames = 0;
    float lowest = 0.0f;
    float highest = 0.0f;
};
WalkerRejections g_walkerRejections;
uint32_t g_walkerAccepted = 0;
uint32_t g_walkerHeld = 0;
uint32_t g_walkerUnreported = 0;

// Water walkers the client stopped reporting liquid under. In Stormwind's canals the client set its liquid-found flag
// on only some frames for a shaman walking on them: 1933 frames counted in two minutes of walking, none turned away
// (the session log, 2026-10-11), and the footstep rings came and went ("waterwalking ripples still only
// intermittently work in stormwind"). A water walker stands on the surface (0.00 yd above it on every frame the client
// did report it), so one still at that height is still on that water: the last surface the client reported for it
// holds until it steps up or down (a dock, the shore) or goes unseen for kWalkerForgetMs.
constexpr int kRememberedWalkers = 8;
constexpr float kWalkerHoldYards = 0.25f;
constexpr ULONGLONG kWalkerForgetMs = 3000;

struct RememberedWalker
{
    uint64_t guid = 0;
    float surface = 0.0f;
    float z = 0.0f;
    ULONGLONG seen = 0;
};
RememberedWalker g_rememberedWalkers[kRememberedWalkers];

void RememberWalker(const WaterContact& contact, ULONGLONG now)
{
    RememberedWalker* slot = &g_rememberedWalkers[0];
    for (RememberedWalker& w : g_rememberedWalkers)
    {
        if (w.guid == contact.guid)
        {
            slot = &w;
            break;
        }
        if (w.seen < slot->seen)
            slot = &w;
    }
    *slot = {contact.guid, contact.surface, contact.position[2], now};
}

// The client's own liquid query, the one its per-unit liquid update (0x007A1BC0) calls with the unit's position:
// char __cdecl (const C3Vector* position, uint32_t* type, float* surface, uint32_t* cleared, int flag), WMO liquid
// first (0x007A09D0), then the terrain's (0x007A0820). The WMO half only finds liquid around a point inside it, so a
// water walker on Stormwind's canals (WMO liquid), feet at the surface or a hair above it, was found on some frames
// and not on others; the terrain half answers for the column, which is why Wintergrasp's lake held every frame.
// Asked again half a yard under the walker's feet, it finds the canal it stands on.
using QueryLiquidFn = engine::WaterLiquidQuery;
constexpr uintptr_t kQueryLiquid = 0x007A0B00;
constexpr float kWalkerProbeYards = 0.5f;

const GuardBytes kQueryLiquidLayout[] = {
    {"liquid query", kQueryLiquid, 20,
     {0x55, 0x8B, 0xEC, 0x8B, 0x45, 0x14, 0x53, 0x8B, 0x5D, 0x08, 0x56, 0x8B, 0x75, 0x10, 0x57, 0x8B, 0x7D, 0x0C, 0x56,
      0x57}},
    {"liquid query (cleared slot, WMO liquid call)", kQueryLiquid + 20, 12,
     {0x53, 0xC7, 0x00, 0x00, 0x00, 0x00, 0x00, 0xE8, 0xB0, 0xFE, 0xFF, 0xFF}},
    {"unit liquid update's query call", 0x007A1BF6, 20,
     {0x6A, 0x01, 0x8D, 0x4D, 0xF8, 0x51, 0x8D, 0xBE, 0x80, 0x00, 0x00, 0x00, 0x57, 0x8D, 0x55, 0x08, 0x52, 0x8D, 0x46,
      0x6C}},
    {"unit liquid update's query call (call, cdecl cleanup)", 0x007A1BF6 + 20, 9,
     {0x50, 0xE8, 0xF0, 0xEE, 0xFF, 0xFF, 0x83, 0xC4, 0x14}},
};

QueryLiquidFn g_queryLiquid = nullptr;
bool g_queryLiquidChecked = false;
uint32_t g_walkerProbed = 0;

QueryLiquidFn LiquidQuery()
{
    if (g_queryLiquidChecked)
        return g_queryLiquid;
    g_queryLiquidChecked = true;
    if (!engine::WaterContactsSupported())
        return nullptr;
    for (const GuardBytes& guard : kQueryLiquidLayout)
        if (std::memcmp(reinterpret_cast<const void*>(guard.address), guard.bytes, guard.size) != 0)
        {
            VF_LOG_ERROR("water walking: the client's %s at 0x%08X differs from the 12340 client; walkers on WMO "
                         "liquid keep only the surfaces the client reports",
                         guard.name, static_cast<unsigned>(guard.address));
            return nullptr;
        }
    g_queryLiquid = reinterpret_cast<QueryLiquidFn>(kQueryLiquid);
    return g_queryLiquid;
}

bool ProbeWalker(WaterContact& contact)
{
    const QueryLiquidFn query = LiquidQuery();
    if (!query)
        return false;
    const float probe[3] = {contact.position[0], contact.position[1], contact.position[2] - kWalkerProbeYards};
    uint32_t type = 0, cleared = 0;
    float surface = 0.0f;
    if (!query(probe, &type, &surface, &cleared, 1) || !std::isfinite(surface) ||
        !WaterWalkerOnSurface(surface - contact.position[2]))
        return false;
    contact.surface = surface;
    ++g_walkerProbed;
    return true;
}

bool HoldWalker(WaterContact& contact, ULONGLONG now)
{
    for (RememberedWalker& w : g_rememberedWalkers)
        if (w.guid == contact.guid && w.seen && now - w.seen <= kWalkerForgetMs &&
            std::fabs(contact.position[2] - w.z) <= kWalkerHoldYards)
        {
            w.seen = now;
            contact.surface = w.surface;
            ++g_walkerHeld;
            return true;
        }
    ++g_walkerUnreported;
    return false;
}

// Whether the mount field is where the 12340 client keeps it (checked once, in the client only).
bool MountFieldKnown()
{
    static const bool known = [] {
        if (!engine::WaterContactsSupported())
            return false;
        __try
        {
            return std::memcmp(reinterpret_cast<const void*>(kMountField.address), kMountField.bytes, kMountField.size) == 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }();
    return known;
}

bool ReadWaterContactUnsafe(uintptr_t object, WaterContact& contact)
{
    if (!IsUnit(object))
        return false;
    const uintptr_t entity = Read<uintptr_t>(object + kObjectWorldEntity);
    if (!ValidPointer(entity, kEntitySize))
        return false;
    const uint32_t liquid = Read<uint32_t>(entity + kEntityLiquidFlags);
    const uint32_t movement = Read<uint32_t>(object + kUnitMovementFlags);
    const bool waterWalking = (movement & kWaterWalkingMovementFlag) && !(movement & kSwimmingMovementFlag);
    const bool found = (liquid & kEntityLiquidFound) != 0;
    // A water walker stands on the surface, so the surface need not cross its body.
    if (!waterWalking && !(found && (liquid & kEntitySurfaceCrossesBody)))
        return false;
    contact.guid = Read<uint64_t>(object + kObjectGuid);
    contact.onTransport = Read<uint64_t>(object + kUnitTransportGuid) != 0;
    const uintptr_t position = contact.onTransport ? entity + kEntityWorldPosition : object + kUnitPosition;
    std::memcpy(contact.position, reinterpret_cast<const void*>(position), sizeof(contact.position));
    contact.radius = Read<float>(object + kUnitCollisionRadius);
    contact.height = Read<float>(object + kUnitCollisionHeight);
    contact.swimming = (movement & kSwimmingMovementFlag) != 0;
    contact.waterWalking = waterWalking;
    const uintptr_t descriptors = Read<uintptr_t>(object + kObjectDescriptors);
    contact.mounted = MountFieldKnown() && ValidPointer(descriptors, kUnitMountDisplay + sizeof(uint32_t)) &&
                      Read<uint32_t>(descriptors + kUnitMountDisplay) != 0;
    const ULONGLONG now = GetTickCount64();
    if (!found)
    {
        if (!ProbeWalker(contact))
            return HoldWalker(contact, now);
        RememberWalker(contact, now);
        return true;
    }
    contact.surface = Read<float>(entity + kEntityLiquidSurface);
    // On land beside water the flag is still set; without a crossing surface only a unit actually on it counts.
    if (waterWalking && !(liquid & kEntitySurfaceCrossesBody))
    {
        const float above = contact.surface - contact.position[2];
        if (!WaterWalkerOnSurface(above))
        {
            WalkerRejections& r = g_walkerRejections;
            r.lowest = r.frames ? std::min(r.lowest, above) : above;
            r.highest = r.frames ? std::max(r.highest, above) : above;
            ++r.frames;
            return false;
        }
    }
    if (waterWalking)
    {
        ++g_walkerAccepted;
        RememberWalker(contact, now);
    }
    return true;
}

enum class BobberRead
{
    Other,
    Rejected,
    Accepted,
};

bool g_loggedRejectedBobber = false;

void LogRejectedBobber(uintptr_t object, uintptr_t vtable)
{
    if (g_loggedRejectedBobber)
        return;
    g_loggedRejectedBobber = true;
    const bool readable = ValidPointer(vtable, (kRawPositionSlot + 1) * sizeof(uintptr_t));
    const uintptr_t handler = Read<uintptr_t>(object + kGameObjectTypeHandler);
    const uintptr_t handlerVtable =
        ValidPointer(handler, kTypeHandlerOwner + sizeof(uintptr_t)) ? Read<uintptr_t>(handler) : 0;
    const bool handlerReadable = ValidPointer(handlerVtable, kTypeHandlerRawPosition + sizeof(uintptr_t));
    VF_LOG_INFO("water bobbers: a fishing node at 0x%08X was skipped: vtable 0x%08X, slot 11 0x%08X, slot 12 0x%08X, "
                "handler 0x%08X (vtable 0x%08X, +0x58 0x%08X, owner 0x%08X)",
                static_cast<unsigned>(object), static_cast<unsigned>(vtable),
                readable ? static_cast<unsigned>(Read<uintptr_t>(vtable + 11 * sizeof(uintptr_t))) : 0u,
                readable ? static_cast<unsigned>(Read<uintptr_t>(vtable + 12 * sizeof(uintptr_t))) : 0u,
                static_cast<unsigned>(handler), static_cast<unsigned>(handlerVtable),
                handlerReadable ? static_cast<unsigned>(Read<uintptr_t>(handlerVtable + kTypeHandlerRawPosition)) : 0u,
                handlerVtable ? static_cast<unsigned>(Read<uintptr_t>(handler + kTypeHandlerOwner)) : 0u);
}

// The attachable class answers from its type handler when it has one; accept it only when that handler answers
// with this object's own +0xE8.
bool HandlerReadsOwnPosition(uintptr_t object)
{
    const uintptr_t handler = Read<uintptr_t>(object + kGameObjectTypeHandler);
    if (!handler)
        return true;
    if (!ValidPointer(handler, kTypeHandlerOwner + sizeof(uintptr_t)))
        return false;
    const uintptr_t vtable = Read<uintptr_t>(handler);
    return ValidPointer(vtable, kTypeHandlerRawPosition + sizeof(uintptr_t)) &&
           Read<uintptr_t>(vtable + kTypeHandlerRawPosition) == kTypeHandlerOwnerPositionFn &&
           Read<uintptr_t>(handler + kTypeHandlerOwner) == object;
}

// The type bit, the fishing-node type byte and the position slot must all agree before the position is read: the
// slot proves +0xE8 is where the client itself reads this object's position.
BobberRead ReadWaterBobberUnsafe(uintptr_t object, uint64_t activePlayer, WaterBobber& bobber)
{
    const uintptr_t descriptors = Read<uintptr_t>(object + kObjectDescriptors);
    if (!ValidPointer(descriptors, kGameObjectDescriptorsSize) ||
        !(Read<uint32_t>(descriptors + kDescriptorTypeMask) & kGameObjectTypeBit) ||
        !ValidPointer(object, kGameObjectSize) || Read<uint8_t>(descriptors + kGameObjectType) != kFishingNodeType)
        return BobberRead::Other;
    const uintptr_t vtable = Read<uintptr_t>(object);
    const uintptr_t position = ValidPointer(vtable, (kRawPositionSlot + 1) * sizeof(uintptr_t))
                                   ? Read<uintptr_t>(vtable + kRawPositionSlot * sizeof(uintptr_t))
                                   : 0;
    const bool readsOwnPosition = position == kGameObjectPositionFn ||
                                  (position == kAttachableGameObjectPositionFn && HandlerReadsOwnPosition(object));
    if (!readsOwnPosition)
    {
        LogRejectedBobber(object, vtable);
        return BobberRead::Rejected;
    }
    bobber.guid = Read<uint64_t>(object + kObjectGuid);
    std::memcpy(bobber.position, reinterpret_cast<const void*>(object + kGameObjectPosition), sizeof(bobber.position));
    bobber.state = Read<uint8_t>(descriptors + kGameObjectState);
    bobber.flags = Read<uint32_t>(descriptors + kGameObjectFlags);
    bobber.own = activePlayer && Read<uint64_t>(descriptors + kGameObjectCreatedBy) == activePlayer;
    return BobberRead::Accepted;
}

// A fish school (a fishing pool, game object type 25): the same checks as a bobber's before its position is read.
bool ReadWaterSchoolUnsafe(uintptr_t object, WaterSchool& school)
{
    const uintptr_t descriptors = Read<uintptr_t>(object + kObjectDescriptors);
    if (!ValidPointer(descriptors, kGameObjectDescriptorsSize) ||
        !(Read<uint32_t>(descriptors + kDescriptorTypeMask) & kGameObjectTypeBit) ||
        !ValidPointer(object, kGameObjectSize) || Read<uint8_t>(descriptors + kGameObjectType) != kFishingHoleType)
        return false;
    const uintptr_t vtable = Read<uintptr_t>(object);
    const uintptr_t position = ValidPointer(vtable, (kRawPositionSlot + 1) * sizeof(uintptr_t))
                                   ? Read<uintptr_t>(vtable + kRawPositionSlot * sizeof(uintptr_t))
                                   : 0;
    if (!(position == kGameObjectPositionFn ||
          (position == kAttachableGameObjectPositionFn && HandlerReadsOwnPosition(object))))
        return false;
    school.guid = Read<uint64_t>(object + kObjectGuid);
    std::memcpy(school.position, reinterpret_cast<const void*>(object + kGameObjectPosition), sizeof(school.position));
    return true;
}

bool WalkVisibleUnitsUnsafe(uintptr_t manager, const float centre[3], bool withBobbers, WaterContactFrame& out)
{
    if (!ValidPointer(manager, kObjectManagerSize) ||
        Read<uint32_t>(manager + kManagerVisibleLinkOffset) != kVisibleListLinkOffset)
        return false;
    uintptr_t previousLink = manager + kManagerVisibleHead;
    uintptr_t object = Read<uintptr_t>(manager + kManagerVisibleFirst);
    const uint64_t activePlayer = withBobbers ? Read<uint64_t>(manager + kManagerActivePlayer) : 0;
    for (uint32_t visited = 0; object && !(object & kListEndMarker); ++visited)
    {
        if (visited == kMaxVisibleObjects)
        {
            out.truncated = true;
            return true;
        }
        if (!ValidPointer(object, kObjectNext + sizeof(uintptr_t)) ||
            Read<uintptr_t>(object + kObjectPreviousLink) != previousLink)
            return false;
        WaterContact contact;
        WaterBobber bobber;
        if (ReadWaterContactUnsafe(object, contact))
            engine::SelectWaterContact(out, contact, centre);
        else if (withBobbers)
        {
            const BobberRead read = ReadWaterBobberUnsafe(object, activePlayer, bobber);
            out.fishingNodes += read != BobberRead::Other;
            out.rejectedNodes += read == BobberRead::Rejected;
            WaterSchool school;
            if (read == BobberRead::Accepted)
                engine::SelectWaterBobber(out, bobber, centre);
            else if (read == BobberRead::Other && ReadWaterSchoolUnsafe(object, school))
                engine::SelectWaterSchool(out, school, centre);
        }
        previousLink = object + kObjectPreviousLink;
        object = Read<uintptr_t>(object + kObjectNext);
    }
    return true;
}

bool WalkVisibleUnits(uintptr_t manager, const float centre[3], bool withBobbers, WaterContactFrame& out)
{
    __try
    {
        return WalkVisibleUnitsUnsafe(manager, centre, withBobbers, out);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool ReadObjectManager(uintptr_t& manager)
{
    __try
    {
        manager = 0;
        const uintptr_t connection = Read<uintptr_t>(kClientConnection);
        if (!connection)
            return true;
        if (!ValidPointer(connection, kConnectionObjectManager + sizeof(uintptr_t)))
            return false;
        manager = Read<uintptr_t>(connection + kConnectionObjectManager);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool ValidCentre(const float centre[3])
{
    for (int i = 0; i < 3; ++i)
        if (!FiniteWithin(centre[i], -kMaxWorldCoordinate, kMaxWorldCoordinate))
            return false;
    return true;
}

// The active player's unit in the visible list (0: not there, or the list doesn't check out).
uintptr_t ActivePlayerObjectUnsafe(uintptr_t manager)
{
    if (!ValidPointer(manager, kObjectManagerSize) ||
        Read<uint32_t>(manager + kManagerVisibleLinkOffset) != kVisibleListLinkOffset)
        return 0;
    const uint64_t activePlayer = Read<uint64_t>(manager + kManagerActivePlayer);
    if (!activePlayer)
        return 0;
    uintptr_t previousLink = manager + kManagerVisibleHead;
    uintptr_t object = Read<uintptr_t>(manager + kManagerVisibleFirst);
    for (uint32_t visited = 0; object && !(object & kListEndMarker) && visited < kMaxVisibleObjects; ++visited)
    {
        if (!ValidPointer(object, kObjectNext + sizeof(uintptr_t)) ||
            Read<uintptr_t>(object + kObjectPreviousLink) != previousLink)
            return 0;
        if (Read<uint64_t>(object + kObjectGuid) == activePlayer)
            return IsUnit(object) ? object : 0;
        previousLink = object + kObjectPreviousLink;
        object = Read<uintptr_t>(object + kObjectNext);
    }
    return 0;
}

// The active player's position, read where ReadWaterContactUnsafe reads a unit's position.
bool FindActivePlayerUnsafe(uintptr_t manager, float position[3], bool& mounted)
{
    mounted = false;
    const uintptr_t object = ActivePlayerObjectUnsafe(manager);
    if (!object)
        return false;
    uintptr_t at = object + kUnitPosition;
    if (Read<uint64_t>(object + kUnitTransportGuid) != 0)
    {
        const uintptr_t entity = Read<uintptr_t>(object + kObjectWorldEntity);
        if (!ValidPointer(entity, kEntitySize))
            return false;
        at = entity + kEntityWorldPosition;
    }
    std::memcpy(position, reinterpret_cast<const void*>(at), 3 * sizeof(float));
    const uintptr_t descriptors = Read<uintptr_t>(object + kObjectDescriptors);
    mounted = MountFieldKnown() && ValidPointer(descriptors, kUnitMountDisplay + sizeof(uint32_t)) &&
              Read<uint32_t>(descriptors + kUnitMountDisplay) != 0;
    return ValidCentre(position);
}

uintptr_t ActivePlayerObject(uintptr_t manager)
{
    __try
    {
        return ActivePlayerObjectUnsafe(manager);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 0;
    }
}

bool FindActivePlayer(uintptr_t manager, float position[3], bool& mounted)
{
    __try
    {
        return FindActivePlayerUnsafe(manager, position, mounted);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

const char* FocusName(const WaterContactFrame& frame)
{
    return frame.hasPlayer ? "you" : "the camera target";
}

// Once a minute while anyone water walks: how many frames counted (and how many of those the client reported no
// liquid for, held on the last surface it did), how many the surface test turned away, and how many had no liquid
// reported and nothing to hold (on land, or never reported on water).
void LogWalkerRejections()
{
    constexpr ULONGLONG kIntervalMs = 60000;
    static ULONGLONG last = 0;
    const ULONGLONG now = GetTickCount64();
    if (now - last < kIntervalMs)
        return;
    last = now;
    const WalkerRejections r = g_walkerRejections;
    if (r.frames || g_walkerAccepted || g_walkerHeld || g_walkerProbed)
        VF_LOG_INFO("water walking: %u frames on the surface, %u more found by asking under the feet, %u held on the "
                    "last surface; %u turned away (surface %.2f to %.2f yd above the feet), %u with no liquid found",
                    g_walkerAccepted, g_walkerProbed, g_walkerHeld, r.frames, r.lowest, r.highest, g_walkerUnreported);
    g_walkerRejections = {};
    g_walkerAccepted = 0;
    g_walkerHeld = 0;
    g_walkerUnreported = 0;
    g_walkerProbed = 0;
}

// The first few water walkers of a session, so a log shows whether the flag and the surface test hold live.
void LogWaterWalkers(const WaterContactFrame& frame, const float centre[3])
{
    constexpr int kLoggedWaterWalkers = 5;
    static int logged = 0;
    static uint64_t loggedGuids[kLoggedWaterWalkers] = {};
    for (uint32_t i = 0; i < frame.count && logged < kLoggedWaterWalkers; ++i)
    {
        const WaterContact& c = frame.contacts[i];
        if (!c.waterWalking || std::find(loggedGuids, loggedGuids + logged, c.guid) != loggedGuids + logged)
            continue;
        loggedGuids[logged++] = c.guid;
        VF_LOG_INFO("water walking: unit %016llX on the surface at %.1f, %.1f (%.2f yd above it, %.1f yd from %s)",
                    static_cast<unsigned long long>(c.guid), c.position[0], c.position[1], -WaterContactDepth(c),
                    std::sqrt(SquaredGroundDistance(c, centre)), FocusName(frame));
    }
}

// One line when the nearest bobber appears and one per state or flag change, so a session log shows whether
// bobbers are read and whether the bite is visible to the client.
void LogBobberEvents(const WaterContactFrame& frame, const float centre[3])
{
    static uint64_t loggedGuid = 0;
    static uint8_t loggedState = 0;
    static uint32_t loggedFlags = 0;
    if (!frame.bobberCount)
        return;
    const WaterBobber& b = frame.bobbers[0];
    const double distance = std::sqrt(SquaredGroundDistance(b.position, centre));
    if (b.guid != loggedGuid)
        VF_LOG_INFO("water bobber %016llX (%s) at %.1f, %.1f, %.1f (%.1f yd from %s): state %u, flags 0x%X",
                    static_cast<unsigned long long>(b.guid), b.own ? "yours" : "another player's", b.position[0],
                    b.position[1], b.position[2], distance, FocusName(frame), b.state, b.flags);
    else if (b.state != loggedState || b.flags != loggedFlags)
        VF_LOG_INFO("water bobber %016llX: state %u -> %u, flags 0x%X -> 0x%X", static_cast<unsigned long long>(b.guid),
                    loggedState, b.state, loggedFlags, b.flags);
    loggedGuid = b.guid;
    loggedState = b.state;
    loggedFlags = b.flags;
}
}

bool WaterWalkerOnSurface(float above)
{
    return above >= -kWaterWalkingSurfaceYards && above <= kWaterWalkingSurfaceAboveYards;
}

float WaterContactDepth(const WaterContact& contact)
{
    return contact.surface - contact.position[2];
}

float ClientRippleDepthLimit(float height)
{
    return std::max(kMinClientRippleDepthLimit, kClientRippleDepthLimitPerHeight * height);
}

namespace engine
{
bool SelectWaterContact(WaterContactFrame& frame, const WaterContact& contact, const float centre[3])
{
    if (frame.count > kMaxWaterContacts || !ValidCentre(centre) || !PlausibleContact(contact) ||
        SquaredGroundDistance(contact, centre) > static_cast<double>(kWaterContactRange) * kWaterContactRange)
        return false;
    uint32_t index = 0;
    while (index < frame.count && !Precedes(contact, frame.contacts[index], centre))
        ++index;
    if (index == kMaxWaterContacts)
        return false;
    const uint32_t count = std::min(frame.count + 1, kMaxWaterContacts);
    for (uint32_t i = count - 1; i > index; --i)
        frame.contacts[i] = frame.contacts[i - 1];
    frame.contacts[index] = contact;
    frame.count = count;
    return true;
}

// The nearest schools first, like the bobbers.
bool SelectWaterSchool(WaterContactFrame& frame, const WaterSchool& school, const float centre[3])
{
    for (float axis : school.position)
        if (!FiniteWithin(axis, -kMaxWorldCoordinate, kMaxWorldCoordinate))
            return false;
    if (frame.schoolCount > kMaxWaterSchools || !ValidCentre(centre))
        return false;
    const double distance = SquaredGroundDistance(school.position, centre);
    if (distance > static_cast<double>(kWaterBobberRange) * kWaterBobberRange)
        return false;
    uint32_t index = 0;
    while (index < frame.schoolCount && SquaredGroundDistance(frame.schools[index].position, centre) <= distance)
        ++index;
    if (index == kMaxWaterSchools)
        return false;
    const uint32_t count = std::min(frame.schoolCount + 1, kMaxWaterSchools);
    for (uint32_t i = count - 1; i > index; --i)
        frame.schools[i] = frame.schools[i - 1];
    frame.schools[index] = school;
    frame.schoolCount = count;
    return true;
}

bool SelectWaterBobber(WaterContactFrame& frame, const WaterBobber& bobber, const float centre[3])
{
    if (frame.bobberCount > kMaxWaterBobbers || !ValidCentre(centre) || !PlausibleBobber(bobber) ||
        SquaredGroundDistance(bobber.position, centre) > static_cast<double>(kWaterBobberRange) * kWaterBobberRange)
        return false;
    uint32_t index = 0;
    while (index < frame.bobberCount && !Precedes(bobber, frame.bobbers[index], centre))
        ++index;
    if (index == kMaxWaterBobbers)
        return false;
    const uint32_t count = std::min(frame.bobberCount + 1, kMaxWaterBobbers);
    for (uint32_t i = count - 1; i > index; --i)
        frame.bobbers[i] = frame.bobbers[i - 1];
    frame.bobbers[index] = bobber;
    frame.bobberCount = count;
    return true;
}

bool CaptureWaterContactsFrom(uintptr_t objectManager, const float centre[3], WaterContactFrame& out, bool withBobbers)
{
    out = {};
    if (!ValidCentre(centre))
        return false;
    if (!objectManager)
        return true;
    // Around the player when the walk can name them (the active player's GUID is checked with the bobber layout).
    float player[3] = {};
    bool mounted = false;
    const bool hasPlayer = withBobbers && FindActivePlayer(objectManager, player, mounted);
    WaterContactFrame captured;
    const bool valid = WalkVisibleUnits(objectManager, hasPlayer ? player : centre, withBobbers, captured);
    if (valid)
    {
        out = captured;
        out.hasPlayer = hasPlayer;
        out.playerMounted = hasPlayer && mounted;
        std::copy(player, player + 3, out.player);
    }
    return valid;
}

bool WaterContactsSupported()
{
    static const bool supported = LayoutMatches();
    return supported;
}

bool WaterBobbersSupported()
{
    static const bool supported = [] {
        const bool matches = WaterContactsSupported() && BobberLayoutMatches();
        VF_LOG_INFO("water bobbers: %s", matches ? "client layout verified, fishing bobbers make ripples"
                                                 : "off (client layout differs)");
        return matches;
    }();
    return supported;
}

void SetWaterLiquidQueryForTests(WaterLiquidQuery query)
{
    g_queryLiquid = query;
    g_queryLiquidChecked = true;
}

uintptr_t ActivePlayerUnit()
{
    uintptr_t manager = 0;
    if (!WaterContactsSupported() || !ReadObjectManager(manager) || !manager)
        return 0;
    return ActivePlayerObject(manager);
}

bool CaptureWaterContacts(const float centre[3], WaterContactFrame& out)
{
    out = {};
    uintptr_t manager = 0;
    if (!WaterContactsSupported() || !ValidCentre(centre) || !ReadObjectManager(manager))
        return false;
    if (!CaptureWaterContactsFrom(manager, centre, out, WaterBobbersSupported()))
        return false;
    uintptr_t after = 0;
    if (ReadObjectManager(after) && after == manager)
    {
        const float* focus = out.hasPlayer ? out.player : centre;
        LogBobberEvents(out, focus);
        LogWaterWalkers(out, focus);
        LogWalkerRejections();
        return true;
    }
    out = {};
    return false;
}
}