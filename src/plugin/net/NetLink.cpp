#define _CRT_SECURE_NO_WARNINGS 1 // _snprintf is fine here; silence VC10 C4996

#include "NetLink.h"
#include "RuntimeImages.h"
#include "../CoopLog.h"
#include "../core/PlayerNick.h"
#include "../core/InventorySnapshotAssembler.h"

#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cstddef>

namespace coop {

namespace {
const int        TICK_MS        = 1;  // max worker idle wait; not the entity-send cadence
const DWORD      ENTITY_SEND_MS = 10; // target 100 Hz for owned-entity snapshots
const u8 CH_RELIABLE=GAME_RELIABLE;
const u8 CH_UNRELIABLE=GAME_STATE;
const u8 CH_BULK=SAVE_BULK;
const DWORD NATIVE_CONNECT_TIMEOUT_MS=10000;
const DWORD NATIVE_RETRY_DELAY_MS=2000;
const int NATIVE_REJECT_IDENTITY=2001;
const int NATIVE_REJECT_PROTOCOL=2002;
const int NATIVE_REJECT_TIMEOUT=2003;
class NativeEventScope {
public:
    NativeEventScope(const CoopNetApi& api,void* context,CoopNetEvent& event)
        :api_(api),context_(context),event_(&event) {}
    ~NativeEventScope() {release();}
    void release() {if(event_){api_.releaseEvent(context_,event_);event_=0;}}
private:
    const CoopNetApi& api_;
    void* context_;
    CoopNetEvent* event_;
    NativeEventScope(const NativeEventScope&);
    NativeEventScope& operator=(const NativeEventScope&);
};

// Net-thread diagnostics. OutputDebugStringA is thread-safe, and CoopLog guards
// its FILE* with a lock, so both are safe to call off the main thread.
void netLog(const char* msg) {
    OutputDebugStringA("[KenshiCoop/net] ");
    OutputDebugStringA(msg ? msg : "");
    OutputDebugStringA("\n");
    char buf[256];
    _snprintf(buf, sizeof(buf) - 1, "[net] %s", msg ? msg : "");
    buf[sizeof(buf) - 1] = '\0';
    coop::logLine(buf);
}
void netErr(const char* msg) {
    OutputDebugStringA("[KenshiCoop/net] ERROR: ");
    OutputDebugStringA(msg ? msg : "");
    OutputDebugStringA("\n");
    char buf[256];
    _snprintf(buf, sizeof(buf) - 1, "[net] %s", msg ? msg : "");
    buf[sizeof(buf) - 1] = '\0';
    coop::logErrLine(buf);
}

// Monotonic ms clock for snapshot capture stamps and the 10 ms send cadence.
// QPC, not GetTickCount: its ~15 ms granularity would quantize both the send
// schedule and the snapshot spacing reconstructed by the receiver.
u32 monoMs() {
    static LARGE_INTEGER freq = { 0 };
    if (freq.QuadPart == 0) {
        if (!QueryPerformanceFrequency(&freq) || freq.QuadPart == 0)
            return (u32)GetTickCount();
    }
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (u32)(((unsigned __int64)c.QuadPart * 1000ULL) /
                 (unsigned __int64)freq.QuadPart);
}

} // namespace

namespace {
u64 freshToken() {
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    static volatile LONG serial=0;
    u64 v=(u64)t.QuadPart ^ ((u64)GetCurrentProcessId()<<32) ^ (u64)(u32)InterlockedIncrement(&serial);
    return v?v:1;
}
}
NetStartOptions::NetStartOptions():isHost(true),transport(NET_TRANSPORT_UDP),port(27800),remoteSteamId(0) {}
NetLink::NetLink():isHost_(false),steamMode_(false),port_(0),inbound_(0),providerModule_(0),context_(0),mainThreadId_(0),
    sessionGeneration_(0),providerGeneration_(0),incarnation_(0),sessionToken_(0),worldToken_(0),latestLoadId_(0),latestLoadWorld_(0),localReady_(false),serverConnection_(0),
    outOwner_(0),outStampMs_(0),outEpoch_(0),captureSequence_(0),outSequence_(0),haveOut_(false),publicationBytes_(0),thread_(0),running_(0),stopFlag_(0),myId_(0),sendEpoch_(0),
    simDelayMs_(0),simJitterMs_(0),simLossPct_(0),receivedBytes_(0),receivedPackets_(0),sentBytes_(0),sentPackets_(0),lastStatsTick_(0),inviteSerial_(0) {
    InitializeCriticalSection(&outCs_); InitializeCriticalSection(&nameCs_); InitializeCriticalSection(&debugCs_); InitializeCriticalSection(&statusCs_); InitializeCriticalSection(&pumpCs_);
    memset(ownerIncarnation_,0,sizeof(ownerIncarnation_)); memset(&api_,0,sizeof(api_)); memset(&status_,0,sizeof(status_)); memset(&debugStats_,0,sizeof(debugStats_));
    memset(localName_,0,sizeof(localName_)); memset(peerName_,0,sizeof(peerName_)); memset(invite_,0,sizeof(invite_)); memset(pendingBytes_,0,sizeof(pendingBytes_));
    status_.phase=NET_IDLE; status_.requested=status_.active=NET_TRANSPORT_NONE; status_.inviteResult=-1;
}
NetLink::~NetLink() {
    stop(); DeleteCriticalSection(&pumpCs_); DeleteCriticalSection(&statusCs_); DeleteCriticalSection(&debugCs_); DeleteCriticalSection(&nameCs_); DeleteCriticalSection(&outCs_);
}
bool NetLink::start(const NetStartOptions& o,Inbound* inbound) {
    stop(); options_=o; isHost_=o.isHost; steamMode_=o.transport==NET_TRANSPORT_STEAM; port_=o.port; ip_=o.address; inbound_=inbound;
    mainThreadId_=GetCurrentThreadId(); sessionGeneration_=freshToken(); providerGeneration_=0; incarnation_=freshToken();
    sessionToken_=isHost_?freshToken():0; worldToken_=isHost_?freshToken():0; localReady_=isHost_;
    latestLoadId_=0; latestLoadWorld_=0; InterlockedExchange(&myId_,0);
    if ((o.transport!=NET_TRANSPORT_STEAM && o.transport!=NET_TRANSPORT_UDP) || o.port<1 || o.port>65535) {
        setError(NET_ERR_PROVIDER_START,COOP_NET_INVALID,"invalid native start options"); setPhase(NET_FAILED); return false;
    }
    return launchThread();
}
bool NetLink::launchThread() {
    clearDebugStats(); NativeLock nativeLock91_statusCs_(&statusCs_); memset(&status_,0,sizeof(status_));
    status_.phase=NET_STARTING; status_.host=isHost_; status_.requested=options_.transport; status_.active=NET_TRANSPORT_NONE;
    status_.sessionGeneration=sessionGeneration_; status_.inviteResult=-1; status_.phaseSinceTick=GetTickCount(); nativeLock91_statusCs_.unlock();
    InterlockedExchange(&stopFlag_,0); InterlockedExchange(&running_,1);
    thread_=CreateThread(0,0,&NetLink::threadEntry,this,0,0);
    if (!thread_) { InterlockedExchange(&running_,0); setError(NET_ERR_THREAD,GetLastError(),"native worker launch failed"); setPhase(NET_FAILED); return false; }
    return true;
}
void NetLink::stop() {
    if(thread_) { InterlockedExchange(&stopFlag_,1); WaitForSingleObject(thread_,INFINITE); CloseHandle(thread_); thread_=0; }
    InterlockedExchange(&running_,0);
    NativeLock nativeLock102_outCs_(&outCs_); publications_.clear(); publicationBytes_=0; commands_.clear(); out_.clear(); haveOut_=false;
    peers_.clear();serverConnection_=0;stateSeen_.clear();delayed_.clear();memset(ownerIncarnation_,0,sizeof(ownerIncarnation_));
    for(unsigned lane=0;lane<3;++lane){pending_[lane].clear();pendingBytes_[lane]=0;}
    receivedBytes_=receivedPackets_=sentBytes_=sentPackets_=0;lastStatsTick_=0;
    SecureZeroMemory(invite_,sizeof(invite_)); options_.udpJoinInvite.assign(options_.udpJoinInvite.size(),'\0'); options_.udpJoinInvite.clear(); nativeLock102_outCs_.unlock();
    NativeLock nativeLock107_nameCs_(&nameCs_); memset(peerName_,0,sizeof(peerName_)); nativeLock107_nameCs_.unlock();
    clearDebugStats(); NativeLock nativeLock108_statusCs_(&statusCs_); status_.phase=NET_IDLE; status_.active=NET_TRANSPORT_NONE; status_.memberMask=0;
    status_.pendingHandshakes=0; status_.inviteResult=-1; status_.inviteRequestId=0; status_.phaseSinceTick=GetTickCount(); nativeLock108_statusCs_.unlock();
}
bool NetLink::pumpMainThread() {
    if(GetCurrentThreadId()!=mainThreadId_ || !steamMode_)return false;
    NativeLock nativeLock113_pumpCs_(&pumpCs_);
    const bool pumped=providerModule_ && api_.pumpMainThread && api_.pumpMainThread()==COOP_NET_OK;
    nativeLock113_pumpCs_.unlock();return pumped;
}
bool NetLink::loadProvider() {
    updater::RuntimeImageLoadGuard imageLoad;
    if (!imageLoad.allowed()) {
        setError(NET_ERR_PROVIDER_LOAD,ERROR_REVISION_MISMATCH,
                 "runtime updated; restart Kenshi before loading network companions");
        return false;
    }
    setError(NET_ERR_PROVIDER_LOAD,0,"native companion module directory could not be resolved");
    HMODULE own=0; wchar_t path[32768];
    if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,(LPCWSTR)&NetLink::threadEntry,&own))return false;
    DWORD n=GetModuleFileNameW(own,path,32768); if(!n || n>=32768)return false;
    wchar_t* slash=wcsrchr(path,L'\\'); if(!slash)return false;
    const wchar_t* name=steamMode_?L"KenshiCoopNetSteam.dll":L"KenshiCoopNetUDP.dll";
    if((slash-path)+1+wcslen(name)>=32768)return false; wcscpy(slash+1,name);
    // Restrict dependency lookup to this module directory and Windows system32.
    HMODULE module=LoadLibraryExW(path,0,0x00000100|0x00000800);
    if(!module) { setError(NET_ERR_PROVIDER_LOAD,GetLastError(),"selected native companion could not be loaded from module directory"); return false; }
    CoopNetGetApiFn getter=(CoopNetGetApiFn)GetProcAddress(module,"KenshiCoopNet_GetApi"); CoopNetApi table; memset(&table,0,sizeof(table));
    if(!getter || getter(COOP_NET_ABI_VERSION,sizeof(table),&table)!=COOP_NET_OK || table.version!=COOP_NET_ABI_VERSION || table.size!=sizeof(table) ||
       !table.create || !table.start || !table.poll || !table.send || !table.releaseEvent || !table.disconnect || !table.stop || !table.destroy || !table.status || !table.peerStats || !table.makeInvite || !table.prepareHost || !table.pumpMainThread) {
        FreeLibrary(module); setError(NET_ERR_PROVIDER_ABI,0,"selected native companion ABI mismatch"); return false;
    }
    NativeLock nativeLock131_pumpCs_(&pumpCs_);providerModule_=module;api_=table;nativeLock131_pumpCs_.unlock();setError(NET_ERR_NONE,0,0);return true;
}
bool NetLink::startProvider() {
    char error[COOP_NET_ERROR_CAPACITY]={0};
    if(!steamMode_ && isHost_) {
        int provision=api_.prepareHost(options_.udpHostProfilePath.c_str(),error,sizeof(error));
        SecureZeroMemory(error,sizeof(error));
        if(provision!=COOP_NET_OK) {setError(provision==COOP_NET_AUTH_FAILED?NET_ERR_PROVIDER_AUTH:NET_ERR_PROVIDER_START,provision,"private UDP host credential could not be prepared");return false;}
    }
    if(!context_)context_=api_.create(error,sizeof(error));
    if(!context_) { setError(NET_ERR_PROVIDER_START,0,"native context creation failed");return false; }
    CoopNetConfig c; memset(&c,0,sizeof(c)); c.size=sizeof(c); c.mode=options_.transport; c.host=isHost_; c.port=port_;
    c.generation=++providerGeneration_;
    if(steamMode_) {
        c.remoteSteamId=isHost_?0:options_.remoteSteamId;
    }
    c.connectTimeoutMs=NATIVE_CONNECT_TIMEOUT_MS; c.connectedTimeoutMs=15000; c.sendBufferBytes=2*1024*1024; c.bulkQueueBytes=1024*1024; c.maxPeers=MAX_JOINS;
    if(!steamMode_) {
        c.address=isHost_?0:ip_.c_str();
        c.profilePath=isHost_?options_.udpHostProfilePath.c_str():0;
        c.joinInvite=isHost_?0:options_.udpJoinInvite.c_str();
    }
    if(steamMode_)netLog(isHost_?"Steam native HOST start":"Steam native JOIN start");
    NativeLock startPumpLock(&pumpCs_);
    int result=api_.start(context_,&c,error,sizeof(error));startPumpLock.unlock();
    if(steamMode_) {
        char diagnostic[COOP_NET_ERROR_CAPACITY+64];
        _snprintf(diagnostic,sizeof(diagnostic)-1,"Steam native start result=%d detail=%s",result,error[0]?error:"none");
        diagnostic[sizeof(diagnostic)-1]='\0';
        netLog(diagnostic);
    }
    if(result!=COOP_NET_OK) {
        setError(result==COOP_NET_AUTH_FAILED?NET_ERR_PROVIDER_AUTH:NET_ERR_PROVIDER_START,result,
                 steamMode_ && error[0]?error:"selected native provider start failed");
        SecureZeroMemory(error,sizeof(error));return false;
    }
    SecureZeroMemory(error,sizeof(error));
    setActiveTransport(options_.transport); setPhase(isHost_?NET_LISTENING:NET_CONNECTING); if(isHost_)setMembers(1u); return true;
}
void NetLink::closeProvider() {
    // This lock fences in-flight main Steam pumps before context/backend/module teardown.
    NativeLock nativeLock161_pumpCs_(&pumpCs_);
    if(context_) { api_.stop(context_); api_.destroy(context_); context_=0; }
    if(providerModule_) { FreeLibrary(providerModule_); providerModule_=0; }
    memset(&api_,0,sizeof(api_)); nativeLock161_pumpCs_.unlock();
}
void NetLink::failQueue() {setError(NET_ERR_QUEUE_FULL,0,"native gameplay queue exhausted; session failed instead of silently dropping reliable events");InterlockedExchange(&stopFlag_,1);setPhase(NET_FAILED);}
bool NetLink::queuePayload(const void* data,unsigned size) {
    if(!data || !size || size>COOP_NET_MAX_MESSAGE-sizeof(NativeFrame))return false;
    NativeLock nativeLock169_outCs_(&outCs_);
    if(publications_.size()>=4096 || size>4*1024*1024-publicationBytes_) { nativeLock169_outCs_.unlock(); failQueue();return false; }
    publications_.push_back(Publication());Publication& p=publications_.back();p.epoch=(u32)sendEpoch_;p.stampMs=monoMs();p.sequence=++captureSequence_;p.bytes.assign((const u8*)data,(const u8*)data+size);publicationBytes_+=size;
    nativeLock169_outCs_.unlock(); return true;
}
bool NetLink::queuePayload(std::vector<u8>& bytes) {
    if(bytes.empty() || bytes.size()>COOP_NET_MAX_MESSAGE-sizeof(NativeFrame))return false;
    NativeLock nativeLock176_outCs_(&outCs_);
    if(publications_.size()>=4096 || bytes.size()>4*1024*1024-publicationBytes_) {nativeLock176_outCs_.unlock();failQueue();return false;}
    publications_.push_back(Publication());
    Publication& p=publications_.back();p.epoch=(u32)sendEpoch_;p.stampMs=monoMs();p.sequence=++captureSequence_;
    p.bytes.swap(bytes);publicationBytes_+=(unsigned)p.bytes.size();
    nativeLock176_outCs_.unlock();return true;
}
void NetLink::setOwnedEntities(u32 owner,const EntityState* arr,unsigned count) {
    NativeLock nativeLock184_outCs_(&outCs_); if(count>8192) { nativeLock184_outCs_.unlock();failQueue();return; }
    outOwner_=owner;if(arr && count)out_.assign(arr,arr+count);else out_.clear();outStampMs_=monoMs();outEpoch_=(u32)sendEpoch_;outSequence_=++captureSequence_;haveOut_=true;nativeLock184_outCs_.unlock();
}
void NetLink::clearWorldPublication() {
    out_.clear();haveOut_=false;
    for(std::deque<Publication>::iterator i=publications_.begin();i!=publications_.end();) {
        if(!nativeSessionType(i->bytes[0]) || i->bytes[0]==PKT_PROCEDURAL ||
           i->bytes[0]==PKT_ENVIRONMENT_ITEMS) { publicationBytes_-=(unsigned)i->bytes.size(); i=publications_.erase(i); }else ++i;
    }
}
void NetLink::bumpSessionEpoch(bool rotateHostWorld) {
    NativeLock nativeLock194_outCs_(&outCs_); InterlockedIncrement(&sendEpoch_); clearWorldPublication();
    if(isHost_ && running_ && rotateHostWorld) { worldToken_=freshToken(); localReady_=true; }
    else if(!isHost_)localReady_=false;
    nativeLock194_outCs_.unlock();
}
bool NetLink::canQueueSaveBytes(unsigned bytes) const {
    NativeLock nativeLock200_outCs_((CRITICAL_SECTION*)&outCs_); const bool ok=bytes<128*1024 && publicationBytes_+pendingBytes_[SAVE_BULK]<256*1024;
    nativeLock200_outCs_.unlock(); NetDebugStats s; copyDebugStats(&s); return ok && s.pendingBulkBytes<512*1024;
}
bool NetLink::queueMakeUdpInvite(u64 generation,u32 request) {
    NativeLock nativeLock210_outCs_(&outCs_);
    if(!running_ || !isHost_ || steamMode_ || generation!=sessionGeneration_ || !request || commands_.size()>=32) { nativeLock210_outCs_.unlock();return false; }
    Command c;c.generation=generation;c.requestId=request;c.serial=++inviteSerial_;commands_.push_back(c);SecureZeroMemory(invite_,sizeof(invite_));
    NativeLock nativeLock213_statusCs_(&statusCs_);status_.inviteRequestId=request;status_.inviteResult=-1;nativeLock213_statusCs_.unlock();nativeLock210_outCs_.unlock();return true;
}
bool NetLink::copyUdpInvite(u64 generation,u32 request,char* out,unsigned cap) const {
    if(!out || !cap)return false;out[0]=0;
    NativeLock nativeLock217_outCs_((CRITICAL_SECTION*)&outCs_); NetStatus s;copyStatus(&s);
    bool ok=generation==sessionGeneration_ && request==s.inviteRequestId && s.inviteResult==COOP_NET_OK && invite_[0] && strlen(invite_)<cap;
    if(ok)strcpy(out,invite_);nativeLock217_outCs_.unlock();return ok;
}
void NetLink::processCommands() {
    std::deque<Command> q;NativeLock nativeLock222_outCs_(&outCs_);q.swap(commands_);nativeLock222_outCs_.unlock();
    for(size_t i=0;i<q.size();++i) {
        const Command& c=q[i];if(c.generation!=sessionGeneration_)continue;
        char secret[COOP_NET_INVITE_CAPACITY]={0},error[COOP_NET_ERROR_CAPACITY]={0};
        int r=api_.makeInvite(options_.udpHostProfilePath.c_str(),secret,sizeof(secret),error,sizeof(error));
        NativeLock nativeLock228_outCs_(&outCs_);NativeLock nativeLock228_statusCs_(&statusCs_);
        if(c.serial==inviteSerial_ && status_.inviteRequestId==c.requestId && status_.sessionGeneration==c.generation) {status_.inviteResult=r;if(r==COOP_NET_OK)memcpy(invite_,secret,sizeof(invite_));}
        nativeLock228_statusCs_.unlock();nativeLock228_outCs_.unlock();SecureZeroMemory(secret,sizeof(secret));SecureZeroMemory(error,sizeof(error));
    }
}
void NetLink::setLocalName(const char* name) {
    NativeLock nativeLock242_nameCs_(&nameCs_);
    memset(localName_, 0, sizeof(localName_));
    if (name && name[0]) {
        unsigned n = 0;
        while (name[n] && n < HELLO_NAME_MAX) {
            localName_[n] = name[n];
            ++n;
        }
    }
    nativeLock242_nameCs_.unlock();
}

