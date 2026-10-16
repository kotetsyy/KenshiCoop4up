// ReplicatorItems.cpp - item planes (monolith split from Replicator.cpp,
// 2026-07-12): publishInventories/applyInventories (Phase 4a container
// contents), publishWorldItems/applyWorldItems (Phase W1 ground-item
// proxies), detectAndPublishWeaponDrops + applyGroundGear (Phase W2
// gear conservation) and the protocol 37 cross-owner transfer intents
// (xferRebase/xferPendingLoss/wdSuppressed/detectAndPublishTransfers/
// applyTransfers).
//
// Shared hubs: owns invSnap_/worldItems_/weaponCensus_/xfer* bookkeeping;
// reads ownHands_ (stamped by the publish TU).
// Must NOT: change any log string - log phrasing is the API consumed by the
// PowerShell oracles (see resources/CODE_MAP.md, log-tag index).

#include "ReplicatorUtil.h"
#include "../../netproto/ContentHash.h"
#include "../game/EngineScenario.h"
#include "../game/InventoryTransactionBridge.h"
#include "../core/InventoryTransactionRules.h"

namespace coop {

Replicator::Key Replicator::keyForInvLocalHand(const unsigned int hand[5]) const {
    Key k; k.t=hand[0]; k.c=hand[1]; k.cs=hand[2]; k.i=hand[3]; k.s=hand[4];
    return k;
}
bool Replicator::queryScenarioInventoryRoot(GameWorld* gw,const unsigned int local[5],unsigned int canonical[5]) const {
    if (!gw || !local || !canonical || !engine::transactionInventory(local)) return false;
    const Key native=keyForInvLocalHand(local);
    Key wire;
    if (isHostRole()) {
        u32 published[5];u8 kind=0;
        inventorySnapshotIdentity(native,published,kind);
        wire=keyForInvLocalHand(published);
    } else wire=invWireKeyForLocal(native);
    unsigned int roundtrip[5];
    if (!resolveInvWireHand(wire,roundtrip) || memcmp(local,roundtrip,sizeof(roundtrip))) return false;
    if (!isHostRole()) {
        // An inventory is authored by HOST even when its NPC is outside the
        // movement stream. Certify the authenticated inventory binding itself,
        // not a raw shared-save hand or unrelated transform-writer ownership.
        std::map<Key,InvRecv>::const_iterator received=invRecv_.find(native);
        if (received==invRecv_.end()) return false;
        const InvRecv& r=received->second;
        if (r.ownerId!=0 || !(r.wireKey==wire) || !r.revision || r.dirty ||
            r.truncated || engine::inventoryRevision(local)!=r.revision) return false;
        static InvItemEntry current[INV_SNAPSHOT_ITEMS_MAX];
        bool truncated=false;
        const unsigned int count=engine::captureContainerContents(
            gw,local,current,INV_SNAPSHOT_ITEMS_MAX,0,&truncated,true);
        if (truncated) return false;
        // Native shop refreshes may repack their display grid. Transaction
        // identity is the full stock, equipment and nested-container contents,
        // not the screen coordinates of its icons.
        if (!inventory_transaction::equivalentCheckpoint(
                r.items.empty()?0:&r.items[0],(unsigned int)r.items.size(),current,count))
            return false;
    }
    canonical[0]=wire.t;canonical[1]=wire.c;canonical[2]=wire.cs;
    canonical[3]=wire.i;canonical[4]=wire.s;
    return true;
}

Character* Replicator::resolveEventChar(const Key& k) const {
    for (std::map<Key, Key>::const_iterator owned = publishAsWire_.begin();
         owned != publishAsWire_.end(); ++owned) {
        if ((owned->second < k) || (k < owned->second)) continue;
        const Key& native = owned->first;
        return engine::resolveCharByHand(native.i, native.s,
            native.t, native.c, native.cs);
    }
    std::map<Key, Character*>::const_iterator pit = proxyByKey_.find(k);
    if (pit == proxyByKey_.end() || !pit->second)
        return engine::resolveCharByHand(k.i, k.s, k.t, k.c, k.cs);
    // Re-ask the engine instead of handing back the cached pointer, and use the
    // TYPED hand API so the two legacy component orders cannot be swapped here -
    // getting that wrong is a silent no-op, which is the failure mode this whole
    // helper exists to remove.
    ObjectHand h;
    if (!engine::charHandOf(pit->second, h)) return 0;
    return engine::resolveChar(h);
}

bool Replicator::resolveInvLocalHand(const Key& k,unsigned int out[5]) const {
    out[0]=k.t; out[1]=k.c; out[2]=k.cs; out[3]=k.i; out[4]=k.s;
    return engine::containerInventoryAvailable(out);
}

Replicator::Key Replicator::invWireKeyForLocal(const Key& local) const {
    std::map<Key,Key>::const_iterator claim = publishAsWire_.find(local);
    if (claim != publishAsWire_.end()) return claim->second;
    std::map<Key,Key>::const_iterator build = mintByLocal_.find(local);
    if (build != mintByLocal_.end()) return build->second;
    unsigned int hand[5] = { local.t, local.c, local.cs, local.i, local.s };
    RootObject* object = engine::resolveObjectByHand(hand);
    std::map<Character*,Key>::const_iterator actor =
        canonicalOf_.find(reinterpret_cast<Character*>(object));
    if (actor != canonicalOf_.end() && actor->second.t == local.t) return actor->second;
    for (std::map<Key,FixtureRow>::const_iterator f = fixtureMap_.begin(); f != fixtureMap_.end(); ++f)
        if (f->second.resolved && memcmp(hand, f->second.hand, sizeof(hand)) == 0) return f->first;
    return local;
}

bool Replicator::resolveInvWireHand(const Key& k,unsigned int out[5]) const {
    // Claims rename a join's LOCAL squad to the host's wire identity. Never
    // confuse a colliding local hand with the peer body named by the packet.
    for (std::map<Key,Key>::const_iterator it=publishAsWire_.begin();it!=publishAsWire_.end();++it)
        if (!(it->second<k) && !(k<it->second))
            return resolveInvLocalHand(it->first,out);
    std::map<Key,Character*>::const_iterator proxy=proxyByKey_.find(k);
    std::map<Key,Key>::const_iterator localClaim=publishAsWire_.find(k);
    bool renamed=localClaim!=publishAsWire_.end() &&
                 ((localClaim->second<k) || (k<localClaim->second));
    if (!renamed && !fixtureMap_.count(k) && !ownBuilds_.count(k) && !peerBuilds_.count(k) &&
        resolveInvLocalHand(k,out) &&
        (ownerClassForHand(out) == 1 || proxy == proxyByKey_.end() || !proxy->second)) return true;
    if (proxy!=proxyByKey_.end() && proxy->second &&
        engine::readObjectHand(reinterpret_cast<RootObject*>(proxy->second),out) &&
        engine::containerInventoryAvailable(out)) return true;
    std::map<Key,OwnBuild>::const_iterator own=ownBuilds_.find(k);
    if (own!=ownBuilds_.end())
        return !own->second.removed && resolveInvLocalHand(keyForInvLocalHand(own->second.hand),out);
    std::map<Key,PeerBuild>::const_iterator peer=peerBuilds_.find(k);
    if (peer!=peerBuilds_.end())
        return peer->second.minted==1 && !peer->second.removed &&
               resolveInvLocalHand(keyForInvLocalHand(peer->second.localHand),out);
    if (fixtureMap_.count(k)) return localHandForFixtureKey(k,out);
    return !renamed && resolveInvLocalHand(k,out);
}

void Replicator::inventorySnapshotIdentity(const Key& local, u32 wireKey[5], u8& keyKind) const {
    Key wire = local;
    keyKind = ownBuilds_.count(local) ? 1 : 0;
    std::map<Key, Key>::const_iterator mint = mintByLocal_.find(local);
    if (mint != mintByLocal_.end()) { keyKind = 1; wire = mint->second; }
    std::map<Key, Key>::const_iterator claim = publishAsWire_.find(local);
    if (claim != publishAsWire_.end()) wire = claim->second;
    wireKey[0] = wire.t; wireKey[1] = wire.c; wireKey[2] = wire.cs;
    wireKey[3] = wire.i; wireKey[4] = wire.s;
}

bool Replicator::inventoryCommitPending(const unsigned int hand[5]) const {
    // An unavailable native inventory is unknown, not an authoritative empty state.
    if (!engine::containerInventoryAvailable(hand)) return true;
    // A cursor lifetime does not depend on which inventory panel is visible.
    if (engine::itemTransferPending(hand)) return true;
    Key local = keyForInvLocalHand(hand);
    std::map<Key,InvRecv>::const_iterator commit=invRecv_.find(local);
    if (commit!=invRecv_.end() && commit->second.transaction && commit->second.dirty)
        return true;
    if (wdPendingDrop(local)) return true;
    for (std::map<u32,XferOut>::const_iterator x=xferOut_.begin();x!=xferOut_.end();++x)
        if ((!x->second.srcPeer && !(x->second.src < local) && !(local < x->second.src)) ||
            (!x->second.dstPeer && !(x->second.dst < local) && !(local < x->second.dst)))
            return true;
    return false;
}

void Replicator::queueXferVerdict(GameWorld* gw, NetLink& net, const InvXferPacket& intent,
                                  const InboundInvXferAck& result) {
    if (intent.operation==1 || intent.operation==2) {
        if (!isHostRole()) return;
        const u32 endpoints[2][5]={{intent.sType,intent.sContainer,intent.sContainerSerial,
                                   intent.sIndex,intent.sSerial},
                                  {intent.dType,intent.dContainer,intent.dContainerSerial,
                                   intent.dIndex,intent.dSerial}};
        // Destination is queued first; the bundled source/ACK closes the barrier.
        for (int endpoint=1;endpoint>=0;--endpoint) {
            Key key=keyForInvLocalHand(endpoints[endpoint]); unsigned int native[5];
            if (!resolveInvWireHand(key,native)) return;
            static InvItemEntry rows[INV_SNAPSHOT_ITEMS_MAX]; bool truncated=false; u32 hash=0;
            unsigned int count=engine::captureContainerContents(gw,native,rows,INV_SNAPSHOT_ITEMS_MAX,
                                                                &hash,&truncated,true);
            u32 wire[5]; u8 kind; inventorySnapshotIdentity(keyForInvLocalHand(native),wire,kind);
            u32 revision=ownsInventoryLocalHand(native) ?
                engine::observeInventoryRevision(native,hash) : engine::nextInventoryCheckpoint(native);
            u8 flags=INV_FLAG_TRANSACTION | (truncated ? INV_FLAG_TRUNCATED : 0);
            if (!ownsInventoryLocalHand(native)) {
                flags|=INV_FLAG_CHECKPOINT;
                inventoryAwaitingActorAck_[invWireKeyForLocal(keyForInvLocalHand(native))]=
                    std::make_pair(intent.ownerId,revision);
            }
            if (endpoint) net.queueInvSnapshot(0,kind,wire,rows,count,flags,revision);
            else {
                InvSnapshotHeader snapshot; memset(&snapshot,0,sizeof(snapshot));
                snapshot.type=PKT_INV_SNAPSHOT; snapshot.ownerId=0; snapshot.keyKind=kind;
                snapshot.flags=flags; snapshot.revision=revision;
                snapshot.cType=wire[0]; snapshot.cContainer=wire[1];
                snapshot.cContainerSerial=wire[2]; snapshot.cIndex=wire[3]; snapshot.cSerial=wire[4];
                snapshot.count=(u16)count;
                net.queueInvXferAck(result.pkt,result.contents,snapshot,rows);
            }
        }
        return;
    }
    unsigned int hand[5];
    Key source; source.t = intent.sType; source.c = intent.sContainer;
    source.cs = intent.sContainerSerial; source.i = intent.sIndex; source.s = intent.sSerial;
    if (!resolveInvWireHand(source, hand) || !ownsInventoryLocalHand(hand)) {
        Key destination; destination.t = intent.dType; destination.c = intent.dContainer;
        destination.cs = intent.dContainerSerial; destination.i = intent.dIndex;
        destination.s = intent.dSerial;
        if (!resolveInvWireHand(destination, hand) || !ownsInventoryLocalHand(hand)) return;
    }
    static InvItemEntry items[INV_SNAPSHOT_ITEMS_MAX]; bool truncated = false;
    unsigned int count = 0;
    bool unsettled = inventoryCommitPending(hand);
    if (unsettled) {
        // The verdict must still leave: waiting for another outgoing verdict here
        // would deadlock simultaneous trades. An empty INCOMPLETE description says
        // "unknown", never "gone", and exposes no other speculative item.
        truncated = true;
    } else {
        count = engine::captureContainerContents(gw, hand, items, INV_SNAPSHOT_ITEMS_MAX,
                                                 0, &truncated, true);
    }
    u32 wireKey[5]; u8 keyKind;
    inventorySnapshotIdentity(keyForInvLocalHand(hand), wireKey, keyKind);
    InvSnapshotHeader snapshot; memset(&snapshot, 0, sizeof(snapshot));
    snapshot.type = (u8)PKT_INV_SNAPSHOT; snapshot.ownerId = result.pkt.ownerId;
    snapshot.keyKind = keyKind; snapshot.flags = truncated ? INV_FLAG_TRUNCATED : 0;
    snapshot.cType = wireKey[0]; snapshot.cContainer = wireKey[1];
    snapshot.cContainerSerial = wireKey[2]; snapshot.cIndex = wireKey[3];
    snapshot.cSerial = wireKey[4]; snapshot.count = (u16)count;
    u32 snapshotHash=0;
    for (unsigned int r=0;r<count;++r) snapshotHash+=invEntryHash(items[r]);
    snapshot.revision=truncated ? engine::inventoryRevision(hand) :
                                 engine::observeInventoryRevision(hand,snapshotHash);
    if (truncated) {
        Key canonical; canonical.t = snapshot.cType; canonical.c = snapshot.cContainer;
        canonical.cs = snapshot.cContainerSerial; canonical.i = snapshot.cIndex;
        canonical.s = snapshot.cSerial;
        deferredInvCommits_.insert(canonical);
    }
    net.queueInvXferAck(result.pkt, result.contents, snapshot, items);
}

void Replicator::publishDeferredInventoryCommits(GameWorld* gw, NetLink& net, u8 localOwnerId) {
    if (deferredInvCommits_.empty()) return;
    static InvItemEntry items[INV_SNAPSHOT_ITEMS_MAX];
    unsigned long now = nowMs();
    for (std::set<Key>::iterator it = deferredInvCommits_.begin();
         it != deferredInvCommits_.end();) {
        unsigned int hand[5];
        if (!resolveInvWireHand(*it, hand)) { ++it; continue; }
        if (!ownsInventoryLocalHand(hand)) { deferredInvCommits_.erase(it++); continue; }
        if (inventoryCommitPending(hand)) { ++it; continue; }
        Key local = keyForInvLocalHand(hand);
        u32 wireKey[5]; u8 keyKind;
        inventorySnapshotIdentity(local, wireKey, keyKind);
        u32 hash = 0;
        bool truncated = false;
        unsigned int count = engine::captureContainerContents(gw, hand, items, INV_SNAPSHOT_ITEMS_MAX,
                                                             &hash, &truncated, true);
        // An incomplete native capture cannot discharge the full-commit obligation.
        if (truncated) { ++it; continue; }
        net.queueInvSnapshot(localOwnerId, keyKind, wireKey, items, count, (u8)0,
                             engine::observeInventoryRevision(hand,hash));
        std::map<Key, InvPub>::iterator pub = invPub_.find(local);
        if (pub != invPub_.end()) {
            unsigned int units = 0;
            for (unsigned int i = 0; i < count; ++i)
                units += (items[i].quantity < 1) ? 1u : (unsigned int)items[i].quantity;
            pub->second.hash = hash; pub->second.lastSendMs = now;
            pub->second.pendingHash = hash; pub->second.pendingSince = now;
            pub->second.lastSentN = count; pub->second.lastSentUnits = units;
        }
        deferredInvCommits_.erase(it++);
    }
}

void Replicator::publishInventories(GameWorld* gw, NetLink& net, u32 ownerId) {
    engine::configureInventoryTransactions(isHostRole(),moneySync_ || storeSync_,moneySync_);
    if (!isHostRole()) {
        for (std::map<Key,u32>::iterator ack=inventoryCheckpointAcks_.begin();
             ack!=inventoryCheckpointAcks_.end();++ack) {
            u32 key[5]={ack->first.t,ack->first.c,ack->first.cs,ack->first.i,ack->first.s};
            net.queueInvSnapshot(ownerId,0,key,0,0,INV_FLAG_CHECKPOINT_ACK,ack->second);
        }
        inventoryCheckpointAcks_.clear();
    }
    unsigned int merchantHands[64][5];
    unsigned int merchantCount=engine::enumMerchantInventoryHands(gw,merchantHands,64);
    if (isHostRole()) {
        for (std::set<u32>::iterator peer=inventoryResyncPeers_.begin();
             peer!=inventoryResyncPeers_.end();) {
            bool found=false;
            for (std::map<Key,u32>::const_iterator actor=playerActorOwners_.begin();
                 actor!=playerActorOwners_.end();++actor) {
                if (actor->second!=*peer) continue;
                found=true; inventoryResyncActors_[actor->first]=*peer;
            }
            if (found) inventoryResyncPeers_.erase(peer++);
            else ++peer;
        }
        for (std::map<Key,u32>::iterator actor=inventoryResyncActors_.begin();
             actor!=inventoryResyncActors_.end();) {
            unsigned int hand[5];
            if (!resolveInvWireHand(actor->first,hand) || inventoryCommitPending(hand)) {
                ++actor; continue;
            }
            static InvItemEntry contents[INV_SNAPSHOT_ITEMS_MAX]; bool truncated=false;
            unsigned int count=engine::captureContainerContents(gw,hand,contents,INV_SNAPSHOT_ITEMS_MAX,
                                                                0,&truncated,true);
            if (truncated) { ++actor; continue; }
            u32 key[5]; u8 kind; inventorySnapshotIdentity(keyForInvLocalHand(hand),key,kind);
            const u32 checkpoint=engine::nextInventoryCheckpoint(hand);
            inventoryAwaitingActorAck_[actor->first]=std::make_pair(actor->second,checkpoint);
            net.queueInvSnapshot(0,kind,key,contents,count,
                                 INV_FLAG_TRANSACTION|INV_FLAG_CHECKPOINT,checkpoint);
            inventoryResyncActors_.erase(actor++);
        }
    }
    // Author the inventory of every container we OWN. ownHands_ is the per-tick set of
    // owned squad-member hands (a character's own hand IS its personal-inventory
    // container hand), so streaming it makes inventory sync fully BIDIRECTIONAL and
    // disjoint by the same tab-rank partition as positional sync (host streams tab-0
    // members, join streams tab-1 members - Doctrine 8). ownedContainers_ adds any
    // explicitly-registered non-squad container (e.g. a baked storage chest / the
    // SETUP-seeded leader). applyInventories skips this same union, so no client ever
    // reconciles a container it authors.
    std::set<Key> owned = ownedContainers_;
    owned.insert(ownHands_.begin(), ownHands_.end());
    if (isHostRole() && moneySync_) {
        for (unsigned int m=0;m<merchantCount;++m) {
            Key merchant=keyForInvLocalHand(merchantHands[m]);
            owned.insert(merchant);
            censusContainers_.insert(merchant);
        }
    }
    // Protocol 34 (storeSync, HOST only): fold in the ~1 Hz container census -
    // every COMPLETE storage chest / machine container near the interest
    // centers becomes an authored container, riding the same per-container
    // hash + settle + safety-resend gate below. The census set is replaced
    // wholesale each pass (containers leaving interest stop being captured;
    // their invPub_ baseline survives for the return).
    if (storeSync_ && isHostRole()) {
        unsigned long cnow = nowMs();
        if (contCensusMs_ == 0 || (cnow - contCensusMs_) >= 1000) {
            contCensusMs_ = cnow;
            const unsigned int MAX_STORE  = 96;
            const unsigned int MAX_CORPSE = 32;
            const float STORE_R  = 400.0f;
            const float CORPSE_R = 400.0f;
            static engine::ContRead stores[MAX_STORE];   // main-thread only
            static engine::ContRead corpses[MAX_CORPSE];
            unsigned int ns = engine::enumContainersNear(gw, STORE_R, stores, MAX_STORE);
            unsigned int nc = engine::enumLootInventoriesNear(gw, CORPSE_R,
                                                                corpses, MAX_CORPSE, true);
            censusContainers_.clear();
            for (unsigned int i = 0; i < ns; ++i) {
                if (!stores[i].hasInv) continue;
                Key k; k.t = stores[i].hand[0]; k.c = stores[i].hand[1];
                k.cs = stores[i].hand[2]; k.i = stores[i].hand[3];
                k.s = stores[i].hand[4];
                censusContainers_.insert(k);
            }
            for (unsigned int i = 0; i < nc; ++i) {
                if (!corpses[i].hasInv) continue;
                Key k; k.t = corpses[i].hand[0]; k.c = corpses[i].hand[1];
                k.cs = corpses[i].hand[2]; k.i = corpses[i].hand[3];
                k.s = corpses[i].hand[4];
                if (ownHands_.count(k)) continue; // squad pocket already authored
                censusContainers_.insert(k);
            }
            for (unsigned int m=0;m<merchantCount && moneySync_;++m)
                censusContainers_.insert(keyForInvLocalHand(merchantHands[m]));
            char cb[128];
            _snprintf(cb, sizeof(cb) - 1,
                      "[inv] CENSUS store=%u lootNpc=%u authored=%u",
                      ns, nc, (unsigned)censusContainers_.size());
            cb[sizeof(cb) - 1] = '\0'; coop::logLine(cb);
        }
        owned.insert(censusContainers_.begin(), censusContainers_.end());
    }
    if (owned.empty()) return;
    // Periodic safety resend (late join / a container returning to interest). The channel
    // is RELIABLE, so this is not loss recovery - it is a cheap re-assert. With
    // INV_ITEMS_MAX at 64 a full snapshot is ~10 KB, so a censused chest farm re-asserting
    // every 5 s would cost real bandwidth for nothing; big snapshots re-assert on the slow
    // interval instead. Small ones keep the historical cadence.
    const unsigned long INV_RESEND_MS      = 5000;
    const unsigned long INV_RESEND_BIG_MS  = 30000;
    const unsigned int  INV_RESEND_BIG_N   = 24;   // entries above which "big" applies
    // A changed snapshot must be STABLE this long before we publish it. A change that only
    // REARRANGES or ADDS (entry count >= last sent) settles fast. A change that REMOVES an
    // entry settles much longer: mid-drag the UI holds the dragged item on the CURSOR, out
    // of the inventory entirely, for up to ~1 s - a transient "item gone" the peer would act
    // on by DESTROYING a worn item. Weapons DO refabricate now (spike 451 recipe), but a
    // destroy+refabricate round-trip still loses identity/quality and churns, so the long
    // window stays. Equip and unequip-to-bag keep the entry count (a MOVE), so they still
    // replicate promptly; only genuine removals (and the in-cursor flicker) wait it out.
    const unsigned long INV_SETTLE_MS        = 350;
    // 1800 -> 1000: the comment above measures the cursor hold at "up to ~1 s",
    // so 1800 was nearly double the hazard it guards. Combined with the panel
    // test below - which skips this window entirely when no drag is possible -
    // the worst case a player can feel drops from about two seconds to one.
    const unsigned long INV_REMOVE_SETTLE_MS = 1000;
    static InvItemEntry items[INV_SNAPSHOT_ITEMS_MAX];
    unsigned long now = nowMs();
    for (std::set<Key>::iterator it = owned.begin();
         it != owned.end(); ++it) {
        unsigned int cHand[5] = { it->t, it->c, it->cs, it->i, it->s };
        if (inventoryCommitPending(cHand)) continue;
        // Do NOT publish a container whose incoming snapshot we have not applied
        // yet. While an inventory panel is open here the apply is deferred (see
        // applyInventories), so this copy is knowingly behind the peer's - and
        // shipping it as authoritative is what duplicated loot: the peer applied
        // our stale, still-full corpse and its reconcile CREATED back the items
        // it had just taken. Session 12:31:30-12:32:05, host deferred for 35 s
        // while the join's remaining-count oscillated 1 -> 2 -> 1 -> 0 -> 1.
        // The peer keeps publishing what IT holds, so nothing is lost: the moment
        // the window closes we apply that and resume from an agreed state.
        //
        // Confirmed against the LIVE panel rather than the flag alone: guiDefer_
        // is only cleared when a further snapshot arrives, so a container that
        // was deferred once and then went quiet would otherwise stay muted for
        // the rest of the session. The map lookup is the cheap gate - only a
        // container already known to be deferred pays for the panel probe.
        if (guiDefer_.count(*it) != 0) {
            if (engine::containerGuiNeedsDefer(gw, cHand)) continue;
            guiDefer_.erase(*it);
            guiDeferSaid_.erase(*it);
        }
        u32 hash = 0;
        bool trunc = false;
        // includeNested (protocol 48): the ONLY capture that wants a worn container's own
        // contents, because this snapshot is what tells the peer where a bagged item lives.
        // Every other reader leaves it off - a nested entry describes a different inventory.
        unsigned int n = engine::captureContainerContents(gw, cHand, items, INV_SNAPSHOT_ITEMS_MAX,
                                                          &hash, &trunc, /*includeNested=*/true);
        // Total UNITS across the capture: the removal-settle signal (see InvPub).
        unsigned int units = 0;
        for (unsigned int ui = 0; ui < n; ++ui)
            units += (items[ui].quantity < 1) ? 1u : (unsigned int)items[ui].quantity;
        std::map<Key, InvPub>::iterator pit = invPub_.find(*it);
        bool first = (pit == invPub_.end());
        if (first) {
            // Track from now; let it settle before the initial publish (cheap, and avoids
            // emitting a half-built inventory captured mid-load).
            InvPub p; p.hash = 0; p.lastSendMs = 0; p.pendingHash = hash; p.pendingSince = now;
            p.lastSentN = 0; p.lastSentUnits = 0;
            p.revision=0;
            invPub_[*it] = p;
            pit = invPub_.find(*it);
        }
        InvPub& pub = pit->second;
        const u32 revision=engine::observeInventoryRevision(cHand,hash);
        bool sent = (pub.lastSendMs != 0) || (pub.hash != 0);
        // A completed remove/restore can return to the same contents during
        // settling. Its newer transaction revision must not wait for a resend.
        bool differs = !sent || (pub.hash != hash) || (pub.revision != revision);
        // Maintain the settle timer: restart it whenever the captured fingerprint moves.
        if (hash != pub.pendingHash) { pub.pendingHash = hash; pub.pendingSince = now; }
        // A removal (fewer UNITS than last sent) waits out the long window; everything
        // else (additions, equip<->loose moves) settles fast. Units not entries: at the
        // INV_ITEMS_MAX cap the entry count cannot fall, which silently disabled this
        // guard for exactly the full inventories that need it most.
        // The long window exists for ONE thing: an item held on the mouse cursor
        // mid-drag is out of the inventory entirely, and publishing that transient
        // would have the peer destroy a worn item. Dragging requires an OPEN
        // PANEL on this container - with no panel there is no cursor, no
        // transient, and nothing to wait out. So the long settle now applies only
        // where the hazard can actually occur, and a corpse being looted from the
        // other machine replicates at the short window instead of ~2 s later,
        // which is the delay reported after 0.1.13 made loot live at all.
        bool dragPossible = engine::containerGuiOpen(gw, cHand);
        unsigned long settleMs = (sent && units < pub.lastSentUnits && dragPossible)
                                     ? INV_REMOVE_SETTLE_MS : INV_SETTLE_MS;
        bool settled  = (now - pub.pendingSince >= settleMs);
        bool changed  = differs && settled;
        unsigned long resendMs = (n >= INV_RESEND_BIG_N) ? INV_RESEND_BIG_MS : INV_RESEND_MS;
        bool periodic = sent && !differs && (now - pub.lastSendMs >= resendMs);
        if (!changed && !periodic) continue;
        // W2 race guard: while the gear census has an unresolved DECREASE pending for this
        // container, a drop intent for it may still be debouncing (the spatial ground query
        // fails in towns, so detectAndPublishWeaponDrops retries for up to MAX_RETRY ticks).
        // Publishing the post-drop bag NOW would reach the peer first, its reconcile would
        // destroy the bag copy, and the late intent's relocateWeaponToGround would find
        // nothing to move (moved=0) - the item would exist only on the dropper's ground.
        // Hold the snapshot until the drop plane has spoken. Same cross-plane gating
        // pattern as wdSuppressed / xferPendingLoss. No worldSync gate needed: the census
        // this reads is only ever populated by detectAndPublishWeaponDrops, which the tick
        // skips entirely when world sync is off - so the predicate is false by construction.
        if (wdPendingDrop(*it)) {
            static int dumpHold = -1;
            if (dumpHold < 0) { const char* e = getenv("KENSHICOOP_INV_DUMP"); dumpHold = (e && e[0] == '1') ? 1 : 0; }
            if (dumpHold) { char b[160]; _snprintf(b, sizeof(b) - 1,
                "[inv] HOLD hand=%u,%u,%u,%u,%u (gear decrease pending drop adjudication)",
                it->t, it->c, it->cs, it->i, it->s);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b); }
            continue;
        }
        // Use the same authority identity for periodic snapshots and transfer commits.
        u8 keyKind;
        u32 wireKey[5];
        inventorySnapshotIdentity(*it, wireKey, keyKind);
        u8 sflags = trunc ? INV_FLAG_TRUNCATED : (u8)0;
        net.queueInvSnapshot(ownerId, keyKind, wireKey, items, n, sflags,revision);
        pub.hash = hash; pub.revision = revision;
        pub.lastSendMs = now; pub.lastSentN = n; pub.lastSentUnits = units;
        if (changed) {
            char b[200];
            _snprintf(b, sizeof(b) - 1,
                "[inv] SEND hand=%u,%u,%u,%u,%u kind=%u key=%u,%u,%u,%u,%u items=%u hash=%u",
                it->t, it->c, it->cs, it->i, it->s, (unsigned)keyKind,
                wireKey[0], wireKey[1], wireKey[2], wireKey[3], wireKey[4],
                n, hash);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            // Additive-only on the peer from here: surface it so the oracles can see when
            // a container is too big to describe (and therefore diverging, not converging).
            if (trunc) {
                char t[160]; _snprintf(t, sizeof(t) - 1,
                    "[inv] SEND-TRUNCATED hand=%u,%u,%u,%u,%u items=%u cap=%u "
                    "(peer reconcile is additive-only)",
                    it->t, it->c, it->cs, it->i, it->s, n, INV_SNAPSHOT_ITEMS_MAX);
                t[sizeof(t) - 1] = '\0'; coop::logLine(t);
            }
            static int dumpInv = -1;
            if (dumpInv < 0) { const char* e = getenv("KENSHICOOP_INV_DUMP"); dumpInv = (e && e[0] == '1') ? 1 : 0; }
            if (dumpInv) { coop::logLine("[inv] SEND-state:"); engine::dumpInventory(gw, cHand); }
        }
    }
}

