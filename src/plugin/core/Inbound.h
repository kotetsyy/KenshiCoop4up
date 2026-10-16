// Inbound - the net->game thread bridge.
//
// Kenshi's engine is single-threaded and NOT thread-safe, so the background net
// thread must never touch game memory. It only pushes small PODs (copied by
// value) into these queues; the main-thread tick hook drains them at a known-safe
// point. Built for VS2010 (v100): a Win32 CRITICAL_SECTION (std::mutex needs
// VS2012+).

#ifndef KENSHICOOP_INBOUND_H
#define KENSHICOOP_INBOUND_H

#include <windows.h>
#include <deque>
#include <vector>
#include "../../netproto/Wire.h"
#include "../net/NativeLock.h"
#include "InventorySnapshotAssembler.h"
#include "../../netproto/EnvironmentItems.h"

namespace coop {

// One received entity plus the network id of the peer that owns it. The owner
// tag lets the receiver apply the right authority rule (drive a peer's entity,
// never one we own). sendMs (wire v35) is the sender's monotonic ms clock at
// capture time - the interp buffer indexes on it (mapped into the local clock)
// instead of the arrival time, so path jitter stays out of the snapshot spacing.
struct InboundEntity {
    u32         ownerId;
    u32         sendMs;
    EntityState e;
};

// One received reliable event (KO/death/revive transition), owner-tagged like an
// entity so the receiver applies it to the right peer's body.
struct InboundEvent {
    u32         ownerId;
    EventPacket ev;
};

// One received container-contents snapshot (Phase 4a): the authoritative owner, the
// container's key, and the full item list. The receiver reconciles its local copy
// of that container to match. count==0 means "now empty". keyKind (protocol 34):
// 0 = cKey is the raw container hand, 1 = cKey is the protocol-27 placer key of a
// session-placed building (the receiver translates through its build maps).
struct InboundInv {
    u32                       ownerId;
    u8                        keyKind;
    u8                        flags;   // protocol 46: INV_FLAG_TRUNCATED (additive-only)
    u32                       revision;
    u32                       cKey[5]; // type, container, containerSerial, index, serial
    std::vector<InvItemEntry> items;
};

// One received world-item snapshot (Phase W1): the authoritative owner (host) and the
// netId-keyed ground items in its interest sphere. The join reconciles its local proxies
// (spawn new / update moved / leave the rest) to match.
struct InboundWorldItems {
    u32                        ownerId;
    std::vector<WorldItemEntry> items;
};

// One received world-item cull (Phase W1): the netIds whose ground items left the world /
// interest sphere, so the join destroys the matching proxies.
struct InboundWorldRemove {
    u32              ownerId;
    std::vector<u32> netIds;
};

// One received world-item CLAIM (protocol 47): a peer consumed the proxies it held for these
// netIds, so WE (the author) must destroy our real ground objects. netIds are in OUR netId
// space; authorId lets a receiver ignore a claim addressed to a different author.
struct InboundWorldClaim {
    u32              ownerId;  // the claiming peer
    u32              authorId; // whose netId space the ids belong to
    std::vector<u32> netIds;
};

// One received NPC existence census (protocol 36, join side): the hands of
// every world NPC within the host's census radius, flat 5xu32 per NPC. The
// join culls local NPCs absent from this list at long range (existence
// authority); position authority stays with the 20 Hz entity stream.
struct InboundNpcCensus {
    u32                ownerId;
    std::vector<u32>   hands; // count*5, readObjectHand layout
    std::vector<float> pos;   // count*3, host position per row (v38 parking)
};

// DROP and PICKUP share one reliable queue: splitting packet families would
// reorder DROP -> PICKUP -> DROP before the native conservation consumer.
struct InboundGroundGear {
    u32 ownerId;
    u8 type;
    union {
        WorldDropPacket drop;
        WorldPickupPacket pickup;
    };
};

// One received cross-owner TRANSFER intent (protocol 37): a peer performed a
// direct UI drag between two containers, at least one of which it does not
// author. The receiver relocates the REAL item between its own copies of the
// two containers (conservation - no fabrication, no destruction).
struct InboundInvXfer {
    u32           ownerId;
    InvXferPacket pkt;
    std::vector<InvItemEntry> contents;
};

// One received transfer VERDICT (protocol 50): the answer to an intent WE
// authored, saying how many units actually landed on the receiver. World-state:
// a verdict for a world that no longer exists is meaningless, and the author's
// latches were dropped by the same reset.
struct InboundInvXferAck {
    u32              ownerId;
    InvXferAckPacket pkt;
    std::vector<InvItemEntry> contents;
};

// One received owner-authoritative medical snapshot (phase 2): the subject's
// owner streams its local-only medical model; the receiver writes it onto its
// driven copy of that body.
struct InboundMedical {
    u32           ownerId;
    MedicalPacket pkt;
};

// One received treatment delta (phase 2): first aid administered on a DRIVEN
// copy, forwarded to the body's owner, who applies the bandage levels raise-only.
struct InboundTreatment {
    u32             ownerId;
    TreatmentPacket pkt;
};

// One received join-dealt damage report (protocol 45): the join's melee on a
// driven world-NPC copy was suppressed locally; the host wounds the real NPC.
struct InboundCombatHit {
    u32             ownerId;
    CombatHitPacket pkt;
};

// One received game-speed packet (consensus speed sync): a REQUEST from a peer
// (host consumes; join requests never reach a join) or the host's arbitrated
// SET (join applies). pkt.type distinguishes the two.
struct InboundSpeed {
    u32         ownerId;
    SpeedPacket pkt;
};

// One received owner-authoritative character-stats snapshot (protocol 17): the
// subject's owner streams its local-only CharStats; the receiver writes it onto
// its driven copy of that body.
struct InboundStats {
    u32         ownerId;
    StatsPacket pkt;
};

// One received shared-pool total (protocol 52): the host publishes the
// authoritative pool; the join writes it onto its own engine wallet after
// re-applying any delta the host has not acked yet.
struct InboundMoney {
    u32         ownerId;
    MoneyPacket pkt;
};

// One received pool delta (protocol 52): a change the join's local economy
// already applied, which the host folds into the authoritative pool.
struct InboundMoneyDelta {
    u32              ownerId;
    MoneyDeltaPacket pkt;
};

// One received player-faction relation row (protocol 24): from the host it is
// the authoritative row (the join applies it); from the join it is a forwarded
// intent (the host applies it, then its own stream echoes the row back).
struct InboundFaction {
    u32           ownerId;
    FactionPacket pkt;
};

// One received game-clock sample (protocol 25, join side): the host's
// absolute in-game hours; the join slews its local sim speed to close the
// offset.
struct InboundTime {
    u32        ownerId;
    TimePacket pkt;
};

// One received baked-door state row (protocol 26): a door the sender's table
// saw move; the receiver applies it through the engine's own door actions
// (baseline updated first - echo-free).
struct InboundDoor {
    u32        ownerId;
    DoorPacket pkt;
};

// One received placed-building announcement (protocol 27): the sender placed
// a building; the receiver mints a local construction site and maps the
// sender's key to its own local hand.
struct InboundBuildPlace {
    u32              ownerId;
    BuildPlacePacket pkt;
};

// One received construction-progress row (protocol 27): progress for a
// building the SENDER placed, applied through the receiver's translation map.
struct InboundBuildState {
    u32              ownerId;
    BuildStatePacket pkt;
};

// One received placed-building door row (protocol 28): keyed by the PLACER's
// building hand + door index; the receiver resolves its local door through
// the build maps and applies via the engine's own door actions.
struct InboundBuildDoor {
    u32             ownerId;
    BuildDoorPacket pkt;
};

// One received building removal (protocol 28): the sender dismantled or
// destroyed a building it placed; the receiver destroys its mapped proxy.
struct InboundBuildRemove {
    u32               ownerId;
    BuildRemovePacket pkt;
};

struct InboundProcedural {
    u32 ownerId;
    ProceduralPacket pkt;
};

// One received machine state row (protocol 33): the HOST's authoritative
// power/production/farm state for a machine; the join resolves the key
// (baked hand or protocol-27 placer key) and applies through the engine's
// own levers.
struct InboundProd {
    u32        ownerId;
    ProdPacket pkt;
};

// One received known-research row (protocol 38): the HOST reports a RESEARCH
// stringID as known; the join applies via Research::startResearch (idempotent
// against already-known sids).
struct InboundResearch {
    u32            ownerId;
    ResearchPacket pkt;
};

// One received property-deed row (protocol 54): a peer reports that a BAKED
// building is (or is no longer) owned by the shared player faction. Applied as
// a pure state write - never the buy path, which would charge the cats twice.
struct InboundDeed {
    u32        ownerId;
    DeedPacket pkt;
};

// One received runtime-fixture identity row (protocol 55): a peer names the
// hand it uses for a fixture and describes where/what it is, so we can pair it
// with our own copy. Carries no gameplay state - only identity.
struct InboundFixture {
    u32           ownerId;
    FixturePacket pkt;
};

// One received stealth detection-map snapshot (protocol 20): the detection
// AUTHORITY (the host's world, where the sneaker is a driven copy) streams who
// notices the sneaker; the sneaker's OWNER replays the entries between its
// local pair so the marker arrows render natively.
struct InboundStealth {
    u32           ownerId;
    StealthPacket pkt;
};

// One received runtime-spawn query (protocol 21, host side): the join names a
// streamed hand it cannot resolve; the host describes it (PKT_SPAWN_INFO).
struct InboundSpawnReq {
    u32            ownerId;
    SpawnReqPacket pkt;
};

// One received runtime-spawn description (protocol 21, join side): the host's
// template/faction/transform for a hand the join asked about; the join mints a
// local proxy body and binds it to the hand key.
struct InboundSpawnInfo {
    u32             ownerId;
    SpawnInfoPacket pkt;
};

// One received join save request (protocol 31, host side): the join's player
// pressed save (local write suppressed); the host runs the authoritative save.
struct InboundSaveReq {
    u32           ownerId;
    SaveReqPacket pkt;
};

// One received save-transfer announce (protocol 31, join side): the host is
// about to stream fileCount files / totalBytes; the join wipes + creates its
// staging folder.
struct InboundSaveBegin {
    u32             ownerId;
    SaveBeginPacket pkt;
};

// One received save-file chunk (protocol 31, join side): header + the file's
// save-relative path + up to SAVE_CHUNK_MAX payload bytes. Ordered-reliable
// delivery means chunks arrive exactly as the sender paced them.
struct InboundSaveFile {
    u32             ownerId;
    SaveFileHeader  hdr;
    std::string     path; // hdr.pathLen bytes, save-folder-relative
    std::vector<u8> data; // hdr.dataLen bytes
};

// One received save-transfer CRC table (protocol 31, join side): verify the
// staged files and commit (or discard).
struct InboundSaveDone {
    u32              ownerId;
    SaveDoneHeader   hdr;
    std::vector<u32> crcs;
};

// One received commit acknowledgement (protocol 31, host side).
struct InboundSaveAck {
    u32           ownerId;
    SaveAckPacket pkt;
};

// One received coordinated-load order (protocol 32, join side): the host
// loaded a save; the join verifies its copy and follows (or NACKs).
struct InboundLoadGo {
    u32          ownerId;
    LoadGoPacket pkt;
};

// One received join load request (protocol 32, host side): the join's player
// pressed load (suppressed locally); the host arbitrates.
struct InboundLoadReq {
    u32           ownerId;
    LoadReqPacket pkt;
};

// One received copy-missing/diverged answer (protocol 32, host side): the
// join can't load the ordered save; a SaveXfer transfer follows.
struct InboundLoadNack {
    u32            ownerId;
    LoadNackPacket pkt;
};

// One received camera hint (protocol 43, host side): the join's camera world
// center, folded into interestCenters as an extra anchor. Latest wins.
struct InboundCamHint {
    u32           ownerId;
    CamHintPacket pkt;
};

// One received cell claim (protocol 49, both directions): the peer's tab is
// standing in this zone cell. World-state - the claims describe where bodies
// are in THIS world, so a reload drops them and the next second re-publishes.
struct InboundCellClaim {
    u32             ownerId;
    CellClaimPacket pkt;
};

// --- Structural world-state classification (Phase 4) -------------------------
// Every inbound queue is exactly one of two kinds, chosen at its DECLARATION:
//   WorldQ<T>   - describes the CURRENT world; dropped on a session-reset edge
//                 (reload / reconnect / disconnect). It self-registers into the
//                 owner's world-state reset list on construction, so
//                 flushWorldState() clears it automatically - a WorldQ CANNOT be
//                 forgotten from the reset (the class of bug that once let a
//                 cross-owner invXfer intent survive a world reload).
//   SessionQ<T> - OUTLIVES the world swap (the connection persists): presence
//                 edges + the coordinated save/load handshake. Never registered,
//                 so flushWorldState() leaves it intact by construction.
// Both forward the only three operations Inbound performs on a queue - push_back
// (net thread), swap-drain (main thread, via the std::deque& conversion) and
// clear (flush) - so the push/drain methods below are unchanged.
struct IClearableQueue {
    virtual void clearQueue() = 0;
    virtual void clearOwner(u32 ownerId) = 0;
    virtual bool hasCapacity() const = 0;
    virtual ~IClearableQueue() {}
};
class WorldInventoryFrames : public IClearableQueue {
    InventorySnapshotAssembler assembly_;
public:
    explicit WorldInventoryFrames(std::vector<IClearableQueue*>& registry) {
        registry.push_back(this);
    }
    int put(const InvSnapshotHeader& h,const InvItemEntry* rows,
            std::vector<InvItemEntry>& complete) { return assembly_.put(h,rows,complete); }
    virtual void clearQueue() { assembly_.clear(); }
    virtual void clearOwner(u32 owner) { assembly_.retire(owner); }
    virtual bool hasCapacity() const { return assembly_.hasCapacity(); }
private:
    WorldInventoryFrames(const WorldInventoryFrames&);
    WorldInventoryFrames& operator=(const WorldInventoryFrames&);
};


template<class T>
class WorldQ : public IClearableQueue {
public:
    // cap == 0: unbounded (reliable queues MUST stay unbounded - dropping a
    // save chunk or an event corrupts the stream). cap > 0: a bounded mailbox
    // for an UNRELIABLE latest-wins traffic class (entity/stealth/cam), where a
    // stalled main thread would otherwise let the queue grow without limit; on
    // overflow the OLDEST entry is dropped, which is exactly "newest wins" for
    // continuous state (Phase 4d bounded mailboxes).
    explicit WorldQ(std::vector<IClearableQueue*>& reg, size_t cap = 0, size_t reliableLimit = 128)
      : cap_(cap), reliableLimit_(reliableLimit) { reg.push_back(this); }
    T& push_back(const T& v) {
        if (cap_ && q_.size() >= cap_) q_.pop_front();
        q_.push_back(v);
        return q_.back();
    }
    operator std::deque<T>&() { return q_; }
    virtual void clearQueue() { q_.clear(); }
    virtual void clearOwner(u32 ownerId) {
        for (typename std::deque<T>::iterator i = q_.begin(); i != q_.end();) {
            if (i->ownerId == ownerId) i = q_.erase(i); else ++i;
        }
    }
    virtual bool hasCapacity() const { return cap_ || q_.size() < reliableLimit_; }
private:
    std::deque<T> q_;
    size_t        cap_;
    size_t        reliableLimit_;
    WorldQ(const WorldQ&);
    WorldQ& operator=(const WorldQ&);
};

template<class T>
class SessionQ {
public:
    SessionQ() {}
    void push_back(const T& v) { q_.push_back(v); }
    operator std::deque<T>&() { return q_; }
    bool hasCapacity() const { return q_.size() < 512; }
private:
    std::deque<T> q_;
    SessionQ(const SessionQ&);
    SessionQ& operator=(const SessionQ&);
};

class Inbound {
public:
    // WorldQ members self-register into worldReset_ (declared first, below), so
    // the init list wires every world-state queue to the structural reset.
    Inbound()
      : sawRemote_(false), generation_(0),
        // Bounded mailboxes (Phase 4d): the three UNRELIABLE latest-wins queues
        // carry a drop-oldest cap so a stalled main thread cannot grow them
        // without bound. ent_ = ~12 s of a 20 Hz * 17-entity stream; stealth/cam
        // are ~1 Hz refreshes. Every other queue is reliable and stays unbounded.
        ent_(worldReset_, 4096),  evt_(worldReset_),        inv_(worldReset_),
        invFrames_(worldReset_),
        wi_(worldReset_),         wir_(worldReset_),        wic_(worldReset_),
        npcCensus_(worldReset_),
        groundGear_(worldReset_), invXfer_(worldReset_), invXferAck_(worldReset_),
        med_(worldReset_),        treat_(worldReset_),      combatHit_(worldReset_),
        speed_(worldReset_),
        stats_(worldReset_),      money_(worldReset_),      moneyDelta_(worldReset_),
        faction_(worldReset_),
        time_(worldReset_),       door_(worldReset_),       prod_(worldReset_),
        research_(worldReset_),   deed_(worldReset_),       fixture_(worldReset_),
        buildPlace_(worldReset_), buildState_(worldReset_),
        buildDoor_(worldReset_),  buildRemove_(worldReset_),
        procedural_(worldReset_, 0, 4096), stealth_(worldReset_, 512),
        environment_(worldReset_, 0, 4096),
        spawnReq_(worldReset_),   spawnInfo_(worldReset_),  camHint_(worldReset_, 64),
        cellClaim_(worldReset_, 64) {
        InitializeCriticalSection(&cs_);
    }
    ~Inbound() { DeleteCriticalSection(&cs_); }

