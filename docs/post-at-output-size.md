# Running post-processing at output size

Below 100% resolution scale the engine runs motion blur, bloom and tonemap at render size and only upscales the
final LDR image ([resolution-scale.md](resolution-scale.md)). If DLSS replaced TAA and its output were fed back
into that render-size chain, most of the gain would be lost. Catalyst Clarity instead keeps the engine at render
size and redirects a few post draws to output-size targets on the D3D11 side, without patching engine code.

## Post shader size conventions

- Fullscreen quads come from a CPU vertex buffer in target pixels. Every post vertex shader computes
  `NDC = 2 * v0 * invPixelSize - 1` with VS `invPixelSize = 1/target`, while PS `invPixelSize = 1/source` (the tap
  step).
- Motion blur derives its UV as `SV_Position * PS invPixelSize` and samples colour, linear depth and velocity with
  that UV. Inputs can be any size (normalised UVs, velocity in NDC), but writing at output size needs
  `invPixelSize = 1/output`.
- Tonemap and the second LDR draw use only interpolated UVs, so they're resolution-agnostic: changing the render
  target and viewport is enough. The only other size-dependent constants are aspect ratios (`H/W`), which differ
  negligibly after the multiple-of-4 rounding.
- Post constants are slices of one 1 MB ring buffer bound with byte offsets (`*SetConstantBuffers1`).

## Identifying the passes

All post passes go through one draw helper. Each pass is identified by the adjacent pair of return addresses on
the stack: the pass function's call into the helper, and the post function's call into the pass. The post frame
alone isn't enough (`0x37468a0` is also on the stack of every exposure and bloom draw). Each pair matches exactly
one draw per frame, at any resolution scale:

| Pass | (pass, post) return RVAs |
|---|---|
| Motion blur | `0x35c16da`, `0x3745d6e` |
| Tonemap | `0x35bf90c`, `0x37468a0` |
| Second LDR draw | `0x35c0a5e`, `0x3747ec4` |
| RenderScaleResample | `0x35c3524`, `0x374809b` |
| Depth of field composite (map, slides) | `0x35bcacb`, `0x35bff15` |

## What reads what

- The render-size HDR mip chain is read **next frame** by screen-space reflections, and this frame by exposure,
  its own mip blur, bloom and tonemap. It must stay filled every frame, even when tonemap reads an output-size
  image.
- TAA's plain output (motion blur on) is read after TAA only by motion blur.
- The render-size LDR target is written by tonemap and the second LDR draw, and read by RenderScaleResample, the
  UI background blur (compute) and, in the map and during slides, the depth of field downsample.
- UI draws read render-size depth already in vanilla below 100%, so they need no change.
- TAA history is written only by TAA.

## Samplers

Motion blur samples all four inputs with **point** filtering (depth and velocity clamp, colour mirror-once, a
32x32 random texture wrap). Output-size motion blur reading render-size depth/velocity is therefore
nearest-sampled, which is acceptable for blur. Bloom up, the second LDR draw and the resample are trilinear;
tonemap is linear with point mips, and its grading LUT is trilinear. No post sampler is anisotropic.

## The scheme

Per frame, after DLSS has upscaled to an output-size image D:

1. **Motion blur** (only if enabled in settings): colour = D, render target = an output-size M, viewport = output.
   The engine's pixel shader is replaced by an HLSL port (`src/renderer/motion_blur_ps.h`) that reads
   `invPixelSize = 1/output` from the mod's own constant buffer, with the engine's constants left bound. M is then
   downsampled into the engine's render-size mip chain, so exposure, bloom and SSR run unchanged. With motion blur
   off, M = D.
2. **Tonemap** and the **second LDR draw** (or the **DoF composite** in the map and during slides, which is
   alpha-blended with SrcAlpha / InvSrcAlpha): colour = M, render target = an output-size LDR texture L,
   viewport = output. Bloom, distortion and the Runner's Vision mask stay render size and are upsampled by UV.
   Tonemap also runs the engine's own draw into the render-size LDR target, so the DoF downsample, which reads it
   mid-frame, finds it filled.
3. **RenderScaleResample** is skipped: L is copied to the swapchain and downsampled into the engine's render-size
   LDR target, which the UI background blur reads next.

Anything unexpected falls back to the engine path for the rest of the frame.

## Lessons learned

- A redirected draw must also unbind the engine's render-size depth-stencil view. Otherwise D3D11 rejects the
  `OMSetRenderTargets` (size mismatch) and the draw still lands in the engine target.
- Patching `invPixelSize` by GPU-copying the engine's ring-buffer slice and updating the copy was unreliable: the
  update was applied before the queued copy, and workarounds ended in `DXGI_ERROR_DEVICE_HUNG`. Replacing the
  pixel shader with a port that reads its own constant buffer works.
- On NVIDIA, issuing an engine draw directly on state restored by a `SwapDeviceContextState` round trip, without
  the engine re-setting its state, intermittently hangs the GPU. The debug layer hides it. Rule: don't run your
  own state-swapped work inside a hooked draw whose engine draw still follows.
- Texture LOD bias: while rendering below output size, anisotropic samplers bound during the scene passes get a
  bias of `log2(render / output)`. Post samplers are left alone.
- Pause menu and upgrades screens need nothing extra. The map has no second LDR draw but runs the DoF; slides
  run the DoF between tonemap and the second LDR draw. The vanilla slide DoF ends with an abrupt cut.
