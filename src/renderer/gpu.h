#pragma once
#include <d3d11_1.h>

// Small GPU helpers for work we inject into the engine's frame. Render thread only.
//
// Everything runs inside a Scope: the engine's whole pipeline state is swapped out for a clean one
// and swapped back afterwards, because Frostbite tracks its own bindings and must not see ours.
// Transfers are shader blits rather than CopyResource, which needs exactly matching resources
// (engine targets differ from ours in typeless/typed format and bind flags).
namespace cs::gpu {

// Creates the clean state and shaders for this device (once). False if unsupported.
bool init(ID3D11Device* device);

class Scope {
public:
    explicit Scope(ID3D11DeviceContext* ctx);
    ~Scope();
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
    bool ok() const { return ctx1_ != nullptr; }

private:
    ID3D11DeviceContext1* ctx1_ = nullptr;
    ID3DDeviceContextState* engine_state_ = nullptr;
};

// dst = src texel for texel over width x height (inside a Scope). Any colour formats; missing
// channels are dropped/zero.
void blit(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* src, ID3D11RenderTargetView* dst, UINT width,
          UINT height);
// dst (width x height) = src stretched over it, bilinear (inside a Scope).
void blit_scaled(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* src, ID3D11RenderTargetView* dst, UINT width,
                 UINT height);
// Debug: linear HDR src stretched onto an LDR dst with a simple Reinhard + gamma (inside a Scope).
void preview(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* src, ID3D11RenderTargetView* dst, UINT width,
             UINT height);
// R32 depth from a depth SRV (e.g. R32_FLOAT_X8X24 view of D32S8) into an R32_FLOAT UAV.
void copy_depth(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* src, ID3D11UnorderedAccessView* dst, UINT width,
                UINT height);

// dst = ps(src) texel for texel over width x height, ps constants in b0 (inside a Scope).
void apply(ID3D11DeviceContext* ctx, ID3D11PixelShader* ps, ID3D11ShaderResourceView* src, ID3D11Buffer* constants,
           ID3D11RenderTargetView* dst, UINT width, UINT height);

// Compiles and creates a pixel shader (ps_5_0); nullptr on failure (logged).
ID3D11PixelShader* create_pixel_shader(ID3D11Device* device, const char* source, const char* name);

// The resource behind a view (no reference kept; identity only).
ID3D11Resource* resource_of(ID3D11View* view);

// Size of the texture behind a view (0x0 if it is not a 2D texture).
void view_size(ID3D11View* view, UINT* width, UINT* height);

}  // namespace cs::gpu
