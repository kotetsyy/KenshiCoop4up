#include "ProceduralWorldSync.h"
#include "../net/NetLink.h"
#include "../core/Inbound.h"
#include "../CoopLog.h"
#include <windows.h>
#include <cstdio>
#include <cstring>

namespace coop {
namespace {
const u32 SCAN_MS = 250;
const u32 REQUEST_MS = 1500;

bool hasId(const std::vector<ProceduralObject>& rows, u64 id) {
    for (unsigned i=0;i<rows.size();++i) if (rows[i].id==id) return true;
    return false;
}
bool sameSet(const std::vector<ProceduralObject>& a, const std::vector<ProceduralObject>& b) {
    if (a.size()!=b.size()) return false;
    for (unsigned i=0;i<a.size();++i) {
        bool found=false;
        for (unsigned j=0;j<b.size();++j)
            if (a[i].id==b[j].id && procedural::sameObject(a[i],b[j])) { found=true; break; }
        if (!found) return false;
    }
    return true;
}
ProceduralPacket control(u32 owner,u64 world,u32 load,i32 x,i32 z,u8 op,u32 epoch) {
    ProceduralPacket p; memset(&p,0,sizeof(p));
    p.type=PKT_PROCEDURAL; p.ownerId=owner; p.worldToken=world; p.loadId=load;
    p.regionX=x; p.regionZ=z; p.operation=op; p.epoch=epoch;
    return p;
}
void logControl(const char* event,bool host,const ProceduralPacket& p,const char* reason) {
    char line[512];
    _snprintf(line,sizeof(line)-1,
        "[procworld] %s role=%s world=%I64u load=%u region=%d,%d epoch=%u owner=%u count=%u digest=%08x reason=%s",
        event,host?"HOST":"JOIN",p.worldToken,p.loadId,p.regionX,p.regionZ,p.epoch,
        p.ownerId,p.count,p.digest,reason);
    line[sizeof(line)-1]=0; logLine(line);
}
}

ProceduralWorldSync::Region::Region():loaded(false),wasLoaded(false),censusComplete(false),reconciled(false),
    requestQueued(false),retirePending(false),epoch(0),lastRequest(0),lastSend(0),pendingReason("") {
    memset(requestedLoads,0,sizeof(requestedLoads));
    memset(acknowledgedEpochs,0,sizeof(acknowledgedEpochs));
}
ProceduralWorldSync::ProceduralWorldSync():nativeWorld_(0),world_(0),nextId_(0),load_(0),lastScan_(0),
    host_(false),registryComplete_(false),active_(false),scopeCommitted_(false),registryState_(-1) {}
void ProceduralWorldSync::reset() {
    regions_.clear(); nativeWorld_=0; world_=0; nextId_=0; load_=0; lastScan_=0;
    registryComplete_=false; active_=false; scopeCommitted_=false;
    registryState_=-1;
    incoming_.clear();
    nativeCensus_.reset();
    for (u32 id=0;id<MAX_PLAYERS;++id) peerScopes_[id].reset(0,0);
    engine::clearProceduralRegionReadiness();
}
bool ProceduralWorldSync::capture(GameWorld* gw,const RegionKey& key,
        std::vector<ProceduralObject>& objects,std::vector<engine::ProceduralBedRead>* native,
        const std::vector<ProceduralObject>* wanted) {
    objects.clear(); if (native) native->clear();
    if (!procedural::managedBedRegion(key.x,key.z)) return true;
    bool complete=false;
    const unsigned n=engine::captureProceduralBeds(gw,key.x,key.z,scratch_,
        PROCEDURAL_OBJECTS_MAX,&complete);
    if (!complete || n>PROCEDURAL_OBJECTS_MAX) return false;
    for (unsigned i=0;i<n;++i) {
        if (!scratch_[i].procedural && !scratch_[i].replica) {
            // A transferred save may already contain the HOST's generated
            // layout bed. Adopt only a positively saved, non-player bed with
            // the complete matching descriptor. Unmatched saved beds stay
            // outside this procedural set and can never enter its removal set.
            bool adopt=false;
            if (wanted && scratch_[i].saved) {
                for (unsigned j=0;j<wanted->size();++j)
                    if (procedural::sameObject(scratch_[i].object,(*wanted)[j])) { adopt=true; break; }
            }
            if (!adopt) continue;
        }
        ProceduralObject p=scratch_[i].object;
        // IDs are assigned by the HOST catalog, not inferred from native hands.
        p.id=1;
        if (!procedural::validObject(p)) return false;
        objects.push_back(p);
        if (native) native->push_back(scratch_[i]);
    }
    return true;
}
void ProceduralWorldSync::updateHostCatalog(GameWorld* gw,const RegionKey& key,Region& r) {
    r.censusComplete=capture(gw,key,r.observed,&r.native);
    if (!r.censusComplete) { r.reconciled=false; return; }
    r.used.assign(r.history.size(),0);
    for (unsigned i=0;i<r.observed.size();++i) {
        unsigned found=(unsigned)r.history.size();
        for (unsigned j=0;j<r.history.size();++j) {
            if (r.used[j]) continue;
            std::map<u64,LocalHand>::const_iterator binding=r.bindings.find(r.history[j].id);
            if (binding!=r.bindings.end() &&
                !memcmp(binding->second.value,r.native[i].hand,sizeof(binding->second.value))) {
                found=j; break;
            }
        }
        if (found==r.history.size()) {
            for (unsigned j=0;j<r.history.size();++j)
                if (!r.used[j] && procedural::samePlacement(r.history[j],r.observed[i])) { found=j; break; }
        }
        if (found==r.history.size()) {
            if (++nextId_==0) { r.censusComplete=false; return; }
            r.observed[i].id=nextId_;
            r.history.push_back(r.observed[i]); r.used.push_back(1);
        } else {
            r.observed[i].id=r.history[found].id;
            r.history[found]=r.observed[i]; r.used[found]=1;
        }
    }
    const bool changed=!r.epoch || !sameSet(r.objects,r.observed);
    if (changed) {
        if (++r.epoch==0) { r.censusComplete=false; return; }
        r.objects=r.observed;
        memset(r.acknowledgedEpochs,0,sizeof(r.acknowledgedEpochs));
    }
    r.bindings.clear();
    for (unsigned i=0;i<r.objects.size();++i) {
        // observed and native share enumeration order; objects may be retained
        // in a prior order when only native registry iteration changed.
        for (unsigned j=0;j<r.observed.size();++j) if (r.objects[i].id==r.observed[j].id) {
            LocalHand h; memcpy(h.value,r.native[j].hand,sizeof(h.value));
            r.bindings[r.objects[i].id]=h; break;
        }
    }
    r.reconciled=true;
    if (changed) logCatalog(key,r,r.objects);
}
void ProceduralWorldSync::sendSnapshot(NetLink& net,const RegionKey& key,Region& r,u32 requestedLoad) {
    if (!registryComplete_ || !r.loaded || !r.censusComplete || !r.epoch || !requestedLoad) return;
    ProceduralPacket p=control(0,world_,requestedLoad,key.x,key.z,PROC_BEGIN,r.epoch);
    p.count=(u32)r.objects.size(); p.digest=procedural::snapshotDigest(r.objects);
    if (!net.queueProcedural(p)) return;
    p.operation=PROC_ROW;
    for (unsigned i=0;i<r.objects.size();++i) {
        p.object=r.objects[i];
        if (!net.queueProcedural(p)) return;
    }
    memset(&p.object,0,sizeof(p.object)); p.operation=PROC_END;
    if (net.queueProcedural(p)) r.lastSend=GetTickCount();
}
bool ProceduralWorldSync::reconcile(GameWorld* gw,const RegionKey& key,Region& r) {
    if (!r.snapshot.complete()) { r.pendingReason="snapshot_incomplete"; return false; }
    if (!r.censusComplete) { r.pendingReason="native_census_incomplete"; return false; }
    const std::vector<ProceduralObject>& wanted=r.snapshot.objects();
    // The wire region is a mutation boundary, not just a routing hint. Validate
    // every descriptor with Kenshi's mapping before removing or creating ANY row.
    for (unsigned i=0;i<wanted.size();++i) {
        int x,z;
        if (!engine::cellAt(gw,wanted[i].x,wanted[i].z,&x,&z) || x!=key.x || z!=key.z) {
            r.pendingReason="snapshot_row_outside_native_region"; return false;
        }
    }
    if (!capture(gw,key,r.observed,&r.native,&wanted)) {
        r.censusComplete=false; r.pendingReason="native_capture_incomplete"; return false;
    }
    r.used.assign(r.native.size(),0);
    bool missing=false;
    for (unsigned i=0;i<wanted.size();++i) {
        bool found=false;
        for (unsigned j=0;j<r.observed.size();++j) {
            if (!r.used[j] && procedural::sameObject(wanted[i],r.observed[j])) {
                r.used[j]=1; r.observed[j].id=wanted[i].id; found=true; break;
            }
        }
        if (!found) missing=true;
    }
    // Preflight the ENTIRE removal set before changing anything. A partial
    // snapshot, saved bed, occupied prop or unsafe native reference never culls.
    bool extra=false;
    for (unsigned j=0;j<r.native.size();++j) if (!r.used[j]) {
        extra=true;
        if ((!r.native[j].procedural && !r.native[j].replica) || !r.native[j].safeToRemove) {
            r.pendingReason="extra_bed_not_safe_to_remove"; return false;
        }
    }
    if (extra) {
        for (unsigned j=0;j<r.native.size();++j)
            if (!r.used[j] && !engine::removeProceduralBed(gw,r.native[j].hand)) {
                r.pendingReason="native_remove_failed"; return false;
            }
        r.pendingReason="waiting_for_native_removal";
        return false; // native destruction must actually leave the next census
    }
    if (missing) {
        for (unsigned i=0;i<wanted.size();++i) {
            if (hasId(r.observed,wanted[i].id)) continue;
            unsigned int hand[5]={0};
            if (!engine::createProceduralBed(gw,wanted[i],world_,r.snapshot.epoch(),hand)) {
                r.pendingReason="native_create_failed"; return false;
            }
        }
        r.pendingReason="waiting_for_native_creation";
        return false; // re-read real engine state before binding/ACK
    }
    if (!sameSet(wanted,r.observed) || procedural::snapshotDigest(r.observed)!=r.snapshot.digest()) {
        r.pendingReason="recaptured_descriptor_mismatch"; return false;
    }
    for (unsigned i=0;i<r.native.size();++i) {
        if (r.native[i].saved &&
            !engine::adoptSavedProceduralBed(gw,r.native[i].hand,r.observed[i])) {
            r.pendingReason="saved_bed_adoption_failed"; return false;
        }
    }
    r.bindings.clear();
    for (unsigned i=0;i<r.observed.size();++i) {
        LocalHand h; memcpy(h.value,r.native[i].hand,sizeof(h.value));
        r.bindings[r.observed[i].id]=h;
    }
    r.objects=r.observed; r.epoch=r.snapshot.epoch();
    r.pendingReason="ready";
    return true;
}
void ProceduralWorldSync::update(GameWorld* gw,NetLink& net,Inbound& inbound,
        bool host,u32 loadId,bool worldLive) {
    if (!net.isRunning()) { if (world_) reset(); engine::setProceduralWorldJoin(false); return; }
    NetStatus status; net.copyStatus(&status);
    const bool connected=host || (status.memberMask&1u)!=0;
    engine::setProceduralWorldJoin(!host && connected);
    if (!connected) { if (world_) reset(); return; }
    if (!worldLive || !gw || !loadId || !engine::proceduralWorldHooksReady()) {
        if (active_ || registryState_!=-2) {
            logControl("SCOPE_INACTIVE",host,control(0,world_,loadId,0,0,0,0),
                !worldLive?"world_not_live":(!gw?"native_world_missing":(!loadId?"load_id_missing":"procedural_hooks_not_ready")));
            engine::clearProceduralRegionReadiness();
        }
        registryComplete_=false; active_=false; lastScan_=0;
        nativeCensus_.reset(); scopeCommitted_=false;
        registryState_=-2;
        return; // Leave reliable controls in Inbound until native scope returns.
    }
    const u64 world=net.currentWorldToken();
    if (!world) {
        if (active_ || registryState_!=-3) {
            logControl("SCOPE_INACTIVE",host,control(0,world_,loadId,0,0,0,0),"world_token_unavailable");
            engine::clearProceduralRegionReadiness();
        }
        registryComplete_=false; active_=false; lastScan_=0; registryState_=-3;
        nativeCensus_.reset(); scopeCommitted_=false;
        return;
    }
    if (world_!=world || host_!=host) { reset(); world_=world; host_=host; }
    if (load_!=loadId) {
        registryState_=-1;
        load_=loadId; lastScan_=0; scopeCommitted_=false;
        for (u32 id=0;id<MAX_PLAYERS;++id) peerScopes_[id].reset(world_,load_);
        nativeCensus_.reset();
        engine::clearProceduralRegionReadiness();
        for (Regions::iterator it=regions_.begin();it!=regions_.end();++it) {
            Region& r=it->second;
            r.reconciled=false; r.lastRequest=0;
            r.loaded=false; r.pendingReason="";
            r.requestQueued=false; r.retirePending=false;
            r.snapshot.reset(world_,load_,it->first.x,it->first.z);
            memset(r.requestedLoads,0,sizeof(r.requestedLoads));
            memset(r.acknowledgedEpochs,0,sizeof(r.acknowledgedEpochs));
        }
    }
    active_=true;
    nativeWorld_=gw;
    incoming_.clear(); inbound.drainProcedural(incoming_);
    if (host) for (u32 id=1;id<MAX_PLAYERS;++id) {
        if (status.memberMask&(1u<<id)) continue;
        peerScopes_[id].reset(world_,load_);
        for (Regions::iterator it=regions_.begin();it!=regions_.end();++it) {
            it->second.requestedLoads[id]=0; it->second.acknowledgedEpochs[id]=0;
        }
    }
    const u32 now=GetTickCount();
    const u64 nativeRevision=engine::proceduralOriginRevision();
    if (!nativeCensus_.current(nativeRevision)) scopeCommitted_=false;
    const bool scan=!lastScan_ || now-lastScan_>=SCAN_MS;
    if (scan) {
        nativeCensus_.begin(nativeRevision);
        lastScan_=now;
        for (Regions::iterator it=regions_.begin();it!=regions_.end();++it) {
            it->second.wasLoaded=it->second.loaded; it->second.loaded=false;
        }
        engine::ProceduralRegionRead loaded[256]; bool complete=false;
        const unsigned n=engine::enumerateProceduralRegions(gw,loaded,256,&complete);
        registryComplete_=complete && n>0 && n<=256;
        if (registryState_!=(registryComplete_?1:0)) {
            registryState_=registryComplete_?1:0;
            ProceduralPacket census=control(net.localId(),world_,load_,0,0,0,0); census.count=n;
            logControl("SCOPE_CENSUS",host,census,registryComplete_?"complete":"native_region_registry_incomplete");
        }
        for (unsigned i=0;i<n && i<256;++i) {
            const RegionKey key(loaded[i].x,loaded[i].z);
            if (!procedural::managedBedRegion(key.x,key.z)) continue;
            Region& r=regions_[key];
            r.loaded=true; r.censusComplete=loaded[i].complete;
            // Never let a recycled native hand inherit a previous incarnation's
            // logical identity. Full descriptors in history survive streaming.
            if (!r.wasLoaded) r.bindings.clear();
            if (host && r.censusComplete) updateHostCatalog(gw,key,r);
        }
        for (Regions::iterator it=regions_.begin();it!=regions_.end();++it) {
            Region& r=it->second;
            if (r.loaded!=r.wasLoaded)
                logControl(r.loaded?"REGION_LOADED":"REGION_UNLOADED",host,
                    control(0,world_,load_,it->first.x,it->first.z,0,r.epoch),"native_membership");
            if (!registryComplete_ || !r.loaded || !r.censusComplete) {
                const char* reason=r.loaded?"native_census_incomplete":"region_unloaded";
                if (strcmp(r.pendingReason,reason)) {
                    r.pendingReason=reason;
                    logControl("PENDING",host,control(0,world_,load_,it->first.x,it->first.z,0,r.epoch),reason);
                }
            }
            if (!registryComplete_ || !r.loaded || !r.censusComplete) {
                if (!host && (r.loaded || r.requestQueued)) scopeCommitted_=false;
                r.reconciled=false;
                engine::setProceduralRegionReady(it->first.x,it->first.z,false);
                if (!r.loaded && !host) {
                    if (r.requestQueued) r.retirePending=true;
                    r.snapshot.reset(world_,load_,it->first.x,it->first.z);
                    r.bindings.clear(); r.lastRequest=0;
                }
            }
        }
    }
    for (std::deque<InboundProcedural>::const_iterator it=incoming_.begin();it!=incoming_.end();++it) {
        const ProceduralPacket& p=it->pkt;
        if (p.worldToken!=world_ || p.loadId!=load_ || p.ownerId!=it->ownerId) continue;
        if (p.operation!=PROC_SCOPE_COMMIT && !procedural::managedBedRegion(p.regionX,p.regionZ)) continue;
        if (host) {
            if (!p.ownerId || p.ownerId>=MAX_PLAYERS) continue;
            if (p.operation==PROC_SCOPE_COMMIT) {
                const bool committed=peerScopes_[p.ownerId].committed();
                if (peerScopes_[p.ownerId].accept(p) && !committed)
                    logControl("SCOPE_COMMIT",host,p,"ordered_membership_committed");
                continue;
            }
            const RegionKey key(p.regionX,p.regionZ);
            if (p.operation==PROC_REQUEST || p.operation==PROC_RETIRE) {
                if (!peerScopes_[p.ownerId].accept(p)) continue;
                Region& r=regions_[key];
                if (p.operation==PROC_RETIRE) {
                    if (r.requestedLoads[p.ownerId]) logControl("RETIRE",host,p,"removed_ack_obligation");
                    r.requestedLoads[p.ownerId]=0;
                    r.acknowledgedEpochs[p.ownerId]=0;
                } else {
                    if (r.requestedLoads[p.ownerId]!=p.loadId) logControl("REQUEST",host,p,"current_membership");
                    r.requestedLoads[p.ownerId]=p.loadId;
                    sendSnapshot(net,key,r,p.loadId);
                }
                continue;
            }
            Regions::iterator found=regions_.find(key);
            if (found==regions_.end()) continue;
            Region& r=found->second;
            if (p.operation==PROC_ACK && registryComplete_ && r.loaded && r.censusComplete &&
                r.requestedLoads[p.ownerId]==p.loadId && p.epoch==r.epoch &&
                p.count==r.objects.size() && p.digest==procedural::snapshotDigest(r.objects)) {
                if (r.acknowledgedEpochs[p.ownerId]!=p.epoch) logControl("ACK",host,p,"exact_snapshot");
                r.acknowledgedEpochs[p.ownerId]=p.epoch;
            }
            continue;
        }
        Regions::iterator found=regions_.find(RegionKey(p.regionX,p.regionZ));
        if (found==regions_.end() || !found->second.loaded) continue;
        Region& r=found->second;
        if (p.ownerId==0 && (p.operation==PROC_BEGIN || p.operation==PROC_ROW || p.operation==PROC_END)) {
            if (!r.snapshot.epoch()) r.snapshot.reset(world_,load_,p.regionX,p.regionZ);
            const bool wasComplete=r.snapshot.complete();
            const u32 previousEpoch=r.snapshot.epoch();
            r.snapshot.accept(p);
            if (p.operation==PROC_BEGIN && previousEpoch!=r.snapshot.epoch())
                logControl("SNAPSHOT_BEGIN",host,p,"receiving");
            if (!wasComplete && r.snapshot.complete()) logControl("SNAPSHOT_COMPLETE",host,p,"count_digest_verified");
            if (!r.snapshot.complete() || !wasComplete) {
                r.reconciled=false;
                scopeCommitted_=false;
                engine::setProceduralRegionReady(p.regionX,p.regionZ,false);
            }
        }
    }
    if (!scan) return;
    for (Regions::iterator it=regions_.begin();it!=regions_.end();++it) {
        Region& r=it->second;
        if (!host && r.retirePending) {
            ProceduralPacket retire=control(net.localId(),world_,load_,it->first.x,it->first.z,PROC_RETIRE,0);
            if (!net.queueProcedural(retire)) { scopeCommitted_=false; continue; }
            r.retirePending=false; r.requestQueued=false;
            logControl("RETIRE",host,retire,"queued_after_native_unload");
        }
        if (!registryComplete_ || !r.loaded || !r.censusComplete) continue;
        if (host) {
            for (u32 id=1;id<MAX_PLAYERS;++id)
                if (r.requestedLoads[id]==load_ && r.acknowledgedEpochs[id]!=r.epoch &&
                    (!r.lastSend || now-r.lastSend>=REQUEST_MS)) sendSnapshot(net,it->first,r,load_);
            continue;
        }
        if (!r.requestQueued) {
            ProceduralPacket request=control(net.localId(),world_,load_,it->first.x,it->first.z,PROC_REQUEST,0);
            if (!net.queueProcedural(request)) { r.reconciled=false; scopeCommitted_=false; continue; }
            r.requestQueued=true; r.lastRequest=now; scopeCommitted_=false;
            logControl("REQUEST",host,request,"queued_current_membership");
        }
        const bool previouslyReady=r.reconciled;
        const char* previousReason=r.pendingReason;
        r.reconciled=reconcile(gw,it->first,r);
        if (strcmp(previousReason,r.pendingReason))
            logControl(r.reconciled?"RECONCILED":"PENDING",host,
                control(net.localId(),world_,load_,it->first.x,it->first.z,0,r.snapshot.epoch()),r.pendingReason);
        if (!r.reconciled) scopeCommitted_=false;
        if (r.reconciled) {
            if (!previouslyReady || !r.snapshot.acknowledged()) {
                ProceduralPacket ack=control(net.localId(),world_,load_,it->first.x,it->first.z,PROC_ACK,r.epoch);
                ack.count=(u32)r.objects.size(); ack.digest=procedural::snapshotDigest(r.objects);
                if (net.queueProcedural(ack)) {
                    r.snapshot.markAcknowledged();
                    if (!previouslyReady) logCatalog(it->first,r,r.objects);
                    if (!previouslyReady || !r.lastSend || now-r.lastSend>=REQUEST_MS) {
                        logControl("ACK",host,ack,previouslyReady?"queued_exact_reack":"queued_after_native_match");
                        r.lastSend=now;
                    }
                } else r.reconciled=false;
            }
        }
        engine::setProceduralRegionReady(it->first.x,it->first.z,r.reconciled);
        if (!r.reconciled && (!r.lastRequest || now-r.lastRequest>=REQUEST_MS)) {
            ProceduralPacket request=control(net.localId(),world_,load_,it->first.x,it->first.z,PROC_REQUEST,0);
            if (net.queueProcedural(request)) { r.lastRequest=now; r.requestQueued=true; }
        }
    }
    nativeCensus_.finish(registryComplete_,engine::proceduralOriginRevision());
    if (!nativeCensus_.current(engine::proceduralOriginRevision())) {
        scopeCommitted_=false;
        engine::clearProceduralRegionReadiness();
        return; // Never commit a loaded set assembled across native generation.
    }
    if (!host && registryComplete_ && !scopeCommitted_) {
        ProceduralPacket commit=control(net.localId(),world_,load_,0,0,PROC_SCOPE_COMMIT,0);
        bool queued=true;
        for (Regions::const_iterator it=regions_.begin();it!=regions_.end();++it) {
            const Region& r=it->second;
            if (r.retirePending) queued=false;
            if (!r.loaded) continue;
            ++commit.count;
            if (!r.censusComplete || !r.reconciled || !r.requestQueued || !r.snapshot.acknowledged()) queued=false;
        }
        if (queued && net.queueProcedural(commit)) {
            scopeCommitted_=true; logControl("SCOPE_COMMIT",host,commit,"queued_after_all_controls");
        }
    }
}
bool ProceduralWorldSync::allLoadedRegionsReady() const {
    if (!active_ || !registryComplete_ || (!host_ && !scopeCommitted_)) return false;
    for (Regions::const_iterator it=regions_.begin();it!=regions_.end();++it) {
        if (!it->second.loaded) continue;
        if (!it->second.censusComplete || !it->second.reconciled) return false;
    }
    return nativeCensus_.current(engine::proceduralOriginRevision());
}
bool ProceduralWorldSync::peerReady(u32 id) const {
    if (!host_ || !active_ || !registryComplete_ || id==0 || id>=MAX_PLAYERS || !peerScopes_[id].committed()) return false;
    for (Regions::const_iterator it=regions_.begin();it!=regions_.end();++it) {
        const Region& r=it->second;
        if (r.requestedLoads[id]!=load_) continue;
        if (!r.loaded || !r.censusComplete || r.acknowledgedEpochs[id]!=r.epoch) return false;
    }
    return nativeCensus_.current(engine::proceduralOriginRevision());
}
bool ProceduralWorldSync::regionReady(i32 x,i32 z) const {
    Regions::const_iterator it=regions_.find(RegionKey(x,z));
    return active_ && registryComplete_ && it!=regions_.end() && it->second.loaded &&
        it->second.censusComplete && it->second.reconciled &&
        nativeCensus_.current(engine::proceduralOriginRevision());
}
bool ProceduralWorldSync::queryRegion(GameWorld*,i32 x,i32 z,ProceduralBedBinding* out) const {
    if (!out) return false;
    memset(out,0,sizeof(*out));
    Regions::const_iterator it=regions_.find(RegionKey(x,z));
    if (!active_ || !registryComplete_ || it==regions_.end() || !it->second.loaded ||
        !it->second.censusComplete || !it->second.epoch ||
        !nativeCensus_.current(engine::proceduralOriginRevision())) return false;
    out->world=world_; out->loadId=load_; out->epoch=it->second.epoch;
    out->regionX=x; out->regionZ=z; out->ready=it->second.reconciled;
    return true;
}
bool ProceduralWorldSync::queryBed(GameWorld* gw,const unsigned int localHand[5],ProceduralBedBinding* out) const {
    if (!out) return false;
    memset(out,0,sizeof(*out));
    if (!active_ || !registryComplete_ || !nativeCensus_.current(engine::proceduralOriginRevision())) return false;
    for (Regions::const_iterator it=regions_.begin();it!=regions_.end();++it) {
        const Region& r=it->second;
        if (!r.loaded || !r.censusComplete || !r.reconciled) continue;
        for (unsigned i=0;i<r.objects.size();++i) {
            std::map<u64,LocalHand>::const_iterator h=r.bindings.find(r.objects[i].id);
            if (h==r.bindings.end() || (localHand && memcmp(localHand,h->second.value,sizeof(h->second.value)))) continue;
            engine::PoseFixtureRead native;
            if (!engine::readPoseFixtureByHand(h->second.value,&native)) continue;
            if (!queryRegion(gw,it->first.x,it->first.z,out)) return false;
            out->id=r.objects[i].id; memcpy(out->hand,h->second.value,sizeof(out->hand));
            return true;
        }
    }
    return false;
}
bool ProceduralWorldSync::localHand(u64 id,unsigned int out[5]) const {
    if (!active_ || !registryComplete_ || !id || !out ||
        !nativeCensus_.current(engine::proceduralOriginRevision())) return false;
    for (Regions::const_iterator it=regions_.begin();it!=regions_.end();++it) {
        const Region& r=it->second;
        if (!r.loaded || !r.censusComplete || !r.reconciled) continue;
        std::map<u64,LocalHand>::const_iterator h=r.bindings.find(id);
        if (h==r.bindings.end()) continue;
        engine::PoseFixtureRead native;
        if (!engine::readPoseFixtureByHand(h->second.value,&native)) return false;
        if (!nativeCensus_.current(engine::proceduralOriginRevision())) return false;
        memcpy(out,h->second.value,sizeof(h->second.value)); return true;
    }
    return false;
}
int ProceduralWorldSync::resolveFixture(const FixturePacket& p,unsigned int out[5],u64* logicalId) const {
    if (logicalId) *logicalId=0;
    if (!engine::proceduralBedTemplate(p.sid)) return 0;
    i32 x,z;
    if (!active_ || !nativeWorld_ || !engine::cellAt(nativeWorld_,p.x,p.z,&x,&z)) return -1;
    if (!procedural::managedBedRegion(x,z)) return 0;
    if (!out || !active_) return -1;
    u64 match=0;
    for (Regions::const_iterator it=regions_.begin();it!=regions_.end();++it) {
        const Region& r=it->second;
        if (!r.loaded || !r.censusComplete || !r.reconciled) continue;
        for (unsigned i=0;i<r.objects.size();++i) {
            const ProceduralObject& o=r.objects[i];
            ProceduralObject candidate=o;
            memcpy(candidate.sid,p.sid,sizeof(candidate.sid));
            candidate.x=p.x; candidate.y=p.y; candidate.z=p.z;
            // Legacy pose hints carry yaw only. Keep the certified catalog's
            // full quaternion; this lookup cannot replace native reconciliation.
            if (p.poseFlags&POSE_FIXTURE_VALID) {
                candidate.heading=p.heading; candidate.floor=p.floor;
                candidate.flags=0;
                if (p.poseFlags&POSE_FIXTURE_OWNER_PRESENT) candidate.flags|=PROC_OWNER;
                if (p.poseFlags&POSE_FIXTURE_OUTSIDE) candidate.flags|=PROC_OUTSIDE;
                if (p.town.index || p.town.serial) candidate.flags|=PROC_TOWN;
                if (p.inside.index || p.inside.serial) candidate.flags|=PROC_INSIDE;
                memcpy(candidate.ownerSid,p.ownerSid,sizeof(candidate.ownerSid));
                memcpy(candidate.townSid,p.townSid,sizeof(candidate.townSid));
                memcpy(candidate.insideSid,p.insideSid,sizeof(candidate.insideSid));
                candidate.townX=p.townX; candidate.townY=p.townY; candidate.townZ=p.townZ;
                candidate.insideX=p.insideX; candidate.insideY=p.insideY; candidate.insideZ=p.insideZ;
            }
            if (!procedural::sameObject(o,candidate)) continue;
            unsigned int hand[5];
            if (!localHand(o.id,hand)) continue;
            engine::PoseFixtureRead native;
            if (!engine::readPoseFixtureByHand(hand,&native) || native.fixture.classType!=(int)p.classType) continue;
            if (match && match!=o.id) return -1;
            match=o.id; memcpy(out,hand,sizeof(hand));
        }
    }
    if (match) { if (logicalId) *logicalId=match; return 1; }
    // Only positive saved/player provenance plus exact native geometry permits
    // the old unmanaged path. A coincident raw hand never proves identity.
    if (engine::proceduralBedOrigin(p.hand)==0) {
        engine::PoseFixtureRead native;
        if (engine::readPoseFixtureByHand(p.hand,&native) && native.fixture.classType==(int)p.classType &&
            !strcmp(native.fixture.sid,p.sid) &&
            procedural::detail::quantize(native.fixture.x,100)==procedural::detail::quantize(p.x,100) &&
            procedural::detail::quantize(native.fixture.y,100)==procedural::detail::quantize(p.y,100) &&
            procedural::detail::quantize(native.fixture.z,100)==procedural::detail::quantize(p.z,100)) return 0;
    }
    return -1;
}
void ProceduralWorldSync::logCatalog(const RegionKey& key,const Region& r,
        const std::vector<ProceduralObject>& objects) const {
    char line[1536];
    const char* role=host_?"HOST":"JOIN";
    for (unsigned i=0;i<objects.size();++i) {
        const ProceduralObject& p=objects[i];
        _snprintf(line,sizeof(line)-1,
            "[procworld] OBJECT role=%s world=%I64u load=%u region=%d,%d epoch=%u id=%I64u category=%u sid='%s' pos=%.3f,%.3f,%.3f yaw=%.5f rotationX=%.9g rotationY=%.9g rotationZ=%.9g rotationW=%.9g floor=%d flags=%u function=%u state=%u progress=%.5f owner='%s' town='%s' townPos=%.3f,%.3f,%.3f inside='%s' insidePos=%.3f,%.3f,%.3f source=catalog",
            role,world_,load_,key.x,key.z,r.epoch,p.id,(unsigned)p.category,p.sid,p.x,p.y,p.z,p.heading,
            p.rotationX,p.rotationY,p.rotationZ,p.rotationW,p.floor,
            (unsigned)p.flags,p.buildingFunction,p.stateFlags,p.constructionProgress,p.ownerSid,p.townSid,
            p.townX,p.townY,p.townZ,p.insideSid,p.insideX,p.insideY,p.insideZ);
        line[sizeof(line)-1]='\0'; logLine(line);
    }
    _snprintf(line,sizeof(line)-1,
        "[procworld] CATALOG role=%s world=%I64u load=%u region=%d,%d epoch=%u complete=1 count=%u digest=%08x source=catalog",
        role,world_,load_,key.x,key.z,r.epoch,(unsigned)objects.size(),procedural::snapshotDigest(objects));
    line[sizeof(line)-1]='\0'; logLine(line);
}
} // namespace coop
