// Resolves the real System32\version.dll exports for the jump stubs in proxy_version.asm.
#include <windows.h>

#include <iterator>

#include "core/proxy.h"

static const char* const kNames[] = {
    "GetFileVersionInfoA",     "GetFileVersionInfoByHandle", "GetFileVersionInfoExA",
    "GetFileVersionInfoExW",   "GetFileVersionInfoSizeA",    "GetFileVersionInfoSizeExA",
    "GetFileVersionInfoSizeExW", "GetFileVersionInfoSizeW",  "GetFileVersionInfoW",
    "VerFindFileA",            "VerFindFileW",               "VerInstallFileA",
    "VerInstallFileW",         "VerLanguageNameA",           "VerLanguageNameW",
    "VerQueryValueA",          "VerQueryValueW",
};

extern "C" FARPROC g_version_procs[std::size(kNames)] = {};

namespace cs {

bool proxy_load() {
    wchar_t path[MAX_PATH];
    UINT n = GetSystemDirectoryW(path, MAX_PATH);
    if (n == 0 || n > MAX_PATH - 16)
        return false;
    wcscat_s(path, L"\\version.dll");
    HMODULE real = LoadLibraryW(path);
    if (!real)
        return false;
    for (size_t i = 0; i < std::size(kNames); ++i)
        g_version_procs[i] = GetProcAddress(real, kNames[i]);
    return true;
}

}  // namespace cs
