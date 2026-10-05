# TAA and DLSS inputs

Everything an upscaler needs from the engine's TAA: where it runs, what it binds, and the conventions of depth,
motion vectors, jitter and exposure. RVAs are for the supported Steam build ([frame-graph.md](frame-graph.md)).

## The TAA function

- TAA resolve function: RVA `0x35d25a0`, called directly from the world renderer at `0x3745a4f`.
  Signature `(rcx = TAA state, rdx = render context, r8 = view)`, returns void.
- It runs exactly once per frame, before Present, and survives resolution changes (the TAA state and view
  pointers stay the same).
- `view+0xac` is the frame index (+1 per frame) and `view+0xb1` is "TAA enabled" (0: it only copies input to
  output).
- History textures come from a table at `state+0x78`, ping-ponged on `frameIndex & 1`.
- On NVIDIA GPUs (adapter vendor `0x10de`) it draws a fullscreen pixel shader `TAAResolvePs` (draw call site
  `0x35d3351`). **On other vendors it dispatches a compute shader `TAAResolve` instead**, with
  `ceil(w/16) x ceil(h/16)` groups. Catalyst Clarity hooks the draw, so it only works on NVIDIA.
- On the High graphics preset (not Medium) the same function issues a **second draw**: a temporal resolve of
  single-channel data (t0 depth, t1 velocity, t2-t4 R8 inputs, RTV0-2 R8). A hook must pick the colour resolve
  by its bindings (HDR colour in t3, HDR target in RTV1), or it will run the upscaler twice per frame.

## TAA resolve bindings

| Slot | Name | Format | Content |
|---|---|---|---|
| PS t0 | `g_depthTexture` | D32S8 (typeless) | scene depth, **reversed-Z** |
| PS t1 | `g_velocityTexture` | R16G16_FLOAT | motion vectors (below) |
| PS t3 | `g_inputTexture0` | R11G11B10_FLOAT (RGBA16F on some settings) | linear HDR colour, a copy of the lit scene |
| PS t4 | `g_historyInputTexture0` | as t3 | colour history |
| PS t5 | `g_inputTexture1` | R8 | Runner's Vision mask ([runners-vision.md](runners-vision.md)) |
| PS t6 | `g_historyInputTexture1` | R8 | mask history |
| RTV 0 | | R8 | written with a constant, never read (dead output) |
| RTV 1 | | as t3 | resolved HDR colour, read by motion blur |
| RTV 2 | | as t3 | next frame's colour history |
| RTV 3 | | R8 | resolved mask, read by tonemap as `runnersVisionAlphaMaskTexture` |
| RTV 4 | | R8 | next frame's mask history |
| cb0 | `ShaderConstants` (384 B) | | slice of a 1 MB constant ring buffer |
| s0 / s1 | | | point / trilinear |

When motion blur is off in the settings, RTV 1 is the R11G11B10 HDR mip-chain texture itself (11 mips) instead
of a plain texture.

## What the engine TAA does

TAA reprojects **only through the velocity buffer**. Velocity is read at the nearest-depth texel of the 3x3
neighbourhood and `prevUV = uv - vel * (0.5, -0.5)`. History is Catmull-Rom resampled (9 taps), rescaled by
`g_exposureMultiplier * g_historyUnexposureMultiplier`, and clipped to the 3x3 neighbourhood AABB in
`x / (1 + luma)` space. Current colour is filtered with 9 jitter-dependent weights (`g_neighborWeights`). The blend
factor is clamped to `[g_minHistoryBlendFactor, g_maxHistoryBlendFactor]`, with an anti-flicker term and
`g_motionSharpeningFactor`. History is dropped off-screen, and NaN becomes 0. The matrices in `ShaderConstants`
(`g_currentToPrevFrameTransform`, `g_invJitteredViewProjection`, `g_invUnjitteredViewProjection`) are declared but
never read. The game's names for the two inverse VPs are swapped relative to the raster.

## Depth

Reversed-Z with an infinite far plane: far/sky = 0, near plane 0.06 (`P[3][2]`). For DLSS: `DepthInverted`.

