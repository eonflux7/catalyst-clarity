#pragma once
#include <string>

namespace cs {
// Plain text log next to the DLL (catalyst_clarity.log), truncated per launch, flushed per line.
void log_open(const std::wstring& path);
void logf(const char* fmt, ...);
}
