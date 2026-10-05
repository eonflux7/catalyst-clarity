#pragma once
#include <windows.h>

#include <string>

namespace cs {

// catalyst_clarity.ini next to the DLL. Missing keys get defaults; the file is written back on load
// so every setting is visible to the user.
struct Config {
    bool any_gpu = true;       // [general] any_gpu: force the engine's NVIDIA TAA path on AMD / Intel (experimental)
    int aa_mode = 2;           // [general] mode: 0 = engine TAA, 1 = skip (raw, debug), 2 = DLSS
    UINT toggle_key = VK_F8;   // [ui] toggle_key, virtual-key code: settings menu
    UINT debug_key = VK_F3;    // [ui] debug_key: debug window
    bool show_on_start = false;
    bool show_fps = false;     // small FPS readout while the overlay is closed
    int dlss_preset = 0;       // [dlss] preset: NGX render preset for every quality mode, 0 = DLSS default, 10-13 = J-M
    int dlss_quality_mode = 2; // [dlss] quality_mode: render_scale::kModes index, 0 = game's Resolution scale slider
    int dlss_sharpness = 0;    // [dlss] sharpness: RCAS on the upscaled image, percent (0 = off)
    bool d3d_debug = false;    // [debug] d3d_debug: D3D11 debug layer, messages to the mod log (slow)
};

Config& config();
void config_load(const std::wstring& path);
void config_save();

}  // namespace cs
