#include "core/log.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace cs {

static FILE* g_file;
static std::mutex g_mutex;

void log_open(const std::wstring& path) {
    std::lock_guard lock(g_mutex);
    if (!g_file)
        g_file = _wfsopen(path.c_str(), L"w", _SH_DENYWR);
}

void logf(const char* fmt, ...) {
    std::lock_guard lock(g_mutex);
    if (!g_file)
        return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(g_file, "%02u:%02u:%02u.%03u [%5lu] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
            GetCurrentThreadId());
    va_list args;
    va_start(args, fmt);
    vfprintf(g_file, fmt, args);
    va_end(args);
    fputc('\n', g_file);
    fflush(g_file);
}

}  // namespace cs
