// ReplicatorItems.cpp - item planes (monolith split from Replicator.cpp,
// 2026-07-12): publishInventories/applyInventories (Phase 4a container
// contents), publishWorldItems/applyWorldItems (Phase W1 ground-item
// proxies), detectAndPublishWeaponDrops + applyWeaponDrops/Pickups (Phase W2
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

namespace coop {

Replicator::Key Replicator::keyForInvLocalHand(const unsigned int hand[5]) const {
    Key k; k.t=hand[0]; k.c=hand[1]; k.cs=hand[2]; k.i=hand[3]; k.s=hand[4];
    return k;
}

Character* Replicator::resolveEventChar(const Key& k) const {
    Character* c = engine::resolveCharByHand(k.i, k.s, k.t, k.c, k.cs);
    if (c) return c;
    std::map<Key, Character*>::const_iterator pit = proxyByKey_.find(k);
    if (pit == proxyByKey_.end() || !pit->second) return 0;
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

void Replicator::publishInventories(GameWorld* gw, NetLink& net, u32 ownerId) {
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
    // Protocol 34 (storeSync, HOST only): fold in the ~1 Hz container census -
    // every COMPLETE storage chest / machine container near the interest
    // centers becomes an authored container, riding the same per-container
    // hash + settle + safety-resend gate below. The census set is replaced
    // wholesale each pass (containers leaving interest stop being captured;
    // their invPub_ baseline survives for the return).
    if (storeSync_) {
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
    InvItemEntry items[INV_ITEMS_MAX];
    unsigned long now = nowMs();
    for (std::set<Key>::iterator it = owned.begin();
         it != owned.end(); ++it) {
        unsigned int cHand[5] = { it->t, it->c, it->cs, it->i, it->s };
        bool unsettled = false;
        for (std::map<u32,XferOut>::const_iterator x=xferOut_.begin();x!=xferOut_.end();++x)
            if ((!x->second.srcPeer && !(x->second.src < *it) && !(*it < x->second.src)) ||
                (!x->second.dstPeer && !(x->second.dst < *it) && !(*it < x->second.dst))) {
                unsettled = true; break;
            }
        if (unsettled) continue;
        if (engine::containerGuiOpen(gw,cHand) && engine::itemTransferPending(cHand)) continue;
        // Skip until the container actually resolves here (post-load it may not yet),
        // so we never blast a spurious "empty" snapshot that would wipe baked contents.
        if (!engine::containerInventoryAvailable(cHand)) continue;
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
        unsigned int n = engine::captureContainerContents(gw, cHand, items, INV_ITEMS_MAX,
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
            invPub_[*it] = p;
            pit = invPub_.find(*it);
        }
        InvPub& pub = pit->second;
        bool sent = (pub.lastSendMs != 0) || (pub.hash != 0);
        bool differs = !sent || (pub.hash != hash);
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
        // Protocol 34 wire identity: a session-placed building rides its
        // protocol-27 placer key (own placement = our hand; a minted proxy =
        // the reverse map). Characters / baked containers stay raw (kind 0).
        u8 keyKind = 0;
        u32 wireKey[5] = { it->t, it->c, it->cs, it->i, it->s };
        if (ownBuilds_.find(*it) != ownBuilds_.end()) {
            keyKind = 1;
        } else {
            std::map<Key, Key>::iterator mit = mintByLocal_.find(*it);
            if (mit != mintByLocal_.end()) {
                keyKind = 1;
                wireKey[0] = mit->second.t; wireKey[1] = mit->second.c;
                wireKey[2] = mit->second.cs; wireKey[3] = mit->second.i;
                wireKey[4] = mit->second.s;
            }
        }
        std::map<Key,Key>::const_iterator claim=publishAsWire_.find(*it);
        if (claim!=publishAsWire_.end()) {
            wireKey[0]=claim->second.t; wireKey[1]=claim->second.c; wireKey[2]=claim->second.cs;
            wireKey[3]=claim->second.i; wireKey[4]=claim->second.s;
        }
        u8 sflags = trunc ? INV_FLAG_TRUNCATED : (u8)0;
        net.queueInvSnapshot(ownerId, keyKind, wireKey, items, n, sflags);
        pub.hash = hash; pub.lastSendMs = now; pub.lastSentN = n; pub.lastSentUnits = units;
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
                    it->t, it->c, it->cs, it->i, it->s, n, INV_ITEMS_MAX);
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
    for (std::map<Key, InvRecv>::iterator it = invRecv_.begin(); it != invRecv_.end(); ++it) {
        if (!it->second.dirty) continue;
        it->second.dirty = false;
        // Never reconcile a container we author (defense-in-depth on the partition):
        // any explicitly-registered container OR any squad member we own this tick.
        if (ownedContainers_.count(it->first) != 0) continue;
        if (ownHands_.count(it->first) != 0) continue;
        if (isHostRole() && censusContainers_.count(it->first)) continue;
        const Key& k = it->first;
        // A mine's container key is the AUTHOR's hand for its own instance of
        // the node's building, which names nothing here - reconciling it
        // verbatim was a silent no-op, so the mine's output never crossed.
        unsigned int cHand[5];
        if (!resolveInvLocalHand(k,cHand)) { it->second.dirty=true; continue; }
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
        if (engine::containerGuiOpen(gw,cHand) && engine::itemTransferPending(cHand)) {
            it->second.dirty=true; continue;
        }
        // A stale parent-only list must never clear a newly transferred bag's contents.
        // Wait for the authority snapshot as a whole; do not rewrite quantities/parent indices.
        std::map<Key,std::map<XKey,XferLatch> >::iterator lt=xferLatch_.find(k);
        if (lt!=xferLatch_.end()) {
            InvItemEntry local[64];
            unsigned int nl=engine::captureContainerContents(gw,cHand,local,64,0,0,true);
            unsigned long now=nowMs();
            for (std::map<XKey,XferLatch>::iterator l=lt->second.begin();l!=lt->second.end();) {
                int want=0,have=0;
                for (unsigned int j=0;j<n;++j)
                    if (items[j].itemType==l->first.second &&
                        strcmp(items[j].stringID,l->first.first.c_str())==0) want+=items[j].quantity;
                for (unsigned int j=0;j<nl;++j)
                    if (local[j].itemType==l->first.second &&
                        strcmp(local[j].stringID,l->first.first.c_str())==0) have+=local[j].quantity;
                if (want==have || now>l->second.deadlineMs) lt->second.erase(l++);
                else ++l;
            }
            if (!lt->second.empty()) { it->second.dirty=true; continue; }
            xferLatch_.erase(lt);
        }
        engine::applyContainerContents(gw, cHand, items, n, it->second.truncated);
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

RootObject* Replicator::liveWorldProxy(const WorldProxy& wp) {
    if (!wp.obj) return 0;
    // No verified hand: nothing to check against, so this is the pre-existing
    // pointer-trust behaviour. Rare (the mint round-trip nearly always succeeds)
    // and visible in the log as "SPAWN ... hand=0,0".
    if (!wp.hand[3] && !wp.hand[4]) return wp.obj;
    RootObject* ro = engine::resolveObjectByHand(wp.hand);
    // A DIFFERENT object behind our hand means the engine recycled the table slot
    // after destroying our proxy, so ours is gone and this one is somebody else's.
    return (ro == wp.obj) ? ro : 0;
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
        RootObject* live = liveWorldProxy(pi->second);
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
            if (isGearType(de[i].itemType)) continue;
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
            // Only the owner authors a squad drop; world-NPC drops belong to the host.
            int ownerClass = ownerClassForHand(de[i].ownerHand);
            if (ownerClass == 2 || (ownerClass == 0 && !isHostRole())) continue;
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
            if (dumpWi) { char b[200]; _snprintf(b, sizeof(b) - 1,
                "[wi] DROP-CAP netId=%u sid='%s' qty=%u pos=%.2f,%.2f,%.2f (query-free)",
                t.netId, t.stringID, t.quantity, t.x, t.y, t.z);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b); }
        }
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
                    char b[200]; _snprintf(b, sizeof(b) - 1,
                        "[wi] SEND netId=%u sid='%s' qty=%u pos=%.2f,%.2f,%.2f hash=%u",
                        tr.netId, tr.stringID, tr.quantity, pos[0], pos[1], pos[2], h);
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
            bool pickedUp = false;
            // A proxy the engine has already destroyed reads as "not picked up", so
            // it falls through to the erase below and claims nothing. That is the
            // right way round: a zone unload must not be mistaken for a pickup, or
            // we would tell the author to destroy an item nobody took.
            RootObject* live = liveWorldProxy(pi->second);
            if (live && engine::groundObjectLiveness(live, 0, &pickedUp)) { ++pi; continue; }
            if (pickedUp) {
                claims[pi->first.first].push_back(pi->first.second);
                if (dumpWi) { char b[160]; _snprintf(b, sizeof(b) - 1,
                    "[wi] CLAIM author=%u netId=%u (proxy consumed locally)",
                    pi->first.first, pi->first.second);
                    b[sizeof(b) - 1] = '\0'; coop::logLine(b); }
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
                    wp.obj = obj; wp.x = e->x; wp.y = e->y; wp.z = e->z; wp.hash = 0;
                    worldProxies_[pk] = wp;
                }
                // hand=0,0 marks a proxy we can only track by pointer - the one
                // remaining way a stale pointer can reach the engine.
                char b2[224]; _snprintf(b2, sizeof(b2) - 1,
                    "[wi] SPAWN owner=%u netId=%u ok=%d sid='%s' pos=%.2f,%.2f,%.2f hand=%u,%u",
                    b->ownerId, e->netId, obj ? 1 : 0, e->stringID, e->x, e->y, e->z,
                    wp.hand[3], wp.hand[4]);
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
                    RootObject* live = liveWorldProxy(wp);
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
            if (testStale && pit->second.obj) {
                engine::removeWorldItemProxy(gw, pit->second.obj);
                char tb[144]; _snprintf(tb, sizeof(tb) - 1,
                    "[wi] TEST-STALE owner=%u netId=%u (proxy freed under the cull)",
                    b->ownerId, *id);
                tb[sizeof(tb) - 1] = '\0'; coop::logLine(tb);
            }
            // Already gone (its block unloaded before the cull arrived): drop the
            // mapping and leave the engine alone. Destroying it a second time is a
            // double-destroy on freed memory, which the engine notices - it logs
            // "alredy has destroy reason" - and does not survive reliably.
            RootObject* live = liveWorldProxy(pit->second);
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
            char b2[200]; _snprintf(b2, sizeof(b2) - 1,
                "[wi] CLAIM-APPLY netId=%u from=%u sid='%s' destroyed=%d",
                *id, b->ownerId, tit->second.stringID, destroyed ? 1 : 0);
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
    if (ownHands_.empty()) return;
    // Correlate the bag-loss with a FREE ground item anywhere in the interest sphere (not just
    // at the feet): a UI drop lands at the cursor, which can be many metres away. A TRADE moves
    // the item into another BAG (isInInventory=true), so it is never a free ground item - hence
    // even a generous radius cannot mistake a trade for a drop.
    const float        GROUND_R    = 60.0f;
    const int          MAX_RETRY   = 30;    // ticks to keep looking for the ground copy
    static int dumpWd = -1;
    if (dumpWd < 0) { const char* e = getenv("KENSHICOOP_INV_DUMP"); dumpWd = (e && e[0] == '1') ? 1 : 0; }
    InvItemEntry items[INV_ITEMS_MAX];
    for (std::set<Key>::iterator it = ownHands_.begin(); it != ownHands_.end(); ++it) {
        unsigned int cHand[5] = { it->t, it->c, it->cs, it->i, it->s };
        if (engine::resolveObjectByHand(cHand) == 0) continue;
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
        // ZERO-ROW READ = NO INFORMATION. A container that yields NO rows at all is
        // mid-reload / not fully resolved, not stripped, so nothing may be concluded from
        // it - neither an edge NOR a baseline. Note the test is the RAW row count, not the
        // gear-only census: a character who really does drop all their gear but still
        // carries food reads rows>0 with an empty gear map, and that IS a real full-strip
        // the drop pass must see (gating on the gear map hid it).
        //
        // Committing a zero-row read as the baseline is the worse half. It used to happen
        // on the FIRST read of every container, right at connect while inventories were
        // still resolving: the baseline went in empty, and the next tick's real contents
        // read as an INCREASE for every worn item - the burst of ref=0/0 PICKUPs at
        // 20:59:54 in the logs (one per gear item per character). Those carry no drop
        // identity, so the receiver could re-home an arbitrary same-sid ground item into
        // the bag. Seeding is therefore deferred until a read we can trust.
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
                bool matched = false;
                if (!added.empty()) {
                    void* got = added.front(); added.pop_front();
                    for (std::deque<GroundWeapon>::iterator g = q.begin(); g != q.end(); ++g) {
                        if (g->item == got) {
                            pkt.refDropOwnerId = g->dropOwnerId; pkt.refDropId = g->dropId;
                            q.erase(g); matched = true; break;
                        }
                    }
                }
                if (!matched && !q.empty()) { // couldn't pin the instance -> oldest same-sid copy
                    pkt.refDropOwnerId = q.front().dropOwnerId; pkt.refDropId = q.front().dropId;
                    q.pop_front();
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
                if (dumpWd) { char b[220]; _snprintf(b, sizeof(b) - 1,
                    "[wd] PICKUP id=%u sid='%s' owner=%u,%u,%u,%u,%u ref=%u/%u prev=%d now=%d trackedLeft=%u",
                    pkt.pickupId, pkt.stringID, it->t, it->c, it->cs, it->i, it->s,
                    pkt.refDropOwnerId, pkt.refDropId, prevCount, ce->second.count, (unsigned)q.size());
                    b[sizeof(b) - 1] = '\0'; coop::logLine(b); }
            }
        }
        // Walk every previously-held weapon kind; a count DECREASE is a candidate drop.
        for (std::map<std::string, WCensusItem>::iterator pe = prevC.items.begin();
             pe != prevC.items.end(); ++pe) {
            std::map<std::string, WCensusItem>::iterator ce = cur.find(pe->first);
            int now = (ce != cur.end()) ? ce->second.count : 0;
            int delta = pe->second.count - now;
            if (delta <= 0) {
                prevC.retries.erase(pe->first); prevC.retryArmMs.erase(pe->first);
                continue;
            }
            // Protocol 37: a pending/applied cross-owner gear transfer must not be
            // read as a ground DROP of the same sid (the count edge is the trade;
            // the baseline update below absorbs it silently).
            if (wdSuppressed(*it, pe->first.c_str(), nowMs())) {
                prevC.retries.erase(pe->first); prevC.retryArmMs.erase(pe->first);
                if (dumpWd) { char b[160]; _snprintf(b, sizeof(b) - 1,
                    "[wd] decrease-suppressed (xfer) sid='%s' delta=%d", pe->first.c_str(), delta);
                    b[sizeof(b) - 1] = '\0'; coop::logLine(b); }
                continue;
            }
            float pos[3] = { 0, 0, 0 };
            unsigned int gtype = pe->second.itemType;
            bool onGround = engine::firstFreeGroundItemPos(gw, cHand, pe->first.c_str(),
                                                           gtype, GROUND_R, pos) != 0;
            if (!onGround) {
                // The engine's spatial item query couldn't locate a free ground copy. This is
                // common in towns and for equipped-then-dropped weapons (the query returns
                // nothing even at a wide radius - see diagGroundScan). Debounce a few ticks to
                // shrug off a 1-frame equip/swap transient WITHOUT committing the lower count.
                int& r = prevC.retries[pe->first];
                if (r == 0) {
                    r = MAX_RETRY;
                    prevC.retryArmMs[pe->first] = nowMs(); // start the absolute hold ceiling
                    if (dumpWd) engine::diagGroundScan(gw, cHand, pe->first.c_str(), GROUND_R);
                }
                // Protocol 37: while the transfer detector is still watching an
                // unresolved LOSS of this sid from this container, the count edge may
                // be a bag-to-bag trade mid-detection - keep holding rather than
                // committing the drop-fallback (the detector either fires the intent,
                // which registers a suppression, or folds the diff and releases us).
                if (r <= 1 && xferPendingLoss(*it, pe->first.c_str())) r = MAX_RETRY;
                if (--r > 0) {
                    if (dumpWd) { char b[160]; _snprintf(b, sizeof(b) - 1,
                        "[wd] decrease-pending hand=%u,%u,%u,%u,%u sid='%s' prev=%d now=%d retry=%d",
                        it->t, it->c, it->cs, it->i, it->s, pe->first.c_str(),
                        pe->second.count, now, r); b[sizeof(b) - 1] = '\0'; coop::logLine(b); }
                    cur[pe->first] = pe->second;        // hold the old count so we re-check next tick
                    curPtrs[pe->first] = prevC.ptrs[pe->first]; // and keep the departed Item* handle(s)
                    continue;
                }
                // Debounce expired: the weapon really LEFT this owned character (we never mutate
                // owned inventories ourselves, so this is a genuine user action). Author the drop
                // at the OWNER's position as the mirror target - the weapon was dropped at its
                // feet, so the peer relocating its own copy there reproduces the drop. (A rare
                // intra-squad trade would be mirrored as a drop here; reconcile then corrects it.)
                prevC.retries.erase(pe->first); prevC.retryArmMs.erase(pe->first);
                // ...unless the departed object is still ALIVE and still INSIDE a
                // container. Then it did not hit the ground at all: it was moved into
                // another inventory - dressing a corpse, stuffing a chest. Authoring a
                // drop for that makes the peer relocate (or, failing that, FABRICATE) a
                // second copy on the ground beside the very container it was just put
                // into, so the item ends up worn AND lying next to the body. That is the
                // reported duplicate, and the author's own bookkeeping calls it out one
                // prune later: "everLive=0: not a free ground item" (session 00:36-00:46,
                // four clothing drops, none of which ever read as a ground object).
                //
                // Only a POSITIVE read suppresses. A dead or unreadable handle keeps the
                // old behaviour, because "the engine streamed the object out" is exactly
                // the case this fallback was written for.
                {
                    std::deque<void*>& dq = prevC.ptrs[pe->first];
                    bool inBag = false;
                    if (!dq.empty() && dq.front()) {
                        bool picked = false;
                        if (engine::groundObjectLiveness(
                                reinterpret_cast<RootObject*>(dq.front()), 0, &picked) == 0 &&
                            picked)
                            inBag = true;
                    }
                    if (inBag) {
                        char b[216]; _snprintf(b, sizeof(b) - 1,
                            "[wd] decrease-moved hand=%u,%u,%u,%u,%u sid='%s' delta=%d "
                            "(still in a container, never on the ground; no drop authored)",
                            it->t, it->c, it->cs, it->i, it->s, pe->first.c_str(), delta);
                        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
                        continue;
                    }
                }
                if (!engine::objectWorldPos(cHand, pos)) {
                    if (dumpWd) { char b[160]; _snprintf(b, sizeof(b) - 1,
                        "[wd] decrease-nopos hand=%u,%u,%u,%u,%u sid='%s' (owner pos unresolved; skip)",
                        it->t, it->c, it->cs, it->i, it->s, pe->first.c_str());
                        b[sizeof(b) - 1] = '\0'; coop::logLine(b); }
                    continue;
                }
                if (dumpWd) { char b[200]; _snprintf(b, sizeof(b) - 1,
                    "[wd] drop-fallback hand=%u,%u,%u,%u,%u sid='%s' (no ground copy; owner pos=%.1f,%.1f,%.1f)",
                    it->t, it->c, it->cs, it->i, it->s, pe->first.c_str(), pos[0], pos[1], pos[2]);
                    b[sizeof(b) - 1] = '\0'; coop::logLine(b); }
            } else {
                prevC.retries.erase(pe->first); prevC.retryArmMs.erase(pe->first);
            }
            // The departed weapon's REAL Item* is the prior tick's handle for this sid; after a
            // UI drop it persists as the now-grounded object (conservation). Remember it so a
            // later PICKUP intent re-homes this exact object without a spatial re-query.
            std::deque<void*>& departed = prevC.ptrs[pe->first];
            for (int d = 0; d < delta; ++d) {
                void* di = departed.empty() ? 0 : departed.front();
                // Prefer the REAL dropped object's position (the exact cursor-drop spot) over
                // the owner-feet fallback - and it's query-free, so both clients agree. But a
                // town-dropped item frequently reports its transform as (0,0,0) the frame it
                // grounds; that sentinel must NOT clobber the good owner-feet fallback (else the
                // peer relocates its copy to world origin and it's invisible near the player).
                float dpos[3] = { pos[0], pos[1], pos[2] };
                if (di) { float ip[3]; if (engine::itemWorldPos(di, ip) &&
                          !(ip[0] == 0.0f && ip[1] == 0.0f && ip[2] == 0.0f)) {
                    dpos[0] = ip[0]; dpos[1] = ip[1]; dpos[2] = ip[2]; } }
                WorldDropPacket pkt; memset(&pkt, 0, sizeof(pkt));
                pkt.type = (u8)PKT_WORLD_DROP; pkt.ownerId = ownerId; pkt.dropId = nextDropId_++;
                pkt.oType = it->t; pkt.oContainer = it->c; pkt.oContainerSerial = it->cs;
                pkt.oIndex = it->i; pkt.oSerial = it->s;
                strncpy(pkt.stringID, pe->first.c_str(), sizeof(pkt.stringID) - 1);
                pkt.itemType = gtype; pkt.quality = pe->second.quality;
                strncpy(pkt.manufacturer, pe->second.manufacturer, sizeof(pkt.manufacturer) - 1);
                strncpy(pkt.material,     pe->second.material,     sizeof(pkt.material) - 1);
                pkt.x = dpos[0]; pkt.y = dpos[1]; pkt.z = dpos[2];
                net.queueWorldDrop(pkt);
                if (di) {
                    trackGroundGear(pe->first, ownerId, pkt.dropId, di, /*authored*/ true);
                    departed.pop_front();
                }
                char b[220]; _snprintf(b, sizeof(b) - 1,
                    "[wd] DROP id=%u sid='%s' owner=%u,%u,%u,%u,%u pos=%.2f,%.2f,%.2f tracked=%u",
                    pkt.dropId, pkt.stringID, it->t, it->c, it->cs, it->i, it->s,
                    pkt.x, pkt.y, pkt.z, (unsigned)groundedWeapons_[pe->first].size());
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
        }
        prevC.items = cur;
        prevC.ptrs  = curPtrs;
    }
}

void Replicator::applyWeaponDrops(GameWorld* gw, Inbound& in) {
    std::deque<InboundWorldDrop> got;
    in.drainWorldDrops(got);
    if (got.empty()) return;
    for (std::deque<InboundWorldDrop>::iterator it = got.begin(); it != got.end(); ++it) {
        const WorldDropPacket& p = it->pkt;
        std::pair<u32, u32> id(p.ownerId, p.dropId);
        if (appliedDrops_.count(id) != 0) continue; // idempotent (reliable resend / replay)
        appliedDrops_.insert(id);
        // Bounded (step 6): ids are per-sender monotonic, so evicting the smallest
        // discards the oldest - far outside any plausible reliable-channel replay.
        if (appliedDrops_.size() > 4096) appliedDrops_.erase(appliedDrops_.begin());
        Key ok; ok.t = p.oType; ok.c = p.oContainer; ok.cs = p.oContainerSerial;
        ok.i = p.oIndex; ok.s = p.oSerial;
        if (ownHands_.count(ok) != 0) continue;     // we own this char -> we dropped it locally
        unsigned int ownerHand[5] = { p.oType, p.oContainer, p.oContainerSerial,
                                      p.oIndex, p.oSerial };
        void* dropped = 0;
        int moved = engine::relocateWeaponToGround(gw, ownerHand, p.stringID, p.itemType,
                                                   p.x, p.y, p.z, &dropped);
        // BACKSTOP: no local copy to relocate. If the owner container does not resolve here
        // at all, the bag simply is not loaded yet - do nothing (fabricating would mint into
        // a container we cannot see, and the sender's resend will retry). But if it DOES
        // resolve, then this client genuinely lacks the item, which historically meant the
        // drop was silently lost (moved=0 and the weapon existed only on the dropper's
        // ground). Rebuild it from the intent's own provenance and drop that.
        // A worn CONTAINER (backpack) is exempt: fabricating one mints an EMPTY bag, so a
        // "heal" would trade a missing backpack for a silently emptied one PLUS a duplicate
        // once the real object turns up. Only relocation preserves the nested contents, so a
        // container that cannot be relocated is reported LOST rather than rebuilt.
        bool healed = false;
        if (moved == 0 && !engine::isContainerItemType(p.itemType) &&
            engine::resolveObjectByHand(ownerHand) != 0) {
            moved = engine::fabricateWeaponToGround(gw, ownerHand, p.stringID, p.itemType,
                                                    (int)p.quality, p.manufacturer, p.material,
                                                    p.x, p.y, p.z, &dropped);
            healed = (moved > 0);
        }
        // Track the relocated REAL object under the drop's SHARED identity so a later PICKUP
        // intent naming (ownerId, dropId) re-homes this exact handle back into the owner's bag
        // (no spatial re-query, which fails in towns; no FIFO-by-sid guess between duplicates).
        if (moved > 0 && dropped)
            trackGroundGear(std::string(p.stringID), p.ownerId, p.dropId, dropped,
                            /*authored*/ false);
        char b[240]; _snprintf(b, sizeof(b) - 1,
            "[wd] APPLY id=%u sid='%s' owner=%u,%u,%u,%u,%u moved=%d pos=%.2f,%.2f,%.2f tracked=%u",
            p.dropId, p.stringID, p.oType, p.oContainer, p.oContainerSerial, p.oIndex,
            p.oSerial, moved, p.x, p.y, p.z,
            (unsigned)groundedWeapons_[std::string(p.stringID)].size());
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        // Distinguish "conserved" from "rebuilt" and from "still lost": a HEAL means the
        // race beat the publish hold, and a LOST means even fabrication failed.
        if (healed) {
            char h[200]; _snprintf(h, sizeof(h) - 1,
                "[wd] APPLY-HEALED id=%u sid='%s' (no local copy; rebuilt from intent provenance)",
                p.dropId, p.stringID);
            h[sizeof(h) - 1] = '\0'; coop::logLine(h);
        } else if (moved == 0) {
            char h[200]; _snprintf(h, sizeof(h) - 1,
                "[wd] APPLY-LOST id=%u sid='%s' (no local copy and no rebuild; drop not mirrored)",
                p.dropId, p.stringID);
            h[sizeof(h) - 1] = '\0'; coop::logLine(h);
        }
    }
}

// TEST-ONLY fault injection: KENSHICOOP_WD_FORGET_TRACK=1 makes the DROP AUTHOR discard its
// ground track the moment it is created, reproducing the state the player's session was
// actually in - a real, identified pickup arrives and the author has no handle for the object.
// Without this lever there is no deterministic way to gate the site-anchored recovery, because
// losing a track in the wild depends on the engine streaming an object out from under us.
//
// Only the AUTHORED side forgets, and that asymmetry is the whole point: the PICKER needs its
// own track to put an identity on the intent at all. Forgetting on both sides instead
// reproduced a different bug - the picker could name nothing, suppressed the intent as
// increase-untracked, and the author was never even asked.
static bool injectForgetTrack(bool authored) {
    static int on = -1;
    if (on < 0) { const char* e = getenv("KENSHICOOP_WD_FORGET_TRACK"); on = (e && e[0] == '1') ? 1 : 0; }
    return on == 1 && authored;
}

void Replicator::parkPendingPickup(const unsigned int targetHand[5], const char* sid,
                                   unsigned int itemType, u32 refOwnerId, u32 refDropId) {
    // Idempotent: a reliable resend of the same intent must not stack retries.
    for (std::deque<PendingPickup>::iterator it = pendingPickups_.begin();
         it != pendingPickups_.end(); ++it)
        if (it->refOwnerId == refOwnerId && it->refDropId == refDropId) return;
    if (pendingPickups_.size() >= WD_PENDING_PICKUPS_MAX) pendingPickups_.pop_front();
    PendingPickup pp;
    for (int k = 0; k < 5; ++k) pp.targetHand[k] = targetHand[k];
    pp.sid = sid ? sid : ""; pp.itemType = itemType;
    pp.refOwnerId = refOwnerId; pp.refDropId = refDropId; pp.sinceMs = nowMs();
    pendingPickups_.push_back(pp);
}

void Replicator::trackGroundGear(const std::string& sid, u32 dropOwnerId, u32 dropId,
                                 void* item, bool authored) {
    if (injectForgetTrack(authored)) {
        char b[160]; _snprintf(b, sizeof(b) - 1,
            "[wd] TRACK-FORGET-INJECTED sid='%s' drop=%u/%u (test lever)",
            sid.c_str(), dropOwnerId, dropId);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        return;
    }
    GroundWeapon g;
    g.dropOwnerId = dropOwnerId; g.dropId = dropId; g.item = item;
    for (int k = 0; k < 5; ++k) { g.hand[k] = 0; g.pendingHand[k] = 0; }
    g.pendingType = 0; g.pendingSinceMs = 0; g.deadReads = 0;
    g.firstDeadMs = 0; g.createdMs = nowMs(); g.everLive = false;
    // The object's save-stable hand, so the re-home can re-resolve it. A runtime-minted
    // item may have no useful hand; the raw pointer stays as the fallback.
    if (item) engine::readObjectHand(reinterpret_cast<RootObject*>(item), g.hand);
    groundedWeapons_[sid].push_back(g);
}

namespace {
// Does the container at cHand hold (sid, type)? Tri-state on purpose: a container that
// yields NO rows is UNREADABLE (mid-resolve), which must never be read as "does not hold
// it" - that is the misreading that would license destroying the last copy.
//   1 = holds it, 0 = readable and does NOT hold it, -1 = unreadable.
int containerHoldsItemState(GameWorld* gw, const unsigned int cHand[5], const char* sid,
                            unsigned int typeCat) {
    if (!sid || !sid[0]) return -1;
    InvItemEntry items[INV_ITEMS_MAX];
    unsigned int n = engine::captureContainerContents(gw, cHand, items, INV_ITEMS_MAX, 0);
    if (n == 0) return -1;
    for (unsigned int i = 0; i < n; ++i) {
        if (typeCat != 0 && items[i].itemType != typeCat) continue;
        if (strcmp(items[i].stringID, sid) == 0) return 1;
    }
    return 0;
}

// TEST-ONLY fault injection. A live re-home refusal depends on the state of a bag we do not
// control (an occupied equip slot, no room), so these levers are the only way to gate the
// recovery deterministically - and the absence of that recovery is precisely what turned a
// refusal into a permanent duplicate.
//   KENSHICOOP_WD_REFUSE_REHOME=1     refuse ONCE   -> exercises the retry
//   KENSHICOOP_WD_REFUSE_REHOME_ALL=1 refuse ALWAYS -> exercises verify-then-destroy, the
//                                     branch that retires our ground copy against the bag
//                                     we can actually read
// Consulted by BOTH re-home attempts (the pickup apply and the reconcile retry), or "refuse
// always" would be quietly satisfied by the retry.
bool injectRehomeRefusal() {
    static int mode = -1; // 0 = off, 1 = first only, 2 = always
    if (mode < 0) {
        const char* all = getenv("KENSHICOOP_WD_REFUSE_REHOME_ALL");
        const char* one = getenv("KENSHICOOP_WD_REFUSE_REHOME");
        mode = (all && all[0] == '1') ? 2 : ((one && one[0] == '1') ? 1 : 0);
    }
    if (mode == 0) return false;
    if (mode == 1) mode = 0;
    coop::logLine("[wd] REHOME-REFUSE-INJECTED (test lever: re-home refused)");
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
static bool injectTransientDead() {
    static int budget = -1;
    if (budget < 0) {
        const char* e = getenv("KENSHICOOP_WD_TRANSIENT_DEAD");
        budget = e ? atoi(e) : 0;
        if (budget < 0) budget = 0;
    }
    if (budget == 0) return false;
    --budget;
    coop::logLine("[wd] TRANSIENT-DEAD-INJECTED (test lever: this read reports the object gone)");
    return true;
}

// Resolve a tracked ground item to a LIVE free ground object, or 0 if it is no longer one.
// The CACHED pointer is tried first and on purpose: it is the object we actually relocated,
// and the liveness probe is SEH-guarded so a dangling pointer simply reads as dead. Trusting
// the HAND first was wrong - a ground item's hand does not necessarily resolve back to that
// item (runtime-minted items have host-only hands), and whatever it did resolve to was then
// read through an Item* cast, so an unrelated object's bytes decided liveness. That is what
// made the author discard live ground tracks and answer a real pickup with "untracked",
// leaving its copy on the ground: the duplicate the player saw.
RootObject* resolveGroundGear(const unsigned int hand[5], void* cached) {
    bool pickedUp = false;
    RootObject* ro = reinterpret_cast<RootObject*>(cached);
    if (ro && engine::groundObjectLiveness(ro, 0, &pickedUp)) return ro;
    if (pickedUp) return 0; // it really has entered a bag - not ours to re-home any more
    // The cached pointer is dead or unreadable (the engine streamed the object out and back,
    // say). The hand is the only other handle we have, so try it - but only now.
    bool haveHand = false;
    for (int k = 0; k < 5; ++k) if (hand[k] != 0) { haveHand = true; break; }
    if (!haveHand) return 0;
    RootObject* byHand = engine::resolveObjectByHand(hand);
    if (!byHand || byHand == ro) return 0;
    if (!engine::groundObjectLiveness(byHand, 0, &pickedUp)) return 0;
    return byHand;
}
} // namespace

void Replicator::retryPendingPickups(GameWorld* gw) {
    if (pendingPickups_.empty()) return;
    unsigned long now = nowMs();
    for (std::deque<PendingPickup>::iterator it = pendingPickups_.begin();
         it != pendingPickups_.end(); ) {
        int moved = 0;
        const char* how = "site";
        // Prefer the named instance if its track resolves again - that re-homes the EXACT object
        // the picker took, where the site scan can only match template + proximity.
        std::map<std::string, std::deque<GroundWeapon> >::iterator sit =
            groundedWeapons_.find(it->sid);
        if (sit != groundedWeapons_.end()) {
            for (std::deque<GroundWeapon>::iterator g = sit->second.begin();
                 g != sit->second.end(); ++g) {
                if (g->dropOwnerId != it->refOwnerId || g->dropId != it->refDropId) continue;
                RootObject* ro = resolveGroundGear(g->hand, g->item);
                if (!ro) break;
                moved = injectRehomeRefusal()
                            ? 0 : engine::addItemPtrToInventory(gw, it->targetHand, ro);
                if (moved) { how = "track"; sit->second.erase(g); }
                break;
            }
        }
        if (!moved && !injectRehomeRefusal())
            moved = engine::pickupWorldItemIntoInventory(gw, it->targetHand, it->sid.c_str(),
                                                         it->itemType, WD_REHOME_SCAN_R);
        if (moved) {
            char b[220]; _snprintf(b, sizeof(b) - 1,
                "[wd] PICKUP-RETRY-OK sid='%s' ref=%u/%u via=%s afterMs=%lu",
                it->sid.c_str(), it->refOwnerId, it->refDropId, how,
                (unsigned long)(now - it->sinceMs));
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            it = pendingPickups_.erase(it);
            continue;
        }
        if (now - it->sinceMs < WD_REHOME_MAX_MS) { ++it; continue; }
        // Out of patience. Unconditional, because this is the state the player SEES: the peer
        // holds the item and we still have a copy somewhere we could not reach.
        char b[240]; _snprintf(b, sizeof(b) - 1,
            "[wd] PICKUP-GAVEUP sid='%s' ref=%u/%u afterMs=%lu (peer took it; our copy could not "
            "be re-homed - expect a duplicate)", it->sid.c_str(), it->refOwnerId, it->refDropId,
            (unsigned long)(now - it->sinceMs));
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        it = pendingPickups_.erase(it);
    }
}

void Replicator::reconcileGroundGear(GameWorld* gw) {
    // Ordered FIRST and outside the groundedWeapons_ guard below: a parked pickup exists
    // precisely because there is no track for it, so gating it on a non-empty track map would
    // never retry the case it was written for.
    retryPendingPickups(gw);
    if (groundedWeapons_.empty()) return;
    static int dumpWd = -1;
    if (dumpWd < 0) { const char* e = getenv("KENSHICOOP_INV_DUMP"); dumpWd = (e && e[0] == '1') ? 1 : 0; }
    unsigned long now = nowMs();
    for (std::map<std::string, std::deque<GroundWeapon> >::iterator sit = groundedWeapons_.begin();
         sit != groundedWeapons_.end(); ++sit) {
        std::deque<GroundWeapon>& q = sit->second;
        for (std::deque<GroundWeapon>::iterator g = q.begin(); g != q.end(); ) {
            RootObject* ro = resolveGroundGear(g->hand, g->item);
            if (!ro) {
                // Not reading as a free ground item. Do NOT conclude anything yet - a single
                // bad read is exactly what used to make us forget a live ground copy and
                // answer a real pickup with "untracked". Only a REPEATED failure retires the
                // track (nothing is on the ground then, so there is no duplicate to resolve).
                if (++g->deadReads == 1) g->firstDeadMs = now;
                // The read count is denominated in ENGINE TICKS (~100-125 Hz), so on its own it
                // retires a track ~25 ms after the drop that created it. The streak has to
                // survive real time too, or a momentary unreadable frame costs the author its
                // only handle on the object and the peer's pickup is answered with nothing.
                unsigned long deadFor = now - g->firstDeadMs;
                unsigned long hold = g->everLive ? WD_DEAD_HOLD_MS : WD_NEVER_LIVE_MAX_MS;
                if (g->deadReads < WD_DEAD_READS_MAX || deadFor < hold) { ++g; continue; }
                char b[240]; _snprintf(b, sizeof(b) - 1,
                    "[wd] ground-prune sid='%s' drop=%u/%u (%u consecutive reads over %lums, "
                    "everLive=%d: not a free ground item)", sit->first.c_str(), g->dropOwnerId,
                    g->dropId, g->deadReads, deadFor, g->everLive ? 1 : 0);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
                g = q.erase(g);
                continue;
            }
            g->deadReads = 0; g->firstDeadMs = 0; g->everLive = true;
            bool pending = false;
            for (int k = 0; k < 5; ++k) if (g->pendingHand[k] != 0) { pending = true; break; }
            if (!pending) { ++g; continue; }
            // A peer took this item but our own bag refused the object. Retry: the refusal is
            // often transient (the container was still resolving, or a reconcile has since
            // freed the equip slot).
            int moved = injectRehomeRefusal() ? 0
                                             : engine::addItemPtrToInventory(gw, g->pendingHand, ro);
            if (moved) {
                char b[200]; _snprintf(b, sizeof(b) - 1,
                    "[wd] REHOME-RETRY-OK sid='%s' drop=%u/%u afterMs=%lu",
                    sit->first.c_str(), g->dropOwnerId, g->dropId,
                    (unsigned long)(now - g->pendingSinceMs));
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
                g = q.erase(g);
                continue;
            }
            if (now - g->pendingSinceMs < WD_REHOME_MAX_MS) { ++g; continue; }
            // Out of patience. The peer holds the item and we still have it on the ground:
            // that is a DUPLICATE - but only once we can SEE our own copy in the target
            // container may we destroy the ground one. Until then, keeping a duplicate beats
            // destroying the only instance (the reconcile cannot rebuild a truncated read,
            // a weapon with fabrication disabled, or a backpack at all).
            int inBag = containerHoldsItemState(gw, g->pendingHand, sit->first.c_str(),
                                                g->pendingType);
            if (inBag == 1) {
                bool destroyed = engine::removeWorldItemProxy(gw, ro);
                char b[220]; _snprintf(b, sizeof(b) - 1,
                    "[wd] REHOME-DEDUPE sid='%s' drop=%u/%u destroyed=%d (bag already holds it)",
                    sit->first.c_str(), g->dropOwnerId, g->dropId, destroyed ? 1 : 0);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
                if (destroyed) { g = q.erase(g); continue; }
            } else if (inBag == -1) {
                // Target unreadable (not loaded / mid-resolve). Neither conclusion is safe,
                // so extend the window rather than decide on a blind read.
                g->pendingSinceMs = now;
                if (dumpWd) { char b[200]; _snprintf(b, sizeof(b) - 1,
                    "[wd] REHOME-WAIT sid='%s' drop=%u/%u (target container unreadable)",
                    sit->first.c_str(), g->dropOwnerId, g->dropId);
                    b[sizeof(b) - 1] = '\0'; coop::logLine(b); }
                ++g;
                continue;
            } else {
                char b[240]; _snprintf(b, sizeof(b) - 1,
                    "[wd] REHOME-DUPE-RISK sid='%s' drop=%u/%u (bag refused it and does NOT "
                    "hold it; leaving our ground copy rather than losing the item)",
                    sit->first.c_str(), g->dropOwnerId, g->dropId);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
            // Stop retrying; the track stays so the item is never orphaned.
            for (int k = 0; k < 5; ++k) g->pendingHand[k] = 0;
            g->pendingSinceMs = 0;
            ++g;
        }
    }
}

void Replicator::applyWeaponPickups(GameWorld* gw, Inbound& in) {
    std::deque<InboundWorldPickup> got;
    in.drainWorldPickups(got);
    if (got.empty()) return;
    for (std::deque<InboundWorldPickup>::iterator it = got.begin(); it != got.end(); ++it) {
        const WorldPickupPacket& p = it->pkt;
        std::pair<u32, u32> id(p.ownerId, p.pickupId);
        if (appliedPickups_.count(id) != 0) continue; // idempotent (reliable resend / replay)
        appliedPickups_.insert(id);
        if (appliedPickups_.size() > 4096) appliedPickups_.erase(appliedPickups_.begin());
        Key ok; ok.t = p.oType; ok.c = p.oContainer; ok.cs = p.oContainerSerial;
        ok.i = p.oIndex; ok.s = p.oSerial;
        if (ownHands_.count(ok) != 0) continue;        // we own this char -> we picked it up locally
        unsigned int targetHand[5] = { p.oType, p.oContainer, p.oContainerSerial,
                                       p.oIndex, p.oSerial };
        std::deque<GroundWeapon>& q = groundedWeapons_[std::string(p.stringID)];
        int moved = 0;
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
            RootObject* ro = injectTransientDead() ? 0
                                                   : resolveGroundGear(pick->hand, pick->item);
            if (!ro) {
                // One unreadable read is NOT proof the object left the ground - the engine
                // streams objects out and back, and reconcileGroundGear deliberately needs a
                // sustained streak before it believes the same thing. Erasing here on a single
                // read threw away the handle AND the intent together, leaving our ground copy
                // with nothing left to retire it. Park the intent and keep trying.
                why = "deferred";
                parkPendingPickup(targetHand, p.stringID, p.itemType,
                                  p.refDropOwnerId, p.refDropId);
            } else {
                moved = injectRehomeRefusal() ? 0
                                             : engine::addItemPtrToInventory(gw, targetHand, ro);
                if (moved) {
                    q.erase(pick); // re-homed; stop tracking it on the ground
                } else {
                    // The object is alive but our bag refused it (occupied equip slot, no
                    // room, container still resolving). Leaving it here is what showed up as
                    // an item duplicated on the author's ground. Arm the bounded retry;
                    // reconcileGroundGear finishes the job - and only ever destroys our
                    // ground copy once it has read the item in the target bag.
                    why = "refused";
                    for (int k = 0; k < 5; ++k) pick->pendingHand[k] = targetHand[k];
                    pick->pendingType    = p.itemType;
                    pick->pendingSinceMs = nowMs();
                }
            }
        } else if (p.refDropId != 0) {
            // The picker NAMED a drop we no longer have tracked. The intent is trustworthy -
            // a real object really did leave the ground over there - so answering "untracked"
            // and doing nothing is not neutral: our copy stays on the ground and the item is
            // duplicated for the rest of the session. That is what the player hit picking up
            // boots on the join. Since the identity is real but our handle for it is not, fall
            // back to WHERE rather than WHICH: relocate a free ground item of the same
            // (sid,type) lying near the picking character. Unlike the FIFO guess this replaces,
            // it is anchored to the pickup site, and it can only ever fire for an identified
            // intent - an identity-less one still gets nothing.
            moved = injectRehomeRefusal()
                        ? 0
                        : engine::pickupWorldItemIntoInventory(gw, targetHand, p.stringID,
                                                               p.itemType, WD_REHOME_SCAN_R);
            // A miss here is not a verdict either: this scan is the spatial query that fails in
            // towns, so answering "untracked" and stopping is how a town pickup left a permanent
            // duplicate. Park it and retry - the track may also come back before the deadline.
            // A miss here is not a verdict either: this scan is the spatial query that fails in
            // towns, so answering "untracked" and stopping is how a town pickup left a permanent
            // duplicate. Park it and retry - the track may also come back before the deadline.
            if (moved) {
                why = "recovered-by-site";
            } else {
                why = "deferred";
                parkPendingPickup(targetHand, p.stringID, p.itemType,
                                  p.refDropOwnerId, p.refDropId);
            }
        } else {
            // NO FALLBACK for an identity-less pickup. Re-homing "the oldest same-sid copy" on
            // a ref=0/0 intent means a bogus pickup TELEPORTS an unrelated ground item into a
            // bag - the census used to emit a burst of exactly those at connect. Without an
            // instance identity we cannot know which object was taken, so we do nothing: the
            // peer's own snapshot still converges its bag, and our ground copy is left alone.
            why = "no-identity";
        }
        char b[260]; _snprintf(b, sizeof(b) - 1,
            "[wd] PICKUP-APPLY id=%u sid='%s' owner=%u,%u,%u,%u,%u ref=%u/%u moved=%d why=%s trackedLeft=%u",
            p.pickupId, p.stringID, p.oType, p.oContainer, p.oContainerSerial, p.oIndex,
            p.oSerial, p.refDropOwnerId, p.refDropId, moved, why, (unsigned)q.size());
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        // Keep the transfer detector blind to the relocation we just made.
    }
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
    std::map<Key, WCensus>::const_iterator it = weaponCensus_.find(k);
    if (it == weaponCensus_.end()) return false;
    unsigned long now = nowMs();
    // A non-zero retry budget means the census saw gear LEAVE this container and is still
    // hunting the ground copy that would let it author the drop intent.
    //
    // But the budget is FRAME-denominated and xferPendingLoss re-arms it, so it can cycle
    // indefinitely while the transfer detector holds an unresolved loss. The drop verdict can
    // afford to wait; the inventory snapshot cannot, because this predicate gates the whole
    // container. Honour the hold only within WD_HOLD_MAX_MS of the FIRST arm - past that we
    // publish and accept the (recoverable, and heal-backstopped) race rather than starving
    // the peer's view of the bag. Observed unbounded: 280 consecutive holds on one container.
    for (std::map<std::string, int>::const_iterator r = it->second.retries.begin();
         r != it->second.retries.end(); ++r) {
        if (r->second <= 0) continue;
        std::map<std::string, unsigned long>::const_iterator a =
            it->second.retryArmMs.find(r->first);
        if (a == it->second.retryArmMs.end()) return true; // armed without a stamp: honour it
        if (now - a->second < WD_HOLD_MAX_MS) return true;
    }
    return false;
}

void Replicator::detectAndPublishTransfers(GameWorld* gw, NetLink& net, u32 ownerId) {
    const unsigned long XFER_GRACE_MS = 10000;
    unsigned long now = nowMs();

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
        if (f.qty <= 0) { engine::finishTransferredBag(edges[i].transferredItem); continue; }
        bool srcOwn = ownedContainers_.count(f.src) != 0 || ownHands_.count(f.src) != 0
                   || censusContainers_.count(f.src) != 0 ||
                      (isHostRole() && ownerClassForHand(edges[i].src) == 0);
        bool dstOwn = ownedContainers_.count(f.dst) != 0 || ownHands_.count(f.dst) != 0
                   || censusContainers_.count(f.dst) != 0 ||
                      (isHostRole() && ownerClassForHand(edges[i].dst) == 0);
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
            net.queueInvXfer(pkt, contents);
            // Latch the pending move on each PEER end so applyInventories cannot
            // reconcile it back while the owner's snapshots are still stale.
            if (!srcOwn) {
                XferLatch& L = xferLatch_[f.src][f.key];
                L.delta -= f.qty; L.deadlineMs = now + XFER_GRACE_MS;
                if (L.delta == 0) xferLatch_[f.src].erase(f.key);
            }
            if (!dstOwn) {
                XferLatch& L = xferLatch_[f.dst][f.key];
                L.delta += f.qty; L.deadlineMs = now + XFER_GRACE_MS;
                if (L.delta == 0) xferLatch_[f.dst].erase(f.key);
            }
            // A gear trade must not be read by the W2 weapon census as a ground
            // drop (src) / pickup (dst) of the same sid.
            if (isGearType(f.key.second)) {
                wdSuppress_[std::make_pair(f.src, f.key.first)] = now + XFER_GRACE_MS;
                wdSuppress_[std::make_pair(f.dst, f.key.first)] = now + XFER_GRACE_MS;
            }
            // Own inventory publication stays pinned until the receiver decides.
            XferOut o;
            o.src = f.src; o.dst = f.dst; o.key = f.key; o.qty = f.qty;
            o.srcPeer = !srcOwn; o.dstPeer = !dstOwn; o.sentMs = now;
            std::map<Key, InvRecv>::const_iterator authority =
                invRecv_.find(!srcOwn ? f.src : f.dst);
            o.peerId = authority != invRecv_.end() ? authority->second.ownerId : 0;
            o.rollbackQty = 0; o.answered = false;
            o.bagItem = engine::isContainerItemType(pkt.itemType) ? edges[i].transferredItem : 0;
            o.hasCanonicalBag = false;
            xferOut_[pkt.xferId] = o;
            char b[240]; _snprintf(b, sizeof(b) - 1,
                "[xfer] SEND id=%u sid='%s' type=%u qty=%d src=%u,%u,%u,%u,%u(%s) dst=%u,%u,%u,%u,%u(%s)",
                pkt.xferId, pkt.stringID, pkt.itemType, f.qty,
                f.src.t, f.src.c, f.src.cs, f.src.i, f.src.s, srcOwn ? "own" : "peer",
                f.dst.t, f.dst.c, f.dst.cs, f.dst.i, f.dst.s, dstOwn ? "own" : "peer");
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        } else engine::finishTransferredBag(edges[i].transferredItem);
    }
}

void Replicator::applyTransfers(GameWorld* gw, Inbound& in, NetLink& net, u32 localId) {
    std::deque<InboundInvXfer> got;
    in.drainInvXfers(got);
    typedef std::pair<u32, u32> TransferId;
    for (std::deque<InboundInvXfer>::iterator it = got.begin(); it != got.end(); ++it) {
        const InvXferPacket& p = it->pkt;
        if (p.ownerId == localId) continue;
        TransferId id(p.ownerId, p.xferId);
        std::map<TransferId, InboundInvXferAck>::iterator done = appliedXfers_.find(id);
        if (done != appliedXfers_.end()) {
            if (done->second.pkt.ownerId == localId)
                net.queueInvXferAck(done->second.pkt, done->second.contents);
        } else {
            InboundInvXfer& pending = pendingXfers_[id];
            pending.ownerId = it->ownerId; pending.pkt = p;
            pending.contents.swap(it->contents);
        }
    }
    unsigned long now = nowMs();
    for (std::map<TransferId, InboundInvXfer>::iterator it = pendingXfers_.begin();
         it != pendingXfers_.end();) {
        const InvXferPacket& p = it->second.pkt;
        Key sk; sk.t=p.sType; sk.c=p.sContainer; sk.cs=p.sContainerSerial; sk.i=p.sIndex; sk.s=p.sSerial;
        Key dk; dk.t=p.dType; dk.c=p.dContainer; dk.cs=p.dContainerSerial; dk.i=p.dIndex; dk.s=p.dSerial;
        unsigned int sHand[5], dHand[5];
        // A hand which is still streaming is not a rejection and must not consume the id.
        if (!resolveInvWireHand(sk,sHand) || !resolveInvWireHand(dk,dHand)) { ++it; continue; }
        sk = keyForInvLocalHand(sHand); dk = keyForInvLocalHand(dHand);
        bool srcOwn = ownedContainers_.count(sk) || ownHands_.count(sk) || censusContainers_.count(sk);
        bool dstOwn = ownedContainers_.count(dk) || ownHands_.count(dk) || censusContainers_.count(dk);
        if (isHostRole()) {
            srcOwn = srcOwn || ownerClassForHand(sHand) == 0;
            dstOwn = dstOwn || ownerClassForHand(dHand) == 0;
        }
        // Observers consume the owners' final snapshots, not a third physical mutation.
        if (!srcOwn && !dstOwn) { pendingXfers_.erase(it++); continue; }
        std::vector<InvItemEntry> actualBag;
        bool bag = engine::isContainerItemType(p.itemType);
        // A sender-owned source carries its exact contents at native hand-off.
        // A receiver-owned source supplies its own actual contents in the verdict.
        int moved = (!bag || p.contentsPresent) ? engine::moveItemBetweenContainers(
            gw, sHand, dHand, p.stringID, p.itemType, p.quantity, true,
            bag && !srcOwn ? &it->second.contents : 0,
            bag && srcOwn ? &actualBag : 0) : 0;
        XKey key(std::string(p.stringID),p.itemType);
        if (moved > 0) {
            if (!srcOwn) {
                XferLatch& l=xferLatch_[sk][key]; l.delta-=moved; l.deadlineMs=now+10000;
            }
            if (!dstOwn) {
                XferLatch& l=xferLatch_[dk][key]; l.delta+=moved; l.deadlineMs=now+10000;
            }
        }
        if (isGearType(p.itemType)) {
            wdSuppress_[std::make_pair(sk,key.first)]=now+10000;
            wdSuppress_[std::make_pair(dk,key.first)]=now+10000;
        }
        InvXferAckPacket ack; memset(&ack,0,sizeof(ack));
        ack.type=(u8)PKT_INV_XFER_ACK;
        ack.ownerId=(srcOwn || dstOwn) ? localId : MAX_PLAYERS;
        ack.xferOwnerId=p.ownerId; ack.xferId=p.xferId;
        ack.applied=(u16)moved; ack.requested=p.quantity;
        ack.verdict=(u8)(moved == 0 ? XFER_ACK_REJECT :
                         moved == p.quantity ? XFER_ACK_ACCEPT : XFER_ACK_PARTIAL);
        ack.contentsPresent = bag && srcOwn && moved > 0 ? 1 : 0;
        InboundInvXferAck& result = appliedXfers_[it->first];
        result.ownerId = ack.ownerId; result.pkt = ack;
        if (ack.contentsPresent) result.contents.swap(actualBag);
        if (ack.ownerId == localId) net.queueInvXferAck(ack, result.contents);
        char b[240]; _snprintf(b,sizeof(b)-1,
            "[xfer] APPLY id=%u from=%u sid='%s' type=%u qty=%u moved=%d ack=%u authority=%d",
            p.xferId,p.ownerId,p.stringID,p.itemType,(unsigned)p.quantity,moved,
            (unsigned)ack.verdict,(int)(ack.ownerId == localId));
        b[sizeof(b)-1]='\0'; coop::logLine(b);
        pendingXfers_.erase(it++);
    }
}

void Replicator::applyXferAcks(GameWorld* gw, Inbound& in, u32 localId) {
    std::deque<InboundInvXferAck> got; in.drainInvXferAcks(got);
    for (std::deque<InboundInvXferAck>::iterator it=got.begin();it!=got.end();++it) {
        const InvXferAckPacket& a=it->pkt;
        if (a.xferOwnerId != localId) continue;
        std::map<u32,XferOut>::iterator o=xferOut_.find(a.xferId);
        if (o==xferOut_.end() || o->second.answered || a.ownerId != o->second.peerId) continue;
        XferOut& x=o->second;
        x.answered=true;
        x.rollbackQty=x.qty-(a.applied < x.qty ? a.applied : x.qty);
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
        if (!x.answered) { ++it; continue; }
        if (x.hasCanonicalBag) {
            if (!engine::applyTransferredBagContents(gw, x.bagItem, x.canonicalBag)) { ++it; continue; }
            x.hasCanonicalBag = false;
        }
        if (x.rollbackQty > 0) {
            unsigned int src[5],dst[5];
            if (!resolveInvLocalHand(x.src,src) || !resolveInvLocalHand(x.dst,dst)) { ++it; continue; }
            int undone=engine::moveItemBetweenContainers(gw,dst,src,x.key.first.c_str(),
                                                         x.key.second,x.rollbackQty);
            if (undone > 0) {
                if (x.srcPeer) releaseXferLatch(x.src,x.key,+undone);
                if (x.dstPeer) releaseXferLatch(x.dst,x.key,-undone);
                x.rollbackQty-=undone;
            }
            if (x.rollbackQty > 0) { ++it; continue; }
        }
        // ACCEPT is not a snapshot barrier: keep peer latches until its state catches up.
        engine::finishTransferredBag(x.bagItem);
        xferOut_.erase(it++);
    }
}

// Undo one intent's contribution to a latch. `delta` is the inverse of what
// detectAndPublishTransfers added, so a lone intent lands the latch back on
// zero and it is erased; a second, still-unanswered intent on the same
// container and item leaves its own contribution behind to be cleared by its
// own verdict. The deadline is deliberately untouched: it belongs to whatever
// is left, not to what we just removed.
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
