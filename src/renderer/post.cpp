#include "renderer/post.h"

#include <windows.h>

#include <d3d11_1.h>

#include "core/d3d_debug.h"
#include "core/log.h"
#include "core/state.h"
#include "renderer/gpu.h"
#include "renderer/motion_blur_ps.h"

namespace cs::post {

// Each pass = two adjacent return addresses: the pass function's call into its draw code, then the post
// function's call into the pass. The post frame alone is not enough: +0x37468a0 is also on the
// stack of every exposure/bloom draw. Each pair matches exactly one draw per frame.
struct Signature {
    uintptr_t pass_rva, post_rva;
};
constexpr Signature kMotionBlur{0x35c16da, 0x3745d6e};
constexpr Signature kTonemap{0x35bf90c, 0x37468a0};
constexpr Signature kLdr2{0x35c0a5e, 0x3747ec4};
constexpr Signature kResample{0x35c3524, 0x374809b};
// Map screen only: LDR depth of field composite, alpha-blended onto the LDR target (blend state:
// colour SrcAlpha/InvSrcAlpha, alpha Zero/One, depth off).
constexpr Signature kDof{0x35bcacb, 0x35bff15};
constexpr int kStackDepth = 12;
constexpr int kMaxSrvs = 16;

enum class Pass { Other, MotionBlur, Tonemap, Ldr2, Dof, Resample };

struct Target {
    ID3D11Texture2D* tex = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    UINT width = 0, height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
};

// This frame's state; references are only identities (the engine and our targets keep them alive)
// except engine_ldr_rtv, which we AddRef and release at Present.
struct Frame {
    bool active = false;
    ID3D11Resource* taa_out = nullptr;           // TAA RTV1 resource (motion blur's input when it is on)
    ID3D11ShaderResourceView* upscaled = nullptr;  // D
    UINT out_w = 0, out_h = 0;
    ID3D11Resource* chain = nullptr;             // render-size HDR mip chain tonemap reads
    ID3D11ShaderResourceView* post_src = nullptr;  // M: what tonemap reads instead
    ID3D11RenderTargetView* engine_ldr_rtv = nullptr;
    ID3D11Resource* engine_ldr = nullptr;        // its texture (identity)
    // L changed since the engine's render-size LDR target was last refilled from it. Engine passes after
    // tonemap may read that target (map DoF downsample, UI background blur).
    bool ldr_dirty = false;
    bool failed = false;
    Status status;
};

static Settings g_settings;
// Motion blur at output size runs our port of the engine shader (motion_blur_ps.h) with invPixelSize
// from our own b1; the engine's $Globals slice stays bound untouched.
static ID3D11PixelShader* g_mb_ps;
static ID3D11Buffer* g_mb_cb;  // 16 B: invPixelSize = 1/output
static bool g_mb_init_tried;
static const char* g_device_lost_after;
static Status g_status;
static Frame g_frame;
static Target g_mb;   // M (RGBA16F, output size)
static Target g_ldr;  // L (engine LDR format, output size)

Settings& settings() { return g_settings; }
const Status& status() { return g_status; }
// After a failure the rest of the frame takes the engine path, except that once tonemap went to L the
// resample must still copy L out (the engine LDR target was not written).
bool active() { return g_frame.active && (!g_frame.failed || g_frame.status.tonemap_done); }

static void destroy(Target& t) {
    for (IUnknown* p : {(IUnknown*)t.rtv, (IUnknown*)t.srv, (IUnknown*)t.tex})
        if (p)
            p->Release();
    t = Target{};
}

static bool make(ID3D11DeviceContext* ctx, Target& t, UINT width, UINT height, DXGI_FORMAT format, const char* name) {
    if (t.tex && t.width == width && t.height == height && t.format == format)
        return true;
    destroy(t);
    ID3D11Device* device = nullptr;
    ctx->GetDevice(&device);
    D3D11_TEXTURE2D_DESC d{};
    d.Width = width;
    d.Height = height;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = format;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    HRESULT hr = device->CreateTexture2D(&d, nullptr, &t.tex);
    if (SUCCEEDED(hr))
        hr = device->CreateShaderResourceView(t.tex, nullptr, &t.srv);
    if (SUCCEEDED(hr))
        hr = device->CreateRenderTargetView(t.tex, nullptr, &t.rtv);
    device->Release();
    if (FAILED(hr)) {
        logf("post: creating %s %ux%u format %d failed 0x%08lx", name, width, height, (int)format, hr);
        destroy(t);
        return false;
    }
    t.width = width;
    t.height = height;
    t.format = format;
    logf("post: %s %ux%u format %d", name, width, height, (int)format);
    return true;
}

// What the mod did in the last few frames, dumped when the device is lost (the GPU fault surfaces a
// frame or more after the work that caused it). Each step is also logged the first time it happens.
constexpr int kTraceSteps = 24;
constexpr int kTraceFrames = 4;
struct Trace {
    const char* steps[kTraceSteps];
    int n = 0;
};
static Trace g_trace[kTraceFrames];
static int g_trace_cur;
static const char* g_seen_steps[32];
static int g_seen_count;

static void trace(const char* step) {
    Trace& t = g_trace[g_trace_cur];
    if (t.n < kTraceSteps)
        t.steps[t.n++] = step;
    else
        t.steps[kTraceSteps - 1] = "(more)";
    for (int i = 0; i < g_seen_count; ++i)
        if (g_seen_steps[i] == step)
            return;
    if (g_seen_count < (int)std::size(g_seen_steps))
        g_seen_steps[g_seen_count++] = step;
    logf("post: first %s", step);
}

static void dump_trace() {
    for (int k = kTraceFrames - 1; k >= 0; --k) {
        const Trace& t = g_trace[(g_trace_cur + kTraceFrames - k) % kTraceFrames];
        char line[1024];
        int len = snprintf(line, sizeof(line), "post: frame -%d:", k);
        for (int i = 0; i < t.n && len < (int)sizeof(line); ++i)
            len += snprintf(line + len, sizeof(line) - len, " %s,", t.steps[i]);
        logf("%s", line);
    }
}

// Call stack (game RVAs) of an unknown draw touching the engine LDR target, once per distinct stack.
static void log_unknown_draw(const char* what) {
    static uint64_t seen[16];
    static int seen_count;
    void* frames[kStackDepth];
    USHORT n = RtlCaptureStackBackTrace(0, kStackDepth, frames, nullptr);
    uintptr_t base = state().game_base;
    uint64_t hash = 1469598103934665603ull;
    for (USHORT i = 0; i < n; ++i)
        hash = (hash ^ reinterpret_cast<uintptr_t>(frames[i])) * 1099511628211ull;
    for (int i = 0; i < seen_count; ++i)
        if (seen[i] == hash)
            return;
    if (seen_count == (int)std::size(seen))
        return;
    seen[seen_count++] = hash;
    char line[512];
    int len = snprintf(line, sizeof(line), "post: unknown draw %s, stack:", what);
    for (USHORT i = 0; i < n && len < (int)sizeof(line); ++i) {
        uintptr_t a = reinterpret_cast<uintptr_t>(frames[i]);
        if (a >= base && a < base + 0x8000000)
            len += snprintf(line + len, sizeof(line) - len, " +0x%llx", (unsigned long long)(a - base));
    }
    logf("%s", line);
}

static bool fail(const char* why) {
    if (g_frame.status.error != why && g_status.error != why)
        logf("post: falling back to render-size post this frame: %s", why);
    g_frame.status.error = why;
    g_frame.failed = true;
    return false;
}

static void check_device(ID3D11DeviceContext* ctx, const char* pass) {
    d3d_debug::drain(ctx, pass);
    if (g_device_lost_after)
        return;
    ID3D11Device* device = nullptr;
    ctx->GetDevice(&device);
    HRESULT reason = device->GetDeviceRemovedReason();
    device->Release();
    if (reason != S_OK) {
        g_device_lost_after = pass;
        logf("post: device removed (reason 0x%08lx) first seen after %s; last frames, oldest first:", reason,
             pass);
        dump_trace();
    }
}

static Pass identify() {
    void* frames[kStackDepth];
    USHORT n = RtlCaptureStackBackTrace(0, kStackDepth, frames, nullptr);
    uintptr_t base = state().game_base;
    auto at = [&](USHORT i, const Signature& sig) {
        return reinterpret_cast<uintptr_t>(frames[i]) == base + sig.pass_rva &&
               reinterpret_cast<uintptr_t>(frames[i + 1]) == base + sig.post_rva;
    };
    for (USHORT i = 0; i + 1 < n; ++i) {
        if (at(i, kMotionBlur))
            return Pass::MotionBlur;
        if (at(i, kTonemap))
            return Pass::Tonemap;
        if (at(i, kLdr2))
            return Pass::Ldr2;
        if (at(i, kResample))
            return Pass::Resample;
        if (at(i, kDof))
            return Pass::Dof;
    }
    return Pass::Other;
}

// The engine's output-merger / rasterizer state around one redirected draw.
struct Saved {
    ID3D11RenderTargetView* rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11DepthStencilView* dsv = nullptr;
    D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
    UINT viewport_count = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    D3D11_RECT scissors[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
    UINT scissor_count = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;

    explicit Saved(ID3D11DeviceContext* ctx) {
        ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, &dsv);
        ctx->RSGetViewports(&viewport_count, viewports);
        ctx->RSGetScissorRects(&scissor_count, scissors);
    }
    ~Saved() {
        for (auto* v : rtvs)
            if (v)
                v->Release();
        if (dsv)
            dsv->Release();
    }
    UINT rtv_count() const {
        UINT n = 0;
        for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i)
            if (rtvs[i])
                n = i + 1;
        return n;
    }
    // RTV0 = rtv, viewport/scissor = width x height. The depth buffer (render size) is unbound: D3D
    // drops an OMSetRenderTargets whose RTV and DSV sizes differ, which left tonemap drawing into the
    // engine target and our L black. Other RTVs must match too, so only RTV0 is kept.
    void redirect(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv, UINT width, UINT height) const {
        static bool logged;
        if (!logged && (dsv || rtv_count() > 1)) {
            logged = true;
            logf("post: redirect drops %s%u extra RTVs", dsv ? "the DSV and " : "", rtv_count() ? rtv_count() - 1 : 0);
        }
        ctx->OMSetRenderTargets(1, &rtv, nullptr);
        D3D11_VIEWPORT vp = viewport_count ? viewports[0] : D3D11_VIEWPORT{0, 0, 0, 0, 0, 1};
        vp.TopLeftX = vp.TopLeftY = 0;
        vp.Width = float(width);
        vp.Height = float(height);
        ctx->RSSetViewports(1, &vp);
        if (scissor_count) {
            D3D11_RECT rect{0, 0, LONG(width), LONG(height)};
            ctx->RSSetScissorRects(1, &rect);
        }
    }
    void restore(ID3D11DeviceContext* ctx) const {
        ctx->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, dsv);
        ctx->RSSetViewports(viewport_count, viewports);
        ctx->RSSetScissorRects(scissor_count, scissors);
    }
};

