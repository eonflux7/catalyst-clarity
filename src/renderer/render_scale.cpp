#include "renderer/render_scale.h"

#include <windows.h>

#include <cmath>

#include "core/config.h"
#include "core/log.h"
#include "core/state.h"

namespace cs::render_scale {

// Global caching the GameRenderSettings instance; ResolutionScale is the float at +0x14.
constexpr uintptr_t kSettingsGlobalRva = 0x2142a58;
constexpr uintptr_t kResolutionScaleOffset = 0x14;

static Status g_status;

const Status& status() { return g_status; }

static float* scale_field() {
    if (!state().code_matched)
        return nullptr;
    __try {
        auto settings = *reinterpret_cast<uint8_t* const*>(state().game_base + kSettingsGlobalRva);
        if (!settings)
            return nullptr;
        auto* field = reinterpret_cast<float*>(settings + kResolutionScaleOffset);
        float v = *field;
        return v > 0.0f && v <= 4.0f ? field : nullptr;  // sanity: a scale, not garbage
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

static bool write(float* field, float value) {
    __try {
        *field = value;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void on_present(bool active) {
    float* field = scale_field();
    g_status.found = field != nullptr;
    if (!field) {
        g_status.forcing = false;
        return;
    }
    g_status.current = *field;
    int mode = config().dlss_quality_mode;
    float target = active && mode > 0 && mode < kModeCount ? kModes[mode].scale : 0.0f;
    if (target > 0.0f) {
        if (!g_status.forcing) {
            g_status.game_value = g_status.current;
            g_status.forcing = true;
        }
        if (std::fabs(g_status.current - target) > 1e-6f && write(field, target)) {
            logf("render_scale: %s, ResolutionScale %.4f -> %.4f", kModes[mode].name, g_status.current, target);
            g_status.current = target;
        }
    } else if (g_status.forcing) {
        g_status.forcing = false;
        if (g_status.game_value > 0.0f && write(field, g_status.game_value)) {
            logf("render_scale: restored the game's ResolutionScale %.4f", g_status.game_value);
            g_status.current = g_status.game_value;
        }
    }
}

}  // namespace cs::render_scale