    // NET thread: a peer joined (id) / a peer left (id, or OWNER_ID_ALL).
    void pushConnect(u32 id) {
        NativeLock nativeLock456(&cs_); conn_.push_back(id); nativeLock456.unlock();
    }
    void pushLeave(u32 id) {
        NativeLock nativeLock459(&cs_); leave_.push_back(id); nativeLock459.unlock();
    }
    // NET thread: one received entity transform, owner-tagged + send-stamped.
    void pushEntity(u32 ownerId, u32 sendMs, const EntityState& e) {
        InboundEntity ie; ie.ownerId = ownerId; ie.sendMs = sendMs; ie.e = e;
        NativeLock nativeLock464(&cs_); ent_.push_back(ie); sawRemote_ = true;
        nativeLock464.unlock();
    }

    // MAIN thread: has this client EVER received an owned-entity batch from a peer?
    // A peer only publishes its owned entities once IT reaches gameplay, so on the
    // host this flips true exactly when the JOIN is loaded + streaming (the reliable
    // "peer is in-game" gate for time-sensitive host actions like a live spawn).
    bool sawRemoteEntity() {
        NativeLock nativeLock473(&cs_); bool v = sawRemote_; nativeLock473.unlock();
        return v;
    }
    // MAIN thread: the current session generation. Bumped by flushWorldState()
    // on every world-state reset edge (reload / reconnect / disconnect). A
    // reference or index captured under an older generation is known-stale.
    // Native dispatch and delayed publication hold the generation fence through
    // enqueue, so a concurrent flush cannot republish an old-world packet.
    u32 sessionGeneration() {
        NativeLock nativeLock482(&cs_); u32 g = generation_; nativeLock482.unlock();
        return g;
    }

