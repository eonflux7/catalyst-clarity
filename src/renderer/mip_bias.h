#pragma once
#include <d3d11.h>

#include <cstdint>

// Texture LOD bias while rendering below output size, so materials keep output-res detail
// (DLSS guide: bias = log2(render width / output width)). Done by swapping each anisotropic sampler
// the engine binds for a biased copy of it (PSSetSamplers hook, any context); engine samplers are
// never modified, so turning it off is immediate.
namespace cs::mip_bias {

struct Settings {
    bool enabled = true;
    float offset = 0.0f;           // added to log2(render / output)
    bool trilinear_too = false;    // also bias MIN_MAG_MIP_LINEAR samplers (not just anisotropic)
};

struct Status {
    bool hooked = false;
    float bias = 0.0f;                 // in effect this frame (0 = off)
    uint32_t samplers = 0;             // biased copies made
    uint32_t swaps_last_frame = 0;     // sampler bindings replaced last frame
};

Settings& settings();
const Status& status();

// Hooks PSSetSamplers through this context's vtable (once).
void install(ID3D11DeviceContext* ctx);
// Render thread, every Present: the bias for the next frame (0 disables), rolls the counters.
void begin_frame(float bias);
// TAA entry: no bias for the rest of the frame (post samplers).
void end_scene();

}  // namespace cs::mip_bias
