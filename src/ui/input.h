#pragma once
#include <windows.h>

#include <cstdint>

// Frees the mouse for the overlay during gameplay. In gameplay the game keeps the cursor clipped and
// re-centred; in the pause menu it lets go, which is why the overlay used to work only there.
// While captured, the game's SetCursorPos / ClipCursor calls are held back (the last clip is restored on
// release) and its GetCursorPos sees a frozen point, so the camera does not follow the overlay cursor.
namespace cs::input {

struct Status {
    bool hooked = false;
    bool captured = false;
    // Counted while captured, logged on release: what the game tried while the overlay was open.
    uint32_t game_set_cursor = 0, game_clip_cursor = 0, game_get_cursor = 0;
};

const Status& status();

// Once, from the render thread: hooks SetCursorPos, ClipCursor and GetCursorPos in user32.
void install();
// Render thread, every frame: on while an overlay window is open.
void set_captured(bool on);

// The overlay's own cursor calls (ImGui's Win32 backend) go to the real functions while this is alive.
struct OwnCalls {
    OwnCalls();
    ~OwnCalls();
};

// The real cursor position, bypassing the frozen point.
bool real_cursor_pos(POINT* p);

}  // namespace cs::input