    // NET thread holds this recursive Win32 lock through a validated world
    // dispatch. A simultaneous main-thread flush cannot republish old packets.
    bool beginWorldEnqueue(u32 expectedGeneration) {
        EnterCriticalSection(&cs_);
        if (generation_ == expectedGeneration) return true;
        LeaveCriticalSection(&cs_);
        return false;
    }
    void endWorldEnqueue() { LeaveCriticalSection(&cs_); }
    class WorldEnqueueLock {
    public:
        WorldEnqueueLock(Inbound* owner,u32 generation,bool enabled):owner_(0) {
            if(enabled && owner && owner->beginWorldEnqueue(generation))owner_=owner;
        }
        ~WorldEnqueueLock() {unlock();}
        bool locked() const {return owner_!=0;}
        void unlock() {if(owner_){owner_->endWorldEnqueue();owner_=0;}}
    private:
        Inbound* owner_;
        WorldEnqueueLock(const WorldEnqueueLock&);
        WorldEnqueueLock& operator=(const WorldEnqueueLock&);
    };
    // NET thread: one received reliable event, owner-tagged.
    void pushEvent(u32 ownerId, const EventPacket& ev) {
        InboundEvent ievt; ievt.ownerId = ownerId; ievt.ev = ev;
        NativeLock nativeLock511(&cs_); evt_.push_back(ievt); nativeLock511.unlock();
    }
    // NET thread: one received container-contents snapshot, owner-tagged.
    void pushInv(u32 ownerId, u8 keyKind, const u32 cKey[5],
                 const InvItemEntry* items, unsigned int count, u8 flags = 0, u32 revision = 0) {
        InboundInv ii;
        ii.ownerId = ownerId;
        ii.keyKind = keyKind;
        ii.flags   = flags;
        ii.revision = revision;
        for (int k = 0; k < 5; ++k) ii.cKey[k] = cKey[k];
        if (items && count > 0) ii.items.assign(items, items + count);
        NativeLock nativeLock522(&cs_); inv_.push_back(ii); nativeLock522.unlock();
    }
    bool pushInvFrame(const InvSnapshotHeader& h,const InvItemEntry* rows) {
        NativeLock lock(&cs_);
        std::vector<InvItemEntry> complete;
        if (!(h.flags & INV_FLAG_CHECKPOINT_ACK)) {
            const int result=invFrames_.put(h,rows,complete);
            if (result<0) return false;
            if (!result) return true;
        }
        InboundInv& item=inv_.push_back(InboundInv());
        item.ownerId=h.ownerId; item.keyKind=h.keyKind;
        item.flags=h.flags & ~INV_FLAG_COMPLETE; item.revision=h.revision;
        item.cKey[0]=h.cType; item.cKey[1]=h.cContainer;
        item.cKey[2]=h.cContainerSerial; item.cKey[3]=h.cIndex; item.cKey[4]=h.cSerial;
        item.items.swap(complete);
        return true;
    }
    // NET thread: one received world-item snapshot, owner-tagged.
    void pushWorldItems(u32 ownerId, const WorldItemEntry* items, unsigned int count) {
        InboundWorldItems wi;
        wi.ownerId = ownerId;
        if (items && count > 0) wi.items.assign(items, items + count);
        NativeLock nativeLock529(&cs_); wi_.push_back(wi); nativeLock529.unlock();
    }
    // NET thread: one received world-item cull (list of netIds), owner-tagged.
    void pushWorldRemove(u32 ownerId, const u32* netIds, unsigned int count) {
        InboundWorldRemove wr;
        wr.ownerId = ownerId;
        if (netIds && count > 0) wr.netIds.assign(netIds, netIds + count);
        NativeLock nativeLock536(&cs_); wir_.push_back(wr); nativeLock536.unlock();
    }
    // NET thread: one received world-item claim (protocol 47), owner-tagged.
    void pushWorldClaim(u32 ownerId, u32 authorId, const u32* netIds, unsigned int count) {
        InboundWorldClaim wc;
        wc.ownerId = ownerId; wc.authorId = authorId;
        if (netIds && count > 0) wc.netIds.assign(netIds, netIds + count);
        NativeLock nativeLock543(&cs_); wic_.push_back(wc); nativeLock543.unlock();
    }
    // NET thread: one received NPC existence census (protocol 36), owner-tagged.
    void pushNpcCensus(u32 ownerId, const u32* hands, const float* pos,
                       unsigned int count) {
        InboundNpcCensus nc;
        nc.ownerId = ownerId;
        if (hands && count > 0) nc.hands.assign(hands, hands + count * 5);
        if (pos && count > 0) nc.pos.assign(pos, pos + count * 3);
        NativeLock nativeLock552(&cs_); npcCensus_.push_back(nc); nativeLock552.unlock();
    }
    // NET thread: one received conservation DROP intent, owner-tagged.
    void pushWorldDrop(u32 ownerId, const WorldDropPacket& pkt) {
        InboundGroundGear edge; edge.ownerId=ownerId; edge.type=PKT_WORLD_DROP; edge.drop=pkt;
        NativeLock lock(&cs_); groundGear_.push_back(edge);
    }
    // NET thread: one received conservation PICKUP intent, owner-tagged.
    void pushWorldPickup(u32 ownerId, const WorldPickupPacket& pkt) {
        InboundGroundGear edge; edge.ownerId=ownerId; edge.type=PKT_WORLD_PICKUP; edge.pickup=pkt;
        NativeLock lock(&cs_); groundGear_.push_back(edge);
    }
    // NET thread: one received cross-owner transfer intent (protocol 37), owner-tagged.
    void pushInvXfer(u32 ownerId, const InvXferPacket& pkt,
                      const InvItemEntry* contents, unsigned int count) {
        NativeLock nativeLock567(&cs_);
        InboundInvXfer& ix = invXfer_.push_back(InboundInvXfer());
        ix.ownerId = ownerId; ix.pkt = pkt;
        if (count) ix.contents.assign(contents, contents + count);
        nativeLock567.unlock();
    }
    // NET thread: one received authority verdict and optional complete bag contents.
    void pushInvXferAck(u32 ownerId, const InvXferAckPacket& pkt,
                         const InvItemEntry* contents, unsigned int count) {
        NativeLock nativeLock576(&cs_);
        InboundInvXferAck& ia = invXferAck_.push_back(InboundInvXferAck());
        ia.ownerId = ownerId; ia.pkt = pkt;
        if (count) ia.contents.assign(contents, contents + count);
        nativeLock576.unlock();
    }
    // NET thread: one received medical snapshot, owner-tagged.
    void pushMedical(u32 ownerId, const MedicalPacket& pkt) {
        InboundMedical im; im.ownerId = ownerId; im.pkt = pkt;
        NativeLock nativeLock585(&cs_); med_.push_back(im); nativeLock585.unlock();
    }
    // NET thread: one received treatment delta, owner-tagged.
    void pushTreatment(u32 ownerId, const TreatmentPacket& pkt) {
        InboundTreatment it; it.ownerId = ownerId; it.pkt = pkt;
        NativeLock nativeLock590(&cs_); treat_.push_back(it); nativeLock590.unlock();
    }
    // NET thread: one received join-dealt damage report, owner-tagged.
    void pushCombatHit(u32 ownerId, const CombatHitPacket& pkt) {
        InboundCombatHit it; it.ownerId = ownerId; it.pkt = pkt;
        NativeLock nativeLock595(&cs_); combatHit_.push_back(it); nativeLock595.unlock();
    }
    // NET thread: one received game-speed request/set, owner-tagged.
    void pushSpeed(u32 ownerId, const SpeedPacket& pkt) {
        InboundSpeed is; is.ownerId = ownerId; is.pkt = pkt;
        NativeLock nativeLock600(&cs_); speed_.push_back(is); nativeLock600.unlock();
    }
    // NET thread: one received character-stats snapshot, owner-tagged.
    void pushStats(u32 ownerId, const StatsPacket& pkt) {
        InboundStats ist; ist.ownerId = ownerId; ist.pkt = pkt;
        NativeLock nativeLock605(&cs_); stats_.push_back(ist); nativeLock605.unlock();
    }
    // NET thread: one received shared-pool total (protocol 52), owner-tagged.
    void pushMoney(u32 ownerId, const MoneyPacket& pkt) {
        InboundMoney imo; imo.ownerId = ownerId; imo.pkt = pkt;
        NativeLock nativeLock610(&cs_); money_.push_back(imo); nativeLock610.unlock();
    }
    // NET thread: one received pool delta (protocol 52), owner-tagged.
    void pushMoneyDelta(u32 ownerId, const MoneyDeltaPacket& pkt) {
        InboundMoneyDelta imd; imd.ownerId = ownerId; imd.pkt = pkt;
        NativeLock nativeLock615(&cs_); moneyDelta_.push_back(imd); nativeLock615.unlock();
    }
    // NET thread: one received faction-relation row (protocol 24), owner-tagged.
    void pushFaction(u32 ownerId, const FactionPacket& pkt) {
        InboundFaction ifa; ifa.ownerId = ownerId; ifa.pkt = pkt;
        NativeLock nativeLock620(&cs_); faction_.push_back(ifa); nativeLock620.unlock();
    }
    // NET thread: one received game-clock sample (protocol 25), owner-tagged.
    void pushTime(u32 ownerId, const TimePacket& pkt) {
        InboundTime iti; iti.ownerId = ownerId; iti.pkt = pkt;
        NativeLock nativeLock625(&cs_); time_.push_back(iti); nativeLock625.unlock();
    }
    // NET thread: one received baked-door state row (protocol 26), owner-tagged.
    void pushDoor(u32 ownerId, const DoorPacket& pkt) {
        InboundDoor ido; ido.ownerId = ownerId; ido.pkt = pkt;
        NativeLock nativeLock630(&cs_); door_.push_back(ido); nativeLock630.unlock();
    }
    // NET thread: one received machine state row (protocol 33), owner-tagged.
    void pushProd(u32 ownerId, const ProdPacket& pkt) {
        InboundProd ip; ip.ownerId = ownerId; ip.pkt = pkt;
        NativeLock nativeLock635(&cs_); prod_.push_back(ip); nativeLock635.unlock();
    }
    // NET thread: one received known-research row (protocol 38), owner-tagged.
    void pushResearch(u32 ownerId, const ResearchPacket& pkt) {
        InboundResearch ir; ir.ownerId = ownerId; ir.pkt = pkt;
        NativeLock nativeLock640(&cs_); research_.push_back(ir); nativeLock640.unlock();
    }
    // NET thread: one received property-deed row (protocol 54), owner-tagged.
    void pushDeed(u32 ownerId, const DeedPacket& pkt) {
        InboundDeed id; id.ownerId = ownerId; id.pkt = pkt;
        NativeLock nativeLock645(&cs_); deed_.push_back(id); nativeLock645.unlock();
    }
    // NET thread: one received fixture identity row (protocol 55), owner-tagged.
    void pushFixture(u32 ownerId, const FixturePacket& pkt) {
        InboundFixture ifx; ifx.ownerId = ownerId; ifx.pkt = pkt;
        NativeLock nativeLock650(&cs_); fixture_.push_back(ifx); nativeLock650.unlock();
    }
    // NET thread: one received placed-building announcement (protocol 27), owner-tagged.
    void pushBuildPlace(u32 ownerId, const BuildPlacePacket& pkt) {
        InboundBuildPlace ibp; ibp.ownerId = ownerId; ibp.pkt = pkt;
        NativeLock nativeLock655(&cs_); buildPlace_.push_back(ibp); nativeLock655.unlock();
    }
    // NET thread: one received construction-progress row (protocol 27), owner-tagged.
    void pushBuildState(u32 ownerId, const BuildStatePacket& pkt) {
        InboundBuildState ibs; ibs.ownerId = ownerId; ibs.pkt = pkt;
        NativeLock nativeLock660(&cs_); buildState_.push_back(ibs); nativeLock660.unlock();
    }
    // NET thread: one received placed-building door row (protocol 28), owner-tagged.
    void pushBuildDoor(u32 ownerId, const BuildDoorPacket& pkt) {
        InboundBuildDoor ibd; ibd.ownerId = ownerId; ibd.pkt = pkt;
        NativeLock nativeLock665(&cs_); buildDoor_.push_back(ibd); nativeLock665.unlock();
    }
    // NET thread: one received building removal (protocol 28), owner-tagged.
    void pushBuildRemove(u32 ownerId, const BuildRemovePacket& pkt) {
        InboundBuildRemove ibr; ibr.ownerId = ownerId; ibr.pkt = pkt;
        NativeLock nativeLock670(&cs_); buildRemove_.push_back(ibr); nativeLock670.unlock();
    }
    // Session-framed ingress, but world-scoped lifetime and reset semantics.
    void pushProcedural(u32 ownerId, const ProceduralPacket& pkt) {
        InboundProcedural p;p.ownerId=ownerId;p.pkt=pkt;
        NativeLock lock(&cs_);procedural_.push_back(p);
    }
    void pushEnvironment(u32 owner,const EnvironmentItemPacket& packet) {
        NativeLock lock(&cs_);
        EnvironmentItemPacket p=packet;p.ownerId=owner;environment_.push_back(p);
    }
    // NET thread: one received stealth detection-map snapshot, owner-tagged.
    void pushStealth(u32 ownerId, const StealthPacket& pkt) {
        InboundStealth isl; isl.ownerId = ownerId; isl.pkt = pkt;
        NativeLock nativeLock675(&cs_); stealth_.push_back(isl); nativeLock675.unlock();
    }
    // NET thread: one received runtime-spawn query (protocol 21), owner-tagged.
    void pushSpawnReq(u32 ownerId, const SpawnReqPacket& pkt) {
        InboundSpawnReq sr; sr.ownerId = ownerId; sr.pkt = pkt;
        NativeLock nativeLock680(&cs_); spawnReq_.push_back(sr); nativeLock680.unlock();
    }
    // NET thread: one received runtime-spawn description (protocol 21), owner-tagged.
    void pushSpawnInfo(u32 ownerId, const SpawnInfoPacket& pkt) {
        InboundSpawnInfo si; si.ownerId = ownerId; si.pkt = pkt;
        NativeLock nativeLock685(&cs_); spawnInfo_.push_back(si); nativeLock685.unlock();
    }
    // NET thread: coordinated-save packets (protocol 31), owner-tagged.
    void pushSaveReq(u32 ownerId, const SaveReqPacket& pkt) {
        InboundSaveReq sr; sr.ownerId = ownerId; sr.pkt = pkt;
        NativeLock nativeLock690(&cs_); saveReq_.push_back(sr); nativeLock690.unlock();
    }
    void pushSaveBegin(u32 ownerId, const SaveBeginPacket& pkt) {
        InboundSaveBegin sb; sb.ownerId = ownerId; sb.pkt = pkt;
        NativeLock nativeLock694(&cs_); saveBegin_.push_back(sb); nativeLock694.unlock();
    }
    void pushSaveFile(u32 ownerId, const SaveFileHeader& hdr,
                      const char* path, const u8* data) {
        InboundSaveFile sf;
        sf.ownerId = ownerId;
        sf.hdr     = hdr;
        sf.path.assign(path, path + hdr.pathLen);
        if (data && hdr.dataLen > 0) sf.data.assign(data, data + hdr.dataLen);
        NativeLock nativeLock703(&cs_); saveFile_.push_back(sf); nativeLock703.unlock();
    }
    void pushSaveDone(u32 ownerId, const SaveDoneHeader& hdr, const u32* crcs) {
        InboundSaveDone sd;
        sd.ownerId = ownerId;
        sd.hdr     = hdr;
        if (crcs && hdr.fileCount > 0) sd.crcs.assign(crcs, crcs + hdr.fileCount);
        NativeLock nativeLock710(&cs_); saveDone_.push_back(sd); nativeLock710.unlock();
    }
    void pushSaveAck(u32 ownerId, const SaveAckPacket& pkt) {
        InboundSaveAck sa; sa.ownerId = ownerId; sa.pkt = pkt;
        NativeLock nativeLock714(&cs_); saveAck_.push_back(sa); nativeLock714.unlock();
    }
    // NET thread: coordinated-load packets (protocol 32), owner-tagged.
    void pushLoadGo(u32 ownerId, const LoadGoPacket& pkt) {
        InboundLoadGo lg; lg.ownerId = ownerId; lg.pkt = pkt;
        NativeLock nativeLock719(&cs_); loadGo_.push_back(lg); nativeLock719.unlock();
    }
    void pushLoadReq(u32 ownerId, const LoadReqPacket& pkt) {
        InboundLoadReq lr; lr.ownerId = ownerId; lr.pkt = pkt;
        NativeLock nativeLock723(&cs_); loadReq_.push_back(lr); nativeLock723.unlock();
    }
    void pushLoadNack(u32 ownerId, const LoadNackPacket& pkt) {
        InboundLoadNack ln; ln.ownerId = ownerId; ln.pkt = pkt;
        NativeLock nativeLock727(&cs_); loadNack_.push_back(ln); nativeLock727.unlock();
    }
    // NET thread: one received camera hint (protocol 43), owner-tagged.
    void pushCamHint(u32 ownerId, const CamHintPacket& pkt) {
        InboundCamHint ch; ch.ownerId = ownerId; ch.pkt = pkt;
        NativeLock nativeLock732(&cs_); camHint_.push_back(ch); nativeLock732.unlock();
    }
    // NET thread: one received cell claim (protocol 49), owner-tagged.
    void pushCellClaim(u32 ownerId, const CellClaimPacket& pkt) {
        InboundCellClaim cc; cc.ownerId = ownerId; cc.pkt = pkt;
        NativeLock nativeLock737(&cs_); cellClaim_.push_back(cc); nativeLock737.unlock();
    }

