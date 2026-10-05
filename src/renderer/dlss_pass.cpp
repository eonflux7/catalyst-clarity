#include "renderer/dlss_pass.h"

#include <windows.h>

#include <cstring>

#include "core/config.h"
#include "core/d3d_debug.h"
#include "core/log.h"
#include "renderer/gpu.h"
#include "renderer/jitter.h"
#include "renderer/post.h"
#include "renderer/rcas_ps.h"
#include "renderer/rv_mask_ps.h"
#include "upscaler/dlss.h"

namespace cs::dlss_pass {

// TAA draw bindings (docs/taa.md).
constexpr int kSrvDepth = 0, kSrvVelocity = 1, kSrvColour = 3, kSrvRvMask = 5, kSrvRvHistory = 6;
constexpr int kRtvOutput = 1, kRtvHistory = 2, kRtvRvMask = 3, kRtvRvHistory = 4;

// Our own textures in plain formats; NGX only ever sees these (plus the engine velocity buffer).
struct Target {
    ID3D11Texture2D* tex = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    ID3D11UnorderedAccessView* uav = nullptr;
    UINT width = 0, height = 0;
};

static Settings g_settings;
static ID3D11PixelShader* g_rv_ps;
static bool g_rv_tried;
static Status g_status;
static bool g_reset = true;
static uint32_t g_last_frame;
static Target g_colour;  // RGBA16F copy of the engine HDR colour (render size)
static Target g_depth;   // R32F copy of the engine depth (render size)
static Target g_output;  // RGBA16F DLSS output (output size)
static Target g_sharp;   // RGBA16F RCAS of g_output, when sharpening is on
static const Target* g_final = &g_output;  // what the rest of the frame reads
static ID3D11PixelShader* g_rcas_ps;
static ID3D11Buffer* g_rcas_cb;
static bool g_rcas_tried;
static UINT g_display_w, g_display_h;  // backbuffer, from the last Present
static bool g_ran_this_frame;          // DLSS wrote g_output since the last Present

Settings& settings() { return g_settings; }
const Status& status() { return g_status; }
void request_reset() { g_reset = true; }

static void destroy(Target& t) {
    for (IUnknown* p : {(IUnknown*)t.uav, (IUnknown*)t.rtv, (IUnknown*)t.srv, (IUnknown*)t.tex})
        if (p)
            p->Release();
    t = Target{};
}

static bool make(ID3D11Device* device, Target& t, UINT width, UINT height, DXGI_FORMAT format, UINT bind,
                 const char* name) {
    if (t.tex && t.width == width && t.height == height)
        return true;
    destroy(t);
    D3D11_TEXTURE2D_DESC d{};
    d.Width = width;
    d.Height = height;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = format;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = bind | D3D11_BIND_SHADER_RESOURCE;
    HRESULT hr = device->CreateTexture2D(&d, nullptr, &t.tex);
    if (SUCCEEDED(hr))
        hr = device->CreateShaderResourceView(t.tex, nullptr, &t.srv);
    if (SUCCEEDED(hr) && (bind & D3D11_BIND_RENDER_TARGET))
        hr = device->CreateRenderTargetView(t.tex, nullptr, &t.rtv);
    if (SUCCEEDED(hr) && (bind & D3D11_BIND_UNORDERED_ACCESS))
        hr = device->CreateUnorderedAccessView(t.tex, nullptr, &t.uav);
    if (FAILED(hr)) {
        logf("dlss_pass: creating %s %ux%u format %d failed 0x%08lx", name, width, height, (int)format, hr);
        destroy(t);
        return false;
    }
    t.width = width;
    t.height = height;
    logf("dlss_pass: %s %ux%u format %d", name, width, height, (int)format);
    return true;
}

static bool fail(const char* why) {
    if (g_status.error != why)
        logf("dlss_pass: falling back to engine TAA: %s", why);
    g_status.error = why;
    g_status.ready = false;
    return false;
}

// The engine's TAA draw, Runner's Vision part only: our port of it (rv_mask_ps.h) with the engine's
// bindings, writing RTV3/RTV4. The engine state is restored afterwards (the TAA draw itself is skipped).
static bool resolve_rv_mask(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* const* srvs,
                            ID3D11RenderTargetView* const* rtvs, DrawFn draw, UINT vertex_count, UINT start_vertex) {
    if (!srvs[kSrvRvMask] || !srvs[kSrvRvHistory] || !rtvs[kRtvRvMask] || !rtvs[kRtvRvHistory])
        return false;
    if (!g_rv_tried) {
        g_rv_tried = true;
        ID3D11Device* device = nullptr;
        ctx->GetDevice(&device);
        g_rv_ps = gpu::create_pixel_shader(device, kRvMaskPs, "rv_mask_ps");
        device->Release();
        logf("dlss_pass: Runner's Vision mask port %s", g_rv_ps ? "ready" : "FAILED");
    }
    if (!g_rv_ps)
        return false;

    ID3D11PixelShader* engine_ps = nullptr;
    ID3D11ClassInstance* instances[256];
    UINT instance_count = 256;
    ctx->PSGetShader(&engine_ps, instances, &instance_count);
    ID3D11RenderTargetView* engine_rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11DepthStencilView* engine_dsv = nullptr;
    ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, engine_rtvs, &engine_dsv);
    ID3D11BlendState* engine_blend = nullptr;
    float blend_factor[4];
    UINT sample_mask = 0;
    ctx->OMGetBlendState(&engine_blend, blend_factor, &sample_mask);