bool NetLink::copyPeerName(u32 id, char* out, unsigned cap) const {
    if (!out || cap == 0) return false;
    out[0] = '\0';
    if (id >= MAX_PLAYERS) return false;
    NativeLock nativeLock258_nameCs_(&nameCs_);
    bool have = peerName_[id][0] != '\0';
    if (have) {
        unsigned n = 0;
        while (peerName_[id][n] && n + 1 < cap && n < HELLO_NAME_MAX) {
            out[n] = peerName_[id][n];
            ++n;
        }
        out[n] = '\0';
    }
    nativeLock258_nameCs_.unlock();
    return have;
}

void NetLink::copyDebugStats(NetDebugStats* out) const {
    if (!out) return;
    NativeLock nativeLock274_debugCs_(&debugCs_);
    *out = debugStats_;
    nativeLock274_debugCs_.unlock();
}

void NetLink::clearDebugStats() {
    NativeLock nativeLock280_debugCs_(&debugCs_);
    memset(&debugStats_, 0, sizeof(debugStats_));
    nativeLock280_debugCs_.unlock();
}

void NetLink::copyStatus(NetStatus* out) const {
    if (!out) return;
    NativeLock nativeLock287_statusCs_(&statusCs_);
    *out = status_;
    nativeLock287_statusCs_.unlock();
}

void NetLink::setPhase(NetPhase p) {
    NativeLock nativeLock293_statusCs_(&statusCs_);
    if (status_.phase != p) {
        status_.phase = p;
        status_.phaseSinceTick = GetTickCount();
    }
    nativeLock293_statusCs_.unlock();
}

void NetLink::setActiveTransport(NetTransport t) {
    NativeLock nativeLock302_statusCs_(&statusCs_);
    status_.active = t;
    nativeLock302_statusCs_.unlock();
}

void NetLink::setError(NetError e, u32 arg, const char* raw) {
    NativeLock nativeLock308_statusCs_(&statusCs_);
    status_.error    = e;
    status_.errorArg = arg;
    memset(status_.errorRaw, 0, sizeof(status_.errorRaw));
    if (raw) strncpy(status_.errorRaw, raw, sizeof(status_.errorRaw) - 1);
    nativeLock308_statusCs_.unlock();
}

void NetLink::setMemberBit(u32 id, bool on) {
    if (id >= MAX_PLAYERS) return;
    NativeLock nativeLock318_statusCs_(&statusCs_);
    if (on) status_.memberMask |= (1u << id);
    else    status_.memberMask &= ~(1u << id);
    nativeLock318_statusCs_.unlock();
}

void NetLink::setMembers(u32 mask) {
    NativeLock nativeLock325_statusCs_(&statusCs_);
    status_.memberMask = mask;
    nativeLock325_statusCs_.unlock();
}

void NetLink::setHandshakeCounts(u32 connectAttempts, u32 pendingHandshakes) {
    NativeLock nativeLock331_statusCs_(&statusCs_);
    status_.connectAttempts   = connectAttempts;
    status_.pendingHandshakes = pendingHandshakes;
    nativeLock331_statusCs_.unlock();
}

void NetLink::queueEvent(const EventPacket& ev) { queuePayload(&ev,sizeof(ev)); }
void NetLink::queueWorldDrop(const WorldDropPacket& pkt) { queuePayload(&pkt,sizeof(pkt)); }
void NetLink::queueMedical(const MedicalPacket& pkt) { queuePayload(&pkt,sizeof(pkt)); }
void NetLink::queueTreatment(const TreatmentPacket& pkt) { queuePayload(&pkt,sizeof(pkt)); }
void NetLink::queueCombatHit(const CombatHitPacket& pkt) { queuePayload(&pkt,sizeof(pkt)); }
void NetLink::queueSpeed(const SpeedPacket& pkt) { queuePayload(&pkt,sizeof(pkt)); }
void NetLink::queueStats(const StatsPacket& pkt) { queuePayload(&pkt,sizeof(pkt)); }
void NetLink::queueMoney(const MoneyPacket& pkt) { queuePayload(&pkt,sizeof(pkt)); }
void NetLink::queueMoneyDelta(const MoneyDeltaPacket& pkt) { queuePayload(&pkt,sizeof(pkt)); }
void NetLink::queueFaction(const FactionPacket& pkt) { queuePayload(&pkt,sizeof(pkt)); }
void NetLink::queueTime(const TimePacket& pkt) { queuePayload(&pkt,sizeof(pkt)); }
void NetLink::queueDoor(const DoorPacket& pkt) { queuePayload(&pkt,sizeof(pkt)); }
void NetLink::queueProd(const ProdPacket& pkt) { queuePayload(&pkt,sizeof(pkt)); }
void NetLink::queueResearch(const ResearchPacket& pkt) { queuePayload(&pkt,sizeof(pkt)); }
void NetLink::queueDeed(const DeedPacket& pkt) { queuePayload(&pkt,sizeof(pkt)); }
void NetLink::queueFixture(const FixturePacket& pkt) { queuePayload(&pkt,sizeof(pkt)); }
void NetLink::queueBuildPlace(const BuildPlacePacket& pkt) { queuePayload(&pkt,sizeof(pkt)); }
void NetLink::queueBuildState(const BuildStatePacket& pkt) { queuePayload(&pkt,sizeof(pkt)); }
void NetLink::queueBuildDoor(const BuildDoorPacket& pkt) { queuePayload(&pkt,sizeof(pkt)); }
void NetLink::queueBuildRemove(const BuildRemovePacket& pkt) { queuePayload(&pkt,sizeof(pkt)); }
u64 NetLink::currentWorldToken() const {
    NativeLock lock((CRITICAL_SECTION*)&outCs_);
    return running_ ? worldToken_ : 0;
}
bool NetLink::currentProcedural(const ProceduralPacket& p,const Peer* peer) const {
    if(!p.worldToken || p.worldToken!=worldToken_ || !p.loadId)return false;
    if(isHost_)return !peer || (p.loadId==peer->expectedLoadId && p.worldToken==peer->expectedWorld);
    return p.loadId==latestLoadId_ && p.worldToken==latestLoadWorld_;
}
bool NetLink::queueProcedural(const ProceduralPacket& pkt) {
    NativeLock lock(&outCs_);
    const u32 origin=isHost_?0:(u32)myId_;
    if(!running_ || pkt.type!=PKT_PROCEDURAL || !currentProcedural(pkt) ||
       !nativePacketValid(&pkt,sizeof(pkt),GAME_RELIABLE,!isHost_,origin))return false;
    return queuePayload(&pkt,sizeof(pkt));
}
bool NetLink::currentEnvironment(const EnvironmentItemPacket& p,const Peer* peer) const {
    if (!p.worldToken || p.worldToken!=worldToken_ || !p.loadId) return false;
    if (isHost_) {
        if (!peer) return true;
        return p.loadId==peer->expectedLoadId && p.worldToken==peer->expectedWorld &&
            (p.ownerId==0 ? p.targetId==peer->playerId :
             p.ownerId==peer->playerId && p.targetId==0);
    }
    return p.loadId==latestLoadId_ && p.worldToken==latestLoadWorld_ &&
        (p.ownerId==0 ? p.targetId==(u32)myId_ :
         p.ownerId==(u32)myId_ && p.targetId==0);
}
bool NetLink::queueEnvironment(const EnvironmentItemPacket& pkt) {
    NativeLock lock(&outCs_);
    const u32 origin=isHost_?0:(u32)myId_;
    if (!running_ || pkt.type!=PKT_ENVIRONMENT_ITEMS || !currentEnvironment(pkt) ||
        !nativePacketValid(&pkt,sizeof(pkt),GAME_RELIABLE,!isHost_,origin)) return false;
    return queuePayload(&pkt,sizeof(pkt));
}
void NetLink::queueStealth(const StealthPacket& pkt) { queuePayload(&pkt,sizeof(pkt)); }
void NetLink::queueCamHint(const CamHintPacket& pkt) { queuePayload(&pkt,sizeof(pkt)); }
void NetLink::queueCellClaim(const CellClaimPacket& pkt) { queuePayload(&pkt,sizeof(pkt)); }
void NetLink::queueSpawnReq(const SpawnReqPacket& pkt) { queuePayload(&pkt,sizeof(pkt)); }
void NetLink::queueSpawnInfo(const SpawnInfoPacket& pkt) { queuePayload(&pkt,sizeof(pkt)); }
void NetLink::queueWorldPickup(const WorldPickupPacket& pkt) { queuePayload(&pkt,sizeof(pkt)); }
void NetLink::queueSaveReq(const SaveReqPacket& pkt) { queuePayload(&pkt,sizeof(pkt)); }
void NetLink::queueSaveBegin(const SaveBeginPacket& pkt) { queuePayload(&pkt,sizeof(pkt)); }
void NetLink::queueSaveAck(const SaveAckPacket& pkt) { queuePayload(&pkt,sizeof(pkt)); }
void NetLink::queueLoadReq(const LoadReqPacket& pkt) { queuePayload(&pkt,sizeof(pkt)); }
void NetLink::queueLoadNack(const LoadNackPacket& pkt) { queuePayload(&pkt,sizeof(pkt)); }

