#pragma once

namespace cs {
// Hooks IDXGISwapChain::Present / ResizeBuffers (dxgi.dll code, so every swapchain goes through
// them). Needs d3d11.dll loaded. Returns false if the dummy device or MinHook fails.
bool install_present_hooks();
}
