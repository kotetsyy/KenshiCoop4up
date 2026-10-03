// Updater.cpp - manifest fetch, payload download, SHA-256 verify, on-disk swap
// of the KenshiCoop.dll + KenshiCoopUI.dll pair. See Updater.h for the design
// and the trust model.

#include "Updater.h"
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
// Written by the worker thread, read by the game thread (the F2 panel). Only
// two producers exist and neither read is used for control flow beyond display,
// so a mutex around the status buffer is enough; the flags are plain aligned
// bools whose torn-read risk on x86-64 is nil.
CRITICAL_SECTION g_lock;
bool          g_lockInit    = false;
char          g_status[256] = "update: idle";
volatile bool g_restart     = false;
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

// The UI companion always sits beside the core; the core loads it from there.
std::string uiDllPath(const std::string& dllPath) {
    size_t slash = dllPath.find_last_of("\\/");
    if (slash == std::string::npos) return std::string();
    return dllPath.substr(0, slash + 1) + "KenshiCoopUI.dll";
}

// ---- tiny manifest parser ---------------------------------------------------
// key=value per line; '#' starts a comment; surrounding whitespace trimmed.
// Deliberately dumb: this parses attacker-reachable input, so it has no state
// machine to get wrong and cannot allocate unboundedly (the caller caps size).

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) --b;
    return s.substr(a, b - a);
}

