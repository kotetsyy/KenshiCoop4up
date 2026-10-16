#include "ScenarioSupport.h"
#include "../game/EngineEnvironmentItems.h"
#include "../game/EngineProcedural.h"
#include "../core/EnvironmentItemsState.h"
#include <core/Functions.h>
#include <kenshi/PlayerInterface.h>
#include <algorithm>
#include <windows.h>

namespace coop { namespace {
struct FixtureActor { unsigned int hand[5];float x,y,z; };
bool fixtureActors(GameWorld* world,Character** out,unsigned* count) {
    *count=0;
    __try {
        if (!world || !world->player) return false;
        unsigned n=(unsigned)world->player->playerCharacters.size();if (!n || n>128) return false;
        for (unsigned i=0;i<n;++i) {
            out[i]=world->player->playerCharacters[i];if (!out[i] || !engine::isPlayerSquad(world,out[i])) return false;
            for (unsigned j=0;j<i;++j) if (out[i]==out[j]) return false;
        }
        *count=n;return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
typedef void (__fastcall* RelocateFn)(Character*,const Ogre::Vector3*);
bool callRelocation(RelocateFn fn,Character* c,const Ogre::Vector3& delta) {
    __try { fn(c,&delta);return true; }
    __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool invokeRelocation(Character* c,float x,float y,float z) {
    static RelocateFn fn=(RelocateFn)KenshiLib::GetRealAddress(&Character::relocationTeleport);
    float ax,ay,az;if (!fn || !engine::readPos(c,&ax,&ay,&az)) return false;
    Ogre::Vector3 delta(x-ax,y-ay,z-az);
    return callRelocation(fn,c,delta);
}
bool savedIdentity(const engine::GroundItemIdentityRow& a,const engine::GroundItemIdentityRow& b) {
    return a.savedInput && b.savedInput && a.instanceID[0] &&
        !strcmp(a.instanceID,b.instanceID) && !strcmp(a.sourceName,b.sourceName) && !strcmp(a.instanceRef,b.instanceRef) && !strcmp(a.baseSID,b.baseSID);
}
class EnvironmentItemsScenario:public TimedScenario {
public:
    explicit EnvironmentItemsScenario(bool lifecycle):TimedScenario(lifecycle?"environment_items_lifecycle":"environment_items",500),
        lifecycle_(lifecycle),roomPinned_(false),baseline_(false),mutated_(false),changed_(false),unloaded_(false),returned_(false),
        reconnectAttempted_(false),reconnected_(false),failed_(false),initialEpoch_(0),initialLoad_(0),nextTravel_(0),samples_(0),postSamples_(0) { memset(&room_,0,sizeof(room_)); }
    virtual void onGameplay(const ScenarioContext& ctx) { pin(ctx); }
    virtual void onStart(const ScenarioContext& ctx) {
        if (lifecycle_) actors_.clear();pin(ctx); // latch the actual post-arm player handles
        logLine("SCENARIO ENVIRONMENT required=native_nonplayer_merchant_room_with_saved_unknown_ground_sentinel no_mock=1");
    }
    virtual bool onTick(const ScenarioContext& ctx) {
        pin(ctx);
        if (!roomPinned_ || !ctx.queryEnvironmentRegion || !ctx.queryEnvironmentItem || !ctx.queryEnvironmentScope) {
            if (ctx.elapsedMs>=15000) failed_=true;
        } else {
            if (lifecycle_) lifecycle(ctx);
            const bool away=lifecycle_ && ctx.elapsedMs>=40000 && ctx.elapsedMs<80000;
            if (evidenceDue(ctx.elapsedMs) && !away) sample(ctx);
            if (ctx.isHost && baseline_ && !mutated_ && ctx.elapsedMs>=10000 && ctx.elapsedMs<30000) mutate(ctx);
        }
        const unsigned long duration=lifecycle_?(ctx.isHost?200000ul:190000ul):(ctx.isHost?70000ul:60000ul);
        if (ctx.elapsedMs<duration) return false;
        passed_=!failed_ && baseline_ && changed_ && postSamples_>=8 && !protected_.empty() && (ctx.isHost?mutated_:true);
        if (lifecycle_) passed_=passed_ && unloaded_ && returned_ && reconnected_ && (ctx.isHost || reconnectAttempted_);
        return true;
    }
private:
    bool lifecycle_,roomPinned_,baseline_,mutated_,changed_,unloaded_,returned_,reconnectAttempted_,reconnected_,failed_;
    EnvironmentAnchor room_;i32 x_,z_;u32 initialEpoch_,initialLoad_;unsigned long nextTravel_;unsigned samples_,postSamples_;
    std::vector<FixtureActor> actors_;
    std::vector<engine::GroundItemIdentityRow> protected_;
    void phase(const ScenarioContext& ctx,const char* name,bool ok) {
        char b[256];_snprintf(b,sizeof(b)-1,"SCENARIO ENVIRONMENT PHASE side=%s name=%s ok=%d t=%lu",ctx.isHost?"HOST":"JOIN",name,ok?1:0,ctx.elapsedMs);b[sizeof(b)-1]=0;logLine(b);
    }
    unsigned stage(unsigned long t) const { return !lifecycle_?(t<10000?0:1):(t<10000?0:t<40000?1:t<80000?2:t<135000?3:4); }
    void pin(const ScenarioContext& ctx) {
        if (!roomPinned_) roomPinned_=engine::environmentMerchantRoom(ctx.gw,&room_,&x_,&z_);
        if (!lifecycle_ || !actors_.empty()) return;
        Character* chars[128];unsigned n=0;if (!fixtureActors(ctx.gw,chars,&n)) return;
        std::vector<FixtureActor> captured;
        for (unsigned i=0;i<n;++i) {
            FixtureActor actor;
            if (!engine::readObjectHand(chars[i],actor.hand) || !engine::readPos(chars[i],&actor.x,&actor.y,&actor.z)) return;
            captured.push_back(actor);
        }
        actors_.swap(captured);
    }
    bool protectedState(const ScenarioContext& ctx,const std::vector<engine::EnvironmentNativeRow>& managed,bool capture) {
        static engine::GroundItemRow rows[ENVIRONMENT_ITEMS_MAX];engine::GroundItemCensusStatus status;
        unsigned n=engine::captureLoadedGroundItems(ctx.gw,rows,ENVIRONMENT_ITEMS_MAX,&status);
        if (!status.complete || n>=ENVIRONMENT_ITEMS_MAX) return false;
        std::vector<engine::GroundItemIdentityRow> unknown;
        for (unsigned i=0;i<n;++i) {
            if (rows[i].zoneX!=x_ || rows[i].zoneY!=z_) continue;
            bool known=false;for (unsigned j=0;j<managed.size();++j) if (!memcmp(rows[i].hand,managed[j].hand,20)) { known=true;break; }
            if (known) continue;
            engine::GroundItemIdentityRow identity;
            if (!engine::readGroundItemIdentity(rows[i].hand,&identity) || identity.identityTruncated) return false;
            if (identity.savedInput && identity.instanceID[0]) unknown.push_back(identity);
        }
        if (capture) { protected_=unknown;return !protected_.empty(); }
        for (unsigned i=0;i<protected_.size();++i) {
            unsigned found=0;
            for (unsigned j=0;j<unknown.size();++j) if (savedIdentity(protected_[i],unknown[j])) {
                const engine::GroundItemIdentityRow& a=protected_[i];const engine::GroundItemIdentityRow& b=unknown[j];
                if (memcmp(&a.item,&b.item,sizeof(a.item)) || a.condition!=b.condition || a.charges!=b.charges || a.fullCharge!=b.fullCharge) return false;
                ++found;
            }
            if (found!=1) return false;
        }
        return !protected_.empty();
    }
    void sample(const ScenarioContext& ctx) {
        std::vector<engine::EnvironmentNativeRow> native;
        if (!engine::captureEnvironmentItems(ctx.gw,x_,z_,native)) return;
        u64 world=0;u32 load=0,scope=0,epoch=0,digest=0;unsigned count=0;
        const bool ready=ctx.queryEnvironmentScope(x_,z_,&world,&load,&scope) && ctx.queryEnvironmentRegion(x_,z_,&epoch,&digest,&count);
        bool complete=ready && count==native.size();unsigned roomCount=0;
        std::vector<u32> fingerprints;
        for (unsigned i=0;i<native.size();++i) {
            u64 id=0;u32 itemEpoch=0,itemDigest=0;
            bool bound=ctx.queryEnvironmentItem(native[i].hand,&id,&itemEpoch,&itemDigest);
            complete=complete && bound && itemEpoch==epoch && itemDigest==digest;
            if (environment::sameAnchor(room_,native[i].object.parent)) ++roomCount;
            const EnvironmentItemObject& o=native[i].object;u32 fingerprint=environment::nativeFingerprint(o);fingerprints.push_back(fingerprint);
            char line[2048];_snprintf(line,sizeof(line)-1,
                "SCENARIO ENVIRONMENT ITEM side=%s stage=%u t=%lu world=%I64u load=%u scope=%u region=%d,%d epoch=%u id=%I64u bound=%d native=%08x hand=%u,%u,%u,%u,%u sid='%s' type=%u qty=%u condition=%.9g charges=%.9g/%.9g manufacturer='%s' material='%s' grade=%u pos=%.9g,%.9g,%.9g rot=%.9g,%.9g,%.9g,%.9g floor=%d flags=%u parent='%s' parentSID='%s' owner='%s' persistent='%s' ownerSID='%s' ownerStream=%u ownerKey=%u,%u,%u,%u,%u persistentSID='%s' persistentStream=%u persistentKey=%u,%u,%u,%u,%u",
                ctx.isHost?"HOST":"JOIN",stage(ctx.elapsedMs),ctx.elapsedMs,world,load,scope,x_,z_,epoch,id,bound?1:0,fingerprint,
                native[i].hand[0],native[i].hand[1],native[i].hand[2],native[i].hand[3],native[i].hand[4],o.item.stringID,o.item.itemType,
                (unsigned)o.item.quantity,o.condition,o.charges,o.fullCharge,o.item.manufacturer,o.item.material,(unsigned)o.item.level,
                o.x,o.y,o.z,o.rotationW,o.rotationX,o.rotationY,o.rotationZ,o.floor,o.flags,o.parent.instance,o.parent.sid,o.properOwner.instance,o.persistentOwner.instance,
                o.properOwner.sid,o.properOwner.actorOwner,o.properOwner.actorKey[0],o.properOwner.actorKey[1],o.properOwner.actorKey[2],o.properOwner.actorKey[3],o.properOwner.actorKey[4],
                o.persistentOwner.sid,o.persistentOwner.actorOwner,o.persistentOwner.actorKey[0],o.persistentOwner.actorKey[1],o.persistentOwner.actorKey[2],o.persistentOwner.actorKey[3],o.persistentOwner.actorKey[4]);
            line[sizeof(line)-1]=0;logLine(line);
        }
        bool preserved=false;
        if (complete) preserved=protectedState(ctx,native,!baseline_);
        if (!baseline_ && complete && roomCount && preserved && ctx.elapsedMs<10000) { baseline_=true;initialEpoch_=epoch;initialLoad_=load;phase(ctx,"baseline",true); }
        if (baseline_ && epoch>initialEpoch_) changed_=true;
        if (baseline_ && complete && !preserved) failed_=true;
        if (complete) ++samples_;
        if (complete && changed_ && preserved && ctx.elapsedMs>=20000) ++postSamples_;
        if (lifecycle_ && reconnectAttempted_ && complete && ctx.peerReady && load!=initialLoad_ && ctx.elapsedMs>=150000) reconnected_=true;
        if (lifecycle_ && ctx.isHost && ctx.elapsedMs>=150000 && complete && ctx.peerReady) reconnected_=true;
        std::sort(fingerprints.begin(),fingerprints.end());u32 content=fnv1aInit();
        for (unsigned i=0;i<fingerprints.size();++i) content=procedural::detail::integer(content,fingerprints[i]);
        char line[640];_snprintf(line,sizeof(line)-1,
            "SCENARIO ENVIRONMENT SAMPLE side=%s stage=%u t=%lu world=%I64u load=%u scope=%u region=%d,%d room='%s' epoch=%u count=%u nativeCount=%u digest=%08x nativeDigest=%08x ready=%d protected=%u preserved=%d",
            ctx.isHost?"HOST":"JOIN",stage(ctx.elapsedMs),ctx.elapsedMs,world,load,scope,x_,z_,room_.instance,epoch,count,(unsigned)native.size(),digest,content,
            complete?1:0,(unsigned)protected_.size(),preserved?1:0);line[sizeof(line)-1]=0;logLine(line);
    }
    void mutate(const ScenarioContext& ctx) {
        std::vector<engine::EnvironmentNativeRow> native;if (!engine::captureEnvironmentItems(ctx.gw,x_,z_,native)) return;
        u64 world=0;u32 load=0,scope=0;if (!ctx.queryEnvironmentScope(x_,z_,&world,&load,&scope)) return;
        for (unsigned i=0;i<native.size();++i) {
            if (!environment::sameAnchor(native[i].object.parent,room_)) continue;
            u64 id=0;u32 epoch=0,digest=0;
            if (!ctx.queryEnvironmentItem(native[i].hand,&id,&epoch,&digest) || !engine::environmentSafeToRemove(ctx.gw,native[i].hand)) continue;
            if (!engine::removeEnvironmentItem(ctx.gw,native[i].hand)) continue;
            mutated_=true;
            char line[320];_snprintf(line,sizeof(line)-1,"SCENARIO ENVIRONMENT MUTATE side=HOST t=%lu world=%I64u load=%u scope=%u region=%d,%d epoch=%u id=%I64u nativeRemoved=1",ctx.elapsedMs,world,load,scope,x_,z_,epoch,id);
            line[sizeof(line)-1]=0;logLine(line);return;
        }
    }
    bool travel(const ScenarioContext& ctx,bool away) {
        Character* players[128];unsigned n=0;if (actors_.empty() || !fixtureActors(ctx.gw,players,&n) || n!=actors_.size()) return false;
        // Check the whole exact saved fixture before moving any body.
        for (unsigned i=0;i<n;++i) {
            unsigned int h[5];if (!engine::readObjectHand(players[i],h)) return false;
            unsigned matches=0;for (unsigned j=0;j<actors_.size();++j) if (!memcmp(h,actors_[j].hand,20)) ++matches;
            if (matches!=1) return false;
        }
        bool ok=true;
        for (unsigned i=0;i<actors_.size();++i) {
            const FixtureActor& a=actors_[i];Character* c=engine::resolveCharByHand(a.hand[3],a.hand[4],a.hand[0],a.hand[1],a.hand[2]);
            float x=a.x+(away?60000.0f:0),y=a.y,z=a.z,ax=0,ay=0,az=0;
            engine::FurnitureRead furniture;memset(&furniture,0,sizeof(furniture));
            bool valid=c && engine::isPlayerSquad(ctx.gw,c) && (!away || engine::terrainHeightAt(x,z,&y)) &&
                engine::readFurniture(c,&furniture) && furniture.valid && !furniture.kind;
            if (valid) { engine::clearGoals(c);engine::endAction(c);valid=invokeRelocation(c,x,y,z) && engine::orderMoveTo(c,x,y,z); }
            valid=valid && engine::readPos(c,&ax,&ay,&az) && (ax-x)*(ax-x)+(ay-y)*(ay-y)+(az-z)*(az-z)<=25;
            ok=ok && valid;
        }
        // Later relocations must not have displaced an earlier fixture member.
        for (unsigned i=0;i<actors_.size();++i) {
            const FixtureActor& a=actors_[i];Character* c=engine::resolveCharByHand(a.hand[3],a.hand[4],a.hand[0],a.hand[1],a.hand[2]);
            float x=a.x+(away?60000.0f:0),y=a.y,ax=0,ay=0,az=0;
            const bool terrain=!away || engine::terrainHeightAt(x,a.z,&y);
            ok=terrain && engine::readPos(c,&ax,&ay,&az) && (ax-x)*(ax-x)+(ay-y)*(ay-y)+(az-a.z)*(az-a.z)<=25 && ok;
        }
        Character* lead=engine::leader(ctx.gw);ok=ok && lead && engine::cameraFocusOn(ctx.gw,lead);
        phase(ctx,away?"travel_away":"travel_return",ok);return ok;
    }
    void lifecycle(const ScenarioContext& ctx) {
        if (ctx.elapsedMs>=40000 && ctx.elapsedMs<80000 && ctx.elapsedMs>=nextTravel_) { nextTravel_=ctx.elapsedMs+3000;travel(ctx,true); }
        if (ctx.elapsedMs>=80000 && ctx.elapsedMs<115000 && ctx.elapsedMs>=nextTravel_) { nextTravel_=ctx.elapsedMs+3000;returned_=travel(ctx,false); }
        engine::ProceduralRegionRead regions[256];bool complete=false;
        unsigned n=engine::enumerateProceduralRegions(ctx.gw,regions,256,&complete);bool present=false;
        for (unsigned i=0;i<n && i<256;++i) if (regions[i].x==x_ && regions[i].z==z_) present=true;
        if (!unloaded_ && ctx.elapsedMs>=45000 && ctx.elapsedMs<80000 && complete && n<256 && !present) { unloaded_=true;phase(ctx,"native_region_unloaded",true); }
        if (!ctx.isHost && !reconnectAttempted_ && ctx.elapsedMs>=135000) {
            reconnectAttempted_=true;bool ok=ctx.reconnectSession && ctx.reconnectSession();phase(ctx,"native_reconnect_requested",ok);if (!ok) failed_=true;
        }
    }
};
} // namespace
Scenario* makeEnvironmentItemsScenario(const std::string& name) {
    if (name=="environment_items") return new EnvironmentItemsScenario(false);
    if (name=="environment_items_lifecycle") return new EnvironmentItemsScenario(true);
    return 0;
}
}
