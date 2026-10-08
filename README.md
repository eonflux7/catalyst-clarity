<p align="center"><img src=".github/banner.png" alt="Catalyst Clarity" width="100%"></p>

NVIDIA DLSS for **Mirror's Edge Catalyst** (PC, Steam). Catalyst Clarity replaces the game's TAA with DLSS
Super Resolution / DLAA and runs motion blur, tonemapping and the rest of post-processing at your output
resolution, so you get a sharp image from a lower render resolution.

It's a drop-in `version.dll` that hooks the game in memory. No game files are modified.

## Features

- **DLSS quality modes**: DLAA (100%), Quality (67%), Balanced (58%), Performance (50%), Ultra Performance (33%),
  or follow the game's own Resolution scale slider.
- **DLSS render preset** selection (default, J, K, L, M).
- **Post-processing at output size**: motion blur, tonemap, depth of field and the UI composite run on the
  upscaled image instead of being upscaled afterwards.
- **Runner's Vision** highlights are resolved temporally like the original TAA, so edges stay stable.
- **Texture LOD bias** while rendering below output size, so textures keep output-resolution detail.
- Optional **RCAS sharpening**.
- **OptiScaler** compatible: use FSR 3.1 or XeSS through OptiScaler instead of DLSS.
- In-game settings menu (**F8**), usable during gameplay and in menus, saved to `catalyst_clarity.ini`.
  **F3** opens a debug window for developers and bug reports.

## Requirements

- Mirror's Edge Catalyst. Made and tested on the **Steam version**. Other versions (Origin / EA app) are untested:
  you can try them at your own risk. The mod hooks the game only if the game code it needs is identical to the
  Steam build. Otherwise it stays inactive, says so in `catalyst_clarity.log`, and the game runs as usual.
