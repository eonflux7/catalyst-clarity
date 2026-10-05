// ImGui overlay drawn into the game's back buffer just before Present.
//
// Threads: only on_present (render thread) calls ImGui. wndproc just queues the messages ImGui
// needs; on_present replays them before NewFrame with no lock held. Calling ImGui from wndproc
// under a lock deadlocked: ImGui's SetCapture re-enters wndproc synchronously (WM_CAPTURECHANGED).
#include "ui/overlay.h"

#include <windows.h>

#include <d3d11.h>

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include <atomic>
#include <mutex>
#include <vector>

#include "core/config.h"
#include "core/log.h"
#include "core/state.h"
#include "renderer/dlss_pass.h"
#include "renderer/mip_bias.h"
#include "renderer/post.h"
#include "renderer/render_scale.h"
#include "renderer/taa.h"
#include "upscaler/dlss.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace cs::overlay {

// Guards g_rtv and ImGui rendering. Recursive: ResizeBuffers can re-enter on the render thread.
static std::recursive_mutex g_render_mutex;
static bool g_initialized;
static bool g_failed;
static IDXGISwapChain* g_swapchain;  // identity only, no reference held
static HWND g_window;
static WNDPROC g_game_wndproc;
static ID3D11Device* g_device;
static ID3D11DeviceContext* g_context;
static ID3D11RenderTargetView* g_rtv;
static std::atomic<bool> g_visible;
static bool g_toggle_down;

static uint64_t g_frames;
static LARGE_INTEGER g_qpc_freq, g_qpc_last;
static float g_frame_ms;

struct QueuedMessage {
    UINT msg;
    WPARAM wparam;
    LPARAM lparam;
};
static std::mutex g_queue_mutex;  // only ever held for a push or a swap, never around Win32 calls
static std::vector<QueuedMessage> g_queue;
static std::atomic<DWORD> g_wndproc_thread;

static bool is_input_message(UINT msg) {
    return (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) || (msg >= WM_KEYFIRST && msg <= WM_KEYLAST) ||
           msg == WM_INPUT;
}

// Messages ImGui's Win32 backend consumes (WM_INPUT is not one of them).
static bool imgui_wants(UINT msg) {
    return (is_input_message(msg) && msg != WM_INPUT) || msg == WM_MOUSELEAVE || msg == WM_SETFOCUS ||
           msg == WM_KILLFOCUS;
}

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (!g_wndproc_thread.exchange(GetCurrentThreadId()))
        logf("overlay: window messages on thread %lu", GetCurrentThreadId());
    if (g_visible) {
        if (imgui_wants(msg)) {
            std::lock_guard lock(g_queue_mutex);
            if (g_queue.size() < 1024)
                g_queue.push_back({msg, wparam, lparam});
        }
        // While the overlay is open the game gets no mouse/keyboard input.
        if (is_input_message(msg))
            return msg == WM_INPUT ? DefWindowProcW(hwnd, msg, wparam, lparam) : 0;
    }
    return CallWindowProcW(g_game_wndproc, hwnd, msg, wparam, lparam);
}

// Feeds queued window messages to ImGui on the render thread. No lock is held while ImGui runs, so
// anything it does that re-enters wndproc (SetCapture → WM_CAPTURECHANGED) only queues.
static void drain_messages() {
    std::vector<QueuedMessage> messages;
    {
        std::lock_guard lock(g_queue_mutex);
        messages.swap(g_queue);
    }
    for (const QueuedMessage& m : messages)
        ImGui_ImplWin32_WndProcHandler(g_window, m.msg, m.wparam, m.lparam);
}

