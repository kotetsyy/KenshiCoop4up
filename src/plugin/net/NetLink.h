// NetLink - owns ENet on a dedicated background thread.
//
// Threading contract:
//   * The net thread EXCLUSIVELY owns the ENetHost; the game thread never
//     touches ENet.
//   * Inbound events (peer connect/leave, received EntityState) are handed to
//     the game thread via the Inbound queue.
//   * The game thread publishes this peer's owned entities via setOwnedEntities();
//     the net thread reads the latest snapshot and transmits it each tick.
//
// VS2010 (v100) compatible: Win32 threads + CRITICAL_SECTION (no std::thread).

#ifndef KENSHICOOP_NETLINK_H
#define KENSHICOOP_NETLINK_H

#include <windows.h>
#include <string>
#include <vector>
#include <deque>
#include <map>
#include <enet/enet.h>

#include "../../netproto/Wire.h"
#include "../core/Inbound.h"

namespace coop {

// ENet UDP payload counters include ENet framing and retransmissions (not IP/UDP
// headers, not Steam tunnel overhead). Sampled ~1 Hz by the net thread in EVERY
// build (the F2 diagnostics report reads it); only the [net-diag] log lines and
// the private overlay are KENSHICOOP_NET_DIAG-only.
struct NetDebugPeer {
    DWORD slot;                   // ENet host peer slot (valid before handshake)
    DWORD playerId;               // 0 = host or not yet assigned by HELLO
    DWORD rttMs;
    DWORD reliableBytesInTransit;
};

struct NetDebugStats {
    DWORD sampleTickMs;           // zero until the first complete sample
    DWORD sentBytesPerSec;
    DWORD recvBytesPerSec;
    DWORD sentPacketsPerSec;
    DWORD recvPacketsPerSec;
    DWORD connectedPeers;
    DWORD maxRttMs;
    DWORD reliableBytesInTransit;
    DWORD peerStatsCount;
    NetDebugPeer peers[MAX_PLAYERS]; // fixed-size, no per-sample allocations
};

// ---- Lifecycle facts (MAIN thread reads a coherent copy via copyStatus) ------
// Written only by NetLink itself (start/stop on the MAIN thread, the worker on
// the NET thread) under one lock, so a reader never sees e.g. "UDP" from a start
// whose Steam tunnel is still open.
enum NetPhase {
    NET_IDLE,         // never started, or stop() completed
    NET_STARTING,     // start accepted (thread launched); transport not open yet
    NET_LISTENING,    // host: listen socket / Steam tunnel open, accepting joins
    NET_CONNECTING,   // join: connect attempt(s) in flight, no link established yet
    NET_HANDSHAKING,  // join: ENet link up, HELLO sent, awaiting WELCOME
    NET_CONNECTED,    // join: WELCOME accepted (player id assigned)
    NET_RECONNECTING, // join: an established session dropped; auto-retrying
    NET_FAILED        // worker gave up (see error); the thread has exited
};
// Numeric values match CoopUiSnapshot::activeTransport (0 Steam, 1 UDP).
enum NetTransport {
    NET_TRANSPORT_NONE  = -1,
    NET_TRANSPORT_STEAM = 0,
    NET_TRANSPORT_UDP   = 1
};
enum NetError {
    NET_ERR_NONE = 0,
    NET_ERR_ENET_INIT,          // enet_initialize failed (fatal)
    NET_ERR_THREAD,             // CreateThread failed (fatal)
    NET_ERR_STEAM_TUNNEL,       // Steam ENet socket hooks did not install (fatal)
    NET_ERR_BIND,               // host: listen socket did not open; arg = port (fatal)
    NET_ERR_SOCKET,             // join: local ENet host did not open (fatal)
    NET_ERR_RESOLVE,            // join: host address did not resolve (fatal on the
                                //   first attempt; retried after a lost session)
    NET_ERR_CONNECT_ALLOC,      // join: enet_host_connect returned no peer (retried)
    NET_ERR_NO_RESPONSE,        // join: attempt timed out before any ENet link;
                                //   arg = attempts so far (retried)
    NET_ERR_HANDSHAKE_DROPPED,  // join: host closed the link after CONNECT but
                                //   before WELCOME (host rejects exactly for a
                                //   protocol mismatch or a full session) (retried)
    NET_ERR_PROTOCOL,           // join: WELCOME carried another protocol; arg = host's
    NET_ERR_PEER_VERSION,       // host: rejected a join's HELLO; arg = its protocol
    NET_ERR_SERVER_FULL         // host: rejected a join, all MAX_JOINS ids in use
};
struct NetStatus {
    NetPhase     phase;
    bool         host;              // role of the current / last start
    NetTransport requested;         // transport asked for at the current / last start
    NetTransport active;            // transport ACTUALLY open (NONE when nothing is)
    NetError     error;             // last error of this start; NONE = none
    u32          errorArg;          // see NetError
    char         errorRaw[128];     // technical English detail ("" = none)
    DWORD        phaseSinceTick;    // GetTickCount when phase last changed
    u32          connectAttempts;   // join: enet_host_connect calls this start
    u32          pendingHandshakes; // host: ENet-linked peers not yet given an id
    // Accepted players by id bit (bit 0 = host). Host: itself + every join
    // whose HELLO was accepted and has not left. Join: host + itself once
    // WELCOME arrived, then the host's accepted-member roster (even nameless
    // joins, and even before a world is loaded). Never the raw ENet peer count.
    u32          memberMask;
};

class NetLink {
public:
    NetLink();
    ~NetLink();

