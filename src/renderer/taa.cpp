#include "renderer/taa.h"

#include <windows.h>

#include <intrin.h>

#include <MinHook.h>

#include <cmath>
#include <cstring>

#include "core/d3d_debug.h"
#include "core/log.h"
#include "core/state.h"
#include "renderer/dlss_pass.h"
#include "renderer/gpu.h"
#include "renderer/mip_bias.h"
#include "renderer/post.h"
#include "renderer/render_scale.h"
#include "upscaler/dlss.h"

namespace cs::taa {

// The engine's TAA resolve function (docs/taa.md): (rcx = TAA state, rdx = render context, r8 = view), void.
constexpr uintptr_t kTaaRva = 0x35d25a0;
// Its first 21 bytes in the running game, compared before hooking:
// mov rax,rsp; push rbp/rsi/rdi/r12-r15; lea rbp,[rax-0x858]
constexpr uint8_t kTaaPrologue[] = {0x48, 0x89, 0xe0, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41,
                                    0x56, 0x41, 0x57, 0x48, 0x8d, 0xa8, 0xa8, 0xf7, 0xff, 0xff};
constexpr uint32_t kCodeReadyTimeoutMs = 300000;
constexpr int kDrawSlot = 13;  // ID3D11DeviceContext::Draw

// TAA draw bindings (docs/taa.md): t3 colour, t5 Runner's Vision mask in;
// RTV1 output, RTV2 next-frame history, RTV3 resolved RV mask, RTV4 RV mask history.
constexpr int kSrvColour = 3, kSrvRvMask = 5;
constexpr int kRtvOutput = 1, kRtvHistory = 2, kRtvRvMask = 3, kRtvRvHistory = 4;

using TaaFn = void (*)(void*, void*, void*);
using DrawFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT);

static TaaFn o_taa;
static DrawFn o_draw;
static ID3D11DeviceContext* g_immediate;  // identity only
static uint8_t* g_taa_entry;
static thread_local int t_in_taa;
static thread_local bool t_in_draw_hook;  // our own blits/draws re-enter the Draw hook

static Status g_status;
static Mode g_mode = Mode::Engine;
static uint32_t g_calls, g_draws, g_other_draws;  // current frame
static Snapshot g_other_last;  // last non-TAA draw inside the TAA call, for logging changes only

// The TAA resolve itself: HDR colour in t3 and an HDR output in RTV1 (RGBA16F, or R11G11B10 when motion
// blur is off). Other draws in the same call (High preset: three R8 targets) bind R8 there.
static bool is_hdr(const TexInfo& t) {
    return t.bound && (t.format == DXGI_FORMAT_R16G16B16A16_FLOAT || t.format == DXGI_FORMAT_R11G11B10_FLOAT);
}

const Status& status() { return g_status; }
Mode& mode() { return g_mode; }

