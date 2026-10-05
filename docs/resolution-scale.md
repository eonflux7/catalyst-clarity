# Resolution scale

How the engine picks its render size, and how Catalyst Clarity's quality modes change it.

## The setting

- `GameRenderSettings.ResolutionScale` is a float at offset `0x14` of the settings instance. A global at RVA
  `0x2142a58` caches the instance pointer. The default is 1.0.
- It's the value behind the in-game **Resolution scale** slider. A `User.cfg` line
  `GameRender.ResolutionScale 0.75` did **not** take effect in testing; the in-game option did.
- Render size = `int(output * scale + 0.5)`, rounded **up to a multiple of 4** when scale != 1.0
  (1920x1080 at 0.75 gives 1440x812, not 1440x810).
- The size is computed in three places: per output view (which also creates the render-size LDR tonemap target
  and an output-size D32S8 for the UI), in the view setup (which writes it into a view descriptor) and at init.
  On PC nothing reads `ResolutionScaleMax` or the `DynamicScaling*` fields, so there is one static render size per
  view and no separate post/display size.
- The view descriptor carries two identical size pairs. Forcing the second pair to output size changed nothing
  observable: all targets stayed at render size. Moving post to output size therefore has to happen on the D3D11
  side ([post-at-output-size.md](post-at-output-size.md)).

## Below 100%

Every post-TAA target is sized from the render size:

- motion blur target with its full mip chain;
- bloom levels: downsample level k reads mip k, down to 1x1 and back up to W/2 x H/2;
- distortion texture (W/2 x H/2) and the LDR tonemap target.

The exposure pyramid is a fixed 256x128. Only the backbuffer and the UI depth are output size. The engine then
upscales once, after tonemap, with RenderScaleResample (LDR, bicubic), and the UI draws at output size on top.

## Changing it at runtime

Writing `ResolutionScale` at Present resizes the render from the next frame, like the slider, including values
below the slider's range (0.5, 0.333), with no stale or garbled frames. Restoring the old value before the game
exits is enough for the game to keep its own saved setting: a forced value isn't saved.

Catalyst Clarity's quality modes use this: DLAA 100%, Quality 66.7%, Balanced 58%, Performance 50%,
Ultra Performance 33.3%. The value is written only while DLSS mode is on and DLSS is available; otherwise the
game's value is restored.
