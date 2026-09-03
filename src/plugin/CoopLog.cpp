// CoopLog implementation. See CoopLog.h for rationale.
//
// VS2010 (v100) compatible: Win32 CRITICAL_SECTION + GetLocalTime, plain stdio.

#define _CRT_SECURE_NO_WARNINGS 1

#include "CoopLog.h"

#include <windows.h>
#include <cstdio>

namespace coop {
namespace {

FILE*            g_fp   = 0;
CRITICAL_SECTION g_cs;
bool             g_init = false;
char             g_tag[16] = { 0 };
// Currently open log file, so logRetarget can tell a real change from a no-op
// and can name the previous file in the breadcrumb it leaves behind.
char             g_path[MAX_PATH] = { 0 };
volatile long    g_fakeSkewMs = 0;

void writeLine(const char* level, const char* msg) {
    if (!g_init) return;
    EnterCriticalSection(&g_cs);
    if (g_fp) {
        // Derive the stamp from wallClockMs() (real clock + injected skew) so
        // log timestamps and the wire time-sync share one clock.
        unsigned long ms = wallClockMs();
        unsigned long hh = (ms / 3600000ul) % 24ul;
        unsigned long mm = (ms / 60000ul) % 60ul;
        unsigned long ss = (ms / 1000ul) % 60ul;
        unsigned long mmm = ms % 1000ul;
        std::fprintf(g_fp, "[%02lu:%02lu:%02lu.%03lu] [%s] %s: %s\n",
                     hh, mm, ss, mmm,
                     g_tag, level, msg ? msg : "");
        std::fflush(g_fp);
    }
    LeaveCriticalSection(&g_cs);
}

} // namespace

unsigned long wallClockMs() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    long ms = (long)((((unsigned long)st.wHour * 60ul + st.wMinute) * 60ul + st.wSecond) * 1000ul
                     + st.wMilliseconds);
    ms += g_fakeSkewMs;
    // Wrap into [0, 24h) so a skew across midnight still formats sanely.
    const long DAY = 24l * 3600l * 1000l;
    ms %= DAY;
    if (ms < 0) ms += DAY;
    return (unsigned long)ms;
}

void logSetFakeSkewMs(long skewMs) { g_fakeSkewMs = skewMs; }

void logInit(const char* path, const char* modeTag) {
    if (g_init) return;
    InitializeCriticalSection(&g_cs);
    g_init = true;

    if (modeTag) {
        size_t i = 0;
        for (; modeTag[i] && i < sizeof(g_tag) - 1; ++i) g_tag[i] = modeTag[i];
        g_tag[i] = '\0';
    }

    if (path && path[0]) {
        g_fp = std::fopen(path, "w"); // fresh file each run
        size_t i = 0;
        for (; path[i] && i < sizeof(g_path) - 1; ++i) g_path[i] = path[i];
        g_path[i] = '\0';
    }
    writeLine("INFO", "log opened");
}

bool logRetarget(const char* path, const char* modeTag) {
    if (!g_init || !path || !path[0]) return false;

    EnterCriticalSection(&g_cs);
    bool same = false;
    {   // Case-insensitive: the same file can be named either way on Windows.
        size_t i = 0;
        for (;; ++i) {
            char a = g_path[i], b = path[i];
            if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
            if (a != b) break;
            if (a == '\0') { same = true; break; }
        }
    }
    FILE* old = g_fp;
    char  oldPath[MAX_PATH];
    { size_t i = 0; for (; g_path[i] && i < sizeof(oldPath) - 1; ++i) oldPath[i] = g_path[i];
      oldPath[i] = '\0'; }
    if (!same) {
        size_t i = 0;
        for (; path[i] && i < sizeof(g_path) - 1; ++i) g_path[i] = path[i];
        g_path[i] = '\0';
        g_fp = std::fopen(path, "w");
        if (modeTag) {
            size_t j = 0;
            for (; modeTag[j] && j < sizeof(g_tag) - 1; ++j) g_tag[j] = modeTag[j];
            g_tag[j] = '\0';
        }
    }
    LeaveCriticalSection(&g_cs);
    if (same) return false;

    // Breadcrumb in BOTH directions, written outside the lock via the normal
    // path so both lines carry a timestamp. Whoever reads either half can find
    // the other; a log that just stops at the moment of Connect is the thing
    // that wasted time before.
    if (old) {
        char b[MAX_PATH + 64];
        _snprintf(b, sizeof(b) - 1, "log continues in %s (role decided at Connect)", g_path);
        b[sizeof(b) - 1] = '\0';
        // Write directly: writeLine() would target the NEW file.
        EnterCriticalSection(&g_cs);
        std::fprintf(old, "[%s] INFO: %s\n", g_tag, b);
        std::fflush(old);
        std::fclose(old);
        LeaveCriticalSection(&g_cs);
        _snprintf(b, sizeof(b) - 1, "log opened (continued from %s)", oldPath);
        b[sizeof(b) - 1] = '\0';
        writeLine("INFO", b);
    } else {
        writeLine("INFO", "log opened");
    }
    return true;
}

void logLine(const char* msg)    { writeLine("INFO",  msg); }
void logErrLine(const char* msg) { writeLine("ERROR", msg); }

void logClose() {
    if (!g_init) return;
    EnterCriticalSection(&g_cs);
    if (g_fp) {
        std::fflush(g_fp);
        std::fclose(g_fp);
        g_fp = 0;
    }
    LeaveCriticalSection(&g_cs);
}

} // namespace coop
