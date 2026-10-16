#include "EnvironmentItemsSync.h"
#include "../game/EngineProcedural.h"
#include "../net/NetLink.h"
#include "../core/Inbound.h"
#include "../CoopLog.h"
#include <windows.h>
#include <cstdio>
#include <set>
namespace coop {
namespace {
void trace(const char* event,const EnvironmentItemPacket& p) {
    char b[384];_snprintf(b,sizeof(b)-1,
        "[environment] %s owner=%u target=%u world=%I64u load=%u region=%d,%d epoch=%u count=%u digest=%08x",
        event,p.ownerId,p.targetId,p.worldToken,p.loadId,p.regionX,p.regionZ,p.epoch,p.count,p.digest);
    b[sizeof(b)-1]=0;logLine(b);
}
bool sameSet(const environment::Objects& a,const environment::Objects& b) {
    if (a.size()!=b.size()) return false;
    for (environment::Objects::const_iterator it=a.begin();it!=a.end();++it) {
        environment::Objects::const_iterator other=b.find(it->first);
        if (other==b.end() || !environment::same(it->second,other->second)) return false;
    }
    return true;
}
ProceduralPacket scopeControl(const EnvironmentItemPacket& p) {
    ProceduralPacket out;memset(&out,0,sizeof(out));out.type=PKT_PROCEDURAL;
    out.worldToken=p.worldToken;out.loadId=p.loadId;out.regionX=p.regionX;out.regionZ=p.regionZ;out.count=p.count;
    out.operation=p.operation==ENV_REQUEST?PROC_REQUEST:p.operation==ENV_RETIRE?PROC_RETIRE:PROC_SCOPE_COMMIT;
    return out;
}
}
EnvironmentItemsSync::EnvironmentItemsSync() { reset(); }
void EnvironmentItemsSync::reset() {
    regions_.clear();world_=nextId_=0;load_=owner_=lastScan_=0;host_=complete_=committed_=false;
    nativeCensus_.reset();
    memset(peerLoads_,0,sizeof(peerLoads_));
    for (unsigned i=0;i<MAX_PLAYERS;++i) scopes_[i].reset(0,0);
    engine::setEnvironmentJoin(false);
}
void EnvironmentItemsSync::retirePeer(u32 peer) {
    if (peer>=MAX_PLAYERS) return;
    peerLoads_[peer]=0;scopes_[peer].reset(0,0);
    for (std::map<Key,Region>::iterator it=regions_.begin();it!=regions_.end();++it)
        it->second.requested[peer]=it->second.acknowledged[peer]=it->second.requestedScope[peer]=0;
}
EnvironmentItemPacket EnvironmentItemsSync::control(const Key& k,u8 op,u32 epoch,u32 load,u32 target) const {
    EnvironmentItemPacket p;memset(&p,0,sizeof(p));p.type=PKT_ENVIRONMENT_ITEMS;p.operation=op;
    p.ownerId=owner_;p.targetId=target;p.worldToken=world_;p.loadId=load;p.regionX=k.first;p.regionZ=k.second;p.epoch=epoch;
    std::map<Key,Region>::const_iterator region=regions_.find(k);
    if (region!=regions_.end()) p.scopeId=region->second.scopeId;
    return p;
}
bool EnvironmentItemsSync::captureHost(GameWorld* gw,const Key& k,Region& r) {
    r.complete=r.ready=false;r.nativeCensus.reset();
    engine::setEnvironmentRegionReady(k.first,k.second,0);
    std::vector<engine::EnvironmentNativeRow> native;
    if (!engine::captureEnvironmentItems(gw,k.first,k.second,native,&r.nativeCensus)) return false;
    environment::Objects observed;std::map<u64,Hand> bindings;std::set<u64> used;
    for (unsigned i=0;i<native.size();++i) {
        u64 id=0;
        for (std::map<u64,Hand>::const_iterator it=r.bindings.begin();it!=r.bindings.end();++it)
            if (!memcmp(it->second.value,native[i].hand,20)) { id=it->first;break; }
        if (!id) {
            for (environment::Objects::const_iterator it=r.objects.begin();it!=r.objects.end();++it)
                if (!used.count(it->first) && environment::sameNative(it->second,native[i].object)) { id=it->first;break; }
        }
        if (!id) { if (++nextId_==0) { r.complete=r.ready=false;return false; } id=nextId_; }
        if (used.count(id)) { r.complete=r.ready=false;return false; }
        used.insert(id);native[i].object.id=id;observed[id]=native[i].object;
        Hand h;memcpy(h.value,native[i].hand,20);bindings[id]=h;
    }
    if (!r.epoch || !sameSet(r.objects,observed)) {
        if (++r.epoch==0) { r.complete=r.ready=false;return false; }
        r.objects.swap(observed);memset(r.acknowledged,0,sizeof(r.acknowledged));r.lastSend=0;
        EnvironmentItemPacket p=control(k,ENV_END,r.epoch,load_,owner_);p.count=(u32)r.objects.size();p.digest=environment::digest(r.objects);trace("HOST-CATALOG",p);
    }
    r.bindings.swap(bindings);
    r.nativeCensus.finish(true,engine::environmentOriginRevision());
    r.complete=r.ready=r.nativeCensus.current(engine::environmentOriginRevision());
    engine::setEnvironmentRegionReady(k.first,k.second,r.ready?&r.nativeCensus:0);return r.ready;
}
bool EnvironmentItemsSync::send(NetLink& net,const Key& k,Region& r,u32 peer) {
    if (!environment::reconciledCensusCurrent(r.complete,r.ready,r.nativeCensus,engine::environmentOriginRevision())) return false;
    EnvironmentItemPacket p=control(k,ENV_BEGIN,r.epoch,r.requested[peer],peer);
    p.scopeId=r.requestedScope[peer];
    p.count=(u32)r.objects.size();p.digest=environment::digest(r.objects);if (!p.digest) return false;
    if (!net.queueEnvironment(p)) return false;
    p.operation=ENV_ROW;
    for (environment::Objects::const_iterator it=r.objects.begin();it!=r.objects.end();++it) { p.object=it->second;if (!net.queueEnvironment(p)) return false; }
    memset(&p.object,0,sizeof(p.object));p.operation=ENV_END;
    if (!net.queueEnvironment(p)) return false;
    trace("SEND-END",p);return true;
}
bool EnvironmentItemsSync::reconcile(GameWorld* gw,const Key& k,Region& r) {
    r.complete=false;r.nativeCensus.reset();
    engine::setEnvironmentRegionReady(k.first,k.second,0);
    if (!r.snapshot.complete()) return false;
    std::vector<engine::EnvironmentNativeRow> native;
    procedural::NativeCensusFence captured;
    if (!engine::captureEnvironmentItems(gw,k.first,k.second,native,&captured)) return false;
    const environment::Objects& wanted=r.snapshot.rows();
    std::vector<bool> used(native.size(),false);
    std::map<u64,Hand> bound;std::vector<EnvironmentItemObject> create;
    for (environment::Objects::const_iterator it=wanted.begin();it!=wanted.end();++it) {
        unsigned found=(unsigned)native.size();
        std::map<u64,Hand>::const_iterator prior=r.bindings.find(it->first);
        // An accepted pickup/transfer excludes the old local object immediately.
        // Never refill that object from an older complete snapshot while its
        // authoritative removal is travelling on the reliable lane.
        if (prior!=r.bindings.end() && engine::environmentItemExcluded(prior->second.value)) return false;
        if (prior!=r.bindings.end()) for (unsigned j=0;j<native.size();++j)
            if (!used[j] && !memcmp(prior->second.value,native[j].hand,20) && environment::sameNative(it->second,native[j].object)) { found=j;break; }
        if (found==native.size()) for (unsigned j=0;j<native.size();++j)
            if (!used[j] && environment::sameNative(it->second,native[j].object)) { found=j;break; }
        if (found!=native.size()) { Hand h;memcpy(h.value,native[found].hand,20);bound[it->first]=h;used[found]=true; }
        else create.push_back(it->second);
    }
    // END+count+digest is necessary but not sufficient. Preflight EVERY affected
    // native object and anchor before any mutation; UNKNOWN never enters native.
    for (unsigned j=0;j<native.size();++j) if (!used[j] && !engine::environmentSafeToRemove(gw,native[j].hand)) return false;
    for (unsigned j=0;j<create.size();++j) {
        unsigned int parent[5],owner[5],persistent[5];
        if (!engine::environmentPreflight(gw,create[j],parent,owner,persistent)) return false;
    }
    if (!captured.current(engine::environmentOriginRevision())) return false;
    std::vector<Hand> minted;
    for (unsigned j=0;j<create.size();++j) {
        Hand h;
        if (!engine::spawnEnvironmentItem(gw,create[j],h.value)) {
            for (unsigned m=0;m<minted.size();++m) engine::removeEnvironmentItem(gw,minted[m].value);
            return false;
        }
        minted.push_back(h);bound[create[j].id]=h;
    }
    for (unsigned j=0;j<native.size();++j) if (!used[j] && !engine::removeEnvironmentItem(gw,native[j].hand)) {
        // Retain successful bindings for idempotent retry; no ACK on partial work.
        r.bindings.swap(bound);return false;
    }
    r.bindings.swap(bound);
    std::vector<engine::EnvironmentNativeRow> after;
    if (!engine::captureEnvironmentItems(gw,k.first,k.second,after,&r.nativeCensus) || after.size()!=wanted.size()) return false;
    std::vector<bool> verified(after.size(),false);
    for (environment::Objects::const_iterator it=wanted.begin();it!=wanted.end();++it) {
        bool found=false;
        for (unsigned j=0;j<after.size();++j) if (!verified[j] && environment::sameNative(it->second,after[j].object)) { verified[j]=true;found=true;break; }
        if (!found) return false;
    }
    r.nativeCensus.finish(true,engine::environmentOriginRevision());
    if (!r.nativeCensus.current(engine::environmentOriginRevision())) return false;
    r.objects=wanted;r.epoch=r.snapshot.epoch();r.complete=true;
    return r.nativeCensus.current(engine::environmentOriginRevision());
}
void EnvironmentItemsSync::update(GameWorld* gw,NetLink& net,Inbound& in,bool host,u32 owner,u64 world,u32 load,bool live) {
    if (!live || !gw || !world || !load || owner>=MAX_PLAYERS || !engine::environmentItemHooksReady()) { reset();return; }
    if (world_!=world || load_!=load || host_!=host || owner_!=owner) {
        reset();world_=world;load_=load;host_=host;owner_=owner;engine::setEnvironmentJoin(!host);
    }
    std::deque<EnvironmentItemPacket> incoming;in.drainEnvironment(incoming);
    const u32 now=GetTickCount();
    const u64 nativeRevision=engine::environmentOriginRevision();
    if (!nativeCensus_.current(nativeRevision)) {
        committed_=false;engine::clearEnvironmentRegionReadiness();
    }
    // Inventory/world mutations and loaded-set enumeration run on the game thread.
    const bool scan=!lastScan_ || now-lastScan_>=250;
    if (scan) {
        nativeCensus_.begin(nativeRevision);
        lastScan_=now;engine::ProceduralRegionRead loaded[256];bool complete=false;
        const unsigned n=engine::enumerateProceduralRegions(gw,loaded,256,&complete);
        complete_=complete && n>0 && n<256;
        std::set<Key> previouslyLoaded;
        for (std::map<Key,Region>::iterator it=regions_.begin();it!=regions_.end();++it) {
            if (it->second.loaded) previouslyLoaded.insert(it->first);
            it->second.loaded=false;
        }
        for (unsigned i=0;i<n && i<256;++i) {
            const Key k(loaded[i].x,loaded[i].z);Region& r=regions_[k];
            if (!previouslyLoaded.count(k)) {
                if (++r.scopeId==0) { complete_=false;continue; }
                r.snapshot.reset(world_,load_,k.first,k.second,r.scopeId);
                r.ready=false;r.nativeCensus.reset();r.lastRequest=0;committed_=false;
            }
            r.retiring=false;r.loaded=true;r.complete=loaded[i].complete;
            if (!r.complete) { r.ready=false;r.nativeCensus.reset();engine::setEnvironmentRegionReady(k.first,k.second,0); }
            else if (host_) captureHost(gw,k,r);
        }
        for (std::map<Key,Region>::iterator it=regions_.begin();it!=regions_.end();++it) if (!it->second.loaded && previouslyLoaded.count(it->first)) {
            it->second.ready=it->second.complete=false;it->second.nativeCensus.reset();it->second.retiring=!host_;committed_=false;
            engine::setEnvironmentRegionReady(it->first.first,it->first.second,0);
        }
    }
    for (std::deque<EnvironmentItemPacket>::const_iterator it=incoming.begin();it!=incoming.end();++it) {
        const EnvironmentItemPacket& p=*it;
        if (p.type!=PKT_ENVIRONMENT_ITEMS || p.worldToken!=world_ || p.targetId!=owner_ || p.ownerId>=MAX_PLAYERS || p.ownerId==owner_) continue;
        const Key k(p.regionX,p.regionZ);
        if (host_) {
            if (!p.loadId || p.loadId<peerLoads_[p.ownerId]) continue;
            if (p.loadId!=peerLoads_[p.ownerId]) {
                if (p.operation!=ENV_REQUEST && p.operation!=ENV_SCOPE_COMMIT) continue;
                peerLoads_[p.ownerId]=p.loadId;scopes_[p.ownerId].reset(world_,p.loadId);
                for (std::map<Key,Region>::iterator r=regions_.begin();r!=regions_.end();++r)
                    r->second.requested[p.ownerId]=r->second.acknowledged[p.ownerId]=r->second.requestedScope[p.ownerId]=0;
            }
            if (p.operation==ENV_SCOPE_COMMIT) { scopes_[p.ownerId].accept(scopeControl(p));continue; }
            if (p.operation==ENV_REQUEST || p.operation==ENV_RETIRE) {
                std::map<Key,Region>::const_iterator existing=regions_.find(k);
                if (!p.scopeId || (existing!=regions_.end() && p.scopeId<existing->second.requestedScope[p.ownerId])) continue;
                if (!scopes_[p.ownerId].accept(scopeControl(p))) continue;
                Region& r=regions_[k];
                r.requestedScope[p.ownerId]=p.scopeId;
                r.requested[p.ownerId]=p.operation==ENV_REQUEST?p.loadId:0;
                if (p.operation==ENV_RETIRE) r.acknowledged[p.ownerId]=0;
                else r.lastSend=0;
            } else if (p.operation==ENV_ACK) {
                std::map<Key,Region>::iterator r=regions_.find(k);
                if (r==regions_.end()) continue;
                if (r->second.loaded && environment::reconciledCensusCurrent(r->second.complete,r->second.ready,r->second.nativeCensus,engine::environmentOriginRevision()) &&
                    r->second.requested[p.ownerId]==p.loadId && r->second.requestedScope[p.ownerId]==p.scopeId && r->second.epoch==p.epoch &&
                    p.count==r->second.objects.size() && p.digest==environment::digest(r->second.objects)) { r->second.acknowledged[p.ownerId]=p.epoch;trace("HOST-ACK",p); }
            }
        } else {
            if (p.ownerId!=0 || p.loadId!=load_) continue;
            std::map<Key,Region>::iterator r=regions_.find(k);
            if (r==regions_.end() || !r->second.loaded || r->second.retiring || p.scopeId!=r->second.scopeId) continue;
            const u32 previous=r->second.snapshot.epoch();
            if (r->second.snapshot.accept(p)) {
                if (previous!=r->second.snapshot.epoch()) { r->second.ready=false;r->second.nativeCensus.reset();committed_=false;engine::setEnvironmentRegionReady(k.first,k.second,0); }
                if (p.operation==ENV_END) trace("RECV-END",p);
            }
            if (!r->second.snapshot.complete()) { r->second.ready=false;r->second.nativeCensus.reset();committed_=false;engine::setEnvironmentRegionReady(k.first,k.second,0); }
        }
    }
    if (!complete_) {
        for (std::map<Key,Region>::iterator it=regions_.begin();it!=regions_.end();++it) {
            it->second.ready=false;it->second.nativeCensus.reset();engine::setEnvironmentRegionReady(it->first.first,it->first.second,0);
        }
        nativeCensus_.reset();committed_=false;return;
    }
    unsigned loadedCount=0;
    for (std::map<Key,Region>::iterator it=regions_.begin();it!=regions_.end();++it) {
        Region& r=it->second;const Key& k=it->first;
        if (!host_ && r.retiring) {
            EnvironmentItemPacket p=control(k,ENV_RETIRE,0,load_,0);
            if (net.queueEnvironment(p)) { r.retiring=false;r.snapshot.reset(world_,load_,k.first,k.second,r.scopeId); }
            continue;
        }
        if (!r.loaded) continue;++loadedCount;
        if (!r.complete) { r.ready=false;committed_=false;engine::setEnvironmentRegionReady(k.first,k.second,0);continue; }
        if (host_) {
            if (!r.lastSend || now-r.lastSend>=1500) {
                bool sent=true;
                for (u32 peer=1;peer<MAX_PLAYERS;++peer) if (r.requested[peer] && r.acknowledged[peer]!=r.epoch) sent=send(net,k,r,peer) && sent;
                if (sent) r.lastSend=now;
            }
        } else {
            if (!r.lastRequest || now-r.lastRequest>=1500) {
                EnvironmentItemPacket p=control(k,ENV_REQUEST,0,load_,0);
                if (net.queueEnvironment(p)) r.lastRequest=now;
            }
            if (r.snapshot.complete()) {
                // Recheck even after ACK: a native birth or player operation may
                // invalidate the local set while a new host epoch is in flight.
                const bool wasReady=environment::reconciledCensusCurrent(r.complete,r.ready,r.nativeCensus,engine::environmentOriginRevision());
                r.ready=reconcile(gw,k,r);
                engine::setEnvironmentRegionReady(k.first,k.second,r.ready?&r.nativeCensus:0);
                if (!r.ready) { committed_=false;continue; }
                EnvironmentItemPacket p=control(k,ENV_ACK,r.snapshot.epoch(),load_,0);
                p.count=(u32)r.objects.size();p.digest=r.snapshot.checksum();
                if (!wasReady && net.queueEnvironment(p)) trace("JOIN-APPLIED",p);
                else if (!wasReady) { r.ready=false;committed_=false;engine::setEnvironmentRegionReady(k.first,k.second,0); }
                // END retry must receive ACK again, even for an already applied epoch.
                if (wasReady && now-r.lastSend>=1500 && net.queueEnvironment(p)) r.lastSend=now;
            }
        }
    }
    if (scan) nativeCensus_.finish(complete_,engine::environmentOriginRevision());
    if (!nativeCensus_.current(engine::environmentOriginRevision())) {
        committed_=false;engine::clearEnvironmentRegionReadiness();
        return; // A scope assembled across native provenance changes cannot commit.
    }
    if (!host_ && !committed_ && allLoadedRegionsReady()) {
        EnvironmentItemPacket p=control(Key(),ENV_SCOPE_COMMIT,0,load_,0);p.count=loadedCount;
        committed_=net.queueEnvironment(p);
    }
}
bool EnvironmentItemsSync::regionReady(i32 x,i32 z) const {
    std::map<Key,Region>::const_iterator it=regions_.find(Key(x,z));
    const u64 revision=engine::environmentOriginRevision();
    return complete_ && it!=regions_.end() && it->second.loaded &&
        environment::reconciledCensusCurrent(it->second.complete,it->second.ready,it->second.nativeCensus,revision) &&
        nativeCensus_.current(engine::environmentOriginRevision());
}
bool EnvironmentItemsSync::allLoadedRegionsReady() const {
    if (!world_ || !complete_) return false;
    const u64 revision=engine::environmentOriginRevision();
    for (std::map<Key,Region>::const_iterator it=regions_.begin();it!=regions_.end();++it)
        if (it->second.retiring || (it->second.loaded &&
            !environment::reconciledCensusCurrent(it->second.complete,it->second.ready,it->second.nativeCensus,revision))) return false;
    return nativeCensus_.current(engine::environmentOriginRevision());
}
bool EnvironmentItemsSync::peerReady(u32 peer) const {
    if (!host_ || !complete_ || peer>=MAX_PLAYERS || !scopes_[peer].committed()) return false;
    const u64 revision=engine::environmentOriginRevision();
    for (std::map<Key,Region>::const_iterator it=regions_.begin();it!=regions_.end();++it) if (it->second.requested[peer] &&
        (!it->second.loaded || !environment::reconciledCensusCurrent(it->second.complete,
            it->second.ready && it->second.acknowledged[peer]==it->second.epoch,it->second.nativeCensus,revision))) return false;
    return nativeCensus_.current(engine::environmentOriginRevision());
}
bool EnvironmentItemsSync::queryRegion(i32 x,i32 z,u32* epoch,u32* checksum,unsigned* count) const {
    std::map<Key,Region>::const_iterator it=regions_.find(Key(x,z));
    if (it==regions_.end() || !regionReady(x,z)) return false;
    if (epoch) *epoch=it->second.epoch;if (checksum) *checksum=environment::digest(it->second.objects);if (count) *count=(unsigned)it->second.objects.size();
    return regionReady(x,z);
}
bool EnvironmentItemsSync::queryItem(const unsigned int hand[5],u64* id,u32* epoch,u32* checksum) const {
    if (!hand) return false;
    for (std::map<Key,Region>::const_iterator r=regions_.begin();r!=regions_.end();++r) {
        if (!regionReady(r->first.first,r->first.second)) continue;
        for (std::map<u64,Hand>::const_iterator it=r->second.bindings.begin();it!=r->second.bindings.end();++it)
            if (!memcmp(hand,it->second.value,20)) {
                if (id) *id=it->first;if (epoch) *epoch=r->second.epoch;if (checksum) *checksum=environment::digest(r->second.objects);
                return regionReady(r->first.first,r->first.second);
            }
    }
    return false;
}
bool EnvironmentItemsSync::queryScope(i32 x,i32 z,u64* world,u32* load,u32* scope) const {
    std::map<Key,Region>::const_iterator r=regions_.find(Key(x,z));
    if (r==regions_.end() || !regionReady(x,z)) return false;
    if (world) *world=world_;if (load) *load=load_;if (scope) *scope=r->second.scopeId;
    return regionReady(x,z);
}
}
