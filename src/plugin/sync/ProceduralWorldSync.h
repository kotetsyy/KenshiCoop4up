#ifndef KENSHICOOP_PROCEDURAL_WORLD_SYNC_H
#define KENSHICOOP_PROCEDURAL_WORLD_SYNC_H

#include "../../netproto/Wire.h"
#include "../core/ProceduralWorldState.h"
#include "../core/Inbound.h"
#include "../game/EngineProcedural.h"
#include <map>
#include <vector>

class GameWorld;
namespace coop {
class NetLink;

struct ProceduralBedBinding {
    u64 world,id;
    u32 loadId,epoch;
    i32 regionX,regionZ;
    unsigned int hand[5];
    bool ready;
};
class Inbound;

// MAIN-thread coordinator. Native object pointers never cross the transport.
class ProceduralWorldSync {
public:
    ProceduralWorldSync();
    void reset();
    void update(GameWorld*, NetLink&, Inbound&, bool host, u32 loadId, bool worldLive);
    // Native loaded-set readiness is local. HOST certification additionally
    // requires the JOIN's ordered reliable scope commit after its ACKs/retirements.
    bool allLoadedRegionsReady() const;
    bool peerReady(u32 playerId) const;
    bool queryRegion(GameWorld*, i32 x, i32 z, ProceduralBedBinding*) const;
    // A null localHand selects the first reconciled bed for native harness use.
    bool queryBed(GameWorld*, const unsigned int localHand[5], ProceduralBedBinding*) const;
    // 1 paired; -1 managed category pending/ambiguous; 0 unmanaged fixture.
    int resolveFixture(const FixturePacket&, unsigned int localHand[5],u64* logicalId=0) const;
    bool localHand(u64 logicalId,unsigned int out[5]) const;
    bool regionReady(i32 x, i32 z) const;

private:
    struct RegionKey {
        i32 x,z;
        RegionKey(i32 a=0,i32 b=0):x(a),z(b) {}
        bool operator<(const RegionKey& b) const { return x<b.x || (x==b.x && z<b.z); }
    };
    struct LocalHand {
        unsigned int value[5];
        LocalHand() { memset(value,0,sizeof(value)); }
    };
    struct Region {
        bool loaded, wasLoaded, censusComplete, reconciled, requestQueued, retirePending;
        u32 epoch, lastRequest, lastSend;
        const char* pendingReason;
        std::vector<ProceduralObject> history, objects, observed;
        std::vector<engine::ProceduralBedRead> native;
        std::vector<u8> used;
        std::map<u64,LocalHand> bindings;
        procedural::RegionSnapshot snapshot;
        u32 requestedLoads[MAX_PLAYERS], acknowledgedEpochs[MAX_PLAYERS];
        Region();
    };
    typedef std::map<RegionKey,Region> Regions;
    Regions regions_;
    procedural::PeerScope peerScopes_[MAX_PLAYERS];
    procedural::NativeCensusFence nativeCensus_;
    engine::ProceduralBedRead scratch_[PROCEDURAL_OBJECTS_MAX];
    std::deque<InboundProcedural> incoming_;
    GameWorld* nativeWorld_;
    u64 world_, nextId_;
    u32 load_, lastScan_;
    bool host_, registryComplete_, active_, scopeCommitted_;
    int registryState_;

    bool capture(GameWorld*, const RegionKey&, std::vector<ProceduralObject>&,
                 std::vector<engine::ProceduralBedRead>* native,
                 const std::vector<ProceduralObject>* wanted=0);
    void updateHostCatalog(GameWorld*, const RegionKey&, Region&);
    void sendSnapshot(NetLink&, const RegionKey&, Region&, u32 requestedLoad);
    bool reconcile(GameWorld*, const RegionKey&, Region&);
    void logCatalog(const RegionKey&, const Region&, const std::vector<ProceduralObject>&) const;
};

} // namespace coop
#endif
