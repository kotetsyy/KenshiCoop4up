#include "../core/EnvironmentItemsState.h"
#include <cassert>
#include <cstring>
using namespace coop;
static EnvironmentItemObject row(u64 id,const char* sid,float x) {
    EnvironmentItemObject o;memset(&o,0,sizeof(o));o.id=id;
    strcpy(o.item.stringID,sid);o.item.itemType=4;o.item.quantity=1;o.item.quality=10000;o.item.level=255;
    o.x=x;o.rotationW=1;o.condition=100;o.flags=ENV_VISIBLE|ENV_FIXED;return o;
}
static EnvironmentItemPacket packet(u8 op,u32 epoch,const environment::Objects& rows) {
    EnvironmentItemPacket p;memset(&p,0,sizeof(p));p.operation=op;p.worldToken=42;p.loadId=7;p.scopeId=3;
    p.regionX=22;p.regionZ=32;p.epoch=epoch;p.count=(u32)rows.size();p.digest=environment::digest(rows);return p;
}
static void nativeCensusReadiness() {
    procedural::NativeCensusFence census;
    assert(!environment::reconciledCensusCurrent(true,true,census,10));
    census.begin(10); // capture has started, but has not proved a complete set
    assert(!environment::reconciledCensusCurrent(true,true,census,10));
    census.finish(true,10);
    assert(environment::reconciledCensusCurrent(true,true,census,10));
    assert(!environment::reconciledCensusCurrent(false,true,census,10));
    assert(!environment::reconciledCensusCurrent(true,false,census,10));

    // A birth/enqueue/exclusion/removal after capture cannot be blessed by
    // finishing reconciliation against the newer journal revision.
    census.begin(10);census.finish(true,10); // capture returned a complete certificate
    census.finish(true,11); // reconciliation cannot refresh that old certificate
    assert(!environment::reconciledCensusCurrent(true,true,census,11));
    census.begin(11);census.finish(true,11);
    assert(environment::reconciledCensusCurrent(true,true,census,11));
    const procedural::NativeCensusFence published=census; // native interaction keeps the same proof
    assert(published.current(11));
    // The cached ready bit and a previously complete census cannot outlive
    // another native mutation, even before the next coordinator update.
    assert(!environment::reconciledCensusCurrent(true,true,census,12));
    assert(!published.current(12)); // copying a certificate cannot turn it into a cached permission bit

    census.begin(12);census.finish(false,12); // partial/failed native census
    assert(!environment::reconciledCensusCurrent(true,true,census,12));
    census.begin(12);census.finish(true,12);
    assert(environment::reconciledCensusCurrent(true,true,census,12));
    census.begin(12); // a new failed capture must revoke the previous proof
    assert(!environment::reconciledCensusCurrent(true,true,census,12));
    census.reset();
    assert(!environment::reconciledCensusCurrent(true,true,census,12));
    assert(!environment::reconciledCensusCurrent(true,true,census,0));
    census.begin(13);census.finish(true,13);
    assert(environment::reconciledCensusCurrent(true,true,census,13));

    environment::Objects empty;
    environment::Snapshot transferred;transferred.reset(42,7,22,32,3);
    EnvironmentItemPacket p=packet(ENV_BEGIN,1,empty);assert(transferred.accept(p));
    p.operation=ENV_END;assert(transferred.accept(p));assert(transferred.complete());
    census.reset();
    assert(!environment::reconciledCensusCurrent(transferred.complete(),true,census,13));
}
int main() {
    environment::Objects objects;objects[1]=row(1,"native-food",10);objects[2]=row(2,"native-tool",20);
    environment::Snapshot snapshot;snapshot.reset(42,7,22,32,3);
    EnvironmentItemPacket p=packet(ENV_ROW,1,objects);p.object=objects[1];
    assert(!snapshot.accept(p));assert(!snapshot.complete()); // no BEGIN, no destructive permission
    p=packet(ENV_BEGIN,1,objects);assert(snapshot.accept(p));
    p.operation=ENV_END;assert(!snapshot.accept(p));assert(!snapshot.complete());
    p.operation=ENV_ROW;p.object=objects[2];assert(snapshot.accept(p));
    assert(snapshot.accept(p)); // exact duplicate delivery
    p.object=objects[1];assert(snapshot.accept(p)); // row arrival order is irrelevant
    p.operation=ENV_BEGIN;assert(snapshot.accept(p)); // BEGIN retry cannot erase rows
    p.operation=ENV_END;assert(snapshot.accept(p));assert(snapshot.complete());assert(snapshot.rows().size()==2);
    assert(snapshot.accept(p));assert(snapshot.complete()); // END retry permits ACK re-send
    p=packet(ENV_BEGIN,2,objects);EnvironmentItemPacket stale=p;stale.scopeId=2;
    assert(!snapshot.accept(stale));assert(snapshot.complete()); // stale zone load cannot revoke current assembly
    stale=p;stale.loadId=6;assert(!snapshot.accept(stale));assert(snapshot.complete());
    stale=p;stale.worldToken=41;assert(!snapshot.accept(stale));assert(snapshot.complete());
    stale=p;stale.regionX=20;assert(!snapshot.accept(stale));assert(snapshot.complete());
    assert(snapshot.accept(p));assert(!snapshot.complete());
    stale=packet(ENV_END,1,objects);assert(!snapshot.accept(stale));assert(!snapshot.complete());
    p.operation=ENV_ROW;p.object=objects[1];assert(snapshot.accept(p));
    p.object.item.quantity=2;assert(!snapshot.accept(p)); // conflicting duplicate poisons epoch
    p=packet(ENV_END,2,objects);assert(!snapshot.accept(p));assert(!snapshot.complete());
    // A later epoch recovers; incomplete data never authorizes an empty set.
    environment::Objects empty;p=packet(ENV_BEGIN,3,empty);assert(snapshot.accept(p));assert(!snapshot.complete());
    p.operation=ENV_END;assert(snapshot.accept(p));assert(snapshot.complete());assert(snapshot.rows().empty());
    p=packet(ENV_BEGIN,4,objects);assert(snapshot.accept(p));
    p.operation=ENV_ROW;p.object=objects[1];assert(snapshot.accept(p));p.object=objects[2];assert(snapshot.accept(p));
    p.operation=ENV_END;p.digest^=1;assert(!snapshot.accept(p));assert(!snapshot.complete());
    p=packet(ENV_BEGIN,5,objects);p.count=ENVIRONMENT_ITEMS_MAX+1;assert(!snapshot.accept(p));assert(!snapshot.complete());
    snapshot.reset(42,7,22,32,4);p=packet(ENV_BEGIN,6,objects);assert(!snapshot.accept(p)); // unload/reload request fence
    p.scopeId=4;assert(snapshot.accept(p));
    EnvironmentItemObject invalid=objects[1];invalid.item.quantity=0;assert(!environment::valid(invalid));
    invalid=objects[1];memset(invalid.item.manufacturer,'x',sizeof(invalid.item.manufacturer));assert(!environment::valid(invalid));
    invalid=objects[1];invalid.rotationW=0;assert(!environment::valid(invalid));
    invalid=objects[1];invalid.item.equipped=1;assert(!environment::valid(invalid));
    invalid=objects[1];invalid.item.parentIdx=1;assert(!environment::valid(invalid));
    invalid=objects[1];invalid.item.itemType=0;assert(!environment::valid(invalid)); // never fabricate buildings from an item packet
    invalid=objects[1];invalid.charges=-1;invalid.fullCharge=-1;assert(environment::valid(invalid)); // preserve finite native sentinels
    invalid=objects[1];invalid.parent.present=1;invalid.parent.type=0;strcpy(invalid.parent.sid,"native-building");
    assert(environment::valid(invalid)); // SDK BUILDING is zero, not an absence sentinel
    EnvironmentItemObject changed=invalid;changed.properOwner=invalid.parent;
    assert(!environment::sameNative(changed,invalid)); // ownership is part of full native state
    changed=invalid;changed.rotationX=0.1f;assert(!environment::sameNative(changed,invalid));
    changed=invalid;changed.fullCharge=2;assert(!environment::sameNative(changed,invalid));
    changed=invalid;strcpy(changed.item.material,"different-native-material");assert(!environment::sameNative(changed,invalid));
    changed=invalid;changed.id=999;
    assert(environment::sameNative(changed,invalid)); // native identity != network id
    assert(environment::nativeFingerprint(changed)==environment::nativeFingerprint(invalid));
    changed=invalid;changed.properOwner.present=1;changed.properOwner.type=1;strcpy(changed.properOwner.sid,"native-actor");
    changed.properOwner.actorOwner=1;changed.properOwner.actorKey[0]=1;changed.properOwner.actorKey[3]=10;changed.properOwner.actorKey[4]=20;
    assert(environment::valid(changed));assert(!environment::sameNative(changed,invalid));
    invalid=changed;changed.properOwner.actorOwner=2;assert(!environment::sameNative(changed,invalid)); // authenticated source is part of actor identity
    changed=invalid;changed.properOwner.actorKey[4]=21;assert(!environment::sameNative(changed,invalid)); // incarnation fence
    changed=invalid;changed.properOwner.actorKey[3]=0;assert(environment::valid(changed)); // native slot zero with a live serial is valid
    changed.properOwner.actorKey[4]=0;assert(!environment::valid(changed)); // empty actor identity is not an owner
    assert(environment::digest(objects)!=environment::digest(empty));
    nativeCensusReadiness();
    return 0;
}
