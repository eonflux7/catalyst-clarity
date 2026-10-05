#pragma once
#include <d3d11.h>

#include <cstdint>

// Find the engine's TAA resolve draw and expose what it binds.
//
// The TAA function (RVA 0x35d25a0, docs/taa.md) issues the TAAResolvePs fullscreen draw on the
// immediate context (call site +0x35d3357; the High preset adds a second draw, see taa.cpp). So "Draw while inside the TAA function" identifies it without shader hashes.
namespace cs::taa {

struct TexInfo {
    bool bound = false;
    UINT width = 0;
    UINT height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;  // view format
    DXGI_FORMAT texture_format = DXGI_FORMAT_UNKNOWN;
    UINT mips = 0;
    UINT bind_flags = 0;
    UINT misc_flags = 0;
};

constexpr int kSrvSlots = 8;
constexpr int kRtvSlots = 5;

struct Snapshot {
    bool valid = false;
    TexInfo srv[kSrvSlots];
    TexInfo rtv[kRtvSlots];
    TexInfo dsv;
    D3D11_VIEWPORT viewport{};
    UINT vertex_count = 0;
    uintptr_t return_rva = 0;  // caller of Draw, relative to the game base (0 if outside the exe)
    bool skipped = false;      // the engine draw did not run (passthrough or DLSS replaced it)
};

struct Status {
    bool function_hooked = false;
    uint32_t code_wait_ms = 0;
    bool draw_hooked = false;
    bool hook_lost = false;  // our jmp at the TAA entry was overwritten
    const char* vendor_patch = "not tried";  // pixel-shader TAA path forced on every GPU vendor
    uint32_t calls_last_frame = 0;
    uint32_t draws_last_frame = 0;
    // Draws inside the TAA call that are not the TAA resolve (High preset adds a temporal resolve of three
    // R8 targets); passed to the engine untouched.
    uint32_t other_draws_last_frame = 0;
    uint32_t view_frame_index = 0;  // view+0xac
    uint8_t view_taa_enabled = 0;   // view+0xb1
    bool skip_failed = false;       // passthrough could not run (see log)
    Snapshot last;
};

enum class Mode : int {
    Engine = 0,  // the game's TAA
    Skip = 1,    // copy the raw inputs to the outputs (jittered, aliased image; debug)
    Dlss = 2,    // DLSS (DLAA while render size == output size)
};

const Status& status();
Mode& mode();

// Init thread: waits until the TAA entry bytes match the expected code, then hooks.
bool install_function_hook();
// Render thread, every Present: hooks Draw on first call, rolls the per-frame counters.
void on_present(IDXGISwapChain* swapchain);

}  // namespace cs::taa
