# Third-party components

| Component | Use | License |
|---|---|---|
| [NVIDIA DLSS SDK](https://github.com/NVIDIA/DLSS) v310.9.1 | NGX headers and static library linked into `dinput8.dll`; `nvngx_dlss.dll` shipped in the release zip | NVIDIA RTX SDKs license (`LICENSE.txt` in the SDK, `catalyst_clarity_licenses/nvidia-dlss-sdk.txt` in the zip) |
| [Dear ImGui](https://github.com/ocornut/imgui) v1.91.5 | in-game overlay | MIT |
| [MinHook](https://github.com/TsudaKageyu/minhook) v1.3.3 | function hooks | BSD 2-Clause |
| [AMD FidelityFX FSR 1](https://github.com/GPUOpen-Effects/FidelityFX-FSR) | RCAS sharpening shader, adapted (`src/renderer/rcas_ps.h`) | MIT (`licenses/amd-fidelityfx-fsr1.txt`) |

The SDK, ImGui and MinHook are downloaded by CMake at configure time and are not part of this repository.
Their license files are copied into the release zip by `build.cmd dist`.

This software contains source code provided by NVIDIA Corporation.

NVIDIA, RTX and DLSS are trademarks of NVIDIA Corporation. Mirror's Edge is a trademark of Electronic Arts Inc.
Catalyst Clarity is not affiliated with or endorsed by NVIDIA, Electronic Arts or DICE.