void Replicator::applyInventories(GameWorld* gw) {
    if (invRecv_.empty()) return;
    // The engine defers every inventory application while the native cursor
    // owns detached stock. Preserve dirty baselines and revisions as well:
    // consuming one here would let a later commerce commit use stale contents.
    if (engine::nativeInventoryCursorActive()) return;
    for (std::map<Key, InvRecv>::iterator it = invRecv_.begin(); it != invRecv_.end(); ++it) {
        if (!it->second.dirty) continue;
        it->second.dirty = false;
        // Never reconcile a container we author (defense-in-depth on the partition):
        // any explicitly-registered container OR any squad member we own this tick.
        const bool hostCommit=it->second.transaction && it->second.ownerId==0 && !isHostRole();
        if (!hostCommit && ownedContainers_.count(it->first) != 0) continue;
        if (!hostCommit && ownHands_.count(it->first) != 0) continue;
        if (isHostRole() && censusContainers_.count(it->first)) continue;
        const Key& k = it->first;
        // A mine's container key is the AUTHOR's hand for its own instance of
        // the node's building, which names nothing here - reconciling it
        // verbatim was a silent no-op, so the mine's output never crossed.
        unsigned int cHand[5];
        if (!resolveInvLocalHand(k,cHand)) { it->second.dirty=true; continue; }
        const bool merchant=!isHostRole() && engine::isMerchantInventory(cHand);
        const bool needsCheckpoint=hostCommit || merchant || engine::inventoryCheckpointPending(cHand);
        if (needsCheckpoint) engine::setInventoryCheckpointPending(cHand,true);
        // Additive rows cannot discharge either a transaction or a disconnected
        // writer's checkpoint. Keep its gate and transaction context intact.
        if (needsCheckpoint && it->second.truncated) { it->second.dirty=true; continue; }
        // Neither a source loss nor a destination gain may reconcile around a
        // pending exact physical drop/pickup, including late-loaded roots.
        if (wdPendingDrop(k)) { it->second.dirty=true; continue; }
        const InvItemEntry* items = it->second.items.empty() ? 0 : &it->second.items[0];
        unsigned int n = (unsigned int)it->second.items.size();
        // An OPEN inventory panel on this container makes the reconcile unsafe: the
        // window holds raw Item* icons the engine never revalidates, so destroying a
        // stack under it faults on the RENDER thread, outside every __except we own.
        // That is the loot crash - three swallowed first-chance READs of -1 in our own
        // capture, then a fatal one inside kenshi_x64.exe. Defer the WHOLE snapshot
        // (not just its removals): a half-applied reconcile that creates now and
        // destroys later leaves a divergence no later snapshot describes. The channel
        // is RELIABLE and re-asserts, and `dirty` re-visits every tick, so closing the
        // window applies the newest snapshot immediately - which is exactly the
        // documented outcome ("close the host's GUI -> the corpse is empty on both").
        // Held open indefinitely, the host's own panel keeps showing ghosts; that is
        // the accepted compromise, and the peer stays authoritative for what is left.
        if (engine::containerGuiNeedsDefer(gw, cHand)) {
            it->second.dirty = true; // re-visit next tick
            unsigned long now = nowMs();
            unsigned long& since = guiDefer_[k];
            if (since == 0) since = now;
            // One line per container per second: this runs at main-loop cadence.
            unsigned long& said = guiDeferSaid_[k];
            if (said == 0 || now - said >= 1000) {
                said = now;
                char b[190]; _snprintf(b, sizeof(b) - 1,
                    "[inv] GUI-DEFER hand=%u,%u,%u,%u,%u items=%u heldMs=%lu (panel open)",
                    k.t, k.c, k.cs, k.i, k.s, n, (unsigned long)(now - since));
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
            continue;
        }
        if (guiDefer_.count(k) != 0) {
            unsigned long now = nowMs();
            char b[190]; _snprintf(b, sizeof(b) - 1,
                "[inv] GUI-RESUME hand=%u,%u,%u,%u,%u items=%u heldMs=%lu",
                k.t, k.c, k.cs, k.i, k.s, n, (unsigned long)(now - guiDefer_[k]));
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            guiDefer_.erase(k);
            guiDeferSaid_.erase(k);
        }
        if (hostCommit && !it->second.truncated) {
            for (std::map<u32,XferOut>::iterator orphan=xferOut_.begin();orphan!=xferOut_.end();) {
                XferOut& operation=orphan->second;
                bool actor=(!operation.srcPeer && !(operation.src<k) && !(k<operation.src)) ||
                           (!operation.dstPeer && !(operation.dst<k) && !(k<operation.dst));
                if (!operation.retired || !actor) { ++orphan; continue; }
                // The complete host actor snapshot resolves the commit ambiguity.
                // No guessed reversal: reconcile it after releasing exact reserves.
                engine::finishItemTransfer(operation.verdictToken);
                if (operation.operation!=1 && operation.operation!=2) {
                    const int outstanding=operation.answered ?
                        operation.appliedQty+operation.rollbackQty : operation.qty;
                    if (operation.srcPeer) releaseXferLatch(operation.src,operation.key,+outstanding);
                    if (operation.dstPeer) releaseXferLatch(operation.dst,operation.key,-outstanding);
                }
                xferOut_.erase(orphan++);
            }
        }
        if (engine::itemTransferPending(cHand)) {
            it->second.dirty=true; continue;
        }
        // A stale parent-only list must never clear a newly transferred bag's contents.
        // Wait for the authority snapshot as a whole; do not rewrite quantities/parent indices.
        std::map<Key,std::map<XKey,XferLatch> >::iterator lt=xferLatch_.find(k);
        if (lt!=xferLatch_.end()) {
            static InvItemEntry local[INV_SNAPSHOT_ITEMS_MAX];
            unsigned int nl=engine::captureContainerContents(gw,cHand,local,INV_SNAPSHOT_ITEMS_MAX,0,0,true);
            for (std::map<XKey,XferLatch>::iterator l=lt->second.begin();l!=lt->second.end();) {
                int want=0,have=0;
                for (unsigned int j=0;j<n;++j)
                    if (items[j].itemType==l->first.second &&
                        strcmp(items[j].stringID,l->first.first.c_str())==0) want+=items[j].quantity;
                for (unsigned int j=0;j<nl;++j)
                    if (local[j].itemType==l->first.second &&
                        strcmp(local[j].stringID,l->first.first.c_str())==0) have+=local[j].quantity;
                bool unresolved=false;
                for (std::map<u32,XferOut>::const_iterator x=xferOut_.begin();x!=xferOut_.end();++x)
                    if (x->second.key==l->first &&
                        ((!(x->second.src<k) && !(k<x->second.src)) ||
                         (!(x->second.dst<k) && !(k<x->second.dst)))) { unresolved=true; break; }
                // A timer is not a native verdict. Releasing this source latch
                // after a lost ACK could restore already-transferred stock.
                if (!unresolved && want==have) lt->second.erase(l++);
                else ++l;
            }
            if (!lt->second.empty()) { it->second.dirty=true; continue; }
            xferLatch_.erase(lt);
        }
        engine::applyContainerContents(gw, cHand, items, n, it->second.truncated);
        if (needsCheckpoint) {
            static InvItemEntry actual[INV_SNAPSHOT_ITEMS_MAX]; u32 actualHash=0,wantedHash=0;
            bool actualTruncated=false;
            unsigned int actualCount=engine::captureContainerContents(gw,cHand,actual,
                INV_SNAPSHOT_ITEMS_MAX,&actualHash,&actualTruncated,true);
            for (unsigned int r=0;r<n;++r) wantedHash+=invEntryHash(items[r]);
            // An owned actor's HOST checkpoint is not its source revision.
            // All received sources must certify native stock before admission.
            const bool ownCommit=hostCommit && ownsInventoryLocalHand(cHand);
            const bool matches=ownCommit ?
                !actualTruncated && ((actualCount==n && actualHash==wantedHash) ||
                    inventory_transaction::equivalentCheckpoint(items,n,actual,actualCount)) :
                engine::admitInventoryCheckpoint(cHand,it->second.revision,
                    items,n,actual,actualCount,it->second.truncated,actualTruncated);
            if (!matches) {
                it->second.dirty=true; continue;
            }
            engine::setInventoryCheckpointPending(cHand,false);
            if (it->second.checkpoint)
                inventoryCheckpointAcks_[invWireKeyForLocal(k)]=it->second.revision;
        }
        if (!it->second.truncated) {
            if (hostCommit && ownsInventoryLocalHand(cHand)) {
                u32 hash=0; for (unsigned int r=0;r<n;++r) hash+=invEntryHash(items[r]);
                engine::observeInventoryRevision(cHand,hash);
            } else engine::noteInventoryRevision(cHand,it->second.revision);
        }
        it->second.transaction=false;
        char b[160];
        _snprintf(b, sizeof(b) - 1,
            "[inv] APPLY hand=%u,%u,%u,%u,%u items=%u",
            k.t, k.c, k.cs, k.i, k.s, n);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        if (it->second.truncated) {
            char t[160]; _snprintf(t, sizeof(t) - 1,
                "[inv] APPLY-TRUNCATED hand=%u,%u,%u,%u,%u items=%u (additive-only; no deletes)",
                k.t, k.c, k.cs, k.i, k.s, n);
            t[sizeof(t) - 1] = '\0'; coop::logLine(t);
        }
        static int dumpInvA = -1;
        if (dumpInvA < 0) { const char* e = getenv("KENSHICOOP_INV_DUMP"); dumpInvA = (e && e[0] == '1') ? 1 : 0; }
        if (dumpInvA) { coop::logLine("[inv] APPLY-result:"); engine::dumpInventory(gw, cHand); }
    }
}

// Local content fingerprint for a tracked world item (change-detection ONLY, so it
// need only be stable on this client - cross-client matching uses netId + position
// tolerance). Mirrors the engine-side worldItemHash inputs (sid + type + qty + qual).
static u32 worldTrackHash(const char* sid, u32 type, u16 qty, u16 qual) {
    u32 h = 2166136261u;
    if (sid) for (const char* p = sid; *p; ++p) { h ^= (unsigned char)*p; h *= 16777619u; }
    h ^= type * 2654435761u;
    h ^= (u32)qty  * 40503u;
    h ^= (u32)qual * 2246822519u;
    return h ? h : 1u;
}

RootObject* Replicator::liveWorldProxy(GameWorld* gw,const WorldProxy& wp) const {
    RootObject* ro=engine::resolveObjectByHand(wp.hand);
    return engine::scenarioProxyIdentityMatches(wp.world,gw,wp.generation,
        engine::groundItemIdentityGeneration(),wp.hand,wp.obj,ro) ? ro : 0;
}

bool Replicator::queryScenarioItemWitness(GameWorld* gw,unsigned int channel,const char* sid,
                                         unsigned int type,const unsigned int sourceHand[5],
                                         engine::ScenarioItemWitness* result) const {
    if (!gw || !sid || !result) return false;
    memset(result,0,sizeof(*result));
    engine::ScenarioItemWitness candidate;
    engine::ScenarioItemWitness* out=&candidate;
    memset(out,0,sizeof(*out));
    unsigned int matches=0;
    if (channel==1 && sourceHand && (sourceHand[3] || sourceHand[4])) {
        std::map<Key,WorldTrack>::const_iterator it=worldTrack_.find(keyForInvLocalHand(sourceHand));
        if (it==worldTrack_.end() || it->second.baseline || !it->second.netId ||
            it->second.itemType!=type || strcmp(it->second.stringID,sid)!=0) return false;
        out->ownerId=localId_; out->eventId=it->second.netId;
        memcpy(out->itemHand,sourceHand,sizeof(out->itemHand));
        matches=1;
    } else if (channel==1) {
        for (std::map<std::pair<u32,u32>,WorldProxy>::const_iterator it=worldProxies_.begin();
             it!=worldProxies_.end();++it) {
            RootObject* live=liveWorldProxy(gw,it->second);
            InvItemEntry row; unsigned int actual[5];
            if (!live || !engine::readPhysicalItemMetadata(reinterpret_cast<Item*>(live),&row,actual) ||
                !engine::physicalItemIsOnGround(reinterpret_cast<Item*>(live))) return false;
            if (row.itemType!=type || strcmp(row.stringID,sid)!=0) continue;
            if (++matches>1) return false;
            out->ownerId=it->first.first; out->eventId=it->first.second;
            memcpy(out->itemHand,actual,sizeof(actual));
        }
    } else if (channel==2) {
        std::map<std::string,std::deque<GroundWeapon> >::const_iterator tracks=groundedWeapons_.find(sid);
        if (tracks==groundedWeapons_.end()) return false;
        unsigned int transitioned[5];
        const unsigned int* nativeSource=sourceHand;
        if (sourceHand && (sourceHand[3] || sourceHand[4]) &&
            engine::readScenarioItemAfter(gw,sourceHand,transitioned)) nativeSource=transitioned;
        for (std::deque<GroundWeapon>::const_iterator it=tracks->second.begin();it!=tracks->second.end();++it) {
            if (nativeSource && (nativeSource[3] || nativeSource[4]) &&
                memcmp(nativeSource,it->hand,sizeof(it->hand))!=0) continue;
            if (++matches>1) return false;
            out->ownerId=it->dropOwnerId; out->eventId=it->dropId;
            memcpy(out->itemHand,it->hand,sizeof(it->hand));
            memcpy(out->sourceHand,it->sourceHand,sizeof(it->sourceHand));
        }
    }
    unsigned int root[5],actual[5]; bool inBag=false; void* physical=0; InvItemEntry row;
    if (matches!=1 || !out->eventId ||
        !engine::readPhysicalItemLocation(out->itemHand,root,&inBag,&physical) || inBag ||
        !engine::readPhysicalItemMetadata(reinterpret_cast<Item*>(physical),&row,actual) ||
        memcmp(actual,out->itemHand,sizeof(actual))!=0 || row.quantity!=1 ||
        row.itemType!=type || strcmp(row.stringID,sid)!=0) return false;
    out->claimOwner=localId_;
    *result=*out;
    return true;
}

bool Replicator::publishPendingGearDrop(GameWorld*, NetLink& net, const Key& key,
                                        WorldDropPacket& p) {
    unsigned int hand[5]={key.t,key.c,key.cs,key.i,key.s};
    unsigned int root[5]; bool inBag=false; void* physical=0;
    engine::readPhysicalItemLocation(hand,root,&inBag,&physical);
    if (p.sourceEquipped==255) {
        // The positive historical drop remains valid after an immediate pickup.
        // Metadata recovery must not require that the item is STILL on the floor.
        InvItemEntry item; unsigned int observed[5];
        if (!physical || !engine::readPhysicalItemMetadata(
                reinterpret_cast<Item*>(physical),&item,observed) ||
            memcmp(hand,observed,sizeof(hand))!=0 ||
            item.itemType!=p.itemType || strcmp(item.stringID,p.stringID)!=0) return false;
        p.quality=item.quality; p.level=item.level;
        strncpy(p.manufacturer,item.manufacturer,sizeof(p.manufacturer)-1);
        strncpy(p.material,item.material,sizeof(p.material)-1);
        // A later read cannot recreate the historical equipped slot.
        p.sourceEquipped=0; p.sourceSlot=0; p.sourceSection=0;
    }
    net.queueWorldDrop(p);
    trackGroundGear(std::string(p.stringID),p.ownerId,p.dropId,physical);
    GroundWeapon& tracked=groundedWeapons_[std::string(p.stringID)].back();
    memcpy(tracked.hand,hand,sizeof(tracked.hand));
    unsigned int wireOwner[5]={p.oType,p.oContainer,p.oContainerSerial,p.oIndex,p.oSerial};
    memcpy(tracked.sourceHand,wireOwner,sizeof(wireOwner));
    char b[512]; _snprintf(b,sizeof(b)-1,
        "[wd] DROP id=%u sid='%s' owner=%u,%u,%u,%u,%u pos=%.2f,%.2f,%.2f tracked=%u "
        "level=%u equipped=%u slot=%u section=%u native=1 dropOwner=%u type=%u itemhand=%u,%u,%u,%u,%u",
        p.dropId,p.stringID,p.oType,p.oContainer,p.oContainerSerial,p.oIndex,p.oSerial,
        p.x,p.y,p.z,(unsigned)groundedWeapons_[std::string(p.stringID)].size(),
        (unsigned)p.level,(unsigned)p.sourceEquipped,(unsigned)p.sourceSlot,
        (unsigned)p.sourceSection,p.ownerId,p.itemType,hand[0],hand[1],hand[2],hand[3],hand[4]);
    b[sizeof(b)-1]='\0'; coop::logLine(b);
    return true;
}

bool Replicator::publishNativeGearPickup(GameWorld* gw, NetLink& net, u32 localId,
                                         const engine::ItemDropEdge& edge) {
    int ownerClass=ownerClassForHand(edge.ownerHand);
    if (ownerClass==2 || (ownerClass==0 &&
        (!edge.positionValid || !weAuthor(gw,localId,edge.x,edge.z)))) return false;
    Key physical=keyForInvLocalHand(edge.beforeItemHand);
    std::map<Key,WorldDropPacket>::iterator pending=pendingNpcDrops_.find(physical);
    if (pending!=pendingNpcDrops_.end()) {
        // The pickup captured the exact pre-pickup floor object, not a same-SID
        // substitute. Its metadata can complete that object's pending DROP.
        WorldDropPacket& p=pending->second;
        if (p.sourceEquipped==255 && edge.metadataValid &&
            p.itemType==edge.item.itemType && strcmp(p.stringID,edge.item.stringID)==0) {
            p.quality=edge.item.quality; p.level=edge.item.level;
            strncpy(p.manufacturer,edge.item.manufacturer,sizeof(p.manufacturer)-1);
            strncpy(p.material,edge.item.material,sizeof(p.material)-1);
            p.sourceEquipped=0; p.sourceSlot=0; p.sourceSection=0;
        }
        if (!publishPendingGearDrop(gw,net,physical,p)) return false;
        pendingNpcDrops_.erase(pending);
    }
    std::map<std::string,std::deque<GroundWeapon> >::iterator sit=
        groundedWeapons_.find(std::string(edge.item.stringID));
    if (sit==groundedWeapons_.end()) return false;
    for (std::deque<GroundWeapon>::iterator g=sit->second.begin();g!=sit->second.end();++g) {
        if (memcmp(g->hand,edge.beforeItemHand,sizeof(g->hand))!=0) continue;
        Key owner=invWireKeyForLocal(keyForInvLocalHand(edge.ownerHand));
        WorldPickupPacket p; memset(&p,0,sizeof(p));
        p.type=PKT_WORLD_PICKUP; p.ownerId=localId; p.pickupId=nextPickupId_++;
        p.oType=owner.t; p.oContainer=owner.c; p.oContainerSerial=owner.cs;
        p.oIndex=owner.i; p.oSerial=owner.s;
        strncpy(p.stringID,edge.item.stringID,sizeof(p.stringID)-1);
        p.itemType=edge.item.itemType; p.quality=edge.item.quality;
        p.refDropOwnerId=g->dropOwnerId; p.refDropId=g->dropId;
        net.queueWorldPickup(p);
        sit->second.erase(g); // the fallback pointer census cannot publish it twice
        char b[512]; _snprintf(b,sizeof(b)-1,
            "[wd] PICKUP id=%u sid='%s' owner=%u,%u,%u,%u,%u ref=%u/%u native=1 pickupOwner=%u type=%u "
            "beforeItemhand=%u,%u,%u,%u,%u afterItemhand=%u,%u,%u,%u,%u",
            p.pickupId,p.stringID,p.oType,p.oContainer,p.oContainerSerial,p.oIndex,p.oSerial,
            p.refDropOwnerId,p.refDropId,p.ownerId,p.itemType,
            edge.beforeItemHand[0],edge.beforeItemHand[1],edge.beforeItemHand[2],edge.beforeItemHand[3],edge.beforeItemHand[4],
            edge.itemHand[0],edge.itemHand[1],edge.itemHand[2],edge.itemHand[3],edge.itemHand[4]);
        b[sizeof(b)-1]='\0'; coop::logLine(b);
        return true;
    }
    return false;
}

void Replicator::publishWorldItems(GameWorld* gw, NetLink& net, u32 ownerId) {
    // Owner-authoritative world stream, BOTH directions since the W1 bidir fix (each
    // client streams the free ground items IT authors). DISCOVERY has two sources now:
    //   (1) the query-free drop hook (engine::drainItemDrops) - a drop captured at
    //       Inventory::dropItem, so a TOWN drop is found even when the spatial query
    //       misses it (the core town-reliability fix); and
    //   (2) the spatial scan (captureWorldItems) - save-native discovery only.
    //       Loading a block is not an item birth; only a captured drop may author it.
    // Both key a track by the item's LOCAL engine hand, so a drop found by BOTH sources
    // converges on ONE track (one netId) - no duplicate proxy. CULLING is now HANDLE-
    // based (engine::groundItemLiveness): a track is removed only when its real item is
    // gone or picked up, NOT when a single scan misses it - killing the town flicker.
    const float         RADIUS       = 60.0f; // interest scope for ground items (v1)
    const float         POS_EPS      = 0.5f;  // re-stream a moved item past this gap
    const unsigned long WI_RESEND_MS = 5000;  // periodic safety resend (loss / late join)
    static int dumpWi = -1;
    if (dumpWi < 0) { const char* e = getenv("KENSHICOOP_INV_DUMP"); dumpWi = (e && e[0] == '1') ? 1 : 0; }
    unsigned long now = nowMs();

    // ECHO GUARD: a proxy we spawned for a PEER's streamed item is a real local
    // RootObject and enumerates like any other ground item - re-publishing it would
    // bounce the item back to its author as a duplicate. Filter every discovery row
    // that resolves to an object in our proxy set.
    // Only LIVE proxies belong in the filter: a dead one's address can be reused by
    // a genuinely new ground item, and a stale entry would then silence that item
    // as an echo of itself and it would never stream.
    std::set<RootObject*> proxyObjs;
    for (std::map<std::pair<u32, u32>, WorldProxy>::iterator pi = worldProxies_.begin();
         pi != worldProxies_.end(); ++pi) {
        RootObject* live = liveWorldProxy(gw,pi->second);
        if (live) proxyObjs.insert(live);
    }

    // Save-native items can first become visible on ANY scan after a zone loads.
    // Seed them without streaming. Drop-hook edges below explicitly promote a
    // real session drop, including one already visible on this first scan.
    if (!worldSeeded_) {
        worldSeeded_ = true;
        engine::WorldItemRaw raw[WORLD_ITEMS_MAX];
        unsigned int n = engine::captureWorldItems(gw, raw, WORLD_ITEMS_MAX, RADIUS);
        unsigned int seeded = 0;
        for (unsigned int i = 0; i < n; ++i) {
            if (isGearType(raw[i].itemType)) continue;
            if (!proxyObjs.empty() &&
                proxyObjs.count(engine::resolveObjectByHand(raw[i].hand)) != 0)
                continue; // already our proxy (defensive; unlikely at first scan)
            Key k; k.t = raw[i].hand[0]; k.c = raw[i].hand[1]; k.cs = raw[i].hand[2];
            k.i = raw[i].hand[3]; k.s = raw[i].hand[4];
            if (worldTrack_.find(k) != worldTrack_.end()) continue;
            WorldTrack t; memset(&t, 0, sizeof(t));
            t.netId = nextWorldNetId_++; t.hash = 0; t.lastSendMs = 0;
            t.x = raw[i].x; t.y = raw[i].y; t.z = raw[i].z; t.seen = true;
            t.baseline = true; // never emit
            strncpy(t.stringID, raw[i].stringID, sizeof(t.stringID) - 1);
            t.stringID[sizeof(t.stringID) - 1] = '\0';
            t.itemType = raw[i].itemType; t.quantity = raw[i].quantity; t.quality = raw[i].quality;
            worldTrack_[k] = t;
            ++seeded;
        }
        char b[96]; _snprintf(b, sizeof(b) - 1,
            "[wi] BASELINE seeded=%u (save-native, never-stream)", seeded);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    // Gear (itemType WEAPON/ARMOUR) rides the W2 conservation drop/pickup channel
    // (the real shared-save object is relocated bag<->ground on each client), so the
    // W1 template-proxy stream skips it in BOTH discovery sources below.

    // ---- Discovery 1: query-free drop-hook edges (town reliable) -----------
    {
        engine::ItemDropEdge de[64];
        unsigned int nde = engine::drainItemDrops(de, 64);
        for (unsigned int i = 0; i < nde; ++i) {
            int nativeOwnerClass=ownerClassForHand(de[i].ownerHand);
            if (!de[i].positionValid && (de[i].kind==1 || nativeOwnerClass==0)) {
                engine::returnItemDrops(de+i,nde-i);
                break; // retain the exact history; never authorize at the zero origin
            }
            if (isGearType(de[i].kind==2 ? de[i].item.itemType : de[i].itemType)) {
                int ownerClass=ownerClassForHand(de[i].ownerHand);
                if (ownerClass==2 || (ownerClass==0 &&
                    !weAuthor(gw,ownerId,de[i].x,de[i].z))) continue;
                if (de[i].kind==2) {
                    bool blocked=pendingNpcDrops_.count(keyForInvLocalHand(de[i].beforeItemHand))!=0;
                    if (!publishNativeGearPickup(gw,net,ownerId,de[i])) {
                        if (blocked) {
                            engine::returnItemDrops(de+i,nde-i);
                            break; // do not discard/shortcut the remaining positive history
                        }
                        coop::logLine("[wd] PICKUP-UNTRACKED-NATIVE (no exact captured floor identity)");
                    }
                    continue;
                }
                if (de[i].kind!=1) continue;
                Key item=keyForInvLocalHand(de[i].itemHand);
                if (pendingNpcDrops_.count(item)) {
                    coop::logLine("[wd] DROP-BLOCKED-NATIVE (earlier exact physical drop still pending)");
                    engine::returnItemDrops(de+i,nde-i);
                    break;
                }
                Key owner=invWireKeyForLocal(keyForInvLocalHand(de[i].ownerHand));
                WorldDropPacket p; memset(&p,0,sizeof(p));
                p.type=PKT_WORLD_DROP; p.ownerId=ownerId; p.dropId=nextDropId_++;
                p.oType=owner.t; p.oContainer=owner.c; p.oContainerSerial=owner.cs;
                p.oIndex=owner.i; p.oSerial=owner.s;
                strncpy(p.stringID,de[i].stringID,sizeof(p.stringID)-1);
                p.itemType=de[i].itemType; p.quality=de[i].quality;
                p.x=de[i].x; p.y=de[i].y; p.z=de[i].z;
                p.sourceEquipped=255; // local pending marker; never transmitted
                if (de[i].metadataValid) {
                    const InvItemEntry& metadata=de[i].item;
                    p.quality=metadata.quality; p.level=metadata.level;
                    p.sourceEquipped=metadata.equipped; p.sourceSlot=metadata.slot;
                    p.sourceSection=metadata.section;
                    strncpy(p.manufacturer,metadata.manufacturer,sizeof(p.manufacturer)-1);
                    strncpy(p.material,metadata.material,sizeof(p.material)-1);
                }
                if (!de[i].itemHand[3] && !de[i].itemHand[4]) {
                    coop::logLine("[wd] DROP-INVALID-NATIVE-HAND (inventory held; positive source identity missing)");
                    p.sourceEquipped=255;
                }
                if (!publishPendingGearDrop(gw,net,item,p)) {
                    pendingNpcDrops_.insert(std::make_pair(item,p));
                    char b[200]; _snprintf(b,sizeof(b)-1,
                        "[wd] DROP-PENDING id=%u sid='%s' owner=%u,%u,%u,%u,%u (captured metadata unreadable; bag held)",
                        p.dropId,p.stringID,p.oType,p.oContainer,p.oContainerSerial,p.oIndex,p.oSerial);
                    b[sizeof(b)-1]='\0'; coop::logLine(b);
                }
                continue;
            }
            if (de[i].kind==2) {
                // Re-key only the exact mint named by the native before/after pickup edge.
                for (std::map<std::pair<u32,u32>,WorldProxy>::iterator pi=worldProxies_.begin();
                     pi!=worldProxies_.end();++pi) {
                    WorldProxy& proxy=pi->second;
                    if (proxy.world!=gw || proxy.generation!=engine::groundItemIdentityGeneration() ||
                        memcmp(proxy.hand,de[i].beforeItemHand,sizeof(proxy.hand))!=0 ||
                        proxy.obj!=de[i].physicalItem) continue;
                    unsigned int root[5],actual[5]; bool inBag=false; void* physical=0; InvItemEntry row;
                    if (engine::readPhysicalItemLocation(de[i].itemHand,root,&inBag,&physical) && inBag &&
                        physical==proxy.obj && memcmp(root,de[i].ownerHand,sizeof(root))==0 &&
                        engine::readPhysicalItemMetadata(reinterpret_cast<Item*>(physical),&row,actual) &&
                        memcmp(actual,de[i].itemHand,sizeof(actual))==0)
                        memcpy(proxy.hand,actual,sizeof(actual));
                }
                continue;
            }
            if (de[i].kind!=1) continue;
            if (de[i].itemHand[3] == 0 && de[i].itemHand[4] == 0) {
                // Unresolved hand: this drop cannot be keyed, so the reliable discovery path
                // gives up on it and only the (town-unreliable) spatial scan can still find
                // it. That is the one way a non-gear drop silently never reaches the peer, so
                // say so unconditionally - it is rare, and guessing at it from a silent log is
                // what made "dropped here, absent there" impossible to pin down.
                char b[200]; _snprintf(b, sizeof(b) - 1,
                    "[wi] DROP-CAP-SKIP sid='%s' qty=%u pos=%.2f,%.2f,%.2f (unresolved item "
                    "hand; spatial scan is the only remaining chance)",
                    de[i].stringID, de[i].quantity, de[i].x, de[i].y, de[i].z);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
                continue;
            }
            // Only the owner authors a squad drop; NPCs use cell authority.
            int ownerClass = ownerClassForHand(de[i].ownerHand);
            if (ownerClass == 2 ||
                (ownerClass == 0 && !weAuthor(gw,ownerId,de[i].x,de[i].z))) continue;
            Key k; k.t = de[i].itemHand[0]; k.c = de[i].itemHand[1]; k.cs = de[i].itemHand[2];
            k.i = de[i].itemHand[3]; k.s = de[i].itemHand[4];
            std::map<Key, WorldTrack>::iterator prior = worldTrack_.find(k);
            if (prior != worldTrack_.end() && !prior->second.baseline) continue;
            WorldTrack t; memset(&t, 0, sizeof(t));
            t.netId = nextWorldNetId_++; t.hash = 0; t.lastSendMs = 0;
            t.x = de[i].x; t.y = de[i].y; t.z = de[i].z; t.seen = true;
            strncpy(t.stringID, de[i].stringID, sizeof(t.stringID) - 1);
            t.stringID[sizeof(t.stringID) - 1] = '\0';
            t.itemType = de[i].itemType; t.quantity = de[i].quantity; t.quality = de[i].quality;
            worldTrack_[k] = t;
            if (dumpWi) { char b[384]; _snprintf(b, sizeof(b) - 1,
                "[wi] DROP-CAP netId=%u sid='%s' qty=%u pos=%.2f,%.2f,%.2f (query-free) author=%u type=%u itemhand=%u,%u,%u,%u,%u",
                t.netId,t.stringID,t.quantity,t.x,t.y,t.z,ownerId,t.itemType,k.t,k.c,k.cs,k.i,k.s);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b); }
        }
    }
    // Retry unreadable descriptors fairly, retaining the positive source intent.
    // Ready native drops were already published in queue order above.
    unsigned int npcAttempts=(unsigned int)pendingNpcDrops_.size();
    if (npcAttempts>64) npcAttempts=64;
    for (unsigned int n=0;n<npcAttempts && !pendingNpcDrops_.empty();++n) {
        std::map<Key,WorldDropPacket>::iterator it=pendingNpcDrops_.upper_bound(pendingNpcDropCursor_);
        if (it==pendingNpcDrops_.end()) it=pendingNpcDrops_.begin();
        pendingNpcDropCursor_=it->first;
        if (publishPendingGearDrop(gw,net,it->first,it->second)) pendingNpcDrops_.erase(it);
    }

    // ---- Discovery 2: the spatial scan (best-effort) -----------------------
    {
        engine::WorldItemRaw raw[WORLD_ITEMS_MAX];
        unsigned int n = engine::captureWorldItems(gw, raw, WORLD_ITEMS_MAX, RADIUS);
        for (unsigned int i = 0; i < n; ++i) {
            if (isGearType(raw[i].itemType)) continue;
            if (!proxyObjs.empty() &&
                proxyObjs.count(engine::resolveObjectByHand(raw[i].hand)) != 0)
                continue; // peer-authored proxy - not ours to publish
            Key k; k.t = raw[i].hand[0]; k.c = raw[i].hand[1]; k.cs = raw[i].hand[2];
            k.i = raw[i].hand[3]; k.s = raw[i].hand[4];
            std::map<Key, WorldTrack>::iterator tit = worldTrack_.find(k);
            if (tit == worldTrack_.end()) {
                WorldTrack t; memset(&t, 0, sizeof(t));
                t.netId = nextWorldNetId_++; t.hash = 0; t.lastSendMs = 0;
                t.x = raw[i].x; t.y = raw[i].y; t.z = raw[i].z; t.seen = true;
                t.baseline = true; // a late streamed-in native, not a proven session drop
                strncpy(t.stringID, raw[i].stringID, sizeof(t.stringID) - 1);
                t.stringID[sizeof(t.stringID) - 1] = '\0';
                t.itemType = raw[i].itemType; t.quantity = raw[i].quantity; t.quality = raw[i].quality;
                worldTrack_[k] = t;
            } else {
                // Refresh the description (a re-stack can change qty/quality).
                WorldTrack& tr = tit->second;
                strncpy(tr.stringID, raw[i].stringID, sizeof(tr.stringID) - 1);
                tr.stringID[sizeof(tr.stringID) - 1] = '\0';
                tr.itemType = raw[i].itemType; tr.quantity = raw[i].quantity; tr.quality = raw[i].quality;
            }
        }
    }

    // ---- Stream new/changed + HANDLE-based cull over every track -----------
    WorldItemEntry send[WORLD_ITEMS_MAX]; unsigned int ns = 0;
    u32 removed[256]; unsigned int nr = 0;
    unsigned int deferred = 0;
    // TEST-ONLY: shrink the per-tick batch (KENSHICOOP_WI_BATCH_MAX) so a scenario can overflow
    // it with a handful of drops. Filling the real 16-entry batch needs a crowd of simultaneous
    // ground items, which is precisely the situation a deterministic test cannot arrange.
    static int batchCap = -1;
    if (batchCap < 0) {
        const char* e = getenv("KENSHICOOP_WI_BATCH_MAX");
        int v = e ? atoi(e) : 0;
        batchCap = (v > 0 && v < (int)WORLD_ITEMS_MAX) ? v : (int)WORLD_ITEMS_MAX;
    }
    const unsigned int cap = (unsigned int)batchCap;
    for (std::map<Key, WorldTrack>::iterator it = worldTrack_.begin(); it != worldTrack_.end(); ) {
        WorldTrack& tr = it->second;
        unsigned int ihand[5] = { it->first.t, it->first.c, it->first.cs, it->first.i, it->first.s };
        float pos[3] = { tr.x, tr.y, tr.z };
        // Query-free liveness: is the real item still on the ground (not gone/picked-up)?
        if (!engine::groundItemLiveness(ihand, pos)) {
            // Baseline (save-native) tracks were never streamed, so the peer has
            // no proxy to remove - just drop our track. Streamed tracks emit a
            // remove so the peer despawns its proxy.
            if (!tr.baseline) { if (nr < 256) removed[nr++] = tr.netId; }
            if (dumpWi) { char b[112]; _snprintf(b, sizeof(b) - 1,
                "[wi] CULL netId=%u (gone/picked-up) baseline=%d", tr.netId, tr.baseline ? 1 : 0);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b); }
            worldTrack_.erase(it++);
            continue;
        }
        // Baseline save-natives never stream (both clients hold them identically).
        if (tr.baseline) { tr.x = pos[0]; tr.y = pos[1]; tr.z = pos[2]; ++it; continue; }
        bool sent = (tr.lastSendMs != 0);
        float dx = pos[0] - tr.x, dy = pos[1] - tr.y, dz = pos[2] - tr.z;
        bool moved = (dx*dx + dy*dy + dz*dz) > (POS_EPS * POS_EPS);
        u32 h = worldTrackHash(tr.stringID, tr.itemType, tr.quantity, tr.quality);
        bool changed = !sent || (tr.hash != h) || moved;
        bool periodic = sent && !changed && (now - tr.lastSendMs >= WI_RESEND_MS);
        // One datagram holds WORLD_ITEMS_MAX entries. Anything that does not fit must keep its
        // "unsent" bookkeeping so the NEXT tick sends it: marking the track sent regardless -
        // stamping tr.hash/lastSendMs and the position outside this test - made the batch's
        // overflow permanently invisible until the 5 s safety resend happened to pick it up,
        // which is a drop the other side simply does not see. Drop a squad's bags out at once
        // and the tail of the burst is exactly what goes missing.
        if (changed || periodic) {
            if (ns < cap) {
                WorldItemEntry& e = send[ns++];
                e.netId = tr.netId;
                strncpy(e.stringID, tr.stringID, sizeof(e.stringID) - 1);
                e.stringID[sizeof(e.stringID) - 1] = '\0';
                e.itemType = tr.itemType;
                e.quantity = tr.quantity;
                e.quality  = tr.quality;
                e.x = pos[0]; e.y = pos[1]; e.z = pos[2];
                e.state = 0;
                tr.hash = h; tr.lastSendMs = now;
                tr.x = pos[0]; tr.y = pos[1]; tr.z = pos[2];
                if (changed && dumpWi) {
                    char b[384]; _snprintf(b, sizeof(b) - 1,
                        "[wi] SEND netId=%u sid='%s' qty=%u pos=%.2f,%.2f,%.2f hash=%u owner=%u type=%u hand=%u,%u,%u,%u,%u",
                        tr.netId,tr.stringID,tr.quantity,pos[0],pos[1],pos[2],h,ownerId,tr.itemType,
                        ihand[0],ihand[1],ihand[2],ihand[3],ihand[4]);
                    b[sizeof(b) - 1] = '\0'; coop::logLine(b);
                }
            } else {
                ++deferred; // retried next tick, still flagged changed
            }
        } else {
            tr.x = pos[0]; tr.y = pos[1]; tr.z = pos[2];
        }
        ++it;
    }
    if (deferred > 0) {
        // Unconditional, and at most one line per tick: a deferred item is one the peer cannot
        // see yet, which is the hardest W1 symptom to tell apart from a drop that was never
        // detected at all.
        char b[140]; _snprintf(b, sizeof(b) - 1,
            "[wi] SEND-DEFER n=%u (batch full at %u; retried next tick)", deferred, cap);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    if (ns > 0) net.queueWorldItems(ownerId, send, ns);
    if (nr > 0) net.queueWorldRemove(ownerId, removed, nr);

    // ---- CLAIM half (protocol 47): a proxy WE hold just went into a local bag ----
    // W1 mirrors a drop but never mirrored the PICKUP: the author's cull tests the liveness
    // of its OWN real item, which is still on its ground, so it never fired. Report each
    // consumed proxy to its author so it destroys the real copy. Only a proxy that still
    // reads AND is now inside an inventory counts - a merely destroyed proxy is dropped
    // silently, because claiming on it would delete the author's item for no reason.
    // Claims are grouped per author (netIds live in the AUTHOR's id space).
    {
        std::map<u32, std::vector<u32> > claims;
        for (std::map<std::pair<u32, u32>, WorldProxy>::iterator pi = worldProxies_.begin();
             pi != worldProxies_.end(); ) {
            if (!pi->second.hand[3] && !pi->second.hand[4]) { ++pi; continue; } // unresolved mint, not proven dead
            bool pickedUp = false;
            // A proxy the engine has already destroyed reads as "not picked up", so
            // it falls through to the erase below and claims nothing. That is the
            // right way round: a zone unload must not be mistaken for a pickup, or
            // we would tell the author to destroy an item nobody took.
            RootObject* live = liveWorldProxy(gw,pi->second);
            if (live && engine::groundObjectLiveness(live, 0, &pickedUp)) { ++pi; continue; }
            if (pickedUp) {
                InvItemEntry row; unsigned int actual[5];
                bool metadata=engine::readPhysicalItemMetadata(reinterpret_cast<Item*>(live),&row,actual);
                unsigned int recipient[5]; bool inBag=false; void* physical=0;
                if (!metadata || !engine::readPhysicalItemLocation(actual,recipient,&inBag,&physical) ||
                    !inBag || physical!=live) { ++pi; continue; }
                int recipientClass=ownerClassForHand(recipient);
                if (recipientClass==2 || (recipientClass==0 &&
                    !weAuthor(gw,ownerId,pi->second.x,pi->second.z))) { ++pi; continue; }
                claims[pi->first.first].push_back(pi->first.second);
                char b[512]; _snprintf(b,sizeof(b)-1,
                    "[wi] CLAIM author=%u netId=%u (proxy consumed locally) claimOwner=%u type=%u itemhand=%u,%u,%u,%u,%u "
                    "complete=%d afterItemhand=%u,%u,%u,%u,%u recipient=%u,%u,%u,%u,%u",
                    pi->first.first,pi->first.second,ownerId,metadata ? row.itemType : 0,
                    pi->second.sourceHand[0],pi->second.sourceHand[1],pi->second.sourceHand[2],
                    pi->second.sourceHand[3],pi->second.sourceHand[4],metadata ? 1 : 0,
                    pi->second.hand[0],pi->second.hand[1],pi->second.hand[2],pi->second.hand[3],pi->second.hand[4],
                    recipient[0],recipient[1],recipient[2],recipient[3],recipient[4]);
                b[sizeof(b)-1]='\0'; coop::logLine(b);
            } else if (dumpWi) { char b[160]; _snprintf(b, sizeof(b) - 1,
                "[wi] PROXY-GONE author=%u netId=%u (destroyed, not claimed)",
                pi->first.first, pi->first.second);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b); }
            // Either way the proxy is no longer ours to track. A later cull for this
            // netId simply finds nothing and is ignored.
            worldProxies_.erase(pi++);
        }
        for (std::map<u32, std::vector<u32> >::iterator ci = claims.begin();
             ci != claims.end(); ++ci)
            net.queueWorldClaim(ownerId, ci->first, &ci->second[0],
                                (unsigned int)ci->second.size());
    }
}