// Bound PS SRVs (references released by the destructor).
struct Srvs {
    ID3D11ShaderResourceView* v[kMaxSrvs] = {};
    explicit Srvs(ID3D11DeviceContext* ctx) { ctx->PSGetShaderResources(0, kMaxSrvs, v); }
    ~Srvs() {
        for (auto* s : v)
            if (s)
                s->Release();
    }
    int slot_of(ID3D11Resource* res) const {
        for (int i = 0; i < kMaxSrvs; ++i)
            if (v[i] && gpu::resource_of(v[i]) == res)
                return i;
        return -1;
    }
};

// True when the bound draw writes nothing but RTV0, so the engine's own draw can run as well as our
// redirected copy (a UAV side effect would happen twice). Logged once per pass name.
static bool only_rtv0(ID3D11DeviceContext* ctx, const Saved& saved, const char* name) {
    ID3D11UnorderedAccessView* uavs[D3D11_1_UAV_SLOT_COUNT] = {};
    ctx->OMGetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 0, D3D11_1_UAV_SLOT_COUNT, uavs);
    bool has_uav = false;
    for (auto* u : uavs)
        if (u) {
            has_uav = true;
            u->Release();
        }
    bool ok = !has_uav && saved.rtv_count() == 1;
    static const char* logged[8];
    static int logged_count;
    for (int i = 0; i < logged_count; ++i)
        if (logged[i] == name)
            return ok;
    if (logged_count < (int)std::size(logged))
        logged[logged_count++] = name;
    logf("post: %s %s the render-size LDR target itself (%u RTVs, UAVs %d)", name, ok ? "also fills" : "cannot fill",
         saved.rtv_count(), has_uav);
    return ok;
}