    ID3D11RenderTargetView* targets[2] = {rtvs[kRtvRvMask], rtvs[kRtvRvHistory]};
    ctx->OMSetRenderTargets(2, targets, nullptr);
    ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    ctx->PSSetShader(g_rv_ps, nullptr, 0);
    draw(ctx, vertex_count, start_vertex);

    ctx->PSSetShader(engine_ps, instances, instance_count);
    ctx->OMSetBlendState(engine_blend, blend_factor, sample_mask);
    ctx->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, engine_rtvs, engine_dsv);
    for (IUnknown* p : {(IUnknown*)engine_ps, (IUnknown*)engine_blend, (IUnknown*)engine_dsv})
        if (p)
            p->Release();
    for (auto* v : engine_rtvs)
        if (v)
            v->Release();
    for (UINT i = 0; i < instance_count; ++i)
        if (instances[i])
            instances[i]->Release();
    return true;
}

// RCAS of g_output into g_sharp (inside the Scope). False: sharpening off or unavailable.
static bool sharpen(ID3D11Device* device, ID3D11DeviceContext* ctx, UINT out_w, UINT out_h) {
    int percent = config().dlss_sharpness;
    if (percent <= 0)
        return false;
    if (!g_rcas_tried) {
        g_rcas_tried = true;
        g_rcas_ps = gpu::create_pixel_shader(device, kRcasPs, "rcas_ps");
        D3D11_BUFFER_DESC d{16, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0};
        if (FAILED(device->CreateBuffer(&d, nullptr, &g_rcas_cb)))
            g_rcas_cb = nullptr;
        logf("dlss_pass: RCAS sharpening %s", g_rcas_ps && g_rcas_cb ? "ready" : "FAILED");
    }
    if (!g_rcas_ps || !g_rcas_cb ||
        !make(device, g_sharp, out_w, out_h, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_RENDER_TARGET, "sharpened"))
        return false;
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ctx->Map(g_rcas_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
        return false;
    float constants[4] = {(percent > 100 ? 100 : percent) / 100.0f, 0, 0, 0};
    memcpy(m.pData, constants, sizeof(constants));
    ctx->Unmap(g_rcas_cb, 0);
    gpu::apply(ctx, g_rcas_ps, g_output.srv, g_rcas_cb, g_sharp.rtv, out_w, out_h);
    return true;
}

bool run(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* const* srvs, ID3D11RenderTargetView* const* rtvs,
         const D3D11_VIEWPORT& viewport, uint32_t frame_index, DrawFn draw, UINT vertex_count, UINT start_vertex) {
    ID3D11Device* device = nullptr;
    ctx->GetDevice(&device);
    device->Release();  // the context keeps it alive
    if (!gpu::init(device))
        return fail("GPU helpers unavailable (see log)");
    if (!srvs[kSrvDepth] || !srvs[kSrvVelocity] || !srvs[kSrvColour] || !rtvs[kRtvOutput])
        return fail("TAA bindings missing");

    UINT in_w, in_h, tgt_w, tgt_h;
    gpu::view_size(srvs[kSrvColour], &in_w, &in_h);
    gpu::view_size(rtvs[kRtvOutput], &tgt_w, &tgt_h);
    if (!in_w || !tgt_w)
        return fail("TAA inputs are not 2D textures");
    // Upscale to the backbuffer; the engine's TAA targets are render size below 100% scale.
    UINT out_w = g_display_w, out_h = g_display_h;
    if (out_w < tgt_w || out_h < tgt_h) {
        out_w = tgt_w;
        out_h = tgt_h;
    }
    bool downsample = out_w != tgt_w || out_h != tgt_h;
    UINT render_w = static_cast<UINT>(viewport.Width), render_h = static_cast<UINT>(viewport.Height);
    if (render_w == 0 || render_h == 0 || render_w > in_w || render_h > in_h) {
        render_w = in_w;
        render_h = in_h;
    }

    if (!make(device, g_colour, in_w, in_h, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_RENDER_TARGET, "colour") ||
        !make(device, g_depth, in_w, in_h, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_UNORDERED_ACCESS, "depth") ||
        !make(device, g_output, out_w, out_h, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_UNORDERED_ACCESS, "output"))
        return fail("could not create our textures");

    // Runner's Vision first, with the engine's TAA bindings: it reads the velocity texture, and running it
    // right after NGX had that texture crashed the GPU (DEVICE_HUNG within seconds; not with the debug
    // layer). It needs nothing from DLSS; if DLSS then fails, the engine's TAA draw rewrites RTV3/RTV4.
    g_status.rv_resolved = false;
    if (g_settings.rv_resolve) {
        g_status.rv_resolved = resolve_rv_mask(ctx, srvs, rtvs, draw, vertex_count, start_vertex);
        d3d_debug::drain(ctx, "RV mask port");
    }

    ID3D11Resource* velocity = nullptr;
    srvs[kSrvVelocity]->GetResource(&velocity);
    d3d_debug::drain(ctx, "engine before DLSS");

    const char* error = nullptr;
    {
        gpu::Scope scope(ctx);
        if (!scope.ok()) {
            error = "no ID3D11DeviceContext1";
        } else {
            bool recreated = false;
            if (!dlss::ensure(ctx, in_w, in_h, out_w, out_h, &recreated)) {
                error = dlss::status().available ? "DLSS feature creation failed" : "DLSS not available";
            } else {
                gpu::blit(ctx, srvs[kSrvColour], g_colour.rtv, in_w, in_h);
                gpu::copy_depth(ctx, srvs[kSrvDepth], g_depth.uav, in_w, in_h);

                jitter::Sample j = jitter::for_frame(frame_index + g_settings.jitter_frame_offset);
                bool reset = g_reset || recreated || frame_index != g_last_frame + 1;
                g_reset = false;
                g_last_frame = frame_index;

                dlss::EvalInputs in;
                in.colour = g_colour.tex;
                in.depth = g_depth.tex;
                in.motion = velocity;
                in.output = g_output.tex;
                in.render_width = render_w;
                in.render_height = render_h;
                in.jitter_x = j.valid ? g_settings.jitter_sign_x * j.x : 0.0f;
                in.jitter_y = j.valid ? g_settings.jitter_sign_y * j.y : 0.0f;
                in.mv_scale_x = g_settings.mv_sign_x * 0.5f * render_w;
                in.mv_scale_y = g_settings.mv_sign_y * 0.5f * render_h;
                in.reset = reset;

                if (!dlss::evaluate(ctx, in)) {
                    error = "DLSS evaluate failed";
                } else {
                    g_status.sharpened = sharpen(device, ctx, out_w, out_h);
                    g_final = g_status.sharpened ? &g_sharp : &g_output;
                    // Output + next-frame history (bilinear downsample while the post chain is still
                    // render size); the Runner's Vision mask passes through unresolved.
                    for (int slot : {kRtvOutput, kRtvHistory}) {
                        if (!rtvs[slot])
                            continue;
                        if (downsample)
                            gpu::blit_scaled(ctx, g_final->srv, rtvs[slot], tgt_w, tgt_h);
                        else
                            gpu::blit(ctx, g_final->srv, rtvs[slot], tgt_w, tgt_h);
                    }
                    if (srvs[kSrvRvMask] && !g_status.rv_resolved) {  // off or port unavailable: raw mask
                        for (int slot : {kRtvRvMask, kRtvRvHistory})
                            if (rtvs[slot])
                                gpu::blit(ctx, srvs[kSrvRvMask], rtvs[slot], in_w, in_h);
                    }
                    g_status.jitter_x = in.jitter_x;
                    g_status.jitter_y = in.jitter_y;
                    g_status.reset_last = reset;
                    g_status.render_width = render_w;
                    g_status.render_height = render_h;
                    g_status.output_width = out_w;
                    g_status.output_height = out_h;
                    g_status.downsampled = downsample;
                }
            }
        }
    }
    if (velocity)
        velocity->Release();
    d3d_debug::drain(ctx, "DLSS pass");
    if (error)
        return fail(error);
    g_status.error = nullptr;
    g_status.ready = true;
    g_ran_this_frame = true;
    if (g_status.downsampled)
        post::after_taa(rtvs[kRtvOutput], g_final->srv, g_status.output_width, g_status.output_height);
    return true;
}

void on_present(IDXGISwapChain* swapchain) {
    DXGI_SWAP_CHAIN_DESC desc;
    if (SUCCEEDED(swapchain->GetDesc(&desc))) {
        g_display_w = desc.BufferDesc.Width;
        g_display_h = desc.BufferDesc.Height;
    }
    bool ran = g_ran_this_frame;
    g_ran_this_frame = false;
    if (!g_settings.preview || !ran || !g_final->srv)
        return;

    ID3D11Device* device = nullptr;
    ID3D11Texture2D* backbuffer = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    if (SUCCEEDED(swapchain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&device))) &&
        SUCCEEDED(swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backbuffer))) &&
        SUCCEEDED(device->CreateRenderTargetView(backbuffer, nullptr, &rtv))) {
        device->GetImmediateContext(&ctx);
        gpu::Scope scope(ctx);
        if (scope.ok())
            gpu::preview(ctx, g_final->srv, rtv, desc.BufferDesc.Width, desc.BufferDesc.Height);
    }
    for (IUnknown* p : {(IUnknown*)ctx, (IUnknown*)rtv, (IUnknown*)backbuffer, (IUnknown*)device})
        if (p)
            p->Release();
}

}  // namespace cs::dlss_pass