namespace {
template<class H,class T> std::vector<u8> payloadTail(const H& h,const T* rows,unsigned count) {
    std::vector<u8> v(sizeof(H)+count*sizeof(T));memcpy(&v[0],&h,sizeof(H));if(count)memcpy(&v[sizeof(H)],rows,count*sizeof(T));return v;
}
}
void NetLink::queueInvSnapshot(u32 owner,u8 kind,const u32 key[5],const InvItemEntry* items,unsigned count,u8 flags,u32 revision) {
    InvSnapshotHeader h;memset(&h,0,sizeof(h));h.type=PKT_INV_SNAPSHOT;h.ownerId=owner;h.keyKind=kind;
    h.flags=flags & ~INV_FLAG_COMPLETE;h.revision=revision;
    h.cType=key[0];h.cContainer=key[1];h.cContainerSerial=key[2];h.cIndex=key[3];h.cSerial=key[4];
    if(count && !items){failQueue();return;}
    if(flags & INV_FLAG_CHECKPOINT_ACK) {
        if(count || flags!=INV_FLAG_CHECKPOINT_ACK || !revision){failQueue();return;}
        queuePayload(&h,sizeof(h));return;
    }
    if(count>INV_SNAPSHOT_ITEMS_MAX){count=INV_SNAPSHOT_ITEMS_MAX;h.flags|=INV_FLAG_TRUNCATED;}
    if(!nativeInventoryValid(items,count)){failQueue();return;}
    h.totalCount=count;h.digest=inventorySnapshotDigest(items,count);
    NativeLock batch(&outCs_);
    do {
        const unsigned remaining=count-h.offset;
        h.count=(u16)(remaining>INV_ITEMS_MAX?INV_ITEMS_MAX:remaining);
        if(h.offset+h.count==count)h.flags|=INV_FLAG_COMPLETE;
        std::vector<u8> v=payloadTail(h,h.count?items+h.offset:0,h.count);queuePayload(v);
        h.offset+=h.count;
    } while(h.offset<count);
}
void NetLink::queueInvXfer(const InvXferPacket& pkt,std::vector<InvItemEntry>& contents) {
    if(contents.size()>INV_ITEMS_MAX){failQueue();return;}InvXferPacket p=pkt;p.contentsCount=(u8)contents.size();
    std::vector<u8> v=payloadTail(p,contents.empty()?0:&contents[0],(unsigned)contents.size());queuePayload(v);contents.clear();
}
void NetLink::queueInvXferAck(const InvXferAckPacket& pkt,const std::vector<InvItemEntry>& contents,const InvSnapshotHeader& snapshot,const InvItemEntry* items) {
    if(contents.size()>INV_ITEMS_MAX){failQueue();return;}NativeLock nativeLock385_outCs_(&outCs_);
    u32 key[5]={snapshot.cType,snapshot.cContainer,snapshot.cContainerSerial,snapshot.cIndex,snapshot.cSerial};
    queueInvSnapshot(snapshot.ownerId,snapshot.keyKind,key,items,snapshot.count,snapshot.flags,snapshot.revision);
    InvXferAckPacket p=pkt;p.contentsCount=(u8)contents.size();std::vector<u8> v=payloadTail(p,contents.empty()?0:&contents[0],(unsigned)contents.size());queuePayload(v);nativeLock385_outCs_.unlock();
}
void NetLink::queueWorldItems(u32 owner,const WorldItemEntry* items,unsigned count) {
    if(count>WORLD_ITEMS_MAX)count=WORLD_ITEMS_MAX;if(count && !items){failQueue();return;}
    WorldItemSnapshotHeader h;h.type=PKT_WORLD_ITEM;h.ownerId=owner;h.count=(u8)count;std::vector<u8> v=payloadTail(h,items,count);queuePayload(v);
}
void NetLink::queueWorldRemove(u32 owner,const u32* ids,unsigned count) {
    if(count>255)count=255;if(count && !ids){failQueue();return;}WorldItemRemoveHeader h;h.type=PKT_WORLD_ITEM_REMOVE;h.ownerId=owner;h.count=(u8)count;
    std::vector<u8> v=payloadTail(h,ids,count);queuePayload(v);
}
void NetLink::queueWorldClaim(u32 owner,u32 author,const u32* ids,unsigned count) {
    if(count>255)count=255;if(count && !ids){failQueue();return;}WorldItemClaimHeader h;h.type=PKT_WORLD_ITEM_CLAIM;h.ownerId=owner;h.authorId=author;h.count=(u8)count;
    std::vector<u8> v=payloadTail(h,ids,count);queuePayload(v);
}
void NetLink::queueNpcCensus(u32 owner,const u32* hands,const float* pos,unsigned count) {
    if(count>NPC_CENSUS_MAX)count=NPC_CENSUS_MAX;if(count && !hands){failQueue();return;}
    NpcCensusHeader h;h.type=PKT_NPC_CENSUS;h.ownerId=owner;h.count=(u16)count;std::vector<u8> v(sizeof(h)+count*32,0);memcpy(&v[0],&h,sizeof(h));
    if(count){memcpy(&v[sizeof(h)],hands,count*20);if(pos)memcpy(&v[sizeof(h)+count*20],pos,count*12);}queuePayload(v);
}
void NetLink::queueSaveFile(const SaveFileHeader& hdr,const char* path,const unsigned char* data,unsigned count) {
    if(!path || !hdr.pathLen || hdr.pathLen>SAVE_PATH_MAX || count!=hdr.dataLen || count>SAVE_CHUNK_MAX || (count && !data)){failQueue();return;}
    std::vector<u8> v(sizeof(hdr)+hdr.pathLen+count);memcpy(&v[0],&hdr,sizeof(hdr));memcpy(&v[sizeof(hdr)],path,hdr.pathLen);if(count)memcpy(&v[sizeof(hdr)+hdr.pathLen],data,count);queuePayload(v);
}
void NetLink::queueSaveDone(const SaveDoneHeader& hdr,const u32* crcs,unsigned count) {
    if(count!=hdr.fileCount || (count && !crcs)){failQueue();return;}std::vector<u8> v=payloadTail(hdr,crcs,count);queuePayload(v);
}
void NetLink::queueLoadGo(const LoadGoPacket& pkt) {
    NativeLock nativeLock415_outCs_(&outCs_);LoadGoPacket p=pkt;p.worldToken=worldToken_;queuePayload(&p,sizeof(p));nativeLock415_outCs_.unlock();
}
void NetLink::queueWorldReady(u32 completedLoadId) {
    NativeLock nativeLock418_outCs_(&outCs_);
    if(!isHost_ && completedLoadId && completedLoadId==latestLoadId_ && latestLoadWorld_==worldToken_) {
        localReady_=true;WorldReadyPacket p;p.type=PKT_WORLD_READY;p.ownerId=(u32)myId_;p.loadId=completedLoadId;p.worldToken=latestLoadWorld_;queuePayload(&p,sizeof(p));
    }
    nativeLock418_outCs_.unlock();
}
void NetLink::setNetSim(unsigned delay,unsigned jitter,unsigned loss) {simDelayMs_=delay;simJitterMs_=jitter;simLossPct_=loss>100?100:loss;}
void NetLink::deliverEntity(u32 owner,u32 stamp,u64 sequence,const EntityState& e) {
    if(!inbound_)return;
    if(!simDelayMs_ && !simJitterMs_ && !simLossPct_) {inbound_->pushEntity(owner,stamp,e);return;}
    if(simLossPct_ && (unsigned)(rand()%100)<simLossPct_)return;
    int jitter=simJitterMs_?(rand()%(int)(2*simJitterMs_+1))-(int)simJitterMs_:0;int delay=(int)simDelayMs_+jitter;if(delay<0)delay=0;
    if(delayed_.size()>=8192){failQueue();return;}
    Delayed d;d.ownerId=owner;d.sendMs=stamp;d.sequence=sequence;d.incarnation=ownerIncarnation_[owner];d.e=e;d.releaseTick=GetTickCount()+delay;d.epoch=(u32)sendEpoch_;d.inboundGeneration=inbound_->sessionGeneration();delayed_.push_back(d);
}
void NetLink::flushDelayed() {
    NativeLock nativeLock434_outCs_(&outCs_);DWORD now=GetTickCount();
    for(std::deque<Delayed>::iterator i=delayed_.begin();i!=delayed_.end();) {
        StateKey key;memset(&key,0,sizeof(key));key.owner=i->ownerId;key.type=PKT_ENTITY_BATCH+256u;memcpy(key.hand,&i->e,sizeof(key.hand));
        const std::map<StateKey,u64>::iterator latest=stateSeen_.find(key);
        if(i->epoch!=(u32)sendEpoch_ || !inbound_ || i->inboundGeneration!=inbound_->sessionGeneration() ||
           i->incarnation!=ownerIncarnation_[i->ownerId] || (latest!=stateSeen_.end() && latest->second>=i->sequence))i=delayed_.erase(i);
        else if((long)(now-i->releaseTick)>=0) {
            Inbound::WorldEnqueueLock worldLock(inbound_,i->inboundGeneration,true);
            if(worldLock.locked()) {
                if(stateSeen_.size()>=32768 && latest==stateSeen_.end())failQueue();
                else {inbound_->pushEntity(i->ownerId,i->sendMs,i->e);stateSeen_[key]=i->sequence;}
                worldLock.unlock();
            }
            i=delayed_.erase(i);
        }else ++i;
    }
    nativeLock434_outCs_.unlock();
}