    // MAIN thread: move all pending items into 'out' (empty on entry).
    void drainConnects(std::deque<u32>& out) {
        NativeLock nativeLock742(&cs_); out.swap(conn_); nativeLock742.unlock();
    }
    void drainLeaves(std::deque<u32>& out) {
        NativeLock nativeLock745(&cs_); out.swap(leave_); nativeLock745.unlock();
    }
    void drainEntities(std::deque<InboundEntity>& out) {
        NativeLock nativeLock748(&cs_); out.swap(ent_); nativeLock748.unlock();
    }
    void drainEvents(std::deque<InboundEvent>& out) {
        NativeLock nativeLock751(&cs_); out.swap(evt_); nativeLock751.unlock();
    }
    // A verdict cannot overtake the preceding reliable canonical snapshot.
    void drainInventory(std::deque<InboundInv>& out,
                        std::deque<InboundInvXferAck>& verdicts) {
        NativeLock nativeLock756(&cs_);
        out.swap(inv_); verdicts.swap(invXferAck_);
        nativeLock756.unlock();
    }
    void drainWorldItems(std::deque<InboundWorldItems>& out) {
        NativeLock nativeLock761(&cs_); out.swap(wi_); nativeLock761.unlock();
    }
    void drainWorldRemove(std::deque<InboundWorldRemove>& out) {
        NativeLock nativeLock764(&cs_); out.swap(wir_); nativeLock764.unlock();
    }
    void drainWorldClaim(std::deque<InboundWorldClaim>& out) {
        NativeLock nativeLock767(&cs_); out.swap(wic_); nativeLock767.unlock();
    }
    void drainNpcCensus(std::deque<InboundNpcCensus>& out) {
        NativeLock nativeLock770(&cs_); out.swap(npcCensus_); nativeLock770.unlock();
    }
    void drainGroundGear(std::deque<InboundGroundGear>& out) {
        NativeLock lock(&cs_); out.swap(groundGear_);
    }
    void drainInvXfers(std::deque<InboundInvXfer>& out) {
        NativeLock nativeLock779(&cs_); out.swap(invXfer_); nativeLock779.unlock();
    }
    void drainMedical(std::deque<InboundMedical>& out) {
        NativeLock nativeLock782(&cs_); out.swap(med_); nativeLock782.unlock();
    }
    void drainTreatments(std::deque<InboundTreatment>& out) {
        NativeLock nativeLock785(&cs_); out.swap(treat_); nativeLock785.unlock();
    }
    void drainCombatHits(std::deque<InboundCombatHit>& out) {
        NativeLock nativeLock788(&cs_); out.swap(combatHit_); nativeLock788.unlock();
    }
    void drainSpeed(std::deque<InboundSpeed>& out) {
        NativeLock nativeLock791(&cs_); out.swap(speed_); nativeLock791.unlock();
    }
    void drainStats(std::deque<InboundStats>& out) {
        NativeLock nativeLock794(&cs_); out.swap(stats_); nativeLock794.unlock();
    }
    void drainFaction(std::deque<InboundFaction>& out) {
        NativeLock nativeLock797(&cs_); out.swap(faction_); nativeLock797.unlock();
    }
    void drainTime(std::deque<InboundTime>& out) {
        NativeLock nativeLock800(&cs_); out.swap(time_); nativeLock800.unlock();
    }
    void drainDoor(std::deque<InboundDoor>& out) {
        NativeLock nativeLock803(&cs_); out.swap(door_); nativeLock803.unlock();
    }
    void drainProd(std::deque<InboundProd>& out) {
        NativeLock nativeLock806(&cs_); out.swap(prod_); nativeLock806.unlock();
    }
    void drainResearch(std::deque<InboundResearch>& out) {
        NativeLock nativeLock809(&cs_); out.swap(research_); nativeLock809.unlock();
    }
    void drainDeed(std::deque<InboundDeed>& out) {
        NativeLock nativeLock812(&cs_); out.swap(deed_); nativeLock812.unlock();
    }
    void drainFixture(std::deque<InboundFixture>& out) {
        NativeLock nativeLock815(&cs_); out.swap(fixture_); nativeLock815.unlock();
    }
    void drainBuildPlace(std::deque<InboundBuildPlace>& out) {
        NativeLock nativeLock818(&cs_); out.swap(buildPlace_); nativeLock818.unlock();
    }
    void drainBuildState(std::deque<InboundBuildState>& out) {
        NativeLock nativeLock821(&cs_); out.swap(buildState_); nativeLock821.unlock();
    }
    void drainBuildDoor(std::deque<InboundBuildDoor>& out) {
        NativeLock nativeLock824(&cs_); out.swap(buildDoor_); nativeLock824.unlock();
    }
    void drainBuildRemove(std::deque<InboundBuildRemove>& out) {
        NativeLock nativeLock827(&cs_); out.swap(buildRemove_); nativeLock827.unlock();
    }
    void drainProcedural(std::deque<InboundProcedural>& out) {
        NativeLock lock(&cs_);out.swap(procedural_);
    }
    void drainEnvironment(std::deque<EnvironmentItemPacket>& out) {
        NativeLock lock(&cs_);out.swap(environment_);
    }
    void drainMoney(std::deque<InboundMoney>& out) {
        NativeLock nativeLock830(&cs_); out.swap(money_); nativeLock830.unlock();
    }
    void drainMoneyDeltas(std::deque<InboundMoneyDelta>& out) {
        NativeLock nativeLock833(&cs_); out.swap(moneyDelta_); nativeLock833.unlock();
    }
    void drainStealth(std::deque<InboundStealth>& out) {
        NativeLock nativeLock836(&cs_); out.swap(stealth_); nativeLock836.unlock();
    }
    void drainSpawnReqs(std::deque<InboundSpawnReq>& out) {
        NativeLock nativeLock839(&cs_); out.swap(spawnReq_); nativeLock839.unlock();
    }
    void drainSpawnInfos(std::deque<InboundSpawnInfo>& out) {
        NativeLock nativeLock842(&cs_); out.swap(spawnInfo_); nativeLock842.unlock();
    }
    void drainSaveReqs(std::deque<InboundSaveReq>& out) {
        NativeLock nativeLock845(&cs_); out.swap(saveReq_); nativeLock845.unlock();
    }
    // BEGIN/FILE/DONE share one ordered native bulk lane. Drain under one lock:
    // DONE must not overtake FILEs arriving between separate drains.
    void drainSaveTransfer(std::deque<InboundSaveBegin>& begins,
                           std::deque<InboundSaveFile>& files,
                           std::deque<InboundSaveDone>& dones) {
        NativeLock nativeLock852(&cs_);
        begins.swap(saveBegin_);
        files.swap(saveFile_);
        dones.swap(saveDone_);
        nativeLock852.unlock();
    }
    void drainSaveAcks(std::deque<InboundSaveAck>& out) {
        NativeLock nativeLock859(&cs_); out.swap(saveAck_); nativeLock859.unlock();
    }
    void drainLoadGos(std::deque<InboundLoadGo>& out) {
        NativeLock nativeLock862(&cs_); out.swap(loadGo_); nativeLock862.unlock();
    }
    void drainLoadReqs(std::deque<InboundLoadReq>& out) {
        NativeLock nativeLock865(&cs_); out.swap(loadReq_); nativeLock865.unlock();
    }
    void drainLoadNacks(std::deque<InboundLoadNack>& out) {
        NativeLock nativeLock868(&cs_); out.swap(loadNack_); nativeLock868.unlock();
    }
    void drainCamHints(std::deque<InboundCamHint>& out) {
        NativeLock nativeLock871(&cs_); out.swap(camHint_); nativeLock871.unlock();
    }
    void drainCellClaims(std::deque<InboundCellClaim>& out) {
        NativeLock nativeLock874(&cs_); out.swap(cellClaim_); nativeLock874.unlock();
    }

