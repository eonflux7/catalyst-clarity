#include "hooks/present.h"

#include <windows.h>

#include <d3d11.h>
#include <dxgi.h>

#include <MinHook.h>

#include "core/log.h"
#include "renderer/taa.h"
#include "ui/overlay.h"

namespace cs {

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using ResizeBuffersFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);

// IDXGISwapChain vtable slots (the game calls Present via [vtable+0x40]).
constexpr int kPresentSlot = 8;
constexpr int kResizeBuffersSlot = 13;

static PresentFn o_present;
static ResizeBuffersFn o_resize_buffers;

static HRESULT STDMETHODCALLTYPE hk_present(IDXGISwapChain* swapchain, UINT sync, UINT flags) {
    if (!(flags & DXGI_PRESENT_TEST)) {
        taa::on_present(swapchain);
        overlay::on_present(swapchain);
    }
    HRESULT hr = o_present(swapchain, sync, flags);
    static bool logged;
    if (FAILED(hr) && !logged) {
        logged = true;
        HRESULT reason = S_OK;
        ID3D11Device* device = nullptr;
        if (SUCCEEDED(swapchain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&device)))) {
            reason = device->GetDeviceRemovedReason();
            device->Release();
        }
        logf("Present failed 0x%08lx, device removed reason 0x%08lx", hr, reason);
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE hk_resize_buffers(IDXGISwapChain* swapchain, UINT count, UINT width,
                                                   UINT height, DXGI_FORMAT format, UINT flags) {
    overlay::on_resize_begin();
    HRESULT hr = o_resize_buffers(swapchain, count, width, height, format, flags);
    logf("ResizeBuffers %ux%u fmt %d -> 0x%08lx", width, height, (int)format, hr);
    return hr;
}

// Creates a throwaway device + swapchain on a hidden window just to read the vtable.
static bool find_swapchain_vtable(void** present, void** resize_buffers) {
    auto create = reinterpret_cast<PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN>(
        GetProcAddress(GetModuleHandleW(L"d3d11.dll"), "D3D11CreateDeviceAndSwapChain"));
    if (!create)
        return false;

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"catalyst_clarity_dummy";
    RegisterClassExW(&wc);
    HWND window = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 16, 16, nullptr,
                                  nullptr, wc.hInstance, nullptr);

    DXGI_SWAP_CHAIN_DESC desc{};
    desc.BufferCount = 1;
    desc.BufferDesc.Width = 16;
    desc.BufferDesc.Height = 16;
    desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.OutputWindow = window;
    desc.SampleDesc.Count = 1;
    desc.Windowed = TRUE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    IDXGISwapChain* swapchain = nullptr;
    ID3D11Device* device = nullptr;
    HRESULT hr = E_FAIL;
    for (D3D_DRIVER_TYPE type : {D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP}) {
        hr = create(nullptr, type, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &desc, &swapchain, &device,
                    nullptr, nullptr);
        if (SUCCEEDED(hr))
            break;
    }
    if (SUCCEEDED(hr)) {
        void** vtable = *reinterpret_cast<void***>(swapchain);
        *present = vtable[kPresentSlot];
        *resize_buffers = vtable[kResizeBuffersSlot];
        swapchain->Release();
        device->Release();
    } else {
        logf("dummy D3D11CreateDeviceAndSwapChain failed: 0x%08lx", hr);
    }
    DestroyWindow(window);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return SUCCEEDED(hr);
}

bool install_present_hooks() {
    void* present = nullptr;
    void* resize_buffers = nullptr;
    if (!find_swapchain_vtable(&present, &resize_buffers))
        return false;

    MH_STATUS s1 = MH_CreateHook(present, &hk_present, reinterpret_cast<void**>(&o_present));
    MH_STATUS s2 = MH_CreateHook(resize_buffers, &hk_resize_buffers, reinterpret_cast<void**>(&o_resize_buffers));
    MH_STATUS s3 = MH_EnableHook(MH_ALL_HOOKS);
    logf("hooks: Present %p (%s), ResizeBuffers %p (%s), enable %s", present, MH_StatusToString(s1),
         resize_buffers, MH_StatusToString(s2), MH_StatusToString(s3));
    return s1 == MH_OK && s2 == MH_OK && s3 == MH_OK;
}

}  // namespace cs