## Motion vectors

- **Units**: NDC, *current minus previous*, y up.
- **Coverage**: camera and object motion. A fullscreen draw reconstructs camera motion from depth, then moving and
  skinned objects redraw on top, depth-tested.
- **Jitter-free**: the camera pass maps the un-jittered current position to the un-jittered previous VP. Object
  draws use a separate view-constants block whose previous VP has the *current* jitter applied, so jittered
  current minus jittered previous cancels the jitter (exact to 4e-8).
- **Approximation**: the camera pass writes `ndc * prevW - prevClip.xy` without dividing by `prevW`. It matches the
  GPU texels to 1.4e-5 but differs from exact reprojection by up to about 2% of the motion, more toward the screen
  edges under fast rotation. Exact camera MVs can be recomputed from depth and `g_currentToPrevFrameTransform`
  (`ShaderConstants` offset 64, row-vector convention).
- **For DLSS** (pixels, previous minus current): `mv_px = vel * (-0.5 * W, +0.5 * H)`, not jittered, at render
  resolution (`MVLowRes`). Verified in game: stable, no visible ghosting beyond normal DLSS behaviour.

## Camera matrices

The `viewConstants` cbuffer (1120 B, shared by VS and PS) uses the **row-vector** convention (`v * M`):

| Offset | Field |
|---|---|
| 32 | `viewMatrix` |
| 96 | `projMatrix` (jittered: `P[2][0] = +2x/W`, `P[2][1] = +2y/H`) |
| 160 | `viewProjMatrix` = view * proj (jittered) |
| 224 | `crViewProjMatrix`: camera-relative VP used for SV_Position, `(pos - cameraPos) * crVP` |
| 288 | `prevViewProjMatrix` |
| 720 | `cameraPos` |

Horizontal FOV 112.4° and vertical 80.0° at 16:9 (`P00 = 0.66985`, `P11 = 1.19084`). No viewport jitter.

## Jitter

- The TAA function calls a jitter callback (`state+0x20`, RVA `0x35cc4d0`) with `frameIndex % count`
  (`count` at `state+0x18`, 1024). If `count < 2` the jitter is 0.
- The callback is a table lookup: a `std::vector<float2>` whose begin/end pointers are globals at RVA `0x2565280` /
  `0x2565288`. The table holds 1024 entries in pixels within ±0.5 and is a correlated multi-jittered pattern (one
  sample per cell of a 32x32 grid, n-rooks stratified on both axes). Settings `TemporalAAJitterUseCmj` and
  `TemporalAAJitterCount` exist.
- Sign: the raster is shifted by NDC `(-2x/W, -2y/H)`, so the image moves `(-x, +y)` pixels (x right, y down).
  Matched exactly against `projMatrix` in two captures.
- **For DLSS**: `JitterOffset = (-x, +y)` of `table[frameIndex % 1024]`, in render pixels. Verified in game:
  `(x, -y)` visibly shimmers.
- The engine TAA's own reconstruction weights are centred at `(-x, -y)`, so they don't flip y. That's a quirk of
  the engine filter and irrelevant for DLSS.

## Exposure

Exposure is a CPU-side constant applied at shading time. The exposure pyramid (256x128 to 1x1, log luminance) is
copied to a staging texture and read back by the CPU with latency; no shader samples it. The lighting and forward
shaders multiply by `exposureMultipliers = (E, 1/E, E, 1/E)`, so the HDR colour TAA receives is **already
pre-exposed** by the current frame's exposure `E_t`, and is exactly the tonemapper's input. TAA `ShaderConstants`
carries `g_exposureMultiplier = E_t` (offset 32), `g_unexposureMultiplier = 1/E_t` (36) and
`g_historyUnexposureMultiplier = 1/E_{t-1}` (40). Tonemap applies no further exposure.

For DLSS: exposure 1.0 with pre-exposure 1 is consistent and is the mod's default. `InPreExposure = E_t` with
exposure `E_t` would also be correct and would let DLSS rescale its history when exposure adapts.