    // Start as host on 'port' / as client to 'ip:port' over 'transport'
    // (NET_TRANSPORT_STEAM tunnels ENet over Steam P2P - steamp2p must already
    // be initialised and its peers set; NET_TRANSPORT_UDP is plain UDP). Any
    // previous worker (live or already failed) is stopped and reaped first.
    // Returns false if ENet init or the thread launch failed (status FAILED);
    // true means the launch was ACCEPTED - the transport opens asynchronously
    // (NET_STARTING -> NET_LISTENING / NET_CONNECTING, or NET_FAILED).
    bool startHost(int port, Inbound* inbound, NetTransport transport);
    bool startClient(const std::string& ip, int port, Inbound* inbound,
                     NetTransport transport);
    // Stop the worker, reap its handle, and reset the status to NET_IDLE.
    // Safe (and cheap) when nothing runs.
    void stop();

    // MAIN thread: coherent copy of the lifecycle facts above.
    void copyStatus(NetStatus* out) const;

    // MAIN thread: publish this peer's owned entities (copied under lock). The
    // net thread re-broadcasts the latest snapshot each tick. Pass count 0 to
    // publish nothing.
    void setOwnedEntities(u32 ownerId, const EntityState* arr, unsigned int count);

    // MAIN thread: queue a reliable one-shot event (KO/death/revive). The net thread
    // drains and sends it on the RELIABLE channel next tick (host broadcasts to all
    // peers; client sends to the host). Thread-safe; copied under lock.
    void queueEvent(const EventPacket& ev);

    // MAIN thread: queue a reliable container-contents snapshot (Phase 4a). The net
    // thread serializes [InvSnapshotHeader][InvItemEntry*count] and sends it on the
    // RELIABLE channel next tick. count may be 0 ("container now empty"). Copied
    // under lock; only enqueued on content-change so the reliable channel stays cheap.
    // keyKind (protocol 34): 0 = cKey is the raw container hand, 1 = cKey is the
    // protocol-27 placer key of a session-placed building (receiver translates).
    // `flags` carries INV_FLAG_TRUNCATED when the capture overflowed INV_ITEMS_MAX, so
    // the receiver reconciles additive-only instead of deleting past the cap.
    void queueInvSnapshot(u32 ownerId, u8 keyKind, const u32 cKey[5],
                          const InvItemEntry* items, unsigned int count, u8 flags = 0);

