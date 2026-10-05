#pragma once
#include <d3d11.h>

#include <cstdint>

// Run the post chain after DLSS at output size (docs/post-at-output-size.md).
//
// Below 100% scale the engine sizes motion blur, bloom and tonemap from the render size. Per
// frame, after DLSS has upscaled to output size D:
//   motion blur (+0x3745d6e, if enabled): mainTexture = D, RTV = our output-size M, viewport = output,
//     our port of its pixel shader reads invPixelSize = 1/output from b1 (motion_blur_ps.h); then M is downsampled
//     into the engine's render-size HDR mip chain, which exposure, bloom and next frame's SSR read.
//     With motion blur off, M = D (the TAA output already is the mip chain).
//   tonemap (+0x37468a0) and the LDR2 draw (+0x3747ec4), or in the map the DoF composite:
//     mainTexture = M (tonemap), RTV = our output-size LDR texture L, viewport = output. The engine's
//     render-size LDR target is kept current by also running the engine's own draw into it (never refilled
//     inside another engine draw: that hangs the GPU), and refilled at the resample (UI background blur). Bloom, distortion and the Runner's Vision mask stay render size and are
//     upsampled by UV.
//   RenderScaleResample (+0x374809b): skipped; L is copied to its swapchain target, and downsampled into
//     the engine's render-size LDR target, which the UI background blur reads next.
// Passes are recognised by the post function's return address on the stack, only between the
// TAA draw and Present. Anything unexpected falls back to the engine path for the rest of the frame.
namespace cs::post {

using DrawFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT);

struct Settings {
    bool enabled = true;  // post at output size; off = DLSS output downsampled into the render-size chain
    // Off: motion blur stays render size and tonemap upsamples its result (debug / bisecting).
    bool motion_blur = true;
};

struct Status {
    bool active = false;            // last frame had an upscaled DLSS image to work with
    bool motion_blur_seen = false;  // the pass ran last frame (off in the game's settings otherwise)
    bool motion_blur_done = false;  // ... and was redirected
    bool tonemap_done = false;
    bool ldr2_done = false;         // gameplay / pause menu only (absent in map and upgrades)
    bool dof_done = false;          // map only: LDR depth of field composite
    bool resample_done = false;
    uint32_t post_draws = 0;        // Draw calls between TAA and Present
    uint32_t ldr_refills = 0;       // engine render-size LDR target refilled from L
    uint32_t other_ldr_writes = 0;  // unrecognised draws writing the engine LDR target (lost: should be 0)
    uint32_t stale_ldr_reads = 0;   // unrecognised draws reading it before a refill (older image: should be 0)
    const char* error = nullptr;    // last fallback reason (sticky until a clean frame)
    const char* device_lost_after = nullptr;  // first pass after which the device reported removal
    const char* mb_shader = "not loaded";  // our motion blur shader port
};

Settings& settings();
const Status& status();

// From the TAA draw after DLSS upscaled: the engine's TAA output RTV (render size) and our upscaled
// image (output size).
void after_taa(ID3D11RenderTargetView* taa_output, ID3D11ShaderResourceView* upscaled, UINT out_width,
               UINT out_height);
// True between after_taa and Present when there is work to do.
bool active();
// Draw hook while active(). Returns true if the draw was issued (redirected) or replaced here.
bool on_draw(ID3D11DeviceContext* ctx, UINT vertex_count, UINT start_vertex, DrawFn draw);
// Present: releases this frame's references, publishes the status.
void end_frame();

}  // namespace cs::post