static bool init(IDXGISwapChain* swapchain) {
    if (FAILED(swapchain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&g_device)))) {
        logf("overlay: swapchain has no D3D11 device, overlay disabled");
        return false;
    }
    g_device->GetImmediateContext(&g_context);
    DXGI_SWAP_CHAIN_DESC desc;
    swapchain->GetDesc(&desc);
    g_window = desc.OutputWindow;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // no imgui.ini in the game folder; settings live in catalyst_clarity.ini
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
    ImGui_ImplWin32_Init(g_window);
    ImGui_ImplDX11_Init(g_device, g_context);

    g_game_wndproc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(g_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&wndproc)));
    g_swapchain = swapchain;
    g_visible = config().show_on_start;
    QueryPerformanceFrequency(&g_qpc_freq);
    QueryPerformanceCounter(&g_qpc_last);
    logf("overlay: initialized, window %p, back buffer %ux%u fmt %d, toggle key 0x%x", g_window,
         desc.BufferDesc.Width, desc.BufferDesc.Height, (int)desc.BufferDesc.Format, config().toggle_key);
    return true;
}

static void poll_toggle_key() {
    bool down = GetForegroundWindow() == g_window && (GetAsyncKeyState((int)config().toggle_key) & 0x8000);
    if (down && !g_toggle_down)
        g_visible = !g_visible;
    g_toggle_down = down;
}

static void update_timing() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    float ms = float(now.QuadPart - g_qpc_last.QuadPart) * 1000.0f / float(g_qpc_freq.QuadPart);
    g_qpc_last = now;
    g_frame_ms = g_frame_ms == 0.0f ? ms : g_frame_ms * 0.95f + ms * 0.05f;
    ++g_frames;
}

static void draw_fps_corner() {
    ImGui::SetNextWindowPos(ImVec2(8, 8));
    ImGui::SetNextWindowBgAlpha(0.4f);
    ImGui::Begin("##fps", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
    ImGui::Text("%.1f fps  %.2f ms", g_frame_ms > 0 ? 1000.0f / g_frame_ms : 0.0f, g_frame_ms);
    ImGui::End();
}

static const char* format_name(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R11G11B10_FLOAT: return "R11G11B10_FLOAT";
    case DXGI_FORMAT_R16G16_FLOAT: return "R16G16_FLOAT";
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return "R16G16B16A16_FLOAT";
    case DXGI_FORMAT_R8_UNORM: return "R8_UNORM";
    case DXGI_FORMAT_R8G8_UNORM: return "R8G8_UNORM";
    case DXGI_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
    case DXGI_FORMAT_R10G10B10A2_UNORM: return "R10G10B10A2_UNORM";
    case DXGI_FORMAT_R32_FLOAT: return "R32_FLOAT";
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS: return "R32_FLOAT_X8X24";
    case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT: return "X32_G8X24_UINT";
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return "D32_FLOAT_S8X24";
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS: return "R24_UNORM_X8";
    default: return nullptr;
    }
}

// Expected roles (docs/taa.md).
static const char* const kSrvRoles[taa::kSrvSlots] = {"depth", "velocity", "", "colour", "", "RV mask", "RV history", ""};
static const char* const kRtvRoles[taa::kRtvSlots] = {"dead R8", "output", "history out", "RV mask out", "RV history out"};

static void binding_row(const char* slot, const char* role, const taa::TexInfo& t) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(slot);
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(role);
    ImGui::TableNextColumn();
    if (!t.bound) {
        ImGui::TextDisabled("-");
        ImGui::TableNextColumn();
        return;
    }
    ImGui::Text("%ux%u", t.width, t.height);
    ImGui::TableNextColumn();
    if (const char* name = format_name(t.format))
        ImGui::TextUnformatted(name);
    else
        ImGui::Text("format %d", (int)t.format);
}

