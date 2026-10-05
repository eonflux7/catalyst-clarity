#include "core/d3d_debug.h"

#include <windows.h>

#include <d3d11sdklayers.h>

#include <MinHook.h>

#include <unordered_map>
#include <vector>

#include "core/config.h"
#include "core/log.h"

namespace cs::d3d_debug {

constexpr int kMaxPerId = 5;

using CreateDeviceFn = decltype(&D3D11CreateDevice);
using CreateDeviceAndSwapChainFn = decltype(&D3D11CreateDeviceAndSwapChain);

static CreateDeviceFn o_create_device;
static CreateDeviceAndSwapChainFn o_create_device_and_swapchain;
static ID3D11InfoQueue* g_queue;
static ID3D11Device* g_queue_device;  // identity
static bool g_queue_tried;
static std::unordered_map<int, int> g_seen;  // message id -> times logged

bool enabled() { return config().d3d_debug; }

static HRESULT WINAPI hk_create_device(IDXGIAdapter* adapter, D3D_DRIVER_TYPE type, HMODULE software, UINT flags,
                                       const D3D_FEATURE_LEVEL* levels, UINT level_count, UINT sdk,
                                       ID3D11Device** device, D3D_FEATURE_LEVEL* level, ID3D11DeviceContext** ctx) {
    HRESULT hr = o_create_device(adapter, type, software, flags | D3D11_CREATE_DEVICE_DEBUG, levels, level_count, sdk,
                                 device, level, ctx);
    logf("d3d_debug: D3D11CreateDevice flags 0x%x + DEBUG -> 0x%08lx", flags, hr);
    if (FAILED(hr))  // layer not installed: give the game what it asked for
        hr = o_create_device(adapter, type, software, flags, levels, level_count, sdk, device, level, ctx);
    return hr;
}

static HRESULT WINAPI hk_create_device_and_swapchain(IDXGIAdapter* adapter, D3D_DRIVER_TYPE type, HMODULE software,
                                                     UINT flags, const D3D_FEATURE_LEVEL* levels, UINT level_count,
                                                     UINT sdk, const DXGI_SWAP_CHAIN_DESC* desc,
                                                     IDXGISwapChain** swapchain, ID3D11Device** device,
                                                     D3D_FEATURE_LEVEL* level, ID3D11DeviceContext** ctx) {
    HRESULT hr = o_create_device_and_swapchain(adapter, type, software, flags | D3D11_CREATE_DEVICE_DEBUG, levels,
                                               level_count, sdk, desc, swapchain, device, level, ctx);
    logf("d3d_debug: D3D11CreateDeviceAndSwapChain flags 0x%x + DEBUG -> 0x%08lx", flags, hr);
    if (FAILED(hr))
        hr = o_create_device_and_swapchain(adapter, type, software, flags, levels, level_count, sdk, desc, swapchain,
                                           device, level, ctx);
    return hr;
}

void install() {
    if (!enabled())
        return;
    HMODULE d3d11 = GetModuleHandleW(L"d3d11.dll");
    void* create = reinterpret_cast<void*>(GetProcAddress(d3d11, "D3D11CreateDevice"));
    void* create_sc = reinterpret_cast<void*>(GetProcAddress(d3d11, "D3D11CreateDeviceAndSwapChain"));
    MH_STATUS a = MH_CreateHook(create, reinterpret_cast<void*>(&hk_create_device),
                                reinterpret_cast<void**>(&o_create_device));
    MH_STATUS b = MH_CreateHook(create_sc, reinterpret_cast<void*>(&hk_create_device_and_swapchain),
                                reinterpret_cast<void**>(&o_create_device_and_swapchain));
    if (a == MH_OK)
        MH_EnableHook(create);
    if (b == MH_OK)
        MH_EnableHook(create_sc);
    logf("d3d_debug: enabled; device creation hooks %s / %s", MH_StatusToString(a), MH_StatusToString(b));
}

static ID3D11InfoQueue* queue_for(ID3D11DeviceContext* ctx) {
    ID3D11Device* device = nullptr;
    ctx->GetDevice(&device);
    if (device != g_queue_device) {
        g_queue_device = device;
        g_queue_tried = false;
        if (g_queue)
            g_queue->Release();
        g_queue = nullptr;
    }
    if (!g_queue_tried) {
        g_queue_tried = true;
        if (SUCCEEDED(device->QueryInterface(__uuidof(ID3D11InfoQueue), reinterpret_cast<void**>(&g_queue)))) {
            g_queue->SetMessageCountLimit(4096);
            D3D11_MESSAGE_SEVERITY deny[] = {D3D11_MESSAGE_SEVERITY_INFO, D3D11_MESSAGE_SEVERITY_MESSAGE};
            D3D11_INFO_QUEUE_FILTER filter{};
            filter.DenyList.NumSeverities = static_cast<UINT>(std::size(deny));
            filter.DenyList.pSeverityList = deny;
            g_queue->AddStorageFilterEntries(&filter);
            logf("d3d_debug: info queue attached");
        } else {
            logf("d3d_debug: no ID3D11InfoQueue (device not created with the debug layer)");
        }
    }
    device->Release();
    return g_queue;
}

void drain(ID3D11DeviceContext* ctx, const char* where) {
    if (!enabled())
        return;
    ID3D11InfoQueue* q = queue_for(ctx);
    if (!q)
        return;
    UINT64 n = q->GetNumStoredMessages();
    static std::vector<char> buf;
    for (UINT64 i = 0; i < n; ++i) {
        SIZE_T size = 0;
        if (FAILED(q->GetMessage(i, nullptr, &size)) || size == 0)
            continue;
        buf.resize(size);
        auto* m = reinterpret_cast<D3D11_MESSAGE*>(buf.data());
        if (FAILED(q->GetMessage(i, m, &size)))
            continue;
        int& seen = g_seen[m->ID];
        if (seen++ < kMaxPerId)
            logf("d3d [%s] sev %d id %d: %.*s", where, (int)m->Severity, (int)m->ID, (int)m->DescriptionByteLength,
                 m->pDescription);
    }
    q->ClearStoredMessages();
}

}  // namespace cs::d3d_debug
