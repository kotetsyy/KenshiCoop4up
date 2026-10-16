#include "NativeBackend.h"
#include "Credentials.h"
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
    UdpCredential credential;
    if (!loadUdpCredential(config,credential)) { detail(error,cap,"Invalid or expired UDP credential"); return COOP_NET_AUTH_FAILED; }
    SteamNetworkingIdentity identity; identity.Clear();
    if (!identity.SetGenericString(credential.localIdentity.c_str())) { detail(error,cap,"Invalid UDP credential identity"); return COOP_NET_AUTH_FAILED; }
    SteamNetworkingErrMsg nativeError;
    if (!GameNetworkingSockets_Init(&identity,nativeError)) { detail(error,cap,"Standalone UDP initialization failed"); return COOP_NET_UNAVAILABLE; }
    struct InitializationGuard {
        NativeBackend& backend;
        bool committed=false;
        ~InitializationGuard() {
            if (!committed) { GameNetworkingSockets_Kill(); clearUdpRoots(); backend=NativeBackend{}; runtimeOwner=nullptr; }
        }
    } guard{backend};
    backend.sockets=SteamNetworkingSockets(); backend.utils=SteamNetworkingUtils();
    if (!backend.sockets || !backend.utils || !installUdpRoot(credential) ||
        !installUdpCertificate(backend.sockets,credential)) {
        detail(error,cap,"Native UDP certificate installation failed"); return COOP_NET_AUTH_FAILED;
    }
    backend.expectedHostIdentity=credential.hostIdentity; backend.clientIdentityPrefix=credential.clientPrefix;
    backend.initialized=true; runtimeOwner=&backend; guard.committed=true; return COOP_NET_OK;
}
void shutdownBackend(NativeBackend& backend) {
    std::lock_guard<std::mutex> lock(runtimeMutex);
    if (runtimeOwner==&backend && backend.initialized) { GameNetworkingSockets_Kill(); clearUdpRoots(); runtimeOwner=nullptr; }
    backend=NativeBackend{};
}
bool authorizeRemote(const NativeBackend& backend, const CoopNetConfig& config,
                     const SteamNetConnectionInfo_t& info, char* error, uint32_t cap) {
    const char* identity=info.m_identityRemote.GetGenericString();
    const int forbidden=k_nSteamNetworkConnectionInfoFlags_Unauthenticated|k_nSteamNetworkConnectionInfoFlags_Relayed;
    // Before AcceptConnection the native cipher is still INVALID. The strict
    // Unencrypted=0 option negotiates AES-GCM; validate its result at Connected.
    const bool unencrypted=info.m_eState==k_ESteamNetworkingConnectionState_Connected &&
        (info.m_nFlags & k_nSteamNetworkConnectionInfoFlags_Unencrypted);
    if (!backend.initialized || !identity || (info.m_nFlags & forbidden) || unencrypted) { detail(error,cap,"UDP requires a native authenticated encrypted direct peer"); return false; }
    if (!config.host) {
        if (backend.expectedHostIdentity==identity) return true;
    } else {
        const auto& prefix=backend.clientIdentityPrefix; const size_t n=strlen(identity);
        if (n>prefix.size() && n<=prefix.size()+13 && memcmp(identity,prefix.data(),prefix.size())==0) {
            bool base36=true; for(size_t i=prefix.size();i<n;++i) if (!((identity[i]>='0' && identity[i]<='9') || (identity[i]>='a' && identity[i]<='z'))) base36=false;
            if (base36) return true;
        }
    }
    detail(error,cap,"UDP authenticated identity does not match the required host or client role"); return false;
}
int32_t prepareHostCredential(const char* path, char* error, uint32_t cap) { return provisionUdpHost(path,error,cap); }
int32_t createClientInvite(const char* path, char* invite, uint32_t inviteCap, char* error, uint32_t errorCap) {
    return issueUdpInvite(path,invite,inviteCap,error,errorCap);
}
int32_t pumpBackendMainThread() { return COOP_NET_INVALID; }
}