static void draw_taa_section() {
    const taa::Status& t = taa::status();
    ImGui::Text("Function hook: %s (code ready after %u ms)%s", t.function_hooked ? "ok" : "NOT INSTALLED",
                t.code_wait_ms, t.hook_lost ? ", HOOK OVERWRITTEN" : "");
    ImGui::Text("Draw hook: %s", t.draw_hooked ? "ok" : "NOT INSTALLED");
    ImGui::Text("Last frame: %u TAA calls, %u TAA draws, %u other draws inside (passed through)",
                t.calls_last_frame, t.draws_last_frame, t.other_draws_last_frame);
    ImGui::Text("View: frame index %u, TAA enabled %u", t.view_frame_index, t.view_taa_enabled);

    int mode = static_cast<int>(taa::mode());
    ImGui::TextUnformatted("Anti-aliasing:");
    ImGui::SameLine();
    bool changed = ImGui::RadioButton("Engine TAA", &mode, static_cast<int>(taa::Mode::Engine));
    ImGui::SameLine();
    changed |= ImGui::RadioButton("Skip (raw)", &mode, static_cast<int>(taa::Mode::Skip));
    ImGui::SameLine();
    changed |= ImGui::RadioButton("DLSS##mode", &mode, static_cast<int>(taa::Mode::Dlss));
    if (changed) {
        taa::mode() = static_cast<taa::Mode>(mode);
        dlss_pass::request_reset();
        config().aa_mode = mode;
        config_save();
    }
    if (taa::mode() == taa::Mode::Skip && t.skip_failed)
        ImGui::TextColored(ImVec4(1, 0.4f, 0.3f, 1), "Passthrough failed (see log)");

    const taa::Snapshot& s = t.last;
    if (!s.valid) {
        ImGui::TextDisabled("No TAA draw seen yet");
        return;
    }
    ImGui::Text("Draw(%u verts), caller +0x%llx, viewport %.0fx%.0f%s", s.vertex_count,
                (unsigned long long)s.return_rva, s.viewport.Width, s.viewport.Height, s.skipped ? ", SKIPPED" : "");
    if (ImGui::BeginTable("taa_bindings", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("Slot");
        ImGui::TableSetupColumn("Expected");
        ImGui::TableSetupColumn("Size");
        ImGui::TableSetupColumn("View format");
        ImGui::TableHeadersRow();
        char slot[16];
        for (int i = 0; i < taa::kSrvSlots; ++i) {
            snprintf(slot, sizeof(slot), "PS t%d", i);
            binding_row(slot, kSrvRoles[i], s.srv[i]);
        }
        for (int i = 0; i < taa::kRtvSlots; ++i) {
            snprintf(slot, sizeof(slot), "RTV %d", i);
            binding_row(slot, kRtvRoles[i], s.rtv[i]);
        }
        binding_row("DSV", "", s.dsv);
        ImGui::EndTable();
    }
}

static void sign_toggle(const char* label, int* sign) {
    bool negative = *sign < 0;
    if (ImGui::Checkbox(label, &negative))
        *sign = negative ? -1 : 1;
}

static void draw_dlss_section() {
    const dlss::Status& d = dlss::status();
    const dlss_pass::Status& p = dlss_pass::status();
    if (d.optiscaler_module[0])
        ImGui::Text("OptiScaler %s (%s): its menu (Insert) picks the upscaler", d.optiscaler_version,
                    d.optiscaler_module);
    if (!d.init_tried) {
        ImGui::TextDisabled("Not initialised (select DLSS above)");
    } else if (!d.available) {
        ImGui::TextColored(ImVec4(1, 0.4f, 0.3f, 1), "DLSS unavailable: result 0x%08x%s", d.init_result,
                           d.needs_driver ? ", driver update needed" : "");
    } else {
        ImGui::Text("Feature: %s (create 0x%08x), %ux%u -> %ux%u%s", d.feature_created ? "ok" : "FAILED",
                    d.create_result, d.in_width, d.in_height, d.out_width, d.out_height,
                    d.in_width == d.out_width ? " (DLAA)" : "");
        ImGui::Text("Evaluations %llu, last result 0x%08x", (unsigned long long)d.evaluations, d.last_eval_result);
    }
    {
        // NGX render presets worth offering on DLSS 310 (A-D removed, E-I/N/O deprecated or reserved).
        static const struct { int value; const char* name; } kPresets[] = {
            {0, "Default (DLSS picks per mode)"}, {10, "J (transformer, less ghosting, more flicker)"},
            {11, "K (transformer, best quality)"}, {12, "L (Ultra Performance default)"},
            {13, "M (Performance default)"}};
        Config& c = config();
        const char* current = "custom";
        for (const auto& pr : kPresets)
            if (pr.value == c.dlss_preset)
                current = pr.name;
        const render_scale::Status& rs = render_scale::status();
        ImGui::SetNextItemWidth(300);
        int qm = c.dlss_quality_mode < render_scale::kModeCount ? c.dlss_quality_mode : 0;
        if (ImGui::BeginCombo("Quality mode", render_scale::kModes[qm].name)) {
            for (int i = 0; i < render_scale::kModeCount; ++i) {
                if (ImGui::Selectable(render_scale::kModes[i].name, i == qm) && i != qm) {
                    c.dlss_quality_mode = i;
                    config_save();
                }
            }
            ImGui::EndCombo();
        }
        if (!rs.found)
            ImGui::TextColored(ImVec4(1, 0.4f, 0.3f, 1), "Resolution scale setting not found (unknown build?)");
        else if (rs.forcing)
            ImGui::Text("Engine ResolutionScale %.3f (forced; the game's own %.3f comes back in Engine TAA mode)",
                        rs.current, rs.game_value);
        else
            ImGui::Text("Engine ResolutionScale %.3f (game setting)", rs.current);
        ImGui::SetNextItemWidth(300);
        if (ImGui::BeginCombo("DLSS preset", current)) {
            for (const auto& pr : kPresets) {
                if (ImGui::Selectable(pr.name, pr.value == c.dlss_preset) && pr.value != c.dlss_preset) {
                    c.dlss_preset = pr.value;
                    config_save();
                    dlss::request_recreate();
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SetNextItemWidth(300);
        ImGui::SliderInt("Sharpening (RCAS)", &c.dlss_sharpness, 0, 100, c.dlss_sharpness ? "%d%%" : "off");
        if (ImGui::IsItemDeactivatedAfterEdit())
            config_save();
        if (c.dlss_sharpness && p.ready && !p.sharpened) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1, 0.4f, 0.3f, 1), "unavailable (see log)");
        }
    }
    if (taa::mode() == taa::Mode::Dlss) {
        if (p.error)
            ImGui::TextColored(ImVec4(1, 0.4f, 0.3f, 1), "Using engine TAA: %s", p.error);
        else if (p.ready)
            ImGui::Text("Active. Jitter to DLSS (%+.3f, %+.3f) px%s", p.jitter_x, p.jitter_y,
                        p.reset_last ? ", reset" : "");
        if (p.ready && p.output_width) {
            ImGui::Text("Render %ux%u -> output %ux%u (%.0f%%)", p.render_width, p.render_height, p.output_width,
                        p.output_height, 100.0 * p.render_width / p.output_width);
        }
    }

    {
        post::Settings& ps = post::settings();
        const post::Status& pst = post::status();
        ImGui::SeparatorText("Post chain (below 100% scale)");
        ImGui::Checkbox("Post at output size (off: DLSS output downsampled to render size)", &ps.enabled);
        ImGui::Checkbox("Motion blur at output size (off: render size, upsampled by tonemap)", &ps.motion_blur);
        if (ps.enabled && p.ready && p.downsampled) {
            auto pass = [](const char* name, bool seen, bool done) {
                ImGui::TextColored(done ? ImVec4(0.4f, 1, 0.4f, 1) : ImVec4(1, 0.8f, 0.3f, 1), "%s: %s", name,
                                   !seen ? "not run" : done ? "output size" : "render size");
            };
            pass("Motion blur", pst.motion_blur_seen, pst.motion_blur_done);
            ImGui::SameLine();
            pass("Tonemap", true, pst.tonemap_done);
            ImGui::SameLine();
            pass("LDR2", true, pst.ldr2_done);
            ImGui::SameLine();
            pass("DoF", true, pst.dof_done);
            ImGui::SameLine();
            pass("Resample replaced", true, pst.resample_done);
            ImGui::Text("Motion blur shader port: %s, LDR refills %u", pst.mb_shader, pst.ldr_refills);
            if (pst.other_ldr_writes)
                ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "%u unknown draws wrote the LDR target (lost)",
                                   pst.other_ldr_writes);
            if (pst.error)
                ImGui::TextColored(ImVec4(1, 0.4f, 0.3f, 1), "Fallback: %s", pst.error);
            if (pst.device_lost_after)
                ImGui::TextColored(ImVec4(1, 0.4f, 0.3f, 1), "GPU device lost after %s", pst.device_lost_after);
        }
    }

    dlss_pass::Settings& s = dlss_pass::settings();
    ImGui::Checkbox("Preview DLSS output on screen (debug: no post, no UI, simple tonemap)", &s.preview);
    ImGui::Checkbox("Resolve Runner's Vision mask (TAA port)", &s.rv_resolve);
    if (s.rv_resolve && p.ready) {
        ImGui::SameLine();
        ImGui::TextColored(p.rv_resolved ? ImVec4(0.4f, 1, 0.4f, 1) : ImVec4(1, 0.8f, 0.3f, 1), "%s",
                           p.rv_resolved ? "active" : "passthrough");
    }

    mip_bias::Settings& m = mip_bias::settings();
    const mip_bias::Status& ms = mip_bias::status();
    ImGui::SeparatorText("Mip bias");
    ImGui::Checkbox("Enabled##mip", &m.enabled);
    ImGui::SameLine();
    ImGui::Checkbox("Trilinear samplers too", &m.trilinear_too);
    ImGui::SetNextItemWidth(160);
    ImGui::SliderFloat("offset (added to log2 render/output)", &m.offset, -2.0f, 1.0f, "%.2f");
    if (!ms.hooked)
        ImGui::TextColored(ImVec4(1, 0.4f, 0.3f, 1), "PSSetSamplers not hooked");
    else
        ImGui::Text("Bias %.2f, %u biased samplers, %u swaps last frame", ms.bias, ms.samplers, ms.swaps_last_frame);
    ImGui::SeparatorText("Inputs (debug)");
    ImGui::TextUnformatted("Jitter = (x, y) from the engine table, times:");
    sign_toggle("negate X##jitter", &s.jitter_sign_x);
    ImGui::SameLine();
    sign_toggle("negate Y##jitter", &s.jitter_sign_y);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90);
    ImGui::InputInt("frame offset", &s.jitter_frame_offset);
    ImGui::TextUnformatted("Motion vectors = velocity * (0.5 W, 0.5 H), times:");
    sign_toggle("negate X##mv", &s.mv_sign_x);
    ImGui::SameLine();
    sign_toggle("negate Y##mv", &s.mv_sign_y);
    if (ImGui::Checkbox("DLSS auto exposure", &dlss::settings().auto_exposure))
        dlss::request_recreate();
    if (ImGui::Button("Reset history"))
        dlss_pass::request_reset();
    ImGui::SameLine();
    if (ImGui::Button("Defaults")) {
        s = dlss_pass::Settings{};
        m = mip_bias::Settings{};
        dlss_pass::request_reset();
    }
}

