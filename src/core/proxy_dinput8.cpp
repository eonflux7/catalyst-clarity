// Resolves the real System32\dinput8.dll exports for the jump stubs in proxy_dinput8.asm.
#include <windows.h>

#include <iterator>

#include "core/proxy.h"

static const char* const kNames[] = {
    "DirectInput8Create", "DllCanUnloadNow",     "DllGetClassObject",
    "DllRegisterServer",  "DllUnregisterServer", "GetdfDIJoystick",
};

extern "C" FARPROC g_dinput8_procs[std::size(kNames)] = {};

namespace cs {

bool proxy_load() {
    wchar_t path[MAX_PATH];
    UINT n = GetSystemDirectoryW(path, MAX_PATH);
    if (n == 0 || n > MAX_PATH - 16)
        return false;
    wcscat_s(path, L"\\dinput8.dll");
    HMODULE real = LoadLibraryW(path);
    if (!real)
        return false;
    for (size_t i = 0; i < std::size(kNames); ++i)
        g_dinput8_procs[i] = GetProcAddress(real, kNames[i]);
    return true;
}

}  // namespace cs
