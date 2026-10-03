// UiModule.cpp - load, validate and drive the optional KenshiCoopUI.dll.
//
// Owner state: this file's statics only. Everything crossing into the provider
// is the POD C ABI of ui/CoopUiApi.h; every call into it is SEH-guarded by a
// wrapper holding no destructible locals (C2712). Main thread only.

#include <windows.h>
#include <cstdio>
#include <cstring>

#include "UiModule.h"
#include "../CoopLog.h"

// Windows SDK 7.0A (v100) predates KB2533623's LoadLibraryEx search flags.
#ifndef LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR
#define LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR 0x00000100
#endif
#ifndef LOAD_LIBRARY_SEARCH_DEFAULT_DIRS
#define LOAD_LIBRARY_SEARCH_DEFAULT_DIRS 0x00001000
#endif

namespace coop {
namespace uimodule {
namespace {

enum UiState { UI_UNLOADED, UI_READY, UI_DISABLED };

UiState    g_state = UI_UNLOADED;
CoopUiApi  g_api;
CoopUiHost g_host; // handed to initialize(); lives as long as the process

const DWORD PATH_CAP = 1040;

void COOP_UI_CALL hostLog(const char* utf8, int error) {
    if (!utf8) return;
    if (error) coop::logErrLine(utf8);
    else       coop::logLine(utf8);
}

// The one failure line for this process: the UI stays off from here on.
void disable(const char* why, DWORD code) {
    g_state = UI_DISABLED;
    char b[256];
    _snprintf(b, sizeof(b) - 1,
              "[coop-ui] KenshiCoopUI.dll %s (code 0x%08lX); F2 panel disabled, networking continues",
              why, (unsigned long)code);
    b[sizeof(b) - 1] = '\0';
    coop::logErrLine(b);
}

// <folder of KenshiCoop.dll>\KenshiCoopUI.dll.
bool providerPath(wchar_t* out, DWORD cap) {
    HMODULE self = 0;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&g_api), &self))
        return false;
    const DWORD n = GetModuleFileNameW(self, out, cap);
    if (n == 0 || n >= cap) return false;
    DWORD cut = n;
    while (cut > 0 && out[cut - 1] != L'\\' && out[cut - 1] != L'/') --cut;
    static const wchar_t kName[] = L"KenshiCoopUI.dll";
    if (cut == 0 || cut + sizeof(kName) / sizeof(kName[0]) > cap) return false;
    memcpy(out + cut, kName, sizeof(kName));
    return true;
}

// Absolute path only. Where the loader supports it (Win8+, Win7 + KB2533623),
// the provider's imports resolve from its own folder, the game folder and
// System32 - never the working directory or PATH. Plain Win7 falls back to the
// altered search order, which still looks in the provider's folder first.
HMODULE loadProvider(const wchar_t* path) {
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    if (k32 && GetProcAddress(k32, "AddDllDirectory")) {
        HMODULE m = LoadLibraryExW(path, 0, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
                                            LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (m || GetLastError() != ERROR_INVALID_PARAMETER) return m;
    }
    return LoadLibraryExW(path, 0, LOAD_WITH_ALTERED_SEARCH_PATH);
}

bool getApiSeh(CoopUiGetApiFn fn, CoopUiApi* api, int* ok, DWORD* code) {
    __try {
        *ok = fn(COOP_UI_API_VERSION, (unsigned int)sizeof(CoopUiApi), api);
        return true;
    } __except (*code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool initializeSeh(int* ok, DWORD* code) {
    __try {
        *ok = g_api.initialize(&g_host);
        return true;
    } __except (*code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool tickSeh(const CoopUiSnapshot* state, CoopUiCommand* command, DWORD* code) {
    __try {
        g_api.tick(state, command);
        return true;
    } __except (*code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool shutdownSeh(DWORD* code) {
    __try {
        g_api.shutdown();
        return true;
    } __except (*code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void loadAndInitialize() {
    g_state = UI_DISABLED; // any early return below leaves the UI off for good
    wchar_t path[PATH_CAP];
    if (!providerPath(path, PATH_CAP)) { disable("path could not be resolved", GetLastError()); return; }
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) {
        disable("not found beside KenshiCoop.dll", GetLastError());
        return;
    }
    HMODULE mod = loadProvider(path);
    if (!mod) { disable("failed to load", GetLastError()); return; }
    // Mapped for the rest of the process: its DllMain ran, and the UI is never
    // hot-unloaded or reloaded. The pin also defeats any stray FreeLibrary.
    HMODULE pinned = 0;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN, path, &pinned);

    CoopUiGetApiFn getApi = (CoopUiGetApiFn)GetProcAddress(mod, "KenshiCoopUI_GetApi");
    if (!getApi) { disable("has no KenshiCoopUI_GetApi export", GetLastError()); return; }
    CoopUiApi api;
    memset(&api, 0, sizeof(api));
    int ok = 0;
    DWORD code = 0;
    if (!getApiSeh(getApi, &api, &ok, &code)) { disable("faulted in KenshiCoopUI_GetApi", code); return; }
    if (!ok) { disable("rejected the requested API version", COOP_UI_API_VERSION); return; }
    if (api.structSize != sizeof(CoopUiApi) || api.apiVersion != COOP_UI_API_VERSION ||
        !api.initialize || !api.tick || !api.shutdown) {
        disable("returned an incompatible API table", api.apiVersion);
        return;
    }
    g_api = api;
    g_host.structSize = sizeof(CoopUiHost);
    g_host.apiVersion = COOP_UI_API_VERSION;
    g_host.log        = &hostLog;
    ok = 0;
    if (!initializeSeh(&ok, &code)) { disable("faulted in initialize", code); return; }
    if (!ok) { disable("initialize failed", 0); return; }
    g_state = UI_READY;
    char b[96];
    _snprintf(b, sizeof(b) - 1, "[coop-ui] KenshiCoopUI.dll loaded (API v%u)",
              (unsigned)COOP_UI_API_VERSION);
    b[sizeof(b) - 1] = '\0';
    coop::logLine(b);
}

} // namespace

void tick(const CoopUiSnapshot* state, CoopUiCommand* command) {
    memset(command, 0, sizeof(*command));
    command->structSize = sizeof(CoopUiCommand);
    command->kind       = COOP_UI_NONE;
    if (g_state == UI_UNLOADED) loadAndInitialize();
    if (g_state != UI_READY) return;

    DWORD code = 0;
    if (!tickSeh(state, command, &code)) {
        disable("faulted in tick", code);
        memset(command, 0, sizeof(*command));
        command->structSize = sizeof(CoopUiCommand);
        // Best effort: hide whatever the provider still has on screen.
        if (!shutdownSeh(&code)) coop::logErrLine("[coop-ui] KenshiCoopUI.dll shutdown after fault also faulted");
        return;
    }
    if (command->structSize != sizeof(CoopUiCommand) ||
        command->kind < COOP_UI_NONE || command->kind > COOP_UI_DISCONNECT) {
        static bool s_warned = false;
        if (!s_warned) {
            s_warned = true;
            coop::logErrLine("[coop-ui] KenshiCoopUI.dll returned a malformed command; ignored");
        }
        memset(command, 0, sizeof(*command));
        command->structSize = sizeof(CoopUiCommand);
        return;
    }
    command->settings.udpIp[sizeof(command->settings.udpIp) - 1] = '\0';
    command->settings.playerName[sizeof(command->settings.playerName) - 1] = '\0';
}

} // namespace uimodule
} // namespace coop
