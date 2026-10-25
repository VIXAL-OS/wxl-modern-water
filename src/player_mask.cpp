// The exact player mask: which pixels of the frame are the player's own.
//
// The water's screen-space reflections look past the player when they are well above the water: the screen holds only
// their front, so a reflected ray passing behind them must not stop there (vw_reflection.hlsli). Which pixels were
// theirs was guessed from shapes round where they are -- a sphere for the body, a slab for a mount's wings -- and a
// wide drake's wing tips reached past any shape that didn't also swallow the scenery beside it, so far water showed a
// stripe of wing ("ARGH STILL DRAKE WING PROBLEMS", Terokkar, the user, 2026-10-16: the drake nowhere near the water).
//
// Here every batch of the player's model family is drawn a second time, right after the client draws it, into a mask
// target cleared to 1 each frame: the client's own shaders and geometry, the client's depth test (the batch's own depth
// passes, scenery in front of the player stays out), blended to 0 whatever the shader returns, and a see-through batch
// (blend mode 2, a wing's membrane) alpha-tested at 128 as wxl-seyris-living-azeroth's PlayerDepth writes its depth.
// Additive batches (glows) and a faded player (stealth, a ghost) write no depth and are left out. The depth copy
// (vw_linear_depth.hlsl) carries the mask beside the depth.
//
// The player's model family: the root of an instance's attachment chain (kInstParent) is the root of the player's body
// model -- the rider, the mount, the weapons and cloak. Batches drawn to targets of another size (the client's shadow
// maps, portraits) are left out. Every state changed is read before and put back after, so the client's state cache
// never sees the draw.
#include "player_mask.h"

#include "d3d9_wrap.h"
#include "engine_actors.h"
#include "log.h"

#include "wxl/PluginApi.h"
#include "game/Gx.hpp"

#include <windows.h>

#include <cstdint>

