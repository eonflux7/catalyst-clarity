#include "ui/input.h"

#include <MinHook.h>

#include <atomic>
#include <mutex>

#include "core/log.h"

namespace cs::input {

using SetCursorPosFn = BOOL(WINAPI*)(int, int);
using ClipCursorFn = BOOL(WINAPI*)(const RECT*);
using GetCursorPosFn = BOOL(WINAPI*)(LPPOINT);

static SetCursorPosFn o_set_cursor_pos;
static ClipCursorFn o_clip_cursor;
static GetCursorPosFn o_get_cursor_pos;

static Status g_status;
static std::atomic<bool> g_captured;
static thread_local int t_own;

static std::mutex g_mutex;  // guards the fields below; never held across a call into user32
static POINT g_frozen;          // what the game's GetCursorPos sees while captured
static bool g_game_clipped;     // the game's last ClipCursor while captured (restored on release)
static RECT g_game_clip;

const Status& status() { return g_status; }

OwnCalls::OwnCalls() { ++t_own; }
OwnCalls::~OwnCalls() { --t_own; }

bool real_cursor_pos(POINT* p) { return (o_get_cursor_pos ? o_get_cursor_pos : &::GetCursorPos)(p) != FALSE; }

static BOOL WINAPI hk_set_cursor_pos(int x, int y) {
    if (!g_captured || t_own)
        return o_set_cursor_pos(x, y);
    std::lock_guard lock(g_mutex);
    g_frozen = {x, y};  // the game reads back what it set: no motion
    ++g_status.game_set_cursor;
    return TRUE;
}

static BOOL WINAPI hk_clip_cursor(const RECT* rect) {
    if (!g_captured || t_own)
        return o_clip_cursor(rect);
    std::lock_guard lock(g_mutex);
    g_game_clipped = rect != nullptr;
    if (rect)
        g_game_clip = *rect;
    ++g_status.game_clip_cursor;
    return TRUE;
}

static BOOL WINAPI hk_get_cursor_pos(LPPOINT p) {
    if (!g_captured || t_own || !p)
        return o_get_cursor_pos(p);
    std::lock_guard lock(g_mutex);
    *p = g_frozen;
    ++g_status.game_get_cursor;
    return TRUE;
}

template <typename T>
static bool hook(const char* name, void* detour, T* original) {
    void* target = nullptr;
    MH_STATUS created = MH_CreateHookApiEx(L"user32", name, detour, reinterpret_cast<void**>(original), &target);
    MH_STATUS enabled = created == MH_OK ? MH_EnableHook(target) : created;
    if (enabled != MH_OK)
        logf("input: %s hook failed: %s", name, MH_StatusToString(enabled));
    return enabled == MH_OK;
}

void install() {
    if (g_status.hooked)
        return;
    bool ok = hook("SetCursorPos", reinterpret_cast<void*>(&hk_set_cursor_pos), &o_set_cursor_pos) &&
              hook("ClipCursor", reinterpret_cast<void*>(&hk_clip_cursor), &o_clip_cursor) &&
              hook("GetCursorPos", reinterpret_cast<void*>(&hk_get_cursor_pos), &o_get_cursor_pos);
    g_status.hooked = ok;
    logf("input: cursor hooks %s", ok ? "installed" : "NOT installed");
}

void set_captured(bool on) {
    if (!g_status.hooked || on == g_captured)
        return;
    if (on) {
        RECT clip{};
        bool clipped = false;
        if (GetClipCursor(&clip)) {
            RECT screen{GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN), 0, 0};
            screen.right = screen.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
            screen.bottom = screen.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
            clipped = !EqualRect(&clip, &screen);
        }
        POINT pos{};
        o_get_cursor_pos(&pos);
        {
            std::lock_guard lock(g_mutex);
            g_frozen = pos;
            g_game_clipped = clipped;
            g_game_clip = clip;
            g_status.game_set_cursor = g_status.game_clip_cursor = g_status.game_get_cursor = 0;
        }
        g_captured = true;
        g_status.captured = true;
        o_clip_cursor(nullptr);
        return;
    }
    g_captured = false;
    g_status.captured = false;
    bool clipped, recentred;
    RECT clip;
    POINT frozen;
    {
        std::lock_guard lock(g_mutex);
        clipped = g_game_clipped;
        clip = g_game_clip;
        frozen = g_frozen;
        recentred = g_status.game_set_cursor > 0;
    }
    // Hand the cursor back clipped like before, and where the game put it if it was re-centring it
    // (gameplay); in menus it stays where the user left it.
    if (recentred)
        o_set_cursor_pos(frozen.x, frozen.y);
    o_clip_cursor(clipped ? &clip : nullptr);
    logf("input: overlay closed; while open the game called SetCursorPos %u, ClipCursor %u, GetCursorPos %u times",
         g_status.game_set_cursor, g_status.game_clip_cursor, g_status.game_get_cursor);
}

}  // namespace cs::input