void Replicator::applyWorldItems(GameWorld* gw, Inbound& in) {
    std::deque<InboundWorldItems>  items;
    std::deque<InboundWorldRemove> rems;
    in.drainWorldItems(items);
    in.drainWorldRemove(rems);
    if (items.empty() && rems.empty()) return;
    static int dumpWi = -1;
    if (dumpWi < 0) { const char* e = getenv("KENSHICOOP_INV_DUMP"); dumpWi = (e && e[0] == '1') ? 1 : 0; }
    const float POS_EPS = 0.5f;

    // Snapshots: spawn a proxy for a new (owner, netId), move it if it changed.
    // netId spaces are per-sender (W1 bidir), so the owner scopes every key.
    for (std::deque<InboundWorldItems>::iterator b = items.begin(); b != items.end(); ++b) {
        for (std::vector<WorldItemEntry>::iterator e = b->items.begin(); e != b->items.end(); ++e) {
            std::pair<u32, u32> pk(b->ownerId, e->netId);
            std::map<std::pair<u32, u32>, WorldProxy>::iterator pit = worldProxies_.find(pk);
            if (pit == worldProxies_.end()) {
                WorldProxy wp;
                RootObject* obj = engine::spawnWorldItemProxy(gw, e->stringID, e->itemType,
                                                              (int)e->quantity, e->x, e->y, e->z,
                                                              wp.hand);
                if (obj) {
                    wp.obj=obj; wp.world=gw; wp.x=e->x; wp.y=e->y; wp.z=e->z; wp.hash=0;
                    wp.generation=engine::groundItemIdentityGeneration();
                    memcpy(wp.sourceHand,wp.hand,sizeof(wp.hand));
                    worldProxies_[pk] = wp;
                }
                // Unregistered mints are retained as unresolved, never used as owned pointers.
                char b2[384]; _snprintf(b2,sizeof(b2)-1,
                    "[wi] SPAWN owner=%u netId=%u ok=%d sid='%s' pos=%.2f,%.2f,%.2f hand=%u,%u,%u,%u,%u type=%u qty=%u",
                    b->ownerId,e->netId,obj ? 1 : 0,e->stringID,e->x,e->y,e->z,
                    wp.hand[0],wp.hand[1],wp.hand[2],wp.hand[3],wp.hand[4],e->itemType,e->quantity);
                b2[sizeof(b2) - 1] = '\0';
                if (dumpWi || !obj || (obj && !wp.hand[3] && !wp.hand[4])) coop::logLine(b2);
            } else {
                WorldProxy& wp = pit->second;
                float dx = e->x - wp.x, dy = e->y - wp.y, dz = e->z - wp.z;
                if ((dx*dx + dy*dy + dz*dz) > (POS_EPS * POS_EPS)) {
                    // setPositionRotation is VIRTUAL: on a freed object this reads a
                    // dangling vtable, which is the worst-behaved use of a stale
                    // proxy we have - and the most frequent, since every snapshot
                    // that nudges an item comes through here.
                    RootObject* live = liveWorldProxy(gw,wp);
                    if (live) engine::updateWorldItemProxy(live, e->x, e->y, e->z);
                    wp.x = e->x; wp.y = e->y; wp.z = e->z;
                    if (dumpWi) { char b2[176]; _snprintf(b2, sizeof(b2) - 1,
                        "[wi] MOVE netId=%u pos=%.2f,%.2f,%.2f live=%d",
                        e->netId, e->x, e->y, e->z, live ? 1 : 0);
                        b2[sizeof(b2) - 1] = '\0'; coop::logLine(b2); }
                }
            }
        }
    }
    // TEST-ONLY fault injection (KENSHICOOP_WI_TEST_STALE): free the proxy through the
    // engine immediately before culling it, so the cull runs against an object the
    // engine has ALREADY destroyed. That is the state a zone teardown leaves behind when
    // players travel out of a block, but the window is one frame wide - the publish
    // sweep re-resolves every proxy each tick and drops the dead ones - so a scenario
    // cannot schedule itself into it. Injecting here makes it certain: without the hand
    // check the cull below is a second GameWorld::destroy on freed memory, which Kenshi
    // reports as "alredy has destroy reason coop-worlditem-cull" (world_item_stale).
    static int testStale = -1;
    if (testStale < 0) {
        const char* e = getenv("KENSHICOOP_WI_TEST_STALE");
        testStale = (e && e[0] == '1') ? 1 : 0;
    }

    // Culls: destroy the proxy and drop the mapping (scoped to the authoring owner).
    for (std::deque<InboundWorldRemove>::iterator b = rems.begin(); b != rems.end(); ++b) {
        for (std::vector<u32>::iterator id = b->netIds.begin(); id != b->netIds.end(); ++id) {
            std::map<std::pair<u32, u32>, WorldProxy>::iterator pit =
                worldProxies_.find(std::make_pair(b->ownerId, *id));
            if (pit == worldProxies_.end()) continue;
            RootObject* staleTestLive=liveWorldProxy(gw,pit->second);
            if (testStale && staleTestLive) {
                engine::removeWorldItemProxy(gw,staleTestLive);
                char tb[144]; _snprintf(tb, sizeof(tb) - 1,
                    "[wi] TEST-STALE owner=%u netId=%u (proxy freed under the cull)",
                    b->ownerId, *id);
                tb[sizeof(tb) - 1] = '\0'; coop::logLine(tb);
            }
            // Already gone (its block unloaded before the cull arrived): drop the
            // mapping and leave the engine alone. Destroying it a second time is a
            // double-destroy on freed memory, which the engine notices - it logs
            // "alredy has destroy reason" - and does not survive reliably.
            RootObject* live = liveWorldProxy(gw,pit->second);
            if (live) engine::removeWorldItemProxy(gw, live);
            worldProxies_.erase(pit);
            if (dumpWi) { char b2[144]; _snprintf(b2, sizeof(b2) - 1,
                "[wi] CULL owner=%u netId=%u live=%d", b->ownerId, *id, live ? 1 : 0);
                b2[sizeof(b2) - 1] = '\0'; coop::logLine(b2); }
        }
    }
}