std::map<std::string, std::string> parseManifest(const std::string& text) {
    std::map<std::string, std::string> m;
    size_t i = 0;
    while (i < text.size()) {
        size_t nl = text.find('\n', i);
        std::string line = text.substr(i, nl == std::string::npos ? std::string::npos : nl - i);
        i = (nl == std::string::npos) ? text.size() : nl + 1;
        size_t h = line.find('#');
        if (h != std::string::npos) line = line.substr(0, h);
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = trim(line.substr(0, eq));
        std::string v = trim(line.substr(eq + 1));
        if (!k.empty()) m[k] = v;
    }
    return m;
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
    wchar_t host[256], path[2048];
    uc.lpszHostName = host; uc.dwHostNameLength = 256;
    uc.lpszUrlPath  = path; uc.dwUrlPathLength  = 2048;
    if (!WinHttpCrackUrl(wurl.c_str(), (DWORD)wurl.size(), 0, &uc)) {
        *err = "malformed url"; return false;
    }
    if (uc.nScheme != INTERNET_SCHEME_HTTPS) { *err = "refused: not https"; return false; }
    std::wstring hostName(host, uc.dwHostNameLength);
    if (!hostAllowed(hostName)) { *err = "refused: host not on the allow list"; return false; }

    HINTERNET ses = WinHttpOpen(L"KenshiCoop-Updater/1.0",
                                WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) { *err = "WinHttpOpen failed"; return false; }
    // Don't let a stalled connection keep the thread (and shutdown) waiting.
    WinHttpSetTimeouts(ses, 10000, 10000, 20000, 20000);

    bool ok = false;
    std::string redirect;
    HINTERNET con = WinHttpConnect(ses, hostName.c_str(), uc.nPort, 0);
    if (con) {
        HINTERNET req = WinHttpOpenRequest(
            con, L"GET", std::wstring(path, uc.dwUrlPathLength).c_str(),
            0, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
            WINHTTP_FLAG_SECURE | WINHTTP_FLAG_REFRESH);
        if (req) {
            DWORD noRedirect = WINHTTP_DISABLE_REDIRECTS;
            WinHttpSetOption(req, WINHTTP_OPTION_DISABLE_FEATURE, &noRedirect, sizeof(noRedirect));
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

// ---- SHA-256 ----------------------------------------------------------------

std::string sha256Hex(const void* data, size_t n) {
    std::string out;
    HCRYPTPROV prov = 0;
    HCRYPTHASH hash = 0;
    if (!CryptAcquireContextA(&prov, 0, 0, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)) return out;
    if (CryptCreateHash(prov, CALG_SHA_256, 0, 0, &hash)) {
        if (CryptHashData(hash, (const BYTE*)data, (DWORD)n, 0)) {
            BYTE dig[32]; DWORD dl = sizeof(dig);
            if (CryptGetHashParam(hash, HP_HASHVAL, dig, &dl, 0) && dl == 32) {
                char hex[65];
                for (int i = 0; i < 32; ++i) _snprintf(hex + i * 2, 3, "%02x", dig[i]);
                hex[64] = '\0';
                out = hex;
            }
        }
        CryptDestroyHash(hash);
    }
    CryptReleaseContext(prov, 0);
    return out;
}

// Hash the on-disk DLL. FILE_SHARE_* so this works while the image is mapped.
std::string sha256File(const std::string& path) {
    HANDLE f = CreateFileA(path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
    if (f == INVALID_HANDLE_VALUE) return std::string();
    DWORD sz = GetFileSize(f, 0);
    if (sz == INVALID_FILE_SIZE || sz == 0 || sz > 32 * 1024 * 1024) {
        CloseHandle(f);
        return std::string();
    }
    std::string buf;
    buf.resize((size_t)sz);
    DWORD got = 0;
    BOOL ok = ReadFile(f, &buf[0], sz, &got, 0);
    CloseHandle(f);
    if (!ok || got != sz) return std::string();
    return sha256Hex(buf.data(), buf.size());
}

std::string lower(const std::string& s) {
    std::string o = s;
    for (size_t i = 0; i < o.size(); ++i)
        if (o[i] >= 'A' && o[i] <= 'Z') o[i] = (char)(o[i] - 'A' + 'a');
    return o;
}

// ---- the swap ---------------------------------------------------------------

// Write `bytes` to `path`, flushed. Removes the partial file on failure.
bool writeStage(const std::string& path, const std::string& bytes) {
    HANDLE f = CreateFileA(path.c_str(), GENERIC_WRITE, 0, 0,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    BOOL wok = WriteFile(f, bytes.data(), (DWORD)bytes.size(), &wrote, 0);
    // Force it to disk before we start renaming, so a power cut cannot leave a
    // half-written image in place of a live one.
    FlushFileBuffers(f);
    CloseHandle(f);
    if (!wok || wrote != bytes.size()) {
        DeleteFileA(path.c_str());
        return false;
    }
    return true;
}

// Put both payloads on disk AS the live pair. Windows refuses to overwrite or
// delete a mapped image but DOES allow renaming one, so: write both payloads
// beside the DLLs, move each running file out of the way, move its payload in.
// Any failure undoes every step already taken, so the install is either the
// complete new pair or the untouched old pair - never a new core with an old
// UI. The UI may be absent (an install that predates the split); then there is
// nothing to move aside for it. The old images cannot be deleted until they are
// unmapped - sweepOldImage does that on the next launch.
bool swapPair(const std::string& dllPath, const std::string& bytes,
              const std::string& uiPath, const std::string& uiBytes, std::string* err) {
    std::string stage   = dllPath + ".new";
    std::string old     = dllPath + ".old";
    std::string uiStage = uiPath + ".new";
    std::string uiOld   = uiPath + ".old";

    if (!writeStage(stage, bytes)) { *err = "cannot stage the DLL next to it"; return false; }
    if (!writeStage(uiStage, uiBytes)) {
        DeleteFileA(stage.c_str());
        *err = "cannot stage the UI DLL next to it";
        return false;
    }

    // Leftovers from an earlier swap, if they unmapped.
    DeleteFileA(old.c_str());
    DeleteFileA(uiOld.c_str());
    bool uiExisted = GetFileAttributesA(uiPath.c_str()) != INVALID_FILE_ATTRIBUTES;

    // Every failure below restores what was already moved, in reverse. The new
    // files overwritten by a rollback are not mapped, so REPLACE_EXISTING works.
    const char* fail = 0;
    if (!MoveFileExA(dllPath.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        fail = "cannot move the running DLL aside (read-only install?)";
    } else if (!MoveFileExA(stage.c_str(), dllPath.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        MoveFileExA(old.c_str(), dllPath.c_str(), MOVEFILE_REPLACE_EXISTING);
        fail = "cannot move the update into place";
    } else if (uiExisted && !MoveFileExA(uiPath.c_str(), uiOld.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        MoveFileExA(old.c_str(), dllPath.c_str(), MOVEFILE_REPLACE_EXISTING);
        fail = "cannot move the running UI DLL aside";
    } else if (!MoveFileExA(uiStage.c_str(), uiPath.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        if (uiExisted) MoveFileExA(uiOld.c_str(), uiPath.c_str(), MOVEFILE_REPLACE_EXISTING);
        MoveFileExA(old.c_str(), dllPath.c_str(), MOVEFILE_REPLACE_EXISTING);
        fail = "cannot move the UI update into place";
    }
    if (!fail) return true;
    DeleteFileA(stage.c_str());
    DeleteFileA(uiStage.c_str());
    *err = fail;
    return false;
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
    std::string got = sha256Hex(payload->data(), payload->size());
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
    std::map<std::string, std::string> m = parseManifest(body);
    std::string ver   = m["version"];
    std::string sha   = lower(m["sha256"]);
    std::string url   = m["url"];
    std::string uiSha = lower(m["uiSha256"]);
    std::string uiUrl = m["uiUrl"];
    std::string proto = m["proto"];
    std::string notes = m["notes"];
    // The core and its UI companion only ever move together: a manifest that
    // does not name BOTH is not installable, whatever else it says.
    if (ver.empty() || sha.size() != 64 || url.empty() || uiSha.size() != 64 || uiUrl.empty()) {
        setStatus("manifest incomplete (need version, sha256, url, uiSha256, uiUrl)");
        return 0;
    }
    // Refuse to go BACKWARDS unless the manifest says to. A deliberate rollback
    // stays possible (allowDowngrade=1); an accidental one - a manifest built
    // from a stale checkout - no longer drags every player back with it. When
    // either side will not parse we cannot order them, so the old behaviour
    // stands: different means install. That keeps a mangled version string
    // recoverable by publishing a good one, instead of wedging every client.
    {
        coop::Ver mine = coop::parseVer(g_buildVersion);
        coop::Ver theirs = coop::parseVer(ver);
        bool allowDown = (m["allowDowngrade"] == "1" || m["allowDowngrade"] == "true");
        if (mine.ok && theirs.ok && !allowDown && coop::cmpVer(theirs, mine) < 0) {
            setStatus("published %s is OLDER than this build %s - ignoring "
                      "(set allowDowngrade=1 in the manifest to force it)",
                      ver.c_str(), g_buildVersion.c_str());
            return 0;
        }
    }
    if (ver == g_buildVersion) {
        // Same release id, but a GitHub asset may have been replaced in place
        // (a tiny fix that did not deserve a new tag), or the UI companion is
        // missing/stale (an older updater that swapped only the core). If both
        // on-disk hashes match the manifest we really are current; otherwise
        // fall through and install this version's pair.
        std::string dll = selfDllPath();
        std::string local = sha256File(dll);
        if (local.empty()) {
            setStatus("up to date (%s, proto %u)", g_buildVersion.c_str(), g_proto);
            return 0;
        }
        if (local == sha && sha256File(uiDllPath(dll)) == uiSha) {
            setStatus("up to date (%s, proto %u)", g_buildVersion.c_str(), g_proto);
            return 0;
        }
        setStatus(local == sha ? "UI companion missing or stale, reinstalling the pair ..."
                               : "same version, newer file on GitHub ...");
    }
    if (!g_settings.autoApply) {
        setStatus("update available: %s%s%s - autoApply off, install by hand",
                  ver.c_str(), proto.empty() ? "" : " proto ", proto.c_str());
        return 0;
    }

    // Both payloads are downloaded and verified BEFORE anything on disk moves;
    // either one failing leaves the installed pair untouched.
    setStatus("downloading %s ...", ver.c_str());
    std::string payload, uiPayload;
    if (!fetchVerified(url, sha, "DLL", &payload)) return 0;
    if (!fetchVerified(uiUrl, uiSha, "UI DLL", &uiPayload)) return 0;

    std::string dll = selfDllPath();
    std::string ui = uiDllPath(dll);
    if (dll.empty() || ui.empty()) { setStatus("cannot locate the loaded DLL; not swapping"); return 0; }
    if (!swapPair(dll, payload, ui, uiPayload, &err)) {
        setStatus("install failed: %s", err.c_str());
        return 0;
    }

    g_restart = true;
    setStatus("updated to %s%s%s - RESTART Kenshi to use it%s%s",
              ver.c_str(), proto.empty() ? "" : " (proto ", proto.c_str(),
              notes.empty() ? "" : " | ", notes.c_str());
    return 0;
}

} // namespace

// ---- public -----------------------------------------------------------------

// ---- BUILT-IN RELEASE SOURCE ------------------------------------------------
// Fill these in with the repository that publishes releases, then rebuild. They
// are compile-time on purpose: coop_config.json ships only in the mod-kit, so a
// config-only setting would leave every plain install silently un-updatable -
// which is exactly the population the feature exists for. Except in private
// diagnostics builds, coop_config.json overrides all of it (updateOwner /
// updateRepo / updateEnabled / ...), so a player can point at a fork or turn
// the whole thing off without a rebuild.
// Empty owner or repo = updates off, no logging, no network.
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
#ifdef KENSHICOOP_NET_DIAG
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
    if (g_thread != 0) return false;
    g_settings     = s;
    g_buildVersion = buildVersion ? buildVersion : "";
    g_proto        = protoVersion;
    if (!s.enabled) {
#ifdef KENSHICOOP_NET_DIAG
        setStatus("off (private network diagnostics build)");
#else
        setStatus("off (no release repo: set updateOwner/updateRepo in "
                  "coop_config.json, or kDefaultOwner/kDefaultRepo at build time)");
#endif
        return false;
    }
    g_thread = CreateThread(0, 0, &threadEntry, 0, 0, 0);
    if (g_thread == 0) { setStatus("cannot start the update thread"); return false; }
    return true;
}

const char* status() { return g_status; }
bool restartRequired() { return g_restart; }

void sweepOldImage() {
    std::string dll = selfDllPath();
    if (dll.empty()) return;
    const std::string olds[2] = { dll + ".old", uiDllPath(dll) + ".old" };
    for (int i = 0; i < 2; ++i) {
        if (GetFileAttributesA(olds[i].c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        if (DeleteFileA(olds[i].c_str()))
            coop::logLine(i == 0 ? "[update] removed the previous DLL left by an earlier update"
                                 : "[update] removed the previous UI DLL left by an earlier update");
    }
}

} // namespace updater
} // namespace coop