static void draw_main_window(IDXGISwapChain* swapchain) {
    ImGui::SetNextWindowPos(ImVec2(40, 40), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(560, 0), ImGuiCond_FirstUseEver);
    ImGui::Begin("Catalyst Clarity " CS_VERSION);

    const State& s = state();
    if (ImGui::CollapsingHeader("Status", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Text("Game base 0x%llx", (unsigned long long)s.game_base);
        if (s.tested_build)
            ImGui::Text("Build: tested (PE timestamp %u)", s.game_timestamp);
        else
            ImGui::TextColored(ImVec4(1, 0.7f, 0.3f, 1), "Build: untested (PE timestamp %u), %s", s.game_timestamp,
                               s.code_matched ? "game code matches" : "game code does not match yet");
        DXGI_SWAP_CHAIN_DESC desc;
        if (SUCCEEDED(swapchain->GetDesc(&desc)))
            ImGui::Text("Back buffer %ux%u, format %d, %u buffers", desc.BufferDesc.Width, desc.BufferDesc.Height,
                        (int)desc.BufferDesc.Format, desc.BufferCount);
        ImGui::Text("Frame %llu, %.1f fps (%.2f ms)", (unsigned long long)g_frames,
                    g_frame_ms > 0 ? 1000.0f / g_frame_ms : 0.0f, g_frame_ms);
    }

    if (ImGui::CollapsingHeader("TAA", ImGuiTreeNodeFlags_DefaultOpen))
        draw_taa_section();
    if (ImGui::CollapsingHeader("DLSS", ImGuiTreeNodeFlags_DefaultOpen))
        draw_dlss_section();

    if (ImGui::CollapsingHeader("Settings", ImGuiTreeNodeFlags_DefaultOpen)) {
        Config& c = config();
        bool changed = false;
        changed |= ImGui::Checkbox("FPS readout when closed", &c.show_fps);
        changed |= ImGui::Checkbox("Open overlay on start", &c.show_on_start);
        ImGui::Text("Toggle key: VK 0x%02X (edit toggle_key in catalyst_clarity.ini)", c.toggle_key);
        if (changed)
            config_save();
    }
    if (ImGui::CollapsingHeader("About")) {
        ImGui::TextUnformatted("Catalyst Clarity " CS_VERSION ". Upscaling by NVIDIA DLSS.");
        ImGui::TextUnformatted("Uses Dear ImGui and MinHook; sharpening is AMD FidelityFX RCAS.");
        ImGui::TextDisabled("Not affiliated with or endorsed by EA, DICE or NVIDIA.");
    }
    ImGui::End();
}

void on_present(IDXGISwapChain* swapchain) {
    if (g_failed)
        return;
    if (!g_initialized) {
        g_initialized = init(swapchain);
        g_failed = !g_initialized;
        if (g_failed)
            return;
    }
    if (swapchain != g_swapchain) {
        DXGI_SWAP_CHAIN_DESC desc;
        if (FAILED(swapchain->GetDesc(&desc)) || desc.OutputWindow != g_window)
            return;  // some other swapchain (not the game window)
        logf("overlay: game window has a new swapchain %p", swapchain);
        on_resize_begin();
        g_swapchain = swapchain;
    }

    update_timing();
    poll_toggle_key();
    bool visible = g_visible;
    if (!visible && !config().show_fps)
        return;

    drain_messages();
    std::lock_guard lock(g_render_mutex);
    if (!g_rtv) {
        ID3D11Texture2D* back_buffer = nullptr;
        if (FAILED(swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back_buffer))))
            return;
        HRESULT hr = g_device->CreateRenderTargetView(back_buffer, nullptr, &g_rtv);
        back_buffer->Release();
        if (FAILED(hr)) {
            logf("overlay: CreateRenderTargetView failed 0x%08lx", hr);
            return;
        }
    }

    ImGui::GetIO().MouseDrawCursor = visible;
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    if (visible)
        draw_main_window(swapchain);
    else
        draw_fps_corner();
    ImGui::Render();

    // The DX11 backend restores pipeline state itself, except the render targets we bind.
    ID3D11RenderTargetView* saved_rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11DepthStencilView* saved_dsv = nullptr;
    g_context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, saved_rtvs, &saved_dsv);
    g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    g_context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, saved_rtvs, saved_dsv);
    for (auto* rtv : saved_rtvs)
        if (rtv)
            rtv->Release();
    if (saved_dsv)
        saved_dsv->Release();
}

void on_resize_begin() {
    std::lock_guard lock(g_render_mutex);
    if (g_rtv) {
        g_rtv->Release();
        g_rtv = nullptr;
    }
}

}  // namespace cs::overlay
