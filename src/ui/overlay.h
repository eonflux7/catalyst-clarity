#pragma once
#include <dxgi.h>

namespace cs::overlay {
// Called from the Present hook on the render thread (before the real Present).
void on_present(IDXGISwapChain* swapchain);
// Called before the real ResizeBuffers: drops our reference to the back buffer.
void on_resize_begin();
}