    // MAIN thread, session reset (protocol 32): drop every queued packet that
    // describes the OLD world after a reload / reconnect / disconnect edge, and
    // start a new session generation.
    //
    // Coverage is STRUCTURAL: every WorldQ registered itself into worldReset_ at
    // construction, so iterating that list clears exactly the world-state queues
    // and nothing else - a queue added later cannot be silently forgotten (the
    // bug that let a cross-owner invXfer transfer intent survive a world reload).
    // SessionQ queues (presence + the coordinated save/load handshake) never
    // register, so they survive by construction - a NACK/GO arriving mid-swap
    // must not be dropped.
    void flushWorldState() {
        NativeLock nativeLock889(&cs_);
        for (size_t i = 0; i < worldReset_.size(); ++i)
            worldReset_[i]->clearQueue();
        // Peer-readiness is world-scoped: the owned-entity batch that set
        // sawRemote_ described the OLD world, so a fresh session must
        // re-observe the peer before time-sensitive host actions (e.g. a live
        // spawn) or a scenario may arm again.
        sawRemote_ = false;
        ++generation_;
        nativeLock889.unlock();
    }

    // NET thread, authenticated incarnation departure/replacement. Only this
    // owner's queued world packets disappear; session controls survive.
    void flushOwnerWorld(u32 ownerId) {
        NativeLock nativeLock904(&cs_);
        for (size_t i = 0; i < worldReset_.size(); ++i)
            worldReset_[i]->clearOwner(ownerId);
        nativeLock904.unlock();
    }

