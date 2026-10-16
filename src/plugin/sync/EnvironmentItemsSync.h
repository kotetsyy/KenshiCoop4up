#ifndef COOP_ENVIRONMENT_ITEMS_SYNC_H
#define COOP_ENVIRONMENT_ITEMS_SYNC_H
#include "../core/EnvironmentItemsState.h"
#include "../game/EngineEnvironmentItems.h"
#include <deque>
namespace coop {
class NetLink;class Inbound;
class EnvironmentItemsSync {
public:
    EnvironmentItemsSync();
    void reset();
    void retirePeer(u32 owner);
    void update(GameWorld*,NetLink&,Inbound&,bool host,u32 owner,u64 world,u32 load,bool live);
    bool allLoadedRegionsReady() const;
    bool peerReady(u32 owner) const;
    bool regionReady(i32 x,i32 z) const;
    // Native scenario receives the actual reconciled authoritative set.
    bool queryRegion(i32 x,i32 z,u32* epoch,u32* digest,unsigned* count) const;
    bool queryItem(const unsigned int hand[5],u64* id,u32* epoch,u32* digest) const;
    bool queryScope(i32 x,i32 z,u64* world,u32* load,u32* scope) const;
private:
    typedef std::pair<i32,i32> Key;
    struct Hand { unsigned int value[5];Hand(){ memset(value,0,sizeof(value)); } };
    struct Region {
        u32 epoch,scopeId,lastRequest,lastSend;
        bool loaded,complete,ready,retiring;
        environment::Objects objects;
        environment::Snapshot snapshot;
        procedural::NativeCensusFence nativeCensus;
        std::map<u64,Hand> bindings;
        u32 requestedScope[MAX_PLAYERS];
        Region():loaded(false),complete(false),ready(false),retiring(false),epoch(0),scopeId(0),lastRequest(0),lastSend(0) { memset(requested,0,sizeof(requested));memset(acknowledged,0,sizeof(acknowledged));memset(requestedScope,0,sizeof(requestedScope)); }
        u32 requested[MAX_PLAYERS],acknowledged[MAX_PLAYERS];
    };
    std::map<Key,Region> regions_;
    procedural::PeerScope scopes_[MAX_PLAYERS];
    procedural::NativeCensusFence nativeCensus_;
    u64 world_,nextId_;u32 load_,owner_,lastScan_;bool host_,complete_,committed_;
    EnvironmentItemPacket control(const Key&,u8 operation,u32 epoch,u32 load,u32 target) const;
    bool captureHost(GameWorld*,const Key&,Region&);
    bool reconcile(GameWorld*,const Key&,Region&);
    bool send(NetLink&,const Key&,Region&,u32 peer);
    u32 peerLoads_[MAX_PLAYERS];
};
}
#endif
