// Schema-2 fixed-repository updater: all four native-runtime images or none.

#include "Updater.h"
#include "UpdatePackage.h"
#include "RuntimeImages.h"
#include "../CoopLog.h"
#include "../core/Config.h"
#include "../core/SemVer.h" // version ordering (unit-tested in prototest)

#include <windows.h>
#include <winhttp.h>
#include <wincrypt.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <map>
#include <vector>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "advapi32.lib")

namespace coop {
namespace updater {

namespace {

// ---- process-wide state -----------------------------------------------------
// Status is written by the download worker and displayed by MAIN. Runtime image
// replacement additionally owns the shared companion/session lifecycle barrier.
CRITICAL_SECTION g_lock;
bool          g_lockInit    = false;
char          g_status[256] = "update: idle";
volatile bool g_restart     = false;
bool          g_recoverySafe = true;
HANDLE        g_thread      = 0;
Settings      g_settings;
std::string   g_buildVersion;
unsigned int  g_proto       = 0;

void setStatus(const char* fmt, ...) {
    char b[256];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf(b, sizeof(b) - 1, fmt, ap);
    va_end(ap);
    b[sizeof(b) - 1] = '\0';
    if (g_lockInit) EnterCriticalSection(&g_lock);
    strncpy(g_status, b, sizeof(g_status) - 1);
    g_status[sizeof(g_status) - 1] = '\0';
    if (g_lockInit) LeaveCriticalSection(&g_lock);
    char line[300];
    _snprintf(line, sizeof(line) - 1, "[update] %s", b);
    line[sizeof(line) - 1] = '\0';
    coop::logLine(line);
}

// ---- paths ------------------------------------------------------------------

// Full path of the loaded KenshiCoop.dll. Empty when the module is not found,
// which disables every path that would write next to it.
std::string selfDllPath() {
    char buf[MAX_PATH];
    HMODULE h = GetModuleHandleA("KenshiCoop.dll");
    if (h == 0) return std::string();
    DWORD n = GetModuleFileNameA(h, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return std::string();
    return std::string(buf, n);
}



// ---- HTTPS ------------------------------------------------------------------

// Hosts a payload URL is allowed to name. The manifest is fetched from a URL we
// build ourselves, but `url=` inside it is data, so it gets checked before we
// connect: a compromised manifest must not be able to redirect the download to
// an arbitrary host, and WinHTTP's automatic redirect handling is turned off
// for the same reason.
bool hostAllowed(const std::wstring& host) {
    static const wchar_t* const kOk[] = {
        L"github.com",
        L"objects.githubusercontent.com",
        L"release-assets.githubusercontent.com",
        L"raw.githubusercontent.com",
        L"codeload.github.com"
    };
    for (size_t i = 0; i < sizeof(kOk) / sizeof(kOk[0]); ++i)
        if (host == kOk[i]) return true;
    return false;
}

std::wstring widen(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), 0, 0);
    if (n <= 0) return std::wstring();
    std::vector<wchar_t> b((size_t)n);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &b[0], n);
    return std::wstring(&b[0], (size_t)n);
}

// GET `url` into `out`, refusing anything bigger than maxBytes. Returns false
// and fills `err` on any failure. Redirects are NOT followed automatically:
// GitHub serves release assets as a 302 to its object store, so exactly one
// hop is taken manually and its target re-checked against hostAllowed.
bool httpGet(const std::string& url, std::string* out, size_t maxBytes,
             std::string* err, int hopsLeft) {
    out->clear();
    if (hopsLeft < 0) { *err = "too many redirects"; return false; }

    std::wstring wurl = widen(url);
    URL_COMPONENTS uc;
    memset(&uc, 0, sizeof(uc));
    uc.dwStructSize = sizeof(uc);
    uc.dwUserNameLength = (DWORD)-1; uc.dwPasswordLength = (DWORD)-1;
    wchar_t host[256], path[2048], extra[4096];
    uc.lpszHostName = host; uc.dwHostNameLength = 256;
    uc.lpszUrlPath  = path; uc.dwUrlPathLength  = 2048;
    uc.lpszExtraInfo = extra; uc.dwExtraInfoLength = 4096;
    if (!WinHttpCrackUrl(wurl.c_str(), (DWORD)wurl.size(), 0, &uc)) {
        *err = "malformed url"; return false;
    }
    if (uc.nScheme != INTERNET_SCHEME_HTTPS || uc.nPort != 443 ||
        uc.dwUserNameLength || uc.dwPasswordLength) { *err = "refused: unsafe HTTPS URL"; return false; }
    std::wstring hostName(host, uc.dwHostNameLength);
    if (!hostAllowed(hostName)) { *err = "refused: host not on the allow list"; return false; }

    HINTERNET ses = WinHttpOpen(L"KenshiCoop-Updater/1.0",
                                WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) { *err = "WinHttpOpen failed"; return false; }
    // Don't let a stalled connection keep the thread (and shutdown) waiting.
    if (!WinHttpSetTimeouts(ses, 10000, 10000, 20000, 20000)) {
        WinHttpCloseHandle(ses); *err = "cannot set bounded HTTP timeouts"; return false;
    }

    bool ok = false;
    std::string redirect;
    HINTERNET con = WinHttpConnect(ses, hostName.c_str(), uc.nPort, 0);
    std::wstring requestPath = std::wstring(path, uc.dwUrlPathLength) + std::wstring(extra, uc.dwExtraInfoLength);
    if (con) {
        HINTERNET req = WinHttpOpenRequest(
            con, L"GET", requestPath.c_str(),
            0, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
            WINHTTP_FLAG_SECURE | WINHTTP_FLAG_REFRESH);
        if (req) {
            DWORD noRedirect = WINHTTP_DISABLE_REDIRECTS;
            if (!WinHttpSetOption(req, WINHTTP_OPTION_DISABLE_FEATURE, &noRedirect, sizeof(noRedirect))) {
                WinHttpCloseHandle(req); WinHttpCloseHandle(con); WinHttpCloseHandle(ses);
                *err = "cannot disable automatic redirects"; return false;
            }
            WinHttpAddRequestHeaders(req,
                L"Cache-Control: no-cache\r\nPragma: no-cache",
                (DWORD)-1L, WINHTTP_ADDREQ_FLAG_ADD);
            if (WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                   WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                WinHttpReceiveResponse(req, 0)) {
                DWORD code = 0, len = sizeof(code);
                WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                    WINHTTP_HEADER_NAME_BY_INDEX, &code, &len, WINHTTP_NO_HEADER_INDEX);
                if (code == 301 || code == 302 || code == 303 || code == 307 || code == 308) {
                    wchar_t loc[2048]; DWORD ll = sizeof(loc);
                    if (WinHttpQueryHeaders(req, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX,
                                            loc, &ll, WINHTTP_NO_HEADER_INDEX)) {
                        int n = WideCharToMultiByte(CP_UTF8, 0, loc, (int)(ll / sizeof(wchar_t)),
                                                    0, 0, 0, 0);
                        if (n > 0) {
                            std::vector<char> b((size_t)n);
                            WideCharToMultiByte(CP_UTF8, 0, loc, (int)(ll / sizeof(wchar_t)),
                                                &b[0], n, 0, 0);
                            redirect.assign(&b[0], (size_t)n);
                            while (!redirect.empty() && redirect[redirect.size() - 1] == '\0') redirect.erase(redirect.size() - 1);
                        }
                    }
                    if (redirect.empty()) *err = "redirect without a location";
                } else if (code != 200) {
                    char e[64]; _snprintf(e, sizeof(e) - 1, "http %lu", (unsigned long)code);
                    e[sizeof(e) - 1] = '\0'; *err = e;
                } else {
                    ok = true;
                    for (;;) {
                        DWORD avail = 0;
                        if (!WinHttpQueryDataAvailable(req, &avail)) { ok = false; *err = "read failed"; break; }
                        if (avail == 0) break;
                        if (out->size() + avail > maxBytes) { ok = false; *err = "payload too large"; break; }
                        std::vector<char> chunk((size_t)avail);
                        DWORD got = 0;
                        if (!WinHttpReadData(req, &chunk[0], avail, &got)) { ok = false; *err = "read failed"; break; }
                        if (got == 0) break;
                        out->append(&chunk[0], (size_t)got);
                    }
                }
            } else {
                *err = "request failed (no network?)";
            }
            WinHttpCloseHandle(req);
        } else { *err = "WinHttpOpenRequest failed"; }
        WinHttpCloseHandle(con);
    } else { *err = "WinHttpConnect failed"; }
    WinHttpCloseHandle(ses);