    // MAIN thread: queue a reliable world-item snapshot (Phase W1). The net thread
    // serializes [WorldItemSnapshotHeader][WorldItemEntry*count] and sends it on the
    // RELIABLE channel next tick. Only enqueued for new/changed ground items, so the
    // channel stays quiet for a settled world. Copied under lock.
    void queueWorldItems(u32 ownerId, const WorldItemEntry* items, unsigned int count);

    // MAIN thread: queue a reliable world-item cull (Phase W1) - the netIds of ground
    // items that left the world / interest sphere. [WorldItemRemoveHeader][u32*count].
    void queueWorldRemove(u32 ownerId, const u32* netIds, unsigned int count);

    // MAIN thread: queue a reliable world-item CLAIM (protocol 47) - the netIds whose
    // proxies WE just consumed, so their AUTHOR destroys its real ground copies. The
    // netIds live in the AUTHOR's space, hence authorId. [WorldItemClaimHeader][u32*count].
    void queueWorldClaim(u32 ownerId, u32 authorId, const u32* netIds, unsigned int count);

    // MAIN thread: queue a reliable wide-radius NPC existence census (protocol
    // 36, host -> join, 1 Hz). 'hands' is count*5 u32s (readObjectHand layout);
    // 'pos' is count*3 floats (v38: host position per row, park authority).
    // [NpcCensusHeader][u32 hand[5] * count][f32 pos[3] * count].
    void queueNpcCensus(u32 ownerId, const u32* hands, const float* pos,
                        unsigned int count);

    // MAIN thread: queue a reliable conservation DROP intent (Phase W2). A fixed-size POD
    // (like an event), sent once on the RELIABLE channel; the peer relocates its own copy
    // of the weapon to the ground. Copied under lock.
    void queueWorldDrop(const WorldDropPacket& pkt);

    // MAIN thread: queue a reliable conservation PICKUP intent (Phase W3), mirror of the
    // drop. The peer re-homes its tracked ground copy back into the character's bag.
    void queueWorldPickup(const WorldPickupPacket& pkt);

    // MAIN thread: queue a reliable cross-owner TRANSFER intent (protocol 37). The peer
    // relocates the real item between its own copies of the two containers.
    void queueInvXfer(const InvXferPacket& pkt);

    // MAIN thread: queue the reliable VERDICT for an intent we just applied
    // (protocol 50) - how many units actually landed here.
    void queueInvXferAck(const InvXferAckPacket& pkt);

    // MAIN thread: queue a reliable owner-authoritative medical snapshot (phase 2,
    // player-squad only). Change-gated by the caller so the channel stays quiet.
    void queueMedical(const MedicalPacket& pkt);

    // MAIN thread: queue a reliable treatment delta (first aid administered on a
    // driven copy, forwarded to the body's owner).
    void queueTreatment(const TreatmentPacket& pkt);

    // MAIN thread: queue a reliable join-dealt damage report (join -> host). The
    // join's melee on a driven world-NPC copy is suppressed locally; the host
    // applies the reported damage to the authoritative body.
    void queueCombatHit(const CombatHitPacket& pkt);

    // MAIN thread: queue a reliable game-speed packet (REQ join->host, SET
    // host->join). Change-gated by the caller; pkt.type selects the direction.
    void queueSpeed(const SpeedPacket& pkt);

    // MAIN thread: queue a reliable owner-authoritative character-stats snapshot
    // (protocol 17, player-squad only). Change-gated by the caller.
    void queueStats(const StatsPacket& pkt);

    // MAIN thread: queue the reliable host-authoritative money-pool total
    // (protocol 52, host -> join). Change-gated by the caller.
    void queueMoney(const MoneyPacket& pkt);