static bool init_motion_blur(ID3D11DeviceContext* ctx) {
    if (g_mb_init_tried)
        return g_mb_ps && g_mb_cb;
    g_mb_init_tried = true;
    ID3D11Device* device = nullptr;
    ctx->GetDevice(&device);
    g_mb_ps = gpu::create_pixel_shader(device, kMotionBlurPs, "motion_blur_ps");
    D3D11_BUFFER_DESC d{};
    d.ByteWidth = 16;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(device->CreateBuffer(&d, nullptr, &g_mb_cb)))
        g_mb_cb = nullptr;
    device->Release();
    logf("post: motion blur shader port %s", g_mb_ps && g_mb_cb ? "ready" : "FAILED");
    return g_mb_ps && g_mb_cb;
}

static bool motion_blur(ID3D11DeviceContext* ctx, UINT vertex_count, UINT start_vertex, DrawFn draw) {
    Frame& f = g_frame;
    if (f.status.motion_blur_seen)
        return fail("motion blur signature matched twice in one frame");
    if (!g_settings.motion_blur) {
        f.status.motion_blur_seen = true;
        return false;
    }
    f.status.motion_blur_seen = true;
    Srvs srvs(ctx);
    int slot = srvs.slot_of(f.taa_out);
    if (slot != 1)  // our port declares mainTexture at t1 like the engine's PS 41401
        return fail("motion blur does not read the TAA output at t1");
    Saved saved(ctx);
    ID3D11RenderTargetView* chain_rtv = saved.rtvs[0];
    if (!chain_rtv)
        return fail("motion blur has no RTV0");
    UINT chain_w, chain_h;
    gpu::view_size(chain_rtv, &chain_w, &chain_h);
    if (!make(ctx, g_mb, f.out_w, f.out_h, DXGI_FORMAT_R16G16B16A16_FLOAT, "motion blur output"))
        return fail("could not create the motion blur target");

    if (!init_motion_blur(ctx))
        return fail("motion blur shader port unavailable (see log)");
    float inv[4] = {1.0f / f.out_w, 1.0f / f.out_h, 0, 0};
    ctx->UpdateSubresource(g_mb_cb, 0, nullptr, inv, 0, 0);

    ID3D11PixelShader* engine_ps = nullptr;
    ID3D11ClassInstance* instances[256];
    UINT instance_count = 256;
    ctx->PSGetShader(&engine_ps, instances, &instance_count);
    ID3D11DeviceContext1* ctx1 = nullptr;
    ID3D11Buffer* engine_cb1 = nullptr;
    UINT cb1_first = 0, cb1_num = 0;
    if (SUCCEEDED(ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&ctx1))))
        ctx1->PSGetConstantBuffers1(1, 1, &engine_cb1, &cb1_first, &cb1_num);

    ctx->PSSetShader(g_mb_ps, nullptr, 0);
    ctx->PSSetConstantBuffers(1, 1, &g_mb_cb);
    ctx->PSSetShaderResources(slot, 1, &f.upscaled);
    saved.redirect(ctx, g_mb.rtv, f.out_w, f.out_h);
    draw(ctx, vertex_count, start_vertex);
    ctx->PSSetShaderResources(slot, 1, &srvs.v[slot]);
    saved.restore(ctx);
    ctx->PSSetShader(engine_ps, instances, instance_count);
    if (ctx1 && engine_cb1)
        ctx1->PSSetConstantBuffers1(1, 1, &engine_cb1, &cb1_first, &cb1_num);
    else
        ctx->PSSetConstantBuffers(1, 1, &engine_cb1);
    if (engine_cb1)
        engine_cb1->Release();
    if (ctx1)
        ctx1->Release();
    if (engine_ps)
        engine_ps->Release();
    for (UINT i = 0; i < instance_count; ++i)
        if (instances[i])
            instances[i]->Release();

    // The render-size chain stays filled for exposure, bloom and next frame's SSR.
    {
        gpu::Scope scope(ctx);
        if (scope.ok())
            gpu::blit_scaled(ctx, g_mb.srv, chain_rtv, chain_w, chain_h);
    }
    f.chain = gpu::resource_of(chain_rtv);
    f.post_src = g_mb.srv;
    f.status.motion_blur_done = true;
    check_device(ctx, "motion blur");
    return true;
}

