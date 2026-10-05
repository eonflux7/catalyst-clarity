#include "renderer/mip_bias.h"

#include <windows.h>

#include <MinHook.h>

#include <algorithm>
#include <atomic>
#include <unordered_map>

#include "core/log.h"

namespace cs::mip_bias {

constexpr int kPsSetSamplersSlot = 10;  // ID3D11DeviceContext::PSSetSamplers

using PsSetSamplersFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, ID3D11SamplerState* const*);

struct Entry {
    ID3D11SamplerState* biased = nullptr;  // nullptr: not a material sampler, bind as is
    float bias = 0.0f;                     // what `biased` was made with
    bool trilinear_too = false;            // the setting `eligible` was decided with
};

static PsSetSamplersFn o_ps_set_samplers;
static Settings g_settings;
static Status g_status;
static std::atomic<float> g_bias{0.0f};
static std::atomic<bool> g_scene{true};  // between Present and the TAA entry
static std::atomic<uint32_t> g_swaps;
// Keyed by the engine's sampler, which we AddRef so the pointer cannot be reused for another one.
static std::unordered_map<ID3D11SamplerState*, Entry> g_cache;
static SRWLOCK g_lock = SRWLOCK_INIT;

Settings& settings() { return g_settings; }
const Status& status() { return g_status; }

static bool eligible(const D3D11_SAMPLER_DESC& d, bool trilinear_too) {
    if (D3D11_DECODE_IS_COMPARISON_FILTER(d.Filter) || d.MaxLOD <= 0.0f)
        return false;  // shadow maps, single-mip textures
    if (D3D11_DECODE_IS_ANISOTROPIC_FILTER(d.Filter))
        return true;
    return trilinear_too && D3D11_DECODE_MIP_FILTER(d.Filter) == D3D11_FILTER_TYPE_LINEAR &&
           D3D11_DECODE_MIN_FILTER(d.Filter) == D3D11_FILTER_TYPE_LINEAR;
}

// Caller holds g_lock exclusively.
static ID3D11SamplerState* biased_for(ID3D11SamplerState* sampler, float bias, bool trilinear_too) {
    auto it = g_cache.find(sampler);
    if (it != g_cache.end() && it->second.trilinear_too == trilinear_too &&
        (!it->second.biased || it->second.bias == bias))
        return it->second.biased;
    if (it == g_cache.end()) {
        sampler->AddRef();
        it = g_cache.emplace(sampler, Entry{}).first;
    }
    Entry& e = it->second;
    if (e.biased) {
        e.biased->Release();  // the context keeps its own reference while it is bound
        e.biased = nullptr;
        --g_status.samplers;
    }
    e.bias = bias;
    e.trilinear_too = trilinear_too;

    D3D11_SAMPLER_DESC d;
    sampler->GetDesc(&d);
    if (!eligible(d, trilinear_too))
        return nullptr;
    d.MipLODBias = std::clamp(d.MipLODBias + bias, -16.0f, 15.99f);
    ID3D11Device* device = nullptr;
    sampler->GetDevice(&device);
    HRESULT hr = device->CreateSamplerState(&d, &e.biased);
    device->Release();
    if (FAILED(hr)) {
        logf("mip_bias: CreateSamplerState failed 0x%08lx (filter 0x%x)", hr, d.Filter);
        e.biased = nullptr;
        return nullptr;
    }
    ++g_status.samplers;
    return e.biased;
}

static void STDMETHODCALLTYPE hk_ps_set_samplers(ID3D11DeviceContext* ctx, UINT start, UINT count,
                                                 ID3D11SamplerState* const* samplers) {
    float bias = g_bias.load(std::memory_order_relaxed);
    if (bias == 0.0f || !g_scene.load(std::memory_order_relaxed) || !samplers || count == 0 || count > D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT) {
        o_ps_set_samplers(ctx, start, count, samplers);
        return;
    }
    ID3D11SamplerState* swapped[D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT];
    bool trilinear_too = g_settings.trilinear_too;
    uint32_t swaps = 0;
    AcquireSRWLockExclusive(&g_lock);
    for (UINT i = 0; i < count; ++i) {
        ID3D11SamplerState* b = samplers[i] ? biased_for(samplers[i], bias, trilinear_too) : nullptr;
        swapped[i] = b ? b : samplers[i];
        swaps += b != nullptr;
    }
    ReleaseSRWLockExclusive(&g_lock);
    g_swaps.fetch_add(swaps, std::memory_order_relaxed);
    o_ps_set_samplers(ctx, start, count, swapped);
}

void install(ID3D11DeviceContext* ctx) {
    if (g_status.hooked)
        return;
    void* target = (*reinterpret_cast<void***>(ctx))[kPsSetSamplersSlot];
    MH_STATUS created =
        MH_CreateHook(target, reinterpret_cast<void*>(&hk_ps_set_samplers), reinterpret_cast<void**>(&o_ps_set_samplers));
    MH_STATUS enabled = created == MH_OK ? MH_EnableHook(target) : created;
    logf("mip_bias: PSSetSamplers hook at %p: %s / %s", target, MH_StatusToString(created),
         MH_StatusToString(enabled));
    g_status.hooked = enabled == MH_OK;
}

void begin_frame(float bias) {
    if (!g_settings.enabled || !g_status.hooked)
        bias = 0.0f;
    if (bias != g_status.bias)
        logf("mip_bias: bias %.3f", bias);
    g_status.bias = bias;
    g_bias.store(bias, std::memory_order_relaxed);
    g_scene.store(true, std::memory_order_relaxed);
    g_status.swaps_last_frame = g_swaps.exchange(0, std::memory_order_relaxed);
}

void end_scene() { g_scene.store(false, std::memory_order_relaxed); }

}  // namespace cs::mip_bias
