# Frame graph

How Mirror's Edge Catalyst (Frostbite, D3D11, PC) renders a gameplay frame, observed in frame captures of the
Steam build. The retail build has no debug markers, so pass names are inferred from shader reflection names and
render-target formats.

All addresses below are RVAs in `MirrorsEdgeCatalyst.exe` (image base `0x140000000`) of the supported build
(PE timestamp `1466098615`). They will be wrong for any other build.

## Pass order

| # | Pass | Output |
|---|---|---|
| 1 | Cloth / morph compute, buffer uploads | |
| 2 | Clear depth-stencil | D32S8 |
| 3 | GBuffer (about 580 draws) | MRT: RGB10A2, RGBA8_SRGB, RGBA8, R11G11B10F; D32S8 |
| 4 | Decals and GBuffer composite | GBuffer |
| 5 | Runner's Vision mask (3 instanced mesh draws, see [runners-vision.md](runners-vision.md)) | R8 |
| 6 | Linearise depth | R32F |
| 7 | Depth pyramid | R32F, mips |
| 8 | SSAO at half resolution + blur | R8 |
| 9 | 4 shadow cascades (896x896), each followed by a screen-space shadow mask; shadow filtering compute | RG8 |
| 10 | Screen-space reflections (tile classify, hierarchical-Z trace, resolve with history) | |
| 11 | Tiled light culling + indirect lighting (compute) | HDR scene colour |
| 12 | Fullscreen deferred lighting (sun, IBL) | HDR scene colour, R11G11B10F |
| 13 | Half-resolution depth | |
| 14 | Forward opaque and sky | HDR scene colour |
| 15 | Atmosphere / aerial perspective | HDR scene colour |
| 16 | Forward transparents (about 90 draws) | HDR scene colour |
| 17 | Half-resolution transparents and particles, then composite | RGBA16F, then HDR scene colour |
| 18 | Copy of the HDR scene colour (the TAA colour input) | R11G11B10F |
| 19 | Velocity: one fullscreen camera-motion draw, then moving and skinned objects (depth-tested) | R16G16F |
| 20 | **TAA resolve** ([taa.md](taa.md)) | HDR colour, history, resolved RV mask |
| 21 | Motion blur (if enabled), then a compute blur over its mip chain | HDR mip chain |
| 22 | Exposure luminance pyramid (256x128 to 1x1) | 1x1 RGBA16F |
| 23 | Bloom downsample / blur / upsample chain | |
| 24 | Tonemap + colour grading + bloom + lens dirt + Runner's Vision tint | LDR RGBA8 |
| 25 | Second LDR draw (glow / lens, gameplay and pause menu only) | LDR |
| 26 | Depth of field (map screen and slides only; see [post-at-output-size.md](post-at-output-size.md)) | LDR |
| 27 | RenderScaleResample (only below 100% resolution scale) | backbuffer |
| 28 | Environment / IBL cube update (not every frame) | |
| 29 | UI background blur (compute) and UI / HUD | backbuffer |
| 30 | Present | |

TAA runs on linear HDR colour **before** motion blur, exposure, bloom and tonemap. That's the natural place for
an upscaler.

## Resolution

- At 100% resolution scale everything (GBuffer, depth, HDR colour, swapchain) is output size. Only a few effects
  run at a fixed half size (SSAO, atmosphere, half-res transparents).
- Below 100% the **whole** chain runs at render size: GBuffer, lighting, TAA and its history, motion blur, bloom,
  the exposure pyramid input and the LDR tonemap target. TAA history is render size, so the engine has no
  temporal upscaling. The only upscale is RenderScaleResample, after tonemap, in LDR: one pass with 16 bilinear
  taps (a 4x4 bicubic kernel). The UI then draws at output size and reads render-size depth plus a small blurred
  copy of the LDR image. Details in [resolution-scale.md](resolution-scale.md).

## Anti-aliasing

The AA setting is one enum (`PostProcessAAMode_None`, `_FxaaLow/Medium/High`, `_FxaaCompute(Extreme)`, `_Smaa1x`,
`_SmaaT2x`, `_TemporalAA`). In TAA mode no FXAA or SMAA pass runs, and between the colour copy and TAA only velocity
draws touch anything. Skipping the TAA draw shows the raw, jittered, aliased scene, so an upscaler that replaces
TAA needs no other AA pass disabled.

## Call sites

The world renderer (`0x3742bd0`) issues the scene passes from sibling call sites. Return addresses seen on the
stack of each pass:

| Pass | Return RVA |
|---|---|
| GBuffer | `0x3743491` |
| SSR | `0x3744216` |
| Lighting | `0x374477f` |
| Colour copy | `0x3745639` |
| Velocity | `0x3745673` |
| TAA | `0x3745a54` (direct `call 0x35d25a0` at `0x3745a4f`) |
| Motion blur | `0x3745d6e` |
| Tonemap (and everything drawn under it: exposure, bloom, DoF) | `0x37468a0` |
| Second LDR draw | `0x3747ec4` |
| RenderScaleResample | `0x374809b` |
| UI background blur | `0x37482d0` |
| Present | `0x3613767` (`call [rax+0x40]`) |

Post passes all go through one draw helper. A pass is identified by the **pair** of adjacent return addresses
(pass function into the helper, post function into the pass), because the post frame alone is shared by many
draws. See [post-at-output-size.md](post-at-output-size.md).