void Replicator::applyWorldClaims(GameWorld* gw, Inbound& in, u32 localId) {
    std::deque<InboundWorldClaim> got;
    in.drainWorldClaim(got);
    if (got.empty()) return;
    static int dumpWi = -1;
    if (dumpWi < 0) { const char* e = getenv("KENSHICOOP_INV_DUMP"); dumpWi = (e && e[0] == '1') ? 1 : 0; }
    for (std::deque<InboundWorldClaim>::iterator b = got.begin(); b != got.end(); ++b) {
        if (b->ownerId == localId) continue;      // our own claim echoed back (relay safety)
        if (b->authorId != localId) continue;     // addressed to a different author
        for (std::vector<u32>::iterator id = b->netIds.begin(); id != b->netIds.end(); ++id) {
            std::map<Key, WorldTrack>::iterator tit = worldTrack_.begin();
            for (; tit != worldTrack_.end(); ++tit)
                if (tit->second.netId == *id) break;
            if (tit == worldTrack_.end()) {
                if (dumpWi) { char b2[160]; _snprintf(b2, sizeof(b2) - 1,
                    "[wi] CLAIM-APPLY netId=%u from=%u (no track; already gone)",
                    *id, b->ownerId); b2[sizeof(b2) - 1] = '\0'; coop::logLine(b2); }
                continue;
            }
            unsigned int ihand[5] = { tit->first.t, tit->first.c, tit->first.cs,
                                      tit->first.i, tit->first.s };
            RootObject* ro = engine::resolveObjectByHand(ihand);
            bool destroyed = (ro != 0) && engine::removeWorldItemProxy(gw, ro);
            char b2[384]; _snprintf(b2,sizeof(b2)-1,
                "[wi] CLAIM-APPLY netId=%u from=%u sid='%s' destroyed=%d author=%u type=%u itemhand=%u,%u,%u,%u,%u",
                *id,b->ownerId,tit->second.stringID,destroyed ? 1 : 0,b->authorId,tit->second.itemType,
                ihand[0],ihand[1],ihand[2],ihand[3],ihand[4]);
            b2[sizeof(b2) - 1] = '\0'; coop::logLine(b2);
            // On success LEAVE the track: the next publish pass finds its liveness gone
            // and emits the ordinary cull, so a THIRD peer drops its proxy too, then
            // erases the track itself. Only when the object could NOT be destroyed do we
            // erase here - otherwise we would keep streaming an item the claimer already
            // took, and it would re-spawn as a fresh proxy (the duplicate we are fixing).
            if (!destroyed) worldTrack_.erase(tit);
        }
    }
}

