#include "src/common/diagnostic_trace.h"

#include "src/common/unique_handle.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>

#ifndef _countof
#define _countof(array) (sizeof(array) / sizeof((array)[0]))
#endif

namespace myrd {

void diagnostic_trace(const char* step) {
    if (step == nullptr) return;

    wchar_t path[MAX_PATH]{};
    const DWORD length = GetEnvironmentVariableW(L"MYRD_TRACE_FILE", path,
                                                 static_cast<DWORD>(MAX_PATH));
    if (length == 0 || length >= MAX_PATH) return;

    UniqueHandle file(CreateFileW(path, FILE_APPEND_DATA,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                  OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file) return;

    SYSTEMTIME now{};
    GetSystemTime(&now);
    char line[320]{};
    const int written = _snprintf_s(
        line, _countof(line), _TRUNCATE,
        "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ %s\r\n", now.wYear, now.wMonth,
        now.wDay, now.wHour, now.wMinute, now.wSecond, now.wMilliseconds, step);
    if (written <= 0) return;

    DWORD ignored = 0;
    static_cast<void>(WriteFile(file.get(), line,
                                static_cast<DWORD>(written), &ignored, nullptr));
    // Forced to the device before returning, and that is the whole point rather than caution:
    // the process this is explaining is expected to be *killed* for exceeding its budget, and
    // a buffered append would be lost at exactly the moment it was needed.
    static_cast<void>(FlushFileBuffers(file.get()));
}

} // namespace myrd
