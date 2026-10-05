# Runner's Vision mask

Runner's Vision (the red highlighting of the route) isn't part of the scene colour. It's a separate R8 mask that
the engine's TAA resolves alongside colour and the tonemapper uses to tint the image. Anything that replaces TAA
has to handle it, or the highlights disappear or shimmer.

## Producer

- An R8 texture at render resolution, cleared every frame. In scenes without Runner's Vision it stays cleared.
- Drawn by 3 instanced mesh draws of the highlighted objects (their pixel shader reads
  `external_runVis_FillAmount`), after the GBuffer decals and before the end of the GBuffer phase.
- Depth test GreaterEqual against the main depth (reversed-Z), no depth write, back-face culling, full viewport.
- They use the same jittered view constants as the scene, so the mask is rasterised **jittered** at render
  resolution.

## Consumers

- TAA resolves it as a second channel: input t5, history t6, resolved mask RTV 3, new history RTV 4.
- The resolved mask is read **only** by the tonemap pixel shader, as `runnersVisionAlphaMaskTexture`, sampled
  with the interpolated UV (not with `SV_Position` or a load). No cbuffer field depends on its size, so a
  render-size mask works unchanged when tonemap runs at output size: the sampler scales it.

## The engine's resolve

Same scheme as colour, without the bicubic history and tonemap weighting:

1. Velocity from the nearest of the 4 diagonal depth texels if it beats the centre (dilation), converted from NDC
   to UV with `(0.5, -0.5)`. `prevUV = uv - v`. Off-screen if `max(|prevNDC| + 2/size) >= 1`.
2. Current value: 3x3 reconstruction filter with the per-frame, jitter-dependent `g_neighborWeights[0..2]` (taps
   NW, N, NE, W, C, E, SW, S, SE, summing to 1). Plain centre texel when off-screen.
3. History: bilinear sample of t6 at `saturate(prevUV)`, times `g_exposureMultiplier * g_historyUnexposureMultiplier`
   (about 1, shared with colour), clamped to `[mn, mx]`, where `mn` / `mx` average the cross (5) and full (9)
   min/max.
4. Weight of the current value:
   `a = saturate(((smoothstep(in, out, min(|mx-h|, |mn-h|)) + motion + antiflickerMultiplier) / 8 * (1 + 8*a*motion)) / (1 + 2(mx-mn)/(mn+mx)))`,
   clamped to `[minHistoryBlend, maxHistoryBlend]`, with `motion = saturate(|v_px|_1 * motionSharpeningFactor)`.
5. NaN becomes 0; the same value goes to RTV 3 and RTV 4.

Gameplay constants: antiflicker multiplier 0.1, in/out distance 0/1, motion sharpening 1, history blend [0, 1], so
a static mask accumulates with `a <= 0.0125`.

Every size-dependent input (width/height, pixel-space motion, the neighbour weights) is render size and already in
the TAA `ShaderConstants` bound at the TAA draw. Catalyst Clarity therefore runs an HLSL port of this resolve
(`src/renderer/rv_mask_ps.h`) at the TAA draw with the engine's bindings untouched, before DLSS evaluates. Because
DLSS gets the same jitter as the engine, the weights stay valid and no constant needs patching.