    // MAIN thread: queue a reliable money-pool delta (protocol 52, join ->
    // host). Emitted once per observed local change; ordered delivery is what
    // makes the host fold it exactly once.
    void queueMoneyDelta(const MoneyDeltaPacket& pkt);
    void queueFaction(const FactionPacket& pkt);
    void queueTime(const TimePacket& pkt);
    void queueDoor(const DoorPacket& pkt);
    // MAIN thread: queue a reliable host-authoritative machine state row
    // (protocol 33). Change-gated + safety-resent by the caller.
    void queueProd(const ProdPacket& pkt);
    // MAIN thread: queue a reliable host-authoritative known-research row
    // (protocol 38). First-sight sent + safety-resent by the caller.
    void queueResearch(const ResearchPacket& pkt);
    // MAIN thread: queue a reliable property-deed ownership row (protocol 54).
    // Symmetric (either client may buy); first-sight sent + safety-resent by the
    // caller, so a party that buys nothing is silent after the baseline.
    void queueDeed(const DeedPacket& pkt);
    // MAIN thread: queue a reliable runtime-fixture identity row (protocol 55).
    // Symmetric and static (a fixture's position/template never change), so this
    // is first-sight plus a slow safety resend and idles at zero traffic.
    void queueFixture(const FixturePacket& pkt);
    void queueBuildPlace(const BuildPlacePacket& pkt);
    void queueBuildState(const BuildStatePacket& pkt);
    void queueBuildDoor(const BuildDoorPacket& pkt);
    void queueBuildRemove(const BuildRemovePacket& pkt);

    // MAIN thread: queue an UNRELIABLE stealth detection-map snapshot (protocol
    // 20, host -> the sneaker's owner). Latest wins; change-gated + throttled by
    // the caller, so loss just delays an arrow update one snapshot.
    void queueStealth(const StealthPacket& pkt);

    // MAIN thread: queue an UNRELIABLE camera hint (protocol 43, either
    // direction, ~1 Hz). Latest wins; loss just delays the anchor one hint.
    void queueCamHint(const CamHintPacket& pkt);

    // MAIN thread: queue a RELIABLE cell claim (protocol 49, either direction,
    // ~1 Hz). Reliable because a dropped claim reverts the cell to host
    // authority until the next re-assert, and that window is a duplicate-
    // authorship window.
    void queueCellClaim(const CellClaimPacket& pkt);

    // MAIN thread: queue a reliable runtime-spawn query (protocol 21, join ->
    // host). Debounced per hand by the caller.
    void queueSpawnReq(const SpawnReqPacket& pkt);

    // MAIN thread: queue a reliable runtime-spawn description (protocol 21,
    // host -> join). Reply-cached by the caller.
    void queueSpawnInfo(const SpawnInfoPacket& pkt);

    // MAIN thread: coordinated-save packets (protocol 31). REQ join -> host
    // (a suppressed local save forwarded for arbitration); BEGIN/FILE/DONE
    // host -> join (the paced folder transfer; FILE is variable-length:
    // header + relative path + payload, serialized by the net thread); ACK
    // join -> host (staged save verified + committed). All CH_RELIABLE - the
    // ordered stream is what makes the chunk protocol stateless per chunk.
    void queueSaveReq(const SaveReqPacket& pkt);
    void queueSaveBegin(const SaveBeginPacket& pkt);
    void queueSaveFile(const SaveFileHeader& hdr, const char* relPath,
                       const unsigned char* data, unsigned int dataLen);
    void queueSaveDone(const SaveDoneHeader& hdr, const u32* crcs, unsigned int count);
    void queueSaveAck(const SaveAckPacket& pkt);

    // MAIN thread: coordinated-load packets (protocol 32). GO host -> join
    // (load this save now, fingerprint attached); REQ join -> host (a
    // suppressed local load forwarded for arbitration); NACK join -> host
    // (copy missing/diverged - answer with a SaveXfer). All CH_RELIABLE.
    void queueLoadGo(const LoadGoPacket& pkt);
    void queueLoadReq(const LoadReqPacket& pkt);
    void queueLoadNack(const LoadNackPacket& pkt);

