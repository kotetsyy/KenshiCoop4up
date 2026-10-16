// Native companion contexts belong exclusively to the network worker.
#ifndef KENSHICOOP_NETLINK_H
#define KENSHICOOP_NETLINK_H
#include <windows.h>
#include <string>
#include <vector>
#include <deque>
#include <map>
#include "NativeFrame.h"
#include "../core/Inbound.h"
namespace coop {
struct NetDebugPeer {
    DWORD slot,playerId,rttMs;
    DWORD pendingReliableBytes,pendingStateBytes,pendingBulkBytes,unackedReliableBytes;
};
struct NetDebugStats {
    DWORD sampleTickMs,sentBytesPerSec,recvBytesPerSec,sentPacketsPerSec,recvPacketsPerSec;
    DWORD connectedPeers,maxRttMs;
    DWORD pendingReliableBytes,pendingStateBytes,pendingBulkBytes,unackedReliableBytes;
    DWORD peerStatsCount;
    NetDebugPeer peers[MAX_PLAYERS];
};
enum NetPhase { NET_IDLE,NET_STARTING,NET_LISTENING,NET_CONNECTING,NET_HANDSHAKING,NET_CONNECTED,NET_RECONNECTING,NET_FAILED };
enum NetTransport { NET_TRANSPORT_NONE=-1,NET_TRANSPORT_STEAM=0,NET_TRANSPORT_UDP=1 };
enum NetError {
    NET_ERR_NONE,NET_ERR_PROVIDER_LOAD,NET_ERR_PROVIDER_ABI,NET_ERR_PROVIDER_START,
    NET_ERR_PROVIDER_AUTH,NET_ERR_PROVIDER_POLL,NET_ERR_QUEUE_FULL,NET_ERR_THREAD,
    NET_ERR_NO_RESPONSE,NET_ERR_HANDSHAKE_DROPPED,NET_ERR_PROTOCOL,NET_ERR_PEER_VERSION,NET_ERR_SERVER_FULL
};
struct NetStartOptions {
    bool isHost;
    NetTransport transport;
    int port;
    std::string address;
    u64 remoteSteamId;
    NetStartOptions();
};
struct NetStatus {
    NetPhase phase;
    bool host;
    NetTransport requested,active;
    NetError error;
    u32 errorArg;
    char errorRaw[128];
    DWORD phaseSinceTick;
    u32 connectAttempts,pendingHandshakes,memberMask;
    u64 sessionGeneration,selfSteamId;
    int authentication,relayAvailability;
};
class NetLink {
public:
    NetLink();
    ~NetLink();
    bool start(const NetStartOptions&,Inbound*);
    void stop();
    bool pumpMainThread();
    void copyStatus(NetStatus* out) const;
    void setOwnedEntities(u32 ownerId, const EntityState* arr, unsigned int count);
    void queueEvent(const EventPacket& ev);
    void queueInvSnapshot(u32 ownerId, u8 keyKind, const u32 cKey[5],
                          const InvItemEntry* items, unsigned int count, u8 flags = 0,
                          u32 revision = 0);
    void queueWorldItems(u32 ownerId, const WorldItemEntry* items, unsigned int count);
    void queueWorldRemove(u32 ownerId, const u32* netIds, unsigned int count);
    void queueWorldClaim(u32 ownerId, u32 authorId, const u32* netIds, unsigned int count);
    void queueNpcCensus(u32 ownerId, const u32* hands, const float* pos,
                        unsigned int count);
    void queueWorldDrop(const WorldDropPacket& pkt);
    void queueWorldPickup(const WorldPickupPacket& pkt);
    void queueInvXfer(const InvXferPacket& pkt, std::vector<InvItemEntry>& contents);
    void queueInvXferAck(const InvXferAckPacket& pkt,
                        const std::vector<InvItemEntry>& contents,
                        const InvSnapshotHeader& snapshot, const InvItemEntry* items);
    void queueMedical(const MedicalPacket& pkt);
    void queueTreatment(const TreatmentPacket& pkt);
    void queueCombatHit(const CombatHitPacket& pkt);
    void queueSpeed(const SpeedPacket& pkt);
    void queueStats(const StatsPacket& pkt);
    void queueMoney(const MoneyPacket& pkt);
    void queueMoneyDelta(const MoneyDeltaPacket& pkt);
    void queueFaction(const FactionPacket& pkt);
    void queueTime(const TimePacket& pkt);
    void queueDoor(const DoorPacket& pkt);
    void queueProd(const ProdPacket& pkt);
    void queueResearch(const ResearchPacket& pkt);
    void queueDeed(const DeedPacket& pkt);
    void queueFixture(const FixturePacket& pkt);
    void queueBuildPlace(const BuildPlacePacket& pkt);
    void queueBuildState(const BuildStatePacket& pkt);
    void queueBuildDoor(const BuildDoorPacket& pkt);
    void queueBuildRemove(const BuildRemovePacket& pkt);
    bool queueProcedural(const ProceduralPacket& pkt);
    bool queueEnvironment(const EnvironmentItemPacket& pkt);
    u64 currentWorldToken() const;
    void queueStealth(const StealthPacket& pkt);
    void queueCamHint(const CamHintPacket& pkt);
    void queueCellClaim(const CellClaimPacket& pkt);
    void queueSpawnReq(const SpawnReqPacket& pkt);
    void queueSpawnInfo(const SpawnInfoPacket& pkt);
    void queueSaveReq(const SaveReqPacket& pkt);
    void queueSaveBegin(const SaveBeginPacket& pkt);
    void queueSaveFile(const SaveFileHeader& hdr, const char* relPath,
                       const unsigned char* data, unsigned int dataLen);
    void queueSaveDone(const SaveDoneHeader& hdr, const u32* crcs, unsigned int count);
    void queueSaveAck(const SaveAckPacket& pkt);
    void queueLoadGo(const LoadGoPacket& pkt);
    void queueLoadReq(const LoadReqPacket& pkt);
    void queueLoadNack(const LoadNackPacket& pkt);
    void setNetSim(unsigned int delayMs, unsigned int jitterMs, unsigned int lossPct);

