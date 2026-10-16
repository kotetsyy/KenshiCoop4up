#ifndef KENSHICOOP_NATIVE_BACKEND_H
#define KENSHICOOP_NATIVE_BACKEND_H

/* Modern-provider implementation only; never included by the v100 core.
   Each DLL links exactly one backend implementation. */
#include "TransportApi.h"
#include <steam/isteamnetworkingsockets.h>
#include <steam/isteamnetworkingutils.h>
#include <string>

namespace coopnet {
struct NativeBackend {
    ISteamNetworkingSockets* sockets = nullptr;
    ISteamNetworkingUtils* utils = nullptr;
    uint64_t selfSteamId = 0;
    std::string expectedHostIdentity;
    std::string clientIdentityPrefix;
    bool initialized = false;
};

/* Steam borrows game-owned context; UDP owns standalone GNS initialization.
   No provider may initialize the other transport, and no Steam DLL loading,
   replacement, SteamAPI_Init or SteamAPI_Shutdown is permitted here. */
int32_t initializeBackend(NativeBackend& backend, const CoopNetConfig& config,
                          char* error, uint32_t errorCapacity);
void shutdownBackend(NativeBackend& backend);
/* Steam main-thread network callback dispatch; UDP rejects this operation. */
int32_t pumpBackendMainThread();
/* Raw allocated-message send after the provider's validation/pressure gates. */
int32_t sendNativePayload(NativeBackend& backend, uint32_t handle, uint32_t traffic,
                          const void* data, uint32_t size);
/* Called before accepting an incoming peer and after authenticated CONNECTED.
   Must validate identity/role/expected host; the common session enforces
   maxPeers without a manual Steam ID allowlist. */
bool authorizeRemote(const NativeBackend& backend, const CoopNetConfig& config,
                     const SteamNetConnectionInfo_t& info, char* error,
                     uint32_t errorCapacity);
int32_t prepareHostCredential(const char* profilePath, char* error,
                              uint32_t errorCapacity);
int32_t createClientInvite(const char* profilePath, char* invite,
                           uint32_t inviteCapacity, char* error,
                           uint32_t errorCapacity);
}
#endif