void Replicator::detectAndPublishWeaponDrops(GameWorld* gw, NetLink& net, u32 ownerId) {
    // Native DROP/PICKUP history must run first. A final-state census cannot
    // reconstruct multiple mutations of one physical object in the same frame.
    if (ownHands_.empty() || engine::hasPendingPhysicalGroundEdges()) return;
    static int dumpWd = -1;
    if (dumpWd < 0) { const char* e = getenv("KENSHICOOP_INV_DUMP"); dumpWd = (e && e[0] == '1') ? 1 : 0; }
    InvItemEntry items[INV_ITEMS_MAX];
    for (std::set<Key>::iterator it = ownHands_.begin(); it != ownHands_.end(); ++it) {
        unsigned int cHand[5] = { it->t, it->c, it->cs, it->i, it->s };
        if (engine::resolveObjectByHand(cHand) == 0) continue;
        // A detached cursor item is not a ground drop. Preserve the census until
        // native add/drop/destroy settles the removal; a completed trade is then
        // absorbed by its transfer suppression before this census runs.
        if (engine::itemTransferPending(cHand)) continue;
        unsigned int n = engine::captureContainerContents(gw, cHand, items, INV_ITEMS_MAX, 0);
        // Build this character's CURRENT GEAR census (copies per "sid", with provenance + type).
        std::map<std::string, WCensusItem> cur;
        for (unsigned int i = 0; i < n; ++i) {
            if (!isGearType(items[i].itemType)) continue;
            WCensusItem& wc = cur[std::string(items[i].stringID)];
            int q = items[i].quantity; if (q < 1) q = 1;
            if (wc.count == 0) {
                strncpy(wc.manufacturer, items[i].manufacturer, sizeof(wc.manufacturer) - 1);
                strncpy(wc.material,     items[i].material,     sizeof(wc.material) - 1);
                wc.quality  = items[i].quality;
                wc.itemType = items[i].itemType;
            }
            wc.count += q;
        }
        // Capture the REAL Item* of each weapon this tick (parallel to the census), so a
        // DROP can remember the exact now-grounded object for a later pickup (the spatial
        // query can't re-find it in towns).
        char wsids[INV_ITEMS_MAX][48];
        void* wptrs[INV_ITEMS_MAX];
        unsigned int nwp = engine::captureWeaponPtrs(gw, cHand, wsids, wptrs, INV_ITEMS_MAX);
        std::map<std::string, std::deque<void*> > curPtrs;
        for (unsigned int i = 0; i < nwp; ++i) curPtrs[std::string(wsids[i])].push_back(wptrs[i]);

        WCensus& prevC = weaponCensus_[*it];
        // An initially empty native read can precede inventory population. Defer
        // the pickup census seed until it repeats; no count loss publishes a DROP.
        // Later increases are actionable only with the exact tracked Item* below.
        if (n == 0) {
            ++prevC.zeroReads;
            // A character genuinely carrying NOTHING also reads zero rows forever, and must
            // eventually get a baseline or their first real pickup is never detected. Accept
            // the empty baseline only once the zero read has REPEATED - a resolving container
            // fills in within a tick or two, a truly empty one never does.
            if (!prevC.seeded && prevC.zeroReads >= WD_EMPTY_SEED_TICKS) {
                prevC.items.clear(); prevC.ptrs.clear(); prevC.seeded = true;
                if (dumpWd) { char b[160]; _snprintf(b, sizeof(b) - 1,
                    "[wd] census-seed-empty hand=%u,%u,%u,%u,%u (%u consecutive zero reads)",
                    it->t, it->c, it->cs, it->i, it->s, prevC.zeroReads);
                    b[sizeof(b) - 1] = '\0'; coop::logLine(b); }
                continue;
            }
            if (dumpWd) { char b[180]; _snprintf(b, sizeof(b) - 1,
                "[wd] census-skip hand=%u,%u,%u,%u,%u (zero-row read #%u; seeded=%d had %u kinds)",
                it->t, it->c, it->cs, it->i, it->s, prevC.zeroReads,
                prevC.seeded ? 1 : 0, (unsigned)prevC.items.size());
                b[sizeof(b) - 1] = '\0'; coop::logLine(b); }
            continue;
        }
        prevC.zeroReads = 0; // the read is trustworthy again
        if (!prevC.seeded) {
            prevC.items = cur; prevC.ptrs = curPtrs; prevC.seeded = true; // baseline; never emit
            if (dumpWd) { char b[140]; _snprintf(b, sizeof(b) - 1,
                "[wd] census-seed hand=%u,%u,%u,%u,%u weaponKinds=%u",
                it->t, it->c, it->cs, it->i, it->s, (unsigned)cur.size());
                b[sizeof(b) - 1] = '\0'; coop::logLine(b); }
            continue;
        }
        // INCREASE pass (PICKUP): a weapon kind whose count rose was picked up by this owned
        // character. Author a reliable PICKUP intent so the peer re-homes its tracked ground
        // copy into this character's bag; consume one of OUR tracked ground copies (the local
        // UI pickup already moved the real object from ground to bag). Done before the drop
        // pass mutates `cur` for debounce. (No tracked copy on the peer => no-op there, so a
        // brand-new/looted weapon never spuriously appears.)
        for (std::map<std::string, WCensusItem>::iterator ce = cur.begin(); ce != cur.end(); ++ce) {
            std::map<std::string, WCensusItem>::iterator pp = prevC.items.find(ce->first);
            int prevCount = (pp != prevC.items.end()) ? pp->second.count : 0;
            int inc = ce->second.count - prevCount;
            if (inc <= 0) continue;
            // Protocol 37: a pending/applied cross-owner gear transfer must not be
            // read as a ground PICKUP of the same sid (the count edge is the trade).
            // The end-of-loop baseline update absorbs the new count silently.
            if (wdSuppressed(*it, ce->first.c_str(), nowMs())) {
                if (dumpWd) { char b[160]; _snprintf(b, sizeof(b) - 1,
                    "[wd] increase-suppressed (xfer) sid='%s' inc=%d", ce->first.c_str(), inc);
                    b[sizeof(b) - 1] = '\0'; coop::logLine(b); }
                continue;
            }
            // Which REAL Item*(s) just ENTERED this bag? The picked-up ground object is the
            // same handle (conservation), so the pointers present now but not last tick are
            // exactly the ones picked up. Correlating each to the ground copy WE tracked
            // recovers its shared (dropOwnerId, dropId) - the identity both clients agree on -
            // so the peer re-homes the EXACT instance rather than guessing FIFO-by-sid.
            std::set<void*> prevSet;
            {
                std::map<std::string, std::deque<void*> >::iterator pit = prevC.ptrs.find(ce->first);
                if (pit != prevC.ptrs.end())
                    for (std::deque<void*>::iterator q = pit->second.begin(); q != pit->second.end(); ++q)
                        prevSet.insert(*q);
            }
            std::deque<void*> added;
            {
                std::map<std::string, std::deque<void*> >::iterator cit = curPtrs.find(ce->first);
                if (cit != curPtrs.end())
                    for (std::deque<void*>::iterator q = cit->second.begin(); q != cit->second.end(); ++q)
                        if (prevSet.count(*q) == 0) added.push_back(*q);
            }
            std::deque<GroundWeapon>& q = groundedWeapons_[ce->first];
            // NO EVIDENCE, NO INTENT. A count increase alone does not prove an object moved
            // from the ground into this bag: captureContainerContents can miss a worn item
            // for a tick and then report it again, and that flicker reads as an increase.
            // Emitting anyway produced identity-less (ref=0/0) intents the receiver can only
            // act on by guessing which same-sid ground item to swallow. The pointer match
            // below IS the evidence, so an unmatched increase is dropped here - the mirror of
            // the receiver refusing to guess. The bag itself still converges over the
            // inventory snapshot channel; only the ground-side relocation needs identity.
            if (added.empty()) {
                if (dumpWd) { char b[200]; _snprintf(b, sizeof(b) - 1,
                    "[wd] increase-unexplained sid='%s' inc=%d prev=%d now=%d (no new Item* "
                    "arrived; census flicker, not a pickup)",
                    ce->first.c_str(), inc, prevCount, ce->second.count);
                    b[sizeof(b) - 1] = '\0'; coop::logLine(b); }
                continue;
            }
            for (int k = 0; k < inc; ++k) {
                WorldPickupPacket pkt; memset(&pkt, 0, sizeof(pkt));
                pkt.type = (u8)PKT_WORLD_PICKUP; pkt.ownerId = ownerId;
                pkt.pickupId = nextPickupId_++;
                pkt.oType = it->t; pkt.oContainer = it->c; pkt.oContainerSerial = it->cs;
                pkt.oIndex = it->i; pkt.oSerial = it->s;
                strncpy(pkt.stringID, ce->first.c_str(), sizeof(pkt.stringID) - 1);
                pkt.itemType = ce->second.itemType; pkt.quality = ce->second.quality;
                // Match a newly-arrived pointer to a tracked ground instance for its identity.
                unsigned int beforeHand[5]={0,0,0,0,0},afterHand[5]={0,0,0,0,0};
                if (!added.empty()) {
                    void* got = added.front(); added.pop_front();
                    for (std::deque<GroundWeapon>::iterator g = q.begin(); g != q.end(); ++g) {
                        if (g->item == got) {
                            InvItemEntry actual; unsigned int actualHand[5],root[5]; bool inBag=false; void* physical=0;
                            if (!engine::readPhysicalItemMetadata(reinterpret_cast<Item*>(got),&actual,actualHand) ||
                                !engine::readPhysicalItemLocation(actualHand,root,&inBag,&physical) || !inBag ||
                                physical!=got || memcmp(root,cHand,sizeof(root))!=0 ||
                                actual.itemType!=pkt.itemType || strcmp(actual.stringID,pkt.stringID)!=0) continue;
                            unsigned int transition[5];
                            bool sameHand=memcmp(actualHand,g->hand,sizeof(actualHand))==0;
                            bool rekey=engine::readScenarioItemAfter(gw,g->hand,transition) &&
                                memcmp(actualHand,transition,sizeof(actualHand))==0;
                            if (!sameHand && !rekey) continue; // pointer equality alone cannot bind an occurrence
                            pkt.refDropOwnerId=g->dropOwnerId; pkt.refDropId=g->dropId;
                            memcpy(beforeHand,g->hand,sizeof(beforeHand)); memcpy(afterHand,actualHand,sizeof(afterHand));
                            q.erase(g); break;
                        }
                    }
                }
                // Still no identity: this bag gained a real object we never tracked on the
                // ground (a save-native ground item, or loot). The peer has nothing to
                // re-home, so an intent would only invite it to guess. Stay silent.
                    if (pkt.refDropId == 0) {
                        // Unconditional: a real object entered this bag and we cannot name it,
                        // so the peer will never be told to give up its ground copy. Rare, and
                        // the direct precursor of a duplicate - it must not be dump-gated.
                        char b[200]; _snprintf(b, sizeof(b) - 1,
                            "[wd] increase-untracked sid='%s' inc=%d (real arrival, but no tracked "
                            "ground instance to name)", ce->first.c_str(), inc);
                        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
                        continue;
                    }
                net.queueWorldPickup(pkt);
                if (dumpWd) { char b[512]; _snprintf(b, sizeof(b) - 1,
                    "[wd] PICKUP id=%u sid='%s' owner=%u,%u,%u,%u,%u ref=%u/%u prev=%d now=%d trackedLeft=%u "
                    "pickupOwner=%u type=%u beforeItemhand=%u,%u,%u,%u,%u afterItemhand=%u,%u,%u,%u,%u",
                    pkt.pickupId,pkt.stringID,it->t,it->c,it->cs,it->i,it->s,
                    pkt.refDropOwnerId,pkt.refDropId,prevCount,ce->second.count,(unsigned)q.size(),ownerId,pkt.itemType,
                    beforeHand[0],beforeHand[1],beforeHand[2],beforeHand[3],beforeHand[4],
                    afterHand[0],afterHand[1],afterHand[2],afterHand[3],afterHand[4]);
                    b[sizeof(b) - 1] = '\0'; coop::logLine(b); }
            }
        }
        prevC.items = cur;
        prevC.ptrs  = curPtrs;
    }
}