    void setLocalName(const char* name);
    bool copyPeerName(u32 id, char* out, unsigned cap) const;
    void bumpSessionEpoch(bool rotateHostWorld = true);
    bool isRunning() const { return running_ != 0; }
    u32  localId()   const { return (u32)myId_; }
    void queueWorldReady(u32 completedLoadId);
    bool canQueueSaveBytes(unsigned bytes) const;

    // MAIN thread: copy one coherent, zeroed-when-stopped traffic sample.
    void copyDebugStats(NetDebugStats* out) const;

private:
    static DWORD WINAPI threadEntry(LPVOID self);
    void threadLoop();
    bool launchThread();
    void setPhase(NetPhase);
    void setActiveTransport(NetTransport);
    void setError(NetError,u32,const char*);
    void setMemberBit(u32,bool);
    void setMembers(u32);
    void setHandshakeCounts(u32,u32);
    void clearDebugStats();
    void failQueue();
    bool loadProvider();
    bool startProvider();
    void closeProvider();
    void sampleNativeStats();
    void sendRoster();
    bool queuePayload(const void*,unsigned);
    bool queuePayload(std::vector<u8>&);
    bool sendNative(u32,unsigned,const void*,unsigned,u32 origin=OWNER_ID_ALL,const NativeFrame* provenance=0);
    void broadcastNative(unsigned,const void*,unsigned,u32 except=0,u32 origin=OWNER_ID_ALL,const NativeFrame* provenance=0);
    void drainPending();
    void disconnectNative(u32,int,const char*);
    void clearWorldPublication();
    void deliverEntity(u32,u32,u64,const EntityState&);
    void flushDelayed();
    static bool shouldRelayType(u8, const void*, unsigned int);
    struct Peer {
        u32 connection,playerId;
        u64 steamId,incarnation;
        bool admitted,ready,closed;
        DWORD connectedAt;
        u32 expectedLoadId;
        u64 expectedWorld;
        NativeReplay replay[3];
        u64 sendSequence[3],captureSequence[3];
        Peer():connection(0),playerId(0),steamId(0),incarnation(0),admitted(false),ready(false),closed(false),connectedAt(0),expectedLoadId(0),expectedWorld(0) { memset(sendSequence,0,sizeof(sendSequence));memset(captureSequence,0,sizeof(captureSequence)); }
    };
    int sendFrame(Peer&,unsigned,std::vector<u8>&);
    bool currentReady(const WorldReadyPacket& p) const { return !isHost_ && localReady_ && p.loadId && p.loadId==latestLoadId_ && p.worldToken==latestLoadWorld_ && p.worldToken==worldToken_; }
    bool currentProcedural(const ProceduralPacket& p,const Peer* peer=0) const;
    bool currentEnvironment(const EnvironmentItemPacket& p,const Peer* peer=0) const;
    struct Publication { std::vector<u8> bytes; u32 epoch,stampMs; u64 sequence; };
    struct Pending { std::vector<u8> bytes; u32 epoch; bool world; };
    struct StateKey {
        u32 owner,type,hand[5];
        bool operator<(const StateKey& b) const { if(owner!=b.owner)return owner<b.owner; if(type!=b.type)return type<b.type; for(unsigned i=0;i<5;++i)if(hand[i]!=b.hand[i])return hand[i]<b.hand[i]; return false; }
    };
    bool acceptState(u32,u8,u64,const void*,unsigned);
    bool acceptEvent(const NativeFrame&,const EventPacket&);
    bool isHost_,steamMode_;
    int port_;
    std::string ip_;
    NetStartOptions options_;
    Inbound* inbound_;
    HMODULE providerModule_;
    CoopNetApi api_;
    void* context_;
    mutable CRITICAL_SECTION pumpCs_;
    DWORD mainThreadId_;
    u64 sessionGeneration_,providerGeneration_,incarnation_,sessionToken_,worldToken_;
    u32 latestLoadId_;
    u64 latestLoadWorld_;
    bool localReady_;
    std::map<u32,Peer> peers_;
    u32 serverConnection_;
    std::map<u32,std::deque<Pending> > pending_[3];
    unsigned pendingBytes_[3];
    std::map<StateKey,u64> stateSeen_;
    u64 ownerIncarnation_[MAX_PLAYERS];
    CRITICAL_SECTION outCs_;
    std::vector<u8> frameBytes_; // worker-only reusable native ABI send buffer
    std::vector<EntityState> out_;
    u32 outOwner_,outStampMs_,outEpoch_;
    u64 captureSequence_,outSequence_;
    bool haveOut_;
    std::deque<Publication> publications_;
    unsigned publicationBytes_;
    mutable CRITICAL_SECTION debugCs_,statusCs_,nameCs_;
    NetDebugStats debugStats_;
    NetStatus status_;
    HANDLE thread_;
    volatile LONG running_,stopFlag_,myId_,sendEpoch_;
    char localName_[64],peerName_[MAX_PLAYERS][64];
    unsigned simDelayMs_,simJitterMs_,simLossPct_;
    struct Delayed { DWORD releaseTick; u32 ownerId,sendMs,epoch,inboundGeneration; u64 sequence,incarnation; EntityState e; };
    std::deque<Delayed> delayed_;
    u32 receivedBytes_,receivedPackets_,sentBytes_,sentPackets_;
    DWORD lastStatsTick_;
    NetLink(const NetLink&);
    NetLink& operator=(const NetLink&);
};
}
#endif