static bool tonemap(ID3D11DeviceContext* ctx, UINT vertex_count, UINT start_vertex, DrawFn draw) {
    Frame& f = g_frame;
    if (f.status.tonemap_done)
        return fail("tonemap signature matched twice in one frame");
    bool keep_source = false;  // tonemap reads the engine's render-size chain and upsamples it by UV
    if (f.status.motion_blur_seen && !f.status.motion_blur_done) {
        if (g_settings.motion_blur || f.failed)
            return false;  // motion blur ran at render size after a failure: keep the engine path
        keep_source = true;
    }
    if (!f.status.motion_blur_seen) {  // motion blur off: TAA wrote the chain itself
        f.chain = f.taa_out;
        f.post_src = f.upscaled;
    }
    Srvs srvs(ctx);
    int slot = keep_source ? -1 : srvs.slot_of(f.chain);
    if (slot < 0 && !keep_source)
        return fail("tonemap does not read the HDR chain");
    Saved saved(ctx);
    ID3D11RenderTargetView* ldr_rtv = saved.rtvs[0];
    if (!ldr_rtv)
        return fail("tonemap has no RTV0");
    D3D11_RENDER_TARGET_VIEW_DESC rd;
    ldr_rtv->GetDesc(&rd);
    if (!make(ctx, g_ldr, f.out_w, f.out_h, rd.Format, "LDR output"))
        return fail("could not create the LDR target");

    // The engine's own draw first, untouched: its render-size LDR target then holds what later engine
    // passes expect (the DoF downsample reads it mid-frame) without a refill. A refill there ran
    // inside the downsample's draw call, and an engine draw issued straight after our SwapDeviceContextState
    // round trip hung the GPU within seconds (slides; same shape as the RV port after DLSS).
    bool engine_draw = only_rtv0(ctx, saved, "tonemap");
    if (engine_draw)
        draw(ctx, vertex_count, start_vertex);

    if (slot >= 0)
        ctx->PSSetShaderResources(slot, 1, &f.post_src);
    saved.redirect(ctx, g_ldr.rtv, f.out_w, f.out_h);
    draw(ctx, vertex_count, start_vertex);
    if (slot >= 0)
        ctx->PSSetShaderResources(slot, 1, &srvs.v[slot]);
    saved.restore(ctx);

    ldr_rtv->AddRef();
    f.engine_ldr_rtv = ldr_rtv;
    f.engine_ldr = gpu::resource_of(ldr_rtv);
    f.ldr_dirty = !engine_draw;
    f.status.tonemap_done = true;
    check_device(ctx, "tonemap");
    return true;
}