namespace player_mask
{
namespace
{
// Client fields (wxl-core offsets: engine/Gx.hpp, game/M2.hpp, game/Unit.hpp).
constexpr uintptr_t kCtxInstance = 0x60;   // M2 draw context -> the instance drawing
constexpr uintptr_t kCtxMaterial = 0x98;   // -> the batch's live material
constexpr uintptr_t kMaterialBlend = 0x02; // material -> blend mode, uint16: 0 opaque, 1 alpha key, 2 alpha, 3+ add/mod
constexpr uintptr_t kInstParent = 0x48;    // instance -> parent instance, null at the root
constexpr uintptr_t kInstAlpha = 0x17C;    // instance -> its own alpha, float
constexpr uintptr_t kUnitModel = 0xB4;     // unit -> body model
constexpr int kBlendAlpha = 2;
constexpr DWORD kSolidAlpha = 128;         // a see-through batch's solid part
constexpr D3DCOLOR kNotPlayer = 0xFFFFFFFF;

using DrawBatchFn = void(__fastcall*)(void* ctx, void* edx);
using DeviceDrawFn = void(__fastcall*)(void* device, void* edx, uint32_t* batch, int indexed);
DrawBatchFn g_origBatch = nullptr;
DeviceDrawFn g_origDraw = nullptr;
// gxApi "D3D9Ex" draws through CGxDeviceD3d9Ex's own copy of the device draw (0x6A77C0, the core's Gx.DeviceDrawEx);
// gxApi "D3D9" through CGxDeviceD3d's (0x6A3620, Gx.DeviceDraw). Both are hooked, the same work behind each.
DeviceDrawFn g_origDrawEx = nullptr;
bool g_hooked = false;
bool g_hookedEx = false;

void* g_playerRoot = nullptr; // this frame's root of the player's model family (compared, never read through)
void* g_drawCtx = nullptr;    // the draw context of the player's batch drawing now (inside M2.DrawBatch)

IDirect3DDevice9* g_device = nullptr; // the device the mask belongs to (not held)
IDirect3DTexture9* g_mask = nullptr;
IDirect3DSurface9* g_maskSurface = nullptr;
UINT g_width = 0, g_height = 0;
bool g_createFailed = false;          // creating the mask failed on g_device: not tried again until a reset
uint32_t g_frame = 1;
uint32_t g_drawnFrame = 0;            // the frame the mask was cleared and drawn in
uint32_t g_sizedFrame = 0;            // the frame the back buffer's size was read in
IDirect3DDevice9* g_sizedDevice = nullptr;
UINT g_frameWidth = 0, g_frameHeight = 0;
bool g_loggedFirst = false;

// The render states the mask draw sets, read before and put back after.
constexpr D3DRENDERSTATETYPE kStates[] = {
    D3DRS_ZENABLE,          D3DRS_ZWRITEENABLE,     D3DRS_ZFUNC,           D3DRS_STENCILENABLE,
    D3DRS_ALPHABLENDENABLE, D3DRS_SRCBLEND,         D3DRS_DESTBLEND,       D3DRS_BLENDOP,
    D3DRS_SEPARATEALPHABLENDENABLE,                 D3DRS_COLORWRITEENABLE, D3DRS_SRGBWRITEENABLE,
    D3DRS_ALPHATESTENABLE,  D3DRS_ALPHAFUNC,        D3DRS_ALPHAREF,        D3DRS_SCISSORTESTENABLE,
};
constexpr int kStateCount = sizeof(kStates) / sizeof(kStates[0]);
constexpr int kScissorState = kStateCount - 1;

bool ReadPtr(uintptr_t at, void*& out)
{
    __try
    {
        out = *reinterpret_cast<void* const*>(at);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        out = nullptr;
        return false;
    }
}

bool ReadU16(uintptr_t at, uint16_t& out)
{
    __try
    {
        out = *reinterpret_cast<const uint16_t*>(at);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        out = 0;
        return false;
    }
}

bool ReadFloat(uintptr_t at, float& out)
{
    __try
    {
        out = *reinterpret_cast<const float*>(at);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        out = 0.0f;
        return false;
    }
}

void* Root(void* instance)
{
    for (int i = 0; instance && i < 8; ++i)
    {
        void* parent = nullptr;
        if (!ReadPtr(reinterpret_cast<uintptr_t>(instance) + kInstParent, parent) || !parent)
            return instance;
        instance = parent;
    }
    return instance;
}

template <typename T> void SafeRelease(T*& p)
{
    if (p)
        p->Release();
    p = nullptr;
}

// The frame's own target size: the back buffer's, read once a frame.
bool FrameSized(IDirect3DDevice9* dev, UINT width, UINT height)
{
    if (g_sizedFrame != g_frame || g_sizedDevice != dev)
    {
        g_sizedFrame = g_frame;
        g_sizedDevice = dev;
        g_frameWidth = g_frameHeight = 0;
        IDirect3DSurface9* back = nullptr;
        if (SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back)) && back)
        {
            D3DSURFACE_DESC desc = {};
            if (SUCCEEDED(back->GetDesc(&desc)))
            {
                g_frameWidth = desc.Width;
                g_frameHeight = desc.Height;
            }
            back->Release();
        }
    }
    return width == g_frameWidth && height == g_frameHeight;
}

bool EnsureMask(IDirect3DDevice9* dev, UINT width, UINT height)
{
    if (dev != g_device)
    {
        // Another device: the old one's mask went with it (released at its reset or loss), never touched again.
        g_mask = nullptr;
        g_maskSurface = nullptr;
        g_device = dev;
        g_createFailed = false;
        g_drawnFrame = 0;
    }
    if (g_mask && g_width == width && g_height == height)
        return true;
    if (g_createFailed)
        return false;
    SafeRelease(g_maskSurface);
    SafeRelease(g_mask);
    g_drawnFrame = 0;
    if (FAILED(dev->CreateTexture(width, height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &g_mask,
                                  nullptr)) ||
        !g_mask || FAILED(g_mask->GetSurfaceLevel(0, &g_maskSurface)) || !g_maskSurface)
    {
        SafeRelease(g_maskSurface);
        SafeRelease(g_mask);
        g_createFailed = true;
        VF_LOG_ERROR("player mask: the %ux%u mask could not be created; the reflections guess the player's shape", width,
                     height);
        return false;
    }
    g_width = width;
    g_height = height;
    return true;
}

struct Redraw
{
    DeviceDrawFn original; // the device class's own draw (D3D9 or D3D9Ex)
    void* device;
    void* edx;
    uint32_t* batch;
    int indexed;
};

void __cdecl DrawAgain(void* user)
{
    const Redraw& r = *static_cast<const Redraw*>(user);
    r.original(r.device, r.edx, r.batch, r.indexed);
}

