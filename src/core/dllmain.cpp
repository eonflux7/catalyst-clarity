// Entry point. DllMain only sets up the dinput8.dll proxy and starts init_thread; everything else
// runs there, outside the loader lock.
//
// Why dinput8.dll: releases up to 0.2.1 were a version.dll proxy, but the EA app's in-game overlay
// loads System32\version.dll into the game before the game asks for it, so the game folder's copy
// was never loaded. Nothing loads dinput8.dll before the game does.
//
// Timing: the game's code is not in its final form when the process starts. DXGI hooks wait for the
// game to load d3d11.dll; hooks on game code also wait until the target bytes match the expected
// code (renderer/taa.cpp), since d3d11.dll can be loaded before that.
#include <windows.h>

#include <MinHook.h>

#include <string>

#include "core/config.h"
#include "core/crash.h"
#include "core/d3d_debug.h"
#include "core/log.h"
#include "core/proxy.h"
#include "core/state.h"
#include "hooks/present.h"
#include "renderer/taa.h"

namespace cs {

static HMODULE g_self;
static State g_state;

State& state() { return g_state; }

static std::wstring self_dir() {
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(g_self, path, MAX_PATH);
    std::wstring s(path, n);
    return s.substr(0, s.find_last_of(L"\\/") + 1);
}

static bool is_game_process() {
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring s(path, n);
    std::wstring name = s.substr(s.find_last_of(L"\\/") + 1);
    return _wcsicmp(name.c_str(), L"MirrorsEdgeCatalyst.exe") == 0;
}

// True when a version.dll from our own folder is loaded: the proxy of a release up to 0.2.1, left
// behind on update. It runs the mod itself, so this copy must not install a second set of hooks.
static bool old_proxy_loaded(const std::wstring& dir) {
    HMODULE old = GetModuleHandleW(L"version.dll");
    if (!old)
        return false;
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(old, path, MAX_PATH);
    std::wstring s(path, n);
    return _wcsicmp(s.substr(0, s.find_last_of(L"\\/") + 1).c_str(), dir.c_str()) == 0;
}

static void read_game_build() {
    auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    g_state.game_base = base;
    g_state.game_timestamp = nt->FileHeader.TimeDateStamp;
    g_state.tested_build = g_state.game_timestamp == kTestedBuildTimestamp;
}

static DWORD WINAPI init_thread(LPVOID) {
    std::wstring dir = self_dir();
    g_state.mod_dir = dir;
    log_open(dir + L"catalyst_clarity.log");
    logf("catalyst-clarity %s, pid %lu", CS_VERSION, GetCurrentProcessId());
    config_load(dir + L"catalyst_clarity.ini");
    taa::mode() = static_cast<taa::Mode>(config().aa_mode);

    read_game_build();
    logf("game base 0x%llx, PE timestamp %u (%s)", (unsigned long long)g_state.game_base,
         g_state.game_timestamp, g_state.tested_build ? "tested build" : "untested build, trying anyway");

    if (MH_Initialize() != MH_OK) {
        logf("MH_Initialize failed");
        return 0;
    }

    ULONGLONG start = GetTickCount64();
    while (!GetModuleHandleW(L"d3d11.dll"))
        Sleep(20);
    logf("d3d11.dll loaded after %llu ms", GetTickCount64() - start);

    // By now the game has loaded version.dll too, if it loads the old proxy at all.
    if (old_proxy_loaded(dir)) {
        logf("version.dll from an older Catalyst Clarity release is loaded: delete it from the game folder");
        return 0;
    }

    g_state.hooks_installed = install_present_hooks();
    d3d_debug::install();  // before the game creates its device (seconds later)
    // d3d11.dll can be loaded before the game's code is in place, so this waits on the code bytes.
    taa::install_function_hook();
    // Installed last: the game's launcher may raise its own exceptions during startup.
    install_crash_logger();
    logf("crash logger installed");
    return 0;
}

}  // namespace cs

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        cs::g_self = instance;
        DisableThreadLibraryCalls(instance);
        if (!cs::proxy_load())
            return FALSE;
        if (cs::is_game_process()) {
            HANDLE t = CreateThread(nullptr, 0, cs::init_thread, nullptr, 0, nullptr);
            if (t)
                CloseHandle(t);
        }
    }
    return TRUE;
}