void Replicator::applyGroundGear(GameWorld* gw, Inbound& in) {
    // Retry existing failures fairly before accepting new history.
    unsigned int attempts=(unsigned int)pendingDrops_.size();
    if (attempts>64) attempts=64;
    for (unsigned int n=0;n<attempts && !pendingDrops_.empty();++n) {
        std::map<std::pair<u32,u32>,WorldDropPacket>::iterator it=
            pendingDrops_.upper_bound(pendingDropCursor_);
        if (it==pendingDrops_.end()) it=pendingDrops_.begin();
        pendingDropCursor_=it->first;
        if (tryApplyWeaponDrop(gw,it->second)) {
            appliedDrops_.insert(it->first);
            pendingDrops_.erase(it);
        }
    }
    retryPendingPickups(gw);
    std::deque<InboundGroundGear> got;
    in.drainGroundGear(got);
    for (std::deque<InboundGroundGear>::const_iterator it=got.begin();it!=got.end();++it) {
        if (it->type==PKT_WORLD_PICKUP) {
            applyWeaponPickup(gw,it->pickup);
            continue;
        }
        const WorldDropPacket& p=it->drop;
        std::pair<u32,u32> id(p.ownerId,p.dropId);
        if (appliedDrops_.count(id) || pendingDrops_.count(id)) continue;
        char b[160]; _snprintf(b,sizeof(b)-1,
            "[wd] DROP-RECEIVED id=%u from=%u sid='%s' (pending physical relocation)",
            p.dropId,p.ownerId,p.stringID);
        b[sizeof(b)-1]='\0'; coop::logLine(b);
        if (tryApplyWeaponDrop(gw,p)) appliedDrops_.insert(id);
        else pendingDrops_.insert(std::make_pair(id,p));
    }
    while (appliedDrops_.size()>4096) appliedDrops_.erase(appliedDrops_.begin());
}

bool Replicator::tryApplyWeaponDrop(GameWorld* gw, const WorldDropPacket& p) {
    Key wire; wire.t=p.oType; wire.c=p.oContainer; wire.cs=p.oContainerSerial;
    wire.i=p.oIndex; wire.s=p.oSerial;
    unsigned int ownerHand[5];
    if (!resolveInvWireHand(wire,ownerHand)) return false;
    Key local=keyForInvLocalHand(ownerHand);
    if (ownHands_.count(local)) return true;
    // A deferred pickup must return its exact object before a later drop can
    // select anything from that inventory. Its referenced earlier DROP is
    // exempt, otherwise the pickup and its own prerequisite would deadlock.
    for (std::deque<PendingPickup>::const_iterator pp=pendingPickups_.begin();
         pp!=pendingPickups_.end();++pp) {
        if (pp->refOwnerId==p.ownerId && pp->refDropId==p.dropId) continue;
        if (pp->itemType==p.itemType && pp->sid==p.stringID &&
            pp->targetHand[0]==p.oType && pp->targetHand[1]==p.oContainer &&
            pp->targetHand[2]==p.oContainerSerial && pp->targetHand[3]==p.oIndex &&
            pp->targetHand[4]==p.oSerial) return false;
    }
    InvItemEntry identity; memset(&identity,0,sizeof(identity));
    strncpy(identity.stringID,p.stringID,sizeof(identity.stringID)-1);
    identity.itemType=p.itemType; identity.quantity=1;
    identity.quality=p.quality; identity.level=p.level;
    identity.equipped=p.sourceEquipped; identity.slot=p.sourceSlot;
    identity.section=p.sourceSection;
    strncpy(identity.manufacturer,p.manufacturer,sizeof(identity.manufacturer)-1);
    strncpy(identity.material,p.material,sizeof(identity.material)-1);
    std::deque<GroundWeapon>& tracks=groundedWeapons_[std::string(p.stringID)];
    GroundWeapon* partial=0;
    for (std::deque<GroundWeapon>::iterator g=tracks.begin();g!=tracks.end();++g)
        if (g->dropOwnerId==p.ownerId && g->dropId==p.dropId) { partial=&*g; break; }
    void* dropped=0; int moved=0;
    if (partial) {
        unsigned int root[5]; bool inBag=false;
        if (engine::readPhysicalItemLocation(partial->hand,root,&inBag,&dropped) && !inBag) {
            engine::updateWorldItemProxy(engine::resolveObjectByHand(partial->hand),p.x,p.y,p.z);
            float position[3];
            if (engine::itemWorldPos(dropped,position)) {
                float dx=position[0]-p.x,dy=position[1]-p.y,dz=position[2]-p.z;
                moved=dx*dx+dy*dy+dz*dz<=0.25f ? 1 : 0;
            }
        }
    } else {
        moved=engine::relocateWeaponToGround(gw,ownerHand,identity,p.x,p.y,p.z,&dropped);
        if (dropped) trackGroundGear(std::string(p.stringID),p.ownerId,p.dropId,dropped);
    }
    if (!moved || !dropped) return false;
    unsigned int itemHand[5]={0,0,0,0,0};
    engine::readObjectHand(reinterpret_cast<RootObject*>(dropped),itemHand);
    unsigned int wireOwner[5]={p.oType,p.oContainer,p.oContainerSerial,p.oIndex,p.oSerial};
    for (std::deque<GroundWeapon>::iterator it=tracks.begin();it!=tracks.end();++it)
        if (it->dropOwnerId==p.ownerId && it->dropId==p.dropId)
            memcpy(it->sourceHand,wireOwner,sizeof(wireOwner));
    char b[512]; _snprintf(b,sizeof(b)-1,
        "[wd] APPLY id=%u sid='%s' owner=%u,%u,%u,%u,%u moved=%d pos=%.2f,%.2f,%.2f tracked=%u dropOwner=%u type=%u itemhand=%u,%u,%u,%u,%u",
        p.dropId,p.stringID,p.oType,p.oContainer,p.oContainerSerial,p.oIndex,
        p.oSerial,moved,p.x,p.y,p.z,(unsigned)tracks.size(),p.ownerId,p.itemType,
        itemHand[0],itemHand[1],itemHand[2],itemHand[3],itemHand[4]);
    b[sizeof(b)-1]='\0'; coop::logLine(b);
    return true;
}


void Replicator::parkPendingPickup(const unsigned int targetHand[5], const char* sid,
                                   unsigned int itemType, u32 refOwnerId, u32 refDropId) {
    // Idempotent: a reliable resend of the same intent must not stack retries.
    for (std::deque<PendingPickup>::iterator it = pendingPickups_.begin();
         it != pendingPickups_.end(); ++it)
        if (it->refOwnerId == refOwnerId && it->refDropId == refDropId) return;
    PendingPickup pp;
    for (int k=0;k<5;++k) pp.targetHand[k]=targetHand[k]; // canonical wire hand
    pp.sid = sid ? sid : ""; pp.itemType = itemType;
    pp.refOwnerId = refOwnerId; pp.refDropId = refDropId; pp.sinceMs = nowMs();
    pendingPickups_.push_back(pp);
}

void Replicator::trackGroundGear(const std::string& sid, u32 dropOwnerId, u32 dropId,
                                 void* item) {
    GroundWeapon g;
    g.dropOwnerId=dropOwnerId; g.dropId=dropId; g.item=item;
    memset(g.hand,0,sizeof(g.hand));
    memset(g.sourceHand,0,sizeof(g.sourceHand));
    // This is the LOCAL native full hand of the actual relocated object. It is
    // not a claim that item hands are save-stable or shared between clients.
    if (item) {
        InvItemEntry metadata;
        engine::readPhysicalItemMetadata(reinterpret_cast<Item*>(item),&metadata,g.hand);
    }
    groundedWeapons_[sid].push_back(g);
}

namespace {

// Existing diagnostic refusal levers retain the exact intent pending. They are
// not proof of native inventory rejection or gameplay conservation; real-engine
// scenarios must exercise the native mutation and verify physical identity.
//   KENSHICOOP_WD_REFUSE_REHOME=1     refuse ONCE   -> exercises the retry
//   KENSHICOOP_WD_REFUSE_REHOME_ALL=1 refuse ALWAYS -> pending/held, never copy or destroy.
// Consulted by both initial and pending exact physical re-home attempts.
bool injectRehomeRefusal(const char* sid,u32 refOwner,u32 refId) {
    static int mode = -1; // 0 = off, 1 = first only, 2 = always
    if (mode < 0) {
        const char* all = getenv("KENSHICOOP_WD_REFUSE_REHOME_ALL");
        const char* one = getenv("KENSHICOOP_WD_REFUSE_REHOME");
        mode = (all && all[0] == '1') ? 2 : ((one && one[0] == '1') ? 1 : 0);
    }
    if (mode == 0) return false;
    if (mode == 1) mode = 0;
    char b[240]; _snprintf(b,sizeof(b)-1,
        "[wd] REHOME-REFUSE-INJECTED (test lever: re-home refused) sid='%s' ref=%u/%u",sid,refOwner,refId);
    b[sizeof(b)-1]='\0'; coop::logLine(b);
    return true;
}

// TEST-ONLY fault injection: KENSHICOOP_WD_TRANSIENT_DEAD=N makes the first N PICKUP-time
// resolutions of a tracked ground object read as dead, then lets them succeed. That is the real
// cause of the duplicate the player saw - the engine streams an object out and back, so a single
// read disagrees with the world for a moment - and it cannot be arranged deterministically any
// other way. Consulted ONLY at the pickup site: the per-tick reconcile would otherwise eat the
// budget before the intent ever arrives.
//
// The distinction it gates is whether an unsatisfied pickup is a VERDICT or a retry. Concluding
// from the one bad read erases the track and answers the peer with nothing, leaving our ground
// copy next to the item the peer now holds; parking it lets the next read - which succeeds -
// finish the re-home.
static bool injectTransientDead(const char* sid,u32 refOwner,u32 refId) {
    static int budget = -1;
    if (budget < 0) {
        const char* e = getenv("KENSHICOOP_WD_TRANSIENT_DEAD");
        budget = e ? atoi(e) : 0;
        if (budget < 0) budget = 0;
    }
    if (budget == 0) return false;
    --budget;
    char b[240]; _snprintf(b,sizeof(b)-1,
        "[wd] TRANSIENT-DEAD-INJECTED (test lever: this read reports the object gone) sid='%s' ref=%u/%u",
        sid,refOwner,refId);
    b[sizeof(b)-1]='\0'; coop::logLine(b);
    return true;
}

// Accept only this locally recorded full native identity and an actual onGround
// item. A recycled pointer, cursor object, or unloaded handle is never a substitute.
void* resolveGroundGear(const unsigned int hand[5]) {
    unsigned int owner[5]; bool inBag=false; void* physical=0;
    return engine::readPhysicalItemLocation(hand,owner,&inBag,&physical) && !inBag ? physical : 0;
}
} // namespace