void NetLink::disconnectNative(u32 connection,int reason,const char* text) {
    const int r=api_.disconnect(context_,connection,reason,text);
    if(r!=COOP_NET_OK && r!=COOP_NET_CLOSED) {
        setError(NET_ERR_PROVIDER_POLL,r,"native peer disconnect failed");
        InterlockedExchange(&stopFlag_,1);setPhase(NET_FAILED);return;
    }
    std::map<u32,Peer>::iterator i=peers_.find(connection);
    if(i!=peers_.end())i->second.closed=true;
}
int NetLink::sendFrame(Peer& peer,unsigned lane,std::vector<u8>& bytes) {
    // Controls may pass readiness-blocked world frames. Replay order is wire
    // acceptance order, while the publication's origin chronology stays intact.
    const u64 sequence=peer.sendSequence[lane]+1;
    memcpy(&bytes[offsetof(NativeFrame,sequence)],&sequence,sizeof(sequence));
    const int r=api_.send(context_,peer.connection,lane,&bytes[0],(u32)bytes.size());
    if(r==COOP_NET_OK) {
        peer.sendSequence[lane]=sequence;sentBytes_+=(u32)bytes.size();++sentPackets_;
        if(bytes[sizeof(NativeFrame)]==PKT_WORLD_READY && !isHost_)peer.ready=true;
    }else if(r==COOP_NET_CLOSED) {
        // Defer retirement until polling has released its current event loan
        // and fanout/drain iterations have finished using their peer references.
        peer.closed=true;
    }else if(r!=COOP_NET_BACKPRESSURE) {
        setError(NET_ERR_PROVIDER_POLL,r,"native send failed");
        InterlockedExchange(&stopFlag_,1);setPhase(NET_FAILED);
    }
    return r;
}
bool NetLink::sendNative(u32 connection,unsigned lane,const void* data,unsigned size,u32 origin,const NativeFrame* provenance) {
    if(!data || !size || lane>2 || size>COOP_NET_MAX_MESSAGE-sizeof(NativeFrame)) {failQueue();return false;}
    std::map<u32,Peer>::iterator it=peers_.find(connection);if(it==peers_.end() || it->second.closed)return false;Peer& peer=it->second;
    const u8 type=*(const u8*)data;const bool world=!nativeSessionType(type);
    if(type!=PKT_HELLO && type!=PKT_WELCOME && !peer.admitted)return false;
    if(type==PKT_WORLD_READY) {WorldReadyPacket p;memcpy(&p,data,sizeof(p));if(!currentReady(p))return false;}
    const bool worldBlocked=world && (!localReady_ || !peer.ready || (isHost_ && peer.expectedWorld!=worldToken_));
    if(world && (!worldToken_ || (worldBlocked && lane==GAME_STATE)))return false;
    if(origin==OWNER_ID_ALL)origin=isHost_?0:(type==PKT_HELLO?1:(u32)myId_);
    if(type==PKT_PROCEDURAL) {
        ProceduralPacket p;
        if(!nativeRead(data,size,p) || size!=sizeof(p) ||
           !nativePacketValid(data,size,lane,!isHost_,origin) || !currentProcedural(p,&peer))return false;
    }
    if(type==PKT_ENVIRONMENT_ITEMS) {
        EnvironmentItemPacket p;
        if(!nativeRead(data,size,p) || size!=sizeof(p) ||
           !nativePacketValid(data,size,lane,!isHost_,origin) || !currentEnvironment(p,&peer))return false;
    }
    if(type==PKT_LOAD_GO) {LoadGoPacket p;memcpy(&p,data,sizeof(p));if(peer.expectedLoadId!=p.loadId || peer.expectedWorld!=p.worldToken)peer.ready=false;peer.expectedLoadId=p.loadId;peer.expectedWorld=p.worldToken;}
    NativeFrame f;memset(&f,0,sizeof(f));f.magic=NATIVE_FRAME_MAGIC;f.version=PROTOCOL_VERSION;f.lane=(u8)lane;f.incarnation=incarnation_;
    f.session=type==PKT_HELLO?0:sessionToken_;f.world=world?worldToken_:0;f.origin=origin;f.payloadBytes=size;f.originIncarnation=provenance?provenance->originIncarnation:incarnation_;f.originSequence=provenance?provenance->originSequence:++peer.captureSequence[lane];
    f.captureMs=provenance?provenance->captureMs:monoMs();
    f.originEpoch=provenance?provenance->originEpoch:(u32)sendEpoch_;
    std::vector<u8>& bytes=frameBytes_;bytes.resize(sizeof(f)+size);memcpy(&bytes[0],&f,sizeof(f));memcpy(&bytes[sizeof(f)],data,size);
    if(lane==GAME_STATE) {
        if(bytes.size()>COOP_NET_MAX_STATE_MESSAGE){failQueue();return false;}
        return sendFrame(peer,lane,bytes)==COOP_NET_OK;
    }
    // Every recipient has its own per-lane FIFO. Accepted recipients never receive a retry twice.
    std::deque<Pending>& q=pending_[lane][connection];
    if(q.empty() && !worldBlocked) {
        const int r=sendFrame(peer,lane,bytes);
        if(r==COOP_NET_OK)return true;
        if(r!=COOP_NET_BACKPRESSURE)return false;
    }
    const unsigned cap=lane==SAVE_BULK?4*1024*1024:2*1024*1024;
    if(q.size()>=4096 || bytes.size()>cap-pendingBytes_[lane]){failQueue();return false;}
    q.push_back(Pending());Pending& p=q.back();p.epoch=(u32)sendEpoch_;p.world=world;p.bytes.swap(bytes);pendingBytes_[lane]+=(unsigned)p.bytes.size();return true;
}
void NetLink::broadcastNative(unsigned lane,const void* p,unsigned n,u32 except,u32 origin,const NativeFrame* provenance) {
    u32 recipient=OWNER_ID_ALL;
    if(n==sizeof(SpawnReqPacket) && *(const u8*)p==PKT_SPAWN_REQ)
        recipient=((const SpawnReqPacket*)p)->targetOwner;
    else if(n==sizeof(SpawnInfoPacket) && *(const u8*)p==PKT_SPAWN_INFO)
        recipient=((const SpawnInfoPacket*)p)->requestorId;
    for(std::map<u32,Peer>::iterator i=peers_.begin();i!=peers_.end();++i)
        if(i->first!=except && i->second.admitted &&
           (recipient==OWNER_ID_ALL || i->second.playerId==recipient))
            sendNative(i->first,lane,p,n,origin,provenance);
}
void NetLink::drainPending() {
    NativeLock nativeLock485_outCs_(&outCs_);
    for(unsigned lane=0;lane<3 && !stopFlag_;++lane)for(std::map<u32,std::deque<Pending> >::iterator i=pending_[lane].begin();i!=pending_[lane].end() && !stopFlag_;++i) {
        std::deque<Pending>& q=i->second;
        std::map<u32,Peer>::iterator peerIt=peers_.find(i->first);
        // Purge stale world frames even behind a readiness-blocked head.
        for(std::deque<Pending>::iterator p=q.begin();p!=q.end();) {
            NativeFrame f;memcpy(&f,&p->bytes[0],sizeof(f));
            bool staleReady=false;
            if(p->bytes[sizeof(NativeFrame)]==PKT_WORLD_READY) {WorldReadyPacket ready;memcpy(&ready,&p->bytes[sizeof(NativeFrame)],sizeof(ready));staleReady=!currentReady(ready);}
            if(p->bytes[sizeof(NativeFrame)]==PKT_PROCEDURAL) {
                ProceduralPacket proc;memcpy(&proc,&p->bytes[sizeof(NativeFrame)],sizeof(proc));
                staleReady=p->epoch!=(u32)sendEpoch_ || !currentProcedural(proc,peerIt==peers_.end()?0:&peerIt->second);
            }
            if(p->bytes[sizeof(NativeFrame)]==PKT_ENVIRONMENT_ITEMS) {
                EnvironmentItemPacket packet;memcpy(&packet,&p->bytes[sizeof(NativeFrame)],sizeof(packet));
                staleReady=p->epoch!=(u32)sendEpoch_ ||
                    !currentEnvironment(packet,peerIt==peers_.end()?0:&peerIt->second);
            }
            if(peerIt==peers_.end() || peerIt->second.closed || staleReady || (p->world && (p->epoch!=(u32)sendEpoch_ || f.world!=worldToken_))) {
                pendingBytes_[lane]-=(unsigned)p->bytes.size();p=q.erase(p);
            }else ++p;
        }
        if(peerIt==peers_.end() || peerIt->second.closed)continue;
        Peer& peer=peerIt->second;
        while(!q.empty() && !stopFlag_) {
            std::deque<Pending>::iterator p=q.begin();
            const bool worldBlocked=!localReady_ || !peer.ready || (isHost_ && peer.expectedWorld!=worldToken_);
            if(p->world && worldBlocked) {
                if(lane!=GAME_RELIABLE)break;
                // Only session controls bypass blocked world data; FIFO is
                // preserved within each class and every bulk/save lane.
                while(p!=q.end() && p->world)++p;
                if(p==q.end())break;
            }
            const int r=sendFrame(peer,lane,p->bytes);
            if(r!=COOP_NET_OK)break;
            pendingBytes_[lane]-=(unsigned)p->bytes.size();q.erase(p);
        }
    }
    nativeLock485_outCs_.unlock();
}
bool NetLink::acceptState(u32 owner,u8 type,u64 sequence,const void* data,unsigned size) {
    StateKey key;memset(&key,0,sizeof(key));key.owner=owner;key.type=type;
    if(type==PKT_ENTITY_BATCH && size==sizeof(EntityState))memcpy(key.hand,data,sizeof(key.hand));
    else if(type==PKT_STEALTH && size==sizeof(StealthPacket))memcpy(key.hand,(const u8*)data+5,sizeof(key.hand));
    std::map<StateKey,u64>::iterator i=stateSeen_.find(key);
    if(i!=stateSeen_.end() && sequence<=i->second)return false;
    if(i==stateSeen_.end() && stateSeen_.size()>=32768){failQueue();return false;}stateSeen_[key]=sequence;return true;
}
bool NetLink::acceptEvent(const NativeFrame& frame,const EventPacket& event) {
    StateKey key;memset(&key,0,sizeof(key));key.owner=event.ownerId;key.type=PKT_EVENT;
    memcpy(key.hand,&frame.originIncarnation,sizeof(frame.originIncarnation));
    memcpy(key.hand+2,&frame.world,sizeof(frame.world));key.hand[4]=frame.originEpoch;
    std::map<StateKey,u64>::iterator previous=stateSeen_.find(key);
    if(previous!=stateSeen_.end() && event.eventId<=previous->second)return false;
    if(previous==stateSeen_.end() && stateSeen_.size()>=32768){failQueue();return false;}
    stateSeen_[key]=event.eventId;return true;
}
void NetLink::sendRoster() {
    RosterPacket p;memset(&p,0,sizeof(p));p.type=PKT_PLAYER_ROSTER;
    NativeLock nativeLock508_nameCs_(&nameCs_);
    memcpy(p.name[0],localName_,sizeof(localName_));
    for(u32 i=1;i<MAX_PLAYERS;++i)memcpy(p.name[i],peerName_[i],sizeof(peerName_[i]));
    nativeLock508_nameCs_.unlock();
    NetStatus s;copyStatus(&s);
    unsigned char bytes[sizeof(RosterPacket)+1+MAX_PLAYERS*sizeof(u64)];
    memcpy(bytes,&p,sizeof(p));bytes[sizeof(p)]=(u8)s.memberMask;
    ownerIncarnation_[0]=incarnation_;
    memcpy(bytes+sizeof(p)+1,ownerIncarnation_,sizeof(ownerIncarnation_));
    broadcastNative(GAME_RELIABLE,bytes,sizeof(bytes));
}
void NetLink::sampleNativeStats() {
    DWORD now=GetTickCount();if(lastStatsTick_ && now-lastStatsTick_<1000)return;
    DWORD elapsed=lastStatsTick_?now-lastStatsTick_:1000;lastStatsTick_=now;
    NetDebugStats s;memset(&s,0,sizeof(s));s.sampleTickMs=now;
    s.sentBytesPerSec=(DWORD)((u64)sentBytes_*1000/elapsed);s.recvBytesPerSec=(DWORD)((u64)receivedBytes_*1000/elapsed);
    s.sentPacketsPerSec=(DWORD)((u64)sentPackets_*1000/elapsed);s.recvPacketsPerSec=(DWORD)((u64)receivedPackets_*1000/elapsed);
    sentBytes_=receivedBytes_=sentPackets_=receivedPackets_=0;u32 pending=0;
    for(std::map<u32,Peer>::iterator i=peers_.begin();i!=peers_.end();++i) {
        CoopNetPeerStats p;memset(&p,0,sizeof(p));p.size=sizeof(p);if(api_.peerStats(context_,i->first,&p)!=COOP_NET_OK)continue;
        if(!i->second.admitted)++pending;++s.connectedPeers;
        if(s.peerStatsCount>=MAX_PLAYERS)continue;NetDebugPeer& d=s.peers[s.peerStatsCount++];d.slot=i->first;d.playerId=i->second.playerId;d.rttMs=p.rttMs>0?(DWORD)p.rttMs:0;
        if(d.rttMs>s.maxRttMs)s.maxRttMs=d.rttMs;
        d.pendingReliableBytes=p.pendingReliableBytes;d.pendingStateBytes=p.pendingStateBytes;d.pendingBulkBytes=p.pendingBulkBytes;d.unackedReliableBytes=p.unackedReliableBytes;
        for(unsigned lane=0;lane<3;++lane) {unsigned bytes=0;std::deque<Pending>& q=pending_[lane][i->first];for(size_t n=0;n<q.size();++n)bytes+=(unsigned)q[n].bytes.size();
            if(lane==GAME_RELIABLE)d.pendingReliableBytes+=bytes;else if(lane==SAVE_BULK)d.pendingBulkBytes+=bytes;else d.pendingStateBytes+=bytes;}
        s.pendingReliableBytes+=d.pendingReliableBytes;s.pendingStateBytes+=d.pendingStateBytes;s.pendingBulkBytes+=d.pendingBulkBytes;s.unackedReliableBytes+=d.unackedReliableBytes;
    }
    NativeLock nativeLock536_outCs_(&outCs_);for(size_t i=0;i<publications_.size();++i) {unsigned lane=nativeLane(publications_[i].bytes[0]);unsigned bytes=(unsigned)publications_[i].bytes.size();if(lane==SAVE_BULK)s.pendingBulkBytes+=bytes;else if(lane==GAME_RELIABLE)s.pendingReliableBytes+=bytes;else s.pendingStateBytes+=bytes;}nativeLock536_outCs_.unlock();
    CoopNetStatus ns;memset(&ns,0,sizeof(ns));ns.size=sizeof(ns);if(api_.status(context_,&ns)==COOP_NET_OK) {
        NativeLock nativeLock538_statusCs_(&statusCs_);status_.selfSteamId=ns.selfSteamId;status_.authentication=ns.authentication;status_.relayAvailability=ns.relayAvailability;status_.pendingHandshakes=pending;nativeLock538_statusCs_.unlock();
    }
    NativeLock nativeLock540_debugCs_(&debugCs_);debugStats_=s;nativeLock540_debugCs_.unlock();
}
bool NetLink::shouldRelayType(u8 type, const void* payload, unsigned int size) {
    // Host-only / handshake / per-peer RTT: never rebroadcast.
    if (type==PKT_INV_SNAPSHOT) {
        InvSnapshotHeader packet;
        if (size<sizeof(packet)) return false;
        memcpy(&packet,payload,sizeof(packet));
        return (packet.flags & INV_FLAG_CHECKPOINT_ACK)==0;
    }
    if (type==PKT_INV_XFER) {
        InvXferPacket packet;
        if (size<sizeof(packet)) return false;
        memcpy(&packet,payload,sizeof(packet));
        return packet.operation==0;
    }
    switch (type) {
        case PKT_HELLO:
        case PKT_WELCOME:
        case PKT_TIME_PING:
        case PKT_TIME_PONG:
        case PKT_WORLD_READY:
        case PKT_SESSION_ACK:
        case PKT_PROCEDURAL:
        case PKT_ENVIRONMENT_ITEMS:
        case PKT_TIME:
        case PKT_SAVE_REQ:
        case PKT_SAVE_ACK:
        case PKT_LOAD_REQ:
        case PKT_LOAD_NACK:
        case PKT_MONEY_DELTA:
        case PKT_COMBAT_HIT:
        case PKT_SPEED_REQ:
#ifdef KENSHICOOP_NET_DIAG
        case PKT_DEBUG_LOG_FILE:
#endif
            return false;
        default:
            return true;
    }
}


