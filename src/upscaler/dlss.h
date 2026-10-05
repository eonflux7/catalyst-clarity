#pragma once
#include <d3d11.h>

#include <cstdint>

// Thin wrapper over the NGX DLSS super-sampling feature on D3D11. Render thread only.
namespace cs::dlss {

struct Settings {
    // Off (default): exposure 1.0 with pre-exposure 1. The colour DLSS gets is already pre-exposed by
    // the engine's current-frame exposure and is exactly the tonemapper's input, so 1.0 is the
    // right value. On: DLSS estimates exposure itself. Changing it recreates the feature.
    bool auto_exposure = false;
};

struct Status {
    bool init_tried = false;
    bool available = false;      // NGX initialised and DLSS SR supported on this GPU/driver
    int init_result = 0;         // NVSDK_NGX_Result of Init / capability query, if it failed
    bool needs_driver = false;
    bool feature_created = false;
    int create_result = 0;
    int last_eval_result = 0;
    UINT in_width = 0, in_height = 0, out_width = 0, out_height = 0;
    uint64_t evaluations = 0;
    // OptiScaler (any proxy name) loaded in the process when NGX was initialised: our NGX calls then go
    // through it, and its menu (Insert) picks DLSS / FSR / XeSS. Empty when absent.
    char optiscaler_module[64] = {};
    char optiscaler_version[32] = {};
};

struct EvalInputs {
    ID3D11Resource* colour = nullptr;  // linear HDR
    ID3D11Resource* depth = nullptr;   // R32_FLOAT, reversed Z
    ID3D11Resource* motion = nullptr;  // R16G16_FLOAT, scaled to pixels by mv_scale
    ID3D11Resource* output = nullptr;  // UAV-capable
    UINT render_width = 0, render_height = 0;
    float jitter_x = 0.0f, jitter_y = 0.0f;  // render pixels
    float mv_scale_x = 1.0f, mv_scale_y = 1.0f;
    bool reset = false;
};

Settings& settings();
const Status& status();

// Initialises NGX once, then (re)creates the DLAA/DLSS feature when sizes or settings change.
// Returns true when a feature is ready to evaluate. Sets *recreated when it made a new one.
bool ensure(ID3D11DeviceContext* ctx, UINT in_width, UINT in_height, UINT out_width, UINT out_height,
            bool* recreated);
void request_recreate();
bool evaluate(ID3D11DeviceContext* ctx, const EvalInputs& in);

}  // namespace cs::dlss
