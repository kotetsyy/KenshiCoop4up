// CoopLog implementation. See CoopLog.h for rationale.
//
// VS2010 (v100) compatible: Win32 CRITICAL_SECTION + GetLocalTime, plain stdio.

#define _CRT_SECURE_NO_WARNINGS 1

#include "CoopLog.h"
#ifdef KENSHICOOP_NET_DIAG
#include "../netproto/Wire.h"
#include <cstring>
#endif

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

#ifdef KENSHICOOP_NET_DIAG
enum { MIRROR_FILE_CAP = 16 * 1024 * 1024 };
HANDLE g_mirrorRead = INVALID_HANDLE_VALUE;
unsigned __int64 g_mirrorOffset = 0;
unsigned g_mirrorPeeked = 0;
FILE* g_remoteFiles[MAX_JOINS + 1] = { 0 };
unsigned __int64 g_remoteOffsets[MAX_JOINS + 1] = { 0 };
unsigned g_remoteSizes[MAX_JOINS + 1] = { 0 };

// Call with g_cs held.
void closeMirror() {
    if (g_mirrorRead != INVALID_HANDLE_VALUE) {
        CloseHandle(g_mirrorRead);
        g_mirrorRead = INVALID_HANDLE_VALUE;
    }
    g_mirrorOffset = 0;
    g_mirrorPeeked = 0;
}

// Call with g_cs held.
void closeRemote(unsigned peerId) {
    if (g_remoteFiles[peerId]) {
        std::fclose(g_remoteFiles[peerId]);
        g_remoteFiles[peerId] = 0;
    }
    g_remoteOffsets[peerId] = 0;
    g_remoteSizes[peerId] = 0;
}

// The peer id is range-checked before this function. Use only the directory
// from the configured host log, never any untrusted bytes for the filename.
bool remotePath(unsigned peerId, char* path) {
    size_t dirLen = 0;
    for (size_t i = 0; g_path[i]; ++i) {
        if (g_path[i] == '\\' || g_path[i] == '/') dirLen = i + 1;
    }
    if (dirLen >= MAX_PATH) return false;
    std::memcpy(path, g_path, dirLen);
    int n = _snprintf(path + dirLen, MAX_PATH - dirLen,
                      "KenshiCoop_join_%u_mirror.log", peerId);
    return n > 0 && (size_t)n < MAX_PATH - dirLen;
}
#endif

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
#ifdef KENSHICOOP_NET_DIAG
        closeMirror();
        for (unsigned id = 1; id <= MAX_JOINS; ++id) closeRemote(id);
#endif
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

#ifdef KENSHICOOP_NET_DIAG
void logMirrorCapture(bool enabled) {
    if (!g_init) return;
    EnterCriticalSection(&g_cs);
    closeMirror();
    if (enabled && g_fp && g_path[0]) {
        g_mirrorRead = CreateFileA(g_path, GENERIC_READ,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE, 0,
                                   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
    }
    LeaveCriticalSection(&g_cs);
}

unsigned logMirrorPeek(char* out, unsigned cap, unsigned __int64* offset) {
    if (!g_init || !out || !cap || !offset) return 0;
    EnterCriticalSection(&g_cs);
    DWORD count = 0;
    if (g_mirrorRead != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER at;
        at.QuadPart = (LONGLONG)g_mirrorOffset;
        if (SetFilePointerEx(g_mirrorRead, at, 0, FILE_BEGIN) &&
            ReadFile(g_mirrorRead, out, cap, &count, 0)) {
            *offset = g_mirrorOffset;
            g_mirrorPeeked = count;
        } else {
            closeMirror();
        }
    }
    LeaveCriticalSection(&g_cs);
    return (unsigned)count;
}

void logMirrorCommit(unsigned bytes) {
    if (!g_init) return;
    EnterCriticalSection(&g_cs);
    if (g_mirrorRead != INVALID_HANDLE_VALUE && bytes && bytes <= g_mirrorPeeked) {
        g_mirrorOffset += bytes;
        g_mirrorPeeked = 0;
    }
    LeaveCriticalSection(&g_cs);
}

bool logRemoteChunk(unsigned peerId, unsigned __int64 offset,
                    const char* data, unsigned bytes) {
    if (!g_init || peerId == 0 || peerId > MAX_JOINS ||
        !data || !bytes || bytes > MIRROR_FILE_CAP) return false;
    EnterCriticalSection(&g_cs);
    bool ok = false;
    FILE*& fp = g_remoteFiles[peerId];
    if (offset == 0) {
        // Offset zero denotes a fresh connection; restart its file and stream.
        closeRemote(peerId);
        char path[MAX_PATH];
        if (remotePath(peerId, path)) fp = std::fopen(path, "wb");
    }
    if (fp && offset == g_remoteOffsets[peerId] &&
        offset <= ~((unsigned __int64)0) - bytes) {
        if (g_remoteSizes[peerId] > MIRROR_FILE_CAP - bytes) {
            // Rotation discards the old on-disk prefix, not the wire offset.
            char path[MAX_PATH];
            if (remotePath(peerId, path)) {
                std::fclose(fp);
                fp = std::fopen(path, "wb");
            } else {
                std::fclose(fp);
                fp = 0;
            }
            g_remoteSizes[peerId] = 0;
        }
        if (fp && std::fwrite(data, 1, bytes, fp) == bytes &&
            std::fflush(fp) == 0) {
            g_remoteSizes[peerId] += bytes;
            g_remoteOffsets[peerId] += bytes;
            ok = true;
        } else {
            // A partial write cannot be retried safely with the same offset.
            closeRemote(peerId);
        }
    }
    LeaveCriticalSection(&g_cs);
    return ok;
}

void logRemoteClose(unsigned peerId) {
    if (!g_init || peerId == 0 || peerId > MAX_JOINS) return;
    EnterCriticalSection(&g_cs);
    closeRemote(peerId);
    LeaveCriticalSection(&g_cs);
}
#endif

void logClose() {
    if (!g_init) return;
    EnterCriticalSection(&g_cs);
#ifdef KENSHICOOP_NET_DIAG
    closeMirror();
    for (unsigned id = 1; id <= MAX_JOINS; ++id) closeRemote(id);
#endif
    if (g_fp) {
        std::fflush(g_fp);
        std::fclose(g_fp);
        g_fp = 0;
    }
    LeaveCriticalSection(&g_cs);
}

} // namespace coop
