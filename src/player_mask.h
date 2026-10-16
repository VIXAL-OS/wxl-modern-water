#pragma once

#include <d3d9.h>

struct WXL_Api;

// The exact player mask: which pixels of the frame are the player's own (their body, their mount, what they carry),
// drawn by the client's own batches a second time into a mask. The water's reflections look past the player by it
// when they are well above the water. See player_mask.cpp.
namespace player_mask
{
// From WXL_Load: the M2 batch draw and device draw hooks. Whether both went in.
bool Install(const WXL_Api* api);

// At the end of every frame (OnFrame, just before Present): the next frame's mask starts empty, and the root of the
// player's model family is read again.
void NextFrame();

// The device's targets are going away (device lost): the mask is released.
void Release();

// The mask as drawn this frame on dev (its size in width, height), or null: the player wasn't drawn this frame (or
// not to the frame's own target), or the hooks aren't in.
IDirect3DTexture9* ThisFrame(IDirect3DDevice9* dev, UINT& width, UINT& height);

// Draws into the mask, as the player's: draw(user) runs with the mask as the only colour target and the device's depth
// kept for the depth test (without it when it can't be paired with the mask), and the targets, viewport, scissor and
// the render states changed are put back after. Only while the device draws to a target of the back buffer's size.
// blendMode is the batch's: 0 opaque and 1 alpha key as the client draws them, 2 (alpha) alpha-tested at 128. The
// hook calls it for each of the player's batches; tests call it for their stand-in player.
bool Draw(IDirect3DDevice9* dev, int blendMode, void(__cdecl* draw)(void* user), void* user);
}
