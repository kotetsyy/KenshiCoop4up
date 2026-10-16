#if defined(_WIN32) && COOP_PROVIDER_STEAM
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif
#include "NativeBackend.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
#include <new>
#include <thread>
#include <unordered_set>

#ifndef COOP_PROVIDER_STEAM
#error COOP_PROVIDER_STEAM must select exactly one native backend
#endif
#if COOP_PROVIDER_STEAM != 0 && COOP_PROVIDER_STEAM != 1
#error Invalid COOP_PROVIDER_STEAM
#endif

namespace coopnet {
int32_t sendNativePayload(NativeBackend& backend,uint32_t handle,uint32_t traffic,const void* data,uint32_t size) {
    auto* message=backend.utils->AllocateMessage(int(size)); if(!message) return COOP_NET_INTERNAL;
    memcpy(message->m_pData,data,size); message->m_conn=handle; message->m_idxLane=uint16_t(traffic);
    message->m_nFlags=traffic==GAME_STATE ? k_nSteamNetworkingSend_UnreliableNoNagle : k_nSteamNetworkingSend_ReliableNoNagle;
    // Steam consumes every submitted message without clearing the caller's pointer.
    // The OSS backend's true flag gives the same ownership on success AND failure.
    // Never release or inspect message after this call.
    int64_t result=0; backend.sockets->SendMessages(1,&message,&result,true);
    if(result>0) return COOP_NET_OK;
    if(result==-k_EResultLimitExceeded) return COOP_NET_BACKPRESSURE;
    if(result==-k_EResultInvalidParam) return COOP_NET_INVALID;
    if(result==-k_EResultNoConnection || result==-k_EResultInvalidState) return COOP_NET_CLOSED;
    return COOP_NET_INTERNAL;
}
namespace {
constexpr uint32_t kMode=COOP_PROVIDER_STEAM ? COOP_NET_STEAM : COOP_NET_UDP;
constexpr size_t kEvents=64, kLoans=64;
constexpr uint32_t kMaxBuffer=16u*1024u*1024u, kBorrowedBytes=4u*1024u*1024u;
struct Peer { HSteamNetConnection handle=0; bool admitted=false; bool connected=false; uint64_t steamId=0; };
struct Context {
    NativeBackend backend;
    CoopNetConfig config{};
    std::thread::id owner=std::this_thread::get_id();
    bool running=false;
    HSteamListenSocket listener=0;
    HSteamNetPollGroup group=0;
    int64_t token=0;
    std::array<Peer,COOP_NET_MAX_PEERS> peers{};
    std::array<SteamNetConnectionStatusChangedCallback_t,kEvents> callbacks{};
    size_t callbackHead=0, callbackCount=0;
    bool callbackOverflow=false;
    std::array<CoopNetEvent,kEvents> events{};
    size_t eventHead=0,eventCount=0;
    bool eventOverflow=false;
    std::array<SteamNetworkingMessage_t*,kLoans> loans{};
    uint32_t loanBytes=0;
};
// Callback publication never invokes native methods while holding this lock.
// It fences the main-thread Steam pump against worker stop/destroy.
std::mutex registryMutex;
std::unordered_set<Context*> contexts;
Context* active=nullptr;
uint64_t nextToken=0;
void detail(char* out,uint32_t cap,const char* text) {
    if (out && cap) { size_t n=(std::min)(size_t(cap-1),strlen(text)); memcpy(out,text,n); out[n]=0; }
}
Context* lookup(void* opaque) {
    std::lock_guard<std::mutex> lock(registryMutex);
    auto* c=static_cast<Context*>(opaque);
    return contexts.count(c) && c->owner==std::this_thread::get_id() ? c : nullptr;
}
Peer* peer(Context& c,HSteamNetConnection handle) { for(auto& p:c.peers) if(p.handle==handle && handle) return &p; return nullptr; }
Peer* emptyPeer(Context& c) { for(auto& p:c.peers) if(!p.handle) return &p; return nullptr; }
uint32_t members(const Context& c) { uint32_t n=0; for(const auto& p:c.peers) if(p.admitted) ++n; return n; }
void callback(SteamNetConnectionStatusChangedCallback_t* change) {
    if (!change) return;
    std::lock_guard<std::mutex> lock(registryMutex);
    Context* c=active;
    if (!c || !c->running || change->m_info.m_nUserData!=c->token) return;
    if (c->callbackCount==kEvents) { c->callbackOverflow=true; return; }
    c->callbacks[(c->callbackHead+c->callbackCount)%kEvents]=*change; ++c->callbackCount;
}
bool pinCallbackModule() {
#if COOP_PROVIDER_STEAM
#if defined(_WIN32)
    // Borrowed Steam can dispatch old callbacks after core FreeLibrary.
    // Pin exactly this provider once; never load or retain another Steam API.
    static const bool pinned=[] {
        HMODULE module=nullptr;
        return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(&callback),&module)!=FALSE;
    }();
    return pinned;
#else
    return false;
#endif
#else
    return true;
#endif
}
void queue(Context& c,uint32_t type,HSteamNetConnection handle,uint64_t steamId,int reason,const char* text) {
    if(c.eventCount==kEvents) { c.eventOverflow=true; return; }
    auto& e=c.events[(c.eventHead+c.eventCount)%kEvents]; e={}; e.size=sizeof(e); e.type=type;
    e.generation=c.config.generation; e.connection=handle; e.remoteSteamId=steamId; e.reason=reason; detail(e.detail,sizeof(e.detail),text); ++c.eventCount;
}
bool lanes(Context& c,HSteamNetConnection handle) {
    // Keep control traffic first; share capacity between continuous state and bulk.
    const int priorities[3]={0,1,1}; const uint16 weights[3]={4,2,1};
    return c.backend.sockets->ConfigureConnectionLanes(handle,3,priorities,weights)==k_EResultOK && c.backend.sockets->SetConnectionPollGroup(handle,c.group);
}
bool allowed(Context& c,const SteamNetConnectionInfo_t& info,char* error) {
    if (!authorizeRemote(c.backend,c.config,info,error,COOP_NET_ERROR_CAPACITY)) return false;
#if COOP_PROVIDER_STEAM
    const uint64_t id=info.m_identityRemote.GetSteamID64();
    if (!id) { detail(error,COOP_NET_ERROR_CAPACITY,"Peer has no authenticated Steam identity"); return false; }
    return c.config.host || id==c.config.remoteSteamId;
#else
    return true;
#endif
}
void closePeer(Context& c,Peer& p,int reason,const char* text,bool emit) {
    const auto handle=p.handle; const auto id=p.steamId; p={};
    c.backend.sockets->CloseConnection(handle,reason,text,false);
    if(emit) queue(c,COOP_NET_EVENT_DISCONNECTED,handle,id,reason,text);
}
void process(Context& c,const SteamNetConnectionStatusChangedCallback_t& change) {
    if(change.m_info.m_nUserData!=c.token) return;
    auto* p=peer(c,change.m_hConn);
    const auto state=change.m_info.m_eState;
    char error[COOP_NET_ERROR_CAPACITY]{};
    if(!p && state==k_ESteamNetworkingConnectionState_Connecting && c.config.host && change.m_info.m_hListenSocket==c.listener) {
        if(members(c)>=c.config.maxPeers || !allowed(c,change.m_info,error)) {
            c.backend.sockets->CloseConnection(change.m_hConn,2001,"Peer admission denied",false); return;
        }
#if COOP_PROVIDER_STEAM
        // Forbid duplicate authenticated principals from occupying multiple seats.
        for(const auto& existing:c.peers) if(existing.admitted) {
            SteamNetConnectionInfo_t old{};
            if(c.backend.sockets->GetConnectionInfo(existing.handle,&old) && old.m_identityRemote==change.m_info.m_identityRemote) {
                c.backend.sockets->CloseConnection(change.m_hConn,2001,"Duplicate peer identity",false); return;
            }
        }
#endif
        p=emptyPeer(c);
        if(!p) { c.backend.sockets->CloseConnection(change.m_hConn,2001,"Session guest limit",false); return; }
        p->handle=change.m_hConn; p->steamId=COOP_PROVIDER_STEAM ? change.m_info.m_identityRemote.GetSteamID64() : 0;
        if(!lanes(c,p->handle) || c.backend.sockets->AcceptConnection(p->handle)!=k_EResultOK) {
            closePeer(c,*p,2002,"Native peer admission failed",false); return;
        }
        p->admitted=true;
    }
    if(!p) return;
    if(state==k_ESteamNetworkingConnectionState_Connecting) {
        queue(c,COOP_NET_EVENT_CONNECTING,p->handle,p->steamId,0,"Connecting");
    } else if(state==k_ESteamNetworkingConnectionState_FindingRoute) {
        queue(c,COOP_NET_EVENT_FINDING_ROUTE,p->handle,p->steamId,0,"Finding route");
    } else if(state==k_ESteamNetworkingConnectionState_Connected) {
        if(!allowed(c,change.m_info,error)) { closePeer(c,*p,2001,"Peer security policy rejected",true); return; }
        p->admitted=true; p->connected=true; p->steamId=COOP_PROVIDER_STEAM ? change.m_info.m_identityRemote.GetSteamID64() : 0;
        queue(c,COOP_NET_EVENT_CONNECTED,p->handle,p->steamId,0,
            COOP_PROVIDER_STEAM ? "Authenticated connection established" : "Encrypted direct UDP established; peer unauthenticated");
    } else if(state==k_ESteamNetworkingConnectionState_ClosedByPeer || state==k_ESteamNetworkingConnectionState_ProblemDetectedLocally) {
        // Do not expose native peer-supplied debug text through the public ABI.
        closePeer(c,*p,change.m_info.m_eEndReason,"Native connection closed",true);
    }
}
void stopContext(Context& c) {
    {
        std::lock_guard<std::mutex> lock(registryMutex);
        if(active==&c) active=nullptr;
        c.running=false; c.callbackHead=0; c.callbackCount=0; c.callbackOverflow=false;
    }
    c.eventHead=0; c.eventCount=0; c.eventOverflow=false;
    if(c.backend.sockets) {
        for(auto& p:c.peers) if(p.handle) closePeer(c,p,1000,"Provider session stopped",false);
        if(c.listener) c.backend.sockets->CloseListenSocket(c.listener);
        if(c.group) {
            SteamNetworkingMessage_t* m=nullptr;
            while(c.backend.sockets->ReceiveMessagesOnPollGroup(c.group,&m,1)>0) m->Release();
            c.backend.sockets->DestroyPollGroup(c.group);
        }
    }
    c.listener=0; c.group=0; c.peers={};
    // Borrowed events are immutable and survive stop/restart until releaseEvent.
    shutdownBackend(c.backend); c.token=0;
}
bool validate(const CoopNetConfig* config,char* error,uint32_t cap) {
    if(!config || config->size!=sizeof(CoopNetConfig) || config->mode!=kMode || config->host>1 ||
        config->port>65535 || !config->generation ||
        config->maxPeers>COOP_NET_MAX_PEERS || config->sendBufferBytes>kMaxBuffer || config->bulkQueueBytes>kMaxBuffer ||
        config->connectTimeoutMs>300000 || config->connectedTimeoutMs>300000 ||
        (config->sendBufferBytes && config->sendBufferBytes<1024) ||
        (config->bulkQueueBytes && config->sendBufferBytes && config->bulkQueueBytes>config->sendBufferBytes)) {
        detail(error,cap,"Invalid provider configuration or selected-mode mismatch"); return false;
    }
#if COOP_PROVIDER_STEAM
    if(config->address || (!config->host && !config->remoteSteamId)) {
        detail(error,cap,"Steam mode requires Steam identities, not UDP addresses"); return false;
    }
#else
    if(config->remoteSteamId || !config->port) {
        detail(error,cap,"UDP mode requires a numeric address and port, not Steam IDs"); return false;
    }
    if(config->address) {
        size_t n=strnlen_s(config->address,129); SteamNetworkingIPAddr addr; addr.Clear();
        if(!n || n>128 || !addr.ParseString(config->address) || addr.m_port) { detail(error,cap,"UDP address must be numeric without a port"); return false; }
        const char* colon=strchr(config->address,':');
        if((colon && !strchr(colon+1,':')) || (config->address[0]=='[' && config->address[n-1]!=']')) {
            detail(error,cap,"UDP address must be numeric without a port"); return false;
        }
    } else if(!config->host) { detail(error,cap,"UDP client requires a numeric address"); return false; }
#endif
    return true;
}
void* COOP_NET_CALL create(char* error,uint32_t cap) {
    detail(error,cap,""); Context* c=nullptr;
    try { c=new Context; std::lock_guard<std::mutex> lock(registryMutex); contexts.insert(c); return c; }
    catch(...) { delete c; detail(error,cap,"Provider context allocation failed"); return nullptr; }
}
int32_t COOP_NET_CALL start(void* opaque,const CoopNetConfig* config,char* error,uint32_t cap) try {
    detail(error,cap,""); auto* c=lookup(opaque);
    if(!c || !validate(config,error,cap)) return COOP_NET_INVALID;
    if(c->running) { detail(error,cap,"Provider context is already running"); return COOP_NET_INVALID; }
    if(!pinCallbackModule()) { detail(error,cap,"Cannot preserve provider callback module lifetime"); return COOP_NET_UNAVAILABLE; }
    {
        std::lock_guard<std::mutex> lock(registryMutex);
        if(active) { detail(error,cap,"Provider DLL already has an active native context"); return COOP_NET_UNAVAILABLE; }
        if(nextToken>=uint64_t(INT64_MAX)) { detail(error,cap,"Provider session token exhausted"); return COOP_NET_INTERNAL; }
        c->token=int64_t(++nextToken); active=c;
    }
    try {
        c->config=*config;
        c->config.maxPeers=config->host ? (config->maxPeers ? config->maxPeers : COOP_NET_MAX_PEERS) : 1;
        c->config.connectTimeoutMs=config->connectTimeoutMs ? config->connectTimeoutMs : 10000;
        c->config.connectedTimeoutMs=config->connectedTimeoutMs ? config->connectedTimeoutMs : 10000;
        c->config.sendBufferBytes=config->sendBufferBytes ? config->sendBufferBytes : 2u*1024u*1024u;
        c->config.bulkQueueBytes=config->bulkQueueBytes ? config->bulkQueueBytes : (std::min)(1024u*1024u,c->config.sendBufferBytes);
        const int32_t result=initializeBackend(c->backend,c->config,error,cap);
        c->config.address=nullptr;
        if(result!=COOP_NET_OK) { stopContext(*c); return result; }
        c->group=c->backend.sockets->CreatePollGroup();
        if(!c->group) { detail(error,cap,"Native receive pollgroup allocation failed"); stopContext(*c); return COOP_NET_INTERNAL; }
        SteamNetworkingConfigValue_t options[12]; int count=0;
        options[count++].SetPtr(k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged,reinterpret_cast<void*>(callback));
        options[count++].SetInt64(k_ESteamNetworkingConfig_ConnectionUserData,c->token);
        options[count++].SetInt32(k_ESteamNetworkingConfig_TimeoutInitial,int32_t(c->config.connectTimeoutMs));
        options[count++].SetInt32(k_ESteamNetworkingConfig_TimeoutConnected,int32_t(c->config.connectedTimeoutMs));
        options[count++].SetInt32(k_ESteamNetworkingConfig_SendBufferSize,int32_t(c->config.sendBufferBytes));
        options[count++].SetInt32(k_ESteamNetworkingConfig_RecvBufferSize,int32_t(kBorrowedBytes));
        options[count++].SetInt32(k_ESteamNetworkingConfig_RecvMaxMessageSize,int32_t(COOP_NET_MAX_MESSAGE));
        options[count++].SetInt32(k_ESteamNetworkingConfig_RecvBufferMessages,256);
        options[count++].SetInt32(k_ESteamNetworkingConfig_IP_AllowWithoutAuth,COOP_PROVIDER_STEAM ? 0 : 2);
        options[count++].SetInt32(k_ESteamNetworkingConfig_IPLocalHost_AllowWithoutAuth,0);
        options[count++].SetInt32(k_ESteamNetworkingConfig_Unencrypted,0);
        {
            std::lock_guard<std::mutex> lock(registryMutex); c->running=true;
        }
        HSteamNetConnection outbound=0;
#if COOP_PROVIDER_STEAM
        if(config->host) c->listener=c->backend.sockets->CreateListenSocketP2P(int(config->port),count,options);
        else { SteamNetworkingIdentity remote; remote.Clear(); remote.SetSteamID64(config->remoteSteamId);
            outbound=c->backend.sockets->ConnectP2P(remote,int(config->port),count,options); }
#else
        SteamNetworkingIPAddr address; address.Clear(); if(config->address) address.ParseString(config->address); address.m_port=uint16_t(config->port);
        if(config->host) c->listener=c->backend.sockets->CreateListenSocketIP(address,count,options);
        else outbound=c->backend.sockets->ConnectByIPAddress(address,count,options);
#endif
        if((config->host && !c->listener) || (!config->host && !outbound)) {
            detail(error,cap,"Selected native listen/connect operation failed"); stopContext(*c); return COOP_NET_UNAVAILABLE;
        }
        if(outbound) { c->peers[0].handle=outbound; if(!lanes(*c,outbound)) {
            detail(error,cap,"Native lane configuration failed"); stopContext(*c); return COOP_NET_INTERNAL; } }
        return COOP_NET_OK;
    } catch(...) { stopContext(*c); detail(error,cap,"Provider initialization failed"); return COOP_NET_INTERNAL; }
} catch(...) { detail(error,cap,"Provider API initialization failed"); return COOP_NET_INTERNAL; }
int32_t COOP_NET_CALL poll(void* opaque,CoopNetEvent* output) try {
    auto* c=lookup(opaque); if(!c || !output || output->size!=sizeof(*output)) return -COOP_NET_INVALID;
    *output={}; output->size=sizeof(*output);
    if(!c->running) return 0;
#if !COOP_PROVIDER_STEAM
    c->backend.sockets->RunCallbacks();
#endif
    for(;;) {
        SteamNetConnectionStatusChangedCallback_t change{}; bool have=false, overflow=false;
        { std::lock_guard<std::mutex> lock(registryMutex);
          overflow=c->callbackOverflow; c->callbackOverflow=false;
          if(c->callbackCount) { change=c->callbacks[c->callbackHead]; c->callbackHead=(c->callbackHead+1)%kEvents; --c->callbackCount; have=true; } }
        if(overflow || c->eventOverflow) {
            const auto generation=c->config.generation; stopContext(*c); output->type=COOP_NET_EVENT_ERROR;
            output->generation=generation; output->reason=COOP_NET_INTERNAL;
            detail(output->detail,sizeof(output->detail),"Bounded native callback/event queue overflow; session stopped"); return 1;
        }
        if(!have) break; process(*c,change);
    }
    if(c->eventCount) { *output=c->events[c->eventHead]; c->eventHead=(c->eventHead+1)%kEvents; --c->eventCount; return 1; }
    auto slot=std::find(c->loans.begin(),c->loans.end(),nullptr);
    if(slot==c->loans.end() || c->loanBytes>kBorrowedBytes-COOP_NET_MAX_MESSAGE) return 0;
    for(size_t rejected=0;rejected<kEvents;++rejected) {
        SteamNetworkingMessage_t* m=nullptr; int received=c->backend.sockets->ReceiveMessagesOnPollGroup(c->group,&m,1);
        if(received<0) return -COOP_NET_INTERNAL; if(!received) return 0;
        auto* p=peer(*c,m->m_conn);
        if(!p || !p->admitted || !p->connected || m->m_nConnUserData!=c->token || m->m_cbSize<=0 ||
            uint32_t(m->m_cbSize)>COOP_NET_MAX_MESSAGE || m->m_idxLane>2) { m->Release(); continue; }
        *slot=m; c->loanBytes+=uint32_t(m->m_cbSize);
        output->type=COOP_NET_EVENT_MESSAGE; output->generation=c->config.generation; output->connection=p->handle;
        output->traffic=m->m_idxLane; output->remoteSteamId=p->steamId; output->dataSize=uint32_t(m->m_cbSize);
        output->data=m->m_pData; output->message=m; return 1;
    }
    return 0;
} catch(...) { return -COOP_NET_INTERNAL; }
int32_t COOP_NET_CALL send(void* opaque,uint32_t handle,uint32_t traffic,const void* data,uint32_t size) try {
    auto* c=lookup(opaque); if(!c || !data || !size || size>COOP_NET_MAX_MESSAGE || traffic>SAVE_BULK) return COOP_NET_INVALID;
    if(traffic==GAME_STATE && size>COOP_NET_MAX_STATE_MESSAGE) return COOP_NET_INVALID;
    if(!c->running) return COOP_NET_CLOSED; auto* p=peer(*c,handle);
    if(!p || !p->admitted || !p->connected) return COOP_NET_CLOSED;
    uint64_t total=0,bulk=0;
    for(const auto& other:c->peers) if(other.handle) {
        SteamNetConnectionRealTimeStatus_t status{}; SteamNetConnectionRealTimeLaneStatus_t lane[3]{};
        if(c->backend.sockets->GetConnectionRealTimeStatus(other.handle,&status,3,lane)!=k_EResultOK) {
            // Terminal native connections discard their lanes before the owner
            // drains the callback. Their state must not close another target.
            SteamNetConnectionInfo_t info{};
            if(!c->backend.sockets->GetConnectionInfo(other.handle,&info) ||
               info.m_eState!=k_ESteamNetworkingConnectionState_Connected) {
                if(other.handle==handle) return COOP_NET_CLOSED;
                continue;
            }
            return COOP_NET_INTERNAL;
        }
        if(status.m_eState!=k_ESteamNetworkingConnectionState_Connected) {
            if(other.handle==handle) return COOP_NET_CLOSED;
            continue;
        }
        // State is refreshed every tick. Do not turn it into seconds-old motion
        // while bulk traffic is active; the caller retries with its next capture.
        if(traffic==GAME_STATE && other.handle==handle && lane[GAME_STATE].m_usecQueueTime>=200000)
            return COOP_NET_BACKPRESSURE;
        total+=uint64_t((std::max)(status.m_cbPendingReliable,0))+(std::max)(status.m_cbPendingUnreliable,0)+(std::max)(status.m_cbSentUnackedReliable,0);
        bulk+=uint64_t((std::max)(lane[2].m_cbPendingReliable,0))+(std::max)(lane[2].m_cbSentUnackedReliable,0);
    }
    if(total+size>c->config.sendBufferBytes || (traffic==SAVE_BULK && bulk+size>c->config.bulkQueueBytes)) return COOP_NET_BACKPRESSURE;
    return sendNativePayload(c->backend,handle,traffic,data,size);
} catch(...) { return COOP_NET_INTERNAL; }
void COOP_NET_CALL releaseEvent(void* opaque,CoopNetEvent* event) noexcept {
    auto* c=lookup(opaque); if(!c || !event || event->size!=sizeof(*event)) return;
    auto found=std::find(c->loans.begin(),c->loans.end(),static_cast<SteamNetworkingMessage_t*>(event->message));
    if(event->message && found!=c->loans.end()) { auto* m=*found; *found=nullptr; c->loanBytes-=uint32_t(m->m_cbSize); m->Release(); }
    *event={}; event->size=sizeof(*event);
}
int32_t COOP_NET_CALL disconnect(void* opaque,uint32_t handle,int32_t reason,const char* text) try {
    auto* c=lookup(opaque); if(!c) return COOP_NET_INVALID;
    if((reason!=0 && (reason<1000 || reason>2999)) || (text && strnlen_s(text,COOP_NET_ERROR_CAPACITY)>COOP_NET_ERROR_CAPACITY-1)) return COOP_NET_INVALID;
    if(!c->running) return COOP_NET_CLOSED; auto* p=peer(*c,handle); if(!p) return COOP_NET_CLOSED;
    closePeer(*c,*p,reason,text ? text : "Disconnected",true); return COOP_NET_OK;
} catch(...) { return COOP_NET_INTERNAL; }
void COOP_NET_CALL stop(void* opaque) noexcept { if(auto* c=lookup(opaque)) stopContext(*c); }
void COOP_NET_CALL destroy(void* opaque) noexcept {
    auto* c=lookup(opaque); if(!c) return; stopContext(*c);
    { std::lock_guard<std::mutex> lock(registryMutex); contexts.erase(c); }
    for(auto* message:c->loans) if(message) message->Release(); delete c;
}
int32_t COOP_NET_CALL status(void* opaque,CoopNetStatus* out) try {
    auto* c=lookup(opaque); if(!c || !out || out->size!=sizeof(*out)) return COOP_NET_INVALID;
    *out={}; out->size=sizeof(*out); out->mode=kMode; out->generation=c->config.generation;
    if(!c->running) { detail(out->detail,sizeof(out->detail),"Stopped"); return COOP_NET_OK; }
    out->selfSteamId=c->backend.selfSteamId; out->listening=c->listener ? 1 : 0; out->connections=members(*c);
#if COOP_PROVIDER_STEAM
    SteamNetAuthenticationStatus_t auth{}; out->authentication=int32_t(c->backend.sockets->GetAuthenticationStatus(&auth));
    SteamRelayNetworkStatus_t relay{}; out->relayAvailability=int32_t(c->backend.utils->GetRelayNetworkStatus(&relay));
    if (!c->config.host && c->peers[0].handle && !c->peers[0].connected) {
        SteamNetConnectionInfo_t pending{};
        if (c->backend.sockets->GetConnectionInfo(c->peers[0].handle, &pending)) {
            std::snprintf(out->detail, sizeof(out->detail),
                "Steam pending state=%d end=%d generationMatch=%d",
                int(pending.m_eState), pending.m_eEndReason, pending.m_nUserData == c->token);
            return COOP_NET_OK;
        }
    }
    detail(out->detail,sizeof(out->detail),"Selected native transport active"); return COOP_NET_OK;
#else
    // Direct IP transport has encryption, not an authenticated principal.
    out->authentication=int32_t(k_ESteamNetworkingAvailability_Unknown);
    detail(out->detail,sizeof(out->detail),"Open UDP active; peers are unauthenticated"); return COOP_NET_OK;
#endif
} catch(...) { return COOP_NET_INTERNAL; }
int32_t COOP_NET_CALL peerStats(void* opaque,uint32_t handle,CoopNetPeerStats* out) try {
    auto* c=lookup(opaque); if(!c || !out || out->size!=sizeof(*out)) return COOP_NET_INVALID;
    if(!c->running) return COOP_NET_CLOSED; auto* p=peer(*c,handle); if(!p || !p->admitted) return COOP_NET_CLOSED;
    SteamNetConnectionRealTimeStatus_t status{}; SteamNetConnectionRealTimeLaneStatus_t lanes[3]{};
    if(c->backend.sockets->GetConnectionRealTimeStatus(handle,&status,3,lanes)!=k_EResultOK) return COOP_NET_CLOSED;
    *out={}; out->size=sizeof(*out); out->connection=handle; out->rttMs=status.m_nPing; out->state=uint32_t(status.m_eState);
    out->pendingReliableBytes=uint32_t((std::max)(lanes[0].m_cbPendingReliable,0));
    out->pendingStateBytes=uint32_t((std::max)(lanes[1].m_cbPendingUnreliable,0));
    out->pendingBulkBytes=uint32_t((std::max)(lanes[2].m_cbPendingReliable,0));
    out->unackedReliableBytes=uint32_t((std::max)(status.m_cbSentUnackedReliable,0));
    out->inboundBytesPerSec=status.m_flInBytesPerSec; out->outboundBytesPerSec=status.m_flOutBytesPerSec;
    out->localQuality=status.m_flConnectionQualityLocal; out->remoteQuality=status.m_flConnectionQualityRemote; return COOP_NET_OK;
} catch(...) { return COOP_NET_INTERNAL; }
int32_t COOP_NET_CALL pumpMainThread() try { return pumpBackendMainThread(); } catch(...) { return COOP_NET_INTERNAL; }
}
}
#if defined(_WIN32)
#define COOP_EXPORT __declspec(dllexport)
#else
#define COOP_EXPORT __attribute__((visibility("default")))
#endif
extern "C" COOP_EXPORT int32_t COOP_NET_CALL KenshiCoopNet_GetApi(uint32_t version,uint32_t size,CoopNetApi* out) {
    if(!out || version!=COOP_NET_ABI_VERSION || size!=sizeof(CoopNetApi)) return COOP_NET_INVALID;
    const CoopNetApi api={COOP_NET_ABI_VERSION,sizeof(CoopNetApi),coopnet::create,coopnet::start,coopnet::poll,
        coopnet::send,coopnet::releaseEvent,coopnet::disconnect,coopnet::stop,coopnet::destroy,coopnet::status,
        coopnet::peerStats,coopnet::pumpMainThread};
    *out=api; return COOP_NET_OK;
}