    // Checked while beginWorldEnqueue holds cs_ (or under the outer recursive
    // native dispatch lock), so reliable ingress cannot grow without a limit.
    bool canAcceptNative() {
        NativeLock nativeLock913(&cs_);
        bool ok = true;
        for (size_t i = 0; i < worldReset_.size(); ++i)
            if (!worldReset_[i]->hasCapacity()) { ok = false; break; }
        ok = ok && conn_.hasCapacity() && leave_.hasCapacity() && saveReq_.hasCapacity() && saveBegin_.hasCapacity() &&
            saveFile_.hasCapacity() && saveDone_.hasCapacity() && saveAck_.hasCapacity() &&
            loadGo_.hasCapacity() && loadReq_.hasCapacity() && loadNack_.hasCapacity();
        nativeLock913.unlock();
        return ok;
    }

private:
    CRITICAL_SECTION          cs_;
    bool                      sawRemote_;  // set once any peer entity batch arrives
    u32                       generation_; // session generation; bumped on flush
    // World-state reset list: every WorldQ below self-registers here at
    // construction so flushWorldState() clears them structurally. MUST be
    // declared before the WorldQ members (member init order).
    std::vector<IClearableQueue*> worldReset_;

    // SESSION-PRESERVING: presence edges (the connection persists across a swap).
    SessionQ<u32>                  conn_;
    SessionQ<u32>                  leave_;

