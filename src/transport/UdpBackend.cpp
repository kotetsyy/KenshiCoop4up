#include "NativeBackend.h"
#include <steam/steamnetworkingsockets.h>
#include <algorithm>
#include <cstring>
#include <mutex>

namespace coopnet {
namespace {
std::mutex runtimeMutex;
NativeBackend* runtimeOwner = nullptr;
void detail(char* out, uint32_t cap, const char* text) {
    if (out && cap) { size_t n=(std::min)(size_t(cap-1),strlen(text)); memcpy(out,text,n); out[n]=0; }
}
}
int32_t initializeBackend(NativeBackend& backend, const CoopNetConfig& config, char* error, uint32_t cap) {
    detail(error,cap,"");
    if (config.mode!=COOP_NET_UDP) { detail(error,cap,"UDP provider mode mismatch"); return COOP_NET_INVALID; }
    std::lock_guard<std::mutex> lock(runtimeMutex);
    if (runtimeOwner || backend.initialized) { detail(error,cap,"Standalone UDP runtime already has an active context"); return COOP_NET_UNAVAILABLE; }
    SteamNetworkingErrMsg nativeError;
    // A native IP identity is deliberately unauthenticated. GNS still negotiates
    // encryption; the provider requires it on every connected direct IP peer.
    if (!GameNetworkingSockets_Init(nullptr,nativeError)) { detail(error,cap,"Standalone UDP initialization failed"); return COOP_NET_UNAVAILABLE; }
    struct InitializationGuard {
        NativeBackend& backend;
        bool committed=false;
        ~InitializationGuard() {
            if (!committed) { GameNetworkingSockets_Kill(); backend=NativeBackend{}; runtimeOwner=nullptr; }
        }
    } guard{backend};
    backend.sockets=SteamNetworkingSockets(); backend.utils=SteamNetworkingUtils();
    if (!backend.sockets || !backend.utils) {
        detail(error,cap,"Native UDP interfaces unavailable"); return COOP_NET_UNAVAILABLE;
    }
    backend.initialized=true; runtimeOwner=&backend; guard.committed=true; return COOP_NET_OK;
}
void shutdownBackend(NativeBackend& backend) {
    std::lock_guard<std::mutex> lock(runtimeMutex);
    if (runtimeOwner==&backend && backend.initialized) { GameNetworkingSockets_Kill(); runtimeOwner=nullptr; }
    backend=NativeBackend{};
}
bool authorizeRemote(const NativeBackend& backend, const CoopNetConfig&,
                     const SteamNetConnectionInfo_t& info, char* error, uint32_t cap) {
    // Before AcceptConnection the native cipher is still INVALID. Unencrypted=0
    // requires AES-GCM negotiation; check the resulting flags at Connected.
    const bool unencrypted=info.m_eState==k_ESteamNetworkingConnectionState_Connected &&
        (info.m_nFlags & k_nSteamNetworkConnectionInfoFlags_Unencrypted);
    if (!backend.initialized || (info.m_nFlags & k_nSteamNetworkConnectionInfoFlags_Relayed) || unencrypted) {
        detail(error,cap,"UDP requires an encrypted direct peer; identities are unauthenticated"); return false;
    }
    return true;
}
int32_t pumpBackendMainThread() { return COOP_NET_INVALID; }
}