void Replicator::retryPendingPickups(GameWorld* gw) {
    unsigned long now=nowMs();
    unsigned int attempts=(unsigned int)pendingPickups_.size();
    if (attempts>64) attempts=64;
    for (unsigned int n=0;n<attempts;++n) {
        PendingPickup& pp=pendingPickups_.front();
        std::pair<u32,u32> drop(pp.refOwnerId,pp.refDropId);
        Key wire=keyForInvLocalHand(pp.targetHand);
        unsigned int targetHand[5];
        bool keep=true;
        if (pendingDrops_.count(drop) || !resolveInvWireHand(wire,targetHand)) {
            pp.sinceMs=now; // zone waiting is not a failed re-home attempt
        } else {
            int moved=0; unsigned int before[5]={0,0,0,0,0},after[5]={0,0,0,0,0};
            std::map<std::string,std::deque<GroundWeapon> >::iterator sit=groundedWeapons_.find(pp.sid);
            if (sit!=groundedWeapons_.end()) {
                for (std::deque<GroundWeapon>::iterator g=sit->second.begin();g!=sit->second.end();++g) {
                    if (g->dropOwnerId!=pp.refOwnerId || g->dropId!=pp.refDropId) continue;
                    void* ro=resolveGroundGear(g->hand);
                    InvItemEntry row; unsigned int actual[5];
                    if (ro && engine::readPhysicalItemMetadata(reinterpret_cast<Item*>(ro),&row,actual) &&
                        row.itemType==pp.itemType && strcmp(row.stringID,pp.sid.c_str())==0) {
                        memcpy(before,g->hand,sizeof(before));
                        moved=injectRehomeRefusal(pp.sid.c_str(),pp.refOwnerId,pp.refDropId) ? 0 :
                            engine::addItemPtrToInventory(gw,targetHand,ro);
                        if (moved) {
                            engine::readObjectHand(reinterpret_cast<RootObject*>(ro),after);
                            sit->second.erase(g);
                        }
                    }
                    break;
                }
            }
            if (moved) {
                char b[512]; _snprintf(b,sizeof(b)-1,
                    "[wd] PICKUP-RETRY-OK sid='%s' ref=%u/%u via=track afterMs=%lu type=%u "
                    "target=%u,%u,%u,%u,%u beforeItemhand=%u,%u,%u,%u,%u afterItemhand=%u,%u,%u,%u,%u",
                    pp.sid.c_str(),pp.refOwnerId,pp.refDropId,(unsigned long)(now-pp.sinceMs),pp.itemType,
                    targetHand[0],targetHand[1],targetHand[2],targetHand[3],targetHand[4],
                    before[0],before[1],before[2],before[3],before[4],after[0],after[1],after[2],after[3],after[4]);
                b[sizeof(b)-1]='\0'; coop::logLine(b);
                keep=false;
            }
        }
        if (keep) {
            // deque end insertion preserves references. Swap the string rather
            // than allocate/copy it on every retry as we rotate for fairness.
            pendingPickups_.push_back(PendingPickup());
            PendingPickup& next=pendingPickups_.back();
            memcpy(next.targetHand,pp.targetHand,sizeof(next.targetHand));
            next.sid.swap(pp.sid); next.itemType=pp.itemType;
            next.refOwnerId=pp.refOwnerId; next.refDropId=pp.refDropId; next.sinceMs=pp.sinceMs;
        }
        pendingPickups_.pop_front();
    }
}

void Replicator::reconcileGroundGear(GameWorld* gw) {
    // Unreadable/unloaded is not destruction. Retain each exact physical track
    // until a positively observed pickup consumes it or the session is reset.
    retryPendingPickups(gw);

}

void Replicator::applyWeaponPickup(GameWorld* gw, const WorldPickupPacket& p) {
        std::pair<u32, u32> id(p.ownerId, p.pickupId);
        if (appliedPickups_.count(id) != 0) return;
        appliedPickups_.insert(id);
        if (appliedPickups_.size() > 4096) appliedPickups_.erase(appliedPickups_.begin());
        Key ok; ok.t = p.oType; ok.c = p.oContainer; ok.cs = p.oContainerSerial;
        ok.i = p.oIndex; ok.s = p.oSerial;
        unsigned int wireHand[5]={p.oType,p.oContainer,p.oContainerSerial,p.oIndex,p.oSerial};
        unsigned int targetHand[5];
        bool targetLoaded=resolveInvWireHand(ok,targetHand);
        if (targetLoaded && ownHands_.count(keyForInvLocalHand(targetHand))) return;
        if (p.refDropId &&
            (!targetLoaded || pendingDrops_.count(std::make_pair(p.refDropOwnerId,p.refDropId)))) {
            parkPendingPickup(wireHand,p.stringID,p.itemType,p.refDropOwnerId,p.refDropId);
            char b[180]; _snprintf(b,sizeof(b)-1,
                "[wd] PICKUP-WAIT-DROP id=%u ref=%u/%u (physical drop or target not loaded)",
                p.pickupId,p.refDropOwnerId,p.refDropId);
            b[sizeof(b)-1]='\0'; coop::logLine(b);
            return;
        }
        if (!targetLoaded) return;
        std::deque<GroundWeapon>& q = groundedWeapons_[std::string(p.stringID)];
        int moved=0; unsigned int before[5]={0,0,0,0,0},after[5]={0,0,0,0,0};
        std::deque<GroundWeapon>::iterator pick = q.end();
        if (p.refDropId != 0) {
            // Re-home the EXACT instance the picker named (both clients tracked it under this
            // (owner,id)). If we don't have it tracked, do NOT guess a same-sid copy - that is
            // the very mistake this identity fixes; a genuine peer copy will match.
            for (std::deque<GroundWeapon>::iterator g = q.begin(); g != q.end(); ++g)
                if (g->dropOwnerId == p.refDropOwnerId && g->dropId == p.refDropId) { pick = g; break; }
        }
        const char* why = "ok";
        if (pick != q.end()) {
            // Re-resolve rather than trust the cached Item*: it may name an object the engine
            // has since despawned, and a dead pointer into addItemPtrToInventory is how a
            // re-home "succeeded" at nothing while our ground copy stayed put.
            memcpy(before,pick->hand,sizeof(before));
            void* ro=injectTransientDead(p.stringID,p.refDropOwnerId,p.refDropId) ? 0 : resolveGroundGear(pick->hand);
            InvItemEntry row; unsigned int actual[5];
            if (ro && (!engine::readPhysicalItemMetadata(reinterpret_cast<Item*>(ro),&row,actual) ||
                row.itemType!=p.itemType || strcmp(row.stringID,p.stringID)!=0)) ro=0;
            if (!ro) {
                // Unreadable or streamed out is not destroyed. Retain this
                // exact physical reference AND its pickup intent.
                why = "deferred";
                parkPendingPickup(wireHand, p.stringID, p.itemType,
                                  p.refDropOwnerId, p.refDropId);
            } else {
                moved=injectRehomeRefusal(p.stringID,p.refDropOwnerId,p.refDropId) ? 0 :
                    engine::addItemPtrToInventory(gw,targetHand,ro);
                if (moved) {
                    engine::readObjectHand(reinterpret_cast<RootObject*>(ro),after);
                    q.erase(pick); // re-homed; stop tracking it on the ground
                } else {
                    // Keep the exact object AND the intent, holding the target
                    // snapshot until the real inventory accepts this object.
                    why="refused";
                    parkPendingPickup(wireHand,p.stringID,p.itemType,
                                      p.refDropOwnerId,p.refDropId);
                }
            }
        } else if (p.refDropId != 0) {
            // No exact physical reference: wait for its drop/loaded track.
            // A same-template object nearby does not prove this item's identity.
            why="deferred";
            parkPendingPickup(wireHand,p.stringID,p.itemType,p.refDropOwnerId,p.refDropId);
        } else {
            // NO FALLBACK for an identity-less pickup. Re-homing "the oldest same-sid copy" on
            // a ref=0/0 intent means a bogus pickup TELEPORTS an unrelated ground item into a
            // bag - the census used to emit a burst of exactly those at connect. Without an
            // instance identity we cannot know which object was taken, so we do nothing: the
            // peer's own snapshot still converges its bag, and our ground copy is left alone.
            why = "no-identity";
        }
        char b[640]; _snprintf(b,sizeof(b)-1,
            "[wd] PICKUP-APPLY id=%u sid='%s' owner=%u,%u,%u,%u,%u ref=%u/%u moved=%d why=%s trackedLeft=%u "
            "pickupOwner=%u type=%u target=%u,%u,%u,%u,%u beforeItemhand=%u,%u,%u,%u,%u afterItemhand=%u,%u,%u,%u,%u",
            p.pickupId,p.stringID,p.oType,p.oContainer,p.oContainerSerial,p.oIndex,p.oSerial,
            p.refDropOwnerId,p.refDropId,moved,why,(unsigned)q.size(),p.ownerId,p.itemType,
            targetHand[0],targetHand[1],targetHand[2],targetHand[3],targetHand[4],
            before[0],before[1],before[2],before[3],before[4],after[0],after[1],after[2],after[3],after[4]);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        // Keep the transfer detector blind to the relocation we just made.

}

// ---- Protocol 37: cross-owner transfer intents ------------------------------


bool Replicator::xferPendingLoss(const Key& k, const char* sid) {
    for (std::map<u32,XferOut>::const_iterator x=xferOut_.begin();x!=xferOut_.end();++x)
        if (!(x->second.src < k) && !(k < x->second.src) && x->second.key.first == sid)
            return true;
    return false;
}

bool Replicator::wdSuppressed(const Key& k, const char* sid, unsigned long now) {
    std::map<std::pair<Key, std::string>, unsigned long>::iterator it =
        wdSuppress_.find(std::make_pair(k, std::string(sid)));
    if (it == wdSuppress_.end()) return false;
    if (now > it->second) { wdSuppress_.erase(it); return false; }
    return true;
}

bool Replicator::wdPendingDrop(const Key& k) const {
    unsigned int hand[5]={k.t,k.c,k.cs,k.i,k.s};
    if (engine::physicalGroundMutationPending(hand)) return true;
    Key wire=invWireKeyForLocal(k);
    for (std::map<Key,WorldDropPacket>::const_iterator d=pendingNpcDrops_.begin();
         d!=pendingNpcDrops_.end();++d) {
        const WorldDropPacket& p=d->second;
        Key owner; owner.t=p.oType; owner.c=p.oContainer; owner.cs=p.oContainerSerial;
        owner.i=p.oIndex; owner.s=p.oSerial;
        if (!(owner<wire) && !(wire<owner)) return true;
    }
    for (std::map<std::pair<u32,u32>,WorldDropPacket>::const_iterator d=pendingDrops_.begin();
         d!=pendingDrops_.end();++d) {
        const WorldDropPacket& p=d->second;
        Key owner; owner.t=p.oType; owner.c=p.oContainer; owner.cs=p.oContainerSerial;
        owner.i=p.oIndex; owner.s=p.oSerial;
        if ((!(owner<wire) && !(wire<owner)) || (!(owner<k) && !(k<owner))) return true;
    }
    for (std::deque<PendingPickup>::const_iterator p=pendingPickups_.begin();
         p!=pendingPickups_.end();++p) {
        Key target=keyForInvLocalHand(p->targetHand);
        if (!(target<wire) && !(wire<target)) return true;
    }
    return false;
}

void Replicator::detectAndPublishTransfers(GameWorld* gw, NetLink& net, u32 ownerId) {
    unsigned long now = nowMs();
    engine::configureInventoryTransactions(isHostRole(),moneySync_ || storeSync_,moneySync_);
    engine::NativeShopIntent shop[64];
    unsigned int shops=engine::drainNativeShopTransactions(shop,64);
    for (unsigned int si=0;si<shops;++si) {
        // Freeze was established by the native hook. Publish that exact, still
        // frozen actor BEFORE its intent; a periodic snapshot may predate a
        // legitimate native change immediately before enqueue.
        const unsigned int* actorHand=shop[si].operation==2 ?
            shop[si].source : shop[si].destination;
        static InvItemEntry actorRows[INV_SNAPSHOT_ITEMS_MAX];
        bool actorTruncated=false; u32 actorHash=0;
        unsigned int actorCount=engine::captureContainerContents(gw,actorHand,actorRows,
            INV_SNAPSHOT_ITEMS_MAX,&actorHash,&actorTruncated,true);
        if (actorTruncated || !engine::transactionInventory(actorHand) ||
            !ownsInventoryLocalHand(actorHand)) {
            // No request or native mutation escaped; release only this refused
            // enqueue's actor gate, never an unrelated received checkpoint.
            engine::setInventoryCheckpointPending(actorHand,false);
            coop::logLine("[shop] REFUSE unavailable complete native actor publication");
            continue;
        }
        shop[si].actorRevision=engine::observeInventoryRevision(actorHand,actorHash);
        u32 actorWire[5]; u8 actorKind;
        inventorySnapshotIdentity(keyForInvLocalHand(actorHand),actorWire,actorKind);
        net.queueInvSnapshot(ownerId,actorKind,actorWire,actorRows,actorCount,0,
                             shop[si].actorRevision);
        InvXferPacket packet; memset(&packet,0,sizeof(packet));
        packet.type=PKT_INV_XFER; packet.ownerId=ownerId; packet.xferId=nextXferId_++;
        Key source=keyForInvLocalHand(shop[si].source),destination=keyForInvLocalHand(shop[si].destination);
        Key ws=invWireKeyForLocal(source),wd=invWireKeyForLocal(destination);
        packet.sType=ws.t; packet.sContainer=ws.c; packet.sContainerSerial=ws.cs;
        packet.sIndex=ws.i; packet.sSerial=ws.s;
        packet.dType=wd.t; packet.dContainer=wd.c; packet.dContainerSerial=wd.cs;
        packet.dIndex=wd.i; packet.dSerial=wd.s;
        memcpy(packet.stringID,shop[si].item.stringID,sizeof(packet.stringID));
        packet.itemType=shop[si].item.itemType; packet.quantity=shop[si].item.quantity;
        packet.quality=shop[si].item.quality; packet.level=shop[si].item.level;
        memcpy(packet.manufacturer,shop[si].item.manufacturer,sizeof(packet.manufacturer));
        memcpy(packet.material,shop[si].item.material,sizeof(packet.material));
        packet.operation=shop[si].operation; packet.sourceRevision=shop[si].revision;
        packet.actorRevision=shop[si].actorRevision;
        if (packet.operation==2) packet.sourceRevision=packet.actorRevision;
        packet.quotedTotal=shop[si].quotedTotal;
        XferOut pending; pending.src=source; pending.dst=destination;
        pending.key=XKey(std::string(packet.stringID),packet.itemType); pending.qty=packet.quantity;
        pending.srcPeer=!ownsInventoryLocalHand(shop[si].source);
        pending.dstPeer=!ownsInventoryLocalHand(shop[si].destination);
        pending.peerId=0; pending.sentMs=now; pending.rollbackQty=0; pending.appliedQty=0;
        pending.answered=false; pending.bagItem=0; pending.verdictToken=0;
        pending.hasCanonicalBag=false; pending.operation=packet.operation;
        pending.retired=false;
        xferOut_[packet.xferId]=pending;
        std::vector<InvItemEntry> noContents;
        net.queueInvXfer(packet,noContents);
    }

    // Exact native remove/add pairs, not coincidental same-template count changes.
    // In particular this also catches food taken from or deposited into a backpack.
    static engine::ItemTransferEdge edges[256]; // game thread only
    unsigned int ne = engine::drainItemTransfers(edges, 256);
    for (unsigned int i = 0; i < ne; ++i) {
        std::vector<InvItemEntry> contents;
        engine::takeItemTransferContents(edges[i].contentsSlot, contents);
        struct Fire { Key src, dst; XKey key; int qty; } f;
        f.src = keyForInvLocalHand(edges[i].src);
        f.dst = keyForInvLocalHand(edges[i].dst);
        f.key = XKey(std::string(edges[i].item.stringID), edges[i].item.itemType);
        f.qty = edges[i].item.quantity;
        if (f.qty <= 0) { engine::finishItemTransfer(edges[i].verdictToken); continue; }
        // Returning a cursor item to the same owner's inventory is not a transfer.
        // There is no remote quantity change to commit or reverse.
        if (!(f.src < f.dst) && !(f.dst < f.src)) {
            engine::finishItemTransfer(edges[i].verdictToken); continue;
        }
        bool srcOwn = ownsInventoryLocalHand(edges[i].src);
        bool dstOwn = ownsInventoryLocalHand(edges[i].dst);
        if (!srcOwn || !dstOwn) {
            // At least one end is peer-authored: the single-writer snapshots cannot
            // carry this move - author the reliable transfer intent.
            InvXferPacket pkt; memset(&pkt, 0, sizeof(pkt));
            pkt.type = (u8)PKT_INV_XFER; pkt.ownerId = ownerId; pkt.xferId = nextXferId_++;
            Key ws = invWireKeyForLocal(f.src), wd = invWireKeyForLocal(f.dst);
            pkt.sType = ws.t; pkt.sContainer = ws.c; pkt.sContainerSerial = ws.cs;
            pkt.sIndex = ws.i; pkt.sSerial = ws.s;
            pkt.dType = wd.t; pkt.dContainer = wd.c; pkt.dContainerSerial = wd.cs;
            pkt.dIndex = wd.i; pkt.dSerial = wd.s;
            strncpy(pkt.stringID, f.key.first.c_str(), sizeof(pkt.stringID) - 1);
            pkt.itemType = f.key.second;
            pkt.quantity = (u16)f.qty;
            pkt.quality = edges[i].item.quality;
            pkt.level = edges[i].item.level;
            memcpy(pkt.manufacturer, edges[i].item.manufacturer, sizeof(pkt.manufacturer));
            memcpy(pkt.material, edges[i].item.material, sizeof(pkt.material));
            pkt.contentsPresent = engine::isContainerItemType(pkt.itemType) ? 1 : 0;
            pkt.operation=!isHostRole() && ownerClassForHand(edges[i].src)==0 ? 3 : 0;
            pkt.sourceRevision=engine::inventoryRevision(edges[i].src);
            net.queueInvXfer(pkt, contents);
            // Latch the pending move on each PEER end so applyInventories cannot
            // reconcile it back while the owner's snapshots are still stale.
            if (!srcOwn) {
                XferLatch& L = xferLatch_[f.src][f.key];
                L.delta -= f.qty;
                if (L.delta == 0) xferLatch_[f.src].erase(f.key);
            }
            if (!dstOwn) {
                XferLatch& L = xferLatch_[f.dst][f.key];
                L.delta += f.qty;
                if (L.delta == 0) xferLatch_[f.dst].erase(f.key);
            }
            // A gear trade must not be read by the W2 weapon census as a ground
            // drop (src) / pickup (dst) of the same sid.
            if (isGearType(f.key.second)) {
                const unsigned long gearSuppressMs=10000;
                wdSuppress_[std::make_pair(f.src, f.key.first)] = now + gearSuppressMs;
                wdSuppress_[std::make_pair(f.dst, f.key.first)] = now + gearSuppressMs;
            }
            // Own inventory publication stays pinned until the receiver decides.
            XferOut o;
            o.operation=pkt.operation;
            o.retired=false;
            o.src = f.src; o.dst = f.dst; o.key = f.key; o.qty = f.qty;
            o.srcPeer = !srcOwn; o.dstPeer = !dstOwn; o.sentMs = now;
            std::map<Key, InvRecv>::const_iterator authority =
                invRecv_.find(!srcOwn ? f.src : f.dst);
            o.peerId = authority != invRecv_.end() ? authority->second.ownerId : MAX_PLAYERS;
            o.rollbackQty = 0; o.appliedQty = 0; o.answered = false;
            o.bagItem = engine::isContainerItemType(pkt.itemType) ? edges[i].transferredItem : 0;
            o.verdictToken = edges[i].verdictToken;
            o.hasCanonicalBag = false;
            xferOut_[pkt.xferId] = o;
            char b[240]; _snprintf(b, sizeof(b) - 1,
                "[xfer] SEND id=%u sid='%s' type=%u qty=%d src=%u,%u,%u,%u,%u(%s) dst=%u,%u,%u,%u,%u(%s)",
                pkt.xferId, pkt.stringID, pkt.itemType, f.qty,
                f.src.t, f.src.c, f.src.cs, f.src.i, f.src.s, srcOwn ? "own" : "peer",
                f.dst.t, f.dst.c, f.dst.cs, f.dst.i, f.dst.s, dstOwn ? "own" : "peer");
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        } else engine::finishItemTransfer(edges[i].verdictToken);
    }
}