    if (!redirect.empty()) return httpGet(redirect, out, maxBytes, err, hopsLeft - 1);
    return ok;
}


// ---- worker -----------------------------------------------------------------

// Download `url` and accept it only as a PE image hashing to `sha`. Sets the
// status and returns false otherwise; `what` names the file in that status.
bool fetchVerified(const std::string& url, const std::string& sha, const char* what,
                   std::string* payload) {
    std::string err;
    if (!httpGet(url, payload, 32 * 1024 * 1024, &err, 3)) {
        setStatus("%s download failed: %s", what, err.c_str());
        return false;
    }
    // A DLL that does not even start with "MZ" is not one; catches a repo that
    // serves an HTML error page with a 200.
    if (payload->size() < 2 || (*payload)[0] != 'M' || (*payload)[1] != 'Z') {
        setStatus("%s download rejected: not a PE image", what);
        return false;
    }
    std::string got = packageHash(payload->data(), payload->size());
    if (got.empty()) { setStatus("cannot hash the download (CryptoAPI unavailable)"); return false; }
    if (got != sha) {
        setStatus("%s download REJECTED: sha256 mismatch (got %.12s..., want %.12s...)",
                  what, got.c_str(), sha.c_str());
        return false;
    }
    return true;
}

DWORD WINAPI threadEntry(LPVOID) {
    std::string manifestUrl = "https://raw.githubusercontent.com/" + g_settings.owner + "/" +
                              g_settings.repo + "/" + g_settings.branch + "/" + g_settings.path;
    std::string body, err;
    setStatus("checking %s/%s ...", g_settings.owner.c_str(), g_settings.repo.c_str());
    if (!httpGet(manifestUrl, &body, 64 * 1024, &err, 3)) {
        setStatus("check failed: %s", err.c_str());
        return 0;
    }
    PackageManifest manifest;
    if (!parsePackageManifest(body, &manifest, &err)) {
        setStatus("manifest rejected: %s", err.c_str()); return 0;
    }
    coop::Ver mine = coop::parseVer(g_buildVersion);
    coop::Ver theirs = coop::parseVer(manifest.version);
    if (!mine.ok || !theirs.ok) { setStatus("invalid installed/published version"); return 0; }
    if (!manifest.allowDowngrade && coop::cmpVer(theirs, mine) < 0) {
        setStatus("published %s is older; ignoring", manifest.version.c_str()); return 0;
    }
    std::string dll = selfDllPath();
    size_t slash = dll.find_last_of("\\/");
    if (dll.empty() || slash == std::string::npos) { setStatus("cannot locate installed images"); return 0; }
    std::string directory = dll.substr(0, slash + 1);
    if (manifest.version == g_buildVersion) {
        bool matches = true;
        for (int i = 0; i < 4; ++i)
            if (packageFileHash(directory + kComponentNames[i]) != manifest.hash[i]) matches = false;
        if (matches) { setStatus("up to date (%s, proto %u)", g_buildVersion.c_str(), g_proto); return 0; }
    }
    if (!g_settings.autoApply) {
        setStatus("update available: %s - autoApply off", manifest.version.c_str()); return 0;
    }
    setStatus("downloading complete runtime %s ...", manifest.version.c_str());
    std::string payloads[4];
    for (int i = 0; i < 4; ++i)
        if (!fetchVerified(manifest.url[i], manifest.hash[i], kComponentNames[i], &payloads[i])) return 0;
    RuntimeImageApplyGuard apply;
    if (!apply.ready()) {
        setStatus("install deferred: cannot acquire runtime image lifecycle"); return 0;
    }
    if (!installPackage(directory, manifest, payloads, &err)) {
        if (!packageRecoveryPending(directory)) apply.allowLoadsAfterRollback();
        else g_restart = true;
        setStatus("install failed: %s%s", err.c_str(),
                  runtimeImagesRestartOnly() ? " - RESTART Kenshi; preserve recovery files" : "");
        return 0;
    }
    g_restart = true;
    setStatus("updated to %s - RESTART Kenshi%s%s", manifest.version.c_str(),
              manifest.notes.empty() ? "" : " | ", manifest.notes.c_str());
    return 0;
}

} // namespace