DWORD WINAPI NetLink::threadEntry(LPVOID self) {
    NetLink* link=(NetLink*)self;
    try {link->threadLoop();}
    catch(...) {link->failQueue();link->closeProvider();link->setActiveTransport(NET_TRANSPORT_NONE);link->clearDebugStats();InterlockedExchange(&link->running_,0);}
    return 0;
}
void NetLink::threadLoop() {
    if(!loadProvider() || !startProvider()) {setPhase(NET_FAILED);closeProvider();InterlockedExchange(&running_,0);return;}
    u32 connectAttempts=isHost_?0:1;bool linkUp=false,linkWelcomed=false,everWelcomed=false;
    DWORD lastConnectAttempt=GetTickCount(),attemptEndedAt=0;
    bool nativeAttemptActive=!isHost_;
    DWORD lastTimePing=0,lastClockLog=0;u32 pingNonce=1;unsigned long bestRttMs=0xfffffffful;long bestOffsetMs=0;unsigned syncSamples=0;
    const long HALF_DAY_MS=12l*3600l*1000l;
    DWORD lastEntitySendTick=0;bool haveEntitySendTick=false,forceEntitySend=true;
    size_t nextEntityBatch=0;
    DWORD rosterAt=0;bool rosterChanged=true;
    unsigned char lastRoster[sizeof(RosterPacket)+1+MAX_PLAYERS*sizeof(u64)]={0};
#ifdef KENSHICOOP_NET_DIAG
    bool mirrorFault[MAX_PLAYERS]={false};DWORD mirrorWindow=GetTickCount();unsigned mirrorSent=0;
#endif
    setHandshakeCounts(connectAttempts,0);
    while(!stopFlag_) {
        processCommands();drainPending();
        if(stopFlag_)break;
        const DWORD attemptNow=GetTickCount();
        if(!isHost_ && !serverConnection_ && nativeAttemptActive && attemptNow-lastConnectAttempt>=NATIVE_CONNECT_TIMEOUT_MS) {
            NativeLock timeoutPumpLock(&pumpCs_);api_.stop(context_);timeoutPumpLock.unlock();
            nativeAttemptActive=false;attemptEndedAt=attemptNow;
            setError(NET_ERR_NO_RESPONSE,connectAttempts,"native initial connection timed out");
        }
        if(!isHost_ && !serverConnection_ && !nativeAttemptActive && attemptNow-attemptEndedAt>=NATIVE_RETRY_DELAY_MS) {
            lastConnectAttempt=attemptNow;NativeLock nativeLock592_pumpCs_(&pumpCs_);api_.stop(context_);api_.destroy(context_);context_=0;nativeLock592_pumpCs_.unlock();
            NativeLock nativeLock593_outCs_(&outCs_);incarnation_=freshToken();sessionToken_=worldToken_=0;latestLoadId_=0;latestLoadWorld_=0;localReady_=false;
            publications_.clear();publicationBytes_=0;clearWorldPublication();stateSeen_.clear();delayed_.clear();memset(ownerIncarnation_,0,sizeof(ownerIncarnation_));
            for(unsigned lane=0;lane<3;++lane) {pending_[lane].clear();pendingBytes_[lane]=0;}peers_.clear();nativeLock593_outCs_.unlock();
            ++connectAttempts;if(!startProvider()){setPhase(NET_FAILED);break;}lastConnectAttempt=GetTickCount();nativeAttemptActive=true;setPhase(everWelcomed?NET_RECONNECTING:NET_CONNECTING);setHandshakeCounts(connectAttempts,0);
        }
        CoopNetEvent ev;int polled=0;
        for(unsigned eventCount=0;eventCount<256 && !stopFlag_;++eventCount) {
            memset(&ev,0,sizeof(ev));ev.size=sizeof(ev);const u32 inboundGeneration=inbound_?inbound_->sessionGeneration():0;
            polled=api_.poll(context_,&ev);if(polled<0){setError(NET_ERR_PROVIDER_POLL,0,"native provider poll failed");setPhase(NET_FAILED);InterlockedExchange(&stopFlag_,1);break;}if(!polled)break;
            NativeEventScope eventScope(api_,context_,ev);
            if(ev.generation!=providerGeneration_){eventScope.release();continue;}
            if(ev.type==COOP_NET_EVENT_CONNECTED) {
                NativeLock nativeLock604_outCs_(&outCs_);
                bool permitted=true;
                if(steamMode_) {
                    permitted=ev.remoteSteamId!=0 && (isHost_ || ev.remoteSteamId==options_.remoteSteamId);
                }
                if(!ev.connection || !permitted || peers_.size()>=MAX_JOINS || peers_.find(ev.connection)!=peers_.end()) {
                    disconnectNative(ev.connection,NATIVE_REJECT_IDENTITY,"native identity admission rejected");
                    if(!isHost_ && !serverConnection_) {nativeAttemptActive=false;attemptEndedAt=GetTickCount();}
                }else {
                    Peer p;p.connection=ev.connection;p.steamId=ev.remoteSteamId;p.connectedAt=GetTickCount();peers_[ev.connection]=p;
                    forceEntitySend=true;
                    if(!isHost_) {
                        serverConnection_=ev.connection;nativeAttemptActive=false;linkUp=true;linkWelcomed=false;setPhase(NET_HANDSHAKING);
                        HelloPacket h;h.type=PKT_HELLO;h.version=PROTOCOL_VERSION;char buf[sizeof(h)+HELLO_NAME_MAX];
                        NativeLock nativeLock618_nameCs_(&nameCs_);unsigned n=0;while(localName_[n] && n<HELLO_NAME_MAX)++n;h.nameLen=(u8)n;memcpy(buf,&h,sizeof(h));if(n)memcpy(buf+sizeof(h),localName_,n);nativeLock618_nameCs_.unlock();
                        sendNative(ev.connection,GAME_RELIABLE,buf,sizeof(h)+n);
                    }
                }
                nativeLock604_outCs_.unlock();
            }else if(ev.type==COOP_NET_EVENT_MESSAGE) {
                NativeLock nativeLock624_outCs_(&outCs_);
                std::map<u32,Peer>::iterator found=peers_.find(ev.connection);
                if(found!=peers_.end() && found->second.closed)continue;
                NativeFrame frame;bool valid=found!=peers_.end() && ev.data && ev.dataSize>=sizeof(frame);
                bool replayed=false,staleOrigin=false;
                if(valid)memcpy(&frame,ev.data,sizeof(frame));
                const u8* payload=valid?(const u8*)ev.data+sizeof(frame):0;
                const unsigned payloadSize=valid?ev.dataSize-(unsigned)sizeof(frame):0;
                const u8 type=packetType(payload,payloadSize);
                if(valid)valid=frame.magic==NATIVE_FRAME_MAGIC && frame.version==PROTOCOL_VERSION && !frame.reserved && frame.lane==ev.traffic && ev.traffic<3 &&
                    frame.payloadBytes==payloadSize && frame.incarnation && frame.originIncarnation && frame.originSequence && frame.sequence &&
                    ev.dataSize<=COOP_NET_MAX_MESSAGE && (ev.traffic!=GAME_STATE || ev.dataSize<=COOP_NET_MAX_STATE_MESSAGE);
                if(valid)valid=nativeSessionType(type)?frame.world==0:frame.world!=0;
                if(valid) {
                    Peer& p=found->second;
                    replayed=ev.traffic!=GAME_STATE && p.incarnation==frame.incarnation && frame.sequence<=p.replay[ev.traffic].newest;
                    if(!replayed) {
                    if(isHost_ && type==PKT_HELLO)valid=!p.playerId && !p.incarnation && frame.session==0 && frame.originIncarnation==frame.incarnation;
                    else if(!isHost_ && type==PKT_WELCOME)valid=!p.admitted && !p.incarnation && frame.session && frame.origin==0 && frame.originIncarnation==frame.incarnation;
                    else valid=frame.incarnation==p.incarnation && frame.session==sessionToken_ && (p.admitted || (isHost_ && type==PKT_SESSION_ACK && p.playerId));
                    if(valid && isHost_ && type!=PKT_HELLO)valid=frame.origin==p.playerId && frame.originIncarnation==p.incarnation;
                    if(valid && !isHost_ && type!=PKT_WELCOME && type!=PKT_PLAYER_ROSTER) {
                        valid=frame.origin<MAX_PLAYERS;
                        if(valid && ownerIncarnation_[frame.origin]!=frame.originIncarnation)staleOrigin=true;
                    }
                    if(valid)valid=nativePacketValid(payload,payloadSize,ev.traffic,isHost_,frame.origin);
                    if(valid && ev.traffic!=GAME_STATE)p.replay[ev.traffic].accept(frame.sequence);
                    }
                }
                if(!valid) {
                    const bool haveHeader=found!=peers_.end() && ev.data && ev.dataSize>=sizeof(frame);
                    u16 remoteVersion=0;
                    if(haveHeader)memcpy(&remoteVersion,(const u8*)ev.data+offsetof(NativeFrame,version),sizeof(remoteVersion));
                    char detail[160];_snprintf(detail,sizeof(detail)-1,
                        "invalid native gameplay frame: type=%u lane=%u origin=%u bytes=%u receiver=%s",
                        (unsigned)type,(unsigned)ev.traffic,haveHeader?(unsigned)frame.origin:OWNER_ID_ALL,
                        payloadSize,isHost_?"host":"client");detail[sizeof(detail)-1]=0;netErr(detail);
                    setError(isHost_ && haveHeader && remoteVersion!=PROTOCOL_VERSION?NET_ERR_PEER_VERSION:NET_ERR_PROTOCOL,remoteVersion,detail);
                    if(!isHost_){setPhase(NET_FAILED);InterlockedExchange(&stopFlag_,1);}
                    disconnectNative(ev.connection,NATIVE_REJECT_PROTOCOL,"invalid authenticated gameplay frame");
                }else if(!replayed && !staleOrigin) {
                    Peer& peer=found->second;
                    if(type==PKT_HELLO)peer.incarnation=frame.incarnation;
                    if(type==PKT_WELCOME){peer.incarnation=frame.incarnation;sessionToken_=frame.session;}
                    const bool world=!nativeSessionType(type);
                    bool proceduralCurrent=true;
                    if(type==PKT_PROCEDURAL) {
                        ProceduralPacket proc;memcpy(&proc,payload,sizeof(proc));
                        proceduralCurrent=currentProcedural(proc,&peer);
                    }
                    if(type==PKT_ENVIRONMENT_ITEMS) {
                        EnvironmentItemPacket packet;memcpy(&packet,payload,sizeof(packet));
                        proceduralCurrent=currentEnvironment(packet,&peer);
                    }
                    const bool scopedControl=type==PKT_PROCEDURAL || type==PKT_ENVIRONMENT_ITEMS;
                    const bool worldCurrent=proceduralCurrent && (!world || (frame.world==worldToken_ && localReady_ && peer.ready && (!isHost_ || peer.expectedWorld==worldToken_)));
                    Inbound::WorldEnqueueLock worldLock(inbound_,inboundGeneration,(world || scopedControl) && worldCurrent);
                    const bool inboundLocked=worldLock.locked();
                    if(worldCurrent && (!(world || scopedControl) || inboundLocked)) {
                        if(type==PKT_EVENT) {
                            EventPacket event;memcpy(&event,payload,sizeof(event));
                            if(!acceptEvent(frame,event))continue;
                        }
                        if(inbound_ && !inbound_->canAcceptNative()) {
                            failQueue();
                            worldLock.unlock();
                            nativeLock624_outCs_.unlock();
                            eventScope.release();
                            break;
                        }
                        receivedBytes_+=ev.dataSize;++receivedPackets_;
                        if(type==PKT_SESSION_ACK && isHost_) {
                            peer.admitted=true;peer.ready=false;setMemberBit(peer.playerId,true);rosterChanged=true;
                            sendRoster(); // bind every admitted origin before its reliable relays
                            if(inbound_)inbound_->pushConnect(peer.playerId);
                        }else if(type==PKT_WORLD_READY && isHost_) {
                            WorldReadyPacket ready;memcpy(&ready,payload,sizeof(ready));
                            if(ready.loadId==peer.expectedLoadId && ready.worldToken==peer.expectedWorld && ready.worldToken==worldToken_)peer.ready=true;
                        }else {
                    if (isHost_ && type == PKT_HELLO) {
                        HelloPacket h;
                        if (readPacket(payload, (unsigned)payloadSize, &h)) {
                            if (h.version != PROTOCOL_VERSION) {
                                char b[128];
                                _snprintf(b, sizeof(b) - 1,
                                          "protocol mismatch: peer v%u, ours v%u; rejecting",
                                          (unsigned)h.version, (unsigned)PROTOCOL_VERSION);
                                b[sizeof(b) - 1] = '\0';
                                netErr(b);
                                setError(NET_ERR_PEER_VERSION, (u32)h.version, b);
                                disconnectNative(ev.connection,NATIVE_REJECT_PROTOCOL,"game handshake rejected");
                            } else {
                                u32 id = 0;
                                for (u32 cand = 1; cand <= MAX_JOINS; ++cand) {
                                    bool taken = false;
                                    for(std::map<u32,Peer>::iterator pi=peers_.begin();pi!=peers_.end();++pi) {
                                        if(pi->first!=ev.connection && pi->second.playerId==cand) {taken=true;break;}
                                    }
                                    if (!taken) { id = cand; break; }
                                }
                                if (id == 0) id = (u32)MAX_JOINS + 1; // forces the reject below
                                if (id > MAX_JOINS) {
                                    netErr("server full (max 4 players: host + 3 joins); rejecting");
                                    setError(NET_ERR_SERVER_FULL, 0,
                                             "server full (host + 3 joins); rejected a HELLO");
                                    disconnectNative(ev.connection,NATIVE_REJECT_PROTOCOL,"game handshake rejected");
                                } else {

                                    peer.playerId = id;
                                    ownerIncarnation_[id]=peer.incarnation;
                                    {
                                        char nm[64];
                                        copyHelloName(payload,
                                                      (unsigned)payloadSize,
                                                      nm, sizeof(nm));
                                        std::string parsed;
                                        if (id < MAX_PLAYERS && parsePlayerNick(nm, parsed)) {
                                            NativeLock nativeLock716_nameCs_(&nameCs_);
                                            memset(peerName_[id], 0, sizeof(peerName_[id]));
                                            memcpy(peerName_[id], parsed.c_str(), parsed.size());
                                            nativeLock716_nameCs_.unlock();
                                            char lb[96];
                                            _snprintf(lb, sizeof(lb) - 1,
                                                      "peer nick id=%u '%s'",
                                                      (unsigned)id, parsed.c_str());
                                            lb[sizeof(lb) - 1] = '\0';
                                            netLog(lb);
                                        }
                                    }
                                    WelcomePacket w;
                                    w.type = (u8)PKT_WELCOME; w.version = PROTOCOL_VERSION; w.playerId = id;
                                    char wbuf[sizeof(WelcomePacket) + 1 + HELLO_NAME_MAX];
                                    unsigned wlen = sizeof(w);
                                    memcpy(wbuf, &w, sizeof(w));
                                    NativeLock nativeLock733_nameCs_(&nameCs_);
                                    unsigned hn = 0;
                                    while (localName_[hn] && hn < HELLO_NAME_MAX) ++hn;
                                    if (hn > 0) {
                                        wbuf[sizeof(w)] = (char)(u8)hn;
                                        memcpy(wbuf + sizeof(w) + 1, localName_, hn);
                                        wlen = sizeof(w) + 1 + hn;
                                    }
                                    nativeLock733_nameCs_.unlock();
                                    sendNative(ev.connection,CH_RELIABLE,wbuf,wlen);
                                    char b[96];
                                    _snprintf(b, sizeof(b) - 1,
                                              "peer connected id=%u (proto v%u)",
                                              (unsigned)id, (unsigned)PROTOCOL_VERSION);
                                    b[sizeof(b) - 1] = '\0';
                                    netLog(b);
                                    // Admission is completed only by the client SESSION_ACK.
                                    setError(NET_ERR_NONE, 0, 0); // an admission supersedes an old rejection note
                                }
                            }
                        }
                    } else if (!isHost_ && type == PKT_WELCOME) {
                        WelcomePacket w;
                        if (readPacket(payload, (unsigned)payloadSize, &w)) {
                            if (w.version != PROTOCOL_VERSION) {
                                char b[128];
                                _snprintf(b, sizeof(b) - 1,
                                          "protocol mismatch: host v%u, ours v%u",
                                          (unsigned)w.version, (unsigned)PROTOCOL_VERSION);
                                b[sizeof(b) - 1] = '\0';
                                netErr(b);
                                setError(NET_ERR_PROTOCOL, (u32)w.version, b);
                                disconnectNative(ev.connection,NATIVE_REJECT_PROTOCOL,"game protocol mismatch");
                                setPhase(NET_FAILED);InterlockedExchange(&stopFlag_,1);
                            } else {
                                InterlockedExchange(&myId_, (LONG)w.playerId);
                                peer.admitted=true;peer.ready=true;peer.playerId=0;ownerIncarnation_[0]=peer.incarnation;ownerIncarnation_[w.playerId]=incarnation_;
                                SessionAckPacket ack;ack.type=PKT_SESSION_ACK;ack.ownerId=w.playerId;
                                sendNative(ev.connection,GAME_RELIABLE,&ack,sizeof(ack));
#ifdef KENSHICOOP_NET_DIAG
                                coop::logMirrorCapture(true);
#endif
                                {
                                    char nm[64];
                                    if (copyWelcomeName(payload,
                                                        (unsigned)payloadSize,
                                                        nm, sizeof(nm))) {
                                        std::string parsed;
                                        if (parsePlayerNick(nm, parsed)) {
                                            NativeLock nativeLock781_nameCs_(&nameCs_);
                                            memset(peerName_[0], 0, sizeof(peerName_[0]));
                                            memcpy(peerName_[0], parsed.c_str(), parsed.size());
                                            nativeLock781_nameCs_.unlock();
                                        }
                                    }
                                }
                                char b[96];
                                _snprintf(b, sizeof(b) - 1,
                                          "peer connected id=%u (proto v%u) - received WELCOME",
                                          (unsigned)w.playerId, (unsigned)PROTOCOL_VERSION);
                                b[sizeof(b) - 1] = '\0';
                                netLog(b);
                                if (inbound_) inbound_->pushConnect(0); // host id = 0
                                linkWelcomed = true;
                                everWelcomed = true;
                                setMembers(w.playerId < MAX_PLAYERS
                                               ? (1u | (1u << w.playerId)) : 1u);
                                setError(NET_ERR_NONE, 0, 0);
                                setPhase(NET_CONNECTED);
                            }
                        }
#ifdef KENSHICOOP_NET_DIAG
                    } else if (isHost_ && type == PKT_DEBUG_LOG_FILE) {
                        const unsigned len = (unsigned)payloadSize;
                        const u32 id = peer.playerId;
                        DebugLogFileHeader hdr;
                        if (ev.traffic == CH_BULK && id > 0 && id <= MAX_JOINS &&
                            readPacket(payload, len, &hdr) &&
                            hdr.byteCount > 0 && hdr.byteCount <= DEBUG_LOG_CHUNK_MAX &&
                            len == sizeof(hdr) + hdr.byteCount) {
                            const char* bytes = (const char*)payload + sizeof(hdr);
                            if (coop::logRemoteChunk(id, hdr.offset, bytes, hdr.byteCount)) {
                                mirrorFault[id] = false;
                                if (hdr.offset == 0) {
                                    char line[96];
                                    _snprintf(line, sizeof(line) - 1,
                                              "[log-mirror] receiving join id=%u in separate file",
                                              (unsigned)id);
                                    line[sizeof(line) - 1] = '\0';
                                    coop::logLine(line);
                                }
                            } else if (!mirrorFault[id]) {
                                mirrorFault[id] = true;
                                netErr("[log-mirror] unable to write client log or invalid offset");
                            }
                        }
#endif
                    } else if (type == PKT_ENTITY_BATCH) {
                        const unsigned len = (unsigned)payloadSize;
                        if (len >= sizeof(EntityBatchHeader) && inbound_) {
                            EntityBatchHeader hdr;
                            std::memcpy(&hdr, payload, sizeof(hdr));
                            unsigned need =
                                sizeof(EntityBatchHeader) + (unsigned)hdr.count * sizeof(EntityState);
                            if (len == need) {
                                const u8* p = payload + sizeof(EntityBatchHeader);
                                for (unsigned i = 0; i < hdr.count; ++i) {
                                    EntityState e;
                                    std::memcpy(&e, p + i * sizeof(EntityState), sizeof(e));
                                    if(acceptState(hdr.ownerId,PKT_ENTITY_BATCH,frame.originSequence,&e,sizeof(e)))deliverEntity(hdr.ownerId,hdr.sendMs,frame.originSequence,e);
                                }
                            }
                        }
                    } else if (type == PKT_EVENT) {
                        EventPacket evp;
                        if (readPacket(payload, (unsigned)payloadSize, &evp)
                            && inbound_) {
                            inbound_->pushEvent(evp.ownerId, evp);
                        }
                    } else if (type == PKT_INV_SNAPSHOT) {
                        const unsigned len = (unsigned)payloadSize;
                        if (len >= sizeof(InvSnapshotHeader) && inbound_) {
                            InvSnapshotHeader hdr;
                            std::memcpy(&hdr, payload, sizeof(hdr));
                            unsigned need = sizeof(InvSnapshotHeader)
                                          + (unsigned)hdr.count * sizeof(InvItemEntry);
                            if (len >= need) {
                                const u8* p = payload + sizeof(InvSnapshotHeader);
                                const InvItemEntry* items =
                                    (hdr.count > 0) ? reinterpret_cast<const InvItemEntry*>(p) : 0;
                                if(!inbound_->pushInvFrame(hdr,items)) {
                                    setError(NET_ERR_PROTOCOL,0,"inconsistent complete inventory snapshot");
                                    if(!isHost_){setPhase(NET_FAILED);InterlockedExchange(&stopFlag_,1);}
                                    disconnectNative(ev.connection,NATIVE_REJECT_PROTOCOL,"inventory snapshot assembly rejected");
                                }
                            }
                        }
                    } else if (type == PKT_WORLD_ITEM) {
                        const unsigned len = (unsigned)payloadSize;
                        if (len >= sizeof(WorldItemSnapshotHeader) && inbound_) {
                            WorldItemSnapshotHeader hdr;
                            std::memcpy(&hdr, payload, sizeof(hdr));
                            unsigned need = sizeof(WorldItemSnapshotHeader)
                                          + (unsigned)hdr.count * sizeof(WorldItemEntry);
                            if (len >= need) {
                                const u8* p = payload + sizeof(WorldItemSnapshotHeader);
                                const WorldItemEntry* items =
                                    (hdr.count > 0) ? reinterpret_cast<const WorldItemEntry*>(p) : 0;
                                inbound_->pushWorldItems(hdr.ownerId, items, hdr.count);
                            }
                        }
                    } else if (type == PKT_WORLD_ITEM_REMOVE) {
                        const unsigned len = (unsigned)payloadSize;
                        if (len >= sizeof(WorldItemRemoveHeader) && inbound_) {
                            WorldItemRemoveHeader hdr;
                            std::memcpy(&hdr, payload, sizeof(hdr));
                            unsigned need = sizeof(WorldItemRemoveHeader)
                                          + (unsigned)hdr.count * sizeof(u32);
                            if (len >= need) {
                                const u8* p = payload + sizeof(WorldItemRemoveHeader);
                                const u32* netIds =
                                    (hdr.count > 0) ? reinterpret_cast<const u32*>(p) : 0;
                                inbound_->pushWorldRemove(hdr.ownerId, netIds, hdr.count);
                            }
                        }
                    } else if (type == PKT_WORLD_ITEM_CLAIM) {
                        const unsigned len = (unsigned)payloadSize;
                        if (len >= sizeof(WorldItemClaimHeader) && inbound_) {
                            WorldItemClaimHeader hdr;
                            std::memcpy(&hdr, payload, sizeof(hdr));
                            unsigned need = sizeof(WorldItemClaimHeader)
                                          + (unsigned)hdr.count * sizeof(u32);
                            if (len >= need) {
                                const u8* p = payload + sizeof(WorldItemClaimHeader);
                                const u32* netIds =
                                    (hdr.count > 0) ? reinterpret_cast<const u32*>(p) : 0;
                                inbound_->pushWorldClaim(hdr.ownerId, hdr.authorId,
                                                         netIds, hdr.count);
                            }
                        }
                    } else if (type == PKT_NPC_CENSUS) {
                        const unsigned len = (unsigned)payloadSize;
                        if (len >= sizeof(NpcCensusHeader) && inbound_) {
                            NpcCensusHeader hdr;
                            std::memcpy(&hdr, payload, sizeof(hdr));
                            unsigned need = sizeof(NpcCensusHeader)
                                          + (unsigned)hdr.count * 5 * sizeof(u32)
                                          + (unsigned)hdr.count * 3 * sizeof(float);
                            if (len >= need && hdr.count <= NPC_CENSUS_MAX) {
                                const u8* p = payload + sizeof(NpcCensusHeader);
                                const u32* hands =
                                    (hdr.count > 0) ? reinterpret_cast<const u32*>(p) : 0;
                                const float* pos = (hdr.count > 0)
                                    ? reinterpret_cast<const float*>(
                                          p + (unsigned)hdr.count * 5 * sizeof(u32))
                                    : 0;
                                inbound_->pushNpcCensus(hdr.ownerId, hands, pos, hdr.count);
                            }
                        }
                    } else if (type == PKT_WORLD_DROP) {
                        WorldDropPacket wdp;
                        if (readPacket(payload, (unsigned)payloadSize, &wdp)
                            && inbound_) {
                            inbound_->pushWorldDrop(wdp.ownerId, wdp);
                        }
                    } else if (type == PKT_WORLD_PICKUP) {
                        WorldPickupPacket wpp;
                        if (readPacket(payload, (unsigned)payloadSize, &wpp)
                            && inbound_) {
                            inbound_->pushWorldPickup(wpp.ownerId, wpp);
                        }
                    } else if (type == PKT_INV_XFER) {
                        InvXferPacket ixp;
                        if (readPacket(payload, (unsigned)payloadSize, &ixp)
                            && ixp.contentsCount <= INV_ITEMS_MAX &&
                            ixp.contentsPresent <= 1 &&
                            (ixp.contentsPresent || ixp.contentsCount == 0) &&
                            payloadSize == sizeof(ixp) + ixp.contentsCount * sizeof(InvItemEntry)
                            && inbound_) {
                            const InvItemEntry* contents = reinterpret_cast<const InvItemEntry*>(
                                payload + sizeof(ixp));
                            inbound_->pushInvXfer(ixp.ownerId, ixp, contents, ixp.contentsCount);
                        }
                    } else if (type == PKT_INV_XFER_ACK) {
                        InvXferAckPacket iap;
                        if (readPacket(payload, (unsigned)payloadSize, &iap)
                            && iap.contentsCount <= INV_ITEMS_MAX &&
                            iap.contentsPresent <= 1 &&
                            (iap.contentsPresent || iap.contentsCount == 0) &&
                            payloadSize == sizeof(iap) + iap.contentsCount * sizeof(InvItemEntry)
                            && inbound_) {
                            const InvItemEntry* contents = reinterpret_cast<const InvItemEntry*>(
                                payload + sizeof(iap));
                            inbound_->pushInvXferAck(iap.ownerId, iap, contents, iap.contentsCount);
                        }
                    } else if (type == PKT_MEDICAL) {
                        MedicalPacket mp;
                        if (readPacket(payload, (unsigned)payloadSize, &mp)
                            && inbound_) {
                            inbound_->pushMedical(mp.ownerId, mp);
                        }
                    } else if (type == PKT_TREATMENT) {
                        TreatmentPacket tp;
                        if (readPacket(payload, (unsigned)payloadSize, &tp)
                            && inbound_) {
                            inbound_->pushTreatment(tp.ownerId, tp);
                        }
                    } else if (type == PKT_COMBAT_HIT) {
                        CombatHitPacket chp;
                        if (readPacket(payload, (unsigned)payloadSize, &chp)
                            && inbound_) {
                            inbound_->pushCombatHit(chp.ownerId, chp);
                        }
                    } else if (type == PKT_SPEED_REQ || type == PKT_SPEED_SET) {
                        SpeedPacket sp;
                        if (readPacket(payload, (unsigned)payloadSize, &sp)
                            && inbound_) {
                            inbound_->pushSpeed(sp.ownerId, sp);
                        }
                    } else if (type == PKT_STATS) {
                        StatsPacket stp;
                        if (readPacket(payload, (unsigned)payloadSize, &stp)
                            && inbound_) {
                            inbound_->pushStats(stp.ownerId, stp);
                        }
                    } else if (type == PKT_MONEY) {
                        MoneyPacket mo;
                        if (readPacket(payload, (unsigned)payloadSize, &mo)
                            && inbound_) {
                            inbound_->pushMoney(mo.ownerId, mo);
                        }
                    } else if (type == PKT_MONEY_DELTA) {
                        MoneyDeltaPacket md;
                        if (readPacket(payload, (unsigned)payloadSize, &md)
                            && inbound_) {
                            inbound_->pushMoneyDelta(md.ownerId, md);
                        }
                    } else if (type == PKT_FACTION) {
                        FactionPacket fa;
                        if (readPacket(payload, (unsigned)payloadSize, &fa)
                            && inbound_) {
                            inbound_->pushFaction(fa.ownerId, fa);
                        }
                    } else if (type == PKT_TIME) {
                        TimePacket ti;
                        if (readPacket(payload, (unsigned)payloadSize, &ti)
                            && inbound_) {
                            inbound_->pushTime(ti.ownerId, ti);
                        }
                    } else if (type == PKT_DOOR) {
                        DoorPacket dp;
                        if (readPacket(payload, (unsigned)payloadSize, &dp)
                            && inbound_) {
                            inbound_->pushDoor(dp.ownerId, dp);
                        }
                    } else if (type == PKT_PROD) {
                        ProdPacket pp;
                        if (readPacket(payload, (unsigned)payloadSize, &pp)
                            && inbound_) {
                            inbound_->pushProd(pp.ownerId, pp);
                        }
                    } else if (type == PKT_RESEARCH) {
                        ResearchPacket rp;
                        if (readPacket(payload, (unsigned)payloadSize, &rp)
                            && inbound_) {
                            inbound_->pushResearch(rp.ownerId, rp);
                        }
                    } else if (type == PKT_DEED) {
                        DeedPacket dep;
                        if (readPacket(payload, (unsigned)payloadSize, &dep)
                            && inbound_) {
                            inbound_->pushDeed(dep.ownerId, dep);
                        }
                    } else if (!isHost_ && type == PKT_PLAYER_ROSTER) {
                        RosterPacket rp;
                        if (readPacket(payload, (unsigned)payloadSize, &rp)) {
                            u32 me = (u32)InterlockedCompareExchange(&myId_, 0, 0);
                            u64 rosterInc[MAX_PLAYERS];memcpy(rosterInc,payload+sizeof(RosterPacket)+1,sizeof(rosterInc));
                            for(u32 oi=0;oi<MAX_PLAYERS;++oi)if(ownerIncarnation_[oi]!=rosterInc[oi]) {
                                for(std::map<StateKey,u64>::iterator si=stateSeen_.begin();si!=stateSeen_.end();) {if(si->first.owner==oi)stateSeen_.erase(si++);else ++si;}
                                ownerIncarnation_[oi]=rosterInc[oi];
                                if(inbound_)inbound_->flushOwnerWorld(oi);
                                for(std::deque<Delayed>::iterator di=delayed_.begin();di!=delayed_.end();) {if(di->ownerId==oi)di=delayed_.erase(di);else ++di;}
                            }
                            const bool hasMembers = payloadSize > sizeof(RosterPacket);
                            const u32 members = hasMembers
                                ? payload[sizeof(RosterPacket)] & ((1u << MAX_PLAYERS) - 1u)
                                : 0;
                            for (u32 i = 0; i < MAX_PLAYERS; ++i) {
                                if (i == me) continue;
                                rp.name[i][HELLO_NAME_MAX] = '\0';
                                std::string parsed;
                                bool ok = parsePlayerNick(rp.name[i], parsed);
                                NativeLock nativeLock1064_nameCs_(&nameCs_);
                                bool changed = strcmp(peerName_[i],
                                                      ok ? parsed.c_str() : "") != 0;
                                memset(peerName_[i], 0, sizeof(peerName_[i]));
                                if (ok) memcpy(peerName_[i], parsed.c_str(), parsed.size());
                                nativeLock1064_nameCs_.unlock();
                                if (i != 0 && linkWelcomed)
                                    setMemberBit(i, hasMembers ? (members & (1u << i)) != 0 : ok);
                                if (changed && ok) {
                                    char lb[96];
                                    _snprintf(lb, sizeof(lb) - 1, "roster id=%u '%s'",
                                              (unsigned)i, parsed.c_str());
                                    lb[sizeof(lb) - 1] = '\0';
                                    netLog(lb);
                                }
                            }
                        }
                    } else if (type == PKT_FIXTURE) {
                        FixturePacket fxp;
                        if (readPacket(payload, (unsigned)payloadSize, &fxp)
                            && inbound_) {
                            inbound_->pushFixture(fxp.ownerId, fxp);
                        }
                    } else if (type == PKT_PROCEDURAL) {
                        ProceduralPacket proc;memcpy(&proc,payload,sizeof(proc));
                        if(inbound_)inbound_->pushProcedural(frame.origin,proc);
                    } else if (type == PKT_ENVIRONMENT_ITEMS) {
                        EnvironmentItemPacket packet;memcpy(&packet,payload,sizeof(packet));
                        if(inbound_)inbound_->pushEnvironment(frame.origin,packet);
                    } else if (type == PKT_BUILD_PLACE) {
                        BuildPlacePacket bp;
                        if (readPacket(payload, (unsigned)payloadSize, &bp)
                            && inbound_) {
                            inbound_->pushBuildPlace(bp.ownerId, bp);
                        }
                    } else if (type == PKT_BUILD_STATE) {
                        BuildStatePacket bs;
                        if (readPacket(payload, (unsigned)payloadSize, &bs)
                            && inbound_) {
                            inbound_->pushBuildState(bs.ownerId, bs);
                        }
                    } else if (type == PKT_BUILD_DOOR) {
                        BuildDoorPacket bd;
                        if (readPacket(payload, (unsigned)payloadSize, &bd)
                            && inbound_) {
                            inbound_->pushBuildDoor(bd.ownerId, bd);
                        }
                    } else if (type == PKT_BUILD_REMOVE) {
                        BuildRemovePacket br;
                        if (readPacket(payload, (unsigned)payloadSize, &br)
                            && inbound_) {
                            inbound_->pushBuildRemove(br.ownerId, br);
                        }
                    } else if (type == PKT_STEALTH) {
                        StealthPacket slp;
                        if (readPacket(payload, (unsigned)payloadSize, &slp)
                            && inbound_) {
                            if(acceptState(slp.ownerId,PKT_STEALTH,frame.originSequence,&slp,sizeof(slp)))inbound_->pushStealth(slp.ownerId, slp);
                        }
                    } else if (type == PKT_SPAWN_REQ) {
                        SpawnReqPacket srp;
                        if (readPacket(payload, (unsigned)payloadSize, &srp)
                            && inbound_ && srp.targetOwner == (u32)InterlockedCompareExchange(&myId_, 0, 0)) {
                            inbound_->pushSpawnReq(srp.ownerId, srp);
                        }
                    } else if (type == PKT_SPAWN_INFO) {
                        SpawnInfoPacket sip;
                        if (readPacket(payload, (unsigned)payloadSize, &sip)
                            && inbound_ && sip.requestorId == (u32)InterlockedCompareExchange(&myId_, 0, 0)) {
                            sip.shopSquadSid[sizeof(sip.shopSquadSid)-1]=0;
                            inbound_->pushSpawnInfo(sip.ownerId, sip);
                        }
                    } else if (type == PKT_SAVE_REQ) {
                        SaveReqPacket sq;
                        if (readPacket(payload, (unsigned)payloadSize, &sq)
                            && inbound_) {
                            inbound_->pushSaveReq(sq.ownerId, sq);
                        }
                    } else if (type == PKT_SAVE_BEGIN) {
                        SaveBeginPacket sb;
                        if (readPacket(payload, (unsigned)payloadSize, &sb)
                            && inbound_) {
                            inbound_->pushSaveBegin(sb.ownerId, sb);
                        }
                    } else if (type == PKT_SAVE_FILE) {
                        const unsigned len = (unsigned)payloadSize;
                        if (len >= sizeof(SaveFileHeader) && inbound_) {
                            SaveFileHeader hdr;
                            std::memcpy(&hdr, payload, sizeof(hdr));
                            unsigned need = sizeof(SaveFileHeader)
                                          + (unsigned)hdr.pathLen + (unsigned)hdr.dataLen;
                            if (len >= need && hdr.pathLen > 0 &&
                                hdr.pathLen <= SAVE_PATH_MAX &&
                                hdr.dataLen <= SAVE_CHUNK_MAX) {
                                const u8* p = payload + sizeof(SaveFileHeader);
                                inbound_->pushSaveFile(hdr.ownerId, hdr,
                                                       (const char*)p,
                                                       (const u8*)p + hdr.pathLen);
                            }
                        }
                    } else if (type == PKT_SAVE_DONE) {
                        const unsigned len = (unsigned)payloadSize;
                        if (len >= sizeof(SaveDoneHeader) && inbound_) {
                            SaveDoneHeader hdr;
                            std::memcpy(&hdr, payload, sizeof(hdr));
                            unsigned need = sizeof(SaveDoneHeader)
                                          + (unsigned)hdr.fileCount * sizeof(u32);
                            if (len >= need) {
                                const u8* p = payload + sizeof(SaveDoneHeader);
                                inbound_->pushSaveDone(hdr.ownerId, hdr,
                                                       (hdr.fileCount > 0)
                                                           ? (const u32*)p : 0);
                            }
                        }
                    } else if (type == PKT_SAVE_ACK) {
                        SaveAckPacket sa;
                        if (readPacket(payload, (unsigned)payloadSize, &sa)
                            && inbound_) {
                            inbound_->pushSaveAck(sa.ownerId, sa);
                        }
                    } else if (type == PKT_LOAD_GO) {
                        LoadGoPacket lg;
                        if (readPacket(payload, (unsigned)payloadSize, &lg)
                            && inbound_) {
                            worldToken_=lg.worldToken;latestLoadId_=lg.loadId;latestLoadWorld_=lg.worldToken;localReady_=false;peer.ready=false;
                            InterlockedIncrement(&sendEpoch_);clearWorldPublication();stateSeen_.clear();delayed_.clear();
                            inbound_->flushWorldState();
                            inbound_->pushLoadGo(lg.ownerId, lg);
                        }
                    } else if (type == PKT_LOAD_REQ) {
                        LoadReqPacket lr;
                        if (readPacket(payload, (unsigned)payloadSize, &lr)
                            && inbound_) {
                            inbound_->pushLoadReq(lr.ownerId, lr);
                        }
                    } else if (type == PKT_LOAD_NACK) {
                        LoadNackPacket ln;
                        if (readPacket(payload, (unsigned)payloadSize, &ln)
                            && inbound_) {
                            inbound_->pushLoadNack(ln.ownerId, ln);
                        }
                    } else if (type == PKT_CAM_HINT) {
                        CamHintPacket chp;
                        if (readPacket(payload, (unsigned)payloadSize, &chp)
                            && inbound_) {
                            if(acceptState(chp.ownerId,PKT_CAM_HINT,frame.originSequence,&chp,sizeof(chp)))inbound_->pushCamHint(chp.ownerId, chp);
                        }
                    } else if (type == PKT_CELL_CLAIM) {
                        CellClaimPacket ccp;
                        if (readPacket(payload, (unsigned)payloadSize, &ccp)
                            && inbound_) {
                            inbound_->pushCellClaim(ccp.ownerId, ccp);
                        }
                    } else if (type == PKT_TIME_PING) {
                        TimePingPacket tp;
                        if (isHost_ && readPacket(payload, (unsigned)payloadSize, &tp)) {
                            TimePongPacket po;
                            po.type = (u8)PKT_TIME_PONG;
                            po.nonce = tp.nonce;
                            po.echoWallMs = tp.senderWallMs;
                            po.responderWallMs = (u32)coop::wallClockMs();
                            sendNative(ev.connection,CH_UNRELIABLE,&po,sizeof(po));
                        }
                    } else if (type == PKT_TIME_PONG) {
                        TimePongPacket po;
                        if (!isHost_ && readPacket(payload, (unsigned)payloadSize, &po)) {
                            unsigned long t1 = coop::wallClockMs();
                            unsigned long t0 = (unsigned long)po.echoWallMs;
                            if (t1 >= t0) { // skip the (rare) sample spanning local midnight
                                unsigned long rtt = t1 - t0;
                                long offset = (long)po.responderWallMs + (long)(rtt / 2ul) - (long)t1;
                                while (offset >= HALF_DAY_MS)  offset -= 2l * HALF_DAY_MS;
                                while (offset < -HALF_DAY_MS)  offset += 2l * HALF_DAY_MS;
                                ++syncSamples;
                                if (rtt <= bestRttMs) { bestRttMs = rtt; bestOffsetMs = offset; }
                            }
                        }
                    }

                        }
                        if(isHost_ && peer.admitted && shouldRelayType(type,payload,payloadSize))broadcastNative(ev.traffic,payload,payloadSize,ev.connection,frame.origin,&frame);
                    }
                    worldLock.unlock();
                }
                nativeLock624_outCs_.unlock();
            }else if(ev.type==COOP_NET_EVENT_DISCONNECTED) {
                NativeLock nativeLock1244_outCs_(&outCs_);std::map<u32,Peer>::iterator i=peers_.find(ev.connection);
                if(i!=peers_.end())i->second.closed=true;
                else if(!isHost_ && !serverConnection_ && nativeAttemptActive) {
                    nativeAttemptActive=false;attemptEndedAt=GetTickCount();
                    setError(NET_ERR_NO_RESPONSE,connectAttempts,"native connection ended before game welcome");
                }
                nativeLock1244_outCs_.unlock();
            }else if(ev.type==COOP_NET_EVENT_ERROR) {
                setError(ev.reason==COOP_NET_AUTH_FAILED?NET_ERR_PROVIDER_AUTH:NET_ERR_PROVIDER_POLL,ev.reason,"native provider stopped the session");
                setPhase(NET_FAILED);InterlockedExchange(&stopFlag_,1);
            }
            eventScope.release();
        }
        if(stopFlag_)break;
        NativeLock nativeLock1278_outCs_(&outCs_);
        for(std::map<u32,Peer>::iterator i=peers_.begin();i!=peers_.end();++i)if(!i->second.closed && !i->second.admitted && GetTickCount()-i->second.connectedAt>NATIVE_CONNECT_TIMEOUT_MS)
            disconnectNative(i->first,NATIVE_REJECT_TIMEOUT,"game admission timed out");
        // No event loans or fanout/pending iterators remain at this point.
        for(std::map<u32,Peer>::iterator i=peers_.begin();i!=peers_.end();) {
            if(!i->second.closed){++i;continue;}
            const Peer p=i->second;peers_.erase(i++);
            for(unsigned lane=0;lane<3;++lane){std::deque<Pending>& q=pending_[lane][p.connection];for(size_t n=0;n<q.size();++n)pendingBytes_[lane]-=(unsigned)q[n].bytes.size();pending_[lane].erase(p.connection);}
            if(isHost_) {
                if(p.admitted && p.playerId) {
                    ownerIncarnation_[p.playerId]=0;
                    if(inbound_)inbound_->flushOwnerWorld(p.playerId);
                    for(std::map<StateKey,u64>::iterator si=stateSeen_.begin();si!=stateSeen_.end();) {if(si->first.owner==p.playerId)stateSeen_.erase(si++);else ++si;}
                    for(std::deque<Delayed>::iterator di=delayed_.begin();di!=delayed_.end();) {if(di->ownerId==p.playerId)di=delayed_.erase(di);else ++di;}
                    NativeLock nativeLock1254_nameCs_(&nameCs_);memset(peerName_[p.playerId],0,sizeof(peerName_[p.playerId]));nativeLock1254_nameCs_.unlock();
                    setMemberBit(p.playerId,false);rosterChanged=true;if(inbound_)inbound_->pushLeave(p.playerId);
                    sendRoster();
#ifdef KENSHICOOP_NET_DIAG
                    coop::logRemoteClose(p.playerId);mirrorFault[p.playerId]=false;
#endif
                }
            }else {
                serverConnection_=0;InterlockedExchange(&myId_,0);bumpSessionEpoch(false);stateSeen_.clear();delayed_.clear();
                NativeLock nativeLock1263_nameCs_(&nameCs_);memset(peerName_,0,sizeof(peerName_));nativeLock1263_nameCs_.unlock();
                if(inbound_)inbound_->pushLeave(OWNER_ID_ALL);
                if(!linkWelcomed)setError(linkUp?NET_ERR_HANDSHAKE_DROPPED:NET_ERR_NO_RESPONSE,connectAttempts,"native connection ended before game welcome");
                linkUp=linkWelcomed=false;nativeAttemptActive=false;attemptEndedAt=GetTickCount();
                setMembers(0);setPhase(everWelcomed?NET_RECONNECTING:NET_CONNECTING);
#ifdef KENSHICOOP_NET_DIAG
                coop::logMirrorCapture(false);
#endif
            }
        }
        nativeLock1278_outCs_.unlock();
        if(stopFlag_)break;
        sampleNativeStats();flushDelayed();

        // Native probes retain the existing wall-clock minimum-RTT filter.
        if(!isHost_ && serverConnection_) {
            DWORD now=GetTickCount();
            if(lastTimePing==0 || now-lastTimePing>=2000) {
                lastTimePing=now;TimePingPacket p;p.type=PKT_TIME_PING;p.nonce=pingNonce++;p.senderWallMs=(u32)coop::wallClockMs();
                NativeLock nativeLock1289_outCs_(&outCs_);sendNative(serverConnection_,GAME_STATE,&p,sizeof(p));nativeLock1289_outCs_.unlock();
            }
            if(syncSamples && (!lastClockLog || now-lastClockLog>=5000)) {
                lastClockLog=now;char b[96];_snprintf(b,sizeof(b)-1,"CLOCKSYNC offset=%ld rtt=%lu n=%u",bestOffsetMs,bestRttMs,syncSamples);b[sizeof(b)-1]=0;netLog(b);
            }
        }
#ifdef KENSHICOOP_NET_DIAG
        DWORD mirrorNow=GetTickCount();if(mirrorNow-mirrorWindow>=1000){mirrorWindow=mirrorNow;mirrorSent=0;}
        if(!isHost_ && myId_>0 && serverConnection_)for(;mirrorSent<8;++mirrorSent) {
            NetDebugStats ds;copyDebugStats(&ds);if(ds.pendingBulkBytes>=65536)break;
            char chunk[DEBUG_LOG_CHUNK_MAX];unsigned __int64 offset=0;unsigned n=coop::logMirrorPeek(chunk,sizeof(chunk),&offset);if(!n)break;
            DebugLogFileHeader h;h.type=PKT_DEBUG_LOG_FILE;h.offset=offset;h.byteCount=(u16)n;std::vector<u8> v=payloadTail(h,(const u8*)chunk,n);
            NativeLock nativeLock1301_outCs_(&outCs_);bool sent=sendNative(serverConnection_,SAVE_BULK,&v[0],(unsigned)v.size());nativeLock1301_outCs_.unlock();if(!sent)break;coop::logMirrorCommit(n);
        }
#endif
        // A single capture FIFO retains ordering of all typed event and bulk markers.
        std::deque<Publication> pub;
        NativeLock nativeLock1306_outCs_(&outCs_);pub.swap(publications_);publicationBytes_=0;nativeLock1306_outCs_.unlock();
        for(size_t i=0;i<pub.size() && !stopFlag_;++i) {
            NativeLock nativeLock1308_outCs_(&outCs_);const bool world=!nativeSessionType(pub[i].bytes[0]);
            if((!world && pub[i].bytes[0]!=PKT_PROCEDURAL &&
                pub[i].bytes[0]!=PKT_ENVIRONMENT_ITEMS) || pub[i].epoch==(u32)sendEpoch_) {
                const unsigned lane=nativeLane(pub[i].bytes[0]);
                NativeFrame capture;memset(&capture,0,sizeof(capture));capture.originIncarnation=incarnation_;capture.originSequence=pub[i].sequence;capture.captureMs=pub[i].stampMs;
                capture.originEpoch=pub[i].epoch;
                if(isHost_)broadcastNative(lane,&pub[i].bytes[0],(unsigned)pub[i].bytes.size(),0,OWNER_ID_ALL,&capture);
                else if(serverConnection_)sendNative(serverConnection_,lane,&pub[i].bytes[0],(unsigned)pub[i].bytes.size(),OWNER_ID_ALL,&capture);
            }
            nativeLock1308_outCs_.unlock();
        }
        DWORD rosterNow=GetTickCount();
        if(isHost_ && (rosterChanged || rosterNow-rosterAt>=1000)) {
            rosterAt=rosterNow;RosterPacket p;memset(&p,0,sizeof(p));p.type=PKT_PLAYER_ROSTER;
            NativeLock nativeLock1320_nameCs_(&nameCs_);memcpy(p.name[0],localName_,sizeof(localName_));for(u32 i=1;i<MAX_PLAYERS;++i)memcpy(p.name[i],peerName_[i],sizeof(peerName_[i]));nativeLock1320_nameCs_.unlock();
            NetStatus s;copyStatus(&s);unsigned char bytes[sizeof(RosterPacket)+1+MAX_PLAYERS*sizeof(u64)];memcpy(bytes,&p,sizeof(p));bytes[sizeof(p)]=(u8)s.memberMask;
            NativeLock nativeLock1322_outCs_(&outCs_);ownerIncarnation_[0]=incarnation_;memcpy(bytes+sizeof(p)+1,ownerIncarnation_,sizeof(ownerIncarnation_));
            if(rosterChanged || memcmp(bytes,lastRoster,sizeof(bytes))) {broadcastNative(GAME_RELIABLE,bytes,sizeof(bytes));memcpy(lastRoster,bytes,sizeof(bytes));}
            nativeLock1322_outCs_.unlock();rosterChanged=false;
        }
        bool connected=false;NativeLock nativeLock1326_outCs_(&outCs_);
        for(std::map<u32,Peer>::iterator i=peers_.begin();i!=peers_.end();++i)if(i->second.admitted && i->second.ready){connected=true;break;}
        DWORD now=connected?monoMs():0;
        if(connected && (forceEntitySend || !haveEntitySendTick || now-lastEntitySendTick>=ENTITY_SEND_MS)) {
            if(haveOut_ && outEpoch_==(u32)sendEpoch_ && !out_.empty()) {
                // Epoch/time and entity bytes came from one locked publication.
                const std::vector<EntityState>& ents=out_;u32 owner=outOwner_,stamp=outStampMs_,epoch=outEpoch_;
                NativeFrame capture;memset(&capture,0,sizeof(capture));capture.originIncarnation=incarnation_;capture.originSequence=outSequence_;capture.captureMs=stamp;
                capture.originEpoch=epoch;
                const unsigned nativeCap=(COOP_NET_MAX_STATE_MESSAGE-sizeof(NativeFrame)-sizeof(EntityBatchHeader))/sizeof(EntityState);
                const unsigned batchCap=nativeCap<ENTITY_BATCH_MAX?nativeCap:ENTITY_BATCH_MAX;
                // A congested unreliable lane may admit only the first chunks.
                // Rotate their order so the tail entities cannot starve.
                const size_t batchCount=(ents.size()+batchCap-1)/batchCap;
                const size_t firstBatch=nextEntityBatch++%batchCount;
                for(size_t batch=0;batch<batchCount;++batch) {
                    const size_t off=((firstBatch+batch)%batchCount)*batchCap;
                    unsigned count=(unsigned)(ents.size()-off);if(count>batchCap)count=batchCap;
                    EntityBatchHeader h;h.type=PKT_ENTITY_BATCH;h.ownerId=owner;h.count=(u8)count;h.sendMs=stamp;h.epoch=epoch;
                    u8 bytes[sizeof(EntityBatchHeader)+ENTITY_BATCH_MAX*sizeof(EntityState)];
                    const unsigned byteCount=sizeof(h)+count*sizeof(EntityState);memcpy(bytes,&h,sizeof(h));memcpy(bytes+sizeof(h),&ents[off],count*sizeof(EntityState));
                    if(isHost_)broadcastNative(GAME_STATE,bytes,byteCount,0,OWNER_ID_ALL,&capture);else if(serverConnection_)sendNative(serverConnection_,GAME_STATE,bytes,byteCount,OWNER_ID_ALL,&capture);
                }
                lastEntitySendTick=now;haveEntitySendTick=true;forceEntitySend=false;
            }
        }
        nativeLock1326_outCs_.unlock();Sleep(TICK_MS);
    }
#ifdef KENSHICOOP_NET_DIAG
    if(isHost_)for(unsigned id=1;id<=MAX_JOINS;++id)coop::logRemoteClose(id);else coop::logMirrorCapture(false);
#endif
    NativeLock nativeLock1351_outCs_(&outCs_);peers_.clear();serverConnection_=0;InterlockedExchange(&myId_,0);stateSeen_.clear();delayed_.clear();memset(ownerIncarnation_,0,sizeof(ownerIncarnation_));for(unsigned lane=0;lane<3;++lane){pending_[lane].clear();pendingBytes_[lane]=0;}nativeLock1351_outCs_.unlock();
    closeProvider();setMembers(0);setHandshakeCounts(connectAttempts,0);setActiveTransport(NET_TRANSPORT_NONE);clearDebugStats();InterlockedExchange(&running_,0);
}
} // namespace coop
