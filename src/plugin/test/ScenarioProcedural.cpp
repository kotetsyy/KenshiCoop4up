#include "ScenarioSupport.h"
#include "../sync/ProceduralWorldSync.h"
#include <core/Functions.h>
#include <kenshi/Building/UseableStuff.h>
#include <kenshi/Character.h>
#include <kenshi/GameWorld.h>
#include <kenshi/PlayerInterface.h>
#include <kenshi/Enums.h>
#include <vector>
#include <windows.h>

namespace coop { namespace {
// Keep native dereferences in a POD-only SEH leaf, not the vector-owning caller.
bool nativeOccupant(const unsigned int bed[5], unsigned int out[5]) {
    RootObject* object = engine::resolveObjectByHand(bed);
    typedef const hand& (__fastcall* OccupantFn)(const UseableStuff*);
    OccupantFn fn = reinterpret_cast<OccupantFn>(KenshiLib::GetRealAddress(&UseableStuff::getOccupant));
    if (!object || !fn) return false;
    __try {
        const hand& h = fn(reinterpret_cast<const UseableStuff*>(object));
        out[0]=(unsigned int)h.type; out[1]=h.container; out[2]=h.containerSerial;
        out[3]=h.index; out[4]=h.serial;
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// A partial captureSquad/listPlayerChars result cannot authorize scene relocation.
bool fixturePlayers(GameWorld* gw, Character** out, unsigned int* count) {
    *count=0;
    __try {
        if (!gw || !gw->player) return false;
        unsigned int n=(unsigned int)gw->player->playerCharacters.size();
        if (!n || n>128) return false;
        for (unsigned int i=0;i<n;++i) {
            Character* c=gw->player->playerCharacters[i];
            if (!c || !engine::isPlayerSquad(gw,c)) return false;
            for (unsigned int j=0;j<i;++j) if (out[j]==c) return false;
            out[i]=c;
        }
        *count=n; return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool fixtureFactoryReady(GameWorld* gw) {
    __try { return gw && gw->theFactory; }
    __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
typedef void (__fastcall* FixtureRelocateFn)(Character*,const Ogre::Vector3&);
bool invokeFixtureRelocation(FixtureRelocateFn fn,Character* c,const Ogre::Vector3& delta) {
    __try { fn(c,delta); return true; }
    __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool relocateFixture(Character* c,float x,float y,float z) {
    static FixtureRelocateFn fn=(FixtureRelocateFn)KenshiLib::GetRealAddress(&Character::relocationTeleport);
    float bx=0,by=0,bz=0;
    if (!fn || !engine::readPos(c,&bx,&by,&bz)) return false;
    Ogre::Vector3 delta(x-bx,y-by,z-bz);
    // Application only; the caller must independently read native positions.
    return invokeFixtureRelocation(fn,c,delta);
}
class ProceduralScenario : public TimedScenario {
public:
    explicit ProceduralScenario(bool lifecycle,bool outside=false) : TimedScenario(outside?"procedural_beds_outside_lifecycle":(lifecycle?"procedural_beds_lifecycle":"procedural_beds"),1000),
        outside_(outside),staged_(!outside),lifecycle_(lifecycle), ordered_(false), observed_(false), censusOk_(false), haveActor_(false),
        census_(0), selectedId_(0), initialUse_(false), postOrdered_(false), unloaded_(false),
        reloaded_(false), reconnectAttempted_(false), reconnected_(false), failed_(false),
        baselineCaptured_(false), createAttempted_(false), createdOk_(false), oldLoad_(0),
        world_(0), lastEpoch_(0), registryComplete_(false), hubLoaded_(false), movedAll_(false),
        returnedAll_(false), nextMotion_(0), finalPreserved_(false), finalUse_(false), initialChecked_(false),
        unloadChecked_(false), reloadChecked_(false), reconnectChecked_(false),
        hostUse_(false),hostReleased_(false),clientOrdered_(false) {
        memset(actor_,0,sizeof(actor_)); memset(&scope_,0,sizeof(scope_)); memset(&created_,0,sizeof(created_));
        memset(clientActor_,0,sizeof(clientActor_));
    }
    virtual void onGameplay(const ScenarioContext& ctx) {
        if (!lifecycle_) return;
        latchActors(ctx);
        if (outside_ && !staged_) { staged_=stageOutside(ctx); if (!staged_) return; }
        captureBaseline(ctx);
        if (ctx.isHost && !createAttempted_ && baselineCaptured_ && !owned_.empty() &&
            fixtureFactoryReady(ctx.gw)) createPlayerBed(ctx);
        captureBaseline(ctx);
    }
    virtual void onStart(const ScenarioContext& ctx) {
        latchActors(ctx);
        if (lifecycle_) {
            captureBaseline(ctx);
            phase(ctx,"required",true,"native_lifecycle");
            if (owned_.empty()) fail(ctx,"setup","complete_player_fixture_missing");
            if (ctx.isHost && !createdOk_) fail(ctx,"setup","prearm_player_bed_missing");
        } else logLine("SCENARIO PROCEDURAL reconnect=NOT_PERFORMED unload=NOT_PERFORMED");
    }
    virtual bool onTick(const ScenarioContext& ctx) {
        latchActors(ctx);
        ProceduralBedBinding selected; memset(&selected,0,sizeof(selected));
        if (ctx.isHost && !ordered_ && haveActor_ && ctx.elapsedMs>=14000 &&
            (!lifecycle_ || ctx.elapsedMs<45000) && pickHubBed(ctx,&selected)) {
            ordered_=orderBed(ctx,selected,actor_);
            if (ordered_) selectedId_=selected.id;
        }
        if (lifecycle_) {
            // Controlled scene-streaming setup moves the complete saved player
            // fixture in BOTH processes, not an unsupported network long warp.
            if (ctx.elapsedMs>=60000 && ctx.elapsedMs<120000 && ctx.elapsedMs>=nextMotion_) {
                nextMotion_=ctx.elapsedMs+2000;
                movedAll_=moveOwned(ctx,true);
            }
            if (ctx.elapsedMs>=120000 && ctx.elapsedMs<150000 && ctx.elapsedMs>=nextMotion_) {
                nextMotion_=ctx.elapsedMs+2000;
                returnedAll_=moveOwned(ctx,false);
            }
            if (ctx.isHost && ctx.elapsedMs>=265000 && hostUse_ && !hostReleased_)
                hostReleased_=releaseHostBed(ctx);
            if (!ctx.isHost && ctx.elapsedMs>=270000 && !clientOrdered_ && reconnected_ &&
                hostUse_ && postOrdered_ && clientActor_[0] && pickHubBed(ctx,&selected,true)) {
                clientOrdered_=orderBed(ctx,selected,clientActor_);
            }
            if (ctx.elapsedMs>=180000 && !reconnectAttempted_ && !ctx.isHost) {
                reconnectAttempted_=true;
                if (ctx.reconnectSession && ctx.reconnectSession())
                    phase(ctx,"reconnect_requested",true,"native_disconnect_connect_accepted");
                else fail(ctx,"reconnect_requested","native_disconnect_connect_rejected");
            }
            if (ctx.isHost && ctx.elapsedMs>=220000 && !postOrdered_ && reconnected_ &&
                pickHubBed(ctx,&selected)) {
                postOrdered_=orderBed(ctx,selected,actor_);
                if (postOrdered_) selectedId_=selected.id;
            }
        }
        if (evidenceDue(ctx.elapsedMs) || (lifecycle_ && ctx.elapsedMs>=340000)) {
            censusOk_=logCensus(ctx);
            bool usingBed=logUse(ctx,lifecycle_ && ctx.elapsedMs>=270000 ? clientActor_ : actor_);
            if (lifecycle_) observeLifecycle(ctx,usingBed);
        }
        if (ctx.elapsedMs<(lifecycle_?340000:60000)) return false;
        passed_=censusOk_ && observed_ && (ctx.isHost ? ordered_ : true);
        if (lifecycle_) passed_=passed_ && !failed_ && initialUse_ && unloaded_ && reloaded_ &&
            reconnected_ && returnedAll_ && (ctx.isHost ? postOrdered_ && createdOk_ : reconnectAttempted_) &&
            hostUse_ && (ctx.isHost?hostReleased_:clientOrdered_) && finalPreserved_ && finalUse_;
        return true;
    }
private:
    // The outside lifecycle fixture is staged in the reported failing cell.
    // Native capture, orders, actor pose and occupant checks remain mandatory;
    // changing an evidence label alone cannot satisfy this scenario.
    i32 fixtureRegionX() const { return outside_?22:20; }
    struct Member {
        unsigned int hand[5];
        float x,y,z,yaw;
    };
    void phase(const ScenarioContext& ctx,const char* name,bool ok,const char* reason) {
        char line[512];
        const unsigned int* participant=lifecycle_ && ctx.elapsedMs>=270000?clientActor_:actor_;
        _snprintf(line,sizeof(line)-1,"SCENARIO PROCLIFE role=%s phase=%s ok=%d world=%I64u load=%u census=%I64u region=%d,32 epoch=%u oldLoad=%u baseline=%u player=%d id=%I64u actor=%u.%u.%u.%u.%u reason=%s t=%lu outside=%d",
            ctx.isHost?"HOST":"JOIN",name,ok?1:0,scope_.world,scope_.loadId,census_,fixtureRegionX(),scope_.epoch,
            oldLoad_,(unsigned int)baseline_.size(),createdOk_?1:0,selectedId_,
            participant[0],participant[1],participant[2],participant[3],participant[4],reason,ctx.elapsedMs,outside_?1:0);
        line[sizeof(line)-1]=0; logLine(line);
    }
    void fail(const ScenarioContext& ctx,const char* name,const char* reason) {
        failed_=true; phase(ctx,name,false,reason);
    }
    void latchActors(const ScenarioContext& ctx) {
        if (haveActor_ && (!lifecycle_ || (!owned_.empty() && clientActor_[0]))) return;
        EntityState sq[129]; unsigned int n=engine::captureSquad(ctx.gw,false,sq,129);
        if (n>=129) return; // A truncated ownership census cannot authorize a move.
        int leader=tabLeaderIdx(sq,n,0);
        if (!haveActor_ && leader>=0) { handFromEntity(sq[leader],actor_); haveActor_=true; }
        if (lifecycle_ && !clientActor_[0]) {
            int client=tabLeaderIdx(sq,n,1);
            if (client>=0) handFromEntity(sq[client],clientActor_);
        }
        if (!lifecycle_ || !owned_.empty()) return;
        int ownLeader=tabLeaderIdx(sq,n,ctx.isHost?0:1);
        if (ownLeader<0) return;
        Character* players[128]; unsigned int count=0;
        if (!fixturePlayers(ctx.gw,players,&count) || count!=n) return;
        handFromEntity(sq[ownLeader],camera_);
        for (unsigned int i=0;i<count;++i) {
            Character* c=players[i]; Member m;
            if (!engine::readObjectHand(c,m.hand) ||
                !engine::readPose(c,&m.x,&m.y,&m.z,&m.yaw) ||
                !procedural::detail::finite(m.x) || !procedural::detail::finite(m.y) ||
                !procedural::detail::finite(m.z)) { owned_.clear(); return; }
            for (unsigned int j=0;j<owned_.size();++j)
                if (memcmp(m.hand,owned_[j].hand,sizeof(m.hand))==0) { owned_.clear(); return; }
            owned_.push_back(m);
        }
        for (unsigned int i=0;i<owned_.size();++i) {
            if (memcmp(owned_[i].hand,camera_,sizeof(camera_))!=0) continue;
            Member first=owned_[0]; owned_[0]=owned_[i]; owned_[i]=first;
            break;
        }
    }
    bool stageOutside(const ScenarioContext& ctx) {
        if (owned_.empty()) return false;
        // Move the real roster to the observed camp approach point. Do not
        // manufacture the native generated furniture this regression measures.
        const float dx=-45630.0f-owned_[0].x,dz=2585.0f-owned_[0].z;
        int rx,rz;
        if (!engine::cellAt(ctx.gw,-45630.0f,2585.0f,&rx,&rz) || rx!=22 || rz!=32) return false;
        bool ok=true;
        for (unsigned int i=0;i<owned_.size();++i) {
            Member& m=owned_[i];
            Character* c=engine::resolveCharByHand(m.hand[3],m.hand[4],m.hand[0],m.hand[1],m.hand[2]);
            float x=m.x+dx,z=m.z+dz,y=1008.0f,ax=0,ay=0,az=0;
            if (!engine::terrainHeightAt(x,z,&y)) y=1008.0f; // observed fixture elevation before streaming
            engine::FurnitureRead furniture; memset(&furniture,0,sizeof(furniture));
            bool ready=c && engine::readFurniture(c,&furniture) && furniture.valid;
            if (ready && furniture.kind) ready=engine::applyFurniture(ctx.gw,c,furniture.furn,furniture.kind,false);
            if (ready) { engine::clearGoals(c); engine::endAction(c); }
            if (!ready || !relocateFixture(c,x,y,z) || !engine::orderMoveTo(c,x,y,z) ||
                !engine::readPos(c,&ax,&ay,&az) || !procedural::detail::finite(ax) ||
                !procedural::detail::finite(ay) || !procedural::detail::finite(az) ||
                (ax-x)*(ax-x)+(ay-y)*(ay-y)+(az-z)*(az-z)>25.0f) ok=false;
        }
        if (!ok) return false;
        for (unsigned int i=0;i<owned_.size();++i) {
            Member& m=owned_[i];
            Character* c=engine::resolveCharByHand(m.hand[3],m.hand[4],m.hand[0],m.hand[1],m.hand[2]);
            if (!c || !engine::readPose(c,&m.x,&m.y,&m.z,&m.yaw)) return false;
        }
        Character* lead=engine::resolveCharByHand(camera_[3],camera_[4],camera_[0],camera_[1],camera_[2]);
        return lead && engine::cameraFocusOn(ctx.gw,lead);
    }
    void captureBaseline(const ScenarioContext& ctx) {
        if (baselineCaptured_) return;
        bool complete=false;
        unsigned int n=engine::captureProceduralBeds(ctx.gw,fixtureRegionX(),32,beds_,PROCEDURAL_OBJECTS_MAX+1,&complete);
        if (!complete || n>PROCEDURAL_OBJECTS_MAX) return;
        baseline_.clear();
        for (unsigned int i=0;i<n;++i)
            if (!beds_[i].procedural && !beds_[i].replica) baseline_.push_back(beds_[i]);
        // Retry an empty pre-arm census, rather than inventing a preservation witness.
        baselineCaptured_=!baseline_.empty();
    }
    void createPlayerBed(const ScenarioContext& ctx) {
        const Member& m=owned_[0]; float y=0;
        const float x=m.x+12.0f,z=m.z+8.0f;
        if (!engine::terrainHeightAt(x,z,&y)) return;
        createAttempted_=true;
        unsigned int h[5]={0,0,0,0,0};
        int rc=engine::placeBuildingAt(ctx.gw,"16845-nodes_otto1.mod",x,y,z,m.yaw,true,h);
        engine::DeedRead deed; memset(&deed,0,sizeof(deed));
        createdOk_=rc==1 && engine::writeDeedByHand(ctx.gw,h,1,"",&deed) && deed.owned &&
            engine::readProceduralBedNative(ctx.gw,engine::resolveObjectByHand(h),&created_) &&
            !created_.procedural && !created_.replica && engine::proceduralBedOrigin(h)==0 &&
            (created_.object.flags&PROC_OWNER) && (created_.object.stateFlags&PROC_COMPLETE);
        phase(ctx,"player_created",createdOk_,createdOk_?"native_completed_player_owned":"native_place_ownership_or_provenance_failed");
    }
    bool moveOwned(const ScenarioContext& ctx,bool away) {
        const char* phaseName=away?"move_away":"move_return";
        Character* players[128]; unsigned int n=0;
        if (owned_.empty() || !fixturePlayers(ctx.gw,players,&n) || n!=owned_.size()) {
            phase(ctx,phaseName,false,"complete_player_fixture_census_changed"); return false;
        }
        // Validate the entire exact hand set before moving ANY actor.
        for (unsigned int i=0;i<n;++i) {
            unsigned int h[5]; unsigned int matches=0;
            if (!engine::readObjectHand(players[i],h)) {
                phase(ctx,phaseName,false,"current_player_hand_unreadable"); return false;
            }
            for (unsigned int j=0;j<owned_.size();++j)
                if (memcmp(h,owned_[j].hand,sizeof(h))==0) ++matches;
            if (matches!=1) {
                phase(ctx,phaseName,false,"exact_player_fixture_membership_changed"); return false;
            }
        }
        bool ok=true;
        for (unsigned int i=0;i<owned_.size();++i) {
            const Member& m=owned_[i];
            Character* c=engine::resolveCharByHand(m.hand[3],m.hand[4],m.hand[0],m.hand[1],m.hand[2]);
            float px=m.x,py=m.y,pz=m.z,ax=0,ay=0,az=0;
            bool applied=false;
            bool ready=c && engine::isPlayerSquad(ctx.gw,c);
            if (away) {
                px+=60000.0f;
                ready=ready && engine::terrainHeightAt(px,pz,&py);
            }
            engine::FurnitureRead furniture; memset(&furniture,0,sizeof(furniture));
            ready=ready && engine::readFurniture(c,&furniture) && furniture.valid;
            if (ready && furniture.kind)
                ready=engine::applyFurniture(ctx.gw,c,furniture.furn,furniture.kind,false);
            if (ready) {
                engine::clearGoals(c);
                engine::endAction(c);
                applied=relocateFixture(c,px,py,pz);
                // Replace the persistent player bed order through native input.
                if (applied) applied=engine::orderMoveTo(c,px,py,pz);
            }
            bool read=c && engine::readPos(c,&ax,&ay,&az);
            bool observed=read && procedural::detail::finite(ax) &&
                procedural::detail::finite(ay) && procedural::detail::finite(az) &&
                (ax-px)*(ax-px)+(ay-py)*(ay-py)+(az-pz)*(az-pz)<=25.0f;
            if (!ready || !applied || !observed) ok=false;
            char line[512];
            _snprintf(line,sizeof(line)-1,"SCENARIO LIFEMOVE role=%s away=%d hand=%u.%u.%u.%u.%u target=%.9g,%.9g,%.9g observed=%.9g,%.9g,%.9g read=%d applied=%d ok=%d t=%lu",
                ctx.isHost?"HOST":"JOIN",away?1:0,m.hand[0],m.hand[1],m.hand[2],m.hand[3],m.hand[4],
                px,py,pz,ax,ay,az,read?1:0,applied?1:0,observed && ready && applied?1:0,ctx.elapsedMs);
            line[sizeof(line)-1]=0; logLine(line);
        }
        // Re-read the WHOLE native set after the last relocation, not just each
        // actor immediately after its own call (which may affect other actors).
        Character* afterPlayers[128]; unsigned int afterCount=0;
        if (!fixturePlayers(ctx.gw,afterPlayers,&afterCount) || afterCount!=n) ok=false;
        for (unsigned int i=0;i<owned_.size();++i) {
            const Member& m=owned_[i]; unsigned int matches=0;
            for (unsigned int j=0;j<afterCount;++j) {
                unsigned int h[5];
                if (engine::readObjectHand(afterPlayers[j],h) &&
                    memcmp(h,m.hand,sizeof(h))==0) ++matches;
            }
            Character* c=engine::resolveCharByHand(m.hand[3],m.hand[4],m.hand[0],m.hand[1],m.hand[2]);
            float px=m.x+(away?60000.0f:0.0f),py=m.y,pz=m.z,ax=0,ay=0,az=0;
            bool target=!away || engine::terrainHeightAt(px,pz,&py);
            bool read=c && engine::readPos(c,&ax,&ay,&az);
            bool observed=matches==1 && target && read && procedural::detail::finite(ax) &&
                procedural::detail::finite(ay) && procedural::detail::finite(az) &&
                (ax-px)*(ax-px)+(ay-py)*(ay-py)+(az-pz)*(az-pz)<=25.0f;
            if (!observed) ok=false;
            char line[512];
            _snprintf(line,sizeof(line)-1,"SCENARIO LIFEMOVEVERIFY role=%s away=%d hand=%u.%u.%u.%u.%u target=%.9g,%.9g,%.9g observed=%.9g,%.9g,%.9g read=%d matches=%u ok=%d t=%lu",
                ctx.isHost?"HOST":"JOIN",away?1:0,m.hand[0],m.hand[1],m.hand[2],m.hand[3],m.hand[4],
                px,py,pz,ax,ay,az,read?1:0,matches,observed?1:0,ctx.elapsedMs);
            line[sizeof(line)-1]=0; logLine(line);
        }
        Character* lead=engine::resolveCharByHand(camera_[3],camera_[4],camera_[0],camera_[1],camera_[2]);
        if (!lead || !engine::cameraFocusOn(ctx.gw,lead)) ok=false;
        phase(ctx,phaseName,ok,ok?"complete_fixture_native_positions_observed":"fixture_native_relocation_or_observation_failed");
        return ok;
    }
    bool orderBed(const ScenarioContext& ctx,const ProceduralBedBinding& selected,const unsigned int participant[5]) {
        Character* c=engine::resolveCharByHand(participant[3],participant[4],participant[0],participant[1],participant[2]);
        RootObject* bed=engine::resolveObjectByHand(selected.hand);
        bool ok=c && bed && engine::isPlayerSquad(ctx.gw,c) &&
            engine::orderWorkAt(c,bed,(int)USE_BED_ORDER);
        if (ok) engine::cameraFocusOn(ctx.gw,c);
        return ok;
    }
    bool releaseHostBed(const ScenarioContext& ctx) {
        if (owned_.empty() || memcmp(owned_[0].hand,actor_,sizeof(actor_))!=0) return false;
        Character* c=engine::resolveCharByHand(actor_[3],actor_[4],actor_[0],actor_[1],actor_[2]);
        engine::FurnitureRead furniture; memset(&furniture,0,sizeof(furniture));
        if (!c || !engine::isPlayerSquad(ctx.gw,c) || !engine::readFurniture(c,&furniture) || !furniture.valid) return false;
        if (furniture.kind && !engine::applyFurniture(ctx.gw,c,furniture.furn,furniture.kind,false)) return false;
        engine::clearGoals(c); engine::endAction(c);
        return engine::orderMoveTo(c,owned_[0].x,owned_[0].y,owned_[0].z);
    }
    bool pickHubBed(const ScenarioContext& ctx, ProceduralBedBinding* out,bool exact=false) {
        // Select real native furniture in the scenario's loaded cell.
        bool complete=false;
        unsigned int n=engine::captureProceduralBeds(ctx.gw,fixtureRegionX(),32,beds_,PROCEDURAL_OBJECTS_MAX+1,&complete);
        if (!complete || n>PROCEDURAL_OBJECTS_MAX || !ctx.queryProceduralBed || (exact && !selectedId_)) return false;
        bool found=false;
        for (unsigned int i=0;i<n;++i) {
            if (!beds_[i].procedural && !beds_[i].replica) continue;
            ProceduralBedBinding binding; memset(&binding,0,sizeof(binding));
            if (!ctx.queryProceduralBed(beds_[i].hand,&binding) || !binding.ready ||
                binding.regionX!=fixtureRegionX() || binding.regionZ!=32) continue;
            if (lifecycle_ && selectedId_ && binding.id==selectedId_) { *out=binding; return true; }
            if (!exact && (!found || binding.id<out->id)) { *out=binding; found=true; }
        }
        return found;
    }
    bool logCensus(const ScenarioContext& ctx) {
        const u64 revision=engine::proceduralOriginRevision();
        bool all=false;
        unsigned int nr=engine::enumerateProceduralRegions(ctx.gw,regions_,PROCEDURAL_OBJECTS_MAX+1,&all);
        registryComplete_=all && nr<=PROCEDURAL_OBJECTS_MAX && nr>0;
        if (nr>PROCEDURAL_OBJECTS_MAX+1) nr=PROCEDURAL_OBJECTS_MAX+1;
        hubLoaded_=false;
        for (unsigned int i=0;i<nr;++i) {
            if (!regions_[i].complete) registryComplete_=false;
            if (regions_[i].x==fixtureRegionX() && regions_[i].z==32) hubLoaded_=true;
        }
        bool ok=registryComplete_;
        // Capture every native loaded region, not a coordinate allowlist or a
        // fixed-size sample. An empty managed set is not this fixture's proof.
        unsigned int enabled=0;
        for (unsigned int i=0;i<nr;++i)
            if (procedural::managedBedRegion(regions_[i].x,regions_[i].z)) regions_[enabled++]=regions_[i];
        nr=enabled; if (!nr) ok=false;
        // Process-wide identity also distinguishes repeated scenario instances.
        static u64 nextCensus=0;
        census_=++nextCensus;
        ProceduralBedBinding censusScope=scope_;
        if (nr) {
            memset(&censusScope,0,sizeof(censusScope));
            if (ctx.queryProceduralRegion)
                ctx.queryProceduralRegion(regions_[0].x,regions_[0].z,&censusScope);
        }
        if (!censusScope.world || !censusScope.loadId) ok=false;
        char line[1536];
        _snprintf(line,sizeof(line)-1,"[procworld] CENSUSBEGIN role=%s world=%I64u load=%u census=%I64u count=%u source=native",
            ctx.isHost?"HOST":"JOIN",censusScope.world,censusScope.loadId,census_,nr);
        line[sizeof(line)-1]=0; logLine(line);
        unsigned int total=0,preservedTotal=0;
        std::vector<ProceduralObject> fixtureObjects;
        ProceduralBedBinding fixtureScope; memset(&fixtureScope,0,sizeof(fixtureScope));
        preservedNow_.clear();
        for (unsigned int r=0;r<nr;++r) {
            ProceduralBedBinding scope; memset(&scope,0,sizeof(scope));
            bool scoped=ctx.queryProceduralRegion && ctx.queryProceduralRegion(regions_[r].x,regions_[r].z,&scope);
            if (!scoped || scope.world!=censusScope.world || scope.loadId!=censusScope.loadId) ok=false;
            bool complete=false;
            unsigned int n=engine::captureProceduralBeds(ctx.gw,regions_[r].x,regions_[r].z,beds_,PROCEDURAL_OBJECTS_MAX+1,&complete);
            if (n>PROCEDURAL_OBJECTS_MAX+1) { n=PROCEDURAL_OBJECTS_MAX+1; complete=false; }
            objects_.clear(); unsigned int preserved=0;
            for (unsigned int i=0;i<n;++i) {
                if (!beds_[i].procedural && !beds_[i].replica) {
                    ++preserved; preservedNow_.push_back(beds_[i]); continue;
                }
                ProceduralObject object=beds_[i].object;
                ProceduralBedBinding binding; memset(&binding,0,sizeof(binding));
                if (!ctx.queryProceduralBed || !ctx.queryProceduralBed(beds_[i].hand,&binding) || !binding.ready ||
                    !scoped || binding.world!=scope.world || binding.loadId!=scope.loadId || binding.epoch!=scope.epoch ||
                    binding.regionX!=regions_[r].x || binding.regionZ!=regions_[r].z) complete=false;
                object.id=binding.id; objects_.push_back(object);
            }
            complete=complete && all && regions_[r].complete && scoped && scope.ready && objects_.size()<=PROCEDURAL_OBJECTS_MAX;
            if (!complete) ok=false;
            total+=(unsigned int)objects_.size();
            preservedTotal+=preserved;
            _snprintf(line,sizeof(line)-1,"[procworld] CATALOG role=%s world=%I64u load=%u census=%I64u region=%d,%d epoch=%u complete=%d count=%u preserved=%u digest=%08x source=native",
                ctx.isHost?"HOST":"JOIN",scope.world,scope.loadId,census_,regions_[r].x,regions_[r].z,scope.epoch,complete?1:0,
                (unsigned int)objects_.size(),preserved,procedural::snapshotDigest(objects_));
            line[sizeof(line)-1]=0; logLine(line);
            for (unsigned int i=0;i<objects_.size();++i) {
                const ProceduralObject& o=objects_[i];
                _snprintf(line,sizeof(line)-1,"[procworld] OBJECT role=%s world=%I64u load=%u census=%I64u region=%d,%d epoch=%u id=%I64u category=%u sid='%s' pos=%.9g,%.9g,%.9g yaw=%.9g rotationX=%.9g rotationY=%.9g rotationZ=%.9g rotationW=%.9g floor=%d flags=%u function=%u state=%u progress=%.9g owner='%s' town='%s' townPos=%.9g,%.9g,%.9g inside='%s' insidePos=%.9g,%.9g,%.9g source=native",
                    ctx.isHost?"HOST":"JOIN",scope.world,scope.loadId,census_,regions_[r].x,regions_[r].z,scope.epoch,o.id,(unsigned int)o.category,o.sid,
                    o.x,o.y,o.z,o.heading,o.rotationX,o.rotationY,o.rotationZ,o.rotationW,o.floor,(unsigned int)o.flags,o.buildingFunction,o.stateFlags,o.constructionProgress,
                    o.ownerSid,o.townSid,o.townX,o.townY,o.townZ,o.insideSid,o.insideX,o.insideY,o.insideZ);
                line[sizeof(line)-1]=0; logLine(line);
            }
            for (unsigned int i=0;i<n;++i) {
                if (beds_[i].procedural || beds_[i].replica) continue;
                const ProceduralObject& o=beds_[i].object; const unsigned int* h=beds_[i].hand;
                _snprintf(line,sizeof(line)-1,"[procworld] PRESERVED role=%s world=%I64u load=%u census=%I64u region=%d,%d epoch=%u hand=%u.%u.%u.%u.%u category=%u sid='%s' pos=%.9g,%.9g,%.9g yaw=%.9g rotationX=%.9g rotationY=%.9g rotationZ=%.9g rotationW=%.9g floor=%d flags=%u function=%u state=%u progress=%.9g owner='%s' town='%s' townPos=%.9g,%.9g,%.9g inside='%s' insidePos=%.9g,%.9g,%.9g source=native",
                    ctx.isHost?"HOST":"JOIN",scope.world,scope.loadId,census_,regions_[r].x,regions_[r].z,scope.epoch,
                    h[0],h[1],h[2],h[3],h[4],(unsigned int)o.category,o.sid,o.x,o.y,o.z,o.heading,
                    o.rotationX,o.rotationY,o.rotationZ,o.rotationW,o.floor,(unsigned int)o.flags,o.buildingFunction,o.stateFlags,o.constructionProgress,
                    o.ownerSid,o.townSid,o.townX,o.townY,o.townZ,o.insideSid,o.insideX,o.insideY,o.insideZ);
                line[sizeof(line)-1]=0; logLine(line);
            }
            _snprintf(line,sizeof(line)-1,"[procworld] REGIONEND role=%s world=%I64u load=%u census=%I64u region=%d,%d epoch=%u count=%u preserved=%u source=native",
                ctx.isHost?"HOST":"JOIN",scope.world,scope.loadId,census_,regions_[r].x,regions_[r].z,scope.epoch,(unsigned int)objects_.size(),preserved);
            line[sizeof(line)-1]=0; logLine(line);
            if (regions_[r].x==fixtureRegionX() && regions_[r].z==32) {
                fixtureObjects=objects_; fixtureScope=scope;
            }
        }
        if (revision!=engine::proceduralOriginRevision()) ok=false;
        _snprintf(line,sizeof(line)-1,"[procworld] CENSUSEND role=%s world=%I64u load=%u census=%I64u complete=%d count=%u beds=%u preserved=%u source=native",
            ctx.isHost?"HOST":"JOIN",censusScope.world,censusScope.loadId,census_,ok?1:0,nr,total,preservedTotal);
        line[sizeof(line)-1]=0; logLine(line);
        if (ok && hubLoaded_) { scope_=fixtureScope; objects_=fixtureObjects; }
        if (ok && hubLoaded_ && lifecycle_) checkLogicalContinuity(ctx);
        return ok;
    }
    bool logUse(const ScenarioContext& ctx,const unsigned int participant[5]) {
        if (!haveActor_ || !participant[0]) return false;
        Character* c=engine::resolveCharByHand(participant[3],participant[4],participant[0],participant[1],participant[2]);
        engine::FurnitureRead furniture; memset(&furniture,0,sizeof(furniture));
        bool read=c && engine::readFurniture(c,&furniture) && furniture.valid;
        ProceduralBedBinding binding; memset(&binding,0,sizeof(binding));
        bool mapped=read && furniture.kind==1 && ctx.queryProceduralBed && ctx.queryProceduralBed(furniture.furn,&binding) && binding.ready;
        unsigned int occupant[5]={0,0,0,0,0};
        bool occupied=mapped && nativeOccupant(furniture.furn,occupant) && memcmp(participant,occupant,sizeof(actor_))==0;
        int pose=c ? engine::readTaskKey(c) : -1;
        float ax=0,ay=0,az=0;
        engine::ProceduralBedRead native; memset(&native,0,sizeof(native));
        bool positions=c && engine::readPos(c,&ax,&ay,&az) && mapped &&
            engine::readProceduralBedNative(ctx.gw,engine::resolveObjectByHand(furniture.furn),&native);
        bool good=occupied && engine::taskIsBedPose(pose) && positions &&
            procedural::detail::finite(ax) && procedural::detail::finite(ay) && procedural::detail::finite(az) &&
            fabs(ax-native.object.x)<=0.01f && fabs(ay-native.object.y)<=0.01f && fabs(az-native.object.z)<=0.01f;
        if (lifecycle_ && good) {
            bool current=censusOk_ && hubLoaded_ && binding.world==scope_.world &&
                binding.loadId==scope_.loadId && binding.epoch==scope_.epoch;
            bool descriptor=false;
            for (unsigned int i=0;i<objects_.size();++i)
                if (objects_[i].id==binding.id && procedural::sameObject(objects_[i],native.object)) {
                    descriptor=true; break;
                }
            current=current && descriptor;
            if (current && !selectedId_ && ctx.elapsedMs<45000) selectedId_=binding.id;
            // A regenerated layout may retire the initial bed. Latch the HOST's
            // first actual post-reconnect bed once; the JOIN must then use it.
            if (current && !ctx.isHost && reconnected_ && ctx.elapsedMs>=220000 && !postOrdered_) {
                selectedId_=binding.id; postOrdered_=true;
            }
            good=current && selectedId_ && binding.id==selectedId_;
        }
        if (good) {
            observed_=true;
            if (!lifecycle_ || ctx.elapsedMs<60000 || ctx.elapsedMs>=120000)
                engine::cameraFocusOn(ctx.gw,c);
        }
        char line[768];
        _snprintf(line,sizeof(line)-1,"SCENARIO PROCBEDUSE role=%s world=%I64u load=%u census=%I64u region=%d,%d epoch=%u id=%I64u actor=%u.%u.%u.%u.%u actorPos=%.9g,%.9g,%.9g bedPos=%.9g,%.9g,%.9g inBed=%d pose=%d occupant=%u.%u.%u.%u.%u ok=%d t=%lu",
            ctx.isHost?"HOST":"JOIN",binding.world,binding.loadId,census_,binding.regionX,binding.regionZ,binding.epoch,binding.id,
            participant[0],participant[1],participant[2],participant[3],participant[4],ax,ay,az,native.object.x,native.object.y,native.object.z,
            read && furniture.kind==1?1:0,pose,occupant[0],occupant[1],occupant[2],occupant[3],occupant[4],good?1:0,ctx.elapsedMs);
        line[sizeof(line)-1]=0; logLine(line);
        return good;
    }
    void checkLogicalContinuity(const ScenarioContext& ctx) {
        if (!world_) world_=scope_.world;
        else if (world_!=scope_.world) {
            if (!failed_) fail(ctx,"scope","host_world_token_changed");
            return;
        }
        if (lastEpoch_) {
            bool changed=previous_.size()!=objects_.size();
            for (unsigned int i=0;i<previous_.size();++i) {
                bool found=false;
                for (unsigned int j=0;j<objects_.size();++j) {
                    if (!procedural::sameObject(previous_[i],objects_[j])) continue;
                    found=true;
                    if (previous_[i].id!=objects_[j].id && !failed_)
                        fail(ctx,"identity","unchanged_descriptor_changed_logical_id");
                    break;
                }
                if (!found) changed=true;
            }
            if ((scope_.epoch<lastEpoch_ || (changed && scope_.epoch<=lastEpoch_)) && !failed_)
                fail(ctx,"identity","changed_layout_without_newer_epoch");
        }
        previous_=objects_;
        lastEpoch_=scope_.epoch;
    }
    bool preserved(const ScenarioContext& ctx) {
        if (!baselineCaptured_ || baseline_.empty() || preservedNow_.empty()) return false;
        for (unsigned int i=0;i<baseline_.size();++i) {
            bool found=false;
            for (unsigned int j=0;j<preservedNow_.size();++j)
                if (memcmp(baseline_[i].hand,preservedNow_[j].hand,sizeof(baseline_[i].hand))==0 &&
                    procedural::sameNativeState(baseline_[i].object,preservedNow_[j].object)) { found=true; break; }
            if (!found) return false;
        }
        if (ctx.isHost) {
            if (!createdOk_) return false;
            bool found=false;
            for (unsigned int j=0;j<preservedNow_.size();++j)
                if (memcmp(created_.hand,preservedNow_[j].hand,sizeof(created_.hand))==0 &&
                    procedural::sameNativeState(created_.object,preservedNow_[j].object) &&
                    engine::proceduralBedOrigin(preservedNow_[j].hand)==0) { found=true; break; }
            if (!found) return false;
            engine::DeedRead deed; memset(&deed,0,sizeof(deed));
            if (!engine::readDeedByHand(ctx.gw,created_.hand,&deed) || !deed.owned) return false;
        }
        return true;
    }
    void observeLifecycle(const ScenarioContext& ctx,bool usingBed) {
        // Armed wall-clock phases: sleep 14..45 s; away 60..120 s;
        // reload 120..180 s; reconnect 180..220 s; host sleep then release;
        // JOIN's own native order at 270 s uses the same bed; final 340 s.
        if (ctx.elapsedMs>=14000 && ctx.elapsedMs<45000 && usingBed) initialUse_=true;
        if (ctx.elapsedMs>=45000 && !initialChecked_) {
            initialChecked_=true;
            if (!initialUse_) fail(ctx,"initial_use","native_sleep_not_observed_14_to_45s");
        }
        if (ctx.elapsedMs>=60000 && ctx.elapsedMs<120000 && !unloaded_ &&
            movedAll_ && registryComplete_ && !hubLoaded_ && censusOk_) {
            unloaded_=true; phase(ctx,"unloaded",true,"complete_native_registry_excludes_fixture_cell");
        }
        if (ctx.elapsedMs>=120000 && !unloadChecked_) {
            unloadChecked_=true;
            if (!unloaded_) fail(ctx,"unloaded","fixture_cell_absence_not_observed_during_away_dwell");
        }
        if (ctx.elapsedMs>=120000 && ctx.elapsedMs<180000 && !reloaded_ && unloaded_ &&
            returnedAll_ && censusOk_ && hubLoaded_ && scope_.ready && scope_.world==world_) {
            reloaded_=true; oldLoad_=scope_.loadId;
            phase(ctx,"reloaded",true,"complete_native_census_and_region_ready");
            if (!preserved(ctx)) fail(ctx,"reload_preservation","native_hand_descriptor_or_player_provenance_changed");
        }
        if (ctx.elapsedMs>=180000 && !reloadChecked_) {
            reloadChecked_=true;
            if (!reloaded_) fail(ctx,"reloaded","native_reload_reconciliation_not_observed");
        }
        if (ctx.elapsedMs>=180000 && !reconnected_ && reloaded_ && censusOk_ && hubLoaded_ &&
            scope_.ready && scope_.world==world_ && oldLoad_ && scope_.loadId && scope_.loadId!=oldLoad_ &&
            (ctx.isHost || reconnectAttempted_)) {
            reconnected_=true; phase(ctx,"reconnected",true,"new_native_load_same_host_world");
        }
        if (ctx.elapsedMs>=220000 && !reconnectChecked_) {
            reconnectChecked_=true;
            if (!reconnected_) fail(ctx,"reconnected","new_ready_native_load_scope_not_observed");
        }
        if (ctx.elapsedMs>=220000 && ctx.elapsedMs<265000 && reconnected_ && usingBed && !hostUse_) {
            hostUse_=true; phase(ctx,"host_use",true,"native_host_player_occupied_current_bed");
        }
        if (ctx.elapsedMs>=340000) {
            finalPreserved_=reconnected_ && censusOk_ && hubLoaded_ && scope_.ready && preserved(ctx);
            if (!finalPreserved_) fail(ctx,"preserved","final_native_hand_descriptor_or_player_provenance_failed");
            else phase(ctx,"preserved",true,"baseline_full_native_hands_and_descriptors_unchanged");
            finalUse_=finalPreserved_ && hostUse_ && usingBed && (ctx.isHost?hostReleased_:clientOrdered_);
            if (!finalUse_) fail(ctx,"post_reconnect_use","final_current_census_native_same_bed_sleep_missing");
            else phase(ctx,"post_reconnect_use",true,"current_native_actor_occupant_pose_and_same_logical_bed");
        }
    }
    bool outside_,staged_;
    bool lifecycle_,ordered_,observed_,censusOk_,haveActor_;
    unsigned int actor_[5];
    unsigned int camera_[5];
    unsigned int clientActor_[5];
    u64 census_,selectedId_;
    bool initialUse_,postOrdered_,unloaded_,reloaded_,reconnectAttempted_,reconnected_,failed_;
    bool baselineCaptured_,createAttempted_,createdOk_;
    u32 oldLoad_;
    u64 world_;
    u32 lastEpoch_;
    bool registryComplete_,hubLoaded_,movedAll_,returnedAll_;
    unsigned long nextMotion_;
    bool finalPreserved_,finalUse_,initialChecked_,unloadChecked_,reloadChecked_,reconnectChecked_;
    bool hostUse_,hostReleased_,clientOrdered_;
    ProceduralBedBinding scope_;
    engine::ProceduralBedRead created_;
    std::vector<Member> owned_;
    std::vector<engine::ProceduralBedRead> baseline_,preservedNow_;
    std::vector<ProceduralObject> previous_;
    engine::ProceduralRegionRead regions_[PROCEDURAL_OBJECTS_MAX+1];
    engine::ProceduralBedRead beds_[PROCEDURAL_OBJECTS_MAX+1];
    std::vector<ProceduralObject> objects_;
};
} // namespace
Scenario* makeProceduralScenario(const std::string& name) {
    if (name=="procedural_beds") return new ProceduralScenario(false);
    if (name=="procedural_beds_outside_lifecycle") return new ProceduralScenario(true,true);
    return name=="procedural_beds_lifecycle" ? new ProceduralScenario(true) : 0;
}
} // namespace coop