// ---- public -----------------------------------------------------------------

// ---- BUILT-IN RELEASE SOURCE ------------------------------------------------
// The owner/repository/feed are fixed. Config may disable checking/application,
// but cannot redirect this binary's trust root to an arbitrary fork.
const char* const kDefaultOwner   = "kotetsyy";
const char* const kDefaultRepo    = "KenshiCoop4up";
const bool        kDefaultEnabled = true;

Settings settingsFromConfig() {
    Settings s;
    s.enabled   = kDefaultEnabled;
    s.owner     = kDefaultOwner;
    s.repo      = kDefaultRepo;
    s.branch    = "main";
    s.path      = "dist/UPDATE.txt";
    s.autoApply = true;
    coop::readUpdateSettings(&s.enabled, &s.owner, &s.repo, &s.branch, &s.path, &s.autoApply);
#if defined(KENSHICOOP_HARNESS) || defined(KENSHICOOP_NET_DIAG) || defined(_DEBUG)
    // The private protocol must never be replaced by a public release, even
    // when a tester still has updateEnabled=true in their local config.
    s.enabled = false;
#endif
    // No repo, nothing to check - stay quiet rather than logging an error every launch.
    if (s.owner.empty() || s.repo.empty()) s.enabled = false;
    if (s.branch.empty()) s.branch = "main";
    if (s.path.empty())   s.path   = "dist/UPDATE.txt";
    return s;
}