void __fastcall hkBatch(void* ctx, void* edx)
{
    void* previous = g_drawCtx;
    void* instance = nullptr;
    g_drawCtx = g_playerRoot && ctx && ReadPtr(reinterpret_cast<uintptr_t>(ctx) + kCtxInstance, instance) && instance &&
                        Root(instance) == g_playerRoot
                    ? ctx
                    : nullptr;
    g_origBatch(ctx, edx);
    g_drawCtx = previous;
}

void DrawThenMask(DeviceDrawFn original, void* device, void* edx, uint32_t* batch, int indexed)
{
    original(device, edx, batch, indexed);
    if (!g_drawCtx)
        return;
    const uintptr_t ctx = reinterpret_cast<uintptr_t>(g_drawCtx);
    void* material = nullptr;
    uint16_t blend = 0;
    if (!ReadPtr(ctx + kCtxMaterial, material) || !material ||
        !ReadU16(reinterpret_cast<uintptr_t>(material) + kMaterialBlend, blend) || blend > kBlendAlpha)
        return;
    void* instance = nullptr;
    float alpha = 1.0f;
    if (ReadPtr(ctx + kCtxInstance, instance) && instance &&
        ReadFloat(reinterpret_cast<uintptr_t>(instance) + kInstAlpha, alpha) && !(alpha >= 0.99f))
        return;
    // The device the water draws on: the real one behind this module's wrapper, when the client draws through it.
    void* raw = wxl::game::gx::RawDevice();
    auto* dev = static_cast<IDirect3DDevice9*>(raw);
    if (FogDevice* wrapper = WrapperOrLatestFogDevice(raw); wrapper && IsWrapperOf(wrapper, raw))
        dev = RealDevice(wrapper);
    if (!dev)
        return;
    Redraw again{original, device, edx, batch, indexed};
    Draw(dev, blend, &DrawAgain, &again);
}

void __fastcall hkDraw(void* device, void* edx, uint32_t* batch, int indexed)
{
    DrawThenMask(g_origDraw, device, edx, batch, indexed);
}

void __fastcall hkDrawEx(void* device, void* edx, uint32_t* batch, int indexed)
{
    DrawThenMask(g_origDrawEx, device, edx, batch, indexed);
}
}

bool Install(const WXL_Api* api)
{
    if (!api || !api->HookAttachByName)
    {
        VF_LOG_INFO("player mask: off (no hook interface); the reflections guess the player's shape");
        return false;
    }
    auto attach = [&](const char* point, void* detour, void** original) {
        return api->HookAttachByName(point, detour, original, WXL_HOOK_DEFAULT_PRIORITY) != 0 ||
               api->HookAttachByName(point, detour, original, 1000) != 0;
    };
    const bool batch = attach("M2.DrawBatch", reinterpret_cast<void*>(&hkBatch), reinterpret_cast<void**>(&g_origBatch));
    const bool draw =
        batch && attach("Gx.DeviceDraw", reinterpret_cast<void*>(&hkDraw), reinterpret_cast<void**>(&g_origDraw));
    g_hooked = batch && draw;
    // The D3D9Ex device class's draw: a core without that point (before core d4e13b1) leaves gxApi "D3D9Ex" unmasked.
    g_hookedEx = g_hooked &&
                 attach("Gx.DeviceDrawEx", reinterpret_cast<void*>(&hkDrawEx), reinterpret_cast<void**>(&g_origDrawEx));
    if (g_hooked && !g_hookedEx)
        VF_LOG_INFO("player mask: no Gx.DeviceDrawEx hook point in this core; under gxApi D3D9Ex the mask stays empty");
    if (g_hooked)
        VF_LOG_INFO("player mask: hooks in; the reflections look past the player's own pixels");
    else
        // A batch hook without its draw hook only ever marks: harmless.
        VF_LOG_ERROR("player mask: off (the %s hook failed); the reflections guess the player's shape",
                     batch ? "device draw" : "M2 batch draw");
    return g_hooked;
}

void NextFrame()
{
    ++g_frame;
    g_playerRoot = nullptr;
    if (!g_hooked)
        return;
    const uintptr_t unit = engine::ActivePlayerUnit();
    void* model = nullptr;
    if (unit && ReadPtr(unit + kUnitModel, model) && model)
        g_playerRoot = Root(model);
}

void Release()
{
    SafeRelease(g_maskSurface);
    SafeRelease(g_mask);
    g_createFailed = false;
    g_drawnFrame = 0;
    g_sizedFrame = 0;
}