    // Debug WAN simulation. When delayMs > 0, received entity batches are held in a
    // net-thread queue and delivered to the game thread only after delayMs +/- jitter
    // has elapsed (lossPct of them are dropped outright). Must be called before
    // startHost/startClient. All-zero = disabled (immediate delivery). See Config.
    void setNetSim(unsigned int delayMs, unsigned int jitterMs, unsigned int lossPct);

    // MAIN thread: display nick sent in HELLO (join) / WELCOME tail (host).
    // Empty = omit the name bytes (legacy HELLO nameLen=0).
    void setLocalName(const char* name);
    // MAIN thread: copy a peer's nick received at handshake. Returns false if
    // that player sent no name (out[0] = 0).
    bool copyPeerName(u32 id, char* out, unsigned cap) const;

    // MAIN thread: advance this peer's session epoch (protocol 44). Called on
    // every session-reset edge (coordinated world reload, connect/disconnect
    // teardown). Subsequent entity batches carry the new epoch, so the peer
    // drops any still-in-flight batch from the prior session; the pending owned-
    // entity snapshot is also dropped so a stale one is not re-stamped with the
    // new epoch and mistaken for fresh. Thread-safe (InterlockedIncrement + the
    // publish lock for the snapshot clear).
    void bumpSessionEpoch();

    // True from an ACCEPTED start until the worker exits (includes the launch
    // window before the transport is open - see copyStatus for the phase).
    bool isRunning() const { return running_ != 0; }
    // host = 0; client = id from WELCOME. myId_ is written by the NET thread when
    // the WELCOME arrives and read here on the MAIN thread, so it is a volatile
    // LONG written via InterlockedExchange; an aligned 32-bit volatile read is
    // atomic on x86/x64 and the volatile bars the compiler from caching a stale
    // value (Phase 4: myId_ cross-thread safety).
    u32  localId()   const { return (u32)myId_; }

    // MAIN thread: copy one coherent, zeroed-when-stopped traffic sample.
    void copyDebugStats(NetDebugStats* out) const;

private:
    static DWORD WINAPI threadEntry(LPVOID self);
    void threadLoop();
    bool launchThread();
    // Status writers (any thread; statusCs_).
    void setPhase(NetPhase p);
    void setActiveTransport(NetTransport t);
    void setError(NetError e, u32 arg, const char* raw);
    void setMemberBit(u32 id, bool on);
    void setMembers(u32 mask);
    void setHandshakeCounts(u32 connectAttempts, u32 pendingHandshakes);

    // Net-thread-only: route a received entity through the WAN sim (delay/drop) when
    // enabled, else deliver immediately. flushDelayed() releases matured entries.
    void deliverEntity(u32 ownerId, u32 sendMs, const EntityState& e);
    void flushDelayed();

    // Net-thread-only (protocol 44): gate an incoming entity batch by its session
    // epoch. Returns false (drop) if 'epoch' is older than the newest accepted
    // from 'ownerId'; otherwise records it and returns true. epochSeen_ is reset
    // at every connection edge so a reconnecting peer restarting at epoch 0 is
    // never locked out.
    bool acceptEpoch(u32 ownerId, u32 epoch);

    // NET-thread: clone `pkt` to every connected ENet peer except `from`.
    // Join-authored game state has to reach other joins; ENet is star-shaped
    // so the host is the only relay.
    void relayToOthers(ENetPeer* from, enet_uint8 channel, ENetPacket* pkt);
    static bool shouldRelayType(u8 type);

    bool        isHost_;
    std::string ip_;
    int         port_;

    ENetHost*   enetHost_;   // net thread only
    ENetPeer*   serverPeer_; // client only; net thread only
    Inbound*    inbound_;