bool start(const Settings& s, const char* buildVersion, unsigned int protoVersion) {
    if (!g_lockInit) { InitializeCriticalSection(&g_lock); g_lockInit = true; }
    if (!g_recoverySafe || g_restart) return false;
#if defined(KENSHICOOP_HARNESS) || defined(KENSHICOOP_NET_DIAG) || defined(_DEBUG)
    setStatus("off (private Harness/Debug build)");
    return false;
#endif
    if (s.owner != kDefaultOwner || s.repo != kDefaultRepo ||
        s.branch != "main" || s.path != "dist/UPDATE.txt") {
        setStatus("off (update source must match the fixed release repository)");
        return false;
    }
    if (g_thread != 0) return false;
    g_settings     = s;
    g_buildVersion = buildVersion ? buildVersion : "";
    g_proto        = protoVersion;
    if (!s.enabled) {
        setStatus("off (updates disabled)");
        return false;
    }
    g_thread = CreateThread(0, 0, &threadEntry, 0, 0, 0);
    if (g_thread == 0) { setStatus("cannot start the update thread"); return false; }
    return true;
}

const char* status() { return g_status; }
bool restartRequired() { return runtimeImagesRestartOnly() || g_restart; }

bool sweepOldImage() {
    std::string dll = selfDllPath();
    size_t slash = dll.find_last_of("\\/");
    if (dll.empty() || slash == std::string::npos) {
        g_recoverySafe = false; setStatus("recovery blocked: cannot locate core"); return false;
    }
    const std::string deployMarker = dll.substr(0, slash + 1) + ".kenshicoop-deploy.pending";
    if (GetFileAttributesA(deployMarker.c_str()) != INVALID_FILE_ATTRIBUTES) {
        g_recoverySafe = false;
        setStatus("deployment recovery pending; initialization blocked, preserve backups");
        return false;
    }
    const DWORD markerError = GetLastError();
    if (markerError != ERROR_FILE_NOT_FOUND && markerError != ERROR_PATH_NOT_FOUND) {
        g_recoverySafe = false;
        setStatus("cannot verify deployment recovery marker; initialization blocked");
        return false;
    }
#if defined(KENSHICOOP_HARNESS) || defined(KENSHICOOP_NET_DIAG) || defined(_DEBUG)
    // Private test runtimes must never recover/delete a public updater's files.
    std::string root = dll.substr(0, slash + 1);
    bool pending = packageRecoveryPending(root);
    if (pending) {
        g_recoverySafe = false; setStatus("private runtime preserves public recovery files; inspect/restart");
        return false;
    }
    return true;
#endif
    const std::string before = packageFileHash(dll);
    std::string error; bool restart = false;
    if (!recoverPackageForStartup(dll.substr(0, slash + 1), before, &restart, &error)) {
        g_recoverySafe = false;
        if (restart) { g_restart = true; setStatus("%s", error.c_str()); }
        else setStatus("recovery blocked: %s", error.c_str());
        return false;
    }
    return true;
}

} // namespace updater
} // namespace coop
