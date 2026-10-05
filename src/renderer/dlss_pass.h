#pragma once
#include <d3d11.h>
#include <dxgi.h>

#include <cstdint>

// Replaces the engine's TAA resolve draw with DLSS.
//
// DLSS always upscales to the output (backbuffer) size. With post at output size (renderer/post.h) the
// post chain runs on that image; otherwise the full-size DLSS image is downsampled into the render-size
// TAA outputs and the engine's post chain runs at render size.
namespace cs::dlss_pass {

struct Settings {
    // Jitter handed to DLSS = (sign_x * x, sign_y * y) of table[frame + frame_offset]. (-x, +y) verified in game
    // ((x, -y) shimmers).
    int jitter_sign_x = -1;
    int jitter_sign_y = 1;
    int jitter_frame_offset = 0;
    // Engine velocity is NDC (cur - prev), y up; DLSS wants pixels (prev - cur): (-0.5 W, +0.5 H).
    int mv_sign_x = -1;
    int mv_sign_y = 1;
    // Debug: draw the full-size DLSS output on the backbuffer (simple tonemap, no post, no UI).
    bool preview = false;
    // Resolve the Runner's Vision mask temporally like the engine TAA; off = raw passthrough
    // (carries the per-frame jitter, highlighted edges shimmer).
    bool rv_resolve = true;
};

struct Status {
    bool ready = false;          // last frame was replaced by DLSS
    const char* error = nullptr; // why the last attempt fell back to the engine TAA
    float jitter_x = 0, jitter_y = 0;  // what DLSS got last frame
    bool reset_last = false;
    UINT render_width = 0, render_height = 0;   // DLSS input (TAA viewport)
    UINT output_width = 0, output_height = 0;   // DLSS output (backbuffer)
    bool downsampled = false;    // output > TAA targets: written back with a bilinear downsample
    bool sharpened = false;  // RCAS ran on the output last frame
    bool rv_resolved = false;    // last frame's Runner's Vision mask went through the port
};

Settings& settings();
const Status& status();
void request_reset();

// Called at the TAA draw with its PS SRVs (t0..) and RTVs (0..4). Returns true if it replaced the
// draw (outputs written); false means the caller should run the original draw.
using DrawFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT);
bool run(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* const* srvs, ID3D11RenderTargetView* const* rtvs,
         const D3D11_VIEWPORT& viewport, uint32_t frame_index, DrawFn draw, UINT vertex_count, UINT start_vertex);

// Render thread, every Present before the overlay: tracks the output size, draws the debug preview.
void on_present(IDXGISwapChain* swapchain);

}  // namespace cs::dlss_pass
