# Modifications

This is a modified version of [wxl-modern-water](https://github.com/manatactical/wxl-modern-water) 1.2 by
manatactical, licensed under GPL-3.0 (see [LICENSE](LICENSE)). The modifications are released under the same license.

- **Who:** VIXAL-OS, with AI coding assistants (Claude and Codex).
- **When:** 7–25 October 2026.
- **What it's based on:** the upstream 1.2 tag.
- **Target:** the 3.3.5a client (build 12340), played on ChromieCraft.

## What changed
- **Startup and visibility:** the world no longer goes invisible at login, water no longer disappears above its surface,
  the device survives a reset, and the water's depth goes half a yard under the surface once shaded (never past the
  ground or rock behind it), so the foam and ripple sheets the game lays just under its rivers and pools show whole
  instead of in flickering slabs (the Frenzyheart River in Sholazar).
- **Rain:** raindrops ring the water, driven by Seyris Living Azeroth's rain signal (`chromie.rain-signal` v1), with
  random placement and visible sizes.
- **Interaction:**
  - Fishing bobbers plop, ring while idle and splash on a bite.
  - Water-walking footsteps and jump landings leave rings, on WMO liquid as well.
  - Units and mounts leave ripples.
  - The waves calm under a player standing in the water.
- **Waterfalls:** turbulence at their bases, lips (calm above, pulsing below, laminar heads, flow lines), pools where
  the water lands, foam and ripples. The falls and lips come from a scan of every map. At a fall's lip the water's
  depth moves down by the fall's height, so the falling sheet under the river's overhanging surface no longer
  flickers through it.
- **Rivers:** they flow, from a scan of every map; ripples drift with the current, and rocks in moving water leave
  wakes.
- **Coasts:** the swell and the surf follow the sea bed, and fish schools ripple the surface.
  - The bed under a player is taken from the world's ground and blended smoothly as they move, so their body and
    their mount don't count as the bed.
- **Looks:**
  - Absorption and murk.
  - Less green canal walls.
  - Lamps on the water: their light, and their reflections as streaks running toward you, broken up by the waves.
    The lights come from Seyris Living Azeroth's light list (`chromie.light-list` v1: lamps, torches, lit windows,
    spell lights) when it publishes one, else from the client's own point lights, which the water now reads in game
    (before, only the offline tests handed it any).
  - Refraction without moiré.
  - No shadow stripes on walls the light skims.
  - Sun glint, and sun glitter: sparkles where the waves catch the sun for a moment, wider with more wind.
  - Reflections stay put as the camera turns: the trace looks for what the water reflects at the same places in the
    world whichever way the camera points (equal angles as seen from the camera), not at places spread over the screen.
  - Reflections no longer break around a hovering mount, and look through it (its see-through wings too) instead of
    falling back to the sky.
  - The exact player mask: the player's own batches (body, mount, gear) are drawn a second time into a mask carried
    beside the depth copy, so far over the water reflections look past exactly their pixels, however wide a drake's
    wings. Without the mask (no two-channel float targets), the reflections guess the player's shape as before.
  - Under a canopy or an overhang, a reflection that runs off the screen shows that cover instead of the sky.

The waterfall, lip and river tables (`src/*_data.inc`) are generated from the game's own map data by separate survey
tools. No game assets are included.

The tests are an offline harness outside this module's folder. They need the client's `waterdata.bin`, set through
`WXL_WATER_DATA`.