- An **NVIDIA RTX** GPU and a current driver for DLSS.
- **AMD and Intel GPUs (experimental, 0.2.0 and later):** the game normally uses a different (compute) TAA path on
  them; the mod switches it to the NVIDIA path so it can be replaced. DLSS itself needs an RTX GPU, so install
  [OptiScaler](#optiscaler) and pick FSR 3.1 or XeSS. Untested on real AMD / Intel hardware so far: reports
  (with `catalyst_clarity.log`) are welcome. `[general] any_gpu=0` turns it off.
- Windows 10 or 11.

## Install

> **AMD / Intel GPU?** Since 0.2.0 the mod switches the game to the TAA path it normally uses only on NVIDIA, so
> it can replace it there too (a no-op on NVIDIA). DLSS needs an RTX GPU, so use [OptiScaler](#optiscaler) for
> FSR 3.1 or XeSS. It isn't tested on real AMD / Intel hardware yet: please report how it went, with
> `catalyst_clarity.log`.

1. Download `catalyst-clarity-<version>.zip` from [Releases](../../releases).
2. Extract it into the game folder, next to `MirrorsEdgeCatalyst.exe`
   (for example `C:\Program Files (x86)\Steam\steamapps\common\Mirrors Edge Catalyst`).
3. Turn off the EA app's in-game overlay in the EA app's settings. With it on,
   `version.dll` isn't loaded at all: no `catalyst_clarity.log` appears and the game runs without the mod.
4. Start the game. DLSS Quality is on by default. Press **F8** for the settings menu.

To uninstall, delete `version.dll`, `nvngx_dlss.dll`, `catalyst_clarity.ini`, `catalyst_clarity.log`,
`catalyst_clarity_README.txt` and the `catalyst_clarity_licenses` folder.

## Settings

The settings menu (F8) changes everything live. The settings are saved in `catalyst_clarity.ini` next to the game exe:

| Section | Key | Values |
|---|---|---|
| `[general]` | `any_gpu` | `1` (default) run the game's NVIDIA TAA path on AMD / Intel too, `0` off |
| `[general]` | `mode` | `2` DLSS (default), `0` the game's TAA, `1` no AA (debug) |
| `[dlss]` | `quality_mode` | `0` follow the game's Resolution scale slider, `1` DLAA, `2` Quality (default), `3` Balanced, `4` Performance, `5` Ultra Performance |
| `[dlss]` | `preset` | `0` DLSS default, `10`-`13` presets J-M |
| `[dlss]` | `sharpness` | `0`-`100`, RCAS sharpening in percent (`0` = off) |
| `[ui]` | `toggle_key` | Windows virtual-key code of the settings menu key (default `119` = F8) |
| `[ui]` | `debug_key` | key of the debug window (default `114` = F3) |
| `[ui]` | `show_on_start`, `show_fps` | `0` / `1` |
| `[debug]` | `d3d_debug` | `1` enables the D3D11 debug layer (slow; needs the Windows Graphics Tools) |

A quality mode below 100% sets the game's resolution scale while DLSS runs and puts your own setting back when
you switch DLSS off. Your saved setting isn't changed.

## OptiScaler

[OptiScaler](https://github.com/optiscaler/OptiScaler) (tested with 0.9.4) works on top of Catalyst Clarity and
lets you pick FSR 3.1 or XeSS instead of DLSS:

1. Install OptiScaler into the game folder as **`dxgi.dll`** (its default name; `version.dll` is taken by this mod),
   along with its `OptiScaler.ini`, `amd_fidelityfx_*.dll`, `libxess*.dll`, `libxell.dll` and `D3D12_Optiscaler\`.
   fakenvapi and dlssg aren't needed.
2. Start the game. Catalyst Clarity's settings menu shows that OptiScaler was detected, and OptiScaler's own menu
   (**Insert**) switches the upscaler.

FSR 3.1 (native DX11) and DLSS through OptiScaler have been checked in game; XeSS hasn't.

## Known issues

- Photo mode hasn't been tested.
- On RTX 20-series GPUs the default DLSS presets (transformer model) are expensive, and DLSS Quality can be slower
  than the game's TAA at 100%. Try Performance mode.
- The game must be running in DirectX 11 (the only renderer the PC version has).
- Some antivirus tools flag `version.dll` proxies on principle. Build it yourself from source if in doubt.

If something goes wrong, `catalyst_clarity.log` in the game folder says what the mod did. Please attach it to
bug reports.

## How it works

The mod hooks `IDXGISwapChain::Present`, the engine's TAA resolve function and `ID3D11DeviceContext::Draw`. At the
TAA draw it hands the engine's HDR colour, depth and motion vectors to DLSS with the engine's own jitter. It then
redirects the post-processing draws to output-size targets.

The knowledge behind it is in [`docs/`](docs):

- [Frame graph](docs/frame-graph.md): the pass order of a frame, resolutions, call sites.
- [TAA and DLSS inputs](docs/taa.md): bindings, depth, motion vectors, jitter sequence, exposure.
- [Runner's Vision mask](docs/runners-vision.md): how the highlight mask is drawn and resolved.
- [Resolution scale](docs/resolution-scale.md): how the render size is set and changed at runtime.
- [Post at output size](docs/post-at-output-size.md): running post-processing on the upscaled image.

## Building

Requirements: Visual Studio 2022 (or the Build Tools) with the C++ x64 workload. CMake and Ninja ship with it.

```
build.cmd           :: build\version.dll
build.cmd dist      :: also dist\catalyst-clarity-<version>.zip
```

CMake downloads Dear ImGui, MinHook and the required files of the NVIDIA DLSS SDK at configure time.
`build\ngx\dev\nvngx_dlss.dll` is the development DLSS runtime with NVIDIA's on-screen debug overlay.

## Credits and licenses

Catalyst Clarity is MIT licensed ([LICENSE](LICENSE)). It uses the NVIDIA DLSS SDK, Dear ImGui, MinHook and an
adaptation of AMD FidelityFX RCAS; see [THIRD_PARTY.md](THIRD_PARTY.md).

NVIDIA, RTX and DLSS are trademarks of NVIDIA Corporation. Mirror's Edge is a trademark of Electronic Arts Inc.
This project isn't affiliated with or endorsed by Electronic Arts, DICE or NVIDIA. You need your own copy of the
game; nothing from the game is included here.