static bool safe_equal(const uint8_t* p, const uint8_t* expected, size_t n) {
    __try {
        return memcmp(p, expected, n) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static void read_view(const uint8_t* view) {
    __try {
        g_status.view_frame_index = *reinterpret_cast<const uint32_t*>(view + 0xac);
        g_status.view_taa_enabled = view[0xb1];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

static void hk_taa(void* taa_state, void* render_context, void* view) {
    ++g_calls;
    read_view(static_cast<const uint8_t*>(view));
    mip_bias::end_scene();
    ++t_in_taa;
    o_taa(taa_state, render_context, view);
    --t_in_taa;
}

static ID3D11Texture2D* texture_of(ID3D11View* view) {
    if (!view)
        return nullptr;
    ID3D11Resource* res = nullptr;
    view->GetResource(&res);
    ID3D11Texture2D* tex = nullptr;
    if (res) {
        res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex));
        res->Release();
    }
    return tex;
}

static TexInfo describe(ID3D11View* view, DXGI_FORMAT view_format) {
    TexInfo info;
    if (ID3D11Texture2D* tex = texture_of(view)) {
        D3D11_TEXTURE2D_DESC desc;
        tex->GetDesc(&desc);
        info.bound = true;
        info.width = desc.Width;
        info.height = desc.Height;
        info.format = view_format;
        info.texture_format = desc.Format;
        info.mips = desc.MipLevels;
        info.bind_flags = desc.BindFlags;
        info.misc_flags = desc.MiscFlags;
        tex->Release();
    }
    return info;
}

static bool same_bindings(const Snapshot& a, const Snapshot& b) {
    auto same = [](const TexInfo& x, const TexInfo& y) {
        return x.bound == y.bound && x.width == y.width && x.height == y.height && x.format == y.format &&
               x.texture_format == y.texture_format && x.mips == y.mips && x.bind_flags == y.bind_flags &&
               x.misc_flags == y.misc_flags;
    };
    for (int i = 0; i < kSrvSlots; ++i)
        if (!same(a.srv[i], b.srv[i]))
            return false;
    for (int i = 0; i < kRtvSlots; ++i)
        if (!same(a.rtv[i], b.rtv[i]))
            return false;
    return true;
}

static void log_bindings(const Snapshot& s) {
    auto line = [](const char* kind, int slot, const TexInfo& t) {
        if (t.bound)
            logf("taa:   %s%d %ux%u view fmt %d, texture fmt %d, mips %u, bind 0x%x, misc 0x%x", kind, slot, t.width,
                 t.height, (int)t.format, (int)t.texture_format, t.mips, t.bind_flags, t.misc_flags);
    };
    logf("taa: draw bindings changed (viewport %.0fx%.0f):", s.viewport.Width, s.viewport.Height);
    for (int i = 0; i < kSrvSlots; ++i)
        line("t", i, s.srv[i]);
    for (int i = 0; i < kRtvSlots; ++i)
        line("RTV", i, s.rtv[i]);
}

// Debug: raw colour into output + history, raw RV mask into its outputs (TAA without the resolve).
static bool passthrough(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* const* srvs,
                        ID3D11RenderTargetView* const* rtvs) {
    ID3D11Device* device = nullptr;
    ctx->GetDevice(&device);
    device->Release();
    if (!gpu::init(device) || !srvs[kSrvColour] || !rtvs[kRtvOutput])
        return false;
    UINT w, h;
    gpu::view_size(rtvs[kRtvOutput], &w, &h);
    gpu::Scope scope(ctx);
    if (!scope.ok())
        return false;
    gpu::blit(ctx, srvs[kSrvColour], rtvs[kRtvOutput], w, h);
    if (rtvs[kRtvHistory])
        gpu::blit(ctx, srvs[kSrvColour], rtvs[kRtvHistory], w, h);
    if (srvs[kSrvRvMask]) {
        for (int slot : {kRtvRvMask, kRtvRvHistory})
            if (rtvs[slot])
                gpu::blit(ctx, srvs[kSrvRvMask], rtvs[slot], w, h);
    }
    return true;
}

static void STDMETHODCALLTYPE hk_draw(ID3D11DeviceContext* ctx, UINT vertex_count, UINT start_vertex) {
    if (t_in_draw_hook || ctx != g_immediate) {
        o_draw(ctx, vertex_count, start_vertex);
        return;
    }
    if (t_in_taa == 0) {
        t_in_draw_hook = true;
        if (!post::active() || !post::on_draw(ctx, vertex_count, start_vertex, o_draw))
            o_draw(ctx, vertex_count, start_vertex);
        t_in_draw_hook = false;
        return;
    }
    t_in_draw_hook = true;

    ID3D11ShaderResourceView* srvs[kSrvSlots] = {};
    ID3D11RenderTargetView* rtvs[kRtvSlots] = {};
    ID3D11DepthStencilView* dsv = nullptr;
    ctx->PSGetShaderResources(0, kSrvSlots, srvs);
    ctx->OMGetRenderTargets(kRtvSlots, rtvs, &dsv);

    Snapshot snap;
    snap.valid = true;
    snap.vertex_count = vertex_count;
    for (int i = 0; i < kSrvSlots; ++i) {
        if (!srvs[i])
            continue;
        D3D11_SHADER_RESOURCE_VIEW_DESC desc;
        srvs[i]->GetDesc(&desc);
        snap.srv[i] = describe(srvs[i], desc.Format);
    }
    for (int i = 0; i < kRtvSlots; ++i) {
        if (!rtvs[i])
            continue;
        D3D11_RENDER_TARGET_VIEW_DESC desc;
        rtvs[i]->GetDesc(&desc);
        snap.rtv[i] = describe(rtvs[i], desc.Format);
    }
    if (dsv) {
        D3D11_DEPTH_STENCIL_VIEW_DESC desc;
        dsv->GetDesc(&desc);
        snap.dsv = describe(dsv, desc.Format);
    }
    UINT viewports = 1;
    ctx->RSGetViewports(&viewports, &snap.viewport);
    auto ret = reinterpret_cast<uintptr_t>(_ReturnAddress());
    uintptr_t base = state().game_base;
    snap.return_rva = ret > base && ret < base + 0x8000000 ? ret - base : 0;

    bool taa_draw = is_hdr(snap.srv[kSrvColour]) && is_hdr(snap.rtv[kRtvOutput]);
    if (!taa_draw) {
        ++g_other_draws;
        if (!g_other_last.valid || !same_bindings(snap, g_other_last)) {
            logf("taa: other draw inside the TAA call, passed through:");
            log_bindings(snap);
        }
        g_other_last = snap;
        o_draw(ctx, vertex_count, start_vertex);
    } else if (g_mode == Mode::Dlss) {
        snap.skipped = dlss_pass::run(ctx, srvs, rtvs, snap.viewport, g_status.view_frame_index, o_draw, vertex_count,
                                      start_vertex);
    } else if (g_mode == Mode::Skip) {
        snap.skipped = passthrough(ctx, srvs, rtvs);
        g_status.skip_failed = !snap.skipped;
    }
    if (taa_draw) {
        ++g_draws;
        if (!snap.skipped)
            o_draw(ctx, vertex_count, start_vertex);
        if (!g_status.last.valid || !same_bindings(snap, g_status.last))
            log_bindings(snap);
        g_status.last = snap;
    }
    for (auto* v : srvs)
        if (v)
            v->Release();
    for (auto* v : rtvs)
        if (v)
            v->Release();
    if (dsv)
        dsv->Release();
    t_in_draw_hook = false;
}

bool install_function_hook() {
    g_taa_entry = reinterpret_cast<uint8_t*>(state().game_base + kTaaRva);
    ULONGLONG start = GetTickCount64();
    while (!safe_equal(g_taa_entry, kTaaPrologue, sizeof(kTaaPrologue))) {
        if (GetTickCount64() - start > kCodeReadyTimeoutMs) {
            logf("taa: entry bytes at RVA 0x%llx never matched after %u ms: this game build differs from the "
                 "tested one, mod inactive", (unsigned long long)kTaaRva, kCodeReadyTimeoutMs);
            return false;
        }
        Sleep(5);
    }
    g_status.code_wait_ms = static_cast<uint32_t>(GetTickCount64() - start);
    state().code_matched = true;

    MH_STATUS created = MH_CreateHook(g_taa_entry, reinterpret_cast<void*>(&hk_taa), reinterpret_cast<void**>(&o_taa));
    MH_STATUS enabled = created == MH_OK ? MH_EnableHook(g_taa_entry) : created;
    logf("taa: entry %p matched after %u ms; hook %s / %s", g_taa_entry, g_status.code_wait_ms,
         MH_StatusToString(created), MH_StatusToString(enabled));
    g_status.function_hooked = enabled == MH_OK;
    return g_status.function_hooked;
}

static void install_draw_hook(IDXGISwapChain* swapchain) {
    ID3D11Device* device = nullptr;
    if (FAILED(swapchain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&device))))
        return;
    ID3D11DeviceContext* ctx = nullptr;
    device->GetImmediateContext(&ctx);
    device->Release();
    if (!ctx)
        return;
    g_immediate = ctx;
    // Hook what the game's context actually calls (RenderDoc's wrapper when it is injected).
    void* target = (*reinterpret_cast<void***>(ctx))[kDrawSlot];
    mip_bias::install(ctx);
    ctx->Release();
    MH_STATUS created = MH_CreateHook(target, reinterpret_cast<void*>(&hk_draw), reinterpret_cast<void**>(&o_draw));
    MH_STATUS enabled = created == MH_OK ? MH_EnableHook(target) : created;
    logf("taa: Draw hook on context %p at %p: %s / %s", g_immediate, target, MH_StatusToString(created),
         MH_StatusToString(enabled));
    g_status.draw_hooked = enabled == MH_OK;
}