    // WORLD-STATE: describe the current world; auto-cleared on a session reset.
    WorldQ<InboundEntity>          ent_;
    WorldQ<InboundEvent>           evt_;
    WorldQ<InboundInv>             inv_;
    WorldInventoryFrames          invFrames_;
    WorldQ<InboundWorldItems>      wi_;
    WorldQ<InboundWorldRemove>     wir_;
    WorldQ<InboundWorldClaim>      wic_;
    WorldQ<InboundNpcCensus>       npcCensus_;
    WorldQ<InboundGroundGear>      groundGear_;
    WorldQ<InboundInvXfer>         invXfer_;
    WorldQ<InboundInvXferAck>      invXferAck_;
    WorldQ<InboundMedical>         med_;
    WorldQ<InboundTreatment>       treat_;
    WorldQ<InboundCombatHit>       combatHit_;
    WorldQ<InboundSpeed>           speed_;
    WorldQ<InboundStats>           stats_;
    WorldQ<InboundMoney>           money_;
    WorldQ<InboundMoneyDelta>      moneyDelta_;
    WorldQ<InboundFaction>         faction_;
    WorldQ<InboundTime>            time_;
    WorldQ<InboundDoor>            door_;
    WorldQ<InboundProd>            prod_;
    WorldQ<InboundResearch>        research_;
    WorldQ<InboundDeed>            deed_;
    WorldQ<InboundFixture>         fixture_;
    WorldQ<InboundBuildPlace>      buildPlace_;
    WorldQ<InboundBuildState>      buildState_;
    WorldQ<InboundBuildDoor>       buildDoor_;
    WorldQ<InboundBuildRemove>     buildRemove_;
    WorldQ<InboundProcedural>      procedural_;
    WorldQ<EnvironmentItemPacket> environment_;
    WorldQ<InboundStealth>         stealth_;
    WorldQ<InboundSpawnReq>        spawnReq_;
    WorldQ<InboundSpawnInfo>       spawnInfo_;
    WorldQ<InboundCamHint>         camHint_;
    WorldQ<InboundCellClaim>       cellClaim_;

    // SESSION-PRESERVING: coordinated save (protocol 31) + load (protocol 32)
    // handshake - a GO/NACK/chunk arriving mid-swap must survive the reset.
    SessionQ<InboundSaveReq>       saveReq_;
    SessionQ<InboundSaveBegin>     saveBegin_;
    SessionQ<InboundSaveFile>      saveFile_;
    SessionQ<InboundSaveDone>      saveDone_;
    SessionQ<InboundSaveAck>       saveAck_;
    SessionQ<InboundLoadGo>        loadGo_;
    SessionQ<InboundLoadReq>       loadReq_;
    SessionQ<InboundLoadNack>      loadNack_;

    Inbound(const Inbound&);
    Inbound& operator=(const Inbound&);
};

} // namespace coop

#endif // KENSHICOOP_INBOUND_H
