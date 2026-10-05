#include "renderer/gpu.h"

#include <windows.h>

#include <d3dcompiler.h>

#include <cstring>

#include "core/log.h"

namespace cs::gpu {

constexpr const char* kBlitVs = R"(
float4 main(uint id : SV_VertexID) : SV_Position {
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
)";
constexpr const char* kBlitPs = R"(
Texture2D<float4> src : register(t0);
float4 main(float4 pos : SV_Position) : SV_Target { return src.Load(int3(pos.xy, 0)); }
)";
// Same triangle with UVs, for the filtered blits (destination size != source size).
constexpr const char* kUvVs = R"(
void main(uint id : SV_VertexID, out float4 pos : SV_Position, out float2 uv : TEXCOORD0) {
    uv = float2((id << 1) & 2, id & 2);
    pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
)";
constexpr const char* kScalePs = R"(
Texture2D<float4> src : register(t0);
SamplerState linear_clamp : register(s0);
float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target { return src.SampleLevel(linear_clamp, uv, 0); }
)";
// Debug view of linear HDR on an LDR backbuffer: Reinhard + gamma 2.2, not the game's tonemap.
constexpr const char* kPreviewPs = R"(
Texture2D<float4> src : register(t0);
SamplerState linear_clamp : register(s0);
float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    float3 c = max(src.SampleLevel(linear_clamp, uv, 0).rgb, 0);
    return float4(pow(c / (1 + c), 1 / 2.2), 1);
}
)";
constexpr const char* kDepthCs = R"(
Texture2D<float> src : register(t0);
RWTexture2D<float> dst : register(u0);
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) { dst[id.xy] = src[id.xy]; }
)";

static ID3D11Device* g_device;  // identity only
static bool g_ready;
static ID3DDeviceContextState* g_clean_state;
static ID3D11VertexShader* g_blit_vs;
static ID3D11PixelShader* g_blit_ps;
static ID3D11ComputeShader* g_depth_cs;
static ID3D11VertexShader* g_uv_vs;
static ID3D11PixelShader* g_scale_ps;
static ID3D11PixelShader* g_preview_ps;
static ID3D11SamplerState* g_linear_clamp;
static bool g_cb_partial;          // D3D11_FEATURE_D3D11_OPTIONS.ConstantBufferPartialUpdate

static ID3DBlob* compile(const char* source, const char* name, const char* target) {
    ID3DBlob* code = nullptr;
    ID3DBlob* errors = nullptr;
    HRESULT hr = D3DCompile(source, strlen(source), name, nullptr, nullptr, "main", target,
                            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    if (FAILED(hr))
        logf("gpu: %s: %s", name, errors ? (const char*)errors->GetBufferPointer() : "compile failed");
    if (errors)
        errors->Release();
    return SUCCEEDED(hr) ? code : nullptr;
}

bool init(ID3D11Device* device) {
    if (device == g_device)
        return g_ready;
    g_device = device;
    g_ready = false;

    ID3D11Device1* device1 = nullptr;
    if (FAILED(device->QueryInterface(__uuidof(ID3D11Device1), reinterpret_cast<void**>(&device1)))) {
        logf("gpu: no ID3D11Device1 (needed to save/restore context state)");
        return false;
    }
    D3D_FEATURE_LEVEL level = device->GetFeatureLevel();
    HRESULT hr = device1->CreateDeviceContextState(0, &level, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device), nullptr,
                                                   &g_clean_state);
    device1->Release();
    if (FAILED(hr)) {
        logf("gpu: CreateDeviceContextState failed 0x%08lx", hr);
        return false;
    }

    ID3DBlob* vs = compile(kBlitVs, "blit_vs", "vs_5_0");
    ID3DBlob* ps = compile(kBlitPs, "blit_ps", "ps_5_0");
    ID3DBlob* cs = compile(kDepthCs, "depth_cs", "cs_5_0");
    ID3DBlob* uv_vs = compile(kUvVs, "uv_vs", "vs_5_0");
    ID3DBlob* scale_ps = compile(kScalePs, "scale_ps", "ps_5_0");
    ID3DBlob* preview_ps = compile(kPreviewPs, "preview_ps", "ps_5_0");
    auto make_ps = [&](ID3DBlob* b, ID3D11PixelShader** out) {
        return SUCCEEDED(device->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, out));
    };
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    bool ok = vs && ps && cs && uv_vs && scale_ps && preview_ps &&
              SUCCEEDED(device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &g_blit_vs)) &&
              make_ps(ps, &g_blit_ps) &&
              SUCCEEDED(device->CreateComputeShader(cs->GetBufferPointer(), cs->GetBufferSize(), nullptr, &g_depth_cs)) &&
              SUCCEEDED(device->CreateVertexShader(uv_vs->GetBufferPointer(), uv_vs->GetBufferSize(), nullptr, &g_uv_vs)) &&
              make_ps(scale_ps, &g_scale_ps) && make_ps(preview_ps, &g_preview_ps) &&
              SUCCEEDED(device->CreateSamplerState(&sd, &g_linear_clamp));
    for (ID3DBlob* b : {vs, ps, cs, uv_vs, scale_ps, preview_ps})
        if (b)
            b->Release();
    if (!ok)
        logf("gpu: shader creation failed");
    D3D11_FEATURE_DATA_D3D11_OPTIONS options{};
    if (SUCCEEDED(device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, &options, sizeof(options))))
        g_cb_partial = options.ConstantBufferPartialUpdate != 0;
    logf("gpu: constant buffer partial update %s", g_cb_partial ? "supported" : "NOT supported");
    g_ready = ok;
    return ok;
}