// Draws that add onto the LDR target after tonemap (LDR2 in gameplay/pause, the DoF composite in the
// map): RTV0 + viewport -> L at output size; shaders, constants, SRVs and the engine's blend state stay.
// Both are UV-addressed, so no constant changes. Like tonemap, the engine's own draw also runs onto its
// render-size target, so later readers of that target (the loading-screen resample) need no refill:
// a refill there ran inside their draw call and hung the GPU below 100% render scale (issue #1).
static bool onto_ldr(ID3D11DeviceContext* ctx, UINT vertex_count, UINT start_vertex, DrawFn draw, const char* pass,
                     const char* name, bool* done) {
    Frame& f = g_frame;
    if (!f.status.tonemap_done)
        return false;
    Saved saved(ctx);
    if (gpu::resource_of(saved.rtvs[0]) != f.engine_ldr)
        return fail(name);
    bool engine_draw = only_rtv0(ctx, saved, pass);
    if (engine_draw)
        draw(ctx, vertex_count, start_vertex);
    saved.redirect(ctx, g_ldr.rtv, f.out_w, f.out_h);
    draw(ctx, vertex_count, start_vertex);
    saved.restore(ctx);
    if (!engine_draw)
        f.ldr_dirty = true;
    *done = true;
    check_device(ctx, name);
    return true;
}

// Refills the engine's render-size LDR target from L (bilinear downsample).
static void refill_engine_ldr(ID3D11DeviceContext* ctx) {
    Frame& f = g_frame;
    UINT lw, lh;
    gpu::view_size(f.engine_ldr_rtv, &lw, &lh);
    gpu::Scope scope(ctx);
    if (!scope.ok())
        return;
    trace("LDR refill");
    gpu::blit_scaled(ctx, g_ldr.srv, f.engine_ldr_rtv, lw, lh);
    f.ldr_dirty = false;
    ++f.status.ldr_refills;
    d3d_debug::drain(ctx, "LDR refill");
}

