#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "NativeBackend.h"
#include <cstdio>
#include <mutex>

namespace coopnet {
namespace {
std::mutex pumpMutex;
ISteamNetworkingSockets* pumpSockets = nullptr;
using SteamCallbackPump = void (__cdecl*)();
SteamCallbackPump pumpSteamCallbacks = nullptr;
uint32_t acquisitionCount = 0;
DWORD pumpThreadId = 0;
int32_t fail(char* error, uint32_t capacity, int32_t result, const char* text) {
    if (error && capacity) std::snprintf(error, capacity, "%s", text);
    return result;
}
template<class T> T bind(HMODULE module, const char* name) {
    return reinterpret_cast<T>(GetProcAddress(module, name));
}
bool validSteamId(uint64_t id) {
    // Public universe, individual account, desktop instance, nonzero account ID.
    return (id >> 56) == 1 && ((id >> 52) & 15) == 1 &&
           ((id >> 32) & 0xfffff) == 1 && static_cast<uint32_t>(id) != 0;
}
}

// Acquisition-only preflight: never initializes authentication or relay state.
int32_t inspectSteamBackend(NativeBackend& backend, char* error, uint32_t capacity) {
    if (backend.initialized)
        return fail(error, capacity, COOP_NET_INVALID, "Steam backend already acquired");
    HMODULE module = GetModuleHandleW(L"steam_api64.dll");
    if (!module) return fail(error, capacity, COOP_NET_UNAVAILABLE, "Game steam_api64.dll is not loaded");
    using Client = void* (__cdecl*)();
    using Handle = int (__cdecl*)();
    using Generic = void* (__cdecl*)(void*, int, int, const char*);
    using Utils = void* (__cdecl*)(void*, int, const char*);
    using LoggedOn = bool (__cdecl*)(void*);
    using SteamId = uint64_t (__cdecl*)(void*);
    using AppId = uint32_t (__cdecl*)(void*);
    const auto clientFn = bind<Client>(module, "SteamClient");
    const auto userFn = bind<Handle>(module, "SteamAPI_GetHSteamUser");
    const auto pipeFn = bind<Handle>(module, "SteamAPI_GetHSteamPipe");
    const auto genericFn = bind<Generic>(module, "SteamAPI_ISteamClient_GetISteamGenericInterface");
    const auto userInterfaceFn = bind<Generic>(module, "SteamAPI_ISteamClient_GetISteamUser");
    const auto utilsInterfaceFn = bind<Utils>(module, "SteamAPI_ISteamClient_GetISteamUtils");
    const auto loggedOnFn = bind<LoggedOn>(module, "SteamAPI_ISteamUser_BLoggedOn");
    const auto steamIdFn = bind<SteamId>(module, "SteamAPI_ISteamUser_GetSteamID");
    const auto appIdFn = bind<AppId>(module, "SteamAPI_ISteamUtils_GetAppID");
    const auto pumpFn = bind<SteamCallbackPump>(module, "SteamAPI_RunCallbacks");
    if (!clientFn || !userFn || !pipeFn || !genericFn || !userInterfaceFn ||
        !utilsInterfaceFn || !loggedOnFn || !steamIdFn || !appIdFn || !pumpFn)
        return fail(error, capacity, COOP_NET_UNAVAILABLE, "Required legacy Steam context/accessor export missing");
    void* client = clientFn();
    const int user = userFn(), pipe = pipeFn();
    if (!client || !user || !pipe)
        return fail(error, capacity, COOP_NET_UNAVAILABLE, "Game Steam client/user/pipe unavailable");
    // These opaque legacy interfaces are accessed only through their own flat exports.
    void* steamUser = userInterfaceFn(client, user, pipe, "SteamUser019");
    void* steamUtils = utilsInterfaceFn(client, pipe, "SteamUtils007");
    if (!steamUser || !steamUtils)
        return fail(error, capacity, COOP_NET_UNAVAILABLE, "SteamUser019/SteamUtils007 acquisition failed");
    if (appIdFn(steamUtils) != 233860)
        return fail(error, capacity, COOP_NET_AUTH_FAILED, "Steam AppID is not Kenshi 233860");
    const uint64_t id = steamIdFn(steamUser);
    if (!loggedOnFn(steamUser) || !validSteamId(id))
        return fail(error, capacity, COOP_NET_AUTH_FAILED, "Steam user not logged on with valid individual identity");
    auto* sockets = static_cast<ISteamNetworkingSockets*>(genericFn(client, user, pipe, "SteamNetworkingSockets012"));
    auto* utils = static_cast<ISteamNetworkingUtils*>(genericFn(client, user, pipe, "SteamNetworkingUtils004"));
    if (!sockets || !utils)
        return fail(error, capacity, COOP_NET_UNAVAILABLE, "Exact SteamNetworkingSockets012/SteamNetworkingUtils004 unavailable");
    SteamNetworkingIdentity identity;
    identity.Clear();
    if (!sockets->GetIdentity(&identity) || identity.GetSteamID64() != id)
        return fail(error, capacity, COOP_NET_AUTH_FAILED, "Networking socket identity disagrees with logged-on Steam user");
    {
        std::lock_guard<std::mutex> lock(pumpMutex);
        if (pumpSockets && (pumpSockets != sockets || pumpSteamCallbacks != pumpFn))
            return fail(error, capacity, COOP_NET_UNAVAILABLE, "Game Steam interface changed during active acquisition");
        pumpSockets = sockets;
        pumpSteamCallbacks = pumpFn;
        ++acquisitionCount;
    }
    backend.sockets = sockets;
    backend.utils = utils;
    backend.selfSteamId = id;
    backend.initialized = true;
    if (error && capacity) error[0] = 0;
    return COOP_NET_OK;
}

int32_t initializeBackend(NativeBackend& backend, const CoopNetConfig& config,
                          char* error, uint32_t capacity) {
    if (config.mode != COOP_NET_STEAM)
        return fail(error, capacity, COOP_NET_INVALID, "Steam provider rejects non-Steam mode");
    if (!config.host && !validSteamId(config.remoteSteamId))
        return fail(error, capacity, COOP_NET_INVALID, "Expected host SteamID is required");
    const int32_t result = inspectSteamBackend(backend, error, capacity);
    if (result != COOP_NET_OK) return result;
    if (!config.host && config.remoteSteamId == backend.selfSteamId) {
        shutdownBackend(backend);
        return fail(error, capacity, COOP_NET_INVALID, "Expected host must be a distinct Steam user");
    }
    const auto auth = backend.sockets->InitAuthentication();
    if (auth < k_ESteamNetworkingAvailability_NeverTried) {
        shutdownBackend(backend);
        return fail(error, capacity, COOP_NET_AUTH_FAILED, "Steam native authentication initialization failed");
    }
    backend.utils->InitRelayNetworkAccess();
    return COOP_NET_OK;
}

void shutdownBackend(NativeBackend& backend) {
    // Borrowed game context: no global shutdown and no interface release.
    std::lock_guard<std::mutex> lock(pumpMutex);
    if (backend.initialized && acquisitionCount && --acquisitionCount == 0) {
        pumpSockets = nullptr;
        pumpSteamCallbacks = nullptr;
        pumpThreadId = 0;
    }
    backend = NativeBackend{};
}

int32_t pumpBackendMainThread() {
    std::lock_guard<std::mutex> lock(pumpMutex);
    const DWORD thread = GetCurrentThreadId();
    if (pumpThreadId && pumpThreadId != thread) return COOP_NET_INVALID;
    if (!pumpThreadId) pumpThreadId = thread;
    if (!pumpSockets || !pumpSteamCallbacks) return COOP_NET_UNAVAILABLE;
    // The game's legacy Steam API does not dispatch the configured callbacks
    // of the sockets interface acquired through GetISteamGenericInterface.
    // Keep lobby dispatch, then explicitly drain sockets on the same thread.
    pumpSteamCallbacks();
    pumpSockets->RunCallbacks();
    return COOP_NET_OK;
}

bool authorizeRemote(const NativeBackend& backend, const CoopNetConfig& config,
                     const SteamNetConnectionInfo_t& info, char* error, uint32_t capacity) {
    const uint64_t remote = info.m_identityRemote.GetSteamID64();
    if (!backend.initialized || !validSteamId(remote) || remote == backend.selfSteamId ||
        (!config.host && remote != config.remoteSteamId)) {
        fail(error, capacity, COOP_NET_AUTH_FAILED, "Unexpected remote Steam identity");
        return false;
    }
    if (info.m_eState == k_ESteamNetworkingConnectionState_Connected &&
        ((info.m_nFlags & (k_nSteamNetworkConnectionInfoFlags_Unauthenticated |
                          k_nSteamNetworkConnectionInfoFlags_Unencrypted)) ||
         backend.sockets->GetAuthenticationStatus(nullptr) != k_ESteamNetworkingAvailability_Current)) {
        fail(error, capacity, COOP_NET_AUTH_FAILED, "Steam connection is not authenticated and encrypted");
        return false;
    }
    return true;
}
}
