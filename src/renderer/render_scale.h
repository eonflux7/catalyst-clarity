#pragma once

// Quality modes: sets the engine's render size by writing GameRenderSettings.ResolutionScale,
// the float behind the in-game Resolution scale slider. Render thread only (call once per Present).
namespace cs::render_scale {

struct Mode {
    const char* name;
    float scale;  // 0 = leave the game's setting alone
};
// Index = [dlss] quality_mode in catalyst_clarity.ini.
constexpr Mode kModes[] = {
    {"Game setting (Resolution scale slider)", 0.0f},
    {"DLAA (100%)", 1.0f},
    {"Quality (67%)", 2.0f / 3.0f},
    {"Balanced (58%)", 0.58f},
    {"Performance (50%)", 0.5f},
    {"Ultra Performance (33%)", 1.0f / 3.0f},
};
constexpr int kModeCount = static_cast<int>(sizeof(kModes) / sizeof(kModes[0]));

struct Status {
    bool found = false;        // settings instance readable (known build)
    float current = 0.0f;      // ResolutionScale as of the last Present
    float game_value = 0.0f;   // the game's own value, restored when we stop forcing (0 = not saved)
    bool forcing = false;      // we wrote the float this frame or keep it at our value
};

const Status& status();

// active: the upscaler runs (mod in DLSS mode). Otherwise the game's own value is put back.
void on_present(bool active);

}  // namespace cs::render_scale