// Any other draw after tonemap: note it if it reads the engine LDR target, count it if it writes it (an
// unknown pass whose output the swapchain copy from L would lose). Never refill here: the engine draw
// would run straight after our Scope round trip, which hangs the GPU (DEVICE_HUNG). If the target is
// stale (a known pass could not also run the engine draw), the reader sees the older contents.
static void other_draw(ID3D11DeviceContext* ctx) {
    Frame& f = g_frame;
    if (!f.status.tonemap_done)
        return;
    {
        Srvs srvs(ctx);
        if (srvs.slot_of(f.engine_ldr) >= 0) {
            trace(f.ldr_dirty ? "unknown draw reads stale LDR" : "unknown draw reads LDR");
            log_unknown_draw("reading the engine LDR target");
            if (f.ldr_dirty)
                ++f.status.stale_ldr_reads;
        }
    }
    ID3D11RenderTargetView* rtv = nullptr;
    ctx->OMGetRenderTargets(1, &rtv, nullptr);
    if (rtv) {
        if (gpu::resource_of(rtv) == f.engine_ldr) {
            ++f.status.other_ldr_writes;
            trace("unknown draw writes LDR");
            log_unknown_draw("writing the engine LDR target");
        }
        rtv->Release();
    }
}

static bool resample(ID3D11DeviceContext* ctx) {
    Frame& f = g_frame;
    if (!f.status.tonemap_done)
        return false;
    Saved saved(ctx);
    ID3D11RenderTargetView* swap_rtv = saved.rtvs[0];
    if (!swap_rtv)
        return false;
    UINT sw, sh;
    gpu::view_size(swap_rtv, &sw, &sh);
    {
        gpu::Scope scope(ctx);
        if (!scope.ok())
            return false;
        // Replaces the engine's LDR upscale.
        if (sw == g_ldr.width && sh == g_ldr.height)
            gpu::blit(ctx, g_ldr.srv, swap_rtv, sw, sh);
        else
            gpu::blit_scaled(ctx, g_ldr.srv, swap_rtv, sw, sh);
    }
    // The render-size copy feeds the UI background blur; skip it if nothing changed L since the
    // last refill.
    if (f.ldr_dirty)
        refill_engine_ldr(ctx);
    f.status.resample_done = true;
    check_device(ctx, "resample");
    return true;
}

void after_taa(ID3D11RenderTargetView* taa_output, ID3D11ShaderResourceView* upscaled, UINT out_width,
               UINT out_height) {
    if (!g_settings.enabled)
        return;
    Frame& f = g_frame;
    f.active = true;
    f.status.active = true;
    f.taa_out = gpu::resource_of(taa_output);
    f.upscaled = upscaled;
    f.out_w = out_width;
    f.out_h = out_height;
}

bool on_draw(ID3D11DeviceContext* ctx, UINT vertex_count, UINT start_vertex, DrawFn draw) {
    ++g_frame.status.post_draws;
    Pass pass = identify();
    if (pass != Pass::Other)
        d3d_debug::drain(ctx, "engine before post pass");
    static const char* const kPassNames[] = {"other", "motion blur", "tonemap", "LDR2", "DoF composite", "resample"};
    if (pass != Pass::Other && g_frame.active)
        trace(kPassNames[static_cast<int>(pass)]);
    switch (pass) {
    case Pass::MotionBlur: return motion_blur(ctx, vertex_count, start_vertex, draw);
    case Pass::Tonemap: return tonemap(ctx, vertex_count, start_vertex, draw);
    case Pass::Ldr2:
        return onto_ldr(ctx, vertex_count, start_vertex, draw, "LDR2", "LDR2 does not write the LDR target",
                        &g_frame.status.ldr2_done);
    case Pass::Dof:
        return onto_ldr(ctx, vertex_count, start_vertex, draw, "DoF composite",
                        "DoF composite does not write the LDR target",
                        &g_frame.status.dof_done);
    case Pass::Resample: return resample(ctx);
    default: other_draw(ctx); return false;
    }
}

void end_frame() {
    Frame& f = g_frame;
    if (f.active) {
        bool clean = !f.failed && f.status.tonemap_done && f.status.resample_done;
        if (clean)
            f.status.error = nullptr;
        else if (!f.status.error)
            f.status.error = !f.status.tonemap_done ? "tonemap pass not seen" : "resample pass not seen";
        g_status = f.status;
        g_status.device_lost_after = g_device_lost_after;
        g_status.mb_shader = g_mb_ps ? "ready" : g_mb_init_tried ? "FAILED" : "not loaded";
    } else {
        g_status = Status{};
    }
    if (f.engine_ldr_rtv)
        f.engine_ldr_rtv->Release();
    f = Frame{};
    if (g_trace[g_trace_cur].n) {
        g_trace_cur = (g_trace_cur + 1) % kTraceFrames;
        g_trace[g_trace_cur].n = 0;
    }
}

}  // namespace cs::post