IDirect3DTexture9* ThisFrame(IDirect3DDevice9* dev, UINT& width, UINT& height)
{
    if (!g_mask || dev != g_device || g_drawnFrame != g_frame)
        return nullptr;
    width = g_width;
    height = g_height;
    return g_mask;
}

bool Draw(IDirect3DDevice9* dev, int blendMode, void(__cdecl* draw)(void* user), void* user)
{
    if (!dev || !draw)
        return false;
    IDirect3DSurface9* target = nullptr;
    if (FAILED(dev->GetRenderTarget(0, &target)) || !target)
        return false;
    D3DSURFACE_DESC targetDesc = {};
    if (FAILED(target->GetDesc(&targetDesc)) || !FrameSized(dev, targetDesc.Width, targetDesc.Height) ||
        !EnsureMask(dev, targetDesc.Width, targetDesc.Height))
    {
        target->Release();
        return false;
    }

    IDirect3DSurface9* others[3] = {};
    for (DWORD i = 1; i < 4; ++i)
        if (FAILED(dev->GetRenderTarget(i, &others[i - 1])))
            others[i - 1] = nullptr;
    // The client's depth, for the depth test: only when it pairs with the mask (single-sampled, at least as large).
    IDirect3DSurface9* depth = nullptr;
    bool depthTest = false;
    if (SUCCEEDED(dev->GetDepthStencilSurface(&depth)) && depth)
    {
        D3DSURFACE_DESC depthDesc = {};
        depthTest = SUCCEEDED(depth->GetDesc(&depthDesc)) && depthDesc.MultiSampleType == D3DMULTISAMPLE_NONE &&
                    depthDesc.Width >= g_width && depthDesc.Height >= g_height;
    }
    D3DVIEWPORT9 viewport = {};
    dev->GetViewport(&viewport);
    RECT scissor = {};
    dev->GetScissorRect(&scissor);
    DWORD saved[kStateCount] = {};
    for (int i = 0; i < kStateCount; ++i)
        dev->GetRenderState(kStates[i], &saved[i]);

    for (DWORD i = 1; i < 4; ++i)
        if (others[i - 1])
            dev->SetRenderTarget(i, nullptr);
    dev->SetRenderTarget(0, g_maskSurface);
    if (depth && !depthTest)
        dev->SetDepthStencilSurface(nullptr);
    if (g_drawnFrame != g_frame)
    {
        // The frame's first batch of the player's: nothing of them drawn yet.
        const D3DVIEWPORT9 whole = {0, 0, g_width, g_height, 0.0f, 1.0f};
        dev->SetViewport(&whole);
        dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
        dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
        dev->Clear(0, nullptr, D3DCLEAR_TARGET, kNotPlayer, 1.0f, 0);
        dev->SetRenderState(D3DRS_SCISSORTESTENABLE, saved[kScissorState]);
        g_drawnFrame = g_frame;
        if (!g_loggedFirst)
        {
            g_loggedFirst = true;
            VF_LOG_INFO("player mask: first drawn, %ux%u, %s", g_width, g_height,
                        depthTest ? "with the client's depth test" : "without a depth test (the depth doesn't pair)");
        }
    }
    dev->SetViewport(&viewport);
    dev->SetScissorRect(&scissor);
    dev->SetRenderState(D3DRS_ZENABLE, depthTest ? D3DZB_TRUE : D3DZB_FALSE);
    dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
    dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ZERO);
    dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ZERO);
    dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
    dev->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED);
    dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    if (blendMode == kBlendAlpha)
    {
        dev->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE);
        dev->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATEREQUAL);
        dev->SetRenderState(D3DRS_ALPHAREF, kSolidAlpha);
    }
    draw(user);

    for (int i = 0; i < kStateCount; ++i)
        dev->SetRenderState(kStates[i], saved[i]);
    dev->SetRenderTarget(0, target);
    for (DWORD i = 1; i < 4; ++i)
        if (others[i - 1])
            dev->SetRenderTarget(i, others[i - 1]);
    if (depth && !depthTest)
        dev->SetDepthStencilSurface(depth);
    dev->SetViewport(&viewport);
    dev->SetScissorRect(&scissor);
    for (IDirect3DSurface9*& other : others)
        SafeRelease(other);
    SafeRelease(depth);
    target->Release();
    return true;
}
}