Scope::Scope(ID3D11DeviceContext* ctx) {
    if (!g_ready ||
        FAILED(ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&ctx1_)))) {
        ctx1_ = nullptr;
        return;
    }
    ctx1_->SwapDeviceContextState(g_clean_state, &engine_state_);
}

Scope::~Scope() {
    if (!ctx1_)
        return;
    // Leave the clean state clean for next time, then restore the engine's.
    ctx1_->ClearState();
    ctx1_->SwapDeviceContextState(engine_state_, nullptr);
    if (engine_state_)
        engine_state_->Release();
    ctx1_->Release();
}

void blit(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* src, ID3D11RenderTargetView* dst, UINT width,
          UINT height) {
    D3D11_VIEWPORT vp{0, 0, float(width), float(height), 0, 1};
    ctx->OMSetRenderTargets(1, &dst, nullptr);
    ctx->RSSetViewports(1, &vp);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->IASetInputLayout(nullptr);
    ctx->VSSetShader(g_blit_vs, nullptr, 0);
    ctx->PSSetShader(g_blit_ps, nullptr, 0);
    ctx->PSSetShaderResources(0, 1, &src);
    ctx->Draw(3, 0);
    ID3D11ShaderResourceView* null_srv = nullptr;
    ID3D11RenderTargetView* null_rtv = nullptr;
    ctx->PSSetShaderResources(0, 1, &null_srv);
    ctx->OMSetRenderTargets(1, &null_rtv, nullptr);
}

void apply(ID3D11DeviceContext* ctx, ID3D11PixelShader* ps, ID3D11ShaderResourceView* src, ID3D11Buffer* constants,
           ID3D11RenderTargetView* dst, UINT width, UINT height) {
    D3D11_VIEWPORT vp{0, 0, float(width), float(height), 0, 1};
    ctx->OMSetRenderTargets(1, &dst, nullptr);
    ctx->RSSetViewports(1, &vp);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->IASetInputLayout(nullptr);
    ctx->VSSetShader(g_blit_vs, nullptr, 0);
    ctx->PSSetShader(ps, nullptr, 0);
    ctx->PSSetShaderResources(0, 1, &src);
    ctx->PSSetConstantBuffers(0, 1, &constants);
    ctx->Draw(3, 0);
    ID3D11ShaderResourceView* null_srv = nullptr;
    ID3D11RenderTargetView* null_rtv = nullptr;
    ID3D11Buffer* null_cb = nullptr;
    ctx->PSSetShaderResources(0, 1, &null_srv);
    ctx->PSSetConstantBuffers(0, 1, &null_cb);
    ctx->OMSetRenderTargets(1, &null_rtv, nullptr);
}

static void filtered(ID3D11DeviceContext* ctx, ID3D11PixelShader* ps, ID3D11ShaderResourceView* src,
                     ID3D11RenderTargetView* dst, UINT width, UINT height) {
    D3D11_VIEWPORT vp{0, 0, float(width), float(height), 0, 1};
    ctx->OMSetRenderTargets(1, &dst, nullptr);
    ctx->RSSetViewports(1, &vp);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->IASetInputLayout(nullptr);
    ctx->VSSetShader(g_uv_vs, nullptr, 0);
    ctx->PSSetShader(ps, nullptr, 0);
    ctx->PSSetShaderResources(0, 1, &src);
    ctx->PSSetSamplers(0, 1, &g_linear_clamp);
    ctx->Draw(3, 0);
    ID3D11ShaderResourceView* null_srv = nullptr;
    ID3D11RenderTargetView* null_rtv = nullptr;
    ctx->PSSetShaderResources(0, 1, &null_srv);
    ctx->OMSetRenderTargets(1, &null_rtv, nullptr);
}

void blit_scaled(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* src, ID3D11RenderTargetView* dst, UINT width,
                 UINT height) {
    filtered(ctx, g_scale_ps, src, dst, width, height);
}

void preview(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* src, ID3D11RenderTargetView* dst, UINT width,
             UINT height) {
    filtered(ctx, g_preview_ps, src, dst, width, height);
}

ID3D11PixelShader* create_pixel_shader(ID3D11Device* device, const char* source, const char* name) {
    ID3DBlob* code = compile(source, name, "ps_5_0");
    ID3D11PixelShader* ps = nullptr;
    if (code) {
        if (FAILED(device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &ps)))
            logf("gpu: CreatePixelShader %s failed", name);
        code->Release();
    }
    return ps;
}

void copy_depth(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* src, ID3D11UnorderedAccessView* dst, UINT width,
                UINT height) {
    ctx->CSSetShader(g_depth_cs, nullptr, 0);
    ctx->CSSetShaderResources(0, 1, &src);
    ctx->CSSetUnorderedAccessViews(0, 1, &dst, nullptr);
    ctx->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
    ID3D11ShaderResourceView* null_srv = nullptr;
    ID3D11UnorderedAccessView* null_uav = nullptr;
    ctx->CSSetShaderResources(0, 1, &null_srv);
    ctx->CSSetUnorderedAccessViews(0, 1, &null_uav, nullptr);
}

ID3D11Resource* resource_of(ID3D11View* view) {
    if (!view)
        return nullptr;
    ID3D11Resource* res = nullptr;
    view->GetResource(&res);
    if (res)
        res->Release();  // the view keeps it alive
    return res;
}

void view_size(ID3D11View* view, UINT* width, UINT* height) {
    *width = *height = 0;
    if (!view)
        return;
    ID3D11Resource* res = nullptr;
    view->GetResource(&res);
    ID3D11Texture2D* tex = nullptr;
    if (res) {
        res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex));
        res->Release();
    }
    if (tex) {
        D3D11_TEXTURE2D_DESC d;
        tex->GetDesc(&d);
        *width = d.Width;
        *height = d.Height;
        tex->Release();
    }
}

}  // namespace cs::gpu
