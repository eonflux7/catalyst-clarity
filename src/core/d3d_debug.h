#pragma once
#include <d3d11.h>

// Opt-in D3D11 debug layer ([debug] d3d_debug=1 in catalyst_clarity.ini): the game's device is created
// with D3D11_CREATE_DEVICE_DEBUG (needs the Graphics Tools optional feature) and the layer's messages
// go to the mod log, tagged with where they were drained. Slow; for diagnosing invalid API use.
namespace cs::d3d_debug {

bool enabled();
// Init thread, after d3d11.dll is loaded and before the game creates its device.
void install();
// Render thread: logs and clears queued messages (first few per message id). `where` names the
// mod step that just ran, or "frame" at Present.
void drain(ID3D11DeviceContext* ctx, const char* where);

}  // namespace cs::d3d_debug