    CRITICAL_SECTION         outCs_;
    std::vector<EntityState> out_;
    u32                      outOwner_;
    u32                      outStampMs_; // capture-time stamp for the batch header (v35)
    bool                     haveOut_;
    // Reliable events queued by the main thread, drained + sent by the net thread.
    // Guarded by outCs_ (same publish lock as out_).
    std::vector<EventPacket> outEvents_;
    // Reliable container-contents snapshots queued by the main thread, drained +
    // serialized by the net thread. Variable-length, so each carries its own item
    // list. Guarded by outCs_.
    struct OutInv {
        u32                       ownerId;
        u8                        keyKind; // protocol 34: 0 raw hand, 1 placer key
        u8                        flags;   // protocol 46: INV_FLAG_TRUNCATED
        u32                       cKey[5];
        std::vector<InvItemEntry> items;
    };
    std::vector<OutInv>      outInv_;
    // Reliable world-item snapshots / culls queued by the main thread (Phase W1),
    // drained + serialized by the net thread. Guarded by outCs_.
    struct OutWorldItems { u32 ownerId; std::vector<WorldItemEntry> items; };
    struct OutWorldRemove { u32 ownerId; std::vector<u32> netIds; };
    struct OutWorldClaim { u32 ownerId; u32 authorId; std::vector<u32> netIds; };
    std::vector<OutWorldItems>  outWorldItems_;
    std::vector<OutWorldRemove> outWorldRemove_;
    std::vector<OutWorldClaim>  outWorldClaim_;
    // Reliable NPC existence census (protocol 36): 5xu32 hands, flat. Guarded
    // by outCs_. 1 Hz from the host, so at most a couple pending at once.
    struct OutNpcCensus { u32 ownerId; std::vector<u32> hands; std::vector<float> pos; };
    std::vector<OutNpcCensus> outNpcCensus_;
    // Reliable conservation DROP intents (Phase W2), fixed-size PODs. Guarded by outCs_.
    std::vector<WorldDropPacket> outWorldDrops_;
    std::vector<WorldPickupPacket> outWorldPickups_;
    // Reliable cross-owner transfer intents (protocol 37). Guarded by outCs_.
    std::vector<InvXferPacket>   outInvXfers_;
    // Reliable transfer verdicts (protocol 50). Guarded by outCs_.
    std::vector<InvXferAckPacket> outInvXferAcks_;
    // Reliable medical snapshots + treatment deltas (phase 2). Guarded by outCs_.
    std::vector<MedicalPacket>   outMedical_;
    std::vector<TreatmentPacket> outTreatments_;
    std::vector<CombatHitPacket> outCombatHits_;
    // Reliable game-speed REQ/SET packets (consensus speed sync). Guarded by outCs_.
    std::vector<SpeedPacket>     outSpeed_;
    // Reliable character-stats snapshots (protocol 17). Guarded by outCs_.
    std::vector<StatsPacket>     outStats_;
    // Reliable money-pool totals + join deltas (protocol 52). Guarded by outCs_.
    std::vector<MoneyPacket>     outMoney_;
    std::vector<MoneyDeltaPacket> outMoneyDelta_;
    std::vector<FactionPacket>   outFaction_;
    std::vector<TimePacket>      outTime_;
    std::vector<DoorPacket>      outDoor_;
    // Reliable machine state rows (protocol 33). Guarded by outCs_.
    std::vector<ProdPacket>      outProd_;
    // Reliable known-research rows (protocol 38). Guarded by outCs_.
    std::vector<ResearchPacket>  outResearch_;
    // Reliable property-deed ownership rows (protocol 54). Guarded by outCs_.
    std::vector<DeedPacket>      outDeed_;
    // Reliable runtime-fixture identity rows (protocol 55). Guarded by outCs_.
    std::vector<FixturePacket>   outFixture_;
    std::vector<BuildPlacePacket> outBuildPlace_;
    std::vector<BuildStatePacket> outBuildState_;
    std::vector<BuildDoorPacket>  outBuildDoor_;
    std::vector<BuildRemovePacket> outBuildRemove_;
    // Unreliable stealth detection-map snapshots (protocol 20). Guarded by outCs_.
    std::vector<StealthPacket>   outStealth_;
    // Unreliable camera hints (protocol 43, ~1 Hz latest-wins). Guarded by outCs_.
    std::vector<CamHintPacket>   outCamHint_;
    // Reliable cell claims (protocol 49, ~1 Hz on change + re-assert). Guarded by outCs_.
    std::vector<CellClaimPacket> outCellClaim_;
    // Reliable runtime-spawn query/description packets (protocol 21). Guarded by outCs_.
    std::vector<SpawnReqPacket>  outSpawnReq_;
    std::vector<SpawnInfoPacket> outSpawnInfo_;
    // Reliable coordinated-save packets (protocol 31). FILE carries its
    // variable tail (relative path + payload) pre-flattened; DONE carries its
    // CRC table. Guarded by outCs_.
    struct OutSaveFile { SaveFileHeader hdr; std::vector<u8> tail; };
    struct OutSaveDone { SaveDoneHeader hdr; std::vector<u32> crcs; };
    std::vector<SaveReqPacket>   outSaveReq_;
    std::vector<SaveBeginPacket> outSaveBegin_;
    std::vector<OutSaveFile>     outSaveFile_;
    std::vector<OutSaveDone>     outSaveDone_;
    std::vector<SaveAckPacket>   outSaveAck_;
    // Reliable coordinated-load packets (protocol 32). Guarded by outCs_.
    std::vector<LoadGoPacket>    outLoadGo_;
    std::vector<LoadReqPacket>   outLoadReq_;
    std::vector<LoadNackPacket>  outLoadNack_;