void Replicator::applyTransfers(GameWorld* gw, Inbound& in, NetLink& net, u32 localId) {
    std::deque<InboundInvXfer> got;
    in.drainInvXfers(got);
    typedef std::pair<u32, u32> TransferId;
    for (std::deque<InboundInvXfer>::iterator it = got.begin(); it != got.end(); ++it) {
        const InvXferPacket& p = it->pkt;
        if (p.ownerId == localId) continue;
        if (p.operation && !isHostRole()) continue;
        if (inventory_transaction::remember(transactionIntents_,p)==inventory_transaction::CONFLICT) {
            coop::logLine("[xfer] REJECT conflicting operation replay"); continue;
        }
        TransferId id(p.ownerId, p.xferId);
        std::map<TransferId, InboundInvXferAck>::iterator done = appliedXfers_.find(id);
        if (done != appliedXfers_.end()) {
            // The reliable stream already carries the original commerce commit.
            // Never build a NEW actor snapshot for an old operation ID: the
            // actor may have consumed or moved its purchase since that commit.
            if (p.operation==1 || p.operation==2) continue;
            if (done->second.pkt.ownerId == localId)
                queueXferVerdict(gw, net, p, done->second);
        } else if (pendingXfers_.find(id)==pendingXfers_.end()) {
            InboundInvXfer& pending = pendingXfers_[id];
            pending.ownerId = it->ownerId; pending.pkt = p;
            pending.contents.swap(it->contents);
        }
    }
    unsigned long now = nowMs();
    for (std::map<TransferId, InboundInvXfer>::iterator it = pendingXfers_.begin();
         it != pendingXfers_.end();) {
        const InvXferPacket& p = it->second.pkt;
        // HOST may have detached shop stock into its own native drag cursor.
        // Keep the request queued until the physical baseline is whole again;
        // rejection here would consume the operation without a stock conflict.
        if ((p.operation==INV_OP_BUY || p.operation==INV_OP_SELL) &&
            engine::nativeInventoryCursorActive()) { ++it; continue; }
        Key sk; sk.t=p.sType; sk.c=p.sContainer; sk.cs=p.sContainerSerial; sk.i=p.sIndex; sk.s=p.sSerial;
        Key dk; dk.t=p.dType; dk.c=p.dContainer; dk.cs=p.dContainerSerial; dk.i=p.dIndex; dk.s=p.dSerial;
        unsigned int sHand[5], dHand[5];
        // A hand which is still streaming is not a rejection and must not consume the id.
        if (!resolveInvWireHand(sk,sHand) || !resolveInvWireHand(dk,dHand)) { ++it; continue; }
        sk = keyForInvLocalHand(sHand); dk = keyForInvLocalHand(dHand);
        bool srcOwn = ownsInventoryLocalHand(sHand);
        bool dstOwn = ownsInventoryLocalHand(dHand);
        // Observers consume the owners' final snapshots, not a third physical mutation.
        if (!p.operation && !srcOwn && !dstOwn) { pendingXfers_.erase(it++); continue; }
        // Canonical contents/physical transfers cannot commit around another
        // unresolved exact floor mutation at either inventory root.
        if (wdPendingDrop(sk) || wdPendingDrop(dk)) { ++it; continue; }
        bool authorized=true;
        if (p.operation) {
            const Key& actor=p.operation==2 ? sk : dk;
            const Key actorWire=invWireKeyForLocal(actor);
            if (inventoryResyncPeers_.count(p.ownerId) ||
                inventoryResyncActors_.count(actorWire) ||
                inventoryAwaitingActorAck_.count(actorWire)) { ++it; continue; }
            std::map<Key,InvRecv>::const_iterator known=invRecv_.find(actor);
            if (known==invRecv_.end() || known->second.dirty) { ++it; continue; }
            authorized=known->second.ownerId==p.ownerId;
            if (p.operation==1 || p.operation==2) {
                if (p.actorRevision && known->second.revision<p.actorRevision) { ++it; continue; }
                authorized=authorized && p.actorRevision &&
                           known->second.revision==p.actorRevision;
            }
            if (p.operation==1) authorized=authorized && engine::isMerchantInventory(sHand);
            if (p.operation==2) authorized=authorized && engine::isMerchantInventory(dHand);
            if (p.operation==3) authorized=authorized && srcOwn &&
                                                     !engine::isMerchantInventory(sHand);
            // Catch a HOST native extraction or purchase that happened since the
            // last periodic publication before checking the client's revision.
            if (srcOwn) {
                static InvItemEntry current[INV_SNAPSHOT_ITEMS_MAX]; u32 hash=0; bool truncated=false;
                engine::captureContainerContents(gw,sHand,current,INV_SNAPSHOT_ITEMS_MAX,&hash,&truncated,true);
                if (truncated) authorized=false;
                else engine::observeInventoryRevision(sHand,hash);
            }
        } else if (isHostRole() && (engine::isMerchantInventory(sHand) ||
                   engine::isMerchantInventory(dHand) || ownerClassForHand(sHand)==0)) {
            // Operation zero cannot be used to bypass shop prices or extraction
            // revisions by disguising a world-container request as a squad drag.
            authorized=false;
        }
        std::vector<InvItemEntry> actualBag;
        bool bag = engine::isContainerItemType(p.itemType);
        // A sender-owned source carries its exact contents at native hand-off.
        // A receiver-owned source supplies its own actual contents in the verdict.
        int wallet=-1,moved=0;
        if (authorized) {
            if (p.operation==1 || p.operation==2)
                moved=engine::commitNativeShopTransaction(gw,sHand,dHand,p,&wallet);
            else if (p.operation==3)
                moved=engine::commitNativeExtraction(gw,sHand,dHand,p);
            else moved=(!bag || p.contentsPresent) ? engine::moveItemBetweenContainers(
                gw,sHand,dHand,p.stringID,p.itemType,p.quantity,true,
                bag && !srcOwn ? &it->second.contents : 0,
                bag && srcOwn ? &actualBag : 0) : 0;
        }
        if (moved && p.operation==2) engine::noteInventoryRevision(sHand,0);
        XKey key(std::string(p.stringID),p.itemType);
        if (moved > 0 && p.operation!=1 && p.operation!=2) {
            if (!srcOwn) {
                XferLatch& l=xferLatch_[sk][key]; l.delta-=moved;
            }
            if (!dstOwn) {
                XferLatch& l=xferLatch_[dk][key]; l.delta+=moved;
            }
        }
        if (isGearType(p.itemType)) {
            wdSuppress_[std::make_pair(sk,key.first)]=now+10000;
            wdSuppress_[std::make_pair(dk,key.first)]=now+10000;
        }
        InvXferAckPacket ack; memset(&ack,0,sizeof(ack));
        ack.type=(u8)PKT_INV_XFER_ACK;
        ack.operation=p.operation; ack.wallet=wallet;
        ack.ownerId=p.operation ? localId : (srcOwn || dstOwn) ? localId : MAX_PLAYERS;
        ack.xferOwnerId=p.ownerId; ack.xferId=p.xferId;
        ack.applied=(u16)moved; ack.requested=p.quantity;
        ack.verdict=(u8)(moved == 0 ? XFER_ACK_REJECT :
                         moved == p.quantity ? XFER_ACK_ACCEPT : XFER_ACK_PARTIAL);
        ack.contentsPresent = bag && srcOwn && moved > 0 ? 1 : 0;
        InboundInvXferAck& result = appliedXfers_[it->first];
        result.ownerId = ack.ownerId; result.pkt = ack;
        if (ack.contentsPresent) result.contents.swap(actualBag);
        if (ack.ownerId == localId) queueXferVerdict(gw, net, p, result);
        char b[240]; _snprintf(b,sizeof(b)-1,
            "[xfer] APPLY id=%u from=%u sid='%s' type=%u qty=%u moved=%d ack=%u authority=%d",
            p.xferId,p.ownerId,p.stringID,p.itemType,(unsigned)p.quantity,moved,
            (unsigned)ack.verdict,(int)(ack.ownerId == localId));
        b[sizeof(b)-1]='\0'; coop::logLine(b);
        pendingXfers_.erase(it++);
    }
}

void Replicator::resyncInventoryPeer(GameWorld* gw,NetLink& net,u32 peer) {
    (void)gw; (void)net;
    if (isHostRole() && peer>0 && peer<MAX_PLAYERS) inventoryResyncPeers_.insert(peer);
}

void Replicator::retireInventoryPeer(GameWorld* gw,u32 owner) {
    (void)gw;
    // Core retires packet revisions and transfer journals after this callback.
    // Retire the corresponding engine admission too, before invRecv_ disappears:
    // a new writer may restart at revision 1, and old local stock is not a quote.
    for (std::map<Key,InvRecv>::const_iterator it=invRecv_.begin();it!=invRecv_.end();++it) {
        if (it->second.ownerId!=owner) continue;
        const Key& k=it->first;
        unsigned int hand[5]={k.t,k.c,k.cs,k.i,k.s};
        engine::retireInventoryRevision(hand);
    }
    for (std::map<u32,XferOut>::iterator it=xferOut_.begin();it!=xferOut_.end();++it) {
        XferOut& pending=it->second;
        if (pending.peerId!=owner && pending.peerId!=MAX_PLAYERS &&
            !(owner==0 && !isHostRole())) continue;
        pending.retired=true;
        coop::logLine("[xfer] RETIRE awaiting complete host actor resnapshot; native grant reserved");
    }
}

void Replicator::applyXferAcks(GameWorld* gw, u32 localId) {
    if (receivedXferAcks_.empty() && xferOut_.empty()) return;
    std::deque<InboundInvXferAck> got; got.swap(receivedXferAcks_);
    for (std::deque<InboundInvXferAck>::iterator it=got.begin();it!=got.end();++it) {
        const InvXferAckPacket& a=it->pkt;
        if (a.xferOwnerId != localId) continue;
        std::map<u32,XferOut>::iterator o=xferOut_.find(a.xferId);
        if (o==xferOut_.end() || o->second.answered) continue;
        XferOut& x=o->second;
        if (x.peerId == MAX_PLAYERS) {
            // A trade can precede the peer's first periodic inventory publication.
            // Its authority snapshot is ingested atomically with the verdict.
            std::map<Key,InvRecv>::const_iterator authority =
                invRecv_.find(x.srcPeer ? x.src : x.dst);
            if (authority != invRecv_.end()) x.peerId = authority->second.ownerId;
        }
        if (a.ownerId != x.peerId) continue;
        if (x.retired) continue;
        if (a.operation!=x.operation || a.requested!=x.qty) continue;
        x.answered=true;
        x.appliedQty = a.applied < x.qty ? a.applied : x.qty;
        x.rollbackQty=(x.operation==1 || x.operation==2) ? 0 : x.qty-x.appliedQty;
        if (a.applied && a.contentsPresent && x.bagItem) {
            x.canonicalBag.swap(it->contents);
            x.hasCanonicalBag = true;
        }
        char b[200]; _snprintf(b,sizeof(b)-1,
            "[xfer] ACK id=%u from=%u applied=%u/%d rollback=%d sid='%s'",
            a.xferId,a.ownerId,(unsigned)a.applied,x.qty,x.rollbackQty,x.key.first.c_str());
        b[sizeof(b)-1]='\0'; coop::logLine(b);
    }
    for (std::map<u32,XferOut>::iterator it=xferOut_.begin();it!=xferOut_.end();) {
        XferOut& x=it->second;
        if (x.retired || !x.answered) { ++it; continue; }
        if (x.operation==1 || x.operation==2) {
            // There was no optimistic commerce mutation on JOIN. HOST endpoint
            // snapshots, not a second item grant or wallet delta, commit it.
            xferOut_.erase(it++); continue;
        }
        unsigned int source[5],destination[5];
        if ((resolveInvLocalHand(x.src,source) && wdPendingDrop(keyForInvLocalHand(source))) ||
            (resolveInvLocalHand(x.dst,destination) && wdPendingDrop(keyForInvLocalHand(destination)))) {
            ++it; continue;
        }
        if (x.hasCanonicalBag) {
            if (!engine::applyTransferredBagContents(gw, x.bagItem, x.canonicalBag)) { ++it; continue; }
            x.hasCanonicalBag = false;
        }
        if (x.rollbackQty > 0) {
            unsigned int src[5],dst[5];
            if (!resolveInvLocalHand(x.src,src) || !resolveInvLocalHand(x.dst,dst)) { ++it; continue; }
            int undone=engine::moveItemBetweenContainers(gw,dst,src,x.key.first.c_str(),
                x.key.second,x.rollbackQty,true,0,0,x.bagItem,false);
            if (undone > 0) {
                if (x.srcPeer) releaseXferLatch(x.src,x.key,+undone);
                if (x.dstPeer) releaseXferLatch(x.dst,x.key,-undone);
                x.rollbackQty-=undone;
            }
            if (x.rollbackQty > 0) { ++it; continue; }
        }
        // The owner's canonical snapshot was ingested with this verdict. Release only
        // this intent; another pending move on the same endpoint keeps its own latch.
        if (x.srcPeer) {
            releaseXferLatch(x.src, x.key, +x.appliedQty);
            std::map<Key, InvRecv>::iterator r = invRecv_.find(x.src);
            if (r != invRecv_.end()) r->second.dirty = true;
        }
        if (x.dstPeer) {
            releaseXferLatch(x.dst, x.key, -x.appliedQty);
            std::map<Key, InvRecv>::iterator r = invRecv_.find(x.dst);
            if (r != invRecv_.end()) r->second.dirty = true;
        }
        engine::finishItemTransfer(x.verdictToken);
        xferOut_.erase(it++);
    }
}

// Undo one intent's contribution to a latch. `delta` is the inverse of what
// detectAndPublishTransfers added, so a lone intent lands the latch back on
// zero and it is erased; a second, still-unanswered intent on the same
// container and item leaves its own contribution behind to be cleared by its
// own verdict. Only an authenticated verdict or a complete reconnect actor
// checkpoint discharges an unresolved native reservation; elapsed time cannot.
void Replicator::releaseXferLatch(const Key& k, const XKey& key, int delta) {
    std::map<Key, std::map<XKey, XferLatch> >::iterator lt = xferLatch_.find(k);
    if (lt == xferLatch_.end()) return;
    std::map<XKey, XferLatch>::iterator le = lt->second.find(key);
    if (le == lt->second.end()) return;
    le->second.delta += delta;
    if (le->second.delta == 0) lt->second.erase(le);
    if (lt->second.empty()) xferLatch_.erase(lt);
}


} // namespace coop
