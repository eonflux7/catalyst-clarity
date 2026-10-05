#include "core/crash.h"

#include <windows.h>

#include <atomic>

#include "core/log.h"

namespace cs {

constexpr int kMaxReports = 8;
constexpr int kMaxFrames = 24;

static std::atomic<int> g_reports;

// "module+0xoffset" for an address (no allocation; fits in out).
static void describe(DWORD64 address, char* out, size_t size) {
    HMODULE module = nullptr;
    char path[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(address), &module) &&
        module) {
        GetModuleFileNameA(module, path, MAX_PATH);
        const char* name = path;
        for (const char* p = path; *p; ++p)
            if (*p == '\\' || *p == '/')
                name = p + 1;
        snprintf(out, size, "%s+0x%llx", name, (unsigned long long)(address - reinterpret_cast<DWORD64>(module)));
    } else {
        snprintf(out, size, "0x%llx", (unsigned long long)address);
    }
}

static bool fatal(DWORD code) {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_PRIV_INSTRUCTION:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_STACK_OVERFLOW:
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
    case 0xC0000409:  // STATUS_STACK_BUFFER_OVERRUN (fail fast)
    case 0xC0000374:  // STATUS_HEAP_CORRUPTION
        return true;
    default:
        return false;
    }
}

// First-chance: the game or its wrapper may handle some of these itself, so this only logs (a few).
static LONG CALLBACK on_exception(EXCEPTION_POINTERS* info) {
    DWORD code = info->ExceptionRecord->ExceptionCode;
    if (!fatal(code) || g_reports.fetch_add(1) >= kMaxReports)
        return EXCEPTION_CONTINUE_SEARCH;

    char where[MAX_PATH + 32];
    describe(reinterpret_cast<DWORD64>(info->ExceptionRecord->ExceptionAddress), where, sizeof(where));
    if (code == EXCEPTION_ACCESS_VIOLATION && info->ExceptionRecord->NumberParameters >= 2)
        logf("crash: exception 0x%08lx at %s (%s 0x%llx)", code, where,
             info->ExceptionRecord->ExceptionInformation[0] == 1 ? "write" : "read",
             (unsigned long long)info->ExceptionRecord->ExceptionInformation[1]);
    else
        logf("crash: exception 0x%08lx at %s", code, where);
    if (code == EXCEPTION_STACK_OVERFLOW)
        return EXCEPTION_CONTINUE_SEARCH;  // not enough stack to unwind safely

    CONTEXT ctx = *info->ContextRecord;
    for (int i = 0; i < kMaxFrames && ctx.Rip; ++i) {
        describe(ctx.Rip, where, sizeof(where));
        logf("crash:   #%02d %s", i, where);
        DWORD64 image_base = 0;
        RUNTIME_FUNCTION* fn = RtlLookupFunctionEntry(ctx.Rip, &image_base, nullptr);
        if (!fn) {  // leaf function: return address at [rsp]
            ctx.Rip = *reinterpret_cast<DWORD64*>(ctx.Rsp);
            ctx.Rsp += 8;
            continue;
        }
        void* handler_data = nullptr;
        DWORD64 establisher = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, ctx.Rip, fn, &ctx, &handler_data, &establisher, nullptr);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

void install_crash_logger() {
    AddVectoredExceptionHandler(1, on_exception);
}

}  // namespace cs