    void clearDebugStats();
    mutable CRITICAL_SECTION debugCs_;
    NetDebugStats debugStats_;

    mutable CRITICAL_SECTION statusCs_;
    NetStatus                status_;

    HANDLE        thread_;
    volatile LONG running_;
    volatile LONG stopFlag_;
    // Written by the NET thread on WELCOME (InterlockedExchange) and read on the
    // MAIN thread via localId(); volatile LONG so the read is atomic + uncached.
    volatile LONG myId_;

    // Session epoch (protocol 44). sendEpoch_ is bumped by the MAIN thread
    // (InterlockedIncrement in bumpSessionEpoch) and read by the NET thread when
    // it stamps an outgoing entity batch - a volatile LONG, so the read is atomic
    // + uncached. epochSeen_ is NET-thread-only (touched only in the receive
    // ladder + connect/disconnect handlers), so it needs no lock.
    volatile LONG        sendEpoch_;
    std::map<u32, u32>   epochSeen_; // newest accepted epoch per ownerId

    // Transport of the current start (set by startHost/startClient before the
    // launch, read-only on the net thread thereafter). Re-chosen on EVERY start,
    // so a Steam session never leaks into a following UDP one.
    bool               steamMode_;

    // Handshake nicks. localName_ is written on the MAIN thread (setLocalName)
    // and read on the NET thread when packing HELLO/WELCOME; peerName_ is
    // written on the NET thread (HELLO/WELCOME recv, disconnect) and read on
    // the MAIN thread (copyPeerName). nameCs_ covers both.
    mutable CRITICAL_SECTION nameCs_;
    char               localName_[64];
    char               peerName_[MAX_PLAYERS][64];

    // WAN sim config (set before launch; read-only on the net thread thereafter).
    unsigned int  simDelayMs_;
    unsigned int  simJitterMs_;
    unsigned int  simLossPct_;
    // Held-back inbound entities awaiting their simulated arrival time. Net-thread
    // only (received and flushed on the same thread), so it needs no lock.
    struct Delayed { DWORD releaseTick; u32 ownerId; u32 sendMs; EntityState e; };
    std::deque<Delayed> delayed_;

    NetLink(const NetLink&);
    NetLink& operator=(const NetLink&);
};

} // namespace coop

#endif // KENSHICOOP_NETLINK_H
