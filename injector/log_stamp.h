#pragma once
// Timestamps every line the launcher prints (stdout is redirected to launcher.log
// by RedirectStdioToLogFile). Include AFTER all other headers in a .cpp: it
// #defines printf so the existing printf() calls in that file pick it up.
#include <Windows.h>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>

inline int StampedPrintf(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    const int needed = vsnprintf(nullptr, 0, fmt, ap);
    va_end(ap);
    if (needed <= 0) {
        va_end(ap2);
        return needed;
    }
    std::string body(static_cast<size_t>(needed) + 1, '\0');
    vsnprintf(body.data(), body.size(), fmt, ap2);
    va_end(ap2);
    body.resize(static_cast<size_t>(needed));

    SYSTEMTIME t;
    GetLocalTime(&t);
    char stamp[48];
    snprintf(stamp, sizeof stamp, "[%04u-%02u-%02u %02u:%02u:%02u.%03u] ",
             t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);

    static std::mutex mtx;
    static bool atLineStart = true;  // guarded by mtx
    std::lock_guard<std::mutex> lock(mtx);
    std::string out;
    out.reserve(body.size() + 64);
    for (char c : body) {
        if (atLineStart) {
            out += stamp;
            atLineStart = false;
        }
        out += c;
        if (c == '\n')
            atLineStart = true;
    }
    fwrite(out.data(), 1, out.size(), stdout);
    return needed;
}

#define printf StampedPrintf