void on_present(IDXGISwapChain* swapchain) {
    static bool draw_hook_tried;
    if (!draw_hook_tried) {
        draw_hook_tried = true;
        install_draw_hook(swapchain);
    }
    post::end_frame();
    if (g_immediate)
        d3d_debug::drain(g_immediate, "frame");
    dlss_pass::on_present(swapchain);
    // Quality modes only while DLSS can run: without it (no RTX GPU) the engine TAA would just get a lower
    // render size. Takes effect from the next frame's view setup.
    const dlss::Status& ds = dlss::status();
    render_scale::on_present(g_mode == Mode::Dlss && !(ds.init_tried && !ds.available));
    // Mip bias for the next frame: log2(render / output) while DLSS runs (0 at DLAA unless offset).
    const dlss_pass::Status& p = dlss_pass::status();
    float bias = 0.0f;
    if (g_mode == Mode::Dlss && p.ready && p.render_width && p.output_width)
        bias = std::log2(float(p.render_width) / float(p.output_width)) + mip_bias::settings().offset;
    mip_bias::begin_frame(bias);

    g_status.calls_last_frame = g_calls;
    g_status.draws_last_frame = g_draws;
    g_status.other_draws_last_frame = g_other_draws;
    g_calls = g_draws = g_other_draws = 0;

    // MinHook writes a 5-byte jmp (E9) at the entry; if something else rewrites it, say so once.
    if (g_status.function_hooked && !g_status.hook_lost && g_taa_entry[0] != 0xE9) {
        g_status.hook_lost = true;
        logf("taa: hook at %p was overwritten (first byte %02x)", g_taa_entry, g_taa_entry[0]);
    }
}

}  // namespace cs::taa
